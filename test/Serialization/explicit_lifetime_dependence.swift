// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -o %t  %S/Inputs/def_explicit_lifetime_dependence.swift \
// RUN: -enable-experimental-feature NonescapableTypes \
// RUN: -disable-lifetime-dependence-diagnostics

// RUN: llvm-bcanalyzer %t/def_explicit_lifetime_dependence.swiftmodule 

// RUN: %target-swift-frontend -module-name lifetime-dependence -emit-sil -I %t %s \
// RUN: -enable-experimental-feature NonescapableTypes | %FileCheck %s

import def_explicit_lifetime_dependence
func testBasic() {
  let capacity = 4
  let a = Array(0..<capacity)
  a.withUnsafeBytes {
    let view = BufferView($0)
    let derivedView = derive(view)
    let consumedView = consumeAndCreate(derivedView)
    let borrowedView = borrowAndCreate(consumedView) 
    let mysteryView = deriveThisOrThat(borrowedView, consumedView)
    use(mysteryView)
  }
}

func testInitializers() {
  let capacity = 4
  let a = Array(0..<capacity)
  let b = Array(0..<capacity)
  a.withUnsafeBytes {
    let view1 = BufferView($0, a)
    let view2 = BufferView($0, b)
    let mysteryView = deriveThisOrThat(view1, view2)
    use(mysteryView)
  }
}

func testReadAccessor() {
  let capacity = 4
  let a = Array(0..<capacity)
  a.withUnsafeBytes {
    let view = BufferView($0, a)
    let w = Wrapper(view)
    use(w.view)
  }
}

func testFakeOptional() {
  _ = FakeOptional<Int>(())
}

// CHECK: sil @$s32def_explicit_lifetime_dependence6deriveyAA10BufferViewVADYlsF : $@convention(thin) (@guaranteed BufferView) -> _scope(0) @owned BufferView
// CHECK: sil @$s32def_explicit_lifetime_dependence16consumeAndCreateyAA10BufferViewVADnYliF : $@convention(thin) (@owned BufferView) -> _inherit(0) @owned BufferView
// CHECK: sil @$s32def_explicit_lifetime_dependence15borrowAndCreateyAA10BufferViewVADYlsF : $@convention(thin) (@guaranteed BufferView) -> _scope(0) @owned BufferView
// CHECK: sil @$s32def_explicit_lifetime_dependence16deriveThisOrThatyAA10BufferViewVADYls_ADYlstF : $@convention(thin) (@guaranteed BufferView, @guaranteed BufferView) -> _scope(0, 1) @owned BufferView

// CHECK: sil @$s32def_explicit_lifetime_dependence10BufferViewVyACSW_SaySiGhYlstcfC : $@convention(method) (UnsafeRawBufferPointer, @guaranteed Array<Int>, @thin BufferView.Type) -> _scope(1) @owned BufferView
