//===--- Attr.h - Swift Language Attribute ASTs -----------------*- C++ -*-===//
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
// This file defines classes related to declaration attributes.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_ATTR_H
#define SWIFT_ATTR_H

#include "swift/AST/ASTAllocated.h"
#include "swift/AST/AttrKind.h"
#include "swift/AST/AutoDiff.h"
#include "swift/AST/ConcreteDeclRef.h"
#include "swift/AST/DeclNameLoc.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/KnownProtocols.h"
#include "swift/AST/MacroDeclaration.h"
#include "swift/AST/Ownership.h"
#include "swift/AST/PlatformKind.h"
#include "swift/AST/StorageImpl.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/EnumTraits.h"
#include "swift/Basic/InlineBitfield.h"
#include "swift/Basic/Located.h"
#include "swift/Basic/OptimizationMode.h"
#include "swift/Basic/Range.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Basic/SourceLoc.h"
#include "swift/Basic/UUID.h"
#include "swift/Basic/Version.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/TrailingObjects.h"
#include "llvm/Support/VersionTuple.h"
#include <optional>

namespace swift {
class ArgumentList;
class ASTPrinter;
class ASTContext;
struct PrintOptions;
class CustomAttr;
class Decl;
class DeclRefTypeRepr;
class AbstractFunctionDecl;
class FuncDecl;
class ClassDecl;
class AccessorDecl;
class GenericFunctionType;
class LazyConformanceLoader;
class LazyMemberLoader;
class ModuleDecl;
class PatternBindingInitializer;
class TrailingWhereClause;
class TypeExpr;
class UnqualifiedIdentTypeRepr;

class alignas(1 << AttrAlignInBits) AttributeBase
    : public ASTAllocated<AttributeBase> {
public:
  /// The location of the '@'.
  const SourceLoc AtLoc;

  /// The source range of the attribute.
  const SourceRange Range;

  /// The location of the attribute.
  SourceLoc getLocation() const { return Range.Start; }
  SourceLoc getStartLoc() const { return Range.Start; }
  SourceLoc getEndLoc() const { return Range.End; }

  /// Return the source range of the attribute.
  SourceRange getRange() const { return Range; }

  SourceRange getRangeWithAt() const {
    if (AtLoc.isValid())
      return {AtLoc, Range.End};
    return Range;
  }

  AttributeBase(const AttributeBase &) = delete;

protected:
  AttributeBase(SourceLoc AtLoc, SourceRange Range)
    : AtLoc(AtLoc), Range(Range) {}
};

class DeclAttributes;
enum class DeclKind : uint8_t;

  /// Represents one declaration attribute.
class DeclAttribute : public AttributeBase {
  friend class DeclAttributes;

protected:
  // clang-format off
  union {
    uint64_t OpaqueBits;

    SWIFT_INLINE_BITFIELD_BASE(DeclAttribute, bitmax(NumDeclAttrKindBits,8)+1+1+1,
      Kind : bitmax(NumDeclAttrKindBits,8),
      // Whether this attribute was implicitly added.
      Implicit : 1,

      Invalid : 1,

      /// Whether the attribute was created by an access note.
      AddedByAccessNote : 1
    );

    SWIFT_INLINE_BITFIELD(ObjCAttr, DeclAttribute, 1+1,
      /// Whether this attribute has location information that trails the main
      /// record, which contains the locations of the parentheses and any names.
      HasTrailingLocationInfo : 1,

      /// Whether the name is implicit, produced as the result of caching.
      ImplicitName : 1
    );

    SWIFT_INLINE_BITFIELD(DynamicReplacementAttr, DeclAttribute, 1,
      /// Whether this attribute has location information that trails the main
      /// record, which contains the locations of the parentheses and any names.
      HasTrailingLocationInfo : 1
    );

    SWIFT_INLINE_BITFIELD(AbstractAccessControlAttr, DeclAttribute, 3,
      AccessLevel : 3
    );

    SWIFT_INLINE_BITFIELD_FULL(AlignmentAttr, DeclAttribute, 32,
      : NumPadBits,
      // The alignment value.
      Value : 32
    );

    SWIFT_INLINE_BITFIELD(ClangImporterSynthesizedTypeAttr, DeclAttribute, 1,
      kind : 1
    );

    SWIFT_INLINE_BITFIELD(EffectsAttr, DeclAttribute, NumEffectsKindBits,
      kind : NumEffectsKindBits
    );

    SWIFT_INLINE_BITFIELD(InlineAttr, DeclAttribute, NumInlineKindBits,
      kind : NumInlineKindBits
    );

    SWIFT_INLINE_BITFIELD(OptimizeAttr, DeclAttribute, NumOptimizationModeBits,
      mode : NumOptimizationModeBits
    );

    SWIFT_INLINE_BITFIELD(ReferenceOwnershipAttr, DeclAttribute,
                          NumReferenceOwnershipBits,
      ownership : NumReferenceOwnershipBits
    );

    SWIFT_INLINE_BITFIELD(SpecializeAttr, DeclAttribute, 1+1,
      exported : 1,
      kind : 1
    );

    SWIFT_INLINE_BITFIELD(ExposeAttr, DeclAttribute, NumExposureKindBits,
      kind : NumExposureKindBits
    );

    SWIFT_INLINE_BITFIELD(ExternAttr, DeclAttribute, NumExternKindBits,
      kind : NumExternKindBits
    );

    SWIFT_INLINE_BITFIELD(SynthesizedProtocolAttr, DeclAttribute, 1,
      isUnchecked : 1
    );

    SWIFT_INLINE_BITFIELD(ObjCImplementationAttr, DeclAttribute, 2,
      isCategoryNameInvalid : 1,
      isEarlyAdopter : 1
    );

    SWIFT_INLINE_BITFIELD(NonisolatedAttr, DeclAttribute, 1,
      isUnsafe : 1
    );

    SWIFT_INLINE_BITFIELD_FULL(AllowFeatureSuppressionAttr, DeclAttribute, 1+31,
      : NumPadBits,
      Inverted : 1,

      NumFeatures : 31
    );
  } Bits;
  // clang-format on

  DeclAttribute *Next = nullptr;

  DeclAttribute(DeclAttrKind DK, SourceLoc AtLoc, SourceRange Range,
                bool Implicit) : AttributeBase(AtLoc, Range) {
    Bits.OpaqueBits = 0;
    Bits.DeclAttribute.Kind = static_cast<unsigned>(DK);
    Bits.DeclAttribute.Implicit = Implicit;
    Bits.DeclAttribute.Invalid = false;
    Bits.DeclAttribute.AddedByAccessNote = false;
  }

private:
  // NOTE: We cannot use DeclKind due to layering. Even if we could, there is no
  // guarantee that the first DeclKind starts at zero. This is only used to
  // build "OnXYZ" flags.
  enum class DeclKindIndex : unsigned {
#define DECL(Name, _) Name,
#define LAST_DECL(Name) Last_Decl = Name
#include "swift/AST/DeclNodes.def"
  };

public:
  enum DeclAttrOptions : uint64_t {
    // There is one entry for each DeclKind, and some higher level buckets
    // below. These are used in Attr.def to control which kinds of declarations
    // an attribute can be attached to.
#define DECL(Name, _) On##Name = 1ull << unsigned(DeclKindIndex::Name),
#include "swift/AST/DeclNodes.def"

    // Abstract class aggregations for use in Attr.def.
    OnValue = 0
#define DECL(Name, _)
#define VALUE_DECL(Name, _) |On##Name
#include "swift/AST/DeclNodes.def"
    ,

    OnNominalType = 0
#define DECL(Name, _)
#define NOMINAL_TYPE_DECL(Name, _) |On##Name
#include "swift/AST/DeclNodes.def"
    ,
    OnConcreteNominalType = OnNominalType & ~OnProtocol,
    OnGenericType = OnNominalType | OnTypeAlias,

    OnAbstractFunction = 0
#define DECL(Name, _)
#define ABSTRACT_FUNCTION_DECL(Name, _) |On##Name
#include "swift/AST/DeclNodes.def"
    ,

    OnOperator = 0
#define DECL(Name, _)
#define OPERATOR_DECL(Name, _) |On##Name
#include "swift/AST/DeclNodes.def"
    ,

    OnAnyDecl = 0
#define DECL(Name, _) |On##Name
#include "swift/AST/DeclNodes.def"
    ,

    /// True if multiple instances of this attribute are allowed on a single
    /// declaration.
    AllowMultipleAttributes = 1ull << (unsigned(DeclKindIndex::Last_Decl) + 1),

    /// True if this is a decl modifier - i.e., that it should not be spelled
    /// with an @.
    DeclModifier = 1ull << (unsigned(DeclKindIndex::Last_Decl) + 2),

    /// True if this is a long attribute that should be printed on its own line.
    ///
    /// Currently has no effect on DeclModifier attributes.
    LongAttribute = 1ull << (unsigned(DeclKindIndex::Last_Decl) + 3),

    /// True if this shouldn't be serialized.
    NotSerialized = 1ull << (unsigned(DeclKindIndex::Last_Decl) + 4),
    
    /// True if this attribute is only valid when parsing a .sil file.
    SILOnly = 1ull << (unsigned(DeclKindIndex::Last_Decl) + 5),

    /// The attribute should be reported by parser as unknown.
    RejectByParser = 1ull << (unsigned(DeclKindIndex::Last_Decl) + 6),

    /// Whether client code cannot use the attribute.
    UserInaccessible = 1ull << (unsigned(DeclKindIndex::Last_Decl) + 7),

    /// Whether adding this attribute can break API
    APIBreakingToAdd = 1ull << (unsigned(DeclKindIndex::Last_Decl) + 8),

    /// Whether removing this attribute can break API
    APIBreakingToRemove = 1ull << (unsigned(DeclKindIndex::Last_Decl) + 9),

    /// Whether adding this attribute can break ABI
    ABIBreakingToAdd = 1ull << (unsigned(DeclKindIndex::Last_Decl) + 10),

    /// Whether removing this attribute can break ABI
    ABIBreakingToRemove = 1ull << (unsigned(DeclKindIndex::Last_Decl) + 11),

    /// The opposite of APIBreakingToAdd
    APIStableToAdd = 1ull << (unsigned(DeclKindIndex::Last_Decl) + 12),

    /// The opposite of APIBreakingToRemove
    APIStableToRemove = 1ull << (unsigned(DeclKindIndex::Last_Decl) + 13),

    /// The opposite of ABIBreakingToAdd
    ABIStableToAdd = 1ull << (unsigned(DeclKindIndex::Last_Decl) + 14),

    /// The opposite of ABIBreakingToRemove
    ABIStableToRemove = 1ull << (unsigned(DeclKindIndex::Last_Decl) + 15),

    /// Whether this attribute is only valid when concurrency is enabled.
    ConcurrencyOnly = 1ull << (unsigned(DeclKindIndex::Last_Decl) + 16),

    /// Whether this attribute is valid on additional decls in ClangImporter.
    OnAnyClangDecl = 1ull << (unsigned(DeclKindIndex::Last_Decl) + 17),
  };

  static_assert(
      (unsigned(DeclKindIndex::Last_Decl) + 17) < 64,
      "Overflow decl attr options bitfields");

  LLVM_READNONE
  static uint64_t getOptions(DeclAttrKind DK);

  uint64_t getOptions() const {
    return getOptions(getKind());
  }

  /// Prints this attribute (if applicable), returning `true` if anything was
  /// printed.
  bool printImpl(ASTPrinter &Printer, const PrintOptions &Options,
                 const Decl *D = nullptr) const;

public:
  DeclAttrKind getKind() const {
    return static_cast<DeclAttrKind>(Bits.DeclAttribute.Kind);
  }

  /// Whether this attribute was implicitly added.
  bool isImplicit() const { return Bits.DeclAttribute.Implicit; }

  /// Set whether this attribute was implicitly added.
  void setImplicit(bool Implicit = true) {
    Bits.DeclAttribute.Implicit = Implicit;
  }

  /// Returns true if this attribute was find to be invalid in some way by
  /// semantic analysis.  In that case, the attribute should not be considered,
  /// the attribute node should be only used to retrieve source information.
  bool isInvalid() const { return Bits.DeclAttribute.Invalid; }
  void setInvalid() { Bits.DeclAttribute.Invalid = true; }

  bool isValid() const { return !isInvalid(); }


  /// Determine whether this attribute was added by an access note. If it was,
  /// the compiler will generally recover from failures involving this attribute
  /// as though it is not present.
  bool getAddedByAccessNote() const {
    return Bits.DeclAttribute.AddedByAccessNote;
  }

  void setAddedByAccessNote(bool accessNote = true) {
    Bits.DeclAttribute.AddedByAccessNote = accessNote;
  }

  /// Returns the address of the next pointer field.
  /// Used for object deserialization.
  DeclAttribute **getMutableNext() {
    return &Next;
  }

  /// Print the attribute to the provided ASTPrinter.
  void print(ASTPrinter &Printer, const PrintOptions &Options,
             const Decl *D = nullptr) const;

  /// Print the attribute to the provided stream.
  void print(llvm::raw_ostream &OS, const Decl *D = nullptr) const;

  /// Returns true if this attribute can appear on the specified decl.  This is
  /// controlled by the flags in Attr.def.
  bool canAppearOnDecl(const Decl *D) const {
    return canAttributeAppearOnDecl(getKind(), D);
  }

  LLVM_READONLY
  static bool canAttributeAppearOnDecl(DeclAttrKind DK, const Decl *D);

  /// Returns true if multiple instances of an attribute kind
  /// can appear on a declaration.
  static bool allowMultipleAttributes(DeclAttrKind DK) {
    return getOptions(DK) & AllowMultipleAttributes;
  }

  bool isLongAttribute() const {
    return isLongAttribute(getKind());
  }
  static bool isLongAttribute(DeclAttrKind DK) {
    return getOptions(DK) & LongAttribute;
  }

  static bool shouldBeRejectedByParser(DeclAttrKind DK) {
    return getOptions(DK) & RejectByParser;
  }

  static bool isSilOnly(DeclAttrKind DK) {
    return getOptions(DK) & SILOnly;
  }

  static bool isConcurrencyOnly(DeclAttrKind DK) {
    return getOptions(DK) & ConcurrencyOnly;
  }

  static bool isUserInaccessible(DeclAttrKind DK) {
    return getOptions(DK) & UserInaccessible;
  }

  static bool isAddingBreakingABI(DeclAttrKind DK) {
    return getOptions(DK) & ABIBreakingToAdd;
  }

#define DECL_ATTR(_, CLASS, OPTIONS, ...)                                                         \
  static constexpr bool isOptionSetFor##CLASS(DeclAttrOptions Bit) {                              \
    return (OPTIONS) & Bit;                                                                       \
  }
#include "swift/AST/DeclAttr.def"

