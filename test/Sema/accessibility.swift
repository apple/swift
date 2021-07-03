// RUN: %target-typecheck-verify-swift -enable-objc-interop -disable-objc-attr-requires-foundation-module -swift-version 5 -enable-access-control-hacks

public protocol PublicProto {
  func publicReq()
}

// expected-note@+1 * {{type declared here}}
internal protocol InternalProto {
  func internalReq()
}

fileprivate protocol FilePrivateProto {
  func filePrivateReq()
}

// expected-note@+1 * {{type declared here}}
private protocol PrivateProto {
  func privateReq()
}

public struct PublicStruct: PublicProto, InternalProto, FilePrivateProto, PrivateProto {
  private func publicReq() {} // expected-error {{method 'publicReq()' must be declared public because it matches a requirement in public protocol 'PublicProto'}} {{none}} expected-note {{mark the instance method as 'public' to satisfy the requirement}} {{3-10=public}}
  private func internalReq() {} // expected-error {{method 'internalReq()' must be declared internal because it matches a requirement in internal protocol 'InternalProto'}} {{none}} expected-note {{mark the instance method as 'internal' to satisfy the requirement}} {{3-10=internal}}
  private func filePrivateReq() {} // expected-error {{method 'filePrivateReq()' must be declared fileprivate because it matches a requirement in fileprivate protocol 'FilePrivateProto'}} {{none}} expected-note {{mark the instance method as 'fileprivate' to satisfy the requirement}} {{3-10=fileprivate}}
  private func privateReq() {} // expected-error {{method 'privateReq()' must be declared fileprivate because it matches a requirement in private protocol 'PrivateProto'}} {{none}} expected-note {{mark the instance method as 'fileprivate' to satisfy the requirement}} {{3-10=fileprivate}}

  public var publicVar = 0
}

// expected-note@+1 * {{type declared here}}
internal struct InternalStruct: PublicProto, InternalProto, FilePrivateProto, PrivateProto {
  private func publicReq() {} // expected-error {{method 'publicReq()' must be as accessible as its enclosing type because it matches a requirement in protocol 'PublicProto'}} {{none}} expected-note {{mark the instance method as 'internal' to satisfy the requirement}} {{3-10=internal}}
  private func internalReq() {} // expected-error {{method 'internalReq()' must be declared internal because it matches a requirement in internal protocol 'InternalProto'}} {{none}} expected-note {{mark the instance method as 'internal' to satisfy the requirement}} {{3-10=internal}}
  private func filePrivateReq() {} // expected-error {{method 'filePrivateReq()' must be declared fileprivate because it matches a requirement in fileprivate protocol 'FilePrivateProto'}} {{none}} expected-note {{mark the instance method as 'fileprivate' to satisfy the requirement}} {{3-10=fileprivate}}
  private func privateReq() {} // expected-error {{method 'privateReq()' must be declared fileprivate because it matches a requirement in private protocol 'PrivateProto'}} {{none}} expected-note {{mark the instance method as 'fileprivate' to satisfy the requirement}} {{3-10=fileprivate}}

  public var publicVar = 0
}

// expected-note@+1 * {{type declared here}}
fileprivate struct FilePrivateStruct: PublicProto, InternalProto, FilePrivateProto, PrivateProto {
  private func publicReq() {} // expected-error {{method 'publicReq()' must be as accessible as its enclosing type because it matches a requirement in protocol 'PublicProto'}} {{none}} expected-note {{mark the instance method as 'fileprivate' to satisfy the requirement}} {{3-10=fileprivate}}
  private func internalReq() {} // expected-error {{method 'internalReq()' must be as accessible as its enclosing type because it matches a requirement in protocol 'InternalProto'}} {{none}} expected-note {{mark the instance method as 'fileprivate' to satisfy the requirement}} {{3-10=fileprivate}}
  private func filePrivateReq() {} // expected-error {{method 'filePrivateReq()' must be declared fileprivate because it matches a requirement in fileprivate protocol 'FilePrivateProto'}} {{none}} expected-note {{mark the instance method as 'fileprivate' to satisfy the requirement}} {{3-10=fileprivate}}
  private func privateReq() {} // expected-error {{method 'privateReq()' must be declared fileprivate because it matches a requirement in private protocol 'PrivateProto'}} {{none}} expected-note {{mark the instance method as 'fileprivate' to satisfy the requirement}} {{3-10=fileprivate}}

  public var publicVar = 0
}

// expected-note@+1 * {{type declared here}}
private struct PrivateStruct: PublicProto, InternalProto, FilePrivateProto, PrivateProto {
  private func publicReq() {} // expected-error {{method 'publicReq()' must be as accessible as its enclosing type because it matches a requirement in protocol 'PublicProto'}} {{none}} expected-note {{mark the instance method as 'fileprivate' to satisfy the requirement}} {{3-10=fileprivate}}
  private func internalReq() {} // expected-error {{method 'internalReq()' must be as accessible as its enclosing type because it matches a requirement in protocol 'InternalProto'}} {{none}} expected-note {{mark the instance method as 'fileprivate' to satisfy the requirement}} {{3-10=fileprivate}}
  private func filePrivateReq() {} // expected-error {{method 'filePrivateReq()' must be declared fileprivate because it matches a requirement in fileprivate protocol 'FilePrivateProto'}} {{none}} expected-note {{mark the instance method as 'fileprivate' to satisfy the requirement}} {{3-10=fileprivate}}
  private func privateReq() {} // expected-error {{method 'privateReq()' must be declared fileprivate because it matches a requirement in private protocol 'PrivateProto'}} {{none}} expected-note {{mark the instance method as 'fileprivate' to satisfy the requirement}} {{3-10=fileprivate}}

  public var publicVar = 0
}

public struct PublicStructWithInternalExtension: PublicProto, InternalProto, FilePrivateProto, PrivateProto {}
// expected-error@-1 {{method 'publicReq()' must be declared public because it matches a requirement in public protocol 'PublicProto'}} {{none}}
// expected-error@-2 {{method 'internalReq()' must be declared internal because it matches a requirement in internal protocol 'InternalProto'}} {{none}}
// expected-error@-3 {{method 'filePrivateReq()' must be declared fileprivate because it matches a requirement in fileprivate protocol 'FilePrivateProto'}} {{none}}
// expected-error@-4 {{method 'privateReq()' must be declared fileprivate because it matches a requirement in private protocol 'PrivateProto'}} {{none}}
internal extension PublicStructWithInternalExtension {
  private func publicReq() {} // expected-note {{move the instance method to another extension where it can be declared 'public' to satisfy the requirement}} {{none}}
  private func internalReq() {} // expected-note {{mark the instance method as 'internal' to satisfy the requirement}} {{3-11=}}
  private func filePrivateReq() {} // expected-note {{mark the instance method as 'fileprivate' to satisfy the requirement}} {{3-10=fileprivate}}
  private func privateReq() {} // expected-note {{mark the instance method as 'fileprivate' to satisfy the requirement}} {{3-10=fileprivate}}
}

