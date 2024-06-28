//===--- FunctionConvention.swift - function conventions ------------------===//
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

import SILBridging

/// SIL function parameter and result conventions based on the AST
/// function type and SIL stage.
///
/// Provides Results and Parameters collections. This does not know
/// anything about FunctionArguments. Use ArgumentConventions instead to
/// maps FunctionArguments down to these conventions.
///
/// The underlying FunctionType must be contextual and expanded. SIL
/// has no use for interface types or unexpanded types.
public struct FunctionConvention : CustomStringConvertible {
  let bridgedFunctionType: BridgedASTType
  let hasLoweredAddresses: Bool

  init(for bridgedFunctionType: BridgedASTType, in function: Function) {
    assert(!bridgedFunctionType.hasTypeParameter(), "requires contextual type")
    self.bridgedFunctionType = bridgedFunctionType
    self.hasLoweredAddresses = function.hasLoweredAddresses
  }

  /// All results including the error.
  public var results: Results {
    Results(bridged: bridgedFunctionType.SILFunctionType_getResultsWithError(),
      hasLoweredAddresses: hasLoweredAddresses)
  }

  public var errorResult: ResultInfo? {
    return ResultInfo(
      bridged: bridgedFunctionType.SILFunctionType_getErrorResult(),
      hasLoweredAddresses: hasLoweredAddresses)
  }

  /// Number of indirect results including the error.
  /// This avoids quadratic lazy iteration on indirectResults.count.
  public var indirectSILResultCount: Int {
    // TODO: Return packs directly in lowered-address mode
    return hasLoweredAddresses
    ? bridgedFunctionType.SILFunctionType_getNumIndirectFormalResultsWithError()
    : bridgedFunctionType.SILFunctionType_getNumPackResults()
  }

  /// Indirect results including the error.
  public var indirectSILResults: LazyFilterSequence<Results> {
    hasLoweredAddresses
    ? results.lazy.filter { $0.isSILIndirect }
    : results.lazy.filter { $0.convention == .pack }
  }

  public var parameters: Parameters {
    Parameters(bridged: bridgedFunctionType.SILFunctionType_getParameters(),
      hasLoweredAddresses: hasLoweredAddresses)
  }

  public var hasSelfParameter: Bool {
    bridgedFunctionType.SILFunctionType_hasSelfParam()
  }

  public var yields: Yields {
    Yields(bridged: bridgedFunctionType.SILFunctionType_getYields(),
      hasLoweredAddresses: hasLoweredAddresses)
  }

  /// If the function result depends on any parameters, return a
  /// Collection of LifetimeDependenceConvention indexed on the
  /// function parameter.
  public var resultDependencies: ResultDependencies? {
    let deps = bridgedFunctionType.SILFunctionType_getLifetimeDependenceInfo()
    if deps.empty() {
      return nil
    }
    return ResultDependencies(bridged: deps,
                                         parameterCount: parameters.count,
                                         hasSelfParameter: hasSelfParameter)
  }

  public var description: String {
    var str = String(taking: bridgedFunctionType.getDebugDescription())
    parameters.forEach { str += "\nparameter: " + $0.description }
    results.forEach { str += "\n   result: " + $0.description }
    str += (hasLoweredAddresses ? "\n[lowered_address]" : "\n[sil_opaque]")
    if let deps = resultDependencies {
      str += "\nresult dependences \(deps)"
    }
    return str
  }
}

/// A function result type and the rules for returning it in SIL.
public struct ResultInfo : CustomStringConvertible {
  /// The unsubstituted parameter type that describes the abstract
  /// calling convention of the parameter.
  ///
  /// TODO: For most purposes, you probably want \c returnValueType.
  public let type: BridgedASTType
  public let convention: ResultConvention
  public let hasLoweredAddresses: Bool

  /// Is this result returned indirectly in SIL? Most formally
  /// indirect results can be returned directly in SIL. This depends
  /// on whether the calling function has lowered addresses.
  public var isSILIndirect: Bool {
    switch convention {
    case .indirect:
      return hasLoweredAddresses || type.isOpenedExistentialWithError()
    case .pack:
      return true
    case .owned, .unowned, .unownedInnerPointer, .autoreleased:
      return false
    }
  }

