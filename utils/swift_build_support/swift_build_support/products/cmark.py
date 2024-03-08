# swift_build_support/products/cmark.py -------------------------*- python -*-
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

from . import cmake_product
from . import earlyswiftdriver


class CMark(cmake_product.CMakeProduct):
    @classmethod
    def is_build_script_impl_product(cls):
        """is_build_script_impl_product -> bool

        Whether this product is produced by build-script-impl.
        """
        return False

    @classmethod
    def is_before_build_script_impl_product(cls):
        """is_before_build_script_impl_product -> bool

        Whether this product is built before any build-script-impl products.
        """
        return True

    # EarlySwiftDriver is the root of the graph, and is the only dependency of
    # this product.
    @classmethod
    def get_dependencies(cls):
        return [earlyswiftdriver.EarlySwiftDriver]

    def should_build(self, host_target):
        """should_build() -> Bool

        Whether or not this product should be built with the given arguments.
        """
        return self.args.build_cmark

    def build(self, host_target):
        """build() -> void

        Perform the build, for a non-build-script-impl product.
        """
        self.cmake_options.define('CMAKE_BUILD_TYPE:STRING',
                                  self.args.cmark_build_variant)
        self.cmake_options.define('CMAKE_INSTALL_PREFIX:PATH',
                                  self.args.install_prefix)

        self.cmake_options.define('CMARK_THREADING', 'ON')

        host_toolchain = self.generate_toolchain_file_for_darwin_or_linux(host_target)

        (platform, _) = host_target.split('-')
        if not host_toolchain and platform == "openbsd":
            toolchain_file = self.get_openbsd_toolchain_file()
            if toolchain_file:
                self.cmake_options.define('CMAKE_TOOLCHAIN_FILE:PATH', toolchain_file)

        # cmark_cmake_options=(
        #     -DCMAKE_C_FLAGS="$(cmark_c_flags ${host})"
        #     -DCMAKE_CXX_FLAGS="$(cmark_c_flags ${host})"
        #     -DCMAKE_OSX_SYSROOT:PATH="${cmake_os_sysroot}"
        #     -DCMAKE_OSX_DEPLOYMENT_TARGET="${cmake_osx_deployment_target}"
        #     -DCMAKE_OSX_ARCHITECTURES="${architecture}"
        # )

        self.build_with_cmake(["all"], self.args.cmark_build_variant, [])

    def should_test(self, host_target):
        """should_test() -> Bool

        Whether or not this product should be tested with the given arguments.
        """
        if self.is_cross_compile_target(host_target):
            return False

        return self.args.test_cmark

    def test(self, host_target):
        """
        Perform the test phase for the product.

        This phase might build and execute the product tests.
        """
        executable_target = 'api_test'
        results_targets = ['test']
        if self.args.cmake_generator == 'Xcode':
            # Xcode generator uses "RUN_TESTS" instead of "test".
            results_targets = ['RUN_TESTS']

        test_env = {
            "CTEST_OUTPUT_ON_FAILURE": "ON"
        }

        # see the comment in cmake_product.py if you want to copy this code to pass
        # environment variables to tests
        self.test_with_cmake(executable_target, results_targets,
                             self.args.cmark_build_variant, [], test_env)

    def should_install(self, host_target):
        """should_install() -> Bool

        Whether or not this product should be installed with the given
        arguments.
        """
        return self.args.install_all

    def install(self, host_target):
        """
        Perform the install phase for the product.

        This phase might copy the artifacts from the previous phases into a
        destination directory.
        """
        self.install_with_cmake(["install"], self.host_install_destdir(host_target))
