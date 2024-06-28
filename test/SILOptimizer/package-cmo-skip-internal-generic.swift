// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-build-swift %t/Lib.swift \
// RUN: -module-name=Lib -package-name Pkg \
// RUN: -parse-as-library -emit-module -emit-module-path %t/Lib.swiftmodule -I%t \
// RUN: -Xfrontend -experimental-package-cmo -Xfrontend -experimental-allow-non-resilient-access \
// RUN: -O -wmo -enable-library-evolution
// RUN: %target-sil-opt %t/Lib.swiftmodule -sil-verify-all -o %t/Lib.sil

// RUN: %target-build-swift -module-name=Main -package-name Pkg -I%t -emit-sil -O -wmo  -Xllvm -sil-disable-pass=FunctionSignatureOpts %t/main.swift -o %t/Main.sil

// RUN: %FileCheck %s --check-prefixes=CHECK < %t/Lib.sil
// RUN: %FileCheck %s --check-prefixes=CHECK-MAIN < %t/Main.sil


//--- main.swift
import Lib 

// CHECK-MAIN-NOT: s3Lib14createPubClassySixlF
// CHECK-MAIN-NOT: s3Lib19usePubStructKeypathySixlF

func checkClasses() {
  /// Inlined
  print(createPubClass(0))

  /// Not inlined as functions below contain private/internal symbols
  // CHECK-MAIN-DAG: function_ref @$s3Lib20createPubClass_neverySixlF
  // CHECK-MAIN-DAG: sil public_external [_semantics "optimize.sil.specialize.generic.never"] @$s3Lib20createPubClass_neverySixlF : $@convention(thin) <T> (@in_guaranteed T) -> Int {
  print(createPubClass_never(0))

  // CHECK-MAIN-DAG: function_ref @$s3Lib14createPrvClassySixlF
  // CHECK-MAIN-DAG: sil @$s3Lib14createPrvClassySixlF : $@convention(thin) <τ_0_0> (@in_guaranteed τ_0_0) -> Int
  print(createPrvClass(0))

  // CHECK-MAIN-DAG: function_ref @$s3Lib20createPrvClass_neverySixlF
  // CHECK-MAIN-DAG: sil [_semantics "optimize.sil.specialize.generic.never"] @$s3Lib20createPrvClass_neverySixlF : $@convention(thin) <τ_0_0> (@in_guaranteed τ_0_0) -> Int
  print(createPrvClass_never(0))
}

func checkNested() {
  // PubContainer initializer is inlined. 
  // CHECK-MAIN-DAG: struct $PubContainer ()  
  let c = PubContainer()
  // CHECK-MAIN-DAG: function_ref @$s3Lib12PubContainerV9pubMemberyxxlF
  // CHECK-MAIN-DAG: sil @$s3Lib12PubContainerV9pubMemberyxxlF : $@convention(method) <τ_0_0> (@in_guaranteed τ_0_0, @in_guaranteed PubContainer) -> @out τ_0_0
  print(c.pubMember(27))
}

func checkKeyPaths() {
  /// Inlined
  // CHECK-MAIN-DAG: function_ref @$s3Lib19getPubStructKeypathys7KeyPathCyAA0cD0VSiGxlF
  print(usePubStructKeypath(0))

  /// Not inlined as functions below contain private/internal symbols
  // CHECK-MAIN-DAG: function_ref @$s3Lib24useInternalStructKeypathySixlF
  // CHECK-MAIN-DAG: sil @$s3Lib24useInternalStructKeypathySixlF : $@convention(thin) <τ_0_0> (@in_guaranteed τ_0_0) -> Int
  print(useInternalStructKeypath(0))

  // CHECK-MAIN-DAG: function_ref @$s3Lib15useClassKeypathySixlF
  // CHECK-MAIN-DAG: sil @$s3Lib15useClassKeypathySixlF : $@convention(thin) <τ_0_0> (@in_guaranteed τ_0_0) -> Int
  print(useClassKeypath(0))
}

checkNested()
checkClasses()
checkKeyPaths()


//--- Lib.swift

// createPubClass<A>(_:)
// CHECK-DAG: sil [serialized_for_package] [canonical] @$s3Lib14createPubClassySixlF : $@convention(thin) <T> (@in_guaranteed T) -> Int {
public func createPubClass<T>(_ t: T) -> Int {
  return getPubClass(t).foo()
}

