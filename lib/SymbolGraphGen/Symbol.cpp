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
#include "swift/AST/Module.h"
#include "swift/AST/ParameterList.h"
#include "swift/Basic/SourceManager.h"
#include "JSON.h"
#include "Symbol.h"
#include "SymbolGraph.h"
#include "SymbolGraphASTWalker.h"

using namespace swift;
using namespace symbolgraphgen;

void Symbol::serializeKind(StringRef Identifier, StringRef DisplayName,
                           llvm::json::OStream &OS) const {
  OS.object([&](){
    OS.attribute("identifier", Identifier);
    OS.attribute("displayName", DisplayName);
  });
}

void Symbol::serializeKind(llvm::json::OStream &OS) const {
  AttributeRAII A("kind", OS);
  switch (VD->getKind()) {
  case swift::DeclKind::Class:
    serializeKind("swift.class", "Class", OS);
    break;
  case swift::DeclKind::Struct:
    serializeKind("swift.struct", "Structure", OS);
    break;
  case swift::DeclKind::Enum:
    serializeKind("swift.enum", "Enumeration", OS);
    break;
  case swift::DeclKind::EnumElement:
    serializeKind("swift.enum.case", "Case", OS);
    break;
  case swift::DeclKind::Protocol:
    serializeKind("swift.protocol", "Protocol", OS);
    break;
  case swift::DeclKind::Constructor:
    serializeKind("swift.init", "Initializer", OS);
    break;
  case swift::DeclKind::Func:
    if (VD->isOperator()) {
      serializeKind("swift.func.op", "Operator", OS);
    } else if (VD->isStatic()) {
      serializeKind("swift.type.method", "Type Method", OS);
    } else if (VD->getDeclContext()->getSelfNominalTypeDecl()){
      serializeKind("swift.method", "Instance Method", OS);
    } else {
      serializeKind("swift.func", "Function", OS);
    }
    break;
  case swift::DeclKind::Var:
    if (VD->isStatic()) {
      serializeKind("swift.type.property", "Type Property", OS);
    } else if (VD->getDeclContext()->getSelfNominalTypeDecl()) {
      serializeKind("swift.property", "Instance Property", OS);
    } else {
      serializeKind("swift.var", "Global Variable", OS);
    }
    break;
  case swift::DeclKind::Subscript:
    if (VD->isStatic()) {
      serializeKind("swift.type.subscript", "Type Subscript", OS);
    } else {
      serializeKind("swift.subscript", "Instance Subscript", OS);
    }
    break;
  case swift::DeclKind::TypeAlias:
    serializeKind("swift.typealias", "Type Alias", OS);
    break;
  case swift::DeclKind::AssociatedType:
    serializeKind("swift.associatedtype", "Associated Type", OS);
    break;
  default:
    llvm_unreachable("Unsupported declaration kind for symbol graph");
  }
}

void Symbol::serializeIdentifier(llvm::json::OStream &OS) const {
  OS.attributeObject("identifier", [&](){
    OS.attribute("precise", Graph.getUSR(VD));
    OS.attribute("interfaceLanguage", "swift");
  });
}

void Symbol::serializePathComponents(llvm::json::OStream &OS) const {
  OS.attributeArray("pathComponents", [&](){
    SmallVector<SmallString<32>, 8> PathComponents;
    Graph.getPathComponents(VD, PathComponents);
    for (auto Component : PathComponents) {
      OS.value(Component);
    }
  });
}

void Symbol::serializeNames(llvm::json::OStream &OS) const {
  OS.attributeObject("names", [&](){
    SmallVector<SmallString<32>, 8> PathComponents;
    Graph.getPathComponents(VD, PathComponents);
    
    OS.attribute("title", PathComponents.back());
    // "navigator": null
    Graph.serializeSubheadingDeclarationFragments("subheading", VD, OS);
    // "prose": null
  });
}