  static bool isAddingBreakingAPI(DeclAttrKind DK) {
    return getOptions(DK) & APIBreakingToAdd;
  }

  static bool isRemovingBreakingABI(DeclAttrKind DK) {
    return getOptions(DK) & ABIBreakingToRemove;
  }
  static bool isRemovingBreakingAPI(DeclAttrKind DK) {
    return getOptions(DK) & APIBreakingToRemove;
  }

  bool isDeclModifier() const {
    return isDeclModifier(getKind());
  }
  static bool isDeclModifier(DeclAttrKind DK) {
    return getOptions(DK) & DeclModifier;
  }

  static bool isOnParam(DeclAttrKind DK) {
    return getOptions(DK) & OnParam;
  }

  static bool isOnFunc(DeclAttrKind DK) {
    return getOptions(DK) & OnFunc;
  }

  static bool isOnClass(DeclAttrKind DK) {
    return getOptions(DK) & OnClass;
  }

  static bool isNotSerialized(DeclAttrKind DK) {
    return getOptions(DK) & NotSerialized;
  }
  bool isNotSerialized() const {
    return isNotSerialized(getKind());
  }

  LLVM_READNONE
  static bool canAttributeAppearOnDeclKind(DeclAttrKind DAK, DeclKind DK);

  /// Returns the source name of the attribute, without the @ or any arguments.
  StringRef getAttrName() const;

  /// Given a name like "inline", return the decl attribute ID that corresponds
  /// to it.  Note that this is a many-to-one mapping, and that the identifier
  /// passed in may only be the first portion of the attribute (e.g. in the case
  /// of the 'unowned(unsafe)' attribute, the string passed in is 'unowned'.
  ///
  /// Also note that this recognizes both attributes like '@inline' (with no @)
  /// and decl modifiers like 'final'.
  ///
  static std::optional<DeclAttrKind> getAttrKindFromString(StringRef Str);

  static DeclAttribute *createSimple(const ASTContext &context,
                                     DeclAttrKind kind, SourceLoc atLoc,
                                     SourceLoc attrLoc);
};

/// Describes a "simple" declaration attribute that carries no data.
template<DeclAttrKind Kind>
class SimpleDeclAttr : public DeclAttribute {
public:
  SimpleDeclAttr(bool IsImplicit)
    : DeclAttribute(Kind, SourceLoc(), SourceLoc(), IsImplicit) {}

  SimpleDeclAttr(SourceLoc AtLoc, SourceLoc NameLoc)
    : DeclAttribute(Kind, AtLoc,
                    SourceRange(AtLoc.isValid() ? AtLoc : NameLoc, NameLoc),
                    /*Implicit=*/false) { }
  SimpleDeclAttr(SourceLoc NameLoc)
    : DeclAttribute(Kind, SourceLoc(), SourceRange(NameLoc, NameLoc),
                    /*Implicit=*/false) { }

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == Kind;
  }
};

// Declare typedefs for all of the simple declaration attributes.
#define SIMPLE_DECL_ATTR(_, CLASS, ...)                                        \
  typedef SimpleDeclAttr<DeclAttrKind::CLASS> CLASS##Attr;
#include "swift/AST/DeclAttr.def"

/// Defines the @_silgen_name attribute.
class SILGenNameAttr : public DeclAttribute {
public:
  SILGenNameAttr(StringRef Name, bool Raw, SourceLoc AtLoc, SourceRange Range,
                 bool Implicit)
      : DeclAttribute(DeclAttrKind::SILGenName, AtLoc, Range, Implicit),
        Name(Name), Raw(Raw) {}

  SILGenNameAttr(StringRef Name, bool Raw, bool Implicit)
    : SILGenNameAttr(Name, Raw, SourceLoc(), SourceRange(), Implicit) {}

  /// The symbol name.
  const StringRef Name;

  /// If true, the name is not to be mangled (underscore prefix on Darwin)
  bool Raw;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::SILGenName;
  }
};

/// Defines the @_section attribute.
class SectionAttr : public DeclAttribute {
public:
  SectionAttr(StringRef Name, SourceLoc AtLoc, SourceRange Range, bool Implicit)
      : DeclAttribute(DeclAttrKind::Section, AtLoc, Range, Implicit),
        Name(Name) {}

  SectionAttr(StringRef Name, bool Implicit)
    : SectionAttr(Name, SourceLoc(), SourceRange(), Implicit) {}

  /// The section name.
  const StringRef Name;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::Section;
  }
};

/// Defines the @_cdecl attribute.
class CDeclAttr : public DeclAttribute {
public:
  CDeclAttr(StringRef Name, SourceLoc AtLoc, SourceRange Range, bool Implicit)
      : DeclAttribute(DeclAttrKind::CDecl, AtLoc, Range, Implicit), Name(Name) {
  }

  CDeclAttr(StringRef Name, bool Implicit)
    : CDeclAttr(Name, SourceLoc(), SourceRange(), Implicit) {}

  /// The symbol name.
  const StringRef Name;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::CDecl;
  }
};

/// Defines the @_semantics attribute.
class SemanticsAttr : public DeclAttribute {
public:
  SemanticsAttr(StringRef Value, SourceLoc AtLoc, SourceRange Range,
                bool Implicit)
      : DeclAttribute(DeclAttrKind::Semantics, AtLoc, Range, Implicit),
        Value(Value) {}

  SemanticsAttr(StringRef Value, bool Implicit)
  : SemanticsAttr(Value, SourceLoc(), SourceRange(), Implicit) {}

  /// The semantics tag value.
  const StringRef Value;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::Semantics;
  }
};

/// Defines the @_alignment attribute.
class AlignmentAttr : public DeclAttribute {
public:
  AlignmentAttr(unsigned Value, SourceLoc AtLoc, SourceRange Range,
                bool Implicit)
      : DeclAttribute(DeclAttrKind::Alignment, AtLoc, Range, Implicit) {
    Bits.AlignmentAttr.Value = Value;
  }

  unsigned getValue() const { return Bits.AlignmentAttr.Value; }
  
  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::Alignment;
  }
};

/// Defines the @_swift_native_objc_runtime_base attribute.
///
/// This attribute indicates a class that should be treated semantically
/// as a native Swift root class, but which inherits a specific Objective-C
/// class at runtime. For most classes this is the runtime's "SwiftObject"
/// root class. The compiler does not need to know about the class; it's the
/// build system's responsibility to link against the ObjC code that implements
/// the root class, and the ObjC implementation's responsibility to ensure
/// instances begin with a Swift-refcounting-compatible object header and
/// override all the necessary NSObject refcounting methods.
class SwiftNativeObjCRuntimeBaseAttr : public DeclAttribute {
public:
  SwiftNativeObjCRuntimeBaseAttr(Identifier BaseClassName, SourceLoc AtLoc,
                                 SourceRange Range, bool Implicit)
      : DeclAttribute(DeclAttrKind::SwiftNativeObjCRuntimeBase, AtLoc, Range,
                      Implicit),
        BaseClassName(BaseClassName) {}

  // The base class's name.
  const Identifier BaseClassName;
  
  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::SwiftNativeObjCRuntimeBase;
  }
};

/// Determine the result of comparing an availability attribute to a specific
/// platform or language version.
enum class AvailableVersionComparison {
  /// The entity is guaranteed to be available.
  Available,

  /// The entity is never available.
  Unavailable,

  /// The entity might be unavailable at runtime, because it was introduced
  /// after the requested minimum platform version.
  PotentiallyUnavailable,

  /// The entity has been obsoleted.
  Obsoleted,
};

/// Describes the platform-agnostic availability of a declaration.
enum class PlatformAgnosticAvailabilityKind {
  /// The associated availability attribute is not platform-agnostic.
  None,
  /// The declaration is deprecated, but can still be used.
  Deprecated,
  /// The declaration is unavailable in Swift, specifically
  UnavailableInSwift,
  /// The declaration is available in some but not all versions
  /// of Swift, as specified by the VersionTuple members.
  SwiftVersionSpecific,
  /// The declaration is available in some but not all versions
  /// of SwiftPM's PackageDescription library, as specified by
  /// the VersionTuple members.
  PackageDescriptionVersionSpecific,
  /// The declaration is unavailable for other reasons.
  Unavailable,
  /// The declaration is unavailable from asynchronous contexts
  NoAsync,
};

/// Defines the @available attribute.
class AvailableAttr : public DeclAttribute {
public:
#define INIT_VER_TUPLE(X) X(X.empty() ? std::optional<llvm::VersionTuple>() : X)

  AvailableAttr(SourceLoc AtLoc, SourceRange Range, PlatformKind Platform,
                StringRef Message, StringRef Rename, ValueDecl *RenameDecl,
                const llvm::VersionTuple &Introduced,
                SourceRange IntroducedRange,
                const llvm::VersionTuple &Deprecated,
                SourceRange DeprecatedRange,
                const llvm::VersionTuple &Obsoleted, SourceRange ObsoletedRange,
                PlatformAgnosticAvailabilityKind PlatformAgnostic,
                bool Implicit, bool IsSPI)
      : DeclAttribute(DeclAttrKind::Available, AtLoc, Range, Implicit),
        Message(Message), Rename(Rename), RenameDecl(RenameDecl),
        INIT_VER_TUPLE(Introduced), IntroducedRange(IntroducedRange),
        INIT_VER_TUPLE(Deprecated), DeprecatedRange(DeprecatedRange),
        INIT_VER_TUPLE(Obsoleted), ObsoletedRange(ObsoletedRange),
        PlatformAgnostic(PlatformAgnostic), Platform(Platform), IsSPI(IsSPI) {}

#undef INIT_VER_TUPLE

  /// The optional message.
  const StringRef Message;

  /// An optional replacement string to emit in a fixit.  This allows simple
  /// declaration renames to be applied by Xcode.
  ///
  /// This should take the form of an operator, identifier, or full function
  /// name, optionally with a prefixed type, similar to the syntax used for
  /// the `NS_SWIFT_NAME` annotation in Objective-C.
  const StringRef Rename;

  /// The declaration referred to by \c Rename. Note that this is only set for
  /// deserialized attributes or inferred attributes from ObjectiveC code.
  /// \c ValueDecl::getRenamedDecl should be used to find the declaration
  /// corresponding to \c Rename.
  ValueDecl *RenameDecl;

  /// Indicates when the symbol was introduced.
  const std::optional<llvm::VersionTuple> Introduced;

  /// Indicates where the Introduced version was specified.
  const SourceRange IntroducedRange;

  /// Indicates when the symbol was deprecated.
  const std::optional<llvm::VersionTuple> Deprecated;

  /// Indicates where the Deprecated version was specified.
  const SourceRange DeprecatedRange;

  /// Indicates when the symbol was obsoleted.
  const std::optional<llvm::VersionTuple> Obsoleted;

  /// Indicates where the Obsoleted version was specified.
  const SourceRange ObsoletedRange;

  /// Indicates if the declaration has platform-agnostic availability.
  const PlatformAgnosticAvailabilityKind PlatformAgnostic;

  /// The platform of the availability.
  const PlatformKind Platform;

  /// Whether this is available as SPI.
  const bool IsSPI;

  /// Whether this is a language-version-specific entity.
  bool isLanguageVersionSpecific() const;

  /// Whether this is a PackageDescription version specific entity.
  bool isPackageDescriptionVersionSpecific() const;

  /// Whether this is an unconditionally unavailable entity.
  bool isUnconditionallyUnavailable() const;

  /// Whether this is an unconditionally deprecated entity.
  bool isUnconditionallyDeprecated() const;

  /// Whether this is a noasync attribute.
  bool isNoAsync() const;

  /// Returns the platform-agnostic availability.
  PlatformAgnosticAvailabilityKind getPlatformAgnosticAvailability() const {
    return PlatformAgnostic;
  }

  /// Determine if a given declaration should be considered unavailable given
  /// the current settings.
  ///
  /// \returns The attribute responsible for making the declaration unavailable.
  static const AvailableAttr *isUnavailable(const Decl *D);

  /// Returns true if the availability applies to a specific
  /// platform.
  bool hasPlatform() const {
    return Platform != PlatformKind::none;
  }

  /// Returns the string for the platform of the attribute.
  StringRef platformString() const {
    return swift::platformString(Platform);
  }

  /// Returns the human-readable string for the platform of the attribute.
  StringRef prettyPlatformString() const {
    return swift::prettyPlatformString(Platform);
  }

  /// Returns true if this attribute is active given the current platform.
  bool isActivePlatform(const ASTContext &ctx) const;

  /// Returns the active version from the AST context corresponding to
  /// the available kind. For example, this will return the effective language
  /// version for swift version-specific availability kind, PackageDescription
  /// version for PackageDescription version-specific availability.
  llvm::VersionTuple getActiveVersion(const ASTContext &ctx) const;

  /// Compare this attribute's version information against the platform or
  /// language version (assuming the this attribute pertains to the active
  /// platform).
  AvailableVersionComparison getVersionAvailability(const ASTContext &ctx) const;

  /// Create an AvailableAttr that indicates specific availability
  /// for all platforms.
  static AvailableAttr *
  createPlatformAgnostic(ASTContext &C, StringRef Message, StringRef Rename = "",
                      PlatformAgnosticAvailabilityKind Reason
                         = PlatformAgnosticAvailabilityKind::Unavailable,
                         llvm::VersionTuple Obsoleted
                         = llvm::VersionTuple());

  /// Create an AvailableAttr that indicates the given \p AsyncFunc should be
  /// preferentially used in async contexts
  static AvailableAttr *createForAlternative(ASTContext &C,
                                             AbstractFunctionDecl *AsyncFunc);

  AvailableAttr *clone(ASTContext &C, bool implicit) const;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::Available;
  }
};

