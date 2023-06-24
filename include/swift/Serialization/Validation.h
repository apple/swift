//===--- Validation.h - Validation / errors for serialization ---*- C++ -*-===//
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

#ifndef SWIFT_SERIALIZATION_VALIDATION_H
#define SWIFT_SERIALIZATION_VALIDATION_H

#include "swift/Basic/LLVM.h"
#include "swift/Serialization/SerializationOptions.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace llvm {
class Triple;
}

namespace swift {

class ModuleFile;
enum class ResilienceStrategy : unsigned;

namespace serialization {

/// Describes whether a serialized module can be used by this compiler.
enum class Status {
  /// The module is valid.
  Valid,

  /// The module file format is too old to be used by this version of the
  /// compiler.
  FormatTooOld,

  /// The module file format is too new to be used by this version of the
  /// compiler.
  FormatTooNew,

  /// The precise revision version doesn't match.
  RevisionIncompatible,

  /// The module is required to be in OSSA, but is not.
  NotInOSSA,

  /// The module file depends on another module that can't be loaded.
  MissingDependency,

  /// The module file is an overlay for a Clang module, which can't be found.
  MissingUnderlyingModule,

  /// The module file depends on a module that is still being loaded, i.e.
  /// there is a circular dependency.
  CircularDependency,

  /// The module file depends on a bridging header that can't be loaded.
  FailedToLoadBridgingHeader,

  /// The module file is malformed in some way.
  Malformed,

  /// The module documentation file is malformed in some way.
  MalformedDocumentation,

  /// The module file's name does not match the module it is being loaded
  /// into.
  NameMismatch,

  /// The module file was built for a different target platform.
  TargetIncompatible,

  /// The module file was built for a target newer than the current target.
  TargetTooNew,

  /// The module file was built with a different SDK than the one in use
  /// to build the client.
  SDKMismatch
};

/// Returns true if the data looks like it contains a serialized AST.
bool isSerializedAST(StringRef data);

/// \see validateSerializedAST()
struct ValidationInfo {
  StringRef name = {};
  StringRef targetTriple = {};
  StringRef shortVersion = {};
  StringRef miscVersion = {};
  version::Version compatibilityVersion = {};
  llvm::VersionTuple userModuleVersion;
  StringRef sdkName = {};
  StringRef problematicRevision = {};
  size_t bytes = 0;
  Status status = Status::Malformed;
  std::vector<StringRef> allowableClients;
};

/// A collection of options that can be used to set up a new AST context
/// before it has been created.
///
/// Note that this is intended to be a transient data structure; as such,
/// <strong>none of the string values added to it are copied</strong>.
///
/// \sa validateSerializedAST()
class ExtendedValidationInfo {
  SmallVector<StringRef, 4> ExtraClangImporterOpts;

  SmallVector<std::pair<PluginSearchOption::Kind, StringRef>, 2>
      PluginSearchOptions;

  std::string SDKPath;
  StringRef ModuleABIName;
  StringRef ModulePackageName;
  StringRef ExportAsName;
  struct {
    unsigned ArePrivateImportsEnabled : 1;
    unsigned IsSIB : 1;
    unsigned IsStaticLibrary : 1;
    unsigned HasHermeticSealAtLink : 1;
    unsigned IsTestable : 1;
    unsigned ResilienceStrategy : 2;
    unsigned IsImplicitDynamicEnabled : 1;
    unsigned IsBuiltFromInterface : 1;
    unsigned IsAllowModuleWithCompilerErrorsEnabled : 1;
    unsigned IsConcurrencyChecked : 1;
    unsigned HasCxxInteroperability : 1;
  } Bits;
public:
  ExtendedValidationInfo() : Bits() {}

  StringRef getSDKPath() const { return SDKPath; }
  void setSDKPath(std::string path) {
    assert(SDKPath.empty());
    SDKPath = path;
  }

  ArrayRef<StringRef> getExtraClangImporterOptions() const {
    return ExtraClangImporterOpts;
  }
  void addExtraClangImporterOption(StringRef option) {
    ExtraClangImporterOpts.push_back(option);
  }

  ArrayRef<std::pair<PluginSearchOption::Kind, StringRef>>
  getPluginSearchOptions() const {
    return PluginSearchOptions;
  }
  void addPluginSearchOption(
      const std::pair<PluginSearchOption::Kind, StringRef> &opt) {
    PluginSearchOptions.push_back(opt);
  }

  bool isSIB() const { return Bits.IsSIB; }
  void setIsSIB(bool val) {
      Bits.IsSIB = val;
  }
  bool arePrivateImportsEnabled() { return Bits.ArePrivateImportsEnabled; }
  void setPrivateImportsEnabled(bool enabled) {
    Bits.ArePrivateImportsEnabled = enabled;
  }
  bool isImplicitDynamicEnabled() { return Bits.IsImplicitDynamicEnabled; }
  void setImplicitDynamicEnabled(bool val) {
    Bits.IsImplicitDynamicEnabled = val;
  }
  bool isStaticLibrary() const { return Bits.IsStaticLibrary; }
  void setIsStaticLibrary(bool val) {
    Bits.IsStaticLibrary = val;
  }
  bool hasHermeticSealAtLink() const { return Bits.HasHermeticSealAtLink; }
  void setHasHermeticSealAtLink(bool val) {
    Bits.HasHermeticSealAtLink = val;
  }
  bool isTestable() const { return Bits.IsTestable; }
  void setIsTestable(bool val) {
    Bits.IsTestable = val;
  }
  ResilienceStrategy getResilienceStrategy() const {
    return ResilienceStrategy(Bits.ResilienceStrategy);
  }
  void setResilienceStrategy(ResilienceStrategy resilience) {
    Bits.ResilienceStrategy = unsigned(resilience);
  }
  bool isBuiltFromInterface() const { return Bits.IsBuiltFromInterface; }
  void setIsBuiltFromInterface(bool val) {
    Bits.IsBuiltFromInterface = val;
  }
  bool isAllowModuleWithCompilerErrorsEnabled() {
    return Bits.IsAllowModuleWithCompilerErrorsEnabled;
  }
  void setAllowModuleWithCompilerErrorsEnabled(bool val) {
    Bits.IsAllowModuleWithCompilerErrorsEnabled = val;
  }

