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

#include "swift/Demangling/Demangler.h"
#include "swift/Demangling/Punycode.h"
#include "swift/Demangling/ManglingUtils.h"
#include "swift/Demangling/ManglingMacros.h"
#include "swift/AST/Ownership.h"
#include "swift/Strings.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"
#include "RemanglerBase.h"
#include <cstdio>
#include <cstdlib>

using namespace swift;
using namespace Demangle;
using namespace Mangle;

[[noreturn]]
static void unreachable(const char *Message) {
  fprintf(stderr, "fatal error: %s\n", Message);
  std::abort();
}

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

  void mangleIndex(Node::IndexType value) {
    if (value == 0) {
      Buffer << '_';
    } else {
      Buffer << (value - 1) << '_';
    }
  }
  void mangleDependentConformanceIndex(Node *node, unsigned depth);

  void mangleChildNodes(Node *node, unsigned depth) {
    mangleNodes(node->begin(), node->end(), depth);
  }
  void mangleChildNodesReversed(Node *node, unsigned depth) {
    for (size_t Idx = 0, Num = node->getNumChildren(); Idx < Num; ++Idx) {
      mangleChildNode(node, Num - Idx - 1, depth);
    }
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
    if (index < node->getNumChildren())
      mangle(node->begin()[index], depth);
  }

  void manglePureProtocol(Node *Proto, unsigned depth) {
    Proto = skipType(Proto);
    if (mangleStandardSubstitution(Proto))
      return;

    mangleChildNodes(Proto, depth);
  }

  void mangleProtocolList(Node *protocols, Node *superclass,
                          bool hasExplicitAnyObject, unsigned depth);

  bool trySubstitution(Node *node, SubstitutionEntry &entry,
                       bool treatAsIdentifier = false);

  void mangleIdentifierImpl(Node *node, bool isOperator);

  bool mangleStandardSubstitution(Node *node);

  void mangleDependentGenericParamIndex(Node *node,
                                        const char *nonZeroPrefix = "",
                                        char zeroOp = 'z');

  std::pair<int, Node *> mangleConstrainedType(Node *node, unsigned depth);

  void mangleFunctionSignature(Node *FuncType, unsigned depth) {
    mangleChildNodesReversed(FuncType, depth);
  }

  void mangleGenericSpecializationNode(Node *node, const char *operatorStr,
                                       unsigned depth);
  void mangleAnyNominalType(Node *node, unsigned depth);
  void mangleAnyGenericType(Node *node, StringRef TypeOp, unsigned depth);
  void mangleGenericArgs(Node *node, char &Separator, unsigned depth,
                         bool fullSubstitutionMap = false);
  void mangleAnyConstructor(Node *node, char kindOp, unsigned depth);
  void mangleAbstractStorage(Node *node, StringRef accessorCode,
                             unsigned depth);
  void mangleAnyProtocolConformance(Node *node, unsigned depth);

  void mangleKeyPathThunkHelper(Node *node, StringRef op, unsigned depth);

  void mangleAutoDiffFunctionOrSimpleThunk(Node *node, StringRef op,
                                           unsigned depth);

#define NODE(ID) void mangle##ID(Node *node, unsigned depth);
#define CONTEXT_NODE(ID)                                                       \
  void mangle##ID(Node *node, unsigned depth);                                 \
//    void mangle##ID(Node *node, unsigned depth, EntityContext &ctx);
#include "swift/Demangling/DemangleNodes.def"

public:
  Remangler(SymbolicResolver Resolver, NodeFactory &Factory)
       : RemanglerBase(Factory), Resolver(Resolver) { }

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

std::pair<int, Node *> Remangler::mangleConstrainedType(Node *node,
                                                        unsigned depth) {
  if (node->getKind() == Node::Kind::Type)
    node = getChildOfType(node);

  SubstitutionEntry entry;
  if (trySubstitution(node, entry))
    return {-1, nullptr};

  Vector<Node *> Chain;
  while (node->getKind() == Node::Kind::DependentMemberType) {
    Chain.push_back(node->getChild(1), Factory);
    node = getChildOfType(node->getFirstChild());
  }

  if (node->getKind() != Node::Kind::DependentGenericParamType) {
    mangle(node, depth + 1);
    if (!Chain.size())
      return {-1, nullptr};
    node = nullptr;
  }

  const char *ListSeparator = (Chain.size() > 1 ? "_" : "");
  for (unsigned i = 1, n = Chain.size(); i <= n; ++i) {
    Node *DepAssocTyRef = Chain[n - i];
    mangle(DepAssocTyRef, depth + 1);
    Buffer << ListSeparator;
    ListSeparator = "";
  }
  if (!Chain.empty())
    addSubstitution(entry);
  return {(int)Chain.size(), node};
}

void Remangler::mangleAnyGenericType(Node *node, StringRef TypeOp,
                                     unsigned depth) {
  SubstitutionEntry entry;
  if (trySubstitution(node, entry)) return;
  mangleChildNodes(node, depth + 1);
  Buffer << TypeOp;
  addSubstitution(entry);
}

void Remangler::mangleAnyNominalType(Node *node, unsigned depth) {
  if (depth > Remangler::MaxDepth) {
    // FIXME: error handling needs doing properly (rdar://79725187)
    unreachable("too complex to remangle");
  }

  if (isSpecialized(node)) {
    SubstitutionEntry entry;
    if (trySubstitution(node, entry))
      return;

    NodePointer unboundType = getUnspecialized(node, Factory);
    mangleAnyNominalType(unboundType, depth + 1);
    char Separator = 'y';
    mangleGenericArgs(node, Separator, depth + 1);

    if (node->getNumChildren() == 3) {
      // Retroactive conformances.
      auto listNode = node->getChild(2);
      for (size_t Idx = 0, Num = listNode->getNumChildren(); Idx < Num; ++Idx) {
        mangle(listNode->getChild(Idx), depth + 1);
      }
    }

    Buffer << 'G';
    addSubstitution(entry);
    return;
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
    unreachable("bad nominal type kind");
  }
}

void Remangler::mangleGenericArgs(Node *node, char &Separator, unsigned depth,
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

      mangleGenericArgs(node->getChild(0), Separator, depth + 1,
                        fullSubstitutionMap);
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
      if (!fullSubstitutionMap)
        break;

      mangleGenericArgs(node->getChild(0), Separator, depth + 1,
                        fullSubstitutionMap);
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
      assert(unboundType->getKind() == Node::Kind::Type);
      NodePointer nominalType = unboundType->getChild(0);
      NodePointer parentOrModule = nominalType->getChild(0);
      mangleGenericArgs(parentOrModule, Separator, depth + 1,
                        fullSubstitutionMap);
      Buffer << Separator;
      Separator = '_';
      mangleChildNodes(node->getChild(1), depth + 1);
      break;
    }
      
    case Node::Kind::BoundGenericFunction: {
      fullSubstitutionMap = true;

      NodePointer unboundFunction = node->getChild(0);
      assert(unboundFunction->getKind() == Node::Kind::Function ||
             unboundFunction->getKind() == Node::Kind::Constructor);
      NodePointer parentOrModule = unboundFunction->getChild(0);
      mangleGenericArgs(parentOrModule, Separator, depth + 1,
                        fullSubstitutionMap);
      Buffer << Separator;
      Separator = '_';
      mangleChildNodes(node->getChild(1), depth + 1);
      break;
    }

    case Node::Kind::Extension:
      mangleGenericArgs(node->getChild(1), Separator, depth + 1,
                        fullSubstitutionMap);
      break;

    default:
      break;
  }
}

void Remangler::mangleAbstractStorage(Node *node, StringRef accessorCode,
                                      unsigned depth) {
  mangleChildNodes(node, depth + 1);
  switch (node->getKind()) {
    case Node::Kind::Subscript: Buffer << "i"; break;
    case Node::Kind::Variable: Buffer << "v"; break;
    default: unreachable("Not a storage node");
  }
  Buffer << accessorCode;
}

void Remangler::mangleAllocator(Node *node, unsigned depth) {
  mangleAnyConstructor(node, 'C', depth + 1);
}

void Remangler::mangleArgumentTuple(Node *node, unsigned depth) {
  Node *Child = skipType(getSingleChild(node));
  if (Child->getKind() == Node::Kind::Tuple &&
      Child->getNumChildren() == 0) {
    Buffer << 'y';
    return;
  }
  mangle(Child, depth + 1);
}

void Remangler::mangleAssociatedType(Node *node, unsigned depth) {
  unreachable("unsupported node");
}

void Remangler::mangleAssociatedTypeRef(Node *node, unsigned depth) {
  SubstitutionEntry entry;
  if (trySubstitution(node, entry)) return;
  mangleChildNodes(node, depth + 1);
  Buffer << "Qa";
  addSubstitution(entry);
}

void Remangler::mangleAssociatedTypeDescriptor(Node *node, unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "Tl";
}

void Remangler::mangleAssociatedConformanceDescriptor(Node *node,
                                                      unsigned depth) {
  mangle(node->getChild(0), depth + 1);
  mangle(node->getChild(1), depth + 1);
  manglePureProtocol(node->getChild(2), depth + 1);
  Buffer << "Tn";
}

void Remangler::mangleDefaultAssociatedConformanceAccessor(Node *node,
                                                           unsigned depth) {
  mangle(node->getChild(0), depth + 1);
  mangle(node->getChild(1), depth + 1);
  manglePureProtocol(node->getChild(2), depth + 1);
  Buffer << "TN";
}

