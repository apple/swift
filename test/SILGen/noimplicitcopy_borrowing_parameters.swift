// RUN: %target-swift-emit-silgen -sil-verify-all %s | %FileCheck %s

////////////////////////
// MARK: Declarations //
////////////////////////

public class Klass {}

public struct NonTrivialStruct {
    var k = Klass()

    func doSomethingDefault() {}
    borrowing func doSomethingBorrowing() {}
    mutating func doSomethingMutating() {}
    //consuming func doSomethingConsuming() {}
}

public protocol P {
    static var value: Self { get }
}

public struct GenericNonTrivialStruct<T : P> {
    var t = T.value

    func doSomethingDefault() {}
    borrowing func doSomethingBorrowing() {}
}

enum AddressOnlyEnum<T> {
    case x(NonTrivialStruct)
    case y(T)
}

enum LoadableEnum {
    case x(NonTrivialStruct)
    case y(Int)
}

func borrowValDefault(_ x: NonTrivialStruct) {}
func borrowValBorrowing(_ x: borrowing NonTrivialStruct) {}
func borrowValDefault<T : P>(_ x: GenericNonTrivialStruct<T>) {}
func borrowValBorrowing<T : P>(_ x: borrowing GenericNonTrivialStruct<T>) {}
//func consumeValOwned(_ x: __owned NonTrivialStruct) {}
//func consumeValConsuming(_ x: consuming NonTrivialStruct) {}
//func consumeValOwned<T : P>(_ x: __owned GenericNonTrivialStruct<T>) {}
//func consumeValConsuming<T : P>(_ x: consuming GenericNonTrivialStruct<T>) {}

////////////////////////
// MARK: Simple Tests //
////////////////////////

// CHECK: sil hidden [ossa] @$s35noimplicitcopy_borrowing_parameters11testConsumeyyAA16NonTrivialStructVF : $@convention(thin) (@guaranteed NonTrivialStruct) -> () {
// CHECK: bb0([[ARG:%.*]] : @noImplicitCopy @guaranteed $NonTrivialStruct):
// CHECK:   [[WRAP:%.*]] = copyable_to_moveonlywrapper [guaranteed]
// CHECK:   [[COPY:%.*]] = copy_value [[WRAP]]
// CHECK:   [[CHECK:%.*]] = mark_must_check [no_consume_or_assign] [[COPY]]
// CHECK:   [[BORROW:%.*]] = begin_borrow [[CHECK]]
// CHECK:   [[COPY2:%.*]] = copy_value [[BORROW]]
// CHECK:   [[MOVE:%.*]] = move_value [[COPY2]]
// CHECK:   destroy_value [[MOVE]]
// CHECK:   end_borrow [[BORROW]]
//
// CHECK:   [[BORROW:%.*]] = begin_borrow [[CHECK]]
// CHECK:   [[EXT:%.*]] = struct_extract [[BORROW]]
// CHECK:   [[UNWRAP:%.*]] = moveonlywrapper_to_copyable [guaranteed] [[EXT]]
// CHECK:   [[COPY:%.*]] = copy_value [[UNWRAP]]
// CHECK:   destroy_value [[COPY]]
// CHECK:   end_borrow [[BORROW]]
//
// CHECK:   destroy_value [[CHECK]]
// CHECK: } // end sil function '$s35noimplicitcopy_borrowing_parameters11testConsumeyyAA16NonTrivialStructVF'
func testConsume(_ x: borrowing NonTrivialStruct) {
    let _ = x
    let _ = x.k
}

// CHECK-LABEL: sil hidden [ossa] @$s35noimplicitcopy_borrowing_parameters7testUseyyAA16NonTrivialStructVF : $@convention(thin) (@guaranteed NonTrivialStruct) -> () {
// CHECK: bb0([[ARG:%.*]] : @noImplicitCopy @guaranteed $NonTrivialStruct):
// CHECK:   [[WRAP:%.*]] = copyable_to_moveonlywrapper [guaranteed]
// CHECK:   [[COPY:%.*]] = copy_value [[WRAP]]
// CHECK:   [[CHECK:%.*]] = mark_must_check [no_consume_or_assign] [[COPY]]
// CHECK:   [[BORROW:%.*]] = begin_borrow [[CHECK]]
// CHECK:   end_borrow [[BORROW]]
// CHECK:   destroy_value [[CHECK]]
// CHECK: } // end sil function '$s35noimplicitcopy_borrowing_parameters7testUseyyAA16NonTrivialStructVF'
func testUse(_ x: borrowing NonTrivialStruct) {
    _ = x
}

