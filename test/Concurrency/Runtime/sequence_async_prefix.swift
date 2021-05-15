// RUN: %target-run-simple-swift(-parse-as-library -Xfrontend -enable-experimental-concurrency)

// REQUIRES: executable_test
// REQUIRES: concurrency

import Swift
import _Concurrency

@main struct Main {
  static func main() async {
    var called = 0

    @concurrent
    func predicate(_ x: Int) -> Bool {
      x < 3
    }

    func predicateAsync(_ x: Int) async -> Bool {
      let handle: Task.Handle<Bool, Error> = Task.runDetached {
        predicate(x)
      }
      called += 1

      return try! await handle.get()
    }

    // workaround for rdar://74289867
    func syncFilterWorkaround(_ xs: [Int]) -> [Int] {
      xs.prefix(while: predicate)
    }

    let xs = [1,2,3,4,5]
    let resSync = syncFilterWorkaround(xs)
    let resAsync = await xs.prefix(while: predicateAsync)

    assert(resAsync == resSync)
    assert(called == 3)
  }
}
