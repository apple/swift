// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend -verify -module-name Lib %t/Lib.swift -emit-module -emit-module-path %t/Lib.swiftmodule -package-name myPkg

/// Type-checking an operator (L30 below) causes look up of all of its decls regardless of which access modifier is
/// used (decl in both Client and Lib in this case) before filtering. The decl in Lib is a package decl and has
/// package-name associated with it, but the use site (in Client) does not have package-name. The package decl
/// should be filtered out and the public == decl should be picked.
// RUN: %target-swift-frontend -typecheck -verify %t/Client.swift -I %t


//--- Lib.swift
public class Decl {
  package static func == (lhs: Decl, rhs: Decl) -> Bool {
    return true
  }
}

//--- Client.swift
import Lib

extension Decl: Equatable {
  public static func == (lhs: Decl, rhs: Decl) -> Bool {
    return false
  }
}

public protocol Proto {}
extension Proto {
  func foo(first: Decl, second: Decl) -> Bool {
    // Type-checking below causes a look up of == in both Client and Lib
    return first == second
  }
}
