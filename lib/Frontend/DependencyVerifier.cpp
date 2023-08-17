//===--- DependencyVerifier.cpp - Dependency Verifier ---------------------===//
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
//  Implements a verifier for dependencies registered against the
//  ReferencedNameTracker in a SourceFile.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/SourceFile.h"
#include "swift/Basic/OptionSet.h"
#include "swift/Demangling/Demangler.h"
#include "swift/Frontend/DiagnosticVerifier.h"
#include "swift/Parse/Lexer.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/Support/FormatVariadic.h"

using namespace swift;

namespace {

/// An \c Expectation represents a user-provided expectation for a particular
/// dependency entry. An expectation is usually written in-line in a comment
/// attached near the relevant declaration and takes one of the following forms:
///
/// // expected-provides {{ProvidedName}}
/// // expected-member {{some.User.member}}
///
/// An expectation contains additional information about the expectation
/// \c Kind, which matches one of the few kinds of dependency entry that are
/// currently representable in the dependency graph.
///
/// As not all combinations of scopes and kinds makes sense, and to ease the
/// addition of further combinations of the two, the supported set of
/// expectations is given by the following matrix:
///
#define EXPECTATION_MATRIX                                                     \
  MATRIX_ENTRY("expected-no-dependency", Negative)                             \
  MATRIX_ENTRY("expected-provides", Provides)                                  \
  MATRIX_ENTRY("expected-superclass", Superclass)                              \
  MATRIX_ENTRY("expected-conformance", Conformance)                            \
  MATRIX_ENTRY("expected-member", Member)                                      \
  MATRIX_ENTRY("expected-dynamic-member", DynamicMember)                       \
///
/// To add a new supported combination, update \c Expectation::Kind
/// then define a new \c MATRIX_ENTRY with the following information:
///
/// MATRIX_ENTRY(<Expectation-Selector-String>, Expectation::Kind)
///
/// Where \c <Expectation-Selector-String> matches the grammar for an
/// expectation. The verifier's parsing routines use this matrix to
/// automatically keep the parser in harmony with the internal representation of
/// the expectation.
struct Expectation {
public:
  enum class Kind : uint8_t {
    Negative,
    Provides,
    Member,
    PotentialMember,
    Superclass = PotentialMember,
    Conformance = PotentialMember,
    DynamicMember,
  };

  /// The full range of the "expected-foo {{}}".
  const char *ExpectedStart, *ExpectedEnd = nullptr;

  /// Additional information about the expectation.
  struct {
    Expectation::Kind Kind;
  } Info;

  /// The raw input buffer for the message text, the part in the {{...}}
  StringRef MessageRange;

public:
  Expectation(const char *estart, const char *eend, Expectation::Kind k,
              StringRef r)
      : ExpectedStart(estart), ExpectedEnd(eend), Info{k}, MessageRange(r) {
    assert(ExpectedStart <= MessageRange.data() &&
           "Message range appears before expected start!");
    assert(MessageRange.data() + MessageRange.size() <= ExpectedEnd &&
           "Message range extends beyond expected end!");
  }
};

/// An \c Obligation represents a compiler-provided entry in the set of
/// dependencies for a given source file. Similar to an \c Expectation an
/// \c Obligation contains a name, information about its kind and flavor, and an
/// extra source location that can be used to guide where diagnostics are
/// emitted. Unlike an \c Expectation, it provides an extra piece of state that
/// represents the obligation's "fulfillment status".
///
/// All \c Obligations begin in the \c Owed state. Once an obligation has been
/// paired with a matching \c Expectation, the obligation may then transition to
/// either \c Fulfilled if it has been satisfied completely, or \c Failed
/// otherwise. The verifier turns all unfulfilled obligations into errors.
struct Obligation {
  /// The state of an \c Obligation
  enum class State : uint8_t {
    /// The \c Obligation is owed and has not been paired with a corresponding
    /// \c Expectation.
    Owed,
    /// The \c Obligation is fulfilled.
    Fulfilled,
    /// The \c Obligation was matched against an \c Expectation, but that
    /// expectation could not
    /// fulfill the obligation because additional requirements did not pass.
    Failed,
  };

