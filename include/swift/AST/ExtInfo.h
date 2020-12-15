//===--- ExtInfo.h - Extended information for function types ----*- C++ -*-===//
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
// Defines the ASTExtInfo and SILExtInfo classes, which are used to store
// the calling convention and related information for function types in the AST
// and SIL respectively. These types are lightweight and immutable; they are
// constructed using builder-pattern style APIs to enforce invariants.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_EXTINFO_H
#define SWIFT_EXTINFO_H

#include "swift/AST/AutoDiff.h"
#include "swift/AST/ClangModuleLoader.h"

#include "llvm/ADT/Optional.h"
#include "llvm/Support/raw_ostream.h"

#include <utility>

namespace clang {
class Type;
class ASTContext;
} // namespace clang

namespace swift {
class AnyFunctionType;
class ASTExtInfo;
class ASTExtInfoBuilder;
class FunctionType;
class SILExtInfo;
class SILExtInfoBuilder;
class SILFunctionType;
enum class SILFunctionTypeRepresentation : uint8_t;
} // namespace swift

namespace swift {

// MARK: - ClangTypeInfo
/// Wrapper class for storing a clang::Type in an (AST|SIL)ExtInfo.
class ClangTypeInfo {
  friend AnyFunctionType;
  friend FunctionType;
  friend SILFunctionType;
  friend ASTExtInfoBuilder;
  friend SILExtInfoBuilder;

  // [NOTE: ClangTypeInfo-contents]
  // We preserve a full clang::Type *, not a clang::FunctionType * as:
  // 1. We need to keep sugar in case we need to present an error to the user
  //    (for AnyFunctionType).
  // 2. The actual type being stored is [ignoring sugar] either a
  //    clang::PointerType, a clang::BlockPointerType, or a
  //    clang::ReferenceType which points to a clang::FunctionType.
  //
  // When used as a part of SILFunctionType, the type is canonical.
  const clang::Type *type;

  constexpr ClangTypeInfo() : type(nullptr) {}
  constexpr ClangTypeInfo(const clang::Type *type) : type(type) {}

  friend bool operator==(ClangTypeInfo lhs, ClangTypeInfo rhs);
  friend bool operator!=(ClangTypeInfo lhs, ClangTypeInfo rhs);
  ClangTypeInfo getCanonical() const;

public:
  constexpr const clang::Type *getType() const { return type; }

  constexpr bool empty() const { return !type; }

  /// Use the ClangModuleLoader to print the Clang type as a string.
  void printType(ClangModuleLoader *cml, llvm::raw_ostream &os) const;

  void dump(llvm::raw_ostream &os, const clang::ASTContext &ctx) const;
};

// MARK: - UnexpectedClangTypeError
/// Potential errors when trying to store a Clang type in an ExtInfo.
struct UnexpectedClangTypeError {
  enum class Kind {
    NullForCOrBlock,
    NonnullForNonCOrBlock,
    NotBlockPointer,
    NotFunctionPointerOrReference,
    NonCanonical,
  };
  const Kind errorKind;
  const clang::Type *type;

  static Optional<UnexpectedClangTypeError> checkClangType(
    SILFunctionTypeRepresentation fnRep, const clang::Type *type,
    bool expectNonnullForCOrBlock, bool expectCanonical);

  void dump();
};

// MARK: - FunctionTypeRepresentation
/// The representation form of a function.
enum class FunctionTypeRepresentation : uint8_t {
  /// A "thick" function that carries a context pointer to reference captured
  /// state. The default native function representation.
  Swift = 0,

  /// A thick function that is represented as an Objective-C block.
  Block,

  /// A "thin" function that needs no context.
  Thin,

  /// A C function pointer (or reference), which is thin and also uses the C
  /// calling convention.
  CFunctionPointer,

  /// The value of the greatest AST function representation.
  Last = CFunctionPointer,
};

// MARK: - SILFunctionTypeRepresentation

/// The representation form of a SIL function.
///
/// This is a superset of FunctionTypeRepresentation. The common representations
/// must share an enum value.
///
/// TODO: The overlap of SILFunctionTypeRepresentation and
/// FunctionTypeRepresentation is a total hack necessitated by the way SIL
/// TypeLowering is currently written. We ought to refactor TypeLowering so that
/// it is not necessary to distinguish these cases.
enum class SILFunctionTypeRepresentation : uint8_t {
  /// A freestanding thick function.
  Thick = uint8_t(FunctionTypeRepresentation::Swift),

