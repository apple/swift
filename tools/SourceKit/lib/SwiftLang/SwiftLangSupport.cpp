//===--- SwiftLangSupport.cpp ---------------------------------------------===//
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

#include "SwiftLangSupport.h"
#include "SwiftASTManager.h"
#include "SwiftEditorDiagConsumer.h"
#include "SourceKit/Core/Context.h"
#include "SourceKit/SwiftLang/Factory.h"
#include "SourceKit/Support/FileSystemProvider.h"
#include "SourceKit/Support/UIdent.h"

#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ClangModuleLoader.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/SILOptions.h"
#include "swift/AST/USRGeneration.h"
#include "swift/Config.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/IDE/CodeCompletionCache.h"
#include "swift/IDE/SyntaxModel.h"
#include "swift/IDE/Utils.h"
#include "swift/IDETool/IDEInspectionInstance.h"
#include "swift/IDETool/SyntacticMacroExpansion.h"

#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#if !defined(_WIN32)
#include <sys/param.h>
#else
#define WIN32_MEAN_AND_LEAN
#define NOMINMAX
#include <windows.h>
#endif

using namespace SourceKit;
using namespace swift;
using namespace swift::ide;
using swift::index::SymbolKind;
using swift::index::SymbolSubKind;
using swift::index::SymbolProperty;
using swift::index::SymbolPropertySet;
using swift::index::SymbolInfo;
using swift::index::SymbolRole;
using swift::index::SymbolRoleSet;

#define KIND(NAME, CONTENT) static UIdent Kind##NAME(CONTENT);
#include "SourceKit/Core/ProtocolUIDs.def"

#define REFACTORING(KIND, NAME, ID) static UIdent Kind##Refactoring##KIND("source.refactoring.kind."#ID);
#include "swift/Refactoring/RefactoringKinds.def"

static UIdent Attr_IBAction("source.decl.attribute.ibaction");
static UIdent Attr_IBOutlet("source.decl.attribute.iboutlet");
static UIdent Attr_IBDesignable("source.decl.attribute.ibdesignable");
static UIdent Attr_IBInspectable("source.decl.attribute.ibinspectable");
static UIdent Attr_GKInspectable("source.decl.attribute.gkinspectable");
static UIdent Attr_Objc("source.decl.attribute.objc");
static UIdent Attr_ObjcNamed("source.decl.attribute.objc.name");
static UIdent Attr_Private("source.decl.attribute.private");
static UIdent Attr_FilePrivate("source.decl.attribute.fileprivate");
static UIdent Attr_Internal("source.decl.attribute.internal");
static UIdent Attr_Package("source.decl.attribute.package");
static UIdent Attr_Public("source.decl.attribute.public");
static UIdent Attr_Open("source.decl.attribute.open");
static UIdent Attr_Setter_Private("source.decl.attribute.setter_access.private");
static UIdent Attr_Setter_FilePrivate("source.decl.attribute.setter_access.fileprivate");
static UIdent Attr_Setter_Internal("source.decl.attribute.setter_access.internal");
static UIdent Attr_Setter_Package("source.decl.attribute.setter_access.package");
static UIdent Attr_Setter_Public("source.decl.attribute.setter_access.public");
static UIdent Attr_Setter_Open("source.decl.attribute.setter_access.open");
static UIdent EffectiveAccess_Public("source.decl.effective_access.public");
static UIdent EffectiveAccess_Internal("source.decl.effective_access.internal");
static UIdent EffectiveAccess_FilePrivate("source.decl.effective_access.fileprivate");
static UIdent EffectiveAccess_LessThanFilePrivate("source.decl.effective_access.less_than_fileprivate");

std::unique_ptr<LangSupport>
SourceKit::createSwiftLangSupport(SourceKit::Context &SKCtx) {
  return std::unique_ptr<LangSupport>(new SwiftLangSupport(SKCtx));
}

const std::string LangSupport::SynthesizedUSRSeparator = "::SYNTHESIZED::";

namespace {

class UIdentVisitor : public ASTVisitor<UIdentVisitor,
                                        UIdent, UIdent, UIdent, UIdent > {
  const bool IsRef;

public:
  explicit UIdentVisitor(bool IsRef) : IsRef(IsRef) { }

  /// TODO: reconsider whether having a default case is a good idea.
  UIdent visitDecl(const Decl *D) { return UIdent(); }

  UIdent visitFuncDecl(const FuncDecl *D);
  UIdent visitVarDecl(const VarDecl *D);
  UIdent visitParamDecl(const ParamDecl *D);
  UIdent visitExtensionDecl(const ExtensionDecl *D);

#define UID_FOR(CLASS) \
  UIdent visit##CLASS##Decl(const CLASS##Decl *) { \
    return IsRef ? KindRef##CLASS : KindDecl##CLASS; \
  }
  UID_FOR(Class)
  UID_FOR(Struct)
  UID_FOR(Enum)
  UID_FOR(EnumElement)
  UID_FOR(Protocol)
  UID_FOR(TypeAlias)
  UID_FOR(AssociatedType)
  UID_FOR(GenericTypeParam)
  UID_FOR(Constructor)
  UID_FOR(Destructor)
  UID_FOR(Subscript)
  UID_FOR(OpaqueType)
  UID_FOR(Macro)
#undef UID_FOR
};

} // anonymous namespace

