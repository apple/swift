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

#include "swift/Demangling/Demangler.h"
#include "swift/Demangling/Punycode.h"
#include "swift/Demangling/ManglingUtils.h"
#include "swift/AST/Ownership.h"
#include "swift/Strings.h"
#include "RemanglerBase.h"
#include <cstdio>
#include <cstdlib>

using namespace swift;
using namespace Demangle;

[[noreturn]]
static void unreachable(const char *Message) {
  fprintf(stderr, "fatal error: %s\n", Message);
  std::abort();
}

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

    void mangle(Node *node, unsigned depth) {
      if (depth > Remangler::MaxDepth) {
        // FIXME: error handling needs doing properly (rdar://79725187)
        unreachable("too complex to remangle");
      }

      switch (node->getKind()) {
#define NODE(ID)                                                               \
  case Node::Kind::ID:                                                         \
    return mangle##ID(node, depth);
#include "swift/Demangling/DemangleNodes.def"
      }
      unreachable("bad demangling tree node");
    }

    void mangleGenericArgs(Node *node, EntityContext &ctx, unsigned depth);
    void mangleAnyNominalType(Node *node, EntityContext &ctx, unsigned depth);

#define NODE(ID) void mangle##ID(Node *node, unsigned depth);
#define CONTEXT_NODE(ID)                                                       \
  void mangle##ID(Node *node, unsigned depth);                                 \
  void mangle##ID(Node *node, EntityContext &ctx, unsigned depth);
#include "swift/Demangling/DemangleNodes.def"

    void mangleIndex(Node::IndexType index);
    void mangleIdentifier(StringRef name, OperatorKind operatorKind);
    void mangleAccessor(Node *storageNode, StringRef accessorCode,
                        EntityContext &ctx, unsigned depth);

    void mangleChildNodes(Node *node, unsigned depth) {
      mangleNodes(node->begin(), node->end(), depth);
    }
    void mangleNodes(Node::iterator i, Node::iterator e, unsigned depth) {
      for (; i != e; ++i) {
        mangle(*i, depth);
      }
    }
    void mangleSingleChildNode(Node *node, unsigned depth) {
      assert(node->getNumChildren() == 1);
      mangle(*node->begin(), depth);
    }
    void mangleChildNode(Node *node, unsigned index, unsigned depth) {
      assert(index < node->getNumChildren());
      mangle(node->begin()[index], depth);
    }

    void mangleSimpleEntity(Node *node, char basicKind, StringRef entityKind,
                            EntityContext &ctx, unsigned depth);
    void mangleNamedEntity(Node *node, char basicKind, StringRef entityKind,
                           EntityContext &ctx, unsigned depth,
                           StringRef ArtificialPrivateDiscriminator = {});
    void mangleTypedEntity(Node *node, char basicKind, StringRef entityKind,
                           EntityContext &ctx, unsigned depth);
    void mangleNamedAndTypedEntity(Node *node, char basicKind,
                                   StringRef entityKind, EntityContext &ctx,
                                   unsigned depth);
    void mangleNominalType(Node *node, char basicKind, EntityContext &ctx,
                           unsigned depth,
                           StringRef ArtificialPrivateDiscriminator = {});

    void mangleProtocolWithoutPrefix(Node *node, unsigned depth);
    void mangleProtocolListWithoutPrefix(Node *node, unsigned depth,
                                         Node *additionalProto = nullptr);

    void mangleEntityContext(Node *node, EntityContext &ctx, unsigned depth);
    void mangleEntityType(Node *node, EntityContext &ctx, unsigned depth);
    void mangleEntityGenericType(Node *node, EntityContext &ctx);

    bool trySubstitution(Node *node, SubstitutionEntry &entry);
    bool mangleStandardSubstitution(Node *node);

    void mangleDependentGenericParamIndex(Node *node, unsigned depth);
    void mangleConstrainedType(Node *node, unsigned depth);
  };
} // end anonymous namespace

#define NODE(ID)
#define CONTEXT_NODE(ID)                                                       \
  void Remangler::mangle##ID(Node *node, unsigned depth) {                     \
    EntityContext ctx;                                                         \
    mangle##ID(node, ctx, depth);                                              \
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
};

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

void Remangler::mangleIdentifier(Node *node, unsigned depth) {
  mangleIdentifier(node->getText(), OperatorKind::NotOperator);
}
void Remangler::manglePrefixOperator(Node *node, unsigned depth) {
  mangleIdentifier(node->getText(), OperatorKind::Prefix);
}
void Remangler::manglePostfixOperator(Node *node, unsigned depth) {
  mangleIdentifier(node->getText(), OperatorKind::Postfix);
}
void Remangler::mangleInfixOperator(Node *node, unsigned depth) {
  mangleIdentifier(node->getText(), OperatorKind::Infix);
}
void Remangler::mangleIdentifier(StringRef ident, OperatorKind operatorKind) {
  // Mangle normal identifiers as
  //   count identifier-char+
  // where the count is the number of characters in the identifier,
  // and where individual identifier characters represent themselves.
  if (operatorKind == OperatorKind::NotOperator) {
    Buffer << ident.size() << ident;
    return;
  }

  // Mangle operator identifiers as
  //   operator ::= 'o' operator-fixity count operator-char+
  //   operator-fixity ::= 'p' // prefix
  //   operator-fixity ::= 'P' // postfix
  //   operator-fixity ::= 'i' // infix
  // where the count is the number of characters in the operator,
  // and where the individual operator characters are translated.
  Buffer << 'o';
  switch (operatorKind) {
    case OperatorKind::NotOperator: unreachable("invalid");
    case OperatorKind::Infix: Buffer << 'i'; break;
    case OperatorKind::Prefix: Buffer << 'p'; break;
    case OperatorKind::Postfix: Buffer << 'P'; break;
  }

  // Mangle ASCII operators directly.
  Buffer << ident.size();
  for (char ch : ident) {
    Buffer << Mangle::translateOperatorChar(ch);
  }
}

void Remangler::mangleNumber(Node *node, unsigned depth) {
  mangleIndex(node->getIndex());
}
void Remangler::mangleIndex(Node::IndexType value) {
  if (value == 0) {
    Buffer << '_';
  } else {
    Buffer << (value - 1) << '_';
  }
}

void Remangler::mangleGlobal(Node *node, unsigned depth) {
  Buffer << "_T";
  mangleChildNodes(node, depth + 1);
}

void Remangler::mangleSuffix(Node *node, unsigned depth) {
  // Just add the suffix back on.
  Buffer << node->getText();
}

void Remangler::mangleGenericSpecialization(Node *node, unsigned depth) {
  unreachable("unsupported");
}

void Remangler::mangleGenericSpecializationPrespecialized(Node *node,
                                                          unsigned depth) {
  unreachable("unsupported");
}
void Remangler::mangleGenericSpecializationNotReAbstracted(Node *node,
                                                           unsigned depth) {
  unreachable("unsupported");
}
void Remangler::mangleGenericSpecializationInResilienceDomain(Node *node,
                                                              unsigned depth) {
  unreachable("unsupported");
}

void Remangler::mangleInlinedGenericFunction(Node *node, unsigned depth) {
  unreachable("unsupported");
}

void Remangler::mangleGenericPartialSpecialization(Node *node, unsigned depth) {
  unreachable("unsupported");
}

void Remangler::mangleGenericPartialSpecializationNotReAbstracted(
    Node *node, unsigned depth) {
  unreachable("unsupported");
}

void Remangler::mangleGenericSpecializationParam(Node *node, unsigned depth) {
  unreachable("unsupported");
}

void Remangler::mangleFunctionSignatureSpecialization(Node *node,
                                                      unsigned depth) {
  unreachable("unsupported");
}

void Remangler::mangleSpecializationPassID(Node *node, unsigned depth) {
  unreachable("unsupported");
}

void Remangler::mangleIsSerialized(Node *node, unsigned depth) {
  Buffer << "q";
}

void Remangler::mangleFunctionSignatureSpecializationReturn(Node *node,
                                                            unsigned depth) {
  unreachable("unsupported");
}

void Remangler::mangleFunctionSignatureSpecializationParam(Node *node,
                                                           unsigned depth) {
  unreachable("unsupported");
}

void Remangler::mangleFunctionSignatureSpecializationParamPayload(
    Node *node, unsigned depth) {
  // This should never be called since mangling parameter payloads require
  // knowing what the parameter kind is.
  unreachable("This should never be called");
}

void Remangler::mangleFunctionSignatureSpecializationParamKind(Node *node,
                                                               unsigned depth) {
  // This should never be called since mangling parameter kinds have influence
  // on the payloads.
  unreachable("This should never be called");
}

void Remangler::mangleRetroactiveConformance(Node *node, unsigned depth) {
  unreachable("Retroactive conformances aren't in the old mangling");
}

void Remangler::mangleProtocolConformanceRefInTypeModule(Node *node,
                                                         unsigned depth) {
  unreachable("Protocol conformance references aren't in the old mangling");
}

void Remangler::mangleProtocolConformanceRefInProtocolModule(Node *node,
                                                             unsigned depth) {
  unreachable("Protocol conformance references aren't in the old mangling");
}

