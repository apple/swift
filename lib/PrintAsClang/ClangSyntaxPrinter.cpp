//===--- ClangSyntaxPrinter.cpp - Printer for C and C++ code ----*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "ClangSyntaxPrinter.h"
#include "PrimitiveTypeMapping.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Module.h"
#include "swift/AST/SwiftNameTranslation.h"
#include "swift/AST/TypeCheckRequests.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/NestedNameSpecifier.h"

using namespace swift;
using namespace cxx_synthesis;

StringRef cxx_synthesis::getCxxSwiftNamespaceName() { return "swift"; }

StringRef cxx_synthesis::getCxxImplNamespaceName() { return "_impl"; }

StringRef cxx_synthesis::getCxxOpaqueStorageClassName() {
  return "OpaqueStorage";
}

bool ClangSyntaxPrinter::isClangKeyword(StringRef name) {
  static const llvm::DenseSet<StringRef> keywords = [] {
    llvm::DenseSet<StringRef> set;
    // FIXME: clang::IdentifierInfo /nearly/ has the API we need to do this
    // in a more principled way, but not quite.
#define KEYWORD(SPELLING, FLAGS) set.insert(#SPELLING);
#define CXX_KEYWORD_OPERATOR(SPELLING, TOK) set.insert(#SPELLING);
#include "clang/Basic/TokenKinds.def"
    return set;
  }();

  return keywords.contains(name);
}

bool ClangSyntaxPrinter::isClangKeyword(Identifier name) {
  if (name.empty())
    return false;
  return ClangSyntaxPrinter::isClangKeyword(name.str());
}

void ClangSyntaxPrinter::printIdentifier(StringRef name) const {
  os << name;
  if (ClangSyntaxPrinter::isClangKeyword(name))
    os << '_';
}

void ClangSyntaxPrinter::printBaseName(const ValueDecl *decl) const {
  assert(decl->getName().isSimpleName());
  printIdentifier(cxx_translation::getNameForCxx(decl));
}

void ClangSyntaxPrinter::printModuleNameCPrefix(const ModuleDecl &mod) {
  os << mod.getName().str() << '_';
}

void ClangSyntaxPrinter::printModuleNamespaceQualifiersIfNeeded(
    const ModuleDecl *referencedModule, const ModuleDecl *currentContext) {
  if (referencedModule == currentContext)
    return;
  printBaseName(referencedModule);
  os << "::";
}

bool ClangSyntaxPrinter::printNominalTypeOutsideMemberDeclTemplateSpecifiers(
    const NominalTypeDecl *typeDecl) {
  // FIXME: Full qualifiers for nested types?
  if (!typeDecl->isGeneric())
    return true;
  printGenericSignature(
      typeDecl->getGenericSignature().getCanonicalSignature());
  return false;
}

bool ClangSyntaxPrinter::printNominalTypeOutsideMemberDeclInnerStaticAssert(
    const NominalTypeDecl *typeDecl) {
  if (!typeDecl->isGeneric())
    return true;
  printGenericSignatureInnerStaticAsserts(
      typeDecl->getGenericSignature().getCanonicalSignature());
  return false;
}

void ClangSyntaxPrinter::printClangTypeReference(const clang::Decl *typeDecl) {
  auto &clangCtx = typeDecl->getASTContext();
  clang::PrintingPolicy pp(clangCtx.getLangOpts());
  const auto *NS = clang::NestedNameSpecifier::getRequiredQualification(
      clangCtx, clangCtx.getTranslationUnitDecl(),
      typeDecl->getLexicalDeclContext());
  if (NS)
    NS->print(os, pp);
  assert(cast<clang::NamedDecl>(typeDecl)->getDeclName().isIdentifier());
  os << cast<clang::NamedDecl>(typeDecl)->getName();
  if (auto *ctd = dyn_cast<clang::ClassTemplateSpecializationDecl>(typeDecl)) {
    if (ctd->getTemplateArgs().size()) {
      os << '<';
      llvm::interleaveComma(ctd->getTemplateArgs().asArray(), os,
                            [&](const clang::TemplateArgument &arg) {
                              arg.print(pp, os, /*IncludeType=*/true);
                            });
      os << '>';
    }
  }
}

void ClangSyntaxPrinter::printNominalTypeReference(
    const NominalTypeDecl *typeDecl, const ModuleDecl *moduleContext) {
  if (typeDecl->hasClangNode()) {
    printClangTypeReference(typeDecl->getClangDecl());
    return;
  }
  printModuleNamespaceQualifiersIfNeeded(typeDecl->getModuleContext(),
                                         moduleContext);
  // FIXME: Full qualifiers for nested types?
  ClangSyntaxPrinter(os).printBaseName(typeDecl);
  if (typeDecl->isGeneric())
    printGenericSignatureParams(
        typeDecl->getGenericSignature().getCanonicalSignature());
}

