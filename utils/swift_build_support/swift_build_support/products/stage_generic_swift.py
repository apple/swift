# ----- stage_generic_swift.py -------------------------------------------===#
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https:#swift.org/LICENSE.txt for license information
# See https:#swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
# ------------------------------------------------------------------------===#

"""This file contains an abstracted implementation of a build-script product for
Swift that can be used to create the stage1 and stage2 builds. It abstracts over
the options that need to vary in between the stage1/stage2 build. At the bottom
of the file, we provide for two different sub-classes of StageGenericSwift that force th
"""

import argparse
import os

from build_swift.build_swift.wrappers import xcrun

from . import cmake_product
from . import cmark
from . import earlyswiftdriver
from . import llvm
from . import swift
from .. import cmake
from .. import compiler_stage
from ..host_specific_configuration import \
    HostSpecificConfiguration


class StageGenericSwift(cmake_product.CMakeProduct):
    STAGE_1 = compiler_stage.STAGE_1
    STAGE_2 = compiler_stage.STAGE_2
    STAGE_2_STDLIB = compiler_stage.STAGE_2_STDLIB
    ALL_STAGES = [
        STAGE_1,
        STAGE_2,
        STAGE_2_STDLIB,
        ]

    def __init__(self, stage, args, toolchain, source_dir, build_dir):
        assert(stage in StageGenericSwift.ALL_STAGES)
        # source_dir is always ../swift.
        source_dir = os.path.abspath(os.path.join(os.path.dirname(source_dir),
                                                  'swift'))
        # Normal Args without stage specific info.
        self.stage = stage
        self.stage_specific_args = compiler_stage.StageArgs(self.stage, args)
        # self.args is set by our super class init.
        args = compiler_stage.StageArgs(compiler_stage.STAGE_1, args)
        # Can vary in between having a _stdlib_stage2 suffix or not.
        cmake_product.CMakeProduct.__init__(self, args, toolchain, source_dir,
                                            build_dir)

    @classmethod
    def is_stage1(cls):
        raise RuntimeError("Abstract method not implemented?!")

    @classmethod
    def is_stdlib_stage2(cls):
        raise RuntimeError("Abstract method not implemented?!")

    @property
    def _runtime_sanitizer_flags(self):
        sanitizer_list = []
        if self.args.enable_tsan_runtime:
            sanitizer_list += ['Thread']
        if len(sanitizer_list) == 0:
            return []
        return [('SWIFT_RUNTIME_USE_SANITIZERS', ';'.join(sanitizer_list))]

    @property
    def _compiler_vendor_flags(self):
        vendor = self.args.compiler_vendor
        if vendor == "none":
            return []

        if vendor != "apple":
            raise RuntimeError("Unknown compiler vendor?! Was build-script \
updated without updating swift.py?")

        swift_compiler_version = ""
        if self.args.swift_compiler_version is not None:
            swift_compiler_version = self.args.swift_compiler_version

        return [
            ('SWIFT_VENDOR', 'Apple'),
            ('SWIFT_VENDOR_UTI', 'com.apple.compilers.llvm.swift'),

            # This has a default of 3.0, so it should be safe to use here.
            ('SWIFT_VERSION', str(self.args.swift_user_visible_version)),

            # FIXME: We are matching build-script-impl here. But it seems like
            # bit rot since this flag is specified in another place with the
            # exact same value in build-script-impl.
            ('SWIFT_COMPILER_VERSION', str(swift_compiler_version)),
        ]

    @property
    def _version_flags(self):
        r = cmake.CMakeOptions()
        if self.args.swift_compiler_version is not None:
            swift_compiler_version = self.args.swift_compiler_version
            r.define('SWIFT_COMPILER_VERSION', str(swift_compiler_version))
        if self.args.clang_compiler_version is not None:
            clang_compiler_version = self.args.clang_compiler_version
            r.define('CLANG_COMPILER_VERSION', str(clang_compiler_version))
        return r

    @property
    def _benchmark_flags(self):
        if not self.args.benchmark:
            return []

        onone_iters = self.args.benchmark_num_onone_iterations
        o_iters = self.args.benchmark_num_o_iterations
        return [
            ('SWIFT_BENCHMARK_NUM_ONONE_ITERATIONS', onone_iters),
            ('SWIFT_BENCHMARK_NUM_O_ITERATIONS', o_iters)
        ]

    @property
    def _compile_db_flags(self):
        return [('CMAKE_EXPORT_COMPILE_COMMANDS', True)]

    @property
    def _force_optimized_typechecker_flags(self):
        return [('SWIFT_FORCE_OPTIMIZED_TYPECHECKER:BOOL',
                 self.args.force_optimized_typechecker)]

    @property
    def _stdlibcore_exclusivity_checking_flags(self):
        return [('SWIFT_STDLIB_ENABLE_STDLIBCORE_EXCLUSIVITY_CHECKING:BOOL',
                 self.args.enable_stdlibcore_exclusivity_checking)]

    @property
    def _enable_experimental_differentiable_programming(self):
        return [('SWIFT_ENABLE_EXPERIMENTAL_DIFFERENTIABLE_PROGRAMMING:BOOL',
                 self.args.enable_experimental_differentiable_programming)]

    @property
    def _enable_experimental_concurrency(self):
        return [('SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY:BOOL',
                 self.args.enable_experimental_concurrency)]

    @property
    def _enable_experimental_distributed(self):
        return [('SWIFT_ENABLE_EXPERIMENTAL_DISTRIBUTED:BOOL',
                 self.args.enable_experimental_distributed)]

    @classmethod
    def is_build_script_impl_product(cls):
        """is_build_script_impl_product -> bool

        Whether this product is produced by build-script-impl.
        """
        return False

    @classmethod
    def is_before_build_script_impl_product(cls):
        """is_before_build_script_impl_product -> bool

        Whether this product is build before any build-script-impl products.
        """
        return False

    # EarlySwiftDriver is the root of the graph, and is the only dependency of
    # this product.
    @classmethod
    def get_dependencies(cls):
        if cls.is_stage1():
            return [llvm.LLVM, cmark.CMark, earlyswiftdriver.EarlySwiftDriver]
        return [swift.Swift]

    def should_build(self, host_target):
        """should_build() -> Bool

        Whether or not this product should be built with the given arguments.
        """
        return self.stage_specific_args.build_swift

    def _add_android_specific_cmake_flags(self, host_target):
        # if [[ "${ANDROID_API_LEVEL}" ]]; then
        #     cmake_options=(
        #         "${cmake_options[@]}"
        #         -DSWIFT_ANDROID_API_LEVEL:STRING="${ANDROID_API_LEVEL}"
        #     )
        # fi

        # if [[ ! "${SKIP_BUILD_ANDROID}" ]] ||
        #    [[ $(is_cross_tools_host ${host}) && "${host}" == "android-"* ]]; then
        #     cmake_options=(
        #         "${cmake_options[@]}"
        #         -DSWIFT_ANDROID_NDK_PATH:STRING="${ANDROID_NDK}"
        #         -DSWIFT_ANDROID_NDK_GCC_VERSION:STRING="${ANDROID_NDK_GCC_VERSION}"
        #         -DSWIFT_ANDROID_${ANDROID_ARCH}_ICU_UC:STRING="${ANDROID_ICU_UC}"
        #         -DSWIFT_ANDROID_${ANDROID_ARCH}_ICU_UC_INCLUDE:STRING="${ANDROID_ICU_UC_INCLUDE}"
        #         -DSWIFT_ANDROID_${ANDROID_ARCH}_ICU_I18N:STRING="${ANDROID_ICU_I18N}"
        #         -DSWIFT_ANDROID_${ANDROID_ARCH}_ICU_I18N_INCLUDE:STRING="${ANDROID_ICU_I18N_INCLUDE}"
        #         -DSWIFT_ANDROID_${ANDROID_ARCH}_ICU_DATA:STRING="${ANDROID_ICU_DATA}"
        #         -DSWIFT_ANDROID_DEPLOY_DEVICE_PATH:STRING="${ANDROID_DEPLOY_DEVICE_PATH}"
        #         -DSWIFT_SDK_ANDROID_ARCHITECTURES:STRING="${ANDROID_ARCH}"
        #     )
        # fi
        pass

    def _add_darwin_specific_cmake_flags(self, host_target):
        """This corresponds to CMAKE_BUILD_SYSTEM"""
        (platform, arch) = host_target.split('-')

        cmake_os_sysroot = xcrun.sdk_path(platform)

        cmake_osx_deployment_target = ''
        if platform == "macosx":
            # TODO: Should this be a multi stage argument?
            cmake_osx_deployment_target = self.args.darwin_deployment_version_osx
        common_c_flags = ' '.join(self.common_cross_c_flags(platform, arch))

        self.cmake_options.define('CMAKE_C_FLAGS', common_c_flags)
        self.cmake_options.define('CMAKE_CXX_FLAGS', common_c_flags)
        self.cmake_options.define('CMAKE_OSX_SYSROOT:PATH', cmake_os_sysroot)
        self.cmake_options.define('CMAKE_OSX_DEPLOYMENT_TARGET',
                                  cmake_osx_deployment_target)
        self.cmake_options.define('CMAKE_OSX_ARCHITECTURES', arch)

        if self.toolchain.lipo is not None:
            self.cmake_options.define('SWIFT_LIPO:PATH', self.toolchain.lipo)
        if self.toolchain.libtool is not None:
            self.cmake_options.define('SWIFT_LIBTOOL', self.toolchain.libtool)
        self.cmake_options.define('CMAKE_IGNORE_PATH', '/usr/lib;/usr/local/lib;/lib')
        self.cmake_options.define('PKG_CONFIG_EXECUTABLE', '/usr/bin/false')

        if host_target == 'macosx-x86_64':
            self.cmake_options.define('SWIFT_HOST_TRIPLE', 'x86_64-apple-macosx10.9')
            self.cmake_options.define('SWIFT_HOST_VARIANT', 'macosx')
            self.cmake_options.define('SWIFT_HOST_VARIANT_SDK', 'OSX')
            self.cmake_options.define('SWIFT_HOST_VARIANT_ARCH', 'x86_64')
        elif host_target == 'macosx-arm64':
            self.cmake_options.define('SWIFT_HOST_TRIPLE', 'arm64-apple-macosx10.9')
            self.cmake_options.define('SWIFT_HOST_VARIANT', 'macosx')
            self.cmake_options.define('SWIFT_HOST_VARIANT_SDK', 'OSX')
            self.cmake_options.define('SWIFT_HOST_VARIANT_ARCH', 'arm64')
        elif host_target == 'macosx-arm64e':
            self.cmake_options.define('SWIFT_HOST_TRIPLE', 'arm64e-apple-macosx10.9')
            self.cmake_options.define('SWIFT_HOST_VARIANT', 'macosx')
            self.cmake_options.define('SWIFT_HOST_VARIANT_SDK', 'OSX')
            self.cmake_options.define('SWIFT_HOST_VARIANT_ARCH', 'arm64e')
        else:
            raise RuntimeError('Unsupported host_target')

        self.cmake_options.define('SWIFT_CHECK_INCREMENTAL_COMPILATION:BOOL', False)
        self.cmake_options.define('SWIFT_DARWIN_DEPLOYMENT_VERSION_IOS', '7.0')
        self.cmake_options.define('SWIFT_DARWIN_DEPLOYMENT_VERSION_OSX', '10.9')
        self.cmake_options.define('SWIFT_DARWIN_DEPLOYMENT_VERSION_TVOS', '9.0')
        self.cmake_options.define('SWIFT_DARWIN_DEPLOYMENT_VERSION_WATCHOS', '2.0')
        self.cmake_options.define('SWIFT_DARWIN_XCRUN_TOOLCHAIN:STRING', 'default')
        self.cmake_options.define('SWIFT_EMBED_BITCODE_SECTION:BOOL', False)
        self.cmake_options.define('SWIFT_ENABLE_COMPATIBILITY_OVERRIDES:BOOL', True)

    def _add_linux_specific_cmake_flags(self, host_target):
        (platform, arch) = host_target.split('-')

        toolchain_file = self.generate_linux_toolchain_file(platform, arch)
        self.cmake_options.define('CMAKE_TOOLCHAIN_FILE:PATH', toolchain_file)

        # Default to ld today.
        self.cmake_options.define('SWIFT_USE_LINKER', 'lld')

    def get_project_build_dir(self, project, host_target):
        build_dir = os.path.join(self.build_dir, '..',
                                 '{}-{}'.format(project, host_target))
        return os.path.realpath(build_dir)

    def stage_specific_build(self, host_target):
        raise RuntimeError("Unimplemented stage specific method?!")

    def build(self, host_target):
        """build() -> void

        Perform the build, for a non-build-script-impl product.
        """
        self.cmake_options.define('CMAKE_BUILD_TYPE:STRING',
                                  self.stage_specific_args.swift_build_variant)

        if host_target.startswith("macosx") or \
           host_target.startswith("iphone") or \
           host_target.startswith("appletv") or \
           host_target.startswith("watch"):
            self._add_darwin_specific_cmake_flags(host_target)
        elif "android" in host_target:
            self._add_android_specific_cmake_flags(host_target)
        elif "linux" in host_target:
            self._add_linux_specific_cmake_flags(host_target)

        # Add platform independent cmake_flags.

        # Add any runtime sanitizer arguments.
        self.cmake_options.extend(self._runtime_sanitizer_flags)

        # Add any compiler vendor cmake flags.
        self.cmake_options.extend(self._compiler_vendor_flags)

        # Add any swift version related cmake flags.
        self.cmake_options.extend(self._version_flags)

        # Add benchmark specific flags.
        self.cmake_options.extend(self._benchmark_flags)

        # Generate the compile db.
        self.cmake_options.extend(self._compile_db_flags)

        # Add the flag if we are supposed to force the typechecker to compile
        # with optimization.
        self.cmake_options.extend(self._force_optimized_typechecker_flags)

        # Add any exclusivity checking flags for stdlibcore.
        self.cmake_options.extend(self._stdlibcore_exclusivity_checking_flags)

        # Add experimental differentiable programming flag.
        self.cmake_options.extend(
            self._enable_experimental_differentiable_programming)

        # Add experimental concurrency flag.
        self.cmake_options.extend(self._enable_experimental_concurrency)

        # Add experimental distributed flag.
        self.cmake_options.extend(self._enable_experimental_distributed)

        # if [[ "${DARWIN_OVERLAY_TARGET}" != "" ]]; then
        #     # Split LOCAL_HOST into a pair ``arch-sdk``
        #     # Example LOCAL_HOST: macosx-x86_64
        #     [[ ${LOCAL_HOST} =~ (.*)-(.*) ]]
        #     overlay_target_closure_cmd="${SWIFT_SOURCE_DIR}/utils/find-overlay-deps-closure.sh ${DARWIN_OVERLAY_TARGET} ${BASH_REMATCH[1]} ${BASH_REMATCH[2]}"
        #     overlay_target_closure=$($overlay_target_closure_cmd)
        #     swift_cmake_options=(
        #         "${swift_cmake_options[@]}"
        #         "-DSWIFT_OVERLAY_TARGETS:STRING=${overlay_target_closure}"
        #     )
        # fi

        # We always assume that we are a cross.

        # native_llvm_tools_path=""
        # native_clang_tools_path=""
        # native_swift_tools_path=""
        # if [[ $(is_cross_tools_host ${host}) ]] ; then

        #     # Don't build benchmarks and tests when building cross compiler.
        #     build_perf_testsuite_this_time=false
        #     build_tests_this_time=false

        #     native_llvm_tools_path="$(build_directory "${LOCAL_HOST}" llvm)/bin"
        #     native_clang_tools_path="$(build_directory "${LOCAL_HOST}" llvm)/bin"
        #     native_swift_tools_path="$(build_directory "${LOCAL_HOST}" swift)/bin"
        # else
        #     # FIXME: Why is the next line not using false_true?
        #     build_perf_testsuite_this_time=$(true_false "$(not ${SKIP_BUILD_BENCHMARKS})")
        #     build_tests_this_time=${SWIFT_INCLUDE_TESTS}
        # fi

        # Always pass in LLVM_TABLEGEN that we just built.
        #        if [[ $(is_cross_tools_host ${host}) ]] ; then
        #            cmake_options=(
        #                "${cmake_options[@]}"
        #                -DLLVM_TABLEGEN=$(build_directory "${LOCAL_HOST}" llvm)/bin/llvm-tblgen
        #            )
        #        fi
        llvm_build_dir = self.get_project_build_dir('llvm', host_target)
        self.cmake_options.define("LLVM_TABLEGEN", os.path.join(llvm_build_dir, 'bin', 'llvm-tblgen'))

        #        # Command-line parameters override any autodetection that we
        #        # might have done.
        #        if [[ "${NATIVE_LLVM_TOOLS_PATH}" ]] ; then
        #            native_llvm_tools_path="${NATIVE_LLVM_TOOLS_PATH}"
        #        fi
        #        if [[ "${NATIVE_CLANG_TOOLS_PATH}" ]] ; then
        #            native_clang_tools_path="${NATIVE_CLANG_TOOLS_PATH}"
        #        fi
        #        if [[ "${NATIVE_SWIFT_TOOLS_PATH}" ]] ; then
        #            native_swift_tools_path="${NATIVE_SWIFT_TOOLS_PATH}"
        #        fi
        if self.args.native_llvm_tools_path is not None:
            self.cmake_options.define('SWIFT_NATIVE_LLVM_TOOLS_PATH:STRING', self.args.native_llvm_tools_path)
        if self.args.native_clang_tools_path is not None:
            self.cmake_options.define('SWIFT_NATIVE_CLANG_TOOLS_PATH:STRING', self.args.native_clang_tools_path)
        if self.args.native_swift_tools_path is not None:
            self.cmake_options.define('SWIFT_NATIVE_SWIFT_TOOLS_PATH:STRING', self.args.native_swift_tools_path)

        #        if [ "${BUILD_LLVM}" == "0" ] ; then
        #            cmake_options=(
        #                "${cmake_options[@]}"
        #                -DLLVM_TOOLS_BINARY_DIR:PATH=/tmp/dummy
        #            )
        #        fi
        if not self.args.build_llvm:
            self.cmake_options.define('LLVM_TOOLS_BINARY_DIR:PATH', os.path.join(os.sep, 'tmp', 'dummy'))

        # Moved into darwin specific options
        #        if [ "${HOST_LIPO}" ] ; then
        #            cmake_options=(
        #                "${cmake_options[@]}"
        #                -DSWIFT_LIPO:PATH="${HOST_LIPO}"
        #            )
        #        fi

        #if not self.args.enable_runtime_function_counters:
        #                              self.cmake_options.define('SWIFT_ENABLE_RUNTIME_FUNCTION_COUNTERS:BOOL',
        #                              self.stage_specific_args.swift_stdlib_assertions)

        # cmake_options=(
        #     "${cmake_options[@]}"
        #     -DCMAKE_C_FLAGS="$(swift_c_flags ${host})"
        #     -DCMAKE_CXX_FLAGS="$(swift_c_flags ${host})"
        #     -DCMAKE_C_FLAGS_RELWITHDEBINFO="-O2 -DNDEBUG"
        #     -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -DNDEBUG"
        #     -DCMAKE_BUILD_TYPE:STRING="${SWIFT_BUILD_TYPE}"
        # // Everything above is moved into a different section
        #
        #     -DLLVM_ENABLE_ASSERTIONS:BOOL=$(true_false "${SWIFT_ENABLE_ASSERTIONS}")
        self.cmake_options.define('LLVM_ENABLE_ASSERTIONS:BOOL',
                                  self.stage_specific_args.swift_assertions)
        #     -DSWIFT_TOOLS_ENABLE_LIBSWIFT:STRING=$(true_false "${LIBSWIFT}")
        self.cmake_options.define('SWIFT_TOOLS_ENABLE_LIBSWIFT:BOOL', False) # For now
        #     -DSWIFT_ANALYZE_CODE_COVERAGE:STRING=$(toupper "${SWIFT_ANALYZE_CODE_COVERAGE}")
        #self.cmake_options.define('SWIFT_ANALYZE_CODE_COVERAGE:BOOL',
        #                          self.stage_specific_args.swift_analyze_code_coverage)
        #     -DSWIFT_STDLIB_BUILD_TYPE:STRING="${SWIFT_STDLIB_BUILD_TYPE}"
        self.cmake_options.define('SWIFT_STDLIB_BUILD_TYPE',
                                  self.stage_specific_args.swift_stdlib_build_variant)
        #     -DSWIFT_STDLIB_ASSERTIONS:BOOL=$(true_false "${SWIFT_STDLIB_ENABLE_ASSERTIONS}")
        self.cmake_options.define('SWIFT_STDLIB_ASSERTIONS:BOOL',
                                  self.stage_specific_args.swift_stdlib_assertions)

        #     -DSWIFT_ENABLE_COMPATIBILITY_OVERRIDES:BOOL=$(true_false "${SWIFT_ENABLE_COMPATIBILITY_OVERRIDES}")
        #     -DSWIFT_STDLIB_SINGLE_THREADED_RUNTIME:BOOL=$(true_false "${SWIFT_STDLIB_SINGLE_THREADED_RUNTIME}")
        #     -DSWIFT_ENABLE_RUNTIME_FUNCTION_COUNTERS:BOOL=$(true_false "${SWIFT_ENABLE_RUNTIME_FUNCTION_COUNTERS}")
        #     -DSWIFT_RUNTIME_MACHO_NO_DYLD:BOOL=$(true_false "${SWIFT_RUNTIME_MACHO_NO_DYLD}")
        #     -DSWIFT_STDLIB_OS_VERSIONING:BOOL=$(true_false "${SWIFT_STDLIB_OS_VERSIONING}")
        #
        # <--- SECTION HANDLED ABOVE
        #     -DSWIFT_NATIVE_LLVM_TOOLS_PATH:STRING="${native_llvm_tools_path}"
        #     -DSWIFT_NATIVE_CLANG_TOOLS_PATH:STRING="${native_clang_tools_path}"
        #     -DSWIFT_NATIVE_SWIFT_TOOLS_PATH:STRING="${native_swift_tools_path}"
        # >--- SECTION HANDLED ABOVE
        #
        #     -DSWIFT_INCLUDE_TOOLS:BOOL=$(true_false "${BUILD_SWIFT_TOOLS}")
        # MG: Touched in stage specific build.
        #self.cmake_options.define('SWIFT_INCLUDE_TOOLS:BOOL', True) # MG: TODO
        #     -DSWIFT_BUILD_REMOTE_MIRROR:BOOL=$(true_false "${BUILD_SWIFT_REMOTE_MIRROR}")
        #self.cmake_options.define('SWIFT_BUILD_REMOTE_MIRROR:BOOL',
        #                          self.stage_specific_args.build_swift_remote_mirror)
        #     -DSWIFT_STDLIB_SIL_DEBUGGING:BOOL=$(true_false "${BUILD_SIL_DEBUGGING_STDLIB}")
        #     -DSWIFT_CHECK_INCREMENTAL_COMPILATION:BOOL=$(true_false "${CHECK_INCREMENTAL_COMPILATION}")
        #     -DSWIFT_REPORT_STATISTICS:BOOL=$(true_false "${REPORT_STATISTICS}")
        #     -DSWIFT_BUILD_DYNAMIC_STDLIB:BOOL=$(true_false "${BUILD_SWIFT_DYNAMIC_STDLIB}")
        self.cmake_options.define('SWIFT_BUILD_DYNAMIC_STDLIB:BOOL',
                                  self.stage_specific_args.build_swift_dynamic_stdlib)
        #     -DSWIFT_BUILD_STATIC_STDLIB:BOOL=$(true_false "${BUILD_SWIFT_STATIC_STDLIB}")
        self.cmake_options.define('SWIFT_BUILD_STATIC_STDLIB:BOOL',
                                  self.stage_specific_args.build_swift_static_stdlib)
        #     -DSWIFT_BUILD_DYNAMIC_SDK_OVERLAY:BOOL=$(true_false "${BUILD_SWIFT_DYNAMIC_SDK_OVERLAY}")
        self.cmake_options.define('SWIFT_BUILD_DYNAMIC_SDK_OVERLAY:BOOL',
                                  self.stage_specific_args.build_swift_dynamic_sdk_overlay)
        #     -DSWIFT_BUILD_STATIC_SDK_OVERLAY:BOOL=$(true_false "${BUILD_SWIFT_STATIC_SDK_OVERLAY}")
        self.cmake_options.define('SWIFT_BUILD_STATIC_SDK_OVERLAY:BOOL',
                                  self.stage_specific_args.build_swift_static_sdk_overlay)
        #     -DSWIFT_BUILD_PERF_TESTSUITE:BOOL=$(true_false "${build_perf_testsuite_this_time}")
        self.cmake_options.define('SWIFT_BUILD_PERF_TESTSUITE:BOOL', False) # MG: TODO
        #     -DSWIFT_BUILD_EXAMPLES:BOOL=$(true_false "${BUILD_SWIFT_EXAMPLES}")
        self.cmake_options.define('SWIFT_BUILD_EXAMPLES:BOOL', False) # MG: TODO
        #     -DSWIFT_INCLUDE_TESTS:BOOL=$(true_false "${build_tests_this_time}")
        self.cmake_options.define('SWIFT_INCLUDE_TESTS:BOOL', True) # MG: TODO
        #     -DSWIFT_EMBED_BITCODE_SECTION:BOOL=$(true_false "${EMBED_BITCODE_SECTION}")
        #     -DSWIFT_TOOLS_ENABLE_LTO:STRING="${SWIFT_TOOLS_ENABLE_LTO}"
        #     -DSWIFT_BUILD_RUNTIME_WITH_HOST_COMPILER:BOOL=$(true_false "${BUILD_RUNTIME_WITH_HOST_COMPILER}")
        #     -DLIBDISPATCH_CMAKE_BUILD_TYPE:STRING="${LIBDISPATCH_BUILD_TYPE}"
        self.cmake_options.define('LIBDISPATCH_CMAKE_BUILD_TYPE:STRING', self.args.libdispatch_build_variant)
        #     "${swift_cmake_options[@]}"
        # )

        # # NOTE: This is not a dead option! It is relied upon for certain
        # # bots/build-configs!
        # #
        # # TODO: In the future when we are always cross compiling and
        # # using Toolchain files, we should put this in either a
        # # toolchain file or a cmake cache.
        # if [[ "${BUILD_TOOLCHAIN_ONLY}" ]]; then
        #     cmake_options+=(
        #     -DSWIFT_TOOL_SIL_OPT_BUILD=FALSE
        #     -DSWIFT_TOOL_SWIFT_IDE_TEST_BUILD=FALSE
        #     -DSWIFT_TOOL_SWIFT_REMOTEAST_TEST_BUILD=FALSE
        #     -DSWIFT_TOOL_LLDB_MODULEIMPORT_TEST_BUILD=FALSE
        #     -DSWIFT_TOOL_SIL_EXTRACT_BUILD=FALSE
        #     -DSWIFT_TOOL_SWIFT_LLVM_OPT_BUILD=FALSE
        #     -DSWIFT_TOOL_SWIFT_SDK_ANALYZER_BUILD=FALSE
        #     -DSWIFT_TOOL_SWIFT_SDK_DIGESTER_BUILD=FALSE
        #     -DSWIFT_TOOL_SOURCEKITD_TEST_BUILD=FALSE
        #     -DSWIFT_TOOL_SOURCEKITD_REPL_BUILD=FALSE
        #     -DSWIFT_TOOL_COMPLETE_TEST_BUILD=FALSE
        #     -DSWIFT_TOOL_SWIFT_REFLECTION_DUMP_BUILD=FALSE
        #     )
        # fi

        # cmake_options=(
        #     "${cmake_options[@]}"
        #     -DCMAKE_INSTALL_PREFIX:PATH="$(get_host_install_prefix ${host})" <--- Handled elsewhere
        #     -DClang_DIR:PATH=${llvm_build_dir}/lib/cmake/clang
        self.cmake_options.define('Clang_DIR', os.path.join(llvm_build_dir, 'lib', 'cmake', 'clang'))
        #     -DLLVM_DIR:PATH=${llvm_build_dir}/lib/cmake/llvm
        self.cmake_options.define('LLVM_DIR', os.path.join(llvm_build_dir, 'lib', 'cmake', 'llvm'))
        #     -DSWIFT_PATH_TO_CMARK_SOURCE:PATH="${CMARK_SOURCE_DIR}"
        self.cmake_options.define('SWIFT_PATH_TO_CMARK_SOURCE',
                           os.path.join(self.source_dir, os.pardir, 'cmark'))
        #     -DSWIFT_PATH_TO_CMARK_BUILD:PATH="$(build_directory ${host} cmark)"
        self.cmake_options.define('SWIFT_PATH_TO_CMARK_BUILD:PATH',
                           self.get_project_build_dir('cmark', host_target))
        #     -DSWIFT_PATH_TO_LIBDISPATCH_SOURCE:PATH="${LIBDISPATCH_SOURCE_DIR}"
        self.cmake_options.define('SWIFT_PATH_TO_LIBDISPATCH_SOURCE',
                           os.path.join(self.source_dir, os.pardir, 'swift-corelibs-libdispatch'))
        #     -DSWIFT_PATH_TO_LIBDISPATCH_BUILD:PATH="$(build_directory ${host} libdispatch)"
        self.cmake_options.define('SWIFT_PATH_TO_LIBDISPATCH_BUILD:PATH',
                           self.get_project_build_dir('libdispatch', host_target))
        #     -DSWIFT_PATH_TO_LIBDISPATCH_STATIC_BUILD:PATH="$(build_directory ${host} libdispatch_static)"
        self.cmake_options.define('SWIFT_PATH_TO_LIBDISPATCH_STATIC_BUILD:PATH',
                           self.get_project_build_dir('libdispatch_static', host_target))
        # )

        # if [[ ! "${SKIP_BUILD_LIBICU}" ]] ; then
        #     LIBICU_BUILD_DIR="$(build_directory ${host} libicu)"
        #     ICU_TMPINSTALL=${LIBICU_BUILD_DIR}/tmp_install
        #     cmake_options=(
        #         "${cmake_options[@]}"
        #         -DSWIFT_PATH_TO_LIBICU_SOURCE:PATH="${LIBICU_SOURCE_DIR}"
        #         -DSWIFT_PATH_TO_LIBICU_BUILD:PATH="${LIBICU_BUILD_DIR}"
        #         -DSWIFT_${SWIFT_HOST_VARIANT_SDK}_${SWIFT_HOST_VARIANT_ARCH}_ICU_UC_INCLUDE:STRING="${ICU_TMPINSTALL}/include"
        #         -DSWIFT_${SWIFT_HOST_VARIANT_SDK}_${SWIFT_HOST_VARIANT_ARCH}_ICU_I18N_INCLUDE:STRING="${ICU_TMPINSTALL}/include"
        #         -DSWIFT_${SWIFT_HOST_VARIANT_SDK}_${SWIFT_HOST_VARIANT_ARCH}_ICU_STATICLIB:BOOL=TRUE
        #     )
        # fi

        # if [[ "${SWIFT_SDKS}" ]] ; then
        #     cmake_options=(
        #         "${cmake_options[@]}"
        #         -DSWIFT_SDKS:STRING="$(join ";" ${SWIFT_SDKS[@]})"
        #     )
        # fi
        # MOVED TO INIT
        try:
            config = HostSpecificConfiguration(host_target, self.args, self.stage_specific_args)
        except argparse.ArgumentError as e:
            exit_rejecting_arguments(six.text_type(e))
        self.cmake_options.define('SWIFT_SDKS:STRING', ';'.join(config.sdks_to_configure))
        self.cmake_options.define('SWIFT_STDLIB_TARGETS', ';'.join(config.swift_stdlib_build_targets))
        self.cmake_options.define('SWIFT_BENCHMARK_TARGETS', ';'.join(config.swift_benchmark_build_targets))
        self.cmake_options.define('SWIFT_RUN_BENCHMARK_TARGETS', ';'.join(config.swift_benchmark_run_targets))
        self.cmake_options.define('SWIFT_TEST_TARGETS', ';'.join(config.swift_test_run_targets))

        # if [[ "${SWIFT_OBJC_INTEROP}" == "0" ]] ; then
        #     cmake_options=(
        #         "${cmake_options[@]}"
        #         -DSWIFT_DISABLE_OBJC_INTEROP:BOOL=True
        #     )
        # fi

        # if [[ "${SWIFT_PRIMARY_VARIANT_SDK}" ]] ; then
        #     cmake_options=(
        #         "${cmake_options[@]}"
        #         -DSWIFT_PRIMARY_VARIANT_SDK:STRING="${SWIFT_PRIMARY_VARIANT_SDK}"
        #     )
        # fi
        if self.stage_specific_args.swift_primary_variant_sdk:
            self.cmake_options.define('SWIFT_PRIMARY_VARIANT_SDK:STRING', self.stage_specific_args.swift_primary_variant_sdk)

        # if [[ "${SWIFT_PRIMARY_VARIANT_ARCH}" ]] ; then
        #     cmake_options=(
        #         "${cmake_options[@]}"
        #         -DSWIFT_PRIMARY_VARIANT_ARCH:STRING="${SWIFT_PRIMARY_VARIANT_ARCH}"
        #     )
        # fi
        if self.stage_specific_args.swift_primary_variant_arch:
            self.cmake_options.define('SWIFT_PRIMARY_VARIANT_ARCH:STRING', self.stage_specific_args.swift_primary_variant_arch)

        # Default is false.
        #
        # if [[ "${SWIFT_STDLIB_STABLE_ABI}" ]] ; then
        #     cmake_options=(
        #         "${cmake_options[@]}"
        #         -DSWIFT_STDLIB_STABLE_ABI:BOOL=$(true_false "${SWIFT_STDLIB_STABLE_ABI}")
        #     )
        # fi

        self.cmake_options.define('SWIFT_INSTALL_COMPONENTS:STRING',
                                  self.stage_specific_args.swift_install_components)

        # if [ "${SWIFT_FREESTANDING_SDK}" ] ; then
        #     cmake_options=(
        #         "${cmake_options[@]}"
        #         -DSWIFT_FREESTANDING_SDK:STRING="${SWIFT_FREESTANDING_SDK}"
        #     )
        # fi

        # if [ "${SWIFT_FREESTANDING_TRIPLE_NAME}" ] ; then
        #     cmake_options=(
        #         "${cmake_options[@]}"
        #         -DSWIFT_FREESTANDING_TRIPLE_NAME:STRING="${SWIFT_FREESTANDING_TRIPLE_NAME}"
        #     )
        # fi

        # if [ "${SWIFT_FREESTANDING_MODULE_NAME}" ] ; then
        #     cmake_options=(
        #         "${cmake_options[@]}"
        #         -DSWIFT_FREESTANDING_MODULE_NAME:STRING="${SWIFT_FREESTANDING_MODULE_NAME}"
        #     )
        # fi

        # if [[ "${SWIFT_FREESTANDING_ARCHS}" ]] ; then
        #     cmake_options=(
        #         "${cmake_options[@]}"
        #         -DSWIFT_FREESTANDING_ARCHS:STRING="$(join ";" ${SWIFT_FREESTANDING_ARCHS[@]})"
        #     )
        # fi

        # if [[ ! "${SKIP_BUILD_LLDB}" ]] ; then
        #     lldb_build_dir=$(build_directory ${host} lldb)
        #     cmake_options=(
        #         "${cmake_options[@]}"
        #         -DLLDB_ENABLE:BOOL=TRUE
        #         -DLLDB_BUILD_DIR:STRING="${lldb_build_dir}"
        #     )
        # fi

        # In stage specific build
        # build_targets=(all "${SWIFT_STDLIB_TARGETS[@]}")
        # if [[ $(true_false "${build_perf_testsuite_this_time}") == "TRUE" ]]; then
        #     native_swift_tools_path="$(build_directory_bin ${LOCAL_HOST} swift)"
        #     cmake_options=(
        #         "${cmake_options[@]}"
        #         -DSWIFT_EXEC:STRING="${native_swift_tools_path}/swiftc"
        #     )
        #     build_targets=("${build_targets[@]}"
        #                    "${SWIFT_BENCHMARK_TARGETS[@]}")
        # fi
        # ;;

        # Now delegate to our subclass to run any part of the stage specific
        # build.
        self.stage_specific_build(host_target)

        # Use tools for the just built toolchain.
        self.build_with_cmake(["all"], self.args.cmark_build_variant, [],
                              prefer_just_built_toolchain = True)

    def should_test(self, host_target):
        """should_test() -> Bool

        Whether or not this product should be tested with the given arguments.
        """
        if self.is_cross_compile_target(host_target):
            return False

        # Only test if we actually specified that we want to build swift stage2.
        result = self.stage_specific_args.build_swift and self.stage_specific_args.test_swift
        return result

    def test(self, host_target):
        """
        Perform the test phase for the product.

        This phase might build and execute the product tests.
        """
        try:
            config = HostSpecificConfiguration(host_target, self.args, self.stage_specific_args)
        except argparse.ArgumentError as e:
            exit_rejecting_arguments(six.text_type(e))

        executable_target = 'SwiftUnitTests'
        results_targets = config.swift_test_run_targets
        if self.args.cmake_generator == 'Xcode':
            # Xcode generator uses "RUN_TESTS" instead of "test".
            raise RuntimeError("Xcode generator not supported?!")
        self.test_with_cmake(executable_target, results_targets,
                             self.args.cmark_build_variant, [])

    def should_install(self, host_target):
        """should_install() -> Bool

        Whether or not this product should be installed with the given
        arguments.
        """
        return self.args.install_all or self.stage_specific_args.install_swift

    def install(self, host_target):
        """
        Perform the install phase for the product.

        This phase might copy the artifacts from the previous phases into a
        destination directory.
        """
        self.install_with_cmake(["install"], self.host_install_destdir(host_target))


