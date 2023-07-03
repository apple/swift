//===--- Effects.swift - Defines function effects -------------------------===//
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

/// All effects for a function.
///
/// It consists of escape and side effects.
public struct FunctionEffects : CustomStringConvertible, NoReflectionChildren {

  public var escapeEffects = EscapeEffects(arguments: [])

  // If nil, the side effects are not computed yet and clients need to assume
  // the worst effects (using `Function.getDefinedSideEffects()`).
  public var sideEffects: SideEffects?

  public init() {}

  public init(copiedFrom src: FunctionEffects, resultArgDelta: Int) {
    self.escapeEffects = EscapeEffects(arguments: src.escapeEffects.arguments.compactMap {
        EscapeEffects.ArgumentEffect(copiedFrom: $0, resultArgDelta: resultArgDelta)
      })
    // Cannot copy side effects
    self.sideEffects = nil
  }

  public var description: String {
    var numArgEffects = escapeEffects.arguments.reduce(0, { max($0, $1.argumentIndex) }) + 1
    if let sideEffects = sideEffects {
      numArgEffects = max(numArgEffects, sideEffects.arguments.count)
    }
    var result = ""
    for argIdx in 0..<numArgEffects {
      var argStrings: [String] = []
      for escEffect in escapeEffects.arguments where escEffect.argumentIndex == argIdx {
        argStrings.append(escEffect.bodyDescription)
      }
      if let sideEffects = sideEffects, argIdx < sideEffects.arguments.count {
        let argDescription = sideEffects.arguments[argIdx].bodyDescription
        if !argDescription.isEmpty {
          argStrings.append(argDescription)
        }
      }
      if !argStrings.isEmpty {
        result += "[%\(argIdx): " + argStrings.joined(separator: ", ") + "]\n"
      }
    }
    if let sideEffects = sideEffects {
      result += "[global: \(sideEffects.global)]\n"
    }
    return result
  }
}

extension Function {
  
  /// Returns the global side effects of the function.
  public func getSideEffects() -> SideEffects.GlobalEffects {
    if let sideEffects = effects.sideEffects {
      /// There are computed side effects.
      return sideEffects.accumulatedEffects
    } else {
      
      var effects = definedGlobalEffects

      // Even a `[readnone]` function can read from indirect arguments.
      if (0..<numArguments).contains(where: {getArgumentConvention(for: $0).isIndirectIn}) {
        effects.memory.read = true
      }
      // Even `[readnone]` and `[readonly]` functions write to indirect results.
      if numIndirectResultArguments > 0 {
        effects.memory.write = true
      }
      return effects
    }
  }

  /// Returns the side effects for a function argument.
  ///
  /// The `argument` can be a function argument in this function or an apply argument in a caller.
  public func getSideEffects(forArgument argument: ProjectedValue,
                             atIndex argumentIndex: Int,
                             withConvention convention: ArgumentConvention) -> SideEffects.GlobalEffects {
    var result = SideEffects.GlobalEffects()

    // Effects are only defined for operations which don't involve a load.
    // In case the argument's path involves a load we need to return the global effects.
    if argument.value.type.isAddress {
      // Indirect arguments:
      if argument.path.mayHaveClassProjection {
        // For example:
        //   bb0(%0: $*C):
        //     %1 = load %0               // the involved load
        //     %2 = ref_element_addr %1   // class projection
        return getSideEffects()
      }
    } else {
      // Direct arguments:
      if argument.path.mayHaveTwoClassProjections {
        // For example:
        //   bb0(%0: $C):
        //     %1 = ref_element_addr %1   // first class projection
        //     %2 = load %1               // the involved load
        //     %3 = ref_element_addr %2   // second class projection
        return getSideEffects()

      } else if argument.path.mayHaveClassProjection {
        // For example:
        //   bb0(%0: $C):
        //     %1 = ref_element_addr %1   // class projection
        //     %2 = load [take] %1        // the involved load
        //     destroy_value %2
        result.ownership = getSideEffects().ownership
      }
    }

    if let sideEffects = effects.sideEffects {
      /// There are computed side effects.
      let argEffect = sideEffects.getArgumentEffects(for: argumentIndex)
      if let effectPath = argEffect.read, effectPath.mayOverlap(with: argument.path) {
        result.memory.read = true
      }
      if let effectPath = argEffect.write, effectPath.mayOverlap(with: argument.path) {
        result.memory.write = true
      }
      if let effectPath = argEffect.copy, effectPath.mayOverlap(with: argument.path) {
        result.ownership.copy = true
      }
      if let effectPath = argEffect.destroy, effectPath.mayOverlap(with: argument.path) {
        result.ownership.destroy = true
      }
      return result
    } else {
      /// Even for defined effects, there might be additional effects due to the argument conventions.
      var result = definedGlobalEffects
      if convention.isIndirectIn {
        // Even a `[readnone]` function can read from an indirect argument.
        result.memory.read = true
      } else if convention.isIndirectOut {
        // Even `[readnone]` and `[readonly]` functions write to indirect results.
        result.memory.write = true
      }
      return result.restrictedTo(argument: argument, withConvention: convention)
    }
  }

