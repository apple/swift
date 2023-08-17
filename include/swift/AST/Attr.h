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
#include "swift/AST/Requirement.h"
#include "swift/AST/StorageImpl.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/InlineBitfield.h"
#include "swift/Basic/Located.h"
#include "swift/Basic/OptimizationMode.h"
#include "swift/Basic/Range.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Basic/SourceLoc.h"
#include "swift/Basic/UUID.h"
#include "swift/Basic/Version.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/TrailingObjects.h"
#include "llvm/Support/VersionTuple.h"

namespace swift {
class ArgumentList;
class ASTPrinter;
class ASTContext;
struct PrintOptions;
class CustomAttr;
class Decl;
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

class alignas(1 << AttrAlignInBits) AttributeBase
    : public ASTAllocated<AttributeBase> {
public:
  /// The location of the '@'.
  const SourceLoc AtLoc;

  /// The source range of the attribute.
  const SourceRange Range;

  /// The location of the attribute.
  SourceLoc getLocation() const { return Range.Start; }

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
  friend class TypeAttributes;

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

    SWIFT_INLINE_BITFIELD(ObjCAttr, DeclAttribute, 1+1+1,
      /// Whether this attribute has location information that trails the main
      /// record, which contains the locations of the parentheses and any names.
      HasTrailingLocationInfo : 1,

      /// Whether the name is implicit, produced as the result of caching.
      ImplicitName : 1,

      /// Whether the @objc was inferred using Swift 3's deprecated inference
      /// rules.
      Swift3Inferred : 1
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

    SWIFT_INLINE_BITFIELD(SynthesizedProtocolAttr, DeclAttribute, 1,
      isUnchecked : 1
    );

    SWIFT_INLINE_BITFIELD(ObjCImplementationAttr, DeclAttribute, 1,
      isCategoryNameInvalid : 1
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
#include "swift/AST/Attr.def"

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
  /// and decl modifiers like 'final'.  This returns DAK_Count on failure.
  ///
  static DeclAttrKind getAttrKindFromString(StringRef Str);
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
#define SIMPLE_DECL_ATTR(_, CLASS, ...) \
 typedef SimpleDeclAttr<DAK_##CLASS> CLASS##Attr;
#include "swift/AST/Attr.def"

/// Defines the @_silgen_name attribute.
class SILGenNameAttr : public DeclAttribute {
public:
  SILGenNameAttr(StringRef Name, bool Raw, SourceLoc AtLoc, SourceRange Range, bool Implicit)
    : DeclAttribute(DAK_SILGenName, AtLoc, Range, Implicit),
      Name(Name), Raw(Raw) {}

  SILGenNameAttr(StringRef Name, bool Raw, bool Implicit)
    : SILGenNameAttr(Name, Raw, SourceLoc(), SourceRange(), Implicit) {}

  /// The symbol name.
  const StringRef Name;

  /// If true, the name is not to be mangled (underscore prefix on Darwin)
  bool Raw;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DAK_SILGenName;
  }
};

/// Defines the @_section attribute.
class SectionAttr : public DeclAttribute {
public:
  SectionAttr(StringRef Name, SourceLoc AtLoc, SourceRange Range, bool Implicit)
    : DeclAttribute(DAK_Section, AtLoc, Range, Implicit),
      Name(Name) {}

  SectionAttr(StringRef Name, bool Implicit)
    : SectionAttr(Name, SourceLoc(), SourceRange(), Implicit) {}

  /// The section name.
  const StringRef Name;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DAK_Section;
  }
};

/// Defines the @_cdecl attribute.
class CDeclAttr : public DeclAttribute {
public:
  CDeclAttr(StringRef Name, SourceLoc AtLoc, SourceRange Range, bool Implicit)
    : DeclAttribute(DAK_CDecl, AtLoc, Range, Implicit),
      Name(Name) {}

  CDeclAttr(StringRef Name, bool Implicit)
    : CDeclAttr(Name, SourceLoc(), SourceRange(), Implicit) {}

  /// The symbol name.
  const StringRef Name;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DAK_CDecl;
  }
};

/// Defines the @_semantics attribute.
class SemanticsAttr : public DeclAttribute {
public:
  SemanticsAttr(StringRef Value, SourceLoc AtLoc, SourceRange Range,
                bool Implicit)
  : DeclAttribute(DAK_Semantics, AtLoc, Range, Implicit),
  Value(Value) {}

  SemanticsAttr(StringRef Value, bool Implicit)
  : SemanticsAttr(Value, SourceLoc(), SourceRange(), Implicit) {}

  /// The semantics tag value.
  const StringRef Value;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DAK_Semantics;
  }
};