UIdent UIdentVisitor::visitFuncDecl(const FuncDecl *D) {
  if (auto AD = dyn_cast<AccessorDecl>(D)) {
    return SwiftLangSupport::getUIDForAccessor(AD->getStorage(),
                                               AD->getAccessorKind(),
                                               IsRef);
  }

  if (auto *Op = D->getOperatorDecl()) {
    switch (Op->getKind()) {
    case DeclKind::PrefixOperator:
      return IsRef ? KindRefFunctionPrefixOperator : KindDeclFunctionPrefixOperator;
    case DeclKind::PostfixOperator:
      return IsRef ? KindRefFunctionPostfixOperator : KindDeclFunctionPostfixOperator;
    case DeclKind::InfixOperator:
      return IsRef ? KindRefFunctionInfixOperator : KindDeclFunctionInfixOperator;
    default:
      llvm_unreachable("unexpected operator kind");
    }
  }

  const DeclContext *DC = D->getDeclContext();
  if (DC->isTypeContext()) {
    if (D->isStatic()) {
      if (D->getCorrectStaticSpelling() == StaticSpellingKind::KeywordClass)
        return IsRef ? KindRefMethodClass : KindDeclMethodClass;
      else
        return IsRef ? KindRefMethodStatic : KindDeclMethodStatic;
    }
    return IsRef ? KindRefMethodInstance : KindDeclMethodInstance;
  }
  return IsRef ? KindRefFunctionFree : KindDeclFunctionFree;
}

UIdent UIdentVisitor::visitVarDecl(const VarDecl *D) {
  const DeclContext *DC = D->getDeclContext();
  if (DC->isTypeContext()) {
    if (D->isStatic()) {
      if (D->getCorrectStaticSpelling() == StaticSpellingKind::KeywordClass)
        return IsRef ? KindRefVarClass : KindDeclVarClass;
      else
        return IsRef ? KindRefVarStatic : KindDeclVarStatic;
    }
    return IsRef ? KindRefVarInstance : KindDeclVarInstance;
  }
  if (DC->isLocalContext())
    return IsRef ? KindRefVarLocal : KindDeclVarLocal;
  return IsRef ? KindRefVarGlobal : KindDeclVarGlobal;
}

UIdent UIdentVisitor::visitParamDecl(const ParamDecl *D) {
  // There is no KindRefVarParam. It's not usually an interesting difference.
  return IsRef ? KindRefVarLocal : KindDeclVarParam;
}

UIdent UIdentVisitor::visitExtensionDecl(const ExtensionDecl *D) {
  assert(!IsRef && "reference to an extension ?");
  if (NominalTypeDecl *NTD = D->getExtendedNominal()) {
    if (isa<StructDecl>(NTD))
      return KindDeclExtensionStruct;
    if (isa<ClassDecl>(NTD))
      return KindDeclExtensionClass;
    if (isa<EnumDecl>(NTD))
      return KindDeclExtensionEnum;
    if (isa<ProtocolDecl>(NTD))
      return KindDeclExtensionProtocol;
  }
  return UIdent();
}

namespace {
/// A simple FileSystemProvider that creates an InMemoryFileSystem for a given
/// dictionary of file contents and overlays that on top of the real filesystem.
class InMemoryFileSystemProvider: public SourceKit::FileSystemProvider {
  /// Provides the real filesystem, overlayed with an InMemoryFileSystem that
  /// contains specified files at specified locations.
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem>
  getFileSystem(OptionsDictionary &options, std::string &error) override {
    auto InMemoryFS = llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem>(
        new llvm::vfs::InMemoryFileSystem());

    static UIdent KeyFiles("key.files");
    static UIdent KeyName("key.name");
    static UIdent KeySourceFile("key.sourcefile");
    static UIdent KeySourceText("key.sourcetext");
    bool failed = options.forEach(KeyFiles, [&](OptionsDictionary &file) {
      StringRef name;
      if (!file.valueForOption(KeyName, name)) {
        error = "missing 'key.name'";
        return true;
      }

      StringRef content;
      if (file.valueForOption(KeySourceText, content)) {
        auto buffer = llvm::MemoryBuffer::getMemBufferCopy(content, name);
        InMemoryFS->addFile(name, 0, std::move(buffer));
        return false;
      }

      StringRef mappedPath;
      if (!file.valueForOption(KeySourceFile, mappedPath)) {
        error = "missing 'key.sourcefile' or 'key.sourcetext'";
        return true;
      }

      auto bufferOrErr = llvm::MemoryBuffer::getFile(mappedPath);
      if (auto err = bufferOrErr.getError()) {
        llvm::raw_string_ostream errStream(error);
        errStream << "error reading target file '" << mappedPath
                  << "': " << err.message() << "\n";
        return true;
      }

      auto renamedBuffer = llvm::MemoryBuffer::getMemBufferCopy(
          bufferOrErr.get()->getBuffer(), name);
      InMemoryFS->addFile(name, 0, std::move(renamedBuffer));
      return false;
    });

    if (failed)
      return nullptr;

    auto OverlayFS = llvm::IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem>(
        new llvm::vfs::OverlayFileSystem(llvm::vfs::getRealFileSystem()));
    OverlayFS->pushOverlay(std::move(InMemoryFS));
    return OverlayFS;
  }
};
}

static void configureIDEInspectionInstance(
    std::shared_ptr<IDEInspectionInstance> IDEInspectionInst,
    std::shared_ptr<GlobalConfig> GlobalConfig) {
  auto Opts = GlobalConfig->getIDEInspectionOpts();
  IDEInspectionInst->setOptions({
    Opts.MaxASTContextReuseCount,
    Opts.CheckDependencyInterval
  });
}

