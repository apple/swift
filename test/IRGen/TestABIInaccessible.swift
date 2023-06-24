// RUN: %target-swift-frontend -disable-type-layout -module-name main -I %t -emit-ir -primary-file %s %S/Inputs/ABIInaccessible.swift | %FileCheck %s

public struct AnotherType<T> {
  init(_ t: T) {
    p = Public<T>(t)
  }
  public var p : Public<T>
}

// Don't pass the metadata of Private<T> to AnotherType<T>'s outlined destroy.
// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s4main4copyyAA11AnotherTypeVyxGAElF"(ptr noalias nocapture sret({{.*}}) %0, ptr noalias nocapture %1, ptr %T)
// CHECK:  [[MD:%.*]] = call swiftcc %swift.metadata_response @"$s4main11AnotherTypeVMa"(i{{.*}} 0, ptr %T)
// CHECK:  [[MD1:%.*]] = extractvalue %swift.metadata_response [[MD]], 0
// CHECK:  [[MD2:%.*]] = call swiftcc %swift.metadata_response @"$s4main6PublicVMa"(i{{.*}} 0, ptr %T)
// CHECK:  [[MD3:%.*]] = extractvalue %swift.metadata_response [[MD2]], 0
// CHECK:  call ptr @"$s4main11AnotherTypeVyxGlWOc"(ptr %1, ptr {{.*}}, ptr %T, ptr [[MD3]], ptr [[MD1]])
public func copy<T>(_ a: AnotherType<T>) -> AnotherType<T> {
  let copy = a
  return copy
}
