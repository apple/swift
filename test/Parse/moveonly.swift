// RUN: %target-typecheck-verify-swift -parse-stdlib -disable-availability-checking -verify-syntax-tree

import Swift

class Klass {}

func argumentsAndReturns(_ x: @_moveOnly Klass) -> @_moveOnly Klass {
    return x
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


