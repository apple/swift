//===--- Identifier.h - Uniqued Identifier ----------------------*- C++ -*-===//
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
// This file defines the Identifier interface.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_IDENTIFIER_H
#define SWIFT_AST_IDENTIFIER_H

#include "swift/Basic/EditorPlaceholder.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/LLVM.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/Support/TrailingObjects.h"

namespace llvm {
  class raw_ostream;
}

namespace swift {
  class ASTContext;
  class ParameterList;

/// DeclRefKind - The kind of reference to an identifier.
enum class DeclRefKind {
  /// An ordinary reference to an identifier, e.g. 'foo'.
  Ordinary,

  /// A reference to an identifier as a binary operator, e.g. '+' in 'a+b'.
  BinaryOperator,

  /// A reference to an identifier as a postfix unary operator, e.g. '++' in
  /// 'a++'.
  PostfixOperator,

  /// A reference to an identifier as a prefix unary operator, e.g. '--' in
  /// '--a'.
  PrefixOperator
};

/// Identifier - This is an instance of a uniqued identifier created by
/// ASTContext.  It just wraps a nul-terminated "const char*".
class Identifier {
  friend class ASTContext;
  friend class DeclBaseName;

  const char *Pointer;
  
public:
  enum : size_t {
    NumLowBitsAvailable = 3,
    RequiredAlignment = 1 << NumLowBitsAvailable,
    SpareBitMask = ((intptr_t)1 << NumLowBitsAvailable) - 1
  };

private:
  /// Constructor, only accessible by ASTContext, which handles the uniquing.
  explicit Identifier(const char *Ptr) : Pointer(Ptr) {
    assert(((uintptr_t)Ptr & SpareBitMask) == 0
           && "Identifier pointer does not use any spare bits");
  }

  /// A type with the alignment expected of a valid \c Identifier::Pointer .
  struct alignas(uint64_t) Aligner {};

  static_assert(alignof(Aligner) >= RequiredAlignment,
                "Identifier table will provide enough spare bits");

public:
  explicit Identifier() : Pointer(nullptr) {}
  
  const char *get() const { return Pointer; }
  
  StringRef str() const { return Pointer; }

  explicit operator std::string() const { return std::string(Pointer); }

  unsigned getLength() const {
    assert(Pointer != nullptr && "Tried getting length of empty identifier");
    return ::strlen(Pointer);
  }
  
  bool empty() const { return Pointer == nullptr; }

  bool is(StringRef string) const { return str().equals(string); }
  
  /// isOperator - Return true if this identifier is an operator, false if it is
  /// a normal identifier.
  /// FIXME: We should maybe cache this.
  bool isOperator() const {
    if (empty())
      return false;
    if (isEditorPlaceholder())
      return false;
    if ((unsigned char)Pointer[0] < 0x80)
      return isOperatorStartCodePoint((unsigned char)Pointer[0]);

    // Handle the high unicode case out of line.
    return isOperatorSlow();
  }

  // Returns whether this is a standard comparison operator,
  // such as '==', '>=' or '!=='.
  bool isStandardComparisonOperator() const {
    return is("==") || is("!=") || is("===") || is("!==") || is("<") ||
           is(">") || is("<=") || is(">=");
  }

  /// isOperatorStartCodePoint - Return true if the specified code point is a
  /// valid start of an operator.
  static bool isOperatorStartCodePoint(uint32_t C) {
    // ASCII operator chars.
    static const char OpChars[] = "/=-+*%<>!&|^~.?";
    if (C < 0x80)
      return memchr(OpChars, C, sizeof(OpChars) - 1) != 0;
    
    // Unicode math, symbol, arrow, dingbat, and line/box drawing chars.
    return (C >= 0x00A1 && C <= 0x00A7)
        || C == 0x00A9 || C == 0x00AB || C == 0x00AC || C == 0x00AE
        || C == 0x00B0 || C == 0x00B1 || C == 0x00B6 || C == 0x00BB
        || C == 0x00BF || C == 0x00D7 || C == 0x00F7
        || C == 0x2016 || C == 0x2017 || (C >= 0x2020 && C <= 0x2027)
        || (C >= 0x2030 && C <= 0x203E) || (C >= 0x2041 && C <= 0x2053)
        || (C >= 0x2055 && C <= 0x205E) || (C >= 0x2190 && C <= 0x23FF)
        || (C >= 0x2500 && C <= 0x2775) || (C >= 0x2794 && C <= 0x2BFF)
        || (C >= 0x2E00 && C <= 0x2E7F) || (C >= 0x3001 && C <= 0x3003)
        || (C >= 0x3008 && C <= 0x3030);
  }
  
