//===--- SILLocation.h - Location information for SIL nodes -----*- C++ -*-===//
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

#ifndef SWIFT_SIL_LOCATION_H
#define SWIFT_SIL_LOCATION_H

#include "llvm/ADT/PointerUnion.h"
#include "swift/Basic/SourceLoc.h"
#include "swift/SIL/SILAllocated.h"
#include "swift/AST/TypeAlignments.h"

#include <cstddef>
#include <type_traits>

namespace swift {

class ReturnStmt;
class BraceStmt;
class AbstractClosureExpr;
class AbstractFunctionDecl;
class SILModule;
class SourceManager;

/// Represents a source location.
///
/// In most cases this is a pointer to an AST node from which the source
/// location can be retrieved from. In case the location refers to a parsed
/// or deserialized SIL file, the source location is represented differently.
/// For details see SILLocation::Storage.
class SILLocation {
public:
  enum LocationKind : uint8_t {
    RegularKind = 1,
    ReturnKind = 2,
    ImplicitReturnKind = 3,
    InlinedKind = 4,
    MandatoryInlinedKind = 5,
    CleanupKind = 6,
    ArtificialUnreachableKind = 7
  };

  /// Describes a position in a source file by explicitly storing the file name,
  /// line and column.
  ///
  /// This is used for parsed locations from a SIL file, for
  /// "-sil-based-debuginfo" (see SILDebugInfoGenerator) and for the
  /// "compiler-generated" singleton location.
  /// In future we might also use it for de-serialized locations from a
  /// swiftmodule file.
  struct FilenameAndLocation : public SILAllocated<FilenameAndLocation> {
    unsigned line;
    uint16_t column;
    StringRef filename;

    FilenameAndLocation(unsigned line = 0, uint16_t column = 0,
                      StringRef filename = StringRef())
        : line(line), column(column), filename(filename) {}

    static FilenameAndLocation *alloc(unsigned line, unsigned column,
                               StringRef filename, SILModule &module);

    inline bool operator==(const FilenameAndLocation &rhs) const {
      return line == rhs.line && column == rhs.column &&
             filename.equals(rhs.filename);
    }

    void dump() const;
    void print(raw_ostream &OS) const;
    friend llvm::hash_code hash_value(const FilenameAndLocation &);
  };

protected:
  template <class T, class Enable = void>
  struct base_type;

  template <class T>
  struct base_type<T,
      typename std::enable_if<std::is_base_of<Decl, T>::value>::type> {
    using type = Decl;
  };

  template <class T>
  struct base_type<T,
      typename std::enable_if<std::is_base_of<Expr, T>::value>::type> {
    using type = Expr;
  };

  template <class T>
  struct base_type<T,
      typename std::enable_if<std::is_base_of<Stmt, T>::value>::type> {
    using type = Stmt;
  };

  template <class T>
  struct base_type<T,
      typename std::enable_if<std::is_base_of<Pattern, T>::value>::type> {
    using type = Pattern;
  };

  /// The int flag indicates whether the node's end location should be used.
  using ASTNodeTy = llvm::PointerIntPair<
      llvm::PointerUnion<Stmt *, Expr *, Decl *, Pattern *>, 1>;

  /// Used in case the location for diagnostics does not match the location for
  /// debugging.
  struct ExtendedASTNodeLoc : public SILAllocated<ExtendedASTNodeLoc> {
    /// Primary AST location, always used for diagnostics.
    ASTNodeTy primary;
    /// Sometimes the location for diagnostics needs to be different than the
    /// one used to emit the line table for debugging.
    ASTNodeTy forDebugging;
    
    ExtendedASTNodeLoc(ASTNodeTy primary, ASTNodeTy forDebugging) :
      primary(primary), forDebugging(forDebugging) {}
  };

private:
  friend class SILInstruction;

  /// Each kind corresponds to a union member in `Storage`.
  enum StorageKind : uint8_t {
    /// For usages see the struct `FilenameAndLocation`.
    FilenameAndLocationKind,
    
