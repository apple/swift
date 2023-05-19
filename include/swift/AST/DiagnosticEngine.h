//===--- DiagnosticEngine.h - Diagnostic Display Engine ---------*- C++ -*-===//
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
//  This file declares the DiagnosticEngine class, which manages any diagnostics
//  emitted by Swift.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_DIAGNOSTICENGINE_H
#define SWIFT_BASIC_DIAGNOSTICENGINE_H

#include "swift/AST/ActorIsolation.h"
#include "swift/AST/DeclNameLoc.h"
#include "swift/AST/DiagnosticConsumer.h"
#include "swift/AST/TypeLoc.h"
#include "swift/Basic/Version.h"
#include "swift/Localization/LocalizationFormat.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/VersionTuple.h"

namespace clang {
class NamedDecl;
}

namespace swift {
  class Decl;
  class DeclAttribute;
  class DiagnosticEngine;
  class GeneratedSourceInfo;
  class SourceManager;
  class ValueDecl;
  class SourceFile;

  enum class DescriptivePatternKind : uint8_t;
  enum class SelfAccessKind : uint8_t;
  enum class ReferenceOwnership : uint8_t;
  enum class StaticSpellingKind : uint8_t;
  enum class DescriptiveDeclKind : uint8_t;
  enum DeclAttrKind : unsigned;
  enum class StmtKind;

  /// Enumeration describing all of possible diagnostics.
  ///
  /// Each of the diagnostics described in Diagnostics.def has an entry in
  /// this enumeration type that uniquely identifies it.
  enum class DiagID : uint32_t;

  /// Describes a diagnostic along with its argument types.
  ///
  /// The diagnostics header introduces instances of this type for each 
  /// diagnostic, which provide both the set of argument types (used to
  /// check/convert the arguments at each call site) and the diagnostic ID
  /// (for other information about the diagnostic).
  template<typename ...ArgTypes>
  struct Diag {
    /// The diagnostic ID corresponding to this diagnostic.
    DiagID ID;
  };

  namespace detail {
    /// Describes how to pass a diagnostic argument of the given type.
    ///
    /// By default, diagnostic arguments are passed by value, because they
    /// tend to be small. Larger diagnostic arguments
    /// need to specialize this class template to pass by reference.
    template<typename T>
    struct PassArgument {
      typedef T type;
    };
  }

  /// A family of wrapper types for compiler data types that forces its
  /// underlying data to be formatted with full qualification.
  ///
  /// So far, this is only useful for \c Type, hence the SFINAE'ing.
  template <typename T, typename = void> struct FullyQualified {};

  template <typename T>
  struct FullyQualified<
      T, typename std::enable_if<std::is_convertible<T, Type>::value>::type> {
    Type t;

  public:
    FullyQualified(T t) : t(t){};

    Type getType() const { return t; }
  };

  /// Describes the kind of diagnostic argument we're storing.
  ///
  enum class DiagnosticArgumentKind {
    String,
    Integer,
    Unsigned,
    Identifier,
    ObjCSelector,
    ValueDecl,
    Type,
    TypeRepr,
    FullyQualifiedType,
    DescriptivePatternKind,
    SelfAccessKind,
    ReferenceOwnership,
    StaticSpellingKind,
    DescriptiveDeclKind,
    DescriptiveStmtKind,
    DeclAttribute,
    VersionTuple,
    LayoutConstraint,
    ActorIsolation,
    Diagnostic,
    ClangDecl
  };

  namespace diag {
    enum class RequirementKind : uint8_t;
  }

  /// Variant type that holds a single diagnostic argument of a known
  /// type.
  ///
  /// All diagnostic arguments are converted to an instance of this class.
  class DiagnosticArgument {
    DiagnosticArgumentKind Kind;
    union {
      int IntegerVal;
      unsigned UnsignedVal;
      StringRef StringVal;
      DeclNameRef IdentifierVal;
      ObjCSelector ObjCSelectorVal;
      ValueDecl *TheValueDecl;
      Type TypeVal;
      TypeRepr *TyR;
      FullyQualified<Type> FullyQualifiedTypeVal;
      DescriptivePatternKind DescriptivePatternKindVal;
      SelfAccessKind SelfAccessKindVal;
      ReferenceOwnership ReferenceOwnershipVal;
      StaticSpellingKind StaticSpellingKindVal;
      DescriptiveDeclKind DescriptiveDeclKindVal;
      StmtKind DescriptiveStmtKindVal;
      const DeclAttribute *DeclAttributeVal;
      llvm::VersionTuple VersionVal;
      LayoutConstraint LayoutConstraintVal;
      ActorIsolation ActorIsolationVal;
      DiagnosticInfo *DiagnosticVal;
      const clang::NamedDecl *ClangDecl;
    };
    
  public:
    DiagnosticArgument(StringRef S)
      : Kind(DiagnosticArgumentKind::String), StringVal(S) {
    }

    DiagnosticArgument(int I) 
      : Kind(DiagnosticArgumentKind::Integer), IntegerVal(I) {
    }

    DiagnosticArgument(unsigned I) 
      : Kind(DiagnosticArgumentKind::Unsigned), UnsignedVal(I) {
    }

    DiagnosticArgument(DeclNameRef R)
        : Kind(DiagnosticArgumentKind::Identifier), IdentifierVal(R) {}

    DiagnosticArgument(DeclName D)
        : Kind(DiagnosticArgumentKind::Identifier),
          IdentifierVal(DeclNameRef(D)) {}

    DiagnosticArgument(DeclBaseName D)
        : Kind(DiagnosticArgumentKind::Identifier),
          IdentifierVal(DeclNameRef(D)) {}

    DiagnosticArgument(Identifier I)
      : Kind(DiagnosticArgumentKind::Identifier),
        IdentifierVal(DeclNameRef(I)) {
    }

    DiagnosticArgument(ObjCSelector S)
      : Kind(DiagnosticArgumentKind::ObjCSelector), ObjCSelectorVal(S) {
    }

    DiagnosticArgument(ValueDecl *VD)
      : Kind(DiagnosticArgumentKind::ValueDecl), TheValueDecl(VD) {
    }

    DiagnosticArgument(Type T)
      : Kind(DiagnosticArgumentKind::Type), TypeVal(T) {
    }

    DiagnosticArgument(TypeRepr *T)
      : Kind(DiagnosticArgumentKind::TypeRepr), TyR(T) {
    }

    DiagnosticArgument(FullyQualified<Type> FQT)
        : Kind(DiagnosticArgumentKind::FullyQualifiedType),
          FullyQualifiedTypeVal(FQT) {}

    DiagnosticArgument(const TypeLoc &TL) {
      if (TypeRepr *tyR = TL.getTypeRepr()) {
        Kind = DiagnosticArgumentKind::TypeRepr;
        TyR = tyR;
      } else {
        Kind = DiagnosticArgumentKind::Type;
        TypeVal = TL.getType();
      }
    }