  /// isOperatorContinuationCodePoint - Return true if the specified code point
  /// is a valid operator code point.
  static bool isOperatorContinuationCodePoint(uint32_t C) {
    if (isOperatorStartCodePoint(C))
      return true;

    // Unicode combining characters and variation selectors.
    return (C >= 0x0300 && C <= 0x036F)
        || (C >= 0x1DC0 && C <= 0x1DFF)
        || (C >= 0x20D0 && C <= 0x20FF)
        || (C >= 0xFE00 && C <= 0xFE0F)
        || (C >= 0xFE20 && C <= 0xFE2F)
        || (C >= 0xE0100 && C <= 0xE01EF);
  }

  static bool isEditorPlaceholder(StringRef name) {
    return swift::isEditorPlaceholder(name);
  }

  bool isEditorPlaceholder() const {
    return !empty() && isEditorPlaceholder(str());
  }
  
  const void *getAsOpaquePointer() const {
      return static_cast<const void *>(Pointer);
  }
  
  static Identifier getFromOpaquePointer(void *P) {
    return Identifier((const char*)P);
  }

  /// Compare two identifiers, producing -1 if \c *this comes before \c other,
  /// 1 if \c *this comes after \c other, and 0 if they are equal.
  ///
  /// Null identifiers come after all other identifiers.
  int compare(Identifier other) const;

  friend llvm::hash_code hash_value(Identifier ident) {
    return llvm::hash_value(ident.getAsOpaquePointer());
  }

  bool operator==(Identifier RHS) const { return Pointer == RHS.Pointer; }
  bool operator!=(Identifier RHS) const { return !(*this==RHS); }

  bool operator<(Identifier RHS) const { return Pointer < RHS.Pointer; }
  
  static Identifier getEmptyKey() {
    uintptr_t Val = static_cast<uintptr_t>(-1);
    Val <<= NumLowBitsAvailable;
    return Identifier((const char*)Val);
  }

  static Identifier getTombstoneKey() {
    uintptr_t Val = static_cast<uintptr_t>(-2);
    Val <<= NumLowBitsAvailable;
    return Identifier((const char*)Val);
  }

private:
  bool isOperatorSlow() const;
};
  
class DeclName;
class DeclNameRef;
class ObjCSelector;

} // end namespace swift

namespace llvm {
  raw_ostream &operator<<(raw_ostream &OS, swift::Identifier I);
  raw_ostream &operator<<(raw_ostream &OS, swift::DeclName I);
  raw_ostream &operator<<(raw_ostream &OS, swift::DeclNameRef I);
  raw_ostream &operator<<(raw_ostream &OS, swift::ObjCSelector S);

  // Identifiers hash just like pointers.
  template<> struct DenseMapInfo<swift::Identifier> {
    static swift::Identifier getEmptyKey() {
      return swift::Identifier::getEmptyKey();
    }
    static swift::Identifier getTombstoneKey() {
      return swift::Identifier::getTombstoneKey();
    }
    static unsigned getHashValue(swift::Identifier Val) {
      return DenseMapInfo<const void*>::getHashValue(Val.get());
    }
    static bool isEqual(swift::Identifier LHS, swift::Identifier RHS) {
      return LHS == RHS;
    }
  };
  
  // An Identifier is "pointer like".
  template<typename T> struct PointerLikeTypeTraits;
  template<>
  struct PointerLikeTypeTraits<swift::Identifier> {
  public:
    static inline void *getAsVoidPointer(swift::Identifier I) {
      return const_cast<void *>(I.getAsOpaquePointer());
    }
    static inline swift::Identifier getFromVoidPointer(void *P) {
      return swift::Identifier::getFromOpaquePointer(P);
    }
    enum { NumLowBitsAvailable = swift::Identifier::NumLowBitsAvailable };
  };
  
} // end namespace llvm

namespace swift {

/// Wrapper that may either be an Identifier or a special name
/// (e.g. for subscripts)
class DeclBaseName {
public:
  enum class Kind: uint8_t {
    Normal,
    Subscript,
    Constructor,
    Destructor
  };
  
private:
  /// In a special DeclName representing a subscript, this opaque pointer
  /// is used as the data of the base name identifier.
  /// This is an implementation detail that should never leak outside of
  /// DeclName.
  static const Identifier::Aligner SubscriptIdentifierData;
  /// As above, for special constructor DeclNames.
  static const Identifier::Aligner ConstructorIdentifierData;
  /// As above, for special destructor DeclNames.
  static const Identifier::Aligner DestructorIdentifierData;

