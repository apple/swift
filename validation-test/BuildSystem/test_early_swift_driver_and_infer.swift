# REQUIRES: standalone_build
# REQUIRES: OS=macosx

# RUN: %empty-directory(%t)
# RUN: mkdir -p %t
# RUN: SKIP_XCODE_VERSION_CHECK=1 SWIFT_BUILD_ROOT=%t %swift_src_root/utils/build-script --dry-run --swiftpm --infer 2>&1 | %FileCheck %s

# CHECK: --- Building earlyswiftdriver ---
