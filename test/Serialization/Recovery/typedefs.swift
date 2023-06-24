// RUN: %empty-directory(%t)

// Cannot use -parse-as-library here because that would compile also the
// #if VERIFY path, which contains top-level code.
// RUN: %target-swift-frontend -emit-sil -o - -emit-module-path %t/Lib.swiftmodule -module-name Lib -I %S/Inputs/custom-modules -disable-objc-attr-requires-foundation-module -enable-objc-interop %s | %FileCheck -check-prefix CHECK-VTABLE %s

// RUN: %target-swift-ide-test -source-filename=x -print-module -module-to-print Lib -I %t -I %S/Inputs/custom-modules | %FileCheck %s

// RUN: %target-swift-ide-test -source-filename=x -print-module -module-to-print Lib -I %t -I %S/Inputs/custom-modules -Xcc -DBAD > %t.txt
// RUN: %FileCheck -check-prefix CHECK-RECOVERY %s < %t.txt
// RUN: %FileCheck -check-prefix CHECK-RECOVERY-NEGATIVE %s < %t.txt

// RUN: %target-swift-frontend -typecheck -I %t -I %S/Inputs/custom-modules -Xcc -DBAD -DTEST -DVERIFY %s -verify
// RUN: %target-swift-frontend -emit-silgen -I %t -I %S/Inputs/custom-modules -Xcc -DBAD -DTEST %s | %FileCheck -check-prefix CHECK-SIL %s

// RUN: %target-swift-frontend -emit-ir -I %t -I %S/Inputs/custom-modules -DTEST %s | %FileCheck --check-prefixes=CHECK-IR,CHECK-IR-%target-runtime %s
// RUN: %target-swift-frontend -emit-ir -I %t -I %S/Inputs/custom-modules -Xcc -DBAD -DTEST %s | %FileCheck --check-prefixes=CHECK-IR,CHECK-IR-%target-runtime %s

// RUN: %target-swift-frontend -typecheck -I %t -I %S/Inputs/custom-modules -Xcc -DBAD %S/Inputs/typedefs-helper.swift -verify

#if TEST

import Typedefs
import Lib

// CHECK-SIL-LABEL: sil hidden [ossa] @$s8typedefs11testSymbolsyyF
func testSymbols() {
  // Check that the symbols are not using 'Bool'.
  // CHECK-SIL: global_addr @$s3Lib9usesAssocs5Int32VSgvp
  _ = Lib.x
  // CHECK-SIL: global_addr @$s3Lib1xs5Int32Vvp
  _ = Lib.usesAssoc
} // CHECK-SIL: end sil function '$s8typedefs11testSymbolsyyF'

// CHECK-IR-LABEL: define{{.*}} void @"$s8typedefs18testVTableBuilding4usery3Lib4UserC_tF
public func testVTableBuilding(user: User) {
  // The important thing in this CHECK line is the "i64 28", which is the offset
  // for the vtable slot for 'lastMethod()'. If the layout here
  // changes, please check that offset is still correct.
  // CHECK-IR-NOT: ret
  // CHECK-IR-objc: getelementptr inbounds ptr, ptr %{{[0-9]+}}, {{i64 28|i32 31}}
  // CHECK-IR-native: getelementptr inbounds ptr, ptr %{{[0-9]+}}, {{i64 25|i32 28}}
  user.lastMethod()
} // CHECK-IR: ret void

#if VERIFY
let _: String = useAssoc(ImportedType.self) // expected-error {{cannot convert value of type 'Int32?' to specified type 'String'}}
let _: Bool? = useAssoc(ImportedType.self) // expected-error {{cannot convert value of type 'Int32?' to specified type 'Bool?'}}
let _: Int32? = useAssoc(ImportedType.self)

let _: String = useAssoc(AnotherType.self) // expected-error {{cannot convert value of type 'AnotherType.Assoc?' (aka 'Optional<Int32>') to specified type 'String'}}
let _: Bool? = useAssoc(AnotherType.self) // expected-error {{cannot convert value of type 'AnotherType.Assoc?' (aka 'Optional<Int32>') to specified type 'Bool?'}}
let _: Int32? = useAssoc(AnotherType.self)