  /// A thick function that is represented as an Objective-C block.
  Block = uint8_t(FunctionTypeRepresentation::Block),

  /// A freestanding thin function that needs no context.
  Thin = uint8_t(FunctionTypeRepresentation::Thin),

  /// A C function pointer, which is thin and also uses the C calling
  /// convention.
  CFunctionPointer = uint8_t(FunctionTypeRepresentation::CFunctionPointer),

  /// The value of the greatest AST function representation.
  LastAST = CFunctionPointer,

  /// The value of the least SIL-only function representation.
  FirstSIL = 8,

  /// A Swift instance method.
  Method = FirstSIL,

  /// An Objective-C method.
  ObjCMethod,

  /// A Swift protocol witness.
  WitnessMethod,

  /// A closure invocation function that has not been bound to a context.
  Closure,

  /// A C++ method that takes a "this" argument (not a static C++ method or
  /// constructor). Except for
  /// handling the "this" argument, has the same behavior as "CFunctionPointer".
  CXXMethod,
};

constexpr SILFunctionTypeRepresentation
convertRepresentation(FunctionTypeRepresentation rep) {
  switch (rep) {
  case FunctionTypeRepresentation::Swift:
    return SILFunctionTypeRepresentation::Thick;
  case FunctionTypeRepresentation::Block:
    return SILFunctionTypeRepresentation::Block;
  case FunctionTypeRepresentation::Thin:
    return SILFunctionTypeRepresentation::Thin;
  case FunctionTypeRepresentation::CFunctionPointer:
    return SILFunctionTypeRepresentation::CFunctionPointer;
  }
  llvm_unreachable("Unhandled FunctionTypeRepresentation!");
};

inline Optional<FunctionTypeRepresentation>
convertRepresentation(SILFunctionTypeRepresentation rep) {
  switch (rep) {
  case SILFunctionTypeRepresentation::Thick:
    return {FunctionTypeRepresentation::Swift};
  case SILFunctionTypeRepresentation::Block:
    return {FunctionTypeRepresentation::Block};
  case SILFunctionTypeRepresentation::Thin:
    return {FunctionTypeRepresentation::Thin};
  case SILFunctionTypeRepresentation::CXXMethod:
  case SILFunctionTypeRepresentation::CFunctionPointer:
    return {FunctionTypeRepresentation::CFunctionPointer};
  case SILFunctionTypeRepresentation::Method:
  case SILFunctionTypeRepresentation::ObjCMethod:
  case SILFunctionTypeRepresentation::WitnessMethod:
  case SILFunctionTypeRepresentation::Closure:
    return None;
  }
  llvm_unreachable("Unhandled SILFunctionTypeRepresentation!");
};

/// Can this calling convention result in a function being called indirectly
/// through the runtime.
constexpr bool canBeCalledIndirectly(SILFunctionTypeRepresentation rep) {
  switch (rep) {
  case SILFunctionTypeRepresentation::Thick:
  case SILFunctionTypeRepresentation::Thin:
  case SILFunctionTypeRepresentation::CFunctionPointer:
  case SILFunctionTypeRepresentation::Block:
  case SILFunctionTypeRepresentation::Closure:
  case SILFunctionTypeRepresentation::CXXMethod:
    return false;
  case SILFunctionTypeRepresentation::ObjCMethod:
  case SILFunctionTypeRepresentation::Method:
  case SILFunctionTypeRepresentation::WitnessMethod:
    return true;
  }

  llvm_unreachable("Unhandled SILFunctionTypeRepresentation in switch.");
}

template <typename Repr> constexpr bool shouldStoreClangType(Repr repr) {
  static_assert(std::is_same<Repr, FunctionTypeRepresentation>::value ||
                    std::is_same<Repr, SILFunctionTypeRepresentation>::value,
                "Expected a Representation type as the argument type.");
  switch (static_cast<SILFunctionTypeRepresentation>(repr)) {
  case SILFunctionTypeRepresentation::CFunctionPointer:
  case SILFunctionTypeRepresentation::Block:
  case SILFunctionTypeRepresentation::CXXMethod:
    return true;
  case SILFunctionTypeRepresentation::ObjCMethod:
  case SILFunctionTypeRepresentation::Thick:
  case SILFunctionTypeRepresentation::Thin:
  case SILFunctionTypeRepresentation::Method:
  case SILFunctionTypeRepresentation::WitnessMethod:
  case SILFunctionTypeRepresentation::Closure:
    return false;
  }
  llvm_unreachable("Unhandled SILFunctionTypeRepresentation.");
}

// MARK: - ASTExtInfoBuilder
/// A builder type for creating an \c ASTExtInfo.
///
/// The main API public includes the \c withXYZ and \p build() methods.
class ASTExtInfoBuilder {
  friend AnyFunctionType;
  friend ASTExtInfo;

