//===--- Remangler.cpp - Swift re-mangling from a demangling tree ---------===//
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
#include "swift/Demangling/ManglingMacros.h"
#include "swift/Demangling/ManglingUtils.h"
#include "swift/Demangling/Punycode.h"
#include "swift/Strings.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"
#include <cstdio>
#include <cstdlib>

using namespace swift;
using namespace Demangle;
using namespace Mangle;

static char getCharOfNodeText(Node *node, unsigned idx) {
  switch (node->getKind()) {
  case Node::Kind::InfixOperator:
  case Node::Kind::PrefixOperator:
  case Node::Kind::PostfixOperator:
    return Mangle::translateOperatorChar(node->getText()[idx]);
  default:
    return node->getText()[idx];
  }
}

bool SubstitutionEntry::identifierEquals(Node *lhs, Node *rhs) {
  unsigned length = lhs->getText().size();
  if (rhs->getText().size() != length)
    return false;
  // The fast path.
  if (lhs->getKind() == rhs->getKind())
    return lhs->getText() == rhs->getText();
  // The slow path.
  for (unsigned i = 0; i < length; ++i) {
    if (getCharOfNodeText(lhs, i) != getCharOfNodeText(rhs, i))
      return false;
  }
  return true;
}

void SubstitutionEntry::deepHash(Node *node) {
  if (treatAsIdentifier) {
    combineHash((size_t) Node::Kind::Identifier);
    assert(node->hasText());
    switch (node->getKind()) {
    case Node::Kind::InfixOperator:
    case Node::Kind::PrefixOperator:
    case Node::Kind::PostfixOperator:
      for (char c : node->getText()) {
        combineHash((unsigned char)translateOperatorChar(c));
      }
      return;
    default:
      break;
    }
  } else {
    combineHash((size_t) node->getKind());
  }
  if (node->hasIndex()) {
    combineHash(node->getIndex());
  } else if (node->hasText()) {
    for (char c : node->getText()) {
      combineHash((unsigned char) c);
    }
  }
  for (Node *child : *node) {
    deepHash(child);
  }
}

bool SubstitutionEntry::deepEquals(Node *lhs, Node *rhs) const {
  if (lhs->getKind() != rhs->getKind())
    return false;
  if (lhs->hasIndex()) {
    if (!rhs->hasIndex())
      return false;
    if (lhs->getIndex() != rhs->getIndex())
      return false;
  } else if (lhs->hasText()) {
    if (!rhs->hasText())
      return false;
    if (lhs->getText() != rhs->getText())
      return false;
  } else if (rhs->hasIndex() || rhs->hasText()) {
    return false;
  }

  if (lhs->getNumChildren() != rhs->getNumChildren())
    return false;

  for (auto li = lhs->begin(), ri = rhs->begin(), le = lhs->end();
       li != le; ++li, ++ri) {
    if (!deepEquals(*li, *ri))
      return false;
  }
  
  return true;
}

// Find a substitution and return its index.
// Returns -1 if no substitution is found.
int RemanglerBase::findSubstitution(const SubstitutionEntry &entry) {
  // First search in InlineSubstitutions.
  SubstitutionEntry *result
  = std::find(InlineSubstitutions, InlineSubstitutions + NumInlineSubsts,
              entry);
  if (result != InlineSubstitutions + NumInlineSubsts)
    return result - InlineSubstitutions;

  // Then search in OverflowSubstitutions.
  auto it = OverflowSubstitutions.find(entry);
  if (it == OverflowSubstitutions.end())
    return -1;

  return it->second;
}

void RemanglerBase::addSubstitution(const SubstitutionEntry &entry) {
  assert(findSubstitution(entry) < 0);
  if (NumInlineSubsts < InlineSubstCapacity) {
    // There is still free space in NumInlineSubsts.
    assert(OverflowSubstitutions.empty());
    InlineSubstitutions[NumInlineSubsts++] = entry;
    return;
  }
  // We have to add the entry to OverflowSubstitutions.
  unsigned Idx = OverflowSubstitutions.size() + InlineSubstCapacity;
  auto result = OverflowSubstitutions.insert({entry, Idx});
  assert(result.second);
  (void) result;
}

namespace {

class Remangler : public RemanglerBase {
  template <typename Mangler>
  friend void Mangle::mangleIdentifier(Mangler &M, StringRef ident);
  friend class Mangle::SubstitutionMerging;

  const bool UsePunycode = true;

  Vector<SubstitutionWord> Words;
  Vector<WordReplacement> SubstWordsInIdent;

  static const size_t MaxNumWords = 26;

  static const unsigned MaxDepth = 1024;

  SubstitutionMerging SubstMerging;

  // A callback for resolving symbolic references.
  SymbolicResolver Resolver;

  void addSubstWordsInIdent(const WordReplacement &repl) {
    SubstWordsInIdent.push_back(repl, Factory);
  }

  void addWord(const SubstitutionWord &word) {
    Words.push_back(word, Factory);
  }

  template <typename Mangler>
  friend void mangleIdentifier(Mangler &M, StringRef ident);

  class EntityContext {
    bool AsContext = false;
  public:
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

  // ###TODO: Consider fixing some of these asserts() to return errors somehow
  Node *getSingleChild(Node *node) {
    assert(node->getNumChildren() == 1);
    return node->getFirstChild();
  }

  Node *getSingleChild(Node *node, Node::Kind kind) {
    Node *Child = getSingleChild(node);
    assert(Child->getKind() == kind);
    return Child;
  }

  Node *skipType(Node *node) {
    if (node->getKind() == Node::Kind::Type)
      return getSingleChild(node);
    return node;
  }

  Node *getChildOfType(Node *node) {
    assert(node->getKind() == Node::Kind::Type);
    return getSingleChild(node);
  }

  // Cannot fail
  void mangleIndex(Node::IndexType value) {
    if (value == 0) {
      Buffer << '_';
    } else {
      Buffer << (value - 1) << '_';
    }
  }
  ManglingError mangleDependentConformanceIndex(Node *node, unsigned depth);

  ManglingError mangleChildNodes(Node *node, unsigned depth) {
    return mangleNodes(node->begin(), node->end(), depth);
  }
  ManglingError mangleChildNodesReversed(Node *node, unsigned depth) {
    for (size_t Idx = 0, Num = node->getNumChildren(); Idx < Num; ++Idx) {
      RETURN_IF_ERROR(mangleChildNode(node, Num - Idx - 1, depth));
    }
    return ManglingError::Success;
  }

  void mangleListSeparator(bool &isFirstListItem) {
    if (isFirstListItem) {
      Buffer << '_';
      isFirstListItem = false;
    }
  }

  void mangleEndOfList(bool isFirstListItem) {
    if (isFirstListItem)
      Buffer << 'y';
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
    if (index < node->getNumChildren())
      return mangle(node->begin()[index], depth);
    return ManglingError::Success;
  }

  ManglingError manglePureProtocol(Node *Proto, unsigned depth) {
    Proto = skipType(Proto);
    if (mangleStandardSubstitution(Proto))
      return ManglingError::Success;

    return mangleChildNodes(Proto, depth);
  }

  ManglingError mangleProtocolList(Node *protocols, Node *superclass,
                                   bool hasExplicitAnyObject, unsigned depth);

  bool trySubstitution(Node *node, SubstitutionEntry &entry,
                       bool treatAsIdentifier = false);

  void mangleIdentifierImpl(Node *node, bool isOperator);

  bool mangleStandardSubstitution(Node *node);

  // Cannot fail
  void mangleDependentGenericParamIndex(Node *node,
                                        const char *nonZeroPrefix = "",
                                        char zeroOp = 'z');

  ManglingErrorOr<std::pair<int, Node *>> mangleConstrainedType(Node *node,
                                                                unsigned depth);

  ManglingError mangleFunctionSignature(Node *FuncType, unsigned depth) {
    return mangleChildNodesReversed(FuncType, depth);
  }

  ManglingError mangleGenericSpecializationNode(Node *node,
                                                const char *operatorStr,
                                                unsigned depth);
  ManglingError mangleAnyNominalType(Node *node, unsigned depth);
  ManglingError mangleAnyGenericType(Node *node, StringRef TypeOp,
                                     unsigned depth);
  ManglingError mangleGenericArgs(Node *node, char &Separator, unsigned depth,
                                  bool fullSubstitutionMap = false);
  ManglingError mangleAnyConstructor(Node *node, char kindOp, unsigned depth);
  ManglingError mangleAbstractStorage(Node *node, StringRef accessorCode,
                                      unsigned depth);
  ManglingError mangleAnyProtocolConformance(Node *node, unsigned depth);

  ManglingError mangleKeyPathThunkHelper(Node *node, StringRef op,
                                         unsigned depth);

