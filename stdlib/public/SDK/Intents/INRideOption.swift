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

@_exported import Intents
import Foundation

#if os(iOS) || os(watchOS)

@available(iOS 10.0, watchOS 3.2, *)
extension INRideOption {
  @available(iOS 10.0, watchOS 3.2, *)
  @available(swift, obsoleted: 4)
  @nonobjc
  public var usesMeteredFare: NSNumber? {
    get {
      return __usesMeteredFare
    }
    set(newUsesMeteredFare) {
      __usesMeteredFare = newUsesMeteredFare
    }
  }

  @available(iOS 10.0, watchOS 3.2, *)
  @available(swift, introduced: 4.0)
  @nonobjc
  public var usesMeteredFare: Bool? {
    get {
      return __usesMeteredFare?.boolValue
    }
    set(newUsesMeteredFare) {
      __usesMeteredFare = newUsesMeteredFare.map { NSNumber(value: $0) }
    }
  }
}

#endif
