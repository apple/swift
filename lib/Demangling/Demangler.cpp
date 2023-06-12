//===--- Demangler.cpp - String to Node-Tree Demangling -------------------===//
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
//  This file implements new Swift de-mangler.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/Compiler.h"
#include "swift/Demangling/Demangler.h"
#include "DemanglerAssert.h"
#include "swift/Demangling/ManglingMacros.h"
#include "swift/Demangling/ManglingUtils.h"
#include "swift/Demangling/Punycode.h"
#include "swift/Strings.h"
#include <stdio.h>
#include <stdlib.h>

using namespace swift;
using namespace Mangle;
using swift::Demangle::FunctionSigSpecializationParamKind;

//////////////////////////////////
// Private utility functions    //
//////////////////////////////////

namespace {

static bool isDeclName(Node::Kind kind) {
  switch (kind) {
    case Node::Kind::Identifier:
    case Node::Kind::LocalDeclName:
    case Node::Kind::PrivateDeclName:
    case Node::Kind::RelatedEntityDeclName:
    case Node::Kind::PrefixOperator:
    case Node::Kind::PostfixOperator:
    case Node::Kind::InfixOperator:
    case Node::Kind::TypeSymbolicReference:
    case Node::Kind::ProtocolSymbolicReference:
      return true;
    default:
      return false;
  }
}

static bool isAnyGeneric(Node::Kind kind) {
  switch (kind) {
    case Node::Kind::Structure:
    case Node::Kind::Class:
    case Node::Kind::Enum:
    case Node::Kind::Protocol:
    case Node::Kind::ProtocolSymbolicReference:
    case Node::Kind::OtherNominalType:
    case Node::Kind::TypeAlias:
    case Node::Kind::TypeSymbolicReference:
      return true;
    default:
      return false;
  }
}

static bool isEntity(Node::Kind kind) {
  // Also accepts some kind which are not entities.
  if (kind == Node::Kind::Type)
    return true;
  return isContext(kind);
}

static bool isRequirement(Node::Kind kind) {
  switch (kind) {
    case Node::Kind::DependentGenericParamPackMarker:
    case Node::Kind::DependentGenericSameTypeRequirement:
    case Node::Kind::DependentGenericSameShapeRequirement:
    case Node::Kind::DependentGenericLayoutRequirement:
    case Node::Kind::DependentGenericConformanceRequirement:
      return true;
    default:
      return false;
  }
}

} // anonymous namespace

//////////////////////////////////
// Public utility functions    //
//////////////////////////////////

void swift::Demangle::failAssert(const char *file, unsigned line,
                                 NodePointer node, const char *expr) {
  std::string treeStr = getNodeTreeAsString(node);

  fatal(0,
        "%s:%u: assertion failed for Node %p: %s\n"
        "%s:%u: Node %p is:\n%s\n",
        file, line, node, expr, file, line, node, treeStr.c_str());
}

bool swift::Demangle::isContext(Node::Kind kind) {
  switch (kind) {
#define NODE(ID)
#define CONTEXT_NODE(ID)                                                \
    case Node::Kind::ID:
#include "swift/Demangling/DemangleNodes.def"
      return true;
    default:
      return false;
  }
}

bool swift::Demangle::isFunctionAttr(Node::Kind kind) {
  switch (kind) {
    case Node::Kind::FunctionSignatureSpecialization:
    case Node::Kind::GenericSpecialization:
    case Node::Kind::GenericSpecializationPrespecialized:
    case Node::Kind::InlinedGenericFunction:
    case Node::Kind::GenericSpecializationNotReAbstracted:
    case Node::Kind::GenericPartialSpecialization:
    case Node::Kind::GenericPartialSpecializationNotReAbstracted:
    case Node::Kind::GenericSpecializationInResilienceDomain:
    case Node::Kind::ObjCAttribute:
    case Node::Kind::NonObjCAttribute:
    case Node::Kind::DynamicAttribute:
    case Node::Kind::DirectMethodReferenceAttribute:
    case Node::Kind::VTableAttribute:
    case Node::Kind::PartialApplyForwarder:
    case Node::Kind::PartialApplyObjCForwarder:
    case Node::Kind::OutlinedVariable:
    case Node::Kind::OutlinedReadOnlyObject:
    case Node::Kind::OutlinedBridgedMethod:
    case Node::Kind::MergedFunction:
    case Node::Kind::DistributedThunk:
    case Node::Kind::DistributedAccessor:
    case Node::Kind::DynamicallyReplaceableFunctionImpl:
    case Node::Kind::DynamicallyReplaceableFunctionKey:
    case Node::Kind::DynamicallyReplaceableFunctionVar:
    case Node::Kind::AsyncFunctionPointer:
    case Node::Kind::AsyncAwaitResumePartialFunction:
    case Node::Kind::AsyncSuspendResumePartialFunction:
    case Node::Kind::AccessibleFunctionRecord:
    case Node::Kind::BackDeploymentThunk:
    case Node::Kind::BackDeploymentFallback:
    case Node::Kind::HasSymbolQuery:
    case Node::Kind::RuntimeDiscoverableAttributeRecord:
      return true;
    default:
      return false;
  }
}

llvm::StringRef
swift::Demangle::makeSymbolicMangledNameStringRef(const char *base) {
  if (!base)
    return {};

  auto end = base;
  while (*end != '\0') {
    // Skip over symbolic references.
    if (*end >= '\x01' && *end <= '\x17')
      end += sizeof(uint32_t);
    else if (*end >= '\x18' && *end <= '\x1F')
      end += sizeof(void*);
    ++end;
  }
  return StringRef(base, end - base);
}

int swift::Demangle::getManglingPrefixLength(llvm::StringRef mangledName) {
  if (mangledName.empty()) return 0;

  llvm::StringRef prefixes[] = {
    /*Swift 4*/   "_T0",
    /*Swift 4.x*/ "$S", "_$S",
    /*Swift 5+*/  "$s", "_$s",
    /*Swift 5+ for filenames*/ "@__swiftmacro_",
  };

  // Look for any of the known prefixes
  for (auto prefix : prefixes) {
    if (mangledName.startswith(prefix))
      return prefix.size();
  }

  return 0;
}

bool swift::Demangle::isSwiftSymbol(llvm::StringRef mangledName) {
  if (isOldFunctionTypeMangling(mangledName))
    return true;

  return getManglingPrefixLength(mangledName) != 0;
}

bool swift::Demangle::isSwiftSymbol(const char *mangledName) {
  StringRef mangledNameRef(mangledName);
  return isSwiftSymbol(mangledNameRef);
}

bool swift::Demangle::isObjCSymbol(llvm::StringRef mangledName) {
  StringRef nameWithoutPrefix = dropSwiftManglingPrefix(mangledName);
  return nameWithoutPrefix.startswith("So") ||
         nameWithoutPrefix.startswith("SC");
}

bool swift::Demangle::isOldFunctionTypeMangling(llvm::StringRef mangledName) {
  return mangledName.startswith("_T");
}

llvm::StringRef swift::Demangle::dropSwiftManglingPrefix(StringRef mangledName){
  return mangledName.drop_front(getManglingPrefixLength(mangledName));
}

static bool isAliasNode(Demangle::NodePointer Node) {
  switch (Node->getKind()) {
  case Demangle::Node::Kind::Type:
    return isAliasNode(Node->getChild(0));
  case Demangle::Node::Kind::TypeAlias:
    return true;
  default:
    return false;
  }
  assert(0 && "unknown node kind");
}

bool swift::Demangle::isAlias(llvm::StringRef mangledName) {
  Demangle::Demangler Dem;
  return isAliasNode(Dem.demangleType(mangledName));
}

static bool isClassNode(Demangle::NodePointer Node) {
  switch (Node->getKind()) {
  case Demangle::Node::Kind::Type:
    return isClassNode(Node->getChild(0));
  case Demangle::Node::Kind::Class:
  case Demangle::Node::Kind::BoundGenericClass:
    return true;
  default:
    return false;
  }
  assert(0 && "unknown node kind");
}

bool swift::Demangle::isClass(llvm::StringRef mangledName) {
  Demangle::Demangler Dem;
  return isClassNode(Dem.demangleType(mangledName));
}

static bool isEnumNode(Demangle::NodePointer Node) {
  switch (Node->getKind()) {
  case Demangle::Node::Kind::Type:
    return isEnumNode(Node->getChild(0));
  case Demangle::Node::Kind::Enum:
  case Demangle::Node::Kind::BoundGenericEnum:
    return true;
  default:
    return false;
  }
  assert(0 && "unknown node kind");
}

bool swift::Demangle::isEnum(llvm::StringRef mangledName) {
  Demangle::Demangler Dem;
  return isEnumNode(Dem.demangleType(mangledName));
}

static bool isProtocolNode(Demangle::NodePointer Node) {
  switch (Node->getKind()) {
  case Demangle::Node::Kind::Type:
    return isProtocolNode(Node->getChild(0));
  case Demangle::Node::Kind::Protocol:
  case Demangle::Node::Kind::ProtocolSymbolicReference:
    return true;
  default:
    return false;
  }
  assert(0 && "unknown node kind");
}

bool swift::Demangle::isProtocol(llvm::StringRef mangledName) {
  Demangle::Demangler Dem;
  return isProtocolNode(Dem.demangleType(dropSwiftManglingPrefix(mangledName)));
}

static bool isStructNode(Demangle::NodePointer Node) {
  switch (Node->getKind()) {
  case Demangle::Node::Kind::Type:
    return isStructNode(Node->getChild(0));
  case Demangle::Node::Kind::Structure:
  case Demangle::Node::Kind::BoundGenericStructure:
    return true;
  default:
    return false;
  }
  assert(0 && "unknown node kind");
}

bool swift::Demangle::isStruct(llvm::StringRef mangledName) {
  Demangle::Demangler Dem;
  return isStructNode(Dem.demangleType(mangledName));
}

std::string swift::Demangle::mangledNameForTypeMetadataAccessor(
    StringRef moduleName, StringRef typeName, Node::Kind typeKind) {
  using namespace Demangle;

  //  kind=Global
  //    kind=NominalTypeDescriptor
  //      kind=Type
  //        kind=Structure|Enum|Class
  //          kind=Module, text=moduleName
  //          kind=Identifier, text=typeName
  Demangle::Demangler D;
  auto *global = D.createNode(Node::Kind::Global);
  {
    auto *nominalDescriptor =
        D.createNode(Node::Kind::TypeMetadataAccessFunction);
    {
      auto *type = D.createNode(Node::Kind::Type);
      {
        auto *module = D.createNode(Node::Kind::Module, moduleName);
        auto *identifier = D.createNode(Node::Kind::Identifier, typeName);
        auto *structNode = D.createNode(typeKind);
        structNode->addChild(module, D);
        structNode->addChild(identifier, D);
        type->addChild(structNode, D);
      }
      nominalDescriptor->addChild(type, D);
    }
    global->addChild(nominalDescriptor, D);
  }

  auto mangleResult = mangleNode(global);
  assert(mangleResult.isSuccess());
  return mangleResult.result();
}

using namespace swift;
using namespace Demangle;

//////////////////////////////////
// Node member functions        //
//////////////////////////////////

size_t Node::getNumChildren() const {
  switch (NodePayloadKind) {
    case PayloadKind::OneChild: return 1;
    case PayloadKind::TwoChildren: return 2;
    case PayloadKind::ManyChildren: return Children.Number;
    default: return 0;
  }
}

Node::iterator Node::begin() const {
  switch (NodePayloadKind) {
    case PayloadKind::OneChild:
    case PayloadKind::TwoChildren:
      return &InlineChildren[0];
    case PayloadKind::ManyChildren:
      return Children.Nodes;
    default:
      return nullptr;
  }
}

Node::iterator Node::end() const {
  switch (NodePayloadKind) {
    case PayloadKind::OneChild:
      return &InlineChildren[1];
    case PayloadKind::TwoChildren:
      return &InlineChildren[2];
    case PayloadKind::ManyChildren:
      return Children.Nodes + Children.Number;
    default:
      return nullptr;
  }
}

void Node::addChild(NodePointer Child, NodeFactory &Factory) {
  DEMANGLER_ALWAYS_ASSERT(Child, this);
  switch (NodePayloadKind) {
    case PayloadKind::None:
      InlineChildren[0] = Child;
      InlineChildren[1] = nullptr;
      NodePayloadKind = PayloadKind::OneChild;
      break;
    case PayloadKind::OneChild:
      assert(!InlineChildren[1]);
      InlineChildren[1] = Child;
      NodePayloadKind = PayloadKind::TwoChildren;
      break;
    case PayloadKind::TwoChildren: {
      NodePointer Child0 = InlineChildren[0];
      NodePointer Child1 = InlineChildren[1];
      Children.Nodes = nullptr;
      Children.Number = 0;
      Children.Capacity = 0;
      Factory.Reallocate(Children.Nodes, Children.Capacity, 3);
      assert(Children.Capacity >= 3);
      Children.Nodes[0] = Child0;
      Children.Nodes[1] = Child1;
      Children.Nodes[2] = Child;
      Children.Number = 3;
      NodePayloadKind = PayloadKind::ManyChildren;
      break;
    }
    case PayloadKind::ManyChildren:
      if (Children.Number >= Children.Capacity) {
        Factory.Reallocate(Children.Nodes, Children.Capacity, 1);
      }
      assert(Children.Number < Children.Capacity);
      Children.Nodes[Children.Number++] = Child;
      break;
    default:
      assert(false && "cannot add child");
  }
}

void Node::removeChildAt(unsigned Pos) {
  switch (NodePayloadKind) {
    case PayloadKind::OneChild:
      assert(Pos == 0);
      NodePayloadKind = PayloadKind::None;
      break;
    case PayloadKind::TwoChildren: {
      assert(Pos < 2);
      if (Pos == 0)
        InlineChildren[0] = InlineChildren[1];
      NodePayloadKind = PayloadKind::OneChild;
      break;
    }
    case PayloadKind::ManyChildren:
      for (unsigned i = Pos, n = Children.Number - 1; i != n; ++i) {
        Children.Nodes[i] = Children.Nodes[i + 1];
      }
      --Children.Number;
      break;
    default:
      assert(false && "cannot remove child");
  }
}

void Node::reverseChildren(size_t StartingAt) {
  assert(StartingAt <= getNumChildren());
  switch (NodePayloadKind) {
    case PayloadKind::TwoChildren:
      if (StartingAt == 0)
        std::swap(InlineChildren[0], InlineChildren[1]);
      break;
    case PayloadKind::ManyChildren:
      std::reverse(Children.Nodes + StartingAt,
                   Children.Nodes + Children.Number);
      break;
    default:
      break;
  }
}

Node* Node::findByKind(Node::Kind kind, int maxDepth) {
  if (getKind() == kind)
    return this;

  if (maxDepth <= 0)
    return nullptr;

  for (auto node : *this)
    if (auto matchingChild = node->findByKind(kind, maxDepth - 1))
      return matchingChild;

  return nullptr;
}

//////////////////////////////////
// NodeFactory member functions //
//////////////////////////////////

void NodeFactory::freeSlabs(Slab *slab) {
  while (slab) {
    Slab *prev = slab->Previous;
    free(slab);
    slab = prev;
  }
}
  
void NodeFactory::clear() {
  assert(!isBorrowed);
  if (CurrentSlab) {
#ifdef NODE_FACTORY_DEBUGGING
    fprintf(stderr, "%s## clear: allocated memory = %zu\n", indent().c_str(), allocatedMemory);
#endif

    freeSlabs(CurrentSlab->Previous);
    
    // Recycle the last allocated slab.
    // Note that the size of the last slab is at least as big as all previous
    // slabs combined. Therefore it's not worth the effort of reusing all slabs.
    // The slab size also stays the same. So at some point the demangling
    // process converges to a single large slab across repeated demangle-clear
    // cycles.
    CurrentSlab->Previous = nullptr;
    CurPtr = (char *)(CurrentSlab + 1);
#ifdef NODE_FACTORY_DEBUGGING
    allocatedMemory = 0;
#endif
  }
}

NodeFactory::Checkpoint NodeFactory::pushCheckpoint() const {
  return {CurrentSlab, CurPtr, End};
}

void NodeFactory::popCheckpoint(NodeFactory::Checkpoint checkpoint) {
  if (checkpoint.Slab == CurrentSlab) {
    if (checkpoint.CurPtr > CurPtr) {
      fatal(0,
            "Popping checkpoint {%p, %p, %p} that is after the current "
            "pointer.\n",
            checkpoint.Slab, checkpoint.CurPtr, checkpoint.End);
    }
    if (checkpoint.End != End) {
      fatal(0,
            "Popping checkpoint {%p, %p, %p} with End that does not match "
            "current End %p.\n",
            checkpoint.Slab, checkpoint.CurPtr, checkpoint.End, End);
    }
#ifndef NDEBUG
    // Scribble the newly freed area.
    memset(checkpoint.CurPtr, 0xaa, CurPtr - checkpoint.CurPtr);
#endif
    CurPtr = checkpoint.CurPtr;
  } else {
    // We may want to keep using the current slab rather than destroying
    // it, since over time this allows us to converge on a steady state
    // with no allocation activity (see the comment above in
    // NodeFactory::clear). Keep using the current slab if the free space in the
    // checkpoint's slab is less than 1/16th of the current slab's space. We
    // won't repeatedly allocate and deallocate the current slab. The size
    // doubles each time so we'll quickly pass the threshold.
    Slab *savedSlab = nullptr;
    if (CurrentSlab) {
      size_t checkpointSlabFreeSpace = checkpoint.End - checkpoint.CurPtr;
      size_t currentSlabSize = End - (char *)(CurrentSlab + 1);
      if (currentSlabSize / 16 > checkpointSlabFreeSpace) {
        savedSlab = CurrentSlab;
        CurrentSlab = CurrentSlab->Previous;
        // No need to save End, as it will still be in place later.
      }
    }

    // Free all slabs (possibly except the one we saved) until we find the end
    // or we find the checkpoint.
    while (CurrentSlab && checkpoint.Slab != CurrentSlab) {
      auto Slab = CurrentSlab;
      CurrentSlab = CurrentSlab->Previous;
      free(Slab);
    }

    // If we ran to the end and the checkpoint actually has a slab pointer, then
    // the checkpoint is invalid.
    if (!CurrentSlab && checkpoint.Slab) {
      fatal(0,
            "Popping checkpoint {%p, %p, %p} with slab that is not within "
            "the allocator's slab chain.\n",
            checkpoint.Slab, checkpoint.CurPtr, checkpoint.End);
    }

    if (savedSlab) {
      // Reinstall the saved slab.
      savedSlab->Previous = CurrentSlab;
      CurrentSlab = savedSlab;
      CurPtr = (char *)(CurrentSlab + 1);
      // End is still valid from before.
    } else {
      // Set CurPtr and End to match the checkpoint's position.
      CurPtr = checkpoint.CurPtr;
      End = checkpoint.End;
    }

#ifndef NDEBUG
    // Scribble the now freed end of the slab.
    if (CurPtr)
      memset(CurPtr, 0xaa, End - CurPtr);
#endif
  }
}