let _ = wrapped // expected-error {{cannot find 'wrapped' in scope}}
let _ = unwrapped // okay

_ = usesWrapped(nil) // expected-error {{cannot find 'usesWrapped' in scope}}
                     // expected-error@-1 {{'nil' requires a contextual type}}
_ = usesUnwrapped(nil) // expected-error {{'nil' is not compatible with expected argument type 'Int32'}}

let _: WrappedAlias = nil // expected-error {{cannot find type 'WrappedAlias' in scope}}
let _: UnwrappedAlias = nil // expected-error {{'nil' cannot initialize specified type 'UnwrappedAlias' (aka 'Int32')}} expected-note {{add '?'}}

let _: ConstrainedWrapped<Int> = nil // expected-error {{cannot find type 'ConstrainedWrapped' in scope}}
let _: ConstrainedUnwrapped<Int> = nil // expected-error {{type 'Int' does not conform to protocol 'HasAssoc'}}

func testExtensions(wrapped: WrappedInt, unwrapped: UnwrappedInt) {
  wrapped.wrappedMethod() // expected-error {{value of type 'WrappedInt' (aka 'Int32') has no member 'wrappedMethod'}}
  unwrapped.unwrappedMethod() // expected-error {{value of type 'UnwrappedInt' has no member 'unwrappedMethod'}}

  ***wrapped // This one works because of the UnwrappedInt extension.
  ***unwrapped // expected-error {{cannot convert value of type 'UnwrappedInt' to expected argument type 'Int32'}}

  let _: WrappedProto = wrapped // expected-error {{value of type 'WrappedInt' (aka 'Int32') does not conform to specified type 'WrappedProto'}}
  let _: UnwrappedProto = unwrapped // expected-error {{value of type 'UnwrappedInt' does not conform to specified type 'UnwrappedProto'}}
}

public class UserDynamicSub: UserDynamic {
  override init() {}
}
// FIXME: Bad error message; really it's that the convenience init hasn't been
// inherited.
_ = UserDynamicSub(conveniently: 0) // expected-error {{argument passed to call that takes no arguments}}

public class UserDynamicConvenienceSub: UserDynamicConvenience {
  override init() {}
}
_ = UserDynamicConvenienceSub(conveniently: 0)

public class UserSub : User {}
// expected-error@-1 {{cannot inherit from class 'User' because it has overridable members that could not be loaded}}
// expected-note@-2 {{could not deserialize 'wrappedProp'}}
// expected-note@-3 {{could not deserialize 'returnsWrappedMethod()'}}
// expected-note@-4 {{could not deserialize 'constrainedWrapped'}}
// expected-note@-5 {{could not deserialize 'subscript(_:)'}}
// expected-note@-6 {{could not deserialize 'subscript(_:)'}}
// expected-note@-7 {{could not deserialize 'init(wrapped:)'}}
// expected-note@-8 {{could not deserialize 'init(generic:)'}}
// expected-note@-9 {{could not deserialize 'init(wrappedRequired:)'}}
// expected-note@-10 {{could not deserialize 'init(wrappedRequiredInSub:)'}}
// expected-note@-11 {{could not deserialize 'init(wrappedDynamic:)'}}
// expected-note@-12 {{could not deserialize 'init(wrappedRequiredDynamic:)'}}

#endif // VERIFY

#else // TEST

import Typedefs

prefix operator ***

// CHECK-LABEL: class User {
// CHECK-RECOVERY-LABEL: class User {
open class User {
  // CHECK: var unwrappedProp: UnwrappedInt?
  // CHECK-RECOVERY: var unwrappedProp: Int32?
  public var unwrappedProp: UnwrappedInt?
  // CHECK: var wrappedProp: WrappedInt?
  // CHECK_RECOVERY: /* placeholder for wrappedProp (vtable entries: 3) (field offsets: 1) */
  public var wrappedProp: WrappedInt?