  Identifier Ident;

public:
  DeclBaseName() : DeclBaseName(Identifier()) {}

  DeclBaseName(Identifier I) : Ident(I) {}

  static DeclBaseName createSubscript() {
    return DeclBaseName(Identifier((const char *)&SubscriptIdentifierData));
  }

  static DeclBaseName createConstructor() {
    return DeclBaseName(Identifier((const char *)&ConstructorIdentifierData));
  }

  static DeclBaseName createDestructor() {
    return DeclBaseName(Identifier((const char *)&DestructorIdentifierData));
  }

  Kind getKind() const {
    if (Ident.get() == (const char *)&SubscriptIdentifierData) {
      return Kind::Subscript;
    } else if (Ident.get() == (const char *)&ConstructorIdentifierData) {
      return Kind::Constructor;
    } else if (Ident.get() == (const char *)&DestructorIdentifierData) {
        return Kind::Destructor;
    } else {
      return Kind::Normal;
    }
  }

  bool isSpecial() const { return getKind() != Kind::Normal; }

  bool isSubscript() const { return getKind() == Kind::Subscript; }

  /// Return the identifier backing the name. Assumes that the name is not
  /// special.
  Identifier getIdentifier() const {
    assert(!isSpecial() && "Cannot retrieve identifier from special names");
    return Ident;
  }

  bool empty() const { return !isSpecial() && getIdentifier().empty(); }

  bool isOperator() const {
    return !isSpecial() && getIdentifier().isOperator();
  }

  bool isEditorPlaceholder() const {
    return !isSpecial() && getIdentifier().isEditorPlaceholder();
  }

  /// A representation of the name to be displayed to users. May be ambiguous
  /// between identifiers and special names.
  StringRef userFacingName() const {
    if (empty())
      return "_";

    switch (getKind()) {
    case Kind::Normal:
      return getIdentifier().str();
    case Kind::Subscript:
      return "subscript";
    case Kind::Constructor:
      return "init";
    case Kind::Destructor:
      return "deinit";
    }
    llvm_unreachable("unhandled kind");
  }

  int compare(DeclBaseName other) const {
    return userFacingName().compare(other.userFacingName());
  }

  bool operator==(StringRef Str) const {
    return !isSpecial() && getIdentifier().is(Str);
  }
  bool operator!=(StringRef Str) const { return !(*this == Str); }

  bool operator==(DeclBaseName RHS) const { return Ident == RHS.Ident; }
  bool operator!=(DeclBaseName RHS) const { return !(*this == RHS); }

  bool operator<(DeclBaseName RHS) const {
    return Ident.get() < RHS.Ident.get();
  }

  const void *getAsOpaquePointer() const { return Ident.get(); }

  static DeclBaseName getFromOpaquePointer(void *P) {
    return Identifier::getFromOpaquePointer(P);
  }
};

} // end namespace swift

namespace llvm {

raw_ostream &operator<<(raw_ostream &OS, swift::DeclBaseName D);

// DeclBaseNames hash just like pointers.
template<> struct DenseMapInfo<swift::DeclBaseName> {
  static swift::DeclBaseName getEmptyKey() {
    return swift::Identifier::getEmptyKey();
  }
  static swift::DeclBaseName getTombstoneKey() {
    return swift::Identifier::getTombstoneKey();
  }
  static unsigned getHashValue(swift::DeclBaseName Val) {
    return DenseMapInfo<const void *>::getHashValue(Val.getAsOpaquePointer());
  }
  static bool isEqual(swift::DeclBaseName LHS, swift::DeclBaseName RHS) {
    return LHS == RHS;
  }
};

// A DeclBaseName is "pointer like".
template <typename T> struct PointerLikeTypeTraits;
template <> struct PointerLikeTypeTraits<swift::DeclBaseName> {
public:
  static inline void *getAsVoidPointer(swift::DeclBaseName D) {
    return const_cast<void *>(D.getAsOpaquePointer());
  }
  static inline swift::DeclBaseName getFromVoidPointer(void *P) {
    return swift::DeclBaseName::getFromOpaquePointer(P);
  }
  enum { NumLowBitsAvailable = PointerLikeTypeTraits<swift::Identifier>::NumLowBitsAvailable };
};

} // end namespace llvm

namespace swift {

/// A declaration name, which may comprise one or more identifier pieces.
class DeclName {
  friend class ASTContext;

