# swift_build_support/products/swift.py -------------------------*- python -*-
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See http://swift.org/LICENSE.txt for license information
# See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
# ----------------------------------------------------------------------------

from . import product


class Swift(product.Product):

    def __init__(self, args, toolchain, source_dir, build_dir):
        product.Product.__init__(self, args, toolchain, source_dir,
                                 build_dir)
        # Add any runtime sanitizer arguments.
        self.cmake_options.extend(self._runtime_sanitizer_flags)

        # Add any compiler vendor cmake flags.
        self.cmake_options.extend(self._compiler_vendor_flags)

    @property
    def _runtime_sanitizer_flags(self):
        sanitizer_list = []
        if self.args.enable_tsan_runtime:
            sanitizer_list += ['Thread']
        if len(sanitizer_list) == 0:
            return []
        return ["-DSWIFT_RUNTIME_USE_SANITIZERS=%s" %
                ";".join(sanitizer_list)]

    @property
    def _compiler_vendor_flags(self):
        if self.args.compiler_vendor == "none":
            return []

        if self.args.compiler_vendor != "apple":
            raise RuntimeError("Unknown compiler vendor?! Was build-script \
updated without updating swift.py?")

        swift_compiler_version = ""
        if self.args.swift_compiler_version is not None:
            swift_compiler_version = self.args.swift_compiler_version

        return [
            "-DSWIFT_VENDOR=Apple",
            "-DSWIFT_VENDOR_UTI=com.apple.compilers.llvm.swift",

            # This has a default of 3.0, so it should be safe to use here.
            "-DSWIFT_VERSION={}".format(self.args.swift_user_visible_version),

            # FIXME: We are matching build-script-impl here. But it seems like
            # bit rot since this flag is specified in another place with the
            # exact same value in build-script-impl.
            "-DSWIFT_COMPILER_VERSION={}".format(swift_compiler_version),
        ]