void Remangler::mangleProtocolConformanceRefInOtherModule(Node *node,
                                                          unsigned depth) {
  unreachable("Protocol conformance references aren't in the old mangling");
}

void Remangler::mangleConcreteProtocolConformance(Node *node, unsigned depth) {
  unreachable("Concrete conformances aren't in the old mangling");
}

void Remangler::mangleAnyProtocolConformanceList(Node *node, unsigned depth) {
  unreachable("Conformance lists aren't in the old mangling");
}

void Remangler::mangleDependentAssociatedConformance(Node *node,
                                                     unsigned depth) {
  unreachable("Dependent associated conformances aren't in the old mangling");
}

void Remangler::mangleDependentProtocolConformanceRoot(Node *node,
                                                       unsigned depth) {
  unreachable("Dependent conformances aren't in the old mangling");
}

void Remangler::mangleDependentProtocolConformanceInherited(Node *node,
                                                            unsigned depth) {
  unreachable("Dependent conformances aren't in the old mangling");
}

void Remangler::mangleDependentProtocolConformanceAssociated(Node *node,
                                                             unsigned depth) {
  unreachable("Dependent conformances aren't in the old mangling");
}

void Remangler::mangleProtocolConformance(Node *node, unsigned depth) {
  // type, protocol name, context
  assert(node->getNumChildren() == 3);
  mangleChildNode(node, 0, depth + 1);
  mangleProtocolWithoutPrefix(node->begin()[1], depth + 1);
  mangleChildNode(node, 2, depth + 1);
}

void Remangler::mangleObjCAttribute(Node *node, unsigned depth) {
  Buffer << "To";
}

void Remangler::mangleNonObjCAttribute(Node *node, unsigned depth) {
  Buffer << "TO";
}

void Remangler::mangleDirectMethodReferenceAttribute(Node *node,
                                                     unsigned depth) {
  Buffer << "Td";
}

void Remangler::mangleDynamicAttribute(Node *node, unsigned depth) {
  Buffer << "TD";
}

void Remangler::mangleVTableAttribute(Node *node, unsigned depth) {
  Buffer << "TV";
}

void Remangler::mangleGenericTypeMetadataPattern(Node *node, unsigned depth) {
  Buffer << "MP";
  mangleSingleChildNode(node, depth + 1); // type
}

void Remangler::mangleTypeMetadataAccessFunction(Node *node, unsigned depth) {
  Buffer << "Ma";
  mangleSingleChildNode(node, depth + 1); // type
}

void Remangler::mangleTypeMetadataInstantiationCache(Node *node,
                                                     unsigned depth) {
  Buffer << "MI";
  mangleSingleChildNode(node, depth + 1); // type
}

void Remangler::mangleTypeMetadataInstantiationFunction(Node *node,
                                                        unsigned depth) {
  Buffer << "Mi";
  mangleSingleChildNode(node, depth + 1); // type
}

void Remangler::mangleTypeMetadataSingletonInitializationCache(Node *node,
                                                               unsigned depth) {
  Buffer << "Ml";
  mangleSingleChildNode(node, depth + 1); // type
}

void Remangler::mangleTypeMetadataCompletionFunction(Node *node,
                                                     unsigned depth) {
  Buffer << "Mr";
  mangleSingleChildNode(node, depth + 1); // type
}

void Remangler::mangleTypeMetadataDemanglingCache(Node *node, unsigned depth) {
  unreachable("not supported");
}

void Remangler::mangleTypeMetadataLazyCache(Node *node, unsigned depth) {
  Buffer << "ML";
  mangleSingleChildNode(node, depth + 1); // type
}

void Remangler::mangleMetaclass(Node *node, unsigned depth) {
  Buffer << "Mm";
  mangleSingleChildNode(node, depth + 1); // type
}

void Remangler::mangleClassMetadataBaseOffset(Node *node, unsigned depth) {
  Buffer << "Mo";
  mangleSingleChildNode(node, depth + 1); // type
}

void Remangler::mangleNominalTypeDescriptor(Node *node, unsigned depth) {
  Buffer << "Mn";
  mangleSingleChildNode(node, depth + 1); // type
}

void Remangler::manglePropertyDescriptor(Node *node, unsigned depth) {
  unreachable("not supported");
}

void Remangler::mangleTypeMetadata(Node *node, unsigned depth) {
  Buffer << "M";
  mangleSingleChildNode(node, depth + 1); // type
}

void Remangler::mangleFullTypeMetadata(Node *node, unsigned depth) {
  Buffer << "Mf";
  mangleChildNodes(node, depth + 1); // type
}

void Remangler::mangleProtocolDescriptor(Node *node, unsigned depth) {
  Buffer << "Mp";
  mangleProtocolWithoutPrefix(node->begin()[0], depth + 1);
}

void Remangler::mangleProtocolRequirementsBaseDescriptor(Node *node,
                                                         unsigned depth) {
  Buffer << "<protocol-requirements-base-descriptor>";
}

void Remangler::mangleProtocolWitnessTablePattern(Node *node, unsigned depth) {
  unreachable("todo");
}

void Remangler::mangleProtocolConformanceDescriptor(Node *node,
                                                    unsigned depth) {
  Buffer << "Mc";
  mangleProtocolConformance(node->begin()[0], depth + 1);
}

void Remangler::mangleProtocolSelfConformanceDescriptor(Node *node,
                                                        unsigned depth) {
  Buffer << "MS";
  mangleProtocol(node->begin()[0], depth + 1);
}

void Remangler::manglePartialApplyForwarder(Node *node, unsigned depth) {
  Buffer << "PA";
  if (node->getNumChildren() == 1) {
    Buffer << "__T";
    mangleSingleChildNode(node, depth + 1); // global
  }
}

void Remangler::manglePartialApplyObjCForwarder(Node *node, unsigned depth) {
  Buffer << "PAo";
  if (node->getNumChildren() == 1) {
    Buffer << "__T";
    mangleSingleChildNode(node, depth + 1); // global
  }
}

void Remangler::mangleMergedFunction(Node *node, unsigned depth) {
  Buffer << "Tm";
}

void Remangler::mangleDynamicallyReplaceableFunctionImpl(Node *node,
                                                         unsigned depth) {
  Buffer << "TI";
}

void Remangler::mangleDynamicallyReplaceableFunctionKey(Node *node,
                                                        unsigned depth) {
  Buffer << "Tx";
}

void Remangler::mangleDynamicallyReplaceableFunctionVar(Node *node,
                                                        unsigned depth) {
  Buffer << "TX";
}

void Remangler::mangleAsyncAwaitResumePartialFunction(Node *node,
                                                      unsigned depth) {
  unreachable("unsupported");
}
void Remangler::mangleAsyncSuspendResumePartialFunction(Node *node,
                                                        unsigned depth) {
  unreachable("unsupported");
}

void Remangler::mangleDirectness(Node *node, unsigned depth) {
  auto getChar = [](Directness d) -> char {
    switch (d) {
    case Directness::Direct: return 'd';
    case Directness::Indirect: return 'i';
    }
    unreachable("bad directness kind");
  };
  Buffer << getChar(Directness(node->getIndex()));
}

void Remangler::mangleValueWitness(Node *node, unsigned depth) {
  const char *Code = nullptr;
  switch (ValueWitnessKind(node->getFirstChild()->getIndex())) {
#define VALUE_WITNESS(MANGLING, NAME) \
    case ValueWitnessKind::NAME: Code = #MANGLING; break;
#include "swift/Demangling/ValueWitnessMangling.def"
  }
  Buffer << 'w' << Code;
  mangleChildNode(node, 1, depth + 1); // type
}

void Remangler::mangleValueWitnessTable(Node *node, unsigned depth) {
  Buffer << "WV";
  mangleSingleChildNode(node, depth + 1); // type
}

void Remangler::mangleConcurrentFunctionType(Node *node, unsigned depth) {
  Buffer << "y";
}

void Remangler::mangleAsyncAnnotation(Node *node, unsigned depth) {
  Buffer << "Z";
}

void Remangler::mangleThrowsAnnotation(Node *node, unsigned depth) {
  Buffer << "z";
}

void Remangler::mangleDifferentiableFunctionType(Node *node, unsigned depth) {
  Buffer << "D";
  mangleChildNodes(node, depth + 1);
}

void Remangler::mangleGlobalActorFunctionType(Node *node, unsigned depth) {
  Buffer << "Y" << (char)node->getIndex(); // differentiability kind
}

void Remangler::mangleFieldOffset(Node *node, unsigned depth) {
  Buffer << "Wv";
  mangleChildNodes(node, depth + 1); // directness, entity
}

void Remangler::mangleEnumCase(Node *node, unsigned depth) {
  Buffer << "WC";
  mangleSingleChildNode(node, depth + 1); // enum case
}

void Remangler::mangleProtocolSelfConformanceWitnessTable(Node *node,
                                                          unsigned depth) {
  Buffer << "WS";
  mangleSingleChildNode(node, depth + 1); // protocol
}

void Remangler::mangleProtocolWitnessTable(Node *node, unsigned depth) {
  Buffer << "WP";
  mangleSingleChildNode(node, depth + 1); // protocol conformance
}