  // CHECK: func returnsUnwrappedMethod() -> UnwrappedInt
  // CHECK-RECOVERY: func returnsUnwrappedMethod() -> Int32
  public func returnsUnwrappedMethod() -> UnwrappedInt { fatalError() }
  // CHECK: func returnsWrappedMethod() -> WrappedInt
  // CHECK-RECOVERY: /* placeholder for returnsWrappedMethod() (vtable entries: 1) */
  public func returnsWrappedMethod() -> WrappedInt { fatalError() }

  // CHECK: func constrainedUnwrapped<T>(_: T) where T : HasAssoc, T.Assoc == Int32
  // CHECK-RECOVERY: func constrainedUnwrapped<T>(_: T) where T : HasAssoc, T.Assoc == Int32
  public func constrainedUnwrapped<T: HasAssoc>(_: T) where T.Assoc == UnwrappedInt { fatalError() }
  // CHECK: func constrainedWrapped<T>(_: T) where T : HasAssoc, T.Assoc == WrappedInt
  // CHECK-RECOVERY: /* placeholder for constrainedWrapped(_:) (vtable entries: 1) */
  public func constrainedWrapped<T: HasAssoc>(_: T) where T.Assoc == WrappedInt { fatalError() }

  // CHECK: subscript(_: WrappedInt) -> () { get }
  // CHECK-RECOVERY: /* placeholder for subscript(_:) (vtable entries: 1) */
  public subscript(_: WrappedInt) -> () { return () }

  // CHECK: subscript<T>(_: T) -> () where T : HasAssoc, T.Assoc == WrappedInt { get }
  // CHECK-RECOVERY: /* placeholder for subscript(_:) (vtable entries: 1) */
  public subscript<T: HasAssoc>(_: T) -> () where T.Assoc == WrappedInt { return () }

  // CHECK: init()
  // CHECK-RECOVERY: init()
  public init() {}

  // CHECK: init(wrapped: WrappedInt)
  // CHECK-RECOVERY: /* placeholder for init(wrapped:) (vtable entries: 1) */
  public init(wrapped: WrappedInt) {}

  // CHECK: convenience init(conveniently: Int)
  // CHECK-RECOVERY: convenience init(conveniently: Int)
  public convenience init(conveniently: Int) { self.init() }

  // CHECK: convenience init<T>(generic: T) where T : HasAssoc, T.Assoc == WrappedInt
  // CHECK-RECOVERY: /* placeholder for init(generic:) */
  public convenience init<T: HasAssoc>(generic: T) where T.Assoc == WrappedInt { self.init() }

  // CHECK: required init(wrappedRequired: WrappedInt)
  // CHECK-RECOVERY: /* placeholder for init(wrappedRequired:) (vtable entries: 1) */
  public required init(wrappedRequired: WrappedInt) {}

  // CHECK: {{^}} init(wrappedRequiredInSub: WrappedInt)
  // CHECK-RECOVERY: /* placeholder for init(wrappedRequiredInSub:) (vtable entries: 1) */
  public init(wrappedRequiredInSub: WrappedInt) {}

  // CHECK: dynamic init(wrappedDynamic: WrappedInt)
  // CHECK-RECOVERY: /* placeholder for init(wrappedDynamic:) */
  @objc public dynamic init(wrappedDynamic: WrappedInt) {}

  // CHECK: dynamic required init(wrappedRequiredDynamic: WrappedInt)
  // CHECK-RECOVERY: /* placeholder for init(wrappedRequiredDynamic:) */
  @objc public dynamic required init(wrappedRequiredDynamic: WrappedInt) {}

  public func lastMethod() {}
}
// CHECK: {{^}$}}
// CHECK-RECOVERY: {{^}$}}

