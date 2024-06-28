// RUN: %target-swift-frontend -emit-ir %s -disable-availability-checking

public protocol Signal {
    mutating func process() -> Float
}

public struct Mixer<each Source: Signal> {
    public var sources: (repeat each Source)

    public mutating func process() -> Float {
        var result: Float = 0

        self.sources = (repeat ({
            var signal = $0
            result += signal.process()
            return signal
        }(each sources)))

        return result
    }
}