void Remangler::mangleGenericProtocolWitnessTable(Node *node, unsigned depth) {
  Buffer << "WG";
  mangleSingleChildNode(node, depth + 1); // protocol conformance
}

void Remangler::mangleResilientProtocolWitnessTable(Node *node,
                                                    unsigned depth) {
  unreachable("todo");
}

void Remangler::mangleGenericProtocolWitnessTableInstantiationFunction(
    Node *node, unsigned depth) {
  Buffer << "WI";
  mangleSingleChildNode(node, depth + 1); // protocol conformance
}

void Remangler::mangleProtocolWitnessTableAccessor(Node *node, unsigned depth) {
  Buffer << "Wa";
  mangleSingleChildNode(node, depth + 1); // protocol conformance
}

void Remangler::mangleLazyProtocolWitnessTableAccessor(Node *node,
                                                       unsigned depth) {
  Buffer << "Wl";
  mangleChildNodes(node, depth + 1); // type, protocol conformance
}

void Remangler::mangleLazyProtocolWitnessTableCacheVariable(Node *node,
                                                            unsigned depth) {
  Buffer << "WL";
  mangleChildNodes(node, depth + 1); // type, protocol conformance
}

void Remangler::mangleAssociatedTypeDescriptor(Node *node, unsigned depth) {
  Buffer << "<associated-type-descriptor>";
}

void Remangler::mangleAssociatedConformanceDescriptor(Node *node,
                                                      unsigned depth) {
  Buffer << "<associated-conformance-descriptor>";
}

void Remangler::mangleDefaultAssociatedConformanceAccessor(Node *node,
                                                           unsigned depth) {
  Buffer << "<default-associated-conformance-descriptor>";
}

void Remangler::mangleBaseConformanceDescriptor(Node *node, unsigned depth) {
  Buffer << "<base-conformance-descriptor>";
}

void Remangler::mangleAssociatedTypeMetadataAccessor(Node *node,
                                                     unsigned depth) {
  Buffer << "Wt";
  mangleChildNodes(node, depth + 1); // protocol conformance, identifier
}

void Remangler::mangleDefaultAssociatedTypeMetadataAccessor(Node *node,
                                                            unsigned depth) {
  Buffer << "<default-associated-type-metadata-accessor>";
}

void Remangler::mangleAssociatedTypeWitnessTableAccessor(Node *node,
                                                         unsigned depth) {
  Buffer << "WT";
  assert(node->getNumChildren() == 3);
  mangleChildNode(node, 0, depth + 1); // protocol conformance
  mangleChildNode(node, 1, depth + 1); // type
  mangleProtocolWithoutPrefix(node->begin()[2], depth + 1); // type
}

void Remangler::mangleBaseWitnessTableAccessor(Node *node, unsigned depth) {
  Buffer << "<base-witness-table-accessor>";
}

void Remangler::mangleReabstractionThunkHelper(Node *node, unsigned depth) {
  Buffer << "<reabstraction-thunk-helper>";
}

void Remangler::mangleReabstractionThunkHelperWithSelf(Node *node,
                                                       unsigned depth) {
  Buffer << "<reabstraction-thunk-helper-with-self>";
}

void Remangler::mangleReabstractionThunk(Node *node, unsigned depth) {
  Buffer << "<reabstraction-thunk>";
}

void Remangler::mangleReabstractionThunkHelperWithGlobalActor(Node *node,
                                                              unsigned depth) {
  Buffer << "<reabstraction-thunk-helper-with-global-actor>";
}

void Remangler::mangleAutoDiffFunction(Node *node, EntityContext &ctx,
                                       unsigned depth) {
  Buffer << "<autodiff-function>";
}

void Remangler::mangleAutoDiffDerivativeVTableThunk(Node *node,
                                                    unsigned depth) {
  Buffer << "<autodiff-derivative-vtable-thunk>";
}

void Remangler::mangleAutoDiffSelfReorderingReabstractionThunk(Node *node,
                                                               unsigned depth) {
  Buffer << "<autodiff-self-reordering-reabstraction-thunk>";
}

void Remangler::mangleAutoDiffSubsetParametersThunk(Node *node,
                                                    unsigned depth) {
  Buffer << "<autodiff-subset-parameters-thunk>";
}

void Remangler::mangleAutoDiffFunctionKind(Node *node, unsigned depth) {
  Buffer << "<autodiff-function-kind>";
}

void Remangler::mangleDifferentiabilityWitness(Node *node, unsigned depth) {
  Buffer << "<differentiability-witness>";
}

void Remangler::mangleIndexSubset(Node *node, unsigned depth) {
  Buffer << "<index-subset>";
}

void Remangler::mangleProtocolSelfConformanceWitness(Node *node,
                                                     unsigned depth) {
  Buffer << "TS";
  mangleSingleChildNode(node, depth + 1); // entity
}

void Remangler::mangleProtocolWitness(Node *node, unsigned depth) {
  Buffer << "TW";
  mangleChildNodes(node, depth + 1); // protocol conformance, entity
}

void Remangler::mangleFunction(Node *node, EntityContext &ctx, unsigned depth) {
  mangleNamedAndTypedEntity(node, 'F', "", ctx, depth + 1);
}

void Remangler::mangleVariable(Node *node, EntityContext &ctx, unsigned depth) {
  mangleNamedAndTypedEntity(node, 'v', "", ctx, depth + 1);
}

void Remangler::mangleSubscript(Node *node, EntityContext &ctx,
                                unsigned depth) {
  assert(node->getNumChildren() >= 2);
  Buffer << 'i';
  mangleEntityContext(node->begin()[0], ctx, depth + 1);
  if (node->getLastChild()->getKind() == Node::Kind::PrivateDeclName)
    mangle(node->getLastChild(), depth + 1);

  if (node->getNumChildren() >= 3
      && node->begin()[1]->getKind() == Node::Kind::LabelList) {
    auto LabelList = node->begin()[1];
    auto Type = node->begin()[2];
    mangleEntityType(applyParamLabels(LabelList, Type, Factory), ctx, depth + 1);
  } else {
    mangleEntityType(node->begin()[1], ctx, depth + 1);
  }
}

void Remangler::mangleAccessor(Node *storageNode, StringRef accessorCode,
                               EntityContext &ctx, unsigned depth) {
  Buffer << 'F';
  mangleEntityContext(storageNode->getChild(0), ctx, depth + 1);
  Buffer << accessorCode;

  auto mangleAccessorType = [&](unsigned TypeIndex) {
    auto LabelList = storageNode->getChild(TypeIndex);
    if (LabelList->getKind() == Node::Kind::LabelList) {
      auto Type = storageNode->getChild(TypeIndex + 1);
      mangleEntityType(applyParamLabels(LabelList, Type, Factory), ctx,
                       depth + 1);
    } else {
      mangleEntityType(storageNode->getChild(TypeIndex), ctx, depth + 1);
    }
  };

  switch (storageNode->getKind()) {
  case Demangle::Node::Kind::Variable: {
    mangleChildNode(storageNode, 1, depth + 1);
    mangleAccessorType(2);
    break;
  }

  case Demangle::Node::Kind::Subscript: {
    auto NumChildren = storageNode->getNumChildren();
    assert(NumChildren <= 4);

    auto PrivateName = storageNode->getChild(NumChildren - 1);
    if (PrivateName->getKind() == Node::Kind::PrivateDeclName)
      mangle(PrivateName, depth + 1);

    mangleIdentifier("subscript", OperatorKind::NotOperator);
    mangleAccessorType(1);
    break;
  }
  default:
      unreachable("Not a storage node");
  }
}

void Remangler::mangleInitializer(Node *node, EntityContext &ctx,
                                  unsigned depth) {
  mangleSimpleEntity(node, 'I', "i", ctx, depth + 1);
}

void Remangler::manglePropertyWrapperBackingInitializer(Node *node,
                                                        EntityContext &ctx,
                                                        unsigned depth) {
  mangleSimpleEntity(node, 'I', "P", ctx, depth + 1);
}

void Remangler::manglePropertyWrapperInitFromProjectedValue(Node *node,
                                                            EntityContext &ctx,
                                                            unsigned depth) {
  mangleSimpleEntity(node, 'I', "W", ctx, depth + 1);
}

void Remangler::mangleDefaultArgumentInitializer(Node *node, EntityContext &ctx,
                                                 unsigned depth) {
  mangleNamedEntity(node, 'I', "A", ctx, depth + 1);
}

void Remangler::mangleAsyncFunctionPointer(Node *node, unsigned depth) {
  Buffer << "Tu";
}

void Remangler::mangleDeallocator(Node *node, EntityContext &ctx,
                                  unsigned depth) {
  mangleSimpleEntity(node, 'F', "D", ctx, depth + 1);
}

void Remangler::mangleDestructor(Node *node, EntityContext &ctx,
                                 unsigned depth) {
  mangleSimpleEntity(node, 'F', "d", ctx, depth + 1);
}

void Remangler::mangleAllocator(Node *node, EntityContext &ctx,
                                unsigned depth) {
  mangleTypedEntity(node, 'F', "C", ctx, depth + 1);
}

