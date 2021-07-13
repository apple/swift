# -*- python -*-
# test_host_specific_configuration.py - Unit tests for
# swift_build_support.host_specific_configuration
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors

import unittest
from argparse import Namespace

from swift_build_support.host_specific_configuration import \
    HostSpecificConfiguration


class ToolchainTestCase(unittest.TestCase):

    def test_should_configure_and_build_when_deployment_is_all(self):
        host_target = 'macosx-x86_64'
        args = self.default_args()
        args.build_osx = True
        args.build_ios_device = True
        args.host_target = host_target
        args.stdlib_deployment_targets = [host_target, 'iphoneos-arm64']
        args.build_stdlib_deployment_targets = 'all'

        hsc = HostSpecificConfiguration(host_target, args)

        self.assertEqual(len(hsc.sdks_to_configure), 2)
        self.assertIn('OSX', hsc.sdks_to_configure)
        self.assertIn('IOS', hsc.sdks_to_configure)

        self.assertEqual(len(hsc.swift_stdlib_build_targets), 2)
        self.assertIn('swift-test-stdlib-macosx-x86_64',
                      hsc.swift_stdlib_build_targets)
        self.assertIn('swift-test-stdlib-iphoneos-arm64',
                      hsc.swift_stdlib_build_targets)

    def test_should_only_deployment_if_specified(self):
        host_target = 'macosx-x86_64'
        args = self.default_args()
        args.build_osx = True
        args.build_ios_device = True
        args.host_target = host_target
        args.stdlib_deployment_targets = [host_target, 'iphoneos-arm64']
        args.build_stdlib_deployment_targets = ['iphoneos-arm64']

        hsc = HostSpecificConfiguration(host_target, args)

        self.assertEqual(len(hsc.sdks_to_configure), 2)
        self.assertIn('OSX', hsc.sdks_to_configure)
        self.assertIn('IOS', hsc.sdks_to_configure)

        self.assertEqual(len(hsc.swift_stdlib_build_targets), 1)
        self.assertIn('swift-test-stdlib-iphoneos-arm64',
                      hsc.swift_stdlib_build_targets)

    def test_should_configure_and_build_when_cross_compiling(self):
        args = self.default_args()
        args.build_ios_device = True
        args.host_target = 'macosx-x86_64'

        hsc = HostSpecificConfiguration('iphoneos-arm64', args)

        self.assertEqual(len(hsc.sdks_to_configure), 1)
        self.assertIn('IOS', hsc.sdks_to_configure)

        self.assertEqual(len(hsc.swift_stdlib_build_targets), 1)
        self.assertIn('swift-test-stdlib-iphoneos-arm64',
                      hsc.swift_stdlib_build_targets)

    def test_should_configure_and_build_cross_compiling_with_stdlib_targets(self):
        args = self.default_args()
        args.build_ios_device = True
        args.host_target = 'macosx-x86_64'
        args.stdlib_deployment_targets = ['iphoneos-arm64']

        hsc = HostSpecificConfiguration('iphoneos-arm64', args)

        self.assertEqual(len(hsc.sdks_to_configure), 1)
        self.assertIn('IOS', hsc.sdks_to_configure)

        self.assertEqual(len(hsc.swift_stdlib_build_targets), 1)
        self.assertIn('swift-test-stdlib-iphoneos-arm64',
                      hsc.swift_stdlib_build_targets)

    def test_should_only_configure_when_cross_compiling_different_stdlib_targets(self):
        args = self.default_args()
        args.build_ios_device = True
        args.host_target = 'macosx-x86_64'
        args.stdlib_deployment_targets = ['iphonesimulator-arm64']

        hsc = HostSpecificConfiguration('iphoneos-arm64', args)

        self.assertEqual(len(hsc.sdks_to_configure), 1)
        self.assertIn('IOS', hsc.sdks_to_configure)

        self.assertEqual(len(hsc.swift_stdlib_build_targets), 0)

    def test_test_should_configure_and_build_when_cross_compiling_macos(self):
        args = self.default_args()
        args.build_osx = True
        args.build_stdlib_when_cross_compiling = True
        args.host_target = 'macosx-x86_64'
        args.stdlib_deployment_targets = ['macosx-x86_64', 'macosx-arm64']

        hsc = HostSpecificConfiguration('macosx-arm64', args)

        self.assertEqual(len(hsc.sdks_to_configure), 1)
        self.assertEqual(len(hsc.swift_stdlib_build_targets), 1)
        self.assertIn('swift-test-stdlib-macosx-arm64',
                      hsc.swift_stdlib_build_targets)

    def test_should_skip_stdlib_when_cross_compiling_if_told_so_macos(self):
        args = self.default_args()
        args.build_osx = True
        args.build_stdlib_when_cross_compiling = False
        args.host_target = 'macosx-x86_64'
        args.stdlib_deployment_targets = ['macosx-x86_64', 'macosx-arm64']

        hsc = HostSpecificConfiguration('macosx-arm64', args)

        self.assertEqual(len(hsc.sdks_to_configure), 0)
        self.assertEqual(len(hsc.swift_stdlib_build_targets), 0)

    def generate_should_skip_building_platform(
            host_target, sdk_name, build_target, build_arg_name):
        def test(self):
            args = self.default_args()
            args.host_target = host_target
            args.stdlib_deployment_targets = [host_target]
            args.build_stdlib_deployment_targets = 'all'

            before = HostSpecificConfiguration(host_target, args)
            self.assertIn(sdk_name, before.sdks_to_configure)
            self.assertNotIn(build_target, before.swift_stdlib_build_targets)

            setattr(args, build_arg_name, True)
            after = HostSpecificConfiguration(host_target, args)
            self.assertIn(sdk_name, after.sdks_to_configure)
            self.assertIn(build_target, after.swift_stdlib_build_targets)
        return test

    test_should_skip_building_android =\
        generate_should_skip_building_platform(
            'android-armv7',
            'ANDROID',
            'swift-test-stdlib-android-armv7',
            'build_android')
    test_should_skip_building_cygwin =\
        generate_should_skip_building_platform(
            'cygwin-x86_64',
            'CYGWIN',
            'swift-test-stdlib-cygwin-x86_64',
            'build_cygwin')
    test_should_skip_building_freebsd =\
        generate_should_skip_building_platform(
            'freebsd-x86_64',
            'FREEBSD',
            'swift-test-stdlib-freebsd-x86_64',
            'build_freebsd')
    test_should_skip_building_ios =\
        generate_should_skip_building_platform(
            'iphoneos-arm64',
            'IOS',
            'swift-test-stdlib-iphoneos-arm64',
            'build_ios_device')
    test_should_skip_building_ios_sim =\
        generate_should_skip_building_platform(
            'iphonesimulator-x86_64',
            'IOS_SIMULATOR',
            'swift-test-stdlib-iphonesimulator-x86_64',
            'build_ios_simulator')
    test_should_skip_building_linux =\
        generate_should_skip_building_platform(
            'linux-x86_64',
            'LINUX',
            'swift-test-stdlib-linux-x86_64',
            'build_linux')
    test_should_skip_building_osx =\
        generate_should_skip_building_platform(
            'macosx-x86_64',
            'OSX',
            'swift-test-stdlib-macosx-x86_64',
            'build_osx')
    test_should_skip_building_tvos =\
        generate_should_skip_building_platform(
            'appletvos-arm64',
            'TVOS',
            'swift-test-stdlib-appletvos-arm64',
            'build_tvos_device')
    test_should_skip_building_tvos_sim =\
        generate_should_skip_building_platform(
            'appletvsimulator-x86_64', 'TVOS_SIMULATOR',
            'swift-test-stdlib-appletvsimulator-x86_64',
            'build_tvos_simulator')
    test_should_skip_building_watchos =\
        generate_should_skip_building_platform(
            'watchos-armv7k',
            'WATCHOS',
            'swift-test-stdlib-watchos-armv7k',
            'build_watchos_device')
    test_should_skip_building_watchos_sim =\
        generate_should_skip_building_platform(
            'watchsimulator-i386',
            'WATCHOS_SIMULATOR',
            'swift-test-stdlib-watchsimulator-i386',
            'build_watchos_simulator')

    def generate_should_build_full_targets_when_test(test_arg_name):
        def test(self):
            host_target = 'macosx-x86_64'
            args = self.default_args()
            args.build_osx = True
            args.host_target = host_target
            args.stdlib_deployment_targets = [host_target]
            args.build_stdlib_deployment_targets = 'all'

            before = HostSpecificConfiguration(host_target, args)
            self.assertIn('swift-test-stdlib-macosx-x86_64',
                          before.swift_stdlib_build_targets)
            self.assertNotIn('swift-stdlib-macosx-x86_64',
                             before.swift_stdlib_build_targets)

            setattr(args, test_arg_name, True)
            after = HostSpecificConfiguration(host_target, args)
            self.assertIn('swift-stdlib-macosx-x86_64',
                          after.swift_stdlib_build_targets)
            self.assertNotIn('swift-test-stdlib-macosx-x86_64',
                             after.swift_stdlib_build_targets)
        return test

    test_should_build_full_targets_when_unittest_extra =\
        generate_should_build_full_targets_when_test(
            'build_swift_stdlib_unittest_extra')
    test_should_build_full_targets_when_validation_test =\
        generate_should_build_full_targets_when_test(
            'validation_test')
    test_should_build_full_targets_when_long_test =\
        generate_should_build_full_targets_when_test(
            'long_test')
    test_should_build_full_targets_when_stress_test =\
        generate_should_build_full_targets_when_test(
            'stress_test')

    def generate_should_skip_testing_platform(
            host_target, build_arg_name, test_arg_name):
        def test(self):
            args = self.default_args()
            setattr(args, build_arg_name, True)
            args.host_target = host_target
            args.stdlib_deployment_targets = [host_target]
            args.build_stdlib_deployment_targets = 'all'

            before = HostSpecificConfiguration(host_target, args)
            self.assertEqual(len(before.swift_test_run_targets), 0)

            setattr(args, test_arg_name, True)
            after = HostSpecificConfiguration(host_target, args)
            self.assertIn('check-swift-{}'.format(host_target),
                          after.swift_test_run_targets)
        return test

    test_should_skip_testing_android =\
        generate_should_skip_testing_platform(
            'android-armv7',
            'build_android',
            'test_android')
    test_should_skip_testing_cygwin =\
        generate_should_skip_testing_platform(
            'cygwin-x86_64',
            'build_cygwin',
            'test_cygwin')
    test_should_skip_testing_freebsd =\
        generate_should_skip_testing_platform(
            'freebsd-x86_64',
            'build_freebsd',
            'test_freebsd')
    # NOTE: test_ios_host is not supported in open-source Swift
    test_should_skip_testing_ios_sim =\
        generate_should_skip_testing_platform(
            'iphonesimulator-x86_64',
            'build_ios_simulator',
            'test_ios_simulator')
    test_should_skip_testing_linux =\
        generate_should_skip_testing_platform(
            'linux-x86_64',
            'build_linux',
            'test_linux')
    test_should_skip_testing_osx =\
        generate_should_skip_testing_platform(
            'macosx-x86_64',
            'build_osx',
            'test_osx')
    # NOTE: test_tvos_host is not supported in open-source Swift
    test_should_skip_testing_tvos_sim =\
        generate_should_skip_testing_platform(
            'appletvsimulator-x86_64',
            'build_tvos_simulator',
            'test_tvos_simulator')
    # NOTE: test_watchos_host is not supported in open-source Swift
    test_should_skip_testing_watchos_sim =\
        generate_should_skip_testing_platform(
            'watchsimulator-i386',
            'build_watchos_simulator',
            'test_watchos_simulator')

    def test_should_skip_testing_32bit_ios(self):
        host_target = 'iphonesimulator-i386'
        args = self.default_args()
        args.build_ios_simulator = True
        args.test_ios_simulator = True
        args.host_target = host_target
        args.stdlib_deployment_targets = [host_target]
        args.build_stdlib_deployment_targets = 'all'

        before = HostSpecificConfiguration(host_target, args)
        self.assertEqual(len(before.swift_test_run_targets), 0)

        args.test_ios_32bit_simulator = True
        after = HostSpecificConfiguration(host_target, args)
        self.assertIn('check-swift-iphonesimulator-i386',
                      after.swift_test_run_targets)

    def test_should_skip_testing_32bit_watchos(self):
        host_target = 'watchsimulator-i386'
        args = self.default_args()
        args.build_watchos_simulator = True
        args.test_watchos_simulator = True
        args.test_watchos_32bit_simulator = False
        args.host_target = host_target
        args.stdlib_deployment_targets = [host_target]
        args.build_stdlib_deployment_targets = 'all'

        before = HostSpecificConfiguration(host_target, args)
        self.assertEqual(len(before.swift_test_run_targets), 0)

        args.test_watchos_32bit_simulator = True
        after = HostSpecificConfiguration(host_target, args)
        self.assertIn('check-swift-watchsimulator-i386',
                      after.swift_test_run_targets)

    def generate_should_allow_testing_only_host(
            host_target, build_arg_name, test_arg_name, host_test_arg_name):
        def test(self):
            args = self.default_args()
            setattr(args, build_arg_name, True)
            setattr(args, test_arg_name, True)
            args.host_target = host_target
            args.stdlib_deployment_targets = [host_target]
            args.build_stdlib_deployment_targets = 'all'

            before = HostSpecificConfiguration(host_target, args)
            self.assertIn('check-swift-{}'.format(host_target),
                          before.swift_test_run_targets)

            setattr(args, host_test_arg_name, True)
            after = HostSpecificConfiguration(host_target, args)
            self.assertIn(
                'check-swift-only_non_executable-{}'.format(host_target),
                after.swift_test_run_targets)
        return test

    test_should_allow_testing_only_host_android =\
        generate_should_allow_testing_only_host(
            'android-armv7',
            'build_android',
            'test_android',
            'test_android_host')
    # NOTE: test_ios_host is not supported in open-source Swift
    # NOTE: test_tvos_host is not supported in open-source Swift
    # NOTE: test_watchos_host is not supported in open-source Swift

    def test_should_allow_testing_only_executable_tests(self):
        args = self.default_args()
        args.build_osx = True
        args.test_osx = True
        args.host_target = 'macosx-x86_64'
        args.stdlib_deployment_targets = ['macosx-x86_64']
        args.build_stdlib_deployment_targets = 'all'

        before = HostSpecificConfiguration('macosx-x86_64', args)
        self.assertIn('check-swift-macosx-x86_64',
                      before.swift_test_run_targets)

        args.only_executable_test = True
        after = HostSpecificConfiguration('macosx-x86_64', args)
        self.assertIn('check-swift-only_executable-macosx-x86_64',
                      after.swift_test_run_targets)

    def test_should_allow_testing_only_non_executable_tests(self):
        args = self.default_args()
        args.build_osx = True
        args.test_osx = True
        args.host_target = 'macosx-x86_64'
        args.stdlib_deployment_targets = ['macosx-x86_64']
        args.build_stdlib_deployment_targets = 'all'

        before = HostSpecificConfiguration('macosx-x86_64', args)
        self.assertIn('check-swift-macosx-x86_64',
                      before.swift_test_run_targets)

        args.only_non_executable_test = True
        after = HostSpecificConfiguration('macosx-x86_64', args)
        self.assertIn('check-swift-only_non_executable-macosx-x86_64',
                      after.swift_test_run_targets)

    def generate_should_build_benchmarks(host_target, build_arg_name):
        def test(self):
            args = self.default_args()
            setattr(args, build_arg_name, True)
            args.host_target = host_target
            args.stdlib_deployment_targets = [host_target]
            args.build_stdlib_deployment_targets = 'all'

            with_benchmark = HostSpecificConfiguration(host_target, args)
            self.assertIn('swift-benchmark-{}'.format(host_target),
                          with_benchmark.swift_benchmark_build_targets)
            self.assertNotIn('check-swift-benchmark-{}'.format(host_target),
                             with_benchmark.swift_benchmark_run_targets)

            args.benchmark = True
            running_benchmarks = HostSpecificConfiguration(host_target, args)
            self.assertIn('swift-benchmark-{}'.format(host_target),
                          running_benchmarks.swift_benchmark_build_targets)
            self.assertIn('check-swift-benchmark-{}'.format(host_target),
                          running_benchmarks.swift_benchmark_run_targets)

            args.build_external_benchmarks = True
            with_external_benchmarks = HostSpecificConfiguration(host_target,
                                                                 args)
            self.assertIn(
                'swift-benchmark-{}'.format(host_target),
                with_external_benchmarks.swift_benchmark_build_targets)
            self.assertIn(
                'swift-benchmark-{}-external'.format(host_target),
                with_external_benchmarks.swift_benchmark_build_targets)
            self.assertIn('check-swift-benchmark-{}'.format(host_target),
                          with_external_benchmarks.swift_benchmark_run_targets)
            self.assertIn(
                'check-swift-benchmark-{}-external'.format(host_target),
                with_external_benchmarks.swift_benchmark_run_targets)

            args.benchmark = False
            not_running_benchmarks = HostSpecificConfiguration(host_target,
                                                               args)
            self.assertIn('swift-benchmark-{}'.format(host_target),
                          not_running_benchmarks.swift_benchmark_build_targets)
            self.assertIn('swift-benchmark-{}-external'.format(host_target),
                          not_running_benchmarks.swift_benchmark_build_targets)
            self.assertNotIn(
                'check-swift-benchmark-{}'.format(host_target),
                not_running_benchmarks.swift_benchmark_run_targets)
            self.assertNotIn(
                'check-swift-benchmark-{}-external'.format(host_target),
                not_running_benchmarks.swift_benchmark_run_targets)
        return test

    test_should_build_and_run_benchmarks_osx_x86_64 =\
        generate_should_build_benchmarks(
            'macosx-x86_64',
            'build_osx')
    test_should_build_and_run_benchmarks_ios_armv7 =\
        generate_should_build_benchmarks(
            'iphoneos-armv7',
            'build_ios_device')
    test_should_build_and_run_benchmarks_ios_arm64 =\
        generate_should_build_benchmarks(
            'iphoneos-arm64',
            'build_ios_device')
    test_should_build_and_run_benchmarks_tvos_arm64 =\
        generate_should_build_benchmarks(
            'appletvos-arm64',
            'build_tvos_device')
    test_should_build_and_run_benchmarks_watchos_armv7k =\
        generate_should_build_benchmarks(
            'watchos-armv7k',
            'build_watchos_device')
    # NOTE: other platforms/architectures do not support benchmarks

    def generate_should_test_only_subset(subset_name, subset_arg_name):
        def test(self):
            host_target = 'macosx-x86_64'
            args = self.default_args()
            args.build_osx = True
            args.test_osx = True
            args.host_target = host_target
            args.stdlib_deployment_targets = [host_target]
            args.build_stdlib_deployment_targets = 'all'

            all = 'check-swift-macosx-x86_64'
            subset = 'check-swift-{}-macosx-x86_64'.format(subset_name)

            before = HostSpecificConfiguration(host_target, args)
            self.assertIn(all, before.swift_test_run_targets)
            self.assertNotIn(subset, before.swift_test_run_targets)

            setattr(args, subset_arg_name, True)
            after = HostSpecificConfiguration(host_target, args)
            self.assertIn(subset, after.swift_test_run_targets)
            self.assertNotIn(all, after.swift_test_run_targets)
        return test

    test_should_test_only_subset_validation =\
        generate_should_test_only_subset('validation', 'validation_test')
    test_should_test_only_subset_long =\
        generate_should_test_only_subset('only_long', 'long_test')
    test_should_test_only_subset_stress =\
        generate_should_test_only_subset('only_stress', 'stress_test')

    def test_should_test_all_when_validation_long_and_stress(self):
        host_target = 'macosx-x86_64'
        args = self.default_args()
        args.build_osx = True
        args.test_osx = True
        args.host_target = host_target
        args.stdlib_deployment_targets = [host_target]
        args.build_stdlib_deployment_targets = 'all'

        all = 'check-swift-macosx-x86_64'
        subset = 'check-swift-all-macosx-x86_64'

        before = HostSpecificConfiguration(host_target, args)
        self.assertIn(all, before.swift_test_run_targets)
        self.assertNotIn(subset, before.swift_test_run_targets)

        args.validation_test = True
        args.long_test = True
        args.stress_test = True
        after = HostSpecificConfiguration(host_target, args)
        self.assertIn(subset, after.swift_test_run_targets)
        self.assertNotIn(all, after.swift_test_run_targets)

    def generate_should_test_only_subset_for_host_only_tests(
            subset_name, subset_arg_name):
        def test(self):
            host_target = 'android-armv7'
            args = self.default_args()
            args.build_android = True
            args.test_android = True
            args.test_android_host = True
            args.host_target = host_target
            args.stdlib_deployment_targets = [host_target]
            args.build_stdlib_deployment_targets = 'all'

            all = 'check-swift-only_non_executable-android-armv7'
            subset = 'check-swift-{}-only_non_executable-android-armv7'\
                .format(subset_name)

            before = HostSpecificConfiguration(host_target, args)
            self.assertIn(all, before.swift_test_run_targets)
            self.assertNotIn(subset, before.swift_test_run_targets)

            setattr(args, subset_arg_name, True)
            after = HostSpecificConfiguration(host_target, args)
            self.assertIn(subset, after.swift_test_run_targets)
            self.assertNotIn(all, after.swift_test_run_targets)
        return test

    test_should_test_only_subset_for_host_only_tests_validation =\
        generate_should_test_only_subset_for_host_only_tests(
            'validation',
            'validation_test')
    test_should_test_only_subset_for_host_only_tests_long =\
        generate_should_test_only_subset_for_host_only_tests(
            'only_long',
            'long_test')
    test_should_test_only_subset_for_host_only_tests_stress =\
        generate_should_test_only_subset_for_host_only_tests(
            'only_stress',
            'stress_test')

    def test_should_test_all_when_validation_long_and_stress_with_host_only(
            self):
        host_target = 'android-armv7'
        args = self.default_args()
        args.build_android = True
        args.test_android = True
        args.test_android_host = True
        args.host_target = host_target
        args.stdlib_deployment_targets = [host_target]
        args.build_stdlib_deployment_targets = 'all'

        all = 'check-swift-only_non_executable-android-armv7'
        subset = 'check-swift-all-only_non_executable-android-armv7'

        before = HostSpecificConfiguration(host_target, args)
        self.assertIn(all, before.swift_test_run_targets)
        self.assertNotIn(subset, before.swift_test_run_targets)

        args.validation_test = True
        args.long_test = True
        args.stress_test = True
        after = HostSpecificConfiguration(host_target, args)
        self.assertIn(subset, after.swift_test_run_targets)
        self.assertNotIn(all, after.swift_test_run_targets)

    def generate_should_test_optimizations(
            optimize_name, optimize_arg_name):
        def test(self):
            host_target = 'macosx-x86_64'
            args = self.default_args()
            args.build_osx = True
            args.test_osx = True
            args.host_target = host_target
            args.stdlib_deployment_targets = [host_target]
            args.build_stdlib_deployment_targets = 'all'

            target = 'check-swift-{}-macosx-x86_64'.format(optimize_name)

            before = HostSpecificConfiguration(host_target, args)
            self.assertNotIn(target, before.swift_test_run_targets)

            setattr(args, optimize_arg_name, True)
            after = HostSpecificConfiguration(host_target, args)
            self.assertIn(target, after.swift_test_run_targets)
        return test

    test_should_test_optimizations =\
        generate_should_test_optimizations(
            'optimize',
            'test_optimized')
    test_should_test_optimizations_size =\
        generate_should_test_optimizations(
            'optimize_size',
            'test_optimize_for_size')
    test_should_test_optimizations_none_implicit_dynamic =\
        generate_should_test_optimizations(
            'optimize_none_with_implicit_dynamic',
            'test_optimize_none_with_implicit_dynamic')

    def test_should_not_test_optimizations_when_testing_only_host(self):
        host_target = 'android-armv7'
        args = self.default_args()
        args.host_target = host_target
        args.build_android = True
        args.test_android = True
        args.stdlib_deployment_targets = [host_target]
        args.build_stdlib_deployment_targets = 'all'
        args.test_optimized = True
        args.test_optimize_for_size = True
        args.test_optimize_none_with_implicit_dynamic = True

        before = HostSpecificConfiguration(host_target, args)
        self.assertIn('check-swift-optimize-android-armv7',
                      before.swift_test_run_targets)
        self.assertIn('check-swift-optimize_size-android-armv7',
                      before.swift_test_run_targets)
        self.assertIn(
            'check-swift-optimize_none_with_implicit_dynamic-android-armv7',
            before.swift_test_run_targets)

        args.test_android_host = True
        after = HostSpecificConfiguration(host_target, args)
        self.assertNotIn('check-swift-optimize-android-armv7',
                         after.swift_test_run_targets)
        self.assertNotIn(
            'check-swift-optimize_size-android-armv7',
            after.swift_test_run_targets)
        self.assertNotIn(
            'check-swift-optimize_none_with_implicit_dynamic-android-armv7',
            after.swift_test_run_targets)

    def test_should_test_optimizations_with_subsets(self):
        host_target = 'android-armv7'
        args = self.default_args()
        args.host_target = host_target
        args.build_android = True
        args.test_android = True
        args.stdlib_deployment_targets = [host_target]
        args.build_stdlib_deployment_targets = 'all'
        args.test_optimized = True
        args.test_optimize_for_size = True
        args.test_optimize_none_with_implicit_dynamic = True
        args.long_test = True

        target_name = 'check-swift-only_long-{}-android-armv7'

        before = HostSpecificConfiguration(host_target, args)
        self.assertIn(target_name.format('optimize'),
                      before.swift_test_run_targets)
        self.assertIn(target_name.format('optimize_size'),
                      before.swift_test_run_targets)
        self.assertIn(target_name.format(
                      'optimize_none_with_implicit_dynamic'),
                      before.swift_test_run_targets)

    def default_args(self):
        return Namespace(
            benchmark=False,
            build_android=False,
            build_cygwin=False,
            build_external_benchmarks=False,
            build_freebsd=False,
            build_ios_device=False,
            build_ios_simulator=False,
            build_linux=False,
            build_osx=False,
            build_swift_stdlib_unittest_extra=False,
            build_tvos_device=False,
            build_tvos_simulator=False,
            build_watchos_device=False,
            build_watchos_simulator=False,
            maccatalyst=False,
            maccatalyst_ios_tests=False,
            long_test=False,
            only_executable_test=False,
            only_non_executable_test=False,
            stress_test=False,
            test_android=False,
            test_android_host=False,
            test_cygwin=False,
            test_freebsd=False,
            test_ios_host=False,
            test_ios_simulator=False,
            test_ios_32bit_simulator=False,
            test_watchos_32bit_simulator=True,
            test_linux=False,
            test_optimize_for_size=False,
            test_optimize_none_with_implicit_dynamic=False,
            test_optimized=False,
            test_osx=False,
            test_tvos_host=False,
            test_tvos_simulator=False,
            test_watchos_host=False,
            test_watchos_simulator=False,
            validation_test=False)


if __name__ == '__main__':
    unittest.main()
