# swift_build_support/products/wasisysroot.py -------------------*- python -*-
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2023 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
# ----------------------------------------------------------------------------

import multiprocessing
import os

from . import cmake_product
from . import llvm
from . import product
from .. import shell


class WASILibc(product.Product):
    @classmethod
    def product_source_name(cls):
        return "wasi-libc"

    @classmethod
    def is_build_script_impl_product(cls):
        return False

    @classmethod
    def is_before_build_script_impl_product(cls):
        return False

    def should_build(self, host_target):
        # wasi-libc should always be built if standard library is being built for
        # WebAssembly.
        return self.args.build_wasmstdlib

    def should_test(self, host_target):
        return False

    def should_install(self, host_target):
        return False

    def build(self, host_target):
        self._build(host_target)
        self._build(host_target, thread_model='posix')

    def _build(self, host_target, thread_model='single'):
        build_root = os.path.dirname(self.build_dir)
        llvm_build_bin_dir = os.path.join(
            '..', build_root, '%s-%s' % ('llvm', host_target), 'bin')
        llvm_tools_path = self.args.native_llvm_tools_path or llvm_build_bin_dir
        clang_tools_path = self.args.native_clang_tools_path or llvm_build_bin_dir
        build_jobs = self.args.build_jobs or multiprocessing.cpu_count()

        sysroot_build_dir = WASILibc.sysroot_build_path(build_root, host_target)
        # FIXME: Manually create an empty dir that is usually created during
        # check-symbols. The directory is required during sysroot installation step.
        os.makedirs(os.path.join(sysroot_build_dir, "share"), exist_ok=True)

        shell.call([
            'make', 'install',
            '-j', str(build_jobs),
            # FIXME: wasi-libc's pre-defined macro list does not expect
            # `__FPCLASS_XXX`, which is introduced by the LLVM 17, yet.
            # So skip the symbol check step by treating the phony target
            # as very old file.
            # https://github.com/llvm/llvm-project/commit/7dd387d2971d7759cadfffeb2082439f6c7ddd49
            '--old-file=check-symbols',
            '-C', self.source_dir,
            'OBJDIR=' + os.path.join(self.build_dir, 'obj-' + thread_model),
            'SYSROOT=' + sysroot_build_dir,
            'INSTALL_DIR=' + WASILibc.sysroot_install_path(build_root),
            'CC=' + os.path.join(clang_tools_path, 'clang'),
            'AR=' + os.path.join(llvm_tools_path, 'llvm-ar'),
            'NM=' + os.path.join(llvm_tools_path, 'llvm-nm'),
            'THREAD_MODEL=' + thread_model,
        ])

    @classmethod
    def get_dependencies(cls):
        return [llvm.LLVM]

    @classmethod
    def sysroot_build_path(cls, build_root, host_target):
        """
        Returns the path to the sysroot build directory, which contains only the
        artifacts of wasi-libc (Not including the artifacts of LLVM runtimes).
        """
        return os.path.join(build_root,
                            '%s-%s' % (cls.product_name(), host_target), 'sysroot')

    @classmethod
    def sysroot_install_path(cls, build_root):
        """
        Returns the path to the sysroot install directory, which contains artifacts
        of wasi-libc and LLVM runtimes.
        """
        return os.path.join(build_root, 'wasi-sysroot')


