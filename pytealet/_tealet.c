
#include <stddef.h>
#include "Python.h"
#include "structmember.h"
#include "frameobject.h"
#include "pythread.h"

#include "tealet.h"


/****************************************************************
 *Implement copyable stubs by using a trampoline
 */
struct stub_arg
{
    tealet_t *current;
    tealet_run_t run;
    void *runarg;
};
static tealet_t *
stub_main(tealet_t *current, void *arg)
{
    void *myarg = 0;
    /* the caller is in arg, return right back to him */
    tealet_switch((tealet_t*)arg, &myarg);
    /* now we are back, myarg should contain the arg to the run function.
     * We were possibly duplicated, so can't trust the original function args.
     */
    {
        struct stub_arg sarg = *(struct stub_arg*)myarg;
        tealet_free(sarg.current, myarg);
        return (sarg.run)(sarg.current, sarg.runarg);
    }
}

/* create a stub and return it */
static tealet_t *
stub_new(tealet_t *t) {
    void *arg = (void*)tealet_current(t);
    return tealet_new(t, stub_main, &arg);
}

/* run a stub */
static int
stub_run(tealet_t *stub, tealet_run_t run, void **parg)
{
    int result;
    void *myarg;
    /* we cannot pass arguments to a different tealet on the stack */
    struct stub_arg *psarg = (struct stub_arg*)tealet_malloc(stub, sizeof(struct stub_arg));
    if (!psarg)
        return TEALET_ERR_MEM;
    psarg->current = stub;
    psarg->run = run;
    psarg->runarg = parg ? *parg : NULL;
    myarg = (void*)psarg;
    result = tealet_switch(stub, &myarg);
    if (result) {
        /* failure */
        tealet_free(stub, psarg);
        return result;
    }
    /* pass back the arg value from the switch */
    if (parg)
        *parg = myarg;
    return 0;
}
/***************************************************************/


#define STATE_NEW 0
#define STATE_STUB 1
#define STATE_RUN 2
#define STATE_EXIT 3

static PyTypeObject PyTealetType;
#define PyTealet_Check(op) PyObject_TypeCheck(op, &PyTealetType)
#define PyTealet_CheckExact(op) (Py_TYPE(op) == &PyTealetType)

static int tls_key;

/* the structure we associate with the main tealet */
typedef struct main_data
{
	long tid;
	PyObject *dustbin[3];
} main_data;

/* The python tealet object */
typedef struct PyTealetObject {
    PyObject_HEAD
    int state;
	tealet_t *tealet;
	PyObject *weakreflist; /* List of weak references */
	/* call stack related information from the thread state */
	PyFrameObject *frame;
	PyObject *exc_type;
	PyObject *exc_val;
	PyObject *exc_tb;
	int recursion_depth;
	
} PyTealetObject;

typedef struct pytealet_main_arg {
	int stub;
	PyTealetObject *dest;
	PyObject *func;
	PyObject *arg;
} pytealet_main_arg;

/* helpers for getting main and current and checking relationship */
static PyTealetObject *GetMain();
static PyTealetObject *GetCurrent(PyTealetObject *main);
static int CheckTarget(PyTealetObject *target, PyTealetObject *main);

static tealet_t * pytealet_main(tealet_t *t_current, void *arg);

static PyObject *TealetError;
static PyObject *InvalidError;
static PyObject *StateError;
static PyObject *DefunctError;

/* helper functions to save and restore callstack related data from the python threadstate
 * into the tealet object
 */
static void
save_tstate(PyTealetObject *current, PyThreadState *tstate)
{
	if (!tstate)
		tstate = PyThreadState_GET();
	assert(current->frame == NULL);
	current->frame = tstate->frame;
	current->recursion_depth = tstate->recursion_depth;
	tstate->frame = NULL;
	tstate->recursion_depth = 0;
	assert(current->exc_val == NULL && current->exc_type == NULL && current->exc_tb == NULL);
	current->exc_type = tstate->exc_type;
	current->exc_val = tstate->exc_value;
	current->exc_tb = tstate->exc_traceback;
	tstate->exc_type = tstate->exc_value = tstate->exc_traceback = NULL;
}