extension PublicStruct {
  public init(x: Int) { self.init() }
}

extension InternalStruct {
  public init(x: Int) { self.init() }
}

extension FilePrivateStruct {
  public init(x: Int) { self.init() }
}

extension PrivateStruct {
  public init(x: Int) { self.init() }
}

public extension PublicStruct {
  public func extMemberPublic() {} // expected-warning {{'public' modifier is redundant for instance method declared in a public extension}} {{3-10=}}
  fileprivate func extFuncPublic() {}
  private func extImplPublic() {}
}
internal extension PublicStruct {
  public func extMemberInternal() {} // expected-warning {{'public' modifier conflicts with extension's default access of 'internal'}} {{none}}
  fileprivate func extFuncInternal() {}
  private func extImplInternal() {}
}
fileprivate extension PublicStruct {
  public func extMemberFilePrivate() {} // expected-warning {{'public' modifier conflicts with extension's default access of 'fileprivate'}} {{none}}
  fileprivate func extFuncFilePrivate() {} // expected-warning {{'fileprivate' modifier is redundant for instance method declared in a fileprivate extension}} {{3-15=}}
  private func extImplFilePrivate() {}
}
private extension PublicStruct {
  public func extMemberPrivate() {} // expected-warning {{'public' modifier conflicts with extension's default access of 'private'}} {{none}}
  fileprivate func extFuncPrivate() {} // expected-warning {{'fileprivate' modifier is redundant for instance method declared in a private (equivalent to fileprivate) extension}} {{3-15=}}
  private func extImplPrivate() {}
}
public extension InternalStruct { // expected-error {{extension of internal struct cannot be declared public}} {{1-8=}}
  public func extMemberPublic() {}
  fileprivate func extFuncPublic() {}
  private func extImplPublic() {}
}
internal extension InternalStruct {
  public func extMemberInternal() {} // expected-warning {{'public' modifier conflicts with extension's default access of 'internal'}} {{none}}
  fileprivate func extFuncInternal() {}
  private func extImplInternal() {}
}
fileprivate extension InternalStruct {
  public func extMemberFilePrivate() {} // expected-warning {{'public' modifier conflicts with extension's default access of 'fileprivate'}} {{none}}
  fileprivate func extFuncFilePrivate() {} // expected-warning {{'fileprivate' modifier is redundant for instance method declared in a fileprivate extension}} {{3-15=}}
  private func extImplFilePrivate() {}
}
private extension InternalStruct {
  public func extMemberPrivate() {} // expected-warning {{'public' modifier conflicts with extension's default access of 'private'}} {{none}}
  fileprivate func extFuncPrivate() {} // expected-warning {{'fileprivate' modifier is redundant for instance method declared in a private (equivalent to fileprivate) extension}} {{3-15=}}
  private func extImplPrivate() {}
}
public extension FilePrivateStruct { // expected-error {{extension of fileprivate struct cannot be declared public}} {{1-8=}}
  public func extMemberPublic() {}
  fileprivate func extFuncPublic() {}
  private func extImplPublic() {}
}
internal extension FilePrivateStruct { // expected-error {{extension of fileprivate struct cannot be declared internal}} {{1-10=}}
  public func extMemberInternal() {}
  fileprivate func extFuncInternal() {}
  private func extImplInternal() {}
}
fileprivate extension FilePrivateStruct {
  public func extMemberFilePrivate() {} // expected-warning {{'public' modifier conflicts with extension's default access of 'fileprivate'}} {{none}}
  fileprivate func extFuncFilePrivate() {} // expected-warning {{'fileprivate' modifier is redundant for instance method declared in a fileprivate extension}} {{3-15=}}
  private func extImplFilePrivate() {}
}
private extension FilePrivateStruct {
  public func extMemberPrivate() {} // expected-warning {{'public' modifier conflicts with extension's default access of 'private'}} {{none}}
  fileprivate func extFuncPrivate() {} // expected-warning {{'fileprivate' modifier is redundant for instance method declared in a private (equivalent to fileprivate) extension}} {{3-15=}}
  private func extImplPrivate() {}
}
public extension PrivateStruct { // expected-error {{extension of private struct cannot be declared public}} {{1-8=}}
  public func extMemberPublic() {}
  fileprivate func extFuncPublic() {}
  private func extImplPublic() {}
}
internal extension PrivateStruct { // expected-error {{extension of private struct cannot be declared internal}} {{1-10=}}
  public func extMemberInternal() {}
  fileprivate func extFuncInternal() {}
  private func extImplInternal() {}
}
fileprivate extension PrivateStruct { // expected-error {{extension of private struct cannot be declared fileprivate}} {{1-13=}}
  public func extMemberFilePrivate() {}
  fileprivate func extFuncFilePrivate() {}
  private func extImplFilePrivate() {}
}
private extension PrivateStruct {
  public func extMemberPrivate() {} // expected-warning {{'public' modifier conflicts with extension's default access of 'private'}} {{none}}
  fileprivate func extFuncPrivate() {} // expected-warning {{'fileprivate' modifier is redundant for instance method declared in a private (equivalent to fileprivate) extension}} {{3-15=}}
  private func extImplPrivate() {}
}


public struct PublicStructDefaultMethods: PublicProto, InternalProto, PrivateProto {
  func publicReq() {} // expected-error {{method 'publicReq()' must be declared public because it matches a requirement in public protocol 'PublicProto'}} {{none}} expected-note {{mark the instance method as 'public' to satisfy the requirement}} {{3-3=public }}
  func internalReq() {}
  func privateReq() {}
}


public class Base {
  required public init() {}
  // expected-note@+1 * {{overridden declaration is here}}
  public func foo() {}
  // expected-note@+1 * {{overridden declaration is here}}
  public internal(set) var bar: Int = 0
  // expected-note@+1 * {{overridden declaration is here}}
  public subscript () -> () { return () }
}


