//===--- Setup.cpp - Load-time setup code ---------------------------------===//
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

#include "swift/Runtime/Metadata.h"

// Helper macros for figuring out the mangled name of a context descriptor.
#define DESCRIPTOR_MANGLING_SUFFIX_Structure Mn
#define DESCRIPTOR_MANGLING_SUFFIX_Class Mn
#define DESCRIPTOR_MANGLING_SUFFIX_Enum Mn
#define DESCRIPTOR_MANGLING_SUFFIX_Protocol Mp

#define DESCRIPTOR_MANGLING_SUFFIX_(X) X
#define DESCRIPTOR_MANGLING_SUFFIX(KIND)                                       \
  DESCRIPTOR_MANGLING_SUFFIX_(DESCRIPTOR_MANGLING_SUFFIX_##KIND)

#define DESCRIPTOR_MANGLING_(CHAR, SUFFIX) $sSc##CHAR##SUFFIX
#define DESCRIPTOR_MANGLING(CHAR, SUFFIX) DESCRIPTOR_MANGLING_(CHAR, SUFFIX)

// Declare context descriptors for all of the concurrency descriptors with
// standard manglings.
#define STANDARD_TYPE(KIND, MANGLING, TYPENAME)
#define STANDARD_TYPE_CONCURRENCY(KIND, MANGLING, TYPENAME)                    \
  extern "C" const swift::ContextDescriptor DESCRIPTOR_MANGLING(               \
      MANGLING, DESCRIPTOR_MANGLING_SUFFIX(KIND));
#include "swift/Demangling/StandardTypesMangling.def"

// Register our type descriptors with standard manglings when the concurrency
// runtime is loaded. This allows the runtime to quickly resolve those standard
// manglings.
__attribute__((constructor)) static void setupStandardConcurrencyDescriptors() {
  static const swift::ConcurrencyStandardTypeDescriptors descriptors = {
#define STANDARD_TYPE(KIND, MANGLING, TYPENAME)
#define STANDARD_TYPE_CONCURRENCY(KIND, MANGLING, TYPENAME)                    \
  &DESCRIPTOR_MANGLING(MANGLING, DESCRIPTOR_MANGLING_SUFFIX(KIND)),
#include "swift/Demangling/StandardTypesMangling.def"
  };
  _swift_registerConcurrencyStandardTypeDescriptors(&descriptors);
}
