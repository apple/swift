//===--- PrintClangValueType.cpp - Printer for C/C++ value types *- C++ -*-===//
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

#include "PrintClangValueType.h"
#include "ClangSyntaxPrinter.h"
#include "DeclAndTypePrinter.h"
#include "OutputLanguageMode.h"
#include "PrimitiveTypeMapping.h"
#include "SwiftToClangInteropContext.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/Decl.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Type.h"
#include "swift/AST/TypeVisitor.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/IRGen/IRABIDetailsProvider.h"
#include "swift/IRGen/Linking.h"
#include "llvm/ADT/STLExtras.h"

using namespace swift;

/// Print out the C type name of a struct/enum declaration.
static void printCTypeName(raw_ostream &os, const NominalTypeDecl *type) {
  ClangSyntaxPrinter printer(os);
  printer.printModuleNameCPrefix(*type->getParentModule());
  // FIXME: add nested type qualifiers to fully disambiguate the name.
  printer.printBaseName(type);
}

/// Print out the C++ type name of a struct/enum declaration.
static void printCxxTypeName(raw_ostream &os, const NominalTypeDecl *type,
                             const ModuleDecl *moduleContext) {
  ClangSyntaxPrinter(os).printPrimaryCxxTypeName(type, moduleContext);
}

void ClangValueTypePrinter::printCxxImplClassName(raw_ostream &os,
                                                  const NominalTypeDecl *type) {
  os << "_impl_";
  ClangSyntaxPrinter(os).printBaseName(type);
}

void ClangValueTypePrinter::printMetadataAccessAsVariable(
    raw_ostream &os, StringRef metadataFuncName,
    ArrayRef<GenericRequirement> genericRequirements, int indent,
    StringRef varName) {
  ClangSyntaxPrinter printer(os);
  os << std::string(indent, ' ') << "auto " << varName << " = "
     << cxx_synthesis::getCxxImplNamespaceName() << "::";
  printer.printSwiftTypeMetadataAccessFunctionCall(metadataFuncName,
                                                   genericRequirements);
  os << ";\n";
}

void ClangValueTypePrinter::printValueWitnessTableAccessAsVariable(
    raw_ostream &os, StringRef metadataFuncName,
    ArrayRef<GenericRequirement> genericRequirements, int indent,
    StringRef metadataVarName, StringRef vwTableVarName) {
  ClangSyntaxPrinter printer(os);
  printMetadataAccessAsVariable(os, metadataFuncName, genericRequirements,
                                indent, metadataVarName);
  printer.printValueWitnessTableAccessSequenceFromTypeMetadata(
      metadataVarName, vwTableVarName, indent);
}

static void
printCValueTypeStorageStruct(raw_ostream &os, const NominalTypeDecl *typeDecl,
                             IRABIDetailsProvider::SizeAndAlignment layout) {
  os << "struct ";
  printCTypeName(os, typeDecl);
  os << " {\n";
  os << "  _Alignas(" << layout.alignment << ") ";
  os << "char _storage[" << layout.size << "];\n";
  os << "};\n\n";
}

void ClangValueTypePrinter::forwardDeclType(
    raw_ostream &os, const NominalTypeDecl *typeDecl,
    DeclAndTypePrinter &declAndTypePrinter) {
  if (typeDecl->isGeneric()) {
    auto genericSignature =
        typeDecl->getGenericSignature().getCanonicalSignature();
    ClangSyntaxPrinter(os).printGenericSignature(genericSignature);
  }
  os << "class";
  declAndTypePrinter.printAvailability(os, typeDecl);
  ClangSyntaxPrinter(os).printSymbolUSRAttribute(typeDecl);
  os << ' ';
  ClangSyntaxPrinter(os).printBaseName(typeDecl);
  os << ";\n";
  printTypePrecedingGenericTraits(os, typeDecl, typeDecl->getModuleContext());
}