void Remangler::mangleBaseConformanceDescriptor(Node *node, unsigned depth) {
  mangle(node->getChild(0), depth + 1);
  manglePureProtocol(node->getChild(1), depth + 1);
  Buffer << "Tb";
}

void Remangler::mangleAssociatedTypeMetadataAccessor(Node *node,
                                                     unsigned depth) {
  mangleChildNodes(node, depth + 1); // protocol conformance, identifier
  Buffer << "Wt";
}

void Remangler::mangleDefaultAssociatedTypeMetadataAccessor(Node *node,
                                                            unsigned depth) {
  mangleChildNodes(node, depth + 1); // protocol conformance, identifier
  Buffer << "TM";
}

void Remangler::mangleAssociatedTypeWitnessTableAccessor(Node *node,
                                                         unsigned depth) {
  mangleChildNodes(node, depth + 1); // protocol conformance, type, protocol
  Buffer << "WT";
}

void Remangler::mangleBaseWitnessTableAccessor(Node *node, unsigned depth) {
  mangleChildNodes(node, depth + 1); // protocol conformance, protocol
  Buffer << "Wb";
}

void Remangler::mangleAutoClosureType(Node *node, unsigned depth) {
  mangleChildNodesReversed(node, depth + 1); // argument tuple, result type
  Buffer << "XK";
}

void Remangler::mangleEscapingAutoClosureType(Node *node, unsigned depth) {
  mangleChildNodesReversed(node, depth + 1); // argument tuple, result type
  Buffer << "XA";
}

void Remangler::mangleNoEscapeFunctionType(Node *node, unsigned depth) {
  mangleChildNodesReversed(node, depth + 1); // argument tuple, result type
  Buffer << "XE";
}

void Remangler::mangleBoundGenericClass(Node *node, unsigned depth) {
  mangleAnyNominalType(node, depth + 1);
}

void Remangler::mangleBoundGenericEnum(Node *node, unsigned depth) {
  Node *Enum = node->getChild(0)->getChild(0);
  assert(Enum->getKind() == Node::Kind::Enum);
  Node *Mod = Enum->getChild(0);
  Node *Id = Enum->getChild(1);
  if (Mod->getKind() == Node::Kind::Module && Mod->getText() == STDLIB_NAME &&
      Id->getKind() == Node::Kind::Identifier && Id->getText() == "Optional") {
    SubstitutionEntry entry;
    if (trySubstitution(node, entry))
      return;
    mangleSingleChildNode(node->getChild(1), depth + 1);
    Buffer << "Sg";
    addSubstitution(entry);
    return;
  }
  mangleAnyNominalType(node, depth + 1);
}

void Remangler::mangleBoundGenericStructure(Node *node, unsigned depth) {
  mangleAnyNominalType(node, depth + 1);
}

void Remangler::mangleBoundGenericOtherNominalType(Node *node, unsigned depth) {
  mangleAnyNominalType(node, depth + 1);
}

void Remangler::mangleBoundGenericProtocol(Node *node, unsigned depth) {
  mangleAnyNominalType(node, depth + 1);
}

void Remangler::mangleBoundGenericTypeAlias(Node *node, unsigned depth) {
  mangleAnyNominalType(node, depth + 1);
}

void Remangler::mangleBoundGenericFunction(Node *node, unsigned depth) {
  SubstitutionEntry entry;
  if (trySubstitution(node, entry))
    return;

  NodePointer unboundFunction = getUnspecialized(node, Factory);
  mangleFunction(unboundFunction, depth + 1);
  char Separator = 'y';
  mangleGenericArgs(node, Separator, depth + 1);
  Buffer << 'G';
  addSubstitution(entry);
}

void Remangler::mangleBuiltinTypeName(Node *node, unsigned depth) {
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
      unreachable("unexpected builtin vector type");
    }
    Buffer << "Bv" << text.substr(0, splitIdx) << '_';
  } else {
    unreachable("unexpected builtin type");
  }
}

void Remangler::mangleCFunctionPointer(Node *node, unsigned depth) {
  if (node->getNumChildren() > 0 &&
      node->getFirstChild()->getKind() == Node::Kind::ClangType) {
    for (size_t Idx = node->getNumChildren() - 1; Idx >= 1; --Idx) {
      mangleChildNode(node, Idx, depth + 1);
    }
    Buffer << "XzC";
    mangleClangType(node->getFirstChild(), depth + 1);
    return;
  }
  mangleChildNodesReversed(node, depth + 1); // argument tuple, result type
  Buffer << "XC";
}

void Remangler::mangleClass(Node *node, unsigned depth) {
  mangleAnyNominalType(node, depth + 1);
}

void Remangler::mangleAnyConstructor(Node *node, char kindOp, unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "f" << kindOp;
}

void Remangler::mangleConstructor(Node *node, unsigned depth) {
  mangleAnyConstructor(node, 'c', depth);
}

void Remangler::mangleCoroutineContinuationPrototype(Node *node,
                                                     unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "TC";
}

void Remangler::manglePredefinedObjCAsyncCompletionHandlerImpl(Node *node,
                                                               unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "TZ";
}

void Remangler::mangleObjCAsyncCompletionHandlerImpl(Node *node,
                                                     unsigned depth) {
  mangleChildNode(node, 0, depth + 1);
  mangleChildNode(node, 1, depth + 1);
  if (node->getNumChildren() == 4)
    mangleChildNode(node, 3, depth + 1);
  Buffer << "Tz";
  mangleChildNode(node, 2, depth + 1);
}

void Remangler::mangleDeallocator(Node *node, unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "fD";
}

void Remangler::mangleDeclContext(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
}

void Remangler::mangleDefaultArgumentInitializer(Node *node, unsigned depth) {
  mangleChildNode(node, 0, depth + 1);
  Buffer << "fA";
  mangleChildNode(node, 1, depth + 1);
}

void Remangler::mangleAsyncFunctionPointer(Node *node, unsigned depth) {
  Buffer << "Tu";
}

void Remangler::mangleDependentAssociatedTypeRef(Node *node, unsigned depth) {
  mangleIdentifier(node->getFirstChild(), depth);
  if (node->getNumChildren() > 1)
    mangleChildNode(node, 1, depth + 1);
}

void Remangler::mangleDependentGenericConformanceRequirement(Node *node,
                                                             unsigned depth) {
  Node *ProtoOrClass = node->getChild(1);
  if (ProtoOrClass->getFirstChild()->getKind() == Node::Kind::Protocol) {
    manglePureProtocol(ProtoOrClass, depth + 1);
    auto NumMembersAndParamIdx =
        mangleConstrainedType(node->getChild(0), depth + 1);
    assert(NumMembersAndParamIdx.first < 0 || NumMembersAndParamIdx.second);
    switch (NumMembersAndParamIdx.first) {
      case -1: Buffer << "RQ"; return; // substitution
      case 0: Buffer << "R"; break;
      case 1: Buffer << "Rp"; break;
      default: Buffer << "RP"; break;
    }
    mangleDependentGenericParamIndex(NumMembersAndParamIdx.second);
    return;
  }
  mangle(ProtoOrClass, depth + 1);
  auto NumMembersAndParamIdx =
      mangleConstrainedType(node->getChild(0), depth + 1);
  assert(NumMembersAndParamIdx.first < 0 || NumMembersAndParamIdx.second);
  switch (NumMembersAndParamIdx.first) {
    case -1: Buffer << "RB"; return; // substitution
    case 0: Buffer << "Rb"; break;
    case 1: Buffer << "Rc"; break;
    default: Buffer << "RC"; break;
  }
  mangleDependentGenericParamIndex(NumMembersAndParamIdx.second);
  return;
}

void Remangler::mangleDependentGenericParamCount(Node *node, unsigned depth) {
  unreachable("handled inline in DependentGenericSignature");
}

void Remangler::mangleDependentGenericParamType(Node *node, unsigned depth) {
  if (node->getChild(0)->getIndex() == 0
      && node->getChild(1)->getIndex() == 0) {
    Buffer << 'x';
    return;
  }
  Buffer << 'q';
  mangleDependentGenericParamIndex(node);
}

void Remangler::mangleDependentGenericSameTypeRequirement(Node *node,
                                                          unsigned depth) {
  mangleChildNode(node, 1, depth + 1);
  auto NumMembersAndParamIdx =
      mangleConstrainedType(node->getChild(0), depth + 1);
  assert(NumMembersAndParamIdx.first < 0 || NumMembersAndParamIdx.second);
  switch (NumMembersAndParamIdx.first) {
    case -1: Buffer << "RS"; return; // substitution
    case 0: Buffer << "Rs"; break;
    case 1: Buffer << "Rt"; break;
    default: Buffer << "RT"; break;
  }
  mangleDependentGenericParamIndex(NumMembersAndParamIdx.second);
}

