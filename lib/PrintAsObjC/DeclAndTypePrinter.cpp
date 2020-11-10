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

#include "swift/AST/ASTContext.h"
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
#include "swift/AST/TypeVisitor.h"
#include "swift/IDE/CommentConversion.h"
#include "swift/Parse/Lexer.h"
#include "swift/Parse/Parser.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclObjC.h"
#include "clang/Basic/CharInfo.h"
#include "clang/Basic/SourceManager.h"

using namespace swift;
using namespace swift::objc_translation;

static bool isNSObjectOrAnyHashable(ASTContext &ctx, Type type) {
  if (auto classDecl = type->getClassOrBoundGenericClass()) {
    return classDecl->getName()
             == ctx.getSwiftId(KnownFoundationEntity::NSObject) &&
           classDecl->getModuleContext()->getName() == ctx.Id_ObjectiveC;
  }
  if (auto nomDecl = type->getAnyNominal()) {
    return nomDecl == ctx.getAnyHashableDecl();
  }

  return false;
}

static bool isAnyObjectOrAny(Type type) {
  return type->isAnyObject() || type->isAny();
}

/// Returns true if \p name matches a keyword in any Clang language mode.
static bool isClangKeyword(Identifier name) {
  static const llvm::DenseSet<StringRef> keywords = []{
    llvm::DenseSet<StringRef> set;
    // FIXME: clang::IdentifierInfo /nearly/ has the API we need to do this
    // in a more principled way, but not quite.
#define KEYWORD(SPELLING, FLAGS) \
    set.insert(#SPELLING);
#define CXX_KEYWORD_OPERATOR(SPELLING, TOK) \
    set.insert(#SPELLING);
#include "clang/Basic/TokenKinds.def"
    return set;
  }();

  if (name.empty())
    return false;
  return keywords.find(name.str()) != keywords.end();
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

class DeclAndTypePrinter::Implementation
  : private DeclVisitor<DeclAndTypePrinter::Implementation>,
    private TypeVisitor<DeclAndTypePrinter::Implementation, void,
                        Optional<OptionalTypeKind>>
{
  using PrinterImpl = Implementation;
  friend ASTVisitor;
  friend TypeVisitor;

  // The output stream is accessible through 'owningPrinter',
  // but it makes the code simpler to have it here too.
  raw_ostream &os;
  DeclAndTypePrinter &owningPrinter;

  SmallVector<const FunctionType *, 4> openFunctionTypes;

  ASTContext &getASTContext() const {
    return owningPrinter.M.getASTContext();
  }

public:
  explicit Implementation(raw_ostream &out, DeclAndTypePrinter &owner)
    : os(out), owningPrinter(owner) {}

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
  void printEncodedString(StringRef str, bool includeQuotes = true) {
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
    OptionalTypeKind kind;
    if (auto objTy =
            ty->getReferenceStorageReferent()->getOptionalObjectType()) {
      kind = OTK_Optional;
      if (D->isImplicitlyUnwrappedOptional())
        kind = OTK_ImplicitlyUnwrappedOptional;

      return {objTy, kind};
    }

    return {ty, OTK_None};
  }

  // Ignore other declarations.
  void visitDecl(Decl *D) {}

  void visitClassDecl(ClassDecl *CD) {
    printDocumentationComment(CD);

    // This is just for testing, so we check explicitly for the attribute instead
    // of asking if the class is weak imported. If the class has availablility,
    // we'll print a SWIFT_AVAIALBLE() which implies __attribute__((weak_imported))
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

  void visitEnumDecl(EnumDecl *ED) {
    printDocumentationComment(ED);
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
      print(objTy, kind, Identifier(), IsFunctionParam);
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
                            FunctionType *methodTy,
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

  void printAbstractFunctionAsMethod(AbstractFunctionDecl *AFD,
                                     bool isClassMethod,
                                     bool isNSUIntegerSubscript = false) {
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

    Optional<ForeignAsyncConvention> asyncConvention
      = AFD->getForeignAsyncConvention();
    Optional<ForeignErrorConvention> errorConvention
      = AFD->getForeignErrorConvention();
    Type rawMethodTy = AFD->getMethodInterfaceType();
    auto methodTy = rawMethodTy->castTo<FunctionType>();
    auto resultTy = getForeignResultType(
        AFD, methodTy, asyncConvention, errorConvention);

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
        print(asyncConvention->completionHandlerType(), None);
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

  void printAbstractFunctionAsFunction(FuncDecl *FD) {
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

    // The result type may be a partial function type we need to close
    // up later.
    PrintMultiPartType multiPart(*this);

    OptionalTypeKind kind;
    Type objTy;
    std::tie(objTy, kind) = getObjectTypeAndOptionality(FD, resultTy);
    visitPart(objTy, kind);

    assert(FD->getAttrs().hasAttribute<CDeclAttr>()
           && "not a cdecl function");

    os << ' ' << FD->getAttrs().getAttribute<CDeclAttr>()->Name << '(';

    auto params = FD->getParameters();
    if (params->size()) {
      interleave(*params,
                 [&](const ParamDecl *param) {
                   OptionalTypeKind kind;
                   Type objTy;
                   std::tie(objTy, kind) = getObjectTypeAndOptionality(
                       param, param->getInterfaceType());
                   print(objTy, kind, param->getName(), IsFunctionParam);
                 },
                 [&] { os << ", "; });
    } else {
      os << "void";
    }

    os << ')';

    // Finish the result type.
    multiPart.finish();

    if (funcTy->getResult()->isUninhabited()) {
      os << " SWIFT_NORETURN";
    } else if (!funcTy->getResult()->isVoid() &&
               !FD->getAttrs().hasAttribute<DiscardableResultAttr>()) {
      os << " SWIFT_WARN_UNUSED_RESULT";
    }

    printAvailability(FD);

    os << ';';
  }

  enum class PrintLeadingSpace : bool {
    No = false,
    Yes = true
  };

  /// Returns \c true if anything was printed.
  bool printAvailability(const Decl *D, PrintLeadingSpace printLeadingSpace =
                                            PrintLeadingSpace::Yes) {
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
            printRenameForDecl(AvAttr, cast<ValueDecl>(D), false);
            os << '\'';
            if (!AvAttr->Message.empty()) {
              os << ": ";
              printEncodedString(AvAttr->Message, false);
            }
            os << "\")";
          } else if (!AvAttr->Message.empty()) {
            maybePrintLeadingSpace();
            os << "SWIFT_UNAVAILABLE_MSG(";
            printEncodedString(AvAttr->Message);
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
            printEncodedString(AvAttr->Message);
            if (!AvAttr->Rename.empty()) {
              os << ", ";
              printRenameForDecl(AvAttr, cast<ValueDecl>(D), true);
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
      if (!AvAttr->Introduced.hasValue() && !AvAttr->Deprecated.hasValue() &&
          !AvAttr->Obsoleted.hasValue() &&
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
      case PlatformKind::none:
        llvm_unreachable("handled above");
      }

      maybePrintLeadingSpace();
      os << "SWIFT_AVAILABILITY(" << plat;
      if (AvAttr->isUnconditionallyUnavailable()) {
        os << ",unavailable";
      } else {
        if (AvAttr->Introduced.hasValue()) {
          os << ",introduced=" << AvAttr->Introduced.getValue().getAsString();
        }
        if (AvAttr->Deprecated.hasValue()) {
          os << ",deprecated=" << AvAttr->Deprecated.getValue().getAsString();
        } else if (AvAttr->isUnconditionallyDeprecated()) {
          // We need to specify some version, we can't just say deprecated.
          // We also can't deprecate it before it's introduced.
          if (AvAttr->Introduced.hasValue()) {
            os << ",deprecated=" << AvAttr->Introduced.getValue().getAsString();
          } else {
            os << ",deprecated=0.0.1";
          }
        }
        if (AvAttr->Obsoleted.hasValue()) {
          os << ",obsoleted=" << AvAttr->Obsoleted.getValue().getAsString();
        }
      }
      if (!AvAttr->Rename.empty() && isa<ValueDecl>(D)) {
        os << ",message=\"'" << cast<ValueDecl>(D)->getBaseName()
           << "' has been renamed to '";
        printRenameForDecl(AvAttr, cast<ValueDecl>(D), false);
        os << '\'';
        if (!AvAttr->Message.empty()) {
          os << ": ";
          printEncodedString(AvAttr->Message, false);
        }
        os << "\"";
      } else if (!AvAttr->Message.empty()) {
        os << ",message=";
        printEncodedString(AvAttr->Message);
      }
      os << ")";
    }
    return hasPrintedAnything;
  }

  const ValueDecl *getRenameDecl(const ValueDecl *D,
                                 const ParsedDeclName renamedParsedDeclName) {
    auto declContext = D->getDeclContext();
    ASTContext &astContext = D->getASTContext();
    auto renamedDeclName = renamedParsedDeclName.formDeclNameRef(astContext);

    if (isa<ClassDecl>(D) || isa<ProtocolDecl>(D)) {
      if (!renamedParsedDeclName.ContextName.empty()) {
        return nullptr;
      }
      SmallVector<ValueDecl *, 1> decls;
      declContext->lookupQualified(declContext->getParentModule(),
          renamedDeclName.withoutArgumentLabels(astContext), NL_OnlyTypes,
          decls);
      if (decls.size() == 1)
        return decls[0];
      return nullptr;
    }

    TypeDecl *typeDecl = declContext->getSelfNominalTypeDecl();

    const ValueDecl *renamedDecl = nullptr;
    SmallVector<ValueDecl *, 4> lookupResults;
    declContext->lookupQualified(typeDecl->getDeclaredInterfaceType(),
                                 renamedDeclName, NL_QualifiedDefault,
                                 lookupResults);

    if (lookupResults.size() == 1) {
      auto candidate = lookupResults[0];
      if (!shouldInclude(candidate))
        return nullptr;
      if (candidate->getKind() != D->getKind() ||
          (candidate->isInstanceMember() !=
           cast<ValueDecl>(D)->isInstanceMember()))
        return nullptr;

      renamedDecl = candidate;
    } else {
      for (auto candidate : lookupResults) {
        if (!shouldInclude(candidate))
          continue;

        if (candidate->getKind() != D->getKind() ||
            (candidate->isInstanceMember() !=
             cast<ValueDecl>(D)->isInstanceMember()))
          continue;

        if (isa<AbstractFunctionDecl>(candidate)) {
          auto cParams = cast<AbstractFunctionDecl>(candidate)->getParameters();
          auto dParams = cast<AbstractFunctionDecl>(D)->getParameters();

          if (cParams->size() != dParams->size())
            continue;

          bool hasSameParameterTypes = true;
          for (auto index : indices(*cParams)) {
            auto cParamsType = cParams->get(index)->getType();
            auto dParamsType = dParams->get(index)->getType();
            if (!cParamsType->matchesParameter(dParamsType,
                                               TypeMatchOptions())) {
              hasSameParameterTypes = false;
              break;
            }
          }

          if (!hasSameParameterTypes) {
            continue;
          }
        }

        if (renamedDecl) {
          // If we found a duplicated candidate then we would silently fail.
          renamedDecl = nullptr;
          break;
        }
        renamedDecl = candidate;
      }
    }
    return renamedDecl;
  }

  void printRenameForDecl(const AvailableAttr *AvAttr, const ValueDecl *D,
                          bool includeQuotes) {
    assert(!AvAttr->Rename.empty());

    const ValueDecl *renamedDecl =
        getRenameDecl(D, parseDeclName(AvAttr->Rename));

    if (renamedDecl) {
      SmallString<128> scratch;
      auto renamedObjCRuntimeName =
          renamedDecl->getObjCRuntimeName()->getString(scratch);
      printEncodedString(renamedObjCRuntimeName, includeQuotes);
    } else {
      printEncodedString(AvAttr->Rename, includeQuotes);
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
    printEncodedString(nominal->getName().str(), /*includeQuotes=*/false);
    os << ".";
    SmallString<32> scratch;
    printEncodedString(VD->getName().getString(scratch),
                       /*includeQuotes=*/false);
    os << "' uses '@objc' inference deprecated in Swift 4; add '@objc' to "
       <<   "provide an Objective-C entrypoint\")";
  }

  void visitFuncDecl(FuncDecl *FD) {
    if (FD->getDeclContext()->isTypeContext())
      printAbstractFunctionAsMethod(FD, FD->isStatic());
    else
      printAbstractFunctionAsFunction(FD);
  }

  void visitConstructorDecl(ConstructorDecl *CD) {
    printAbstractFunctionAsMethod(CD, false);
  }

  bool maybePrintIBOutletCollection(Type ty) {
    if (auto unwrapped = ty->getOptionalObjectType())
      ty = unwrapped;

    auto genericTy = ty->getAs<BoundGenericStructType>();
    if (!genericTy || genericTy->getDecl() != getASTContext().getArrayDecl())
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
        if (nominal == ctx.getArrayDecl() ||
            nominal == ctx.getDictionaryDecl() ||
            nominal == ctx.getSetDecl() ||
            nominal == ctx.getStringDecl() ||
            (!getKnownTypeInfo(nominal) && getObjCBridgedClass(nominal))) {
          // We fast-path the most common cases in the condition above.
          os << ", copy";
        } else if (nominal == ctx.getUnmanagedDecl()) {
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
      print(objTy, kind, objCName);
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

  /// Where nullability information should be printed.
  enum class NullabilityPrintKind {
    Before,
    After,
    ContextSensitive,
  };

  void printNullability(Optional<OptionalTypeKind> kind,
                        NullabilityPrintKind printKind
                          = NullabilityPrintKind::After) {
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
  /// _ObjectiveCBridgeable), return that class.
  ///
  /// Otherwise returns null.
  const ClassDecl *getObjCBridgedClass(const NominalTypeDecl *nominal) {
    // Print imported bridgeable decls as their unbridged type.
    if (nominal->hasClangNode())
      return nullptr;

    auto &ctx = nominal->getASTContext();

    // Dig out the ObjectiveCBridgeable protocol.
    auto proto = ctx.getProtocol(KnownProtocolKind::ObjectiveCBridgeable);
    if (!proto) return nullptr;

    // Determine whether this nominal type is _ObjectiveCBridgeable.
    SmallVector<ProtocolConformance *, 2> conformances;
    if (!nominal->lookupConformance(&owningPrinter.M, proto, conformances))
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
  /// primitives and basic types, return the info in \c specialNames containing
  /// the Clang name and whether it can be nullable in C.
  Optional<CTypeInfo> getKnownTypeInfo(const TypeDecl *typeDecl) {
    auto &specialNames = owningPrinter.specialNames;
    if (specialNames.empty()) {
      ASTContext &ctx = getASTContext();
#define MAP(SWIFT_NAME, CLANG_REPR, NEEDS_NULLABILITY)                       \
      specialNames[{ctx.StdlibModuleName, ctx.getIdentifier(#SWIFT_NAME)}] = \
        { CLANG_REPR, NEEDS_NULLABILITY }

      MAP(CBool, "bool", false);

      MAP(CChar, "char", false);
      MAP(CWideChar, "wchar_t", false);
      MAP(CChar16, "char16_t", false);
      MAP(CChar32, "char32_t", false);

      MAP(CSignedChar, "signed char", false);
      MAP(CShort, "short", false);
      MAP(CInt, "int", false);
      MAP(CLong, "long", false);
      MAP(CLongLong, "long long", false);

      MAP(CUnsignedChar, "unsigned char", false);
      MAP(CUnsignedShort, "unsigned short", false);
      MAP(CUnsignedInt, "unsigned int", false);
      MAP(CUnsignedLong, "unsigned long", false);
      MAP(CUnsignedLongLong, "unsigned long long", false);

      MAP(CFloat, "float", false);
      MAP(CDouble, "double", false);

      MAP(Int8, "int8_t", false);
      MAP(Int16, "int16_t", false);
      MAP(Int32, "int32_t", false);
      MAP(Int64, "int64_t", false);
      MAP(UInt8, "uint8_t", false);
      MAP(UInt16, "uint16_t", false);
      MAP(UInt32, "uint32_t", false);
      MAP(UInt64, "uint64_t", false);

      MAP(Float, "float", false);
      MAP(Double, "double", false);
      MAP(Float32, "float", false);
      MAP(Float64, "double", false);

      MAP(Int, "NSInteger", false);
      MAP(UInt, "NSUInteger", false);
      MAP(Bool, "BOOL", false);

      MAP(OpaquePointer, "void *", true);
      MAP(UnsafeRawPointer, "void const *", true);
      MAP(UnsafeMutableRawPointer, "void *", true);

      Identifier ID_ObjectiveC = ctx.Id_ObjectiveC;
      specialNames[{ID_ObjectiveC, ctx.getIdentifier("ObjCBool")}]
        = { "BOOL", false};
      specialNames[{ID_ObjectiveC, ctx.getIdentifier("Selector")}]
        = { "SEL", true };
      specialNames[{ID_ObjectiveC,
                    ctx.getIdentifier(
                      ctx.getSwiftName(KnownFoundationEntity::NSZone))}]
        = { "struct _NSZone *", true };

      specialNames[{ctx.Id_Darwin, ctx.getIdentifier("DarwinBoolean")}]
        = { "Boolean", false};

      Identifier ID_CoreGraphics = ctx.getIdentifier("CoreGraphics");
      specialNames[{ID_CoreGraphics, ctx.getIdentifier("CGFloat")}]
        = { "CGFloat", false };

      // Use typedefs we set up for SIMD vector types.
#define MAP_SIMD_TYPE(BASENAME, _, __) \
      specialNames[{ctx.Id_simd, ctx.getIdentifier(#BASENAME "2")}] \
        = { "swift_" #BASENAME "2", false };                        \
      specialNames[{ctx.Id_simd, ctx.getIdentifier(#BASENAME "3")}] \
        = { "swift_" #BASENAME "3", false };                        \
      specialNames[{ctx.Id_simd, ctx.getIdentifier(#BASENAME "4")}] \
        = { "swift_" #BASENAME "4", false };
#include "swift/ClangImporter/SIMDMappedTypes.def"
      static_assert(SWIFT_MAX_IMPORTED_SIMD_ELEMENTS == 4,
                    "must add or remove special name mappings if max number of "
                    "SIMD elements is changed");
    }

    Identifier moduleName = typeDecl->getModuleContext()->getName();
    Identifier name = typeDecl->getName();
    auto iter = specialNames.find({moduleName, name});
    if (iter == specialNames.end())
      return None;
    return iter->second;
  }

  /// If \p typeDecl is one of the standard library types used to map in Clang
  /// primitives and basic types, print out the appropriate spelling and
  /// return true.
  ///
  /// This handles typealiases and structs provided by the standard library
  /// for interfacing with C and Objective-C.
  bool printIfKnownSimpleType(const TypeDecl *typeDecl,
                              Optional<OptionalTypeKind> optionalKind) {
    Optional<CTypeInfo> knownTypeInfo = getKnownTypeInfo(typeDecl);
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
      return clangDecl->hasAttr<clang::SwiftNewtypeAttr>();
    };

    // Use the type as bridged to Objective-C unless the element type is itself
    // an imported type or a collection.
    const StructDecl *SD = ty->getStructOrBoundGenericStruct();
    if (ty->isAny()) {
      ty = ctx.getAnyObjectType();
    } else if (SD != ctx.getArrayDecl() &&
        SD != ctx.getDictionaryDecl() &&
        SD != ctx.getSetDecl() &&
        !isSwiftNewtype(SD)) {
      ty = ctx.getBridgedToObjC(&owningPrinter.M, ty);
    }

    assert(ty && "unknown bridged type");

    print(ty, None);
  }

  /// If \p BGT represents a generic struct used to import Clang types, print
  /// it out.
  bool printIfKnownGenericStruct(const BoundGenericStructType *BGT,
                                 Optional<OptionalTypeKind> optionalKind) {
    StructDecl *SD = BGT->getDecl();
    if (!SD->getModuleContext()->isStdlibModule())
      return false;

    ASTContext &ctx = getASTContext();

    if (SD == ctx.getUnmanagedDecl()) {
      auto args = BGT->getGenericArgs();
      assert(args.size() == 1);
      visitPart(args.front(), optionalKind);
      os << " __unsafe_unretained";
      return true;
    }

    // Everything from here on is some kind of pointer type.
    bool isConst;
    if (SD == ctx.getUnsafePointerDecl()) {
      isConst = true;
    } else if (SD == ctx.getAutoreleasingUnsafeMutablePointerDecl() ||
               SD == ctx.getUnsafeMutablePointerDecl()) {
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

    SmallVector<ProtocolDecl *, 2> protos;
    for (auto proto : layout.getProtocols())
      protos.push_back(proto->getDecl());
    printProtocols(protos);

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
      interleave(FT->getParams(),
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

                   print(param.getParameterType(), OTK_None, param.getLabel(),
                         IsFunctionParam);
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
             Identifier name = Identifier(),
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
      if (isClangKeyword(name)) {
        os << '_';
      }
    }
  }
};

auto DeclAndTypePrinter::getImpl() -> Implementation {
  return Implementation(os, *this);
}

bool DeclAndTypePrinter::shouldInclude(const ValueDecl *VD) {
  return isVisibleToObjC(VD, minRequiredAccess) &&
         !VD->getAttrs().hasAttribute<ImplementationOnlyAttr>();
}

void DeclAndTypePrinter::print(const Decl *D) {
  getImpl().print(D);
}

void DeclAndTypePrinter::print(Type ty) {
  getImpl().print(ty, /*overridingOptionality*/None);
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
