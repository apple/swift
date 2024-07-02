//===--- CodeCompletionOrganizer.cpp --------------------------------------===//
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

#include "CodeCompletionOrganizer.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Module.h"
#include "swift/Frontend/Frontend.h"
#include "swift/IDE/CodeCompletionResultPrinter.h"
#include "swift/IDE/FuzzyStringMatcher.h"
#include "swift/IDE/ImportDepth.h"
#include "swift/Markup/XMLUtils.h"
#include "clang/Basic/CharInfo.h"
#include "clang/Basic/Module.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/ilist.h"
#include "llvm/ADT/ilist_node.h"

using namespace SourceKit;
using namespace CodeCompletion;
using namespace swift;
using namespace ide;

namespace {
enum class ItemKind : uint8_t {
  None,
  Group,
  Result,
};
struct Item {
  std::string name;
  std::string description;
  uint8_t kind : 2;
  uint8_t isExactMatch : 1;
  double matchScore = 0.0; ///< The quality of the filter matching.
  double finalScore = -1.0; ///< The final score including match and context.
  ItemKind getKind() const { return static_cast<ItemKind>(kind); }
  Item(ItemKind k = ItemKind::None)
      : kind(static_cast<decltype(kind)>(k)), isExactMatch(0) {}
  virtual ~Item() {}
};
struct Result : public Item {
  Completion *value;
  Result(Completion *result = nullptr)
      : Item(ItemKind::Result), value(result) {}
  static bool classof(const Item *item) {
    return item->getKind() == ItemKind::Result;
  }
};
} // end anonymous namespace

struct CodeCompletion::Group : public Item {
  std::vector<std::unique_ptr<Item>> contents;
  Group() : Item(ItemKind::Group) {}
  static bool classof(const Item *item) {
    return item->getKind() == ItemKind::Group;
  }
};

//===----------------------------------------------------------------------===//
// extendCompletions
//===----------------------------------------------------------------------===//

std::vector<Completion *> SourceKit::CodeCompletion::extendCompletions(
    ArrayRef<SwiftResult *> swiftResults, CompletionSink &sink,
    SwiftCompletionInfo &info, const NameToPopularityMap *nameToPopularity,
    const Options &options, Completion *prefix, bool clearFlair) {

  ImportDepth depth;
  if (info.compilerInstance) {
    // Build import depth map.
    depth = ImportDepth(
        info.compilerInstance->getASTContext(),
        info.compilerInstance->getInvocation().getFrontendOptions());
  }

  if (info.completionContext)
    sink.adoptSwiftSink(info.completionContext->getResultSink());

  std::vector<Completion *> results;
  for (auto *result : swiftResults) {
    CompletionBuilder builder(sink, *result);
    if (result->getSemanticContext() == SemanticContextKind::OtherModule) {
      builder.setModuleImportDepth(depth.lookup(result->getModuleName()));

      if (info.completionContext->typeContextKind ==
              TypeContextKind::Required &&
          result->getKind() == CodeCompletionResultKind::Declaration) {
      }
    }

    if (prefix) {
      builder.setPrefix(prefix->getCompletionString());
      builder.setSemanticContext(prefix->getSemanticContext());
    }

    if (clearFlair) {
      builder.setFlair(CodeCompletionFlair());
      builder.setSemanticContext(SemanticContextKind::None);
    }

    // If this result is not from the current module, try to get a popularity
    // score for it.
    if (nameToPopularity) {
      builder.setPopularityFactor(
          nameToPopularity->lookup(builder.getOriginalName()));
    }

    results.push_back(builder.finish());
  }

  return results;
}

bool SourceKit::CodeCompletion::addCustomCompletions(
    CompletionSink &sink, std::vector<Completion *> &completions,
    ArrayRef<CustomCompletionInfo> customCompletions,
    CompletionKind completionKind) {

  auto addCompletion = [&](CustomCompletionInfo customCompletion) {
    using Chunk = CodeCompletionString::Chunk;
    auto nameCopy = StringRef(customCompletion.Name).copy(sink.allocator);
    auto chunk = Chunk::createWithText(Chunk::ChunkKind::Text, 0, nameCopy);
    auto *completionString =
        CodeCompletionString::create(sink.allocator, chunk);
    auto *contextFreeResult =
        ContextFreeCodeCompletionResult::createPatternOrBuiltInOperatorResult(
            sink.swiftSink, CodeCompletionResultKind::Pattern, completionString,
            CodeCompletionOperatorKind::None,
            /*BriefDocComment=*/"", CodeCompletionResultType::unknown(),
            ContextFreeNotRecommendedReason::None,
            CodeCompletionDiagnosticSeverity::None, /*DiagnosticMessage=*/"");
    auto *swiftResult = new (sink.allocator) CodeCompletion::SwiftResult(
        *contextFreeResult, SemanticContextKind::Local,
        CodeCompletionFlairBit::ExpressionSpecific,
        /*NumBytesToErase=*/0, CodeCompletionResultTypeRelation::Unrelated,
        ContextualNotRecommendedReason::None);

    CompletionBuilder builder(sink, *swiftResult);
    builder.setCustomKind(customCompletion.Kind);
    completions.push_back(builder.finish());
  };

  bool changed = false;

  for (auto &custom : customCompletions) {
    switch (completionKind) {
    case CompletionKind::StmtOrExpr:
      if (custom.Contexts.contains(CustomCompletionInfo::Stmt)) {
        changed = true;
        addCompletion(custom);
      }
      break;
    case CompletionKind::PostfixExprBeginning:
    case CompletionKind::CallArg:
    case CompletionKind::ReturnStmtExpr:
    case CompletionKind::YieldStmtExpr:
      if (custom.Contexts.contains(CustomCompletionInfo::Expr)) {
        changed = true;
        addCompletion(custom);
      }
      break;
    case CompletionKind::ForEachSequence:
      if (custom.Contexts.contains(CustomCompletionInfo::ForEachSequence)) {
        changed = true;
        addCompletion(custom);
      }
      break;
    case CompletionKind::TypePossibleFunctionParamBeginning:
    case CompletionKind::TypeDeclResultBeginning:
    case CompletionKind::TypeBeginning:
    case CompletionKind::TypeSimpleOrComposition:
    case CompletionKind::TypeSimpleBeginning:
      if (custom.Contexts.contains(CustomCompletionInfo::Type)) {
        changed = true;
        addCompletion(custom);
      }
      break;
    default:
      break;
    }
  }

  return changed;
}