void Remangler::mangleConstructor(Node *node, EntityContext &ctx,
                                  unsigned depth) {
  mangleTypedEntity(node, 'F', "c", ctx, depth + 1);
}

void Remangler::mangleIVarInitializer(Node *node, EntityContext &ctx,
                                      unsigned depth) {
  mangleSimpleEntity(node, 'F', "e", ctx, depth + 1);
}

void Remangler::mangleIVarDestroyer(Node *node, EntityContext &ctx,
                                    unsigned depth) {
  mangleSimpleEntity(node, 'F', "E", ctx, depth + 1);
}

void Remangler::mangleGetter(Node *node, EntityContext &ctx, unsigned depth) {
  mangleAccessor(node->getFirstChild(), "g", ctx, depth + 1);
}

void Remangler::mangleGlobalGetter(Node *node, EntityContext &ctx,
                                   unsigned depth) {
  mangleAccessor(node->getFirstChild(), "G", ctx, depth + 1);
}

void Remangler::mangleSetter(Node *node, EntityContext &ctx, unsigned depth) {
  mangleAccessor(node->getFirstChild(), "s", ctx, depth + 1);
}

void Remangler::mangleMaterializeForSet(Node *node, EntityContext &ctx,
                                        unsigned depth) {
  mangleAccessor(node->getFirstChild(), "m", ctx, depth + 1);
}

void Remangler::mangleWillSet(Node *node, EntityContext &ctx, unsigned depth) {
  mangleAccessor(node->getFirstChild(), "w", ctx, depth + 1);
}

void Remangler::mangleDidSet(Node *node, EntityContext &ctx, unsigned depth) {
  mangleAccessor(node->getFirstChild(), "W", ctx, depth + 1);
}

void Remangler::mangleOwningMutableAddressor(Node *node, EntityContext &ctx,
                                             unsigned depth) {
  mangleAccessor(node->getFirstChild(), "aO", ctx, depth + 1);
}

void Remangler::mangleNativeOwningMutableAddressor(Node *node,
                                                   EntityContext &ctx,
                                                   unsigned depth) {
  mangleAccessor(node->getFirstChild(), "ao", ctx, depth + 1);
}

void Remangler::mangleNativePinningMutableAddressor(Node *node,
                                                    EntityContext &ctx,
                                                    unsigned depth) {
  mangleAccessor(node->getFirstChild(), "ap", ctx, depth + 1);
}

void Remangler::mangleUnsafeMutableAddressor(Node *node, EntityContext &ctx,
                                             unsigned depth) {
  mangleAccessor(node->getFirstChild(), "au", ctx, depth + 1);
}

void Remangler::mangleOwningAddressor(Node *node, EntityContext &ctx,
                                      unsigned depth) {
  mangleAccessor(node->getFirstChild(), "lO", ctx, depth + 1);
}

void Remangler::mangleNativeOwningAddressor(Node *node, EntityContext &ctx,
                                            unsigned depth) {
  mangleAccessor(node->getFirstChild(), "lo", ctx, depth + 1);
}

void Remangler::mangleNativePinningAddressor(Node *node, EntityContext &ctx,
                                             unsigned depth) {
  mangleAccessor(node->getFirstChild(), "lp", ctx, depth + 1);
}

void Remangler::mangleUnsafeAddressor(Node *node, EntityContext &ctx,
                                      unsigned depth) {
  mangleAccessor(node->getFirstChild(), "lu", ctx, depth + 1);
}

void Remangler::mangleReadAccessor(Node *node, EntityContext &ctx,
                                   unsigned depth) {
  mangleAccessor(node->getFirstChild(), "r", ctx, depth + 1);
}

void Remangler::mangleModifyAccessor(Node *node, EntityContext &ctx,
                                     unsigned depth) {
  mangleAccessor(node->getFirstChild(), "M", ctx, depth + 1);
}

void Remangler::mangleExplicitClosure(Node *node, EntityContext &ctx,
                                      unsigned depth) {
  mangleNamedAndTypedEntity(node, 'F', "U", ctx, depth + 1); // name is index
}

void Remangler::mangleImplicitClosure(Node *node, EntityContext &ctx,
                                      unsigned depth) {
  mangleNamedAndTypedEntity(node, 'F', "u", ctx, depth + 1); // name is index
}

void Remangler::mangleStatic(Node *node, EntityContext &ctx, unsigned depth) {
  Buffer << 'Z';
  mangleEntityContext(node->getChild(0), ctx, depth + 1);
}

void Remangler::mangleSimpleEntity(Node *node, char basicKind,
                                   StringRef entityKind, EntityContext &ctx,
                                   unsigned depth) {
  assert(node->getNumChildren() == 1);
  Buffer << basicKind;
  mangleEntityContext(node->begin()[0], ctx, depth + 1);
  Buffer << entityKind;
}

void Remangler::mangleNamedEntity(Node *node, char basicKind,
                                  StringRef entityKind, EntityContext &ctx,
                                  unsigned depth,
                                  StringRef artificialPrivateDiscriminator) {
  assert(node->getNumChildren() == 2);
  if (basicKind != '\0') Buffer << basicKind;
  mangleEntityContext(node->begin()[0], ctx, depth + 1);
  Buffer << entityKind;
  
  auto privateDiscriminator = ctx.takeAnonymousContextDiscriminator();
  if (!privateDiscriminator.empty() && isdigit(privateDiscriminator[0]))
    privateDiscriminator = "_" + privateDiscriminator;
  if (!artificialPrivateDiscriminator.empty())
    privateDiscriminator.append(artificialPrivateDiscriminator.data(),
                                artificialPrivateDiscriminator.size());
  
  // Include the artificial private discriminator if one was given.
  auto name = node->getChild(1);
  if (!privateDiscriminator.empty()
      && name->getKind() == Node::Kind::Identifier) {
    Buffer << 'P';
    mangleIdentifier(privateDiscriminator, OperatorKind::NotOperator);
  }
  mangle(name, depth + 1);
}

void Remangler::mangleTypedEntity(Node *node, char basicKind,
                                  StringRef entityKind, EntityContext &ctx,
                                  unsigned depth) {
  assert(node->getNumChildren() == 2 || node->getNumChildren() == 3);
  Buffer << basicKind;
  mangleEntityContext(node->begin()[0], ctx, depth + 1);
  Buffer << entityKind;

  if (node->begin()[1]->getKind() == Node::Kind::LabelList) {
    auto LabelList = node->begin()[1];
    auto Type = node->begin()[2];
    mangleEntityType(applyParamLabels(LabelList, Type, Factory), ctx,
                     depth + 1);
  } else {
    mangleEntityType(node->begin()[1], ctx, depth + 1);
  }
}

void Remangler::mangleNamedAndTypedEntity(Node *node, char basicKind,
                                          StringRef entityKind,
                                          EntityContext &ctx, unsigned depth) {
  assert(node->getNumChildren() == 3 || node->getNumChildren() == 4);
  Buffer << basicKind;
  mangleEntityContext(node->begin()[0], ctx, depth + 1);
  Buffer << entityKind;
  mangleChildNode(node, 1, depth + 1); // decl name / index

  if (node->begin()[2]->getKind() == Node::Kind::LabelList) {
    auto LabelList = node->begin()[2];
    auto Type = node->begin()[3];
    mangleEntityType(applyParamLabels(LabelList, Type, Factory), ctx,
                     depth + 1);
  } else {
    mangleEntityType(node->begin()[2], ctx, depth + 1);
  }
}

void Remangler::mangleEntityContext(Node *node, EntityContext &ctx,
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
      mangleAnyNominalType(node, ctx, depth + 1);
      return;

    default:
      break;
  }

  switch (node->getKind()) {
#define NODE(ID)                                \
  case Node::Kind::ID:
#define CONTEXT_NODE(ID)
#include "swift/Demangling/DemangleNodes.def"
    unreachable("not a context node");

#define NODE(ID)
#define CONTEXT_NODE(ID)                                                       \
  case Node::Kind::ID:                                                         \
    return mangle##ID(node, ctx, depth + 1);
#include "swift/Demangling/DemangleNodes.def"
  }
  unreachable("bad node kind");
}

void Remangler::mangleEntityType(Node *node, EntityContext &ctx,
                                 unsigned depth) {
  assert(node->getKind() == Node::Kind::Type);
  assert(node->getNumChildren() == 1);
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
    unsigned inputIndex = node->getNumChildren() - 2;
    assert(inputIndex <= 1);
    for (unsigned i = 0; i <= inputIndex; ++i)
      mangle(node->begin()[i], depth + 1);
    auto returnType = node->begin()[inputIndex+1];
    assert(returnType->getKind() == Node::Kind::ReturnType);
    assert(returnType->getNumChildren() == 1);
    mangleEntityType(returnType->begin()[0], ctx, depth + 1);
    return;
  }
  default:
    mangle(node, depth + 1);
    return;
  }
}

void Remangler::mangleLocalDeclName(Node *node, unsigned depth) {
  Buffer << 'L';
  mangleChildNodes(node, depth + 1); // index, identifier
}

