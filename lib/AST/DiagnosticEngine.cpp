//===--- DiagnosticEngine.cpp - Diagnostic Display Engine -----------------===//
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
//
//  This file defines the DiagnosticEngine class, which manages any diagnostics
//  emitted by Swift.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticSuppression.h"
#include "swift/AST/DiagnosticsCommon.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Module.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/PrintOptions.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/TypeRepr.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Config.h"
#include "swift/Localization/LocalizationFormat.h"
#include "swift/Parse/Lexer.h" // bad dependency
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;

namespace {
enum class DiagnosticOptions {
  /// No options.
  none,

  /// The location of this diagnostic points to the beginning of the first
  /// token that the parser considers invalid.  If this token is located at the
  /// beginning of the line, then the location is adjusted to point to the end
  /// of the previous token.
  ///
  /// This behavior improves experience for "expected token X" diagnostics.
  PointsToFirstBadToken,

  /// After a fatal error subsequent diagnostics are suppressed.
  Fatal,

  /// An API or ABI breakage diagnostic emitted by the API digester.
  APIDigesterBreakage,

  /// A deprecation warning or error.
  Deprecation,

  /// A diagnostic warning about an unused element.
  NoUsage,
};
struct StoredDiagnosticInfo {
  DiagnosticKind kind : 2;
  bool pointsToFirstBadToken : 1;
  bool isFatal : 1;
  bool isAPIDigesterBreakage : 1;
  bool isDeprecation : 1;
  bool isNoUsage : 1;

  constexpr StoredDiagnosticInfo(DiagnosticKind k, bool firstBadToken,
                                 bool fatal, bool isAPIDigesterBreakage,
                                 bool deprecation, bool noUsage)
      : kind(k), pointsToFirstBadToken(firstBadToken), isFatal(fatal),
        isAPIDigesterBreakage(isAPIDigesterBreakage), isDeprecation(deprecation),
        isNoUsage(noUsage) {}
  constexpr StoredDiagnosticInfo(DiagnosticKind k, DiagnosticOptions opts)
      : StoredDiagnosticInfo(k,
                             opts == DiagnosticOptions::PointsToFirstBadToken,
                             opts == DiagnosticOptions::Fatal,
                             opts == DiagnosticOptions::APIDigesterBreakage,
                             opts == DiagnosticOptions::Deprecation,
                             opts == DiagnosticOptions::NoUsage) {}
};

// Reproduce the DiagIDs, as we want both the size and access to the raw ids
// themselves.
enum LocalDiagID : uint32_t {
#define DIAG(KIND, ID, Options, Text, Signature) ID,
#include "swift/AST/DiagnosticsAll.def"
  NumDiags
};
} // end anonymous namespace

// TODO: categorization
static const constexpr StoredDiagnosticInfo storedDiagnosticInfos[] = {
#define ERROR(ID, Options, Text, Signature)                                    \
  StoredDiagnosticInfo(DiagnosticKind::Error, DiagnosticOptions::Options),
#define WARNING(ID, Options, Text, Signature)                                  \
  StoredDiagnosticInfo(DiagnosticKind::Warning, DiagnosticOptions::Options),
#define NOTE(ID, Options, Text, Signature)                                     \
  StoredDiagnosticInfo(DiagnosticKind::Note, DiagnosticOptions::Options),
#define REMARK(ID, Options, Text, Signature)                                   \
  StoredDiagnosticInfo(DiagnosticKind::Remark, DiagnosticOptions::Options),
#include "swift/AST/DiagnosticsAll.def"
};
static_assert(sizeof(storedDiagnosticInfos) / sizeof(StoredDiagnosticInfo) ==
                  LocalDiagID::NumDiags,
              "array size mismatch");

static constexpr const char * const diagnosticStrings[] = {
#define DIAG(KIND, ID, Options, Text, Signature) Text,
#include "swift/AST/DiagnosticsAll.def"
    "<not a diagnostic>",
};

static constexpr const char *const debugDiagnosticStrings[] = {
#define DIAG(KIND, ID, Options, Text, Signature) Text " [" #ID "]",
#include "swift/AST/DiagnosticsAll.def"
    "<not a diagnostic>",
};

static constexpr const char *const diagnosticIDStrings[] = {
#define DIAG(KIND, ID, Options, Text, Signature) #ID,
#include "swift/AST/DiagnosticsAll.def"
    "<not a diagnostic>",
};

static constexpr const char *const fixItStrings[] = {
#define DIAG(KIND, ID, Options, Text, Signature)
#define FIXIT(ID, Text, Signature) Text,
#include "swift/AST/DiagnosticsAll.def"
    "<not a fix-it>",
};

#define EDUCATIONAL_NOTES(DIAG, ...)                                           \
  static constexpr const char *const DIAG##_educationalNotes[] = {__VA_ARGS__, \
                                                                  nullptr};
#include "swift/AST/EducationalNotes.def"

// NOTE: sadly, while GCC and Clang support array designators in C++, they are
// not part of the standard at the moment, so Visual C++ doesn't support them.
// This construct allows us to provide a constexpr array initialized to empty
// values except in the cases that EducationalNotes.def are provided, similar to
// what the C array would have looked like.
template<int N>
struct EducationalNotes {
  constexpr EducationalNotes() : value() {
    for (auto i = 0; i < N; ++i) value[i] = {};
#define EDUCATIONAL_NOTES(DIAG, ...)                                           \
  value[LocalDiagID::DIAG] = DIAG##_educationalNotes;
#include "swift/AST/EducationalNotes.def"
  }
  const char *const *value[N];
};

static constexpr EducationalNotes<LocalDiagID::NumDiags> _EducationalNotes = EducationalNotes<LocalDiagID::NumDiags>();
static constexpr auto educationalNotes = _EducationalNotes.value;

DiagnosticState::DiagnosticState() {
  // Initialize our ignored diagnostics to default
  ignoredDiagnostics.resize(LocalDiagID::NumDiags);
}

static CharSourceRange toCharSourceRange(SourceManager &SM, SourceRange SR) {
  return CharSourceRange(SM, SR.Start, Lexer::getLocForEndOfToken(SM, SR.End));
}

static CharSourceRange toCharSourceRange(SourceManager &SM, SourceLoc Start,
                                         SourceLoc End) {
  return CharSourceRange(SM, Start, End);
}

/// Extract a character at \p Loc. If \p Loc is the end of the buffer,
/// return '\f'.
static char extractCharAfter(SourceManager &SM, SourceLoc Loc) {
  auto chars = SM.extractText({Loc, 1});
  return chars.empty() ? '\f' : chars[0];
}

/// Extract a character immediately before \p Loc. If \p Loc is the
/// start of the buffer, return '\f'.
static char extractCharBefore(SourceManager &SM, SourceLoc Loc) {
  // We have to be careful not to go off the front of the buffer.
  auto bufferID = SM.findBufferContainingLoc(Loc);
  auto bufferRange = SM.getRangeForBuffer(bufferID);
  if (bufferRange.getStart() == Loc)
    return '\f';
  auto chars = SM.extractText({Loc.getAdvancedLoc(-1), 1}, bufferID);
  assert(!chars.empty() && "Couldn't extractText with valid range");
  return chars[0];
}

InFlightDiagnostic &InFlightDiagnostic::highlight(SourceRange R) {
  assert(IsActive && "Cannot modify an inactive diagnostic");
  if (Engine && R.isValid())
    Engine->getActiveDiagnostic()
        .addRange(toCharSourceRange(Engine->SourceMgr, R));
  return *this;
}

