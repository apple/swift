//===--- SwiftRemoteMirrorLegacyInterop.h - Interop with legacy libs. -*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------------===//
///
/// \file
/// This header provides an interface for using multiple versions of the Swift remote
/// mirror library to inspect processes built with different versions of Swift, or
/// processes where different libraries are built with different versions of Swift.
///
//===----------------------------------------------------------------------------===//

#ifndef SWIFT_REMOTE_MIRROR_LEGACY_INTEROP_H
#define SWIFT_REMOTE_MIRROR_LEGACY_INTEROP_H

#if defined(__APPLE__) && defined(__MACH__)

#include "SwiftRemoteMirrorLegacyInteropTypes.h"
#include "SwiftRemoteMirror.h"

#include <string.h>
#include <dlfcn.h>
#include <mach-o/getsect.h>

#include <CoreFoundation/CFDictionary.h>
#include <TargetConditionals.h>

/// The "public" interface follows. All of these functions are the same
/// as the corresponding swift_reflection_* functions, except for taking
/// or returning _interop data types in some circumstances.

static inline SwiftReflectionInteropContextRef
swift_reflection_interop_createReflectionContext(
    void *ReaderContext,
    uint8_t PointerSize,
    FreeBytesFunction FreeBytes,
    ReadBytesFunction ReadBytes,
    GetStringLengthFunction GetStringLength,
    GetSymbolAddressFunction GetSymbolAddress);

static inline SwiftReflectionInteropContextRef
swift_reflection_interop_createReflectionContextWithDataLayout(
    void *ReaderContext,
    QueryDataLayoutFunction DataLayout,
    FreeBytesFunction FreeBytes,
    ReadBytesFunction ReadBytes,
    GetStringLengthFunction GetStringLength,
    GetSymbolAddressFunction GetSymbolAddress);

/// Add a library handle to the interop context. Returns 1 if the
/// library was added successfully, 0 if a symbol couldn't be looked up
/// or the reported metadata version is too old.
static inline int
swift_reflection_interop_addLibrary(
  SwiftReflectionInteropContextRef ContextRef, void *LibraryHandle);

static inline void
swift_reflection_interop_destroyReflectionContext(
  SwiftReflectionInteropContextRef ContextRef);

/// Set the is-Swift mask for the stable ABI on the current system.
/// NOTE: must be called after interop_addLibrary is used to add the
/// stable ABI remote mirror library in order to take effect.
static inline void
swift_reflection_interop_setClassIsSwiftMask(
  SwiftReflectionInteropContextRef ContextRef, uint64_t mask);

static inline int
swift_reflection_interop_addImage(SwiftReflectionInteropContextRef ContextRef,
                                  swift_addr_t imageStart);

static inline int
swift_reflection_interop_readIsaMask(SwiftReflectionInteropContextRef ContextRef,
                                     uintptr_t *outIsaMask);

/// Look up a metadata pointer and return an interop structure that can
/// be passed to other calls. Returns { 0, 0 } if the metadata pointer
/// was not recognized.
static inline swift_metadata_interop_t
swift_reflection_interop_lookupMetadata(SwiftReflectionInteropContextRef ContextRef,
                                       uintptr_t RemoteTyperef);

static inline swift_typeref_interop_t
swift_reflection_interop_typeRefForMetadata(SwiftReflectionInteropContextRef ContextRef,
                                            swift_metadata_interop_t Metadata);

static inline swift_typeref_interop_t
swift_reflection_interop_typeRefForInstance(SwiftReflectionInteropContextRef ContextRef,
                                            uintptr_t Object);

static inline swift_typeref_interop_t
swift_reflection_interop_typeRefForMangledTypeName(
  SwiftReflectionInteropContextRef ContextRef,
  const char *MangledName,
  uint64_t Length);

static inline char *
swift_reflection_interop_copyDemangledNameForTypeRef(
  SwiftReflectionInteropContextRef ContextRef,
  swift_typeref_interop_t OpaqueTypeRef);

static inline swift_typeinfo_t
swift_reflection_interop_infoForTypeRef(SwiftReflectionInteropContextRef ContextRef,
                                        swift_typeref_interop_t OpaqueTypeRef);

static inline swift_childinfo_interop_t
swift_reflection_interop_childOfTypeRef(SwiftReflectionInteropContextRef ContextRef,
                                        swift_typeref_interop_t OpaqueTypeRef,
                                        unsigned Index);

static inline swift_typeinfo_interop_t
swift_reflection_interop_infoForMetadata(SwiftReflectionInteropContextRef ContextRef,
                                        swift_metadata_interop_t Metadata);

static inline swift_childinfo_interop_t
swift_reflection_interop_childOfMetadata(SwiftReflectionInteropContextRef ContextRef,
                                        swift_metadata_interop_t Metadata,
                                        unsigned Index);

static inline swift_typeinfo_interop_t
swift_reflection_interop_infoForInstance(SwiftReflectionInteropContextRef ContextRef,
                                         uintptr_t Object);

static inline swift_childinfo_interop_t
swift_reflection_interop_childOfInstance(SwiftReflectionInteropContextRef ContextRef,
                                         uintptr_t Object,
                                         unsigned Index);

static inline unsigned
swift_reflection_interop_genericArgumentCountOfTypeRef(
  SwiftReflectionInteropContextRef ContextRef, swift_typeref_interop_t OpaqueTypeRef);

static inline swift_typeref_interop_t
swift_reflection_interop_genericArgumentOfTypeRef(
  SwiftReflectionInteropContextRef ContextRef, swift_typeref_interop_t OpaqueTypeRef,
  unsigned Index);

static inline int
swift_reflection_interop_projectExistential(SwiftReflectionInteropContextRef ContextRef,
                                            swift_addr_t ExistentialAddress,
                                            swift_typeref_interop_t ExistentialTypeRef,
                                            swift_typeref_interop_t *OutInstanceTypeRef,
                                            swift_addr_t *OutStartOfInstanceData);