//===----------------------------------------------------------------------===//
// CodeCompletionOrganizer::Impl declaration
//===----------------------------------------------------------------------===//

class CodeCompletionOrganizer::Impl {
  std::unique_ptr<Group> rootGroup;
  CompletionKind completionKind;
  TypeContextKind typeContextKind;

  void groupStemsRecursive(Group *group, bool recurseIntoNewGroups,
                           StringRef(getStem)(StringRef));

public:
  Impl(CompletionKind kind, TypeContextKind typeContextKind);

  void addCompletionsWithFilter(ArrayRef<Completion *> completions,
                                StringRef filterText, Options options,
                                const FilterRules &rules,
                                Completion *&exactMatch);

  void sort(Options options);

  void groupOverloads() {
    groupStemsRecursive(
        rootGroup.get(), /*recurseIntoNewGroups*/ false, [](StringRef name) {
          auto endIdx = name.find_first_of("([");
          if (endIdx == 0 && !name.empty())
            return name.slice(0, 1); // [ => subscript, ( => initializer
          return name.slice(0, endIdx);
        });
  }

  void groupStems() {
    groupStemsRecursive(rootGroup.get(), /*recurseIntoNewGroups*/ true,
        [](StringRef name) {
          unsigned i = 0;
          while (i < name.size()) {
            char c = name[i];
            // FIXME: unicode
            if (i > 0 && clang::isUppercase(c) &&
                !clang::isUppercase(name[i - 1]))
              break;
            if (!clang::isAlphanumeric(c))
              break;
            ++i;
          }
          if (i == 0 && !name.empty())
            return name.slice(0, 1);
          return name.slice(0, i);
        });
  }

  CodeCompletionViewRef takeView() {
    assert(rootGroup);
    auto view = std::make_shared<CodeCompletionView>();
    view->rootGroup = rootGroup.release(); // View takes ownership.

    // Reset fields manually in case the move constructors don't.
    rootGroup = nullptr;

    return view;
  }
};

//===----------------------------------------------------------------------===//
// CodeCompletionOrganizer implementation
//===----------------------------------------------------------------------===//

CodeCompletionOrganizer::CodeCompletionOrganizer(const Options &options,
                                                 CompletionKind kind,
                                                 TypeContextKind typeContextKind)
    : impl(*new Impl(kind, typeContextKind)), options(options) {}
CodeCompletionOrganizer::~CodeCompletionOrganizer() { delete &impl; }

void CodeCompletionOrganizer::preSortCompletions(
    llvm::MutableArrayRef<Completion *> completions) {
  // We do a case-sensitive sort here, then do a case-insensitive sort after any
  // name-based grouping.

  llvm::array_pod_sort(completions.begin(), completions.end(),
                       [](Completion *const *a, Completion *const *b) {

     // Sort first by filter name (case-sensitive).
     if (int primary = (*a)->getFilterName().compare((*b)->getFilterName()))
       return primary;

     // Next, sort by full description text.
     return (*a)->getDescription().compare((*b)->getDescription());
   });
}

void CodeCompletionOrganizer::addCompletionsWithFilter(
    ArrayRef<Completion *> completions, StringRef filterText,
    const FilterRules &rules, Completion *&exactMatch) {
  impl.addCompletionsWithFilter(completions, filterText, options, rules,
                                exactMatch);
}

void CodeCompletionOrganizer::groupAndSort(const Options &options) {
  if (options.groupStems)
    impl.groupStems();
  else if (options.groupOverloads)
    impl.groupOverloads();

  impl.sort(options);
}

CodeCompletionViewRef CodeCompletionOrganizer::takeResultsView() {
  return impl.takeView();
}

//===----------------------------------------------------------------------===//
// CodeCompletionOrganizer::Impl utilities
//===----------------------------------------------------------------------===//

static std::unique_ptr<Group> make_group(StringRef name) {
  auto g = std::make_unique<Group>();
  g->name = name.str();
  g->description = name.str();
  return g;
}

static std::unique_ptr<Result> make_result(Completion *result) {
  auto r = std::make_unique<Result>(result);
  r->name = result->getFilterName().str();
  r->description = result->getDescription().str();
  return r;
}


