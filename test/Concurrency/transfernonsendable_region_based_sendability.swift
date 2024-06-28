// RUN: %target-swift-frontend  -strict-concurrency=complete -disable-availability-checking -parse-as-library -emit-sil -o /dev/null -verify -verify-additional-prefix tns- %s -enable-upcoming-feature GlobalActorIsolatedTypesUsability

// REQUIRES: concurrency
// REQUIRES: asserts

/*
 This file tests the early features of the flow-sensitive Sendable checking implemented by the TransferNonSendable SIL pass.
 In particular, it tests the behavior of applications that cross isolation domains:
 - non-Sendable values are statically grouped into "regions" of values that may alias or point to each other
 - passing a non-Sendable value to an isolation-crossing call "consumed" the region of that value
 - accessing a non-Sendable value requires its region to have not been consumed

 All non-Sendable arguments, and "self" for non-Sendable classes, are assumed to be in the same region at the start of
 each function, and that region must be preserved at all times - it cannot be passed to another isolation domain.
 */

class NonSendable { // expected-complete-note 73{{}}
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

var bool : Bool { false }

func test_isolation_crossing_sensitivity(a : A) async {
  let ns0 = NonSendable();
  let ns1 = NonSendable();

  // This call does not consume ns0
  foo_noniso(ns0);

  // This call consumes ns1
  await a.foo(ns1); // expected-tns-warning {{sending 'ns1' risks causing data races}}
  // expected-tns-note @-1 {{sending 'ns1' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}

  print(ns0);
  print(ns1); // expected-tns-note {{access can happen concurrently}}
}

func test_arg_nonconsumable(a : A, ns_arg : NonSendable) async {
  let ns_let = NonSendable();

  // Safe to consume an rvalue.
  await a.foo(NonSendable()); // expected-complete-warning {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}

  // Safe to consume an lvalue.
  await a.foo(ns_let); // expected-complete-warning {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}

  // Not safe to consume an arg.
  await a.foo(ns_arg); // expected-tns-warning {{sending 'ns_arg' risks causing data races}}
  // expected-tns-note @-1 {{sending task-isolated 'ns_arg' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and task-isolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}

  // Check for no duplicate warnings once self is "consumed"
  await a.foo(NonSendable()); // expected-complete-warning {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}
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
  await a.run_closure(captures0) // expected-tns-warning {{sending value of non-Sendable type '() -> ()' with later accesses from nonisolated context to actor-isolated context risks causing data races}}
  // expected-complete-warning @-1 {{passing argument of non-sendable type '() -> ()' into actor-isolated context may introduce data races}}
  // expected-complete-note @-2 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}

  print(ns0) // expected-tns-note {{access can happen concurrently}}
  print(ns1)
  print(ns2)
  print(ns3)

  // this should consume ns1 and ns2
  await a.run_closure(captures12) // expected-tns-warning {{sending value of non-Sendable type '() -> ()' with later accesses from nonisolated context to actor-isolated context risks causing data races}}
  // expected-complete-warning @-1 {{passing argument of non-sendable type '() -> ()' into actor-isolated context may introduce data races}}
  // expected-complete-note @-2 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}

  // This only touches ns1/ns2 so we get an error on ns1 since it is first.
  print(ns0)
  print(ns1) // expected-tns-note {{access can happen concurrently}}
  print(ns2)
  print(ns3)

  // this should consume ns3
  await a.run_closure(captures3indirect) // expected-tns-warning {{sending value of non-Sendable type '() -> ()' with later accesses from nonisolated context to actor-isolated context risks causing data races}}
  // expected-complete-warning @-1 {{passing argument of non-sendable type '() -> ()' into actor-isolated context may introduce data races}}
  // expected-complete-note @-2 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}

  // This only uses ns3 so we should only emit an error on ns3.
  print(ns0)
  print(ns1)
  print(ns2)
  print(ns3) // expected-tns-note {{access can happen concurrently}}
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
    await a.foo(ns0_0) // expected-tns-warning {{sending 'ns0_0' risks causing data races}}
    // expected-tns-note @-1 {{sending 'ns0_0' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}

    if (b) {
      print(ns0_0) // expected-tns-note {{access can happen concurrently}}
    } else {
      print(ns0_1) // expected-tns-note {{access can happen concurrently}}
    }
  } else {
    await a.foo(ns0_1) // expected-tns-warning {{sending 'ns0_1' risks causing data races}}
    // expected-tns-note @-1 {{sending 'ns0_1' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}

    if b {
      print(ns0_0) // expected-tns-note {{access can happen concurrently}}
    } else {
      print(ns0_1) // expected-tns-note {{access can happen concurrently}}
    }
  }

