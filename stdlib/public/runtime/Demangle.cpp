//===----------------------------------------------------------------------===//
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

#include "Private.h"
#include "swift/ABI/TypeIdentity.h"
#include "swift/Basic/Range.h"
#include "swift/Runtime/Metadata.h"
#include "swift/Runtime/Portability.h"
#include "swift/Strings.h"

#include <vector>
#include <inttypes.h>

#if SWIFT_OBJC_INTEROP
#include <objc/runtime.h>
#endif

using namespace swift;

Demangle::NodePointer
swift::_buildDemanglingForContext(const ContextDescriptor *context,
                                  llvm::ArrayRef<NodePointer> demangledGenerics,
                                  Demangle::Demangler &Dem) {
  unsigned usedDemangledGenerics = 0;
  NodePointer node = nullptr;

  // Walk up the context tree.
  llvm::SmallVector<const ContextDescriptor *, 8> descriptorPath;
  {
    const ContextDescriptor *parent = context;
    while (parent) {
      descriptorPath.push_back(parent);
      parent = parent->Parent;
    }
  }

  auto getGenericArgsTypeListForContext =
    [&](const ContextDescriptor *context) -> NodePointer {
      if (demangledGenerics.empty())
        return nullptr;

      if (context->getKind() == ContextDescriptorKind::Anonymous)
        return nullptr;
      
      auto generics = context->getGenericContext();
      if (!generics)
        return nullptr;
      
      auto numParams = generics->getGenericContextHeader().NumParams;
      if (numParams <= usedDemangledGenerics)
        return nullptr;
      
      auto genericArgsList = Dem.createNode(Node::Kind::TypeList);
      for (unsigned e = generics->getGenericContextHeader().NumParams;
           usedDemangledGenerics < e;
           ++usedDemangledGenerics) {
        genericArgsList->addChild(demangledGenerics[usedDemangledGenerics],
                                  Dem);
      }
      return genericArgsList;
    };
  
  for (auto component : llvm::reverse(descriptorPath)) {
    switch (auto kind = component->getKind()) {
    case ContextDescriptorKind::Module: {
      assert(node == nullptr && "module should be top level");
      auto name = llvm::cast<ModuleContextDescriptor>(component)->Name.get();
      node = Dem.createNode(Node::Kind::Module, name);
      break;
    }
    
    case ContextDescriptorKind::Extension: {
      auto extension = llvm::cast<ExtensionContextDescriptor>(component);
      // Demangle the extension self type.
      auto selfType = Dem.demangleType(extension->getMangledExtendedContext(),
                                       ResolveToDemanglingForContext(Dem));
      if (selfType->getKind() == Node::Kind::Type)
        selfType = selfType->getChild(0);
      
      // Substitute in the generic arguments.
      auto genericArgsList = getGenericArgsTypeListForContext(component);
      
      if (selfType->getKind() == Node::Kind::BoundGenericEnum
          || selfType->getKind() == Node::Kind::BoundGenericStructure
          || selfType->getKind() == Node::Kind::BoundGenericClass
          || selfType->getKind() == Node::Kind::BoundGenericOtherNominalType) {
        if (genericArgsList) {
          auto substSelfType = Dem.createNode(selfType->getKind());
          substSelfType->addChild(selfType->getChild(0), Dem);
          substSelfType->addChild(genericArgsList, Dem);
          selfType = substSelfType;
        } else {
          // TODO: Use the unsubstituted type if we can't handle the
          // substitutions yet.
          selfType = selfType->getChild(0)->getChild(0);
        }
      }
      
      auto extNode = Dem.createNode(Node::Kind::Extension);
      extNode->addChild(node, Dem);
      extNode->addChild(selfType, Dem);
      
      // TODO: Turn the generic signature into a demangling as the third
      // generic argument.
      
      node = extNode;
      break;
    }

    case ContextDescriptorKind::Protocol: {
      auto protocol = llvm::cast<ProtocolDescriptor>(component);
      auto name = protocol->Name.get();

      auto protocolNode = Dem.createNode(Node::Kind::Protocol);
      protocolNode->addChild(node, Dem);
      auto nameNode = Dem.createNode(Node::Kind::Identifier, name);
      protocolNode->addChild(nameNode, Dem);

      node = protocolNode;
      break;
    }

    default:
      // Form a type context demangling for type contexts.
      if (auto type = llvm::dyn_cast<TypeContextDescriptor>(component)) {
        auto identity = ParsedTypeIdentity::parse(type);

        Node::Kind nodeKind;
        Node::Kind genericNodeKind;
        switch (kind) {
        case ContextDescriptorKind::Class:
          nodeKind = Node::Kind::Class;
          genericNodeKind = Node::Kind::BoundGenericClass;
          break;
        case ContextDescriptorKind::Struct:
          nodeKind = Node::Kind::Structure;
          genericNodeKind = Node::Kind::BoundGenericStructure;
          break;
        case ContextDescriptorKind::Enum:
          nodeKind = Node::Kind::Enum;
          genericNodeKind = Node::Kind::BoundGenericEnum;
          break;
        default:
          // We don't know about this kind of type. Use an "other type" mangling
          // for it.
          nodeKind = Node::Kind::OtherNominalType;
          genericNodeKind = Node::Kind::BoundGenericOtherNominalType;
          break;
        }
        
        // Override the node kind if this is a Clang-imported type so we give it
        // a stable mangling.
        if (identity.isCTypedef()) {
          nodeKind = Node::Kind::TypeAlias;
        } else if (nodeKind != Node::Kind::Structure &&
                   _isCImportedTagType(type, identity)) {
          nodeKind = Node::Kind::Structure;
        }
        
        auto typeNode = Dem.createNode(nodeKind);
        typeNode->addChild(node, Dem);
        auto nameNode = Dem.createNode(Node::Kind::Identifier,
                                       identity.getABIName());
        if (identity.isAnyRelatedEntity()) {
          auto kindNode = Dem.createNode(Node::Kind::Identifier,
                                     identity.getRelatedEntityName());
          auto relatedName = Dem.createNode(Node::Kind::RelatedEntityDeclName);
          relatedName->addChild(kindNode, Dem);
          relatedName->addChild(nameNode, Dem);
          nameNode = relatedName;
        }
        typeNode->addChild(nameNode, Dem);
        node = typeNode;
        
        // Apply generic arguments if the context is generic.
        if (auto genericArgsList = getGenericArgsTypeListForContext(component)){
          auto unspecializedType = Dem.createNode(Node::Kind::Type);
          unspecializedType->addChild(node, Dem);

          auto genericNode = Dem.createNode(genericNodeKind);
          genericNode->addChild(unspecializedType, Dem);
          genericNode->addChild(genericArgsList, Dem);
          node = genericNode;
        }
        
        break;
      }

      // This runtime doesn't understand this context, or it's a context with
      // no richer runtime information available about it (such as an anonymous
      // context). Use an unstable mangling to represent the context by its
      // pointer identity.
      char addressBuf[sizeof(void*) * 2 + 1 + 1];
      snprintf(addressBuf, sizeof(addressBuf), "$%" PRIxPTR, (uintptr_t)component);
      
      auto anonNode = Dem.createNode(Node::Kind::AnonymousContext);
      CharVector addressStr;
      addressStr.append(addressBuf, Dem);
      auto name = Dem.createNode(Node::Kind::Identifier, addressStr);
      anonNode->addChild(name, Dem);
      anonNode->addChild(node, Dem);
      
      // Collect generic arguments if the context is generic.
      auto genericArgsList = getGenericArgsTypeListForContext(component);
      if (!genericArgsList)
        genericArgsList = Dem.createNode(Node::Kind::TypeList);
      anonNode->addChild(genericArgsList, Dem);
      
      node = anonNode;
      
      break;
    }
  }
  
  // Wrap the final result in a top-level Type node.
  auto top = Dem.createNode(Node::Kind::Type);
  top->addChild(node, Dem);
  return top;
}