//===----------------------------------------------------------------------===//
// CodeCompletionOrganizer::Impl implementation
//===----------------------------------------------------------------------===//

CodeCompletionOrganizer::Impl::Impl(CompletionKind kind, TypeContextKind typeContextKind)
    : completionKind(kind), typeContextKind(typeContextKind) {
  assert(!rootGroup && "initialized twice");
  rootGroup = make_group("");
}

static bool matchesExpectedStyle(Completion *completion, NameStyle style) {
  switch (completion->getAssociatedDeclKind()) {
  case CodeCompletionDeclKind::Class:
  case CodeCompletionDeclKind::Actor:
  case CodeCompletionDeclKind::Struct:
  case CodeCompletionDeclKind::Enum:
  case CodeCompletionDeclKind::Protocol:
  case CodeCompletionDeclKind::TypeAlias:
  case CodeCompletionDeclKind::AssociatedType:
    return style.possiblyUpperCamelCase();

  case CodeCompletionDeclKind::StaticMethod:
  case CodeCompletionDeclKind::InstanceMethod:
  case CodeCompletionDeclKind::FreeFunction:
  case CodeCompletionDeclKind::StaticVar:
  case CodeCompletionDeclKind::InstanceVar:
  case CodeCompletionDeclKind::LocalVar:
  case CodeCompletionDeclKind::GlobalVar:
    return style.possiblyLowerCamelCase();

  default:
    return true; // Conservatively say yes.
  }
}

static bool isHighPriorityKeyword(CodeCompletionKeywordKind kind) {
  switch (kind) {
  case CodeCompletionKeywordKind::kw_let:
  case CodeCompletionKeywordKind::kw_var:
  case CodeCompletionKeywordKind::kw_if:
  case CodeCompletionKeywordKind::kw_for:
  case CodeCompletionKeywordKind::kw_while:
  case CodeCompletionKeywordKind::kw_return:
  case CodeCompletionKeywordKind::kw_func:
    return true;
  default:
    return false;
  }
}

bool FilterRules::hideFilterName(StringRef name) const {
  auto I = hideByFilterName.find(name);
  if (I != hideByFilterName.end())
    return I->getValue();
  return hideAll;
}

bool FilterRules::hideCompletion(const Completion &completion) const {
  return hideCompletion(completion.getSwiftResult(), completion.getFilterName(),
                        completion.getDescription(),
                        completion.getCustomKind());
}

bool FilterRules::hideCompletion(const SwiftResult &completion,
                                 StringRef filterName, StringRef description,
                                 void *customKind) const {

  if (!description.empty()) {
    auto I = hideByDescription.find(description);
    if (I != hideByDescription.end())
      return I->getValue();
  }

  if (!filterName.empty()) {
    auto I = hideByFilterName.find(filterName);
    if (I != hideByFilterName.end())
      return I->getValue();
  }

  switch (completion.getKind()) {
  case CodeCompletionResultKind::BuiltinOperator:
  case CodeCompletionResultKind::Declaration:
    break;
  case CodeCompletionResultKind::Keyword: {
    auto I = hideKeyword.find(completion.getKeywordKind());
    if (I != hideKeyword.end())
      return I->second;
    if (hideAllKeywords)
      return true;
    break;
  }
  case CodeCompletionResultKind::Pattern: {
    if (customKind) {
      // FIXME: individual custom completions
      if (hideCustomCompletions)
        return true;
    }
    break;
  }
  case CodeCompletionResultKind::Literal: {
    auto I = hideValueLiteral.find(completion.getLiteralKind());
    if (I != hideValueLiteral.end())
      return I->second;
    if (hideAllValueLiterals)
      return true;
    break;
  }
  }

  if (!completion.getModuleName().empty()) {
    // FIXME: try each submodule chain starting from the most specific.
    auto M = hideModule.find(completion.getModuleName());
    if (M != hideModule.end())
      return M->getValue();
  }

  return hideAll;
}

