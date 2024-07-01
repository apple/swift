// REQUIRES: asserts

// RUN: %target-swift-frontend -emit-supported-features %s -enable-experimental-feature SupportedFeaturesTest | %FileCheck %s

// CHECK: "SupportedArguments"
// CHECK: "abi"
// CHECK: "emit-module"
// CHECK: "LastOption"
// CHECK: "SupportedFeatures"
// CHECK: "AsyncAwait"
// CHECK: "SupportedFeaturesTest
// CHECK: "LastFeature"
