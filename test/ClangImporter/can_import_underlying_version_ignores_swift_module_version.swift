// REQUIRES: VENDOR=apple
// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/frameworks)
// RUN: %empty-directory(%t/overlaydir)

// RUN: cp -rf %S/Inputs/frameworks/Simple.framework %t/frameworks/

// Creates a framework with a Clang module and a .tbd as well as a Swift
// overlay. Both the .tbd and the .swiftmodule have versions, but they have
// different values. When `_underlyingVersion` is specified in the
// conditional, the version in the `.tbd` should be honored. When `_version`
// is specified, the version from the `.swiftmodule` should be honored.

// RUN: sed -i -e "s/1830\.100/3/g" %t/frameworks/Simple.framework/Simple.tbd
// RUN: echo "@_exported import Simple" > %t.overlay.swift
// RUN: echo "public func additional() {}" >> %t.overlay.swift

// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -enable-objc-interop -disable-implicit-concurrency-module-import -module-name Simple -F %t/frameworks/ %t.overlay.swift -user-module-version 5 -emit-module-path %t/overlaydir/Simple.swiftmodule
// RUN: %target-typecheck-verify-swift -disable-implicit-concurrency-module-import -I %t/overlaydir/ -F %t/frameworks

import Simple

func canImportUnderlyingVersion() {
#if canImport(Simple, _underlyingVersion: 2)
  let a = 1  // expected-warning {{initialization of immutable value 'a' was never used; consider replacing with assignment to '_' or removing it}}
#endif
  
#if canImport(Simple, _underlyingVersion: 3)
  let b = 1  // expected-warning {{initialization of immutable value 'b' was never used; consider replacing with assignment to '_' or removing it}}
#endif

#if canImport(Simple, _underlyingVersion: 4)
  let c = 1
#endif
}

func canImportVersion() {
#if canImport(Simple, _version: 4)
  let a = 1  // expected-warning {{initialization of immutable value 'a' was never used; consider replacing with assignment to '_' or removing it}}
#endif
  
#if canImport(Simple, _version: 5)
  let b = 1  // expected-warning {{initialization of immutable value 'b' was never used; consider replacing with assignment to '_' or removing it}}
#endif

#if canImport(Simple, _version: 6)
  let c = 1
#endif
}
