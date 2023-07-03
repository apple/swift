//===--- TestContext.cpp - Helper for setting up ASTContexts --------------===//
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

#include "TestContext.h"
#include "swift/AST/GenericParamList.h"
#include "swift/AST/Module.h"
#include "swift/AST/ParseRequests.h"
#include "swift/Strings.h"
#include "swift/Subsystems.h"

using namespace swift;
using namespace swift::unittest;

static Decl *createOptionalType(ASTContext &ctx, SourceFile *fileForLookups,
                                Identifier name) {
  auto *wrapped = GenericTypeParamDecl::createImplicit(
      fileForLookups, ctx.getIdentifier("Wrapped"),
      /*depth*/ 0, /*index*/ 0);
  auto params = GenericParamList::create(ctx, SourceLoc(), wrapped,
                                         SourceLoc());
  auto decl = new (ctx) EnumDecl(SourceLoc(), name, SourceLoc(),
                                 /*inherited*/{}, params, fileForLookups);
  wrapped->setDeclContext(decl);
  return decl;
}

TestContext::TestContext(ShouldDeclareOptionalTypes optionals)
    : Ctx(*ASTContext::get(LangOpts, TypeCheckerOpts, SILOpts, SearchPathOpts,
                           ClangImporterOpts, SymbolGraphOpts, SourceMgr,
                           Diags)) {
  registerParseRequestFunctions(Ctx.evaluator);
  registerTypeCheckerRequestFunctions(Ctx.evaluator);
  registerClangImporterRequestFunctions(Ctx.evaluator);
  auto stdlibID = Ctx.getIdentifier(STDLIB_NAME);
  auto *module = ModuleDecl::create(stdlibID, Ctx);
  Ctx.addLoadedModule(module);

  FileForLookups = new (Ctx) SourceFile(*module, SourceFileKind::Library,
                                        /*buffer*/ llvm::None);
  module->addFile(*FileForLookups);

  if (optionals == DeclareOptionalTypes) {
    SmallVector<ASTNode, 2> optionalTypes;
    optionalTypes.push_back(createOptionalType(
        Ctx, FileForLookups, Ctx.getIdentifier("Optional")));
    optionalTypes.push_back(createOptionalType(
        Ctx, FileForLookups, Ctx.getIdentifier("ImplicitlyUnwrappedOptional")));

    auto result = SourceFileParsingResult{Ctx.AllocateCopy(optionalTypes),
                                          /*tokens*/ llvm::None,
                                          /*interfaceHash*/ llvm::None};

    Ctx.evaluator.cacheOutput(ParseSourceFileRequest{FileForLookups},
                              std::move(result));
  }
}
