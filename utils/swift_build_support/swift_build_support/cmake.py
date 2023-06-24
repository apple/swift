# swift_build_support/cmake.py - Detect host machine's CMake -*- python -*-
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
# ----------------------------------------------------------------------------
#
# Find the path to a CMake executable on the host machine.
#
# ----------------------------------------------------------------------------


import multiprocessing
import os
import platform
import re
from numbers import Number

from . import shell


class CMakeOptions(object):
    """List like object used to define cmake options
    """

    def __init__(self, initial_options=None):
        self._options = []
        if initial_options is not None:
            self.extend(initial_options)

    def define(self, var, value):
        """Utility to define cmake options in this object.

        opts.define("FOO", "BAR")       # -> -DFOO=BAR
        opts.define("FLAG:BOOL", True)  # -> -FLAG:BOOL=TRUE
        """
        if var.endswith(':BOOL') or isinstance(value, bool):
            value = self.true_false(value)
        if value is None:
            value = ""
        elif not isinstance(value, (str, Number)):
            raise ValueError('define: invalid value for key %s: %s (%s)' %
                             (var, value, type(value)))
        self._options.append('-D%s=%s' % (var, value))

    def undefine(self, var):
        """Utility to undefine cmake options in this object.

        opts.undefine("FOO")       # -> -UFOO
        """
        self._options.append('-U%s' % var)

    def extend(self, tuples_or_options):
        if isinstance(tuples_or_options, CMakeOptions):
            self += tuples_or_options
        else:
            for (variable, value) in tuples_or_options:
                self.define(variable, value)

    def extend_raw(self, option_strings):
        self._options.extend(option_strings)

    @staticmethod
    def true_false(value):
        if hasattr(value, 'lower'):
            value = value.lower()
        if value in [True, 1, 'true', 'yes', '1']:
            return 'TRUE'
        if value in [False, 0, 'false', 'no', '0']:
            return 'FALSE'
        raise ValueError("true_false: invalid value: %s" % value)

    def __len__(self):
        return self._options.__len__()

    def __iter__(self):
        return self._options.__iter__()

    def __contains__(self, item):
        return self._options.__contains__(item)

    def __add__(self, other):
        ret = CMakeOptions()
        ret._options += self._options
        ret._options += list(other)
        return ret

    def __iadd__(self, other):
        self._options += list(other)
        return self


