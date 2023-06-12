// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend %s -module-name UsesCxxInterop -emit-module -emit-module-path %t/UsesCxxInterop.swiftmodule -cxx-interoperability-mode=default

// RUN: %target-swift-frontend %s -D USE -typecheck -module-name TestMod -I %t -verify

// RUN: %target-swift-frontend %s -D USE -typecheck -module-name TestMod -I %t -disable-cxx-interop-requirement-at-import

// Verify that 'disable-cxx-interop-requirement-at-import' works for the module being built.
// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -module-name UsesCxxInterop -emit-module -emit-module-path %t/UsesCxxInterop.swiftmodule -cxx-interoperability-mode=default -disable-cxx-interop-requirement-at-import
// RUN: %target-swift-frontend %s -D USE -typecheck -module-name TestMod -I %t

#if USE
import UsesCxxInterop // expected-error {{module 'UsesCxxInterop' was built with C++ interoperability enabled, but current compilation does not enable C++ interoperability}}
// expected-note@-1 {{visit https://www.swift.org/documentation/cxx-interop/project-build-setup to learn how to enable C++ interoperability}}

#else
public func testFun() { }

#endif