void ClangSyntaxPrinter::printNominalTypeQualifier(
    const NominalTypeDecl *typeDecl, const ModuleDecl *moduleContext) {
  printNominalTypeReference(typeDecl, moduleContext);
  os << "::";
}

void ClangSyntaxPrinter::printModuleNamespaceStart(
    const ModuleDecl &moduleContext) const {
  os << "namespace ";
  printBaseName(&moduleContext);
  os << " SWIFT_PRIVATE_ATTR";
  printSymbolUSRAttribute(&moduleContext);
  os << " {\n";
}

/// Print a C++ namespace declaration with the give name and body.
void ClangSyntaxPrinter::printNamespace(
    llvm::function_ref<void(raw_ostream &OS)> namePrinter,
    llvm::function_ref<void(raw_ostream &OS)> bodyPrinter,
    NamespaceTrivia trivia, const ModuleDecl *moduleContext) const {
  os << "namespace ";
  namePrinter(os);
  if (trivia == NamespaceTrivia::AttributeSwiftPrivate)
    os << " SWIFT_PRIVATE_ATTR";
  if (moduleContext)
    printSymbolUSRAttribute(moduleContext);
  os << " {\n\n";
  bodyPrinter(os);
  os << "\n} // namespace ";
  namePrinter(os);
  os << "\n\n";
}

void ClangSyntaxPrinter::printNamespace(
    StringRef name, llvm::function_ref<void(raw_ostream &OS)> bodyPrinter,
    NamespaceTrivia trivia) const {
  printNamespace([&](raw_ostream &os) { os << name; }, bodyPrinter, trivia);
}

void ClangSyntaxPrinter::printExternC(
    llvm::function_ref<void(raw_ostream &OS)> bodyPrinter) const {
  os << "#ifdef __cplusplus\n";
  os << "extern \"C\" {\n";
  os << "#endif\n\n";
  bodyPrinter(os);
  os << "\n#ifdef __cplusplus\n";
  os << "}\n";
  os << "#endif\n";
}

void ClangSyntaxPrinter::printObjCBlock(
    llvm::function_ref<void(raw_ostream &OS)> bodyPrinter) const {
  os << "#if defined(__OBJC__)\n";
  bodyPrinter(os);
  os << "\n#endif\n";
}

void ClangSyntaxPrinter::printSwiftImplQualifier() const {
  os << "swift::" << cxx_synthesis::getCxxImplNamespaceName() << "::";
}

void ClangSyntaxPrinter::printInlineForThunk() const {
  os << "SWIFT_INLINE_THUNK ";
}

void ClangSyntaxPrinter::printInlineForHelperFunction() const {
  os << "SWIFT_INLINE_PRIVATE_HELPER ";
}

void ClangSyntaxPrinter::printNullability(
    llvm::Optional<OptionalTypeKind> kind,
    NullabilityPrintKind printKind) const {
  if (!kind)
    return;

  switch (printKind) {
  case NullabilityPrintKind::ContextSensitive:
    switch (*kind) {
    case OTK_None:
      os << "nonnull";
      break;
    case OTK_Optional:
      os << "nullable";
      break;
    case OTK_ImplicitlyUnwrappedOptional:
      os << "null_unspecified";
      break;
    }
    break;
  case NullabilityPrintKind::After:
    os << ' ';
    LLVM_FALLTHROUGH;
  case NullabilityPrintKind::Before:
    switch (*kind) {
    case OTK_None:
      os << "_Nonnull";
      break;
    case OTK_Optional:
      os << "_Nullable";
      break;
    case OTK_ImplicitlyUnwrappedOptional:
      os << "_Null_unspecified";
      break;
    }
    break;
  }

  if (printKind != NullabilityPrintKind::After)
    os << ' ';
}

void ClangSyntaxPrinter::printSwiftTypeMetadataAccessFunctionCall(
    StringRef name, ArrayRef<GenericRequirement> requirements) {
  os << name << "(0";
  printGenericRequirementsInstantiantions(requirements, LeadingTrivia::Comma);
  os << ')';
}