  // If bits are added or removed, then TypeBase::AnyFunctionTypeBits
  // and NumMaskBits must be updated, and they must match.
  //
  //   |representation|noEscape|async|throws|differentiability|
  //   |    0 .. 3    |    4   |  5  |   6  |      7 .. 8     |
  //
  enum : unsigned {
    RepresentationMask = 0xF << 0,
    NoEscapeMask = 1 << 4,
    AsyncMask = 1 << 5,
    ThrowsMask = 1 << 6,
    DifferentiabilityMaskOffset = 7,
    DifferentiabilityMask = 0x3 << DifferentiabilityMaskOffset,
    NumMaskBits = 9
  };

  unsigned bits; // Naturally sized for speed.

  ClangTypeInfo clangTypeInfo;

  using Representation = FunctionTypeRepresentation;

  ASTExtInfoBuilder(unsigned bits, ClangTypeInfo clangTypeInfo)
      : bits(bits), clangTypeInfo(clangTypeInfo) {}

public:
  /// An ExtInfoBuilder for a typical Swift function: @convention(swift),
  /// @escaping, non-throwing, non-differentiable.
  ASTExtInfoBuilder()
      : ASTExtInfoBuilder(Representation::Swift, false, false,
                          DifferentiabilityKind::NonDifferentiable, nullptr) {}

  // Constructor for polymorphic type.
  ASTExtInfoBuilder(Representation rep, bool throws)
      : ASTExtInfoBuilder(rep, false, throws,
                          DifferentiabilityKind::NonDifferentiable, nullptr) {}

  // Constructor with no defaults.
  ASTExtInfoBuilder(Representation rep, bool isNoEscape, bool throws,
                    DifferentiabilityKind diffKind, const clang::Type *type)
      : ASTExtInfoBuilder(
            ((unsigned)rep) | (isNoEscape ? NoEscapeMask : 0) |
                (throws ? ThrowsMask : 0) |
                (((unsigned)diffKind << DifferentiabilityMaskOffset) &
                 DifferentiabilityMask),
            ClangTypeInfo(type)) {}

  void checkInvariants() const;

  /// Check if \c this is well-formed and create an ExtInfo.
  ASTExtInfo build() const;

  constexpr Representation getRepresentation() const {
    unsigned rawRep = bits & RepresentationMask;
    assert(rawRep <= unsigned(Representation::Last) &&
           "unexpected SIL representation");
    return Representation(rawRep);
  }

  constexpr bool isNoEscape() const { return bits & NoEscapeMask; }

  constexpr bool isAsync() const { return bits & AsyncMask; }

  constexpr bool isThrowing() const { return bits & ThrowsMask; }

  constexpr DifferentiabilityKind getDifferentiabilityKind() const {
    return DifferentiabilityKind((bits & DifferentiabilityMask) >>
                                 DifferentiabilityMaskOffset);
  }

  constexpr bool isDifferentiable() const {
    return getDifferentiabilityKind() >
           DifferentiabilityKind::NonDifferentiable;
  }

  ClangTypeInfo getClangTypeInfo() const { return clangTypeInfo; }

