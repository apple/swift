//===-- Import.h - Representation of imports --------------------*- C++ -*-===//
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
/// This file contains types used to represent information about imports
/// throughout the AST.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IMPORT_H
#define SWIFT_IMPORT_H

#include "swift/AST/AttrKind.h"
#include "swift/AST/Identifier.h"
#include "swift/Basic/Located.h"
#include "swift/Basic/OptionSet.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/None.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>

namespace swift {
class ASTContext;
class ModuleDecl;

// MARK: - Fundamental import enums

/// Describes what kind of name is being imported.
///
/// If the enumerators here are changed, make sure to update all diagnostics
/// using ImportKind as a select index.
enum class ImportKind : uint8_t {
  Module = 0,
  Type,
  Struct,
  Class,
  Enum,
  Protocol,
  Var,
  Func
};

inline bool isScopedImportKind(ImportKind importKind) {
  return importKind != ImportKind::Module;
}

/// Possible attributes for imports in source files.
enum class ImportFlags {
  /// The imported module is exposed to anyone who imports the parent module.
  Exported = 0x1,

  /// This source file has access to testable declarations in the imported
  /// module.
  Testable = 0x2,

  /// This source file has access to private declarations in the imported
  /// module.
  PrivateImport = 0x4,

  /// The imported module is an implementation detail of this file and should
  /// not be required to be present if the main module is ever imported
  /// elsewhere.
  ///
  /// Mutually exclusive with Exported.
  ImplementationOnly = 0x8,

  /// The module is imported to have access to named SPIs which is an
  /// implementation detail of this file.
  SPIAccessControl = 0x10,

  /// The module is imported assuming that the module itself predates
  /// concurrency.
  Preconcurrency = 0x20,

  /// The module's symbols are linked weakly.
  WeakLinked = 0x40,

  /// Used for DenseMap.
  Reserved = 0x80,

  /// The imported module can only be referenced from SPI decls, or
  /// implementation details.
  SPIOnly = 0x100
};

/// \see ImportFlags
using ImportOptions = OptionSet<ImportFlags>;

void simple_display(llvm::raw_ostream &out, ImportOptions options);

// MARK: - Import Paths

namespace detail {
  using ImportPathElement = Located<Identifier>;
  using ImportPathRaw = llvm::ArrayRef<ImportPathElement>;

  template<typename Subclass>
  class ImportPathBase {
  public:
    using Element = ImportPathElement;
    using Raw = ImportPathRaw;

  protected:
    Raw raw;

    ImportPathBase(Raw raw) : raw(raw) { }

  public:
    const Raw &getRaw() const { return raw; }

    Raw::iterator begin() const {
      return raw.begin();
    }

    Raw::iterator end() const {
      return raw.end();
    }

    const Element &operator[](size_t i) const { return raw[i]; }
    bool empty() const { return raw.empty(); }
    size_t size() const { return raw.size(); }

    const Element &front() const { return raw.front(); }
    const Element &back() const { return raw.back(); }

    /// True if \c this and \c other are precisely equal, including SourceLocs.
    bool operator==(const Subclass &other) const {
      return raw == other.raw;
    }

    /// True if \c this and \c other contain the same identifiers in the same
    /// order, ignoring SourceLocs.
    bool isSameAs(const Subclass &other) const {
      return size() == other.size()
             && std::equal(this->begin(), this->end(), other.begin(),
                  [](const Element &l, const Element &r) -> bool {
                    return l.Item == r.Item;
                  }
                );
    }

    Subclass getTopLevelPath() const {
      assert(size() >= 1 && "nothing to take");
      return Subclass(raw.take_front());
    }

    Subclass getParentPath() const {
      assert(size() >= 0 && "nothing to take");
      return Subclass(raw.drop_back());
    }

    SourceRange getSourceRange() const {
      if (empty()) return SourceRange();
      return SourceRange(raw.front().Loc, raw.back().Loc);
    }

    void print(llvm::raw_ostream &os) const {
      llvm::interleave(*this,
                       [&](Element elem) { os << elem.Item.str(); },
                       [&]() { os << "."; });
    }