static void addCppExtensionsToStdlibType(const NominalTypeDecl *typeDecl,
                                         raw_ostream &os,
                                         ClangSyntaxPrinter &printer,
                                         raw_ostream &cPrologueOS) {
  if (typeDecl == typeDecl->getASTContext().getStringDecl()) {
    // Perform String -> NSString conversion using
    // _bridgeToObjectiveCImpl.
    // FIXME: This is an extension, we should
    // just expose the method to C once extensions are
    // supported.
    // FIXME: This C passing should not be here, remove it.
    cPrologueOS << "struct swift_interop_stub_Swift_String {\n"
                   "#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFu\n"
                   "uint64_t _1;\n"
                   "void * _Nullable _2;\n"
                   "#elif UINTPTR_MAX == 0xFFFFFFFF\n"
                   "uint32_t _1;\n"
                   "uint32_t _2;\n"
                   "uint32_t _3;\n"
                   "#endif\n"
                   "};\n";
    cPrologueOS
        << "static SWIFT_INLINE_THUNK struct swift_interop_stub_Swift_String "
           "swift_interop_passDirect_Swift_String(const char * "
           "_Nonnull value) {\n"
           "struct swift_interop_stub_Swift_String result;\n"
           "#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFu\n"
           "memcpy(&result._1, value, 8);\n"
           "memcpy(&result._2, value + 8, 8);\n"
           "#elif UINTPTR_MAX == 0xFFFFFFFF\n"
           "memcpy(&result._1, value, 4);\n"
           "memcpy(&result._2, value + 4, 4);\n"
           "memcpy(&result._3, value + 8, 4);\n"
           "#endif\n"
           "return result;\n"
           "}\n";
    cPrologueOS << "SWIFT_EXTERN void *_Nonnull "
                   "$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF(swift_interop_stub_"
                   "Swift_String) SWIFT_NOEXCEPT SWIFT_CALL;\n";
    cPrologueOS << "SWIFT_EXTERN swift_interop_stub_Swift_String "
                   "$sSS10FoundationE36_"
                   "unconditionallyBridgeFromObjectiveCySSSo8NSStringCSgFZ("
                   "void * _Nullable) SWIFT_NOEXCEPT SWIFT_CALL;\n";
    cPrologueOS << "SWIFT_EXTERN swift_interop_stub_Swift_String "
                   "$sSS7cStringSSSPys4Int8VG_tcfC("
                   "const char * _Nonnull) SWIFT_NOEXCEPT SWIFT_CALL;\n";
    printer.printObjCBlock([](raw_ostream &os) {
      os << "  ";
      ClangSyntaxPrinter(os).printInlineForThunk();
      os << "operator NSString * _Nonnull () const noexcept {\n";
      os << "    return (__bridge_transfer NSString "
            "*)(_impl::$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF(_impl::swift_interop_"
            "passDirect_Swift_String(_getOpaquePointer())));\n";
      os << "  }\n";
      os << "static ";
      ClangSyntaxPrinter(os).printInlineForThunk();
      os << "String init(NSString * _Nonnull nsString) noexcept {\n";
      os << "    auto result = _make();\n";
      os << "    auto res = "
            "_impl::$sSS10FoundationE36_"
            "unconditionallyBridgeFromObjectiveCySSSo8NSStringCSgFZ((__bridge "
            "void *)nsString);\n";
      os << "    memcpy(result._getOpaquePointer(), &res, sizeof(res));\n";
      os << "    return result;\n";
      os << "  }\n";
    });
    // Add additional methods for the `String` declaration.
    printer.printDefine("SWIFT_CXX_INTEROP_STRING_MIXIN");
    printer.printIncludeForShimHeader("_SwiftStdlibCxxOverlay.h");
  } else if (typeDecl == typeDecl->getASTContext().getOptionalDecl()) {
    // Add additional methods for the `Optional` declaration.
    printer.printDefine("SWIFT_CXX_INTEROP_OPTIONAL_MIXIN");
    printer.printIncludeForShimHeader("_SwiftStdlibCxxOverlay.h");
  }
}