/// Indicates that the given declaration is visible to Objective-C.
class ObjCAttr final : public DeclAttribute,
    private llvm::TrailingObjects<ObjCAttr, SourceLoc> {
  friend TrailingObjects;

  /// The Objective-C name associated with this entity, stored in its opaque
  /// representation so that we can use null as an indicator for "no name".
  void *NameData;

  /// Create an implicit @objc attribute with the given (optional) name.
  explicit ObjCAttr(std::optional<ObjCSelector> name, bool implicitName)
      : DeclAttribute(DeclAttrKind::ObjC, SourceLoc(), SourceRange(),
                      /*Implicit=*/true),
        NameData(nullptr) {
    Bits.ObjCAttr.HasTrailingLocationInfo = false;
    Bits.ObjCAttr.ImplicitName = implicitName;

    if (name) {
      NameData = name->getOpaqueValue();
    }
  }

  /// Create an @objc attribute written in the source.
  ObjCAttr(SourceLoc atLoc, SourceRange baseRange,
           std::optional<ObjCSelector> name, SourceRange parenRange,
           ArrayRef<SourceLoc> nameLocs);

  /// Determine whether this attribute has trailing location information.
  bool hasTrailingLocationInfo() const {
    return Bits.ObjCAttr.HasTrailingLocationInfo;
  }

  /// Retrieve the trailing location information.
  MutableArrayRef<SourceLoc> getTrailingLocations() {
    assert(hasTrailingLocationInfo() && "No trailing location information");
    unsigned length = 2;
    if (auto name = getName())
      length += name->getNumSelectorPieces();
    return {getTrailingObjects<SourceLoc>(), length};
  }

  /// Retrieve the trailing location information.
  ArrayRef<SourceLoc> getTrailingLocations() const {
    assert(hasTrailingLocationInfo() && "No trailing location information");
    unsigned length = 2;
    if (auto name = getName())
      length += name->getNumSelectorPieces();
    return {getTrailingObjects<SourceLoc>(), length};
  }

public:
  /// Create implicit ObjC attribute with a given (optional) name.
  static ObjCAttr *create(ASTContext &Ctx, std::optional<ObjCSelector> name,
                          bool implicitName);

  /// Create an unnamed Objective-C attribute, i.e., @objc.
  static ObjCAttr *createUnnamed(ASTContext &Ctx, SourceLoc AtLoc, 
                                 SourceLoc ObjCLoc);

  static ObjCAttr *createUnnamedImplicit(ASTContext &Ctx);

  /// Create a nullary Objective-C attribute, which has a single name
  /// with no colon following it.
  ///
  /// Note that a nullary Objective-C attribute may represent either a
  /// selector for a zero-parameter function or some other Objective-C
  /// entity, such as a class or protocol.
  static ObjCAttr *createNullary(ASTContext &Ctx, SourceLoc AtLoc, 
                                 SourceLoc ObjCLoc, SourceLoc LParenLoc, 
                                 SourceLoc NameLoc, Identifier Name,
                                 SourceLoc RParenLoc);

  /// Create an implicit nullary Objective-C attribute, which has a
  /// single name with no colon following it.
  ///
  /// Note that a nullary Objective-C attribute may represent either a
  /// selector for a zero-parameter function or some other Objective-C
  /// entity, such as a class or protocol.
  static ObjCAttr *createNullary(ASTContext &Ctx, Identifier Name, 
                                 bool isNameImplicit);

  /// Create a "selector" Objective-C attribute, which has some number
  /// of identifiers followed by colons.
  static ObjCAttr *createSelector(ASTContext &Ctx, SourceLoc AtLoc, 
                                  SourceLoc ObjCLoc, SourceLoc LParenLoc, 
                                  ArrayRef<SourceLoc> NameLocs,
                                  ArrayRef<Identifier> Names,
                                  SourceLoc RParenLoc);

  /// Create an implicit "selector" Objective-C attribute, which has
  /// some number of identifiers followed by colons.
  static ObjCAttr *createSelector(ASTContext &Ctx, ArrayRef<Identifier> Names,
                                  bool isNameImplicit);

  /// Determine whether this attribute has a name associated with it.
  bool hasName() const { return NameData != nullptr; }

  /// Retrieve the name of this entity, if specified.
  std::optional<ObjCSelector> getName() const {
    if (!hasName())
      return std::nullopt;

    return ObjCSelector::getFromOpaqueValue(NameData);
  }

  /// Determine whether the name associated with this attribute was
  /// implicit.
  bool isNameImplicit() const { return Bits.ObjCAttr.ImplicitName; }
  void setNameImplicit(bool newValue) { Bits.ObjCAttr.ImplicitName = newValue; }

  /// Set the name of this entity.
  void setName(ObjCSelector name, bool implicit) {
    // If we already have a name and we have location information, make sure
    // drop the location information rather than allowing it to corrupt our
    // state
    if (hasTrailingLocationInfo() &&
        (!hasName() ||
         getName()->getNumSelectorPieces() < name.getNumSelectorPieces())) {
      Bits.ObjCAttr.HasTrailingLocationInfo = false;
    }

    NameData = name.getOpaqueValue();
    Bits.ObjCAttr.ImplicitName = implicit;
  }

  /// Retrieve the source locations for the names in a non-implicit
  /// nullary or selector attribute.
  ArrayRef<SourceLoc> getNameLocs() const;

  /// Retrieve the location of the opening parentheses, if there is one.
  SourceLoc getLParenLoc() const;

  /// Retrieve the location of the closing parentheses, if there is one.
  SourceLoc getRParenLoc() const;

  /// Clone the given attribute, producing an implicit copy of the
  /// original without source location information.
  ObjCAttr *clone(ASTContext &context) const;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::ObjC;
  }
};

class MainTypeAttr final : public DeclAttribute {
public:
  MainTypeAttr(bool isImplicit)
      : DeclAttribute(DeclAttrKind::MainType, SourceLoc(), SourceLoc(),
                      isImplicit) {}

  MainTypeAttr(SourceLoc AtLoc, SourceLoc NameLoc)
      : DeclAttribute(DeclAttrKind::MainType, AtLoc,
                      SourceRange(AtLoc.isValid() ? AtLoc : NameLoc, NameLoc),
                      /*Implicit=*/false) {}

  MainTypeAttr(SourceLoc NameLoc)
      : DeclAttribute(DeclAttrKind::MainType, SourceLoc(),
                      SourceRange(NameLoc, NameLoc),
                      /*Implicit=*/false) {}

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::MainType;
  }
};

class PrivateImportAttr final
: public DeclAttribute {
  StringRef SourceFile;

  PrivateImportAttr(SourceLoc atLoc, SourceRange baseRange,
                    StringRef sourceFile, SourceRange parentRange);

public:
  static PrivateImportAttr *create(ASTContext &Ctxt, SourceLoc AtLoc,
                                   SourceLoc PrivateLoc, SourceLoc LParenLoc,
                                   StringRef sourceFile, SourceLoc RParenLoc);

  StringRef getSourceFile() const {
    return SourceFile;
  }
  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::PrivateImport;
  }
};

/// The @_dynamicReplacement(for:) attribute.
class DynamicReplacementAttr final
    : public DeclAttribute,
      private llvm::TrailingObjects<DynamicReplacementAttr, SourceLoc> {
  friend TrailingObjects;
  friend class DynamicallyReplacedDeclRequest;

  DeclNameRef ReplacedFunctionName;
  LazyMemberLoader *Resolver = nullptr;
  uint64_t ResolverContextData;

  /// Create an @_dynamicReplacement(for:) attribute written in the source.
  DynamicReplacementAttr(SourceLoc atLoc, SourceRange baseRange,
                         DeclNameRef replacedFunctionName,
                         SourceRange parenRange);

  DynamicReplacementAttr(DeclNameRef name, AbstractFunctionDecl *f)
      : DeclAttribute(DeclAttrKind::DynamicReplacement, SourceLoc(),
                      SourceRange(),
                      /*Implicit=*/false),
        ReplacedFunctionName(name), Resolver(nullptr), ResolverContextData(0) {
    Bits.DynamicReplacementAttr.HasTrailingLocationInfo = false;
  }

  DynamicReplacementAttr(DeclNameRef name, LazyMemberLoader *Resolver = nullptr,
                         uint64_t Data = 0)
      : DeclAttribute(DeclAttrKind::DynamicReplacement, SourceLoc(),
                      SourceRange(),
                      /*Implicit=*/false),
        ReplacedFunctionName(name), Resolver(Resolver),
        ResolverContextData(Data) {
    Bits.DynamicReplacementAttr.HasTrailingLocationInfo = false;
  }

  /// Retrieve the trailing location information.
  MutableArrayRef<SourceLoc> getTrailingLocations() {
    assert(Bits.DynamicReplacementAttr.HasTrailingLocationInfo);
    unsigned length = 2;
    return {getTrailingObjects<SourceLoc>(), length};
  }

  /// Retrieve the trailing location information.
  ArrayRef<SourceLoc> getTrailingLocations() const {
    assert(Bits.DynamicReplacementAttr.HasTrailingLocationInfo);
    unsigned length = 2; // lParens, rParens
    return {getTrailingObjects<SourceLoc>(), length};
  }

public:
  static DynamicReplacementAttr *
  create(ASTContext &Context, SourceLoc AtLoc, SourceLoc DynReplLoc,
         SourceLoc LParenLoc, DeclNameRef replacedFunction, SourceLoc RParenLoc);

  static DynamicReplacementAttr *create(ASTContext &ctx,
                                        DeclNameRef replacedFunction,
                                        AbstractFunctionDecl *replacedFuncDecl);

  static DynamicReplacementAttr *create(ASTContext &ctx,
                                        DeclNameRef replacedFunction,
                                        LazyMemberLoader *Resolver,
                                        uint64_t Data);

  DeclNameRef getReplacedFunctionName() const {
    return ReplacedFunctionName;
  }

  /// Retrieve the location of the opening parentheses, if there is one.
  SourceLoc getLParenLoc() const;

  /// Retrieve the location of the closing parentheses, if there is one.
  SourceLoc getRParenLoc() const;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::DynamicReplacement;
  }
};

/// The \c @_typeEraser(TypeEraserType) attribute.
class TypeEraserAttr final : public DeclAttribute {
  TypeExpr *TypeEraserExpr;
  LazyMemberLoader *Resolver;
  uint64_t ResolverContextData;

  friend class ResolveTypeEraserTypeRequest;

  TypeEraserAttr(SourceLoc atLoc, SourceRange range, TypeExpr *typeEraserExpr,
                 LazyMemberLoader *Resolver, uint64_t Data)
      : DeclAttribute(DeclAttrKind::TypeEraser, atLoc, range,
                      /*Implicit=*/false),
        TypeEraserExpr(typeEraserExpr), Resolver(Resolver),
        ResolverContextData(Data) {}

public:
  static TypeEraserAttr *create(ASTContext &ctx,
                                SourceLoc atLoc, SourceRange range,
                                TypeExpr *typeEraserRepr);

  static TypeEraserAttr *create(ASTContext &ctx,
                                LazyMemberLoader *Resolver,
                                uint64_t Data);

  /// Retrieve the parsed type repr for this attribute, if it
  /// was parsed. Else returns \c nullptr.
  TypeRepr *getParsedTypeEraserTypeRepr() const;

  /// Retrieve the parsed location for this attribute, if it was parsed.
  SourceLoc getLoc() const;

  /// Retrieve the resolved type of this attribute if it has been resolved by a
  /// successful call to \c getResolvedType(). Otherwise,
  /// returns \c Type()
  ///
  /// This entrypoint is only suitable for syntactic clients like the
  /// AST printer. Semantic clients should use \c getResolvedType() instead.
  Type getTypeWithoutResolving() const;

  /// Returns \c true if the type eraser type has a valid implementation of the
  /// erasing initializer for the given protocol.
  bool hasViableTypeEraserInit(ProtocolDecl *protocol) const;

  /// Resolves the type of this attribute.
  ///
  /// This entrypoint is suitable for semantic clients like the
  /// expression checker. Syntactic clients should use
  /// \c getTypeWithoutResolving() instead.
  Type getResolvedType(const ProtocolDecl *PD) const;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::TypeEraser;
  }
};

/// Represents any sort of access control modifier.
class AbstractAccessControlAttr : public DeclAttribute {
protected:
  AbstractAccessControlAttr(DeclAttrKind DK, SourceLoc atLoc, SourceRange range,
                            AccessLevel access, bool implicit)
      : DeclAttribute(DK, atLoc, range, implicit) {
    Bits.AbstractAccessControlAttr.AccessLevel = static_cast<unsigned>(access);
    assert(getAccess() == access && "not enough bits for access control");
  }

public:
  AccessLevel getAccess() const {
    return static_cast<AccessLevel>(Bits.AbstractAccessControlAttr.AccessLevel);
  }

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::AccessControl ||
           DA->getKind() == DeclAttrKind::SetterAccess;
  }
};

/// Represents a 'private', 'internal', or 'public' marker on a declaration.
class AccessControlAttr : public AbstractAccessControlAttr {
public:
  AccessControlAttr(SourceLoc atLoc, SourceRange range, AccessLevel access,
                    bool implicit = false)
      : AbstractAccessControlAttr(DeclAttrKind::AccessControl, atLoc, range,
                                  access, implicit) {}

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::AccessControl;
  }
};

/// Represents a 'private', 'internal', or 'public' marker for a setter on a
/// declaration.
class SetterAccessAttr : public AbstractAccessControlAttr {
public:
  SetterAccessAttr(SourceLoc atLoc, SourceRange range, AccessLevel access,
                   bool implicit = false)
      : AbstractAccessControlAttr(DeclAttrKind::SetterAccess, atLoc, range,
                                  access, implicit) {}

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::SetterAccess;
  }
};

/// SPI attribute applied to both decls and imports.
class SPIAccessControlAttr final : public DeclAttribute,
                                   private llvm::TrailingObjects<SPIAccessControlAttr, Identifier> {
  friend TrailingObjects;

  SPIAccessControlAttr(SourceLoc atLoc, SourceRange range,
                       ArrayRef<Identifier> spiGroups);

  // Number of trailing SPI group identifiers.
  size_t numSPIGroups;

public:
  static SPIAccessControlAttr *create(ASTContext &context, SourceLoc atLoc,
                                      SourceRange range,
                                      ArrayRef<Identifier> spiGroups);

  SPIAccessControlAttr *clone(ASTContext &C, bool implicit) const;

  /// Name of SPIs declared by the attribute.
  ///
  /// Note: A single SPI name per attribute is currently supported but this
  /// may change with the syntax change.
  ArrayRef<Identifier> getSPIGroups() const {
    return { this->template getTrailingObjects<Identifier>(),
             numSPIGroups };
  }

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::SPIAccessControl;
  }
};

/// Represents an inline attribute.
class InlineAttr : public DeclAttribute {
public:
  InlineAttr(SourceLoc atLoc, SourceRange range, InlineKind kind)
      : DeclAttribute(DeclAttrKind::Inline, atLoc, range, /*Implicit=*/false) {
    Bits.InlineAttr.kind = unsigned(kind);
  }

  InlineAttr(InlineKind kind)
    : InlineAttr(SourceLoc(), SourceRange(), kind) {}

  InlineKind getKind() const { return InlineKind(Bits.InlineAttr.kind); }
  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::Inline;
  }
};

/// Represents the optimize attribute.
class OptimizeAttr : public DeclAttribute {
public:
  OptimizeAttr(SourceLoc atLoc, SourceRange range, OptimizationMode mode)
      : DeclAttribute(DeclAttrKind::Optimize, atLoc, range,
                      /*Implicit=*/false) {
    Bits.OptimizeAttr.mode = unsigned(mode);
  }

  OptimizeAttr(OptimizationMode mode)
    : OptimizeAttr(SourceLoc(), SourceRange(), mode) {}

  OptimizationMode getMode() const {
    return OptimizationMode(Bits.OptimizeAttr.mode);
  }
  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::Optimize;
  }
};