    void getString(SmallVectorImpl<char> &modulePathStr) const {
      llvm::raw_svector_ostream os(modulePathStr);
      print(os);
    }
  };

  // These shims avoid circularity between ASTContext.h and Import.h.
  ImportPathRaw ImportPathBuilder_copyToImpl(ASTContext &ctx,
                                             ImportPathRaw raw);
  Identifier ImportPathBuilder_getIdentifierImpl(ASTContext &ctx,
                                                 StringRef string);

  template<typename Subclass>
  class ImportPathBuilder {
    using Scratch = llvm::SmallVector<ImportPathElement, 4>;
    Scratch scratch;

  public:
    using value_type = Scratch::value_type;
    using reference = Scratch::reference;
    using iterator = Scratch::iterator;
    using const_iterator = Scratch::const_iterator;
    using difference_type = Scratch::difference_type;
    using size_type = Scratch::size_type;

    Subclass get() const {
      return Subclass(scratch);
    }

    Subclass copyTo(ASTContext &ctx) const {
      return Subclass(ImportPathBuilder_copyToImpl(ctx, scratch));
    }

    ImportPathBuilder() : scratch() { }
    ImportPathBuilder(const ImportPathElement &elem) : scratch() {
      scratch = { elem };
    }
    ImportPathBuilder(Identifier name, SourceLoc loc = SourceLoc())
        : ImportPathBuilder(ImportPathElement(name, loc)) { }

    template<typename Iterator>
    ImportPathBuilder(Iterator begin, Iterator end) : scratch(begin, end) { }

    template<typename Range>
    ImportPathBuilder(Range collection)
        : scratch(collection.begin(), collection.end()) { }

    /// Parses \p text into elements separated by \p separator, with identifiers
    /// from \p ctx and invalid SourceLocs.
    ///
    /// \warning This is not very robust; for instance, it doesn't check the
    /// validity of the identifiers.
    ImportPathBuilder(ASTContext &ctx, StringRef text, char separator)
        : scratch()
    {
      while (!text.empty()) {
        StringRef next;
        std::tie(next, text) = text.split(separator);
        push_back(ImportPathBuilder_getIdentifierImpl(ctx, next));
      }
    }

    /// Parses \p text into elements separated by \p separator, with identifiers
    /// from \p ctx starting at \p loc.
    ///
    /// \warning This is not very robust; for instance, it doesn't check the
    /// validity of the identifiers.
    ImportPathBuilder(ASTContext &ctx, StringRef text, char separator,
                      SourceLoc loc)
        : scratch() {
      while (!text.empty()) {
        StringRef next;
        std::tie(next, text) = text.split(separator);
        push_back({ImportPathBuilder_getIdentifierImpl(ctx, next), loc});
        loc = loc.getAdvancedLocOrInvalid(next.size() + 1);
      }
    }

    void push_back(const ImportPathElement &elem) { scratch.push_back(elem); }
    void push_back(Identifier name, SourceLoc loc = SourceLoc()) {
      scratch.push_back({ name, loc });
    }

    void pop_back() { scratch.pop_back(); }

    bool empty() const { return scratch.empty(); }
    size_t size() const { return scratch.size(); }

    llvm::SmallVector<ImportPathElement, 4>::iterator begin() {
      return scratch.begin();
    }
    llvm::SmallVector<ImportPathElement, 4>::iterator end() {
      return scratch.end();
    }

    const ImportPathElement &front() const { return scratch.front(); }
    ImportPathElement &front() { return scratch.front(); }
    const ImportPathElement &back() const { return scratch.back(); }
    ImportPathElement &back() { return scratch.back(); }

    template<typename Iterator>
    void append(Iterator begin, Iterator end) {
      scratch.append(begin, end);
    }

