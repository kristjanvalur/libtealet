
import unittest
import math

import _tealet
import random
random.seed(0)

# Utility stuff for creating tealets
def tealet_new_descend(descend, func=None, arg=None, klass=_tealet.tealet, retarg=False):
    while descend > 0:
        return tealet_new_descend(descend-1, func, arg, klass=klass, retarg=retarg)
    t = klass()
    if func:
        r = t.run(func, arg)
    else:
        t.stub()
        r = None
    return (t, r) if retarg else t

def tealet_new_rnd(func=None, arg=None, klass=_tealet.tealet, retarg=False):
    return tealet_new_descend(random.randint(0, 20), func, arg, klass, retarg)

def stub_new(func=None, arg=None, klass=_tealet.tealet, retarg=False):
    stub = tealet_new_descend(random.randint(0, 20), klass=klass, retarg=False)
    if func:
        r = stub.run(func, arg)
    else:
        r = None
    return (dup, r) if retarg else dup

def stub_new2(func=None, arg=None, klass=_tealet.tealet, retarg=False):
    stub = tealet_new_descend(random.randint(0, 20), klass=klass, retarg=False)
    dup = klass(stub)
    if func:
        r = dup.run(func, arg)
    else:
        r = None
    return (dup, r) if retarg else dup

the_stub=[None]
def stub_new3(func=None, arg=None, klass=_tealet.tealet, retarg=False):
    if (random.randint(0, 10) == 0):
        the_stub[0] = None
    if not the_stub[0]:
        the_stub[0] = tealet_new_descend(random.randint(0, 20), klass=klass)
    dup = klass(the_stub[0])
    if func:
        r = dup.run(func, arg)
    else:
        r = None
    return (dup, r) if retarg else dup

newmode = 0
newarray = [tealet_new_rnd, stub_new, stub_new2, stub_new3]
def get_new():
    if newmode >= 0:
        return newarray[newmode]
    return newarray(random.randint(0, len(newarray)-1))


class TestModule(unittest.TestCase):
    def testMain(self):
        self.assertEqual(_tealet.main(), _tealet.current())

    def testMain2(self):
        self.assertEqual(_tealet.main(), _tealet.current().main)

    def testMain3(self):
        self.assertEqual(_tealet.main().state, _tealet.STATE_RUN)

class TestSimple(unittest.TestCase):
    def testSimple(self):
        status = [0]
        def run(current, arg):
            status[0] = 1
            return arg
        get_new()(run, _tealet.current())
        self.assertEqual(status[0], 1)

class TestStatus(unittest.TestCase):
    def testStatusRun(self):
        t = _tealet.current()
        self.assertEqual(t.main, _tealet.main())
        self.assertEqual(t.state, _tealet.STATE_RUN)

    def testStatusStub(self):
        stub = get_new()()
        status = [None]
        self.assertEqual(stub.state, _tealet.STATE_STUB)
        def run(current, arg):
            status[0] = current.state
            return arg
        stub.run(run, _tealet.current())
        self.assertEqual(status[0], _tealet.STATE_RUN)

class TestSubclass(unittest.TestCase):
    class sc(_tealet.tealet):
        dude = [0]
        def __repr__(self):
            return "<myrepr %r>"%super(TestSubclass.sc, self).__repr__()
        def __del__(self):
            self.dude[0] = 1
    def testSubclass(self):
        def foo(current, arg):
            arg.switch(current)
            return arg
        t = get_new()(foo, _tealet.current(), klass=self.sc)
        self.assertEqual(repr(t)[:7], "<myrepr")
        self.assertEqual(self.sc.dude[0], 0)
        t.switch()
        self.assertEqual(self.sc.dude[0], 0)
        del t
        self.assertEqual(self.sc.dude[0], 1)

class TestSwitch(unittest.TestCase):
    def testSwitch(self):
        status = [0]
        t = [None, None]
        def t2(current, arg):
            self.assertNotEqual(current, _tealet.main())
            self.assertNotEqual(current, t[0])
            t[1] = current
            self.assertEqual(status[0], 1)
            status[0] = 2
            self.assertEqual(_tealet.current(), current)
            t[0].switch()
            self.assertEqual(status[0], 3)
            status[0] = 4
            self.assertEqual(_tealet.current(), current)
            t[0].switch()
            self.assertEqual(status[0], 5)
            status[0] = 6
            self.assertEqual(current, t[1])
            self.assertEqual(_tealet.current(), current)
            t[1].switch() #noop
            self.assertEqual(status[0], 6)
            status[0] = 7
            self.assertEqual(_tealet.current(), current)
            return _tealet.main()

        def t1(current, arg):
            self.assertNotEqual(current, _tealet.main())
            t[0] = current
            self.assertEqual(status[0], 0)
            status[0] = 1
            self.assertEqual(current, _tealet.current())
            get_new()(t2)
            self.assertEqual(status[0], 2)
            status[0] = 3
            self.assertEqual(current, _tealet.current())
            t[1].switch()
            self.assertEqual(status[0], 4)
            status[0] = 5
            self.assertEqual(current, _tealet.current())
            return t[1]

        get_new()(t1)
        self.assertEqual(status[0], 7)


    def testSwitchNew(self):
        # 1 is high on the stack.  We then create 2 lower on the stack
        # the execution is : m 1 m 2 1 m 2 m */
        def new1(current, arg):
            # switch back to the creator
            arg.switch()
            # now we want to trample the stack
            stub = tealet_new_descend(50)
            del stub
            # back to main
            return _tealet.main()

        def new2(current, arg):
            # switch to tealet 1 to trample the stack
            arg.switch();
            # back to main
            return _tealet.main()

        tealet1 = get_new()(new1, _tealet.current())
        # the tealet is now running
        tealet2 = tealet_new_descend(4, new2, tealet1)

        self.assertEqual(tealet2.state, _tealet.STATE_RUN);
        tealet2.switch()

    def testSwitchArg(self):
        # 1 is high on the stack.  We then create 2 lower on the stack
        # the execution is : m 1 m 2 1 m 2 m */
        def new1(current, arg):
            # switch back to the creator
            r = arg.switch(2)
            self.assertEqual(r, 4)
            # now we want to trample the stack
            stub = tealet_new_descend(50)
            del stub
            # back to main
            return _tealet.main(), 5

        def new2(current, arg):
            # switch to tealet 1 to trample the stack
            r = arg.switch(4);
            self.assertEqual(r, 6)
            # back to main
            return _tealet.main(), 7

        tealet1, r = get_new()(new1, _tealet.current(), retarg=True)
        self.assertEqual(r, 2)
        # the tealet is now running
        tealet2, r = tealet_new_descend(4, new2, tealet1, retarg=True)
        self.assertEqual(r, 5)

        self.assertEqual(tealet2.state, _tealet.STATE_RUN);
        r = tealet2.switch(6)
        self.assertEqual(r, 7)


