#!/usr/bin/env python3

import argparse
import os
import subprocess


def perform_build(args, swiftbuild_path):
    swiftbuild_args = [
        swiftbuild_path,
        "--package-path",
        args.package_path,
        "--build-path",
        args.build_path,
        "--configuration",
        args.configuration,
        "-Xswiftc",
        "-I",
        "-Xswiftc",
        os.path.join(args.toolchain, 'include', 'swift'),
        "-Xswiftc",
        "-L",
        "-Xswiftc",
        os.path.join(args.toolchain, 'lib', 'swift', 'macosx'),
        "-Xswiftc",
        "-lswiftRemoteMirror"
    ]
    if args.verbose:
        swiftbuild_args.append("--verbose")
    print(' '.join(swiftbuild_args))
    subprocess.call(swiftbuild_args)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--verbose", "-v", action="store_true")
    parser.add_argument("--package-path", type=str, required=True)
    parser.add_argument("--build-path", type=str, required=True)
    parser.add_argument("--toolchain", type=str, required=True)
    parser.add_argument("--configuration", type=str, choices=['debug', 'release'],
                        default='release')

    args = parser.parse_args()

    # Create our bin directory so we can copy in the binaries.
    bin_dir = os.path.join(args.build_path, "bin")
    if not os.path.isdir(bin_dir):
        os.makedirs(bin_dir)

    swiftbuild_path = os.path.join(args.toolchain, "bin", "swift-build")
    perform_build(args, swiftbuild_path)


if __name__ == "__main__":
    main()