InFlightDiagnostic &InFlightDiagnostic::highlightChars(SourceLoc Start,
                                                       SourceLoc End) {
  assert(IsActive && "Cannot modify an inactive diagnostic");
  if (Engine && Start.isValid())
    Engine->getActiveDiagnostic()
        .addRange(toCharSourceRange(Engine->SourceMgr, Start, End));
  return *this;
}

/// Add an insertion fix-it to the currently-active diagnostic.  The
/// text is inserted immediately *after* the token specified.
///
InFlightDiagnostic &
InFlightDiagnostic::fixItInsertAfter(SourceLoc L, StringRef FormatString,
                                     ArrayRef<DiagnosticArgument> Args) {
  L = Lexer::getLocForEndOfToken(Engine->SourceMgr, L);
  return fixItInsert(L, FormatString, Args);
}

/// Add a token-based removal fix-it to the currently-active
/// diagnostic.
InFlightDiagnostic &InFlightDiagnostic::fixItRemove(SourceRange R) {
  assert(IsActive && "Cannot modify an inactive diagnostic");
  if (R.isInvalid() || !Engine) return *this;

  // Convert from a token range to a CharSourceRange, which points to the end of
  // the token we want to remove.
  auto &SM = Engine->SourceMgr;
  auto charRange = toCharSourceRange(SM, R);

  // If we're removing something (e.g. a keyword), do a bit of extra work to
  // make sure that we leave the code in a good place, without extraneous white
  // space around its hole.  Specifically, check to see there is whitespace
  // before and after the end of range.  If so, nuke the space afterward to keep
  // things consistent.
  if (extractCharAfter(SM, charRange.getEnd()) == ' ' &&
      isspace(extractCharBefore(SM, charRange.getStart()))) {
    charRange = CharSourceRange(charRange.getStart(),
                                charRange.getByteLength()+1);
  }
  Engine->getActiveDiagnostic().addFixIt(Diagnostic::FixIt(charRange, {}, {}));
  return *this;
}

InFlightDiagnostic &
InFlightDiagnostic::fixItReplace(SourceRange R, StringRef FormatString,
                                 ArrayRef<DiagnosticArgument> Args) {
  auto &SM = Engine->SourceMgr;
  auto charRange = toCharSourceRange(SM, R);

  Engine->getActiveDiagnostic().addFixIt(
      Diagnostic::FixIt(charRange, FormatString, Args));
  return *this;
}

InFlightDiagnostic &InFlightDiagnostic::fixItReplace(SourceRange R,
                                                     StringRef Str) {
  if (Str.empty())
    return fixItRemove(R);

  assert(IsActive && "Cannot modify an inactive diagnostic");
  if (R.isInvalid() || !Engine) return *this;

  auto &SM = Engine->SourceMgr;
  auto charRange = toCharSourceRange(SM, R);

  // If we're replacing with something that wants spaces around it, do a bit of
  // extra work so that we don't suggest extra spaces.
  // FIXME: This could probably be applied to structured fix-its as well.
  if (Str.back() == ' ') {
    if (isspace(extractCharAfter(SM, charRange.getEnd())))
      Str = Str.drop_back();
  }
  if (!Str.empty() && Str.front() == ' ') {
    if (isspace(extractCharBefore(SM, charRange.getStart())))
      Str = Str.drop_front();
  }
  
  return fixItReplace(R, "%0", {Str});
}

InFlightDiagnostic &
InFlightDiagnostic::fixItReplaceChars(SourceLoc Start, SourceLoc End,
                                      StringRef FormatString,
                                      ArrayRef<DiagnosticArgument> Args) {
  assert(IsActive && "Cannot modify an inactive diagnostic");
  if (Engine && Start.isValid())
    Engine->getActiveDiagnostic().addFixIt(
        Diagnostic::FixIt(toCharSourceRange(Engine->SourceMgr, Start, End),
                          FormatString, Args));
  return *this;
}

InFlightDiagnostic &InFlightDiagnostic::fixItExchange(SourceRange R1,
                                                      SourceRange R2) {
  assert(IsActive && "Cannot modify an inactive diagnostic");

  auto &SM = Engine->SourceMgr;
  // Convert from a token range to a CharSourceRange
  auto charRange1 = toCharSourceRange(SM, R1);
  auto charRange2 = toCharSourceRange(SM, R2);
  // Extract source text.
  auto text1 = SM.extractText(charRange1);
  auto text2 = SM.extractText(charRange2);

  Engine->getActiveDiagnostic().addFixIt(
      Diagnostic::FixIt(charRange1, "%0", {text2}));
  Engine->getActiveDiagnostic().addFixIt(
      Diagnostic::FixIt(charRange2, "%0", {text1}));
  return *this;
}

InFlightDiagnostic &
InFlightDiagnostic::limitBehavior(DiagnosticBehavior limit) {
  Engine->getActiveDiagnostic().setBehaviorLimit(limit);
  return *this;
}

InFlightDiagnostic &
InFlightDiagnostic::warnUntilSwiftVersion(unsigned majorVersion) {
  if (!Engine->languageVersion.isVersionAtLeast(majorVersion)) {
    limitBehavior(DiagnosticBehavior::Warning)
      .wrapIn(diag::error_in_future_swift_version, majorVersion);
  }

  return *this;
}

InFlightDiagnostic &
InFlightDiagnostic::warnInSwiftInterface(const DeclContext *context) {
  auto sourceFile = context->getParentSourceFile();
  if (sourceFile && sourceFile->Kind == SourceFileKind::Interface) {
    return limitBehavior(DiagnosticBehavior::Warning);
  }

  return *this;
}

InFlightDiagnostic &
InFlightDiagnostic::wrapIn(const Diagnostic &wrapper) {
  // Save current active diagnostic into WrappedDiagnostics, ignoring state
  // so we don't get a None return or influence future diagnostics.
  DiagnosticState tempState;
  Engine->state.swap(tempState);
  llvm::SaveAndRestore<DiagnosticBehavior>
      limit(Engine->getActiveDiagnostic().BehaviorLimit,
            DiagnosticBehavior::Unspecified);

  Engine->WrappedDiagnostics.push_back(
       *Engine->diagnosticInfoForDiagnostic(Engine->getActiveDiagnostic()));

  Engine->state.swap(tempState);

  auto &wrapped = Engine->WrappedDiagnostics.back();

  // Copy and update its arg list.
  Engine->WrappedDiagnosticArgs.emplace_back(wrapped.FormatArgs);
  wrapped.FormatArgs = Engine->WrappedDiagnosticArgs.back();

  // Overwrite the ID and argument with those from the wrapper.
  Engine->getActiveDiagnostic().ID = wrapper.ID;
  Engine->getActiveDiagnostic().Args = wrapper.Args;

  // Set the argument to the diagnostic being wrapped.
  assert(wrapper.getArgs().front().getKind() == DiagnosticArgumentKind::Diagnostic);
  Engine->getActiveDiagnostic().Args.front() = &wrapped;

  return *this;
}

void InFlightDiagnostic::flush() {
  if (!IsActive)
    return;
  
  IsActive = false;
  if (Engine)
    Engine->flushActiveDiagnostic();
}

void Diagnostic::addChildNote(Diagnostic &&D) {
  assert(storedDiagnosticInfos[(unsigned)D.ID].kind == DiagnosticKind::Note &&
         "Only notes can have a parent.");
  assert(storedDiagnosticInfos[(unsigned)ID].kind != DiagnosticKind::Note &&
         "Notes can't have children.");
  ChildNotes.push_back(std::move(D));
}