public extension Base {
  open func extMemberPublic() {} // expected-warning {{'open' modifier conflicts with extension's default access of 'public'}}
}
internal extension Base {
  open func extMemberInternal() {} // expected-warning {{'open' modifier conflicts with extension's default access of 'internal'}}
}

public class PublicSub: Base {
  private required init() {} // expected-error {{'required' initializer must be accessible wherever class 'PublicSub' can be subclassed}} {{3-10=internal}}
  override func foo() {} // expected-error {{overriding instance method must be as accessible as the declaration it overrides}} {{3-3=public }}
  override var bar: Int { // expected-error {{overriding property must be as accessible as the declaration it overrides}} {{3-3=public }}
    get { return 0 }
    set {}
  }
  override subscript () -> () { return () } // expected-error {{overriding subscript must be as accessible as the declaration it overrides}} {{3-3=public }}
}

public class PublicSubGood: Base {
  required init() {} // okay
}

internal class InternalSub: Base {
  required private init() {} // expected-error {{'required' initializer must be accessible wherever class 'InternalSub' can be subclassed}} {{12-19=internal}}
  private override func foo() {} // expected-error {{overriding instance method must be as accessible as its enclosing type}} {{3-10=internal}}
  private override var bar: Int { // expected-error {{overriding property must be as accessible as its enclosing type}} {{3-10=internal}}
    get { return 0 }
    set {}
  }
  private override subscript () -> () { return () } // expected-error {{overriding subscript must be as accessible as its enclosing type}} {{3-10=internal}}
}

internal class InternalSubGood: Base {
  required init() {} // no-warning
  override func foo() {}
  override var bar: Int {
    get { return 0 }
    set {}
  }
  override subscript () -> () { return () }
}

internal class InternalSubPrivateSet: Base {
  required init() {}
  private(set) override var bar: Int { // expected-error {{overriding property must be as accessible as its enclosing type}} {{3-16=}}
    get { return 0 }
    set {}
  }
  private(set) override subscript () -> () { // okay; read-only in base class
    get { return () }
    set {}
  }
}

fileprivate class FilePrivateSub: Base {
  required private init() {} // expected-error {{'required' initializer must be accessible wherever class 'FilePrivateSub' can be subclassed}} {{12-19=fileprivate}}
  private override func foo() {} // expected-error {{overriding instance method must be as accessible as its enclosing type}} {{3-10=fileprivate}}
  private override var bar: Int { // expected-error {{overriding property must be as accessible as its enclosing type}} {{3-10=fileprivate}}
    get { return 0 }
    set {}
  }
  private override subscript () -> () { return () } // expected-error {{overriding subscript must be as accessible as its enclosing type}} {{3-10=fileprivate}}
}

fileprivate class FilePrivateSubGood: Base {
  required init() {} // no-warning
  override func foo() {}
  override var bar: Int {
    get { return 0 }
    set {}
  }
  override subscript () -> () { return () }
}

fileprivate class FilePrivateSubGood2: Base {
  fileprivate required init() {} // no-warning
  fileprivate override func foo() {}
  fileprivate override var bar: Int {
    get { return 0 }
    set {}
  }
  fileprivate override subscript () -> () { return () }
}

fileprivate class FilePrivateSubPrivateSet: Base {
  required init() {}
  private(set) override var bar: Int { // expected-error {{overriding property must be as accessible as its enclosing type}} {{3-10=fileprivate}}
    get { return 0 }
    set {}
  }
  private(set) override subscript () -> () { // okay; read-only in base class
    get { return () }
    set {}
  }
}

private class PrivateSub: Base {
  required private init() {} // expected-error {{'required' initializer must be accessible wherever class 'PrivateSub' can be subclassed}} {{12-19=fileprivate}}
  private override func foo() {} // expected-error {{overriding instance method must be as accessible as its enclosing type}} {{3-10=fileprivate}}
  private override var bar: Int { // expected-error {{overriding property must be as accessible as its enclosing type}} {{3-10=fileprivate}}
    get { return 0 }
    set {}
  }
  private override subscript () -> () { return () } // expected-error {{overriding subscript must be as accessible as its enclosing type}} {{3-10=fileprivate}}
}

private class PrivateSubGood: Base {
  required fileprivate init() {}
  fileprivate override func foo() {}
  fileprivate override var bar: Int {
    get { return 0 }
    set {}
  }
  fileprivate override subscript () -> () { return () }
}

private class PrivateSubPrivateSet: Base {
  required fileprivate init() {}
  fileprivate override func foo() {}
  private(set) override var bar: Int { // expected-error {{setter of overriding property must be as accessible as its enclosing type}}
    get { return 0 }
    set {}
  }
  private(set) override subscript () -> () { // okay; read-only in base class
    get { return () }
    set {}
  }
}

public typealias PublicTA1 = PublicStruct
public typealias PublicTA2 = InternalStruct // expected-error {{type alias cannot be declared public because its underlying type uses an internal type}}
public typealias PublicTA3 = FilePrivateStruct // expected-error {{type alias cannot be declared public because its underlying type uses a fileprivate type}}
public typealias PublicTA4 = PrivateStruct // expected-error {{type alias cannot be declared public because its underlying type uses a private type}}

// expected-note@+1 {{type declared here}}
internal typealias InternalTA1 = PublicStruct
internal typealias InternalTA2 = InternalStruct
internal typealias InternalTA3 = FilePrivateStruct // expected-error {{type alias cannot be declared internal because its underlying type uses a fileprivate type}}
internal typealias InternalTA4 = PrivateStruct // expected-error {{type alias cannot be declared internal because its underlying type uses a private type}}

public typealias PublicFromInternal = InternalTA1 // expected-error {{type alias cannot be declared public because its underlying type uses an internal type}}

typealias FunctionType1 = (PrivateStruct) -> PublicStruct // expected-error {{type alias must be declared private or fileprivate because its underlying type uses a private type}}
typealias FunctionType2 = (PublicStruct) -> PrivateStruct // expected-error {{type alias must be declared private or fileprivate because its underlying type uses a private type}}
typealias FunctionType3 = (PrivateStruct) -> PrivateStruct // expected-error {{type alias must be declared private or fileprivate because its underlying type uses a private type}}

typealias ArrayType = [PrivateStruct] // expected-error {{type alias must be declared private or fileprivate because its underlying type uses a private type}}
typealias DictType = [String : PrivateStruct] // expected-error {{type alias must be declared private or fileprivate because its underlying type uses a private type}}
typealias GenericArgs = Optional<PrivateStruct> // expected-error {{type alias must be declared private or fileprivate because its underlying type uses a private type}}


