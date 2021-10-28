// RUN: %target-swift-frontend -parse-stdlib %s -disable-access-control -disable-objc-attr-requires-foundation-module -emit-sil

class Klass {}

func useKlass(_ k: Klass) {}

func useMove(_ k: Klass) -> Klass {
    let k2 = k
    let k3 = k2
    let k4 = Builtin.move(k2)
    useKlass(k3)
    return k4
}