  /// Represents a compound declaration name.
  struct alignas(Identifier) CompoundDeclName final : llvm::FoldingSetNode,
      private llvm::TrailingObjects<CompoundDeclName, Identifier> {
    friend TrailingObjects;
    friend class DeclName;

    DeclBaseName BaseName;
    size_t NumArgs;

    explicit CompoundDeclName(DeclBaseName BaseName, size_t NumArgs)
        : BaseName(BaseName), NumArgs(NumArgs) { }
    
    ArrayRef<Identifier> getArgumentNames() const {
      return {getTrailingObjects<Identifier>(), NumArgs};
    }
    MutableArrayRef<Identifier> getArgumentNames() {
      return {getTrailingObjects<Identifier>(), NumArgs};
    }

    /// Uniquing for the ASTContext.
    static void Profile(llvm::FoldingSetNodeID &id, DeclBaseName baseName,
                        ArrayRef<Identifier> argumentNames);

    void Profile(llvm::FoldingSetNodeID &id) {
      Profile(id, BaseName, getArgumentNames());
    }
  };

  /// Either a single identifier piece stored inline, or a reference to a
  /// compound declaration name.
  llvm::PointerUnion<DeclBaseName, CompoundDeclName *> BaseNameOrCompound;

  explicit DeclName(void *Opaque)
    : BaseNameOrCompound(decltype(BaseNameOrCompound)::getFromOpaqueValue(Opaque))
  {}

  void initialize(ASTContext &C, DeclBaseName baseName,
                  ArrayRef<Identifier> argumentNames);

public:
  /// Build a null name.
  DeclName() : BaseNameOrCompound(DeclBaseName()) {}

  /// Build a simple value name with one component.
  /*implicit*/ DeclName(DeclBaseName simpleName)
      : BaseNameOrCompound(simpleName) {}

  /*implicit*/ DeclName(Identifier simpleName)
      : DeclName(DeclBaseName(simpleName)) {}

  /// Build a compound value name given a base name and a set of argument names.
  DeclName(ASTContext &C, DeclBaseName baseName,
           ArrayRef<Identifier> argumentNames) {
    initialize(C, baseName, argumentNames);
  }

  /// Build a compound value name given a base name and a set of argument names
  /// extracted from a parameter list.
  DeclName(ASTContext &C, DeclBaseName baseName, ParameterList *paramList);

  /// Retrieve the 'base' name, i.e., the name that follows the introducer,
  /// such as the 'foo' in 'func foo(x:Int, y:Int)' or the 'bar' in
  /// 'var bar: Int'.
  DeclBaseName getBaseName() const {
    if (auto compound = BaseNameOrCompound.dyn_cast<CompoundDeclName*>())
      return compound->BaseName;

    return BaseNameOrCompound.get<DeclBaseName>();
  }

  /// Assert that the base name is not special and return its identifier.
  Identifier getBaseIdentifier() const {
    auto baseName = getBaseName();
    assert(!baseName.isSpecial() &&
           "Can't retrieve the identifier of a special base name");
    return baseName.getIdentifier();
  }

  /// Retrieve the names of the arguments, if there are any.
  ArrayRef<Identifier> getArgumentNames() const {
    if (auto compound = BaseNameOrCompound.dyn_cast<CompoundDeclName*>())
      return compound->getArgumentNames();

    return { };
  }

  bool isSpecial() const { return getBaseName().isSpecial(); }

  explicit operator bool() const {
    if (BaseNameOrCompound.dyn_cast<CompoundDeclName*>())
      return true;
    return !BaseNameOrCompound.get<DeclBaseName>().empty();
  }
  
  /// True if this is a simple one-component name.
  bool isSimpleName() const {
    return BaseNameOrCompound.is<DeclBaseName>();
  }

  /// True if this is a compound name.
  bool isCompoundName() const {
    return !isSimpleName();
  }
  
  /// True if this name is a simple one-component name identical to the
  /// given identifier.
  bool isSimpleName(DeclBaseName name) const {
    return isSimpleName() && getBaseName() == name;
  }
  
  /// True if this name is a simple one-component name equal to the
  /// given string.
  bool isSimpleName(StringRef name) const {
    return isSimpleName() && getBaseName() == name;
  }

  /// True if this name is a compound name equal to the given base name and
  /// argument names.
  bool isCompoundName(DeclBaseName base, ArrayRef<StringRef> args) const;

  /// True if this name is a compound name equal to the given normal
  /// base name and argument names.
  bool isCompoundName(StringRef base, ArrayRef<StringRef> args) const;
  
  /// True if this name is an operator.
  bool isOperator() const {
    return getBaseName().isOperator();
  }
  
