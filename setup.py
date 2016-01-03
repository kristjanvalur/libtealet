#setup.py
# coding=utf-8
# the setuping code
import os
import os.path

from setuptools import setup, Extension, find_packages

# To use a consistent encoding
from codecs import open

here = os.path.abspath(os.path.dirname(__file__))

# Get the long description from the README file
with open(os.path.join(here, 'README.rst'), encoding='utf-8') as f:
    long_description = f.read()
            
_tealet = Extension(
    name="_tealet",
    sources=[
        "src/_tealet.c",
        "ctealet/tealet.c",
    ],
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
)