    /// The most common location kind: a pointer to the AST from which the
    /// source location can be retrieved.
    /// This is the "normal" case when compiling a swift file.
    ASTNodeKind,
    
    /// Use for a few locations in pattern-code. See ExtendedASTNodeLoc.
    ExtendedASTNodeKind,
    
    /// This is used when parsing a SIL file.
    /// Note: this is only used for functions/instruction for which there is no
    /// location specified in the SIL file.
    /// In case a SIL instruction is annotated with a parsable location in
    /// the SIL file, the instruction gets a FilenameAndLocation.
    SourceLocKind
  };

  /// The storage which actually points to the source location. Each union
  /// member corresponds to a StorageKind and is basically a pointer.
  /// Note that a referenced FilenameAndLocation or ExtendedASTNodeLoc is not
  /// owned by SILLocation, thus not freed on destruction. Those data structures
  /// are allocated with the SILModule's bump pointer allocator.
  union Storage {
    Storage() : filePositionLoc(nullptr) {}
    Storage(FilenameAndLocation *F) : filePositionLoc(F) {}
    Storage(ASTNodeTy N) : ASTNodeLoc(N) {}
    Storage(ExtendedASTNodeLoc *E) : extendedASTNodeLoc(E) {}
    Storage(SourceLoc L) : sourceLoc(L) {}

    const FilenameAndLocation *filePositionLoc;
    ASTNodeTy ASTNodeLoc;
    ExtendedASTNodeLoc *extendedASTNodeLoc;
    SourceLoc sourceLoc;
  };

  static_assert(sizeof(SILLocation::Storage) == sizeof(void *),
                "SILLocation::Storage must stay small");

  /// Contains the LocationKind, StorageKind and some extra flags.
  /// This fits nicely in a single byte. If for some reason we need more flags
  /// it's possible to extend this to e.g. a uint16_t.
  /// But SILNode::locationKindAndFlags must be updated accordingly.
  union KindAndFlags {
    KindAndFlags() : packedKindAndFlags(0) {}
    KindAndFlags(LocationKind kind, StorageKind storageKind)
      : fields({kind, storageKind, 0, 0, 0}) {
      assert(fields.kind == kind && "LocationKind overflow");
      assert(fields.storageKind == storageKind && "StorageKind overflow");
    }
    KindAndFlags(uint8_t packed) : packedKindAndFlags(packed) {}
  
    uint8_t packedKindAndFlags;
    struct Fields {
      uint8_t kind: 3;
      uint8_t storageKind: 2;
      uint8_t autoGenerated: 1;
      uint8_t implicit: 1;
      uint8_t inPrologue: 1;
    } fields;
  };

  static_assert(sizeof(SILLocation::KindAndFlags) == sizeof(uint8_t),
                "SILLocation::KindAndFlags should be 1 byte");

  // Data fields:
  Storage storage;
  KindAndFlags kindAndFlags;

  StorageKind getStorageKind() const {
    return (StorageKind)kindAndFlags.fields.storageKind;
  }

  ASTNodeTy getPrimaryASTNode() const {
    switch (getStorageKind()) {
    case ASTNodeKind:
      return storage.ASTNodeLoc;
    case ExtendedASTNodeKind:
      return storage.extendedASTNodeLoc->primary;
    case SourceLocKind:
    case FilenameAndLocationKind:
      llvm_unreachable("location type has no AST node");
    }
    llvm_unreachable("covered switch");
    }

  /// Returns true if the location has a separate AST node for debugging.
  /// See ExtendedASTNodeLoc.
  bool hasASTNodeForDebugging() const {
    return getStorageKind() == ExtendedASTNodeKind;
  }

  template <typename T>
  T *getNodeAs(ASTNodeTy Node) const {
    using base = typename base_type<T>::type*;
    return dyn_cast_or_null<T>(Node.getPointer().dyn_cast<base>());
  }

