//===--- ParseRequests.h - Parsing Requests ---------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file defines parsing requests.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_PARSE_REQUESTS_H
#define SWIFT_PARSE_REQUESTS_H

#include "swift/AST/ASTTypeIDs.h"
#include "swift/AST/EvaluatorDependencies.h"
#include "swift/AST/SimpleRequest.h"
#include "swift/Basic/Fingerprint.h"
#include "swift/Parse/Token.h"

namespace swift {

struct ASTNode;

/// Report that a request of the given kind is being evaluated, so it
/// can be recorded by the stats reporter.
template<typename Request>
void reportEvaluatedRequest(UnifiedStatsReporter &stats,
                            const Request &request);

struct FingerprintAndMembers {
  llvm::Optional<Fingerprint> fingerprint = llvm::None;
  ArrayRef<Decl *> members = {};
  bool operator==(const FingerprintAndMembers &x) const {
    return fingerprint == x.fingerprint && members == x.members;
  }
};

void simple_display(llvm::raw_ostream &out, const FingerprintAndMembers &value);

/// Parse the members of a nominal type declaration or extension.
/// Return a fingerprint and the members.
class ParseMembersRequest
    : public SimpleRequest<ParseMembersRequest,
                           FingerprintAndMembers(IterableDeclContext *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  FingerprintAndMembers evaluate(Evaluator &evaluator,
                                 IterableDeclContext *idc) const;

public:
  // Caching
  bool isCached() const { return true; }
};

/// Parse the body of a function, initializer, or deinitializer.
class ParseAbstractFunctionBodyRequest
    : public SimpleRequest<ParseAbstractFunctionBodyRequest,
                           BodyAndFingerprint(AbstractFunctionDecl *),
                           RequestFlags::SeparatelyCached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  BodyAndFingerprint evaluate(Evaluator &evaluator,
                              AbstractFunctionDecl *afd) const;

public:
  // Caching
  bool isCached() const { return true; }
  llvm::Optional<BodyAndFingerprint> getCachedResult() const;
  void cacheResult(BodyAndFingerprint value) const;
};

struct SourceFileParsingResult {
  ArrayRef<ASTNode> TopLevelItems;
  llvm::Optional<ArrayRef<Token>> CollectedTokens;
  llvm::Optional<StableHasher> InterfaceHasher;
};

/// Parse the top-level items of a SourceFile.
class ParseSourceFileRequest
    : public SimpleRequest<
          ParseSourceFileRequest, SourceFileParsingResult(SourceFile *),
          RequestFlags::SeparatelyCached | RequestFlags::DependencySource> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  SourceFileParsingResult evaluate(Evaluator &evaluator, SourceFile *SF) const;

public:
  // Caching.
  bool isCached() const { return true; }
  llvm::Optional<SourceFileParsingResult> getCachedResult() const;
  void cacheResult(SourceFileParsingResult result) const;

public:
  evaluator::DependencySource
  readDependencySource(const evaluator::DependencyRecorder &) const;
};

/// Parse the ExportedSourceFile for a given SourceFile.
class ExportedSourceFileRequest
    : public SimpleRequest<ExportedSourceFileRequest,
                           void *(const SourceFile *),
                           RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  void *evaluate(Evaluator &evaluator, const SourceFile *SF) const;

public:
  // Cached.
  bool isCached() const { return true; }
};

/// Parse the top-level items of a SourceFile.
class ParseTopLevelDeclsRequest
    : public SimpleRequest<
          ParseTopLevelDeclsRequest, ArrayRef<Decl *>(SourceFile *),
          RequestFlags::Cached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  ArrayRef<Decl *> evaluate(Evaluator &evaluator, SourceFile *SF) const;

public:
  // Caching.
  bool isCached() const { return true; }
};

void simple_display(llvm::raw_ostream &out,
                    const IDEInspectionCallbacksFactory *factory);

class IDEInspectionSecondPassRequest
    : public SimpleRequest<IDEInspectionSecondPassRequest,
                           bool(SourceFile *, IDEInspectionCallbacksFactory *),
                           RequestFlags::Uncached|RequestFlags::DependencySource> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  bool evaluate(Evaluator &evaluator, SourceFile *SF,
                IDEInspectionCallbacksFactory *Factory) const;

public:
  evaluator::DependencySource
  readDependencySource(const evaluator::DependencyRecorder &) const;
};

/// The zone number for the parser.
#define SWIFT_TYPEID_ZONE Parse
#define SWIFT_TYPEID_HEADER "swift/AST/ParseTypeIDZone.def"
#include "swift/Basic/DefineTypeIDZone.h"
#undef SWIFT_TYPEID_ZONE
#undef SWIFT_TYPEID_HEADER

// Set up reporting of evaluated requests.
#define SWIFT_REQUEST(Zone, RequestType, Sig, Caching, LocOptions)             \
  template <>                                                                  \
  inline void reportEvaluatedRequest(UnifiedStatsReporter &stats,              \
                                     const RequestType &request) {             \
    ++stats.getFrontendCounters().RequestType;                                 \
  }
#include "swift/AST/ParseTypeIDZone.def"
#undef SWIFT_REQUEST

} // end namespace swift

#endif // SWIFT_PARSE_REQUESTS_H
