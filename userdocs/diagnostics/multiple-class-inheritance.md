# Multiple Inheritance of Classes

In some programming languages, a class can have multiple superclasses. This feature is called multiple inheritance and is unsupported in Swift. However, using protocols, one can achieve a similar effect as multiple inheritance.

Consider the following example with a class `Bird` conforming to two protocols `Animal` and `Flyable`.

```swift
protocol Flyable {
    func fly()
}

extension Animal {
    func move() {
        print("Moving...")
    }
}

protocol Animal {
    func move()
}

extension Flyable {
    func fly() {
        print("Flying...")
    }
}

class Bird: Animal, Flyable {
    // move() and fly() use default implementations
}
```

Since `Animal` and `Flyable` provide default implementations for their protocol requirements, `Bird` can conform to both without implementing `fly()` or `move()` and the default implementation will be used. This effect is similar to class inheritance where a subclass inherits properties, subscripts and methods from a superclass. The default implementation can be overriden by defining `fly()` or `move()` without an `override` keyword.

There is one caveat here on the interaction with subclassing. If a superclass conforms a protocol but does not override the default implementation of a protocol requirement, its subclasses cannot override the default implementation ([SR-103](https://bugs.swift.org/browse/SR-103)).

To learn more about defining and adopting protocols as well as inheritance and subclassing, see the [Protocols](https://docs.swift.org/swift-book/LanguageGuide/Protocols.html) section and the [Inheritance](https://docs.swift.org/swift-book/LanguageGuide/Inheritance.html) section in _The Swift Programming Language_.
