// RUN: %empty-directory(%t)
// RUN: %target-build-swift-dylib(%t/%target-library-name(TestDidWillSet)) -module-name TestDidWillSet -emit-module -emit-module-path %t/TestDidWillSet.swiftmodule -swift-version 5 %S/Inputs/dynamic_replacement_property_observer_orig.swift -Xfrontend -enable-private-imports -Xfrontend -enable-implicit-dynamic
// RUN: %target-build-swift-dylib(%t/%target-library-name(TestDidWillSet2)) -I%t -L%t -lTestDidWillSet %target-rpath(%t) -module-name TestDidWillSet2 -swift-version 5 %S/Inputs/dynamic_replacement_property_observer_repl.swift
// RUN: %target-build-swift -I%t -L%t -lTestDidWillSet -o %t/main %target-rpath(%t) %s -swift-version 5
// RUN: %target-codesign %t/main %t/%target-library-name(TestDidWillSet) %t/%target-library-name(TestDidWillSet2)
// RUN: %target-run %t/main %t/%target-library-name(TestDidWillSet) %t/%target-library-name(TestDidWillSet)

// REQUIRES: executable_test

// UNSUPPORTED: swift_test_mode_optimize_none_with_implicit_dynamic
// Dynamic replacement is not supported when targeting Wasm.
// UNSUPPORTED: wasm

@_private(sourceFile: "dynamic_replacement_property_observer_orig.swift") import TestDidWillSet

#if canImport(Darwin)
  import Darwin
#elseif canImport(Glibc)
  import Glibc
#elseif canImport(Android)
  import Android
#elseif os(Windows)
  import CRT
  import WinSDK
#else
#error("Unsupported platform")
#endif

private func target_library_name(_ name: String) -> String {
#if canImport(Darwin)
  return "lib\(name).dylib"
#elseif os(Windows)
  return "\(name).dll"
#else
  return "lib\(name).so"
#endif
}

var s = Stored(i: 5,  y: 5, z: 5)
var h = HeapStored()

// CHECK: Stored.i.didSet from 5 to 10 original
s.i = 10
// CHECK: Stored.y.willSet from 5 to 11 original
s.y = 11
// CHECK: Stored.z.willSet from 5 to 12 original
// CHECK: Stored.z.didSet from 5 to 12 original
s.z = 12

// CHECK: HeapStored.z.willSet from 5 to 16 original
// CHECK: HeapStored.z.didSet from 5 to 16 original
h.z = 16

// CHECK: myglobal.didSet from 1 to 13 original
myglobal = 13
// CHECK: myglobal2.willSet from 1 to 14 original
myglobal2 = 14
// CHECK: myglobal3.willSet from 1 to 15 original
// CHECK: myglobal3.didSet from 1 to 15 original
myglobal3 = 15

var executablePath = CommandLine.arguments[0]
executablePath.removeLast(4)

// Now, test with the module containing the replacements.

#if os(Linux)
	_ = dlopen(target_library_name("Module2"), RTLD_NOW)
#elseif os(Windows)
        _ = LoadLibraryA(target_library_name("Module2"))
#else
	_ = dlopen(executablePath+target_library_name("Module2"), RTLD_NOW)
#endif

// CHECK: Stored.i.didSet from 5 to 10 replacement
s.i = 10
// CHECK: Stored.y.willSet from 5 to 11 replacement
s.y = 11
// CHECK: Stored.z.willSet from 5 to 12 replacement
// CHECK: Stored.z.didSet from 5 to 12 replacement
s.z = 12

// CHECK: HeapStored.z.willSet from 5 to 16 replacement
// CHECK: HeapStored.z.didSet from 5 to 16 replacement
h.z = 16

// CHECK: myglobal.didSet from 1 to 13 replacement
myglobal = 13
// CHECK: myglobal2.willSet from 1 to 14 replacement
myglobal2 = 14
// CHECK: myglobal3.willSet from 1 to 15 replacement
// CHECK: myglobal3.didSet from 1 to 15 replacement
myglobal3 = 15
