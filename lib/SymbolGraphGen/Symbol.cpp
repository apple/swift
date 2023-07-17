//===--- Symbol.cpp - Symbol Graph Node -----------------------------------===//
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

#include "swift/AST/ASTContext.h"
#include "swift/AST/Comment.h"
#include "swift/AST/Module.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/RawComment.h"
#include "swift/AST/USRGeneration.h"
#include "swift/Basic/PrimitiveParsing.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/Unicode.h"
#include "swift/SymbolGraphGen/DocumentationCategory.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/SourceManager.h"
#include "AvailabilityMixin.h"
#include "JSON.h"
#include "Symbol.h"
#include "SymbolGraph.h"
#include "SymbolGraphASTWalker.h"
#include "DeclarationFragmentPrinter.h"

#include <queue>

using namespace swift;
using namespace symbolgraphgen;

Symbol::Symbol(SymbolGraph *Graph, const ExtensionDecl *ED,
               const NominalTypeDecl *SynthesizedBaseTypeDecl, Type BaseType)
    : Symbol::Symbol(Graph, nullptr, ED, SynthesizedBaseTypeDecl, BaseType) {}

Symbol::Symbol(SymbolGraph *Graph, const ValueDecl *VD,
               const NominalTypeDecl *SynthesizedBaseTypeDecl, Type BaseType)
    : Symbol::Symbol(Graph, VD, nullptr, SynthesizedBaseTypeDecl, BaseType) {}

Symbol::Symbol(SymbolGraph *Graph, const ValueDecl *VD, const ExtensionDecl *ED,
               const NominalTypeDecl *SynthesizedBaseTypeDecl, Type BaseType)
    : Graph(Graph), D(VD), BaseType(BaseType),
      SynthesizedBaseTypeDecl(SynthesizedBaseTypeDecl) {
  if (!BaseType && SynthesizedBaseTypeDecl)
    BaseType = SynthesizedBaseTypeDecl->getDeclaredInterfaceType();
  if (D == nullptr) {
    D = ED;
  }
}

void Symbol::serializeKind(StringRef Identifier, StringRef DisplayName,
                           llvm::json::OStream &OS) const {
  OS.object([&](){
    OS.attribute("identifier", Identifier);
    OS.attribute("displayName", DisplayName);
  });
}

std::pair<StringRef, StringRef> Symbol::getKind(const Decl *D) {
  // Make sure supportsKind stays in sync with getKind.
  assert(Symbol::supportsKind(D->getKind()) && "unsupported decl kind");
  switch (D->getKind()) {
  case swift::DeclKind::Class:
    return {"swift.class", "Class"};
  case swift::DeclKind::Struct:
    return {"swift.struct", "Structure"};
  case swift::DeclKind::Enum:
    return {"swift.enum", "Enumeration"};
  case swift::DeclKind::EnumElement:
    return {"swift.enum.case", "Case"};
  case swift::DeclKind::Protocol:
    return {"swift.protocol", "Protocol"};
  case swift::DeclKind::Constructor:
    return {"swift.init", "Initializer"};
  case swift::DeclKind::Destructor:
    return {"swift.deinit", "Deinitializer"};
  case swift::DeclKind::Func: {
    const auto *VD = cast<ValueDecl>(D);

    if (VD->isOperator())
      return {"swift.func.op", "Operator"};
    if (VD->isStatic())
      return {"swift.type.method", "Type Method"};
    if (VD->getDeclContext()->getSelfNominalTypeDecl())
      return {"swift.method", "Instance Method"};
    return {"swift.func", "Function"};
  }
  case swift::DeclKind::Param: LLVM_FALLTHROUGH;
  case swift::DeclKind::Var: {
    const auto *VD = cast<ValueDecl>(D);

    if (VD->isStatic())
      return {"swift.type.property", "Type Property"};
    if (VD->getDeclContext()->getSelfNominalTypeDecl())
      return {"swift.property", "Instance Property"};
    return {"swift.var", "Global Variable"};
  }
  case swift::DeclKind::Subscript: {
    const auto *VD = cast<ValueDecl>(D);

    if (VD->isStatic())
      return {"swift.type.subscript", "Type Subscript"};
    return {"swift.subscript", "Instance Subscript"};
  }
  case swift::DeclKind::TypeAlias:
    return {"swift.typealias", "Type Alias"};
  case swift::DeclKind::AssociatedType:
    return {"swift.associatedtype", "Associated Type"};
  case swift::DeclKind::Extension:
    return {"swift.extension", "Extension"};
  case swift::DeclKind::Macro:
    return {"swift.macro", "Macro"};
  default:
    llvm::errs() << "Unsupported kind: " << D->getKindName(D->getKind());
    llvm_unreachable("Unsupported declaration kind for symbol graph");
  }
}

