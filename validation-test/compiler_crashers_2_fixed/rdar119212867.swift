// RUN: %target-swift-frontend -emit-ir %s -disable-availability-checking

protocol P {
    func evaluate(_ item: Int) -> Bool
}

struct G<each F: P>: P {
    init(filters: repeat each F) {
        self.filters = (repeat each filters)
    }

    var filters: (repeat each F)

    func evaluate(_ item: Int) -> Bool {
        var result = true

        (repeat result.and((each filters).evaluate(item)))

        return result
    }
}

private extension Bool {
    mutating func and(_ other: @autoclosure () -> Bool) {
        self = self && other()
    }
}

struct S1: P {
    func evaluate(_ item: Int) -> Bool {
        print("S1", item)
        return false
    }
}
struct S2: P {
    func evaluate(_ item: Int) -> Bool {
        print("S2", item)
        return false
    }
}

do {
    let filter = G(filters: S1(), S2())
    print(filter.evaluate(5))
}