  /// Global effect of the function, defined by effect attributes.
  public var definedGlobalEffects: SideEffects.GlobalEffects {
    switch name {
    case "_swift_stdlib_malloc_size", "_swift_stdlib_has_malloc_size":
      // These C runtime functions, which are used in the array implementation, have defined effects.
      return SideEffects.GlobalEffects(memory: SideEffects.Memory(read: true))
    default:
      break
    }
    if isProgramTerminationPoint {
      // We can ignore any memory writes in a program termination point, because it's not relevant
      // for the caller. But we need to consider memory reads, otherwise preceeding memory writes
      // would be eliminated by dead-store-elimination in the caller. E.g. String initialization
      // for error strings which are printed by the program termination point.
      // Regarding ownership: a program termination point must not touch any reference counted objects.
      return SideEffects.GlobalEffects(memory: SideEffects.Memory(read: true))
    }
    var result = SideEffects.GlobalEffects.worstEffects
    switch effectAttribute {
    case .none:
      // The common case: there is no effect attribute, so we have to assume the worst effects.
      break
    case .readNone:
      result.memory.read = false
      result.memory.write = false
      result.ownership.destroy = false
      result.allocates = false
    case .readOnly:
      result.memory.write = false
      result.ownership.destroy = false
      result.allocates = false
    case .releaseNone:
      result.ownership.destroy = false
    }
    return result
  }
}

/// Escape effects.
///
/// The escape effects describe which arguments are not escaping or escaping to
/// the return value or other arguments.
public struct EscapeEffects : CustomStringConvertible, NoReflectionChildren {
  public var arguments: [ArgumentEffect]

  public func canEscape(argumentIndex: Int, path: SmallProjectionPath) -> Bool {
    return !arguments.contains(where: {
      if case .notEscaping = $0.kind, $0.argumentIndex == argumentIndex {
        if path.matches(pattern: $0.pathPattern) {
          return true
        }
      }
      return false
    })
  }

  public var description: String {
    let maxArgIdx = arguments.reduce(0) { max($0, $1.argumentIndex) }
    var result = ""
    for argIdx in 0...maxArgIdx {
      var argStrings: [String] = []
      for escEffect in arguments where escEffect.argumentIndex == argIdx {
        argStrings.append(escEffect.bodyDescription)
      }
      if !argStrings.isEmpty {
        result += "[%\(argIdx): " + argStrings.joined(separator: ", ") + "]\n"
      }
    }
    return result
  }

  /// An escape effect on a function argument.
  public struct ArgumentEffect : Equatable, CustomStringConvertible, NoReflectionChildren {

    public enum Kind : Equatable {
      /// The argument value does not escape.
      ///
      /// Syntax examples:
      ///    [%0: noescape]      // argument 0 does not escape
      ///    [%0: noescape **]   // argument 0 and all transitively contained values do not escape
      ///
      case notEscaping
      