// This is mostly to check when changes are necessary for the CHECK-IR lines
// above.
// CHECK-VTABLE-LABEL: sil_vtable User {
// (10 words of normal class metadata on 64-bit platforms, 13 on 32-bit)
// 10 CHECK-VTABLE-NEXT: #User.unwrappedProp!getter:
// 11 CHECK-VTABLE-NEXT: #User.unwrappedProp!setter:
// 12 CHECK-VTABLE-NEXT: #User.unwrappedProp!modify:
// 13 CHECK-VTABLE-NEXT: #User.wrappedProp!getter:
// 14 CHECK-VTABLE-NEXT: #User.wrappedProp!setter:
// 15 CHECK-VTABLE-NEXT: #User.wrappedProp!modify:
// 16 CHECK-VTABLE-NEXT: #User.returnsUnwrappedMethod:
// 17 CHECK-VTABLE-NEXT: #User.returnsWrappedMethod:
// 18 CHECK-VTABLE-NEXT: #User.constrainedUnwrapped:
// 19 CHECK-VTABLE-NEXT: #User.constrainedWrapped:
// 20 CHECK-VTABLE-NEXT: #User.subscript!getter:
// 21 CHECK-VTABLE-NEXT: #User.subscript!getter:
// 22 CHECK-VTABLE-NEXT: #User.init!allocator:
// 23 CHECK-VTABLE-NEXT: #User.init!allocator:
// 24 CHECK-VTABLE-NEXT: #User.init!allocator:
// 25 CHECK-VTABLE-NEXT: #User.init!allocator:
// 26 CHECK-VTABLE-NEXT: #User.lastMethod:
// CHECK-VTABLE: }


// CHECK-LABEL: class UserConvenience
// CHECK-RECOVERY-LABEL: class UserConvenience
open class UserConvenience {
  // CHECK: init()
  // CHECK-RECOVERY: init()
  public init() {}

  // CHECK: convenience init(wrapped: WrappedInt)
  // CHECK-RECOVERY: /* placeholder for init(wrapped:) */
  public convenience init(wrapped: WrappedInt) { self.init() }

  // CHECK: convenience init(conveniently: Int)
  // CHECK-RECOVERY: convenience init(conveniently: Int)
  public convenience init(conveniently: Int) { self.init() }
}
// CHECK: {{^}$}}
// CHECK-RECOVERY: {{^}$}}

// CHECK-LABEL: class UserDynamic
// CHECK-RECOVERY-LABEL: class UserDynamic
open class UserDynamic {
  // CHECK: init()
  // CHECK-RECOVERY: init()
  @objc public dynamic init() {}

  // CHECK: init(wrapped: WrappedInt)
  // CHECK-RECOVERY: /* placeholder for init(wrapped:) */
  @objc public dynamic init(wrapped: WrappedInt) {}

  // CHECK: convenience init(conveniently: Int)
  // CHECK-RECOVERY: convenience init(conveniently: Int)
  @objc public dynamic convenience init(conveniently: Int) { self.init() }
}
// CHECK: {{^}$}}
// CHECK-RECOVERY: {{^}$}}

// CHECK-LABEL: class UserDynamicConvenience
// CHECK-RECOVERY-LABEL: class UserDynamicConvenience
open class UserDynamicConvenience {
  // CHECK: init()
  // CHECK-RECOVERY: init()
  @objc public dynamic init() {}

  // CHECK: convenience init(wrapped: WrappedInt)
  // CHECK-RECOVERY: /* placeholder for init(wrapped:) */
  @objc public dynamic convenience init(wrapped: WrappedInt) { self.init() }

  // CHECK: convenience init(conveniently: Int)
  // CHECK-RECOVERY: convenience init(conveniently: Int)
  @objc public dynamic convenience init(conveniently: Int) { self.init() }
}
// CHECK: {{^}$}}
// CHECK-RECOVERY: {{^}$}}


// CHECK-LABEL: class UserSub
// CHECK-RECOVERY-LABEL: class UserSub
open class UserSub : User {
  // CHECK: init(wrapped: WrappedInt?)
  // CHECK-RECOVERY: /* placeholder for init(wrapped:) (vtable entries: 1) */
  public override init(wrapped: WrappedInt?) { super.init() }

  // CHECK: required init(wrappedRequired: WrappedInt?)
  // CHECK-RECOVERY: /* placeholder for init(wrappedRequired:) (vtable entries: 1) */
  public required init(wrappedRequired: WrappedInt?) { super.init() }

