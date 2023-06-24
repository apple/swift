// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/cas

/// Doesn't run because the command doesn't have CAS enabled.
// RUN: not %cache-tool -cas-path %t/cas -cache-tool-action print-base-key -- \
// RUN:   %target-swift-frontend %s -c -allow-unstable-cache-key-for-testing 2>&1 | \
// RUN:   %FileCheck %s --check-prefix=NO-CAS

// NO-CAS: Requested command-line arguments do not enable CAS

/// Check few working cases.
// RUN: %cache-tool -cas-path %t/cas -cache-tool-action print-base-key -- \
// RUN:   %target-swift-frontend -enable-cas %s -c -allow-unstable-cache-key-for-testing > %t1.casid
/// A different CAS doesn't affect base key.
// RUN: %cache-tool -cas-path %t/cas -cache-tool-action print-base-key -- \
// RUN:   %target-swift-frontend -enable-cas %s -c -allow-unstable-cache-key-for-testing -cas-path %t > %t2.casid
/// Output path doesn't affect base key.
// RUN: %cache-tool -cas-path %t/cas -cache-tool-action print-base-key -- \
// RUN:   %target-swift-frontend -enable-cas %s -c -allow-unstable-cache-key-for-testing -o %t/test.o > %t3.casid
/// Add -D will change.
// RUN: %cache-tool -cas-path %t/cas -cache-tool-action print-base-key -- \
// RUN:   %target-swift-frontend -enable-cas %s -c -allow-unstable-cache-key-for-testing -DTEST > %t4.casid

// RUN: diff %t1.casid %t2.casid
// RUN: diff %t1.casid %t3.casid
// RUN: not diff %t1.casid %t4.casid

/// Check filelist option.
// RUN: echo "%s" > %t/filelist-1
// RUN: echo "%s" > %t/filelist-2
// RUN: cp %s %t/temp.swift
// RUN: echo "%t/temp.swift" > %t/filelist-3
// RUN: %cache-tool -cas-path %t/cas -cache-tool-action print-base-key -- \
// RUN:   %target-swift-frontend -enable-cas -filelist %t/filelist-1 -c -allow-unstable-cache-key-for-testing > %t5.casid
// RUN: %cache-tool -cas-path %t/cas -cache-tool-action print-base-key -- \
// RUN:   %target-swift-frontend -enable-cas -filelist %t/filelist-2 -c -allow-unstable-cache-key-for-testing > %t6.casid
// RUN: %cache-tool -cas-path %t/cas -cache-tool-action print-base-key -- \
// RUN:   %target-swift-frontend -enable-cas -filelist %t/filelist-3 -c -allow-unstable-cache-key-for-testing > %t7.casid
// RUN: diff %t5.casid %t6.casid
// RUN: not diff %t5.casid %t7.casid

/// Test output keys.
// RUN: %cache-tool -cas-path %t/cas -cache-tool-action print-output-keys -- \
// RUN:   %target-swift-frontend -enable-cas %s -emit-module -c -emit-dependencies \
// RUN:   -emit-tbd -emit-tbd-path %t/test.tbd -o %t/test.o -allow-unstable-cache-key-for-testing | %FileCheck %s

// CHECK: test.o
// CHECK-NEXT: "OutputKind": "object"
// CHECK-NEXT: "Input"
// CHECK-NEXT: "CacheKey"

// CHECK: test.swiftmodule
// CHECK-NEXT: "OutputKind": "swiftmodule"
// CHECK-NEXT: "Input"
// CHECK-NEXT: "CacheKey"

// CHECK: test.d
// CHECK-NEXT: "OutputKind": "dependencies"
// CHECK-NEXT: "Input"
// CHECK-NEXT: "CacheKey"

// CHECK: test.tbd
// CHECK-NEXT: "OutputKind": "tbd"
// CHECK-NEXT: "Input"
// CHECK-NEXT: "CacheKey"

// CHECK: <cached-diagnostics>
// CHECK-NEXT: "OutputKind": "cached-diagnostics"
// CHECK-NEXT: "Input": "<cached-diagnostics>"
// CHECK-NEXT: "CacheKey"
