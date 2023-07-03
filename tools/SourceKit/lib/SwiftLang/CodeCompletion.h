//===--- CodeCompletion.h - -------------------------------------*- C++ -*-===//
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

#ifndef LLVM_SOURCEKIT_LIB_SWIFTLANG_CODECOMPLETION_H
#define LLVM_SOURCEKIT_LIB_SWIFTLANG_CODECOMPLETION_H

#include "SourceKit/Core/LLVM.h"
#include "swift/Basic/StringExtras.h"
#include "swift/IDE/CodeCompletionResult.h"
#include "swift/IDE/CodeCompletionResultSink.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringMap.h"

namespace SourceKit {
namespace CodeCompletion {

using swift::NullTerminatedStringRef;
using swift::ide::CodeCompletionDeclKind;
using swift::ide::CodeCompletionFlair;
using swift::ide::CodeCompletionKeywordKind;
using swift::ide::CodeCompletionLiteralKind;
using swift::ide::CodeCompletionOperatorKind;
using swift::ide::CodeCompletionResultKind;
using swift::ide::CodeCompletionResultTypeRelation;
using swift::ide::CodeCompletionString;
using swift::ide::SemanticContextKind;
using SwiftResult = swift::ide::CodeCompletionResult;
using swift::ide::CompletionKind;

struct Group;
class CodeCompletionOrganizer;
class CompletionBuilder;

/// A representation of the 'popularity' of a code completion result.
struct PopularityFactor {
  /// Raw popularity score in the range [-1, 1], where higher values are more
  /// popular and 0.0 indicates an unknown popularity.
  double rawValue = 0.0;
  bool isPopular() const { return rawValue > 0.0; }
  bool isUnpopular() const { return rawValue < 0.0; }
  PopularityFactor() = default;
  explicit PopularityFactor(double value) : rawValue(value) {}
};

struct NameStyle {
  enum WordDelimiter : uint8_t {
    Unknown,
    Lowercase,                // lowercase
    Uppercase,                // UPPERCASE
    UpperCamelCase,           // UpperCamelCase
    LowerCamelCase,           // lowerCamelCase
    LowercaseWithUnderscores, // lowercase_with_underscores
    UppercaseWithUnderscores, // UPPERCASE_WITH_UNDERSCORES
  };

  WordDelimiter wordDelimiter = Unknown;
  uint8_t leadingUnderscores : 2;
  uint8_t trailingUnderscores : 2;

  explicit NameStyle(StringRef name);

  bool possiblyLowerCamelCase() const {
    return wordDelimiter == Lowercase || wordDelimiter == LowerCamelCase;
  }
  bool possiblyUpperCamelCase() const {
    return wordDelimiter == Uppercase || wordDelimiter == UpperCamelCase;
  }
};

/// Code completion result type for SourceKit::SwiftLangSupport.
///
/// Extends a \c swift::ide::CodeCompletionResult with extra fields that are
/// filled in by SourceKit. Generally stored in an \c CompletionSink.
class Completion {
  const SwiftResult &base;
  void *opaqueCustomKind = nullptr;
  llvm::Optional<uint8_t> moduleImportDepth;
  PopularityFactor popularityFactor;
  StringRef name;
  StringRef description;
  friend class CompletionBuilder;

public:
  static constexpr unsigned numSemanticContexts = 8;
  static constexpr unsigned maxModuleImportDepth = 10;

  /// Wraps \p base with an \c Completion.  The \p name and \p description
  /// should outlive the result, generally by being stored in the same
  /// \c CompletionSink or in a sink that was adopted by the sink that this
  /// \c Compleiton is being stored in.
  Completion(const SwiftResult &base, StringRef description)
      : base(base), description(description) {}

  const SwiftResult &getSwiftResult() const { return base; }

  bool hasCustomKind() const { return opaqueCustomKind; }
  void *getCustomKind() const { return opaqueCustomKind; }
  StringRef getDescription() const { return description; }
  llvm::Optional<uint8_t> getModuleImportDepth() const {
    return moduleImportDepth;
  }

  /// A popularity factory in the range [-1, 1]. The higher the value, the more
  /// 'popular' this result is.  0 indicates unknown.
  PopularityFactor getPopularityFactor() const { return popularityFactor; }

  // MARK: Methods that forward to the SwiftResult

  CodeCompletionResultKind getKind() const {
    return getSwiftResult().getKind();
  }

  CodeCompletionDeclKind getAssociatedDeclKind() const {
    return getSwiftResult().getAssociatedDeclKind();
  }

  CodeCompletionLiteralKind getLiteralKind() const {
    return getSwiftResult().getLiteralKind();
  }

  CodeCompletionKeywordKind getKeywordKind() const {
    return getSwiftResult().getKeywordKind();
  }

  bool isOperator() const { return getSwiftResult().isOperator(); }

  CodeCompletionOperatorKind getOperatorKind() const {
    return getSwiftResult().getOperatorKind();
  }

  bool isSystem() const { return getSwiftResult().isSystem(); }

  CodeCompletionResultTypeRelation getExpectedTypeRelation() const {
    return getSwiftResult().getExpectedTypeRelation();
  }

  SemanticContextKind getSemanticContext() const {
    return getSwiftResult().getSemanticContext();
  }

  CodeCompletionFlair getFlair() const { return getSwiftResult().getFlair(); }