  ManglingError mangleAutoDiffFunctionOrSimpleThunk(Node *node, StringRef op,
                                                    unsigned depth);

#define NODE(ID) ManglingError mangle##ID(Node *node, unsigned depth);
#define CONTEXT_NODE(ID)                                                       \
  ManglingError mangle##ID(Node *node, unsigned depth);                        \
//    ManglingError mangle##ID(Node *node, unsigned depth, EntityContext &ctx);
#include "swift/Demangling/DemangleNodes.def"

public:
  Remangler(SymbolicResolver Resolver, NodeFactory &Factory)
       : RemanglerBase(Factory), Resolver(Resolver) { }

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
};

bool Remangler::trySubstitution(Node *node, SubstitutionEntry &entry,
                                bool treatAsIdentifier) {
  if (mangleStandardSubstitution(node))
    return true;

  // Go ahead and initialize the substitution entry.
  entry.setNode(node, treatAsIdentifier);

  int Idx = findSubstitution(entry);
  if (Idx < 0)
    return false;

  if (Idx >= 26) {
    Buffer << 'A';
    mangleIndex(Idx - 26);
    return true;
  }
  char SubstChar = Idx + 'A';
  StringRef Subst(&SubstChar, 1);
  if (!SubstMerging.tryMergeSubst(*this, Subst, /*isStandardSubst*/ false)) {
    Buffer << 'A' << Subst;
  }
  return true;
}

void Remangler::mangleIdentifierImpl(Node *node, bool isOperator) {
  SubstitutionEntry entry;
  if (trySubstitution(node, entry, /*treatAsIdentifier*/ true)) return;
  if (isOperator) {
    Mangle::mangleIdentifier(*this,
                              Mangle::translateOperator(node->getText()));
  } else {
    Mangle::mangleIdentifier(*this, node->getText());
  }
  addSubstitution(entry);
}

bool Remangler::mangleStandardSubstitution(Node *node) {
  if (node->getKind() != Node::Kind::Structure
      && node->getKind() != Node::Kind::Class
      && node->getKind() != Node::Kind::Enum
      && node->getKind() != Node::Kind::Protocol)
    return false;

  Node *context = node->getFirstChild();
  if (context->getKind() != Node::Kind::Module
      || context->getText() != STDLIB_NAME)
    return false;

  // Ignore private stdlib names
  if (node->getChild(1)->getKind() != Node::Kind::Identifier)
    return false;

  if (auto Subst = getStandardTypeSubst(
          node->getChild(1)->getText(), /*allowConcurrencyManglings=*/true)) {
    if (!SubstMerging.tryMergeSubst(*this, *Subst, /*isStandardSubst*/ true)) {
      Buffer << 'S' << *Subst;
    }
    return true;
  }
  return false;
}

void Remangler::mangleDependentGenericParamIndex(Node *node,
                                                 const char *nonZeroPrefix,
                                                 char zeroOp) {
  if (node->getKind() == Node::Kind::ConstrainedExistentialSelf) {
    Buffer << 's';
    return;
  }

  auto paramDepth = node->getChild(0)->getIndex();
  auto index = node->getChild(1)->getIndex();

  if (paramDepth != 0) {
    Buffer << nonZeroPrefix << 'd';
    mangleIndex(paramDepth - 1);
    mangleIndex(index);
    return;
  }
  if (index != 0) {
    Buffer << nonZeroPrefix;
    mangleIndex(index - 1);
    return;
  }
  // depth == index == 0
  Buffer << zeroOp;
}

ManglingErrorOr<std::pair<int, Node *>>
Remangler::mangleConstrainedType(Node *node, unsigned depth) {
  if (node->getKind() == Node::Kind::Type)
    node = getChildOfType(node);

  SubstitutionEntry entry;
  if (trySubstitution(node, entry))
    return std::pair<int, Node *>{-1, nullptr};

  Vector<Node *> Chain;
  while (node->getKind() == Node::Kind::DependentMemberType) {
    Chain.push_back(node->getChild(1), Factory);
    node = getChildOfType(node->getFirstChild());
  }

  if (node->getKind() != Node::Kind::DependentGenericParamType &&
      node->getKind() != Node::Kind::ConstrainedExistentialSelf) {
    RETURN_IF_ERROR(mangle(node, depth + 1));
    if (!Chain.size())
      return std::pair<int, Node *>{-1, nullptr};
    node = nullptr;
  }

  const char *ListSeparator = (Chain.size() > 1 ? "_" : "");
  for (unsigned i = 1, n = Chain.size(); i <= n; ++i) {
    Node *DepAssocTyRef = Chain[n - i];
    RETURN_IF_ERROR(mangle(DepAssocTyRef, depth + 1));
    Buffer << ListSeparator;
    ListSeparator = "";
  }
  if (!Chain.empty())
    addSubstitution(entry);
  return std::pair<int, Node *>{(int)Chain.size(), node};
}

ManglingError Remangler::mangleAnyGenericType(Node *node, StringRef TypeOp,
                                              unsigned depth) {
  SubstitutionEntry entry;
  if (!trySubstitution(node, entry)) {
    RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
    Buffer << TypeOp;
    addSubstitution(entry);
  }
  return ManglingError::Success;
}

ManglingError Remangler::mangleAnyNominalType(Node *node, unsigned depth) {
  if (depth > Remangler::MaxDepth) {
    return MANGLING_ERROR(ManglingError::TooComplex, node);
  }

  if (isSpecialized(node)) {
    SubstitutionEntry entry;
    if (trySubstitution(node, entry))
      return ManglingError::Success;

    auto unspec = getUnspecialized(node, Factory);
    if (!unspec.isSuccess())
      return unspec.error();
    NodePointer unboundType = unspec.result();
    RETURN_IF_ERROR(mangleAnyNominalType(unboundType, depth + 1));
    char Separator = 'y';
    RETURN_IF_ERROR(mangleGenericArgs(node, Separator, depth + 1));

    if (node->getNumChildren() == 3) {
      // Retroactive conformances.
      auto listNode = node->getChild(2);
      for (size_t Idx = 0, Num = listNode->getNumChildren(); Idx < Num; ++Idx) {
        RETURN_IF_ERROR(mangle(listNode->getChild(Idx), depth + 1));
      }
    }

    Buffer << 'G';
    addSubstitution(entry);
    return ManglingError::Success;
  }
  switch (node->getKind()) {
  case Node::Kind::Structure:
    return mangleAnyGenericType(node, "V", depth);
  case Node::Kind::Enum:
    return mangleAnyGenericType(node, "O", depth);
  case Node::Kind::Class:
    return mangleAnyGenericType(node, "C", depth);
  case Node::Kind::OtherNominalType:
    return mangleAnyGenericType(node, "XY", depth);
  case Node::Kind::TypeAlias:
    return mangleAnyGenericType(node, "a", depth);
  default:
    return MANGLING_ERROR(ManglingError::BadNominalTypeKind, node);
  }
}

ManglingError Remangler::mangleGenericArgs(Node *node, char &Separator,
                                           unsigned depth,
                                           bool fullSubstitutionMap) {
  switch (node->getKind()) {
    case Node::Kind::Protocol:
      // A protocol cannot be the parent of a nominal type, so this case should
      // never be hit by valid swift code. But the indexer might generate a URL
      // from invalid swift code, which has a bound generic inside a protocol.
      // The ASTMangler treats a protocol like any other nominal type in this
      // case, so we also support it in the remangler.
    case Node::Kind::Structure:
    case Node::Kind::Enum:
    case Node::Kind::Class:
    case Node::Kind::TypeAlias:
      if (node->getKind() == Node::Kind::TypeAlias)
        fullSubstitutionMap = true;

      RETURN_IF_ERROR(mangleGenericArgs(node->getChild(0), Separator, depth + 1,
                                        fullSubstitutionMap));
      Buffer << Separator;
      Separator = '_';
      break;

    case Node::Kind::Function:
    case Node::Kind::Getter:
    case Node::Kind::Setter:
    case Node::Kind::WillSet:
    case Node::Kind::DidSet:
    case Node::Kind::ReadAccessor:
    case Node::Kind::ModifyAccessor:
    case Node::Kind::UnsafeAddressor:
    case Node::Kind::UnsafeMutableAddressor:
    case Node::Kind::Allocator:
    case Node::Kind::Constructor:
    case Node::Kind::Destructor:
    case Node::Kind::Variable:
    case Node::Kind::Subscript:
    case Node::Kind::ExplicitClosure:
    case Node::Kind::ImplicitClosure:
    case Node::Kind::DefaultArgumentInitializer:
    case Node::Kind::Initializer:
    case Node::Kind::PropertyWrapperBackingInitializer:
    case Node::Kind::PropertyWrapperInitFromProjectedValue:
    case Node::Kind::Static:
    case Node::Kind::RuntimeAttributeGenerator:
      if (!fullSubstitutionMap)
        break;

      RETURN_IF_ERROR(mangleGenericArgs(node->getChild(0), Separator, depth + 1,
                                        fullSubstitutionMap));
      if (Demangle::nodeConsumesGenericArgs(node)) {
        Buffer << Separator;
        Separator = '_';
      }
      break;

    case Node::Kind::BoundGenericOtherNominalType:
    case Node::Kind::BoundGenericStructure:
    case Node::Kind::BoundGenericEnum:
    case Node::Kind::BoundGenericClass:
    case Node::Kind::BoundGenericProtocol:
    case Node::Kind::BoundGenericTypeAlias: {
      if (node->getKind() == Node::Kind::BoundGenericTypeAlias)
        fullSubstitutionMap = true;

      NodePointer unboundType = node->getChild(0);
      DEMANGLER_ASSERT(unboundType->getKind() == Node::Kind::Type, node);
      NodePointer nominalType = unboundType->getChild(0);
      NodePointer parentOrModule = nominalType->getChild(0);
      RETURN_IF_ERROR(mangleGenericArgs(parentOrModule, Separator, depth + 1,
                                        fullSubstitutionMap));
      Buffer << Separator;
      Separator = '_';
      RETURN_IF_ERROR(mangleChildNodes(node->getChild(1), depth + 1));
      break;
    }

    case Node::Kind::ConstrainedExistential: {
      Buffer << Separator;
      Separator = '_';
      RETURN_IF_ERROR(mangleChildNodes(node->getChild(1), depth + 1));
      break;
    }

    case Node::Kind::BoundGenericFunction: {
      fullSubstitutionMap = true;

      NodePointer unboundFunction = node->getChild(0);
      DEMANGLER_ASSERT(unboundFunction->getKind() == Node::Kind::Function ||
                           unboundFunction->getKind() ==
                               Node::Kind::Constructor,
                       node);
      NodePointer parentOrModule = unboundFunction->getChild(0);
      RETURN_IF_ERROR(mangleGenericArgs(parentOrModule, Separator, depth + 1,
                                        fullSubstitutionMap));
      Buffer << Separator;
      Separator = '_';
      RETURN_IF_ERROR(mangleChildNodes(node->getChild(1), depth + 1));
      break;
    }

    case Node::Kind::Extension:
      RETURN_IF_ERROR(mangleGenericArgs(node->getChild(1), Separator, depth + 1,
                                        fullSubstitutionMap));
      break;

    default:
      break;
  }

  return ManglingError::Success;
}

ManglingError Remangler::mangleAbstractStorage(Node *node,
                                               StringRef accessorCode,
                                               unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  switch (node->getKind()) {
    case Node::Kind::Subscript: Buffer << "i"; break;
    case Node::Kind::Variable: Buffer << "v"; break;
    default:
      return MANGLING_ERROR(ManglingError::NotAStorageNode, node);
  }
  Buffer << accessorCode;
  return ManglingError::Success;
}

ManglingError Remangler::mangleAllocator(Node *node, unsigned depth) {
  return mangleAnyConstructor(node, 'C', depth + 1);
}

ManglingError Remangler::mangleArgumentTuple(Node *node, unsigned depth) {
  Node *Child = skipType(getSingleChild(node));
  if (Child->getKind() == Node::Kind::Tuple &&
      Child->getNumChildren() == 0) {
    Buffer << 'y';
    return ManglingError::Success;
  }
  return mangle(Child, depth + 1);
}

ManglingError Remangler::mangleAssociatedType(Node *node, unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleAssociatedTypeRef(Node *node, unsigned depth) {
  SubstitutionEntry entry;
  if (trySubstitution(node, entry))
    return ManglingError::Success;
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "Qa";
  addSubstitution(entry);
  return ManglingError::Success;
}

ManglingError Remangler::mangleAssociatedTypeDescriptor(Node *node,
                                                        unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "Tl";
  return ManglingError::Success;
}

ManglingError Remangler::mangleAssociatedConformanceDescriptor(Node *node,
                                                               unsigned depth) {
  RETURN_IF_ERROR(mangle(node->getChild(0), depth + 1));
  RETURN_IF_ERROR(mangle(node->getChild(1), depth + 1));
  RETURN_IF_ERROR(manglePureProtocol(node->getChild(2), depth + 1));
  Buffer << "Tn";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleDefaultAssociatedConformanceAccessor(Node *node,
                                                      unsigned depth) {
  RETURN_IF_ERROR(mangle(node->getChild(0), depth + 1));
  RETURN_IF_ERROR(mangle(node->getChild(1), depth + 1));
  RETURN_IF_ERROR(manglePureProtocol(node->getChild(2), depth + 1));
  Buffer << "TN";
  return ManglingError::Success;
}

ManglingError Remangler::mangleBaseConformanceDescriptor(Node *node,
                                                         unsigned depth) {
  RETURN_IF_ERROR(mangle(node->getChild(0), depth + 1));
  RETURN_IF_ERROR(manglePureProtocol(node->getChild(1), depth + 1));
  Buffer << "Tb";
  return ManglingError::Success;
}

ManglingError Remangler::mangleAssociatedTypeMetadataAccessor(Node *node,
                                                              unsigned depth) {
  RETURN_IF_ERROR(
      mangleChildNodes(node, depth + 1)); // protocol conformance, identifier
  Buffer << "Wt";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleDefaultAssociatedTypeMetadataAccessor(Node *node,
                                                       unsigned depth) {
  RETURN_IF_ERROR(
      mangleChildNodes(node, depth + 1)); // protocol conformance, identifier
  Buffer << "TM";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleAssociatedTypeWitnessTableAccessor(Node *node,
                                                    unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(
      node, depth + 1)); // protocol conformance, type, protocol
  Buffer << "WT";
  return ManglingError::Success;
}

ManglingError Remangler::mangleBaseWitnessTableAccessor(Node *node,
                                                        unsigned depth) {
  RETURN_IF_ERROR(
      mangleChildNodes(node, depth + 1)); // protocol conformance, protocol
  Buffer << "Wb";
  return ManglingError::Success;
}

ManglingError Remangler::mangleAutoClosureType(Node *node, unsigned depth) {
  RETURN_IF_ERROR(
      mangleChildNodesReversed(node, depth + 1)); // argument tuple, result type
  Buffer << "XK";
  return ManglingError::Success;
}

ManglingError Remangler::mangleEscapingAutoClosureType(Node *node,
                                                       unsigned depth) {
  RETURN_IF_ERROR(
      mangleChildNodesReversed(node, depth + 1)); // argument tuple, result type
  Buffer << "XA";
  return ManglingError::Success;
}

ManglingError Remangler::mangleNoEscapeFunctionType(Node *node,
                                                    unsigned depth) {
  RETURN_IF_ERROR(
      mangleChildNodesReversed(node, depth + 1)); // argument tuple, result type
  Buffer << "XE";
  return ManglingError::Success;
}

ManglingError Remangler::mangleBoundGenericClass(Node *node, unsigned depth) {
  return mangleAnyNominalType(node, depth + 1);
}

ManglingError Remangler::mangleBoundGenericEnum(Node *node, unsigned depth) {
  Node *Enum = node->getChild(0)->getChild(0);
  DEMANGLER_ASSERT(Enum->getKind() == Node::Kind::Enum, node);
  Node *Mod = Enum->getChild(0);
  Node *Id = Enum->getChild(1);
  if (Mod->getKind() == Node::Kind::Module && Mod->getText() == STDLIB_NAME &&
      Id->getKind() == Node::Kind::Identifier && Id->getText() == "Optional") {
    SubstitutionEntry entry;
    if (trySubstitution(node, entry))
      return ManglingError::Success;
    RETURN_IF_ERROR(mangleSingleChildNode(node->getChild(1), depth + 1));
    Buffer << "Sg";
    addSubstitution(entry);
    return ManglingError::Success;
  }
  return mangleAnyNominalType(node, depth + 1);
}

ManglingError Remangler::mangleBoundGenericStructure(Node *node,
                                                     unsigned depth) {
  return mangleAnyNominalType(node, depth + 1);
}

ManglingError Remangler::mangleBoundGenericOtherNominalType(Node *node,
                                                            unsigned depth) {
  return mangleAnyNominalType(node, depth + 1);
}

ManglingError Remangler::mangleBoundGenericProtocol(Node *node,
                                                    unsigned depth) {
  return mangleAnyNominalType(node, depth + 1);
}

ManglingError Remangler::mangleBoundGenericTypeAlias(Node *node,
                                                     unsigned depth) {
  return mangleAnyNominalType(node, depth + 1);
}

ManglingError Remangler::mangleBoundGenericFunction(Node *node,
                                                    unsigned depth) {
  SubstitutionEntry entry;
  if (trySubstitution(node, entry))
    return ManglingError::Success;

  auto unspec = getUnspecialized(node, Factory);
  if (!unspec.isSuccess())
    return unspec.error();
  NodePointer unboundFunction = unspec.result();
  RETURN_IF_ERROR(mangleFunction(unboundFunction, depth + 1));
  char Separator = 'y';
  RETURN_IF_ERROR(mangleGenericArgs(node, Separator, depth + 1));
  Buffer << 'G';
  addSubstitution(entry);
  return ManglingError::Success;
}

ManglingError Remangler::mangleBuiltinTypeName(Node *node, unsigned depth) {
  Buffer << 'B';
  StringRef text = node->getText();

  if (text == BUILTIN_TYPE_NAME_BRIDGEOBJECT) {
    Buffer << 'b';
  } else if (text == BUILTIN_TYPE_NAME_UNSAFEVALUEBUFFER) {
    Buffer << 'B';
  } else if (text == BUILTIN_TYPE_NAME_UNKNOWNOBJECT) {
    Buffer << 'O';
  } else if (text == BUILTIN_TYPE_NAME_NATIVEOBJECT) {
    Buffer << 'o';
  } else if (text == BUILTIN_TYPE_NAME_RAWPOINTER) {
    Buffer << 'p';
  } else if (text == BUILTIN_TYPE_NAME_RAWUNSAFECONTINUATION) {
    Buffer << 'c';
  } else if (text == BUILTIN_TYPE_NAME_JOB) {
    Buffer << 'j';
  } else if (text == BUILTIN_TYPE_NAME_DEFAULTACTORSTORAGE) {
    Buffer << 'D';
  } else if (text == BUILTIN_TYPE_NAME_NONDEFAULTDISTRIBUTEDACTORSTORAGE) {
    Buffer << 'd';
  } else if (text == BUILTIN_TYPE_NAME_EXECUTOR) {
    Buffer << 'e';
  } else if (text == BUILTIN_TYPE_NAME_SILTOKEN) {
    Buffer << 't';
  } else if (text == BUILTIN_TYPE_NAME_INTLITERAL) {
    Buffer << 'I';
  } else if (text == BUILTIN_TYPE_NAME_WORD) {
    Buffer << 'w';
  } else if (text.consume_front(BUILTIN_TYPE_NAME_INT)) {
    Buffer << 'i' << text << '_';
  } else if (text.consume_front(BUILTIN_TYPE_NAME_FLOAT)) {
    Buffer << 'f' << text << '_';
  } else if (text.consume_front(BUILTIN_TYPE_NAME_VEC)) {
    // Avoid using StringRef::split because its definition is not
    // provided in the header so that it requires linking with libSupport.a.
    size_t splitIdx = text.find('x');
    auto element = text.substr(splitIdx).substr(1);
    if (element == "RawPointer") {
      Buffer << 'p';
    } else if (element.consume_front("FPIEEE")) {
      Buffer << 'f' << element << '_';
    } else if (element.consume_front("Int")) {
      Buffer << 'i' << element << '_';
    } else {
      return MANGLING_ERROR(ManglingError::UnexpectedBuiltinVectorType, node);
    }
    Buffer << "Bv" << text.substr(0, splitIdx) << '_';
  } else {
    return MANGLING_ERROR(ManglingError::UnexpectedBuiltinType, node);
  }
  return ManglingError::Success;
}

ManglingError Remangler::mangleCFunctionPointer(Node *node, unsigned depth) {
  if (node->getNumChildren() > 0 &&
      node->getFirstChild()->getKind() == Node::Kind::ClangType) {
    for (size_t Idx = node->getNumChildren() - 1; Idx >= 1; --Idx) {
      RETURN_IF_ERROR(mangleChildNode(node, Idx, depth + 1));
    }
    Buffer << "XzC";
    return mangleClangType(node->getFirstChild(), depth + 1);
  }
  RETURN_IF_ERROR(
      mangleChildNodesReversed(node, depth + 1)); // argument tuple, result type
  Buffer << "XC";
  return ManglingError::Success;
}

ManglingError Remangler::mangleClass(Node *node, unsigned depth) {
  return mangleAnyNominalType(node, depth + 1);
}

ManglingError Remangler::mangleAnyConstructor(Node *node, char kindOp,
                                              unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "f" << kindOp;
  return ManglingError::Success;
}

ManglingError Remangler::mangleConstructor(Node *node, unsigned depth) {
  return mangleAnyConstructor(node, 'c', depth);
}

ManglingError Remangler::mangleCoroutineContinuationPrototype(Node *node,
                                                              unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "TC";
  return ManglingError::Success;
}

ManglingError
Remangler::manglePredefinedObjCAsyncCompletionHandlerImpl(Node *node,
                                                          unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "TZ";
  return ManglingError::Success;
}

ManglingError Remangler::mangleObjCAsyncCompletionHandlerImpl(Node *node,
                                                              unsigned depth) {
  RETURN_IF_ERROR(mangleChildNode(node, 0, depth + 1));
  RETURN_IF_ERROR(mangleChildNode(node, 1, depth + 1));
  if (node->getNumChildren() == 4)
    RETURN_IF_ERROR(mangleChildNode(node, 3, depth + 1));
  Buffer << "Tz";
  return mangleChildNode(node, 2, depth + 1);
}

ManglingError Remangler::mangleDeallocator(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "fD";
  return ManglingError::Success;
}

ManglingError Remangler::mangleDeclContext(Node *node, unsigned depth) {
  return mangleSingleChildNode(node, depth + 1);
}

ManglingError Remangler::mangleDefaultArgumentInitializer(Node *node,
                                                          unsigned depth) {
  RETURN_IF_ERROR(mangleChildNode(node, 0, depth + 1));
  Buffer << "fA";
  return mangleChildNode(node, 1, depth + 1);
}

ManglingError Remangler::mangleAsyncFunctionPointer(Node *node,
                                                    unsigned depth) {
  Buffer << "Tu";
  return ManglingError::Success;
}

ManglingError Remangler::mangleDependentAssociatedTypeRef(Node *node,
                                                          unsigned depth) {
  RETURN_IF_ERROR(mangleIdentifier(node->getFirstChild(), depth));
  if (node->getNumChildren() > 1)
    return mangleChildNode(node, 1, depth + 1);
  return ManglingError::Success;
}

ManglingError
Remangler::mangleDependentGenericConformanceRequirement(Node *node,
                                                        unsigned depth) {
  DEMANGLER_ASSERT(node->getNumChildren() == 2, node);
  Node *ProtoOrClass = node->getChild(1);
  DEMANGLER_ASSERT(ProtoOrClass->hasChildren(), ProtoOrClass);
  if (ProtoOrClass->getFirstChild()->getKind() == Node::Kind::Protocol) {
    RETURN_IF_ERROR(manglePureProtocol(ProtoOrClass, depth + 1));
    auto Mangling = mangleConstrainedType(node->getChild(0), depth + 1);
    if (!Mangling.isSuccess())
      return Mangling.error();
    auto NumMembersAndParamIdx = Mangling.result();
    DEMANGLER_ASSERT(
        NumMembersAndParamIdx.first < 0 || NumMembersAndParamIdx.second, node);
    switch (NumMembersAndParamIdx.first) {
    case -1:
      Buffer << "RQ";
      return ManglingError::Success; // substitution
    case 0:
      Buffer << "R";
      break;
    case 1:
      Buffer << "Rp";
      break;
    default:
      Buffer << "RP";
      break;
    }
    mangleDependentGenericParamIndex(NumMembersAndParamIdx.second);
    return ManglingError::Success;
  }
  RETURN_IF_ERROR(mangle(ProtoOrClass, depth + 1));
  auto Mangling = mangleConstrainedType(node->getChild(0), depth + 1);
  if (!Mangling.isSuccess())
    return Mangling.error();
  auto NumMembersAndParamIdx = Mangling.result();
  DEMANGLER_ASSERT(
      NumMembersAndParamIdx.first < 0 || NumMembersAndParamIdx.second, node);
  switch (NumMembersAndParamIdx.first) {
  case -1:
    Buffer << "RB";
    return ManglingError::Success; // substitution
  case 0:
    Buffer << "Rb";
    break;
  case 1:
    Buffer << "Rc";
    break;
  default:
    Buffer << "RC";
    break;
  }
  mangleDependentGenericParamIndex(NumMembersAndParamIdx.second);
  return ManglingError::Success;
}

ManglingError Remangler::mangleDependentGenericParamCount(Node *node,
                                                          unsigned depth) {
  // handled inline in DependentGenericSignature
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleDependentGenericParamType(Node *node,
                                                         unsigned depth) {
  if (node->getChild(0)->getIndex() == 0
      && node->getChild(1)->getIndex() == 0) {
    Buffer << 'x';
    return ManglingError::Success;
  }
  Buffer << 'q';
  mangleDependentGenericParamIndex(node);
  return ManglingError::Success;
}

ManglingError
Remangler::mangleDependentGenericSameTypeRequirement(Node *node,
                                                     unsigned depth) {
  RETURN_IF_ERROR(mangleChildNode(node, 1, depth + 1));
  auto Mangling = mangleConstrainedType(node->getChild(0), depth + 1);
  if (!Mangling.isSuccess())
    return Mangling.error();
  auto NumMembersAndParamIdx = Mangling.result();
  DEMANGLER_ASSERT(
      NumMembersAndParamIdx.first < 0 || NumMembersAndParamIdx.second, node);
  switch (NumMembersAndParamIdx.first) {
  case -1:
    Buffer << "RS";
    return ManglingError::Success; // substitution
  case 0:
    Buffer << "Rs";
    break;
  case 1:
    Buffer << "Rt";
    break;
  default:
    Buffer << "RT";
    break;
  }
  mangleDependentGenericParamIndex(NumMembersAndParamIdx.second);
  return ManglingError::Success;
}

ManglingError
Remangler::mangleDependentGenericSameShapeRequirement(Node *node,
                                                      unsigned depth) {
  RETURN_IF_ERROR(mangleChildNode(node, 1, depth + 1));
  auto Mangling = mangleConstrainedType(node->getChild(0), depth + 1);
  if (!Mangling.isSuccess())
    return Mangling.error();
  auto NumMembersAndParamIdx = Mangling.result();
  DEMANGLER_ASSERT(
      NumMembersAndParamIdx.first < 0 || NumMembersAndParamIdx.second, node);
  switch (NumMembersAndParamIdx.first) {
  case 0:
    Buffer << "Rh";
    break;
  default:
    assert(false && "Invalid same-shape requirement");
    return ManglingError::AssertionFailed;
  }
  mangleDependentGenericParamIndex(NumMembersAndParamIdx.second);
  return ManglingError::Success;
}

ManglingError
Remangler::mangleDependentGenericLayoutRequirement(Node *node, unsigned depth) {
  auto Mangling = mangleConstrainedType(node->getChild(0), depth + 1);
  if (!Mangling.isSuccess())
    return Mangling.error();
  auto NumMembersAndParamIdx = Mangling.result();
  DEMANGLER_ASSERT(
      NumMembersAndParamIdx.first < 0 || NumMembersAndParamIdx.second, node);
  switch (NumMembersAndParamIdx.first) {
    case -1: Buffer << "RL"; break; // substitution
    case 0: Buffer << "Rl"; break;
    case 1: Buffer << "Rm"; break;
    default: Buffer << "RM"; break;
  }
  // If not a substitution, mangle the dependent generic param index.
  if (NumMembersAndParamIdx.first != -1)
    mangleDependentGenericParamIndex(NumMembersAndParamIdx.second);
  DEMANGLER_ASSERT(node->getChild(1)->getKind() == Node::Kind::Identifier,
                   node);
  DEMANGLER_ASSERT(node->getChild(1)->getText().size() == 1, node);
  Buffer << node->getChild(1)->getText()[0];
  if (node->getNumChildren() >= 3)
    RETURN_IF_ERROR(mangleChildNode(node, 2, depth + 1));
  if (node->getNumChildren() >= 4)
    RETURN_IF_ERROR(mangleChildNode(node, 3, depth + 1));
  return ManglingError::Success;
}

ManglingError Remangler::mangleDependentGenericSignature(Node *node,
                                                         unsigned depth) {
  size_t ParamCountEnd = 0;
  for (size_t Idx = 0, Num = node->getNumChildren(); Idx < Num; ++Idx) {
    Node *Child = node->getChild(Idx);
    if (Child->getKind() == Node::Kind::DependentGenericParamCount) {
      ParamCountEnd = Idx + 1;
    } else {
      // requirement
      RETURN_IF_ERROR(mangleChildNode(node, Idx, depth + 1));
    }
  }
  // If there's only one generic param, mangle nothing.
  if (ParamCountEnd == 1 && node->getChild(0)->getIndex() == 1) {
    Buffer << 'l';
    return ManglingError::Success;
  }

  // Remangle generic params.
  Buffer << 'r';
  for (size_t Idx = 0; Idx < ParamCountEnd; ++Idx) {
    Node *Count = node->getChild(Idx);
    if (Count->getIndex() > 0) {
      mangleIndex(Count->getIndex() - 1);
    } else {
      Buffer << 'z';
    }
  }
  Buffer << 'l';
  return ManglingError::Success;
}

ManglingError Remangler::mangleDependentGenericParamPackMarker(Node *node,
                                                      unsigned depth) {
  DEMANGLER_ASSERT(node->getNumChildren() == 1, node);
  DEMANGLER_ASSERT(node->getChild(0)->getKind() == Node::Kind::Type, node);
  Buffer << "Rv";
  mangleDependentGenericParamIndex(node->getChild(0)->getChild(0));
  return ManglingError::Success;
}

ManglingError Remangler::mangleDependentGenericType(Node *node,
                                                    unsigned depth) {
  RETURN_IF_ERROR(
      mangleChildNodesReversed(node, depth + 1)); // type, generic signature
  Buffer << 'u';
  return ManglingError::Success;
}

ManglingError Remangler::mangleDependentMemberType(Node *node, unsigned depth) {
  auto Mangling = mangleConstrainedType(node, depth + 1);
  if (!Mangling.isSuccess())
    return Mangling.error();
  auto NumMembersAndParamIdx = Mangling.result();
  switch (NumMembersAndParamIdx.first) {
    case -1:
      break; // substitution
    case 0:
      return MANGLING_ERROR(ManglingError::WrongDependentMemberType, node);
    case 1:
      Buffer << 'Q';
      if (auto dependentBase = NumMembersAndParamIdx.second) {
        mangleDependentGenericParamIndex(dependentBase, "y", 'z');
      } else {
        Buffer << 'x';
      }
      break;
    default:
      Buffer << 'Q';
      if (auto dependentBase = NumMembersAndParamIdx.second) {
        mangleDependentGenericParamIndex(dependentBase, "Y", 'Z');
      } else {
        Buffer << 'X';
      }
      break;
  }

  return ManglingError::Success;
}

ManglingError Remangler::mangleDependentPseudogenericSignature(Node *node,
                                                               unsigned depth) {
  return mangleDependentGenericSignature(node, depth + 1);
}

ManglingError Remangler::mangleDestructor(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "fd";
  return ManglingError::Success;
}

ManglingError Remangler::mangleDidSet(Node *node, unsigned depth) {
  return mangleAbstractStorage(node->getFirstChild(), "W", depth + 1);
}

ManglingError Remangler::mangleDirectness(Node *node, unsigned depth) {
  switch (node->getIndex()) {
  case unsigned(Directness::Direct):
    Buffer << 'd';
    break;
  case unsigned(Directness::Indirect):
    Buffer << 'i';
    break;
  default:
    return MANGLING_ERROR(ManglingError::BadDirectness, node);
  }
  return ManglingError::Success;
}

ManglingError Remangler::mangleDynamicAttribute(Node *node, unsigned depth) {
  Buffer << "TD";
  return ManglingError::Success;
}

ManglingError Remangler::mangleDirectMethodReferenceAttribute(Node *node,
                                                              unsigned depth) {
  Buffer << "Td";
  return ManglingError::Success;
}

ManglingError Remangler::mangleDynamicSelf(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1)); // type
  Buffer << "XD";
  return ManglingError::Success;
}

ManglingError Remangler::mangleEnum(Node *node, unsigned depth) {
  return mangleAnyNominalType(node, depth + 1);
}

ManglingError Remangler::mangleErrorType(Node *node, unsigned depth) {
  Buffer << "Xe";
  return ManglingError::Success;
}

ManglingError Remangler::mangleConstrainedExistential(Node *node,
                                                      unsigned int depth) {
  RETURN_IF_ERROR(mangleChildNode(node, 0, depth + 1));
  RETURN_IF_ERROR(mangleChildNode(node, 1, depth + 1));
  Buffer << "XP";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleConstrainedExistentialRequirementList(Node *node,
                                                       unsigned int depth) {
  assert(node->getNumChildren() > 0);
  bool FirstElem = true;
  for (size_t Idx = 0, Num = node->getNumChildren(); Idx < Num; ++Idx) {
    RETURN_IF_ERROR(mangleChildNode(node, Idx, depth + 1));
    mangleListSeparator(FirstElem);
  }
  return ManglingError::Success;
}

ManglingError Remangler::mangleConstrainedExistentialSelf(Node *node,
                                                          unsigned int depth) {
  Buffer << "s";
  return ManglingError::Success;
}

ManglingError Remangler::mangleExistentialMetatype(Node *node, unsigned depth) {
  if (node->getFirstChild()->getKind() == Node::Kind::MetatypeRepresentation) {
    RETURN_IF_ERROR(mangleChildNode(node, 1, depth + 1));
    Buffer << "Xm";
    return mangleChildNode(node, 0, depth + 1);
  } else {
    RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
    Buffer << "Xp";
  }
  return ManglingError::Success;
}

ManglingError Remangler::mangleExplicitClosure(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNode(node, 0, depth + 1)); // context
  RETURN_IF_ERROR(mangleChildNode(node, 2, depth + 1)); // type
  Buffer << "fU";
  return mangleChildNode(node, 1, depth + 1); // index
}

ManglingError Remangler::mangleExtension(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNode(node, 1, depth + 1));
  RETURN_IF_ERROR(mangleChildNode(node, 0, depth + 1));
  if (node->getNumChildren() == 3)
    RETURN_IF_ERROR(mangleChildNode(node, 2, depth + 1)); // generic signature
  Buffer << 'E';
  return ManglingError::Success;
}

ManglingError Remangler::mangleAnonymousContext(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNode(node, 1, depth + 1));
  RETURN_IF_ERROR(mangleChildNode(node, 0, depth + 1));
  if (node->getNumChildren() >= 3)
    RETURN_IF_ERROR(mangleTypeList(node->getChild(2), depth + 1));
  else
    Buffer << 'y';
  Buffer << "XZ";
  return ManglingError::Success;
}

ManglingError Remangler::mangleFieldOffset(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNode(node, 1, depth + 1)); // variable
  Buffer << "Wv";
  return mangleChildNode(node, 0, depth + 1); // directness
}

ManglingError Remangler::mangleEnumCase(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1)); // enum case
  Buffer << "WC";
  return ManglingError::Success;
}

