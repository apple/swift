// RUN: %target-build-swift -swift-version 5 %s -strict-concurrency=complete -Xfrontend -verify

// REQUIRES: concurrency
// REQUIRES: OS=macosx

class NonSendableKlass {} // expected-note 3{{class 'NonSendableKlass' does not conform to the 'Sendable' protocol}}

@available(macOS 10.15, *)
actor MyActor {
  var x = NonSendableKlass()

  nonisolated func doSomething() -> NonSendableKlass {
    return self.assumeIsolated { isolatedSelf in // expected-warning {{type 'NonSendableKlass' does not conform to the 'Sendable' protocol}}
      let x: NonSendableKlass = isolatedSelf.x
      return x
    }
  }

  nonisolated func doSomething2() -> NonSendableKlass {
    let r: NonSendableKlass = assumeIsolated { isolatedSelf in // expected-warning {{type 'NonSendableKlass' does not conform to the 'Sendable' protocol}}
      let x: NonSendableKlass = isolatedSelf.x
      return x
    }
    return r
  }
}

@available(macOS 10.15, *)
nonisolated func mainActorAssumeIsolated() -> NonSendableKlass {
  return MainActor.assumeIsolated { // expected-warning {{type 'NonSendableKlass' does not conform to the 'Sendable' protocol}}
    NonSendableKlass()
  }
}
