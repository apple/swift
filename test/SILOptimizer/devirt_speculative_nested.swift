
// RUN: %target-swift-frontend -module-name devirt_speculative_nested %s -parse-as-library -enable-spec-devirt -O -emit-sil | %FileCheck %s
// RUN: %target-swift-frontend -module-name devirt_speculative_nested %s -parse-as-library -Osize -emit-sil
//
// Test speculative devirtualization.

public class Outer<T> {
  final class Inner : Base {
    @inline(never) override func update() {
    }
  }
}

public class Base {
  @inline(never) func update() { }
}

// FIXME: We don't speculate to the override Outer<T>.Inner.update() here,
// because we cannot express the cast -- the cast "returns" a new archetype
// T, much like an opened existential.
//
// But at least, we shouldn't crash.

// CHECK-LABEL: sil @$s25devirt_speculative_nested3foo1xyAA4BaseC_tF : $@convention(thin) (@guaranteed Base) -> ()
// CHECK: checked_cast_br [exact] Base in %0 : $Base to Base
// CHECK: function_ref @$s25devirt_speculative_nested4BaseC6updateyyF
// CHECK: class_method %0 : $Base, #Base.update
// CHECK: return

public func foo(x: Base) {
  x.update()
}