/* helper functions to save and restore callstack related data from the python threadstate
 * into the tealet object
 */
static void
restore_tstate(PyTealetObject *current, PyThreadState *tstate)
{
	if (!tstate)
		tstate = PyThreadState_GET();
	assert(tstate->frame == NULL);
	
	tstate->frame = current->frame;
	tstate->recursion_depth = current->recursion_depth;
	current->frame = NULL;
	current->recursion_depth = 0;
	assert(!PyErr_Occurred());
	Py_CLEAR(tstate->exc_type); /* there can be cruft here from the last tealet's exit */
	Py_CLEAR(tstate->exc_value);
	Py_CLEAR(tstate->exc_traceback);
	tstate->exc_type = current->exc_type;
	tstate->exc_value = current->exc_val;
	tstate->exc_traceback = current->exc_tb;
	current->exc_type = current->exc_val = current->exc_tb = NULL;
}

/* Helper functions to fill/empty the dustbin.  We must be careful not to
 * clear references at a delicate moment before switching, rather
 * references must be cleared after, so that any side-effects of
 * clearing references won't affect the state of the program.
 */
static void
dustbin_fill(tealet_t *tealet, PyObject *a, PyObject *b, PyObject *c)
{
	main_data *mdata = (main_data*)*tealet_main_userpointer(tealet);
	assert(!mdata->dustbin[0]);
	assert(!mdata->dustbin[1]);
	assert(!mdata->dustbin[2]);
	mdata->dustbin[0] = a;
	mdata->dustbin[1] = b;
	mdata->dustbin[2] = c;
}

static void
dustbin_clear(tealet_t *tealet)
{
	main_data *mdata = (main_data*)*tealet_main_userpointer(tealet);
	PyObject *a, *b, *c;
	a = mdata->dustbin[0];
	b = mdata->dustbin[1];
	c = mdata->dustbin[2];
	mdata->dustbin[0] = mdata->dustbin[1] = mdata->dustbin[2] = NULL;
	Py_XDECREF(a);
	Py_XDECREF(b);
	Py_XDECREF(c);
}

static PyObject *
pytealet_new(PyTypeObject *subtype, PyObject *args, PyObject *kwds)
{
	PyTealetObject *src = NULL;
	PyTealetObject *result;
	if (args && PyTuple_GET_SIZE(args)>0) {
		src = (PyTealetObject*)PyTuple_GET_ITEM(args, 0);
		if (!PyTealet_Check(src)) {
			PyErr_SetNone(PyExc_TypeError);
			return NULL;
		}
		if (src->state != STATE_NEW && src->state != STATE_STUB) {
			PyErr_SetString(StateError, "state must be new or stub");
			return NULL;
		}
	}
	result = (PyTealetObject*)subtype->tp_alloc(subtype, 0);
	if (!result)
		return NULL;
	result->state = STATE_NEW;
	result->tealet = NULL;
	result->frame = NULL;
	result->exc_type = result->exc_val = result->exc_tb = NULL;
	result->recursion_depth = 0;
	result->weakreflist = NULL;
	if (src) {
		if (src->state == STATE_STUB) {
			result->tealet = tealet_duplicate(src->tealet);
			if (!result->tealet) {
				Py_DECREF(result);
				return PyErr_NoMemory();
			}
			result->tealet->data = (void*)result;
		}
		result->state = src->state;
	}
	return (PyObject*) result;
}

static void
pytealet_dealloc(PyObject *obj)
{
	PyTealetObject *tealet = (PyTealetObject *)obj;
	if (tealet->state == STATE_RUN) {
		int err = PyErr_WarnEx(PyExc_RuntimeWarning, "freeing an active tealet leaks memory", 1);
		if (err) {
			PyErr_WriteUnraisable(Py_None);
		}
	}
	/* leave tealet->frame alone, it's a weakref */
	Py_XDECREF(tealet->exc_type);
	Py_XDECREF(tealet->exc_val);
	Py_XDECREF(tealet->exc_tb);
	if (tealet->weakreflist != NULL)
        PyObject_ClearWeakRefs(obj);
	if (tealet->tealet)
		tealet_delete(tealet->tealet);
	Py_TYPE(obj)->tp_free(obj);
}