  /// True if this name should be found by a decl ref or member ref under the
  /// name specified by 'refName'.
  ///
  /// We currently match compound names either when their first component
  /// matches a simple name lookup or when the full compound name matches.
  bool matchesRef(DeclName refName) const {
    // Identical names always match.
    if (BaseNameOrCompound == refName.BaseNameOrCompound)
      return true;
    // If the reference is a simple name, try simple name matching.
    if (refName.isSimpleName())
      return refName.getBaseName() == getBaseName();
    // The names don't match.
    return false;
  }

  /// Add a DeclName to a lookup table so that it can be found by its simple
  /// name or its compound name.
  template<typename LookupTable, typename Element>
  void addToLookupTable(LookupTable &table, const Element &elt) {
    table[*this].push_back(elt);
    if (!isSimpleName()) {
      table[getBaseName()].push_back(elt);
    }
  }

  /// Compare two declaration names, producing -1 if \c *this comes before
  /// \c other,  1 if \c *this comes after \c other, and 0 if they are equal.
  ///
  /// Null declaration names come after all other declaration names.
  int compare(DeclName other) const;

  friend bool operator==(DeclName lhs, DeclName rhs) {
    return lhs.getOpaqueValue() == rhs.getOpaqueValue();
  }

  friend bool operator!=(DeclName lhs, DeclName rhs) {
    return !(lhs == rhs);
  }

  friend llvm::hash_code hash_value(DeclName name) {
    using llvm::hash_value;
    return hash_value(name.getOpaqueValue());
  }

  friend bool operator<(DeclName lhs, DeclName rhs) {
    return lhs.compare(rhs) < 0;
  }

  friend bool operator<=(DeclName lhs, DeclName rhs) {
    return lhs.compare(rhs) <= 0;
  }

  friend bool operator>(DeclName lhs, DeclName rhs) {
    return lhs.compare(rhs) > 0;
  }

  friend bool operator>=(DeclName lhs, DeclName rhs) {
    return lhs.compare(rhs) >= 0;
  }

  void *getOpaqueValue() const { return BaseNameOrCompound.getOpaqueValue(); }
  static DeclName getFromOpaqueValue(void *p) { return DeclName(p); }

  /// Get a string representation of the name,
  ///
  /// \param scratch Scratch space to use.
  StringRef getString(llvm::SmallVectorImpl<char> &scratch,
                      bool skipEmptyArgumentNames = false) const;

  /// Print the representation of this declaration name to the given
  /// stream.
  ///
  /// \param skipEmptyArgumentNames When true, don't print the argument labels
  /// if they are all empty.
  llvm::raw_ostream &print(llvm::raw_ostream &os,
                           bool skipEmptyArgumentNames = false) const;

  /// Print a "pretty" representation of this declaration name to the given
  /// stream.
  ///
  /// This is the name used for diagnostics; it is not necessarily the
  /// fully-specified name that would be written in the source.
  llvm::raw_ostream &printPretty(llvm::raw_ostream &os) const;

  /// Dump this name to standard error.
  SWIFT_DEBUG_DUMP;
};

void simple_display(llvm::raw_ostream &out, DeclName name);

/// An in-source reference to another declaration, including qualification
/// information.
class DeclNameRef {
  friend class ASTContext;

  /// Contains the name and module for a DeclNameRef with a module selector.
  struct alignas(Identifier) SelectiveDeclNameRef : llvm::FoldingSetNode {
    Identifier moduleSelector;   // Note: currently can never be empty().
    DeclName fullName;

    SelectiveDeclNameRef(Identifier moduleSelector, DeclName fullName)
      : moduleSelector(moduleSelector), fullName(fullName) { }

    /// Uniquing for the ASTContext.
    static void Profile(llvm::FoldingSetNodeID &id, Identifier moduleSelector,
                        DeclName fullName);

    void Profile(llvm::FoldingSetNodeID &id) {
      Profile(id, moduleSelector, fullName);
    }

    // Make vanilla new/delete illegal for SelectiveDeclNameRef.
    void *operator new(size_t Bytes) = delete;
    void operator delete(void *Data) = delete;

    // Only allow allocation of SelectiveDeclNameRef using the allocator
    // in ASTContext or by doing a placement new.
    void *operator new(size_t Bytes, const ASTContext &C,
                       unsigned Alignment = alignof(SelectiveDeclNameRef));
    void *operator new(size_t Bytes, void *Mem) {
      assert(Mem);
      return Mem;
    }
  };

  llvm::PointerUnion<DeclName, SelectiveDeclNameRef *> storage;