      /// The argument value escapes to the function return value.
      ///
      /// Syntax examples:
      ///    [%0: escape s1 => %r]   // field 2 of argument 0 exclusively escapes via return.
      ///    [%0: escape s1 -> %r]   // field 2 of argument 0 - and other values - escape via return
      ///
      /// The `isExclusive` flag is true if only the argument escapes, but nothing else escapes to
      /// the return value.
      /// For example, `isExclusive` is true for the following function:
      ///
      ///   @_effect(escaping c => return)
      ///   func exclusiveEscape(_ c: Class) -> Class { return c }
      ///
      /// but not in this case:
      ///
      ///   var global: Class
      ///
      ///   @_effect(escaping c -> return)
      ///   func notExclusiveEscape(_ c: Class) -> Class { return cond ? c : global }
      ///
      /// Also, if `isExclusive` is true, there must not be a store in the chain from the argument to
      /// the return, e.g.
      ///
      ///   @_effect(escaping x -> return)
      ///   func notExclusiveEscape(_ s: String) -> Class {
      ///     c = new Class()
      ///     c.s = s             // Store, which prevents the effect to be `isExclusive`
      ///     return s
      ///   }
      case escapingToReturn(toPath: SmallProjectionPath, isExclusive: Bool)

      /// Like `escapingToReturn`, but the argument escapes to another argument.
      ///
      /// Example: The argument effects of
      ///
      ///   func argToArgEscape(_ r: inout Class, _ c: Class) { r = c }
      ///
      /// would be
      ///    [%1: escape => %0]   // Argument 1 escapes to argument 0
      ///
      /// It's not allowed that the argument (or a derived value) is _stored_ to an object which is loaded from somewhere.
      /// In the following example `c` is loaded from the indirect inout argument and `s` is stored to a field of `c`:
      ///
      ///   func noEscapeEffect(_ c: inout Class, s: String) {
      ///     c.s = s
      ///   }
      ///
      /// In this case there is no escaping effect from `s` to `c`.
      /// Note that theoretically this rule also applies to the `escapingToReturn` effect, but it's impossible
      /// to construct such a situation for an argument which is only escaping to the function return.
      ///
      /// The `escapingToArgument` doesn't have an `isExclusive` flag, because an argument-to-argument escape
      /// always involves a store, which makes an exclusive escape impossible.
      case escapingToArgument(toArgumentIndex: Int, toPath: SmallProjectionPath)
    }

    /// To which argument does this effect apply to?
    public let argumentIndex: Int
    
    /// To which projection(s) of the argument does this effect apply to?
    public let pathPattern: SmallProjectionPath
    
    /// The kind of effect.
    public let kind: Kind
    
    /// True, if this effect is derived in an optimization pass.
    /// False, if this effect is defined in the Swift source code.
    public let isDerived: Bool

    public init(_ kind: Kind, argumentIndex: Int, pathPattern: SmallProjectionPath, isDerived: Bool = true) {
      self.argumentIndex = argumentIndex
      self.pathPattern = pathPattern
      self.kind = kind
      self.isDerived = isDerived
    }

    /// Copy the ArgumentEffect by applying a delta on the argument index.
    ///
    /// This is used when copying argument effects for specialized functions where
    /// the indirect result is converted to a direct return value (in this case the
    /// `resultArgDelta` is -1).
    init?(copiedFrom srcEffect: ArgumentEffect, resultArgDelta: Int) {
      if srcEffect.argumentIndex + resultArgDelta < 0 {
        return nil
      }
      self.argumentIndex = srcEffect.argumentIndex + resultArgDelta
      self.pathPattern = srcEffect.pathPattern
      self.isDerived = srcEffect.isDerived

      switch srcEffect.kind {
      case .notEscaping:
        self.kind = .notEscaping

      case .escapingToReturn(let toPath, let exclusive):
        if resultArgDelta > 0 {
          if resultArgDelta != 1 {
            return nil
          }
          self.kind = .escapingToArgument(toArgumentIndex: 0, toPath: toPath)
        } else {
          self.kind = .escapingToReturn(toPath: toPath, isExclusive: exclusive)
        }
      case .escapingToArgument(let toArgIdx, let toPath):
        let resultingToArgIdx = toArgIdx + resultArgDelta
        if resultingToArgIdx < 0 {
          if resultingToArgIdx != -1 {
            return nil
          }
          self.kind = .escapingToReturn(toPath: toPath, isExclusive: false)
        } else {
          self.kind = .escapingToArgument(toArgumentIndex: resultingToArgIdx, toPath: toPath)
        }
      }
    }

    public func matches(_ rhsArgIdx: Int, _ rhsPath: SmallProjectionPath) -> Bool {
      if argumentIndex != rhsArgIdx {
        return false
      }
      return rhsPath.matches(pattern: pathPattern)
    }

