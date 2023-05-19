// RUN: %target-swift-frontend -print-target-info -runtime-compatibility-version 5.8 | %FileCheck %s

// REQUIRES: OS=macosx

// CHECK:       "libraryName": "swiftCompatibilityPacks",
// CHECK-NEXT:  "filter": "all",
// CHECK-NEXT:  "forceLoad": false
