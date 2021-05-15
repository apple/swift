// RUN: %target-run-simple-swift(-parse-as-library -Xfrontend -enable-experimental-concurrency)

// REQUIRES: executable_test
// REQUIRES: concurrency

import Swift
import _Concurrency

@main struct Main {
  static func main() async {
    var called = 0

    func ignoreAsync(_ x: Int) async -> Void {
      let handle: Task.Handle<Void, Error> = Task.runDetached {
        ()
      }
      called += 1

      return try! await handle.get()
    }

    let xs = [1,2,3,4,5]
    await xs.forEach(ignoreAsync)
    
    assert(called == xs.count)
  }
}