void Remangler::manglePrivateDeclName(Node *node, unsigned depth) {
  Buffer << 'P';
  mangleChildNodes(node, depth + 1); // identifier, identifier
}

void Remangler::mangleRelatedEntityDeclName(Node *node, unsigned depth) {
  // Non-round-trip mangling: pretend we have a private discriminator "$A" for a
  // related entity "A".
  NodePointer kindNode = node->getFirstChild();
  Buffer << 'P' << (kindNode->getText().size() + 1) << '$' << kindNode->getText();
  mangleChildNode(node, 1, depth + 1);
}

void Remangler::mangleTypeMangling(Node *node, unsigned depth) {
  Buffer << 't';
  mangleSingleChildNode(node, depth + 1); // type
}

void Remangler::mangleType(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
}

template <size_t N> 
static bool stripPrefix(StringRef &string, const char (&data)[N]) {
  constexpr size_t prefixLength = N - 1;
  if (!string.startswith(StringRef(data, prefixLength)))
    return false;
  string = string.drop_front(prefixLength);
  return true;
}

void Remangler::mangleBuiltinTypeName(Node *node, unsigned depth) {
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
      unreachable("unexpected builtin vector type");
    }
  } else {
    unreachable("unexpected builtin type");
  }
}

void Remangler::mangleTypeAlias(Node *node, EntityContext &ctx,
                                unsigned depth) {
  mangleAnyNominalType(node, ctx, depth + 1);
}

void Remangler::mangleFunctionType(Node *node, unsigned depth) {
  Buffer << 'F';
  mangleChildNodes(node, depth + 1); // argument tuple, result type
}

void Remangler::mangleUncurriedFunctionType(Node *node, unsigned depth) {
  Buffer << 'f';
  mangleChildNodes(node, depth + 1); // argument tuple, result type
}

void Remangler::mangleObjCBlock(Node *node, unsigned depth) {
  Buffer << 'b';
  mangleChildNodes(node, depth + 1); // argument tuple, result type
}

void Remangler::mangleEscapingObjCBlock(Node *node, unsigned depth) {
  // We shouldn't ever be remangling anything with a DWARF-only mangling.
  Buffer << "<escaping block type>";
}

void Remangler::mangleCFunctionPointer(Node *node, unsigned depth) {
  Buffer << 'c';
  mangleChildNodes(node, depth + 1); // argument tuple, result type
}

void Remangler::mangleAutoClosureType(Node *node, unsigned depth) {
  Buffer << 'K';
  mangleChildNodes(node, depth + 1); // argument tuple, result type
}

void Remangler::mangleNoEscapeFunctionType(Node *node, unsigned depth) {
  Buffer << 'F';
  mangleChildNodes(node, depth + 1); // argument tuple, result type
}

void Remangler::mangleEscapingAutoClosureType(Node *node, unsigned depth) {
  Buffer << 'K';
  mangleChildNodes(node, depth + 1); // argument tuple, result type
}

void Remangler::mangleThinFunctionType(Node *node, unsigned depth) {
  Buffer << "Xf";
  mangleChildNodes(node, depth + 1); // argument tuple, result type
}

void Remangler::mangleArgumentTuple(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
}

void Remangler::mangleReturnType(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
}

void Remangler::mangleImplFunctionType(Node *node, unsigned depth) {
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
      unreachable("bad callee convention");
    }
  } else {
    Buffer << 't';
  }
  for (; i != e &&
         (*i)->getKind() == Node::Kind::ImplFunctionAttribute; ++i) {
    mangle(*i, depth + 1); // impl function attribute
  }
  if (i != e &&
      ((*i)->getKind() == Node::Kind::DependentGenericSignature ||
       (*i)->getKind() == Node::Kind::DependentPseudogenericSignature)) {
    Buffer << ((*i)->getKind() == Node::Kind::DependentGenericSignature
              ? 'G' : 'g');
    mangleDependentGenericSignature((*i), depth + 1);
    ++i;
  }
  Buffer << '_';
  for (; i != e && (*i)->getKind() == Node::Kind::ImplParameter; ++i) {
    mangleImplParameter(*i, depth + 1);
  }
  Buffer << '_';
  mangleNodes(i, e, depth + 1); // impl results
  Buffer << '_';
}

void Remangler::mangleImplFunctionAttribute(Node *node, unsigned depth) {
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
    unreachable("bad impl-function-attribute");
  }
}

void Remangler::mangleImplFunctionConvention(Node *node, unsigned depth) {
  mangle(node->getChild(0), depth + 1);
}

void Remangler::mangleImplFunctionConventionName(Node *node, unsigned depth) {
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
    unreachable("bad impl-function-convention-name");
  }
}

void Remangler::mangleClangType(Node *node, unsigned depth) {
  unreachable("unsupported");
}

void Remangler::mangleImplParameter(Node *node, unsigned depth) {
  assert(node->getNumChildren() == 2);
  mangleChildNodes(node, depth + 1); // impl convention, type
}

void Remangler::mangleImplErrorResult(Node *node, unsigned depth) {
  assert(node->getNumChildren() == 2);
  Buffer << 'z';
  mangleChildNodes(node, depth + 1); // impl convention, type
}

void Remangler::mangleImplResult(Node *node, unsigned depth) {
  assert(node->getNumChildren() == 2);
  mangleChildNodes(node, depth + 1); // impl convention, type
}

void Remangler::mangleImplYield(Node *node, unsigned depth) {
  assert(node->getNumChildren() == 2);
  Buffer << 'Y';
  mangleChildNodes(node, depth + 1); // impl convention, type
}

void Remangler::mangleImplDifferentiabilityKind(Node *node, unsigned depth) {
  // TODO(TF-750): Check if this code path actually triggers and add a test.
  Buffer << (char)node->getIndex();
}

void Remangler::mangleImplEscaping(Node *node, unsigned depth) {
  // The old mangler does not encode escaping.
}

void Remangler::mangleImplPatternSubstitutions(Node *node, unsigned depth) {
  // The old mangler does not encode substituted function types.
}

void Remangler::mangleImplInvocationSubstitutions(Node *node, unsigned depth) {
  // The old mangler does not encode substituted function types.
}

void Remangler::mangleImplConvention(Node *node, unsigned depth) {
  assert(node->getKind() == Node::Kind::ImplConvention);
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
    unreachable("invalid impl convention");
  }
}

void Remangler::mangleImplParameterResultDifferentiability(Node *node,
                                                           unsigned depth) {
  assert(node->getKind() == Node::Kind::ImplDifferentiabilityKind);
  StringRef text = node->getText();
  // Empty string represents default differentiability.
  if (text.empty())
    return;
  if (text == "@noDerivative") {
    Buffer << 'w';
    return;
  }
  unreachable("Invalid impl differentiability");
}

void Remangler::mangleDynamicSelf(Node *node, unsigned depth) {
  Buffer << 'D';
  mangleSingleChildNode(node, depth + 1); // type
}

void Remangler::mangleErrorType(Node *node, unsigned depth) { Buffer << "ERR"; }

void Remangler::mangleSILBoxType(Node *node, unsigned depth) {
  Buffer << 'X' << 'b';
  mangleSingleChildNode(node, depth + 1);
}

void Remangler::mangleMetatype(Node *node, unsigned depth) {
  if (node->getNumChildren() == 1) {
    Buffer << 'M';
    mangleSingleChildNode(node, depth + 1); // type
  } else {
    assert(node->getNumChildren() == 2);
    Buffer << "XM";
    mangleChildNodes(node, depth + 1); // metatype representation, type
  }
}

void Remangler::mangleExistentialMetatype(Node *node, unsigned depth) {
  if (node->getNumChildren() == 1) {
    Buffer << "PM";
    mangleSingleChildNode(node, depth + 1); // type
  } else {
    assert(node->getNumChildren() == 2);
    Buffer << "XPM";
    mangleChildNodes(node, depth + 1); // metatype representation, type
  }
}

void Remangler::mangleMetatypeRepresentation(Node *node, unsigned depth) {
  StringRef text = node->getText();
  if (text == "@thin") {
    Buffer << 't';
  } else if (text == "@thick") {
    Buffer << 'T';
  } else if (text == "@objc_metatype") {
    Buffer << 'o';
  } else {
    unreachable("bad metatype representation");
  }
}

void Remangler::mangleProtocolList(Node *node, unsigned depth) {
  // In its usual use as a type, this gets a prefix 'P'.
  Buffer << 'P';
  mangleProtocolListWithoutPrefix(node, depth + 1);
}

void Remangler::mangleProtocolListWithoutPrefix(Node *node, unsigned depth,
                                                Node *additionalProto) {
  assert(node->getKind() == Node::Kind::ProtocolList);
  assert(node->getNumChildren() == 1);
  auto typeList = node->begin()[0];
  assert(typeList->getKind() == Node::Kind::TypeList);
  for (auto &child : *typeList) {
    mangleProtocolWithoutPrefix(child, depth + 1);
  }
  if (additionalProto) {
    mangleProtocolWithoutPrefix(additionalProto, depth + 1);
  }
  Buffer << '_';
}