  if (b) {
    await a.foo(ns1_0) // expected-tns-warning {{sending 'ns1_0' risks causing data races}}
    // expected-tns-note @-1 {{sending 'ns1_0' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}

    if b {
      print(ns1_0) // expected-tns-note {{access can happen concurrently}}
    } else {
      print(ns1_1) // expected-tns-note {{access can happen concurrently}}
    }
  } else {
    await a.foo(ns1_1) // expected-tns-warning {{sending 'ns1_1' risks causing data races}}
    // expected-tns-note @-1 {{sending 'ns1_1' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}

    if b {
      print(ns1_0) // expected-tns-note {{access can happen concurrently}}
    } else {
      print(ns1_1) // expected-tns-note {{access can happen concurrently}}
    }
  }

  if (b) {
    await a.foo(ns2_0) // expected-tns-warning {{sending 'ns2_0' risks causing data races}}
    // expected-tns-note @-1 {{sending 'ns2_0' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}

    if b {
      print(ns2_0) // expected-tns-note {{access can happen concurrently}}
    } else {
      print(ns2_1) // expected-tns-note {{access can happen concurrently}}
    }
  } else {
    await a.foo(ns2_1) // expected-tns-warning {{sending 'ns2_1' risks causing data races}}
    // expected-tns-note @-1 {{sending 'ns2_1' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}

    if b {
      print(ns2_0) // expected-tns-note {{access can happen concurrently}}
    } else {
      print(ns2_1) // expected-tns-note {{access can happen concurrently}}
    }
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
    await a.foo(ns0_0) // expected-tns-warning {{sending 'ns0_0' risks causing data races}}
    // expected-tns-note @-1 {{sending 'ns0_0' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}

    if b {
      print(ns0_0) // expected-tns-note {{access can happen concurrently}}
    } else {
      print(ns0_1) // expected-tns-note {{access can happen concurrently}}
    }
  } else {
    await a.foo(ns0_1) // expected-tns-warning {{sending 'ns0_1' risks causing data races}}
    // expected-tns-note @-1 {{sending 'ns0_1' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}

    if b {
      print(ns0_0) // expected-tns-note {{access can happen concurrently}}
    } else {
      print(ns0_1) // expected-tns-note {{access can happen concurrently}}
    }
  }

  if (b) {
    await a.foo(ns1_0) // expected-tns-warning {{sending 'ns1_0' risks causing data races}}
    // expected-tns-note @-1 {{sending 'ns1_0' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}

    if b {
      print(ns1_0) // expected-tns-note {{access can happen concurrently}}
    } else {
      print(ns1_1) // expected-tns-note {{access can happen concurrently}}
    }
  } else {
    await a.foo(ns1_1) // expected-tns-warning {{sending 'ns1_1' risks causing data races}}
    // expected-tns-note @-1 {{sending 'ns1_1' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}

    if b {
      print(ns1_0) // expected-tns-note {{access can happen concurrently}}
    } else {
      print(ns1_1) // expected-tns-note {{access can happen concurrently}}
    }
  }

  if (b) {
    await a.foo(ns2_0) // expected-tns-warning {{sending 'ns2_0' risks causing data races}}
    // expected-tns-note @-1 {{sending 'ns2_0' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}

    if b {
      print(ns2_0) // expected-tns-note {{access can happen concurrently}}
    } else {
      print(ns2_1) // expected-tns-note {{access can happen concurrently}}
    }
  } else {
    await a.foo(ns2_1) // expected-tns-warning {{sending 'ns2_1' risks causing data races}}
    // expected-tns-note @-1 {{sending 'ns2_1' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}

    if b {
      print(ns2_0) // expected-tns-note {{access can happen concurrently}}
    } else {
      print(ns2_1) // expected-tns-note {{access can happen concurrently}}
    }
  }