  // CHECK: required init(wrappedRequiredInSub: WrappedInt?)
  // CHECK-RECOVERY: /* placeholder for init(wrappedRequiredInSub:) (vtable entries: 1) */
  public required override init(wrappedRequiredInSub: WrappedInt?) { super.init() }

  // CHECK: required init(wrappedRequiredDynamic: WrappedInt)
  // CHECK-RECOVERY: /* placeholder for init(wrappedRequiredDynamic:) */
  public required init(wrappedRequiredDynamic: WrappedInt) { super.init() }
}
// CHECK: {{^}$}}
// CHECK-RECOVERY: {{^}$}}


// CHECK-DAG: let x: MysteryTypedef
// CHECK-RECOVERY-DAG: let x: Int32
public let x: MysteryTypedef = 0

public protocol HasAssoc {
  associatedtype Assoc
}

extension ImportedType: HasAssoc {}

public struct AnotherType: HasAssoc {
  public typealias Assoc = MysteryTypedef
}

public func useAssoc<T: HasAssoc>(_: T.Type) -> T.Assoc? { return nil }

// CHECK-DAG: let usesAssoc: ImportedType.Assoc?
// CHECK-RECOVERY-DAG: let usesAssoc: Int32?
public let usesAssoc = useAssoc(ImportedType.self)
// CHECK-DAG: let usesAssoc2: AnotherType.Assoc?
// CHECK-RECOVERY-DAG: let usesAssoc2: AnotherType.Assoc?
public let usesAssoc2 = useAssoc(AnotherType.self)


// CHECK-DAG: let wrapped: WrappedInt
// CHECK-RECOVERY-NEGATIVE-NOT: let wrapped:
public let wrapped = WrappedInt(0)
// CHECK-DAG: let unwrapped: UnwrappedInt
// CHECK-RECOVERY-DAG: let unwrapped: Int32
public let unwrapped: UnwrappedInt = 0

// CHECK-DAG: let wrappedMetatype: WrappedInt.Type
// CHECK-RECOVERY-NEGATIVE-NOT: let wrappedMetatype:
public let wrappedMetatype = WrappedInt.self
// CHECK-DAG: let wrappedOptional: WrappedInt?
// CHECK-RECOVERY-NEGATIVE-NOT: let wrappedOptional:
public let wrappedOptional: WrappedInt? = nil
// CHECK-DAG: let wrappedIUO: WrappedInt!
// CHECK-RECOVERY-NEGATIVE-NOT: let wrappedIUO:
public let wrappedIUO: WrappedInt! = nil
// CHECK-DAG: let wrappedArray: [WrappedInt]
// CHECK-RECOVERY-NEGATIVE-NOT: let wrappedArray:
public let wrappedArray: [WrappedInt] = []
// CHECK-DAG: let wrappedDictionary: [Int : WrappedInt]
// CHECK-RECOVERY-NEGATIVE-NOT: let wrappedDictionary:
public let wrappedDictionary: [Int: WrappedInt] = [:]
// CHECK-DAG: let wrappedTuple: (WrappedInt, Int)?
// CHECK-RECOVERY-NEGATIVE-NOT: let wrappedTuple:
public let wrappedTuple: (WrappedInt, Int)? = nil
// CHECK-DAG: let wrappedTuple2: (Int, WrappedInt)?
// CHECK-RECOVERY-NEGATIVE-NOT: let wrappedTuple2:
public let wrappedTuple2: (Int, WrappedInt)? = nil
// CHECK-DAG: let wrappedClosure: ((WrappedInt) -> Void)?
// CHECK-RECOVERY-NEGATIVE-NOT: let wrappedClosure:
public let wrappedClosure: ((WrappedInt) -> Void)? = nil
// CHECK-DAG: let wrappedClosure2: (() -> WrappedInt)?
// CHECK-RECOVERY-NEGATIVE-NOT: let wrappedClosure2:
public let wrappedClosure2: (() -> WrappedInt)? = nil
// CHECK-DAG: let wrappedClosure3: ((Int, WrappedInt) -> Void)?
// CHECK-RECOVERY-NEGATIVE-NOT: let wrappedClosure3:
public let wrappedClosure3: ((Int, WrappedInt) -> Void)? = nil
// CHECK-DAG: let wrappedClosureInout: ((inout WrappedInt) -> Void)?
// CHECK-RECOVERY-NEGATIVE-NOT: let wrappedClosureInout:
public let wrappedClosureInout: ((inout WrappedInt) -> Void)? = nil


