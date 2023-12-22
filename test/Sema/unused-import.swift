// RUN: %empty-directory(%t)
// RUN: split-file --leading-lines %s %t

/// Build the libraries.
// RUN: %target-swift-frontend -emit-module %t/PublicLib.swift -o %t -enable-library-evolution

/// Check diagnostics.
// RUN: %target-swift-frontend -typecheck %t/UnusedImportClient.swift -I %t -verify -Runused-module
// RUN: %target-swift-frontend -typecheck %t/UsedImportClient1.swift -I %t -verify -Runused-module
// RUN: %target-swift-frontend -typecheck %t/UsedImportClient2.swift -I %t -verify -Runused-module
// RUN: %target-swift-frontend -typecheck %t/UsedImportClient3.swift -I %t -verify -Runused-module
// RUN: %target-swift-frontend -typecheck %t/UsedImportClient4.swift -I %t -verify -Runused-module
// RUN: %target-swift-frontend -typecheck %t/UsedImportClient5.swift -I %t -verify -Runused-module
// RUN: %target-swift-frontend -typecheck %t/UsedImportClient6.swift -I %t -verify -Runused-module
// RUN: %target-swift-frontend -typecheck %t/UsedImportClient7.swift -I %t -verify -Runused-module

//--- PublicLib.swift
public let PublicLibConstant: Int = 42
public protocol PublicImportProto {
    associatedtype T
}
public struct PublicImportType {
    public init() {}
}
open class PublicImportClass {}

@propertyWrapper
public struct PublicLibWrapper<T> {
  public var wrappedValue: T
  public init(wrappedValue: T) {
    self.wrappedValue = wrappedValue
  }
}

extension Float: ExpressibleByArrayLiteral {
    public init(arrayLiteral: Float...) {
        self.init()
        var sum: Float = 0
        for element in arrayLiteral {
            sum = sum + element
        }
        self = sum
    }
}

extension Int {
    public static var theBestInt: Int {
        return 42
    }
}

/// Short test to demonstrate unused import remark
//--- UnusedImportClient.swift
import PublicLib // expected-remark {{import of 'PublicLib' unused during compilation}}

//--- UsedImportClient1.swift
import PublicLib
public struct PublicLibClientStruct {
    let sp1: PublicImportType
}

//--- UsedImportClient2.swift
import PublicLib
public protocol PublicLibClientProtocol : PublicImportProto {}

//--- UsedImportClient3.swift
import PublicLib
public class PublicLibClientSubclass : PublicImportClass {}

//--- UsedImportClient4.swift
import PublicLib
public struct PublicLibClientStruct {
    @PublicLibWrapper
    var wp1: Int
}

//--- UsedImportClient5.swift
import PublicLib
public let PublicLibClientConstant: Int = 84 - PublicLibConstant

//--- UsedImportClient6.swift
import PublicLib
func foo() {
    let _: Float = [1.0, 2.0, 3.0]
}

//--- UsedImportClient7.swift
import PublicLib
func foo() {
    print(Int.theBestInt)
}