  if (b) {
    await a.foo(ns3_0) // expected-tns-warning {{sending 'ns3_0' risks causing data races}}
    // expected-tns-note @-1 {{sending 'ns3_0' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}

    if b {
      print(ns3_0) // expected-tns-note {{access can happen concurrently}}
    } else {
      print(ns3_1) // expected-tns-note {{access can happen concurrently}}
    }
  } else {
    await a.foo(ns3_1) // expected-tns-warning {{sending 'ns3_1' risks causing data races}}
    // expected-tns-note @-1 {{sending 'ns3_1' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}

    if b {
      print(ns3_0) // expected-tns-note {{access can happen concurrently}}
    } else {
      print(ns3_1) // expected-tns-note {{access can happen concurrently}}
    }
  }

  if (b) {
    await a.foo(ns4_0) // expected-tns-warning {{sending 'ns4_0' risks causing data races}}
    // expected-tns-note @-1 {{sending 'ns4_0' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}

    if b {
      print(ns4_0) // expected-tns-note {{access can happen concurrently}}
    } else {
      print(ns4_1) // expected-tns-note {{access can happen concurrently}}
    }
  } else {
    await a.foo(ns4_1) // expected-tns-warning {{sending 'ns4_1' risks causing data races}}
    // expected-tns-note @-1 {{sending 'ns4_1' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}

    if b {
      print(ns4_0) // expected-tns-note {{access can happen concurrently}}
    } else {
      print(ns4_1) // expected-tns-note {{access can happen concurrently}}
    }
  }

  if (b) {
    await a.foo(ns5_0) // expected-tns-warning {{sending 'ns5_0' risks causing data races}}
    // expected-tns-note @-1 {{sending 'ns5_0' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'Any' into actor-isolated context may introduce data races}}

    if b {
      print(ns5_0) // expected-tns-note {{access can happen concurrently}}
    } else {
      print(ns5_1) // expected-tns-note {{access can happen concurrently}}
    }
  } else {
    await a.foo(ns5_1) // expected-tns-warning {{sending 'ns5_1' risks causing data races}}
    // expected-tns-note @-1 {{sending 'ns5_1' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'Any' into actor-isolated context may introduce data races}}

    if b {
      print(ns5_0) // expected-tns-note {{access can happen concurrently}}
    } else {
      print(ns5_1) // expected-tns-note {{access can happen concurrently}}
    }
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
    await a.foo(captures_self) // expected-tns-warning {{sending 'captures_self' risks causing data races}}
    // expected-tns-note @-1 {{sending task-isolated 'captures_self' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and task-isolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type '() -> ()' into actor-isolated context may introduce data races}}
    // expected-complete-note @-3 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
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
    // expected-complete-warning @-1 {{passing argument of non-sendable type '() -> ()' into actor-isolated context may introduce data races}}
    // expected-complete-note @-2 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
  }
}

actor A_Sendable {
  func bar() {}

  func bar(a : A) async {
    let captures_self = { self.bar() }

    // This is not a cross-isolation call, so it should be permitted
    foo_noniso(captures_self)

    // This is a cross-isolation call passing a call that should be treated
    // as isolated to self since it captures self which is isolated to our
    // actor and is non-Sendable. For now, we ban this since we do not
    // support the ability to dynamically invoke the synchronous closure on
    // the specific actor.
    await a.foo(captures_self) // expected-tns-warning {{sending 'captures_self' risks causing data races}}
    // expected-tns-note @-1 {{sending 'self'-isolated 'captures_self' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and 'self'-isolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type '() -> ()' into actor-isolated context may introduce data races}}
    // expected-complete-note @-3 {{a function type must be marked '@Sendable' to conform to 'Sendable'}}
  }
}

