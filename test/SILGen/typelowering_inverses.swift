// RUN: %target-swift-frontend -emit-silgen -enable-experimental-feature NonescapableTypes -disable-availability-checking -module-name main %s | %FileCheck %s

protocol NoCopyP: ~Copyable {}

struct NC: ~Copyable {}

struct RudeStruct<T: ~Copyable>: Copyable {
    let thing: Int
}

enum RudeEnum<T: ~Copyable>: Copyable {
  case holder(Int)
  case whatever
}

struct CondCopyableStruct<T: ~Copyable>: ~Copyable {}

extension CondCopyableStruct: Copyable where T: Copyable {}

enum CondCopyableEnum<T: ~Copyable>: ~Copyable {
  case some(T)
  case none
}

extension CondCopyableEnum: Copyable where T: Copyable {}

protocol NoEscapeP: ~Escapable {}

struct NE: ~Escapable {}

struct TooRudeStruct<T: ~Escapable>: Escapable {
    let thing: Int
}

enum TooRudeEnum<T: ~Escapable>: Escapable {
  case holder(Int)
  case whatever
}

struct CondEscapableStruct<T: ~Escapable>: ~Escapable {}

extension CondEscapableStruct: Escapable where T: Escapable {}

enum CondEscapableEnum<T: ~Escapable>: ~Escapable {
  case some(T)
  case none
}

extension CondEscapableEnum: Escapable where T: Escapable {}

// MARK: ensure certain conditionally Copyable types are treated as trivial (no ownership in func signature).

// CHECK: sil hidden [ossa] @$s4main5checkyyAA10RudeStructVySiGF : $@convention(thin) (RudeStruct<Int>) -> () {
func check(_ t: RudeStruct<Int>) {}

// CHECK: sil hidden [ossa] @$s4main5checkyyAA10RudeStructVyAA2NCVGF : $@convention(thin) (RudeStruct<NC>) -> () {
func check(_ t: RudeStruct<NC>) {}

// CHECK: sil hidden [ossa] @$s4main5checkyyAA8RudeEnumOySiGF : $@convention(thin) (RudeEnum<Int>) -> () {
func check(_ t: RudeEnum<Int>) {}

// CHECK: sil hidden [ossa] @$s4main5checkyyAA8RudeEnumOyAA2NCVGF : $@convention(thin) (RudeEnum<NC>) -> () {
func check(_ t: RudeEnum<NC>) {}

// CHECK: sil hidden [ossa] @$s4main5checkyyAA18CondCopyableStructVySiGF : $@convention(thin) (CondCopyableStruct<Int>) -> () {
func check(_ t: CondCopyableStruct<Int>) {}

// CHECK: sil hidden [ossa] @$s4main5checkyyAA18CondCopyableStructVyAA2NCVGF : $@convention(thin) (@guaranteed CondCopyableStruct<NC>) -> () {
func check(_ t: borrowing CondCopyableStruct<NC>) {}

// CHECK: sil hidden [ossa] @$s4main5checkyyAA16CondCopyableEnumOySiGF : $@convention(thin) (CondCopyableEnum<Int>) -> () {
func check(_ t: CondCopyableEnum<Int>) {}

// CHECK: sil hidden [ossa] @$s4main5checkyyAA16CondCopyableEnumOyAA2NCVGF : $@convention(thin) (@guaranteed CondCopyableEnum<NC>) -> () {
func check(_ t: borrowing CondCopyableEnum<NC>) {}

// CHECK: sil hidden [ossa] @$s4main5checkyyAA16CondCopyableEnumOyxGlF : $@convention(thin) <T> (@in_guaranteed CondCopyableEnum<T>) -> () {
func check<T>(_ t: CondCopyableEnum<T>) {}

// CHECK: sil hidden [ossa] @$s4main5checkyyAA16CondCopyableEnumOyxGRi_zlF : $@convention(thin) <U where U : ~Copyable> (@in_guaranteed CondCopyableEnum<U>) -> () {
func check<U: ~Copyable>(_ t: borrowing CondCopyableEnum<U>) {}

// CHECK: sil hidden [ossa] @$s4main5checkyyAA7NoCopyP_pF : $@convention(thin) (@in_guaranteed any NoCopyP) -> () {
func check(_ t: any NoCopyP) {}

// CHECK: sil hidden [ossa] @$s4main5checkyyAA7NoCopyP_pRi_s_XPF : $@convention(thin) (@in_guaranteed any NoCopyP & ~Copyable) -> () {
func check(_ t: borrowing any NoCopyP & ~Copyable) {}

// CHECK: sil hidden [ossa] @$s4main5checkyyAA7NoCopyP_pRi_s_XPnF : $@convention(thin) (@in any NoCopyP & ~Copyable) -> () {
func check(_ t: consuming any NoCopyP & ~Copyable) {}

// CHECK: sil hidden [ossa] @$s4main5checkyyAA7NoCopyP_pRi_s_XPzF : $@convention(thin) (@inout any NoCopyP & ~Copyable) -> () {
func check(_ t: inout any NoCopyP & ~Copyable) {}

// MARK: ensure certain conditionally Escapable types are treated as trivial (no ownership in func signature).

// CHECK: sil hidden [ossa] @$s4main5checkyyAA13TooRudeStructVySiGF : $@convention(thin) (TooRudeStruct<Int>) -> () {
func check(_ t: TooRudeStruct<Int>) {}