/// Defines the @_alignment attribute.
class AlignmentAttr : public DeclAttribute {
public:
  AlignmentAttr(unsigned Value, SourceLoc AtLoc, SourceRange Range,
                bool Implicit)
      : DeclAttribute(DAK_Alignment, AtLoc, Range, Implicit) {
    Bits.AlignmentAttr.Value = Value;
  }

  unsigned getValue() const { return Bits.AlignmentAttr.Value; }
  
  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DAK_Alignment;
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
  SwiftNativeObjCRuntimeBaseAttr(Identifier BaseClassName,
                                 SourceLoc AtLoc, SourceRange Range,
                                 bool Implicit)
    : DeclAttribute(DAK_SwiftNativeObjCRuntimeBase, AtLoc, Range, Implicit),
      BaseClassName(BaseClassName) {}
  
  // The base class's name.
  const Identifier BaseClassName;
  
  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DAK_SwiftNativeObjCRuntimeBase;
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
#define INIT_VER_TUPLE(X)                                                      \
  X(X.empty() ? llvm::Optional<llvm::VersionTuple>() : X)

  AvailableAttr(SourceLoc AtLoc, SourceRange Range,
                   PlatformKind Platform,
                   StringRef Message, StringRef Rename, ValueDecl *RenameDecl,
                   const llvm::VersionTuple &Introduced,
                   SourceRange IntroducedRange,
                   const llvm::VersionTuple &Deprecated,
                   SourceRange DeprecatedRange,
                   const llvm::VersionTuple &Obsoleted,
                   SourceRange ObsoletedRange,
                   PlatformAgnosticAvailabilityKind PlatformAgnostic,
                   bool Implicit,
                   bool IsSPI)
    : DeclAttribute(DAK_Available, AtLoc, Range, Implicit),
      Message(Message), Rename(Rename), RenameDecl(RenameDecl),
      INIT_VER_TUPLE(Introduced), IntroducedRange(IntroducedRange),
      INIT_VER_TUPLE(Deprecated), DeprecatedRange(DeprecatedRange),
      INIT_VER_TUPLE(Obsoleted), ObsoletedRange(ObsoletedRange),
      PlatformAgnostic(PlatformAgnostic),
      Platform(Platform),
      IsSPI(IsSPI)
  {}

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
  const llvm::Optional<llvm::VersionTuple> Introduced;

  /// Indicates where the Introduced version was specified.
  const SourceRange IntroducedRange;

  /// Indicates when the symbol was deprecated.
  const llvm::Optional<llvm::VersionTuple> Deprecated;

  /// Indicates where the Deprecated version was specified.
  const SourceRange DeprecatedRange;

  /// Indicates when the symbol was obsoleted.
  const llvm::Optional<llvm::VersionTuple> Obsoleted;

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
    return DA->getKind() == DAK_Available;
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
  explicit ObjCAttr(llvm::Optional<ObjCSelector> name, bool implicitName)
      : DeclAttribute(DAK_ObjC, SourceLoc(), SourceRange(), /*Implicit=*/true),
        NameData(nullptr) {
    Bits.ObjCAttr.HasTrailingLocationInfo = false;
    Bits.ObjCAttr.ImplicitName = implicitName;
    Bits.ObjCAttr.Swift3Inferred = false;

    if (name) {
      NameData = name->getOpaqueValue();
    }
  }

