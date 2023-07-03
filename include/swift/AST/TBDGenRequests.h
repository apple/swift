//===--- TBDGenRequests.h - TBDGen Requests ---------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file defines TBDGen requests.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_TBDGEN_REQUESTS_H
#define SWIFT_TBDGEN_REQUESTS_H

#include "swift/AST/ASTTypeIDs.h"
#include "swift/AST/SimpleRequest.h"
#include "swift/IRGen/Linking.h"
#include "swift/IRGen/TBDGen.h"
#include "swift/SIL/SILDeclRef.h"

namespace llvm {

class DataLayout;
class Triple;

namespace MachO {
class InterfaceFile;
} // end namespace MachO

template <class AllocatorTy>
class StringSet;
} // end namespace llvm

namespace swift {

class FileUnit;
class ModuleDecl;

namespace apigen {
class API;
} // end namespace apigen

class TBDGenDescriptor final {
  using FileOrModule = llvm::PointerUnion<FileUnit *, ModuleDecl *>;
  FileOrModule Input;
  TBDGenOptions Opts;

  TBDGenDescriptor(FileOrModule input, const TBDGenOptions &opts)
      : Input(input), Opts(opts) {
    assert(input);
  }

public:
  /// Returns the file or module we're emitting TBD for.
  FileOrModule getFileOrModule() const { return Input; }

  /// If the input is a single file, returns that file. Otherwise returns
  /// \c nullptr.
  FileUnit *getSingleFile() const;

  /// Returns the parent module for TBD emission.
  ModuleDecl *getParentModule() const;

  /// Returns the TBDGen options.
  const TBDGenOptions &getOptions() const { return Opts; }
  TBDGenOptions &getOptions() { return Opts; }

  const StringRef getDataLayoutString() const;
  const llvm::Triple &getTarget() const;

  bool operator==(const TBDGenDescriptor &other) const;
  bool operator!=(const TBDGenDescriptor &other) const {
    return !(*this == other);
  }

  static TBDGenDescriptor forFile(FileUnit *file, const TBDGenOptions &opts) {
    return TBDGenDescriptor(file, opts);
  }

  static TBDGenDescriptor forModule(ModuleDecl *M, const TBDGenOptions &opts) {
    return TBDGenDescriptor(M, opts);
  }
};

llvm::hash_code hash_value(const TBDGenDescriptor &desc);
void simple_display(llvm::raw_ostream &out, const TBDGenDescriptor &desc);
SourceLoc extractNearestSourceLoc(const TBDGenDescriptor &desc);

using TBDFile = llvm::MachO::InterfaceFile;

/// Computes the TBD file for a given Swift module or file.
class GenerateTBDRequest
    : public SimpleRequest<GenerateTBDRequest,
                           TBDFile(TBDGenDescriptor),
                           RequestFlags::Uncached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  TBDFile evaluate(Evaluator &evaluator, TBDGenDescriptor desc) const;
};

/// Retrieve the public symbols for a file or module.
class PublicSymbolsRequest
    : public SimpleRequest<PublicSymbolsRequest,
                           std::vector<std::string>(TBDGenDescriptor),
                           RequestFlags::Uncached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  std::vector<std::string>
  evaluate(Evaluator &evaluator, TBDGenDescriptor desc) const;
};

/// Retrieve API information for a file or module.
class APIGenRequest
    : public SimpleRequest<APIGenRequest,
                           apigen::API(TBDGenDescriptor),
                           RequestFlags::Uncached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  apigen::API evaluate(Evaluator &evaluator, TBDGenDescriptor desc) const;
};

/// Describes the origin of a particular symbol, including the stage of
/// compilation it is introduced, as well as information on what decl introduces
/// it.
class SymbolSource {
public:
  enum class Kind {
    /// A symbol introduced when emitting a SIL decl.
    SIL,

    /// A symbol introduced when emitting LLVM IR.
    IR,

    /// A symbol used to customize linker behavior, introduced by TBDGen.
    LinkerDirective,