class WasmLLVMRuntimeLibs(cmake_product.CMakeProduct):
    @classmethod
    def product_source_name(cls):
        return os.path.join("llvm-project", "runtimes")

    @classmethod
    def is_build_script_impl_product(cls):
        return False

    @classmethod
    def is_before_build_script_impl_product(cls):
        return False

    def should_build(self, host_target):
        # LLVM runtime libraries should always be built for WebAssembly
        # if standard library is being built for WebAssembly.
        return self.args.build_wasmstdlib

    def should_test(self, host_target):
        return False

    def should_install(self, host_target):
        return False

    def build(self, host_target):
        self._build(host_target)

    def _build(self, host_target, enable_wasi_threads=False):
        build_root = os.path.dirname(self.build_dir)
        llvm_build_bin_dir = os.path.join(
            '..', build_root, '%s-%s' % ('llvm', host_target), 'bin')
        llvm_tools_path = self.args.native_llvm_tools_path or llvm_build_bin_dir
        clang_tools_path = self.args.native_clang_tools_path or llvm_build_bin_dir

        cmake_has_threads = 'TRUE' if enable_wasi_threads else 'FALSE'

        self.cmake_options.define('CMAKE_SYSROOT:PATH',
                                  WASILibc.sysroot_build_path(build_root, host_target))
        self.cmake_options.define('LLVM_ENABLE_RUNTIMES:STRING',
                                  'libcxx;libcxxabi;compiler-rt')
        libdir_suffix = '/wasm32-wasi'
        if enable_wasi_threads:
            libdir_suffix = '/wasm32-wasi-threads'
        self.cmake_options.define('LIBCXX_LIBDIR_SUFFIX:STRING', libdir_suffix)
        self.cmake_options.define('LIBCXXABI_LIBDIR_SUFFIX:STRING', libdir_suffix)
        self.cmake_options.define('CMAKE_STAGING_PREFIX:PATH', '/')

        self.cmake_options.define('COMPILER_RT_DEFAULT_TARGET_ARCH:STRING', 'wasm32')
        self.cmake_options.define('COMPILER_RT_DEFAULT_TARGET_ONLY:BOOL', 'TRUE')
        self.cmake_options.define('COMPILER_RT_BAREMETAL_BUILD:BOOL', 'TRUE')
        self.cmake_options.define('COMPILER_RT_BUILD_XRAY:BOOL', 'FALSE')
        self.cmake_options.define('COMPILER_RT_INCLUDE_TESTS:BOOL', 'FALSE')
        self.cmake_options.define('COMPILER_RT_HAS_FPIC_FLAG:BOOL', 'FALSE')
        self.cmake_options.define('COMPILER_RT_EXCLUDE_ATOMIC_BUILTIN:BOOL', 'FALSE')
        self.cmake_options.define('COMPILER_RT_OS_DIR:STRING', 'wasi')

        self.cmake_options.define('CMAKE_C_COMPILER_WORKS:BOOL', 'TRUE')
        self.cmake_options.define('CMAKE_CXX_COMPILER_WORKS:BOOL', 'TRUE')

        self.cmake_options.define('CMAKE_SYSTEM_NAME:STRING', 'WASI')
        self.cmake_options.define('CMAKE_SYSTEM_PROCESSOR:STRING', 'wasm32')
        self.cmake_options.define('CMAKE_AR:FILEPATH',
                                  os.path.join(llvm_tools_path, 'llvm-ar'))
        self.cmake_options.define('CMAKE_RANLIB:FILEPATH',
                                  os.path.join(llvm_tools_path, 'llvm-ranlib'))
        self.cmake_options.define('CMAKE_C_COMPILER:FILEPATH',
                                  os.path.join(clang_tools_path, 'clang'))
        self.cmake_options.define('CMAKE_CXX_COMPILER:STRING',
                                  os.path.join(clang_tools_path, 'clang++'))

        c_flags = []
        # Explicitly disable exceptions even though it's usually implicitly disabled by
        # LIBCXX_ENABLE_EXCEPTIONS because the CMake feature check fails to detect
        # -fno-exceptions support in clang due to missing compiler-rt while configuring
        # as mono project.
        cxx_flags = ['-fno-exceptions']
        if enable_wasi_threads:
            c_flags.append('-pthread')
            cxx_flags.append('-pthread')
        self.cmake_options.define('CMAKE_C_FLAGS:STRING', ' '.join(c_flags))
        self.cmake_options.define('CMAKE_CXX_FLAGS:STRING', ' '.join(cxx_flags))

        target_triple = 'wasm32-wasi-threads' if enable_wasi_threads else 'wasm32-wasi'
        self.cmake_options.define('CMAKE_C_COMPILER_TARGET:STRING', target_triple)
        self.cmake_options.define('CMAKE_CXX_COMPILER_TARGET:STRING', target_triple)

        self.cmake_options.define('CXX_SUPPORTS_CXX11:BOOL', 'TRUE')

        self.cmake_options.define('LIBCXX_ENABLE_THREADS:BOOL', cmake_has_threads)
        self.cmake_options.define('LIBCXX_HAS_PTHREAD_API:BOOL', cmake_has_threads)
        self.cmake_options.define('LIBCXX_HAS_EXTERNAL_THREAD_API:BOOL', 'FALSE')
        self.cmake_options.define('LIBCXX_BUILD_EXTERNAL_THREAD_LIBRARY:BOOL', 'FALSE')
        self.cmake_options.define('LIBCXX_HAS_WIN32_THREAD_API:BOOL', 'FALSE')
        self.cmake_options.define('LIBCXX_ENABLE_SHARED:BOOL', 'FALSE')
        self.cmake_options.define('LIBCXX_ENABLE_EXPERIMENTAL_LIBRARY:BOOL', 'FALSE')
        self.cmake_options.define('LIBCXX_ENABLE_EXCEPTIONS:BOOL', 'FALSE')
        self.cmake_options.define('LIBCXX_ENABLE_FILESYSTEM:BOOL', 'FALSE')
        self.cmake_options.define('LIBCXX_CXX_ABI', 'libcxxabi')
        self.cmake_options.define('LIBCXX_HAS_MUSL_LIBC:BOOL', 'TRUE')

        self.cmake_options.define('LIBCXX_ABI_VERSION', '2')
        self.cmake_options.define('LIBCXXABI_ENABLE_EXCEPTIONS:BOOL', 'FALSE')
        self.cmake_options.define('LIBCXXABI_ENABLE_SHARED:BOOL', 'FALSE')
        self.cmake_options.define('LIBCXXABI_SILENT_TERMINATE:BOOL', 'TRUE')
        self.cmake_options.define('LIBCXXABI_ENABLE_THREADS:BOOL', cmake_has_threads)
        self.cmake_options.define('LIBCXXABI_HAS_PTHREAD_API:BOOL', cmake_has_threads)
        self.cmake_options.define('LIBCXXABI_HAS_EXTERNAL_THREAD_API:BOOL', 'FALSE')
        self.cmake_options.define('LIBCXXABI_BUILD_EXTERNAL_THREAD_LIBRARY:BOOL',
                                  'FALSE')
        self.cmake_options.define('LIBCXXABI_HAS_WIN32_THREAD_API:BOOL', 'FALSE')
        self.cmake_options.define('LIBCXXABI_ENABLE_PIC:BOOL', 'FALSE')
        self.cmake_options.define('UNIX:BOOL', 'TRUE')

        self.build_with_cmake([], self.args.build_variant, [],
                              prefer_native_toolchain=True)
        self.install_with_cmake(
            ["install"], WASILibc.sysroot_install_path(build_root))

    @classmethod
    def get_dependencies(cls):
        return [WASILibc, llvm.LLVM]

class WasmThreadsLLVMRuntimeLibs(WasmLLVMRuntimeLibs):
    def build(self, host_target):
        self._build(host_target, enable_wasi_threads=True)