    template<typename Range>
    void append(Range collection) {
      append(collection.begin(), collection.end());
    }
  };
}

/// @name ImportPathBase Comparison Operators
/// @{
template <typename Subclass>
inline bool operator<(const detail::ImportPathBase<Subclass> &LHS,
                      const detail::ImportPathBase<Subclass> &RHS) {
  using Element = typename detail::ImportPathBase<Subclass>::Element;
  auto Comparator = [](const Element &l, const Element &r) {
    return l.Item.compare(r.Item) < 0;
  };
  return std::lexicographical_compare(LHS.begin(), LHS.end(), RHS.begin(),
                                      RHS.end(), Comparator);
}
/// @}

/// An undifferentiated series of dotted identifiers in an \c import statement,
/// like \c Foo.Bar. Each identifier is packaged with its corresponding source
/// location.
///
/// The first element of an \c ImportPath is always a top-level module name. The
/// remaining elements could specify a scope (naming a declaration in the
/// module) or a chain of submodule names. \c ImportPath does not differentiate
/// between these cases; its \c getModule() and \c getAccess() methods take an
/// \c ImportKind parameter to decide how to divvy up these identifiers.
///
/// \c ImportPath is only used when analyzing the parsed representation of code.
/// Most code should use \c ImportPath::Module or \c ImportPath::Access, which
/// have semantic meaning.
///
/// \c ImportPath is essentially a wrapper around \c ArrayRef and does not own
/// its elements, so something else needs to manage their lifetime.
/// \c ImportDecl owns the memory backing \c ImportDecl::getImportPath().
class ImportPath : public detail::ImportPathBase<ImportPath> {
public:
  /// A single dotted name from an \c ImportPath, \c ImportPath::Module, or
  /// \c ImportPath::Access, with its source location.
  using Element = detail::ImportPathBase<ImportPath>::Element;

  /// The backing type for \c ImportPath, \c ImportPath::Module, and
  /// \c ImportPath::Access; namely, an \c ArrayRef of \c ImportPath::Elements.
  using Raw = detail::ImportPathBase<ImportPath>::Raw;

  /// A helper type which encapsulates a temporary vector and can produce an
  /// import path from it. In addition to the obvious use in a temporary
  /// variable, this type can be used mid-expression to produce an import path
  /// that is valid until the end of the expression.
  using Builder = detail::ImportPathBuilder<ImportPath>;

  /// Represents an access path--the portion of an \c ImportPath which describes
  /// the name of a declaration to scope the import to.
  ///
  /// \c ImportPath::Access is used in scoped imports to designate a specific
  /// declaration inside the module. The import will only* cover this
  /// declaration, and will import it with a higher "priority" than usual, so
  /// name lookup will prefer it over identically-named declarations visible
  /// through other imports.
  ///
  /// (* Not actually only--e.g. extensions will be imported too. The primary
  /// use case for scoped imports is actually to resolve name conflicts, not to
  /// reduce the set of visible declarations.)
  ///
  /// When \c ImportPath::Access is empty, this means the import covers all
  /// declarations in the module.
  ///
  /// Although in theory Swift could support scoped imports of nested
  /// declarations, in practice it currently only supports scoped imports of
  /// top-level declarations. Reflecting this, \c ImportPath::Access is backed
  /// by an \c ArrayRef, but it asserts that the access path has zero or one
  /// elements.
  ///
  /// \c ImportPath::Access is essentially a wrapper around \c ArrayRef and does
  /// not own its elements, so something else needs to manage their lifetime.
  /// \c ImportDecl owns the memory backing \c ImportDecl::getAccessPath().
  class Access : public detail::ImportPathBase<Access> {
  public:
    /// A helper type which encapsulates a temporary vector and can produce a
    /// scope path from it. In addition to the obvious use in a temporary
    /// variable, this type can be used mid-expression to produce a scope path
    /// that is valid until the end of the expression.
    using Builder = detail::ImportPathBuilder<Access>;

    Access(ImportPath::Raw raw) : ImportPathBase(raw) {
      assert(size() <= 1 && "nested scoped imports are not supported");
    }

    Access() : ImportPathBase({}) { }

    /// Returns \c true if the scope of this import includes \c name. An empty
    /// scope matches all names.
    bool matches(DeclName name) const {
      return empty() || DeclName(front().Item).matchesRef(name);
    }
  };

