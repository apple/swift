// RUN: %target-typecheck-verify-swift -swift-version 4 -enable-access-control-hacks

private struct PrivateStruct {} // expected-note 6{{type declared here}}
internal struct InternalStruct {} // expected-note 2{{type declared here}}
public struct PublicStruct {}

public protocol BaseProtocol {
  associatedtype T
}

public protocol PublicProtocol1 : BaseProtocol where T == PrivateStruct {
  // expected-warning@-1 {{public protocol's 'where' clause should not use a private struct}}

  associatedtype X : BaseProtocol where X.T == PrivateStruct
  // expected-warning@-1 {{associated type in a public protocol uses a private type in its requirement}}
}

public protocol PublicProtocol2 : BaseProtocol where T == InternalStruct {
  // expected-warning@-1 {{public protocol's 'where' clause should not use an internal struct}}

  associatedtype X : BaseProtocol where X.T == InternalStruct
  // expected-warning@-1 {{associated type in a public protocol uses an internal type in its requirement}}
}

public protocol PublicProtocol3 : BaseProtocol where T == PublicStruct {
  associatedtype X : BaseProtocol where X.T == PublicStruct
}

internal protocol InternalProtocol1 : BaseProtocol where T == PrivateStruct {
  // expected-warning@-1 {{internal protocol's 'where' clause should not use a private struct}}

  associatedtype X : BaseProtocol where X.T == PrivateStruct
  // expected-warning@-1 {{associated type in an internal protocol uses a private type in its requirement}}
}

internal protocol InternalProtocol2 : BaseProtocol where T == InternalStruct {
  associatedtype X : BaseProtocol where X.T == InternalStruct
}

internal protocol InternalProtocol3 : BaseProtocol where T == PublicStruct {
  associatedtype X : BaseProtocol where X.T == PublicStruct
}

protocol Protocol1 : BaseProtocol where T == PrivateStruct {
  // expected-warning@-1 {{protocol should be declared fileprivate because its 'where' clause uses a private struct}}

  associatedtype X : BaseProtocol where X.T == PrivateStruct
  // expected-warning@-1 {{associated type in an internal protocol uses a private type in its requirement}}
}

protocol Protocol2 : BaseProtocol where T == InternalStruct {
  associatedtype X : BaseProtocol where X.T == InternalStruct
}

protocol Protocol3 : BaseProtocol where T == PublicStruct {
  associatedtype X : BaseProtocol where X.T == PublicStruct
}
