#!/usr/bin/python

# Note that this test should still "pass" when no swiftinterfaces have been
# generated.

# RUN: %empty-directory(%t)
# RUN: %{python} %s %target-os %module-target-triple %platform-sdk-overlay-dir %t \
# RUN:   %target-swift-frontend -build-module-from-parseable-interface \
# RUN:     -Fsystem %sdk/System/Library/PrivateFrameworks/ \
# RUN:     | sort > %t/failures.txt
# RUN: grep '# %target-os:' %s > %t/filter.txt || true
# RUN: test ! -e %t/failures.txt || \
# RUN:   diff %t/filter.txt %t/failures.txt

# REQUIRES: nonexecutable_test, no_asan
# REQUIRES: swift_stable_abi

# Expected failures by platform
# -----------------------------
# macosx: XCTest
# ios: XCTest
# tvos: XCTest

from __future__ import print_function

import os
import subprocess
import sys

target_os = sys.argv[1]
target_module_triple = sys.argv[2]
sdk_overlay_dir = sys.argv[3]
output_dir = sys.argv[4]
compiler_invocation = sys.argv[5:]

for filename in os.listdir(sdk_overlay_dir):
    module_name, ext = os.path.splitext(filename)
    if ext == ".swiftinterface":
        interface_file = os.path.join(sdk_overlay_dir, filename)
    elif ext == ".swiftmodule":
        module_path = os.path.join(sdk_overlay_dir, filename)
        if os.path.isdir(module_path):
            interface_file = os.path.join(module_path,
                                          target_module_triple + ".swiftinterface")
        else:
            continue
    else:
        continue

    if module_name in [
        "DifferentiationUnittest",
        "Swift",
        "SwiftLang",
    ]:
        continue

    # swift -build-module-from-parseable-interface
    output_path = os.path.join(output_dir, module_name + ".swiftmodule")
    status = subprocess.call(compiler_invocation +
                             ["-o", output_path, "-module-name", module_name,
                              interface_file])
    if status != 0:
        print("# " + target_os + ": " + module_name)
