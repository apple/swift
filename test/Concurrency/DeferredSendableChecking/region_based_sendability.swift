// RUN: %target-typecheck-verify-swift -emit-sil -strict-concurrency=complete -enable-experimental-feature SendNonSendable -disable-availability-checking
// REQUIRES: concurrency
// REQUIRES: asserts

/*
 This file tests the early features of the flow-sensitive Sendable checking implemented by the SendNonSendable SIL pass.
 In particular, it tests the behavior of applications that cross isolation domains:
 - non-Sendable values are statically grouped into "regions" of values that may alias or point to each other
 - passing a non-Sendable value to an isolation-crossing call "consumed" the region of that value
 - accessing a non-Sendable value requires its region to have not been consumed

 All non-Sendable arguments, and "self" for non-Sendable classes, are assumed to be in the same region at the start of
 each function, and that region must be preserved at all times - it cannot be passed to another isolation domain.
 */

class NonSendable {
    var x : Any
    var y : Any

    init() {
        x = 0;
        y = 0;
    }

    init(_ z : Any) {
        x = z;
        y = z;
    }
}

actor A {
    func run_closure(_ closure : () -> ()) async {
        closure();
    }

    func foo(_ ns : Any) {}

    func foo_multi(_ : Any, _ : Any, _ : Any) {}

    func foo_vararg(_ : Any ...) {}
}

func foo_noniso(_ ns : Any) {}

func foo_noniso_multi(_ : Any, _ : Any, _ : Any) {}

func foo_noniso_vararg(_ : Any ...) {}

func test_isolation_crossing_sensitivity(a : A) async {
    let ns0 = NonSendable();
    let ns1 = NonSendable();

    //this call does not consume ns0
    foo_noniso(ns0);

    //this call consumes ns1
    await a.foo(ns1); //expected-warning{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (1 access site displayed)}}

    print(ns0);
    print(ns1); //expected-note{{access here could race}}
}

func test_arg_nonconsumable(a : A, ns_arg : NonSendable) async {
    let ns_let = NonSendable();

    // safe to consume an rvalue
    await a.foo(NonSendable());

    // safe to consume an lvalue
    await a.foo(ns_let);

    // not safe to consume an arg
    await a.foo(ns_arg); //expected-warning{{call site passes `self` or a non-sendable argument of this function to another thread, potentially yielding a race with the caller}}

    // check for no duplicate warnings once self is "consumed"
    await a.foo(NonSendable());
}

func test_closure_capture(a : A) async {
    let ns0 = NonSendable();
    let ns1 = NonSendable();
    let ns2 = NonSendable();
    let ns3 = NonSendable();

    let captures0 = { print(ns0) };
    let captures12 = { print(ns1); print(ns2)};

    let captures3 = { print(ns3) };
    let captures3indirect = { captures3() };

    // all lets should still be accessible - no consumed have happened yet
    print(ns0)
    print(ns1)
    print(ns2)
    print(ns3)

    // this should consume ns0
    await a.run_closure(captures0) //expected-warning{{passing argument of non-sendable type '() -> ()' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (3 access sites displayed)}}

    print(ns0) //expected-note{{access here could race}}
    print(ns1)
    print(ns2)
    print(ns3)

    // this should consume ns1 and ns2
    await a.run_closure(captures12) //expected-warning{{passing argument of non-sendable type '() -> ()' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (4 access sites displayed)}}

    print(ns0) //expected-note{{access here could race}}
    print(ns1) //expected-note{{access here could race}}
    print(ns2) //expected-note{{access here could race}}
    print(ns3)

    // this should consume ns3
    await a.run_closure(captures3indirect) //expected-warning{{passing argument of non-sendable type '() -> ()' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (1 access site displayed)}}

    print(ns0) //expected-note{{access here could race}}
    print(ns1) //expected-note{{access here could race}}
    print(ns2) //expected-note{{access here could race}}
    print(ns3) //expected-note{{access here could race}}
}

