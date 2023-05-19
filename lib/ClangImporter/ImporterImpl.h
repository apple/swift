//===--- ImporterImpl.h - Import Clang Modules: Implementation --*- C++ -*-===//
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
// This file provides the implementation class definitions for the Clang
// module loader.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_CLANG_IMPORTER_IMPL_H
#define SWIFT_CLANG_IMPORTER_IMPL_H

#include "ClangAdapter.h"
#include "ClangSourceBufferImporter.h"
#include "ImportEnumInfo.h"
#include "ImportName.h"
#include "SwiftLookupTable.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/Module.h"
#include "swift/AST/RequirementSignature.h"
#include "swift/AST/Type.h"
#include "swift/Basic/FileTypes.h"
#include "swift/Basic/StringExtras.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/ClangImporter/ClangModule.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/AST/DeclVisitor.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/IdentifierTable.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Serialization/ModuleFileExtension.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/Support/Path.h"
#include <functional>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace llvm {

class SmallBitVector;

}

namespace clang {
class APValue;
class DeclarationName;
class MangleContext;
class ObjCInterfaceDecl;
class ObjCMethodDecl;
class ObjCPropertyDecl;
class ParmVarDecl;
class Parser;
class QualType;
class TypedefNameDecl;
}

namespace swift {

class ASTContext;
class ClassDecl;
class ConstructorDecl;
class Decl;
class DeclContext;
class Expr;
class ExtensionDecl;
class FuncDecl;
class Identifier;
class Pattern;
class SubscriptDecl;
class ValueDecl;

/// Describes the kind of conversion to apply to a constant value.
enum class ConstantConvertKind {
  /// No conversion required.
  None,
  /// Construct the given type from the constant value by calling
  /// init(rawValue:).
  Construction,
  /// Construct the given type from the constant value by force
  /// unwrapping the result of init(rawValue:).
  ConstructionWithUnwrap
};

/// Describes the kind of type import we're performing.
enum class ImportTypeKind {
  /// Import a type in its most abstract form, without any adjustment.
  Abstract,

  /// Import the underlying type of a typedef.
  Typedef,

  /// Import the type of a literal value.
  Value,

  /// Import the type of an Objective-C generic argument.
  ObjCCollectionElement,

  /// Import the declared type of a variable.
  Variable,
  
  /// Import the declared type of an audited variable.
  ///
  /// This is exactly like ImportTypeKind::Variable, except it
  /// disables wrapping CF class types in Unmanaged.
  AuditedVariable,

  /// Import the declared type of a struct or union field.
  RecordField,
  
  /// Import the result type of a function.
  ///
  /// This provides special treatment for 'void', among other things, and
  /// enables the conversion of bridged types.
  Result,

  /// Import the result type of an audited function.
  ///
  /// This is exactly like ImportTypeKind::Result, except it
  /// disables wrapping CF class types in Unmanaged.
  AuditedResult,

  /// Import the type of a function parameter.
  ///
  /// Special handling:
  /// * C and C++ pointers become `UnsafePointer?` or `UnsafeMutablePointer?`
  /// * C++ references become `UnsafePointer` or `UnsafeMutablePointer`
  /// * Bridging that requires type conversions is allowed.
  /// Parameters are always considered CF-audited.
  Parameter,

  /// Import the type of a parameter to a completion handler that can indicate
  /// a thrown error.
  ///
  /// Special handling:
  /// * _Nullable_result is treated as _Nonnull rather than _Nullable_result.
  CompletionHandlerResultParameter,

  /// Import the type of a parameter declared with
  /// \c CF_RETURNS_RETAINED.
  ///
  /// This ensures that the parameter is not marked as Unmanaged.
  CFRetainedOutParameter,

  /// Import the type of a parameter declared with
  /// \c CF_RETURNS_NON_RETAINED.
  ///
  /// This ensures that the parameter is not marked as Unmanaged.
  CFUnretainedOutParameter,

  /// Import the type of an ObjC property.
  ///
  /// This enables the conversion of bridged types. Properties are always
  /// considered CF-audited.
  Property,

  /// Import the type of an ObjC property accessor marked 'weak',
  /// 'assign', or 'unsafe_unretained'.
  ///
  /// Like Property, but doesn't allow bridging to a value type, since that
  /// would discard the ownership.
  PropertyWithReferenceSemantics,

  /// Import the underlying type of an enum.
  ///
  /// This provides special treatment for 'NSUInteger'.
  Enum
};

/// Flags which are extracted from an imported declaration to influence how its
/// type is imported. Typically used via \c ImportTypeAttrs to form an option
/// set.
///
/// \warning Do not use this as a random grab bag of flags to \c importType() .
/// This information is intended to be extracted and applied all at once.
enum class ImportTypeAttr : uint8_t {
  /// Type should be imported as though declaration was marked with
  /// \c __attribute__((noescape)) .
  NoEscape = 1 << 0,

  /// Type should be imported as though declaration was marked with
  /// \c __attribute__((swift_attr("@MainActor"))) .
  MainActor = 1 << 1,

  /// Type should be imported as though declaration was marked with
  /// \c __attribute__((swift_attr("@Sendable"))) .
  Sendable = 1 << 2,

  /// Type is in a declaration where it would be imported as Sendable by
  /// default. This comes directly from the parameters to
  /// \c getImportTypeAttrs() and merely affects diagnostics.
  DefaultsToSendable = 1 << 3
};

/// Attributes which were set on the declaration and affect how its type is
/// imported.
///
/// \seeAlso ImportTypeAttr
using ImportTypeAttrs = OptionSet<ImportTypeAttr>;

/// Extracts the \c ImportTypeAttrs from a declaration.
///
/// \param D The declaration to extract attributes from.
/// \param isParam Is the declaration a function parameter? If so, additional
///        attributes will be imported.
/// \param sendableByDefault If the sendability of the declaration is not
///        specified, should the \c \@Sendable attribute be added implicitly?
///        Used for e.g. completion handlers.
ImportTypeAttrs getImportTypeAttrs(const clang::Decl *D, bool isParam = false,
                                   bool sendableByDefault = false);

struct ImportDiagnostic {
  ImportDiagnosticTarget target;
  Diagnostic diag;
  clang::SourceLocation loc;

  ImportDiagnostic(ImportDiagnosticTarget target, const Diagnostic &diag,
                   clang::SourceLocation loc)
      : target(target), diag(diag), loc(loc) {}

  bool operator==(const ImportDiagnostic &other) const {
    return target == other.target && loc == other.loc &&
           diag.getID() == other.diag.getID();
  }
};

/// Controls whether \p decl, when imported, should name the fully-bridged
/// Swift type or the original Clang type.
///
/// In either case we end up losing sugar at some uses sites, so this is more
/// about what the right default is.
static inline Bridgeability
getTypedefBridgeability(const clang::TypedefNameDecl *decl) {
  if (decl->hasAttr<clang::SwiftBridgedTypedefAttr>() ||
      decl->getUnderlyingType()->isBlockPointerType()) {
    return Bridgeability::Full;
  }
  return Bridgeability::None;
}

/// Describes the kind of the C type that can be mapped to a stdlib
/// swift type.
enum class MappedCTypeKind {
  UnsignedInt,
  SignedInt,
  UnsignedWord,
  SignedWord,
  FloatIEEEsingle,
  FloatIEEEdouble,
  FloatX87DoubleExtended,
  VaList,
  ObjCBool,
  ObjCSel,
  ObjCId,
  ObjCClass,
  CGFloat,
  Block,
};

/// Describes what to do with the C name of a type that can be mapped to
/// a Swift standard library type.
enum class MappedTypeNameKind {
  DoNothing,
  DefineOnly,
  DefineAndUse
};

/// Describes certain kinds of methods that need to be specially
/// handled by the importer.
enum class SpecialMethodKind {
  Regular,
  Constructor,
  NSDictionarySubscriptGetter
};

#define SWIFT_PROTOCOL_SUFFIX "Protocol"
#define SWIFT_CFTYPE_SUFFIX "Ref"

/// Describes whether to classify a factory method as an initializer.
enum class FactoryAsInitKind {
  /// Infer based on name and type (the default).
  Infer,
  /// Treat as a class method.
  AsClassMethod,
  /// Treat as an initializer.
  AsInitializer
};

namespace importer {
struct PlatformAvailability {
private:
  PlatformKind platformKind;

public:
  /// Returns true when the given platform should be considered for
  /// availabilityon imported declarations.
  bool isPlatformRelevant(StringRef platform) const;

  /// Returns true when the given declaration with the given deprecation
  /// should be included in the cutoff of imported deprecated APIs marked
  /// unavailable.
  bool treatDeprecatedAsUnavailable(const clang::Decl *clangDecl,
                                    const llvm::VersionTuple &version,
                                    bool isAsync) const;

  /// The message to embed for implicitly unavailability if a deprecated
  /// API is now unavailable.
  std::string deprecatedAsUnavailableMessage;

  /// The message to embed for implicit async unavailability based on
  /// deprecation.
  std::string asyncDeprecatedAsUnavailableMessage;

  PlatformAvailability(const LangOptions &opts);

private:
  PlatformAvailability(const PlatformAvailability&) = delete;
  PlatformAvailability &operator=(const PlatformAvailability &) = delete;
};
}

using LookupTableMap =
    llvm::DenseMap<StringRef, std::unique_ptr<SwiftLookupTable>>;

/// The result of importing a clang type. It holds both the Swift Type
/// as well as a bool in which 'true' indicates either:
///   This is an Optional type.
///   This is a function type where the result type is an Optional.
/// It is otherwise 'false'.
class ImportedType {
  Type type;
  bool isIUO;

public:
  ImportedType() {
    type = Type();
    isIUO = false;
  }