/* make stub here */
static PyObject *
pytealet_stub(PyObject *self)
{
	PyTealetObject *main, *pytealet = (PyTealetObject*)self;
	tealet_t *tresult;
	if (pytealet->state != STATE_NEW) {
		PyErr_SetString(StateError, "must be new");
		return NULL;
	}
	assert(pytealet->tealet == NULL);
	main = GetMain();
	if (!main)
		return NULL;
	tresult = stub_new(main->tealet);
	if (!tresult)
		return PyErr_NoMemory();
	pytealet->tealet = tresult;
	pytealet->state = STATE_STUB;
	tresult->data = (void*)pytealet;
	Py_INCREF(self);
	return self;
}

/* run a tealet and optinonally run */
static PyObject *
pytealet_run(PyObject *self, PyObject *args, PyObject *kwds)
{
	PyTealetObject *target = (PyTealetObject *)self;
	PyTealetObject *current;
	PyObject *func; 
	PyObject *farg = Py_None;
	int fail;
	tealet_t *tealet;
	char *keywords[] = {"function", "arg", NULL};
	pytealet_main_arg targ, *ptarg;
	PyThreadState *tstate = PyThreadState_GET();
	PyObject *result = NULL;
	void *switch_arg;

	current = GetCurrent(NULL);
	if (!current)
		return NULL;
	if (CheckTarget(target, current))
		return NULL;

	if (target->state != STATE_NEW && target->state != STATE_STUB) {
		PyErr_SetString(StateError, "must be new or stub");
		return NULL;
	}
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O:run", keywords,
		&func, &farg))
		return NULL;
	
	if (target->state == STATE_NEW) {
		ptarg = &targ; /* can use the stack because of the way tealet_new works */
		ptarg->stub = 0;
	} else {
		/* must allocate the argument on the heap because we will switch here */
		ptarg = (pytealet_main_arg*)PyObject_Malloc(sizeof(*ptarg));
		if (!ptarg)
			return PyErr_NoMemory();
		ptarg->stub = 1;
	}

	ptarg->dest = target;
	ptarg->func = func;
	ptarg->arg = farg;
	
	/* pass the argument to the main function */
	switch_arg = (void*)ptarg;

	save_tstate(current, tstate);
	if (ptarg->stub) {
		fail = stub_run(target->tealet, pytealet_main, &switch_arg);
		if (fail) {
			PyObject_Free(ptarg);
			PyErr_NoMemory();
			goto err;
		}
	} else {
		PyTealetObject *main = GetMain();
		if (!main)
			goto err;
		tealet = tealet_new(main->tealet, pytealet_main, &switch_arg);
		if (!tealet) {
			PyErr_NoMemory();
			goto err;
		}
	}
	/* success */
	result = (PyObject *)switch_arg;
err:
	/* restore frame */
	restore_tstate(current, tstate);
	/* clear garbage */
	dustbin_clear(current->tealet);
	return result;
}

/* switch to a different tealet */
static PyObject *
pytealet_switch(PyObject *_self, PyObject *args)
{
	PyTealetObject *self = (PyTealetObject *)_self;
	PyTealetObject *current;
	int fail;
	PyThreadState *tstate = PyThreadState_GET();
	PyObject *pyarg = Py_None;
	void *switch_arg;
	
	if (!PyArg_ParseTuple(args, "|O:switch", &pyarg))
		return NULL;

	if (self->state != STATE_RUN) {
		PyErr_SetString(StateError, "must be active");
		return NULL;
	}
	assert(self->tealet);
	current = GetCurrent(NULL);
	if (!current)
		return NULL;
	if (CheckTarget(self, current))
		return NULL;
	
	Py_INCREF(pyarg);
	switch_arg = (void*)pyarg;
	/* switch */
	save_tstate(current, tstate);
	fail = tealet_switch(self->tealet, &switch_arg);
	restore_tstate(current, tstate);

	/* clear out garbage */
	dustbin_clear(current->tealet);
	
	if (fail == TEALET_ERR_DEFUNCT) {
		Py_DECREF(pyarg);
		PyErr_SetString(DefunctError, "target is defunct");
		return NULL;
	} else if (fail == TEALET_ERR_MEM) {
		Py_DECREF(pyarg);
		return PyErr_NoMemory();
	}
	/* return the arg passed to us */
	pyarg = (PyObject *)switch_arg;
	return pyarg;
}
	