    public var bodyDescription: String {
      let patternStr = (isDerived ? "" : "!") + (pathPattern.isEmpty ? "" : " \(pathPattern)")
      switch kind {
        case .notEscaping:
          return "noescape\(patternStr)"
        case .escapingToReturn(let toPath, let exclusive):
          let toPathStr = (toPath.isEmpty ? "" : ".\(toPath)")
          return "escape\(patternStr) \(exclusive ? "=>" : "->") %r\(toPathStr)"
        case .escapingToArgument(let toArgIdx, let toPath):
          let toPathStr = (toPath.isEmpty ? "" : ".\(toPath)")
          return "escape\(patternStr) -> %\(toArgIdx)\(toPathStr)"
      }
    }

    public var description: String {
      "\(argumentIndex): \(bodyDescription)"
    }
  }
}

/// Side effects.
///
/// Side effects describe the memory (read, write) and ownership (copy, destroy)
/// of the function arguments and the function as a whole.
public struct SideEffects : CustomStringConvertible, NoReflectionChildren {
  /// Effects, which can be attributed to a specific argument.
  ///
  /// This array is indexed by the argument index. Arguments which indices, which
  /// are not included in this array, are defined to have no effects.
  public let arguments: [ArgumentEffects]
  
  /// Effects, which cannot be attributed to a specific argument.
  public let global: GlobalEffects
  
  public init(arguments: [ArgumentEffects], global: GlobalEffects) {
    self.arguments = arguments
    self.global = global
  }

  /// Returns the effects of an argument.
  ///
  /// In constrast to using `arguments` directly, it's valid to have an `argumentIndex`
  /// which is larger than the number of elements in `arguments`.
  public func getArgumentEffects(for argumentIndex: Int) -> ArgumentEffects {
    if argumentIndex < arguments.count {
      return arguments[argumentIndex]
    }
    return ArgumentEffects()
  }

  /// The "accumulated" effects of the function, which includes the `global` effects and
  /// all argument effects.
  public var accumulatedEffects: GlobalEffects {
    var result = global
    for argEffect in arguments {
      if argEffect.read != nil    { result.memory.read = true }
      if argEffect.write != nil   { result.memory.write = true }
      if argEffect.copy != nil    { result.ownership.copy = true }
      if argEffect.destroy != nil { result.ownership.destroy = true }
    }
    return result
  }

  public var description: String {
    var result = ""
    for (argIdx, argument) in arguments.enumerated() {
      let argDescription = argument.bodyDescription
      if !argDescription.isEmpty {
        result += "[%\(argIdx): \(argDescription)]\n"
      }
    }
    result += "[global: \(global)]\n"
    return result
  }
  
  /// Side-effects of a specific function argument.
  ///
  /// The paths describe what (projeted) values of an argument are affected.
  /// If a path is nil, than there is no such effect on the argument.
  ///
  /// A path can contain any projection or wildcards, as long as there is no load involved.
  /// In other words, an effect path can refer to the argument value directly or to anything the
  /// argument "points to", but does not cover anything which is loaded from memory.
  /// For example, if the `write` path for an inout argument is nil, there is no write to the inout address.
  /// But there might very well be a write to a class field of a reference which is loaded from the inout.
  /// ```
  ///   bb0(%0 : $*X):                   // inout
  ///     %1 = load %0                   // read path = v**
  ///     %2 = ref_element_addr %1, #f
  ///     store %x to %2                 // not covered by argument effects! -> goes to global effects
  /// ```
  ///
  /// For direct argument it's different, because a direct argument (which contains a class reference) can point
  /// to a class field.
  /// ```
  ///   bb0(%0 : $X):                    // direct argument
  ///     %1 = ref_element_addr %0, #f
  ///     store %x to %1                 // write path = c*.v**
  ///     %2 = load %1                   // read path = c*.v**
  ///     %3 = ref_element_addr %2, #f
  ///     store %x to %3                 // not covered by argument effects! -> goes to global effects
  /// ```
  public struct ArgumentEffects : Equatable, CustomStringConvertible, NoReflectionChildren {

    /// If not nil, the function may read from the argument at the path.
    public var read: SmallProjectionPath?

    /// If not nil, the function may write to the argument at the path.
    public var write: SmallProjectionPath?
    
    /// If not nil, the function may copy/retain the argument at the path (only non-trivial values).
    public var copy: SmallProjectionPath?
    