void Symbol::serializeKind(llvm::json::OStream &OS) const {
  AttributeRAII A("kind", OS);
  std::pair<StringRef, StringRef> IDAndName = getKind(D);
  serializeKind(IDAndName.first, IDAndName.second, OS);
}

void Symbol::serializeIdentifier(llvm::json::OStream &OS) const {
  OS.attributeObject("identifier", [&](){
    SmallString<256> USR;
    getUSR(USR);
    OS.attribute("precise", USR.str());
    OS.attribute("interfaceLanguage", "swift");
  });
}

void Symbol::serializePathComponents(llvm::json::OStream &OS) const {
  OS.attributeArray("pathComponents", [&](){
    SmallVector<PathComponent, 8> PathComponents;
    getPathComponents(PathComponents);
    for (auto Component : PathComponents) {
      OS.value(Component.Title);
    }
  });
}

void Symbol::serializeNames(llvm::json::OStream &OS) const {
  OS.attributeObject("names", [&](){
    SmallVector<PathComponent, 8> PathComponents;
    getPathComponents(PathComponents);

    const ValueDecl *Decl = nullptr;
    if (const auto *ED = dyn_cast<ExtensionDecl>(D)) {
      Decl = ED->getExtendedNominal();
    } else if (const auto *VD = dyn_cast<ValueDecl>(D)) {
      Decl = VD;
    }
      
    if (isa<GenericTypeDecl>(Decl) || isa<EnumElementDecl>(Decl)) {
      SmallString<64> FullyQualifiedTitle;

      for (const auto *It = PathComponents.begin(); It != PathComponents.end(); ++It) {
        if (It != PathComponents.begin()) {
          FullyQualifiedTitle.push_back('.');
        }
        FullyQualifiedTitle.append(It->Title);
      }
      
      OS.attribute("title", FullyQualifiedTitle.str());
    } else {
      OS.attribute("title", PathComponents.back().Title);
    }

    Graph->serializeNavigatorDeclarationFragments("navigator", *this, OS);
    Graph->serializeSubheadingDeclarationFragments("subHeading", *this, OS);
    // "prose": null
  });
}

void Symbol::serializePosition(StringRef Key, SourceLoc Loc,
                               SourceManager &SourceMgr,
                               llvm::json::OStream &OS) const {
  // Note: Line and columns are zero-based in this serialized format.
  auto LineAndColumn = SourceMgr.getPresumedLineAndColumnForLoc(Loc);
  auto Line = LineAndColumn.first - 1;
  auto Column = LineAndColumn.second - 1;

  OS.attributeObject(Key, [&](){
    OS.attribute("line", Line);
    OS.attribute("character", Column);
  });
}

void Symbol::serializeRange(size_t InitialIndentation,
                            SourceRange Range, SourceManager &SourceMgr,
                            llvm::json::OStream &OS) const {
  OS.attributeObject("range", [&](){
    // Note: Line and columns in the serialized format are zero-based.
    auto Start = Range.Start.getAdvancedLoc(InitialIndentation);
    serializePosition("start", Start, SourceMgr, OS);

    auto End = SourceMgr.isBeforeInBuffer(Range.End, Start)
      ? Start
      : Range.End;
    serializePosition("end", End, SourceMgr, OS);
  });
}

