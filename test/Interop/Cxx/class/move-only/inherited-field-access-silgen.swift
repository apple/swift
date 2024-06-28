// RUN: %target-swift-emit-sil -I %S/Inputs -cxx-interoperability-mode=swift-6 %s -validate-tbd-against-ir=none | %FileCheck %s
// RUN: %target-swift-emit-sil -I %S/Inputs -cxx-interoperability-mode=upcoming-swift %s -validate-tbd-against-ir=none | %FileCheck %s

import MoveOnlyCxxValueType

func testGetX() -> CInt {
    let derived = NonCopyableHolderDerivedDerived(42)
    return derived.x.x
}

let _ = testGetX()

func testSetX(_ x: CInt) {
    var derived = NonCopyableHolderDerivedDerived(42)
    derived.x.x = 2
}

testSetX(2)

// Beware: this test exhibits a subtle difference between open source Clang and AppleClang:
// AppleClang runs in an ABI compatibility mode with Clang <= 4, which uses a
// different criteria to determine whether a C++ type can be passed in 
// registers. This causes Swift to assume that NonCopyableHolderDerivedDerived
// can be both loadable or address-only, depending on the compiler flavor used.
// (rdar://128424443)

// CHECK: sil shared [transparent] @$sSo024NonCopyableHolderDerivedD0V1xSo0aB0Vvlu : $@convention(method) (@{{(in_)?}}guaranteed NonCopyableHolderDerivedDerived) -> UnsafePointer<NonCopyable>
// CHECK: {{.*}}(%[[SELF_VAL:.*]] : ${{(\*)?}}NonCopyableHolderDerivedDerived):
// CHECK: function_ref @{{.*}}__synthesizedBaseCall___synthesizedBaseGetterAccessor{{.*}} : $@convention(cxx_method) (@in_guaranteed NonCopyableHolderDerivedDerived) -> UnsafePointer<NonCopyable>
// CHECK-NEXT: apply %{{.*}}

// CHECK: sil shared [transparent] @$sSo024NonCopyableHolderDerivedD0V1xSo0aB0Vvau : $@convention(method) (@inout NonCopyableHolderDerivedDerived) -> UnsafeMutablePointer<NonCopyable>
// CHECK: function_ref @{{.*}}__synthesizedBaseCall___synthesizedBaseSetterAccessor{{.*}} : $@convention(cxx_method) (@inout NonCopyableHolderDerivedDerived) -> UnsafeMutablePointer<NonCopyable>
