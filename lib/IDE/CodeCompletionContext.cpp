//===--- CodeCompletionContext.cpp ----------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/Basic/Assertions.h"
#include "swift/IDE/CodeCompletionContext.h"
#include "swift/IDE/CodeCompletionCache.h"

using namespace swift;
using namespace swift::ide;

std::vector<CodeCompletionResult *>
CodeCompletionContext::sortCompletionResults(
    ArrayRef<CodeCompletionResult *> Results) {
  std::vector<CodeCompletionResult *> SortedResults(Results.begin(),
                                                    Results.end());

  std::sort(SortedResults.begin(), SortedResults.end(),
            [](const auto &LHS, const auto &RHS) {
              int Result = StringRef(LHS->getFilterName())
                               .compare_insensitive(RHS->getFilterName());
              // If the case insensitive comparison is equal, then secondary
              // sort order should be case sensitive.
              if (Result == 0)
                Result = LHS->getFilterName().compare(RHS->getFilterName());
              return Result < 0;
            });

  return SortedResults;
}

static MutableArrayRef<CodeCompletionResult *> copyCodeCompletionResults(
    CodeCompletionResultSink &targetSink, CodeCompletionCache::Value &source,
    CodeCompletionFilter filter, const ExpectedTypeContext *TypeContext,
    const DeclContext *DC, bool CanCurrDeclContextHandleAsync) {
  assert(filter && "Should never have an empty filter");

  // We will be adding foreign results (from another sink) into TargetSink.
  // TargetSink should have an owning pointer to the allocator that keeps the
  // results alive.
  targetSink.ForeignAllocators.push_back(source.Allocator);
  auto startSize = targetSink.Results.size();

  CodeCompletionMacroRoles expectedMacroRoles = getCompletionMacroRoles(filter);
  std::function<bool(const ContextFreeCodeCompletionResult *)>
      shouldIncludeResult =
          [filter, expectedMacroRoles](
              const ContextFreeCodeCompletionResult *R) -> bool {
    if (R->getKind() != CodeCompletionResultKind::Declaration)
      return false;

    switch (R->getAssociatedDeclKind()) {
    case CodeCompletionDeclKind::EnumElement:
    case CodeCompletionDeclKind::Constructor:
    case CodeCompletionDeclKind::Destructor:
    case CodeCompletionDeclKind::Subscript:
    case CodeCompletionDeclKind::StaticMethod:
    case CodeCompletionDeclKind::InstanceMethod:
    case CodeCompletionDeclKind::PrefixOperatorFunction:
    case CodeCompletionDeclKind::PostfixOperatorFunction:
    case CodeCompletionDeclKind::InfixOperatorFunction:
    case CodeCompletionDeclKind::FreeFunction:
    case CodeCompletionDeclKind::StaticVar:
    case CodeCompletionDeclKind::InstanceVar:
    case CodeCompletionDeclKind::LocalVar:
    case CodeCompletionDeclKind::GlobalVar:
      return filter.contains(CodeCompletionFilterFlag::Expr);

    case CodeCompletionDeclKind::Module:
    case CodeCompletionDeclKind::Class:
    case CodeCompletionDeclKind::Actor:
    case CodeCompletionDeclKind::Struct:
    case CodeCompletionDeclKind::Enum:
    case CodeCompletionDeclKind::Protocol:
    case CodeCompletionDeclKind::TypeAlias:
    case CodeCompletionDeclKind::AssociatedType:
    case CodeCompletionDeclKind::GenericTypeParam:
      return filter.contains(CodeCompletionFilterFlag::Type);

    case CodeCompletionDeclKind::PrecedenceGroup:
      return filter.contains(CodeCompletionFilterFlag::PrecedenceGroup);

    case CodeCompletionDeclKind::Macro:
      return (bool)(R->getMacroRoles() & expectedMacroRoles);
    }

    llvm_unreachable("Unhandled associated decl kind");
  };

  USRBasedTypeContext USRTypeContext(TypeContext, source.USRTypeArena);

  for (auto contextFreeResult : source.Results) {
    if (!shouldIncludeResult(contextFreeResult)) {
      continue;
    }

    CodeCompletionResultTypeRelation typeRelation =
        contextFreeResult->calculateContextualTypeRelation(DC, TypeContext,
                                                           &USRTypeContext);
    ContextualNotRecommendedReason notRecommendedReason =
        contextFreeResult->calculateContextualNotRecommendedReason(
            ContextualNotRecommendedReason::None,
            CanCurrDeclContextHandleAsync);

    auto contextualResult = new (*targetSink.Allocator) CodeCompletionResult(
        *contextFreeResult, SemanticContextKind::OtherModule,
        CodeCompletionFlair(),
        /*numBytesToErase=*/0, typeRelation, notRecommendedReason);
    targetSink.Results.push_back(contextualResult);
  }

  return llvm::MutableArrayRef(targetSink.Results.data() + startSize,
                               targetSink.Results.size() - startSize);
}

void CodeCompletionContext::addResultsFromModules(
    ArrayRef<RequestedCachedModule> RequestedModules,
    const ExpectedTypeContext &TypeContext, const DeclContext *DC,
    bool CanCurrDeclContextHandleAsync) {
  // Use the current SourceFile as the DeclContext so that we can use it to
  // perform qualified lookup, and to get the correct visibility for
  // @testable imports. Also it cannot use 'DC' since it would apply decl
  // context changes to cached results.
  const SourceFile *SF = DC->getParentSourceFile();

  for (auto &R : RequestedModules) {
    // FIXME(thread-safety): lock the whole AST context.  We might load a
    // module.
    std::optional<CodeCompletionCache::ValueRefCntPtr> V = Cache.get(R.Key);
    if (!V.has_value()) {
      // No cached results found. Fill the cache.
      V = Cache.createValue();
      // Temporary sink in which we gather the result. The cache value retains
      // the sink's allocator.
      CodeCompletionResultSink Sink;
      Sink.annotateResult = getAnnotateResult();
      Sink.addInitsToTopLevel = getAddInitsToTopLevel();
      Sink.includeObjectLiterals = includeObjectLiterals();
      Sink.addCallWithNoDefaultArgs = addCallWithNoDefaultArgs();
      Sink.setProduceContextFreeResults((*V)->USRTypeArena);
      lookupCodeCompletionResultsFromModule(Sink, R.TheModule, R.Key.AccessPath,
                                            R.Key.ResultsHaveLeadingDot, SF);
      (*V)->Allocator = Sink.Allocator;
      auto &CachedResults = (*V)->Results;
      CachedResults.reserve(Sink.Results.size());
      // Instead of copying the context free results out of the sink's allocator
      // retain the sink's entire allocator (which also includes the contextual
      // properities) and simply store pointers to the context free results that
      // back the contextual results.
      for (auto Result : Sink.Results) {
        assert(
            Result->getContextFreeResult().getResultType().isBackedByUSRs() &&
            "Results stored in the cache should have their result types backed "
            "by a USR because the cache might outlive the ASTContext the "
            "results were created from.");
        CachedResults.push_back(&Result->getContextFreeResult());
      }
      Cache.set(R.Key, *V);
    }
    assert(V.has_value());
    auto newItems =
        copyCodeCompletionResults(getResultSink(), **V, R.Filter, &TypeContext,
                                  DC, CanCurrDeclContextHandleAsync);
    postProcessCompletionResults(newItems, CodeCompletionKind, DC,
                                 &getResultSink());
  }
}
