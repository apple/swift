// RUN: %target-typecheck-verify-swift -dump-ast > %t.dump
// RUN: %FileCheck %s < %t.dump

// SR-13815
extension Optional {
    func sr13815() -> SR13815? { SR13815() }
    static func sr13815_2() -> SR13815? { SR13815() }
    static func sr13815_3() -> SR13815? { SR13815() }
    static var sr13815_wrongType: Int { 0 }
    static var sr13815_overload: SR13815 { SR13815() }
    init(overloaded: Void) { self = nil }
}

struct SR13815 {
    static var sr13815: SR13815? = SR13815()
    static var sr13815_2: SR13815? = SR13815()
    static var sr13815_wrongType: SR13815? { SR13815() }
    static var p_SR13815: SR13815? { SR13815() }
    static func sr13815_3() -> SR13815? { SR13815() }
    static var sr13815_overload: SR13815? { SR13815() }
    init(overloaded: Void) {}
    init?(failable: Void) {}
    init() {}
}

protocol P_SR13815 {}
extension Optional: P_SR13815 where Wrapped: Equatable {
    static func p_SR13815() {}
}

let _: SR13815? = .sr13815
let _: SR13815? = .sr13815_wrongType
let _: SR13815? = .init()
let _: SR13815? = .sr13815() // expected-error {{instance member 'sr13815' cannot be used on type 'SR13815?'}}
let _: SR13815? = .sr13815_2()
let _: SR13815? = .init(SR13815())
let _: SR13815? = .init(overloaded: ())
// If members exist on Optional and Wrapped, always choose the one on optional
// CHECK: declref_expr {{.*}} location='{{.*}}optional_overload.swift:37:
// CHECK-SAME: decl='optional_overload.(file).Optional extension.init(overloaded:)@
let _: SR13815? = .sr13815_overload
// Should choose the overload from Optional even if the Wrapped overload would otherwise have a better score
// CHECK: member_ref_expr {{.*}} location='{{.*}}optional_overload.swift:41:
// CHECK-SAME: decl='optional_overload.(file).Optional extension.sr13815_overload@
let _: SR13815? = .init(failable: ())
let _: SR13815? = .sr13815_3()
let _: SR13815? = .p_SR13815