static inline int swift_reflection_interop_projectEnum(
    SwiftReflectionInteropContextRef ContextRef, swift_addr_t EnumAddress,
    swift_typeref_interop_t EnumTypeRef, int *CaseIndex);

static inline void
swift_reflection_interop_dumpTypeRef(SwiftReflectionInteropContextRef ContextRef,
                                     swift_typeref_interop_t OpaqueTypeRef);

static inline void
swift_reflection_interop_dumpInfoForTypeRef(SwiftReflectionInteropContextRef ContextRef,
                                            swift_typeref_interop_t OpaqueTypeRef);

static inline void
swift_reflection_interop_dumpInfoForMetadata(SwiftReflectionInteropContextRef ContextRef,
                                             swift_metadata_interop_t Metadata);

static inline void
swift_reflection_interop_dumpInfoForInstance(SwiftReflectionInteropContextRef ContextRef,
                                             uintptr_t Object);

static inline size_t
swift_reflection_interop_demangle(SwiftReflectionInteropContextRef ContextRef,
                                  const char *MangledName,
                                  size_t Length,
                                  char *OutDemangledName,
                                  size_t MaxLength);



/// \name Internal implementation details, clients don't need to use these.
/// @{

/// The minimum supported metadata version for the legacy library.
#define SWIFT_LEGACY_METADATA_MIN_VERSION 3

/// The legacy reflection info struct.
typedef struct swift_reflection_legacy_info {
  swift_reflection_section_t fieldmd;
  swift_reflection_section_t assocty;
  swift_reflection_section_t builtin;
  swift_reflection_section_t capture;
  swift_reflection_section_t typeref;
  swift_reflection_section_t reflstr;
  uintptr_t LocalStartAddress;
  uintptr_t RemoteStartAddress;
} swift_reflection_legacy_info_t;

/// The signature of the legacy ReadBytesFunction.
typedef int (*ReadBytesFunctionLegacy)(void *reader_context, swift_addr_t address,
                                       void *dest, uint64_t size);

struct SwiftReflectionFunctions {
  unsigned long long *classIsSwiftMaskPtr;

  uint16_t (*getSupportedMetadataVersion)(void);

  SwiftReflectionContextRef (*createReflectionContext)(
    void *ReaderContext,
    uint8_t PointerSize,
    FreeBytesFunction FreeBytes,
    ReadBytesFunction ReadBytes,
    GetStringLengthFunction GetStringLength,
    GetSymbolAddressFunction GetSymbolAddress);

  // Optional creation function that takes a data layout query function.
  SwiftReflectionContextRef (*createReflectionContextWithDataLayout)(
    void *ReaderContext,
    QueryDataLayoutFunction DataLayout,
    FreeBytesFunction FreeBytes,
    ReadBytesFunction ReadBytes,
    GetStringLengthFunction GetStringLength,
    GetSymbolAddressFunction GetSymbolAddress);
  
  SwiftReflectionContextRef (*createReflectionContextLegacy)(
    void *ReaderContext,
    PointerSizeFunction getPointerSize,
    SizeSizeFunction getSizeSize,
    ReadBytesFunctionLegacy readBytes,
    GetStringLengthFunction getStringLength,
    GetSymbolAddressFunction getSymbolAddress);
  
  void (*destroyReflectionContext)(SwiftReflectionContextRef Context);
  
  void (*addReflectionInfo)(SwiftReflectionContextRef Context,
                            swift_reflection_info_t Info);

  void (*addReflectionInfoLegacy)(SwiftReflectionContextRef Context,
                                  swift_reflection_legacy_info_t Info);

  int (*addImage)(SwiftReflectionContextRef ContextRef,
                  swift_addr_t imageStart);

  int (*readIsaMask)(SwiftReflectionContextRef ContextRef, uintptr_t *outIsaMask);
  
  swift_typeref_t (*typeRefForMetadata)(SwiftReflectionContextRef ContextRef,
                                        uintptr_t Metadata);
  
  int (*ownsObject)(SwiftReflectionContextRef ContextRef, uintptr_t Object);
  
  int (*ownsAddress)(SwiftReflectionContextRef ContextRef, uintptr_t Address);
  
  uintptr_t (*metadataForObject)(SwiftReflectionContextRef ContextRef, uintptr_t Object);
  
  swift_typeref_t (*typeRefForInstance)(SwiftReflectionContextRef ContextRef,
                                        uintptr_t Object);

  swift_typeref_t (*typeRefForMangledTypeName)(SwiftReflectionContextRef ContextRef,
                                           const char *MangledName,
                                           uint64_t Length);

  char * (*copyDemangledNameForTypeRef)(
  SwiftReflectionContextRef ContextRef, swift_typeref_t OpaqueTypeRef);

  swift_typeinfo_t (*infoForTypeRef)(SwiftReflectionContextRef ContextRef,
                                      swift_typeref_t OpaqueTypeRef);

  swift_childinfo_t (*childOfTypeRef)(SwiftReflectionContextRef ContextRef,
                                      swift_typeref_t OpaqueTypeRef,
                                      unsigned Index);

  swift_typeinfo_t (*infoForMetadata)(SwiftReflectionContextRef ContextRef,
                                      uintptr_t Metadata);

  swift_childinfo_t (*childOfMetadata)(SwiftReflectionContextRef ContextRef,
                                       uintptr_t Metadata,
                                       unsigned Index);
  swift_typeinfo_t (*infoForInstance)(SwiftReflectionContextRef ContextRef,
                                      uintptr_t Object);

  swift_childinfo_t (*childOfInstance)(SwiftReflectionContextRef ContextRef,
                                       uintptr_t Object,
                                       unsigned Index);