  /// Create an @objc attribute written in the source.
  ObjCAttr(SourceLoc atLoc, SourceRange baseRange,
           llvm::Optional<ObjCSelector> name, SourceRange parenRange,
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
  static ObjCAttr *create(ASTContext &Ctx, llvm::Optional<ObjCSelector> name,
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
  llvm::Optional<ObjCSelector> getName() const {
    if (!hasName())
      return llvm::None;

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

  /// Determine whether this attribute was inferred based on Swift 3's
  /// deprecated @objc inference rules.
  bool isSwift3Inferred() const {
    return Bits.ObjCAttr.Swift3Inferred;
  }

  /// Set whether this attribute was inferred based on Swift 3's deprecated
  /// @objc inference rules.
  void setSwift3Inferred(bool inferred = true) {
    Bits.ObjCAttr.Swift3Inferred = inferred;
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
    return DA->getKind() == DAK_ObjC;
  }
};

class MainTypeAttr final : public DeclAttribute {
public:
  MainTypeAttr(bool isImplicit)
      : DeclAttribute(DAK_MainType, SourceLoc(), SourceLoc(), isImplicit) {}

  MainTypeAttr(SourceLoc AtLoc, SourceLoc NameLoc)
      : DeclAttribute(DAK_MainType, AtLoc,
                      SourceRange(AtLoc.isValid() ? AtLoc : NameLoc, NameLoc),
                      /*Implicit=*/false) {}

  MainTypeAttr(SourceLoc NameLoc)
      : DeclAttribute(DAK_MainType, SourceLoc(), SourceRange(NameLoc, NameLoc),
                      /*Implicit=*/false) {}

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DAK_MainType;
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
    return DA->getKind() == DAK_PrivateImport;
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
      : DeclAttribute(DAK_DynamicReplacement, SourceLoc(), SourceRange(),
                      /*Implicit=*/false),
        ReplacedFunctionName(name),
        Resolver(nullptr), ResolverContextData(0) {
    Bits.DynamicReplacementAttr.HasTrailingLocationInfo = false;
  }

  DynamicReplacementAttr(DeclNameRef name,
                         LazyMemberLoader *Resolver = nullptr,
                         uint64_t Data = 0)
      : DeclAttribute(DAK_DynamicReplacement, SourceLoc(), SourceRange(),
                      /*Implicit=*/false),
        ReplacedFunctionName(name),
        Resolver(Resolver), ResolverContextData(Data) {
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
    return DA->getKind() == DAK_DynamicReplacement;
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
      : DeclAttribute(DAK_TypeEraser, atLoc, range, /*Implicit=*/false),
        TypeEraserExpr(typeEraserExpr),
        Resolver(Resolver), ResolverContextData(Data) {}

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
    return DA->getKind() == DAK_TypeEraser;
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
    return DA->getKind() == DAK_AccessControl ||
           DA->getKind() == DAK_SetterAccess;
  }
};

/// Represents a 'private', 'internal', or 'public' marker on a declaration.
class AccessControlAttr : public AbstractAccessControlAttr {
public:
  AccessControlAttr(SourceLoc atLoc, SourceRange range, AccessLevel access,
                    bool implicit = false)
      : AbstractAccessControlAttr(DAK_AccessControl, atLoc, range, access,
                                  implicit) {}

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DAK_AccessControl;
  }
};

/// Represents a 'private', 'internal', or 'public' marker for a setter on a
/// declaration.
class SetterAccessAttr : public AbstractAccessControlAttr {
public:
  SetterAccessAttr(SourceLoc atLoc, SourceRange range,
                          AccessLevel access, bool implicit = false)
      : AbstractAccessControlAttr(DAK_SetterAccess, atLoc, range, access,
                                  implicit) {}

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DAK_SetterAccess;
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
    return DA->getKind() == DAK_SPIAccessControl;
  }
};

/// Represents an inline attribute.
class InlineAttr : public DeclAttribute {
public:
  InlineAttr(SourceLoc atLoc, SourceRange range, InlineKind kind)
      : DeclAttribute(DAK_Inline, atLoc, range, /*Implicit=*/false) {
    Bits.InlineAttr.kind = unsigned(kind);
  }

  InlineAttr(InlineKind kind)
    : InlineAttr(SourceLoc(), SourceRange(), kind) {}

  InlineKind getKind() const { return InlineKind(Bits.InlineAttr.kind); }
  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DAK_Inline;
  }
};

/// Represents the optimize attribute.
class OptimizeAttr : public DeclAttribute {
public:
  OptimizeAttr(SourceLoc atLoc, SourceRange range, OptimizationMode mode)
      : DeclAttribute(DAK_Optimize, atLoc, range, /*Implicit=*/false) {
    Bits.OptimizeAttr.mode = unsigned(mode);
  }

  OptimizeAttr(OptimizationMode mode)
    : OptimizeAttr(SourceLoc(), SourceRange(), mode) {}

  OptimizationMode getMode() const {
    return OptimizationMode(Bits.OptimizeAttr.mode);
  }
  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DAK_Optimize;
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
     : DeclAttribute(DAK_Exclusivity, atLoc, range, /*Implicit=*/false),
       mode(mode) {}

  ExclusivityAttr(Mode mode)
    : ExclusivityAttr(SourceLoc(), SourceRange(), mode) {}

  Mode getMode() const { return mode; }

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DAK_Exclusivity;
  }
};

/// Represents the side effects attribute.
class EffectsAttr : public DeclAttribute {
  StringRef customString;
  SourceLoc customStringLoc;

public:
  EffectsAttr(SourceLoc atLoc, SourceRange range, EffectsKind kind)
      : DeclAttribute(DAK_Effects, atLoc, range, /*Implicit=*/false) {
    Bits.EffectsAttr.kind = unsigned(kind);
  }

