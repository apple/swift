// REQUIRES: swift_swift_parser

// RUN: %target-typecheck-verify-swift -swift-version 5 -module-name Swift -parse-stdlib

// expected-warning@+2{{@expression has been removed in favor of @freestanding(expression)}}
// expected-warning@+1{{external macro implementation type 'A.B' could not be found for macro 'myMacro()'}}
@expression macro myMacro() = #externalMacro(module: "A", type: "B")

// Protocols needed for string literals to work
public protocol ExpressibleByUnicodeScalarLiteral {
  associatedtype UnicodeScalarLiteralType: _ExpressibleByBuiltinUnicodeScalarLiteral

  init(unicodeScalarLiteral value: UnicodeScalarLiteralType)
}

public protocol _ExpressibleByBuiltinExtendedGraphemeClusterLiteral
  : _ExpressibleByBuiltinUnicodeScalarLiteral {

  init(
    _builtinExtendedGraphemeClusterLiteral start: Builtin.RawPointer,
    utf8CodeUnitCount: Builtin.Word,
    isASCII: Builtin.Int1)
}

public protocol ExpressibleByExtendedGraphemeClusterLiteral
  : ExpressibleByUnicodeScalarLiteral {

  associatedtype ExtendedGraphemeClusterLiteralType
    : _ExpressibleByBuiltinExtendedGraphemeClusterLiteral

  init(extendedGraphemeClusterLiteral value: ExtendedGraphemeClusterLiteralType)
}

extension ExpressibleByExtendedGraphemeClusterLiteral
  where ExtendedGraphemeClusterLiteralType == UnicodeScalarLiteralType {

  @_transparent
  public init(unicodeScalarLiteral value: ExtendedGraphemeClusterLiteralType) {
    self.init(extendedGraphemeClusterLiteral: value)
  }
}

public protocol _ExpressibleByBuiltinStringLiteral
  : _ExpressibleByBuiltinExtendedGraphemeClusterLiteral {

  init(
    _builtinStringLiteral start: Builtin.RawPointer,
    utf8CodeUnitCount: Builtin.Word,
    isASCII: Builtin.Int1)
}

public protocol ExpressibleByStringLiteral
  : ExpressibleByExtendedGraphemeClusterLiteral {
  associatedtype StringLiteralType: _ExpressibleByBuiltinStringLiteral

  init(stringLiteral value: StringLiteralType)
}

extension ExpressibleByStringLiteral
  where StringLiteralType == ExtendedGraphemeClusterLiteralType {

  @_transparent
  public init(extendedGraphemeClusterLiteral value: StringLiteralType) {
    self.init(stringLiteral: value)
  }
}

public protocol _ExpressibleByBuiltinUnicodeScalarLiteral {
  init(_builtinUnicodeScalarLiteral value: Builtin.Int32)
}