func basic_loopiness(a : A, b : Bool) async {
  let ns = NonSendable()

  while (b) {
    await a.foo(ns) // expected-tns-warning {{sending 'ns' risks causing data races}}
    // expected-tns-note @-1 {{sending 'ns' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and local nonisolated uses}}
    // expected-tns-note @-2 {{access can happen concurrently}}
    // expected-complete-warning @-3 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}
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

  await a.foo(ns0) // expected-tns-warning {{sending 'ns0' risks causing data races}}
  // expected-tns-note @-1 {{sending 'ns0' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}
  await a.foo(ns3) // expected-tns-note {{access can happen concurrently}}
  // expected-complete-warning @-1 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}
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
  // expected-complete-warning @-1 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}
  await a.foo(ns3)
  // expected-complete-warning @-1 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}
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
  // expected-complete-warning @-1 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}
  await a.foo(ns1)
  // expected-complete-warning @-1 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}
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

  // This should merge ns0 and ns1.
  var box = ClassBox()
  box = ClassBox()
  box.contents = ns0
  box.contents = ns1

  await a.foo(ns0) // expected-tns-warning {{sending 'ns0' risks causing data races}}
  // expected-tns-note @-1 {{sending 'ns0' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}
  await a.foo(ns1) // expected-tns-note {{access can happen concurrently}}
  // expected-complete-warning @-1 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}
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
  // expected-complete-warning @-1 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}
  await a.foo(ns1)
  // expected-complete-warning @-1 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}
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

  await a.foo(ns0) // expected-tns-warning {{sending 'ns0' risks causing data races}}
  // expected-tns-note @-1 {{sending 'ns0' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}
  await a.foo(ns1) // expected-tns-note {{access can happen concurrently}}
  // expected-complete-warning @-1 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}
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
    await a.foo(ns0) // expected-tns-warning {{sending 'ns0' risks causing data races}}
    // expected-tns-note @-1 {{sending 'ns0' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}

    if bool {
      foo_noniso(ns0); // expected-tns-note {{access can happen concurrently}}
    } else if bool {
      foo_noniso(ns1); // expected-tns-note {{access can happen concurrently}}
    } else if bool {
      foo_noniso(ns2); // expected-tns-note {{access can happen concurrently}}
    } else if bool {
      foo_noniso(ns3); // expected-tns-note {{access can happen concurrently}}
    } else if bool {
      foo_noniso(ns4);
    } else if bool {
      foo_noniso(ns012); // expected-tns-note {{access can happen concurrently}}
    } else {
      foo_noniso(ns13); // expected-tns-note {{access can happen concurrently}}
    }
  case 1:
    await a.foo(ns1) // expected-tns-warning {{sending 'ns1' risks causing data races}}
    // expected-tns-note @-1 {{sending 'ns1' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}

    if bool {
      foo_noniso(ns0); // expected-tns-note {{access can happen concurrently}}
    } else if bool {
      foo_noniso(ns1); // expected-tns-note {{access can happen concurrently}}
    } else if bool {
      foo_noniso(ns2); // expected-tns-note {{access can happen concurrently}}
    } else if bool {
      foo_noniso(ns3); // expected-tns-note {{access can happen concurrently}}
    } else if bool {
      foo_noniso(ns4);
    } else if bool {
      foo_noniso(ns012); // expected-tns-note {{access can happen concurrently}}
    } else {
      foo_noniso(ns13); // expected-tns-note {{access can happen concurrently}}
    }
  case 2:
    await a.foo(ns2) // expected-tns-warning {{sending 'ns2' risks causing data races}}
    // expected-tns-note @-1 {{sending 'ns2' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}

    if bool {
      foo_noniso(ns0); // expected-tns-note {{access can happen concurrently}}
    } else if bool {
      foo_noniso(ns1); // expected-tns-note {{access can happen concurrently}}
    } else if bool {
      foo_noniso(ns2); // expected-tns-note {{access can happen concurrently}}
    } else if bool {
      foo_noniso(ns3); // expected-tns-note {{access can happen concurrently}}
    } else if bool {
      foo_noniso(ns4);
    } else if bool {
      foo_noniso(ns012); // expected-tns-note {{access can happen concurrently}}
    } else {
      foo_noniso(ns13); // expected-tns-note {{access can happen concurrently}}
    }
  case 3:
    await a.foo(ns4) // expected-tns-warning {{sending 'ns4' risks causing data races}}
    // expected-tns-note @-1 {{sending 'ns4' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and local nonisolated uses}}
    // expected-complete-warning @-2 {{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}

    foo_noniso(ns0);
    foo_noniso(ns1);
    foo_noniso(ns2);
    foo_noniso(ns4); // expected-tns-note {{access can happen concurrently}}
    foo_noniso(ns012);
    foo_noniso(ns13);
  case 4:
    await a.foo(ns012) // expected-tns-warning {{sending value of non-Sendable type '(NonSendable, NonSendable, NonSendable)' with later accesses from nonisolated context to actor-isolated context risks causing data races}}
    // TODO: Interestingly with complete, we emit this error 3 times?!
    // expected-complete-warning @-2 3{{passing argument of non-sendable type '(NonSendable, NonSendable, NonSendable)' into actor-isolated context may introduce data races}}

    if bool {
      foo_noniso(ns0); // expected-tns-note {{access can happen concurrently}}
    } else if bool {
      foo_noniso(ns1); // expected-tns-note {{access can happen concurrently}}
    } else if bool {
      foo_noniso(ns2); // expected-tns-note {{access can happen concurrently}}
    } else if bool {
      foo_noniso(ns3); // expected-tns-note {{access can happen concurrently}}
    } else if bool {
      foo_noniso(ns4);
    } else if bool {
      foo_noniso(ns012); // expected-tns-note {{access can happen concurrently}}
    } else {
      foo_noniso(ns13); // expected-tns-note {{access can happen concurrently}}
    }
  default:
    await a.foo(ns13) // expected-tns-warning {{sending value of non-Sendable type '(NonSendable, NonSendable)' with later accesses from nonisolated context to actor-isolated context risks causing data races}}
    // expected-complete-warning @-1 2{{passing argument of non-sendable type '(NonSendable, NonSendable)' into actor-isolated context may introduce data races}}

    if bool {
      foo_noniso(ns0); // expected-tns-note {{access can happen concurrently}}
    } else if bool {
      foo_noniso(ns1); // expected-tns-note {{access can happen concurrently}}
    } else if bool {
      foo_noniso(ns2); // expected-tns-note {{access can happen concurrently}}
    } else if bool {
      foo_noniso(ns3); // expected-tns-note {{access can happen concurrently}}
    } else if bool {
      foo_noniso(ns4);
    } else if bool {
      foo_noniso(ns012); // expected-tns-note {{access can happen concurrently}}
    } else  {
      foo_noniso(ns13); // expected-tns-note {{access can happen concurrently}}
    }
  }
}

