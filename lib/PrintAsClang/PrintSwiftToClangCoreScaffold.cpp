//===--- PrintSwiftToClangCoreScaffold.cpp - Print core decls ---*- C++ -*-===//
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

#include "PrintSwiftToClangCoreScaffold.h"
#include "ClangSyntaxPrinter.h"
#include "PrimitiveTypeMapping.h"
#include "SwiftToClangInteropContext.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Type.h"
#include "swift/IRGen/IRABIDetailsProvider.h"
#include "swift/IRGen/Linking.h"
#include "clang/Basic/TargetInfo.h"
#include "llvm/ADT/STLExtras.h"

using namespace swift;

static void printKnownStruct(
    PrimitiveTypeMapping &typeMapping, raw_ostream &os, StringRef name,
    const IRABIDetailsProvider::TypeRecordABIRepresentation &typeRecord) {
  assert(typeRecord.getMembers().size() > 1);
  os << "struct " << name << " {\n";
  for (const auto &ty : llvm::enumerate(typeRecord.getMembers())) {
    os << "  ";
    ClangSyntaxPrinter(os).printKnownCType(ty.value(), typeMapping);
    os << " _" << ty.index() << ";\n";
  }
  os << "};\n";
}

static void printKnownTypedef(
    PrimitiveTypeMapping &typeMapping, raw_ostream &os, StringRef name,
    const IRABIDetailsProvider::TypeRecordABIRepresentation &typeRecord) {
  assert(typeRecord.getMembers().size() == 1);
  os << "typedef ";
  ClangSyntaxPrinter(os).printKnownCType(typeRecord.getMembers()[0],
                                         typeMapping);
  os << " " << name << ";\n";
}

static void printKnownType(
    PrimitiveTypeMapping &typeMapping, raw_ostream &os, StringRef name,
    const IRABIDetailsProvider::TypeRecordABIRepresentation &typeRecord) {
  if (typeRecord.getMembers().size() == 1)
    return printKnownTypedef(typeMapping, os, name, typeRecord);
  printKnownStruct(typeMapping, os, name, typeRecord);
}

static void
printValueWitnessTableFunctionType(raw_ostream &os, StringRef prefix,
                                   StringRef name, StringRef returnType,
                                   std::string paramTypes,
                                   uint16_t ptrauthDisc) {
  os << "using " << prefix << name << "Ty = " << returnType
     << "(* __ptrauth_swift_value_witness_function_pointer(" << ptrauthDisc
     << "))(" << paramTypes << ") SWIFT_NOEXCEPT_FUNCTION_PTR;\n";
}

static std::string makeParams(const char *arg) { return arg; }

template <class... T>
static std::string makeParams(const char *arg, const T... args) {
  return std::string(arg) + ", " + makeParams(args...);
}

static void printValueWitnessTable(raw_ostream &os) {
  std::string members;
  llvm::raw_string_ostream membersOS(members);

  // C++ only supports noexcept on `using` function types in C++17.
  os << "#if __cplusplus > 201402L\n";
  os << "#  define SWIFT_NOEXCEPT_FUNCTION_PTR noexcept\n";
  os << "#else\n";
  os << "#  define SWIFT_NOEXCEPT_FUNCTION_PTR\n";
  os << "#endif\n\n";

#define WANT_ONLY_REQUIRED_VALUE_WITNESSES
#define DATA_VALUE_WITNESS(lowerId, upperId, type)                             \
  membersOS << "  " << type << " " << #lowerId << ";\n";
#define FUNCTION_VALUE_WITNESS(lowerId, upperId, returnType, paramTypes)       \
  printValueWitnessTableFunctionType(                                          \
      os, "ValueWitness", #upperId, returnType, makeParams paramTypes,         \
      SpecialPointerAuthDiscriminators::upperId);                              \
  membersOS << "  ValueWitness" << #upperId << "Ty _Nonnull " << #lowerId      \
            << ";\n";
#define MUTABLE_VALUE_TYPE "void * _Nonnull"
#define IMMUTABLE_VALUE_TYPE "const void * _Nonnull"
#define MUTABLE_BUFFER_TYPE "void * _Nonnull"
#define IMMUTABLE_BUFFER_TYPE "const void * _Nonnull"
#define TYPE_TYPE "void * _Nonnull"
#define SIZE_TYPE "size_t"
#define INT_TYPE "int"
#define UINT_TYPE "unsigned"
#define VOID_TYPE "void"
#include "swift/ABI/ValueWitness.def"

  membersOS << "\n  constexpr size_t getAlignment() const { return (flags & "
            << TargetValueWitnessFlags<uint64_t>::AlignmentMask << ") + 1; }\n";
  os << "\nstruct ValueWitnessTable {\n" << membersOS.str() << "};\n\n";
  membersOS.str().clear();

#define WANT_ONLY_ENUM_VALUE_WITNESSES
#define DATA_VALUE_WITNESS(lowerId, upperId, type)                             \
  membersOS << "  " << type << " " << #lowerId << ";\n";
#define FUNCTION_VALUE_WITNESS(lowerId, upperId, returnType, paramTypes)       \
  printValueWitnessTableFunctionType(                                          \
      os, "EnumValueWitness", #upperId, returnType, makeParams paramTypes,     \
      SpecialPointerAuthDiscriminators::upperId);                              \
  membersOS << "  EnumValueWitness" << #upperId << "Ty _Nonnull " << #lowerId  \
            << ";\n";
#define MUTABLE_VALUE_TYPE "void * _Nonnull"
#define IMMUTABLE_VALUE_TYPE "const void * _Nonnull"
#define MUTABLE_BUFFER_TYPE "void * _Nonnull"
#define IMMUTABLE_BUFFER_TYPE "const void * _Nonnull"
#define TYPE_TYPE "void * _Nonnull"
#define SIZE_TYPE "size_t"
#define INT_TYPE "int"
#define UINT_TYPE "unsigned"
#define VOID_TYPE "void"
#include "swift/ABI/ValueWitness.def"

  os << "\nstruct EnumValueWitnessTable {\n"
     << "  ValueWitnessTable vwTable;\n"
     << membersOS.str() << "};\n\n";

  os << "#undef SWIFT_NOEXCEPT_FUNCTION_PTR\n\n";
}