  /// A token returned when an \c Obligation is fulfilled or failed. An \c
  /// Obligation is the only type that may construct fulfillment tokens.
  ///
  /// \c FulfillmentToken prevents misuse of the \c Obligation
  /// structure by requiring its state to be changed along all program paths.
  struct FulfillmentToken {
    friend Obligation;

  private:
    FulfillmentToken() = default;
  };

  /// An \c Obligation::Key is a reduced set of the common data contained in an
  /// \c Obligation and an \c Expectation.
  ///
  /// This provides a way to use a value of either type to index into an  \c
  /// ObligationMap.
  struct Key {
    StringRef Name;
    Expectation::Kind Kind;

  public:
    Key() = delete;

  private:
    Key(StringRef Name, Expectation::Kind Kind) : Name(Name), Kind(Kind) {}

  public:
    static Key forNegative(StringRef name) {
      return Key{name, Expectation::Kind::Negative};
    }

    static Key forProvides(StringRef name) {
      return Key{name, Expectation::Kind::Provides};
    }

    static Key forDynamicMember(StringRef name) {
      return Key{name, Expectation::Kind::DynamicMember};
    }

    static Key forPotentialMember(StringRef name) {
      return Key{name, Expectation::Kind::PotentialMember};
    }

    static Key forMember(StringRef name) {
      return Key{name, Expectation::Kind::Member};
    }

    static Key forExpectation(const Expectation &E) {
      return Key{E.MessageRange, E.Info.Kind};
    }

  public:
    struct Info {
      static inline Obligation::Key getEmptyKey() {
        return Obligation::Key{llvm::DenseMapInfo<StringRef>::getEmptyKey(),
                               static_cast<Expectation::Kind>(~0)};
      }
      static inline Obligation::Key getTombstoneKey() {
        return Obligation::Key{llvm::DenseMapInfo<StringRef>::getTombstoneKey(),
                               static_cast<Expectation::Kind>(~0U - 1)};
      }
      static unsigned getHashValue(const Obligation::Key &Val) {
        return llvm::hash_combine(Val.Name, Val.Kind);
      }
      static bool isEqual(const Obligation::Key &LHS,
                          const Obligation::Key &RHS) {
        return LHS.Name.equals(RHS.Name) && LHS.Kind == RHS.Kind;
      }
    };
  };

private:
  StringRef name;
  Expectation::Kind kind;
  State state;

public:
  Obligation(StringRef name, Expectation::Kind k)
      : name(name), kind{k}, state(State::Owed) {
    assert(k != Expectation::Kind::Negative &&
           "Cannot form negative obligation!");
  }

  Expectation::Kind getKind() const { return kind; }
  StringRef getName() const { return name; }

  StringRef renderAsFixit(ASTContext &Ctx) const {
    llvm::StringRef selector =
#define MATRIX_ENTRY(SELECTOR, KIND)                                           \
  if (getKind() == Expectation::Kind::KIND) {                                  \
    return SELECTOR;                                                           \
  }

        [this]() -> StringRef {
      EXPECTATION_MATRIX
      return "";
    }();
#undef MATRIX_ENTRY
    return Ctx.AllocateCopy(("// " + selector + "{{" + getName() + "}}").str());
  }

public:
  bool isOwed() const { return state == State::Owed; }
  FulfillmentToken fulfill() {
    assert(state == State::Owed &&
           "Cannot fulfill an obligation more than once!");
    state = State::Fulfilled;
    return FulfillmentToken{};
  }
  FulfillmentToken fail() {
    assert(state == State::Owed && "Cannot fail an obligation more than once!");
    state = State::Failed;
    return FulfillmentToken{};
  }
};

/// The \c DependencyVerifier implements routines to verify a set of \c
/// Expectations in a given source file meet and match a set of \c Obligations
/// in the referenced name trackers associated with that file.
class DependencyVerifier {
  SourceManager &SM;

public:
  explicit DependencyVerifier(SourceManager &SM) : SM(SM) {}