    /// A symbol with an unknown origin.
    // FIXME: This should be eliminated.
    Unknown
  };
  Kind kind;

private:
  union {
    SILDeclRef silDeclRef;
    irgen::LinkEntity irEntity;
  };

  explicit SymbolSource(SILDeclRef ref) : kind(Kind::SIL) {
    silDeclRef = ref;
  }
  explicit SymbolSource(irgen::LinkEntity entity) : kind(Kind::IR) {
    irEntity = entity;
  }
  explicit SymbolSource(Kind kind) : kind(kind) {
    assert(kind == Kind::LinkerDirective || kind == Kind::Unknown);
  }

public:
  static SymbolSource forSILDeclRef(SILDeclRef ref) {
    return SymbolSource{ref};
  }
  static SymbolSource forIRLinkEntity(irgen::LinkEntity entity) {
    return SymbolSource{entity};
  }
  static SymbolSource forLinkerDirective() {
    return SymbolSource{Kind::LinkerDirective};
  }
  static SymbolSource forUnknown() {
    return SymbolSource{Kind::Unknown};
  }

  bool isLinkerDirective() const {
    return kind == Kind::LinkerDirective;
  }

  SILDeclRef getSILDeclRef() const {
    assert(kind == Kind::SIL);
    return silDeclRef;
  }
  irgen::LinkEntity getIRLinkEntity() const {
    assert(kind == Kind::IR);
    return irEntity;
  }
};

/// Maps a symbol back to its source for lazy compilation.
class SymbolSourceMap {
  friend class SymbolSourceMapRequest;

  using Storage = llvm::StringMap<SymbolSource>;
  const Storage *storage;

  explicit SymbolSourceMap(const Storage *storage) : storage(storage) {
    assert(storage);
  }

public:
  llvm::Optional<SymbolSource> find(StringRef symbol) const {
    auto result = storage->find(symbol);
    if (result == storage->end())
      return llvm::None;
    return result->second;
  }

  friend bool operator==(const SymbolSourceMap &lhs,
                         const SymbolSourceMap &rhs) {
    return lhs.storage == rhs.storage;
  }
  friend bool operator!=(const SymbolSourceMap &lhs,
                         const SymbolSourceMap &rhs) {
    return !(lhs == rhs);
  }

  friend void simple_display(llvm::raw_ostream &out, const SymbolSourceMap &) {
    out << "(symbol storage map)";
  }
};

/// Computes a map of symbols to their SymbolSource for a file or module.
class SymbolSourceMapRequest
    : public SimpleRequest<SymbolSourceMapRequest,
                           SymbolSourceMap(TBDGenDescriptor),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  SymbolSourceMap evaluate(Evaluator &evaluator, TBDGenDescriptor desc) const;

public:
  // Cached.
  bool isCached() const { return true; }
};

/// Report that a request of the given kind is being evaluated, so it
/// can be recorded by the stats reporter.
template <typename Request>
void reportEvaluatedRequest(UnifiedStatsReporter &stats,
                            const Request &request);

/// The zone number for TBDGen.
#define SWIFT_TYPEID_ZONE TBDGen
#define SWIFT_TYPEID_HEADER "swift/AST/TBDGenTypeIDZone.def"
#include "swift/Basic/DefineTypeIDZone.h"
#undef SWIFT_TYPEID_ZONE
#undef SWIFT_TYPEID_HEADER

// Set up reporting of evaluated requests.
#define SWIFT_REQUEST(Zone, RequestType, Sig, Caching, LocOptions)             \
template<>                                                                     \
inline void reportEvaluatedRequest(UnifiedStatsReporter &stats,                \
                                   const RequestType &request) {               \
  ++stats.getFrontendCounters().RequestType;                                   \
}
#include "swift/AST/TBDGenTypeIDZone.def"
#undef SWIFT_REQUEST

} // end namespace swift

#endif // SWIFT_TBDGEN_REQUESTS_H