  EffectsAttr(SourceLoc atLoc, SourceRange range, StringRef customString,
              SourceLoc customStringLoc)
      : DeclAttribute(DAK_Effects, atLoc, range, /*Implicit=*/false),
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
    return DA->getKind() == DAK_Effects;
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
      : DeclAttribute(DAK_ReferenceOwnership, range.Start, range,
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
    return DA->getKind() == DAK_ReferenceOwnership;
  }
};

/// Defines the attribute that we use to model documentation comments.
class RawDocCommentAttr : public DeclAttribute {
  /// Source range of the attached comment.  This comment is located before
  /// the declaration.
  CharSourceRange CommentRange;

public:
  RawDocCommentAttr(CharSourceRange CommentRange)
      : DeclAttribute(DAK_RawDocComment, SourceLoc(), SourceRange(),
                      /*Implicit=*/false),
        CommentRange(CommentRange) {}

  CharSourceRange getCommentRange() const { return CommentRange; }

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DAK_RawDocComment;
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
    : DeclAttribute(DAK_ObjCBridged, SourceLoc(), SourceRange(),
                    /*Implicit=*/true),
      ObjCClass(ObjCClass)
  {
  }

  /// Retrieve the Objective-C class to which this foreign class is toll-free
  /// bridged.
  ClassDecl *getObjCClass() const { return ObjCClass; }

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DAK_ObjCBridged;
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
  SynthesizedProtocolAttr(ProtocolDecl *protocol,
                          LazyConformanceLoader *Loader,
                          bool isUnchecked)
    : DeclAttribute(DAK_SynthesizedProtocol, SourceLoc(), SourceRange(),
                    /*Implicit=*/true), Loader(Loader), protocol(protocol)
  {
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
    return DA->getKind() == DAK_SynthesizedProtocol;
  }
};

/// The @_specialize attribute, which forces specialization on the specified
/// type list.
class SpecializeAttr final
    : public DeclAttribute,
      private llvm::TrailingObjects<SpecializeAttr, Identifier,
                                    AvailableAttr *, Type> {
  friend class SpecializeAttrTargetDeclRequest;
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

  GenericSignature getSpecializedSignature() const {
    return specializedSignature;
  }

  void setSpecializedSignature(GenericSignature newSig) {
    specializedSignature = newSig;
  }

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

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DAK_Specialize;
  }
};

class InitializesAttr final
    : public DeclAttribute,
      private llvm::TrailingObjects<InitializesAttr, Identifier> {
  friend TrailingObjects;

  size_t numProperties;

  InitializesAttr(SourceLoc atLoc, SourceRange range,
                  ArrayRef<Identifier> properties);

public:
  static InitializesAttr *create(ASTContext &ctx,
                                 SourceLoc atLoc, SourceRange range,
                                 ArrayRef<Identifier> properties);

  size_t numTrailingObjects(OverloadToken<Identifier>) const {
    return numProperties;
  }

  unsigned getNumProperties() const { return numProperties; }

  ArrayRef<Identifier> getProperties() const {
    return {getTrailingObjects<Identifier>(), numProperties};
  }

  ArrayRef<VarDecl *> getPropertyDecls(AccessorDecl *attachedTo) const;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DAK_Initializes;
  }
};

class AccessesAttr final
    : public DeclAttribute,
      private llvm::TrailingObjects<AccessesAttr, Identifier> {
  friend TrailingObjects;

  size_t numProperties;

  AccessesAttr(SourceLoc atLoc, SourceRange range,
               ArrayRef<Identifier> properties);

public:
  static AccessesAttr *create(ASTContext &ctx,
                              SourceLoc atLoc, SourceRange range,
                              ArrayRef<Identifier> properties);

  size_t numTrailingObjects(OverloadToken<Identifier>) const {
    return numProperties;
  }

  ArrayRef<Identifier> getProperties() const {
    return {getTrailingObjects<Identifier>(), numProperties};
  }

  ArrayRef<VarDecl *> getPropertyDecls(AccessorDecl *attachedTo) const;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DAK_Accesses;
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
    return DA->getKind() == DAK_StorageRestrictions;
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
    return DA->getKind() == DAK_Implements;
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
    : DeclAttribute(DAK_ObjCRuntimeName, AtLoc, Range, Implicit),
      Name(Name) {}

  explicit ObjCRuntimeNameAttr(const ObjCAttr &Original)
    : ObjCRuntimeNameAttr(getSimpleName(Original), Original.AtLoc,
                          Original.Range, Original.isImplicit()) {}

  const StringRef Name;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DAK_ObjCRuntimeName;
  }
};

/// Attribute that specifies a protocol conformance that has been restated
/// (i.e., is redundant) but should still be emitted in Objective-C metadata.
class RestatedObjCConformanceAttr : public DeclAttribute {
public:
  explicit RestatedObjCConformanceAttr(ProtocolDecl *proto)
    : DeclAttribute(DAK_RestatedObjCConformance, SourceLoc(), SourceRange(),
                    /*Implicit=*/true),
      Proto(proto) {}