func reuse_args_safe(a : A) async {
  let ns0 = NonSendable();
  let ns1 = NonSendable();
  let ns2 = NonSendable();

  // this should be perfectly legal - arguments are assumed to come from the same region
  await a.foo_multi(ns0, ns0, ns0);
  // expected-complete-warning @-1 3{{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}
  await a.foo_multi(ns1, ns2, ns1);
  // expected-complete-warning @-1 3{{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}
}

func one_consume_many_require(a : A) async {
  let ns0 = NonSendable();
  let ns1 = NonSendable();
  let ns2 = NonSendable();
  let ns3 = NonSendable();
  let ns4 = NonSendable();

  await a.foo_multi(ns0, ns1, ns2);
  // expected-complete-warning @-1 3{{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}
  // expected-tns-warning @-2 {{sending 'ns0' risks causing data races}}
  // expected-tns-note @-3 {{sending 'ns0' to actor-isolated instance method 'foo_multi' risks causing data races between actor-isolated and local nonisolated uses}}
  // expected-tns-warning @-4 {{sending 'ns1' risks causing data races}}
  // expected-tns-note @-5 {{sending 'ns1' to actor-isolated instance method 'foo_multi' risks causing data races between actor-isolated and local nonisolated uses}}
  // expected-tns-warning @-6 {{sending 'ns2' risks causing data races}}
  // expected-tns-note @-7 {{sending 'ns2' to actor-isolated instance method 'foo_multi' risks causing data races between actor-isolated and local nonisolated uses}}

  foo_noniso_multi(ns0, ns3, ns4); // expected-tns-note {{access can happen concurrently}}
  foo_noniso_multi(ns3, ns1, ns4); // expected-tns-note {{access can happen concurrently}}
  foo_noniso_multi(ns4, ns3, ns2); // expected-tns-note {{access can happen concurrently}}
}

