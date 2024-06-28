// RUN: %empty-directory(%t)
// RUN: %target-build-swift-dylib(%t/%target-library-name(Module1)) -module-name Module1 -emit-module -emit-module-path %t/Module1.swiftmodule -swift-version 5 %S/Inputs/dynamic_replacement_without_previous_calls1.swift
// RUN: %target-build-swift-dylib(%t/%target-library-name(Module2)) -I%t -L%t -lModule1 %target-rpath(%t) -module-name Module2 -emit-module -emit-module-path %t/Module2.swiftmodule -swift-version 5 %S/Inputs/dynamic_replacement_without_previous_calls2.swift -Xfrontend -disable-previous-implementation-calls-in-dynamic-replacements
// RUN: %target-build-swift -I%t -L%t -lModule1 -o %t/main %target-rpath(%t) %s -swift-version 5
// RUN: %target-codesign %t/main %t/%target-library-name(Module1) %t/%target-library-name(Module2)
// RUN: %target-run %t/main %t/%target-library-name(Module1) %t/%target-library-name(Module2)

// REQUIRES: executable_test
// UNSUPPORTED: wasm

import Module1

import StdlibUnittest

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

var DynamicallyReplaceable = TestSuite("DynamicallyReplaceable")

DynamicallyReplaceable.test("DynamicallyReplaceable") {
  var executablePath = CommandLine.arguments[0]
  executablePath.removeLast(4)

  // First, test with only the original module.
  expectEqual(0, selfRec(2, 0))
  expectEqual(0, AClass().selfRec(2, 0))
  expectEqual(0, AStruct().selfRec(2, 0))

  // Now, test with the module containing the replacements.
#if os(Linux)
	_ = dlopen(target_library_name("Module2"), RTLD_NOW)
#elseif os(Windows)
        _ = LoadLibraryA(target_library_name("Module2"))
#else
	_ = dlopen(executablePath+target_library_name("Module2"), RTLD_NOW)
#endif
  expectEqual(2, selfRec(2, 0))
  expectEqual(2, AClass().selfRec(2, 0))
  expectEqual(2, AStruct().selfRec(2, 0))
}

runAllTests()