  bool verifyFile(const SourceFile *SF);

public:
  using ObligationMap = llvm::MapVector<
      Obligation::Key, Obligation,
      llvm::DenseMap<Obligation::Key, unsigned, Obligation::Key::Info>>;
  using NegativeExpectationMap = llvm::StringMap<Expectation>;

private:
  /// These routines return \c true on failure, \c false otherwise.
  bool parseExpectations(const SourceFile *SF,
                         std::vector<Expectation> &Expectations);
  bool constructObligations(const SourceFile *SF, ObligationMap &map);

  bool verifyObligations(const SourceFile *SF,
                         const std::vector<Expectation> &Exs,
                         ObligationMap &Obs,
                         NegativeExpectationMap &NegativeExpectations);

  bool verifyNegativeExpectations(const SourceFile *SF, ObligationMap &Obs,
                                  NegativeExpectationMap &Negs);

  bool diagnoseUnfulfilledObligations(const SourceFile *SF, ObligationMap &OM);

private:
  /// Given an \c ObligationMap and an \c Expectation, attempt to identify a
  /// corresponding owed \c Obligation and verify it. If there is a matching
  /// obligation, the \p fulfill callback is given the obligation. Otherwise \p
  /// fail is called with the unmatched expectation value.
  void matchExpectationOrFail(
      ObligationMap &OM, const Expectation &expectation,
      llvm::function_ref<Obligation::FulfillmentToken(Obligation &)> fulfill,
      llvm::function_ref<void(const Expectation &)> fail) {
    auto entry = OM.find(Obligation::Key::forExpectation(expectation));
    if (entry == OM.end()) {
      return fail(expectation);
    } else {
      fulfill(entry->second);
    }
  }

  /// For each owed \c Obligation, call the provided callback with its
  /// relevant name data and the Obligation itself.
  void
  forEachOwedObligation(ObligationMap &OM,
                        llvm::function_ref<void(StringRef, Obligation &)> f) {
    for (auto &p : OM) {
      if (p.second.isOwed())
        f(p.first.Name, p.second);
    }
  }

private:
  StringRef copyQualifiedTypeName(ASTContext &Ctx, NominalTypeDecl *subject) {
    auto printOptions = PrintOptions();
    printOptions.FullyQualifiedTypes = true;
    auto key = subject->getDeclaredInterfaceType()->getString(printOptions);
    return Ctx.AllocateCopy(key);
  }

private:
  template <typename... ArgTypes>
  InFlightDiagnostic
  diagnose(DiagnosticEngine &Diags, const char *LocPtr, Diag<ArgTypes...> ID,
           typename detail::PassArgument<ArgTypes>::type... Args) const {
    auto Loc = SourceLoc(llvm::SMLoc::getFromPointer(LocPtr));
    return Diags.diagnose(Loc, ID, std::move(Args)...);
  }
};
} // end anonymous namespace

