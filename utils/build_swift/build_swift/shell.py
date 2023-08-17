# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors


"""
Shell utilities wrapper module.
"""


import abc
import collections
import functools
import itertools
import os
import shlex
import shutil
import subprocess
import sys
from copy import copy as _copy
from pathlib import Path
from shlex import quote as _quote
from shlex import split
from subprocess import CalledProcessError


__all__ = [
    'CalledProcessError',
    'AbstractWrapper',
    'CommandWrapper',
    'ExecutableWrapper',

    'quote',
    'split',
    'rerun_as_root',

    'Popen',
    'call',
    'check_call',
    'check_output',

    'copy',
    'pushd',
    'makedirs',
    'move',
    'remove',
    'symlink',
    'which',

    'wraps',

    'ECHO_PREFIX',
    'PIPE',
    'STDOUT',
    'DEVNULL',
]


_PY_VERSION = (sys.version_info.major, sys.version_info.minor)

ECHO_PREFIX = '>>> '

# Re-export subprocess constants
PIPE = subprocess.PIPE
STDOUT = subprocess.STDOUT

try:
    DEVNULL = subprocess.DEVNULL
except AttributeError:
    DEVNULL = -3


# -----------------------------------------------------------------------------
# Helpers

def _flatmap(func, *iterables):
    """Helper function that maps the given func over the iterables and then
    creates a single flat iterable from the results.
    """

    return itertools.chain.from_iterable(map(func, *iterables))


def _convert_pathlib_path(path):
    """Helper function used to convert an instance of pathlib.Path into a
    unicode string.
    """

    if Path is None:
        return path

    if isinstance(path, Path):
        return str(path)

    return path


def _get_stream_file(stream):
    """Helper function used to decode the standard PIPE and STDOUT constants
    into actual file objects.
    """

    if stream == PIPE:
        return sys.stdout
    if stream == STDOUT:
        return sys.stdout
    if stream == DEVNULL:
        raise ValueError('DEVNULL should be replaced by now!')

    return stream


def _echo_command(command, stream, prefix=ECHO_PREFIX):
    """Helper function used to echo a given command to some stream. An optional
    command prefix can be provided.
    """

    if stream == DEVNULL:
        return

    stream = _get_stream_file(stream)

    stream.write('{}{}\n'.format(prefix, quote(command)))
    stream.flush()


def _normalize_args(args):
    """Normalizes a list of arguments containing one or more strings and
    CommandWrapper instances into a one-dimensional list of strings.
    """

    if isinstance(args, (str,)):
        return shlex.split(args)

    def normalize_arg(arg):
        arg = _convert_pathlib_path(arg)

        if isinstance(arg, (str,)):
            return [str(arg)]
        if isinstance(arg, AbstractWrapper):
            return list(map(_convert_pathlib_path, arg.command))

        raise ValueError('Invalid argument type: {}'.format(
            type(arg).__name__))

    if isinstance(args, AbstractWrapper):
        return normalize_arg(args)

    return list(_flatmap(normalize_arg, args))


# -----------------------------------------------------------------------------
# Decorators

def _normalize_command(func):
    """Decorator used to uniformly normalize the input command of the
    subprocess wrappers.
    """

    @functools.wraps(func)
    def wrapper(command, **kwargs):
        if not isinstance(command, (str,)):
            command = _normalize_args(command)

        return func(command, **kwargs)

    return wrapper


def _add_echo_kwarg(func):
    """Decorator used to add the 'echo' keyword-only argument that echos the
    input command to whatever stdout the user passes (or sys.stdout if not
    supplied).
    """

    @functools.wraps(func)
    def wrapper(command, **kwargs):
        if kwargs.pop('echo', False):
            stdout = kwargs.get('stdout', sys.stdout)
            _echo_command(command, stdout)

        return func(command, **kwargs)

    return wrapper


# -----------------------------------------------------------------------------
# Public Functions

