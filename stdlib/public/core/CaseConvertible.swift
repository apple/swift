//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

public protocol CaseConvertible {
    
    associatedtype RawValue
    
    var rawValue: RawValue { get }

}

extension CaseConvertible where RawValue: BinaryInteger {
    
    public func asFloat() -> Float { Float(rawValue) }
    public func asDouble() -> Double { Double(rawValue) }
    public func asInt() -> Float { Int(rawValue) }
    
}