    DiagnosticArgument(DescriptivePatternKind DPK)
        : Kind(DiagnosticArgumentKind::DescriptivePatternKind),
          DescriptivePatternKindVal(DPK) {}

    DiagnosticArgument(ReferenceOwnership RO)
        : Kind(DiagnosticArgumentKind::ReferenceOwnership),
          ReferenceOwnershipVal(RO) {}

    DiagnosticArgument(SelfAccessKind SAK)
        : Kind(DiagnosticArgumentKind::SelfAccessKind),
          SelfAccessKindVal(SAK) {}

    DiagnosticArgument(StaticSpellingKind SSK)
        : Kind(DiagnosticArgumentKind::StaticSpellingKind),
          StaticSpellingKindVal(SSK) {}

    DiagnosticArgument(DescriptiveDeclKind DDK)
        : Kind(DiagnosticArgumentKind::DescriptiveDeclKind),
          DescriptiveDeclKindVal(DDK) {}

    DiagnosticArgument(StmtKind SK)
        : Kind(DiagnosticArgumentKind::DescriptiveStmtKind),
          DescriptiveStmtKindVal(SK) {}

    DiagnosticArgument(const DeclAttribute *attr)
        : Kind(DiagnosticArgumentKind::DeclAttribute),
          DeclAttributeVal(attr) {}

    DiagnosticArgument(llvm::VersionTuple version)
      : Kind(DiagnosticArgumentKind::VersionTuple),
        VersionVal(version) { }

    DiagnosticArgument(LayoutConstraint L)
      : Kind(DiagnosticArgumentKind::LayoutConstraint), LayoutConstraintVal(L) {
    }

    DiagnosticArgument(ActorIsolation AI)
      : Kind(DiagnosticArgumentKind::ActorIsolation),
        ActorIsolationVal(AI) {
    }

    DiagnosticArgument(DiagnosticInfo *D)
      : Kind(DiagnosticArgumentKind::Diagnostic),
        DiagnosticVal(D) {
    }

    DiagnosticArgument(const clang::NamedDecl *ND)
        : Kind(DiagnosticArgumentKind::ClangDecl), ClangDecl(ND) {}

    /// Initializes a diagnostic argument using the underlying type of the
    /// given enum.
    template<
        typename EnumType,
        typename std::enable_if<std::is_enum<EnumType>::value>::type* = nullptr>
    DiagnosticArgument(EnumType value)
      : DiagnosticArgument(
          static_cast<typename std::underlying_type<EnumType>::type>(value)) {}

    DiagnosticArgumentKind getKind() const { return Kind; }

    StringRef getAsString() const {
      assert(Kind == DiagnosticArgumentKind::String);
      return StringVal;
    }

    int getAsInteger() const {
      assert(Kind == DiagnosticArgumentKind::Integer);
      return IntegerVal;
    }

    unsigned getAsUnsigned() const {
      assert(Kind == DiagnosticArgumentKind::Unsigned);
      return UnsignedVal;
    }

    DeclNameRef getAsIdentifier() const {
      assert(Kind == DiagnosticArgumentKind::Identifier);
      return IdentifierVal;
    }

    ObjCSelector getAsObjCSelector() const {
      assert(Kind == DiagnosticArgumentKind::ObjCSelector);
      return ObjCSelectorVal;
    }

    ValueDecl *getAsValueDecl() const {
      assert(Kind == DiagnosticArgumentKind::ValueDecl);
      return TheValueDecl;
    }

    Type getAsType() const {
      assert(Kind == DiagnosticArgumentKind::Type);
      return TypeVal;
    }

    TypeRepr *getAsTypeRepr() const {
      assert(Kind == DiagnosticArgumentKind::TypeRepr);
      return TyR;
    }

    FullyQualified<Type> getAsFullyQualifiedType() const {
      assert(Kind == DiagnosticArgumentKind::FullyQualifiedType);
      return FullyQualifiedTypeVal;
    }

    DescriptivePatternKind getAsDescriptivePatternKind() const {
      assert(Kind == DiagnosticArgumentKind::DescriptivePatternKind);
      return DescriptivePatternKindVal;
    }

    ReferenceOwnership getAsReferenceOwnership() const {
      assert(Kind == DiagnosticArgumentKind::ReferenceOwnership);
      return ReferenceOwnershipVal;
    }

    SelfAccessKind getAsSelfAccessKind() const {
      assert(Kind == DiagnosticArgumentKind::SelfAccessKind);
      return SelfAccessKindVal;
    }

    StaticSpellingKind getAsStaticSpellingKind() const {
      assert(Kind == DiagnosticArgumentKind::StaticSpellingKind);
      return StaticSpellingKindVal;
    }

    DescriptiveDeclKind getAsDescriptiveDeclKind() const {
      assert(Kind == DiagnosticArgumentKind::DescriptiveDeclKind);
      return DescriptiveDeclKindVal;
    }

    StmtKind getAsDescriptiveStmtKind() const {
      assert(Kind == DiagnosticArgumentKind::DescriptiveStmtKind);
      return DescriptiveStmtKindVal;
    }

    const DeclAttribute *getAsDeclAttribute() const {
      assert(Kind == DiagnosticArgumentKind::DeclAttribute);
      return DeclAttributeVal;
    }

    llvm::VersionTuple getAsVersionTuple() const {
      assert(Kind == DiagnosticArgumentKind::VersionTuple);
      return VersionVal;
    }

    LayoutConstraint getAsLayoutConstraint() const {
      assert(Kind == DiagnosticArgumentKind::LayoutConstraint);
      return LayoutConstraintVal;
    }

    ActorIsolation getAsActorIsolation() const {
      assert(Kind == DiagnosticArgumentKind::ActorIsolation);
      return ActorIsolationVal;
    }

    DiagnosticInfo *getAsDiagnostic() const {
      assert(Kind == DiagnosticArgumentKind::Diagnostic);
      return DiagnosticVal;
    }

    const clang::NamedDecl *getAsClangDecl() const {
      assert(Kind == DiagnosticArgumentKind::ClangDecl);
      return ClangDecl;
    }
  };

  /// Describes the current behavior to take with a diagnostic.
  /// Ordered from most severe to least.
  enum class DiagnosticBehavior : uint8_t {
    Unspecified = 0,
    Fatal,
    Error,
    Warning,
    Remark,
    Note,
    Ignore,
  };

  struct DiagnosticFormatOptions {
    const std::string OpeningQuotationMark;
    const std::string ClosingQuotationMark;
    const std::string AKAFormatString;
    const std::string OpaqueResultFormatString;

    DiagnosticFormatOptions(std::string OpeningQuotationMark,
                            std::string ClosingQuotationMark,
                            std::string AKAFormatString,
                            std::string OpaqueResultFormatString)
        : OpeningQuotationMark(OpeningQuotationMark),
          ClosingQuotationMark(ClosingQuotationMark),
          AKAFormatString(AKAFormatString),
          OpaqueResultFormatString(OpaqueResultFormatString) {}