func test_regions(a : A, b : Bool) async {
    // definitely aliases - should be in the same region
    let ns0_0 = NonSendable();
    let ns0_1 = ns0_0;

    // possibly aliases - should be in the same region
    let ns1_0 = NonSendable();
    var ns1_1 = NonSendable();
    if (b) {
        ns1_1 = ns1_0;
    }

    // accessible via pointer - should be in the same region
    let ns2_0 = NonSendable();
    let ns2_1 = NonSendable();
    ns2_0.x = ns2_1;

    // check for each of the above pairs that consuming half of it consumes the other half

    if (b) {
        await a.foo(ns0_0) //expected-warning{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (2 access sites displayed)}}

        print(ns0_0) //expected-note{{access here could race}}
        print(ns0_1) //expected-note{{access here could race}}
    } else {
        await a.foo(ns0_1) //expected-warning{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (2 access sites displayed)}}

        print(ns0_0) //expected-note{{access here could race}}
        print(ns0_1) //expected-note{{access here could race}}
    }

    if (b) {
        await a.foo(ns1_0) //expected-warning{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (2 access sites displayed)}}

        print(ns1_0) //expected-note{{access here could race}}
        print(ns1_1) //expected-note{{access here could race}}
    } else {
        await a.foo(ns1_1) //expected-warning{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (2 access sites displayed)}}

        print(ns1_0) //expected-note{{access here could race}}
        print(ns1_1) //expected-note{{access here could race}}
    }

    if (b) {
        await a.foo(ns2_0) //expected-warning{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (2 access sites displayed)}}

        print(ns2_0) //expected-note{{access here could race}}
        print(ns2_1) //expected-note{{access here could race}}
    } else {
        await a.foo(ns2_1) //expected-warning{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (2 access sites displayed)}}

        print(ns2_0) //expected-note{{access here could race}}
        print(ns2_1) //expected-note{{access here could race}}
    }
}

func test_indirect_regions(a : A, b : Bool) async {
    // in each of the following, nsX_0 and nsX_1 should be in the same region for various reasons

    // 1 constructed using 0
    let ns0_0 = NonSendable();
    let ns0_1 = NonSendable(NonSendable(ns0_0));

    // 1 constructed using 0, indirectly
    let ns1_0 = NonSendable();
    let ns1_aux = NonSendable(ns1_0);
    let ns1_1 = NonSendable(ns1_aux);

    // 1 and 0 constructed with same aux value
    let ns2_aux = NonSendable();
    let ns2_0 = NonSendable(ns2_aux);
    let ns2_1 = NonSendable(ns2_aux);

    // 0 points to aux, which points to 1
    let ns3_aux = NonSendable();
    let ns3_0 = NonSendable();
    let ns3_1 = NonSendable();
    ns3_0.x = ns3_aux;
    ns3_aux.x = ns3_1;

    // 1 and 0 point to same aux value
    let ns4_aux = NonSendable();
    let ns4_0 = NonSendable();
    let ns4_1 = NonSendable();
    ns4_0.x = ns4_aux;
    ns4_1.x = ns4_aux;

    // same aux value points to both 0 and 1
    let ns5_aux = NonSendable();
    let ns5_0 = ns5_aux.x;
    let ns5_1 = ns5_aux.y;

    // now check for each pair that consuming half of it consumed the other half

    if (b) {
        await a.foo(ns0_0) //expected-warning{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (2 access sites displayed)}}

        print(ns0_0) //expected-note{{access here could race}}
        print(ns0_1) //expected-note{{access here could race}}
    } else {
        await a.foo(ns0_1) //expected-warning{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (2 access sites displayed)}}

        print(ns0_0) //expected-note{{access here could race}}
        print(ns0_1) //expected-note{{access here could race}}
    }

    if (b) {
        await a.foo(ns1_0) //expected-warning{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (2 access sites displayed)}}

        print(ns1_0) //expected-note{{access here could race}}
        print(ns1_1) //expected-note{{access here could race}}
    } else {
        await a.foo(ns1_1) //expected-warning{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (2 access sites displayed)}}

        print(ns1_0) //expected-note{{access here could race}}
        print(ns1_1) //expected-note{{access here could race}}
    }

    if (b) {
        await a.foo(ns2_0) //expected-warning{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (2 access sites displayed)}}

        print(ns2_0) //expected-note{{access here could race}}
        print(ns2_1) //expected-note{{access here could race}}
    } else {
        await a.foo(ns2_1) //expected-warning{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (2 access sites displayed)}}

        print(ns2_0) //expected-note{{access here could race}}
        print(ns2_1) //expected-note{{access here could race}}
    }

    if (b) {
        await a.foo(ns3_0) //expected-warning{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (2 access sites displayed)}}

        print(ns3_0) //expected-note{{access here could race}}
        print(ns3_1) //expected-note{{access here could race}}
    } else {
        await a.foo(ns3_1) //expected-warning{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (2 access sites displayed)}}

        print(ns3_0) //expected-note{{access here could race}}
        print(ns3_1) //expected-note{{access here could race}}
    }

    if (b) {
        await a.foo(ns4_0) //expected-warning{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (2 access sites displayed)}}

        print(ns4_0) //expected-note{{access here could race}}
        print(ns4_1) //expected-note{{access here could race}}
    } else {
        await a.foo(ns4_1) //expected-warning{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (2 access sites displayed)}}

        print(ns4_0) //expected-note{{access here could race}}
        print(ns4_1) //expected-note{{access here could race}}
    }

    if (b) {
        await a.foo(ns5_0) //expected-warning{{passing argument of non-sendable type 'Any' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (2 access sites displayed)}}

        print(ns5_0) //expected-note{{access here could race}}
        print(ns5_1) //expected-note{{access here could race}}
    } else {
        await a.foo(ns5_1) //expected-warning{{passing argument of non-sendable type 'Any' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (2 access sites displayed)}}

        print(ns5_0) //expected-note{{access here could race}}
        print(ns5_1) //expected-note{{access here could race}}
    }
}