// FIXME: This stuff should be merged with the existing logic in
// include/swift/RemoteInspection/TypeRefBuilder.h as part of the rewrite
// to change stdlib reflection over to using remote mirrors.

Demangle::NodePointer
swift::_swift_buildDemanglingForMetadata(const Metadata *type,
                                         Demangle::Demangler &Dem);

static Demangle::NodePointer
_buildDemanglerForBuiltinType(const Metadata *type, Demangle::Demangler &Dem) {
#define BUILTIN_TYPE(Symbol, Name) \
  if (type == &METADATA_SYM(Symbol).base) \
    return Dem.createNode(Node::Kind::BuiltinTypeName, Name);
#if !SWIFT_STDLIB_ENABLE_VECTOR_TYPES
#define BUILTIN_VECTOR_TYPE(ElementSymbol, ElementName, Width)
#endif
#include "swift/Runtime/BuiltinTypes.def"
  return nullptr;
}

// Build a demangled type tree for a type pack.
static Demangle::NodePointer
_buildDemanglingForMetadataPack(MetadataPackPointer pack, size_t count,
                                Demangle::Demangler &Dem) {
  using namespace Demangle;

  auto node = Dem.createNode(Node::Kind::Pack);

  for (size_t i = 0; i < count; ++i) {
    auto elt = _swift_buildDemanglingForMetadata(pack.getElements()[i], Dem);
    if (elt == nullptr)
      return nullptr;

    node->addChild(elt, Dem);
  }

  return node;
}