def quote(command):
    """Extension of the standard shutil.quote that handles both strings and
    lists of strings. This mirrors how the subprocess package can handle
    commands as both a standalone string or list of strings.

    >>> quote('/Applications/App Store.app')
    "'/Applications/App Store.app'"

    >>> quote(['rm', '-rf', '~/Documents/My Homework'])
    "rm -rf '~/Documents/My Homework'"
    """

    if isinstance(command, (str,)):
        return _quote(command)

    if isinstance(command, collections.abc.Iterable):
        return ' '.join([_quote(arg) for arg in _normalize_args(command)])

    raise ValueError('Invalid command type: {}'.format(type(command).__name__))


def rerun_as_root():
    """Replace the current process with itself running with root permissions.
    Prompt the user for their password to support this.
    """

    euid = os.geteuid()
    if euid == 0:
        return

    args = ['sudo', sys.executable] + sys.argv + [os.environ]
    os.execlpe('sudo', *args)


# -----------------------------------------------------------------------------
# Subprocess Wrappers

class Popen(subprocess.Popen):
    """Wrapper around subprocess.Popen which allows for a more flexible command
    type and echoing the input command to stdout.
    """

    def __init__(self, command, **kwargs):
        """In order to utilize the same function decorators used to back-port
        devnull support and add new features, we create a closure to capture
        the input 'self' value while retaining a standard function signature.

        Django has a really nice (and relatively simple) general purpose
        solution to this problem in the form of their `method_decorator`.
        """

        @_normalize_command
        @_add_echo_kwarg
        def closure(command, **kwargs):
            super(Popen, self).__init__(command, **kwargs)

        closure(command, **kwargs)

    # Back-port the context manager behavior for Python 3.1 and below.
    if _PY_VERSION < (3, 2):
        def __enter__(self):
            return self

        def __exit__(self, *exc):
            self.wait()


@_normalize_command
@_add_echo_kwarg
def call(command, **kwargs):
    """Simple wrapper around subprocess.call which backports DEVNULL support
    and adds support for the echo keyword-only argument.
    """

    return subprocess.call(command, **kwargs)


@_normalize_command
@_add_echo_kwarg
def check_call(command, **kwargs):
    """Simple wrapper around subprocess.check_call which backports DEVNULL
    support and adds support for the echo keyword-only argument.
    """

    return subprocess.check_call(command, **kwargs)


@_normalize_command
@_add_echo_kwarg
def check_output(command, **kwargs):
    """Simple wrapper around subprocess.check_output which backports DEVNULL
    support and adds support for the echo keyword-only argument.

    Output is returned as a unicode string.
    """

    kwargs['encoding'] = 'utf-8'

    output = subprocess.check_output(command, **kwargs)

    return output


# -----------------------------------------------------------------------------
# Shell Utilities

def copy(source, dest, echo=False):
    """Emulates the `cp` command to copy a file or directory.
    """

    source = _convert_pathlib_path(source)
    dest = _convert_pathlib_path(dest)

    if os.path.isfile(source):
        if echo:
            _echo_command(['cp', source, dest], sys.stdout)
        return shutil.copyfile(source, dest)

    if os.path.isdir(source):
        if echo:
            _echo_command(['cp', '-R', source, dest], sys.stdout)
        return shutil.copytree(source, dest)


class pushd(object):
    """Context manager to mimic the behavior of pushd and popd, moving the
    current working directory to a new path and then restoring it when exiting
    the current block.
    """

    def __init__(self, path, echo=False):
        path = _convert_pathlib_path(path)

        self.cwd = os.getcwd()
        self.path = os.path.expanduser(path)
        self.echo = echo

    def __enter__(self):
        if self.echo:
            _echo_command(['pushd', self.path], sys.stdout)

        os.chdir(self.path)
        return self.path

    def __exit__(self, *args):
        if self.echo:
            _echo_command(['popd'], sys.stdout)

        os.chdir(self.cwd)


def makedirs(path, echo=False):
    """Emulates the `mkdir -p` command to recursively create directories for
    the path given if it doesn't already exist.
    """

    path = _convert_pathlib_path(path)
    if os.path.exists(path):
        return

    if echo:
        _echo_command(['mkdir', '-p', path], sys.stdout)

    os.makedirs(path)