SwiftLangSupport::SwiftLangSupport(SourceKit::Context &SKCtx)
    : NotificationCtr(SKCtx.getNotificationCenter()),
      SwiftExecutablePath(SKCtx.getSwiftExecutablePath()),
      ReqTracker(SKCtx.getRequestTracker()), CCCache(new SwiftCompletionCache) {
  llvm::SmallString<128> LibPath(SKCtx.getRuntimeLibPath());
  llvm::sys::path::append(LibPath, "swift");
  RuntimeResourcePath = std::string(LibPath.str());
  DiagnosticDocumentationPath = SKCtx.getDiagnosticDocumentationPath().str();

  Stats = std::make_shared<SwiftStatistics>();
  EditorDocuments = std::make_shared<SwiftEditorDocumentFileMap>();

  std::shared_ptr<PluginRegistry> Plugins = std::make_shared<PluginRegistry>();

  ASTMgr = std::make_shared<SwiftASTManager>(
      EditorDocuments, SKCtx.getGlobalConfiguration(), Stats, ReqTracker,
      Plugins, SwiftExecutablePath, RuntimeResourcePath,
      DiagnosticDocumentationPath);

  IDEInspectionInst = std::make_shared<IDEInspectionInstance>(Plugins);
  configureIDEInspectionInstance(IDEInspectionInst,
                                 SKCtx.getGlobalConfiguration());

  CompileManager = std::make_shared<compile::SessionManager>(
      SwiftExecutablePath, RuntimeResourcePath, DiagnosticDocumentationPath,
      Plugins);

  // By default, just use the in-memory cache.
  CCCache->inMemory = std::make_unique<ide::CodeCompletionCache>();

  SyntacticMacroExpansions =
      std::make_shared<SyntacticMacroExpansion>(SwiftExecutablePath, Plugins);

  // Provide a default file system provider.
  setFileSystemProvider("in-memory-vfs", std::make_unique<InMemoryFileSystemProvider>());
}

SwiftLangSupport::~SwiftLangSupport() {
}

void SwiftLangSupport::globalConfigurationUpdated(
    std::shared_ptr<GlobalConfig> Config) {
  configureIDEInspectionInstance(IDEInspectionInst, Config);
}

void SwiftLangSupport::dependencyUpdated() {
  IDEInspectionInst->markCachedCompilerInstanceShouldBeInvalidated();
}

UIdent SwiftLangSupport::getUIDForDeclLanguage(const swift::Decl *D) {
  if (D->hasClangNode())
    return KindObjC;
  return KindSwift;
}

UIdent SwiftLangSupport::getUIDForDecl(const Decl *D, bool IsRef) {
  return UIdentVisitor(IsRef).visit(const_cast<Decl*>(D));
}

UIdent SwiftLangSupport::getUIDForExtensionOfDecl(const Decl *D) {
  switch (D->getKind()) {
    case swift::DeclKind::Struct:
      return KindDeclExtensionStruct;
    case swift::DeclKind::Enum:
      return KindDeclExtensionEnum;
    case swift::DeclKind::Class:
      return KindDeclExtensionClass;
    case swift::DeclKind::Protocol:
      return KindDeclExtensionProtocol;
    default:
      llvm_unreachable("cannot have extension.");
  }
}

UIdent SwiftLangSupport::getUIDForLocalVar(bool IsRef) {
  return IsRef ? KindRefVarLocal : KindDeclVarLocal;
}

UIdent SwiftLangSupport::getUIDForAccessor(const ValueDecl *D,
                                           AccessorKind AccKind,
                                           bool IsRef) {
  switch (AccKind) {
  case AccessorKind::Get:
    return IsRef ? KindRefAccessorGetter : KindDeclAccessorGetter;
  case AccessorKind::Set:
    return IsRef ? KindRefAccessorSetter : KindDeclAccessorSetter;
  case AccessorKind::WillSet:
    return IsRef ? KindRefAccessorWillSet : KindDeclAccessorWillSet;
  case AccessorKind::DidSet:
    return IsRef ? KindRefAccessorDidSet : KindDeclAccessorDidSet;
  case AccessorKind::Address:
    return IsRef ? KindRefAccessorAddress : KindDeclAccessorAddress;
  case AccessorKind::MutableAddress:
    return IsRef ? KindRefAccessorMutableAddress
                 : KindDeclAccessorMutableAddress;
  case AccessorKind::Read:
    return IsRef ? KindRefAccessorRead : KindDeclAccessorRead;
  case AccessorKind::Modify:
    return IsRef ? KindRefAccessorModify : KindDeclAccessorModify;
  case AccessorKind::Init:
    return IsRef ? KindRefAccessorInit : KindDeclAccessorInit;
  }

  llvm_unreachable("Unhandled AccessorKind in switch.");
}

SourceKit::UIdent SwiftLangSupport::getUIDForModuleRef() {
  return KindRefModule;
}

SourceKit::UIdent SwiftLangSupport::getUIDForObjCAttr() {
  return Attr_Objc;
}

UIdent SwiftLangSupport::getUIDForRefactoringKind(ide::RefactoringKind Kind){
  switch(Kind) {
  case ide::RefactoringKind::None: llvm_unreachable("cannot end up here.");
#define REFACTORING(KIND, NAME, ID)                                            \
  case ide::RefactoringKind::KIND: return KindRefactoring##KIND;
#include "swift/Refactoring/RefactoringKinds.def"
  }
}

