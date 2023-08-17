# ===-- build_script_invocation.py ---------------------------------------===#
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https:#swift.org/LICENSE.txt for license information
# See https:#swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
# ===---------------------------------------------------------------------===#

import os
import platform
import shlex

from build_swift.build_swift import argparse
from build_swift.build_swift.constants import BUILD_SCRIPT_IMPL_PATH
from build_swift.build_swift.constants import SWIFT_BUILD_ROOT
from build_swift.build_swift.constants import SWIFT_REPO_NAME
from build_swift.build_swift.constants import SWIFT_SOURCE_ROOT

from swift_build_support.swift_build_support import products
from swift_build_support.swift_build_support import shell
from swift_build_support.swift_build_support import targets
from swift_build_support.swift_build_support import workspace
from swift_build_support.swift_build_support.cmake import CMake
from swift_build_support.swift_build_support.host_specific_configuration \
    import HostSpecificConfiguration
from swift_build_support.swift_build_support.productpipeline_list_builder \
    import ProductPipelineListBuilder
from swift_build_support.swift_build_support.targets \
    import StdlibDeploymentTarget
from swift_build_support.swift_build_support.utils import clear_log_time
from swift_build_support.swift_build_support.utils \
    import exit_rejecting_arguments
from swift_build_support.swift_build_support.utils import fatal_error
from swift_build_support.swift_build_support.utils import log_time_in_scope


