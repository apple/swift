// RUN: %target-typecheck-verify-swift -Xcc -fmodule-map-file=%S/Inputs/objc_implementation_private.modulemap
// REQUIRES: objc_interop

@_implementationOnly import objc_implementation_internal

@_objcImplementation extension InternalObjCClass {
  @objc public func method(fromHeader1: CInt) {
    // OK
  }
}