  explicit DeclNameRef(void *Opaque)
    : storage(decltype(storage)::getFromOpaqueValue(Opaque)) { }

  void initialize(ASTContext &C, Identifier moduleScope, DeclName fullName);

public:
  static DeclNameRef createSubscript();
  static DeclNameRef createConstructor();

  DeclNameRef() : storage(DeclName()) { }

  void *getOpaqueValue() const {
    return storage.getOpaqueValue();
  }
  static DeclNameRef getFromOpaqueValue(void *p);

  explicit DeclNameRef(ASTContext &C, Identifier moduleSelector,
                       DeclName fullName) {
    initialize(C, moduleSelector, fullName);
  }

  explicit DeclNameRef(ASTContext &C, Identifier moduleSelector,
                       DeclBaseName baseName, ArrayRef<Identifier> argLabels) {
    initialize(C, moduleSelector, DeclName(C, baseName, argLabels));
  }

  explicit DeclNameRef(DeclName FullName)
    : storage(FullName) { }

  bool hasModuleSelector() const {
    return storage.is<SelectiveDeclNameRef *>();
  }

  Identifier getModuleSelector() const {
    if (!hasModuleSelector())
      return Identifier();

    return storage.get<SelectiveDeclNameRef *>()->moduleSelector;
  }

  /// The name of the declaration being referenced.
  DeclName getFullName() const {
    if (!hasModuleSelector())
      return storage.get<DeclName>();

    return storage.get<SelectiveDeclNameRef *>()->fullName;
  }

  /// The base name of the declaration being referenced.
  DeclBaseName getBaseName() const {
    return getFullName().getBaseName();
  }

  Identifier getBaseIdentifier() const {
    return getFullName().getBaseIdentifier();
  }

  ArrayRef<Identifier> getArgumentNames() const {
    return getFullName().getArgumentNames();
  }

  bool isSimpleName() const {
    return getFullName().isSimpleName();
  }

  bool isSimpleName(DeclBaseName name) const {
    return getFullName().isSimpleName(name);
  }

  bool isSimpleName(StringRef name) const {
    return getFullName().isSimpleName(name);
  }

  bool isSpecial() const {
    return getFullName().isSpecial();
  }

  bool isOperator() const {
    return getFullName().isOperator();
  }

  bool isCompoundName() const {
    return getFullName().isCompoundName();
  }

  explicit operator bool() const {
    return (bool)getFullName();
  }

  /// Compare two declaration names, producing -1 if \c *this comes before
  /// \c other,  1 if \c *this comes after \c other, and 0 if they are equal.
  ///
  /// Null declaration names come after all other declaration names.
  int compare(DeclNameRef other) const {
    return getFullName().compare(other.getFullName());
  }

  friend bool operator==(DeclNameRef lhs, DeclNameRef rhs) {
    return lhs.getOpaqueValue() == rhs.getOpaqueValue();
  }

  friend bool operator!=(DeclNameRef lhs, DeclNameRef rhs) {
    return !(lhs == rhs);
  }

  friend llvm::hash_code hash_value(DeclNameRef name) {
    using llvm::hash_value;
    return hash_value(name.getFullName().getOpaqueValue());
  }

  friend bool operator<(DeclNameRef lhs, DeclNameRef rhs) {
    return lhs.compare(rhs) < 0;
  }

  friend bool operator<=(DeclNameRef lhs, DeclNameRef rhs) {
    return lhs.compare(rhs) <= 0;
  }

  friend bool operator>(DeclNameRef lhs, DeclNameRef rhs) {
    return lhs.compare(rhs) > 0;
  }

  friend bool operator>=(DeclNameRef lhs, DeclNameRef rhs) {
    return lhs.compare(rhs) >= 0;
  }

  DeclNameRef withoutArgumentLabels(ASTContext &C) const;
  DeclNameRef withArgumentLabels(ASTContext &C,
                                 ArrayRef<Identifier> argumentNames) const;

  /// Get a string representation of the name,
  ///
  /// \param scratch Scratch space to use.
  StringRef getString(llvm::SmallVectorImpl<char> &scratch,
                      bool skipEmptyArgumentNames = false) const;

  /// Print the representation of this declaration name to the given
  /// stream.
  ///
  /// \param skipEmptyArgumentNames When true, don't print the argument labels
  /// if they are all empty.
  llvm::raw_ostream &print(llvm::raw_ostream &os,
                           bool skipEmptyArgumentNames = false) const;

  /// Print a "pretty" representation of this declaration name to the given
  /// stream.
  ///
  /// This is the name used for diagnostics; it is not necessarily the
  /// fully-specified name that would be written in the source.
  llvm::raw_ostream &printPretty(llvm::raw_ostream &os) const;

