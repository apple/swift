//===--- LibPrespecialized.cpp - Interface for prespecializations----------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/Runtime/LibPrespecialized.h"
#include "Private.h"
#include "swift/Basic/Lazy.h"
#include "swift/Runtime/EnvironmentVariables.h"
#include "swift/Runtime/Metadata.h"

#if SWIFT_STDLIB_HAS_DLADDR && __has_include(<dlfcn.h>)
#include <dlfcn.h>
#define USE_DLOPEN 1
#endif

#if __has_include(<mach-o/dyld_priv.h>)
#include <mach-o/dyld_priv.h>
#endif

using namespace swift;

static const LibPrespecializedData<InProcess> *findLibPrespecialized() {
  if (!runtime::environment::SWIFT_DEBUG_ENABLE_LIB_PRESPECIALIZED())
    return nullptr;

  const void *dataPtr = nullptr;
#if USE_DLOPEN
  auto path = runtime::environment::SWIFT_DEBUG_LIB_PRESPECIALIZED_PATH();
  if (path && path[0]) {
    // Use RTLD_NOLOAD to avoid actually loading the library. We just want to
    // find it if it has already been loaded by other means, such as
    // DYLD_INSERT_LIBRARIES.
    void *handle = dlopen(path, RTLD_LAZY | RTLD_NOLOAD);
    if (!handle) {
      swift::warning(0, "Failed to load prespecializations library: %s\n",
                     dlerror());
      return nullptr;
    }

    dataPtr = dlsym(handle, LIB_PRESPECIALIZED_TOP_LEVEL_SYMBOL_NAME);
  }
#if DYLD_GET_SWIFT_PRESPECIALIZED_DATA_DEFINED
  else if (SWIFT_RUNTIME_WEAK_CHECK(_dyld_get_swift_prespecialized_data)) {
    // Disable the prespecializations library if anything in the shared cache is
    // overridden. Eventually we want to be cleverer and only disable the
    // prespecializations that have been invalidated, but we'll start with the
    // simplest approach.
    if (!dyld_shared_cache_some_image_overridden())
      dataPtr = SWIFT_RUNTIME_WEAK_USE(_dyld_get_swift_prespecialized_data());
  }
#endif
#endif

  if (!dataPtr)
    return nullptr;

  auto *data =
      reinterpret_cast<const LibPrespecializedData<InProcess> *>(dataPtr);
  if (data->majorVersion !=
      LibPrespecializedData<InProcess>::currentMajorVersion)
    return nullptr;

  return data;
}

const LibPrespecializedData<InProcess> *swift::getLibPrespecializedData() {
  return SWIFT_LAZY_CONSTANT(findLibPrespecialized());
}

// Returns true if the type has any arguments that aren't plain types (packs or
// unknown kinds).
static bool hasNonTypeGenericArguments(const TypeContextDescriptor *description) {
  auto generics = description->getGenericContext();
  if (!generics)
    return false;

  for (auto param : generics->getGenericParams())
    if (param.getKind() != GenericParamKind::Type)
      return true;

  return false;
}

static bool disableForValidation = false;

Metadata *
swift::getLibPrespecializedMetadata(const TypeContextDescriptor *description,
                                    const void *const *arguments) {
  if (disableForValidation)
    return nullptr;

  auto *data = getLibPrespecializedData();
  if (!data)
    return nullptr;

  // We don't support types with pack parameters yet (and especially not types
  // with unknown parameter kinds) so don't even try to look those up.
  if (hasNonTypeGenericArguments(description))
    return nullptr;

  Demangler dem;
  auto mangleNode = _buildDemanglingForGenericType(description, arguments, dem);
  if (!mangleNode) {
    if (SWIFT_UNLIKELY(runtime::environment::
                           SWIFT_DEBUG_ENABLE_LIB_PRESPECIALIZED_LOGGING()))
      fprintf(stderr,
              "Prespecializations library: failed to build demangling with "
              "descriptor %p.\n",
              description);
    return nullptr;
  }

  if (mangleNode->getKind() != Node::Kind::Global) {
    auto wrapper = dem.createNode(Node::Kind::Global);
    wrapper->addChild(mangleNode, dem);
    mangleNode = wrapper;
  }
  auto resolver = [](SymbolicReferenceKind kind,
                     const void *ref) -> NodePointer {
    swift::fatalError(0,
                      "Unexpected symbolic reference %p in generated mangle "
                      "tree for generic type lookup.",
                      ref);
  };
  auto mangling = Demangle::mangleNode(mangleNode, resolver, dem);
  if (!mangling.isSuccess()) {
    swift::warning(0,
                   "Mangling for prespecialized metadata failed with code %d",
                   mangling.error().code);
    return nullptr;
  }

  auto key = mangling.result();
  auto *metadataMap = data->getMetadataMap();
  auto *element = metadataMap->find(key.data(), key.size());
  auto *result = element ? element->value : nullptr;
  if (SWIFT_UNLIKELY(runtime::environment::
                         SWIFT_DEBUG_ENABLE_LIB_PRESPECIALIZED_LOGGING()))
    fprintf(stderr, "Prespecializations library: found %p for key '%.*s'.\n",
            result, (int)key.size(), key.data());
  return result;
}

void _swift_validatePrespecializedMetadata() {
  auto *data = getLibPrespecializedData();
  if (!data) {
    return;
  }

  disableForValidation = true;

  unsigned validated = 0;
  unsigned failed = 0;

  auto *metadataMap = data->getMetadataMap();
  auto metadataMapSize = metadataMap->arraySize;
  auto *array = metadataMap->array();
  for (uint64_t i = 0; i < metadataMapSize; i++) {
    auto &element = array[i];
    if (!element.key || !element.value)
      continue;

    validated++;

    const char *mangledName = element.key;
    // Skip the leading $.
    if (mangledName[0] == '$')
      mangledName++;

    auto result = swift_getTypeByMangledName(MetadataState::Complete,
                                             mangledName, nullptr, {}, {});
    if (auto *error = result.getError()) {
      fprintf(stderr,
              "Prespecializations library validation: unable to build metadata "
              "for mangled name '%s'\n",
              mangledName);
      failed++;
      continue;
    }

    if (!compareGenericMetadata(result.getType().getMetadata(), element.value))
      failed++;
  }

  fprintf(stderr,
          "Prespecializations library validation: validated %u entries, %u "
          "failures.\n",
          validated, failed);
}
