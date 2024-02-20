// REQUIRES: swift_swift_parser, asserts
//
// UNSUPPORTED: back_deploy_concurrency
// REQUIRES: concurrency
// REQUIRES: distributed
//
// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t-scratch)

// RUN: %target-swift-frontend -typecheck -verify -disable-availability-checking -plugin-path %swift-plugin-dir -I %t -dump-macro-expansions %s 2>&1 | %FileCheck %s

import Distributed

typealias System = LocalTestingDistributedActorSystem

@_DistributedProtocol
protocol G0<ActorSystem>: DistributedActor where ActorSystem: DistributedActorSystem<any Codable> {
  distributed func get() -> String
}

// @_DistributedProtocol ->

// CHECK: distributed actor $G0<ActorSystem>: G0,
// CHECK:   Distributed._DistributedActorStub
// CHECK:   where ActorSystem: DistributedActorSystem<any Codable>,
// CHECK:         ActorSystem.ActorID: Codable
// CHECK: {
// CHECK:   distributed func get() -> String {
// CHECK:    if #available (SwiftStdlib 5.11, *) {
// CHECK:      Distributed._distributedStubFatalError()
// CHECK:    } else {
// CHECK:      fatalError("distributed method stud: \(#function)")
// CHECK:    }
// CHECK:  }
// CHECK: }

// CHECK: extension G0 where Self: Distributed._DistributedActorStub {
// CHECK:   distributed func get() -> String {
// CHECK:     if #available (SwiftStdlib 5.11, *) {
// CHECK:       Distributed._distributedStubFatalError()
// CHECK:     } else {
// CHECK:       fatalError("distributed method stud: \(#function)")
// CHECK:     }
// CHECK:   }
// CHECK: }

@_DistributedProtocol
public protocol G1<ActorSystem>: DistributedActor where ActorSystem: DistributedActorSystem<any Codable> {
  distributed func get() -> String
}

// @_DistributedProtocol ->

// CHECK: public distributed actor $G1<ActorSystem>: G1,
// CHECK:   Distributed._DistributedActorStub
// CHECK:   where ActorSystem: DistributedActorSystem<any Codable>,
// CHECK:         ActorSystem.ActorID: Codable
// CHECK: {
// CHECK:   public distributed func get() -> String {
// CHECK:     if #available (SwiftStdlib 5.11, *) {
// CHECK:       Distributed._distributedStubFatalError()
// CHECK:     } else {
// CHECK:       fatalError("distributed method stud: \(#function)")
// CHECK:     }
// CHECK:   }
// CHECK: }

// CHECK: extension G1 where Self: Distributed._DistributedActorStub {
// CHECK:   public distributed func get() -> String {
// CHECK:     if #available (SwiftStdlib 5.11, *) {
// CHECK:       Distributed._distributedStubFatalError()
// CHECK:     } else {
// CHECK:       fatalError("distributed method stud: \(#function)")
// CHECK:     }
// CHECK:   }
// CHECK: }


@_DistributedProtocol
protocol Base<ActorSystem>: DistributedActor where ActorSystem: DistributedActorSystem<any Codable> {
  distributed func base() -> Int
  func notDistributed() -> Int
}
// @_DistributedProtocol ->

// CHECK: distributed actor $Base<ActorSystem>: Base,
// CHECK:   Distributed._DistributedActorStub
// CHECK:   where ActorSystem: DistributedActorSystem<any Codable>,
// CHECK:         ActorSystem.ActorID: Codable
// CHECK: {
// CHECK:   distributed func base() -> Int {
// CHECK:    if #available (SwiftStdlib 5.11, *) {
// CHECK:      Distributed._distributedStubFatalError()
// CHECK:    } else {
// CHECK:      fatalError("distributed method stud: \(#function)")
// CHECK:    }
//
// CHECK:   func notDistributed() -> Int {
// CHECK:    if #available (SwiftStdlib 5.11, *) {
// CHECK:      Distributed._distributedStubFatalError()
// CHECK:    } else {
// CHECK:      fatalError("distributed method stud: \(#function)")
// CHECK:     }
// CHECK:   }
// CHECK: }

// CHECK: extension Base where Self: Distributed._DistributedActorStub {
// CHECK:   distributed func base() -> Int {
// CHECK:     if #available (SwiftStdlib 5.11, *) {
// CHECK:       Distributed._distributedStubFatalError()
// CHECK:     } else {
// CHECK:       fatalError("distributed method stud: \(#function)")
// CHECK:     }
// CHECK:   }
//
// CHECK:  func notDistributed() -> Int {
// CHECK:    if #available (SwiftStdlib 5.11, *) {
// CHECK:      Distributed._distributedStubFatalError()
// CHECK:    } else {
// CHECK:      fatalError("distributed method stud: \(#function)")
// CHECK:    }
// CHECK:  }
// CHECK: }

@_DistributedProtocol
protocol G3<ActorSystem>: Base, DistributedActor where ActorSystem: DistributedActorSystem<any Codable> {
  distributed func get() -> String
  distributed func another(param: Int) -> Int
}

// @_DistributedProtocol ->

// CHECK: distributed actor $G3<ActorSystem>: G3,
// CHECK:   Distributed._DistributedActorStub
// CHECK:   where ActorSystem: DistributedActorSystem<any Codable>,
// CHECK:         ActorSystem.ActorID: Codable
// CHECK: {
// CHECK:   distributed func get() -> String {
// CHECK:     if #available (SwiftStdlib 5.11, *) {
// CHECK:       Distributed._distributedStubFatalError()
// CHECK:     } else {
// CHECK:       fatalError("distributed method stud: \(#function)")
// CHECK:     }
// CHECK:   }
// CHECK:   distributed func another(param: Int) -> Int {
// CHECK:     if #available (SwiftStdlib 5.11, *) {
// CHECK:       Distributed._distributedStubFatalError()
// CHECK:     } else {
// CHECK:       fatalError("distributed method stud: \(#function)")
// CHECK:     }
// CHECK:   }
// CHECK: }

// CHECK: extension G3 where Self: Distributed._DistributedActorStub {
// CHECK:   distributed func get() -> String {
// CHECK:     if #available (SwiftStdlib 5.11, *) {
// CHECK:       Distributed._distributedStubFatalError()
// CHECK:     } else {
// CHECK:       fatalError("distributed method stud: \(#function)")
// CHECK:     }
// CHECK:   }
// CHECK:   distributed func another(param: Int) -> Int {
// CHECK:     if #available (SwiftStdlib 5.11, *) {
// CHECK:       Distributed._distributedStubFatalError()
// CHECK:     } else {
// CHECK:       fatalError("distributed method stud: \(#function)")
// CHECK:     }
// CHECK:   }
// CHECK: }

