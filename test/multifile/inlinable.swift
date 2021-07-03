// RUN: %target-swift-frontend -typecheck -verify %s %S/Inputs/inlinable-other.swift -enable-access-control-hacks

@frozen public struct HasInitExpr {
  public var hasInlinableInit = mutatingFunc(&OtherStruct.staticProp)
  // expected-warning@-1 {{setter for 'staticProp' is internal and should not be referenced from a property initializer in a '@frozen' type}}
}

public func mutatingFunc(_: inout Int) -> Int {
  return 0
}