// CHECK-LABEL: sil hidden [ossa] @$s35noimplicitcopy_borrowing_parameters24testCallBorrowValDefaultyyAA16NonTrivialStructVF : $@convention(thin) (@guaranteed NonTrivialStruct) -> () {
// CHECK: bb0([[ARG:%.*]] : @noImplicitCopy @guaranteed $NonTrivialStruct):
// CHECK:   [[WRAP:%.*]] = copyable_to_moveonlywrapper [guaranteed]
// CHECK:   [[COPY:%.*]] = copy_value [[WRAP]]
// CHECK:   [[CHECK:%.*]] = mark_must_check [no_consume_or_assign] [[COPY]]
// CHECK:   [[BORROW:%.*]] = begin_borrow [[CHECK]]
// CHECK:   [[UNWRAP:%.*]] = moveonlywrapper_to_copyable [guaranteed] [[BORROW]]
// CHECK:   apply {{%.*}}([[UNWRAP]])
// CHECK:   end_borrow [[BORROW]]
// CHECK:   destroy_value [[CHECK]]
// CHECK: } // end sil function '$s35noimplicitcopy_borrowing_parameters24testCallBorrowValDefaultyyAA16NonTrivialStructVF'
func testCallBorrowValDefault(_ x: borrowing NonTrivialStruct) {
    borrowValDefault(x)
}

// CHECK-LABEL: sil hidden [ossa] @$s35noimplicitcopy_borrowing_parameters26testCallBorrowValBorrowingyyAA16NonTrivialStructVF : $@convention(thin) (@guaranteed NonTrivialStruct) -> () {
// CHECK: bb0([[ARG:%.*]] : @noImplicitCopy @guaranteed $NonTrivialStruct):
// CHECK:   [[WRAP:%.*]] = copyable_to_moveonlywrapper [guaranteed]
// CHECK:   [[COPY:%.*]] = copy_value [[WRAP]]
// CHECK:   [[CHECK:%.*]] = mark_must_check [no_consume_or_assign] [[COPY]]
// CHECK:   [[BORROW:%.*]] = begin_borrow [[CHECK]]
func testCallBorrowValBorrowing(_ x: borrowing NonTrivialStruct) {
    borrowValBorrowing(x)
}

// CHECK-LABEL: sil hidden [ossa] @$s35noimplicitcopy_borrowing_parameters25testCallMethodSelfDefaultyyAA16NonTrivialStructVF : $@convention(thin) (@guaranteed NonTrivialStruct) -> () {
// CHECK: bb0([[ARG:%.*]] : @noImplicitCopy @guaranteed $NonTrivialStruct):
// CHECK:   [[WRAP:%.*]] = copyable_to_moveonlywrapper [guaranteed]
// CHECK:   [[COPY:%.*]] = copy_value [[WRAP]]
// CHECK:   [[CHECK:%.*]] = mark_must_check [no_consume_or_assign] [[COPY]]
// CHECK:   [[BORROW:%.*]] = begin_borrow [[CHECK]]
// CHECK:   [[UNWRAP:%.*]] = moveonlywrapper_to_copyable [guaranteed] [[BORROW]]
// CHECK:   apply {{%.*}}([[UNWRAP]])
// CHECK:   end_borrow [[BORROW]]
// CHECK:   destroy_value [[CHECK]]
// CHECK: } // end sil function '$s35noimplicitcopy_borrowing_parameters25testCallMethodSelfDefaultyyAA16NonTrivialStructVF'
func testCallMethodSelfDefault(_ x: borrowing NonTrivialStruct) {
    x.doSomethingDefault()
}

// CHECK-LABEL: sil hidden [ossa] @$s35noimplicitcopy_borrowing_parameters27testCallMethodSelfBorrowingyyAA16NonTrivialStructVF : $@convention(thin) (@guaranteed NonTrivialStruct) -> () {
// CHECK:   [[WRAP:%.*]] = copyable_to_moveonlywrapper [guaranteed]
// CHECK:   [[COPY:%.*]] = copy_value [[WRAP]]
// CHECK:   [[CHECK:%.*]] = mark_must_check [no_consume_or_assign] [[COPY]]
// CHECK:   [[BORROW:%.*]] = begin_borrow [[CHECK]]
// CHECK:   [[UNWRAP:%.*]] = moveonlywrapper_to_copyable [guaranteed] [[BORROW]]
// TODO: This should be passed directly without a conversion.
// CHECK:   apply {{%.*}}([[UNWRAP]])
// CHECK: } // end sil function '$s35noimplicitcopy_borrowing_parameters27testCallMethodSelfBorrowingyyAA16NonTrivialStructVF'
func testCallMethodSelfBorrowing(_ x: borrowing NonTrivialStruct) {
    x.doSomethingBorrowing()
}

