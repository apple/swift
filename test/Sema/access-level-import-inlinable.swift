// Test use of decls restricted by an import access-level in inlinable code
// and frozen structs.

// RUN: %empty-directory(%t)
// RUN: split-file %s %t --leading-lines

/// Build the libraries.
// RUN: %target-swift-frontend -emit-module %t/PublicLib.swift -o %t \
// RUN:   -swift-version 5 -enable-library-evolution
// RUN: %target-swift-frontend -emit-module %t/PackageLib.swift -o %t \
// RUN:   -swift-version 5 -enable-library-evolution
// RUN: %target-swift-frontend -emit-module %t/InternalLib.swift -o %t \
// RUN:   -swift-version 5 -enable-library-evolution
// RUN: %target-swift-frontend -emit-module %t/FileprivateLib.swift -o %t \
// RUN:   -swift-version 5 -enable-library-evolution
// RUN: %target-swift-frontend -emit-module %t/PrivateLib.swift -o %t \
// RUN:   -swift-version 5 -enable-library-evolution

/// Check diagnostics.
// RUN: %target-swift-frontend -typecheck %t/Client.swift -I %t \
// RUN:   -enable-library-evolution -swift-version 5 -verify \
// RUN:   -package-name package

//--- PublicLib.swift
public protocol PublicImportProto {
    associatedtype T
}
public struct PublicImportType {
    public init() {}
}
public func PublicFunc() {}

@propertyWrapper
public struct PublicImportWrapper<T> {
  public var wrappedValue: T
  public init(wrappedValue: T) {
    self.wrappedValue = wrappedValue
  }
}

//--- PackageLib.swift
public struct PackageImportType {
    public init() {}
}

//--- InternalLib.swift
public protocol InternalImportProto {
    associatedtype T
}
public struct InternalImportType {
    public init() {}
}
public func InternalFunc() {}

@frozen public struct InternalImportFrozenType {}

//--- FileprivateLib.swift
public protocol FileprivateImportProto {
    associatedtype T
}

@propertyWrapper
public struct FileprivateImportWrapper<T> {
  public var wrappedValue: T
  public init(wrappedValue: T) {
    self.wrappedValue = wrappedValue
  }
}

//--- PrivateLib.swift
public struct PrivateImportType {
    public init() {}
}

//--- Client.swift
public import PublicLib

package import PackageLib
// expected-note@-1 4 {{struct 'PackageImportType' imported as 'package' from 'PackageLib' here}}

internal import InternalLib
// expected-note@-1 9 {{struct 'InternalImportType' imported as 'internal' from 'InternalLib' here}}
// expected-note@-2 4 {{protocol 'InternalImportProto' imported as 'internal' from 'InternalLib' here}}
// expected-note@-3 2 {{global function 'InternalFunc()' imported as 'internal' from 'InternalLib' here}}
// expected-note@-4 2 {{struct 'InternalImportFrozenType' imported as 'internal' from 'InternalLib' here}}

fileprivate import FileprivateLib
// expected-note@-1 2 {{generic struct 'FileprivateImportWrapper' imported as 'fileprivate' from 'FileprivateLib' here}}
// expected-note@-2 2 {{initializer 'init(wrappedValue:)' imported as 'fileprivate' from 'FileprivateLib' here}}
// expected-note@-3 2 {{protocol 'FileprivateImportProto' imported as 'fileprivate' from 'FileprivateLib' here}}

private import PrivateLib
// expected-note@-1 10 {{struct 'PrivateImportType' imported as 'private' from 'PrivateLib' here}}
 // expected-note@-2 2 {{initializer 'init()' imported as 'private' from 'PrivateLib' here}}

public struct GenericType<T, U> {}

