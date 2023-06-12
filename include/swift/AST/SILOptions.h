//===--- SILOptions.h - Swift Language SILGen and SIL options ---*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines the options which control the generation, processing,
// and optimization of SIL.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_SILOPTIONS_H
#define SWIFT_AST_SILOPTIONS_H

#include "swift/Basic/FunctionBodySkipping.h"
#include "swift/Basic/OptimizationMode.h"
#include "swift/Basic/OptionSet.h"
#include "swift/Basic/Sanitizers.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Remarks/RemarkFormat.h"
#include <climits>
#include <string>

namespace swift {

enum class LexicalLifetimesOption : uint8_t {
  // Do not insert lexical markers.
  Off = 0,

  // Insert lexical markers via lexical borrow scopes and the lexical flag on
  // alloc_stacks produced from alloc_boxes, but strip them when lowering out of
  // Raw SIL.
  DiagnosticMarkersOnly,

  // Insert lexical markers and use them to lengthen object lifetime based on
  // lexical scope.
  On,
};

enum class CopyPropagationOption : uint8_t {
  // Do not add any copy propagation passes.
  Off = 0,

  // Only add the copy propagation passes requested by other flags, currently
  // just -enable-ossa-modules.
  RequestedPassesOnly,

  // Add all relevant copy propagation passes.  If a setting, e.g.
  // -enable-ossa-modules, requests to add copy propagation to the pipeline, do
  // so.
  On
};

enum class DestroyHoistingOption : uint8_t {
  // Do not run DestroyAddrHoisting.
  Off = 0,

  // Run DestroyAddrHoisting pass after AllocBoxToStack in the function passes.
  On = 1
};

enum class CrossModuleOptimizationMode : uint8_t {
  Off = 0,
  Default = 1,
  Aggressive = 2
};

class SILModule;

class SILOptions {
public:
  /// Controls the aggressiveness of the performance inliner.
  int InlineThreshold = -1;

  /// Controls the aggressiveness of the performance inliner for Osize.
  int CallerBaseBenefitReductionFactor = 2;

  /// Controls the aggressiveness of the loop unroller.
  int UnrollThreshold = 250;

  /// Remove all runtime assertions during optimizations.
  bool RemoveRuntimeAsserts = false;

  /// Both emit lexical markers and use them to extend object lifetime to the
  /// observable end of lexical scope.
  LexicalLifetimesOption LexicalLifetimes = LexicalLifetimesOption::On;

  /// Whether to run SIL copy propagation to shorten object lifetime in whatever
  /// optimization pipeline is currently used.
  ///
  /// When this is 'On' the pipeline has default behavior.
  CopyPropagationOption CopyPropagation = CopyPropagationOption::On;

  /// Whether to run the DestroyAddrHoisting pass.
  ///
  /// When this 'On' the pipeline has the default behavior.
  DestroyHoistingOption DestroyHoisting = DestroyHoistingOption::On;

  /// Controls whether the SIL ARC optimizations are run.
  bool EnableARCOptimizations = true;

  /// Controls whether specific OSSA optimizations are run. For benchmarking
  /// purposes.
  bool EnableOSSAOptimizations = true;

  /// Controls whether to turn on speculative devirtualization.
  /// It is turned off by default.
  bool EnableSpeculativeDevirtualization = false;

  /// Controls whether to emit actor data-race checks.
  bool EnableActorDataRaceChecks = false;

  /// Should we run any SIL performance optimizations
  ///
  /// Useful when you want to enable -O LLVM opts but not -O SIL opts.
  bool DisableSILPerfOptimizations = false;

  /// Controls whether cross module optimization is enabled.
  CrossModuleOptimizationMode CMOMode = CrossModuleOptimizationMode::Off;

  /// Enables the emission of stack protectors in functions.
  bool EnableStackProtection = true;

  /// Like `EnableStackProtection` and also enables moving of values to
  /// temporaries for stack protection.
  bool EnableMoveInoutStackProtection = false;

  /// Enables codegen support for clang imported ptrauth qualified field
  /// function pointers.
  bool EnableImportPtrauthFieldFunctionPointers = false;

  /// Controls whether or not paranoid verification checks are run.
  bool VerifyAll = false;

  /// If true, no SIL verification is done at all.
  bool VerifyNone = false;

  /// Are we debugging sil serialization.
  bool DebugSerialization = false;

  /// Whether to dump verbose SIL with scope and location information.
  bool EmitVerboseSIL = false;

  /// Should we sort SIL functions, vtables, witness tables, and global
  /// variables by name when we print it out. This eases diffing of SIL files.
  bool EmitSortedSIL = false;

  /// See \ref FrontendOptions.PrintFullConvention
  bool PrintFullConvention = false;

  /// Whether to stop the optimization pipeline after serializing SIL.
  bool StopOptimizationAfterSerialization = false;

  /// Whether to stop the optimization pipeline right before we lower ownership
  /// and go from OSSA to non-ownership SIL.
  bool StopOptimizationBeforeLoweringOwnership = false;