void Remangler::mangleDependentGenericLayoutRequirement(Node *node,
                                                        unsigned depth) {
  auto NumMembersAndParamIdx =
      mangleConstrainedType(node->getChild(0), depth + 1);
  assert(NumMembersAndParamIdx.first < 0 || NumMembersAndParamIdx.second);
  switch (NumMembersAndParamIdx.first) {
    case -1: Buffer << "RL"; break; // substitution
    case 0: Buffer << "Rl"; break;
    case 1: Buffer << "Rm"; break;
    default: Buffer << "RM"; break;
  }
  // If not a substitution, mangle the dependent generic param index.
  if (NumMembersAndParamIdx.first != -1)
    mangleDependentGenericParamIndex(NumMembersAndParamIdx.second);
  assert(node->getChild(1)->getKind() == Node::Kind::Identifier);
  assert(node->getChild(1)->getText().size() == 1);
  Buffer << node->getChild(1)->getText()[0];
  if (node->getNumChildren() >=3)
    mangleChildNode(node, 2, depth + 1);
  if (node->getNumChildren() >=4)
    mangleChildNode(node, 3, depth + 1);
}

void Remangler::mangleDependentGenericSignature(Node *node, unsigned depth) {
  size_t ParamCountEnd = 0;
  for (size_t Idx = 0, Num = node->getNumChildren(); Idx < Num; ++Idx) {
    Node *Child = node->getChild(Idx);
    if (Child->getKind() == Node::Kind::DependentGenericParamCount) {
      ParamCountEnd = Idx + 1;
    } else {
      // requirement
      mangleChildNode(node, Idx, depth + 1);
    }
  }
  // If there's only one generic param, mangle nothing.
  if (ParamCountEnd == 1 && node->getChild(0)->getIndex() == 1) {
    Buffer << 'l';
    return;
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
}

void Remangler::mangleDependentGenericType(Node *node, unsigned depth) {
  mangleChildNodesReversed(node, depth + 1); // type, generic signature
  Buffer << 'u';
}

void Remangler::mangleDependentMemberType(Node *node, unsigned depth) {
  auto NumMembersAndParamIdx = mangleConstrainedType(node, depth + 1);
  switch (NumMembersAndParamIdx.first) {
    case -1:
      break; // substitution
    case 0:
      unreachable("wrong dependent member type");
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
}

void Remangler::mangleDependentPseudogenericSignature(Node *node,
                                                      unsigned depth) {
  unreachable("handled inline");
}

void Remangler::mangleDestructor(Node *node, unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "fd";
}

void Remangler::mangleDidSet(Node *node, unsigned depth) {
  mangleAbstractStorage(node->getFirstChild(), "W", depth + 1);
}

void Remangler::mangleDirectness(Node *node, unsigned depth) {
  if (node->getIndex() == unsigned(Directness::Direct)) {
    Buffer << 'd';
  } else {
    assert(node->getIndex() == unsigned(Directness::Indirect));
    Buffer << 'i';
  }
}

void Remangler::mangleDynamicAttribute(Node *node, unsigned depth) {
  Buffer << "TD";
}

void Remangler::mangleDirectMethodReferenceAttribute(Node *node,
                                                     unsigned depth) {
  Buffer << "Td";
}

void Remangler::mangleDynamicSelf(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1); // type
  Buffer << "XD";
}

void Remangler::mangleEnum(Node *node, unsigned depth) {
  mangleAnyNominalType(node, depth + 1);
}

void Remangler::mangleErrorType(Node *node, unsigned depth) { Buffer << "Xe"; }

void Remangler::mangleExistentialMetatype(Node *node, unsigned depth) {
  if (node->getFirstChild()->getKind() == Node::Kind::MetatypeRepresentation) {
    mangleChildNode(node, 1, depth + 1);
    Buffer << "Xm";
    mangleChildNode(node, 0, depth + 1);
  } else {
    mangleSingleChildNode(node, depth + 1);
    Buffer << "Xp";
  }
}

void Remangler::mangleExplicitClosure(Node *node, unsigned depth) {
  mangleChildNode(node, 0, depth + 1); // context
  mangleChildNode(node, 2, depth + 1); // type
  Buffer << "fU";
  mangleChildNode(node, 1, depth + 1); // index
}

void Remangler::mangleExtension(Node *node, unsigned depth) {
  mangleChildNode(node, 1, depth + 1);
  mangleChildNode(node, 0, depth + 1);
  if (node->getNumChildren() == 3)
    mangleChildNode(node, 2, depth + 1); // generic signature
  Buffer << 'E';
}

void Remangler::mangleAnonymousContext(Node *node, unsigned depth) {
  mangleChildNode(node, 1, depth + 1);
  mangleChildNode(node, 0, depth + 1);
  if (node->getNumChildren() >= 3)
    mangleTypeList(node->getChild(2), depth + 1);
  else
    Buffer << 'y';
  Buffer << "XZ";
}

void Remangler::mangleFieldOffset(Node *node, unsigned depth) {
  mangleChildNode(node, 1, depth + 1); // variable
  Buffer << "Wv";
  mangleChildNode(node, 0, depth + 1); // directness
}

void Remangler::mangleEnumCase(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1); // enum case
  Buffer << "WC";
}

void Remangler::mangleFullTypeMetadata(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "Mf";
}

void Remangler::mangleFunction(Node *node, unsigned depth) {
  mangleChildNode(node, 0, depth + 1); // context
  mangleChildNode(node, 1, depth + 1); // name

  bool hasLabels = node->getChild(2)->getKind() == Node::Kind::LabelList;
  Node *FuncType = getSingleChild(node->getChild(hasLabels ? 3 : 2));

  if (hasLabels)
    mangleChildNode(node, 2, depth + 1); // parameter labels

  if (FuncType->getKind() == Node::Kind::DependentGenericType) {
    mangleFunctionSignature(getSingleChild(FuncType->getChild(1)), depth + 1);
    mangleChildNode(FuncType, 0, depth + 1); // generic signature
  } else {
    mangleFunctionSignature(FuncType, depth + 1);
  }

  Buffer << "F";
}

