//===--- DeclAndTypePrinter.cpp - Emit ObjC decls from a Swift AST --------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "DeclAndTypePrinter.h"
#include "ClangSyntaxPrinter.h"
#include "PrimitiveTypeMapping.h"
#include "PrintClangClassType.h"
#include "PrintClangFunction.h"
#include "PrintClangValueType.h"
#include "SwiftToClangInteropContext.h"

#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ClangSwiftTypeCorrespondence.h"
#include "swift/AST/Comment.h"
#include "swift/AST/Decl.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/ForeignAsyncConvention.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/SwiftNameTranslation.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/TypeVisitor.h"
#include "swift/IDE/CommentConversion.h"
#include "swift/IRGen/IRABIDetailsProvider.h"
#include "swift/IRGen/Linking.h"
#include "swift/Parse/Lexer.h"
#include "swift/Parse/Parser.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclObjC.h"
#include "clang/Basic/CharInfo.h"
#include "clang/Basic/SourceManager.h"
#include "SwiftToClangInteropContext.h"

using namespace swift;
using namespace swift::objc_translation;

static bool isNSObjectOrAnyHashable(ASTContext &ctx, Type type) {
  if (auto classDecl = type->getClassOrBoundGenericClass()) {
    return classDecl->getName()
             == ctx.getSwiftId(KnownFoundationEntity::NSObject) &&
           classDecl->getModuleContext()->getName() == ctx.Id_ObjectiveC;
  }

  return type->isAnyHashable();
}

static bool isAnyObjectOrAny(Type type) {
  return type->isAnyObject() || type->isAny();
}

// For a given Decl and Type, if the type is not an optional return
// the type and OTK_None as the optionality. If the type is
// optional, return the underlying object type, and an optionality
// that is based on the type but overridden by the return value of
// isImplicitlyUnwrappedOptional().
std::pair<Type, OptionalTypeKind>
DeclAndTypePrinter::getObjectTypeAndOptionality(const ValueDecl *D, Type ty) {
  OptionalTypeKind kind;
  if (auto objTy = ty->getReferenceStorageReferent()->getOptionalObjectType()) {
    kind = OTK_Optional;
    if (D->isImplicitlyUnwrappedOptional())
      kind = OTK_ImplicitlyUnwrappedOptional;

    return {objTy, kind};
  }

  return {ty, OTK_None};
}

namespace {
  /// Whether the type being printed is in function param position.
  enum IsFunctionParam_t : bool {
    IsFunctionParam = true,
    IsNotFunctionParam = false,
  };
} // end anonymous namespace

/// Returns true if the given selector might be classified as an init method
/// by Objective-C ARC.
static bool looksLikeInitMethod(ObjCSelector selector) {
  ArrayRef<Identifier> selectorPieces = selector.getSelectorPieces();
  assert(!selectorPieces.empty());
  auto firstPiece = selectorPieces.front().str();
  if (!firstPiece.startswith("init")) return false;
  return !(firstPiece.size() > 4 && clang::isLowercase(firstPiece[4]));
}

// Enters and leaves a new lexical scope when emitting
// members of a Swift type.
struct CxxEmissionScopeRAII {
  DeclAndTypePrinter &printer;
  CxxDeclEmissionScope &prevScope;
  CxxDeclEmissionScope scope;

  CxxEmissionScopeRAII(DeclAndTypePrinter &printer)
      : printer(printer), prevScope(printer.getCxxDeclEmissionScope()) {
    printer.setCxxDeclEmissionScope(scope);
  }
  ~CxxEmissionScopeRAII() { printer.setCxxDeclEmissionScope(prevScope); }
};