void ClangValueTypePrinter::printValueTypeDecl(
    const NominalTypeDecl *typeDecl, llvm::function_ref<void(void)> bodyPrinter,
    DeclAndTypePrinter &declAndTypePrinter) {
  // FIXME: Add support for generic structs.
  llvm::Optional<IRABIDetailsProvider::SizeAndAlignment> typeSizeAlign;
  Optional<CanGenericSignature> genericSignature;
  auto printGenericSignature = [&](raw_ostream &os) {
    if (!genericSignature)
      return;
    ClangSyntaxPrinter(os).printGenericSignature(*genericSignature);
  };
  auto printGenericParamRefs = [&](raw_ostream &os) {
    if (!genericSignature)
      return;
    ClangSyntaxPrinter(os).printGenericSignatureParams(*genericSignature);
  };
  if (typeDecl->isGeneric()) {
    genericSignature = typeDecl->getGenericSignature().getCanonicalSignature();
    // FIXME: Support generic requirements.
    assert(genericSignature->getRequirements().empty());
    // FIXME: Can we make some better layout than opaque layout for generic
    // types.
  } else if (!typeDecl->isResilient()) {
    typeSizeAlign =
        interopContext.getIrABIDetails().getTypeSizeAlignment(typeDecl);
    // typeSizeAlign can be null if this is not a fixed-layout type,
    // e.g. it has resilient fields.
    if (typeSizeAlign && typeSizeAlign->size == 0) {
      // FIXME: How to represent 0 sized structs?
      return;
    }
  }
  bool isOpaqueLayout = !typeSizeAlign.has_value();

  ClangSyntaxPrinter printer(os);

  auto typeMetadataFunc = irgen::LinkEntity::forTypeMetadataAccessFunction(
      typeDecl->getDeclaredType()->getCanonicalType());
  std::string typeMetadataFuncName = typeMetadataFunc.mangleAsString();
  auto typeMetadataFuncGenericParams =
      interopContext.getIrABIDetails()
          .getTypeMetadataAccessFunctionGenericRequirementParameters(
              const_cast<NominalTypeDecl *>(typeDecl));

  // Print out a forward declaration of the "hidden" _impl class.
  printer.printNamespace(cxx_synthesis::getCxxImplNamespaceName(),
                         [&](raw_ostream &os) {
                           printGenericSignature(os);
                           os << "class";
                           declAndTypePrinter.printAvailability(os, typeDecl);
                           os << ' ';
                           printCxxImplClassName(os, typeDecl);
                           os << ";\n\n";

                           // Print out special functions, like functions that
                           // access type metadata.
                           printer.printCTypeMetadataTypeFunction(
                               typeDecl, typeMetadataFuncName,
                               typeMetadataFuncGenericParams);
                           // Print out global variables for resilient enum
                           // cases
                           if (isa<EnumDecl>(typeDecl) && isOpaqueLayout) {
                             auto elementTagMapping =
                                 interopContext.getIrABIDetails()
                                     .getEnumTagMapping(
                                         cast<EnumDecl>(typeDecl));
                             os << "// Tags for resilient enum ";
                             os << typeDecl->getName().str() << '\n';
                             os << "extern \"C\" {\n";
                             for (const auto &pair : elementTagMapping) {
                               os << "extern unsigned "
                                  << pair.second.globalVariableName << ";\n";
                             }
                             os << "}\n";
                           }
                         });

  auto printEnumVWTableVariable = [&](StringRef metadataName = "metadata",
                                      StringRef vwTableName = "vwTable",
                                      StringRef enumVWTableName =
                                          "enumVWTable") {
    ClangValueTypePrinter::printValueWitnessTableAccessAsVariable(
        os, typeMetadataFuncName, typeMetadataFuncGenericParams);
    os << "    const auto *" << enumVWTableName << " = reinterpret_cast<";
    ClangSyntaxPrinter(os).printSwiftImplQualifier();
    os << "EnumValueWitnessTable";
    os << " *>(" << vwTableName << ");\n";
  };

  // Print out the C++ class itself.
  printGenericSignature(os);
  os << "class";
  declAndTypePrinter.printAvailability(os, typeDecl);
  ClangSyntaxPrinter(os).printSymbolUSRAttribute(typeDecl);
  os << ' ';
  ClangSyntaxPrinter(os).printBaseName(typeDecl);
  os << " final {\n";
  os << "public:\n";
  if (genericSignature)
    ClangSyntaxPrinter(os).printGenericSignatureInnerStaticAsserts(
        *genericSignature);

  // Print out the destructor.
  os << "  ";
  printer.printInlineForThunk();
  os << '~';
  printer.printBaseName(typeDecl);
  os << "() noexcept {\n";
  ClangValueTypePrinter::printValueWitnessTableAccessAsVariable(
      os, typeMetadataFuncName, typeMetadataFuncGenericParams);
  os << "    vwTable->destroy(_getOpaquePointer(), metadata._0);\n";
  os << "  }\n";

  // copy constructor.
  os << "  ";
  printer.printInlineForThunk();
  printer.printBaseName(typeDecl);
  os << "(const ";
  printer.printBaseName(typeDecl);
  os << " &other) noexcept {\n";
  ClangValueTypePrinter::printValueWitnessTableAccessAsVariable(
      os, typeMetadataFuncName, typeMetadataFuncGenericParams);
  if (isOpaqueLayout) {
    os << "    _storage = ";
    printer.printSwiftImplQualifier();
    os << cxx_synthesis::getCxxOpaqueStorageClassName()
       << "(vwTable->size, vwTable->getAlignment());\n";
  }
  os << "    vwTable->initializeWithCopy(_getOpaquePointer(), const_cast<char "
        "*>(other._getOpaquePointer()), metadata._0);\n";
  os << "  }\n";

  // copy assignment.
  os << "  ";
  printer.printInlineForThunk();
  printer.printBaseName(typeDecl);
  os << " &operator =(const ";
  printer.printBaseName(typeDecl);
  os << " &other) noexcept {\n";
  ClangValueTypePrinter::printValueWitnessTableAccessAsVariable(
      os, typeMetadataFuncName, typeMetadataFuncGenericParams);
  os << "    vwTable->assignWithCopy(_getOpaquePointer(), const_cast<char "
        "*>(other._getOpaquePointer()), metadata._0);\n";
  os << "  return *this;\n";
  os << "  }\n";

  // FIXME: implement the move assignment.
  os << "  ";
  printer.printInlineForThunk();
  printer.printBaseName(typeDecl);
  os << " &operator =(";
  printer.printBaseName(typeDecl);
  os << " &&other) = delete;\n";

  // FIXME: implement the move constructor.
  os << "  [[noreturn]] ";
  // NOTE: Do not apply attribute((used))
  // here to ensure the linker error isn't
  // forced, so just mark this an inline
  // helper function instead.
  printer.printInlineForHelperFunction();
  printer.printBaseName(typeDecl);
  os << "(";
  printer.printBaseName(typeDecl);
  StringRef moveErrorMessage = "C++ does not support moving a Swift value yet";
  os << " &&) noexcept {\n  "
        "swift::_impl::_fatalError_Cxx_move_of_Swift_value_type_not_supported_"
        "yet();\n  swift::_impl::_swift_stdlib_reportFatalError(\"swift\", 5, "
        "\""
     << moveErrorMessage << "\", " << moveErrorMessage.size()
     << ", 0);\n  abort();\n  }\n";

  bodyPrinter();
  if (typeDecl->isStdlibDecl())
    addCppExtensionsToStdlibType(typeDecl, os, printer, cPrologueOS);

  os << "private:\n";

  // Print out private default constructor.
  os << "  ";
  printer.printInlineForThunk();
  printer.printBaseName(typeDecl);
  if (isOpaqueLayout) {
    os << "(";
    printer.printSwiftImplQualifier();
    os << "ValueWitnessTable * _Nonnull vwTable) noexcept : "
          "_storage(vwTable->size, "
          "vwTable->getAlignment()) {}\n";
  } else {
    os << "() noexcept {}\n";
  }
  // Print out '_make' function which returns an unitialized instance for
  // passing to Swift.
  os << "  static ";
  printer.printInlineForThunk();
  printer.printBaseName(typeDecl);
  os << " _make() noexcept {";
  if (isOpaqueLayout) {
    os << "\n";
    ClangValueTypePrinter::printValueWitnessTableAccessAsVariable(
        os, typeMetadataFuncName, typeMetadataFuncGenericParams);
    os << "    return ";
    printer.printBaseName(typeDecl);
    os << "(vwTable);\n  }\n";
  } else {
    os << " return ";
    printer.printBaseName(typeDecl);
    os << "(); }\n";
  }
  // Print out the private accessors to the underlying Swift value storage.
  os << "  ";
  printer.printInlineForThunk();
  os << "const char * _Nonnull _getOpaquePointer() const noexcept { return "
        "_storage";
  if (isOpaqueLayout)
    os << ".getOpaquePointer()";
  os << "; }\n";
  os << "  ";
  printer.printInlineForThunk();
  os << "char * _Nonnull _getOpaquePointer() noexcept { return _storage";
  if (isOpaqueLayout)
    os << ".getOpaquePointer()";
  os << "; }\n";
  os << "\n";
  // Print out helper function for enums
  if (isa<EnumDecl>(typeDecl)) {
    os << "  ";
    printer.printInlineForThunk();
    os << "char * _Nonnull _destructiveProjectEnumData() noexcept {\n";
    printEnumVWTableVariable();
    os << "    enumVWTable->destructiveProjectEnumData(_getOpaquePointer(), "
          "metadata._0);\n";
    os << "    return _getOpaquePointer();\n";
    os << "  }\n";
    os << "  ";
    printer.printInlineForThunk();
    os << "void _destructiveInjectEnumTag(unsigned tag) noexcept {\n";
    printEnumVWTableVariable();
    os << "    enumVWTable->destructiveInjectEnumTag(_getOpaquePointer(), tag, "
          "metadata._0);\n";
    os << "  }\n";
    os << "  ";
    printer.printInlineForThunk();
    os << "unsigned _getEnumTag() const noexcept {\n";
    printEnumVWTableVariable();
    os << "    return enumVWTable->getEnumTag(_getOpaquePointer(), "
          "metadata._0);\n";
    os << "  }\n";
  }
  // Print out the storage for the value type.
  os << "  ";
  if (isOpaqueLayout) {
    printer.printSwiftImplQualifier();
    os << cxx_synthesis::getCxxOpaqueStorageClassName() << " _storage;\n";
  } else {
    os << "alignas(" << typeSizeAlign->alignment << ") ";
    os << "char _storage[" << typeSizeAlign->size << "];\n";
  }
  // Wrap up the value type.
  os << "  friend class " << cxx_synthesis::getCxxImplNamespaceName() << "::";
  printCxxImplClassName(os, typeDecl);
  printGenericParamRefs(os);
  os << ";\n";

  printer.printSwiftMangledNameForDebugger(typeDecl);

  os << "};\n";
  os << '\n';

  const auto *moduleContext = typeDecl->getModuleContext();
  // Print out the "hidden" _impl class.
  printer.printNamespace(
      cxx_synthesis::getCxxImplNamespaceName(), [&](raw_ostream &os) {
        printGenericSignature(os);
        os << "class";
        declAndTypePrinter.printAvailability(os, typeDecl);
        os << ' ';
        printCxxImplClassName(os, typeDecl);
        os << " {\n";
        os << "public:\n";
        if (genericSignature)
          ClangSyntaxPrinter(os).printGenericSignatureInnerStaticAsserts(
              *genericSignature);

        os << "  static ";
        ClangSyntaxPrinter(os).printInlineForThunk();
        os << "char * _Nonnull getOpaquePointer(";
        printCxxTypeName(os, typeDecl, moduleContext);
        printGenericParamRefs(os);
        os << " &object) { return object._getOpaquePointer(); }\n";

        os << "  static ";
        ClangSyntaxPrinter(os).printInlineForThunk();
        os << "const char * _Nonnull getOpaquePointer(const ";
        printCxxTypeName(os, typeDecl, moduleContext);
        printGenericParamRefs(os);
        os << " &object) { return object._getOpaquePointer(); }\n";

        os << "  template<class T>\n";
        os << "  static ";
        ClangSyntaxPrinter(os).printInlineForHelperFunction();
        printCxxTypeName(os, typeDecl, moduleContext);
        printGenericParamRefs(os);
        os << " returnNewValue(T callable) {\n";
        os << "    auto result = ";
        printCxxTypeName(os, typeDecl, moduleContext);
        printGenericParamRefs(os);
        os << "::_make();\n";
        os << "    callable(result._getOpaquePointer());\n";
        os << "    return result;\n";
        os << "  }\n";
        // Print out helper function for initializeWithTake
        os << "  static ";
        ClangSyntaxPrinter(os).printInlineForThunk();
        os << "void initializeWithTake(char * _Nonnull "
              "destStorage, char * _Nonnull srcStorage) {\n";
        ClangValueTypePrinter::printValueWitnessTableAccessAsVariable(
            os, typeMetadataFuncName, typeMetadataFuncGenericParams);
        os << "    vwTable->initializeWithTake(destStorage, srcStorage, "
              "metadata._0);\n";
        os << "  }\n";
        os << "};\n";
      });

  if (!isOpaqueLayout)
    printCValueTypeStorageStruct(cPrologueOS, typeDecl, *typeSizeAlign);

  printTypeGenericTraits(os, typeDecl, typeMetadataFuncName,
                         typeMetadataFuncGenericParams,
                         typeDecl->getModuleContext(), declAndTypePrinter);
}