func one_consume_one_require(a : A) async {
  let ns0 = NonSendable();
  let ns1 = NonSendable();
  let ns2 = NonSendable();

  await a.foo_multi(ns0, ns1, ns2);
  // expected-complete-warning @-1 3{{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}
  // expected-tns-warning @-2 {{sending 'ns0' risks causing data races}}
  // expected-tns-note @-3 {{sending 'ns0' to actor-isolated instance method 'foo_multi' risks causing data races between actor-isolated and local nonisolated uses}}
  // expected-tns-warning @-4 {{sending 'ns1' risks causing data races}}
  // expected-tns-note @-5 {{sending 'ns1' to actor-isolated instance method 'foo_multi' risks causing data races between actor-isolated and local nonisolated uses}}
  // expected-tns-warning @-6 {{sending 'ns2' risks causing data races}}
  // expected-tns-note @-7 {{sending 'ns2' to actor-isolated instance method 'foo_multi' risks causing data races between actor-isolated and local nonisolated uses}}

  foo_noniso_multi(ns0, ns1, ns2); // expected-tns-note 3{{access can happen concurrently}}
}

func many_consume_one_require(a : A) async {
  let ns0 = NonSendable();
  let ns1 = NonSendable();
  let ns2 = NonSendable();
  let ns3 = NonSendable();
  let ns4 = NonSendable();
  let ns5 = NonSendable();

  await a.foo_multi(ns0, ns3, ns3) // expected-tns-warning {{sending 'ns0' risks causing data races}}
  // expected-tns-note @-1 {{sending 'ns0' to actor-isolated instance method 'foo_multi' risks causing data races between actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 3{{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}
  await a.foo_multi(ns4, ns1, ns4) // expected-tns-warning {{sending 'ns1' risks causing data races}}
  // expected-tns-note @-1 {{sending 'ns1' to actor-isolated instance method 'foo_multi' risks causing data races between actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 3{{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}
  await a.foo_multi(ns5, ns5, ns2) // expected-tns-warning {{sending 'ns2' risks causing data races}}
  // expected-tns-note @-1 {{sending 'ns2' to actor-isolated instance method 'foo_multi' risks causing data races between actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 3{{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}
  foo_noniso_multi(ns0, ns1, ns2); //expected-tns-note 3{{access can happen concurrently}}
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

  await a.foo_multi(ns0, ns3, ns3) // expected-tns-warning {{sending 'ns0' risks causing data races}}
  // expected-tns-note @-1 {{sending 'ns0' to actor-isolated instance method 'foo_multi' risks causing data races between actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 3{{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}
  await a.foo_multi(ns4, ns1, ns4) // expected-tns-warning {{sending 'ns1' risks causing data races}}
  // expected-tns-note @-1 {{sending 'ns1' to actor-isolated instance method 'foo_multi' risks causing data races between actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 3{{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}
  await a.foo_multi(ns5, ns5, ns2) // expected-tns-warning {{sending 'ns2' risks causing data races}}
  // expected-tns-note @-1 {{sending 'ns2' to actor-isolated instance method 'foo_multi' risks causing data races between actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 3{{passing argument of non-sendable type 'NonSendable' into actor-isolated context may introduce data races}}

  foo_noniso_multi(ns0, ns6, ns7); // expected-tns-note {{access can happen concurrently}}
  foo_noniso_multi(ns6, ns1, ns7); // expected-tns-note {{access can happen concurrently}}
  foo_noniso_multi(ns7, ns6, ns2); // expected-tns-note {{access can happen concurrently}}
}


func reuse_args_safe_vararg(a : A) async {
  let ns0 = NonSendable();
  let ns1 = NonSendable();
  let ns2 = NonSendable();

  // this should be perfectly legal - arguments are assumed to come from the same region
  await a.foo_vararg(ns0, ns0, ns0);
  // expected-complete-warning @-1 {{passing argument of non-sendable type 'Any...' into actor-isolated context may introduce data races}}
  await a.foo_vararg(ns1, ns2, ns1);
  // expected-complete-warning @-1 {{passing argument of non-sendable type 'Any...' into actor-isolated context may introduce data races}}
}

func one_consume_many_require_varag(a : A) async {
  let ns0 = NonSendable();
  let ns1 = NonSendable();
  let ns2 = NonSendable();
  let ns3 = NonSendable();
  let ns4 = NonSendable();

  //TODO: find a way to make the type used in the diagnostic more specific than the signature type
  await a.foo_vararg(ns0, ns1, ns2);
  // expected-tns-warning @-1 {{sending value of non-Sendable type 'Any...' with later accesses from nonisolated context to actor-isolated context risks causing data races}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'Any...' into actor-isolated context may introduce data races}}

  if bool {
    foo_noniso_vararg(ns0, ns3, ns4); // expected-tns-note {{access can happen concurrently}}
  } else if bool {
    foo_noniso_vararg(ns3, ns1, ns4); // expected-tns-note {{access can happen concurrently}}
  } else {
    foo_noniso_vararg(ns4, ns3, ns2); // expected-tns-note {{access can happen concurrently}}
  }
}