NodePointer NodeFactory::createNode(Node::Kind K) {
  return new (Allocate<Node>()) Node(K);
}
NodePointer NodeFactory::createNode(Node::Kind K, Node::IndexType Index) {
  return new (Allocate<Node>()) Node(K, Index);
}
NodePointer NodeFactory::createNodeWithAllocatedText(Node::Kind K,
                                                     llvm::StringRef Text) {
  return new (Allocate<Node>()) Node(K, Text);
}
NodePointer NodeFactory::createNode(Node::Kind K, const CharVector &Text) {
  return createNodeWithAllocatedText(K, Text.str());
}
NodePointer NodeFactory::createNode(Node::Kind K, const char *Text) {
  return new (Allocate<Node>()) Node(K, llvm::StringRef(Text));
}

#ifdef NODE_FACTORY_DEBUGGING
int NodeFactory::nestingLevel = 0;
#endif

//////////////////////////////////
// CharVector member functions  //
//////////////////////////////////

void CharVector::append(StringRef Rhs, NodeFactory &Factory) {
  if (NumElems + Rhs.size() > Capacity)
    Factory.Reallocate(Elems, Capacity, /*Growth*/ Rhs.size());
  memcpy(Elems + NumElems, Rhs.data(), Rhs.size());
  NumElems += Rhs.size();
  assert(NumElems <= Capacity);
}

void CharVector::append(int Number, NodeFactory &Factory) {
  const int MaxIntPrintSize = 11;
  if (NumElems + MaxIntPrintSize > Capacity)
    Factory.Reallocate(Elems, Capacity, /*Growth*/ MaxIntPrintSize);
  int Length = snprintf(Elems + NumElems, MaxIntPrintSize, "%d", Number);
  assert(Length > 0 && Length < MaxIntPrintSize);
  NumElems += Length;
}

void CharVector::append(unsigned long long Number, NodeFactory &Factory) {
  const int MaxPrintSize = 21;
  if (NumElems + MaxPrintSize > Capacity)
    Factory.Reallocate(Elems, Capacity, /*Growth*/ MaxPrintSize);
  int Length = snprintf(Elems + NumElems, MaxPrintSize, "%llu", Number);
  assert(Length > 0 && Length < MaxPrintSize);
  NumElems += Length;
}

//////////////////////////////////
// Demangler member functions   //
//////////////////////////////////

void Demangler::clear() {
  NodeStack.free();
  Substitutions.free();
  NodeFactory::clear();
}

Demangler::DemangleInitRAII::DemangleInitRAII(Demangler &Dem,
        StringRef MangledName,
        std::function<SymbolicReferenceResolver_t> TheSymbolicReferenceResolver)
// Save the current demangler state so we can restore it.
  : Dem(Dem),
    NodeStack(Dem.NodeStack), Substitutions(Dem.Substitutions),
    NumWords(Dem.NumWords), Text(Dem.Text), Pos(Dem.Pos),
    SymbolicReferenceResolver(std::move(Dem.SymbolicReferenceResolver))
{
  // Reset the demangler state for a nested job.
  Dem.NodeStack.init(Dem, 16);
  Dem.Substitutions.init(Dem, 16);
  Dem.NumWords = 0;
  Dem.Text = MangledName;
  Dem.Pos = 0;
  Dem.SymbolicReferenceResolver = std::move(TheSymbolicReferenceResolver);
}

Demangler::DemangleInitRAII::~DemangleInitRAII() {
  // Restore the saved state.
  Dem.NodeStack = NodeStack;
  Dem.Substitutions = Substitutions;
  Dem.NumWords = NumWords;
  Dem.Text = Text;
  Dem.Pos = Pos;
  Dem.SymbolicReferenceResolver = std::move(SymbolicReferenceResolver);
}

NodePointer Demangler::demangleSymbol(StringRef MangledName,
        std::function<SymbolicReferenceResolver_t> Resolver) {
  DemangleInitRAII state(*this, MangledName, std::move(Resolver));

#if SWIFT_SUPPORT_OLD_MANGLING
  // Demangle old-style class and protocol names, which are still used in the
  // ObjC metadata.
  if (nextIf("_Tt"))
    return demangleOldSymbolAsNode(Text, *this);
#endif

  unsigned PrefixLength = getManglingPrefixLength(MangledName);
  if (PrefixLength == 0)
    return nullptr;

  IsOldFunctionTypeMangling = isOldFunctionTypeMangling(MangledName);
  Pos += PrefixLength;

  // If any other prefixes are accepted, please update Mangler::verify.

  if (!parseAndPushNodes())
    return nullptr;

  NodePointer topLevel = createNode(Node::Kind::Global);

  NodePointer Parent = topLevel;
  while (NodePointer FuncAttr = popNode(isFunctionAttr)) {
    Parent->addChild(FuncAttr, *this);
    if (FuncAttr->getKind() == Node::Kind::PartialApplyForwarder ||
        FuncAttr->getKind() == Node::Kind::PartialApplyObjCForwarder)
      Parent = FuncAttr;
  }
  for (Node *Nd : NodeStack) {
    switch (Nd->getKind()) {
      case Node::Kind::Type:
        Parent->addChild(Nd->getFirstChild(), *this);
        break;
      default:
        Parent->addChild(Nd, *this);
        break;
    }
  }
  if (topLevel->getNumChildren() == 0)
    return nullptr;

  return topLevel;
}

NodePointer Demangler::demangleType(StringRef MangledName,
        std::function<SymbolicReferenceResolver_t> Resolver) {
  DemangleInitRAII state(*this, MangledName, std::move(Resolver));

  parseAndPushNodes();

  if (NodePointer Result = popNode())
    return Result;

  return createNode(Node::Kind::Suffix, Text);
}

bool Demangler::parseAndPushNodes() {
  const auto textSize = Text.size();
  while (Pos < textSize) {
    NodePointer Node = demangleOperator();
    if (!Node)
      return false;
    pushNode(Node);
  }
  return true;
}

NodePointer Demangler::addChild(NodePointer Parent, NodePointer Child) {
  if (!Parent || !Child)
    return nullptr;
  Parent->addChild(Child, *this);
  return Parent;
}

NodePointer Demangler::createWithChild(Node::Kind kind,
                                            NodePointer Child) {
  if (!Child)
    return nullptr;
  NodePointer Nd = createNode(kind);
  Nd->addChild(Child, *this);
  return Nd;
}

NodePointer Demangler::createType(NodePointer Child) {
  return createWithChild(Node::Kind::Type, Child);
}

NodePointer Demangler::Demangler::createWithChildren(Node::Kind kind,
                                       NodePointer Child1, NodePointer Child2) {
  if (!Child1 || !Child2)
    return nullptr;
  NodePointer Nd = createNode(kind);
  Nd->addChild(Child1, *this);
  Nd->addChild(Child2, *this);
  return Nd;
}

NodePointer Demangler::createWithChildren(Node::Kind kind,
                                               NodePointer Child1,
                                               NodePointer Child2,
                                               NodePointer Child3) {
  if (!Child1 || !Child2 || !Child3)
    return nullptr;
  NodePointer Nd = createNode(kind);
  Nd->addChild(Child1, *this);
  Nd->addChild(Child2, *this);
  Nd->addChild(Child3, *this);
  return Nd;
}

NodePointer Demangler::createWithChildren(Node::Kind kind, NodePointer Child1,
                                          NodePointer Child2,
                                          NodePointer Child3,
                                          NodePointer Child4) {
  if (!Child1 || !Child2 || !Child3 || !Child4)
    return nullptr;
  NodePointer Nd = createNode(kind);
  Nd->addChild(Child1, *this);
  Nd->addChild(Child2, *this);
  Nd->addChild(Child3, *this);
  Nd->addChild(Child4, *this);
  return Nd;
}

NodePointer Demangler::changeKind(NodePointer Node, Node::Kind NewKind) {
  if (!Node)
    return nullptr;
  NodePointer NewNode = nullptr;
  if (Node->hasText()) {
    NewNode = createNodeWithAllocatedText(NewKind, Node->getText());
  } else if (Node->hasIndex()) {
    NewNode = createNode(NewKind, Node->getIndex());
  } else {
    NewNode = createNode(NewKind);
  }
  for (NodePointer Child : *Node) {
    NewNode->addChild(Child, *this);
  }
  return NewNode;
}

NodePointer Demangler::demangleTypeMangling() {
  auto Type = popNode(Node::Kind::Type);
  auto LabelList = popFunctionParamLabels(Type);
  auto TypeMangling = createNode(Node::Kind::TypeMangling);

  addChild(TypeMangling, LabelList);
  TypeMangling = addChild(TypeMangling, Type);
  return TypeMangling;
}

NodePointer Demangler::demangleSymbolicReference(unsigned char rawKind) {
  // The symbolic reference is a 4-byte machine integer encoded in the following
  // four bytes.
  if (Pos + 4 > Text.size())
    return nullptr;
  const void *at = Text.data() + Pos;
  int32_t value;
  memcpy(&value, at, 4);
  Pos += 4;
  
  // Map the encoded kind to a specific kind and directness.
  SymbolicReferenceKind kind;
  Directness direct;
  switch (rawKind) {
  case 0x01:
    kind = SymbolicReferenceKind::Context;
    direct = Directness::Direct;
    break;
  case 0x02:
    kind = SymbolicReferenceKind::Context;
    direct = Directness::Indirect;
    break;
  case 0x09:
    kind = SymbolicReferenceKind::AccessorFunctionReference;
    direct = Directness::Direct;
    break;
  case 0x0a:
    kind = SymbolicReferenceKind::UniqueExtendedExistentialTypeShape;
    direct = Directness::Direct;
    break;
  case 0x0b:
    kind = SymbolicReferenceKind::NonUniqueExtendedExistentialTypeShape;
    direct = Directness::Direct;
    break;
  // These are all currently reserved but unused.
  case 0x03: // direct to protocol conformance descriptor
  case 0x04: // indirect to protocol conformance descriptor
  case 0x05: // direct to associated conformance descriptor
  case 0x06: // indirect to associated conformance descriptor
  case 0x07: // direct to associated conformance access function
  case 0x08: // indirect to associated conformance access function
  default:
    return nullptr;
  }
  
  // Use the resolver, if any, to produce the demangling tree the symbolic
  // reference represents.
  NodePointer resolved = nullptr;
  if (SymbolicReferenceResolver)
    resolved = SymbolicReferenceResolver(kind, direct, value, at);

  // With no resolver, or a resolver that failed, refuse to demangle further.
  if (!resolved)
    return nullptr;
  
  // Types register as substitutions even when symbolically referenced.
  // OOPS: Except for opaque type references!
  if (kind == SymbolicReferenceKind::Context &&
      resolved->getKind() != Node::Kind::OpaqueTypeDescriptorSymbolicReference &&
      resolved->getKind() != Node::Kind::OpaqueReturnTypeOf)
    addSubstitution(resolved);
  return resolved;
}

NodePointer Demangler::demangleTypeAnnotation() {
  switch (char c2 = nextChar()) {
  case 'a':
    return createNode(Node::Kind::AsyncAnnotation);
  case 'b':
    return createNode(Node::Kind::ConcurrentFunctionType);
  case 'c':
    return createWithChild(
        Node::Kind::GlobalActorFunctionType, popTypeAndGetChild());
  case 'i':
    return createType(
        createWithChild(Node::Kind::Isolated, popTypeAndGetChild()));
  case 'j':
    return demangleDifferentiableFunctionType();
  case 'k':
    return createType(
        createWithChild(Node::Kind::NoDerivative, popTypeAndGetChild()));
  case 't':
    return createType(
        createWithChild(Node::Kind::CompileTimeConst, popTypeAndGetChild()));
  default:
    return nullptr;
  }
}

NodePointer Demangler::demangleOperator() {
recur:
  switch (unsigned char c = nextChar()) {
    case 0xFF:
      // A 0xFF byte is used as alignment padding for symbolic references
      // when the platform toolchain has alignment restrictions for the
      // relocations that form the reference value. It can be skipped.
      goto recur;
    case 1: case 2:   case 3:   case 4:   case 5: case 6: case 7: case 8:
    case 9: case 0xA: case 0xB: case 0xC:
      return demangleSymbolicReference((unsigned char)c);
    case 'A': return demangleMultiSubstitutions();
    case 'B': return demangleBuiltinType();
    case 'C': return demangleAnyGenericType(Node::Kind::Class);
    case 'D': return demangleTypeMangling();
    case 'E': return demangleExtensionContext();
    case 'F': return demanglePlainFunction();
    case 'G': return demangleBoundGenericType();
    case 'H':
      switch (char c2 = nextChar()) {
      case 'A': return demangleDependentProtocolConformanceAssociated();
      case 'a': return createNode(Node::Kind::RuntimeDiscoverableAttributeRecord);
      case 'C': return demangleConcreteProtocolConformance();
      case 'D': return demangleDependentProtocolConformanceRoot();
      case 'I': return demangleDependentProtocolConformanceInherited();
      case 'P':
        return createWithChild(
            Node::Kind::ProtocolConformanceRefInTypeModule, popProtocol());
      case 'p':
        return createWithChild(
            Node::Kind::ProtocolConformanceRefInProtocolModule, popProtocol());

      // Runtime records (type/protocol/conformance/function)
      case 'c':
        return createWithChild(Node::Kind::ProtocolConformanceDescriptorRecord,
                               popProtocolConformance());
      case 'n':
        return createWithPoppedType(Node::Kind::NominalTypeDescriptorRecord);
      case 'o': // XXX
        return createWithChild(Node::Kind::OpaqueTypeDescriptorRecord, popNode());
      case 'r':
        return createWithChild(Node::Kind::ProtocolDescriptorRecord, popProtocol());
      case 'F':
        return createNode(Node::Kind::AccessibleFunctionRecord);

      default:
        pushBack();
        pushBack();
        return demangleIdentifier();
      }

    case 'I': return demangleImplFunctionType();
    case 'K': return createNode(Node::Kind::ThrowsAnnotation);
    case 'L': return demangleLocalIdentifier();
    case 'M': return demangleMetatype();
    case 'N': return createWithChild(Node::Kind::TypeMetadata,
                                     popNode(Node::Kind::Type));
    case 'O': return demangleAnyGenericType(Node::Kind::Enum);
    case 'P': return demangleAnyGenericType(Node::Kind::Protocol);
    case 'Q': return demangleArchetype();
    case 'R': return demangleGenericRequirement();
    case 'S': return demangleStandardSubstitution();
    case 'T': return demangleThunkOrSpecialization();
    case 'V': return demangleAnyGenericType(Node::Kind::Structure);
    case 'W': return demangleWitness();
    case 'X': return demangleSpecialType();
    case 'Y': return demangleTypeAnnotation();
    case 'Z': return createWithChild(Node::Kind::Static, popNode(isEntity));
    case 'a': return demangleAnyGenericType(Node::Kind::TypeAlias);
    case 'c': return popFunctionType(Node::Kind::FunctionType);
    case 'd': return createNode(Node::Kind::VariadicMarker);
    case 'f': return demangleFunctionEntity();
    case 'g': return demangleRetroactiveConformance();
    case 'h': return createType(createWithChild(Node::Kind::Shared,
                                                popTypeAndGetChild()));
    case 'i': return demangleSubscript();
    case 'l': return demangleGenericSignature(/*hasParamCounts*/ false);
    case 'm': return createType(createWithChild(Node::Kind::Metatype,
                                                popNode(Node::Kind::Type)));
    case 'n':
      return createType(
          createWithChild(Node::Kind::Owned, popTypeAndGetChild()));
    case 'o': return demangleOperatorIdentifier();
    case 'p': return demangleProtocolListType();
    case 'q': return createType(demangleGenericParamIndex());
    case 'r': return demangleGenericSignature(/*hasParamCounts*/ true);
    case 's': return createNode(Node::Kind::Module, STDLIB_NAME);
    case 't': return popTuple();
    case 'u': return demangleGenericType();
    case 'v': return demangleVariable();
    case 'w': return demangleValueWitness();
    case 'x': return createType(getDependentGenericParamType(0, 0));
    case 'y': return createNode(Node::Kind::EmptyList);
    case 'z': return createType(createWithChild(Node::Kind::InOut,
                                                popTypeAndGetChild()));
    case '_': return createNode(Node::Kind::FirstElementMarker);
    case '.':
      // IRGen still uses '.<n>' to disambiguate partial apply thunks and
      // outlined copy functions. We treat such a suffix as "unmangled suffix".
      pushBack();
      return createNode(Node::Kind::Suffix, consumeAll());
    default:
      pushBack();
      return demangleIdentifier();
  }
}

int Demangler::demangleNatural() {
  if (!isDigit(peekChar()))
    return -1000;
  int num = 0;
  while (true) {
    char c = peekChar();
    if (!isDigit(c))
      return num;
    int newNum = (10 * num) + (c - '0');
    if (newNum < num)
      return -1000;
    num = newNum;
    nextChar();
  }
}

int Demangler::demangleIndex() {
  if (nextIf('_'))
    return 0;
  int num = demangleNatural();
  if (num >= 0 && nextIf('_'))
    return num + 1;
  return -1000;
}

NodePointer Demangler::demangleIndexAsNode() {
  int Idx = demangleIndex();
  if (Idx >= 0)
    return createNode(Node::Kind::Number, Idx);
  return nullptr;
}