static struct PyMethodDef pytealet_methods[] = {
	{"stub", (PyCFunction) pytealet_stub, METH_NOARGS, ""},
	{"run", (PyCFunction) pytealet_run, METH_VARARGS|METH_KEYWORDS, ""},
    {"switch", (PyCFunction) pytealet_switch, METH_VARARGS, ""},
	{NULL,       NULL}          /* sentinel */
};

/************
 * Properties
 */
static PyObject *
pytealet_get_main(PyObject *_self, void *_closure)
{
	PyTealetObject *self = (PyTealetObject *)_self;
	PyTealetObject *main = (PyTealetObject *)(self->tealet->main->data);
	Py_INCREF(main);
	return (PyObject*)main;
}

static PyObject *
pytealet_get_state(PyObject *_self, void *_closure)
{
	PyTealetObject *self = (PyTealetObject *)_self;
	return PyInt_FromLong(self->state);
}

static PyObject *
pytealet_get_frame(PyObject *_self, void *_closure)
{
	PyTealetObject *self = (PyTealetObject *)_self;
	PyObject *frame = (PyObject*)self->frame;
	if (!frame) {
		/* is it the current tealet of the current thread? */
		if (self == GetCurrent(NULL)) {
			PyThreadState *tstate = PyThreadState_GET();
			frame = (PyObject*)tstate->frame;
		}
	}
	if (!frame)
		frame = Py_None;
	Py_INCREF(frame);
	return frame;
}

static PyObject *
pytealet_get_tid(PyObject *_self, void *_closure)
{
	PyTealetObject *self = (PyTealetObject *)_self;
	long tid = 0;
	if (self->tealet) {
		main_data *mdata = (main_data*)*tealet_main_userpointer(self->tealet);
		tid = mdata->tid;
	}
	return PyInt_FromLong(tid);
}


static struct PyGetSetDef pytealet_getset[] = {
	{"main", pytealet_get_main, NULL, "", NULL},
	{"state", pytealet_get_state, NULL, "", NULL},
	{"frame", pytealet_get_frame, NULL, "", NULL},
	{"thread_id", pytealet_get_tid, NULL, "", NULL},
	{0}
};


static PyTypeObject PyTealetType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_tealet.tealet",                           /* tp_name */
    sizeof(PyTealetObject),                     /* tp_basicsize */
    0,                                          /* tp_itemsize */
    (destructor)pytealet_dealloc,                 /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_compare */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    0,                                          /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,   /* tp_flags */
    "",                                         /* tp_doc */
    0,                                          /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    offsetof(PyTealetObject,weakreflist),       /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    pytealet_methods,                           /* tp_methods */
    0,                                          /* tp_members */
    pytealet_getset,                            /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,                                          /* tp_init */
    0,                                          /* tp_alloc */
    pytealet_new,                               /* tp_new */
    0,                                          /* tp_free */
};


