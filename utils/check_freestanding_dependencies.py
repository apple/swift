#!/usr/bin/env python3
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors

import argparse
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument("--library", help="path to libswiftCore.a to check")
parser.add_argument("--vendor", help="flavor of the freestanding stdlib")
parser.add_argument("--nm-path", help="path to llvm-nm binary to use")
args = parser.parse_args()


################################################################################
#
# EXPECTED/ALLOWED DEPENDENCIES
#
# Before adding new symbols into these lists please consult with:
# @kubamracek, @compnerd
#
# The 'freestanding' build of the Swift runtime and standard library is
# intended to depend on as few platform symbols/APIs as possible.
#
################################################################################
cxx_dependencies = [
    "___cxa_guard_acquire", "___cxa_guard_release",
]
math_dependencies = [
    "_ceill", "_cos", "_cosf", "_cosl", "_exp", "_exp2", "_exp2f", "_exp2l",
    "_expf", "_expl", "_floorl", "_fma", "_fmaf", "_fmal", "_fmod", "_fmodf",
    "_fmodl", "_log", "_log10", "_log10f", "_log10l", "_log2", "_log2f",
    "_log2l", "_logf", "_logl", "_nearbyintl", "_remainder", "_remainderf",
    "_remainderl", "_rintl", "_roundl", "_sin", "_sinf", "_sinl", "_truncl",
]
threading_package_dependencies = [
    "_pthread_mutex_destroy", "_pthread_cond_broadcast",
    "_pthread_get_stacksize_np", "_pthread_key_create", "_pthread_self",
    "_pthread_equal", "_pthread_mutex_lock", "_pthread_getspecific",
    "_pthread_mutex_unlock", "_pthread_cond_wait", "_pthread_setspecific",
    "_pthread_get_stackaddr_np"
]
common_expected_dependencies = [
    "___bzero", "___divti3", "___error", "___stderrp", "___stdoutp",
    "___truncsfhf2", "___udivti3", "_abort", "_arc4random_buf",
    "_calloc", "_close", "_fflush", "_flockfile", "_fprintf",
    "_fputc", "_fputs", "_free", "_funlockfile", "_fwrite", "_malloc",
    "_malloc_size", "_memchr", "_memcmp", "_memcpy", "_memmove", "_memset",
    "_posix_memalign", "_putc", "_read", "_realloc", "_snprintf", "_strchr",
    "_strcmp", "_strdup", "_strlen", "_strncmp", "_strtod", "_strtof",
    "_strtol", "_strtold", "_vprintf", "_vsnprintf", "_write",
] + cxx_dependencies + math_dependencies
vendor_apple_specific_dependencies = [
    "___stack_chk_fail", "___stack_chk_guard",
    "_getsectiondata", "__dyld_register_func_for_add_image",
]
################################################################################


if args.vendor == "apple":
    vendor_specific_dependencies = vendor_apple_specific_dependencies

    nm = args.nm_path
    lines = subprocess.check_output(
        [nm, "--portability", "--undefined-only", args.library]) \
        .decode("utf-8").strip().splitlines()
    deps = [line.split(" ")[0] for line in lines if " U " in line]
    print("")
else:
    print("vendor {} not handled yet".format(args.vendor))
    sys.exit(1)

deps = [dep for dep in deps if not dep.startswith("_$")]
deps = [dep for dep in deps if not dep.startswith("__Z")]
deps = [dep for dep in deps if not dep.startswith("section$start$")]
deps = [dep for dep in deps if not dep.startswith("section$end$")]
deps = [dep for dep in deps if not dep.startswith("___swift_stdlib")]
deps = [dep for dep in deps if not dep.startswith("__swift_stdlib")]
deps = [dep for dep in deps if not dep.startswith("__swift")]
deps = [dep for dep in deps if not dep.startswith("_swift_")]
deps = [dep for dep in deps if not dep.startswith("__stdlib_")]
deps = [dep for dep in deps if not dep.startswith("_getSuperclassMetadata")]

deps = set(deps)

print("libswiftCore.a dependencies:")
print("\n".join(sorted(deps)))
print("")

# for sanity checking that we are getting a valid symbol list
required_dependencies = ["_malloc", "_free"]

fail = False
for symbol in required_dependencies:
    if symbol not in deps:
        print("Error: Required dependency '{}' missing".format(symbol))
        fail = True

allowlist = set(common_expected_dependencies + vendor_specific_dependencies +
                threading_package_dependencies)
for symbol in deps:
    if symbol not in allowlist:
        print("Error: Unexpected dependency '{}'".format(symbol))
        fail = True

for symbol in allowlist:
    if symbol not in deps:
        print("Warning: Allowed dependency '{}' not present".format(symbol))

print("")
print("All checks done. Result: {}".format("FAIL" if fail else "SUCCESS"))
sys.exit(1 if fail else 0)
