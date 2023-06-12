// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/NonAPI)
// RUN: %empty-directory(%t/API)
// RUN: split-file %s %t

// RUN: %target-swift-emit-module-interface(%t/NonAPI/Library.swiftinterface) %t/Library.swift -module-name Library -target %target-swift-abi-5.8-triple
// RUN: %target-swift-emit-module-interface(%t/API/Library.swiftinterface) %t/Library.swift -module-name Library -target %target-swift-abi-5.8-triple -library-level api

// Build Client.swift against the Library.swiftinterface without
// `-library-level api`. Since the deployment target of the library is
// SwiftStdlib 5.8, the newer overload that returns a String should be selected
// by overload resolution during the implicit module build.

// RUN: %target-build-swift %t/Client.swift -o %t/NonAPI/client -I %t/NonAPI/
// RUN: %target-codesign %t/NonAPI/client
// RUN: %target-run %t/NonAPI/client | %FileCheck %s --check-prefix=CHECK-NON-API

// Build Client.swift against the Library.swiftinterface with
// `-library-level api`. Since the deployment target of the client that will
// get a copy of `fragileFuncUsingOverload()` is earlier than SwiftStdlib 5.8,
// the older overload returning an Int should be selected during the implicit
// module build even though the library targets SwiftStdlib 5.8.

// RUN: %target-build-swift %t/Client.swift -o %t/API/client -I %t/API/
// RUN: %target-codesign %t/API/client
// RUN: %target-run %t/API/client | %FileCheck %s --check-prefix=CHECK-API

// REQUIRES: executable_test
// REQUIRES: OS=macosx || OS=ios || OS=tvos || OS=watchos

//--- Library.swift

@_disfavoredOverload
@_alwaysEmitIntoClient
public func overloadedFunc() -> Int {
  return 1234
}

@available(SwiftStdlib 5.8, *)
@_alwaysEmitIntoClient
public func overloadedFunc() -> String {
  return "String"
}

@_alwaysEmitIntoClient
public func fragileFuncUsingOverload() -> any CustomStringConvertible {
  return overloadedFunc()
}

//--- Client.swift

import Library

// CHECK-NON-API: String
// CHECK-API: 1234
print(fragileFuncUsingOverload())
