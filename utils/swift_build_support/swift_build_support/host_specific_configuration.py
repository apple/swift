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

import sys
from argparse import ArgumentError

from .targets import StdlibDeploymentTarget


class HostSpecificConfiguration(object):

    """Configuration information for an individual host."""

    def __init__(self, host_target, args):
        """Initialize for the given `host_target`."""

        # Compute the set of deployment targets to configure/build.
        if host_target == args.host_target:
            # This host is the user's desired product, so honor the requested
            # set of targets to configure/build.
            stdlib_targets_to_configure = args.stdlib_deployment_targets
            if "all" in args.build_stdlib_deployment_targets:
                stdlib_targets_to_build = set(stdlib_targets_to_configure)
            else:
                stdlib_targets_to_build = set(
                    args.build_stdlib_deployment_targets).intersection(
                    set(args.stdlib_deployment_targets))
        else:
            # Otherwise, this is a host we are building as part of
            # cross-compiling, so we only need the target itself.
            stdlib_targets_to_configure = [host_target]
            stdlib_targets_to_build = set(stdlib_targets_to_configure)

        # Compute derived information from the arguments.
        #
        # FIXME: We should move the platform-derived arguments to be entirely
        # data driven, so that we can eliminate this code duplication and just
        # iterate over all supported platforms.
        platforms_to_skip_build = self.__platforms_to_skip_build(args)
        platforms_to_skip_test = self.__platforms_to_skip_test(args)
        platforms_archs_to_skip_test = \
            self.__platforms_archs_to_skip_test(args)
        platforms_to_skip_test_host = self.__platforms_to_skip_test_host(args)

        # Compute the lists of **CMake** targets for each use case (configure
        # vs. build vs. run) and the SDKs to configure with.
        self.sdks_to_configure = set()
        self.swift_stdlib_build_targets = []
        self.swift_test_run_targets = []
        self.swift_benchmark_build_targets = []
        self.swift_benchmark_run_targets = []
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
                if args.build_swift_stdlib_unittest_extra or \
                        args.validation_test or args.long_test or \
                        args.stress_test:
                    self.swift_stdlib_build_targets.append(
                        "swift-stdlib-" + name)
                else:
                    self.swift_stdlib_build_targets.append(
                        "swift-test-stdlib-" + name)
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

                # Support for running the macCatalyst tests with
                # the iOS-like target triple.
                if name == "macosx-x86_64" and args.maccatalyst \
                   and args.maccatalyst_ios_tests:
                    (self.swift_test_run_targets
                     .append("check-swift{}{}-{}".format(
                         subset_suffix, suffix, "macosx-maccatalyst-x86_64")))
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

    def __platforms_to_skip_build(self, args):
        platforms_to_skip_build = set()
        if not args.build_linux:
            platforms_to_skip_build.add(StdlibDeploymentTarget.Linux)
        if not args.build_freebsd:
            platforms_to_skip_build.add(StdlibDeploymentTarget.FreeBSD)
        if not args.build_cygwin:
            platforms_to_skip_build.add(StdlibDeploymentTarget.Cygwin)
        if not args.build_osx:
            platforms_to_skip_build.add(StdlibDeploymentTarget.OSX)
        if not args.build_ios_device:
            platforms_to_skip_build.add(StdlibDeploymentTarget.iOS)
        if not args.build_ios_simulator:
            platforms_to_skip_build.add(StdlibDeploymentTarget.iOSSimulator)
        if not args.build_tvos_device:
            platforms_to_skip_build.add(StdlibDeploymentTarget.AppleTV)
        if not args.build_tvos_simulator:
            platforms_to_skip_build.add(
                StdlibDeploymentTarget.AppleTVSimulator)
        if not args.build_watchos_device:
            platforms_to_skip_build.add(StdlibDeploymentTarget.AppleWatch)
        if not args.build_watchos_simulator:
            platforms_to_skip_build.add(
                StdlibDeploymentTarget.AppleWatchSimulator)
        if not args.build_android:
            platforms_to_skip_build.add(StdlibDeploymentTarget.Android)
        return platforms_to_skip_build

    def __platforms_to_skip_test(self, args):
        platforms_to_skip_test = set()
        if not args.test_linux:
            platforms_to_skip_test.add(StdlibDeploymentTarget.Linux)
        if not args.test_freebsd:
            platforms_to_skip_test.add(StdlibDeploymentTarget.FreeBSD)
        if not args.test_cygwin:
            platforms_to_skip_test.add(StdlibDeploymentTarget.Cygwin)
        if not args.test_osx:
            platforms_to_skip_test.add(StdlibDeploymentTarget.OSX)
        if not args.test_ios_host and not args.only_non_executable_test:
            platforms_to_skip_test.add(StdlibDeploymentTarget.iOS)
        elif not args.only_non_executable_test:
            raise ArgumentError(None,
                                "error: iOS device tests are not " +
                                "supported in open-source Swift.")
        if not args.test_ios_simulator:
            platforms_to_skip_test.add(StdlibDeploymentTarget.iOSSimulator)
        if not args.test_tvos_host and not args.only_non_executable_test:
            platforms_to_skip_test.add(StdlibDeploymentTarget.AppleTV)
        elif not args.only_non_executable_test:
            raise ArgumentError(None,
                                "error: tvOS device tests are not " +
                                "supported in open-source Swift.")
        if not args.test_tvos_simulator:
            platforms_to_skip_test.add(StdlibDeploymentTarget.AppleTVSimulator)
        if not args.test_watchos_host and not args.only_non_executable_test:
            platforms_to_skip_test.add(StdlibDeploymentTarget.AppleWatch)
        elif not args.only_non_executable_test:
            raise ArgumentError(None,
                                "error: watchOS device tests are not " +
                                "supported in open-source Swift.")
        if not args.test_watchos_simulator:
            platforms_to_skip_test.add(
                StdlibDeploymentTarget.AppleWatchSimulator)
        if not args.test_android:
            platforms_to_skip_test.add(StdlibDeploymentTarget.Android)
        if not args.test_wasi:
            platforms_to_skip_test.add(StdlibDeploymentTarget.WASI)

        return platforms_to_skip_test

    def __platforms_archs_to_skip_test(self, args):
        platforms_archs_to_skip_test = set()
        if not args.test_ios_32bit_simulator:
            platforms_archs_to_skip_test.add(
                StdlibDeploymentTarget.iOSSimulator.i386)
        return platforms_archs_to_skip_test

    def __platforms_to_skip_test_host(self, args):
        platforms_to_skip_test_host = set()
        if not args.test_android_host:
            platforms_to_skip_test_host.add(StdlibDeploymentTarget.Android)
        if not args.test_ios_host and not args.only_non_executable_test:
            platforms_to_skip_test_host.add(StdlibDeploymentTarget.iOS)
        if not args.test_tvos_host and not args.only_non_executable_test:
            platforms_to_skip_test_host.add(StdlibDeploymentTarget.AppleTV)
        if not args.test_watchos_host and not args.only_non_executable_test:
            platforms_to_skip_test_host.add(StdlibDeploymentTarget.AppleWatch)
        return platforms_to_skip_test_host
