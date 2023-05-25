import Swift

public class SimpleClass {
    public let x: Int

    public init(x: Int) {
        self.x = x
    }

    deinit {
        print("SimpleClass deinitialized!")
    }
}

public struct SimpleBig {
    let x0: Int = 0
    let x1: Int = 0
    let x2: Int = 0
    let x3: Int = 0
    let x4: Int = 0
    let x5: Int = 0
    let x6: Int = 0
    let x7: Int = 0
    let x8: Int = 0
    let x9: Int = 0

    public init() {}
}

public struct Simple {
    public let x: Int
    public let y: SimpleClass
    public let z: SimpleBig

    public init(x: Int, y: SimpleClass, z: SimpleBig) {
        self.x = x
        self.y = y
        self.z = z
    }
}

public class GenericClass<T> {
    let x: T

    public init(x: T) {
        self.x = x
    }
}

public struct GenericBig<T> {
    let x: T   
    let x1: T  
    let x2: T  
    let x3: T  
    let x4: T  
    let x5: T  
    let x6: T  
    let x7: T  
    let x8: T  
    let x9: T  
    let x10: T 

    public init(x: T) {
        self.x = x
        self.x1 = x
        self.x2 = x
        self.x3 = x
        self.x4 = x
        self.x5 = x
        self.x6 = x
        self.x7 = x
        self.x8 = x
        self.x9 = x
        self.x10 = x
    }
}

public struct Generic<A, B, C> {
    public init(x: A, y: GenericClass<B>, z: GenericBig<C>) {
        self.x = x
        self.y = y
        self.z = z
    }

    public let x: A
    public let y: GenericClass<B>
    public let z: GenericBig<C>
}

public struct WeakNativeWrapper {
    public weak var x: SimpleClass?
    let y: Int = 0

    public init(x: SimpleClass) {
        self.x = x
    }
}

public struct UnownedNativeWrapper {
    public unowned let x: SimpleClass
    let y: Int = 0

    public init(x: SimpleClass) {
        self.x = x
    }
}

public struct ClosureWrapper {
    public let f: () -> ()
    let x: Int = 0

    public init(f: @escaping () -> ()) {
        self.f = f
    }
}

public protocol SomeProtocol {}

public struct ExistentialWrapper {
    let x: any SomeProtocol
    let y: Int = 0

    public init(x: any SomeProtocol) {
        self.x = x
    }
}

public protocol SomeClassProtocol: AnyObject {}

public struct ExistentialRefWrapper {
    let x: any SomeClassProtocol
    let y: Int = 0

    public init(x: any SomeClassProtocol) {
        self.x = x
    }
}

public enum NullableRefEnum {
    case nonEmpty(SimpleClass)
    case empty
}

public enum ForwardToPayloadEnum {
    case nonEmpty(SimpleClass, Int)
    case empty
}

public struct GenericTupleWrapper<T> {
    let x: Int = 23
    let y: (T, Int)

    public init(_ y: (T, Int)) {
        self.y = y
    }
}

public struct GenericNestedOuter<T> {
    public struct Inner {
        let x: Int = 34
        let y: T

        public init(_ y: T) {
            self.y = y
        }
    }
}

public struct GenericNestedRefOuter<T: AnyObject> {
    public struct Inner {
        let x: Int = 34
        let y: T

        public init(_ y: T) {
            self.y = y
        }
    }
}

public enum SimpleEnum {
    case a(AnyObject, Int)
    case b
    case c(Int, AnyObject)
}

struct SimpleEnumWrapper {
    let x: SimpleEnum
    let y: Int = 2
}

public struct GenericEnumWrapper<T> {
    let x: SimpleEnumWrapper
    let y: T

    public init(_ x: SimpleEnum, _ y: T) {
        self.x = SimpleEnumWrapper(x: x)
        self.y = y
    }
}

public struct Recursive3<T> {
    let x: Int
    let y: AnyObject
}

public struct Recursive2<T> {
    let x: Recursive3<Recursive<T>>
    let y: AnyObject
}

public struct Recursive<T> {
    let x: T
    let xs: Recursive2<T>?

    public init(_ x: T, _ xs: Recursive2<T>?) {
        self.x = x
        self.xs = xs
    }
}

#if os(macOS)
import Foundation

@objc
public class ObjcClass: NSObject {
    public let x: Int

    public init(x: Int) {
        self.x = x
    }

    deinit {
        print("ObjcClass deinitialized!")
    }
}

public struct ObjcWrapper {
    public let x: ObjcClass
    let y: Int = 0

    public init(x: ObjcClass) {
        self.x = x
    }
}

public struct WeakObjcWrapper {
    public weak var x: ObjcClass?
    let y: Int = 0

    public init(x: ObjcClass) {
        self.x = x
    }
}

public struct UnownedObjcWrapper {
    public unowned let x: ObjcClass
    let y: Int = 0

    public init(x: ObjcClass) {
        self.x = x
    }
}
#endif