void Remangler::mangleFunctionSignatureSpecialization(Node *node,
                                                      unsigned depth) {
  for (NodePointer Param : *node) {
    if (Param->getKind() == Node::Kind::FunctionSignatureSpecializationParam &&
        Param->getNumChildren() > 0) {
      Node *KindNd = Param->getChild(0);
      switch (FunctionSigSpecializationParamKind(KindNd->getIndex())) {
        case FunctionSigSpecializationParamKind::ConstantPropFunction:
        case FunctionSigSpecializationParamKind::ConstantPropGlobal:
          mangleIdentifier(Param->getChild(1), depth + 1);
          break;
        case FunctionSigSpecializationParamKind::ConstantPropString: {
          NodePointer TextNd = Param->getChild(2);
          StringRef Text = TextNd->getText();
          if (!Text.empty() && (isDigit(Text[0]) || Text[0] == '_')) {
            std::string Buffer = "_";
            Buffer.append(Text.data(), Text.size());
            TextNd = Factory.createNode(Node::Kind::Identifier, Buffer);
          }
          mangleIdentifier(TextNd, depth + 1);
          break;
        }
        case FunctionSigSpecializationParamKind::ClosureProp:
          mangleIdentifier(Param->getChild(1), depth + 1);
          for (unsigned i = 2, e = Param->getNumChildren(); i != e; ++i) {
            mangleType(Param->getChild(i), depth + 1);
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
    mangle(Child, depth + 1);

    if (Child->getKind() == Node::Kind::SpecializationPassID &&
        node->hasIndex()) {
      Buffer << node->getIndex();
    }
  }
  if (!returnValMangled)
    Buffer << "_n";
}

void Remangler::mangleFunctionSignatureSpecializationReturn(Node *node,
                                                            unsigned depth) {
  mangleFunctionSignatureSpecializationParam(node, depth + 1);
}

void Remangler::mangleFunctionSignatureSpecializationParam(Node *node,
                                                           unsigned depth) {
  if (!node->hasChildren()) {
    Buffer << 'n';
    return;
  }

  // The first child is always a kind that specifies the type of param that we
  // have.
  Node *KindNd = node->getChild(0);
  unsigned kindValue = KindNd->getIndex();
  auto kind = FunctionSigSpecializationParamKind(kindValue);

  switch (kind) {
    case FunctionSigSpecializationParamKind::ConstantPropFunction:
      Buffer << "pf";
      return;
    case FunctionSigSpecializationParamKind::ConstantPropGlobal:
      Buffer << "pg";
      return;
    case FunctionSigSpecializationParamKind::ConstantPropInteger:
      Buffer << "pi" << node->getChild(1)->getText();
      return;
    case FunctionSigSpecializationParamKind::ConstantPropFloat:
      Buffer << "pd" << node->getChild(1)->getText();
      return;
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
        unreachable("Unknown encoding");
      }
      return;
    }
    case FunctionSigSpecializationParamKind::ClosureProp:
      Buffer << 'c';
      return;
    case FunctionSigSpecializationParamKind::BoxToValue:
      Buffer << 'i';
      return;
    case FunctionSigSpecializationParamKind::BoxToStack:
      Buffer << 's';
      return;
    case FunctionSigSpecializationParamKind::SROA:
      Buffer << 'x';
      return;
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
      return;
  }
}

void Remangler::mangleFunctionSignatureSpecializationParamKind(Node *node,
                                                               unsigned depth) {
  unreachable("handled inline");
}

void Remangler::mangleFunctionSignatureSpecializationParamPayload(
    Node *node, unsigned depth) {
  unreachable("handled inline");
}

void Remangler::mangleFunctionType(Node *node, unsigned depth) {
  mangleFunctionSignature(node, depth + 1);
  Buffer << 'c';
}

void Remangler::mangleGenericProtocolWitnessTable(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "WG";
}

void Remangler::mangleGenericProtocolWitnessTableInstantiationFunction(
    Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "WI";
}

void Remangler::mangleResilientProtocolWitnessTable(Node *node,
                                                    unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "Wr";
}

void Remangler::mangleGenericPartialSpecialization(Node *node, unsigned depth) {
  for (NodePointer Child : *node) {
    if (Child->getKind() == Node::Kind::GenericSpecializationParam) {
      mangleChildNode(Child, 0, depth + 1);
      break;
    }
  }
  Buffer << (node->getKind() ==
        Node::Kind::GenericPartialSpecializationNotReAbstracted ? "TP" : "Tp");
  for (NodePointer Child : *node) {
    if (Child->getKind() != Node::Kind::GenericSpecializationParam)
      mangle(Child, depth + 1);
  }
}

void Remangler::mangleGenericPartialSpecializationNotReAbstracted(
    Node *node, unsigned depth) {
  mangleGenericPartialSpecialization(node, depth + 1);
}

void Remangler::mangleGenericSpecializationNode(Node *node,
                                                const char *operatorStr,
                                                unsigned depth) {
  bool FirstParam = true;
  for (NodePointer Child : *node) {
    if (Child->getKind() == Node::Kind::GenericSpecializationParam) {
      mangleChildNode(Child, 0, depth + 1);
      mangleListSeparator(FirstParam);
    }
  }
  assert(!FirstParam && "generic specialization with no substitutions");

  Buffer << operatorStr;

  for (NodePointer Child : *node) {
    if (Child->getKind() != Node::Kind::GenericSpecializationParam)
      mangle(Child, depth + 1);
  }
}

void Remangler::mangleGenericSpecialization(Node *node, unsigned depth) {
  mangleGenericSpecializationNode(node, "Tg", depth + 1);
}

void Remangler::mangleGenericSpecializationPrespecialized(Node *node,
                                                          unsigned depth) {
  mangleGenericSpecializationNode(node, "Ts", depth + 1);
}

void Remangler::mangleGenericSpecializationNotReAbstracted(Node *node,
                                                           unsigned depth) {
  mangleGenericSpecializationNode(node, "TG", depth + 1);
}

void Remangler::mangleGenericSpecializationInResilienceDomain(Node *node,
                                                              unsigned depth) {
  mangleGenericSpecializationNode(node, "TB", depth + 1);
}

void Remangler::mangleInlinedGenericFunction(Node *node, unsigned depth) {
  mangleGenericSpecializationNode(node, "Ti", depth + 1);
}

void Remangler::mangleGenericSpecializationParam(Node *node, unsigned depth) {
  unreachable("handled inline");
}

void Remangler::mangleGenericTypeMetadataPattern(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "MP";
}

void Remangler::mangleGenericTypeParamDecl(Node *node, unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "fp";
}

void Remangler::mangleGetter(Node *node, unsigned depth) {
  mangleAbstractStorage(node->getFirstChild(), "g", depth + 1);
}

void Remangler::mangleGlobal(Node *node, unsigned depth) {
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
      case Node::Kind::ObjCAttribute:
      case Node::Kind::NonObjCAttribute:
      case Node::Kind::DynamicAttribute:
      case Node::Kind::VTableAttribute:
      case Node::Kind::DirectMethodReferenceAttribute:
      case Node::Kind::MergedFunction:
      case Node::Kind::DynamicallyReplaceableFunctionKey:
      case Node::Kind::DynamicallyReplaceableFunctionImpl:
      case Node::Kind::DynamicallyReplaceableFunctionVar:
      case Node::Kind::AsyncFunctionPointer:
      case Node::Kind::AsyncAwaitResumePartialFunction:
      case Node::Kind::AsyncSuspendResumePartialFunction:
        mangleInReverseOrder = true;
        break;
      default:
        mangle(Child, depth + 1);
        if (mangleInReverseOrder) {
          auto ReverseIter = Iter;
          while (ReverseIter != node->begin()) {
            --ReverseIter;
            mangle(*ReverseIter, depth + 1);
          }
          mangleInReverseOrder = false;
        }
        break;
    }
  }
}

void Remangler::mangleGlobalGetter(Node *node, unsigned depth) {
  mangleAbstractStorage(node->getFirstChild(), "G", depth + 1);
}

void Remangler::mangleIdentifier(Node *node, unsigned depth) {
  mangleIdentifierImpl(node, /*isOperator*/ false);
}

void Remangler::mangleIndex(Node *node, unsigned depth) {
  unreachable("handled inline");
}

void Remangler::mangleUnknownIndex(Node *node, unsigned depth) {
  unreachable("handled inline");
}

void Remangler::mangleIVarInitializer(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "fe";
}

void Remangler::mangleIVarDestroyer(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "fE";
}

void Remangler::mangleImplDifferentiabilityKind(Node *node, unsigned depth) {
  Buffer << (char)node->getIndex();
}

void Remangler::mangleImplEscaping(Node *node, unsigned depth) {
  Buffer << 'e';
}

void Remangler::mangleImplConvention(Node *node, unsigned depth) {
  char ConvCh = llvm::StringSwitch<char>(node->getText())
                  .Case("@callee_unowned", 'y')
                  .Case("@callee_guaranteed", 'g')
                  .Case("@callee_owned", 'x')
                  .Default(0);
  assert(ConvCh && "invalid impl callee convention");
  Buffer << ConvCh;
}

void Remangler::mangleImplParameterResultDifferentiability(Node *node,
                                                           unsigned depth) {
  assert(node->hasText());
  // Empty string represents default differentiability.
  if (node->getText().empty())
    return;
  char diffChar = llvm::StringSwitch<char>(node->getText())
                      .Case("@noDerivative", 'w')
                      .Default(0);
  assert(diffChar && "Invalid impl differentiability");
  Buffer << diffChar;
}

void Remangler::mangleImplFunctionAttribute(Node *node, unsigned depth) {
  unreachable("handled inline");
}

void Remangler::mangleImplFunctionConvention(Node *node, unsigned depth) {
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
  assert(FuncAttr && "invalid impl function convention");
  if ((FuncAttr == 'B' || FuncAttr == 'C') && node->getNumChildren() > 1 &&
      node->getChild(1)->getKind() == Node::Kind::ClangType) {
    Buffer << 'z' << FuncAttr;
    mangleClangType(node->getChild(1), depth + 1);
    return;
  }
  Buffer << FuncAttr;
}

void Remangler::mangleImplFunctionConventionName(Node *node, unsigned depth) {
  unreachable("handled inline");
}

void Remangler::mangleClangType(Node *node, unsigned depth) {
  Buffer << node->getText().size() << node->getText();
}

void Remangler::mangleImplInvocationSubstitutions(Node *node, unsigned depth) {
  unreachable("handled inline");
}

void Remangler::mangleImplPatternSubstitutions(Node *node, unsigned depth) {
  unreachable("handled inline");
}

void Remangler::mangleImplFunctionType(Node *node, unsigned depth) {
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
      assert(Child->getNumChildren() == 2 || Child->getNumChildren() == 3);
      mangle(Child->getLastChild(), depth + 1);
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
    mangle(GenSig, depth + 1);
  if (InvocationSubs) {
    Buffer << 'y';
    mangleChildNodes(InvocationSubs->getChild(0), depth + 1);
    if (InvocationSubs->getNumChildren() >= 2)
      mangleRetroactiveConformance(InvocationSubs->getChild(1), depth + 1);
  }
  if (PatternSubs) {
    mangle(PatternSubs->getChild(0), depth + 1);
    Buffer << 'y';
    mangleChildNodes(PatternSubs->getChild(1), depth + 1);
    if (PatternSubs->getNumChildren() >= 3) {
      NodePointer retroactiveConf = PatternSubs->getChild(2);
      if (retroactiveConf->getKind() == Node::Kind::TypeList) {
        mangleChildNodes(retroactiveConf, depth + 1);
      } else {
        mangleRetroactiveConformance(retroactiveConf, depth + 1);
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
        assert(ConvCh && "invalid impl callee convention");
        Buffer << ConvCh;
        break;
      }
      case Node::Kind::ImplFunctionConvention: {
        mangleImplFunctionConvention(Child, depth + 1);
        break;
      }
      case Node::Kind::ImplFunctionAttribute: {
        char FuncAttr = llvm::StringSwitch<char>(Child->getText())
                        .Case("@yield_once", 'A')
                        .Case("@yield_many", 'G')
                        .Case("@Sendable", 'h')
                        .Case("@async", 'H')
                        .Default(0);
        assert(FuncAttr && "invalid impl function attribute");
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
                .Default(0);
        assert(ConvCh && "invalid impl parameter convention");
        Buffer << ConvCh;
        // Mangle parameter differentiability, if it exists.
        if (Child->getNumChildren() == 3)
          mangleImplParameterResultDifferentiability(Child->getChild(1),
                                                     depth + 1);
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
                        .Default(0);
        assert(ConvCh && "invalid impl parameter convention");
        Buffer << ConvCh;
        // Mangle result differentiability, if it exists.
        if (Child->getNumChildren() == 3)
          mangleImplParameterResultDifferentiability(Child->getChild(1),
                                                     depth + 1);
        break;
      }
      default:
        break;
    }
  }
  Buffer << '_';
}

void Remangler::mangleImplicitClosure(Node *node, unsigned depth) {
  mangleChildNode(node, 0, depth + 1); // context
  mangleChildNode(node, 2, depth + 1); // type
  Buffer << "fu";
  mangleChildNode(node, 1, depth + 1); // index
}

