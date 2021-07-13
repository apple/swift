# swift_build_support/products/product.py -----------------------*- python -*-
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2021 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
# ----------------------------------------------------------------------------

import os

from . import product
from .. import cmake
from .. import shell


class CMakeProduct(product.Product):
    def build_with_cmake(self, build_targets, build_type, build_args,
                         prefer_just_built_toolchain = False):
        assert self.toolchain.cmake is not None
        cmake_build = []
        _cmake = cmake.CMake(self.args, self.toolchain,
                             prefer_just_built_toolchain)

        if self.toolchain.distcc_pump:
            cmake_build.append(self.toolchain.distcc_pump)
        cmake_build.extend([self.toolchain.cmake, "--build"])

        generator_output_path = ""
        if self.args.cmake_generator == "Ninja":
            generator_output_path = os.path.join(self.build_dir, "build.ninja")

        cmake_cache_path = os.path.join(self.build_dir, "CMakeCache.txt")
        if self.args.reconfigure or not os.path.isfile(cmake_cache_path) or \
                (generator_output_path and not os.path.isfile(generator_output_path)):
            if not os.path.exists(self.build_dir):
                os.makedirs(self.build_dir)

            # Use `cmake-file-api` in case it is available.
            query_dir = os.path.join(self.build_dir, ".cmake", "api", "v1", "query")
            if not os.path.exists(query_dir):
                os.makedirs(query_dir)
            open(os.path.join(query_dir, "codemodel-v2"), 'a').close()
            open(os.path.join(query_dir, "cache-v2"), 'a').close()

            env = None
            if self.toolchain.distcc:
                env = {
                    "DISTCC_HOSTS": "localhost,lzo,cpp"
                }

            with shell.pushd(self.build_dir):
                shell.call([self.toolchain.cmake] + list(self.cmake_options) +
                           list(_cmake.common_options(self)) +
                           self.args.extra_cmake_options + [self.source_dir],
                           env=env)

        if not self.args.skip_build or self.product_name() == "llvm":
            if self.args.cmake_generator == "Xcode":
                # Xcode generator uses "ALL_BUILD" instead of "all".
                # Also, xcodebuild uses -target instead of bare names.
                build_targets = build_targets[:]
                build_targets = [val for target in build_targets
                                 for val in ["-target",
                                             target if target != "all"
                                             else "ALL_BUILD"]]

                # Xcode can't restart itself if it turns out we need to reconfigure.
                # Do an advance build to handle that.
                shell.call(cmake_build + [self.build_dir, "--config", build_type])

            shell.call(cmake_build + [self.build_dir, "--config", build_type, "--"]
                       + build_args + build_targets)

    def test_with_cmake(self, executable_target, results_targets,
                        build_type, build_args):
        assert self.toolchain.cmake is not None
        cmake_build = []

        if self.toolchain.distcc_pump:
            cmake_build.append(self.toolchain.distcc_pump)
        cmake_args = [self.toolchain.cmake, "--build", self.build_dir,
                      "--config", build_type, "--"]
        cmake_build.extend(cmake_args + build_args)

        def target_flag(target):
            if self.args.cmake_generator == "Xcode":
                return ["-target", target]
            return [target]

        if executable_target:
            shell.call(cmake_build + target_flag(executable_target))

        for target in results_targets:
            if target:
                test_target = target
                print("--- %s ---" % target)
                if test_target.startswith("check-swift") and self.args.test_paths:
                    test_target = test_target + "-custom"

                shell.call(cmake_build + target_flag(test_target))

                print("--- %s finished ---" % target)

    def install_with_cmake(self, install_targets, install_destdir):
        assert self.toolchain.cmake is not None
        cmake_build = []

        if self.toolchain.distcc_pump:
            cmake_build.append(self.toolchain.distcc_pump)
        cmake_args = [self.toolchain.cmake, "--build", self.build_dir, "--"]
        cmake_build.extend(cmake_args + install_targets)

        environment = {'DESTDIR': install_destdir}
        shell.call(cmake_build, env=environment)
