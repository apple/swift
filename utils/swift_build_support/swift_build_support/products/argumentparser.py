# swift_build_support/products/argumentparser.py -----------------*- python -*-
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

from . import foundation
from . import libdispatch
from . import swift
from . import xctest


class ArgumentParser(product.Product):
    @classmethod
    def product_source_name(cls):
        return "swift-argument-parser"

    @classmethod
    def is_build_script_impl_product(cls):
        return False

    def should_build(self, host_target):
        return self.args.build_swift_argument_parser

    def get_dependencies(cls):
        return [
            foundation.Foundation,
            libdispatch.LibDispatch,
            swift.Swift,
            xctest.XCTest,
        ]

    def build(self, host_target):
        toolchain_path = targets.toolchain_path(self.args.install_destdir,
                                                self.args.install_prefix)
        swiftc = os.path.join(toolchain_path, 'bin', 'swiftc')

        # FIXME: this is a workaround for CMake <3.16 which does not correctly
        # generate the build rules if you are not in the build directory.
        try:
            shell.makedirs(self.build_dir)
        except OSError:
            pass

        with shell.pushd(self.build_dir):
            build_root = os.path.dirname(self.build_dir)
            dispatch_build_dir = \
                    os.path.join(build_root, "{}-{}".format("dispatch", host_target)
            foundation_build_dir = \
                    os.path.join(build_root, "{}-{}".format("foundation", host_target)
            xctest_build_Dir = \
                    os.path.join(build_root, "{}-{}".format("xctest", host_target)

            shell.call([
                self.toolchain.cmake,
                '-B', self.build_dir,
                '-D', "CMAKE_MAKE_PROGRAM={}".format(self.toolchain.ninja),
                '-D', "CMAKE_Swift_COMPILER={}".format(swiftc),
                '-D', "dispatch_DIR={}".format(dispatch_build_dir),
                '-D', "Foundation_DIR={}".format(foundation_build_dir),
                '-D', "XCTest_DIR={}".format(xctest_build_dir)
            shell.call([
                self.toolchain.cmake,
                "--build", self.build_dir,
            ])