// CHECK-LABEL: sil hidden [ossa] @$s35noimplicitcopy_borrowing_parameters19testEscapingClosureyyAA16NonTrivialStructVF : $@convention(thin) (@guaranteed NonTrivialStruct) -> () {
// CHECK: bb0([[ARG:%.*]] : @noImplicitCopy @guaranteed $NonTrivialStruct):
// CHECK:   [[WRAP:%.*]] = copyable_to_moveonlywrapper [guaranteed]
// CHECK:   [[COPY:%.*]] = copy_value [[WRAP]]
// CHECK:   [[CHECK:%.*]] = mark_must_check [no_consume_or_assign] [[COPY]]
// CHECK:   [[COPY2:%.*]] = copy_value [[CHECK]]
// CHECK:   [[UNWRAP:%.*]] = moveonlywrapper_to_copyable [owned] [[COPY2]]
// CHECK:   partial_apply [callee_guaranteed] {{%.*}}([[UNWRAP]])
// CHECK: } // end sil function '$s35noimplicitcopy_borrowing_parameters19testEscapingClosureyyAA16NonTrivialStructVF'
func testEscapingClosure(_ x: borrowing NonTrivialStruct) {
    var f: () -> () = {}
    f = {
        _ = x
    }
    _ = f
}

// CHECK-LABEL: sil hidden [ossa] @$s35noimplicitcopy_borrowing_parameters22testNonEscapingClosureyyAA0E13TrivialStructVF : $@convention(thin) (@guaranteed NonTrivialStruct) -> () {
// CHECK: bb0([[ARG:%.*]] : @noImplicitCopy @guaranteed $NonTrivialStruct):
// CHECK:   [[WRAP:%.*]] = copyable_to_moveonlywrapper [guaranteed]
// CHECK:   [[COPY:%.*]] = copy_value [[WRAP]]
// CHECK:   [[CHECK:%.*]] = mark_must_check [no_consume_or_assign] [[COPY]]
// CHECK:   [[COPY2:%.*]] = copy_value [[CHECK]]
// CHECK:   [[UNWRAP:%.*]] = moveonlywrapper_to_copyable [owned] [[COPY2]]
// CHECK:   [[PAI:%.*]] = partial_apply [callee_guaranteed] {{%.*}}([[UNWRAP]])
// CHECK: } // end sil function '$s35noimplicitcopy_borrowing_parameters22testNonEscapingClosureyyAA0E13TrivialStructVF'
func testNonEscapingClosure(_ x: borrowing NonTrivialStruct) {
    func useNonEscaping(_ f: () -> ()) {}
    useNonEscaping {
        _ = x
    }
}

// CHECK-LABEL: sil hidden [ossa] @$s35noimplicitcopy_borrowing_parameters36testLoadableBorrowingConsumeOperatoryyAA16NonTrivialStructVF : $@convention(thin) (@guaranteed NonTrivialStruct) -> () {
// CHECK: bb0([[ARG:%.*]] : @noImplicitCopy
// CHECK:   [[WRAP:%.*]] = copyable_to_moveonlywrapper [guaranteed]
// CHECK:   [[COPY:%.*]] = copy_value [[WRAP]]
// CHECK:   [[CHECK:%.*]] = mark_must_check [no_consume_or_assign] [[COPY]]
// CHECK:   [[BORROW:%.*]] = begin_borrow [[CHECK]]
// CHECK:   [[COPY2:%.*]] = copy_value [[BORROW]]
// CHECK:   [[MOVE:%.*]] = move_value [allows_diagnostics] [[COPY2]]
// CHECK:   destroy_value [[MOVE]]
// CHECK:   end_borrow [[BORROW]]
// CHECK:   destroy_value [[CHECK]]
// CHECK: } // end sil function '$s35noimplicitcopy_borrowing_parameters36testLoadableBorrowingConsumeOperatoryyAA16NonTrivialStructVF'
func testLoadableBorrowingConsumeOperator(_ x: borrowing NonTrivialStruct) {
    _ = consume x
}

// CHECK-LABEL: sil hidden [ossa] @$s35noimplicitcopy_borrowing_parameters25testLoadableBorrowingEnumyyAA0eG0OF : $@convention(thin) (@guaranteed LoadableEnum) -> () {
// CHECK: bb0([[ARG:%.*]] : @noImplicitCopy
// CHECK:   [[WRAP:%.*]] = copyable_to_moveonlywrapper [guaranteed]
// CHECK:   [[COPY:%.*]] = copy_value [[WRAP]]
// CHECK:   [[CHECK:%.*]] = mark_must_check [no_consume_or_assign] [[COPY]]
// CHECK:   [[BORROW:%.*]] = begin_borrow [[CHECK]]
// CHECK:   [[COPY2:%.*]] = copy_value [[BORROW]]
// CHECK:   [[UNWRAP:%.*]] = moveonlywrapper_to_copyable [owned] [[COPY2]]
// CHECK:   switch_enum [[UNWRAP]]
// CHECK: } // end sil function '$s35noimplicitcopy_borrowing_parameters25testLoadableBorrowingEnumyyAA0eG0OF'
func testLoadableBorrowingEnum(_ x: borrowing LoadableEnum) {
    switch x {
    case let .x(y):
        _ = y
        break
    case .y:
        break
    }
}