static void printTypeMetadataResponseType(SwiftToClangInteropContext &ctx,
                                          PrimitiveTypeMapping &typeMapping,
                                          raw_ostream &os) {
  os << "// Swift type metadata response type.\n";
  // Print out the type metadata structure.
  auto funcSig = ctx.getIrABIDetails().getTypeMetadataAccessFunctionSignature();
  printKnownType(typeMapping, os, "MetadataResponseTy", funcSig.returnType);
  assert(funcSig.parameterTypes.size() == 1);
  os << "// Swift type metadata request type.\n";
  printKnownType(typeMapping, os, "MetadataRequestTy",
                 funcSig.parameterTypes[0]);
}

void printPrimitiveGenericTypeTraits(raw_ostream &os, ASTContext &astContext,
                                     PrimitiveTypeMapping &typeMapping,
                                     bool isCForwardDefinition) {
  Type supportedPrimitiveTypes[] = {
      astContext.getBoolType(),

      // Primitive integer, C integer and Int/UInt mappings.
      astContext.getInt8Type(), astContext.getUInt8Type(),
      astContext.getInt16Type(), astContext.getUInt16Type(),
      astContext.getInt32Type(), astContext.getUInt32Type(),
      astContext.getInt64Type(), astContext.getUInt64Type(),

      // Primitive floating point type mappings.
      astContext.getFloatType(), astContext.getDoubleType(),

      // Pointer types.
      // FIXME: support raw pointers?
      astContext.getOpaquePointerType(),

      astContext.getIntType(), astContext.getUIntType()};

  auto primTypesArray = llvm::makeArrayRef(supportedPrimitiveTypes);

  // Ensure that `long` and `unsigned long` are treated as valid
  // generic Swift types (`Int` and `UInt`) on platforms
  // that do define `Int`/`ptrdiff_t` as `long` and don't define `int64_t` to be
  // `long`.
  auto &clangTI =
      astContext.getClangModuleLoader()->getClangASTContext().getTargetInfo();
  bool isSwiftIntLong =
      clangTI.getPtrDiffType(0) == clang::TransferrableTargetInfo::SignedLong;
  bool isInt64Long =
      clangTI.getInt64Type() == clang::TransferrableTargetInfo::SignedLong;
  if (!(isSwiftIntLong && !isInt64Long))
    primTypesArray = primTypesArray.drop_back(2);

  for (Type type : primTypesArray) {
    auto typeInfo = *typeMapping.getKnownCxxTypeInfo(
        type->getNominalOrBoundGenericNominal());

    auto typeMetadataFunc = irgen::LinkEntity::forTypeMetadata(
        type->getCanonicalType(), irgen::TypeMetadataAddress::AddressPoint);
    std::string typeMetadataVarName = typeMetadataFunc.mangleAsString();

    if (isCForwardDefinition) {
      os << "// type metadata address for " << type.getString() << ".\n";
      os << "SWIFT_IMPORT_STDLIB_SYMBOL extern size_t " << typeMetadataVarName
         << ";\n";
      continue;
    }

    os << "template<>\n";
    os << "static inline const constexpr bool isUsableInGenericContext<"
       << typeInfo.name << "> = true;\n\n";

    os << "template<>\nstruct TypeMetadataTrait<" << typeInfo.name << "> {\n"
       << "  static ";
    ClangSyntaxPrinter(os).printInlineForThunk();
    os << "void * _Nonnull getTypeMetadata() {\n"
       << "    return &" << cxx_synthesis::getCxxImplNamespaceName()
       << "::" << typeMetadataVarName << ";\n"
       << "  }\n};\n\n";
  }
}

void swift::printSwiftToClangCoreScaffold(SwiftToClangInteropContext &ctx,
                                          ASTContext &astContext,
                                          PrimitiveTypeMapping &typeMapping,
                                          raw_ostream &os) {
  ClangSyntaxPrinter printer(os);
  printer.printNamespace(
      cxx_synthesis::getCxxSwiftNamespaceName(),
      [&](raw_ostream &) {
        printer.printNamespace(
            cxx_synthesis::getCxxImplNamespaceName(), [&](raw_ostream &) {
              printer.printExternC([&](raw_ostream &os) {
                printTypeMetadataResponseType(ctx, typeMapping, os);
                os << "\n";
                printValueWitnessTable(os);
                os << "\n";
                printPrimitiveGenericTypeTraits(os, astContext, typeMapping,
                                                /*isCForwardDefinition=*/true);
              });
              os << "\n";
            });
        os << "\n";
        // C++ only supports inline variables from C++17.
        ClangSyntaxPrinter(os).printIgnoredCxx17ExtensionDiagnosticBlock([&]() {
          printPrimitiveGenericTypeTraits(os, astContext, typeMapping,
                                          /*isCForwardDefinition=*/false);
        });
      },
      ClangSyntaxPrinter::NamespaceTrivia::AttributeSwiftPrivate);
}
