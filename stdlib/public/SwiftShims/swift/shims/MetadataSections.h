//===--- MetadataSections.h -----------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the declaration of the MetadataSectionsRange and 
/// MetadataSections struct, which represent, respectively,  information about  
/// an image's section, and an image's metadata information (which is composed
/// of multiple section information).
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_STDLIB_SHIMS_METADATASECTIONS_H
#define SWIFT_STDLIB_SHIMS_METADATASECTIONS_H

#if defined(__cplusplus) && !defined(__swift__)
#include <atomic>
#endif

#include "SwiftStddef.h"
#include "SwiftStdint.h"

#ifdef __cplusplus
namespace swift { 
extern "C" {
#endif

/// Specifies the address range corresponding to a section.
typedef struct MetadataSectionRange {
  __swift_uintptr_t start;
  __swift_size_t length;
} MetadataSectionRange;


/// Identifies the address space ranges for the Swift metadata required by the
/// Swift runtime.
///
/// \warning If you change the size of this structure by adding fields, it is an
///   ABI-breaking change on platforms that use it. Make sure to increment
///   \c CurrentSectionMetadataVersion if you do. To minimize impact, always add
///   new fields to the \em end of the structure.
struct MetadataSections {
  __swift_uintptr_t version;

  /// The base address of the image where this metadata section was defined, as
  /// reported when the section was registered with the Swift runtime.
  ///
  /// The value of this field is equivalent to the value of
  /// \c SymbolInfo::baseAddress as returned from \c SymbolInfo::lookup() for a
  /// symbol in the image that contains these sections.
  ///
  /// For Mach-O images, set this field to \c __dso_handle (i.e. the Mach header
  /// for the image.) For ELF images, set it to \c __dso_handle (the runtime
  /// will adjust it to the start of the ELF image when the image is loaded.)
  /// For COFF images, set this field to \c __ImageBase.
  ///
  /// For platforms that have a single statically-linked image or no dynamic
  /// loader (i.e. no equivalent of \c __dso_handle or \c __ImageBase), this
  /// field is ignored and should be set to \c nullptr.
  ///
  /// \bug When imported into Swift, this field is not atomic.
  ///
  /// \sa swift_addNewDSOImage()
#if defined(__swift__) || defined(__STDC_NO_ATOMICS__)
  const void *baseAddress;
#elif defined(__cplusplus)
  std::atomic<const void *> baseAddress;
#else
  _Atomic(const void *) baseAddress;
#endif

  /// Unused.
  ///
  /// These pointers (or the space they occupy) can be repurposed without
  /// causing ABI breakage. Set them to \c nullptr.
  void *unused0;
  void *unused1;

  MetadataSectionRange swift5_protocols;
  MetadataSectionRange swift5_protocol_conformances;
  MetadataSectionRange swift5_type_metadata;
  MetadataSectionRange swift5_typeref;
  MetadataSectionRange swift5_reflstr;
  MetadataSectionRange swift5_fieldmd;
  MetadataSectionRange swift5_assocty;
  MetadataSectionRange swift5_replace;
  MetadataSectionRange swift5_replac2;
  MetadataSectionRange swift5_builtin;
  MetadataSectionRange swift5_capture;
  MetadataSectionRange swift5_accessible_functions;
  MetadataSectionRange swift5_runtime_attributes;
  MetadataSectionRange swift5_tests;
};

#ifdef __cplusplus
} //extern "C"
} // namespace swift
#endif

#endif // SWIFT_STDLIB_SHIMS_METADATASECTIONS_H