  ImportedType(Type ty, bool implicitlyUnwrap)
      : type(ty), isIUO(implicitlyUnwrap) {
#if !defined(NDEBUG)
    if (implicitlyUnwrap) {
      assert(ty->getOptionalObjectType() || ty->getAs<AnyFunctionType>());
      if (!ty->getOptionalObjectType()) {
        auto fnTy = ty->castTo<AnyFunctionType>();
        assert(fnTy->getResult()->getOptionalObjectType());
      }
    }
#endif
  }

  Type getType() const { return type; }

  bool isImplicitlyUnwrapped() const { return isIUO; }

  // Allow a direct test in boolean contexts. It makes sense to base
  // this entirely on the type as the isIUO is meaningless for a null
  // type.
  explicit operator bool() const { return type.getPointer() != nullptr; }
};

/// Wraps a Clang source location with additional optional information used to
/// resolve it for diagnostics.
struct HeaderLoc {
  clang::SourceLocation clangLoc;
  SourceLoc fallbackLoc;
  const clang::SourceManager *sourceMgr;

  explicit HeaderLoc(clang::SourceLocation clangLoc,
                     SourceLoc fallbackLoc = SourceLoc(),
                     const clang::SourceManager *sourceMgr = nullptr)
    : clangLoc(clangLoc), fallbackLoc(fallbackLoc), sourceMgr(sourceMgr) {}
};

struct ImportDiagnosticTargetHasher {
  std::size_t operator()(const ImportDiagnosticTarget &target) const {
    return std::hash<void *>()(target.getOpaqueValue());
  }
};

struct ImportDiagnosticHasher {
  std::size_t operator()(const ImportDiagnostic &diag) const {
    return llvm::hash_combine(diag.target.getOpaqueValue(), diag.diag.getID(),
                              diag.loc.getHashValue());
  }
};

/// Implementation of the Clang importer.
class LLVM_LIBRARY_VISIBILITY ClangImporter::Implementation 
  : public LazyMemberLoader,
    public LazyConformanceLoader
{
  friend class ClangImporter;
  using Version = importer::ImportNameVersion;

public:
  Implementation(ASTContext &ctx,
                 DWARFImporterDelegate *dwarfImporterDelegate);
  ~Implementation();

  class DiagnosticWalker : public clang::RecursiveASTVisitor<DiagnosticWalker> {
  public:
    DiagnosticWalker(ClangImporter::Implementation &Impl);
    bool TraverseDecl(clang::Decl *D);
    bool TraverseParmVarDecl(clang::ParmVarDecl *D);
    bool VisitDecl(clang::Decl *D);
    bool VisitMacro(const clang::MacroInfo *MI);
    bool VisitObjCObjectPointerType(clang::ObjCObjectPointerType *T);
    bool VisitType(clang::Type *T);

  private:
    Implementation &Impl;
    clang::SourceLocation TypeReferenceSourceLocation;
  };

  /// Swift AST context.
  ASTContext &SwiftContext;

  // Associates a vector of import diagnostics with a ClangNode
  std::unordered_map<ImportDiagnosticTarget, std::vector<ImportDiagnostic>,
                     ImportDiagnosticTargetHasher>
      ImportDiagnostics;

  // Tracks the set of import diagnostics already produced for deduplication
  // purposes.
  std::unordered_set<ImportDiagnostic, ImportDiagnosticHasher>
      CollectedDiagnostics;

  const bool ImportForwardDeclarations;
  const bool DisableSwiftBridgeAttr;
  const bool BridgingHeaderExplicitlyRequested;
  const bool DisableOverlayModules;
  const bool EnableClangSPI;
  bool importSymbolicCXXDecls;

  bool IsReadingBridgingPCH;
  llvm::SmallVector<clang::serialization::SubmoduleID, 2> PCHImportedSubmodules;

  const Version CurrentVersion;

  constexpr static const char * const moduleImportBufferName =
    "<swift-imported-modules>";
  constexpr static const char * const bridgingHeaderBufferName =
    "<bridging-header-import>";

private:
  DiagnosticWalker Walker;

  /// The Swift lookup tables, per module.
  ///
  /// Annoyingly, we list this table early so that it gets torn down after
  /// the underlying Clang instances that reference it
  /// (through the Swift name lookup module file extension).
  LookupTableMap LookupTables;

  /// A helper class used to bring Clang buffers into Swift's SourceManager
  /// for the purpose of emitting diagnostics.
  ///
  /// Listed early so that it gets torn down after the underlying Clang
  /// instances that also use these buffers.
  importer::ClangSourceBufferImporter BuffersForDiagnostics;

  /// The fake buffer used to import modules.
  ///
  /// \see getNextIncludeLoc
  clang::FileID DummyIncludeBuffer;

  /// A count of the number of load module operations.
  ///
  /// \see getNextIncludeLoc
  unsigned IncludeCounter = 0;

  /// Generate a dummy Clang source location for header includes and module
  /// imports.
  ///
  /// These have to be unique and valid or Clang gets very confused.
  clang::SourceLocation getNextIncludeLoc();

  /// Used to avoid running the AST verifier over the same declarations.
  size_t VerifiedDeclsCounter = 0;

  /// Clang compiler invocation.
  std::shared_ptr<clang::CompilerInvocation> Invocation;

  /// Clang compiler instance, which is used to actually load Clang
  /// modules.
  std::unique_ptr<clang::CompilerInstance> Instance;

  /// Clang compiler action, which is used to actually run the
  /// parser.
  std::unique_ptr<clang::FrontendAction> Action;

  /// Clang parser, which is used to load textual headers.
  std::unique_ptr<clang::Parser> Parser;

  /// Clang parser, which is used to load textual headers.
  std::unique_ptr<clang::MangleContext> Mangler;

  /// Clang arguments used to create the Clang invocation.
  std::vector<std::string> ClangArgs;

  /// Mapping from Clang swift_attr attribute text to the Swift source buffer
  /// IDs that contain that attribute text. These are re-used when parsing the
  /// Swift attributes on import.
  llvm::StringMap<unsigned> ClangSwiftAttrSourceBuffers;

  /// Mapping from modules in which a Clang swift_attr attribute occurs, to be
  /// used when parsing the attribute text.
  llvm::SmallDenseMap<ModuleDecl *, SourceFile *> ClangSwiftAttrSourceFiles;

public:
  /// The Swift lookup table for the bridging header.
  std::unique_ptr<SwiftLookupTable> BridgingHeaderLookupTable;

  /// Mapping of already-imported declarations.
  llvm::DenseMap<std::pair<const clang::Decl *, Version>, Decl *> ImportedDecls;

  /// The set of "special" typedef-name declarations, which are
  /// mapped to specific Swift types.
  ///
  /// Normal typedef-name declarations imported into Swift will maintain
  /// equality between the imported declaration's underlying type and the
  /// import of the underlying type. A typedef-name declaration is special
  /// when this is not the case, e.g., Objective-C's "BOOL" has an underlying
  /// type of "signed char", but is mapped to a special Swift struct type
  /// ObjCBool.
  llvm::SmallDenseMap<const clang::TypedefNameDecl *, MappedTypeNameKind, 16>
    SpecialTypedefNames;

  /// Provide a single extension point for any given type per clang
  /// submodule
  llvm::DenseMap<std::pair<NominalTypeDecl *, const clang::Module *>,
                 ExtensionDecl *> extensionPoints;

  /// Typedefs that we should not be importing.  We should be importing
  /// underlying decls instead.
  llvm::DenseSet<const clang::Decl *> SuperfluousTypedefs;

  /// Tag decls whose typedefs were imported instead.
  ///
  /// \sa SuperfluousTypedefs
  llvm::DenseSet<const clang::Decl *> DeclsWithSuperfluousTypedefs;

  /// Mapping of already-imported declarations from protocols, which
  /// can (and do) get replicated into classes.
  llvm::DenseMap<std::tuple<const clang::Decl *, DeclContext *, Version>,
                 Decl *> ImportedProtocolDecls;

  /// Mapping from identifiers to the set of macros that have that name along
  /// with their corresponding Swift declaration.
  ///
  /// Multiple macro definitions can map to the same declaration if the
  /// macros are identically defined.
  llvm::DenseMap<Identifier,
                 SmallVector<std::pair<const clang::MacroInfo *, ValueDecl *>,
                             2>>
    ImportedMacros;

  // Mapping from macro to value for macros that expand to constant values.
  llvm::DenseMap<const clang::MacroInfo *, std::pair<clang::APValue, Type>>
    ImportedMacroConstants;

  // Mapping from imported types to their raw value types.
  llvm::DenseMap<const NominalTypeDecl *, Type> RawTypes;

  clang::CompilerInstance *getClangInstance() {
    return Instance.get();
  }

private:
  /// The Importer may be configured to load modules of a different OS Version
  /// than the underlying Swift compilation. This is the `TargetOptions`
  /// corresponding to the instantiating Swift compilation's triple. These are
  /// to be used by all IRGen/CodeGen clients of `ClangImporter`.
  std::unique_ptr<clang::TargetInfo> CodeGenTargetInfo;
  std::unique_ptr<clang::CodeGenOptions> CodeGenOpts;

public:
  void setSwiftTargetInfo(clang::TargetInfo *SwiftTargetInfo) {
    CodeGenTargetInfo.reset(SwiftTargetInfo);
  }
  clang::TargetInfo *getSwiftTargetInfo() const {
    return CodeGenTargetInfo.get();
  }

  void setSwiftCodeGenOptions(clang::CodeGenOptions *SwiftCodeGenOpts) {
    CodeGenOpts.reset(SwiftCodeGenOpts);
  }
  clang::CodeGenOptions *getSwiftCodeGenOptions() const {
    return CodeGenOpts.get();
  }

private:
  /// Generation number that is used for crude versioning.
  ///
  /// This value is incremented every time a new module is imported.
  unsigned Generation = 1;

  void bumpGeneration() {
    ++Generation;
    SwiftContext.bumpGeneration();
  }

public:
  /// Keep track of subscript declarations based on getter/setter
  /// pairs.
  llvm::DenseMap<std::pair<FuncDecl *, FuncDecl *>, SubscriptDecl *> Subscripts;

  llvm::DenseMap<
      NominalTypeDecl *,
      llvm::DenseMap<llvm::StringRef, std::pair<FuncDecl *, FuncDecl *>>>
      GetterSetterMap;

  /// Keep track of getter/setter pairs for functions imported from C++
  /// subscript operators based on the type in which they are declared and
  /// the type of their parameter.
  ///
  /// `.first` corresponds to a getter
  /// `.second` corresponds to a setter
  llvm::MapVector<std::pair<NominalTypeDecl *, Type>,
                  std::pair<FuncDecl *, FuncDecl *>> cxxSubscripts;

  // Keep track of the decls that were already cloned for this specific class.
  llvm::DenseMap<std::pair<ValueDecl *, DeclContext *>, ValueDecl *>
      clonedBaseMembers;

  // Cache for already-specialized function templates and any thunks they may
  // have.
  llvm::DenseMap<clang::FunctionDecl *, ValueDecl *>
      specializedFunctionTemplates;

  /// Stores qualified names of C++ template specializations that were too deep
  /// to import into Swift.
  llvm::StringSet<> tooDeepTemplateSpecializations;

  /// Keeps track of the Clang functions that have been turned into
  /// properties.
  llvm::DenseMap<const clang::FunctionDecl *, VarDecl *> FunctionsAsProperties;

  importer::EnumInfo getEnumInfo(const clang::EnumDecl *decl) {
    return getNameImporter().getEnumInfo(decl);
  }
  importer::EnumKind getEnumKind(const clang::EnumDecl *decl) {
    return getNameImporter().getEnumKind(decl);
  }

private:
  /// A mapping from imported declarations to their "alternate" declarations,
  /// for cases where a single Clang declaration is imported to two
  /// different Swift declarations.
  llvm::DenseMap<Decl *, TinyPtrVector<ValueDecl *>> AlternateDecls;

public:
  /// Keep track of initializer declarations that correspond to
  /// imported methods.
  llvm::DenseMap<
      std::tuple<const clang::ObjCMethodDecl *, const DeclContext *, Version>,
      ConstructorDecl *> Constructors;

  /// Keep track of all initializers that have been imported into a
  /// nominal type.
  llvm::DenseMap<const NominalTypeDecl *, TinyPtrVector<ConstructorDecl *>>
      ConstructorsForNominal;

  /// Keep track of all member declarations that have been imported into
  /// a nominal type.
  llvm::DenseMap<const NominalTypeDecl *,
                 llvm::DenseMap<DeclBaseName,
                                TinyPtrVector<ValueDecl *>>>
      MembersForNominal;

  /// Keep track of the nested 'Code' enum for imported error wrapper
  /// structs.
  llvm::DenseMap<const StructDecl *, EnumDecl *> ErrorCodeEnums;

  /// Retrieve the alternative declaration for the given imported
  /// Swift declaration.
  ArrayRef<ValueDecl *> getAlternateDecls(Decl *decl) {
    auto known = AlternateDecls.find(decl);
    if (known == AlternateDecls.end()) return {};
    return known->second;
  }

  /// Add an alternative decl
  void addAlternateDecl(Decl *forDecl, ValueDecl *altDecl) {
    auto &vec = AlternateDecls[forDecl];
    for (auto alt : vec)
      if (alt == altDecl)
        return;
    vec.push_back(altDecl);
  }

private:
  /// NSObject, imported into Swift.
  Type NSObjectTy;

  /// A pair containing a ClangModuleUnit,
  /// and whether the overlays of its re-exported modules have all been forced
  /// to load already.
  using ModuleInitPair = llvm::PointerIntPair<ClangModuleUnit *, 1, bool>;

public:
  /// A map from Clang modules to their Swift wrapper modules.
  llvm::SmallDenseMap<const clang::Module *, ModuleInitPair, 16> ModuleWrappers;

  /// The module unit that contains declarations from imported headers.
  ClangModuleUnit *ImportedHeaderUnit = nullptr;

  /// The modules re-exported by imported headers.
  llvm::SmallVector<clang::Module *, 8> ImportedHeaderExports;

  /// The modules that requested imported headers.
  ///
  /// These are used to look up Swift classes forward-declared with \@class.
  TinyPtrVector<ModuleDecl *> ImportedHeaderOwners;

  /// Clang's objectAtIndexedSubscript: selector.
  clang::Selector objectAtIndexedSubscript;

  /// Clang's setObjectAt:indexedSubscript: selector.
  clang::Selector setObjectAtIndexedSubscript;

  /// Clang's objectForKeyedSubscript: selector.
  clang::Selector objectForKeyedSubscript;

  /// Clang's setObject:forKeyedSubscript: selector.
  clang::Selector setObjectForKeyedSubscript;

private:
  /// Records those modules that we have looked up.
  llvm::DenseMap<Identifier, ModuleDecl *> checkedModules;

  /// The set of imported protocols for a declaration, used only to
  /// load all members of the declaration.
  llvm::DenseMap<const Decl *, ArrayRef<ProtocolDecl *>>
    ImportedProtocols;

  /// The set of declaration context for which we've already ruled out the
  /// presence of globals-as-members.
  llvm::DenseSet<const IterableDeclContext *> checkedGlobalsAsMembers;

  void startedImportingEntity();

public:
  importer::PlatformAvailability platformAvailability;

private:
  /// For importing names. This is initialized by the ClangImporter::create()
  /// after having set up a suitable Clang instance.
  std::unique_ptr<importer::NameImporter> nameImporter = nullptr;

  /// If there is a single .PCH file imported into the __ObjC module, this
  /// is the filename of that PCH. When other files are imported, this should
  /// be llvm::None.
  Optional<std::string> SinglePCHImport = None;

public:
  importer::NameImporter &getNameImporter() {
    assert(nameImporter && "haven't finished initialization");
    return *nameImporter;
  }

  /// Tracks top level decls from the bridging header.
  std::vector<clang::Decl *> BridgeHeaderTopLevelDecls;
  std::vector<llvm::PointerUnion<clang::ImportDecl *, ImportDecl *>>
    BridgeHeaderTopLevelImports;

  /// Tracks macro definitions from the bridging header.
  std::vector<clang::IdentifierInfo *> BridgeHeaderMacros;
  /// Tracks included headers from the bridging header.
  llvm::DenseSet<clang::FileEntryRef> BridgeHeaderFiles;

  void addBridgeHeaderTopLevelDecls(clang::Decl *D);
  bool shouldIgnoreBridgeHeaderTopLevelDecl(clang::Decl *D);

private:
  /// When set, ClangImporter is disabled, and all requests go to the
  /// DWARFImporter delegate.
  bool DisableSourceImport;
  
  /// The DWARF importer delegate, if installed.
  DWARFImporterDelegate *DWARFImporter = nullptr;

public:
  /// Only used for testing.
  void setDWARFImporterDelegate(DWARFImporterDelegate &delegate);

private:
  /// The list of Clang modules found in the debug info.
  llvm::DenseMap<Identifier, LoadedFile *> DWARFModuleUnits;

  /// Load a module using the clang::CompilerInstance.
  ModuleDecl *loadModuleClang(SourceLoc importLoc,
                              ImportPath::Module path);
  
  /// "Load" a module from debug info. Because debug info types are read on
  /// demand, this doesn't really do any work.
  ModuleDecl *loadModuleDWARF(SourceLoc importLoc,
                              ImportPath::Module path);

public:
  /// Load a module using either method.
  ModuleDecl *loadModule(SourceLoc importLoc,
                         ImportPath::Module path);

  void recordImplicitUnwrapForDecl(ValueDecl *decl, bool isIUO) {
    if (!isIUO)
      return;

#if !defined(NDEBUG)
    Type ty;
    if (auto *FD = dyn_cast<FuncDecl>(decl)) {
      ty = FD->getResultInterfaceType();
    } else if (auto *CD = dyn_cast<ConstructorDecl>(decl)) {
      ty = CD->getResultInterfaceType();
    } else {
      ty = cast<AbstractStorageDecl>(decl)->getValueInterfaceType();
    }
    assert(ty->getOptionalObjectType());
#endif

    decl->setImplicitlyUnwrappedOptional(true);
  }

  /// Retrieve the Clang AST context.
  clang::ASTContext &getClangASTContext() const {
    return Instance->getASTContext();
  }

  /// Retrieve the Clang Sema object.
  clang::Sema &getClangSema() const {
    return Instance->getSema();
  }

  /// Retrieve the Clang AST context.
  clang::Preprocessor &getClangPreprocessor() const {
    return Instance->getPreprocessor();
  }
  
  clang::CodeGenOptions &getCodeGenOpts() const {
    return Instance->getCodeGenOpts();
  }

  importer::ClangSourceBufferImporter &getBufferImporterForDiagnostics() {
    return BuffersForDiagnostics;
  }

  /// Imports the given header contents into the Clang context.
  bool importHeader(ModuleDecl *adapter, StringRef headerName,
                    SourceLoc diagLoc, bool trackParsedSymbols,
                    std::unique_ptr<llvm::MemoryBuffer> contents,
                    bool implicitImport);

  /// Retrieve the imported module that should contain the given
  /// Clang decl.
  ClangModuleUnit *getClangModuleForDecl(const clang::Decl *D,
                                         bool allowForwardDeclaration = false);

  /// Returns the module \p MI comes from, or \c None if \p MI does not have
  /// a valid associated module.
  ///
  /// The returned module may be null (but not \c None) if \p MI comes from
  /// an imported header.
  const clang::Module *getClangOwningModule(ClangNode Node) const;

  /// Whether NSUInteger can be imported as Int in certain contexts. If false,
  /// should always be imported as UInt.
  static bool shouldAllowNSUIntegerAsInt(bool isFromSystemModule,
                                         const clang::NamedDecl *decl);

  /// Converts the given Swift identifier for Clang.
  clang::DeclarationName exportName(Identifier name);

  /// Imports the full name of the given Clang declaration into Swift.
  ///
  /// Note that this may result in a name very different from the Clang name,
  /// so it should not be used when referencing Clang symbols.
  ///
  /// \param D The Clang declaration whose name should be imported.
  importer::ImportedName importFullName(const clang::NamedDecl *D,
                                        Version version,
                                        clang::DeclarationName givenName =
                                          clang::DeclarationName()) {
    return getNameImporter().importName(D, version, givenName);
  }

  /// Print an imported name as a string suitable for the swift_name attribute,
  /// or the 'Rename' field of AvailableAttr.
  void printSwiftName(importer::ImportedName name,
                      importer::ImportNameVersion version,
                      bool fullyQualified,
                      llvm::raw_ostream &os);

  /// Emit a diagnostic, taking care not to interrupt a diagnostic that's
  /// already in flight.
  template<typename ...Args>
  void diagnose(Args &&...args) {
    // If we're in the middle of pretty-printing, suppress diagnostics.
    if (SwiftContext.Diags.isPrettyPrintingDecl()) {
      return;
    }

    SwiftContext.Diags.diagnose(std::forward<Args>(args)...);
  }

  /// Emit a diagnostic, taking care not to interrupt a diagnostic that's
  /// already in flight.
  template<typename ...Args>
  void diagnose(SourceLoc loc, Args &&...args) {
    // If we're in the middle of pretty-printing, suppress diagnostics.
    if (SwiftContext.Diags.isPrettyPrintingDecl()) {
      return;
    }

    SwiftContext.Diags.diagnose(loc, std::forward<Args>(args)...);
  }

  /// Emit a diagnostic at a clang source location, falling back to a Swift
  /// location if the clang one is invalid.
  ///
  /// The diagnostic will appear in the header file rather than in a generated
  /// interface. Use this to diagnose issues with declarations that are not
  /// imported or that are not reflected in a generated interface.
  template<typename ...Args>
  InFlightDiagnostic diagnose(HeaderLoc loc, Args &&...args) {
    // If we're in the middle of pretty-printing, suppress diagnostics.
    if (SwiftContext.Diags.isPrettyPrintingDecl()) {
      return InFlightDiagnostic();
    }

    auto swiftLoc = loc.fallbackLoc;
    if (loc.clangLoc.isValid()) {
      auto &clangSrcMgr = loc.sourceMgr ? *loc.sourceMgr
                        : getClangASTContext().getSourceManager();
      auto &bufferImporter = getBufferImporterForDiagnostics();
      swiftLoc = bufferImporter.resolveSourceLocation(clangSrcMgr,
                                                      loc.clangLoc);
    }

    return SwiftContext.Diags.diagnose(swiftLoc, std::forward<Args>(args)...);
  }

  void addImportDiagnostic(
      ImportDiagnosticTarget target, Diagnostic &&diag,
      clang::SourceLocation loc);

  /// Import the given Clang identifier into Swift.
  ///
  /// \param identifier The Clang identifier to map into Swift.
  ///
  /// \param removePrefix The prefix to remove from the Clang name to produce
  /// the Swift name. If the Clang name does not start with this prefix,
  /// nothing is removed.
  Identifier importIdentifier(const clang::IdentifierInfo *identifier,
                              StringRef removePrefix = "");

  /// Import an Objective-C selector.
  ObjCSelector importSelector(clang::Selector selector);

  /// Import a Swift name as a Clang selector.
  clang::Selector exportSelector(DeclName name, bool allowSimpleName = true);

  /// Export a Swift Objective-C selector as a Clang Objective-C selector.
  clang::Selector exportSelector(ObjCSelector selector);

  /// Import the given Swift source location into Clang.
  clang::SourceLocation exportSourceLoc(SourceLoc loc);

  /// Import the given Clang source location into Swift.
  SourceLoc importSourceLoc(clang::SourceLocation loc);

  /// Import the given Clang source range into Swift.
  SourceRange importSourceRange(clang::SourceRange loc);

  /// Import the given Clang preprocessor macro as a Swift value decl.
  ///
  /// \p macroNode must be a MacroInfo or a ModuleMacro.
  ///
  /// \returns The imported declaration, or null if the macro could not be
  /// translated into Swift.
  ValueDecl *importMacro(Identifier name, ClangNode macroNode);

  /// Map a Clang identifier name to its imported Swift equivalent.
  StringRef getSwiftNameFromClangName(StringRef name);

  /// Retrieve the Swift source buffer ID that corresponds to the given
  /// swift_attr attribute text, creating one if necessary.
  unsigned getClangSwiftAttrSourceBuffer(StringRef attributeText);

  /// Retrieve the placeholder source file for use in parsing Swift attributes
  /// in the given module.
  SourceFile &getClangSwiftAttrSourceFile(ModuleDecl &module);

  /// Import attributes from the given Clang declaration to its Swift
  /// equivalent.
  ///
  /// \param ClangDecl The decl being imported.
  /// \param MappedDecl The decl to attach attributes to.
  /// \param NewContext If present, the Clang node for the context the decl is
  /// being imported into, which may affect info from API notes.
  void importAttributes(const clang::NamedDecl *ClangDecl, Decl *MappedDecl,
                        const clang::ObjCContainerDecl *NewContext = nullptr);

  Type applyImportTypeAttrs(ImportTypeAttrs attrs, Type type,
                 llvm::function_ref<void(Diagnostic &&)> addImportDiagnosticFn);

  /// If we already imported a given decl, return the corresponding Swift decl.
  /// Otherwise, return nullptr.
  Optional<Decl *> importDeclCached(const clang::NamedDecl *ClangDecl,
                                    Version version,
                                    bool UseCanonicalDecl = true);

  Decl *importDeclImpl(const clang::NamedDecl *ClangDecl, Version version,
                       bool &TypedefIsSuperfluous, bool &HadForwardDeclaration);

  Decl *importDeclAndCacheImpl(const clang::NamedDecl *ClangDecl,
                               Version version,
                               bool SuperfluousTypedefsAreTransparent,
                               bool UseCanonicalDecl);

  /// Same as \c importDeclReal, but for use inside importer
  /// implementation.
  ///
  /// Unlike \c importDeclReal, this function for convenience transparently
  /// looks through superfluous typedefs and returns the imported underlying
  /// decl in that case.
  Decl *importDecl(const clang::NamedDecl *ClangDecl, Version version,
                   bool UseCanonicalDecl = true) {
    return importDeclAndCacheImpl(ClangDecl, version,
                                  /*SuperfluousTypedefsAreTransparent=*/true,
                                  /*UseCanonicalDecl*/ UseCanonicalDecl);
  }

  /// Import the given Clang declaration into Swift.  Use this function
  /// outside of the importer implementation, when importing a decl requested by
  /// Swift code.
  ///
  /// \returns The imported declaration, or null if this declaration could
  /// not be represented in Swift.
  Decl *importDeclReal(const clang::NamedDecl *ClangDecl, Version version,
                       bool useCanonicalDecl = true) {
    return importDeclAndCacheImpl(ClangDecl, version,
                                  /*SuperfluousTypedefsAreTransparent=*/false,
                                  /*UseCanonicalDecl*/ useCanonicalDecl);
  }

  /// Import a cloned version of the given declaration, which is part of
  /// an Objective-C protocol and currently must be a method or property, into
  /// the given declaration context.
  ///
  /// \returns The imported declaration, or null if this declaration could not
  /// be represented in Swift.
  Decl *importMirroredDecl(const clang::NamedDecl *decl, DeclContext *dc,
                           Version version, ProtocolDecl *proto);

  void importInheritedConstructors(const clang::ObjCInterfaceDecl *curObjCClass,
                                   const ClassDecl *classDecl,
                                   SmallVectorImpl<Decl *> &newMembers);
  void importMirroredProtocolMembers(const clang::ObjCContainerDecl *decl,
                                     DeclContext *dc, Optional<DeclBaseName> name,
                                     SmallVectorImpl<Decl *> &members);

  /// Utility function for building simple generic signatures.
  GenericSignature buildGenericSignature(GenericParamList *genericParams,
                                          DeclContext *dc);

  /// Import the given Clang declaration context into Swift.
  ///
  /// Usually one will use \c importDeclContextOf instead.
  ///
  /// \returns The imported declaration context, or null if it could not
  /// be converted.
  DeclContext *importDeclContextImpl(const clang::Decl *ImportingDecl,
                                     const clang::DeclContext *dc);

private:
  /// Declarations currently being imported by \c importDeclForDeclContext().
  /// Used to break cycles when a swift_name attribute is circular in a way that
  /// can't be resolved, or there is some other cycle through
  /// \c importDeclContextOf().
  llvm::SmallVector<std::tuple<const clang::Decl *, StringRef,
                               const clang::NamedDecl *, Version, bool>, 8>
      contextDeclsBeingImported;

  /// Records which contexts \c importDeclForDeclContext() has already warned
  /// were unimportable.
  llvm::SmallPtrSet<const clang::NamedDecl *, 4> contextDeclsWarnedAbout;

  /// Exactly equivalent to \c importDecl(), except with additional
  /// cycle-breaking code.
  ///
  /// \param writtenName The name that should be used for the declaration
  ///        in cycle diagnostics.
  Decl *importDeclForDeclContext(const clang::Decl *ImportingDecl,
                                 StringRef writtenName,
                                 const clang::NamedDecl *ClangDecl,
                                 Version version,
                                 bool UseCanonicalDecl = true);

public:
  /// Import the declaration context of a given Clang declaration into
  /// Swift.
  ///
  /// \param context The effective context as determined by importFullName.
  ///
  /// \returns The imported declaration context, or null if it could not
  /// be converted.
  DeclContext *importDeclContextOf(const clang::Decl *D,
                                   EffectiveClangContext context);

  /// Determine whether the given declaration is considered
  /// 'unavailable' in Swift.
  bool isUnavailableInSwift(const clang::Decl *decl) {
    return importer::isUnavailableInSwift(
        decl, &platformAvailability, SwiftContext.LangOpts.EnableObjCInterop);
  }

  /// Add "Unavailable" annotation to the swift declaration.
  void markUnavailable(ValueDecl *decl, StringRef unavailabilityMsg);

  /// Create a decl with error type and an "unavailable" attribute on it
  /// with the specified message.
  ValueDecl *createUnavailableDecl(Identifier name, DeclContext *dc,
                                   Type type, StringRef UnavailableMessage,
                                   bool isStatic, ClangNode ClangN);

  /// Add a synthesized typealias to the given nominal type.
  void addSynthesizedTypealias(NominalTypeDecl *nominal, Identifier name,
                               Type underlyingType);

  void addSynthesizedProtocolAttrs(
      NominalTypeDecl *nominal,
      ArrayRef<KnownProtocolKind> synthesizedProtocolAttrs,
      bool isUnchecked = false);

  void makeComputed(AbstractStorageDecl *storage, AccessorDecl *getter,
                    AccessorDecl *setter);

  /// Retrieve the standard library module.
  ModuleDecl *getStdlibModule();

  /// Retrieve the named module.
  ///
  /// \param name The name of the module.
  ///
  /// \returns The named module, or null if the module has not been imported.
  ModuleDecl *getNamedModule(StringRef name);

  /// Returns the "Foundation" module, if it can be loaded.
  ///
  /// After this has been called, the Foundation module will or won't be loaded
  /// into the ASTContext.
  ModuleDecl *tryLoadFoundationModule();

  /// Returns whether or not the "Foundation" module can be imported, without loading it.
  bool canImportFoundationModule();

  /// Retrieves the Swift wrapper for the given Clang module, creating
  /// it if necessary.
  ClangModuleUnit *getWrapperForModule(const clang::Module *underlying,
                                       SourceLoc importLoc = SourceLoc());

  /// Constructs a Swift module for the given Clang module.
  ModuleDecl *finishLoadingClangModule(const clang::Module *clangModule,
                                       SourceLoc importLoc);

  /// Call finishLoadingClangModule on each deferred import collected
  /// while scanning a bridging header or PCH.
  void handleDeferredImports(SourceLoc diagLoc);

  /// Retrieve the named Swift type, e.g., Int32.
  ///
  /// \param moduleName The name of the module in which the type should occur.
  ///
  /// \param name The name of the type to find.
  ///
  /// \returns The named type, or null if the type could not be found.
  Type getNamedSwiftType(StringRef moduleName, StringRef name);

  /// Retrieve the named Swift type, e.g., Int32.
  ///
  /// \param module The module in which the type should occur.
  ///
  /// \param name The name of the type to find.
  ///
  /// \returns The named type, or null if the type could not be found.
  Type getNamedSwiftType(ModuleDecl *module, StringRef name);

  /// Retrieve the NSObject type.
  Type getNSObjectType();

  /// Retrieve the NSObject protocol type.
  Type getNSObjectProtocolType();

  /// Determines whether the given type matches an implicit type
  /// bound of "Hashable", which is used to validate NSDictionary/NSSet.
  bool matchesHashableBound(Type type);

  /// Determines whether the type declared by the given declaration
  /// is over-aligned.
  bool isOverAligned(const clang::TypeDecl *typeDecl);
  bool isOverAligned(clang::QualType type);

  /// Determines whether the given Clang type is serializable in a
  /// Swift AST.  This should only be called after successfully importing
  /// the type, because it will look for a stable serialization path for any
  /// referenced declarations, which may depend on whether there's a known
  /// import of it.  (It will not try to import the declaration to avoid
  /// circularity problems.)
  ///
  /// Note that this will only check the requested sugaring of the given
  /// type (depending on \c checkCanonical); the canonical type may be
  /// serializable even if the non-canonical type is not, or vice-versa.
  bool isSerializable(clang::QualType type, bool checkCanonical);

  /// Try to find a stable Swift serialization path for the given Clang
  /// declaration.
  StableSerializationPath findStableSerializationPath(const clang::Decl *decl);

  /// Look up and attempt to import a Clang declaration with
  /// the given name.
  Decl *importDeclByName(StringRef name);

  /// Import the given Clang type into Swift.
  ///
  /// \param type The Clang type to import.
  ///
  /// \param kind A classification of the immediate context in which this type
  ///   will be used. Different contexts result in the type being imported
  ///   differently; for example, CF types are normally considered Unmanaged,
  ///   but in parameter position they are known to always be passed at +0.
  ///   See also the \p topLevelBridgeability parameter.
  ///
  /// \param addImportDiagnosticFn A function that can be called to add import
  ///   diagnostics to the declaration being imported. This can be any lambda or
  ///   callable object, but it's designed to be compatible with
  ///   \c ImportDiagnosticAdder .
  ///
  /// \param allowNSUIntegerAsInt If true, NSUInteger will be imported as Int
  ///   in certain contexts. If false, it will always be imported as UInt.
  ///
  /// \param topLevelBridgeability A classification of the top-level context in
  ///   which this type will be used. This and \p kind are used together to
  ///   determine whether a type can be imported in a more Swifty way than
  ///   a naive translation of its C type. Full bridgeability requires that SIL
  ///   can get back to the original Clang type if it needs to, which implies
  ///   that this type is part of a top-level declaration where we do bridging.
  ///   Without full bridgeability, we can still do some Swifty importing (e.g.
  ///   mapping NSString to String) if we're in an immediate context \p kind
  ///   that allows bridging, but only in cases where Swift's default mapping
  ///   "back" to C is the correct one. If the original type has something
  ///   funny going on, we either have to use a less lossy version of the type
  ///   (ObjCBool rather than Bool) or refuse to import it at all (a block with
  ///   the \c ns_returns_retained attribute).
  ///
  /// \param attrs Attributes extracted from the declaration containing the type
  ///   that should be applied to it. This should usually generated by applying
  ///   \c getImportTypeAttrs() to the declaration.
  ///
  /// \param optional If the imported type was a pointer-like type in C, this
  ///   optionality is applied to the resulting Swift type.
  ///
  /// \param resugarNSErrorPointer If true, Objective-C's `NSError **` is
  ///   imported as Foundation.NSErrorPointer rather than
  ///   AutoreleasingUnsafeMutablePointer<...>. This is usually desirable
  ///   behavior, but isn't necessary when we use Swift's \c throws anyway.
  ///   Strictly speaking, though, this is a hack used to break cyclic
  ///   dependencies.
  ///
  /// \returns An ImportedType value which holds the imported type. If
  ///          this type is an Optional, it also has a flag which
  ///          indicates if the Optional is implicitly unwrapped. If
  ///          the type cannot be represented in Swift, then the type
  ///          field will be null.
  ImportedType
  importType(clang::QualType type, ImportTypeKind kind,
             llvm::function_ref<void(Diagnostic &&)> addImportDiagnosticFn,
             bool allowNSUIntegerAsInt, Bridgeability topLevelBridgeability,
             ImportTypeAttrs attrs,
             OptionalTypeKind optional = OTK_ImplicitlyUnwrappedOptional,
             bool resugarNSErrorPointer = true,
             Optional<unsigned> completionHandlerErrorParamIndex = None);

  /// Import the given Clang type into Swift.
  ///
  /// For a description of parameters, see importType(). This differs
  /// only in that it returns a Type rather than ImportedType, which
  /// means that we do not retain the information of whether the type
  /// returned might be an implicitly unwrapped optional.
  ///
  /// \returns The imported type, or null if this type could not be
  ///   represented in Swift.
  Type importTypeIgnoreIUO(
      clang::QualType type, ImportTypeKind kind,
      llvm::function_ref<void(Diagnostic &&)> addImportDiagnosticFn,
      bool allowNSUIntegerAsInt, Bridgeability topLevelBridgeability,
      ImportTypeAttrs attrs,
      OptionalTypeKind optional = OTK_ImplicitlyUnwrappedOptional,
      bool resugarNSErrorPointer = true);

  /// Import the given Clang type into Swift, returning the
  /// Swift parameters and result type and whether we should treat it
  /// as an optional that is implicitly unwrapped.
  ///
  /// The parameters are returned via \c parameterList, and the result type is
  /// the return type of this method.
  ///
  /// \returns A pair of the imported result type and whether we should treat
  /// it as an optional that is implicitly unwrapped. The returned
  /// type is null if it cannot be represented in Swift.

  /// Import the given function type.
  ///
  /// This routine should be preferred when importing function types for
  /// which we have actual function parameters, e.g., when dealing with a
  /// function declaration, because it produces a function type whose input
  /// tuple has argument names.
  ///
  /// \param dc The context the function is being imported into.
  /// \param clangDecl The underlying declaration, if any; should only be
  ///   considered for any attributes it might carry.
  /// \param params The parameter types to the function.
  /// \param isVariadic Whether the function is variadic.
  /// \param isFromSystemModule Whether to apply special rules that only apply
  ///   to system APIs.
  /// \param name The name of the function.
  /// \param[out] parameterList The parameters visible inside the function body.
  ImportedType importFunctionParamsAndReturnType(
      DeclContext *dc, const clang::FunctionDecl *clangDecl,
      ArrayRef<const clang::ParmVarDecl *> params, bool isVariadic,
      bool isFromSystemModule, DeclName name, ParameterList *&parameterList,
      ArrayRef<GenericTypeParamDecl *> genericParams);

  /// Import the given function return type.
  ///
  /// \param dc The context the function is being imported into.
  /// \param clangDecl The underlying declaration, if any; should only be
  ///   considered for any attributes it might carry.
  /// \param allowNSUIntegerAsInt If true, NSUInteger will be imported as Int
  ///        in certain contexts. If false, it will always be imported as UInt.
  ///
  /// \returns the imported function return type, or null if the type cannot be
  /// imported.
  ImportedType importFunctionReturnType(DeclContext *dc,
                                        const clang::FunctionDecl *clangDecl,
                                        bool allowNSUIntegerAsInt);

  ImportedType importFunctionReturnType(const clang::FunctionDecl *clangDecl,
                                        DeclContext *dc);

  /// Import the parameter list for a function
  ///
  /// \param clangDecl The underlying declaration, if any; should only be
  ///   considered for any attributes it might carry.
  /// \param params The parameter types to the function.
  /// \param isVariadic Whether the function is variadic.
  /// \param allowNSUIntegerAsInt If true, NSUInteger will be imported as Int
  ///        in certain contexts. If false, it will always be imported as UInt.
  /// \param argNames The argument names
  ///
  /// \returns The imported parameter list on success, or null on failure
  ParameterList *importFunctionParameterList(
      DeclContext *dc, const clang::FunctionDecl *clangDecl,
      ArrayRef<const clang::ParmVarDecl *> params, bool isVariadic,
      bool allowNSUIntegerAsInt, ArrayRef<Identifier> argNames,
      ArrayRef<GenericTypeParamDecl *> genericParams, Type resultType);

  struct ImportParameterTypeResult {
    /// The imported parameter Swift type.
    swift::Type swiftTy;
    /// If the parameter is or not inout.
    bool isInOut;
    /// If the parameter is implicitly unwrapped or not.
    bool isParamTypeImplicitlyUnwrapped;
  };

  /// Import a parameter type
  ///
  /// \param param The underlaying parameter declaraction.
  /// \param optionalityOfParam The kind of optionality for the parameter
  ///        being imported.
  /// \param allowNSUIntegerAsInt If true, NSUInteger will be import as Int
  ///        in certain contexts. If false, it will always be import as UInt.
  /// \param isNSDictionarySubscriptGetter If true, the parameter is being
  ///        imported as part of an NSDictionary subscript getter. If false,
  ///        the parameter belongs to some other kind of method/function.
  /// \param paramIsError If true, the parameter being imported is an NSError
  ///        parameter. If false, the parameter is not an error parameter.
  /// \param paramIsCompletionHandler If true, the parameter being imported is
  ///        a completion handler. If false, the parameter is not a completion
  ///        handler.
  /// \param completionHandlerErrorParamIndex If it contains a value, the value
  ///        indicates the index of the completion handler whose error
  ///        parameter is used to indicate throwing. If None, the function does
  ///        not have such parameter.
  /// \param genericParams For C++ functions, an array of the generic type
  ///        parameters of the function. For the rest of cases, an empty array
  ///        can be provided.
  /// \param addImportDiagnosticFn A function that can be called to add import
  ///        diagnostics to the declaration being imported. This can be any
  ///        lambda or callable object, but it's designed to be compatible
  ///        with \c ImportDiagnosticAdder .
  ///
  /// \returns The imported parameter result on success, or None on failure.
  Optional<ImportParameterTypeResult> importParameterType(
      const clang::ParmVarDecl *param, OptionalTypeKind optionalityOfParam,
      bool allowNSUIntegerAsInt, bool isNSDictionarySubscriptGetter,
      bool paramIsError, bool paramIsCompletionHandler,
      Optional<unsigned> completionHandlerErrorParamIndex,
      ArrayRef<GenericTypeParamDecl *> genericParams,
      llvm::function_ref<void(Diagnostic &&)> addImportDiagnosticFn);

  ImportedType importPropertyType(const clang::ObjCPropertyDecl *clangDecl,
                                  bool isFromSystemModule);

  /// Determines what the type of an effectful, computed read-only property
  /// would be, if the given method were imported as such a property.
  ImportedType importEffectfulPropertyType(const clang::ObjCMethodDecl *decl,
                                            DeclContext *dc,
                                            importer::ImportedName name,
                                            bool isFromSystemModule);

  /// Attempt to infer a default argument for a parameter with the
  /// given Clang \c type, \c baseName, and optionality.
  static ArgumentAttrs
  inferDefaultArgument(clang::QualType type, OptionalTypeKind clangOptionality,
                       DeclBaseName baseName, StringRef argumentLabel,
                       bool isFirstParameter, bool isLastParameter,
                       importer::NameImporter &nameImporter);

  /// Import the parameter and return types of an Objective-C method.
  ///
  /// The parameters are returned via \c bodyParams, and the result type is
  /// the return type of this method.
  ///
  /// Note that this is not appropriate to use for property accessor methods.
  /// Use #importAccessorParamsAndReturnType instead.
  ///
  /// \param dc The context the method is being imported into.
  /// \param clangDecl The underlying declaration.
  /// \param params The parameter types to the function. Note that this may not
  ///   include all parameters defined on the ObjCMethodDecl.
  /// \param isVariadic Whether the function is variadic.
  /// \param isFromSystemModule Whether to apply special rules that only apply
  ///   to system APIs.
  /// \param[out] bodyParams The patterns visible inside the function body.
  /// \param importedName How to import the name of the method.
  /// \param[out] errorConv Whether and how the method throws NSErrors.
  /// \param kind Controls whether we're building a type for a method that
  ///   needs special handling.
  ///
  /// \returns the imported result type, or null if the type cannot be
  /// imported.
  ImportedType
  importMethodParamsAndReturnType(const DeclContext *dc,
                                  const clang::ObjCMethodDecl *clangDecl,
                                  ArrayRef<const clang::ParmVarDecl *> params,
                                  bool isVariadic,
                                  bool isFromSystemModule,
                                  ParameterList **bodyParams,
                                  importer::ImportedName importedName,
                                  Optional<ForeignAsyncConvention> &asyncConv,
                                  Optional<ForeignErrorConvention> &errorConv,
                                  SpecialMethodKind kind);

  /// Import the type of an Objective-C method that will be imported as an
  /// accessor for \p property.
  ///
  /// The parameters are returned via \c bodyParams, and the result type is
  /// the return type of this method.
  ///
  /// \param dc The context the method is being imported into.
  /// \param property The property the method will be an accessor for.
  /// \param clangDecl The underlying declaration.
  /// \param isFromSystemModule Whether to apply special rules that only apply
  ///   to system APIs.
  /// \param importedName How to import the name of the method. This is still
  ///   important to satisfy the AST verifier, even though the method is an
  ///   accessor.
  /// \param[out] params The patterns visible inside the function body.
  ///
  /// \returns the imported result type, or null if the type cannot be
  /// imported.
  ImportedType
  importAccessorParamsAndReturnType(const DeclContext *dc,
                                    const clang::ObjCPropertyDecl *property,
                                    const clang::ObjCMethodDecl *clangDecl,
                                    bool isFromSystemModule,
                                    importer::ImportedName importedName,
                                    ParameterList **params);

  /// Determine whether the given typedef-name is "special", meaning
  /// that it has performed some non-trivial mapping of its underlying type
  /// based on the name of the typedef.
  Optional<MappedTypeNameKind>
  getSpecialTypedefKind(clang::TypedefNameDecl *decl);

  /// Look up a name, accepting only typedef results.
  const clang::TypedefNameDecl *lookupTypedef(clang::DeclarationName);

  /// Return whether a global of the given type should be imported as a
  /// 'let' declaration as opposed to 'var'.
  bool shouldImportGlobalAsLet(clang::QualType type);

  /// Record the set of imported protocols for the given declaration,
  /// to be used by member loading.
  ///
  /// FIXME: This is all a hack; we should have lazier deserialization
  /// of protocols separate from their conformances.
  void recordImportedProtocols(Decl *decl,
                               ArrayRef<ProtocolDecl *> protocols) {
    // Nothing to do for protocols.
    if (isa<ProtocolDecl>(decl)) return;

    if (protocols.empty())
      return;

    ImportedProtocols[decl] = SwiftContext.AllocateCopy(protocols);

    if (auto nominal = dyn_cast<NominalTypeDecl>(decl)) {
      nominal->setConformanceLoader(this, 0);
    } else {
      auto ext = cast<ExtensionDecl>(decl);
      ext->setConformanceLoader(this, 0);
    }
  }

  /// Retrieve the imported protocols for the given declaration.
  ArrayRef<ProtocolDecl *> getImportedProtocols(const Decl *decl) {
    auto known = ImportedProtocols.find(decl);
    if (known != ImportedProtocols.end())
      return known->second;
    return ArrayRef<ProtocolDecl *>();
  }

  EnumDecl *lookupErrorCodeEnum(const StructDecl *errorWrapper) {
    auto found = ErrorCodeEnums.find(errorWrapper);
    if (found == ErrorCodeEnums.end())
      return nullptr;
    return found->second;
  }

  virtual void
  loadAllMembers(Decl *D, uint64_t unused) override;

  virtual TinyPtrVector<ValueDecl *>
  loadNamedMembers(const IterableDeclContext *IDC, DeclBaseName N,
                   uint64_t contextData) override;

private:
  void
  loadAllMembersOfObjcContainer(Decl *D,
                                const clang::ObjCContainerDecl *objcContainer);
  void loadAllMembersOfRecordDecl(NominalTypeDecl *swiftDecl,
                                  const clang::RecordDecl *clangRecord);

  void collectMembersToAdd(const clang::ObjCContainerDecl *objcContainer,
                           Decl *D, DeclContext *DC,
                           SmallVectorImpl<Decl *> &members);
  void insertMembersAndAlternates(const clang::NamedDecl *nd,
                                  SmallVectorImpl<Decl *> &members);
  void loadAllMembersIntoExtension(Decl *D, uint64_t extra);

  /// Imports \p decl under \p nameVersion with the name \p newName, and adds
  /// it and its alternates to \p ext.
  ///
  /// \returns true if \p decl was successfully imported, whether or not it was
  /// ultimately added to \p ext. This matches the behavior of
  /// forEachDistinctName's callback.
  bool addMemberAndAlternatesToExtension(
      clang::NamedDecl *decl, importer::ImportedName newName,
      importer::ImportNameVersion nameVersion, ExtensionDecl *ext);

public:
  void
  loadAllConformances(
    const Decl *D, uint64_t contextData,
    SmallVectorImpl<ProtocolConformance *> &Conformances) override;

  void finishNormalConformance(NormalProtocolConformance *conformance,
                               uint64_t unused) override;
  
  /// Returns the default definition type for \p ATD.
  Type loadAssociatedTypeDefault(const AssociatedTypeDecl *ATD,
                                 uint64_t contextData) override {
    llvm_unreachable("unimplemented for ClangImporter");
  }

  ValueDecl *
  loadDynamicallyReplacedFunctionDecl(const DynamicReplacementAttr *DRA,
                                      uint64_t contextData) override {
    llvm_unreachable("unimplemented for ClangImporter");
  }

  AbstractFunctionDecl *
  loadReferencedFunctionDecl(const DerivativeAttr *DA,
                             uint64_t contextData) override {
    llvm_unreachable("unimplemented for ClangImporter");
  }

  Type loadTypeEraserType(const TypeEraserAttr *TRA,
                          uint64_t contextData) override {
    llvm_unreachable("unimplemented for ClangImporter");
  }

  ValueDecl *loadTargetFunctionDecl(const SpecializeAttr *attr,
                                    uint64_t contextData) override {
    llvm_unreachable("unimplemented for ClangImporter");
  }

  void loadRequirementSignature(const ProtocolDecl *decl, uint64_t contextData,
                                SmallVectorImpl<Requirement> &reqs,
                                SmallVectorImpl<ProtocolTypeAlias> &typeAliases) override {
    llvm_unreachable("unimplemented for ClangImporter");
  }

  void loadAssociatedTypes(
      const ProtocolDecl *decl, uint64_t contextData,
      SmallVectorImpl<AssociatedTypeDecl *> &assocTypes) override {
    llvm_unreachable("unimplemented for ClangImporter");
  }

  void loadPrimaryAssociatedTypes(
      const ProtocolDecl *decl, uint64_t contextData,
      SmallVectorImpl<AssociatedTypeDecl *> &assocTypes) override {
    llvm_unreachable("unimplemented for ClangImporter");
  }

  template <typename DeclTy, typename ...Targs>
  DeclTy *createDeclWithClangNode(ClangNode ClangN, AccessLevel access,
                                  Targs &&... Args) {
    assert(ClangN);
    void *DeclPtr = allocateMemoryForDecl<DeclTy>(SwiftContext, sizeof(DeclTy),
                                                  true);
    auto D = ::new (DeclPtr) DeclTy(std::forward<Targs>(Args)...);
    D->setClangNode(ClangN);
    D->setAccess(access);
    if (auto ASD = dyn_cast<AbstractStorageDecl>(D))
      ASD->setSetterAccess(access);

    // SwiftAttrs on ParamDecls are interpreted by applyParamAttributes().
    if (!isa<ParamDecl>(D))
      importSwiftAttrAttributes(D);

    return D;
  }

  void importSwiftAttrAttributes(Decl *decl);

  /// Find the lookup table that corresponds to the given Clang module.
  ///
  /// \param clangModule The module, or null to indicate that we're talking
  /// about the directly-parsed headers.
  SwiftLookupTable *findLookupTable(const clang::Module *clangModule);

  /// Find the lookup table that should contain the given Clang declaration.
  SwiftLookupTable *findLookupTable(const clang::Decl *decl);

  /// Visit each of the lookup tables in some deterministic order.
  ///
  /// \param fn Invoke the given visitor for each table. If the
  /// visitor returns true, stop early.
  ///
  /// \returns \c true if the \c visitor ever returns \c true, \c
  /// false otherwise.
  bool forEachLookupTable(llvm::function_ref<bool(SwiftLookupTable &table)> fn);

  /// Determine whether the given Clang entry is visible.
  ///
  /// FIXME: this is an elaborate hack to badly reflect Clang's
  /// submodule visibility into Swift.
  bool isVisibleClangEntry(const clang::NamedDecl *clangDecl);
  bool isVisibleClangEntry(SwiftLookupTable::SingleEntry entry);

  /// Look for namespace-scope values with the given name in the given
  /// Swift lookup table.
  bool lookupValue(SwiftLookupTable &table, DeclName name,
                   VisibleDeclConsumer &consumer);

  /// Look for namespace-scope values with the given name using the
  /// DWARFImporterDelegate.
  /// \param inModule only return results from this module.
  void lookupValueDWARF(DeclName name, NLKind lookupKind, Identifier inModule,
                        SmallVectorImpl<ValueDecl *> &results);

  /// Look for top-level scope types with a name and kind using the
  /// DWARFImporterDelegate.
  void lookupTypeDeclDWARF(StringRef rawName, ClangTypeKind kind,
                           llvm::function_ref<void(TypeDecl *)> receiver);

  /// Look for namespace-scope values in the given Swift lookup table.
  void lookupVisibleDecls(SwiftLookupTable &table,
                          VisibleDeclConsumer &consumer);

  /// Look for Objective-C members with the given name in the given
  /// Swift lookup table.
  void lookupObjCMembers(SwiftLookupTable &table, DeclName name,
                         VisibleDeclConsumer &consumer);

  /// Look for all Objective-C members in the given Swift lookup table.
  void lookupAllObjCMembers(SwiftLookupTable &table,
                            VisibleDeclConsumer &consumer);

  /// Emits diagnostics for any declarations named name
  /// whose direct declaration context is a TU.
  void diagnoseTopLevelValue(const DeclName &name);

  /// Emit diagnostics for declarations named name that are members
  /// of the provided container.
  void diagnoseMemberValue(const DeclName &name,
                           const clang::DeclContext *container);

  /// Emit any import diagnostics associated with the given Clang node.
  void diagnoseTargetDirectly(ImportDiagnosticTarget target);

private:
  ImportDiagnosticTarget importDiagnosticTargetFromLookupTableEntry(
      SwiftLookupTable::SingleEntry entry);

  bool emitDiagnosticsForTarget(
      ImportDiagnosticTarget target,
      clang::SourceLocation fallbackLoc = clang::SourceLocation());

public:
  /// Determine the effective Clang context for the given Swift nominal type.
  EffectiveClangContext
  getEffectiveClangContext(const NominalTypeDecl *nominal);

  /// Attempts to import the name of \p decl with each possible
  /// ImportNameVersion. \p action will be called with each unique name.
  ///
  /// In this case, "unique" means either the full name is distinct or the
  /// effective context is distinct. This method does not attempt to handle
  /// "unresolved" contexts in any special way---if one name references a
  /// particular Clang declaration and the other has an unresolved context that
  /// will eventually reference that declaration, the contexts will still be
  /// considered distinct.
  ///
  /// If \p action returns false, the current name will \e not be added to the
  /// set of seen names.
  ///
  /// The active name is always first, followed by the other names in the order
  /// of ImportNameVersion::forEachOtherImportNameVersion.
  void forEachDistinctName(
      const clang::NamedDecl *decl,
      llvm::function_ref<bool(importer::ImportedName,
                              importer::ImportNameVersion)> action) {
    getNameImporter().forEachDistinctImportName(decl, CurrentVersion, action);
  }

  /// Dump the Swift-specific name lookup tables we generate.
  void dumpSwiftLookupTables();

  void setSinglePCHImport(Optional<std::string> PCHFilename) {
    if (PCHFilename.has_value()) {
      assert(llvm::sys::path::extension(PCHFilename.value())
                 .endswith(file_types::getExtension(file_types::TY_PCH)) &&
             "Single PCH imported filename doesn't have .pch extension!");
    }
    SinglePCHImport = PCHFilename;
  }

  /// If there was is a single .pch bridging header without other imported
  /// files, we can provide the PCH filename for declaration caching,
  /// especially in code completion.
  StringRef getSinglePCHImport() const {
    if (SinglePCHImport.has_value())
      return *SinglePCHImport;
    return StringRef();
  }

  /// Returns true if the given C/C++ record should be imported as a reference
  /// type into Swift.
  static bool recordHasReferenceSemantics(const clang::RecordDecl *decl,
                                          ASTContext &ctx);
};

class ImportDiagnosticAdder {
  ClangImporter::Implementation &impl;
  ImportDiagnosticTarget target;
  const clang::SourceLocation loc;

public:
  ImportDiagnosticAdder(
      ClangImporter::Implementation &impl, ImportDiagnosticTarget target,
      clang::SourceLocation loc)
      : impl(impl), target(target), loc(loc) {}

