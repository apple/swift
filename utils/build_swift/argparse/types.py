# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors


import os.path
import re
import shlex

from . import ArgumentTypeError


__all__ = [
    'PathType',
    'RegexType',
    'ClangVersionType',
    'SwiftVersionType',
]


# -----------------------------------------------------------------------------

class PathType(object):
    """PathType denotes a valid path-like object. When called paths will be
    fully expanded with the option to assert the file or directory referenced
    by the path exists.
    """

    def __init__(self, assert_exists=False):
        self.assert_exists = assert_exists

    def __call__(self, path):
        path = os.path.expanduser(path)
        path = os.path.abspath(path)
        path = os.path.realpath(path)

        if self.assert_exists:
            assert os.path.exists(path)

        return path


class RegexType(object):
    """Argument type used to validate an input string against a regular
    expression.
    """

    def __init__(self, regex, error_message=None):
        self._regex = regex
        self._error_message = error_message or 'Invalid value'

    def __call__(self, value):
        matches = re.match(self._regex, value)
        if matches is None:
            raise ArgumentTypeError(self._error_message, value)

        return value


class ClangVersionType(RegexType):
    """Argument type used to validate Clang version strings.
    """

    ERROR_MESSAGE = ('Invalid version value, must be '
                     '"MAJOR.MINOR.PATCH" or "MAJOR.MINOR.PATCH.PATCH"')

    VERSION_REGEX = r'^(\d+)\.(\d+)\.(\d+)(\.(\d+))?$'

    def __init__(self):
        super(ClangVersionType, self).__init__(
            ClangVersionType.VERSION_REGEX,
            ClangVersionType.ERROR_MESSAGE)


class SwiftVersionType(RegexType):
    """Argument type used to validate Swift version strings.
    """

    ERROR_MESSAGE = ('Invalid version value, must be "MAJOR.MINOR" '
                     'or "MAJOR.MINOR.PATCH"')
    VERSION_REGEX = r'^(\d+)\.(\d+)(\.(\d+))?$'

    def __init__(self):
        super(SwiftVersionType, self).__init__(
            SwiftVersionType.VERSION_REGEX,
            SwiftVersionType.ERROR_MESSAGE)


class ShellSplitType(object):
    """Parse and split shell arguments into a list of strings. Recognizes `,`
    as a separator as well as white spaces.

    For example it converts the following:

    '-BAR="foo bar" -BAZ="foo,bar",-QUX 42'

    into

    ['-BAR=foo bar', '-BAZ=foo,bar', '-QUX', '42']
    """

    def __call__(self, value):
        lex = shlex.shlex(value, posix=True)
        lex.whitespace_split = True
        lex.whitespace += ','
        return list(lex)