void CodeCompletionOrganizer::Impl::addCompletionsWithFilter(
    ArrayRef<Completion *> completions, StringRef filterText, Options options,
    const FilterRules &rules, Completion *&exactMatch) {
  assert(rootGroup);

  auto &contents = rootGroup->contents;

  // If we have no filter text, add all non-hidden results.
  if (filterText.empty()) {
    bool hideLowPriority =
        options.hideLowPriority &&
        completionKind != CompletionKind::TypePossibleFunctionParamBeginning &&
        completionKind != CompletionKind::TypeDeclResultBeginning &&
        completionKind != CompletionKind::TypeBeginning &&
        completionKind != CompletionKind::TypeSimpleOrComposition &&
        completionKind != CompletionKind::TypeSimpleBeginning &&
        completionKind != CompletionKind::PostfixExpr;
    for (Completion *completion : completions) {
      if (rules.hideCompletion(*completion))
        continue;

      if (options.hideLowPriority &&
          (completion->isNotRecommended() ||
           completion->getExpectedTypeRelation() ==
               CodeCompletionResultTypeRelation::Invalid))
        continue;

      NameStyle style(completion->getFilterName());
      bool hideUnderscore = options.hideUnderscores && style.leadingUnderscores;
      if (hideUnderscore && options.reallyHideAllUnderscores)
        continue;

      bool hideByNameStyle =
          options.hideByNameStyle &&
          completion->getKind() == CodeCompletionResultKind::Declaration &&
          !matchesExpectedStyle(completion, style);

      hideByNameStyle |= hideUnderscore;

      switch (completion->getSemanticContext()) {
      case SemanticContextKind::Super:
      case SemanticContextKind::OutsideNominal:
        if (hideUnderscore)
          continue;
        break;
      case SemanticContextKind::OtherModule:
      case SemanticContextKind::None:
        if (auto depth = completion->getModuleImportDepth()) {
          if (*depth == 0) // Treat as if it's "thismodule"
            break;
        }
        if (completion->getExpectedTypeRelation() >=
                CodeCompletionResultTypeRelation::Convertible ||
            (completion->getKind() == CodeCompletionResultKind::Literal &&
             completionKind != CompletionKind::StmtOrExpr &&
             typeContextKind < TypeContextKind::Required))
          break;

        if (completion->getKind() == CodeCompletionResultKind::Keyword &&
            completionKind == CompletionKind::StmtOrExpr &&
            isHighPriorityKeyword(completion->getKeywordKind()))
          break;

        if (hideByNameStyle || hideLowPriority)
          continue;
        break;
      default:
        break;
      }

      // Build wrapper and add to results.
      contents.push_back(make_result(completion));
    }
    return;
  }

  FuzzyStringMatcher pattern(filterText);
  pattern.normalize = true;
  for (Completion *completion : completions) {
    if (rules.hideCompletion(*completion))
      continue;

    // Hide literals other than the ones that are also keywords if they don't
    // match the expected types.
    if (completion->getKind() == CodeCompletionResultKind::Literal &&
        typeContextKind == TypeContextKind::Required &&
        completion->getExpectedTypeRelation() <
            CodeCompletionResultTypeRelation::Convertible &&
        completion->getLiteralKind() !=
            CodeCompletionLiteralKind::BooleanLiteral &&
        completion->getLiteralKind() != CodeCompletionLiteralKind::NilLiteral)
      continue;

    bool match = false;
    if (options.fuzzyMatching && filterText.size() >= options.minFuzzyLength) {
      match = pattern.matchesCandidate(completion->getFilterName());
    } else {
      match = completion->getFilterName().starts_with_insensitive(filterText);
    }

    bool isExactMatch =
        match && completion->getFilterName().equals_insensitive(filterText);

    if (isExactMatch) {
      if (!exactMatch) { // first match
        exactMatch = completion;
      } else if (completion->getFilterName() != exactMatch->getFilterName()) {
        if (completion->getFilterName() == filterText && // first case-sensitive match
            exactMatch->getFilterName() != filterText)
          exactMatch = completion;
        else if (pattern.scoreCandidate(completion->getFilterName()) > // better match
                 pattern.scoreCandidate(exactMatch->getFilterName()))
          exactMatch = completion;
      }

      match = (options.addInnerResults || options.addInnerOperators)
                  ? options.includeExactMatch
                  : true;
    }

    // Build wrapper and add to results.
    if (match) {
      auto wrapper = make_result(completion);
      if (options.fuzzyMatching) {
        wrapper->matchScore = pattern.scoreCandidate(completion->getFilterName());
      }
      wrapper->isExactMatch = isExactMatch;

      contents.push_back(std::move(wrapper));
    }
  }
}

static double getSemanticContextScore(bool useImportDepth,
                                      Completion *completion) {
  double order = -1.0;
  switch (completion->getSemanticContext()) {
  case SemanticContextKind::Local: order = 1; break;
  case SemanticContextKind::CurrentNominal: order = 2; break;
  case SemanticContextKind::Super: order = 3; break;
  case SemanticContextKind::OutsideNominal: order = 4; break;
  case SemanticContextKind::CurrentModule: order = 5; break;
  case SemanticContextKind::OtherModule: {
    unsigned depth = Completion::maxModuleImportDepth + 1; // unknown > known
    if (useImportDepth && completion->getModuleImportDepth())
      depth = *completion->getModuleImportDepth();
    // We treat depth == 0 the same as CurrentModule.
    // Note: there is a gap in between that we use for keywords.
    order = (depth == 0) ? 5.0 : 6.0; // Base value.
    order += double(depth) / (Completion::maxModuleImportDepth + 1);
    assert((depth == 0 && order == 5.0) ||
           (depth && 6.0 <= order && order <= 7.0));
    break;
  }
  case SemanticContextKind::None: {
    order =
        completion->getKind() == CodeCompletionResultKind::Keyword ? 5.5 : 8.0;
    break;
  }
  }
  assert(0.0 <= order && order <= 8.0);
  return (8.0 - order) / 8.0;
}

static double combinedScore(const Options &options, double matchScore,
                            Completion *completion) {

  double score = matchScore * options.fuzzyMatchWeight;
  score += getSemanticContextScore(options.useImportDepth, completion) *
           options.semanticContextWeight;

  PopularityFactor popularity = completion->getPopularityFactor();
  if (popularity.isPopular())
    score += popularity.rawValue + options.popularityBonus;
  else if (popularity.isUnpopular())
    score += popularity.rawValue - options.popularityBonus;

  return score;
}

static int compareResultName(Item &a, Item &b) {
  // Sort first by filter name (case-insensitive).
  if (int primary = StringRef(a.name).compare_insensitive(b.name))
    return primary;

  // Next, sort by full description text.
  return a.description.compare(b.description);
}