UIdent SwiftLangSupport::getUIDForCodeCompletionDeclKind(
    ide::CodeCompletionDeclKind Kind, bool IsRef) {
  if (IsRef) {
    // FIXME: The code-completion kind is also used for semantic annotations.
    // We should either create a new kind or rename the code-completion kind to
    // something more general.
    switch (Kind) {
    case CodeCompletionDeclKind::Module: return KindRefModule;
    case CodeCompletionDeclKind::Class: return KindRefClass;
    case CodeCompletionDeclKind::Actor: return KindRefActor;
    case CodeCompletionDeclKind::Struct: return KindRefStruct;
    case CodeCompletionDeclKind::Enum: return KindRefEnum;
    case CodeCompletionDeclKind::EnumElement: return KindRefEnumElement;
    case CodeCompletionDeclKind::Protocol: return KindRefProtocol;
    case CodeCompletionDeclKind::AssociatedType: return KindRefAssociatedType;
    case CodeCompletionDeclKind::TypeAlias: return KindRefTypeAlias;
    case CodeCompletionDeclKind::GenericTypeParam: return KindRefGenericTypeParam;
    case CodeCompletionDeclKind::Constructor: return KindRefConstructor;
    case CodeCompletionDeclKind::Destructor: return KindRefDestructor;
    case CodeCompletionDeclKind::Subscript: return KindRefSubscript;
    case CodeCompletionDeclKind::StaticMethod: return KindRefMethodClass;
    case CodeCompletionDeclKind::InstanceMethod: return KindRefMethodInstance;
    case CodeCompletionDeclKind::PrefixOperatorFunction: return KindRefFunctionPrefixOperator;
    case CodeCompletionDeclKind::PostfixOperatorFunction: return KindRefFunctionPostfixOperator;
    case CodeCompletionDeclKind::InfixOperatorFunction: return KindRefFunctionInfixOperator;
    case CodeCompletionDeclKind::PrecedenceGroup: return KindRefPrecedenceGroup;
    case CodeCompletionDeclKind::FreeFunction: return KindRefFunctionFree;
    case CodeCompletionDeclKind::StaticVar: return KindRefVarClass;
    case CodeCompletionDeclKind::InstanceVar: return KindRefVarInstance;
    case CodeCompletionDeclKind::LocalVar: return KindRefVarLocal;
    case CodeCompletionDeclKind::GlobalVar: return KindRefVarGlobal;
    case CodeCompletionDeclKind::Macro: return KindRefMacro;
    }
  }

  switch (Kind) {
  case CodeCompletionDeclKind::Module: return KindDeclModule;
  case CodeCompletionDeclKind::Class: return KindDeclClass;
  case CodeCompletionDeclKind::Actor: return KindDeclActor;
  case CodeCompletionDeclKind::Struct: return KindDeclStruct;
  case CodeCompletionDeclKind::Enum: return KindDeclEnum;
  case CodeCompletionDeclKind::EnumElement: return KindDeclEnumElement;
  case CodeCompletionDeclKind::Protocol: return KindDeclProtocol;
  case CodeCompletionDeclKind::AssociatedType: return KindDeclAssociatedType;
  case CodeCompletionDeclKind::TypeAlias: return KindDeclTypeAlias;
  case CodeCompletionDeclKind::GenericTypeParam: return KindDeclGenericTypeParam;
  case CodeCompletionDeclKind::Constructor: return KindDeclConstructor;
  case CodeCompletionDeclKind::Destructor: return KindDeclDestructor;
  case CodeCompletionDeclKind::Subscript: return KindDeclSubscript;
  case CodeCompletionDeclKind::StaticMethod: return KindDeclMethodClass;
  case CodeCompletionDeclKind::InstanceMethod: return KindDeclMethodInstance;
  case CodeCompletionDeclKind::PrefixOperatorFunction: return KindDeclFunctionPrefixOperator;
  case CodeCompletionDeclKind::PostfixOperatorFunction: return KindDeclFunctionPostfixOperator;
  case CodeCompletionDeclKind::InfixOperatorFunction: return KindDeclFunctionInfixOperator;
  case CodeCompletionDeclKind::PrecedenceGroup: return KindDeclPrecedenceGroup;
  case CodeCompletionDeclKind::FreeFunction: return KindDeclFunctionFree;
  case CodeCompletionDeclKind::StaticVar: return KindDeclVarClass;
  case CodeCompletionDeclKind::InstanceVar: return KindDeclVarInstance;
  case CodeCompletionDeclKind::LocalVar: return KindDeclVarLocal;
  case CodeCompletionDeclKind::GlobalVar: return KindDeclVarGlobal;
  case CodeCompletionDeclKind::Macro: return KindDeclMacro;
  }

  llvm_unreachable("Unhandled CodeCompletionDeclKind in switch.");
}

UIdent SwiftLangSupport::getUIDForSyntaxNodeKind(SyntaxNodeKind SC) {
  switch (SC) {
  case SyntaxNodeKind::Keyword:
    return KindKeyword;
  case SyntaxNodeKind::Identifier:
  case SyntaxNodeKind::DollarIdent:
    return KindIdentifier;
  case SyntaxNodeKind::Operator:
    return KindOperator;
  case SyntaxNodeKind::Integer:
  case SyntaxNodeKind::Floating:
    return KindNumber;
  case SyntaxNodeKind::String:
    return KindString;
  case SyntaxNodeKind::StringInterpolationAnchor:
    return KindStringInterpolation;
  case SyntaxNodeKind::CommentLine:
  case SyntaxNodeKind::CommentBlock:
    return KindComment;
  case SyntaxNodeKind::CommentMarker:
    return KindCommentMarker;
  case SyntaxNodeKind::CommentURL:
    return KindCommentURL;
  case SyntaxNodeKind::DocCommentLine:
  case SyntaxNodeKind::DocCommentBlock:
    return KindDocComment;
  case SyntaxNodeKind::DocCommentField:
    return KindDocCommentField;
  case SyntaxNodeKind::TypeId:
    return KindTypeIdentifier;
  case SyntaxNodeKind::BuildConfigKeyword:
    return KindBuildConfigKeyword;
  case SyntaxNodeKind::BuildConfigId:
    return KindBuildConfigId;
  case SyntaxNodeKind::PoundDirectiveKeyword:
    return KindPoundDirectiveKeyword;
  case SyntaxNodeKind::AttributeId:
    return KindAttributeId;
  case SyntaxNodeKind::AttributeBuiltin:
    return KindAttributeBuiltin;
  case SyntaxNodeKind::EditorPlaceholder:
    return KindPlaceholder;
  case SyntaxNodeKind::ObjectLiteral:
    return KindObjectLiteral;
  }

  // Default to a known kind to prevent crashing in non-asserts builds
  assert(0 && "Unhandled SyntaxNodeKind in switch.");
  return KindIdentifier;
}

