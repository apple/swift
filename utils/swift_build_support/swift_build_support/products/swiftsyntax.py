# swift_build_support/products/swiftsyntax.py --------------------*- python -*-
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

from build_swift.build_swift.constants import MULTIROOT_DATA_FILE_PATH

from . import cmark
from . import foundation
from . import libcxx
from . import libdispatch
from . import libicu
from . import llbuild
from . import llvm
from . import product
from . import swift
from . import swiftpm
from . import xctest
from .. import shell


class SwiftSyntax(product.Product):
    @classmethod
    def product_source_name(cls):
        """product_source_name() -> str

        The name of the source code directory of this product.
        """
        return "swift-syntax"

    @classmethod
    def is_build_script_impl_product(cls):
        return False

    @classmethod
    def is_before_build_script_impl_product(cls):
        return False

    @classmethod
    def is_swiftpm_unified_build_product(cls):
        return True

    def run_swiftsyntax_build_script(self, target, command, additional_params=[]):
        script_path = os.path.join(self.source_dir, 'build-script.py')

        build_cmd = [
            script_path,
            command,
            '--build-dir', self.build_dir,
            '--multiroot-data-file', MULTIROOT_DATA_FILE_PATH,
            '--toolchain', self.install_toolchain_path(target)
        ]

        if self.is_release():
            build_cmd.append('--release')

        if self.args.swiftsyntax_enable_rawsyntax_validation:
            build_cmd.append('--enable-rawsyntax-validation')

        if self.args.swiftsyntax_enable_test_fuzzing:
            build_cmd.append('--enable-test-fuzzing')

        if self.args.verbose_build:
            build_cmd.append('--verbose')

        build_cmd.extend(additional_params)

        shell.call(build_cmd)

    def should_build(self, host_target):
        return True

    def build(self, host_target):
        if self.args.swiftsyntax_verify_generated_files:
            build_cmd = [
                os.path.join(self.source_dir, 'build-script.py'),
                'verify-source-code',
                '--toolchain', self.install_toolchain_path(host_target)
            ]
            if self.args.verbose_build:
                build_cmd.append('--verbose')
            shell.call(build_cmd)

        self.run_swiftsyntax_build_script(target=host_target,
                                          command='build')

    def should_test(self, host_target):
        return self.args.test_swiftsyntax

    def test(self, host_target):
        llvm_build_dir = os.path.join(self.build_dir, '..', 'llvm-' + host_target)
        llvm_build_dir = os.path.realpath(llvm_build_dir)

        self.run_swiftsyntax_build_script(target=host_target,
                                          command='test',
                                          additional_params=[
                                              '--filecheck-exec',
                                              os.path.join(llvm_build_dir,
                                                           'bin',
                                                           'FileCheck')
                                          ])

    def should_install(self, host_target):
        return self.args.install_swiftsyntax

    def install(self, target_name):
        # SwiftSyntax doesn't produce any products that should be installed
        # into the toolchain. All tools using it link against SwiftSyntax
        # statically.
        pass

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
                llbuild.LLBuild,
                swiftpm.SwiftPM]