class DeclAndTypePrinter::Implementation
    : private DeclVisitor<DeclAndTypePrinter::Implementation>,
      private TypeVisitor<DeclAndTypePrinter::Implementation, void,
                          Optional<OptionalTypeKind>>,
      private ClangSyntaxPrinter {
  using PrinterImpl = Implementation;
  friend ASTVisitor;
  friend TypeVisitor;

  DeclAndTypePrinter &owningPrinter;
  OutputLanguageMode outputLang;

  SmallVector<const FunctionType *, 4> openFunctionTypes;

  ASTContext &getASTContext() const {
    return owningPrinter.M.getASTContext();
  }

  // Returns an implementation that prints to the module's prologue.
  Implementation getModuleProloguePrinter() {
    return Implementation(owningPrinter.prologueOS, owningPrinter, outputLang);
  }

public:
  explicit Implementation(raw_ostream &out, DeclAndTypePrinter &owner,
                          OutputLanguageMode outputLang)
      : ClangSyntaxPrinter(out), owningPrinter(owner), outputLang(outputLang) {}

  void print(const Decl *D) {
    PrettyStackTraceDecl trace("printing", D);
    ASTVisitor::visit(const_cast<Decl *>(D));
  }

  void maybePrintObjCGenericParameters(const ClassDecl *importedClass) {
    auto *clangDecl = importedClass->getClangDecl();
    auto *objcClass = dyn_cast_or_null<clang::ObjCInterfaceDecl>(clangDecl);
    if (!objcClass)
      return;
    if (!objcClass->getTypeParamList())
      return;
    assert(objcClass->getTypeParamList()->size() != 0);
    os << "<";
    interleave(*objcClass->getTypeParamList(),
               [this](const clang::ObjCTypeParamDecl *param) {
                 os << param->getName();
               },
               [this] { os << ", "; });
    os << ">";
  }

  void printAdHocCategory(iterator_range<const ValueDecl * const *> members) {
    assert(members.begin() != members.end());

    const DeclContext *origDC = (*members.begin())->getDeclContext();
    auto *baseClass = origDC->getSelfClassDecl();

    os << "@interface " << getNameForObjC(baseClass);
    maybePrintObjCGenericParameters(baseClass);
    os << " (SWIFT_EXTENSION(" << origDC->getParentModule()->getName()
       << "))\n";
    printMembers</*allowDelayed*/true>(members);
    os << "@end\n\n";
  }

  bool shouldInclude(const ValueDecl *VD) {
    return owningPrinter.shouldInclude(VD);
  }

  bool isEmptyExtensionDecl(const ExtensionDecl *ED) {
    auto members = ED->getMembers();
    auto hasMembers = std::any_of(members.begin(), members.end(),
                                  [this](const Decl *D) -> bool {
      if (auto VD = dyn_cast<ValueDecl>(D))
        if (shouldInclude(VD))
          return true;
      return false;
    });

    auto protocols = ED->getLocalProtocols(ConformanceLookupKind::OnlyExplicit);
    auto hasProtocols = std::any_of(protocols.begin(), protocols.end(),
                                    [this](const ProtocolDecl *PD) -> bool {
      return shouldInclude(PD);
    });

    return (!hasMembers && !hasProtocols);
  }

private:
  void recordEmittedDeclInCurrentCxxLexicalScope(const ValueDecl *vd) {
    assert(outputLang == OutputLanguageMode::Cxx);
    owningPrinter.getCxxDeclEmissionScope().emittedDeclarationNames.insert(
        cxx_translation::getNameForCxx(vd));
  }

  /// Prints a protocol adoption list: <code>&lt;NSCoding, NSCopying&gt;</code>
  ///
  /// This method filters out non-ObjC protocols.
  void printProtocols(ArrayRef<ProtocolDecl *> protos) {
    SmallVector<ProtocolDecl *, 4> protosToPrint;
    std::copy_if(protos.begin(), protos.end(),
                 std::back_inserter(protosToPrint),
                 [this](const ProtocolDecl *PD) -> bool {
      return shouldInclude(PD);
    });

    // Drop protocols from the list that are implied by other protocols.
    ProtocolType::canonicalizeProtocols(protosToPrint);

    if (protosToPrint.empty())
      return;

    os << " <";
    interleave(protosToPrint,
               [this](const ProtocolDecl *PD) { os << getNameForObjC(PD); },
               [this] { os << ", "; });
    os << ">";
  }

  /// Prints the members of a class, extension, or protocol.
  template <bool AllowDelayed = false, typename R>
  void printMembers(R &&members) {
    CxxEmissionScopeRAII cxxScopeRAII(owningPrinter);
    // FIXME: Actually track emitted members in nested
    // lexical scopes.
    // FIXME: Emit unavailable C++ decls for not emitted
    // nested members.
    bool protocolMembersOptional = false;
    for (const Decl *member : members) {
      auto VD = dyn_cast<ValueDecl>(member);
      if (!VD || !shouldInclude(VD) || isa<TypeDecl>(VD))
        continue;
      if (isa<AccessorDecl>(VD))
        continue;
      if (!AllowDelayed && owningPrinter.delayedMembers.count(VD)) {
        os << "// '" << VD->getName() << "' below\n";
        continue;
      }
      if (VD->getAttrs().hasAttribute<OptionalAttr>() !=
          protocolMembersOptional) {
        protocolMembersOptional = !protocolMembersOptional;
        os << (protocolMembersOptional ? "@optional\n" : "@required\n");
      }
      ASTVisitor::visit(const_cast<ValueDecl*>(VD));
    }
  }

  void printDocumentationComment(Decl *D) {
    swift::markup::MarkupContext MC;
    auto DC = getSingleDocComment(MC, D);
    if (DC)
      ide::getDocumentationCommentAsDoxygen(DC, os);
  }

  /// Prints an encoded string, escaped properly for C.
  void printEncodedString(raw_ostream &os, StringRef str,
                          bool includeQuotes = true) {
    // NB: We don't use raw_ostream::write_escaped() because it does hex escapes
    // for non-ASCII chars.

    llvm::SmallString<128> Buf;
    StringRef decodedStr = Lexer::getEncodedStringSegment(str, Buf);

    if (includeQuotes) os << '"';
    for (unsigned char c : decodedStr) {
      switch (c) {
      case '\\':
        os << '\\' << '\\';
        break;
      case '\t':
        os << '\\' << 't';
        break;
      case '\n':
        os << '\\' << 'n';
        break;
      case '"':
        os << '\\' << '"';
        break;
      default:
        if (c < 0x20 || c == 0x7F) {
          os << '\\' << 'x';
          os << llvm::hexdigit((c >> 4) & 0xF);
          os << llvm::hexdigit((c >> 0) & 0xF);
        } else {
          os << c;
        }
      }
    }
    if (includeQuotes) os << '"';
  }

  // For a given Decl and Type, if the type is not an optional return
  // the type and OTK_None as the optionality. If the type is
  // optional, return the underlying object type, and an optionality
  // that is based on the type but overridden by the return value of
  // isImplicitlyUnwrappedOptional().
  static std::pair<Type, OptionalTypeKind>
  getObjectTypeAndOptionality(const ValueDecl *D, Type ty) {
    return DeclAndTypePrinter::getObjectTypeAndOptionality(D, ty);
  }

  // Ignore other declarations.
  void visitDecl(Decl *D) {}

  void visitClassDecl(ClassDecl *CD) {
    printDocumentationComment(CD);

    if (outputLang == OutputLanguageMode::Cxx) {
      // FIXME: Non objc class.
      // FIXME: forward decl should be handled by ModuleWriter.
      ClangValueTypePrinter::forwardDeclType(os, CD, owningPrinter);
      ClangClassTypePrinter(os).printClassTypeDecl(
          CD, [&]() { printMembers(CD->getMembers()); }, owningPrinter);
      recordEmittedDeclInCurrentCxxLexicalScope(CD);
      return;
    }

    // This is just for testing, so we check explicitly for the attribute instead
    // of asking if the class is weak imported. If the class has availability,
    // we'll print a SWIFT_AVAILABLE() which implies __attribute__((weak_imported))
    // already.
    if (CD->getAttrs().hasAttribute<WeakLinkedAttr>())
      os << "SWIFT_WEAK_IMPORT\n";

    if (CD->getAttrs().hasAttribute<IBDesignableAttr>()) {
      os << "IB_DESIGNABLE\n";
    }

    bool hasResilientAncestry =
      CD->checkAncestry().contains(AncestryFlags::ResilientOther);
    if (hasResilientAncestry) {
      os << "SWIFT_RESILIENT_CLASS";
    } else {
      os << "SWIFT_CLASS";
    }

    StringRef customName = getNameForObjC(CD, CustomNamesOnly);
    if (customName.empty()) {
      llvm::SmallString<32> scratch;
      os << "(\"" << CD->getObjCRuntimeName(scratch) << "\")";
      printAvailability(CD);
      os << "\n@interface " << CD->getName();
    } else {
      os << "_NAMED(\"" << CD->getName() << "\")";
      printAvailability(CD);
      os << "\n@interface " << customName;
    }

    if (auto superDecl = CD->getSuperclassDecl())
      os << " : " << getNameForObjC(superDecl);
    printProtocols(CD->getLocalProtocols(ConformanceLookupKind::OnlyExplicit));
    os << "\n";
    printMembers(CD->getMembers());
    os << "@end\n";
  }

  void visitStructDecl(StructDecl *SD) {
    if (outputLang != OutputLanguageMode::Cxx)
      return;
    // FIXME: Print struct's doc comment.
    ClangValueTypePrinter printer(os, owningPrinter.prologueOS,
                                  owningPrinter.interopContext);
    printer.printValueTypeDecl(
        SD, /*bodyPrinter=*/
        [&]() {
          printMembers(SD->getMembers());
          for (const auto *ed :
               owningPrinter.interopContext.getExtensionsForNominalType(SD)) {
            auto sign = ed->getGenericSignature();
            // FIXME: support requirements.
            if (!sign.getRequirements().empty())
              continue;
            printMembers(ed->getMembers());
          }
        },
        owningPrinter);
    recordEmittedDeclInCurrentCxxLexicalScope(SD);
  }

  void visitExtensionDecl(ExtensionDecl *ED) {
    if (isEmptyExtensionDecl(ED))
      return;

    auto baseClass = ED->getSelfClassDecl();

    if (printAvailability(ED, PrintLeadingSpace::No))
      os << "\n";
    os << "@interface " << getNameForObjC(baseClass);
    maybePrintObjCGenericParameters(baseClass);
    os << " (SWIFT_EXTENSION(" << ED->getModuleContext()->getName() << "))";
    printProtocols(ED->getLocalProtocols(ConformanceLookupKind::OnlyExplicit));
    os << "\n";
    printMembers(ED->getMembers());
    os << "@end\n";
  }

  void visitProtocolDecl(ProtocolDecl *PD) {
    printDocumentationComment(PD);

    StringRef customName = getNameForObjC(PD, CustomNamesOnly);
    if (customName.empty()) {
      llvm::SmallString<32> scratch;
      os << "SWIFT_PROTOCOL(\"" << PD->getObjCRuntimeName(scratch) << "\")";
      printAvailability(PD);
      os << "\n@protocol " << PD->getName();
    } else {
      os << "SWIFT_PROTOCOL_NAMED(\"" << PD->getName() << "\")";
      printAvailability(PD);
      os << "\n@protocol " << customName;
    }

    printProtocols(PD->getInheritedProtocols());
    os << "\n";
    printMembers(PD->getMembers());
    os << "@end\n";
  }

  void visitEnumDeclCxx(EnumDecl *ED) {
    assert(owningPrinter.outputLang == OutputLanguageMode::Cxx);

    ClangValueTypePrinter valueTypePrinter(os, owningPrinter.prologueOS,
                                           owningPrinter.interopContext);
    ClangSyntaxPrinter syntaxPrinter(os);
    DeclAndTypeClangFunctionPrinter clangFuncPrinter(
        os, owningPrinter.prologueOS, owningPrinter.typeMapping,
        owningPrinter.interopContext, owningPrinter);

    auto &outOfLineOS = owningPrinter.outOfLineDefinitionsOS;
    ClangSyntaxPrinter outOfLineSyntaxPrinter(outOfLineOS);
    DeclAndTypeClangFunctionPrinter outOfLineFuncPrinter(
        owningPrinter.outOfLineDefinitionsOS, owningPrinter.prologueOS,
        owningPrinter.typeMapping, owningPrinter.interopContext, owningPrinter);
    ClangValueTypePrinter outOfLineValTyPrinter(
        owningPrinter.outOfLineDefinitionsOS, owningPrinter.prologueOS,
        owningPrinter.interopContext);

    auto elementTagMapping =
        owningPrinter.interopContext.getIrABIDetails().getEnumTagMapping(ED);
    // Sort cases based on their assigned tag indices
    llvm::stable_sort(elementTagMapping, [](const auto &p1, const auto &p2) {
      return p1.second.tag < p2.second.tag;
    });

    auto printIsFunction = [&](StringRef caseName, EnumDecl *ED) {
      std::string declName, defName, name;
      llvm::raw_string_ostream declOS(declName), defOS(defName), nameOS(name);
      ClangSyntaxPrinter(nameOS).printIdentifier(caseName);
      name[0] = std::toupper(name[0]);

      os << "  ";
      ClangSyntaxPrinter(os).printInlineForThunk();
      os << "bool is" << name << "() const;\n";

      outOfLineSyntaxPrinter
          .printNominalTypeOutsideMemberDeclTemplateSpecifiers(ED);
      outOfLineOS << "  ";
      ClangSyntaxPrinter(outOfLineOS).printInlineForThunk();
      outOfLineOS << " bool ";
      outOfLineSyntaxPrinter.printNominalTypeQualifier(
          ED, /*moduleContext=*/ED->getModuleContext());
      outOfLineOS << "is" << name << "() const {\n";
      outOfLineOS << "    return *this == ";
      outOfLineSyntaxPrinter.printNominalTypeQualifier(
          ED, /*moduleContext=*/ED->getModuleContext());
      outOfLineSyntaxPrinter.printIdentifier(caseName);
      outOfLineOS << ";\n";
      outOfLineOS << "  }\n";
    };

    auto printGetFunction = [&](EnumElementDecl *elementDecl) {
      auto associatedValueList = elementDecl->getParameterList();
      // TODO: add tuple type support
      if (!elementDecl->hasAssociatedValues() ||
          associatedValueList->size() > 1) {
        return;
      }
      auto paramType = associatedValueList->front()->getInterfaceType();

      std::string declName, defName, name;
      llvm::raw_string_ostream declOS(declName), defOS(defName), nameOS(name);
      ClangSyntaxPrinter(nameOS).printIdentifier(elementDecl->getNameStr());
      name[0] = std::toupper(name[0]);

      clangFuncPrinter.printCustomCxxFunction(
          {paramType},
          /*NeedsReturnTypes=*/true,
          [&](auto &types) {
            // Printing function name and return type
            os << "  ";
            ClangSyntaxPrinter(os).printInlineForThunk();
            os << types[paramType] << " get" << name;
            outOfLineSyntaxPrinter
                .printNominalTypeOutsideMemberDeclTemplateSpecifiers(ED);
            outOfLineOS << "  ";
            ClangSyntaxPrinter(outOfLineOS).printInlineForThunk();
            outOfLineOS << types[paramType] << ' ';
            outOfLineSyntaxPrinter.printNominalTypeQualifier(
                ED, /*moduleContext=*/ED->getModuleContext());
            outOfLineOS << "get" << name;
          },
          [&](auto &types) {}, true,
          [&](auto &types) {
            // Printing function body
            outOfLineOS << "    if (!is" << name << "()) abort();\n";
            outOfLineOS << "    alignas(";
            outOfLineSyntaxPrinter.printBaseName(elementDecl->getParentEnum());
            outOfLineOS << ") unsigned char buffer[sizeof(";
            outOfLineSyntaxPrinter.printBaseName(elementDecl->getParentEnum());
            outOfLineOS << ")];\n";
            outOfLineOS << "    auto *thisCopy = new(buffer) ";
            outOfLineSyntaxPrinter.printBaseName(elementDecl->getParentEnum());
            outOfLineOS << "(*this);\n";
            outOfLineOS << "    char * _Nonnull payloadFromDestruction = "
                           "thisCopy->_destructiveProjectEnumData();\n";
            if (const auto *gtpt = paramType->getAs<GenericTypeParamType>()) {
              DeclAndTypeClangFunctionPrinter::printGenericReturnSequence(
                  outOfLineOS, gtpt, [](StringRef) {},
                  /*initializeWithTake=*/StringRef("payloadFromDestruction"));
              return;
            }
            // FIXME: unify non-generic return with regular function emission
            // return path.
            Type objectType;
            OptionalTypeKind optKind;
            std::tie(objectType, optKind) = getObjectTypeAndOptionality(
                paramType->getNominalOrBoundGenericNominal(), paramType);
            auto objectTypeDecl = objectType->getNominalOrBoundGenericNominal();

            if (auto knownCxxType =
                    owningPrinter.typeMapping.getKnownCxxTypeInfo(
                        objectTypeDecl)) {
              outOfLineOS << "    " << types[paramType] << " result;\n";
              outOfLineOS << "    "
                             "memcpy(&result, payloadFromDestruction, "
                             "sizeof(result));\n";
              outOfLineOS << "    return result;\n  ";
            } else {
              outOfLineOS << "    return swift::";
              outOfLineOS << cxx_synthesis::getCxxImplNamespaceName();
              outOfLineOS << "::implClassFor<";
              outOfLineSyntaxPrinter.printModuleNamespaceQualifiersIfNeeded(
                  objectTypeDecl->getModuleContext(),
                  elementDecl->getParentEnum()->getModuleContext());
              outOfLineSyntaxPrinter.printBaseName(objectTypeDecl);
              outOfLineOS << ">::type";
              if (isa<ClassDecl>(objectTypeDecl)) {
                outOfLineOS << "::makeRetained(*reinterpret_cast<void "
                               "**>(payloadFromDestruction));\n  ";
              } else {
                outOfLineOS << "::returnNewValue([&](char * _Nonnull result) "
                               "SWIFT_INLINE_THUNK_ATTRIBUTES {\n";
                outOfLineOS << "      swift::"
                            << cxx_synthesis::getCxxImplNamespaceName();
                outOfLineOS << "::implClassFor<";
                outOfLineSyntaxPrinter.printModuleNamespaceQualifiersIfNeeded(
                    objectTypeDecl->getModuleContext(),
                    elementDecl->getParentEnum()->getModuleContext());
                outOfLineSyntaxPrinter.printBaseName(objectTypeDecl);
                outOfLineOS << ">::type";
                outOfLineOS << "::initializeWithTake(result, "
                               "payloadFromDestruction);\n";
                outOfLineOS << "    });\n  ";
              }
            }
          },
          ED, ED->getModuleContext(), outOfLineOS);
    };

    auto printStruct = [&](StringRef caseName, EnumElementDecl *elementDecl,
                           Optional<IRABIDetailsProvider::EnumElementInfo>
                               elementInfo) {
      os << "  inline const static struct _impl_" << caseName << " {  "
         << "// impl struct for case " << caseName << '\n';
      os << "    ";
      syntaxPrinter.printInlineForThunk();
      os << "constexpr operator cases() const {\n";
      os << "      return cases::";
      syntaxPrinter.printIdentifier(caseName);
      os << ";\n";
      os << "    }\n";

      if (elementDecl != nullptr) {
        assert(elementInfo.has_value());

        Type paramType;

        // TODO: support tuple type
        if (elementDecl->hasAssociatedValues() &&
            elementDecl->getParameterList()->size() == 1) {
          paramType =
              elementDecl->getParameterList()->front()->getInterfaceType();
        }

        SmallVector<Type> neededTypes;
        if (paramType) {
          neededTypes.push_back(paramType);
        }

        clangFuncPrinter.printCustomCxxFunction(
            neededTypes,
            /*NeedsReturnTypes=*/false,
            [&](auto &types) {
              const auto *ED = elementDecl->getParentEnum();
              // Printing function name and return type
              os << "    SWIFT_INLINE_THUNK "; // TODO
              syntaxPrinter.printNominalTypeReference(ED,
                                                      ED->getModuleContext());
              os << " operator()";

              outOfLineSyntaxPrinter
                  .printNominalTypeOutsideMemberDeclTemplateSpecifiers(ED);
              outOfLineOS << "  ";
              outOfLineSyntaxPrinter.printInlineForThunk();
              outOfLineSyntaxPrinter.printNominalTypeReference(
                  ED, ED->getModuleContext());
              outOfLineOS << ' ';
              outOfLineSyntaxPrinter.printNominalTypeQualifier(
                  ED, /*moduleContext=*/ED->getModuleContext());
              outOfLineOS << "_impl_" << caseName << "::operator()";
            },
            [&](auto &types) {
              // Printing parameters
              if (!paramType) {
                return;
              }
              os << types[paramType] << " val";
              outOfLineOS << types[paramType] << " val";
            },
            true,
            [&](auto &types) {
              auto *ED = elementDecl->getParentEnum();
              // Printing function body
              outOfLineOS << "    auto result = ";
              outOfLineSyntaxPrinter.printNominalTypeQualifier(
                  ED, ED->getModuleContext());
              outOfLineOS << "_make();\n";
              if (paramType) {
                if (paramType->getAs<GenericTypeParamType>()) {
                  auto type = types[paramType];
                  ClangSyntaxPrinter(outOfLineOS)
                      .printIgnoredCxx17ExtensionDiagnosticBlock([&]() {
                        // FIXME: handle C++ types.
                        outOfLineOS << "if constexpr (std::is_base_of<::swift::"
                                    << cxx_synthesis::getCxxImplNamespaceName()
                                    << "::RefCountedClass, " << type
                                    << ">::value) {\n";
                        outOfLineOS << "    void *ptr = ::swift::"
                                    << cxx_synthesis::getCxxImplNamespaceName()
                                    << "::_impl_RefCountedClass::"
                                       "copyOpaquePointer(val);\n";
                        outOfLineOS
                            << "    memcpy(result._getOpaquePointer(), &ptr, "
                               "sizeof(ptr));\n";
                        outOfLineOS << "} else if constexpr (::swift::"
                                    << cxx_synthesis::getCxxImplNamespaceName()
                                    << "::isValueType<" << type << ">) {\n";

                        outOfLineOS << "    alignas(" << type;
                        outOfLineOS << ") unsigned char buffer[sizeof(" << type;
                        outOfLineOS << ")];\n";
                        outOfLineOS << "    auto *valCopy = new(buffer) "
                                    << type;
                        outOfLineOS << "(val);\n";
                        outOfLineOS << "    ";
                        outOfLineOS << cxx_synthesis::getCxxSwiftNamespaceName()
                                    << "::";
                        outOfLineOS << cxx_synthesis::getCxxImplNamespaceName();
                        outOfLineOS << "::implClassFor<" << type;
                        outOfLineOS << ">::type::initializeWithTake(result._"
                                       "getOpaquePointer(), ";
                        outOfLineOS << cxx_synthesis::getCxxSwiftNamespaceName()
                                    << "::";
                        outOfLineOS << cxx_synthesis::getCxxImplNamespaceName();
                        outOfLineOS << "::implClassFor<" << type;
                        outOfLineOS << ">::type::getOpaquePointer(*valCopy)";
                        outOfLineOS << ");\n";
                        outOfLineOS << "} else {\n";
                        outOfLineOS
                            << "    memcpy(result._getOpaquePointer(), &val, "
                               "sizeof(val));\n";
                        outOfLineOS << "}\n";
                      });
                } else {

                  OptionalTypeKind optKind;
                  Type objectType;
                  std::tie(objectType, optKind) =
                      DeclAndTypePrinter::getObjectTypeAndOptionality(
                          ED, paramType);
                  auto objectTypeDecl =
                      objectType->getNominalOrBoundGenericNominal();
                  assert(objectTypeDecl != nullptr);

                  if (owningPrinter.typeMapping.getKnownCxxTypeInfo(
                          objectTypeDecl)) {
                    outOfLineOS
                        << "    memcpy(result._getOpaquePointer(), &val, "
                           "sizeof(val));\n";
                  } else if (isa<ClassDecl>(objectTypeDecl)) {
                    outOfLineOS
                        << "    auto op = swift::"
                        << cxx_synthesis::getCxxImplNamespaceName()
                        << "::_impl_RefCountedClass::copyOpaquePointer(val);\n";
                    outOfLineOS << "    memcpy(result._getOpaquePointer(), "
                                   "&op, sizeof(op));\n";
                  } else {
                    objectTypeDecl =
                        paramType->getNominalOrBoundGenericNominal();
                    outOfLineOS << "    alignas(";
                    outOfLineSyntaxPrinter
                        .printModuleNamespaceQualifiersIfNeeded(
                            objectTypeDecl->getModuleContext(),
                            ED->getModuleContext());
                    outOfLineSyntaxPrinter.printBaseName(objectTypeDecl);
                    outOfLineOS << ") unsigned char buffer[sizeof(";
                    outOfLineSyntaxPrinter
                        .printModuleNamespaceQualifiersIfNeeded(
                            objectTypeDecl->getModuleContext(),
                            ED->getModuleContext());
                    outOfLineSyntaxPrinter.printBaseName(objectTypeDecl);
                    outOfLineOS << ")];\n";
                    outOfLineOS << "    auto *valCopy = new(buffer) ";
                    outOfLineSyntaxPrinter
                        .printModuleNamespaceQualifiersIfNeeded(
                            objectTypeDecl->getModuleContext(),
                            ED->getModuleContext());
                    outOfLineSyntaxPrinter.printBaseName(objectTypeDecl);
                    outOfLineOS << "(val);\n";
                    outOfLineOS << "    ";
                    outOfLineOS << cxx_synthesis::getCxxSwiftNamespaceName()
                                << "::";
                    outOfLineOS << cxx_synthesis::getCxxImplNamespaceName();
                    outOfLineOS << "::implClassFor<";
                    outOfLineSyntaxPrinter
                        .printModuleNamespaceQualifiersIfNeeded(
                            objectTypeDecl->getModuleContext(),
                            ED->getModuleContext());
                    outOfLineSyntaxPrinter.printBaseName(objectTypeDecl);
                    outOfLineOS << ">::type::initializeWithTake(result._"
                                   "getOpaquePointer(), ";
                    outOfLineOS << cxx_synthesis::getCxxSwiftNamespaceName()
                                << "::";
                    outOfLineOS << cxx_synthesis::getCxxImplNamespaceName();
                    outOfLineOS << "::implClassFor<";
                    outOfLineSyntaxPrinter
                        .printModuleNamespaceQualifiersIfNeeded(
                            objectTypeDecl->getModuleContext(),
                            ED->getModuleContext());
                    outOfLineSyntaxPrinter.printBaseName(objectTypeDecl);
                    outOfLineOS << ">::type::getOpaquePointer(*valCopy)";
                    outOfLineOS << ");\n";
                  }
                }
              }

              outOfLineOS << "    result._destructiveInjectEnumTag(";
              if (ED->isResilient()) {
                outOfLineOS << cxx_synthesis::getCxxImplNamespaceName()
                            << "::" << elementInfo->globalVariableName;
              } else {
                outOfLineOS << elementInfo->tag;
              }
              outOfLineOS << ");\n";
              outOfLineOS << "    return result;\n";
              outOfLineOS << "  ";
            },
            ED, ED->getModuleContext(), outOfLineOS);
      }
      os << "  } ";
      syntaxPrinter.printIdentifier(caseName);
      if (elementDecl)
        syntaxPrinter.printSymbolUSRAttribute(elementDecl);
      os << ";\n";
    };

    valueTypePrinter.printValueTypeDecl(
        ED, /*bodyPrinter=*/
        [&]() {
          os << '\n';
          os << "  enum class cases {";
          llvm::interleave(
              elementTagMapping, os,
              [&](const auto &pair) {
                os << "\n    ";
                syntaxPrinter.printIdentifier(pair.first->getNameStr());
                syntaxPrinter.printSymbolUSRAttribute(pair.first);
              },
              ",");
          // TODO: allow custom name for this special case
          auto resilientUnknownDefaultCaseName = "unknownDefault";
          if (ED->isResilient()) {
            os << (ED->getNumElements() > 0 ? ",\n    " : "\n    ")
               << resilientUnknownDefaultCaseName;
          }
          os << "\n  };\n\n"; // enum class cases' closing bracket

          os << "#pragma clang diagnostic push\n";
          os << "#pragma clang diagnostic ignored \"-Wc++17-extensions\"  "
             << "// allow use of inline static data member\n";
          for (const auto &pair : elementTagMapping) {
            // Printing struct
            printStruct(pair.first->getNameStr(), pair.first, pair.second);
            // Printing `is` function
            printIsFunction(pair.first->getNameStr(), ED);
            if (pair.first->hasAssociatedValues()) {
              // Printing `get` function
              printGetFunction(pair.first);
            }
            os << '\n';
          }

          if (ED->isResilient()) {
            // Printing struct for unknownDefault
            printStruct(resilientUnknownDefaultCaseName,
                        /* elementDecl */ nullptr,
                        /* elementInfo */ None);
            // Printing isUnknownDefault
            printIsFunction(resilientUnknownDefaultCaseName, ED);
            os << '\n';
          }
          os << "#pragma clang diagnostic pop\n";

          // Printing operator cases()
          os << "  ";
          ClangSyntaxPrinter(os).printInlineForThunk();
          os << "operator cases() const {\n";
          if (ED->isResilient()) {
            if (!elementTagMapping.empty()) {
              os << "    auto tag = _getEnumTag();\n";
            }
            for (const auto &pair : elementTagMapping) {
              os << "    if (tag == "
                 << cxx_synthesis::getCxxImplNamespaceName();
              os << "::" << pair.second.globalVariableName
                 << ") return cases::";
              syntaxPrinter.printIdentifier(pair.first->getNameStr());
              os << ";\n";
            }
            os << "    return cases::" << resilientUnknownDefaultCaseName
               << ";\n";
          } else { // non-resilient enum
            os << "    switch (_getEnumTag()) {\n";
            for (const auto &pair : elementTagMapping) {
              os << "      case " << pair.second.tag << ": return cases::";
              syntaxPrinter.printIdentifier(pair.first->getNameStr());
              os << ";\n";
            }
            // TODO: change to Swift's fatalError when it's available in C++
            os << "      default: abort();\n";
            os << "    }\n"; // switch's closing bracket
          }
          os << "  }\n"; // operator cases()'s closing bracket
          os << "\n";

          printMembers(ED->getMembers());
        },
        owningPrinter);
    recordEmittedDeclInCurrentCxxLexicalScope(ED);
  }

  void visitEnumDecl(EnumDecl *ED) {
    printDocumentationComment(ED);

    if (outputLang == OutputLanguageMode::Cxx) {
      visitEnumDeclCxx(ED);
      return;
    }

    os << "typedef ";
    StringRef customName = getNameForObjC(ED, CustomNamesOnly);
    if (customName.empty()) {
      os << "SWIFT_ENUM(";
    } else {
      os << "SWIFT_ENUM_NAMED(";
    }
    print(ED->getRawType(), OTK_None);
    if (customName.empty()) {
      os << ", " << ED->getName();
    } else {
      os << ", " << customName
         << ", \"" << ED->getName() << "\"";
    }
    os << ", "
       << (ED->isFormallyExhaustive(/*useDC*/nullptr) ? "closed" : "open")
       << ") {\n";

    for (auto Elt : ED->getAllElements()) {
      printDocumentationComment(Elt);

      // Print the cases as the concatenation of the enum name with the case
      // name.
      os << "  ";
      if (printSwiftEnumElemNameInObjC(Elt, os)) {
        os << " SWIFT_COMPILE_NAME(\"" << Elt->getBaseIdentifier() << "\")";
      }

      // Print the raw values, even the ones that we synthesize.
      auto *ILE = cast<IntegerLiteralExpr>(Elt->getStructuralRawValueExpr());
      os << " = ";
      if (ILE->isNegative())
        os << "-";
      os << ILE->getDigitsText();
      os << ",\n";
    }
    os << "};\n";
  }

  void printSingleMethodParam(StringRef selectorPiece,
                              const ParamDecl *param,
                              const clang::ParmVarDecl *clangParam,
                              bool forceNSUInteger) {
    os << selectorPiece << ":(";
    if (forceNSUInteger ||
        (clangParam && isNSUInteger(clangParam->getType()))) {
      os << "NSUInteger";
    } else {
      OptionalTypeKind kind;
      Type objTy;
      std::tie(objTy, kind) =
          getObjectTypeAndOptionality(param, param->getInterfaceType());
      print(objTy, kind, "", IsFunctionParam);
    }
    os << ")";

    if (!param->hasName()) {
      os << "_";
    } else {
      Identifier name = param->getName();
      os << name;
      if (isClangKeyword(name))
        os << "_";
    }
  }

  template <typename T>
  static const T *findClangBase(const T *member) {
    while (member) {
      if (member->getClangDecl())
        return member;
      member = member->getOverriddenDecl();
    }
    return nullptr;
  }

  /// Returns true if \p clangTy is the typedef for NSUInteger.
  bool isNSUInteger(clang::QualType clangTy) {
    if (const auto* elaboratedTy = dyn_cast<clang::ElaboratedType>(clangTy)) {
      clangTy = elaboratedTy->desugar();
    }
    const auto *typedefTy = dyn_cast<clang::TypedefType>(clangTy);
    if (!typedefTy)
      return false;

    const clang::IdentifierInfo *nameII = typedefTy->getDecl()->getIdentifier();
    if (!nameII)
      return false;
    if (nameII->getName() != "NSUInteger")
      return false;

    return true;
  }

  Type getForeignResultType(AbstractFunctionDecl *AFD,
                            AnyFunctionType *methodTy,
                            Optional<ForeignAsyncConvention> asyncConvention,
                            Optional<ForeignErrorConvention> errorConvention) {
    // A foreign error convention can affect the result type as seen in
    // Objective-C.
    if (errorConvention) {
      switch (errorConvention->getKind()) {
      case ForeignErrorConvention::ZeroResult:
      case ForeignErrorConvention::NonZeroResult:
        // The error convention provides the result type.
        return errorConvention->getResultType();

      case ForeignErrorConvention::NilResult:
        // Errors are propagated via 'nil' returns.
        return OptionalType::get(methodTy->getResult());

      case ForeignErrorConvention::NonNilError:
      case ForeignErrorConvention::ZeroPreservedResult:
        break;
      }
    }

    // Asynchronous methods return their results via completion handler.
    if (asyncConvention) {
      return getASTContext().TheEmptyTupleType;
    }

    auto result = methodTy->getResult();
    if (result->isUninhabited())
      return getASTContext().TheEmptyTupleType;
    return result;
  }

  /// Returns true if \p sel is the no-argument selector 'init'.
  static bool selectorIsInit(ObjCSelector sel) {
    return sel.getNumArgs() == 0 &&
           sel.getSelectorPieces().front().str() == "init";
  }

  /// Returns true if the given function overload is safe to emit in the current
  /// C++ lexical scope.
  bool canPrintOverloadOfFunction(const AbstractFunctionDecl *funcDecl) const {
    assert(outputLang == OutputLanguageMode::Cxx);
    auto &overloads =
        owningPrinter.getCxxDeclEmissionScope().emittedFunctionOverloads;
    auto cxxName = cxx_translation::getNameForCxx(funcDecl);
    auto overloadIt = overloads.find(cxxName);
    if (overloadIt == overloads.end()) {
      overloads.insert(std::make_pair(
          cxxName,
          llvm::SmallVector<const AbstractFunctionDecl *>({funcDecl})));
      return true;
    }
    auto selfArity =
        funcDecl->getParameters() ? funcDecl->getParameters()->size() : 0;
    for (const auto *overload : overloadIt->second) {
      auto arity =
          overload->getParameters() ? overload->getParameters()->size() : 0;
      // Avoid printing out an overload with the same and arity, as that might
      // be an ambiguous overload on the C++ side.
      // FIXME: we should take types into account, not all overloads with the
      // same arity are ambiguous in C++.
      if (selfArity == arity) {
        owningPrinter.getCxxDeclEmissionScope()
            .additionalUnrepresentableDeclarations.push_back(funcDecl);
        return false;
      }
    }
    overloadIt->second.push_back(funcDecl);
    return true;
  }

  void printAbstractFunctionAsMethod(AbstractFunctionDecl *AFD,
                                     bool isClassMethod,
                                     bool isNSUIntegerSubscript = false,
                                     const SubscriptDecl *SD = nullptr) {
    Optional<ForeignAsyncConvention> asyncConvention =
        AFD->getForeignAsyncConvention();
    Optional<ForeignErrorConvention> errorConvention =
        AFD->getForeignErrorConvention();
    Type rawMethodTy = AFD->getMethodInterfaceType();
    auto methodTy = rawMethodTy->castTo<FunctionType>();
    auto resultTy =
        getForeignResultType(AFD, methodTy, asyncConvention, errorConvention);

    if (outputLang == OutputLanguageMode::Cxx) {
      // FIXME: Support operators.
      if (AFD->isOperator() || (AFD->isStatic() && AFD->isImplicit()))
        return;

      auto *typeDeclContext = dyn_cast<NominalTypeDecl>(AFD->getParent());
      if (!typeDeclContext) {
        typeDeclContext =
            cast<ExtensionDecl>(AFD->getParent())->getExtendedNominal();
      }

      std::string cFuncDecl;
      llvm::raw_string_ostream cFuncPrologueOS(cFuncDecl);
      auto funcABI = Implementation(cFuncPrologueOS, owningPrinter, outputLang)
                         .printSwiftABIFunctionSignatureAsCxxFunction(
                             AFD, methodTy,
                             /*selfTypeDeclContext=*/typeDeclContext);
      if (!funcABI)
        return;
      Optional<IRABIDetailsProvider::MethodDispatchInfo> dispatchInfo;
      if (!isa<ConstructorDecl>(AFD)) {
        dispatchInfo = owningPrinter.interopContext.getIrABIDetails()
                           .getMethodDispatchInfo(AFD);
        if (!dispatchInfo)
          return;
      }
      // FIXME: handle getters/setters ambiguities here too.
      if (!isa<AccessorDecl>(AFD)) {
        if (!canPrintOverloadOfFunction(AFD))
          return;
      }

      owningPrinter.prologueOS << cFuncPrologueOS.str();

      printDocumentationComment(AFD);
      DeclAndTypeClangFunctionPrinter declPrinter(
          os, owningPrinter.prologueOS, owningPrinter.typeMapping,
          owningPrinter.interopContext, owningPrinter);
      if (auto *accessor = dyn_cast<AccessorDecl>(AFD)) {
        if (SD)
          declPrinter.printCxxSubscriptAccessorMethod(
              owningPrinter, typeDeclContext, accessor, funcABI->getSignature(),
              funcABI->getSymbolName(), resultTy,
              /*isDefinition=*/false, dispatchInfo);
        else
          declPrinter.printCxxPropertyAccessorMethod(
              owningPrinter, typeDeclContext, accessor, funcABI->getSignature(),
              funcABI->getSymbolName(), resultTy,
              /*isStatic=*/isClassMethod,
              /*isDefinition=*/false, dispatchInfo);
      } else {
        declPrinter.printCxxMethod(owningPrinter, typeDeclContext, AFD,
                                   funcABI->getSignature(),
                                   funcABI->getSymbolName(), resultTy,
                                   /*isStatic=*/isClassMethod,
                                   /*isDefinition=*/false, dispatchInfo);
      }

      DeclAndTypeClangFunctionPrinter defPrinter(
          owningPrinter.outOfLineDefinitionsOS, owningPrinter.prologueOS,
          owningPrinter.typeMapping, owningPrinter.interopContext,
          owningPrinter);

      if (auto *accessor = dyn_cast<AccessorDecl>(AFD)) {
        if (SD)
          defPrinter.printCxxSubscriptAccessorMethod(
              owningPrinter, typeDeclContext, accessor, funcABI->getSignature(),
              funcABI->getSymbolName(), resultTy, /*isDefinition=*/true,
              dispatchInfo);
        else
          defPrinter.printCxxPropertyAccessorMethod(
              owningPrinter, typeDeclContext, accessor, funcABI->getSignature(),
              funcABI->getSymbolName(), resultTy,
              /*isStatic=*/isClassMethod,
              /*isDefinition=*/true, dispatchInfo);
      } else {
        defPrinter.printCxxMethod(owningPrinter, typeDeclContext, AFD,
                                  funcABI->getSignature(),
                                  funcABI->getSymbolName(), resultTy,
                                  /*isStatic=*/isClassMethod,
                                  /*isDefinition=*/true, dispatchInfo);
      }

      // FIXME: SWIFT_WARN_UNUSED_RESULT
      return;
    }
    printDocumentationComment(AFD);

    if (isClassMethod)
      os << "+ (";
    else
      os << "- (";

    const clang::ObjCMethodDecl *clangMethod = nullptr;
    if (!isNSUIntegerSubscript) {
      if (const AbstractFunctionDecl *clangBase = findClangBase(AFD)) {
        clangMethod =
            dyn_cast_or_null<clang::ObjCMethodDecl>(clangBase->getClangDecl());
      }
    }

    // Constructors and methods returning DynamicSelf return
    // instancetype.
    if (isa<ConstructorDecl>(AFD) ||
        (isa<FuncDecl>(AFD) && cast<FuncDecl>(AFD)->hasDynamicSelfResult() &&
         !AFD->hasAsync())) {
      if (errorConvention && errorConvention->stripsResultOptionality()) {
        printNullability(OTK_Optional, NullabilityPrintKind::ContextSensitive);
      } else if (auto ctor = dyn_cast<ConstructorDecl>(AFD)) {
        OptionalTypeKind kind = OTK_None;
        if (ctor->isFailable()) {
          if (ctor->isImplicitlyUnwrappedOptional())
            kind = OTK_ImplicitlyUnwrappedOptional;
          else
            kind = OTK_Optional;
        }
        printNullability(kind,
                         NullabilityPrintKind::ContextSensitive);
      } else {
        auto func = cast<FuncDecl>(AFD);
        OptionalTypeKind kind;
        Type objTy;
        std::tie(objTy, kind) =
          getObjectTypeAndOptionality(func, func->getResultInterfaceType());

        printNullability(kind,
                         NullabilityPrintKind::ContextSensitive);
      }

      os << "instancetype";
    } else if (resultTy->isVoid() &&
               AFD->getAttrs().hasAttribute<IBActionAttr>()) {
      os << "IBAction";
    } else if (clangMethod && isNSUInteger(clangMethod->getReturnType())) {
      os << "NSUInteger";
    } else {
      // IBSegueAction is placed before whatever return value is chosen.
      if (AFD->getAttrs().hasAttribute<IBSegueActionAttr>()) {
        os << "IBSegueAction ";
      }

      OptionalTypeKind kind;
      Type objTy;
      std::tie(objTy, kind) = getObjectTypeAndOptionality(AFD, resultTy);
      print(objTy, kind);
    }

    os << ")";

    auto selector = AFD->getObjCSelector();
    ArrayRef<Identifier> selectorPieces = selector.getSelectorPieces();

    const auto &params = AFD->getParameters()->getArray();
    unsigned paramIndex = 0;
    for (unsigned i = 0, n = selectorPieces.size(); i != n; ++i) {
      if (i > 0) os << ' ';

      // Retrieve the selector piece.
      StringRef piece = selectorPieces[i].empty() ? StringRef("")
                                                  : selectorPieces[i].str();

      // If we have an async convention and this is the completion handler
      // parameter, print it.
      if (asyncConvention &&
          i == asyncConvention->completionHandlerParamIndex()) {
        os << piece << ":(";
        print(asyncConvention->completionHandlerType(), OTK_None);
        os << ")completionHandler";
        continue;
      }

      // If we have an error convention and this is the error
      // parameter, print it.
      if (errorConvention && i == errorConvention->getErrorParameterIndex()) {
        os << piece << ":(";
        print(errorConvention->getErrorParameterType(), None);
        os << ")error";
        continue;
      }

      // Zero-parameter initializers with a long selector.
      if (isa<ConstructorDecl>(AFD) &&
          cast<ConstructorDecl>(AFD)->isObjCZeroParameterWithLongSelector()) {
        os << piece;
        continue;
      }

      // Zero-parameter methods.
      if (params.empty()) {
        assert(paramIndex == 0);
        os << piece;
        paramIndex = 1;
        continue;
      }

      const clang::ParmVarDecl *clangParam = nullptr;
      if (clangMethod)
        clangParam = clangMethod->parameters()[paramIndex];

      // Single-parameter methods.
      bool forceNSUInteger = isNSUIntegerSubscript && (i == n-1);
      printSingleMethodParam(piece, params[paramIndex], clangParam,
                             forceNSUInteger);
      ++paramIndex;
    }

    bool skipAvailability = false;
    bool makeNewUnavailable = false;
    bool makeNewExplicitlyAvailable = false;
    // Swift designated initializers are Objective-C designated initializers.
    if (auto ctor = dyn_cast<ConstructorDecl>(AFD)) {
      if (ctor->hasStubImplementation()
          || ctor->getFormalAccess() < owningPrinter.minRequiredAccess) {
        // This will only be reached if the overridden initializer has the
        // required access
        os << " SWIFT_UNAVAILABLE";
        skipAvailability = true;
        // If -init is unavailable, then +new should be, too:
        makeNewUnavailable = selectorIsInit(selector);
      } else {
        if (ctor->isDesignatedInit() &&
            !isa<ProtocolDecl>(ctor->getDeclContext())) {
          os << " OBJC_DESIGNATED_INITIALIZER";
        }

        // If -init is newly available, +new should be as well if the class
        // inherits from NSObject.
        if (selectorIsInit(selector) && !ctor->getOverriddenDecl()) {
          auto container = ctor->getDeclContext();
          auto *classDecl = container->getSelfClassDecl();
          if (!classDecl) {
            assert(container->getSelfProtocolDecl());
          } else {
            while (classDecl->hasSuperclass()) {
              classDecl = classDecl->getSuperclassDecl();
              assert(classDecl &&
                     "shouldn't PrintAsObjC with invalid superclasses");
            }
            if (classDecl->hasClangNode() &&
                classDecl->getNameStr() == "NSObject") {
              makeNewExplicitlyAvailable = true;
            }
          }
        }
      }

      if (!looksLikeInitMethod(AFD->getObjCSelector())) {
        os << " SWIFT_METHOD_FAMILY(init)";
      }
    } else {
      if (looksLikeInitMethod(AFD->getObjCSelector())) {
        os << " SWIFT_METHOD_FAMILY(none)";
      }
      if (asyncConvention) {
        // Async methods don't have result types to annotate.
      } else if (methodTy->getResult()->isUninhabited()) {
        os << " SWIFT_NORETURN";
      } else if (!methodTy->getResult()->isVoid() &&
                 !AFD->getAttrs().hasAttribute<DiscardableResultAttr>()) {
        os << " SWIFT_WARN_UNUSED_RESULT";
      }
    }

    if (!skipAvailability) {
      printAvailability(AFD);
    }

    if (auto accessor = dyn_cast<AccessorDecl>(AFD)) {
      printSwift3ObjCDeprecatedInference(accessor->getStorage());
    } else {
      printSwift3ObjCDeprecatedInference(AFD);
    }

    os << ";\n";

    if (makeNewUnavailable) {
      assert(!makeNewExplicitlyAvailable);
      // Downgrade this to a warning in pre-Swift-5 mode. This isn't perfect
      // because it's a diagnostic inflicted on /clients/, but it's close
      // enough. It really is invalid to call +new when -init is unavailable.
      StringRef annotationName = "SWIFT_UNAVAILABLE_MSG";
      if (!getASTContext().isSwiftVersionAtLeast(5))
        annotationName = "SWIFT_DEPRECATED_MSG";
      os << "+ (nonnull instancetype)new " << annotationName
         << "(\"-init is unavailable\");\n";
    } else if (makeNewExplicitlyAvailable) {
      os << "+ (nonnull instancetype)new;\n";
    }
  }

  /// Print the core function declaration for a given function with the given
  /// name.
  void printFunctionDeclAsCFunctionDecl(FuncDecl *FD, StringRef name,
                                        Type resultTy) {
    // The result type may be a partial function type we need to close
    // up later.
    PrintMultiPartType multiPart(*this);

    OptionalTypeKind kind;
    Type objTy;
    std::tie(objTy, kind) = getObjectTypeAndOptionality(FD, resultTy);
    visitPart(objTy, kind);

    os << ' ' << name << '(';

    auto params = FD->getParameters();
    if (params->size()) {
      size_t index = 1;
      interleaveComma(*params, os, [&](const ParamDecl *param) {
        OptionalTypeKind kind;
        Type objTy;
        std::tie(objTy, kind) =
            getObjectTypeAndOptionality(param, param->getInterfaceType());
        StringRef paramName =
            param->getName().empty() ? "" : param->getName().str();
        print(objTy, kind, paramName, IsFunctionParam);
        ++index;
      });
    } else {
      os << "void";
    }

    os << ')';

    // Finish the result type.
    multiPart.finish();
  }

  /// Print C or C++ trailing attributes for a function declaration.
  void printFunctionClangAttributes(FuncDecl *FD, AnyFunctionType *funcTy) {
    if (funcTy->getResult()->isUninhabited()) {
      os << " SWIFT_NORETURN";
    } else if (!funcTy->getResult()->isVoid() &&
               !FD->getAttrs().hasAttribute<DiscardableResultAttr>()) {
      os << " SWIFT_WARN_UNUSED_RESULT";
    }
  }

  // Print out the function signature for a @_cdecl function.
  void printAbstractFunctionAsCFunction(FuncDecl *FD) {
    printDocumentationComment(FD);
    Optional<ForeignAsyncConvention> asyncConvention
      = FD->getForeignAsyncConvention();
    Optional<ForeignErrorConvention> errorConvention
      = FD->getForeignErrorConvention();
    assert(!FD->getGenericSignature() &&
           "top-level generic functions not supported here");
    auto funcTy = FD->getInterfaceType()->castTo<FunctionType>();
    auto resultTy = getForeignResultType(
        FD, funcTy, asyncConvention, errorConvention);

    assert(FD->getAttrs().hasAttribute<CDeclAttr>() && "not a cdecl function");
    os << "SWIFT_EXTERN ";
    printFunctionDeclAsCFunctionDecl(
        FD, FD->getAttrs().getAttribute<CDeclAttr>()->Name, resultTy);
    printFunctionClangAttributes(FD, funcTy);
    printAvailability(FD);
    os << ";\n";
  }

  struct FunctionSwiftABIInformation {
    FunctionSwiftABIInformation(AbstractFunctionDecl *FD,
                                LoweredFunctionSignature signature)
        : signature(signature) {
      isCDecl = FD->getAttrs().hasAttribute<CDeclAttr>();
      if (!isCDecl) {
        auto mangledName = SILDeclRef(FD).mangle();
        symbolName = FD->getASTContext().AllocateCopy(mangledName);
      } else {
        symbolName = FD->getAttrs().getAttribute<CDeclAttr>()->Name;
      }
    }

    StringRef getSymbolName() const { return symbolName; }

    bool useCCallingConvention() const { return isCDecl; }

    bool useMangledSymbolName() const { return !isCDecl; }

    const LoweredFunctionSignature &getSignature() const { return signature; }

  private:
    bool isCDecl;
    StringRef symbolName;
    LoweredFunctionSignature signature;
  };

  /// Print the C function declaration that represents the given native Swift
  /// function, or its dispatch thunk.
  ClangRepresentation printCFunctionWithLoweredSignature(
      AbstractFunctionDecl *FD, const FunctionSwiftABIInformation &funcABI,
      Type resultTy, AnyFunctionType *funcTy, StringRef symbolName,
      StringRef comment = "") {
    std::string cRepresentationString;
    llvm::raw_string_ostream cRepresentationOS(cRepresentationString);
    cRepresentationOS << "SWIFT_EXTERN ";

    DeclAndTypeClangFunctionPrinter funcPrinter(
        cRepresentationOS, owningPrinter.prologueOS, owningPrinter.typeMapping,
        owningPrinter.interopContext, owningPrinter);

    auto representation = funcPrinter.printFunctionSignature(
        FD, funcABI.getSignature(), symbolName, resultTy,
        DeclAndTypeClangFunctionPrinter::FunctionSignatureKind::CFunctionProto);
    if (representation.isUnsupported())
      return representation;

    os << cRepresentationOS.str();
    // Swift functions can't throw exceptions, we can only
    // throw them from C++ when emitting C++ inline thunks for the Swift
    // functions.
    if (!funcTy->isThrowing())
      os << " SWIFT_NOEXCEPT";
    if (!funcABI.useCCallingConvention())
      os << " SWIFT_CALL";
    printAvailability(FD);
    os << ';';
    if (funcABI.useMangledSymbolName()) {
      // add a comment with a demangled function name.
      os << " // ";
      if (!comment.empty())
        os << comment << ' ';
      FD->getName().print(os);
    }
    os << "\n";
    return representation;
  }

  // Print out the extern C Swift ABI function signature.
  Optional<FunctionSwiftABIInformation>
  printSwiftABIFunctionSignatureAsCxxFunction(
      AbstractFunctionDecl *FD, Optional<FunctionType *> givenFuncType = None,
      Optional<NominalTypeDecl *> selfTypeDeclContext = None) {
    assert(outputLang == OutputLanguageMode::Cxx);
    Optional<ForeignAsyncConvention> asyncConvention =
        FD->getForeignAsyncConvention();
    Optional<ForeignErrorConvention> errorConvention =
        FD->getForeignErrorConvention();
    // FIXME (Alex): Make type adjustments for C++.
    AnyFunctionType *funcTy;
    if (givenFuncType || FD->getInterfaceType()->is<FunctionType>()) {
      funcTy = givenFuncType ? *givenFuncType
                             : FD->getInterfaceType()->castTo<FunctionType>();
    } else {
      funcTy = FD->getInterfaceType()->castTo<GenericFunctionType>();
    }

    auto resultTy =
        getForeignResultType(FD, funcTy, asyncConvention, errorConvention);

    auto signature = owningPrinter.interopContext.getIrABIDetails()
                         .getFunctionLoweredSignature(FD);
    // FIXME: Add a note saying that this func is unsupported.
    if (!signature)
      return None;
    FunctionSwiftABIInformation funcABI(FD, *signature);

    auto representation = printCFunctionWithLoweredSignature(
        FD, funcABI, resultTy, funcTy, funcABI.getSymbolName());
    if (representation.isUnsupported())
      // FIXME: Emit remark about unemitted declaration.
      return None;

    if (selfTypeDeclContext && !isa<ConstructorDecl>(FD)) {
      if (auto dispatchInfo = owningPrinter.interopContext.getIrABIDetails()
                                  .getMethodDispatchInfo(FD)) {
        // Emit the C signature for the dispatch thunk.
        if (dispatchInfo->getKind() ==
            IRABIDetailsProvider::MethodDispatchInfo::Kind::Thunk) {
          auto thunkRepresentation = printCFunctionWithLoweredSignature(
              FD, funcABI, resultTy, funcTy, dispatchInfo->getThunkSymbolName(),
              "dispatch thunk for");
          assert(!thunkRepresentation.isUnsupported());
        } else if (dispatchInfo->getKind() ==
                   IRABIDetailsProvider::MethodDispatchInfo::Kind::
                       IndirectVTableRelativeOffset) {
          // Emit the C signature for the class metadata base offset.
          owningPrinter.interopContext.runIfStubForDeclNotEmitted(
              dispatchInfo->getBaseOffsetSymbolName(), [&] {
                auto baseClassOffsetType =
                    owningPrinter.interopContext.getIrABIDetails()
                        .getClassBaseOffsetSymbolType();
                os << "SWIFT_EXTERN ";
                ClangSyntaxPrinter(os).printKnownCType(
                    baseClassOffsetType, owningPrinter.typeMapping);
                os << ' ' << dispatchInfo->getBaseOffsetSymbolName()
                   << "; // class metadata base offset\n";
              });
        }
      }
    }

    return funcABI;
  }

  // Print out the C++ inline function thunk that
  // represents the Swift function in C++.
  void printAbstractFunctionAsCxxFunctionThunk(
      FuncDecl *FD, const FunctionSwiftABIInformation &funcABI) {
    assert(outputLang == OutputLanguageMode::Cxx);
    printDocumentationComment(FD);

    Optional<ForeignAsyncConvention> asyncConvention =
        FD->getForeignAsyncConvention();
    Optional<ForeignErrorConvention> errorConvention =
        FD->getForeignErrorConvention();
    auto funcTy = FD->getInterfaceType()->castTo<AnyFunctionType>();
    auto resultTy =
        getForeignResultType(FD, funcTy, asyncConvention, errorConvention);

    DeclAndTypeClangFunctionPrinter funcPrinter(
        os, owningPrinter.prologueOS, owningPrinter.typeMapping,
        owningPrinter.interopContext, owningPrinter);
    DeclAndTypeClangFunctionPrinter::FunctionSignatureModifiers modifiers;
    modifiers.isInline = true;
    // FIXME: Support throwing exceptions for Swift errors.
    modifiers.isNoexcept = !funcTy->isThrowing();
    auto result = funcPrinter.printFunctionSignature(
        FD, funcABI.getSignature(), cxx_translation::getNameForCxx(FD),
        resultTy,
        DeclAndTypeClangFunctionPrinter::FunctionSignatureKind::CxxInlineThunk,
        modifiers);
    assert(
        !result.isUnsupported()); // The C signature should be unsupported too.
    printFunctionClangAttributes(FD, funcTy);
    printAvailability(FD);
    os << " {\n";
    funcPrinter.printCxxThunkBody(
        FD, funcABI.getSignature(), funcABI.getSymbolName(),
        /*typeDeclContext=*/nullptr, FD->getModuleContext(), resultTy,
        FD->getParameters(), funcTy->isThrowing(), funcTy);
    os << "}\n";
  }

  enum class PrintLeadingSpace : bool {
    No = false,
    Yes = true
  };

  bool printAvailability(const Decl *D, PrintLeadingSpace printLeadingSpace =
                                            PrintLeadingSpace::Yes) {
    return printAvailability(os, D, printLeadingSpace);
  }

public:
  /// Returns \c true if anything was printed.
  bool printAvailability(
      raw_ostream &os, const Decl *D,
      PrintLeadingSpace printLeadingSpace = PrintLeadingSpace::Yes) {
    bool hasPrintedAnything = false;
    auto maybePrintLeadingSpace = [&] {
      if (printLeadingSpace == PrintLeadingSpace::Yes || hasPrintedAnything)
        os << " ";
      hasPrintedAnything = true;
    };

    for (auto AvAttr : D->getAttrs().getAttributes<AvailableAttr>()) {
      if (AvAttr->Platform == PlatformKind::none) {
        if (AvAttr->PlatformAgnostic ==
            PlatformAgnosticAvailabilityKind::Unavailable) {
          // Availability for *
          if (!AvAttr->Rename.empty() && isa<ValueDecl>(D)) {
            // rename
            maybePrintLeadingSpace();
            os << "SWIFT_UNAVAILABLE_MSG(\"'"
               << cast<ValueDecl>(D)->getBaseName()
               << "' has been renamed to '";
            printRenameForDecl(os, AvAttr, cast<ValueDecl>(D), false);
            os << '\'';
            if (!AvAttr->Message.empty()) {
              os << ": ";
              printEncodedString(os, AvAttr->Message, false);
            }
            os << "\")";
          } else if (!AvAttr->Message.empty()) {
            maybePrintLeadingSpace();
            os << "SWIFT_UNAVAILABLE_MSG(";
            printEncodedString(os, AvAttr->Message);
            os << ")";
          } else {
            maybePrintLeadingSpace();
            os << "SWIFT_UNAVAILABLE";
          }
          break;
        }
        if (AvAttr->isUnconditionallyDeprecated()) {
          if (!AvAttr->Rename.empty() || !AvAttr->Message.empty()) {
            maybePrintLeadingSpace();
            os << "SWIFT_DEPRECATED_MSG(";
            printEncodedString(os, AvAttr->Message);
            if (!AvAttr->Rename.empty()) {
              os << ", ";
              printRenameForDecl(os, AvAttr, cast<ValueDecl>(D), true);
            }
            os << ")";
          } else {
            maybePrintLeadingSpace();
            os << "SWIFT_DEPRECATED";
          }
        }
        continue;
      }

      // Availability for a specific platform
      if (!AvAttr->Introduced.has_value() && !AvAttr->Deprecated.has_value() &&
          !AvAttr->Obsoleted.has_value() &&
          !AvAttr->isUnconditionallyDeprecated() &&
          !AvAttr->isUnconditionallyUnavailable()) {
        continue;
      }

      const char *plat;
      switch (AvAttr->Platform) {
      case PlatformKind::macOS:
        plat = "macos";
        break;
      case PlatformKind::iOS:
        plat = "ios";
        break;
      case PlatformKind::macCatalyst:
        plat = "maccatalyst";
        break;
      case PlatformKind::tvOS:
        plat = "tvos";
        break;
      case PlatformKind::watchOS:
        plat = "watchos";
        break;
      case PlatformKind::macOSApplicationExtension:
        plat = "macos_app_extension";
        break;
      case PlatformKind::iOSApplicationExtension:
        plat = "ios_app_extension";
        break;
      case PlatformKind::macCatalystApplicationExtension:
        plat = "maccatalyst_app_extension";
        break;
      case PlatformKind::tvOSApplicationExtension:
        plat = "tvos_app_extension";
        break;
      case PlatformKind::watchOSApplicationExtension:
        plat = "watchos_app_extension";
        break;
      case PlatformKind::OpenBSD:
        plat = "openbsd";
        break;
      case PlatformKind::Windows:
        plat = "windows";
        break;
      case PlatformKind::none:
        llvm_unreachable("handled above");
      }

      maybePrintLeadingSpace();
      os << "SWIFT_AVAILABILITY(" << plat;
      if (AvAttr->isUnconditionallyUnavailable()) {
        os << ",unavailable";
      } else {
        if (AvAttr->Introduced.has_value()) {
          os << ",introduced=" << AvAttr->Introduced.value().getAsString();
        }
        if (AvAttr->Deprecated.has_value()) {
          os << ",deprecated=" << AvAttr->Deprecated.value().getAsString();
        } else if (AvAttr->isUnconditionallyDeprecated()) {
          // We need to specify some version, we can't just say deprecated.
          // We also can't deprecate it before it's introduced.
          if (AvAttr->Introduced.has_value()) {
            os << ",deprecated=" << AvAttr->Introduced.value().getAsString();
          } else {
            os << ",deprecated=0.0.1";
          }
        }
        if (AvAttr->Obsoleted.has_value()) {
          os << ",obsoleted=" << AvAttr->Obsoleted.value().getAsString();
        }
      }
      if (!AvAttr->Rename.empty() && isa<ValueDecl>(D)) {
        os << ",message=\"'" << cast<ValueDecl>(D)->getBaseName()
           << "' has been renamed to '";
        printRenameForDecl(os, AvAttr, cast<ValueDecl>(D), false);
        os << '\'';
        if (!AvAttr->Message.empty()) {
          os << ": ";
          printEncodedString(os, AvAttr->Message, false);
        }
        os << "\"";
      } else if (!AvAttr->Message.empty()) {
        os << ",message=";
        printEncodedString(os, AvAttr->Message);
      }
      os << ")";
    }
    return hasPrintedAnything;
  }

private:
  void printRenameForDecl(raw_ostream &os, const AvailableAttr *AvAttr,
                          const ValueDecl *D, bool includeQuotes) {
    assert(!AvAttr->Rename.empty());

    auto *renamedDecl = evaluateOrDefault(
        getASTContext().evaluator, RenamedDeclRequest{D, AvAttr}, nullptr);
    if (renamedDecl) {
      assert(shouldInclude(renamedDecl) &&
             "ObjC printer logic mismatch with renamed decl");
      SmallString<128> scratch;
      auto renamedObjCRuntimeName =
          renamedDecl->getObjCRuntimeName()->getString(scratch);
      printEncodedString(os, renamedObjCRuntimeName, includeQuotes);
    } else {
      printEncodedString(os, AvAttr->Rename, includeQuotes);
    }
  }

  void printSwift3ObjCDeprecatedInference(ValueDecl *VD) {
    const LangOptions &langOpts = getASTContext().LangOpts;
    if (!langOpts.EnableSwift3ObjCInference ||
        langOpts.WarnSwift3ObjCInference == Swift3ObjCInferenceWarnings::None) {
      return;
    }
    auto attr = VD->getAttrs().getAttribute<ObjCAttr>();
    if (!attr || !attr->isSwift3Inferred())
      return;

    os << " SWIFT_DEPRECATED_OBJC(\"Swift ";
    if (isa<VarDecl>(VD))
      os << "property";
    else if (isa<SubscriptDecl>(VD))
      os << "subscript";
    else if (isa<ConstructorDecl>(VD))
      os << "initializer";
    else
      os << "method";
    os << " '";
    auto nominal = VD->getDeclContext()->getSelfNominalTypeDecl();
    printEncodedString(os, nominal->getName().str(), /*includeQuotes=*/false);
    os << ".";
    SmallString<32> scratch;
    printEncodedString(os, VD->getName().getString(scratch),
                       /*includeQuotes=*/false);
    os << "' uses '@objc' inference deprecated in Swift 4; add '@objc' to "
       <<   "provide an Objective-C entrypoint\")";
  }

  void visitFuncDecl(FuncDecl *FD) {
    if (outputLang == OutputLanguageMode::Cxx) {
      if (FD->getDeclContext()->isTypeContext())
        return printAbstractFunctionAsMethod(FD, FD->isStatic());

      // Emit the underlying C signature that matches the Swift ABI
      // in the generated C++ implementation prologue for the module.
      std::string cFuncDecl;
      llvm::raw_string_ostream cFuncPrologueOS(cFuncDecl);
      auto funcABI = Implementation(cFuncPrologueOS, owningPrinter, outputLang)
                         .printSwiftABIFunctionSignatureAsCxxFunction(FD);
      if (!funcABI) {
        owningPrinter.getCxxDeclEmissionScope()
            .additionalUnrepresentableDeclarations.push_back(FD);
        return;
      }
      if (!canPrintOverloadOfFunction(FD))
        return;
      owningPrinter.prologueOS << cFuncPrologueOS.str();
      printAbstractFunctionAsCxxFunctionThunk(FD, *funcABI);
      recordEmittedDeclInCurrentCxxLexicalScope(FD);
      return;
    }
    if (FD->getDeclContext()->isTypeContext())
      printAbstractFunctionAsMethod(FD, FD->isStatic());
    else
      printAbstractFunctionAsCFunction(FD);
  }

  void visitConstructorDecl(ConstructorDecl *CD) {
    printAbstractFunctionAsMethod(CD, false);
  }

  bool maybePrintIBOutletCollection(Type ty) {
    if (auto unwrapped = ty->getOptionalObjectType())
      ty = unwrapped;

    auto genericTy = ty->getAs<BoundGenericStructType>();
    if (!genericTy || !genericTy->isArray())
      return false;

    assert(genericTy->getGenericArgs().size() == 1);

    auto argTy = genericTy->getGenericArgs().front();
    if (auto classDecl = argTy->getClassOrBoundGenericClass())
      os << "IBOutletCollection(" << getNameForObjC(classDecl) << ") ";
    else
      os << "IBOutletCollection(id) ";
    return true;
  }

  /// Returns whether \p ty is the C type \c CFTypeRef, or some typealias
  /// thereof.
  bool isCFTypeRef(Type ty) {
    if (auto existential = dyn_cast<ExistentialType>(ty.getPointer()))
      ty = existential->getConstraintType();

    const TypeAliasDecl *TAD = nullptr;
    while (auto aliasTy = dyn_cast<TypeAliasType>(ty.getPointer())) {
      TAD = aliasTy->getDecl();
      ty = aliasTy->getSinglyDesugaredType();
    }

    if (!TAD || !TAD->hasClangNode())
      return false;

    if (owningPrinter.ID_CFTypeRef.empty())
      owningPrinter.ID_CFTypeRef = getASTContext().getIdentifier("CFTypeRef");
    return TAD->getName() == owningPrinter.ID_CFTypeRef;
  }

  /// Returns true if \p ty can be used with Objective-C reference-counting
  /// annotations like \c strong and \c weak.
  bool isObjCReferenceCountableObjectType(Type ty) {
    if (auto classDecl = ty->getClassOrBoundGenericClass()) {
      if (classDecl->isForeignReferenceType())
        return false;

      switch (classDecl->getForeignClassKind()) {
      case ClassDecl::ForeignKind::Normal:
      case ClassDecl::ForeignKind::RuntimeOnly:
        return true;
      case ClassDecl::ForeignKind::CFType:
        return false;
      }
    }

    if (ty->isObjCExistentialType() && !isCFTypeRef(ty))
      return true;

    return false;
  }

  void visitVarDecl(VarDecl *VD) {
    assert(VD->getDeclContext()->isTypeContext() &&
           "cannot handle global variables right now");

    if (outputLang == OutputLanguageMode::Cxx) {
      // FIXME: Documentation.
      auto *getter = VD->getOpaqueAccessor(AccessorKind::Get);
      printAbstractFunctionAsMethod(getter, /*isStatic=*/VD->isStatic());
      if (auto *setter = VD->getOpaqueAccessor(AccessorKind::Set))
        printAbstractFunctionAsMethod(setter, /*isStatic=*/VD->isStatic());
      return;
    }

    printDocumentationComment(VD);

    if (VD->isStatic()) {
      // Older Clangs don't support class properties.
      os << "SWIFT_CLASS_PROPERTY(";
    }

    // For now, never promise atomicity.
    os << "@property (nonatomic";

    if (VD->isStatic())
      os << ", class";

    ASTContext &ctx = getASTContext();
    bool isSettable = VD->isSettable(nullptr);
    if (isSettable && !ctx.isAccessControlDisabled()) {
      isSettable =
          (VD->getSetterFormalAccess() >= owningPrinter.minRequiredAccess);
    }
    if (!isSettable)
      os << ", readonly";

    // Print the ownership semantics, if relevant.
    Type ty = VD->getInterfaceType();
    if (auto referenceStorageTy = ty->getAs<ReferenceStorageType>()) {
      switch (referenceStorageTy->getOwnership()) {
      case ReferenceOwnership::Strong:
        llvm_unreachable("not represented with a ReferenceStorageType");
      case ReferenceOwnership::Weak: {
        auto innerTy =
            referenceStorageTy->getReferentType()->getOptionalObjectType();
        if (isObjCReferenceCountableObjectType(innerTy))
          os << ", weak";
        break;
      }
      case ReferenceOwnership::Unowned:
      case ReferenceOwnership::Unmanaged:
        // We treat "unowned" as "unsafe_unretained" (even though it's more
        // like "safe_unretained") because we want people to think twice about
        // allowing that object to disappear. "unowned(unsafe)" (and
        // Swift.Unmanaged, handled below) really are "unsafe_unretained".
        os << ", unsafe_unretained";
        break;
      }
    } else {
      Type copyTy = ty;
      bool isOptional = false;
      if (auto unwrappedTy = copyTy->getOptionalObjectType()) {
        isOptional = true;
        copyTy = unwrappedTy;
      }

      auto nominal = copyTy->getNominalOrBoundGenericNominal();
      if (nominal && isa<StructDecl>(nominal)) {
        if (copyTy->isArray() ||
            copyTy->isDictionary() ||
            copyTy->isSet() ||
            copyTy->isString() ||
            getObjCBridgedClass(nominal)) {
          // We fast-path the most common cases in the condition above.
          os << ", copy";
        } else if (copyTy->isUnmanaged()) {
          os << ", unsafe_unretained";
          // Don't print unsafe_unretained twice.
          if (auto boundTy = copyTy->getAs<BoundGenericType>()) {
            ty = boundTy->getGenericArgs().front();
            if (isOptional)
              ty = OptionalType::get(ty);
          }
        }
      } else if (auto fnTy = copyTy->getAs<FunctionType>()) {
        switch (fnTy->getRepresentation()) {
        case FunctionTypeRepresentation::Block:
        case FunctionTypeRepresentation::Swift:
          os << ", copy";
          break;
        case FunctionTypeRepresentation::Thin:
        case FunctionTypeRepresentation::CFunctionPointer:
          break;
        }
      } else if (isObjCReferenceCountableObjectType(copyTy)) {
        os << ", strong";
      }
    }

    Identifier objCName = VD->getObjCPropertyName();
    bool hasReservedName = isClangKeyword(objCName);

    // Handle custom accessor names.
    llvm::SmallString<64> buffer;
    if (hasReservedName ||
        VD->getObjCGetterSelector() !=
          VarDecl::getDefaultObjCGetterSelector(ctx, objCName)) {
      os << ", getter=" << VD->getObjCGetterSelector().getString(buffer);
    }
    if (isSettable) {
      if (hasReservedName ||
          VD->getObjCSetterSelector() !=
            VarDecl::getDefaultObjCSetterSelector(ctx, objCName)) {
        buffer.clear();
        os << ", setter=" << VD->getObjCSetterSelector().getString(buffer);
      }
    }

    os << ") ";
    if (VD->getAttrs().hasAttribute<IBInspectableAttr>()) {
      os << "IBInspectable ";
    }

    if (VD->getAttrs().hasAttribute<IBOutletAttr>()) {
      if (!maybePrintIBOutletCollection(ty))
        os << "IBOutlet ";
    }

    clang::QualType clangTy;
    if (const VarDecl *base = findClangBase(VD))
      if (auto prop = dyn_cast<clang::ObjCPropertyDecl>(base->getClangDecl()))
        clangTy = prop->getType();

    if (!clangTy.isNull() && isNSUInteger(clangTy)) {
      os << "NSUInteger " << objCName;
      if (hasReservedName)
        os << "_";
    } else {
      OptionalTypeKind kind;
      Type objTy;
      std::tie(objTy, kind) = getObjectTypeAndOptionality(VD, ty);
      print(objTy, kind, objCName.str().str());
    }

    printSwift3ObjCDeprecatedInference(VD);

    printAvailability(VD);

    auto *getter = VD->getOpaqueAccessor(AccessorKind::Get);
    auto *setter = VD->getOpaqueAccessor(AccessorKind::Set);

    os << ";";
    if (VD->isStatic()) {
      os << ")\n";
      // Older Clangs don't support class properties, so print the accessors as
      // well. This is harmless.
      printAbstractFunctionAsMethod(getter, true);
      if (isSettable) {
        assert(setter && "settable ObjC property missing setter decl");
        printAbstractFunctionAsMethod(setter, true);
      }
    } else {
      os << "\n";
      if (looksLikeInitMethod(VD->getObjCGetterSelector()))
        printAbstractFunctionAsMethod(getter, false);
      if (isSettable && looksLikeInitMethod(VD->getObjCSetterSelector()))
        printAbstractFunctionAsMethod(setter, false);
    }
  }

  void visitSubscriptDecl(SubscriptDecl *SD) {
    if (outputLang == OutputLanguageMode::Cxx) {
      if (!SD->isInstanceMember())
        return;
      auto *getter = SD->getOpaqueAccessor(AccessorKind::Get);
      printAbstractFunctionAsMethod(getter, false,
                                    /*isNSUIntegerSubscript=*/false, SD);
      return;
    }
    assert(SD->isInstanceMember() && "static subscripts not supported");

    bool isNSUIntegerSubscript = false;
    if (auto clangBase = findClangBase(SD)) {
      const auto *clangGetter =
          cast<clang::ObjCMethodDecl>(clangBase->getClangDecl());
      const auto *indexParam = clangGetter->parameters().front();
      isNSUIntegerSubscript = isNSUInteger(indexParam->getType());
    }

    auto *getter = SD->getOpaqueAccessor(AccessorKind::Get);
    printAbstractFunctionAsMethod(getter, false,
                                  isNSUIntegerSubscript);
    if (auto *setter = SD->getOpaqueAccessor(AccessorKind::Set))
      printAbstractFunctionAsMethod(setter, false, isNSUIntegerSubscript);
  }

  /// Visit part of a type, such as the base of a pointer type.
  ///
  /// If a full type is being printed, use print() instead.
  void visitPart(Type ty, Optional<OptionalTypeKind> optionalKind) {
    TypeVisitor::visit(ty, optionalKind);
  }

  /// Determine whether this generic Swift nominal type maps to a
  /// generic Objective-C class.
  static bool hasGenericObjCType(const NominalTypeDecl *nominal) {
    auto clangDecl = nominal->getClangDecl();
    if (!clangDecl) return false;

    auto objcClass = dyn_cast<clang::ObjCInterfaceDecl>(clangDecl);
    if (!objcClass) return false;

    if (objcClass->getTypeParamList() == nullptr) return false;

    return true;
  }

public:
  /// If \p nominal is bridged to an Objective-C class (via a conformance to
  /// _ObjectiveCBridgeable) and is not an imported Clang type or a known type,
  /// return that class.
  ///
  /// Otherwise returns null.
  const ClassDecl *getObjCBridgedClass(const NominalTypeDecl *nominal) {
    // Print known types as their unbridged type.
    if (owningPrinter.typeMapping.getKnownObjCTypeInfo(nominal))
      return nullptr;

    // Print imported bridgeable decls as their unbridged type.
    if (nominal->hasClangNode())
      return nullptr;

    if (isa<ProtocolDecl>(nominal))
      return nullptr;

    auto &ctx = nominal->getASTContext();

    // Dig out the ObjectiveCBridgeable protocol.
    auto proto = ctx.getProtocol(KnownProtocolKind::ObjectiveCBridgeable);
    if (!proto) return nullptr;

    // Determine whether this nominal type is _ObjectiveCBridgeable.
    SmallVector<ProtocolConformance *, 2> conformances;
    if (!nominal->lookupConformance(proto, conformances))
      return nullptr;

    // Dig out the Objective-C type.
    auto conformance = conformances.front();
    Type objcType = ProtocolConformanceRef(conformance).getTypeWitnessByName(
                                           nominal->getDeclaredType(),
                                           ctx.Id_ObjectiveCType);

    // Dig out the Objective-C class.
    return objcType->getClassOrBoundGenericClass();
  }

private:
  /// If the nominal type is bridged to Objective-C (via a conformance
  /// to _ObjectiveCBridgeable), print the bridged type.
  void printObjCBridgeableType(const NominalTypeDecl *swiftNominal,
                               const ClassDecl *objcClass,
                               ArrayRef<Type> typeArgs,
                               Optional<OptionalTypeKind> optionalKind) {
    auto &ctx = swiftNominal->getASTContext();
    assert(objcClass);

    Type rewrittenArgsBuf[2];

    // Detect when the type arguments correspond to the unspecialized
    // type, and clear them out. There is some type-specific hackery
    // here for:
    //
    //   NSArray<id> --> NSArray
    //   NSDictionary<NSObject *, id> --> NSDictionary
    //   NSSet<id> --> NSSet
    if (!typeArgs.empty() &&
        (!hasGenericObjCType(objcClass)
         || (swiftNominal == ctx.getArrayDecl() &&
             isAnyObjectOrAny(typeArgs[0]))
         || (swiftNominal == ctx.getDictionaryDecl() &&
             isNSObjectOrAnyHashable(ctx, typeArgs[0]) &&
             isAnyObjectOrAny(typeArgs[1]))
         || (swiftNominal == ctx.getSetDecl() &&
             isNSObjectOrAnyHashable(ctx, typeArgs[0])))) {
      typeArgs = {};
    }

    // Use the proper upper id<NSCopying> bound for Dictionaries with
    // upper-bounded keys.
    else if (swiftNominal == ctx.getDictionaryDecl() &&
             isNSObjectOrAnyHashable(ctx, typeArgs[0])) {
      if (auto protoTy = ctx.getNSCopyingType()) {
        rewrittenArgsBuf[0] = protoTy;
        rewrittenArgsBuf[1] = typeArgs[1];
        typeArgs = rewrittenArgsBuf;
      }
    }

    // Print the class type.
    SmallString<32> objcNameScratch;
    os << objcClass->getObjCRuntimeName(objcNameScratch);

    // Print the type arguments, if present.
    if (!typeArgs.empty()) {
      os << "<";
        interleave(typeArgs,
                   [this](Type type) {
                     printCollectionElement(type);
                   },
                   [this] { os << ", "; });
      os << ">";
    }

    os << " *";
    printNullability(optionalKind);
  }

  /// If the nominal type is bridged to Objective-C (via a conformance to
  /// _ObjectiveCBridgeable), print the bridged type. Otherwise, nothing is
  /// printed.
  ///
  /// \returns true iff printed something.
  bool printIfObjCBridgeable(const NominalTypeDecl *nominal,
                             ArrayRef<Type> typeArgs,
                             Optional<OptionalTypeKind> optionalKind) {
    if (const ClassDecl *objcClass = getObjCBridgedClass(nominal)) {
      printObjCBridgeableType(nominal, objcClass, typeArgs, optionalKind);
      return true;
    }
    return false;
  }

  /// If \p typeDecl is one of the standard library types used to map in Clang
  /// primitives and basic types, print out the appropriate spelling and
  /// return true.
  ///
  /// This handles typealiases and structs provided by the standard library
  /// for interfacing with C and Objective-C.
  bool printIfKnownSimpleType(const TypeDecl *typeDecl,
                              Optional<OptionalTypeKind> optionalKind) {
    auto knownTypeInfo =
        owningPrinter.typeMapping.getKnownObjCTypeInfo(typeDecl);
    if (!knownTypeInfo)
      return false;
    os << knownTypeInfo->name;
    if (knownTypeInfo->canBeNullable)
      printNullability(optionalKind);
    return true;
  }

  void visitType(TypeBase *Ty, Optional<OptionalTypeKind> optionalKind) {
    assert(Ty->getDesugaredType() == Ty && "unhandled sugared type");
    os << "/* ";
    Ty->print(os);
    os << " */";
  }

  void visitErrorType(ErrorType *Ty, Optional<OptionalTypeKind> optionalKind) {
    os << "/* error */id";
  }

  bool isClangPointerType(const clang::TypeDecl *clangTypeDecl) const {
    ASTContext &ctx = getASTContext();
    auto &clangASTContext = ctx.getClangModuleLoader()->getClangASTContext();
    clang::QualType clangTy = clangASTContext.getTypeDeclType(clangTypeDecl);
    return swift::canImportAsOptional(clangTy.getTypePtr());
  }

  bool printImportedAlias(const TypeAliasDecl *alias,
                          ArrayRef<Type> genericArgs,
                          Optional<OptionalTypeKind> optionalKind) {
    if (!alias->hasClangNode())
      return false;

    if (auto *clangTypeDecl =
          dyn_cast<clang::TypeDecl>(alias->getClangDecl())) {
      assert(!alias->isGeneric()
             && "generic typealias backed by clang typedecl?");

      maybePrintTagKeyword(alias);
      os << getNameForObjC(alias);

      if (isClangPointerType(clangTypeDecl))
        printNullability(optionalKind);
    } else if (auto *clangObjCClass
               = dyn_cast<clang::ObjCInterfaceDecl>(alias->getClangDecl())){
      assert(!alias->isGeneric()
             && "generic typealias backed by clang interface?");

      os << clangObjCClass->getName() << " *";
      printNullability(optionalKind);
    } else {
      auto *clangCompatAlias =
      cast<clang::ObjCCompatibleAliasDecl>(alias->getClangDecl());

      os << clangCompatAlias->getName();
      if (!genericArgs.empty())
        printGenericArgs(genericArgs);
      os << " *";
      printNullability(optionalKind);
    }

    return true;
  }

  void visitTypeAliasType(TypeAliasType *aliasTy,
                               Optional<OptionalTypeKind> optionalKind) {
    const TypeAliasDecl *alias = aliasTy->getDecl();
    auto genericArgs = aliasTy->getDirectGenericArgs();

    if (printIfKnownSimpleType(alias, optionalKind))
      return;

    if (printImportedAlias(alias, genericArgs, optionalKind))
      return;

    visitPart(aliasTy->getSinglyDesugaredType(), optionalKind);
  }

  void maybePrintTagKeyword(const TypeDecl *NTD) {
    if (isa<EnumDecl>(NTD) && !NTD->hasClangNode()) {
      os << "enum ";
      return;
    }

    auto clangDecl = dyn_cast_or_null<clang::TagDecl>(NTD->getClangDecl());
    if (!clangDecl)
      return;

    if (clangDecl->getTypedefNameForAnonDecl())
      return;

    ASTContext &ctx = getASTContext();
    auto importer = static_cast<ClangImporter *>(ctx.getClangModuleLoader());
    if (importer->hasTypedef(clangDecl))
      return;

    os << clangDecl->getKindName() << " ";
  }

  void visitStructType(StructType *ST,
                       Optional<OptionalTypeKind> optionalKind) {
    const StructDecl *SD = ST->getStructOrBoundGenericStruct();

    // Handle known type names.
    if (printIfKnownSimpleType(SD, optionalKind))
      return;

    // Handle bridged types.
    if (printIfObjCBridgeable(SD, { }, optionalKind))
      return;

    maybePrintTagKeyword(SD);
    os << getNameForObjC(SD);

    // Handle swift_newtype applied to a pointer type.
    if (auto *clangDecl = cast_or_null<clang::TypeDecl>(SD->getClangDecl()))
      if (isClangPointerType(clangDecl))
        printNullability(optionalKind);
  }

  /// Print a collection element type using Objective-C generics syntax.
  ///
  /// This will print the type as bridged to Objective-C.
  void printCollectionElement(Type ty) {
    ASTContext &ctx = getASTContext();

    auto isSwiftNewtype = [](const StructDecl *SD) -> bool {
      if (!SD)
        return false;
      auto *clangDecl = SD->getClangDecl();
      if (!clangDecl)
        return false;
      return clangDecl->hasAttr<clang::SwiftNewTypeAttr>();
    };

    // Use the type as bridged to Objective-C unless the element type is itself
    // an imported type or a collection.
    const StructDecl *SD = ty->getStructOrBoundGenericStruct();
    if (ty->isAny()) {
      ty = ctx.getAnyObjectType();
    } else if (!ty->isKnownStdlibCollectionType() && !isSwiftNewtype(SD)) {
      ty = ctx.getBridgedToObjC(&owningPrinter.M, ty);
    }

    assert(ty && "unknown bridged type");

    print(ty, None);
  }

  /// If \p BGT represents a generic struct used to import Clang types, print
  /// it out.
  bool printIfKnownGenericStruct(const BoundGenericStructType *BGT,
                                 Optional<OptionalTypeKind> optionalKind) {
    auto bgsTy = Type(const_cast<BoundGenericStructType *>(BGT));

    if (bgsTy->isUnmanaged()) {
      auto args = BGT->getGenericArgs();
      assert(args.size() == 1);
      visitPart(args.front(), optionalKind);
      os << " __unsafe_unretained";
      return true;
    }

    // Everything from here on is some kind of pointer type.
    bool isConst;
    if (bgsTy->isUnsafePointer()) {
      isConst = true;
    } else if (bgsTy->isAutoreleasingUnsafeMutablePointer() ||
               bgsTy->isUnsafeMutablePointer()) {
      isConst = false;
    } else {
      // Not a pointer.
      return false;
    }

    auto args = BGT->getGenericArgs();
    assert(args.size() == 1);
    visitPart(args.front(), OTK_None);
    if (isConst)
      os << " const";
    os << " *";
    printNullability(optionalKind);
    return true;
  }

  void visitBoundGenericStructType(BoundGenericStructType *BGT,
                                   Optional<OptionalTypeKind> optionalKind) {
    // Handle bridged types.
    if (printIfObjCBridgeable(BGT->getDecl(), BGT->getGenericArgs(),
                              optionalKind))
      return;

    if (printIfKnownGenericStruct(BGT, optionalKind))
      return;

    visitBoundGenericType(BGT, optionalKind);
  }

  void printGenericArgs(BoundGenericType *BGT) {
    printGenericArgs(BGT->getGenericArgs());
  }

  void printGenericArgs(ArrayRef<Type> genericArgs) {
    os << '<';
    interleave(genericArgs,
               [this](Type t) { print(t, None); },
               [this] { os << ", "; });
    os << '>';
  }

  void visitBoundGenericClassType(BoundGenericClassType *BGT,
                                  Optional<OptionalTypeKind> optionalKind) {
    // Only handle imported ObjC generics.
    auto CD = BGT->getClassOrBoundGenericClass();
    if (!CD->isObjC())
      return visitType(BGT, optionalKind);

    assert(CD->getClangDecl() && "objc generic class w/o clang node?!");
    auto clangDecl = cast<clang::NamedDecl>(CD->getClangDecl());
    if (isa<clang::ObjCInterfaceDecl>(clangDecl)) {
      os << clangDecl->getName();
    } else {
      maybePrintTagKeyword(CD);
      os << clangDecl->getName();
    }
    printGenericArgs(BGT);
    if (isa<clang::ObjCInterfaceDecl>(clangDecl)) {
      os << " *";
    }
    printNullability(optionalKind);
  }

  void visitBoundGenericType(BoundGenericType *BGT,
                             Optional<OptionalTypeKind> optionalKind) {
    // Handle bridged types.
    if (!isa<StructDecl>(BGT->getDecl()) &&
        printIfObjCBridgeable(BGT->getDecl(), BGT->getGenericArgs(),
                              optionalKind))
      return;

    if (auto underlying = BGT->getOptionalObjectType()) {
      visitPart(underlying, OTK_Optional);
    } else
      visitType(BGT, optionalKind);
  }

  void visitEnumType(EnumType *ET, Optional<OptionalTypeKind> optionalKind) {
    const EnumDecl *ED = ET->getDecl();

    // Handle bridged types.
    if (printIfObjCBridgeable(ED, { }, optionalKind))
      return;

    maybePrintTagKeyword(ED);
    os << getNameForObjC(ED);
  }

  void visitClassType(ClassType *CT, Optional<OptionalTypeKind> optionalKind) {
    const ClassDecl *CD = CT->getClassOrBoundGenericClass();
    assert(CD->isObjC());
    auto clangDecl = dyn_cast_or_null<clang::NamedDecl>(CD->getClangDecl());
    if (clangDecl) {
      // Hack for <os/object.h> types, which use classes in Swift but
      // protocols in Objective-C, and a typedef to hide the difference.
      StringRef osObjectName = maybeGetOSObjectBaseName(clangDecl);
      if (!osObjectName.empty()) {
        os << osObjectName << "_t";
      } else if (isa<clang::ObjCInterfaceDecl>(clangDecl)) {
        os << clangDecl->getName() << " *";
      } else {
        maybePrintTagKeyword(CD);
        os << clangDecl->getName();
      }
    } else {
      os << getNameForObjC(CD) << " *";
    }
    printNullability(optionalKind);
  }

  void visitExistentialType(Type T,
                            Optional<OptionalTypeKind> optionalKind,
                            bool isMetatype = false) {
    auto layout = T->getExistentialLayout();
    if (layout.isErrorExistential()) {
      if (isMetatype) os << "Class";
      else os << "NSError *";
      printNullability(optionalKind);
      return;
    }

    if (auto superclass = layout.explicitSuperclass) {
      auto *CD = superclass->getClassOrBoundGenericClass();
      assert(CD->isObjC());
      if (isMetatype) {
        os << "SWIFT_METATYPE(" << getNameForObjC(CD) << ")";
      } else {
        os << getNameForObjC(CD);
        if (auto *BGT = superclass->getAs<BoundGenericClassType>())
          printGenericArgs(BGT);
      }
    } else {
      os << (isMetatype ? "Class" : "id");
    }

    printProtocols(layout.getProtocols());

    if (layout.explicitSuperclass && !isMetatype)
      os << " *";

    printNullability(optionalKind);
  }

  void visitProtocolType(ProtocolType *PT,
                         Optional<OptionalTypeKind> optionalKind) {
    visitExistentialType(PT, optionalKind, /*isMetatype=*/false);
  }

  void visitProtocolCompositionType(ProtocolCompositionType *PCT,
                                    Optional<OptionalTypeKind> optionalKind) {
    visitExistentialType(PCT, optionalKind, /*isMetatype=*/false);
  }

  void visitExistentialType(ExistentialType *ET,
                            Optional<OptionalTypeKind> optionalKind) {
    visitPart(ET->getConstraintType(), optionalKind);
  }

  void visitExistentialMetatypeType(ExistentialMetatypeType *MT,
                                    Optional<OptionalTypeKind> optionalKind) {
    Type instanceTy = MT->getInstanceType();
    visitExistentialType(instanceTy, optionalKind, /*isMetatype=*/true);
  }

  void visitMetatypeType(MetatypeType *MT,
                         Optional<OptionalTypeKind> optionalKind) {
    Type instanceTy = MT->getInstanceType();
    if (auto classTy = instanceTy->getAs<ClassType>()) {
      const ClassDecl *CD = classTy->getDecl();
      assert(CD->isObjC());
      os << "SWIFT_METATYPE(" << getNameForObjC(CD) << ")";
      printNullability(optionalKind);
    } else {
      visitType(MT, optionalKind);
    }
  }

  void visitGenericTypeParamType(GenericTypeParamType *type,
                                 Optional<OptionalTypeKind> optionalKind) {
    const GenericTypeParamDecl *decl = type->getDecl();
    assert(decl && "can't print canonicalized GenericTypeParamType");

    if (auto *extension = dyn_cast<ExtensionDecl>(decl->getDeclContext())) {
      const ClassDecl *extendedClass = extension->getSelfClassDecl();
      assert(extendedClass->isGeneric());
      assert(extension->getGenericParams()->size() ==
             extendedClass->getGenericParams()->size() &&
             "extensions with custom generic parameters?");
      assert(extension->getGenericSignature().getCanonicalSignature() ==
                 extendedClass->getGenericSignature().getCanonicalSignature() &&
             "constrained extensions or custom generic parameters?");
      type = extendedClass->getGenericSignature()->getSugaredType(type);
      decl = type->getDecl();
    }

    if (auto *proto = dyn_cast<ProtocolDecl>(decl->getDeclContext())) {
      if (type->isEqual(proto->getSelfInterfaceType())) {
        printNullability(optionalKind, NullabilityPrintKind::ContextSensitive);
        os << "instancetype";
        return;
      }
    }

    assert(decl->getClangDecl() && "can only handle imported ObjC generics");
    os << cast<clang::ObjCTypeParamDecl>(decl->getClangDecl())->getName();
    printNullability(optionalKind);
  }

  void printFunctionType(FunctionType *FT, char pointerSigil,
                         Optional<OptionalTypeKind> optionalKind) {
    visitPart(FT->getResult(), OTK_None);
    os << " (" << pointerSigil;
    printNullability(optionalKind);
    openFunctionTypes.push_back(FT);
  }

  void visitFunctionType(FunctionType *FT,
                         Optional<OptionalTypeKind> optionalKind) {
    switch (FT->getRepresentation()) {
    case AnyFunctionType::Representation::Thin:
      llvm_unreachable("can't represent thin functions in ObjC");
    // Native Swift function types bridge to block types.
    case AnyFunctionType::Representation::Swift:
    case AnyFunctionType::Representation::Block:
      printFunctionType(FT, '^', optionalKind);
      break;
    case AnyFunctionType::Representation::CFunctionPointer:
      printFunctionType(FT, '*', optionalKind);
    }
  }

  /// Print the part of a function type that appears after where the variable
  /// name would go.
  ///
  /// This is necessary to handle C's awful declarator syntax.
  /// "(A) -> ((B) -> C)" becomes "C (^ (^)(A))(B)".
  void finishFunctionType(const FunctionType *FT) {
    os << ")(";
    if (!FT->getParams().empty()) {
      interleave(
          FT->getParams(),
          [this](const AnyFunctionType::Param &param) {
            switch (param.getValueOwnership()) {
            case ValueOwnership::Default:
            case ValueOwnership::Shared:
              break;
            case ValueOwnership::Owned:
              os << "SWIFT_RELEASES_ARGUMENT ";
              break;
            case ValueOwnership::InOut:
              llvm_unreachable("bad specifier");
            }

            print(param.getParameterType(), OTK_None,
                  param.getLabel().str().str(), IsFunctionParam);
          },
          [this] { os << ", "; });
    } else {
      os << "void";
    }
    os << ")";
  }

  void visitTupleType(TupleType *TT, Optional<OptionalTypeKind> optionalKind) {
    assert(TT->getNumElements() == 0);
    os << "void";
  }

  void visitPackType(PackType *PT, Optional<OptionalTypeKind> optionalKind) {
    assert(PT->getNumElements() == 0);
    os << "void";
  }

  void visitPackExpansionType(PackExpansionType *PET, Optional<OptionalTypeKind> optionalKind) {
    os << "void";
  }

  void visitPackElementType(PackElementType *PET, Optional<OptionalTypeKind> optionalKind) {
    llvm_unreachable("Not implemented");
  }

  void visitParenType(ParenType *PT, Optional<OptionalTypeKind> optionalKind) {
    visitPart(PT->getSinglyDesugaredType(), optionalKind);
  }

  void visitSyntaxSugarType(SyntaxSugarType *SST,
                            Optional<OptionalTypeKind> optionalKind) {
    visitPart(SST->getSinglyDesugaredType(), optionalKind);
  }

  void visitDynamicSelfType(DynamicSelfType *DST,
                            Optional<OptionalTypeKind> optionalKind) {
    printNullability(optionalKind, NullabilityPrintKind::ContextSensitive);
    os << "instancetype";
  }

  void visitReferenceStorageType(ReferenceStorageType *RST,
                                 Optional<OptionalTypeKind> optionalKind) {
    visitPart(RST->getReferentType(), optionalKind);
  }

  /// RAII class for printing multi-part C types, such as functions and arrays.
  class PrintMultiPartType {
    PrinterImpl &Printer;
    decltype(PrinterImpl::openFunctionTypes) savedFunctionTypes;

    PrintMultiPartType(const PrintMultiPartType &) = delete;
  public:
    explicit PrintMultiPartType(PrinterImpl &Printer)
      : Printer(Printer) {
      savedFunctionTypes.swap(Printer.openFunctionTypes);
    }

    void finish() {
      auto &openFunctionTypes = Printer.openFunctionTypes;
      while (!openFunctionTypes.empty()) {
        const FunctionType *openFunctionTy = openFunctionTypes.pop_back_val();
        Printer.finishFunctionType(openFunctionTy);
      }
      openFunctionTypes = std::move(savedFunctionTypes);
      savedFunctionTypes.clear();
    }

    ~PrintMultiPartType() {
      finish();
    }
  };

  /// Print a full type, optionally declaring the given \p name.
  ///
  /// This will properly handle nested function types (see
  /// finishFunctionType()). If only a part of a type is being printed, use
  /// visitPart().
public:
  void print(Type ty, Optional<OptionalTypeKind> optionalKind,
             StringRef name = "",
             IsFunctionParam_t isFuncParam = IsNotFunctionParam) {
    PrettyStackTraceType trace(getASTContext(), "printing", ty);

    if (isFuncParam)
      if (auto fnTy = ty->lookThroughAllOptionalTypes()
                        ->getAs<AnyFunctionType>())
        if (fnTy->isNoEscape())
          os << "SWIFT_NOESCAPE ";

    PrintMultiPartType multiPart(*this);
    visitPart(ty, optionalKind);
    if (!name.empty()) {
      os << ' ' << name;
      if (isClangKeyword(name))
        os << '_';
    }
  }
};