  public var description: String {
    convention.description + ": "
    + String(taking: type.getDebugDescription())
  }
}

extension FunctionConvention {
  public struct Results : Collection {
    let bridged: BridgedResultInfoArray
    let hasLoweredAddresses: Bool

    public var startIndex: Int { 0 }

    public var endIndex: Int { bridged.count() }

    public func index(after index: Int) -> Int {
      return index + 1
    }

    public subscript(_ index: Int) -> ResultInfo {
      return ResultInfo(bridged: bridged.at(index),
        hasLoweredAddresses: hasLoweredAddresses)
    }
  }
}

public struct ParameterInfo : CustomStringConvertible {
  /// The parameter type that describes the abstract calling
  /// convention of the parameter.
  public let type: BridgedASTType
  public let convention: ArgumentConvention
  public let options: UInt8
  public let hasLoweredAddresses: Bool

  public init(type: BridgedASTType, convention: ArgumentConvention, options: UInt8, hasLoweredAddresses: Bool) {
    self.type = type
    self.convention = convention
    self.options = options
    self.hasLoweredAddresses = hasLoweredAddresses
  }

  /// Is this parameter passed indirectly in SIL? Most formally
  /// indirect results can be passed directly in SIL (opaque values
  /// mode). This depends on whether the calling function has lowered
  /// addresses.
  public var isSILIndirect: Bool {
    switch convention {
    case .indirectIn, .indirectInGuaranteed:
      return hasLoweredAddresses || type.isOpenedExistentialWithError()
    case .indirectInout, .indirectInoutAliasable, .indirectInCXX:
      return true
    case .directOwned, .directUnowned, .directGuaranteed:
      return false
    case .packInout, .packOwned, .packGuaranteed:
      return true
    case .indirectOut, .packOut:
      fatalError("invalid parameter convention")
    }
  }

  public var description: String {
    convention.description + ": "
    + String(taking: type.getDebugDescription())
  }
}

extension FunctionConvention {
  public struct Parameters : Collection {
    let bridged: BridgedParameterInfoArray
    let hasLoweredAddresses: Bool

    public var startIndex: Int { 0 }

    public var endIndex: Int { bridged.count() }

    public func index(after index: Int) -> Int {
      return index + 1
    }

    public subscript(_ index: Int) -> ParameterInfo {
      return ParameterInfo(bridged: bridged.at(index),
        hasLoweredAddresses: hasLoweredAddresses)
    }
  }
}

extension FunctionConvention {
  public struct Yields : Collection {
    let bridged: BridgedYieldInfoArray
    let hasLoweredAddresses: Bool

    public var startIndex: Int { 0 }

    public var endIndex: Int { bridged.count() }

    public func index(after index: Int) -> Int {
      return index + 1
    }

    public subscript(_ index: Int) -> ParameterInfo {
      return ParameterInfo(bridged: bridged.at(index),
        hasLoweredAddresses: hasLoweredAddresses)
    }
  }
}

public enum LifetimeDependenceConvention : CustomStringConvertible {
  case inherit
  case scope

  public var description: String {
    switch self {
    case .inherit:
      return "inherit"
    case .scope:
      return "scope"
    }
  }
}

extension FunctionConvention {
  /// Collection of LifetimeDependenceConvention? that parallels parameters.
  public struct ResultDependencies : Collection, CustomStringConvertible {
    let bridged: BridgedLifetimeDependenceInfo
    let paramCount: Int
    let hasSelfParam: Bool

    init(bridged: BridgedLifetimeDependenceInfo, parameterCount: Int,
         hasSelfParameter: Bool) {
      assert(!bridged.empty())
      self.bridged = bridged
      self.paramCount = parameterCount
      self.hasSelfParam = hasSelfParameter
    }

    public var startIndex: Int { 0 }

