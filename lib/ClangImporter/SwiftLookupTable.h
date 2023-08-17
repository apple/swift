//===--- SwiftLookupTable.h - Swift Lookup Table ----------------*- C++ -*-===//
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
// This file implements support for Swift name lookup tables stored in Clang
// modules.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_CLANGIMPORTER_SWIFTLOOKUPTABLE_H
#define SWIFT_CLANGIMPORTER_SWIFTLOOKUPTABLE_H

#include "swift/Basic/Debug.h"
#include "swift/Basic/LLVM.h"
#include "swift/AST/Identifier.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclObjC.h"
#include "clang/Serialization/ASTBitCodes.h"
#include "clang/Serialization/ModuleFileExtension.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/Support/Compiler.h"
#include <functional>
#include <utility>

namespace llvm {
class BitstreamWriter;
}

namespace clang {
class NamedDecl;
class DeclContext;
class MacroInfo;
class ModuleMacro;
class ObjCCategoryDecl;
class TypedefNameDecl;
}

namespace swift {

/// A name from a SwiftLookupTable that has not been deserialized into a
/// DeclBaseName yet.
struct SerializedSwiftName {
  /// The kind of the name if it is a special name
  DeclBaseName::Kind Kind;
  /// The name of the identifier if it is not a special name
  StringRef Name;

  SerializedSwiftName() : Kind(DeclBaseName::Kind::Normal), Name(StringRef()) {}

  explicit SerializedSwiftName(DeclBaseName::Kind Kind)
      : Kind(Kind), Name(StringRef()) {}

  explicit SerializedSwiftName(StringRef Name)
      : Kind(DeclBaseName::Kind::Normal), Name(Name) {}

  SerializedSwiftName(DeclBaseName BaseName) {
    Kind = BaseName.getKind();
    if (BaseName.getKind() == DeclBaseName::Kind::Normal) {
      Name = BaseName.getIdentifier().str();
    }
  }

  /// Deserialize the name, adding it to the context's identifier table
  DeclBaseName toDeclBaseName(ASTContext &Context) const;

  bool empty() const {
    return Kind == DeclBaseName::Kind::Normal && Name.empty();
  }

  /// Return a string representation of the name that can be used for sorting
  StringRef userFacingName() const {
    switch (Kind) {
    case DeclBaseName::Kind::Normal:
      return Name;
    case DeclBaseName::Kind::Subscript:
      return "subscript";
    case DeclBaseName::Kind::Constructor:
      return "init";
    case DeclBaseName::Kind::Destructor:
      return "deinit";
    }
    llvm_unreachable("unhandled kind");
  }

  bool operator<(SerializedSwiftName RHS) const {
    return userFacingName() < RHS.userFacingName();
  }

  bool operator==(SerializedSwiftName RHS) const {
    if (Kind != RHS.Kind)
      return false;

    if (Kind == DeclBaseName::Kind::Normal) {
      assert(RHS.Kind == DeclBaseName::Kind::Normal);
      return Name == RHS.Name;
    } else {
      return true;
    }
  }
};

} // end namespace swift

namespace llvm {

using swift::SerializedSwiftName;

// Inherit the DenseMapInfo from StringRef but add a few special cases for
// special names
template<> struct DenseMapInfo<SerializedSwiftName> {
  static SerializedSwiftName getEmptyKey() {
    return SerializedSwiftName(DenseMapInfo<StringRef>::getEmptyKey());
  }
  static SerializedSwiftName getTombstoneKey() {
    return SerializedSwiftName(DenseMapInfo<StringRef>::getTombstoneKey());
  }
  static unsigned getHashValue(SerializedSwiftName Val) {
    if (Val.Kind == swift::DeclBaseName::Kind::Normal) {
      return DenseMapInfo<StringRef>::getHashValue(Val.Name);
    } else {
      return (unsigned)Val.Kind;
    }
  }
  static bool isEqual(SerializedSwiftName LHS, SerializedSwiftName RHS) {
    if (LHS.Kind != RHS.Kind)
      return false;

    if (LHS.Kind == swift::DeclBaseName::Kind::Normal) {
      assert(RHS.Kind == swift::DeclBaseName::Kind::Normal);
      return DenseMapInfo<StringRef>::isEqual(LHS.Name, RHS.Name);
    } else {
      return LHS.Kind == RHS.Kind;
    }
  }
};

} // end namespace llvm