auto DeclAndTypePrinter::getImpl() -> Implementation {
  return Implementation(os, *this, outputLang);
}

static bool isAsyncAlternativeOfOtherDecl(const ValueDecl *VD) {
  auto AFD = dyn_cast<AbstractFunctionDecl>(VD);
  if (!AFD || !AFD->isAsyncContext() || !AFD->getObjCSelector())
    return false;

  auto type = AFD->getDeclContext()->getSelfNominalTypeDecl();
  if (!type)
    return false;
  auto others = type->lookupDirect(AFD->getObjCSelector(),
                                   AFD->isInstanceMember());

  for (auto other : others)
    if (other->getAsyncAlternative() == AFD)
      return true;

  return false;
}

static bool isStringNestedType(const ValueDecl *VD, StringRef Typename) {
  auto ctx = VD->getDeclContext();
  return VD->hasName() && VD->getName().isSimpleName() &&
         VD->getBaseIdentifier().str() == Typename &&
         isa<ExtensionDecl>(ctx->getAsDecl()) &&
         cast<ExtensionDecl>(ctx->getAsDecl())->getExtendedNominal() ==
             VD->getASTContext().getStringDecl();
}

static bool hasExposeAttr(const ValueDecl *VD, bool isExtension = false) {
  if (isa<NominalTypeDecl>(VD) && VD->getModuleContext()->isStdlibModule()) {
    if (VD == VD->getASTContext().getStringDecl())
      return true;
    if (VD == VD->getASTContext().getArrayDecl())
      return true;
    if (VD == VD->getASTContext().getOptionalDecl() && !isExtension)
      return true;
    if (isStringNestedType(VD, "UTF8View") || isStringNestedType(VD, "Index"))
      return true;
    return false;
  }
  // Clang decls don't need to be explicitly exposed.
  if (VD->hasClangNode())
    return true;
  if (VD->getAttrs().hasAttribute<ExposeAttr>())
    return true;
  if (const auto *NMT = dyn_cast<NominalTypeDecl>(VD->getDeclContext()))
    return hasExposeAttr(NMT);
  if (const auto *ED = dyn_cast<ExtensionDecl>(VD->getDeclContext())) {
    // FIXME: Do not expose 'index' methods as the overloads are conflicting.
    // this should either be prohibited in the stdlib module, or the overloads
    // should be renamed automatically or using the expose attribute.
    if ((ED->getExtendedNominal() == VD->getASTContext().getArrayDecl() ||
         ED->getExtendedNominal() == VD->getASTContext().getStringDecl()) &&
        (isa<AbstractFunctionDecl>(VD) &&
         !cast<AbstractFunctionDecl>(VD)->getName().getBaseName().isSpecial() &&
         cast<AbstractFunctionDecl>(VD)
             ->getName()
             .getBaseName()
             .getIdentifier()
             .str()
             .contains_insensitive("index")))
      return false;
    // Limit exposition of String constructors as there's overloading conflict.
    // FIXME: resolve it in some other way.
    if (ED->getExtendedNominal() == VD->getASTContext().getStringDecl()) {
      if (isa<ConstructorDecl>(VD))
        return false;
    }

    return hasExposeAttr(ED->getExtendedNominal(), /*isExtension=*/true);
  }
  return false;
}