// class methods in general cannot consume self, check that this is true except for Sendable classes (and actors)

class C_NonSendable {
    func bar() {}

    func bar(a : A) async {
        let captures_self = { self.bar() }

        // this is not a cross-isolation call, so it should be permitted
        foo_noniso(captures_self)

        // this is a cross-isolation call that captures non-Sendable self, so it should not be permitted
        await a.foo(captures_self) //expected-warning{{call site passes `self` or a non-sendable argument of this function to another thread, potentially yielding a race with the caller}}
    }
}

final class C_Sendable : Sendable {
    func bar() {}

    func bar(a : A) async {
        let captures_self = { self.bar() }

        // this is not a cross-isolation call, so it should be permitted
        foo_noniso(captures_self)

        // this is a cross-isolation, but self is Sendable, so it should be permitted
        await a.foo(captures_self)
    }
}

actor A_Sendable {
    func bar() {}

    func bar(a : A) async {
        let captures_self = { self.bar() }

        // this is not a cross-isolation call, so it should be permitted
        foo_noniso(captures_self)

        // this is a cross-isolation, but self is Sendable, so it should be permitted
        await a.foo(captures_self)
    }
}

func basic_loopiness(a : A, b : Bool) async {
    let ns = NonSendable()

    while (b) {
        await a.foo(ns)
        //expected-warning@-1{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (1 access site displayed)}}
        //expected-note@-2{{access here could race}}
    }
}

func basic_loopiness_unsafe(a : A, b : Bool) async {
    var ns0 = NonSendable()
    var ns1 = NonSendable()
    var ns2 = NonSendable()
    var ns3 = NonSendable()

    //should merge all vars
    while (b) {
        (ns0, ns1, ns2, ns3) = (ns1, ns2, ns3, ns0)
    }

    await a.foo(ns0) //expected-warning{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (1 access site displayed)}}
    await a.foo(ns3) //expected-note{{access here could race}}
}

func basic_loopiness_safe(a : A, b : Bool) async {
    var ns0 = NonSendable()
    var ns1 = NonSendable()
    var ns2 = NonSendable()
    var ns3 = NonSendable()

    //should keep ns0 and ns3 separate
    while (b) {
        (ns0, ns1, ns2, ns3) = (ns1, ns0, ns3, ns2)
    }

    await a.foo(ns0)
    await a.foo(ns3)
}

struct StructBox {
    var contents : NonSendable

    init() {
        contents = NonSendable()
    }
}

func test_struct_assign_doesnt_merge(a : A, b : Bool) async {
    let ns0 = NonSendable()
    let ns1 = NonSendable()

    //this should merge ns0 and ns1
    var box = StructBox()
    box.contents = ns0
    box.contents = ns1

    await a.foo(ns0)
    await a.foo(ns1)
}