  StringRef getModuleABIName() const { return ModuleABIName; }
  void setModuleABIName(StringRef name) { ModuleABIName = name; }

  StringRef getModulePackageName() const { return ModulePackageName; }
  void setModulePackageName(StringRef name) { ModulePackageName = name; }

  StringRef getExportAsName() const { return ExportAsName; }
  void setExportAsName(StringRef name) { ExportAsName = name; }

  bool isConcurrencyChecked() const {
    return Bits.IsConcurrencyChecked;
  }
  void setIsConcurrencyChecked(bool val = true) {
    Bits.IsConcurrencyChecked = val;
  }
  bool hasCxxInteroperability() const { return Bits.HasCxxInteroperability; }
  void setHasCxxInteroperability(bool val) {
    Bits.HasCxxInteroperability = val;
  }
};

struct SearchPath {
  std::string Path;
  bool IsFramework;
  bool IsSystem;
};

/// Returns info about the serialized AST in the given data.
///
/// If the returned status is anything but Status::Valid, the serialized data
/// cannot be loaded by this version of the compiler. If the returned size is
/// non-zero, it's possible to skip over this module's data, in case there is
/// more data in the buffer. The returned name, which may be empty, directly
/// points into the given data buffer.
///
/// Note that this does not actually try to load the module or validate any
/// of its dependencies; it only checks that it /can/ be loaded.
///
/// \param data A buffer containing the serialized AST. Result information
/// refers directly into this buffer.
/// \param requiresOSSAModules If true, necessitates the module to be
/// compiled with -enable-ossa-modules.
/// \param requiredSDK If not empty, only accept modules built with
/// a compatible SDK. The StringRef represents the canonical SDK name.
/// \param requiresRevisionMatch if true, expects the swift tag to match in
/// addition to the module format version number.
/// \param[out] extendedInfo If present, will be populated with additional
/// compilation options serialized into the AST at build time that may be
/// necessary to load it properly.
/// \param[out] dependencies If present, will be populated with list of
/// input files the module depends on, if present in INPUT_BLOCK.
ValidationInfo validateSerializedAST(
    StringRef data, bool requiresOSSAModules, StringRef requiredSDK,
    bool requiresRevisionMatch = true,
    ExtendedValidationInfo *extendedInfo = nullptr,
    SmallVectorImpl<SerializationOptions::FileDependency> *dependencies =
        nullptr,
    SmallVectorImpl<SearchPath> *searchPaths = nullptr);

/// Emit diagnostics explaining a failure to load a serialized AST.
///
/// - \p Ctx is an AST context through which any diagnostics are surfaced.
/// - \p diagLoc is the (possibly invalid) location used in the diagnostics.
/// - \p loadInfo and \p extendedInfo describe the attempt to load an AST
///   (\ref validateSerializedAST). Note that loadInfo.Status must not be
///   Status::Valid.
/// - \p moduleBufferID and \p moduleDocBufferID are the buffer identifiers
///   of the module input and doc input buffers respectively (\ref 
///   SerializedModuleLoaderBase::loadAST, \ref ModuleFile::load).
/// - \p loadedModuleFile is an invalid loaded module.
/// - \p ModuleName is the name used to refer to the module in diagnostics.
void diagnoseSerializedASTLoadFailure(
    ASTContext &Ctx, SourceLoc diagLoc, const ValidationInfo &loadInfo,
    StringRef moduleBufferID, StringRef moduleDocBufferID,
    ModuleFile *loadedModuleFile, Identifier ModuleName);

/// Emit diagnostics explaining a failure to load a serialized AST,
/// this version only supports diagnostics relevant to transitive dependencies:
/// missing dependency, missing underlying module, or circular dependency.
///
/// \see diagnoseSerializedASTLoadFailure that supports all diagnostics.
///
/// - \p Ctx is an AST context through which any diagnostics are surfaced.
/// - \p diagLoc is the (possibly invalid) location used in the diagnostics.
/// - \p status describes the issue with loading the AST. It must not be
///   Status::Valid.
/// - \p loadedModuleFile is an invalid loaded module.
/// - \p ModuleName is the name used to refer to the module in diagnostics.
/// - \p forTestable indicates if we loaded the AST for a @testable import.
void diagnoseSerializedASTLoadFailureTransitive(
    ASTContext &Ctx, SourceLoc diagLoc, const serialization::Status status,
    ModuleFile *loadedModuleFile, Identifier ModuleName, bool forTestable);

/// Determine whether two triples are considered to be compatible for module
/// serialization purposes.
bool areCompatible(const llvm::Triple &moduleTarget,
                   const llvm::Triple &ctxTarget);

} // end namespace serialization
} // end namespace swift

#endif