  void operator () (Diagnostic &&diag) {
    impl.addImportDiagnostic(target, std::move(diag), loc);
  }
};

namespace importer {

/// Whether this is a forward declaration of a type. We ignore forward
/// declarations in certain cases, and instead process the real declarations.
bool isForwardDeclOfType(const clang::Decl *decl);

/// Whether we should suppress the import of the given Clang declaration.
bool shouldSuppressDeclImport(const clang::Decl *decl);

/// Identifies certain UIKit constants that used to have overlay equivalents,
/// but are now renamed using the swift_name attribute.
bool isSpecialUIKitStructZeroProperty(const clang::NamedDecl *decl);

/// \returns true if \p a has the same underlying type as \p b after removing
/// any pointer/reference specifiers. Note that this does not currently look through
/// nested types other than pointers or references.
bool hasSameUnderlyingType(const clang::Type *a,
                           const clang::TemplateTypeParmDecl *b);

/// Add command-line arguments for a normal import of Clang code.
void getNormalInvocationArguments(std::vector<std::string> &invocationArgStrs,
                                  ASTContext &ctx);

/// Add command-line arguments common to all imports of Clang code.
void addCommonInvocationArguments(std::vector<std::string> &invocationArgStrs,
                                  ASTContext &ctx,
                                  bool ignoreClangTarget);

/// Finds a particular kind of nominal by looking through typealiases.
template <typename T>
static T *dynCastIgnoringCompatibilityAlias(Decl *D) {
  static_assert(std::is_base_of<NominalTypeDecl, T>::value,
                "only meant for use with NominalTypeDecl and subclasses");
  if (auto *alias = dyn_cast_or_null<TypeAliasDecl>(D)) {
    if (!alias->isCompatibilityAlias())
      return nullptr;
    D = alias->getDeclaredInterfaceType()->getAnyNominal();
  }
  return dyn_cast_or_null<T>(D);
}

/// Finds a particular kind of nominal by looking through typealiases.
template <typename T>
static T *castIgnoringCompatibilityAlias(Decl *D) {
  static_assert(std::is_base_of<NominalTypeDecl, T>::value,
                "only meant for use with NominalTypeDecl and subclasses");
  if (auto *alias = dyn_cast_or_null<TypeAliasDecl>(D)) {
    assert(alias->isCompatibilityAlias() &&
           "non-compatible typealias found where nominal was expected");
    D = alias->getDeclaredInterfaceType()->getAnyNominal();
  }
  return cast_or_null<T>(D);
}

class SwiftNameLookupExtension : public clang::ModuleFileExtension {
  std::unique_ptr<SwiftLookupTable> &pchLookupTable;
  LookupTableMap &lookupTables;
  ASTContext &swiftCtx;
  ClangSourceBufferImporter &buffersForDiagnostics;
  const PlatformAvailability &availability;

public:
  SwiftNameLookupExtension(std::unique_ptr<SwiftLookupTable> &pchLookupTable,
                           LookupTableMap &tables, ASTContext &ctx,
                           ClangSourceBufferImporter &buffersForDiagnostics,
                           const PlatformAvailability &avail)
      : // Update in response to D97702 landing.
        clang::ModuleFileExtension(),
        pchLookupTable(pchLookupTable), lookupTables(tables), swiftCtx(ctx),
        buffersForDiagnostics(buffersForDiagnostics), availability(avail) {}

