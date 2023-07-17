//===--- ModuleFormat.h - The internals of serialized modules ---*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Contains various constants and helper types to deal with serialized
/// modules.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SERIALIZATION_MODULEFORMAT_H
#define SWIFT_SERIALIZATION_MODULEFORMAT_H

#include "swift/AST/Decl.h"
#include "swift/AST/FineGrainedDependencyFormat.h"
#include "swift/AST/Types.h"
#include "llvm/ADT/PointerEmbeddedInt.h"
#include "llvm/Bitcode/BitcodeConvenience.h"
#include "llvm/Bitstream/BitCodes.h"

namespace swift {
class ModuleFile;
class TypeDeserializer;
namespace serialization {

using llvm::PointerEmbeddedInt;
using llvm::BCArray;
using llvm::BCBlob;
using llvm::BCFixed;
using llvm::BCGenericRecordLayout;
using llvm::BCRecordLayout;
using llvm::BCVBR;

/// Magic number for serialized module files.
const unsigned char SWIFTMODULE_SIGNATURE[] = { 0xE2, 0x9C, 0xA8, 0x0E };

/// Alignment of each serialized modules inside a .swift_ast section.
const unsigned char SWIFTMODULE_ALIGNMENT = 4;

/// Serialized module format major version number.
///
/// Always 0 for Swift 1.x - 4.x.
const uint16_t SWIFTMODULE_VERSION_MAJOR = 0;

/// Serialized module format minor version number.
///
/// When the format changes IN ANY WAY, this number should be incremented.
/// To ensure that two separate changes don't silently get merged into one
/// in source control, you should also update the comment to briefly
/// describe what change you made. The content of this comment isn't important;
/// it just ensures a conflict if two people change the module format.
/// Don't worry about adhering to the 80-column limit for this line.
const uint16_t SWIFTMODULE_VERSION_MINOR = 795; // @storageRestrictions attribute

/// A standard hash seed used for all string hashes in a serialized module.
///
/// This is the same as the default used by llvm::djbHash, just provided
/// explicitly here to note that it's part of the format.
const uint32_t SWIFTMODULE_HASH_SEED = 5381;

using DeclIDField = BCFixed<31>;

// TypeID must be the same as DeclID because it is stored in the same way.
using TypeID = DeclID;
using TypeIDField = DeclIDField;

using TypeIDWithBitField = BCFixed<32>;

// ClangTypeID must be the same as DeclID because it is stored in the same way.
using ClangTypeID = TypeID;
using ClangTypeIDField = TypeIDField;

// IdentifierID must be the same as DeclID because it is stored in the same way.
using IdentifierID = DeclID;
using IdentifierIDField = DeclIDField;

// LocalDeclContextID must be the same as DeclID because it is stored in the
// same way.
using LocalDeclContextID = DeclID;
using LocalDeclContextIDField = DeclIDField;

/// Stores either a DeclID or a LocalDeclContextID, using 32 bits.
class DeclContextID {
  int32_t rawValue;
  explicit DeclContextID(int32_t rawValue) : rawValue(rawValue) {}
public:
  DeclContextID() : DeclContextID(0) {}

  static DeclContextID forDecl(DeclID value) {
    assert(value && "should encode null using DeclContextID()");
    assert(llvm::isUInt<31>(value) && "too many DeclIDs");
    return DeclContextID(static_cast<int32_t>(value));
  }
  static DeclContextID forLocalDeclContext(LocalDeclContextID value) {
    assert(value && "should encode null using DeclContextID()");
    assert(llvm::isUInt<31>(value) && "too many LocalDeclContextIDs");
    return DeclContextID(-static_cast<int32_t>(value));
  }

  explicit operator bool() const {
    return rawValue != 0;
  }

  llvm::Optional<DeclID> getAsDeclID() const {
    if (rawValue > 0)
      return DeclID(rawValue);
    return llvm::None;
  }

  llvm::Optional<LocalDeclContextID> getAsLocalDeclContextID() const {
    if (rawValue < 0)
      return LocalDeclContextID(-rawValue);
    return llvm::None;
  }

  static DeclContextID getFromOpaqueValue(uint32_t opaqueValue) {
    return DeclContextID(opaqueValue);
  }
  uint32_t getOpaqueValue() const { return rawValue; }
};

class DeclContextIDField : public BCFixed<32> {
public:
  static DeclContextID convert(uint64_t rawValue) {
    assert(llvm::isUInt<32>(rawValue));
    return DeclContextID::getFromOpaqueValue(rawValue);
  }
};

// ProtocolConformanceID must be the same as DeclID because it is stored
// in the same way.
using ProtocolConformanceID = DeclID;
using ProtocolConformanceIDField = DeclIDField;

// The low two bits of the ProtocolConformanceID determine the kind:
// 00 -- abstract conformance
// 01 -- concrete conformance
// 10 -- pack conformance
struct SerializedProtocolConformanceKind {
  enum  {
    Abstract = 0,
    Concrete = 1,
    Pack = 2,