  /// Represents a module path--the portion of an \c ImportPath which describes
  /// the name of the module being imported, possibly including submodules.
  ///
  /// \c ImportPath::Module contains one or more identifiers. The first
  /// identifier names a top-level module. The second and subsequent
  /// identifiers, if present, chain together to name a specific submodule to
  /// import. (Although Swift modules cannot currently contain submodules, Swift
  /// can import Clang submodules.)
  ///
  /// \c ImportPath::Module is essentially a wrapper around \c ArrayRef and
  /// does not own its elements, so something else needs to manage their
  /// lifetime. \c ImportDecl owns the memory backing
  /// \c ImportDecl::getModulePath().
  class Module : public detail::ImportPathBase<Module> {
  public:
    /// A helper type which encapsulates a temporary vector and can produce a
    /// module path from it. In addition to the obvious use in a temporary
    /// variable, this type can be used mid-expression to produce a module path
    /// that is valid until the end of the expression.
    using Builder = detail::ImportPathBuilder<Module>;

    Module(ImportPath::Raw raw) : ImportPathBase(raw) {
      assert(size() >= 1 && "must have a top-level module");
    }

    // Note: This type does not have a constructor which just takes an
    // `Identifier` because it would not be able to create a temporary
    // `ImportPath::Element` with a long enough lifetime to return. Use
    // `ImportPath::Module::Builder` to create a temporary module path.

    bool hasSubmodule() const {
      return size() != 1;
    }

    ImportPath::Raw getSubmodulePath() const {
      return getRaw().drop_front();
    }
  };

  ImportPath(Raw raw) : ImportPathBase(raw) {
    assert(raw.size() >= 1 && "ImportPath must contain a module name");
  }

  /// Extracts the portion of the \c ImportPath which represents a module name,
  /// including submodules if appropriate.
  Module getModulePath(bool isScoped) const {
    if (isScoped)
      return Module(getRaw().drop_back());

    return Module(getRaw());
  }

  /// Extracts the portion of the \c ImportPath which represents a scope for the
  /// import.
  Access getAccessPath(bool isScoped) const {
    if (isScoped) {
      assert(size() >= 2 && "scoped ImportPath must contain a decl name");
      return Access(getRaw().take_back());
    }

    return Access();
  }

  /// Extracts the portion of the \c ImportPath which represents a module name,
  /// including submodules, assuming the \c ImportDecl has the indicated
  /// \c importKind.
  Module getModulePath(ImportKind importKind) const {
    return getModulePath(isScopedImportKind(importKind));
  }

  /// Extracts the portion of the \c ImportPath which represents a scope for the
  /// import, assuming the \c ImportDecl has the indicated \c importKind.
  Access getAccessPath(ImportKind importKind) const {
    return getAccessPath(isScopedImportKind(importKind));
  }
};

// MARK: - Abstractions of imports

/// Convenience struct to keep track of an import path and whether or not it
/// is scoped.
class UnloadedImportedModule {
  // This is basically an ArrayRef with a bit stolen from the pointer.
  // FIXME: Extract an ArrayRefIntPair type from this.
  llvm::PointerIntPair<ImportPath::Raw::iterator, 1, bool> dataAndIsScoped;
  ImportPath::Raw::size_type length;

  ImportPath::Raw::iterator data() const {
    return dataAndIsScoped.getPointer();
  }

  bool isScoped() const {
    return dataAndIsScoped.getInt();
  }

  ImportPath::Raw getRaw() const {
    return ImportPath::Raw(data(), length);
  }

  UnloadedImportedModule(ImportPath::Raw raw, bool isScoped)
    : dataAndIsScoped(raw.data(), isScoped), length(raw.size()) { }

public:
  UnloadedImportedModule(ImportPath importPath, bool isScoped)
    : UnloadedImportedModule(importPath.getRaw(), isScoped) { }

  UnloadedImportedModule(ImportPath importPath, ImportKind importKind)
    : UnloadedImportedModule(importPath, isScopedImportKind(importKind)) { }

  ImportPath getImportPath() const {
    return ImportPath(getRaw());
  }