    DiagnosticFormatOptions()
        : OpeningQuotationMark("'"), ClosingQuotationMark("'"),
          AKAFormatString("'%s' (aka '%s')"),
          OpaqueResultFormatString("'%s' (%s of '%s')") {}

    /// When formatting fix-it arguments, don't include quotes or other
    /// additions which would result in invalid code.
    static DiagnosticFormatOptions formatForFixIts() {
      return DiagnosticFormatOptions("", "", "%s", "%s");
    }
  };

  enum class FixItID : uint32_t;

  /// Represents a fix-it defined  with a format string and optional
  /// DiagnosticArguments. The template parameters allow the
  /// fixIt... methods on InFlightDiagnostic to infer their own
  /// template params.
  template <typename... ArgTypes> struct StructuredFixIt { FixItID ID; };

  /// Diagnostic - This is a specific instance of a diagnostic along with all of
  /// the DiagnosticArguments that it requires. 
  class Diagnostic {
  public:
    typedef DiagnosticInfo::FixIt FixIt;

  private:
    DiagID ID;
    SmallVector<DiagnosticArgument, 3> Args;
    SmallVector<CharSourceRange, 2> Ranges;
    SmallVector<FixIt, 2> FixIts;
    std::vector<Diagnostic> ChildNotes;
    SourceLoc Loc;
    bool IsChildNote = false;
    const swift::Decl *Decl = nullptr;
    DiagnosticBehavior BehaviorLimit = DiagnosticBehavior::Unspecified;

    friend DiagnosticEngine;
    friend class InFlightDiagnostic;

  public:
    // All constructors are intentionally implicit.
    template<typename ...ArgTypes>
    Diagnostic(Diag<ArgTypes...> ID,
               typename detail::PassArgument<ArgTypes>::type... VArgs)
      : ID(ID.ID) {
      DiagnosticArgument DiagArgs[] = {
        DiagnosticArgument(0), std::move(VArgs)... 
      };
      Args.append(DiagArgs + 1, DiagArgs + 1 + sizeof...(VArgs));
    }

    /*implicit*/Diagnostic(DiagID ID, ArrayRef<DiagnosticArgument> Args)
      : ID(ID), Args(Args.begin(), Args.end()) {}
    
    // Accessors.
    DiagID getID() const { return ID; }
    ArrayRef<DiagnosticArgument> getArgs() const { return Args; }
    ArrayRef<CharSourceRange> getRanges() const { return Ranges; }
    ArrayRef<FixIt> getFixIts() const { return FixIts; }
    ArrayRef<Diagnostic> getChildNotes() const { return ChildNotes; }
    bool isChildNote() const { return IsChildNote; }
    SourceLoc getLoc() const { return Loc; }
    const class Decl *getDecl() const { return Decl; }
    DiagnosticBehavior getBehaviorLimit() const { return BehaviorLimit; }

    void setLoc(SourceLoc loc) { Loc = loc; }
    void setIsChildNote(bool isChildNote) { IsChildNote = isChildNote; }
    void setDecl(const class Decl *decl) { Decl = decl; }
    void setBehaviorLimit(DiagnosticBehavior limit){ BehaviorLimit = limit; }

    /// Returns true if this object represents a particular diagnostic.
    ///
    /// \code
    /// someDiag.is(diag::invalid_diagnostic)
    /// \endcode
    template<typename ...OtherArgTypes>
    bool is(Diag<OtherArgTypes...> Other) const {
      return ID == Other.ID;
    }

    void addRange(CharSourceRange R) {
      Ranges.push_back(R);
    }

    // Avoid copying the fix-it text more than necessary.
    void addFixIt(FixIt &&F) {
      FixIts.push_back(std::move(F));
    }

    void addChildNote(Diagnostic &&D);
    void insertChildNote(unsigned beforeIndex, Diagnostic &&D);
  };
  
  /// Describes an in-flight diagnostic, which is currently active
  /// within the diagnostic engine and can be augmented within additional
  /// information (source ranges, Fix-Its, etc.).
  ///
  /// Only a single in-flight diagnostic can be active at one time, and all
  /// additional information must be emitted through the active in-flight
  /// diagnostic.
  class InFlightDiagnostic {
    friend class DiagnosticEngine;
    
    DiagnosticEngine *Engine;
    bool IsActive;
    
    /// Create a new in-flight diagnostic. 
    ///
    /// This constructor is only available to the DiagnosticEngine.
    InFlightDiagnostic(DiagnosticEngine &Engine)
      : Engine(&Engine), IsActive(true) { }
    
    InFlightDiagnostic(const InFlightDiagnostic &) = delete;
    InFlightDiagnostic &operator=(const InFlightDiagnostic &) = delete;
    InFlightDiagnostic &operator=(InFlightDiagnostic &&) = delete;

  public:
    /// Create an active but unattached in-flight diagnostic.
    /// 
    /// The resulting diagnostic can be used as a dummy, accepting the
    /// syntax to add additional information to a diagnostic without
    /// actually emitting a diagnostic.
    InFlightDiagnostic() : Engine(0), IsActive(true) { }
    
    /// Transfer an in-flight diagnostic to a new object, which is
    /// typically used when returning in-flight diagnostics.
    InFlightDiagnostic(InFlightDiagnostic &&Other)
      : Engine(Other.Engine), IsActive(Other.IsActive) {
      Other.IsActive = false;
    }
    
    ~InFlightDiagnostic() {
      if (IsActive)
        flush();
    }
    
    /// Flush the active diagnostic to the diagnostic output engine.
    void flush();

    /// Prevent the diagnostic from behaving more severely than \p limit. For
    /// instance, if \c DiagnosticBehavior::Warning is passed, an error will be
    /// emitted as a warning, but a note will still be emitted as a note.
    InFlightDiagnostic &limitBehavior(DiagnosticBehavior limit);

    /// Limit the diagnostic behavior to warning until the specified version.
    ///
    /// This helps stage in fixes for stricter diagnostics as warnings
    /// until the next major language version.
    InFlightDiagnostic &warnUntilSwiftVersion(unsigned majorVersion);

    /// Conditionally limit the diagnostic behavior to warning until
    /// the specified version.  If the condition is false, no limit is
    /// imposed, meaning (presumably) it is treated as an error.
    ///
    /// This helps stage in fixes for stricter diagnostics as warnings
    /// until the next major language version.
    InFlightDiagnostic &warnUntilSwiftVersionIf(bool shouldLimit,
                                                unsigned majorVersion) {
      if (!shouldLimit) return *this;
      return warnUntilSwiftVersion(majorVersion);
    }

