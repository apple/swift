// RUN: %target-run-simple-swift(-Xfrontend -sil-verify-all) | %FileCheck %s
// RUN: %target-run-simple-swift(-O -Xfrontend -sil-verify-all) | %FileCheck %s

// REQUIRES: executable_test

public struct BufferView<T>: ~Copyable {
    var ptr: UnsafeBufferPointer<T>

    var count: Int {
        return ptr.count
    }

    subscript(_ x: Int) -> T {
        return ptr[x]
    }

    deinit {}
}

extension Array {
    public mutating func withBufferView<U>(_ f: (borrowing BufferView<Element>) -> U) -> U {
        return withUnsafeBufferPointer {
            return f(BufferView(ptr: $0))
        }
    }
}

func testBufferView(_ x: __owned [Int]) {
    var y = consume x
    // CHECK: 1
    // CHECK: 2
    // CHECK: 3
    y.withBufferView {
        for i in 0..<$0.count {
            print($0[i])
        }
    }
}

@inline(never)
func getBool() -> Bool { return true }

func testConditionalBufferView(_ x: __owned [Int]) {
    (consume x).withUnsafeBufferPointer {
        let y = BufferView(ptr: $0)
        // CHECK: 4
        // CHECK: 5
        // CHECK: 6
        if getBool() {
            for i in 0..<y.count {
                print(y[i])
            }
        }
    }
}

func main() {
    testBufferView([1,2,3])
    testConditionalBufferView([4,5,6])
}

main()
