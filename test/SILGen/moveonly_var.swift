// RUN: %target-swift-emit-silgen -enable-experimental-feature MoveOnlyClasses %s | %FileCheck %s
// RUN: %target-swift-emit-sil -O -sil-verify-all -enable-experimental-feature MoveOnlyClasses %s | %FileCheck %s

//////////////////
// Declarations //
//////////////////

class OrdinaryClass {}

@_moveOnly
public enum MaybeKlass {
    case just(Klass)
    case none
}

@_moveOnly
public class Klass {
    var intField: Int
    var klsField: OrdinaryClass // FIXME(104504239): this is suppose to be MaybeKlass, or better yet, Optional<Klass>

    init() {
        klsField = OrdinaryClass()
        intField = 5
    }
}

public func nonConsumingUseKlass(_ k: borrowing Klass) {}

///////////
// Tests //
///////////

// -----------
// Class Tests
//

// CHECK-LABEL: useVarKlass
public func useVarKlassNoErrorSimple() {
    var k = Klass()
    k = Klass()

    nonConsumingUseKlass(k)
    let k2 = k
    let _ = k2
}

/*
public func useVarKlassErrorSimple() {
    var k = Klass()
    let k1 = k
    let _ = k1
    let k2 = k
    let _ = k2

    k = Klass()
    let k3 = k
    let _ = k3
    let k4 = k
    let _ = k4

    k = Klass()
    let k5 = k
    let _ = k5
}
*/