class CMake(object):

    def __init__(self, args, toolchain, prefer_just_built_toolchain=False):
        """If prefer_just_built_toolchain is set to True, we set the clang, clang++,
        and Swift compilers from the installed toolchain.
        """
        self.args = args
        self.toolchain = toolchain
        self.prefer_just_built_toolchain = prefer_just_built_toolchain

    def common_options(self, product=None):
        """Return options used for all products, including LLVM/Clang
        """
        args = self.args
        toolchain = self.toolchain
        options = CMakeOptions()
        define = options.define

        options += ['-G', args.cmake_generator]

        sanitizers = []
        if args.enable_asan:
            sanitizers.append('Address')
        if args.enable_ubsan:
            sanitizers.append('Undefined')
        if args.enable_tsan:
            sanitizers.append('Thread')
        if args.enable_lsan:
            sanitizers.append('Leaks')
        if sanitizers:
            define("LLVM_USE_SANITIZER", ";".join(sanitizers))

        if args.enable_sanitize_coverage:
            define("LLVM_USE_SANITIZE_COVERAGE", "ON")

        if args.export_compile_commands:
            define("CMAKE_EXPORT_COMPILE_COMMANDS", "ON")

        if args.distcc:
            define("CMAKE_C_COMPILER_LAUNCHER:PATH", toolchain.distcc)
            define("CMAKE_CXX_COMPILER_LAUNCHER:PATH", toolchain.distcc)

        if args.sccache:
            define("CMAKE_C_COMPILER_LAUNCHER:PATH", toolchain.sccache)
            define("CMAKE_CXX_COMPILER_LAUNCHER:PATH", toolchain.sccache)

        if args.cmake_c_launcher:
            define("CMAKE_C_COMPILER_LAUNCHER:PATH", args.cmake_c_launcher)
        if args.cmake_cxx_launcher:
            define("CMAKE_CXX_COMPILER_LAUNCHER:PATH", args.cmake_cxx_launcher)

        if self.prefer_just_built_toolchain and product:
            toolchain_path = product.install_toolchain_path(args.host_target)
            cmake_swiftc_path = os.getenv('CMAKE_Swift_COMPILER',
                                          os.path.join(toolchain_path, 'bin', 'swiftc'))
            define("CMAKE_C_COMPILER:PATH", os.path.join(toolchain_path,
                                                         'bin', 'clang'))
            define("CMAKE_CXX_COMPILER:PATH", os.path.join(toolchain_path,
                                                           'bin', 'clang++'))
            define("CMAKE_Swift_COMPILER:PATH", cmake_swiftc_path)
        else:
            cmake_swiftc_path = os.getenv('CMAKE_Swift_COMPILER', toolchain.swiftc)
            define("CMAKE_C_COMPILER:PATH", toolchain.cc)
            define("CMAKE_CXX_COMPILER:PATH", toolchain.cxx)
            define("CMAKE_Swift_COMPILER:PATH", cmake_swiftc_path)
        define("CMAKE_LIBTOOL:PATH", toolchain.libtool)
        define("CMAKE_AR:PATH", toolchain.ar)
        define("CMAKE_RANLIB:PATH", toolchain.ranlib)

        if args.cmake_generator == 'Xcode':
            define("CMAKE_CONFIGURATION_TYPES",
                   "Debug;Release;MinSizeRel;RelWithDebInfo")

        if args.clang_user_visible_version:
            major, minor, patch = \
                args.clang_user_visible_version.components[0:3]
            define("LLVM_VERSION_MAJOR:STRING", major)
            define("LLVM_VERSION_MINOR:STRING", minor)
            define("LLVM_VERSION_PATCH:STRING", patch)
            define("CLANG_VERSION_MAJOR:STRING", major)
            define("CLANG_VERSION_MINOR:STRING", minor)
            define("CLANG_VERSION_PATCH:STRING", patch)

        if args.build_ninja and args.cmake_generator == 'Ninja':
            define('CMAKE_MAKE_PROGRAM', toolchain.ninja)
        elif args.cmake_generator == 'Ninja' and toolchain.ninja is not None:
            define('CMAKE_MAKE_PROGRAM', toolchain.ninja)

        return options

    def build_args(self):
        """Return arguments to the build tool used for all products
        """
        args = self.args
        toolchain = self.toolchain
        jobs = args.build_jobs
        if args.distcc:
            jobs = shell.capture([toolchain.distcc, '-j'],
                                 dry_run=False, echo=False).rstrip()

        build_args = list(args.build_args)

        if args.cmake_generator == 'Ninja':
            build_args += ['-j%s' % jobs]
            if args.verbose_build:
                build_args += ['-v']

        elif args.cmake_generator == 'Unix Makefiles':
            build_args += ['-j%s' % jobs]
            if args.verbose_build:
                build_args += ['VERBOSE=1']

        elif args.cmake_generator == 'Xcode':
            build_args += ['-parallelizeTargets',
                           '-jobs', str(jobs)]

        return build_args

    # Determine the version of the installed CMake binary.
    def installed_cmake_version(self, cmake_binary):
        version = shell.capture([cmake_binary, '--version'], dry_run=False,
                                echo=True, optional=True)
        (c_major, c_minor, c_patch) = (0, 0, 0)
        if version is not None:
            x = re.findall(r'cmake version (\d+)\.(\d+)\.(\d+)',
                           version.rstrip())
            if len(x) == 1:
                (c_major, c_minor, c_patch) = map(int, x[0])

        return (c_major, c_minor, c_patch)

    # Determine the version of the checked out CMake source.
    def cmake_source_version(self, cmake_source_dir):
        cmake_version_file = os.path.join(cmake_source_dir, 'Source',
                                          'CMakeVersion.cmake')
        major = -1
        minor = -1
        patch = -1

        file = open(cmake_version_file, "r")
        for line in file.readlines():
            m = re.findall(r'set\(CMake_VERSION_MAJOR (\d+)\)', line)
            if len(m) == 1:
                major = int(m[0])
                continue

            m = re.findall(r'set\(CMake_VERSION_MINOR (\d+)\)', line)
            if len(m) == 1:
                minor = int(m[0])
                continue

            m = re.findall(r'set\(CMake_VERSION_PATCH (\d+)\)', line)
            if len(m) == 1:
                patch = int(m[0])
                continue

        if major == -1 or minor == -1 or patch == -1:
            raise RuntimeError("Cant determine CMake version from %s"
                               % cmake_version_file)

        return (major, minor, patch)

    # Build CMake from source.
    def build_cmake(self, source_root, build_root):
        cmake_bootstrap = os.path.join(source_root, 'cmake', 'bootstrap')

        if hasattr(self.args, 'build_script_impl_args'):
            for opt in self.args.build_script_impl_args:
                m = re.findall('--build-dir=(.*)', opt)
                if len(m) == 1:
                    build_root = m[0]

        cmake_build_dir = os.path.join(build_root, 'cmake-%s' %
                                       self.args.host_target)
        if not os.path.isdir(cmake_build_dir):
            os.makedirs(cmake_build_dir)

        cwd = os.getcwd()
        os.chdir(cmake_build_dir)
        build_jobs = self.args.build_jobs or multiprocessing.cpu_count()
        shell.call_without_sleeping([cmake_bootstrap, '--no-qt-gui',
                                     '--parallel=%s' % build_jobs, '--',
                                     '-DCMAKE_USE_OPENSSL=OFF'], echo=True)
        shell.call_without_sleeping(['make', '-j%s' % build_jobs],
                                    echo=True)
        os.chdir(cwd)
        return os.path.join(cmake_build_dir, 'bin', 'cmake')

    # For Linux and FreeBSD only, determine the version of the installed
    # CMake compared to the source and build the source if necessary.
    # Returns the path to the cmake binary.
    def check_cmake_version(self, source_root, build_root):
        if not platform.system() in ["Linux", "FreeBSD"]:
            return

        cmake_source_dir = os.path.join(source_root, 'cmake')
        # If the source is not checked out then don't attempt to build cmake.
        if not os.path.isdir(cmake_source_dir):
            return

        cmake_binary = 'cmake'
        try:
            if self.args.cmake is not None:
                cmake_binary = self.args.cmake
        except AttributeError:
            cmake_binary = 'cmake'

        installed_ver = self.installed_cmake_version(cmake_binary)
        if installed_ver >= self.cmake_source_version(cmake_source_dir):
            return
        else:
            # Build CMake from source and return the path to the executable.
            return self.build_cmake(source_root, build_root)
