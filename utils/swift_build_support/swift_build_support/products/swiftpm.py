# swift_build_support/products/swiftpm.py -----------------------*- python -*-
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
from ..targets import StdlibDeploymentTarget


class SwiftPM(product.Product):
    @classmethod
    def product_source_name(cls):
        return "swiftpm"

    @classmethod
    def is_build_script_impl_product(cls):
        return False

    @classmethod
    def is_before_build_script_impl_product(cls):
        return False

    def should_build(self, host_target):
        return True

    def run_bootstrap_script(self, action, host_target, additional_params=[]):
        script_path = os.path.join(
            self.source_dir, 'Utilities', 'bootstrap')

        if self.args.native_swift_tools_path is not None:
            toolchain_path = os.path.split(self.args.native_swift_tools_path)[0]
        else:
            toolchain_path = self.install_toolchain_path(host_target)
        swiftc = os.path.join(toolchain_path, "bin", "swiftc")

        # FIXME: We require llbuild build directory in order to build. Is
        # there a better way to get this?
        build_root = os.path.dirname(self.build_dir)
        llbuild_build_dir = os.path.join(
            build_root, '%s-%s' % ("llbuild", host_target))

        helper_cmd = [script_path, action]

        if action == 'clean':
            helper_cmd += ["--build-dir", self.build_dir]
            shell.call(helper_cmd)
            return

        if self.is_release():
            helper_cmd.append("--release")

        helper_cmd += [
            "--swiftc-path", swiftc,
            "--clang-path", self.toolchain.cc,
            "--cmake-path", self.toolchain.cmake,
            "--ninja-path", self.toolchain.ninja,
            "--build-dir", self.build_dir,
            "--llbuild-build-dir", llbuild_build_dir
        ]

        # Pass Dispatch directory down if we built it
        dispatch_build_dir = os.path.join(
            build_root, '%s-%s' % ("libdispatch", host_target))

        if os.path.exists(dispatch_build_dir):
            helper_cmd += [
                "--dispatch-build-dir", dispatch_build_dir
            ]

        # Pass Foundation directory down if we built it
        foundation_build_dir = os.path.join(
            build_root, '%s-%s' % ("foundation", host_target))

        if os.path.exists(foundation_build_dir):
            helper_cmd += [
                "--foundation-build-dir", foundation_build_dir
            ]

        # Pass Cross compile host info
        if self.has_cross_compile_hosts(self.args):
            helper_cmd += ['--cross-compile-hosts']
            if StdlibDeploymentTarget.get_target_for_name(
                    host_target).platform.is_darwin:
                for cross_compile_host in self.args.cross_compile_hosts:
                    helper_cmd += [cross_compile_host]
            elif host_target != self.args.host_target:
                helper_cmd += [host_target]
                helper_cmd.append("--skip-cmake-bootstrap")
                build_toolchain_path = self.get_install_destdir(
                    self.args, host_target, self.build_dir) + self.args.install_prefix
                resource_dir = '%s/lib/swift' % build_toolchain_path
                helper_cmd += [
                    '--cross-compile-config',
                    StdlibDeploymentTarget.get_target_for_name(host_target).platform
                    .swiftpm_config(self.args, output_dir=build_toolchain_path,
                                    swift_toolchain=toolchain_path,
                                    resource_path=resource_dir)]

        helper_cmd.extend(additional_params)

        shell.call(helper_cmd)

    def build(self, host_target):
        self.run_bootstrap_script('build', host_target, ["--reconfigure"])

    def should_test(self, host_target):
        return self.args.test_swiftpm

    def test(self, host_target):
        self.run_bootstrap_script('test', host_target)

    def should_clean(self, host_target):
        return self.args.clean_swiftpm

    def clean(self, host_target):
        self.run_bootstrap_script('clean', host_target)

    def should_install(self, host_target):
        return self.args.install_swiftpm

    @classmethod
    def has_cross_compile_hosts(self, args):
        return args.cross_compile_hosts

    @classmethod
    def get_install_destdir(self, args, host_target, build_dir):
        install_destdir = args.install_destdir
        if self.has_cross_compile_hosts(args):
            if StdlibDeploymentTarget.get_target_for_name(
                    host_target).platform.is_darwin:
                build_root = os.path.dirname(build_dir)
                install_destdir = '%s/intermediate-install/%s' % (
                                  build_root, host_target)
            elif host_target != args.host_target:
                install_destdir = os.path.join(install_destdir, host_target)
        return install_destdir

    def install(self, host_target):
        install_destdir = self.get_install_destdir(self.args,
                                                   host_target,
                                                   self.build_dir)
        install_prefix = install_destdir + self.args.install_prefix

        self.run_bootstrap_script('install', host_target, [
            '--prefix', install_prefix
        ])

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