  constexpr SILFunctionTypeRepresentation getSILRepresentation() const {
    unsigned rawRep = bits & RepresentationMask;
    return SILFunctionTypeRepresentation(rawRep);
  }

  constexpr bool hasSelfParam() const {
    switch (getSILRepresentation()) {
    case SILFunctionTypeRepresentation::Thick:
    case SILFunctionTypeRepresentation::Block:
    case SILFunctionTypeRepresentation::Thin:
    case SILFunctionTypeRepresentation::CFunctionPointer:
    case SILFunctionTypeRepresentation::Closure:
      return false;
    case SILFunctionTypeRepresentation::ObjCMethod:
    case SILFunctionTypeRepresentation::Method:
    case SILFunctionTypeRepresentation::WitnessMethod:
    case SILFunctionTypeRepresentation::CXXMethod:
      return true;
    }
    llvm_unreachable("Unhandled SILFunctionTypeRepresentation in switch.");
  }

  /// True if the function representation carries context.
  constexpr bool hasContext() const {
    switch (getSILRepresentation()) {
    case SILFunctionTypeRepresentation::Thick:
    case SILFunctionTypeRepresentation::Block:
      return true;
    case SILFunctionTypeRepresentation::Thin:
    case SILFunctionTypeRepresentation::Method:
    case SILFunctionTypeRepresentation::ObjCMethod:
    case SILFunctionTypeRepresentation::WitnessMethod:
    case SILFunctionTypeRepresentation::CFunctionPointer:
    case SILFunctionTypeRepresentation::Closure:
    case SILFunctionTypeRepresentation::CXXMethod:
      return false;
    }
    llvm_unreachable("Unhandled SILFunctionTypeRepresentation in switch.");
  }

  // Note that we don't have setters. That is by design, use
  // the following with methods instead of mutating these objects.
  LLVM_NODISCARD
  ASTExtInfoBuilder withRepresentation(Representation rep) const {
    return ASTExtInfoBuilder((bits & ~RepresentationMask) | (unsigned)rep,
                             shouldStoreClangType(rep) ? clangTypeInfo
                                                       : ClangTypeInfo());
  }
  LLVM_NODISCARD
  ASTExtInfoBuilder withNoEscape(bool noEscape = true) const {
    return ASTExtInfoBuilder(noEscape ? (bits | NoEscapeMask)
                                      : (bits & ~NoEscapeMask),
                             clangTypeInfo);
  }
  LLVM_NODISCARD
  ASTExtInfoBuilder withAsync(bool async = true) const {
    return ASTExtInfoBuilder(async ? (bits | AsyncMask)
                                   : (bits & ~AsyncMask),
                             clangTypeInfo);
  }
  LLVM_NODISCARD
  ASTExtInfoBuilder withThrows(bool throws = true) const {
    return ASTExtInfoBuilder(
        throws ? (bits | ThrowsMask) : (bits & ~ThrowsMask), clangTypeInfo);
  }
  LLVM_NODISCARD
  ASTExtInfoBuilder
  withDifferentiabilityKind(DifferentiabilityKind differentiability) const {
    return ASTExtInfoBuilder(
        (bits & ~DifferentiabilityMask) |
            ((unsigned)differentiability << DifferentiabilityMaskOffset),
        clangTypeInfo);
  }
  LLVM_NODISCARD
  ASTExtInfoBuilder withClangFunctionType(const clang::Type *type) const {
    return ASTExtInfoBuilder(bits, ClangTypeInfo(type));
  }

  /// Put a SIL representation in the ExtInfo.
  ///
  /// SIL type lowering transiently generates AST function types with SIL
  /// representations. However, they shouldn't persist in the AST, and
  /// don't need to be parsed, printed, or serialized.
  LLVM_NODISCARD
  ASTExtInfoBuilder
  withSILRepresentation(SILFunctionTypeRepresentation rep) const {
    return ASTExtInfoBuilder((bits & ~RepresentationMask) | (unsigned)rep,
                             shouldStoreClangType(rep) ? clangTypeInfo
                                                       : ClangTypeInfo());
  }