  /// Dump this name to standard error.
  LLVM_ATTRIBUTE_DEPRECATED(void dump() const LLVM_ATTRIBUTE_USED,
                            "only for use within the debugger");
};

inline DeclNameRef DeclNameRef::getFromOpaqueValue(void *p) {
  return DeclNameRef(p);
}

inline DeclNameRef DeclNameRef::withoutArgumentLabels(ASTContext &C) const {
  return DeclNameRef(C, getModuleSelector(), getBaseName());
}

inline DeclNameRef DeclNameRef::withArgumentLabels(
    ASTContext &C, ArrayRef<Identifier> argumentNames) const {
  return DeclNameRef(C, getModuleSelector(), getBaseName(), argumentNames);
}

inline DeclNameRef DeclNameRef::createSubscript() {
  return DeclNameRef(DeclBaseName::createSubscript());
}

inline DeclNameRef DeclNameRef::createConstructor() {
  return DeclNameRef(DeclBaseName::createConstructor());
}

void simple_display(llvm::raw_ostream &out, DeclNameRef name);

enum class ObjCSelectorFamily : unsigned {
  None,
#define OBJC_SELECTOR_FAMILY(LABEL, PREFIX) LABEL,
#include "swift/AST/ObjCSelectorFamily.def"
};

/// Represents an Objective-C selector.
class ObjCSelector {
  /// The storage for an Objective-C selector.
  ///
  /// A zero-argument selector is represented as simple name.
  /// A selector with N arguments is represented as a compound name with
  /// N arguments, where the simple name is a placeholder.
  DeclName Storage;

  explicit ObjCSelector(DeclName storage) : Storage(storage) { }

  friend struct llvm::DenseMapInfo<ObjCSelector>;

public:
  /// Form a selector with the given number of arguments and the given selector
  /// pieces.
  ObjCSelector(ASTContext &ctx, unsigned numArgs, ArrayRef<Identifier> pieces);

  /// Construct an invalid ObjCSelector.
  ObjCSelector() : Storage() {}

  /// Convert to true if the decl name is valid.
  explicit operator bool() const { return (bool)Storage; }

  /// Determine the number of arguments in the selector.
  ///
  /// When this is zero, the number of selector pieces will be one. Otherwise,
  /// it equals the number of selector pieces.
  unsigned getNumArgs() const {
    if (Storage.isSimpleName()) {
      return 0;
    }

    return Storage.getArgumentNames().size();
  }

  /// Determine the number of selector pieces in the selector.
  ///
  /// When this is one, the number of arguments may either be zero or one.
  /// Otherwise, it equals the number of arguments.
  unsigned getNumSelectorPieces() const {
    return getSelectorPieces().size();
  }

  /// Retrieve the pieces in this selector.
  ArrayRef<Identifier> getSelectorPieces() const {
    if (Storage.isSimpleName()) {
      return { reinterpret_cast<const Identifier*>(&Storage), 1 };
    }

    return Storage.getArgumentNames();
  }

  /// Asserts that this is a nullary selector and returns the single identifier.
  Identifier getSimpleName() const {
    assert(Storage.isSimpleName() && "not a nullary selector");
    return Storage.getBaseIdentifier();
  }

  /// Get a string representation of the selector.
  ///
  /// \param scratch Scratch space to use.
  StringRef getString(llvm::SmallVectorImpl<char> &scratch) const;

  ObjCSelectorFamily getSelectorFamily() const;

  void *getOpaqueValue() const { return Storage.getOpaqueValue(); }
  static ObjCSelector getFromOpaqueValue(void *p) {
    return ObjCSelector(DeclName::getFromOpaqueValue(p));
  }

  /// Dump this selector to standard error.
  SWIFT_DEBUG_DUMP;

  /// Compare two Objective-C selectors, producing -1 if \c *this comes before
  /// \c other,  1 if \c *this comes after \c other, and 0 if they are equal.
  int compare(ObjCSelector other) const {
    return Storage.compare(other.Storage);
  }

  friend bool operator==(ObjCSelector lhs, ObjCSelector rhs) {
    return lhs.getOpaqueValue() == rhs.getOpaqueValue();
  }

  friend bool operator!=(ObjCSelector lhs, ObjCSelector rhs) {
    return !(lhs == rhs);
  }

  friend bool operator<(ObjCSelector lhs, ObjCSelector rhs) {
    return lhs.compare(rhs) < 0;
  }

