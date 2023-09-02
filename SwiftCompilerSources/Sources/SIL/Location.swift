//===--- Location.swift - Source location ---------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import SILBridging

public struct Location: Equatable, CustomStringConvertible {
  let bridged: swift.SILDebugLocation

  public var description: String {
    let stdString = bridged.getDebugDescription()
    return String(_cxxString: stdString)
  }
  
  public var sourceLoc: SourceLoc? {
    return SourceLoc(bridged: bridged.getLocation().getSourceLoc())
  }

  /// Keeps the debug scope but marks it as auto-generated.
  public var autoGenerated: Location {
    Location(bridged: bridged.getAutogeneratedLocation())
  }

  public var hasValidLineNumber: Bool { bridged.hasValidLineNumber() }
  public var isAutoGenerated: Bool { bridged.isAutoGenerated() }

  public var isDebugSteppable: Bool { hasValidLineNumber && !isAutoGenerated }

  public static func ==(lhs: Location, rhs: Location) -> Bool {
    lhs.bridged.isEqualTo(rhs.bridged)
  }

  public func hasSameSourceLocation(as other: Location) -> Bool {
    bridged.hasSameSourceLocation(other.bridged)
  }

  public static var artificialUnreachableLocation: Location {
    Location(bridged: swift.SILDebugLocation.getArtificialUnreachableLocation())
  }
}