  bool isEqualTo(ASTExtInfoBuilder other, bool useClangTypes) const {
    return bits == other.bits &&
      (useClangTypes ? (clangTypeInfo == other.clangTypeInfo) : true);
  }

  constexpr std::pair<unsigned, const void *> getFuncAttrKey() const {
    return std::make_pair(bits, clangTypeInfo.getType());
  }
}; // end ASTExtInfoBuilder

// MARK: - ASTExtInfo
/// Calling convention and related information for AnyFunctionType + subclasses.
///
/// New instances can be made from existing instances via \c ASTExtInfoBuilder,
/// typically using a code pattern like:
/// \code
/// extInfo.intoBuilder().withX(x).withY(y).build()
/// \endcode
class ASTExtInfo {
  friend ASTExtInfoBuilder;
  friend AnyFunctionType;

  ASTExtInfoBuilder builder;

  // Only for use by ASTExtInfoBuilder::build. Don't use it elsewhere!
  ASTExtInfo(ASTExtInfoBuilder builder) : builder(builder) {}

  ASTExtInfo(unsigned bits, ClangTypeInfo clangTypeInfo)
      : builder(bits, clangTypeInfo) {
    builder.checkInvariants();
  };

public:
  /// An ExtInfo for a typical Swift function: @convention(swift), @escaping,
  /// non-throwing, non-differentiable.
  ASTExtInfo() : builder() { builder.checkInvariants(); };

  /// Create a builder with the same state as \c this.
  ASTExtInfoBuilder intoBuilder() const { return builder; }

private:
  constexpr unsigned getBits() const { return builder.bits; }

public:
  constexpr FunctionTypeRepresentation getRepresentation() const {
    return builder.getRepresentation();
  }

  constexpr SILFunctionTypeRepresentation getSILRepresentation() const {
    return builder.getSILRepresentation();
  }

  constexpr bool isNoEscape() const { return builder.isNoEscape(); }

  constexpr bool isAsync() const { return builder.isAsync(); }

  constexpr bool isThrowing() const { return builder.isThrowing(); }

  constexpr DifferentiabilityKind getDifferentiabilityKind() const {
    return builder.getDifferentiabilityKind();
  }

  constexpr bool isDifferentiable() const { return builder.isDifferentiable(); }

  ClangTypeInfo getClangTypeInfo() const { return builder.getClangTypeInfo(); }

  constexpr bool hasSelfParam() const { return builder.hasSelfParam(); }

  constexpr bool hasContext() const { return builder.hasContext(); }

  /// Helper method for changing the representation.
  ///
  /// Prefer using \c ASTExtInfoBuilder::withRepresentation for chaining.
  LLVM_NODISCARD
  ASTExtInfo withRepresentation(ASTExtInfoBuilder::Representation rep) const {
    return builder.withRepresentation(rep).build();
  }

  /// Helper method for changing only the noEscape field.
  ///
  /// Prefer using \c ASTExtInfoBuilder::withNoEscape for chaining.
  LLVM_NODISCARD
  ASTExtInfo withNoEscape(bool noEscape = true) const {
    return builder.withNoEscape(noEscape).build();
  }

  /// Helper method for changing only the throws field.
  ///
  /// Prefer using \c ASTExtInfoBuilder::withThrows for chaining.
  LLVM_NODISCARD
  ASTExtInfo withThrows(bool throws = true) const {
    return builder.withThrows(throws).build();
  }

  /// Helper method for changing only the async field.
  ///
  /// Prefer using \c ASTExtInfoBuilder::withAsync for chaining.
  LLVM_NODISCARD
  ASTExtInfo withAsync(bool async = true) const {
    return builder.withAsync(async).build();
  }

  bool isEqualTo(ASTExtInfo other, bool useClangTypes) const {
    return builder.isEqualTo(other.builder, useClangTypes);
  }

  constexpr std::pair<unsigned, const void *> getFuncAttrKey() const {
    return builder.getFuncAttrKey();
  }
}; // end ASTExtInfo

// MARK: - SILFunctionLanguage

/// A language-level calling convention.
enum class SILFunctionLanguage : uint8_t {
  /// A variation of the Swift calling convention.
  Swift = 0,

