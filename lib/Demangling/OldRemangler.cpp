//===--- OldRemangler.cpp - Old Swift Re-mangler --------------------------===//
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
//  This file implements the remangler, which turns a demangling parse
//  tree back into a mangled string.  This is useful for tools which
//  want to extract subtrees from mangled strings.
//
//===----------------------------------------------------------------------===//

#include "DemanglerAssert.h"
#include "RemanglerBase.h"
#include "swift/AST/Ownership.h"
#include "swift/Demangling/Demangler.h"
#include "swift/Demangling/ManglingUtils.h"
#include "swift/Demangling/Punycode.h"
#include "swift/Strings.h"
#include <cstdio>
#include <cstdlib>

using namespace swift;
using namespace Demangle;

namespace {
  class Remangler : public RemanglerBase {
    static const unsigned MaxDepth = 1024;

  public:
    Remangler(NodeFactory &Factory) : RemanglerBase(Factory) { }

    class EntityContext {
      bool AsContext = false;
      std::string AnonymousContextDiscriminator;
    public:
      bool isAsContext() const {
        return AsContext;
      }
      
      void setAnonymousContextDiscriminator(StringRef discriminator) {
        AnonymousContextDiscriminator = discriminator.str();
      }
      
      std::string takeAnonymousContextDiscriminator() {
        auto r = std::move(AnonymousContextDiscriminator);
        AnonymousContextDiscriminator.clear();
        return r;
      }

      class ManglingContextRAII {
        EntityContext &Ctx;
        bool SavedValue;
      public:
        ManglingContextRAII(EntityContext &ctx)
          : Ctx(ctx), SavedValue(ctx.AsContext) {
          ctx.AsContext = true;
        }

        ~ManglingContextRAII() {
          Ctx.AsContext = SavedValue;
        }
      };
    };

    ManglingError mangle(Node *node, unsigned depth) {
      if (depth > Remangler::MaxDepth) {
        return MANGLING_ERROR(ManglingError::TooComplex, node);
      }

      switch (node->getKind()) {
#define NODE(ID)                                                               \
  case Node::Kind::ID:                                                         \
    return mangle##ID(node, depth);
#include "swift/Demangling/DemangleNodes.def"
      }
      return MANGLING_ERROR(ManglingError::BadNodeKind, node);
    }

    ManglingError mangleGenericArgs(Node *node, EntityContext &ctx,
                                    unsigned depth);
    ManglingError mangleAnyNominalType(Node *node, EntityContext &ctx,
                                       unsigned depth);

#define NODE(ID) ManglingError mangle##ID(Node *node, unsigned depth);
#define CONTEXT_NODE(ID)                                                       \
  ManglingError mangle##ID(Node *node, unsigned depth);                        \
  ManglingError mangle##ID(Node *node, EntityContext &ctx, unsigned depth);
#include "swift/Demangling/DemangleNodes.def"

    void mangleIndex(Node::IndexType index);
    ManglingError mangleIdentifier(StringRef name, OperatorKind operatorKind);
    ManglingError mangleAccessor(Node *storageNode, StringRef accessorCode,
                                 EntityContext &ctx, unsigned depth);

    ManglingError mangleChildNodes(Node *node, unsigned depth) {
      return mangleNodes(node->begin(), node->end(), depth);
    }
    ManglingError mangleNodes(Node::iterator i, Node::iterator e,
                              unsigned depth) {
      for (; i != e; ++i) {
        RETURN_IF_ERROR(mangle(*i, depth));
      }
      return ManglingError::Success;
    }
    ManglingError mangleSingleChildNode(Node *node, unsigned depth) {
      if (node->getNumChildren() != 1)
        return MANGLING_ERROR(ManglingError::MultipleChildNodes, node);

      return mangle(*node->begin(), depth);
    }
    ManglingError mangleChildNode(Node *node, unsigned index, unsigned depth) {
      DEMANGLER_ASSERT(index < node->getNumChildren(), node);
      return mangle(node->begin()[index], depth);
    }

    ManglingError mangleSimpleEntity(Node *node, char basicKind,
                                     StringRef entityKind, EntityContext &ctx,
                                     unsigned depth);
    ManglingError
    mangleNamedEntity(Node *node, char basicKind, StringRef entityKind,
                      EntityContext &ctx, unsigned depth,
                      StringRef ArtificialPrivateDiscriminator = {});
    ManglingError mangleTypedEntity(Node *node, char basicKind,
                                    StringRef entityKind, EntityContext &ctx,
                                    unsigned depth);
    ManglingError mangleNamedAndTypedEntity(Node *node, char basicKind,
                                            StringRef entityKind,
                                            EntityContext &ctx, unsigned depth);
    ManglingError
    mangleNominalType(Node *node, char basicKind, EntityContext &ctx,
                      unsigned depth,
                      StringRef ArtificialPrivateDiscriminator = {});

    ManglingError mangleProtocolWithoutPrefix(Node *node, unsigned depth);
    ManglingError
    mangleProtocolListWithoutPrefix(Node *node, unsigned depth,
                                    Node *additionalProto = nullptr);

    ManglingError mangleEntityContext(Node *node, EntityContext &ctx,
                                      unsigned depth);
    ManglingError mangleEntityType(Node *node, EntityContext &ctx,
                                   unsigned depth);
    ManglingError mangleEntityGenericType(Node *node, EntityContext &ctx);

    bool trySubstitution(Node *node, SubstitutionEntry &entry);
    bool mangleStandardSubstitution(Node *node);

    ManglingError mangleDependentGenericParamIndex(Node *node, unsigned depth);
    ManglingError mangleConstrainedType(Node *node, unsigned depth);
  };
} // end anonymous namespace

#define NODE(ID)
#define CONTEXT_NODE(ID)                                                       \
  ManglingError Remangler::mangle##ID(Node *node, unsigned depth) {            \
    EntityContext ctx;                                                         \
    return mangle##ID(node, ctx, depth);                                       \
  }
#include "swift/Demangling/DemangleNodes.def"

/// Re-apply labels from the function to its parameter type
/// to preserve old mangling style.
///
/// \param LabelList The list of labels to apply.
/// \param OrigType  The function parameter type to apply labels to.
/// \param Factory   The node factory to use to allocate new nodes.
static NodePointer applyParamLabels(NodePointer LabelList, NodePointer OrigType,
                                    NodeFactory &Factory) {
  if (LabelList->getNumChildren() == 0)
    return OrigType;

  auto applyParamLabels = [&](NodePointer ArgTuple) -> NodePointer {
    assert(ArgTuple->getKind() == Node::Kind::ArgumentTuple);

    auto ParamsType = Factory.createNode(Node::Kind::ArgumentTuple);
    auto Tuple = Factory.createNode(Node::Kind::Tuple);

    auto processParameter = [&](NodePointer Label, NodePointer Param) {
      if (Label->getKind() == Node::Kind::FirstElementMarker) {
        Tuple->addChild(Param, Factory);
        return;
      }

      auto NewParam = Factory.createNode(Node::Kind::TupleElement);
      NewParam->addChild(Factory.createNodeWithAllocatedText(
                           Node::Kind::TupleElementName, Label->getText()),
                         Factory);

      for (auto &Child : *Param)
        NewParam->addChild(Child, Factory);

      Tuple->addChild(NewParam, Factory);
    };

    auto OrigTuple = ArgTuple->getFirstChild()->getFirstChild();

    if (OrigTuple->getKind() != Node::Kind::Tuple) {
      processParameter(LabelList->getChild(0), OrigTuple);
    } else {
      for (unsigned i = 0, n = OrigTuple->getNumChildren(); i != n; ++i) {
        processParameter(LabelList->getChild(i), OrigTuple->getChild(i));
      }
    }

    auto Type = Factory.createNode(Node::Kind::Type);
    Type->addChild(Tuple, Factory);
    ParamsType->addChild(Type, Factory);
    return ParamsType;
  };

  auto visitTypeChild = [&](NodePointer Child) -> NodePointer {
    if (Child->getKind() != Node::Kind::FunctionType &&
        Child->getKind() != Node::Kind::NoEscapeFunctionType)
      return Child;

    auto FuncType = Factory.createNode(Node::Kind::FunctionType);
    for (unsigned i = 0, n = Child->getNumChildren(); i != n; ++i) {
      NodePointer FuncChild = Child->getChild(i);
      if (FuncChild->getKind() == Node::Kind::ArgumentTuple)
        FuncChild = applyParamLabels(FuncChild);
      FuncType->addChild(FuncChild, Factory);
    }
    return FuncType;
  };

  auto Type = Factory.createNode(OrigType->getKind());
  for (auto &Child : *OrigType)
    Type->addChild(visitTypeChild(Child), Factory);

  return Type;
}

bool Remangler::trySubstitution(Node *node, SubstitutionEntry &entry) {
  if (mangleStandardSubstitution(node))
    return true;

  // Go ahead and initialize the substitution entry.
  entry.setNode(node, /*treatAsIdentifier=*/ false);

  int Idx = findSubstitution(entry);
  if (Idx < 0)
    return false;

  Buffer << 'S';
  mangleIndex(Idx);
  return true;
}

static bool isInSwiftModule(Node *node) {
  Node *context = node->getFirstChild();
  return (context->getKind() == Node::Kind::Module &&
          context->getText() == STDLIB_NAME &&
          // Check for private declarations in Swift
          node->getChild(1)->getKind() == Node::Kind::Identifier);
}

bool Remangler::mangleStandardSubstitution(Node *node) {
  // Look for known substitutions.
  switch (node->getKind()) {
#define SUCCESS_IF_IS(VALUE, EXPECTED, SUBSTITUTION)            \
    do {                                                        \
      if ((VALUE) == (EXPECTED)) {                              \
        Buffer << SUBSTITUTION;                                    \
        return true;                                            \
      }                                                         \
    } while (0)
#define SUCCESS_IF_TEXT_IS(EXPECTED, SUBSTITUTION)              \
    SUCCESS_IF_IS(node->getText(), EXPECTED, SUBSTITUTION)
#define SUCCESS_IF_DECLNAME_IS(EXPECTED, SUBSTITUTION)          \
    SUCCESS_IF_IS(node->getChild(1)->getText(), EXPECTED, SUBSTITUTION)

    case Node::Kind::Module:
      SUCCESS_IF_TEXT_IS(STDLIB_NAME, "s");
      SUCCESS_IF_TEXT_IS(MANGLING_MODULE_OBJC, "So");
      SUCCESS_IF_TEXT_IS(MANGLING_MODULE_CLANG_IMPORTER, "SC");
      break;
    case Node::Kind::Structure:
      if (isInSwiftModule(node)) {
        SUCCESS_IF_DECLNAME_IS("Array", "Sa");
        SUCCESS_IF_DECLNAME_IS("Bool", "Sb");
        SUCCESS_IF_DECLNAME_IS("UnicodeScalar", "Sc");
        SUCCESS_IF_DECLNAME_IS("Double", "Sd");
        SUCCESS_IF_DECLNAME_IS("Float", "Sf");
        SUCCESS_IF_DECLNAME_IS("Int", "Si");
        SUCCESS_IF_DECLNAME_IS("UnsafeRawPointer", "SV");
        SUCCESS_IF_DECLNAME_IS("UnsafeMutableRawPointer", "Sv");
        SUCCESS_IF_DECLNAME_IS("UnsafePointer", "SP");
        SUCCESS_IF_DECLNAME_IS("UnsafeMutablePointer", "Sp");
        SUCCESS_IF_DECLNAME_IS("UnsafeBufferPointer", "SR");
        SUCCESS_IF_DECLNAME_IS("UnsafeMutableBufferPointer", "Sr");
        SUCCESS_IF_DECLNAME_IS("String", "SS");
        SUCCESS_IF_DECLNAME_IS("UInt", "Su");
      }
      break;
    case Node::Kind::Enum:
      if (isInSwiftModule(node)) {
        SUCCESS_IF_DECLNAME_IS("Optional", "Sq");
        SUCCESS_IF_DECLNAME_IS("ImplicitlyUnwrappedOptional", "SQ");
      }
      break;

    default:
      break;

#undef SUCCESS_IF_DECLNAME_IS
#undef SUCCESS_IF_TEXT_IS
#undef SUCCESS_IF_IS
  }
  return false;
}

