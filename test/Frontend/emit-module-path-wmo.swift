// RUN: %empty-directory(%t)
// Default output:
// RUN: pushd %t
// RUN: %target-swift-frontend -emit-module -module-name foo -whole-module-optimization %s
// RUN: test -f %t/foo.swiftmodule
// RUN: popd

// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -module-name foo -emit-module-path %t/bar.swiftmodule -whole-module-optimization %s
// RUN: test -f %t/bar.swiftmodule
// RUN: not test -f %t/foo.swiftmodule
