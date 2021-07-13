# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors


"""
Default option value definitions.
"""


from __future__ import absolute_import, unicode_literals

import os
import platform

from . import shell
from .versions import Version


__all__ = [
    # Command line configurable
    'BUILD_VARIANT',
    'CMAKE_GENERATOR',
    'COMPILER_VENDOR',
    'SWIFT_USER_VISIBLE_VERSION',
    'CLANG_USER_VISIBLE_VERSION',
    'SWIFT_ANALYZE_CODE_COVERAGE',
    'DARWIN_XCRUN_TOOLCHAIN',
    'DARWIN_DEPLOYMENT_VERSION_OSX',
    'DARWIN_DEPLOYMENT_VERSION_IOS',
    'DARWIN_DEPLOYMENT_VERSION_TVOS',
    'DARWIN_DEPLOYMENT_VERSION_WATCHOS',
    'UNIX_INSTALL_PREFIX',
    'DARWIN_INSTALL_PREFIX',
    'LLVM_MAX_PARALLEL_LTO_LINK_JOBS',
    'SWIFT_MAX_PARALLEL_LTO_LINK_JOBS',
    'DSYMUTIL_JOBS'

    # Constants
]

# Options that can be "configured" by command line options

BUILD_VARIANT = 'Debug'
CMAKE_GENERATOR = 'Ninja'

COMPILER_VENDOR = 'none'
SWIFT_USER_VISIBLE_VERSION = Version('5.5')
CLANG_USER_VISIBLE_VERSION = Version('10.0.0')
SWIFT_ANALYZE_CODE_COVERAGE = 'false'

DARWIN_XCRUN_TOOLCHAIN = 'default'
DARWIN_DEPLOYMENT_VERSION_OSX = '10.9'
DARWIN_DEPLOYMENT_VERSION_IOS = '7.0'
DARWIN_DEPLOYMENT_VERSION_TVOS = '9.0'
DARWIN_DEPLOYMENT_VERSION_WATCHOS = '2.0'

UNIX_INSTALL_PREFIX = '/usr'
DARWIN_INSTALL_PREFIX = ('/Applications/Xcode.app/Contents/Developer/'
                         'Toolchains/XcodeDefault.xctoolchain/usr')

DSYMUTIL_JOBS = 1


def _system_memory():
    """Returns the system memory as an int. None if the system memory cannot
    be determined.

    TODO: Support Linux and Windows platforms.
    """

    if platform.platform() == 'Darwin':
        try:
            output = shell.check_output(['sysctl', 'hw.memsize']).strip()
            return int(output.split(' ')[1])
        except shell.CalledProcessError:
            return None

    return None


def _default_llvm_lto_link_jobs():
    """Use the formula (GB Memory - 3)/6.0GB to get the number of parallel
    link threads we can support. This gives the OS 3 GB of room to work with.

    This is a bit conservative, but I have found that this hueristic prevents
    me from swapping on my test machine.
    """

    memory = _system_memory()
    if memory is None:
        return None

    return int((memory / 1000000000.0 - 3.0) / 6.0)


def _default_swift_lto_link_jobs():
    """Use the formula (GB Memory - 3)/8.0GB to get the number of parallel
    link threads we can support. This gives the OS 3 GB of room to work with.

    This is a bit conservative, but I have found that this hueristic prevents
    me from swapping on my test machine.
    """

    memory = _system_memory()
    if memory is None:
        return None

    return int((memory / 1000000000.0 - 3.0) / 8.0)


LLVM_MAX_PARALLEL_LTO_LINK_JOBS = _default_llvm_lto_link_jobs()
SWIFT_MAX_PARALLEL_LTO_LINK_JOBS = _default_swift_lto_link_jobs()


def llvm_install_components():
    """Convenience function for getting the default llvm install components for
    platforms.
    """
    components = ['llvm-cov', 'llvm-profdata', 'IndexStore', 'clang',
                  'clang-resource-headers', 'compiler-rt', 'clangd']
    if os.sys.platform == 'darwin':
        components.extend(['dsymutil'])
    else:
        components.extend(['lld'])
    return ';'.join(components)

# Options that can only be "configured" by editing this file.
#
# These options are not exposed as command line options on purpose.  If you
# need to change any of these, you should do so on trunk or in a branch.
