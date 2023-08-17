# swift_build_support/host_configuration_support.py -------------*- python -*-
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
# ----------------------------------------------------------------------------

import re
import sys
from argparse import ArgumentError

from . import compiler_stage
from .cmake import CMakeOptions
from .targets import StdlibDeploymentTarget


class HostSpecificConfiguration(object):
    """Configuration information for an individual host."""

    def __init__(self, host_target, args, stage_dependent_args=None):
        """Initialize for the given `host_target`."""
        # If we were not passed a stage_dependent_args object, then we do not need
        # to make a distinction in between them and can just use args.
        if not isinstance(args, compiler_stage.StageArgs):
            args = compiler_stage.StageArgs(compiler_stage.STAGE_1, args)
        if stage_dependent_args is None:
            stage_dependent_args = args

        # Compute the set of deployment targets to configure/build.
        if host_target == stage_dependent_args.host_target:
            # This host is the user's desired product, so honor the requested
            # set of targets to configure/build.
            stdlib_targets_to_configure = stage_dependent_args.stdlib_deployment_targets
            if "all" in stage_dependent_args.build_stdlib_deployment_targets:
                stdlib_targets_to_build = set(stdlib_targets_to_configure)
            else:
                stdlib_targets_to_build = set(
                    stage_dependent_args.build_stdlib_deployment_targets).intersection(
                    set(stage_dependent_args.stdlib_deployment_targets))
        else:
            # Otherwise, this is a host we are building as part of
            # cross-compiling, so we only need the target itself.
            stdlib_targets_to_configure = [host_target]
            if stage_dependent_args.stdlib_deployment_targets:
                # there are some build configs that expect
                # not to be building the stdlib for the target
                # since it will be provided by different means
                stdlib_targets_to_build = set(
                    stdlib_targets_to_configure).intersection(
                    set(stage_dependent_args.stdlib_deployment_targets))
            else:
                stdlib_targets_to_build = set(stdlib_targets_to_configure)

        if hasattr(stage_dependent_args, 'stdlib_deployment_targets') and \
           stage_dependent_args.stdlib_deployment_targets == []:
            stdlib_targets_to_configure = []
            stdlib_targets_to_build = []

        # Compute derived information from the arguments.
        #
        # FIXME: We should move the platform-derived arguments to be entirely
        # data driven, so that we can eliminate this code duplication and just
        # iterate over all supported platforms.
        platforms_to_skip_build = \
            self.__platforms_to_skip_build(args, stage_dependent_args)
        platforms_to_skip_test = \
            self.__platforms_to_skip_test(args, stage_dependent_args)
        platforms_archs_to_skip_test = \
            self.__platforms_archs_to_skip_test(args, stage_dependent_args,
                                                host_target)
        platforms_to_skip_test_host = \
            self.__platforms_to_skip_test_host(args, stage_dependent_args)

        # Compute the lists of **CMake** targets for each use case (configure
        # vs. build vs. run) and the SDKs to configure with.
        self.sdks_to_configure = set()
        self.swift_stdlib_build_targets = []
        self.swift_libexec_build_targets = []
        self.swift_test_run_targets = []
        self.swift_benchmark_build_targets = []
        self.swift_benchmark_run_targets = []
        self.swift_flags = ''
        self.cmake_options = CMakeOptions()
        for deployment_target_name in stdlib_targets_to_configure:
            # Get the target object.
            deployment_target = StdlibDeploymentTarget.get_target_for_name(
                deployment_target_name)
            if deployment_target is None:
                sys.stderr.write('ERROR: unknown target: {}\n'.format(
                    deployment_target_name))
                sys.stderr.flush()
                sys.exit(1)

            # Add the SDK to use.
            deployment_platform = deployment_target.platform
            self.sdks_to_configure.add(deployment_platform.sdk_name)

            # If we aren't actually building this target (only configuring
            # it), do nothing else.
            if deployment_target_name not in stdlib_targets_to_build:
                continue

            # Compute which actions are desired.
            build = (
                deployment_platform not in platforms_to_skip_build)
            test = (
                deployment_platform not in platforms_to_skip_test)
            test_host_only = None
            dt_supports_benchmark = deployment_target.supports_benchmark
            build_benchmarks = build and dt_supports_benchmark
            build_external_benchmarks = all([build, dt_supports_benchmark,
                                             args.build_external_benchmarks])
            build_libexec = build and args.build_swift_libexec

            # FIXME: Note, `build-script-impl` computed a property here
            # w.r.t. testing, but it was actually unused.

            # For platforms which normally require a connected device to
            # test, the default behavior is to run tests that only require
            # the host (i.e., they do not attempt to execute).
            if deployment_platform.uses_host_tests and \
                    deployment_platform not in \
                    platforms_to_skip_test_host:
                test_host_only = True

            name = deployment_target.name

            for skip_test_arch in platforms_archs_to_skip_test:
                if deployment_target.name == skip_test_arch.name:
                    test = False

            if build:
                # Validation, long, and stress tests require building the full
                # standard library, whereas the other targets can build a
                # slightly smaller subset which is faster to build.
                #
                # NOTE: We currently do not separate testing options for
                # stage1/stage2 compiler. This can change with time.
                if stage_dependent_args.build_swift_stdlib_unittest_extra or \
                        args.validation_test or args.long_test or \
                        args.stress_test:
                    self.swift_stdlib_build_targets.append(
                        "swift-stdlib-" + name)
                else:
                    self.swift_stdlib_build_targets.append(
                        "swift-test-stdlib-" + name)
            if build_libexec:
                self.swift_libexec_build_targets.append(
                    'swift-libexec-' + name)
            if build_benchmarks:
                self.swift_benchmark_build_targets.append(
                    "swift-benchmark-" + name)
                if args.benchmark:
                    self.swift_benchmark_run_targets.append(
                        "check-swift-benchmark-" + name)

            if build_external_benchmarks:
                # Add support for the external benchmarks.
                self.swift_benchmark_build_targets.append(
                    "swift-benchmark-{}-external".format(name))
                if args.benchmark:
                    self.swift_benchmark_run_targets.append(
                        "check-swift-benchmark-{}-external".format(name))
            if test:
                if test_host_only:
                    suffix = "-only_non_executable"
                elif args.only_executable_test:
                    suffix = "-only_executable"
                elif args.only_non_executable_test:
                    suffix = "-only_non_executable"
                else:
                    suffix = ""
                subset_suffix = ""
                if args.validation_test and args.long_test and \
                        args.stress_test:
                    subset_suffix = "-all"
                elif args.validation_test:
                    subset_suffix = "-validation"
                elif args.long_test:
                    subset_suffix = "-only_long"
                elif args.stress_test:
                    subset_suffix = "-only_stress"
                else:
                    subset_suffix = ""

                # If the compiler is being tested after being built to use the
                # standalone swift-driver, we build a test-target to
                # run a reduced set of lit-tests that verify the early swift-driver.
                if args.test_early_swift_driver and\
                   not test_host_only:
                    self.swift_test_run_targets.append(
                        "check-swift-only_early_swiftdriver-{}".format(name))

                # Support for running the macCatalyst tests with
                # the iOS-like target triple.
                macosx_platform_match = re.search("macosx-(.*)", name)
                if macosx_platform_match and args.maccatalyst \
                   and args.maccatalyst_ios_tests:
                    (self.swift_test_run_targets
                     .append("check-swift{}{}-{}-{}".format(
                         subset_suffix, suffix, "macosx-maccatalyst",
                         macosx_platform_match.group(1))))
                else:
                    (self.swift_test_run_targets
                     .append("check-swift{}{}-{}".format(
                         subset_suffix, suffix, name)))

                if args.test_optimized and not test_host_only:
                    self.swift_test_run_targets.append(
                        "check-swift{}-optimize-{}".format(
                            subset_suffix, name))
                if args.test_optimize_for_size and not test_host_only:
                    self.swift_test_run_targets.append(
                        "check-swift{}-optimize_size-{}".format(
                            subset_suffix, name))
                if args.test_optimize_none_with_implicit_dynamic and \
                        not test_host_only:
                    self.swift_test_run_targets.append(
                        "check-swift{}-optimize_none_with_implicit_dynamic-{}"
                        .format(subset_suffix, name))

            # Only pull in these flags when cross-compiling with
            # --cross-compile-hosts.
            if deployment_target_name != args.host_target and \
               host_target != args.host_target:
                self.add_flags_for_cross_compilation(args, deployment_target)

    def add_flags_for_cross_compilation(self, args, deployment_target):
        self.swift_flags = deployment_target.platform.swift_flags(args)
        self.cmake_options = deployment_target.platform.cmake_options(args)

    def __platforms_to_skip_build(self, args, stage_dependent_args):
        platforms_to_skip_build = set()
        if not stage_dependent_args.build_linux:
            platforms_to_skip_build.add(StdlibDeploymentTarget.Linux)
        if not stage_dependent_args.build_freebsd:
            platforms_to_skip_build.add(StdlibDeploymentTarget.FreeBSD)
        if not stage_dependent_args.build_cygwin:
            platforms_to_skip_build.add(StdlibDeploymentTarget.Cygwin)
        if not stage_dependent_args.build_osx:
            platforms_to_skip_build.add(StdlibDeploymentTarget.OSX)
        if not stage_dependent_args.build_ios_device:
            platforms_to_skip_build.add(StdlibDeploymentTarget.iOS)
        if not stage_dependent_args.build_ios_simulator:
            platforms_to_skip_build.add(StdlibDeploymentTarget.iOSSimulator)
        if not stage_dependent_args.build_tvos_device:
            platforms_to_skip_build.add(StdlibDeploymentTarget.AppleTV)
        if not stage_dependent_args.build_tvos_simulator:
            platforms_to_skip_build.add(
                StdlibDeploymentTarget.AppleTVSimulator)
        if not stage_dependent_args.build_watchos_device:
            platforms_to_skip_build.add(StdlibDeploymentTarget.AppleWatch)
        if not stage_dependent_args.build_watchos_simulator:
            platforms_to_skip_build.add(
                StdlibDeploymentTarget.AppleWatchSimulator)
        if not stage_dependent_args.build_android:
            platforms_to_skip_build.add(StdlibDeploymentTarget.Android)
        if not args.build_wasm:
            platforms_to_skip_build.add(StdlibDeploymentTarget.WASI)
        return platforms_to_skip_build

    def __platforms_to_skip_test(self, args, stage_dependent_args):
        platforms_to_skip_test = set()
        if not stage_dependent_args.test_linux:
            platforms_to_skip_test.add(StdlibDeploymentTarget.Linux)
        if not stage_dependent_args.test_freebsd:
            platforms_to_skip_test.add(StdlibDeploymentTarget.FreeBSD)
        if not stage_dependent_args.test_cygwin:
            platforms_to_skip_test.add(StdlibDeploymentTarget.Cygwin)
        if not stage_dependent_args.test_osx:
            platforms_to_skip_test.add(StdlibDeploymentTarget.OSX)
        if not stage_dependent_args.test_ios_host and not args.only_non_executable_test:
            platforms_to_skip_test.add(StdlibDeploymentTarget.iOS)
        elif not args.only_non_executable_test:
            raise ArgumentError(None,
                                "error: iOS device tests are not " +
                                "supported in open-source Swift.")
        if not stage_dependent_args.test_ios_simulator:
            platforms_to_skip_test.add(StdlibDeploymentTarget.iOSSimulator)
        if not stage_dependent_args.test_tvos_host and \
           not args.only_non_executable_test:
            platforms_to_skip_test.add(StdlibDeploymentTarget.AppleTV)
        elif not args.only_non_executable_test:
            raise ArgumentError(None,
                                "error: tvOS device tests are not " +
                                "supported in open-source Swift.")
        if not stage_dependent_args.test_tvos_simulator:
            platforms_to_skip_test.add(StdlibDeploymentTarget.AppleTVSimulator)
        if not stage_dependent_args.test_watchos_host and \
           not args.only_non_executable_test:
            platforms_to_skip_test.add(StdlibDeploymentTarget.AppleWatch)
        elif not args.only_non_executable_test:
            raise ArgumentError(None,
                                "error: watchOS device tests are not " +
                                "supported in open-source Swift.")
        if not stage_dependent_args.test_watchos_simulator:
            platforms_to_skip_test.add(
                StdlibDeploymentTarget.AppleWatchSimulator)
        if not stage_dependent_args.test_android:
            platforms_to_skip_test.add(StdlibDeploymentTarget.Android)

        return platforms_to_skip_test

    def __platforms_archs_to_skip_test(self, args, stage_dependent_args, host_target):
        platforms_archs_to_skip_test = set()
        if not stage_dependent_args.test_watchos_32bit_simulator:
            platforms_archs_to_skip_test.add(
                StdlibDeploymentTarget.AppleWatchSimulator.i386)
        if host_target == StdlibDeploymentTarget.OSX.x86_64.name:
            platforms_archs_to_skip_test.add(
                StdlibDeploymentTarget.iOSSimulator.arm64)
            platforms_archs_to_skip_test.add(
                StdlibDeploymentTarget.AppleTVSimulator.arm64)
            platforms_archs_to_skip_test.add(
                StdlibDeploymentTarget.AppleWatchSimulator.arm64)
        if host_target == StdlibDeploymentTarget.OSX.arm64.name:
            platforms_archs_to_skip_test.add(
                StdlibDeploymentTarget.iOSSimulator.x86_64)
            platforms_archs_to_skip_test.add(
                StdlibDeploymentTarget.AppleTVSimulator.x86_64)
            platforms_archs_to_skip_test.add(
                StdlibDeploymentTarget.AppleWatchSimulator.i386)
            platforms_archs_to_skip_test.add(
                StdlibDeploymentTarget.AppleWatchSimulator.x86_64)

        return platforms_archs_to_skip_test

    def __platforms_to_skip_test_host(self, args, stage_dependent_args):
        platforms_to_skip_test_host = set()
        if not stage_dependent_args.test_android_host:
            platforms_to_skip_test_host.add(StdlibDeploymentTarget.Android)
        if not stage_dependent_args.test_ios_host and \
           not args.only_non_executable_test:
            platforms_to_skip_test_host.add(StdlibDeploymentTarget.iOS)
        if not stage_dependent_args.test_tvos_host and \
           not args.only_non_executable_test:
            platforms_to_skip_test_host.add(StdlibDeploymentTarget.AppleTV)
        if not stage_dependent_args.test_watchos_host and \
           not args.only_non_executable_test:
            platforms_to_skip_test_host.add(StdlibDeploymentTarget.AppleWatch)
        return platforms_to_skip_test_host