  bool isNotRecommended() const { return getSwiftResult().isNotRecommended(); }

  unsigned getNumBytesToErase() const {
    return getSwiftResult().getNumBytesToErase();
  }

  CodeCompletionString *getCompletionString() const {
    return getSwiftResult().getCompletionString();
  }

  StringRef getModuleName() const { return getSwiftResult().getModuleName(); }

  StringRef getBriefDocComment() const {
    return getSwiftResult().getBriefDocComment();
  }

  ArrayRef<NullTerminatedStringRef> getAssociatedUSRs() const {
    return getSwiftResult().getAssociatedUSRs();
  }

  StringRef getFilterName() const {
    return getSwiftResult().getFilterName();
  }

  /// Allow "upcasting" the completion result to a SwiftResult.
  operator const SwiftResult &() const { return getSwiftResult(); }
};

/// Storage sink for \c Completion objects.
///
/// In addition to allocating the results themselves, uses \c swiftSink to keep
/// the storage for the underlying swift results alive.
struct CompletionSink {
  swift::ide::CodeCompletionResultSink swiftSink;
  llvm::BumpPtrAllocator allocator;

  /// Adds references to a swift sink's allocators to keep its storage alive.
  void adoptSwiftSink(swift::ide::CodeCompletionResultSink &sink) {
    swiftSink.ForeignAllocators.insert(swiftSink.ForeignAllocators.end(),
                                       sink.ForeignAllocators.begin(),
                                       sink.ForeignAllocators.end());
    swiftSink.ForeignAllocators.push_back(sink.Allocator);
  }
};

class CompletionBuilder {
  CompletionSink &sink;
  const SwiftResult &base;
  bool modified = false;
  SemanticContextKind semanticContext;
  CodeCompletionFlair flair;
  CodeCompletionString *completionString;
  void *customKind = nullptr;
  llvm::Optional<uint8_t> moduleImportDepth;
  PopularityFactor popularityFactor;

public:
  CompletionBuilder(CompletionSink &sink, const SwiftResult &base);

  void setCustomKind(void *opaqueCustomKind) { customKind = opaqueCustomKind; }

  void setModuleImportDepth(llvm::Optional<uint8_t> value) {
    assert(!value || *value <= Completion::maxModuleImportDepth);
    moduleImportDepth = value;
  }

  void setSemanticContext(SemanticContextKind kind) {
    modified = true;
    semanticContext = kind;
  }
  void setFlair(CodeCompletionFlair value) {
    modified = true;
    flair = value;
  }

  void setPopularityFactor(PopularityFactor val) { popularityFactor = val; }

  void setPrefix(CodeCompletionString *prefix);

  StringRef getOriginalName() const {
    return base.getFilterName();
  }

  Completion *finish();
};


/// Immutable view of code completion results.
///
/// Provides a possibly filtered view of code completion results
/// (\c Completion) organized into groups.  Clients walk the tree using
/// CodeCompletionView::Walker.  The \c Completion objects are not owned
/// by the view and must outlive it.
class CodeCompletionView {
  const Group *rootGroup = nullptr; ///< Owned by the view.

  friend class CodeCompletionOrganizer;
  friend class LimitedResultView;
  CodeCompletionView(const CodeCompletionView &) = delete;
  void operator=(const CodeCompletionView &) = delete;
public:

  CodeCompletionView() = default;
  CodeCompletionView(CodeCompletionView &&) = default;
  virtual ~CodeCompletionView();

  struct Walker;
  virtual bool walk(Walker &walker) const;
};

/// Interface implemented by clients of \c CodeCompletionView.
struct CodeCompletionView::Walker {
  virtual ~Walker() {}
  virtual bool handleResult(Completion *result) = 0;
  virtual void startGroup(StringRef name) = 0;
  virtual void endGroup() = 0;
};

using CodeCompletionViewRef = std::shared_ptr<const CodeCompletionView>;

class LimitedResultView : public CodeCompletionView {
  const CodeCompletionView &baseView;
  mutable unsigned start;
  unsigned maxResults;

public:
  LimitedResultView(const CodeCompletionView &baseView, unsigned start,
                    unsigned maxResults)
      : baseView(baseView), start(start), maxResults(maxResults) {}

  unsigned getNextOffset() const;
  bool walk(Walker &walker) const override;
};

struct FilterRules {
  bool hideAll = false;

  bool hideAllValueLiterals = false;
  llvm::SmallDenseMap<CodeCompletionLiteralKind, bool, 8> hideValueLiteral;

  bool hideAllKeywords = false;
  llvm::DenseMap<CodeCompletionKeywordKind, bool> hideKeyword;

  bool hideCustomCompletions = false;
  // FIXME: hide individual custom completions

  llvm::StringMap<bool> hideModule;
  llvm::StringMap<bool> hideByFilterName;
  llvm::StringMap<bool> hideByDescription;

  bool hideCompletion(const Completion &completion) const;
  bool hideCompletion(const SwiftResult &completion, StringRef name,
                      StringRef description, void *customKind = nullptr) const;
  bool hideFilterName(StringRef name) const;
};

} // end namespace CodeCompletion
} // end namespace SourceKit

#endif // LLVM_SOURCEKIT_LIB_SWIFTLANG_CODECOMPLETION_H