/// Demangle the given type name to a generic parameter reference, which
/// will be returned as (depth, index).
static llvm::Optional<std::pair<unsigned, unsigned>>
demangleToGenericParamRef(StringRef typeName) {
  StackAllocatedDemangler<1024> demangler;
  NodePointer node = demangler.demangleType(typeName);
  if (!node)
    return None;

  // Find the flat index that the right-hand side refers to.
  if (node->getKind() == Demangle::Node::Kind::Type)
    node = node->getChild(0);
  if (node->getKind() != Demangle::Node::Kind::DependentGenericParamType)
    return None;

  return std::pair<unsigned, unsigned>(node->getChild(0)->getIndex(),
                                       node->getChild(1)->getIndex());
}

/// Build an array of demangling trees for each generic argument of the given
/// type metadata.
///
/// Note:
/// - The input array has an entry for those generic parameter descriptors which
///   are key arguments only.
/// - The output array has an entry for each generic parameter descriptor,
///   whether or not it is a key argument.
///
/// The generic parameters which are not key arguments were made non-canonical
/// by constraining them to a concrete type or another generic parameter.
///
/// We figure out their type by looking at the same-type requirements of the
/// generic context. We demangle their type from the requirement, using the
/// generic arguments area as the substitution map; this gives us the metadata
/// for their argument. Then we convert the metadata to a mangling.
///
/// The output array is flat; the generic parameters of each depth are
/// concatenated together.
static bool _buildDemanglingForGenericArgs(
    const Metadata *type,
    const TypeContextDescriptor *description,
    llvm::SmallVectorImpl<NodePointer> &demangledGenerics,
    Demangle::Demangler &Dem) {
  auto generics = description->getGenericContext();
  if (!generics)
    return true;

  auto genericArgs = description->getGenericArguments(type);

  auto packHeader = generics->getGenericPackShapeHeader();
  auto packDescriptors = generics->getGenericPackShapeDescriptors();

  unsigned packIndex = 0;
  unsigned argIndex = packHeader.NumShapeClasses;

  bool missingWrittenArguments = false;

  for (auto param : generics->getGenericParams()) {
    switch (param.getKind()) {
    case GenericParamKind::Type:
      // The type should have a key argument unless it's been same-typed to
      // another type.
      if (param.hasKeyArgument()) {
        auto genericArg = reinterpret_cast<const Metadata *>(genericArgs[argIndex]);

        auto genericArgDemangling =
          _swift_buildDemanglingForMetadata(genericArg, Dem);
        if (!genericArgDemangling)
          return false;
        demangledGenerics.push_back(genericArgDemangling);

        ++argIndex;
      } else {
        // Leave a gap for us to fill in by looking at same-type requirements.
        demangledGenerics.push_back(nullptr);
        missingWrittenArguments = true;
      }

      break;

    case GenericParamKind::TypePack:
      if (param.hasKeyArgument()) {
        auto packDescriptor = packDescriptors[packIndex];
        assert(packDescriptor.Kind == GenericPackKind::Metadata);
        assert(packDescriptor.ShapeClass < packHeader.NumShapeClasses);
        assert(packDescriptor.Index == argIndex);

        MetadataPackPointer pack(genericArgs[argIndex]);
        size_t count = reinterpret_cast<size_t>(genericArgs[packDescriptor.ShapeClass]);

        auto genericArgDemangling = _buildDemanglingForMetadataPack(pack, count, Dem);
        if (genericArgDemangling == nullptr)
          return false;

        demangledGenerics.push_back(genericArgDemangling);

        ++packIndex;
        ++argIndex;
      } else {
        // Leave a gap for us to fill in by looking at same-type requirements.
        demangledGenerics.push_back(nullptr);
        missingWrittenArguments = true;
      }

      break;

    default:
      // We don't know about this kind of parameter.
      return false;
    }
  }

  // If there is no follow-up work to do, we're done.
  if (!missingWrittenArguments)
    return true;

  // We have generic arguments that would be written, but have been
  // canonicalized away. Use same-type requirements to reconstitute them.
  SubstGenericParametersFromMetadata substitutions(
        description, reinterpret_cast<const void * const *>(genericArgs));

  // Retrieve the mapping information needed for depth/index -> flat index.
  llvm::SmallVector<unsigned, 8> genericParamCounts;
  (void)_gatherGenericParameterCounts(description, genericParamCounts, Dem);

  // Walk through the generic requirements to evaluate same-type
  // constraints that are needed to fill in missing generic arguments.
  for (const auto &req : generics->getGenericRequirements()) {
    // We only care about same-type constraints.
    if (req.Flags.getKind() != GenericRequirementKind::SameType)
      continue;

    auto lhsParam = demangleToGenericParamRef(req.getParam());
    if (!lhsParam)
      continue;

    assert(!req.Flags.isPackRequirement() &&
           "Pack requirements not supported here yet");

    // If we don't yet have an argument for this parameter, it's a
    // same-type-to-concrete constraint.
    auto lhsFlatIndex =
      _depthIndexToFlatIndex(lhsParam->first, lhsParam->second,
                             genericParamCounts);
    if (!lhsFlatIndex || *lhsFlatIndex >= demangledGenerics.size())
      return false;

    if (!demangledGenerics[*lhsFlatIndex]) {
      // Substitute into the right-hand side.
      auto *genericArg =
          swift_getTypeByMangledName(MetadataState::Abstract,
            req.getMangledTypeName(),
            reinterpret_cast<const void * const *>(genericArgs),
            [&substitutions](unsigned depth, unsigned index) {
              // FIXME: Variadic generics
              return substitutions.getMetadata(depth, index).getMetadata();
            },
            [&substitutions](const Metadata *type, unsigned index) {
              return substitutions.getWitnessTable(type, index);
            }).getType().getMetadata();
      if (!genericArg)
        return false;

      demangledGenerics[*lhsFlatIndex] =
          _swift_buildDemanglingForMetadata(genericArg, Dem);

      continue;
    }

    // If we do have an argument for this parameter, it might be that
    // the right-hand side is itself a generic parameter, which means
    // we have a same-type constraint A == B where A is already filled in.
    auto rhsParam = demangleToGenericParamRef(req.getMangledTypeName());
    if (!rhsParam)
      return false;

    auto rhsFlatIndex =
      _depthIndexToFlatIndex(rhsParam->first, rhsParam->second,
                             genericParamCounts);
    if (!rhsFlatIndex || *rhsFlatIndex >= demangledGenerics.size())
      return false;

    if (demangledGenerics[*rhsFlatIndex] || !demangledGenerics[*lhsFlatIndex])
      return false;

    demangledGenerics[*rhsFlatIndex] = demangledGenerics[*lhsFlatIndex];
  }

  return true;
}