  template <typename T>
  bool isNode(ASTNodeTy Node) const {
    assert(isASTNode());
    ASTNodeTy primaryNd = getPrimaryASTNode();
    if (primaryNd.getPointer().is<typename base_type<T>::type*>())
      return isa<T>(Node.getPointer().get<typename base_type<T>::type*>());
    return false;
  }

  template <typename T>
  T *castNodeTo(ASTNodeTy Node) const {
    return cast<T>(Node.getPointer().get<typename base_type<T>::type*>());
  }

  SourceLoc getSourceLoc(ASTNodeTy N) const;
  static SourceLoc getStartSourceLoc(ASTNodeTy N);
  static SourceLoc getEndSourceLoc(ASTNodeTy N);

protected:
  SILLocation(LocationKind K) : kindAndFlags(K, FilenameAndLocationKind) {}

  /// Constructs a null location.
  SILLocation() : SILLocation(RegularKind) { assert(isNull()); }

  /// Constructs a location like \p l, but with a different kind.
  SILLocation(SILLocation l, LocationKind K)
      : storage(l.storage), kindAndFlags(l.kindAndFlags.packedKindAndFlags) {
    kindAndFlags.fields.kind = K;
  }

  SILLocation(FilenameAndLocation *filePos, LocationKind K)
      : storage(filePos), kindAndFlags(K, FilenameAndLocationKind) {
    assert(filePos && !filePos->filename.empty());
  }

  // It is okay to pass a nullptr, but preferably, a null-location should be
  // created with `invalid()`.
  SILLocation(ASTNodeTy Node, LocationKind K)
      : storage(Node), kindAndFlags(K, ASTNodeKind) {}

  SILLocation(ExtendedASTNodeLoc *ext, LocationKind K)
      : storage(ext), kindAndFlags(K, ExtendedASTNodeKind) {
  }

  SILLocation(SourceLoc L, LocationKind K, bool Implicit);

  // Used by SILInstruction.
  SILLocation(Storage storage, uint8_t packedKindAndFlags) :
    storage(storage), kindAndFlags(packedKindAndFlags) {}

public:
  // When an ASTNode gets implicitly converted into a SILLocation we
  // construct a RegularLocation. Since RegularLocations represent the majority
  // of locations, this greatly simplifies the user code.
  //
  // It is okay to pass a nullptr to these constructors, but preferably, a
  // null-location should be created with `invalid()`.
  SILLocation(Stmt *S);
  SILLocation(Expr *E);
  SILLocation(Decl *D);
  SILLocation(Pattern *P);

  /// Returns a null location.
  static SILLocation invalid() { return SILLocation(); }

  LocationKind getKind() const {
    return (LocationKind)kindAndFlags.fields.kind;
  }

  /// Artificial locations and the top-level module locations will be null.
  bool isNull() const {
    switch (getStorageKind()) {
    case ASTNodeKind:
    case ExtendedASTNodeKind:     return !getPrimaryASTNode().getPointer();
    case FilenameAndLocationKind: return storage.filePositionLoc == nullptr;
    case SourceLocKind:           return storage.sourceLoc.isInvalid();
    }
    llvm_unreachable("covered switch");
  }
  explicit operator bool() const { return !isNull(); }

  bool hasValidLineNumber() const {
    if (isNull())
      return false;
    if (isFilenameAndLocation() && getFilenameAndLocation()->line == 0)
      return false;
    return true;
  }

  /// Return true if this location is backed by an AST node.
  bool isASTNode() const {
    switch (getStorageKind()) {
    case ASTNodeKind:
    case ExtendedASTNodeKind:
      return true;
    case SourceLocKind:
    case FilenameAndLocationKind:
      return false;
    }
    llvm_unreachable("covered switch");
  }

  /// Returns true if this location came from a SIL file.
  bool isSILFile() const { return getStorageKind() == SourceLocKind; }

  bool isFilenameAndLocation() const {
    return getStorageKind() == FilenameAndLocationKind && !isNull();
  }
  const FilenameAndLocation *getFilenameAndLocation() const {
    assert(isFilenameAndLocation());
    return storage.filePositionLoc;
  }

