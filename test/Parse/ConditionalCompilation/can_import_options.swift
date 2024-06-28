// RUN: %empty-directory(%t)
// RUN: %target-typecheck-verify-swift -disable-implicit-concurrency-module-import \
// RUN:   -disable-implicit-string-processing-module-import \
// RUN:   -module-can-import Foo -module-can-import-version Bar 113.330.1.2 0.0 \
// RUN:   -module-can-import-version Baz 113.330.1.2 113.330.1.2

func canImport() {
#if canImport(Foo)
  let basicCheck = 1 // expected-warning {{initialization of immutable value 'basicCheck' was never used; consider replacing with assignment to '_' or removing it}}
#endif

#if canImport(Foo, _version: 1)
  // No actual Foo to be imported since it is not versioned.
  let versionCheck = 1
#endif
}

func canImportVersioned() {
#if canImport(Bar, _version: 0)
  let majorZero = 1 // expected-warning {{initialization of immutable value 'majorZero' was never used; consider replacing with assignment to '_' or removing it}}
#endif

#if canImport(Bar, _version: 112)
  let majorSmaller = 1 // expected-warning {{initialization of immutable value 'majorSmaller' was never used; consider replacing with assignment to '_' or removing it}}
#endif
  
#if canImport(Bar, _version: 113)
  let majorEqual = 1 // expected-warning {{initialization of immutable value 'majorEqual' was never used; consider replacing with assignment to '_' or removing it}}
#endif

#if canImport(Bar, _version: 114)
  let majorLarger = 1
#endif
  
#if canImport(Bar, _version: 113.329)
  let minorSmaller = 1 // expected-warning {{initialization of immutable value 'minorSmaller' was never used; consider replacing with assignment to '_' or removing it}}
#endif

#if canImport(Bar, _version: 113.330)
  let minorEqual = 1 // expected-warning {{initialization of immutable value 'minorEqual' was never used; consider replacing with assignment to '_' or removing it}}
#endif

#if canImport(Bar, _version: 113.331)
  let minorLarger = 1
#endif
  
#if canImport(Bar, _version: 113.330.0)
  let patchSmaller = 1 // expected-warning {{initialization of immutable value 'patchSmaller' was never used; consider replacing with assignment to '_' or removing it}}
#endif

#if canImport(Bar, _version: 113.330.1)
  let patchEqual = 1 // expected-warning {{initialization of immutable value 'patchEqual' was never used; consider replacing with assignment to '_' or removing it}}
#endif
  
#if canImport(Bar, _version: 113.330.2)
  let patchLarger = 1
#endif

#if canImport(Bar, _version: 113.330.1.1)
  let buildSmaller = 1 // expected-warning {{initialization of immutable value 'buildSmaller' was never used; consider replacing with assignment to '_' or removing it}}
#endif

#if canImport(Bar, _version: 113.330.1.2)
  let buildEqual = 1 // expected-warning {{initialization of immutable value 'buildEqual' was never used; consider replacing with assignment to '_' or removing it}}
#endif
  
#if canImport(Bar, _version: 113.330.1.3)
  let buildLarger = 1
#endif
  
#if canImport(Bar, _version: 113.330.1.2.0) // expected-warning {{trailing components of version '113.330.1.2' are ignored}}
  let extraComponent = 1 // expected-warning {{initialization of immutable value 'extraComponent' was never used; consider replacing with assignment to '_' or removing it}}
#endif

#if canImport(Bar, _underlyingVersion: 113.33)
  // Bar is a Swift module with no underlying clang module.
  let underlyingMinorSmaller = 1
#endif

#if canImport(Bar)
  let noVersion = 1 // expected-warning {{initialization of immutable value 'noVersion' was never used; consider replacing with assignment to '_' or removing it}}
#endif
}

func canImportUnderlyingVersion() {
#if canImport(Baz, _underlyingVersion: 0)
  let majorZero = 1 // expected-warning {{initialization of immutable value 'majorZero' was never used; consider replacing with assignment to '_' or removing it}}
#endif

#if canImport(Baz, _underlyingVersion: 112)
  let majorSmaller = 1 // expected-warning {{initialization of immutable value 'majorSmaller' was never used; consider replacing with assignment to '_' or removing it}}
#endif
  
#if canImport(Baz, _underlyingVersion: 113)
  let majorEqual = 1 // expected-warning {{initialization of immutable value 'majorEqual' was never used; consider replacing with assignment to '_' or removing it}}
#endif

#if canImport(Baz, _underlyingVersion: 114)
  let majorLarger = 1
#endif
  
#if canImport(Baz, _underlyingVersion: 113.329)
  let minorSmaller = 1 // expected-warning {{initialization of immutable value 'minorSmaller' was never used; consider replacing with assignment to '_' or removing it}}
#endif

#if canImport(Baz, _underlyingVersion: 113.330)
  let minorEqual = 1 // expected-warning {{initialization of immutable value 'minorEqual' was never used; consider replacing with assignment to '_' or removing it}}
#endif

#if canImport(Baz, _underlyingVersion: 113.331)
  let minorLarger = 1
#endif
  
#if canImport(Baz, _underlyingVersion: 113.330.0)
  let patchSmaller = 1 // expected-warning {{initialization of immutable value 'patchSmaller' was never used; consider replacing with assignment to '_' or removing it}}
#endif

#if canImport(Baz, _underlyingVersion: 113.330.1)
  let patchEqual = 1 // expected-warning {{initialization of immutable value 'patchEqual' was never used; consider replacing with assignment to '_' or removing it}}
#endif
  
#if canImport(Baz, _underlyingVersion: 113.330.2)
  let patchLarger = 1
#endif

#if canImport(Baz, _underlyingVersion: 113.330.1.1)
  let buildSmaller = 1 // expected-warning {{initialization of immutable value 'buildSmaller' was never used; consider replacing with assignment to '_' or removing it}}
#endif

#if canImport(Baz, _underlyingVersion: 113.330.1.2)
  let buildEqual = 1 // expected-warning {{initialization of immutable value 'buildEqual' was never used; consider replacing with assignment to '_' or removing it}}
#endif
  
#if canImport(Baz, _underlyingVersion: 113.330.1.3)
  let buildLarger = 1
#endif
  
#if canImport(Baz, _underlyingVersion: 113.330.1.2.0) // expected-warning {{trailing components of version '113.330.1.2' are ignored}}
  let extraComponent = 1 // expected-warning {{initialization of immutable value 'extraComponent' was never used; consider replacing with assignment to '_' or removing it}}
#endif

#if canImport(Baz, _version: 113.33)
  let version = 1 // expected-warning {{initialization of immutable value 'version' was never used; consider replacing with assignment to '_' or removing it}}
#endif
}
