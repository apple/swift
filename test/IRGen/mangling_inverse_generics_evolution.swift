// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-ir -o - %s -module-name test \
// RUN:   -enable-experimental-feature NoncopyableGenerics \
// RUN:   -enable-experimental-feature NonescapableTypes \
// RUN:   -parse-as-library \
// RUN:   -enable-library-evolution \
// RUN:   -g \
// RUN:   > %t/test.irgen

// RUN: %FileCheck %s < %t/test.irgen

public protocol P: ~Copyable { }

public struct CallMe<U: ~Copyable> {
  public enum Maybe<T: ~Copyable>: ~Copyable {
    // CHECK-LABEL: @"$s4test6CallMeV5MaybeOAARiczrlE4someyAEyx_qd__Gqd__cAGmr__lFWC"
    // CHECK: @"$s4test6CallMeV5MaybeOAARiczrlE4noneyAEyx_qd__GAGmr__lFWC"
    case none
    case some(T)
  }
}

extension CallMe {
  public enum Box<T: ~Copyable>: ~Copyable {
    // CHECK-LABEL: @"$s4test6CallMeV3BoxO4someyAEyx_qd__Gqd__cAGmr__lFWC"
    // CHECK: @"$s4test6CallMeV3BoxO4noneyAEyx_qd__GAGmr__lFWC"
    case none
    case some(T)
  }
}

// CHECK: @"$s4test13ManagedBufferC12_doNotCallMeACyxq_Gyt_tcfCTq"


public protocol Hello<Person>: ~Copyable {
  // CHECK: @"$s4test5HelloP6PersonAC_AA1PTn"
  // CHECK: @"$s6Person4test5HelloPTl" =
  associatedtype Person: P & ~Copyable

  // CHECK: @"$s4test5HelloP14favoritePerson0D0QzvrTq" =
  var favoritePerson: Person { get }

  // CHECK: @"$s4test5HelloP5greetyy6PersonQzFTq"
  func greet(_ person: borrowing Person)

  // CHECK: @"$s4test5HelloP10overloadedyyqd__lFTj"
  func overloaded<T>(_: borrowing T)

  // CHECK: @"$s4test5HelloP10overloadedyyqd__Ricd__lFTj"
  func overloaded<T: ~Copyable>(_: borrowing T)
}

// CHECK: $s4test2XQVAARiczrlE1AVMi
protocol Q: ~Copyable {
  associatedtype A: ~Copyable
}

struct XQ<T: ~Copyable>: ~Copyable {
}

extension XQ: Q where T: ~Copyable {
  struct A { }
}

protocol HasResult {
  associatedtype Success
}

extension Result: HasResult {
  func testMe() -> Success? {
    var x: Result.Success? = nil
    if case let .success(s) = self {
      x = s
    }
    return x
  }
}

// Class metadata

@_fixed_layout
open class ManagedBuffer<Header, Element: ~Copyable> {
   @_preInverseGenerics
  public final var header: Header

  // CHECK: @"$s4test13ManagedBufferC12_doNotCallMeACyxq_Gyt_tcfCTj"
  @_preInverseGenerics
  @usableFromInline
  internal init(_doNotCallMe: ()) {
    fatalError("boom")
  }
}

extension UnsafePointer {
  struct AtomicRepresentation { }
}

// CHECK: $sSP4testE20AtomicRepresentationVySi_GD
func useAtomicRepresentation() {
  let x = UnsafePointer<Int>.AtomicRepresentation()
  print(x)
}

struct Box<Wrapped: ~Copyable>: ~Copyable { }

struct List<Element: ~Copyable>: ~Copyable {
  // CHECK: $s4test4ListVAARiczrlE4NodeVwst
  struct Node: ~Copyable {
    var element: Element
    var next: Link
  }
  typealias Link = Box<Node>?

  var head: Link
}
