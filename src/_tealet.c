
#include <stddef.h>

#include <Python.h>
#include <structmember.h>
#include <frameobject.h>
#include <pythread.h>

#include "tealet.h"
#include "tools.h"


#define STATE_NEW 0
#define STATE_STUB 1
#define STATE_RUN 2
#define STATE_EXIT 3

static PyTypeObject PyTealetType;
#define PyTealet_Check(op) PyObject_TypeCheck(op, &PyTealetType)
#define PyTealet_CheckExact(op) (Py_TYPE(op) == &PyTealetType)

static int tls_key;

/* convenience macros to access the "extra" argument as a pointer to PyTealetObject */
#define GET_TEALET_PY(t) ((struct PyTealetObject*) ((t)->extra))
#define SET_TEALET_PY(t, v) do { \
		(t)->extra = (void*) (v); \
	} while (0)

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
static PyTealetObject *GetMain(void);
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
			SET_TEALET_PY(result->tealet, result);
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
	PyTealetObject *tmain, *pytealet = (PyTealetObject*)self;
	tealet_t *tresult;
	if (pytealet->state != STATE_NEW) {
		PyErr_SetString(StateError, "must be new");
		return NULL;
	}
	assert(pytealet->tealet == NULL);
	tmain = GetMain();
	if (!tmain)
		return NULL;
	tresult = tealet_stub_new(tmain->tealet);
	if (!tresult)
		return PyErr_NoMemory();
	pytealet->tealet = tresult;
	pytealet->state = STATE_STUB;
	SET_TEALET_PY(tresult, pytealet);
	Py_INCREF(self);
	return self;
}
PyDoc_STRVAR(pytealet_stub_doc,
"stub() -> None\n\n"
"turn this tealet into a stub that can be duplicated by passing it\n"
"to the Tealet constructor.  This captures the current stack position\n"
"for re-use in other tealets.\n"
"Can only be called on a new Tealet object.");

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
		fail = tealet_stub_run(target->tealet, pytealet_main, &switch_arg);
		if (fail) {
			PyObject_Free(ptarg);
			PyErr_NoMemory();
			goto err;
		}
	} else {
		PyTealetObject *tmain = GetMain();
		if (!tmain)
			goto err;
		tealet = tealet_new(tmain->tealet, pytealet_main, &switch_arg);
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
PyDoc_STRVAR(pytealet_run_doc,
"run(function, arg=None) -> arg\n\n\
Start a tealet running in function, passing a single optional arg.\n\
Returns the switch argument used when switching back to the original tealet.");

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
PyDoc_STRVAR(pytealet_switch_doc,
"switch(arg=None) -> arg\n\n\
Switch to this tealet.  Returns the arg used when switching back.");
	
static struct PyMethodDef pytealet_methods[] = {
	{"stub", (PyCFunction) pytealet_stub, METH_NOARGS, pytealet_stub_doc},
	{"run", (PyCFunction) pytealet_run, METH_VARARGS|METH_KEYWORDS, pytealet_run_doc},
	{"switch", (PyCFunction) pytealet_switch, METH_VARARGS, pytealet_switch_doc},
	{NULL,       NULL}          /* sentinel */
};

/************
 * Properties
 */
static PyObject *
pytealet_get_main(PyObject *_self, void *_closure)
{
	PyTealetObject *self = (PyTealetObject *)_self;
	PyTealetObject *tmain = GET_TEALET_PY(self->tealet->main);
	Py_INCREF(tmain);
	return (PyObject*)tmain;
}
PyDoc_STRVAR(pytealet_get_main_doc, 
"The main tealet associated with this tealet.");

static PyObject *
pytealet_get_state(PyObject *_self, void *_closure)
{
	PyTealetObject *self = (PyTealetObject *)_self;
#if PY_MAJOR_VERSION >= 3
	return PyLong_FromLong(self->state);
#else
	return PyInt_FromLong(self->state);
#endif
}
PyDoc_STRVAR(pytealet_get_state_doc,
"The current state of the objects, one of:\n\
STATE_NEW, STATE_STUB, STATE_RUN, STATE_EXIT.");

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
PyDoc_STRVAR(pytealet_get_frame_doc,
"The frame of the tealet if it is in the STATE_RUN state.");

static PyObject *
pytealet_get_tid(PyObject *_self, void *_closure)
{
	PyTealetObject *self = (PyTealetObject *)_self;
	long tid = 0;
	if (self->tealet) {
		main_data *mdata = (main_data*)*tealet_main_userpointer(self->tealet);
		tid = mdata->tid;
	}
#if PY_MAJOR_VERSION >= 3
	return PyLong_FromLong(tid);
#else
	return PyInt_FromLong(tid);
#endif
}
PyDoc_STRVAR(pytealet_get_tid_doc,
"The thread id of the thread this tealet belongs to.");

static struct PyGetSetDef pytealet_getset[] = {
	{"main", pytealet_get_main, NULL, pytealet_get_main_doc, NULL},
	{"state", pytealet_get_state, NULL, pytealet_get_state_doc, NULL},
	{"frame", pytealet_get_frame, NULL, pytealet_get_frame_doc, NULL},
	{"thread_id", pytealet_get_tid, NULL, pytealet_get_tid_doc, NULL},
	{0}
};

PyDoc_STRVAR(pytealet_type_doc,
"tealet(t=None) -> new tealet object\n\n\
Creates a new tealet object, ready to be run.  If passed a stub tealet,\n\
the new one is also a stub, a copy of the original.  This can be useful\n\
to make new tealets start at a fixed position on the stack.");

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
	pytealet_type_doc,                          /* tp_doc */
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
		assert(GET_TEALET_PY(t_current) == tealet);
		PyObject_Free(arg); /* heap allocated */
	} else {
		/* set up the pointer in the tealet */
		tealet->tealet = t_current;
		SET_TEALET_PY(t_current, tealet);
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
	SET_TEALET_PY(t_current, NULL);
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
static PyTealetObject *GetMain(void)
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
		tmain = tealet_initialize(&talloc, 0);
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
		SET_TEALET_PY(tmain, t_main); /* back link */
		PyThread_set_key_value(tls_key, (void*)t_main);
	}
	assert(t_main->tealet);
	assert(TEALET_IS_MAIN(t_main->tealet));
	assert(t_main->state == STATE_RUN);		
	return t_main;
}

