//===--- WitnessTable.swift -----------------------------------------------===//
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
import OptimizerBridging

public struct WitnessTable : CustomStringConvertible, NoReflectionChildren {
  public let bridged: BridgedWitnessTable

  public init(bridged: BridgedWitnessTable) { self.bridged = bridged }

  public struct Entry : CustomStringConvertible, NoReflectionChildren {
    public let bridged: BridgedWitnessTableEntry
    
    internal init(bridged: BridgedWitnessTableEntry) {
      self.bridged = bridged
    }
    
    public init(_ entry: swift.SILWitnessTable.Entry) {
      self.bridged = BridgedWitnessTableEntry(entry: entry)
    }
    
    public typealias Kind = swift.SILWitnessTable.WitnessKind
    
    public var kind: Kind {
      return bridged.getKind()
    }
    
    public var methodFunction: Function? {
      assert(kind == .Method)
      return bridged.getMethodFunction().function
    }

    public var description: String {
      let stdString = bridged.getDebugDescription()
      return String(_cxxString: stdString)
    }
  }

  public struct EntryArray : BridgedRandomAccessCollection {
    fileprivate let base: BridgedWitnessTableEntryArray
    
    public var startIndex: Int { return 0 }
    public var endIndex: Int { return base.count }
    
    public subscript(_ index: Int) -> Entry {
      assert(index >= startIndex && index < endIndex)
      return Entry(bridged: base[index])
    }
  }

  public var entries: EntryArray {
    let entries = bridged.getEntries()
    return EntryArray(base: entries)
  }

  public var description: String {
    let stdString = bridged.getDebugDescription()
    return String(_cxxString: stdString)
  }
  
  static public func create(
    _ ctx: BridgedPassContext, // TODO: layering problem here with context.
    _ spec: swift.SpecializedProtocolConformance,
    _ entries: [Entry]
  ) -> WitnessTable {
    entries.withBridgedArrayRef { ref in
      WitnessTable(bridged: BridgedWitnessTable.create(ctx.getSILModuleOpaque(), spec, ref))
    }
  }
}

public struct DefaultWitnessTable : CustomStringConvertible, NoReflectionChildren {
  public let bridged: BridgedDefaultWitnessTable

  public init(bridged: BridgedDefaultWitnessTable) { self.bridged = bridged }

  public typealias Entry = WitnessTable.Entry
  public typealias EntryArray = WitnessTable.EntryArray

  public var entries: EntryArray {
    let entries = bridged.getEntries()
    return EntryArray(base: entries)
  }

  public var description: String {
    let stdString = bridged.getDebugDescription()
    return String(_cxxString: stdString)
  }
}

extension OptionalBridgedWitnessTable {
  public var witnessTable: WitnessTable? {
    if let table = table {
      return WitnessTable(bridged: BridgedWitnessTable(table: table))
    }
    return nil
  }
}

extension OptionalBridgedDefaultWitnessTable {
  public var defaultWitnessTable: DefaultWitnessTable? {
    if let table = table {
      return DefaultWitnessTable(bridged: BridgedDefaultWitnessTable(table: table))
    }
    return nil
  }
}