ManglingError Remangler::mangleIdentifier(Node *node, unsigned depth) {
  return mangleIdentifier(node->getText(), OperatorKind::NotOperator);
}
ManglingError Remangler::manglePrefixOperator(Node *node, unsigned depth) {
  return mangleIdentifier(node->getText(), OperatorKind::Prefix);
}
ManglingError Remangler::manglePostfixOperator(Node *node, unsigned depth) {
  return mangleIdentifier(node->getText(), OperatorKind::Postfix);
}
ManglingError Remangler::mangleInfixOperator(Node *node, unsigned depth) {
  return mangleIdentifier(node->getText(), OperatorKind::Infix);
}
ManglingError Remangler::mangleIdentifier(StringRef ident,
                                          OperatorKind operatorKind) {
  // Mangle normal identifiers as
  //   count identifier-char+
  // where the count is the number of characters in the identifier,
  // and where individual identifier characters represent themselves.

  // Mangle operator identifiers as
  //   operator ::= 'o' operator-fixity count operator-char+
  //   operator-fixity ::= 'p' // prefix
  //   operator-fixity ::= 'P' // postfix
  //   operator-fixity ::= 'i' // infix
  // where the count is the number of characters in the operator,
  // and where the individual operator characters are translated.
  switch (operatorKind) {
  case OperatorKind::NotOperator:
    Buffer << ident.size() << ident;
    return ManglingError::Success;
  case OperatorKind::Infix:
    Buffer << "oi";
    break;
  case OperatorKind::Prefix:
    Buffer << "op";
    break;
  case OperatorKind::Postfix:
    Buffer << "oP";
    break;
  }

  // Mangle ASCII operators directly.
  Buffer << ident.size();
  for (char ch : ident) {
    Buffer << Mangle::translateOperatorChar(ch);
  }
  return ManglingError::Success;
}

ManglingError Remangler::mangleNumber(Node *node, unsigned depth) {
  mangleIndex(node->getIndex());
  return ManglingError::Success;
}

void Remangler::mangleIndex(Node::IndexType value) {
  if (value == 0) {
    Buffer << '_';
  } else {
    Buffer << (value - 1) << '_';
  }
}

ManglingError Remangler::mangleGlobal(Node *node, unsigned depth) {
  Buffer << "_T";
  return mangleChildNodes(node, depth + 1);
}

ManglingError Remangler::mangleSuffix(Node *node, unsigned depth) {
  // Just add the suffix back on.
  Buffer << node->getText();
  return ManglingError::Success;
}