/* return a borrowed ref to this threads current tealet */
static PyTealetObject *
GetCurrent(PyTealetObject *tmain)
{
	if (!tmain)
		tmain = GetMain();
	if (!tmain)
		return NULL;
	return GET_TEALET_PY(tealet_current(tmain->tealet));
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
module_current(void)
{
	PyTealetObject* current = GetCurrent(NULL);
	Py_XINCREF(current);
	return (PyObject*)current;
}
PyDoc_STRVAR(module_current_doc,
"current() -> t\n\n\
Get the currently executing tealet object.");

static PyObject *
module_main(void)
{
	PyTealetObject* tmain = GetMain();
	Py_XINCREF(tmain);
	return (PyObject*)tmain;
}
PyDoc_STRVAR(module_main_doc,
"main() -> t\n\n\
Get the main tealet of the currently executing tealet object.\n\
Equivalent to current().main.");

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
PyDoc_STRVAR(hide_frame_doc,
"hide_frame(func, args=(), kwds={}) -> result\n\n\
Call 'func(*args, **kwds)' and return the result.\n\
Cuts the frame chain so that a traceback will not show the calling\n\
stack.  This can be useful to hide trampoline functions and so on\n\
to make sure unittests pass.");

static PyMethodDef module_methods[] = {
	{"current", (PyCFunction)module_current, METH_NOARGS, module_current_doc},
	{"main", (PyCFunction)module_main, METH_NOARGS, module_main_doc},
	{"hide_frame", (PyCFunction)hide_frame, METH_VARARGS, hide_frame_doc},
	{NULL,                      NULL}            /* Sentinel */
};


PyDoc_STRVAR(module_doc,
"This module provides a simple interface to the Tealet stack slicing library.\n"
"It allows the creation of execution contexts and explicit switching between\n"
"them.");

PyDoc_STRVAR(tealet_error_doc,"Base class for tealet errors");
PyDoc_STRVAR(tealet_defuncterror_doc,"The tealet is corrupt, its state could not be saved.");
PyDoc_STRVAR(tealet_invaliderror_doc,"The tealet is not part of the current group.");
PyDoc_STRVAR(tealet_stateerror_doc,"The tealet is in an invalid state");

#if PY_MAJOR_VERSION >= 3
  static struct PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT,
    "_tealet", /* m_name */
    module_doc,          /* m_doc */
    -1,                  /* m_size */
    module_methods,      /* m_methods */
    NULL,                /* m_reload */
    NULL,                /* m_traverse */
    NULL,                /* m_clear */
    NULL,                /* m_free */
  };
#endif

static PyObject *
moduleinit(void)
{
	PyObject *m;
	PyTealetObject *tmain;

	tls_key = PyThread_create_key();

	/* init the type */
	if (PyType_Ready(&PyTealetType))
		return NULL;

	tmain = GetMain();
	if (!tmain)
		return NULL;
#if PY_MAJOR_VERSION >= 3
    m = PyModule_Create(&moduledef);
#else
	m = Py_InitModule3("_tealet", module_methods, module_doc);
#endif
	if (m == NULL)
		return m;

	/* Todo: Improve error handling */
	Py_INCREF(&PyTealetType);
	PyModule_AddObject(m, "tealet", (PyObject*)&PyTealetType);
	TealetError = PyErr_NewExceptionWithDoc(
		"_tealet.TealetError", tealet_error_doc, NULL, NULL);
	PyModule_AddObject(m, "TealetError", TealetError);
	DefunctError = PyErr_NewExceptionWithDoc(
		"_tealet.DefunctError", tealet_defuncterror_doc, TealetError, NULL);
	PyModule_AddObject(m, "DefunctError", DefunctError);
	InvalidError = PyErr_NewExceptionWithDoc(
		"_tealet.InvalidError", tealet_invaliderror_doc, TealetError, NULL);
	PyModule_AddObject(m, "InvalidError", InvalidError);
	StateError = PyErr_NewExceptionWithDoc(
		"_tealet.StateError", tealet_stateerror_doc, TealetError, NULL);
	PyModule_AddObject(m, "StateError", StateError);

	PyModule_AddIntMacro(m, STATE_NEW);
	PyModule_AddIntMacro(m, STATE_STUB);
	PyModule_AddIntMacro(m, STATE_RUN);
	PyModule_AddIntMacro(m, STATE_EXIT);
	return m;
}

#if PY_MAJOR_VERSION < 3
    PyMODINIT_FUNC
    init_tealet(void)
    {
        moduleinit();
    }
#else
    PyMODINIT_FUNC
    PyInit__tealet(void)
    {
        return moduleinit();
    }
#endif