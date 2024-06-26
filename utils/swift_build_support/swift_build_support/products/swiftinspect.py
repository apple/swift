# swift_build_support/products/swiftinspect.py --------------------*- python -*-
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
import platform

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
from .. import targets


# Build against the current installed toolchain.
class SwiftInspect(product.Product):
    @classmethod
    def product_source_name(cls):
        return "swift-inspect"

    @classmethod
    def is_build_script_impl_product(cls):
        return False

    @classmethod
    def is_before_build_script_impl_product(cls):
        return False

    def should_build(self, host_target):
        return True

    def build(self, host_target):
        run_build_script_helper(host_target, self, self.args)

    def should_test(self, host_target):
        return self.args.test_swift_inspect

    def test(self, host_target):
        """Just run a single instance of the command for both .debug and
           .release.
        """
        pass

    def should_install(self, host_target):
        return False

    def install(self, host_target):
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


def _get_toolchain_path(host_target, product, args):
    # this logic initially was inside run_build_script_helper
    # and was factored out so it can be used in testing as well

    install_destdir = args.install_destdir
    if swiftpm.SwiftPM.has_cross_compile_hosts(args):
        install_destdir = swiftpm.SwiftPM.get_install_destdir(args,
                                                              host_target,
                                                              product.build_dir)
    toolchain_path = targets.toolchain_path(install_destdir,
                                            args.install_prefix)

    if platform.system() == 'Darwin':
        # The prefix is an absolute path, so concatenate without os.path.
        toolchain_path += \
            targets.darwin_toolchain_prefix(args.install_prefix)

    return toolchain_path


def run_build_script_helper(host_target, product, args):
    toolchain_path = _get_toolchain_path(host_target, product, args)

    # Our source_dir is expected to be './$SOURCE_ROOT/benchmarks'. That is due
    # the assumption that each product is in its own build directory. This
    # product is not like that and has its package/tools instead in
    # ./$SOURCE_ROOT/swift/benchmark.
    package_path = os.path.join(product.source_dir,
                                '..', 'swift', 'tools', 'swift-inspect')
    package_path = os.path.abspath(package_path)

    # We use a separate python helper to enable quicker iteration when working
    # on this by avoiding going through build-script to test small changes.
    helper_path = os.path.join(package_path, 'build_script_helper.py')

    build_cmd = [
        helper_path,
        '--verbose',
        '--package-path', package_path,
        '--build-path', product.build_dir,
        '--toolchain', toolchain_path,
    ]
    shell.call(build_cmd)