  unsigned (*genericArgumentCountOfTypeRef)(swift_typeref_t OpaqueTypeRef);

  swift_typeref_t (*genericArgumentOfTypeRef)(swift_typeref_t OpaqueTypeRef,
                                              unsigned Index);

  int (*projectExistential)(SwiftReflectionContextRef ContextRef,
                            swift_addr_t ExistentialAddress,
                            swift_typeref_t ExistentialTypeRef,
                            swift_typeref_t *OutInstanceTypeRef,
                            swift_addr_t *OutStartOfInstanceData);

  int (*projectEnumValue)(SwiftReflectionContextRef ContextRef,
                          swift_addr_t EnumAddress, swift_typeref_t EnumTypeRef,
                          int *CaseIndex);

  void (*dumpTypeRef)(swift_typeref_t OpaqueTypeRef);

  void (*dumpInfoForTypeRef)(SwiftReflectionContextRef ContextRef,
                             swift_typeref_t OpaqueTypeRef);

  void (*dumpInfoForMetadata)(SwiftReflectionContextRef ContextRef,
                              uintptr_t Metadata);

  void (*dumpInfoForInstance)(SwiftReflectionContextRef ContextRef,
                              uintptr_t Object);

  size_t (*demangle)(const char *MangledName,
                     size_t Length,
                     char *OutDemangledName,
                     size_t MaxLength);
};

struct SwiftReflectionInteropContextLibrary {
  SwiftReflectionContextRef Context;
  struct SwiftReflectionFunctions Functions;
  int IsLegacy;
};

struct SwiftReflectionInteropContextFreeList {
  struct SwiftReflectionInteropContextFreeList *Next;
  const void *Pointer;
  void *Context;
};

struct SwiftReflectionInteropContextLegacyImageRangeList {
  struct SwiftReflectionInteropContextLegacyImageRangeList *Next;
  swift_addr_t Start, End;
};

struct SwiftReflectionInteropContext {
  void *ReaderContext;
  QueryDataLayoutFunction DataLayout;
  FreeBytesFunction FreeBytes;
  ReadBytesFunction ReadBytes;
  uint64_t (*GetStringLength)(void *reader_context,
                              swift_addr_t address);
  swift_addr_t (*GetSymbolAddress)(void *reader_context,
                                   const char *name,
                                   uint64_t name_length);
  
  struct SwiftReflectionInteropContextLibrary *Libraries;
  int LibraryCount;
  
  struct SwiftReflectionInteropContextFreeList *FreeList;
  struct SwiftReflectionInteropContextLegacyImageRangeList *LegacyImageRangeList;
  
  CFMutableDictionaryRef AddressToLibraryCache;
};

#define FOREACH_LIBRARY \
  for (struct SwiftReflectionInteropContextLibrary *Library = &ContextRef->Libraries[0]; \
       Library < &ContextRef->Libraries[ContextRef->LibraryCount]; \
       ++Library)
#define LIBRARY_INDEX (Library - ContextRef->Libraries)
#define DECLARE_LIBRARY(index) \
  struct SwiftReflectionInteropContextLibrary *Library = &ContextRef->Libraries[index]

static inline int
swift_reflection_interop_libraryOwnsAddress(
  struct SwiftReflectionInteropContext *ContextRef,
  struct SwiftReflectionInteropContextLibrary *Library,
  uintptr_t Address) {
  if (!Library->IsLegacy)
    return Library->Functions.ownsAddress(Library->Context, Address);

  // Search the images list to see if the address is in one of them.
  struct SwiftReflectionInteropContextLegacyImageRangeList *Node =
    ContextRef->LegacyImageRangeList;
  while (Node != NULL) {
    if (Node->Start <= Address && Address < Node->End)
      return 1;
    Node = Node->Next;
  }
  return 0;
}

static inline struct SwiftReflectionInteropContextLibrary *
swift_reflection_interop_libraryForAddress(
  struct SwiftReflectionInteropContext *ContextRef,
  uintptr_t Address) {
  uintptr_t cachedIndex;
  if (CFDictionaryGetValueIfPresent(ContextRef->AddressToLibraryCache,
                                    (void *)Address,
                                    (const void **)&cachedIndex)) {
    return &ContextRef->Libraries[cachedIndex];
  }
  
  FOREACH_LIBRARY {
    if (swift_reflection_interop_libraryOwnsAddress(ContextRef, Library, Address)) {
      CFDictionarySetValue(ContextRef->AddressToLibraryCache,
                           (void *)Address,
                           (void *)LIBRARY_INDEX);
      return Library;
    }
  }
  return NULL;
}

static inline uintptr_t
swift_reflection_interop_metadataForObject(
  struct SwiftReflectionInteropContext *ContextRef,
  uintptr_t Object) {
  FOREACH_LIBRARY {
    if (Library->IsLegacy)
      continue;
    uintptr_t Metadata = Library->Functions.metadataForObject(Library->Context, Object);
    if (Metadata != 0)
      return Metadata;
  }
  return 0;
}

static inline struct SwiftReflectionInteropContextLibrary *
swift_reflection_interop_libraryForObject(
  struct SwiftReflectionInteropContext *ContextRef,
  uintptr_t Object) {
  uintptr_t Metadata = swift_reflection_interop_metadataForObject(ContextRef, Object);
  if (Metadata == 0) {
    // If we couldn't retrieve metadata, assume it belongs to a legacy library.
    FOREACH_LIBRARY {
      if (Library->IsLegacy)
        return Library;
    }
    return NULL;
  }
  
  return swift_reflection_interop_libraryForAddress(ContextRef, Metadata);
}