void ClangValueTypePrinter::printParameterCxxToCUseScaffold(
    const ModuleDecl *moduleContext, llvm::function_ref<void()> typePrinter,
    llvm::function_ref<void()> cxxParamPrinter, bool isSelf) {
  // A Swift value type is passed to its underlying Swift function
  if (isSelf) {
    os << "_getOpaquePointer()";
  } else {
    // FIXME: can we propagate the _impl request here?
    typePrinter();
    os << "::getOpaquePointer(";
    cxxParamPrinter();
    os << ')';
  }
}

void ClangValueTypePrinter::printValueTypeReturnType(
    const NominalTypeDecl *type, OutputLanguageMode outputLang,
    TypeUseKind typeUse, const ModuleDecl *moduleContext) {
  assert(isa<StructDecl>(type) || isa<EnumDecl>(type));
  assert(outputLang == OutputLanguageMode::Cxx);
  // FIXME: make a type use.
  if (outputLang == OutputLanguageMode::Cxx) {
    if (typeUse == TypeUseKind::CxxTypeName)
      printCxxTypeName(os, type, moduleContext);
    else {
      assert(typeUse == TypeUseKind::CxxImplTypeName);
      ClangSyntaxPrinter(os).printModuleNamespaceQualifiersIfNeeded(
          type->getModuleContext(), moduleContext);
      os << cxx_synthesis::getCxxImplNamespaceName() << "::";
      printCxxImplClassName(os, type);
    }
  }
}

