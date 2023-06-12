//===--- MetadataLookup.cpp - Swift Language Type Name Lookup -------------===//
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
// Implementations of runtime functions for looking up a type by name.
//
//===----------------------------------------------------------------------===//

#include "../CompatibilityOverride/CompatibilityOverride.h"
#include "ImageInspection.h"
#include "Private.h"
#include "swift/ABI/TypeIdentity.h"
#include "swift/Basic/Lazy.h"
#include "swift/Demangling/Demangler.h"
#include "swift/Demangling/TypeDecoder.h"
#include "swift/RemoteInspection/Records.h"
#include "swift/Runtime/Casting.h"
#include "swift/Runtime/Concurrent.h"
#include "swift/Runtime/Debug.h"
#include "swift/Runtime/EnvironmentVariables.h"
#include "swift/Runtime/HeapObject.h"
#include "swift/Runtime/Metadata.h"
#include "swift/Strings.h"
#include "swift/Threading/Mutex.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/StringExtras.h"
#include <cctype>
#include <cstring>
#include <functional>
#include <list>
#include <new>
#include <vector>

using namespace swift;
using namespace Demangle;
using namespace reflection;

#if SWIFT_OBJC_INTEROP
#include <objc/runtime.h>
#include <objc/message.h>
#include <objc/objc.h>
#include <dlfcn.h>
#endif

/// A Demangler suitable for resolving runtime type metadata strings.
template <class Base = Demangler>
class DemanglerForRuntimeTypeResolution : public Base {
public:
  using Base::demangleSymbol;
  using Base::demangleType;

  // Force callers to explicitly pass `nullptr` to demangleSymbol or
  // demangleType if they don't want to demangle symbolic references.
  NodePointer demangleSymbol(StringRef symbolName) = delete;
  NodePointer demangleType(StringRef typeName) = delete;

  NodePointer demangleTypeRef(StringRef symbolName) {
    // Resolve symbolic references to type contexts into the absolute address of
    // the type context descriptor, so that if we see a symbolic reference in
    // the mangled name we can immediately find the associated metadata.
    return Base::demangleType(symbolName,
                              ResolveAsSymbolicReference(*this));
  }
};

/// Resolve the relative reference in a mangled symbolic reference.
static uintptr_t resolveSymbolicReferenceOffset(SymbolicReferenceKind kind,
                                                Directness isIndirect,
                                                int32_t offset,
                                                const void *base) {
  uintptr_t ptr;
  // Function references may be resolved differently than other data references.
  switch (kind) {
  case SymbolicReferenceKind::AccessorFunctionReference:
    ptr = (uintptr_t)TargetCompactFunctionPointer<InProcess, void>::resolve(base, offset);
    break;
  default:
    ptr = detail::applyRelativeOffset(base, offset);
    break;
  }

  // Indirect references may be authenticated in a way appropriate for the
  // referent.
  if (isIndirect == Directness::Indirect) {
    switch (kind) {
    case SymbolicReferenceKind::Context: {
      ContextDescriptor *contextPtr =
        *(const TargetSignedContextPointer<InProcess> *)ptr;
      return (uintptr_t)contextPtr;
    }
    case SymbolicReferenceKind::UniqueExtendedExistentialTypeShape:
    case SymbolicReferenceKind::NonUniqueExtendedExistentialTypeShape:
    case SymbolicReferenceKind::AccessorFunctionReference: {
      swift_unreachable("should not be indirectly referenced");
    }
    }
    swift_unreachable("unknown symbolic reference kind");
  } else {
    return ptr;
  }
}

NodePointer
ResolveAsSymbolicReference::operator()(SymbolicReferenceKind kind,
                                       Directness isIndirect,
                                       int32_t offset,
                                       const void *base) {
  // Resolve the absolute pointer to the entity being referenced.
  auto ptr = resolveSymbolicReferenceOffset(kind, isIndirect, offset, base);
  if (SWIFT_UNLIKELY(!ptr)) {
    auto symInfo = SymbolInfo::lookup(base);
    const char *fileName = "<unknown>";
    const char *symbolName = "<unknown>";
    if (symInfo) {
      if (symInfo->getFilename())
        fileName = symInfo->getFilename();
      if (symInfo->getSymbolName())
        symbolName = symInfo->getSymbolName();
    }
    swift::fatalError(
        0,
        "Failed to look up symbolic reference at %p - offset %" PRId32
        " - symbol %s in %s\n",
        base, offset, symbolName, fileName);
  }

  // Figure out this symbolic reference's grammatical role.
  Node::Kind nodeKind;
  bool isType;
  switch (kind) {
  case Demangle::SymbolicReferenceKind::Context: {
    auto descriptor = (const ContextDescriptor *)ptr;
    switch (descriptor->getKind()) {
    case ContextDescriptorKind::Protocol:
      nodeKind = Node::Kind::ProtocolSymbolicReference;
      isType = false;
      break;
    
    case ContextDescriptorKind::OpaqueType:
      nodeKind = Node::Kind::OpaqueTypeDescriptorSymbolicReference;
      isType = false;
      break;
        
    default:
      if (auto typeContext = dyn_cast<TypeContextDescriptor>(descriptor)) {
        nodeKind = Node::Kind::TypeSymbolicReference;
        isType = true;
        break;
      }
      
      // References to other kinds of context aren't yet implemented.
      return nullptr;
    }
    break;
  }
  case Demangle::SymbolicReferenceKind::AccessorFunctionReference: {
    // Save the pointer to the accessor function. We can't demangle it any
    // further as AST, but the consumer of the demangle tree may be able to
    // invoke the function to resolve the thing they're trying to access.
    nodeKind = Node::Kind::AccessorFunctionReference;
    isType = false;
#if SWIFT_PTRAUTH
    // The pointer refers to an accessor function, which we need to sign.
    ptr = (uintptr_t)ptrauth_sign_unauthenticated((void*)ptr,
      ptrauth_key_function_pointer, 0);
#endif
    break;
  }
  case Demangle::SymbolicReferenceKind::UniqueExtendedExistentialTypeShape:
    nodeKind = Node::Kind::UniqueExtendedExistentialTypeShapeSymbolicReference;
    isType = false;
#if SWIFT_PTRAUTH
    ptr = (uintptr_t)ptrauth_sign_unauthenticated((void*)ptr,
      ptrauth_key_process_independent_data,
      SpecialPointerAuthDiscriminators::ExtendedExistentialTypeShape);
#endif
    break;
  case Demangle::SymbolicReferenceKind::NonUniqueExtendedExistentialTypeShape:
    nodeKind = Node::Kind::NonUniqueExtendedExistentialTypeShapeSymbolicReference;
    isType = false;
#if SWIFT_PTRAUTH
    ptr = (uintptr_t)ptrauth_sign_unauthenticated((void*)ptr,
      ptrauth_key_process_independent_data,
      SpecialPointerAuthDiscriminators::NonUniqueExtendedExistentialTypeShape);
#endif
    break;
  }
  
  auto node = Dem.createNode(nodeKind, ptr);
  if (isType) {
    auto typeNode = Dem.createNode(Node::Kind::Type);
    typeNode->addChild(node, Dem);
    node = typeNode;
  }
  return node;
}

static NodePointer
_buildDemanglingForSymbolicReference(SymbolicReferenceKind kind,
                                     const void *resolvedReference,
                                     Demangler &Dem) {
  switch (kind) {
  case SymbolicReferenceKind::Context:
    return _buildDemanglingForContext(
      (const ContextDescriptor *)resolvedReference, {}, Dem);

  case SymbolicReferenceKind::AccessorFunctionReference:
#if SWIFT_PTRAUTH
    // The pointer refers to an accessor function, which we need to sign.
    resolvedReference = ptrauth_sign_unauthenticated(resolvedReference,
      ptrauth_key_function_pointer, 0);
#endif
    return Dem.createNode(Node::Kind::AccessorFunctionReference,
                          (uintptr_t)resolvedReference);

  case SymbolicReferenceKind::UniqueExtendedExistentialTypeShape:
#if SWIFT_PTRAUTH
    resolvedReference = ptrauth_sign_unauthenticated(resolvedReference,
      ptrauth_key_process_independent_data,
      SpecialPointerAuthDiscriminators::ExtendedExistentialTypeShape);
#endif
    return Dem.createNode(Node::Kind::UniqueExtendedExistentialTypeShapeSymbolicReference,
                          (uintptr_t)resolvedReference);

  case SymbolicReferenceKind::NonUniqueExtendedExistentialTypeShape:
#if SWIFT_PTRAUTH
    // The pointer refers to an accessor function, which we need to sign.
    resolvedReference = ptrauth_sign_unauthenticated(resolvedReference,
      ptrauth_key_process_independent_data,
      SpecialPointerAuthDiscriminators::NonUniqueExtendedExistentialTypeShape);
#endif
    return Dem.createNode(Node::Kind::NonUniqueExtendedExistentialTypeShapeSymbolicReference,
                          (uintptr_t)resolvedReference);
  }
  
  swift_unreachable("invalid symbolic reference kind");
}
  
NodePointer
ResolveToDemanglingForContext::operator()(SymbolicReferenceKind kind,
                                          Directness isIndirect,
                                          int32_t offset,
                                          const void *base) {
  auto ptr = resolveSymbolicReferenceOffset(kind, isIndirect, offset, base);

  return _buildDemanglingForSymbolicReference(kind, (const void *)ptr, Dem);
}

NodePointer
ExpandResolvedSymbolicReferences::operator()(SymbolicReferenceKind kind,
                                             const void *ptr) {
  return _buildDemanglingForSymbolicReference(kind, (const void *)ptr, Dem);
}

#pragma mark Nominal type descriptor cache
// Type Metadata Cache.

namespace {
  struct TypeMetadataSection {
    const TypeMetadataRecord *Begin, *End;
    const TypeMetadataRecord *begin() const {
      return Begin;
    }
    const TypeMetadataRecord *end() const {
      return End;
    }
  };

  struct NominalTypeDescriptorCacheEntry {
  private:
    const char *Name;
    size_t NameLength;
    const ContextDescriptor *Description;

  public:
    NominalTypeDescriptorCacheEntry(const llvm::StringRef name,
                                    const ContextDescriptor *description)
        : Description(description) {
      char *nameCopy = reinterpret_cast<char *>(malloc(name.size()));
      memcpy(nameCopy, name.data(), name.size());
      Name = nameCopy;
      NameLength = name.size();
    }

    const ContextDescriptor *getDescription() const { return Description; }

    bool matchesKey(llvm::StringRef aName) {
      return aName == llvm::StringRef{Name, NameLength};
    }

    friend llvm::hash_code
    hash_value(const NominalTypeDescriptorCacheEntry &value) {
      return hash_value(llvm::StringRef{value.Name, value.NameLength});
    }

    template <class... T>
    static size_t getExtraAllocationSize(T &&... ignored) {
      return 0;
    }
  };
} // end anonymous namespace

struct TypeMetadataPrivateState {
  ConcurrentReadableHashMap<NominalTypeDescriptorCacheEntry> NominalCache;
  ConcurrentReadableArray<TypeMetadataSection> SectionsToScan;
  
  TypeMetadataPrivateState() {
    initializeTypeMetadataRecordLookup();
  }

};

static Lazy<TypeMetadataPrivateState> TypeMetadataRecords;

static void
_registerTypeMetadataRecords(TypeMetadataPrivateState &T,
                             const TypeMetadataRecord *begin,
                             const TypeMetadataRecord *end) {
  T.SectionsToScan.push_back(TypeMetadataSection{begin, end});
}

void swift::addImageTypeMetadataRecordBlockCallbackUnsafe(
    const void *baseAddress,
    const void *records, uintptr_t recordsSize) {
  assert(recordsSize % sizeof(TypeMetadataRecord) == 0
         && "weird-sized type metadata section?!");

  // If we have a section, enqueue the type metadata for lookup.
  auto recordBytes = reinterpret_cast<const char *>(records);
  auto recordsBegin
    = reinterpret_cast<const TypeMetadataRecord*>(records);
  auto recordsEnd
    = reinterpret_cast<const TypeMetadataRecord*>(recordBytes + recordsSize);

  // Type metadata cache should always be sufficiently initialized by this
  // point. Attempting to go through get() may also lead to an infinite loop,
  // since we register records during the initialization of
  // TypeMetadataRecords.
  _registerTypeMetadataRecords(TypeMetadataRecords.unsafeGetAlreadyInitialized(),
                               recordsBegin, recordsEnd);
}

void swift::addImageTypeMetadataRecordBlockCallback(const void *baseAddress,
                                                    const void *records,
                                                    uintptr_t recordsSize) {
  TypeMetadataRecords.get();
  addImageTypeMetadataRecordBlockCallbackUnsafe(baseAddress,
                                                records, recordsSize);
}

void
swift::swift_registerTypeMetadataRecords(const TypeMetadataRecord *begin,
                                         const TypeMetadataRecord *end) {
  auto &T = TypeMetadataRecords.get();
  _registerTypeMetadataRecords(T, begin, end);
}

static const ContextDescriptor *
_findContextDescriptor(Demangle::NodePointer node,
                           Demangle::Demangler &Dem);

/// Find the context descriptor for the type extended by the given extension.
///
/// If \p maybeExtension isn't actually an extension context, returns nullptr.
static const ContextDescriptor *
_findExtendedTypeContextDescriptor(const ContextDescriptor *maybeExtension,
                                   Demangler &demangler,
                                   Demangle::NodePointer *demangledNode
                                     = nullptr) {
  auto extension = dyn_cast<ExtensionContextDescriptor>(maybeExtension);
  if (!extension)
    return nullptr;

  Demangle::NodePointer localNode;
  Demangle::NodePointer &node = demangledNode ? *demangledNode : localNode;

  auto mangledName = extension->getMangledExtendedContext();
  node = demangler.demangleType(mangledName,
                                ResolveAsSymbolicReference(demangler));
  if (!node)
    return nullptr;
  if (node->getKind() == Node::Kind::Type) {
    if (node->getNumChildren() < 1)
      return nullptr;
    node = node->getChild(0);
  }
  if (Demangle::isSpecialized(node)) {
    auto unspec = Demangle::getUnspecialized(node, demangler);
    if (!unspec.isSuccess())
      return nullptr;
    node = unspec.result();
  }

  return _findContextDescriptor(node, demangler);
}

