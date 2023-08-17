# swift_build_support/products/product.py -----------------------*- python -*-
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

import abc
import os

from build_swift.build_swift.wrappers import xcrun

from .. import cmake
from .. import shell
from .. import targets


def is_release_variant(build_variant):
    return build_variant in ['Release', 'RelWithDebInfo']


def is_debug_info_variant(build_variant):
    return build_variant in ['Debug', 'RelWithDebInfo']


class Product(object):
    @classmethod
    def product_name(cls):
        """product_name() -> str

        The identifier-style name to use for this product.
        """
        return cls.__name__.lower()

    @classmethod
    def product_source_name(cls):
        """product_source_name() -> str

        The name of the source code directory of this product.
        It provides a customization point for Product subclasses. It is set to
        the value of product_name() by default for this reason.
        """

        llvm_projects = ['clang',
                         'clang-tools-extra',
                         'compiler-rt',
                         'libcxx',
                         'lldb',
                         'llvm']

        if cls.product_name() in llvm_projects:
            return "llvm-project/{}".format(cls.product_name())
        return cls.product_name()

    @classmethod
    def is_build_script_impl_product(cls):
        """is_build_script_impl_product -> bool

        Whether this product is produced by build-script-impl.
        """
        raise NotImplementedError

    @classmethod
    def is_before_build_script_impl_product(cls):
        """is_before_build_script_impl_product -> bool

        Whether this product is built before any build-script-impl products.
        Such products must be non-build_script_impl products.
        Because such products are built ahead of the compiler, they are
        built using the host toolchain.
        """
        raise NotImplementedError

    @classmethod
    def is_ignore_install_all_product(cls):
        """is_ignore_install_all_product -> bool

        Whether this product is to ignore the install-all directive
        and insted always respect its own should_install.
        This is useful when we run -install-all but have products
        which should never be installed into the toolchain
        (e.g. earlyswiftdriver)
        """
        return False

    @classmethod
    def is_swiftpm_unified_build_product(cls):
        """is_swiftpm_unified_build_product -> bool

        Whether this product should be built in the unified build of SwiftPM
        products.
        """
        return False

    @classmethod
    def is_nondarwin_only_build_product(cls):
        """Returns true if this target should be skipped in darwin builds when
        inferring dependencies.
        """
        return False

    @classmethod
    def get_dependencies(cls):
        """Return a list of products that this product depends upon"""
        raise NotImplementedError

    def should_clean(self, host_target):
        """should_clean() -> Bool

        Whether or not this product should be cleaned before being built
        """
        return False

    def clean(self, host_target):
        """clean() -> void

        Perform the clean, for a non-build-script-impl product.
        """
        raise NotImplementedError

    def should_build(self, host_target):
        """should_build() -> Bool

        Whether or not this product should be built with the given arguments.
        """
        raise NotImplementedError

    def build(self, host_target):
        """build() -> void

        Perform the build, for a non-build-script-impl product.
        """
        raise NotImplementedError

    def should_test(self, host_target):
        """should_test() -> Bool

        Whether or not this product should be tested with the given arguments.
        """
        raise NotImplementedError

    def test(self, host_target):
        """test() -> void

        Run the tests, for a non-build-script-impl product.
        """
        raise NotImplementedError

    def should_install(self, host_target):
        """should_install() -> Bool

        Whether or not this product should be installed with the given
        arguments.
        """
        raise NotImplementedError

    def install(self, host_target):
        """install() -> void

        Install to the toolchain, for a non-build-script-impl product.
        """
        raise NotImplementedError

    def __init__(self, args, toolchain, source_dir, build_dir):
        """
        Parameters
        ----------
        args : `argparse.Namespace`
            The arguments passed by the user to the invocation of the script.
        toolchain : `swift_build_support.toolchain.Toolchain`
            The toolchain being used to build the product. The toolchain will
            point to the tools that the builder should use to build (like the
            compiler or the linker).
        build_dir: string
            The directory in which the product should put all of its build
            products.
        """
        self.args = args
        self.toolchain = toolchain
        self.source_dir = source_dir
        self.build_dir = build_dir
        self.cmake_options = cmake.CMakeOptions()
        self.common_c_flags = ['-Wno-unknown-warning-option',
                               '-Werror=unguarded-availability-new']

    def is_release(self):
        """is_release() -> Bool

        Whether or not this target is built as a release variant
        """
        return is_release_variant(self.args.build_variant)

    def is_debug_info(self):
        """is_debug_info() -> Bool

        Whether or not this target is built with debug info
        """
        return is_debug_info_variant(self.args.build_variant)

    def install_toolchain_path(self, host_target):
        """toolchain_path() -> string

        Returns the path to the toolchain that is being created as part of this
        build
        """
        install_destdir = self.args.install_destdir
        if self.args.cross_compile_hosts:
            if self.is_darwin_host(host_target):
                install_destdir = self.host_install_destdir(host_target)
            else:
                install_destdir = os.path.join(install_destdir, self.args.host_target)
        return targets.toolchain_path(install_destdir,
                                      self.args.install_prefix)

    def native_toolchain_path(self, host_target):
        if self.args.native_swift_tools_path is not None:
            return os.path.split(self.args.native_swift_tools_path)[0]
        else:
            return self.install_toolchain_path(host_target)

    def is_darwin_host(self, host_target):
        return host_target.startswith("macosx") or \
            host_target.startswith("iphone") or \
            host_target.startswith("appletv") or \
            host_target.startswith("watch")

    def should_include_host_in_lipo(self, host_target):
        return self.args.cross_compile_hosts and \
            self.is_darwin_host(host_target)

    def host_install_destdir(self, host_target):
        if self.args.cross_compile_hosts:
            # If cross compiling tools, install into a host-specific subdirectory.
            if self.should_include_host_in_lipo(host_target):
                # If this is one of the hosts we should lipo,
                # install in to a temporary subdirectory.
                return '%s/intermediate-install/%s' % \
                    (os.path.dirname(self.build_dir), host_target)
            elif host_target == "merged-hosts" or \
                    not self.args.cross_compile_append_host_target_to_destdir:
                # This assumes that all hosts are merged to the lipo, or the build
                # was told not to append anything.
                return self.args.install_destdir
            else:
                return '%s/%s' % (self.args.install_destdir, host_target)
        else:
            return self.args.install_destdir

    def is_cross_compile_target(self, host_target):
        return self.args.cross_compile_hosts and \
            host_target in self.args.cross_compile_hosts

    def has_cross_compile_hosts(self):
        return self.args.cross_compile_hosts

    def target_for_platform(self, platform, arch, include_version=True):
        target = None
        if platform == 'macosx':
            target = '{}-apple-macosx{}'.format(
                arch,
                self.args.darwin_deployment_version_osx if include_version else "")
        elif platform == 'iphonesimulator':
            target = '{}-apple-ios{}-simulator'.format(
                arch,
                self.args.darwin_deployment_version_ios if include_version else "")
        elif platform == 'iphoneos':
            target = '{}-apple-ios{}'.format(
                arch,
                self.args.darwin_deployment_version_ios if include_version else "")
        elif platform == 'appletvsimulator':
            target = '{}-apple-tvos{}-simulator'.format(
                arch,
                self.args.darwin_deployment_version_tvos if include_version else "")
        elif platform == 'appletvos':
            target = '{}-apple-tvos{}'.format(
                arch,
                self.args.darwin_deployment_version_tvos if include_version else "")
        elif platform == 'watchsimulator':
            target = '{}-apple-watchos{}-simulator'.format(
                arch,
                self.args.darwin_deployment_version_watchos if include_version else "")
        elif platform == 'watchos':
            target = '{}-apple-watchos{}'.format(
                arch,
                self.args.darwin_deployment_version_watchos if include_version else "")
        return target

    def generate_darwin_toolchain_file(self, platform, arch):
        """
        Generates a new CMake tolchain file that specifies Darwin as a target
        plaftorm.

            Returns: path on the filesystem to the newly generated toolchain file.
        """

        shell.makedirs(self.build_dir)
        toolchain_file = os.path.join(self.build_dir, 'BuildScriptToolchain.cmake')

        cmake_osx_sysroot = xcrun.sdk_path(platform)

        target = self.target_for_platform(platform, arch)
        if not target:
            raise RuntimeError('Unhandled platform {}?!'.format(platform))

        toolchain_args = {}

        toolchain_args['CMAKE_SYSTEM_NAME'] = 'Darwin'
        toolchain_args['CMAKE_OSX_SYSROOT'] = cmake_osx_sysroot
        toolchain_args['CMAKE_OSX_ARCHITECTURES'] = arch
        toolchain_args['CMAKE_SHARED_LIBRARY_RUNTIME_C_FLAG'] = '\"-Wl,-rpath,\"'
        toolchain_args['CMAKE_SYSTEM_VERSION'] = self.args.darwin_deployment_version_osx

        if self.toolchain.cc.endswith('clang'):
            toolchain_args['CMAKE_C_COMPILER_TARGET'] = target
        if self.toolchain.cxx.endswith('clang++'):
            toolchain_args['CMAKE_CXX_COMPILER_TARGET'] = target
        # Swift always supports cross compiling.
        toolchain_args['CMAKE_Swift_COMPILER_TARGET'] = target

        toolchain_args['CMAKE_LINKER'] = self.toolchain.ld
        toolchain_args['CMAKE_CC'] = self.toolchain.cc
        toolchain_args['CMAKE_CXX'] = self.toolchain.cxx
        toolchain_args['CMAKE_ASM_COMPILER_AR'] = self.toolchain.ar
        toolchain_args['CMAKE_ASM_COMPILER_RANLIB'] = self.toolchain.ranlib
        toolchain_args['CMAKE_LINKER'] = self.toolchain.ld

        # Sort by the key so that we always produce the same toolchain file
        data = sorted(toolchain_args.items(), key=lambda x: x[0])
        if not self.args.dry_run:
            with open(toolchain_file, 'w') as f:
                f.writelines("set({} {})\n".format(k, v) for k, v in data)
        else:
            print("DRY_RUN! Writing Toolchain file to path: {}".format(toolchain_file))

        return toolchain_file

    def get_linux_target_components(self, arch):
        # Map tuples of (platform, arch) to ABI
        #
        # E.x.: Hard ABI or Soft ABI for Linux map to gnueabihf
        arch_platform_to_abi = {
            # For now always map to hard float ABI.
            'armv7': ('arm', 'gnueabihf')
        }

        abi = 'gnu'
        vendor = 'unknown'

        try:
            output = shell.capture([self.toolchain.cc, "--print-target-triple"])

            # clang can't handle default `*-unknown-linux-*` components on Alpine,
            # it needs special handling to propagate `vendor` and `abi` intact
            if 'alpine-linux-musl' in output:
                vendor = 'alpine'
                abi = 'musl'

            sysroot_arch, abi = arch_platform_to_abi.get(arch, (arch, abi))

        except BaseException:
            # Default is just arch, gnu
            sysroot_arch, abi = arch_platform_to_abi.get(arch, (arch, abi))
        return sysroot_arch, vendor, abi

    def get_linux_sysroot(self, platform, arch):
        if not self.is_cross_compile_target('{}-{}'.format(platform, arch)):
            return None
        sysroot_arch, abi = self.get_linux_target_components(arch)
        # $ARCH-$PLATFORM-$ABI
        # E.x.: aarch64-linux-gnu
        sysroot_dirname = '{}-{}-{}'.format(sysroot_arch, platform, abi)
        return os.path.join(os.sep, 'usr', sysroot_dirname)

    def get_linux_target(self, platform, arch):
        sysroot_arch, vendor, abi = self.get_linux_target_components(arch)
        return '{}-{}-linux-{}'.format(sysroot_arch, vendor, abi)

    def generate_linux_toolchain_file(self, platform, arch):
        """
        Generates a new CMake tolchain file that specifies Linux as a target
        plaftorm.

            Returns: path on the filesystem to the newly generated toolchain file.
        """

        shell.makedirs(self.build_dir)
        toolchain_file = os.path.join(self.build_dir, 'BuildScriptToolchain.cmake')

        toolchain_args = {}

        toolchain_args['CMAKE_SYSTEM_NAME'] = 'Linux'
        toolchain_args['CMAKE_SYSTEM_PROCESSOR'] = arch

        # We only set the actual sysroot if we are actually cross
        # compiling. This is important since otherwise cmake seems to change the
        # RUNPATH to be a relative rather than an absolute path, breaking
        # certain cmark tests (and maybe others).
        maybe_sysroot = self.get_linux_sysroot(platform, arch)
        if maybe_sysroot is not None:
            toolchain_args['CMAKE_SYSROOT'] = maybe_sysroot

        target = self.get_linux_target(platform, arch)
        if self.toolchain.cc.endswith('clang'):
            toolchain_args['CMAKE_C_COMPILER_TARGET'] = target
        if self.toolchain.cxx.endswith('clang++'):
            toolchain_args['CMAKE_CXX_COMPILER_TARGET'] = target
        # Swift always supports cross compiling.
        toolchain_args['CMAKE_Swift_COMPILER_TARGET'] = target
        toolchain_args['CMAKE_FIND_ROOT_PATH_MODE_PROGRAM'] = 'NEVER'
        toolchain_args['CMAKE_FIND_ROOT_PATH_MODE_LIBRARY'] = 'ONLY'
        toolchain_args['CMAKE_FIND_ROOT_PATH_MODE_INCLUDE'] = 'ONLY'
        toolchain_args['CMAKE_FIND_ROOT_PATH_MODE_PACKAGE'] = 'ONLY'

        # Sort by the key so that we always produce the same toolchain file
        data = sorted(toolchain_args.items(), key=lambda x: x[0])
        if not self.args.dry_run:
            with open(toolchain_file, 'w') as f:
                f.writelines("set({} {})\n".format(k, v) for k, v in data)
        else:
            print("DRY_RUN! Writing Toolchain file to path: {}".format(toolchain_file))

        return toolchain_file

    def generate_toolchain_file_for_darwin_or_linux(self, host_target):
        """
        Checks `host_target` platform and generates a new CMake tolchain file
        appropriate for that target plaftorm (either Darwin or Linux). Defines
        `CMAKE_C_FLAGS` and `CMAKE_CXX_FLAGS` as CMake options. Also defines
        `CMAKE_TOOLCHAIN_FILE` with the path of the generated toolchain file
        as a CMake option.

            Returns: path to the newly generated toolchain file on the
            filesystem.
        """

        (platform, arch) = host_target.split('-')
        common_c_flags = ' '.join(self.common_cross_c_flags(platform, arch))
        self.cmake_options.define('CMAKE_C_FLAGS', common_c_flags)
        self.cmake_options.define('CMAKE_CXX_FLAGS', common_c_flags)

        toolchain_file = None
        if self.is_darwin_host(host_target):
            toolchain_file = self.generate_darwin_toolchain_file(platform, arch)
            self.cmake_options.define('CMAKE_TOOLCHAIN_FILE:PATH', toolchain_file)
        elif platform == "linux":
            toolchain_file = self.generate_linux_toolchain_file(platform, arch)
            self.cmake_options.define('CMAKE_TOOLCHAIN_FILE:PATH', toolchain_file)

        return toolchain_file

    def get_openbsd_toolchain_file(self):
        return os.getenv('OPENBSD_USE_TOOLCHAIN_FILE')

    def common_cross_c_flags(self, platform, arch, include_arch=False):
        cross_flags = []

        target = self.target_for_platform(platform, arch, include_version=True)
        if include_arch and target:
            cross_flags.append('-arch {}'.format(arch))
            cross_flags.append('-target {}'.format(target))

        if self.is_release():
            cross_flags.append('-fno-stack-protector')

        return self.common_c_flags + cross_flags