bool DiagnosticEngine::isDiagnosticPointsToFirstBadToken(DiagID ID) const {
  return storedDiagnosticInfos[(unsigned) ID].pointsToFirstBadToken;
}

bool DiagnosticEngine::isAPIDigesterBreakageDiagnostic(DiagID ID) const {
  return storedDiagnosticInfos[(unsigned)ID].isAPIDigesterBreakage;
}

bool DiagnosticEngine::isDeprecationDiagnostic(DiagID ID) const {
  return storedDiagnosticInfos[(unsigned)ID].isDeprecation;
}

bool DiagnosticEngine::isNoUsageDiagnostic(DiagID ID) const {
  return storedDiagnosticInfos[(unsigned)ID].isNoUsage;
}

bool DiagnosticEngine::finishProcessing() {
  bool hadError = false;
  for (auto &Consumer : Consumers) {
    hadError |= Consumer->finishProcessing();
  }
  return hadError;
}

/// Skip forward to one of the given delimiters.
///
/// \param Text The text to search through, which will be updated to point
/// just after the delimiter.
///
/// \param Delim The first character delimiter to search for.
///
/// \param FoundDelim On return, true if the delimiter was found, or false
/// if the end of the string was reached.
///
/// \returns The string leading up to the delimiter, or the empty string
/// if no delimiter is found.
static StringRef 
skipToDelimiter(StringRef &Text, char Delim, bool *FoundDelim = nullptr) {
  unsigned Depth = 0;
  if (FoundDelim)
    *FoundDelim = false;

  unsigned I = 0;
  for (unsigned N = Text.size(); I != N; ++I) {
    if (Text[I] == '{') {
      ++Depth;
      continue;
    }
    if (Depth > 0) {
      if (Text[I] == '}')
        --Depth;
      continue;
    }
    
    if (Text[I] == Delim) {
      if (FoundDelim)
        *FoundDelim = true;
      break;
    }
  }

  assert(Depth == 0 && "Unbalanced {} set in diagnostic text");
  StringRef Result = Text.substr(0, I);
  Text = Text.substr(I + 1);
  return Result;
}

/// Handle the integer 'select' modifier.  This is used like this:
/// %select{foo|bar|baz}2.  This means that the integer argument "%2" has a
/// value from 0-2.  If the value is 0, the diagnostic prints 'foo'.
/// If the value is 1, it prints 'bar'.  If it has the value 2, it prints 'baz'.
/// This is very useful for certain classes of variant diagnostics.
static void formatSelectionArgument(StringRef ModifierArguments,
                                    ArrayRef<DiagnosticArgument> Args,
                                    unsigned SelectedIndex,
                                    DiagnosticFormatOptions FormatOpts,
                                    llvm::raw_ostream &Out) {
  bool foundPipe = false;
  do {
    assert((!ModifierArguments.empty() || foundPipe) &&
           "Index beyond bounds in %select modifier");
    StringRef Text = skipToDelimiter(ModifierArguments, '|', &foundPipe);
    if (SelectedIndex == 0) {
      DiagnosticEngine::formatDiagnosticText(Out, Text, Args, FormatOpts);
      break;
    }
    --SelectedIndex;
  } while (true);
  
}

static bool isInterestingTypealias(Type type) {
  // Dig out the typealias declaration, if there is one.
  TypeAliasDecl *aliasDecl = nullptr;
  if (auto aliasTy = dyn_cast<TypeAliasType>(type.getPointer()))
    aliasDecl = aliasTy->getDecl();
  else
    return false;

  if (type->isVoid())
    return false;

  // The 'Swift.AnyObject' typealias is not 'interesting'.
  if (aliasDecl->getName() ==
        aliasDecl->getASTContext().getIdentifier("AnyObject") &&
      (aliasDecl->getParentModule()->isStdlibModule() ||
       aliasDecl->getParentModule()->isBuiltinModule())) {
    return false;
  }

  // Compatibility aliases are only interesting insofar as their underlying
  // types are interesting.
  if (aliasDecl->isCompatibilityAlias()) {
    auto underlyingTy = aliasDecl->getUnderlyingType();
    return isInterestingTypealias(underlyingTy);
  }

  // Builtin types are never interesting typealiases.
  if (type->is<BuiltinType>()) return false;

  return true;
}

/// Walks the type recursively desugaring  types to display, but skipping
/// `GenericTypeParamType` because we would lose association with its original
/// declaration and end up presenting the parameter in τ_0_0 format on
/// diagnostic.
static Type getAkaTypeForDisplay(Type type) {
  return type.transform([](Type visitTy) -> Type {
    if (isa<SugarType>(visitTy.getPointer()) &&
        !isa<GenericTypeParamType>(visitTy.getPointer()))
      return getAkaTypeForDisplay(visitTy->getDesugaredType());
    return visitTy;
  });
}

/// Decide whether to show the desugared type or not.  We filter out some
/// cases to avoid too much noise.
static bool shouldShowAKA(Type type, StringRef typeName) {
  // Canonical types are already desugared.
  if (type->isCanonical())
    return false;

  // Only show 'aka' if there's a typealias involved; other kinds of sugar
  // are easy enough for people to read on their own.
  if (!type.findIf(isInterestingTypealias))
    return false;

  // If they are textually the same, don't show them.  This can happen when
  // they are actually different types, because they exist in different scopes
  // (e.g. everyone names their type parameters 'T').
  if (typeName == getAkaTypeForDisplay(type).getString())
    return false;

  return true;
}

/// If a type is part of an argument list which includes another, distinct type
/// with the same string representation, it should be qualified during
/// formatting.
static bool typeSpellingIsAmbiguous(Type type,
                                    ArrayRef<DiagnosticArgument> Args,
                                    PrintOptions &PO) {
  for (auto arg : Args) {
    if (arg.getKind() == DiagnosticArgumentKind::Type) {
      auto argType = arg.getAsType();
      if (argType && argType->getWithoutParens().getPointer() != type.getPointer() &&
          argType->getWithoutParens().getString(PO) == type.getString(PO)) {
        // Currently, existential types are spelled the same way
        // as protocols and compositions. We can remove this once
        // existenials are printed with 'any'.
        if (type->is<ExistentialType>() || argType->isExistentialType()) {
          auto constraint = type;
          if (auto existential = type->getAs<ExistentialType>())
            constraint = existential->getConstraintType();

          auto argConstraint = argType;
          if (auto existential = argType->getAs<ExistentialType>())
            argConstraint = existential->getConstraintType();

          if (constraint.getPointer() != argConstraint.getPointer())
            return true;

          continue;
        }

        return true;
      }
    }
  }
  return false;
}

void swift::printClangDeclName(const clang::NamedDecl *ND,
                               llvm::raw_ostream &os) {
  ND->getNameForDiagnostic(os, ND->getASTContext().getPrintingPolicy(), false);
}