public protocol HasAssocType {
  associatedtype Inferred
  func test(input: Inferred)
}

public struct AssocTypeImpl: HasAssocType {
  public func test(input: Bool) {}
}
public let _: AssocTypeImpl.Inferred?


public let x: PrivateStruct = PrivateStruct() // expected-error {{constant cannot be declared public because its type uses a private type}}
public var a: PrivateStruct?, b: PrivateStruct? // expected-error 2 {{variable cannot be declared public because its type uses a private type}}
public var (c, d): (PrivateStruct?, PrivateStruct?) // expected-error {{variable cannot be declared public because its type uses a private type}}

var internalVar: PrivateStruct? // expected-error {{variable must be declared private or fileprivate because its type uses a private type}}

let internalConstant = PrivateStruct() // expected-error {{constant must be declared private or fileprivate because its type 'PrivateStruct' uses a private type}}
public let publicConstant = [InternalStruct]() // expected-error {{constant cannot be declared public because its type '[InternalStruct]' uses an internal type}}

public struct Properties {
  public let x: PrivateStruct = PrivateStruct() // expected-error {{property cannot be declared public because its type uses a private type}}
  public var a: PrivateStruct?, b: PrivateStruct? // expected-error 2 {{property cannot be declared public because its type uses a private type}}
  public var (c, d): (PrivateStruct?, PrivateStruct?) // expected-error {{property cannot be declared public because its type uses a private type}}

  let y = PrivateStruct() // expected-error {{property must be declared fileprivate because its type 'PrivateStruct' uses a private type}}
}

public struct Subscripts {
  subscript (a: PrivateStruct) -> Int { return 0 } // expected-error {{subscript must be declared fileprivate because its index uses a private type}}
  subscript (a: Int) -> PrivateStruct { return PrivateStruct() } // expected-error {{subscript must be declared fileprivate because its element type uses a private type}}

  public subscript (a: PrivateStruct, b: Int) -> Int { return 0 } // expected-error {{subscript cannot be declared public because its index uses a private type}}
  public subscript (a: Int, b: PrivateStruct) -> Int { return 0 } // expected-error {{subscript cannot be declared public because its index uses a private type}}
  public subscript (a: InternalStruct, b: PrivateStruct) -> InternalStruct { return InternalStruct() } // expected-error {{subscript cannot be declared public because its index uses a private type}}
  public subscript (a: PrivateStruct, b: InternalStruct) -> PrivateStruct { return PrivateStruct() } // expected-error {{subscript cannot be declared public because its index uses a private type}}
  public subscript (a: Int, b: Int) -> InternalStruct { return InternalStruct() } // expected-error {{subscript cannot be declared public because its element type uses an internal type}}
}

public struct Methods {
  func foo(a: PrivateStruct) -> Int { return 0 } // expected-error {{method must be declared fileprivate because its parameter uses a private type}}
  func bar(a: Int) -> PrivateStruct { return PrivateStruct() } // expected-error {{method must be declared fileprivate because its result uses a private type}}

  public func a(a: PrivateStruct, b: Int) -> Int { return 0 } // expected-error {{method cannot be declared public because its parameter uses a private type}}
  public func b(a: Int, b: PrivateStruct) -> Int { return 0 } // expected-error {{method cannot be declared public because its parameter uses a private type}}
  public func c(a: InternalStruct, b: PrivateStruct) -> InternalStruct { return InternalStruct() } // expected-error {{method cannot be declared public because its parameter uses a private type}}
  public func d(a: PrivateStruct, b: InternalStruct) -> PrivateStruct { return PrivateStruct() } // expected-error {{method cannot be declared public because its parameter uses a private type}}
  public func e(a: Int, b: Int) -> InternalStruct { return InternalStruct() } // expected-error {{method cannot be declared public because its result uses an internal type}}
}

func privateParam(a: PrivateStruct) {} // expected-error {{function must be declared private or fileprivate because its parameter uses a private type}}

public struct Initializers {
  init(a: PrivateStruct) {} // expected-error {{initializer must be declared fileprivate because its parameter uses a private type}}

  public init(a: PrivateStruct, b: Int) {} // expected-error {{initializer cannot be declared public because its parameter uses a private type}}
  public init(a: Int, b: PrivateStruct) {} // expected-error {{initializer cannot be declared public because its parameter uses a private type}}
  public init(a: InternalStruct, b: PrivateStruct) {} // expected-error {{initializer cannot be declared public because its parameter uses a private type}}
  public init(a: PrivateStruct, b: InternalStruct) { } // expected-error {{initializer cannot be declared public because its parameter uses a private type}}
}


public class PublicClass {}
// expected-note@+2 * {{type declared here}}
// expected-note@+1 * {{superclass is declared here}}
internal class InternalClass {}
// expected-note@+1 * {{type declared here}}
private class PrivateClass {}

public protocol AssocTypes {
  associatedtype Foo

  associatedtype Internal: InternalClass // expected-error {{associated type in a public protocol uses an internal type in its requirement}}
  associatedtype InternalConformer: InternalProto // expected-error {{associated type in a public protocol uses an internal type in its requirement}}

  associatedtype PrivateConformer: PrivateProto // expected-error {{associated type in a public protocol uses a private type in its requirement}}
  associatedtype PI: PrivateProto, InternalProto // expected-error {{associated type in a public protocol uses a private type in its requirement}}
  associatedtype IP: InternalProto, PrivateProto // expected-error {{associated type in a public protocol uses a private type in its requirement}}

  associatedtype PrivateDefault = PrivateStruct // expected-error {{associated type in a public protocol uses a private type in its default definition}}
  associatedtype PublicDefault = PublicStruct
  associatedtype PrivateDefaultConformer: PublicProto = PrivateStruct // expected-error {{associated type in a public protocol uses a private type in its default definition}}
  associatedtype PublicDefaultConformer: PrivateProto = PublicStruct // expected-error {{associated type in a public protocol uses a private type in its requirement}}
  associatedtype PrivatePrivateDefaultConformer: PrivateProto = PrivateStruct // expected-error {{associated type in a public protocol uses a private type in its requirement}}
  associatedtype PublicPublicDefaultConformer: PublicProto = PublicStruct
}

public protocol RequirementTypes {
  var x: PrivateStruct { get } // expected-error {{property cannot be declared public because its type uses a private type}}
  subscript(x: Int) -> InternalStruct { get set } // expected-error {{subscript cannot be declared public because its element type uses an internal type}}
  func foo() -> PrivateStruct // expected-error {{method cannot be declared public because its result uses a private type}}
  init(x: PrivateStruct) // expected-error {{initializer cannot be declared public because its parameter uses a private type}}
}

