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

import os
import errno
import shutil
from platform import system

from . import cmake_product
from . import cmark
from .. import shell
from .. import targets
from ..cmake import CMakeOptions
from ..host_specific_configuration \
    import HostSpecificConfiguration


class LLVM(cmake_product.CMakeProduct):

    def __init__(self, args, toolchain, source_dir, build_dir):
        cmake_product.CMakeProduct.__init__(self, args, toolchain, source_dir,
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
        return False

    @classmethod
    def is_before_build_script_impl_product(cls):
        """is_before_build_script_impl_product -> bool

        Whether this product is built before any build-script-impl products.
        """
        return True

    @property
    def _compiler_vendor_flags(self):
        if self.args.compiler_vendor == "none":
            return []

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

    def llvm_c_flags(self, platform, arch):
        result = self.common_cross_c_flags(platform, arch, include_arch=True)
        if self.is_debug_info():
            if self.args.lto_type:
                result.append('-gline-tables-only')
            else:
                result.append('-g')
        return result

    def copy_lib_stripping_architecture(self, source, dest, arch):

        # An alternative approach would be to use || to first
        # attempt the removal of the slice and fall back to the
        # copy when failing.
        # However, this would leave unneeded error messages in the logs
        # that may hinder investigation; in addition, in this scenario
        # the `call` function seems to not propagate correctly failure
        # exit codes.
        if arch in shell.capture(['lipo', '-archs', source], dry_run=False):
            shell.call(['lipo', '-remove', arch, source, '-output', dest])
        else:
            shutil.copy(source, dest)

    def copy_embedded_compiler_rt_builtins_from_darwin_host_toolchain(
            self, clang_dest_dir):
        host_cxx_dir = os.path.dirname(self.toolchain.cxx)
        host_lib_clang_dir = os.path.join(host_cxx_dir, os.pardir, 'lib', 'clang')
        dest_lib_clang_dir = os.path.join(clang_dest_dir, 'lib', 'clang')

        if not os.path.exists(host_lib_clang_dir) or \
           not os.path.exists(dest_lib_clang_dir):
            return 0

        dest_cxx_builtins_version = os.listdir(dest_lib_clang_dir)
        dest_builtins_dir = os.path.join(clang_dest_dir, 'lib', 'clang',
                                         dest_cxx_builtins_version[0],
                                         'lib', 'darwin')

        if os.path.exists(dest_builtins_dir):
            for host_cxx_builtins_path in os.listdir(host_lib_clang_dir):
                host_cxx_builtins_dir = os.path.join(host_lib_clang_dir,
                                                     host_cxx_builtins_path,
                                                     'lib', 'darwin')
                print('copying compiler-rt embedded builtins from {}'
                      ' into the local clang build directory {}.'.format(
                          host_cxx_builtins_dir, dest_builtins_dir))

                for _os in ['ios', 'watchos', 'tvos']:
                    # Copy over the device .a when necessary
                    lib_name = 'libclang_rt.{}.a'.format(_os)
                    host_lib_path = os.path.join(host_cxx_builtins_dir, lib_name)
                    dest_lib_path = os.path.join(dest_builtins_dir, lib_name)
                    if not os.path.isfile(dest_lib_path):
                        if os.path.isfile(host_lib_path):
                            if _os == 'tvos':
                                self.copy_lib_stripping_architecture(host_lib_path,
                                                                     dest_lib_path,
                                                                     'i386')
                            else:
                                shutil.copy(host_lib_path, dest_lib_path)
                        elif self.args.verbose_build:
                            print('no file exists at {}'.format(host_lib_path))

                    # Copy over the simulator .a when necessary
                    sim_lib_name = 'libclang_rt.{}sim.a'.format(_os)
                    host_sim_lib_path = os.path.join(host_cxx_builtins_dir,
                                                     sim_lib_name)
                    dest_sim_lib_path = os.path.join(dest_builtins_dir, sim_lib_name)

                    if not os.path.isfile(dest_sim_lib_path):
                        if os.path.isfile(host_sim_lib_path):
                            if _os == 'tvos':
                                # This is to avoid strip failures when generating
                                # a toolchain
                                self.copy_lib_stripping_architecture(
                                    host_sim_lib_path, dest_sim_lib_path, 'i386')
                            else:
                                shutil.copy(host_sim_lib_path, dest_sim_lib_path)

                        elif os.path.isfile(host_lib_path):
                            # The simulator .a might not exist if the host
                            # Xcode is old. In that case, copy over the
                            # device library to the simulator location to allow
                            # clang to find it. The device library has the simulator
                            # slices in Xcode that doesn't have the simulator .a, so
                            # the link is still valid.
                            print('copying over faux-sim library {} to {}'.format(
                                host_lib_path, sim_lib_name))
                            if _os == 'tvos':
                                print('Remove i386 from tvOS {}'.format(
                                    dest_sim_lib_path))
                                shell.call(['lipo', '-remove', 'i386', host_lib_path,
                                            '-output', dest_sim_lib_path])
                            else:
                                shutil.copy(host_lib_path, dest_sim_lib_path)
                        elif self.args.verbose_build:
                            print('no file exists at {}', host_sim_lib_path)

                os.makedirs(os.path.join(dest_builtins_dir, 'macho_embedded'),
                            exist_ok=True)
                for _flavor in ['hard_pic', 'hard_static', 'soft_pic', 'soft_static']:
                    # Copy over the macho_embedded .a when necessary
                    lib_name = os.path.join('macho_embedded', 'libclang_rt.{}.a'.format(
                        _flavor))
                    host_lib_path = os.path.join(host_cxx_builtins_dir, lib_name)
                    dest_lib_path = os.path.join(dest_builtins_dir, lib_name)
                    if not os.path.isfile(dest_lib_path):
                        if os.path.isfile(host_lib_path):
                            shutil.copy(host_lib_path, dest_lib_path)
                        elif self.args.verbose_build:
                            print('no file exists at {}'.format(host_lib_path))

    def should_build(self, host_target):
        """should_build() -> Bool

        Whether or not this product should be built with the given arguments.
        """
        # LLVM will always be built in part
        return True

    def build(self, host_target):
        """build() -> void

        Perform the build, for a non-build-script-impl product.
        """

        (platform, arch) = host_target.split('-')

        llvm_cmake_options = self.host_cmake_options(host_target)[0]
        llvm_cmake_options.extend_raw(self.args.llvm_cmake_options)

        # TODO: handle cross compilation
        llvm_cmake_options.define('CMAKE_INSTALL_PREFIX:PATH', self.args.install_prefix)
        llvm_cmake_options.define('INTERNAL_INSTALL_PREFIX', 'local')

        if host_target.startswith('linux'):
            toolchain_file = self.generate_linux_toolchain_file(platform, arch)
            llvm_cmake_options.define('CMAKE_TOOLCHAIN_FILE:PATH', toolchain_file)
            if not self.is_release():
                # On Linux build LLVM and subprojects with -gsplit-dwarf which is more
                # space/time efficient than -g on that platform.
                llvm_cmake_options.define('LLVM_USE_SPLIT_DWARF:BOOL', 'YES')

        if not self.args._build_llvm:
            # Indicating we don't want to build LLVM at all should
            # override everything.
            build_targets = []
        elif self.args.skip_build or not self.args.build_llvm:
            # We can't skip the build completely because the standalone
            # build of Swift depends on these.
            build_targets = ['llvm-tblgen', 'clang-resource-headers',
                             'intrinsics_gen', 'clang-tablegen-targets']

            # If we are not performing a toolchain-only build or generating
            # Xcode projects, then we also want to include the following
            # targets for testing purposes.
            if (
                not self.args.build_toolchain_only
                and self.args.cmake_generator != 'Xcode'
            ):
                build_targets.extend([
                    'FileCheck',
                    'not',
                    'llvm-nm',
                    'llvm-size'
                ])
        else:
            build_targets = ['all']

            if self.args.llvm_ninja_targets_for_cross_compile_hosts and \
               self.is_cross_compile_target(host_target):
                build_targets = (self.args.llvm_ninja_targets_for_cross_compile_hosts)
            elif self.args.llvm_ninja_targets:
                build_targets = (self.args.llvm_ninja_targets)

        if self.args.host_libtool:
            llvm_cmake_options.define('CMAKE_LIBTOOL', self.args.host_libtool)

        # Note: we set the variable:
        #
        # LLVM_TOOL_SWIFT_BUILD
        #
        # below because this script builds swift separately, and people
        # often have reasons to symlink the swift directory into
        # llvm/tools, e.g. to build LLDB.

        llvm_c_flags = ' '.join(self.llvm_c_flags(platform, arch))
        llvm_cmake_options.define('CMAKE_C_FLAGS', llvm_c_flags)
        llvm_cmake_options.define('CMAKE_CXX_FLAGS', llvm_c_flags)
        llvm_cmake_options.define('CMAKE_C_FLAGS_RELWITHDEBINFO', '-O2 -DNDEBUG')
        llvm_cmake_options.define('CMAKE_CXX_FLAGS_RELWITHDEBINFO', '-O2 -DNDEBUG')
        llvm_cmake_options.define('CMAKE_BUILD_TYPE:STRING',
                                  self.args.llvm_build_variant)
        llvm_cmake_options.define('LLVM_TOOL_SWIFT_BUILD:BOOL', 'FALSE')
        llvm_cmake_options.define('LLVM_TOOL_LLD_BUILD:BOOL', 'TRUE')
        llvm_cmake_options.define('LLVM_INCLUDE_DOCS:BOOL', 'TRUE')
        llvm_cmake_options.define('LLVM_ENABLE_LTO:STRING', self.args.lto_type)
        llvm_cmake_options.define('COMPILER_RT_INTERCEPT_LIBDISPATCH', 'ON')

        llvm_enable_projects = ['clang']

        if self.args.build_compiler_rt and \
                not self.is_cross_compile_target(host_target):
            llvm_enable_projects.append('compiler-rt')

        if self.args.build_clang_tools_extra:
            llvm_enable_projects.append('clang-tools-extra')

        # On non-Darwin platforms, build lld so we can always have a
        # linker that is compatible with the swift we are using to
        # compile the stdlib.
        #
        # This makes it easier to build target stdlibs on systems that
        # have old toolchains without more modern linker features.

        target = targets.StdlibDeploymentTarget.get_target_for_name(host_target)

        if not target.platform.is_darwin or self.args.build_lld:
            llvm_enable_projects.append('lld')

        llvm_cmake_options.define('LLVM_ENABLE_PROJECTS',
                                  ';'.join(llvm_enable_projects))

        # In the near future we are aiming to build compiler-rt with
        # LLVM_ENABLE_RUNTIMES
        # Until that happens, we need to unset this variable from
        # LLVM CMakeCache.txt for two reasons
        # * prevent PRs testing this variable to affect other runs landing
        #   unrelated features
        # * avoid fallouts should we land such change and then have to revert
        #   it to account for unforeseen regressions
        llvm_cmake_options.undefine('LLVM_ENABLE_RUNTIMES')

        # NOTE: This is not a dead option! It is relied upon for certain
        # bots/build-configs!
        #
        # TODO: In the future when we are always cross compiling and
        # using Toolchain files, we should put this in either a
        # toolchain file or a cmake cache.
        if self.args.build_toolchain_only:
            clang_tool_driver_build = CMakeOptions.true_false(
                not self.args.build_runtime_with_host_compiler)
            llvm_cmake_options.define('LLVM_BUILD_TOOLS', 'NO')
            llvm_cmake_options.define('LLVM_INSTALL_TOOLCHAIN_ONLY', 'YES')
            llvm_cmake_options.define('LLVM_INCLUDE_TESTS', 'NO')
            llvm_cmake_options.define('CLANG_INCLUDE_TESTS', 'NO')
            llvm_cmake_options.define('LLVM_INCLUDE_UTILS', 'NO')
            llvm_cmake_options.define('LLVM_TOOL_LLI_BUILD', 'NO')
            llvm_cmake_options.define('LLVM_TOOL_LLVM_AR_BUILD', 'NO')
            llvm_cmake_options.define('CLANG_TOOL_CLANG_CHECK_BUILD', 'NO')
            llvm_cmake_options.define('CLANG_TOOL_ARCMT_TEST_BUILD', 'NO')
            llvm_cmake_options.define('CLANG_TOOL_C_ARCMT_TEST_BUILD', 'NO')
            llvm_cmake_options.define('CLANG_TOOL_C_INDEX_TEST_BUILD', 'NO')
            llvm_cmake_options.define('CLANG_TOOL_DRIVER_BUILD',
                                      clang_tool_driver_build)
            llvm_cmake_options.define('CLANG_TOOL_DIAGTOOL_BUILD', 'NO')
            llvm_cmake_options.define('CLANG_TOOL_SCAN_BUILD_BUILD', 'NO')
            llvm_cmake_options.define('CLANG_TOOL_SCAN_VIEW_BUILD', 'NO')
            llvm_cmake_options.define('CLANG_TOOL_CLANG_FORMAT_BUILD', 'NO')

        if not self.args.llvm_include_tests:
            llvm_cmake_options.define('LLVM_INCLUDE_TESTS', 'NO')
            llvm_cmake_options.define('CLANG_INCLUDE_TESTS', 'NO')

        build_root = os.path.dirname(self.build_dir)
        host_machine_target = targets.StdlibDeploymentTarget.host_target().name
        host_build_dir = os.path.join(build_root, 'llvm-{}'.format(
            host_machine_target))

        # Install config files for linux-static
        bin_dir = os.path.join(host_build_dir, 'bin')
        try:
            os.makedirs(bin_dir)
        except FileExistsError:
            pass

        musl_cfg = os.path.join(bin_dir, f'{arch}-swift-linux-musl-clang.cfg')
        with open(musl_cfg, "wt") as f:
            f.write(f"""
-target {arch}-swift-linux-musl
-rtlib=compiler-rt
-stdlib=libc++
-fuse-ld=lld
-unwindlib=libunwind
-lc++abi
-funwind-tables
-fasynchronous-unwind-tables
            """)
        for name in (f'{arch}-swift-linux-musl-clang++.cfg', ):
            try:
                os.symlink(musl_cfg, os.path.join(host_build_dir, 'bin', name))
            except FileExistsError:
                pass

        if self.is_cross_compile_target(host_target):
            build_root = os.path.dirname(self.build_dir)
            host_machine_target = targets.StdlibDeploymentTarget.host_target().name
            host_build_dir = os.path.join(build_root, 'llvm-{}'.format(
                host_machine_target))
            llvm_tblgen = os.path.join(host_build_dir, 'bin', 'llvm-tblgen')
            llvm_cmake_options.define('LLVM_TABLEGEN', llvm_tblgen)
            clang_tblgen = os.path.join(host_build_dir, 'bin', 'clang-tblgen')
            llvm_cmake_options.define('CLANG_TABLEGEN', clang_tblgen)
            confusable_chars_gen = os.path.join(host_build_dir, 'bin',
                                                'clang-tidy-confusable-chars-gen')
            llvm_cmake_options.define('CLANG_TIDY_CONFUSABLE_CHARS_GEN',
                                      confusable_chars_gen)
            pseudo_gen = os.path.join(host_build_dir, 'bin', 'clang-pseudo-gen')
            llvm_cmake_options.define('CLANG_PSEUDO_GEN', pseudo_gen)
            llvm = os.path.join(host_build_dir, 'llvm')
            llvm_cmake_options.define('LLVM_NATIVE_BUILD', llvm)

        host_config = HostSpecificConfiguration(host_target, self.args)

        self.cmake_options.extend(host_config.cmake_options)
        self.cmake_options.extend(llvm_cmake_options)

        self._handle_cxx_headers(host_target, platform)

        self.build_with_cmake(build_targets, self.args.llvm_build_variant, [])

        # copy over the compiler-rt builtins for iOS/tvOS/watchOS to ensure
        # that Swift's stdlib can use compiler-rt builtins when targeting
        # iOS/tvOS/watchOS.
        if self.args.build_llvm and system() == 'Darwin':
            self.copy_embedded_compiler_rt_builtins_from_darwin_host_toolchain(
                self.build_dir)

    def _handle_cxx_headers(self, host_target, platform):
        # When we are building LLVM create symlinks to the c++ headers. We need
        # to do this before building LLVM since compiler-rt depends on being
        # built with the just built clang compiler. These are normally put into
        # place during the cmake step of LLVM's build when libcxx is in
        # tree... but we are not building llvm with libcxx in tree when we build
        # swift. So we need to do configure's work here.
        if system() == 'Darwin':
            # We don't need this for Darwin since libcxx is present in SDKs present
            # in Xcode 12.5 and onward (build-script requires Xcode 13.0 at a minimum),
            # and clang knows how to find it there
            # However, we should take care of removing the symlink
            # laid down by a previous invocation, so to avoid failures
            # finding c++ headers should the target folder become invalid
            cxx_include_symlink = os.path.join(self.build_dir, 'include', 'c++')
            if os.path.islink(cxx_include_symlink):
                print('removing the symlink to system headers in the local '
                      f'clang build directory {cxx_include_symlink} .',
                      flush=True)
                shell.remove(cxx_include_symlink)

            return

        host_cxx_headers_dir = None
        if system() == 'Haiku':
            host_cxx_headers_dir = '/boot/system/develop/headers/c++'

        # This means we're building natively on Android in the Termux
        # app, which supplies the $PREFIX variable.
        elif os.environ.get('ANDROID_DATA'):
            host_cxx_headers_dir = os.path.join(os.environ['PREFIX'], 'include', 'c++')

        # Linux
        else:
            host_cxx_headers_dir = '/usr/include/c++'

        if self.is_cross_compile_target(host_target) and \
                platform == "openbsd":
            toolchain_file = self.get_openbsd_toolchain_file()
            if toolchain_file:
                self.llvm_cmake_options.define('CMAKE_TOOLCHAIN_FILE:PATH',
                                               toolchain_file)

        # Find the path in which the local clang build is expecting to find
        # the c++ header files.
        built_cxx_include_dir = os.path.join(self.build_dir, 'include')
        if not os.path.exists(built_cxx_include_dir):
            os.makedirs(built_cxx_include_dir)
        print('symlinking the system headers ({}) into the local '
              'clang build directory ({}).'.format(
                  host_cxx_headers_dir, built_cxx_include_dir), flush=True)
        shell.call(['ln', '-s', '-f', host_cxx_headers_dir, built_cxx_include_dir])

    def should_test(self, host_target):
        """should_test() -> Bool

        Whether or not this product should be tested with the given arguments.
        """

        # We don't test LLVM
        return False

    def test(self, host_target):
        """
        Perform the test phase for the product.

        This phase might build and execute the product tests.
        """
        pass

    def should_install(self, host_target):
        """should_install() -> Bool

        Whether or not this product should be installed with the given
        arguments.
        """
        return self.args.install_llvm

    def install(self, host_target):
        """
        Perform the install phase for the product.

        This phase might copy the artifacts from the previous phases into a
        destination directory.
        """

        host_install_destdir = self.host_install_destdir(host_target)
        install_targets = ['install']
        if self.args.llvm_install_components and \
           self.args.llvm_install_components != 'all':
            install_targets = []
            components = self.args.llvm_install_components.split(';')
            for component in components:
                if self.is_cross_compile_target(host_target):
                    if component == 'compiler-rt':
                        continue
                install_targets.append('install-{}'.format(component))

        self.install_with_cmake(install_targets, host_install_destdir)

        if self.args.llvm_install_components and system() == 'Darwin':
            clang_dest_dir = '{}{}'.format(host_install_destdir,
                                           self.args.install_prefix)
            self.copy_embedded_compiler_rt_builtins_from_darwin_host_toolchain(
                clang_dest_dir)