/// Build a demangled type tree for a nominal type.
static Demangle::NodePointer
_buildDemanglingForNominalType(const Metadata *type, Demangle::Demangler &Dem) {
  using namespace Demangle;

  // Get the context descriptor from the type metadata.
  const TypeContextDescriptor *description;

  switch (type->getKind()) {
  case MetadataKind::Class: {
    auto classType = static_cast<const ClassMetadata *>(type);
#if SWIFT_OBJC_INTEROP
    // Peek through artificial subclasses.
    while (classType->isTypeMetadata() && classType->isArtificialSubclass())
      classType = classType->Superclass;
#endif
    description = classType->getDescription();
    break;
  }
  case MetadataKind::Enum:
  case MetadataKind::Optional: {
    auto enumType = static_cast<const EnumMetadata *>(type);
    description = enumType->Description;
    break;
  }
  case MetadataKind::Struct: {
    auto structType = static_cast<const StructMetadata *>(type);
    description = structType->Description;
    break;
  }
  case MetadataKind::ForeignClass: {
    auto foreignType = static_cast<const ForeignClassMetadata *>(type);
    description = foreignType->Description;
    break;
  }
  case MetadataKind::ForeignReferenceType: {
    auto foreignType = static_cast<const ForeignReferenceTypeMetadata *>(type);
    description = foreignType->Description;
    break;
  }
  default:
    return nullptr;
  }

  // Gather the complete set of generic arguments that must be written to
  // form this type.
  llvm::SmallVector<NodePointer, 8> demangledGenerics;
  if (!_buildDemanglingForGenericArgs(type, description, demangledGenerics, Dem))
    return nullptr;
  
  return _buildDemanglingForContext(description, demangledGenerics, Dem);
}