void Symbol::serializePosition(StringRef Key, SourceLoc Loc,
                               SourceManager &SourceMgr,
                               llvm::json::OStream &OS) const {
  // Note: Line and columns are zero-based in this serialized format.
  auto LineAndColumn = SourceMgr.getLineAndColumn(Loc);
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

void Symbol::serializeDocComment(llvm::json::OStream &OS) const {
  OS.attributeObject("docComment", [&](){
    auto LL = Graph.Ctx.getLineList(VD->getRawComment(/*SerializedOK=*/true));
    size_t InitialIndentation = LL.getLines().empty()
      ? 0
      : markup::measureIndentation(LL.getLines().front().Text);
    OS.attributeArray("lines", [&](){
      for (const auto &Line : LL.getLines()) {
        // Line object
        OS.object([&](){
          // Trim off any initial indentation from the line's
          // text and start of its source range, if it has one.
          if (Line.Range.isValid()) {
            serializeRange(InitialIndentation,
                           Line.Range, Graph.M.getASTContext().SourceMgr, OS);
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
  if (const auto *FD = dyn_cast_or_null<FuncDecl>(VD)) {
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
                Graph.serializeDeclarationFragments("declarationFragments",
                                                     Param, OS);
              }); // end parameter object
            }
          }); // end parameters:
        }
      }

      // Returns
      if (const auto ReturnType = FD->getResultInterfaceType()) {
        Graph.serializeDeclarationFragments("returns", ReturnType, OS);
      }
    });
  }
}

void Symbol::serializeGenericParam(const swift::GenericTypeParamType &Param,
                                   llvm::json::OStream &OS) const {
  OS.object([&](){
    OS.attribute("name", Param.getName().str());
    OS.attribute("index", Param.getIndex());
    OS.attribute("depth", Param.getDepth());
  });
}

void Symbol::serializeGenericRequirement(const swift::Requirement &Req,
                                         llvm::json::OStream &OS) const {
  OS.object([&](){
    switch (Req.getKind()) {
      case swift::RequirementKind::Conformance:
        OS.attribute("kind", "conformance");
        break;
      case swift::RequirementKind::Superclass:
        OS.attribute("kind", "superclass");
        break;
      case swift::RequirementKind::SameType:
        OS.attribute("kind", "sameType");
        break;
      case swift::RequirementKind::Layout:
        return;
    }
    OS.attribute("lhs", Req.getFirstType()->getString());
    OS.attribute("rhs", Req.getSecondType()->getString());
  });
}

void Symbol::serializeSwiftGenericMixin(llvm::json::OStream &OS) const {
  if (const auto *GC = VD->getAsGenericContext()) {
      if (const auto Generics = GC->getGenericSignature()) {

      OS.attributeObject("swiftGenerics", [&](){
        if (!Generics->getGenericParams().empty()) {
          OS.attributeArray("parameters", [&](){
            for (const auto Param : Generics->getGenericParams()) {
              if (const auto *D = Param->getDecl()) {
                if (D->isImplicit()) {
                  continue;
                }
              }
              serializeGenericParam(*Param, OS);
            }
          }); // end parameters:
        }

        if (!Generics->getRequirements().empty()) {
          OS.attributeArray("constraints", [&](){
            for (const auto &Requirement : Generics->getRequirements()) {
              serializeGenericRequirement(Requirement, OS);
            }
          }); // end constraints:
        }

      }); // end swiftGenerics:
    }
  }
}

void Symbol::serializeSwiftExtensionMixin(llvm::json::OStream &OS) const {
  if (const auto *Extension
          = dyn_cast_or_null<ExtensionDecl>(VD->getInnermostDeclContext())) {
    OS.attributeObject("swiftExtension", [&](){
      if (const auto *ExtendedNominal = Extension->getExtendedNominal()) {
        if (const auto *ExtendedModule = ExtendedNominal->getModuleContext()) {
          OS.attribute("extendedModule", ExtendedModule->getNameStr());
        }
      }
      auto Generics = Extension->getGenericSignature();
      if (Generics && !Generics->getRequirements().empty()) {
        OS.attributeArray("constraints", [&](){
          for (const auto &Requirement : Generics->getRequirements()) {
            serializeGenericRequirement(Requirement, OS);
          }
        }); // end constraints:
      }
    }); // end swiftExtension:
  }
}

void Symbol::serializeDeclarationFragmentMixin(llvm::json::OStream &OS) const {
  Graph.serializeDeclarationFragments("declarationFragments", VD, OS);
}

void Symbol::serializeAccessLevelMixin(llvm::json::OStream &OS) const {
  OS.attribute("accessLevel", getAccessLevelSpelling(VD->getFormalAccess()));
}