/// Format a single diagnostic argument and write it to the given
/// stream.
static void formatDiagnosticArgument(StringRef Modifier,
                                     StringRef ModifierArguments,
                                     ArrayRef<DiagnosticArgument> Args,
                                     unsigned ArgIndex,
                                     DiagnosticFormatOptions FormatOpts,
                                     llvm::raw_ostream &Out) {
  const DiagnosticArgument &Arg = Args[ArgIndex];
  switch (Arg.getKind()) {
  case DiagnosticArgumentKind::Integer:
    if (Modifier == "select") {
      assert(Arg.getAsInteger() >= 0 && "Negative selection index");
      formatSelectionArgument(ModifierArguments, Args, Arg.getAsInteger(),
                              FormatOpts, Out);
    } else if (Modifier == "s") {
      if (Arg.getAsInteger() != 1)
        Out << 's';
    } else {
      assert(Modifier.empty() && "Improper modifier for integer argument");
      Out << Arg.getAsInteger();
    }
    break;

  case DiagnosticArgumentKind::Unsigned:
    if (Modifier == "select") {
      formatSelectionArgument(ModifierArguments, Args, Arg.getAsUnsigned(),
                              FormatOpts, Out);
    } else if (Modifier == "s") {
      if (Arg.getAsUnsigned() != 1)
        Out << 's';
    } else {
      assert(Modifier.empty() && "Improper modifier for unsigned argument");
      Out << Arg.getAsUnsigned();
    }
    break;

  case DiagnosticArgumentKind::String:
    if (Modifier == "select") {
      formatSelectionArgument(ModifierArguments, Args,
                              Arg.getAsString().empty() ? 0 : 1, FormatOpts,
                              Out);
    } else {
      assert(Modifier.empty() && "Improper modifier for string argument");
      Out << Arg.getAsString();
    }
    break;

  case DiagnosticArgumentKind::Identifier:
    if (Modifier == "select") {
      formatSelectionArgument(ModifierArguments, Args,
                              Arg.getAsIdentifier() ? 1 : 0, FormatOpts,
                              Out);
    } else {
      assert(Modifier.empty() && "Improper modifier for identifier argument");
      Out << FormatOpts.OpeningQuotationMark;
      Arg.getAsIdentifier().printPretty(Out);
      Out << FormatOpts.ClosingQuotationMark;
    }
    break;

  case DiagnosticArgumentKind::ObjCSelector:
    assert(Modifier.empty() && "Improper modifier for selector argument");
    Out << FormatOpts.OpeningQuotationMark << Arg.getAsObjCSelector()
        << FormatOpts.ClosingQuotationMark;
    break;

  case DiagnosticArgumentKind::ValueDecl:
    Out << FormatOpts.OpeningQuotationMark;
    Arg.getAsValueDecl()->getName().printPretty(Out);
    Out << FormatOpts.ClosingQuotationMark;
    break;

  case DiagnosticArgumentKind::FullyQualifiedType:
  case DiagnosticArgumentKind::Type: {
    assert(Modifier.empty() && "Improper modifier for Type argument");
    
    // Strip extraneous parentheses; they add no value.
    Type type;
    bool needsQualification = false;

    // Compute the appropriate print options for this argument.
    auto printOptions = PrintOptions::forDiagnosticArguments();
    if (Arg.getKind() == DiagnosticArgumentKind::Type) {
      type = Arg.getAsType()->getWithoutParens();
      if (type.isNull()) {
        // FIXME: We should never receive a nullptr here, but this is causing
        // crashes (rdar://75740683). Remove once ParenType never contains
        // nullptr as the underlying type.
        Out << "<null>";
        break;
      }
      if (type->getASTContext().TypeCheckerOpts.PrintFullConvention)
        printOptions.PrintFunctionRepresentationAttrs =
            PrintOptions::FunctionRepresentationMode::Full;
      needsQualification = typeSpellingIsAmbiguous(type, Args, printOptions);
    } else {
      assert(Arg.getKind() == DiagnosticArgumentKind::FullyQualifiedType);
      type = Arg.getAsFullyQualifiedType().getType()->getWithoutParens();
      if (type.isNull()) {
        // FIXME: We should never receive a nullptr here, but this is causing
        // crashes (rdar://75740683). Remove once ParenType never contains
        // nullptr as the underlying type.
        Out << "<null>";
        break;
      }
      if (type->getASTContext().TypeCheckerOpts.PrintFullConvention)
        printOptions.PrintFunctionRepresentationAttrs =
            PrintOptions::FunctionRepresentationMode::Full;
      needsQualification = true;
    }

    // If a type has an unresolved type, print it with syntax sugar removed for
    // clarity. For example, print `Array<_>` instead of `[_]`.
    if (type->hasUnresolvedType()) {
      type = type->getWithoutSyntaxSugar();
    }

    if (needsQualification &&
        isa<OpaqueTypeArchetypeType>(type.getPointer()) &&
        cast<ArchetypeType>(type.getPointer())->isRoot()) {
      auto opaqueTypeDecl = type->castTo<OpaqueTypeArchetypeType>()->getDecl();

      llvm::SmallString<256> NamingDeclText;
      llvm::raw_svector_ostream OutNaming(NamingDeclText);
      auto namingDecl = opaqueTypeDecl->getNamingDecl();
      if (namingDecl->getDeclContext()->isTypeContext()) {
        auto selfTy = namingDecl->getDeclContext()->getSelfInterfaceType();
        selfTy->print(OutNaming);
        OutNaming << '.';
      }
      namingDecl->getName().printPretty(OutNaming);

      auto descriptiveKind = opaqueTypeDecl->getDescriptiveKind();

      Out << llvm::format(FormatOpts.OpaqueResultFormatString.c_str(),
                          type->getString(printOptions).c_str(),
                          Decl::getDescriptiveKindName(descriptiveKind).data(),
                          NamingDeclText.c_str());

    } else {
      printOptions.FullyQualifiedTypes = needsQualification;
      std::string typeName = type->getString(printOptions);

      if (shouldShowAKA(type, typeName)) {
        llvm::SmallString<256> AkaText;
        llvm::raw_svector_ostream OutAka(AkaText);

        getAkaTypeForDisplay(type)->print(OutAka, printOptions);
        Out << llvm::format(FormatOpts.AKAFormatString.c_str(),
                            typeName.c_str(), AkaText.c_str());
      } else {
        Out << FormatOpts.OpeningQuotationMark << typeName
            << FormatOpts.ClosingQuotationMark;
      }
    }
    break;
  }

  case DiagnosticArgumentKind::TypeRepr:
    assert(Modifier.empty() && "Improper modifier for TypeRepr argument");
    assert(Arg.getAsTypeRepr() && "TypeRepr argument is null");
    Out << FormatOpts.OpeningQuotationMark << Arg.getAsTypeRepr()
        << FormatOpts.ClosingQuotationMark;
    break;

  case DiagnosticArgumentKind::DescriptivePatternKind:
    assert(Modifier.empty() &&
           "Improper modifier for DescriptivePatternKind argument");
    Out << Pattern::getDescriptivePatternKindName(
        Arg.getAsDescriptivePatternKind());
    break;

  case DiagnosticArgumentKind::SelfAccessKind:
    if (Modifier == "select") {
      formatSelectionArgument(ModifierArguments, Args,
                              unsigned(Arg.getAsSelfAccessKind()),
                              FormatOpts, Out);
    } else {
      assert(Modifier.empty() &&
             "Improper modifier for SelfAccessKind argument");
      Out << Arg.getAsSelfAccessKind();
    }
    break;

  case DiagnosticArgumentKind::ReferenceOwnership:
    if (Modifier == "select") {
      formatSelectionArgument(ModifierArguments, Args,
                              unsigned(Arg.getAsReferenceOwnership()),
                              FormatOpts, Out);
    } else {
      assert(Modifier.empty() &&
             "Improper modifier for ReferenceOwnership argument");
      Out << Arg.getAsReferenceOwnership();
    }
    break;

  case DiagnosticArgumentKind::StaticSpellingKind:
    if (Modifier == "select") {
      formatSelectionArgument(ModifierArguments, Args,
                              unsigned(Arg.getAsStaticSpellingKind()),
                              FormatOpts, Out);
    } else {
      assert(Modifier.empty() &&
             "Improper modifier for StaticSpellingKind argument");
      Out << Arg.getAsStaticSpellingKind();
    }
    break;

  case DiagnosticArgumentKind::DescriptiveDeclKind:
    assert(Modifier.empty() &&
           "Improper modifier for DescriptiveDeclKind argument");
    Out << Decl::getDescriptiveKindName(Arg.getAsDescriptiveDeclKind());
    break;

  case DiagnosticArgumentKind::DescriptiveStmtKind:
    assert(Modifier.empty() && "Improper modifier for StmtKind argument");
    Out << Stmt::getDescriptiveKindName(Arg.getAsDescriptiveStmtKind());
    break;

  case DiagnosticArgumentKind::DeclAttribute:
    assert(Modifier.empty() &&
           "Improper modifier for DeclAttribute argument");
    if (Arg.getAsDeclAttribute()->isDeclModifier())
      Out << FormatOpts.OpeningQuotationMark
          << Arg.getAsDeclAttribute()->getAttrName()
          << FormatOpts.ClosingQuotationMark;
    else
      Out << '@' << Arg.getAsDeclAttribute()->getAttrName();
    break;

  case DiagnosticArgumentKind::VersionTuple:
    assert(Modifier.empty() &&
           "Improper modifier for VersionTuple argument");
    Out << Arg.getAsVersionTuple().getAsString();
    break;
  case DiagnosticArgumentKind::LayoutConstraint:
    assert(Modifier.empty() && "Improper modifier for LayoutConstraint argument");
    Out << FormatOpts.OpeningQuotationMark << Arg.getAsLayoutConstraint()
        << FormatOpts.ClosingQuotationMark;
    break;
  case DiagnosticArgumentKind::ActorIsolation:
    assert(Modifier.empty() && "Improper modifier for ActorIsolation argument");
    switch (auto isolation = Arg.getAsActorIsolation()) {
    case ActorIsolation::ActorInstance:
      Out << "actor-isolated";
      break;

    case ActorIsolation::GlobalActor:
    case ActorIsolation::GlobalActorUnsafe: {
      if (isolation.isMainActor()) {
        Out << "main actor-isolated";
      } else {
        Type globalActor = isolation.getGlobalActor();
        Out << "global actor " << FormatOpts.OpeningQuotationMark
          << globalActor.getString()
          << FormatOpts.ClosingQuotationMark << "-isolated";
      }
      break;
    }

    case ActorIsolation::Independent:
    case ActorIsolation::Unspecified:
      Out << "nonisolated";
      break;
    }
    break;

  case DiagnosticArgumentKind::Diagnostic: {
    assert(Modifier.empty() && "Improper modifier for Diagnostic argument");
    auto diagArg = Arg.getAsDiagnostic();
    DiagnosticEngine::formatDiagnosticText(Out, diagArg->FormatString,
                                           diagArg->FormatArgs);
    break;
  }

  case DiagnosticArgumentKind::ClangDecl:
    assert(Modifier.empty() && "Improper modifier for ClangDecl argument");
    Out << FormatOpts.OpeningQuotationMark;
    printClangDeclName(Arg.getAsClangDecl(), Out);
    Out << FormatOpts.ClosingQuotationMark;
    break;
  }
}