UIdent SwiftLangSupport::getUIDForSyntaxStructureKind(
    SyntaxStructureKind Kind) {
  switch (Kind) {
    case SyntaxStructureKind::Class:
      return KindDeclClass;
    case SyntaxStructureKind::Struct:
      return KindDeclStruct;
    case SyntaxStructureKind::Protocol:
      return KindDeclProtocol;
    case SyntaxStructureKind::Enum:
      return KindDeclEnum;
    case SyntaxStructureKind::Extension:
      return KindDeclExtension;
    case SyntaxStructureKind::FreeFunction:
      return KindDeclFunctionFree;
    case SyntaxStructureKind::InstanceFunction:
      return KindDeclMethodInstance;
    case SyntaxStructureKind::StaticFunction:
      return KindDeclMethodStatic;
    case SyntaxStructureKind::ClassFunction:
      return KindDeclMethodClass;
    case SyntaxStructureKind::GlobalVariable:
      return KindDeclVarGlobal;
    case SyntaxStructureKind::InstanceVariable:
      return KindDeclVarInstance;
    case SyntaxStructureKind::StaticVariable:
      return KindDeclVarStatic;
    case SyntaxStructureKind::ClassVariable:
      return KindDeclVarClass;
    case SyntaxStructureKind::LocalVariable:
      return KindDeclVarLocal;
    case SyntaxStructureKind::EnumCase:
      return KindDeclEnumCase;
    case SyntaxStructureKind::EnumElement:
      return KindDeclEnumElement;
    case SyntaxStructureKind::TypeAlias:
      return KindDeclTypeAlias;
    case SyntaxStructureKind::Subscript:
      return KindDeclSubscript;
    case SyntaxStructureKind::AssociatedType:
      return KindDeclAssociatedType;
    case SyntaxStructureKind::GenericTypeParam:
      return KindDeclGenericTypeParam;
    case SyntaxStructureKind::Parameter:
      return KindDeclVarParam;
    case SyntaxStructureKind::ForEachStatement:
      return KindStmtForEach;
    case SyntaxStructureKind::WhileStatement:
      return KindStmtWhile;
    case SyntaxStructureKind::RepeatWhileStatement:
      return KindStmtRepeatWhile;
    case SyntaxStructureKind::IfStatement:
      return KindStmtIf;
    case SyntaxStructureKind::GuardStatement:
      return KindStmtGuard;
    case SyntaxStructureKind::SwitchStatement:
      return KindStmtSwitch;
    case SyntaxStructureKind::CaseStatement:
      return KindStmtCase;
    case SyntaxStructureKind::BraceStatement:
      return KindStmtBrace;
    case SyntaxStructureKind::CallExpression:
      return KindExprCall;
    case SyntaxStructureKind::ArrayExpression:
      return KindExprArray;
    case SyntaxStructureKind::DictionaryExpression:
      return KindExprDictionary;
    case SyntaxStructureKind::ObjectLiteralExpression:
      return KindExprObjectLiteral;
    case SyntaxStructureKind::TupleExpression:
      return KindExprTuple;
    case SyntaxStructureKind::ClosureExpression:
      return KindExprClosure;
    case SyntaxStructureKind::Argument:
      return KindExprArg;
  }

  llvm_unreachable("Unhandled SyntaxStructureKind in switch.");
}

UIdent SwiftLangSupport::getUIDForSyntaxStructureElementKind(
    SyntaxStructureElementKind Kind) {
  switch (Kind) {
    case SyntaxStructureElementKind::Id: return KindStructureElemId;
    case SyntaxStructureElementKind::Expr: return KindStructureElemExpr;
    case SyntaxStructureElementKind::InitExpr: return KindStructureElemInitExpr;
    case SyntaxStructureElementKind::ConditionExpr: return KindStructureElemCondExpr;
    case SyntaxStructureElementKind::Pattern: return KindStructureElemPattern;
    case SyntaxStructureElementKind::TypeRef: return KindStructureElemTypeRef;
  }

  llvm_unreachable("Unhandled SyntaxStructureElementKind in switch.");
}

SourceKit::UIdent SwiftLangSupport::
getUIDForRangeKind(swift::ide::RangeKind Kind) {
  switch (Kind) {
    case swift::ide::RangeKind::SingleExpression: return KindRangeSingleExpression;
    case swift::ide::RangeKind::SingleStatement: return KindRangeSingleStatement;
    case swift::ide::RangeKind::SingleDecl: return KindRangeSingleDeclaration;
    case swift::ide::RangeKind::MultiStatement: return KindRangeMultiStatement;
    case swift::ide::RangeKind::MultiTypeMemberDecl:
      return KindRangeMultiTypeMemberDeclaration;
    case swift::ide::RangeKind::PartOfExpression: return KindRangeInvalid;
    case swift::ide::RangeKind::Invalid: return KindRangeInvalid;
  }

  llvm_unreachable("Unhandled RangeKind in switch.");
}

SourceKit::UIdent SwiftLangSupport::
getUIDForRegionType(swift::ide::RegionType Type) {
  switch (Type) {
    case swift::ide::RegionType::ActiveCode: return KindEditActive;
    case swift::ide::RegionType::InactiveCode: return KindEditInactive;
    case swift::ide::RegionType::Selector: return KindEditSelector;
    case swift::ide::RegionType::String: return KindEditString;
    case swift::ide::RegionType::Comment: return KindEditComment;
    case swift::ide::RegionType::Mismatch: return KindEditMismatch;
    case swift::ide::RegionType::Unmatched: return KindEditUnknown;
  }
}

SourceKit::UIdent SwiftLangSupport::
getUIDForRefactoringRangeKind(ide::RefactoringRangeKind Kind) {
  switch (Kind) {
  case ide::RefactoringRangeKind::BaseName:
    return KindRenameRangeBase;
  case ide::RefactoringRangeKind::KeywordBaseName:
    return KindRenameRangeKeywordBase;
  case ide::RefactoringRangeKind::ParameterName:
    return KindRenameRangeParam;
  case ide::RefactoringRangeKind::NoncollapsibleParameterName:
    return KindRenameRangeNoncollapsibleParam;
  case ide::RefactoringRangeKind::DeclArgumentLabel:
    return KindRenameRangeDeclArgLabel;
  case ide::RefactoringRangeKind::CallArgumentLabel:
    return KindRenameRangeCallArgLabel;
  case ide::RefactoringRangeKind::CallArgumentColon:
    return KindRenameRangeCallArgColon;
  case ide::RefactoringRangeKind::CallArgumentCombined:
    return KindRenameRangeCallArgCombined;
  case ide::RefactoringRangeKind::SelectorArgumentLabel:
    return KindRenameRangeSelectorArgLabel;
  }
}