@inlinable public func inlinable() {

  PublicFunc()
  InternalFunc() // expected-error {{global function 'InternalFunc()' is internal and cannot be referenced from an '@inlinable' function}}

  let _: PublicImportType
  let _: InternalImportType // expected-error {{struct 'InternalImportType' is internal and cannot be referenced from an '@inlinable' function}}

  let _ = PublicImportType()
  let _ = PrivateImportType() // expected-error {{struct 'PrivateImportType' is private and cannot be referenced from an '@inlinable' function}}
  // expected-error @-1 {{initializer 'init()' is private and cannot be referenced from an '@inlinable' function}}

  let _: any PublicImportProto
  let _: any InternalImportProto // expected-error {{protocol 'InternalImportProto' is internal and cannot be referenced from an '@inlinable' function}}

  let _: any FileprivateImportProto & InternalImportProto // expected-error {{protocol 'FileprivateImportProto' is fileprivate and cannot be referenced from an '@inlinable' function}}
  // expected-error @-1 {{protocol 'InternalImportProto' is internal and cannot be referenced from an '@inlinable' function}}

  func PublicFuncUsesPublic(_: PublicImportType) {}
  func PublicFuncUsesPackage(_: PackageImportType) {} // expected-error {{struct 'PackageImportType' is package and cannot be referenced from an '@inlinable' function}}}

  func PublicFuncUsesPublic() -> PublicImportType {
    fatalError()
  }
  func PublicFuncReturnUsesInternal() -> InternalImportType { // expected-error {{struct 'InternalImportType' is internal and cannot be referenced from an '@inlinable' function}}
    fatalError()
  }

  @PublicImportWrapper
  var wrappedPublic: PublicImportType

  @FileprivateImportWrapper // expected-error {{initializer 'init(wrappedValue:)' is fileprivate and cannot be referenced from an '@inlinable' function}}
  // expected-error @-1 {{generic struct 'FileprivateImportWrapper' is fileprivate and cannot be referenced from an '@inlinable' function}}
  var wrappedFileprivate: PublicImportType

  let _: GenericType<PublicImportType, PublicImportType>
  let _: GenericType<InternalImportType, PrivateImportType> // expected-error {{struct 'InternalImportType' is internal and cannot be referenced from an '@inlinable' function}}
  // expected-error @-1 {{struct 'PrivateImportType' is private and cannot be referenced from an '@inlinable' function}}
}

@_alwaysEmitIntoClient public func alwaysEmitIntoClient() {

  PublicFunc()
  InternalFunc() // expected-error {{global function 'InternalFunc()' is internal and cannot be referenced from an '@_alwaysEmitIntoClient' function}}

  let _: PublicImportType
  let _: InternalImportType // expected-error {{struct 'InternalImportType' is internal and cannot be referenced from an '@_alwaysEmitIntoClient' function}}

  let _ = PublicImportType()
  let _ = PrivateImportType() // expected-error {{struct 'PrivateImportType' is private and cannot be referenced from an '@_alwaysEmitIntoClient' function}}
  // expected-error @-1 {{initializer 'init()' is private and cannot be referenced from an '@_alwaysEmitIntoClient' function}}

  let _: any PublicImportProto
  let _: any InternalImportProto // expected-error {{protocol 'InternalImportProto' is internal and cannot be referenced from an '@_alwaysEmitIntoClient' function}}

  let _: any FileprivateImportProto & InternalImportProto // expected-error {{protocol 'FileprivateImportProto' is fileprivate and cannot be referenced from an '@_alwaysEmitIntoClient' function}}
  // expected-error @-1 {{protocol 'InternalImportProto' is internal and cannot be referenced from an '@_alwaysEmitIntoClient' function}}

  func PublicFuncUsesPublic(_: PublicImportType) {}
  func PublicFuncUsesPackage(_: PackageImportType) {} // expected-error {{struct 'PackageImportType' is package and cannot be referenced from an '@_alwaysEmitIntoClient' function}}}

  func PublicFuncUsesPublic() -> PublicImportType {
    fatalError()
  }
  func PublicFuncReturnUsesInternal() -> InternalImportType { // expected-error {{struct 'InternalImportType' is internal and cannot be referenced from an '@_alwaysEmitIntoClient' function}}
    fatalError()
  }

  @PublicImportWrapper
  var wrappedPublic: PublicImportType

  @FileprivateImportWrapper // expected-error {{initializer 'init(wrappedValue:)' is fileprivate and cannot be referenced from an '@_alwaysEmitIntoClient' function}}
  // expected-error @-1 {{generic struct 'FileprivateImportWrapper' is fileprivate and cannot be referenced from an '@_alwaysEmitIntoClient' function}}
  var wrappedFileprivate: PublicImportType

  let _: GenericType<PublicImportType, PublicImportType>
  let _: GenericType<InternalImportType, PrivateImportType> // expected-error {{struct 'InternalImportType' is internal and cannot be referenced from an '@_alwaysEmitIntoClient' function}}
  // expected-error @-1 {{struct 'PrivateImportType' is private and cannot be referenced from an '@_alwaysEmitIntoClient' function}}
}

@frozen public struct BadFields1 {
  private var field: PrivateImportType // expected-error {{type referenced from a stored property in a '@frozen' struct must be '@usableFromInline' or public}}
  // expected-note @-1 {{struct 'PrivateImportType' is imported by this file as 'private' from 'PrivateLib'}}
}

@_fixed_layout public struct FixedBadFields1 {
// expected-warning@-1 {{'@frozen' attribute is now used for fixed-layout structs}}
  private var field: PrivateImportType // expected-error {{type referenced from a stored property in a '@frozen' struct must be '@usableFromInline' or public}}
  // expected-note @-1 {{struct 'PrivateImportType' is imported by this file as 'private' from 'PrivateLib'}}
}

