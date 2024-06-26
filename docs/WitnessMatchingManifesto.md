# Protocol Witness Matching Manifesto

Author: [Suyash Srijan](https://www.github.com/theblixguy) 

### Jargon

Before we dive into the document, let's clarify some jargon:

- A _protocol requirement_ (or just _requirement_) is a declaration inside a protocol that all conforming types must satisfy.
- A _protocol witness_ (or just _witness_) is a value or a type that satisfies a protocol requirement.

For example:

```swift
protocol P {
  func foo() // This is a protocol requirement
}

struct S: P {
  func foo() { // This is a protocol witness
    ...
  }
}
```

## Introduction

In Swift (at the time of this writing), protocol witnesses have to pretty much match exactly with a protocol requirement. For example:

```swift
protocol P {
  func foo()
}

struct S: P {
  func foo() { ... }
}
```

```swift
protocol P {
  func getView() -> UIView
}

struct S: P {
  func getView() -> UIView { ... }
}
```

A few specific differences are allowed:

1. A non-throwing function can witness a throwing function requirement:

```swift
protocol P {
  func foo() throws
}

struct S: P {
  // Even though 'foo' here is not marked 'throws', it is
  // still able to satisfy the protocol requirement. The
  // caller has to handle the possibility that it might 
  // throw, but the witness is ofcourse free to throw or 
  // not to, depending on whether it makes sense in the
  // implementation.
  func foo() { ... }
}
```

2. A non-failable initializer can witness a failable initializer requirement:

```swift
protocol P {
  init?()
}

struct S: P {
  // Similarly, the initializer here is free to return
  // a newly created value or just return 'nil' instead.
  init() {}
}
```

3. A non-escaping closure can witness one that is marked `@escaping`:

```swift
protocol P {
  func foo(completion: @escaping() -> ()) 
}

struct S: P {
  // Similar to above, you can use a non-escaping closure here
  // if you want.
  func foo(completion: () -> ()) { ... }
}
```

4. A non-generic function requirement can be witnessed by a generic one:

```swift
struct Some {}

protocol P {
  func foo(arg: Some)
}

struct S: P {
  // This is okay, since 'foo' can accept any T, including 'Some'.
  // If T was constrained in a way that it couldn't accept 'Some',
  // then this (obviously) won't be a valid witness.
  func foo<T>(arg: T) {}
}
```

However, almost any other kind of mismatch is forbidden. For example:

```swift
protocol P {
  func getView() -> UIView
}

struct S: P {
  func getView() -> MyCustomUIViewSubclass { ... } // Not allowed
}
```

```swift
protocol P {
  init()
}

struct S: P {
  init(arg: Int = 0) { ... } // Not allowed
}
```

Many people have argued that the existing model is too restrictive and that more forms of mismatching should be allowed. It's also been suggested that the existing model feels inconsistent with other parts of the language; for example, the code example above where the witness's return type is a subclass of the requirement's return type is allowed with class method overrides. In order to figure out how respond to these statements, we first have to address a very basic question: what should the model be for how we expect witnesses to match with requirements?

This manifesto starts by listing some possible changes we could make that would allow more mismatches. It then sets out some design principles and describes some of the constraints on how the language can reasonably behave. Finally, it proposes a basic model for how witness matching should work in the language. The ultimate goal of this document is to provide a foundation for discussing witness matching in Swift and to spur a series of evolution proposals to bring the language into alignment with its proposed design.

## Likely

These are mismatches that aren't unreasonable to be allowed. Allowing this is mostly a matter of doing the implementation work and does not require any changes to the current syntax. 

These forms of mismatching has also been requested from time to time and can help improve the expressivity of the language, without fundamentally changing it.

### Desugaring

We can enable certain forms of "desugaring", such as allowing enum cases to satisfy static requirements:

_Note: This has now been implemented in Swift 5.3. See [SE-0280](https://github.com/apple/swift-evolution/blob/master/proposals/0280-enum-cases-as-protocol-witnesses.md) for more details._

For example:

```swift
protocol P {
  static var foo: Self { get }
  static func bar(_ value: String) -> Self
}

enum E: P {
  case foo
  case bar(_ value: String)
}

let e: some E = E.foo
```

Enum cases already _behave_ like static properties/functions in the language, both syntactically and semantically, so it's not unreasonable to think of a case as "sugar" for a `static var` or `static func`.

### Default arguments

We can allow functions with default arguments to witness function requirements:

```swift
protocol P {
  func bar(arg: Int)
}

struct S: P {
  func bar(arg: Int, anotherArg: Bool = true) { ... }
}
```

As a special case, we can also allow a function with all default arguments to witness a function requirement with no arguments:

```swift
protocol P {
  init()
}

struct S: P {
  init(arg1: Int = 0, arg2: Bool = false) { ... }
}
```

This seems reasonable, because the protocol really just requires the ability to call `init()` on the conforming type, which you can in this case since all arguments have default values.

Related bugs:
- None

### Subtyping

We can allow protocol witnesses to be covariant (i.e. have subtypes of the requirement):

```swift
protocol P {
  func getView() -> UIView
}

struct S: P {
  func getView() -> MySubclassOfUIView { ... }
}
```

We already allow this in classes, so it probably makes sense for protocols witnesses to follow the same rules that exist for classes, for example, optional subtyping:

```swift
class A {
  func bar() -> Int? { ... }
}

class B: A {
  override func bar() -> Int { ... }
}
```

As we loosen the rules around subtyping and other kinds of matching, it is possible that associated type inference could degrade, so it is important to keep that in mind when working on this feature. There have been some ideas around how to improve it though, such as ones mentioned in [this](https://forums.swift.org/t/rfc-associated-type-inference/7168) post.

Related bugs:
- https://bugs.swift.org/browse/SR-522
- https://bugs.swift.org/browse/SR-1950

Old PR with possible implementation: https://github.com/apple/swift/pull/8718

## Maybe/Unlikely

These are mismatches where either it's not clear whether it belongs to the language, whether it provides any concrete benefits, or it just complicates the existing design and/or introduces other implementation/runtime complexity.

### Properties with function type

We could allow a function to match a property protocol requirement with a function type. For example:

```swift
protocol P {
  var bar: () -> Int
}

struct S: P {
  func bar() -> Int { ... }
}
```

or vice versa - letting a function requirement be witnessed by a property with function type:

```swift
protocol P {
  func bar() -> Int
}

struct S: P {
  var bar: () -> Int { ... }
}
```

### Syntactic matching

We could allow anything that meets the syntactic needs of the requirement to witness that requirement. For example:

```swift
protocol P {
  static func bar(_ instance: Self) -> (Int) -> Bool
}

struct S: P {
  func bar(_ arg: Int) -> Bool { ... }
}
```

We could go to the extreme and allow other kinds of matching, such as allowing `@dynamicMemberLookup` to satisfy arbitrary protocol requirements.

## Design Principles

The basic design principle is that matching shouldn't be entirely based on how the requirement is _written_ (or _spelled_), but rather it should be based on what the protocol requirement _semantically requires_. We probably do not want to take this principle to its extreme though and instead use it to provide a balance between language expressivity and complexity.

Here's a really simple example (which is allowed today):

```swift
protocol P {
  func foo(_ arg1: Int)
}

struct S: P {
  func foo(_ arg2: Int) { ... }
}
```

Here, the requirement is that a caller should be able to invoke a method `T.foo` (where `T` is a concrete type conforming to `P`) which takes a single argument of type `Int` which can be passed without the need to specify an argument name. One can argue that the parameter label should match too, however the name of that label is irrelevant because it does not change the semantics of the requirement, which is that "I should be able to call `foo` without an argument name".

However, this shouldn't be allowed (and isn't currently allowed):

```swift
protocol P {
  func foo(arg1: Int)
}

struct S: P {
  func foo(arg2: Int) { ... }
}
```

because the semantics of the function now have a fundamental difference i.e. you can no longer invoke `T.foo(arg1:)`. In this case, if we subsitute `T` with `S`, then `S.foo(arg1: 123)` is not a valid invocation. 

Of course, we can allow people to work around it, by introducing a new language feature or re-using an existing one:

```swift
struct S: P {
  // Not currently allowed, but could be
  @_implements(P, foo(arg1:))
  func foo(arg2: Int) { ... }
}
```

However, that adds a lot of extra complexity for very little benefit; in fact, you now have to write _more_ code to work around it, whereas it's just simpler to rename the label.

Another example is a "subtype"-like semantic behavior:

```swift
protocol P {
  func foo() throws
}

struct S: P {
  func foo() { ... }
}
```

Even though `S.foo` is not marked `throws` (and `throws` is not really a type), a type `T -> U` can be thought of as a "subtype" of `T -> U throws`. Here, the requirement is that "I can call a function `foo` with no arguments and it _might_ throw". Now, whether `S.foo` throws or not depends on whether it makes sense to do so in the implementation. So, the semantics stay intact.

However, such "subtyping" should not be allowed in places where this is some sort of deeper guarantee. For example:

```swift
// Strawman annotation based on https://forums.swift.org/t/pitch-genericizing-over-annotations-like-throws
protocol P {
  func foo() alwaysthrows
}

struct S: P {
  func foo() throws { print("I didn't throw!") }
}
```

Here, `alwaysthrows` is a semantic guarantee; it says that the method will _always_ throw when it is called. However, `S.foo` may or may not throw. So, `T -> U throws` is not a "subtype" of `T -> U alwaysthrows`.

## A Basic Model

The basic model for witness matching would embrace the above design principles. Luckily, the current model already does that to some extent and by allowing the proposed changes we can align the model to be more in sync with what is reasonable to do.

In practice, this means:

- Exact matches are allowed
- Mismatches are allowed, as long as the semantic requirements are satisfied (for examples, a `throws` mismatch)
- Everything else is disallowed