// Build a demangled type tree for a type.
//
// FIXME: This should use MetadataReader.h.
Demangle::NodePointer
swift::_swift_buildDemanglingForMetadata(const Metadata *type,
                                         Demangle::Demangler &Dem) {
  using namespace Demangle;

  switch (type->getKind()) {
  case MetadataKind::Class:
  case MetadataKind::Enum:
  case MetadataKind::Optional:
  case MetadataKind::Struct:
  case MetadataKind::ForeignClass:
  case MetadataKind::ForeignReferenceType:
    return _buildDemanglingForNominalType(type, Dem);
  case MetadataKind::ObjCClassWrapper: {
#if SWIFT_OBJC_INTEROP
    auto objcWrapper = static_cast<const ObjCClassWrapperMetadata *>(type);
    const char *className = class_getName(objcWrapper->getObjCClassObject());
    
    auto module = Dem.createNode(Node::Kind::Module, MANGLING_MODULE_OBJC);
    auto node = Dem.createNode(Node::Kind::Class);
    node->addChild(module, Dem);
    node->addChild(Dem.createNode(Node::Kind::Identifier,
                                       llvm::StringRef(className)), Dem);
    
    return node;
#else
    assert(false && "no ObjC interop");
    return nullptr;
#endif
  }
  case MetadataKind::Existential: {
    auto exis = static_cast<const ExistentialTypeMetadata *>(type);
    auto protocols = exis->getProtocols();

    auto type_list = Dem.createNode(Node::Kind::TypeList);
    auto proto_list = Dem.createNode(Node::Kind::ProtocolList);
    proto_list->addChild(type_list, Dem);

    // The protocol descriptors should be pre-sorted since the compiler will
    // only ever make a swift_getExistentialTypeMetadata invocation using
    // its canonical ordering of protocols.

    for (auto protocol : protocols) {
#if SWIFT_OBJC_INTEROP
      if (protocol.isObjC()) {
        // The protocol name is mangled as a type symbol, with the _Tt prefix.
        StringRef ProtoName(protocol.getName());
        NodePointer protocolNode = Dem.demangleSymbol(ProtoName);

        // ObjC protocol names aren't mangled.
        if (!protocolNode) {
          auto module = Dem.createNode(Node::Kind::Module,
                                            MANGLING_MODULE_OBJC);
          auto node = Dem.createNode(Node::Kind::Protocol);
          node->addChild(module, Dem);
          node->addChild(Dem.createNode(Node::Kind::Identifier, ProtoName),
                         Dem);
          auto typeNode = Dem.createNode(Node::Kind::Type);
          typeNode->addChild(node, Dem);
          type_list->addChild(typeNode, Dem);
          continue;
        }

        // Dig out the protocol node.
        // Global -> (Protocol|TypeMangling)
        protocolNode = protocolNode->getChild(0);
        if (protocolNode->getKind() == Node::Kind::TypeMangling) {
          protocolNode = protocolNode->getChild(0); // TypeMangling -> Type
          protocolNode = protocolNode->getChild(0); // Type -> ProtocolList
          protocolNode = protocolNode->getChild(0); // ProtocolList -> TypeList
          protocolNode = protocolNode->getChild(0); // TypeList -> Type

          assert(protocolNode->getKind() == Node::Kind::Type);
          assert(protocolNode->getChild(0)->getKind() == Node::Kind::Protocol);
        } else {
          assert(protocolNode->getKind() == Node::Kind::Protocol);
        }

        type_list->addChild(protocolNode, Dem);
        continue;
      }
#endif

      auto protocolNode =
          _buildDemanglingForContext(protocol.getSwiftProtocol(), { }, Dem);
      if (!protocolNode)
        return nullptr;

      type_list->addChild(protocolNode, Dem);
    }

    if (auto superclass = exis->getSuperclassConstraint()) {
      // If there is a superclass constraint, we mangle it specially.
      auto result = Dem.createNode(Node::Kind::ProtocolListWithClass);
      auto superclassNode = _swift_buildDemanglingForMetadata(superclass, Dem);

      result->addChild(proto_list, Dem);
      result->addChild(superclassNode, Dem);
      return result;
    }

    if (exis->isClassBounded()) {
      // Check if the class constraint is implied by any of our
      // protocols.
      bool requiresClassImplicit = false;

      for (auto protocol : protocols) {
        if (protocol.getClassConstraint() == ProtocolClassConstraint::Class)
          requiresClassImplicit = true;
      }

      // If it was implied, we don't do anything special.
      if (requiresClassImplicit)
        return proto_list;

      // If the existential type has an explicit AnyObject constraint,
      // we must mangle it as such.
      auto result = Dem.createNode(Node::Kind::ProtocolListWithAnyObject);
      result->addChild(proto_list, Dem);
      return result;
    }

    // Just a simple composition of protocols.
    return proto_list;
  }
  case MetadataKind::ExtendedExistential: {
    // FIXME: Implement this by demangling the extended existential and
    // substituting the generalization arguments into the demangle tree.
    // For now, unconditional casts will report '<<< invalid type >>>' when
    // they fail.
    // TODO: for clients that need to guarantee round-tripping, demangle
    // to a SymbolicExtendedExistentialType.
    return nullptr;
  }
  case MetadataKind::ExistentialMetatype: {
    auto metatype = static_cast<const ExistentialMetatypeMetadata *>(type);
    auto instance = _swift_buildDemanglingForMetadata(metatype->InstanceType,
                                                      Dem);
    auto node = Dem.createNode(Node::Kind::ExistentialMetatype);
    node->addChild(instance, Dem);
    return node;
  }
  case MetadataKind::Function: {
    auto func = static_cast<const FunctionTypeMetadata *>(type);

    Node::Kind kind;
    switch (func->getConvention()) {
    case FunctionMetadataConvention::Swift:
      if (!func->isEscaping())
        kind = Node::Kind::NoEscapeFunctionType;
      else
        kind = Node::Kind::FunctionType;
      break;
    case FunctionMetadataConvention::Block:
      kind = Node::Kind::ObjCBlock;
      break;
    case FunctionMetadataConvention::CFunctionPointer:
      kind = Node::Kind::CFunctionPointer;
      break;
    case FunctionMetadataConvention::Thin:
      kind = Node::Kind::ThinFunctionType;
      break;
    }

    llvm::SmallVector<std::pair<NodePointer, bool>, 8> inputs;
    for (unsigned i = 0, e = func->getNumParameters(); i < e; ++i) {
      auto param = func->getParameter(i);
      auto flags = func->getParameterFlags(i);
      auto input = _swift_buildDemanglingForMetadata(param, Dem);

      auto wrapInput = [&](Node::Kind kind) {
        auto parent = Dem.createNode(kind);
        parent->addChild(input, Dem);
        input = parent;
      };
      if (flags.isNoDerivative()) {
        wrapInput(Node::Kind::NoDerivative);
      }
      switch (flags.getValueOwnership()) {
      case ValueOwnership::Default:
        /* nothing */
        break;
      case ValueOwnership::InOut:
        wrapInput(Node::Kind::InOut);
        break;
      case ValueOwnership::Shared:
        wrapInput(Node::Kind::Shared);
        break;
      case ValueOwnership::Owned:
        wrapInput(Node::Kind::Owned);
        break;
      }
      if (flags.isIsolated()) {
        wrapInput(Node::Kind::Isolated);
      }

      inputs.push_back({input, flags.isVariadic()});
    }

    NodePointer totalInput = nullptr;
    switch (inputs.size()) {
    case 1: {
      auto singleParam = inputs.front();

      // If the sole unlabeled parameter has a non-tuple type, encode
      // the parameter list as a single type.
      if (!singleParam.second) {
        auto singleType = singleParam.first;
        if (singleType->getKind() == Node::Kind::Type)
          singleType = singleType->getFirstChild();
        if (singleType->getKind() != Node::Kind::Tuple) {
          totalInput = singleParam.first;
          break;
        }
      }

      // Otherwise it requires a tuple wrapper.
      SWIFT_FALLTHROUGH;
    }

    // This covers both none and multiple parameters.
    default:
      auto tuple = Dem.createNode(Node::Kind::Tuple);
      for (auto &input : inputs) {
        NodePointer eltType;
        bool isVariadic;
        std::tie(eltType, isVariadic) = input;

        // Tuple element := variadic-marker label? type
        auto tupleElt = Dem.createNode(Node::Kind::TupleElement);

        if (isVariadic)
          tupleElt->addChild(Dem.createNode(Node::Kind::VariadicMarker), Dem);

        if (eltType->getKind() == Node::Kind::Type) {
          tupleElt->addChild(eltType, Dem);
        } else {
          auto type = Dem.createNode(Node::Kind::Type);
          type->addChild(eltType, Dem);
          tupleElt->addChild(type, Dem);
        }

        tuple->addChild(tupleElt, Dem);
      }
      totalInput = tuple;
      break;
    }

    NodePointer parameters = Dem.createNode(Node::Kind::ArgumentTuple);
    NodePointer paramType = Dem.createNode(Node::Kind::Type);

    paramType->addChild(totalInput, Dem);
    parameters->addChild(paramType, Dem);

    NodePointer resultTy = _swift_buildDemanglingForMetadata(func->ResultType,
                                                             Dem);
    NodePointer result = Dem.createNode(Node::Kind::ReturnType);
    result->addChild(resultTy, Dem);
    
    auto funcNode = Dem.createNode(kind);
    if (func->hasGlobalActor()) {
      auto globalActorTypeNode =
          _swift_buildDemanglingForMetadata(func->getGlobalActor(), Dem);
      NodePointer globalActorNode =
          Dem.createNode(Node::Kind::GlobalActorFunctionType);
      globalActorNode->addChild(globalActorTypeNode, Dem);
      funcNode->addChild(globalActorNode, Dem);
    }
    switch (func->getDifferentiabilityKind().Value) {
    case FunctionMetadataDifferentiabilityKind::NonDifferentiable:
      break;
    case FunctionMetadataDifferentiabilityKind::Forward:
      funcNode->addChild(Dem.createNode(
          Node::Kind::DifferentiableFunctionType,
          (Node::IndexType)MangledDifferentiabilityKind::Forward), Dem);
      break;
    case FunctionMetadataDifferentiabilityKind::Reverse:
      funcNode->addChild(Dem.createNode(
          Node::Kind::DifferentiableFunctionType,
          (Node::IndexType)MangledDifferentiabilityKind::Reverse), Dem);
      break;
    case FunctionMetadataDifferentiabilityKind::Normal:
      funcNode->addChild(Dem.createNode(
          Node::Kind::DifferentiableFunctionType,
          (Node::IndexType)MangledDifferentiabilityKind::Normal), Dem);
      break;
    case FunctionMetadataDifferentiabilityKind::Linear:
      funcNode->addChild(Dem.createNode(
          Node::Kind::DifferentiableFunctionType,
          (Node::IndexType)MangledDifferentiabilityKind::Linear), Dem);
      break;
    }
    if (func->isThrowing())
      funcNode->addChild(Dem.createNode(Node::Kind::ThrowsAnnotation), Dem);
    if (func->isSendable()) {
      funcNode->addChild(
          Dem.createNode(Node::Kind::ConcurrentFunctionType), Dem);
    }
    if (func->isAsync())
      funcNode->addChild(Dem.createNode(Node::Kind::AsyncAnnotation), Dem);

    funcNode->addChild(parameters, Dem);
    funcNode->addChild(result, Dem);
    return funcNode;
  }
  case MetadataKind::Metatype: {
    auto metatype = static_cast<const MetatypeMetadata *>(type);
    auto instance = _swift_buildDemanglingForMetadata(metatype->InstanceType,
                                                      Dem);
    if (!instance)
      return nullptr;

    auto typeNode = Dem.createNode(Node::Kind::Type);
    typeNode->addChild(instance, Dem);
    auto node = Dem.createNode(Node::Kind::Metatype);
    node->addChild(typeNode, Dem);
    return node;
  }
  case MetadataKind::Tuple: {
    auto tuple = static_cast<const TupleTypeMetadata *>(type);
    const char *labels = tuple->Labels;
    auto tupleNode = Dem.createNode(Node::Kind::Tuple);
    for (unsigned i = 0, e = tuple->NumElements; i < e; ++i) {
      auto elt = Dem.createNode(Node::Kind::TupleElement);

      // Add a label child if applicable:
      if (labels) {
        // Look for the next space in the labels string.
        if (const char *space = strchr(labels, ' ')) {
          // If there is one, and the label isn't empty, add a label child.
          if (labels != space) {
            auto eltName =
              Dem.createNode(Node::Kind::TupleElementName,
                                  llvm::StringRef(labels, space - labels));
            elt->addChild(eltName, Dem);
          }

          // Skip past the space.
          labels = space + 1;
        }
      }

      // Add the element type child.
      auto eltType =
        _swift_buildDemanglingForMetadata(tuple->getElement(i).Type, Dem);

      if (eltType->getKind() == Node::Kind::Type) {
        elt->addChild(eltType, Dem);
      } else {
        auto type = Dem.createNode(Node::Kind::Type);
        type->addChild(eltType, Dem);
        elt->addChild(type, Dem);
      }

      // Add the completed element to the tuple.
      tupleNode->addChild(elt, Dem);
    }
    return tupleNode;
  }
  case MetadataKind::HeapLocalVariable:
  case MetadataKind::HeapGenericLocalVariable:
  case MetadataKind::ErrorObject:
    break;
  case MetadataKind::Opaque:
  default: {
    if (auto builtinType = _buildDemanglerForBuiltinType(type, Dem))
      return builtinType;
    
    // FIXME: Some opaque types do have manglings, but we don't have enough info
    // to figure them out.
    break;
  }
  }
  // Not a type.
  return nullptr;
}