class ClassBox {
    var contents : NonSendable

    init() {
        contents = NonSendable()
    }
}

func test_class_assign_merges(a : A, b : Bool) async {
    let ns0 = NonSendable()
    let ns1 = NonSendable()

    //this should merge ns0 and ns1
    //TODO: check out the warning here more
    var box = ClassBox() //expected-warning {{}}
    box.contents = ns0
    box.contents = ns1

    await a.foo(ns0) //expected-warning{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (1 access site displayed)}}
    await a.foo(ns1) //expected-note{{access here could race}}
}

func test_stack_assign_doesnt_merge(a : A, b : Bool) async {
    let ns0 = NonSendable()
    let ns1 = NonSendable()

    //this should NOT merge ns0 and ns1
    var contents : NonSendable
    contents = ns0
    contents = ns1

    foo_noniso(contents) //must use var to avoid warning

    await a.foo(ns0)
    await a.foo(ns1)
}

func test_stack_assign_and_capture_merges(a : A, b : Bool) async {
    let ns0 = NonSendable()
    let ns1 = NonSendable()

    //this should NOT merge ns0 and ns1
    var contents = NonSendable()

    let closure = { print(contents) }
    foo_noniso(closure) //must use let to avoid warning

    contents = ns0
    contents = ns1

    await a.foo(ns0) //expected-warning{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (1 access site displayed)}}
    await a.foo(ns1) //expected-note{{access here could race}}
}

func test_tuple_formation(a : A, i : Int) async {
    let ns0 = NonSendable();
    let ns1 = NonSendable();
    let ns2 = NonSendable();
    let ns3 = NonSendable();
    let ns4 = NonSendable();
    let ns012 = (ns0, ns1, ns2);
    let ns13 = (ns1, ns3);

    switch (i) {
    case 0:
        await a.foo(ns0) //expected-warning{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (6 access sites displayed)}}

        foo_noniso(ns0); //expected-note{{access here could race}}
        foo_noniso(ns1); //expected-note{{access here could race}}
        foo_noniso(ns2); //expected-note{{access here could race}}
        foo_noniso(ns3); //expected-note{{access here could race}}
        foo_noniso(ns4);
        foo_noniso(ns012); //expected-note{{access here could race}}
        foo_noniso(ns13); //expected-note{{access here could race}}
    case 1:
        await a.foo(ns1) //expected-warning{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (6 access sites displayed)}}

        foo_noniso(ns0); //expected-note{{access here could race}}
        foo_noniso(ns1); //expected-note{{access here could race}}
        foo_noniso(ns2); //expected-note{{access here could race}}
        foo_noniso(ns3); //expected-note{{access here could race}}
        foo_noniso(ns4);
        foo_noniso(ns012); //expected-note{{access here could race}}
        foo_noniso(ns13); //expected-note{{access here could race}}
    case 2:
        await a.foo(ns2) //expected-warning{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (6 access sites displayed)}}

        foo_noniso(ns0); //expected-note{{access here could race}}
        foo_noniso(ns1); //expected-note{{access here could race}}
        foo_noniso(ns2); //expected-note{{access here could race}}
        foo_noniso(ns3); //expected-note{{access here could race}}
        foo_noniso(ns4);
        foo_noniso(ns012); //expected-note{{access here could race}}
        foo_noniso(ns13); //expected-note{{access here could race}}
    case 3:
        await a.foo(ns4) //expected-warning{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (1 access site displayed)}}

        foo_noniso(ns0);
        foo_noniso(ns1);
        foo_noniso(ns2);
        foo_noniso(ns4); //expected-note{{access here could race}}
        foo_noniso(ns012);
        foo_noniso(ns13);
    case 4:
        await a.foo(ns012) //expected-warning{{passing argument of non-sendable type '(NonSendable, NonSendable, NonSendable)' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (6 access sites displayed)}}

        foo_noniso(ns0); //expected-note{{access here could race}}
        foo_noniso(ns1); //expected-note{{access here could race}}
        foo_noniso(ns2); //expected-note{{access here could race}}
        foo_noniso(ns3); //expected-note{{access here could race}}
        foo_noniso(ns4);
        foo_noniso(ns012); //expected-note{{access here could race}}
        foo_noniso(ns13); //expected-note{{access here could race}}
    default:
        await a.foo(ns13) //expected-warning{{passing argument of non-sendable type '(NonSendable, NonSendable)' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (6 access sites displayed)}}

        foo_noniso(ns0); //expected-note{{access here could race}}
        foo_noniso(ns1); //expected-note{{access here could race}}
        foo_noniso(ns2); //expected-note{{access here could race}}
        foo_noniso(ns3); //expected-note{{access here could race}}
        foo_noniso(ns4);
        foo_noniso(ns012); //expected-note{{access here could race}}
        foo_noniso(ns13); //expected-note{{access here could race}}
    }
}