  /// The protocol to which this type conforms.
  ProtocolDecl * const Proto;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DAK_RestatedObjCConformance;
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
    : DeclAttribute(DAK_ClangImporterSynthesizedType, SourceLoc(),
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
    return DA->getKind() == DAK_ClangImporterSynthesizedType;
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
    return DA->getKind() == DAK_Custom;
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
  ProjectedValuePropertyAttr(Identifier PropertyName,
                              SourceLoc AtLoc, SourceRange Range,
                              bool Implicit)
    : DeclAttribute(DAK_ProjectedValueProperty, AtLoc, Range, Implicit),
      ProjectionPropertyName(PropertyName) {}

  // The projection property name.
  const Identifier ProjectionPropertyName;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DAK_ProjectedValueProperty;
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
                          StringRef OriginalModuleName,
                          PlatformKind Platform,
                          const llvm::VersionTuple MovedVersion,
                          bool Implicit)
    : DeclAttribute(DAK_OriginallyDefinedIn, AtLoc, Range, Implicit),
      OriginalModuleName(OriginalModuleName),
      Platform(Platform),
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
    bool IsSimulator;
    llvm::VersionTuple Version;
  };

  /// Returns non-optional if this attribute is active given the current platform.
  /// The value provides more details about the active platform.
  llvm::Optional<ActiveVersion> isActivePlatform(const ASTContext &ctx) const;
  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DAK_OriginallyDefinedIn;
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
    return DA->getKind() == DAK_Differentiable;
  }
};

/// A declaration name with location.
struct DeclNameRefWithLoc {
  /// The declaration name.
  DeclNameRef Name;
  /// The declaration name location.
  DeclNameLoc Loc;
  /// An optional accessor kind.
  llvm::Optional<AccessorKind> AccessorKind;

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
  llvm::Optional<AutoDiffDerivativeFunctionKind> Kind = llvm::None;

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
    return DA->getKind() == DAK_Derivative;
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
    return DA->getKind() == DAK_Transpose;
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
    : DeclAttribute(DAK_NonSendable, AtLoc, Range, Implicit),
      Specificity(Specificity)
  {}

  NonSendableAttr(NonSendableKind Specificity, bool Implicit = false)
    : NonSendableAttr(SourceLoc(), SourceRange(), Specificity, Implicit) {}

  /// Was this '@_nonSendable(_assumed)'?
  const NonSendableKind Specificity;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DAK_NonSendable;
  }
};

/// The @_unavailableFromAsync attribute, used to make function declarations
/// unavailable from async contexts.
class UnavailableFromAsyncAttr : public DeclAttribute {
public:
  UnavailableFromAsyncAttr(StringRef Message, SourceLoc AtLoc,
                           SourceRange Range, bool Implicit)
      : DeclAttribute(DAK_UnavailableFromAsync, AtLoc, Range, Implicit),
        Message(Message) {}
  UnavailableFromAsyncAttr(StringRef Message, bool Implicit)
      : UnavailableFromAsyncAttr(Message, SourceLoc(), SourceRange(),
                                 Implicit) {}
  const StringRef Message;

  bool hasMessage() const { return !Message.empty(); }

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DAK_UnavailableFromAsync;
  }
};

/// The `@backDeployed(...)` attribute, used to make function declarations
/// available for back deployment to older OSes via emission into the client
/// binary.
class BackDeployedAttr : public DeclAttribute {
public:
  BackDeployedAttr(SourceLoc AtLoc, SourceRange Range, PlatformKind Platform,
                   const llvm::VersionTuple Version, bool Implicit)
      : DeclAttribute(DAK_BackDeployed, AtLoc, Range, Implicit),
        Platform(Platform), Version(Version) {}

  /// The platform the symbol is available for back deployment on.
  const PlatformKind Platform;

  /// The earliest platform version that may use the back deployed implementation.
  const llvm::VersionTuple Version;

  /// Returns true if this attribute is active given the current platform.
  bool isActivePlatform(const ASTContext &ctx) const;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DAK_BackDeployed;
  }
};

/// Defines the `@_expose` attribute, used to expose declarations in the
/// header used by C/C++ to interoperate with Swift.
class ExposeAttr : public DeclAttribute {
public:
  ExposeAttr(StringRef Name, SourceLoc AtLoc, SourceRange Range, bool Implicit)
      : DeclAttribute(DAK_Expose, AtLoc, Range, Implicit), Name(Name) {}