    /// If not nil, the function may destroy/release the argument at the path (only non-trivial values).
    public var destroy: SmallProjectionPath?

    public init() {}

    var isEmpty: Bool { self == ArgumentEffects() }

    /// The `description` without the square brackets
    public var bodyDescription: String {
      var results: [String] = []
      if let path = read    { results.append("read \(path)") }
      if let path = write   { results.append("write \(path)") }
      if let path = copy    { results.append("copy \(path)") }
      if let path = destroy { results.append("destroy \(path)") }
      return results.joined(separator: ", ")
    }

    public var description: String { "[\(bodyDescription)]" }
  }

  /// "Global" effects of the function.
  ///
  /// Global effects are effects which cannot be associated with function arguments,
  /// for example reading from a global variable or reading from loaded value from an argument.
  public struct GlobalEffects : Equatable, CustomStringConvertible, NoReflectionChildren {

    /// Memory reads and writes.
    public var memory: Memory
    
    /// Copies and destroys.
    public var ownership: Ownership

    /// If true, the function may allocate an object.
    ///
    /// This only includes allocations, which escape the function. Local allocations
    /// are not observable form the outside and are therefore not considered.
    public var allocates: Bool

    /// If true, destroys of lexical values may not be hoisted over applies of
    /// the function.
    ///
    /// This is true when the function (or a callee, transitively) contains a
    /// deinit barrier instruction.
    public var isDeinitBarrier: Bool

    /// When called with default arguments, it creates an "effect-free" GlobalEffects.
    public init(memory: Memory = Memory(read: false, write: false),
                ownership: Ownership = Ownership(copy: false, destroy: false),
                allocates: Bool = false,
                isDeinitBarrier: Bool = false) {
      self.memory = memory
      self.ownership = ownership
      self.allocates = allocates
      self.isDeinitBarrier = isDeinitBarrier
    }

    public mutating func merge(with other: GlobalEffects) {
      memory.merge(with: other.memory)
      ownership.merge(with: other.ownership)
      allocates = allocates || other.allocates
      isDeinitBarrier = isDeinitBarrier || other.isDeinitBarrier
    }

    /// Removes effects, which cannot occur for an `argument` value with a given `convention`.
    public func restrictedTo(argument: ProjectedValue, withConvention convention: ArgumentConvention) -> GlobalEffects {
      var result = self
      let isTrivial = argument.value.hasTrivialNonPointerType
      if isTrivial {
        // There cannot be any ownership effects on trivial arguments.
        result.ownership = SideEffects.Ownership()
      }
      switch convention {
      case .indirectIn, .packOwned:
        result.memory.write = false
      case .indirectInGuaranteed, .packGuaranteed:
        result.memory.write = false
        result.ownership.destroy = false
      case .indirectOut, .packOut, .packInout:
        result.memory.read = false
        result.ownership.copy = false
        result.ownership.destroy = false

      case .directGuaranteed:
        // Note that `directGuaranteed` still has a "destroy" effect, because an object stored in
        // a class property could be destroyed.
        if !argument.path.mayHaveClassProjection {
          result.ownership.destroy = false
        }
        fallthrough
      case .directOwned, .directUnowned:
        if isTrivial {
          // Trivial direct arguments cannot have class properties which could be loaded from/stored to.
          result.memory = SideEffects.Memory()
        }

      case .indirectInout, .indirectInoutAliasable:
        break
      }
      return result
    }

    public static var worstEffects: GlobalEffects {
      GlobalEffects(memory: .worstEffects, ownership: .worstEffects, allocates: true, isDeinitBarrier: true)
    }

    public var description: String {
      var res: [String] = [memory.description, ownership.description].filter { !$0.isEmpty }
      if allocates { res += ["allocate"] }
      if isDeinitBarrier { res += ["deinit_barrier"] }
      return res.joined(separator: ",")
    }
  }

  /// Memory read and write effects.
  public struct Memory : Equatable, CustomStringConvertible, NoReflectionChildren {
    public var read: Bool
    public var write: Bool

    public init(read: Bool = false, write: Bool = false) {
      self.read = read
      self.write = write
    }

    public var description: String {
      switch (read, write) {
        case (false, false): return ""
        case (false, true):  return "write"
        case (true, false):  return "read"
        case (true, true):   return "read,write"
      }
    }