def move(source, dest, echo=False):
    """Emulates the `mv` command to move files or directories.
    """

    source = _convert_pathlib_path(source)
    dest = _convert_pathlib_path(dest)

    if echo:
        _echo_command(['mv', source, dest], sys.stdout)

    return shutil.move(source, dest)


def remove(path, echo=False):
    """Emulates the `rm` command for both files and directories.
    """

    path = _convert_pathlib_path(path)

    if os.path.isfile(path):
        if echo:
            _echo_command(['rm', path], sys.stdout)
        return os.remove(path)

    if os.path.isdir(path):
        if echo:
            _echo_command(['rm', '-rf', path], sys.stdout)
        return shutil.rmtree(path, ignore_errors=True)


def symlink(source, dest, echo=False):
    """Emulates the `ln` command to symlink a file or directory.
    """

    source = _convert_pathlib_path(source)
    dest = _convert_pathlib_path(dest)

    if echo:
        _echo_command(['ln', '-s', source, dest], sys.stdout)

    return os.symlink(source, dest)


def which(command, mode=os.F_OK | os.X_OK, path=None):
    """Polyfill for the Python 3 shutil.which function. Does not support
    Windows platforms.
    """

    # Default to environment PATH or os.defpath
    path = path or os.environ.get('PATH', os.defpath)

    for location in path.split(os.pathsep):
        # If command is a full path then candidate will be just command
        candidate = os.path.join(location, command)

        if os.path.isfile(candidate) and os.access(candidate, mode):
            return candidate

    return None


# -----------------------------------------------------------------------------
# Wrappers

def wraps(command):
    """Simple utility function to instantiate a CommandWrapper instance in a
    more fluent way.
    """

    return CommandWrapper(command)


class AbstractWrapper(object, metaclass=abc.ABCMeta):
    """Abstract base class for implementing wrappers around command line
    utilities and executables. Subclasses must implement the `command` method
    which returns a command list suitable for use with executor instances.
    """

    def __call__(self, *args, **kwargs):
        return self.check_call(*args, **kwargs)

    @abc.abstractproperty
    @property
    def command(self):
        """Subclasses must implement a command property.
        """

        raise NotImplementedError()

    def __build_command(self, args):
        args = _normalize_args(args)

        return self.command + _normalize_args(args)

    # -------------------------------------------------------------------------

    def Popen(self, args, **kwargs):
        return Popen(self.__build_command(args), **kwargs)

    def call(self, args, **kwargs):
        return call(self.__build_command(args), **kwargs)

    def check_call(self, args, **kwargs):
        return check_call(self.__build_command(args), **kwargs)

    def check_output(self, args, **kwargs):
        return check_output(self.__build_command(args), **kwargs)


class CommandWrapper(AbstractWrapper):
    """Wrapper class for command line utilities which can be initialized
    on-demand for the desired command.
    """

    __slots__ = ('_command')

    def __init__(self, command):
        super(CommandWrapper, self).__init__()

        self._command = _normalize_args(command)

    @property
    def command(self):
        return _copy(self._command)


class ExecutableWrapper(AbstractWrapper):
    """Wrapper class for executable utilities. This class is suitable as a base
    class for implementing wrapper classes around executables, simply subclass
    and define the `EXECUTABLE` attribute to the correct executable name or
    path.
    """

    EXECUTABLE = None

    def __init__(self):
        if self.EXECUTABLE is None:
            raise AttributeError('{}.EXECUTABLE cannot be None'.format(
                type(self).__name__))

        self.EXECUTABLE = _convert_pathlib_path(self.EXECUTABLE)

        if not isinstance(self.EXECUTABLE, (str,)):
            raise AttributeError(
                '{}.EXECUTABLE must be an executable name or path'.format(
                    type(self).__name__))

        super(ExecutableWrapper, self).__init__()

    @property
    def command(self):
        return [_copy(self.EXECUTABLE)]

    @property
    def path(self):
        return which(self.EXECUTABLE)