protocol DefaultRefinesPrivate : PrivateProto {} // expected-error {{protocol must be declared private or fileprivate because it refines a private protocol}}
public protocol PublicRefinesPrivate : PrivateProto {} // expected-error {{public protocol cannot refine a private protocol}}
public protocol PublicRefinesInternal : InternalProto {} // expected-error {{public protocol cannot refine an internal protocol}}
public protocol PublicRefinesPI : PrivateProto, InternalProto {} // expected-error {{public protocol cannot refine a private protocol}}
public protocol PublicRefinesIP : InternalProto, PrivateProto {} // expected-error {{public protocol cannot refine a private protocol}}

private typealias PrivateTypeAlias = PublicProto; // expected-note {{type declared here}}
private typealias PrivateCompoundTypeAlias = PublicProto & AnyObject // expected-note {{type declared here}}

protocol DefaultRefinesPrivateClass: PrivateClass {} // expected-error {{protocol must be declared private or fileprivate because it refines a private class}}
public protocol PublicRefinesPrivateClass: PrivateClass {} // expected-error {{public protocol cannot refine a private class}}
public protocol PublicRefinesPrivateTypeAlias: PrivateTypeAlias {} // expected-error {{public protocol cannot refine a private type alias}}
public protocol PublicRefinesPrivateCompoundTypeAlias: PrivateCompoundTypeAlias {} // expected-error {{public protocol cannot refine a private type alias}}



// expected-note@+1 * {{type declared here}}
private typealias PrivateInt = Int
enum DefaultRawPrivate : PrivateInt { // expected-error {{enum must be declared private or fileprivate because its raw type uses a private type}}
  case A
}

// Note: fileprivate is the most visible valid access level for
// Outer.DefaultRawPrivate, so the diagnostic should say that.
class Outer {
  enum DefaultRawPrivate : PrivateInt { // expected-error {{enum must be declared fileprivate because its raw type uses a private type}}
    case A
  }
}

public enum PublicRawPrivate : PrivateInt { // expected-error {{enum cannot be declared public because its raw type uses a private type}}
  case A
}
public enum MultipleConformance : PrivateProto, PrivateInt { // expected-error {{enum cannot be declared public because its raw type uses a private type}} expected-error {{must appear first}} {{35-35=PrivateInt, }} {{47-59=}}
  case A
  func privateReq() {}
}

open class OpenSubclassInternal : InternalClass {} // expected-error {{class cannot be declared open because its superclass is internal}} expected-error {{superclass 'InternalClass' of open class must be open}}
public class PublicSubclassPublic : PublicClass {}
public class PublicSubclassInternal : InternalClass {} // expected-error {{class cannot be declared public because its superclass is internal}}
public class PublicSubclassPrivate : PrivateClass {} // expected-error {{class cannot be declared public because its superclass is private}}

class DefaultSubclassPublic : PublicClass {}
class DefaultSubclassInternal : InternalClass {}
class DefaultSubclassPrivate : PrivateClass {} // expected-error {{class must be declared private or fileprivate because its superclass is private}}

// expected-note@+1 * {{superclass is declared here}}
public class PublicGenericClass<T> {}
// expected-note@+2 * {{type declared here}}
// expected-note@+1 * {{superclass is declared here}}
internal class InternalGenericClass<T> {}
// expected-note@+1 * {{type declared here}}
private class PrivateGenericClass<T> {}

open class OpenConcreteSubclassInternal : InternalGenericClass<Int> {} // expected-error {{class cannot be declared open because its superclass is internal}} expected-error {{superclass 'InternalGenericClass<Int>' of open class must be open}}
public class PublicConcreteSubclassPublic : PublicGenericClass<Int> {}
public class PublicConcreteSubclassInternal : InternalGenericClass<Int> {} // expected-error {{class cannot be declared public because its superclass is internal}}
public class PublicConcreteSubclassPrivate : PrivateGenericClass<Int> {} // expected-error {{class cannot be declared public because its superclass is private}}
public class PublicConcreteSubclassPublicPrivateArg : PublicGenericClass<PrivateStruct> {} // expected-error {{class cannot be declared public because its superclass uses a private type as a generic parameter}}
public class PublicConcreteSubclassPublicInternalArg : PublicGenericClass<InternalStruct> {} // expected-error {{class cannot be declared public because its superclass uses an internal type as a generic parameter}}
open class OpenConcreteSubclassPublicFilePrivateArg : PublicGenericClass<FilePrivateStruct> {} // expected-error {{class cannot be declared open because its superclass uses a fileprivate type as a generic parameter}} expected-error {{superclass 'PublicGenericClass<FilePrivateStruct>' of open class must be open}}
internal class InternalConcreteSubclassPublicFilePrivateArg : InternalGenericClass<PrivateStruct> {} // expected-error {{class cannot be declared internal because its superclass uses a private type as a generic parameter}}

open class OpenGenericSubclassInternal<T> : InternalGenericClass<T> {} // expected-error {{class cannot be declared open because its superclass is internal}} expected-error {{superclass 'InternalGenericClass<T>' of open class must be open}}
public class PublicGenericSubclassPublic<T> : PublicGenericClass<T> {}
public class PublicGenericSubclassInternal<T> : InternalGenericClass<T> {} // expected-error {{class cannot be declared public because its superclass is internal}}
public class PublicGenericSubclassPrivate<T> : PrivateGenericClass<T> {} // expected-error {{class cannot be declared public because its superclass is private}}



public enum PublicEnumPrivate {
  case A(PrivateStruct) // expected-error {{enum case in a public enum uses a private type}}
}
enum DefaultEnumPrivate {
  case A(PrivateStruct) // expected-error {{enum case in an internal enum uses a private type}}
}
public enum PublicEnumPI {
  case A(InternalStruct) // expected-error {{enum case in a public enum uses an internal type}}
  case B(PrivateStruct, InternalStruct) // expected-error {{enum case in a public enum uses a private type}} expected-error {{enum case in a public enum uses an internal type}}
  case C(InternalStruct, PrivateStruct) // expected-error {{enum case in a public enum uses an internal type}} expected-error {{enum case in a public enum uses a private type}}
}
enum DefaultEnumPublic {
  case A(PublicStruct) // no-warning
}


struct DefaultGeneric<T> {}

