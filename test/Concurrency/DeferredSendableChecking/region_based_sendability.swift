// RUN: %target-typecheck-verify-swift -emit-sil -strict-concurrency=complete -enable-experimental-feature DeferredSendableChecking
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

@available(SwiftStdlib 5.1, *)
actor A {
    func run_closure(_ closure : () -> ()) async {
        closure();
    }

    func foo(_ ns : Any) {}
}

func foo_noniso(_ ns : Any) {}

@available(SwiftStdlib 5.1, *)
func test_isolation_crossing_sensitivity(a : A) async {
    let ns0 = NonSendable();
    let ns1 = NonSendable();

    //this call does not consume ns0
    foo_noniso(ns0);

    //this call consumes ns1
    await a.foo(ns1);

    print(ns0);
    print(ns1); //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
}

@available(SwiftStdlib 5.1, *)
func test_arg_nonconsumable(a : A, ns_arg : NonSendable) async {
    let ns_let = NonSendable();

    // safe to consume an rvalue
    await a.foo(NonSendable());

    // safe to consume an lvalue
    await a.foo(ns_let);

    // not safe to consume an arg
    await a.foo(ns_arg); //expected-warning{{This application could pass `self` or a Non-Sendable argument of this function to another thread, potentially yielding a race with the caller}}
}

@available(SwiftStdlib 5.1, *)
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
    await a.run_closure(captures0)

    print(ns0) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
    print(ns1)
    print(ns2)
    print(ns3)

    // this should consume ns1 and ns2
    await a.run_closure(captures12)

    print(ns0) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
    print(ns1) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
    print(ns2) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
    print(ns3)

    // this should consume ns3
    await a.run_closure(captures3indirect)

    print(ns0) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
    print(ns1) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
    print(ns2) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
    print(ns3) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
}

@available(SwiftStdlib 5.1, *)
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
        await a.foo(ns0_0)

        print(ns0_0) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
        print(ns0_1) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
    } else {
        await a.foo(ns0_1)

        print(ns0_0) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
        print(ns0_1) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
    }

    if (b) {
        await a.foo(ns1_0)

        print(ns1_0) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
        print(ns1_1) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
    } else {
        await a.foo(ns1_1)

        print(ns1_0) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
        print(ns1_1) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
    }

    if (b) {
        await a.foo(ns2_0)

        print(ns2_0) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
        print(ns2_1) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
    } else {
        await a.foo(ns2_1)

        print(ns2_0) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
        print(ns2_1) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
    }
}

@available(SwiftStdlib 5.1, *)
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
        await a.foo(ns0_0)

        print(ns0_0) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
        print(ns0_1) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
    } else {
        await a.foo(ns0_1)

        print(ns0_0) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
        print(ns0_1) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
    }

    if (b) {
        await a.foo(ns1_0)

        print(ns1_0) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
        print(ns1_1) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
    } else {
        await a.foo(ns1_1)

        print(ns1_0) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
        print(ns1_1) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
    }

    if (b) {
        await a.foo(ns2_0)

        print(ns2_0) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
        print(ns2_1) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
    } else {
        await a.foo(ns2_1)

        print(ns2_0) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
        print(ns2_1) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
    }

    if (b) {
        await a.foo(ns3_0)

        print(ns3_0) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
        print(ns3_1) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
    } else {
        await a.foo(ns3_1)

        print(ns3_0) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
        print(ns3_1) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
    }

    if (b) {
        await a.foo(ns4_0)

        print(ns4_0) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
        print(ns4_1) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
    } else {
        await a.foo(ns4_1)

        print(ns4_0) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
        print(ns4_1) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
    }

    if (b) {
        await a.foo(ns5_0)

        print(ns5_0) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
        print(ns5_1) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
    } else {
        await a.foo(ns5_1)

        print(ns5_0) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
        print(ns5_1) //expected-warning{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
    }
}

// class methods in general cannot consume self, check that this is true except for Sendable classes (and actors)

@available(SwiftStdlib 5.1, *)
class C_NonSendable {
    func bar() {}

    func bar(a : A) async {
        let captures_self = { self.bar() }

        // this is not a cross-isolation call, so it should be permitted
        foo_noniso(captures_self)

        // this is a cross-isolation call that captures non-Sendable self, so it should not be permitted
        await a.foo(captures_self) //expected-warning{{This application could pass `self` or a Non-Sendable argument of this function to another thread, potentially yielding a race with the caller}}
    }
}

@available(SwiftStdlib 5.1, *)
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

@available(SwiftStdlib 5.1, *)
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

@available(SwiftStdlib 5.1, *)
func basic_loopiness(a : A, b : Bool) async {
    let ns = NonSendable()

    while (b) {
        //TODO: remove duplicate warnings here and in following test cases
        await a.foo(ns) //expected-warning 2{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
    }
}

@available(SwiftStdlib 5.1, *)
func basic_loopiness_unsafe(a : A, b : Bool) async {
    var ns0 = NonSendable()
    var ns1 = NonSendable()
    var ns2 = NonSendable()
    var ns3 = NonSendable()

    //should merge all vars
    while (b) {
        (ns0, ns1, ns2, ns3) = (ns1, ns2, ns3, ns0)
    }

    await a.foo(ns0)
    await a.foo(ns3) //expected-warning 2{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
}

@available(SwiftStdlib 5.1, *)
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

@available(SwiftStdlib 5.1, *)
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

@available(SwiftStdlib 5.1, *)
func test_class_assign_merges(a : A, b : Bool) async {
    let ns0 = NonSendable()
    let ns1 = NonSendable()

    //this should merge ns0 and ns1
    //TODO: check out the warning here more
    var box = ClassBox() //expected-warning {{}}
    box.contents = ns0
    box.contents = ns1

    await a.foo(ns0)
    await a.foo(ns1) //expected-warning 2{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
}

@available(SwiftStdlib 5.1, *)
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

@available(SwiftStdlib 5.1, *)
func test_stack_assign_and_capture_merges(a : A, b : Bool) async {
    let ns0 = NonSendable()
    let ns1 = NonSendable()

    //this should NOT merge ns0 and ns1
    var contents = NonSendable()

    let closure = { print(contents) }
    foo_noniso(closure) //must use let to avoid warning

    contents = ns0
    contents = ns1

    await a.foo(ns0)
    await a.foo(ns1) //expected-warning 2{{Non-Sendable value consumed, then used at this site; could yield race with another thread}}
}
