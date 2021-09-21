// RUN: %target-typecheck-verify-swift -parse-stdlib -disable-availability-checking -verify-syntax-tree

import Swift

class Klass {}

func argumentsAndReturns(_ x: @_moveOnly Klass) -> @_moveOnly Klass {
    return x
}

func passArguments(_ x: @_moveOnly Klass) -> () {
    let y: @_moveOnly Klass = argumentsAndReturns(x)
    print(y)
}

func letDecls(_ x: @_moveOnly Klass) -> () {
    let y: @_moveOnly Klass = x
    print(y)
}

func varDecls(_ x: @_moveOnly Klass, _ x2: @_moveOnly Klass) -> () {
    var y: @_moveOnly Klass = x
    y = x2
    print(y)
}

func getKlass() -> @_moveOnly Builtin.NativeObject {
    let k = Klass()
    let b = Builtin.unsafeCastToNativeObject(k)
    return Builtin.move(b)
}

var g: @_moveOnly Builtin.NativeObject = getKlass()
let g2: @_moveOnly Builtin.NativeObject = getKlass()
var g3: @_moveOnly Builtin.NativeObject { getKlass() }

struct MyStruct {
    // Error if @_moveOnly on struct fields. We do not have move only types and
    // these are part of MyStruct.
    var x: @_moveOnly Builtin.NativeObject = getKlass()
    let y: @_moveOnly Builtin.NativeObject = getKlass()

    // We do support @_moveOnly on computed properties though since they are
    // functions and we support @_moveOnly return values.
    var myMoveOnly: @_moveOnly Builtin.NativeObject {
        return getKlass()
    }
}

// Do not error on class fields. The moveOnly field is separate from the
// underlying class itself so the fact the class is not move only does not
// suggest that the binding inside the class can be.
class MyClass {
    var x: @_moveOnly Builtin.NativeObject = getKlass()
    let y: @_moveOnly Builtin.NativeObject = getKlass()

    var myMoveOnly: @_moveOnly Builtin.NativeObject {
        return getKlass()
    }
}

// We need to error on Enums since the case is part of the value and we do not
// support move only types.
enum MyEnum {
    case none
    case moveOnlyCase(@_moveOnly Klass)

    // We suport doing it on computed properties though.
    var myMoveOnly: @_moveOnly Builtin.NativeObject {
        return getKlass()
    }
}