    public var endIndex: Int { paramCount }

    public func index(after index: Int) -> Int {
      return index + 1
    }

    public subscript(_ index: Int) -> LifetimeDependenceConvention? {
      let inherit = bridged.checkInherit(bridgedIndex(parameterIndex: index))
      let scope = bridged.checkScope(bridgedIndex(parameterIndex: index))
      if inherit {
        assert(!scope, "mutualy exclusive lifetime specifiers")
        return .inherit
      }
      if scope {
        return .scope
      }
      return nil
    }

    private func bridgedIndex(parameterIndex: Int) -> Int {
      return parameterIndex
    }

    public var description: String {
      String(taking: bridged.getDebugDescription()) +
        "\nparamCount: \(paramCount) self: \(hasSelfParam)"
    }
  }
}

public enum ResultConvention : CustomStringConvertible {
  /// This result is returned indirectly, i.e. by passing the address of an uninitialized object in memory.  The callee is responsible for leaving an initialized object at this address.  The callee may assume that the address does not alias any valid object.
  case indirect

  /// The caller is responsible for destroying this return value.  Its type is non-trivial.
  case owned

  /// The caller is not responsible for destroying this return value.  Its type may be trivial, or it may simply be offered unsafely.  It is valid at the instant of the return, but further operations may invalidate it.
  case unowned

  /// The caller is not responsible for destroying this return value.  The validity of the return value is dependent on the 'self' parameter, so it may be invalidated if that parameter is released.
  case unownedInnerPointer

  /// This value has been (or may have been) returned autoreleased.  The caller should make an effort to reclaim the autorelease.  The type must be a class or class existential type, and this must be the only return value.
  case autoreleased

  /// This value is a pack that is returned indirectly by passing a pack address (which may or may not be further indirected, depending on the pact type).  The callee is responsible for leaving an initialized object in each element of the pack.
  case pack

  /// Does this result convention require indirect storage? This reflects a FunctionType's conventions, as opposed to the SIL conventions that dictate SILValue types.
  public var isASTIndirect: Bool {
    switch self {
    case .indirect, .pack:
      return true
    default:
      return false
    }
  }
  public var isASTDirect: Bool {
    return !isASTIndirect
  }

  public var description: String {
    switch self {
    case .indirect:
      return "indirect"
    case .owned:
      return "owned"
    case .unowned:
      return "unowned"
    case .unownedInnerPointer:
      return "unownedInnerPointer"
    case .autoreleased:
      return "autoreleased"
    case .pack:
      return "pack"
    }
  }
}

// Bridging utilities

extension ResultInfo {
  init(bridged: BridgedResultInfo, hasLoweredAddresses: Bool) {
    self.type = BridgedASTType(type: bridged.type)
    self.convention = ResultConvention(bridged: bridged.convention)
    self.hasLoweredAddresses = hasLoweredAddresses
  }
  init(bridged: OptionalBridgedResultInfo, hasLoweredAddresses: Bool) {
    self.type = BridgedASTType(type: bridged.type!)
    self.convention = ResultConvention(bridged: bridged.convention)
    self.hasLoweredAddresses = hasLoweredAddresses
  }
}

extension ResultConvention {
  init(bridged: BridgedResultConvention) {
    switch bridged {
      case .Indirect:            self = .indirect
      case .Owned:               self = .owned
      case .Unowned:             self = .unowned
      case .UnownedInnerPointer: self = .unownedInnerPointer
      case .Autoreleased:        self = .autoreleased
      case .Pack:                self = .pack
      default:
        fatalError("unsupported result convention")
    }
  }
}

extension ParameterInfo {
  init(bridged: BridgedParameterInfo, hasLoweredAddresses: Bool) {
    self.type = BridgedASTType(type: bridged.type)
    self.convention = bridged.convention.convention
    self.options = bridged.options
    self.hasLoweredAddresses = hasLoweredAddresses
  }

  public var _bridged: BridgedParameterInfo {
    BridgedParameterInfo(type.type!, convention.bridged, options)
  }
}