/// Represents the exclusivity attribute.
class ExclusivityAttr : public DeclAttribute {
public:
  enum Mode {
    Checked,
    Unchecked
  };

private:
  Mode mode;

public:
  ExclusivityAttr(SourceLoc atLoc, SourceRange range, Mode mode)
      : DeclAttribute(DeclAttrKind::Exclusivity, atLoc, range,
                      /*Implicit=*/false),
        mode(mode) {}

  ExclusivityAttr(Mode mode)
    : ExclusivityAttr(SourceLoc(), SourceRange(), mode) {}

  Mode getMode() const { return mode; }

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::Exclusivity;
  }
};

/// Represents the side effects attribute.
class EffectsAttr : public DeclAttribute {
  StringRef customString;
  SourceLoc customStringLoc;

public:
  EffectsAttr(SourceLoc atLoc, SourceRange range, EffectsKind kind)
      : DeclAttribute(DeclAttrKind::Effects, atLoc, range, /*Implicit=*/false) {
    Bits.EffectsAttr.kind = unsigned(kind);
  }

  EffectsAttr(SourceLoc atLoc, SourceRange range, StringRef customString,
              SourceLoc customStringLoc)
      : DeclAttribute(DeclAttrKind::Effects, atLoc, range, /*Implicit=*/false),
        customString(customString), customStringLoc(customStringLoc) {
    Bits.EffectsAttr.kind = unsigned(EffectsKind::Custom);
  }

  EffectsAttr(EffectsKind kind)
  : EffectsAttr(SourceLoc(), SourceRange(), kind) {}

  EffectsAttr(StringRef customString)
  : EffectsAttr(SourceLoc(), SourceRange(), customString, SourceLoc()) {}

  StringRef getCustomString() const {
    assert(getKind() == EffectsKind::Custom);
    return customString;
  }
  
  SourceLoc getCustomStringLocation() const {
    return customStringLoc;
  }

  EffectsKind getKind() const { return EffectsKind(Bits.EffectsAttr.kind); }
  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::Effects;
  }

  EffectsAttr *clone(ASTContext &ctx) const {
    if (getKind() == EffectsKind::Custom) {
      return new (ctx) EffectsAttr(customString);
    }
    return new (ctx) EffectsAttr(getKind());
  }
};



/// Represents weak/unowned/unowned(unsafe) decl modifiers.
class ReferenceOwnershipAttr : public DeclAttribute {
public:
  ReferenceOwnershipAttr(SourceRange range, ReferenceOwnership kind)
      : DeclAttribute(DeclAttrKind::ReferenceOwnership, range.Start, range,
                      /*Implicit=*/false) {
    Bits.ReferenceOwnershipAttr.ownership = unsigned(kind);
  }

  ReferenceOwnershipAttr(ReferenceOwnership kind)
      : ReferenceOwnershipAttr(SourceRange(), kind) {}

  ReferenceOwnership get() const {
    return ReferenceOwnership(Bits.ReferenceOwnershipAttr.ownership);
  }

  /// Returns a copy of this attribute without any source information.
  ReferenceOwnershipAttr *clone(ASTContext &context) const {
    return new (context) ReferenceOwnershipAttr(get());
  }

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::ReferenceOwnership;
  }
};

/// Defines the attribute that we use to model documentation comments.
class RawDocCommentAttr : public DeclAttribute {
  /// Source range of the attached comment.  This comment is located before
  /// the declaration.
  CharSourceRange CommentRange;

public:
  RawDocCommentAttr(CharSourceRange CommentRange)
      : DeclAttribute(DeclAttrKind::RawDocComment, SourceLoc(), SourceRange(),
                      /*Implicit=*/false),
        CommentRange(CommentRange) {}

  CharSourceRange getCommentRange() const { return CommentRange; }

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::RawDocComment;
  }
};

/// An attribute applied to a CoreFoundation class that is toll-free bridged to
/// an Objective-C class.
///
/// This attribute is introduced by the Clang importer, and is therefore always
/// implicit.
class ObjCBridgedAttr : public DeclAttribute {
  ClassDecl *ObjCClass;

public:
  ObjCBridgedAttr(ClassDecl *ObjCClass)
      : DeclAttribute(DeclAttrKind::ObjCBridged, SourceLoc(), SourceRange(),
                      /*Implicit=*/true),
        ObjCClass(ObjCClass) {}

  /// Retrieve the Objective-C class to which this foreign class is toll-free
  /// bridged.
  ClassDecl *getObjCClass() const { return ObjCClass; }

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::ObjCBridged;
  }
};

/// An attribute that specifies a synthesized conformance of a known
/// protocol for the declaration to which it appertains.
///
/// There is no spelling for this particular attribute in source code;
/// rather, it is introduced by the Clang importer to indicate
/// synthesized conformances.
class SynthesizedProtocolAttr : public DeclAttribute {
  LazyConformanceLoader *Loader;
  ProtocolDecl *protocol;

public:
  SynthesizedProtocolAttr(ProtocolDecl *protocol, LazyConformanceLoader *Loader,
                          bool isUnchecked)
      : DeclAttribute(DeclAttrKind::SynthesizedProtocol, SourceLoc(),
                      SourceRange(),
                      /*Implicit=*/true),
        Loader(Loader), protocol(protocol) {
    Bits.SynthesizedProtocolAttr.isUnchecked = unsigned(isUnchecked);
  }

  /// Retrieve the known protocol kind naming the protocol to be
  /// synthesized.
  ProtocolDecl *getProtocol() const {
    return protocol;
  }

  bool isUnchecked() const {
    return bool(Bits.SynthesizedProtocolAttr.isUnchecked);
  }

  /// Retrieve the lazy loader that will be used to populate the
  /// synthesized conformance.
  LazyConformanceLoader *getLazyLoader() const { return Loader; }

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::SynthesizedProtocol;
  }
};

/// The @_specialize attribute, which forces specialization on the specified
/// type list.
class SpecializeAttr final
    : public DeclAttribute,
      private llvm::TrailingObjects<SpecializeAttr, Identifier,
                                    AvailableAttr *, Type> {
  friend class SpecializeAttrTargetDeclRequest;
  friend class SerializeAttrGenericSignatureRequest;
  friend TrailingObjects;

public:
  // NOTE: When adding new kinds, you must update the inline bitfield macro.
  enum class SpecializationKind {
    Full,
    Partial
  };

private:
  TrailingWhereClause *trailingWhereClause;
  GenericSignature specializedSignature;

  DeclNameRef targetFunctionName;
  LazyMemberLoader *resolver = nullptr;
  uint64_t resolverContextData;
  size_t numSPIGroups;
  size_t numAvailableAttrs;
  size_t numTypeErasedParams;
  bool typeErasedParamsInitialized;

  SpecializeAttr(SourceLoc atLoc, SourceRange Range,
                 TrailingWhereClause *clause, bool exported,
                 SpecializationKind kind, GenericSignature specializedSignature,
                 DeclNameRef targetFunctionName, ArrayRef<Identifier> spiGroups,
                 ArrayRef<AvailableAttr *> availabilityAttrs,
                 size_t typeErasedParamsCount);

public:
  static SpecializeAttr *
  create(ASTContext &Ctx, SourceLoc atLoc, SourceRange Range,
         TrailingWhereClause *clause, bool exported, SpecializationKind kind,
         DeclNameRef targetFunctionName, ArrayRef<Identifier> spiGroups,
         ArrayRef<AvailableAttr *> availabilityAttrs,
         size_t typeErasedParamsCount,
         GenericSignature specializedSignature = nullptr);

  static SpecializeAttr *create(ASTContext &ctx, bool exported,
                                SpecializationKind kind,
                                ArrayRef<Identifier> spiGroups,
                                ArrayRef<AvailableAttr *> availabilityAttrs,
                                GenericSignature specializedSignature,
                                DeclNameRef replacedFunction);

  static SpecializeAttr *create(ASTContext &ctx, bool exported,
                                SpecializationKind kind,
                                ArrayRef<Identifier> spiGroups,
                                ArrayRef<AvailableAttr *> availabilityAttrs,
                                ArrayRef<Type> typeErasedParams,
                                GenericSignature specializedSignature,
                                DeclNameRef replacedFunction,
                                LazyMemberLoader *resolver, uint64_t data);

  size_t numTrailingObjects(OverloadToken<Identifier>) const {
    return numSPIGroups;
  }

  size_t numTrailingObjects(OverloadToken<AvailableAttr *>) const {
    return numAvailableAttrs;
  }
  /// Name of SPIs declared by the attribute.
  ///
  /// Note: A single SPI name per attribute is currently supported but this
  /// may change with the syntax change.
  ArrayRef<Identifier> getSPIGroups() const {
    return { this->template getTrailingObjects<Identifier>(),
             numSPIGroups };
  }

  ArrayRef<AvailableAttr *> getAvailableAttrs() const {
    return {this->template getTrailingObjects<AvailableAttr *>(),
            numAvailableAttrs};
  }

  ArrayRef<Type> getTypeErasedParams() const {
    if (!typeErasedParamsInitialized)
      return {};

    return {this->template getTrailingObjects<Type>(),
            numTypeErasedParams};
  }

  void setTypeErasedParams(const ArrayRef<Type> typeErasedParams) {
    assert(typeErasedParams.size() == numTypeErasedParams);
    if (!typeErasedParamsInitialized) {
      std::uninitialized_copy(typeErasedParams.begin(), typeErasedParams.end(), getTrailingObjects<Type>());
      typeErasedParamsInitialized = true;
    }
  }

  TrailingWhereClause *getTrailingWhereClause() const;

  bool isExported() const {
    return Bits.SpecializeAttr.exported;
  }

  SpecializationKind getSpecializationKind() const {
    return SpecializationKind(Bits.SpecializeAttr.kind);
  }

  bool isFullSpecialization() const {
    return getSpecializationKind() == SpecializationKind::Full;
  }

  bool isPartialSpecialization() const {
    return getSpecializationKind() == SpecializationKind::Partial;
  }

  DeclNameRef getTargetFunctionName() const {
    return targetFunctionName;
  }

  /// \p forDecl is the value decl that the attribute belongs to.
  ValueDecl *getTargetFunctionDecl(const ValueDecl *forDecl) const;

  /// \p forDecl is the value decl that the attribute belongs to.
  GenericSignature
  getSpecializedSignature(const AbstractFunctionDecl *forDecl) const;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::Specialize;
  }
};

class StorageRestrictionsAttr final
    : public DeclAttribute,
      private llvm::TrailingObjects<StorageRestrictionsAttr, Identifier> {
  friend TrailingObjects;

  size_t NumInitializes;
  size_t NumAccesses;

  size_t numTrailingObjects(OverloadToken<Identifier>) const {
    return NumInitializes + NumAccesses;
  }

public:
  StorageRestrictionsAttr(SourceLoc AtLoc, SourceRange Range,
                          ArrayRef<Identifier> initializes,
                          ArrayRef<Identifier> accesses, bool Implicit);

  unsigned getNumInitializesProperties() const { return NumInitializes; }

  unsigned getNumAccessesProperties() const { return NumAccesses; }

  ArrayRef<Identifier> getInitializesNames() const {
    return {getTrailingObjects<Identifier>(), NumInitializes};
  }

  ArrayRef<Identifier> getAccessesNames() const {
    return {getTrailingObjects<Identifier>() + NumInitializes, NumAccesses};
  }

  ArrayRef<VarDecl *> getInitializesProperties(AccessorDecl *attachedTo) const;
  ArrayRef<VarDecl *> getAccessesProperties(AccessorDecl *attachedTo) const;

  static StorageRestrictionsAttr *create(ASTContext &ctx, SourceLoc atLoc,
                                         SourceRange range,
                                         ArrayRef<Identifier> initializes,
                                         ArrayRef<Identifier> accesses);

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::StorageRestrictions;
  }
};

/// The @_implements attribute, which treats a decl as the implementation for
/// some named protocol requirement (but otherwise not-visible by that name).
class ImplementsAttr : public DeclAttribute {
  TypeRepr *TyR;
  DeclName MemberName;
  DeclNameLoc MemberNameLoc;

  ImplementsAttr(SourceLoc atLoc, SourceRange Range,
                 TypeRepr *TyR,
                 DeclName MemberName,
                 DeclNameLoc MemberNameLoc);

public:
  static ImplementsAttr *create(ASTContext &Ctx, SourceLoc atLoc,
                                SourceRange Range,
                                TypeRepr *TyR,
                                DeclName MemberName,
                                DeclNameLoc MemberNameLoc);

  static ImplementsAttr *create(DeclContext *DC,
                                ProtocolDecl *Proto,
                                DeclName MemberName);

  ProtocolDecl *getProtocol(DeclContext *dc) const;
  TypeRepr *getProtocolTypeRepr() const { return TyR; }

  DeclName getMemberName() const { return MemberName; }
  DeclNameLoc getMemberNameLoc() const { return MemberNameLoc; }

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::Implements;
  }
};

/// A limited variant of \c \@objc that's used for classes with generic ancestry.
class ObjCRuntimeNameAttr : public DeclAttribute {
  static StringRef getSimpleName(const ObjCAttr &Original) {
    assert(Original.hasName());
    return Original.getName()->getSimpleName().str();
  }
public:
  ObjCRuntimeNameAttr(StringRef Name, SourceLoc AtLoc, SourceRange Range,
                      bool Implicit)
      : DeclAttribute(DeclAttrKind::ObjCRuntimeName, AtLoc, Range, Implicit),
        Name(Name) {}

  explicit ObjCRuntimeNameAttr(const ObjCAttr &Original)
    : ObjCRuntimeNameAttr(getSimpleName(Original), Original.AtLoc,
                          Original.Range, Original.isImplicit()) {}

  const StringRef Name;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::ObjCRuntimeName;
  }
};

/// Attribute that specifies a protocol conformance that has been restated
/// (i.e., is redundant) but should still be emitted in Objective-C metadata.
class RestatedObjCConformanceAttr : public DeclAttribute {
public:
  explicit RestatedObjCConformanceAttr(ProtocolDecl *proto)
      : DeclAttribute(DeclAttrKind::RestatedObjCConformance, SourceLoc(),
                      SourceRange(),
                      /*Implicit=*/true),
        Proto(proto) {}

  /// The protocol to which this type conforms.
  ProtocolDecl * const Proto;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::RestatedObjCConformance;
  }
};

/// Attached to type declarations synthesized by the Clang importer.
///
/// Used to control manglings.
class ClangImporterSynthesizedTypeAttr : public DeclAttribute {
public:
  // NOTE: When adding new kinds, you must update the inline bitfield macro.
  enum class Kind : char {
    /// A struct synthesized by the importer to represent an NSError with a
    /// particular domain, as specified by an enum with the \c ns_error_domain
    /// Clang attribute.
    ///
    /// This one is for enums with names.
    NSErrorWrapper,

