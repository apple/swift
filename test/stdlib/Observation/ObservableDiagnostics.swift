// REQUIRES: swift_swift_parser

// RUN: %target-typecheck-verify-swift -disable-availability-checking -parse-as-library -enable-experimental-feature Macros -enable-experimental-feature ExtensionMacros -plugin-path %swift-host-lib-dir/plugins

// REQUIRES: observation
// REQUIRES: concurrency
// REQUIRES: objc_interop
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime
// REQUIRES: rdar112623492

import Observation

@Observable // expected-error {{'@Observable' cannot be applied to actor type 'MyActor'}}
actor MyActor {}

@Observable // expected-error {{'@Observable' cannot be applied to enumeration type 'MyEnum'}}
enum MyEnum {
  case myCase // required for macro invocation
}

@Observable // expected-error {{'@Observable' cannot be applied to struct type 'MyStruct'}}
struct MyStruct {}
