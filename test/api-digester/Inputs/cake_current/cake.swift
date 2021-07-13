import APINotesTest

public struct S1 {
  public init(_ : Double) {}
  mutating public func foo1() {}
  mutating public func foo2() {}
  public static func foo3() {}
  public func foo4() {}
  public func foo5(x : Int, y: Int, z: Int) {}
}

public class C0 {
  public func foo4(a : Void?) {}
}

public class C1: C0 {
  public func foo1() {}
  public func foo2(_ : ()->()) {}
  public var CIIns1 : C1?
  public weak var CIIns2 : C1?
  public func foo3(a : ()?) {}
  public init(_ : C1) {}
}

public typealias C3 = C1

public struct NSSomestruct2 {
  public static func foo1(_ a : C3) {}
}

public class C4: NewType {}

public class C5 {
  @objc
  public dynamic func dy_foo() {}
}

@frozen
public struct C6 {}

public enum IceKind {}

@available(macOS 10.15, iOS 13, tvOS 13, watchOS 6, *)
public enum FutureKind {
  case FineToAdd
}

public protocol P1 {}

public protocol P2 {}

public extension P1 {
  func P1Constraint() {}
}

@frozen
public struct fixedLayoutStruct {
  public var a = 1
  public func OKChange() {}
  private static let constant = 0
  public var b = 2
  public func foo() {}
  private var c = 3
  private lazy var lazy_d = 4
  public var height: Int {
    get { return 0 }
  }
}

@usableFromInline
@frozen
struct fixedLayoutStruct2 {
  public var NoLongerWithFixedBinaryOrder: Int { return 1 }
  public var BecomeFixedBinaryOrder = 1
}

@frozen
public enum FrozenKind {
  case Unchanged
  case Rigid
  case Fixed
  case AddedCase
}

public class C7: P1 {
  public func foo(_ a: Int, _ b: Int) {}
}

public class C8: C7 {}

public protocol P3: P1, P4 {}

public protocol P4 {}

extension fixedLayoutStruct: P2 {}

public protocol AssociatedTypePro {
  associatedtype T1
  associatedtype T2
  associatedtype T3 = C6
}

public class RemoveSetters {
  public private(set) var Value = 4
  public subscript(_ idx: Int) -> Int {
    get { return 1 }
  }
}

public protocol RequiementChanges {
  associatedtype addedTypeWithDefault = Int
  associatedtype addedTypeWithoutDefault
  func addedFunc()
  var addedVar: Int { get }
}

/// This protocol shouldn't be complained because its requirements are all derived.
public protocol DerivedProtocolRequiementChanges: RequiementChanges {}

public class SuperClassRemoval {}

public struct ClassToStruct {
  public init() {}
}

open class ClassWithMissingDesignatedInits {
  // Remove the @_hasMissingDesignatedInitializers attribute
  public init() {}
  public convenience init(x: Int) { self.init() }
}

open class ClassWithoutMissingDesignatedInits {
  // Add the @_hasMissingDesignatedInitializers attribute by adding an inaccessible
  // init
  public init() {}
  public convenience init(x: Int) { self.init() }
  internal init(y: Int) {}
}

public class SubclassWithMissingDesignatedInits: ClassWithMissingDesignatedInits {
}

public class SubclassWithoutMissingDesignatedInits: ClassWithoutMissingDesignatedInits {
}

public enum ProtocolToEnum {}

public class SuperClassChange: C8 {}


public class GenericClass<T> {}

public class SubGenericClass: GenericClass<P2> {}

@objc
public protocol ObjCProtocol {
  @objc
  func removeOptional()
  @objc
  optional func addOptional()
}

public var GlobalLetChangedToVar = 1
public let GlobalVarChangedToLet = 1

public class ClassWithOpenMember {
  public class func foo() {}
  public var property: Int {get { return 1}}
  public func bar() {}
}

public class EscapingFunctionType {
  public func removedEscaping(_ a: ()->()) {}
  public func addedEscaping(_ a: @escaping ()->()) {}
}

prefix operator ..*..

public func ownershipChange(_ a: Int, _ b: __owned Int) {}

@usableFromInline
@_fixed_layout
class _NoResilientClass {
  @usableFromInline
  func NoLongerFinalFunc() {}
  private func FuncPositionChange1() {}
  private func FuncPositionChange0() {}
  private func FuncPositionChange2() {}
}

public class FinalFuncContainer {
  public final func NewFinalFunc() {}
  public func NoLongerFinalFunc() {}
}

public protocol AssociatedTypesProtocol {
  associatedtype T
}

public class TChangesFromIntToString: AssociatedTypesProtocol {
  public typealias T = String
}

public protocol HasMutatingMethod {
  mutating func foo()
  var bar: Int { mutating get }
}

public protocol HasMutatingMethodClone: HasMutatingMethod {
  func foo()
  var bar: Int { get }
}

public protocol Animal {}
public class Cat: Animal { public init() {} }
public class Dog: Animal { public init() {} }

public class Zoo {
  public init() {}
  @inlinable
  @available(iOS 13.0, OSX 10.15, tvOS 13.0, watchOS 6.0, *)
  public var current: some Animal {
    return Dog()
  }
  @inlinable
  @available(iOS 13.0, OSX 10.15, tvOS 13.0, watchOS 6.0, *)
  public func getCurrentAnimalInlinable() -> some Animal {
    return Dog()
  }
}

public func returnFunctionTypeOwnershipChange() -> (__owned C1) -> () { return { _ in } }

@objc(NewObjCClass)
public class SwiftObjcClass {
  @objc(NewObjCFool:NewObjCA:NewObjCB:)
  public func foo(a:Int, b:Int, c: Int) {}
}

@available(macOS 10.15, iOS 13, tvOS 13, watchOS 6, *)
open class AddingNewDesignatedInit {
  public init(_ b: Bool) {}
  public init() {}
  public convenience init(foo: Int) {
    self.init()
    print(foo)
  }
}

public extension Float {
  func floatHigher() {}
}

infix operator <==> : AssignmentPrecedence

public actor FutureActor {
  public func foo(a:Int, b:Int, c: Int) {}
}