namespace {
enum class ResultBucket {
  NotRecommended,
  Normal,
  Literal,
  NormalTypeMatch,
  LiteralTypeMatch,
  HighPriorityKeyword,
  Operator,
  ExpressionSpecific,
  ExactMatch,
};
} // end anonymous namespace

static ResultBucket getResultBucket(Item &item, bool hasRequiredTypes,
                                    bool skipMetaGroups = false) {
  if (item.isExactMatch && !skipMetaGroups)
    return ResultBucket::ExactMatch;

  if (isa<Group>(item))
    return ResultBucket::Normal; // FIXME: take best contained result.
  auto *completion = cast<Result>(item).value;

  if (completion->isNotRecommended() && !skipMetaGroups)
    return ResultBucket::NotRecommended;

  if (!skipMetaGroups) {
    auto flair = completion->getFlair();
    if (flair.contains(CodeCompletionFlairBit::ExpressionSpecific) ||
        flair.contains(CodeCompletionFlairBit::SuperChain) ||
        flair.contains(CodeCompletionFlairBit::ArgumentLabels))
      return ResultBucket::ExpressionSpecific;
  }

  if (completion->isOperator())
    return ResultBucket::Operator;

  switch (completion->getKind()) {
  case CodeCompletionResultKind::Literal:
    if (completion->getExpectedTypeRelation() >=
        CodeCompletionResultTypeRelation::Convertible) {
      return ResultBucket::LiteralTypeMatch;
    } else if (!hasRequiredTypes) {
      return ResultBucket::Literal;
    } else {
      // When we have type context, we still show literals that are keywords,
      // but we treat them as keywords instead of literals for prioritization.
      return ResultBucket::Normal;
    }
  case CodeCompletionResultKind::Keyword:
    return isHighPriorityKeyword(completion->getKeywordKind())
               ? ResultBucket::HighPriorityKeyword
               : ResultBucket::Normal;
  case CodeCompletionResultKind::Pattern:
  case CodeCompletionResultKind::Declaration:
    switch (completion->getExpectedTypeRelation()) {
    case swift::ide::CodeCompletionResultTypeRelation::Convertible:
      return ResultBucket::NormalTypeMatch;
    case swift::ide::CodeCompletionResultTypeRelation::NotApplicable:
    case swift::ide::CodeCompletionResultTypeRelation::Unknown:
    case swift::ide::CodeCompletionResultTypeRelation::Unrelated:
      return ResultBucket::Normal;
    case swift::ide::CodeCompletionResultTypeRelation::Invalid:
      if (!skipMetaGroups)
        return ResultBucket::NotRecommended;
      return ResultBucket::Normal;
    }
  case CodeCompletionResultKind::BuiltinOperator:
    llvm_unreachable("operators should be handled above");
  }
}

template <class T, size_t N>
static size_t getIndex(const T (&array)[N], T element) {
  auto I = std::find(array, &array[N], element);
  assert(I != &array[N]);
  return std::distance(array, I);
}

static int compareHighPriorityKeywords(Item &a_, Item &b_) {
  static CodeCompletionKeywordKind order[] = {
    CodeCompletionKeywordKind::kw_let,
    CodeCompletionKeywordKind::kw_var,
    CodeCompletionKeywordKind::kw_if,
    CodeCompletionKeywordKind::kw_for,
    CodeCompletionKeywordKind::kw_while,
    CodeCompletionKeywordKind::kw_return,
    CodeCompletionKeywordKind::kw_func,
  };

  auto a = getIndex(order, cast<Result>(a_).value->getKeywordKind());
  auto b = getIndex(order, cast<Result>(b_).value->getKeywordKind());
  return a < b ? -1 : (b < a ? 1 : 0);
}

static int compareLiterals(Item &a_, Item &b_) {
  static CodeCompletionLiteralKind order[] = {
    CodeCompletionLiteralKind::IntegerLiteral,
    CodeCompletionLiteralKind::StringLiteral,
    CodeCompletionLiteralKind::BooleanLiteral,
    CodeCompletionLiteralKind::ColorLiteral,
    CodeCompletionLiteralKind::ImageLiteral,
    CodeCompletionLiteralKind::ArrayLiteral,
    CodeCompletionLiteralKind::DictionaryLiteral,
    CodeCompletionLiteralKind::Tuple,
    CodeCompletionLiteralKind::NilLiteral,
  };

  auto a = getIndex(order, cast<Result>(a_).value->getLiteralKind());
  auto b = getIndex(order, cast<Result>(b_).value->getLiteralKind());

  if (a != b)
    return a < b ? -1 : 1;

  // Sort true before false instead of alphabetically.
  if (cast<Result>(a_).value->getLiteralKind() == CodeCompletionLiteralKind::BooleanLiteral)
    return b_.name.compare(a_.name);

  return 0;
}

