# swift_build_support/products/product.py -----------------------*- python -*-
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2021 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
# ----------------------------------------------------------------------------

import os

from build_swift.build_swift.wrappers import xcrun

from . import product
from .. import cmake
from .. import shell


class CMakeProduct(product.Product):
    def is_verbose(self):
        return self.args.verbose_build

    def build_with_cmake(self, build_targets, build_type, build_args,
                         prefer_just_built_toolchain=False):
        assert self.toolchain.cmake is not None
        cmake_build = []
        _cmake = cmake.CMake(self.args, self.toolchain,
                             prefer_just_built_toolchain)

        if self.toolchain.distcc_pump:
            cmake_build.append(self.toolchain.distcc_pump)
        cmake_build.extend([self.toolchain.cmake, "--build"])

        # If we are verbose...
        if self.is_verbose():
            # And ninja, add a -v.
            if self.args.cmake_generator == "Ninja":
                build_args.append('-v')

        generator_output_path = ""
        if self.args.cmake_generator == "Ninja":
            generator_output_path = os.path.join(self.build_dir, "build.ninja")

        cmake_cache_path = os.path.join(self.build_dir, "CMakeCache.txt")
        if self.args.reconfigure or not os.path.isfile(cmake_cache_path) or \
                (generator_output_path and not os.path.isfile(generator_output_path)):
            if not os.path.exists(self.build_dir):
                os.makedirs(self.build_dir)

            # Use `cmake-file-api` in case it is available.
            query_dir = os.path.join(self.build_dir, ".cmake", "api", "v1", "query")
            if not os.path.exists(query_dir):
                os.makedirs(query_dir)
            open(os.path.join(query_dir, "codemodel-v2"), 'a').close()
            open(os.path.join(query_dir, "cache-v2"), 'a').close()

            env = None
            if self.toolchain.distcc:
                env = {
                    "DISTCC_HOSTS": "localhost,lzo,cpp"
                }

            with shell.pushd(self.build_dir):
                shell.call(["env"] + [self.toolchain.cmake]
                           + list(_cmake.common_options(self))
                           + list(self.cmake_options)
                           + self.args.extra_cmake_options + [self.source_dir],
                           env=env)

        is_llvm = self.product_name() == "llvm"
        if (not is_llvm and not self.args.skip_build) or (
            is_llvm and self.args._build_llvm
        ):
            cmake_opts = [self.build_dir, "--config", build_type]

            if self.args.cmake_generator == "Xcode":
                # CMake automatically adds "--target ALL_BUILD" if we don't
                # pass this.
                cmake_opts += ["--target", "ZERO_CHECK"]

                # Xcode generator uses "ALL_BUILD" instead of "all".
                # Also, xcodebuild uses -target instead of bare names.
                build_targets = build_targets[:]
                build_targets = [val for target in build_targets
                                 for val in ["-target",
                                             target if target != "all"
                                             else "ALL_BUILD"]]

                # Xcode can't restart itself if it turns out we need to reconfigure.
                # Do an advance build to handle that.
                shell.call(["env"] + cmake_build + cmake_opts)

            shell.call(
                ["env"] + cmake_build + cmake_opts + ["--"] + build_args
                        + _cmake.build_args() + build_targets
            )

    def test_with_cmake(self, executable_target, results_targets,
                        build_type, build_args, test_env=None):
        assert self.toolchain.cmake is not None
        cmake_build = []
        _cmake = cmake.CMake(self.args, self.toolchain)

        if self.toolchain.distcc_pump:
            cmake_build.append(self.toolchain.distcc_pump)

        # If we are verbose...
        if self.is_verbose():
            # And ninja, add a -v.
            if self.args.cmake_generator == "Ninja":
                build_args.append('-v')

        cmake_args = [self.toolchain.cmake, "--build", self.build_dir,
                      "--config", build_type, "--"]
        cmake_build.extend(cmake_args + build_args + _cmake.build_args())

        def target_flag(target):
            if self.args.cmake_generator == "Xcode":
                return ["-target", target]
            return [target]

        if executable_target:
            shell.call(cmake_build + target_flag(executable_target))

        for target in results_targets:
            if target:
                test_target = target
                print("--- %s ---" % target)
                if test_target.startswith("check-swift") and self.args.test_paths:
                    test_target = test_target + "-custom"

                # note that passing variables via test_env won't affect lit tests -
                # lit.cfg will filter environment variables out!
                shell.call(cmake_build + target_flag(test_target), env=test_env)

                print("--- %s finished ---" % target)

    def install_with_cmake(self, install_targets, install_destdir):
        assert self.toolchain.cmake is not None
        cmake_build = []

        if self.toolchain.distcc_pump:
            cmake_build.append(self.toolchain.distcc_pump)
        cmake_args = [self.toolchain.cmake, "--build", self.build_dir, "--"]
        cmake_build.extend(cmake_args + install_targets)

        environment = {'DESTDIR': install_destdir}
        shell.call(cmake_build, env=environment)

    def host_cmake_options(self, host_target):
        (platform, arch) = host_target.split('-')

        llvm_target_arch = None
        cmake_osx_deployment_target = None
        swift_host_triple = None
        swift_host_variant = platform
        swift_host_variant_sdk = platform.upper()
        swift_host_variant_arch = arch

        llvm_cmake_options = cmake.CMakeOptions()
        swift_cmake_options = cmake.CMakeOptions()

        if host_target.startswith("android"):
            # Clang uses a different sysroot natively on Android in the Termux
            # app, which the Termux build scripts pass in through a $PREFIX
            # variable.
            prefix = os.environ.get("PREFIX")
            if prefix:
                llvm_cmake_options.define('DEFAULT_SYSROOT:STRING',
                                          os.path.dirname(prefix))
                llvm_cmake_options.define('CLANG_DEFAULT_LINKER:STRING', 'lld')

            # Android doesn't support building all of compiler-rt yet.
            if not self.is_cross_compile_target(host_target):
                llvm_cmake_options.define('COMPILER_RT_INCLUDE_TESTS:BOOL', 'FALSE')

            if host_target == 'android-aarch64':
                llvm_target_arch = 'AArch64'
                swift_host_triple = 'aarch64-unknown-linux-android{}'.format(
                    self.args.android_api_level)
            elif host_target == "android-armv7":
                llvm_target_arch = "ARM"
                swift_host_triple = 'armv7-unknown-linux-androideabi{}'.format(
                    self.args.android_api_level)
            elif host_target == "android-x86_64":
                llvm_target_arch = "X86"
                swift_host_triple = 'x86_64-unknown-linux-android{}'.format(
                    self.args.android_api_level)

        elif host_target == 'linux-armv6':
            swift_host_triple = 'armv6-unknown-linux-gnueabihf'
            llvm_target_arch = 'ARM'

        elif host_target == 'linux-armv7':
            swift_host_triple = 'armv7-unknown-linux-gnueabihf'
            llvm_target_arch = 'ARM'

        elif host_target.startswith('linux-static'):

            if host_target == 'linux-static-aarch64':
                swift_host_triple = 'aarch64-swift-linux-musl'
                llvm_target_arch = 'AArch64'
            elif host_target == 'linux-static-x86_64':
                swift_host_triple = 'x86_64-swift-linux-musl'
                llvm_target_arch = 'X86'

        elif host_target.startswith('macosx') or \
                host_target.startswith('iphone') or \
                host_target.startswith('appletv') or \
                host_target.startswith('watch'):

            swift_cmake_options.define('Python3_EXECUTABLE',
                                       self.toolchain.find_tool('python3'))

            if host_target == 'macosx-x86_64':
                swift_host_triple = 'x86_64-apple-macosx{}'.format(
                    self.args.darwin_deployment_version_osx)
                llvm_target_arch = None
                swift_host_variant_sdk = 'OSX'
                cmake_osx_deployment_target = self.args.darwin_deployment_version_osx

            elif host_target == 'macosx-arm64':
                # xcrun_sdk_name = 'macosx'
                llvm_target_arch = 'AArch64'
                swift_host_triple = 'arm64-apple-macosx{}'.format(
                    self.args.darwin_deployment_version_osx)
                swift_host_variant = 'macosx'
                swift_host_variant_sdk = 'OSX'
                swift_host_variant_arch = 'arm64'
                cmake_osx_deployment_target = self.args.darwin_deployment_version_osx

            elif host_target == 'macosx-arm64e':
                # xcrun_sdk_name = 'macosx'
                llvm_target_arch = 'AArch64'
                swift_host_triple = 'arm64e-apple-macosx{}'.format(
                    self.args.darwin_deployment_version_osx)
                swift_host_variant = 'macosx'
                swift_host_variant_sdk = 'OSX'
                swift_host_variant_arch = 'arm64e'
                cmake_osx_deployment_target = self.args.darwin_deployment_version_osx

            elif host_target == 'iphonesimulator-x86_64':
                swift_host_triple = 'x86_64-apple-ios{}-simulator'.format(
                    self.args.darwin_deployment_version_ios)
                llvm_target_arch = 'X86'
                swift_host_variant_sdk = 'IOS_SIMULATOR'
                cmake_osx_deployment_target = None

            elif host_target == 'iphonesimulator-arm64':
                swift_host_triple = 'arm64-apple-ios{}-simulator'.format(
                    self.args.darwin_deployment_version_ios)
                llvm_target_arch = 'AArch64'
                swift_host_variant = 'iphonesimulator'
                swift_host_variant_sdk = 'IOS_SIMULATOR'
                swift_host_variant_arch = 'arm64'
                cmake_osx_deployment_target = None

            elif host_target == 'iphoneos-armv7s':
                swift_host_triple = 'armv7s-apple-ios{}'.format(
                    self.args.darwin_deployment_version_ios)
                llvm_target_arch = 'ARM'
                swift_host_variant_sdk = 'IOS'
                cmake_osx_deployment_target = None

            elif host_target == 'iphoneos-arm64':
                swift_host_triple = 'arm64-apple-ios{}'.format(
                    self.args.darwin_deployment_version_ios)
                llvm_target_arch = 'AArch64'
                swift_host_variant_sdk = 'IOS'
                cmake_osx_deployment_target = None

            elif host_target == 'iphoneos-arm64e':
                swift_host_triple = 'arm64e-apple-ios{}'.format(
                    self.args.darwin_deployment_version_ios)
                llvm_target_arch = 'AArch64'
                swift_host_variant_sdk = 'IOS'
                cmake_osx_deployment_target = None

            elif host_target == 'appletvsimulator-x86_64':
                swift_host_triple = 'x86_64-apple-tvos{}-simulator'.format(
                    self.args.darwin_deployment_version_tvos)
                llvm_target_arch = 'X86'
                swift_host_variant_sdk = 'IOS'
                cmake_osx_deployment_target = None

            elif host_target == 'appletvsimulator-arm64':
                swift_host_triple = 'arm64-apple-tvos{}-simulator'.format(
                    self.args.darwin_deployment_version_tvos)
                swift_host_variant = 'appletvsimulator'
                swift_host_variant_sdk = 'TVOS_SIMULATOR'
                swift_host_variant_arch = 'arm64'
                cmake_osx_deployment_target = None

            elif host_target == 'appletvos-arm64':
                swift_host_triple = 'arm64-apple-tvos{}'.format(
                    self.args.darwin_deployment_version_tvos)
                llvm_target_arch = 'AArch64'
                swift_host_variant_sdk = 'TVOS'
                cmake_osx_deployment_target = None

            elif host_target == 'watchsimulator-i386':
                swift_host_triple = 'i386-apple-watchos{}-simulator'.format(
                    self.args.darwin_deployment_version_watchos)
                llvm_target_arch = 'X86'
                swift_host_variant_sdk = 'WATCHOS_SIMULATOR'
                cmake_osx_deployment_target = None

            elif host_target == 'watchsimulator-x86_64':
                swift_host_triple = 'x86_64-apple-watchos{}-simulator'.format(
                    self.args.darwin_deployment_version_watchos)
                llvm_target_arch = 'X86'
                swift_host_variant_sdk = 'WATCHOS_SIMULATOR'
                cmake_osx_deployment_target = None

            elif host_target == 'watchsimulator-arm64':
                swift_host_triple = 'arm64-apple-watchos{}-simulator'.format(
                    self.args.darwin_deployment_version_watchos)
                llvm_target_arch = 'AArch64'
                swift_host_variant = 'watchsimulator'
                swift_host_variant_sdk = 'WATCHOS_SIMULATOR'
                swift_host_variant_arch = 'arm64'
                cmake_osx_deployment_target = None

            elif host_target == 'watchos-armv7k':
                swift_host_triple = 'armv7k-apple-watchos{}'.format(
                    self.args.darwin_deployment_version_watchos)
                llvm_target_arch = 'ARM'
                swift_host_variant_sdk = 'WATCHOS'
                cmake_osx_deployment_target = None

            elif host_target == 'watchos-arm64_32':
                swift_host_triple = 'arm64_32-apple-watchos{}'.format(
                    self.args.darwin_deployment_version_watchos)
                llvm_target_arch = 'AArch64'
                swift_host_variant_sdk = 'WATCHOS'
                cmake_osx_deployment_target = None

            darwin_sdk_deployment_targets = os.environ.get(
                'DARWIN_SDK_DEPLOYMENT_TARGETS')
            if darwin_sdk_deployment_targets:
                targets = darwin_sdk_deployment_targets.split(';')
                for target in targets:
                    (target_platform, target_arch, target_version) = target.split('-')
                    sdk_target = '{}_{}'.format(target_platform.upper(),
                                                target_arch.upper())
                    option_name = 'SWIFTLIB_DEPLOYMENT_VERSION_{}'.format(sdk_target)
                    swift_cmake_options.define(option_name, target_version)

            cmake_os_sysroot = xcrun.sdk_path(sdk=platform)

            llvm_cmake_options.define('CMAKE_OSX_DEPLOYMENT_TARGET:STRING',
                                      cmake_osx_deployment_target)
            llvm_cmake_options.define('CMAKE_OSX_SYSROOT:PATH', cmake_os_sysroot)
            llvm_cmake_options.define('COMPILER_RT_ENABLE_IOS:BOOL', 'FALSE')
            llvm_cmake_options.define('COMPILER_RT_ENABLE_WATCHOS:BOOL', 'FALSE')
            llvm_cmake_options.define('COMPILER_RT_ENABLE_TVOS:BOOL', 'FALSE')
            llvm_cmake_options.define('SANITIZER_MIN_OSX_VERSION',
                                      cmake_osx_deployment_target)
            llvm_cmake_options.define('LLVM_ENABLE_MODULES:BOOL',
                                      cmake.CMakeOptions.true_false(
                                          self.args.llvm_enable_modules))
            llvm_cmake_options.define('CMAKE_OSX_ARCHITECTURES', arch)

            # in build-script, if lto_type is defined, it will be set for llvm and
            # swift, so we don't have to have separate cases here
            if self.args.lto_type:
                llvm_cmake_options.define('LLVM_PARALLEL_LINK_JOBS',
                                          self.args.llvm_max_parallel_lto_link_jobs)
                swift_cmake_options.define('SWIFT_PARALLEL_LINK_JOBS', self.args
                                           .swift_tools_max_parallel_lto_link_jobs)
                llvm_cmake_options.define('LLVM_ENABLE_MODULE_DEBUGGING:BOOL', 'NO')
                swift_cmake_options.define('LLVM_ENABLE_MODULE_DEBUGGING:BOOL', 'NO')

            swift_cmake_options.define('SWIFT_DARWIN_DEPLOYMENT_VERSION_OSX',
                                       self.args.darwin_deployment_version_osx)
            swift_cmake_options.define('SWIFT_DARWIN_DEPLOYMENT_VERSION_IOS',
                                       self.args.darwin_deployment_version_ios)
            swift_cmake_options.define('SWIFT_DARWIN_DEPLOYMENT_VERSION_TVOS',
                                       self.args.darwin_deployment_version_tvos)
            swift_cmake_options.define('SWIFT_DARWIN_DEPLOYMENT_VERSION_WATCHOS',
                                       self.args.darwin_deployment_version_watchos)
            swift_cmake_options.define('CMAKE_OSX_SYSROOT:PATH', cmake_os_sysroot)
            # This is needed to make sure to avoid using the wrong architecture
            # in the compiler checks CMake performs
            swift_cmake_options.define('CMAKE_OSX_ARCHITECTURES', arch)

        # We don't currently support building compiler-rt for cross-compile targets.
        # It's not clear that's useful anyway.
        if self.is_cross_compile_target(host_target):
            llvm_cmake_options.define('LLVM_TOOL_COMPILER_RT_BUILD:BOOL', 'FALSE')
            llvm_cmake_options.define('LLVM_BUILD_EXTERNAL_COMPILER_RT:BOOL', 'FALSE')
        else:
            llvm_cmake_options.define('LLVM_TOOL_COMPILER_RT_BUILD:BOOL',
                                      cmake.CMakeOptions.true_false(
                                          self.args.build_compiler_rt))
            llvm_cmake_options.define('LLVM_BUILD_EXTERNAL_COMPILER_RT:BOOL',
                                      cmake.CMakeOptions.true_false(
                                          self.args.build_compiler_rt))

        # If we are asked to not generate test targets for LLVM and or Swift,
        # disable as many LLVM tools as we can. This improves compile time when
        # compiling with LTO.
        #
        # *NOTE* Currently we do not support testing LLVM via build-script. But in a
        # future commit we will.
        if llvm_target_arch:
            llvm_cmake_options.define('LLVM_TARGET_ARCH', llvm_target_arch)

        # For cross-compilable hosts, we need to know the triple
        # and it must be the same for both LLVM and Swift
        if swift_host_triple:
            llvm_cmake_options.define('LLVM_HOST_TRIPLE:STRING', swift_host_triple)
            swift_cmake_options.define('SWIFT_HOST_TRIPLE:STRING', swift_host_triple)

        swift_cmake_options.define('SWIFT_HOST_VARIANT', swift_host_variant)
        swift_cmake_options.define('SWIFT_HOST_VARIANT_SDK', swift_host_variant_sdk)
        swift_cmake_options.define('SWIFT_HOST_VARIANT_ARCH', swift_host_variant_arch)

        llvm_cmake_options.define('LLVM_LIT_ARGS', '{} -j {}'.format(
            self.args.lit_args, self.args.lit_jobs))
        swift_cmake_options.define('LLVM_LIT_ARGS', '{} -j {}'.format(
            self.args.lit_args, self.args.lit_jobs))

        if self.args.clang_profile_instr_use:
            llvm_cmake_options.define('LLVM_PROFDATA_FILE',
                                      self.args.clang_profile_instr_use)

        if self.args.swift_profile_instr_use:
            swift_cmake_options.define('SWIFT_PROFDATA_FILE',
                                       self.args.swift_profile_instr_use)

        llvm_cmake_options.define('COVERAGE_DB', self.args.coverage_db)

        return (llvm_cmake_options, swift_cmake_options)