    /// Wraps this diagnostic in another diagnostic. That is, \p wrapper will be
    /// emitted in place of the diagnostic that otherwise would have been
    /// emitted.
    ///
    /// The first argument of \p wrapper must be of type 'Diagnostic *'.
    ///
    /// The emitted diagnostic will have:
    ///
    /// \li The ID, message, and behavior of \c wrapper.
    /// \li The arguments of \c wrapper, with the last argument replaced by the
    ///     diagnostic currently in \c *this.
    /// \li The location, ranges, decl, fix-its, and behavior limit of the
    ///     diagnostic currently in \c *this.
    InFlightDiagnostic &wrapIn(const Diagnostic &wrapper);

    /// Wraps this diagnostic in another diagnostic. That is, \p ID and
    /// \p VArgs will be emitted in place of the diagnostic that otherwise would
    /// have been emitted.
    ///
    /// The first argument of \p ID must be of type 'Diagnostic *'.
    ///
    /// The emitted diagnostic will have:
    ///
    /// \li The ID, message, and behavior of \c ID.
    /// \li The arguments of \c VArgs, with an argument appended for the
    ///     diagnostic currently in \c *this.
    /// \li The location, ranges, decl, fix-its, and behavior limit of the
    ///     diagnostic currently in \c *this.
    template<typename ...ArgTypes>
    InFlightDiagnostic &
    wrapIn(Diag<DiagnosticInfo *, ArgTypes...> ID,
           typename detail::PassArgument<ArgTypes>::type... VArgs) {
      Diagnostic wrapper{ID, nullptr, std::move(VArgs)...};
      return wrapIn(wrapper);
    }

    /// Add a token-based range to the currently-active diagnostic.
    InFlightDiagnostic &highlight(SourceRange R);

    /// Add a character-based range to the currently-active diagnostic.
    InFlightDiagnostic &highlightChars(SourceLoc Start, SourceLoc End);

    static const char *fixItStringFor(const FixItID id);

    /// Add a token-based replacement fix-it to the currently-active
    /// diagnostic.
    template <typename... ArgTypes>
    InFlightDiagnostic &
    fixItReplace(SourceRange R, StructuredFixIt<ArgTypes...> fixIt,
                 typename detail::PassArgument<ArgTypes>::type... VArgs) {
      DiagnosticArgument DiagArgs[] = { std::move(VArgs)... };
      return fixItReplace(R, fixItStringFor(fixIt.ID), DiagArgs);
    }

    /// Add a character-based replacement fix-it to the currently-active
    /// diagnostic.
    template <typename... ArgTypes>
    InFlightDiagnostic &
    fixItReplaceChars(SourceLoc Start, SourceLoc End,
                      StructuredFixIt<ArgTypes...> fixIt,
                      typename detail::PassArgument<ArgTypes>::type... VArgs) {
      DiagnosticArgument DiagArgs[] = { std::move(VArgs)... };
      return fixItReplaceChars(Start, End, fixItStringFor(fixIt.ID), DiagArgs);
    }

    /// Add an insertion fix-it to the currently-active diagnostic.
    template <typename... ArgTypes>
    InFlightDiagnostic &
    fixItInsert(SourceLoc L, StructuredFixIt<ArgTypes...> fixIt,
                typename detail::PassArgument<ArgTypes>::type... VArgs) {
      DiagnosticArgument DiagArgs[] = { std::move(VArgs)... };
      return fixItReplaceChars(L, L, fixItStringFor(fixIt.ID), DiagArgs);
    }

    /// Add an insertion fix-it to the currently-active diagnostic.  The
    /// text is inserted immediately *after* the token specified.
    template <typename... ArgTypes>
    InFlightDiagnostic &
    fixItInsertAfter(SourceLoc L, StructuredFixIt<ArgTypes...> fixIt,
                     typename detail::PassArgument<ArgTypes>::type... VArgs) {
      DiagnosticArgument DiagArgs[] = { std::move(VArgs)... };
      return fixItInsertAfter(L, fixItStringFor(fixIt.ID), DiagArgs);
    }

    /// Add a token-based replacement fix-it to the currently-active
    /// diagnostic.
    InFlightDiagnostic &fixItReplace(SourceRange R, StringRef Str);

    /// Add a character-based replacement fix-it to the currently-active
    /// diagnostic.
    InFlightDiagnostic &fixItReplaceChars(SourceLoc Start, SourceLoc End,
                                          StringRef Str) {
      return fixItReplaceChars(Start, End, "%0", {Str});
    }

    /// Add an insertion fix-it to the currently-active diagnostic.
    InFlightDiagnostic &fixItInsert(SourceLoc L, StringRef Str) {
      return fixItReplaceChars(L, L, "%0", {Str});
    }

    /// Add an insertion fix-it to the currently-active diagnostic.  The
    /// text is inserted immediately *after* the token specified.
    InFlightDiagnostic &fixItInsertAfter(SourceLoc L, StringRef Str) {
      return fixItInsertAfter(L, "%0", {Str});
    }
    
    /// Add a token-based removal fix-it to the currently-active
    /// diagnostic.
    InFlightDiagnostic &fixItRemove(SourceRange R);
    
    /// Add a character-based removal fix-it to the currently-active
    /// diagnostic.
    InFlightDiagnostic &fixItRemoveChars(SourceLoc Start, SourceLoc End) {
      return fixItReplaceChars(Start, End, {});
    }

    /// Add two replacement fix-it exchanging source ranges to the
    /// currently-active diagnostic.
    InFlightDiagnostic &fixItExchange(SourceRange R1, SourceRange R2);
    
  private:
    InFlightDiagnostic &fixItReplace(SourceRange R, StringRef FormatString,
                                     ArrayRef<DiagnosticArgument> Args);

    InFlightDiagnostic &fixItReplaceChars(SourceLoc Start, SourceLoc End,
                                          StringRef FormatString,
                                          ArrayRef<DiagnosticArgument> Args);

    InFlightDiagnostic &fixItInsert(SourceLoc L, StringRef FormatString,
                                    ArrayRef<DiagnosticArgument> Args) {
      return fixItReplaceChars(L, L, FormatString, Args);
    }

    InFlightDiagnostic &fixItInsertAfter(SourceLoc L, StringRef FormatString,
                                         ArrayRef<DiagnosticArgument> Args);
  };

  /// Class to track, map, and remap diagnostic severity and fatality
  ///
  class DiagnosticState {
    /// Whether we should continue to emit diagnostics, even after a
    /// fatal error
    bool showDiagnosticsAfterFatalError = false;

    /// Don't emit any warnings
    bool suppressWarnings = false;
    
    /// Don't emit any remarks
    bool suppressRemarks = false;

    /// Emit all warnings as errors
    bool warningsAsErrors = false;

    /// Whether a fatal error has occurred
    bool fatalErrorOccurred = false;

    /// Whether any error diagnostics have been emitted.
    bool anyErrorOccurred = false;

    /// Track the previous emitted Behavior, useful for notes
    DiagnosticBehavior previousBehavior = DiagnosticBehavior::Unspecified;