  friend bool operator<=(ObjCSelector lhs, ObjCSelector rhs) {
    return lhs.compare(rhs) <= 0;
  }

  friend bool operator>(ObjCSelector lhs, ObjCSelector rhs) {
    return lhs.compare(rhs) > 0;
  }

  friend bool operator>=(ObjCSelector lhs, ObjCSelector rhs) {
    return lhs.compare(rhs) >= 0;
  }
};

} // end namespace swift

namespace llvm {
  // A DeclName is "pointer like".
  template<typename T> struct PointerLikeTypeTraits;
  template<>
  struct PointerLikeTypeTraits<swift::DeclName> {
  public:
    static inline void *getAsVoidPointer(swift::DeclName name) {
      return name.getOpaqueValue();
    }
    static inline swift::DeclName getFromVoidPointer(void *ptr) {
      return swift::DeclName::getFromOpaqueValue(ptr);
    }
    enum { NumLowBitsAvailable = PointerLikeTypeTraits<swift::DeclBaseName>::NumLowBitsAvailable - 1 };
  };

  // DeclNames hash just like pointers.
  template<> struct DenseMapInfo<swift::DeclName> {
    static swift::DeclName getEmptyKey() {
      return swift::Identifier::getEmptyKey();
    }
    static swift::DeclName getTombstoneKey() {
      return swift::Identifier::getTombstoneKey();
    }
    static unsigned getHashValue(swift::DeclName Val) {
      return DenseMapInfo<void*>::getHashValue(Val.getOpaqueValue());
    }
    static bool isEqual(swift::DeclName LHS, swift::DeclName RHS) {
      return LHS.getOpaqueValue() == RHS.getOpaqueValue();
    }
  };

  // A DeclNameRef is "pointer like" just like DeclNames.
  template<typename T> struct PointerLikeTypeTraits;
  template<>
  struct PointerLikeTypeTraits<swift::DeclNameRef> {
  public:
    static inline void *getAsVoidPointer(swift::DeclNameRef name) {
      return name.getOpaqueValue();
    }
    static inline swift::DeclNameRef getFromVoidPointer(void *ptr) {
      return swift::DeclNameRef::getFromOpaqueValue(ptr);
    }
    enum { NumLowBitsAvailable = PointerLikeTypeTraits<swift::DeclName>::NumLowBitsAvailable - 1 };
  };

  // DeclNameRefs hash just like DeclNames.
  template<> struct DenseMapInfo<swift::DeclNameRef> {
    static swift::DeclNameRef getEmptyKey() {
      return swift::DeclNameRef(DenseMapInfo<swift::DeclName>::getEmptyKey());
    }
    static swift::DeclNameRef getTombstoneKey() {
      return swift::DeclNameRef(DenseMapInfo<swift::DeclName>::getTombstoneKey());
    }
    static unsigned getHashValue(swift::DeclNameRef Val) {
      return DenseMapInfo<swift::DeclName>::getHashValue(Val.getFullName());
    }
    static bool isEqual(swift::DeclNameRef LHS, swift::DeclNameRef RHS) {
      return DenseMapInfo<swift::DeclName>::isEqual(LHS.getFullName(),
                                                    RHS.getFullName());
    }
  };

  // An ObjCSelector is "pointer like".
  template<typename T> struct PointerLikeTypeTraits;
  template<>
  struct PointerLikeTypeTraits<swift::ObjCSelector> {
  public:
    static inline void *getAsVoidPointer(swift::ObjCSelector name) {
      return name.getOpaqueValue();
    }
    static inline swift::ObjCSelector getFromVoidPointer(void *ptr) {
      return swift::ObjCSelector::getFromOpaqueValue(ptr);
    }
    enum { NumLowBitsAvailable = 0 };
  };

  // ObjCSelectors hash just like pointers.
  template<> struct DenseMapInfo<swift::ObjCSelector> {
    static swift::ObjCSelector getEmptyKey() {
      return swift::ObjCSelector(DenseMapInfo<swift::DeclName>::getEmptyKey());
    }
    static swift::ObjCSelector getTombstoneKey() {
      return swift::ObjCSelector(
               DenseMapInfo<swift::DeclName>::getTombstoneKey());
    }
    static unsigned getHashValue(swift::ObjCSelector Val) {
      return DenseMapInfo<void*>::getHashValue(Val.getOpaqueValue());
    }
    static bool isEqual(swift::ObjCSelector LHS, swift::ObjCSelector RHS) {
      return LHS.getOpaqueValue() == RHS.getOpaqueValue();
    }
  };
} // end namespace llvm

#endif