  /// Marks the location as coming from auto-generated body.
  void markAutoGenerated() { kindAndFlags.fields.autoGenerated = true; }

  /// Marks the location as not being auto-generated.
  /// FIXME: This functionality is only used to work around bugs and should be
  /// removed.
  void markNonAutoGenerated() { kindAndFlags.fields.autoGenerated = false; }

  /// Returns this location with the auto-generated flag set.
  SILLocation asAutoGenerated() const {
    SILLocation loc = *this;
    loc.markAutoGenerated();
    return loc;
  }

  /// Returns true if the location represents an artificially generated
  /// body, such as thunks or default destructors.
  bool isAutoGenerated() const { return kindAndFlags.fields.autoGenerated; }

  /// Returns true if the location was created from an implicit AST node.
  /// TODO: This is very similar to autogenerated,
  ///       and these two properties should be merged.
  bool isImplicit() const { return kindAndFlags.fields.implicit; }

  /// Mark this location as not being implicit.
  void markExplicit() { kindAndFlags.fields.implicit = false; }

  /// Returns false if the location should be represented in debuginfo.
  bool isHiddenFromDebugInfo() const {
    return (isAutoGenerated() || isImplicit()) && !hasASTNodeForDebugging();
  }

  /// Returns true if the line number of this location is zero.
  bool isLineZero(const SourceManager &SM) const {
    return decodeForDebugging(SM).line == 0;
  }

  /// Changes the default source location position to point to the end of
  /// the AST node.
  void pointToEnd();

  /// Mark this location as being part of the function
  /// prologue, which means that it deals with setting up the stack
  /// frame. The first breakpoint location in a function is at the end
  /// of the prologue.
  void markAsPrologue() { kindAndFlags.fields.inPrologue = true; }

  /// Check is this location is part of a function's implicit prologue.
  bool isInPrologue() const { return kindAndFlags.fields.inPrologue; }

  /// Check if the corresponding source code location definitely points
  ///  to the end of the AST node.
  bool pointsToEnd() const;

  template <typename T> bool is() const { return T::isKind(*this); }

  /// If the current value is of the specified AST unit type T,
  /// return it, otherwise return null.
  template <typename T> T *getAsASTNode() const {
    return isASTNode() ? getNodeAs<T>(getPrimaryASTNode()) : nullptr;
  }

  /// Returns true if the Location currently points to the AST node
  /// matching type T.
  template <typename T> bool isASTNode() const {
    return isASTNode() && isNode<T>(getPrimaryASTNode());
  }

  /// Returns the primary value as the specified AST node type. If the
  /// specified type is incorrect, asserts.
  template <typename T> T *castToASTNode() const {
    return castNodeTo<T>(getPrimaryASTNode());
  }

  /// Return the location as a DeclContext or null.
  DeclContext *getAsDeclContext() const;

  /// Returns the source location for diagnoses.
  SourceLoc getSourceLoc() const;

  /// Heuristic to detect the fake source locations generated for synthesized
  /// conformances.
  bool isSynthesizedAST() {
    return isASTNode() && getSourceLoc().isInvalid();
  }

  /// Returns the source location for debugging.
  /// See ExtendedASTNodeLoc.
  SourceLoc getSourceLocForDebugging() const;
  
  SourceLoc getStartSourceLoc() const;
  SourceLoc getEndSourceLoc() const;
  SourceRange getSourceRange() const {
    return {getStartSourceLoc(), getEndSourceLoc()};
  }

  /// Extract the line, column, and filename from \p Loc.
  ///
  /// \p ForceGeneratedSourceToDisk can be set to true to create a temporary
  /// file on-disk for buffers containing generated source code, returning the
  /// name of that temporary file.
  static FilenameAndLocation decode(SourceLoc Loc, const SourceManager &SM,
                                    bool ForceGeneratedSourceToDisk = false);

