# swift_build_support/targets.py - Build target helpers -*- python -*-
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors

import os
import platform

from . import cmake
from . import shell

try:
    from build_swift.build_swift.wrappers import xcrun
except ImportError:
    from build_swift.wrappers import xcrun


class Platform(object):
    """
    Abstract representation of a platform Swift can run on.
    """

    def __init__(self, name, archs, sdk_name=None):
        """
        Create a platform with the given name and list of architectures.
        """
        self.name = name
        self.targets = [Target(self, arch) for arch in archs]
        # FIXME: Eliminate this argument; apparently the SDK names are
        # internally a private implementation detail of the build script, so we
        # should just make them the same as the platform name.
        self.sdk_name = name.upper() if sdk_name is None else sdk_name

        # Add a property for each arch.
        for target in self.targets:
            setattr(self, target.arch, target)

    @property
    def is_darwin(self):
        """Convenience function for checking if this is a Darwin platform."""
        return isinstance(self, DarwinPlatform)

    @property
    def supports_benchmark(self):
        # By default, we don't support benchmarks on most platforms.
        return False

    @property
    def uses_host_tests(self):
        """
        Check if this is a Darwin platform that needs a connected device
        for tests.
        """
        # By default, we don't use connected devices on most platforms.
        return False

    def contains(self, target_name):
        """
        Returns True if the given target name belongs to a one of this
        platform's targets.
        """
        for target in self.targets:
            if target.name == target_name:
                return True
        return False

    def swift_flags(self, args):
        """
        Swift compiler flags for a platform, useful for cross-compiling
        """
        return ''

    def cmake_options(self, args):
        """
        CMake flags to build for a platform, useful for cross-compiling
        """
        return cmake.CMakeOptions()

    def swiftpm_config(self, args, output_dir, swift_toolchain, resource_path):
        """
        Generate a JSON file that SPM can use to cross-compile
        """
        raise NotImplementedError('Generating a SwiftPM cross-compilation JSON file '
                                  'for %s is not supported yet' % self.name)


class DarwinPlatform(Platform):
    def __init__(self, name, archs, sdk_name=None, is_simulator=False):
        self.is_simulator = is_simulator
        super(DarwinPlatform, self).__init__(name, archs, sdk_name)

    @property
    def is_embedded(self):
        """Check if this is a Darwin platform for embedded devices."""
        return self.name != "macosx" and self.name != "maccatalyst"

    @property
    def supports_benchmark(self):
        # By default, on Darwin we support benchmarks on all non-simulator
        # platforms.
        return not self.is_simulator

    @property
    def uses_host_tests(self):
        """
        Check if this is a Darwin platform that needs a connected device
        for tests.
        """
        return self.is_embedded and not self.is_simulator

    def sdk_supports_architecture(self, arch, toolchain):
        """
        Convenience function for checking whether the SDK supports the
        target architecture.
        """

        # The names match up with the xcrun SDK names.
        xcrun_sdk_name = self.name

        if (xcrun_sdk_name == 'watchos' and arch == 'armv7k'):
            return True

        sdk_path = xcrun.sdk_path(sdk=xcrun_sdk_name, toolchain=toolchain)
        if not sdk_path:
            raise RuntimeError('Cannot find SDK path for %s' % xcrun_sdk_name)

        # Find the SDKSettings.plist for this sdK
        plistCommand = [
            '/usr/libexec/PlistBuddy',
            '-c',
            'Print :SupportedTargets:%s:Archs' % (self.name),
            '%s/SDKSettings.plist' % (sdk_path)
        ]

        sdk_archs = shell.capture(plistCommand, dry_run=False, echo=True)
        return arch in sdk_archs