/// Format the given diagnostic text and place the result in the given
/// buffer.
void DiagnosticEngine::formatDiagnosticText(
    llvm::raw_ostream &Out, StringRef InText, ArrayRef<DiagnosticArgument> Args,
    DiagnosticFormatOptions FormatOpts) {
  while (!InText.empty()) {
    size_t Percent = InText.find('%');
    if (Percent == StringRef::npos) {
      // Write the rest of the string; we're done.
      Out.write(InText.data(), InText.size());
      break;
    }
    
    // Write the string up to (but not including) the %, then drop that text
    // (including the %).
    Out.write(InText.data(), Percent);
    InText = InText.substr(Percent + 1);
    
    // '%%' -> '%'.
    if (InText[0] == '%') {
      Out.write('%');
      InText = InText.substr(1);
      continue;
    }

    // Parse an optional modifier.
    StringRef Modifier;
    {
      size_t Length = InText.find_if_not(isalpha);
      Modifier = InText.substr(0, Length);
      InText = InText.substr(Length);
    }
    
    if (Modifier == "error") {
      Out << StringRef("<<INTERNAL ERROR: encountered %error in diagnostic text>>");
      continue;
    }

    // Parse the optional argument list for a modifier, which is brace-enclosed.
    StringRef ModifierArguments;
    if (InText[0] == '{') {
      InText = InText.substr(1);
      ModifierArguments = skipToDelimiter(InText, '}');
    }
    
    // Find the digit sequence, and parse it into an argument index.
    size_t Length = InText.find_if_not(isdigit);
    unsigned ArgIndex;      
    bool IndexParseFailed = InText.substr(0, Length).getAsInteger(10, ArgIndex);

    if (IndexParseFailed) {
      Out << StringRef("<<INTERNAL ERROR: unparseable argument index in diagnostic text>>");
      continue;
    }

    InText = InText.substr(Length);

    if (ArgIndex >= Args.size()) {
      Out << StringRef("<<INTERNAL ERROR: out-of-range argument index in diagnostic text>>");
      continue;
    }

    // Convert the argument to a string.
    formatDiagnosticArgument(Modifier, ModifierArguments, Args, ArgIndex,
                             FormatOpts, Out);
  }
}

static DiagnosticKind toDiagnosticKind(DiagnosticBehavior behavior) {
  switch (behavior) {
  case DiagnosticBehavior::Unspecified:
    llvm_unreachable("unspecified behavior");
  case DiagnosticBehavior::Ignore:
    llvm_unreachable("trying to map an ignored diagnostic");
  case DiagnosticBehavior::Error:
  case DiagnosticBehavior::Fatal:
    return DiagnosticKind::Error;
  case DiagnosticBehavior::Note:
    return DiagnosticKind::Note;
  case DiagnosticBehavior::Warning:
    return DiagnosticKind::Warning;
  case DiagnosticBehavior::Remark:
    return DiagnosticKind::Remark;
  }

  llvm_unreachable("Unhandled DiagnosticKind in switch.");
}

static
DiagnosticBehavior toDiagnosticBehavior(DiagnosticKind kind, bool isFatal) {
  switch (kind) {
  case DiagnosticKind::Note:
    return DiagnosticBehavior::Note;
  case DiagnosticKind::Error:
    return isFatal ? DiagnosticBehavior::Fatal : DiagnosticBehavior::Error;
  case DiagnosticKind::Warning:
    return DiagnosticBehavior::Warning;
  case DiagnosticKind::Remark:
    return DiagnosticBehavior::Remark;
  }
  llvm_unreachable("Unhandled DiagnosticKind in switch.");
}

// A special option only for compiler writers that causes Diagnostics to assert
// when a failure diagnostic is emitted. Intended for use in the debugger.
llvm::cl::opt<bool> AssertOnError("swift-diagnostics-assert-on-error",
                                  llvm::cl::init(false));
// A special option only for compiler writers that causes Diagnostics to assert
// when a warning diagnostic is emitted. Intended for use in the debugger.
llvm::cl::opt<bool> AssertOnWarning("swift-diagnostics-assert-on-warning",
                                    llvm::cl::init(false));