void Remangler::mangleImplParameter(Node *node, unsigned depth) {
  unreachable("handled inline");
}

void Remangler::mangleImplResult(Node *node, unsigned depth) {
  unreachable("handled inline");
}

void Remangler::mangleImplYield(Node *node, unsigned depth) {
  unreachable("handled inline");
}

void Remangler::mangleImplErrorResult(Node *node, unsigned depth) {
  unreachable("handled inline");
}

void Remangler::mangleInOut(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << 'z';
}

void Remangler::mangleIsolated(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "Yi";
}

void Remangler::mangleShared(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << 'h';
}

void Remangler::mangleOwned(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << 'n';
}

void Remangler::mangleNoDerivative(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "Yk";
}

void Remangler::mangleInfixOperator(Node *node, unsigned depth) {
  mangleIdentifierImpl(node, /*isOperator*/ true);
  Buffer << "oi";
}

void Remangler::mangleInitializer(Node *node, unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "fi";
}

void Remangler::manglePropertyWrapperBackingInitializer(Node *node,
                                                        unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "fP";
}

void Remangler::manglePropertyWrapperInitFromProjectedValue(Node *node,
                                                            unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "fW";
}

void Remangler::mangleLazyProtocolWitnessTableAccessor(Node *node,
                                                       unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "Wl";
}

void Remangler::mangleLazyProtocolWitnessTableCacheVariable(Node *node,
                                                            unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "WL";
}

void Remangler::mangleLocalDeclName(Node *node, unsigned depth) {
  mangleChildNode(node, 1, depth + 1); // identifier
  Buffer << 'L';
  mangleChildNode(node, 0, depth + 1); // index
}

void Remangler::mangleMaterializeForSet(Node *node, unsigned depth) {
  mangleAbstractStorage(node->getFirstChild(), "m", depth + 1);
}

void Remangler::mangleMetatype(Node *node, unsigned depth) {
  if (node->getFirstChild()->getKind() == Node::Kind::MetatypeRepresentation) {
    mangleChildNode(node, 1, depth + 1);
    Buffer << "XM";
    mangleChildNode(node, 0, depth + 1);
  } else {
    mangleSingleChildNode(node, depth + 1);
    Buffer << 'm';
  }
}

void Remangler::mangleMetatypeRepresentation(Node *node, unsigned depth) {
  if (node->getText() == "@thin") {
    Buffer << 't';
  } else if (node->getText() == "@thick") {
    Buffer << 'T';
  } else if (node->getText() == "@objc_metatype") {
    Buffer << 'o';
  } else {
    unreachable("wrong metatype representation");
  }
}

void Remangler::mangleMetaclass(Node *node, unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "Mm";
}

void Remangler::mangleModifyAccessor(Node *node, unsigned depth) {
  mangleAbstractStorage(node->getFirstChild(), "M", depth + 1);
}

void Remangler::mangleModule(Node *node, unsigned depth) {
  if (node->getText() == STDLIB_NAME) {
    Buffer << 's';
  } else if (node->getText() == MANGLING_MODULE_OBJC) {
    Buffer << "So";
  } else if (node->getText() == MANGLING_MODULE_CLANG_IMPORTER) {
    Buffer << "SC";
  } else {
    mangleIdentifier(node, depth);
  }
}

void Remangler::mangleNativeOwningAddressor(Node *node, unsigned depth) {
  mangleAbstractStorage(node->getFirstChild(), "lo", depth + 1);
}

void Remangler::mangleNativeOwningMutableAddressor(Node *node, unsigned depth) {
  mangleAbstractStorage(node->getFirstChild(), "ao", depth + 1);
}

void Remangler::mangleNativePinningAddressor(Node *node, unsigned depth) {
  mangleAbstractStorage(node->getFirstChild(), "lp", depth + 1);
}

void Remangler::mangleNativePinningMutableAddressor(Node *node,
                                                    unsigned depth) {
  mangleAbstractStorage(node->getFirstChild(), "aP", depth + 1);
}

void Remangler::mangleClassMetadataBaseOffset(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "Mo";
}

void Remangler::mangleNominalTypeDescriptor(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "Mn";
}

void Remangler::mangleOpaqueTypeDescriptor(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "MQ";
}

void Remangler::mangleOpaqueTypeDescriptorAccessor(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "Mg";
}

void Remangler::mangleOpaqueTypeDescriptorAccessorImpl(Node *node,
                                                       unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "Mh";
}

void Remangler::mangleOpaqueTypeDescriptorAccessorKey(Node *node,
                                                      unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "Mj";
}

void Remangler::mangleOpaqueTypeDescriptorAccessorVar(Node *node,
                                                      unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "Mk";
}

void Remangler::manglePropertyDescriptor(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "MV";
}

void Remangler::mangleNonObjCAttribute(Node *node, unsigned depth) {
  Buffer << "TO";
}

void Remangler::mangleTuple(Node *node, unsigned depth) {
  mangleTypeList(node, depth + 1);
  Buffer << 't';
}

void Remangler::mangleNumber(Node *node, unsigned depth) {
  mangleIndex(node->getIndex());
}

void Remangler::mangleObjCAttribute(Node *node, unsigned depth) {
  Buffer << "To";
}

void Remangler::mangleObjCBlock(Node *node, unsigned depth) {
  if (node->getNumChildren() > 0 &&
      node->getFirstChild()->getKind() == Node::Kind::ClangType) {
    for (size_t Idx = node->getNumChildren() - 1; Idx >= 1; --Idx) {
      mangleChildNode(node, Idx, depth + 1);
    }
    Buffer << "XzB";
    mangleClangType(node->getFirstChild(), depth + 1);
    return;
  }
  mangleChildNodesReversed(node, depth + 1);
  Buffer << "XB";
}

void Remangler::mangleEscapingObjCBlock(Node *node, unsigned depth) {
  mangleChildNodesReversed(node, depth + 1);
  Buffer << "XL";
}

void Remangler::mangleOwningAddressor(Node *node, unsigned depth) {
  mangleAbstractStorage(node->getFirstChild(), "lO", depth + 1);
}

void Remangler::mangleOwningMutableAddressor(Node *node, unsigned depth) {
  mangleAbstractStorage(node->getFirstChild(), "aO", depth + 1);
}

void Remangler::manglePartialApplyForwarder(Node *node, unsigned depth) {
  mangleChildNodesReversed(node, depth + 1);
  Buffer << "TA";
}

void Remangler::manglePartialApplyObjCForwarder(Node *node, unsigned depth) {
  mangleChildNodesReversed(node, depth + 1);
  Buffer << "Ta";
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
  Buffer << "TQ";
  mangleChildNode(node, 0, depth + 1);
}

void Remangler::mangleAsyncSuspendResumePartialFunction(Node *node,
                                                        unsigned depth) {
  Buffer << "TY";
  mangleChildNode(node, 0, depth + 1);
}

void Remangler::manglePostfixOperator(Node *node, unsigned depth) {
  mangleIdentifierImpl(node, /*isOperator*/ true);
  Buffer << "oP";
}

void Remangler::manglePrefixOperator(Node *node, unsigned depth) {
  mangleIdentifierImpl(node, /*isOperator*/ true);
  Buffer << "op";
}

void Remangler::manglePrivateDeclName(Node *node, unsigned depth) {
  mangleChildNodesReversed(node, depth + 1);
  Buffer << (node->getNumChildren() == 1 ? "Ll" : "LL");
}

void Remangler::mangleProtocol(Node *node, unsigned depth) {
  mangleAnyGenericType(node, "P", depth + 1);
}

void Remangler::mangleRetroactiveConformance(Node *node, unsigned depth) {
  mangleAnyProtocolConformance(node->getChild(1), depth + 1);
  Buffer << 'g';
  mangleIndex(node->getChild(0)->getIndex());
}

void Remangler::mangleProtocolConformance(Node *node, unsigned depth) {
  Node *Ty = getChildOfType(node->getChild(0));
  Node *GenSig = nullptr;
  if (Ty->getKind() == Node::Kind::DependentGenericType) {
    GenSig = Ty->getFirstChild();
    Ty = Ty->getChild(1);
  }
  mangle(Ty, depth + 1);
  if (node->getNumChildren() == 4)
    mangleChildNode(node, 3, depth + 1);
  manglePureProtocol(node->getChild(1), depth + 1);
  mangleChildNode(node, 2, depth + 1);
  if (GenSig)
    mangle(GenSig, depth + 1);
}

void Remangler::mangleProtocolConformanceRefInTypeModule(Node *node,
                                                         unsigned depth) {
  manglePureProtocol(node->getChild(0), depth + 1);
  Buffer << "HP";
}

void Remangler::mangleProtocolConformanceRefInProtocolModule(Node *node,
                                                             unsigned depth) {
  manglePureProtocol(node->getChild(0), depth + 1);
  Buffer << "Hp";
}

void Remangler::mangleProtocolConformanceRefInOtherModule(Node *node,
                                                          unsigned depth) {
  manglePureProtocol(node->getChild(0), depth + 1);
  mangleChildNode(node, 1, depth + 1);
}

void Remangler::mangleConcreteProtocolConformance(Node *node, unsigned depth) {
  mangleType(node->getChild(0), depth + 1);
  mangle(node->getChild(1), depth + 1);
  if (node->getNumChildren() > 2)
    mangleAnyProtocolConformanceList(node->getChild(2), depth + 1);
  else
    Buffer << "y";
  Buffer << "HC";
}