namespace swift {

/// The context into which a Clang declaration will be imported.
///
/// When the context into which a declaration will be imported matches
/// a Clang declaration context (the common case), the result will be
/// expressed as a declaration context. Otherwise, if the Clang type
/// is not itself a declaration context (for example, a typedef that
/// comes into Swift as a strong type), the Clang type declaration
/// will be provided. Finally, if the context is known only via its
/// Swift name, this will be recorded as
class EffectiveClangContext {
public:
  enum Kind : uint8_t {
    DeclContext,
    TypedefContext,
    UnresolvedContext, // must be last
  };

private:
  union {
    const clang::DeclContext *DC;
    const clang::TypedefNameDecl *Typedef;
    struct {
      const char *Data;
    } Unresolved;
  };

  /// If KindOrBiasedLength < Kind::UnresolvedContext, this represents a Kind.
  /// Otherwise it's (uintptr_t)Kind::UnresolvedContext plus the length of
  /// Unresolved.Data.
  uintptr_t KindOrBiasedLength;

public:
  EffectiveClangContext() : KindOrBiasedLength(DeclContext) {
    DC = nullptr;
  }

  EffectiveClangContext(const clang::DeclContext *dc)
      : KindOrBiasedLength(DeclContext) {
    assert(dc != nullptr && "use null constructor instead");

    // Skip over any linkage spec decl contexts
    while (auto externCDecl = dyn_cast<clang::LinkageSpecDecl>(dc)) {
      dc = externCDecl->getLexicalDeclContext();
    }

    if (auto tagDecl = dyn_cast<clang::TagDecl>(dc)) {
      DC = tagDecl->getCanonicalDecl();
    } else if (auto oiDecl = dyn_cast<clang::ObjCInterfaceDecl>(dc)) {
      DC = oiDecl->getCanonicalDecl();
    } else if (auto opDecl = dyn_cast<clang::ObjCProtocolDecl>(dc)) {
      DC = opDecl->getCanonicalDecl();
    } else if (auto omDecl = dyn_cast<clang::ObjCMethodDecl>(dc)) {
      DC = omDecl->getCanonicalDecl();
    } else if (auto fDecl = dyn_cast<clang::FunctionDecl>(dc)) {
      DC = fDecl->getCanonicalDecl();
    } else {
      assert(isa<clang::TranslationUnitDecl>(dc) ||
             isa<clang::NamespaceDecl>(dc) ||
             isa<clang::ObjCContainerDecl>(dc) &&
                 "No other kinds of effective Clang contexts");
      DC = dc;
    }
  }

  EffectiveClangContext(const clang::TypedefNameDecl *typedefName)
    : KindOrBiasedLength(TypedefContext) {
    Typedef = typedefName->getCanonicalDecl();
  }

  EffectiveClangContext(StringRef unresolved)
      : KindOrBiasedLength(UnresolvedContext + unresolved.size()) {
    Unresolved.Data = unresolved.data();
  }

  /// Determine whether this effective Clang context was set.
  explicit operator bool() const {
    return getKind() != DeclContext || DC != nullptr;
  }

  /// Determine the kind of effective Clang context.
  Kind getKind() const {
    if (KindOrBiasedLength >= UnresolvedContext)
      return UnresolvedContext;
    return static_cast<Kind>(KindOrBiasedLength);

  }

  /// Retrieve the declaration context.
  const clang::DeclContext *getAsDeclContext() const {
    return getKind() == DeclContext ? DC : nullptr;
  }

  /// Retrieve the typedef declaration.
  const clang::TypedefNameDecl *getTypedefName() const {
    assert(getKind() == TypedefContext);
    return Typedef;
  }

  /// Retrieve the unresolved context name.
  StringRef getUnresolvedName() const {
    assert(getKind() == UnresolvedContext);
    return StringRef(Unresolved.Data, KindOrBiasedLength - UnresolvedContext);
  }