DiagnosticBehavior DiagnosticState::determineBehavior(const Diagnostic &diag) {
  // We determine how to handle a diagnostic based on the following rules
  //   1) Map the diagnostic to its "intended" behavior, applying the behavior
  //      limit for this particular emission
  //   2) If current state dictates a certain behavior, follow that
  //   3) If the user ignored this specific diagnostic, follow that
  //   4) If the user substituted a different behavior for this behavior, apply
  //      that change
  //   5) Update current state for use during the next diagnostic

  //   1) Map the diagnostic to its "intended" behavior, applying the behavior
  //      limit for this particular emission
  auto diagInfo = storedDiagnosticInfos[(unsigned)diag.getID()];
  DiagnosticBehavior lvl =
      std::max(toDiagnosticBehavior(diagInfo.kind, diagInfo.isFatal),
               diag.getBehaviorLimit());
  assert(lvl != DiagnosticBehavior::Unspecified);

  //   2) If current state dictates a certain behavior, follow that

  // Notes relating to ignored diagnostics should also be ignored
  if (previousBehavior == DiagnosticBehavior::Ignore
      && lvl == DiagnosticBehavior::Note)
    lvl = DiagnosticBehavior::Ignore;

  // Suppress diagnostics when in a fatal state, except for follow-on notes
  if (fatalErrorOccurred)
    if (!showDiagnosticsAfterFatalError && lvl != DiagnosticBehavior::Note)
      lvl = DiagnosticBehavior::Ignore;

  //   3) If the user ignored this specific diagnostic, follow that
  if (ignoredDiagnostics[(unsigned)diag.getID()])
    lvl = DiagnosticBehavior::Ignore;

  //   4) If the user substituted a different behavior for this behavior, apply
  //      that change
  if (lvl == DiagnosticBehavior::Warning) {
    if (warningsAsErrors)
      lvl = DiagnosticBehavior::Error;
    if (suppressWarnings)
      lvl = DiagnosticBehavior::Ignore;
  }
  
  if (lvl == DiagnosticBehavior::Remark) {
    if (suppressRemarks)
      lvl = DiagnosticBehavior::Ignore;
  }

  //   5) Update current state for use during the next diagnostic
  if (lvl == DiagnosticBehavior::Fatal) {
    fatalErrorOccurred = true;
    anyErrorOccurred = true;
  } else if (lvl == DiagnosticBehavior::Error) {
    anyErrorOccurred = true;
  }

  assert((!AssertOnError || !anyErrorOccurred) && "We emitted an error?!");
  assert((!AssertOnWarning || (lvl != DiagnosticBehavior::Warning)) &&
         "We emitted a warning?!");

  previousBehavior = lvl;
  return lvl;
}

void DiagnosticEngine::flushActiveDiagnostic() {
  assert(ActiveDiagnostic && "No active diagnostic to flush");
  handleDiagnostic(std::move(*ActiveDiagnostic));
  ActiveDiagnostic.reset();
}

void DiagnosticEngine::handleDiagnostic(Diagnostic &&diag) {
  if (TransactionCount == 0) {
    emitDiagnostic(diag);
    WrappedDiagnostics.clear();
    WrappedDiagnosticArgs.clear();
  } else {
    onTentativeDiagnosticFlush(diag);
    TentativeDiagnostics.emplace_back(std::move(diag));
  }
}

void DiagnosticEngine::clearTentativeDiagnostics() {
  TentativeDiagnostics.clear();
  WrappedDiagnostics.clear();
  WrappedDiagnosticArgs.clear();
}

void DiagnosticEngine::emitTentativeDiagnostics() {
  for (auto &diag : TentativeDiagnostics) {
    emitDiagnostic(diag);
  }
  clearTentativeDiagnostics();
}

void DiagnosticEngine::forwardTentativeDiagnosticsTo(
    DiagnosticEngine &targetEngine) {
  for (auto &diag : TentativeDiagnostics) {
    targetEngine.handleDiagnostic(std::move(diag));
  }
  clearTentativeDiagnostics();
}

/// Returns the access level of the least accessible PrettyPrintedDeclarations
/// buffer that \p decl should appear in.
///
/// This is always \c Public unless \p decl is a \c ValueDecl and its
/// access level is below \c Public. (That can happen with @testable and
/// @_private imports.)
static AccessLevel getBufferAccessLevel(const Decl *decl) {
  AccessLevel level = AccessLevel::Public;
  if (auto *VD = dyn_cast<ValueDecl>(decl))
    level = VD->getFormalAccessScope().accessLevelForDiagnostics();
  if (level > AccessLevel::Public) level = AccessLevel::Public;
  return level;
}

