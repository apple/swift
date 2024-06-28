// These are purely to add test coverage of different constructs when emitting modules

public protocol _NoCopyP: ~Copyable {}
public protocol _NoEscapableP: ~Escapable {}

extension Int: _NoCopyP {}

public struct _Toys {
  static func test_parallelAssignment() {
    var y: Int
    var x: Int
    (x, y) = (10, 11)
  }

  public struct rdar118697289_S1<Element> {
    let element: Element
    func f() -> Element { element }
  }

  public struct rdar118697289_S2<Element> {
      let element: Element
      subscript(i: Int) -> Element {
          element
      }
  }

  public static func allCopyable1<T>(_ a: T, _ b: T) -> T { return a }

  public static func allCopyable2<T>(_ s: T)
                                  where T: _NoCopyP {}

  public static func oneCopyable1<T, V: ~Copyable>(_ s: T, _ v: borrowing V)
                                  where T: _NoCopyP {}

  public static func oneCopyable2<T, V>(_ s: borrowing T, _ v: V)
                                  where T: _NoCopyP, T: ~Copyable {}

  public static func oneCopyable3<T, V>(_ s: borrowing T, _ v: V)
                                  where T: _NoCopyP & ~Copyable {}

  public static func basic_some(_ s: some _NoCopyP) {}

  public static func basic_some_nc(_ s: borrowing some _NoCopyP & ~Copyable) {}

  public static func oneEscapable<T, V>(_ s: T, _ v: V)
                                    where T: _NoEscapableP, T: ~Escapable {}

  public static func canEscapeButConforms<T: _NoEscapableP>(_ t: T) {}

  public static func opaqueNonEscapable(_ s: some _NoEscapableP & ~Escapable) {}

  public static func opaqueEscapable(_ s: some _NoEscapableP) {}
}

public struct ExplicitHello<T: ~Copyable>: ~Copyable {
  let thing: T
}
extension ExplicitHello: Copyable where T: Copyable {}

public struct Hello<T: ~Copyable>: ~Copyable, ~Escapable where T: ~Escapable {}

extension Hello: Escapable where T: Escapable {}
extension Hello: Copyable where T: Copyable {}

public protocol TestAssocTypes {
  associatedtype A: ~Copyable, _NoCopyP = Int
}

public typealias SomeAlias<G> = Hello<G>

public typealias AliasWithInverse<G> = Hello<G> where G: ~Copyable, G: ~Escapable

public struct RudePointer<T: ~Copyable>: Copyable {}

public class C {}

public func noInversesSTART() {}
public func checkAny<Result>(_ t: Result) where Result: Any {}
public func usingClassConstraint<Result>(arg: Result) -> Result? where Result: C { return arg }
public func withAnyObject<Result>(_ t: Result) where Result: AnyObject {}
public func noInversesEND() {}

public func checkAnyInv1<Result>(_ t: borrowing Result) where Result: Any & ~Copyable {}
public func checkAnyInv2<Result: Any>(_ t: borrowing Result) where Result: ~Copyable & ~Escapable {}
public func checkAnyObject<Result>(_ t: Result) where Result: AnyObject {}

// coverage for rdar://123281976
public struct Outer<A: ~Copyable>: ~Copyable {
  public func innerFn<B: ~Copyable>(_ b: borrowing B) {}
  public struct InnerStruct<C: ~Copyable>: ~Copyable {
    public func g<D>(_ d: borrowing D) where D: ~Copyable {}
  }
  public struct InnerVariation1<D: ~Copyable>: ~Copyable, ~Escapable {}
  public struct InnerVariation2<D: ~Escapable>: ~Copyable, ~Escapable {}
}

extension Outer: Copyable where A: Copyable {}
extension Outer.InnerStruct: Copyable where C: Copyable, A: Copyable {}

extension Outer.InnerVariation1: Copyable where A: Copyable, D: Copyable & Escapable {}
extension Outer.InnerVariation2: Escapable where A: Escapable, D: Escapable {}

extension Outer.InnerStruct {
    public func hello<T: ~Escapable>(_ t: T) {}
}

@_preInverseGenerics
public func old_swap<T: ~Copyable>(_ a: inout T, _ b: inout T) {}

@_preInverseGenerics
public func borrowsNoncopyable<T: ~Copyable>(_ t: borrowing T) {}

@_disallowFeatureSuppression(NoncopyableGenerics)
public func suppressesNoncopyableGenerics<T: ~Copyable>(_ t: borrowing T) {}

// coverage for rdar://127389991
@_disallowFeatureSuppression(NoncopyableGenerics)
public struct LoudlyNC<T: ~Copyable> {}
public func _indexHumongousDonuts<TTT, T>(_ aggregate: UnsafePointer<TTT>, _ index: Int) -> T {
    return UnsafeRawPointer(aggregate).load(
    fromByteOffset: index * MemoryLayout<T>.stride, as: T.self)
}
public func referToLoud(_ t: LoudlyNC<String>) {}
@_disallowFeatureSuppression(NoncopyableGenerics) public func referToLoudProperGuarding(_ t: LoudlyNC<String>) {}
public struct NoCopyPls: ~Copyable {}
public func substCopyable(_ t: String?) {}
public func substGenericCopyable<T>(_ t: T?) {}
public func substNC(_ t: borrowing NoCopyPls?) {}
public func substGenericNC<T: ~Copyable>(_ t: borrowing T?) {}

// coverage for rdar://126090425
protocol P : ~Copyable {} // NOTE: it's important that this is NOT public.
protocol Q: ~Copyable {}  // NOTE: it's important that this is NOT public.
public protocol Publik: ~Copyable {}
public struct Concrete : (P & ~Copyable) {}
public struct Generic<T: Publik & ~Copyable> : (P & ~Copyable) {}
public struct VeryNested: (P & (Q & ~Copyable & Publik) & (P & ~Copyable)) {}
public struct Twice: P & ~Copyable, Q & ~Copyable {}
public struct RegularTwice: ~Copyable, ~Copyable {}

// coverage for rdar://130179698
public struct Continuation<T: ~Copyable, E: Error> {
  public func resume(returning value: consuming T) where E == Never {}
}