void ClangValueTypePrinter::printValueTypeReturnScaffold(
    const NominalTypeDecl *type, const ModuleDecl *moduleContext,
    llvm::function_ref<void()> typePrinter,
    llvm::function_ref<void(StringRef)> bodyPrinter) {
  assert(isa<StructDecl>(type) || isa<EnumDecl>(type));
  os << "  return ";
  typePrinter();
  os << "::returnNewValue([&](char * _Nonnull result) "
        "SWIFT_INLINE_THUNK_ATTRIBUTES {\n    ";
  bodyPrinter("result");
  os << ";\n";
  os << "  });\n";
}

void ClangValueTypePrinter::printClangTypeSwiftGenericTraits(
    raw_ostream &os, const TypeDecl *typeDecl, const ModuleDecl *moduleContext,
    DeclAndTypePrinter &declAndTypePrinter) {
  assert(typeDecl->hasClangNode());
  // Do not reference unspecialized templates.
  if (isa<clang::ClassTemplateDecl>(typeDecl->getClangDecl()))
    return;
  auto typeMetadataFunc = irgen::LinkEntity::forTypeMetadataAccessFunction(
      typeDecl->getDeclaredInterfaceType()->getCanonicalType());
  std::string typeMetadataFuncName = typeMetadataFunc.mangleAsString();
  printTypeGenericTraits(os, typeDecl, typeMetadataFuncName,
                         /*typeMetadataFuncRequirements=*/{}, moduleContext,
                         declAndTypePrinter);
}