void Remangler::mangleDependentProtocolConformanceRoot(Node *node,
                                                       unsigned depth) {
  mangleType(node->getChild(0), depth + 1);
  manglePureProtocol(node->getChild(1), depth + 1);
  Buffer << "HD";
  mangleDependentConformanceIndex(node->getChild(2), depth + 1);
}

void Remangler::mangleDependentProtocolConformanceInherited(Node *node,
                                                            unsigned depth) {
  mangleAnyProtocolConformance(node->getChild(0), depth + 1);
  manglePureProtocol(node->getChild(1), depth + 1);
  Buffer << "HI";
  mangleDependentConformanceIndex(node->getChild(2), depth + 1);
}

void Remangler::mangleDependentAssociatedConformance(Node *node,
                                                     unsigned depth) {
  mangleType(node->getChild(0), depth + 1);
  manglePureProtocol(node->getChild(1), depth + 1);
}

void Remangler::mangleDependentProtocolConformanceAssociated(Node *node,
                                                             unsigned depth) {
  mangleAnyProtocolConformance(node->getChild(0), depth + 1);
  mangleDependentAssociatedConformance(node->getChild(1), depth + 1);
  Buffer << "HA";
  mangleDependentConformanceIndex(node->getChild(2), depth + 1);
}

void Remangler::mangleDependentConformanceIndex(Node *node, unsigned depth) {
  assert(node->getKind() == Node::Kind::Index ||
         node->getKind() == Node::Kind::UnknownIndex);
  assert(node->hasIndex() == (node->getKind() == Node::Kind::Index));
  mangleIndex(node->hasIndex() ? node->getIndex() + 2 : 1);
}

void Remangler::mangleAnyProtocolConformance(Node *node, unsigned depth) {
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
    break;
  }
}

void Remangler::mangleAnyProtocolConformanceList(Node *node, unsigned depth) {
  bool firstElem = true;
  for (NodePointer child : *node) {
    mangleAnyProtocolConformance(child, depth + 1);
    mangleListSeparator(firstElem);
  }
  mangleEndOfList(firstElem);
}

void Remangler::mangleProtocolDescriptor(Node *node, unsigned depth) {
  manglePureProtocol(getSingleChild(node), depth + 1);
  Buffer << "Mp";
}

void Remangler::mangleProtocolRequirementsBaseDescriptor(Node *node,
                                                         unsigned depth) {
  manglePureProtocol(getSingleChild(node), depth + 1);
  Buffer << "TL";
}

void Remangler::mangleProtocolSelfConformanceDescriptor(Node *node,
                                                        unsigned depth) {
  manglePureProtocol(node->getChild(0), depth + 1);
  Buffer << "MS";
}

void Remangler::mangleProtocolConformanceDescriptor(Node *node,
                                                    unsigned depth) {
  mangleProtocolConformance(node->getChild(0), depth + 1);
  Buffer << "Mc";
}

void Remangler::mangleProtocolList(Node *node, Node *superclass,
                                   bool hasExplicitAnyObject, unsigned depth) {
  auto *protocols = getSingleChild(node, Node::Kind::TypeList);
  bool FirstElem = true;
  for (NodePointer Child : *protocols) {
    manglePureProtocol(Child, depth + 1);
    mangleListSeparator(FirstElem);
  }
  mangleEndOfList(FirstElem);
  if (superclass) {
    mangleType(superclass, depth + 1);
    Buffer << "Xc";
    return;
  } else if (hasExplicitAnyObject) {
    Buffer << "Xl";
    return;
  }
  Buffer << 'p';
}

void Remangler::mangleProtocolList(Node *node, unsigned depth) {
  mangleProtocolList(node, nullptr, false, depth + 1);
}

void Remangler::mangleProtocolListWithClass(Node *node, unsigned depth) {
  mangleProtocolList(node->getChild(0), node->getChild(1), false, depth + 1);
}

void Remangler::mangleProtocolListWithAnyObject(Node *node, unsigned depth) {
  mangleProtocolList(node->getChild(0), nullptr, true, depth + 1);
}

void Remangler::mangleProtocolSelfConformanceWitness(Node *node,
                                                     unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "TS";
}

void Remangler::mangleProtocolWitness(Node *node, unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "TW";
}

void Remangler::mangleProtocolSelfConformanceWitnessTable(Node *node,
                                                          unsigned depth) {
  manglePureProtocol(node->getChild(0), depth + 1);
  Buffer << "WS";
}

void Remangler::mangleProtocolWitnessTable(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "WP";
}

void Remangler::mangleProtocolWitnessTablePattern(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "Wp";
}

void Remangler::mangleProtocolWitnessTableAccessor(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "Wa";
}

void Remangler::mangleReabstractionThunk(Node *node, unsigned depth) {
  mangleChildNodesReversed(node, depth + 1);
  Buffer << "Tr";
}

void Remangler::mangleReabstractionThunkHelper(Node *node, unsigned depth) {
  mangleChildNodesReversed(node, depth + 1);
  Buffer << "TR";
}

void Remangler::mangleReabstractionThunkHelperWithSelf(Node *node,
                                                       unsigned depth) {
  mangleChildNodesReversed(node, depth + 1);
  Buffer << "Ty";
}

void Remangler::mangleReabstractionThunkHelperWithGlobalActor(Node *node,
                                                              unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "TU";
}

void Remangler::mangleAutoDiffFunctionOrSimpleThunk(Node *node, StringRef op,
                                                    unsigned depth) {
  auto childIt = node->begin();
  while (childIt != node->end() &&
         (*childIt)->getKind() != Node::Kind::AutoDiffFunctionKind)
    mangle(*childIt++, depth + 1);
  Buffer << op;
  mangle(*childIt++, depth + 1); // kind
  mangle(*childIt++, depth + 1); // parameter indices
  Buffer << 'p';
  mangle(*childIt++, depth + 1); // result indices
  Buffer << 'r';
}

void Remangler::mangleAutoDiffFunction(Node *node, unsigned depth) {
  mangleAutoDiffFunctionOrSimpleThunk(node, "TJ", depth + 1);
}

void Remangler::mangleAutoDiffDerivativeVTableThunk(Node *node,
                                                    unsigned depth) {
  mangleAutoDiffFunctionOrSimpleThunk(node, "TJV", depth + 1);
}

void Remangler::mangleAutoDiffSelfReorderingReabstractionThunk(Node *node,
                                                               unsigned depth) {
  auto childIt = node->begin();
  mangle(*childIt++, depth + 1); // from type
  mangle(*childIt++, depth + 1); // to type
  if ((*childIt)->getKind() == Node::Kind::DependentGenericSignature)
    mangleDependentGenericSignature(*childIt++, depth + 1);
  Buffer << "TJO";
  mangle(*childIt++, depth + 1); // kind
}

void Remangler::mangleAutoDiffSubsetParametersThunk(Node *node,
                                                    unsigned depth) {
  auto childIt = node->begin();
  while (childIt != node->end() &&
         (*childIt)->getKind() != Node::Kind::AutoDiffFunctionKind)
    mangle(*childIt++, depth + 1);
  Buffer << "TJS";
  mangle(*childIt++, depth + 1); // kind
  mangle(*childIt++, depth + 1); // parameter indices
  Buffer << 'p';
  mangle(*childIt++, depth + 1); // result indices
  Buffer << 'r';
  mangle(*childIt++, depth + 1); // to parameter indices
  Buffer << 'P';
}

void Remangler::mangleAutoDiffFunctionKind(Node *node, unsigned depth) {
  Buffer << (char)node->getIndex();
}

void Remangler::mangleDifferentiabilityWitness(Node *node, unsigned depth) {
  auto childIt = node->begin();
  while (childIt != node->end() && (*childIt)->getKind() != Node::Kind::Index)
    mangle(*childIt++, depth + 1);
  if (node->getLastChild()->getKind() ==
          Node::Kind::DependentGenericSignature)
    mangle(node->getLastChild(), depth + 1);
  Buffer << "WJ" << (char)(*childIt++)->getIndex();
  mangle(*childIt++, depth + 1); // parameter indices
  Buffer << 'p';
  mangle(*childIt++, depth + 1); // result indices
  Buffer << 'r';
}

void Remangler::mangleIndexSubset(Node *node, unsigned depth) {
  Buffer << node->getText();
}

void Remangler::mangleReadAccessor(Node *node, unsigned depth) {
  mangleAbstractStorage(node->getFirstChild(), "r", depth + 1);
}

void Remangler::mangleKeyPathThunkHelper(Node *node, StringRef op,
                                         unsigned depth) {
  for (NodePointer Child : *node)
    if (Child->getKind() != Node::Kind::IsSerialized)
      mangle(Child, depth + 1);
  Buffer << op;
  for (NodePointer Child : *node)
    if (Child->getKind() == Node::Kind::IsSerialized)
      mangle(Child, depth + 1);
}

void Remangler::mangleKeyPathGetterThunkHelper(Node *node, unsigned depth) {
  mangleKeyPathThunkHelper(node, "TK", depth + 1);
}

void Remangler::mangleKeyPathSetterThunkHelper(Node *node, unsigned depth) {
  mangleKeyPathThunkHelper(node, "Tk", depth + 1);
}

void Remangler::mangleKeyPathEqualsThunkHelper(Node *node, unsigned depth) {
  mangleKeyPathThunkHelper(node, "TH", depth + 1);
}

