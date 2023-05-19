// RUN: %target-typecheck-verify-swift -enable-experimental-feature NoImplicitCopy

// This is a variation of `ownership_modifiers.swift` with the expected error
// lines removed, so that the file is parsed by the SwiftSyntax parser
// and we validate that both parsers correctly handle this file.

struct borrowing {}
struct consuming {}

struct Foo {}

func foo(x: borrowing Foo) {}
func bar(x: consuming Foo) {}
func baz(x: (borrowing Foo, consuming Foo) -> ()) {}

// `borrowing` and `consuming` are contextual keywords, so they should also
// continue working as type and/or parameter names

func zim(x: borrowing) {}
func zang(x: consuming) {}
func zung(x: borrowing consuming) {}
func zip(x: consuming borrowing) {}
func zap(x: (borrowing, consuming) -> ()) {}
func zoop(x: (borrowing consuming, consuming borrowing) -> ()) {}

// Parameter specifier names are regular identifiers in other positions,
// including argument labels.

func argumentLabel(borrowing consuming: Int) {}
func argumentLabel(consuming borrowing: Int) {}
func argumentLabel(__shared __owned: Int) {}
func argumentLabel(__owned __shared: Int) {}

// We should parse them as argument labels in function types, even though that

// isn't currently supported.

func argumentLabel(anonBorrowingInClosure: (_ borrowing: Int) -> ()) {}
func argumentLabel(anonConsumingInClosure: (_ consuming: Int) -> ()) {}
func argumentLabel(anonSharedInClosure: (_ __shared: Int) -> ()) {}
func argumentLabel(anonOwnedInClosure: (_ __owned: Int) -> ()) {}

struct MethodModifiers {
    mutating func mutating() {}
    borrowing func borrowing() {}
    consuming func consuming() {}
    nonmutating func nonmutating() {}
    __consuming func __consuming() {}
}
