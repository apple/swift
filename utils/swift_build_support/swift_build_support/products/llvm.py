# swift_build_support/products/llvm.py --------------------------*- python -*-
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

from . import cmark
from . import product
from ..cmake import CMakeOptions


class LLVM(product.Product):

    def __init__(self, args, toolchain, source_dir, build_dir):
        product.Product.__init__(self, args, toolchain, source_dir,
                                 build_dir)

        # Add the cmake option for enabling or disabling assertions.
        self.cmake_options.define(
            'LLVM_ENABLE_ASSERTIONS:BOOL', args.llvm_assertions)

        # Add the cmake option for LLVM_TARGETS_TO_BUILD.
        self.cmake_options.define(
            'LLVM_TARGETS_TO_BUILD', args.llvm_targets_to_build)

        # Add the cmake options for vendors
        self.cmake_options.extend(self._compiler_vendor_flags)

        # Add the cmake options for compiler version information.
        self.cmake_options.extend(self._version_flags)

    @classmethod
    def is_build_script_impl_product(cls):
        """is_build_script_impl_product -> bool

        Whether this product is produced by build-script-impl.
        """
        return True

    @classmethod
    def is_before_build_script_impl_product(cls):
        """is_before_build_script_impl_product -> bool

        Whether this product is build before any build-script-impl products.
        """
        return False

    @property
    def _compiler_vendor_flags(self):
        if self.args.compiler_vendor == "none":
            return []

        if self.args.compiler_vendor == "swiftwasm":
            return [
                ('CLANG_VENDOR', 'SwiftWasm'),
                ('CLANG_VENDOR_UTI', 'org.swiftwasm.compilers.llvm.clang'),
                ('PACKAGE_VERSION', str(self.args.clang_user_visible_version))
            ]

        if self.args.compiler_vendor != "apple":
            raise RuntimeError("Unknown compiler vendor?!")

        return [
            ('CLANG_VENDOR', 'Apple'),
            ('CLANG_VENDOR_UTI', 'com.apple.compilers.llvm.clang'),
            # This is safe since we always provide a default.
            ('PACKAGE_VERSION', str(self.args.clang_user_visible_version))
        ]

    @property
    def _version_flags(self):
        result = CMakeOptions()
        if self.args.clang_compiler_version is not None:
            result.define(
                'CLANG_REPOSITORY_STRING',
                "clang-{}".format(self.args.clang_compiler_version))
        return result

    @classmethod
    def get_dependencies(cls):
        return [cmark.CMark]
