//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

public protocol CxxOptional<Wrapped> {
  associatedtype Wrapped

  func __convertToBool() -> Bool

  var pointee: Wrapped { get }
}

extension CxxOptional {
  @inlinable
  public var hasValue: Bool {
    get {
      return __convertToBool()
    }
  }

  @inlinable
  public var value: Wrapped? {
    get {
      guard hasValue else { return nil }
      return pointee
    }
  }
}

extension Optional {
  @inlinable
  public init(fromCxx value: some CxxOptional<Wrapped>) {
    guard value.__convertToBool() else {
      self = nil
      return
    }
    self = value.pointee
  }
}
