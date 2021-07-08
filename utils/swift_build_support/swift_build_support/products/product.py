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

        Whether this product is build before any build-script-impl products.
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

    def install_toolchain_path(self, host_target):
        """toolchain_path() -> string

        Returns the path to the toolchain that is being created as part of this
        build
        """
        install_destdir = self.args.install_destdir
        if self.args.cross_compile_hosts:
            if targets.StdlibDeploymentTarget.get_target_for_name(
                    host_target).platform.is_darwin:
                build_root = os.path.dirname(self.build_dir)
                install_destdir = '%s/intermediate-install/%s' % (build_root,
                                                                  host_target)
        return targets.toolchain_path(install_destdir,
                                      self.args.install_prefix)

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
                    (self.args.install_destdir, host_target)
            elif host_target == "merged-hosts":
                # This assumes that all hosts are merged to the lipo.
                return self.args.install_destdir
            else:
                return '%s/%s' % (self.args.install_destdir, host_target)
        else:
            return self.args.install_destdir

    def is_cross_compile_target(self, host_target):
        return self.args.cross_compile_hosts and \
            host_target in self.args.cross_compile_hosts

    def generate_darwin_toolchain_file(self, platform, arch):
        shell.makedirs(self.build_dir)
        toolchain_file = os.path.join(self.build_dir, 'BuildScriptToolchain.cmake')

        cmake_osx_sysroot = xcrun.sdk_path(platform)

        target = None
        if platform == 'macosx':
            target = '{}-apple-macosx{}'.format(
                arch, self.args.darwin_deployment_version_osx)
        elif platform == 'iphonesimulator':
            target = '{}-apple-ios{}'.format(
                arch, self.args.darwin_deployment_version_ios)
        elif platform == 'iphoneos':
            target = '{}-apple-ios{}'.format(
                arch, self.args.darwin_deployment_version_ios)
        elif platform == 'appletvsimulator':
            target = '{}-apple-tvos{}'.format(
                arch, self.args.darwin_deployment_version_tvos)
        elif platform == 'appletvos':
            target = '{}-apple-tvos{}'.format(
                arch, self.args.darwin_deployment_version_tvos)
        elif platform == 'watchsimulator':
            target = '{}-apple-watchos{}'.format(
                arch, self.args.darwin_deployment_version_watchos)
        elif platform == 'watchos':
            target = '{}-apple-watchos{}'.format(
                arch, self.args.darwin_deployment_version_watchos)
        else:
            raise RuntimeError("Unhandled platform?!")

        toolchain_args = {}

        toolchain_args['CMAKE_SYSTEM_NAME'] = 'Darwin'
        toolchain_args['CMAKE_OSX_SYSROOT'] = cmake_osx_sysroot
        toolchain_args['CMAKE_OSX_ARCHITECTURES'] = arch

        if self.toolchain.cc.endswith('clang'):
            toolchain_args['CMAKE_C_COMPILER_TARGET'] = target
        if self.toolchain.cxx.endswith('clang++'):
            toolchain_args['CMAKE_CXX_COMPILER_TARGET'] = target
        # Swift always supports cross compiling.
        toolchain_args['CMAKE_Swift_COMPILER_TARGET'] = target

        # Sort by the key so that we always produce the same toolchain file
        data = sorted(toolchain_args.items(), key=lambda x: x[0])
        if not self.args.dry_run:
            with open(toolchain_file, 'w') as f:
                f.writelines("set({} {})\n".format(k, v) for k, v in data)
        else:
            print("DRY_RUN! Writing Toolchain file to path: {}".format(toolchain_file))

        return toolchain_file

    def common_cross_c_flags(self, platform, arch):
        cross_flags = []

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