  /// Compares two EffectiveClangContexts without resolving unresolved names.
  bool equalsWithoutResolving(const EffectiveClangContext &other) const {
    if (getKind() != other.getKind())
      return false;
    switch (getKind()) {
    case DeclContext:
      return DC == other.DC;
    case TypedefContext:
      return Typedef == other.Typedef;
    case UnresolvedContext:
      return getUnresolvedName() == other.getUnresolvedName();
    }
    llvm_unreachable("unhandled kind");
  }
};

static_assert(sizeof(EffectiveClangContext) <= 2 * sizeof(void *),
              "should be small");

class SwiftLookupTableReader;
class SwiftLookupTableWriter;

/// Lookup table major version number.
///
const uint16_t SWIFT_LOOKUP_TABLE_VERSION_MAJOR = 1;

/// Lookup table minor version number.
///
/// When the format changes IN ANY WAY, this number should be incremented.
const uint16_t SWIFT_LOOKUP_TABLE_VERSION_MINOR = 18; // Unsafe C++ method renaming.


/// A lookup table that maps Swift names to the set of Clang
/// declarations with that particular name.
///
/// The names of C entities can undergo significant transformations
/// when they are mapped into Swift, which makes Clang's name lookup
/// mechanisms useless when searching for the Swift name of
/// entities. This lookup table provides efficient access to the C
/// entities based on their Swift names, and is used by the Clang
/// importer to satisfy the Swift compiler's queries.
class SwiftLookupTable {
public:
  /// The kind of context in which a name occurs.
  ///
  /// Note that values 0 and 1 are reserved for empty and tombstone
  /// keys.
  enum class ContextKind : uint8_t {
    /// A translation unit.
    TranslationUnit = 2,
    /// A tag declaration (struct, enum, union, C++ class).
    Tag,
    /// An Objective-C class.
    ObjCClass,
    /// An Objective-C protocol.
    ObjCProtocol,
    /// A typedef that produces a strong type in Swift.
    Typedef,
  };

  /// Determine whether the given context requires a name to disambiguate.
  static bool contextRequiresName(ContextKind kind);

  /// A single entry referencing either a named declaration or a macro.
  typedef llvm::PointerUnion<clang::NamedDecl *, clang::MacroInfo *,
                             clang::ModuleMacro *>
    SingleEntry;

  /// A stored version of the context of an entity, which is Clang
  /// ASTContext-independent.
  typedef std::pair<ContextKind, StringRef> StoredContext;

  /// An entry in the table of C entities indexed by full Swift name.
  struct FullTableEntry {
    /// The context in which the entities with the given name occur, e.g.,
    /// a class, struct, translation unit, etc.
    /// is always the canonical DeclContext for the entity.
    StoredContext Context;

    /// The set of Clang declarations and macros with this name and in
    /// this context.
    ///
    /// The low bit indicates whether we have a declaration or macro
    /// (declaration = unset, macro = set) and the second lowest bit
    /// indicates whether we have a serialization ID (set = DeclID or
    /// {IdentifierID,SubmoduleID}, as appropriate) vs. a pointer (unset,
    /// clang::NamedDecl *, clang::MacroInfo *, clang::ModuleMacro *).
    /// In the ID case, the upper N-2 bits are the ID value; in the pointer
    /// case, the lower two bits will always be clear due to the alignment of
    /// the Clang pointers.
    llvm::SmallVector<uint64_t, 2> DeclsOrMacros;
  };

  /// Whether the given entry is a macro entry.
  static bool isMacroEntry(uint64_t entry) { return entry & 0x01; }

  /// Whether the given entry is a declaration entry.
  static bool isDeclEntry(uint64_t entry) { return !isMacroEntry(entry); }

  /// Whether the given entry is a serialization ID.
  static bool isSerializationIDEntry(uint64_t entry) { return (entry & 0x02); }

  /// Whether the given entry is an AST node.
  static bool isASTNodeEntry(uint64_t entry) {
    return !isSerializationIDEntry(entry);
  }

  /// Retrieve the pointer for an entry.
  static void *getPointerFromEntry(uint64_t entry) {
    assert(isASTNodeEntry(entry) && "Not an AST node entry");
    const uint64_t mask = ~static_cast<uint64_t>(0x03);
    return reinterpret_cast<void *>(entry & mask);
  }

  /// Encode a Clang named declaration as an entry in the table.
  static uint64_t encodeEntry(clang::NamedDecl *decl) {
    assert(decl);
    auto bits = reinterpret_cast<uintptr_t>(decl);
    assert((bits & 0x03) == 0 && "low bits set?");
    return bits;
  }