    public mutating func merge(with other: Memory) {
      read = read || other.read
      write = write || other.write
    }

    public static var worstEffects: Memory {
      Memory(read: true, write: true)
    }
  }

  /// Copy and destroy effects.
  public struct Ownership : Equatable, CustomStringConvertible, NoReflectionChildren {
    public var copy: Bool
    public var destroy: Bool

    public init(copy: Bool = false, destroy: Bool = false) {
      self.copy = copy
      self.destroy = destroy
    }

    public var description: String {
      switch (copy, destroy) {
        case (false, false): return ""
        case (false, true):  return "destroy"
        case (true, false):  return "copy"
        case (true, true):   return "copy,destroy"
      }
    }

    public mutating func merge(with other: Ownership) {
      copy = copy || other.copy
      destroy = destroy || other.destroy
    }

    public static var worstEffects: Ownership {
      Ownership(copy: true, destroy: true)
    }
  }
}

//===----------------------------------------------------------------------===//
//                               Parsing
//===----------------------------------------------------------------------===//

extension StringParser {

  mutating func parseEffectFromSource(for function: Function,
                            params: Dictionary<String, Int>) throws -> EscapeEffects.ArgumentEffect {
    if consume("notEscaping") {
      let argIdx = try parseArgumentIndexFromSource(for: function, params: params)
      let path = try parsePathPatternFromSource(for: function, type: function.argumentTypes[argIdx])
      return EscapeEffects.ArgumentEffect(.notEscaping, argumentIndex: argIdx, pathPattern: path, isDerived: false)
    }
    if consume("escaping") {
      let fromArgIdx = try parseArgumentIndexFromSource(for: function, params: params)
      let fromPath = try parsePathPatternFromSource(for: function, type: function.argumentTypes[fromArgIdx])
      let exclusive = try parseEscapingArrow()
      
      if consume("return") {
        if function.numIndirectResultArguments > 0 {
          if function.numIndirectResultArguments != 1 {
            try throwError("multi-value returns not supported yet")
          }
          let toPath = try parsePathPatternFromSource(for: function, type: function.argumentTypes[0])

          // Exclusive escapes are ignored for indirect return values.
          return EscapeEffects.ArgumentEffect(.escapingToArgument(toArgumentIndex: 0, toPath: toPath),
                                      argumentIndex: fromArgIdx, pathPattern: fromPath, isDerived: false)
        }
        let toPath = try parsePathPatternFromSource(for: function, type: function.resultType)
        return EscapeEffects.ArgumentEffect(.escapingToReturn(toPath: toPath, isExclusive: exclusive),
                                    argumentIndex: fromArgIdx, pathPattern: fromPath, isDerived: false)
      }
      if exclusive {
        try throwError("exclusive escapes to arguments are not supported")
      }
      let toArgIdx = try parseArgumentIndexFromSource(for: function, params: params)
      let toPath = try parsePathPatternFromSource(for: function, type: function.argumentTypes[toArgIdx])
      return EscapeEffects.ArgumentEffect(.escapingToArgument(toArgumentIndex: toArgIdx, toPath: toPath),
                                  argumentIndex: fromArgIdx, pathPattern: fromPath, isDerived: false)
    }
    try throwError("unknown effect")
  }

  private mutating func parseArgumentIndexFromSource(for function: Function,
                                             params: Dictionary<String, Int>) throws -> Int {
    if consume("self") {
      if !function.hasSelfArgument {
        try throwError("function does not have a self argument")
      }
      return function.selfArgumentIndex
    }
    if let name = consumeIdentifier() {
      guard let idx = params[name] else {
        try throwError("parameter not found")
      }
      return idx + function.numIndirectResultArguments
    }
    try throwError("parameter name expected")
  }
  
  private mutating func parsePathPatternFromSource(for function: Function, type: Type) throws -> SmallProjectionPath {
    if consume(".") {
      return try parseProjectionPathFromSource(for: function, type: type)
    }
    if !type.isClass {
      try throwError("the value is not a class - add 'anyValueFields'")
    }
    return SmallProjectionPath()
  }

  mutating func parseEffectsFromSIL(to effects: inout FunctionEffects) throws {
    if consume("global") {
      if !consume(":") {
        try throwError("expected ':'")
      }
      try parseGlobalSideEffectsFromSIL(to: &effects)
      return
    }
    let argumentIndex = try parseArgumentIndexFromSIL()
    if !consume(":") {
      try throwError("expected ':'")
    }
    try parseEffectsFromSIL(argumentIndex: argumentIndex, to: &effects)
  }
  