    /// A struct synthesized by the importer to represent an NSError with a
    /// particular domain, as specified by an enum with the \c ns_error_domain
    /// Clang attribute.
    ///
    /// This one is for anonymous enums that are immediately typedef'd, giving
    /// them a unique name for linkage purposes according to the C++ standard.
    NSErrorWrapperAnon,
  };

  /// The (Clang) name of the declaration that caused this type declaration to
  /// be synthesized.
  ///
  /// Must be a valid Swift identifier as well, for mangling purposes.
  const StringRef originalTypeName;

  explicit ClangImporterSynthesizedTypeAttr(StringRef originalTypeName,
                                            Kind kind)
      : DeclAttribute(DeclAttrKind::ClangImporterSynthesizedType, SourceLoc(),
                      SourceRange(), /*Implicit=*/true),
        originalTypeName(originalTypeName) {
    assert(!originalTypeName.empty());
    Bits.ClangImporterSynthesizedTypeAttr.kind = unsigned(kind);
  }

  Kind getKind() const {
    return Kind(Bits.ClangImporterSynthesizedTypeAttr.kind);
  }

  StringRef getManglingName() const {
    return manglingNameForKind(getKind());
  }

  static StringRef manglingNameForKind(Kind kind) {
    switch (kind) {
    case Kind::NSErrorWrapper:
      return "e";
    case Kind::NSErrorWrapperAnon:
      return "E";
    }
    llvm_unreachable("unhandled kind");
  }

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::ClangImporterSynthesizedType;
  }
};

/// Defines a custom attribute.
class CustomAttr final : public DeclAttribute {
  TypeExpr *typeExpr;
  ArgumentList *argList;
  PatternBindingInitializer *initContext;
  Expr *semanticInit = nullptr;

  mutable unsigned isArgUnsafeBit : 1;

  CustomAttr(SourceLoc atLoc, SourceRange range, TypeExpr *type,
             PatternBindingInitializer *initContext, ArgumentList *argList,
             bool implicit);

public:
  static CustomAttr *create(ASTContext &ctx, SourceLoc atLoc, TypeExpr *type,
                            bool implicit = false) {
    return create(ctx, atLoc, type, /*initContext*/ nullptr,
                  /*argList*/ nullptr, implicit);
  }

  static CustomAttr *create(ASTContext &ctx, SourceLoc atLoc, TypeExpr *type,
                            PatternBindingInitializer *initContext,
                            ArgumentList *argList, bool implicit = false);

  TypeExpr *getTypeExpr() const { return typeExpr; }
  TypeRepr *getTypeRepr() const;
  Type getType() const;

  /// Destructure an attribute's type repr for a macro reference.
  ///
  /// For a 1-level member type repr whose base and member are both identifier
  /// types, e.g. `Foo.Bar`, return a pair of the base and the member.
  ///
  /// For an identifier type repr, return a pair of `nullptr` and the
  /// identifier.
  std::pair<UnqualifiedIdentTypeRepr *, DeclRefTypeRepr *>
  destructureMacroRef();

  /// Whether the attribute has any arguments.
  bool hasArgs() const { return argList != nullptr; }

  /// The argument list of the attribute if it has any arguments, \c nullptr
  /// otherwise.
  ArgumentList *getArgs() const { return argList; }
  void setArgs(ArgumentList *newArgs) { argList = newArgs; }

  /// Determine whether the argument is '(unsafe)', a special subexpression
  /// used by global actors.
  bool isArgUnsafe() const;
  void setArgIsUnsafe(bool unsafe) { isArgUnsafeBit = unsafe; }

  Expr *getSemanticInit() const { return semanticInit; }
  void setSemanticInit(Expr *expr) { semanticInit = expr; }

  PatternBindingInitializer *getInitContext() const { return initContext; }

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::Custom;
  }

private:
  friend class CustomAttrNominalRequest;
  void resetTypeInformation(TypeExpr *repr);

private:
  friend class CustomAttrTypeRequest;
  void setType(Type ty);
};

/// Relates a property to its projection value property, as described by a property wrapper. For
/// example, given
/// \code
/// @A var foo: Int
/// \endcode
///
/// Where \c A is a property wrapper that has a \c projectedValue property, the compiler
/// synthesizes a declaration $foo an attaches the attribute
/// \c _projectedValuePropertyAttr($foo) to \c foo to record the link.
class ProjectedValuePropertyAttr : public DeclAttribute {
public:
  ProjectedValuePropertyAttr(Identifier PropertyName, SourceLoc AtLoc,
                             SourceRange Range, bool Implicit)
      : DeclAttribute(DeclAttrKind::ProjectedValueProperty, AtLoc, Range,
                      Implicit),
        ProjectionPropertyName(PropertyName) {}

  // The projection property name.
  const Identifier ProjectionPropertyName;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::ProjectedValueProperty;
  }
};

/// Describes a symbol that was originally defined in another module. For
/// example, given the following declaration:
///
/// \code
/// @_originallyDefinedIn(module: "Original", OSX 10.15) var foo: Int
/// \endcode
///
/// The variable \p foo was originally defined in another module called
/// \p Original prior to OSX 10.15
class OriginallyDefinedInAttr: public DeclAttribute {
public:
  OriginallyDefinedInAttr(SourceLoc AtLoc, SourceRange Range,
                          StringRef OriginalModuleName, PlatformKind Platform,
                          const llvm::VersionTuple MovedVersion, bool Implicit)
      : DeclAttribute(DeclAttrKind::OriginallyDefinedIn, AtLoc, Range,
                      Implicit),
        OriginalModuleName(OriginalModuleName), Platform(Platform),
        MovedVersion(MovedVersion) {}

  OriginallyDefinedInAttr *clone(ASTContext &C, bool implicit) const;

  // The original module name.
  const StringRef OriginalModuleName;

  /// The platform of the symbol.
  const PlatformKind Platform;

  /// Indicates when the symbol was moved here.
  const llvm::VersionTuple MovedVersion;

  struct ActiveVersion {
    StringRef ModuleName;
    PlatformKind Platform;
    llvm::VersionTuple Version;
    bool ForTargetVariant = false;
  };

  /// Returns non-optional if this attribute is active given the current platform.
  /// The value provides more details about the active platform.
  std::optional<ActiveVersion> isActivePlatform(const ASTContext &ctx) const;
  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::OriginallyDefinedIn;
  }
};

/// Attribute that marks a function as differentiable.
///
/// Examples:
///   @differentiable(reverse)
///   @differentiable(reverse, wrt: (self, x, y))
///   @differentiable(reverse, wrt: (self, x, y) where T : FloatingPoint)
class DifferentiableAttr final
    : public DeclAttribute,
      private llvm::TrailingObjects<DifferentiableAttr,
                                    ParsedAutoDiffParameter> {
  friend TrailingObjects;
  friend class DifferentiableAttributeTypeCheckRequest;

  /// The declaration on which the `@differentiable` attribute is declared.
  /// May not be a valid declaration for `@differentiable` attributes.
  /// Resolved during parsing and deserialization.
  Decl *OriginalDeclaration = nullptr;
  /// The differentiability kind.
  DifferentiabilityKind DifferentiabilityKind;
  /// The number of parsed differentiability parameters specified in 'wrt:'.
  unsigned NumParsedParameters = 0;
  /// The differentiability parameter indices, resolved by the type checker.
  /// The bit stores whether the parameter indices have been computed.
  ///
  /// Note: it is necessary to use a bit instead of `nullptr` parameter indices
  /// to represent "parameter indices not yet type-checked" because invalid
  /// attributes have `nullptr` parameter indices but have been type-checked.
  llvm::PointerIntPair<IndexSubset *, 1, bool> ParameterIndicesAndBit;
  /// The trailing where clause (optional).
  TrailingWhereClause *WhereClause = nullptr;
  /// The generic signature for autodiff associated functions. Resolved by the
  /// type checker based on the original function's generic signature and the
  /// attribute's where clause requirements. This is set only if the attribute
  /// has a where clause.
  GenericSignature DerivativeGenericSignature;
  /// The source location of the implicitly inherited protocol requirement
  /// `@differentiable` attribute. Used for diagnostics, not serialized.
  ///
  /// This is set during conformance type-checking, only for implicit
  /// `@differentiable` attributes created for non-public protocol witnesses of
  /// protocol requirements with `@differentiable` attributes.
  SourceLoc ImplicitlyInheritedDifferentiableAttrLocation;

  explicit DifferentiableAttr(bool implicit, SourceLoc atLoc,
                              SourceRange baseRange,
                              enum DifferentiabilityKind diffKind,
                              ArrayRef<ParsedAutoDiffParameter> parameters,
                              TrailingWhereClause *clause);

  explicit DifferentiableAttr(Decl *original, bool implicit, SourceLoc atLoc,
                              SourceRange baseRange,
                              enum DifferentiabilityKind diffKind,
                              IndexSubset *parameterIndices,
                              GenericSignature derivativeGenericSignature);

public:
  static DifferentiableAttr *create(ASTContext &context, bool implicit,
                                    SourceLoc atLoc, SourceRange baseRange,
                                    enum DifferentiabilityKind diffKind,
                                    ArrayRef<ParsedAutoDiffParameter> params,
                                    TrailingWhereClause *clause);

  static DifferentiableAttr *create(AbstractFunctionDecl *original,
                                    bool implicit, SourceLoc atLoc,
                                    SourceRange baseRange,
                                    enum DifferentiabilityKind diffKind,
                                    IndexSubset *parameterIndices,
                                    GenericSignature derivativeGenSig);

  Decl *getOriginalDeclaration() const { return OriginalDeclaration; }

  /// Sets the original declaration on which this attribute is declared.
  /// Should only be used by parsing and deserialization.
  void setOriginalDeclaration(Decl *originalDeclaration);

private:
  /// Returns true if the given `@differentiable` attribute has been
  /// type-checked.
  bool hasBeenTypeChecked() const;

public:
  IndexSubset *getParameterIndices() const;
  void setParameterIndices(IndexSubset *parameterIndices);

  /// The parsed differentiability parameters, i.e. the list of parameters
  /// specified in 'wrt:'.
  ArrayRef<ParsedAutoDiffParameter> getParsedParameters() const {
    return {getTrailingObjects<ParsedAutoDiffParameter>(), NumParsedParameters};
  }
  MutableArrayRef<ParsedAutoDiffParameter> getParsedParameters() {
    return {getTrailingObjects<ParsedAutoDiffParameter>(), NumParsedParameters};
  }
  size_t numTrailingObjects(OverloadToken<ParsedAutoDiffParameter>) const {
    return NumParsedParameters;
  }

  enum DifferentiabilityKind getDifferentiabilityKind() const {
    return DifferentiabilityKind;
  }

  bool isNormalDifferentiability() const {
    return DifferentiabilityKind == DifferentiabilityKind::Normal;
  }

  bool isLinearDifferentiability() const {
    return DifferentiabilityKind == DifferentiabilityKind::Linear;
  }

  bool isForwardDifferentiability() const {
    return DifferentiabilityKind == DifferentiabilityKind::Forward;
  }

  bool isReverseDifferentiability() const {
    return DifferentiabilityKind == DifferentiabilityKind::Reverse;
  }

  TrailingWhereClause *getWhereClause() const { return WhereClause; }

  GenericSignature getDerivativeGenericSignature() const {
    return DerivativeGenericSignature;
  }
  void setDerivativeGenericSignature(GenericSignature derivativeGenSig) {
    DerivativeGenericSignature = derivativeGenSig;
  }

  SourceLoc getImplicitlyInheritedDifferentiableAttrLocation() const {
    return ImplicitlyInheritedDifferentiableAttrLocation;
  }
  void getImplicitlyInheritedDifferentiableAttrLocation(SourceLoc loc) {
    assert(isImplicit());
    ImplicitlyInheritedDifferentiableAttrLocation = loc;
  }

  /// Get the derivative generic environment for the given `@differentiable`
  /// attribute and original function.
  GenericEnvironment *
  getDerivativeGenericEnvironment(AbstractFunctionDecl *original) const;

  // Print the attribute to the given stream.
  // If `omitWrtClause` is true, omit printing the `wrt:` clause.
  void print(llvm::raw_ostream &OS, const Decl *D, bool omitWrtClause = false) const;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::Differentiable;
  }
};

/// A declaration name with location.
struct DeclNameRefWithLoc {
  /// The declaration name.
  DeclNameRef Name;
  /// The declaration name location.
  DeclNameLoc Loc;
  /// An optional accessor kind.
  std::optional<AccessorKind> AccessorKind;

  void print(ASTPrinter &Printer) const;
};