void ClangValueTypePrinter::printTypePrecedingGenericTraits(
    raw_ostream &os, const NominalTypeDecl *typeDecl,
    const ModuleDecl *moduleContext) {
  assert(!typeDecl->hasClangNode());
  ClangSyntaxPrinter printer(os);
  // FIXME: avoid popping out of the module's namespace here.
  os << "} // end namespace \n\n";
  os << "namespace swift SWIFT_PRIVATE_ATTR {\n";

  os << "#pragma clang diagnostic push\n";
  os << "#pragma clang diagnostic ignored \"-Wc++17-extensions\"\n";
  if (!typeDecl->isGeneric()) {
    // FIXME: generic type support.
    os << "template<>\n";
    os << "static inline const constexpr bool isUsableInGenericContext<";
    printer.printNominalTypeReference(typeDecl,
                                      /*moduleContext=*/nullptr);
    os << "> = true;\n";
  }
  os << "#pragma clang diagnostic pop\n";
  os << "} // namespace swift\n";
  os << "\n";
  printer.printModuleNamespaceStart(*moduleContext);
}

void ClangValueTypePrinter::printTypeGenericTraits(
    raw_ostream &os, const TypeDecl *typeDecl, StringRef typeMetadataFuncName,
    ArrayRef<GenericRequirement> typeMetadataFuncRequirements,
    const ModuleDecl *moduleContext, DeclAndTypePrinter &declAndTypePrinter) {
  auto *NTD = dyn_cast<NominalTypeDecl>(typeDecl);
  ClangSyntaxPrinter printer(os);
  // FIXME: avoid popping out of the module's namespace here.
  os << "} // end namespace \n\n";
  os << "namespace swift SWIFT_PRIVATE_ATTR {\n";

  if (typeDecl->hasClangNode()) {
    /// Print a reference to the type metadata fucntion for a C++ type.
    ClangSyntaxPrinter(os).printNamespace(
        cxx_synthesis::getCxxImplNamespaceName(), [&](raw_ostream &os) {
          ClangSyntaxPrinter(os).printCTypeMetadataTypeFunction(
              typeDecl, typeMetadataFuncName, typeMetadataFuncRequirements);
        });
  }

  os << "#pragma clang diagnostic push\n";
  os << "#pragma clang diagnostic ignored \"-Wc++17-extensions\"\n";
  if (typeDecl->hasClangNode()) {
    // FIXME: share the code.
    os << "template<>\n";
    os << "static inline const constexpr bool isUsableInGenericContext<";
    printer.printClangTypeReference(typeDecl->getClangDecl());
    os << "> = true;\n";
  }
  if (!NTD || printer.printNominalTypeOutsideMemberDeclTemplateSpecifiers(NTD))
    os << "template<>\n";
  os << "struct";
  declAndTypePrinter.printAvailability(os, typeDecl);
  os << " TypeMetadataTrait<";
  if (typeDecl->hasClangNode()) {
    printer.printClangTypeReference(typeDecl->getClangDecl());
  } else {
    assert(NTD);
    printer.printNominalTypeReference(NTD,
                                      /*moduleContext=*/nullptr);
  }
  os << "> {\n";
  os << "  static ";
  ClangSyntaxPrinter(os).printInlineForThunk();
  os << "void * _Nonnull getTypeMetadata() {\n";
  os << "    return ";
  if (!typeDecl->hasClangNode()) {
    printer.printBaseName(typeDecl->getModuleContext());
    os << "::";
  }
  os << cxx_synthesis::getCxxImplNamespaceName() << "::";
  ClangSyntaxPrinter(os).printSwiftTypeMetadataAccessFunctionCall(
      typeMetadataFuncName, typeMetadataFuncRequirements);
  os << "._0;\n";
  os << "  }\n};\n";

  os << "namespace " << cxx_synthesis::getCxxImplNamespaceName() << "{\n";

  if (typeDecl->hasClangNode()) {
    os << "template<>\n";
    os << "static inline const constexpr bool isSwiftBridgedCxxRecord<";
    printer.printClangTypeReference(typeDecl->getClangDecl());
    os << "> = true;\n";
  }

  if (!isa<ClassDecl>(typeDecl) && !typeDecl->hasClangNode() &&
      typeMetadataFuncRequirements.empty()) {
    // FIXME: generic support.
    os << "template<>\n";
    os << "static inline const constexpr bool isValueType<";
    printer.printBaseName(typeDecl->getModuleContext());
    os << "::";
    printer.printBaseName(typeDecl);
    os << "> = true;\n";
    if (NTD && NTD->isResilient()) {
      os << "template<>\n";
      os << "static inline const constexpr bool isOpaqueLayout<";
      printer.printBaseName(typeDecl->getModuleContext());
      os << "::";
      printer.printBaseName(typeDecl);
      os << "> = true;\n";
    }
  }

  // FIXME: generic support.
  if (!typeDecl->hasClangNode() && typeMetadataFuncRequirements.empty()) {
    assert(NTD);
    os << "template<>\n";
    os << "struct";
    declAndTypePrinter.printAvailability(os, typeDecl);
    os << " implClassFor<";
    printer.printBaseName(typeDecl->getModuleContext());
    os << "::";
    printer.printBaseName(typeDecl);
    os << "> { using type = ";
    printer.printBaseName(typeDecl->getModuleContext());
    os << "::" << cxx_synthesis::getCxxImplNamespaceName() << "::";
    printCxxImplClassName(os, NTD);
    os << "; };\n";
  }
  os << "} // namespace\n";
  os << "#pragma clang diagnostic pop\n";
  os << "} // namespace swift\n";
  os << "\n";
  printer.printModuleNamespaceStart(*moduleContext);
}