@frozen public struct BadFields2 {
  private var field: PrivateImportType? // expected-error {{type referenced from a stored property in a '@frozen' struct must be '@usableFromInline' or public}}
  // expected-note @-1 {{struct 'PrivateImportType' is imported by this file as 'private' from 'PrivateLib'}}
}

@_fixed_layout public struct FixedBadFields2 {
// expected-warning@-1 {{'@frozen' attribute is now used for fixed-layout structs}}
  private var field: PrivateImportType? // expected-error {{type referenced from a stored property in a '@frozen' struct must be '@usableFromInline' or public}}
  // expected-note @-1 {{struct 'PrivateImportType' is imported by this file as 'private' from 'PrivateLib'}}
}

@frozen public struct BadFields3 {
  internal var field: PackageImportType? // expected-error {{type referenced from a stored property in a '@frozen' struct must be '@usableFromInline' or public}}
  // expected-note @-1 {{struct 'PackageImportType' is imported by this file as 'package' from 'PackageLib'}}
}

@_fixed_layout public struct FixedBadFields3 {
// expected-warning@-1 {{'@frozen' attribute is now used for fixed-layout structs}}
  internal var field: PackageImportType? // expected-error {{type referenced from a stored property in a '@frozen' struct must be '@usableFromInline' or public}}
  // expected-note @-1 {{struct 'PackageImportType' is imported by this file as 'package' from 'PackageLib'}}
}

@frozen @usableFromInline struct BadFields4 {
  internal var field: InternalImportType? // expected-error {{type referenced from a stored property in a '@frozen' struct must be '@usableFromInline' or public}}
  // expected-note @-1 {{struct 'InternalImportType' is imported by this file as 'internal' from 'InternalLib'}}
}

@_fixed_layout @usableFromInline struct FixedBadFields4 {
// expected-warning@-1 {{'@frozen' attribute is now used for fixed-layout structs}}
  internal var field: InternalImportType? // expected-error {{type referenced from a stored property in a '@frozen' struct must be '@usableFromInline' or public}}
  // expected-note @-1 {{struct 'InternalImportType' is imported by this file as 'internal' from 'InternalLib'}}
}

@frozen public struct BadFields5 {
  private var field: PrivateImportType? { // expected-error {{type referenced from a stored property in a '@frozen' struct must be '@usableFromInline' or public}}
  // expected-note @-1 {{struct 'PrivateImportType' is imported by this file as 'private' from 'PrivateLib'}}
    didSet {}
  }
}

@_fixed_layout public struct FixedBadFields5 {
// expected-warning@-1 {{'@frozen' attribute is now used for fixed-layout structs}}
  private var field: PrivateImportType? { // expected-error {{type referenced from a stored property in a '@frozen' struct must be '@usableFromInline' or public}}
  // expected-note @-1 {{struct 'PrivateImportType' is imported by this file as 'private' from 'PrivateLib'}}
    didSet {}
  }
}

@frozen public struct BadFields6 {
  private var field: InternalImportFrozenType? { // expected-error {{type referenced from a stored property in a '@frozen' struct must be '@usableFromInline' or public}}
  // expected-note @-1 {{struct 'InternalImportFrozenType' is imported by this file as 'internal' from 'InternalLib'}}
    didSet {}
  }
}

@_fixed_layout public struct FixedBadFields6 {
// expected-warning@-1 {{'@frozen' attribute is now used for fixed-layout structs}}
  private var field: InternalImportFrozenType? { // expected-error {{type referenced from a stored property in a '@frozen' struct must be '@usableFromInline' or public}}
  // expected-note @-1 {{struct 'InternalImportFrozenType' is imported by this file as 'internal' from 'InternalLib'}}
    didSet {}
  }
}

// expected-error@+1 {{the result of a '@usableFromInline' function must be '@usableFromInline' or public}}
@usableFromInline func notReallyUsableFromInline() -> InternalImportType? { return nil }
  // expected-note @-1 {{struct 'InternalImportType' is imported by this file as 'internal' from 'InternalLib'}}
@frozen public struct BadFields7 {
  private var field = notReallyUsableFromInline() // expected-error {{type referenced from a stored property with inferred type 'InternalImportType?' in a '@frozen' struct must be '@usableFromInline' or public}}
}

@_fixed_layout public struct FrozenBadFields7 {
// expected-warning@-1 {{'@frozen' attribute is now used for fixed-layout structs}}
  private var field = notReallyUsableFromInline() // expected-error {{type referenced from a stored property with inferred type 'InternalImportType?' in a '@frozen' struct must be '@usableFromInline' or public}}
}

@frozen public struct OKFields {
  internal static var staticProp: InternalImportType?
  private var computed: PrivateImportType? { return nil }
}

@_fixed_layout public struct FixedOKFields {
// expected-warning@-1 {{'@frozen' attribute is now used for fixed-layout structs}}
  internal static var staticProp: InternalImportType?
  private var computed: PrivateImportType? { return nil }
}