void Remangler::mangleKeyPathHashThunkHelper(Node *node, unsigned depth) {
  mangleKeyPathThunkHelper(node, "Th", depth + 1);
}

void Remangler::mangleReturnType(Node *node, unsigned depth) {
  mangleArgumentTuple(node, depth + 1);
}

void Remangler::mangleRelatedEntityDeclName(Node *node, unsigned depth) {
  mangleChildNode(node, 1, depth + 1);
  NodePointer kindNode = node->getFirstChild();
  if (kindNode->getText().size() != 1)
    unreachable("cannot handle multi-byte related entities");
  Buffer << "L" << kindNode->getText();
}

void Remangler::mangleSILBoxType(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "Xb";
}

void Remangler::mangleMoveOnlyType(Node *node, unsigned depth) {
  unreachable("Not implemented!");
}

void Remangler::mangleSetter(Node *node, unsigned depth) {
  mangleAbstractStorage(node->getFirstChild(), "s", depth + 1);
}

void Remangler::mangleSpecializationPassID(Node *node, unsigned depth) {
  Buffer << node->getIndex();
}

void Remangler::mangleIsSerialized(Node *node, unsigned depth) {
  Buffer << 'q';
}

void Remangler::mangleStatic(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << 'Z';
}

void Remangler::mangleOtherNominalType(Node *node, unsigned depth) {
  mangleAnyNominalType(node, depth + 1);
}

void Remangler::mangleStructure(Node *node, unsigned depth) {
  mangleAnyNominalType(node, depth + 1);
}

void Remangler::mangleSubscript(Node *node, unsigned depth) {
  mangleAbstractStorage(node, "p", depth + 1);
}

void Remangler::mangleSuffix(Node *node, unsigned depth) {
  // Just add the suffix back on.
  Buffer << node->getText();
}

void Remangler::mangleThinFunctionType(Node *node, unsigned depth) {
  mangleFunctionSignature(node, depth + 1);
  Buffer << "Xf";
}

void Remangler::mangleTupleElement(Node *node, unsigned depth) {
  mangleChildNodesReversed(node, depth + 1); // tuple type, element name?
}

void Remangler::mangleTupleElementName(Node *node, unsigned depth) {
  mangleIdentifier(node, depth + 1);
}

void Remangler::mangleType(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
}

void Remangler::mangleTypeAlias(Node *node, unsigned depth) {
  mangleAnyNominalType(node, depth + 1);
}

void Remangler::mangleTypeList(Node *node, unsigned depth) {
  bool FirstElem = true;
  for (size_t Idx = 0, Num = node->getNumChildren(); Idx < Num; ++Idx) {
    mangleChildNode(node, Idx, depth + 1);
    mangleListSeparator(FirstElem);
  }
  mangleEndOfList(FirstElem);
}

void Remangler::mangleLabelList(Node *node, unsigned depth) {
  if (node->getNumChildren() == 0)
    Buffer << 'y';
  else
    mangleChildNodes(node, depth + 1);
}

void Remangler::mangleTypeMangling(Node *node, unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << 'D';
}

void Remangler::mangleTypeMetadata(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "N";
}

void Remangler::mangleTypeMetadataAccessFunction(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "Ma";
}

void Remangler::mangleTypeMetadataInstantiationCache(Node *node,
                                                     unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "MI";
}

void Remangler::mangleTypeMetadataInstantiationFunction(Node *node,
                                                        unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "Mi";
}

void Remangler::mangleTypeMetadataSingletonInitializationCache(Node *node,
                                                               unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "Ml";
}

void Remangler::mangleTypeMetadataCompletionFunction(Node *node,
                                                     unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "Mr";
}

void Remangler::mangleTypeMetadataDemanglingCache(Node *node, unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "MD";
}

void Remangler::mangleTypeMetadataLazyCache(Node *node, unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "ML";
}

void Remangler::mangleUncurriedFunctionType(Node *node, unsigned depth) {
  mangleFunctionSignature(node, depth + 1);
  // Mangle as regular function type (there is no "uncurried function type"
  // in the new mangling scheme).
  Buffer << 'c';
}

void Remangler::mangleUnsafeAddressor(Node *node, unsigned depth) {
  mangleAbstractStorage(node->getFirstChild(), "lu", depth + 1);
}

void Remangler::mangleUnsafeMutableAddressor(Node *node, unsigned depth) {
  mangleAbstractStorage(node->getFirstChild(), "au", depth + 1);
}

void Remangler::mangleValueWitness(Node *node, unsigned depth) {
  mangleChildNode(node, 1, depth + 1); // type
  const char *Code = nullptr;
  switch (ValueWitnessKind(node->getFirstChild()->getIndex())) {
#define VALUE_WITNESS(MANGLING, NAME) \
    case ValueWitnessKind::NAME: Code = #MANGLING; break;
#include "swift/Demangling/ValueWitnessMangling.def"
  }
  Buffer << 'w' << Code;
}

void Remangler::mangleValueWitnessTable(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "WV";
}

void Remangler::mangleVariable(Node *node, unsigned depth) {
  mangleAbstractStorage(node, "p", depth + 1);
}

void Remangler::mangleVTableAttribute(Node *node, unsigned depth) {
  unreachable("Old-fashioned vtable thunk in new mangling format");
}

void Remangler::mangleVTableThunk(Node *node, unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "TV";
}

#define REF_STORAGE(Name, ...)                                                 \
  void Remangler::mangle##Name(Node *node, unsigned depth) {                   \
    mangleSingleChildNode(node, depth + 1);                                    \
    Buffer << manglingOf(ReferenceOwnership::Name);                            \
  }
#include "swift/AST/ReferenceStorage.def"

void Remangler::mangleWillSet(Node *node, unsigned depth) {
  mangleAbstractStorage(node->getFirstChild(), "w", depth + 1);
}

void Remangler::mangleReflectionMetadataBuiltinDescriptor(Node *node,
                                                          unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "MB";
}

void Remangler::mangleReflectionMetadataFieldDescriptor(Node *node,
                                                        unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "MF";
}

void Remangler::mangleReflectionMetadataAssocTypeDescriptor(Node *node,
                                                            unsigned depth) {
  mangleSingleChildNode(node, depth + 1); // protocol-conformance
  Buffer << "MA";
}

void Remangler::mangleReflectionMetadataSuperclassDescriptor(Node *node,
                                                             unsigned depth) {
  mangleSingleChildNode(node, depth + 1); // protocol-conformance
  Buffer << "MC";
}

void Remangler::mangleCurryThunk(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "Tc";
}

void Remangler::mangleDispatchThunk(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "Tj";
}

void Remangler::mangleMethodDescriptor(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "Tq";
}

void Remangler::mangleMethodLookupFunction(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "Mu";
}

void Remangler::mangleObjCMetadataUpdateFunction(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "MU";
}

void Remangler::mangleObjCResilientClassStub(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "Ms";
}

void Remangler::mangleFullObjCResilientClassStub(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "Mt";
}

void Remangler::mangleConcurrentFunctionType(Node *node, unsigned depth) {
  Buffer << "Yb";
}

void Remangler::mangleAsyncAnnotation(Node *node, unsigned depth) {
  Buffer << "Ya";
}

void Remangler::mangleDifferentiableFunctionType(Node *node, unsigned depth) {
  Buffer << "Yj" << (char)node->getIndex(); // differentiability kind
}

void Remangler::mangleGlobalActorFunctionType(Node *node, unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "Yc";
}

void Remangler::mangleThrowsAnnotation(Node *node, unsigned depth) {
  Buffer << 'K';
}

void Remangler::mangleEmptyList(Node *node, unsigned depth) { Buffer << 'y'; }

void Remangler::mangleFirstElementMarker(Node *node, unsigned depth) {
  Buffer << '_';
}

void Remangler::mangleVariadicMarker(Node *node, unsigned depth) {
  Buffer << 'd';
}

void Remangler::mangleOutlinedCopy(Node *node, unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "WOy";
}

void Remangler::mangleOutlinedConsume(Node *node, unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "WOe";
}

void Remangler::mangleOutlinedRetain(Node *node, unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "WOr";
}

void Remangler::mangleOutlinedRelease(Node *node, unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "WOs";
}

void Remangler::mangleOutlinedInitializeWithTake(Node *node, unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "WOb";
}

void Remangler::mangleOutlinedInitializeWithCopy(Node *node, unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "WOc";
}

void Remangler::mangleOutlinedAssignWithTake(Node *node, unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "WOd";
}

void Remangler::mangleOutlinedAssignWithCopy(Node *node, unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "WOf";
}

void Remangler::mangleOutlinedDestroy(Node *node, unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "WOh";
}

void Remangler::mangleOutlinedVariable(Node *node, unsigned depth) {
  Buffer << "Tv";
  mangleIndex(node->getIndex());
}

void Remangler::mangleOutlinedBridgedMethod(Node *node, unsigned depth) {
  Buffer << "Te";
  Buffer << node->getText();
  Buffer << "_";
}