    Shift = 2,
    Mask = 3
  };
};

// GenericSignatureID must be the same as DeclID because it is stored in the
// same way.
using GenericSignatureID = DeclID;
using GenericSignatureIDField = DeclIDField;

using GenericEnvironmentID = unsigned;
using GenericEnvironmentIDField = BCFixed<32>;

// SubstitutionMapID must be the same as DeclID because it is stored in the
// same way.
using SubstitutionMapID = DeclID;
using SubstitutionMapIDField = DeclIDField;

// ModuleID must be the same as IdentifierID because it is stored the same way.
using ModuleID = IdentifierID;
using ModuleIDField = IdentifierIDField;

// SILLayoutID must be the same as DeclID because it is stored in the same way.
using SILLayoutID = DeclID;
using SILLayoutIDField = DeclIDField;

using BitOffset = PointerEmbeddedInt<unsigned, 31>;
using BitOffsetField = BCFixed<31>;

// CharOffset must be the same as BitOffset because it is stored in the
// same way.
using CharOffset = BitOffset;
using CharOffsetField = BitOffsetField;

using FileSizeField = BCVBR<16>;
using FileModTimeOrContentHashField = BCVBR<16>;
using FileHashField = BCVBR<16>;

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum class OpaqueReadOwnership : uint8_t {
  Owned,
  Borrowed,
  OwnedOrBorrowed,
};
using OpaqueReadOwnershipField = BCFixed<2>;

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum class ReadImplKind : uint8_t {
  Stored = 0,
  Get,
  Inherited,
  Address,
  Read,
};
using ReadImplKindField = BCFixed<3>;

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum class WriteImplKind : uint8_t {
  Immutable = 0,
  Stored,
  StoredWithObservers,
  InheritedWithObservers,
  Set,
  MutableAddress,
  Modify,
};
using WriteImplKindField = BCFixed<3>;

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum class ReadWriteImplKind : uint8_t {
  Immutable = 0,
  Stored,
  MutableAddress,
  MaterializeToTemporary,
  Modify,
  StoredWithDidSet,
  InheritedWithDidSet,
};
using ReadWriteImplKindField = BCFixed<3>;

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum class StaticSpellingKind : uint8_t {
  None = 0,
  KeywordStatic,
  KeywordClass,
};
using StaticSpellingKindField = BCFixed<2>;

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum class FunctionTypeRepresentation : uint8_t {
  Swift = 0,
  Block,
  Thin,
  CFunctionPointer,
};
using FunctionTypeRepresentationField = BCFixed<4>;

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum class DifferentiabilityKind : uint8_t {
  NonDifferentiable = 0,
  Forward,
  Reverse,
  Normal,
  Linear,
};
using DifferentiabilityKindField = BCFixed<3>;

// These IDs must \em not be renumbered or reordered without incrementing the
// module version.
enum class AutoDiffDerivativeFunctionKind : uint8_t {
  JVP = 0,
  VJP
};
using AutoDiffDerivativeFunctionKindField = BCFixed<1>;

enum class ForeignErrorConventionKind : uint8_t {
  ZeroResult,
  NonZeroResult,
  ZeroPreservedResult,
  NilResult,
  NonNilError,
};

using ForeignErrorConventionKindField = BCFixed<3>;

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum class SILFunctionTypeRepresentation : uint8_t {
  Thick = 0,
  Block,
  Thin,
  CFunctionPointer,

  FirstSIL = 8,
  Method = FirstSIL,
  ObjCMethod,
  WitnessMethod,
  Closure,
  CXXMethod,
};
using SILFunctionTypeRepresentationField = BCFixed<4>;

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum class SILCoroutineKind : uint8_t {
  None = 0,
  YieldOnce = 1,
  YieldMany = 2,
};
using SILCoroutineKindField = BCFixed<2>;

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum OperatorKind : uint8_t {
  Infix = 0,
  Prefix,
  Postfix,
  PrecedenceGroup,  // only for cross references
};
// This is currently required to have the same width as AccessorKindField.
using OperatorKindField = BCFixed<4>;

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum AccessorKind : uint8_t {
  Get = 0,
  Set,
  WillSet,
  DidSet,
  Address,
  MutableAddress,
  Read,
  Modify,
  Init,
};
using AccessorKindField = BCFixed<4>;

using AccessorCountField = BCFixed<3>;

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum CtorInitializerKind : uint8_t {
  Designated = 0,
  Convenience = 1,
  Factory = 2,
  ConvenienceFactory = 3,
};
using CtorInitializerKindField = BCFixed<2>;

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum class ParamDeclSpecifier : uint8_t {
  Default = 0,
  InOut = 1,
  Borrowing = 2,
  Consuming = 3,
  LegacyShared = 4,
  LegacyOwned = 5,
};
using ParamDeclSpecifierField = BCFixed<3>;

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum class VarDeclIntroducer : uint8_t {
  Let = 0,
  Var = 1,
  InOut = 2,
};
using VarDeclIntroducerField = BCFixed<2>;

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum class ParameterConvention : uint8_t {
  Indirect_In,
  Indirect_Inout,
  Indirect_InoutAliasable,
  Direct_Owned,
  Direct_Unowned,
  Direct_Guaranteed,
  Indirect_In_Guaranteed,
  Indirect_In_Constant,
  Pack_Owned,
  Pack_Inout,
  Pack_Guaranteed,
};
using ParameterConventionField = BCFixed<4>;

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum class SILParameterDifferentiability : uint8_t {
  DifferentiableOrNotApplicable = 0,
  NotDifferentiable,
};

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum class ResultConvention : uint8_t {
  Indirect,
  Owned,
  Unowned,
  UnownedInnerPointer,
  Autoreleased,
  Pack,
};
using ResultConventionField = BCFixed<3>;

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum class SILResultDifferentiability : uint8_t {
  DifferentiableOrNotApplicable = 0,
  NotDifferentiable,
};

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum MetatypeRepresentation : uint8_t {
  MR_None, MR_Thin, MR_Thick, MR_ObjC
};
using MetatypeRepresentationField = BCFixed<2>;

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum class SelfAccessKind : uint8_t {
  NonMutating = 0,
  Mutating,
  LegacyConsuming,
  Consuming,
  Borrowing
};
using SelfAccessKindField = BCFixed<3>;
  
/// Translates an operator decl fixity to a Serialization fixity, whose values
/// are guaranteed to be stable.
static inline OperatorKind getStableFixity(OperatorFixity fixity) {
  switch (fixity) {
  case OperatorFixity::Prefix:
    return Prefix;
  case OperatorFixity::Postfix:
    return Postfix;
  case OperatorFixity::Infix:
    return Infix;
  }
  llvm_unreachable("Unhandled case in switch");
}

/// Translates a stable Serialization fixity back to an AST operator fixity.
static inline OperatorFixity getASTOperatorFixity(OperatorKind fixity) {
  switch (fixity) {
  case Prefix:
    return OperatorFixity::Prefix;
  case Postfix:
    return OperatorFixity::Postfix;
  case Infix:
    return OperatorFixity::Infix;
  case PrecedenceGroup:
    llvm_unreachable("Not an operator kind");
  }
  llvm_unreachable("Unhandled case in switch");
}

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum GenericRequirementKind : uint8_t {
  SameShape = 0,
  Conformance = 1,
  SameType    = 2,
  Superclass  = 3,
  Layout = 4,
};
using GenericRequirementKindField = BCFixed<3>;

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum LayoutRequirementKind : uint8_t {
  UnknownLayout = 0,
  TrivialOfExactSize = 1,
  TrivialOfAtMostSize = 2,
  Trivial = 3,
  RefCountedObject = 4,
  NativeRefCountedObject = 5,
  Class = 6,
  NativeClass = 7
};
using LayoutRequirementKindField = BCFixed<3>;

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum Associativity : uint8_t {
  NonAssociative = 0,
  LeftAssociative,
  RightAssociative
};
using AssociativityField = BCFixed<2>;

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum ReferenceOwnership : uint8_t {
  Strong = 0,
  Weak,
  Unowned,
  Unmanaged,
};
using ReferenceOwnershipField = BCFixed<2>;

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum class DefaultArgumentKind : uint8_t {
  None = 0,
  Normal,
  FileID,
  FileIDSpelledAsFile,
  FilePath,
  FilePathSpelledAsFile,
  Line,
  Column,
  Function,
  Inherited,
  DSOHandle,
  NilLiteral,
  EmptyArray,
  EmptyDictionary,
  StoredProperty,
};
using DefaultArgumentField = BCFixed<4>;

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum LibraryKind : uint8_t {
  Library = 0,
  Framework
};
using LibraryKindField = BCFixed<1>;

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum class AccessLevel : uint8_t {
  Private = 0,
  FilePrivate,
  Internal,
  Package,
  Public,
  Open,
};
using AccessLevelField = BCFixed<3>;

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum class DeclNameKind: uint8_t {
  Normal,
  Subscript,
  Constructor,
  Destructor
};

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum SpecialIdentifierID : uint8_t {
  /// Special IdentifierID value for the Builtin module.
  BUILTIN_MODULE_ID = 0,
  /// Special IdentifierID value for the current module.
  CURRENT_MODULE_ID,
  /// Special value for the module for imported Objective-C headers.
  OBJC_HEADER_MODULE_ID,
  /// Special value for the special subscript name
  SUBSCRIPT_ID,
  /// Special value for the special constructor name
  CONSTRUCTOR_ID,
  /// Special value for the special destructor name
  DESTRUCTOR_ID,

  /// The number of special Identifier IDs. This value should never be encoded;
  /// it should only be used to count the number of names above. As such, it
  /// is correct and necessary to add new values above this one.
  NUM_SPECIAL_IDS
};

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum class EnumElementRawValueKind : uint8_t {
  /// No raw value serialized.
  None = 0,
  /// Integer literal.
  IntegerLiteral,
  /// TODO: Float, string, char, etc.
};

using EnumElementRawValueKindField = BCFixed<4>;

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum class ResilienceExpansion : uint8_t {
  Minimal = 0,
  Maximal,
};

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum class ImportControl : uint8_t {
  /// `import FooKit`
  Normal = 0,
  /// `@_exported import FooKit`
  Exported,
  /// `@_implementationOnly import FooKit`
  ImplementationOnly,
  /// `internal import FooKit` or more restrictive.
  InternalOrBelow,
  /// `package import FooKit`
  PackageOnly,
};
using ImportControlField = BCFixed<3>;

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum class ClangDeclPathComponentKind : uint8_t {
  Record = 0,
  Enum,
  Namespace,
  Typedef,
  TypedefAnonDecl,
  ObjCInterface,
  ObjCProtocol,
};

enum class GenericEnvironmentKind : uint8_t {
  OpenedExistential,
  OpenedElement
};

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum class MacroRole : uint8_t {
  Expression,
  Declaration,
  Accessor,
  MemberAttribute,
  Member,
  Peer,
  Conformance,
  CodeItem,
  Extension,
};
using MacroRoleField = BCFixed<4>;

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum class MacroIntroducedDeclNameKind : uint8_t {
  Named = 0,
  Overloaded,
  Accessors,
  Prefixed,
  Suffixed,
  Arbitrary,
};
using MacroIntroducedDeclNameKindField = BCFixed<4>;

// These IDs must \em not be renumbered or reordered without incrementing
// the module version.
enum class PluginSearchOptionKind : uint8_t {
  PluginPath,
  ExternalPluginPath,
  LoadPluginLibrary,
  LoadPluginExecutable,
};
using PluginSearchOptionKindField = BCFixed<3>;

// Encodes a VersionTuple:
//
//  Major
//  Minor
//  Subminor
//  HasMinor
//  HasSubminor
#define BC_AVAIL_TUPLE\
    BCVBR<5>,\
    BCVBR<5>,\
    BCVBR<4>,\
    BCFixed<1>,\
    BCFixed<1>

#define LIST_VER_TUPLE_PIECES(X)\
  X##_Major, X##_Minor, X##_Subminor, X##_HasMinor, X##_HasSubminor
#define DEF_VER_TUPLE_PIECES(X) unsigned LIST_VER_TUPLE_PIECES(X)
#define DECODE_VER_TUPLE(X)\
  if (X##_HasMinor) {\
    if (X##_HasSubminor)\
      X = llvm::VersionTuple(X##_Major, X##_Minor, X##_Subminor);\
    else\
      X = llvm::VersionTuple(X##_Major, X##_Minor);\
    }\
  else X = llvm::VersionTuple(X##_Major);
#define ENCODE_VER_TUPLE(X, X_Expr)\
    unsigned X##_Major = 0, X##_Minor = 0, X##_Subminor = 0,\
             X##_HasMinor = 0, X##_HasSubminor = 0;\
    const auto &X##_Val = X_Expr;\
    if (X##_Val.has_value()) {\
      const auto &Y = X##_Val.value();\
      X##_Major = Y.getMajor();\
      X##_Minor = Y.getMinor().value_or(0);\
      X##_Subminor = Y.getSubminor().value_or(0);\
      X##_HasMinor = Y.getMinor().has_value();\
      X##_HasSubminor = Y.getSubminor().has_value();\
    }

/// The various types of blocks that can occur within a serialized Swift
/// module.
///
/// Some of these are shared with the swiftdoc format, which is a stable format.
/// Be very very careful when renumbering them.
enum BlockID {
  /// The module block, which contains all of the other blocks (and in theory
  /// allows a single file to contain multiple modules).
  MODULE_BLOCK_ID = llvm::bitc::FIRST_APPLICATION_BLOCKID,

  /// The control block, which contains all of the information that needs to
  /// be validated prior to committing to loading the serialized module.
  ///
  /// This is part of a stable format and must not be renumbered!
  ///
  /// \sa control_block
  CONTROL_BLOCK_ID,

  /// The input block, which contains all the files this module depends on.
  ///
  /// \sa input_block
  INPUT_BLOCK_ID,

  /// The "decls-and-types" block, which contains all of the declarations that
  /// come from this module.
  ///
  /// Types are also stored here, so that types that just wrap a Decl don't need
  /// a separate entry in the file.
  ///
  /// \sa decls_block
  DECLS_AND_TYPES_BLOCK_ID,

  /// The identifier block, which contains all of the strings used in
  /// identifiers in the module.
  ///
  /// Unlike other blocks in the file, all data within this block is completely
  /// opaque. Offsets into this block should point directly into the blob at a
  /// null-terminated UTF-8 string.
  IDENTIFIER_DATA_BLOCK_ID,

  /// The index block, which contains cross-referencing information for the
  /// module.
  ///
  /// \sa index_block
  INDEX_BLOCK_ID,

  /// The block for SIL functions.
  ///
  /// \sa sil_block
  SIL_BLOCK_ID,

  /// The index block for SIL functions.
  ///
  /// \sa sil_index_block
  SIL_INDEX_BLOCK_ID,

  /// A sub-block of the control block that contains configuration options
  /// needed to successfully load this module.
  ///
  /// \sa options_block
  OPTIONS_BLOCK_ID,

  /// The declaration member-tables index block, a sub-block of the index block.
  ///
  /// \sa decl_member_tables_block
  DECL_MEMBER_TABLES_BLOCK_ID,

  /// The module documentation container block, which contains all other
  /// documentation blocks.
  ///
  /// This is part of a stable format and must not be renumbered!
  MODULE_DOC_BLOCK_ID = 96,

  /// The comment block, which contains documentation comments.
  ///
  /// This is part of a stable format and must not be renumbered!
  ///
  /// \sa comment_block
  COMMENT_BLOCK_ID,

  /// The module source location container block, which contains all other
  /// source location blocks.
  ///
  /// This is part of a stable format and should not be renumbered.
  ///
  /// Though we strive to keep the format stable, breaking the format of
  /// .swiftsourceinfo doesn't have consequences as serious as breaking the
  /// format of .swiftdoc because .swiftsourceinfo file is for local development
  /// use only.
  MODULE_SOURCEINFO_BLOCK_ID = 192,

  /// The source location block, which contains decl locations.
  ///
  /// This is part of a stable format and should not be renumbered.
  ///
  /// Though we strive to keep the format stable, breaking the format of
  /// .swiftsourceinfo doesn't have consequences as serious as breaking the
  /// format
  /// of .swiftdoc because .swiftsourceinfo file is for local development use
  /// only.
  ///
  /// \sa decl_locs_block
  DECL_LOCS_BLOCK_ID,

  /// The incremental dependency information block.
  ///
  /// This is part of a stable format and should not be renumbered.
  INCREMENTAL_INFORMATION_BLOCK_ID =
      fine_grained_dependencies::INCREMENTAL_INFORMATION_BLOCK_ID,
};

/// The record types within the control block.
///
/// Be VERY VERY careful when changing this block; it is also used by the
/// swiftdoc format, which \e must \e remain \e stable. Adding new records is
/// okay---they will be ignored---but modifying existing ones must be done
/// carefully. You may need to update the swiftdoc version in DocFormat.h.
///
/// \sa CONTROL_BLOCK_ID
namespace control_block {
  enum {
    METADATA = 1,
    MODULE_NAME,
    TARGET,
    SDK_NAME,
    REVISION,
    IS_OSSA,
    ALLOWABLE_CLIENT_NAME
  };

  using MetadataLayout = BCRecordLayout<
    METADATA, // ID
    BCFixed<16>, // Module format major version
    BCFixed<16>, // Module format minor version
    BCVBR<8>, // length of "short version string" in the blob
    BCVBR<8>, // length of "short compatibility version string" in the blob
    BCVBR<17>, // User module format major version
    BCVBR<17>, // User module format minor version
    BCVBR<17>, // User module format sub-minor version
    BCVBR<17>, // User module format build version
    BCBlob // misc. version information
  >;

  using ModuleNameLayout = BCRecordLayout<
    MODULE_NAME,
    BCBlob
  >;

  using TargetLayout = BCRecordLayout<
    TARGET,
    BCBlob // LLVM triple
  >;

  using SDKNameLayout = BCRecordLayout<
    SDK_NAME,
    BCBlob
  >;

  using RevisionLayout = BCRecordLayout<
    REVISION,
    BCBlob
  >;

  using IsOSSALayout = BCRecordLayout<
    IS_OSSA,
    BCFixed<1>
  >;

  using AllowableClientLayout = BCRecordLayout<
    ALLOWABLE_CLIENT_NAME,
    BCBlob
  >;
}

/// The record types within the options block (a sub-block of the control
/// block).
///
/// \sa OPTIONS_BLOCK_ID
namespace options_block {
  enum {
    SDK_PATH = 1,
    XCC,
    IS_SIB,
    IS_STATIC_LIBRARY,
    HAS_HERMETIC_SEAL_AT_LINK,
    IS_TESTABLE,
    RESILIENCE_STRATEGY,
    ARE_PRIVATE_IMPORTS_ENABLED,
    IS_IMPLICIT_DYNAMIC_ENABLED,
    IS_BUILT_FROM_INTERFACE,
    IS_ALLOW_MODULE_WITH_COMPILER_ERRORS_ENABLED,
    MODULE_ABI_NAME,
    IS_CONCURRENCY_CHECKED,
    MODULE_PACKAGE_NAME,
    MODULE_EXPORT_AS_NAME,
    PLUGIN_SEARCH_OPTION,
    HAS_CXX_INTEROPERABILITY_ENABLED,
  };

  using SDKPathLayout = BCRecordLayout<
    SDK_PATH,
    BCBlob // path
  >;

  using XCCLayout = BCRecordLayout<
    XCC,
    BCBlob // -Xcc flag, as string
  >;

  using PluginSearchOptionLayout = BCRecordLayout<
    PLUGIN_SEARCH_OPTION,
    PluginSearchOptionKindField, // kind
    BCBlob                       // option value string
  >;

  using IsSIBLayout = BCRecordLayout<
    IS_SIB,
    BCFixed<1> // Is this an intermediate file?
  >;

  using IsStaticLibraryLayout = BCRecordLayout<
    IS_STATIC_LIBRARY
  >;

  using HasHermeticSealAtLinkLayout = BCRecordLayout<
    HAS_HERMETIC_SEAL_AT_LINK
  >;

  using IsTestableLayout = BCRecordLayout<
    IS_TESTABLE
  >;

  using ArePrivateImportsEnabledLayout = BCRecordLayout<
    ARE_PRIVATE_IMPORTS_ENABLED
  >;

  using IsImplicitDynamicEnabledLayout = BCRecordLayout<
    IS_IMPLICIT_DYNAMIC_ENABLED
  >;

  using ResilienceStrategyLayout = BCRecordLayout<
    RESILIENCE_STRATEGY,
    BCFixed<2>
  >;

  using IsBuiltFromInterfaceLayout = BCRecordLayout<
    IS_BUILT_FROM_INTERFACE
  >;

  using IsAllowModuleWithCompilerErrorsEnabledLayout = BCRecordLayout<
    IS_ALLOW_MODULE_WITH_COMPILER_ERRORS_ENABLED
  >;

  using ModuleABINameLayout = BCRecordLayout<
    MODULE_ABI_NAME,
    BCBlob
  >;

  using IsConcurrencyCheckedLayout = BCRecordLayout<
    IS_CONCURRENCY_CHECKED
  >;

  using ModulePackageNameLayout = BCRecordLayout<
    MODULE_PACKAGE_NAME,
    BCBlob
  >;

  using ModuleExportAsNameLayout = BCRecordLayout<
    MODULE_EXPORT_AS_NAME,
    BCBlob
  >;

  using HasCxxInteroperabilityEnabledLayout = BCRecordLayout<
    HAS_CXX_INTEROPERABILITY_ENABLED
  >;
}

/// The record types within the input block.
///
/// \sa INPUT_BLOCK_ID
namespace input_block {
  enum {
    IMPORTED_MODULE = 1,
    LINK_LIBRARY,
    IMPORTED_HEADER,
    IMPORTED_HEADER_CONTENTS,
    MODULE_FLAGS, // [unused]
    SEARCH_PATH,
    FILE_DEPENDENCY,
    DEPENDENCY_DIRECTORY,
    MODULE_INTERFACE_PATH,
    IMPORTED_MODULE_SPIS,
  };

  using ImportedModuleLayout = BCRecordLayout<
    IMPORTED_MODULE,
    ImportControlField, // import kind
    BCFixed<1>,         // scoped?
    BCFixed<1>,         // has spis?
    BCBlob // module name, with submodule path pieces separated by \0s.
           // If the 'scoped' flag is set, the final path piece is an access
           // path within the module.
  >;

  using ImportedModuleLayoutSPI = BCRecordLayout<
    IMPORTED_MODULE_SPIS,
    BCBlob // SPI names, separated by \0s
  >;

  using LinkLibraryLayout = BCRecordLayout<
    LINK_LIBRARY,
    LibraryKindField, // kind
    BCFixed<1>, // forced?
    BCBlob // library name
  >;

  using ImportedHeaderLayout = BCRecordLayout<
    IMPORTED_HEADER,
    BCFixed<1>, // exported?
    FileSizeField, // file size (for validation)
    FileHashField, // file hash (for validation)
    BCBlob // file path
  >;

  using ImportedHeaderContentsLayout = BCRecordLayout<
    IMPORTED_HEADER_CONTENTS,
    BCBlob
  >;

  using SearchPathLayout = BCRecordLayout<
    SEARCH_PATH,
    BCFixed<1>, // framework?
    BCFixed<1>, // system?
    BCBlob      // path
  >;

  using FileDependencyLayout = BCRecordLayout<
    FILE_DEPENDENCY,
    FileSizeField,                 // file size (for validation)
    FileModTimeOrContentHashField, // mtime or content hash (for validation)
    BCFixed<1>,                    // are we reading mtime (0) or hash (1)?
    BCFixed<1>,                    // SDK-relative?
    BCVBR<8>,                      // subpath-relative index (0=none)
    BCBlob                         // path
  >;

  using DependencyDirectoryLayout = BCRecordLayout<
    DEPENDENCY_DIRECTORY,
    BCBlob
  >;

  using ModuleInterfaceLayout = BCRecordLayout<
    MODULE_INTERFACE_PATH,
    BCBlob // file path
  >;

}

/// The record types within the "decls-and-types" block.
///
/// \sa DECLS_AND_TYPES_BLOCK_ID
namespace decls_block {
  enum RecordKind : uint16_t {
#define RECORD(Id) Id,
#define RECORD_VAL(Id, Value) Id = Value,
#include "DeclTypeRecordNodes.def"
  };

  namespace detail {
  enum TypeRecords : uint16_t {
#define TYPE(Id) Id##_TYPE = decls_block::RecordKind::Id##_TYPE,
#include "DeclTypeRecordNodes.def"
  };

  template <TypeRecords Record>
  class TypeRecordDispatch {};

  template <TypeRecords RecordCode, typename ...Ts>
  struct code { public: constexpr static TypeRecords value = RecordCode; };

  struct function_deserializer {
    static llvm::Expected<Type>
    deserialize(ModuleFile &MF, SmallVectorImpl<uint64_t> &scratch,
                StringRef blobData, bool isGeneric);
  };

#define TYPE_LAYOUT_IMPL(LAYOUT, ...)                                          \
  using LAYOUT = BCRecordLayout<__VA_ARGS__>;                                  \
  template <>                                                                  \
  class detail::TypeRecordDispatch<                                            \
      detail::code<detail::TypeRecords::__VA_ARGS__>::value> {                 \
    friend class swift::ModuleFile;                                            \
    static llvm::Expected<Type>                                                \
    deserialize(ModuleFile &MF,                                                \
                llvm::SmallVectorImpl<uint64_t> &scratch,                      \
                StringRef blobData);                                           \
  }
  } // namespace detail

/// This \c TYPE_LAYOUT(...) macro replaces the usual \c BCRecordLayout coding
/// structures below by enforcing structural checks for the definition of
/// deserialization members. If you forget to define a \c TYPE_LAYOUT(...) for a
/// \c TYPE(...) there will be a gnarly SFINAE error pointing at it in
/// DeclTypeRecordNodes.def.
///
/// This macro pairs with \c DESERIALIZE_TYPE(...) in Deserialization.cpp such
/// that if you forget \c DESERIALIZE_TYPE(...) you will come up
/// with a linker error.
#define TYPE_LAYOUT(LAYOUT, ...) TYPE_LAYOUT_IMPL(LAYOUT, __VA_ARGS__)

  using ClangTypeLayout = BCRecordLayout<
    CLANG_TYPE,
    BCArray<BCVBR<6>>
  >;

  /// A flag to mark a decl as being invalid
  using ErrorFlagLayout = BCRecordLayout<
    ERROR_FLAG
  >;

  /// A placeholder for invalid types
  TYPE_LAYOUT(ErrorTypeLayout,
    ERROR_TYPE,
    TypeIDField // original type (if any)
  );

  TYPE_LAYOUT(BuiltinAliasTypeLayout,
    BUILTIN_ALIAS_TYPE,
    DeclIDField, // typealias decl
    TypeIDField  // canonical type (a fallback)
  );

  TYPE_LAYOUT(TypeAliasTypeLayout,
    NAME_ALIAS_TYPE,
    DeclIDField,           // typealias decl
    TypeIDField,           // parent type
    TypeIDField,           // underlying type
    TypeIDField,           // substituted type
    SubstitutionMapIDField // substitution map
  );

  TYPE_LAYOUT(GenericTypeParamTypeLayout,
    GENERIC_TYPE_PARAM_TYPE,
    BCFixed<1>,  // parameter pack?
    DeclIDField, // generic type parameter decl or depth
    BCVBR<4> // index + 1, or zero if we have a generic type
            // parameter decl
  );

  TYPE_LAYOUT(DependentMemberTypeLayout,
    DEPENDENT_MEMBER_TYPE,
    TypeIDField, // base type
    DeclIDField  // associated type decl
  );
  TYPE_LAYOUT(NominalTypeLayout,
    NOMINAL_TYPE,
    DeclIDField, // decl
    TypeIDField  // parent
  );

  TYPE_LAYOUT(ParenTypeLayout,
    PAREN_TYPE,
    TypeIDField // inner type
  );

  TYPE_LAYOUT(TupleTypeLayout,
    TUPLE_TYPE
  );

  using TupleTypeEltLayout = BCRecordLayout<
    TUPLE_TYPE_ELT,
    IdentifierIDField,  // name
    TypeIDField         // type
  >;

  TYPE_LAYOUT(FunctionTypeLayout,
    FUNCTION_TYPE,
    TypeIDField,                     // output
    FunctionTypeRepresentationField, // representation
    ClangTypeIDField,                // type
    BCFixed<1>,                      // noescape?
    BCFixed<1>,                      // concurrent?
    BCFixed<1>,                      // async?
    BCFixed<1>,                      // throws?
    DifferentiabilityKindField,      // differentiability kind
    TypeIDField                      // global actor
    // trailed by parameters
  );

  using FunctionParamLayout = BCRecordLayout<
    FUNCTION_PARAM,
    IdentifierIDField,   // name
    IdentifierIDField,   // internal label
    TypeIDField,         // type
    BCFixed<1>,          // vararg?
    BCFixed<1>,          // autoclosure?
    BCFixed<1>,          // non-ephemeral?
    ParamDeclSpecifierField, // inout, shared or owned?
    BCFixed<1>,          // isolated
    BCFixed<1>,          // noDerivative?
    BCFixed<1>           // compileTimeConst
  >;

  TYPE_LAYOUT(MetatypeTypeLayout,
    METATYPE_TYPE,
    TypeIDField,                // instance type
    MetatypeRepresentationField // representation
  );

  TYPE_LAYOUT(ExistentialMetatypeTypeLayout,
    EXISTENTIAL_METATYPE_TYPE,
    TypeIDField,                // instance type
    MetatypeRepresentationField // representation
  );

  TYPE_LAYOUT(PrimaryArchetypeTypeLayout,
    PRIMARY_ARCHETYPE_TYPE,
    GenericSignatureIDField, // generic environment
    TypeIDField              // interface type
  );

  TYPE_LAYOUT(OpenedArchetypeTypeLayout,
    OPENED_ARCHETYPE_TYPE,
    TypeIDField,              // the interface type
    GenericEnvironmentIDField // generic environment ID
  );

  TYPE_LAYOUT(OpaqueArchetypeTypeLayout,
    OPAQUE_ARCHETYPE_TYPE,
    DeclIDField,           // the opaque type decl
    TypeIDField,           // the interface type
    SubstitutionMapIDField // the arguments
  );

  TYPE_LAYOUT(PackArchetypeTypeLayout,
    PACK_ARCHETYPE_TYPE,
    GenericSignatureIDField, // generic environment
    TypeIDField              // interface type
  );

  TYPE_LAYOUT(ElementArchetypeTypeLayout,
    ELEMENT_ARCHETYPE_TYPE,
    TypeIDField,              // the interface type
    GenericEnvironmentIDField // generic environment ID
  );

  TYPE_LAYOUT(DynamicSelfTypeLayout,
    DYNAMIC_SELF_TYPE,
    TypeIDField // self type
  );

  TYPE_LAYOUT(ProtocolCompositionTypeLayout,
    PROTOCOL_COMPOSITION_TYPE,
    BCFixed<1>,          // has AnyObject constraint
    BCArray<TypeIDField> // protocols
  );

  TYPE_LAYOUT(ParameterizedProtocolTypeLayout,
    PARAMETERIZED_PROTOCOL_TYPE,
    TypeIDField,         // base
    BCArray<TypeIDField> // arguments
  );

  TYPE_LAYOUT(BoundGenericTypeLayout,
    BOUND_GENERIC_TYPE,
    DeclIDField,         // generic decl
    TypeIDField,         // parent
    BCArray<TypeIDField> // generic arguments
  );

  TYPE_LAYOUT(GenericFunctionTypeLayout,
    GENERIC_FUNCTION_TYPE,
    TypeIDField,                     // output
    FunctionTypeRepresentationField, // representation
    BCFixed<1>,                      // concurrent?
    BCFixed<1>,                      // async?
    BCFixed<1>,                      // throws?
    DifferentiabilityKindField,      // differentiability kind
    TypeIDField,                     // global actor
    GenericSignatureIDField          // generic signature

    // trailed by parameters
  );

  TYPE_LAYOUT(SILFunctionTypeLayout,
    SIL_FUNCTION_TYPE,
    BCFixed<1>,                         // concurrent?
    BCFixed<1>,                         // async?
    SILCoroutineKindField,              // coroutine kind
    ParameterConventionField,           // callee convention
    SILFunctionTypeRepresentationField, // representation
    BCFixed<1>,                         // pseudogeneric?
    BCFixed<1>,                         // noescape?
    DifferentiabilityKindField,         // differentiability kind
    BCFixed<1>,                         // error result?
    BCVBR<6>,                           // number of parameters
    BCVBR<5>,                           // number of yields
    BCVBR<5>,                           // number of results
    GenericSignatureIDField,            // invocation generic signature
    SubstitutionMapIDField,             // invocation substitutions
    SubstitutionMapIDField,             // pattern substitutions
    ClangTypeIDField,    // clang function type, for foreign conventions
    BCArray<TypeIDField> // parameter types/conventions, alternating
                          // followed by result types/conventions, alternating
                          // followed by error result type/convention
    // Optionally a protocol conformance (for witness_methods)
    // Optionally a substitution map (for substituted function types)
  );

  TYPE_LAYOUT(SILBlockStorageTypeLayout,
    SIL_BLOCK_STORAGE_TYPE,
    TypeIDField // capture type
  );

  TYPE_LAYOUT(SILMoveOnlyWrappedTypeLayout,
    SIL_MOVE_ONLY_TYPE,
    TypeIDField            // inner type
  );

  using SILLayoutLayout = BCRecordLayout<
    SIL_LAYOUT,
    GenericSignatureIDField,    // generic signature
    BCFixed<1>,                 // captures generic env
    BCVBR<8>,                   // number of fields
    BCArray<TypeIDWithBitField> // field types with mutability
  >;

  TYPE_LAYOUT(SILBoxTypeLayout,
    SIL_BOX_TYPE,
    SILLayoutIDField,      // layout
    SubstitutionMapIDField // substitutions
  );

#define SYNTAX_SUGAR_TYPE_LAYOUT(LAYOUT, CODE)                                 \
  TYPE_LAYOUT(LAYOUT, CODE, TypeIDField)

  SYNTAX_SUGAR_TYPE_LAYOUT(ArraySliceTypeLayout, ARRAY_SLICE_TYPE);
  SYNTAX_SUGAR_TYPE_LAYOUT(OptionalTypeLayout, OPTIONAL_TYPE);
  SYNTAX_SUGAR_TYPE_LAYOUT(VariadicSequenceTypeLayout, VARIADIC_SEQUENCE_TYPE);
  SYNTAX_SUGAR_TYPE_LAYOUT(ExistentialTypeLayout, EXISTENTIAL_TYPE);

  TYPE_LAYOUT(DictionaryTypeLayout,
    DICTIONARY_TYPE,
    TypeIDField, // key type
    TypeIDField  // value type
  );

  TYPE_LAYOUT(ReferenceStorageTypeLayout,
    REFERENCE_STORAGE_TYPE,
    ReferenceOwnershipField, // ownership
    TypeIDField              // implementation type
  );

  TYPE_LAYOUT(UnboundGenericTypeLayout,
    UNBOUND_GENERIC_TYPE,
    DeclIDField, // generic decl
    TypeIDField  // parent
  );

  TYPE_LAYOUT(PackExpansionTypeLayout,
    PACK_EXPANSION_TYPE,
    TypeIDField, // pattern type
    TypeIDField  // count type
  );

  TYPE_LAYOUT(PackElementTypeLayout,
    PACK_ELEMENT_TYPE,
    TypeIDField,  // pack type
    BCFixed<32>   // level
  );

  TYPE_LAYOUT(PackTypeLayout,
    PACK_TYPE,
    BCArray<TypeIDField>  // component types
  );

  TYPE_LAYOUT(SILPackTypeLayout,
    SIL_PACK_TYPE,
    BCFixed<1>,         // is address
    BCArray<TypeIDField>// component types
  );

  using TypeAliasLayout = BCRecordLayout<
    TYPE_ALIAS_DECL,
    IdentifierIDField, // name
    DeclContextIDField,// context decl
    TypeIDField, // underlying type
    TypeIDField, // interface type (no longer used)
    BCFixed<1>,  // implicit flag
    GenericSignatureIDField, // generic environment
    AccessLevelField, // access level
    BCArray<TypeIDField> // dependency types
    // Trailed by generic parameters (if any).
  >;

  using GenericTypeParamDeclLayout = BCRecordLayout<GENERIC_TYPE_PARAM_DECL,
    IdentifierIDField, // name
    BCFixed<1>,        // implicit flag
    BCFixed<1>,        // parameter pack?
    BCVBR<4>,          // depth
    BCVBR<4>,          // index
    BCFixed<1>         // opaque type?
  >;

  using AssociatedTypeDeclLayout = BCRecordLayout<
    ASSOCIATED_TYPE_DECL,
    IdentifierIDField,   // name
    DeclContextIDField,  // context decl
    TypeIDField,         // default definition
    BCFixed<1>,          // implicit flag
    BCArray<DeclIDField> // overridden associated types
  >;

  using StructLayout = BCRecordLayout<
    STRUCT_DECL,
    IdentifierIDField,      // name
    DeclContextIDField,     // context decl
    BCFixed<1>,             // implicit flag
    BCFixed<1>,             // isObjC
    GenericSignatureIDField, // generic environment
    AccessLevelField,       // access level
    BCVBR<4>,               // number of conformances
    BCVBR<4>,               // number of inherited types
    BCArray<TypeIDField>    // inherited types, followed by dependency types
    // Trailed by the generic parameters (if any), the members record, and
    // finally conformance info (if any).
  >;

  using EnumLayout = BCRecordLayout<
    ENUM_DECL,
    IdentifierIDField,      // name
    DeclContextIDField,     // context decl
    BCFixed<1>,             // implicit flag
    BCFixed<1>,             // isObjC
    GenericSignatureIDField, // generic environment
    TypeIDField,            // raw type
    AccessLevelField,       // access level
    BCVBR<4>,               // number of conformances
    BCVBR<4>,               // number of inherited types
    BCArray<TypeIDField>    // inherited types, followed by dependency types
    // Trailed by the generic parameters (if any), the members record, and
    // finally conformance info (if any).
  >;

  using ClassLayout = BCRecordLayout<
    CLASS_DECL,
    IdentifierIDField,      // name
    DeclContextIDField,     // context decl
    BCFixed<1>,             // implicit?
    BCFixed<1>,             // explicitly objc?
    BCFixed<1>,             // Explicitly actor?
    BCFixed<1>,             // inherits convenience initializers from its superclass?
    BCFixed<1>,             // has missing designated initializers?
    GenericSignatureIDField, // generic environment
    TypeIDField,            // superclass
    AccessLevelField,       // access level
    BCVBR<4>,               // number of conformances
    BCVBR<4>,               // number of inherited types
    BCArray<TypeIDField>    // inherited types, followed by dependency types
    // Trailed by the generic parameters (if any), the members record, and
    // finally conformance info (if any).
  >;

  using ProtocolLayout = BCRecordLayout<
    PROTOCOL_DECL,
    IdentifierIDField,      // name
    DeclContextIDField,     // context decl
    BCFixed<1>,             // implicit flag
    BCFixed<1>,             // class-bounded?
    BCFixed<1>,             // objc?
    BCFixed<1>,             // existential-type-supported?
    AccessLevelField,       // access level
    BCVBR<4>,               // number of inherited types
    BCArray<TypeIDField>    // inherited types, followed by dependency types
    // Trailed by the generic parameters (if any), the members record, and
    // the default witness table record
  >;

  /// A default witness table for a protocol.
  using DefaultWitnessTableLayout = BCRecordLayout<
    DEFAULT_WITNESS_TABLE,
    BCArray<DeclIDField>
    // An array of requirement / witness pairs
  >;

  using ConstructorLayout = BCRecordLayout<
    CONSTRUCTOR_DECL,
    DeclContextIDField, // context decl
    BCFixed<1>,  // failable?
    BCFixed<1>,  // IUO result?
    BCFixed<1>,  // implicit?
    BCFixed<1>,  // objc?
    BCFixed<1>,  // stub implementation?
    BCFixed<1>,  // async?
    BCFixed<1>,  // throws?
    CtorInitializerKindField,  // initializer kind
    GenericSignatureIDField, // generic environment
    DeclIDField, // overridden decl
    BCFixed<1>,   // whether the overridden decl affects ABI
    AccessLevelField, // access level
    BCFixed<1>,   // requires a new vtable slot
    BCFixed<1>,   // 'required' but overridden is not (used for recovery)
    BCVBR<5>,     // number of parameter name components
    BCArray<IdentifierIDField> // name components,
                               // followed by TypeID dependencies
    // This record is trailed by:
    // - its generic parameters, if any
    // - its parameter patterns,
    // - the foreign error convention, if any
    // - inlinable body text, if any
  >;

  using VarLayout = BCRecordLayout<
    VAR_DECL,
    IdentifierIDField, // name
    DeclContextIDField,  // context decl
    BCFixed<1>,   // implicit?
    BCFixed<1>,   // explicitly objc?
    BCFixed<1>,   // static?
    VarDeclIntroducerField,   // introducer
    BCFixed<1>,   // is getter mutating?
    BCFixed<1>,   // is setter mutating?
    BCFixed<1>,   // is this the backing storage for a lazy property?
    BCFixed<1>,   // top level global?
    DeclIDField,  // if this is a lazy property, this is the backing storage
    OpaqueReadOwnershipField,   // opaque read ownership
    ReadImplKindField,   // read implementation
    WriteImplKindField,   // write implementation
    ReadWriteImplKindField,   // read-write implementation
    AccessorCountField, // number of accessors
    TypeIDField,  // interface type
    BCFixed<1>,   // IUO value?
    DeclIDField,  // overridden decl
    AccessLevelField, // access level
    AccessLevelField, // setter access, if applicable
    DeclIDField, // opaque return type decl
    BCFixed<2>,  // # of property wrapper backing properties
    BCVBR<4>,    // total number of vtable entries introduced by all accessors
    BCArray<TypeIDField> // accessors, backing properties, and dependencies
  >;

  using ParamLayout = BCRecordLayout<
    PARAM_DECL,
    IdentifierIDField,       // argument name
    IdentifierIDField,       // parameter name
    DeclContextIDField,      // context decl
    ParamDeclSpecifierField, // specifier
    TypeIDField,             // interface type
    BCFixed<1>,              // isIUO?
    BCFixed<1>,              // isVariadic?
    BCFixed<1>,              // isAutoClosure?
    BCFixed<1>,              // isIsolated?
    BCFixed<1>,              // isCompileTimeConst?
    DefaultArgumentField,    // default argument kind
    TypeIDField,             // default argument type
    BCBlob                   // default argument text
  >;

  using FuncLayout = BCRecordLayout<
    FUNC_DECL,
    DeclContextIDField,  // context decl
    BCFixed<1>,   // implicit?
    BCFixed<1>,   // is 'static' or 'class'?
    StaticSpellingKindField, // spelling of 'static' or 'class'
    BCFixed<1>,   // isObjC?
    SelfAccessKindField,   // self access kind
    BCFixed<1>,   // has forced static dispatch?
    BCFixed<1>,   // async?
    BCFixed<1>,   // throws?
    GenericSignatureIDField, // generic environment
    TypeIDField,  // result interface type
    BCFixed<1>,   // IUO result?
    DeclIDField,  // operator decl
    DeclIDField,  // overridden function
    BCFixed<1>,   // whether the overridden decl affects ABI
    BCVBR<5>,     // 0 for a simple name, otherwise the number of parameter name
                  // components plus one
    AccessLevelField, // access level
    BCFixed<1>,   // requires a new vtable slot
    DeclIDField,  // opaque result type decl
    BCFixed<1>,   // isUserAccessible?
    BCFixed<1>,   // is distributed thunk
    BCArray<IdentifierIDField> // name components,
                               // followed by TypeID dependencies
    // The record is trailed by:
    // - its _silgen_name, if any
    // - its generic parameters, if any
    // - body parameter patterns
    // - the foreign error convention, if any
    // - inlinable body text, if any
  >;

  using ConditionalSubstitutionConditionLayout = BCRecordLayout<
    CONDITIONAL_SUBSTITUTION_COND,
    BCFixed<1>, // is unavailable?
    BC_AVAIL_TUPLE // the OS version triple.
  >;

  using ConditionalSubstitutionLayout = BCRecordLayout<
    CONDITIONAL_SUBSTITUTION,
    SubstitutionMapIDField
    // Trailed by N conditions that include a version and
    // unavailability indicator.
  >;

  using OpaqueTypeLayout = BCRecordLayout<
    OPAQUE_TYPE_DECL,
    DeclContextIDField, // decl context
    DeclIDField, // naming decl
    GenericSignatureIDField, // interface generic signature
    TypeIDField, // interface type for opaque type
    GenericSignatureIDField, // generic environment
    SubstitutionMapIDField, // optional substitution map for underlying type
    AccessLevelField // access level
    // trailed by generic parameters
    // trailed by conditional substitutions
  >;

  // TODO: remove the unnecessary FuncDecl components here
  using AccessorLayout = BCRecordLayout<
    ACCESSOR_DECL,
    DeclContextIDField,  // context decl
    BCFixed<1>,   // implicit?
    BCFixed<1>,   // is 'static' or 'class'?
    StaticSpellingKindField, // spelling of 'static' or 'class'
    BCFixed<1>,   // isObjC?
    SelfAccessKindField,   // self access kind
    BCFixed<1>,   // has forced static dispatch?
    BCFixed<1>,   // async?
    BCFixed<1>,   // throws?
    GenericSignatureIDField, // generic environment
    TypeIDField,  // result interface type
    BCFixed<1>,   // IUO result?
    DeclIDField,  // overridden function
    BCFixed<1>,   // whether the overridden decl affects ABI
    DeclIDField,  // AccessorStorageDecl
    AccessorKindField, // accessor kind
    AccessLevelField, // access level
    BCFixed<1>,   // requires a new vtable slot
    BCFixed<1>,   // is transparent
    BCFixed<1>,   // is distributed thunk
    BCArray<IdentifierIDField> // name components,
                               // followed by TypeID dependencies
    // The record is trailed by:
    // - its _silgen_name, if any
    // - its generic parameters, if any
    // - body parameter patterns
    // - the foreign error convention, if any
    // - inlinable body text, if any
  >;

  using PatternBindingLayout = BCRecordLayout<
    PATTERN_BINDING_DECL,
    DeclContextIDField, // context decl
    BCFixed<1>,  // implicit flag
    BCFixed<1>,  // static?
    StaticSpellingKindField, // spelling of 'static' or 'class'
    BCVBR<3>,    // numpatterns
    BCArray<DeclContextIDField> // init contexts
    // The patterns trail the record.
  >;

  template <unsigned Code>
  using UnaryOperatorLayout = BCRecordLayout<
    Code, // ID field
    IdentifierIDField,  // name
    DeclContextIDField  // context decl
  >;

  using PrefixOperatorLayout = UnaryOperatorLayout<PREFIX_OPERATOR_DECL>;
  using PostfixOperatorLayout = UnaryOperatorLayout<POSTFIX_OPERATOR_DECL>;

  using InfixOperatorLayout = BCRecordLayout<
    INFIX_OPERATOR_DECL,
    IdentifierIDField, // name
    DeclContextIDField,// context decl
    DeclIDField        // precedence group
  >;

  using PrecedenceGroupLayout = BCRecordLayout<
    PRECEDENCE_GROUP_DECL,
    IdentifierIDField, // name
    DeclContextIDField,// context decl
    AssociativityField,// associativity
    BCFixed<1>,        // assignment
    BCVBR<2>,          // numHigherThan
    BCArray<DeclIDField> // higherThan, followed by lowerThan
  >;

  using EnumElementLayout = BCRecordLayout<
    ENUM_ELEMENT_DECL,
    DeclContextIDField,// context decl
    BCFixed<1>,  // implicit?
    BCFixed<1>,  // has payload?
    EnumElementRawValueKindField,  // raw value kind
    BCFixed<1>,  // implicit raw value?
    BCFixed<1>,  // negative raw value?
    IdentifierIDField, // raw value
    BCVBR<5>, // number of parameter name components
    BCArray<IdentifierIDField> // name components,

    // The record is trailed by:
    // - its argument parameters, if any
  >;

  using SubscriptLayout = BCRecordLayout<
    SUBSCRIPT_DECL,
    DeclContextIDField, // context decl
    BCFixed<1>,  // implicit?
    BCFixed<1>,  // objc?
    BCFixed<1>,   // is getter mutating?
    BCFixed<1>,   // is setter mutating?
    OpaqueReadOwnershipField,   // opaque read ownership
    ReadImplKindField,   // read implementation
    WriteImplKindField,   // write implementation
    ReadWriteImplKindField,   // read-write implementation
    AccessorCountField, // number of accessors
    GenericSignatureIDField, // generic environment
    TypeIDField, // element interface type
    BCFixed<1>,  // IUO element?
    DeclIDField, // overridden decl
    AccessLevelField, // access level
    AccessLevelField, // setter access, if applicable
    StaticSpellingKindField,    // is subscript static?
    BCVBR<5>,    // number of parameter name components
    DeclIDField, // opaque return type decl
    BCVBR<4>,    // total number of vtable entries introduced by all accessors
    BCArray<IdentifierIDField> // name components,
                               // followed by DeclID accessors,
                               // followed by TypeID dependencies
    // Trailed by:
    // - generic parameters, if any
    // - the indices pattern
  >;

  using ExtensionLayout = BCRecordLayout<
    EXTENSION_DECL,
    TypeIDField, // extended type
    DeclIDField, // extended nominal
    DeclContextIDField, // context decl
    BCFixed<1>,  // implicit flag
    GenericSignatureIDField,  // generic environment
    BCVBR<4>,    // # of protocol conformances
    BCVBR<4>,    // number of inherited types
    BCArray<TypeIDField> // inherited types, followed by TypeID dependencies
    // Trailed by the generic parameter lists, members record, and then
    // conformance info (if any).
  >;

  using DestructorLayout = BCRecordLayout<
    DESTRUCTOR_DECL,
    DeclContextIDField, // context decl
    BCFixed<1>,  // implicit?
    BCFixed<1>,  // objc?
    GenericSignatureIDField // generic environment
    // This record is trailed by its inlinable body text
  >;

  using MacroLayout = BCRecordLayout<
    MACRO_DECL,
    DeclContextIDField,  // context decl
    BCFixed<1>,   // implicit?
    GenericSignatureIDField, // generic environment
    BCFixed<1>,   // whether there is a parameter list
    TypeIDField,  // result interface type
    AccessLevelField, // access level
    BCVBR<5>,    // number of parameter name components
    BCVBR<3>,    // builtin macro definition ID
    BCFixed<1>,  // whether it has an expanded macro definition
    IdentifierIDField, // external module name, for external macros
    IdentifierIDField,  // external type name, for external macros
    BCArray<IdentifierIDField> // name components,
                               // followed by TypeID dependencies
    // The record is trailed by:
    // - its generic parameters, if any
    // - parameter list, if present
    // - expanded macro definition, if needed.
  >;

  /// The expanded macro definition text.
  using ExpandedMacroDefinitionLayout = BCRecordLayout<
    EXPANDED_MACRO_DEFINITION,
    BCFixed<1>, // whether it has replacements
    BCBlob // expansion text
    // potentially trailed by the expanded macro replacements
  >;

  /// The replacements to be performed for an expanded macro definition.
  using ExpandedMacroReplacementsLayout = BCRecordLayout<
    EXPANDED_MACRO_REPLACEMENTS,
    BCArray<BCVBR<6>> // a set of replacement triples (start offset,
                      // end offset, parameter index)
  >;

  using InlinableBodyTextLayout = BCRecordLayout<
    INLINABLE_BODY_TEXT,
    BCBlob // body text
  >;

  using ParameterListLayout = BCRecordLayout<
    PARAMETERLIST,
    BCArray<DeclIDField> // params
  >;

  using ParenPatternLayout = BCRecordLayout<
    PAREN_PATTERN
    // The sub-pattern trails the record.
  >;

  using TuplePatternLayout = BCRecordLayout<
    TUPLE_PATTERN,
    TypeIDField, // type
    BCVBR<5>     // arity
    // The elements trail the record.
  >;

  using TuplePatternEltLayout = BCRecordLayout<
    TUPLE_PATTERN_ELT,
    IdentifierIDField     // label
    // The element pattern trails the record.
  >;

  using NamedPatternLayout = BCRecordLayout<
    NAMED_PATTERN,
    DeclIDField, // associated VarDecl
    TypeIDField  // type
  >;

  using AnyPatternLayout = BCRecordLayout<
    ANY_PATTERN,
    TypeIDField, // type
    BCFixed<1>   // isAsyncLet
    // FIXME: is the type necessary?
  >;

  using TypedPatternLayout = BCRecordLayout<
    TYPED_PATTERN,
    TypeIDField  // associated type
    // The sub-pattern trails the record.
  >;

  using BindingPatternLayout = BCRecordLayout<
    VAR_PATTERN,
    BCFixed<1>  // isLet?
    // The sub-pattern trails the record.
  >;

  using GenericParamListLayout = BCRecordLayout<
    GENERIC_PARAM_LIST,
    BCArray<DeclIDField>        // the GenericTypeParamDecls
  >;

  using GenericSignatureLayout = BCRecordLayout<
    GENERIC_SIGNATURE,
    BCArray<TypeIDField>         // generic parameter types
  >;

  using GenericEnvironmentLayout = BCRecordLayout<
    GENERIC_ENVIRONMENT,
    BCFixed<1>,                  // GenericEnvironmentKind
    TypeIDField,                 // existential type or shape class
    GenericSignatureIDField,     // parent signature
    SubstitutionMapIDField       // substitution map
  >;

  using SubstitutionMapLayout = BCRecordLayout<
    SUBSTITUTION_MAP,
    GenericSignatureIDField,     // generic signature
    BCVBR<5>,                    // # of replacement types
    BCArray<TypeIDField>         // replacement types and conformances
  >;

  using SILGenericSignatureLayout = BCRecordLayout<
    SIL_GENERIC_SIGNATURE,
    BCArray<TypeIDField>         // (generic parameter name, sugared interface
                                 //  type) pairs
  >;

  using RequirementSignatureLayout = BCRecordLayout<
    REQUIREMENT_SIGNATURE,
    BCArray<BCVBR<6>>            // requirements and protocol type aliases
  >;

  using AssociatedTypeLayout = BCRecordLayout<
    ASSOCIATED_TYPE,
    DeclIDField                  // associated type decl
  >;

  using PrimaryAssociatedTypeLayout = BCRecordLayout<
    PRIMARY_ASSOCIATED_TYPE,
    DeclIDField                  // associated type decl
  >;

  /// Specifies the private discriminator string for a private declaration. This
  /// identifies the declaration's original source file in some opaque way.
  using PrivateDiscriminatorLayout = BCRecordLayout<
    PRIVATE_DISCRIMINATOR,
    IdentifierIDField  // discriminator string, as an identifier
  >;

  using LocalDiscriminatorLayout = BCRecordLayout<
    LOCAL_DISCRIMINATOR,
    BCVBR<2> // context-scoped discriminator counter
  >;

  using FilenameForPrivateLayout = BCRecordLayout<
    FILENAME_FOR_PRIVATE,
    IdentifierIDField  // the file name, as an identifier
  >;

  using DeserializationSafetyLayout = BCRecordLayout<
    DESERIALIZATION_SAFETY,
    IdentifierIDField // name to debug access to unsafe decl
  >;

  using NormalProtocolConformanceLayout = BCRecordLayout<
    NORMAL_PROTOCOL_CONFORMANCE,
    DeclIDField, // the protocol
    DeclContextIDField, // the decl that provided this conformance
    BCVBR<5>, // type mapping count
    BCVBR<5>, // value mapping count
    BCVBR<5>, // requirement signature conformance count
    BCFixed<1>, // unchecked
    BCArray<DeclIDField>
    // The array contains requirement signature conformances, then
    // type witnesses, then value witnesses.
  >;

  using SelfProtocolConformanceLayout = BCRecordLayout<
    SELF_PROTOCOL_CONFORMANCE,
    DeclIDField // the protocol
  >;

  using SpecializedProtocolConformanceLayout = BCRecordLayout<
    SPECIALIZED_PROTOCOL_CONFORMANCE,
    ProtocolConformanceIDField, // underlying conformance
    TypeIDField,           // conforming type
    SubstitutionMapIDField // substitution map
  >;

  using InheritedProtocolConformanceLayout = BCRecordLayout<
    INHERITED_PROTOCOL_CONFORMANCE,
    ProtocolConformanceIDField,  // underlying conformance
    TypeIDField                  // the conforming type
  >;

  using BuiltinProtocolConformanceLayout = BCRecordLayout<
    BUILTIN_PROTOCOL_CONFORMANCE,
    TypeIDField, // the conforming type
    DeclIDField, // the protocol
    GenericSignatureIDField, // the generic signature
    BCFixed<2>, // the builtin conformance kind
    BCArray<BCVBR<6>> // conditional requirements
  >;

  using PackConformanceLayout = BCRecordLayout<
    PACK_CONFORMANCE,
    TypeIDField,                         // pattern type
    DeclIDField,                         // the protocol
    BCArray<ProtocolConformanceIDField>  // pattern conformances
  >;

  using ProtocolConformanceXrefLayout = BCRecordLayout<
    PROTOCOL_CONFORMANCE_XREF,
    DeclIDField, // the protocol being conformed to
    DeclIDField, // the nominal type of the conformance
    ModuleIDField // the module in which the conformance can be found
  >;

  using MembersLayout = BCRecordLayout<
    MEMBERS,
    BCArray<DeclIDField>
  >;

  using XRefLayout = BCRecordLayout<
    XREF,
    ModuleIDField,  // base module ID
    BCVBR<4>        // xref path length (cannot be 0)
  >;

  using XRefTypePathPieceLayout = BCRecordLayout<
    XREF_TYPE_PATH_PIECE,
    IdentifierIDField, // name
    IdentifierIDField, // private discriminator
    BCFixed<1>,        // restrict to protocol extension
    BCFixed<1>         // imported from Clang?
  >;
  
  using XRefOpaqueReturnTypePathPieceLayout = BCRecordLayout<
    XREF_OPAQUE_RETURN_TYPE_PATH_PIECE,
    IdentifierIDField // mangled name of defining decl
  >;

  using XRefValuePathPieceLayout = BCRecordLayout<
    XREF_VALUE_PATH_PIECE,
    TypeIDField,       // type
    IdentifierIDField, // name
    BCFixed<1>,        // restrict to protocol extension
    BCFixed<1>,        // imported from Clang?
    BCFixed<1>         // static?
  >;

  using XRefInitializerPathPieceLayout = BCRecordLayout<
    XREF_INITIALIZER_PATH_PIECE,
    TypeIDField,             // type
    BCFixed<1>,              // restrict to protocol extension
    BCFixed<1>,              // imported from Clang?
    CtorInitializerKindField // initializer kind
  >;

  using XRefExtensionPathPieceLayout = BCRecordLayout<
    XREF_EXTENSION_PATH_PIECE,
    ModuleIDField,       // module ID
    GenericSignatureIDField  // for a constrained extension,
                             // the generic signature
  >;

  using XRefOperatorOrAccessorPathPieceLayout = BCRecordLayout<
    XREF_OPERATOR_OR_ACCESSOR_PATH_PIECE,
    IdentifierIDField, // name
    AccessorKindField  // accessor kind OR operator fixity
  >;
  static_assert(std::is_same<AccessorKindField, OperatorKindField>::value,
                "accessor kinds and operator kinds are not compatible");

  using XRefGenericParamPathPieceLayout = BCRecordLayout<
    XREF_GENERIC_PARAM_PATH_PIECE,
    BCVBR<5>, // depth
    BCVBR<5>  // index
  >;

  using SILGenNameDeclAttrLayout = BCRecordLayout<
    SILGenName_DECL_ATTR,
    BCFixed<1>, // implicit flag
    BCBlob      // _silgen_name
  >;

  using SectionDeclAttrLayout = BCRecordLayout<
    Section_DECL_ATTR,
    BCFixed<1>, // implicit flag
    BCBlob      // _section
  >;

  using CDeclDeclAttrLayout = BCRecordLayout<
    CDecl_DECL_ATTR,
    BCFixed<1>, // implicit flag
    BCBlob      // _silgen_name
  >;

  using SPIAccessControlDeclAttrLayout = BCRecordLayout<
    SPIAccessControl_DECL_ATTR,
    BCArray<IdentifierIDField>  // SPI names
  >;

  using AlignmentDeclAttrLayout = BCRecordLayout<
    Alignment_DECL_ATTR,
    BCFixed<1>, // implicit flag
    BCVBR<8>    // alignment
  >;
  
  using SwiftNativeObjCRuntimeBaseDeclAttrLayout = BCRecordLayout<
    SwiftNativeObjCRuntimeBase_DECL_ATTR,
    BCFixed<1>, // implicit flag
    IdentifierIDField // name
  >;

  using MainTypeDeclAttrLayout = BCRecordLayout<
    MainType_DECL_ATTR,
    BCFixed<1> // implicit flag
  >;

  using SemanticsDeclAttrLayout = BCRecordLayout<
    Semantics_DECL_ATTR,
    BCFixed<1>, // implicit flag
    BCBlob      // semantics value
  >;

  using EffectsDeclAttrLayout = BCRecordLayout<
    Effects_DECL_ATTR,
    BCFixed<3>,   // EffectKind
    DeclIDField   // Custom effect string or 0.
  >;

  using ForeignErrorConventionLayout = BCRecordLayout<
    FOREIGN_ERROR_CONVENTION,
    ForeignErrorConventionKindField,  // kind
    BCFixed<1>,                       // owned
    BCFixed<1>,                       // replaced
    BCVBR<4>,                         // error parameter index
    TypeIDField,                      // error parameter type
    TypeIDField                       // result type
  >;

  using ForeignAsyncConventionLayout = BCRecordLayout<
    FOREIGN_ASYNC_CONVENTION,
    TypeIDField, // completion handler type
    BCVBR<4>,    // completion handler parameter index
    BCVBR<4>,    // completion handler error parameter index (+1)
    BCVBR<4>,    // completion handler error flag parameter index (+1)
    BCFixed<1>   // completion handler error flag polarity
  >;

  using AbstractClosureExprLayout = BCRecordLayout<
    ABSTRACT_CLOSURE_EXPR_CONTEXT,
    TypeIDField, // type
    BCFixed<1>, // implicit
    BCVBR<4>, // discriminator
    DeclContextIDField // parent context decl
  >;

  using TopLevelCodeDeclContextLayout = BCRecordLayout<
    TOP_LEVEL_CODE_DECL_CONTEXT,
    DeclContextIDField // parent context decl
  >;

  using PatternBindingInitializerLayout = BCRecordLayout<
    PATTERN_BINDING_INITIALIZER_CONTEXT,
    DeclIDField, // parent pattern binding decl
    BCVBR<3>,    // binding index in the pattern binding decl
    BCBlob       // initializer text, if present
  >;

  using DefaultArgumentInitializerLayout = BCRecordLayout<
    DEFAULT_ARGUMENT_INITIALIZER_CONTEXT,
    DeclContextIDField, // parent context decl
    BCVBR<3> // parameter index
  >;

  // Stub layouts, unused.
  using ReferenceOwnershipDeclAttrLayout
    = BCRecordLayout<ReferenceOwnership_DECL_ATTR>;
  using RawDocCommentDeclAttrLayout = BCRecordLayout<RawDocComment_DECL_ATTR>;
  using AccessControlDeclAttrLayout = BCRecordLayout<AccessControl_DECL_ATTR>;
  using SetterAccessDeclAttrLayout = BCRecordLayout<SetterAccess_DECL_ATTR>;
  using ObjCBridgedDeclAttrLayout = BCRecordLayout<ObjCBridged_DECL_ATTR>;
  using SynthesizedProtocolDeclAttrLayout
    = BCRecordLayout<SynthesizedProtocol_DECL_ATTR>;
  using ImplementsDeclAttrLayout = BCRecordLayout<Implements_DECL_ATTR>;
  using ObjCRuntimeNameDeclAttrLayout
    = BCRecordLayout<ObjCRuntimeName_DECL_ATTR>;
  using RestatedObjCConformanceDeclAttrLayout
    = BCRecordLayout<RestatedObjCConformance_DECL_ATTR>;
  using ClangImporterSynthesizedTypeDeclAttrLayout
    = BCRecordLayout<ClangImporterSynthesizedType_DECL_ATTR>;
  using PrivateImportDeclAttrLayout = BCRecordLayout<PrivateImport_DECL_ATTR>;
  using ProjectedValuePropertyDeclAttrLayout = BCRecordLayout<
      ProjectedValueProperty_DECL_ATTR,
      BCFixed<1>,        // isImplicit
      IdentifierIDField  // name
  >;

  using InlineDeclAttrLayout = BCRecordLayout<
    Inline_DECL_ATTR,
    BCFixed<2>  // inline value
  >;

  using NonSendableDeclAttrLayout = BCRecordLayout<
    NonSendable_DECL_ATTR,
    BCFixed<1>  // non-sendable kind
  >;

  using OptimizeDeclAttrLayout = BCRecordLayout<
    Optimize_DECL_ATTR,
    BCFixed<2>  // optimize value
  >;

  using ExclusivityDeclAttrLayout = BCRecordLayout<
    Optimize_DECL_ATTR,
    BCFixed<2>  // exclusivity mode
  >;

  using AvailableDeclAttrLayout = BCRecordLayout<
    Available_DECL_ATTR,
    BCFixed<1>, // implicit flag
    BCFixed<1>, // is unconditionally unavailable?
    BCFixed<1>, // is unconditionally deprecated?
    BCFixed<1>, // is unavailable from async?
    BCFixed<1>, // is this PackageDescription version-specific kind?
    BCFixed<1>, // is SPI?
    BC_AVAIL_TUPLE, // Introduced
    BC_AVAIL_TUPLE, // Deprecated
    BC_AVAIL_TUPLE, // Obsoleted
    BCVBR<5>,    // platform
    DeclIDField, // rename declaration (if any)
    BCVBR<5>,    // number of bytes in message string
    BCVBR<5>,    // number of bytes in rename string
    BCBlob       // message, followed by rename
  >;

  using OriginallyDefinedInDeclAttrLayout = BCRecordLayout<
    OriginallyDefinedIn_DECL_ATTR,
    BCFixed<1>,     // implicit flag
    BC_AVAIL_TUPLE, // moved OS version
    BCVBR<5>,       // platform
    BCBlob          // original module name
  >;

  using ObjCDeclAttrLayout = BCRecordLayout<
    ObjC_DECL_ATTR,
    BCFixed<1>, // implicit flag
    BCFixed<1>, // Swift 3 inferred
    BCFixed<1>, // implicit name flag
    BCVBR<4>,   // # of arguments (+1) or zero if no name
    BCArray<IdentifierIDField>
  >;

  using ObjCImplementationDeclAttrLayout = BCRecordLayout<
    ObjCImplementation_DECL_ATTR,
    BCFixed<1>,                // implicit flag
    BCFixed<1>,                // category name invalid
    IdentifierIDField          // category name
  >;

  using SpecializeDeclAttrLayout = BCRecordLayout<
      Specialize_DECL_ATTR,
      BCFixed<1>,              // exported flag
      BCFixed<1>,              // specialization kind
      GenericSignatureIDField, // specialized signature
      DeclIDField,             // target function
      BCVBR<4>, // # of arguments (+1) or 1 if simple decl name, 0 if no target
      BCVBR<4>, // # of SPI groups
      BCVBR<4>, // # of availability attributes
      BCVBR<4>, // # of type erased parameters
      BCArray<IdentifierIDField> // target function pieces, spi groups, type erased params
      >;

  using InitializesDeclAttrLayout = BCRecordLayout<
      Initializes_DECL_ATTR,
      BCArray<IdentifierIDField> // initialized properties
  >;

  using AccessesDeclAttrLayout = BCRecordLayout<
      Accesses_DECL_ATTR,
      BCArray<IdentifierIDField> // initialized properties
  >;

  using StorageRestrictionsDeclAttrLayout = BCRecordLayout<
      StorageRestrictions_DECL_ATTR,
      BCVBR<16>, // num "initializes" properties
      BCArray<IdentifierIDField> // properties
  >;

  using DifferentiableDeclAttrLayout = BCRecordLayout<
    Differentiable_DECL_ATTR,
    BCFixed<1>, // Implicit flag.
    DifferentiabilityKindField, // Differentiability kind.
    GenericSignatureIDField, // Derivative generic signature.
    BCArray<BCFixed<1>> // Differentiation parameter indices' bitvector.
  >;

  using DerivativeDeclAttrLayout = BCRecordLayout<
    Derivative_DECL_ATTR,
    BCFixed<1>, // Implicit flag.
    IdentifierIDField, // Original name.
    BCFixed<1>, // Has original accessor kind?
    AccessorKindField, // Original accessor kind.
    DeclIDField, // Original function declaration.
    AutoDiffDerivativeFunctionKindField, // Derivative function kind.
    BCArray<BCFixed<1>> // Differentiation parameter indices' bitvector.
  >;

  using TransposeDeclAttrLayout = BCRecordLayout<
    Transpose_DECL_ATTR,
    BCFixed<1>, // Implicit flag.
    IdentifierIDField, // Original name.
    DeclIDField, // Original function declaration.
    BCArray<BCFixed<1>> // Transposed parameter indices' bitvector.
  >;

#define SIMPLE_DECL_ATTR(X, CLASS, ...)         \
  using CLASS##DeclAttrLayout = BCRecordLayout< \
    CLASS##_DECL_ATTR, \
    BCFixed<1> /* implicit flag */ \
  >;
#include "swift/AST/Attr.def"

  using DynamicReplacementDeclAttrLayout = BCRecordLayout<
    DynamicReplacement_DECL_ATTR,
    BCFixed<1>, // implicit flag
    DeclIDField, // replaced function
    BCVBR<4>,   // # of arguments (+1) or zero if no name
    BCArray<IdentifierIDField>
  >;

  using TypeEraserDeclAttrLayout = BCRecordLayout<
    TypeEraser_DECL_ATTR,
    BCFixed<1>, // implicit flag
    TypeIDField // type eraser type
  >;

  using CustomDeclAttrLayout = BCRecordLayout<
    Custom_DECL_ATTR,
    BCFixed<1>,  // implicit flag
    TypeIDField, // type referenced by this custom attribute
    BCFixed<1>   // is the argument (unsafe)
  >;

  using UnavailableFromAsyncDeclAttrLayout = BCRecordLayout<
    UnavailableFromAsync_DECL_ATTR,
    BCFixed<1>, // Implicit flag
    BCBlob      // Message
  >;

  using BackDeployedDeclAttrLayout = BCRecordLayout<
    BackDeployed_DECL_ATTR,
    BCFixed<1>,     // implicit flag
    BC_AVAIL_TUPLE, // OS version
    BCVBR<5>        // platform
  >;

  using ExposeDeclAttrLayout = BCRecordLayout<Expose_DECL_ATTR,
                                              BCFixed<1>, // implicit flag
                                              BCBlob      // declaration name
                                              >;

  using DocumentationDeclAttrLayout = BCRecordLayout<
    Documentation_DECL_ATTR,
    BCFixed<1>,         // implicit flag
    IdentifierIDField,  // metadata text
    BCFixed<1>,         // has visibility
    AccessLevelField    // visibility
  >;

  using MacroRoleDeclAttrLayout = BCRecordLayout<
    MacroRole_DECL_ATTR,
    BCFixed<1>,                // implicit flag
    BCFixed<1>,                // macro syntax
    MacroRoleField,            // macro role
    BCVBR<5>,                  // number of names
    BCVBR<5>,                  // number of conformances
    BCArray<IdentifierIDField> // introduced names, where each is encoded as
                               //   - introduced kind
                               //   - base name
                               //   - # of argument labels + 1 (or 0 if none)
                               //   - argument labels
                               // trialed by introduced conformances
  >;

#undef SYNTAX_SUGAR_TYPE_LAYOUT
#undef TYPE_LAYOUT
#undef TYPE_LAYOUT_IMPL
}

/// Returns the encoding kind for the given decl.
///
/// Note that this does not work for all encodable decls, only those designed
/// to be stored in a hash table.
static inline decls_block::RecordKind getKindForTable(const Decl *D) {
  using namespace decls_block;

  switch (D->getKind()) {
  case DeclKind::TypeAlias:
    return decls_block::TYPE_ALIAS_DECL;
  case DeclKind::Enum:
    return decls_block::ENUM_DECL;
  case DeclKind::Struct:
    return decls_block::STRUCT_DECL;
  case DeclKind::Class:
    return decls_block::CLASS_DECL;
  case DeclKind::Protocol:
    return decls_block::PROTOCOL_DECL;

  case DeclKind::Func:
    return decls_block::FUNC_DECL;
  case DeclKind::Var:
    return decls_block::VAR_DECL;
  case DeclKind::Param:
    return decls_block::PARAM_DECL;

  case DeclKind::Subscript:
    return decls_block::SUBSCRIPT_DECL;
  case DeclKind::Constructor:
    return decls_block::CONSTRUCTOR_DECL;
  case DeclKind::Destructor:
    return decls_block::DESTRUCTOR_DECL;
  case DeclKind::Macro:
    return decls_block::MACRO_DECL;

  default:
    llvm_unreachable("cannot store this kind of decl in a hash table");
  }
}

/// The record types within the identifier block.
///
/// \sa IDENTIFIER_BLOCK_ID
namespace identifier_block {
  enum {
    IDENTIFIER_DATA = 1
  };

  using IdentifierDataLayout = BCRecordLayout<IDENTIFIER_DATA, BCBlob>;
}

/// The record types within the index block.
///
/// \sa INDEX_BLOCK_ID
namespace index_block {
  enum RecordKind {
    TYPE_OFFSETS = 1,
    DECL_OFFSETS,
    IDENTIFIER_OFFSETS,
    TOP_LEVEL_DECLS,
    OPERATORS,
    EXTENSIONS,
    CLASS_MEMBERS_FOR_DYNAMIC_LOOKUP,
    OPERATOR_METHODS,

    /// The Objective-C method index, which contains a mapping from
    /// Objective-C selectors to the methods/initializers/properties/etc. that
    /// produce Objective-C methods.
    OBJC_METHODS,

    /// The derivative function configuration table, which maps original
    /// function declaration names to derivative function configurations.
    DERIVATIVE_FUNCTION_CONFIGURATIONS,

    ENTRY_POINT,
    LOCAL_DECL_CONTEXT_OFFSETS,
    LOCAL_TYPE_DECLS,
    OPAQUE_RETURN_TYPE_DECLS,
    GENERIC_SIGNATURE_OFFSETS,
    GENERIC_ENVIRONMENT_OFFSETS,
    PROTOCOL_CONFORMANCE_OFFSETS,
    PACK_CONFORMANCE_OFFSETS,
    SIL_LAYOUT_OFFSETS,

    PRECEDENCE_GROUPS,
    NESTED_TYPE_DECLS,
    DECL_MEMBER_NAMES,
    DECL_FINGERPRINTS,

    ORDERED_TOP_LEVEL_DECLS,

    SUBSTITUTION_MAP_OFFSETS,
    CLANG_TYPE_OFFSETS,
    EXPORTED_PRESPECIALIZATION_DECLS,
    LastRecordKind = EXPORTED_PRESPECIALIZATION_DECLS,
  };

  constexpr const unsigned RecordIDFieldWidth = 5;
  static_assert(LastRecordKind < (1 << RecordIDFieldWidth),
                "not enough bits for all record kinds");
  using RecordIDField = BCFixed<RecordIDFieldWidth>;

  using OffsetsLayout = BCGenericRecordLayout<
    RecordIDField, // record ID
    BCArray<BitOffsetField>
  >;

  using DeclListLayout = BCGenericRecordLayout<
    RecordIDField, // record ID
    BCVBR<16>,  // table offset within the blob (see below)
    BCBlob  // map from identifier strings to decl kinds / decl IDs
  >;

  using GroupNamesLayout = BCGenericRecordLayout<
    RecordIDField, // record ID
    BCBlob       // actual names
  >;

  using ExtensionTableLayout = BCRecordLayout<
    EXTENSIONS, // record ID
    BCVBR<16>,  // table offset within the blob (see below)
    BCBlob  // map from identifier strings to decl kinds / decl IDs
  >;

  using ObjCMethodTableLayout = BCRecordLayout<
    OBJC_METHODS,  // record ID
    BCVBR<16>,     // table offset within the blob (see below)
    BCBlob         // map from Objective-C selectors to methods with that selector
  >;

  using NestedTypeDeclsLayout = BCRecordLayout<
    NESTED_TYPE_DECLS, // record ID
    BCVBR<16>,  // table offset within the blob (see below)
    BCBlob  // map from identifier strings to decl kinds / decl IDs
  >;

  using DeclMemberNamesLayout = BCRecordLayout<
    DECL_MEMBER_NAMES, // record ID
    BCVBR<16>,  // table offset within the blob (see below)
    BCBlob  // map from member DeclBaseNames to offsets of DECL_MEMBERS records
  >;

  using DerivativeFunctionConfigTableLayout = BCRecordLayout<
    DERIVATIVE_FUNCTION_CONFIGURATIONS,  // record ID
    BCVBR<16>,     // table offset within the blob (see below)
    BCBlob         // map from original declaration names to derivative configs
  >;

  using EntryPointLayout = BCRecordLayout<
    ENTRY_POINT,
    DeclIDField  // the ID of the main class; 0 if there was a main source file
  >;

  using OrderedDeclsLayout = BCGenericRecordLayout<
    RecordIDField,        // record ID
    BCArray<DeclIDField>  // list of decls by ID
  >;

  using DeclFingerprintsLayout = BCRecordLayout<
    DECL_FINGERPRINTS, // record ID
    BCVBR<16>,   // table offset within the blob (see below)
    BCBlob       // map from member DeclIDs to strings
  >;
}

/// \sa DECL_MEMBER_TABLES_BLOCK_ID
namespace decl_member_tables_block {
  enum RecordKind {
    DECL_MEMBERS = 1,
  };

  using DeclMembersLayout = BCRecordLayout<
    DECL_MEMBERS, // record ID
    BCVBR<16>,  // table offset within the blob (see below)
    BCBlob  // maps from DeclIDs to DeclID vectors
  >;
}

} // end namespace serialization
} // end namespace swift

#endif