static inline int
swift_reflection_interop_loadFunctions(struct SwiftReflectionInteropContext *Context,
                                       void *Handle) {
  if (Handle == NULL)
    return 0;

  struct SwiftReflectionInteropContextLibrary *Library = &Context
    ->Libraries[Context->LibraryCount];
  struct SwiftReflectionFunctions *Functions = &Library->Functions;

#ifndef __cplusplus
#define decltype(x) void *
#endif
#define LOAD_NAMED(field, symbol, required) do { \
    Functions->field = (decltype(Functions->field))dlsym(Handle, symbol); \
    if (required && Functions->field == NULL) return 0; \
  } while (0)
#define LOAD(name) LOAD_NAMED(name, "swift_reflection_" #name, 1)
#define LOAD_OPT(name) LOAD_NAMED(name, "swift_reflection_" #name, 0)
  
  Functions->classIsSwiftMaskPtr =
    (unsigned long long *)dlsym(Handle, "swift_reflection_classIsSwiftMask");
  LOAD(getSupportedMetadataVersion);
  uint16_t version = Functions->getSupportedMetadataVersion();
  if (version < SWIFT_LEGACY_METADATA_MIN_VERSION)
    return 0;
  
  int IsLegacy = dlsym(Handle, "swift_reflection_addImage") == NULL;
  
  if (IsLegacy) {
    LOAD_NAMED(createReflectionContextLegacy, "swift_reflection_createReflectionContext", 1);
    LOAD_NAMED(addReflectionInfoLegacy, "swift_reflection_addReflectionInfo", 1);
  } else {
    LOAD(createReflectionContext);
    LOAD(addReflectionInfo);
    LOAD(addImage);
    LOAD(ownsObject);
    LOAD(ownsAddress);
    LOAD(metadataForObject);
    
    // Optional creation function.
    LOAD_OPT(createReflectionContextWithDataLayout);
  }
  
  LOAD(destroyReflectionContext);
  LOAD(readIsaMask);
  LOAD(typeRefForMetadata);
  LOAD(typeRefForInstance);
  LOAD(typeRefForMangledTypeName);
  LOAD_OPT(copyDemangledNameForTypeRef);
  LOAD(infoForTypeRef);
  LOAD(childOfTypeRef);
  LOAD(infoForMetadata);
  LOAD(childOfMetadata);
  LOAD(infoForInstance);
  LOAD(childOfInstance);
  LOAD(genericArgumentCountOfTypeRef);
  LOAD(genericArgumentOfTypeRef);
  LOAD(projectExistential);
  LOAD_OPT(projectEnumValue);
  LOAD(dumpTypeRef);
  LOAD(dumpInfoForTypeRef);
  
  Library->IsLegacy = IsLegacy;
  ++Context->LibraryCount;
  
  return 1;
  
#undef LOAD
#undef LOAD_NAMED
#ifndef __cplusplus
#undef decltype
#endif
}

static inline int
swift_reflection_interop_readBytesAdapter(void *reader_context,
                                          swift_addr_t address,
                                          void *dest,
                                          uint64_t size) {
  SwiftReflectionInteropContextRef Context =
    (SwiftReflectionInteropContextRef)reader_context;

  void *FreeContext;
  const void *ptr = Context->ReadBytes(Context->ReaderContext, address, size,
                                       &FreeContext);
  if (ptr == NULL)
    return 0;
  
  memcpy(dest, ptr, size);
  if (Context->FreeBytes != NULL)
    Context->FreeBytes(Context->ReaderContext, ptr, FreeContext);
  return 1;
}

static inline uint8_t
swift_reflection_interop_getSizeAdapter(void *reader_context) {
  // Legacy library doesn't pay attention to these anyway.
  (void)reader_context;
  return sizeof(void *);
}

static inline uint64_t
swift_reflection_interop_GetStringLengthAdapter(
  void *reader_context, swift_addr_t address) {
  SwiftReflectionInteropContextRef Context =
    (SwiftReflectionInteropContextRef)reader_context;
  return Context->GetStringLength(Context->ReaderContext, address);
}

static inline swift_addr_t
swift_reflection_interop_GetSymbolAddressAdapter(
  void *reader_context, const char *name, uint64_t name_length) {
  SwiftReflectionInteropContextRef Context =
    (SwiftReflectionInteropContextRef)reader_context;
  return Context->GetSymbolAddress(Context->ReaderContext, name, name_length);
}

static inline int
swift_reflection_interop_minimalDataLayoutQueryFunction4(
  void *ReaderContext,
  DataLayoutQueryType type,
  void *inBuffer, void *outBuffer) {
  (void)ReaderContext;
  (void)inBuffer;
  switch (type) {
  case DLQ_GetPointerSize:
  case DLQ_GetSizeSize: {
    uint8_t *result = (uint8_t *)outBuffer;
    *result = 4;
    return 1;
  }
  case DLQ_GetObjCReservedLowBits: {
    uint8_t *result = (uint8_t *)outBuffer;
    // Swift assumes this for all 32-bit platforms, including Darwin
    *result = 0;
    return 1;
  }
  case DLQ_GetLeastValidPointerValue: {
    uint64_t *result = (uint64_t *)outBuffer;
    // Swift assumes this for all 32-bit platforms, including Darwin
    *result = 0x1000;
    return 1;
  }
  default:
    return 0;
  }
}