static int compareOperators(Item &a_, Item &b_) {
  using CCOK = CodeCompletionOperatorKind;
  static CCOK order[] = {
      CCOK::Dot,         // .
      CCOK::QuestionDot, // ?.
      CCOK::Bang,        // !
      CCOK::LParen, // ( -- not really an operator, but treated as one in some
                    // cases.
      CCOK::Eq,     // =

      CCOK::EqEq,      // ==
      CCOK::NotEq,     // !=
      CCOK::Less,      // <
      CCOK::Greater,   // >
      CCOK::LessEq,    // <=
      CCOK::GreaterEq, // >=

      CCOK::Plus,   // +
      CCOK::Minus,  // -
      CCOK::Star,   // *
      CCOK::Slash,  // /
      CCOK::Modulo, // %

      CCOK::PlusEq,   // +=
      CCOK::MinusEq,  // -=
      CCOK::StarEq,   // *=
      CCOK::SlashEq,  // /=
      CCOK::ModuloEq, // %=

      CCOK::AmpAmp,   // &&
      CCOK::PipePipe, // ||

      CCOK::Unknown,

      CCOK::Amp,            // &
      CCOK::Pipe,           // |
      CCOK::Caret,          // ^
      CCOK::LessLess,       // <<
      CCOK::GreaterGreater, // >>

      CCOK::AmpEq,            // &=
      CCOK::PipeEq,           // |=
      CCOK::CaretEq,          // ^=
      CCOK::LessLessEq,       // <<=
      CCOK::GreaterGreaterEq, // >>=

      // Range
      CCOK::DotDotDot,  // ...
      CCOK::DotDotLess, // ..<

      CCOK::AmpStar,  // &*
      CCOK::AmpPlus,  // &+
      CCOK::AmpMinus, // &-

      // Misc.
      CCOK::EqEqEq,  // ===
      CCOK::NotEqEq, // !==
      CCOK::TildeEq, // ~=
  };

  auto a = getIndex(order, cast<Result>(a_).value->getOperatorKind());
  auto b = getIndex(order, cast<Result>(b_).value->getOperatorKind());
  return a < b ? -1 : (b < a ? 1 : 0);
}

static bool isTopNonLiteralResult(Item &item, ResultBucket literalBucket) {
  if (isa<Group>(item))
    return true; // FIXME: should have a semantic context for groups.
  auto *completion = cast<Result>(item).value;

  switch (literalBucket) {
  case ResultBucket::Literal:
    return completion->getSemanticContext() <=
           SemanticContextKind::CurrentNominal;
  case ResultBucket::LiteralTypeMatch:
    return completion->getExpectedTypeRelation() >=
           CodeCompletionResultTypeRelation::Convertible;
  default:
    llvm_unreachable("invalid literal bucket");
  }
}

static void sortTopN(const Options &options, Group *group,
                     bool hasRequiredTypes) {

  auto &contents = group->contents;
  if (contents.empty() || options.showTopNonLiteralResults == 0)
    return;

  auto best = getResultBucket(*contents[0], hasRequiredTypes);
  if (best == ResultBucket::LiteralTypeMatch || best == ResultBucket::Literal) {

    unsigned beginNewIndex = 0;
    unsigned endNewIndex = 0;
    for (unsigned i = 1; i < contents.size(); ++i) {
      auto bucket = getResultBucket(*contents[i], hasRequiredTypes);
      if (bucket < best) {
        // This algorithm assumes we don't have both literal and
        // literal-type-match at the start of the list.
        assert(bucket != ResultBucket::Literal);
        if (isTopNonLiteralResult(*contents[i], best)) {
          beginNewIndex = i;
          endNewIndex = beginNewIndex + 1;
          for (; endNewIndex < contents.size() &&
                 endNewIndex < beginNewIndex + options.showTopNonLiteralResults;
               ++endNewIndex) {
            if (!isTopNonLiteralResult(*contents[endNewIndex], best))
              break;
          }
        }
        break;
      }
    }

    if (!beginNewIndex)
      return;

    assert(endNewIndex > beginNewIndex && endNewIndex <= contents.size());

    // Temporarily copy the first result to temporary storage.
    SmallVector<Item *, 16> firstResults;
    for (unsigned i = 0; i < endNewIndex; ++i) {
      firstResults.push_back(contents[i].release());
    }

    // Swap the literals with the next few results.
    for (unsigned ci = 0, i = beginNewIndex; i < endNewIndex; ++i, ++ci) {
      assert(ci < contents.size() && !contents[ci]);
      contents[ci] = std::unique_ptr<Item>(firstResults[i]);
    }
    unsigned topN = endNewIndex - beginNewIndex;
    assert(topN <= options.showTopNonLiteralResults);
    for (unsigned ci = topN, i = 0; i < beginNewIndex; ++i, ++ci) {
      assert(ci < contents.size() && !contents[ci]);
      contents[ci] = std::unique_ptr<Item>(firstResults[i]);
    }
  }
}

