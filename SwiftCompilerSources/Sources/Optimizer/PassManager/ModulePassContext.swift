//===--- ModulePassContext.swift ------------------------------------------===//
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

import SIL
import OptimizerBridging

/// The context which is passed to a `ModulePass`'s run-function.
///
/// It provides access to all functions, v-tables and witness tables of a module,
/// but it doesn't provide any APIs to modify functions.
/// In order to modify a function, a module pass must use `transform(function:)`.
struct ModulePassContext : Context, CustomStringConvertible {
  let _bridged: BridgedPassContext

  public var description: String {
    let stdString = _bridged.getModuleDescription()
    return String(_cxxString: stdString)
  }

  struct FunctionList : CollectionLikeSequence, IteratorProtocol {
    private var currentFunction: Function?
    
    fileprivate init(first: Function?) { currentFunction = first }

    mutating func next() -> Function? {
      if let f = currentFunction {
        currentFunction = BridgedPassContext.getNextFunctionInModule(f.bridged).function
        return f
      }
      return nil
    }
  }

  struct GlobalVariableList : CollectionLikeSequence, IteratorProtocol {
    private var currentGlobal: GlobalVariable?

    fileprivate init(first: GlobalVariable?) { currentGlobal = first }

    mutating func next() -> GlobalVariable? {
      if let g = currentGlobal {
        currentGlobal = BridgedPassContext.getNextGlobalInModule(g.bridged).globalVar
        return g
      }
      return nil
    }
  }

  struct VTableArray : BridgedRandomAccessCollection {
    fileprivate let bridged: BridgedPassContext.VTableArray

    var startIndex: Int { return 0 }
    var endIndex: Int { return bridged.count }

    subscript(_ index: Int) -> VTable {
      assert(index >= startIndex && index < endIndex)
      return VTable(bridged: BridgedVTable(vTable: bridged.base![index]))
    }
  }

  struct WitnessTableList : CollectionLikeSequence, IteratorProtocol {
    private var currentTable: WitnessTable?
    
    fileprivate init(first: WitnessTable?) { currentTable = first }

    mutating func next() -> WitnessTable? {
      if let t = currentTable {
        currentTable = BridgedPassContext.getNextWitnessTableInModule(t.bridged).witnessTable
        return t
      }
      return nil
    }
  }

  struct DefaultWitnessTableList : CollectionLikeSequence, IteratorProtocol {
    private var currentTable: DefaultWitnessTable?
    
    fileprivate init(first: DefaultWitnessTable?) { currentTable = first }

    mutating func next() -> DefaultWitnessTable? {
      if let t = currentTable {
        currentTable = BridgedPassContext.getNextDefaultWitnessTableInModule(t.bridged).defaultWitnessTable
        return t
      }
      return nil
    }
  }

  var functions: FunctionList {
    FunctionList(first: _bridged.getFirstFunctionInModule().function)
  }
  
  var globalVariables: GlobalVariableList {
    GlobalVariableList(first: _bridged.getFirstGlobalInModule().globalVar)
  }

  var vTables: VTableArray {
    VTableArray(bridged: _bridged.getVTables())
  }
  
  var witnessTables: WitnessTableList {
    WitnessTableList(first: _bridged.getFirstWitnessTableInModule().witnessTable)
  }

  var defaultWitnessTables: DefaultWitnessTableList {
    DefaultWitnessTableList(first: _bridged.getFirstDefaultWitnessTableInModule().defaultWitnessTable)
  }

  /// Run a closure with a `PassContext` for a function, which allows to modify that function.
  ///
  /// Only a single `transform` can be alive at the same time, i.e. it's not allowed to nest
  /// calls to `transform`.
  func transform(function: Function, _ runOnFunction: (FunctionPassContext) -> ()) {
    _bridged.beginTransformFunction(function.bridged)
    runOnFunction(FunctionPassContext(_bridged: _bridged))
    _bridged.endTransformFunction();
  }
}

extension GlobalVariable {
  func setIsLet(to value: Bool, _ context: ModulePassContext) {
    bridged.setLet(value)
  }
}