const ValueDecl *Symbol::getDeclInheritingDocs() const {
  // get the decl that would provide docs for this symbol
  const auto *DocCommentProvidingDecl =
      dyn_cast_or_null<ValueDecl>(getDocCommentProvidingDecl(D));

  // if the decl is the same as the one for this symbol, we're not
  // inheriting docs, so return null. however, if this symbol is
  // a synthesized symbol, `D` is actually the source symbol, and
  // we should point to that one regardless.
  if (DocCommentProvidingDecl == D && !SynthesizedBaseTypeDecl) {
    return nullptr;
  } else {
    // otherwise, return whatever `getDocCommentProvidingDecl` returned.
    // it will be null if there are no decls that provide docs for this
    // symbol.
    return DocCommentProvidingDecl;
  }
}

const ValueDecl *Symbol::getForeignProtocolRequirement() const {
  if (const auto *VD = dyn_cast_or_null<ValueDecl>(D)) {
    std::queue<const ValueDecl *> requirements;
    while (true) {
      for (auto *req : VD->getSatisfiedProtocolRequirements()) {
        if (req->getModuleContext()->getNameStr() != Graph->M.getNameStr())
          return req;
        else
          requirements.push(req);
      }
      if (requirements.empty())
        return nullptr;
      VD = requirements.front();
      requirements.pop();
    }
  }

  return nullptr;
}

const ValueDecl *Symbol::getProtocolRequirement() const {
  if (const auto *VD = dyn_cast_or_null<ValueDecl>(D)) {
    auto reqs = VD->getSatisfiedProtocolRequirements();

    if (!reqs.empty())
      return reqs.front();
    else
      return nullptr;
  }

  return nullptr;
}

const ValueDecl *Symbol::getInheritedDecl() const {
  const ValueDecl *InheritingDecl = nullptr;
  if (const auto *ID = getDeclInheritingDocs())
    InheritingDecl = ID;

  if (!InheritingDecl && getSynthesizedBaseTypeDecl())
    InheritingDecl = getSymbolDecl();

  if (!InheritingDecl) {
    if (const auto *ID = getForeignProtocolRequirement())
      InheritingDecl = ID;
  }

  if (!InheritingDecl) {
    if (const auto *ID = getProtocolRequirement())
      InheritingDecl = ID;
  }

  return InheritingDecl;
}

namespace {

StringRef getFileNameForDecl(const Decl *D) {
  if (!D) return StringRef{};

  SourceLoc Loc = D->getLoc(/*SerializedOK=*/true);
  if (Loc.isInvalid()) return StringRef{};

  SourceManager &SourceM = D->getASTContext().SourceMgr;
  return SourceM.getDisplayNameForLoc(Loc);
}

StringRef getFileNameForDecl(const clang::Decl *ClangD) {
  if (!ClangD) return StringRef{};

  const clang::SourceManager &ClangSourceMgr = ClangD->getASTContext().getSourceManager();
  clang::PresumedLoc Loc = ClangSourceMgr.getPresumedLoc(ClangD->getLocation());
  if (Loc.isInvalid()) return StringRef{};

  return StringRef(Loc.getFilename());
}

void serializeFileURI(llvm::json::OStream &OS, StringRef FileName) {
  // FIXME: This can emit invalid URIs if the file name has a space in it (rdar://69242070)
  SmallString<1024> FileURI("file://");
  FileURI.append(FileName);
  OS.attribute("uri", FileURI.str());
}

}