  ExposeAttr(StringRef Name, bool Implicit)
      : ExposeAttr(Name, SourceLoc(), SourceRange(), Implicit) {}

  /// The exposed declaration name.
  const StringRef Name;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DAK_Expose;
  }
};

/// The `@_documentation(...)` attribute, used to override a symbol's visibility
/// in symbol graphs, and/or adding arbitrary metadata to it.
class DocumentationAttr: public DeclAttribute {
public:
  DocumentationAttr(SourceLoc AtLoc, SourceRange Range, StringRef Metadata,
                    llvm::Optional<AccessLevel> Visibility, bool Implicit)
      : DeclAttribute(DAK_Documentation, AtLoc, Range, Implicit),
        Metadata(Metadata), Visibility(Visibility) {}

  DocumentationAttr(StringRef Metadata, llvm::Optional<AccessLevel> Visibility,
                    bool Implicit)
      : DocumentationAttr(SourceLoc(), SourceRange(), Metadata, Visibility,
                          Implicit) {}

  const StringRef Metadata;
  const llvm::Optional<AccessLevel> Visibility;

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DAK_Documentation;
  }
};

class ObjCImplementationAttr final : public DeclAttribute {
public:
  Identifier CategoryName;

  ObjCImplementationAttr(Identifier CategoryName, SourceLoc AtLoc,
                         SourceRange Range, bool Implicit = false,
                         bool isCategoryNameInvalid = false)
    : DeclAttribute(DAK_ObjCImplementation, AtLoc, Range, Implicit),
      CategoryName(CategoryName) {
    Bits.ObjCImplementationAttr.isCategoryNameInvalid = isCategoryNameInvalid;
  }

  bool isCategoryNameInvalid() const {
    return Bits.ObjCImplementationAttr.isCategoryNameInvalid;
  }

  void setCategoryNameInvalid(bool newValue = true) {
    Bits.ObjCImplementationAttr.isCategoryNameInvalid = newValue;
  }

  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DAK_ObjCImplementation;
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
    return DA->getKind() == DAK_MacroRole;
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
  RawLayoutAttr(TypeRepr *LikeType,
                SourceLoc AtLoc,
                SourceRange Range)
    : DeclAttribute(DAK_RawLayout, AtLoc, Range, /*implicit*/false),
      LikeType(LikeType), SizeOrCount(0), Alignment(~0u)
  {}
  
  /// Construct a `@_rawLayout(likeArrayOf: T, count: N)` attribute.
  RawLayoutAttr(TypeRepr *LikeType, unsigned Count,
                SourceLoc AtLoc,
                SourceRange Range)
    : DeclAttribute(DAK_RawLayout, AtLoc, Range, /*implicit*/false),
      LikeType(LikeType), SizeOrCount(Count), Alignment(0)
  {}
  
  /// Construct a `@_rawLayout(size: N, alignment: M)` attribute.
  RawLayoutAttr(unsigned Size, unsigned Alignment,
                SourceLoc AtLoc,
                SourceRange Range)
    : DeclAttribute(DAK_RawLayout, AtLoc, Range, /*implicit*/false),
      LikeType(nullptr), SizeOrCount(Size), Alignment(Alignment)
  {}
  
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
  llvm::Optional<std::pair<TypeRepr *, unsigned>> getArrayLikeTypeAndCount() const {
    if (!LikeType)
      return llvm::None;
    if (Alignment == ~0u)
      return llvm::None;
    return std::make_pair(LikeType, SizeOrCount);
  }
  
  /// Return the size and alignment of the attributed type. Returns
  /// None if the attribute specifies layout like some other type.
  llvm::Optional<std::pair<unsigned, unsigned>> getSizeAndAlignment() const {
    if (LikeType)
      return llvm::None;
    return std::make_pair(SizeOrCount, Alignment);
  }
  
  Type getResolvedLikeType(StructDecl *sd) const;

  /// Return the type whose single-element layout the attribute type should get
  /// its layout from. Returns None if the attribute specifies an array or manual
  /// layout.
  llvm::Optional<Type> getResolvedScalarLikeType(StructDecl *sd) const {
    if (!LikeType)
      return llvm::None;
    if (Alignment != ~0u)
      return llvm::None;
    return getResolvedLikeType(sd);
  }
  
  /// Return the type whose array layout the attribute type should get its
  /// layout from, along with the size of that array. Returns None if the
  /// attribute specifies scalar or manual layout.
  llvm::Optional<std::pair<Type, unsigned>>
  getResolvedArrayLikeTypeAndCount(StructDecl *sd) const {
    if (!LikeType)
      return llvm::None;
    if (Alignment == ~0u)
      return llvm::None;
    return std::make_pair(getResolvedLikeType(sd), SizeOrCount);
  }
  