/// Recognize imported tag types, which have a special mangling rule.
///
/// This should be kept in sync with the AST mangler and with
/// buildContextDescriptorMangling in MetadataReader.
bool swift::_isCImportedTagType(const TypeContextDescriptor *type,
                                const ParsedTypeIdentity &identity) {
  // Tag types are always imported as structs or enums.
  if (type->getKind() != ContextDescriptorKind::Enum &&
      type->getKind() != ContextDescriptorKind::Struct)
    return false;

  // Not a typedef imported as a nominal type.
  if (identity.isCTypedef())
    return false;

  // Not a related entity.
  if (identity.isAnyRelatedEntity())
    return false;

  // Imported from C.
  return type->Parent->isCImportedContext();
}

ParsedTypeIdentity
ParsedTypeIdentity::parse(const TypeContextDescriptor *type) {
  ParsedTypeIdentity result;

  // The first component is the user-facing name and (unless overridden)
  // the ABI name.
  StringRef component = type->Name.get();
  result.UserFacingName = component;

  // If we don't have import info, we're done.
  if (!type->getTypeContextDescriptorFlags().hasImportInfo()) {
    result.FullIdentity = result.UserFacingName;
    return result;
  }

  // Otherwise, start parsing the import information.
  result.ImportInfo.emplace();

  // The identity starts with the user-facing name.
  const char *startOfIdentity = component.begin();
  const char *endOfIdentity = component.end();

#ifndef NDEBUG
  enum {
    AfterName,
    AfterABIName,
    AfterSymbolNamespace,
    AfterRelatedEntityName,
    AfterIdentity,
  } stage = AfterName;
#endif

  while (true) {
    // Parse the next component.  If it's empty, we're done.
    component = StringRef(component.end() + 1);
    if (component.empty()) break;

    // Update the identity bounds and assert that the identity
    // components are in the right order.
    auto kind = TypeImportComponent(component[0]);
    if (kind == TypeImportComponent::ABIName) {
#ifndef NDEBUG
      assert(stage < AfterABIName);
      stage = AfterABIName;
      assert(result.UserFacingName != component.drop_front(1) &&
             "user-facing name was same as the ABI name");
#endif
      startOfIdentity = component.begin() + 1;
      endOfIdentity = component.end();
    } else if (kind == TypeImportComponent::SymbolNamespace) {
#ifndef NDEBUG
      assert(stage < AfterSymbolNamespace);
      stage = AfterSymbolNamespace;
#endif
      endOfIdentity = component.end();
    } else if (kind == TypeImportComponent::RelatedEntityName) {
#ifndef NDEBUG
      assert(stage < AfterRelatedEntityName);
      stage = AfterRelatedEntityName;
#endif
      endOfIdentity = component.end();
    } else {
#ifndef NDEBUG
      // Anything else is assumed to not be part of the identity.
      stage = AfterIdentity;
#endif
    }

    // Collect the component, whatever it is.
    result.ImportInfo->collect</*asserting*/true>(component);
  }

#ifndef NDEBUG
  assert(stage != AfterName && "no components?");
#endif

  // Record the full identity.
  result.FullIdentity =
    StringRef(startOfIdentity, endOfIdentity - startOfIdentity);

  return result;
}

#if SWIFT_OBJC_INTEROP
/// Determine whether the two demangle trees both refer to the same
/// Objective-C class or protocol referenced by name.
static bool sameObjCTypeManglings(Demangle::NodePointer node1,
                                  Demangle::NodePointer node2) {
  // Entities need to be of the same kind.
  if (node1->getKind() != node2->getKind())
    return false;

  auto name1 = Demangle::getObjCClassOrProtocolName(node1);
  if (!name1) return false;

  auto name2 = Demangle::getObjCClassOrProtocolName(node2);
  if (!name2) return false;

  return *name1 == *name2;
}
#endif

/// Optimization for the case where we need to compare a StringRef and a null terminated C string
/// Not converting s2 to a StringRef avoids the need to call both strlen and memcmp when non-matching
/// but equal length
static bool stringRefEqualsCString(StringRef s1, const char *s2) {
  size_t length = s1.size();
  // It may be possible for s1 to contain embedded NULL characters
  // so additionally validate that the lengths match
  return strncmp(s1.data(), s2, length) == 0 && strlen(s2) == length;
}

bool
swift::_contextDescriptorMatchesMangling(const ContextDescriptor *context,
                                         Demangle::NodePointer node) {
  while (context) {
    if (node->getKind() == Demangle::Node::Kind::Type)
      node = node->getChild(0);
    
    // We can directly match symbolic references to the current context.
    if (node) {
      if (node->getKind() == Demangle::Node::Kind::TypeSymbolicReference
         || node->getKind() == Demangle::Node::Kind::ProtocolSymbolicReference){
        if (equalContexts(context,
               reinterpret_cast<const ContextDescriptor *>(node->getIndex()))) {
          return true;
        }
      }
    }

    switch (context->getKind()) {
    case ContextDescriptorKind::Module: {
      auto module = cast<ModuleContextDescriptor>(context);
      // Match to a mangled module name.
      if (node->getKind() != Demangle::Node::Kind::Module)
        return false;
      if (!stringRefEqualsCString(node->getText(), module->Name.get()))
        return false;
      
      node = nullptr;
      break;
    }
    
    case ContextDescriptorKind::Extension: {
      auto extension = cast<ExtensionContextDescriptor>(context);
      
      // Check whether the extension context matches the mangled context.
      if (node->getKind() != Demangle::Node::Kind::Extension)
        return false;
      if (node->getNumChildren() < 2)
        return false;
      
      // Check that the context being extended matches as well.
      auto extendedContextNode = node->getChild(1);
      DemanglerForRuntimeTypeResolution<> demangler;

      auto extendedDescriptorFromNode =
        _findContextDescriptor(extendedContextNode, demangler);

      Demangle::NodePointer extendedContextDemangled;
      auto extendedDescriptorFromDemangled =
        _findExtendedTypeContextDescriptor(extension, demangler,
                                           &extendedContextDemangled);

      // Determine whether the contexts match.
      bool contextsMatch =
        extendedDescriptorFromNode && extendedDescriptorFromDemangled &&
        equalContexts(extendedDescriptorFromNode,
                      extendedDescriptorFromDemangled);
      
#if SWIFT_OBJC_INTEROP
      // If we have manglings of the same Objective-C type, the contexts match.
      if (!contextsMatch &&
          (!extendedDescriptorFromNode || !extendedDescriptorFromDemangled) &&
          sameObjCTypeManglings(extendedContextNode,
                                extendedContextDemangled)) {
        contextsMatch = true;
      }
#endif

      if (!contextsMatch)
        return false;
      
      // Check whether the generic signature of the extension matches the
      // mangled constraints, if any.

      if (node->getNumChildren() >= 3) {
        // NB: If we ever support extensions with independent generic arguments
        // like `extension <T> Array where Element == Optional<T>`, we'd need
        // to look at the mangled context name to match up generic arguments.
        // That would probably need a new extension mangling form, though.
        
        // TODO
      }
      
      // The parent context of the extension should match in the mangling and
      // context descriptor.
      node = node->getChild(0);
      break;
    }

    case ContextDescriptorKind::Protocol:
      // Match a protocol context.
      if (node->getKind() == Demangle::Node::Kind::Protocol) {
        auto proto = llvm::cast<ProtocolDescriptor>(context);
        auto nameNode = node->getChild(1);
        if (nameNode->getKind() != Demangle::Node::Kind::Identifier)
          return false;
        if (stringRefEqualsCString(nameNode->getText(), proto->Name.get())) {
          node = node->getChild(0);
          break;
        }
      }
      return false;

    default:
      if (auto type = llvm::dyn_cast<TypeContextDescriptor>(context)) {
        llvm::Optional<ParsedTypeIdentity> _identity;
        auto getIdentity = [&]() -> const ParsedTypeIdentity & {
          if (_identity) return *_identity;
          _identity = ParsedTypeIdentity::parse(type);
          return *_identity;
        };

        switch (node->getKind()) {
        // If the mangled name doesn't indicate a type kind, accept anything.
        // Otherwise, try to match them up.
        case Demangle::Node::Kind::OtherNominalType:
          break;
        case Demangle::Node::Kind::Structure:
          // We allow non-structs to match Kind::Structure if they are
          // imported C tag types.  This is necessary because we artificially
          // make imported C tag types Kind::Structure.
          if (type->getKind() != ContextDescriptorKind::Struct &&
              !_isCImportedTagType(type, getIdentity()))
            return false;
          break;
        case Demangle::Node::Kind::Class:
          if (type->getKind() != ContextDescriptorKind::Class)
            return false;
          break;
        case Demangle::Node::Kind::Enum:
          if (type->getKind() != ContextDescriptorKind::Enum)
            return false;
          break;
        case Demangle::Node::Kind::TypeAlias:
          if (!getIdentity().isCTypedef())
            return false;
          break;

        default:
          return false;
        }

        auto nameNode = node->getChild(1);
        
        // Declarations synthesized by the Clang importer get a small tag
        // string in addition to their name.
        if (nameNode->getKind() == Demangle::Node::Kind::RelatedEntityDeclName){          
          if (!getIdentity().isRelatedEntity(
                                        nameNode->getFirstChild()->getText()))
            return false;
          
          nameNode = nameNode->getChild(1);
        } else if (getIdentity().isAnyRelatedEntity()) {
          return false;
        }
        
        // We should only match public or internal declarations with stable
        // names. The runtime metadata for private declarations would be
        // anonymized.
        if (nameNode->getKind() == Demangle::Node::Kind::Identifier) {
          if (nameNode->getText() != getIdentity().getABIName())
            return false;
          
          node = node->getChild(0);
          break;
        }
        
        return false;

      }

      // We don't know about this kind of context, or it doesn't have a stable
      // name we can match to.
      return false;
    }
    
    context = context->Parent;
  }
  
  // We should have reached the top of the node tree at the same time we reached
  // the top of the context tree.
  if (node)
    return false;
  
  return true;
}

// returns the nominal type descriptor for the type named by typeName
static const ContextDescriptor *
_searchTypeMetadataRecords(TypeMetadataPrivateState &T,
                           Demangle::NodePointer node) {
#if SWIFT_OBJC_INTEROP
  // Classes in the __C module are ObjC classes. They never have a
  // nominal type descriptor, so don't bother to search for one.
  if (node && node->getKind() == Node::Kind::Class)
    if (auto child = node->getFirstChild())
      if (child->getKind() == Node::Kind::Module && child->hasText())
        if (child->getText() == MANGLING_MODULE_OBJC)
          return nullptr;
#endif

  for (auto &section : T.SectionsToScan.snapshot()) {
    for (const auto &record : section) {
      if (auto context = record.getContextDescriptor()) {
        if (_contextDescriptorMatchesMangling(context, node)) {
          return context;
        }
      }
    }
  }

  return nullptr;
}

#define DESCRIPTOR_MANGLING_SUFFIX_Structure Mn
#define DESCRIPTOR_MANGLING_SUFFIX_Enum Mn
#define DESCRIPTOR_MANGLING_SUFFIX_Protocol Mp