void Symbol::serializeDocComment(llvm::json::OStream &OS) const {
  if (ClangNode ClangN = D->getClangNode()) {
    if (!Graph->Walker.Options.IncludeClangDocs)
      return;

    if (auto *ClangD = ClangN.getAsDecl()) {
      const clang::ASTContext &ClangContext = ClangD->getASTContext();
      const clang::RawComment *RC =
          ClangContext.getRawCommentForAnyRedecl(ClangD);
      if (!RC || !RC->isDocumentation())
        return;

      // TODO: Replace this with `getFormattedLines` when it's in and add the
      // line and column ranges. Also consider handling cross-language
      // hierarchies, ie. if there's no comment on the ObjC decl we should
      // look up the hierarchy (and vice versa).
      std::string Text = RC->getFormattedText(ClangContext.getSourceManager(),
                                              ClangContext.getDiagnostics());
      Text = unicode::sanitizeUTF8(Text);

      SmallVector<StringRef, 8> Lines;
      splitIntoLines(Text, Lines);

      OS.attributeObject("docComment", [&]() {
        StringRef FileName = getFileNameForDecl(ClangD);
        if (!FileName.empty())
          serializeFileURI(OS, FileName);
        if (const auto *ModuleD = D->getModuleContext()) {
          OS.attribute("module", ModuleD->getNameStr());
        }
        OS.attributeArray("lines", [&]() {
          for (StringRef Line : Lines) {
            OS.object([&](){
              OS.attribute("text", Line);
            });
          }
        });
      });
    }
    return;
  }

  const auto *DocCommentProvidingDecl = D;
  if (!Graph->Walker.Options.SkipInheritedDocs) {
    DocCommentProvidingDecl =
        dyn_cast_or_null<ValueDecl>(getDocCommentProvidingDecl(D));
    if (!DocCommentProvidingDecl) {
      DocCommentProvidingDecl = D;
    }
  }
  auto RC = DocCommentProvidingDecl->getRawComment();
  if (RC.isEmpty()) {
    return;
  }

  OS.attributeObject("docComment", [&](){
    StringRef FileName = getFileNameForDecl(DocCommentProvidingDecl);
    if (!FileName.empty())
      serializeFileURI(OS, FileName);
    if (const auto *ModuleD = DocCommentProvidingDecl->getModuleContext()) {
      OS.attribute("module", ModuleD->getNameStr());
    }
    auto LL = Graph->Ctx.getLineList(RC);
    StringRef FirstNonBlankLine;
    for (const auto &Line : LL.getLines()) {
      if (!Line.Text.empty()) {
        FirstNonBlankLine = Line.Text;
        break;
      }
    }
    size_t InitialIndentation = FirstNonBlankLine.empty()
      ? 0
      : markup::measureIndentation(FirstNonBlankLine);
    OS.attributeArray("lines", [&](){
      for (const auto &Line : LL.getLines()) {
        // Line object
        OS.object([&](){
          // Trim off any initial indentation from the line's
          // text and start of its source range, if it has one.
          if (Line.Range.isValid()) {
            serializeRange(std::min(InitialIndentation,
                                    Line.FirstNonspaceOffset),
                           Line.Range, Graph->M.getASTContext().SourceMgr, OS);
          }
          auto TrimmedLine = Line.Text.drop_front(std::min(InitialIndentation,
                                                  Line.FirstNonspaceOffset));
          OS.attribute("text", TrimmedLine);
        });
      }
    }); // end lines: []
  }); // end docComment:
}

void Symbol::serializeFunctionSignature(llvm::json::OStream &OS) const {
  if (const auto *FD = dyn_cast_or_null<FuncDecl>(D)) {
    OS.attributeObject("functionSignature", [&](){

      // Parameters
      if (const auto *ParamList = FD->getParameters()) {
        if (ParamList->size()) {
          OS.attributeArray("parameters", [&](){
            for (const auto *Param : *ParamList) {
              auto ExternalName = Param->getArgumentName().str();
              auto InternalName = Param->getParameterName().str();

              OS.object([&](){
                if (ExternalName.empty()) {
                  OS.attribute("name", InternalName);
                } else {
                  OS.attribute("name", ExternalName);
                  if (ExternalName != InternalName &&
                      !InternalName.empty()) {
                    OS.attribute("internalName", InternalName);
                  }
                }
                Graph->serializeDeclarationFragments("declarationFragments",
                                                     Symbol(Graph, Param,
                                                            getSynthesizedBaseTypeDecl(),
                                                            getBaseType()), OS);
              }); // end parameter object
            }
          }); // end parameters:
        }
      }

      // Returns
      if (const auto ReturnType = FD->getResultInterfaceType()) {
        Graph->serializeDeclarationFragments("returns", ReturnType, BaseType,
                                             OS);
      }
    });
  }
}

