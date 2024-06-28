# swift_build_support/products/swiftdriver.py -------------------*- python -*-
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

import os

from . import cmark
from . import foundation
from . import libcxx
from . import libdispatch
from . import libicu
from . import llbuild
from . import llvm
from . import product
from . import swift
from . import xctest
from .. import shell
from .. import targets


# SwiftDriver is a standalone compiler-driver application written in
# Swift. This build product is *the* driver product that is
# installed into a resulting toolchain. It is built-with and depends-on
# other build products of this build (compiler, package-manager, etc).
class SwiftDriver(product.Product):
    @classmethod
    def product_source_name(cls):
        return "swift-driver"

    @classmethod
    def is_build_script_impl_product(cls):
        return False

    @classmethod
    def is_before_build_script_impl_product(cls):
        return False

    def should_build(self, host_target):
        return self.args.build_swift_driver

    @classmethod
    def get_dependencies(cls):
        return [cmark.CMark,
                llvm.LLVM,
                libcxx.LibCXX,
                libicu.LibICU,
                swift.Swift,
                libdispatch.LibDispatch,
                foundation.Foundation,
                xctest.XCTest,
                llbuild.LLBuild]

    def should_clean(self, host_target):
        return self.args.clean_swift_driver

    def clean(self, host_target):
        run_build_script_helper('clean', host_target, self, self.args)

    def build(self, host_target):
        run_build_script_helper('build', host_target, self, self.args)

    def should_test(self, host_target):
        return self.args.test_swift_driver

    def test(self, host_target):
        run_build_script_helper('test', host_target, self, self.args)

    def should_install(self, host_target):
        return self.args.install_swift_driver

    def install(self, host_target):
        run_build_script_helper('install', host_target, self, self.args)


def run_build_script_helper(action, host_target, product, args):
    build_root = os.path.dirname(product.build_dir)
    script_path = os.path.join(
        product.source_dir, 'Utilities', 'build-script-helper.py')

    install_destdir = product.host_install_destdir(host_target)
    toolchain_path = product.native_toolchain_path(host_target)

    # Pass Dispatch directory down if we built it
    dispatch_build_dir = os.path.join(
        build_root, '%s-%s' % ('libdispatch', host_target))

    # Pass the swift lit tests if we're testing and the Swift tests were built
    swift_build_dir = os.path.join(
        build_root, 'swift-{}'.format(host_target))
    lit_test_dir = os.path.join(
        swift_build_dir, 'test-{}'.format(host_target))

    is_release = product.is_release()
    configuration = 'release' if is_release else 'debug'
    helper_cmd = [
        script_path,
        action,
        '--package-path', product.source_dir,
        '--build-path', product.build_dir,
        '--configuration', configuration,
        '--toolchain', toolchain_path,
        '--ninja-bin', product.toolchain.ninja,
        '--cmake-bin', product.toolchain.cmake,
    ]
    if os.path.exists(dispatch_build_dir):
        helper_cmd += [
            '--dispatch-build-dir', dispatch_build_dir
        ]
    if os.path.exists(lit_test_dir) and action == 'test':
        helper_cmd += [
            '--lit-test-dir', lit_test_dir
        ]
    # Pass Cross compile host info
    if product.has_cross_compile_hosts():
        if product.is_darwin_host(host_target):
            helper_cmd += ['--cross-compile-hosts']
            for cross_compile_host in args.cross_compile_hosts:
                helper_cmd += [cross_compile_host]
        elif product.is_cross_compile_target(host_target):
            helper_cmd += ['--cross-compile-hosts', host_target]
            build_toolchain_path = install_destdir + args.install_prefix
            resource_dir = '%s/lib/swift' % build_toolchain_path
            helper_cmd += [
                '--cross-compile-config',
                targets.StdlibDeploymentTarget.get_target_for_name(
                    host_target).platform.swiftpm_config(
                    args, output_dir=build_toolchain_path,
                    swift_toolchain=toolchain_path, resource_path=resource_dir)]

    if args.enable_asan:
        helper_cmd.append('--enable-asan')

    if args.verbose_build:
        helper_cmd.append('--verbose')

    if action == 'install':
        helper_cmd += [
            '--prefix', install_destdir + args.install_prefix
        ]

    shell.call(helper_cmd)