ManglingError Remangler::mangleFullTypeMetadata(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "Mf";
  return ManglingError::Success;
}

ManglingError Remangler::mangleFunction(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNode(node, 0, depth + 1)); // context
  RETURN_IF_ERROR(mangleChildNode(node, 1, depth + 1)); // name

  bool hasLabels = node->getChild(2)->getKind() == Node::Kind::LabelList;
  Node *FuncType = getSingleChild(node->getChild(hasLabels ? 3 : 2));

  if (hasLabels)
    RETURN_IF_ERROR(mangleChildNode(node, 2, depth + 1)); // parameter labels

  if (FuncType->getKind() == Node::Kind::DependentGenericType) {
    RETURN_IF_ERROR(mangleFunctionSignature(
        getSingleChild(FuncType->getChild(1)), depth + 1));
    RETURN_IF_ERROR(
        mangleChildNode(FuncType, 0, depth + 1)); // generic signature
  } else {
    RETURN_IF_ERROR(mangleFunctionSignature(FuncType, depth + 1));
  }

  Buffer << "F";

  return ManglingError::Success;
}

ManglingError Remangler::mangleFunctionSignatureSpecialization(Node *node,
                                                               unsigned depth) {
  for (NodePointer Param : *node) {
    if (Param->getKind() == Node::Kind::FunctionSignatureSpecializationParam &&
        Param->getNumChildren() > 0) {
      Node *KindNd = Param->getChild(0);
      switch (FunctionSigSpecializationParamKind(KindNd->getIndex())) {
        case FunctionSigSpecializationParamKind::ConstantPropFunction:
        case FunctionSigSpecializationParamKind::ConstantPropGlobal:
          RETURN_IF_ERROR(mangleIdentifier(Param->getChild(1), depth + 1));
          break;
        case FunctionSigSpecializationParamKind::ConstantPropString: {
          NodePointer TextNd = Param->getChild(2);
          StringRef Text = TextNd->getText();
          if (!Text.empty() && (isDigit(Text[0]) || Text[0] == '_')) {
            std::string Buffer = "_";
            Buffer.append(Text.data(), Text.size());
            TextNd = Factory.createNode(Node::Kind::Identifier, Buffer);
          }
          RETURN_IF_ERROR(mangleIdentifier(TextNd, depth + 1));
          break;
        }
        case FunctionSigSpecializationParamKind::ClosureProp:
        case FunctionSigSpecializationParamKind::ConstantPropKeyPath:
          RETURN_IF_ERROR(mangleIdentifier(Param->getChild(1), depth + 1));
          for (unsigned i = 2, e = Param->getNumChildren(); i != e; ++i) {
            RETURN_IF_ERROR(mangleType(Param->getChild(i), depth + 1));
          }
          break;
        default:
          break;
      }
    }
  }
  Buffer << "Tf";
  bool returnValMangled = false;
  for (NodePointer Child : *node) {
    if (Child->getKind() == Node::Kind::FunctionSignatureSpecializationReturn) {
      Buffer << '_';
      returnValMangled = true;
    }
    RETURN_IF_ERROR(mangle(Child, depth + 1));

    if (Child->getKind() == Node::Kind::SpecializationPassID &&
        node->hasIndex()) {
      Buffer << node->getIndex();
    }
  }
  if (!returnValMangled)
    Buffer << "_n";

  return ManglingError::Success;
}

