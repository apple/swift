// RUN: %target-run-simple-swift(%import-libdispatch -parse-as-library)

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: libdispatch

@available(SwiftStdlib 5.5, *)
actor Number {
    var val: Int

    func increment() { val += 1 }

    func fib(n: Int) -> Int {
        if n < 2 {
            return n
        }
        return fib(n: n-1) + fib(n: n-2)
    }

    init() async {
        val = 0

        Task.detached { await self.increment() }

        // do some synchronous work
        let ans = fib(n: 40)
        guard ans == 102334155 else {
            fatalError("got ans = \(ans). miscomputation?")
        }

        // make sure task didn't modify me!
        guard val == 0 else {
            fatalError("race!")
        }
    }

    init(iterations: Int) async {
        repeat {
            val = 0
        } while iterations > 0
    }
}

@available(SwiftStdlib 5.5, *)
@main struct Main {
    static func main() async {
        _ = await Number()

        _ = await Number(iterations: 1000)
    }
}

