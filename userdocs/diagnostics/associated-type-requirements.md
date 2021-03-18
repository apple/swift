# Using Protocols with `Self` or Associated Type Requirements
Protocols in Swift may be used as types, as part of a generic constraint, or as part of an opaque result type.

```swift
// CustomStringConvertible can be used as a type.
func foo(bar: CustomStringConvertible) { /* ... */ }

// ...or as a generic constraint on 'T'.
func bar<T: CustomStringConvertible>(baz: T) { /* ... */ }

// ...or as part of an opaque result type.
func baz() -> some CustomStringConvertible { /* ... */ }
```

While all Swift protocols can be used as generic constraints and as part of opaque result types, not all protocols can be used as types in general. Specifically, if a protocol has a requirement which references `Self` or an associated type, it cannot be used as a type. One such protocol is `Identifiable`, which has the requirement `var id: ID { get }`, where `ID` is an associated type. As a result, the following code is not allowed:

```swift
func foo(bar: Identifiable) { /* ... */ }
// error: protocol 'Identifiable' can only be used as a conformance constraint because it has Self or associated type requirements
```

Protocols like `Identifiable` which have `Self` or associated type requirements cannot be used as types because such types would rarely be useful in practice. They would be unable to allow use of `Self` or associated type requirements like `var id: ID { get }` because the associated type is not specified.

When working with protocols having `Self` or associated type requirements constrained generics, opaque result types, or manual type erasure is sufficient to support most use cases. To learn more, see the [Protocols](https://docs.swift.org/swift-book/LanguageGuide/Protocols.html), [Generics](https://docs.swift.org/swift-book/LanguageGuide/Generics.html), and [Opaque Types](https://docs.swift.org/swift-book/LanguageGuide/OpaqueTypes.html) sections of _The Swift Programming Language_.