/* The main function.  Invoked either from tealet.new or tealet.run */
static tealet_t *
pytealet_main(tealet_t *t_current, void *arg)
{
	pytealet_main_arg *targ = (pytealet_main_arg*)arg;
	PyTealetObject *tealet = targ->dest;
	PyObject *func = targ->func;
	PyObject *farg = targ->arg;
	PyObject *result, *return_arg;
	PyTealetObject *return_to;
	tealet_t *t_return;
	
	if (targ->stub) {
		assert(tealet->state == STATE_STUB);
		assert(t_current == tealet->tealet);
		assert(t_current->data == (void*)tealet);
		PyObject_Free(arg); /* heap allocated */
	} else {
		/* set up the pointer in the tealet */
		tealet->tealet = t_current;
		t_current->data = (void*)tealet;
	}

	/* We only have borrowed references from the calling tealet.
	 * the argument to the function will get their own reference, but
	 * anything we need after the function we keep oru own references
	 * for, because when the function returns, the calling tealet
	 * may have exited and dropped the references we borrowed.
	 */
	Py_INCREF(func);
	Py_INCREF(tealet);
	
	/* clear frame and run the tealet function */
	tealet->state = STATE_RUN;
	result = PyObject_CallFunctionObjArgs(func, tealet, farg, NULL);
	
	/* return_to can be a tuple of tealet, arg */
	return_to = NULL;
	return_arg = NULL;
	if (result && PyTuple_Check(result)) {
		/* arg and return_to are borrowed refs */
		if (PyTuple_GET_SIZE(result)>0)
			return_to = (PyTealetObject*)PyTuple_GET_ITEM(result, 0);
		if (PyTuple_GET_SIZE(result)>1)
			return_arg = PyTuple_GET_ITEM(result, 1);
	} else
		return_to = (PyTealetObject*)result;
		
	/* perform sanity checks on the result */
	if (return_to) {
		/* it is ok to rock the GC boat here, because we will switch to
		 * main in case of error, and main is always around
		 */
		if (!PyTealet_Check(return_to)) {
			return_to = NULL;
			PyErr_SetString(PyExc_TypeError, "tealet object expected");
		} else if (!return_to->state == STATE_RUN) {
			return_to = NULL;
			PyErr_SetString(StateError, "must be 'run'");
		} else if (CheckTarget(return_to, tealet))
			return_to = NULL;
	}
	if (!return_to) {
		Py_CLEAR(result);
		return_arg = NULL;
	}
	if (!return_arg)
		return_arg = Py_None;
	
	/* handle errors */
	if (!return_to) {
		PyErr_WriteUnraisable(func);
		/* must switch to main */
		return_to = GetMain();
		assert(return_to);
		result = (PyObject*)return_to;
		Py_INCREF(result);
	}
	/* now, the reference to return_to and return_arg are borrowed, kept alive
	 * by 'result', which may be the same as return_to.
	 */
	
	/* clear the old tealet */
	tealet->state = STATE_EXIT;
	tealet->tealet = NULL; /* will be auto-deleted on return */
	t_current->data = NULL;
	t_return = return_to->tealet;
	
	/* decref the objects after the switch */
	dustbin_fill(t_return, func, (PyObject*)tealet, result);
	
	Py_INCREF(return_arg);
	if (tealet_exit(t_return, (void*)return_arg, TEALET_EXIT_DEFAULT))
		tealet_exit(t_return->main, (void *)return_arg, TEALET_EXIT_DEFAULT);
	/* never reach here */
	return 0;
}


/* return a borrowed reference to this thread's main tealet */
static PyTealetObject *GetMain()
{
	/* Get the thread's main tealet */
	PyTealetObject *t_main = (PyTealetObject*)PyThread_get_key_value(tls_key);
	if (!t_main) {
		tealet_alloc_t talloc;
		tealet_t *tmain;
		main_data *mdata;
		talloc.malloc_p = (tealet_malloc_t)&PyMem_Malloc;
		talloc.free_p = (tealet_free_t)&PyMem_Free;
		talloc.context = NULL;
		tmain = tealet_initialize(&talloc);
		if (!tmain) {
			PyErr_NoMemory();
			return NULL;
		}
		mdata = (main_data*)PyMem_Malloc(sizeof(*mdata));
		if (!mdata) {
			tealet_finalize(tmain);
			PyErr_NoMemory();
			return NULL;
		}
		memset(mdata, 0, sizeof(*mdata));
		mdata->tid = PyThread_get_thread_ident();
		*tealet_main_userpointer(tmain) = (void*)mdata;
	
		/* create the main tealet */
		t_main = (PyTealetObject*)pytealet_new(&PyTealetType, NULL, NULL);
		if (!t_main) {
			tealet_finalize(tmain);
			PyMem_Free(mdata);
			return NULL;
		}
		t_main->tealet = tmain;
		t_main->state = STATE_RUN;
		tmain->data = (void*)t_main; /* back link */
		PyThread_set_key_value(tls_key, (void*)t_main);
	}
	assert(t_main->tealet);
	assert(TEALET_IS_MAIN(t_main->tealet));
	assert(t_main->state == STATE_RUN);		
	return t_main;
}