// NB: This function is not used directly in the Swift codebase, but is
// exported for Xcode support and is used by the sanitizers. Please coordinate
// before changing.
//
/// Demangles a Swift symbol name.
///
/// \param mangledName is the symbol name that needs to be demangled.
/// \param mangledNameLength is the length of the string that should be
/// demangled.
/// \param outputBuffer is the user provided buffer where the demangled name
/// will be placed. If nullptr, a new buffer will be malloced. In that case,
/// the user of this API is responsible for freeing the returned buffer.
/// \param outputBufferSize is the size of the output buffer. If the demangled
/// name does not fit into the outputBuffer, the output will be truncated and
/// the size will be updated, indicating how large the buffer should be.
/// \param flags can be used to select the demangling style. TODO: We should
//// define what these will be.
/// \returns the demangled name. Returns nullptr if the input String is not a
/// Swift mangled name.
SWIFT_RUNTIME_EXPORT
char *swift_demangle(const char *mangledName,
                     size_t mangledNameLength,
                     char *outputBuffer,
                     size_t *outputBufferSize,
                     uint32_t flags) {
  if (flags != 0) {
    swift::fatalError(0, "Only 'flags' value of '0' is currently supported.");
  }
  if (outputBuffer != nullptr && outputBufferSize == nullptr) {
    swift::fatalError(0, "'outputBuffer' is passed but the size is 'nullptr'.");
  }

  // Check if we are dealing with Swift mangled name, otherwise, don't try
  // to demangle and send indication to the user.
  if (!Demangle::isSwiftSymbol(mangledName))
    return nullptr; // Not a mangled name

#if !SWIFT_STDLIB_HAS_TYPE_PRINTING
  return nullptr;
#else
  // Demangle the name.
  auto options = Demangle::DemangleOptions();
  options.DisplayDebuggerGeneratedModule = false;
  auto result =
      Demangle::demangleSymbolAsString(mangledName,
                                       mangledNameLength,
                                       options);

  // If the output buffer is not provided, malloc memory ourselves.
  if (outputBuffer == nullptr || *outputBufferSize == 0) {
    return strdup(result.c_str());
  }

  // Copy into the provided buffer.
  _swift_strlcpy(outputBuffer, result.c_str(), *outputBufferSize);

  // Indicate a failure if the result did not fit and was truncated
  // by setting the required outputBufferSize.
  if (*outputBufferSize < result.length() + 1) {
    *outputBufferSize = result.length() + 1;
  }

  return outputBuffer;
#endif
}
