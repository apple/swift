// RUN: %target-swift-emit-silgen %s | %FileCheck %s
// SR-8398
func fibonacci(_ n: Int) -> Int {
  var cache: [Int: Int] = [:]

  func recursive(_ m: Int) -> Int {
    return cache[m] ?? {
      // Make sure cache is only captured once in the closure
      // CHECK: implicit closure #1 in recursive #1
      // CHECK-LABEL: sil private [transparent] [ossa] @{{.*}}9fibonacci{{.*}}9recursive{{.*}} : $@convention(thin) @async (Int, @guaranteed { var Dictionary<Int, Int> }) -> (Int, @error Error)
      // CHECK: closure #1 in implicit closure #1 in recursive #1
      // CHECK-LABEL: sil private [ossa] @{{.*}}9fibonacci{{.*}}9recursive{{.*}} : $@convention(thin) (Int, @guaranteed { var Dictionary<Int, Int> }) -> Int
      let output = m < 2 ? m : recursive(m - 1) + recursive(m - 2)
      cache[m] = output
      return output
      }()
  }
  return recursive(n)
}

class C {
    required init() {}
    func f() {}
    // Make sure that we capture dynamic self type if an explicit self isn't guaranteed
    func returnsSelf() -> Self {
        return { self.f(); return .init() }()
        // CHECK-LABEL: sil private [ossa] @{{.*}}returnsSelf{{.*}} : $@convention(thin) (@guaranteed C) -> @owned C
    }

    func returnsSelf1() -> Self {
        return { [weak self] in self?.f(); return .init() }()
        // CHECK-LABEL: sil private [ossa] @{{.*}}returnsSelf{{.*}}  : $@convention(thin) (@guaranteed { var @sil_weak Optional<C> }, @thick @dynamic_self C.Type) -> @owned C
    }

    func returnsSelf2() -> Self {
        return { [unowned self] in self.f(); return .init() }()
        // CHECK-LABEL: sil private [ossa] @{{.*}}returnsSelf{{.*}}  : $@convention(thin) (@guaranteed @sil_unowned C, @thick @dynamic_self C.Type) -> @owned C
    }
}


