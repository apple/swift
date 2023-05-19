# tests/products/test_swift.py ----------------------------------*- python -*-
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
# ----------------------------------------------------------------------------

import argparse
import os
import shutil
import sys
import tempfile
import unittest
from io import StringIO

from swift_build_support import shell
from swift_build_support.products import Swift
from swift_build_support.toolchain import host_toolchain
from swift_build_support.workspace import Workspace


class SwiftTestCase(unittest.TestCase):

    def setUp(self):
        # Setup workspace
        tmpdir1 = os.path.realpath(tempfile.mkdtemp())
        tmpdir2 = os.path.realpath(tempfile.mkdtemp())
        os.makedirs(os.path.join(tmpdir1, 'swift'))

        self.workspace = Workspace(source_root=tmpdir1,
                                   build_root=tmpdir2)

        # Setup toolchain
        self.toolchain = host_toolchain()
        self.toolchain.cc = '/path/to/cc'
        self.toolchain.cxx = '/path/to/cxx'

        # Setup args
        self.args = argparse.Namespace(
            enable_tsan_runtime=False,
            compiler_vendor='none',
            swift_compiler_version=None,
            clang_compiler_version=None,
            swift_user_visible_version=None,
            darwin_deployment_version_osx="10.9",
            benchmark=False,
            benchmark_num_onone_iterations=3,
            benchmark_num_o_iterations=3,
            disable_guaranteed_normal_arguments=True,
            force_optimized_typechecker=False,
            enable_stdlibcore_exclusivity_checking=False,
            enable_experimental_differentiable_programming=False,
            enable_experimental_concurrency=False,
            enable_experimental_cxx_interop=False,
            enable_cxx_interop_swift_bridging_header=False,
            enable_experimental_distributed=False,
            enable_experimental_observation=False,
            swift_enable_backtracing=False,
            build_early_swiftsyntax=False,
            build_swift_stdlib_static_print=False,
            build_swift_stdlib_unicode_data=True,
            swift_freestanding_is_darwin=False,
            build_swift_private_stdlib=True,
            swift_tools_ld64_lto_codegen_only_for_supporting_targets=False)

        # Setup shell
        shell.dry_run = True
        self._orig_stdout = sys.stdout
        self._orig_stderr = sys.stderr
        self.stdout = StringIO()
        self.stderr = StringIO()
        sys.stdout = self.stdout
        sys.stderr = self.stderr

    def tearDown(self):
        shutil.rmtree(self.workspace.build_root)
        shutil.rmtree(self.workspace.source_root)
        sys.stdout = self._orig_stdout
        sys.stderr = self._orig_stderr
        shell.dry_run = False
        self.workspace = None
        self.toolchain = None
        self.args = None

    def test_by_default_no_cmake_options(self):
        swift = Swift(
            args=self.args,
            toolchain=self.toolchain,
            source_dir='/path/to/src',
            build_dir='/path/to/build')
        expected = [
            '-DCMAKE_EXPORT_COMPILE_COMMANDS=TRUE',
            '-DSWIFT_FORCE_OPTIMIZED_TYPECHECKER:BOOL=FALSE',
            '-DSWIFT_STDLIB_ENABLE_STDLIBCORE_EXCLUSIVITY_CHECKING:BOOL=FALSE',
            '-DSWIFT_ENABLE_EXPERIMENTAL_DIFFERENTIABLE_PROGRAMMING:BOOL=FALSE',
            '-DSWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY:BOOL=FALSE',
            '-DSWIFT_ENABLE_EXPERIMENTAL_CXX_INTEROP:BOOL=FALSE',
            '-DSWIFT_ENABLE_CXX_INTEROP_SWIFT_BRIDGING_HEADER:BOOL=FALSE',
            '-DSWIFT_ENABLE_EXPERIMENTAL_DISTRIBUTED:BOOL=FALSE',
            '-DSWIFT_ENABLE_EXPERIMENTAL_OBSERVATION:BOOL=FALSE',
            '-DSWIFT_ENABLE_BACKTRACING:BOOL=FALSE',
            '-DSWIFT_STDLIB_STATIC_PRINT=FALSE',
            '-DSWIFT_FREESTANDING_IS_DARWIN:BOOL=FALSE',
            '-DSWIFT_STDLIB_BUILD_PRIVATE:BOOL=TRUE',
            '-DSWIFT_STDLIB_ENABLE_UNICODE_DATA=TRUE',
            '-DSWIFT_TOOLS_LD64_LTO_CODEGEN_ONLY_FOR_SUPPORTING_TARGETS:BOOL=FALSE',
        ]
        self.assertEqual(set(swift.cmake_options), set(expected))

    def test_swift_runtime_tsan(self):
        self.args.enable_tsan_runtime = True
        swift = Swift(
            args=self.args,
            toolchain=self.toolchain,
            source_dir='/path/to/src',
            build_dir='/path/to/build')
        flags_set = [
            '-DSWIFT_RUNTIME_USE_SANITIZERS=Thread',
            '-DCMAKE_EXPORT_COMPILE_COMMANDS=TRUE',
            '-DSWIFT_FORCE_OPTIMIZED_TYPECHECKER:BOOL=FALSE',
            '-DSWIFT_STDLIB_ENABLE_STDLIBCORE_EXCLUSIVITY_CHECKING:BOOL=FALSE',
            '-DSWIFT_ENABLE_EXPERIMENTAL_DIFFERENTIABLE_PROGRAMMING:BOOL=FALSE',
            '-DSWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY:BOOL=FALSE',
            '-DSWIFT_ENABLE_EXPERIMENTAL_CXX_INTEROP:BOOL=FALSE',
            '-DSWIFT_ENABLE_CXX_INTEROP_SWIFT_BRIDGING_HEADER:BOOL=FALSE',
            '-DSWIFT_ENABLE_EXPERIMENTAL_DISTRIBUTED:BOOL=FALSE',
            '-DSWIFT_ENABLE_EXPERIMENTAL_OBSERVATION:BOOL=FALSE',
            '-DSWIFT_ENABLE_BACKTRACING:BOOL=FALSE',
            '-DSWIFT_STDLIB_STATIC_PRINT=FALSE',
            '-DSWIFT_FREESTANDING_IS_DARWIN:BOOL=FALSE',
            '-DSWIFT_STDLIB_BUILD_PRIVATE:BOOL=TRUE',
            '-DSWIFT_STDLIB_ENABLE_UNICODE_DATA=TRUE',
            '-DSWIFT_TOOLS_LD64_LTO_CODEGEN_ONLY_FOR_SUPPORTING_TARGETS:BOOL=FALSE',
        ]
        self.assertEqual(set(swift.cmake_options), set(flags_set))

    def test_swift_compiler_vendor_flags(self):
        self.args.compiler_vendor = "none"
        self.args.swift_user_visible_version = None
        self.args.swift_compiler_version = None
        swift = Swift(
            args=self.args,
            toolchain=self.toolchain,
            source_dir='/path/to/src',
            build_dir='/path/to/build')
        self.assertListEqual(
            [], [x for x in swift.cmake_options if 'SWIFT_VENDOR' in x])
        self.assertListEqual(
            [], [x for x in swift.cmake_options if 'SWIFT_VENDOR_UTI' in x])
        self.assertListEqual(
            [], [x for x in swift.cmake_options if 'SWIFT_VERSION' in x])
        self.assertListEqual(
            [],
            [x for x in swift.cmake_options if 'SWIFT_COMPILER_VERSION' in x])

        self.args.compiler_vendor = "apple"
        self.args.swift_user_visible_version = "1.3"
        self.args.swift_compiler_version = None
        swift = Swift(
            args=self.args,
            toolchain=self.toolchain,
            source_dir='/path/to/src',
            build_dir='/path/to/build')
        self.assertIn('-DSWIFT_VENDOR=Apple', swift.cmake_options)
        self.assertIn(
            '-DSWIFT_VENDOR_UTI=com.apple.compilers.llvm.swift',
            swift.cmake_options)
        self.assertIn('-DSWIFT_VERSION=1.3', swift.cmake_options)
        self.assertIn('-DSWIFT_COMPILER_VERSION=', swift.cmake_options)

        self.args.compiler_vendor = "apple"
        self.args.swift_user_visible_version = "1.3"
        self.args.swift_compiler_version = "2.3"
        swift = Swift(
            args=self.args,
            toolchain=self.toolchain,
            source_dir='/path/to/src',
            build_dir='/path/to/build')
        self.assertIn('-DSWIFT_VENDOR=Apple', swift.cmake_options)
        self.assertIn(
            '-DSWIFT_VENDOR_UTI=com.apple.compilers.llvm.swift',
            swift.cmake_options)
        self.assertIn('-DSWIFT_VERSION=1.3', swift.cmake_options)
        self.assertIn('-DSWIFT_COMPILER_VERSION=2.3', swift.cmake_options)

        self.args.compiler_vendor = "unknown"
        with self.assertRaises(RuntimeError):
            swift = Swift(
                args=self.args,
                toolchain=self.toolchain,
                source_dir='/path/to/src',
                build_dir='/path/to/build')

    def test_version_flags(self):
        # First make sure that by default, we do not get any version flags.
        swift = Swift(
            args=self.args,
            toolchain=self.toolchain,
            source_dir='/path/to/src',
            build_dir='/path/to/build')
        self.assertListEqual(
            [],
            [x for x in swift.cmake_options if 'SWIFT_COMPILER_VERSION' in x]
        )
        self.assertListEqual(
            [],
            [x for x in swift.cmake_options if 'CLANG_COMPILER_VERSION' in x]
        )

        self.args.swift_compiler_version = "3.0"
        self.args.clang_compiler_version = None
        swift = Swift(
            args=self.args,
            toolchain=self.toolchain,
            source_dir='/path/to/src',
            build_dir='/path/to/build')
        self.assertListEqual(
            ['-DSWIFT_COMPILER_VERSION=3.0'],
            [x for x in swift.cmake_options if 'SWIFT_COMPILER_VERSION' in x]
        )
        self.assertListEqual(
            [],
            [x for x in swift.cmake_options if 'CLANG_COMPILER_VERSION' in x]
        )

        self.args.swift_compiler_version = None
        self.args.clang_compiler_version = "3.8.0"
        swift = Swift(
            args=self.args,
            toolchain=self.toolchain,
            source_dir='/path/to/src',
            build_dir='/path/to/build')
        self.assertListEqual(
            [],
            [x for x in swift.cmake_options if 'SWIFT_COMPILER_VERSION' in x]
        )
        self.assertListEqual(
            ['-DCLANG_COMPILER_VERSION=3.8.0'],
            [x for x in swift.cmake_options if 'CLANG_COMPILER_VERSION' in x]
        )

        self.args.swift_compiler_version = "1.0"
        self.args.clang_compiler_version = "1.9.3"
        swift = Swift(
            args=self.args,
            toolchain=self.toolchain,
            source_dir='/path/to/src',
            build_dir='/path/to/build')
        self.assertListEqual(
            ['-DSWIFT_COMPILER_VERSION=1.0'],
            [x for x in swift.cmake_options if 'SWIFT_COMPILER_VERSION' in x]
        )
        self.assertListEqual(
            ['-DCLANG_COMPILER_VERSION=1.9.3'],
            [x for x in swift.cmake_options if 'CLANG_COMPILER_VERSION' in x]
        )
        self.args.swift_compiler_version = None
        self.args.clang_compiler_version = None

    def test_benchmark_flags(self):
        self.args.benchmark = True
        swift = Swift(
            args=self.args,
            toolchain=self.toolchain,
            source_dir='/path/to/src',
            build_dir='/path/to/build')
        # By default, we should get an argument of 3 for both -Onone and -O
        self.assertEqual(
            ['-DSWIFT_BENCHMARK_NUM_ONONE_ITERATIONS=3',
             '-DSWIFT_BENCHMARK_NUM_O_ITERATIONS=3'],
            [x for x in swift.cmake_options if 'SWIFT_BENCHMARK_NUM' in x])

        self.args.benchmark_num_onone_iterations = 20
        swift = Swift(
            args=self.args,
            toolchain=self.toolchain,
            source_dir='/path/to/src',
            build_dir='/path/to/build')
        self.assertEqual(
            ['-DSWIFT_BENCHMARK_NUM_ONONE_ITERATIONS=20',
             '-DSWIFT_BENCHMARK_NUM_O_ITERATIONS=3'],
            [x for x in swift.cmake_options if 'SWIFT_BENCHMARK_NUM' in x])
        self.args.benchmark_num_onone_iterations = 3

        self.args.benchmark_num_o_iterations = 30
        swift = Swift(
            args=self.args,
            toolchain=self.toolchain,
            source_dir='/path/to/src',
            build_dir='/path/to/build')
        self.assertEqual(
            ['-DSWIFT_BENCHMARK_NUM_ONONE_ITERATIONS=3',
             '-DSWIFT_BENCHMARK_NUM_O_ITERATIONS=30'],
            [x for x in swift.cmake_options if 'SWIFT_BENCHMARK_NUM' in x])
        self.args.benchmark_num_onone_iterations = 3

        self.args.benchmark_num_onone_iterations = 10
        self.args.benchmark_num_o_iterations = 25
        swift = Swift(
            args=self.args,
            toolchain=self.toolchain,
            source_dir='/path/to/src',
            build_dir='/path/to/build')
        self.assertEqual(
            ['-DSWIFT_BENCHMARK_NUM_ONONE_ITERATIONS=10',
             '-DSWIFT_BENCHMARK_NUM_O_ITERATIONS=25'],
            [x for x in swift.cmake_options if 'SWIFT_BENCHMARK_NUM' in x])

    def test_force_optimized_typechecker_flags(self):
        self.args.force_optimized_typechecker = True
        swift = Swift(
            args=self.args,
            toolchain=self.toolchain,
            source_dir='/path/to/src',
            build_dir='/path/to/build')
        self.assertEqual(
            ['-DSWIFT_FORCE_OPTIMIZED_TYPECHECKER:BOOL=TRUE'],
            [x for x in swift.cmake_options
             if 'SWIFT_FORCE_OPTIMIZED_TYPECHECKER' in x])

    def test_exclusivity_checking_flags(self):
        self.args.enable_stdlibcore_exclusivity_checking = True
        swift = Swift(
            args=self.args,
            toolchain=self.toolchain,
            source_dir='/path/to/src',
            build_dir='/path/to/build')
        self.assertEqual(
            ['-DSWIFT_STDLIB_ENABLE_STDLIBCORE_EXCLUSIVITY_CHECKING:BOOL='
             'TRUE'],
            [x for x in swift.cmake_options
             if 'SWIFT_STDLIB_ENABLE_STDLIBCORE_EXCLUSIVITY_CHECKING' in x])

    def test_experimental_differentiable_programming_flags(self):
        self.args.enable_experimental_differentiable_programming = True
        swift = Swift(
            args=self.args,
            toolchain=self.toolchain,
            source_dir='/path/to/src',
            build_dir='/path/to/build')
        self.assertEqual(
            ['-DSWIFT_ENABLE_EXPERIMENTAL_DIFFERENTIABLE_PROGRAMMING:BOOL='
             'TRUE'],
            [x for x in swift.cmake_options
             if 'DSWIFT_ENABLE_EXPERIMENTAL_DIFFERENTIABLE_PROGRAMMING' in x])

    def test_experimental_concurrency_flags(self):
        self.args.enable_experimental_concurrency = True
        swift = Swift(
            args=self.args,
            toolchain=self.toolchain,
            source_dir='/path/to/src',
            build_dir='/path/to/build')
        self.assertEqual(
            ['-DSWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY:BOOL='
             'TRUE'],
            [x for x in swift.cmake_options
             if 'DSWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY' in x])

    def test_experimental_cxx_interop_flags(self):
        self.args.enable_experimental_cxx_interop = True
        swift = Swift(
            args=self.args,
            toolchain=self.toolchain,
            source_dir='/path/to/src',
            build_dir='/path/to/build')
        self.assertEqual(
            ['-DSWIFT_ENABLE_EXPERIMENTAL_CXX_INTEROP:BOOL=TRUE'],
            [option for option in swift.cmake_options
             if 'DSWIFT_ENABLE_EXPERIMENTAL_CXX_INTEROP' in option])

    def test_experimental_cxx_interop_bridging_header_flags(self):
        self.args.enable_cxx_interop_swift_bridging_header = True
        swift = Swift(
            args=self.args,
            toolchain=self.toolchain,
            source_dir='/path/to/src',
            build_dir='/path/to/build')
        self.assertEqual(
            ['-DSWIFT_ENABLE_CXX_INTEROP_SWIFT_BRIDGING_HEADER:BOOL=TRUE'],
            [option for option in swift.cmake_options
             if 'DSWIFT_ENABLE_CXX_INTEROP_SWIFT_BRIDGING_HEADER' in option])

    def test_experimental_distributed_flags(self):
        self.args.enable_experimental_distributed = True
        swift = Swift(
            args=self.args,
            toolchain=self.toolchain,
            source_dir='/path/to/src',
            build_dir='/path/to/build')
        self.assertEqual(
            ['-DSWIFT_ENABLE_EXPERIMENTAL_DISTRIBUTED:BOOL='
             'TRUE'],
            [x for x in swift.cmake_options
             if 'DSWIFT_ENABLE_EXPERIMENTAL_DISTRIBUTED' in x])

    def test_experimental_observation_flags(self):
        self.args.enable_experimental_observation = True
        swift = Swift(
            args=self.args,
            toolchain=self.toolchain,
            source_dir='/path/to/src',
            build_dir='/path/to/build')
        self.assertEqual(
            ['-DSWIFT_ENABLE_EXPERIMENTAL_OBSERVATION:BOOL='
             'TRUE'],
            [x for x in swift.cmake_options
             if 'DSWIFT_ENABLE_EXPERIMENTAL_OBSERVATION' in x])

    def test_backtracing_flags(self):
        self.args.swift_enable_backtracing = True
        swift = Swift(
            args=self.args,
            toolchain=self.toolchain,
            source_dir='/path/to/src',
            build_dir='/path/to/build')
        self.assertEqual(
            ['-DSWIFT_ENABLE_BACKTRACING:BOOL='
             'TRUE'],
            [x for x in swift.cmake_options
             if 'DSWIFT_ENABLE_BACKTRACING' in x])

    def test_freestanding_is_darwin_flags(self):
        self.args.swift_freestanding_is_darwin = True
        swift = Swift(
            args=self.args,
            toolchain=self.toolchain,
            source_dir='/path/to/src',
            build_dir='/path/to/build')
        self.assertEqual(
            ['-DSWIFT_FREESTANDING_IS_DARWIN:BOOL='
             'TRUE'],
            [x for x in swift.cmake_options
                if 'SWIFT_FREESTANDING_IS_DARWIN' in x])

    def test_build_swift_private_stdlib_flags(self):
        self.args.build_swift_private_stdlib = False
        swift = Swift(
            args=self.args,
            toolchain=self.toolchain,
            source_dir='/path/to/src',
            build_dir='/path/to/build')
        self.assertEqual(
            ['-DSWIFT_STDLIB_BUILD_PRIVATE:BOOL='
             'FALSE'],
            [x for x in swift.cmake_options
                if 'SWIFT_STDLIB_BUILD_PRIVATE' in x])

    def test_swift_tools_ld64_lto_codegen_only_for_supporting_targets(self):
        self.args.swift_tools_ld64_lto_codegen_only_for_supporting_targets = True
        swift = Swift(
            args=self.args,
            toolchain=self.toolchain,
            source_dir='/path/to/src',
            build_dir='/path/to/build')
        self.assertEqual(
            ['-DSWIFT_TOOLS_LD64_LTO_CODEGEN_ONLY_FOR_SUPPORTING_TARGETS:BOOL='
             'TRUE'],
            [x for x in swift.cmake_options
                if 'SWIFT_TOOLS_LD64_LTO_CODEGEN_ONLY_FOR_SUPPORTING_TARGETS' in x])
