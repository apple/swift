// RUN: %target-swift-frontend -parse -verify %s -disable-experimental-parser-round-trip -enable-experimental-feature TrailingComma

// Tuple and Tuple Pattern

let _ = (a: 1, b: 2, c: 3,)

let (_, _,) = (1,)

// Arguments and Parameters

func foo(a: Int = 0, b: Int = 0, c: Int = 0,) -> Int {
    return a + b + c
}

foo(a: 1, b: 2, c: 3,)

// Subscript

foo[1, 2,]

// KeyPath Subscript

\Foo.bar[0,1,]

// Generic Parameters

struct S<T1, T2,> { }

func foo<T1, T2,>() { }

protocol P<T1, T2> {
    associatedtype T1
    associatedtype T2
}

// Generic Where Clause List

struct S<T1, T2, T3,> where T1: P1, T2: P2, { }

// Closure Capture List

let _ = { [obj1, obj2,] in }

// Switch Case List

switch number {
    case 1, 2,:
        break
    default:
        break
}

// Attributes

@Foo(a, b, c,) struct S { }

f(_: @foo(1, 2,) Int)

// Macro Expansions

#foo(1, 2,)

struct S {
    #foo(1, 2,)
}

// Macro Role Attribute

@attached(extension, conformances: OptionSet,)
macro OptionSet<RawType>() = #externalMacro(module: "SwiftMacros", type: "OptionSetMacro")

// String Literal Interpolation

"\(1,)"

// Availability Spec List

if #available(iOS 15, watchOS 9, *,) { }

if #unavailable(iOS 15, watchOS 9,) { }