/// Skip \c \@objcImplementation \c extension member implementations and
/// overrides. They are already declared in handwritten headers, and they may
/// have attributes that aren't allowed in a category.
///
/// \return true if \p VD should \em not be included in the header.
static bool excludeForObjCImplementation(const ValueDecl *VD) {
  // Exclude member implementations; they are declared elsewhere.
  if (VD->isObjCMemberImplementation())
    return true;
  // Exclude overrides in an @_objcImplementation extension; the decl they're
  // overriding is declared elsewhere.
  if (VD->isImplicit() && VD->getOverriddenDecl()) {
    auto ED = dyn_cast<ExtensionDecl>(VD->getDeclContext());
    if (ED && ED->isObjCImplementation())
      return true;
  }
  return false;
}

static bool isExposedToThisModule(const ModuleDecl &M, const ValueDecl *VD,
                                  const llvm::StringSet<> &exposedModules) {
  if (VD->hasClangNode())
    return true;
  auto *mc = VD->getModuleContext();
  if (mc == &M)
    return true;
  // Only certain declarations are exposed from
  // the standard library.
  if (mc->isStdlibModule())
    return hasExposeAttr(VD);
  return exposedModules.count(mc->getName().str());
}

static bool isEnumExposableToCxx(const ValueDecl *VD,
                                 DeclAndTypePrinter &printer) {
  auto *enumDecl = dyn_cast<EnumDecl>(VD);
  if (!enumDecl)
    return true;
  // The supported set of enum elements is restricted by
  // the types that can be represented in C++. We already
  // check for different type categories in `getDeclRepresentation`, however,
  // we also need to perform additional check on whether the type can be
  // emitted here as well, to ensure that we don't emit types from dependent
  // modules that do not have a C++ representation.
  for (const auto *enumCase : enumDecl->getAllCases()) {
    for (const auto *elementDecl : enumCase->getElements()) {
      if (!elementDecl->hasAssociatedValues())
        continue;
      if (auto *params = elementDecl->getParameterList()) {
        for (const auto *param : *params) {
          auto paramType = param->getInterfaceType();
          if (DeclAndTypeClangFunctionPrinter::getTypeRepresentation(
                  printer.getTypeMapping(), printer.getInteropContext(),
                  printer, enumDecl->getModuleContext(), paramType)
                  .isUnsupported())
            return false;
        }
      }
    }
  }
  return true;
}

