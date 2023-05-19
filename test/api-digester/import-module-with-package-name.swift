// REQUIRES: VENDOR=apple

// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t.mod)
// RUN: %empty-directory(%t.sdk)
// RUN: %empty-directory(%t.module-cache)
// RUN: split-file %s %t

// Make sure api-digester loads an interface with package-name correctly

// Generate module Lib
// RUN: %target-swift-frontend  %t/Lib.swift -emit-module -module-name Lib -emit-module-path %t.mod/Lib.swiftmodule -emit-module-interface-path %t.mod/Lib.swiftinterface -emit-private-module-interface-path %t.mod/Lib.private.swiftinterface -package-name myLib -parse-as-library -enable-library-evolution -module-cache-path %t.module-cache -swift-version 5

// Dumping sdk for Lib ABI via .swiftmodule file should work
// RUN: %api-digester -dump-sdk -abort-on-module-fail -abi -module Lib -o - -module-cache-path %t.module-cache -I %t.mod > %t.dump-lib-binary.json
// RUN: %FileCheck -check-prefix=LIB-BINARY %s < %t.dump-lib-binary.json
// LIB-BINARY: PkgKlass
// LIB-BINARY: pkgFunc
// LIB-BINARY: PublicStruct
// LIB-BINARY: PkgKlassUFI
// LIB-BINARY: libFunc

// Dumping sdk file for Lib ABI via .swiftinterface file should work
// RUN: %api-digester -dump-sdk -abort-on-module-fail -abi -module Lib -use-interface-for-module Lib -o - -module-cache-path %t.module-cache -I %t.mod > %t.dump-lib-interface.json
// RUN: %FileCheck -check-prefix=LIB-INTERFACE %s < %t.dump-lib-interface.json
// LIB-INTERFACE-NOT: PkgKlass
// LIB-INTERFACE-NOT: pkgFunc
// LIB-INTERFACE: PublicStruct
// LIB-INTERFACE: PkgKlassUFI
// LIB-INTERFACE: libFunc

// Generate module Client
// RUN: %target-swift-frontend %t/Client.swift -emit-module -module-name Client -emit-module-path %t.mod/Client.swiftmodule -emit-module-interface-path %t.mod/Client.swiftinterface -emit-private-module-interface-path %t.mod/Client.private.swiftinterface -package-name myLib -parse-as-library -enable-library-evolution -module-cache-path %t.module-cache -swift-version 5  -I %t.mod

// RUN: rm -f %t.mod/Lib.swiftmodule
// RUN: rm -f %t.module-cache/Lib*.swiftmodule

// Dumping sdk for Client ABI via .swiftmodule file should work
// RUN: %api-digester -dump-sdk -abort-on-module-fail -abi -module Client -o - -module-cache-path %t.module-cache -I %t.mod > %t.dump-client-binary.json
// RUN: %FileCheck -check-prefix=CLIENT-BINARY %s < %t.dump-client-binary.json
// CLIENT-BINARY-NOT: PkgKlass
// CLIENT-BINARY-NOT: pkgFunc
// CLIENT-BINARY-NOT: PublicStruct
// CLIENT-BINARY-NOT: PkgKlassUFI
// CLIENT-BINARY-NOT: libFunc
// CLIENT-BINARY: clientFunc
// CLIENT-BINARY: clientFuncInlinable

// Dumping sdk for Client ABI via .swiftinterface file should work
// RUN: %api-digester -dump-sdk -abort-on-module-fail -abi -module Client -use-interface-for-module Client -o - -module-cache-path %t.module-cache -I %t.mod > %t.dump-client-interface.json
// RUN: %FileCheck -check-prefix=CLIENT-INTERFACE %s < %t.dump-client-interface.json
// CLIENT-INTERFACE-NOT: PkgKlass
// CLIENT-INTERFACE-NOT: pkgFunc
// CLIENT-INTERFACE-NOT: PublicStruct
// CLIENT-INTERFACE-NOT: PkgKlassUFI
// CLIENT-INTERFACE-NOT: libFunc
// CLIENT-INTERFACE: clientFunc
// CLIENT-INTERFACE: clientFuncInlinable


//--- Lib.swift
package class PkgKlass {
  package class func foo() {}
  package func foo2(_ : Int) {}
  package weak var bar : PkgKlass?
  package var bar2 : PkgKlass?
}

package func pkgFunc() -> (PkgKlass) -> () { return { _ in } }

public struct PublicStruct {
  package init(_ : PkgKlass?) {}
  public static func baz(_ arg: String?) {}
}

@usableFromInline
package struct PkgKlassUFI {
  @usableFromInline
  package init() {}
  @usableFromInline
  package func ufiFunc() {}
}

@inlinable
public func libFunc() {
  PkgKlassUFI().ufiFunc()
}

//--- Client.swift
import Lib

public func clientFunc() {
  PkgKlass.foo()
  let result = pkgFunc()
  let s = PublicStruct(nil)
  PublicStruct.baz("")
  print(s, result)
}

@inlinable
public func clientFuncInlinable() {
  let x = PkgKlassUFI()
  libFunc()
  print(x)
}
