#!/usr/bin/env python3

import argparse
import os
import shutil
import subprocess


def perform_build(args, swiftbuild_path, config, binary_name, opt_flag):
    assert config in ["debug", "release"]
    assert binary_name in ["Benchmark_O", "Benchmark_Osize", "Benchmark_Onone"]
    assert opt_flag in ["-O", "-Osize", "-Onone"]

    inner_build_dir = os.path.join(args.build_path, binary_name)
    swiftbuild_args = [
        swiftbuild_path,
        "--package-path",
        args.package_path,
        "--scratch-path",
        inner_build_dir,
        "--configuration",
        config,
        "-Xswiftc",
        "-Xllvm",
        "-Xswiftc",
        "-align-module-to-page-size",
        "-Xswiftc",
        opt_flag,
    ]
    if args.verbose:
        swiftbuild_args.append("--verbose")
    subprocess.call(swiftbuild_args)

    # Copy the benchmark file into the final ./bin directory.
    binpath = os.path.join(inner_build_dir, config, "SwiftBench")
    finalpath = os.path.join(args.build_path, "bin", binary_name)
    shutil.copy(binpath, finalpath)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--verbose", "-v", action="store_true")
    parser.add_argument("--package-path", type=str, required=True)
    parser.add_argument("--build-path", type=str, required=True)
    parser.add_argument("--toolchain", type=str, required=True)

    args = parser.parse_args()

    # Create our bin directory so we can copy in the binaries.
    bin_dir = os.path.join(args.build_path, "bin")
    if not os.path.isdir(bin_dir):
        os.makedirs(bin_dir)

    swiftbuild_path = os.path.join(args.toolchain, "bin", "swift-build")
    perform_build(args, swiftbuild_path, "debug", "Benchmark_Onone", "-Onone")
    perform_build(args, swiftbuild_path, "release", "Benchmark_Osize", "-Osize")
    perform_build(args, swiftbuild_path, "release", "Benchmark_O", "-O")


if __name__ == "__main__":
    main()