  mutating func parseEffectsFromSIL(argumentIndex: Int, to effects: inout FunctionEffects) throws {
    repeat {
      if consume("noescape") {
        let isDerived = !consume("!")
        let path = try parseProjectionPathFromSIL()
        let effect = EscapeEffects.ArgumentEffect(.notEscaping, argumentIndex: argumentIndex,
                                                  pathPattern: path, isDerived: isDerived)
        effects.escapeEffects.arguments.append(effect)

      } else if consume("escape") {
        let isDerived = !consume("!")
        let fromPath = try parseProjectionPathFromSIL()
        let exclusive = try parseEscapingArrow()
        let effect: EscapeEffects.ArgumentEffect
        if consume("%r") {
          let toPath = consume(".") ? try parseProjectionPathFromSIL() : SmallProjectionPath()
          effect = EscapeEffects.ArgumentEffect(.escapingToReturn(toPath: toPath, isExclusive: exclusive),
                                                argumentIndex: argumentIndex, pathPattern: fromPath, isDerived: isDerived)
        } else {
          if exclusive {
            try throwError("exclusive escapes to arguments are not supported")
          }
          let toArgIdx = try parseArgumentIndexFromSIL()
          let toPath = consume(".") ? try parseProjectionPathFromSIL() : SmallProjectionPath()
          effect = EscapeEffects.ArgumentEffect(.escapingToArgument(toArgumentIndex: toArgIdx, toPath: toPath),
                                                argumentIndex: argumentIndex, pathPattern: fromPath, isDerived: isDerived)
        }
        effects.escapeEffects.arguments.append(effect)

      } else if consume("read") {
        try parseSideEffectPath(\.read, for: argumentIndex)
      } else if consume("write") {
        try parseSideEffectPath(\.write, for: argumentIndex)
      } else if consume("copy") {
        try parseSideEffectPath(\.copy, for: argumentIndex)
      } else if consume("destroy") {
        try parseSideEffectPath(\.destroy, for: argumentIndex)
      } else {
        try throwError("unknown effect")
      }
    } while consume(",")

    func parseSideEffectPath(_ e: WritableKeyPath<SideEffects.ArgumentEffects, SmallProjectionPath?>, for argumentIndex: Int) throws {
      var arguments = effects.sideEffects?.arguments ?? []
      while arguments.count <= argumentIndex {
        arguments.append(SideEffects.ArgumentEffects())
      }
      arguments[argumentIndex][keyPath: e] = try parseProjectionPathFromSIL()
      let global = effects.sideEffects?.global ?? SideEffects.GlobalEffects()
      effects.sideEffects = SideEffects(arguments: arguments, global: global)
    }
  }

  mutating func parseGlobalSideEffectsFromSIL(to effects: inout FunctionEffects) throws {
    var globalEffects = SideEffects.GlobalEffects()
    repeat {
      if consume("read")          { globalEffects.memory.read = true }
      else if consume("write")    { globalEffects.memory.write = true }
      else if consume("copy")     { globalEffects.ownership.copy = true }
      else if consume("destroy")  { globalEffects.ownership.destroy = true }
      else if consume("allocate") { globalEffects.allocates = true }
      else if consume("deinit_barrier") { globalEffects.isDeinitBarrier = true }
      else {
        break
      }
    } while consume(",")
    let arguments = effects.sideEffects?.arguments ?? []
    effects.sideEffects = SideEffects(arguments: arguments, global: globalEffects)
    return
  }

  private mutating func parseArgumentIndexFromSIL() throws -> Int {
    if consume("%") {
      if let argIdx = consumeInt() {
        return argIdx
      }
      try throwError("expected argument index")
    }
    try throwError("expected parameter")
  }

  private mutating func parseEscapingArrow() throws -> Bool {
    if consume("=>") { return true }
    if consume("->") { return false }
    try throwError("expected '=>' or '->'")
  }
}

public extension Optional where Wrapped == SmallProjectionPath {
  mutating func merge(with path: SmallProjectionPath) {
    if let existingPath = self {
      self = existingPath.merge(with: path)
    } else {
      self = path
    }
  }
}