static SubstitutionMap getSubMapForDecl(const ValueDecl *D, Type BaseType) {
  if (!BaseType || BaseType->isExistentialType())
    return {};

  // Map from the base type into the this declaration's innermost type context,
  // or if we're dealing with an extension rather than a member, into its
  // extended nominal (the extension's own requirements shouldn't be considered
  // in the substitution).
  swift::DeclContext *DC;
  if (isa<swift::ExtensionDecl>(D))
    DC = cast<swift::ExtensionDecl>(D)->getExtendedNominal();
  else
    DC = D->getInnermostDeclContext()->getInnermostTypeContext();

  swift::ModuleDecl *M = DC->getParentModule();
  if (isa<swift::NominalTypeDecl>(D) || isa<swift::ExtensionDecl>(D)) {
    return BaseType->getContextSubstitutionMap(M, DC);
  }

  const swift::ValueDecl *SubTarget = D;
  if (isa<swift::ParamDecl>(D)) {
    auto *DC = D->getDeclContext();
    if (auto *FD = dyn_cast<swift::AbstractFunctionDecl>(DC))
      SubTarget = FD;
  }
  return BaseType->getMemberSubstitutionMap(M, SubTarget);
}

void Symbol::serializeSwiftGenericMixin(llvm::json::OStream &OS) const {
  SubstitutionMap SubMap;
  const auto *VD = dyn_cast<ValueDecl>(D);

  if (VD && BaseType) {
    SubMap = getSubMapForDecl(VD, BaseType);
  } else {
    SubMap = {};
  }

  if (const auto *GC = D->getAsGenericContext()) {
    if (const auto Generics = GC->getGenericSignature()) {

      SmallVector<const GenericTypeParamType *, 4> FilteredParams;
      SmallVector<Requirement, 4> FilteredRequirements;
      filterGenericParams(Generics.getGenericParams(), FilteredParams,
                          SubMap);

      const auto *Self = dyn_cast<NominalTypeDecl>(D);
      if (!Self) {
        Self = D->getDeclContext()->getSelfNominalTypeDecl();
      }

      filterGenericRequirements(Generics.getRequirements(), Self,
                                FilteredRequirements, SubMap, FilteredParams);

      if (FilteredParams.empty() && FilteredRequirements.empty()) {
        return;
      }

      OS.attributeObject("swiftGenerics", [&](){
        if (!FilteredParams.empty()) {
          OS.attributeArray("parameters", [&](){
            for (const auto *Param : FilteredParams) {
              ::serialize(Param, OS);
            }
          }); // end parameters:
        }

        if (!FilteredRequirements.empty()) {
          OS.attributeArray("constraints", [&](){
            for (const auto &Req : FilteredRequirements) {
              ::serialize(Req, OS);
            }
          }); // end constraints:
        }
      }); // end swiftGenerics:
    }
  }
}

void Symbol::serializeSwiftExtensionMixin(llvm::json::OStream &OS) const {
  if (const auto *ED = dyn_cast<ExtensionDecl>(D)) {
    ::serialize(ED, OS);
  } else if (const auto *VD = dyn_cast<ValueDecl>(D)) {
    if (const auto *Extension =
            dyn_cast_or_null<ExtensionDecl>(VD->getDeclContext())) {
      ::serialize(Extension, OS);
    }
  }
}

void Symbol::serializeDeclarationFragmentMixin(llvm::json::OStream &OS) const {
  Graph->serializeDeclarationFragments("declarationFragments", *this, OS);
}

void Symbol::serializeAccessLevelMixin(llvm::json::OStream &OS) const {
  if (const auto *ED = dyn_cast<ExtensionDecl>(D)) {
    OS.attribute("accessLevel",
                 getAccessLevelSpelling(getEffectiveAccessLevel(ED)));
  } else if (const auto *VD = dyn_cast<ValueDecl>(D)) {
    OS.attribute("accessLevel", getAccessLevelSpelling(VD->getFormalAccess()));
  }
}