  /// A variation of the C calling convention.
  C,
};

/// Map a SIL function representation to the base language calling convention
/// it uses.
constexpr
SILFunctionLanguage getSILFunctionLanguage(SILFunctionTypeRepresentation rep) {
  switch (rep) {
  case SILFunctionTypeRepresentation::ObjCMethod:
  case SILFunctionTypeRepresentation::CFunctionPointer:
  case SILFunctionTypeRepresentation::Block:
  case SILFunctionTypeRepresentation::CXXMethod:
    return SILFunctionLanguage::C;
  case SILFunctionTypeRepresentation::Thick:
  case SILFunctionTypeRepresentation::Thin:
  case SILFunctionTypeRepresentation::Method:
  case SILFunctionTypeRepresentation::WitnessMethod:
  case SILFunctionTypeRepresentation::Closure:
    return SILFunctionLanguage::Swift;
  }

  llvm_unreachable("Unhandled SILFunctionTypeRepresentation in switch.");
}

// MARK: - SILExtInfoBuilder
/// A builder type for creating an \c SILExtInfo.
///
/// The main API public includes the \c withXYZ and \p build() methods.
class SILExtInfoBuilder {
  friend SILExtInfo;
  friend SILFunctionType;

  // If bits are added or removed, then TypeBase::SILFunctionTypeBits
  // and NumMaskBits must be updated, and they must match.

  //   |representation|pseudogeneric| noescape | async | differentiability|
  //   |    0 .. 3    |      4      |     5    |   6   |      7 .. 8     |
  //
  enum : unsigned {
    RepresentationMask = 0xF << 0,
    PseudogenericMask = 1 << 4,
    NoEscapeMask = 1 << 5,
    AsyncMask = 1 << 6,
    DifferentiabilityMaskOffset = 7,
    DifferentiabilityMask = 0x3 << DifferentiabilityMaskOffset,
    NumMaskBits = 9
  };

  unsigned bits; // Naturally sized for speed.

  ClangTypeInfo clangTypeInfo;

  using Language = SILFunctionLanguage;
  using Representation = SILFunctionTypeRepresentation;

  SILExtInfoBuilder(unsigned bits, ClangTypeInfo clangTypeInfo)
      : bits(bits), clangTypeInfo(clangTypeInfo.getCanonical()) {}

  static constexpr unsigned makeBits(Representation rep, bool isPseudogeneric,
                                     bool isNoEscape, bool isAsync,
                                     DifferentiabilityKind diffKind) {
    return ((unsigned)rep) | (isPseudogeneric ? PseudogenericMask : 0) |
           (isNoEscape ? NoEscapeMask : 0) | (isAsync ? AsyncMask : 0) |
           (((unsigned)diffKind << DifferentiabilityMaskOffset) &
            DifferentiabilityMask);
  }

public:
  /// An ExtInfoBuilder for a typical Swift function: thick, @escaping,
  /// non-pseudogeneric, non-differentiable.
  SILExtInfoBuilder()
      : SILExtInfoBuilder(makeBits(SILFunctionTypeRepresentation::Thick, false,
                                   false, false,
                                   DifferentiabilityKind::NonDifferentiable),
                          ClangTypeInfo(nullptr)) {}

  SILExtInfoBuilder(Representation rep, bool isPseudogeneric, bool isNoEscape,
                    bool isAsync, DifferentiabilityKind diffKind,
                    const clang::Type *type)
      : SILExtInfoBuilder(makeBits(rep, isPseudogeneric, isNoEscape, isAsync,
                                   diffKind),
                          ClangTypeInfo(type)) {}

  // Constructor for polymorphic type.
  SILExtInfoBuilder(ASTExtInfoBuilder info, bool isPseudogeneric)
      : SILExtInfoBuilder(makeBits(info.getSILRepresentation(), isPseudogeneric,
                                   info.isNoEscape(), info.isAsync(),
                                   info.getDifferentiabilityKind()),
                          info.getClangTypeInfo()) {}

  void checkInvariants() const;

  /// Check if \c this is well-formed and create an ExtInfo.
  SILExtInfo build() const;

  /// What is the abstract representation of this function value?
  constexpr Representation getRepresentation() const {
    return Representation(bits & RepresentationMask);
  }

