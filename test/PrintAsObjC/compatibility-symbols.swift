// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) %S/../Inputs/empty.swift -typecheck -emit-objc-header-path %t/empty.h
// RUN: %clang -extract-api -o %t/empty.symbols.json --target=%target-triple -isysroot %clang-importer-sdk-path -F %clang-importer-sdk-path/frameworks --extract-api-ignores=%swift-share-dir/swift/compatibility-symbols -fmodules -x objective-c-header %t/empty.h
// RUN: %FileCheck %s --input-file %t/empty.symbols.json

// REQUIRES: objc_interop

// Make sure that any macros or typedefs added to the Clang compatibility header are reflected in
// the `comptibility-symbols` file that is installed in the toolchain.

// CHECK: "symbols": []