class SwiftStage1(StageGenericSwift):

    @classmethod
    def is_stage1(cls):
        return True

    @classmethod
    def is_stdlib_stage2(cls):
        return False

    def __init__(self, args, toolchain, source_dir, build_dir):
        super(SwiftStage1, self).__init__(StageGenericSwift.STAGE_1,
                                          args, toolchain, source_dir,
                                          build_dir)


class SwiftStdlibStage2(StageGenericSwift):

    @classmethod
    def is_stage1(cls):
        return False

    @classmethod
    def is_stdlib_stage2(cls):
        return True

    def __init__(self, args, toolchain, source_dir, build_dir):
        super(SwiftStdlibStage2, self).__init__(StageGenericSwift.STAGE_2_STDLIB,
                                                args, toolchain, source_dir,
                                                build_dir)

    def stage_specific_build(self, host_target):
        # Since we are the stage2 compiler, we build with the host compiler
        # since the host compiler is the stage 1 compiler.
        self.cmake_options.define('SWIFT_BUILD_RUNTIME_WITH_HOST_COMPILER:BOOL', True)


        # For now, we do not support building the syntax parser lib. The test is
        # expecting that we are building the syntax parser lib, but we aren't
        # building any tools, so we can't.
        #
        # TODO: We could try to reference the toolchain internal syntax parser
        # lib, but that would require us to use swift libraries not created by
        # the stage2 build.
        self.cmake_options.define('SWIFT_BUILD_SYNTAXPARSERLIB:BOOL', False)

        # Since we are only building the stdlib, do not include any tools.
        self.cmake_options.define('SWIFT_INCLUDE_TOOLS:BOOL', False)

        # We always use toolchain files and compile/run tests against the host
        # compiler today.
        self.cmake_options.define('SWIFT_RUN_TESTS_WITH_HOST_COMPILER', True)
