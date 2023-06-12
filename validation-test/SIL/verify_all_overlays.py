#!/usr/bin/env python3
# RUN: ${python} %s %target-swiftmodule-name %platform-sdk-overlay-dir \
# RUN:     %swift_src_root \
# RUN:     %target-sil-opt -sdk %sdk -enable-sil-verify-all \
# RUN:       -F %sdk/System/Library/PrivateFrameworks \
# RUN:       -F "%xcode-extra-frameworks-dir"

# REQUIRES: long_test
# REQUIRES: nonexecutable_test


import os
import subprocess
import sys

target_swiftmodule_name = sys.argv[1]
sdk_overlay_dir = sys.argv[2]
source_dir = sys.argv[3]
sil_opt_invocation = sys.argv[4:]

for module_file in os.listdir(sdk_overlay_dir):
    extra_args = []
    module_name, ext = os.path.splitext(module_file)
    if ext != ".swiftmodule":
        continue
    # Skip the standard library because it's tested elsewhere.
    if module_name == "Swift":
        continue
    # Skip the C++ standard library overlay because it's not yet shipped
    # in any released SDK.
    if module_name == "CxxStdlib":
        continue
    # TODO(TF-1229): Fix the "_Differentiation" module.
    if module_name == "_Differentiation":
        continue
    # TODO: fix the DifferentiationUnittest module.
    if module_name == "DifferentiationUnittest":
        continue
    # Backtracing needs its own additional modules in the module path
    if module_name == "_Backtracing":
        extra_args = ["-I", os.path.join(source_dir, "stdlib",
                                         "public", "Backtracing", "modules"),
                      "-I", os.path.join(source_dir, "include")]

    print("# " + module_name)

    module_path = os.path.join(sdk_overlay_dir, module_file)
    if os.path.isdir(module_path):
        module_path = os.path.join(module_path, target_swiftmodule_name)

    # llvm-bcanalyzer | not grep Unknown
    bcanalyzer_output = subprocess.check_output(["llvm-bcanalyzer",
                                                 module_path]).decode("utf-8")
    if "Unknown" in bcanalyzer_output:
        print(bcanalyzer_output)
        sys.exit(1)

    # sil-opt
    # We are deliberately discarding the output here; we're just making sure
    # it can be generated.
    subprocess.check_output(sil_opt_invocation +
                            [module_path, "-module-name", module_name] +
                            extra_args)