/// The `@derivative(of:)` attribute registers a function as a derivative of
/// another function-like declaration: a 'func', 'init', 'subscript', or 'var'
/// computed property declaration.
///
/// The `@derivative(of:)` attribute also has an optional `wrt:` clause
/// specifying the parameters that are differentiated "with respect to", i.e.
/// the differentiability parameters. The differentiability parameters must
/// conform to the `Differentiable` protocol.
///
/// If the `wrt:` clause is unspecified, the differentiability parameters are
/// inferred to be all parameters that conform to `Differentiable`.
///
/// `@derivative(of:)` attribute type-checking verifies that the type of the
/// derivative function declaration is consistent with the type of the
/// referenced original declaration and the differentiability parameters.
///
/// Examples:
///   @derivative(of: sin(_:))
///   @derivative(of: +, wrt: (lhs, rhs))
class DerivativeAttr final
    : public DeclAttribute,
      private llvm::TrailingObjects<DerivativeAttr, ParsedAutoDiffParameter> {
  friend TrailingObjects;
  friend class DerivativeAttrOriginalDeclRequest;

  /// The declaration on which the `@derivative` attribute is declared.
  /// May not be a valid declaration for `@derivative` attributes.
  /// Resolved during parsing and deserialization.
  Decl *OriginalDeclaration = nullptr;
  /// The base type for the referenced original declaration. This field is
  /// non-null only for parsed attributes that reference a qualified original
  /// declaration. This field is not serialized; type-checking uses it to
  /// resolve the original declaration, which is serialized.
  TypeRepr *BaseTypeRepr;
  /// The original function name.
  DeclNameRefWithLoc OriginalFunctionName;
  /// The original function.
  ///
  /// The states are:
  /// - nullptr:
  ///   The original function is unknown. The typechecker is responsible for
  ///   eventually resolving it.
  /// - AbstractFunctionDecl:
  ///   The original function is known to be this `AbstractFunctionDecl`.
  /// - LazyMemberLoader:
  ///   This `LazyMemberLoader` knows how to resolve the original function.
  ///   `ResolverContextData` is an additional piece of data that the
  ///   `LazyMemberLoader` needs.
  // TODO(TF-1235): Making `DerivativeAttr` immutable will simplify this by
  // removing the `AbstractFunctionDecl` state.
  llvm::PointerUnion<AbstractFunctionDecl *, LazyMemberLoader *> OriginalFunction;
  /// Data representing the original function declaration. See doc comment for
  /// `OriginalFunction`.
  uint64_t ResolverContextData = 0;
  /// The number of parsed differentiability parameters specified in 'wrt:'.
  unsigned NumParsedParameters = 0;
  /// The differentiability parameter indices, resolved by the type checker.
  IndexSubset *ParameterIndices = nullptr;
  /// The derivative function kind (JVP or VJP), resolved by the type checker.
  std::optional<AutoDiffDerivativeFunctionKind> Kind = std::nullopt;

  explicit DerivativeAttr(bool implicit, SourceLoc atLoc, SourceRange baseRange,
                          TypeRepr *baseTypeRepr, DeclNameRefWithLoc original,
                          ArrayRef<ParsedAutoDiffParameter> params);

  explicit DerivativeAttr(bool implicit, SourceLoc atLoc, SourceRange baseRange,
                          TypeRepr *baseTypeRepr, DeclNameRefWithLoc original,
                          IndexSubset *parameterIndices);

public:
  static DerivativeAttr *create(ASTContext &context, bool implicit,
                                SourceLoc atLoc, SourceRange baseRange,
                                TypeRepr *baseTypeRepr,
                                DeclNameRefWithLoc original,
                                ArrayRef<ParsedAutoDiffParameter> params);

  static DerivativeAttr *create(ASTContext &context, bool implicit,
                                SourceLoc atLoc, SourceRange baseRange,
                                TypeRepr *baseTypeRepr,
                                DeclNameRefWithLoc original,
                                IndexSubset *parameterIndices);

  Decl *getOriginalDeclaration() const { return OriginalDeclaration; }

  /// Sets the original declaration on which this attribute is declared.
  /// Should only be used by parsing and deserialization.
  void setOriginalDeclaration(Decl *originalDeclaration);

  TypeRepr *getBaseTypeRepr() const { return BaseTypeRepr; }
  DeclNameRefWithLoc getOriginalFunctionName() const {
    return OriginalFunctionName;
  }
  AbstractFunctionDecl *getOriginalFunction(ASTContext &context) const;
  void setOriginalFunction(AbstractFunctionDecl *decl);
  void setOriginalFunctionResolver(LazyMemberLoader *resolver,
                                   uint64_t resolverContextData);

  AutoDiffDerivativeFunctionKind getDerivativeKind() const {
    assert(Kind && "Derivative function kind has not yet been resolved");
    return *Kind;
  }
  void setDerivativeKind(AutoDiffDerivativeFunctionKind kind) { Kind = kind; }

  /// The parsed differentiability parameters, i.e. the list of parameters
  /// specified in 'wrt:'.
  ArrayRef<ParsedAutoDiffParameter> getParsedParameters() const {
    return {getTrailingObjects<ParsedAutoDiffParameter>(), NumParsedParameters};
  }
  MutableArrayRef<ParsedAutoDiffParameter> getParsedParameters() {
    return {getTrailingObjects<ParsedAutoDiffParameter>(), NumParsedParameters};
  }
  size_t numTrailingObjects(OverloadToken<ParsedAutoDiffParameter>) const {
    return NumParsedParameters;
  }

  IndexSubset *getParameterIndices() const {
    return ParameterIndices;
  }
  void setParameterIndices(IndexSubset *parameterIndices) {
    ParameterIndices = parameterIndices;
  }

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::Derivative;
  }
};

/// The `@transpose(of:)` attribute registers a function as a transpose of
/// another function-like declaration: a 'func', 'init', 'subscript', or 'var'
/// computed property declaration.
///
/// The `@transpose(of:)` attribute also has a `wrt:` clause specifying the
/// parameters that are transposed "with respect to", i.e. the linearity
/// parameters.
///
/// Examples:
///   @transpose(of: foo)
///   @transpose(of: +, wrt: (0, 1))
class TransposeAttr final
    : public DeclAttribute,
      private llvm::TrailingObjects<TransposeAttr, ParsedAutoDiffParameter> {
  friend TrailingObjects;

  /// The base type for the referenced original declaration. This field is
  /// non-null only for parsed attributes that reference a qualified original
  /// declaration. This field is not serialized; type-checking uses it to
  /// resolve the original declaration, which is serialized.
  TypeRepr *BaseTypeRepr;
  /// The original function name.
  DeclNameRefWithLoc OriginalFunctionName;
  /// The original function declaration, resolved by the type checker.
  AbstractFunctionDecl *OriginalFunction = nullptr;
  /// The number of parsed linearity parameters specified in 'wrt:'.
  unsigned NumParsedParameters = 0;
  /// The linearity parameter indices, resolved by the type checker.
  IndexSubset *ParameterIndices = nullptr;

  explicit TransposeAttr(bool implicit, SourceLoc atLoc, SourceRange baseRange,
                         TypeRepr *baseType, DeclNameRefWithLoc original,
                         ArrayRef<ParsedAutoDiffParameter> params);

  explicit TransposeAttr(bool implicit, SourceLoc atLoc, SourceRange baseRange,
                         TypeRepr *baseType, DeclNameRefWithLoc original,
                         IndexSubset *parameterIndices);

public:
  static TransposeAttr *create(ASTContext &context, bool implicit,
                               SourceLoc atLoc, SourceRange baseRange,
                               TypeRepr *baseType, DeclNameRefWithLoc original,
                               ArrayRef<ParsedAutoDiffParameter> params);

  static TransposeAttr *create(ASTContext &context, bool implicit,
                               SourceLoc atLoc, SourceRange baseRange,
                               TypeRepr *baseType, DeclNameRefWithLoc original,
                               IndexSubset *parameterIndices);

  TypeRepr *getBaseTypeRepr() const { return BaseTypeRepr; }
  DeclNameRefWithLoc getOriginalFunctionName() const {
    return OriginalFunctionName;
  }
  AbstractFunctionDecl *getOriginalFunction() const {
    return OriginalFunction;
  }
  void setOriginalFunction(AbstractFunctionDecl *decl) {
    OriginalFunction = decl;
  }

  /// The parsed linearity parameters, i.e. the list of parameters specified in
  /// 'wrt:'.
  ArrayRef<ParsedAutoDiffParameter> getParsedParameters() const {
    return {getTrailingObjects<ParsedAutoDiffParameter>(), NumParsedParameters};
  }
  MutableArrayRef<ParsedAutoDiffParameter> getParsedParameters() {
    return {getTrailingObjects<ParsedAutoDiffParameter>(), NumParsedParameters};
  }
  size_t numTrailingObjects(OverloadToken<ParsedAutoDiffParameter>) const {
    return NumParsedParameters;
  }

  IndexSubset *getParameterIndices() const {
    return ParameterIndices;
  }
  void setParameterIndices(IndexSubset *parameterIndices) {
    ParameterIndices = parameterIndices;
  }

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::Transpose;
  }
};

enum class NonSendableKind : uint8_t {
  /// A plain '@_nonSendable' attribute. Should be applied directly to
  /// particular declarations; overrides even an explicit 'Sendable'
  /// conformance.
  Specific,

  /// A '@_nonSendable(_assumed)' attribute. Should be applied to large swaths
  /// of declarations; does not override explicit 'Sendable' conformances.
  Assumed
};

/// Marks a declaration as explicitly non-Sendable.
class NonSendableAttr : public DeclAttribute {
public:
  NonSendableAttr(SourceLoc AtLoc, SourceRange Range,
                  NonSendableKind Specificity, bool Implicit = false)
      : DeclAttribute(DeclAttrKind::NonSendable, AtLoc, Range, Implicit),
        Specificity(Specificity) {}

  NonSendableAttr(NonSendableKind Specificity, bool Implicit = false)
    : NonSendableAttr(SourceLoc(), SourceRange(), Specificity, Implicit) {}

  /// Was this '@_nonSendable(_assumed)'?
  const NonSendableKind Specificity;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::NonSendable;
  }
};

/// The @_unavailableFromAsync attribute, used to make function declarations
/// unavailable from async contexts.
class UnavailableFromAsyncAttr : public DeclAttribute {
public:
  UnavailableFromAsyncAttr(StringRef Message, SourceLoc AtLoc,
                           SourceRange Range, bool Implicit)
      : DeclAttribute(DeclAttrKind::UnavailableFromAsync, AtLoc, Range,
                      Implicit),
        Message(Message) {}
  UnavailableFromAsyncAttr(StringRef Message, bool Implicit)
      : UnavailableFromAsyncAttr(Message, SourceLoc(), SourceRange(),
                                 Implicit) {}
  const StringRef Message;

  bool hasMessage() const { return !Message.empty(); }

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::UnavailableFromAsync;
  }
};

/// The `@backDeployed(...)` attribute, used to make function declarations
/// available for back deployment to older OSes via emission into the client
/// binary.
class BackDeployedAttr : public DeclAttribute {
public:
  BackDeployedAttr(SourceLoc AtLoc, SourceRange Range, PlatformKind Platform,
                   const llvm::VersionTuple Version, bool Implicit)
      : DeclAttribute(DeclAttrKind::BackDeployed, AtLoc, Range, Implicit),
        Platform(Platform), Version(Version) {}

  /// The platform the symbol is available for back deployment on.
  const PlatformKind Platform;

  /// The earliest platform version that may use the back deployed implementation.
  const llvm::VersionTuple Version;

  /// Returns true if this attribute is active given the current platform.
  bool isActivePlatform(const ASTContext &ctx) const;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::BackDeployed;
  }
};

/// Defines the `@_expose` attribute, used to expose declarations in the
/// header used by C/C++ to interoperate with Swift.
class ExposeAttr : public DeclAttribute {
public:
  ExposeAttr(StringRef Name, SourceLoc AtLoc, SourceRange Range,
             ExposureKind Kind, bool Implicit)
      : DeclAttribute(DeclAttrKind::Expose, AtLoc, Range, Implicit),
        Name(Name) {
    Bits.ExposeAttr.kind = static_cast<unsigned>(Kind);
  }

  ExposeAttr(StringRef Name, ExposureKind Kind, bool Implicit)
      : ExposeAttr(Name, SourceLoc(), SourceRange(), Kind, Implicit) {}

  /// The exposed declaration name.
  const StringRef Name;

  /// Returns the kind of exposure.
  ExposureKind getExposureKind() const {
    return static_cast<ExposureKind>(Bits.ExposeAttr.kind);
  }

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::Expose;
  }
};

/// Define the `@_extern` attribute, used to import external declarations in
/// the specified way to interoperate with Swift.
class ExternAttr : public DeclAttribute {
  SourceLoc LParenLoc, RParenLoc;

public:
  ExternAttr(std::optional<StringRef> ModuleName, std::optional<StringRef> Name,
             SourceLoc AtLoc, SourceLoc LParenLoc, SourceLoc RParenLoc,
             SourceRange Range, ExternKind Kind, bool Implicit)
      : DeclAttribute(DeclAttrKind::Extern, AtLoc, Range, Implicit),
        LParenLoc(LParenLoc), RParenLoc(RParenLoc), ModuleName(ModuleName),
        Name(Name) {
    Bits.ExternAttr.kind = static_cast<unsigned>(Kind);
  }

  ExternAttr(std::optional<StringRef> ModuleName, std::optional<StringRef> Name,
             ExternKind Kind, bool Implicit)
      : ExternAttr(ModuleName, Name, SourceLoc(), SourceLoc(), SourceLoc(),
                   SourceRange(), Kind, Implicit) {}

  /// The module name to import the named declaration in it
  /// Used for Wasm import declaration.
  const std::optional<StringRef> ModuleName;

  /// The declaration name to import
  /// std::nullopt if the declaration name is not specified with @_extern(c)
  const std::optional<StringRef> Name;

  SourceLoc getLParenLoc() const { return LParenLoc; }
  SourceLoc getRParenLoc() const { return RParenLoc; }

  /// Returns the kind of extern.
  ExternKind getExternKind() const {
    return static_cast<ExternKind>(Bits.ExternAttr.kind);
  }

  /// Returns the C name of the given declaration.
  /// \p forDecl is the func decl that the attribute belongs to.
  StringRef getCName(const FuncDecl *forDecl) const;

  /// Find an ExternAttr with the given kind in the given DeclAttributes.
  static ExternAttr *find(DeclAttributes &attrs, ExternKind kind);

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::Extern;
  }
};

/// The `@_documentation(...)` attribute, used to override a symbol's visibility
/// in symbol graphs, and/or adding arbitrary metadata to it.
class DocumentationAttr: public DeclAttribute {
public:
  DocumentationAttr(SourceLoc AtLoc, SourceRange Range, StringRef Metadata,
                    std::optional<AccessLevel> Visibility, bool Implicit)
      : DeclAttribute(DeclAttrKind::Documentation, AtLoc, Range, Implicit),
        Metadata(Metadata), Visibility(Visibility) {}

  DocumentationAttr(StringRef Metadata, std::optional<AccessLevel> Visibility,
                    bool Implicit)
      : DocumentationAttr(SourceLoc(), SourceRange(), Metadata, Visibility,
                          Implicit) {}

  const StringRef Metadata;
  const std::optional<AccessLevel> Visibility;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::Documentation;
  }
};

class ObjCImplementationAttr final : public DeclAttribute {
public:
  Identifier CategoryName;

  ObjCImplementationAttr(Identifier CategoryName, SourceLoc AtLoc,
                         SourceRange Range, bool isEarlyAdopter = false,
                         bool Implicit = false,
                         bool isCategoryNameInvalid = false)
      : DeclAttribute(DeclAttrKind::ObjCImplementation, AtLoc, Range, Implicit),
        CategoryName(CategoryName) {
    Bits.ObjCImplementationAttr.isCategoryNameInvalid = isCategoryNameInvalid;
    Bits.ObjCImplementationAttr.isEarlyAdopter = isEarlyAdopter;
  }

  /// Early adopters use the \c \@_objcImplementation spelling. For backwards
  /// compatibility, issues with them are diagnosed as warnings, not errors.
  bool isEarlyAdopter() const {
    return Bits.ObjCImplementationAttr.isEarlyAdopter;
  }

  bool isCategoryNameInvalid() const {
    return Bits.ObjCImplementationAttr.isCategoryNameInvalid;
  }

  void setCategoryNameInvalid(bool newValue = true) {
    Bits.ObjCImplementationAttr.isCategoryNameInvalid = newValue;
  }

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::ObjCImplementation;
  }
};

/// Represents nonisolated modifier.
class NonisolatedAttr final : public DeclAttribute {
public:
  NonisolatedAttr(SourceLoc atLoc, SourceRange range, bool unsafe,
                  bool implicit)
      : DeclAttribute(DeclAttrKind::Nonisolated, atLoc, range, implicit) {
    Bits.NonisolatedAttr.isUnsafe = unsafe;
    assert((isUnsafe() == unsafe) && "not enough bits for unsafe state");
  }