class TestRandom1(unittest.TestCase):
    max_status = 10000

    def randomRun(self, index):
        cur = _tealet.current()
        while True:
            i = random.randint(0, len(self.tealets))
            self.status += 1;
            if i == len(self.tealets):
                break
            prevstatus = self.status
            self.got_index = i
            if not self.tealets[i]:
                if self.status >= self.max_status:
                    break
                #print "new", i
                get_new()(self.randomTealet, i)
            else:
                #print 'switch', i
                d = self.tealets[i].switch()
                #assert d == math.sqrt(2344.2)
            self.assertTrue(self.status >= prevstatus)
            self.assertEqual(_tealet.current(), cur)
            self.assertEqual(self.tealets[index], cur)
            self.assertEqual(self.got_index, index)
            if self.status >= self.max_status:
                break

    def randomTealet(self, current, index):
        i = self.got_index;
        self.assertEqual(_tealet.current(), current)
        self.assertEqual(i, index)
        self.assertTrue(i > 0 and i < len(self.tealets))
        self.assertEqual(self.tealets[i], None)
        self.tealets[i] = current
        self.randomRun(i)
        self.tealets[i] = None

        i = random.randint(0, len(self.tealets)-1)
        if not self.tealets[i]:
            self.assertTrue(self.tealets[0]);
            i = 0
        self.got_index = i
        #print "ret", i
        return self.tealets[i]

    def testRandom(self):
        self.tealets = [None]*127
        self.status = 0
        self.tealets[0] = _tealet.current()
        while self.status < self.max_status:
            self.randomRun(0)

        self.assertEqual(_tealet.current(), self.tealets[0])
        for i in range(1, len(self.tealets)):
            while self.tealets[i]:
                self.randomRun(0)


class TestRandom2(unittest.TestCase):
    MAX_STATUS = 10000
    N_RUNS = 10
    MAX_DESCEND = 20
    ARRAYSIZE = 127

    def randomTealet(self, cur, index):
        self.assertEqual(_tealet.current(), cur)
        self.assertTrue(index > 0 and index < len(self.tealets))
        self.assertEqual(self.tealets[index], None)
        self.tealets[index] = cur
        self.randomRun(index)
        self.tealets[index] = None
        return self.tealets[0] # switch to main

    def randomRun(self, index):
        self.assertTrue(self.tealets[index] == None or self.tealets[index] == _tealet.current())
        self.tealets[index] = _tealet.current()
        for i in range(self.N_RUNS):
            if self.randomDescend(index, random.randint(0, self.MAX_DESCEND+1)) == 0:
                break;
        self.tealets[index] = None

    def randomDescend(self, index, level):
        if level > 0:
            return self.randomDescend(index, level-1)
        # find target
        target = random.randint(0, len(self.tealets)-1)
        if self.status < self.MAX_STATUS:
            self.status += 1;
            if not self.tealets[target]:
                get_new()(self.randomTealet, target)
            else:
                self.tealets[target].switch()
            return 1
        else:
            # find a telet other than us to flush
            for j in range(len(self.tealets)):
                k = (j + target) % len(self.tealets)
                if k != index and self.tealets[k]:
                    self.status += 1;
                    self.tealets[k].switch()
                    return 1
            return 0

    def testRandom(self):
        self.tealets = [None] * self.ARRAYSIZE
        self.status = 0
        self.tealets[0] = _tealet.current()

        while self.status < self.MAX_STATUS:
            self.randomRun(0);

        # drain the system
        self.tealets[0] = _tealet.current()
        while True:
            found = False
            for i, t in enumerate(self.tealets[1:]):
                if t:
                    self.status += 1
                    t.switch()
                    found = True
                    break
            if not found:
                break
        self.tealets[0] = None



if __name__ == '__main__':
    import sys
    if not sys.argv[1:]:
        sys.argv.append('-v')

    for i in range(0, len(newarray)):
        newmode = i
        unittest.main()
    newmode = -1
    unittest.main()

