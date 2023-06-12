// RUN: %empty-directory(%t)

// FIXME: should work without syntize accessors too
// RUN: %target-swift-frontend -DSYNTHESIZE_ACCESSORS -enable-library-evolution -module-name Hello -emit-module -o %t/Hello.swiftmodule -emit-module-interface-path %t/Hello.swiftinterface %S/Inputs/moveonly_simple.swift

// rdar://106164128
// REQUIRES: rdar106164128

// TODO: finish this test by verifying the interface with FileCheck