// CHECK: sil hidden [ossa] @$s4main5checkyyAA13TooRudeStructVyAA2NEVGF : $@convention(thin) (TooRudeStruct<NE>) -> () {
func check(_ t: TooRudeStruct<NE>) {}

// CHECK: sil hidden [ossa] @$s4main5checkyyAA11TooRudeEnumOySiGF : $@convention(thin) (TooRudeEnum<Int>) -> () {
func check(_ t: TooRudeEnum<Int>) {}

// CHECK: sil hidden [ossa] @$s4main5checkyyAA11TooRudeEnumOyAA2NEVGF : $@convention(thin) (TooRudeEnum<NE>) -> () {
func check(_ t: TooRudeEnum<NE>) {}

// CHECK: sil hidden [ossa] @$s4main5checkyyAA19CondEscapableStructVySiGF : $@convention(thin) (CondEscapableStruct<Int>) -> () {
func check(_ t: CondEscapableStruct<Int>) {}

// CHECK: sil hidden [ossa] @$s4main5checkyyAA19CondEscapableStructVyAA2NEVGF : $@convention(thin) (@guaranteed CondEscapableStruct<NE>) -> () {
func check(_ t: borrowing CondEscapableStruct<NE>) {}

// CHECK: sil hidden [ossa] @$s4main5checkyyAA17CondEscapableEnumOySiGF : $@convention(thin) (CondEscapableEnum<Int>) -> () {
func check(_ t: CondEscapableEnum<Int>) {}

// CHECK: sil hidden [ossa] @$s4main5checkyyAA17CondEscapableEnumOyAA2NEVGF : $@convention(thin) (@guaranteed CondEscapableEnum<NE>) -> () {
func check(_ t: borrowing CondEscapableEnum<NE>) {}

// CHECK: sil hidden [ossa] @$s4main5checkyyAA17CondEscapableEnumOyxGlF : $@convention(thin) <T> (@in_guaranteed CondEscapableEnum<T>) -> () {
func check<T>(_ t: CondEscapableEnum<T>) {}

// CHECK: sil hidden [ossa] @$s4main5checkyyAA17CondEscapableEnumOyxGRi0_zlF : $@convention(thin) <U where U : ~Escapable> (@in_guaranteed CondEscapableEnum<U>) -> () {
func check<U: ~Escapable>(_ t: borrowing CondEscapableEnum<U>) {}

// CHECK: sil hidden [ossa] @$s4main5checkyyAA9NoEscapeP_pF : $@convention(thin) (@in_guaranteed any NoEscapeP) -> () {
func check(_ t: any NoEscapeP) {}

// CHECK: sil hidden [ossa] @$s4main5checkyyAA9NoEscapeP_pRi0_s_XPF : $@convention(thin) (@in_guaranteed any NoEscapeP & ~Escapable) -> () {
func check(_ t: borrowing any NoEscapeP & ~Escapable) {}

// CHECK: sil hidden [ossa] @$s4main5checkyyAA9NoEscapeP_pRi0_s_XPnF : $@convention(thin) (@in any NoEscapeP & ~Escapable) -> () {
func check(_ t: consuming any NoEscapeP & ~Escapable) {}

// CHECK: sil hidden [ossa] @$s4main5checkyyAA9NoEscapeP_pRi0_s_XPzF : $@convention(thin) (@inout any NoEscapeP & ~Escapable) -> () {
func check(_ t: inout any NoEscapeP & ~Escapable) {}

// MARK: conditionally Copyable & Escapable SILGen

struct MyStruct<T: ~Copyable & ~Escapable>: ~Copyable & ~Escapable {
    var x: T
}

extension MyStruct: Copyable where T: Copyable {}

extension MyStruct: Escapable where T: Escapable {}

enum MyEnum<T: ~Copyable & ~Escapable>: ~Copyable & ~Escapable {
    case x(T)
    case knoll
}

extension MyEnum: Copyable where T: Copyable {}

extension MyEnum: Escapable where T: Escapable {}

enum Trivial {
    case a, b, c
}

// CHECK-LABEL: sil{{.*}} @{{.*}}13trivialStruct
func trivialStruct() -> Int {
    // CHECK: [[ALLOC:%.*]] = alloc_stack $MyStruct<Trivial>
    // CHECK-NOT: destroy_addr [[ALLOC]] :
    // CHECK: dealloc_stack [[ALLOC]] :
     return MemoryLayout.size(ofValue: MyStruct(x: Trivial.a))
}
// CHECK-LABEL: sil{{.*}} @{{.*}}11trivialEnum
func trivialEnum() -> Int {
    // CHECK: [[ALLOC:%.*]] = alloc_stack $MyEnum<Trivial>
    // CHECK-NOT: destroy_addr [[ALLOC]] :
    // CHECK: dealloc_stack [[ALLOC]] :
     return MemoryLayout.size(ofValue: MyEnum.x(Trivial.a))
}

struct MyAssortment {
    var a: MyStruct<Trivial>
    var b: MyEnum<Trivial>
}

// CHECK-LABEL: sil{{.*}} @{{.*}}4frob
func frob(x: MyAssortment) -> Int {
    // CHECK: [[ALLOC:%.*]] = alloc_stack $MyAssortment
    // CHECK-NOT: destroy_addr [[ALLOC]] :
    // CHECK: dealloc_stack [[ALLOC]] :
    return MemoryLayout.size(ofValue: x)
}

extension MyEnum: BitwiseCopyable
    where T: Copyable & BitwiseCopyable {}