UIdent SwiftLangSupport::getUIDForSymbol(SymbolInfo sym, bool isRef) {

#define UID_FOR(CLASS) isRef ? KindRef##CLASS : KindDecl##CLASS;

  switch (sym.SubKind) {
  default: break;
  case SymbolSubKind::AccessorGetter: return UID_FOR(AccessorGetter);
  case SymbolSubKind::AccessorSetter: return UID_FOR(AccessorSetter);
  case SymbolSubKind::SwiftAccessorWillSet: return UID_FOR(AccessorWillSet);
  case SymbolSubKind::SwiftAccessorDidSet: return UID_FOR(AccessorDidSet);
  case SymbolSubKind::SwiftAccessorAddressor: return UID_FOR(AccessorAddress);
  case SymbolSubKind::SwiftAccessorMutableAddressor: return UID_FOR(AccessorMutableAddress);
  }

#define SIMPLE_CASE(KIND) \
  case SymbolKind::KIND: \
    return UID_FOR(KIND);

  switch (sym.Kind) {
  SIMPLE_CASE(Enum)
  SIMPLE_CASE(Struct)
  SIMPLE_CASE(Class)
  SIMPLE_CASE(Protocol)
  SIMPLE_CASE(Constructor)
  SIMPLE_CASE(Destructor)
  SIMPLE_CASE(Macro)

  case SymbolKind::EnumConstant:
    return UID_FOR(EnumElement);

  case SymbolKind::TypeAlias:
    if (sym.SubKind == SymbolSubKind::SwiftAssociatedType)
      return UID_FOR(AssociatedType);
    if (sym.SubKind == SymbolSubKind::SwiftGenericTypeParam)
      return UID_FOR(GenericTypeParam);
    return UID_FOR(TypeAlias);

  case SymbolKind::Function:
  case SymbolKind::StaticMethod:
    if (sym.SubKind == SymbolSubKind::SwiftPrefixOperator)
      return UID_FOR(FunctionPrefixOperator);
    if (sym.SubKind == SymbolSubKind::SwiftPostfixOperator)
      return UID_FOR(FunctionPostfixOperator);
    if (sym.SubKind == SymbolSubKind::SwiftInfixOperator)
      return UID_FOR(FunctionInfixOperator);
    if (sym.Kind == SymbolKind::StaticMethod) {
      return UID_FOR(MethodStatic);
    } else {
      return UID_FOR(FunctionFree);
    }
  case SymbolKind::Variable:
    return UID_FOR(VarGlobal);
  case SymbolKind::InstanceMethod:
    return UID_FOR(MethodInstance);
  case SymbolKind::ClassMethod:
    return UID_FOR(MethodClass);
  case SymbolKind::InstanceProperty:
    if (sym.SubKind == SymbolSubKind::SwiftSubscript)
      return UID_FOR(Subscript);
    return UID_FOR(VarInstance);
  case SymbolKind::ClassProperty:
    return UID_FOR(VarClass);
  case SymbolKind::StaticProperty:
    return UID_FOR(VarStatic);

  case SymbolKind::Extension:
    assert(!isRef && "reference to extension decl?");
    if (sym.SubKind == SymbolSubKind::SwiftExtensionOfStruct) {
      return KindDeclExtensionStruct;
    } else if (sym.SubKind == SymbolSubKind::SwiftExtensionOfClass) {
      return KindDeclExtensionClass;
    } else if (sym.SubKind == SymbolSubKind::SwiftExtensionOfEnum) {
      return KindDeclExtensionEnum;
    } else if (sym.SubKind == SymbolSubKind::SwiftExtensionOfProtocol) {
      return KindDeclExtensionProtocol;
    } else {
      llvm_unreachable("missing extension sub kind");
    }

  case SymbolKind::Module:
    return KindRefModule;

  case SymbolKind::CommentTag:
    return KindCommentTag;

  default:
    // TODO: reconsider whether having a default case is a good idea.
    llvm_unreachable("unhandled symbol kind Swift doesn't use");
  }

#undef SIMPLE_CASE
#undef UID_FOR
}

SourceKit::UIdent SwiftLangSupport::getUIDForNameKind(swift::ide::NameKind Kind) {
  switch(Kind) {
  case swift::ide::NameKind::ObjC: return KindNameObjc;
  case swift::ide::NameKind::Swift: return KindNameSwift;
  }

  llvm_unreachable("Unhandled NameKind in switch.");
}

swift::ide::NameKind SwiftLangSupport::getNameKindForUID(SourceKit::UIdent Id) {
  if (Id == KindNameObjc)
    return swift::ide::NameKind::ObjC;
  assert(Id == KindNameSwift);
  return swift::ide::NameKind::Swift;
}