void ClangSyntaxPrinter::printValueWitnessTableAccessSequenceFromTypeMetadata(
    StringRef metadataVariable, StringRef vwTableVariable, int indent) {
  os << std::string(indent, ' ');
  os << "auto *vwTableAddr = ";
  os << "reinterpret_cast<";
  printSwiftImplQualifier();
  os << "ValueWitnessTable **>(" << metadataVariable << "._0) - 1;\n";
  os << "#ifdef __arm64e__\n";
  os << std::string(indent, ' ');
  os << "auto *" << vwTableVariable << " = ";
  os << "reinterpret_cast<";
  printSwiftImplQualifier();
  os << "ValueWitnessTable *>(ptrauth_auth_data(";
  os << "reinterpret_cast<void *>(*vwTableAddr), "
        "ptrauth_key_process_independent_data, ";
  os << "ptrauth_blend_discriminator(vwTableAddr, "
     << SpecialPointerAuthDiscriminators::ValueWitnessTable << ")));\n";
  os << "#else\n";
  os << std::string(indent, ' ');
  os << "auto *" << vwTableVariable << " = *vwTableAddr;\n";
  os << "#endif\n";
}

void ClangSyntaxPrinter::printCTypeMetadataTypeFunction(
    const TypeDecl *typeDecl, StringRef typeMetadataFuncName,
    llvm::ArrayRef<GenericRequirement> genericRequirements) {
  // FIXME: Support generic requirements > 3.
  if (!genericRequirements.empty())
    os << "static_assert(" << genericRequirements.size()
       << " <= " << NumDirectGenericTypeMetadataAccessFunctionArgs
       << ", \"unsupported generic requirement list for metadata func\");\n";
  os << "// Type metadata accessor for " << typeDecl->getNameStr() << "\n";
  os << "SWIFT_EXTERN ";
  printSwiftImplQualifier();
  os << "MetadataResponseTy " << typeMetadataFuncName << '(';
  printSwiftImplQualifier();
  os << "MetadataRequestTy";
  if (!genericRequirements.empty())
    os << ", ";
  llvm::interleaveComma(genericRequirements, os,
                        [&](const GenericRequirement &) {
                          // FIXME: Print parameter name.
                          os << "void * _Nonnull";
                        });
  os << ')';
  os << " SWIFT_NOEXCEPT SWIFT_CALL;\n\n";
}

void ClangSyntaxPrinter::printGenericTypeParamTypeName(
    const GenericTypeParamType *gtpt) {
  os << "T_" << gtpt->getDepth() << '_' << gtpt->getIndex();
}

void ClangSyntaxPrinter::printGenericSignature(
    const CanGenericSignature &signature) {
  os << "template<";
  llvm::interleaveComma(signature.getInnermostGenericParams(), os,
                        [&](const GenericTypeParamType *genericParamType) {
                          os << "class ";
                          printGenericTypeParamTypeName(genericParamType);
                        });
  os << ">\n";
  os << "#ifdef __cpp_concepts\n";
  os << "requires ";
  llvm::interleave(
      signature.getInnermostGenericParams(), os,
      [&](const GenericTypeParamType *genericParamType) {
        os << "swift::isUsableInGenericContext<";
        printGenericTypeParamTypeName(genericParamType);
        os << ">";
      },
      " && ");
  os << "\n#endif // __cpp_concepts\n";
}

void ClangSyntaxPrinter::printGenericSignatureInnerStaticAsserts(
    const CanGenericSignature &signature) {
  os << "#ifndef __cpp_concepts\n";
  llvm::interleave(
      signature.getInnermostGenericParams(), os,
      [&](const GenericTypeParamType *genericParamType) {
        os << "static_assert(swift::isUsableInGenericContext<";
        printGenericTypeParamTypeName(genericParamType);
        os << ">, \"type cannot be used in a Swift generic context\");";
      },
      "\n");
  os << "\n#endif // __cpp_concepts\n";
}

void ClangSyntaxPrinter::printGenericSignatureParams(
    const CanGenericSignature &signature) {
  os << '<';
  llvm::interleaveComma(signature.getInnermostGenericParams(), os,
                        [&](const GenericTypeParamType *genericParamType) {
                          printGenericTypeParamTypeName(genericParamType);
                        });
  os << '>';
}

void ClangSyntaxPrinter::printGenericRequirementInstantiantion(
    const GenericRequirement &requirement) {
  assert(requirement.isAnyMetadata() &&
         "protocol requirements not supported yet!");
  auto *gtpt = requirement.getTypeParameter()->getAs<GenericTypeParamType>();
  assert(gtpt && "unexpected generic param type");
  os << "swift::TypeMetadataTrait<";
  printGenericTypeParamTypeName(gtpt);
  os << ">::getTypeMetadata()";
}

