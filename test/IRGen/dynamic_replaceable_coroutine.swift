// RUN: %target-swift-frontend -module-name A -enable-implicit-dynamic -emit-ir %s | %FileCheck %s


extension Int {
  public struct Thing {
    var _int: Int
    init(_ int: Int) {
      self._int = int
    }
  }

  public var thing: Thing {
    get { Thing(self) }
    // Make sure the initialization of `thing` is after the dynamic replacement
    // check. Coro splitting does not like memsets before the coro.begin.

    // CHECK: define{{.*}} swiftcc { ptr, ptr } @"$sSi1AE5thingSiAAE5ThingVvM"
    // CHECK: call ptr @swift_getFunctionReplacement
    // CHECK: br
    // CHECK: AllocaSpillBB:
    // CHECK: [[THING:%.*]] = getelementptr inbounds %"$sSi1AE5thingSiAAE5ThingVvM.Frame", ptr %0, i32 0
    // CHECK: call void @llvm.memset{{.*}}(ptr {{.*}} [[THING]]
    // CHECK: ret
    _modify {
      var thing = Thing(self)
      yield &thing
    }
  }
}