static inline int
swift_reflection_interop_minimalDataLayoutQueryFunction8(
  void *ReaderContext,
  DataLayoutQueryType type,
  void *inBuffer, void *outBuffer) {
  (void)ReaderContext;
  (void)inBuffer;
  // Caveat: This assumes the process being examined is
  // running in the same kind of environment as this host code.
#if defined(__APPLE__) && __APPLE__
    int applePlatform = 1;
#else
    int applePlatform = 0;
#endif
#if defined(__APPLE__) && __APPLE__ && ((defined(TARGET_OS_IOS) && TARGET_OS_IOS) || (defined(TARGET_OS_IOS) && TARGET_OS_WATCH) || (defined(TARGET_OS_TV) && TARGET_OS_TV) || defined(__arm64__))
    int iosDerivedPlatform = 1;
#else
    int iosDerivedPlatform = 0;
#endif

  switch (type) {
  case DLQ_GetPointerSize:
  case DLQ_GetSizeSize: {
    uint8_t *result = (uint8_t *)outBuffer;
    *result = 8;
    return 1;
  }
  case DLQ_GetObjCReservedLowBits: {
    uint8_t *result = (uint8_t *)outBuffer;
    if (applePlatform && !iosDerivedPlatform) {
      *result = 1;
    } else {
      *result = 0;
    }
    return 1;
  }
  case DLQ_GetLeastValidPointerValue: {
    uint64_t *result = (uint64_t *)outBuffer;
    if (applePlatform) {
      // On 64-bit Apple platforms, Swift reserves the first 4GiB
      *result = 0x100000000;
    } else {
      // Swift reserves the first 4KiB everywhere else.
      *result = 0x1000;
    }
    return 1;
  }
  default:
    return 0;
  }
}

static inline SwiftReflectionInteropContextRef
swift_reflection_interop_createReflectionContext(
    void *ReaderContext,
    uint8_t PointerSize,
    FreeBytesFunction FreeBytes,
    ReadBytesFunction ReadBytes,
    GetStringLengthFunction GetStringLength,
    GetSymbolAddressFunction GetSymbolAddress) {
  QueryDataLayoutFunction DataLayout;
  if (PointerSize == 4)
    DataLayout = swift_reflection_interop_minimalDataLayoutQueryFunction4;
  else if (PointerSize == 8)
    DataLayout = swift_reflection_interop_minimalDataLayoutQueryFunction8;
  else
    abort(); // Can't handle sizes other than 4 and 8.

  return swift_reflection_interop_createReflectionContextWithDataLayout(
    ReaderContext,
    DataLayout,
    FreeBytes,
    ReadBytes,
    GetStringLength,
    GetSymbolAddress);
}

static inline SwiftReflectionInteropContextRef
swift_reflection_interop_createReflectionContextWithDataLayout(
    void *ReaderContext,
    QueryDataLayoutFunction DataLayout,
    FreeBytesFunction FreeBytes,
    ReadBytesFunction ReadBytes,
    GetStringLengthFunction GetStringLength,
    GetSymbolAddressFunction GetSymbolAddress) { 
 
  SwiftReflectionInteropContextRef ContextRef =
    (SwiftReflectionInteropContextRef)calloc(sizeof(*ContextRef), 1);
  
  ContextRef->ReaderContext = ReaderContext;
  ContextRef->DataLayout = DataLayout;
  ContextRef->FreeBytes = FreeBytes;
  ContextRef->ReadBytes = ReadBytes;
  ContextRef->GetStringLength = GetStringLength;
  ContextRef->GetSymbolAddress = GetSymbolAddress;
  
  ContextRef->AddressToLibraryCache = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
  
  return ContextRef;
}

static inline int
swift_reflection_interop_addLibrary(
  SwiftReflectionInteropContextRef ContextRef, void *LibraryHandle) {
  size_t NewSize = (ContextRef->LibraryCount + 1) * sizeof(*ContextRef->Libraries);
  ContextRef->Libraries = realloc(ContextRef->Libraries, NewSize);
  int Success = swift_reflection_interop_loadFunctions(ContextRef, LibraryHandle);
  if (Success) {
    struct SwiftReflectionInteropContextLibrary *Library =
      &ContextRef->Libraries[ContextRef->LibraryCount - 1];
    if (Library->IsLegacy) {
      Library->Context = Library->Functions.createReflectionContextLegacy(
        ContextRef,
        swift_reflection_interop_getSizeAdapter, swift_reflection_interop_getSizeAdapter,
        swift_reflection_interop_readBytesAdapter,
        swift_reflection_interop_GetStringLengthAdapter,
        swift_reflection_interop_GetSymbolAddressAdapter);
    } else if (Library->Functions.createReflectionContextWithDataLayout) {
      Library->Context =
        Library->Functions.createReflectionContextWithDataLayout(
        ContextRef->ReaderContext,
        ContextRef->DataLayout,
        ContextRef->FreeBytes,
        ContextRef->ReadBytes,
        ContextRef->GetStringLength,
        ContextRef->GetSymbolAddress);          
    } else {
      uint8_t PointerSize;
      int result = ContextRef->DataLayout(
        ContextRef->ReaderContext, DLQ_GetPointerSize, NULL, &PointerSize);
      if (!result)
        abort(); // We need the pointer size, can't proceed without it.

      Library->Context = Library->Functions.createReflectionContext(
        ContextRef->ReaderContext,
        PointerSize,
        ContextRef->FreeBytes,
        ContextRef->ReadBytes,
        ContextRef->GetStringLength,
        ContextRef->GetSymbolAddress);
    }
  }
  return Success;
}

static inline void
swift_reflection_interop_destroyReflectionContext(
  SwiftReflectionInteropContextRef ContextRef) {
  FOREACH_LIBRARY {
    Library->Functions.destroyReflectionContext(Library->Context);
  }
  free(ContextRef->Libraries);
  struct SwiftReflectionInteropContextLegacyImageRangeList *LegacyImageRangeList
    = ContextRef->LegacyImageRangeList;
  while (LegacyImageRangeList != NULL) {
    struct SwiftReflectionInteropContextLegacyImageRangeList *Next
      = LegacyImageRangeList->Next;
    free(LegacyImageRangeList);
    LegacyImageRangeList = Next;
  }
  struct SwiftReflectionInteropContextFreeList *FreeList = ContextRef->FreeList;
  while (FreeList != NULL) {
    ContextRef->FreeBytes(ContextRef->ReaderContext,
                          FreeList->Pointer, FreeList->Context);
    struct SwiftReflectionInteropContextFreeList *Next = FreeList->Next;
    free(FreeList);
    FreeList = Next;
  }
  
  CFRelease(ContextRef->AddressToLibraryCache);
  
  free(ContextRef);
}

