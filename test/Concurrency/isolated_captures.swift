// RUN: %target-swift-frontend -verify -disable-availability-checking -strict-concurrency=complete -verify-additional-prefix region-isolation- -emit-sil -o /dev/null %s

// REQUIRES: concurrency
// REQUIRES: asserts

@globalActor
actor MyActor {
  static let shared = MyActor()
  @MyActor static var ns: NotSendable?
  @MyActor static func ohNo() { ns!.x += 1 }
}

@globalActor
actor YourActor {
  static let shared = YourActor()
  @YourActor static var ns: NotSendable?
  @YourActor static func ohNo() { ns!.x += 1 }
}

// expected-complete-note@+1 3{{class 'NotSendable' does not conform to the 'Sendable' protocol}}
class NotSendable {
  var x: Int = 0

  @MyActor init() {
    MyActor.ns = self
  }

  init(x: Int) {
    self.x = x
  }

  @MyActor func stash() {
    MyActor.ns = self
  }
}

@MyActor func exhibitRace1() async {
  let ns = NotSendable(x: 0)
  MyActor.ns = ns

  await { @YourActor in
    // expected-region-isolation-warning @+3 {{sending 'ns' risks causing data races}}
    // expected-region-isolation-note @+2 {{global actor 'MyActor'-isolated 'ns' is captured by a global actor 'YourActor'-isolated closure. global actor 'YourActor'-isolated uses in closure may race against later global actor 'MyActor'-isolated uses}}
    // expected-complete-warning@+1 {{capture of 'ns' with non-sendable type 'NotSendable' in an isolated closure; this is an error in the Swift 6 language mode}}
    YourActor.ns = ns
  }()

  await withTaskGroup(of: Void.self) {
    $0.addTask {
      await MyActor.ohNo()
    }

    $0.addTask {
      await YourActor.ohNo()
    }
  }
}

@MyActor func exhibitRace2() async {
  let ns = NotSendable(x: 0)
  ns.stash()

  await { @YourActor in
    // expected-region-isolation-warning @+3 {{sending 'ns' risks causing data races}}
    // expected-region-isolation-note @+2 {{global actor 'MyActor'-isolated 'ns' is captured by a global actor 'YourActor'-isolated closure. global actor 'YourActor'-isolated uses in closure may race against later global actor 'MyActor'-isolated uses}}
    // expected-complete-warning@+1 {{capture of 'ns' with non-sendable type 'NotSendable' in an isolated closure; this is an error in the Swift 6 language mode}}
    YourActor.ns = ns
  }()

  await withTaskGroup(of: Void.self) {
    $0.addTask {
      await MyActor.ohNo()
    }

    $0.addTask {
      await YourActor.ohNo()
    }
  }
}

@MyActor func exhibitRace3() async {
  let ns = NotSendable()

  await { @YourActor in
    // expected-region-isolation-warning @+3 {{sending 'ns' risks causing data races}}
    // expected-region-isolation-note @+2 {{global actor 'MyActor'-isolated 'ns' is captured by a global actor 'YourActor'-isolated closure. global actor 'YourActor'-isolated uses in closure may race against later global actor 'MyActor'-isolated uses}}
    // expected-complete-warning@+1 {{capture of 'ns' with non-sendable type 'NotSendable' in an isolated closure; this is an error in the Swift 6 language mode}}
    YourActor.ns = ns
  }()

  await withTaskGroup(of: Void.self) {
    $0.addTask {
      await MyActor.ohNo()
    }

    $0.addTask {
      await YourActor.ohNo()
    }
  }
}