class BuildScriptInvocation(object):
    """Represent a single build script invocation.
    """

    def __init__(self, toolchain, args):
        self.toolchain = toolchain
        self.args = args

        self.workspace = workspace.Workspace(
            source_root=SWIFT_SOURCE_ROOT,
            build_root=os.path.join(SWIFT_BUILD_ROOT, args.build_subdir))

        clear_log_time()

    @property
    def install_all(self):
        return self.args.install_all or self.args.infer_dependencies

    def build_ninja(self):
        if not os.path.exists(self.workspace.source_dir("ninja")):
            fatal_error(
                "can't find source directory for ninja "
                "(tried %s)" % (self.workspace.source_dir("ninja")))

        ninja_build = products.Ninja.new_builder(
            args=self.args,
            toolchain=self.toolchain,
            workspace=self.workspace,
            host=StdlibDeploymentTarget.get_target_for_name(
                self.args.host_target))
        ninja_build.build()
        self.toolchain.ninja = ninja_build.ninja_bin_path

    def convert_to_impl_arguments(self):
        """convert_to_impl_arguments() -> (env, args)

        Convert the invocation to an environment and list of arguments suitable
        for invoking `build-script-impl`.
        """

        # Create local shadows, for convenience.
        args = self.args
        toolchain = self.toolchain

        cmake = CMake(args=args,
                      toolchain=self.toolchain)

        impl_args = [
            "--workspace", self.workspace.source_root,
            "--build-dir", self.workspace.build_root,
            "--install-prefix", args.install_prefix,
            "--host-target", args.host_target,
            "--stdlib-deployment-targets={}".format(
                " ".join(args.stdlib_deployment_targets)),
            "--host-cc", toolchain.cc,
            "--host-cxx", toolchain.cxx,
            "--darwin-xcrun-toolchain", args.darwin_xcrun_toolchain,
            "--darwin-deployment-version-osx=%s" % (
                args.darwin_deployment_version_osx),
            "--darwin-deployment-version-ios=%s" % (
                args.darwin_deployment_version_ios),
            "--darwin-deployment-version-tvos=%s" % (
                args.darwin_deployment_version_tvos),
            "--darwin-deployment-version-watchos=%s" % (
                args.darwin_deployment_version_watchos),
            "--cmake", toolchain.cmake,
            "--llvm-build-type", args.llvm_build_variant,
            "--swift-build-type", args.swift_build_variant,
            "--swift-stdlib-build-type", args.swift_stdlib_build_variant,
            "--lldb-build-type", args.lldb_build_variant,
            "--foundation-build-type", args.foundation_build_variant,
            "--libdispatch-build-type", args.libdispatch_build_variant,
            "--libicu-build-type", args.libicu_build_variant,
            "--xctest-build-type", args.build_variant,
            "--llbuild-build-type", args.build_variant,
            "--swift-enable-assertions", str(args.swift_assertions).lower(),
            "--swift-stdlib-enable-assertions", str(
                args.swift_stdlib_assertions).lower(),
            "--swift-analyze-code-coverage", str(
                args.swift_analyze_code_coverage).lower(),
            "--llbuild-enable-assertions", str(
                args.llbuild_assertions).lower(),
            "--lldb-assertions", str(
                args.lldb_assertions).lower(),
            "--cmake-generator", args.cmake_generator,
            "--cross-compile-append-host-target-to-destdir", str(
                args.cross_compile_append_host_target_to_destdir).lower(),
            "--build-jobs", str(args.build_jobs),
            "--lit-jobs", str(args.lit_jobs),
            "--common-cmake-options=%s" % ' '.join(
                shlex.quote(opt) for opt in cmake.common_options()),
            "--build-args=%s" % ' '.join(
                shlex.quote(arg) for arg in cmake.build_args()),
            "--dsymutil-jobs", str(args.dsymutil_jobs),
            '--build-swift-libexec', str(args.build_swift_libexec).lower(),
            '--swift-enable-backtracing', str(args.swift_enable_backtracing).lower(),
            '--build-swift-remote-mirror', str(args.build_swift_remote_mirror).lower(),
        ]

        # Compute any product specific cmake arguments.
        #
        # NOTE: The sum(list(...)) is b/c compute_product_pipelines returns a
        # tuple of lists of which the first is the build-script-impl products
        # and the second is the non-build-script-impl-products. It guarantees
        # that when we concatenate these two lists together we get a valid
        # dependency graph.
        for product_class in sum(list(self.compute_product_pipelines()[0]), []):
            if not product_class.is_build_script_impl_product():
                continue

            product_name = product_class.product_name()
            product_source_name = product_class.product_source_name()
            source_dir = self.workspace.source_dir(product_source_name)

            if not os.path.exists(source_dir):
                fatal_error(
                    "can't find source directory for %s "
                    "(tried %s)" % (product_name, source_dir))

            product = product_class(
                args=args,
                toolchain=self.toolchain,
                source_dir=source_dir,
                # FIXME: This is incorrect since it always assumes the host
                # target I think?
                build_dir=self.workspace.build_dir(
                    args.host_target, product_name))
            cmake_opts = product.cmake_options

            # FIXME: We should be using shlex.quote here but we run into issues
            # with build-script-impl/cmake not being happy with all of the
            # extra "'" in the strings. To fix this easily, we really need to
            # just invoke cmake from build-script directly rather than futzing
            # with build-script-impl. This makes even more sense since there
            # really isn't a security issue here.
            if cmake_opts:
                impl_args += [
                    "--{}-cmake-options={}".format(
                        product_name, ' '.join(cmake_opts))
                ]

        if args.build_toolchain_only:
            impl_args += [
                "--build-toolchain-only=1"
            ]

        if args.build_stdlib_deployment_targets:
            impl_args += [
                "--build-stdlib-deployment-targets", " ".join(
                    args.build_stdlib_deployment_targets)]
        if args.cross_compile_hosts:
            impl_args += [
                "--cross-compile-hosts", " ".join(args.cross_compile_hosts)]
        if args.cross_compile_deps_path is not None:
            impl_args += [
                "--cross-compile-deps-path=%s" % args.cross_compile_deps_path
            ]

        if args.test_paths:
            impl_args += ["--test-paths", " ".join(args.test_paths)]

        if toolchain.ninja:
            impl_args += ["--ninja-bin=%s" % toolchain.ninja]
        if args.distcc:
            impl_args += [
                "--distcc",
                "--distcc-pump=%s" % toolchain.distcc_pump
            ]
        if args.sccache:
            args.cmake_c_launcher = toolchain.sccache
            args.cmake_cxx_launcher = toolchain.sccache

        # *NOTE* We use normal cmake to pass through tsan/ubsan options. We do
        # NOT pass them to build-script-impl.
        if args.enable_asan:
            impl_args += ["--enable-asan"]
            # If we are on linux, disable leak detection when running ASAN. We
            # have a separate bot that checks for leaks.
            if platform.system() == 'Linux':
                os.environ['ASAN_OPTIONS'] = 'detect_leaks=0'
        if args.enable_ubsan:
            impl_args += ["--enable-ubsan"]

        # If we have lsan, we need to export our suppression list. The actual
        # passing in of the LSAN flag is done via the normal cmake method. We
        # do not pass the flag to build-script-impl.
        if args.enable_lsan:
            supp_file = os.path.join(SWIFT_SOURCE_ROOT, SWIFT_REPO_NAME,
                                     "utils",
                                     "lsan_leaks_suppression_list.txt")
            os.environ['LSAN_OPTIONS'] = 'suppressions={}'.format(supp_file)
        if args.verbose_build:
            impl_args += ["--verbose-build"]
        if args.install_symroot:
            impl_args += [
                "--install-symroot", os.path.abspath(args.install_symroot)
            ]
        if args.install_destdir:
            impl_args += [
                "--install-destdir", os.path.abspath(args.install_destdir)
            ]

        if args.skip_build:
            impl_args += ["--skip-build"]
        if not args.build_benchmarks:
            impl_args += ["--skip-build-benchmarks"]

        if args.swift_disable_dead_stripping:
            args.extra_cmake_options.append('-DSWIFT_DISABLE_DEAD_STRIPPING:BOOL=TRUE')
        if args.build_backdeployconcurrency:
            args.extra_cmake_options.append(
                '-DSWIFT_BACK_DEPLOY_CONCURRENCY:BOOL=TRUE')

        swift_syntax_src = os.path.join(self.workspace.source_root,
                                        "swift-syntax")
        args.extra_cmake_options.append(
            '-DSWIFT_PATH_TO_SWIFT_SYNTAX_SOURCE:PATH={}'.format(swift_syntax_src))

        if args.build_early_swiftsyntax:
            impl_args += ["--swift-earlyswiftsyntax"]

        # Then add subproject install flags that either skip building them /or/
        # if we are going to build them and install_all is set, we also install
        # them.
        conditional_subproject_configs = [
            (args.build_llvm, "llvm"),
            (args.build_swift, "swift"),
            (args.build_foundation, "foundation"),
            (args.build_xctest, "xctest"),
            (args.build_lldb, "lldb"),
            (args.build_llbuild, "llbuild"),
            (args.build_libcxx, "libcxx"),
            (args.build_libdispatch, "libdispatch"),
            (args.build_libicu, "libicu"),
            (args.build_libxml2, 'libxml2'),
            (args.build_zlib, 'zlib'),
            (args.build_curl, 'curl')
        ]
        for (should_build, string_name) in conditional_subproject_configs:
            if not should_build and not self.args.infer_dependencies:
                impl_args += ["--skip-build-{}".format(string_name)]
            elif self.install_all:
                impl_args += ["--install-{}".format(string_name)]

        if args.build_swift_dynamic_stdlib:
            impl_args += ["--build-swift-dynamic-stdlib"]
        if args.build_swift_static_stdlib:
            impl_args += ["--build-swift-static-stdlib"]
        if args.build_swift_stdlib_unittest_extra:
            impl_args += ["--build-swift-stdlib-unittest-extra"]
        if args.build_swift_dynamic_sdk_overlay:
            impl_args += ["--build-swift-dynamic-sdk-overlay"]
        if args.build_swift_static_sdk_overlay:
            impl_args += ["--build-swift-static-sdk-overlay"]

        if not args.build_android:
            impl_args += ["--skip-build-android"]
        if not args.build_clang_tools_extra:
            impl_args += ["--skip-build-clang-tools-extra"]

        if not args.test and not args.long_test and not args.stress_test:
            impl_args += ["--skip-test-swift"]
        if not args.test:
            impl_args += [
                "--skip-test-lldb",
                "--skip-test-llbuild",
                "--skip-test-xctest",
                "--skip-test-foundation",
                "--skip-test-libdispatch",
                "--skip-test-libicu",
            ]
        if args.build_runtime_with_host_compiler:
            impl_args += ["--build-runtime-with-host-compiler"]
        if args.validation_test:
            impl_args += ["--validation-test"]
        if args.long_test:
            impl_args += ["--long-test"]
        if args.stress_test:
            impl_args += ["--stress-test"]
        if args.skip_local_build:
            impl_args += ["--skip-local-build"]
        if args.only_executable_test:
            impl_args += ["--only-executable-test"]
        if not args.benchmark:
            impl_args += ["--skip-test-benchmarks"]
        if args.android:
            impl_args += [
                "--android-arch", args.android_arch,
                "--android-ndk", args.android_ndk,
                "--android-api-level", args.android_api_level,
            ]
        # If building natively on an Android host, only pass the API level.
        if StdlibDeploymentTarget.Android.contains(StdlibDeploymentTarget
                                                   .host_target().name):
            impl_args += ["--android-api-level", args.android_api_level]
        if args.android_deploy_device_path:
            impl_args += [
                "--android-deploy-device-path",
                args.android_deploy_device_path,
            ]

        if platform.system() == 'Darwin':
            impl_args += [
                "--toolchain-prefix",
                targets.darwin_toolchain_prefix(
                    args.install_prefix),
                "--host-lipo", toolchain.lipo,
            ]

            # Isolate build from the system; Darwin toolchains build against SDKs.
            # For additional isolation, disable pkg-config. Homebrew's pkg-config
            # prioritizes CommandLineTools paths, resulting in compile errors.
            args.extra_cmake_options += [
                '-DCMAKE_IGNORE_PATH=/usr/lib;/usr/local/lib;/lib',
                '-DPKG_CONFIG_EXECUTABLE=/usr/bin/false',
            ]

        if toolchain.libtool is not None:
            impl_args += [
                "--host-libtool", toolchain.libtool,
            ]
        if args.native_clang_tools_path is not None:
            impl_args += [
                "--native-clang-tools-path=%s" % args.native_clang_tools_path
            ]
        if args.native_llvm_tools_path is not None:
            impl_args += [
                "--native-llvm-tools-path=%s" % args.native_llvm_tools_path
            ]
        if args.native_swift_tools_path is not None:
            impl_args += [
                "--native-swift-tools-path=%s" % args.native_swift_tools_path
            ]

        # If we have extra_swift_args, combine all of them together and then
        # add them as one command.
        if args.extra_swift_args:
            impl_args += [
                "--extra-swift-args=%s" % ';'.join(args.extra_swift_args)
            ]

        # Enable macCatalyst
        if args.maccatalyst:
            (args.extra_cmake_options
             .append('-DSWIFT_ENABLE_MACCATALYST:BOOL=TRUE'))
        if args.maccatalyst_ios_tests:
            impl_args += ["--darwin-test-maccatalyst-ios-like=1"]

        # Provide a fixed backtracer path, if required
        if args.swift_runtime_fixed_backtracer_path is not None:
            impl_args += [
                '--swift-runtime-fixed-backtracer-path=%s' %
                args.swift_runtime_fixed_backtracer_path
            ]

        # If we have extra_cmake_options, combine all of them together and then
        # add them as one command.
        if args.extra_cmake_options:
            impl_args += [
                "--extra-cmake-options=%s" % ' '.join(
                    shlex.quote(opt) for opt in args.extra_cmake_options)
            ]

        if args.lto_type is not None:
            impl_args += [
                "--llvm-enable-lto=%s" % args.lto_type,
                "--swift-tools-enable-lto=%s" % args.lto_type
            ]
            if args.llvm_max_parallel_lto_link_jobs is not None:
                impl_args += [
                    "--llvm-num-parallel-lto-link-jobs=%s" %
                    min(args.llvm_max_parallel_lto_link_jobs, args.build_jobs)
                ]
            if args.swift_tools_max_parallel_lto_link_jobs is not None:
                impl_args += [
                    "--swift-tools-num-parallel-lto-link-jobs=%s" %
                    min(args.swift_tools_max_parallel_lto_link_jobs,
                        args.build_jobs)
                ]

        if args.bootstrapping_mode is not None:
            impl_args += [
                "--bootstrapping=%s" % args.bootstrapping_mode,
            ]

        impl_args += args.build_script_impl_args

        if args.dry_run:
            impl_args += ["--dry-run"]

        if args.reconfigure:
            impl_args += ["--reconfigure"]

        if args.clang_profile_instr_use:
            impl_args += [
                "--clang-profile-instr-use=%s" %
                os.path.abspath(args.clang_profile_instr_use)
            ]

        if args.lit_args:
            impl_args += ["--llvm-lit-args=%s" % args.lit_args]

        if args.coverage_db:
            impl_args += [
                "--coverage-db=%s" %
                os.path.abspath(args.coverage_db)
            ]

        if args.llvm_install_components:
            impl_args += [
                "--llvm-install-components=%s" % args.llvm_install_components
            ]

        # On non-Darwin platforms, build lld so we can always have a
        # linker that is compatible with the swift we are using to
        # compile the stdlib.
        #
        # This makes it easier to build target stdlibs on systems that
        # have old toolchains without more modern linker features.
        #
        # On Darwin, only build lld if explicitly requested using --build-lld.
        should_build_lld = (platform.system() != 'Darwin' or args.build_lld)
        if not should_build_lld:
            impl_args += [
                "--skip-build-lld"
            ]

        if not args.clean_libdispatch:
            impl_args += [
                "--skip-clean-libdispatch"
            ]

        if not args.clean_foundation:
            impl_args += [
                "--skip-clean-foundation"
            ]

        if not args.clean_xctest:
            impl_args += [
                "--skip-clean-xctest"
            ]

        if not args.clean_llbuild:
            impl_args += [
                "--skip-clean-llbuild"
            ]

        if args.llvm_ninja_targets:
            impl_args += [
                "--llvm-ninja-targets=%s" % ' '.join(args.llvm_ninja_targets)
            ]

        if args.llvm_ninja_targets_for_cross_compile_hosts:
            impl_args += [
                "--llvm-ninja-targets-for-cross-compile-hosts=%s" %
                ' '.join(args.llvm_ninja_targets_for_cross_compile_hosts)
            ]

        if args.darwin_symroot_path_filters:
            impl_args += [
                "--darwin_symroot_path_filters=%s" %
                ' '.join(args.darwin_symroot_path_filters)
            ]

        # Compute the set of host-specific variables, which we pass through to
        # the build script via environment variables.
        host_specific_variables = self.compute_host_specific_variables()
        impl_env = {}
        for (host_target, options) in host_specific_variables.items():
            for (name, value) in options.items():
                # We mangle into an environment variable we can easily evaluate
                # from the `build-script-impl`.
                impl_env["HOST_VARIABLE_{}__{}".format(
                    host_target.replace("-", "_"), name)] = value

        return (impl_env, impl_args)

    def compute_host_specific_variables(self):
        """compute_host_specific_variables(args) -> dict

        Compute the host-specific options, organized as a dictionary keyed by
        host of options.
        """

        args = self.args
        args.build_root = self.workspace.build_root

        options = {}
        for host_target in [args.host_target] + args.cross_compile_hosts:
            # Compute the host specific configuration.
            try:
                config = HostSpecificConfiguration(host_target, args)
            except argparse.ArgumentError as e:
                exit_rejecting_arguments(str(e))

            # Convert into `build-script-impl` style variables.
            options[host_target] = {
                "SWIFT_SDKS": " ".join(sorted(
                    config.sdks_to_configure)),
                "SWIFT_STDLIB_TARGETS": " ".join(
                    config.swift_stdlib_build_targets),
                "SWIFT_LIBEXEC_TARGETS": " ".join(
                    config.swift_libexec_build_targets),
                "SWIFT_BENCHMARK_TARGETS": " ".join(
                    config.swift_benchmark_build_targets),
                "SWIFT_RUN_BENCHMARK_TARGETS": " ".join(
                    config.swift_benchmark_run_targets),
                "SWIFT_TEST_TARGETS": " ".join(
                    config.swift_test_run_targets),
                "SWIFT_FLAGS": config.swift_flags,
                "SWIFT_TARGET_CMAKE_OPTIONS": " ".join(config.cmake_options),
            }

        return options

    def compute_product_pipelines(self):
        """compute_product_pipelines() -> [[Product]]

        A list of lists of products.

        Compute lists of product pipelines that we should run. It is guaranteed
        that all product pipeline lists consist of solely build-script-impl
        products or build-script products. So one can always check the first
        element to know if a pipeline returned from the builder is an impl
        product or not.
        """
        builder = ProductPipelineListBuilder(self.args)

        builder.begin_pipeline()

        builder.add_product(products.EarlySwiftSyntax,
                            is_enabled=self.args.build_early_swiftsyntax)

        # If --skip-early-swift-driver is passed in, swift will be built
        # as usual, but relying on its own C++-based (Legacy) driver.
        # Otherwise, we build an "early" swift-driver using the host
        # toolchain, which the later-built compiler will forward
        # `swiftc` invocations to. That is, if we find a Swift compiler
        # in the host toolchain. If the host toolchain is not equipped with
        # a Swift compiler, a warning is emitted. In the future, it may become
        # mandatory that the host toolchain come with its own `swiftc`.
        builder.add_product(products.EarlySwiftDriver,
                            is_enabled=self.args.build_early_swift_driver)

        builder.add_product(products.CMark,
                            is_enabled=self.args.build_cmark)

        # If --skip-build-llvm is passed in, LLVM cannot be completely disabled, as
        # Swift still needs a few LLVM targets like tblgen to be built for it to be
        # configured. Instead, handle this in the product for now.
        builder.add_product(products.LLVM,
                            is_enabled=True)

        builder.add_product(products.LibXML2,
                            is_enabled=self.args.build_libxml2)

        builder.add_product(products.zlib.Zlib,
                            is_enabled=self.args.build_zlib)

        builder.add_product(products.curl.LibCurl,
                            is_enabled=self.args.build_curl)

        # Begin a build-script-impl pipeline for handling the compiler toolchain
        # and a subset of the tools that we build. We build these in this manner
        # to preserve current build-script-impl run behavior as we transition
        # the build-script code base. The main difference is that these are all
        # build, tested, and installed all at once instead of performing build,
        # test, install like a normal build-script product.
        builder.begin_impl_pipeline(should_run_epilogue_operations=False)

        builder.add_impl_product(products.LibCXX,
                                 is_enabled=self.args.build_libcxx)
        builder.add_impl_product(products.LibICU,
                                 is_enabled=self.args.build_libicu)
        builder.add_impl_product(products.Swift,
                                 is_enabled=self.args.build_swift)
        builder.add_impl_product(products.LLDB,
                                 is_enabled=self.args.build_lldb)

        # Begin a new build-script-impl pipeline that builds libraries that we
        # build as part of build-script-impl but that we should eventually move
        # onto build-script products.
        builder.begin_impl_pipeline(should_run_epilogue_operations=True)
        builder.add_impl_product(products.LibDispatch,
                                 is_enabled=self.args.build_libdispatch)
        builder.add_impl_product(products.Foundation,
                                 is_enabled=self.args.build_foundation)
        builder.add_impl_product(products.XCTest,
                                 is_enabled=self.args.build_xctest)
        builder.add_impl_product(products.LLBuild,
                                 is_enabled=self.args.build_llbuild)

        # Begin the post build-script-impl build phase.
        builder.begin_pipeline()

        builder.add_product(products.BackDeployConcurrency,
                            is_enabled=self.args.build_backdeployconcurrency)
        builder.add_product(products.SwiftPM,
                            is_enabled=self.args.build_swiftpm)
        builder.add_product(products.SwiftSyntax,
                            is_enabled=self.args.build_swiftsyntax)
        builder.add_product(products.SKStressTester,
                            is_enabled=self.args.build_skstresstester)
        builder.add_product(products.SwiftFormat,
                            is_enabled=self.args.build_swiftformat)
        builder.add_product(products.SwiftEvolve,
                            is_enabled=self.args.build_swiftevolve)
        builder.add_product(products.IndexStoreDB,
                            is_enabled=self.args.build_indexstoredb)
        builder.add_product(products.PlaygroundSupport,
                            is_enabled=self.args.build_playgroundsupport)
        builder.add_product(products.SourceKitLSP,
                            is_enabled=self.args.build_sourcekitlsp)
        builder.add_product(products.Benchmarks,
                            is_enabled=self.args.build_toolchainbenchmarks)
        builder.add_product(products.SwiftInspect,
                            is_enabled=self.args.build_swift_inspect)
        builder.add_product(products.TSanLibDispatch,
                            is_enabled=self.args.tsan_libdispatch_test)
        builder.add_product(products.SwiftDocC,
                            is_enabled=self.args.build_swiftdocc)
        builder.add_product(products.SwiftDocCRender,
                            is_enabled=self.args.install_swiftdocc)
        builder.add_product(products.MinimalStdlib,
                            is_enabled=self.args.build_minimalstdlib)

        # Keep SwiftDriver at last.
        # swift-driver's integration with the build scripts is not fully
        # supported. Using swift-driver to build these products may hit
        # failures.
        builder.add_product(products.SwiftDriver,
                            is_enabled=self.args.build_swift_driver
                            or self.args.install_swift_driver)

        # Now that we have constructed our pass pipelines using our builder, get
        # the final schedule and finalize the builder.
        return builder.finalize(shouldInfer=self.args.infer_dependencies)

    def execute(self):
        """Execute the invocation with the configured arguments."""

        # Convert to a build-script-impl invocation.
        (self.impl_env, self.impl_args) = self.convert_to_impl_arguments()

        # If using the legacy implementation, delegate all behavior to
        # `build-script-impl`.
        if self.args.legacy_impl:
            # Execute the underlying build script implementation.
            shell.call_without_sleeping([BUILD_SCRIPT_IMPL_PATH] + self.impl_args,
                                        env=self.impl_env, echo=True)
            return

        # Otherwise, we compute and execute the individual actions ourselves.
        # Compute the list of hosts to operate on.
        all_host_names = [
            self.args.host_target] + self.args.cross_compile_hosts
        all_hosts = [StdlibDeploymentTarget.get_target_for_name(name)
                     for name in all_host_names]

        # Compute the list of lists of product classes to operate on.
        #
        # FIXME: This should really be per-host, but the current structure
        # matches that of `build-script-impl`.
        (product_pipelines, last_impl_index) = self.compute_product_pipelines()

        # Execute each "product pipeline".
        for index in range(len(product_pipelines)):
            perform_epilogue_opts = last_impl_index == index
            pipeline = product_pipelines[index]

            # Skip empty pipelines.
            if len(pipeline) == 0:
                if perform_epilogue_opts:
                    self._execute_merged_host_lipo_core_action()
                continue

            is_impl = pipeline[0].is_build_script_impl_product()
            if is_impl:
                self._execute_impl(pipeline, all_hosts, perform_epilogue_opts)
            else:
                assert index != last_impl_index
                if index > last_impl_index:
                    non_darwin_cross_compile_hostnames = [
                        target for target in self.args.cross_compile_hosts if not
                        StdlibDeploymentTarget.get_target_for_name(
                            target).platform.is_darwin
                    ]
                    self._execute(pipeline, [self.args.host_target] +
                                  non_darwin_cross_compile_hostnames)
                else:
                    self._execute(pipeline, all_host_names)

        # And then perform the rest of the non-core epilogue actions.

        # Extract symbols...
        for host_target in all_hosts:
            self._execute_extract_symbols_action(host_target)

        # Package...
        for host_target in all_hosts:
            self._execute_package_action(host_target)

        # Lipo...
        self._execute_merged_host_lipo_action()

    def _execute_impl(self, pipeline, all_hosts, should_run_epilogue_operations):
        # Build...
        for host_target in all_hosts:
            # FIXME: We should only compute these once.
            try:
                config = HostSpecificConfiguration(host_target.name, self.args)
            except argparse.ArgumentError as e:
                exit_rejecting_arguments(str(e))
            print("Building the standard library for: {}".format(
                " ".join(config.swift_stdlib_build_targets)))
            if config.swift_test_run_targets and (
                    self.args.test or self.args.long_test):
                print("Running Swift tests for: {}".format(
                    " ".join(config.swift_test_run_targets)))
            if config.swift_benchmark_run_targets and self.args.benchmark:
                print("Running Swift benchmarks for: {}".format(
                    " ".join(config.swift_benchmark_run_targets)))

            for product_class in pipeline:
                self._execute_build_action(host_target, product_class)

        # Test...
        for host_target in all_hosts:
            for product_class in pipeline:
                self._execute_test_action(host_target, product_class)

        # Install...
        for host_target in all_hosts:
            for product_class in pipeline:
                self._execute_install_action(host_target, product_class)

        # And then we may be asked to perform several post-processing operations
        # on what we have built. If we are not supposed to do so, bail now.
        if not should_run_epilogue_operations:
            return

        # Core Lipo...
        self._execute_merged_host_lipo_core_action()

    def _execute(self, pipeline, all_host_names):
        for host_target in all_host_names:
            if self.args.skip_local_build and host_target == self.args.host_target:
                continue
            for product_class in pipeline:
                # Execute clean, build, test, install
                self.execute_product_build_steps(product_class, host_target)

    def _execute_build_action(self, host_target, product_class):
        action_name = "{}-{}-build".format(host_target.name,
                                           product_class.product_name())
        self._execute_action(action_name)

    def _execute_test_action(self, host_target, product_class):
        action_name = "{}-{}-test".format(host_target.name,
                                          product_class.product_name())
        self._execute_action(action_name)

    def _execute_install_action(self, host_target, product_class):
        action_name = "{}-{}-install".format(host_target.name,
                                             product_class.product_name())
        self._execute_action(action_name)

    def _execute_extract_symbols_action(self, host_target):
        action_name = "{}-extractsymbols".format(host_target.name)
        self._execute_action(action_name)

    def _execute_package_action(self, host_target):
        action_name = "{}-package".format(host_target.name)
        self._execute_action(action_name)

    def _execute_merged_host_lipo_action(self):
        self._execute_action("merged-hosts-lipo")

    def _execute_merged_host_lipo_core_action(self):
        self._execute_action("merged-hosts-lipo-core")

    def _execute_action(self, action_name):
        with log_time_in_scope(action_name):
            shell.call_without_sleeping(
                [BUILD_SCRIPT_IMPL_PATH] + self.impl_args +
                ["--only-execute", action_name],
                env=self.impl_env, echo=self.args.verbose_build)

    def execute_product_build_steps(self, product_class, host_target):
        product_source = product_class.product_source_name()
        product_name = product_class.product_name()
        if product_class.is_swiftpm_unified_build_product():
            build_dir = self.workspace.swiftpm_unified_build_dir(
                host_target)
        else:
            build_dir = self.workspace.build_dir(
                host_target, product_name)
        product = product_class(
            args=self.args,
            toolchain=self.toolchain,
            source_dir=self.workspace.source_dir(product_source),
            build_dir=build_dir)
        if product.should_clean(host_target):
            log_message = "Cleaning %s" % product_name
            print("--- {} ---".format(log_message))
            with log_time_in_scope(log_message):
                product.clean(host_target)
        if product.should_build(host_target):
            log_message = "Building %s" % product_name
            print("--- {} ---".format(log_message))
            with log_time_in_scope(log_message):
                product.build(host_target)
        if product.should_test(host_target):
            log_message = "Running tests for %s" % product_name
            print("--- {} ---".format(log_message))
            with log_time_in_scope(log_message):
                product.test(host_target)
            print("--- Finished tests for %s ---" % product_name)
        # Install the product if it should be installed specifically, or
        # if it should be built and `install_all` is set to True.
        # The exception is select before_build_script_impl products
        # which set `is_ignore_install_all_product` to True, ensuring
        # they are never installed. (e.g. earlySwiftDriver).
        if product.should_install(host_target) or \
           (self.install_all and product.should_build(host_target) and
           not product.is_ignore_install_all_product()):
            log_message = "Installing %s" % product_name
            print("--- {} ---".format(log_message))
            with log_time_in_scope(log_message):
                product.install(host_target)
