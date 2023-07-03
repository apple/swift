//===--- TBDGenVisitor.h - AST Visitor for TBD generation -----------------===//
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
//  This file defines the visitor that finds all symbols in a swift AST.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_TBDGEN_TBDGENVISITOR_H
#define SWIFT_TBDGEN_TBDGENVISITOR_H

#include "swift/AST/ASTMangler.h"
#include "swift/AST/FileUnit.h"
#include "swift/AST/Module.h"
#include "swift/Basic/LLVM.h"
#include "swift/IRGen/IRSymbolVisitor.h"
#include "swift/IRGen/Linking.h"
#include "swift/SIL/SILDeclRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/Triple.h"
#include "llvm/TextAPI/InterfaceFile.h"

using namespace swift::irgen;
using StringSet = llvm::StringSet<>;

namespace llvm {
class DataLayout;
}

namespace swift {

class TBDGenDescriptor;
struct TBDGenOptions;
class SymbolSource;

namespace tbdgen {

enum class LinkerPlatformId: uint8_t {
#define LD_PLATFORM(Name, Id) Name = Id,
#include "ldPlatformKinds.def"
};

struct InstallNameStore {
  // The default install name to use when no specific install name is specified.
  std::string InstallName;
  // The install name specific to the platform id. This takes precedence over
  // the default install name.
  std::map<uint8_t, std::string> PlatformInstallName;
  StringRef getInstallName(LinkerPlatformId Id) const;
};

/// A set of callbacks for recording APIs.
class APIRecorder {
public:
  virtual ~APIRecorder() {}

  virtual void addSymbol(StringRef name, llvm::MachO::SymbolKind kind,
                         SymbolSource source) {}
  virtual void addObjCInterface(const ClassDecl *decl) {}
  virtual void addObjCCategory(const ExtensionDecl *decl) {}
  virtual void addObjCMethod(const GenericContext *ctx, SILDeclRef method) {}
};

class SimpleAPIRecorder final : public APIRecorder {
public:
  using SymbolCallbackFn = llvm::function_ref<void(
      StringRef, llvm::MachO::SymbolKind, SymbolSource)>;

  SimpleAPIRecorder(SymbolCallbackFn func) : func(func) {}

  void addSymbol(StringRef symbol, llvm::MachO::SymbolKind kind,
                 SymbolSource source) override {
    func(symbol, kind, source);
  }
private:
  SymbolCallbackFn func;
};

class TBDGenVisitor : public IRSymbolVisitor {
#ifndef NDEBUG
  /// Tracks the symbols emitted to ensure we don't emit any duplicates.
  llvm::StringSet<> DuplicateSymbolChecker;
#endif

  llvm::Optional<llvm::DataLayout> DataLayout = llvm::None;
  const StringRef DataLayoutDescription;

  UniversalLinkageInfo UniversalLinkInfo;
  ModuleDecl *SwiftModule;
  const TBDGenOptions &Opts;
  APIRecorder &recorder;

  using SymbolKind = llvm::MachO::SymbolKind;

  std::vector<Decl*> DeclStack;
  std::unique_ptr<std::map<std::string, InstallNameStore>>
    previousInstallNameMap;
  std::unique_ptr<std::map<std::string, InstallNameStore>>
    parsePreviousModuleInstallNameMap();
  void addSymbolInternal(StringRef name, llvm::MachO::SymbolKind kind,
                         SymbolSource source);
  void addLinkerDirectiveSymbolsLdHide(StringRef name, llvm::MachO::SymbolKind kind);
  void addLinkerDirectiveSymbolsLdPrevious(StringRef name, llvm::MachO::SymbolKind kind);
  void addSymbol(StringRef name, SymbolSource source,
                 SymbolKind kind = SymbolKind::GlobalSymbol);

  void addSymbol(LinkEntity entity);
  
  bool addClassMetadata(ClassDecl *CD);

public:
  TBDGenVisitor(const llvm::Triple &target, const StringRef dataLayoutString,
                ModuleDecl *swiftModule, const TBDGenOptions &opts,
                APIRecorder &recorder)
      : DataLayoutDescription(dataLayoutString),
        UniversalLinkInfo(target, opts.HasMultipleIGMs, /*forcePublic*/ false,
                          /*static=*/false),
        SwiftModule(swiftModule), Opts(opts), recorder(recorder),
        previousInstallNameMap(parsePreviousModuleInstallNameMap()) {}

  /// Create a new visitor using the target and layout information from a
  /// TBDGenDescriptor.
  TBDGenVisitor(const TBDGenDescriptor &desc, APIRecorder &recorder);

  ~TBDGenVisitor() { assert(DeclStack.empty()); }

  /// Adds the global symbols associated with the first file.
  void addFirstFileSymbols();

  /// Visit the files specified by a given TBDGenDescriptor.
  void visit(const TBDGenDescriptor &desc);

  // --- IRSymbolVisitor ---

  bool willVisitDecl(Decl *D) override;
  void didVisitDecl(Decl *D) override;

  void addFunction(SILDeclRef declRef) override;
  void addFunction(StringRef name, SILDeclRef declRef) override;
  void addGlobalVar(VarDecl *VD) override;
  void addLinkEntity(LinkEntity entity) override;
  void addObjCInterface(ClassDecl *CD) override;
  void addObjCMethod(AbstractFunctionDecl *AFD) override;
  void addProtocolWitnessThunk(RootProtocolConformance *C,
                               ValueDecl *requirementDecl) override;
};
} // end namespace tbdgen
} // end namespace swift

#endif