void Symbol::serializeLocationMixin(llvm::json::OStream &OS) const {
  auto Loc = VD->getLoc(/*SerializedOK=*/true);
  if (Loc.isInvalid()) {
    return;
  }
  auto FileName = VD->getASTContext().SourceMgr.getDisplayNameForLoc(Loc);
  OS.attributeObject("location", [&](){
    if (!FileName.empty()) {
      SmallString<1024> FileURI("file://");
      FileURI.append(FileName);
      OS.attribute("uri", FileURI.str());
    }
    serializePosition("position", Loc, Graph.M.getASTContext().SourceMgr, OS);
  });
}

llvm::Optional<StringRef>
Symbol::getDomain(PlatformAgnosticAvailabilityKind AgnosticKind,
                  PlatformKind Kind) const {
  switch (AgnosticKind) {
    // SPM- and Swift-specific availability.
    case PlatformAgnosticAvailabilityKind::PackageDescriptionVersionSpecific:
      return { "SwiftPM" };
    case PlatformAgnosticAvailabilityKind::SwiftVersionSpecific:
    case PlatformAgnosticAvailabilityKind::UnavailableInSwift:
      return { "Swift" };
    // Although these are in the agnostic kinds, they are actually a signal
    // that there is either platform-specific or completely platform-agnostic.
    // They'll be handled below.
    case PlatformAgnosticAvailabilityKind::Deprecated:
    case PlatformAgnosticAvailabilityKind::Unavailable:
    case PlatformAgnosticAvailabilityKind::None:
      break;
  }

  // Platform-specific availability.
  switch (Kind) {
    case swift::PlatformKind::iOS:
      return { "iOS" };
    case swift::PlatformKind::macCatalyst:
      return { "macCatalyst" };
    case swift::PlatformKind::OSX:
      return { "macOS" };
    case swift::PlatformKind::tvOS:
      return { "tvOS" };
    case swift::PlatformKind::watchOS:
      return { "watchOS" };
    case swift::PlatformKind::iOSApplicationExtension:
      return { "iOSAppExtension" };
    case swift::PlatformKind::macCatalystApplicationExtension:
      return { "macCatalystAppExtension" };
    case swift::PlatformKind::OSXApplicationExtension:
      return { "macOSAppExtension" };
    case swift::PlatformKind::tvOSApplicationExtension:
      return { "tvOSAppExtension" };
    case swift::PlatformKind::watchOSApplicationExtension:
      return { "watchOSAppExtension" };
    // Platform-agnostic availability, such as "unconditionally deprecated"
    // or "unconditionally obsoleted".
    case swift::PlatformKind::none:
      return None;
  }
}

void Symbol::serializeAvailabilityMixin(llvm::json::OStream &OS) const {
  SmallVector<const AvailableAttr *, 4> Availabilities;
  for (const auto *Attr : VD->getAttrs()) {
    if (const auto *AvAttr = dyn_cast<AvailableAttr>(Attr)) {
      Availabilities.push_back(AvAttr);
    }
  }
  if (Availabilities.empty()) {
    return;
  }

  OS.attributeArray("availability", [&](){
    for (const auto *AvAttr : Availabilities) {
      OS.object([&](){
        auto Domain = getDomain(AvAttr->getPlatformAgnosticAvailability(),
                                AvAttr->Platform);
        if (Domain) {
          OS.attribute("domain", *Domain);
        }
        if (AvAttr->Introduced) {
          AttributeRAII Introduced("introduced", OS);
          symbolgraphgen::serialize(*AvAttr->Introduced, OS);
        }
        if (AvAttr->Deprecated) {
          AttributeRAII Deprecated("deprecated", OS);
          symbolgraphgen::serialize(*AvAttr->Deprecated, OS);
        }
        if (AvAttr->Obsoleted) {
          AttributeRAII Obsoleted("obsoleted", OS);
          symbolgraphgen::serialize(*AvAttr->Obsoleted, OS);
        }
        if (!AvAttr->Message.empty()) {
          OS.attribute("message", AvAttr->Message);
        }
        if (!AvAttr->Rename.empty()) {
          OS.attribute("renamed", AvAttr->Rename);
        }
        if (AvAttr->isUnconditionallyDeprecated()) {
          OS.attribute("isUnconditionallyDeprecated", true);
        }
        if (AvAttr->isUnconditionallyUnavailable()) {
          OS.attribute("isUnconditionallyUnavailable", true);
        }
      }); // end availability object
    }
  }); // end availability: []
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
    serializeLocationMixin(OS);
  });
}