ManglingError Remangler::mangleGenericSpecialization(Node *node,
                                                     unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError
Remangler::mangleGenericSpecializationPrespecialized(Node *node,
                                                     unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}
ManglingError
Remangler::mangleGenericSpecializationNotReAbstracted(Node *node,
                                                      unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}
ManglingError
Remangler::mangleGenericSpecializationInResilienceDomain(Node *node,
                                                         unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleInlinedGenericFunction(Node *node,
                                                      unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleGenericPartialSpecialization(Node *node,
                                                            unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError
Remangler::mangleGenericPartialSpecializationNotReAbstracted(Node *node,
                                                             unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleGenericSpecializationParam(Node *node,
                                                          unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleFunctionSignatureSpecialization(Node *node,
                                                               unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleSpecializationPassID(Node *node,
                                                    unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleIsSerialized(Node *node, unsigned depth) {
  Buffer << "q";
  return ManglingError::Success;
}

ManglingError Remangler::mangleMetatypeParamsRemoved(Node *node, unsigned depth) {
  Buffer << "m";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleFunctionSignatureSpecializationReturn(Node *node,
                                                       unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError
Remangler::mangleFunctionSignatureSpecializationParam(Node *node,
                                                      unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError
Remangler::mangleFunctionSignatureSpecializationParamPayload(Node *node,
                                                             unsigned depth) {
  // This should never be called since mangling parameter payloads require
  // knowing what the parameter kind is.
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError
Remangler::mangleFunctionSignatureSpecializationParamKind(Node *node,
                                                          unsigned depth) {
  // This should never be called since mangling parameter kinds have influence
  // on the payloads.
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleRetroactiveConformance(Node *node,
                                                      unsigned depth) {
  // Retroactive conformances aren't in the old mangling
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError
Remangler::mangleProtocolConformanceRefInTypeModule(Node *node,
                                                    unsigned depth) {
  // Protocol conformance references aren't in the old mangling
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError
Remangler::mangleProtocolConformanceRefInProtocolModule(Node *node,
                                                        unsigned depth) {
  // Protocol conformance references aren't in the old mangling
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError
Remangler::mangleProtocolConformanceRefInOtherModule(Node *node,
                                                     unsigned depth) {
  // Protocol conformance references aren't in the old mangling
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleConcreteProtocolConformance(Node *node,
                                                           unsigned depth) {
  // Concrete conformances aren't in the old mangling
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleAnyProtocolConformanceList(Node *node,
                                                          unsigned depth) {
  // Conformance lists aren't in the old mangling
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleDependentAssociatedConformance(Node *node,
                                                              unsigned depth) {
  // Dependent associated conformances aren't in the old mangling
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError
Remangler::mangleDependentProtocolConformanceRoot(Node *node, unsigned depth) {
  // Dependent conformances aren't in the old mangling
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError
Remangler::mangleDependentProtocolConformanceInherited(Node *node,
                                                       unsigned depth) {
  // Dependent conformances aren't in the old mangling
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError
Remangler::mangleDependentProtocolConformanceAssociated(Node *node,
                                                        unsigned depth) {
  // Dependent conformances aren't in the old mangling
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleProtocolConformance(Node *node, unsigned depth) {
  // type, protocol name, context
  DEMANGLER_ASSERT(node->getNumChildren() == 3, node);
  RETURN_IF_ERROR(mangleChildNode(node, 0, depth + 1));
  RETURN_IF_ERROR(mangleProtocolWithoutPrefix(node->begin()[1], depth + 1));
  return mangleChildNode(node, 2, depth + 1);
}

ManglingError Remangler::mangleObjCAttribute(Node *node, unsigned depth) {
  Buffer << "To";
  return ManglingError::Success;
}

ManglingError Remangler::mangleNonObjCAttribute(Node *node, unsigned depth) {
  Buffer << "TO";
  return ManglingError::Success;
}

ManglingError Remangler::mangleDirectMethodReferenceAttribute(Node *node,
                                                              unsigned depth) {
  Buffer << "Td";
  return ManglingError::Success;
}

ManglingError Remangler::mangleDynamicAttribute(Node *node, unsigned depth) {
  Buffer << "TD";
  return ManglingError::Success;
}

ManglingError Remangler::mangleVTableAttribute(Node *node, unsigned depth) {
  Buffer << "TV";
  return ManglingError::Success;
}

ManglingError Remangler::mangleGenericTypeMetadataPattern(Node *node,
                                                          unsigned depth) {
  Buffer << "MP";
  return mangleSingleChildNode(node, depth + 1); // type
}

ManglingError Remangler::mangleTypeMetadataAccessFunction(Node *node,
                                                          unsigned depth) {
  Buffer << "Ma";
  return mangleSingleChildNode(node, depth + 1); // type
}

ManglingError Remangler::mangleTypeMetadataInstantiationCache(Node *node,
                                                              unsigned depth) {
  Buffer << "MI";
  return mangleSingleChildNode(node, depth + 1); // type
}

ManglingError
Remangler::mangleTypeMetadataInstantiationFunction(Node *node, unsigned depth) {
  Buffer << "Mi";
  return mangleSingleChildNode(node, depth + 1); // type
}

ManglingError
Remangler::mangleTypeMetadataSingletonInitializationCache(Node *node,
                                                          unsigned depth) {
  Buffer << "Ml";
  return mangleSingleChildNode(node, depth + 1); // type
}

ManglingError Remangler::mangleTypeMetadataCompletionFunction(Node *node,
                                                              unsigned depth) {
  Buffer << "Mr";
  return mangleSingleChildNode(node, depth + 1); // type
}

ManglingError Remangler::mangleTypeMetadataDemanglingCache(Node *node,
                                                           unsigned depth) {
  // not supported
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleTypeMetadataLazyCache(Node *node,
                                                     unsigned depth) {
  Buffer << "ML";
  return mangleSingleChildNode(node, depth + 1); // type
}

ManglingError Remangler::mangleMetaclass(Node *node, unsigned depth) {
  Buffer << "Mm";
  return mangleSingleChildNode(node, depth + 1); // type
}

ManglingError Remangler::mangleClassMetadataBaseOffset(Node *node,
                                                       unsigned depth) {
  Buffer << "Mo";
  return mangleSingleChildNode(node, depth + 1); // type
}

ManglingError Remangler::mangleNominalTypeDescriptor(Node *node,
                                                     unsigned depth) {
  Buffer << "Mn";
  return mangleSingleChildNode(node, depth + 1); // type
}

ManglingError Remangler::mangleNominalTypeDescriptorRecord(Node *node,
                                                           unsigned depth) {
  Buffer << "Hn";
  return mangleSingleChildNode(node, depth + 1); // type
}

ManglingError Remangler::manglePropertyDescriptor(Node *node, unsigned depth) {
  // not supported
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleTypeMetadata(Node *node, unsigned depth) {
  Buffer << "M";
  return mangleSingleChildNode(node, depth + 1); // type
}

ManglingError Remangler::mangleFullTypeMetadata(Node *node, unsigned depth) {
  Buffer << "Mf";
  return mangleChildNodes(node, depth + 1); // type
}

ManglingError Remangler::mangleProtocolDescriptor(Node *node, unsigned depth) {
  Buffer << "Mp";
  return mangleProtocolWithoutPrefix(node->begin()[0], depth + 1);
}

ManglingError Remangler::mangleProtocolDescriptorRecord(Node *node,
                                                        unsigned depth) {
  Buffer << "Hr";
  return mangleProtocolWithoutPrefix(node->begin()[0], depth + 1);
}

ManglingError
Remangler::mangleProtocolRequirementsBaseDescriptor(Node *node,
                                                    unsigned depth) {
  // ###TODO: Is this an error?
  Buffer << "<protocol-requirements-base-descriptor>";
  return ManglingError::Success;
}

ManglingError Remangler::mangleProtocolWitnessTablePattern(Node *node,
                                                           unsigned depth) {
  // todo
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleProtocolConformanceDescriptor(Node *node,
                                                             unsigned depth) {
  Buffer << "Mc";
  return mangleProtocolConformance(node->begin()[0], depth + 1);
}

ManglingError
Remangler::mangleProtocolConformanceDescriptorRecord(Node *node,
                                                     unsigned depth) {
  Buffer << "Hc";
  return mangleProtocolConformance(node->begin()[0], depth + 1);
}

ManglingError
Remangler::mangleProtocolSelfConformanceDescriptor(Node *node, unsigned depth) {
  Buffer << "MS";
  return mangleProtocol(node->begin()[0], depth + 1);
}

ManglingError Remangler::manglePartialApplyForwarder(Node *node,
                                                     unsigned depth) {
  Buffer << "PA";
  if (node->getNumChildren() == 1) {
    Buffer << "__T";
    RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1)); // global
  }
  return ManglingError::Success;
}

ManglingError Remangler::manglePartialApplyObjCForwarder(Node *node,
                                                         unsigned depth) {
  Buffer << "PAo";
  if (node->getNumChildren() == 1) {
    Buffer << "__T";
    RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1)); // global
  }
  return ManglingError::Success;
}

ManglingError Remangler::mangleMergedFunction(Node *node, unsigned depth) {
  Buffer << "Tm";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleDistributedThunk(Node *node, unsigned depth) {
  Buffer << "TE";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleDistributedAccessor(Node *node, unsigned depth) {
  Buffer << "TF";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleDynamicallyReplaceableFunctionImpl(Node *node,
                                                    unsigned depth) {
  Buffer << "TI";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleDynamicallyReplaceableFunctionKey(Node *node, unsigned depth) {
  Buffer << "Tx";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleDynamicallyReplaceableFunctionVar(Node *node, unsigned depth) {
  Buffer << "TX";
  return ManglingError::Success;
}

ManglingError Remangler::mangleAsyncAwaitResumePartialFunction(Node *node,
                                                               unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}
ManglingError
Remangler::mangleAsyncSuspendResumePartialFunction(Node *node, unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleDirectness(Node *node, unsigned depth) {
  switch (node->getIndex()) {
  case uint64_t(Directness::Direct):
    Buffer << 'd';
    break;
  case uint64_t(Directness::Indirect):
    Buffer << 'i';
    break;
  default:
    return MANGLING_ERROR(ManglingError::BadDirectness, node);
  }
  return ManglingError::Success;
}

ManglingError Remangler::mangleValueWitness(Node *node, unsigned depth) {
  const char *Code = nullptr;
  switch (node->getFirstChild()->getIndex()) {
#define VALUE_WITNESS(MANGLING, NAME)                                          \
  case uint64_t(ValueWitnessKind::NAME):                                       \
    Code = #MANGLING;                                                          \
    break;
#include "swift/Demangling/ValueWitnessMangling.def"
  default:
    return MANGLING_ERROR(ManglingError::BadValueWitnessKind, node);
  }
  Buffer << 'w' << Code;
  return mangleChildNode(node, 1, depth + 1); // type
}

ManglingError Remangler::mangleValueWitnessTable(Node *node, unsigned depth) {
  Buffer << "WV";
  return mangleSingleChildNode(node, depth + 1); // type
}

ManglingError Remangler::mangleConcurrentFunctionType(Node *node,
                                                      unsigned depth) {
  Buffer << "y";
  return ManglingError::Success;
}

ManglingError Remangler::mangleAsyncAnnotation(Node *node, unsigned depth) {
  Buffer << "Z";
  return ManglingError::Success;
}

ManglingError Remangler::mangleThrowsAnnotation(Node *node, unsigned depth) {
  Buffer << "z";
  return ManglingError::Success;
}

ManglingError Remangler::mangleDifferentiableFunctionType(Node *node,
                                                          unsigned depth) {
  Buffer << "D";
  return mangleChildNodes(node, depth + 1);
}

ManglingError Remangler::mangleGlobalActorFunctionType(Node *node,
                                                       unsigned depth) {
  Buffer << "Y" << (char)node->getIndex(); // differentiability kind
  return ManglingError::Success;
}

ManglingError Remangler::mangleFieldOffset(Node *node, unsigned depth) {
  Buffer << "Wv";
  return mangleChildNodes(node, depth + 1); // directness, entity
}

ManglingError Remangler::mangleEnumCase(Node *node, unsigned depth) {
  Buffer << "WC";
  return mangleSingleChildNode(node, depth + 1); // enum case
}

ManglingError
Remangler::mangleProtocolSelfConformanceWitnessTable(Node *node,
                                                     unsigned depth) {
  Buffer << "WS";
  return mangleSingleChildNode(node, depth + 1); // protocol
}

ManglingError Remangler::mangleProtocolWitnessTable(Node *node,
                                                    unsigned depth) {
  Buffer << "WP";
  return mangleSingleChildNode(node, depth + 1); // protocol conformance
}

ManglingError Remangler::mangleGenericProtocolWitnessTable(Node *node,
                                                           unsigned depth) {
  Buffer << "WG";
  return mangleSingleChildNode(node, depth + 1); // protocol conformance
}

ManglingError Remangler::mangleResilientProtocolWitnessTable(Node *node,
                                                             unsigned depth) {
  // todo
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleGenericProtocolWitnessTableInstantiationFunction(
    Node *node, unsigned depth) {
  Buffer << "WI";
  return mangleSingleChildNode(node, depth + 1); // protocol conformance
}

ManglingError Remangler::mangleProtocolWitnessTableAccessor(Node *node,
                                                            unsigned depth) {
  Buffer << "Wa";
  return mangleSingleChildNode(node, depth + 1); // protocol conformance
}

ManglingError
Remangler::mangleLazyProtocolWitnessTableAccessor(Node *node, unsigned depth) {
  Buffer << "Wl";
  return mangleChildNodes(node, depth + 1); // type, protocol conformance
}

ManglingError
Remangler::mangleLazyProtocolWitnessTableCacheVariable(Node *node,
                                                       unsigned depth) {
  Buffer << "WL";
  return mangleChildNodes(node, depth + 1); // type, protocol conformance
}

ManglingError Remangler::mangleAssociatedTypeDescriptor(Node *node,
                                                        unsigned depth) {
  // ###TODO: Check this (and similar ones below).  Should this be a failure?
  Buffer << "<associated-type-descriptor>";
  return ManglingError::Success;
}

ManglingError Remangler::mangleAssociatedConformanceDescriptor(Node *node,
                                                               unsigned depth) {
  Buffer << "<associated-conformance-descriptor>";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleDefaultAssociatedConformanceAccessor(Node *node,
                                                      unsigned depth) {
  Buffer << "<default-associated-conformance-descriptor>";
  return ManglingError::Success;
}

ManglingError Remangler::mangleBaseConformanceDescriptor(Node *node,
                                                         unsigned depth) {
  Buffer << "<base-conformance-descriptor>";
  return ManglingError::Success;
}

ManglingError Remangler::mangleAssociatedTypeMetadataAccessor(Node *node,
                                                              unsigned depth) {
  Buffer << "Wt";
  return mangleChildNodes(node, depth + 1); // protocol conformance, identifier
}

ManglingError
Remangler::mangleDefaultAssociatedTypeMetadataAccessor(Node *node,
                                                       unsigned depth) {
  Buffer << "<default-associated-type-metadata-accessor>";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleAssociatedTypeWitnessTableAccessor(Node *node,
                                                    unsigned depth) {
  Buffer << "WT";
  DEMANGLER_ASSERT(node->getNumChildren() == 3, node);
  RETURN_IF_ERROR(mangleChildNode(node, 0, depth + 1)); // protocol conformance
  RETURN_IF_ERROR(mangleChildNode(node, 1, depth + 1)); // type
  return mangleProtocolWithoutPrefix(node->begin()[2], depth + 1); // type
}

ManglingError Remangler::mangleBaseWitnessTableAccessor(Node *node,
                                                        unsigned depth) {
  Buffer << "<base-witness-table-accessor>";
  return ManglingError::Success;
}

ManglingError Remangler::mangleReabstractionThunkHelper(Node *node,
                                                        unsigned depth) {
  Buffer << "<reabstraction-thunk-helper>";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleReabstractionThunkHelperWithSelf(Node *node, unsigned depth) {
  Buffer << "<reabstraction-thunk-helper-with-self>";
  return ManglingError::Success;
}

ManglingError Remangler::mangleReabstractionThunk(Node *node, unsigned depth) {
  Buffer << "<reabstraction-thunk>";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleReabstractionThunkHelperWithGlobalActor(Node *node,
                                                         unsigned depth) {
  Buffer << "<reabstraction-thunk-helper-with-global-actor>";
  return ManglingError::Success;
}

ManglingError Remangler::mangleAutoDiffFunction(Node *node, EntityContext &ctx,
                                                unsigned depth) {
  Buffer << "<autodiff-function>";
  return ManglingError::Success;
}

ManglingError Remangler::mangleAutoDiffDerivativeVTableThunk(Node *node,
                                                             unsigned depth) {
  Buffer << "<autodiff-derivative-vtable-thunk>";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleAutoDiffSelfReorderingReabstractionThunk(Node *node,
                                                          unsigned depth) {
  Buffer << "<autodiff-self-reordering-reabstraction-thunk>";
  return ManglingError::Success;
}

ManglingError Remangler::mangleAutoDiffSubsetParametersThunk(Node *node,
                                                             unsigned depth) {
  Buffer << "<autodiff-subset-parameters-thunk>";
  return ManglingError::Success;
}

ManglingError Remangler::mangleAutoDiffFunctionKind(Node *node,
                                                    unsigned depth) {
  Buffer << "<autodiff-function-kind>";
  return ManglingError::Success;
}

ManglingError Remangler::mangleDifferentiabilityWitness(Node *node,
                                                        unsigned depth) {
  Buffer << "<differentiability-witness>";
  return ManglingError::Success;
}

ManglingError Remangler::mangleIndexSubset(Node *node, unsigned depth) {
  Buffer << "<index-subset>";
  return ManglingError::Success;
}

ManglingError Remangler::mangleProtocolSelfConformanceWitness(Node *node,
                                                              unsigned depth) {
  Buffer << "TS";
  return mangleSingleChildNode(node, depth + 1); // entity
}

ManglingError Remangler::mangleProtocolWitness(Node *node, unsigned depth) {
  Buffer << "TW";
  return mangleChildNodes(node, depth + 1); // protocol conformance, entity
}

ManglingError Remangler::mangleFunction(Node *node, EntityContext &ctx,
                                        unsigned depth) {
  return mangleNamedAndTypedEntity(node, 'F', "", ctx, depth + 1);
}

ManglingError Remangler::mangleVariable(Node *node, EntityContext &ctx,
                                        unsigned depth) {
  return mangleNamedAndTypedEntity(node, 'v', "", ctx, depth + 1);
}

ManglingError Remangler::mangleSubscript(Node *node, EntityContext &ctx,
                                         unsigned depth) {
  DEMANGLER_ASSERT(node->getNumChildren() >= 2, node);
  Buffer << 'i';
  RETURN_IF_ERROR(mangleEntityContext(node->begin()[0], ctx, depth + 1));
  if (node->getLastChild()->getKind() == Node::Kind::PrivateDeclName)
    RETURN_IF_ERROR(mangle(node->getLastChild(), depth + 1));

  if (node->getNumChildren() >= 3
      && node->begin()[1]->getKind() == Node::Kind::LabelList) {
    auto LabelList = node->begin()[1];
    auto Type = node->begin()[2];
    RETURN_IF_ERROR(mangleEntityType(applyParamLabels(LabelList, Type, Factory),
                                     ctx, depth + 1));
  } else {
    RETURN_IF_ERROR(mangleEntityType(node->begin()[1], ctx, depth + 1));
  }
  return ManglingError::Success;
}

ManglingError Remangler::mangleMacro(Node *node, unsigned depth) {
  Buffer << "fm";
  return mangleChildNodes(node, depth + 1);
}

ManglingError Remangler::mangleFreestandingMacroExpansion(
    Node *node, unsigned depth) {
  Buffer << "fMf";
  RETURN_IF_ERROR(mangleIndex(node, depth + 1));
  return mangleChildNodes(node, depth + 1);
}

ManglingError Remangler::mangleAccessorAttachedMacroExpansion(
    Node *node, unsigned depth) {
  Buffer << "fMa";
  RETURN_IF_ERROR(mangleIndex(node, depth + 1));
  return mangleChildNodes(node, depth + 1);
}

ManglingError Remangler::mangleMemberAttributeAttachedMacroExpansion(
    Node *node, unsigned depth) {
  Buffer << "fMr";
  RETURN_IF_ERROR(mangleIndex(node, depth + 1));
  return mangleChildNodes(node, depth + 1);
}

ManglingError Remangler::mangleMemberAttachedMacroExpansion(
    Node *node, unsigned depth) {
  Buffer << "fMm";
  RETURN_IF_ERROR(mangleIndex(node, depth + 1));
  return mangleChildNodes(node, depth + 1);
}

ManglingError Remangler::manglePeerAttachedMacroExpansion(
    Node *node, unsigned depth) {
  Buffer << "fMp";
  RETURN_IF_ERROR(mangleIndex(node, depth + 1));
  return mangleChildNodes(node, depth + 1);
}

ManglingError Remangler::mangleConformanceAttachedMacroExpansion(
    Node *node, unsigned depth) {
  Buffer << "fMc";
  RETURN_IF_ERROR(mangleIndex(node, depth + 1));
  return mangleChildNodes(node, depth + 1);
}

ManglingError Remangler::mangleMacroExpansionUniqueName(
    Node *node, unsigned depth) {
  Buffer << "fMu";
  RETURN_IF_ERROR(mangleIndex(node, depth + 1));
  return mangleChildNodes(node, depth + 1);
}

ManglingError Remangler::mangleAccessor(Node *storageNode,
                                        StringRef accessorCode,
                                        EntityContext &ctx, unsigned depth) {
  Buffer << 'F';
  RETURN_IF_ERROR(
      mangleEntityContext(storageNode->getChild(0), ctx, depth + 1));
  Buffer << accessorCode;

  auto mangleAccessorType = [&](unsigned TypeIndex) {
    auto LabelList = storageNode->getChild(TypeIndex);
    if (LabelList->getKind() == Node::Kind::LabelList) {
      auto Type = storageNode->getChild(TypeIndex + 1);
      return mangleEntityType(applyParamLabels(LabelList, Type, Factory), ctx,
                              depth + 1);
    } else {
      return mangleEntityType(storageNode->getChild(TypeIndex), ctx, depth + 1);
    }
  };

  switch (storageNode->getKind()) {
  case Demangle::Node::Kind::Variable: {
    RETURN_IF_ERROR(mangleChildNode(storageNode, 1, depth + 1));
    RETURN_IF_ERROR(mangleAccessorType(2));
    break;
  }

  case Demangle::Node::Kind::Subscript: {
    auto NumChildren = storageNode->getNumChildren();
    DEMANGLER_ASSERT(NumChildren <= 4, storageNode);

    auto PrivateName = storageNode->getChild(NumChildren - 1);
    if (PrivateName->getKind() == Node::Kind::PrivateDeclName)
      RETURN_IF_ERROR(mangle(PrivateName, depth + 1));

    RETURN_IF_ERROR(mangleIdentifier("subscript", OperatorKind::NotOperator));
    RETURN_IF_ERROR(mangleAccessorType(1));
    break;
  }
  default:
    return MANGLING_ERROR(ManglingError::NotAStorageNode, storageNode);
  }

  return ManglingError::Success;
}

ManglingError Remangler::mangleInitializer(Node *node, EntityContext &ctx,
                                           unsigned depth) {
  return mangleSimpleEntity(node, 'I', "i", ctx, depth + 1);
}

ManglingError Remangler::manglePropertyWrapperBackingInitializer(
    Node *node, EntityContext &ctx, unsigned depth) {
  return mangleSimpleEntity(node, 'I', "P", ctx, depth + 1);
}

ManglingError Remangler::manglePropertyWrapperInitFromProjectedValue(
    Node *node, EntityContext &ctx, unsigned depth) {
  return mangleSimpleEntity(node, 'I', "W", ctx, depth + 1);
}

ManglingError Remangler::mangleDefaultArgumentInitializer(Node *node,
                                                          EntityContext &ctx,
                                                          unsigned depth) {
  return mangleNamedEntity(node, 'I', "A", ctx, depth + 1);
}

ManglingError Remangler::mangleAsyncFunctionPointer(Node *node,
                                                    unsigned depth) {
  Buffer << "Tu";
  return ManglingError::Success;
}

ManglingError Remangler::mangleDeallocator(Node *node, EntityContext &ctx,
                                           unsigned depth) {
  return mangleSimpleEntity(node, 'F', "D", ctx, depth + 1);
}

ManglingError Remangler::mangleDestructor(Node *node, EntityContext &ctx,
                                          unsigned depth) {
  return mangleSimpleEntity(node, 'F', "d", ctx, depth + 1);
}

ManglingError Remangler::mangleAllocator(Node *node, EntityContext &ctx,
                                         unsigned depth) {
  return mangleTypedEntity(node, 'F', "C", ctx, depth + 1);
}

ManglingError Remangler::mangleConstructor(Node *node, EntityContext &ctx,
                                           unsigned depth) {
  return mangleTypedEntity(node, 'F', "c", ctx, depth + 1);
}

ManglingError Remangler::mangleIVarInitializer(Node *node, EntityContext &ctx,
                                               unsigned depth) {
  return mangleSimpleEntity(node, 'F', "e", ctx, depth + 1);
}

ManglingError Remangler::mangleIVarDestroyer(Node *node, EntityContext &ctx,
                                             unsigned depth) {
  return mangleSimpleEntity(node, 'F', "E", ctx, depth + 1);
}

ManglingError Remangler::mangleGetter(Node *node, EntityContext &ctx,
                                      unsigned depth) {
  return mangleAccessor(node->getFirstChild(), "g", ctx, depth + 1);
}

ManglingError Remangler::mangleGlobalGetter(Node *node, EntityContext &ctx,
                                            unsigned depth) {
  return mangleAccessor(node->getFirstChild(), "G", ctx, depth + 1);
}

ManglingError Remangler::mangleSetter(Node *node, EntityContext &ctx,
                                      unsigned depth) {
  return mangleAccessor(node->getFirstChild(), "s", ctx, depth + 1);
}

ManglingError Remangler::mangleMaterializeForSet(Node *node, EntityContext &ctx,
                                                 unsigned depth) {
  return mangleAccessor(node->getFirstChild(), "m", ctx, depth + 1);
}

ManglingError Remangler::mangleWillSet(Node *node, EntityContext &ctx,
                                       unsigned depth) {
  return mangleAccessor(node->getFirstChild(), "w", ctx, depth + 1);
}

ManglingError Remangler::mangleDidSet(Node *node, EntityContext &ctx,
                                      unsigned depth) {
  return mangleAccessor(node->getFirstChild(), "W", ctx, depth + 1);
}

ManglingError Remangler::mangleOwningMutableAddressor(Node *node,
                                                      EntityContext &ctx,
                                                      unsigned depth) {
  return mangleAccessor(node->getFirstChild(), "aO", ctx, depth + 1);
}

ManglingError Remangler::mangleNativeOwningMutableAddressor(Node *node,
                                                            EntityContext &ctx,
                                                            unsigned depth) {
  return mangleAccessor(node->getFirstChild(), "ao", ctx, depth + 1);
}

ManglingError Remangler::mangleNativePinningMutableAddressor(Node *node,
                                                             EntityContext &ctx,
                                                             unsigned depth) {
  return mangleAccessor(node->getFirstChild(), "ap", ctx, depth + 1);
}

ManglingError Remangler::mangleUnsafeMutableAddressor(Node *node,
                                                      EntityContext &ctx,
                                                      unsigned depth) {
  return mangleAccessor(node->getFirstChild(), "au", ctx, depth + 1);
}

ManglingError Remangler::mangleOwningAddressor(Node *node, EntityContext &ctx,
                                               unsigned depth) {
  return mangleAccessor(node->getFirstChild(), "lO", ctx, depth + 1);
}

ManglingError Remangler::mangleNativeOwningAddressor(Node *node,
                                                     EntityContext &ctx,
                                                     unsigned depth) {
  return mangleAccessor(node->getFirstChild(), "lo", ctx, depth + 1);
}

ManglingError Remangler::mangleNativePinningAddressor(Node *node,
                                                      EntityContext &ctx,
                                                      unsigned depth) {
  return mangleAccessor(node->getFirstChild(), "lp", ctx, depth + 1);
}

ManglingError Remangler::mangleUnsafeAddressor(Node *node, EntityContext &ctx,
                                               unsigned depth) {
  return mangleAccessor(node->getFirstChild(), "lu", ctx, depth + 1);
}

ManglingError Remangler::mangleReadAccessor(Node *node, EntityContext &ctx,
                                            unsigned depth) {
  return mangleAccessor(node->getFirstChild(), "r", ctx, depth + 1);
}

ManglingError Remangler::mangleModifyAccessor(Node *node, EntityContext &ctx,
                                              unsigned depth) {
  return mangleAccessor(node->getFirstChild(), "M", ctx, depth + 1);
}

ManglingError Remangler::mangleExplicitClosure(Node *node, EntityContext &ctx,
                                               unsigned depth) {
  return mangleNamedAndTypedEntity(node, 'F', "U", ctx,
                                   depth + 1); // name is index
}

ManglingError Remangler::mangleImplicitClosure(Node *node, EntityContext &ctx,
                                               unsigned depth) {
  return mangleNamedAndTypedEntity(node, 'F', "u", ctx,
                                   depth + 1); // name is index
}

ManglingError Remangler::mangleStatic(Node *node, EntityContext &ctx,
                                      unsigned depth) {
  Buffer << 'Z';
  return mangleEntityContext(node->getChild(0), ctx, depth + 1);
}

ManglingError Remangler::mangleSimpleEntity(Node *node, char basicKind,
                                            StringRef entityKind,
                                            EntityContext &ctx,
                                            unsigned depth) {
  DEMANGLER_ASSERT(node->getNumChildren() == 1, node);
  Buffer << basicKind;
  RETURN_IF_ERROR(mangleEntityContext(node->begin()[0], ctx, depth + 1));
  Buffer << entityKind;
  return ManglingError::Success;
}

ManglingError
Remangler::mangleNamedEntity(Node *node, char basicKind, StringRef entityKind,
                             EntityContext &ctx, unsigned depth,
                             StringRef artificialPrivateDiscriminator) {
  DEMANGLER_ASSERT(node->getNumChildren() == 2, node);
  if (basicKind != '\0') Buffer << basicKind;
  RETURN_IF_ERROR(mangleEntityContext(node->begin()[0], ctx, depth + 1));
  Buffer << entityKind;
  
  auto privateDiscriminator = ctx.takeAnonymousContextDiscriminator();
  if (!privateDiscriminator.empty() &&
      swift::Mangle::isDigit(privateDiscriminator[0]))
    privateDiscriminator = "_" + privateDiscriminator;
  if (!artificialPrivateDiscriminator.empty())
    privateDiscriminator.append(artificialPrivateDiscriminator.data(),
                                artificialPrivateDiscriminator.size());
  
  // Include the artificial private discriminator if one was given.
  auto name = node->getChild(1);
  if (!privateDiscriminator.empty()
      && name->getKind() == Node::Kind::Identifier) {
    Buffer << 'P';
    RETURN_IF_ERROR(
        mangleIdentifier(privateDiscriminator, OperatorKind::NotOperator));
  }
  return mangle(name, depth + 1);
}

ManglingError Remangler::mangleTypedEntity(Node *node, char basicKind,
                                           StringRef entityKind,
                                           EntityContext &ctx, unsigned depth) {
  DEMANGLER_ASSERT(node->getNumChildren() == 2 || node->getNumChildren() == 3,
                   node);
  Buffer << basicKind;
  RETURN_IF_ERROR(mangleEntityContext(node->begin()[0], ctx, depth + 1));
  Buffer << entityKind;

  if (node->begin()[1]->getKind() == Node::Kind::LabelList) {
    auto LabelList = node->begin()[1];
    auto Type = node->begin()[2];
    RETURN_IF_ERROR(mangleEntityType(applyParamLabels(LabelList, Type, Factory),
                                     ctx, depth + 1));
  } else {
    RETURN_IF_ERROR(mangleEntityType(node->begin()[1], ctx, depth + 1));
  }
  return ManglingError::Success;
}

ManglingError Remangler::mangleNamedAndTypedEntity(Node *node, char basicKind,
                                                   StringRef entityKind,
                                                   EntityContext &ctx,
                                                   unsigned depth) {
  DEMANGLER_ASSERT(node->getNumChildren() == 3 || node->getNumChildren() == 4,
                   node);
  Buffer << basicKind;
  RETURN_IF_ERROR(mangleEntityContext(node->begin()[0], ctx, depth + 1));
  Buffer << entityKind;
  RETURN_IF_ERROR(mangleChildNode(node, 1, depth + 1)); // decl name / index

  if (node->begin()[2]->getKind() == Node::Kind::LabelList) {
    auto LabelList = node->begin()[2];
    auto Type = node->begin()[3];
    RETURN_IF_ERROR(mangleEntityType(applyParamLabels(LabelList, Type, Factory),
                                     ctx, depth + 1));
  } else {
    RETURN_IF_ERROR(mangleEntityType(node->begin()[2], ctx, depth + 1));
  }
  return ManglingError::Success;
}

ManglingError Remangler::mangleEntityContext(Node *node, EntityContext &ctx,
                                             unsigned depth) {
  // Remember that we're mangling a context.
  EntityContext::ManglingContextRAII raii(ctx);

  // Deal with bound generic types.
  switch (node->getKind()) {
    case Node::Kind::BoundGenericStructure:
    case Node::Kind::BoundGenericEnum:
    case Node::Kind::BoundGenericClass:
    case Node::Kind::BoundGenericOtherNominalType:
    case Node::Kind::BoundGenericTypeAlias:
      return mangleAnyNominalType(node, ctx, depth + 1);

    default:
      break;
  }

  switch (node->getKind()) {
#define NODE(ID)                                \
  case Node::Kind::ID:
#define CONTEXT_NODE(ID)
#include "swift/Demangling/DemangleNodes.def"
    return MANGLING_ERROR(ManglingError::NotAContextNode, node);

#define NODE(ID)
#define CONTEXT_NODE(ID)                                                       \
  case Node::Kind::ID:                                                         \
    return mangle##ID(node, ctx, depth + 1);
#include "swift/Demangling/DemangleNodes.def"
  }

  return MANGLING_ERROR(ManglingError::BadNodeKind, node);
}

ManglingError Remangler::mangleEntityType(Node *node, EntityContext &ctx,
                                          unsigned depth) {
  DEMANGLER_ASSERT(node->getKind() == Node::Kind::Type, node);
  DEMANGLER_ASSERT(node->getNumChildren() == 1, node);
  node = node->begin()[0];

  // Expand certain kinds of type within the entity context.
  switch (node->getKind()) {
  case Node::Kind::NoEscapeFunctionType:
  case Node::Kind::FunctionType:
  case Node::Kind::UncurriedFunctionType: {
    Buffer << ((node->getKind() == Node::Kind::FunctionType ||
             node->getKind() == Node::Kind::NoEscapeFunctionType)
                ? 'F'
                : 'f');
    DEMANGLER_ASSERT(node->getNumChildren() >= 2, node);
    unsigned inputIndex = node->getNumChildren() - 2;
    for (unsigned i = 0; i <= inputIndex; ++i)
      RETURN_IF_ERROR(mangle(node->begin()[i], depth + 1));
    auto returnType = node->begin()[inputIndex+1];
    DEMANGLER_ASSERT(returnType->getKind() == Node::Kind::ReturnType,
                     returnType);
    DEMANGLER_ASSERT(returnType->getNumChildren() == 1, returnType);
    return mangleEntityType(returnType->begin()[0], ctx, depth + 1);
  }
  default:
    return mangle(node, depth + 1);
  }
}

ManglingError Remangler::mangleLocalDeclName(Node *node, unsigned depth) {
  Buffer << 'L';
  return mangleChildNodes(node, depth + 1); // index, identifier
}

ManglingError Remangler::manglePrivateDeclName(Node *node, unsigned depth) {
  Buffer << 'P';
  return mangleChildNodes(node, depth + 1); // identifier, identifier
}

ManglingError Remangler::mangleRelatedEntityDeclName(Node *node,
                                                     unsigned depth) {
  // Non-round-trip mangling: pretend we have a private discriminator "$A" for a
  // related entity "A".
  NodePointer kindNode = node->getFirstChild();
  Buffer << 'P' << (kindNode->getText().size() + 1) << '$' << kindNode->getText();
  return mangleChildNode(node, 1, depth + 1);
}

ManglingError Remangler::mangleTypeMangling(Node *node, unsigned depth) {
  Buffer << 't';
  return mangleSingleChildNode(node, depth + 1); // type
}

ManglingError Remangler::mangleType(Node *node, unsigned depth) {
  return mangleSingleChildNode(node, depth + 1);
}

template <size_t N> 
static bool stripPrefix(StringRef &string, const char (&data)[N]) {
  constexpr size_t prefixLength = N - 1;
  if (!string.startswith(StringRef(data, prefixLength)))
    return false;
  string = string.drop_front(prefixLength);
  return true;
}

ManglingError Remangler::mangleBuiltinTypeName(Node *node, unsigned depth) {
  Buffer << 'B';
  StringRef text = node->getText();

  if (text == "Builtin.BridgeObject") {
    Buffer << 'b';
  } else if (text == "Builtin.UnsafeValueBuffer") {
    Buffer << 'B';
  } else if (text == "Builtin.UnknownObject") {
    Buffer << 'O';
  } else if (text == "Builtin.NativeObject") {
    Buffer << 'o';
  } else if (text == "Builtin.RawPointer") {
    Buffer << 'p';
  } else if (text == "Builtin.Word") {
    Buffer << 'w';
  } else if (stripPrefix(text, "Builtin.Int")) {
    Buffer << 'i' << text << '_';
  } else if (stripPrefix(text, "Builtin.FPIEEE")) {
    Buffer << 'f' << text << '_';
  } else if (stripPrefix(text, "Builtin.Vec")) {
    // Avoid using StringRef::split because its definition is not
    // provided in the header so that it requires linking with libSupport.a.
    size_t splitIdx = text.find('x');
    Buffer << 'v' << text.substr(0, splitIdx) << 'B';
    auto element = text.substr(splitIdx).substr(1);
    if (element == "RawPointer") {
      Buffer << 'p';
    } else if (stripPrefix(element, "FPIEEE")) {
      Buffer << 'f' << element << '_';
    } else if (stripPrefix(element, "Int")) {
      Buffer << 'i' << element << '_';
    } else {
      return MANGLING_ERROR(ManglingError::UnexpectedBuiltinVectorType, node);
    }
  } else {
    return MANGLING_ERROR(ManglingError::UnexpectedBuiltinType, node);
  }

  return ManglingError::Success;
}

ManglingError Remangler::mangleTypeAlias(Node *node, EntityContext &ctx,
                                         unsigned depth) {
  return mangleAnyNominalType(node, ctx, depth + 1);
}

ManglingError Remangler::mangleFunctionType(Node *node, unsigned depth) {
  Buffer << 'F';
  return mangleChildNodes(node, depth + 1); // argument tuple, result type
}

ManglingError Remangler::mangleUncurriedFunctionType(Node *node,
                                                     unsigned depth) {
  Buffer << 'f';
  return mangleChildNodes(node, depth + 1); // argument tuple, result type
}

ManglingError Remangler::mangleObjCBlock(Node *node, unsigned depth) {
  Buffer << 'b';
  return mangleChildNodes(node, depth + 1); // argument tuple, result type
}

ManglingError Remangler::mangleEscapingObjCBlock(Node *node, unsigned depth) {
  // ###TODO: Is this right?  Should this be an error?
  // We shouldn't ever be remangling anything with a DWARF-only mangling.
  Buffer << "<escaping block type>";
  return ManglingError::Success;
}

ManglingError Remangler::mangleCFunctionPointer(Node *node, unsigned depth) {
  Buffer << 'c';
  return mangleChildNodes(node, depth + 1); // argument tuple, result type
}

ManglingError Remangler::mangleAutoClosureType(Node *node, unsigned depth) {
  Buffer << 'K';
  return mangleChildNodes(node, depth + 1); // argument tuple, result type
}

ManglingError Remangler::mangleNoEscapeFunctionType(Node *node,
                                                    unsigned depth) {
  Buffer << 'F';
  return mangleChildNodes(node, depth + 1); // argument tuple, result type
}

ManglingError Remangler::mangleEscapingAutoClosureType(Node *node,
                                                       unsigned depth) {
  Buffer << 'K';
  return mangleChildNodes(node, depth + 1); // argument tuple, result type
}

ManglingError Remangler::mangleThinFunctionType(Node *node, unsigned depth) {
  Buffer << "Xf";
  return mangleChildNodes(node, depth + 1); // argument tuple, result type
}

ManglingError Remangler::mangleArgumentTuple(Node *node, unsigned depth) {
  return mangleSingleChildNode(node, depth + 1);
}

ManglingError Remangler::mangleReturnType(Node *node, unsigned depth) {
  return mangleSingleChildNode(node, depth + 1);
}

ManglingError Remangler::mangleImplFunctionType(Node *node, unsigned depth) {
  Buffer << "XF";
  auto i = node->begin(), e = node->end();
  if (i != e && (*i)->getKind() == Node::Kind::ImplConvention) {
    StringRef text = (*i)->getText();
    ++i;
    if (text == "@callee_unowned") {
      Buffer << 'd';
    } else if (text == "@callee_guaranteed") {
      Buffer << 'g';
    } else if (text == "@callee_owned") {
      Buffer << 'o';
    } else {
      return MANGLING_ERROR(ManglingError::InvalidImplCalleeConvention, *i);
    }
  } else {
    Buffer << 't';
  }
  for (; i != e &&
         (*i)->getKind() == Node::Kind::ImplFunctionAttribute; ++i) {
    RETURN_IF_ERROR(mangle(*i, depth + 1)); // impl function attribute
  }
  if (i != e &&
      ((*i)->getKind() == Node::Kind::DependentGenericSignature ||
       (*i)->getKind() == Node::Kind::DependentPseudogenericSignature)) {
    Buffer << ((*i)->getKind() == Node::Kind::DependentGenericSignature
              ? 'G' : 'g');
    RETURN_IF_ERROR(mangleDependentGenericSignature((*i), depth + 1));
    ++i;
  }
  Buffer << '_';
  for (; i != e && (*i)->getKind() == Node::Kind::ImplParameter; ++i) {
    RETURN_IF_ERROR(mangleImplParameter(*i, depth + 1));
  }
  Buffer << '_';
  RETURN_IF_ERROR(mangleNodes(i, e, depth + 1)); // impl results
  Buffer << '_';
  return ManglingError::Success;
}

ManglingError Remangler::mangleImplFunctionAttribute(Node *node,
                                                     unsigned depth) {
  StringRef text = node->getText();
  if (text == "@yield_once") {
    Buffer << "A";
  } else if (text == "@yield_many") {
    Buffer << "G";
  } else if (text == "@Sendable") {
    Buffer << "h";
  } else if (text == "@async") {
    Buffer << "H";
  } else {
    return MANGLING_ERROR(ManglingError::InvalidImplFunctionAttribute, node);
  }
  return ManglingError::Success;
}

ManglingError Remangler::mangleImplFunctionConvention(Node *node,
                                                      unsigned depth) {
  return mangle(node->getChild(0), depth + 1);
}

ManglingError Remangler::mangleImplFunctionConventionName(Node *node,
                                                          unsigned depth) {
  StringRef text = node->getText();
  if (text == "block") {
    Buffer << "Cb";
  } else if (text == "c") {
    Buffer << "Cc";
  } else if (text == "method") {
    Buffer << "Cm";
  } else if (text == "objc_method") {
    Buffer << "CO";
  } else if (text == "witness_method") {
    Buffer << "Cw";
  } else {
    return MANGLING_ERROR(ManglingError::InvalidImplCalleeConvention, node);
  }
  return ManglingError::Success;
}

ManglingError Remangler::mangleClangType(Node *node, unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleImplParameter(Node *node, unsigned depth) {
  DEMANGLER_ASSERT(node->getNumChildren() == 2, node);
  return mangleChildNodes(node, depth + 1); // impl convention, type
}

ManglingError Remangler::mangleImplErrorResult(Node *node, unsigned depth) {
  DEMANGLER_ASSERT(node->getNumChildren() == 2, node);
  Buffer << 'z';
  return mangleChildNodes(node, depth + 1); // impl convention, type
}

ManglingError Remangler::mangleImplResult(Node *node, unsigned depth) {
  DEMANGLER_ASSERT(node->getNumChildren() == 2, node);
  return mangleChildNodes(node, depth + 1); // impl convention, type
}

ManglingError Remangler::mangleImplYield(Node *node, unsigned depth) {
  DEMANGLER_ASSERT(node->getNumChildren() == 2, node);
  Buffer << 'Y';
  return mangleChildNodes(node, depth + 1); // impl convention, type
}

ManglingError Remangler::mangleImplDifferentiabilityKind(Node *node,
                                                         unsigned depth) {
  // TODO(TF-750): Check if this code path actually triggers and add a test.
  Buffer << (char)node->getIndex();
  return ManglingError::Success;
}

ManglingError Remangler::mangleImplEscaping(Node *node, unsigned depth) {
  // The old mangler does not encode escaping.
  return ManglingError::Success;
}

ManglingError Remangler::mangleImplPatternSubstitutions(Node *node,
                                                        unsigned depth) {
  // The old mangler does not encode substituted function types.
  return ManglingError::Success;
}

ManglingError Remangler::mangleImplInvocationSubstitutions(Node *node,
                                                           unsigned depth) {
  // The old mangler does not encode substituted function types.
  return ManglingError::Success;
}

ManglingError Remangler::mangleImplConvention(Node *node, unsigned depth) {
  DEMANGLER_ASSERT(node->getKind() == Node::Kind::ImplConvention, node);
  StringRef text = node->getText();
  if (text == "@autoreleased") {
    Buffer << 'a';
  } else if (text == "@unowned") {
    Buffer << 'd';
  } else if (text == "@unowned_inner_pointer") {
    Buffer << 'D'; // only in results
  } else if (text == "@guaranteed") {
    Buffer << 'g';
  } else if (text == "@deallocating") {
    Buffer << 'e';
  } else if (text == "@in") {
    Buffer << 'i'; // only in parameters
  } else if (text == "@out") {
    Buffer << 'i'; // only in results
  } else if (text == "@inout") {
    Buffer << 'l';
  } else if (text == "@owned") {
    Buffer << 'o';
  } else {
    return MANGLING_ERROR(ManglingError::InvalidImplParameterConvention, node);
  }
  return ManglingError::Success;
}

ManglingError
Remangler::mangleImplParameterResultDifferentiability(Node *node,
                                                      unsigned depth) {
  DEMANGLER_ASSERT(node->getKind() == Node::Kind::ImplDifferentiabilityKind,
                   node);
  StringRef text = node->getText();
  // Empty string represents default differentiability.
  if (text.empty())
    return ManglingError::Success;
  if (text == "@noDerivative") {
    Buffer << 'w';
    return ManglingError::Success;
  }
  return MANGLING_ERROR(ManglingError::InvalidImplDifferentiability, node);
}

ManglingError Remangler::mangleDynamicSelf(Node *node, unsigned depth) {
  Buffer << 'D';
  return mangleSingleChildNode(node, depth + 1); // type
}

ManglingError Remangler::mangleErrorType(Node *node, unsigned depth) {
  Buffer << "ERR";
  return ManglingError::Success;
}

ManglingError Remangler::mangleSILBoxType(Node *node, unsigned depth) {
  Buffer << 'X' << 'b';
  return mangleSingleChildNode(node, depth + 1);
}

ManglingError Remangler::mangleMetatype(Node *node, unsigned depth) {
  if (node->getNumChildren() == 1) {
    Buffer << 'M';
    return mangleSingleChildNode(node, depth + 1); // type
  } else {
    DEMANGLER_ASSERT(node->getNumChildren() == 2, node);
    Buffer << "XM";
    return mangleChildNodes(node, depth + 1); // metatype representation, type
  }
}

ManglingError Remangler::mangleExistentialMetatype(Node *node, unsigned depth) {
  if (node->getNumChildren() == 1) {
    Buffer << "PM";
    return mangleSingleChildNode(node, depth + 1); // type
  } else {
    DEMANGLER_ASSERT(node->getNumChildren() == 2, node);
    Buffer << "XPM";
    return mangleChildNodes(node, depth + 1); // metatype representation, type
  }
}

ManglingError Remangler::mangleMetatypeRepresentation(Node *node,
                                                      unsigned depth) {
  StringRef text = node->getText();
  if (text == "@thin") {
    Buffer << 't';
  } else if (text == "@thick") {
    Buffer << 'T';
  } else if (text == "@objc_metatype") {
    Buffer << 'o';
  } else {
    return MANGLING_ERROR(ManglingError::InvalidMetatypeRepresentation, node);
  }
  return ManglingError::Success;
}

ManglingError Remangler::mangleProtocolList(Node *node, unsigned depth) {
  // In its usual use as a type, this gets a prefix 'P'.
  Buffer << 'P';
  return mangleProtocolListWithoutPrefix(node, depth + 1);
}

ManglingError
Remangler::mangleProtocolListWithoutPrefix(Node *node, unsigned depth,
                                           Node *additionalProto) {
  DEMANGLER_ASSERT(node->getKind() == Node::Kind::ProtocolList, node);
  DEMANGLER_ASSERT(node->getNumChildren() == 1, node);
  auto typeList = node->begin()[0];
  DEMANGLER_ASSERT(typeList->getKind() == Node::Kind::TypeList, typeList);
  for (auto &child : *typeList) {
    RETURN_IF_ERROR(mangleProtocolWithoutPrefix(child, depth + 1));
  }
  if (additionalProto) {
    RETURN_IF_ERROR(mangleProtocolWithoutPrefix(additionalProto, depth + 1));
  }
  Buffer << '_';
  return ManglingError::Success;
}

#define REF_STORAGE(Name, ...)                                                 \
  ManglingError Remangler::mangle##Name(Node *node, unsigned depth) {          \
    Buffer << manglingOf(ReferenceOwnership::Name);                            \
    return mangleSingleChildNode(node, depth + 1); /* type */                  \
  }
#include "swift/AST/ReferenceStorage.def"

ManglingError Remangler::mangleShared(Node *node, unsigned depth) {
  Buffer << 'h';
  return mangleSingleChildNode(node, depth + 1); // type
}

ManglingError Remangler::mangleOwned(Node *node, unsigned depth) {
  Buffer << 'n';
  return mangleSingleChildNode(node, depth + 1); // type
}

ManglingError Remangler::mangleInOut(Node *node, unsigned depth) {
  Buffer << 'R';
  return mangleSingleChildNode(node, depth + 1); // type
}

ManglingError Remangler::mangleIsolated(Node *node, unsigned depth) {
  Buffer << "Yi";
  return mangleSingleChildNode(node, depth + 1); // type
}

ManglingError Remangler::mangleCompileTimeConst(Node *node, unsigned depth) {
  Buffer << "Yt";
  return mangleSingleChildNode(node, depth + 1); // type
}

ManglingError Remangler::mangleNoDerivative(Node *node, unsigned depth) {
  Buffer << 'k';
  return mangleSingleChildNode(node, depth + 1); // type
}

ManglingError Remangler::mangleTuple(Node *node, unsigned depth) {
  size_t NumElems = node->getNumChildren();
  if (NumElems > 0 &&
      node->getChild(NumElems - 1)->getFirstChild()->getKind() ==
      Node::Kind::VariadicMarker) {
    Buffer << 't';
  } else {
    Buffer << 'T';
  }
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1)); // tuple elements
  Buffer << '_';
  return ManglingError::Success;
}

ManglingError Remangler::mangleTupleElement(Node *node, unsigned depth) {
  return mangleChildNodes(node, depth + 1); // tuple element name?, type
}

ManglingError Remangler::mangleTupleElementName(Node *node, unsigned depth) {
  return mangleIdentifier(node->getText(), OperatorKind::NotOperator);
}

ManglingError Remangler::manglePack(Node *node, unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleSILPackDirect(Node *node, unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleSILPackIndirect(Node *node, unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::manglePackExpansion(Node *node, unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleDependentGenericType(Node *node,
                                                    unsigned depth) {
  Buffer << 'u';
  return mangleChildNodes(node, depth + 1); // generic signature, type
}

ManglingError Remangler::mangleDependentPseudogenericSignature(Node *node,
                                                               unsigned depth) {
  return mangleDependentGenericSignature(node, depth + 1);
}

ManglingError Remangler::mangleDependentGenericParamPackMarker(Node *node,
                                                               unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleDependentGenericSignature(Node *node,
                                                         unsigned depth) {
  auto i = node->begin(), e = node->end();

  // If there's only one generic param, mangle nothing.
  if (node->getNumChildren() >= 1
      && node->getChild(0)->getKind() == Node::Kind::DependentGenericParamCount
      && node->getChild(0)->getIndex() == 1
      && (node->getNumChildren() == 1
          || node->getChild(1)->getKind() != Node::Kind::DependentGenericParamCount))
  {
    ++i;
    goto mangle_requirements;
  }

  // Remangle generic params.
  for (; i != e &&
         (*i)->getKind() == Node::Kind::DependentGenericParamCount; ++i) {
    auto count = *i;
    if (count->getIndex() > 0)
      mangleIndex(count->getIndex() - 1);
    else
      Buffer << 'z';
  }

mangle_requirements:
  if (i == e) { // no generic requirements
    Buffer << 'r';
    return ManglingError::Success;
  }

  Buffer << 'R';
  RETURN_IF_ERROR(mangleNodes(i, e, depth + 1)); // generic requirements
  Buffer << 'r';
  return ManglingError::Success;
}

ManglingError Remangler::mangleDependentGenericParamCount(Node *node,
                                                          unsigned depth) {
  // handled inline in DependentGenericSignature
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError
Remangler::mangleDependentGenericConformanceRequirement(Node *node,
                                                        unsigned depth) {
  RETURN_IF_ERROR(mangleConstrainedType(node->getChild(0), depth + 1));
  // If the constraint represents a protocol, use the shorter mangling.
  if (node->getNumChildren() == 2
      && node->getChild(1)->getKind() == Node::Kind::Type
      && node->getChild(1)->getNumChildren() == 1
      && node->getChild(1)->getChild(0)->getKind() == Node::Kind::Protocol) {
    return mangleProtocolWithoutPrefix(node->getChild(1)->getChild(0),
                                       depth + 1);
  }

  return mangle(node->getChild(1), depth + 1);
}

ManglingError
Remangler::mangleDependentGenericSameTypeRequirement(Node *node,
                                                     unsigned depth) {
  RETURN_IF_ERROR(mangleConstrainedType(node->getChild(0), depth + 1));
  Buffer << 'z';
  return mangle(node->getChild(1), depth + 1);
}

ManglingError
Remangler::mangleDependentGenericSameShapeRequirement(Node *node,
                                                      unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError
Remangler::mangleDependentGenericLayoutRequirement(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleConstrainedType(node->getChild(0), depth + 1));
  Buffer << 'l';
  auto id =  node->getChild(1)->getText();
  auto size = -1;
  if (node->getNumChildren() > 2) {
    size = node->getChild(2)->getIndex();
  }
  int alignment = -1;
  if (node->getNumChildren() > 3) {
    alignment = node->getChild(3)->getIndex();
  }
  Buffer << id;
  if (size >= 0)
    Buffer << size;
  if (alignment >= 0) {
    Buffer << "_" << alignment;
  }
  return ManglingError::Success;
}

ManglingError Remangler::mangleConstrainedType(Node *node, unsigned depth) {
  if (node->getFirstChild()->getKind()
        == Node::Kind::DependentGenericParamType) {
    // Can be mangled without an introducer.
    return mangleDependentGenericParamIndex(node->getFirstChild(), depth + 1);
  } else {
    return mangle(node, depth + 1);
  }
}

ManglingError Remangler::mangleAssociatedType(Node *node, unsigned depth) {
  if (node->hasChildren()) {
    DEMANGLER_ASSERT(node->getNumChildren() == 1, node);
    return mangleProtocolListWithoutPrefix(*node->begin(), depth + 1);
  } else {
    Buffer << '_';
    return ManglingError::Success;
  }
}

ManglingError Remangler::mangleDeclContext(Node *node, unsigned depth) {
  return mangleSingleChildNode(node, depth + 1);
}

ManglingError Remangler::mangleExtension(Node *node, EntityContext &ctx,
                                         unsigned depth) {
  DEMANGLER_ASSERT(node->getNumChildren() == 2 || node->getNumChildren() == 3,
                   node);
  if (node->getNumChildren() == 3) {
    Buffer << 'e';
  } else {
    Buffer << 'E';
  }
  // module
  RETURN_IF_ERROR(mangleEntityContext(node->begin()[0], ctx, depth + 1));
  if (node->getNumChildren() == 3) {
    // generic sig
    RETURN_IF_ERROR(
        mangleDependentGenericSignature(node->begin()[2], depth + 1));
  }

  // context
  return mangleEntityContext(node->begin()[1], ctx, depth + 1);
}

ManglingError Remangler::mangleAnonymousContext(Node *node, EntityContext &ctx,
                                                unsigned depth) {
  RETURN_IF_ERROR(mangleEntityContext(node->getChild(1), ctx, depth + 1));

  // Since we can't change the old mangling, mangle an anonymous context by
  // introducing a private discriminator onto its child contexts.
  ctx.setAnonymousContextDiscriminator(node->getChild(0)->getText());

  return ManglingError::Success;
}

ManglingError Remangler::mangleModule(Node *node, EntityContext &ctx,
                                      unsigned depth) {
  SubstitutionEntry entry;
  if (trySubstitution(node, entry))
    return ManglingError::Success;

  // Module types get an M prefix, but module contexts don't.
  if (!ctx.isAsContext()) Buffer << 'M';
  RETURN_IF_ERROR(mangleIdentifier(node->getText(), OperatorKind::NotOperator));
  addSubstitution(entry);
  return ManglingError::Success;
}

ManglingError Remangler::mangleAssociatedTypeRef(Node *node, unsigned depth) {
  SubstitutionEntry entry;
  if (trySubstitution(node, entry))
    return ManglingError::Success;
  Buffer << "Q";
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1)); // type, identifier
  addSubstitution(entry);
  return ManglingError::Success;
}

ManglingError Remangler::mangleDependentMemberType(Node *node, unsigned depth) {
  Vector<Node *> members;
  Node *base = node;
  do {
    members.push_back(base, Factory);
    base = base->getFirstChild()->getFirstChild();
  } while (base->getKind() == Node::Kind::DependentMemberType);

  DEMANGLER_ASSERT(base->getKind() == Node::Kind::DependentGenericParamType &&
                       "dependent members not based on a generic param are "
                       "non-canonical and shouldn't need remangling",
                   base);
  DEMANGLER_ASSERT(members.size() >= 1, node);
  if (members.size() == 1) {
    Buffer << 'w';
    RETURN_IF_ERROR(mangleDependentGenericParamIndex(base, depth + 1));
    RETURN_IF_ERROR(mangle(members[0]->getChild(1), depth + 1));
  } else {
    Buffer << 'W';
    RETURN_IF_ERROR(mangleDependentGenericParamIndex(base, depth + 1));

    for (unsigned i = 1, n = members.size(); i <= n; ++i) {
      Node *member = members[n - i];
      RETURN_IF_ERROR(mangle(member->getChild(1), depth + 1));
    }
    Buffer << '_';
  }

  return ManglingError::Success;
}

ManglingError Remangler::mangleDependentAssociatedTypeRef(Node *node,
                                                          unsigned depth) {
  SubstitutionEntry entry;
  if (trySubstitution(node, entry))
    return ManglingError::Success;

  if (node->getNumChildren() > 1) {
    Buffer << 'P';
    RETURN_IF_ERROR(mangleProtocolWithoutPrefix(node->getChild(1), depth + 1));
  }
  RETURN_IF_ERROR(mangleIdentifier(node->getFirstChild(), depth + 1));

  addSubstitution(entry);
  return ManglingError::Success;
}

ManglingError Remangler::mangleDependentGenericParamIndex(Node *node,
                                                          unsigned depth) {
  auto paramDepth = node->getChild(0)->getIndex();
  auto index = node->getChild(1)->getIndex();

  if (paramDepth != 0) {
    Buffer << 'd';
    mangleIndex(paramDepth - 1);
    mangleIndex(index);
    return ManglingError::Success;
  }
  if (index != 0) {
    mangleIndex(index - 1);
    return ManglingError::Success;
  }

  // paramDepth == index == 0
  Buffer << 'x';
  return ManglingError::Success;
}

ManglingError Remangler::mangleDependentGenericParamType(Node *node,
                                                         unsigned depth) {
  if (node->getChild(0)->getIndex() == 0
      && node->getChild(1)->getIndex() == 0) {
    Buffer << 'x';
    return ManglingError::Success;
  }

  Buffer << 'q';
  return mangleDependentGenericParamIndex(node, depth + 1);
}

ManglingError Remangler::mangleIndex(Node *node, unsigned depth) {
  mangleIndex(node->getIndex());
  return ManglingError::Success;
}

ManglingError Remangler::mangleUnknownIndex(Node *node, unsigned depth) {
  // should not be reached in an arbitrary context
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleProtocol(Node *node, EntityContext &ctx,
                                        unsigned depth) {
  return mangleNominalType(node, 'P', ctx, depth + 1);
}

ManglingError Remangler::mangleProtocolWithoutPrefix(Node *node,
                                                     unsigned depth) {
  if (mangleStandardSubstitution(node))
    return ManglingError::Success;

  if (node->getKind() == Node::Kind::Type) {
    DEMANGLER_ASSERT(node->getNumChildren() == 1, node);
    node = node->begin()[0];
  }

  DEMANGLER_ASSERT(node->getKind() == Node::Kind::Protocol, node);
  EntityContext ctx;
  return mangleNominalType(node, '\0', ctx, depth + 1);
}

ManglingError Remangler::mangleGenericArgs(Node *node, EntityContext &ctx,
                                           unsigned depth) {
  switch (node->getKind()) {
  case Node::Kind::Structure:
  case Node::Kind::Enum:
  case Node::Kind::Class: {
    NodePointer parentOrModule = node->getChild(0);
    RETURN_IF_ERROR(mangleGenericArgs(parentOrModule, ctx, depth + 1));

    // No generic arguments at this level
    Buffer << '_';
    break;
  }

  case Node::Kind::BoundGenericStructure:
  case Node::Kind::BoundGenericEnum:
  case Node::Kind::BoundGenericClass: {
    NodePointer unboundType = node->getChild(0);
    DEMANGLER_ASSERT(unboundType->getKind() == Node::Kind::Type, unboundType);
    NodePointer nominalType = unboundType->getChild(0);
    NodePointer parentOrModule = nominalType->getChild(0);
    RETURN_IF_ERROR(mangleGenericArgs(parentOrModule, ctx, depth + 1));

    RETURN_IF_ERROR(mangleTypeList(node->getChild(1), depth + 1));
    break;
  }

  case Node::Kind::AnonymousContext:
  case Node::Kind::Extension: {
    RETURN_IF_ERROR(mangleGenericArgs(node->getChild(1), ctx, depth + 1));
    break;
  }

  default:
    break;
  }

  return ManglingError::Success;
}

ManglingError Remangler::mangleAnyNominalType(Node *node, EntityContext &ctx,
                                              unsigned depth) {
  if (depth > Remangler::MaxDepth)
    return MANGLING_ERROR(ManglingError::TooComplex, node);

  if (isSpecialized(node)) {
    Buffer << 'G';

    auto unspec = getUnspecialized(node, Factory);
    if (!unspec.isSuccess())
      return unspec.error();
    NodePointer unboundType = unspec.result();

    RETURN_IF_ERROR(mangleAnyNominalType(unboundType, ctx, depth + 1));
    return mangleGenericArgs(node, ctx, depth + 1);
  }

  switch (node->getKind()) {
  case Node::Kind::Type:
    RETURN_IF_ERROR(mangleAnyNominalType(node->getChild(0), ctx, depth + 1));
    break;
  case Node::Kind::OtherNominalType:
    // Mangle unknown type kinds as structures since we can't change the old
    // mangling. Give the mangling an artificial "private discriminator" so that
    // clients who understand the old mangling know this is an unstable
    // mangled name.
    RETURN_IF_ERROR(
        mangleNominalType(node, 'V', ctx, depth + 1, "_UnknownTypeKind"));
    break;
  case Node::Kind::Structure:
    RETURN_IF_ERROR(mangleNominalType(node, 'V', ctx, depth + 1));
    break;
  case Node::Kind::Enum:
    RETURN_IF_ERROR(mangleNominalType(node, 'O', ctx, depth + 1));
    break;
  case Node::Kind::Class:
    RETURN_IF_ERROR(mangleNominalType(node, 'C', ctx, depth + 1));
    break;
  case Node::Kind::TypeAlias:
    RETURN_IF_ERROR(mangleNominalType(node, 'a', ctx, depth + 1));
    break;
  default:
    return MANGLING_ERROR(ManglingError::BadNominalTypeKind, node);
  }
  return ManglingError::Success;
}

ManglingError Remangler::mangleStructure(Node *node, EntityContext &ctx,
                                         unsigned depth) {
  return mangleAnyNominalType(node, ctx, depth + 1);
}

ManglingError Remangler::mangleEnum(Node *node, EntityContext &ctx,
                                    unsigned depth) {
  return mangleAnyNominalType(node, ctx, depth + 1);
}

ManglingError Remangler::mangleClass(Node *node, EntityContext &ctx,
                                     unsigned depth) {
  return mangleAnyNominalType(node, ctx, depth + 1);
}

ManglingError Remangler::mangleOtherNominalType(Node *node, EntityContext &ctx,
                                                unsigned depth) {
  return mangleAnyNominalType(node, ctx, depth + 1);
}

ManglingError
Remangler::mangleNominalType(Node *node, char kind, EntityContext &ctx,
                             unsigned depth,
                             StringRef artificialPrivateDiscriminator) {
  SubstitutionEntry entry;
  if (node->getKind() == Node::Kind::Type) {
    node = node->getChild(0);
  }
  if (trySubstitution(node, entry))
    return ManglingError::Success;
  RETURN_IF_ERROR(mangleNamedEntity(node, kind, "", ctx, depth + 1,
                                    artificialPrivateDiscriminator));
  addSubstitution(entry);
  return ManglingError::Success;
}

ManglingError Remangler::mangleBoundGenericClass(Node *node, unsigned depth) {
  EntityContext ctx;
  return mangleAnyNominalType(node, ctx, depth + 1);
}

ManglingError Remangler::mangleBoundGenericStructure(Node *node,
                                                     unsigned depth) {
  EntityContext ctx;
  return mangleAnyNominalType(node, ctx, depth + 1);
}

ManglingError Remangler::mangleBoundGenericEnum(Node *node, unsigned depth) {
  EntityContext ctx;
  return mangleAnyNominalType(node, ctx, depth + 1);
}

ManglingError Remangler::mangleBoundGenericOtherNominalType(Node *node,
                                                            unsigned depth) {
  EntityContext ctx;
  return mangleAnyNominalType(node, ctx, depth + 1);
}

ManglingError Remangler::mangleBoundGenericProtocol(Node *node,
                                                    unsigned depth) {
  EntityContext ctx;
  return mangleAnyNominalType(node, ctx, depth + 1);
}

ManglingError Remangler::mangleBoundGenericTypeAlias(Node *node,
                                                     unsigned depth) {
  EntityContext ctx;
  return mangleAnyNominalType(node, ctx, depth + 1);
}

ManglingError Remangler::mangleBoundGenericFunction(Node *node,
                                                    unsigned depth) {
  EntityContext ctx;
  // Not really a nominal type, but it works for functions, too.
  return mangleAnyNominalType(node, ctx, depth + 1);
}

ManglingError Remangler::mangleTypeList(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1)); // all types
  Buffer << '_';
  return ManglingError::Success;
}

ManglingError Remangler::mangleLabelList(Node *node, unsigned depth) {
  if (node->getNumChildren() == 0) {
    Buffer << 'y';
    return ManglingError::Success;
  } else {
    return mangleChildNodes(node, depth + 1);
  }
}

ManglingError
Remangler::mangleReflectionMetadataBuiltinDescriptor(Node *node,
                                                     unsigned depth) {
  Buffer << "MRb";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleReflectionMetadataFieldDescriptor(Node *node, unsigned depth) {
  Buffer << "MRf";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleReflectionMetadataAssocTypeDescriptor(Node *node,
                                                       unsigned depth) {
  Buffer << "MRa";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleReflectionMetadataSuperclassDescriptor(Node *node,
                                                        unsigned depth) {
  Buffer << "MRc";
  return ManglingError::Success;
}

ManglingError Remangler::mangleGenericTypeParamDecl(Node *node,
                                                    unsigned depth) {
  // todo
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleCurryThunk(Node *node, unsigned depth) {
  // ###TODO: Are these errors?!
  Buffer << "<curry-thunk>";
  return ManglingError::Success;
}

ManglingError Remangler::mangleDispatchThunk(Node *node, unsigned depth) {
  Buffer << "<dispatch-thunk>";
  return ManglingError::Success;
}

ManglingError Remangler::mangleMethodDescriptor(Node *node, unsigned depth) {
  Buffer << "<method-descriptor>";
  return ManglingError::Success;
}

ManglingError Remangler::mangleMethodLookupFunction(Node *node,
                                                    unsigned depth) {
  Buffer << "<method-lookup-function>";
  return ManglingError::Success;
}

ManglingError Remangler::mangleObjCMetadataUpdateFunction(Node *node,
                                                          unsigned depth) {
  Buffer << "<objc-metadata-update-function>";
  return ManglingError::Success;
}

ManglingError Remangler::mangleObjCResilientClassStub(Node *node,
                                                      unsigned depth) {
  Buffer << "<objc-resilient-class-stub>";
  return ManglingError::Success;
}

ManglingError Remangler::mangleFullObjCResilientClassStub(Node *node,
                                                          unsigned depth) {
  Buffer << "<full-objc-resilient-class-stub>";
  return ManglingError::Success;
}

ManglingError Remangler::mangleEmptyList(Node *node, unsigned depth) {
  Buffer << "<empty>";
  return ManglingError::Success;
}

ManglingError Remangler::mangleFirstElementMarker(Node *node, unsigned depth) {
  Buffer << "<first>";
  return ManglingError::Success;
}

ManglingError Remangler::mangleVariadicMarker(Node *node, unsigned depth) {
  // Handled in mangleTuple

  // ###TODO: Is this an error?
  return ManglingError::Success;
}

ManglingError Remangler::mangleOutlinedCopy(Node *node, unsigned depth) {
  Buffer << "Wy";
  return mangleChildNodes(node, depth + 1);
}

ManglingError Remangler::mangleOutlinedConsume(Node *node, unsigned depth) {
  Buffer << "We";
  return mangleChildNodes(node, depth + 1);
}

ManglingError Remangler::mangleOutlinedRetain(Node *node, unsigned depth) {
  Buffer << "Wr";
  return mangleSingleChildNode(node, depth + 1);
}

ManglingError Remangler::mangleOutlinedRelease(Node *node, unsigned depth) {
  Buffer << "Ws";
  return mangleSingleChildNode(node, depth + 1);
}

ManglingError Remangler::mangleOutlinedInitializeWithTake(Node *node,
                                                          unsigned depth) {
  Buffer << "Wb";
  return mangleSingleChildNode(node, depth + 1);
}

ManglingError Remangler::mangleOutlinedInitializeWithCopy(Node *node,
                                                          unsigned depth) {
  Buffer << "Wc";
  return mangleSingleChildNode(node, depth + 1);
}

ManglingError Remangler::mangleOutlinedAssignWithTake(Node *node,
                                                      unsigned depth) {
  Buffer << "Wd";
  return mangleSingleChildNode(node, depth + 1);
}

ManglingError Remangler::mangleOutlinedAssignWithCopy(Node *node,
                                                      unsigned depth) {
  Buffer << "Wf";
  return mangleSingleChildNode(node, depth + 1);
}

ManglingError Remangler::mangleOutlinedDestroy(Node *node, unsigned depth) {
  Buffer << "Wh";
  return mangleSingleChildNode(node, depth + 1);
}

ManglingError Remangler::mangleOutlinedVariable(Node *node, unsigned depth) {
  Buffer << "Tv" << node->getIndex();
  return mangleSingleChildNode(node, depth + 1);
}

ManglingError Remangler::mangleOutlinedReadOnlyObject(Node *node, unsigned depth) {
  Buffer << "Tv" << node->getIndex() << 'r';
  return mangleSingleChildNode(node, depth + 1);
}

ManglingError Remangler::mangleOutlinedBridgedMethod(Node *node,
                                                     unsigned depth) {
  Buffer << "Te" << node->getText();
  return mangleSingleChildNode(node, depth + 1);
}

ManglingError Remangler::mangleCoroutineContinuationPrototype(Node *node,
                                                              unsigned depth) {
  Buffer << "TC";
  return mangleChildNodes(node, depth + 1);
}

ManglingError Remangler::mangleKeyPathGetterThunkHelper(Node *node,
                                                        unsigned depth) {
  Buffer << "TK";
  return mangleChildNodes(node, depth + 1);
}

ManglingError Remangler::mangleKeyPathSetterThunkHelper(Node *node,
                                                        unsigned depth) {
  Buffer << "Tk";
  return mangleChildNodes(node, depth + 1);
}

ManglingError Remangler::mangleKeyPathEqualsThunkHelper(Node *node,
                                                        unsigned depth) {
  Buffer << "TH";
  return mangleChildNodes(node, depth + 1);
}

ManglingError Remangler::mangleKeyPathHashThunkHelper(Node *node,
                                                      unsigned depth) {
  Buffer << "Th";
  return mangleChildNodes(node, depth + 1);
}

ManglingError Remangler::mangleProtocolListWithClass(Node *node,
                                                     unsigned depth) {
  Buffer << "Xc";
  RETURN_IF_ERROR(mangleChildNode(node, 1, depth + 1));
  return mangleProtocolListWithoutPrefix(node->getChild(0), depth + 1);
}

ManglingError Remangler::mangleProtocolListWithAnyObject(Node *node,
                                                         unsigned depth) {
  Node *P = Factory.createNode(Node::Kind::Protocol);
  P->addChild(Factory.createNode(Node::Kind::Module, "Swift"), Factory);
  P->addChild(Factory.createNode(Node::Kind::Identifier, "AnyObject"), Factory);
  Buffer << "P";
  return mangleProtocolListWithoutPrefix(node->getChild(0), depth + 1,
                                         /*additionalProto*/ P);
}

ManglingError Remangler::mangleVTableThunk(Node *node, unsigned depth) {
  Buffer << "TV";
  return mangleChildNodes(node, depth + 1);
}

ManglingError Remangler::mangleSILBoxTypeWithLayout(Node *node,
                                                    unsigned depth) {
  DEMANGLER_ASSERT(node->getKind() == Node::Kind::SILBoxTypeWithLayout, node);
  DEMANGLER_ASSERT(node->getNumChildren() == 1 || node->getNumChildren() == 3,
                   node);
  Buffer << "XB";
  auto layout = node->getChild(0);
  DEMANGLER_ASSERT(layout->getKind() == Node::Kind::SILBoxLayout, layout);
  NodePointer genericArgs = nullptr;
  if (node->getNumChildren() == 3) {
    NodePointer signature = node->getChild(1);
    DEMANGLER_ASSERT(signature->getKind() ==
                         Node::Kind::DependentGenericSignature,
                     signature);
    genericArgs = node->getChild(2);
    DEMANGLER_ASSERT(genericArgs->getKind() == Node::Kind::TypeList,
                     genericArgs);

    Buffer << 'G';
    RETURN_IF_ERROR(mangleDependentGenericSignature(signature, depth + 1));
  }
  RETURN_IF_ERROR(mangleSILBoxLayout(layout, depth + 1));
  if (genericArgs) {
    for (unsigned i = 0; i < genericArgs->getNumChildren(); ++i) {
      auto type = genericArgs->getChild(i);
      DEMANGLER_ASSERT(genericArgs->getKind() == Node::Kind::Type, genericArgs);
      RETURN_IF_ERROR(mangleType(type, depth + 1));
    }
    Buffer << '_';
  }

  return ManglingError::Success;
}

ManglingError Remangler::mangleSILBoxLayout(Node *node, unsigned depth) {
  DEMANGLER_ASSERT(node->getKind() == Node::Kind::SILBoxLayout, node);
  for (unsigned i = 0; i < node->getNumChildren(); ++i) {
    DEMANGLER_ASSERT(node->getKind() == Node::Kind::SILBoxImmutableField ||
                         node->getKind() == Node::Kind::SILBoxMutableField,
                     node);
    RETURN_IF_ERROR(mangle(node->getChild(i), depth + 1));
  }
  Buffer << '_';

  return ManglingError::Success;
}

ManglingError Remangler::mangleSILBoxMutableField(Node *node, unsigned depth) {
  Buffer << 'm';
  DEMANGLER_ASSERT(node->getNumChildren() == 1 &&
                       node->getChild(0)->getKind() == Node::Kind::Type,
                   node);
  return mangleType(node->getChild(0), depth + 1);
}

ManglingError Remangler::mangleSILBoxImmutableField(Node *node,
                                                    unsigned depth) {
  Buffer << 'i';
  DEMANGLER_ASSERT(node->getNumChildren() == 1 &&
                       node->getChild(0)->getKind() == Node::Kind::Type,
                   node);
  return mangleType(node->getChild(0), depth + 1);
}

ManglingError Remangler::mangleAssocTypePath(Node *node, unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleModuleDescriptor(Node *node, unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleExtensionDescriptor(Node *node, unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleAnonymousDescriptor(Node *node, unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleAssociatedTypeGenericParamRef(Node *node,
                                                             unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleTypeSymbolicReference(Node *node,
                                                     EntityContext &,
                                                     unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleProtocolSymbolicReference(Node *node,
                                                         EntityContext &,
                                                         unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError
Remangler::mangleOpaqueTypeDescriptorSymbolicReference(Node *node,
                                                       unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleSugaredOptional(Node *node, unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleSugaredArray(Node *node, unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleSugaredDictionary(Node *node, unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleSugaredParen(Node *node, unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleOpaqueReturnType(Node *node, unsigned depth) {
  if (node->hasChildren()
      && node->getFirstChild()->getKind() == Node::Kind::OpaqueReturnTypeIndex){
    Buffer << "QU";
    mangleIndex(node->getFirstChild()->getIndex());
    return ManglingError::Success;
  }

  Buffer << "Qu";
  return ManglingError::Success;
}
ManglingError Remangler::mangleOpaqueReturnTypeIndex(Node *node, unsigned depth) {
  return ManglingError::WrongNodeType;
}
ManglingError Remangler::mangleOpaqueReturnTypeParent(Node *node, unsigned depth) {
  return ManglingError::WrongNodeType;
}
ManglingError Remangler::mangleOpaqueReturnTypeOf(Node *node,
                                                  EntityContext &ctx,
                                                  unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}
ManglingError Remangler::mangleOpaqueType(Node *node, unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}
ManglingError Remangler::mangleOpaqueTypeDescriptor(Node *node,
                                                    unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}
ManglingError Remangler::mangleOpaqueTypeDescriptorRecord(Node *node,
                                                          unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}
ManglingError Remangler::mangleOpaqueTypeDescriptorAccessor(Node *node,
                                                            unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}
ManglingError
Remangler::mangleOpaqueTypeDescriptorAccessorImpl(Node *node, unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}
ManglingError Remangler::mangleOpaqueTypeDescriptorAccessorKey(Node *node,
                                                               unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}
ManglingError Remangler::mangleOpaqueTypeDescriptorAccessorVar(Node *node,
                                                               unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}
ManglingError Remangler::mangleAccessorFunctionReference(Node *node,
                                                         unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}
ManglingError Remangler::mangleMetadataInstantiationCache(Node *node,
                                                          unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}
ManglingError Remangler::mangleGlobalVariableOnceToken(Node *node,
                                                       unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}
ManglingError Remangler::mangleGlobalVariableOnceFunction(Node *node,
                                                          unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}
ManglingError Remangler::mangleGlobalVariableOnceDeclList(Node *node,
                                                          unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}
ManglingError
Remangler::manglePredefinedObjCAsyncCompletionHandlerImpl(Node *node,
                                                          unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}
ManglingError Remangler::mangleObjCAsyncCompletionHandlerImpl(Node *node,
                                                              unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}
ManglingError Remangler::mangleConstrainedExistential(Node *node,
                                                      unsigned int depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}
ManglingError
Remangler::mangleConstrainedExistentialRequirementList(Node *node,
                                                       unsigned int depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}
ManglingError Remangler::mangleConstrainedExistentialSelf(Node *node,
                                                          unsigned int depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}
ManglingError Remangler::mangleUniquable(Node *node, unsigned int depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}
ManglingError Remangler::mangleExtendedExistentialTypeShape(Node *node,
                                                     unsigned int depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}
ManglingError Remangler::mangleSymbolicExtendedExistentialType(Node *node,
                                                     unsigned int depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}
ManglingError Remangler::
mangleUniqueExtendedExistentialTypeShapeSymbolicReference(Node *node,
                                                     unsigned int depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}
ManglingError Remangler::
mangleNonUniqueExtendedExistentialTypeShapeSymbolicReference(Node *node,
                                                     unsigned int depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}
ManglingError
Remangler::mangleCanonicalSpecializedGenericMetaclass(Node *node,
                                                      unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1)); // type
  Buffer << "MM";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleCanonicalSpecializedGenericTypeMetadataAccessFunction(
    Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "Mb";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleNoncanonicalSpecializedGenericTypeMetadata(Node *node,
                                                            unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "MN";
  return ManglingError::Success;
}

ManglingError Remangler::mangleNoncanonicalSpecializedGenericTypeMetadataCache(
    Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "MJ";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleCanonicalPrespecializedGenericTypeCachingOnceToken(
    Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "Mz";
  return ManglingError::Success;
}

/// The top-level interface to the remangler.
ManglingErrorOr<std::string>
Demangle::mangleNodeOld(NodePointer node) {
  if (!node) return std::string();

  NodeFactory Factory;
  Remangler remangler(Factory);
  ManglingError err = remangler.mangle(node, 0);
  if (!err.isSuccess())
    return err;

  return remangler.str();
}

ManglingErrorOr<llvm::StringRef>
Demangle::mangleNodeOld(NodePointer node, NodeFactory &Factory) {
  if (!node) return llvm::StringRef();

  Remangler remangler(Factory);
  ManglingError err = remangler.mangle(node, 0);
  if (!err.isSuccess())
    return err;

  return remangler.getBufferStr();
}

ManglingErrorOr<const char *>
Demangle::mangleNodeAsObjcCString(NodePointer node,
                                  NodeFactory &Factory) {
  DEMANGLER_ASSERT(node, node);

  Remangler remangler(Factory);
  remangler.append("_Tt");
  ManglingError err = remangler.mangle(node, 0);
  if (!err.isSuccess())
    return err;
  remangler.append(StringRef("_", 2)); // Include the trailing 0 char.

  return remangler.getBufferStr().data();
}

ManglingError Remangler::mangleAccessibleFunctionRecord(Node *node,
                                                        unsigned depth) {
  Buffer << "HF";
  return ManglingError::Success;
}

ManglingError Remangler::mangleBackDeploymentThunk(Node *node, unsigned depth) {
  Buffer << "Twb";
  return ManglingError::Success;
}

ManglingError Remangler::mangleBackDeploymentFallback(Node *node,
                                                      unsigned depth) {
  Buffer << "TwB";
  return ManglingError::Success;
}

ManglingError Remangler::mangleHasSymbolQuery(Node *node, unsigned depth) {
  Buffer << "TwS";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleRuntimeDiscoverableAttributeRecord(Node *node,
                                                    unsigned depth) {
  Buffer << "Ha";
  return ManglingError::Success;
}

ManglingError Remangler::mangleRuntimeAttributeGenerator(Node *node,
                                                         EntityContext &ctx,
                                                         unsigned depth) {
  return mangleSimpleEntity(node, 'I', "a", ctx, depth + 1);
}