  static bool classof(const DeclAttribute *DA) {
    return DA->getKind() == DAK_RawLayout;
  }
};

/// Predicate used to filter MatchingAttributeRange.
template <typename ATTR, bool AllowInvalid> struct ToAttributeKind {
  ToAttributeKind() {}

  llvm::Optional<const ATTR *> operator()(const DeclAttribute *Attr) const {
    if (isa<ATTR>(Attr) && (Attr->isValid() || AllowInvalid))
      return cast<ATTR>(Attr);
    return llvm::None;
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
  findMostSpecificActivePlatform(const ASTContext &ctx) const;

  /// Returns the first @available attribute that indicates
  /// a declaration is unavailable, or the first one that indicates it's
  /// potentially unavailable, or null otherwise.
  const AvailableAttr *getPotentiallyUnavailable(const ASTContext &ctx) const;

  /// Returns the first @available attribute that indicates
  /// a declaration is unavailable, or null otherwise.
  const AvailableAttr *getUnavailable(const ASTContext &ctx) const;

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

  SourceLoc getStartLoc(bool forModifiers = false) const;
};

/// Predicate used to filter attributes to only the original attributes.
class OrigDeclAttrFilter {
  const Decl *decl;

public:
  OrigDeclAttrFilter() : decl(nullptr) {}

  OrigDeclAttrFilter(const Decl *decl) : decl(decl) {}

  llvm::Optional<const DeclAttribute *>
  operator()(const DeclAttribute *Attr) const;
};

/// Attributes applied directly to the declaration.
///
/// We should really just have \c DeclAttributes and \c SemanticDeclAttributes,
/// but currently almost all callers expect the latter. Instead of changing all
/// callers of \c getAttrs, instead provide a way to retrieve the original
/// attributes.
class OrigDeclAttributes {
public:
  using OrigFilteredRange = OptionalTransformRange<iterator_range<DeclAttributes::const_iterator>, OrigDeclAttrFilter>;

private:
  OrigFilteredRange origRange;

public:
  OrigDeclAttributes() : origRange(make_range(DeclAttributes::const_iterator(nullptr), DeclAttributes::const_iterator(nullptr)), OrigDeclAttrFilter()) {}

  OrigDeclAttributes(const DeclAttributes &allAttrs, const Decl *decl) : origRange(make_range(allAttrs.begin(), allAttrs.end()), OrigDeclAttrFilter(decl)) {}

  OrigFilteredRange::iterator begin() const { return origRange.begin(); }
  OrigFilteredRange::iterator end() const { return origRange.end(); }

  template <typename AttrType, bool AllowInvalid>
  using AttributeKindRange =
  OptionalTransformRange<OrigFilteredRange, ToAttributeKind<AttrType, AllowInvalid>>;

  template <typename AttrType, bool AllowInvalid = false>
  AttributeKindRange<AttrType, AllowInvalid> getAttributes() const {
    return AttributeKindRange<AttrType, AllowInvalid>(origRange, ToAttributeKind<AttrType, AllowInvalid>());
  }

  /// Retrieve the first attribute of the given attribute class.
  template <typename AttrType>
  const AttrType *getAttribute(bool allowInvalid = false) const {
    for (auto *attr : origRange) {
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

/// TypeAttributes - These are attributes that may be applied to types.
class TypeAttributes {
  // Get a SourceLoc for every possible attribute that can be parsed in source.
  // the presence of the attribute is indicated by its location being set.
  SourceLoc AttrLocs[TAK_Count];

  /// The custom attributes, in a linked list.
  CustomAttr *CustomAttrs = nullptr;

public:
  /// AtLoc - This is the location of the first '@' in the attribute specifier.
  /// If this is an empty attribute specifier, then this will be an invalid loc.
  SourceLoc AtLoc;

  struct Convention {
    StringRef Name = {};
    DeclNameRef WitnessMethodProtocol = {};
    Located<StringRef> ClangType = Located<StringRef>(StringRef(), {});
    /// Convenience factory function to create a Swift convention.
    ///
    /// Don't use this function if you are creating a C convention as you
    /// probably need a ClangType field as well.
    static Convention makeSwiftConvention(StringRef name) {
      return {name, DeclNameRef(), Located<StringRef>("", {})};
    }
  };

  llvm::Optional<Convention> ConventionArguments;

  DifferentiabilityKind differentiabilityKind =
      DifferentiabilityKind::NonDifferentiable;

  // For an opened existential type, the known ID.
  llvm::Optional<UUID> OpenedID;

  // For an opened existential type, the constraint type.
  llvm::Optional<TypeRepr *> ConstraintType;

  // For a reference to an opaque return type, the mangled name and argument
  // index into the generic signature.
  struct OpaqueReturnTypeRef {
    StringRef mangledName;
    unsigned index;
  };
  llvm::Optional<OpaqueReturnTypeRef> OpaqueReturnTypeOf;

  TypeAttributes() {}

  bool isValid() const { return AtLoc.isValid(); }

  void clearAttribute(TypeAttrKind A) {
    AttrLocs[A] = SourceLoc();
  }

  bool has(TypeAttrKind A) const {
    return getLoc(A).isValid();
  }

  SourceLoc getLoc(TypeAttrKind A) const {
    return AttrLocs[A];
  }

  void setOpaqueReturnTypeOf(StringRef mangling, unsigned index) {
    OpaqueReturnTypeOf = OpaqueReturnTypeRef{mangling, index};
  }

  void setAttr(TypeAttrKind A, SourceLoc L) {
    assert(!L.isInvalid() && "Cannot clear attribute with this method");
    AttrLocs[A] = L;
  }

  void getAttrLocs(SmallVectorImpl<SourceLoc> &Locs) const {
    for (auto Loc : AttrLocs) {
      if (Loc.isValid())
        Locs.push_back(Loc);
    }
  }

  // This attribute list is empty if no attributes are specified.  Note that
  // the presence of the leading @ is not enough to tell, because we want
  // clients to be able to remove attributes they process until they get to
  // an empty list.
  bool empty() const {
    if (CustomAttrs)
      return false;

    for (SourceLoc elt : AttrLocs)
      if (elt.isValid())
        return false;

    return true;
  }

  bool hasConvention() const { return ConventionArguments.has_value(); }

  /// Returns the primary calling convention string.
  ///
  /// Note: For C conventions, this may not represent the full convention.
  StringRef getConventionName() const {
    return ConventionArguments.value().Name;
  }

  /// Show the string enclosed between @convention(..)'s parentheses.
  ///
  /// For example, @convention(foo, bar) will give the string "foo, bar".
  void getConventionArguments(SmallVectorImpl<char> &buffer) const;

  bool hasOwnership() const {
    return getOwnership() != ReferenceOwnership::Strong;
  }
  ReferenceOwnership getOwnership() const {
#define REF_STORAGE(Name, name, ...) \
    if (has(TAK_sil_##name)) return ReferenceOwnership::Name;
#include "swift/AST/ReferenceStorage.def"
    return ReferenceOwnership::Strong;
  }

  void clearOwnership() {
#define REF_STORAGE(Name, name, ...) \
    clearAttribute(TAK_sil_##name);
#include "swift/AST/ReferenceStorage.def"
  }

  bool hasOpenedID() const { return OpenedID.has_value(); }
  UUID getOpenedID() const { return *OpenedID; }

  bool hasConstraintType() const { return ConstraintType.has_value(); }
  TypeRepr *getConstraintType() const { return *ConstraintType; }

  /// Given a name like "autoclosure", return the type attribute ID that
  /// corresponds to it.  This returns TAK_Count on failure.
  ///
  static TypeAttrKind getAttrKindFromString(StringRef Str);

  /// Return the name (like "autoclosure") for an attribute ID.
  static const char *getAttrName(TypeAttrKind kind);

  void addCustomAttr(CustomAttr *attr) {
    attr->Next = CustomAttrs;
    CustomAttrs = attr;
  }

  // Iterator for the custom type attributes.
  class iterator {
    CustomAttr *attr;

  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = CustomAttr*;
    using difference_type = std::ptrdiff_t;
    using pointer = value_type*;
    using reference = value_type&;    

    iterator() : attr(nullptr) { }
    explicit iterator(CustomAttr *attr) : attr(attr) { }

    iterator &operator++() {
      attr = static_cast<CustomAttr *>(attr->Next);
      return *this;
    }

    bool operator==(iterator x) const { return x.attr == attr; }
    bool operator!=(iterator x) const { return x.attr != attr; }

    CustomAttr *operator*() const { return attr; }
    CustomAttr &operator->() const { return *attr; }
  };

  llvm::iterator_range<iterator> getCustomAttrs() const {
    return llvm::make_range(iterator(CustomAttrs), iterator());
  }
};

void simple_display(llvm::raw_ostream &out, const DeclAttribute *attr);

inline SourceLoc extractNearestSourceLoc(const DeclAttribute *attr) {
  return attr->getLocation();
}

/// Determine whether the given attribute is available, looking up the
/// attribute by name.
bool hasAttribute(const LangOptions &langOpts, llvm::StringRef attributeName);

} // end namespace swift

#endif
