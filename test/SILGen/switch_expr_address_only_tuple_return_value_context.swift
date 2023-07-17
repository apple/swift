// RUN: %target-swift-emit-silgen -verify %s

// rdar://112213253

struct BigNontrivialThing {
    var x: Any
    var y: Any
}
enum Foo {
    case a(String)
    case b(BigNontrivialThing, String)

    var unpacked: (BigNontrivialThing?, String) {
        switch self {
        case .a(let s):
            (nil, s)
        case .b(let bnt, let s):
            (bnt, s)
        }
    }
}

func throwBNT() throws -> BigNontrivialThing? { fatalError() }
func nothrowBNT() -> BigNontrivialThing? { fatalError() }
func throwStr() throws -> String { fatalError() }
func nothrowStr() -> String { fatalError() }

func maybeThrowDuringSingleExprSwitchResult(condition: Bool) throws
    -> (BigNontrivialThing?, String) {
    
    switch (condition, condition) {
    case (false, false):
        (nothrowBNT(), nothrowStr())
    case (false, true):
        (nothrowBNT(), try throwStr())
    case (true, false):
        (try throwBNT(), nothrowStr())
    case (true, true):
        (try throwBNT(), try throwStr())
    }
}