ManglingError
Remangler::mangleFunctionSignatureSpecializationReturn(Node *node,
                                                       unsigned depth) {
  return mangleFunctionSignatureSpecializationParam(node, depth + 1);
}

ManglingError
Remangler::mangleFunctionSignatureSpecializationParam(Node *node,
                                                      unsigned depth) {
  if (!node->hasChildren()) {
    Buffer << 'n';
    return ManglingError::Success;
  }

  // The first child is always a kind that specifies the type of param that we
  // have.
  Node *KindNd = node->getChild(0);
  unsigned kindValue = KindNd->getIndex();
  auto kind = FunctionSigSpecializationParamKind(kindValue);

  switch (kind) {
    case FunctionSigSpecializationParamKind::ConstantPropFunction:
      Buffer << "pf";
      break;
    case FunctionSigSpecializationParamKind::ConstantPropGlobal:
      Buffer << "pg";
      break;
    case FunctionSigSpecializationParamKind::ConstantPropInteger:
      Buffer << "pi" << node->getChild(1)->getText();
      break;
    case FunctionSigSpecializationParamKind::ConstantPropFloat:
      Buffer << "pd" << node->getChild(1)->getText();
      break;
    case FunctionSigSpecializationParamKind::ConstantPropString: {
      Buffer << "ps";
      StringRef encodingStr = node->getChild(1)->getText();
      if (encodingStr == "u8") {
        Buffer << 'b';
      } else if (encodingStr == "u16") {
        Buffer << 'w';
      } else if (encodingStr == "objc") {
        Buffer << 'c';
      } else {
        return MANGLING_ERROR(ManglingError::UnknownEncoding, node);
      }
      break;
    }
    case FunctionSigSpecializationParamKind::ConstantPropKeyPath:
      Buffer << "pk";
      break;
    case FunctionSigSpecializationParamKind::ClosureProp:
      Buffer << 'c';
      break;
    case FunctionSigSpecializationParamKind::BoxToValue:
      Buffer << 'i';
      break;
    case FunctionSigSpecializationParamKind::BoxToStack:
      Buffer << 's';
      break;
    case FunctionSigSpecializationParamKind::InOutToOut:
      Buffer << 'r';
      break;
    case FunctionSigSpecializationParamKind::SROA:
      Buffer << 'x';
      break;
    default:
      if (kindValue &
          unsigned(
              FunctionSigSpecializationParamKind::ExistentialToGeneric)) {
        Buffer << 'e';
        if (kindValue & unsigned(FunctionSigSpecializationParamKind::Dead))
          Buffer << 'D';
        if (kindValue &
            unsigned(FunctionSigSpecializationParamKind::OwnedToGuaranteed))
          Buffer << 'G';
        if (kindValue &
            unsigned(FunctionSigSpecializationParamKind::GuaranteedToOwned))
          Buffer << 'O';
      } else if (kindValue &
                 unsigned(FunctionSigSpecializationParamKind::Dead)) {
        Buffer << 'd';
        if (kindValue &
            unsigned(FunctionSigSpecializationParamKind::OwnedToGuaranteed))
          Buffer << 'G';
        if (kindValue &
            unsigned(FunctionSigSpecializationParamKind::GuaranteedToOwned))
          Buffer << 'O';
      } else if (kindValue &
              unsigned(FunctionSigSpecializationParamKind::OwnedToGuaranteed)) {
        Buffer << 'g';
      } else if (kindValue &
                 unsigned(
                     FunctionSigSpecializationParamKind::GuaranteedToOwned)) {
        Buffer << 'o';
      }
      if (kindValue & unsigned(FunctionSigSpecializationParamKind::SROA))
        Buffer << 'X';
      break;
  }

  return ManglingError::Success;
}