    /// Track which diagnostics should be ignored.
    llvm::BitVector ignoredDiagnostics;

    friend class DiagnosticStateRAII;

  public:
    DiagnosticState();

    /// Figure out the Behavior for the given diagnostic, taking current
    /// state such as fatality into account.
    DiagnosticBehavior determineBehavior(const Diagnostic &diag);

    bool hadAnyError() const { return anyErrorOccurred; }
    bool hasFatalErrorOccurred() const { return fatalErrorOccurred; }

    void setShowDiagnosticsAfterFatalError(bool val = true) {
      showDiagnosticsAfterFatalError = val;
    }
    bool getShowDiagnosticsAfterFatalError() {
      return showDiagnosticsAfterFatalError;
    }

    /// Whether to skip emitting warnings
    void setSuppressWarnings(bool val) { suppressWarnings = val; }
    bool getSuppressWarnings() const { return suppressWarnings; }
    
    /// Whether to skip emitting remarks
    void setSuppressRemarks(bool val) { suppressRemarks = val; }
    bool getSuppressRemarks() const { return suppressRemarks; }

    /// Whether to treat warnings as errors
    void setWarningsAsErrors(bool val) { warningsAsErrors = val; }
    bool getWarningsAsErrors() const { return warningsAsErrors; }

    void resetHadAnyError() {
      anyErrorOccurred = false;
      fatalErrorOccurred = false;
    }

    /// Set whether a diagnostic should be ignored.
    void setIgnoredDiagnostic(DiagID id, bool ignored) {
      ignoredDiagnostics[(unsigned)id] = ignored;
    }

    void swap(DiagnosticState &other) {
      std::swap(showDiagnosticsAfterFatalError, other.showDiagnosticsAfterFatalError);
      std::swap(suppressWarnings, other.suppressWarnings);
      std::swap(suppressRemarks, other.suppressRemarks);
      std::swap(warningsAsErrors, other.warningsAsErrors);
      std::swap(fatalErrorOccurred, other.fatalErrorOccurred);
      std::swap(anyErrorOccurred, other.anyErrorOccurred);
      std::swap(previousBehavior, other.previousBehavior);
      std::swap(ignoredDiagnostics, other.ignoredDiagnostics);
    }

  private:
    // Make the state movable only
    DiagnosticState(const DiagnosticState &) = delete;
    const DiagnosticState &operator=(const DiagnosticState &) = delete;

    DiagnosticState(DiagnosticState &&) = default;
    DiagnosticState &operator=(DiagnosticState &&) = default;
  };

  /// Class responsible for formatting diagnostics and presenting them
  /// to the user.
  class DiagnosticEngine {
  public:
    /// The source manager used to interpret source locations and
    /// display diagnostics.
    SourceManager &SourceMgr;

  private:
    /// The diagnostic consumer(s) that will be responsible for actually
    /// emitting diagnostics.
    SmallVector<DiagnosticConsumer *, 2> Consumers;

    /// Tracks diagnostic behaviors and state
    DiagnosticState state;

    /// The currently active diagnostic, if there is one.
    Optional<Diagnostic> ActiveDiagnostic;

    /// Diagnostics wrapped by ActiveDiagnostic, if any.
    SmallVector<DiagnosticInfo, 2> WrappedDiagnostics;
    SmallVector<std::vector<DiagnosticArgument>, 4> WrappedDiagnosticArgs;

    /// All diagnostics that have are no longer active but have not yet
    /// been emitted due to an open transaction.
    SmallVector<Diagnostic, 4> TentativeDiagnostics;

    /// The set of declarations for which we have pretty-printed
    /// results that we can point to on the command line.
    llvm::DenseMap<const Decl *, SourceLoc> PrettyPrintedDeclarations;

    llvm::BumpPtrAllocator TransactionAllocator;
    /// A set of all strings involved in current transactional chain.
    /// This is required because diagnostics are not directly emitted
    /// but rather stored until all transactions complete.
    llvm::StringSet<llvm::BumpPtrAllocator &> TransactionStrings;

    /// Diagnostic producer to handle the logic behind retrieving a localized
    /// diagnostic message.
    std::unique_ptr<diag::LocalizationProducer> localization;

    /// The number of open diagnostic transactions. Diagnostics are only
    /// emitted once all transactions have closed.
    unsigned TransactionCount = 0;

    /// For batch mode, use this to know where to output a diagnostic from a
    /// non-primary file. It's any location in the buffer of the current primary
    /// input being compiled.
    /// May be invalid.
    SourceLoc bufferIndirectlyCausingDiagnostic;
    
    /// Print diagnostic names after their messages
    bool printDiagnosticNames = false;

    /// Path to diagnostic documentation directory.
    std::string diagnosticDocumentationPath = "";

    /// The Swift language version. This is used to limit diagnostic behavior
    /// until a specific language version, e.g. Swift 6.
    version::Version languageVersion;

    /// Whether we are actively pretty-printing a declaration as part of
    /// diagnostics.
    bool IsPrettyPrintingDecl = false;

    friend class InFlightDiagnostic;
    friend class DiagnosticTransaction;
    friend class CompoundDiagnosticTransaction;
    friend class DiagnosticStateRAII;
    friend class DiagnosticQueue;

  public:
    explicit DiagnosticEngine(SourceManager &SourceMgr)
        : SourceMgr(SourceMgr), ActiveDiagnostic(),
          TransactionStrings(TransactionAllocator) {}

    /// hadAnyError - return true if any *error* diagnostics have been emitted.
    bool hadAnyError() const { return state.hadAnyError(); }

    bool hasFatalErrorOccurred() const {
      return state.hasFatalErrorOccurred();
    }

    void setShowDiagnosticsAfterFatalError(bool val = true) {
      state.setShowDiagnosticsAfterFatalError(val);
    }
    bool getShowDiagnosticsAfterFatalError() {
      return state.getShowDiagnosticsAfterFatalError();
    }

    void flushConsumers() {
      for (auto consumer : Consumers)
        consumer->flush();
    }

    /// Whether to skip emitting warnings
    void setSuppressWarnings(bool val) { state.setSuppressWarnings(val); }
    bool getSuppressWarnings() const {
      return state.getSuppressWarnings();
    }

    /// Whether to skip emitting remarks
    void setSuppressRemarks(bool val) { state.setSuppressRemarks(val); }
    bool getSuppressRemarks() const {
      return state.getSuppressRemarks();
    }

    /// Whether to treat warnings as errors
    void setWarningsAsErrors(bool val) { state.setWarningsAsErrors(val); }
    bool getWarningsAsErrors() const {
      return state.getWarningsAsErrors();
    }

    /// Whether to print diagnostic names after their messages
    void setPrintDiagnosticNames(bool val) {
      printDiagnosticNames = val;
    }
    bool getPrintDiagnosticNames() const {
      return printDiagnosticNames;
    }