llvm::Optional<UIdent>
SwiftLangSupport::getUIDForDeclAttribute(const swift::DeclAttribute *Attr) {
  // Check special-case names first.
  switch (Attr->getKind()) {
    case DAK_IBAction: {
      return Attr_IBAction;
    }
    case DAK_IBSegueAction: {
      static UIdent Attr_IBSegueAction("source.decl.attribute.ibsegueaction");
      return Attr_IBSegueAction;
    }
    case DAK_IBOutlet: {
      return Attr_IBOutlet;
    }
    case DAK_IBDesignable: {
      return Attr_IBDesignable;
    }
    case DAK_IBInspectable: {
      return Attr_IBInspectable;
    }
    case DAK_GKInspectable: {
      return Attr_GKInspectable;
    }
    case DAK_ObjC: {
      if (cast<ObjCAttr>(Attr)->hasName()) {
        return Attr_ObjcNamed;
      } else {
        return Attr_Objc;
      }
    }
    case DAK_AccessControl: {
      switch (cast<AbstractAccessControlAttr>(Attr)->getAccess()) {
        case AccessLevel::Private:
          return Attr_Private;
        case AccessLevel::FilePrivate:
          return Attr_FilePrivate;
        case AccessLevel::Internal:
          return Attr_Internal;
        case AccessLevel::Package:
          return Attr_Package;
        case AccessLevel::Public:
          return Attr_Public;
        case AccessLevel::Open:
          return Attr_Open;
      }
    }
    case DAK_SetterAccess: {
      switch (cast<AbstractAccessControlAttr>(Attr)->getAccess()) {
        case AccessLevel::Private:
          return Attr_Setter_Private;
        case AccessLevel::FilePrivate:
          return Attr_Setter_FilePrivate;
        case AccessLevel::Internal:
          return Attr_Setter_Internal;
        case AccessLevel::Package:
          return Attr_Setter_Package;
        case AccessLevel::Public:
          return Attr_Setter_Public;
        case AccessLevel::Open:
          return Attr_Setter_Open;
      }
    }

    // Ignore these.
    case DAK_ShowInInterface:
    case DAK_RawDocComment:
    case DAK_HasInitialValue:
    case DAK_HasStorage:
      return llvm::None;
    default:
      break;
  }

  switch (Attr->getKind()) {
    case DAK_Count:
      break;
#define DECL_ATTR(X, CLASS, ...)\
    case DAK_##CLASS: {\
      static UIdent Attr_##X("source.decl.attribute."#X); \
      return Attr_##X; \
    }
#include "swift/AST/Attr.def"
  }

  return llvm::None;
}

UIdent SwiftLangSupport::getUIDForFormalAccessScope(const swift::AccessScope Scope) {
  if (Scope.isPublic()) {
    return EffectiveAccess_Public;
  } else if (Scope.isInternal()) {
    return EffectiveAccess_Internal;
  } else if (Scope.isFileScope()) {
    return EffectiveAccess_FilePrivate;
  } else if (Scope.isPrivate()) {
    return EffectiveAccess_LessThanFilePrivate;
  } else {
    llvm_unreachable("Unsupported access scope");
  }
}

std::vector<UIdent> SwiftLangSupport::UIDsFromDeclAttributes(const DeclAttributes &Attrs) {
  std::vector<UIdent> AttrUIDs;

  for (auto Attr : Attrs) {
    if (auto AttrUID = getUIDForDeclAttribute(Attr)) {
      AttrUIDs.push_back(AttrUID.value());
    }
  }

  return AttrUIDs;
}

bool SwiftLangSupport::printDisplayName(const swift::ValueDecl *D,
                                        llvm::raw_ostream &OS) {
  if (!D->hasName())
    return true;

  OS << D->getName();
  return false;
}

bool SwiftLangSupport::printUSR(const ValueDecl *D, llvm::raw_ostream &OS) {
  return ide::printValueDeclUSR(D, OS);
}

bool SwiftLangSupport::printDeclTypeUSR(const ValueDecl *D, llvm::raw_ostream &OS) {
  return ide::printDeclTypeUSR(D, OS);
}

bool SwiftLangSupport::printTypeUSR(Type Ty, llvm::raw_ostream &OS) {
  return ide::printTypeUSR(Ty, OS);
}

bool SwiftLangSupport::printAccessorUSR(const AbstractStorageDecl *D,
                                        AccessorKind AccKind,
                                        llvm::raw_ostream &OS) {
  return ide::printAccessorUSR(D, AccKind, OS);
}

void SwiftLangSupport::printMemberDeclDescription(const swift::ValueDecl *VD,
                                                  swift::Type baseTy,
                                                  bool usePlaceholder,
                                                  llvm::raw_ostream &OS) {
  // Base name.
  OS << VD->getBaseName().userFacingName();

  // Parameters.
  auto *M = VD->getModuleContext();
  auto substMap = baseTy->getMemberSubstitutionMap(M, VD);
  auto printSingleParam = [&](ParamDecl *param) {
    auto paramTy = param->getInterfaceType();

    // Label.
    if (!param->getArgumentName().empty())
      OS << param->getArgumentName() << ": ";

    // InOut.
    if (param->isInOut()) {
      OS << "&";
      paramTy = paramTy->getInOutObjectType();
    }

    // Type.
    if (usePlaceholder)
      OS << "<#T##";

    paramTy = paramTy.subst(substMap);
    if (paramTy->hasError() && param->getTypeRepr()) {
      // Fallback to 'TypeRepr' printing.
      param->getTypeRepr()->print(OS);
    } else {
      paramTy.print(OS);
    }

    if (usePlaceholder)
      OS << "#>";
  };
  auto printParams = [&](const ParameterList *params) {
    OS << '(';
    bool isFirst = true;
    for (auto param : params->getArray()) {
      if (isFirst)
        isFirst = false;
      else
        OS << ", ";
      printSingleParam(param);
    }
    OS << ')';
  };
  if (isa<EnumElementDecl>(VD) || isa<FuncDecl>(VD)) {
    if (const auto ParamList = getParameterList(const_cast<ValueDecl *>(VD))) {
      printParams(ParamList);
    }
  } else if (isa<VarDecl>(VD)) {
    // Var decl doesn't have parameters.
  } else {
    llvm_unreachable("Unsupported Decl kind for printMemberDeclDescription()");
  }
}

std::string SwiftLangSupport::resolvePathSymlinks(StringRef FilePath) {
  std::string InputPath = FilePath.str();
  llvm::SmallString<256> output;
  if (llvm::sys::fs::real_path(InputPath, output))
    return InputPath;
  return std::string(output.str());
}

void SwiftLangSupport::getStatistics(StatisticsReceiver receiver) {
  std::vector<Statistic *> stats = {
#define SWIFT_STATISTIC(VAR, UID, DESC) &Stats->VAR,
#include "SwiftStatistics.def"
  };
  receiver(stats);
}