  NonisolatedAttr(bool unsafe, bool implicit)
      : NonisolatedAttr({}, {}, unsafe, implicit) {}

  bool isUnsafe() const { return Bits.NonisolatedAttr.isUnsafe; }

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::Nonisolated;
  }
};

/// A macro role attribute, spelled with either @attached or @freestanding,
/// which declares one of the roles that a given macro can inhabit.
class MacroRoleAttr final
    : public DeclAttribute,
      private llvm::TrailingObjects<MacroRoleAttr, MacroIntroducedDeclName,
                                    TypeExpr *> {
  friend TrailingObjects;

  MacroSyntax syntax;
  MacroRole role;
  unsigned numNames;
  unsigned numConformances;
  SourceLoc lParenLoc, rParenLoc;

  MacroRoleAttr(SourceLoc atLoc, SourceRange range, MacroSyntax syntax,
                SourceLoc lParenLoc, MacroRole role,
                ArrayRef<MacroIntroducedDeclName> names,
                ArrayRef<TypeExpr *> conformances,
                SourceLoc rParenLoc, bool implicit);

public:
  static MacroRoleAttr *create(ASTContext &ctx, SourceLoc atLoc,
                               SourceRange range, MacroSyntax syntax,
                               SourceLoc lParenLoc, MacroRole role,
                               ArrayRef<MacroIntroducedDeclName> names,
                               ArrayRef<TypeExpr *> conformances,
                               SourceLoc rParenLoc, bool implicit);

  size_t numTrailingObjects(OverloadToken<MacroIntroducedDeclName>) const {
    return numNames;
  }

  size_t numTrailingObjects(OverloadToken<TypeExpr *>) const {
    return numConformances;
  }

  SourceLoc getLParenLoc() const { return lParenLoc; }
  SourceLoc getRParenLoc() const { return rParenLoc; }

  MacroSyntax getMacroSyntax() const { return syntax; }
  MacroRole getMacroRole() const { return role; }
  ArrayRef<MacroIntroducedDeclName> getNames() const;
  ArrayRef<TypeExpr *> getConformances() const;
  bool hasNameKind(MacroIntroducedDeclNameKind kind) const;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::MacroRole;
  }
};

/// Specifies the raw storage used by a type.
class RawLayoutAttr final : public DeclAttribute {
  /// The element type to share size and alignment with, if any.
  TypeRepr *LikeType;
  /// The number of elements in an array to share stride and alignment with,
  /// or zero if no such size was specified. If `LikeType` is null, this is
  /// the size in bytes of the raw storage.
  unsigned SizeOrCount;
  /// If `LikeType` is null, the alignment in bytes to use for the raw storage.
  unsigned Alignment;
  /// The resolved like type.
  mutable Type CachedResolvedLikeType = Type();

  friend class ResolveRawLayoutLikeTypeRequest;

public:
  /// Construct a `@_rawLayout(like: T)` attribute.
  RawLayoutAttr(TypeRepr *LikeType, SourceLoc AtLoc, SourceRange Range)
      : DeclAttribute(DeclAttrKind::RawLayout, AtLoc, Range,
                      /*implicit*/ false),
        LikeType(LikeType), SizeOrCount(0), Alignment(~0u) {}

  /// Construct a `@_rawLayout(likeArrayOf: T, count: N)` attribute.
  RawLayoutAttr(TypeRepr *LikeType, unsigned Count, SourceLoc AtLoc,
                SourceRange Range)
      : DeclAttribute(DeclAttrKind::RawLayout, AtLoc, Range,
                      /*implicit*/ false),
        LikeType(LikeType), SizeOrCount(Count), Alignment(0) {}

  /// Construct a `@_rawLayout(size: N, alignment: M)` attribute.
  RawLayoutAttr(unsigned Size, unsigned Alignment, SourceLoc AtLoc,
                SourceRange Range)
      : DeclAttribute(DeclAttrKind::RawLayout, AtLoc, Range,
                      /*implicit*/ false),
        LikeType(nullptr), SizeOrCount(Size), Alignment(Alignment) {}

  /// Return the type whose single-element layout the attribute type should get
  /// its layout from. Returns null if the attribute specifies an array or manual
  /// layout.
  TypeRepr *getScalarLikeType() const {
    if (!LikeType)
      return nullptr;
    if (Alignment != ~0u)
      return nullptr;
    return LikeType;
  }
  
  /// Return the type whose array layout the attribute type should get its
  /// layout from, along with the size of that array. Returns None if the
  /// attribute specifies scalar or manual layout.
  std::optional<std::pair<TypeRepr *, unsigned>>
  getArrayLikeTypeAndCount() const {
    if (!LikeType)
      return std::nullopt;
    if (Alignment == ~0u)
      return std::nullopt;
    return std::make_pair(LikeType, SizeOrCount);
  }

  /// Return the size and alignment of the attributed type. Returns
  /// None if the attribute specifies layout like some other type.
  std::optional<std::pair<unsigned, unsigned>> getSizeAndAlignment() const {
    if (LikeType)
      return std::nullopt;
    return std::make_pair(SizeOrCount, Alignment);
  }

  Type getResolvedLikeType(StructDecl *sd) const;

  /// Return the type whose single-element layout the attribute type should get
  /// its layout from. Returns None if the attribute specifies an array or manual
  /// layout.
  std::optional<Type> getResolvedScalarLikeType(StructDecl *sd) const {
    if (!LikeType)
      return std::nullopt;
    if (Alignment != ~0u)
      return std::nullopt;
    return getResolvedLikeType(sd);
  }

  /// Return the type whose array layout the attribute type should get its
  /// layout from, along with the size of that array. Returns None if the
  /// attribute specifies scalar or manual layout.
  std::optional<std::pair<Type, unsigned>>
  getResolvedArrayLikeTypeAndCount(StructDecl *sd) const {
    if (!LikeType)
      return std::nullopt;
    if (Alignment == ~0u)
      return std::nullopt;
    return std::make_pair(getResolvedLikeType(sd), SizeOrCount);
  }

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::RawLayout;
  }
};

/// Predicate used to filter MatchingAttributeRange.
template <typename ATTR, bool AllowInvalid> struct ToAttributeKind {
  ToAttributeKind() {}

  std::optional<const ATTR *> operator()(const DeclAttribute *Attr) const {
    if (isa<ATTR>(Attr) && (Attr->isValid() || AllowInvalid))
      return cast<ATTR>(Attr);
    return std::nullopt;
  }
};

/// The @_allowFeatureSuppression(Foo, Bar) attribute.  The feature
/// names are intentionally not validated, and the attribute itself is
/// not printed when rendering a module interface.
class AllowFeatureSuppressionAttr final
    : public DeclAttribute,
      private llvm::TrailingObjects<AllowFeatureSuppressionAttr, Identifier> {
  friend TrailingObjects;

  AllowFeatureSuppressionAttr(SourceLoc atLoc, SourceRange range, bool implicit,
                              bool inverted, ArrayRef<Identifier> features);

public:
  static AllowFeatureSuppressionAttr *create(ASTContext &ctx, SourceLoc atLoc,
                                             SourceRange range, bool implicit,
                                             bool inverted,
                                             ArrayRef<Identifier> features);

  bool getInverted() const { return Bits.AllowFeatureSuppressionAttr.Inverted; }

  ArrayRef<Identifier> getSuppressedFeatures() const {
    return {getTrailingObjects<Identifier>(),
            Bits.AllowFeatureSuppressionAttr.NumFeatures};
  }

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DeclAttrKind::AllowFeatureSuppression;
  }
};

/// Attributes that may be applied to declarations.
class DeclAttributes {
  /// Linked list of declaration attributes.
  DeclAttribute *DeclAttrs;

public:
  DeclAttributes() : DeclAttrs(nullptr) {}

  bool isEmpty() const {
    return DeclAttrs == nullptr;
  }

  void getAttrRanges(SmallVectorImpl<SourceRange> &Ranges) const {
    for (auto Attr : *this) {
      auto R = Attr->getRangeWithAt();
      if (R.isValid())
        Ranges.push_back(R);
    }
  }

  /// If this attribute set has a prefix/postfix attribute on it, return this.
  UnaryOperatorKind getUnaryOperatorKind() const {
    if (hasAttribute<PrefixAttr>())
      return UnaryOperatorKind::Prefix;
    if (hasAttribute<PostfixAttr>())
      return UnaryOperatorKind::Postfix;
    return UnaryOperatorKind::None;
  }

  bool isUnavailable(const ASTContext &ctx) const {
    return getUnavailable(ctx) != nullptr;
  }

  /// Determine whether there is a swiftVersionSpecific attribute that's
  /// unavailable relative to the provided language version.
  bool
  isUnavailableInSwiftVersion(const version::Version &effectiveVersion) const;

  /// Finds the most-specific platform-specific attribute that is
  /// active for the current platform.
  const AvailableAttr *
  findMostSpecificActivePlatform(const ASTContext &ctx,
                                 bool ignoreAppExtensions = false) const;

  /// Returns the first @available attribute that indicates
  /// a declaration is unavailable, or the first one that indicates it's
  /// potentially unavailable, or null otherwise.
  const AvailableAttr *getPotentiallyUnavailable(const ASTContext &ctx) const;

  /// Returns the first @available attribute that indicates
  /// a declaration is unavailable, or null otherwise.
  const AvailableAttr *getUnavailable(const ASTContext &ctx,
                                      bool ignoreAppExtensions = false) const;

  /// Returns the first @available attribute that indicates
  /// a declaration is deprecated on all deployment targets, or null otherwise.
  const AvailableAttr *getDeprecated(const ASTContext &ctx) const;

  /// Returns the first @available attribute that indicates
  /// a declaration will be deprecated in the future, or null otherwise.
  const AvailableAttr *getSoftDeprecated(const ASTContext &ctx) const;

  /// Returns the first @available attribute that indicates
  /// a declaration is unavailable from asynchronous contexts, or null
  /// otherwise.
  const AvailableAttr *getNoAsync(const ASTContext &ctx) const;

  /// Returns the `@backDeployed` attribute that is active for the current
  /// platform.
  const BackDeployedAttr *getBackDeployed(const ASTContext &ctx) const;

  SWIFT_DEBUG_DUMPER(dump(const Decl *D = nullptr));
  void print(ASTPrinter &Printer, const PrintOptions &Options,
             const Decl *D = nullptr) const;
  static void print(ASTPrinter &Printer, const PrintOptions &Options,
                    ArrayRef<const DeclAttribute *> FlattenedAttrs,
                    const Decl *D = nullptr);

  template <typename T, typename DERIVED>
  class iterator_base {
    T *Impl;
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = T*;
    using difference_type = std::ptrdiff_t;
    using pointer = value_type*;
    using reference = value_type&;    

    explicit iterator_base(T *Impl) : Impl(Impl) {}
    DERIVED &operator++() { Impl = Impl->Next; return (DERIVED&)*this; }
    bool operator==(const iterator_base &X) const { return X.Impl == Impl; }
    bool operator!=(const iterator_base &X) const { return X.Impl != Impl; }
    T *operator*() const { return Impl; }
    T &operator->() const { return *Impl; }
  };

  /// Add a constructed DeclAttribute to this list.
  void add(DeclAttribute *Attr) {
    Attr->Next = DeclAttrs;
    DeclAttrs = Attr;
  }

  /// Add multiple constructed DeclAttributes to this list.
  void add(DeclAttributes &Attrs) {
    for (auto attr : Attrs) {
      add(attr);
    }
  }

  // Iterator interface over DeclAttribute objects.
  class iterator : public iterator_base<DeclAttribute, iterator> {
  public:
    explicit iterator(DeclAttribute *Impl) : iterator_base(Impl) {}
  };

  class const_iterator : public iterator_base<const DeclAttribute,
                                              const_iterator> {
  public:
    explicit const_iterator(const DeclAttribute *Impl)
        : iterator_base(Impl) {}
  };

  iterator begin() { return iterator(DeclAttrs); }
  iterator end() { return iterator(nullptr); }
  const_iterator begin() const { return const_iterator(DeclAttrs); }
  const_iterator end() const { return const_iterator(nullptr); }

  /// Retrieve the first attribute of the given attribute class.
  template <typename ATTR>
  const ATTR *getAttribute(bool AllowInvalid = false) const {
    return const_cast<DeclAttributes *>(this)->getAttribute<ATTR>(AllowInvalid);
  }

  template <typename ATTR>
  ATTR *getAttribute(bool AllowInvalid = false) {
    for (auto Attr : *this)
      if (auto *SpecificAttr = dyn_cast<ATTR>(Attr))
        if (SpecificAttr->isValid() || AllowInvalid)
          return SpecificAttr;
    return nullptr;
  }

  /// Determine whether there is an attribute with the given attribute class.
  template <typename ATTR>
  bool hasAttribute(bool AllowInvalid = false) const {
    return getAttribute<ATTR>(AllowInvalid) != nullptr;
  }

  /// Retrieve the first attribute with the given kind.
  const DeclAttribute *getAttribute(DeclAttrKind DK,
                                    bool AllowInvalid = false) const {
    for (auto Attr : *this)
      if (Attr->getKind() == DK && (Attr->isValid() || AllowInvalid))
        return Attr;
    return nullptr;
  }

  /// Retrieve the first attribute with the given kind.
  DeclAttribute *getAttribute(DeclAttrKind DK,
                              bool AllowInvalid = false) {
    for (auto Attr : *this)
      if (Attr->getKind() == DK && (Attr->isValid() || AllowInvalid))
        return Attr;
    return nullptr;
  }

  /// Returns the "winning" \c NonSendableAttr or \c SendableAttr in this
  /// attribute list, or \c nullptr if there are none.
  const DeclAttribute *getEffectiveSendableAttr() const;

  DeclAttribute *getEffectiveSendableAttr() {
    return const_cast<DeclAttribute *>(
         const_cast<const DeclAttributes *>(this)->getEffectiveSendableAttr());
  }

public:
  template <typename ATTR, bool AllowInvalid>
  using AttributeKindRange =
      OptionalTransformRange<iterator_range<const_iterator>,
                             ToAttributeKind<ATTR, AllowInvalid>,
                             const_iterator>;

  /// Return a range with all attributes in DeclAttributes with AttrKind
  /// ATTR.
  template <typename ATTR, bool AllowInvalid = false>
  AttributeKindRange<ATTR, AllowInvalid> getAttributes() const {
    return AttributeKindRange<ATTR, AllowInvalid>(
        make_range(begin(), end()), ToAttributeKind<ATTR, AllowInvalid>());
  }

  /// Return the range of semantics attributes attached to this attribute set.
  auto getSemanticsAttrs() const
      -> decltype(getAttributes<SemanticsAttr>()) {
    return getAttributes<SemanticsAttr>();
  }