ManglingError
Remangler::mangleFunctionSignatureSpecializationParamKind(Node *node,
                                                          unsigned depth) {
  // handled inline
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError
Remangler::mangleFunctionSignatureSpecializationParamPayload(Node *node,
                                                             unsigned depth) {
  // handled inline
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleFunctionType(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleFunctionSignature(node, depth + 1));
  Buffer << 'c';
  return ManglingError::Success;
}

ManglingError Remangler::mangleGenericProtocolWitnessTable(Node *node,
                                                           unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "WG";
  return ManglingError::Success;
}

ManglingError Remangler::mangleGenericProtocolWitnessTableInstantiationFunction(
    Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "WI";
  return ManglingError::Success;
}

ManglingError Remangler::mangleResilientProtocolWitnessTable(Node *node,
                                                             unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "Wr";
  return ManglingError::Success;
}

ManglingError Remangler::mangleGenericPartialSpecialization(Node *node,
                                                            unsigned depth) {
  for (NodePointer Child : *node) {
    if (Child->getKind() == Node::Kind::GenericSpecializationParam) {
      RETURN_IF_ERROR(mangleChildNode(Child, 0, depth + 1));
      break;
    }
  }
  Buffer << (node->getKind() ==
        Node::Kind::GenericPartialSpecializationNotReAbstracted ? "TP" : "Tp");
  for (NodePointer Child : *node) {
    if (Child->getKind() != Node::Kind::GenericSpecializationParam)
      RETURN_IF_ERROR(mangle(Child, depth + 1));
  }

  return ManglingError::Success;
}

ManglingError
Remangler::mangleGenericPartialSpecializationNotReAbstracted(Node *node,
                                                             unsigned depth) {
  return mangleGenericPartialSpecialization(node, depth + 1);
}

ManglingError
Remangler::mangleGenericSpecializationNode(Node *node, const char *operatorStr,
                                           unsigned depth) {
  bool FirstParam = true;
  for (NodePointer Child : *node) {
    if (Child->getKind() == Node::Kind::GenericSpecializationParam) {
      RETURN_IF_ERROR(mangleChildNode(Child, 0, depth + 1));
      mangleListSeparator(FirstParam);
    }
  }
  DEMANGLER_ASSERT(
      !FirstParam && "generic specialization with no substitutions", node);

  Buffer << operatorStr;

  for (NodePointer Child : *node) {
    if (Child->getKind() != Node::Kind::GenericSpecializationParam)
      RETURN_IF_ERROR(mangle(Child, depth + 1));
  }

  return ManglingError::Success;
}

ManglingError Remangler::mangleGenericSpecialization(Node *node,
                                                     unsigned depth) {
  return mangleGenericSpecializationNode(node, "Tg", depth + 1);
}

ManglingError
Remangler::mangleGenericSpecializationPrespecialized(Node *node,
                                                     unsigned depth) {
  return mangleGenericSpecializationNode(node, "Ts", depth + 1);
}

ManglingError
Remangler::mangleGenericSpecializationNotReAbstracted(Node *node,
                                                      unsigned depth) {
  return mangleGenericSpecializationNode(node, "TG", depth + 1);
}

ManglingError
Remangler::mangleGenericSpecializationInResilienceDomain(Node *node,
                                                         unsigned depth) {
  return mangleGenericSpecializationNode(node, "TB", depth + 1);
}

ManglingError Remangler::mangleInlinedGenericFunction(Node *node,
                                                      unsigned depth) {
  return mangleGenericSpecializationNode(node, "Ti", depth + 1);
}

ManglingError Remangler::mangleGenericSpecializationParam(Node *node,
                                                          unsigned depth) {
  // handled inline
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleGenericTypeMetadataPattern(Node *node,
                                                          unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "MP";
  return ManglingError::Success;
}

ManglingError Remangler::mangleGenericTypeParamDecl(Node *node,
                                                    unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "fp";
  return ManglingError::Success;
}

ManglingError Remangler::mangleGetter(Node *node, unsigned depth) {
  return mangleAbstractStorage(node->getFirstChild(), "g", depth + 1);
}

ManglingError Remangler::mangleGlobal(Node *node, unsigned depth) {
  Buffer << MANGLING_PREFIX_STR;
  bool mangleInReverseOrder = false;
  for (auto Iter = node->begin(), End = node->end(); Iter != End; ++Iter) {
    Node *Child = *Iter;
    switch (Child->getKind()) {
      case Node::Kind::FunctionSignatureSpecialization:
      case Node::Kind::GenericSpecialization:
      case Node::Kind::GenericSpecializationPrespecialized:
      case Node::Kind::GenericSpecializationNotReAbstracted:
      case Node::Kind::GenericSpecializationInResilienceDomain:
      case Node::Kind::InlinedGenericFunction:
      case Node::Kind::GenericPartialSpecialization:
      case Node::Kind::GenericPartialSpecializationNotReAbstracted:
      case Node::Kind::OutlinedBridgedMethod:
      case Node::Kind::OutlinedVariable:
      case Node::Kind::OutlinedReadOnlyObject:
      case Node::Kind::ObjCAttribute:
      case Node::Kind::NonObjCAttribute:
      case Node::Kind::DynamicAttribute:
      case Node::Kind::VTableAttribute:
      case Node::Kind::DirectMethodReferenceAttribute:
      case Node::Kind::MergedFunction:
      case Node::Kind::DistributedThunk:
      case Node::Kind::DistributedAccessor:
      case Node::Kind::DynamicallyReplaceableFunctionKey:
      case Node::Kind::DynamicallyReplaceableFunctionImpl:
      case Node::Kind::DynamicallyReplaceableFunctionVar:
      case Node::Kind::AsyncFunctionPointer:
      case Node::Kind::AsyncAwaitResumePartialFunction:
      case Node::Kind::AsyncSuspendResumePartialFunction:
      case Node::Kind::AccessibleFunctionRecord:
      case Node::Kind::BackDeploymentThunk:
      case Node::Kind::BackDeploymentFallback:
      case Node::Kind::HasSymbolQuery:
      case Node::Kind::RuntimeDiscoverableAttributeRecord:
        mangleInReverseOrder = true;
        break;
      default:
        RETURN_IF_ERROR(mangle(Child, depth + 1));
        if (mangleInReverseOrder) {
          auto ReverseIter = Iter;
          while (ReverseIter != node->begin()) {
            --ReverseIter;
            RETURN_IF_ERROR(mangle(*ReverseIter, depth + 1));
          }
          mangleInReverseOrder = false;
        }
        break;
    }
  }
  return ManglingError::Success;
}

ManglingError Remangler::mangleGlobalGetter(Node *node, unsigned depth) {
  return mangleAbstractStorage(node->getFirstChild(), "G", depth + 1);
}

ManglingError Remangler::mangleIdentifier(Node *node, unsigned depth) {
  mangleIdentifierImpl(node, /*isOperator*/ false);
  return ManglingError::Success;
}

ManglingError Remangler::mangleIndex(Node *node, unsigned depth) {
  // handled inline
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleUnknownIndex(Node *node, unsigned depth) {
  // handled inline
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleIVarInitializer(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "fe";
  return ManglingError::Success;
}

ManglingError Remangler::mangleIVarDestroyer(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "fE";
  return ManglingError::Success;
}

ManglingError Remangler::mangleImplDifferentiabilityKind(Node *node,
                                                         unsigned depth) {
  Buffer << (char)node->getIndex();
  return ManglingError::Success;
}

ManglingError Remangler::mangleImplEscaping(Node *node, unsigned depth) {
  Buffer << 'e';
  return ManglingError::Success;
}

ManglingError Remangler::mangleImplConvention(Node *node, unsigned depth) {
  char ConvCh = llvm::StringSwitch<char>(node->getText())
                  .Case("@callee_unowned", 'y')
                  .Case("@callee_guaranteed", 'g')
                  .Case("@callee_owned", 'x')
                  .Default(0);
  if (!ConvCh)
    return MANGLING_ERROR(ManglingError::InvalidImplCalleeConvention, node);
  Buffer << ConvCh;
  return ManglingError::Success;
}

ManglingError
Remangler::mangleImplParameterResultDifferentiability(Node *node,
                                                      unsigned depth) {
  DEMANGLER_ASSERT(node->hasText(), node);
  // Empty string represents default differentiability.
  if (node->getText().empty())
    return ManglingError::Success;
  char diffChar = llvm::StringSwitch<char>(node->getText())
                      .Case("@noDerivative", 'w')
                      .Default(0);
  if (!diffChar)
    return MANGLING_ERROR(ManglingError::InvalidImplDifferentiability, node);
  Buffer << diffChar;

  return ManglingError::Success;
}

ManglingError Remangler::mangleImplFunctionAttribute(Node *node,
                                                     unsigned depth) {
  // handled inline
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleImplFunctionConvention(Node *node,
                                                      unsigned depth) {
  StringRef text =
      (node->getNumChildren() > 0 && node->getFirstChild()->hasText())
          ? node->getFirstChild()->getText()
          : "";
  char FuncAttr = llvm::StringSwitch<char>(text)
                      .Case("block", 'B')
                      .Case("c", 'C')
                      .Case("method", 'M')
                      .Case("objc_method", 'O')
                      .Case("closure", 'K')
                      .Case("witness_method", 'W')
                      .Default(0);
  DEMANGLER_ASSERT(FuncAttr && "invalid impl function convention", node);
  if ((FuncAttr == 'B' || FuncAttr == 'C') && node->getNumChildren() > 1 &&
      node->getChild(1)->getKind() == Node::Kind::ClangType) {
    Buffer << 'z' << FuncAttr;
    return mangleClangType(node->getChild(1), depth + 1);
  }
  Buffer << FuncAttr;
  return ManglingError::Success;
}

ManglingError Remangler::mangleImplFunctionConventionName(Node *node,
                                                          unsigned depth) {
  // handled inline
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleClangType(Node *node, unsigned depth) {
  Buffer << node->getText().size() << node->getText();
  return ManglingError::Success;
}

ManglingError Remangler::mangleImplInvocationSubstitutions(Node *node,
                                                           unsigned depth) {
  // handled inline
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleImplPatternSubstitutions(Node *node,
                                                        unsigned depth) {
  // handled inline
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleImplFunctionType(Node *node, unsigned depth) {
  const char *PseudoGeneric = "";
  Node *GenSig = nullptr;
  Node *PatternSubs = nullptr;
  Node *InvocationSubs = nullptr;
  for (NodePointer Child : *node) {
    switch (auto kind = Child->getKind()) {
    case Node::Kind::ImplParameter:
    case Node::Kind::ImplResult:
    case Node::Kind::ImplYield:
    case Node::Kind::ImplErrorResult:
      // Mangle type. Type should be the last child.
      DEMANGLER_ASSERT(
          Child->getNumChildren() == 2 || Child->getNumChildren() == 3, node);
      RETURN_IF_ERROR(mangle(Child->getLastChild(), depth + 1));
      break;
    case Node::Kind::DependentPseudogenericSignature:
      PseudoGeneric = "P";
      LLVM_FALLTHROUGH;
    case Node::Kind::DependentGenericSignature:
      GenSig = Child;
      break;
    case Node::Kind::ImplPatternSubstitutions:
      PatternSubs = Child;
      break;
    case Node::Kind::ImplInvocationSubstitutions:
      InvocationSubs = Child;
      break;
    default:
      break;
    }
  }
  if (GenSig)
    RETURN_IF_ERROR(mangle(GenSig, depth + 1));
  if (InvocationSubs) {
    Buffer << 'y';
    RETURN_IF_ERROR(mangleChildNodes(InvocationSubs->getChild(0), depth + 1));
    if (InvocationSubs->getNumChildren() >= 2) {
      RETURN_IF_ERROR(
          mangleRetroactiveConformance(InvocationSubs->getChild(1), depth + 1));
    }
  }
  if (PatternSubs) {
    RETURN_IF_ERROR(mangle(PatternSubs->getChild(0), depth + 1));
    Buffer << 'y';
    RETURN_IF_ERROR(mangleChildNodes(PatternSubs->getChild(1), depth + 1));
    if (PatternSubs->getNumChildren() >= 3) {
      NodePointer retroactiveConf = PatternSubs->getChild(2);
      if (retroactiveConf->getKind() == Node::Kind::TypeList) {
        RETURN_IF_ERROR(mangleChildNodes(retroactiveConf, depth + 1));
      } else {
        RETURN_IF_ERROR(
            mangleRetroactiveConformance(retroactiveConf, depth + 1));
      }
    }
  }

  Buffer << 'I';

  if (PatternSubs)
    Buffer << 's';
  if (InvocationSubs)
    Buffer << 'I';

  Buffer << PseudoGeneric;
  for (NodePointer Child : *node) {
    switch (Child->getKind()) {
      case Node::Kind::ImplDifferentiabilityKind:
        Buffer << (char)Child->getIndex();
        break;
      case Node::Kind::ImplEscaping:
        Buffer << 'e';
        break;
      case Node::Kind::ImplConvention: {
        char ConvCh = llvm::StringSwitch<char>(Child->getText())
                        .Case("@callee_unowned", 'y')
                        .Case("@callee_guaranteed", 'g')
                        .Case("@callee_owned", 'x')
                        .Case("@convention(thin)", 't')
                        .Default(0);
        if (!ConvCh)
          return MANGLING_ERROR(ManglingError::InvalidImplCalleeConvention,
                               Child);
        Buffer << ConvCh;
        break;
      }
      case Node::Kind::ImplFunctionConvention: {
        RETURN_IF_ERROR(mangleImplFunctionConvention(Child, depth + 1));
        break;
      }
      case Node::Kind::ImplFunctionAttribute: {
        char FuncAttr = llvm::StringSwitch<char>(Child->getText())
                        .Case("@yield_once", 'A')
                        .Case("@yield_many", 'G')
                        .Case("@Sendable", 'h')
                        .Case("@async", 'H')
                        .Default(0);
        if (!FuncAttr) {
          return MANGLING_ERROR(ManglingError::InvalidImplFunctionAttribute,
                               Child);
        }
        Buffer << FuncAttr;
        break;
      }
      case Node::Kind::ImplYield:
        Buffer << 'Y';
        LLVM_FALLTHROUGH;
      case Node::Kind::ImplParameter: {
        // Mangle parameter convention.
        char ConvCh =
            llvm::StringSwitch<char>(Child->getFirstChild()->getText())
                .Case("@in", 'i')
                .Case("@inout", 'l')
                .Case("@inout_aliasable", 'b')
                .Case("@in_guaranteed", 'n')
                .Case("@in_constant", 'c')
                .Case("@owned", 'x')
                .Case("@guaranteed", 'g')
                .Case("@deallocating", 'e')
                .Case("@unowned", 'y')
                .Case("@pack_guaranteed", 'p')
                .Case("@pack_owned", 'v')
                .Case("@pack_inout", 'm')
                .Default(0);
        if (!ConvCh) {
          return MANGLING_ERROR(ManglingError::InvalidImplParameterConvention,
                               Child->getFirstChild());
        }
        Buffer << ConvCh;
        // Mangle parameter differentiability, if it exists.
        if (Child->getNumChildren() == 3) {
          RETURN_IF_ERROR(mangleImplParameterResultDifferentiability(
              Child->getChild(1), depth + 1));
        }
        break;
      }
      case Node::Kind::ImplErrorResult:
        Buffer << 'z';
        LLVM_FALLTHROUGH;
      case Node::Kind::ImplResult: {
        char ConvCh = llvm::StringSwitch<char>(Child->getFirstChild()->getText())
                        .Case("@out", 'r')
                        .Case("@owned", 'o')
                        .Case("@unowned", 'd')
                        .Case("@unowned_inner_pointer", 'u')
                        .Case("@autoreleased", 'a')
                        .Case("@pack_out", 'k')
                        .Default(0);
        if (!ConvCh) {
          return MANGLING_ERROR(ManglingError::InvalidImplParameterConvention,
                               Child->getFirstChild());
        }
        Buffer << ConvCh;
        // Mangle result differentiability, if it exists.
        if (Child->getNumChildren() == 3) {
          RETURN_IF_ERROR(mangleImplParameterResultDifferentiability(
              Child->getChild(1), depth + 1));
        }
        break;
      }
      default:
        break;
    }
  }
  Buffer << '_';

  return ManglingError::Success;
}

ManglingError Remangler::mangleImplicitClosure(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNode(node, 0, depth + 1)); // context
  RETURN_IF_ERROR(mangleChildNode(node, 2, depth + 1)); // type
  Buffer << "fu";
  return mangleChildNode(node, 1, depth + 1); // index
}

ManglingError Remangler::mangleImplParameter(Node *node, unsigned depth) {
  // handled inline
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleImplResult(Node *node, unsigned depth) {
  // handled inline
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleImplYield(Node *node, unsigned depth) {
  // handled inline
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleImplErrorResult(Node *node, unsigned depth) {
  // handled inline
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleInOut(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << 'z';
  return ManglingError::Success;
}

ManglingError Remangler::mangleIsolated(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "Yi";
  return ManglingError::Success;
}

ManglingError Remangler::mangleCompileTimeConst(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "Yt";
  return ManglingError::Success;
}

ManglingError Remangler::mangleShared(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << 'h';
  return ManglingError::Success;
}

ManglingError Remangler::mangleOwned(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << 'n';
  return ManglingError::Success;
}

ManglingError Remangler::mangleNoDerivative(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "Yk";
  return ManglingError::Success;
}

ManglingError Remangler::mangleInfixOperator(Node *node, unsigned depth) {
  mangleIdentifierImpl(node, /*isOperator*/ true);
  Buffer << "oi";
  return ManglingError::Success;
}

ManglingError Remangler::mangleInitializer(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "fi";
  return ManglingError::Success;
}

ManglingError
Remangler::manglePropertyWrapperBackingInitializer(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "fP";
  return ManglingError::Success;
}

ManglingError
Remangler::manglePropertyWrapperInitFromProjectedValue(Node *node,
                                                       unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "fW";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleLazyProtocolWitnessTableAccessor(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "Wl";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleLazyProtocolWitnessTableCacheVariable(Node *node,
                                                       unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "WL";
  return ManglingError::Success;
}

ManglingError Remangler::mangleLocalDeclName(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNode(node, 1, depth + 1)); // identifier
  Buffer << 'L';
  return mangleChildNode(node, 0, depth + 1); // index
}

ManglingError Remangler::mangleMaterializeForSet(Node *node, unsigned depth) {
  return mangleAbstractStorage(node->getFirstChild(), "m", depth + 1);
}

ManglingError Remangler::mangleMetatype(Node *node, unsigned depth) {
  if (node->getFirstChild()->getKind() == Node::Kind::MetatypeRepresentation) {
    RETURN_IF_ERROR(mangleChildNode(node, 1, depth + 1));
    Buffer << "XM";
    RETURN_IF_ERROR(mangleChildNode(node, 0, depth + 1));
  } else {
    RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
    Buffer << 'm';
  }
  return ManglingError::Success;
}

ManglingError Remangler::mangleMetatypeRepresentation(Node *node,
                                                      unsigned depth) {
  auto text = node->getText();
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

ManglingError Remangler::mangleMetaclass(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "Mm";
  return ManglingError::Success;
}

ManglingError Remangler::mangleModifyAccessor(Node *node, unsigned depth) {
  return mangleAbstractStorage(node->getFirstChild(), "M", depth + 1);
}

ManglingError Remangler::mangleModule(Node *node, unsigned depth) {
  auto text = node->getText();
  if (text == STDLIB_NAME) {
    Buffer << 's';
  } else if (text == MANGLING_MODULE_OBJC) {
    Buffer << "So";
  } else if (text == MANGLING_MODULE_CLANG_IMPORTER) {
    Buffer << "SC";
  } else {
    return mangleIdentifier(node, depth);
  }
  return ManglingError::Success;
}

ManglingError Remangler::mangleNativeOwningAddressor(Node *node,
                                                     unsigned depth) {
  return mangleAbstractStorage(node->getFirstChild(), "lo", depth + 1);
}

ManglingError Remangler::mangleNativeOwningMutableAddressor(Node *node,
                                                            unsigned depth) {
  return mangleAbstractStorage(node->getFirstChild(), "ao", depth + 1);
}

ManglingError Remangler::mangleNativePinningAddressor(Node *node,
                                                      unsigned depth) {
  return mangleAbstractStorage(node->getFirstChild(), "lp", depth + 1);
}

ManglingError Remangler::mangleNativePinningMutableAddressor(Node *node,
                                                             unsigned depth) {
  return mangleAbstractStorage(node->getFirstChild(), "aP", depth + 1);
}

ManglingError Remangler::mangleClassMetadataBaseOffset(Node *node,
                                                       unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "Mo";
  return ManglingError::Success;
}

ManglingError Remangler::mangleNominalTypeDescriptor(Node *node,
                                                     unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "Mn";
  return ManglingError::Success;
}

ManglingError Remangler::mangleNominalTypeDescriptorRecord(Node *node,
                                                           unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "Hn";
  return ManglingError::Success;
}

ManglingError Remangler::mangleOpaqueTypeDescriptor(Node *node,
                                                    unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "MQ";
  return ManglingError::Success;
}

ManglingError Remangler::mangleOpaqueTypeDescriptorRecord(Node *node,
                                                          unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "Ho";
  return ManglingError::Success;
}

ManglingError Remangler::mangleOpaqueTypeDescriptorAccessor(Node *node,
                                                            unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "Mg";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleOpaqueTypeDescriptorAccessorImpl(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "Mh";
  return ManglingError::Success;
}

ManglingError Remangler::mangleOpaqueTypeDescriptorAccessorKey(Node *node,
                                                               unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "Mj";
  return ManglingError::Success;
}

ManglingError Remangler::mangleOpaqueTypeDescriptorAccessorVar(Node *node,
                                                               unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "Mk";
  return ManglingError::Success;
}

ManglingError Remangler::manglePropertyDescriptor(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "MV";
  return ManglingError::Success;
}

ManglingError Remangler::mangleNonObjCAttribute(Node *node, unsigned depth) {
  Buffer << "TO";
  return ManglingError::Success;
}

ManglingError Remangler::mangleTuple(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleTypeList(node, depth + 1));
  Buffer << 't';
  return ManglingError::Success;
}

ManglingError Remangler::manglePack(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleTypeList(node, depth + 1));
  Buffer << "QP";
  return ManglingError::Success;
}

ManglingError Remangler::mangleSILPackDirect(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleTypeList(node, depth + 1));
  Buffer << "QSd";
  return ManglingError::Success;
}

ManglingError Remangler::mangleSILPackIndirect(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleTypeList(node, depth + 1));
  Buffer << "QSi";
  return ManglingError::Success;
}

ManglingError Remangler::manglePackExpansion(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "Qp";
  return ManglingError::Success;
}

ManglingError Remangler::mangleNumber(Node *node, unsigned depth) {
  mangleIndex(node->getIndex());
  return ManglingError::Success;
}

ManglingError Remangler::mangleObjCAttribute(Node *node, unsigned depth) {
  Buffer << "To";
  return ManglingError::Success;
}

ManglingError Remangler::mangleObjCBlock(Node *node, unsigned depth) {
  if (node->getNumChildren() > 0 &&
      node->getFirstChild()->getKind() == Node::Kind::ClangType) {
    for (size_t Idx = node->getNumChildren() - 1; Idx >= 1; --Idx) {
      RETURN_IF_ERROR(mangleChildNode(node, Idx, depth + 1));
    }
    Buffer << "XzB";
    return mangleClangType(node->getFirstChild(), depth + 1);
  }
  RETURN_IF_ERROR(mangleChildNodesReversed(node, depth + 1));
  Buffer << "XB";
  return ManglingError::Success;
}

ManglingError Remangler::mangleEscapingObjCBlock(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodesReversed(node, depth + 1));
  Buffer << "XL";
  return ManglingError::Success;
}

ManglingError Remangler::mangleOwningAddressor(Node *node, unsigned depth) {
  return mangleAbstractStorage(node->getFirstChild(), "lO", depth + 1);
}

ManglingError Remangler::mangleOwningMutableAddressor(Node *node,
                                                      unsigned depth) {
  return mangleAbstractStorage(node->getFirstChild(), "aO", depth + 1);
}

ManglingError Remangler::manglePartialApplyForwarder(Node *node,
                                                     unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodesReversed(node, depth + 1));
  Buffer << "TA";
  return ManglingError::Success;
}

ManglingError Remangler::manglePartialApplyObjCForwarder(Node *node,
                                                         unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodesReversed(node, depth + 1));
  Buffer << "Ta";
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
  Buffer << "TQ";
  return mangleChildNode(node, 0, depth + 1);
}

ManglingError
Remangler::mangleAsyncSuspendResumePartialFunction(Node *node, unsigned depth) {
  Buffer << "TY";
  return mangleChildNode(node, 0, depth + 1);
}

ManglingError Remangler::manglePostfixOperator(Node *node, unsigned depth) {
  mangleIdentifierImpl(node, /*isOperator*/ true);
  Buffer << "oP";
  return ManglingError::Success;
}

ManglingError Remangler::manglePrefixOperator(Node *node, unsigned depth) {
  mangleIdentifierImpl(node, /*isOperator*/ true);
  Buffer << "op";
  return ManglingError::Success;
}

ManglingError Remangler::manglePrivateDeclName(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodesReversed(node, depth + 1));
  Buffer << (node->getNumChildren() == 1 ? "Ll" : "LL");
  return ManglingError::Success;
}

ManglingError Remangler::mangleProtocol(Node *node, unsigned depth) {
  return mangleAnyGenericType(node, "P", depth + 1);
}

ManglingError Remangler::mangleRetroactiveConformance(Node *node,
                                                      unsigned depth) {
  RETURN_IF_ERROR(mangleAnyProtocolConformance(node->getChild(1), depth + 1));
  Buffer << 'g';
  mangleIndex(node->getChild(0)->getIndex());
  return ManglingError::Success;
}

ManglingError Remangler::mangleProtocolConformance(Node *node, unsigned depth) {
  Node *Ty = getChildOfType(node->getChild(0));
  Node *GenSig = nullptr;
  if (Ty->getKind() == Node::Kind::DependentGenericType) {
    GenSig = Ty->getFirstChild();
    Ty = Ty->getChild(1);
  }
  RETURN_IF_ERROR(mangle(Ty, depth + 1));
  if (node->getNumChildren() == 4)
    RETURN_IF_ERROR(mangleChildNode(node, 3, depth + 1));
  RETURN_IF_ERROR(manglePureProtocol(node->getChild(1), depth + 1));
  RETURN_IF_ERROR(mangleChildNode(node, 2, depth + 1));
  if (GenSig)
    RETURN_IF_ERROR(mangle(GenSig, depth + 1));
  return ManglingError::Success;
}

ManglingError
Remangler::mangleProtocolConformanceRefInTypeModule(Node *node,
                                                    unsigned depth) {
  RETURN_IF_ERROR(manglePureProtocol(node->getChild(0), depth + 1));
  Buffer << "HP";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleProtocolConformanceRefInProtocolModule(Node *node,
                                                        unsigned depth) {
  RETURN_IF_ERROR(manglePureProtocol(node->getChild(0), depth + 1));
  Buffer << "Hp";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleProtocolConformanceRefInOtherModule(Node *node,
                                                     unsigned depth) {
  RETURN_IF_ERROR(manglePureProtocol(node->getChild(0), depth + 1));
  return mangleChildNode(node, 1, depth + 1);
}

ManglingError Remangler::mangleConcreteProtocolConformance(Node *node,
                                                           unsigned depth) {
  RETURN_IF_ERROR(mangleType(node->getChild(0), depth + 1));
  RETURN_IF_ERROR(mangle(node->getChild(1), depth + 1));
  if (node->getNumChildren() > 2) {
    RETURN_IF_ERROR(
        mangleAnyProtocolConformanceList(node->getChild(2), depth + 1));
  } else {
    Buffer << "y";
  }
  Buffer << "HC";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleDependentProtocolConformanceRoot(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleType(node->getChild(0), depth + 1));
  RETURN_IF_ERROR(manglePureProtocol(node->getChild(1), depth + 1));
  Buffer << "HD";
  return mangleDependentConformanceIndex(node->getChild(2), depth + 1);
}

ManglingError
Remangler::mangleDependentProtocolConformanceInherited(Node *node,
                                                       unsigned depth) {
  RETURN_IF_ERROR(mangleAnyProtocolConformance(node->getChild(0), depth + 1));
  RETURN_IF_ERROR(manglePureProtocol(node->getChild(1), depth + 1));
  Buffer << "HI";
  return mangleDependentConformanceIndex(node->getChild(2), depth + 1);
}

ManglingError Remangler::mangleDependentAssociatedConformance(Node *node,
                                                              unsigned depth) {
  RETURN_IF_ERROR(mangleType(node->getChild(0), depth + 1));
  return manglePureProtocol(node->getChild(1), depth + 1);
}

ManglingError
Remangler::mangleDependentProtocolConformanceAssociated(Node *node,
                                                        unsigned depth) {
  RETURN_IF_ERROR(mangleAnyProtocolConformance(node->getChild(0), depth + 1));
  RETURN_IF_ERROR(
      mangleDependentAssociatedConformance(node->getChild(1), depth + 1));
  Buffer << "HA";
  return mangleDependentConformanceIndex(node->getChild(2), depth + 1);
}

ManglingError Remangler::mangleDependentConformanceIndex(Node *node,
                                                         unsigned depth) {
  DEMANGLER_ASSERT(node->getKind() == Node::Kind::Index ||
                       node->getKind() == Node::Kind::UnknownIndex,
                   node);
  DEMANGLER_ASSERT(node->hasIndex() == (node->getKind() == Node::Kind::Index),
                   node);
  mangleIndex(node->hasIndex() ? node->getIndex() + 2 : 1);
  return ManglingError::Success;
}

ManglingError Remangler::mangleAnyProtocolConformance(Node *node,
                                                      unsigned depth) {
  switch (node->getKind()) {
  case Node::Kind::ConcreteProtocolConformance:
    return mangleConcreteProtocolConformance(node, depth + 1);
  case Node::Kind::DependentProtocolConformanceRoot:
    return mangleDependentProtocolConformanceRoot(node, depth + 1);
  case Node::Kind::DependentProtocolConformanceInherited:
    return mangleDependentProtocolConformanceInherited(node, depth + 1);
  case Node::Kind::DependentProtocolConformanceAssociated:
    return mangleDependentProtocolConformanceAssociated(node, depth + 1);
  default:
    // Should this really succeed?!
    return ManglingError::Success;
  }
}

ManglingError Remangler::mangleAnyProtocolConformanceList(Node *node,
                                                          unsigned depth) {
  bool firstElem = true;
  for (NodePointer child : *node) {
    RETURN_IF_ERROR(mangleAnyProtocolConformance(child, depth + 1));
    mangleListSeparator(firstElem);
  }
  mangleEndOfList(firstElem);
  return ManglingError::Success;
}

ManglingError Remangler::mangleProtocolDescriptor(Node *node, unsigned depth) {
  RETURN_IF_ERROR(manglePureProtocol(getSingleChild(node), depth + 1));
  Buffer << "Mp";
  return ManglingError::Success;
}

ManglingError Remangler::mangleProtocolDescriptorRecord(Node *node,
                                                        unsigned depth) {
  RETURN_IF_ERROR(manglePureProtocol(getSingleChild(node), depth + 1));
  Buffer << "Hr";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleProtocolRequirementsBaseDescriptor(Node *node,
                                                    unsigned depth) {
  RETURN_IF_ERROR(manglePureProtocol(getSingleChild(node), depth + 1));
  Buffer << "TL";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleProtocolSelfConformanceDescriptor(Node *node, unsigned depth) {
  RETURN_IF_ERROR(manglePureProtocol(node->getChild(0), depth + 1));
  Buffer << "MS";
  return ManglingError::Success;
}

ManglingError Remangler::mangleProtocolConformanceDescriptor(Node *node,
                                                             unsigned depth) {
  RETURN_IF_ERROR(mangleProtocolConformance(node->getChild(0), depth + 1));
  Buffer << "Mc";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleProtocolConformanceDescriptorRecord(Node *node,
                                                     unsigned depth) {
  RETURN_IF_ERROR(mangleProtocolConformance(node->getChild(0), depth + 1));
  Buffer << "Hc";
  return ManglingError::Success;
}

ManglingError Remangler::mangleProtocolList(Node *node, Node *superclass,
                                            bool hasExplicitAnyObject,
                                            unsigned depth) {
  auto *protocols = getSingleChild(node, Node::Kind::TypeList);
  bool FirstElem = true;
  for (NodePointer Child : *protocols) {
    RETURN_IF_ERROR(manglePureProtocol(Child, depth + 1));
    mangleListSeparator(FirstElem);
  }
  mangleEndOfList(FirstElem);
  if (superclass) {
    RETURN_IF_ERROR(mangleType(superclass, depth + 1));
    Buffer << "Xc";
    return ManglingError::Success;
  } else if (hasExplicitAnyObject) {
    Buffer << "Xl";
    return ManglingError::Success;
  }
  Buffer << 'p';
  return ManglingError::Success;
}

ManglingError Remangler::mangleProtocolList(Node *node, unsigned depth) {
  return mangleProtocolList(node, nullptr, false, depth + 1);
}

ManglingError Remangler::mangleProtocolListWithClass(Node *node,
                                                     unsigned depth) {
  return mangleProtocolList(node->getChild(0), node->getChild(1), false,
                            depth + 1);
}

ManglingError Remangler::mangleProtocolListWithAnyObject(Node *node,
                                                         unsigned depth) {
  return mangleProtocolList(node->getChild(0), nullptr, true, depth + 1);
}

ManglingError Remangler::mangleProtocolSelfConformanceWitness(Node *node,
                                                              unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "TS";
  return ManglingError::Success;
}

ManglingError Remangler::mangleProtocolWitness(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "TW";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleProtocolSelfConformanceWitnessTable(Node *node,
                                                     unsigned depth) {
  RETURN_IF_ERROR(manglePureProtocol(node->getChild(0), depth + 1));
  Buffer << "WS";
  return ManglingError::Success;
}

ManglingError Remangler::mangleProtocolWitnessTable(Node *node,
                                                    unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "WP";
  return ManglingError::Success;
}

ManglingError Remangler::mangleProtocolWitnessTablePattern(Node *node,
                                                           unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "Wp";
  return ManglingError::Success;
}

ManglingError Remangler::mangleProtocolWitnessTableAccessor(Node *node,
                                                            unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "Wa";
  return ManglingError::Success;
}

ManglingError Remangler::mangleReabstractionThunk(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodesReversed(node, depth + 1));
  Buffer << "Tr";
  return ManglingError::Success;
}

ManglingError Remangler::mangleReabstractionThunkHelper(Node *node,
                                                        unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodesReversed(node, depth + 1));
  Buffer << "TR";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleReabstractionThunkHelperWithSelf(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodesReversed(node, depth + 1));
  Buffer << "Ty";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleReabstractionThunkHelperWithGlobalActor(Node *node,
                                                         unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "TU";
  return ManglingError::Success;
}

ManglingError Remangler::mangleAutoDiffFunctionOrSimpleThunk(Node *node,
                                                             StringRef op,
                                                             unsigned depth) {
  auto childIt = node->begin();
  while (childIt != node->end() &&
         (*childIt)->getKind() != Node::Kind::AutoDiffFunctionKind)
    RETURN_IF_ERROR(mangle(*childIt++, depth + 1));
  Buffer << op;
  RETURN_IF_ERROR(mangle(*childIt++, depth + 1)); // kind
  RETURN_IF_ERROR(mangle(*childIt++, depth + 1)); // parameter indices
  Buffer << 'p';
  RETURN_IF_ERROR(mangle(*childIt++, depth + 1)); // result indices
  Buffer << 'r';
  return ManglingError::Success;
}

ManglingError Remangler::mangleAutoDiffFunction(Node *node, unsigned depth) {
  return mangleAutoDiffFunctionOrSimpleThunk(node, "TJ", depth + 1);
}

ManglingError Remangler::mangleAutoDiffDerivativeVTableThunk(Node *node,
                                                             unsigned depth) {
  return mangleAutoDiffFunctionOrSimpleThunk(node, "TJV", depth + 1);
}

ManglingError
Remangler::mangleAutoDiffSelfReorderingReabstractionThunk(Node *node,
                                                          unsigned depth) {
  auto childIt = node->begin();
  RETURN_IF_ERROR(mangle(*childIt++, depth + 1)); // from type
  RETURN_IF_ERROR(mangle(*childIt++, depth + 1)); // to type
  if ((*childIt)->getKind() == Node::Kind::DependentGenericSignature)
    RETURN_IF_ERROR(mangleDependentGenericSignature(*childIt++, depth + 1));
  Buffer << "TJO";
  return mangle(*childIt++, depth + 1); // kind
}

ManglingError Remangler::mangleAutoDiffSubsetParametersThunk(Node *node,
                                                             unsigned depth) {
  auto childIt = node->begin();
  while (childIt != node->end() &&
         (*childIt)->getKind() != Node::Kind::AutoDiffFunctionKind)
    RETURN_IF_ERROR(mangle(*childIt++, depth + 1));
  Buffer << "TJS";
  RETURN_IF_ERROR(mangle(*childIt++, depth + 1)); // kind
  RETURN_IF_ERROR(mangle(*childIt++, depth + 1)); // parameter indices
  Buffer << 'p';
  RETURN_IF_ERROR(mangle(*childIt++, depth + 1)); // result indices
  Buffer << 'r';
  RETURN_IF_ERROR(mangle(*childIt++, depth + 1)); // to parameter indices
  Buffer << 'P';
  return ManglingError::Success;
}

ManglingError Remangler::mangleAutoDiffFunctionKind(Node *node,
                                                    unsigned depth) {
  Buffer << (char)node->getIndex();
  return ManglingError::Success;
}

ManglingError Remangler::mangleDifferentiabilityWitness(Node *node,
                                                        unsigned depth) {
  auto childIt = node->begin();
  while (childIt != node->end() && (*childIt)->getKind() != Node::Kind::Index)
    RETURN_IF_ERROR(mangle(*childIt++, depth + 1));
  if (node->getLastChild()->getKind() ==
          Node::Kind::DependentGenericSignature)
    RETURN_IF_ERROR(mangle(node->getLastChild(), depth + 1));
  Buffer << "WJ" << (char)(*childIt++)->getIndex();
  RETURN_IF_ERROR(mangle(*childIt++, depth + 1)); // parameter indices
  Buffer << 'p';
  RETURN_IF_ERROR(mangle(*childIt++, depth + 1)); // result indices
  Buffer << 'r';
  return ManglingError::Success;
}

ManglingError Remangler::mangleIndexSubset(Node *node, unsigned depth) {
  Buffer << node->getText();
  return ManglingError::Success;
}

ManglingError Remangler::mangleReadAccessor(Node *node, unsigned depth) {
  return mangleAbstractStorage(node->getFirstChild(), "r", depth + 1);
}

ManglingError Remangler::mangleKeyPathThunkHelper(Node *node, StringRef op,
                                                  unsigned depth) {
  for (NodePointer Child : *node)
    if (Child->getKind() != Node::Kind::IsSerialized)
      RETURN_IF_ERROR(mangle(Child, depth + 1));
  Buffer << op;
  for (NodePointer Child : *node)
    if (Child->getKind() == Node::Kind::IsSerialized)
      RETURN_IF_ERROR(mangle(Child, depth + 1));
  return ManglingError::Success;
}

ManglingError Remangler::mangleKeyPathGetterThunkHelper(Node *node,
                                                        unsigned depth) {
  return mangleKeyPathThunkHelper(node, "TK", depth + 1);
}

ManglingError Remangler::mangleKeyPathSetterThunkHelper(Node *node,
                                                        unsigned depth) {
  return mangleKeyPathThunkHelper(node, "Tk", depth + 1);
}

ManglingError Remangler::mangleKeyPathEqualsThunkHelper(Node *node,
                                                        unsigned depth) {
  return mangleKeyPathThunkHelper(node, "TH", depth + 1);
}

ManglingError Remangler::mangleKeyPathHashThunkHelper(Node *node,
                                                      unsigned depth) {
  return mangleKeyPathThunkHelper(node, "Th", depth + 1);
}

ManglingError Remangler::mangleReturnType(Node *node, unsigned depth) {
  return mangleArgumentTuple(node, depth + 1);
}

ManglingError Remangler::mangleRelatedEntityDeclName(Node *node,
                                                     unsigned depth) {
  RETURN_IF_ERROR(mangleChildNode(node, 1, depth + 1));
  NodePointer kindNode = node->getFirstChild();
  if (kindNode->getText().size() != 1)
    return MANGLING_ERROR(ManglingError::MultiByteRelatedEntity, kindNode);
  Buffer << "L" << kindNode->getText();
  return ManglingError::Success;
}

ManglingError Remangler::mangleSILBoxType(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "Xb";
  return ManglingError::Success;
}

ManglingError Remangler::mangleSetter(Node *node, unsigned depth) {
  return mangleAbstractStorage(node->getFirstChild(), "s", depth + 1);
}

ManglingError Remangler::mangleSpecializationPassID(Node *node,
                                                    unsigned depth) {
  Buffer << node->getIndex();
  return ManglingError::Success;
}

ManglingError Remangler::mangleIsSerialized(Node *node, unsigned depth) {
  Buffer << 'q';
  return ManglingError::Success;
}

ManglingError Remangler::mangleMetatypeParamsRemoved(Node *node, unsigned depth) {
  Buffer << 'm';
  return ManglingError::Success;
}

ManglingError Remangler::mangleStatic(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << 'Z';
  return ManglingError::Success;
}

ManglingError Remangler::mangleOtherNominalType(Node *node, unsigned depth) {
  return mangleAnyNominalType(node, depth + 1);
}

ManglingError Remangler::mangleStructure(Node *node, unsigned depth) {
  return mangleAnyNominalType(node, depth + 1);
}

ManglingError Remangler::mangleSubscript(Node *node, unsigned depth) {
  return mangleAbstractStorage(node, "p", depth + 1);
}

ManglingError Remangler::mangleMacro(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "fm";
  return ManglingError::Success;
}

ManglingError Remangler::mangleFreestandingMacroExpansion(
    Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNode(node, 0, depth + 1));
  if (auto privateDiscriminator = node->getChild(3))
    RETURN_IF_ERROR(mangle(privateDiscriminator, depth + 1));
  RETURN_IF_ERROR(mangleChildNode(node, 1, depth + 1));
  Buffer << "fMf";
  return mangleChildNode(node, 2, depth + 1);
}

ManglingError Remangler::mangleAccessorAttachedMacroExpansion(
    Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNode(node, 0, depth + 1));
  RETURN_IF_ERROR(mangleChildNode(node, 1, depth + 1));
  RETURN_IF_ERROR(mangleChildNode(node, 2, depth + 1));
  Buffer << "fMa";
  return mangleChildNode(node, 3, depth + 1);
}

ManglingError Remangler::mangleMemberAttributeAttachedMacroExpansion(
    Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNode(node, 0, depth + 1));
  RETURN_IF_ERROR(mangleChildNode(node, 1, depth + 1));
  RETURN_IF_ERROR(mangleChildNode(node, 2, depth + 1));
  Buffer << "fMr";
  return mangleChildNode(node, 3, depth + 1);
}

ManglingError Remangler::mangleMemberAttachedMacroExpansion(
    Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNode(node, 0, depth + 1));
  RETURN_IF_ERROR(mangleChildNode(node, 1, depth + 1));
  RETURN_IF_ERROR(mangleChildNode(node, 2, depth + 1));
  Buffer << "fMm";
  return mangleChildNode(node, 3, depth + 1);
}

ManglingError Remangler::manglePeerAttachedMacroExpansion(
    Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNode(node, 0, depth + 1));
  RETURN_IF_ERROR(mangleChildNode(node, 1, depth + 1));
  RETURN_IF_ERROR(mangleChildNode(node, 2, depth + 1));
  Buffer << "fMp";
  return mangleChildNode(node, 3, depth + 1);
}

ManglingError Remangler::mangleConformanceAttachedMacroExpansion(
    Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNode(node, 0, depth + 1));
  RETURN_IF_ERROR(mangleChildNode(node, 1, depth + 1));
  RETURN_IF_ERROR(mangleChildNode(node, 2, depth + 1));
  Buffer << "fMc";
  return mangleChildNode(node, 3, depth + 1);
}

ManglingError Remangler::mangleMacroExpansionUniqueName(
    Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNode(node, 0, depth + 1));
  if (auto privateDiscriminator = node->getChild(3))
    RETURN_IF_ERROR(mangle(privateDiscriminator, depth + 1));
  RETURN_IF_ERROR(mangleChildNode(node, 1, depth + 1));
  Buffer << "fMu";
  return mangleChildNode(node, 2, depth + 1);
}

ManglingError Remangler::mangleSuffix(Node *node, unsigned depth) {
  // Just add the suffix back on.
  Buffer << node->getText();
  return ManglingError::Success;
}

ManglingError Remangler::mangleThinFunctionType(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleFunctionSignature(node, depth + 1));
  Buffer << "Xf";
  return ManglingError::Success;
}

ManglingError Remangler::mangleTupleElement(Node *node, unsigned depth) {
  return mangleChildNodesReversed(node, depth + 1); // tuple type, element name?
}

ManglingError Remangler::mangleTupleElementName(Node *node, unsigned depth) {
  return mangleIdentifier(node, depth + 1);
}

ManglingError Remangler::mangleType(Node *node, unsigned depth) {
  return mangleSingleChildNode(node, depth + 1);
}

ManglingError Remangler::mangleTypeAlias(Node *node, unsigned depth) {
  return mangleAnyNominalType(node, depth + 1);
}

ManglingError Remangler::mangleTypeList(Node *node, unsigned depth) {
  bool FirstElem = true;
  for (size_t Idx = 0, Num = node->getNumChildren(); Idx < Num; ++Idx) {
    RETURN_IF_ERROR(mangleChildNode(node, Idx, depth + 1));
    mangleListSeparator(FirstElem);
  }
  mangleEndOfList(FirstElem);
  return ManglingError::Success;
}

ManglingError Remangler::mangleLabelList(Node *node, unsigned depth) {
  if (node->getNumChildren() == 0)
    Buffer << 'y';
  else
    RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  return ManglingError::Success;
}

ManglingError Remangler::mangleTypeMangling(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << 'D';
  return ManglingError::Success;
}

ManglingError Remangler::mangleTypeMetadata(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "N";
  return ManglingError::Success;
}

ManglingError Remangler::mangleTypeMetadataAccessFunction(Node *node,
                                                          unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "Ma";
  return ManglingError::Success;
}

ManglingError Remangler::mangleTypeMetadataInstantiationCache(Node *node,
                                                              unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "MI";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleTypeMetadataInstantiationFunction(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "Mi";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleTypeMetadataSingletonInitializationCache(Node *node,
                                                          unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "Ml";
  return ManglingError::Success;
}

ManglingError Remangler::mangleTypeMetadataCompletionFunction(Node *node,
                                                              unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "Mr";
  return ManglingError::Success;
}

ManglingError Remangler::mangleTypeMetadataDemanglingCache(Node *node,
                                                           unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "MD";
  return ManglingError::Success;
}

ManglingError Remangler::mangleTypeMetadataLazyCache(Node *node,
                                                     unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "ML";
  return ManglingError::Success;
}

ManglingError Remangler::mangleUncurriedFunctionType(Node *node,
                                                     unsigned depth) {
  RETURN_IF_ERROR(mangleFunctionSignature(node, depth + 1));
  // Mangle as regular function type (there is no "uncurried function type"
  // in the new mangling scheme).
  Buffer << 'c';
  return ManglingError::Success;
}

ManglingError Remangler::mangleUnsafeAddressor(Node *node, unsigned depth) {
  return mangleAbstractStorage(node->getFirstChild(), "lu", depth + 1);
}

ManglingError Remangler::mangleUnsafeMutableAddressor(Node *node,
                                                      unsigned depth) {
  return mangleAbstractStorage(node->getFirstChild(), "au", depth + 1);
}

ManglingError Remangler::mangleValueWitness(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNode(node, 1, depth + 1)); // type
  const char *Code = nullptr;
  switch (ValueWitnessKind(node->getFirstChild()->getIndex())) {
#define VALUE_WITNESS(MANGLING, NAME) \
    case ValueWitnessKind::NAME: Code = #MANGLING; break;
#include "swift/Demangling/ValueWitnessMangling.def"
  }
  Buffer << 'w' << Code;
  return ManglingError::Success;
}

ManglingError Remangler::mangleValueWitnessTable(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "WV";
  return ManglingError::Success;
}

ManglingError Remangler::mangleVariable(Node *node, unsigned depth) {
  return mangleAbstractStorage(node, "p", depth + 1);
}

ManglingError Remangler::mangleVTableAttribute(Node *node, unsigned depth) {
  // Old-fashioned vtable thunk in new mangling format
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleVTableThunk(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "TV";
  return ManglingError::Success;
}

#define REF_STORAGE(Name, ...)                                                 \
  ManglingError Remangler::mangle##Name(Node *node, unsigned depth) {          \
    RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));                   \
    Buffer << manglingOf(ReferenceOwnership::Name);                            \
    return ManglingError::Success;                                             \
  }
#include "swift/AST/ReferenceStorage.def"

ManglingError Remangler::mangleWillSet(Node *node, unsigned depth) {
  return mangleAbstractStorage(node->getFirstChild(), "w", depth + 1);
}

ManglingError
Remangler::mangleReflectionMetadataBuiltinDescriptor(Node *node,
                                                     unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "MB";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleReflectionMetadataFieldDescriptor(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "MF";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleReflectionMetadataAssocTypeDescriptor(Node *node,
                                                       unsigned depth) {
  RETURN_IF_ERROR(
      mangleSingleChildNode(node, depth + 1)); // protocol-conformance
  Buffer << "MA";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleReflectionMetadataSuperclassDescriptor(Node *node,
                                                        unsigned depth) {
  RETURN_IF_ERROR(
      mangleSingleChildNode(node, depth + 1)); // protocol-conformance
  Buffer << "MC";
  return ManglingError::Success;
}

ManglingError Remangler::mangleCurryThunk(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "Tc";
  return ManglingError::Success;
}

ManglingError Remangler::mangleDispatchThunk(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "Tj";
  return ManglingError::Success;
}

ManglingError Remangler::mangleMethodDescriptor(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "Tq";
  return ManglingError::Success;
}

ManglingError Remangler::mangleMethodLookupFunction(Node *node,
                                                    unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "Mu";
  return ManglingError::Success;
}

ManglingError Remangler::mangleObjCMetadataUpdateFunction(Node *node,
                                                          unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "MU";
  return ManglingError::Success;
}

ManglingError Remangler::mangleObjCResilientClassStub(Node *node,
                                                      unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "Ms";
  return ManglingError::Success;
}

ManglingError Remangler::mangleFullObjCResilientClassStub(Node *node,
                                                          unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "Mt";
  return ManglingError::Success;
}

ManglingError Remangler::mangleConcurrentFunctionType(Node *node,
                                                      unsigned depth) {
  Buffer << "Yb";
  return ManglingError::Success;
}

ManglingError Remangler::mangleAsyncAnnotation(Node *node, unsigned depth) {
  Buffer << "Ya";
  return ManglingError::Success;
}

ManglingError Remangler::mangleDifferentiableFunctionType(Node *node,
                                                          unsigned depth) {
  Buffer << "Yj" << (char)node->getIndex(); // differentiability kind
  return ManglingError::Success;
}

ManglingError Remangler::mangleGlobalActorFunctionType(Node *node,
                                                       unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "Yc";
  return ManglingError::Success;
}

ManglingError Remangler::mangleThrowsAnnotation(Node *node, unsigned depth) {
  Buffer << 'K';
  return ManglingError::Success;
}

ManglingError Remangler::mangleEmptyList(Node *node, unsigned depth) {
  Buffer << 'y';
  return ManglingError::Success;
}

ManglingError Remangler::mangleFirstElementMarker(Node *node, unsigned depth) {
  Buffer << '_';
  return ManglingError::Success;
}

ManglingError Remangler::mangleVariadicMarker(Node *node, unsigned depth) {
  Buffer << 'd';
  return ManglingError::Success;
}

ManglingError Remangler::mangleOutlinedCopy(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "WOy";
  return ManglingError::Success;
}

ManglingError Remangler::mangleOutlinedConsume(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "WOe";
  return ManglingError::Success;
}

ManglingError Remangler::mangleOutlinedRetain(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "WOr";
  return ManglingError::Success;
}

ManglingError Remangler::mangleOutlinedRelease(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "WOs";
  return ManglingError::Success;
}

ManglingError Remangler::mangleOutlinedInitializeWithTake(Node *node,
                                                          unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "WOb";
  return ManglingError::Success;
}

ManglingError Remangler::mangleOutlinedInitializeWithCopy(Node *node,
                                                          unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "WOc";
  return ManglingError::Success;
}

ManglingError Remangler::mangleOutlinedAssignWithTake(Node *node,
                                                      unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "WOd";
  return ManglingError::Success;
}

ManglingError Remangler::mangleOutlinedAssignWithCopy(Node *node,
                                                      unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "WOf";
  return ManglingError::Success;
}

ManglingError Remangler::mangleOutlinedDestroy(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "WOh";
  return ManglingError::Success;
}

ManglingError Remangler::mangleOutlinedVariable(Node *node, unsigned depth) {
  Buffer << "Tv";
  mangleIndex(node->getIndex());
  return ManglingError::Success;
}

ManglingError Remangler::mangleOutlinedReadOnlyObject(Node *node, unsigned depth) {
  Buffer << "Tv";
  mangleIndex(node->getIndex());
  Buffer << 'r';
  return ManglingError::Success;
}

ManglingError Remangler::mangleOutlinedBridgedMethod(Node *node,
                                                     unsigned depth) {
  Buffer << "Te";
  Buffer << node->getText();
  Buffer << "_";
  return ManglingError::Success;
}

ManglingError Remangler::mangleSILBoxTypeWithLayout(Node *node,
                                                    unsigned depth) {
  DEMANGLER_ASSERT(node->getNumChildren() == 1 || node->getNumChildren() == 3,
                   node);
  DEMANGLER_ASSERT(node->getChild(0)->getKind() == Node::Kind::SILBoxLayout,
                   node);
  auto layout = node->getChild(0);
  auto layoutTypeList = Factory.createNode(Node::Kind::TypeList);
  for (unsigned i = 0, e = layout->getNumChildren(); i < e; ++i) {
    DEMANGLER_ASSERT(
        layout->getChild(i)->getKind() == Node::Kind::SILBoxImmutableField ||
            layout->getChild(i)->getKind() == Node::Kind::SILBoxMutableField,
        layout->getChild(i));
    auto field = layout->getChild(i);
    DEMANGLER_ASSERT(field->getNumChildren() == 1 &&
                         field->getChild(0)->getKind() == Node::Kind::Type,
                     field);
    auto fieldType = field->getChild(0);
    // 'inout' mangling is used to represent mutable fields.
    if (field->getKind() == Node::Kind::SILBoxMutableField) {
      auto inout = Factory.createNode(Node::Kind::InOut);
      inout->addChild(fieldType->getChild(0), Factory);
      fieldType = Factory.createNode(Node::Kind::Type);
      fieldType->addChild(inout, Factory);
    }
    layoutTypeList->addChild(fieldType, Factory);
  }
  RETURN_IF_ERROR(mangleTypeList(layoutTypeList, depth + 1));

  if (node->getNumChildren() == 3) {
    auto signature = node->getChild(1);
    auto genericArgs = node->getChild(2);
    DEMANGLER_ASSERT(
        signature->getKind() == Node::Kind::DependentGenericSignature, node);
    DEMANGLER_ASSERT(genericArgs->getKind() == Node::Kind::TypeList, node);
    RETURN_IF_ERROR(mangleTypeList(genericArgs, depth + 1));
    RETURN_IF_ERROR(mangleDependentGenericSignature(signature, depth + 1));
    Buffer << "XX";
  } else {
    Buffer << "Xx";
  }
  return ManglingError::Success;
}

ManglingError Remangler::mangleSILBoxLayout(Node *node, unsigned depth) {
  // should be part of SILBoxTypeWithLayout
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleSILBoxMutableField(Node *node, unsigned depth) {
  // should be part of SILBoxTypeWithLayout
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleSILBoxImmutableField(Node *node,
                                                    unsigned depth) {
  // should be part of SILBoxTypeWithLayout
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError Remangler::mangleAssocTypePath(Node *node, unsigned depth) {
  bool FirstElem = true;
  for (NodePointer Child : *node) {
    RETURN_IF_ERROR(mangle(Child, depth + 1));
    mangleListSeparator(FirstElem);
  }
  return ManglingError::Success;
}

ManglingError Remangler::mangleModuleDescriptor(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangle(node->getChild(0), depth + 1));
  Buffer << "MXM";
  return ManglingError::Success;
}

ManglingError Remangler::mangleExtensionDescriptor(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangle(node->getChild(0), depth + 1));
  Buffer << "MXE";
  return ManglingError::Success;
}

ManglingError Remangler::mangleAnonymousDescriptor(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangle(node->getChild(0), depth + 1));
  if (node->getNumChildren() == 1) {
    Buffer << "MXX";
  } else {
    RETURN_IF_ERROR(mangleIdentifier(node->getChild(1), depth + 1));
    Buffer << "MXY";
  }
  return ManglingError::Success;
}

ManglingError Remangler::mangleAssociatedTypeGenericParamRef(Node *node,
                                                             unsigned depth) {
  RETURN_IF_ERROR(mangleType(node->getChild(0), depth + 1));
  RETURN_IF_ERROR(mangleAssocTypePath(node->getChild(1), depth + 1));
  Buffer << "MXA";
  return ManglingError::Success;
}

ManglingError Remangler::mangleTypeSymbolicReference(Node *node,
                                                     unsigned depth) {
  return mangle(
      Resolver(SymbolicReferenceKind::Context, (const void *)node->getIndex()),
      depth + 1);
}

ManglingError Remangler::mangleProtocolSymbolicReference(Node *node,
                                                         unsigned depth) {
  return mangle(
      Resolver(SymbolicReferenceKind::Context, (const void *)node->getIndex()),
      depth + 1);
}

ManglingError
Remangler::mangleOpaqueTypeDescriptorSymbolicReference(Node *node,
                                                       unsigned depth) {
  return mangle(
      Resolver(SymbolicReferenceKind::Context, (const void *)node->getIndex()),
      depth + 1);
}

ManglingError Remangler::mangleSugaredOptional(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleType(node->getChild(0), depth + 1));
  Buffer << "XSq";
  return ManglingError::Success;
}

ManglingError Remangler::mangleSugaredArray(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleType(node->getChild(0), depth + 1));
  Buffer << "XSa";
  return ManglingError::Success;
}

ManglingError Remangler::mangleSugaredDictionary(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleType(node->getChild(0), depth + 1));
  RETURN_IF_ERROR(mangleType(node->getChild(1), depth + 1));
  Buffer << "XSD";
  return ManglingError::Success;
}

ManglingError Remangler::mangleSugaredParen(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangleType(node->getChild(0), depth + 1));
  Buffer << "XSp";
  return ManglingError::Success;
}

ManglingError Remangler::mangleOpaqueReturnType(Node *node, unsigned depth) {
  if (node->hasChildren()
      && node->getFirstChild()->getKind() == Node::Kind::OpaqueReturnTypeIndex) {
    Buffer << "QR";
    mangleIndex(node->getFirstChild()->getIndex());
    return ManglingError::Success;
  }
  Buffer << "Qr";
  return ManglingError::Success;
}
ManglingError Remangler::mangleOpaqueReturnTypeIndex(Node *node, unsigned depth) {
  // Cannot appear unparented to an OpaqueReturnType.
  return ManglingError::WrongNodeType;
}
ManglingError Remangler::mangleOpaqueReturnTypeParent(Node *node, unsigned depth) {
  // Cannot appear unparented to an OpaqueReturnType.
  return ManglingError::WrongNodeType;
}
ManglingError Remangler::mangleOpaqueReturnTypeOf(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangle(node->getChild(0), depth + 1));
  Buffer << "QO";
  return ManglingError::Success;
}
ManglingError Remangler::mangleOpaqueType(Node *node, unsigned depth) {
  SubstitutionEntry entry;
  if (trySubstitution(node, entry))
    return ManglingError::Success;

  DEMANGLER_ASSERT(node->getNumChildren() >= 3, node);
  RETURN_IF_ERROR(mangle(node->getChild(0), depth + 1));
  auto boundGenerics = node->getChild(2);
  for (unsigned i = 0; i < boundGenerics->getNumChildren(); ++i) {
    Buffer << (i == 0 ? 'y' : '_');
    RETURN_IF_ERROR(mangleChildNodes(boundGenerics->getChild(i), depth + 1));
  }
  if (node->getNumChildren() >= 4) {
    auto retroactiveConformances = node->getChild(3);
    for (unsigned i = 0; i < retroactiveConformances->getNumChildren(); ++i) {
      RETURN_IF_ERROR(mangle(retroactiveConformances->getChild(i), depth + 1));
    }
  }
  Buffer << "Qo";
  mangleIndex(node->getChild(1)->getIndex());

  addSubstitution(entry);
  return ManglingError::Success;
}
ManglingError Remangler::mangleAccessorFunctionReference(Node *node,
                                                         unsigned depth) {
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError
Remangler::mangleCanonicalSpecializedGenericMetaclass(Node *node,
                                                      unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
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

ManglingError Remangler::mangleMetadataInstantiationCache(Node *node,
                                                          unsigned depth) {
  RETURN_IF_ERROR(mangleSingleChildNode(node, depth + 1));
  Buffer << "MK";
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

ManglingError Remangler::mangleGlobalVariableOnceToken(Node *node,
                                                       unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "Wz";
  return ManglingError::Success;
}

ManglingError Remangler::mangleGlobalVariableOnceFunction(Node *node,
                                                          unsigned depth) {
  RETURN_IF_ERROR(mangleChildNodes(node, depth + 1));
  Buffer << "WZ";
  return ManglingError::Success;
}

ManglingError Remangler::mangleGlobalVariableOnceDeclList(Node *node,
                                                          unsigned depth) {
  for (unsigned i = 0, e = node->getNumChildren(); i < e; ++i) {
    RETURN_IF_ERROR(mangle(node->getChild(i), depth + 1));
    Buffer << '_';
  }
  return ManglingError::Success;
}

ManglingError Remangler::mangleAccessibleFunctionRecord(Node *node,
                                                        unsigned depth) {
  Buffer << "HF";
  return ManglingError::Success;
}

ManglingError Remangler::mangleBackDeploymentThunk(Node *node,
                                                   unsigned depth) {
  Buffer << "Twb";
  return ManglingError::Success;
}

ManglingError Remangler::mangleBackDeploymentFallback(Node *node,
                                                      unsigned depth) {
  Buffer << "TwB";
  return ManglingError::Success;
}

ManglingError Remangler::mangleUniquable(Node *node, unsigned depth) {
  RETURN_IF_ERROR(mangle(node->getChild(0), depth + 1));
  Buffer << "Mq";
  return ManglingError::Success;
}

ManglingError Remangler::mangleExtendedExistentialTypeShape(Node *node,
                                                            unsigned depth) {
  NodePointer genSig, type;
  if (node->getNumChildren() == 1) {
    genSig = nullptr;
    type = node->getChild(0);
  } else {
    genSig = node->getChild(0);
    type = node->getChild(1);
  }

  if (genSig) {
    RETURN_IF_ERROR(mangle(genSig, depth + 1));  
  }
  RETURN_IF_ERROR(mangle(type, depth + 1));

  if (genSig)
    Buffer << "XG";
  else
    Buffer << "Xg";

  return ManglingError::Success;
}

ManglingError Remangler::mangleHasSymbolQuery(Node *node, unsigned depth) {
  Buffer << "TwS";
  return ManglingError::Success;
}

ManglingError Remangler::mangleSymbolicExtendedExistentialType(Node *node,
                                                     unsigned int depth) {
  RETURN_IF_ERROR(mangle(node->getChild(0), depth+1));
  for (auto arg: *node->getChild(1))
    RETURN_IF_ERROR(mangle(arg, depth+1));
  if (node->getNumChildren() > 2)
    for (auto conf: *node->getChild(2))
      RETURN_IF_ERROR(mangle(conf, depth+1));
  return ManglingError::Success;
}
ManglingError Remangler::
mangleUniqueExtendedExistentialTypeShapeSymbolicReference(Node *node,
                                                     unsigned int depth) {
  // We don't support absolute references in the mangling of these
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}
ManglingError Remangler::
mangleNonUniqueExtendedExistentialTypeShapeSymbolicReference(Node *node,
                                                     unsigned int depth) {
  // We don't support absolute references in the mangling of these
  return MANGLING_ERROR(ManglingError::UnsupportedNodeKind, node);
}

ManglingError
Remangler::mangleRuntimeDiscoverableAttributeRecord(Node *node,
                                                    unsigned depth) {
  Buffer << "Ha";
  return ManglingError::Success;
}

ManglingError
Remangler::mangleRuntimeAttributeGenerator(Node *node,
                                           unsigned depth) {
  RETURN_IF_ERROR(mangleChildNode(node, 0, depth + 1));
  RETURN_IF_ERROR(mangleChildNode(node, 1, depth + 1));
  Buffer << "fa";
  return ManglingError::Success;
}

} // anonymous namespace

/// The top-level interface to the remangler.
ManglingErrorOr<std::string> Demangle::mangleNode(NodePointer node) {
  return mangleNode(node, [](SymbolicReferenceKind, const void *) -> NodePointer {
                            return nullptr;
                          });
  //  unreachable("should not try to mangle a symbolic reference; "
  //              "resolve it to a non-symbolic demangling tree instead");
}

ManglingErrorOr<std::string>
Demangle::mangleNode(NodePointer node, SymbolicResolver resolver) {
  if (!node) return std::string();

  NodeFactory Factory;
  Remangler remangler(resolver, Factory);
  ManglingError err = remangler.mangle(node, 0);
  if (!err.isSuccess())
    return err;

  return remangler.str();
}

ManglingErrorOr<llvm::StringRef>
Demangle::mangleNode(NodePointer node, SymbolicResolver resolver,
                     NodeFactory &Factory) {
  if (!node)
    return StringRef();

  Remangler remangler(resolver, Factory);
  ManglingError err = remangler.mangle(node, 0);
  if (!err.isSuccess())
    return err;

  return remangler.getBufferStr();
}

bool Demangle::isSpecialized(Node *node) {
  // We shouldn't get here with node being NULL; if we do, assert in debug,
  // or return false at runtime (which should at least help diagnose things
  // further if it happens).
  assert(node);
  if (!node)
    return false;

  switch (node->getKind()) {
    case Node::Kind::BoundGenericStructure:
    case Node::Kind::BoundGenericEnum:
    case Node::Kind::BoundGenericClass:
    case Node::Kind::BoundGenericOtherNominalType:
    case Node::Kind::BoundGenericTypeAlias:
    case Node::Kind::BoundGenericProtocol:
    case Node::Kind::BoundGenericFunction:
    case Node::Kind::ConstrainedExistential:
      return true;

    case Node::Kind::Structure:
    case Node::Kind::Enum:
    case Node::Kind::Class:
    case Node::Kind::TypeAlias:
    case Node::Kind::OtherNominalType:
    case Node::Kind::Protocol:
    case Node::Kind::Function:
    case Node::Kind::Allocator:
    case Node::Kind::Constructor:
    case Node::Kind::Destructor:
    case Node::Kind::Variable:
    case Node::Kind::Subscript:
    case Node::Kind::ExplicitClosure:
    case Node::Kind::ImplicitClosure:
    case Node::Kind::Initializer:
    case Node::Kind::PropertyWrapperBackingInitializer:
    case Node::Kind::PropertyWrapperInitFromProjectedValue:
    case Node::Kind::DefaultArgumentInitializer:
    case Node::Kind::RuntimeAttributeGenerator:
    case Node::Kind::Getter:
    case Node::Kind::Setter:
    case Node::Kind::WillSet:
    case Node::Kind::DidSet:
    case Node::Kind::ReadAccessor:
    case Node::Kind::ModifyAccessor:
    case Node::Kind::UnsafeAddressor:
    case Node::Kind::UnsafeMutableAddressor:
    case Node::Kind::Static:
      assert(node->getNumChildren() > 0);
      return node->getNumChildren() > 0 && isSpecialized(node->getChild(0));

    case Node::Kind::Extension:
      assert(node->getNumChildren() > 1);
      return node->getNumChildren() > 1 && isSpecialized(node->getChild(1));

    default:
      return false;
  }
}

ManglingErrorOr<NodePointer> Demangle::getUnspecialized(Node *node,
                                                        NodeFactory &Factory) {
  unsigned NumToCopy = 2;
  switch (node->getKind()) {
    case Node::Kind::Function:
    case Node::Kind::Getter:
    case Node::Kind::Setter:
    case Node::Kind::WillSet:
    case Node::Kind::DidSet:
    case Node::Kind::ReadAccessor:
    case Node::Kind::ModifyAccessor:
    case Node::Kind::UnsafeAddressor:
    case Node::Kind::UnsafeMutableAddressor:
    case Node::Kind::Allocator:
    case Node::Kind::Constructor:
    case Node::Kind::Destructor:
    case Node::Kind::Variable:
    case Node::Kind::Subscript:
    case Node::Kind::ExplicitClosure:
    case Node::Kind::ImplicitClosure:
    case Node::Kind::Initializer:
    case Node::Kind::PropertyWrapperBackingInitializer:
    case Node::Kind::PropertyWrapperInitFromProjectedValue:
    case Node::Kind::DefaultArgumentInitializer:
    case Node::Kind::RuntimeAttributeGenerator:
    case Node::Kind::Static:
      NumToCopy = node->getNumChildren();
      LLVM_FALLTHROUGH;
    case Node::Kind::Structure:
    case Node::Kind::Enum:
    case Node::Kind::Class:
    case Node::Kind::TypeAlias:
    case Node::Kind::OtherNominalType: {
      NodePointer result = Factory.createNode(node->getKind());
      NodePointer parentOrModule = node->getChild(0);
      if (isSpecialized(parentOrModule)) {
        auto unspec = getUnspecialized(parentOrModule, Factory);
        if (!unspec.isSuccess())
          return unspec;
        parentOrModule = unspec.result();
      }
      result->addChild(parentOrModule, Factory);
      for (unsigned Idx = 1; Idx < NumToCopy; ++Idx) {
        result->addChild(node->getChild(Idx), Factory);
      }
      return result;
    }

    case Node::Kind::BoundGenericStructure:
    case Node::Kind::BoundGenericEnum:
    case Node::Kind::BoundGenericClass:
    case Node::Kind::BoundGenericProtocol:
    case Node::Kind::BoundGenericOtherNominalType:
    case Node::Kind::BoundGenericTypeAlias: {
      NodePointer unboundType = node->getChild(0);
      DEMANGLER_ASSERT(unboundType->getKind() == Node::Kind::Type, unboundType);
      NodePointer nominalType = unboundType->getChild(0);
      if (isSpecialized(nominalType))
        return getUnspecialized(nominalType, Factory);
      return nominalType;
    }

    case Node::Kind::ConstrainedExistential: {
      NodePointer unboundType = node->getChild(0);
      DEMANGLER_ASSERT(unboundType->getKind() == Node::Kind::Type, unboundType);
      return unboundType;
    }

    case Node::Kind::BoundGenericFunction: {
      NodePointer unboundFunction = node->getChild(0);
      DEMANGLER_ASSERT(unboundFunction->getKind() == Node::Kind::Function ||
                           unboundFunction->getKind() ==
                               Node::Kind::Constructor,
                       unboundFunction);
      if (isSpecialized(unboundFunction))
        return getUnspecialized(unboundFunction, Factory);
      return unboundFunction;
    }

    case Node::Kind::Extension: {
      NodePointer parent = node->getChild(1);
      if (!isSpecialized(parent))
        return node;
      auto unspec = getUnspecialized(parent, Factory);
      if (!unspec.isSuccess())
        return unspec.error();
      NodePointer result = Factory.createNode(Node::Kind::Extension);
      result->addChild(node->getFirstChild(), Factory);
      result->addChild(unspec.result(), Factory);
      if (node->getNumChildren() == 3) {
        // Add the generic signature of the extension.
        result->addChild(node->getChild(2), Factory);
      }
      return result;
    }
    default:
      return MANGLING_ERROR(ManglingError::BadNominalTypeKind, node);
  }
}