NodePointer Demangler::demangleMultiSubstitutions() {
  int RepeatCount = -1;
  while (true) {
    char c = nextChar();
    if (c == 0) {
      // End of text.
      return nullptr;
    }
    if (isLowerLetter(c)) {
      // It's a substitution with an index < 26.
      NodePointer Nd = pushMultiSubstitutions(RepeatCount, c - 'a');
      if (!Nd)
        return nullptr;
      pushNode(Nd);
      RepeatCount = -1;
      // A lowercase letter indicates that there are more substitutions to
      // follow.
      continue;
    }
    if (isUpperLetter(c)) {
      // The last substitution.
      return pushMultiSubstitutions(RepeatCount, c - 'A');
    }
    if (c == '_') {
      // The previously demangled number is actually not a repeat count but
      // the large (> 26) index of a substitution. Because it's an index we
      // have to add 27 and not 26.
      unsigned Idx = RepeatCount + 27;
      if (Idx >= Substitutions.size())
        return nullptr;
      return Substitutions[Idx];
    }
    pushBack();
    // Not a letter? Then it can only be a natural number which might be the
    // repeat count or a large (> 26) substitution index.
    RepeatCount = demangleNatural();
    if (RepeatCount < 0)
      return nullptr;
  }
}

NodePointer Demangler::pushMultiSubstitutions(int RepeatCount,
                                              size_t SubstIdx) {
  if (SubstIdx >= Substitutions.size())
    return nullptr;
  if (RepeatCount > SubstitutionMerging::MaxRepeatCount)
    return nullptr;
  NodePointer Nd = Substitutions[SubstIdx];
  while (RepeatCount-- > 1) {
    pushNode(Nd);
  }
  return Nd;
}

NodePointer Demangler::createSwiftType(Node::Kind typeKind, const char *name) {
  return createType(createWithChildren(typeKind,
    createNode(Node::Kind::Module, STDLIB_NAME),
    createNode(Node::Kind::Identifier, name)));
}

NodePointer Demangler::demangleStandardSubstitution() {
  switch (char c = nextChar()) {
    case 'o':
      return createNode(Node::Kind::Module, MANGLING_MODULE_OBJC);
    case 'C':
      return createNode(Node::Kind::Module, MANGLING_MODULE_CLANG_IMPORTER);
    case 'g': {
      NodePointer OptionalTy =
        createType(createWithChildren(Node::Kind::BoundGenericEnum,
          createSwiftType(Node::Kind::Enum, "Optional"),
          createWithChild(Node::Kind::TypeList, popNode(Node::Kind::Type))));
      addSubstitution(OptionalTy);
      return OptionalTy;
    }
    default: {
      pushBack();
      int RepeatCount = demangleNatural();
      if (RepeatCount > SubstitutionMerging::MaxRepeatCount)
        return nullptr;
      bool secondLevelSubstitution = nextIf('c');
      if (NodePointer Nd = createStandardSubstitution(
              nextChar(), secondLevelSubstitution)) {
        while (RepeatCount-- > 1) {
          pushNode(Nd);
        }
        return Nd;
      }
      return nullptr;
    }
  }
}