  /// Return whether this attribute set includes the given semantics attribute.
  bool hasSemanticsAttr(StringRef attrValue) const {
    return llvm::any_of(getSemanticsAttrs(), [&](const SemanticsAttr *attr) {
      return attrValue.equals(attr->Value);
    });
  }

  // Remove the given attribute from the list of attributes. Used when
  // the attribute was semantically invalid.
  void removeAttribute(const DeclAttribute *attr) {
    // If it's the first attribute, remove it.
    if (DeclAttrs == attr) {
      DeclAttrs = attr->Next;
      return;
    }

    // Otherwise, find it in the list. This is inefficient, but rare.
    for (auto **prev = &DeclAttrs; *prev; prev = &(*prev)->Next) {
      if ((*prev)->Next == attr) {
        (*prev)->Next = attr->Next;
        return;
      }
    }
    llvm_unreachable("Attribute not found for removal");
  }

  /// Set the raw chain of attributes.  Used for deserialization.
  void setRawAttributeChain(DeclAttribute *Chain) {
    DeclAttrs = Chain;
  }

  DeclAttribute *getRawAttributeChain() const { return DeclAttrs; }

  SourceLoc getStartLoc(bool forModifiers = false) const;
};

/// Predicate used to filter attributes to only the parsed attributes.
class ParsedDeclAttrFilter {
  const Decl *decl;

public:
  ParsedDeclAttrFilter() : decl(nullptr) {}

  ParsedDeclAttrFilter(const Decl *decl) : decl(decl) {}

  std::optional<const DeclAttribute *>
  operator()(const DeclAttribute *Attr) const;
};

/// Attributes written in source on a declaration.
///
/// We should really just have \c DeclAttributes and \c SemanticDeclAttributes,
/// but currently almost all callers expect the latter. Instead of changing all
/// callers of \c getAttrs, instead provide a way to retrieve the original
/// attributes.
class ParsedDeclAttributes {
public:
  using ParsedFilteredRange =
      OptionalTransformRange<iterator_range<DeclAttributes::const_iterator>,
                             ParsedDeclAttrFilter>;

private:
  ParsedFilteredRange parsedRange;

public:
  ParsedDeclAttributes()
      : parsedRange(make_range(DeclAttributes::const_iterator(nullptr),
                               DeclAttributes::const_iterator(nullptr)),
                    ParsedDeclAttrFilter()) {}

  ParsedDeclAttributes(const DeclAttributes &allAttrs, const Decl *decl)
      : parsedRange(make_range(allAttrs.begin(), allAttrs.end()),
                    ParsedDeclAttrFilter(decl)) {}

  ParsedFilteredRange::iterator begin() const { return parsedRange.begin(); }
  ParsedFilteredRange::iterator end() const { return parsedRange.end(); }

  template <typename AttrType, bool AllowInvalid>
  using AttributeKindRange =
      OptionalTransformRange<ParsedFilteredRange,
                             ToAttributeKind<AttrType, AllowInvalid>>;

  template <typename AttrType, bool AllowInvalid = false>
  AttributeKindRange<AttrType, AllowInvalid> getAttributes() const {
    return AttributeKindRange<AttrType, AllowInvalid>(
        parsedRange, ToAttributeKind<AttrType, AllowInvalid>());
  }

  /// Retrieve the first attribute of the given attribute class.
  template <typename AttrType>
  const AttrType *getAttribute(bool allowInvalid = false) const {
    for (auto *attr : parsedRange) {
      if (auto *specificAttr = dyn_cast<AttrType>(attr)) {
        if (specificAttr->isValid() || allowInvalid)
          return specificAttr;
      }
    }
    return nullptr;
  }

  /// Determine whether there is an attribute with the given attribute class.
  template <typename AttrType>
  bool hasAttribute(bool allowInvalid = false) const {
    return getAttribute<AttrType>(allowInvalid) != nullptr;
  }
};

class alignas(1 << AttrAlignInBits) TypeAttribute
    : public ASTAllocated<TypeAttribute> {
protected:
  // clang-format off
  union {
    uint64_t OpaqueBits;

    SWIFT_INLINE_BITFIELD_BASE(TypeAttribute, bitmax(NumTypeAttrKindBits,8)+1+1,
      Kind : bitmax(NumTypeAttrKindBits,8),
      Implicit : 1,
      Invalid : 1
    );

    SWIFT_INLINE_BITFIELD(DifferentiableTypeAttr, TypeAttribute, 8,
      Differentiability : 8
    );

    SWIFT_INLINE_BITFIELD_FULL(OpaqueReturnTypeOfTypeAttr, TypeAttribute, 32,
      : NumPadBits,
      Index : 32
    );

    SWIFT_INLINE_BITFIELD_FULL(IsolatedTypeAttr, TypeAttribute, 8,
      Kind : 8
    );
  } Bits;
  // clang-format on

  SourceLoc AttrLoc;

  TypeAttribute(TypeAttrKind kind, SourceLoc attrLoc) : AttrLoc(attrLoc) {
    Bits.TypeAttribute.Kind = unsigned(kind);
    Bits.TypeAttribute.Implicit = false;
    Bits.TypeAttribute.Invalid = false;
  }

public:
  TypeAttrKind getKind() const {
    return TypeAttrKind(Bits.TypeAttribute.Kind);
  }
  const char *getAttrName() const {
    return getAttrName(getKind());
  }

  bool isInvalid() const {
    return Bits.TypeAttribute.Invalid;
  }

  bool isImplicit() const {
    return Bits.TypeAttribute.Implicit;
  }

  void setInvalid() {
    Bits.TypeAttribute.Invalid = true;
  }

  void setImplicit() {
    Bits.TypeAttribute.Implicit = true;
  }

  /// Return the location of the attribute identifier / keyword.
  SourceLoc getAttrLoc() const {
    return AttrLoc;
  }

  SourceLoc getStartLoc() const;
  SourceLoc getEndLoc() const;
  SourceRange getSourceRange() const;

  /// Given a name like "autoclosure", return the type attribute ID that
  /// corresponds to it.
  static std::optional<TypeAttrKind> getAttrKindFromString(StringRef Str);

  /// Returns true if type attributes of the given kind only appear in SIL.
  static bool isSilOnly(TypeAttrKind TK);

  /// Return the name (like "autoclosure") for an attribute ID.
  static const char *getAttrName(TypeAttrKind kind);

  static TypeAttribute *createSimple(const ASTContext &context,
                                     TypeAttrKind kind,
                                     SourceLoc atLoc,
                                     SourceLoc attrLoc);

  SWIFT_DEBUG_DUMPER(dump());
  void print(ASTPrinter &Printer, const PrintOptions &Options) const;
};

class AtTypeAttrBase : public TypeAttribute {
  SourceLoc AtLoc;
public:
  AtTypeAttrBase(TypeAttrKind kind, SourceLoc atLoc, SourceLoc attrLoc)
    : TypeAttribute(kind, attrLoc), AtLoc(atLoc) {}

  SourceLoc getAtLoc() const { return AtLoc; }

  SourceLoc getStartLocImpl() const { return AtLoc; }
  SourceLoc getEndLocImpl() const { return getAttrLoc(); }
};

class AtTypeAttrWithArgsBase : public AtTypeAttrBase {
  SourceRange Parens;
public:
  AtTypeAttrWithArgsBase(TypeAttrKind kind, SourceLoc atLoc, SourceLoc idLoc,
                         SourceRange parens)
    : AtTypeAttrBase(kind, atLoc, idLoc), Parens(parens) {}

  SourceRange getParensRange() const { return Parens; }

  SourceLoc getEndLocImpl() const { return Parens.End; }
};

template <TypeAttrKind Kind, class Base = AtTypeAttrBase>
class SimpleTypeAttr : public Base {
public:
  template <class... Args>
  SimpleTypeAttr(Args ...args) : Base(Kind, args...) {}

  static constexpr TypeAttrKind StaticKind = Kind;

  static bool classof(const TypeAttribute *attr) {
    return attr->getKind() == Kind;
  }
};

template <TypeAttrKind Kind>
using SimpleTypeAttrWithArgs = SimpleTypeAttr<Kind, AtTypeAttrWithArgsBase>;

#define SIMPLE_TYPE_ATTR(SPELLING, CLASS)                                      \
  using CLASS##TypeAttr = SimpleTypeAttr<TypeAttrKind::CLASS>;
#include "swift/AST/TypeAttr.def"

class ConventionTypeAttr
    : public SimpleTypeAttrWithArgs<TypeAttrKind::Convention> {
  Located<StringRef> Name;
  DeclNameRef WitnessMethodProtocol;
  Located<StringRef> ClangType;

public:
  ConventionTypeAttr(SourceLoc atLoc, SourceLoc kwLoc,
                     SourceRange parens,
                     Located<StringRef> name,
                     DeclNameRef witnessMethodProtocol,
                     Located<StringRef> clangType)
    : SimpleTypeAttr(atLoc, kwLoc, parens),
      Name(name),
      WitnessMethodProtocol(witnessMethodProtocol),
      ClangType(clangType) {}

  StringRef getConventionName() const { return Name.Item; }
  SourceLoc getConventionLoc() const { return Name.Loc; }
  std::optional<StringRef> getClangType() const {
    if (!ClangType.Item.empty()) return ClangType.Item;
    return {};
  }
  SourceLoc getClangTypeLoc() const { return ClangType.Loc; }
  DeclNameRef getWitnessMethodProtocol() const {
    return WitnessMethodProtocol;
  }

  void printImpl(ASTPrinter &printer, const PrintOptions &options) const;
};

class DifferentiableTypeAttr
    : public SimpleTypeAttrWithArgs<TypeAttrKind::Differentiable> {
  SourceLoc DifferentiabilityLoc;
public:
  DifferentiableTypeAttr(SourceLoc atLoc, SourceLoc kwLoc,
                         SourceRange parensRange,
                         Located<DifferentiabilityKind> differentiability)
    : SimpleTypeAttr(atLoc, kwLoc, parensRange),
      DifferentiabilityLoc(differentiability.Loc) {
    Bits.DifferentiableTypeAttr.Differentiability =
      unsigned(differentiability.Item);
  }

  SourceLoc getEndLocImpl() const {
    // "Override" this method to handle that @differentiable doesn't
    // always take an argument.
    if (getParensRange().isValid()) return getParensRange().End;
    return getAttrLoc();
  }

  DifferentiabilityKind getDifferentiability() const {
    return DifferentiabilityKind(Bits.DifferentiableTypeAttr.Differentiability);
  }
  SourceLoc getDifferentiabilityLoc() const {
    return DifferentiabilityLoc;
  }

  void printImpl(ASTPrinter &printer, const PrintOptions &options) const;
};

class OpaqueReturnTypeOfTypeAttr
    : public SimpleTypeAttrWithArgs<TypeAttrKind::OpaqueReturnTypeOf> {
  Located<StringRef> MangledName;
  SourceLoc IndexLoc;
public:
  OpaqueReturnTypeOfTypeAttr(SourceLoc atLoc, SourceLoc kwLoc, SourceRange parens,
                             Located<StringRef> mangledName,
                             Located<unsigned> index)
    : SimpleTypeAttr(atLoc, kwLoc, parens), MangledName(mangledName) {
    Bits.OpaqueReturnTypeOfTypeAttr.Index = index.Item;
  }

  StringRef getMangledName() const { return MangledName.Item; }
  SourceLoc getMangledNameLoc() const { return MangledName.Loc; }
  unsigned getIndex() const { return Bits.OpaqueReturnTypeOfTypeAttr.Index; }
  SourceLoc getIndexLoc() const { return IndexLoc; }

  void printImpl(ASTPrinter &printer, const PrintOptions &options) const;
};

class OpenedTypeAttr : public SimpleTypeAttrWithArgs<TypeAttrKind::Opened> {
  Located<UUID> ID;
  TypeRepr *ConstraintType;
public:
  OpenedTypeAttr(SourceLoc atLoc, SourceLoc kwLoc, SourceRange parensRange,
                 Located<UUID> id, TypeRepr *constraintType)
    : SimpleTypeAttr(atLoc, kwLoc, parensRange),
      ID(id), ConstraintType(constraintType) {}

  UUID getUUID() const {
    return ID.Item;
  }
  SourceLoc getUUIDLoc() const {
    return ID.Loc;
  }
  TypeRepr *getConstraintType() const {
    return ConstraintType;
  }

  void printImpl(ASTPrinter &printer, const PrintOptions &options) const;
};

class PackElementTypeAttr
    : public SimpleTypeAttrWithArgs<TypeAttrKind::PackElement> {
  Located<UUID> ID;
public:
  PackElementTypeAttr(SourceLoc atLoc, SourceLoc kwLoc, SourceRange parensRange,
                      Located<UUID> id)
    : SimpleTypeAttr(atLoc, kwLoc, parensRange), ID(id) {}

  UUID getUUID() const {
    return ID.Item;
  }
  SourceLoc getUUIDLoc() const {
    return ID.Loc;
  }

  void printImpl(ASTPrinter &printer, const PrintOptions &options) const;
};

/// The @isolated function type attribute, not to be confused with the
/// `isolated` declaration modifier (IsolatedAttr) or the `isolated`
/// parameter specifier (IsolatedTypeRepr).
class IsolatedTypeAttr : public SimpleTypeAttrWithArgs<TypeAttrKind::Isolated> {
public:
  enum class IsolationKind : uint8_t {
    Dynamic
  };

private:
  SourceLoc KindLoc;

public:
  IsolatedTypeAttr(SourceLoc atLoc, SourceLoc kwLoc, SourceRange parensRange,
                   Located<IsolationKind> kind)
    : SimpleTypeAttr(atLoc, kwLoc, parensRange), KindLoc(kind.Loc) {
    Bits.IsolatedTypeAttr.Kind = uint8_t(kind.Item);
  }

  IsolationKind getIsolationKind() const {
    return IsolationKind(Bits.IsolatedTypeAttr.Kind);
  }
  SourceLoc getIsolationKindLoc() const {
    return KindLoc;
  }
  const char *getIsolationKindName() const {
    return getIsolationKindName(getIsolationKind());
  }
  static const char *getIsolationKindName(IsolationKind kind);

  void printImpl(ASTPrinter &printer, const PrintOptions &options) const;
};

using TypeOrCustomAttr =
  llvm::PointerUnion<CustomAttr*, TypeAttribute*>;

void simple_display(llvm::raw_ostream &out, const DeclAttribute *attr);

inline SourceLoc extractNearestSourceLoc(const DeclAttribute *attr) {
  return attr->getLocation();
}

/// Determine whether the given attribute is available, looking up the
/// attribute by name.
bool hasAttribute(const LangOptions &langOpts, llvm::StringRef attributeName);

template <>
struct EnumTraits<TypeAttrKind> {
  static constexpr size_t NumValues = NumTypeAttrKinds;
};

} // end namespace swift

#endif