#define REF_STORAGE(Name, ...)                                                 \
  void Remangler::mangle##Name(Node *node, unsigned depth) {                   \
    Buffer << manglingOf(ReferenceOwnership::Name);                            \
    mangleSingleChildNode(node, depth + 1); /* type */                         \
  }
#include "swift/AST/ReferenceStorage.def"

void Remangler::mangleShared(Node *node, unsigned depth) {
  Buffer << 'h';
  mangleSingleChildNode(node, depth + 1); // type
}

void Remangler::mangleOwned(Node *node, unsigned depth) {
  Buffer << 'n';
  mangleSingleChildNode(node, depth + 1); // type
}

void Remangler::mangleInOut(Node *node, unsigned depth) {
  Buffer << 'R';
  mangleSingleChildNode(node, depth + 1); // type
}

void Remangler::mangleIsolated(Node *node, unsigned depth) {
  Buffer << "Yi";
  mangleSingleChildNode(node, depth + 1); // type
}

void Remangler::mangleNoDerivative(Node *node, unsigned depth) {
  Buffer << 'k';
  mangleSingleChildNode(node, depth + 1); // type
}

void Remangler::mangleTuple(Node *node, unsigned depth) {
  size_t NumElems = node->getNumChildren();
  if (NumElems > 0 &&
      node->getChild(NumElems - 1)->getFirstChild()->getKind() ==
      Node::Kind::VariadicMarker) {
    Buffer << 't';
  } else {
    Buffer << 'T';
  }
  mangleChildNodes(node, depth + 1); // tuple elements
  Buffer << '_';
}

void Remangler::mangleTupleElement(Node *node, unsigned depth) {
  mangleChildNodes(node, depth + 1); // tuple element name?, type
}

void Remangler::mangleTupleElementName(Node *node, unsigned depth) {
  mangleIdentifier(node->getText(), OperatorKind::NotOperator);
}

void Remangler::mangleDependentGenericType(Node *node, unsigned depth) {
  Buffer << 'u';
  mangleChildNodes(node, depth + 1); // generic signature, type
}

void Remangler::mangleDependentPseudogenericSignature(Node *node,
                                                      unsigned depth) {
  mangleDependentGenericSignature(node, depth + 1);
}

void Remangler::mangleDependentGenericSignature(Node *node, unsigned depth) {
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
    return;
  }
  
  Buffer << 'R';
  mangleNodes(i, e, depth + 1); // generic requirements
  Buffer << 'r';
}

void Remangler::mangleDependentGenericParamCount(Node *node, unsigned depth) {
  unreachable("handled inline in DependentGenericSignature");
}

void Remangler::mangleDependentGenericConformanceRequirement(Node *node,
                                                             unsigned depth) {
  mangleConstrainedType(node->getChild(0), depth + 1);
  // If the constraint represents a protocol, use the shorter mangling.
  if (node->getNumChildren() == 2
      && node->getChild(1)->getKind() == Node::Kind::Type
      && node->getChild(1)->getNumChildren() == 1
      && node->getChild(1)->getChild(0)->getKind() == Node::Kind::Protocol) {
    mangleProtocolWithoutPrefix(node->getChild(1)->getChild(0), depth + 1);
    return;
  }

  mangle(node->getChild(1), depth + 1);
}

void Remangler::mangleDependentGenericSameTypeRequirement(Node *node,
                                                          unsigned depth) {
  mangleConstrainedType(node->getChild(0), depth + 1);
  Buffer << 'z';
  mangle(node->getChild(1), depth + 1);
}

void Remangler::mangleDependentGenericLayoutRequirement(Node *node,
                                                        unsigned depth) {
  mangleConstrainedType(node->getChild(0), depth + 1);
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
}

void Remangler::mangleConstrainedType(Node *node, unsigned depth) {
  if (node->getFirstChild()->getKind()
        == Node::Kind::DependentGenericParamType) {
    // Can be mangled without an introducer.
    mangleDependentGenericParamIndex(node->getFirstChild(), depth + 1);
  } else {
    mangle(node, depth + 1);
  }
}

void Remangler::mangleAssociatedType(Node *node, unsigned depth) {
  if (node->hasChildren()) {
    assert(node->getNumChildren() == 1);
    mangleProtocolListWithoutPrefix(*node->begin(), depth + 1);
  } else {
    Buffer << '_';
  }
}

void Remangler::mangleDeclContext(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
}

void Remangler::mangleExtension(Node *node, EntityContext &ctx,
                                unsigned depth) {
  assert(node->getNumChildren() == 2 || node->getNumChildren() == 3);
  if (node->getNumChildren() == 3) {
    Buffer << 'e';
  } else {
    Buffer << 'E';
  }
  mangleEntityContext(node->begin()[0], ctx, depth + 1); // module
  if (node->getNumChildren() == 3) {
    mangleDependentGenericSignature(node->begin()[2], depth + 1); // generic sig
  }

  mangleEntityContext(node->begin()[1], ctx, depth + 1); // context
}

void Remangler::mangleAnonymousContext(Node *node, EntityContext &ctx,
                                       unsigned depth) {
  mangleEntityContext(node->getChild(1), ctx, depth + 1);

  // Since we can't change the old mangling, mangle an anonymous context by
  // introducing a private discriminator onto its child contexts.
  ctx.setAnonymousContextDiscriminator(node->getChild(0)->getText());
}

void Remangler::mangleModule(Node *node, EntityContext &ctx, unsigned depth) {
  SubstitutionEntry entry;
  if (trySubstitution(node, entry)) return;

  // Module types get an M prefix, but module contexts don't.
  if (!ctx.isAsContext()) Buffer << 'M';
  mangleIdentifier(node->getText(), OperatorKind::NotOperator);
  addSubstitution(entry);
}

void Remangler::mangleAssociatedTypeRef(Node *node, unsigned depth) {
  SubstitutionEntry entry;
  if (trySubstitution(node, entry)) return;
  Buffer << "Q";
  mangleChildNodes(node, depth + 1); // type, identifier
  addSubstitution(entry);
}

void Remangler::mangleDependentMemberType(Node *node, unsigned depth) {
  Vector<Node *> members;
  Node *base = node;
  do {
    members.push_back(base, Factory);
    base = base->getFirstChild()->getFirstChild();
  } while (base->getKind() == Node::Kind::DependentMemberType);

  assert(base->getKind() == Node::Kind::DependentGenericParamType
         && "dependent members not based on a generic param are non-canonical"
            " and shouldn't need remangling");
  assert(members.size() >= 1);
  if (members.size() == 1) {
    Buffer << 'w';
    mangleDependentGenericParamIndex(base, depth + 1);
    mangle(members[0]->getChild(1), depth + 1);
  } else {
    Buffer << 'W';
    mangleDependentGenericParamIndex(base, depth + 1);

    for (unsigned i = 1, n = members.size(); i <= n; ++i) {
      Node *member = members[n - i];
      mangle(member->getChild(1), depth + 1);
    }
    Buffer << '_';
  }
}

void Remangler::mangleDependentAssociatedTypeRef(Node *node, unsigned depth) {
  SubstitutionEntry entry;
  if (trySubstitution(node, entry)) return;

  if (node->getNumChildren() > 1) {
    Buffer << 'P';
    mangleProtocolWithoutPrefix(node->getChild(1), depth + 1);
  }
  mangleIdentifier(node->getFirstChild(), depth + 1);

  addSubstitution(entry);
}

void Remangler::mangleDependentGenericParamIndex(Node *node, unsigned depth) {
  auto paramDepth = node->getChild(0)->getIndex();
  auto index = node->getChild(1)->getIndex();

  if (paramDepth != 0) {
    Buffer << 'd';
    mangleIndex(paramDepth - 1);
    mangleIndex(index);
    return;
  }
  if (index != 0) {
    mangleIndex(index - 1);
    return;
  }

  // paramDepth == index == 0
  Buffer << 'x';
}

void Remangler::mangleDependentGenericParamType(Node *node, unsigned depth) {
  if (node->getChild(0)->getIndex() == 0
      && node->getChild(1)->getIndex() == 0) {
    Buffer << 'x';
    return;
  }

  Buffer << 'q';
  mangleDependentGenericParamIndex(node, depth + 1);
}

void Remangler::mangleIndex(Node *node, unsigned depth) {
  mangleIndex(node->getIndex());
}

void Remangler::mangleUnknownIndex(Node *node, unsigned depth) {
  unreachable("should not be reached in an arbitrary context");
}

void Remangler::mangleProtocol(Node *node, EntityContext &ctx, unsigned depth) {
  mangleNominalType(node, 'P', ctx, depth + 1);
}

void Remangler::mangleProtocolWithoutPrefix(Node *node, unsigned depth) {
  if (mangleStandardSubstitution(node))
    return;

  if (node->getKind() == Node::Kind::Type) {
    assert(node->getNumChildren() == 1);
    node = node->begin()[0];
  }

  assert(node->getKind() == Node::Kind::Protocol);
  EntityContext ctx;
  mangleNominalType(node, '\0', ctx, depth + 1);
}