  clang::ModuleFileExtensionMetadata getExtensionMetadata() const override;
  void hashExtension(ExtensionHashBuilder &HBuilder) const override;

  std::unique_ptr<clang::ModuleFileExtensionWriter>
  createExtensionWriter(clang::ASTWriter &writer) override;

  std::unique_ptr<clang::ModuleFileExtensionReader>
  createExtensionReader(const clang::ModuleFileExtensionMetadata &metadata,
                        clang::ASTReader &reader,
                        clang::serialization::ModuleFile &mod,
                        const llvm::BitstreamCursor &stream) override;
};

/// Determines whether the given swift_attr attribute describes the main
/// actor.
bool isMainActorAttr(const clang::SwiftAttrAttr *swiftAttr);

/// Determines whether the given swift_attr controls mutability
bool isMutabilityAttr(const clang::SwiftAttrAttr *swiftAttr);

/// Apply an attribute to a function type.
static inline Type applyToFunctionType(
    Type type, llvm::function_ref<ASTExtInfo(ASTExtInfo)> transform) {
  // Recurse into optional types.
  if (Type objectType = type->getOptionalObjectType()) {
    return OptionalType::get(applyToFunctionType(objectType, transform));
  }

  // Apply transform to function types.
  if (auto funcType = type->getAs<FunctionType>()) {
    auto newExtInfo = transform(funcType->getExtInfo());
    if (!newExtInfo.isEqualTo(funcType->getExtInfo(), /*useClangTypes=*/true))
      return FunctionType::get(funcType->getParams(), funcType->getResult(),
                               newExtInfo);
  }

  return type;
}

inline Optional<const clang::EnumDecl *> findAnonymousEnumForTypedef(
    const ASTContext &ctx,
    const clang::TypedefType *typedefType) {
  auto *typedefDecl = typedefType->getDecl();
  auto *lookupTable = ctx.getClangModuleLoader()->findLookupTable(typedefDecl->getOwningModule());

  auto foundDecls = lookupTable->lookup(
      SerializedSwiftName(typedefDecl->getName()), EffectiveClangContext());

  auto found = llvm::find_if(foundDecls, [](SwiftLookupTable::SingleEntry decl) {
    return decl.is<clang::NamedDecl *>() &&
        isa<clang::EnumDecl>(decl.get<clang::NamedDecl *>());
  });

  if (found != foundDecls.end())
    return cast<clang::EnumDecl>(found->get<clang::NamedDecl *>());

  // If a swift_private attribute has been attached to the enum, its name will
  // be prefixed with two underscores
  llvm::SmallString<32> swiftPrivateName;
  swiftPrivateName += "__";
  swiftPrivateName += typedefDecl->getName();
  foundDecls = lookupTable->lookup(
      SerializedSwiftName(ctx.getIdentifier(swiftPrivateName)),
      EffectiveClangContext());

  auto swiftPrivateFound =
      llvm::find_if(foundDecls, [](SwiftLookupTable::SingleEntry decl) {
        return decl.is<clang::NamedDecl *>() &&
               isa<clang::EnumDecl>(decl.get<clang::NamedDecl *>()) &&
               decl.get<clang::NamedDecl *>()
                   ->hasAttr<clang::SwiftPrivateAttr>();
      });

  if (swiftPrivateFound != foundDecls.end())
    return cast<clang::EnumDecl>(swiftPrivateFound->get<clang::NamedDecl *>());

  return None;
}

inline std::string getPrivateOperatorName(const std::string &OperatorToken) {
#define OVERLOADED_OPERATOR(Name, Spelling, Token, Unary, Binary, MemberOnly)  \
  if (OperatorToken == Spelling) {                                             \
    return "__operator" #Name;                                                 \
  };
#include "clang/Basic/OperatorKinds.def"
  return "None";
}

}
}

#endif
