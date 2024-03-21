// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -typecheck -parse-as-library -verify -disable-availability-checking 2>&1 %s

// REQUIRES: concurrency
// REQUIRES: distributed

import Distributed

@available(SwiftStdlib 6.0, *)
// expected-error@+2{{distributed actor 'DAM' cannot conform to 'DM' because of serialization requirement error in distributed instance method 'accept'}}
// expected-error@+1{{distributed actor 'DAM' cannot conform to 'DM' because of serialization requirement error in distributed instance method 'fetch()'}}
distributed actor DAM: DM {
  typealias ActorSystem = LocalTestingDistributedActorSystem
}

// MARK: - DM

struct Nein {}

@available(SwiftStdlib 6.0, *)
protocol DM: DistributedActor {
  // The protocol imposes no requirements, however when an actor conforms to
  // this protocol, and specifies a system or requirement, we'll check against
  // the DA's requirement.

  // expected-error@+1{{result type 'Nein' of distributed instance method 'fetch' does not conform to serialization requirement 'Codable'}}
  distributed func fetch() async -> Nein

  // expected-error@+1{{parameter '' of type 'Nein' in distributed instance method does not conform to serialization requirement 'Codable'}}
  distributed func accept(_: Nein) async
}

// MARK: - DM default implementation

@available(SwiftStdlib 6.0, *)
extension DM {
  distributed func fetch() async -> Nein {
    .init()
  }

  distributed func accept(_: Nein) async {
  }
}