static inline void
swift_reflection_interop_setClassIsSwiftMask(
  SwiftReflectionInteropContextRef ContextRef, uint64_t mask) {
  FOREACH_LIBRARY {
    if (Library->Functions.classIsSwiftMaskPtr)
      *Library->Functions.classIsSwiftMaskPtr = mask;
  }
}

#ifndef __LP64__
typedef const struct mach_header MachHeader;
#else
typedef const struct mach_header_64 MachHeader;
#endif

static inline int
swift_reflection_interop_findSection(MachHeader *Header, const char *Name,
                                     swift_reflection_section_t *Sect) {
  unsigned long Size;
  void *Address = getsectiondata(Header, "__TEXT", Name, &Size);
  if (!Address)
    return 0;
  
  Sect->Begin = Address;
  Sect->End = (void *)((uintptr_t)Address + Size);
  return 1;
}

static inline int
swift_reflection_interop_addImageLegacy(
  SwiftReflectionInteropContextRef ContextRef,
  struct SwiftReflectionInteropContextLibrary *Library,
  swift_addr_t ImageStart) {
  void *FreeContext;
  const void *Buf;
  Buf = ContextRef->ReadBytes(ContextRef->ReaderContext,
                              ImageStart,
                              sizeof(MachHeader),
                              &FreeContext);
  if (Buf == NULL)
    return 0;
  
  MachHeader *Header = (MachHeader *)Buf;
  
  if (Header->magic != MH_MAGIC && Header->magic != MH_MAGIC_64) {
    if (ContextRef->FreeBytes != NULL)
      ContextRef->FreeBytes(ContextRef->ReaderContext, Buf, FreeContext);
    return 0;
  }
  
  // Read the commands.
  uint32_t Length = Header->sizeofcmds;
  if (ContextRef->FreeBytes != NULL)
    ContextRef->FreeBytes(ContextRef->ReaderContext, Buf, FreeContext);
  
  Buf = ContextRef->ReadBytes(ContextRef->ReaderContext,
                              ImageStart,
                              Length,
                              &FreeContext);
  if (Buf == NULL)
    return 0;
  Header = (MachHeader *)Buf;
  
  // Find the TEXT segment and figure out where the end is.
  unsigned long TextSize;
  uint8_t *TextSegment = getsegmentdata(Header, "__TEXT", &TextSize);
  if (ContextRef->FreeBytes != NULL)
    ContextRef->FreeBytes(ContextRef->ReaderContext, Buf, FreeContext);
  
  if (TextSegment == NULL) {
    return 0;
  }
  unsigned long TextEnd = TextSegment - (uint8_t *)Buf + TextSize;

  // Read everything including the TEXT segment.
  Buf = ContextRef->ReadBytes(ContextRef->ReaderContext,
                              ImageStart,
                              TextEnd,
                              &FreeContext);
  if (Buf == NULL)
    return 0;
  Header = (MachHeader *)Buf;

  // Find all the sections and fill out the reflection info.
  swift_reflection_legacy_info_t info = {};
  
  int success = 0;
  success = swift_reflection_interop_findSection(Header,
                                                 "__swift3_fieldmd",
                                                 &info.fieldmd) || success;
  success = swift_reflection_interop_findSection(Header,
                                                 "__swift3_assocty",
                                                 &info.assocty) || success;
  success = swift_reflection_interop_findSection(Header,
                                                 "__swift3_builtin",
                                                 &info.builtin) || success;
  success = swift_reflection_interop_findSection(Header,
                                                 "__swift3_capture",
                                                 &info.capture) || success;
  success = swift_reflection_interop_findSection(Header,
                                                 "__swift3_typeref",
                                                 &info.typeref) || success;
  success = swift_reflection_interop_findSection(Header,
                                                 "__swift3_reflstr",
                                                 &info.reflstr) || success;
  
  if (!success) {
    if (ContextRef->FreeBytes != NULL)
      ContextRef->FreeBytes(ContextRef->ReaderContext, Buf, FreeContext);
    return 0;
  }
    
  info.LocalStartAddress = (uintptr_t)Buf;
  info.RemoteStartAddress = (uintptr_t)ImageStart;
  
  Library->Functions.addReflectionInfoLegacy(Library->Context, info);
  
  // Find the data segment and add it to our list.
  unsigned long DataSize;
  const uint8_t *DataSegment = getsegmentdata(Header, "__DATA", &DataSize);
  uintptr_t DataSegmentStart = (uintptr_t)(DataSegment - (const uint8_t *)Buf + ImageStart);
  
  struct SwiftReflectionInteropContextLegacyImageRangeList *Node =
    (struct SwiftReflectionInteropContextLegacyImageRangeList *)malloc(sizeof(*Node));
  Node->Next = ContextRef->LegacyImageRangeList;
  Node->Start = ImageStart;
  Node->End = DataSegmentStart + DataSize;
  ContextRef->LegacyImageRangeList = Node;
  
  // If the buffer needs to be freed, save buffer and free context to free it when the
  //  reflection context is destroyed.
  if (ContextRef->FreeBytes != NULL) {
    struct SwiftReflectionInteropContextFreeList *FreeListNode =
      (struct SwiftReflectionInteropContextFreeList *)malloc(sizeof(*FreeListNode));
    FreeListNode->Next = ContextRef->FreeList;
    FreeListNode->Pointer = Buf;
    FreeListNode->Context = FreeContext;
    ContextRef->FreeList = FreeListNode;
  }
  
  return 1;
}

