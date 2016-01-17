#setup.py
# coding=utf-8
# the setuping code
from __future__ import print_function
import os
import os.path
import subprocess

from setuptools import setup, Extension, find_packages
from distutils.unixccompiler import UnixCCompiler
from distutils.msvccompiler import MSVCCompiler
import distutils.ccompiler

# To use a consistent encoding
from codecs import open

here = os.path.abspath(os.path.dirname(__file__))

# Get the long description from the README file
with open(os.path.join(here, 'README.rst'), encoding='utf-8') as f:
    long_description = f.read()

def run_setup(extra_src, extra_obj):
    _tealet = Extension(
        name="_tealet",
        sources=[
            "src/_tealet.c",
            "ctealet/tealet.c",
            "ctealet/tools.c",
        ] + extra_src,
        include_dirs=[
            "ctealet",
        ],
        extra_objects=extra_obj,
    )

    setup(
        name="tealet",
        version='0.1.0',
        description="A tealet module for Python",
        long_description=long_description,
        url="https://bitbucket.org/krisvale/tealet",
        author="Kristján Valur Jónsson",
        author_email="sweskman@gmail.com",
        license="MIT",

        ext_modules=[_tealet,],
        packages=["tealet"],
        test_suite="tealet.tests",
        install_requires=["six"],
        classifiers=[
            'Development Status :: 3 - Alpha',
            'Intended Audience :: Developers',
            'Topic :: Software Development :: Libraries',
            'License :: OSI Approved :: MIT License',
            'Programming Language :: Python :: 2',
            'Programming Language :: Python :: 2.6',
            'Programming Language :: Python :: 2.7',
            'Programming Language :: Python :: 3',
        ],
        keywords='development stack-slicing lightweight-threads',
    )


# find any extra assembly sources by compiling and executing
# a special filet
def new_compiler():
    return distutils.ccompiler.new_compiler()
def find_asm():
    compiler = new_compiler()
    source = 'ctealet/platf_tealet/tealet_platformselect.c'
    a_out = "./a_out"
    compiler.compile([source])
    compiler.link_executable(compiler.object_filenames([source]), a_out)
    exe = compiler.executable_filename(a_out)
    out = subprocess.check_output([exe], stderr = subprocess.STDOUT)
    os.unlink(exe)
    asm = []
    extensions = set()
    for l in out.splitlines():
       asm.append("ctealet/" + l)
       extensions.add(os.path.splitext(l)[1])

    # patch compiler if it is a unixcompiler and make it able
    # to compiler .s objects
    if isinstance(compiler, distutils.unixccompiler.UnixCCompiler):
        for ext in extensions:
            compiler.src_extensions.append(ext)
    return asm

def find_obj(asm):
    # on windows, we have to assemble object files ourselves
    compiler = new_compiler()
    if not isinstance(compiler, distutils.msvccompiler.MSVCCompiler):
        return []
    compiler.initialize()
    # todo: detect 64 bit windows
    cmd = "ml.exe"
    args = ["/coff"] # win32 only
    ml = compiler.find_exe(cmd) # the assembler.

    # patch compiler to recognize .asm as a source
    compiler.src_extensions.append(".asm")
    objects = compiler.object_filenames(asm, 0, compiler.output_dir)
    for a,o in zip(asm, objects):
        cmd = [ml, "/c", "/Cx"] + args + ["/Fo"+o, a]
        compiler.spawn(cmd)
        objects.append(o)
    return objects

def extra_files():
    asm = find_asm()
    obj = find_obj(asm)
    if obj:
        return [], obj
    else:
        return asm, []

a, o = extra_files()
run_setup(a, o)
# remove temporary objects if present
for f in o:
    try:
        os.unlink(f)
    except OSError:
        pass
