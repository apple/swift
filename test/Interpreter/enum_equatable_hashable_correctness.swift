// RUN: %target-run-simple-swift
// RUN: %target-run-simple-swift(-Xfrontend -unavailable-decl-optimization=complete)
// REQUIRES: executable_test

import StdlibUnittest

enum Token: Hashable {
  case string(String)
  case number(Int)
  case comma
  case colon
}

enum Combo<T: Hashable, U: Hashable>: Hashable {
  case none
  case first(T)
  case second(U)
  case both(T, U)
}

@available(*, unavailable)
struct UnavailableStruct: Hashable {}

enum HasUnavailableCases: Hashable {
  case available
  case availablePayload(Int)

  @available(*, unavailable)
  case unavailable

  @available(*, unavailable)
  case unavailablePayload(UnavailableStruct)
}

var EnumSynthesisTests = TestSuite("EnumSynthesis")

EnumSynthesisTests.test("BasicEquatability/Hashability") {
  checkHashable([
    Token.string("foo"),
    Token.number(10),
    Token.comma,
    Token.colon,
  ], equalityOracle: { $0 == $1 })
}

// Not guaranteed by the semantics of Hashable, but we sanity check that the
// synthesized hash function is good enough to not let nearby values collide.
EnumSynthesisTests.test("CloseValuesDoNotCollide") {
  expectNotEqual(Token.string("foo").hashValue, Token.string("goo").hashValue)
  expectNotEqual(Token.number(10).hashValue, Token.number(11).hashValue)
}

EnumSynthesisTests.test("GenericEquatability/Hashability") {
  checkHashable([
    Combo<String, Int>.none,
    Combo<String, Int>.first("a"),
    Combo<String, Int>.second(5),
    Combo<String, Int>.both("foo", 5),
  ], equalityOracle: { $0 == $1 })
}

EnumSynthesisTests.test("CloseGenericValuesDoNotCollide") {
  expectNotEqual(Combo<String, Int>.first("foo").hashValue, Combo<String, Int>.first("goo").hashValue)
  expectNotEqual(Combo<String, Int>.second(3).hashValue, Combo<String, Int>.second(4).hashValue)
  expectNotEqual(Combo<String, Int>.both("foo", 3).hashValue, Combo<String, Int>.both("goo", 3).hashValue)
  expectNotEqual(Combo<String, Int>.both("foo", 3).hashValue, Combo<String, Int>.both("foo", 4).hashValue)
  expectNotEqual(Combo<String, Int>.both("foo", 3).hashValue, Combo<String, Int>.both("goo", 4).hashValue)
}

EnumSynthesisTests.test("HasUnavailableCasesEquatability/Hashability") {
  checkHashable([
    HasUnavailableCases.available,
    HasUnavailableCases.availablePayload(2),
  ], equalityOracle: { $0 == $1 })
}

func hashEncode(_ body: (inout Hasher) -> ()) -> Int {
  var hasher = Hasher()
  body(&hasher)
  return hasher.finalize()
}

// Make sure that if the user overrides the synthesized member, that one gets
// used instead.
enum Overrides: Hashable {
  case a(Int), b(String)
  var hashValue: Int { return 2 }
  func hash(into hasher: inout Hasher) {
    hasher.combine(2)
  }
  static func == (lhs: Overrides, rhs: Overrides) -> Bool { return true }
}

EnumSynthesisTests.test("ExplicitOverridesSynthesized") {
  checkHashable(expectedEqual: true, Overrides.a(4), .b("foo"))
  expectEqual(Overrides.a(4).hashValue, 2)
  expectEqual(
    hashEncode { $0.combine(Overrides.a(4)) },
    hashEncode { $0.combine(2) })
}

// ...even in an extension.
enum OverridesInExtension: Hashable {
  case a(Int), b(String)
}
extension OverridesInExtension {
  var hashValue: Int { return 2 }
  func hash(into hasher: inout Hasher) {
    hasher.combine(2)
  }
  static func == (lhs: OverridesInExtension, rhs: OverridesInExtension) -> Bool { return true }
}

EnumSynthesisTests.test("ExplicitOverridesSynthesizedInExtension") {
  checkHashable(expectedEqual: true, OverridesInExtension.a(4), .b("foo"))
  expectEqual(OverridesInExtension.a(4).hashValue, 2)
  expectEqual(
    hashEncode { $0.combine(OverridesInExtension.a(4)) },
    hashEncode { $0.combine(2) })
}

// Try an indirect enum.
enum BinaryTree<Element: Hashable>: Hashable {
  indirect case tree(BinaryTree, BinaryTree)
  case leaf(Element)
}

EnumSynthesisTests.test("IndirectEquatability/Hashability") {
  let one = BinaryTree<Int>.tree(.leaf(10), .leaf(20))
  let two = BinaryTree<Int>.tree(.leaf(10), .leaf(30))
  let three = BinaryTree<Int>.tree(.leaf(15), .leaf(20))
  let four = BinaryTree<Int>.tree(.leaf(15), .leaf(30))
  checkHashable([one, two, three, four], equalityOracle: { $0 == $1 })
}

runAllTests()
