// RUN: %target-typecheck-verify-swift -enable-experimental-move-only -parse-stdlib -disable-availability-checking

import Swift

class Klass {}

func argumentsAndReturns(@_noImplicitCopy _ x: Klass) -> Klass {
    return x
}
func letDecls(_ x: Klass) -> () {
    @_noImplicitCopy let y: Klass = x
    print(y)
}

func varDecls(_ x: Klass, _ x2: Klass) -> () {
    @_noImplicitCopy var y: Klass = x // expected-error {{'@_noImplicitCopy' attribute can only be applied to local lets}}
    y = x2
    print(y)
}

func getKlass() -> Builtin.NativeObject {
    let k = Klass()
    return Builtin.unsafeCastToNativeObject(k)
}

@_noImplicitCopy var g: Builtin.NativeObject = getKlass() // expected-error {{'@_noImplicitCopy' attribute can only be applied to local lets}}
@_noImplicitCopy let g2: Builtin.NativeObject = getKlass() // expected-error {{'@_noImplicitCopy' attribute can only be applied to local lets}}
@_noImplicitCopy var g3: Builtin.NativeObject { getKlass() } // expected-error {{'@_noImplicitCopy' attribute can only be applied to local lets}}

struct MyStruct {
    // Error if @_noImplicitCopy on struct fields. We do not have move only types and
    // these are part of MyStruct.
    //
    // TODO: Make error specific for move only on struct/enum.
    @_noImplicitCopy var x: Builtin.NativeObject = getKlass() // expected-error {{'@_noImplicitCopy' attribute can only be applied to local lets}}
    @_noImplicitCopy let y: Builtin.NativeObject = getKlass() // expected-error {{'@_noImplicitCopy' attribute can only be applied to local lets}}

    @_noImplicitCopy var myMoveOnly: Builtin.NativeObject { // expected-error {{'@_noImplicitCopy' attribute can only be applied to local lets}}
        return getKlass()
    }

    func foo<T>(@_noImplicitCopy _ t: T) {
    }
}

struct MyGenericStruct<T> {
    func foo(@_noImplicitCopy _ t: T) {
    }
}

protocol P {
    @_noImplicitCopy var x: Builtin.NativeObject { get } // expected-error {{'@_noImplicitCopy' attribute can only be applied to local lets}}
}

func foo<T>(@_noImplicitCopy _ t: T) {
}

// Do not error on class fields. The noImplicitCopy field is separate from the
// underlying class itself so the fact the class is not move only does not
// suggest that the binding inside the class can be.
class MyClass {
    @_noImplicitCopy var x: Builtin.NativeObject = getKlass() // expected-error {{'@_noImplicitCopy' attribute can only be applied to local lets}}
    @_noImplicitCopy let y: Builtin.NativeObject = getKlass() // expected-error {{'@_noImplicitCopy' attribute can only be applied to local lets}}

    @_noImplicitCopy var myMoveOnly: Builtin.NativeObject { // expected-error {{'@_noImplicitCopy' attribute can only be applied to local lets}}
        return getKlass()
    }

    func foo<T>(@_noImplicitCopy _ t: T) {
    }
}

class MyGenericClass<T> {
    @_noImplicitCopy var x: T? = nil // expected-error {{'@_noImplicitCopy' attribute can only be applied to local lets}}
    @_noImplicitCopy let y: T? = nil // expected-error {{'@_noImplicitCopy' attribute can only be applied to local lets}}

    @_noImplicitCopy var myMoveOnly: T? { // expected-error {{'@_noImplicitCopy' attribute can only be applied to local lets}}
        return nil
    }

    @_noImplicitCopy var myMoveOnly2: Builtin.NativeObject? { // expected-error {{'@_noImplicitCopy' attribute can only be applied to local lets}}
        return nil
    }

    func foo(@_noImplicitCopy _ t: T) {
    }
}

// We need to error on Enums since the case is part of the value and we do not
// support move only types.
enum MyEnum {
    case none
    case noImplicitCopyCase(Klass)

    // We support doing it on computed properties though.
    @_noImplicitCopy var myMoveOnly: Builtin.NativeObject { // expected-error {{'@_noImplicitCopy' attribute can only be applied to local lets}}
        return getKlass()
    }
}

// We need to error on Enums since the case is part of the value and we do not
// support move only types.
enum MyGenericEnum<T> {
    case none
    case noImplicitCopyCase(Klass)

    // We support doing it on computed properties though.
    @_noImplicitCopy var myMoveOnly: Builtin.NativeObject { // expected-error {{'@_noImplicitCopy' attribute can only be applied to local lets}}
        return getKlass()
    }

    // We support doing it on computed properties though.
    @_noImplicitCopy var myMoveOnly2: T? { // expected-error {{'@_noImplicitCopy' attribute can only be applied to local lets}}
        return nil
    }
}

struct UnsafePointerWithOwner<T> {
    var owner: AnyObject? = nil
    var data: UnsafePointer<T>? = nil

    func doNothing() {}
}

func useUnsafePointerWithOwner<T>(_ x: UnsafePointerWithOwner<T>) {
    // We allow for this here (even without opaque values, since we check this
    // at the SIL level in SILGen).
    @_noImplicitCopy let y = x
    y.doNothing()
    let z = y
    print(z)
}

func useGeneric<T>(_ x: T) {
    // We allow for this here (even without opaque values, since we check this
    // at the SIL level in SILGen).
    @_noImplicitCopy let y = x
    let z = y
    print(z)
}

@_moveOnly
struct MoveOnly {
    var k = Klass()
}

func useMoveOnly(@_noImplicitCopy _ x: __shared MoveOnly) -> MoveOnly { // expected-error {{'@_noImplicitCopy' has no effect when applied to a noncopyable type}}
    return x
}

func useMoveOnly2(_ x: __shared MoveOnly) {
    @_noImplicitCopy let y = x // expected-error {{'@_noImplicitCopy' has no effect when applied to a noncopyable type}}
    let _ = y
}
