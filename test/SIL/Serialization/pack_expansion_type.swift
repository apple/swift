// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module %s -emit-module-path %t/pack_expansion_type.swiftmodule -DLIB
// RUN: %target-swift-frontend -emit-ir %s -I %t -DAPP -module-name main

#if LIB

public func calleeWithPack<each T>(_: repeat each T) {}

public func calleeGeneric<T>(_: T) {}

@_transparent public func transparentCaller<each T>(_ t: repeat each T) {
  calleeWithPack(repeat [each t])
  calleeGeneric((repeat [each t]))
}

@_alwaysEmitIntoClient public func serializedCaller<each T>(_ t: repeat each T) {
  calleeWithPack(repeat [each t])
  calleeGeneric((repeat [each t]))
}

public func transparentCaller2() {
  transparentCaller(1, "hi", false)
}

@_alwaysEmitIntoClient
public func calleeWithRequirement<each T: Equatable>(_ t: repeat each T) {
  repeat ((each t) == (each t))
}

#elseif APP

import pack_expansion_type

transparentCaller(1, "hi", false)
serializedCaller(1, "hi", false)
calleeWithRequirement(1, "hi")

#endif