struct DefaultGenericPrivate<T: PrivateProto> {} // expected-error {{generic struct must be declared private or fileprivate because its generic parameter uses a private type}}
struct DefaultGenericPrivate2<T: PrivateClass> {} // expected-error {{generic struct must be declared private or fileprivate because its generic parameter uses a private type}}
struct DefaultGenericPrivateReq<T> where T == PrivateClass {} // expected-error  {{same-type requirement makes generic parameter 'T' non-generic}}
// expected-error@-1 {{generic struct must be declared private or fileprivate because its generic requirement uses a private type}}
struct DefaultGenericPrivateReq2<T> where T: PrivateProto {} // expected-error {{generic struct must be declared private or fileprivate because its generic requirement uses a private type}}

public struct PublicGenericInternal<T: InternalProto> {} // expected-error {{generic struct cannot be declared public because its generic parameter uses an internal type}}

public struct PublicGenericPI<T: PrivateProto, U: InternalProto> {} // expected-error {{generic struct cannot be declared public because its generic parameter uses a private type}}
public struct PublicGenericIP<T: InternalProto, U: PrivateProto> {} // expected-error {{generic struct cannot be declared public because its generic parameter uses a private type}}
public struct PublicGenericPIReq<T: PrivateProto> where T: InternalProto {} // expected-error {{generic struct cannot be declared public because its generic parameter uses a private type}}
public struct PublicGenericIPReq<T: InternalProto> where T: PrivateProto {} // expected-error {{generic struct cannot be declared public because its generic requirement uses a private type}}

public func genericFunc<T: InternalProto>(_: T) {} // expected-error {{function cannot be declared public because its generic parameter uses an internal type}} {}
public class GenericClass<T: InternalProto> { // expected-error {{generic class cannot be declared public because its generic parameter uses an internal type}}
  public init<T: PrivateProto>(_: T) {} // expected-error {{initializer cannot be declared public because its generic parameter uses a private type}}
  public func genericMethod<T: PrivateProto>(_: T) {} // expected-error {{instance method cannot be declared public because its generic parameter uses a private type}}
}
public enum GenericEnum<T: InternalProto> { // expected-error {{generic enum cannot be declared public because its generic parameter uses an internal type}}
  case A
}


public protocol PublicMutationOperations {
  var size: Int { get set }
  subscript (_: Int) -> Int { get set }
}

internal protocol InternalMutationOperations {
  var size: Int { get set }
  subscript (_: Int) -> Int { get set }
}

public struct AccessorsControl : InternalMutationOperations {
  private var size = 0 // expected-error {{property 'size' must be declared internal because it matches a requirement in internal protocol 'InternalMutationOperations'}} {{none}} expected-note {{mark the property as 'internal' to satisfy the requirement}} {{3-10=internal}}
  private subscript (_: Int) -> Int { // expected-error {{subscript must be declared internal because it matches a requirement in internal protocol 'InternalMutationOperations'}} {{none}} expected-note {{mark the subscript as 'internal' to satisfy the requirement}} {{3-10=internal}}
    get { return 42 }
    set {}
  }
}

public struct PrivateSettersPublic : InternalMutationOperations {
  // Please don't change the formatting here; it's a precise fix-it test.
  public private(set) var size = 0 // expected-error {{setter for property 'size' must be declared internal because it matches a requirement in internal protocol 'InternalMutationOperations'}} {{none}} expected-note {{mark the property as 'internal' to satisfy the requirement}} {{10-17=internal}}
  public private(set) subscript (_: Int) -> Int { // expected-error {{subscript setter must be declared internal because it matches a requirement in internal protocol 'InternalMutationOperations'}} {{none}} expected-note {{mark the subscript as 'internal' to satisfy the requirement}} {{10-17=internal}}
    get { return 42 }
    set {}
  }
}

internal struct PrivateSettersInternal : PublicMutationOperations {
  // Please don't change the formatting here; it's a precise fix-it test.
  private(set)var size = 0 // expected-error {{setter for property 'size' must be as accessible as its enclosing type because it matches a requirement in protocol 'PublicMutationOperations'}} {{none}} expected-note {{mark the property as 'internal' to satisfy the requirement}} {{3-15=}}

  internal private(set)subscript (_: Int) -> Int { // expected-error {{subscript setter must be as accessible as its enclosing type because it matches a requirement in protocol 'PublicMutationOperations'}} {{none}} expected-note {{mark the subscript as 'internal' to satisfy the requirement}} {{12-24=}}
    get { return 42 }
    set {}
  }
}

public protocol PublicReadOnlyOperations {
  var size: Int { get }
  subscript (_: Int) -> Int { get }
}

internal struct PrivateSettersForReadOnlyInternal : PublicReadOnlyOperations {
  public private(set) var size = 0
  internal private(set) subscript (_: Int) -> Int { // no-warning
    get { return 42 }
    set {}
  }
}

public struct PrivateSettersForReadOnlyPublic : PublicReadOnlyOperations {
  public private(set) var size = 0 // no-warning
  internal private(set) subscript (_: Int) -> Int { // expected-error {{subscript must be declared public because it matches a requirement in public protocol 'PublicReadOnlyOperations'}} {{none}} expected-note {{mark the subscript as 'public' to satisfy the requirement}} {{3-11=public}}
    get { return 42 }
    set {}
  }
}


public protocol PublicOperatorProto {
  static prefix func !(_: Self) -> Self
}

internal protocol InternalOperatorProto {
  static prefix func !(_: Self) -> Self
}

fileprivate protocol FilePrivateOperatorProto {
  static prefix func !(_: Self) -> Self
}

private protocol PrivateOperatorProto {
  static prefix func !(_: Self) -> Self
}

public struct PublicOperatorAdopter : PublicOperatorProto {
  // expected-error@-1 {{method '!' must be declared public because it matches a requirement in public protocol 'PublicOperatorProto'}}
  fileprivate struct Inner : PublicOperatorProto {
  }
}
private prefix func !(input: PublicOperatorAdopter) -> PublicOperatorAdopter { // expected-note {{mark the operator function as 'public' to satisfy the requirement}} {{1-8=public}}
  return input
}
private prefix func !(input: PublicOperatorAdopter.Inner) -> PublicOperatorAdopter.Inner {
  return input
}