func one_consume_one_require_vararg(a : A) async {
  let ns0 = NonSendable();
  let ns1 = NonSendable();
  let ns2 = NonSendable();

  await a.foo_vararg(ns0, ns1, ns2);
  // expected-tns-warning @-1 {{sending value of non-Sendable type 'Any...' with later accesses from nonisolated context to actor-isolated context risks causing data races}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'Any...' into actor-isolated context may introduce data races}}

  foo_noniso_vararg(ns0, ns1, ns2); // expected-tns-note 1{{access can happen concurrently}}
}

func many_consume_one_require_vararg(a : A) async {
  let ns0 = NonSendable();
  let ns1 = NonSendable();
  let ns2 = NonSendable();
  let ns3 = NonSendable();
  let ns4 = NonSendable();
  let ns5 = NonSendable();

  await a.foo_vararg(ns0, ns3, ns3)
  // expected-tns-warning @-1 {{sending value of non-Sendable type 'Any...' with later accesses from nonisolated context to actor-isolated context risks causing data races}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'Any...' into actor-isolated context may introduce data races}}
  await a.foo_vararg(ns4, ns1, ns4)
  // expected-tns-warning @-1 {{sending value of non-Sendable type 'Any...' with later accesses from nonisolated context to actor-isolated context risks causing data races}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'Any...' into actor-isolated context may introduce data races}}
  await a.foo_vararg(ns5, ns5, ns2)
  // expected-tns-warning @-1 {{sending value of non-Sendable type 'Any...' with later accesses from nonisolated context to actor-isolated context risks causing data races}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'Any...' into actor-isolated context may introduce data races}}

  foo_noniso_vararg(ns0, ns1, ns2); // expected-tns-note 3{{access can happen concurrently}}
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
  // expected-tns-warning @-1 {{sending value of non-Sendable type 'Any...' with later accesses from nonisolated context to actor-isolated context risks causing data races}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'Any...' into actor-isolated context may introduce data races}}
  await a.foo_vararg(ns4, ns1, ns4)
  // expected-tns-warning @-1 {{sending value of non-Sendable type 'Any...' with later accesses from nonisolated context to actor-isolated context risks causing data races}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'Any...' into actor-isolated context may introduce data races}}
  await a.foo_vararg(ns5, ns5, ns2)
  // expected-tns-warning @-1 {{sending value of non-Sendable type 'Any...' with later accesses from nonisolated context to actor-isolated context risks causing data races}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'Any...' into actor-isolated context may introduce data races}}

  if bool {
    foo_noniso_vararg(ns0, ns6, ns7); // expected-tns-note {{access can happen concurrently}}
  } else if bool {
    foo_noniso_vararg(ns6, ns1, ns7);  // expected-tns-note {{access can happen concurrently}}
  } else {
    foo_noniso_vararg(ns7, ns6, ns2);  // expected-tns-note {{access can happen concurrently}}
  }
}

enum E { // expected-complete-note {{consider making enum 'E' conform to the 'Sendable' protocol}}
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
        await a.foo(ns3.x); // expected-tns-warning {{sending value of non-Sendable type 'Any' with later accesses from nonisolated context to actor-isolated context risks causing data races}}
        // expected-complete-warning @-1 {{passing argument of non-sendable type 'Any' into actor-isolated context may introduce data races}}
      default: ()
      }

      //this case does nothing
      case .E3:
        foo_noniso(e4);
  }

  await a.foo(e1); // expected-tns-warning {{sending 'e1' risks causing data races}}
  // expected-tns-note @-1 {{sending 'e1' to actor-isolated instance method 'foo' risks causing data races between actor-isolated and local nonisolated uses}}
  // expected-complete-warning @-2 {{passing argument of non-sendable type 'E' into actor-isolated context may introduce data races}}
  foo_noniso(e2); // expected-tns-note {{access can happen concurrently}}
  foo_noniso(e3); // expected-tns-note {{access can happen concurrently}}
  foo_noniso(e4);
}