  constexpr Language getLanguage() const {
    return getSILFunctionLanguage(getRepresentation());
  }

  /// Is this function pseudo-generic?  A pseudo-generic function
  /// is not permitted to dynamically depend on its type arguments.
  constexpr bool isPseudogeneric() const { return bits & PseudogenericMask; }

  // Is this function guaranteed to be no-escape by the type system?
  constexpr bool isNoEscape() const { return bits & NoEscapeMask; }

  constexpr bool isAsync() const { return bits & AsyncMask; }

  constexpr DifferentiabilityKind getDifferentiabilityKind() const {
    return DifferentiabilityKind((bits & DifferentiabilityMask) >>
                                 DifferentiabilityMaskOffset);
  }

  constexpr bool isDifferentiable() const {
    return getDifferentiabilityKind() !=
           DifferentiabilityKind::NonDifferentiable;
  }

  /// Get the underlying ClangTypeInfo value.
  ClangTypeInfo getClangTypeInfo() const { return clangTypeInfo; }

  constexpr bool hasSelfParam() const {
    switch (getRepresentation()) {
    case Representation::Thick:
    case Representation::Block:
    case Representation::Thin:
    case Representation::CFunctionPointer:
    case Representation::Closure:
      return false;
    case Representation::ObjCMethod:
    case Representation::Method:
    case Representation::WitnessMethod:
    case SILFunctionTypeRepresentation::CXXMethod:
      return true;
    }
    llvm_unreachable("Unhandled Representation in switch.");
  }

  /// True if the function representation carries context.
  constexpr bool hasContext() const {
    switch (getRepresentation()) {
    case Representation::Thick:
    case Representation::Block:
      return true;
    case Representation::Thin:
    case Representation::CFunctionPointer:
    case Representation::ObjCMethod:
    case Representation::Method:
    case Representation::WitnessMethod:
    case Representation::Closure:
    case SILFunctionTypeRepresentation::CXXMethod:
      return false;
    }
    llvm_unreachable("Unhandled Representation in switch.");
  }

  // Note that we don't have setters. That is by design, use
  // the following with methods instead of mutating these objects.
  LLVM_NODISCARD
  SILExtInfoBuilder withRepresentation(Representation rep) const {
    return SILExtInfoBuilder((bits & ~RepresentationMask) | (unsigned)rep,
                             shouldStoreClangType(rep) ? clangTypeInfo
                                                       : ClangTypeInfo());
  }
  LLVM_NODISCARD
  SILExtInfoBuilder withIsPseudogeneric(bool isPseudogeneric = true) const {
    return SILExtInfoBuilder(isPseudogeneric ? (bits | PseudogenericMask)
                                             : (bits & ~PseudogenericMask),
                             clangTypeInfo);
  }
  LLVM_NODISCARD
  SILExtInfoBuilder withNoEscape(bool noEscape = true) const {
    return SILExtInfoBuilder(noEscape ? (bits | NoEscapeMask)
                                      : (bits & ~NoEscapeMask),
                             clangTypeInfo);
  }
  LLVM_NODISCARD
  SILExtInfoBuilder withAsync(bool isAsync = true) const {
    return SILExtInfoBuilder(isAsync ? (bits | AsyncMask) : (bits & ~AsyncMask),
                             clangTypeInfo);
  }
  LLVM_NODISCARD
  SILExtInfoBuilder
  withDifferentiabilityKind(DifferentiabilityKind differentiability) const {
    return SILExtInfoBuilder(
        (bits & ~DifferentiabilityMask) |
            ((unsigned)differentiability << DifferentiabilityMaskOffset),
        clangTypeInfo);
  }
  LLVM_NODISCARD
  SILExtInfoBuilder withClangFunctionType(const clang::Type *type) const {
    return SILExtInfoBuilder(bits, ClangTypeInfo(type).getCanonical());
  }

  bool isEqualTo(SILExtInfoBuilder other, bool useClangTypes) const {
    return bits == other.bits &&
           (useClangTypes ? (clangTypeInfo == other.clangTypeInfo) : true);
  }

