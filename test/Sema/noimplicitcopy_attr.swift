// RUN: %target-typecheck-verify-swift -parse-stdlib -disable-availability-checking -verify-syntax-tree

import Swift

class Klass {}

func argumentsAndReturns(@_noImplicitCopy _ x: Klass) -> /*@_noImplicitCopy*/ Klass {
    return x
}
func passArguments(@_noImplicitCopy _ x: Klass, @_noImplicitCopy _ y : Klass) -> () {
    @_noImplicitCopy let y = argumentsAndReturns(x)
    print(y)
}

func letDecls(@_noImplicitCopy _ x: Klass) -> () {
    @_noImplicitCopy let y: Klass = x
    print(y)
}

func varDecls(@_noImplicitCopy _ x: Klass, @_noImplicitCopy _ x2: Klass) -> () {
    @_noImplicitCopy var y: Klass = x
    y = x2
    print(y)
}

func getKlass() -> Builtin.NativeObject {
    let k = Klass()
    let b = Builtin.unsafeCastToNativeObject(k)
    return Builtin.move(b)
}

@_noImplicitCopy var g: Builtin.NativeObject = getKlass()
@_noImplicitCopy let g2: Builtin.NativeObject = getKlass()
@_noImplicitCopy var g3: Builtin.NativeObject { getKlass() }

struct MyStruct {
    // Error if @_noImplicitCopy on struct fields. We do not have move only types and
    // these are part of MyStruct.
    //
    // TODO: Make error specific for move only on struct/enum.
    @_noImplicitCopy var x: Builtin.NativeObject = getKlass() // expected-error {{'@_noImplicitCopy' attribute cannot be applied to stored struct properties}}
    @_noImplicitCopy let y: Builtin.NativeObject = getKlass() // expected-error {{'@_noImplicitCopy' attribute cannot be applied to stored struct properties}}

    // We do support @_noImplicitCopy on computed properties though since they are
    // functions and we support @_noImplicitCopy return values.
    @_noImplicitCopy var myMoveOnly: Builtin.NativeObject {
        return getKlass()
    }

    func foo<T>(@_noImplicitCopy _ t: T) { // expected-error {{'@_noImplicitCopy' attribute cannot be applied to entities in generic contexts}}
    }
}

struct MyGenericStruct<T> {
    func foo(@_noImplicitCopy _ t: T) { // expected-error {{'@_noImplicitCopy' attribute cannot be applied to entities in generic contexts}}
    }
}

protocol P {
    @_noImplicitCopy var x: Builtin.NativeObject { get } // expected-error {{'@_noImplicitCopy' attribute cannot be applied to protocol properties}}
}

func foo<T>(@_noImplicitCopy _ t: T) { // expected-error {{'@_noImplicitCopy' attribute cannot be applied to entities in generic contexts}}
}

func foo2<T>(_ t: T, @_noImplicitCopy _ t2: Builtin.NativeObject) {
}

// Do not error on class fields. The noImplicitCopy field is separate from the
// underlying class itself so the fact the class is not move only does not
// suggest that the binding inside the class can be.
class MyClass {
    @_noImplicitCopy var x: Builtin.NativeObject = getKlass()
    @_noImplicitCopy let y: Builtin.NativeObject = getKlass()

    @_noImplicitCopy var myMoveOnly: Builtin.NativeObject {
        return getKlass()
    }

    func foo<T>(@_noImplicitCopy _ t: T) { // expected-error {{'@_noImplicitCopy' attribute cannot be applied to entities in generic contexts}}
    }
}

class MyGenericClass<T> {
    @_noImplicitCopy var x: T? = nil // expected-error {{'@_noImplicitCopy' attribute cannot be applied to entities in generic contexts}}
    @_noImplicitCopy let y: T? = nil // expected-error {{'@_noImplicitCopy' attribute cannot be applied to entities in generic contexts}}

    @_noImplicitCopy var myMoveOnly: T? { // expected-error {{'@_noImplicitCopy' attribute cannot be applied to entities in generic contexts}}
        return nil
    }

    @_noImplicitCopy var myMoveOnly2: Builtin.NativeObject? {
        return nil
    }

    func foo(@_noImplicitCopy _ t: T) { // expected-error {{'@_noImplicitCopy' attribute cannot be applied to entities in generic contexts}}
    }
}

// We need to error on Enums since the case is part of the value and we do not
// support move only types.
enum MyEnum {
    case none
    case noImplicitCopyCase(Klass)

    // We suport doing it on computed properties though.
    @_noImplicitCopy var myMoveOnly: Builtin.NativeObject {
        return getKlass()
    }
}

// We need to error on Enums since the case is part of the value and we do not
// support move only types.
enum MyGenericEnum<T> {
    case none
    case noImplicitCopyCase(Klass)

    // We suport doing it on computed properties though.
    @_noImplicitCopy var myMoveOnly: Builtin.NativeObject {
        return getKlass()
    }

    // We suport doing it on computed properties though.
    @_noImplicitCopy var myMoveOnly2: T? { // expected-error {{'@_noImplicitCopy' attribute cannot be applied to entities in generic contexts}}
        return nil
    }
}

