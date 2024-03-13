// RUN: %target-typecheck-verify-swift -dump-ast > %t.dump
// RUN: %FileCheck %s < %t.dump

// https://github.com/apple/swift/issues/56212

extension Optional {
    func member1() -> S1? {}
    static func member2() -> S1? {}
    static func member3() -> S1? {}
    static var member_wrongType: Int { get {} }
    static var member_overload: S1 { get {} }

    init(overloaded: Void) {}
}

protocol P1 {}
extension Optional: P1 where Wrapped: Equatable {
    static func member4() {}
}

struct S1 {
    static var member1: S1? = S1()
    static var member2: S1? = S1()
    static func member3() -> S1? {}
    static var member4: S1? { get {} }
    static var member_wrongType: S1? { get {} }
    static var member_overload: S1? { get {} }

    init(overloaded: Void) {}
    init?(failable: Void) {}
    init() {}
}

let _: S1? = .member1
let _: S1? = .member_wrongType
let _: S1? = .init()
let _: S1? = .member1() // expected-error {{instance member 'member1' of type 'S1?' cannot be used in static context}}
let _: S1? = .member2()
let _: S1? = .init(S1())
let _: S1? = .init(overloaded: ())
// If members exist on Optional and Wrapped, always choose the one on optional
// CHECK: declref_expr {{.*}} location={{.*}}optional_overload.swift:40
// CHECK-SAME: decl="optional_overload.(file).Optional extension.init(overloaded:)@
let _: S1? = .member_overload
// Should choose the overload from Optional even if the Wrapped overload would otherwise have a better score
// CHECK: member_ref_expr {{.*}} location={{.*}}optional_overload.swift:44
// CHECK-SAME: decl="optional_overload.(file).Optional extension.member_overload@
let _: S1? = .init(failable: ())
let _: S1? = .member3()
let _: S1? = .member4