public struct InternalOperatorAdopter : InternalOperatorProto {
  // expected-error@-1 {{method '!' must be declared internal because it matches a requirement in internal protocol 'InternalOperatorProto'}}
  fileprivate struct Inner : InternalOperatorProto {
  }
}
private prefix func !(input: InternalOperatorAdopter) -> InternalOperatorAdopter { // expected-note {{mark the operator function as 'internal' to satisfy the requirement}} {{1-8=internal}}
  return input
}
private prefix func !(input: InternalOperatorAdopter.Inner) -> InternalOperatorAdopter.Inner {
  return input
}

public struct FilePrivateOperatorAdopter : FilePrivateOperatorProto {
  fileprivate struct Inner : FilePrivateOperatorProto {
  }
}
private prefix func !(input: FilePrivateOperatorAdopter) -> FilePrivateOperatorAdopter {
  return input
}
private prefix func !(input: FilePrivateOperatorAdopter.Inner) -> FilePrivateOperatorAdopter.Inner {
  return input
}

public struct PrivateOperatorAdopter : PrivateOperatorProto {
  fileprivate struct Inner : PrivateOperatorProto {
  }
}
private prefix func !(input: PrivateOperatorAdopter) -> PrivateOperatorAdopter {
  return input
}
private prefix func !(input: PrivateOperatorAdopter.Inner) -> PrivateOperatorAdopter.Inner {
  return input
}


public protocol Equatablish {
  static func ==(lhs: Self, rhs: Self) /* -> bool */ // expected-note {{protocol requires function '=='}}
}

fileprivate struct EquatablishOuter {
  internal struct Inner : Equatablish {}
}
private func ==(lhs: EquatablishOuter.Inner, rhs: EquatablishOuter.Inner) {}

fileprivate struct EquatablishOuter2 {
  internal struct Inner : Equatablish {
    fileprivate static func ==(lhs: Inner, rhs: Inner) {}
  }
}

fileprivate struct EquatablishOuterProblem {
  internal struct Inner : Equatablish { // expected-error {{type 'EquatablishOuterProblem.Inner' does not conform to protocol 'Equatablish'}}
    private static func ==(lhs: Inner, rhs: Inner) {}
  }
}

internal struct EquatablishOuterProblem2 {
  public struct Inner : Equatablish {
    fileprivate static func ==(lhs: Inner, rhs: Inner) {} // expected-error {{method '==' must be as accessible as its enclosing type because it matches a requirement in protocol 'Equatablish'}} {{none}}
    // expected-note@-1 {{mark the operator function as 'internal' to satisfy the requirement}} {{5-16=internal}}
  }
}

internal struct EquatablishOuterProblem3 {
  public struct Inner : Equatablish { // expected-error {{method '==' must be as accessible as its enclosing type because it matches a requirement in protocol 'Equatablish'}} {{none}}
  }
}
private func ==(lhs: EquatablishOuterProblem3.Inner, rhs: EquatablishOuterProblem3.Inner) {}
// expected-note@-1 {{mark the operator function as 'internal' to satisfy the requirement}} {{1-8=internal}}

internal struct EquatablishOuterProblem4 {
  public struct Inner : Equatablish {} // expected-error {{method '==' must be as accessible as its enclosing type because it matches a requirement in protocol 'Equatablish'}} {{none}}
}
internal extension EquatablishOuterProblem4.Inner {
  fileprivate static func ==(lhs: EquatablishOuterProblem4.Inner, rhs: EquatablishOuterProblem4.Inner) {}
  // expected-note@-1 {{mark the operator function as 'internal' to satisfy the requirement}} {{3-15=}}
}

internal struct EquatablishOuterProblem5 {
  public struct Inner : Equatablish {} // expected-error {{method '==' must be as accessible as its enclosing type because it matches a requirement in protocol 'Equatablish'}} {{none}}
}
private extension EquatablishOuterProblem5.Inner {
  static func ==(lhs: EquatablishOuterProblem5.Inner, rhs: EquatablishOuterProblem5.Inner) {}
  // expected-note@-1 {{move the operator function to another extension where it can be declared 'internal' to satisfy the requirement}} {{none}}
}


public protocol AssocTypeProto {
  associatedtype Assoc
}

fileprivate struct AssocTypeOuter {
  internal struct Inner : AssocTypeProto {
    fileprivate typealias Assoc = Int
  }
}

fileprivate struct AssocTypeOuterProblem {
  internal struct Inner : AssocTypeProto {
    private typealias Assoc = Int // expected-error {{type alias 'Assoc' must be as accessible as its enclosing type because it matches a requirement in protocol 'AssocTypeProto'}} {{none}} expected-note {{mark the type alias as 'fileprivate' to satisfy the requirement}} {{5-12=fileprivate}}
  }
}

internal struct AssocTypeOuterProblem2 {
  public struct Inner : AssocTypeProto {
    fileprivate typealias Assoc = Int // expected-error {{type alias 'Assoc' must be as accessible as its enclosing type because it matches a requirement in protocol 'AssocTypeProto'}} {{none}} expected-note {{mark the type alias as 'internal' to satisfy the requirement}} {{5-16=internal}}
  }
}

internal struct AssocTypeOuterProblem3 {
  public struct Inner : AssocTypeProto {} // expected-error {{type alias 'Assoc' must be as accessible as its enclosing type because it matches a requirement in protocol 'AssocTypeProto'}} {{none}}
}
internal extension AssocTypeOuterProblem3.Inner {
  fileprivate typealias Assoc = Int // expected-note {{mark the type alias as 'internal' to satisfy the requirement}} {{3-15=}}
}

internal struct AssocTypeOuterProblem4 {
  public struct Inner : AssocTypeProto {} // expected-error {{type alias 'Assoc' must be as accessible as its enclosing type because it matches a requirement in protocol 'AssocTypeProto'}} {{none}}
}
private extension AssocTypeOuterProblem4.Inner {
  typealias Assoc = Int // expected-note {{move the type alias to another extension where it can be declared 'internal' to satisfy the requirement}} {{none}}
}

internal typealias InternalComposition = PublicClass & PublicProto // expected-note {{declared here}}
public class DerivedFromInternalComposition : InternalComposition { // expected-error {{class cannot be declared public because its superclass is internal}}
  public func publicReq() {}
}

internal typealias InternalGenericComposition<T> = PublicGenericClass<T> & PublicProto // expected-note {{declared here}}
public class DerivedFromInternalGenericComposition : InternalGenericComposition<Int> { // expected-error {{class cannot be declared public because its superclass is internal}}
  public func publicReq() {}
}

internal typealias InternalConcreteGenericComposition = PublicGenericClass<Int> & PublicProto // expected-note {{declared here}}
public class DerivedFromInternalConcreteGenericComposition : InternalConcreteGenericComposition { // expected-error {{class cannot be declared public because its superclass is internal}}
  public func publicReq() {}
}

