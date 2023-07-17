// REQUIRES: VENDOR=apple
// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/frameworks)
// RUN: %empty-directory(%t/overlaydir)

// RUN: cp -rf %S/Inputs/frameworks/Simple.framework %t/frameworks/

// Creates a framework with a Clang module and a .tbd as well as a Swift
// overlay. Only the .swiftmodule has a version; the .tbd is empty. When
// `_underlyingVersion` is specified in the conditional a diagnostic should be
// emitted and the conditional ignored. When `_version` is specified there
// should be no diagnostics and the version in the `.swiftmodule` should be
// honored.

// RUN: rm -rf %t/frameworks/Simple.framework/Simple.tbd
// RUN: echo "@_exported import Simple" > %t.overlay.swift
// RUN: echo "public func additional() {}" >> %t.overlay.swift

// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -enable-objc-interop -disable-implicit-concurrency-module-import -module-name Simple -F %t/frameworks/ %t.overlay.swift -user-module-version 3 -emit-module-path %t/overlaydir/Simple.swiftmodule
// RUN: %target-typecheck-verify-swift -disable-implicit-concurrency-module-import -I %t/overlaydir/ -F %t/frameworks

import Simple

func canImportUnderlyingVersion() {
#if canImport(Simple, _underlyingVersion: 2) // expected-warning {{cannot find user version number for Clang module 'Simple'; version number ignored}}
  let a = 1  // expected-warning {{initialization of immutable value 'a' was never used; consider replacing with assignment to '_' or removing it}}
#endif

#if canImport(Simple, _underlyingVersion: 3) // expected-warning {{cannot find user version number for Clang module 'Simple'; version number ignored}}
  let b = 1 // expected-warning {{initialization of immutable value 'b' was never used; consider replacing with assignment to '_' or removing it}}
#endif
  
#if canImport(Simple, _underlyingVersion: 4) // expected-warning {{cannot find user version number for Clang module 'Simple'; version number ignored}}
  let c = 1 // expected-warning {{initialization of immutable value 'c' was never used; consider replacing with assignment to '_' or removing it}}
#endif

}

func canImportVersion() {
#if canImport(Simple, _version: 2)
  let a = 1  // expected-warning {{initialization of immutable value 'a' was never used; consider replacing with assignment to '_' or removing it}}
#endif

#if canImport(Simple, _version: 3)
  let b = 1 // expected-warning {{initialization of immutable value 'b' was never used; consider replacing with assignment to '_' or removing it}}
#endif
  
#if canImport(Simple, _version: 4)
  let c = 1
#endif
}
