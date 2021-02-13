// RUN: %target-run-simple-swift(-parse-as-library -Xfrontend -enable-experimental-concurrency)

// REQUIRES: executable_test
// REQUIRES: concurrency

import Swift
import _Concurrency

@main struct Main {
  static func main() async {
    var called = 0

    @concurrent
    func transformation(_ x: Int) -> [Int] {
      return [x + 1]
    }

    func transformationAsync(_ x: Int) async -> [Int] {
      let handle: Task.Handle<[Int], Error> = Task.runDetached {
        transformation(x)
      }
      called += 1

      return try! await handle.get()
    }

    // workaround for rdar://74289867
    func syncFlatMapWorkaround(_ xs: [Int]) -> [Int] {
      xs.flatMap(transformation)
    }

    let xs = [1,2,3,4,5]
    let resSync = syncFlatMapWorkaround(xs)
    let resAsync = await xs.flatMap(transformationAsync)

    assert(resAsync == resSync)
    assert(called == xs.count)
  }
}