public struct Wrapper<T> {
    public let x: T
    let y: Int = 0

    public init(x: T) {
        self.x = x
    }
}

struct InternalGeneric<T> {
    let x: T
    let y: Int
}

public enum SinglePayloadSimpleClassEnum {
    case empty0
    case empty1
    case empty2
    case empty3
    case empty4
    case empty5
    case empty6
    case nonEmpty(SimpleClass)
}

public struct ContainsSinglePayloadSimpleClassEnum {
    public let x: SinglePayloadSimpleClassEnum
    public let y: AnyObject

    public init(x: SinglePayloadSimpleClassEnum, y: AnyObject) {
        self.x = x
        self.y = y
    }
}

public enum SinglePayloadEnum<T> {
    case empty
    case nonEmpty(T?)
}

public struct SinglePayloadEnumWrapper<T> {
    let x: SinglePayloadEnum<SinglePayloadEnum<T>>
    let y: Int

    public init(x: SinglePayloadEnum<SinglePayloadEnum<T>>, y: Int) {
        self.x = x
        self.y = y
    }
}

public enum MultiPayloadEnum {
    case a(String, Int)
    case b(Int, String)
    case c(Bool)
    case d
}

public struct MultiPayloadEnumWrapper {
    let x: MultiPayloadEnum
    let y: AnyObject

    public init(x: MultiPayloadEnum, y: AnyObject) {
        self.x = x
        self.y = y
    }
}

public struct ComplexNesting<A, B, C, D> {
    let pre: Filler = Filler()
    let a: NestedA<A>
    let b: NestedB<B>
    let c: NestedC<C>
    let d: NestedD<D>

    struct Filler {
        let x: Int16 = 23
        let y: Bool = false
    }

    struct NestedA<T> {
        let x: Int = 32
        let y: NestedB<T>
        let z: Bool = false

        init(y: T) {
            self.y = NestedB(y: y)
        }
    }

    struct NestedB<T> {
        let x: Bool = false
        let y: NestedC<T>
        let z: Int = 32

        init(y: T) {
            self.y = NestedC(y: y)
        }
    }

    enum NestedC<T> {
        case a(Int, T, Bool)
        case b(Int, Bool)
        case c

        init(y: T) {
            self = .a(32, y, false)
        }
    }

    struct NestedD<T> {
        let x: Bool = false
        let y: T
        let z: Int = 32
    }

    public init(_ a: A, _ b: B, _ c: C, _ d: D) {
        self.a = NestedA(y: a)
        self.b = NestedB(y: b)
        self.c = NestedC(y: c)
        self.d = NestedD(y: d)
    }
}

internal enum InternalEnum {
  case a(Int, AnyObject)
  case b(Int)
  case c(String)
}

public struct InternalEnumWrapper {
  internal let x: InternalEnum
  internal let y: Int = 32

  public init(x: AnyObject) {
    self.x = .a(23, x)
  }
}

public struct PrespecializedStruct<T> {
    let y: Int = 0
    let x: T
    let z: T

    public init(x: T) {
        self.x = x
        self.z = x
    }
}

@inline(never)
public func consume<T>(_ x: T.Type) {
    withExtendedLifetime(x) {}
}
public func preSpec() {
    consume(PrespecializedStruct<AnyObject>.self)
    consume(PrespecializedStruct<SimpleClass>.self)
    consume(PrespecializedStruct<Int>.self)
}

@inline(never)
public func testAssign<T>(_ ptr: UnsafeMutablePointer<T>, from x: T) {
    ptr.pointee = x
}

@inline(never)
public func testInit<T>(_ ptr: UnsafeMutablePointer<T>, to x: T) {
    ptr.initialize(to: x)
}

@inline(never)
public func testDestroy<T>(_ ptr: UnsafeMutablePointer<T>) {
    ptr.deinitialize(count: 1)
}

@inline(never)
public func allocateInternalGenericPtr<T>(of tpe: T.Type) -> UnsafeMutableRawPointer {
    return UnsafeMutableRawPointer(
        UnsafeMutablePointer<InternalGeneric<T>>.allocate(capacity: 1))
}

@inline(never)
public func testGenericAssign<T>(_ ptr: __owned UnsafeMutableRawPointer, from x: T) {
    let ptr = ptr.assumingMemoryBound(to: InternalGeneric<T>.self)
    let x = InternalGeneric(x: x, y: 23)
    testAssign(ptr, from: x)
}

@inline(never)
public func testGenericInit<T>(_ ptr: __owned UnsafeMutableRawPointer, to x: T) {
    let ptr = ptr.assumingMemoryBound(to: InternalGeneric<T>.self)
    let x = InternalGeneric(x: x, y: 23)
    testInit(ptr, to: x)
}

@inline(never)
public func testGenericDestroy<T>(_ ptr: __owned UnsafeMutableRawPointer, of tpe: T.Type) {
    let ptr = ptr.assumingMemoryBound(to: tpe)
    testDestroy(ptr)
}