    void setDiagnosticDocumentationPath(std::string path) {
      diagnosticDocumentationPath = path;
    }
    StringRef getDiagnosticDocumentationPath() {
      return diagnosticDocumentationPath;
    }

    bool isPrettyPrintingDecl() const { return IsPrettyPrintingDecl; }

    void setLanguageVersion(version::Version v) { languageVersion = v; }

    void setLocalization(StringRef locale, StringRef path) {
      assert(!locale.empty());
      assert(!path.empty());
      localization = diag::LocalizationProducer::producerFor(
          locale, path, getPrintDiagnosticNames());
    }

    void ignoreDiagnostic(DiagID id) {
      state.setIgnoredDiagnostic(id, true);
    }

    void resetHadAnyError() {
      state.resetHadAnyError();
    }

    /// Add an additional DiagnosticConsumer to receive diagnostics.
    void addConsumer(DiagnosticConsumer &Consumer) {
      Consumers.push_back(&Consumer);
    }

    /// Remove a specific DiagnosticConsumer.
    void removeConsumer(DiagnosticConsumer &Consumer) {
      Consumers.erase(
          std::remove(Consumers.begin(), Consumers.end(), &Consumer));
    }

    /// Remove and return all \c DiagnosticConsumers.
    std::vector<DiagnosticConsumer *> takeConsumers() {
      auto Result = std::vector<DiagnosticConsumer*>(Consumers.begin(),
                                                     Consumers.end());
      Consumers.clear();
      return Result;
    }

    /// Return all \c DiagnosticConsumers.
    ArrayRef<DiagnosticConsumer *> getConsumers() const {
      return Consumers;
    }

    /// Emit a diagnostic using a preformatted array of diagnostic
    /// arguments.
    ///
    /// \param Loc The location to which the diagnostic refers in the source
    /// code.
    ///
    /// \param ID The diagnostic ID.
    ///
    /// \param Args The preformatted set of diagnostic arguments. The caller
    /// must ensure that the diagnostic arguments have the appropriate type.
    ///
    /// \returns An in-flight diagnostic, to which additional information can
    /// be attached.
    InFlightDiagnostic diagnose(SourceLoc Loc, DiagID ID, 
                                ArrayRef<DiagnosticArgument> Args) {
      return diagnose(Loc, Diagnostic(ID, Args));
    }

    /// Emit a diagnostic using a preformatted array of diagnostic
    /// arguments.
    ///
    /// \param Loc The declaration name location to which the
    /// diagnostic refers in the source code.
    ///
    /// \param ID The diagnostic ID.
    ///
    /// \param Args The preformatted set of diagnostic arguments. The caller
    /// must ensure that the diagnostic arguments have the appropriate type.
    ///
    /// \returns An in-flight diagnostic, to which additional information can
    /// be attached.
    InFlightDiagnostic diagnose(DeclNameLoc Loc, DiagID ID, 
                                ArrayRef<DiagnosticArgument> Args) {
      return diagnose(Loc.getBaseNameLoc(), Diagnostic(ID, Args));
    }

    /// Emit an already-constructed diagnostic at the given location.
    ///
    /// \param Loc The location to which the diagnostic refers in the source
    /// code.
    ///
    /// \param D The diagnostic.
    ///
    /// \returns An in-flight diagnostic, to which additional information can
    /// be attached.
    InFlightDiagnostic diagnose(SourceLoc Loc, const Diagnostic &D) {
      assert(!ActiveDiagnostic && "Already have an active diagnostic");
      ActiveDiagnostic = D;
      ActiveDiagnostic->setLoc(Loc);
      return InFlightDiagnostic(*this);
    }
    
    /// Emit a diagnostic with the given set of diagnostic arguments.
    ///
    /// \param Loc The location to which the diagnostic refers in the source
    /// code.
    ///
    /// \param ID The diagnostic to be emitted.
    ///
    /// \param Args The diagnostic arguments, which will be converted to
    /// the types expected by the diagnostic \p ID.
    template<typename ...ArgTypes>
    InFlightDiagnostic 
    diagnose(SourceLoc Loc, Diag<ArgTypes...> ID,
             typename detail::PassArgument<ArgTypes>::type... Args) {
      return diagnose(Loc, Diagnostic(ID, std::move(Args)...));
    }

    /// Delete an API that may lead clients to avoid specifying source location.
    template<typename ...ArgTypes>
    InFlightDiagnostic
    diagnose(Diag<ArgTypes...> ID,
             typename detail::PassArgument<ArgTypes>::type... Args) = delete;

    /// Emit a diagnostic with the given set of diagnostic arguments.
    ///
    /// \param Loc The declaration name location to which the
    /// diagnostic refers in the source code.
    ///
    /// \param ID The diagnostic to be emitted.
    ///
    /// \param Args The diagnostic arguments, which will be converted to
    /// the types expected by the diagnostic \p ID.
    template<typename ...ArgTypes>
    InFlightDiagnostic 
    diagnose(DeclNameLoc Loc, Diag<ArgTypes...> ID,
             typename detail::PassArgument<ArgTypes>::type... Args) {
      return diagnose(Loc.getBaseNameLoc(), Diagnostic(ID, std::move(Args)...));
    }

    /// Emit a diagnostic using a preformatted array of diagnostic
    /// arguments.
    ///
    /// \param decl The declaration to which this diagnostic refers, which
    /// may or may not have associated source-location information.
    ///
    /// \param id The diagnostic ID.
    ///
    /// \param args The preformatted set of diagnostic arguments. The caller
    /// must ensure that the diagnostic arguments have the appropriate type.
    ///
    /// \returns An in-flight diagnostic, to which additional information can
    /// be attached.
    InFlightDiagnostic diagnose(const Decl *decl, DiagID id,
                                ArrayRef<DiagnosticArgument> args) {
      return diagnose(decl, Diagnostic(id, args));
    }

    /// Emit an already-constructed diagnostic referencing the given
    /// declaration.
    ///
    /// \param decl The declaration to which this diagnostic refers, which
    /// may or may not have associated source-location information.
    ///
    /// \param diag The diagnostic.
    ///
    /// \returns An in-flight diagnostic, to which additional information can
    /// be attached.
    InFlightDiagnostic diagnose(const Decl *decl, const Diagnostic &diag) {
      assert(!ActiveDiagnostic && "Already have an active diagnostic");
      ActiveDiagnostic = diag;
      ActiveDiagnostic->setDecl(decl);
      return InFlightDiagnostic(*this);
    }

    /// Emit a diagnostic with the given set of diagnostic arguments.
    ///
    /// \param decl The declaration to which this diagnostic refers, which
    /// may or may not have associated source-location information.
    ///
    /// \param id The diagnostic to be emitted.
    ///
    /// \param args The diagnostic arguments, which will be converted to
    /// the types expected by the diagnostic \p ID.
    template<typename ...ArgTypes>
    InFlightDiagnostic
    diagnose(const Decl *decl, Diag<ArgTypes...> id,
             typename detail::PassArgument<ArgTypes>::type... args) {
      return diagnose(decl, Diagnostic(id, std::move(args)...));
    }