public typealias BadPublicComposition1 = InternalClass & PublicProto // expected-error {{type alias cannot be declared public because its underlying type uses an internal type}}
public typealias BadPublicComposition2 = PublicClass & InternalProto // expected-error {{type alias cannot be declared public because its underlying type uses an internal type}}
public typealias BadPublicComposition3<T> = InternalGenericClass<T> & PublicProto // expected-error {{type alias cannot be declared public because its underlying type uses an internal type}}
public typealias BadPublicComposition4 = InternalGenericClass<Int> & PublicProto // expected-error {{type alias cannot be declared public because its underlying type uses an internal type}}
public typealias BadPublicComposition5 = PublicGenericClass<InternalStruct> & PublicProto // expected-error {{type alias cannot be declared public because its underlying type uses an internal type}}


open class ClassWithProperties {
  open open(set) var openProp = 0 // expected-warning {{'open(set)' modifier is redundant for an open property}} {{8-18=}}
  public public(set) var publicProp = 0 // expected-warning {{'public(set)' modifier is redundant for a public property}} {{10-22=}}
  internal internal(set) var internalProp = 0 // expected-warning {{'internal(set)' modifier is redundant for an internal property}} {{12-26=}}
  fileprivate fileprivate(set) var fileprivateProp = 0 // expected-warning {{'fileprivate(set)' modifier is redundant for a fileprivate property}} {{15-32=}}
  private private(set) var privateProp = 0 // expected-warning {{'private(set)' modifier is redundant for a private property}} {{11-24=}}
  internal(set) var defaultProp = 0 // expected-warning {{'internal(set)' modifier is redundant for an internal property}} {{3-17=}}
}

extension ClassWithProperties {
  // expected-warning@+1 {{'internal(set)' modifier is redundant for an internal property}} {{12-26=}}
  internal internal(set) var defaultExtProp: Int {
    get { return 42 }
    set {}
  }
  // expected-warning@+1 {{'internal(set)' modifier is redundant for an internal property}} {{3-17=}}
  internal(set) var defaultExtProp2: Int {
    get { return 42 }
    set {}
  }
}

public extension ClassWithProperties {
  // expected-warning@+2 {{'public' modifier is redundant for property declared in a public extension}} {{3-10=}}
  // expected-warning@+1 {{'public(set)' modifier is redundant for a public property}} {{10-22=}}
  public public(set) var publicExtProp: Int {
    get { return 42 }
    set {}
  }
  // expected-warning@+1 {{'public(set)' modifier is redundant for a public property}} {{3-15=}}
  public(set) var publicExtProp2: Int {
    get { return 42 }
    set {}
  }
}

internal extension ClassWithProperties {
  // expected-warning@+2 {{'internal' modifier is redundant for property declared in an internal extension}} {{3-12=}}
  // expected-warning@+1 {{'internal(set)' modifier is redundant for an internal property}} {{12-26=}}
  internal internal(set) var internalExtProp: Int {
    get { return 42 }
    set {}
  }
  // expected-warning@+1 {{'internal(set)' modifier is redundant for an internal property}} {{3-17=}}
  internal(set) var internalExtProp2: Int {
    get { return 42 }
    set {}
  }
}

fileprivate extension ClassWithProperties {
  // expected-warning@+2 {{'fileprivate' modifier is redundant for property declared in a fileprivate extension}} {{3-15=}}
  // expected-warning@+1 {{'fileprivate(set)' modifier is redundant for a fileprivate property}} {{15-32=}}
  fileprivate fileprivate(set) var fileprivateExtProp: Int {
    get { return 42 }
    set {}
  }
  // expected-warning@+1 {{'fileprivate(set)' modifier is redundant for a fileprivate property}} {{3-20=}}
  fileprivate(set) var fileprivateExtProp2: Int {
    get { return 42 }
    set {}
  }
  private(set) var fileprivateExtProp3: Int {
    get { return 42 }
    set {}
  }
}

private extension ClassWithProperties {
  // expected-warning@+2 {{'fileprivate' modifier is redundant for property declared in a private (equivalent to fileprivate) extension}} {{3-15=}}
  // expected-warning@+1 {{'fileprivate(set)' modifier is redundant for a fileprivate property}} {{15-32=}}
  fileprivate fileprivate(set) var privateExtProp: Int {
    get { return 42 }
    set {}
  }
  // expected-warning@+1 {{'fileprivate(set)' modifier is redundant for a fileprivate property}} {{3-20=}}
  fileprivate(set) var privateExtProp2: Int {
    get { return 42 }
    set {}
  }
  private(set) var privateExtProp3: Int {
    get { return 42 }
    set {}
  }
}

public var inferredType = PrivateStruct() // expected-error {{variable cannot be declared public because its type 'PrivateStruct' uses a private type}}
public var inferredGenericParameters: Optional = PrivateStruct() // expected-error {{variable cannot be declared public because its type uses a private type}}
public var explicitType: Optional<PrivateStruct> = PrivateStruct() // expected-error {{variable cannot be declared public because its type uses a private type}}

// rdar://problem/47557376
@objc open class ObjCBase {
  init() {}
  @objc open dynamic func foo() {}
  @objc open dynamic var bar: Int = 0
}
open class ObjCSub: ObjCBase {}
public extension ObjCSub {
  // Don't try to remove the 'open', since it's needed to be a valid 'override'.
  open override func foo() {} // expected-warning {{'open' modifier conflicts with extension's default access of 'public'}} {{none}}
  open override var bar: Int { // expected-warning {{'open' modifier conflicts with extension's default access of 'public'}} {{none}}
    get { return 0 }
    set {}
  }
}

public struct TestGenericSubscripts {
  // We'd like these to be errors in a future version of Swift, but they weren't
  // in Swift 5.0.
  public subscript<T: PrivateProto>(_: T) -> Int { return 0 } // expected-warning {{subscript should not be declared public because its generic parameter uses a private type}} {{none}}
  public subscript<T>(where _: T) -> Int where T: PrivateProto { return 0 } // expected-warning {{subscript should not be declared public because its generic requirement uses a private type}} {{none}}
}

public typealias TestGenericAlias<T: PrivateProto> = T // expected-warning {{type alias should not be declared public because its generic parameter uses a private type}}
public typealias TestGenericAliasWhereClause<T> = T where T: PrivateProto // expected-warning {{type alias should not be declared public because its generic requirement uses a private type}}