bool DeclAndTypePrinter::shouldInclude(const ValueDecl *VD) {
  return !VD->isInvalid() && (!requiresExposedAttribute || hasExposeAttr(VD)) &&
         (outputLang == OutputLanguageMode::Cxx
              ? cxx_translation::isVisibleToCxx(VD, minRequiredAccess) &&
                    isExposedToThisModule(M, VD, exposedModules) &&
                    cxx_translation::isExposableToCxx(VD) &&
                    isEnumExposableToCxx(VD, *this)
              : isVisibleToObjC(VD, minRequiredAccess)) &&
         !VD->getAttrs().hasAttribute<ImplementationOnlyAttr>() &&
         !isAsyncAlternativeOfOtherDecl(VD) &&
         !excludeForObjCImplementation(VD);
}

bool DeclAndTypePrinter::isVisible(const ValueDecl *vd) const {
  return outputLang == OutputLanguageMode::Cxx
             ? cxx_translation::isVisibleToCxx(vd, minRequiredAccess)
             : isVisibleToObjC(vd, minRequiredAccess);
}

void DeclAndTypePrinter::print(const Decl *D) {
  getImpl().print(D);
}

void DeclAndTypePrinter::print(Type ty) {
  getImpl().print(ty, /*overridingOptionality*/None);
}