void Remangler::mangleSILBoxTypeWithLayout(Node *node, unsigned depth) {
  assert(node->getNumChildren() == 1 || node->getNumChildren() == 3);
  assert(node->getChild(0)->getKind() == Node::Kind::SILBoxLayout);
  auto layout = node->getChild(0);
  auto layoutTypeList = Factory.createNode(Node::Kind::TypeList);
  for (unsigned i = 0, e = layout->getNumChildren(); i < e; ++i) {
    assert(layout->getChild(i)->getKind() == Node::Kind::SILBoxImmutableField
           || layout->getChild(i)->getKind() == Node::Kind::SILBoxMutableField);
    auto field = layout->getChild(i);
    assert(field->getNumChildren() == 1
           && field->getChild(0)->getKind() == Node::Kind::Type);
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
  mangleTypeList(layoutTypeList, depth + 1);

  if (node->getNumChildren() == 3) {
    auto signature = node->getChild(1);
    auto genericArgs = node->getChild(2);
    assert(signature->getKind() == Node::Kind::DependentGenericSignature);
    assert(genericArgs->getKind() == Node::Kind::TypeList);
    mangleTypeList(genericArgs, depth + 1);
    mangleDependentGenericSignature(signature, depth + 1);
    Buffer << "XX";
  } else {
    Buffer << "Xx";
  }
}

void Remangler::mangleSILBoxLayout(Node *node, unsigned depth) {
  unreachable("should be part of SILBoxTypeWithLayout");
}

void Remangler::mangleSILBoxMutableField(Node *node, unsigned depth) {
  unreachable("should be part of SILBoxTypeWithLayout");
}

void Remangler::mangleSILBoxImmutableField(Node *node, unsigned depth) {
  unreachable("should be part of SILBoxTypeWithLayout");
}

void Remangler::mangleAssocTypePath(Node *node, unsigned depth) {
  bool FirstElem = true;
  for (NodePointer Child : *node) {
    mangle(Child, depth + 1);
    mangleListSeparator(FirstElem);
  }
}

void Remangler::mangleModuleDescriptor(Node *node, unsigned depth) {
  mangle(node->getChild(0), depth + 1);
  Buffer << "MXM";
}

void Remangler::mangleExtensionDescriptor(Node *node, unsigned depth) {
  mangle(node->getChild(0), depth + 1);
  Buffer << "MXE";
}

void Remangler::mangleAnonymousDescriptor(Node *node, unsigned depth) {
  mangle(node->getChild(0), depth + 1);
  if (node->getNumChildren() == 1) {
    Buffer << "MXX";
  } else {
    mangleIdentifier(node->getChild(1), depth + 1);
    Buffer << "MXY";
  }
}

void Remangler::mangleAssociatedTypeGenericParamRef(Node *node,
                                                    unsigned depth) {
  mangleType(node->getChild(0), depth + 1);
  mangleAssocTypePath(node->getChild(1), depth + 1);
  Buffer << "MXA";
}

void Remangler::mangleTypeSymbolicReference(Node *node, unsigned depth) {
  return mangle(
      Resolver(SymbolicReferenceKind::Context, (const void *)node->getIndex()),
      depth + 1);
}

void Remangler::mangleProtocolSymbolicReference(Node *node, unsigned depth) {
  return mangle(
      Resolver(SymbolicReferenceKind::Context, (const void *)node->getIndex()),
      depth + 1);
}

void Remangler::mangleOpaqueTypeDescriptorSymbolicReference(Node *node,
                                                            unsigned depth) {
  return mangle(
      Resolver(SymbolicReferenceKind::Context, (const void *)node->getIndex()),
      depth + 1);
}

void Remangler::mangleSugaredOptional(Node *node, unsigned depth) {
  mangleType(node->getChild(0), depth + 1);
  Buffer << "XSq";
}

void Remangler::mangleSugaredArray(Node *node, unsigned depth) {
  mangleType(node->getChild(0), depth + 1);
  Buffer << "XSa";
}

void Remangler::mangleSugaredDictionary(Node *node, unsigned depth) {
  mangleType(node->getChild(0), depth + 1);
  mangleType(node->getChild(1), depth + 1);
  Buffer << "XSD";
}

void Remangler::mangleSugaredParen(Node *node, unsigned depth) {
  mangleType(node->getChild(0), depth + 1);
  Buffer << "XSp";
}

void Remangler::mangleOpaqueReturnType(Node *node, unsigned depth) {
  Buffer << "Qr";
}
void Remangler::mangleOpaqueReturnTypeOf(Node *node, unsigned depth) {
  mangle(node->getChild(0), depth + 1);
  Buffer << "QO";
}
void Remangler::mangleOpaqueType(Node *node, unsigned depth) {
  SubstitutionEntry entry;
  if (trySubstitution(node, entry)) return;

  mangle(node->getChild(0), depth + 1);
  auto boundGenerics = node->getChild(2);
  for (unsigned i = 0; i < boundGenerics->getNumChildren(); ++i) {
    Buffer << (i == 0 ? 'y' : '_');
    mangleChildNodes(boundGenerics->getChild(i), depth + 1);
  }
  if (node->getNumChildren() >= 4) {
    auto retroactiveConformances = node->getChild(3);
    for (unsigned i = 0; i < retroactiveConformances->getNumChildren(); ++i) {
      mangle(retroactiveConformances->getChild(i), depth + 1);
    }
  }
  Buffer << "Qo";
  mangleIndex(node->getChild(1)->getIndex());

  addSubstitution(entry);
}
void Remangler::mangleAccessorFunctionReference(Node *node, unsigned depth) {
  unreachable("can't remangle");
}

void Remangler::mangleCanonicalSpecializedGenericMetaclass(Node *node,
                                                           unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "MM";
}

void Remangler::mangleCanonicalSpecializedGenericTypeMetadataAccessFunction(
    Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "Mb";
}

void Remangler::mangleMetadataInstantiationCache(Node *node, unsigned depth) {
  mangleSingleChildNode(node, depth + 1);
  Buffer << "MK";
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

void Remangler::mangleGlobalVariableOnceToken(Node *node, unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "Wz";
}

void Remangler::mangleGlobalVariableOnceFunction(Node *node, unsigned depth) {
  mangleChildNodes(node, depth + 1);
  Buffer << "WZ";
}

void Remangler::mangleGlobalVariableOnceDeclList(Node *node, unsigned depth) {
  for (unsigned i = 0, e = node->getNumChildren(); i < e; ++i) {
    mangle(node->getChild(i), depth + 1);
    Buffer << '_';
  }
}

} // anonymous namespace

/// The top-level interface to the remangler.
std::string Demangle::mangleNode(NodePointer node) {
  return mangleNode(node, [](SymbolicReferenceKind, const void *) -> NodePointer {
    unreachable("should not try to mangle a symbolic reference; "
                "resolve it to a non-symbolic demangling tree instead");
  });
}

std::string
Demangle::mangleNode(NodePointer node, SymbolicResolver resolver) {
  if (!node) return "";

  NodeFactory Factory;
  Remangler remangler(resolver, Factory);
  remangler.mangle(node, 0);

  return remangler.str();
}

llvm::StringRef
Demangle::mangleNode(NodePointer node, SymbolicResolver resolver,
                     NodeFactory &Factory) {
  if (!node)
    return StringRef();

  Remangler remangler(resolver, Factory);
  remangler.mangle(node, 0);

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
    case Node::Kind::Getter:
    case Node::Kind::Setter:
    case Node::Kind::WillSet:
    case Node::Kind::DidSet:
    case Node::Kind::ReadAccessor:
    case Node::Kind::ModifyAccessor:
    case Node::Kind::UnsafeAddressor:
    case Node::Kind::UnsafeMutableAddressor:
      assert(node->getNumChildren() > 0);
      return node->getNumChildren() > 0 && isSpecialized(node->getChild(0));

    case Node::Kind::Extension:
      assert(node->getNumChildren() > 1);
      return node->getNumChildren() > 1 && isSpecialized(node->getChild(1));

    default:
      return false;
  }
}

NodePointer Demangle::getUnspecialized(Node *node, NodeFactory &Factory) {
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
      NumToCopy = node->getNumChildren();
      LLVM_FALLTHROUGH;
    case Node::Kind::Structure:
    case Node::Kind::Enum:
    case Node::Kind::Class:
    case Node::Kind::TypeAlias:
    case Node::Kind::OtherNominalType: {
      NodePointer result = Factory.createNode(node->getKind());
      NodePointer parentOrModule = node->getChild(0);
      if (isSpecialized(parentOrModule))
        parentOrModule = getUnspecialized(parentOrModule, Factory);
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
      assert(unboundType->getKind() == Node::Kind::Type);
      NodePointer nominalType = unboundType->getChild(0);
      if (isSpecialized(nominalType))
        return getUnspecialized(nominalType, Factory);
      return nominalType;
    }

    case Node::Kind::BoundGenericFunction: {
      NodePointer unboundFunction = node->getChild(0);
      assert(unboundFunction->getKind() == Node::Kind::Function ||
             unboundFunction->getKind() == Node::Kind::Constructor);
      if (isSpecialized(unboundFunction))
        return getUnspecialized(unboundFunction, Factory);
      return unboundFunction;
    }

    case Node::Kind::Extension: {
      NodePointer parent = node->getChild(1);
      if (!isSpecialized(parent))
        return node;
      NodePointer result = Factory.createNode(Node::Kind::Extension);
      result->addChild(node->getFirstChild(), Factory);
      result->addChild(getUnspecialized(parent, Factory), Factory);
      if (node->getNumChildren() == 3) {
        // Add the generic signature of the extension.
        result->addChild(node->getChild(2), Factory);
      }
      return result;
    }
    default:
      unreachable("bad nominal type kind");
  }
}
