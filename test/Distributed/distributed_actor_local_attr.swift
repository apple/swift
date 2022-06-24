// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend-emit-module -emit-module-path %t/FakeDistributedActorSystems.swiftmodule -module-name FakeDistributedActorSystems -disable-availability-checking %S/Inputs/FakeDistributedActorSystems.swift
// RUN: %target-swift-frontend -typecheck -verify -verify-ignore-unknown -disable-availability-checking -I %t 2>&1 %s
// REQUIRES: concurrency
// REQUIRES: distributed

// TODO(distributed): rdar://82419661 remove -verify-ignore-unknown here, no warnings should be emitted for our
//  generated code but right now a few are, because of Sendability checks -- need to track it down more.

import Distributed
import FakeDistributedActorSystems

@available(SwiftStdlib 5.5, *)
typealias DefaultDistributedActorSystem = FakeActorSystem

// ==== ----------------------------------------------------------------------------------------------------------------

distributed actor Capybara {
  let name = "Caplin"
  let surname = "The Capybara"

  distributed func echo(_ param: String) -> String {
    param
  }

  func localFunc(_ : NotCodable) -> String {
    "localFunc"
  }
}

struct NotCodable {}

func test(param: _local Capybara) { // expected-error 2{{'_local' cannot be used in this position, only usable by a DistributedActor's 'whenLocal' method}}
}

actor Named {
  let name: String = ""
}

// TODO(distributed): immutable property access is not possible yet need to fix accessible checks
//func ok_sync(capybara: Capybara) async throws {
//  let n = Named()
//  _ = n.name // ok
//
//  let _: String? = capybara.whenLocal { loc in
//    loc.name // ok!
//  }
//}

func ok_async(capybara: Capybara) async throws {
  let value: String? = await capybara.whenLocal { loc in
    await loc.localFunc(.init()) // ok!
  }
  _ = value
}

actor A {}
func test(a: _local A) async throws { // expected-error 2{{'_local' cannot be used in this position, only usable by a DistributedActor's 'whenLocal' method}}
  // expected-error@-1{{'_local' parameter has non-distributed-actor type 'A'}}
}

class C {}
func test(a: _local C) async throws { // expected-error 2{{'_local' cannot be used in this position, only usable by a DistributedActor's 'whenLocal' method}}
  // expected-error@-1{{'_local' parameter has non-distributed-actor type 'C'}}
}

struct S {}
func test(a: _local S) async throws { // expected-error 2{{'_local' cannot be used in this position, only usable by a DistributedActor's 'whenLocal' method}}
  // expected-error@-1{{'_local' parameter has non-distributed-actor type 'S'}}
}
