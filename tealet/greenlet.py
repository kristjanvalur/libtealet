# A greenlet emulation module using tealets
import weakref
import sys
import six

import _tealet

# reimplement six.reraise to avoid reference cycles
if not six.PY2:
    def _reraise(tp, value, tb):
        # two changes to six.reraise for py3:
        # 1 clear locals to avoid traceback cycles
        # 2 allow tp to be an instance, and val to be 0
        try:
            if six.PY2:
                six.reraise(tp, value, tb)  # use this to avoid invalid py2 raise syntax in py3 code
            if value is None:
                if issubclass(tp, BaseException):
                    value = tp()
                else:
                    value = tp
            if value.__traceback__ is not tb:
                raise value.with_traceback(tb)
            raise value
        finally:
            # avoid traceback refrence cycle
            tp = value = tb = None
else:
    six.exec_("""def _reraise(tp, value, tb):
        try:
            raise tp, value, tb;
        finally:
            del tb
        """, globals(), locals())

class error(Exception):
    pass
class GreenletExit(BaseException):
    pass

class ErrorWrapper(object):
    def __enter__(self):
        pass
    def __exit__(self, tp, val, tb):
        try:
            if isinstance(val, _tealet.TealetError):
                # want to create a new exception with an existing traceback.
                # six doesn't quite support that, so we help it along.
                if not six.PY2:
                    # don't use illegal python2 syntax
                    e = error(str(val))
                    e.__cause__ = val
                    raise e.with_traceback(tb)
                else:
                    _reraise(error, val, tb)
        finally:
            # avoid traceback refrence cycle
            tp = val = tb = None
ErrorWrapper = ErrorWrapper() # stateless singleton

tealetmap = weakref.WeakValueDictionary()

def getcurrent():
    t = _tealet.current()
    try:
        return tealetmap[t]
    except KeyError:
        if _tealet.main() is t:
            # this is the main tealet, so create a main greenlet
            return greenlet(parent=t)
        # otherwise, this is a a foreign tealet.
        return None

class greenlet(object):
    def __init__(self, run=None, parent=None):
        # must create it on this thread, not dynamically when run
        # this will bind it to the right thread
        self.run = run
        if isinstance(parent, _tealet.tealet):
            # main greenlet for this thread
            self._tealet = parent
            self.parent = self # main greenlets are their own parents and don't go away
            self._main = self
            self._garbage = []
        else:
            self._tealet = _tealet.tealet().stub()
            if not parent:
                parent = getcurrent()
            self.parent = parent
            self._main = parent._main
            # perform housekeeping
            self._housekeeping()
        tealetmap[self._tealet] = self

    def _housekeeping(self):
        if self._main._garbage:
            garbage = self._main._garbage[:]
            del self._main._garbage[:]
            for g in garbage:
                g._kill()

    def __del__(self):
        # since 3.4, __del__ is invoked only once.
        self._kill()

    def _kill(self):
        try:
            if self:
                if _tealet.current() == self._tealet:
                    # Can't kill ourselves from here
                    return
                tealetmap[self._tealet] = self # re-insert
                current = getcurrent()
                if not current:
                    # this is a foreign tealet, not a greenlet
                    self._main._garbage.append(self)
                    return

                old = self.parent
                self.parent = current
                try:
                    self.throw()
                except error:
                    # This must be a foreign tealet.  Insert it to
                    # it's main tealet's garbage heap
                    self._main._garbage.append(self)
                finally:
                    self.parent = old
        except AttributeError as e:
            # ignore attribute errors at exit due to module teardown
            if "NoneType" not in str(e):
                raise

    @property
    def gr_frame(self):
        if self._tealet is _tealet.current():
            return self._tealet.frame
        # tealet is paused.  Emulated greenlet by returning
        # the frame which called "switch" or "throw"
        f = self._tealet.frame
        if f:
            return f.f_back.f_back

    @property
    def dead(self):
        return self._tealet.state == _tealet.STATE_EXIT

    if not six.PY2:
        def __bool__(self):
            return self._tealet.state == _tealet.STATE_RUN
    else:
        def __nonzero__(self):
            return self._tealet.state == _tealet.STATE_RUN

    def switch(self, *args, **kwds):
        return self._switch((False, args, kwds))

    def throw(self, t=None, v=None, tb=None):
        if not t:
            t = GreenletExit
        return self._switch((t, v, tb))

    def _switch(self, arg):
        with ErrorWrapper:
            run = getattr(self, "run", None)
            if run:
                del self.run
                #here we can tweak how we create the new stack
                arg = self._tealet.run(self._greenlet_main, (run, arg))
            else:
                if not self:
                    return self._parent()._switch(arg)
                arg = self._tealet.switch(arg)
        try:
            return self._Result(arg) # can raise arg
        finally:
            # avoid traceback refrence cycle
            arg = None

    @staticmethod
    def _Result(arg):
        # The return value is stored in the current greenlet.
        err, args, kwds = arg
        if err:
            try:
                _reraise(err, args, kwds)
            finally:
                # avoid traceback refrence cycle
                err = args = kwds = arg = None
        if args and kwds:
            return (args, kwds)
        elif kwds:
            return kwds
        elif args:
            if len(args) == 1:
                return args[0]
            return args
        return None

    @staticmethod
    def _greenlet_main(current, arg):
        run, (err, args, kwds) = arg
        try:
            if not err:
                result = _tealet.hide_frame(run, args, kwds)
                arg = (False, (result,), None)
            else:
                try:
                    _reraise(err, args, kwds)
                finally:
                    # avoid traceback refrence cycle
                    err = args = kwds = arg = None
        except GreenletExit as e:
            arg = (False, (e,), None)
        except:
            arg = sys.exc_info()
        p = getcurrent()._parent()
        try:
            return p._tealet, arg
        finally:
            # avoid exception reference cycle
            arg = None

    def _parent(self):
        # Find the closest parent alive
        p = self.parent
        while not p:
            p = p.parent
        return p

    getcurrent = staticmethod(getcurrent)
    error = error
    GreenletExit = GreenletExit