  /// Do we always serialize SIL in OSSA form?
  ///
  /// If this is disabled we do not serialize in OSSA form when optimizing.
  bool EnableOSSAModules = false;

  /// If set to true, compile with the SIL Opaque Values enabled.
  bool EnableSILOpaqueValues = false;

  /// Require linear OSSA lifetimes after SILGen
  bool OSSACompleteLifetimes = false;

  /// Enable pack metadata stack "promotion".
  ///
  /// More accurately, enable skipping mandatory heapification of pack metadata
  /// when possible.
  bool EnablePackMetadataStackPromotion = true;

  // The kind of function bodies to skip emitting.
  FunctionBodySkipping SkipFunctionBodies = FunctionBodySkipping::None;

  /// Optimization mode being used.
  OptimizationMode OptMode = OptimizationMode::NotSet;

  enum AssertConfiguration: unsigned {
    // Used by standard library code to distinguish between a debug and release
    // build.
    Debug = 0,     // Enables all asserts.
    Release = 1,   // Disables asserts.
    Unchecked = 2, // Disables asserts, preconditions, and runtime checks.

    // Leave the assert_configuration instruction around.
    DisableReplacement = UINT_MAX
  };

  /// The assert configuration controls how assertions behave.
  unsigned AssertConfig = Debug;

  /// Should we print out instruction counts if -print-stats is passed in?
  bool PrintInstCounts = false;

  /// Instrument code to generate profiling information.
  bool GenerateProfile = false;

  /// Path to the profdata file to be used for PGO, or the empty string.
  std::string UseProfile = "";

  /// Emit a mapping of profile counters for use in coverage.
  bool EmitProfileCoverageMapping = false;

  bool emitTBD = false;

  /// Should we use a pass pipeline passed in via a json file? Null by default.
  llvm::StringRef ExternalPassPipelineFilename;

  /// Don't generate code using partial_apply in SIL generation.
  bool DisableSILPartialApply = false;

  /// Print debug information into the SIL file
  bool PrintDebugInfo = false;

  /// The name of the SIL outputfile if compiled with SIL debugging
  /// (-sil-based-debuginfo).
  std::string SILOutputFileNameForDebugging;

  /// If set to true, compile with the SIL Ownership Model enabled.
  bool VerifySILOwnership = true;

  /// Assume that code will be executed in a single-threaded environment.
  bool AssumeSingleThreaded = false;

  /// Turn @inline(__always) attributes into no-ops.
  ///
  /// For experimentation around code size reduction.
  bool IgnoreAlwaysInline = false;

  /// Indicates which sanitizer is turned on.
  OptionSet<SanitizerKind> Sanitizers;

  /// Emit compile-time diagnostics when the law of exclusivity is violated.
  bool EnforceExclusivityStatic = true;

  /// Emit checks to trap at run time when the law of exclusivity is violated.
  bool EnforceExclusivityDynamic = true;

  /// Emit extra exclusivity markers for memory access and verify coverage.
  bool VerifyExclusivity = false;

  /// When building the stdlib with opts should we lower ownership after
  /// serialization? Otherwise we do before.
  bool SerializeStdlibWithOwnershipWithOpts = true;

  /// Calls to the replaced method inside of the replacement method will call
  /// the previous implementation.
  ///
  /// @_dynamicReplacement(for: original())
  /// func replacement() {
  ///   if (...)
  ///     original() // calls original() implementation if true
  /// }
  bool EnableDynamicReplacementCanCallPreviousImplementation = true;

  /// Are we parsing the stdlib, i.e. -parse-stdlib?
  bool ParseStdlib = false;

  /// If true, check for leaking instructions when the SILModule is destructed.
  ///
  /// Warning: this is not thread safe. It can only be enabled in case there
  /// is a single SILModule in a single thread.
  bool checkSILModuleLeaks = false;

  /// The name of the file to which the backend should save optimization
  /// records.
  std::string OptRecordFile;

  /// The regex that filters the passes that should be saved to the optimization
  /// records.
  std::string OptRecordPasses;

  /// The format used for serializing remarks (default: YAML)
  llvm::remarks::Format OptRecordFormat = llvm::remarks::Format::YAML;

  SILOptions() {}

  /// Return a hash code of any components from these options that should
  /// contribute to a Swift Bridging PCH hash.
  llvm::hash_code getPCHHashComponents() const {
    return llvm::hash_value(0);
  }

  /// Return a hash code of any components from these options that should
  /// contribute to a Swift Dependency Scanning hash.
  llvm::hash_code getModuleScanningHashComponents() const {
    return llvm::hash_value(0);
  }

  bool shouldOptimize() const {
    return OptMode > OptimizationMode::NoOptimization;
  }

  /// Returns true if we support inserting lexical lifetimes given the current
  /// SIL stage.
  ///
  /// Defined in SILModule.h.
  bool supportsLexicalLifetimes(const SILModule &mod) const;
};

} // end namespace swift

#endif