  /// Return the decoded FilenameAndLocation.
  /// In case the location has a separate AST node for debugging, this node is
  /// used. See ExtendedASTNodeLoc.
  FilenameAndLocation decodeForDebugging(const SourceManager &SM) const {
    return isFilenameAndLocation() ? *getFilenameAndLocation()
                            : decode(getSourceLocForDebugging(), SM);
  }

  /// Compiler-generated locations may be applied to instructions without any
  /// clear correspondence to an AST node in an otherwise normal function.
  static FilenameAndLocation *getCompilerGeneratedLoc();
  
  /// Pretty-print the value.
  void dump() const;
  void print(raw_ostream &OS, const SourceManager &SM) const;
  void print(raw_ostream &OS) const;

  inline bool operator==(const SILLocation& R) const {
    if (kindAndFlags.packedKindAndFlags != R.kindAndFlags.packedKindAndFlags)
      return false;

    if (isFilenameAndLocation()) {
      assert(R.isFilenameAndLocation());
      return *getFilenameAndLocation() == *R.getFilenameAndLocation();
    }

    return storage.filePositionLoc == R.storage.filePositionLoc;
  }

  inline bool operator!=(const SILLocation &R) const { return !(*this == R); }

  bool hasSameSourceLocation(const SILLocation &rhs) {
    if (*this == rhs)
      return true;
    if (isASTNode() && rhs.isASTNode()) {
      return getSourceLoc(getPrimaryASTNode()) == rhs.getSourceLoc(rhs.getPrimaryASTNode());
    }
    return false;
  }

  friend llvm::hash_code hash_value(const SILLocation &);
  friend class RegularLocation;
};

inline llvm::hash_code hash_value(const SILLocation &R) {
  return llvm::hash_combine(R.kindAndFlags.packedKindAndFlags,
                            *R.storage.filePositionLoc);
}

inline llvm::hash_code hash_value(const SILLocation::FilenameAndLocation &R) {
  return llvm::hash_combine(R.line, R.column, R.filename);
}

/// Allowed on any instruction.
class RegularLocation : public SILLocation {
public:
  RegularLocation(Stmt *S) : SILLocation(ASTNodeTy(S), RegularKind) {}
  RegularLocation(Expr *E) : SILLocation(ASTNodeTy(E), RegularKind) {}
  RegularLocation(Decl *D) : SILLocation(ASTNodeTy(D), RegularKind) {}
  RegularLocation(Pattern *P) : SILLocation(ASTNodeTy(P), RegularKind) {}
  /// Used for bindings that need to be injected into a different scope than
  /// that of their VarDecls.
  RegularLocation(Decl *D, SILLocation LocForDebugging, SILModule &Module);
  /// Used for switch statements to avoid line table entries for trivial
  /// unsuccessful case comparisons.
  RegularLocation(Stmt *S, Pattern *P, SILModule &Module);
  RegularLocation(SILLocation ForDebuggingOrDiagnosticsOnly, SILModule &Module,
                  bool isForDebugOnly = true);
  RegularLocation(SourceLoc L, bool Implicit = true)
      : SILLocation(L, RegularKind, Implicit) {}
  RegularLocation(FilenameAndLocation *filePos)
    : SILLocation(filePos, RegularKind) {}

  /// Convert \p loc to a RegularLocation.
  explicit RegularLocation(SILLocation loc) : SILLocation(loc, RegularKind) {}

  /// Returns a location representing the module.
  /// This is just a null location.
  static RegularLocation getModuleLocation() { return RegularLocation(); }

  /// Compiler-generated locations may be applied to instructions without any
  /// clear correspondence to an AST node in an otherwise normal function.
  /// The auto-generated bit also turns off certain diagnostics passes such.
  static RegularLocation getAutoGeneratedLocation() {
    RegularLocation AL(getCompilerGeneratedLoc());
    AL.markAutoGenerated();
    return AL;
  }