//////////////////////////////////////////////
// MARK: Simple AddressOnly Borrowing Tests //
//////////////////////////////////////////////

// CHECK-LABEL: sil hidden [ossa] @$s35noimplicitcopy_borrowing_parameters31testAddressOnlyBorrowingConsumeyyAA23GenericNonTrivialStructVyxGAA1PRzlF : $@convention(thin) <T where T : P> (@in_guaranteed GenericNonTrivialStruct<T>) -> () {
// CHECK: bb0([[ARG:%.*]] : @noImplicitCopy
// CHECK:   [[WRAP:%.*]] = copyable_to_moveonlywrapper_addr [[ARG]]
// CHECK:   [[CHECK:%.*]] = mark_must_check [no_consume_or_assign] [[WRAP]]
// CHECK:   [[STACK:%.*]] = alloc_stack
// CHECK:   [[UNWRAP:%.*]] = moveonlywrapper_to_copyable_addr [[CHECK]]
// CHECK:   copy_addr [[UNWRAP]] to [init] [[STACK]]
// CHECK:   destroy_addr [[STACK]]
// CHECK:   [[UNWRAP:%.*]] = moveonlywrapper_to_copyable_addr [[CHECK]]
// TODO: We probably want the unwrap to be on the struct_element_addr, not the other wya around.
// CHECK:   [[GEP:%.*]] = struct_element_addr [[UNWRAP]]
// CHECK:   [[STACK:%.*]] = alloc_stack
// CHECK:   copy_addr [[GEP]] to [init] [[STACK]]
// CHECK:   destroy_addr [[STACK]]
// CHECK: } // end sil function '$s35noimplicitcopy_borrowing_parameters31testAddressOnlyBorrowingConsumeyyAA23GenericNonTrivialStructVyxGAA1PRzlF'
func testAddressOnlyBorrowingConsume<T : P>(_ x: borrowing GenericNonTrivialStruct<T>) {
    let _ = x 
    let _ = x.t
}

func testAddressOnlyBorrowingConsume2<T : P>(_ x: borrowing GenericNonTrivialStruct<T>) {    
    var y = x 
    y = x 
    _ = y
}

func testAddressOnlyBorrowingConsume3<T : P>(_ x: borrowing GenericNonTrivialStruct<T>) {    
    let y = x 
    _ = y
}

func testAddressOnlyBorrowingUse<T : P>(_ x: borrowing GenericNonTrivialStruct<T>) {
    _ = x
}

func testAddressOnlyBorrowingUseAndConsume<T : P>(_ x: borrowing GenericNonTrivialStruct<T>) {    
    borrowValDefault(x)
    let _ = x 
}

func testAddressOnlyBorrowingCallBorrowValDefault<T : P>(_ x: borrowing GenericNonTrivialStruct<T>) {
    borrowValDefault(x)
}

func testAddressOnlyBorrowingCallBorrowValBorrowing<T : P>(_ x: borrowing GenericNonTrivialStruct<T>) {
    borrowValBorrowing(x)
}

func testAddressOnlyBorrowingCallMethodSelfDefault<T : P>(_ x: borrowing GenericNonTrivialStruct<T>) {
    x.doSomethingDefault()
}

func testAddressOnlyBorrowingCallMethodSelfBorrowing<T : P>(_ x: borrowing GenericNonTrivialStruct<T>) {
    x.doSomethingBorrowing()
}

func testAddressOnlyBorrowingEscapingClosure<T : P>(_ x: borrowing GenericNonTrivialStruct<T>) {    
    var f: () -> () = {}
    f = { 
        _ = x
    }
    _ = f
}

func testAddressOnlyBorrowingNonEscapingClosure<T : P>(_ x: borrowing GenericNonTrivialStruct<T>) {
    func useNonEscaping(_ f: () -> ()) {}
    useNonEscaping { 
        _ = x
    }
}

func testAddressOnlyBorrowingCast<T : P>(_ x: borrowing GenericNonTrivialStruct<T>) {
    let _ = x as Any 
}

func testAddressOnlyBorrowingCastCheck<T : P>(_ x: borrowing GenericNonTrivialStruct<T>) {
    if x is Any { 
    }
}

func testAddressOnlyBorrowingEnum<T>(_ x: borrowing AddressOnlyEnum<T>) {
    switch x {
    case let .x(y):
        _ = y
        break
    case let .y(z):
        _ = z
        break
    }
}
