#!/usr/bin/env python3
# utils/sil-opt-verify-all-modules.py - Verifies Swift modules -*- python -*-
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors

import argparse
import glob
import multiprocessing
import os
import shlex
import subprocess
import sys
import tempfile


def get_verify_toolchain_modules_commands(toolchain_dir, sil_opt):
    if sil_opt is None:
        sil_opt = os.path.join(toolchain_dir, 'usr', 'bin', 'sil-opt')

    toolchain_basename = os.path.basename(toolchain_dir)
    if toolchain_basename.startswith('Legacy'):
        return []
    if toolchain_basename.startswith('XcodeDefault'):
        toolchain_name = 'XcodeDefault'
    if toolchain_basename.startswith('tvOS'):
        toolchain_name = 'tvOS'
    if toolchain_basename.startswith('OSX'):
        toolchain_name = 'OSX'
    if toolchain_basename.startswith('watchOS'):
        toolchain_name = 'watchOS'
    if toolchain_basename.startswith('iOS'):
        toolchain_name = 'iOS'

    return get_verify_resource_dir_modules_commands(
        os.path.join(toolchain_dir, 'usr', 'lib', 'swift'),
        os.path.join(toolchain_dir, 'usr', 'bin', 'sil-opt'),
        toolchain_name)


def get_verify_build_dir_commands(build_dir, toolchain_name='XcodeDefault'):
    return get_verify_resource_dir_modules_commands(
        os.path.join(build_dir, 'lib', 'swift'),
        os.path.join(build_dir, 'bin', 'sil-opt'),
        toolchain_name)


def get_verify_resource_dir_modules_commands(
        resource_dir, sil_opt, toolchain_name):
    print("================================================================")
    print("Resource dir: " + resource_dir)
    print("sil-opt path: " + sil_opt)

    known_platforms = [
        ('appletvos', 'arm64', 'arm64-apple-tvos9.0'),
        ('appletvsimulator', 'x86_64', 'x86_64-apple-tvos9.0'),
        ('iphoneos', 'arm64', 'arm64-apple-ios7.0'),
        ('iphonesimulator', 'x86_64', 'x86_64-apple-ios7.0'),
        ('macosx', 'x86_64', 'x86_64-apple-macosx10.9'),
        ('watchos', 'armv7k', 'armv7k-apple-watchos2.0'),
        ('watchsimulator', 'i386', 'i386-apple-watchos2.0'),
    ]

    commands = []
    module_cache_dir = tempfile.mkdtemp(
        prefix="swift-testsuite-clang-module-cache")
    for (subdir, arch, triple) in known_platforms:
        modules_dir = os.path.join(resource_dir, subdir, arch)
        print(modules_dir)
        modules = glob.glob(os.path.join(modules_dir, '*.swiftmodule'))
        for module_file_name in modules:
            if module_file_name.endswith('XCTest.swiftmodule'):
                # FIXME: sil-opt does not have the '-F' option.
                continue
            commands.append([
                'xcrun', '--toolchain', toolchain_name, '--sdk', subdir,
                sil_opt,
                '-target', triple,
                '-resource-dir', resource_dir,
                '-module-cache-path', module_cache_dir,
                '-verify',
                module_file_name,
            ])

    return commands


def quote_shell_command(args):
    return " ".join([shlex.quote(a) for a in args])


def run_commands_in_parallel(commands):
    makefile = ".DEFAULT_GOAL := all\n"
    targets = []
    for c in commands:
        target_name = "target" + str(len(targets))
        targets.append(target_name)
        makefile += target_name + ":\n"
        makefile += \
            "\t" + quote_shell_command(c) + \
            " > {target}.stdout\n".format(target=target_name)

    makefile += "all: " + " ".join(targets) + "\n"

    temp_dir = tempfile.mkdtemp(prefix="swift-testsuite-main")
    with open(os.path.join(temp_dir, 'Makefile'), 'w') as makefile_file:
        makefile_file.write(makefile)

    max_processes = multiprocessing.cpu_count()
    subprocess.check_call([
        'make',
        '-C', temp_dir,
        '-j', str(max_processes),
        '--keep-going'
    ])


def main():
    parser = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description="""Verifies Swift modules.""")
    parser.add_argument(
        "--sil-opt",
        help="use the specified 'sil-opt' binary",
        metavar="PATH")
    parser.add_argument(
        "--verify-build-dir",
        help="verify the Swift resource directory under the given build dir.",
        metavar="PATH")
    parser.add_argument(
        "--verify-xcode",
        help="verify the Xcode.app that is currently xcode-select'ed",
        action="store_true")
    args = parser.parse_args()

    if args.verify_build_dir is not None and args.verify_xcode:
        print("--verify-build-dir and --verify-xcode can't be used together")
        return 1

    if args.verify_build_dir is not None:
        commands = get_verify_build_dir_commands(args.verify_build_dir)

    if args.verify_xcode:
        # Find Xcode.
        swift_path = subprocess.check_output(['xcrun', '--find', 'swift'])
        xcode_path = swift_path
        for _ in range(0, 7):
            xcode_path = os.path.dirname(xcode_path)

        toolchains_dir = os.path.join(
            xcode_path, 'Contents', 'Developer', 'Toolchains')
        toolchains = glob.glob(os.path.join(toolchains_dir, '*.xctoolchain'))

        commands = []
        for toolchain_dir in toolchains:
            commands += get_verify_toolchain_modules_commands(
                toolchain_dir, args.sil_opt)

    run_commands_in_parallel(commands)

    return 0


if __name__ == "__main__":
    sys.exit(main())