  /// Returns a location that is compiler-generated, but with a hint as to where
  /// it may have been generated from. These locations will have an artificial
  /// line location of zero in DWARF, but in CodeView we want to use the given
  /// line since line zero does not represent an artificial line in CodeView.
  template <typename InputLocTy>
  static RegularLocation getAutoGeneratedLocation(InputLocTy L) {
    RegularLocation AL(L);
    AL.markAutoGenerated();
    return AL;
  }

  /// Returns a location that is empty for diagnostics, and L for the debug info
  /// Used for \c hop_to_executor instructions.
  static RegularLocation getDebugOnlyLocation(SILLocation L, SILModule &M) {
    if (L.isASTNode())
      return RegularLocation(L, M);
    return getAutoGeneratedLocation(L);
  }

  /// Returns a location that uses L for diagnostics but is otherwise an auto
  /// generated location.
  static RegularLocation getDiagnosticsOnlyLocation(SILLocation L,
                                                    SILModule &M) {
    if (L.isASTNode())
      return RegularLocation(L, M, false /*is for debug only*/);
    return getAutoGeneratedLocation(L);
  }

  static bool isKind(const SILLocation& L) {
    return L.getKind() == RegularKind;
  }

private:
  RegularLocation() : SILLocation(RegularKind) {}
  static SILLocation::ExtendedASTNodeLoc *
  getDebugOnlyExtendedASTNodeLoc(SILLocation L, SILModule &Module);
  static SILLocation::ExtendedASTNodeLoc *
  getDiagnosticOnlyExtendedASTNodeLoc(SILLocation L, SILModule &Module);
};

/// Used to represent a return instruction in user code.
///
/// Allowed on an BranchInst, ReturnInst.
class ReturnLocation : public SILLocation {
public:
  ReturnLocation(ReturnStmt *RS);

  /// Construct the return location for a constructor or a destructor.
  ReturnLocation(BraceStmt *BS);

  ReturnLocation(FilenameAndLocation *filePos)
    : SILLocation(filePos, ReturnKind) {}

  static bool isKind(const SILLocation& L) {
    return L.getKind() == ReturnKind;
  }
};

/// Used on the instruction that was generated to represent an implicit
/// return from a function.
///
/// Allowed on an BranchInst, ReturnInst.
class ImplicitReturnLocation : public SILLocation {
public:

  ImplicitReturnLocation(AbstractClosureExpr *E);

  ImplicitReturnLocation(ReturnStmt *S);

  ImplicitReturnLocation(AbstractFunctionDecl *AFD);

  ImplicitReturnLocation(FilenameAndLocation *filePos)
    : SILLocation(filePos, ImplicitReturnKind) {}

  /// Convert \p loc to an ImplicitReturnLocation.
  explicit ImplicitReturnLocation(SILLocation Loc);

  static bool isKind(const SILLocation& L) {
    return L.getKind() == ImplicitReturnKind;
  }
};

/// Marks instructions that correspond to inlined function body and
/// setup code. This location should not be used for inlined transparent
/// bodies, see MandatoryInlinedLocation.
///
/// This location wraps the call site ASTNode.
///
/// Allowed on any instruction except for ReturnInst.
class InlinedLocation : public SILLocation {
public:
  /// Convert \p loc to an InlinedLocation.
  InlinedLocation(SILLocation L) : SILLocation(L, InlinedKind) {}

  static bool isKind(const SILLocation& L) {
    return L.getKind() == InlinedKind;
  }
};

/// Marks instructions that correspond to inlined function body and
/// setup code for transparent functions, inlined as part of mandatory inlining
/// pass.
///
/// This location wraps the call site ASTNode.
///
/// Allowed on any instruction except for ReturnInst.
class MandatoryInlinedLocation : public SILLocation {
public:
  MandatoryInlinedLocation() : SILLocation(MandatoryInlinedKind) {}

  /// Convert \p loc to a MandatoryInlinedLocation.
  explicit MandatoryInlinedLocation(SILLocation L)
    : SILLocation(L, MandatoryInlinedKind) {}