  constexpr std::pair<unsigned, const void *> getFuncAttrKey() const {
    return std::make_pair(bits, clangTypeInfo.getType());
  }
}; // end SILExtInfoBuilder

// MARK: - SILExtInfo
/// Calling convention information for SILFunctionType.
///
/// New instances can be made from existing instances via \c SILExtInfoBuilder,
/// typically using a code pattern like:
/// \code
/// extInfo.intoBuilder().withX(x).withY(y).build()
/// \endcode
class SILExtInfo {
  friend SILExtInfoBuilder;
  friend SILFunctionType;

  SILExtInfoBuilder builder;

  // Only for use by SILExtInfoBuilder::build. Don't use it elsewhere!
  SILExtInfo(SILExtInfoBuilder builder) : builder(builder) {}

  SILExtInfo(unsigned bits, ClangTypeInfo clangTypeInfo)
      : builder(bits, clangTypeInfo) {
    builder.checkInvariants();
  };

public:
  /// An ExtInfo for a typical Swift function: thick, @escaping,
  /// non-pseudogeneric, non-differentiable.
  SILExtInfo() : builder() { builder.checkInvariants(); };

  SILExtInfo(ASTExtInfo info, bool isPseudogeneric)
      : builder(info.intoBuilder(), isPseudogeneric) {
    builder.checkInvariants();
  }

  /// A default ExtInfo but with a Thin convention.
  static SILExtInfo getThin() {
    return SILExtInfoBuilder(SILExtInfoBuilder::Representation::Thin, false,
                             false, false,
                             DifferentiabilityKind::NonDifferentiable, nullptr)
        .build();
  }

  /// Create a builder with the same state as \c this.
  SILExtInfoBuilder intoBuilder() const { return builder; }

private:
  constexpr unsigned getBits() const { return builder.bits; }

public:
  constexpr SILFunctionTypeRepresentation getRepresentation() const {
    return builder.getRepresentation();
  }

  constexpr SILFunctionLanguage getLanguage() const {
    return builder.getLanguage();
  }

  constexpr bool isPseudogeneric() const { return builder.isPseudogeneric(); }

  constexpr bool isNoEscape() const { return builder.isNoEscape(); }

  constexpr bool isAsync() const { return builder.isAsync(); }

  constexpr DifferentiabilityKind getDifferentiabilityKind() const {
    return builder.getDifferentiabilityKind();
  }

  constexpr bool isDifferentiable() const { return builder.isDifferentiable(); }

  ClangTypeInfo getClangTypeInfo() const { return builder.getClangTypeInfo(); }

  constexpr bool hasSelfParam() const { return builder.hasSelfParam(); }

  constexpr bool hasContext() const { return builder.hasContext(); }

  /// Helper method for changing the Representation.
  ///
  /// Prefer using \c SILExtInfoBuilder::withRepresentation for chaining.
  SILExtInfo withRepresentation(SILExtInfoBuilder::Representation rep) const {
    return builder.withRepresentation(rep).build();
  }

  /// Helper method for changing only the NoEscape field.
  ///
  /// Prefer using \c SILExtInfoBuilder::withNoEscape for chaining.
  SILExtInfo withNoEscape(bool noEscape = true) const {
    return builder.withNoEscape(noEscape).build();
  }
  

  SILExtInfo withAsync(bool isAsync = true) const {
    return builder.withAsync(isAsync).build();
  }

  bool isEqualTo(SILExtInfo other, bool useClangTypes) const {
    return builder.isEqualTo(other.builder, useClangTypes);
  }

  constexpr std::pair<unsigned, const void *> getFuncAttrKey() const {
    return builder.getFuncAttrKey();
  }

  Optional<UnexpectedClangTypeError> checkClangType() const;
};

/// Helper function to obtain the useClangTypes parameter for checking equality
/// of ExtInfos.
///
/// Typically, the argument will be a function type which was used to obtain one
/// of the ExtInfos.
template <typename HasContext> bool useClangTypes(HasContext hasContext) {
  return hasContext->getASTContext().LangOpts.UseClangFunctionTypes;
}

} // end namespace swift

#endif // SWIFT_EXTINFO_H