void Symbol::serializeMetadataMixin(llvm::json::OStream &OS) const {
  StringRef Category = documentationMetadataForDecl(D);
  if (!Category.empty())
    OS.attribute("metadata", Category);
}

void Symbol::serializeLocationMixin(llvm::json::OStream &OS) const {
  if (const auto *VD = dyn_cast<ValueDecl>(D)) {
    if (ClangNode ClangN = VD->getClangNode()) {
      if (!Graph->Walker.Options.IncludeClangDocs)
        return;

      if (auto *ClangD = ClangN.getAsDecl()) {
        StringRef FileName = getFileNameForDecl(ClangD);
        if (!FileName.empty()) {
          OS.attributeObject("location", [&](){
            // TODO: We should use a common function to fill in the location
            // information for both cursor info and symbol graph gen, then also
            // include position here.
            serializeFileURI(OS, FileName);
          });
        }
      }

      return;
    }
  }
    
  auto FileName = getFileNameForDecl(D);
  if (FileName.empty()) {
    return;
  }
  // TODO: Fold serializePosition into serializeFileURI so we don't need to load Loc twice?
  auto Loc = D->getLoc(/*SerializedOK=*/true);
  if (Loc.isInvalid()) {
    return;
  }
  OS.attributeObject("location", [&](){
    serializeFileURI(OS, FileName);
    serializePosition("position", Loc, Graph->M.getASTContext().SourceMgr, OS);
  });
}

namespace {
/// Get the availabilities for each domain on a declaration without walking
/// up the parent hierarchy.
///
/// \param D The declaration whose availabilities the method will collect.
/// \param Availabilities The domain -> availability map that will be updated.
/// \param IsParent If \c true\c, will update or fill availabilities for a given
/// domain with different "inheriting" rules rather than filling from
/// duplicate \c \@available attributes on the same declaration.
void getAvailabilities(const Decl *D,
                       llvm::StringMap<Availability> &Availabilities,
                       bool IsParent) {
  // DeclAttributes is a linked list in reverse order from where they
  // appeared in the source. Let's re-reverse them.
  SmallVector<const AvailableAttr *, 4> AvAttrs;
  for (const auto *Attr : D->getAttrs()) {
    if (const auto *AvAttr = dyn_cast<AvailableAttr>(Attr)) {
      AvAttrs.push_back(AvAttr);
    }
  }
  std::reverse(AvAttrs.begin(), AvAttrs.end());

  // Now go through them in source order.
  for (auto *AvAttr : AvAttrs) {
    Availability NewAvailability(*AvAttr);
    if (NewAvailability.empty()) {
      continue;
    }
    auto ExistingAvailability = Availabilities.find(NewAvailability.Domain);
    if (ExistingAvailability != Availabilities.end()) {
      // There are different rules for filling in missing components
      // or replacing existing components from a parent's @available
      // attribute compared to duplicate @available attributes on the
      // same declaration.
      // See the respective methods below for an explanation for the
      // replacement/filling rules.
      if (IsParent) {
        ExistingAvailability->getValue().updateFromParent(NewAvailability);
      } else {
        ExistingAvailability->getValue().updateFromDuplicate(NewAvailability);
      }
    } else {
      // There are no availabilities for this domain yet, so either
      // inherit the parent's in its entirety or set it from this declaration.
      Availabilities.insert(std::make_pair(NewAvailability.Domain,
                                           NewAvailability));
    }
  }
}

/// Get the availabilities of a declaration, considering all of its
/// parent context's except for the module.
void getInheritedAvailabilities(const Decl *D,
llvm::StringMap<Availability> &Availabilities) {
  getAvailabilities(D, Availabilities, /*IsParent*/false);

  auto CurrentContext = D->getDeclContext();
  while (CurrentContext) {
    if (const auto *Parent = CurrentContext->getAsDecl()) {
      if (isa<ModuleDecl>(Parent)) {
        return;
      }
      getAvailabilities(Parent, Availabilities, /*IsParent*/true);
    }
    CurrentContext = CurrentContext->getParent();
  }
}

} // end anonymous namespace

