// RUN: %target-swift-frontend %s -O -emit-sil -parse-as-library | %FileCheck %s
protocol SomeP {}

public enum SpecialEnum : SomeP {}

// CHECK-LABEL: sil shared [noinline] @$s20existential_metatype17checkProtocolType0aE0Sbxm_tAA5SomePRzlFAA11SpecialEnumO_Tgm5 : $@convention(thin) () -> Bool {
// CHECK:       bb0:
// CHECK-NEXT:   %0 = integer_literal $Builtin.Int1, -1
// CHECK-NEXT:   %1 = struct $Bool (%0 : $Builtin.Int1)
// CHECK-NEXT:   return %1 : $Bool
// CHECK-LABEL: } // end sil function '$s20existential_metatype17checkProtocolType0aE0Sbxm_tAA5SomePRzlFAA11SpecialEnumO_Tgm5'
@inline(never)
func checkProtocolType<P : SomeP>(existentialType: P.Type) -> Bool {
  return existentialType == SpecialEnum.self
}

public func testProtocolType() -> Bool {
  return checkProtocolType(existentialType: SpecialEnum.self)
}
