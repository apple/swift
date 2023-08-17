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

/// A C++ type that represents a pair of two values.
///
/// C++ standard library type `std::pair` conforms to this protocol.
public protocol CxxPair<First, Second> {
  associatedtype First
  associatedtype Second

  init(first: First, second: Second) // memberwise init, synthesized by Swift

  var first: First { get set }
  var second: Second { get set }
}
