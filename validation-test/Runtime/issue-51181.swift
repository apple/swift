// RUN: %target-run-simple-swift
// REQUIRES: executable_test

// https://github.com/apple/swift/issues/51181

private protocol AnyChangeTracker {
    var myVariable: Any? { get }
    var isModified: Bool { get }
}

protocol ChangeTrackerType {
    associatedtype Value
    var originalValue: Value { get }
    var value: Value { get set }
    var isModified: Bool { get }
}

extension ChangeTrackerType where Value: Equatable {
    var isModified: Bool {
	return value != originalValue
    }
}

struct ChangeTracker<T: Equatable>: ChangeTrackerType {
    let originalValue: T
    var value: T

    init(value: T) {
	originalValue = value
	self.value = value
    }
}

extension ChangeTracker: AnyChangeTracker where Value: OptionalType, Value.Wrapped: Equatable {
    var myVariable: Any? {
	return value
    }
}

protocol OptionalType {
    associatedtype Wrapped
    var value: Wrapped? { get }
}

extension Optional: OptionalType {
    var value: Wrapped? {
	return self
    }
}

let s: Any = ChangeTracker<String?>(value: "Foo")

guard let s = s as? AnyChangeTracker else {
  fatalError("Does not comply to AnyChangeTracker")
}

let myVar = String(describing: s.myVariable ?? "nil")
assert(myVar == "Optional(\"Foo\")")