static void sortRecursive(const Options &options, Group *group,
                          bool hasRequiredTypes) {
  // Sort all of the subgroups first, and fill in the bucket for each result.
  auto &contents = group->contents;
  double best = -1.0;
  for (auto &item : contents) {
    if (auto *g = dyn_cast<Group>(item.get())) {
      sortRecursive(options, g, hasRequiredTypes);
    } else {
      Result *r = cast<Result>(item.get());
      item->finalScore = combinedScore(options, item->matchScore, r->value);
    }

    if (item->finalScore > best)
      best = item->finalScore;
  }

  group->finalScore = best;

  // Now sort the group itself.

  if (options.sortByName) {
    llvm::array_pod_sort(contents.begin(), contents.end(),
        [](const std::unique_ptr<Item> *a, const std::unique_ptr<Item> *b) {
      return compareResultName(**a, **b);
    });
    return;
  }

  std::sort(contents.begin(), contents.end(), [=](const std::unique_ptr<Item> &a_, const std::unique_ptr<Item> &b_) {
    Item &a = *a_;
    Item &b = *b_;

    auto bucketA = getResultBucket(a, hasRequiredTypes);
    auto bucketB = getResultBucket(b, hasRequiredTypes);
    if (bucketA < bucketB)
      return false;
    else if (bucketB < bucketA)
      return true;

    // Try again, skipping any meta groups like "expr-specific" in case that
    // lets us order
    if (bucketA == ResultBucket::ExactMatch ||
        bucketA == ResultBucket::ExpressionSpecific ||
        bucketA == ResultBucket::NotRecommended) {
      bucketA = getResultBucket(a, hasRequiredTypes, /*skipMetaGroups*/ true);
      bucketB = getResultBucket(b, hasRequiredTypes, /*skipMetaGroups*/ true);
      if (bucketA < bucketB)
        return false;
      else if (bucketB < bucketA)
        return true;
    }

    // Special internal orderings.
    switch (bucketA) {
    case ResultBucket::HighPriorityKeyword:
      return compareHighPriorityKeywords(a, b) < 0;
    case ResultBucket::Literal:
    case ResultBucket::LiteralTypeMatch:
      return compareLiterals(a, b) < 0;
    case ResultBucket::Operator: {
      int cmp = compareOperators(a, b);
      if (cmp != 0)
        return cmp < 0;
      break;
    }
    default:
      break;
    }

    // "Normal" order.
    if (a.finalScore < b.finalScore)
      return false;
    else if (b.finalScore < a.finalScore)
      return true;

    return compareResultName(a, b) < 0;
  });
}

void CodeCompletionOrganizer::Impl::sort(Options options) {
  bool hasRequiredTypes = typeContextKind == TypeContextKind::Required;
  sortRecursive(options, rootGroup.get(), hasRequiredTypes);
  if (options.showTopNonLiteralResults != 0)
    sortTopN(options, rootGroup.get(), hasRequiredTypes);
}

void CodeCompletionOrganizer::Impl::groupStemsRecursive(
    Group *group, bool recurseIntoNewGroups, StringRef(getStem)(StringRef)) {
  std::vector<std::unique_ptr<Item>> newContents;
  std::vector<std::unique_ptr<Item>> &worklist = group->contents;

  auto getSubStem = [getStem](StringRef name, StringRef groupName) {
    StringRef subName = name.slice(groupName.size(), StringRef::npos);
    return getStem(subName);
  };

  if (worklist.empty())
    return;

  auto start = worklist.begin();
  while (start != worklist.end()) {
    if (auto *g = dyn_cast<Group>(start->get())) {
      groupStemsRecursive(g, recurseIntoNewGroups, getStem);
      newContents.push_back(std::move(*start));
      ++start;
      continue;
    }

    StringRef stem = getSubStem((*start)->name, group->name);
    auto end = start;
    auto next = ++end;
    while (end != worklist.end() && !stem.empty() &&
           stem == getSubStem((*end)->name, group->name))
      ++end;
    if (end == next) {
      // Only one element; don't group.
      newContents.push_back(std::move(*start));
      start = end; // == next == ++start
    } else if (end == worklist.end() && newContents.empty()) {
      // Only one group; inline it.
      // FIXME: this is wrong, we should try to sub-group.
      assert(start == worklist.begin());
      std::swap(newContents, worklist);
      break;
    } else {
      std::string name = (Twine(group->name) + stem).str();
      if (recurseIntoNewGroups) {
        while (true) {
          StringRef next = getSubStem((*start)->name, name);
          if (next.empty())
            break;
          auto I = start; ++I;
          for ( ; I != end; ++I)
            if (next != getSubStem((*I)->name, name))
              goto done;
          name += next;
        }
      done:
        ; // exit label
      }

      auto newGroup = make_group(name);
      for (; start != end; ++start)
        newGroup->contents.push_back(std::move(*start));

      if (recurseIntoNewGroups)
        groupStemsRecursive(newGroup.get(), recurseIntoNewGroups, getStem);
      newContents.push_back(std::move(newGroup));
    }
  }

  group->contents = std::move(newContents);
}

//===----------------------------------------------------------------------===//
// CodeCompletionView
//===----------------------------------------------------------------------===//

static bool walkRecursive(CodeCompletionView::Walker &walker, const Item *item) {
  if (auto *result = dyn_cast<Result>(item))
    return walker.handleResult(result->value);

  walker.startGroup(item->name);
  for (auto &child : cast<Group>(item)->contents) {
    if (!walkRecursive(walker, child.get()))
      return false;
  }
  walker.endGroup();
  return true;
}

bool CodeCompletionView::walk(CodeCompletionView::Walker &walker) const {
  assert(rootGroup);
  return walkRecursive(walker, rootGroup);
}

CodeCompletionView::~CodeCompletionView() { delete rootGroup; }

unsigned LimitedResultView::getNextOffset() const { return start; }

bool LimitedResultView::walk(CodeCompletionView::Walker &walker) const {
  const Group *root = baseView.rootGroup;
  walker.startGroup(root->name);
  auto begin = root->contents.begin();
  auto end = root->contents.end();
  unsigned count = 0;
  while (count < start && begin != end) {
    ++count;
    ++begin;
  }
  while (begin != end && (maxResults == 0 || count < start + maxResults)) {
    if (!walkRecursive(walker, begin->get()))
      return false;
    ++count;
    ++begin;
  }

  if (begin == end) {
    assert(maxResults == 0 || count <= start + maxResults);
    start = 0;
  } else {
    start = count;
  }

  walker.endGroup();
  return true;
}