    /// Emit a parent diagnostic and attached notes.
    ///
    /// \param parentDiag An InFlightDiagnostic representing the parent diag.
    ///
    /// \param builder A closure which builds and emits notes to be attached to
    /// the parent diag.
    void diagnoseWithNotes(InFlightDiagnostic parentDiag,
                           llvm::function_ref<void(void)> builder);

    /// \returns true if diagnostic is marked with PointsToFirstBadToken
    /// option.
    bool isDiagnosticPointsToFirstBadToken(DiagID id) const;

    /// \returns true if the diagnostic is an API digester API or ABI breakage
    /// diagnostic.
    bool isAPIDigesterBreakageDiagnostic(DiagID id) const;

    /// \returns true if the diagnostic is marking a deprecation.
    bool isDeprecationDiagnostic(DiagID id) const;

    /// \returns true if the diagnostic is marking an unused element.
    bool isNoUsageDiagnostic(DiagID id) const;

    /// \returns true if any diagnostic consumer gave an error while invoking
    //// \c finishProcessing.
    bool finishProcessing();

    /// Format the given diagnostic text and place the result in the given
    /// buffer.
    static void formatDiagnosticText(
        llvm::raw_ostream &Out, StringRef InText,
        ArrayRef<DiagnosticArgument> FormatArgs,
        DiagnosticFormatOptions FormatOpts = DiagnosticFormatOptions());

  private:
    /// Called when tentative diagnostic is about to be flushed,
    /// to apply any required transformations e.g. copy string arguments
    /// to extend their lifetime.
    void onTentativeDiagnosticFlush(Diagnostic &diagnostic);

    /// Flush the active diagnostic.
    void flushActiveDiagnostic();
    
    /// Retrieve the active diagnostic.
    Diagnostic &getActiveDiagnostic() { return *ActiveDiagnostic; }

    /// Generate DiagnosticInfo for a Diagnostic to be passed to consumers.
    Optional<DiagnosticInfo>
    diagnosticInfoForDiagnostic(const Diagnostic &diagnostic);

    /// Send \c diag to all diagnostic consumers.
    void emitDiagnostic(const Diagnostic &diag);

    /// Retrieve the set of child notes that describe how the generated
    /// source buffer was derived, e.g., a macro expansion backtrace.
    std::vector<Diagnostic> getGeneratedSourceBufferNotes(SourceLoc loc);

    /// Handle a new diagnostic, which will either be emitted, or added to an
    /// active transaction.
    void handleDiagnostic(Diagnostic &&diag);

    /// Clear any tentative diagnostics.
    void clearTentativeDiagnostics();

    /// Send all tentative diagnostics to all diagnostic consumers and
    /// delete them.
    void emitTentativeDiagnostics();

    /// Forward all tentative diagnostics to a different diagnostic engine.
    void forwardTentativeDiagnosticsTo(DiagnosticEngine &targetEngine);

  public:
    DiagnosticKind declaredDiagnosticKindFor(const DiagID id);

    llvm::StringRef diagnosticStringFor(const DiagID id,
                                        bool printDiagnosticNames);

    static llvm::StringRef diagnosticIDStringFor(const DiagID id);

    /// If there is no clear .dia file for a diagnostic, put it in the one
    /// corresponding to the SourceLoc given here.
    /// In particular, in batch mode when a diagnostic is located in
    /// a non-primary file, use this affordance to place it in the .dia
    /// file for the primary that is currently being worked on.
    void setBufferIndirectlyCausingDiagnosticToInput(SourceLoc);
    void resetBufferIndirectlyCausingDiagnostic();
    SourceLoc getDefaultDiagnosticLoc() const {
      return bufferIndirectlyCausingDiagnostic;
    }
  };

  /// Remember details about the state of a diagnostic engine and restore them
  /// when the object is destroyed.
  ///
  /// Diagnostic engines contain state about the most recent diagnostic emitted
  /// which influences subsequent emissions; in particular, if you try to emit
  /// a note and the previous diagnostic was ignored, the note will be ignored
  /// too. This can be a problem in code structured like:
  ///
  ///     D->diagnose(diag::an_error);
  ///     if (conditionWhichMightEmitDiagnostics())
  ///        D->diagnose(diag::a_note); // might be affected by diagnostics from
  ///                                   // conditionWhichMightEmitDiagnostics()!
  ///
  /// To prevent this, functions which are called for their return values but
  /// may emit diagnostics as a side effect can use \c DiagnosticStateRAII to
  /// ensure that their changes to diagnostic engine state don't leak out and
  /// affect the caller's diagnostics.
  class DiagnosticStateRAII {
    llvm::SaveAndRestore<DiagnosticBehavior> previousBehavior;

  public:
    DiagnosticStateRAII(DiagnosticEngine &diags)
      : previousBehavior(diags.state.previousBehavior) {}

    ~DiagnosticStateRAII() {}
  };

  class BufferIndirectlyCausingDiagnosticRAII {
  private:
    DiagnosticEngine &Diags;
  public:
    BufferIndirectlyCausingDiagnosticRAII(const SourceFile &SF);
    ~BufferIndirectlyCausingDiagnosticRAII() {
      Diags.resetBufferIndirectlyCausingDiagnostic();
    }
  };

  /// Represents a diagnostic transaction. While a transaction is
  /// open, all recorded diagnostics are saved until the transaction commits,
  /// at which point they are emitted. If the transaction is instead aborted,
  /// the diagnostics are erased. Transactions may be nested but must be closed
  /// in LIFO order. An open transaction is implicitly committed upon
  /// destruction.
  class DiagnosticTransaction {
  protected:
    DiagnosticEngine &Engine;

    /// How many tentative diagnostics there were when the transaction
    /// was opened.
    unsigned PrevDiagnostics;

    /// How many other transactions were open when this transaction was
    /// opened.
    unsigned Depth;

    /// Whether this transaction is currently open.
    bool IsOpen = true;

  public:
    DiagnosticTransaction(const DiagnosticTransaction &) = delete;
    DiagnosticTransaction &operator=(const DiagnosticTransaction &) = delete;

    explicit DiagnosticTransaction(DiagnosticEngine &engine)
      : Engine(engine),
        PrevDiagnostics(Engine.TentativeDiagnostics.size()),
        Depth(Engine.TransactionCount),
        IsOpen(true)
    {
      Engine.TransactionCount++;
    }

    ~DiagnosticTransaction() {
      if (IsOpen) {
        commit();
      }

      if (Depth == 0) {
        Engine.TransactionStrings.clear();
        Engine.TransactionAllocator.Reset();
      }
    }

