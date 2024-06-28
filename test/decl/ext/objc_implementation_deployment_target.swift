// Hardcode x86_64 macOS because Apple Silicon was born ABI-stable
// RUN: %target-typecheck-verify-swift -import-objc-header %S/Inputs/objc_implementation.h -target x86_64-apple-macosx10.14.3 -enable-experimental-feature ObjCImplementation
// REQUIRES: objc_interop
// REQUIRES: OS=macosx

@objc @implementation extension ObjCImplSubclass {
  // expected-error@-1 {{'@implementation' of an Objective-C class requires a minimum deployment target of at least macOS 10.14.4}}
}

@objc(Conformance) @implementation extension ObjCClass {
  // expected-error@-1 {{'@implementation' of an Objective-C class requires a minimum deployment target of at least macOS 10.14.4}}
  func requiredMethod1() {}
  func requiredMethod2() {}
}

@_objcImplementation(EmptyCategory) extension ObjCClass {
  // expected-warning@-1 {{'@implementation' of an Objective-C class requires a minimum deployment target of at least macOS 10.14.4; this will become an error after adopting '@implementation'}}
  // expected-warning@-2 {{'@_objcImplementation' is deprecated; use '@implementation' instead}}
}