func reuse_args_safe(a : A) async {
    let ns0 = NonSendable();
    let ns1 = NonSendable();
    let ns2 = NonSendable();

    // this should be perfectly legal - arguments are assumed to come from the same region
    await a.foo_multi(ns0, ns0, ns0);
    await a.foo_multi(ns1, ns2, ns1);
}

func one_consume_many_require(a : A) async {
    let ns0 = NonSendable();
    let ns1 = NonSendable();
    let ns2 = NonSendable();
    let ns3 = NonSendable();
    let ns4 = NonSendable();

    await a.foo_multi(ns0, ns1, ns2);
    //expected-warning@-1 3{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (1 access site displayed)}}

    foo_noniso_multi(ns0, ns3, ns4); //expected-note {{access here could race}}
    foo_noniso_multi(ns3, ns1, ns4); //expected-note {{access here could race}}
    foo_noniso_multi(ns4, ns3, ns2); //expected-note {{access here could race}}
}

func one_consume_one_require(a : A) async {
    let ns0 = NonSendable();
    let ns1 = NonSendable();
    let ns2 = NonSendable();

    await a.foo_multi(ns0, ns1, ns2);
    //expected-warning@-1 3{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (1 access site displayed)}}

    foo_noniso_multi(ns0, ns1, ns2); //expected-note 3{{access here could race}}
}

func many_consume_one_require(a : A) async {
    let ns0 = NonSendable();
    let ns1 = NonSendable();
    let ns2 = NonSendable();
    let ns3 = NonSendable();
    let ns4 = NonSendable();
    let ns5 = NonSendable();

    await a.foo_multi(ns0, ns3, ns3)
    //expected-warning@-1{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (1 access site displayed)}}
    await a.foo_multi(ns4, ns1, ns4)
    //expected-warning@-1{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (1 access site displayed)}}
    await a.foo_multi(ns5, ns5, ns2)
    //expected-warning@-1{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (1 access site displayed)}}

    foo_noniso_multi(ns0, ns1, ns2); //expected-note 3{{access here could race}}
}

func many_consume_many_require(a : A) async {
    let ns0 = NonSendable();
    let ns1 = NonSendable();
    let ns2 = NonSendable();
    let ns3 = NonSendable();
    let ns4 = NonSendable();
    let ns5 = NonSendable();
    let ns6 = NonSendable();
    let ns7 = NonSendable();

    await a.foo_multi(ns0, ns3, ns3)
    //expected-warning@-1{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (1 access site displayed)}}
    await a.foo_multi(ns4, ns1, ns4)
    //expected-warning@-1{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (1 access site displayed)}}
    await a.foo_multi(ns5, ns5, ns2)
    //expected-warning@-1{{passing argument of non-sendable type 'NonSendable' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (1 access site displayed)}}

    foo_noniso_multi(ns0, ns6, ns7); //expected-note {{access here could race}}
    foo_noniso_multi(ns6, ns1, ns7); //expected-note {{access here could race}}
    foo_noniso_multi(ns7, ns6, ns2); //expected-note {{access here could race}}
}


func reuse_args_safe_vararg(a : A) async {
    let ns0 = NonSendable();
    let ns1 = NonSendable();
    let ns2 = NonSendable();

    // this should be perfectly legal - arguments are assumed to come from the same region
    await a.foo_vararg(ns0, ns0, ns0);
    await a.foo_vararg(ns1, ns2, ns1);
}