    bool hasErrors() const {
      ArrayRef<Diagnostic> diagnostics(Engine.TentativeDiagnostics.begin() +
                                           PrevDiagnostics,
                                       Engine.TentativeDiagnostics.end());

      for (auto &diagnostic : diagnostics) {
        auto behavior = Engine.state.determineBehavior(diagnostic);
        if (behavior == DiagnosticBehavior::Fatal ||
            behavior == DiagnosticBehavior::Error)
          return true;
      }

      return false;
    }

    /// Abort and close this transaction and erase all diagnostics
    /// record while it was open.
    void abort() {
      close();
      Engine.TentativeDiagnostics.erase(
        Engine.TentativeDiagnostics.begin() + PrevDiagnostics,
        Engine.TentativeDiagnostics.end());
    }

    /// Commit and close this transaction. If this is the top-level
    /// transaction, emit any diagnostics that were recorded while it was open.
    void commit() {
      close();
      if (Depth == 0) {
        assert(PrevDiagnostics == 0);
        Engine.emitTentativeDiagnostics();
      }
    }

  private:
    void close() {
      assert(IsOpen && "only open transactions may be closed");
      IsOpen = false;
      Engine.TransactionCount--;
      assert(Depth == Engine.TransactionCount &&
             "transactions must be closed LIFO");
    }
  };

  /// Represents a diagnostic transaction which constructs a compound diagnostic
  /// from any diagnostics emitted inside. A compound diagnostic consists of a
  /// parent error, warning, or remark followed by a variable number of child
  /// notes. The semantics are otherwise the same as a regular
  /// DiagnosticTransaction.
  class CompoundDiagnosticTransaction : public DiagnosticTransaction {
  public:
    explicit CompoundDiagnosticTransaction(DiagnosticEngine &engine)
        : DiagnosticTransaction(engine) {}

    ~CompoundDiagnosticTransaction() {
      if (IsOpen) {
        commit();
      }

      if (Depth == 0) {
        Engine.TransactionStrings.clear();
        Engine.TransactionAllocator.Reset();
      }
    }

    void commit() {
      assert(PrevDiagnostics < Engine.TentativeDiagnostics.size() &&
             "CompoundDiagnosticTransaction must contain at least one diag");

      // The first diagnostic is assumed to be the parent. If this is not an
      // error or warning, we'll assert later when trying to add children.
      Diagnostic &parent = Engine.TentativeDiagnostics[PrevDiagnostics];

      // Associate the children with the parent.
      for (auto diag =
               Engine.TentativeDiagnostics.begin() + PrevDiagnostics + 1;
           diag != Engine.TentativeDiagnostics.end(); ++diag) {
        diag->setIsChildNote(true);
        parent.addChildNote(std::move(*diag));
      }

      // Erase the children, they'll be emitted alongside their parent.
      Engine.TentativeDiagnostics.erase(Engine.TentativeDiagnostics.begin() +
                                            PrevDiagnostics + 1,
                                        Engine.TentativeDiagnostics.end());

      DiagnosticTransaction::commit();
    }
  };

  /// Represents a queue of diagnostics that have their emission delayed until
  /// the queue is destroyed. This is similar to DiagnosticTransaction, but
  /// with a few key differences:
  /// 
  /// - The queue maintains its own diagnostic engine (which may be accessed
  ///   through `getDiags()`), and diagnostics must be specifically emitted
  ///   using that engine to be enqueued.
  /// - It allows for non-LIFO transactions, as each queue operates
  ///   independently.
  /// - A queue can be drained multiple times without having to be recreated
  ///   (unlike DiagnosticTransaction, it has no concept of "closing").
  ///
  /// Note you may add DiagnosticTransactions to the queue's diagnostic engine,
  /// but they must be closed before attempting to clear or emit the diagnostics
  /// in the queue.
  ///
  class DiagnosticQueue final {
    /// The underlying diagnostic engine that the diagnostics will be emitted
    /// by.
    DiagnosticEngine &UnderlyingEngine;

    /// A temporary engine used to queue diagnostics.
    DiagnosticEngine QueueEngine;

    /// Whether the queued diagnostics should be emitted on the destruction of
    /// the queue, or whether they should be cleared.
    bool EmitOnDestruction;

  public:
    DiagnosticQueue(const DiagnosticQueue &) = delete;
    DiagnosticQueue &operator=(const DiagnosticQueue &) = delete;

    /// Create a new diagnostic queue with a given engine to forward the
    /// diagnostics to.
    explicit DiagnosticQueue(DiagnosticEngine &engine, bool emitOnDestruction)
        : UnderlyingEngine(engine), QueueEngine(engine.SourceMgr),
          EmitOnDestruction(emitOnDestruction) {
      // Open a transaction to avoid emitting any diagnostics for the temporary
      // engine.
      QueueEngine.TransactionCount++;
    }

    /// Retrieve the engine which may be used to enqueue diagnostics.
    DiagnosticEngine &getDiags() { return QueueEngine; }

    /// Retrieve the underlying engine which will receive the diagnostics.
    DiagnosticEngine &getUnderlyingDiags() const { return UnderlyingEngine; }

    /// Clear this queue and erase all diagnostics recorded.
    void clear() {
      assert(QueueEngine.TransactionCount == 1 &&
             "Must close outstanding DiagnosticTransactions before draining");
      QueueEngine.clearTentativeDiagnostics();
    }

    /// Emit all the diagnostics recorded by this queue.
    void emit() {
      assert(QueueEngine.TransactionCount == 1 &&
             "Must close outstanding DiagnosticTransactions before draining");
      QueueEngine.forwardTentativeDiagnosticsTo(UnderlyingEngine);
    }

    ~DiagnosticQueue() {
      if (EmitOnDestruction) {
        emit();
      } else {
        clear();
      }
      QueueEngine.TransactionCount--;
    }
  };

  inline void
  DiagnosticEngine::diagnoseWithNotes(InFlightDiagnostic parentDiag,
                                      llvm::function_ref<void(void)> builder) {
    CompoundDiagnosticTransaction transaction(*this);
    parentDiag.flush();
    builder();
  }

  void printClangDeclName(const clang::NamedDecl *ND, llvm::raw_ostream &os);

  /// Temporary on-stack storage and unescaping for encoded diagnostic
  /// messages.
  class EncodedDiagnosticMessage {
    llvm::SmallString<128> Buf;

  public:
    /// \param S A string with an encoded message
    EncodedDiagnosticMessage(StringRef S);

    /// The unescaped message to display to the user.
    const StringRef Message;
  };

/// Returns a value that can be used to select between accessor kinds in
/// diagnostics.
///
/// This is correlated with diag::availability_deprecated and others.
std::pair<unsigned, DeclName>
getAccessorKindAndNameForDiagnostics(const ValueDecl *D);

/// Retrieve the macro name for a generated source info that represents
/// a macro expansion.
DeclName getGeneratedSourceInfoMacroName(const GeneratedSourceInfo &info);

} // end namespace swift

#endif