void Symbol::serializeAvailabilityMixin(llvm::json::OStream &OS) const {
  llvm::StringMap<Availability> Availabilities;
  getInheritedAvailabilities(D, Availabilities);

  if (Availabilities.empty()) {
    return;
  }

  OS.attributeArray("availability", [&]{
    for (const auto &Availability : Availabilities) {
      Availability.getValue().serialize(OS);
    }
  });
}

void Symbol::serializeSPIMixin(llvm::json::OStream &OS) const {
  if (D->isSPI())
    OS.attribute("spi", true);
}

void Symbol::serialize(llvm::json::OStream &OS) const {
  OS.object([&](){
    serializeKind(OS);
    serializeIdentifier(OS);
    serializePathComponents(OS);
    serializeNames(OS);
    serializeDocComment(OS);

    // "Mixins"
    serializeFunctionSignature(OS);
    serializeSwiftGenericMixin(OS);
    serializeSwiftExtensionMixin(OS);
    serializeDeclarationFragmentMixin(OS);
    serializeAccessLevelMixin(OS);
    serializeAvailabilityMixin(OS);
    serializeMetadataMixin(OS);
    serializeLocationMixin(OS);
    serializeSPIMixin(OS);
  });
}

swift::DeclName Symbol::getName(const Decl *D) const {
  if (const auto *ED = dyn_cast<ExtensionDecl>(D)) {
    return ED->getExtendedNominal()->getName();
  } else {
    return cast<ValueDecl>(D)->getName();
  }
}

const ValueDecl *Symbol::getSymbolDecl() const {
  if (const auto *ED = dyn_cast<ExtensionDecl>(D)) {
    return ED->getExtendedNominal();
  } else {
    return cast<ValueDecl>(D);
  }
}

void
Symbol::getPathComponents(SmallVectorImpl<PathComponent> &Components) const {
  const ValueDecl *Decl = nullptr;
  if (const auto *ED = dyn_cast<ExtensionDecl>(D)) {
    Decl = ED->getExtendedNominal();
  } else if (const auto *VD = dyn_cast<ValueDecl>(D)) {
    Decl = VD;
  }

  // Note: this is also used for sourcekit's cursor-info request, so can be
  // called on local symbols too. For such symbols, the path contains all parent
  // decl contexts that are currently representable in the symbol graph,
  // skipping over the rest (e.g. containing closures and accessors).

  auto collectPathComponents =
      [&](const ValueDecl *Decl,
          SmallVectorImpl<PathComponent> &DeclComponents) {
        // Collect the spellings, kinds, and decls of the fully qualified
        // identifier components.
        while (Decl && !isa<ModuleDecl>(Decl)) {
          SmallString<32> Scratch;
          getName(Decl).getString(Scratch);

          if (supportsKind(Decl->getKind()))
            DeclComponents.push_back({Scratch, getKind(Decl).first, Decl});

          // Find the next parent.
          auto *DC = Decl->getDeclContext();
          while (DC &&
                 DC->getContextKind() == DeclContextKind::AbstractClosureExpr)
            DC = DC->getParent();
          if (DC) {
            if (const auto *Nominal = DC->getSelfNominalTypeDecl()) {
              Decl = Nominal;
            } else {
              Decl = dyn_cast_or_null<ValueDecl>(DC->getAsDecl());
            }
          } else {
            Decl = nullptr;
          }
        }
      };

  if (const auto BaseTypeDecl = getSynthesizedBaseTypeDecl()) {
    // This is a synthesized member of some base type declaration, actually
    // existing on another type, such as a default implementation of
    // a protocol. Build a path as if it were defined in the base type.
    SmallString<32> LastPathComponent;
    getName(Decl).getString(LastPathComponent);
    if (supportsKind(Decl->getKind()))
      Components.push_back({LastPathComponent, getKind(Decl).first, Decl});
    collectPathComponents(BaseTypeDecl, Components);
  } else {
    // Otherwise, this is just a normal declaration, so we can build
    // its path normally.
    collectPathComponents(Decl, Components);
  }

  // The list is leaf-to-root, but we want root-to-leaf, so reverse it.
  std::reverse(Components.begin(), Components.end());
}