bool DependencyVerifier::parseExpectations(
    const SourceFile *SF, std::vector<Expectation> &Expectations) {
  const auto MaybeBufferID = SF->getBufferID();
  if (!MaybeBufferID) {
    llvm::errs() << "source file has no buffer: " << SF->getFilename();
    return true;
  }

  const auto BufferID = MaybeBufferID.value();
  const CharSourceRange EntireRange = SM.getRangeForBuffer(BufferID);
  const StringRef InputFile = SM.extractText(EntireRange);

  for (size_t Match = InputFile.find("expected-"); Match != StringRef::npos;
       Match = InputFile.find("expected-", Match + 1)) {
    StringRef MatchStart = InputFile.substr(Match);
    const char *DiagnosticLoc = MatchStart.data();

    Expectation::Kind ExpectedKind;
    {
#define MATRIX_ENTRY(EXPECTATION_SELECTOR, KIND)                               \
  .StartsWith(EXPECTATION_SELECTOR, [&]() {                                    \
    ExpectedKind = Expectation::Kind::KIND;                                    \
    MatchStart = MatchStart.substr(strlen(EXPECTATION_SELECTOR));              \
  })

      // clang-format off
          llvm::StringSwitch<llvm::function_ref<void(void)>>{MatchStart}
            EXPECTATION_MATRIX
            .Default([]() {})();
      // clang-format on
#undef MATRIX_ENTRY
    }

    // Skip any whitespace before the {{.
    MatchStart = MatchStart.substr(MatchStart.find_first_not_of(" \t"));
    auto &diags = SF->getASTContext().Diags;

    const size_t TextStartIdx = MatchStart.find("{{");
    if (TextStartIdx == StringRef::npos) {
      diagnose(diags, MatchStart.data(),
               diag::expectation_missing_opening_braces);
      continue;
    }

    const size_t End = MatchStart.find("}}");
    if (End == StringRef::npos) {
      diagnose(diags, MatchStart.data(),
               diag::expectation_missing_closing_braces);
      continue;
    }

    // Check if the next expectation should be in the same line.
    StringRef AfterEnd = MatchStart.substr(End + strlen("}}"));
    AfterEnd = AfterEnd.substr(AfterEnd.find_first_not_of(" \t"));
    const char *ExpectedEnd = AfterEnd.data();

    // Strip out the trailing whitespace.
    while (isspace(ExpectedEnd[-1]))
      --ExpectedEnd;

    Expectations.emplace_back(DiagnosticLoc, ExpectedEnd, ExpectedKind,
                              MatchStart.slice(2, End));
  }
  return false;
}

bool DependencyVerifier::constructObligations(const SourceFile *SF,
                                              ObligationMap &Obligations) {
  auto &Ctx = SF->getASTContext();
  Ctx.evaluator.enumerateReferencesInFile(SF, [&](const auto &reference) {
    using NodeKind = evaluator::DependencyCollector::Reference::Kind;
    switch (reference.kind) {
    case NodeKind::Empty:
    case NodeKind::Tombstone:
      llvm_unreachable("Cannot enumerate dead dependency!");

    case NodeKind::PotentialMember: {
      auto key = copyQualifiedTypeName(Ctx, reference.subject->getSelfNominalTypeDecl());
      Obligations.insert({Obligation::Key::forPotentialMember(key),
                          {"", Expectation::Kind::PotentialMember}});
    } break;
    case NodeKind::UsedMember: {
      auto demContext = copyQualifiedTypeName(Ctx, reference.subject->getSelfNominalTypeDecl());
      auto name = reference.name.userFacingName();
      auto key = Ctx.AllocateCopy((demContext + "." + name).str());
      Obligations.insert(
          {Obligation::Key::forMember(key), {key, Expectation::Kind::Member}});
    } break;
    case NodeKind::Dynamic: {
      auto key = Ctx.AllocateCopy(reference.name.userFacingName());
      Obligations.insert({Obligation::Key::forDynamicMember(key),
                          {"", Expectation::Kind::DynamicMember}});
    } break;
    case NodeKind::TopLevel: {
      auto key = Ctx.AllocateCopy(reference.name.userFacingName());
      Obligations.insert({Obligation::Key::forProvides(key),
                          {key, Expectation::Kind::Provides}});
    } break;
    }
  });

  return false;
}

bool DependencyVerifier::verifyObligations(
    const SourceFile *SF, const std::vector<Expectation> &ExpectedDependencies,
    ObligationMap &OM, llvm::StringMap<Expectation> &NegativeExpectations) {
  auto &diags = SF->getASTContext().Diags;
  for (auto &expectation : ExpectedDependencies) {
    if (expectation.Info.Kind == Expectation::Kind::Negative) {
      // We'll verify negative expectations separately.
      NegativeExpectations.insert({expectation.MessageRange, expectation});
      continue;
    }

    matchExpectationOrFail(
        OM, expectation,
        [&](Obligation &O) {
          switch (expectation.Info.Kind) {
          case Expectation::Kind::Negative:
            llvm_unreachable("Should have been handled above!");
          case Expectation::Kind::Member:
            return O.fulfill();
          case Expectation::Kind::PotentialMember:
            assert(O.getName().empty());
            return O.fulfill();
          case Expectation::Kind::Provides:
          case Expectation::Kind::DynamicMember:
            return O.fulfill();
          }

          llvm_unreachable("Unhandled expectation kind!");
        },
        [&](const Expectation &e) {
          diagnose(diags, e.MessageRange.begin(),
                   diag::missing_member_dependency,
                   static_cast<uint8_t>(expectation.Info.Kind), e.MessageRange);
        });
  }

  return false;
}

