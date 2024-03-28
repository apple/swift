// RUN: %target-typecheck-verify-swift -warn-concurrency -disable-availability-checking
// REQUIRES: concurrency
// REQUIRES: distributed

import Distributed


// This system does not exist: but we should not crash, but just diagnose about it:
typealias DefaultDistributedActorSystem = ClusterSystem // expected-error{{cannot find type 'ClusterSystem' in scope}}

distributed actor MyActor {
  // expected-error@-1{{distributed actor 'MyActor' does not declare ActorSystem it can be used with.}}
  // expected-note@-2{{you can provide a module-wide default actor system by declaring:}}

  distributed var foo: String {
    return "xyz"
  }
}

distributed actor BadSystemActor {
  // expected-error@-1{{distributed actor 'BadSystemActor' does not declare ActorSystem it can be used with.}}
  // expected-note@-2{{you can provide a module-wide default actor system by declaring:}}

  // This system does not exist: but we should not crash, but just diagnose about it:
  typealias ActorSystem = ClusterSystem // expected-error{{cannot find type 'ClusterSystem' in scope}}
}