void Symbol::
getFragmentInfo(SmallVectorImpl<FragmentInfo> &FragmentInfos) const {
  SmallPtrSet<const Decl*, 8> Referenced;

  auto Options = Graph->getDeclarationFragmentsPrintOptions();
  if (getBaseType()) {
    Options.setBaseType(getBaseType());
    Options.PrintAsMember = true;
  }

  llvm::json::OStream OS(llvm::nulls());
  OS.object([&]{
    DeclarationFragmentPrinter Printer(Graph, OS, {"ignored"}, &Referenced);
    getSymbolDecl()->print(Printer, Options);
  });

  for (auto *D: Referenced) {
    if (!Symbol::supportsKind(D->getKind()))
      continue;
    if (auto *VD = dyn_cast<ValueDecl>(D)) {
      FragmentInfos.push_back(FragmentInfo{VD, {}});
      Symbol RefSym(Graph, VD, nullptr);
      RefSym.getPathComponents(FragmentInfos.back().ParentContexts);
    }
  }
}

void Symbol::printPath(llvm::raw_ostream &OS) const {
  SmallVector<PathComponent, 8> Components;
  getPathComponents(Components);
  for (auto it = Components.begin(); it != Components.end(); ++it) {
    if (it != Components.begin()) {
      OS << '.';
    }
    OS << it->Title.str();
  }
}

void Symbol::getUSR(SmallVectorImpl<char> &USR) const {
  llvm::raw_svector_ostream OS(USR);
  ide::printDeclUSR(D, OS);
  if (SynthesizedBaseTypeDecl) {
    OS << "::SYNTHESIZED::";
    ide::printDeclUSR(SynthesizedBaseTypeDecl, OS);
  }
}

bool Symbol::supportsKind(DeclKind Kind) {
  switch (Kind) {
  case DeclKind::Class: LLVM_FALLTHROUGH;
  case DeclKind::Struct: LLVM_FALLTHROUGH;
  case DeclKind::Enum: LLVM_FALLTHROUGH;
  case DeclKind::EnumElement: LLVM_FALLTHROUGH;
  case DeclKind::Protocol: LLVM_FALLTHROUGH;
  case DeclKind::Constructor: LLVM_FALLTHROUGH;
  case DeclKind::Destructor: LLVM_FALLTHROUGH;
  case DeclKind::Func: LLVM_FALLTHROUGH;
  case DeclKind::Var: LLVM_FALLTHROUGH;
  case DeclKind::Param: LLVM_FALLTHROUGH;
  case DeclKind::Subscript: LLVM_FALLTHROUGH;
  case DeclKind::TypeAlias: LLVM_FALLTHROUGH;
  case DeclKind::AssociatedType: LLVM_FALLTHROUGH;
  case DeclKind::Extension: LLVM_FALLTHROUGH;
  case DeclKind::Macro:
    return true;
  default:
    return false;
  }
}

AccessLevel Symbol::getEffectiveAccessLevel(const ExtensionDecl *ED) {
  AccessLevel maxPropertyAL = AccessLevel::Private;
  for (auto Member : ED->getMembers()) {
    if (const auto *VMember = dyn_cast<ValueDecl>(Member)) {
      maxPropertyAL = std::max(maxPropertyAL, VMember->getFormalAccess());
    }
  }

  AccessLevel maxInheritedAL = AccessLevel::Private;
  for (auto Inherited : ED->getInherited()) {
    if (const auto Type = Inherited.getType()) {
      if (const auto *Proto = dyn_cast_or_null<ProtocolDecl>(
              Type->getAnyNominal())) {
        maxInheritedAL = std::max(maxInheritedAL, Proto->getFormalAccess());
      }
    }
  }

  return std::min(ED->getExtendedNominal()->getFormalAccess(),
                  std::max(maxPropertyAL, maxInheritedAL));
}