void DeclAndTypePrinter::printAvailability(raw_ostream &os, const Decl *D) {
  getImpl().printAvailability(os, D);
}

void DeclAndTypePrinter::printAdHocCategory(
    iterator_range<const ValueDecl * const *> members) {
  getImpl().printAdHocCategory(members);
}

bool DeclAndTypePrinter::isEmptyExtensionDecl(const ExtensionDecl *ED) {
  return getImpl().isEmptyExtensionDecl(ED);
}

const TypeDecl *DeclAndTypePrinter::getObjCTypeDecl(const TypeDecl* TD) {
  if (auto *nominal = dyn_cast<NominalTypeDecl>(TD))
    if (auto *bridged = getImpl().getObjCBridgedClass(nominal))
      return bridged;

  return TD;
}

StringRef
DeclAndTypePrinter::maybeGetOSObjectBaseName(const clang::NamedDecl *decl) {
  StringRef name = decl->getName();
  if (!name.consume_front("OS_"))
    return StringRef();

  clang::SourceLocation loc = decl->getLocation();
  if (!loc.isMacroID())
    return StringRef();

  // Hack: check to see if the name came from a macro in <os/object.h>.
  clang::SourceManager &sourceMgr = decl->getASTContext().getSourceManager();
  clang::SourceLocation expansionLoc =
      sourceMgr.getImmediateExpansionRange(loc).getBegin();
  clang::SourceLocation spellingLoc = sourceMgr.getSpellingLoc(expansionLoc);

  if (!sourceMgr.getFilename(spellingLoc).endswith("/os/object.h"))
    return StringRef();

  return name;
}