FileSystemProvider *SwiftLangSupport::getFileSystemProvider(StringRef Name) {
  auto It = FileSystemProviders.find(Name);
  if (It == FileSystemProviders.end())
    return nullptr;
  return It->second.get();
}

void SwiftLangSupport::setFileSystemProvider(
     StringRef Name, std::unique_ptr<FileSystemProvider> FileSystemProvider) {
  assert(FileSystemProvider);
  auto Result = FileSystemProviders.try_emplace(Name, std::move(FileSystemProvider));
  assert(Result.second && "tried to set existing FileSystemProvider");
}

llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem>
SwiftLangSupport::getFileSystem(const llvm::Optional<VFSOptions> &vfsOptions,
                                llvm::Optional<StringRef> primaryFile,
                                std::string &error) {
  // First, try the specified vfsOptions.
  if (vfsOptions) {
    auto provider = getFileSystemProvider(vfsOptions->name);
    if (!provider) {
      error = "unknown virtual filesystem '" + vfsOptions->name + "'";
      return nullptr;
    }

    return provider->getFileSystem(*vfsOptions->options, error);
  }

  // Otherwise, try to find an open document with a filesystem.
  if (primaryFile) {
    if (auto doc = EditorDocuments->getByUnresolvedName(*primaryFile)) {
      return doc->getFileSystem();
    }
  }

  // Fallback to the real filesystem.
  return llvm::vfs::getRealFileSystem();
}

void SwiftLangSupport::performWithParamsToCompletionLikeOperation(
    llvm::MemoryBuffer *UnresolvedInputFile, unsigned Offset,
    bool InsertCodeCompletionToken, ArrayRef<const char *> Args,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
    SourceKitCancellationToken CancellationToken,
    llvm::function_ref<void(CancellableResult<CompletionLikeOperationParams>)>
        PerformOperation) {
  assert(FileSystem);

  // Resolve symlinks for the input file; we resolve them for the input files
  // in the arguments as well.
  // FIXME: We need the Swift equivalent of Clang's FileEntry.
  llvm::SmallString<128> bufferIdentifier;
  if (auto err = FileSystem->getRealPath(
          UnresolvedInputFile->getBufferIdentifier(), bufferIdentifier))
    bufferIdentifier = UnresolvedInputFile->getBufferIdentifier();

  // Create a buffer for code completion. This contains '\0' at 'Offset'
  // position of 'UnresolvedInputFile' buffer.
  auto origOffset = Offset;

  // If we inserted a code completion token into the buffer, this keeps the
  // buffer alive. Should never be accessed, `buffer` should be used instead.
  std::unique_ptr<llvm::MemoryBuffer> codeCompletionBuffer;
  llvm::MemoryBuffer *buffer;
  if (InsertCodeCompletionToken) {
    codeCompletionBuffer = ide::makeCodeCompletionMemoryBuffer(
        UnresolvedInputFile, Offset, bufferIdentifier);
    buffer = codeCompletionBuffer.get();
  } else {
    buffer = UnresolvedInputFile;
  }

  SourceManager SM;
  DiagnosticEngine Diags(SM);
  PrintingDiagnosticConsumer PrintDiags;
  EditorDiagConsumer TraceDiags;
  trace::TracedOperation TracedOp{trace::OperationKind::CodeCompletion};

  Diags.addConsumer(PrintDiags);
  if (TracedOp.enabled()) {
    Diags.addConsumer(TraceDiags);
    trace::SwiftInvocation SwiftArgs;
    trace::initTraceInfo(SwiftArgs, bufferIdentifier, Args);
    TracedOp.setDiagnosticProvider(
        [&](SmallVectorImpl<DiagnosticEntryInfo> &diags) {
          TraceDiags.getAllDiagnostics(diags);
        });
    TracedOp.start(
        SwiftArgs,
        {std::make_pair("OriginalOffset", std::to_string(origOffset)),
         std::make_pair("Offset", std::to_string(Offset))});
  }
  ForwardingDiagnosticConsumer CIDiags(Diags);

  CompilerInvocation Invocation;
  std::string CompilerInvocationError;
  bool CreatingInvocationFailed = getASTManager()->initCompilerInvocation(
      Invocation, Args, FrontendOptions::ActionType::Typecheck, Diags,
      buffer->getBufferIdentifier(), FileSystem, CompilerInvocationError);
  if (CreatingInvocationFailed) {
    PerformOperation(CancellableResult<CompletionLikeOperationParams>::failure(
        CompilerInvocationError));
    return;
  }
  if (!Invocation.getFrontendOptions().InputsAndOutputs.hasInputs()) {
    PerformOperation(CancellableResult<CompletionLikeOperationParams>::failure(
        "no input filenames specified"));
    return;
  }

  // Pin completion instance.
  auto CompletionInst = getIDEInspectionInstance();

  auto CancellationFlag = std::make_shared<std::atomic<bool>>(false);
  ReqTracker->setCancellationHandler(CancellationToken, [CancellationFlag] {
    CancellationFlag->store(true, std::memory_order_relaxed);
  });

  CompletionLikeOperationParams Params = {Invocation, buffer, &CIDiags,
                                          CancellationFlag};
  PerformOperation(
      CancellableResult<CompletionLikeOperationParams>::success(Params));
}

CloseClangModuleFiles::~CloseClangModuleFiles() {
  clang::Preprocessor &PP = loader.getClangPreprocessor();
  clang::ModuleMap &ModMap = PP.getHeaderSearchInfo().getModuleMap();
  for (auto I = ModMap.module_begin(), E = ModMap.module_end(); I != E; ++I) {
    clang::Module *M = I->second;
    if (!M->isSubModule() && M->getASTFile())
      M->getASTFile()->closeFile();
  }
}

void SourceKit::disableExpensiveSILOptions(SILOptions &Opts) {
  // Disable the sanitizers.
  Opts.Sanitizers = {};

  // Disable PGO and code coverage.
  Opts.GenerateProfile = false;
  Opts.EmitProfileCoverageMapping = false;
  Opts.UseProfile = "";
}