bool DependencyVerifier::verifyNegativeExpectations(
    const SourceFile *SF, ObligationMap &Obligations,
    NegativeExpectationMap &NegativeExpectations) {
  forEachOwedObligation(Obligations, [&](StringRef key, Obligation &p) {
    auto entry = NegativeExpectations.find(key);
    if (entry == NegativeExpectations.end()) {
      return;
    }

    auto &expectation = entry->second;
    diagnose(SF->getASTContext().Diags, expectation.MessageRange.begin(),
             diag::negative_expectation_violated, expectation.MessageRange);
    p.fail();
  });
  return false;
}

bool DependencyVerifier::diagnoseUnfulfilledObligations(
    const SourceFile *SF, ObligationMap &Obligations) {
  CharSourceRange EntireRange = SM.getRangeForBuffer(*SF->getBufferID());
  StringRef InputFile = SM.extractText(EntireRange);
  auto &diags = SF->getASTContext().Diags;
  auto &Ctx = SF->getASTContext();
  forEachOwedObligation(Obligations, [&](StringRef key, Obligation &p) {
    // HACK: Diagnosing the end of the buffer will print a carat pointing
    // at the file path, but not print any of the buffer's contents, which
    // might be misleading.
    auto Loc = SourceLoc(llvm::SMLoc::getFromPointer(InputFile.end()));
    switch (p.getKind()) {
    case Expectation::Kind::Negative:
      llvm_unreachable("Obligations may not be negative; only Expectations!");
    case Expectation::Kind::Member:
    case Expectation::Kind::DynamicMember:
    case Expectation::Kind::PotentialMember:
      diags
          .diagnose(Loc, diag::unexpected_dependency,
                    static_cast<uint8_t>(p.getKind()), key)
          .fixItInsert(Loc, p.renderAsFixit(Ctx));
      break;
    case Expectation::Kind::Provides:
      diags.diagnose(Loc, diag::unexpected_provided_entity, p.getName())
          .fixItInsert(Loc, p.renderAsFixit(Ctx));
      break;
    }
  });

  return false;
}

bool DependencyVerifier::verifyFile(const SourceFile *SF) {
  std::vector<Expectation> ExpectedDependencies;
  if (parseExpectations(SF, ExpectedDependencies)) {
    return true;
  }

  ObligationMap Obligations;
  if (constructObligations(SF, Obligations)) {
    return true;
  }

  NegativeExpectationMap Negatives;
  if (verifyObligations(SF, ExpectedDependencies, Obligations, Negatives)) {
    return true;
  }

  if (verifyNegativeExpectations(SF, Obligations, Negatives)) {
    return true;
  }

  if (diagnoseUnfulfilledObligations(SF, Obligations)) {
    return true;
  }

  return SF->getASTContext().Diags.hadAnyError();
}

//===----------------------------------------------------------------------===//
// MARK: Main entrypoints
//===----------------------------------------------------------------------===//

bool swift::verifyDependencies(SourceManager &SM, ArrayRef<FileUnit *> SFs) {
  bool HadError = false;
  DependencyVerifier Verifier{SM};
  for (const auto *FU : SFs) {
    if (const auto *SF = dyn_cast<SourceFile>(FU))
      HadError |= Verifier.verifyFile(SF);
  }
  return HadError;
}

bool swift::verifyDependencies(SourceManager &SM, ArrayRef<SourceFile *> SFs) {
  bool HadError = false;
  DependencyVerifier Verifier{SM};
  for (const auto *SF : SFs) {
    HadError |= Verifier.verifyFile(SF);
  }
  return HadError;
}

#undef EXPECTATION_MATRIX
