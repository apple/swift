# swift_build_support/products/swift.py -------------------------*- python -*-
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
from . import earlyswiftdriver
from . import libcxx
from . import llvm
from . import product
from ..cmake import CMakeOptions


class Swift(product.Product):

    def __init__(self, args, toolchain, source_dir, build_dir):
        product.Product.__init__(self, args, toolchain, source_dir,
                                 build_dir)
        # Add any runtime sanitizer arguments.
        self.cmake_options.extend(self._runtime_sanitizer_flags)

        # Add any compiler vendor cmake flags.
        self.cmake_options.extend(self._compiler_vendor_flags)

        # Add any swift version related cmake flags.
        self.cmake_options.extend(self._version_flags)

        # Add benchmark specific flags.
        self.cmake_options.extend(self._benchmark_flags)

        # Generate the compile db.
        self.cmake_options.extend(self._compile_db_flags)

        # Add the flag if we are supposed to force the typechecker to compile
        # with optimization.
        self.cmake_options.extend(self._force_optimized_typechecker_flags)

        # Add any exclusivity checking flags for stdlibcore.
        self.cmake_options.extend(self._stdlibcore_exclusivity_checking_flags)

        # Add experimental differentiable programming flag.
        self.cmake_options.extend(
            self._enable_experimental_differentiable_programming)

        # Add experimental concurrency flag.
        self.cmake_options.extend(self._enable_experimental_concurrency)

        # Add experimental cxx interop flags.
        self.cmake_options.extend(self._enable_experimental_cxx_interop)
        self.cmake_options.extend(self._enable_cxx_interop_swift_bridging_header)

        # Add experimental distributed flag.
        self.cmake_options.extend(self._enable_experimental_distributed)

        # Add backtracing flag.
        self.cmake_options.extend(self._enable_backtracing)

        # Add experimental observation flag.
        self.cmake_options.extend(self._enable_experimental_observation)

        # Add static vprintf flag
        self.cmake_options.extend(self._enable_stdlib_static_vprintf)

        # Add freestanding related flags.
        self.cmake_options.extend(self._freestanding_is_darwin)

        self.cmake_options.extend(self._build_swift_private_stdlib)

        self.cmake_options.extend(self._enable_stdlib_unicode_data)

        self.cmake_options.extend(
            self._swift_tools_ld64_lto_codegen_only_for_supporting_targets)

    @classmethod
    def is_build_script_impl_product(cls):
        """is_build_script_impl_product -> bool

        Whether this product is produced by build-script-impl.
        """
        return True

    @classmethod
    def is_before_build_script_impl_product(cls):
        return False

    @property
    def _runtime_sanitizer_flags(self):
        sanitizer_list = []
        if self.args.enable_tsan_runtime:
            sanitizer_list += ['Thread']
        if len(sanitizer_list) == 0:
            return []
        return [('SWIFT_RUNTIME_USE_SANITIZERS', ';'.join(sanitizer_list))]

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
            ('SWIFT_VENDOR', 'Apple'),
            ('SWIFT_VENDOR_UTI', 'com.apple.compilers.llvm.swift'),

            # This has a default of 3.0, so it should be safe to use here.
            ('SWIFT_VERSION', str(self.args.swift_user_visible_version)),

            # FIXME: We are matching build-script-impl here. But it seems like
            # bit rot since this flag is specified in another place with the
            # exact same value in build-script-impl.
            ('SWIFT_COMPILER_VERSION', str(swift_compiler_version)),
        ]

    @property
    def _version_flags(self):
        r = CMakeOptions()
        if self.args.swift_compiler_version is not None:
            swift_compiler_version = self.args.swift_compiler_version
            r.define('SWIFT_COMPILER_VERSION', str(swift_compiler_version))
        if self.args.clang_compiler_version is not None:
            clang_compiler_version = self.args.clang_compiler_version
            r.define('CLANG_COMPILER_VERSION', str(clang_compiler_version))
        return r

    @property
    def _benchmark_flags(self):
        if not self.args.benchmark:
            return []

        onone_iters = self.args.benchmark_num_onone_iterations
        o_iters = self.args.benchmark_num_o_iterations
        return [
            ('SWIFT_BENCHMARK_NUM_ONONE_ITERATIONS', onone_iters),
            ('SWIFT_BENCHMARK_NUM_O_ITERATIONS', o_iters)
        ]

    @property
    def _compile_db_flags(self):
        return [('CMAKE_EXPORT_COMPILE_COMMANDS', True)]

    @property
    def _force_optimized_typechecker_flags(self):
        return [('SWIFT_FORCE_OPTIMIZED_TYPECHECKER:BOOL',
                 self.args.force_optimized_typechecker)]

    @property
    def _stdlibcore_exclusivity_checking_flags(self):
        return [('SWIFT_STDLIB_ENABLE_STDLIBCORE_EXCLUSIVITY_CHECKING:BOOL',
                 self.args.enable_stdlibcore_exclusivity_checking)]

    @property
    def _enable_experimental_differentiable_programming(self):
        return [('SWIFT_ENABLE_EXPERIMENTAL_DIFFERENTIABLE_PROGRAMMING:BOOL',
                 self.args.enable_experimental_differentiable_programming)]

    @property
    def _enable_experimental_concurrency(self):
        return [('SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY:BOOL',
                 self.args.enable_experimental_concurrency)]

    @property
    def _enable_experimental_cxx_interop(self):
        return [('SWIFT_ENABLE_EXPERIMENTAL_CXX_INTEROP:BOOL',
                 self.args.enable_experimental_cxx_interop)]

    @property
    def _enable_cxx_interop_swift_bridging_header(self):
        return [('SWIFT_ENABLE_CXX_INTEROP_SWIFT_BRIDGING_HEADER:BOOL',
                 self.args.enable_cxx_interop_swift_bridging_header)]

    @property
    def _enable_experimental_distributed(self):
        return [('SWIFT_ENABLE_EXPERIMENTAL_DISTRIBUTED:BOOL',
                 self.args.enable_experimental_distributed)]

    @property
    def _enable_backtracing(self):
        return [('SWIFT_ENABLE_BACKTRACING:BOOL',
                 self.args.swift_enable_backtracing)]

    @property
    def _enable_experimental_observation(self):
        return [('SWIFT_ENABLE_EXPERIMENTAL_OBSERVATION:BOOL',
                 self.args.enable_experimental_observation)]

    @property
    def _enable_stdlib_static_vprintf(self):
        return [('SWIFT_STDLIB_STATIC_PRINT',
                 self.args.build_swift_stdlib_static_print)]

    @property
    def _enable_stdlib_unicode_data(self):
        return [('SWIFT_STDLIB_ENABLE_UNICODE_DATA',
                 self.args.build_swift_stdlib_unicode_data)]

    @property
    def _freestanding_is_darwin(self):
        return [('SWIFT_FREESTANDING_IS_DARWIN:BOOL',
                 self.args.swift_freestanding_is_darwin)]

    @property
    def _build_swift_private_stdlib(self):
        return [('SWIFT_STDLIB_BUILD_PRIVATE:BOOL',
                 self.args.build_swift_private_stdlib)]

    @property
    def _swift_tools_ld64_lto_codegen_only_for_supporting_targets(self):
        return [('SWIFT_TOOLS_LD64_LTO_CODEGEN_ONLY_FOR_SUPPORTING_TARGETS:BOOL',
                 self.args.swift_tools_ld64_lto_codegen_only_for_supporting_targets)]

    @classmethod
    def get_dependencies(cls):
        return [cmark.CMark,
                earlyswiftdriver.EarlySwiftDriver,
                llvm.LLVM,
                libcxx.LibCXX]
