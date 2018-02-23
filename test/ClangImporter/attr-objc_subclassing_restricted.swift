// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -typecheck -import-objc-header %S/Inputs/attr-objc_subclassing_restricted.h %s -swift-version 5 -verify

// No errors in Swift 3 and 4 modes.
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -typecheck -import-objc-header %S/Inputs/attr-objc_subclassing_restricted.h %s -swift-version 4

// REQUIRES: objc_interop

class Sub: Restricted { // expected-error {{cannot inherit from non-open class 'Restricted' outside of its defining module}}
}