#define DESCRIPTOR_MANGLING_SUFFIX_(X) X
#define DESCRIPTOR_MANGLING_SUFFIX(KIND) \
  DESCRIPTOR_MANGLING_SUFFIX_(DESCRIPTOR_MANGLING_SUFFIX_ ## KIND)

#define DESCRIPTOR_MANGLING_(CHAR, SUFFIX) \
  $sS ## CHAR ## SUFFIX
#define DESCRIPTOR_MANGLING(CHAR, SUFFIX) DESCRIPTOR_MANGLING_(CHAR, SUFFIX)

#define STANDARD_TYPE(KIND, MANGLING, TYPENAME) \
  extern "C" const ContextDescriptor DESCRIPTOR_MANGLING(MANGLING, DESCRIPTOR_MANGLING_SUFFIX(KIND));

// FIXME: When the _Concurrency library gets merged into the Standard Library,
// we will be able to reference those symbols directly as well.
#define STANDARD_TYPE_CONCURRENCY(KIND, MANGLING, TYPENAME)

#if !SWIFT_OBJC_INTEROP
# define OBJC_INTEROP_STANDARD_TYPE(KIND, MANGLING, TYPENAME)
#endif

#include "swift/Demangling/StandardTypesMangling.def"

static const ConcurrencyStandardTypeDescriptors *concurrencyDescriptors;

static const ContextDescriptor *
_findContextDescriptor(Demangle::NodePointer node,
                       Demangle::Demangler &Dem) {
  NodePointer symbolicNode = node;
  if (symbolicNode->getKind() == Node::Kind::Type)
    symbolicNode = symbolicNode->getChild(0);

  // If we have a symbolic reference to a context, resolve it immediately.
  if (symbolicNode->getKind() == Node::Kind::TypeSymbolicReference) {
    return cast<TypeContextDescriptor>(
      (const ContextDescriptor *)symbolicNode->getIndex());
  }

#if SWIFT_STDLIB_SHORT_MANGLING_LOOKUPS
  // Fast-path lookup for standard library type references with short manglings.
  if (symbolicNode->getNumChildren() >= 2
      && symbolicNode->getChild(0)->getKind() == Node::Kind::Module
      && symbolicNode->getChild(0)->getText().equals("Swift")
      && symbolicNode->getChild(1)->getKind() == Node::Kind::Identifier) {
    auto name = symbolicNode->getChild(1)->getText();

#define STANDARD_TYPE(KIND, MANGLING, TYPENAME) \
    if (name.equals(#TYPENAME)) { \
      return &DESCRIPTOR_MANGLING(MANGLING, DESCRIPTOR_MANGLING_SUFFIX(KIND)); \
    }
  // FIXME: When the _Concurrency library gets merged into the Standard Library,
  // we will be able to reference those symbols directly as well.
#define STANDARD_TYPE_CONCURRENCY(KIND, MANGLING, TYPENAME)                    \
  if (concurrencyDescriptors && name.equals(#TYPENAME)) {                      \
    return concurrencyDescriptors->TYPENAME;                                   \
  }
#if !SWIFT_OBJC_INTEROP
# define OBJC_INTEROP_STANDARD_TYPE(KIND, MANGLING, TYPENAME)
#endif

#include "swift/Demangling/StandardTypesMangling.def"
  }
#endif
  
  const ContextDescriptor *foundContext = nullptr;
  auto &T = TypeMetadataRecords.get();

  // Nothing to resolve if have a generic parameter.
  if (symbolicNode->getKind() == Node::Kind::DependentGenericParamType)
    return nullptr;

  auto mangling =
    Demangle::mangleNode(node, ExpandResolvedSymbolicReferences(Dem), Dem);

  if (!mangling.isSuccess())
    return nullptr;

  StringRef mangledName = mangling.result();


  // Look for an existing entry.
  // Find the bucket for the metadata entry.
  {
    auto snapshot = T.NominalCache.snapshot();
    if (auto Value = snapshot.find(mangledName))
      return Value->getDescription();
  }

  // Check type metadata records		   
  // Scan any newly loaded images for context descriptors, then try the context
  foundContext = _searchTypeMetadataRecords(T, node);
  
  // Check protocol conformances table. Note that this has no support for		
  // resolving generic types yet.
  if (!foundContext)
    foundContext = _searchConformancesByMangledTypeName(node);
  
  if (foundContext)
    T.NominalCache.getOrInsert(mangledName, [&](NominalTypeDescriptorCacheEntry
                                                    *entry,
                                                bool created) {
      if (created)
        ::new (entry) NominalTypeDescriptorCacheEntry{mangledName, foundContext};
      return true;
    });

  return foundContext;
}

void swift::_swift_registerConcurrencyStandardTypeDescriptors(
    const ConcurrencyStandardTypeDescriptors *descriptors) {
  concurrencyDescriptors = descriptors;
}

#pragma mark Protocol descriptor cache
namespace {
  struct ProtocolSection {
    const ProtocolRecord *Begin, *End;

    const ProtocolRecord *begin() const {
      return Begin;
    }
    const ProtocolRecord *end() const {
      return End;
    }
  };

  struct ProtocolDescriptorCacheEntry {
  private:
    const char *Name;
    size_t NameLength;
    const ProtocolDescriptor *Description;

  public:
    ProtocolDescriptorCacheEntry(const llvm::StringRef name,
                                 const ProtocolDescriptor *description)
        : Description(description) {
      char *nameCopy = reinterpret_cast<char *>(malloc(name.size()));
      memcpy(nameCopy, name.data(), name.size());
      Name = nameCopy;
      NameLength = name.size();
    }

    const ProtocolDescriptor *getDescription() const { return Description; }

    bool matchesKey(llvm::StringRef aName) {
      return aName == llvm::StringRef{Name, NameLength};
    }

    friend llvm::hash_code
    hash_value(const ProtocolDescriptorCacheEntry &value) {
      return hash_value(llvm::StringRef{value.Name, value.NameLength});
    }

    template <class... T>
    static size_t getExtraAllocationSize(T &&... ignored) {
      return 0;
    }
  };

  struct ProtocolMetadataPrivateState {
    ConcurrentReadableHashMap<ProtocolDescriptorCacheEntry> ProtocolCache;
    ConcurrentReadableArray<ProtocolSection> SectionsToScan;

    ProtocolMetadataPrivateState() {
      initializeProtocolLookup();
    }
  };

  static Lazy<ProtocolMetadataPrivateState> Protocols;
}

static void
_registerProtocols(ProtocolMetadataPrivateState &C,
                   const ProtocolRecord *begin,
                   const ProtocolRecord *end) {
  C.SectionsToScan.push_back(ProtocolSection{begin, end});
}

void swift::addImageProtocolsBlockCallbackUnsafe(const void *baseAddress,
                                                 const void *protocols,
                                                 uintptr_t protocolsSize) {
  assert(protocolsSize % sizeof(ProtocolRecord) == 0 &&
         "protocols section not a multiple of ProtocolRecord");

  // If we have a section, enqueue the protocols for lookup.
  auto protocolsBytes = reinterpret_cast<const char *>(protocols);
  auto recordsBegin
    = reinterpret_cast<const ProtocolRecord *>(protocols);
  auto recordsEnd
    = reinterpret_cast<const ProtocolRecord *>(protocolsBytes + protocolsSize);

  // Conformance cache should always be sufficiently initialized by this point.
  _registerProtocols(Protocols.unsafeGetAlreadyInitialized(),
                     recordsBegin, recordsEnd);
}

void swift::addImageProtocolsBlockCallback(const void *baseAddress,
                                           const void *protocols,
                                           uintptr_t protocolsSize) {
  Protocols.get();
  addImageProtocolsBlockCallbackUnsafe(baseAddress, protocols, protocolsSize);
}

void swift::swift_registerProtocols(const ProtocolRecord *begin,
                                    const ProtocolRecord *end) {
  auto &C = Protocols.get();
  _registerProtocols(C, begin, end);
}

static const ProtocolDescriptor *
_searchProtocolRecords(ProtocolMetadataPrivateState &C,
                       NodePointer node) {
  for (auto &section : C.SectionsToScan.snapshot()) {
    for (const auto &record : section) {
      if (auto protocol = record.Protocol.getPointer()) {
        if (_contextDescriptorMatchesMangling(protocol, node))
          return protocol;
      }
    }
  }

  return nullptr;
}

static const ProtocolDescriptor *
_findProtocolDescriptor(NodePointer node,
                        Demangle::Demangler &Dem,
                        std::string &mangledName) {
  const ProtocolDescriptor *foundProtocol = nullptr;
  auto &T = Protocols.get();

  // If we have a symbolic reference to a context, resolve it immediately.
  NodePointer symbolicNode = node;
  if (symbolicNode->getKind() == Node::Kind::Type)
    symbolicNode = symbolicNode->getChild(0);
  if (symbolicNode->getKind() == Node::Kind::ProtocolSymbolicReference)
    return cast<ProtocolDescriptor>(
      (const ContextDescriptor *)symbolicNode->getIndex());

  auto mangling =
    Demangle::mangleNode(node, ExpandResolvedSymbolicReferences(Dem), Dem);

  if (!mangling.isSuccess())
    return nullptr;

  mangledName = mangling.result().str();

  // Look for an existing entry.
  // Find the bucket for the metadata entry.
  {
    auto snapshot = T.ProtocolCache.snapshot();
    if (auto Value = snapshot.find(mangledName))
      return Value->getDescription();
  }

  // Check type metadata records
  foundProtocol = _searchProtocolRecords(T, node);

  if (foundProtocol) {
    T.ProtocolCache.getOrInsert(mangledName, [&](ProtocolDescriptorCacheEntry
                                                     *entry,
                                                 bool created) {
      if (created)
        ::new (entry) ProtocolDescriptorCacheEntry{mangledName, foundProtocol};
      return true;
    });
  }

  return foundProtocol;
}

#pragma mark Type field descriptor cache
namespace {
struct FieldDescriptorCacheEntry {
private:
  const Metadata *Type;
  const FieldDescriptor *Description;

public:
  FieldDescriptorCacheEntry(const Metadata *type,
                            const FieldDescriptor *description)
      : Type(type), Description(description) {}

  const FieldDescriptor *getDescription() { return Description; }

  int compareWithKey(const Metadata *other) const {
    auto a = (uintptr_t)Type;
    auto b = (uintptr_t)other;
    return a == b ? 0 : (a < b ? -1 : 1);
  }

  template <class... Args>
  static size_t getExtraAllocationSize(Args &&... ignored) {
    return 0;
  }
};

} // namespace

#pragma mark Metadata lookup via mangled name

llvm::Optional<unsigned>
swift::_depthIndexToFlatIndex(unsigned depth, unsigned index,
                              llvm::ArrayRef<unsigned> paramCounts) {
  // Out-of-bounds depth.
  if (depth >= paramCounts.size()) return None;

  // Compute the flat index.
  unsigned flatIndex = index + (depth == 0 ? 0 : paramCounts[depth - 1]);

  // Out-of-bounds index.
  if (flatIndex >= paramCounts[depth]) return None;

  return flatIndex;
}

/// Gather generic parameter counts from a context descriptor.
///
/// \returns true if the innermost descriptor is generic.
bool swift::_gatherGenericParameterCounts(
    const ContextDescriptor *descriptor,
    llvm::SmallVectorImpl<unsigned> &genericParamCounts,
    Demangler &BorrowFrom) {
  DemanglerForRuntimeTypeResolution<> demangler;
  demangler.providePreallocatedMemory(BorrowFrom);

  if (auto extension = _findExtendedTypeContextDescriptor(descriptor,
                                                          demangler)) {
    // If we have a nominal type extension descriptor, extract the extended type
    // and use that. If the extension is not nominal, then we can use the
    // extension's own signature.
    descriptor = extension;
  }

  // Once we hit a non-generic descriptor, we're done.
  if (!descriptor->isGeneric()) return false;

  // Recurse to record the parent context's generic parameters.
  auto parent = descriptor->Parent.get();
  (void)_gatherGenericParameterCounts(parent, genericParamCounts, demangler);

  // Record a new level of generic parameters if the count exceeds the
  // previous count.
  unsigned parentCount = parent->getNumGenericParams();
  unsigned myCount = descriptor->getNumGenericParams();
  if (myCount > parentCount) {
    genericParamCounts.push_back(myCount);
    return true;
  }

  return false;
}

/// Retrieve the generic parameters introduced in this context.
static llvm::ArrayRef<GenericParamDescriptor>
getLocalGenericParams(const ContextDescriptor *context) {
  if (!context->isGeneric())
    return { };

  // Determine where to start looking at generic parameters.
  unsigned startParamIndex;
  if (auto parent = context->Parent.get())
    startParamIndex = parent->getNumGenericParams();
  else
    startParamIndex = 0;

  auto genericContext = context->getGenericContext();
  return genericContext->getGenericParams().slice(startParamIndex);
}

namespace {

/// Function object that produces substitutions for the generic parameters
/// that occur within a mangled name, using the complete set of generic
/// arguments "as written".
///
/// Use with \c _getTypeByMangledName to decode potentially-generic types.
class SubstGenericParametersFromWrittenArgs {
  /// The complete set of generic arguments.
  const llvm::SmallVectorImpl<MetadataOrPack> &allGenericArgs;

  /// The counts of generic parameters at each level.
  const llvm::SmallVectorImpl<unsigned> &genericParamCounts;

public:
  /// Initialize a new function object to handle substitutions. Both
  /// parameters are references to vectors that must live longer than
  /// this function object.
  ///
  /// \param allGenericArgs The complete set of generic arguments, as written.
  /// This could come directly from "source" (where all generic arguments are
  /// encoded) or from metadata via gatherWrittenGenericArgs().
  ///
  /// \param genericParamCounts The count of generic parameters at each
  /// generic level, typically gathered by _gatherGenericParameterCounts.
  explicit SubstGenericParametersFromWrittenArgs(
      const llvm::SmallVectorImpl<MetadataOrPack> &allGenericArgs,
      const llvm::SmallVectorImpl<unsigned> &genericParamCounts)
      : allGenericArgs(allGenericArgs),
        genericParamCounts(genericParamCounts) {}

  MetadataOrPack getMetadata(unsigned depth, unsigned index) const;
  const WitnessTable *getWitnessTable(const Metadata *type,
                                      unsigned index) const;
};

}  // end anonymous namespace

static void _gatherWrittenGenericArgs(
    const Metadata *metadata, const TypeContextDescriptor *description,
    llvm::SmallVectorImpl<MetadataOrPack> &allGenericArgs,
    Demangler &BorrowFrom);

static llvm::Optional<TypeLookupError>
_gatherGenericParameters(const ContextDescriptor *context,
                         llvm::ArrayRef<MetadataOrPack> genericArgs,
                         const Metadata *parent,
                         llvm::SmallVectorImpl<unsigned> &genericParamCounts,
                         llvm::SmallVectorImpl<const void *> &allGenericArgsVec,
                         Demangler &demangler) {
  auto makeCommonErrorStringGetter = [&] {
    auto metadataVector = genericArgs.vec();
    return [=] {
      std::string str;

      str += "_gatherGenericParameters: context: ";

      if (auto contextInfo = SymbolInfo::lookup(context)) {
        str += contextInfo->getSymbolName();
        str += " ";
      }

      char *contextStr;
      swift_asprintf(&contextStr, "%p", context);
      str += contextStr;
      free(contextStr);

      str += " <";

      bool first = true;
      for (MetadataOrPack metadata : genericArgs) {
        if (!first)
          str += ", ";
        first = false;
        str += metadata.nameForMetadata();
      }

      str += "> ";

      str += "parent: ";
      if (parent)
        str += nameForMetadata(parent);
      else
        str += "<null>";
      str += " - ";

      return str;
    };
  };

  // Figure out the various levels of generic parameters we have in
  // this type.
  (void)_gatherGenericParameterCounts(context,
                                      genericParamCounts, demangler);
  unsigned numTotalGenericParams =
    genericParamCounts.empty() ? 0 : genericParamCounts.back();
  
  // Check whether we have the right number of generic arguments.
  if (genericArgs.size() == getLocalGenericParams(context).size()) {
    // Okay: genericArgs is the innermost set of generic arguments.
  } else if (genericArgs.size() == numTotalGenericParams && !parent) {
    // Okay: genericArgs is the complete set of generic arguments.
  } else {
    auto commonString = makeCommonErrorStringGetter();
    auto genericArgsSize = genericArgs.size();
    return TypeLookupError([=] {
      return commonString() + "incorrect number of generic args (" +
             std::to_string(genericArgsSize) + "), " +
             std::to_string(getLocalGenericParams(context).size()) +
             " local params, " + std::to_string(numTotalGenericParams) +
             " total params";
    });
  }
  
  // If there are generic parameters at any level, check the generic
  // requirements and fill in the generic arguments vector.
  if (!genericParamCounts.empty()) {
    // Compute the set of generic arguments "as written".
    llvm::SmallVector<MetadataOrPack, 8> allGenericArgs;

    // If we have a parent, gather it's generic arguments "as written".
    if (parent) {
      _gatherWrittenGenericArgs(parent, parent->getTypeContextDescriptor(),
                                allGenericArgs, demangler);
    }
    
    // Add the generic arguments we were given.
    allGenericArgs.insert(allGenericArgs.end(),
                          genericArgs.begin(), genericArgs.end());
    
    // Copy the generic arguments needed for metadata from the generic
    // arguments "as written".
    auto generics = context->getGenericContext();
    assert(generics);
    {
      // Add a placeholder length for each shape class.
      auto packShapeHeader = generics->getGenericPackShapeHeader();
      if (packShapeHeader.NumShapeClasses > 0) {
        assert(allGenericArgsVec.empty());
        allGenericArgsVec.resize(packShapeHeader.NumShapeClasses);
      }

      // If we have the wrong number of generic arguments, fail.
      auto genericParams = generics->getGenericParams();
      unsigned n = genericParams.size();
      if (allGenericArgs.size() != n) {
        auto commonString = makeCommonErrorStringGetter();
        auto argsVecSize = allGenericArgsVec.size();
        return TypeLookupError([=] {
          return commonString() + "have " + std::to_string(argsVecSize) +
                 "generic args, expected " + std::to_string(n);
        });
      }

      // Add metadata for each canonical generic parameter.
      for (unsigned i = 0; i != n; ++i) {
        const auto &param = genericParams[i];
        auto arg = allGenericArgs[i];

        switch (param.getKind()) {
        case GenericParamKind::Type: {
          if (!arg.isMetadata()) {
            auto commonString = makeCommonErrorStringGetter();
            return TypeLookupError([=] {
              return commonString() + "param " + std::to_string(i) +
                     " expected metadata but got a metadata pack";
            });
          }

          if (param.hasKeyArgument()) {
            allGenericArgsVec.push_back(arg.getMetadata());
          }

          break;
        }
        case GenericParamKind::TypePack: {
          if (!arg.isMetadataPack()) {
            auto commonString = makeCommonErrorStringGetter();
            return TypeLookupError([=] {
              return commonString() + "param " + std::to_string(i) +
                     " expected a metadata pack but got metadata";
            });
          }

          if (param.hasKeyArgument()) {
            allGenericArgsVec.push_back(arg.getMetadataPack().getPointer());
          }

          break;
        }
        default:
          auto commonString = makeCommonErrorStringGetter();
          return TypeLookupError([=] {
            return commonString() + "param " + std::to_string(i) +
                   " has unexpected kind " +
                   std::to_string(static_cast<uint8_t>(param.getKind()));
          });
        }
      }

      // Fill in the length for each shape class.
      auto packShapeDescriptors = generics->getGenericPackShapeDescriptors();
      for (auto packShapeDescriptor : packShapeDescriptors) {
        if (packShapeDescriptor.Kind != GenericPackKind::Metadata)
          continue;

        assert(packShapeDescriptor.Index < allGenericArgsVec.size());
        assert(packShapeDescriptor.ShapeClass < packShapeHeader.NumShapeClasses);

        MetadataPackPointer pack(allGenericArgsVec[packShapeDescriptor.Index]);
        assert(pack.getLifetime() == PackLifetime::OnHeap);

        allGenericArgsVec[packShapeDescriptor.ShapeClass] =
            reinterpret_cast<const void *>(pack.getNumElements());
      }
    }

    // Check whether the generic requirements are satisfied, collecting
    // any extra arguments we need for the instantiation function.
    SubstGenericParametersFromWrittenArgs substitutions(allGenericArgs,
                                                        genericParamCounts);
    auto error = _checkGenericRequirements(
        generics->getGenericRequirements(), allGenericArgsVec,
        [&substitutions](unsigned depth, unsigned index) {
          return substitutions.getMetadata(depth, index).Ptr;
        },
        [&substitutions](const Metadata *type, unsigned index) {
          return substitutions.getWitnessTable(type, index);
        });
    if (error)
      return *error;

    // If we still have the wrong number of generic arguments, this is
    // some kind of metadata mismatch.
    if (generics->getGenericContextHeader().getNumArguments() !=
        allGenericArgsVec.size()) {
      auto commonString = makeCommonErrorStringGetter();
      auto argsVecSize = allGenericArgsVec.size();
      return TypeLookupError([=] {
        return commonString() + "generic argument count mismatch, expected " +
               std::to_string(
                   generics->getGenericContextHeader().getNumArguments()) +
               ", have " + std::to_string(argsVecSize);
      });
    }
  }

  return llvm::None;
}

namespace {

/// Find the offset of the protocol requirement for an associated type with
/// the given name in the given protocol descriptor.
llvm::Optional<const ProtocolRequirement *>
findAssociatedTypeByName(const ProtocolDescriptor *protocol, StringRef name) {
  // If we don't have associated type names, there's nothing to do.
  const char *associatedTypeNamesPtr = protocol->AssociatedTypeNames.get();
  if (!associatedTypeNamesPtr) return None;

  // Look through the list of associated type names.
  StringRef associatedTypeNames(associatedTypeNamesPtr);
  unsigned matchingAssocTypeIdx = 0;
  bool found = false;
  while (!associatedTypeNames.empty()) {
    // Avoid using StringRef::split because its definition is not
    // provided in the header so that it requires linking with libSupport.a.
    auto splitIdx = associatedTypeNames.find(' ');
    if (associatedTypeNames.substr(0, splitIdx) == name) {
      found = true;
      break;
    }

    ++matchingAssocTypeIdx;
    associatedTypeNames = associatedTypeNames.substr(splitIdx).substr(1);
  }

  if (!found) return None;

  // We have a match on the Nth associated type; go find the Nth associated
  // type requirement.
  unsigned currentAssocTypeIdx = 0;
  unsigned numRequirements = protocol->NumRequirements;
  auto requirements = protocol->getRequirements();
  for (unsigned reqIdx = 0; reqIdx != numRequirements; ++reqIdx) {
    if (requirements[reqIdx].Flags.getKind() !=
        ProtocolRequirementFlags::Kind::AssociatedTypeAccessFunction)
      continue;

    if (currentAssocTypeIdx == matchingAssocTypeIdx)
      return requirements.begin() + reqIdx;

    ++currentAssocTypeIdx;
  }

  swift_unreachable("associated type names don't line up");
}

} // end unnamed namespace

static Lazy<Mutex> DynamicReplacementLock;

namespace {
struct OpaqueTypeMappings {
  llvm::DenseMap<const OpaqueTypeDescriptor *, const OpaqueTypeDescriptor *>
      descriptorMapping;
  const OpaqueTypeDescriptor* find(const OpaqueTypeDescriptor *orig) {
    const OpaqueTypeDescriptor *replacement = nullptr;
    DynamicReplacementLock.get().withLock([&] {
      auto entry = descriptorMapping.find(orig);
      if (entry != descriptorMapping.end())
        replacement = entry->second;
    });
    return replacement;
  }

  // We take a mutex argument to make sure someone is holding the lock.
  void insert(const OpaqueTypeDescriptor *orig,
              const OpaqueTypeDescriptor *replacement, const Mutex &) {
    descriptorMapping[orig] = replacement;
  }
};
} // end unnamed namespace

static Lazy<OpaqueTypeMappings> opaqueTypeMappings;

static const OpaqueTypeDescriptor *
_findOpaqueTypeDescriptor(NodePointer demangleNode,
                          Demangler &dem) {
  // Directly resolve a symbolic reference.
  if (demangleNode->getKind()
        == Node::Kind::OpaqueTypeDescriptorSymbolicReference) {
    auto context = (const ContextDescriptor *)demangleNode->getIndex();
    auto *orig = cast<OpaqueTypeDescriptor>(context);
    if (auto *entry = opaqueTypeMappings.get().find(orig)) {
      return entry;
    }
    return orig;
  }
  
  // TODO: Find non-symbolic-referenced opaque decls.
  return nullptr;
}

namespace {

/// Constructs metadata by decoding a mangled type name, for use with
/// \c TypeDecoder.
class DecodedMetadataBuilder {
private:
  /// The demangler we'll use when building new nodes.
  Demangler &demangler;

  /// Substitute generic parameters.
  SubstGenericParameterFn substGenericParameter;

  /// Substitute dependent witness tables.
  SubstDependentWitnessTableFn substWitnessTable;

  /// Ownership information related to the metadata we are trying to lookup.
  TypeReferenceOwnership ReferenceOwnership;

public:
  using BuiltType = MetadataOrPack;

  struct BuiltLayoutConstraint {
    bool operator==(BuiltLayoutConstraint rhs) const { return true; }
    operator bool() const { return true; }
  };
  using BuiltLayoutConstraint = BuiltLayoutConstraint;
  using BuiltTypeDecl = const ContextDescriptor *;
  using BuiltProtocolDecl = ProtocolDescriptorRef;
  using BuiltGenericSignature = const Metadata *;
  using BuiltSubstitution = std::pair<BuiltType, BuiltType>;
  using BuiltSubstitutionMap = llvm::ArrayRef<BuiltSubstitution>;
  using BuiltGenericTypeParam = const Metadata *;

  struct BuiltRequirement {
    RequirementKind Kind;
    BuiltType FirstType;

    union {
      BuiltType SecondType;
      BuiltLayoutConstraint SecondLayout;
    };

    BuiltRequirement(RequirementKind kind, BuiltType first,
                     BuiltType second)
        : Kind(kind), FirstType(first), SecondType(second) {
      assert(first);
      assert(second);
      assert(kind != RequirementKind::Layout);
    }

    BuiltRequirement(RequirementKind kind, BuiltType first,
                     BuiltLayoutConstraint second)
        : Kind(kind), FirstType(first), SecondLayout(second) {
      assert(first);
      assert(second);
      assert(kind == RequirementKind::Layout);
    }

    /// Determine the kind of requirement.
    RequirementKind getKind() const {
      return Kind;
    }

    /// Retrieve the first type.
    BuiltType getFirstType() const {
      return FirstType;
    }

    /// Retrieve the second type.
    BuiltType getSecondType() const {
      assert(getKind() != RequirementKind::Layout);
      return SecondType;
    }

    /// Retrieve the layout constraint.
    BuiltLayoutConstraint getLayoutConstraint() const {
      assert(getKind() == RequirementKind::Layout);
      return SecondLayout;
    }
  };

  DecodedMetadataBuilder(Demangler &demangler,
                         SubstGenericParameterFn substGenericParameter,
                         SubstDependentWitnessTableFn substWitnessTable)
    : demangler(demangler),
      substGenericParameter(substGenericParameter),
      substWitnessTable(substWitnessTable) { }

  BuiltType decodeMangledType(NodePointer node,
                              bool forRequirement = true) {
    return Demangle::decodeMangledType(*this, node, forRequirement)
        .getType();
  }

  Demangle::NodeFactory &getNodeFactory() { return demangler; }

  TypeLookupErrorOr<BuiltType>
  resolveOpaqueType(NodePointer opaqueDecl,
                    llvm::ArrayRef<llvm::ArrayRef<BuiltType>> genericArgs,
                    unsigned ordinal) {
    auto descriptor = _findOpaqueTypeDescriptor(opaqueDecl, demangler);
    if (!descriptor)
      return BuiltType();
    auto outerContext = descriptor->Parent.get();

    llvm::SmallVector<MetadataOrPack, 8> allGenericArgs;
    for (auto argSet : genericArgs)
      allGenericArgs.append(argSet.begin(), argSet.end());
    
    // Gather the generic parameters we need to parameterize the opaque decl.
    llvm::SmallVector<unsigned, 8> genericParamCounts;
    llvm::SmallVector<const void *, 8> allGenericArgsVec;

    if (auto error = _gatherGenericParameters(
            outerContext, allGenericArgs, nullptr, /* no parent */
            genericParamCounts, allGenericArgsVec, demangler))
      return *error;

    auto mangledName = descriptor->getUnderlyingTypeArgument(ordinal);
    SubstGenericParametersFromMetadata substitutions(descriptor,
                                                     allGenericArgsVec.data());
    return BuiltType(
        swift_getTypeByMangledName(MetadataState::Complete,
                                   mangledName, allGenericArgsVec.data(),
        [&substitutions](unsigned depth, unsigned index) {
          // FIXME: Variadic generics
          return substitutions.getMetadata(depth, index).getMetadataOrNull();
        },
        [&substitutions](const Metadata *type, unsigned index) {
          return substitutions.getWitnessTable(type, index);
        }).getType().getMetadata());
  }

  BuiltTypeDecl createTypeDecl(NodePointer node,
                               bool &typeAlias) const {
    // Look for a nominal type descriptor based on its mangled name.
    return _findContextDescriptor(node, demangler);
  }

  BuiltProtocolDecl createProtocolDecl(NodePointer node) const {
    // Look for a protocol descriptor based on its mangled name.
    std::string mangledName;
    if (auto protocol = _findProtocolDescriptor(node, demangler, mangledName))
      return ProtocolDescriptorRef::forSwift(protocol);;

#if SWIFT_OBJC_INTEROP
    // Look for a Swift-defined @objc protocol with the Swift 3 mangling that
    // is used for Objective-C entities.
    auto mangling = mangleNodeAsObjcCString(node, demangler);
    if (mangling.isSuccess()) {
      const char *objcMangledName = mangling.result();
      if (auto protocol = objc_getProtocol(objcMangledName))
        return ProtocolDescriptorRef::forObjC(protocol);
    }
#endif

    return ProtocolDescriptorRef();
  }

  BuiltProtocolDecl createObjCProtocolDecl(
                                         const std::string &mangledName) const {
#if SWIFT_OBJC_INTEROP
    return ProtocolDescriptorRef::forObjC(
        objc_getProtocol(mangledName.c_str()));
#else
    return ProtocolDescriptorRef();
#endif
  }

  TypeLookupErrorOr<BuiltType>
  createObjCClassType(const std::string &mangledName) const {
#if SWIFT_OBJC_INTEROP
    auto objcClass = objc_getClass(mangledName.c_str());
    return BuiltType(
        swift_getObjCClassMetadata((const ClassMetadata *)objcClass));
#else
    return BuiltType();
#endif
  }

  TypeLookupErrorOr<BuiltType>
  createBoundGenericObjCClassType(const std::string &mangledName,
                                  llvm::ArrayRef<BuiltType> args) const {
    // Generic arguments of lightweight Objective-C generic classes are not
    // reified in the metadata.
    return createObjCClassType(mangledName);
  }

  TypeLookupErrorOr<BuiltType>
  createNominalType(BuiltTypeDecl metadataOrTypeDecl, BuiltType parent) const {
    // Treat nominal type creation the same way as generic type creation,
    // but with no generic arguments at this level.
    return createBoundGenericType(metadataOrTypeDecl, { }, parent);
  }

  TypeLookupErrorOr<BuiltType> createTypeAliasType(BuiltTypeDecl typeAliasDecl,
                                                   BuiltType parent) const {
    // We can't support sugared types here since we have no way to
    // resolve the underlying type of the type alias. However, some
    // CF types are mangled as type aliases.
    return createNominalType(typeAliasDecl, parent);
  }

  TypeLookupErrorOr<BuiltType>
  createBoundGenericType(BuiltTypeDecl anyTypeDecl,
                         llvm::ArrayRef<BuiltType> genericArgs,
                         BuiltType parent) const {
    auto typeDecl = dyn_cast<TypeContextDescriptor>(anyTypeDecl);
    if (!typeDecl) {
      if (auto protocol = dyn_cast<ProtocolDescriptor>(anyTypeDecl))
        return BuiltType(_getSimpleProtocolTypeMetadata(protocol));

      return BuiltType();
    }

    if (!parent.isMetadataOrNull()) {
      return TYPE_LOOKUP_ERROR_FMT("Tried to build a bound generic type where "
                                   "the parent type is a pack");
    }

    // Figure out the various levels of generic parameters we have in
    // this type.
    llvm::SmallVector<unsigned, 8> genericParamCounts;
    llvm::SmallVector<const void *, 8> allGenericArgsVec;

    if (auto error = _gatherGenericParameters(typeDecl, genericArgs,
                                              parent.getMetadataOrNull(),
                                              genericParamCounts,
                                              allGenericArgsVec, demangler))
      return *error;

    // Call the access function.
    auto accessFunction = typeDecl->getAccessFunction();
    if (!accessFunction) return BuiltType();

    return BuiltType(accessFunction(MetadataState::Abstract,
                                    allGenericArgsVec));
  }

  TypeLookupErrorOr<BuiltType>
  createSymbolicExtendedExistentialType(NodePointer shapeNode,
                                  llvm::ArrayRef<BuiltType> genArgs) const {
    const ExtendedExistentialTypeShape *shape;
    if (shapeNode->getKind() ==
          Node::Kind::UniqueExtendedExistentialTypeShapeSymbolicReference) {
      shape = reinterpret_cast<const ExtendedExistentialTypeShape *>(
                shapeNode->getIndex());
    } else if (shapeNode->getKind() ==
        Node::Kind::NonUniqueExtendedExistentialTypeShapeSymbolicReference) {
      auto nonUniqueShape =
        reinterpret_cast<const NonUniqueExtendedExistentialTypeShape *>(
                shapeNode->getIndex());
      shape = swift_getExtendedExistentialTypeShape(nonUniqueShape);
    } else {
      return TYPE_LOOKUP_ERROR_FMT("Tried to build an extended existential "
                                   "metatype from an unexpected shape node");
    }

    auto rawShape =
      swift_auth_data_non_address(shape,
          SpecialPointerAuthDiscriminators::ExtendedExistentialTypeShape);
    auto genSig = rawShape->getGeneralizationSignature();

    // Collect the type arguments; they should all be key arguments.
    if (genArgs.size() != genSig.getParams().size())
      return TYPE_LOOKUP_ERROR_FMT("Length mismatch building an extended "
                                   "existential metatype");
    llvm::SmallVector<const void *, 8> allArgsVec;

    // FIXME: variadic-generics
    for (auto arg : genArgs)
      allArgsVec.push_back(arg.getMetadata());

    // Collect any other generic arguments.
    auto error = _checkGenericRequirements(
        genSig.getRequirements(), allArgsVec,
        [genArgs](unsigned depth, unsigned index) -> const Metadata * {
          if (depth != 0 || index >= genArgs.size())
            return nullptr;

          // FIXME: variadic generics
          return genArgs[index].getMetadata();
        },
        [](const Metadata *type, unsigned index) -> const WitnessTable * {
          swift_unreachable("never called");
        });
    if (error)
      return *error;

    return BuiltType(
        swift_getExtendedExistentialTypeMetadata_unique(shape,
                                                        allArgsVec.data()));
  }

  TypeLookupErrorOr<BuiltType> createBuiltinType(StringRef builtinName,
                                                 StringRef mangledName) const {
#define BUILTIN_TYPE(Symbol, _) \
    if (mangledName.equals(#Symbol)) \
      return BuiltType(&METADATA_SYM(Symbol).base);
#if !SWIFT_STDLIB_ENABLE_VECTOR_TYPES
#define BUILTIN_VECTOR_TYPE(ElementSymbol, ElementName, Width)
#endif
#include "swift/Runtime/BuiltinTypes.def"
    return BuiltType();
  }

  TypeLookupErrorOr<BuiltType> createMetatypeType(
      BuiltType instance,
      llvm::Optional<Demangle::ImplMetatypeRepresentation> repr = None) const {
    if (!instance.isMetadata())
      return TYPE_LOOKUP_ERROR_FMT("Tried to build a metatype from a pack");
    return BuiltType(swift_getMetatypeMetadata(instance.getMetadata()));
  }

  TypeLookupErrorOr<BuiltType> createExistentialMetatypeType(
      BuiltType instance,
      llvm::Optional<Demangle::ImplMetatypeRepresentation> repr = None) const {
    if (!instance.isMetadata()) {
      return TYPE_LOOKUP_ERROR_FMT("Tried to build an existential metatype "
                                   "from a pack");
    }
    auto *instanceMetadata = instance.getMetadata();
    if (instanceMetadata->getKind() != MetadataKind::Existential
        && instanceMetadata->getKind() != MetadataKind::ExistentialMetatype) {
      return TYPE_LOOKUP_ERROR_FMT("Tried to build an existential metatype from "
                                   "a type that was neither an existential nor "
                                   "an existential metatype");
    }
    return BuiltType(swift_getExistentialMetatypeMetadata(instanceMetadata));
  }

  TypeLookupErrorOr<BuiltType>
  createProtocolCompositionType(llvm::ArrayRef<BuiltProtocolDecl> protocols,
                                BuiltType superclass, bool isClassBound,
                                bool forRequirement = true) const {
    // Determine whether we have a class bound.
    ProtocolClassConstraint classConstraint = ProtocolClassConstraint::Any;
    if (isClassBound || superclass) {
      classConstraint = ProtocolClassConstraint::Class;
    } else {
      for (auto protocol : protocols) {
        if (protocol.getClassConstraint() == ProtocolClassConstraint::Class) {
          classConstraint = ProtocolClassConstraint::Class;
          break;
        }
      }
    }

    if (!superclass.isMetadataOrNull()) {
      return TYPE_LOOKUP_ERROR_FMT("Tried to build a protocol composition where "
                                   "the superclass type is a pack");
    }
    return BuiltType(
        swift_getExistentialTypeMetadata(classConstraint,
                                         superclass.getMetadataOrNull(),
                                         protocols.size(), protocols.data()));
  }

  TypeLookupErrorOr<BuiltType>
  createConstrainedExistentialType(BuiltType base,
                                   llvm::ArrayRef<BuiltRequirement> rs) const {
    // FIXME: Runtime plumbing.
    return BuiltType();
  }

  TypeLookupErrorOr<BuiltType> createDynamicSelfType(BuiltType selfType) const {
    // Free-standing mangled type strings should not contain DynamicSelfType.
    return BuiltType();
  }

  BuiltType
  createGenericTypeParameterType(unsigned depth, unsigned index) const {
    // Use the callback, when provided.
    // FIXME: variadic-generics
    if (substGenericParameter)
      return BuiltType(substGenericParameter(depth, index));

    return BuiltType();
  }

  TypeLookupErrorOr<BuiltType>
  createFunctionType(
      llvm::ArrayRef<Demangle::FunctionParam<BuiltType>> params,
      BuiltType result, FunctionTypeFlags flags,
      FunctionMetadataDifferentiabilityKind diffKind,
      BuiltType globalActorType) const {
    assert(
        (flags.isDifferentiable() && diffKind.isDifferentiable()) ||
        (!flags.isDifferentiable() && !diffKind.isDifferentiable()));

    if (!result.isMetadata()) {
      return TYPE_LOOKUP_ERROR_FMT("Tried to build a function type where "
                                   "the result type is a pack");
    }

    llvm::SmallVector<const Metadata *, 8> paramTypes;
    llvm::SmallVector<uint32_t, 8> paramFlags;

    // Fill in the parameters.
    paramTypes.reserve(params.size());
    if (flags.hasParameterFlags())
      paramFlags.reserve(params.size());
    for (const auto &param : params) {
      if (!param.getType().isMetadata()) {
        return TYPE_LOOKUP_ERROR_FMT("Tried to build a function type where "
                                     "a parameter type is a pack");
      }
      paramTypes.push_back(param.getType().getMetadata());
      if (flags.hasParameterFlags())
        paramFlags.push_back(param.getFlags().getIntValue());
    }

    if (globalActorType) {
      if (!globalActorType.isMetadata()) {
        return TYPE_LOOKUP_ERROR_FMT("Tried to build a function type where "
                                     "the global actor type is a pack");
      }
      flags = flags.withGlobalActor(true);
    }

    return BuiltType(
      flags.hasGlobalActor()
        ? swift_getFunctionTypeMetadataGlobalActor(flags, diffKind, paramTypes.data(),
            flags.hasParameterFlags() ? paramFlags.data() : nullptr,
            result.getMetadata(), globalActorType.getMetadata())
        : flags.isDifferentiable()
          ? swift_getFunctionTypeMetadataDifferentiable(
                flags, diffKind, paramTypes.data(),
                flags.hasParameterFlags() ? paramFlags.data() : nullptr,
                result.getMetadata())
          : swift_getFunctionTypeMetadata(
                flags, paramTypes.data(),
                flags.hasParameterFlags() ? paramFlags.data() : nullptr,
                result.getMetadata()));
  }

  TypeLookupErrorOr<BuiltType> createImplFunctionType(
      Demangle::ImplParameterConvention calleeConvention,
      llvm::ArrayRef<Demangle::ImplFunctionParam<BuiltType>> params,
      llvm::ArrayRef<Demangle::ImplFunctionResult<BuiltType>> results,
      llvm::Optional<Demangle::ImplFunctionResult<BuiltType>> errorResult,
      ImplFunctionTypeFlags flags) {
    // We can't realize the metadata for a SILFunctionType.
    return BuiltType();
  }

  TypeLookupErrorOr<BuiltType>
  createTupleType(llvm::ArrayRef<BuiltType> elements,
                  std::string labels) const {
    for (auto element : elements) {
      if (!element.isMetadata()) {
        return TYPE_LOOKUP_ERROR_FMT("Tried to build a tuple type where "
                                     "an element type is a pack");
      }
    }

    auto flags = TupleTypeFlags().withNumElements(elements.size());
    if (!labels.empty())
      flags = flags.withNonConstantLabels(true);
    return BuiltType(
        swift_getTupleTypeMetadata(
          MetadataState::Abstract, flags,
          reinterpret_cast<const Metadata * const *>(elements.data()),
          labels.empty() ? nullptr : labels.c_str(),
          /*proposedWitnesses=*/nullptr));
  }

  TypeLookupErrorOr<BuiltType>
  createPackType(llvm::ArrayRef<BuiltType> elements) const {
    for (auto element : elements) {
      if (!element.isMetadata()) {
        return TYPE_LOOKUP_ERROR_FMT("Can't have nested metadata packs");
      }
    }

    MetadataPackPointer pack(swift_allocateMetadataPack(
        reinterpret_cast<const Metadata * const *>(elements.data()),
        elements.size()));

    return BuiltType(pack);
  }

  TypeLookupErrorOr<BuiltType>
  createSILPackType(llvm::ArrayRef<BuiltType> elements, bool isElementAddress) const {
    return TYPE_LOOKUP_ERROR_FMT("Lowered SILPackType cannot be demangled");
  }

  TypeLookupErrorOr<BuiltType>
  createPackExpansionType(BuiltType patternType, BuiltType countType) const {
    // FIXME: Runtime support for variadic generics.
    return BuiltType();
  }

  TypeLookupErrorOr<BuiltType> createDependentMemberType(StringRef name,
                                                         BuiltType base) const {
    return TYPE_LOOKUP_ERROR_FMT("Unbound dependent member type cannot be demangled");
  }

  TypeLookupErrorOr<BuiltType>
  createDependentMemberType(StringRef name, BuiltType base,
                            BuiltProtocolDecl protocol) const {
#if SWIFT_OBJC_INTEROP
    if (protocol.isObjC())
      return BuiltType();
#endif

    auto swiftProtocol = protocol.getSwiftProtocol();

    // Look for the named associated type within the protocol.
    auto assocType = findAssociatedTypeByName(swiftProtocol, name);
    if (!assocType) return BuiltType();

    auto projectDependentMemberType = [&](const Metadata *baseMetadata) -> const Metadata * {
      auto witnessTable = swift_conformsToProtocol(baseMetadata, swiftProtocol);
      if (!witnessTable)
        return nullptr;

      // Call the associated type access function.
  #if SWIFT_STDLIB_USE_RELATIVE_PROTOCOL_WITNESS_TABLES
      auto tbl = reinterpret_cast<RelativeWitnessTable *>(
        const_cast<WitnessTable *>(witnessTable));
      return swift_getAssociatedTypeWitnessRelative(
                                   MetadataState::Abstract,
                                   tbl,
                                   baseMetadata,
                                   swiftProtocol->getRequirementBaseDescriptor(),
                                   *assocType).Value;
  #else
      return swift_getAssociatedTypeWitness(
                                   MetadataState::Abstract,
                                   const_cast<WitnessTable *>(witnessTable),
                                   baseMetadata,
                                   swiftProtocol->getRequirementBaseDescriptor(),
                                   *assocType).Value;
  #endif
    };

    if (base.isMetadata()) {
      return BuiltType(projectDependentMemberType(base.getMetadata()));
    } else {
      MetadataPackPointer basePack = base.getMetadataPack();

      llvm::SmallVector<const Metadata *, 4> packElts;
      for (size_t i = 0, e = basePack.getNumElements(); i < e; ++i) {
        auto *projectedElt = projectDependentMemberType(basePack.getElements()[i]);
        packElts.push_back(projectedElt);
      }

      return BuiltType(swift_allocateMetadataPack(packElts.data(), packElts.size()));
    }
  }

#define REF_STORAGE(Name, ...)                                                 \
  TypeLookupErrorOr<BuiltType> create##Name##StorageType(BuiltType base) {     \
    ReferenceOwnership.set##Name();                                            \
    return base;                                                               \
  }
#include "swift/AST/ReferenceStorage.def"

  TypeLookupErrorOr<BuiltType> createSILBoxType(BuiltType base) const {
    // FIXME: Implement.
    return BuiltType();
  }

  struct BuiltSILBoxField {
    BuiltType Type;
    bool Mutable;

    BuiltSILBoxField(BuiltType type, bool isMutable)
      : Type(type), Mutable(isMutable) {}
  };

  BuiltLayoutConstraint getLayoutConstraint(LayoutConstraintKind kind) {
    return {};
  }
  BuiltLayoutConstraint
  getLayoutConstraintWithSizeAlign(LayoutConstraintKind kind, unsigned size,
                                   unsigned alignment) {
    return {};
  }

  TypeLookupErrorOr<BuiltType> createSILBoxTypeWithLayout(
      llvm::ArrayRef<BuiltSILBoxField> Fields,
      llvm::ArrayRef<BuiltSubstitution> Substitutions,
      llvm::ArrayRef<BuiltRequirement> Requirements) const {
    // FIXME: Implement.
    return BuiltType();
  }

  bool isExistential(BuiltType) {
    // FIXME: Implement.
    return true;
  }

  TypeReferenceOwnership getReferenceOwnership() const {
    return ReferenceOwnership;
  }

  TypeLookupErrorOr<BuiltType> createOptionalType(BuiltType base) {
    // Mangled types for building metadata don't contain sugared types
    return BuiltType();
  }

  TypeLookupErrorOr<BuiltType> createArrayType(BuiltType base) {
    // Mangled types for building metadata don't contain sugared types
    return BuiltType();
  }

  TypeLookupErrorOr<BuiltType> createDictionaryType(BuiltType key,
                                                    BuiltType value) {
    // Mangled types for building metadata don't contain sugared types
    return BuiltType();
  }

  TypeLookupErrorOr<BuiltType> createParenType(BuiltType base) {
    // Mangled types for building metadata don't contain sugared types
    return BuiltType();
  }
};

}

SWIFT_CC(swift)
static TypeLookupErrorOr<TypeInfo>
swift_getTypeByMangledNodeImpl(MetadataRequest request, Demangler &demangler,
                               Demangle::NodePointer node,
                               const void *const *origArgumentVector,
                               SubstGenericParameterFn substGenericParam,
                               SubstDependentWitnessTableFn substWitnessTable) {
  // Simply call an accessor function if that's all we got.
  if (node->getKind() == Node::Kind::AccessorFunctionReference) {
    // The accessor function is passed the pointer to the original argument
    // buffer. It's assumed to match the generic context.
    auto accessorFn =
        (const Metadata *(*)(const void * const *))node->getIndex();
    auto type = accessorFn(origArgumentVector);
    // We don't call checkMetadataState here since the result may not really
    // *be* type metadata. If the accessor returns a type, it is responsible
    // for completing the metadata.
    return TypeInfo{MetadataResponse{type, MetadataState::Complete},
                    TypeReferenceOwnership()};
  }
  
  // TODO: propagate the request down to the builder instead of calling
  // swift_checkMetadataState after the fact.
  DecodedMetadataBuilder builder(demangler, substGenericParam,
                                 substWitnessTable);
  auto type = Demangle::decodeMangledType(builder, node);
  if (type.isError()) {
    return *type.getError();
  }
  if (!type.getType()) {
    return TypeLookupError("NULL type but no error provided");
  }

  if (!type.getType().isMetadata()) {
    return TypeLookupError("Cannot demangle a free-standing pack");
  }

  return TypeInfo{swift_checkMetadataState(request,
                  type.getType().getMetadata()),
                  builder.getReferenceOwnership()};
}

SWIFT_CC(swift)
static TypeLookupErrorOr<TypeInfo>
swift_getTypeByMangledNameImpl(MetadataRequest request, StringRef typeName,
                               const void *const *origArgumentVector,
                               SubstGenericParameterFn substGenericParam,
                               SubstDependentWitnessTableFn substWitnessTable) {
  DemanglerForRuntimeTypeResolution<StackAllocatedDemangler<2048>> demangler;

  NodePointer node;

  // Check whether this is the convenience syntax "ModuleName.ClassName".
  auto getDotPosForConvenienceSyntax = [&]() -> size_t {
    size_t dotPos = llvm::StringRef::npos;
    for (unsigned i = 0; i < typeName.size(); ++i) {
      // Should only contain one dot.
      if (typeName[i] == '.') {
        if (dotPos == llvm::StringRef::npos) {
          dotPos = i;
          continue;
        } else {
          return llvm::StringRef::npos;
        }
      }
      
      // Should not contain symbolic references.
      if ((unsigned char)typeName[i] <= '\x1F') {
        return llvm::StringRef::npos;
      }
    }
    return dotPos;
  };

  auto dotPos = getDotPosForConvenienceSyntax();
  if (dotPos != llvm::StringRef::npos) {
    // Form a demangle tree for this class.
    NodePointer classNode = demangler.createNode(Node::Kind::Class);
    NodePointer moduleNode = demangler.createNode(Node::Kind::Module,
                                                  typeName.substr(0, dotPos));
    NodePointer nameNode = demangler.createNode(Node::Kind::Identifier,
                                                typeName.substr(dotPos + 1));
    classNode->addChild(moduleNode, demangler);
    classNode->addChild(nameNode, demangler);

    node = classNode;
  } else {
    // Demangle the type name.
    node = demangler.demangleTypeRef(typeName);
    if (!node) {
      return TypeInfo();
    }
  }

  return swift_getTypeByMangledNode(request, demangler, node,
                                    origArgumentVector,
                                    substGenericParam, substWitnessTable);
}

SWIFT_CC(swift) SWIFT_RUNTIME_EXPORT
const Metadata * _Nullable
swift_getTypeByMangledNameInEnvironment(
                        const char *typeNameStart,
                        size_t typeNameLength,
                        const TargetGenericEnvironment<InProcess> *environment,
                        const void * const *genericArgs) {
  llvm::StringRef typeName(typeNameStart, typeNameLength);
  SubstGenericParametersFromMetadata substitutions(environment, genericArgs);
  TypeLookupErrorOr<TypeInfo> result = swift_getTypeByMangledName(
    MetadataState::Complete, typeName,
    genericArgs,
    [&substitutions](unsigned depth, unsigned index) {
      // FIXME: Variadic generics
      return substitutions.getMetadata(depth, index).getMetadataOrNull();
    },
    [&substitutions](const Metadata *type, unsigned index) {
      return substitutions.getWitnessTable(type, index);
    });
  if (result.isError()
      && runtime::environment::SWIFT_DEBUG_FAILED_TYPE_LOOKUP()) {
    TypeLookupError *error = result.getError();
    char *errorString = error->copyErrorString();
    swift::warning(0, "failed type lookup for %.*s: %s\n",
                   (int)typeNameLength, typeNameStart,
                   errorString);
    error->freeErrorString(errorString);
    return nullptr;
  }
  return result.getType().getMetadata();
}

SWIFT_CC(swift) SWIFT_RUNTIME_EXPORT
const Metadata * _Nullable
swift_getTypeByMangledNameInEnvironmentInMetadataState(
                        size_t metadataState,
                        const char *typeNameStart,
                        size_t typeNameLength,
                        const TargetGenericEnvironment<InProcess> *environment,
                        const void * const *genericArgs) {
  llvm::StringRef typeName(typeNameStart, typeNameLength);
  SubstGenericParametersFromMetadata substitutions(environment, genericArgs);
  TypeLookupErrorOr<TypeInfo> result = swift_getTypeByMangledName(
    (MetadataState)metadataState, typeName,
    genericArgs,
    [&substitutions](unsigned depth, unsigned index) {
      // FIXME: Variadic generics
      return substitutions.getMetadata(depth, index).getMetadataOrNull();
    },
    [&substitutions](const Metadata *type, unsigned index) {
      return substitutions.getWitnessTable(type, index);
    });
  if (result.isError()
      && runtime::environment::SWIFT_DEBUG_FAILED_TYPE_LOOKUP()) {
    TypeLookupError *error = result.getError();
    char *errorString = error->copyErrorString();
    swift::warning(0, "failed type lookup for %.*s: %s\n",
                   (int)typeNameLength, typeNameStart,
                   errorString);
    error->freeErrorString(errorString);
    return nullptr;
  }
  return result.getType().getMetadata();
}

SWIFT_CC(swift) SWIFT_RUNTIME_EXPORT
const Metadata * _Nullable
swift_getTypeByMangledNameInContext(
                        const char *typeNameStart,
                        size_t typeNameLength,
                        const TargetContextDescriptor<InProcess> *context,
                        const void * const *genericArgs) {
  llvm::StringRef typeName(typeNameStart, typeNameLength);
  SubstGenericParametersFromMetadata substitutions(context, genericArgs);
  TypeLookupErrorOr<TypeInfo> result = swift_getTypeByMangledName(
    MetadataState::Complete, typeName,
    genericArgs,
    [&substitutions](unsigned depth, unsigned index) {
      // FIXME: Variadic generics
      return substitutions.getMetadata(depth, index).getMetadataOrNull();
    },
    [&substitutions](const Metadata *type, unsigned index) {
      return substitutions.getWitnessTable(type, index);
    });
  if (result.isError()
      && runtime::environment::SWIFT_DEBUG_FAILED_TYPE_LOOKUP()) {
    TypeLookupError *error = result.getError();
    char *errorString = error->copyErrorString();
    swift::warning(0, "failed type lookup for %.*s: %s\n",
                   (int)typeNameLength, typeNameStart,
                   errorString);
    error->freeErrorString(errorString);
    return nullptr;
  }
  return result.getType().getMetadata();
}

SWIFT_CC(swift) SWIFT_RUNTIME_EXPORT
const Metadata * _Nullable
swift_getTypeByMangledNameInContextInMetadataState(
                        size_t metadataState,
                        const char *typeNameStart,
                        size_t typeNameLength,
                        const TargetContextDescriptor<InProcess> *context,
                        const void * const *genericArgs) {
  llvm::StringRef typeName(typeNameStart, typeNameLength);
  SubstGenericParametersFromMetadata substitutions(context, genericArgs);
  TypeLookupErrorOr<TypeInfo> result = swift_getTypeByMangledName(
    (MetadataState)metadataState, typeName,
    genericArgs,
    [&substitutions](unsigned depth, unsigned index) {
      // FIXME: Variadic generics
      return substitutions.getMetadata(depth, index).getMetadataOrNull();
    },
    [&substitutions](const Metadata *type, unsigned index) {
      return substitutions.getWitnessTable(type, index);
    });
  if (result.isError()
      && runtime::environment::SWIFT_DEBUG_FAILED_TYPE_LOOKUP()) {
    TypeLookupError *error = result.getError();
    char *errorString = error->copyErrorString();
    swift::warning(0, "failed type lookup for %.*s: %s\n",
                   (int)typeNameLength, typeNameStart,
                   errorString);
    error->freeErrorString(errorString);
    return nullptr;
  }
  return result.getType().getMetadata();

}

/// Demangle a mangled name, but don't allow symbolic references.
SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_INTERNAL
const Metadata *_Nullable
swift_stdlib_getTypeByMangledNameUntrusted(const char *typeNameStart,
                                           size_t typeNameLength) {
  llvm::StringRef typeName(typeNameStart, typeNameLength);
  for (char c : typeName) {
    if (c >= '\x01' && c <= '\x1F')
      return nullptr;
  }

  return swift_getTypeByMangledName(MetadataState::Complete, typeName, nullptr,
                                    {}, {}).getType().getMetadata();
}

TypeLookupErrorOr<MetadataPackPointer>
swift::getTypePackByMangledName(StringRef typeName,
                                const void *const *origArgumentVector,
                                SubstGenericParameterFn substGenericParam,
                                SubstDependentWitnessTableFn substWitnessTable) {
  DemanglerForRuntimeTypeResolution<StackAllocatedDemangler<2048>> demangler;

  NodePointer node = demangler.demangleTypeRef(typeName);
  if (!node)
    return TypeLookupError("Demangling failed");

  DecodedMetadataBuilder builder(demangler, substGenericParam,
                                 substWitnessTable);
  auto type = Demangle::decodeMangledType(builder, node);
  if (type.isError()) {
    return *type.getError();
  }
  if (!type.getType()) {
    return TypeLookupError("NULL type but no error provided");
  }

  if (!type.getType().isMetadataPack()) {
    return TypeLookupError("This entry point is only for packs");
  }

  return type.getType().getMetadataPack();
}

// ==== Function metadata functions ----------------------------------------------

static llvm::Optional<llvm::StringRef>
cstrToStringRef(const char *typeNameStart, size_t typeNameLength) {
  llvm::StringRef typeName(typeNameStart, typeNameLength);
  for (char c : typeName) {
    if (c >= '\x01' && c <= '\x1F')
      return llvm::None;
  }
  return typeName;
}

/// Given mangling for a method, extract its function type in demangled
/// representation.
static NodePointer extractFunctionTypeFromMethod(Demangler &demangler,
                                                 const char *typeNameStart,
                                                 size_t typeNameLength) {
  llvm::Optional<llvm::StringRef> typeName =
      cstrToStringRef(typeNameStart, typeNameLength);
  if (!typeName)
    return nullptr;

  auto node = demangler.demangleSymbol(*typeName);
  if (!node)
    return nullptr;

  node = node->findByKind(Node::Kind::Function, /*maxDepth=*/2);
  if (!node)
    return nullptr;

  node = node->findByKind(Node::Kind::Type, /*maxDepth=*/2);
  if (!node)
    return nullptr;

  // If this is a generic function, it requires special handling.
  if (auto genericType =
          node->findByKind(Node::Kind::DependentGenericType, /*maxDepth=*/1)) {
    node = genericType->findByKind(Node::Kind::Type, /*maxDepth=*/1);
    return node->findByKind(Node::Kind::FunctionType, /*maxDepth=*/1);
  }

  auto funcType = node->getFirstChild();
  assert(funcType->getKind() == Node::Kind::FunctionType);
  return funcType;
}

/// For a single unlabeled parameter this function returns whole
/// `ArgumentTuple`, for everything else a `Tuple` element inside it.
static NodePointer getParameterList(NodePointer funcType) {
  assert(funcType->getKind() == Node::Kind::FunctionType);

  auto parameterContainer =
      funcType->findByKind(Node::Kind::ArgumentTuple, /*maxDepth=*/1);
  assert(parameterContainer->getNumChildren() > 0);

  // This is a type that covers entire parameter list.
  auto parameterList = parameterContainer->getFirstChild();
  assert(parameterList->getKind() == Node::Kind::Type);

  auto parameters = parameterList->getFirstChild();
  if (parameters->getKind() == Node::Kind::Tuple)
    return parameters;

  return parameterContainer;
}

static const Metadata *decodeType(TypeDecoder<DecodedMetadataBuilder> &decoder,
                                  NodePointer type) {
  assert(type->getKind() == Node::Kind::Type);

  auto builtTypeOrError = decoder.decodeMangledType(type);

  if (builtTypeOrError.isError()) {
    auto err = builtTypeOrError.getError();
    char *errStr = err->copyErrorString();
    err->freeErrorString(errStr);
    return nullptr;
  }

  if (!builtTypeOrError.getType().isMetadata())
    return nullptr;

  return builtTypeOrError.getType().getMetadata();
}

SWIFT_CC(swift)
SWIFT_RUNTIME_STDLIB_SPI
unsigned swift_func_getParameterCount(const char *typeNameStart,
                                      size_t typeNameLength) {
  StackAllocatedDemangler<1024> demangler;

  auto funcType =
      extractFunctionTypeFromMethod(demangler, typeNameStart, typeNameLength);
  if (!funcType)
    return -1;

  auto parameterList = getParameterList(funcType);
  return parameterList->getNumChildren();
}

SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_SPI
const Metadata *_Nullable
swift_func_getReturnTypeInfo(const char *typeNameStart, size_t typeNameLength,
                             GenericEnvironmentDescriptor *genericEnv,
                             const void * const *genericArguments) {
  StackAllocatedDemangler<1024> demangler;

  auto *funcType =
      extractFunctionTypeFromMethod(demangler, typeNameStart, typeNameLength);
  if (!funcType)
    return nullptr;

  auto resultType = funcType->getLastChild();
  if (!resultType)
    return nullptr;

  assert(resultType->getKind() == Node::Kind::ReturnType);

  SubstGenericParametersFromMetadata substFn(genericEnv, genericArguments);

  DecodedMetadataBuilder builder(
      demangler,
      /*substGenericParam=*/
      [&substFn](unsigned depth, unsigned index) {
        // FIXME: Variadic generics
        return substFn.getMetadata(depth, index).getMetadataOrNull();
      },
      /*SubstDependentWitnessTableFn=*/
      [&substFn](const Metadata *type, unsigned index) {
        return substFn.getWitnessTable(type, index);
      });

  TypeDecoder<DecodedMetadataBuilder> decoder(builder);

  return decodeType(decoder, resultType->getFirstChild());
}

SWIFT_CC(swift) SWIFT_RUNTIME_STDLIB_SPI
unsigned
swift_func_getParameterTypeInfo(
    const char *typeNameStart, size_t typeNameLength,
    GenericEnvironmentDescriptor *genericEnv,
    const void * const *genericArguments,
    Metadata const **types, unsigned typesLength) {
  if (typesLength < 0) return -1;

  StackAllocatedDemangler<1024> demangler;

  auto *funcType =
      extractFunctionTypeFromMethod(demangler, typeNameStart, typeNameLength);
  if (!funcType)
    return -1;

  auto parameterList = getParameterList(funcType);

  // Only successfully return if the expected parameter count is the same
  // as space prepared for it in the buffer.
  if (!(parameterList && parameterList->getNumChildren() == typesLength))
    return -2;

  SubstGenericParametersFromMetadata substFn(genericEnv, genericArguments);

  DecodedMetadataBuilder builder(
      demangler,
      /*substGenericParam=*/
      [&substFn](unsigned depth, unsigned index) {
        // FIXME: Variadic generics
        return substFn.getMetadata(depth, index).getMetadataOrNull();
      },
      /*SubstDependentWitnessTableFn=*/
      [&substFn](const Metadata *type, unsigned index) {
        return substFn.getWitnessTable(type, index);
      });
  TypeDecoder<DecodedMetadataBuilder> decoder(builder);

  // for each parameter (TupleElement), store it into the provided buffer
  for (unsigned index = 0; index != typesLength; ++index) {
    auto *parameter = parameterList->getChild(index);

    if (parameter->getKind() == Node::Kind::TupleElement) {
      assert(parameter->getNumChildren() == 1);
      parameter = parameter->getFirstChild();
    }

    assert(parameter->getKind() == Node::Kind::Type);

    auto type = decodeType(decoder, parameter);
    if (!type)
      return -3; // Failed to decode a type.

    types[index] = type;
  } // end foreach parameter

  return typesLength;
}

SWIFT_CC(swift)
SWIFT_RUNTIME_STDLIB_SPI
BufferAndSize
swift_distributed_getWitnessTables(GenericEnvironmentDescriptor *genericEnv,
                                   const void *const *genericArguments) {
  assert(genericEnv);
  assert(genericArguments);

  llvm::SmallVector<const void *, 4> witnessTables;
  SubstGenericParametersFromMetadata substFn(genericEnv, genericArguments);

  auto error = _checkGenericRequirements(
      genericEnv->getGenericRequirements(), witnessTables,
      [&substFn](unsigned depth, unsigned index) {
        // FIXME: Variadic generics
        return substFn.getMetadata(depth, index).getMetadataOrNull();
      },
      [&substFn](const Metadata *type, unsigned index) {
        return substFn.getWitnessTable(type, index);
      });

  if (error) {
    return {/*ptr=*/nullptr, -1};
  }

  if (witnessTables.empty())
    return {/*ptr=*/nullptr, 0};

  void **tables = (void **)malloc(witnessTables.size() * sizeof(void *));
  for (unsigned i = 0, n = witnessTables.size(); i != n; ++i)
    tables[i] = const_cast<void *>(witnessTables[i]);

  return {tables, static_cast<intptr_t>(witnessTables.size())};
}

// ==== End of Function metadata functions ---------------------------------------

SWIFT_CC(swift) SWIFT_RUNTIME_EXPORT
MetadataResponse
swift_getOpaqueTypeMetadata(MetadataRequest request,
                            const void * const *arguments,
                            const OpaqueTypeDescriptor *descriptor,
                            unsigned index) {
  auto mangledName = descriptor->getUnderlyingTypeArgument(index);
  SubstGenericParametersFromMetadata substitutions(descriptor, arguments);

  return swift_getTypeByMangledName(request.getState(),
                                    mangledName, arguments,
    [&substitutions](unsigned depth, unsigned index) {
      // FIXME: Variadic generics
      return substitutions.getMetadata(depth, index).getMetadataOrNull();
    },
    [&substitutions](const Metadata *type, unsigned index) {
      return substitutions.getWitnessTable(type, index);
    }).getType().getResponse();
}

SWIFT_CC(swift) SWIFT_RUNTIME_EXPORT
const WitnessTable *
swift_getOpaqueTypeConformance(const void * const *arguments,
                               const OpaqueTypeDescriptor *descriptor,
                               unsigned index) {
  auto response = swift_getOpaqueTypeMetadata(
                                    MetadataRequest(MetadataState::Complete),
                                    arguments, descriptor, index);
  return (const WitnessTable *)response.Value;
}

#if SWIFT_OBJC_INTEROP

// Return the ObjC class for the given type name.
// This gets installed as a callback from libobjc.

static bool validateObjCMangledName(const char *_Nonnull typeName) {
  // Accept names with a mangling prefix.
  if (getManglingPrefixLength(typeName))
    return true;

  // Accept names that start with a digit (unprefixed mangled names).
  if (isdigit(typeName[0]))
    return true;

  // Accept names that contain a dot.
  if (strchr(typeName, '.'))
    return true;

  // Reject anything else.
  return false;
}

// FIXME: delete this #if and dlsym once we don't
// need to build with older libobjc headers
#if !OBJC_GETCLASSHOOK_DEFINED
using objc_hook_getClass =  BOOL(*)(const char * _Nonnull name,
                                    Class _Nullable * _Nonnull outClass);
#endif
static objc_hook_getClass OldGetClassHook;

static BOOL
getObjCClassByMangledName(const char * _Nonnull typeName,
                          Class _Nullable * _Nonnull outClass) {
  // Demangle old-style class and protocol names, which are still used in the
  // ObjC metadata.
  StringRef typeStr(typeName);
  const Metadata *metadata = nullptr;
  if (typeStr.startswith("_Tt")) {
    Demangler demangler;
    auto node = demangler.demangleSymbol(typeName);
    if (!node)
      return NO;

    // If we successfully demangled but there is a suffix, then we did NOT use
    // the entire name, and this is NOT a match. Reject it.
    if (node->hasChildren() &&
        node->getLastChild()->getKind() == Node::Kind::Suffix)
      return NO;

    metadata = swift_getTypeByMangledNode(
      MetadataState::Complete, demangler, node,
      nullptr,
      /* no substitutions */
      [&](unsigned depth, unsigned index) { return nullptr; },
      [&](const Metadata *type, unsigned index) { return nullptr; }
    ).getType().getMetadata();
  } else {
    if (validateObjCMangledName(typeName))
      metadata = swift_stdlib_getTypeByMangledNameUntrusted(typeStr.data(),
                                                            typeStr.size());
  }
  if (metadata) {
    auto objcClass =
      reinterpret_cast<Class>(
        const_cast<ClassMetadata *>(
          swift_getObjCClassFromMetadataConditional(metadata)));

    if (objcClass) {
      *outClass = objcClass;
      return YES;
    }
  }

  return OldGetClassHook(typeName, outClass);
}

__attribute__((constructor))
static void installGetClassHook() {
  if (SWIFT_RUNTIME_WEAK_CHECK(objc_setHook_getClass)) {
    SWIFT_RUNTIME_WEAK_USE(objc_setHook_getClass(getObjCClassByMangledName, &OldGetClassHook));
  }
}

#endif

unsigned SubstGenericParametersFromMetadata::
buildDescriptorPath(const ContextDescriptor *context,
                    Demangler &borrowFrom) const {
  assert(sourceKind == SourceKind::Metadata);

  // Terminating condition: we don't have a context.
  if (!context)
    return 0;

  DemanglerForRuntimeTypeResolution<> demangler;
  demangler.providePreallocatedMemory(borrowFrom);

  if (auto extension = _findExtendedTypeContextDescriptor(context, demangler)) {
    // If we have a nominal type extension descriptor, extract the extended type
    // and use that. If the extension is not nominal, then we can use the
    // extension's own signature.
    context = extension;
  }

  // Add the parent's contribution to the descriptor path.
  const ContextDescriptor *parent = context->Parent.get();
  unsigned numKeyGenericParamsInParent = buildDescriptorPath(parent, demangler);

  // If this context is non-generic, we're done.
  if (!context->isGeneric())
    return numKeyGenericParamsInParent;

  // Count the number of key generic params at this level.
  auto allGenericParams = baseContext->getGenericContext()->getGenericParams();
  unsigned parentCount = parent->getNumGenericParams();
  unsigned localCount = context->getNumGenericParams();
  auto localGenericParams = allGenericParams.slice(parentCount,
                                                   localCount - parentCount);

  unsigned numKeyGenericParamsHere = 0;
  bool hasNonKeyGenericParams = false;
  for (const auto &genericParam : localGenericParams) {
    if (genericParam.hasKeyArgument())
      ++numKeyGenericParamsHere;
    else
      hasNonKeyGenericParams = true;
  }

  // Form the path element if there are any new generic parameters.
  if (localCount > parentCount)
    descriptorPath.push_back(PathElement{localGenericParams,
                                         context->getNumGenericParams(),
                                         numKeyGenericParamsInParent,
                                         numKeyGenericParamsHere,
                                         hasNonKeyGenericParams});
  return numKeyGenericParamsInParent + numKeyGenericParamsHere;
}

  /// Builds a path from the generic environment.
unsigned SubstGenericParametersFromMetadata::
buildEnvironmentPath(
    const TargetGenericEnvironment<InProcess> *environment) const {
  unsigned totalParamCount = 0;
  unsigned totalKeyParamCount = 0;
  auto genericParams = environment->getGenericParameters();
  for (unsigned numLocalParams : environment->getGenericParameterCounts()) {
    // Adjust totalParamCount so we have the # of local parameters.
    numLocalParams -= totalParamCount;

    // Get the local generic parameters.
    auto localGenericParams = genericParams.slice(0, numLocalParams);
    genericParams = genericParams.slice(numLocalParams);

    // Count the parameters.
    unsigned numKeyGenericParamsInParent = totalKeyParamCount;
    unsigned numKeyGenericParamsHere = 0;
    bool hasNonKeyGenericParams = false;
    for (const auto &genericParam : localGenericParams) {
      if (genericParam.hasKeyArgument())
        ++numKeyGenericParamsHere;
      else
        hasNonKeyGenericParams = true;
    }

    // Update totals.
    totalParamCount += numLocalParams;
    totalKeyParamCount += numKeyGenericParamsHere;

    // Add to the descriptor path.
    descriptorPath.push_back(PathElement{localGenericParams,
                                         totalParamCount,
                                         numKeyGenericParamsInParent,
                                         numKeyGenericParamsHere,
                                         hasNonKeyGenericParams});
  }

  return totalKeyParamCount;
}

unsigned SubstGenericParametersFromMetadata::buildShapePath(
    const TargetExtendedExistentialTypeShape<InProcess> *shape) const {
  unsigned totalParamCount = 0;

  auto genSig = shape->getGeneralizationSignature();
  if (!genSig.getParams().empty()) {
    totalParamCount += genSig.getParams().size();
    descriptorPath.push_back(PathElement{genSig.getParams(),
                                         totalParamCount,
                                         /*numKeyGenericParamsInParent*/ 0,
                                         (unsigned)genSig.getParams().size(),
                                         /*hasNonKeyGenericParams*/ false});
  }

  const unsigned genSigParamCount = genSig.getParams().size();
  auto reqSig = shape->getRequirementSignature();
  assert(reqSig.getParams().size() > genSig.getParams().size());
  {
    auto remainingParams = reqSig.getParams().drop_front(genSig.getParams().size());
    totalParamCount += remainingParams.size();
    descriptorPath.push_back(PathElement{remainingParams,
                                         totalParamCount,
                                         genSigParamCount,
                                         (unsigned)remainingParams.size(),
                                         /*hasNonKeyGenericParams*/ false});
  }

  // All parameters in this signature are key parameters.
  return totalParamCount;
}

void SubstGenericParametersFromMetadata::setup() const {
  if (!descriptorPath.empty())
    return;

  switch (sourceKind) {
  case SourceKind::Metadata: {
    assert(baseContext);
    DemanglerForRuntimeTypeResolution<StackAllocatedDemangler<2048>> demangler;
    numKeyGenericParameters = buildDescriptorPath(baseContext, demangler);
    return;
  }
  case SourceKind::Environment: {
    assert(environment);
    numKeyGenericParameters = buildEnvironmentPath(environment);
    return;
  }
  case SourceKind::Shape: {
    assert(shape);
    numKeyGenericParameters = buildShapePath(shape);
    return;
  }
  }
}

MetadataOrPack
SubstGenericParametersFromMetadata::getMetadata(
                                        unsigned depth, unsigned index) const {
  // On first access, compute the descriptor path.
  setup();

  // If the depth is too great, there is nothing to do.
  if (depth >= descriptorPath.size())
    return MetadataOrPack();

  /// Retrieve the descriptor path element at this depth.
  auto &pathElement = descriptorPath[depth];

  // Check whether the index is clearly out of bounds.
  if (index >= pathElement.numTotalGenericParams)
    return MetadataOrPack();

  // Compute the flat index.
  unsigned flatIndex = pathElement.numKeyGenericParamsInParent;
  if (pathElement.hasNonKeyGenericParams > 0) {
    // We have non-key generic parameters at this level, so the index needs to
    // be checked more carefully.
    auto genericParams = pathElement.localGenericParams;

    // Make sure that the requested parameter itself has a key argument.
    if (!genericParams[index].hasKeyArgument())
      return MetadataOrPack();

    // Increase the flat index for each parameter with a key argument, up to
    // the given index.
    for (const auto &genericParam : genericParams.slice(0, index)) {
      if (genericParam.hasKeyArgument())
        ++flatIndex;
    }
  } else {
    flatIndex += index;
  }

  return MetadataOrPack(genericArgs[flatIndex]);
}

const WitnessTable *
SubstGenericParametersFromMetadata::getWitnessTable(const Metadata *type,
                                                    unsigned index) const {
  // On first access, compute the descriptor path.
  setup();

  return (const WitnessTable *)genericArgs[index + numKeyGenericParameters];
}

MetadataOrPack SubstGenericParametersFromWrittenArgs::getMetadata(
                                        unsigned depth, unsigned index) const {
  if (auto flatIndex =
          _depthIndexToFlatIndex(depth, index, genericParamCounts)) {
    if (*flatIndex < allGenericArgs.size()) {
      return MetadataOrPack(allGenericArgs[*flatIndex]);
    }
  }

  return MetadataOrPack();
}

const WitnessTable *
SubstGenericParametersFromWrittenArgs::getWitnessTable(const Metadata *type,
                                                       unsigned index) const {
  return nullptr;
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

static void _gatherWrittenGenericArgs(
    const Metadata *metadata, const TypeContextDescriptor *description,
    llvm::SmallVectorImpl<MetadataOrPack> &allGenericArgs,
    Demangler &BorrowFrom) {
  if (!description)
    return;
  auto generics = description->getGenericContext();
  if (!generics)
    return;

  bool missingWrittenArguments = false;
  auto genericArgs = description->getGenericArguments(metadata);
  for (auto param : generics->getGenericParams()) {
    switch (param.getKind()) {
    case GenericParamKind::Type:
      // The type should have a key argument unless it's been same-typed to
      // another type.
      if (param.hasKeyArgument()) {
        auto genericArg = *genericArgs++;
        allGenericArgs.push_back(MetadataOrPack(genericArg));
      } else {
        // Leave a gap for us to fill in by looking at same type info.
        allGenericArgs.push_back(MetadataOrPack());
        missingWrittenArguments = true;
      }

      break;

    case GenericParamKind::TypePack:
      // The type should have a key argument unless it's been same-typed to
      // another type.
      if (param.hasKeyArgument()) {
        auto genericArg = reinterpret_cast<const Metadata * const *>(*genericArgs++);
        MetadataPackPointer pack(genericArg);
        allGenericArgs.push_back(MetadataOrPack(pack));
      } else {
        // Leave a gap for us to fill in by looking at same type info.
        allGenericArgs.push_back(MetadataOrPack());
        missingWrittenArguments = true;
      }

      break;

    default:
      // We don't know about this kind of parameter. Create placeholders where
      // needed.
      if (param.hasKeyArgument()) {
        allGenericArgs.push_back(MetadataOrPack());
        ++genericArgs;
      }

      break;
    }
  }

  // If there is no follow-up work to do, we're done.
  if (!missingWrittenArguments)
    return;

  // We have generic arguments that would be written, but have been
  // canonicalized away. Use same-type requirements to reconstitute them.

  // Retrieve the mapping information needed for depth/index -> flat index.
  llvm::SmallVector<unsigned, 8> genericParamCounts;
  (void)_gatherGenericParameterCounts(description, genericParamCounts,
                                      BorrowFrom);

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
    if (!lhsFlatIndex || *lhsFlatIndex >= allGenericArgs.size())
      continue;

    if (!allGenericArgs[*lhsFlatIndex]) {
      // Substitute into the right-hand side.
      SubstGenericParametersFromWrittenArgs substitutions(allGenericArgs,
                                                          genericParamCounts);
      allGenericArgs[*lhsFlatIndex] =
          MetadataOrPack(swift_getTypeByMangledName(MetadataState::Abstract,
            req.getMangledTypeName(),
            (const void * const *)allGenericArgs.data(),
            [&substitutions](unsigned depth, unsigned index) {
              // FIXME: Variadic generics
              return substitutions.getMetadata(depth, index).getMetadataOrNull();
            },
            [&substitutions](const Metadata *type, unsigned index) {
              return substitutions.getWitnessTable(type, index);
            }).getType().getMetadata());
      continue;
    }

    // If we do have an argument for this parameter, it might be that
    // the right-hand side is itself a generic parameter, which means
    // we have a same-type constraint A == B where A is already filled in.
    auto rhsParam = demangleToGenericParamRef(req.getMangledTypeName());
    if (!rhsParam)
      continue;

    auto rhsFlatIndex =
      _depthIndexToFlatIndex(rhsParam->first, rhsParam->second,
                             genericParamCounts);
    if (!rhsFlatIndex || *rhsFlatIndex >= allGenericArgs.size())
      continue;

    if (allGenericArgs[*rhsFlatIndex] || !allGenericArgs[*lhsFlatIndex])
      continue;

    allGenericArgs[*rhsFlatIndex] = allGenericArgs[*lhsFlatIndex];
  }
}

struct InitializeDynamicReplacementLookup {
  InitializeDynamicReplacementLookup() {
    initializeDynamicReplacementLookup();
  }
};

SWIFT_ALLOWED_RUNTIME_GLOBAL_CTOR_BEGIN
static InitializeDynamicReplacementLookup initDynamicReplacements;
SWIFT_ALLOWED_RUNTIME_GLOBAL_CTOR_END

void DynamicReplacementDescriptor::enableReplacement() const {
  // Weakly linked symbols can be zero.
  if (replacedFunctionKey.get() == nullptr)
    return;

  auto *chainRoot = const_cast<DynamicReplacementChainEntry *>(
      replacedFunctionKey->root.get());

  // Make sure this entry is not already enabled.
  // This does not work until we make sure that when a dynamic library is
  // unloaded all descriptors are removed.
#if 0
  for (auto *curr = chainRoot; curr != nullptr; curr = curr->next) {
    if (curr == chainEntry.get()) {
      swift::swift_abortDynamicReplacementEnabling();
    }
  }
#endif

  // Unlink the previous entry if we are not chaining.
  if (!shouldChain() && chainRoot->next) {
    auto *previous = chainRoot->next;
    chainRoot->next = previous->next;
    //chainRoot->implementationFunction = previous->implementationFunction;
    swift_ptrauth_copy_code_or_data(
        reinterpret_cast<void **>(&chainRoot->implementationFunction),
        reinterpret_cast<void *const *>(&previous->implementationFunction),
        replacedFunctionKey->getExtraDiscriminator(),
        !replacedFunctionKey->isAsync(), /*allowNull*/ false);
  }

  // First populate the current replacement's chain entry.
  auto *currentEntry =
      const_cast<DynamicReplacementChainEntry *>(chainEntry.get());
  // currentEntry->implementationFunction = chainRoot->implementationFunction;
  swift_ptrauth_copy_code_or_data(
      reinterpret_cast<void **>(&currentEntry->implementationFunction),
      reinterpret_cast<void *const *>(&chainRoot->implementationFunction),
      replacedFunctionKey->getExtraDiscriminator(),
      !replacedFunctionKey->isAsync(), /*allowNull*/ false);

  currentEntry->next = chainRoot->next;

  // Link the replacement entry.
  chainRoot->next = chainEntry.get();
  // chainRoot->implementationFunction = getReplacementFunction();
  swift_ptrauth_init_code_or_data(
      reinterpret_cast<void **>(&chainRoot->implementationFunction),
      reinterpret_cast<void *>(getReplacementFunction()),
      replacedFunctionKey->getExtraDiscriminator(),
      !replacedFunctionKey->isAsync());
}

void DynamicReplacementDescriptor::disableReplacement() const {
  const auto *chainRoot = replacedFunctionKey->root.get();
  auto *thisEntry =
      const_cast<DynamicReplacementChainEntry *>(chainEntry.get());

  // Find the entry previous to this one.
  auto *prev = chainRoot;
  while (prev && prev->next != thisEntry)
    prev = prev->next;
  if (!prev) {
    swift::swift_abortDynamicReplacementDisabling();
    return;
  }

  // Unlink this entry.
  auto *previous = const_cast<DynamicReplacementChainEntry *>(prev);
  previous->next = thisEntry->next;
  // previous->implementationFunction = thisEntry->implementationFunction;
  swift_ptrauth_copy_code_or_data(
      reinterpret_cast<void **>(&previous->implementationFunction),
      reinterpret_cast<void *const *>(&thisEntry->implementationFunction),
      replacedFunctionKey->getExtraDiscriminator(),
      !replacedFunctionKey->isAsync(), /*allowNull*/ false);
}

/// An automatic dynamic replacement entry.
namespace {
class AutomaticDynamicReplacementEntry {
  RelativeDirectPointer<DynamicReplacementScope, false> replacementScope;
  uint32_t flags;

public:
  void enable() const { replacementScope->enable(); }

  uint32_t getFlags() { return flags; }
};

/// A list of automatic dynamic replacement scopes.
class AutomaticDynamicReplacements
    : private swift::ABI::TrailingObjects<AutomaticDynamicReplacements,
                                          AutomaticDynamicReplacementEntry> {
  uint32_t flags;
  uint32_t numScopes;

  using TrailingObjects =
      swift::ABI::TrailingObjects<AutomaticDynamicReplacements,
                                  AutomaticDynamicReplacementEntry>;
  friend TrailingObjects;

  llvm::ArrayRef<AutomaticDynamicReplacementEntry>
  getReplacementEntries() const {
    return {
        this->template getTrailingObjects<AutomaticDynamicReplacementEntry>(),
        numScopes};
  }

public:
  void enableReplacements() const {
    for (auto &replacementEntry : getReplacementEntries())
      replacementEntry.enable();
  }

  uint32_t getNumScopes() const { return numScopes; }
};

/// A map from original to replaced opaque type descriptor of a some type.
class DynamicReplacementSomeDescriptor {
  RelativeIndirectablePointer<
      const OpaqueTypeDescriptor, false, int32_t,
      TargetSignedPointer<InProcess, OpaqueTypeDescriptor *
                                         __ptrauth_swift_type_descriptor>>
      originalOpaqueTypeDesc;
  RelativeDirectPointer<const OpaqueTypeDescriptor, false>
      replacementOpaqueTypeDesc;

public:
  void enable(const Mutex &lock) const {
    opaqueTypeMappings.get().insert(originalOpaqueTypeDesc.get(),
                                    replacementOpaqueTypeDesc.get(), lock);
  }
};

/// A list of dynamic replacements of some types.
class AutomaticDynamicReplacementsSome
    : private swift::ABI::TrailingObjects<AutomaticDynamicReplacementsSome,
                                          DynamicReplacementSomeDescriptor> {
  uint32_t flags;
  uint32_t numEntries;
  using TrailingObjects =
      swift::ABI::TrailingObjects<AutomaticDynamicReplacementsSome,
                                  DynamicReplacementSomeDescriptor>;
  friend TrailingObjects;

  llvm::ArrayRef<DynamicReplacementSomeDescriptor>
  getReplacementEntries() const {
    return {
        this->template getTrailingObjects<DynamicReplacementSomeDescriptor>(),
        numEntries};
  }

public:
  void enableReplacements(const Mutex &lock) const {
    for (auto &replacementEntry : getReplacementEntries())
      replacementEntry.enable(lock);
  }
  uint32_t getNumEntries() const { return numEntries; }
};

} // anonymous namespace

void swift::addImageDynamicReplacementBlockCallback(
    const void *baseAddress,
    const void *replacements, uintptr_t replacementsSize,
    const void *replacementsSome, uintptr_t replacementsSomeSize) {

  auto *automaticReplacements =
      reinterpret_cast<const AutomaticDynamicReplacements *>(replacements);

  const AutomaticDynamicReplacementsSome *someReplacements = nullptr;
  if (replacementsSomeSize) {
    someReplacements =
        reinterpret_cast<const AutomaticDynamicReplacementsSome *>(
            replacementsSome);
  }

  auto sizeOfCurrentEntry = sizeof(AutomaticDynamicReplacements) +
                            (automaticReplacements->getNumScopes() *
                             sizeof(AutomaticDynamicReplacementEntry));
  auto sizeOfCurrentSomeEntry =
      replacementsSomeSize == 0
          ? 0
          : sizeof(AutomaticDynamicReplacementsSome) +
                (someReplacements->getNumEntries() *
                 sizeof(DynamicReplacementSomeDescriptor));

  auto &lock = DynamicReplacementLock.get();
  lock.withLock([&] {
    auto endOfAutomaticReplacements =
        ((const char *)automaticReplacements) + replacementsSize;
    while (((const char *)automaticReplacements) < endOfAutomaticReplacements) {
      automaticReplacements->enableReplacements();
      automaticReplacements =
          reinterpret_cast<const AutomaticDynamicReplacements *>(
              ((const char *)automaticReplacements) + sizeOfCurrentEntry);
      if ((const char*)automaticReplacements <  endOfAutomaticReplacements)
        sizeOfCurrentEntry = sizeof(AutomaticDynamicReplacements) +
                             (automaticReplacements->getNumScopes() *
                              sizeof(AutomaticDynamicReplacementEntry));
    }
    if (!replacementsSomeSize)
      return;
    auto endOfSomeReplacements =
        ((const char *)someReplacements) + replacementsSomeSize;
    while (((const char *)someReplacements) < endOfSomeReplacements) {
      someReplacements->enableReplacements(lock);
      someReplacements =
          reinterpret_cast<const AutomaticDynamicReplacementsSome *>(
              ((const char *)someReplacements) + sizeOfCurrentSomeEntry);
      if  ((const char*) someReplacements < endOfSomeReplacements)
        sizeOfCurrentSomeEntry = sizeof(AutomaticDynamicReplacementsSome) +
                                 (someReplacements->getNumEntries() *
                                  sizeof(DynamicReplacementSomeDescriptor));
    }
  });
}

void swift::swift_enableDynamicReplacementScope(
    const DynamicReplacementScope *scope) {
  scope = swift_auth_data_non_address(
      scope, SpecialPointerAuthDiscriminators::DynamicReplacementScope);
  DynamicReplacementLock.get().withLock([=] { scope->enable(); });
}

void swift::swift_disableDynamicReplacementScope(
    const DynamicReplacementScope *scope) {
  scope = swift_auth_data_non_address(
      scope, SpecialPointerAuthDiscriminators::DynamicReplacementScope);
  DynamicReplacementLock.get().withLock([=] { scope->disable(); });
}
#define OVERRIDE_METADATALOOKUP COMPATIBILITY_OVERRIDE
#include COMPATIBILITY_OVERRIDE_INCLUDE_PATH
