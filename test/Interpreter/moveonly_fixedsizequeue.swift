// RUN: %target-run-simple-swift(-Xfrontend -sil-verify-all)
// RUN: %target-run-simple-swift(-O -Xfrontend -sil-verify-all)

// REQUIRES: executable_test

enum FixedSizeQueueError : Error {
    case outOfSpace
}

struct FixedSizeQueue<T> : ~Copyable {
    private var readOffset: Int
    private var writeOffset: Int
    private var ptr: UnsafeMutableBufferPointer<T>

    init(capacity: Int) {
        readOffset = 0
        writeOffset = 0
        ptr = UnsafeMutableBufferPointer.allocate(capacity: capacity)
    }

    deinit {
        ptr.deinitialize().deallocate()
    }

    mutating func read() -> T? {
        if readOffset == writeOffset {
            return nil
        }

        let x = ptr[readOffset]
        readOffset = (readOffset + 1) % ptr.count
        return x
    }

    mutating func write(_ element: T) throws {
        if ((writeOffset + 1) % ptr.count) == readOffset {
            throw FixedSizeQueueError.outOfSpace
        }
        ptr[writeOffset] = element
        writeOffset = (writeOffset + 1) % ptr.count
    }

    private consuming func cleanupQueue() -> UnsafeMutableBufferPointer<T> {
        let buffer = self.ptr
        discard self
        return buffer
    }

    // Drain the queue... returning the value.
    mutating func drain() -> UnsafeMutableBufferPointer<T> {
        let count = ptr.count
        let buffer = cleanupQueue()
        self = .init(capacity: count)
        return buffer
    }
}

func main() throws {
    var x = FixedSizeQueue<Int>(capacity: 10)
    precondition(x.read() == nil)
    try x.write(0)
    try x.write(9)
    try x.write(1)
    try x.write(3)
    precondition(x.read() == 0)
    precondition(x.read() == 9)
    precondition(x.read() == 1)
    precondition(x.read() == 3)

    let d = x.drain()
    precondition(d[0] == 0)
    precondition(d[1] == 9)
    precondition(d[2] == 1)
    precondition(d[3] == 3)

    precondition(x.read() == nil)
    try x.write(3)
    try x.write(1)
    try x.write(9)
    try x.write(0)
    precondition(x.read() == 3)
    precondition(x.read() == 1)
    precondition(x.read() == 9)
    precondition(x.read() == 0)
}

try main()