void Remangler::mangleGenericArgs(Node *node, EntityContext &ctx,
                                  unsigned depth) {
  switch (node->getKind()) {
  case Node::Kind::Structure:
  case Node::Kind::Enum:
  case Node::Kind::Class: {
    NodePointer parentOrModule = node->getChild(0);
    mangleGenericArgs(parentOrModule, ctx, depth + 1);

    // No generic arguments at this level
    Buffer << '_';
    break;
  }

  case Node::Kind::BoundGenericStructure:
  case Node::Kind::BoundGenericEnum:
  case Node::Kind::BoundGenericClass: {
    NodePointer unboundType = node->getChild(0);
    assert(unboundType->getKind() == Node::Kind::Type);
    NodePointer nominalType = unboundType->getChild(0);
    NodePointer parentOrModule = nominalType->getChild(0);
    mangleGenericArgs(parentOrModule, ctx, depth + 1);

    mangleTypeList(node->getChild(1), depth + 1);
    break;
  }

  case Node::Kind::AnonymousContext:
  case Node::Kind::Extension: {
    mangleGenericArgs(node->getChild(1), ctx, depth + 1);
    break;
  }

  default:
    break;
  }
}

void Remangler::mangleAnyNominalType(Node *node, EntityContext &ctx,
                                     unsigned depth) {
  if (depth > Remangler::MaxDepth) {
    // FIXME: error handling needs doing properly (rdar://79725187)
    unreachable("too complex to remangle");
  }

  if (isSpecialized(node)) {
    Buffer << 'G';

    NodePointer unboundType = getUnspecialized(node, Factory);

    mangleAnyNominalType(unboundType, ctx, depth + 1);
    mangleGenericArgs(node, ctx, depth + 1);
    return;
  }

  switch (node->getKind()) {
  case Node::Kind::Type:
    mangleAnyNominalType(node->getChild(0), ctx, depth + 1);
    break;
  case Node::Kind::OtherNominalType:
    // Mangle unknown type kinds as structures since we can't change the old
    // mangling. Give the mangling an artificial "private discriminator" so that
    // clients who understand the old mangling know this is an unstable
    // mangled name.
    mangleNominalType(node, 'V', ctx, depth + 1, "_UnknownTypeKind");
    break;
  case Node::Kind::Structure:
    mangleNominalType(node, 'V', ctx, depth + 1);
    break;
  case Node::Kind::Enum:
    mangleNominalType(node, 'O', ctx, depth + 1);
    break;
  case Node::Kind::Class:
    mangleNominalType(node, 'C', ctx, depth + 1);
    break;
  case Node::Kind::TypeAlias:
    mangleNominalType(node, 'a', ctx, depth + 1);
    break;
  default:
    unreachable("bad nominal type kind");
  }
}

void Remangler::mangleStructure(Node *node, EntityContext &ctx,
                                unsigned depth) {
  mangleAnyNominalType(node, ctx, depth + 1);
}

void Remangler::mangleEnum(Node *node, EntityContext &ctx, unsigned depth) {
  mangleAnyNominalType(node, ctx, depth + 1);
}

void Remangler::mangleClass(Node *node, EntityContext &ctx, unsigned depth) {
  mangleAnyNominalType(node, ctx, depth + 1);
}

void Remangler::mangleOtherNominalType(Node *node, EntityContext &ctx,
                                       unsigned depth) {
  mangleAnyNominalType(node, ctx, depth + 1);
}

void Remangler::mangleNominalType(Node *node, char kind, EntityContext &ctx,
                                  unsigned depth,
                                  StringRef artificialPrivateDiscriminator) {
  SubstitutionEntry entry;
  if (node->getKind() == Node::Kind::Type) {
    node = node->getChild(0);
  }
  if (trySubstitution(node, entry)) return;
  mangleNamedEntity(node, kind, "", ctx, depth + 1,
                    artificialPrivateDiscriminator);
  addSubstitution(entry);
}

void Remangler::mangleBoundGenericClass(Node *node, unsigned depth) {
  EntityContext ctx;
  mangleAnyNominalType(node, ctx, depth + 1);
}

void Remangler::mangleBoundGenericStructure(Node *node, unsigned depth) {
  EntityContext ctx;
  mangleAnyNominalType(node, ctx, depth + 1);
}

void Remangler::mangleBoundGenericEnum(Node *node, unsigned depth) {
  EntityContext ctx;
  mangleAnyNominalType(node, ctx, depth + 1);
}

void Remangler::mangleBoundGenericOtherNominalType(Node *node, unsigned depth) {
  EntityContext ctx;
  mangleAnyNominalType(node, ctx, depth + 1);
}

void Remangler::mangleBoundGenericProtocol(Node *node, unsigned depth) {
  EntityContext ctx;
  mangleAnyNominalType(node, ctx, depth + 1);
}

void Remangler::mangleBoundGenericTypeAlias(Node *node, unsigned depth) {
  EntityContext ctx;
  mangleAnyNominalType(node, ctx, depth + 1);
}

void Remangler::mangleBoundGenericFunction(Node *node, unsigned depth) {
  EntityContext ctx;
  // Not really a nominal type, but it works for functions, too.
  mangleAnyNominalType(node, ctx, depth + 1);
}

void Remangler::mangleTypeList(Node *node, unsigned depth) {
  mangleChildNodes(node, depth + 1); // all types
  Buffer << '_';
}

void Remangler::mangleLabelList(Node *node, unsigned depth) {
  if (node->getNumChildren() == 0)
    Buffer << 'y';
  else
    mangleChildNodes(node, depth + 1);
}

void Remangler::mangleReflectionMetadataBuiltinDescriptor(Node *node,
                                                          unsigned depth) {
  Buffer << "MRb";
}

void Remangler::mangleReflectionMetadataFieldDescriptor(Node *node,
                                                        unsigned depth) {
  Buffer << "MRf";
}

void Remangler::mangleReflectionMetadataAssocTypeDescriptor(Node *node,
                                                            unsigned depth) {
  Buffer << "MRa";
}

void Remangler::mangleReflectionMetadataSuperclassDescriptor(Node *node,
                                                             unsigned depth) {
  Buffer << "MRc";
}

void Remangler::mangleGenericTypeParamDecl(Node *node, unsigned depth) {
  unreachable("todo");
}

void Remangler::mangleCurryThunk(Node *node, unsigned depth) {
  Buffer << "<curry-thunk>";
}

void Remangler::mangleDispatchThunk(Node *node, unsigned depth) {
  Buffer << "<dispatch-thunk>";
}

void Remangler::mangleMethodDescriptor(Node *node, unsigned depth) {
  Buffer << "<method-descriptor>";
}

void Remangler::mangleMethodLookupFunction(Node *node, unsigned depth) {
  Buffer << "<method-lookup-function>";
}

void Remangler::mangleObjCMetadataUpdateFunction(Node *node, unsigned depth) {
  Buffer << "<objc-metadata-update-function>";
}

void Remangler::mangleObjCResilientClassStub(Node *node, unsigned depth) {
  Buffer << "<objc-resilient-class-stub>";
}

void Remangler::mangleFullObjCResilientClassStub(Node *node, unsigned depth) {
  Buffer << "<full-objc-resilient-class-stub>";
}

void Remangler::mangleEmptyList(Node *node, unsigned depth) {
  Buffer << "<empty>";
}

void Remangler::mangleFirstElementMarker(Node *node, unsigned depth) {
  Buffer << "<first>";
}

void Remangler::mangleVariadicMarker(Node *node, unsigned depth) {
  // Handled in mangleTuple
}

void Remangler::mangleOutlinedCopy(Node *node, unsigned depth) {
  Buffer << "Wy";
  mangleChildNodes(node, depth + 1);
}

void Remangler::mangleOutlinedConsume(Node *node, unsigned depth) {
  Buffer << "We";
  mangleChildNodes(node, depth + 1);
}

void Remangler::mangleOutlinedRetain(Node *node, unsigned depth) {
  Buffer << "Wr";
  mangleSingleChildNode(node, depth + 1);
}

void Remangler::mangleOutlinedRelease(Node *node, unsigned depth) {
  Buffer << "Ws";
  mangleSingleChildNode(node, depth + 1);
}

void Remangler::mangleOutlinedInitializeWithTake(Node *node, unsigned depth) {
  Buffer << "Wb";
  mangleSingleChildNode(node, depth + 1);
}

void Remangler::mangleOutlinedInitializeWithCopy(Node *node, unsigned depth) {
  Buffer << "Wc";
  mangleSingleChildNode(node, depth + 1);
}

void Remangler::mangleOutlinedAssignWithTake(Node *node, unsigned depth) {
  Buffer << "Wd";
  mangleSingleChildNode(node, depth + 1);
}

void Remangler::mangleOutlinedAssignWithCopy(Node *node, unsigned depth) {
  Buffer << "Wf";
  mangleSingleChildNode(node, depth + 1);
}

void Remangler::mangleOutlinedDestroy(Node *node, unsigned depth) {
  Buffer << "Wh";
  mangleSingleChildNode(node, depth + 1);
}

void Remangler::mangleOutlinedVariable(Node *node, unsigned depth) {
  Buffer << "Tv" << node->getIndex();
  mangleSingleChildNode(node, depth + 1);
}

