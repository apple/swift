//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

@_exported import SwiftGlibc // Clang module

public let MAP_FAILED =
  UnsafeMutableRawPointer(bitPattern: -1)! as UnsafeMutableRawPointer!

@available(swift, deprecated: 3.0, message: "Please use '.pi' to get the value of correct type and avoid casting.")
public let M_PI = Double.pi

@available(swift, deprecated: 3.0, message: "Please use '.pi / 2' to get the value of correct type and avoid casting.")
public let M_PI_2 = Double.pi / 2

@available(swift, deprecated: 3.0, message: "Please use '.pi / 4' to get the value of correct type and avoid casting.")
public let M_PI_4 = Double.pi / 4