class ProductBuilder(object):
    """
    Abstract base class for all ProductBuilders.

    An specific ProductBuilder will implement the interface methods depending
    how the product want to be build. Multiple products can use the same
    product builder if parametrized right (for example all the products build
    using CMake).

    Ideally a ProductBuilder will be initialized with references to the
    invocation arguments, the calculated toolchain, the calculated workspace,
    and the target host, but the base class doesn't impose those requirements
    in order to be flexible.

    NOTE: Python doesn't need an explicit abstract base class, but it helps
    documenting the interface.
    """

    @abc.abstractmethod
    def __init__(self, product_class, args, toolchain, workspace):
        """
        Create a product builder for the given product class.

        Parameters
        ----------
        product_class : class
            A subtype of `Product` which describes the product being built by
            this builder.
        args : `argparse.Namespace`
            The arguments passed by the user to the invocation of the script. A
            builder should consider this argument read-only.
        toolchain : `swift_build_support.toolchain.Toolchain`
            The toolchain being used to build the product. The toolchain will
            point to the tools that the builder should use to build (like the
            compiler or the linker).
        workspace : `swift_build_support.workspace.Workspace`
            The workspace where the source code and the build directories have
            to be located. A builder should use the workspace to access its own
            source/build directory, as well as other products source/build
            directories.
        """
        pass

    @abc.abstractmethod
    def build(self):
        """
        Perform the build phase for the product.

        This phase might also imply a configuration phase, but each product
        builder is free to determine how to do it.
        """
        pass

    @abc.abstractmethod
    def test(self):
        """
        Perform the test phase for the product.

        This phase might build and execute the product tests.
        """
        pass

    @abc.abstractmethod
    def install(self):
        """
        Perform the install phase for the product.

        This phase might copy the artifacts from the previous phases into a
        destination directory.
        """
        pass
