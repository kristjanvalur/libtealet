#setup.py
# coding=utf-8
# the setuping code
from __future__ import print_function
import os
import os.path
import subprocess

from setuptools import setup, Extension, find_packages
from distutils.unixccompiler import UnixCCompiler

# To use a consistent encoding
from codecs import open

here = os.path.abspath(os.path.dirname(__file__))

# Get the long description from the README file
with open(os.path.join(here, 'README.rst'), encoding='utf-8') as f:
    long_description = f.read()

# find any extra assembly sources
asm = []
cmd = ("cc ctealet/platf_tealet/tealet_platformselect.c && "
       "./a.out && rm a.out") 
try:
    out = subprocess.check_output(cmd, shell = True, stderr = subprocess.STDOUT)
    for l in out.splitlines():
       asm.append("ctealet/" + l)
except subprocess.CalledProcessError:
    pass

#patch type to allow assembly sources
UnixCCompiler.src_extensions.append(".s")

_tealet = Extension(
    name="_tealet",
    sources=[
        "src/_tealet.c",
        "ctealet/tealet.c",
        "ctealet/tools.c",
    ] + asm,
    include_dirs=[
        "ctealet",
    ],
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
)