Optional<DiagnosticInfo>
DiagnosticEngine::diagnosticInfoForDiagnostic(const Diagnostic &diagnostic) {
  auto behavior = state.determineBehavior(diagnostic);
  if (behavior == DiagnosticBehavior::Ignore)
    return None;

  // Figure out the source location.
  SourceLoc loc = diagnostic.getLoc();
  if (loc.isInvalid() && diagnostic.getDecl()) {
    const Decl *decl = diagnostic.getDecl();
    // If a declaration was provided instead of a location, and that declaration
    // has a location we can point to, use that location.
    loc = decl->getLoc();

    if (loc.isInvalid()) {
      // There is no location we can point to. Pretty-print the declaration
      // so we can point to it.
      SourceLoc ppLoc = PrettyPrintedDeclarations[decl];
      if (ppLoc.isInvalid()) {
        class TrackingPrinter : public StreamPrinter {
          SmallVectorImpl<std::pair<const Decl *, uint64_t>> &Entries;
          AccessLevel bufferAccessLevel;

        public:
          TrackingPrinter(
              SmallVectorImpl<std::pair<const Decl *, uint64_t>> &Entries,
              raw_ostream &OS, AccessLevel bufferAccessLevel) :
            StreamPrinter(OS), Entries(Entries),
            bufferAccessLevel(bufferAccessLevel) {}

          void printDeclLoc(const Decl *D) override {
            if (getBufferAccessLevel(D) == bufferAccessLevel)
              Entries.push_back({ D, OS.tell() });
          }
        };
        SmallVector<std::pair<const Decl *, uint64_t>, 8> entries;
        llvm::SmallString<128> buffer;
        llvm::SmallString<128> bufferName;
        const Decl *ppDecl = decl;
        {
          // The access level of the buffer we want to print. Declarations below
          // this access level will be omitted from the buffer; declarations
          // above it will be printed, but (except for Open declarations in the
          // Public buffer) will not be recorded in PrettyPrintedDeclarations as
          // the "true" SourceLoc for the declaration.
          AccessLevel bufferAccessLevel = getBufferAccessLevel(decl);

          // Figure out which declaration to print. It's the top-most
          // declaration (not a module).
          auto dc = decl->getDeclContext();

          // FIXME: Horrible, horrible hackaround. We're not getting a
          // DeclContext everywhere we should.
          if (!dc) {
            return None;
          }

          while (!dc->isModuleContext()) {
            switch (dc->getContextKind()) {
            case DeclContextKind::Package:
              llvm_unreachable("Not in a package context!");
              break;
            case DeclContextKind::Module:
              llvm_unreachable("Not in a module context!");
              break;

            case DeclContextKind::FileUnit:
            case DeclContextKind::TopLevelCodeDecl:
              break;

            case DeclContextKind::ExtensionDecl:
              ppDecl = cast<ExtensionDecl>(dc);
              break;

            case DeclContextKind::GenericTypeDecl:
              ppDecl = cast<GenericTypeDecl>(dc);
              break;

            case DeclContextKind::SerializedLocal:
            case DeclContextKind::Initializer:
            case DeclContextKind::AbstractClosureExpr:
            case DeclContextKind::AbstractFunctionDecl:
            case DeclContextKind::SubscriptDecl:
            case DeclContextKind::EnumElementDecl:
            case DeclContextKind::MacroDecl:
              break;
            }

            dc = dc->getParent();
          }

          // Build the module name path (in reverse), which we use to
          // build the name of the buffer.
          SmallVector<StringRef, 4> nameComponents;
          while (dc) {
            nameComponents.push_back(cast<ModuleDecl>(dc)->getName().str());
            dc = dc->getParent();
          }

          for (unsigned i = nameComponents.size(); i; --i) {
            bufferName += nameComponents[i-1];
            bufferName += '.';
          }

          if (auto value = dyn_cast<ValueDecl>(ppDecl)) {
            bufferName += value->getBaseName().userFacingName();
          } else if (auto ext = dyn_cast<ExtensionDecl>(ppDecl)) {
            bufferName += ext->getExtendedType().getString();
          }

          // If we're using a lowered access level, give the buffer a distinct
          // name.
          if (bufferAccessLevel != AccessLevel::Public) {
            assert(bufferAccessLevel < AccessLevel::Public
                   && "Above-public access levels should use public buffer");
            bufferName += " (";
            bufferName += getAccessLevelSpelling(bufferAccessLevel);
            bufferName += ")";
          }

          // Pretty-print the declaration we've picked.
          llvm::raw_svector_ostream out(buffer);
          TrackingPrinter printer(entries, out, bufferAccessLevel);
          llvm::SaveAndRestore<bool> isPrettyPrinting(
              IsPrettyPrintingDecl, true);
          ppDecl->print(
              printer,
              PrintOptions::printForDiagnostics(
                  bufferAccessLevel,
                  decl->getASTContext().TypeCheckerOpts.PrintFullConvention));
        }

        // Build a buffer with the pretty-printed declaration.
        auto bufferID = SourceMgr.addMemBufferCopy(buffer, bufferName);
        auto memBufferStartLoc = SourceMgr.getLocForBufferStart(bufferID);

        SourceMgr.setGeneratedSourceInfo(
            bufferID,
            GeneratedSourceInfo{
              GeneratedSourceInfo::PrettyPrinted,
              CharSourceRange(),
              CharSourceRange(memBufferStartLoc, buffer.size()),
              ASTNode(const_cast<Decl *>(ppDecl)).getOpaqueValue(),
              nullptr
            }
        );

        // Go through all of the pretty-printed entries and record their
        // locations.
        for (auto entry : entries) {
          PrettyPrintedDeclarations[entry.first] =
              memBufferStartLoc.getAdvancedLoc(entry.second);
        }

        // Grab the pretty-printed location.
        ppLoc = PrettyPrintedDeclarations[decl];
      }

      loc = ppLoc;
    }
  }

  StringRef Category;
  if (isAPIDigesterBreakageDiagnostic(diagnostic.getID()))
    Category = "api-digester-breaking-change";
  else if (isDeprecationDiagnostic(diagnostic.getID()))
    Category = "deprecation";
  else if (isNoUsageDiagnostic(diagnostic.getID()))
    Category = "no-usage";

  auto fixIts = diagnostic.getFixIts();
  if (loc.isValid()) {
    // If the diagnostic is being emitted in a generated buffer, drop the
    // fix-its, as the user will have no way of applying them.
    auto bufferID = SourceMgr.findBufferContainingLoc(loc);
    if (auto generatedInfo = SourceMgr.getGeneratedSourceInfo(bufferID)) {
      switch (generatedInfo->kind) {
      case GeneratedSourceInfo::ExpressionMacroExpansion:
      case GeneratedSourceInfo::FreestandingDeclMacroExpansion:
      case GeneratedSourceInfo::AccessorMacroExpansion:
      case GeneratedSourceInfo::MemberAttributeMacroExpansion:
      case GeneratedSourceInfo::MemberMacroExpansion:
      case GeneratedSourceInfo::PeerMacroExpansion:
      case GeneratedSourceInfo::ConformanceMacroExpansion:
      case GeneratedSourceInfo::PrettyPrinted:
        fixIts = {};
        break;
      case GeneratedSourceInfo::ReplacedFunctionBody:
        // A replaced function body is for user-written code, so fix-its are
        // still valid.
        break;
      }
    }
  }

  return DiagnosticInfo(
      diagnostic.getID(), loc, toDiagnosticKind(behavior),
      diagnosticStringFor(diagnostic.getID(), getPrintDiagnosticNames()),
      diagnostic.getArgs(), Category, getDefaultDiagnosticLoc(),
      /*child note info*/ {}, diagnostic.getRanges(), fixIts,
      diagnostic.isChildNote());
}

std::vector<Diagnostic>
DiagnosticEngine::getGeneratedSourceBufferNotes(SourceLoc loc) {
  // The set of child notes we're building up.
  std::vector<Diagnostic> childNotes;

  // If the location is invalid, there's nothing to do.
  if (loc.isInvalid())
    return childNotes;

  // If we already emitted these notes for a prior part of the diagnostic,
  // don't do so again.
  auto currentBufferID = SourceMgr.findBufferContainingLoc(loc);
  SourceLoc currentLoc = loc;
  do {
    auto generatedInfo = SourceMgr.getGeneratedSourceInfo(currentBufferID);
    if (!generatedInfo)
      return childNotes;

    ASTNode expansionNode =
        ASTNode::getFromOpaqueValue(generatedInfo->astNode);

    switch (generatedInfo->kind) {
    case GeneratedSourceInfo::ExpressionMacroExpansion:
    case GeneratedSourceInfo::FreestandingDeclMacroExpansion:
    case GeneratedSourceInfo::AccessorMacroExpansion:
    case GeneratedSourceInfo::MemberAttributeMacroExpansion:
    case GeneratedSourceInfo::MemberMacroExpansion:
    case GeneratedSourceInfo::PeerMacroExpansion:
    case GeneratedSourceInfo::ConformanceMacroExpansion: {
      SourceRange origRange = expansionNode.getSourceRange();
      DeclName macroName = getGeneratedSourceInfoMacroName(*generatedInfo);

      Diagnostic expansionNote(diag::in_macro_expansion, macroName);
      expansionNote.setLoc(origRange.Start);
      expansionNote.addRange(
          Lexer::getCharSourceRangeFromSourceRange(SourceMgr, origRange));
      expansionNote.setIsChildNote(true);
      childNotes.push_back(std::move(expansionNote));
      break;
    }

    case GeneratedSourceInfo::PrettyPrinted:
      break;

    case GeneratedSourceInfo::ReplacedFunctionBody:
      return childNotes;
    }

    // Walk up the stack.
    currentLoc = expansionNode.getStartLoc();
    if (currentLoc.isInvalid())
      return childNotes;

    currentBufferID = SourceMgr.findBufferContainingLoc(currentLoc);
  } while (true);
}