//===----------------------------------------------------------------------===//
// CompletionBuilder
//===----------------------------------------------------------------------===//

CompletionBuilder::CompletionBuilder(CompletionSink &sink,
                                     const SwiftResult &base)
    : sink(sink), base(base) {
  semanticContext = base.getSemanticContext();
  flair = base.getFlair();
  completionString = nullptr;
}

void CompletionBuilder::setPrefix(CodeCompletionString *prefix) {
  if (!prefix)
    return;

  modified = true;

  // The underlying text is kept alive by an CompletionSink.  The chunks
  // themselves get copied into the CodeCompletionString.
  std::vector<CodeCompletionString::Chunk> chunks;
  chunks.reserve(prefix->getChunks().size());
  for (auto chunk : prefix->getChunks()) {
    if (chunk.is(CodeCompletionString::Chunk::ChunkKind::TypeAnnotation))
      continue; // The type is the type of the actual result, not the prefix.
    chunks.push_back(chunk);
  }

  auto existing = base.getCompletionString()->getChunks();
  chunks.insert(chunks.end(), existing.begin(), existing.end());
  completionString = CodeCompletionString::create(sink.allocator, chunks);
}

Completion *CompletionBuilder::finish() {
  const SwiftResult *newBase = &this->base;
  llvm::SmallString<64> nameStorage;
  if (modified) {
    // We've modified the original result, so build a new one.
    auto opKind = CodeCompletionOperatorKind::None;
    if (base.isOperator())
      opKind = base.getOperatorKind();

    const ContextFreeCodeCompletionResult &contextFreeBase =
        base.getContextFreeResult();

    auto *newCompletionString = contextFreeBase.getCompletionString();
    auto newFilterName = contextFreeBase.getFilterName();
    if (completionString) {
      newCompletionString = completionString;
      newFilterName =
          getCodeCompletionResultFilterName(completionString, sink.allocator);
    }

    ContextFreeCodeCompletionResult *contextFreeResult =
        new (sink.allocator) ContextFreeCodeCompletionResult(
            contextFreeBase.getKind(),
            contextFreeBase.getOpaqueAssociatedKind(), opKind,
            contextFreeBase.getMacroRoles(), contextFreeBase.isSystem(),
            contextFreeBase.hasAsyncAlternative(),
            newCompletionString, contextFreeBase.getModuleName(),
            contextFreeBase.getBriefDocComment(),
            contextFreeBase.getAssociatedUSRs(),
            contextFreeBase.getResultType(),
            contextFreeBase.getNotRecommendedReason(),
            contextFreeBase.getDiagnosticSeverity(),
            contextFreeBase.getDiagnosticMessage(), newFilterName,
            contextFreeBase.getNameForDiagnostics());
    newBase = base.withContextFreeResultSemanticContextAndFlair(
        *contextFreeResult, semanticContext, flair, sink.swiftSink);
  }

  llvm::SmallString<64> description;
  {
    llvm::raw_svector_ostream OSS(description);
    ide::printCodeCompletionResultDescription(*newBase, OSS,
                                              /*leadingPunctuation=*/true);
  }

  auto *result = new (sink.allocator)
      Completion(*newBase, description.str().copy(sink.allocator));
  result->moduleImportDepth = moduleImportDepth;
  result->popularityFactor = popularityFactor;
  result->opaqueCustomKind = customKind;
  return result;
}

//===----------------------------------------------------------------------===//
// NameStyle
//===----------------------------------------------------------------------===//

NameStyle::NameStyle(StringRef name)
    : leadingUnderscores(0), trailingUnderscores(0) {
  // Trim leading and trailing underscores.
  StringRef center = name.ltrim("_");
  if (center == "")
    return;
  leadingUnderscores = name.size() - center.size();
  center = center.rtrim("_");
  assert(!center.empty());
  trailingUnderscores = name.size() - center.size() - leadingUnderscores;

  unsigned pos = 0;

  enum Case {
    None = 0,
    Lower,
    Upper,
  };

  auto caseOf = [](char c) {
    if (clang::isLowercase(c))
      return Lower;
    if (clang::isUppercase(c))
      return Upper;
    return None;
  };

  unsigned underscores = 0;
  unsigned caseCount[3] = {0, 0, 0};
  Case leadingCase = None;
  for (; pos < center.size(); ++pos) {
    char c = center[pos];
    Case curCase = caseOf(c);
    if (!leadingCase)
      leadingCase = curCase;

    underscores += (c == '_');
    caseCount[curCase] += 1;
  }

  assert(caseCount[leadingCase] > 0);

  if (caseCount[Lower] && !caseCount[Upper]) {
    wordDelimiter = underscores ? LowercaseWithUnderscores : Lowercase;
    return;
  }
  if (caseCount[Upper] && !caseCount[Lower]) {
    wordDelimiter = underscores ? UppercaseWithUnderscores : Uppercase;
    return;
  }
  if (leadingCase && !underscores) {
    wordDelimiter = leadingCase == Lower ? LowerCamelCase : UpperCamelCase;
    return;
  }

  // FIXME: should we try to choose a delimiter if there is more than one?
  wordDelimiter = Unknown;
}