  ImportPath::Module getModulePath() const {
    return getImportPath().getModulePath(isScoped());
  }

  ImportPath::Access getAccessPath() const {
    return getImportPath().getAccessPath(isScoped());
  }

  friend bool operator==(const UnloadedImportedModule &lhs,
                         const UnloadedImportedModule &rhs) {
    return (lhs.getRaw() == rhs.getRaw()) &&
           (lhs.isScoped() == rhs.isScoped());
  }
};

/// Convenience struct to keep track of a module along with its access path.
struct alignas(uint64_t) ImportedModule {
  /// The access path from an import: `import Foo.Bar` -> `Foo.Bar`.
  ImportPath::Access accessPath;
  /// The actual module corresponding to the import.
  ///
  /// Invariant: The pointer is non-null.
  ModuleDecl *importedModule;

  ImportedModule(ImportPath::Access accessPath,
                 ModuleDecl *importedModule)
      : accessPath(accessPath), importedModule(importedModule) {
    assert(this->importedModule);
  }

  explicit ImportedModule(ModuleDecl *importedModule)
      : ImportedModule(ImportPath::Access(), importedModule) { }

  bool operator==(const ImportedModule &other) const {
    return (this->importedModule == other.importedModule) &&
           (this->accessPath == other.accessPath);
  }

  /// Uniques the items in \p imports, ignoring the source locations of the
  /// access paths.
  ///
  /// The order of items in \p imports is \e not preserved.
  static void removeDuplicates(SmallVectorImpl<ImportedModule> &imports);

  // Purely here to allow ImportedModule and UnloadedImportedModule to
  // substitute into the same templates.
  ImportPath::Access getAccessPath() const { return accessPath; }

  /// Arbitrarily orders ImportedModule records, for inclusion in sets and such.
  class Order {
  public:
    bool operator()(const ImportedModule &lhs,
                    const ImportedModule &rhs) const {
      if (lhs.importedModule != rhs.importedModule)
        return std::less<const ModuleDecl *>()(lhs.importedModule,
                                               rhs.importedModule);
      if (lhs.accessPath.getRaw().data() != rhs.accessPath.getRaw().data())
        return std::less<ImportPath::Raw::iterator>()(lhs.accessPath.begin(),
                                                   rhs.accessPath.begin());
      return lhs.accessPath.size() < rhs.accessPath.size();
    }
  };
};

/// Augments a type representing an import to also include information about the
/// import's attributes. This is usually used with either \c ImportedModule or
/// \c UnloadedImportedModule.
template<class ModuleInfo>
struct AttributedImport {
  /// Information about the module and access path being imported.
  ModuleInfo module;

  /// The location of the 'import' keyword, for an explicit import.
  SourceLoc importLoc;

  /// Flags indicating which attributes of this import are present.
  ImportOptions options;

  /// If this is a @_private import, the value of its 'sourceFile:' argument;
  /// otherwise, empty string.
  StringRef sourceFileArg;

  /// Names of explicitly imported SPI groups.
  ArrayRef<Identifier> spiGroups;

  /// When the import declaration has a `@preconcurrency` annotation, this
  /// is the source range covering the annotation.
  SourceRange preconcurrencyRange;

  /// If the import declaration has a `@_documentation(visibility: <access>)`
  /// attribute, this is the given access level.
  llvm::Optional<AccessLevel> docVisibility;

  /// Access level limiting how imported types can be exported.
  AccessLevel accessLevel;

  /// Location of the attribute that defined \c accessLevel. Also indicates
  /// if the access level was implicit or explicit.
  SourceLoc accessLevelLoc;