static inline int
swift_reflection_interop_addImage(SwiftReflectionInteropContextRef ContextRef,
                                  swift_addr_t imageStart) {
  FOREACH_LIBRARY {
    int Success;
    if (Library->IsLegacy) {
      Success = swift_reflection_interop_addImageLegacy(ContextRef,
                                                        Library,
                                                        imageStart);
    } else {
      Success = Library->Functions.addImage(Library->Context, imageStart);
    }
    if (Success) {
      return 1;
    }
  }
  return 0;
}

static inline int
swift_reflection_interop_readIsaMask(SwiftReflectionInteropContextRef ContextRef,
                                     uintptr_t *outIsaMask) {
  FOREACH_LIBRARY {
    int Success = Library->Functions.readIsaMask(Library->Context, outIsaMask);
    if (Success)
      return 1;
  }
  return 0;
}

static inline swift_metadata_interop_t
swift_reflection_interop_lookupMetadata(SwiftReflectionInteropContextRef ContextRef,
                                       uintptr_t Metadata) {
  swift_metadata_interop_t Result = {};
  struct SwiftReflectionInteropContextLibrary *Library =
    swift_reflection_interop_libraryForAddress(ContextRef, Metadata);
  if (Library != NULL) {
    Result.Metadata = Metadata;
    Result.Library = (int)LIBRARY_INDEX;
  }
  return Result;
}

static inline swift_typeref_interop_t
swift_reflection_interop_typeRefForMetadata(SwiftReflectionInteropContextRef ContextRef,
                                            swift_metadata_interop_t Metadata) {
  DECLARE_LIBRARY(Metadata.Library);
  swift_typeref_interop_t Result;
  Result.Typeref = Library->Functions.
    typeRefForMetadata(Library->Context, Metadata.Metadata);
  Result.Library = Metadata.Library;
  return Result;
}

static inline swift_typeref_interop_t
swift_reflection_interop_typeRefForInstance(SwiftReflectionInteropContextRef ContextRef,
                                            uintptr_t Object) {
  swift_typeref_interop_t Result = {};
  struct SwiftReflectionInteropContextLibrary *Library
    = swift_reflection_interop_libraryForObject(ContextRef, Object);
  if (Library != NULL) {
    swift_typeref_t Typeref = Library->Functions.typeRefForInstance(Library->Context,
                                                                    Object);
    Result.Typeref = Typeref;
    Result.Library = (int)LIBRARY_INDEX;
  }
  return Result;
}

static inline swift_typeref_interop_t
swift_reflection_interop_typeRefForMangledTypeName(
  SwiftReflectionInteropContextRef ContextRef,
  const char *MangledName,
  uint64_t Length) {
  swift_typeref_interop_t Result;
  FOREACH_LIBRARY {
    swift_typeref_t Typeref = Library->Functions.typeRefForMangledTypeName(
      Library->Context, MangledName, Length);
    if (Typeref == 0)
      continue;
    
    Result.Typeref = Typeref;
    Result.Library = (int)LIBRARY_INDEX;
    return Result;
  }
  
  Result.Typeref = 0;
  Result.Library = 0;
  return Result;
}

static inline char *
swift_reflection_interop_copyDemangledNameForTypeRef(
  SwiftReflectionInteropContextRef ContextRef,
  swift_typeref_interop_t OpaqueTypeRef) {
  DECLARE_LIBRARY(OpaqueTypeRef.Library);
  if (Library->Functions.copyDemangledNameForTypeRef)
    return Library->Functions.copyDemangledNameForTypeRef(Library->Context,
                                                          OpaqueTypeRef.Typeref);
  return NULL;
}

static inline swift_typeinfo_t
swift_reflection_interop_infoForTypeRef(SwiftReflectionInteropContextRef ContextRef,
                                        swift_typeref_interop_t OpaqueTypeRef) {
  DECLARE_LIBRARY(OpaqueTypeRef.Library);
  return Library->Functions.infoForTypeRef(Library->Context, OpaqueTypeRef.Typeref);
}

static inline swift_childinfo_interop_t
swift_reflection_interop_childOfTypeRef(SwiftReflectionInteropContextRef ContextRef,
                                        swift_typeref_interop_t OpaqueTypeRef,
                                        unsigned Index) {
  DECLARE_LIBRARY(OpaqueTypeRef.Library);
  swift_childinfo_t LibResult = Library->Functions.childOfTypeRef(Library->Context,
                                                                  OpaqueTypeRef.Typeref,
                                                                  Index);
  swift_childinfo_interop_t Result;
  Result.Name = LibResult.Name;
  Result.Offset = LibResult.Offset;
  Result.Kind = LibResult.Kind;
  Result.TR.Typeref = LibResult.TR;
  Result.TR.Library = OpaqueTypeRef.Library;
  return Result;
}

static inline swift_typeinfo_interop_t
swift_reflection_interop_infoForMetadata(SwiftReflectionInteropContextRef ContextRef,
                                        swift_metadata_interop_t Metadata) {
  DECLARE_LIBRARY(Metadata.Library);
  return Library->Functions.infoForMetadata(Library->Context, Metadata.Metadata);
}

static inline swift_childinfo_interop_t
swift_reflection_interop_childOfMetadata(SwiftReflectionInteropContextRef ContextRef,
                                        swift_metadata_interop_t Metadata,
                                        unsigned Index) {
  DECLARE_LIBRARY(Metadata.Library);
  swift_childinfo_t LibResult = Library->Functions.childOfMetadata(Library->Context,
                                                                   Metadata.Metadata,
                                                                   Index);
  swift_childinfo_interop_t Result;
  Result.Name = LibResult.Name;
  Result.Offset = LibResult.Offset;
  Result.Kind = LibResult.Kind;
  Result.TR.Typeref = LibResult.TR;
  Result.TR.Library = Metadata.Library;
  return Result;
}