/* return a borrowed ref to this threads current tealet */
static PyTealetObject *
GetCurrent(PyTealetObject *main)
{
	if (!main)
		main = GetMain();
	if (!main)
		return NULL;
	return (PyTealetObject*) (tealet_current(main->tealet)->data);
}

/* check if a target tealet is valid */
static int
CheckTarget(PyTealetObject *target, PyTealetObject *ref)
{
	if (!ref)
		ref = GetMain();
	if (!ref)
		return -1;
	if (!target->tealet)
		return 0; /* no tealet yet */
	if (ref->tealet->main != target->tealet->main) {
		PyErr_SetString(InvalidError, "foreign tealet");
		return -1;
	}
	return 0;
}

/******************************************
 * Module methods
 */


static PyObject *
module_current()
{
	PyTealetObject* current = GetCurrent(NULL);
	Py_XINCREF(current);
	return (PyObject*)current;
}

static PyObject *
module_main()
{
	PyTealetObject* main = GetMain();
	Py_XINCREF(main);
	return (PyObject*)main;
}

static PyObject *
hide_frame(PyObject *self, PyObject *_args)
{
	/* this function calls a method, clearing the frame.  This hides
	 * higher frames in the callstack
	 */
	PyObject *func, *args=NULL, *kwds=NULL;
	PyThreadState *tstate = PyThreadState_GET();
	PyFrameObject *f = tstate->frame;
	PyObject *result;
	if (!PyArg_ParseTuple(_args, "O|OO:hide_frame", &func, &args, &kwds))
		return NULL;
	if (!args) {
		PyObject *empty = PyTuple_New(0);
		if (!empty)
			return NULL;
		tstate->frame = NULL;
		result = PyObject_Call(func, empty, kwds);
		Py_DECREF(empty);
	} else {
		tstate->frame = NULL;
		result = PyObject_Call(func, args, kwds);
	}
	tstate->frame = f;
	return result;
}

static PyMethodDef module_methods[] = {
	{"current", (PyCFunction)module_current, METH_NOARGS, ""},
	{"main", (PyCFunction)module_main, METH_NOARGS, ""},
	{"hide_frame", (PyCFunction)hide_frame, METH_VARARGS, ""},
 {NULL,                      NULL}            /* Sentinel */
};



PyMODINIT_FUNC
init_tealet(void)
{
	PyObject *m;
	PyTealetObject *main;

	tls_key = PyThread_create_key();

	/* init the type */
	if (PyType_Ready(&PyTealetType))
		return;

	main = GetMain();
	if (!main)
		return;
	
	m = Py_InitModule3("_tealet", module_methods, "");
    if (m == NULL)
        return;

	/* Todo: Improve error handling */
	PyModule_AddObject(m, "tealet", (PyObject*)&PyTealetType);
	TealetError = PyErr_NewException("_tealet.TealetError", NULL, NULL);
	PyModule_AddObject(m, "TealetError", TealetError);
	DefunctError = PyErr_NewException("_tealet.DefunctError", TealetError, NULL);
	PyModule_AddObject(m, "DefunctError", DefunctError);
	InvalidError = PyErr_NewException("_tealet.InvalidError", TealetError, NULL);
	PyModule_AddObject(m, "InvalidError", InvalidError);
	StateError = PyErr_NewException("_tealet.StateError", TealetError, NULL);
	PyModule_AddObject(m, "StateError", StateError);

	PyModule_AddIntMacro(m, STATE_NEW);
	PyModule_AddIntMacro(m, STATE_STUB);
	PyModule_AddIntMacro(m, STATE_RUN);
	PyModule_AddIntMacro(m, STATE_EXIT);
	return;
}