  AttributedImport(ModuleInfo module, SourceLoc importLoc = SourceLoc(),
                   ImportOptions options = ImportOptions(),
                   StringRef filename = {}, ArrayRef<Identifier> spiGroups = {},
                   SourceRange preconcurrencyRange = {},
                   llvm::Optional<AccessLevel> docVisibility = llvm::None,
                   AccessLevel accessLevel = AccessLevel::Public,
                   SourceLoc accessLevelLoc = SourceLoc())
      : module(module), importLoc(importLoc), options(options),
        sourceFileArg(filename), spiGroups(spiGroups),
        preconcurrencyRange(preconcurrencyRange), docVisibility(docVisibility),
        accessLevel(accessLevel), accessLevelLoc(accessLevelLoc) {
    assert(!(options.contains(ImportFlags::Exported) &&
             options.contains(ImportFlags::ImplementationOnly)) ||
           options.contains(ImportFlags::Reserved));
  }

  template<class OtherModuleInfo>
  AttributedImport(ModuleInfo module, AttributedImport<OtherModuleInfo> other)
    : AttributedImport(module, other.importLoc, other.options,
                       other.sourceFileArg, other.spiGroups,
                       other.preconcurrencyRange, other.docVisibility,
                       other.accessLevel, other.accessLevelLoc) { }

  friend bool operator==(const AttributedImport<ModuleInfo> &lhs,
                         const AttributedImport<ModuleInfo> &rhs) {
    return lhs.module == rhs.module &&
           lhs.options.toRaw() == rhs.options.toRaw() &&
           lhs.sourceFileArg == rhs.sourceFileArg &&
           lhs.spiGroups == rhs.spiGroups &&
           lhs.docVisibility == rhs.docVisibility &&
           lhs.accessLevel == rhs.accessLevel &&
           lhs.accessLevelLoc == rhs.accessLevelLoc;
  }

  AttributedImport<ImportedModule> getLoaded(ModuleDecl *loadedModule) const {
    return { ImportedModule(module.getAccessPath(), loadedModule), *this };
  }
};

void simple_display(llvm::raw_ostream &out,
                    const ImportedModule &import);

void simple_display(llvm::raw_ostream &out,
                    const UnloadedImportedModule &import);

// This is a quasi-implementation detail of the template version below.
void simple_display(llvm::raw_ostream &out,
                    const AttributedImport<std::tuple<>> &import);

template<typename ModuleInfo>
void simple_display(llvm::raw_ostream &out,
                    const AttributedImport<ModuleInfo> &import) {
  // Print the module.
  simple_display(out, import.module);

  // Print the other details of the import, using the std::tuple<>
  // specialization.
  AttributedImport<std::tuple<>> importWithoutModule({}, import);
  simple_display(out, importWithoutModule);
}

// MARK: - Implicit imports

/// The kind of stdlib that should be imported.
enum class ImplicitStdlibKind {
  /// No standard library should be implicitly imported.
  None,

  /// The Builtin module should be implicitly imported.
  Builtin,

  /// The regular Swift standard library should be implicitly imported.
  Stdlib
};

/// Represents unprocessed options for implicit imports.
struct ImplicitImportInfo {
  /// The implicit stdlib to import.
  ImplicitStdlibKind StdlibKind;

  /// Whether we should attempt to import an underlying Clang half of this
  /// module.
  bool ShouldImportUnderlyingModule;

  /// The bridging header path for this module, empty if there is none.
  StringRef BridgingHeaderPath;

  /// The names of additional modules to be loaded and implicitly imported.
  SmallVector<AttributedImport<UnloadedImportedModule>, 4>
      AdditionalUnloadedImports;

  /// An additional list of already-loaded modules which should be implicitly
  /// imported.
  SmallVector<AttributedImport<ImportedModule>, 4>
      AdditionalImports;

  ImplicitImportInfo()
      : StdlibKind(ImplicitStdlibKind::None),
        ShouldImportUnderlyingModule(false) {}
};

/// Contains names of and pointers to modules that must be implicitly imported.
struct ImplicitImportList {
  ArrayRef<AttributedImport<ImportedModule>> imports;
  ArrayRef<AttributedImport<UnloadedImportedModule>> unloadedImports;

  friend bool operator==(const ImplicitImportList &lhs,
                         const ImplicitImportList &rhs) {
    return lhs.imports == rhs.imports
        && lhs.unloadedImports == rhs.unloadedImports;
  }
};

/// A list of modules to implicitly import.
void simple_display(llvm::raw_ostream &out,
                    const ImplicitImportList &importList);

}

