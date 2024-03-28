// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-build-swift -O -wmo -Xfrontend -experimental-package-cmo -Xfrontend -experimental-allow-non-resilient-access -parse-as-library %t/Lib.swift -emit-module -emit-module-path %t/Lib.swiftmodule -module-name=Lib -package-name Pkg -I%t -enable-library-evolution

// RUN: %target-sil-opt %t/Lib.swiftmodule -o %t/Lib-sil-opt.sil
// RUN: %FileCheck %s --check-prefix=CHECK-LIB < %t/Lib-sil-opt.sil

// RUN: %target-build-swift -module-name=Main -package-name Pkg -I%t -emit-sil %t/main.swift -o %t/Main.sil
// RUN: %FileCheck %s < %t/Main.sil

// REQUIRES: swift_in_compiler


//--- main.swift

import Lib

// CHECK-NOT: witness_method
runPub([.root, .size])
runPkg([.root, .size])


//--- Lib.swift

public class PubKlass {
  public var pubVar: String = ""
  public init() {}
  public func pubFunc() {}
}

package class PkgKlass {
  package var pkgVar: String = ""
  package init() {}
  package func pkgFunc() {}
}

public protocol PubProto {
  associatedtype Element = Self
  static var root: Self { get }
  init(rawValue: UInt16)
}

public struct PubStruct: PubProto {
  public let rawValue: UInt16
  public init(rawValue: UInt16) { self.rawValue = rawValue }

  public static let root = Self(rawValue: 1 << 0)
  public static let env = Self(rawValue: 1 << 1)
  public static let size = Self(rawValue: 1 << 2)
}

public func runPub(_ arg: [PubStruct]) {
  print(arg)
}

package protocol PkgProto {
  associatedtype Element = Self
  static var root: Self { get }
  init(rawValue: UInt16)
}

package struct PkgStruct: PkgProto {
  package let rawValue: UInt16
  package init(rawValue: UInt16) { self.rawValue = rawValue }

  package static let root = Self(rawValue: 1 << 0)
  package static let env = Self(rawValue: 1 << 1)
  package static let size = Self(rawValue: 1 << 2)
}

package func runPkg(_ arg: [PkgStruct]) {
  print(arg)
}

//CHECK-LIB-LABEL: sil_vtable PubKlass {
//CHECK-LIB-NEXT: @$s3Lib8PubKlassC6pubVarSSvg
//CHECK-LIB-NEXT: @$s3Lib8PubKlassC6pubVarSSvs
//CHECK-LIB-NEXT: @$s3Lib8PubKlassC6pubVarSSvM
//CHECK-LIB-NEXT: @$s3Lib8PubKlassCACycfC
//CHECK-LIB-NEXT: @$s3Lib8PubKlassC7pubFuncyyF
//CHECK-LIB-NEXT: @$s3Lib8PubKlassCfD

//CHECK-LIB-LABEL: sil_vtable PkgKlass {
//CHECK-LIB-NEXT: @$s3Lib8PkgKlassC6pkgVarSSvg 
//CHECK-LIB-NEXT: @$s3Lib8PkgKlassC6pkgVarSSvs
//CHECK-LIB-NEXT: @$s3Lib8PkgKlassC6pkgVarSSvM
//CHECK-LIB-NEXT: @$s3Lib8PkgKlassCACycfC
//CHECK-LIB-NEXT: @$s3Lib8PkgKlassC7pkgFuncyyF
//CHECK-LIB-NEXT: @$s3Lib8PkgKlassCfD

//CHECK-LIB-LABEL: sil_witness_table PubStruct: PubProto module Lib {
//CHECK-LIB-NEXT:   associated_type Element: PubStruct
//CHECK-LIB-NEXT:   method #PubProto.root!getter: <Self where Self : PubProto> (Self.Type) -> () -> Self : nil
//CHECK-LIB-NEXT:   method #PubProto.init!allocator: <Self where Self : PubProto> (Self.Type) -> (UInt16) -> Self : nil

//CHECK-LIB-LABEL: sil_witness_table package PkgStruct: PkgProto module Lib {
//CHECK-LIB-NEXT:   associated_type Element: PkgStruct
//CHECK-LIB-NEXT:   method #PkgProto.root!getter: <Self where Self : PkgProto> (Self.Type) -> () -> Self : nil
//CHECK-LIB-NEXT:   method #PkgProto.init!allocator: <Self where Self : PkgProto> (Self.Type) -> (UInt16) -> Self : nil

//CHECK-LIB-LABEL: sil_default_witness_table PubProto {
//CHECK-LIB-NEXT:   associated_type Element: Self

//CHECK-LIB-LABEL: sil_default_witness_table package PkgProto {
//CHECK-LIB-NEXT:   associated_type Element: Self
