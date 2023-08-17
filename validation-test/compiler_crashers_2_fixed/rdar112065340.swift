// RUN: %target-swift-frontend -emit-ir %s

public func test<each T: BinaryInteger>(_ values: repeat each T) {
    var values = (repeat UInt32(truncatingIfNeeded: each values))
    withUnsafePointer(to: &values) { ptr in
        _ = ptr[0]
    }
}