  // Encode a Clang macro as an entry in the table.
  static uint64_t encodeEntry(clang::MacroInfo *macro) {
    assert(macro);
    auto bits = reinterpret_cast<uintptr_t>(macro);
    assert((bits & 0x03) == 0 && "low bits set?");
    return bits | 0x01;
  }

  // Encode a Clang macro as an entry in the table.
  static uint64_t encodeEntry(clang::ModuleMacro *macro) {
    assert(macro);
    auto bits = reinterpret_cast<uintptr_t>(macro);
    assert((bits & 0x03) == 0 && "low bits set?");
    return bits | 0x01;
  }

private:
  using TableType =
      llvm::DenseMap<SerializedSwiftName, SmallVector<FullTableEntry, 2>>;
  using CacheCallback = void(SmallVectorImpl<FullTableEntry> &,
                             SwiftLookupTableReader &,
                             SerializedSwiftName);

  /// A table mapping from the base name of Swift entities to all of
  /// the C entities that have that name, in all contexts.
  TableType LookupTable;

  /// A table mapping the base names of Swift entities to all of the C entities
  /// that are remapped to that name by the globals-as-members utility, in all contexts.
  TableType GlobalsAsMembers;

  /// The list of Objective-C categories and extensions.
  llvm::SmallVector<clang::ObjCCategoryDecl *, 4> Categories;

  /// A mapping from stored contexts to the set of global declarations that
  /// are mapped to members within that context.
  ///
  /// The values use the same representation as
  /// FullTableEntry::DeclsOrMacros.
  llvm::DenseMap<StoredContext, SmallVector<uint64_t, 2>> GlobalsAsMembersIndex;

  /// The reader responsible for lazily loading the contents of this table.
  SwiftLookupTableReader *Reader;

  /// Entries whose effective contexts could not be resolved, and
  /// therefore will need to be added later.
  SmallVector<std::tuple<DeclName, SingleEntry, EffectiveClangContext>, 4>
    UnresolvedEntries;

  friend class SwiftLookupTableReader;
  friend class SwiftLookupTableWriter;

  /// Find or create the table entry for the given base name.
  llvm::DenseMap<SerializedSwiftName, SmallVector<FullTableEntry, 2>>::iterator
  findOrCreate(TableType &table,
               SerializedSwiftName baseName,
               llvm::function_ref<CacheCallback> create);

  /// Add the given entry to the list of entries, if it's not already
  /// present.
  ///
  /// \returns true if the entry was added, false otherwise.
  bool addLocalEntry(SingleEntry newEntry, SmallVectorImpl<uint64_t> &entries);

public:
  explicit SwiftLookupTable(SwiftLookupTableReader *reader) : Reader(reader) { }

  /// Maps a stored declaration entry to an actual Clang declaration.
  clang::NamedDecl *mapStoredDecl(uint64_t &entry);

  /// Maps a stored macro entry to an actual Clang macro.
  SingleEntry mapStoredMacro(uint64_t &entry, bool assumeModule = false);

  /// Maps a stored entry to an actual Clang AST node.
  SingleEntry mapStored(uint64_t &entry, bool assumeModule = false);

  /// Translate a Clang DeclContext into a context kind and name.
  static llvm::Optional<StoredContext> translateDeclContext(
                                         const clang::DeclContext *dc);

  /// Translate a Clang effective context into a context kind and name.
  llvm::Optional<StoredContext> translateContext(EffectiveClangContext context);

  /// Add an entry to the lookup table.
  ///
  /// \param name The Swift name of the entry.
  /// \param newEntry The Clang declaration or macro.
  /// \param effectiveContext The effective context in which name lookup occurs.
  void addEntry(DeclName name, SingleEntry newEntry,
                EffectiveClangContext effectiveContext);

  /// Add an Objective-C category or extension to the table.
  void addCategory(clang::ObjCCategoryDecl *category);

  /// Resolve any unresolved entries.
  ///
  /// \param unresolved Will be populated with the list of entries
  /// that could not be resolved.
  ///
  /// \returns true if any remaining entries could not be resolved,
  /// and false otherwise.
  bool resolveUnresolvedEntries(SmallVectorImpl<SingleEntry> &unresolved);

private:
  /// Lookup the set of entities with the given base name.
  ///
  /// \param baseName The base name to search for. All results will
  /// have this base name.
  ///
  /// \param searchContext The context in which the resulting set of
  /// entities should reside. This may be None to indicate that
  /// all results from all contexts should be produced.
  SmallVector<SingleEntry, 4>
  lookup(SerializedSwiftName baseName,
         llvm::Optional<StoredContext> searchContext);