class AndroidPlatform(Platform):
    @property
    def uses_host_tests(self):
        """
        Check if this is a Darwin platform that needs a connected device
        for tests.
        """
        return True

    def swift_flags(self, args):
        flags = '-target %s-unknown-linux-android%s ' % (args.android_arch,
                                                         args.android_api_level)

        flags += '-resource-dir %s/swift-%s-%s/lib/swift ' % (
                 args.build_root, self.name, args.android_arch)

        android_toolchain_path = self.ndk_toolchain_path(args)

        flags += '-sdk %s/sysroot ' % (android_toolchain_path)
        flags += '-tools-directory %s/bin' % (android_toolchain_path)
        return flags

    def cmake_options(self, args):
        options = cmake.CMakeOptions()
        options.define('CMAKE_SYSTEM_NAME', 'Android')
        options.define('CMAKE_SYSTEM_VERSION' , args.android_api_level)
        options.define('CMAKE_SYSTEM_PROCESSOR', args.android_arch if not
                       args.android_arch == 'armv7'
                       else 'armv7-a')
        options.define('CMAKE_ANDROID_NDK:PATH', args.android_ndk)
        return options

    def ndk_toolchain_path(self, args):
        return '%s/toolchains/llvm/prebuilt/%s' % (
            args.android_ndk, StdlibDeploymentTarget.host_target().name)

    def swiftpm_config(self, args, output_dir, swift_toolchain, resource_path):
        config_file = '%s/swiftpm-android-%s.json' % (output_dir, args.android_arch)

        if os.path.exists(config_file):
            print("Using existing config at %s" % config_file)
            return config_file

        spm_json = '{\n'
        spm_json += '  "version": 1,\n'
        spm_json += '  "target": "%s-unknown-linux-android%s",\n' % (
                    args.android_arch, args.android_api_level)
        spm_json += '  "toolchain-bin-dir": "%s/bin",\n' % swift_toolchain
        spm_json += '  "sdk": "%s/sysroot",\n' % self.ndk_toolchain_path(args)

        spm_json += '  "extra-cc-flags": [ "-fPIC", "-I%s/usr/include" ],\n' % (
                    args.cross_compile_deps_path)

        spm_json += '  "extra-swiftc-flags": [\n'
        spm_json += '    "-resource-dir", "%s",\n' % resource_path
        spm_json += '    "-tools-directory", "%s/bin",\n' % (
                    self.ndk_toolchain_path(args))
        spm_json += '    "-Xcc", "-I%s/usr/include",\n' % args.cross_compile_deps_path
        spm_json += '    "-L%s/usr/lib"\n' % args.cross_compile_deps_path
        spm_json += '  ],\n'

        spm_json += '  "extra-cpp-flags": [ "-lstdc++" ]\n'
        spm_json += '}'

        with open(config_file, 'w') as f:
            f.write(spm_json)
        return config_file


class OpenBSDPlatform(Platform):
    def cmake_options(self, args):
        toolchain_file = os.getenv('OPENBSD_USE_TOOLCHAIN_FILE')
        if not toolchain_file:
            return ''
        return f'-DCMAKE_TOOLCHAIN_FILE="${toolchain_file}"'


class Target(object):
    """
    Abstract representation of a target Swift can run on.
    """

    def __init__(self, platform, arch):
        self.platform = platform
        self.arch = arch
        # Delegate to the platform, this is usually not arch specific.
        self.supports_benchmark = self.platform.supports_benchmark

    @property
    def name(self):
        return "{}-{}".format(self.platform.name, self.arch)


