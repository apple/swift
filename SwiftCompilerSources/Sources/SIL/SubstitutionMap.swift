//===--- SubstitutionMap.swift --------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import SILBridging

public struct SubstitutionMap {
  public let bridged: swift.SubstitutionMap

  public init(_ bridged: swift.SubstitutionMap) {
    self.bridged = bridged
  }
  
  public init() {
    self.bridged = swift.SubstitutionMap()
  }

  public var isEmpty: Bool { bridged.empty() }

  public var replacementTypes: OptionalTypeArray {
    let types = BridgedTypeArray.fromReplacementTypes(bridged)
    return OptionalTypeArray(bridged: types)
  }
}