// CHECK-DAG: sil [serialized_for_package] [_semantics "optimize.sil.specialize.generic.never"] [canonical] @$s3Lib20createPubClass_neverySixlF : $@convention(thin) <T> (@in_guaranteed T) -> Int {
@_semantics("optimize.sil.specialize.generic.never")
public func createPubClass_never<T>(_ t: T) -> Int {
  return getPubClass(t).foo()
}

// CHECK-DAG: sil [serialized_for_package] [canonical] @$s3Lib11getPubClassyAA13PublicDerivedCyxGxlF : $@convention(thin) <T> (@in_guaranteed T) -> @owned PublicDerived<T> {
public func getPubClass<T>(_ t : T) -> PublicDerived<T> {
  return PublicDerived<T>(t)
}

// Not serialized
public func createPrvClass<T>(_ t: T) -> Int {
  return getPrvClass(t).foo()
}

// Not serialized
@_semantics("optimize.sil.specialize.generic.never")
public func createPrvClass_never<T>(_ t: T) -> Int {
  return getPrvClass(t).foo()
}

// Not serialized
private func getPrvClass<T>(_ t : T) -> PrivateBase<T> {
  return PrivateDerived<T>(t)
}

public class PublicBase<T> {
  public var t: T
  public func foo() -> Int { return 27 }
  public init(_ t: T) { self.t = t }
}

public class PublicDerived<T> : PublicBase<T> {
  override public func foo() -> Int { return 28 }
}

private class PrivateBase<T> {
  var t: T
  func foo() -> Int { return 27 }
  init(_ t: T) { self.t = t }
}

private class PrivateDerived<T> : PrivateBase<T> {
  override func foo() -> Int { return 28 }
}

public struct PubContainer {
  private final class PrvBase {}
  public init() {}

  // Not serialized; contains exported func
  // but references a private class.
  public func pubMember<T>(_ t: T) -> T {
    var arr = Array<PrvBase>()
    arr.append(PrvBase())
    print(arr)
    exportedFunc(arr)
    return t
  }
}

@_specialize(exported: true, where T == Int)
@inlinable
public func exportedFunc<T>(_ t: T) {
  print(t)
}

public func pubFunc<T>(_ t: T) -> Int {
  return getPubClass(t).foo()
}

@_semantics("optimize.sil.specialize.generic.never")
public func pubFuncNoSpecialize<T>(_ t: T) -> Int {
  return getPubClass(t).foo()
}


struct InternalStruct {
  var x: Int { return 27 }
  var y: Int { return 28 }
}

public struct PubStruct {
  public var x: Int { return 27 }
  public var y: Int { return 28 }
}

class Myclass {
  var x: Int { return 27 }
  var y: Int { return 28 }
}

class Derived : Myclass {
  override var x: Int { return 29 }
  override var y: Int { return 30 }
}


func getInternalStructKeypath<T>(_ t: T) -> KeyPath<InternalStruct, Int> {
  return \InternalStruct.x
}

// CHECK-DAG: sil [canonical] @$s3Lib19getPubStructKeypathys7KeyPathCyAA0cD0VSiGxlF : $@convention(thin) <τ_0_0> (@in_guaranteed τ_0_0) -> @owned KeyPath<PubStruct, Int>
public func getPubStructKeypath<T>(_ t: T) -> KeyPath<PubStruct, Int> {
  return \PubStruct.x
}

public func useInternalStructKeypath<T>(_ t: T) -> Int {
  let s = InternalStruct()
  return s[keyPath: getInternalStructKeypath(t)]
}

// CHECK-DAG: sil [serialized_for_package] [canonical] @$s3Lib19usePubStructKeypathySixlF : $@convention(thin) <T> (@in_guaranteed T) -> Int {
public func usePubStructKeypath<T>(_ t: T) -> Int {
  let p = PubStruct()
  return p[keyPath: getPubStructKeypath(t)]
}

func getClassKeypath<T>(_ t: T) -> KeyPath<Myclass, Int> {
  return \Myclass.x
}


public func useClassKeypath<T>(_ t: T) -> Int {
  let c = Derived()
  return c[keyPath: getClassKeypath(t)]
}