class StdlibDeploymentTarget(object):
    OSX = DarwinPlatform("macosx", archs=["x86_64", "arm64"],
                         sdk_name="OSX")

    iOS = DarwinPlatform("iphoneos", archs=["arm64", "arm64e"],
                         sdk_name="IOS")
    iOSSimulator = DarwinPlatform("iphonesimulator", archs=["x86_64", "arm64"],
                                  sdk_name="IOS_SIMULATOR",
                                  is_simulator=True)

    AppleTV = DarwinPlatform("appletvos", archs=["arm64"],
                             sdk_name="TVOS")
    AppleTVSimulator = DarwinPlatform("appletvsimulator", archs=["x86_64", "arm64"],
                                      sdk_name="TVOS_SIMULATOR",
                                      is_simulator=True)

    AppleWatch = DarwinPlatform("watchos", archs=["armv7k", "arm64_32"],
                                sdk_name="WATCHOS")

    AppleWatchSimulator = DarwinPlatform("watchsimulator",
                                         archs=["i386", "x86_64", "arm64"],
                                         sdk_name="WATCHOS_SIMULATOR",
                                         is_simulator=True)

    # A platform that's not tied to any particular OS, and it meant to be used
    # to build the stdlib as standalone and/or statically linked.
    Freestanding = Platform("freestanding", archs=[
        "i386", "x86_64",
        "armv7", "armv7s", "armv7k", "armv7m", "armv7em",
        "arm64", "arm64e"])

    Linux = Platform("linux", archs=[
        "x86_64",
        "i686",
        "armv5",
        "armv6",
        "armv7",
        "aarch64",
        "powerpc",
        "powerpc64",
        "powerpc64le",
        "s390x"])

    FreeBSD = Platform("freebsd", archs=["x86_64", "arm64"])

    OpenBSD = OpenBSDPlatform("openbsd", archs=["amd64"])

    Cygwin = Platform("cygwin", archs=["x86_64"])

    Android = AndroidPlatform("android", archs=["armv7", "aarch64", "x86_64"])

    Windows = Platform("windows", archs=["x86_64"])

    Haiku = Platform("haiku", archs=["x86_64"])

    WASI = Platform("wasi", archs=["wasm32"])

    # The list of known platforms.
    known_platforms = [
        OSX,
        iOS, iOSSimulator,
        AppleTV, AppleTVSimulator,
        AppleWatch, AppleWatchSimulator,
        Freestanding,
        Linux,
        FreeBSD,
        OpenBSD,
        Cygwin,
        Android,
        Windows,
        Haiku,
        WASI]

    # Cache of targets by name.
    _targets_by_name = dict((target.name, target)
                            for platform in known_platforms
                            for target in platform.targets)

    _sdk_targets = {
        'OSX': OSX.targets,
        'IOS': iOS.targets,
        'IOS_SIMULATOR': iOSSimulator.targets,
        'TVOS': AppleTV.targets,
        'TVOS_SIMULATOR': AppleTVSimulator.targets,
        'WATCHOS': AppleWatch.targets,
        'WATCHOS_SIMULATOR': AppleWatchSimulator.targets,
    }

    @staticmethod
    def host_target():
        """
        Return the host target for the build machine, if it is one of
        the recognized targets. Otherwise, throw a NotImplementedError.
        """
        system = platform.system()
        machine = platform.machine()

        if system == 'Linux':
            if 'ANDROID_DATA' in os.environ:
                if machine.startswith('armv7'):
                    return StdlibDeploymentTarget.Android.armv7
                elif machine == 'aarch64':
                    return StdlibDeploymentTarget.Android.aarch64
                raise NotImplementedError('Android System with architecture '
                                          '"%s" is not supported' % machine)

            if machine == 'x86_64':
                return StdlibDeploymentTarget.Linux.x86_64
            elif machine == 'i686':
                return StdlibDeploymentTarget.Linux.i686
            elif machine.startswith('armv7'):
                # linux-armv7* is canonicalized to 'linux-armv7'
                return StdlibDeploymentTarget.Linux.armv7
            elif machine.startswith('armv6'):
                # linux-armv6* is canonicalized to 'linux-armv6'
                return StdlibDeploymentTarget.Linux.armv6
            elif machine.startswith('armv5'):
                # linux-armv5* is canonicalized to 'linux-armv5'
                return StdlibDeploymentTarget.Linux.armv5
            elif machine == 'aarch64':
                return StdlibDeploymentTarget.Linux.aarch64
            elif machine == 'ppc':
                return StdlibDeploymentTarget.Linux.powerpc
            elif machine == 'ppc64':
                return StdlibDeploymentTarget.Linux.powerpc64
            elif machine == 'ppc64le':
                return StdlibDeploymentTarget.Linux.powerpc64le
            elif machine == 's390x':
                return StdlibDeploymentTarget.Linux.s390x

        elif system == 'Darwin':
            if machine == 'x86_64':
                return StdlibDeploymentTarget.OSX.x86_64
            elif machine == 'arm64':
                return StdlibDeploymentTarget.OSX.arm64
            elif machine == 'arm64e':
                return StdlibDeploymentTarget.OSX.arm64e

        elif system == 'FreeBSD':
            if machine == 'amd64':
                return StdlibDeploymentTarget.FreeBSD.x86_64
            elif machine == 'arm64':
                return StdlibDeploymentTarget.FreeBSD.arm64

        elif system == 'OpenBSD':
            if machine == 'amd64':
                return StdlibDeploymentTarget.OpenBSD.amd64

        elif system == 'CYGWIN_NT-10.0':
            if machine == 'x86_64':
                return StdlibDeploymentTarget.Cygwin.x86_64

        elif system == 'Windows':
            if machine == "AMD64":
                return StdlibDeploymentTarget.Windows.x86_64

        elif system == 'Haiku':
            if machine == 'x86_64':
                return StdlibDeploymentTarget.Haiku.x86_64

        elif system == 'WASI':
            if machine == 'wasm32':
                return StdlibDeploymentTarget.WASI.wasm32

        raise NotImplementedError('System "%s" with architecture "%s" is not '
                                  'supported' % (system, machine))

    @classmethod
    def get_target_for_name(cls, name):
        return cls._targets_by_name.get(name)

    @classmethod
    def get_targets_by_name(cls, names):
        return [cls.get_target_for_name(name) for name in names]

    @classmethod
    def get_target_names(cls):
        return sorted([name for (name, target) in
                       cls._targets_by_name.items()])

    @classmethod
    def get_migrated_targets_for_sdk(cls, sdk_name):
        return cls._sdk_targets.get(sdk_name, None)

    @classmethod
    def get_all_migrated_sdks(cls):
        return cls._sdk_targets.keys()


def install_prefix():
    """
    Returns the default path at which built Swift products (like bin, lib,
    and include) will be installed, based on the host machine's operating
    system.
    """
    if platform.system() == 'Darwin':
        return '/Applications/Xcode.app/Contents/Developer/Toolchains/' + \
            'XcodeDefault.xctoolchain/usr'
    else:
        return '/usr'


def darwin_toolchain_prefix(darwin_install_prefix):
    """
    Given the install prefix for a Darwin system, and assuming that path
    is to a .xctoolchain directory, return the path to the .xctoolchain
    directory.
    """
    return os.path.split(darwin_install_prefix)[0]


def toolchain_path(install_destdir, install_prefix):
    """
    Given the install prefix for a Darwin system, and assuming that path
    is to a .xctoolchain directory, return the path to the .xctoolchain
    directory in the given install directory.
    This toolchain is being populated during the build-script invocation.
    Downstream products can use products that were previously installed into
    this toolchain.
    """
    built_toolchain_path = install_destdir
    if platform.system() == 'Darwin':
        # The prefix is an absolute path, so concatenate without os.path.
        built_toolchain_path += darwin_toolchain_prefix(install_prefix) + "/usr"
    else:
        built_toolchain_path += install_prefix
    return built_toolchain_path
