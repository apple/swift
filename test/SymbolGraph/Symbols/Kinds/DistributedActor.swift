// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -module-name DistributedActor -emit-module -emit-module-path %t/
// RUN: %target-swift-symbolgraph-extract %windows_vfs_overlay_opt -module-name DistributedActor -I %t -pretty-print -output-dir %t
// RUN: %FileCheck %s --input-file %t/DistributedActor.symbols.json

// REQUIRES: concurrency
// REQUIRES: distributed
import Distributed

@available(macOS 13.0, iOS 16.0, watchOS 9.0, tvOS 16.0, *)
public distributed actor DA {
  public typealias ActorSystem = LocalTestingDistributedActorSystem
}

// CHECK: "identifier": "swift.class"
// CHECK-NEXT: "displayName": "Class"
// CHECK: pathComponents
// CHECK-NEXT: "DA"
// CHECK: "kind": "keyword"
// CHECK-NEXT: "spelling": "distributed actor"

