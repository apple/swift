// RUN: %target-typecheck-verify-swift -parse-stdlib -disable-availability-checking -verify-syntax-tree

import Swift

class Klass {}

func argumentsAndReturns(@_noImplicitCopy _ x: Klass) -> Klass {
    return x
}

func letDecls(@_noImplicitCopy  _ x: Klass) -> () {
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