void Remangler::mangleOutlinedBridgedMethod(Node *node, unsigned depth) {
  Buffer << "Te" << node->getText();
  mangleSingleChildNode(node, depth + 1);
}

void Remangler::mangleCoroutineContinuationPrototype(Node *node,
                                                     unsigned depth) {
  Buffer << "TC";
  mangleChildNodes(node, depth + 1);
}

void Remangler::mangleKeyPathGetterThunkHelper(Node *node, unsigned depth) {
  Buffer << "TK";
  mangleChildNodes(node, depth + 1);
}

void Remangler::mangleKeyPathSetterThunkHelper(Node *node, unsigned depth) {
  Buffer << "Tk";
  mangleChildNodes(node, depth + 1);
}

void Remangler::mangleKeyPathEqualsThunkHelper(Node *node, unsigned depth) {
  Buffer << "TH";
  mangleChildNodes(node, depth + 1);
}

void Remangler::mangleKeyPathHashThunkHelper(Node *node, unsigned depth) {
  Buffer << "Th";
  mangleChildNodes(node, depth + 1);
}

void Remangler::mangleProtocolListWithClass(Node *node, unsigned depth) {
  Buffer << "Xc";
  mangleChildNode(node, 1, depth + 1);
  mangleProtocolListWithoutPrefix(node->getChild(0), depth + 1);
}

void Remangler::mangleProtocolListWithAnyObject(Node *node, unsigned depth) {
  Node *P = Factory.createNode(Node::Kind::Protocol);
  P->addChild(Factory.createNode(Node::Kind::Module, "Swift"), Factory);
  P->addChild(Factory.createNode(Node::Kind::Identifier, "AnyObject"), Factory);
  Buffer << "P";
  mangleProtocolListWithoutPrefix(node->getChild(0), depth + 1,
                                  /*additionalProto*/ P);
}

void Remangler::mangleVTableThunk(Node *node, unsigned depth) {
  Buffer << "TV";
  mangleChildNodes(node, depth + 1);
}

void Remangler::mangleSILBoxTypeWithLayout(Node *node, unsigned depth) {
  assert(node->getKind() == Node::Kind::SILBoxTypeWithLayout);
  assert(node->getNumChildren() == 1 || node->getNumChildren() == 3);
  Buffer << "XB";
  auto layout = node->getChild(0);
  assert(layout->getKind() == Node::Kind::SILBoxLayout);
  NodePointer genericArgs = nullptr;
  if (node->getNumChildren() == 3) {
    NodePointer signature = node->getChild(1);
    assert(signature->getKind() == Node::Kind::DependentGenericSignature);
    genericArgs = node->getChild(2);
    assert(genericArgs->getKind() == Node::Kind::TypeList);
    
    Buffer << 'G';
    mangleDependentGenericSignature(signature, depth + 1);
  }
  mangleSILBoxLayout(layout, depth + 1);
  if (genericArgs) {
    for (unsigned i = 0; i < genericArgs->getNumChildren(); ++i) {
      auto type = genericArgs->getChild(i);
      assert(genericArgs->getKind() == Node::Kind::Type);
      mangleType(type, depth + 1);
    }
    Buffer << '_';
  }
}

void Remangler::mangleSILBoxLayout(Node *node, unsigned depth) {
  assert(node->getKind() == Node::Kind::SILBoxLayout);
  for (unsigned i = 0; i < node->getNumChildren(); ++i) {
    assert(node->getKind() == Node::Kind::SILBoxImmutableField
           || node->getKind() == Node::Kind::SILBoxMutableField);
    mangle(node->getChild(i), depth + 1);
  }
  Buffer << '_';
}

void Remangler::mangleSILBoxMutableField(Node *node, unsigned depth) {
  Buffer << 'm';
  assert(node->getNumChildren() == 1
         && node->getChild(0)->getKind() == Node::Kind::Type);
  mangleType(node->getChild(0), depth + 1);
}

void Remangler::mangleSILBoxImmutableField(Node *node, unsigned depth) {
  Buffer << 'i';
  assert(node->getNumChildren() == 1
         && node->getChild(0)->getKind() == Node::Kind::Type);
  mangleType(node->getChild(0), depth + 1);
}

void Remangler::mangleAssocTypePath(Node *node, unsigned depth) {
  unreachable("unsupported");
}

void Remangler::mangleModuleDescriptor(Node *node, unsigned depth) {
  unreachable("unsupported");
}

void Remangler::mangleExtensionDescriptor(Node *node, unsigned depth) {
  unreachable("unsupported");
}

void Remangler::mangleAnonymousDescriptor(Node *node, unsigned depth) {
  unreachable("unsupported");
}

void Remangler::mangleAssociatedTypeGenericParamRef(Node *node,
                                                    unsigned depth) {
  unreachable("unsupported");
}

void Remangler::mangleTypeSymbolicReference(Node *node, EntityContext &,
                                            unsigned depth) {
  unreachable("unsupported");
}

void Remangler::mangleProtocolSymbolicReference(Node *node, EntityContext &,
                                                unsigned depth) {
  unreachable("unsupported");
}

void Remangler::mangleOpaqueTypeDescriptorSymbolicReference(Node *node,
                                                            unsigned depth) {
  unreachable("unsupported");
}

void Remangler::mangleSugaredOptional(Node *node, unsigned depth) {
  unreachable("unsupported");
}

void Remangler::mangleSugaredArray(Node *node, unsigned depth) {
  unreachable("unsupported");
}

void Remangler::mangleSugaredDictionary(Node *node, unsigned depth) {
  unreachable("unsupported");
}

void Remangler::mangleSugaredParen(Node *node, unsigned depth) {
  unreachable("unsupported");
}

void Remangler::mangleOpaqueReturnType(Node *node, unsigned depth) {
  Buffer << "Qu";
}
void Remangler::mangleOpaqueReturnTypeOf(Node *node, EntityContext &ctx,
                                         unsigned depth) {
  unreachable("unsupported");
}
void Remangler::mangleOpaqueType(Node *node, unsigned depth) {
  unreachable("unsupported");
}
void Remangler::mangleOpaqueTypeDescriptor(Node *node, unsigned depth) {
  unreachable("unsupported");
}
void Remangler::mangleOpaqueTypeDescriptorAccessor(Node *node, unsigned depth) {
  unreachable("unsupported");
}
void Remangler::mangleOpaqueTypeDescriptorAccessorImpl(Node *node,
                                                       unsigned depth) {
  unreachable("unsupported");
}
void Remangler::mangleOpaqueTypeDescriptorAccessorKey(Node *node,
                                                      unsigned depth) {
  unreachable("unsupported");
}
void Remangler::mangleOpaqueTypeDescriptorAccessorVar(Node *node,
                                                      unsigned depth) {
  unreachable("unsupported");
}
void Remangler::mangleAccessorFunctionReference(Node *node, unsigned depth) {
  unreachable("can't remangle");
}
void Remangler::mangleMetadataInstantiationCache(Node *node, unsigned depth) {
  unreachable("unsupported");
}
void Remangler::mangleGlobalVariableOnceToken(Node *node, unsigned depth) {
  unreachable("unsupported");
}
void Remangler::mangleGlobalVariableOnceFunction(Node *node, unsigned depth) {
  unreachable("unsupported");
}
void Remangler::mangleGlobalVariableOnceDeclList(Node *node, unsigned depth) {
  unreachable("unsupported");
}
void Remangler::manglePredefinedObjCAsyncCompletionHandlerImpl(Node *node,
                                                               unsigned depth) {
  unreachable("unsupported");
}
void Remangler::mangleObjCAsyncCompletionHandlerImpl(Node *node,
                                                     unsigned depth) {
  unreachable("unsupported");
}

void Remangler::mangleCanonicalSpecializedGenericMetaclass(Node *node,
                                                           unsigned depth) {
  mangleSingleChildNode(node, depth + 1); // type
  Buffer << "MM";
}

void Remangler::mangleCanonicalSpecializedGenericTypeMetadataAccessFunction(
    Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "Mb";
}

void Remangler::mangleNoncanonicalSpecializedGenericTypeMetadata(
    Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "MN";
}

void Remangler::mangleNoncanonicalSpecializedGenericTypeMetadataCache(
    Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "MJ";
}

void Remangler::mangleCanonicalPrespecializedGenericTypeCachingOnceToken(
    Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "Mz";
}

/// The top-level interface to the remangler.
std::string Demangle::mangleNodeOld(NodePointer node) {
  if (!node) return "";

  NodeFactory Factory;
  Remangler remangler(Factory);
  remangler.mangle(node, 0);

  return remangler.str();
}

llvm::StringRef Demangle::mangleNodeOld(NodePointer node, NodeFactory &Factory) {
  if (!node) return "";

  Remangler remangler(Factory);
  remangler.mangle(node, 0);

  return remangler.getBufferStr();
}

const char *Demangle::mangleNodeAsObjcCString(NodePointer node,
                                              NodeFactory &Factory) {
  assert(node);

  Remangler remangler(Factory);
  remangler.append("_Tt");
  remangler.mangle(node, 0);
  remangler.append(StringRef("_", 2)); // Include the trailing 0 char.

  return remangler.getBufferStr().data();
}
