// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-build-swift-dylib(%t/%target-library-name(Utils)) %t/Utils.swift -module-name Utils -package-name mypkg -emit-module -emit-module-path %t/Utils.swiftmodule -enable-library-evolution

// RUN: %target-swift-frontend -typecheck -verify %t/User.swift -package-name mypkg -enable-library-evolution -I %t -L %t -lUtils

//--- Utils.swift

// expected-note@+1 * {{type declared here}}
public struct PubStruct {
  var inPub: String
  public init() {
    inPub = "pub"
  }
}

// expected-note@+1 * {{type declared here}}
package struct PkgStruct { 
  var inPkg: String
  package init() {
    inPkg = "pkg"
  }
}

// expected-note@+1 * {{type declared here}}
public protocol PubProto {
  var pubVar: PubStruct { get set }
}

// expected-note@+1 * {{type declared here}}
package protocol PkgProto {
  var pkgVar: PkgStruct { get set }
}

//--- User.swift
@_implementationOnly import Utils

public func f() -> PubStruct? { // expected-error {{cannot use struct 'PubStruct' here; 'Utils' has been imported as implementation-only}}
  return nil
}

package func g() -> PubStruct? { // expected-error {{cannot use struct 'PubStruct' here; 'Utils' has been imported as implementation-only}}
    return nil
}

package func h() -> PkgStruct? { // expected-error {{cannot use struct 'PkgStruct' here; 'Utils' has been imported as implementation-only}}
    return nil
}

func i() -> PubStruct? { // no-error
    return nil
}
func j() -> PkgStruct? { // no-error
    return nil
}


package struct UserPkg1: PubProto { // expected-error {{cannot use protocol 'PubProto' here; 'Utils' has been imported as implementation-only}}
  package var pubVar: PubStruct // expected-error {{cannot use struct 'PubStruct' here; 'Utils' has been imported as implementation-only}}
  package init() {
    pubVar = PubStruct()
  }
}

package struct UserPkg2: PkgProto { // expected-error {{cannot use protocol 'PkgProto' here; 'Utils' has been imported as implementation-only}}
  package var pkgVar: PkgStruct // expected-error {{cannot use struct 'PkgStruct' here; 'Utils' has been imported as implementation-only}}
  package init() {
    pkgVar = PkgStruct()
  }
}
