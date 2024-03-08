# swift_build_support/products/swiftcompatibilitylibs.py --------*- python -*-
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
# ----------------------------------------------------------------------------

from . import cmake_product


# Build against the current installed toolchain.
class SwiftCompatibilityLibs(cmake_product.CMakeProduct):
    @classmethod
    def product_source_name(cls):
        return "swift-compatibility-libs"

    @classmethod
    def is_build_script_impl_product(cls):
        return False

    @classmethod
    def is_before_build_script_impl_product(cls):
        return False

    def should_build(self, host_target):
        return True

    def build(self, host_target):
        build_variant = 'RelWithDebInfo'
        self.build_with_cmake(["all"], build_variant, [], prefer_just_built_toolchain=True)

    def should_test(self, host_target):
        return False

    def test(self, host_target):
        """Just run a single instance of the command for both .debug and
           .release.
        """
        pass

    def should_install(self, host_target):
        return self.args.install_swift_compatibility_libs

    def install(self, host_target):
        install_destdir = self.host_install_destdir(host_target)
        self.install_with_cmake(["install"], install_destdir + self.args.install_prefix)

    @classmethod
    def get_dependencies(cls):
        return []