NodePointer Demangler::createStandardSubstitution(
    char Subst, bool SecondLevel) {
#define STANDARD_TYPE(KIND, MANGLING, TYPENAME)                   \
  if (!SecondLevel && Subst == #MANGLING[0]) {                    \
    return createSwiftType(Node::Kind::KIND, #TYPENAME);          \
  }

#define STANDARD_TYPE_CONCURRENCY(KIND, MANGLING, TYPENAME)                   \
  if (SecondLevel && Subst == #MANGLING[0]) {                    \
    return createSwiftType(Node::Kind::KIND, #TYPENAME);          \
  }

#include "swift/Demangling/StandardTypesMangling.def"
  return nullptr;
}

NodePointer Demangler::demangleIdentifier() {
  bool hasWordSubsts = false;
  bool isPunycoded = false;
  char c = peekChar();
  if (!isDigit(c))
    return nullptr;
  if (c == '0') {
    nextChar();
    if (peekChar() == '0') {
      nextChar();
      isPunycoded = true;
    } else {
      hasWordSubsts = true;
    }
  }
  CharVector Identifier;
  do {
    while (hasWordSubsts && isLetter(peekChar())) {
      char c = nextChar();
      int WordIdx = 0;
      if (isLowerLetter(c)) {
        WordIdx = c - 'a';
      } else {
        assert(isUpperLetter(c));
        WordIdx = c - 'A';
        hasWordSubsts = false;
      }
      if (WordIdx >= NumWords)
        return nullptr;
      assert(WordIdx < MaxNumWords);
      StringRef Slice = Words[WordIdx];
      Identifier.append(Slice, *this);
    }
    if (nextIf('0'))
      break;
    int numChars = demangleNatural();
    if (numChars <= 0)
      return nullptr;
    if (isPunycoded)
      nextIf('_');
    if (Pos + numChars > Text.size())
      return nullptr;
    StringRef Slice = StringRef(Text.data() + Pos, numChars);
    if (isPunycoded) {
      std::string PunycodedString;
      if (!Punycode::decodePunycodeUTF8(Slice, PunycodedString))
        return nullptr;
      Identifier.append(StringRef(PunycodedString), *this);
    } else {
      Identifier.append(Slice, *this);
      int wordStartPos = -1;
      for (int Idx = 0, End = (int)Slice.size(); Idx <= End; ++Idx) {
        char c = (Idx < End ? Slice[Idx] : 0);
        if (wordStartPos >= 0 && isWordEnd(c, Slice[Idx - 1])) {
          if (Idx - wordStartPos >= 2 && NumWords < MaxNumWords) {
            StringRef word(Slice.begin() + wordStartPos, Idx - wordStartPos);
            Words[NumWords++] = word;
          }
          wordStartPos = -1;
        }
        if (wordStartPos < 0 && isWordStart(c)) {
          wordStartPos = Idx;
        }
      }
    }
    Pos += numChars;
  } while (hasWordSubsts);

  if (Identifier.empty())
    return nullptr;
  NodePointer Ident = createNode(Node::Kind::Identifier, Identifier);
  addSubstitution(Ident);
  return Ident;
}

NodePointer Demangler::demangleOperatorIdentifier() {
  NodePointer Ident = popNode(Node::Kind::Identifier);
  if (!Ident)
    return nullptr;

  static const char op_char_table[] = "& @/= >    <*!|+?%-~   ^ .";

  CharVector OpStr;
  for (signed char c : Ident->getText()) {
    if (c < 0) {
      // Pass through Unicode characters.
      OpStr.push_back(c, *this);
      continue;
    }
    if (!isLowerLetter(c))
      return nullptr;
    char o = op_char_table[c - 'a'];
    if (o == ' ')
      return nullptr;
    OpStr.push_back(o, *this);
  }
  switch (nextChar()) {
    case 'i': return createNode(Node::Kind::InfixOperator, OpStr);
    case 'p': return createNode(Node::Kind::PrefixOperator, OpStr);
    case 'P': return createNode(Node::Kind::PostfixOperator, OpStr);
    default: return nullptr;
  }
}

NodePointer Demangler::demangleLocalIdentifier() {
  if (nextIf('L')) {
    NodePointer discriminator = popNode(Node::Kind::Identifier);
    NodePointer name = popNode(isDeclName);
    return createWithChildren(Node::Kind::PrivateDeclName, discriminator, name);
  }
  if (nextIf('l')) {
    NodePointer discriminator = popNode(Node::Kind::Identifier);
    return createWithChild(Node::Kind::PrivateDeclName, discriminator);
  }
  if ((peekChar() >= 'a' && peekChar() <= 'j') ||
      (peekChar() >= 'A' && peekChar() <= 'J')) {
    char relatedEntityKind = nextChar();
    NodePointer kindNd = createNode(Node::Kind::Identifier,
                                    StringRef(&relatedEntityKind, 1));
    NodePointer name = popNode();
    NodePointer result = createNode(Node::Kind::RelatedEntityDeclName);
    addChild(result, kindNd);
    return addChild(result, name);
  }
  NodePointer discriminator = demangleIndexAsNode();
  NodePointer name = popNode(isDeclName);
  return createWithChildren(Node::Kind::LocalDeclName, discriminator, name);
}

NodePointer Demangler::popModule() {
  if (NodePointer Ident = popNode(Node::Kind::Identifier))
    return changeKind(Ident, Node::Kind::Module);
  return popNode(Node::Kind::Module);
}

NodePointer Demangler::popContext() {
  if (NodePointer Mod = popModule())
    return Mod;

  if (NodePointer Ty = popNode(Node::Kind::Type)) {
    if (Ty->getNumChildren() != 1)
      return nullptr;
    NodePointer Child = Ty->getFirstChild();
    if (!isContext(Child->getKind()))
      return nullptr;
    return Child;
  }
  return popNode(isContext);
}

NodePointer Demangler::popTypeAndGetChild() {
  NodePointer Ty = popNode(Node::Kind::Type);
  if (!Ty || Ty->getNumChildren() != 1)
    return nullptr;
  return Ty->getFirstChild();
}

NodePointer Demangler::popTypeAndGetAnyGeneric() {
  NodePointer Child = popTypeAndGetChild();
  if (Child && isAnyGeneric(Child->getKind()))
    return Child;
  return nullptr;
}

NodePointer Demangler::demangleBuiltinType() {
  NodePointer Ty = nullptr;
  const int maxTypeSize = 4096; // a very conservative upper bound
  switch (nextChar()) {
    case 'b':
      Ty = createNode(Node::Kind::BuiltinTypeName,
                               BUILTIN_TYPE_NAME_BRIDGEOBJECT);
      break;
    case 'B':
      Ty = createNode(Node::Kind::BuiltinTypeName,
                              BUILTIN_TYPE_NAME_UNSAFEVALUEBUFFER);
      break;
    case 'e':
      Ty = createNode(Node::Kind::BuiltinTypeName,
                              BUILTIN_TYPE_NAME_EXECUTOR);
      break;
    case 'f': {
      int size = demangleIndex() - 1;
      if (size <= 0 || size > maxTypeSize)
        return nullptr;
      CharVector name;
      name.append(BUILTIN_TYPE_NAME_FLOAT, *this);
      name.append(size, *this);
      Ty = createNode(Node::Kind::BuiltinTypeName, name);
      break;
    }
    case 'i': {
      int size = demangleIndex() - 1;
      if (size <= 0 || size > maxTypeSize)
        return nullptr;
      CharVector name;
      name.append(BUILTIN_TYPE_NAME_INT, *this);
      name.append(size, *this);
      Ty = createNode(Node::Kind::BuiltinTypeName, name);
      break;
    }
    case 'I':
      Ty = createNode(Node::Kind::BuiltinTypeName,
                      BUILTIN_TYPE_NAME_INTLITERAL);
      break;
    case 'v': {
      int elts = demangleIndex() - 1;
      if (elts <= 0 || elts > maxTypeSize)
        return nullptr;
      NodePointer EltType = popTypeAndGetChild();
      if (!EltType || EltType->getKind() != Node::Kind::BuiltinTypeName ||
          !EltType->getText().startswith(BUILTIN_TYPE_NAME_PREFIX))
        return nullptr;
      CharVector name;
      name.append(BUILTIN_TYPE_NAME_VEC, *this);
      name.append(elts, *this);
      name.push_back('x', *this);
      name.append(EltType->getText().substr(BUILTIN_TYPE_NAME_PREFIX.size()),
                  *this);
      Ty = createNode(Node::Kind::BuiltinTypeName, name);
      break;
    }
    case 'O':
      Ty = createNode(Node::Kind::BuiltinTypeName,
                               BUILTIN_TYPE_NAME_UNKNOWNOBJECT);
      break;
    case 'o':
      Ty = createNode(Node::Kind::BuiltinTypeName,
                               BUILTIN_TYPE_NAME_NATIVEOBJECT);
      break;
    case 'p':
      Ty = createNode(Node::Kind::BuiltinTypeName,
                               BUILTIN_TYPE_NAME_RAWPOINTER);
      break;
    case 'j':
      Ty = createNode(Node::Kind::BuiltinTypeName,
                               BUILTIN_TYPE_NAME_JOB);
      break;
    case 'D':
      Ty = createNode(Node::Kind::BuiltinTypeName,
                               BUILTIN_TYPE_NAME_DEFAULTACTORSTORAGE);
      break;
    case 'd':
      Ty = createNode(Node::Kind::BuiltinTypeName,
                               BUILTIN_TYPE_NAME_NONDEFAULTDISTRIBUTEDACTORSTORAGE);
      break;
    case 'c':
      Ty = createNode(Node::Kind::BuiltinTypeName,
                               BUILTIN_TYPE_NAME_RAWUNSAFECONTINUATION);
      break;
    case 't':
      Ty = createNode(Node::Kind::BuiltinTypeName, BUILTIN_TYPE_NAME_SILTOKEN);
      break;
    case 'w':
      Ty = createNode(Node::Kind::BuiltinTypeName,
                               BUILTIN_TYPE_NAME_WORD);
      break;
    default:
      return nullptr;
  }
  return createType(Ty);
}

NodePointer Demangler::demangleAnyGenericType(Node::Kind kind) {
  NodePointer Name = popNode(isDeclName);
  NodePointer Ctx = popContext();
  NodePointer NTy = createType(createWithChildren(kind, Ctx, Name));
  addSubstitution(NTy);
  return NTy;
}

NodePointer Demangler::demangleExtensionContext() {
  NodePointer GenSig = popNode(Node::Kind::DependentGenericSignature);
  NodePointer Module = popModule();
  NodePointer Type = popTypeAndGetAnyGeneric();
  NodePointer Ext = createWithChildren(Node::Kind::Extension, Module, Type);
  if (GenSig)
    Ext = addChild(Ext, GenSig);
  return Ext;
}

/// Associate any \c OpaqueReturnType nodes with the declaration whose opaque
/// return type they refer back to.
static Node *setParentForOpaqueReturnTypeNodes(Demangler &D,
                                              Node *parent,
                                              Node *visitedNode) {
  if (!parent || !visitedNode)
    return nullptr;
  if (visitedNode->getKind() == Node::Kind::OpaqueReturnType) {
    // If this node is not already parented, parent it.
    if (visitedNode->hasChildren()
        && visitedNode->getLastChild()->getKind() == Node::Kind::OpaqueReturnTypeParent) {
      return parent;
    }
    visitedNode->addChild(D.createNode(Node::Kind::OpaqueReturnTypeParent,
                                       (Node::IndexType)parent), D);
    return parent;
  }
  
  // If this node is one that may in turn define its own opaque return type,
  // stop recursion, since any opaque return type nodes underneath would refer
  // to the nested declaration rather than the one we're looking at.
  if (visitedNode->getKind() == Node::Kind::Function
      || visitedNode->getKind() == Node::Kind::Variable
      || visitedNode->getKind() == Node::Kind::Subscript) {
    return parent;
  }
  
  for (size_t i = 0, e = visitedNode->getNumChildren(); i < e; ++i) {
    setParentForOpaqueReturnTypeNodes(D, parent, visitedNode->getChild(i));
  }
  return parent;
}

NodePointer Demangler::demanglePlainFunction() {
  NodePointer GenSig = popNode(Node::Kind::DependentGenericSignature);
  NodePointer Type = popFunctionType(Node::Kind::FunctionType);
  NodePointer LabelList = popFunctionParamLabels(Type);

  if (GenSig) {
    Type = createType(createWithChildren(Node::Kind::DependentGenericType,
                                         GenSig, Type));
  }

  auto Name = popNode(isDeclName);
  auto Ctx = popContext();

  NodePointer result = LabelList
    ? createWithChildren(Node::Kind::Function, Ctx, Name, LabelList, Type)
    : createWithChildren(Node::Kind::Function, Ctx, Name, Type);
    
  result = setParentForOpaqueReturnTypeNodes(*this, result, Type);
  return result;
}

NodePointer Demangler::popFunctionType(Node::Kind kind, bool hasClangType) {
  NodePointer FuncType = createNode(kind);
  NodePointer ClangType = nullptr;
  if (hasClangType) {
    ClangType = demangleClangType();
  }
  addChild(FuncType, ClangType);
  addChild(FuncType, popNode(Node::Kind::GlobalActorFunctionType));
  addChild(FuncType, popNode(Node::Kind::DifferentiableFunctionType));
  addChild(FuncType, popNode(Node::Kind::ThrowsAnnotation));
  addChild(FuncType, popNode(Node::Kind::ConcurrentFunctionType));
  addChild(FuncType, popNode(Node::Kind::AsyncAnnotation));

  FuncType = addChild(FuncType, popFunctionParams(Node::Kind::ArgumentTuple));
  FuncType = addChild(FuncType, popFunctionParams(Node::Kind::ReturnType));
  return createType(FuncType);
}

NodePointer Demangler::popFunctionParams(Node::Kind kind) {
  NodePointer ParamsType = nullptr;
  if (popNode(Node::Kind::EmptyList)) {
    ParamsType = createType(createNode(Node::Kind::Tuple));
  } else {
    ParamsType = popNode(Node::Kind::Type);
  }
  return createWithChild(kind, ParamsType);
}

NodePointer Demangler::popFunctionParamLabels(NodePointer Type) {
  if (!IsOldFunctionTypeMangling && popNode(Node::Kind::EmptyList))
    return createNode(Node::Kind::LabelList);

  if (!Type || Type->getKind() != Node::Kind::Type)
    return nullptr;

  auto FuncType = Type->getFirstChild();
  if (FuncType->getKind() == Node::Kind::DependentGenericType)
    FuncType = FuncType->getChild(1)->getFirstChild();

  if (FuncType->getKind() != Node::Kind::FunctionType &&
      FuncType->getKind() != Node::Kind::NoEscapeFunctionType)
    return nullptr;

  unsigned FirstChildIdx = 0;
  if (FuncType->getChild(FirstChildIdx)->getKind()
        == Node::Kind::GlobalActorFunctionType)
    ++FirstChildIdx;
  if (FuncType->getChild(FirstChildIdx)->getKind()
        == Node::Kind::DifferentiableFunctionType)
    ++FirstChildIdx;
  if (FuncType->getChild(FirstChildIdx)->getKind()
        == Node::Kind::ThrowsAnnotation)
    ++FirstChildIdx;
  if (FuncType->getChild(FirstChildIdx)->getKind()
        == Node::Kind::ConcurrentFunctionType)
    ++FirstChildIdx;
  if (FuncType->getChild(FirstChildIdx)->getKind()
        == Node::Kind::AsyncAnnotation)
    ++FirstChildIdx;
  auto ParameterType = FuncType->getChild(FirstChildIdx);

  assert(ParameterType->getKind() == Node::Kind::ArgumentTuple);

  NodePointer ParamsType = ParameterType->getFirstChild();
  assert(ParamsType->getKind() == Node::Kind::Type);
  auto Params = ParamsType->getFirstChild();
  unsigned NumParams =
    Params->getKind() == Node::Kind::Tuple ? Params->getNumChildren() : 1;

  if (NumParams == 0)
    return nullptr;

  auto getChildIf =
      [](NodePointer Node,
         Node::Kind filterBy) -> std::pair<NodePointer, unsigned> {
    for (unsigned i = 0, n = Node->getNumChildren(); i != n; ++i) {
      auto Child = Node->getChild(i);
      if (Child->getKind() == filterBy)
        return {Child, i};
    }
    return {nullptr, 0};
  };

  auto getLabel = [&](NodePointer Params, unsigned Idx) -> NodePointer {
    // Old-style function type mangling has labels as part of the argument.
    if (IsOldFunctionTypeMangling) {
      auto Param = Params->getChild(Idx);
      auto Label = getChildIf(Param, Node::Kind::TupleElementName);

      if (Label.first) {
        Param->removeChildAt(Label.second);
        return createNodeWithAllocatedText(Node::Kind::Identifier,
                                           Label.first->getText());
      }

      return createNode(Node::Kind::FirstElementMarker);
    }

    return popNode();
  };

  auto LabelList = createNode(Node::Kind::LabelList);
  auto Tuple = ParameterType->getFirstChild()->getFirstChild();

  if (IsOldFunctionTypeMangling &&
      (!Tuple || Tuple->getKind() != Node::Kind::Tuple))
    return LabelList;

  bool hasLabels = false;
  for (unsigned i = 0; i != NumParams; ++i) {
    auto Label = getLabel(Tuple, i);

    if (!Label)
      return nullptr;

    if (Label->getKind() != Node::Kind::Identifier &&
        Label->getKind() != Node::Kind::FirstElementMarker)
      return nullptr;

    LabelList->addChild(Label, *this);
    hasLabels |= Label->getKind() != Node::Kind::FirstElementMarker;
  }

  // Old style label mangling can produce label list without
  // actual labels, we need to support that case specifically.
  if (!hasLabels)
    return createNode(Node::Kind::LabelList);

  if (!IsOldFunctionTypeMangling)
    LabelList->reverseChildren();

  return LabelList;
}

NodePointer Demangler::popTuple() {
  NodePointer Root = createNode(Node::Kind::Tuple);

  if (!popNode(Node::Kind::EmptyList)) {
    bool firstElem = false;
    do {
      firstElem = (popNode(Node::Kind::FirstElementMarker) != nullptr);
      NodePointer TupleElmt = createNode(Node::Kind::TupleElement);
      addChild(TupleElmt, popNode(Node::Kind::VariadicMarker));
      if (NodePointer Ident = popNode(Node::Kind::Identifier)) {
        TupleElmt->addChild(createNodeWithAllocatedText(
                              Node::Kind::TupleElementName, Ident->getText()),
                            *this);
      }
      NodePointer Ty = popNode(Node::Kind::Type);
      if (!Ty)
        return nullptr;
      TupleElmt->addChild(Ty, *this);
      Root->addChild(TupleElmt, *this);
    } while (!firstElem);

    Root->reverseChildren();
  }
  return createType(Root);
}

NodePointer Demangler::popPack() {
  NodePointer Root = createNode(Node::Kind::Pack);

  if (!popNode(Node::Kind::EmptyList)) {
    bool firstElem = false;
    do {
      firstElem = (popNode(Node::Kind::FirstElementMarker) != nullptr);
      NodePointer Ty = popNode(Node::Kind::Type);
      if (!Ty)
        return nullptr;
      Root->addChild(Ty, *this);
    } while (!firstElem);

    Root->reverseChildren();
  }
  return createType(Root);
}

NodePointer Demangler::popSILPack() {
  NodePointer Root;

  switch (nextChar()) {
  case 'd':
    Root = createNode(Node::Kind::SILPackDirect);
    break;

  case 'i':
    Root = createNode(Node::Kind::SILPackIndirect);
    break;

  default:
    return nullptr;
  }

  if (!popNode(Node::Kind::EmptyList)) {
    bool firstElem = false;
    do {
      firstElem = (popNode(Node::Kind::FirstElementMarker) != nullptr);
      NodePointer Ty = popNode(Node::Kind::Type);
      if (!Ty)
        return nullptr;
      Root->addChild(Ty, *this);
    } while (!firstElem);

    Root->reverseChildren();
  }

  return createType(Root);
}

NodePointer Demangler::popTypeList() {
  NodePointer Root = createNode(Node::Kind::TypeList);

  if (!popNode(Node::Kind::EmptyList)) {
    bool firstElem = false;
    do {
      firstElem = (popNode(Node::Kind::FirstElementMarker) != nullptr);
      NodePointer Ty = popNode(Node::Kind::Type);
      if (!Ty)
        return nullptr;
      Root->addChild(Ty, *this);
    } while (!firstElem);
    
    Root->reverseChildren();
  }
  return Root;
}

NodePointer Demangler::popProtocol() {
  if (NodePointer Type = popNode(Node::Kind::Type)) {
    if (Type->getNumChildren() < 1)
      return nullptr;

    if (!isProtocolNode(Type))
      return nullptr;

    return Type;
  }
  
  if (NodePointer SymbolicRef = popNode(Node::Kind::ProtocolSymbolicReference)){
    return SymbolicRef;
  }

  NodePointer Name = popNode(isDeclName);
  NodePointer Ctx = popContext();
  NodePointer Proto = createWithChildren(Node::Kind::Protocol, Ctx, Name);
  return createType(Proto);
}

NodePointer Demangler::popAnyProtocolConformanceList() {
  NodePointer conformanceList
    = createNode(Node::Kind::AnyProtocolConformanceList);
  if (!popNode(Node::Kind::EmptyList)) {
    bool firstElem = false;
    do {
      firstElem = (popNode(Node::Kind::FirstElementMarker) != nullptr);
      NodePointer anyConformance = popAnyProtocolConformance();
      if (!anyConformance)
        return nullptr;
      conformanceList->addChild(anyConformance, *this);
    } while (!firstElem);

    conformanceList->reverseChildren();
  }
  return conformanceList;
}

NodePointer Demangler::popAnyProtocolConformance() {
  return popNode([](Node::Kind kind) {
    switch (kind) {
    case Node::Kind::ConcreteProtocolConformance:
    case Node::Kind::DependentProtocolConformanceRoot:
    case Node::Kind::DependentProtocolConformanceInherited:
    case Node::Kind::DependentProtocolConformanceAssociated:
      return true;

    default:
      return false;
    }
  });
}

NodePointer Demangler::demangleRetroactiveProtocolConformanceRef() {
  NodePointer module = popModule();
  NodePointer proto = popProtocol();
  auto protocolConformanceRef =
      createWithChildren(Node::Kind::ProtocolConformanceRefInOtherModule,
                         proto, module);
  return protocolConformanceRef;
}

NodePointer Demangler::demangleConcreteProtocolConformance() {
  NodePointer conditionalConformanceList = popAnyProtocolConformanceList();

  NodePointer conformanceRef =
      popNode(Node::Kind::ProtocolConformanceRefInTypeModule);
  if (!conformanceRef) {
    conformanceRef =
        popNode(Node::Kind::ProtocolConformanceRefInProtocolModule);
  }
  if (!conformanceRef)
    conformanceRef = demangleRetroactiveProtocolConformanceRef();

  NodePointer type = popNode(Node::Kind::Type);
  return createWithChildren(Node::Kind::ConcreteProtocolConformance,
                            type, conformanceRef, conditionalConformanceList);
}

NodePointer Demangler::popDependentProtocolConformance() {
  return popNode([](Node::Kind kind) {
    switch (kind) {
    case Node::Kind::DependentProtocolConformanceRoot:
    case Node::Kind::DependentProtocolConformanceInherited:
    case Node::Kind::DependentProtocolConformanceAssociated:
      return true;

    default:
      return false;
    }
  });
}

NodePointer Demangler::demangleDependentProtocolConformanceRoot() {
  NodePointer index = demangleDependentConformanceIndex();
  NodePointer protocol = popProtocol();
  NodePointer dependentType = popNode(Node::Kind::Type);
  return createWithChildren(Node::Kind::DependentProtocolConformanceRoot,
                            dependentType, protocol, index);
}

NodePointer Demangler::demangleDependentProtocolConformanceInherited() {
  NodePointer index = demangleDependentConformanceIndex();
  NodePointer protocol = popProtocol();
  NodePointer nested = popDependentProtocolConformance();
  return createWithChildren(Node::Kind::DependentProtocolConformanceInherited,
                            nested, protocol, index);
}

NodePointer Demangler::popDependentAssociatedConformance() {
  NodePointer protocol = popProtocol();
  NodePointer dependentType = popNode(Node::Kind::Type);
  return createWithChildren(Node::Kind::DependentAssociatedConformance,
                            dependentType, protocol);
}

NodePointer Demangler::demangleDependentProtocolConformanceAssociated() {
  NodePointer index = demangleDependentConformanceIndex();
  NodePointer associatedConformance = popDependentAssociatedConformance();
  NodePointer nested = popDependentProtocolConformance();
  return createWithChildren(Node::Kind::DependentProtocolConformanceAssociated,
                            nested, associatedConformance, index);
}

NodePointer Demangler::demangleDependentConformanceIndex() {
  int index = demangleIndex();
  // index < 0 indicates a demangling error.
  // index == 0 is ill-formed by the (originally buggy) use of this production.
  if (index <= 0) return nullptr;

  // index == 1 indicates an unknown index.
  if (index == 1) return createNode(Node::Kind::UnknownIndex);

  // Remove the index adjustment.
  return createNode(Node::Kind::Index, unsigned(index) - 2);
}

NodePointer Demangler::demangleRetroactiveConformance() {
  NodePointer index = demangleIndexAsNode();
  NodePointer conformance = popAnyProtocolConformance();
  return createWithChildren(Node::Kind::RetroactiveConformance, index, conformance);
}

NodePointer Demangler::popRetroactiveConformances() {
  NodePointer conformancesNode = nullptr;
  while (auto conformance = popNode(Node::Kind::RetroactiveConformance)) {
    if (!conformancesNode)
      conformancesNode = createNode(Node::Kind::TypeList);
    conformancesNode->addChild(conformance, *this);
  }
  if (conformancesNode)
    conformancesNode->reverseChildren();
  return conformancesNode;
}

bool Demangler::demangleBoundGenerics(Vector<NodePointer> &TypeListList,
                                      NodePointer &RetroactiveConformances) {
  RetroactiveConformances = popRetroactiveConformances();
  
  for (;;) {
    NodePointer TList = createNode(Node::Kind::TypeList);
    TypeListList.push_back(TList, *this);
    while (NodePointer Ty = popNode(Node::Kind::Type)) {
      TList->addChild(Ty, *this);
    }
    TList->reverseChildren();
    
    if (popNode(Node::Kind::EmptyList))
      break;
    if (!popNode(Node::Kind::FirstElementMarker))
      return false;
  }
  return true;
}

NodePointer Demangler::demangleBoundGenericType() {
  NodePointer RetroactiveConformances;
  Vector<NodePointer> TypeListList(*this, 4);
  
  if (!demangleBoundGenerics(TypeListList, RetroactiveConformances))
    return nullptr;

  NodePointer Nominal = popTypeAndGetAnyGeneric();
  if (!Nominal)
    return nullptr;
  NodePointer BoundNode = demangleBoundGenericArgs(Nominal, TypeListList, 0);
  if (!BoundNode)
    return nullptr;
  addChild(BoundNode, RetroactiveConformances);
  NodePointer NTy = createType(BoundNode);
  addSubstitution(NTy);
  return NTy;
}

bool Demangle::nodeConsumesGenericArgs(Node *node) {
  switch (node->getKind()) {
    case Node::Kind::Variable:
    case Node::Kind::Subscript:
    case Node::Kind::ImplicitClosure:
    case Node::Kind::ExplicitClosure:
    case Node::Kind::DefaultArgumentInitializer:
    case Node::Kind::Initializer:
    case Node::Kind::PropertyWrapperBackingInitializer:
    case Node::Kind::PropertyWrapperInitFromProjectedValue:
    case Node::Kind::Static:
    case Node::Kind::RuntimeAttributeGenerator:
      return false;
    default:
      return true;
  }
}

NodePointer Demangler::demangleBoundGenericArgs(NodePointer Nominal,
                                    const Vector<NodePointer> &TypeLists,
                                    size_t TypeListIdx) {
  // TODO: This would be a lot easier if we represented bound generic args
  // flatly in the demangling tree, since that's how they're mangled and also
  // how the runtime generally wants to consume them.
  
  if (!Nominal)
    return nullptr;

  if (TypeListIdx >= TypeLists.size())
    return nullptr;

  // Associate a context symbolic reference with all remaining generic
  // arguments.
  if (Nominal->getKind() == Node::Kind::TypeSymbolicReference
      || Nominal->getKind() == Node::Kind::ProtocolSymbolicReference) {
    auto remainingTypeList = createNode(Node::Kind::TypeList);
    for (unsigned i = TypeLists.size() - 1;
         i >= TypeListIdx && i < TypeLists.size();
         --i) {
      auto list = TypeLists[i];
      for (auto child : *list) {
        remainingTypeList->addChild(child, *this);
      }
    }
    return createWithChildren(Node::Kind::BoundGenericOtherNominalType,
                              createType(Nominal), remainingTypeList);
  }

  // Generic arguments for the outermost type come first.
  if (Nominal->getNumChildren() == 0)
    return nullptr;
  NodePointer Context = Nominal->getFirstChild();

  bool consumesGenericArgs = nodeConsumesGenericArgs(Nominal);

  NodePointer args = TypeLists[TypeListIdx];

  if (consumesGenericArgs)
    ++TypeListIdx;

  if (TypeListIdx < TypeLists.size()) {
    NodePointer BoundParent = nullptr;
    if (Context->getKind() == Node::Kind::Extension) {
      BoundParent = demangleBoundGenericArgs(Context->getChild(1), TypeLists,
                                             TypeListIdx);
      BoundParent = createWithChildren(Node::Kind::Extension,
                                       Context->getFirstChild(),
                                       BoundParent);
      if (Context->getNumChildren() == 3) {
        // Add the generic signature of the extension context.
        addChild(BoundParent, Context->getChild(2));
      }
    } else {
      BoundParent = demangleBoundGenericArgs(Context, TypeLists, TypeListIdx);
    }
    // Rebuild this type with the new parent type, which may have
    // had its generic arguments applied.
    NodePointer NewNominal = createWithChild(Nominal->getKind(), BoundParent);
    if (!NewNominal)
      return nullptr;

    // Append remaining children of the origin nominal.
    for (unsigned Idx = 1; Idx < Nominal->getNumChildren(); ++Idx) {
      addChild(NewNominal, Nominal->getChild(Idx));
    }
    Nominal = NewNominal;
  }
  if (!consumesGenericArgs)
    return Nominal;

  // If there were no arguments at this level there is nothing left
  // to do.
  if (args->getNumChildren() == 0)
    return Nominal;

  Node::Kind kind;
  switch (Nominal->getKind()) {
  case Node::Kind::Class:
    kind = Node::Kind::BoundGenericClass;
    break;
  case Node::Kind::Structure:
    kind = Node::Kind::BoundGenericStructure;
    break;
  case Node::Kind::Enum:
    kind = Node::Kind::BoundGenericEnum;
    break;
  case Node::Kind::Protocol:
    kind = Node::Kind::BoundGenericProtocol;
    break;
  case Node::Kind::OtherNominalType:
    kind = Node::Kind::BoundGenericOtherNominalType;
    break;
  case Node::Kind::TypeAlias:
    kind = Node::Kind::BoundGenericTypeAlias;
    break;
  case Node::Kind::Function:
  case Node::Kind::Constructor:
    // Well, not really a nominal type.
    return createWithChildren(Node::Kind::BoundGenericFunction, Nominal, args);
  default:
    return nullptr;
  }
  return createWithChildren(kind, createType(Nominal), args);
}

NodePointer Demangler::demangleImplParamConvention(Node::Kind ConvKind) {
  const char *attr = nullptr;
  switch (nextChar()) {
    case 'i': attr = "@in"; break;
    case 'c':
      attr = "@in_constant";
      break;
    case 'l': attr = "@inout"; break;
    case 'b': attr = "@inout_aliasable"; break;
    case 'n': attr = "@in_guaranteed"; break;
    case 'x': attr = "@owned"; break;
    case 'g': attr = "@guaranteed"; break;
    case 'e': attr = "@deallocating"; break;
    case 'y': attr = "@unowned"; break;
    case 'v': attr = "@pack_owned"; break;
    case 'p': attr = "@pack_guaranteed"; break;
    case 'm': attr = "@pack_inout"; break;
    default:
      pushBack();
      return nullptr;
  }
  return createWithChild(ConvKind,
                         createNode(Node::Kind::ImplConvention, attr));
}

NodePointer Demangler::demangleImplResultConvention(Node::Kind ConvKind) {
  const char *attr = nullptr;
  switch (nextChar()) {
    case 'r': attr = "@out"; break;
    case 'o': attr = "@owned"; break;
    case 'd': attr = "@unowned"; break;
    case 'u': attr = "@unowned_inner_pointer"; break;
    case 'a': attr = "@autoreleased"; break;
    case 'k': attr = "@pack_out"; break;
    default:
      pushBack();
      return nullptr;
  }
  return createWithChild(ConvKind,
                         createNode(Node::Kind::ImplConvention, attr));
}

NodePointer Demangler::demangleImplParameterResultDifferentiability() {
  // Empty string represents default differentiability.
  const char *attr = "";
  if (nextIf('w'))
    attr = "@noDerivative";
  return createNode(Node::Kind::ImplParameterResultDifferentiability, attr);
}

NodePointer Demangler::demangleClangType() {
  int numChars = demangleNatural();
  if (numChars <= 0 || Pos + numChars > Text.size())
    return nullptr;
  CharVector mangledClangType;
  mangledClangType.append(StringRef(Text.data() + Pos, numChars), *this);
  Pos = Pos + numChars;
  return createNode(Node::Kind::ClangType, mangledClangType);
}

NodePointer Demangler::demangleImplFunctionType() {
  NodePointer type = createNode(Node::Kind::ImplFunctionType);

  if (nextIf('s')) {
    Vector<NodePointer> Substitutions;
    NodePointer SubstitutionRetroConformances;
    if (!demangleBoundGenerics(Substitutions, SubstitutionRetroConformances))
      return nullptr;

    NodePointer sig = popNode(Node::Kind::DependentGenericSignature);
    if (!sig)
      return nullptr;

    auto subsNode = createNode(Node::Kind::ImplPatternSubstitutions);
    subsNode->addChild(sig, *this);
    assert(Substitutions.size() == 1);
    subsNode->addChild(Substitutions[0], *this);
    if (SubstitutionRetroConformances)
      subsNode->addChild(SubstitutionRetroConformances, *this);
    type->addChild(subsNode, *this);
  }

  if (nextIf('I')) {
    Vector<NodePointer> Substitutions;
    NodePointer SubstitutionRetroConformances;
    if (!demangleBoundGenerics(Substitutions, SubstitutionRetroConformances))
      return nullptr;

    auto subsNode = createNode(Node::Kind::ImplInvocationSubstitutions);
    assert(Substitutions.size() == 1);
    subsNode->addChild(Substitutions[0], *this);
    if (SubstitutionRetroConformances)
      subsNode->addChild(SubstitutionRetroConformances, *this);
    type->addChild(subsNode, *this);
  }
  
  NodePointer GenSig = popNode(Node::Kind::DependentGenericSignature);
  if (GenSig && nextIf('P'))
    GenSig = changeKind(GenSig, Node::Kind::DependentPseudogenericSignature);

  if (nextIf('e'))
    type->addChild(createNode(Node::Kind::ImplEscaping), *this);

  switch ((MangledDifferentiabilityKind)peekChar()) {
  case MangledDifferentiabilityKind::Normal:  // 'd'
  case MangledDifferentiabilityKind::Linear:  // 'l'
  case MangledDifferentiabilityKind::Forward: // 'f'
  case MangledDifferentiabilityKind::Reverse: // 'r'
    type->addChild(
        createNode(
            Node::Kind::ImplDifferentiabilityKind, (Node::IndexType)nextChar()),
        *this);
    break;
  default:
    break;
  }

  const char *CAttr = nullptr;
  switch (nextChar()) {
    case 'y': CAttr = "@callee_unowned"; break;
    case 'g': CAttr = "@callee_guaranteed"; break;
    case 'x': CAttr = "@callee_owned"; break;
    case 't': CAttr = "@convention(thin)"; break;
    default: return nullptr;
  }
  type->addChild(createNode(Node::Kind::ImplConvention, CAttr), *this);

  const char *FConv = nullptr;
  bool hasClangType = false;
  switch (nextChar()) {
  case 'B': FConv = "block"; break;
  case 'C': FConv = "c"; break;
  case 'z': {
    switch (nextChar()) {
    case 'B': hasClangType = true; FConv = "block"; break;
    case 'C': hasClangType = true; FConv = "c"; break;
    default: pushBack(); pushBack(); break;
    }
    break;
  }
  case 'M': FConv = "method"; break;
  case 'O': FConv = "objc_method"; break;
  case 'K': FConv = "closure"; break;
  case 'W': FConv = "witness_method"; break;
  default: pushBack(); break;
  }
  if (FConv) {
    auto FAttrNode = createNode(Node::Kind::ImplFunctionConvention);
    FAttrNode->addChild(
        createNode(Node::Kind::ImplFunctionConventionName, FConv), *this);
    if (hasClangType)
      addChild(FAttrNode, demangleClangType());
    type->addChild(FAttrNode, *this);
  }

  const char *CoroAttr = nullptr;
  if (nextIf('A'))
    CoroAttr = "@yield_once";
  else if (nextIf('G'))
    CoroAttr = "@yield_many";
  if (CoroAttr)
    type->addChild(createNode(Node::Kind::ImplFunctionAttribute, CoroAttr), *this);

  if (nextIf('h')) {
    type->addChild(createNode(Node::Kind::ImplFunctionAttribute, "@Sendable"),
                   *this);
  }

  if (nextIf('H')) {
    type->addChild(createNode(Node::Kind::ImplFunctionAttribute, "@async"),
                   *this);
  }

  addChild(type, GenSig);

  int NumTypesToAdd = 0;
  while (NodePointer Param =
             demangleImplParamConvention(Node::Kind::ImplParameter)) {
    type = addChild(type, Param);
    if (NodePointer Diff = demangleImplParameterResultDifferentiability())
      Param = addChild(Param, Diff);
    ++NumTypesToAdd;
  }
  while (NodePointer Result = demangleImplResultConvention(
                                                    Node::Kind::ImplResult)) {
    type = addChild(type, Result);
    if (NodePointer Diff = demangleImplParameterResultDifferentiability())
      Result = addChild(Result, Diff);
    ++NumTypesToAdd;
  }
  while (nextIf('Y')) {
    NodePointer YieldResult =
      demangleImplParamConvention(Node::Kind::ImplYield);
    if (!YieldResult)
      return nullptr;
    type = addChild(type, YieldResult);
    ++NumTypesToAdd;
  }
  if (nextIf('z')) {
    NodePointer ErrorResult = demangleImplResultConvention(
                                                  Node::Kind::ImplErrorResult);
    if (!ErrorResult)
      return nullptr;
    type = addChild(type, ErrorResult);
    ++NumTypesToAdd;
  }
  if (!nextIf('_'))
    return nullptr;

  for (int Idx = 0; Idx < NumTypesToAdd; ++Idx) {
    NodePointer ConvTy = popNode(Node::Kind::Type);
    if (!ConvTy)
      return nullptr;
    type->getChild(type->getNumChildren() - Idx - 1)->addChild(ConvTy, *this);
  }
  
  return createType(type);
}

NodePointer Demangler::demangleMetatype() {
  switch (nextChar()) {
    case 'a':
      return createWithPoppedType(Node::Kind::TypeMetadataAccessFunction);
    case 'A':
      return createWithChild(Node::Kind::ReflectionMetadataAssocTypeDescriptor,
                             popProtocolConformance());
    case 'b':
      return createWithPoppedType(
          Node::Kind::CanonicalSpecializedGenericTypeMetadataAccessFunction);
    case 'B':
      return createWithChild(Node::Kind::ReflectionMetadataBuiltinDescriptor,
                               popNode(Node::Kind::Type));
    case 'c':
      return createWithChild(Node::Kind::ProtocolConformanceDescriptor,
                             popProtocolConformance());
    case 'C': {
      NodePointer Ty = popNode(Node::Kind::Type);
      if (!Ty || !isAnyGeneric(Ty->getChild(0)->getKind()))
        return nullptr;
      return createWithChild(Node::Kind::ReflectionMetadataSuperclassDescriptor,
                             Ty->getChild(0));
    }
    case 'D':
      return createWithPoppedType(Node::Kind::TypeMetadataDemanglingCache);
    case 'f':
      return createWithPoppedType(Node::Kind::FullTypeMetadata);
    case 'F':
      return createWithChild(Node::Kind::ReflectionMetadataFieldDescriptor,
                             popNode(Node::Kind::Type));
    case 'g':
      return createWithChild(Node::Kind::OpaqueTypeDescriptorAccessor,
                             popNode());
    case 'h':
      return createWithChild(Node::Kind::OpaqueTypeDescriptorAccessorImpl,
                             popNode());
    case 'i':
      return createWithPoppedType(Node::Kind::TypeMetadataInstantiationFunction);
    case 'I':
      return createWithPoppedType(Node::Kind::TypeMetadataInstantiationCache);
    case 'j':
      return createWithChild(Node::Kind::OpaqueTypeDescriptorAccessorKey,
                             popNode());
    case 'J':
      return createWithChild(Node::Kind::NoncanonicalSpecializedGenericTypeMetadataCache, popNode());
    case 'k':
      return createWithChild(Node::Kind::OpaqueTypeDescriptorAccessorVar,
                             popNode());
    case 'K':
      return createWithChild(Node::Kind::MetadataInstantiationCache,
                             popNode());
    case 'l':
      return createWithPoppedType(
                          Node::Kind::TypeMetadataSingletonInitializationCache);
    case 'L':
      return createWithPoppedType(Node::Kind::TypeMetadataLazyCache);
    case 'm':
      return createWithPoppedType(Node::Kind::Metaclass);
    case 'M':
      return createWithPoppedType(
          Node::Kind::CanonicalSpecializedGenericMetaclass);
    case 'n':
      return createWithPoppedType(Node::Kind::NominalTypeDescriptor);
    case 'N':
      return createWithPoppedType(
          Node::Kind::NoncanonicalSpecializedGenericTypeMetadata);
    case 'o':
      return createWithPoppedType(Node::Kind::ClassMetadataBaseOffset);
    case 'p':
      return createWithChild(Node::Kind::ProtocolDescriptor, popProtocol());
    case 'P':
      return createWithPoppedType(Node::Kind::GenericTypeMetadataPattern);
    case 'q':
      return createWithChild(Node::Kind::Uniquable, popNode());
    case 'Q':
      return createWithChild(Node::Kind::OpaqueTypeDescriptor, popNode());
    case 'r':
      return createWithPoppedType(Node::Kind::TypeMetadataCompletionFunction);
    case 's':
      return createWithPoppedType(Node::Kind::ObjCResilientClassStub);
    case 'S':
      return createWithChild(Node::Kind::ProtocolSelfConformanceDescriptor,
                             popProtocol());
    case 't':
      return createWithPoppedType(Node::Kind::FullObjCResilientClassStub);
    case 'u':
      return createWithPoppedType(Node::Kind::MethodLookupFunction);
    case 'U':
      return createWithPoppedType(Node::Kind::ObjCMetadataUpdateFunction);
    case 'V':
      return createWithChild(Node::Kind::PropertyDescriptor,
                             popNode(isEntity));
    case 'X':
      return demanglePrivateContextDescriptor();
    case 'z':
      return createWithPoppedType(
          Node::Kind::CanonicalPrespecializedGenericTypeCachingOnceToken);
    default:
      return nullptr;
  }
}
  
NodePointer Demangler::demanglePrivateContextDescriptor() {
  switch (nextChar()) {
  case 'E': {
    auto Extension = popContext();
    if (!Extension)
      return nullptr;
    return createWithChild(Node::Kind::ExtensionDescriptor, Extension);
  }
  case 'M': {
    auto Module = popModule();
    if (!Module)
      return nullptr;
    return createWithChild(Node::Kind::ModuleDescriptor, Module);
  }
  case 'Y': {
    auto Discriminator = popNode();
    if (!Discriminator)
      return nullptr;
    auto Context = popContext();
    if (!Context)
      return nullptr;
    
    auto node = createNode(Node::Kind::AnonymousDescriptor);
    node->addChild(Context, *this);
    node->addChild(Discriminator, *this);
    return node;
  }
  case 'X': {
    auto Context = popContext();
    if (!Context)
      return nullptr;
    return createWithChild(Node::Kind::AnonymousDescriptor, Context);
  }
  case 'A': {
    auto path = popAssocTypePath();
    if (!path)
      return nullptr;
    auto base = popNode(Node::Kind::Type);
    if (!base)
      return nullptr;
    return createWithChildren(Node::Kind::AssociatedTypeGenericParamRef,
                              base, path);
  }
  default:
    return nullptr;
  }
}

NodePointer Demangler::demangleArchetype() {
  switch (nextChar()) {
  case 'a': {
    NodePointer Ident = popNode(Node::Kind::Identifier);
    NodePointer ArcheTy = popTypeAndGetChild();
    NodePointer AssocTy = createType(
          createWithChildren(Node::Kind::AssociatedTypeRef, ArcheTy, Ident));
    addSubstitution(AssocTy);
    return AssocTy;
  }
  case 'O': {
    auto definingContext = popContext();
    return createWithChild(Node::Kind::OpaqueReturnTypeOf, definingContext);
  }
  case 'o': {
    auto index = demangleIndex();
    Vector<NodePointer> boundGenericArgs;
    NodePointer retroactiveConformances;
    if (!demangleBoundGenerics(boundGenericArgs, retroactiveConformances))
      return nullptr;
    auto Name = popNode();
    if (!Name)
      return nullptr;
    auto opaque = createWithChildren(Node::Kind::OpaqueType, Name,
                                   createNode(Node::Kind::Index, index));
    auto boundGenerics = createNode(Node::Kind::TypeList);
    for (unsigned i = boundGenericArgs.size(); i-- > 0;)
      boundGenerics->addChild(boundGenericArgs[i], *this);
    opaque->addChild(boundGenerics, *this);
    if (retroactiveConformances)
      opaque->addChild(retroactiveConformances, *this);
    
    auto opaqueTy = createType(opaque);
    addSubstitution(opaqueTy);
    return opaqueTy;
  }
  case 'r': {
    return createType(createNode(Node::Kind::OpaqueReturnType));
  }
  case 'R': {
    int ordinal = demangleIndex();
    if (ordinal < 0)
      return NULL;
    return createType(createWithChild(Node::Kind::OpaqueReturnType,
                      createNode(Node::Kind::OpaqueReturnTypeIndex, ordinal)));
  }

  case 'x': {
    NodePointer T = demangleAssociatedTypeSimple(nullptr);
    addSubstitution(T);
    return T;
  }
      
  case 'X': {
    NodePointer T = demangleAssociatedTypeCompound(nullptr);
    addSubstitution(T);
    return T;
  }

  case 'y': {
    NodePointer T = demangleAssociatedTypeSimple(demangleGenericParamIndex());
    addSubstitution(T);
    return T;
  }
  case 'Y': {
    NodePointer T = demangleAssociatedTypeCompound(
                                                 demangleGenericParamIndex());
    addSubstitution(T);
    return T;
  }

  case 'z': {
    NodePointer T = demangleAssociatedTypeSimple(
                                          getDependentGenericParamType(0, 0));
    addSubstitution(T);
    return T;
  }
  case 'Z': {
    NodePointer T = demangleAssociatedTypeCompound(
                                          getDependentGenericParamType(0, 0));
    addSubstitution(T);
    return T;
  }
  case 'p': {
    NodePointer CountTy = popTypeAndGetChild();
    NodePointer PatternTy = popTypeAndGetChild();
    NodePointer PackExpansionTy = createType(
          createWithChildren(Node::Kind::PackExpansion, PatternTy, CountTy));
    return PackExpansionTy;
  }
  case 'e': {
    NodePointer PackTy = popTypeAndGetChild();
    int level = demangleIndex();
    if (level < 0)
      return NULL;

    NodePointer PackElementTy = createType(
          createWithChildren(Node::Kind::PackElement, PackTy,
              createNode(Node::Kind::PackElementLevel, level)));
    return PackElementTy;
  }
  case 'P':
    return popPack();
  case 'S':
    return popSILPack();
  default:
    return nullptr;
  }
}

NodePointer Demangler::demangleAssociatedTypeSimple(NodePointer Base) {
  NodePointer ATName = popAssocTypeName();
  NodePointer BaseTy;
  if (Base) {
    BaseTy = createType(Base);
  } else {
    BaseTy = popNode(Node::Kind::Type);
  }
  return createType(createWithChildren(Node::Kind::DependentMemberType,
                                       BaseTy, ATName));
}

NodePointer Demangler::demangleAssociatedTypeCompound(NodePointer Base) {
  Vector<NodePointer> AssocTyNames(*this, 4);
  bool firstElem = false;
  do {
    firstElem = (popNode(Node::Kind::FirstElementMarker) != nullptr);
    NodePointer AssocTyName = popAssocTypeName();
    if (!AssocTyName)
      return nullptr;
    AssocTyNames.push_back(AssocTyName, *this);
  } while (!firstElem);
    
  NodePointer BaseTy;
  if (Base)
    BaseTy = createType(Base);
  else
    BaseTy = popNode(Node::Kind::Type);

  while (NodePointer AssocTy = AssocTyNames.pop_back_val()) {
    NodePointer depTy = createNode(Node::Kind::DependentMemberType);
    depTy = addChild(depTy, BaseTy);
    BaseTy = createType(addChild(depTy, AssocTy));
  }
  return BaseTy;
}

NodePointer Demangler::popAssocTypeName() {
  NodePointer Proto = popNode(Node::Kind::Type);
  if (Proto && !isProtocolNode(Proto))
    return nullptr;

  // If we haven't seen a protocol, check for a symbolic reference.
  if (!Proto)
    Proto = popNode(Node::Kind::ProtocolSymbolicReference);

  NodePointer Id = popNode(Node::Kind::Identifier);
  NodePointer AssocTy = createWithChild(Node::Kind::DependentAssociatedTypeRef, Id);
  addChild(AssocTy, Proto);
  return AssocTy;
}
  
NodePointer Demangler::popAssocTypePath() {
  NodePointer AssocTypePath = createNode(Node::Kind::AssocTypePath);
  bool firstElem = false;
  do {
    firstElem = (popNode(Node::Kind::FirstElementMarker) != nullptr);
    NodePointer AssocTy = popAssocTypeName();
    if (!AssocTy)
      return nullptr;
    AssocTypePath->addChild(AssocTy, *this);
  } while (!firstElem);
  AssocTypePath->reverseChildren();
  return AssocTypePath;
}

NodePointer Demangler::getDependentGenericParamType(int depth, int index) {
  if (depth < 0 || index < 0)
    return nullptr;

  auto paramTy = createNode(Node::Kind::DependentGenericParamType);
  paramTy->addChild(createNode(Node::Kind::Index, depth), *this);
  paramTy->addChild(createNode(Node::Kind::Index, index), *this);
  return paramTy;
}

NodePointer Demangler::demangleGenericParamIndex() {
  if (nextIf('d')) {
    int depth = demangleIndex() + 1;
    int index = demangleIndex();
    return getDependentGenericParamType(depth, index);
  }
  if (nextIf('z')) {
    return getDependentGenericParamType(0, 0);
  }
  if (nextIf('s')) {
    return createNode(Node::Kind::ConstrainedExistentialSelf);
  }
  return getDependentGenericParamType(0, demangleIndex() + 1);
}

NodePointer Demangler::popProtocolConformance() {
  NodePointer GenSig = popNode(Node::Kind::DependentGenericSignature);
  NodePointer Module = popModule();
  NodePointer Proto = popProtocol();
  NodePointer Type = popNode(Node::Kind::Type);
  NodePointer Ident = nullptr;
  if (!Type) {
    // Property behavior conformance
    Ident = popNode(Node::Kind::Identifier);
    Type = popNode(Node::Kind::Type);
  }
  if (GenSig) {
    Type = createType(createWithChildren(Node::Kind::DependentGenericType,
                                         GenSig, Type));
  }
  NodePointer Conf = createWithChildren(Node::Kind::ProtocolConformance,
                                        Type, Proto, Module);
  addChild(Conf, Ident);
  return Conf;
}

NodePointer Demangler::demangleThunkOrSpecialization() {
  switch (char c = nextChar()) {
    case 'c': return createWithChild(Node::Kind::CurryThunk, popNode(isEntity));
    case 'j': return createWithChild(Node::Kind::DispatchThunk, popNode(isEntity));
    case 'q': return createWithChild(Node::Kind::MethodDescriptor, popNode(isEntity));
    case 'o': return createNode(Node::Kind::ObjCAttribute);
    case 'O': return createNode(Node::Kind::NonObjCAttribute);
    case 'D': return createNode(Node::Kind::DynamicAttribute);
    case 'd': return createNode(Node::Kind::DirectMethodReferenceAttribute);
    case 'E': return createNode(Node::Kind::DistributedThunk);
    case 'F': return createNode(Node::Kind::DistributedAccessor);
    case 'a': return createNode(Node::Kind::PartialApplyObjCForwarder);
    case 'A': return createNode(Node::Kind::PartialApplyForwarder);
    case 'm': return createNode(Node::Kind::MergedFunction);
    case 'X': return createNode(Node::Kind::DynamicallyReplaceableFunctionVar);
    case 'x': return createNode(Node::Kind::DynamicallyReplaceableFunctionKey);
    case 'I': return createNode(Node::Kind::DynamicallyReplaceableFunctionImpl);
    case 'Y':
    case 'Q': {
      NodePointer discriminator = demangleIndexAsNode();
      return createWithChild(
          c == 'Q' ? Node::Kind::AsyncAwaitResumePartialFunction :
             /*'Y'*/ Node::Kind::AsyncSuspendResumePartialFunction,
          discriminator);
    }
    case 'C': {
      NodePointer type = popNode(Node::Kind::Type);
      return createWithChild(Node::Kind::CoroutineContinuationPrototype, type);
    }
    case 'z':
    case 'Z': {
      NodePointer flagMode = demangleIndexAsNode();
      NodePointer sig = popNode(Node::Kind::DependentGenericSignature);
      NodePointer resultType = popNode(Node::Kind::Type);
      NodePointer implType = popNode(Node::Kind::Type);
      auto node = createWithChildren(c == 'z'
                                  ? Node::Kind::ObjCAsyncCompletionHandlerImpl
                                  : Node::Kind::PredefinedObjCAsyncCompletionHandlerImpl,
                                implType, resultType, flagMode);
      if (sig)
        addChild(node, sig);
      return node;
    }
    case 'V': {
      NodePointer Base = popNode(isEntity);
      NodePointer Derived = popNode(isEntity);
      return createWithChildren(Node::Kind::VTableThunk, Derived, Base);
    }
    case 'W': {
      NodePointer Entity = popNode(isEntity);
      NodePointer Conf = popProtocolConformance();
      return createWithChildren(Node::Kind::ProtocolWitness, Conf, Entity);
    }
    case 'S':
      return createWithChild(Node::Kind::ProtocolSelfConformanceWitness,
                             popNode(isEntity));
    case 'R':
    case 'r':
    case 'y': {
      Node::Kind kind;
      if (c == 'R') kind = Node::Kind::ReabstractionThunkHelper;
      else if (c == 'y') kind = Node::Kind::ReabstractionThunkHelperWithSelf;
      else kind = Node::Kind::ReabstractionThunk;
      NodePointer Thunk = createNode(kind);
      if (NodePointer GenSig = popNode(Node::Kind::DependentGenericSignature))
        addChild(Thunk, GenSig);
      if (kind == Node::Kind::ReabstractionThunkHelperWithSelf)
        addChild(Thunk, popNode(Node::Kind::Type));
      addChild(Thunk, popNode(Node::Kind::Type));
      addChild(Thunk, popNode(Node::Kind::Type));
      return Thunk;
    }
    case 'g':
      return demangleGenericSpecialization(Node::Kind::GenericSpecialization);
    case 'G':
      return demangleGenericSpecialization(Node::Kind::
                                          GenericSpecializationNotReAbstracted);
    case 'B':
      return demangleGenericSpecialization(Node::Kind::
                                      GenericSpecializationInResilienceDomain);
    case 's':
      return demangleGenericSpecialization(
          Node::Kind::GenericSpecializationPrespecialized);
    case 'i':
      return demangleGenericSpecialization(Node::Kind::InlinedGenericFunction);
    case 'p': {
      NodePointer Spec = demangleSpecAttributes(Node::Kind::
                                                GenericPartialSpecialization);
      NodePointer Param = createWithChild(Node::Kind::GenericSpecializationParam,
                                          popNode(Node::Kind::Type));
      return addChild(Spec, Param);
    }
    case'P': {
      NodePointer Spec = demangleSpecAttributes(Node::Kind::
                                  GenericPartialSpecializationNotReAbstracted);
      NodePointer Param = createWithChild(Node::Kind::GenericSpecializationParam,
                                          popNode(Node::Kind::Type));
      return addChild(Spec, Param);
    }
    case'f':
      return demangleFunctionSpecialization();
    case 'K':
    case 'k': {
      auto nodeKind = c == 'K' ? Node::Kind::KeyPathGetterThunkHelper
                               : Node::Kind::KeyPathSetterThunkHelper;

      bool isSerialized = nextIf('q');

      std::vector<NodePointer> types;
      auto node = popNode();
      if (!node || node->getKind() != Node::Kind::Type)
        return nullptr;
      do {
        types.push_back(node);
        node = popNode();
      } while (node && node->getKind() == Node::Kind::Type);
      
      NodePointer result;
      if (node) {
        if (node->getKind() == Node::Kind::DependentGenericSignature) {
          auto decl = popNode();
          if (!decl)
            return nullptr;
          result = createWithChildren(nodeKind, decl, /*sig*/ node);
        } else {
          result = createWithChild(nodeKind, /*decl*/ node);
        }
      } else {
        return nullptr;
      }
      for (auto i = types.rbegin(), e = types.rend(); i != e; ++i) {
        result->addChild(*i, *this);
      }

      if (isSerialized)
        result->addChild(createNode(Node::Kind::IsSerialized), *this);

      return result;
    }
    case 'l': {
      auto assocTypeName = popAssocTypeName();
      if (!assocTypeName)
        return nullptr;

      return createWithChild(Node::Kind::AssociatedTypeDescriptor,
                             assocTypeName);
    }
    case 'L':
      return createWithChild(Node::Kind::ProtocolRequirementsBaseDescriptor,
                             popProtocol());
    case 'M':
      return createWithChild(Node::Kind::DefaultAssociatedTypeMetadataAccessor,
                             popAssocTypeName());

    case 'n': {
      NodePointer requirementTy = popProtocol();
      NodePointer conformingType = popAssocTypePath();
      NodePointer protoTy = popNode(Node::Kind::Type);
      return createWithChildren(Node::Kind::AssociatedConformanceDescriptor,
                                protoTy, conformingType, requirementTy);
    }

    case 'N': {
      NodePointer requirementTy = popProtocol();
      auto assocTypePath = popAssocTypePath();
      NodePointer protoTy = popNode(Node::Kind::Type);
      return createWithChildren(
                            Node::Kind::DefaultAssociatedConformanceAccessor,
                            protoTy, assocTypePath, requirementTy);
    }

    case 'b': {
      NodePointer requirementTy = popProtocol();
      NodePointer protoTy = popNode(Node::Kind::Type);
      return createWithChildren(Node::Kind::BaseConformanceDescriptor,
                                protoTy, requirementTy);
    }

    case 'H':
    case 'h': {
      auto nodeKind = c == 'H' ? Node::Kind::KeyPathEqualsThunkHelper
                               : Node::Kind::KeyPathHashThunkHelper;

      bool isSerialized = nextIf('q');

      NodePointer genericSig = nullptr;
      std::vector<NodePointer> types;
      
      auto node = popNode();
      if (node) {
        if (node->getKind() == Node::Kind::DependentGenericSignature) {
          genericSig = node;
        } else if (node->getKind() == Node::Kind::Type) {
          types.push_back(node);
        } else {
          return nullptr;
        }
      } else {
        return nullptr;
      }
      
      while (auto node = popNode()) {
        if (node->getKind() != Node::Kind::Type) {
          return nullptr;
        }
        types.push_back(node);
      }
      
      NodePointer result = createNode(nodeKind);
      for (auto i = types.rbegin(), e = types.rend(); i != e; ++i) {
        result->addChild(*i, *this);
      }
      if (genericSig)
        result->addChild(genericSig, *this);

      if (isSerialized)
        result->addChild(createNode(Node::Kind::IsSerialized), *this);

      return result;
    }
    case 'v': {
      int Idx = demangleIndex();
      if (Idx < 0)
        return nullptr;
      if (nextChar() == 'r')
        return createNode(Node::Kind::OutlinedReadOnlyObject, Idx);
      return createNode(Node::Kind::OutlinedVariable, Idx);
    }
    case 'e': {
      std::string Params = demangleBridgedMethodParams();
      if (Params.empty())
        return nullptr;
      return createNode(Node::Kind::OutlinedBridgedMethod, Params);
    }
    case 'u': return createNode(Node::Kind::AsyncFunctionPointer);
    case 'U': {
      auto globalActor = popNode(Node::Kind::Type);
      if (!globalActor)
        return nullptr;
      
      auto reabstraction = popNode();
      if (!reabstraction)
        return nullptr;
      
      auto node = createNode(Node::Kind::ReabstractionThunkHelperWithGlobalActor);
      node->addChild(reabstraction, *this);
      node->addChild(globalActor, *this);
      return node;
    }
    case 'J':
      switch (peekChar()) {
      case 'S':
        nextChar();
        return demangleAutoDiffSubsetParametersThunk();
      case 'O':
        nextChar();
        return demangleAutoDiffSelfReorderingReabstractionThunk();
      case 'V':
        nextChar();
        return demangleAutoDiffFunctionOrSimpleThunk(
            Node::Kind::AutoDiffDerivativeVTableThunk);
      default:
        return demangleAutoDiffFunctionOrSimpleThunk(
            Node::Kind::AutoDiffFunction);
      }
    case 'w':
      switch (nextChar()) {
      case 'b': return createNode(Node::Kind::BackDeploymentThunk);
      case 'B': return createNode(Node::Kind::BackDeploymentFallback);
      case 'S': return createNode(Node::Kind::HasSymbolQuery);
      default:
        return nullptr;
      }
    default:
      return nullptr;
  }
}

NodePointer
Demangler::demangleAutoDiffFunctionOrSimpleThunk(Node::Kind nodeKind) {
  auto result = createNode(nodeKind);
  while (auto *originalNode = popNode())
    result = addChild(result, originalNode);
  result->reverseChildren();
  auto kind = demangleAutoDiffFunctionKind();
  result = addChild(result, kind);
  result = addChild(result, demangleIndexSubset());
  if (!nextIf('p'))
    return nullptr;
  result = addChild(result, demangleIndexSubset());
  if (!nextIf('r'))
    return nullptr;
  return result;
}

NodePointer Demangler::demangleAutoDiffFunctionKind() {
  char kind = nextChar();
  if (kind != 'f' && kind != 'r' && kind != 'd' && kind != 'p')
    return nullptr;
  return createNode(Node::Kind::AutoDiffFunctionKind, kind);
}

NodePointer Demangler::demangleAutoDiffSubsetParametersThunk() {
  auto result = createNode(Node::Kind::AutoDiffSubsetParametersThunk);
  while (auto *node = popNode())
    result = addChild(result, node);
  result->reverseChildren();
  auto kind = demangleAutoDiffFunctionKind();
  result = addChild(result, kind);
  result = addChild(result, demangleIndexSubset());
  if (!nextIf('p'))
    return nullptr;
  result = addChild(result, demangleIndexSubset());
  if (!nextIf('r'))
    return nullptr;
  result = addChild(result, demangleIndexSubset());
  if (!nextIf('P'))
    return nullptr;
  return result;
}

NodePointer Demangler::demangleAutoDiffSelfReorderingReabstractionThunk() {
  auto result = createNode(
      Node::Kind::AutoDiffSelfReorderingReabstractionThunk);
  addChild(result, popNode(Node::Kind::DependentGenericSignature));
  result = addChild(result, popNode(Node::Kind::Type));
  result = addChild(result, popNode(Node::Kind::Type));
  if (result)
    result->reverseChildren();
  result = addChild(result, demangleAutoDiffFunctionKind());
  return result;
}

NodePointer Demangler::demangleDifferentiabilityWitness() {
  auto result = createNode(Node::Kind::DifferentiabilityWitness);
  auto optionalGenSig = popNode(Node::Kind::DependentGenericSignature);
  while (auto *node = popNode())
    result = addChild(result, node);
  result->reverseChildren();
  MangledDifferentiabilityKind kind;
  switch (auto c = nextChar()) {
  case 'f': kind = MangledDifferentiabilityKind::Forward; break;
  case 'r': kind = MangledDifferentiabilityKind::Reverse; break;
  case 'd': kind = MangledDifferentiabilityKind::Normal; break;
  case 'l': kind = MangledDifferentiabilityKind::Linear; break;
  default: return nullptr;
  }
  result = addChild(
      result, createNode(Node::Kind::Index, (Node::IndexType)kind));
  result = addChild(result, demangleIndexSubset());
  if (!nextIf('p'))
    return nullptr;
  result = addChild(result, demangleIndexSubset());
  if (!nextIf('r'))
    return nullptr;
  addChild(result, optionalGenSig);
  return result;
}

NodePointer Demangler::demangleIndexSubset() {
  std::string str;
  for (auto c = peekChar(); c == 'S' || c == 'U'; c = peekChar()) {
    str.push_back(c);
    (void)nextChar();
  }
  if (str.empty())
    return nullptr;
  return createNode(Node::Kind::IndexSubset, str);
}

NodePointer Demangler::demangleDifferentiableFunctionType() {
  MangledDifferentiabilityKind kind;
  switch (auto c = nextChar()) {
  case 'f': kind = MangledDifferentiabilityKind::Forward; break;
  case 'r': kind = MangledDifferentiabilityKind::Reverse; break;
  case 'd': kind = MangledDifferentiabilityKind::Normal; break;
  case 'l': kind = MangledDifferentiabilityKind::Linear; break;
  default: return nullptr;
  }
  return createNode(
      Node::Kind::DifferentiableFunctionType, (Node::IndexType)kind);
}

std::string Demangler::demangleBridgedMethodParams() {
  if (nextIf('_'))
    return std::string();

  std::string Str;

  auto kind = nextChar();
  switch (kind) {
  default:
    return std::string();
  case 'o': case 'p': case 'a': case 'm':
    Str.push_back(kind);
  }

  while (!nextIf('_')) {
    auto c = nextChar();
    if (c != 'n' && c != 'b' && c != 'g')
      return std::string();
    Str.push_back(c);
  }
  return Str;
}

NodePointer Demangler::demangleGenericSpecialization(Node::Kind SpecKind) {
  NodePointer Spec = demangleSpecAttributes(SpecKind);
  if (!Spec)
    return nullptr;
  NodePointer TyList = popTypeList();
  if (!TyList)
    return nullptr;
  for (NodePointer Ty : *TyList) {
    Spec->addChild(createWithChild(Node::Kind::GenericSpecializationParam, Ty),
                   *this);
  }
  return Spec;
}

NodePointer Demangler::demangleFunctionSpecialization() {
  NodePointer Spec = demangleSpecAttributes(
        Node::Kind::FunctionSignatureSpecialization);
  while (Spec && !nextIf('_')) {
    Spec = addChild(Spec, demangleFuncSpecParam(Node::Kind::FunctionSignatureSpecializationParam));
  }
  if (!nextIf('n'))
    Spec = addChild(Spec, demangleFuncSpecParam(Node::Kind::FunctionSignatureSpecializationReturn));

  if (!Spec)
    return nullptr;

  // Add the required parameters in reverse order.
  for (size_t Idx = 0, Num = Spec->getNumChildren(); Idx < Num; ++Idx) {
    NodePointer Param = Spec->getChild(Num - Idx - 1);
    if (Param->getKind() != Node::Kind::FunctionSignatureSpecializationParam)
      continue;

    if (Param->getNumChildren() == 0)
      continue;
    NodePointer KindNd = Param->getFirstChild();
    assert(KindNd->getKind() ==
             Node::Kind::FunctionSignatureSpecializationParamKind);
    auto ParamKind = (FunctionSigSpecializationParamKind)KindNd->getIndex();
    switch (ParamKind) {
      case FunctionSigSpecializationParamKind::ConstantPropFunction:
      case FunctionSigSpecializationParamKind::ConstantPropGlobal:
      case FunctionSigSpecializationParamKind::ConstantPropString:
      case FunctionSigSpecializationParamKind::ConstantPropKeyPath:
      case FunctionSigSpecializationParamKind::ClosureProp: {
        size_t FixedChildren = Param->getNumChildren();
        while (NodePointer Ty = popNode(Node::Kind::Type)) {
          if (ParamKind != FunctionSigSpecializationParamKind::ClosureProp &&
              ParamKind != FunctionSigSpecializationParamKind::ConstantPropKeyPath)
            return nullptr;
          Param = addChild(Param, Ty);
        }
        NodePointer Name = popNode(Node::Kind::Identifier);
        if (!Name)
          return nullptr;
        StringRef Text = Name->getText();
        if (ParamKind ==
                FunctionSigSpecializationParamKind::ConstantPropString &&
            !Text.empty() && Text[0] == '_') {
          // A '_' escapes a leading digit or '_' of a string constant.
          Text = Text.drop_front(1);
        }
        addChild(Param, createNodeWithAllocatedText(
          Node::Kind::FunctionSignatureSpecializationParamPayload, Text));
        Param->reverseChildren(FixedChildren);
        break;
      }
      default:
        break;
    }
  }
  return Spec;
}

NodePointer Demangler::demangleFuncSpecParam(Node::Kind Kind) {
  assert(Kind == Node::Kind::FunctionSignatureSpecializationParam ||
         Kind == Node::Kind::FunctionSignatureSpecializationReturn);
  NodePointer Param = createNode(Kind);
  switch (nextChar()) {
    case 'n':
      return Param;
    case 'c':
      // Consumes an identifier and multiple type parameters.
      // The parameters will be added later.
      return addChild(Param, createNode(
        Node::Kind::FunctionSignatureSpecializationParamKind,
        uint64_t(FunctionSigSpecializationParamKind::ClosureProp)));
    case 'p': {
      switch (nextChar()) {
        case 'f':
          // Consumes an identifier parameter, which will be added later.
          return addChild(
              Param,
              createNode(Node::Kind::FunctionSignatureSpecializationParamKind,
                         Node::IndexType(FunctionSigSpecializationParamKind::
                                             ConstantPropFunction)));
        case 'g':
          // Consumes an identifier parameter, which will be added later.
          return addChild(
              Param,
              createNode(
                  Node::Kind::FunctionSignatureSpecializationParamKind,
                  Node::IndexType(
                      FunctionSigSpecializationParamKind::ConstantPropGlobal)));
        case 'i':
          return addFuncSpecParamNumber(Param,
                    FunctionSigSpecializationParamKind::ConstantPropInteger);
        case 'd':
          return addFuncSpecParamNumber(Param,
                      FunctionSigSpecializationParamKind::ConstantPropFloat);
        case 's': {
          // Consumes an identifier parameter (the string constant),
          // which will be added later.
          const char *Encoding = nullptr;
          switch (nextChar()) {
            case 'b': Encoding = "u8"; break;
            case 'w': Encoding = "u16"; break;
            case 'c': Encoding = "objc"; break;
            default: return nullptr;
          }
          addChild(Param,
                   createNode(
                       Node::Kind::FunctionSignatureSpecializationParamKind,
                       Node::IndexType(
                           swift::Demangle::FunctionSigSpecializationParamKind::
                               ConstantPropString)));
          return addChild(Param, createNode(
                  Node::Kind::FunctionSignatureSpecializationParamPayload,
                  Encoding));
        }
        case 'k': {
          // Consumes two types and a SHA1 identifier.
          return addChild(
              Param,
              createNode(Node::Kind::FunctionSignatureSpecializationParamKind,
                         Node::IndexType(FunctionSigSpecializationParamKind::
                                             ConstantPropKeyPath)));
        }
        default:
          return nullptr;
      }
    }
    case 'e': {
      unsigned Value =
          unsigned(FunctionSigSpecializationParamKind::ExistentialToGeneric);
      if (nextIf('D'))
        Value |= unsigned(FunctionSigSpecializationParamKind::Dead);
      if (nextIf('G'))
        Value |=
            unsigned(FunctionSigSpecializationParamKind::OwnedToGuaranteed);
      if (nextIf('O'))
        Value |=
            unsigned(FunctionSigSpecializationParamKind::GuaranteedToOwned);
      if (nextIf('X'))
        Value |= unsigned(FunctionSigSpecializationParamKind::SROA);
      return addChild(
          Param,
          createNode(Node::Kind::FunctionSignatureSpecializationParamKind,
                     Value));
    }
    case 'd': {
      unsigned Value = unsigned(FunctionSigSpecializationParamKind::Dead);
      if (nextIf('G'))
        Value |= unsigned(FunctionSigSpecializationParamKind::OwnedToGuaranteed);
      if (nextIf('O'))
        Value |=
            unsigned(FunctionSigSpecializationParamKind::GuaranteedToOwned);
      if (nextIf('X'))
        Value |= unsigned(FunctionSigSpecializationParamKind::SROA);
      return addChild(Param, createNode(
                  Node::Kind::FunctionSignatureSpecializationParamKind, Value));
    }
    case 'g': {
      unsigned Value = unsigned(FunctionSigSpecializationParamKind::
                                OwnedToGuaranteed);
      if (nextIf('X'))
        Value |= unsigned(FunctionSigSpecializationParamKind::SROA);
      return addChild(Param, createNode(
                  Node::Kind::FunctionSignatureSpecializationParamKind, Value));
    }
    case 'o': {
      unsigned Value =
          unsigned(FunctionSigSpecializationParamKind::GuaranteedToOwned);
      if (nextIf('X'))
        Value |= unsigned(FunctionSigSpecializationParamKind::SROA);
      return addChild(
          Param,
          createNode(Node::Kind::FunctionSignatureSpecializationParamKind,
                     Value));
    }
    case 'x':
      return addChild(Param, createNode(
                Node::Kind::FunctionSignatureSpecializationParamKind,
                unsigned(FunctionSigSpecializationParamKind::SROA)));
    case 'i':
      return addChild(Param, createNode(
                Node::Kind::FunctionSignatureSpecializationParamKind,
                unsigned(FunctionSigSpecializationParamKind::BoxToValue)));
    case 's':
      return addChild(Param, createNode(
                Node::Kind::FunctionSignatureSpecializationParamKind,
                unsigned(FunctionSigSpecializationParamKind::BoxToStack)));
    case 'r':
      return addChild(
          Param,
          createNode(Node::Kind::FunctionSignatureSpecializationParamKind,
                     unsigned(FunctionSigSpecializationParamKind::InOutToOut)));
    default:
      return nullptr;
  }
}

NodePointer Demangler::addFuncSpecParamNumber(NodePointer Param,
                                    FunctionSigSpecializationParamKind Kind) {
  Param->addChild(createNode(
        Node::Kind::FunctionSignatureSpecializationParamKind, unsigned(Kind)),
        *this);
  CharVector Str;
  while (isDigit(peekChar())) {
    Str.push_back(nextChar(), *this);
  }
  if (Str.empty())
    return nullptr;
  return addChild(Param, createNode(
     Node::Kind::FunctionSignatureSpecializationParamPayload, Str));
}

NodePointer Demangler::demangleSpecAttributes(Node::Kind SpecKind) {
  bool metatypeParamsRemoved = nextIf('m');
  bool isSerialized = nextIf('q');

  int PassID = (int)nextChar() - '0';
  if (PassID < 0 || PassID > 9)
    return nullptr;

  NodePointer SpecNd = createNode(SpecKind);

  if (metatypeParamsRemoved)
    SpecNd->addChild(createNode(Node::Kind::MetatypeParamsRemoved), *this);

  if (isSerialized)
    SpecNd->addChild(createNode(Node::Kind::IsSerialized),
                     *this);

  SpecNd->addChild(createNode(Node::Kind::SpecializationPassID, PassID),
                   *this);
  return SpecNd;
}

NodePointer Demangler::demangleWitness() {
  switch (char c = nextChar()) {
    case 'C':
      return createWithChild(Node::Kind::EnumCase,
                             popNode(isEntity));
    case 'V':
      return createWithChild(Node::Kind::ValueWitnessTable,
                             popNode(Node::Kind::Type));
    case 'v': {
      unsigned Directness;
      switch (nextChar()) {
        case 'd': Directness = unsigned(Directness::Direct); break;
        case 'i': Directness = unsigned(Directness::Indirect); break;
        default: return nullptr;
      }
      return createWithChildren(Node::Kind::FieldOffset,
                        createNode(Node::Kind::Directness, Directness),
                        popNode(isEntity));
    }
    case 'S':
      return createWithChild(Node::Kind::ProtocolSelfConformanceWitnessTable,
                             popProtocol());
    case 'P':
      return createWithChild(Node::Kind::ProtocolWitnessTable,
                             popProtocolConformance());
    case 'p':
      return createWithChild(Node::Kind::ProtocolWitnessTablePattern,
                             popProtocolConformance());
    case 'G':
      return createWithChild(Node::Kind::GenericProtocolWitnessTable,
                             popProtocolConformance());
    case 'I':
      return createWithChild(
                  Node::Kind::GenericProtocolWitnessTableInstantiationFunction,
                  popProtocolConformance());

    case 'r':
      return createWithChild(Node::Kind::ResilientProtocolWitnessTable,
                             popProtocolConformance());

    case 'l': {
      NodePointer Conf = popProtocolConformance();
      NodePointer Type = popNode(Node::Kind::Type);
      return createWithChildren(Node::Kind::LazyProtocolWitnessTableAccessor,
                                Type, Conf);
    }
    case 'L': {
      NodePointer Conf = popProtocolConformance();
      NodePointer Type = popNode(Node::Kind::Type);
      return createWithChildren(
               Node::Kind::LazyProtocolWitnessTableCacheVariable, Type, Conf);
    }
    case 'a':
      return createWithChild(Node::Kind::ProtocolWitnessTableAccessor,
                             popProtocolConformance());
    case 't': {
      NodePointer Name = popNode(isDeclName);
      NodePointer Conf = popProtocolConformance();
      return createWithChildren(Node::Kind::AssociatedTypeMetadataAccessor,
                                Conf, Name);
    }
    case 'T': {
      NodePointer ProtoTy = popNode(Node::Kind::Type);
      NodePointer ConformingType = popAssocTypePath();
      NodePointer Conf = popProtocolConformance();
      return createWithChildren(Node::Kind::AssociatedTypeWitnessTableAccessor,
                                Conf, ConformingType, ProtoTy);
    }
    case 'b': {
      NodePointer ProtoTy = popNode(Node::Kind::Type);
      NodePointer Conf = popProtocolConformance();
      return createWithChildren(Node::Kind::BaseWitnessTableAccessor,
                                Conf, ProtoTy);
    }
    case 'O': {
      switch (nextChar()) {
      case 'y': {
        if (auto sig = popNode(Node::Kind::DependentGenericSignature))
          return createWithChildren(Node::Kind::OutlinedCopy,
                                    popNode(Node::Kind::Type), sig);
        return createWithChild(Node::Kind::OutlinedCopy,
                               popNode(Node::Kind::Type));
      }
      case 'e': {
        if (auto sig = popNode(Node::Kind::DependentGenericSignature))
          return createWithChildren(Node::Kind::OutlinedConsume,
                                    popNode(Node::Kind::Type), sig);
        return createWithChild(Node::Kind::OutlinedConsume,
                               popNode(Node::Kind::Type));
      }
      case 'r': {
        if (auto sig = popNode(Node::Kind::DependentGenericSignature))
          return createWithChildren(Node::Kind::OutlinedRetain,
                                    popNode(Node::Kind::Type), sig);
        return createWithChild(Node::Kind::OutlinedRetain,
                               popNode(Node::Kind::Type));
      }
      case 's': {
        if (auto sig = popNode(Node::Kind::DependentGenericSignature))
          return createWithChildren(Node::Kind::OutlinedRelease,
                                    popNode(Node::Kind::Type), sig);
        return createWithChild(Node::Kind::OutlinedRelease,
                               popNode(Node::Kind::Type));
      }
      case 'b': {
        if (auto sig = popNode(Node::Kind::DependentGenericSignature))
          return createWithChildren(Node::Kind::OutlinedInitializeWithTake,
                                    popNode(Node::Kind::Type), sig);
        return createWithChild(Node::Kind::OutlinedInitializeWithTake,
                               popNode(Node::Kind::Type));
      }
      case 'c': {
        if (auto sig = popNode(Node::Kind::DependentGenericSignature))
          return createWithChildren(Node::Kind::OutlinedInitializeWithCopy,
                                    popNode(Node::Kind::Type), sig);
        return createWithChild(Node::Kind::OutlinedInitializeWithCopy,
                               popNode(Node::Kind::Type));
      }
      case 'd': {
        if (auto sig = popNode(Node::Kind::DependentGenericSignature))
          return createWithChildren(Node::Kind::OutlinedAssignWithTake,
                                    popNode(Node::Kind::Type), sig);
        return createWithChild(Node::Kind::OutlinedAssignWithTake,
                               popNode(Node::Kind::Type));
      }
      case 'f': {
        if (auto sig = popNode(Node::Kind::DependentGenericSignature))
          return createWithChildren(Node::Kind::OutlinedAssignWithCopy,
                                    popNode(Node::Kind::Type), sig);
        return createWithChild(Node::Kind::OutlinedAssignWithCopy,
                               popNode(Node::Kind::Type));
      }
      case 'h': {
        if (auto sig = popNode(Node::Kind::DependentGenericSignature))
          return createWithChildren(Node::Kind::OutlinedDestroy,
                                    popNode(Node::Kind::Type), sig);
        return createWithChild(Node::Kind::OutlinedDestroy,
                               popNode(Node::Kind::Type));
      }
      default:
        return nullptr;
      }
    }
    case 'Z':
    case 'z': {
      auto declList = createNode(Node::Kind::GlobalVariableOnceDeclList);
      std::vector<NodePointer> vars;
      while (auto sig = popNode(Node::Kind::FirstElementMarker)) {
        auto identifier = popNode(isDeclName);
        if (!identifier)
          return nullptr;
        vars.push_back(identifier);
      }
      for (auto i = vars.rbegin(); i != vars.rend(); ++i) {
        declList->addChild(*i, *this);
      }
      
      auto context = popContext();
      if (!context)
        return nullptr;
      Node::Kind kind = c == 'Z'
        ? Node::Kind::GlobalVariableOnceFunction
        : Node::Kind::GlobalVariableOnceToken;
      return createWithChildren(kind,
                                context,
                                declList);
    }
    case 'J':
      return demangleDifferentiabilityWitness();
    default:
      return nullptr;
  }
}

NodePointer Demangler::demangleSpecialType() {
  switch (auto specialChar = nextChar()) {
    case 'E':
      return popFunctionType(Node::Kind::NoEscapeFunctionType);
    case 'A':
      return popFunctionType(Node::Kind::EscapingAutoClosureType);
    case 'f':
      return popFunctionType(Node::Kind::ThinFunctionType);
    case 'K':
      return popFunctionType(Node::Kind::AutoClosureType);
    case 'U':
      return popFunctionType(Node::Kind::UncurriedFunctionType);
    case 'L':
      return popFunctionType(Node::Kind::EscapingObjCBlock);
    case 'B':
      return popFunctionType(Node::Kind::ObjCBlock);
    case 'C':
      return popFunctionType(Node::Kind::CFunctionPointer);
    case 'g':
    case 'G':
      return demangleExtendedExistentialShape(specialChar);
    case 'j':
      return demangleSymbolicExtendedExistentialType();
    case 'z':
      switch (auto cchar = nextChar()) {
      case 'B':
        return popFunctionType(Node::Kind::ObjCBlock, true);
      case 'C':
        return popFunctionType(Node::Kind::CFunctionPointer, true);
      default:
        return nullptr;
      }
    case 'o':
      return createType(createWithChild(Node::Kind::Unowned,
                                        popNode(Node::Kind::Type)));
    case 'u':
      return createType(createWithChild(Node::Kind::Unmanaged,
                                        popNode(Node::Kind::Type)));
    case 'w':
      return createType(createWithChild(Node::Kind::Weak,
                                        popNode(Node::Kind::Type)));
    case 'b':
      return createType(createWithChild(Node::Kind::SILBoxType,
                                        popNode(Node::Kind::Type)));
    case 'D':
      return createType(createWithChild(Node::Kind::DynamicSelf,
                                        popNode(Node::Kind::Type)));
    case 'M': {
      NodePointer MTR = demangleMetatypeRepresentation();
      NodePointer Type = popNode(Node::Kind::Type);
      return createType(createWithChildren(Node::Kind::Metatype, MTR, Type));
    }
    case 'm': {
      NodePointer MTR = demangleMetatypeRepresentation();
      NodePointer Type = popNode(Node::Kind::Type);
      return createType(createWithChildren(Node::Kind::ExistentialMetatype,
                                           MTR, Type));
    }
    case 'P': {
      NodePointer Reqs = demangleConstrainedExistentialRequirementList();
      NodePointer Base = popNode(Node::Kind::Type);
      return createType(
          createWithChildren(Node::Kind::ConstrainedExistential, Base, Reqs));
    }
    case 'p':
      return createType(createWithChild(Node::Kind::ExistentialMetatype,
                                        popNode(Node::Kind::Type)));
    case 'c': {
      NodePointer Superclass = popNode(Node::Kind::Type);
      NodePointer Protocols = demangleProtocolList();
      return createType(createWithChildren(Node::Kind::ProtocolListWithClass,
                                           Protocols, Superclass));
    }
    case 'l': {
      NodePointer Protocols = demangleProtocolList();
      return createType(createWithChild(Node::Kind::ProtocolListWithAnyObject,
                                        Protocols));
    }
    case 'X':
    case 'x': {
      // SIL box types.
      NodePointer signature = nullptr, genericArgs = nullptr;
      if (specialChar == 'X') {
        signature = popNode(Node::Kind::DependentGenericSignature);
        if (!signature)
          return nullptr;
        genericArgs = popTypeList();
        if (!genericArgs)
          return nullptr;
      }
      
      auto fieldTypes = popTypeList();
      if (!fieldTypes)
        return nullptr;
      // Build layout.
      auto layout = createNode(Node::Kind::SILBoxLayout);
      for (size_t i = 0, e = fieldTypes->getNumChildren(); i < e; ++i) {
        auto fieldType = fieldTypes->getChild(i);
        assert(fieldType->getKind() == Node::Kind::Type);
        bool isMutable = false;
        // 'inout' typelist mangling is used to represent mutable fields.
        if (fieldType->getChild(0)->getKind() == Node::Kind::InOut) {
          isMutable = true;
          fieldType = createType(fieldType->getChild(0)->getChild(0));
        }
        auto field = createNode(isMutable
                                         ? Node::Kind::SILBoxMutableField
                                         : Node::Kind::SILBoxImmutableField);
        field->addChild(fieldType, *this);
        layout->addChild(field, *this);
      }
      auto boxTy = createNode(Node::Kind::SILBoxTypeWithLayout);
      boxTy->addChild(layout, *this);
      if (signature) {
        boxTy->addChild(signature, *this);
        assert(genericArgs);
        boxTy->addChild(genericArgs, *this);
      }
      return createType(boxTy);
    }
    case 'Y':
      return demangleAnyGenericType(Node::Kind::OtherNominalType);
    case 'Z': {
      auto types = popTypeList();
      auto name = popNode(Node::Kind::Identifier);
      auto parent = popContext();
      auto anon = createNode(Node::Kind::AnonymousContext);
      anon = addChild(anon, name);
      anon = addChild(anon, parent);
      anon = addChild(anon, types);
      return anon;
    }
    case 'e':
      return createType(createNode(Node::Kind::ErrorType));
    case 'S':
      // Sugared type for debugger.
      switch (nextChar()) {
      case 'q':
        return createType(createWithChild(Node::Kind::SugaredOptional,
                                          popNode(Node::Kind::Type)));
      case 'a':
        return createType(createWithChild(Node::Kind::SugaredArray,
                                          popNode(Node::Kind::Type)));
      case 'D': {
        NodePointer value = popNode(Node::Kind::Type);
        NodePointer key = popNode(Node::Kind::Type);
        return createType(createWithChildren(Node::Kind::SugaredDictionary,
                                             key, value));
      }
      case 'p':
        return createType(createWithChild(Node::Kind::SugaredParen,
                                          popNode(Node::Kind::Type)));
      default:
        return nullptr;
      }
    default:
      return nullptr;
  }
}

NodePointer Demangler::demangleSymbolicExtendedExistentialType() {
  NodePointer retroactiveConformances = popRetroactiveConformances();

  NodePointer args = createNode(Node::Kind::TypeList);
  while (NodePointer ty = popNode(Node::Kind::Type)) {
    args->addChild(ty, *this);
  }
  args->reverseChildren();

  auto shape = popNode();
  if (!shape)
    return nullptr;
  if (shape->getKind() !=
        Node::Kind::UniqueExtendedExistentialTypeShapeSymbolicReference &&
      shape->getKind() !=
        Node::Kind::NonUniqueExtendedExistentialTypeShapeSymbolicReference)
    return nullptr;

  NodePointer existentialType;
  if (!retroactiveConformances) {
    existentialType =
      createWithChildren(Node::Kind::SymbolicExtendedExistentialType,
                         shape, args);
  } else {
    existentialType =
      createWithChildren(Node::Kind::SymbolicExtendedExistentialType,
                         shape, args, retroactiveConformances);
  }
  return createType(existentialType);
}

NodePointer Demangler::demangleExtendedExistentialShape(char nodeKind) {
  assert(nodeKind == 'g' || nodeKind == 'G');

  NodePointer type = popNode(Node::Kind::Type);

  NodePointer genSig = nullptr;
  if (nodeKind == 'G')
    genSig = popNode(Node::Kind::DependentGenericSignature);

  if (genSig) {
    return createWithChildren(Node::Kind::ExtendedExistentialTypeShape,
                              genSig, type);
  } else {
    return createWithChild(Node::Kind::ExtendedExistentialTypeShape, type);
  }
}

NodePointer Demangler::demangleMetatypeRepresentation() {
  switch (nextChar()) {
    case 't':
      return createNode(Node::Kind::MetatypeRepresentation, "@thin");
    case 'T':
      return createNode(Node::Kind::MetatypeRepresentation, "@thick");
    case 'o':
      return createNode(Node::Kind::MetatypeRepresentation,
                                 "@objc_metatype");
    default:
      return nullptr;
  }
}

NodePointer Demangler::demangleAccessor(NodePointer ChildNode) {
  Node::Kind Kind;
  switch (nextChar()) {
    case 'm': Kind = Node::Kind::MaterializeForSet; break;
    case 's': Kind = Node::Kind::Setter; break;
    case 'g': Kind = Node::Kind::Getter; break;
    case 'G': Kind = Node::Kind::GlobalGetter; break;
    case 'w': Kind = Node::Kind::WillSet; break;
    case 'W': Kind = Node::Kind::DidSet; break;
    case 'r': Kind = Node::Kind::ReadAccessor; break;
    case 'M': Kind = Node::Kind::ModifyAccessor; break;
    case 'i': Kind = Node::Kind::InitAccessor; break;
    case 'a':
      switch (nextChar()) {
        case 'O': Kind = Node::Kind::OwningMutableAddressor; break;
        case 'o': Kind = Node::Kind::NativeOwningMutableAddressor; break;
        case 'P': Kind = Node::Kind::NativePinningMutableAddressor; break;
        case 'u': Kind = Node::Kind::UnsafeMutableAddressor; break;
        default: return nullptr;
      }
      break;
    case 'l':
      switch (nextChar()) {
        case 'O': Kind = Node::Kind::OwningAddressor; break;
        case 'o': Kind = Node::Kind::NativeOwningAddressor; break;
        case 'p': Kind = Node::Kind::NativePinningAddressor; break;
        case 'u': Kind = Node::Kind::UnsafeAddressor; break;
        default: return nullptr;
      }
      break;
    case 'p': // Pseudo-accessor referring to the variable/subscript itself
      return ChildNode;
    default: return nullptr;
  }
  NodePointer Entity = createWithChild(Kind, ChildNode);
  return Entity;
}

NodePointer Demangler::demangleFunctionEntity() {
  enum {
    None,
    TypeAndMaybePrivateName,
    TypeAndIndex,
    Index,
    ContextArg,
  } Args;

  Node::Kind Kind = Node::Kind::EmptyList;
  switch (nextChar()) {
    case 'D': Args = None; Kind = Node::Kind::Deallocator; break;
    case 'd': Args = None; Kind = Node::Kind::Destructor; break;
    case 'E': Args = None; Kind = Node::Kind::IVarDestroyer; break;
    case 'e': Args = None; Kind = Node::Kind::IVarInitializer; break;
    case 'i': Args = None; Kind = Node::Kind::Initializer; break;
    case 'C':
      Args = TypeAndMaybePrivateName; Kind = Node::Kind::Allocator; break;
    case 'c':
      Args = TypeAndMaybePrivateName; Kind = Node::Kind::Constructor; break;
    case 'U': Args = TypeAndIndex; Kind = Node::Kind::ExplicitClosure; break;
    case 'u': Args = TypeAndIndex; Kind = Node::Kind::ImplicitClosure; break;
    case 'A': Args = Index; Kind = Node::Kind::DefaultArgumentInitializer; break;
    case 'a':
      Args = ContextArg;
      Kind = Node::Kind::RuntimeAttributeGenerator;
      break;
    case 'm': return demangleEntity(Node::Kind::Macro);
    case 'M': return demangleMacroExpansion();
    case 'p': return demangleEntity(Node::Kind::GenericTypeParamDecl);
    case 'P':
      Args = None;
      Kind = Node::Kind::PropertyWrapperBackingInitializer;
      break;
    case 'W':
      Args = None;
      Kind = Node::Kind::PropertyWrapperInitFromProjectedValue;
      break;
    default: return nullptr;
  }

  NodePointer NameOrIndex = nullptr, ParamType = nullptr, LabelList = nullptr,
              Context = nullptr;
  switch (Args) {
    case None:
      break;
    case TypeAndMaybePrivateName:
      NameOrIndex = popNode(Node::Kind::PrivateDeclName);
      ParamType = popNode(Node::Kind::Type);
      LabelList = popFunctionParamLabels(ParamType);
      break;
    case TypeAndIndex:
      NameOrIndex = demangleIndexAsNode();
      ParamType = popNode(Node::Kind::Type);
      break;
    case Index:
      NameOrIndex = demangleIndexAsNode();
      break;
    case ContextArg:
      Context = popNode();
      break;
  }
  NodePointer Entity = createWithChild(Kind, popContext());
  switch (Args) {
    case None:
      break;
    case Index:
      Entity = addChild(Entity, NameOrIndex);
      break;
    case TypeAndMaybePrivateName:
      addChild(Entity, LabelList);
      Entity = addChild(Entity, ParamType);
      addChild(Entity, NameOrIndex);
      break;
    case TypeAndIndex:
      Entity = addChild(Entity, NameOrIndex);
      Entity = addChild(Entity, ParamType);
      break;
    case ContextArg:
      Entity = addChild(Entity, Context);
      break;
  }
  return Entity;
}

NodePointer Demangler::demangleEntity(Node::Kind Kind) {
  NodePointer Type = popNode(Node::Kind::Type);
  NodePointer LabelList = popFunctionParamLabels(Type);
  NodePointer Name = popNode(isDeclName);
  NodePointer Context = popContext();
  auto result = LabelList ? createWithChildren(Kind, Context, Name, LabelList, Type)
                          : createWithChildren(Kind, Context, Name, Type);
                          
  result = setParentForOpaqueReturnTypeNodes(*this, result, Type);
  return result;
}

NodePointer Demangler::demangleVariable() {
  NodePointer Variable = demangleEntity(Node::Kind::Variable);
  return demangleAccessor(Variable);
}

NodePointer Demangler::demangleSubscript() {
  NodePointer PrivateName = popNode(Node::Kind::PrivateDeclName);
  NodePointer Type = popNode(Node::Kind::Type);
  NodePointer LabelList = popFunctionParamLabels(Type);
  NodePointer Context = popContext();

  if (!Type)
    return nullptr;

  NodePointer Subscript = createNode(Node::Kind::Subscript);
  Subscript = addChild(Subscript, Context);
  addChild(Subscript, LabelList);
  Subscript = addChild(Subscript, Type);
  addChild(Subscript, PrivateName);
  
  Subscript = setParentForOpaqueReturnTypeNodes(*this, Subscript, Type);

  return demangleAccessor(Subscript);
}

NodePointer Demangler::demangleProtocolList() {
  NodePointer TypeList = createNode(Node::Kind::TypeList);
  NodePointer ProtoList = createWithChild(Node::Kind::ProtocolList, TypeList);
  if (!popNode(Node::Kind::EmptyList)) {
    bool firstElem = false;
    do {
      firstElem = (popNode(Node::Kind::FirstElementMarker) != nullptr);
      NodePointer Proto = popProtocol();
      if (!Proto)
        return nullptr;
      TypeList->addChild(Proto, *this);
    } while (!firstElem);

    TypeList->reverseChildren();
  }
  return ProtoList;
}

NodePointer Demangler::demangleProtocolListType() {
  NodePointer ProtoList = demangleProtocolList();
  return createType(ProtoList);
}

NodePointer Demangler::demangleConstrainedExistentialRequirementList() {
  NodePointer ReqList =
      createNode(Node::Kind::ConstrainedExistentialRequirementList);
  bool firstElem = false;
  do {
    firstElem = (popNode(Node::Kind::FirstElementMarker) != nullptr);
    NodePointer Req = popNode(isRequirement);
    if (!Req)
      return nullptr;
    ReqList->addChild(Req, *this);
  } while (!firstElem);

  ReqList->reverseChildren();
  return ReqList;
}

NodePointer Demangler::demangleGenericSignature(bool hasParamCounts) {
  NodePointer Sig = createNode(Node::Kind::DependentGenericSignature);
  if (hasParamCounts) {
    while (!nextIf('l')) {
      int count = 0;
      if (!nextIf('z'))
        count = demangleIndex() + 1;
      if (count < 0)
        return nullptr;
      Sig->addChild(createNode(Node::Kind::DependentGenericParamCount,
                                        count), *this);
    }
  } else {
    Sig->addChild(createNode(Node::Kind::DependentGenericParamCount, 1),
                  *this);
  }
  size_t NumCounts = Sig->getNumChildren();
  while (NodePointer Req = popNode(isRequirement)) {
    Sig->addChild(Req, *this);
  }
  Sig->reverseChildren(NumCounts);
  return Sig;
}

NodePointer Demangler::demangleGenericRequirement() {

  enum { Generic, Assoc, CompoundAssoc, Substitution } TypeKind;
  enum { Protocol, BaseClass, SameType, SameShape, Layout, PackMarker } ConstraintKind;

  switch (nextChar()) {
    case 'v': ConstraintKind = PackMarker; TypeKind = Generic; break;
    case 'c': ConstraintKind = BaseClass; TypeKind = Assoc; break;
    case 'C': ConstraintKind = BaseClass; TypeKind = CompoundAssoc; break;
    case 'b': ConstraintKind = BaseClass; TypeKind = Generic; break;
    case 'B': ConstraintKind = BaseClass; TypeKind = Substitution; break;
    case 't': ConstraintKind = SameType; TypeKind = Assoc; break;
    case 'T': ConstraintKind = SameType; TypeKind = CompoundAssoc; break;
    case 's': ConstraintKind = SameType; TypeKind = Generic; break;
    case 'S': ConstraintKind = SameType; TypeKind = Substitution; break;
    case 'm': ConstraintKind = Layout; TypeKind = Assoc; break;
    case 'M': ConstraintKind = Layout; TypeKind = CompoundAssoc; break;
    case 'l': ConstraintKind = Layout; TypeKind = Generic; break;
    case 'L': ConstraintKind = Layout; TypeKind = Substitution; break;
    case 'p': ConstraintKind = Protocol; TypeKind = Assoc; break;
    case 'P': ConstraintKind = Protocol; TypeKind = CompoundAssoc; break;
    case 'Q': ConstraintKind = Protocol; TypeKind = Substitution; break;
    case 'h': ConstraintKind = SameShape; TypeKind = Generic; break;
    default:  ConstraintKind = Protocol; TypeKind = Generic; pushBack(); break;
  }

  NodePointer ConstrTy = nullptr;

  switch (TypeKind) {
  case Generic:
    ConstrTy = createType(demangleGenericParamIndex());
    break;
  case Assoc:
    ConstrTy = demangleAssociatedTypeSimple(demangleGenericParamIndex());
    addSubstitution(ConstrTy);
    break;
  case CompoundAssoc:
    ConstrTy = demangleAssociatedTypeCompound(demangleGenericParamIndex());
    addSubstitution(ConstrTy);
    break;
  case Substitution:
    ConstrTy = popNode(Node::Kind::Type);
    break;
  }

  switch (ConstraintKind) {
  case PackMarker:
    return createWithChild(
        Node::Kind::DependentGenericParamPackMarker, ConstrTy);
  case Protocol:
    return createWithChildren(
        Node::Kind::DependentGenericConformanceRequirement, ConstrTy,
        popProtocol());
  case BaseClass:
    return createWithChildren(
        Node::Kind::DependentGenericConformanceRequirement, ConstrTy,
        popNode(Node::Kind::Type));
  case SameType:
    return createWithChildren(Node::Kind::DependentGenericSameTypeRequirement,
                              ConstrTy, popNode(Node::Kind::Type));
  case SameShape:
    return createWithChildren(Node::Kind::DependentGenericSameShapeRequirement,
                              ConstrTy, popNode(Node::Kind::Type));
  case Layout: {
    auto c = nextChar();
    NodePointer size = nullptr;
    NodePointer alignment = nullptr;
    const char *name = nullptr;
    if (c == 'U') {
      name = "U";
    } else if (c == 'R') {
      name = "R";
    } else if (c == 'N') {
      name = "N";
    } else if (c == 'C') {
      name = "C";
    } else if (c == 'D') {
      name = "D";
    } else if (c == 'T') {
      name = "T";
    } else if (c == 'E') {
      size = demangleIndexAsNode();
      if (!size)
        return nullptr;
      alignment = demangleIndexAsNode();
      name = "E";
    } else if (c == 'e') {
      size = demangleIndexAsNode();
      if (!size)
        return nullptr;
      name = "e";
    } else if (c == 'M') {
      size = demangleIndexAsNode();
      if (!size)
        return nullptr;
      alignment = demangleIndexAsNode();
      name = "M";
    } else if (c == 'm') {
      size = demangleIndexAsNode();
      if (!size)
        return nullptr;
      name = "m";
    } else {
      // Unknown layout constraint.
      return nullptr;
    }

    auto NameNode = createNode(Node::Kind::Identifier, name);
    auto NodeKind = Node::Kind::DependentGenericLayoutRequirement;
    auto LayoutRequirement = createWithChildren(NodeKind, ConstrTy, NameNode);
    if (size)
      addChild(LayoutRequirement, size);
    if (alignment)
      addChild(LayoutRequirement, alignment);
    return LayoutRequirement;
  }
  }
  return nullptr;
}

NodePointer Demangler::demangleGenericType() {
  NodePointer GenSig = popNode(Node::Kind::DependentGenericSignature);
  NodePointer Ty = popNode(Node::Kind::Type);
  return createType(createWithChildren(Node::Kind::DependentGenericType,
                                       GenSig, Ty));
}

static int decodeValueWitnessKind(StringRef CodeStr) {
#define VALUE_WITNESS(MANGLING, NAME) \
  if (CodeStr == #MANGLING) return (int)ValueWitnessKind::NAME;
#include "swift/Demangling/ValueWitnessMangling.def"
  return -1;
}

NodePointer Demangler::demangleValueWitness() {
  char Code[2];
  Code[0] = nextChar();
  Code[1] = nextChar();
  int Kind = decodeValueWitnessKind(StringRef(Code, 2));
  if (Kind < 0)
    return nullptr;
  NodePointer VW = createNode(Node::Kind::ValueWitness);
  addChild(VW, createNode(Node::Kind::Index, unsigned(Kind)));
  return addChild(VW, popNode(Node::Kind::Type));
}

static bool isMacroExpansionNodeKind(Node::Kind kind) {
  return kind == Node::Kind::AccessorAttachedMacroExpansion ||
         kind == Node::Kind::MemberAttributeAttachedMacroExpansion ||
         kind == Node::Kind::FreestandingMacroExpansion ||
         kind == Node::Kind::MemberAttachedMacroExpansion ||
         kind == Node::Kind::PeerAttachedMacroExpansion ||
         kind == Node::Kind::ConformanceAttachedMacroExpansion;
}

NodePointer Demangler::demangleMacroExpansion() {
  Node::Kind kind;
  bool isAttached;
  bool isFreestanding;
  switch (nextChar()) {
  case 'a':
    kind = Node::Kind::AccessorAttachedMacroExpansion;
    isAttached = true;
    isFreestanding = false;
    break;

  case 'r':
    kind = Node::Kind::MemberAttributeAttachedMacroExpansion;
    isAttached = true;
    isFreestanding = false;
    break;

  case 'f':
    kind = Node::Kind::FreestandingMacroExpansion;
    isAttached = false;
    isFreestanding = true;
    break;

  case 'm':
    kind = Node::Kind::MemberAttachedMacroExpansion;
    isAttached = true;
    isFreestanding = false;
    break;

  case 'p':
    kind = Node::Kind::PeerAttachedMacroExpansion;
    isAttached = true;
    isFreestanding = false;
    break;

  case 'c':
    kind = Node::Kind::ConformanceAttachedMacroExpansion;
    isAttached = true;
    isFreestanding = false;
    break;

  case 'u':
    kind = Node::Kind::MacroExpansionUniqueName;
    isAttached = false;
    isFreestanding = false;
    break;

  default:
    return nullptr;
  }

  NodePointer macroName = popNode(Node::Kind::Identifier);
  NodePointer privateDiscriminator = nullptr;
  if (isFreestanding)
    privateDiscriminator = popNode(Node::Kind::PrivateDeclName);
  NodePointer attachedName = nullptr;
  if (isAttached)
    attachedName = popNode(isDeclName);

  NodePointer context = popNode(isMacroExpansionNodeKind);
  if (!context)
    context = popContext();
  NodePointer discriminator = demangleIndexAsNode();
  NodePointer result;
  if (isAttached) {
    result = createWithChildren(
        kind, context, attachedName, macroName, discriminator);
  } else {
    result = createWithChildren(kind, context, macroName, discriminator);
  }
  if (privateDiscriminator)
    result->addChild(privateDiscriminator, *this);
  return result;
}
