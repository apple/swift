// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking -parse-as-library)

// REQUIRES: concurrency
// REQUIRES: executable_test
// REQUIRES: concurrency_runtime

import Darwin

actor DeterministicOrderThanksToSend {
    var counter = 0

    func test_mainActor_taskOrdering() async {
        var tasks = [(Int, Task<Void, Never>)]()
        for iteration in 1...100 {
            fputs("spawn:\(iteration)\n", stderr)


            self.send {
                fputs("run:\(iteration)\n", stderr)
                self.counter += 1
                precondition(counter == iteration, "counter:\(counter) != iteration:\(iteration)") // often fails
            }
        }

        while self.counter < 100 {
            try? await Task.sleep(until: .now.advanced(by: .milliseconds(100)), clock: .continuous)
        }
    }
}


@available(SwiftStdlib 5.1, *)
@main struct Main {
    static func main() async {
        let deterministicOrderThanksToSend = DeterministicOrderThanksToSend()
        await deterministicOrderThanksToSend.test_mainActor_taskOrdering()
    }
}