  static MandatoryInlinedLocation getAutoGeneratedLocation() {
    return MandatoryInlinedLocation(
      RegularLocation::getAutoGeneratedLocation());
  }

  static bool isKind(const SILLocation& L) {
    return L.getKind() == MandatoryInlinedKind;
  }
};

/// Used on the instruction performing auto-generated cleanup such as
/// deallocs, destructor calls.
///
/// The cleanups are performed after completing the evaluation of the AST Node
/// wrapped inside the SILLocation. This location wraps the statement
/// representing the enclosing scope, for example, FuncDecl, ParenExpr. The
/// scope's end location points to the SourceLoc that shows when the operation
/// is performed at runtime.
///
/// Allowed on any instruction except for ReturnInst.
/// Locations of an inlined destructor should also be represented by this.
class CleanupLocation : public SILLocation {
public:
  CleanupLocation(Expr *E) : SILLocation(ASTNodeTy(E), CleanupKind) {}
  CleanupLocation(Stmt *S) : SILLocation(ASTNodeTy(S), CleanupKind) {}
  CleanupLocation(Pattern *P) : SILLocation(ASTNodeTy(P), CleanupKind) {}
  CleanupLocation(Decl *D) : SILLocation(ASTNodeTy(D), CleanupKind) {}
  
  /// Convert \p loc to a CleanupLocation.
  explicit CleanupLocation(SILLocation L) : SILLocation(L, CleanupKind) {}

  /// Returns a null location.
  static CleanupLocation invalid() { return CleanupLocation(); }

  /// Returns a location representing a cleanup on the module level.
  /// This is just a null location.
  static CleanupLocation getModuleCleanupLocation() {
    return CleanupLocation();
  }

  static bool isKind(const SILLocation& L) {
    return L.getKind() == CleanupKind;
  }

private:
  CleanupLocation() : SILLocation(CleanupKind) {}
};

/// Used to represent an unreachable location that was
/// auto-generated and has no correspondence to user code. It should
/// not be used in diagnostics or for debugging.
///
/// Differentiates an unreachable instruction, which is generated by
/// DCE, from an unreachable instruction in user code (output of SILGen).
/// Allowed on an unreachable instruction.
class ArtificialUnreachableLocation : public SILLocation {
public:
  ArtificialUnreachableLocation() : SILLocation(ArtificialUnreachableKind) {}

  static bool isKind(const SILLocation& L) {
    return (L.getKind() == ArtificialUnreachableKind);
  }
};

class SILDebugScope;

/// A SILLocation paired with a SILDebugScope.
class SILDebugLocation {
  const SILDebugScope *debugScope;
  SILLocation location;

public:
  SILDebugLocation()
      : debugScope(nullptr),
        location(RegularLocation::getAutoGeneratedLocation()) {}
  SILDebugLocation(SILLocation location, const SILDebugScope *debugScope)
      : debugScope(debugScope), location(location) {}
  SILLocation getLocation() const { return location; }
  const SILDebugScope *getScope() const { return debugScope; }
  bool hasValidLineNumber() const { return location.hasValidLineNumber(); }
  bool isAutoGenerated() const { return location.isAutoGenerated(); }
  operator bool() const { return bool(location) && debugScope; }

  std::string getDebugDescription() const;

  SWIFT_IMPORT_UNSAFE
  SILDebugLocation getAutogeneratedLocation() const {
    SILDebugLocation autoGenLoc(RegularLocation::getAutoGeneratedLocation(), getScope());
    return autoGenLoc;
  }

  bool isEqualTo(SILDebugLocation rhs) const {
    return getLocation() == rhs.getLocation() && getScope() == rhs.getScope();
  }

  bool hasSameSourceLocation(swift::SILDebugLocation rhs) const {
    return getLocation().hasSameSourceLocation(rhs.getLocation()) &&
           getScope() == rhs.getScope();
  }

  static SILDebugLocation getArtificialUnreachableLocation() {
    return SILDebugLocation(ArtificialUnreachableLocation(), nullptr);
  }
};

} // end swift namespace

#endif