// MARK: - DenseMapInfo

namespace llvm {

template<>
struct DenseMapInfo<swift::ImportOptions> {
  using ImportOptions = swift::ImportOptions;

  using UnsignedDMI = DenseMapInfo<uint8_t>;

  static inline ImportOptions getEmptyKey() {
    return ImportOptions(UnsignedDMI::getEmptyKey());
  }
  static inline ImportOptions getTombstoneKey() {
    return ImportOptions(UnsignedDMI::getTombstoneKey());
  }
  static inline unsigned getHashValue(ImportOptions options) {
    return UnsignedDMI::getHashValue(options.toRaw());
  }
  static bool isEqual(ImportOptions a, ImportOptions b) {
    return UnsignedDMI::isEqual(a.toRaw(), b.toRaw());
  }
};

template <>
class DenseMapInfo<swift::ImportedModule> {
  using ImportedModule = swift::ImportedModule;
  using ModuleDecl = swift::ModuleDecl;
public:
  static ImportedModule getEmptyKey() {
    return {{}, llvm::DenseMapInfo<ModuleDecl *>::getEmptyKey()};
  }
  static ImportedModule getTombstoneKey() {
    return {{}, llvm::DenseMapInfo<ModuleDecl *>::getTombstoneKey()};
  }

  static unsigned getHashValue(const ImportedModule &val) {
    auto pair = std::make_pair(val.accessPath.size(), val.importedModule);
    return llvm::DenseMapInfo<decltype(pair)>::getHashValue(pair);
  }

  static bool isEqual(const ImportedModule &lhs,
                      const ImportedModule &rhs) {
    return lhs.importedModule == rhs.importedModule &&
           lhs.accessPath.isSameAs(rhs.accessPath);
  }
};

template<typename ModuleInfo>
struct DenseMapInfo<swift::AttributedImport<ModuleInfo>> {
  using AttributedImport = swift::AttributedImport<ModuleInfo>;

  using ModuleInfoDMI = DenseMapInfo<ModuleInfo>;
  using ImportOptionsDMI = DenseMapInfo<swift::ImportOptions>;
  using StringRefDMI = DenseMapInfo<StringRef>;
  using SourceLocDMI = DenseMapInfo<swift::SourceLoc>;
  // We can't include spiGroups in the hash because ArrayRef<Identifier> is not
  // DenseMapInfo-able, but we do check that the spiGroups match in isEqual().

  static inline AttributedImport getEmptyKey() {
    return AttributedImport(ModuleInfoDMI::getEmptyKey(),
                            SourceLocDMI::getEmptyKey(),
                            ImportOptionsDMI::getEmptyKey(),
                            StringRefDMI::getEmptyKey(),
                            {}, {}, None,
                            swift::AccessLevel::Public, {});
  }
  static inline AttributedImport getTombstoneKey() {
    return AttributedImport(ModuleInfoDMI::getTombstoneKey(),
                            SourceLocDMI::getEmptyKey(),
                            ImportOptionsDMI::getTombstoneKey(),
                            StringRefDMI::getTombstoneKey(),
                            {}, {}, None,
                            swift::AccessLevel::Public, {});
  }
  static inline unsigned getHashValue(const AttributedImport &import) {
    return detail::combineHashValue(
        ModuleInfoDMI::getHashValue(import.module),
        detail::combineHashValue(
          ImportOptionsDMI::getHashValue(import.options),
          StringRefDMI::getHashValue(import.sourceFileArg)));
  }
  static bool isEqual(const AttributedImport &a,
                      const AttributedImport &b) {
    return ModuleInfoDMI::isEqual(a.module, b.module) &&
           ImportOptionsDMI::isEqual(a.options, b.options) &&
           StringRefDMI::isEqual(a.sourceFileArg, b.sourceFileArg) &&
           a.spiGroups == b.spiGroups &&
           a.docVisibility == b.docVisibility &&
           a.accessLevel == b.accessLevel &&
           a.accessLevelLoc == b.accessLevelLoc;
  }
};
}

#endif