static inline swift_typeinfo_interop_t
swift_reflection_interop_infoForInstance(SwiftReflectionInteropContextRef ContextRef,
                                         uintptr_t Object) {
  swift_typeinfo_t Result = {};
  struct SwiftReflectionInteropContextLibrary *Library
    = swift_reflection_interop_libraryForObject(ContextRef, Object);
  
  if (Library != NULL) {
    Result = Library->Functions.infoForInstance(Library->Context, Object);
  } else {
    Result.Kind = SWIFT_UNKNOWN;
  }
  
  return Result;
}

static inline swift_childinfo_interop_t
swift_reflection_interop_childOfInstance(SwiftReflectionInteropContextRef ContextRef,
                                         uintptr_t Object,
                                         unsigned Index) {
  swift_childinfo_interop_t Result = {};
  struct SwiftReflectionInteropContextLibrary *Library
    = swift_reflection_interop_libraryForObject(ContextRef, Object);
  if (Library != NULL) {
    swift_childinfo_t LibResult = Library->Functions.childOfInstance(Library->Context,
                                                                     Object, Index);
    Result.Name = LibResult.Name;
    Result.Offset = LibResult.Offset;
    Result.Kind = LibResult.Kind;
    Result.TR.Typeref = LibResult.TR;
    Result.TR.Library = (int)LIBRARY_INDEX;
  } else {
    Result.Kind = SWIFT_UNKNOWN;
  }
  return Result;
}

static inline unsigned
swift_reflection_interop_genericArgumentCountOfTypeRef(
  SwiftReflectionInteropContextRef ContextRef, swift_typeref_interop_t OpaqueTypeRef) {
  DECLARE_LIBRARY(OpaqueTypeRef.Library);
  return Library->Functions.genericArgumentCountOfTypeRef(OpaqueTypeRef.Typeref);
}

static inline swift_typeref_interop_t
swift_reflection_interop_genericArgumentOfTypeRef(
  SwiftReflectionInteropContextRef ContextRef, swift_typeref_interop_t OpaqueTypeRef,
  unsigned Index) {
  DECLARE_LIBRARY(OpaqueTypeRef.Library);
  swift_typeref_interop_t Result;
  Result.Typeref = Library->Functions.genericArgumentOfTypeRef(OpaqueTypeRef.Typeref,
                                                               Index);
  Result.Library = OpaqueTypeRef.Library;
  return Result;
}

static inline int
swift_reflection_interop_projectExistential(SwiftReflectionInteropContextRef ContextRef,
                                            swift_addr_t ExistentialAddress,
                                            swift_typeref_interop_t ExistentialTypeRef,
                                            swift_typeref_interop_t *OutInstanceTypeRef,
                                            swift_addr_t *OutStartOfInstanceData) {
  DECLARE_LIBRARY(ExistentialTypeRef.Library);
  int Success = Library->Functions.projectExistential(Library->Context,
                                                      ExistentialAddress,
                                                      ExistentialTypeRef.Typeref,
                                                      &OutInstanceTypeRef->Typeref,
                                                      OutStartOfInstanceData);
  if (!Success)
    return 0;
  
  OutInstanceTypeRef->Library = ExistentialTypeRef.Library;
  return 1;
}

static inline int swift_reflection_interop_projectEnumValue(
    SwiftReflectionInteropContextRef ContextRef, swift_addr_t EnumAddress,
    swift_typeref_interop_t EnumTypeRef, int *CaseIndex) {
  DECLARE_LIBRARY(EnumTypeRef.Library);
  return Library->Functions.projectEnumValue &&
         Library->Functions.projectEnumValue(Library->Context, EnumAddress,
                                             EnumTypeRef.Typeref, CaseIndex);
}

static inline void
swift_reflection_interop_dumpTypeRef(SwiftReflectionInteropContextRef ContextRef,
                                     swift_typeref_interop_t OpaqueTypeRef) {
  DECLARE_LIBRARY(OpaqueTypeRef.Library);
  Library->Functions.dumpTypeRef(OpaqueTypeRef.Typeref);
}

static inline void
swift_reflection_interop_dumpInfoForTypeRef(SwiftReflectionInteropContextRef ContextRef,
                                            swift_typeref_interop_t OpaqueTypeRef) {
  DECLARE_LIBRARY(OpaqueTypeRef.Library);
  Library->Functions.dumpInfoForTypeRef(Library->Context, OpaqueTypeRef.Typeref);
}

static inline void
swift_reflection_interop_dumpInfoForMetadata(SwiftReflectionInteropContextRef ContextRef,
                                             swift_metadata_interop_t Metadata) {
  DECLARE_LIBRARY(Metadata.Library);
  Library->Functions.dumpInfoForMetadata(Library->Context, Metadata.Metadata);
}

static inline void
swift_reflection_interop_dumpInfoForInstance(SwiftReflectionInteropContextRef ContextRef,
                                             uintptr_t Object) {
  struct SwiftReflectionInteropContextLibrary *Library
    = swift_reflection_interop_libraryForObject(ContextRef, Object);
  if (Library != NULL) {
    Library->Functions.dumpInfoForInstance(Library->Context, Object);
  }
}

static inline size_t
swift_reflection_interop_demangle(SwiftReflectionInteropContextRef ContextRef,
                                  const char *MangledName,
                                  size_t Length,
                                  char *OutDemangledName,
                                  size_t MaxLength) {
  FOREACH_LIBRARY {
    return Library->Functions.demangle(MangledName, Length, OutDemangledName, MaxLength);
  }
  return 0;
}

#undef FOREACH_LIBRARY
#undef LIBRARY_INDEX
#undef DECLARE_LIBRARY

/// @}

#endif // defined(__APPLE__) && defined(__MACH__)

#endif // SWIFT_REMOTE_MIRROR_LEGACY_INTEROP_H