void DiagnosticEngine::emitDiagnostic(const Diagnostic &diagnostic) {

  ArrayRef<Diagnostic> childNotes = diagnostic.getChildNotes();
  std::vector<Diagnostic> extendedChildNotes;

  if (auto info = diagnosticInfoForDiagnostic(diagnostic)) {
    // If the diagnostic location is within a buffer containing generated
    // source code, add child notes showing where the generation occurred.
    // We need to avoid doing this if this is itself a child note, as otherwise
    // we'd end up doubling up on notes.
    if (!info->IsChildNote) {
      extendedChildNotes = getGeneratedSourceBufferNotes(info->Loc);
    }
    if (!extendedChildNotes.empty()) {
      extendedChildNotes.insert(extendedChildNotes.end(),
                                childNotes.begin(), childNotes.end());
      childNotes = extendedChildNotes;
    }

    SmallVector<DiagnosticInfo, 1> childInfo;
    for (unsigned i : indices(childNotes)) {
      auto child = diagnosticInfoForDiagnostic(childNotes[i]);
      assert(child);
      assert(child->Kind == DiagnosticKind::Note &&
             "Expected child diagnostics to all be notes?!");
      childInfo.push_back(*child);
    }
    TinyPtrVector<DiagnosticInfo *> childInfoPtrs;
    for (unsigned i : indices(childInfo)) {
      childInfoPtrs.push_back(&childInfo[i]);
    }
    info->ChildDiagnosticInfo = childInfoPtrs;

    SmallVector<std::string, 1> educationalNotePaths;
    auto associatedNotes = educationalNotes[(uint32_t)diagnostic.getID()];
    while (associatedNotes && *associatedNotes) {
      SmallString<128> notePath(getDiagnosticDocumentationPath());
      llvm::sys::path::append(notePath, *associatedNotes);
      educationalNotePaths.push_back(notePath.str().str());
      ++associatedNotes;
    }
    info->EducationalNotePaths = educationalNotePaths;

    for (auto &consumer : Consumers) {
      consumer->handleDiagnostic(SourceMgr, *info);
    }
  }

  // For compatibility with DiagnosticConsumers which don't know about child
  // notes. These can be ignored by consumers which do take advantage of the
  // grouping.
  for (auto &childNote : childNotes)
    emitDiagnostic(childNote);
}

DiagnosticKind DiagnosticEngine::declaredDiagnosticKindFor(const DiagID id) {
  return storedDiagnosticInfos[(unsigned)id].kind;
}

llvm::StringRef
DiagnosticEngine::diagnosticStringFor(const DiagID id,
                                      bool printDiagnosticNames) {
  auto defaultMessage = printDiagnosticNames
                            ? debugDiagnosticStrings[(unsigned)id]
                            : diagnosticStrings[(unsigned)id];

  if (auto producer = localization.get()) {
    auto localizedMessage = producer->getMessageOr(id, defaultMessage);
    return localizedMessage;
  }
  return defaultMessage;
}

llvm::StringRef
DiagnosticEngine::diagnosticIDStringFor(const DiagID id) {
  return diagnosticIDStrings[(unsigned)id];
}

const char *InFlightDiagnostic::fixItStringFor(const FixItID id) {
  return fixItStrings[(unsigned)id];
}

void DiagnosticEngine::setBufferIndirectlyCausingDiagnosticToInput(
    SourceLoc loc) {
  // If in the future, nested BufferIndirectlyCausingDiagnosticRAII need be
  // supported, the compiler will need a stack for
  // bufferIndirectlyCausingDiagnostic.
  assert(bufferIndirectlyCausingDiagnostic.isInvalid() &&
         "Buffer should not already be set.");
  bufferIndirectlyCausingDiagnostic = loc;
  assert(bufferIndirectlyCausingDiagnostic.isValid() &&
         "Buffer must be valid for previous assertion to work.");
}

void DiagnosticEngine::resetBufferIndirectlyCausingDiagnostic() {
  bufferIndirectlyCausingDiagnostic = SourceLoc();
}

DiagnosticSuppression::DiagnosticSuppression(DiagnosticEngine &diags)
  : diags(diags)
{
  consumers = diags.takeConsumers();
}

DiagnosticSuppression::~DiagnosticSuppression() {
  for (auto consumer : consumers)
    diags.addConsumer(*consumer);
}

bool DiagnosticSuppression::isEnabled(const DiagnosticEngine &diags) {
  return diags.getConsumers().empty();
}

BufferIndirectlyCausingDiagnosticRAII::BufferIndirectlyCausingDiagnosticRAII(
    const SourceFile &SF)
    : Diags(SF.getASTContext().Diags) {
  auto id = SF.getBufferID();
  if (!id)
    return;
  auto loc = SF.getASTContext().SourceMgr.getLocForBufferStart(*id);
  if (loc.isValid())
    Diags.setBufferIndirectlyCausingDiagnosticToInput(loc);
}

void DiagnosticEngine::onTentativeDiagnosticFlush(Diagnostic &diagnostic) {
  for (auto &argument : diagnostic.Args) {
    if (argument.getKind() != DiagnosticArgumentKind::String)
      continue;

    auto content = argument.getAsString();
    if (content.empty())
      continue;

    auto I = TransactionStrings.insert(content).first;
    argument = DiagnosticArgument(StringRef(I->getKeyData()));
  }
}

EncodedDiagnosticMessage::EncodedDiagnosticMessage(StringRef S)
    : Message(Lexer::getEncodedStringSegment(S, Buf, /*IsFirstSegment=*/true,
                                             /*IsLastSegment=*/true,
                                             /*IndentToStrip=*/~0U)) {}

std::pair<unsigned, DeclName>
swift::getAccessorKindAndNameForDiagnostics(const ValueDecl *D) {
  // This should always be one more than the last AccessorKind supported in
  // the diagnostics. If you need to change it, change the assertion below as
  // well.
  static const unsigned NOT_ACCESSOR_INDEX = 2;

  if (auto *accessor = dyn_cast<AccessorDecl>(D)) {
    DeclName Name = accessor->getStorage()->getName();
    assert(accessor->isGetterOrSetter());
    return {static_cast<unsigned>(accessor->getAccessorKind()), Name};
  }

  return {NOT_ACCESSOR_INDEX, D->getName()};
}

DeclName
swift::getGeneratedSourceInfoMacroName(const GeneratedSourceInfo &info) {
  ASTNode expansionNode = ASTNode::getFromOpaqueValue(info.astNode);
  switch (info.kind) {
  case GeneratedSourceInfo::ExpressionMacroExpansion:
  case GeneratedSourceInfo::FreestandingDeclMacroExpansion:
  case GeneratedSourceInfo::AccessorMacroExpansion:
  case GeneratedSourceInfo::MemberAttributeMacroExpansion:
  case GeneratedSourceInfo::MemberMacroExpansion:
  case GeneratedSourceInfo::PeerMacroExpansion:
  case GeneratedSourceInfo::ConformanceMacroExpansion: {
    DeclName macroName;
    if (auto customAttr = info.attachedMacroCustomAttr) {
      // FIXME: How will we handle deserialized custom attributes like this?
      auto declRefType = cast<DeclRefTypeRepr>(customAttr->getTypeRepr());
      return declRefType->getNameRef().getFullName();
    }

    if (auto expansionExpr = dyn_cast_or_null<MacroExpansionExpr>(
            expansionNode.dyn_cast<Expr *>())) {
      return expansionExpr->getMacroName().getFullName();
    }

    auto expansionDecl =
        cast<MacroExpansionDecl>(expansionNode.get<Decl *>());
      return expansionDecl->getMacroName().getFullName();
  }

  case GeneratedSourceInfo::PrettyPrinted:
  case GeneratedSourceInfo::ReplacedFunctionBody:
    return DeclName();
  }
}