void ClangSyntaxPrinter::printGenericRequirementsInstantiantions(
    ArrayRef<GenericRequirement> requirements, LeadingTrivia leadingTrivia) {
  if (leadingTrivia == LeadingTrivia::Comma && !requirements.empty())
    os << ", ";
  llvm::interleaveComma(requirements, os,
                        [&](const GenericRequirement &requirement) {
                          printGenericRequirementInstantiantion(requirement);
                        });
}

void ClangSyntaxPrinter::printPrimaryCxxTypeName(
    const NominalTypeDecl *type, const ModuleDecl *moduleContext) {
  printModuleNamespaceQualifiersIfNeeded(type->getModuleContext(),
                                         moduleContext);
  // FIXME: Print class qualifiers for nested class references.
  printBaseName(type);
}

void ClangSyntaxPrinter::printIncludeForShimHeader(StringRef headerName) {
  printIgnoredDiagnosticBlock("non-modular-include-in-framework-module", [&] {
    os << "// Look for the C++ interop support header relative to clang's "
          "resource dir:\n";
    os << "//  "
          "'<toolchain>/usr/lib/clang/<version>/include/../../../swift/"
          "swiftToCxx'.\n";
    os << "#if __has_include(<../../../swift/swiftToCxx/" << headerName
       << ">)\n";
    os << "#include <../../../swift/swiftToCxx/" << headerName << ">\n";
    os << "#elif __has_include(<../../../../../lib/swift/swiftToCxx/"
       << headerName << ">)\n";
    os << "//  "
          "'<toolchain>/usr/local/lib/clang/<version>/include/../../../../../"
          "lib/"
          "swift/swiftToCxx'.\n";
    os << "#include <../../../../../lib/swift/swiftToCxx/" << headerName
       << ">\n";
    os << "// Alternatively, allow user to find the header using additional "
          "include path into '<toolchain>/lib/swift'.\n";
    os << "#elif __has_include(<swiftToCxx/" << headerName << ">)\n";
    os << "#include <swiftToCxx/" << headerName << ">\n";
    os << "#endif\n";
  });
}

void ClangSyntaxPrinter::printDefine(StringRef macroName) {
  os << "#define " << macroName << "\n";
}

void ClangSyntaxPrinter::printIgnoredDiagnosticBlock(
    StringRef diagName, llvm::function_ref<void()> bodyPrinter) {
  os << "#pragma clang diagnostic push\n";
  os << "#pragma clang diagnostic ignored \"-W" << diagName << "\"\n";
  bodyPrinter();
  os << "#pragma clang diagnostic pop\n";
}

void ClangSyntaxPrinter::printIgnoredCxx17ExtensionDiagnosticBlock(
    llvm::function_ref<void()> bodyPrinter) {
  printIgnoredDiagnosticBlock("c++17-extensions", bodyPrinter);
}

void ClangSyntaxPrinter::printSymbolUSRAttribute(const ValueDecl *D) const {
  if (isa<ModuleDecl>(D)) {
    os << " SWIFT_SYMBOL_MODULE(\"";
    printBaseName(D);
    os << "\")";
    return;
  }
  auto result = evaluateOrDefault(D->getASTContext().evaluator,
                                  USRGenerationRequest{D}, std::string());
  if (result.empty())
    return;
  os << " SWIFT_SYMBOL(\"" << result << "\")";
}

void ClangSyntaxPrinter::printKnownCType(
    Type t, PrimitiveTypeMapping &typeMapping) const {
  auto info =
      typeMapping.getKnownCTypeInfo(t->getNominalOrBoundGenericNominal());
  assert(info.has_value() && "not a known type");
  os << info->name;
  if (info->canBeNullable)
    os << " _Null_unspecified";
}

void ClangSyntaxPrinter::printSwiftMangledNameForDebugger(
    const NominalTypeDecl *typeDecl) {
  printIgnoredCxx17ExtensionDiagnosticBlock([&]() {

  os << "#pragma clang diagnostic push\n";
  os << "#pragma clang diagnostic ignored \"-Wreserved-identifier\"\n";
    auto mangled_name = mangler.mangleTypeForDebugger(
        typeDecl->getDeclaredInterfaceType(), nullptr);
    if (!mangled_name.empty()) {
      os << "  typedef char " << mangled_name << ";\n";
      os << "  static inline constexpr " << mangled_name
         << " __swift_mangled_name = 0;\n";
    }
  });
  os << "#pragma clang diagnostic pop\n";
}