  /// Retrieve the set of global declarations that are going to be
  /// imported as the given Swift name into the given context.
  ///
  /// \param baseName The base name to search for. All results will
  /// have this base name.
  ///
  /// \param searchContext The context in which the resulting set of
  /// entities should reside. This may be None to indicate that
  /// all results from all contexts should be produced.
  SmallVector<SingleEntry, 4>
  lookupGlobalsAsMembersImpl(SerializedSwiftName baseName,
                             llvm::Optional<StoredContext> searchContext);

  /// Retrieve the set of global declarations that are going to be imported as
  /// members in the given context.
  SmallVector<SingleEntry, 4>
  allGlobalsAsMembersInContext(StoredContext context);

public:
  /// Lookup an unresolved context name and resolve it to a Clang
  /// named declaration.
  clang::NamedDecl *resolveContext(StringRef unresolvedName);

  /// Lookup the set of entities with the given base name.
  ///
  /// \param baseName The base name to search for. All results will
  /// have this base name.
  ///
  /// \param searchContext The context in which the resulting set of
  /// entities should reside.
  SmallVector<SingleEntry, 4> lookup(SerializedSwiftName baseName,
                                     EffectiveClangContext searchContext);

  /// Retrieve the set of base names that are stored in the lookup table.
  SmallVector<SerializedSwiftName, 4> allBaseNames();

  /// Lookup Objective-C members with the given base name, regardless
  /// of context.
  SmallVector<clang::NamedDecl *, 4>
  lookupObjCMembers(SerializedSwiftName baseName);

  /// Lookup member operators with the given base name, regardless of context.
  SmallVector<clang::NamedDecl *, 4>
  lookupMemberOperators(SerializedSwiftName baseName);

  /// Retrieve the set of Objective-C categories and extensions.
  ArrayRef<clang::ObjCCategoryDecl *> categories();

  /// Retrieve the set of global declarations that are going to be
  /// imported as members into the given context.
  ///
  /// \param baseName The base name to search for. All results will
  /// have this base name.
  ///
  /// \param searchContext The context in which the resulting set of
  /// entities should reside.
  SmallVector<SingleEntry, 4>
  lookupGlobalsAsMembers(SerializedSwiftName baseName,
                         llvm::Optional<EffectiveClangContext> searchContext);

  SmallVector<SingleEntry, 4>
  allGlobalsAsMembersInContext(EffectiveClangContext context);

  /// Retrieve the set of global declarations that are going to be
  /// imported as members.
  SmallVector<SingleEntry, 4> allGlobalsAsMembers();

  /// Deserialize all entries.
  void deserializeAll();

  /// Dump the internal representation of this lookup table.
  SWIFT_DEBUG_DUMP;

  void dump(llvm::raw_ostream &os) const;
};

namespace importer {
class ClangSourceBufferImporter;
class NameImporter;

/// Add the given named declaration as an entry to the given Swift name
/// lookup table, including any of its child entries.
void addEntryToLookupTable(SwiftLookupTable &table, clang::NamedDecl *named,
                           NameImporter &);

/// Add the macros from the given Clang preprocessor to the given
/// Swift name lookup table.
void addMacrosToLookupTable(SwiftLookupTable &table, NameImporter &);

/// Finalize a lookup table, handling any as-yet-unresolved entries
/// and emitting diagnostics if necessary.
void finalizeLookupTable(SwiftLookupTable &table, NameImporter &,
                         ClangSourceBufferImporter &buffersForDiagnostics);
}
}

namespace llvm {

template <> struct DenseMapInfo<swift::SwiftLookupTable::ContextKind> {
  typedef swift::SwiftLookupTable::ContextKind ContextKind;
  static ContextKind getEmptyKey() {
    return static_cast<ContextKind>(0);
  }
  static ContextKind getTombstoneKey() {
    return static_cast<ContextKind>(1);
  }
  static unsigned getHashValue(ContextKind kind) {
    return static_cast<unsigned>(kind);
  }
  static bool isEqual(ContextKind lhs, ContextKind rhs) {
    return lhs == rhs;
  }
};

}

#endif // SWIFT_CLANGIMPORTER_SWIFTLOOKUPTABLE_H