// CHECK-DAG: var wrappedFirst: WrappedInt?
// CHECK-DAG: var normalSecond: Int?
// CHECK-RECOVERY-NEGATIVE-NOT: var wrappedFirst:
// CHECK-RECOVERY-DAG: var normalSecond: Int?
public var wrappedFirst: WrappedInt?, normalSecond: Int?
// CHECK-DAG: var normalFirst: Int?
// CHECK-DAG: var wrappedSecond: WrappedInt?
// CHECK-RECOVERY-DAG: var normalFirst: Int?
// CHECK-RECOVERY-NEGATIVE-NOT: var wrappedSecond:
public var normalFirst: Int?, wrappedSecond: WrappedInt?
// CHECK-DAG: var wrappedThird: WrappedInt?
// CHECK-DAG: var wrappedFourth: WrappedInt?
// CHECK-RECOVERY-NEGATIVE-NOT: var wrappedThird:
// CHECK-RECOVERY-NEGATIVE-NOT: var wrappedFourth:
public var wrappedThird, wrappedFourth: WrappedInt?

// CHECK-DAG: func usesWrapped(_ wrapped: WrappedInt)
// CHECK-RECOVERY-NEGATIVE-NOT: func usesWrapped(
public func usesWrapped(_ wrapped: WrappedInt) {}
// CHECK-DAG: func usesUnwrapped(_ unwrapped: UnwrappedInt)
// CHECK-RECOVERY-DAG: func usesUnwrapped(_ unwrapped: Int32)
public func usesUnwrapped(_ unwrapped: UnwrappedInt) {}

// CHECK-DAG: func returnsWrapped() -> WrappedInt
// CHECK-RECOVERY-NEGATIVE-NOT: func returnsWrapped(
public func returnsWrapped() -> WrappedInt { fatalError() }

// CHECK-DAG: func returnsWrappedGeneric<T>(_: T.Type) -> WrappedInt
// CHECK-RECOVERY-NEGATIVE-NOT: func returnsWrappedGeneric<
public func returnsWrappedGeneric<T>(_: T.Type) -> WrappedInt { fatalError() }

public protocol WrappedProto {}
public protocol UnwrappedProto {}

public typealias WrappedAlias = WrappedInt
public typealias UnwrappedAlias = UnwrappedInt

public typealias ConstrainedWrapped<T: HasAssoc> = T where T.Assoc == WrappedInt
public typealias ConstrainedUnwrapped<T: HasAssoc> = T where T.Assoc == UnwrappedInt

// CHECK-LABEL: extension Int32 : UnwrappedProto {
// CHECK-NEXT: func unwrappedMethod()
// CHECK-NEXT: prefix static func *** (x: UnwrappedInt)
// CHECK-NEXT: }
// CHECK-RECOVERY-LABEL: extension Int32 : UnwrappedProto {
// CHECK-RECOVERY-NEXT: func unwrappedMethod()
// CHECK-RECOVERY-NEXT: prefix static func *** (x: Int32)
// CHECK-RECOVERY-NEXT: }
// CHECK-RECOVERY-NEGATIVE-NOT: extension UnwrappedInt
extension UnwrappedInt: UnwrappedProto {
  public func unwrappedMethod() {}
  public static prefix func ***(x: UnwrappedInt) {}
}

// CHECK-LABEL: extension WrappedInt : WrappedProto {
// CHECK-NEXT: func wrappedMethod()
// CHECK-NEXT: prefix static func *** (x: WrappedInt)
// CHECK-NEXT: }
// CHECK-RECOVERY-NEGATIVE-NOT: extension WrappedInt
extension WrappedInt: WrappedProto {
  public func wrappedMethod() {}
  public static prefix func ***(x: WrappedInt) {}
}

#endif // TEST
