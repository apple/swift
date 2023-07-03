//===--- SILGenRequests.h - SILGen Requests ---------------------*- C++ -*-===//
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
//  This file defines SILGen requests.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILGEN_REQUESTS_H
#define SWIFT_SILGEN_REQUESTS_H

#include "swift/AST/ASTTypeIDs.h"
#include "swift/AST/EvaluatorDependencies.h"
#include "swift/AST/SimpleRequest.h"
#include "swift/AST/SourceFile.h"
#include "swift/SIL/SILDeclRef.h"

namespace swift {

class LangOptions;
class ModuleDecl;
class SILModule;
class SILOptions;
class IRGenOptions;

namespace Lowering {
  class TypeConverter;
}

/// Report that a request of the given kind is being evaluated, so it
/// can be recorded by the stats reporter.
template<typename Request>
void reportEvaluatedRequest(UnifiedStatsReporter &stats,
                            const Request &request);

using SILRefsToEmit = llvm::SmallVector<SILDeclRef, 1>;

/// Describes a file or module to be lowered to SIL.
struct ASTLoweringDescriptor {
  llvm::PointerUnion<FileUnit *, ModuleDecl *> context;
  Lowering::TypeConverter &conv;
  const SILOptions &opts;
  const IRGenOptions *irgenOptions;

  /// A specific set of SILDeclRefs to emit. If set, only these refs will be
  /// emitted. Otherwise the entire \c context will be emitted.
  llvm::Optional<SILRefsToEmit> refsToEmit;

  friend llvm::hash_code hash_value(const ASTLoweringDescriptor &owner) {
    return llvm::hash_combine(owner.context, (void *)&owner.conv,
                              (void *)&owner.opts,
                              owner.refsToEmit);
  }

  friend bool operator==(const ASTLoweringDescriptor &lhs,
                         const ASTLoweringDescriptor &rhs) {
    return lhs.context == rhs.context &&
           &lhs.conv == &rhs.conv &&
           &lhs.opts == &rhs.opts &&
           lhs.refsToEmit == rhs.refsToEmit;
  }

  friend bool operator!=(const ASTLoweringDescriptor &lhs,
                         const ASTLoweringDescriptor &rhs) {
    return !(lhs == rhs);
  }

public:
  static ASTLoweringDescriptor
  forFile(FileUnit &sf, Lowering::TypeConverter &conv, const SILOptions &opts,
          llvm::Optional<SILRefsToEmit> refsToEmit = llvm::None,
          const IRGenOptions *irgenOptions = nullptr) {
    return ASTLoweringDescriptor{&sf, conv, opts, irgenOptions, refsToEmit};
  }

  static ASTLoweringDescriptor
  forWholeModule(ModuleDecl *mod, Lowering::TypeConverter &conv,
                 const SILOptions &opts,
                 llvm::Optional<SILRefsToEmit> refsToEmit = llvm::None,
                 const IRGenOptions *irgenOptions = nullptr) {
    return ASTLoweringDescriptor{mod, conv, opts, irgenOptions, refsToEmit};
  }

  /// Retrieves the files to generate SIL for. If the descriptor is configured
  /// only to emit a specific set of SILDeclRefs, this will be empty.
  ArrayRef<FileUnit *> getFilesToEmit() const;

  /// If the module or file contains SIL that needs parsing, returns the file
  /// to be parsed, or \c nullptr if parsing isn't required.
  SourceFile *getSourceFileToParse() const;
};

void simple_display(llvm::raw_ostream &out, const ASTLoweringDescriptor &d);

SourceLoc extractNearestSourceLoc(const ASTLoweringDescriptor &desc);

/// Lowers a file or module to SIL. In most cases this involves transforming
/// a file's AST into SIL, through SILGen. However it can also handle files
/// containing SIL in textual or binary form, which will be parsed or
/// deserialized as needed.
class ASTLoweringRequest
    : public SimpleRequest<
          ASTLoweringRequest, std::unique_ptr<SILModule>(ASTLoweringDescriptor),
          RequestFlags::Uncached | RequestFlags::DependencySource> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  std::unique_ptr<SILModule> evaluate(Evaluator &evaluator,
                                      ASTLoweringDescriptor desc) const;

public:
  // Incremental dependencies.
  evaluator::DependencySource
  readDependencySource(const evaluator::DependencyRecorder &) const;
};

/// Parses a .sil file into a SILModule.
class ParseSILModuleRequest
    : public SimpleRequest<ParseSILModuleRequest,
                           std::unique_ptr<SILModule>(ASTLoweringDescriptor),
                           RequestFlags::Uncached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  std::unique_ptr<SILModule> evaluate(Evaluator &evaluator,
                                      ASTLoweringDescriptor desc) const;
};

/// The zone number for SILGen.
#define SWIFT_TYPEID_ZONE SILGen
#define SWIFT_TYPEID_HEADER "swift/AST/SILGenTypeIDZone.def"
#include "swift/Basic/DefineTypeIDZone.h"
#undef SWIFT_TYPEID_ZONE
#undef SWIFT_TYPEID_HEADER

 // Set up reporting of evaluated requests.
#define SWIFT_REQUEST(Zone, RequestType, Sig, Caching, LocOptions)             \
template<>                                                                     \
inline void reportEvaluatedRequest(UnifiedStatsReporter &stats,                \
                            const RequestType &request) {                      \
  ++stats.getFrontendCounters().RequestType;                                   \
}
#include "swift/AST/SILGenTypeIDZone.def"
#undef SWIFT_REQUEST

} // end namespace swift

#endif // SWIFT_SILGEN_REQUESTS_H