func one_consume_many_require_varag(a : A) async {
    let ns0 = NonSendable();
    let ns1 = NonSendable();
    let ns2 = NonSendable();
    let ns3 = NonSendable();
    let ns4 = NonSendable();

    //TODO: find a way to make the type used in the diagnostic more specific than the signature type
    await a.foo_vararg(ns0, ns1, ns2);
    //expected-warning@-1 {{passing argument of non-sendable type 'Any...' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (3 access sites displayed)}}

    foo_noniso_vararg(ns0, ns3, ns4); //expected-note {{access here could race}}
    foo_noniso_vararg(ns3, ns1, ns4); //expected-note {{access here could race}}
    foo_noniso_vararg(ns4, ns3, ns2); //expected-note {{access here could race}}
}

func one_consume_one_require_vararg(a : A) async {
    let ns0 = NonSendable();
    let ns1 = NonSendable();
    let ns2 = NonSendable();

    await a.foo_vararg(ns0, ns1, ns2);
    //expected-warning@-1 {{passing argument of non-sendable type 'Any...' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (3 access sites displayed)}}

    foo_noniso_vararg(ns0, ns1, ns2); //expected-note 3{{access here could race}}
}

func many_consume_one_require_vararg(a : A) async {
    let ns0 = NonSendable();
    let ns1 = NonSendable();
    let ns2 = NonSendable();
    let ns3 = NonSendable();
    let ns4 = NonSendable();
    let ns5 = NonSendable();

    await a.foo_vararg(ns0, ns3, ns3)
    //expected-warning@-1{{passing argument of non-sendable type 'Any...' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (1 access site displayed)}}
    await a.foo_vararg(ns4, ns1, ns4)
    //expected-warning@-1{{passing argument of non-sendable type 'Any...' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (1 access site displayed)}}
    await a.foo_vararg(ns5, ns5, ns2)
    //expected-warning@-1{{passing argument of non-sendable type 'Any...' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (1 access site displayed)}}

    foo_noniso_vararg(ns0, ns1, ns2); //expected-note 3{{access here could race}}
}

func many_consume_many_require_vararg(a : A) async {
    let ns0 = NonSendable();
    let ns1 = NonSendable();
    let ns2 = NonSendable();
    let ns3 = NonSendable();
    let ns4 = NonSendable();
    let ns5 = NonSendable();
    let ns6 = NonSendable();
    let ns7 = NonSendable();

    await a.foo_vararg(ns0, ns3, ns3)
    //expected-warning@-1{{passing argument of non-sendable type 'Any...' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (1 access site displayed)}}
    await a.foo_vararg(ns4, ns1, ns4)
    //expected-warning@-1{{passing argument of non-sendable type 'Any...' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (1 access site displayed)}}
    await a.foo_vararg(ns5, ns5, ns2)
    //expected-warning@-1{{passing argument of non-sendable type 'Any...' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (1 access site displayed)}}

    foo_noniso_vararg(ns0, ns6, ns7); //expected-note {{access here could race}}
    foo_noniso_vararg(ns6, ns1, ns7); //expected-note {{access here could race}}
    foo_noniso_vararg(ns7, ns6, ns2); //expected-note {{access here could race}}
}

enum E {
    case E1(NonSendable)
    case E2(NonSendable)
    case E3(NonSendable)
}

func enum_test(a : A) async {
    let e1 = E.E1(NonSendable())
    let e2 = E.E2(NonSendable())
    let e3 = E.E3(NonSendable())
    let e4 = E.E1(NonSendable())

    switch (e1) {

    //this case merges e1 and e2
    case let .E1(ns1):
        switch (e2) {
        case let .E2(ns2):
            ns1.x = ns2.x
        default: ()
        }

    //this case consumes e3
    case .E2:
        switch (e3) {
        case let .E3(ns3):
            await a.foo(ns3.x); //expected-warning{{passing argument of non-sendable type 'Any' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (1 access site displayed)}}
        default: ()
        }

    //this case does nothing
    case .E3:
        foo_noniso(e4);
    }

    await a.foo(e1); //expected-warning{{passing argument of non-sendable type 'E' from nonisolated context to actor-isolated context at this call site could yield a race with accesses later in this function (1 access site displayed)}}
    foo_noniso(e2); //expected-note{{access here could race}}
    foo_noniso(e3); //expected-note{{access here could race}}
    foo_noniso(e4);
}
