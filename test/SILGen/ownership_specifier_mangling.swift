// RUN: %target-swift-emit-silgen -enable-experimental-feature NoImplicitCopy %s | %FileCheck %s

// The internal `__shared` and `__owned` modifiers would always affect
// symbol mangling, even if they don't have a concrete impact on ABI. The
// The public `borrowing` and `consuming` modifiers are only supposed to
// affect symbol mangling when they would change the default ABI behavior.

// CHECK-LABEL: @$s28ownership_specifier_mangling17officialModifiersyySS_SStF
func officialModifiers(_: String, _: String) {}
// CHECK-LABEL: @$s28ownership_specifier_mangling17officialModifiersyySS_SSntF
func officialModifiers(_: borrowing String, _: consuming String) {}
// CHECK-LABEL: @$s28ownership_specifier_mangling17officialModifiersyySSn_SStF
func officialModifiers(_: consuming String, _: borrowing String) {}

// CHECK-LABEL: @$s28ownership_specifier_mangling15legacyModifiersyySS_SStF
func legacyModifiers(_: String, _: String) {}
// CHECK-LABEL: @$s28ownership_specifier_mangling15legacyModifiersyySSh_SSntF
func legacyModifiers(_: __shared String, _: __owned String) {}
// CHECK-LABEL: @$s28ownership_specifier_mangling15legacyModifiersyySSn_SShtF
func legacyModifiers(_: __owned String, _: __shared String) {}

class Foo {
    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC17officialModifiers_ACSS_SStcfc
    init(officialModifiers: String, _: String) {}
    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC17officialModifiers_ACSSh_SStcfc
    init(officialModifiers: borrowing String, _: consuming String) {}
    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC17officialModifiers_ACSS_SShtcfc
    init(officialModifiers: consuming String, _: borrowing String) {}

    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC15legacyModifiers_ACSS_SStcfc
    init(legacyModifiers: String, _: String) {}
    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC15legacyModifiers_ACSSh_SSntcfc
    init(legacyModifiers: __shared String, _: __owned String) {}
    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC15legacyModifiers_ACSSn_SShtcfc
    init(legacyModifiers: __owned String, _: __shared String) {}

    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC26officialModifiersInClosureyyySS_SStXEF
    func officialModifiersInClosure(_: (String, String) -> ()) {}
    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC26officialModifiersInClosure2bcyySS_SSntXE_tF
    func officialModifiersInClosure(bc: (borrowing String, consuming String) -> ()) {}
    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC26officialModifiersInClosure2cbyySSn_SStXE_tF
    func officialModifiersInClosure(cb: (consuming String, borrowing String) -> ()) {}

    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC24legacyModifiersInClosureyyySS_SStXEF
    func legacyModifiersInClosure(_: (String, String) -> ()) {}
    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC24legacyModifiersInClosure2bcyySSh_SSntXE_tF
    func legacyModifiersInClosure(bc: (__shared String, __owned String) -> ()) {}
    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC24legacyModifiersInClosure2cbyySSn_SShtXE_tF
    func legacyModifiersInClosure(cb: (__owned String, __shared String) -> ()) {}

    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC26officialModifiersInClosureACySS_SStXE_tcfc
    init(officialModifiersInClosure: (String, String) -> ()) {}
    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC29officialModifiersInClosure_bcACySS_SSntXE_tcfc
    init(officialModifiersInClosure_bc: (borrowing String, consuming String) -> ()) {}
    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC29officialModifiersInClosure_cbACySSn_SStXE_tcfc
    init(officialModifiersInClosure_cb: (consuming String, borrowing String) -> ()) {}

    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC24legacyModifiersInClosureACySS_SStXE_tcfc
    init(legacyModifiersInClosure: (String, String) -> ()) {}
    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC27legacyModifiersInClosure_bcACySSh_SSntXE_tcfc
    init(legacyModifiersInClosure_bc: (__shared String, __owned String) -> ()) {}
    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC27legacyModifiersInClosure_cbACySSn_SShtXE_tcfc
    init(legacyModifiersInClosure_cb: (__owned String, __shared String) -> ()) {}

    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC26officialModifiersInClosureyySS_SStcvg
    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC26officialModifiersInClosureyySS_SStcvs
    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC26officialModifiersInClosureyySS_SStcvM
    var officialModifiersInClosure: (String, String) -> () = {_,_ in}
    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC29officialModifiersInClosure_bcyySS_SSntcvg
    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC29officialModifiersInClosure_bcyySS_SSntcvs
    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC29officialModifiersInClosure_bcyySS_SSntcvM
    var officialModifiersInClosure_bc: (borrowing String, consuming String) -> () = {_,_ in}
    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC29officialModifiersInClosure_cbyySSn_SStcvg
    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC29officialModifiersInClosure_cbyySSn_SStcvs
    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC29officialModifiersInClosure_cbyySSn_SStcvM
    var officialModifiersInClosure_cb: (consuming String, borrowing String) -> () = {_,_ in}

    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC24legacyModifiersInClosureyySS_SStcvg
    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC24legacyModifiersInClosureyySS_SStcvs
    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC24legacyModifiersInClosureyySS_SStcvM
    var legacyModifiersInClosure: (String, String) -> () = {_,_ in}
    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC27legacyModifiersInClosure_bcyySSh_SSntcvg
    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC27legacyModifiersInClosure_bcyySSh_SSntcvs
    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC27legacyModifiersInClosure_bcyySSh_SSntcvM
    var legacyModifiersInClosure_bc: (__shared String, __owned String) -> () = {_,_ in}
    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC27legacyModifiersInClosure_cbyySSn_SShtcvg
    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC27legacyModifiersInClosure_cbyySSn_SShtcvs
    // CHECK-LABEL: @$s28ownership_specifier_mangling3FooC27legacyModifiersInClosure_cbyySSn_SShtcvM
    var legacyModifiersInClosure_cb: (__owned String, __shared String) -> () = {_,_ in}
}
