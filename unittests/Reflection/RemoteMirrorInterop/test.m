//===--- test.m - SwiftRemoteMirrorLegacyInterop test program. ------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------------===//
///
/// \file
/// This is a test program that exercises the SwiftRemoteMirrorLegacyInterop header.
///
//===----------------------------------------------------------------------------===//

#import <dlfcn.h>
#import <Foundation/Foundation.h>
#import <mach/mach.h>
#import <mach-o/dyld.h>

#import "SwiftRemoteMirrorLegacyInterop.h"


void *Load(char *path) {
  void *Handle = dlopen(path, RTLD_LOCAL);
  if (Handle == NULL) {
    fprintf(stderr, "loading %s: %s\n", path, dlerror());
    exit(1);
  }
  return Handle;
}

void Free(void *reader_context, const void *bytes, void *context) {
  assert(reader_context == (void *)0xdeadbeef);
  assert(context == (void *)0xfeedface);
  free((void *)bytes);
}

const void *ReadBytes(void *context, swift_addr_t address, uint64_t size,
                      void **outFreeContext) {
  assert(context == (void *)0xdeadbeef);
  *outFreeContext = (void *)0xfeedface;

  void *Buffer = malloc(size);
  vm_size_t InOutSize = size;
  kern_return_t result = vm_read_overwrite(mach_task_self(), address, size, (vm_address_t)Buffer, &InOutSize);
  if (result != KERN_SUCCESS) abort();
  if (InOutSize != size) abort();
  return Buffer;
}

uint64_t GetStringLength(void *context, swift_addr_t address) {
  assert(context == (void *)0xdeadbeef);
  return strlen((char *)address);
}

swift_addr_t GetSymbolAddress(void *context, const char *name, uint64_t name_length) {
  (void)name_length;
  assert(context == (void *)0xdeadbeef);
  return (swift_addr_t)dlsym(RTLD_DEFAULT, name);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <libtestswift.dylib> <libswiftRemoteMirror.dylib ...>\n",
                    argv[0]);
    exit(1);
  }
  
  char *TestLibPath = argv[1];
  
  void *TestHandle = Load(TestLibPath);
  intptr_t (*Test)(void) = dlsym(TestHandle, "test");
  
  uintptr_t Obj = Test();
  
  SwiftReflectionInteropContextRef Context =
    swift_reflection_interop_createReflectionContext(
      (void *)0xdeadbeef,
      sizeof(void *),
      Free,
      ReadBytes,
      GetStringLength,
      GetSymbolAddress);
  if (Context == NULL) {
    fprintf(stderr, "Unable to create a reflection context!\n");
    exit(1);
  }
  
  for (int i = 2; i < argc; i++) {
    void *Handle = Load(argv[i]);
    int Success = swift_reflection_interop_addLibrary(Context, Handle);
    if (!Success) {
      fprintf(stderr, "Failed to add library at %s\n", argv[i]);
      exit(1);
    }
    
    unsigned long long *isSwiftMaskPtr = dlsym(
      Handle, "swift_reflection_classIsSwiftMask");
    if (isSwiftMaskPtr) {
      printf("%s has isSwiftMask. Original value is %lld\n",
             argv[i], *isSwiftMaskPtr);
      swift_reflection_interop_setClassIsSwiftMask(Context, 1);
      printf("Set mask to 1, value is now %lld\n", *isSwiftMaskPtr);
      swift_reflection_interop_setClassIsSwiftMask(Context, 2);
      printf("Set mask to 2, value is now %lld\n", *isSwiftMaskPtr);
    } else {
      printf("%s does not have isSwiftMask\n", argv[i]);
    }
  }
  
  int hasLegacy = 0;
  int hasNonLegacy = 0;
  for (int i = 0; i < Context->LibraryCount; i++) {
    if (Context->Libraries[i].IsLegacy)
      hasLegacy = 1;
    else
      hasNonLegacy = 1;
  }
  if (hasLegacy && !hasNonLegacy) {
    printf("We can't run tests with only a legacy library. Giving up.\n");
    exit(0);
  }
  
  uint32_t ImageCount = _dyld_image_count();
  for (uint32_t i = 0; i < ImageCount; i++) {
    swift_addr_t Image = (swift_addr_t)_dyld_get_image_header(i);
    swift_reflection_interop_addImage(Context, Image);
  }
  
  swift_typeref_interop_t Type = swift_reflection_interop_typeRefForInstance(Context, Obj);
  if (Type.Typeref != 0) {
    swift_typeinfo_t TypeInfo = swift_reflection_interop_infoForTypeRef(Context, Type);
    printf("Kind:%u Size:%u Alignment:%u Stride:%u NumFields:%u\n",
           TypeInfo.Kind, TypeInfo.Size, TypeInfo.Alignment, TypeInfo.Stride,
           TypeInfo.NumFields);
    char *Name = swift_reflection_interop_copyDemangledNameForTypeRef(Context, Type);
    if (Name) {
      printf("Demangled name: %s\n", Name);
      free(Name);
    } else {
      printf("Could not get name.\n");
    }
  } else {
    printf("Unknown typeref!\n");
  }
  
  uintptr_t Metadata = *(uintptr_t *)Obj;
  swift_metadata_interop_t LookedUp =
    swift_reflection_interop_lookupMetadata(Context, Metadata);
  printf("Original metadata: %p\n", (void *)Metadata);
  printf("Looked up metadata: Metadata=%p Library=%d\n",
         (void *)LookedUp.Metadata, LookedUp.Library);
  
  swift_typeinfo_t TypeInfo = swift_reflection_interop_infoForInstance(Context, Obj);
  if (TypeInfo.Kind != SWIFT_UNKNOWN) {
    printf("Kind:%u Size:%u Alignment:%u Stride:%u NumFields:%u\n",
           TypeInfo.Kind, TypeInfo.Size, TypeInfo.Alignment, TypeInfo.Stride,
           TypeInfo.NumFields);
  
    for (unsigned i = 0; i < TypeInfo.NumFields; ++i) {
      swift_childinfo_interop_t ChildInfo = swift_reflection_interop_childOfInstance(
        Context, Obj, i);
      char *TypeName = swift_reflection_interop_copyDemangledNameForTypeRef(Context, ChildInfo.TR);

      printf("  [%u]: %s Offset:%u Kind:%u Type:%s ", i,
             ChildInfo.Name, ChildInfo.Offset, ChildInfo.Kind, TypeName);


      switch (ChildInfo.Kind) {
      case SWIFT_BUILTIN:
        printf("(Builtin value not displayed)\n");
        break;
      case SWIFT_NO_PAYLOAD_ENUM:
        {
          int CaseNdx;

          if (swift_reflection_interop_projectEnumValue(Context,
                                                        Obj + ChildInfo.Offset,
                                                        ChildInfo.TR,
                                                        &CaseNdx)) {
            swift_childinfo_interop_t CaseInfo
              = swift_reflection_interop_childOfTypeRef(Context, ChildInfo.TR,
                                                        CaseNdx);

            printf("Value: %s (%d)\n", CaseInfo.Name, CaseNdx);
          } else {
            printf("Value: unknown\n");
          }
        }
        break;
      default:
        printf("(Value not displayed)\n");
        break;
      }

      free(TypeName);
    }
  } else {
    printf("Unknown typeinfo!\n");
  }
  
  swift_reflection_interop_destroyReflectionContext(Context);
}
