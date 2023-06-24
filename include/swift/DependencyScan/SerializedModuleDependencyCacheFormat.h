//=== SerializedModuleDependencyCacheFormat.h - serialized format -*- C++-*-=//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_DEPENDENCY_SERIALIZEDCACHEFORMAT_H
#define SWIFT_DEPENDENCY_SERIALIZEDCACHEFORMAT_H

#include "llvm/Bitcode/BitcodeConvenience.h"
#include "llvm/Bitstream/BitCodes.h"

namespace llvm {
class MemoryBuffer;
namespace vfs{
class OutputBackend;
}
}

namespace swift {

class DiagnosticEngine;
class SwiftDependencyScanningService;

namespace dependencies {
namespace module_dependency_cache_serialization {

using llvm::BCArray;
using llvm::BCBlob;
using llvm::BCFixed;
using llvm::BCRecordLayout;
using llvm::BCVBR;

/// Every .moddepcache file begins with these 4 bytes, for easy identification.
const unsigned char MODULE_DEPENDENCY_CACHE_FORMAT_SIGNATURE[] = {'I', 'M', 'D','C'};
const unsigned MODULE_DEPENDENCY_CACHE_FORMAT_VERSION_MAJOR = 4;
/// Increment this on every change.
const unsigned MODULE_DEPENDENCY_CACHE_FORMAT_VERSION_MINOR = 1;

/// Various identifiers in this format will rely on having their strings mapped
/// using this ID.
using IdentifierIDField = BCVBR<13>;
using FileIDField = IdentifierIDField;
using ModuleIDField = IdentifierIDField;
using ContextHashIDField = IdentifierIDField;

/// A bit that indicates whether or not a module is a framework
using IsFrameworkField = BCFixed<1>;

/// Arrays of various identifiers, distinguished for readability
using IdentifierIDArryField = llvm::BCArray<IdentifierIDField>;
using ModuleIDArryField = llvm::BCArray<IdentifierIDField>;

/// Identifiers used to refer to the above arrays
using FileIDArrayIDField = IdentifierIDField;
using ContextHashIDField = IdentifierIDField;
using ImportArrayIDField = IdentifierIDField;
using FlagIDArrayIDField = IdentifierIDField;
using DependencyIDArrayIDField = IdentifierIDField;

/// The ID of the top-level block containing the dependency graph
const unsigned GRAPH_BLOCK_ID = llvm::bitc::FIRST_APPLICATION_BLOCKID;

/// The .moddepcache file format consists of a METADATA record, followed by
/// zero or more IDENTIFIER records that contain various strings seen in the graph
/// (e.g. file names or compiler flags), followed by zero or more IDENTIFIER_ARRAY records
/// which are arrays of identifiers seen in the graph (e.g. list of source files or list of compile flags),
/// followed by zero or more MODULE_NODE, *_DETAILS_NODE pairs of records.
namespace graph_block {
enum {
  METADATA = 1,
  MODULE_NODE,
  SWIFT_INTERFACE_MODULE_DETAILS_NODE,
  SWIFT_SOURCE_MODULE_DETAILS_NODE,
  SWIFT_PLACEHOLDER_MODULE_DETAILS_NODE,
  SWIFT_BINARY_MODULE_DETAILS_NODE,
  CLANG_MODULE_DETAILS_NODE,
  IDENTIFIER_NODE,
  IDENTIFIER_ARRAY_NODE
};

// Always the first record in the file.
using MetadataLayout = BCRecordLayout<
    METADATA,    // ID
    BCFixed<16>, // Inter-Module Dependency graph format major version
    BCFixed<16>, // Inter-Module Dependency graph format minor version
    BCBlob       // Compiler version string
    >;

// After the metadata record, we have zero or more identifier records,
// for each unique string that is referenced in the graph.
//
// Identifiers are referenced by their sequence number, starting from 1.
// The identifier value 0 is special; it always represents the empty string.
// There is no IDENTIFIER_NODE serialized that corresponds to it, instead
// the first IDENTIFIER_NODE always has a sequence number of 1.
using IdentifierNodeLayout = BCRecordLayout<IDENTIFIER_NODE, BCBlob>;

// After the identifier records we have zero or more identifier array records.
//
// These arrays are also referenced by their sequence number,
// starting from 1, similar to identifiers above. Value 0 indicates an
// empty array. This record is used because individual array fields must
// appear as the last field of whatever record they belong to, and several of
// the below record layouts contain multiple arrays.
using IdentifierArrayLayout =
    BCRecordLayout<IDENTIFIER_ARRAY_NODE, IdentifierIDArryField>;

// After the array records, we have a sequence of Module info
// records, each of which is followed by one of:
// - SwiftInterfaceModuleDetails
// - SwiftSourceModuleDetails
// - SwiftBinaryModuleDetails
// - SwiftPlaceholderModuleDetails
// - ClangModuleDetails
using ModuleInfoLayout =
    BCRecordLayout<MODULE_NODE,                  // ID
                   IdentifierIDField,            // moduleName
                   ContextHashIDField,           // contextHash
                   ImportArrayIDField,           // moduleImports
                   DependencyIDArrayIDField      // resolvedModuleDependencies
                   >;

using SwiftInterfaceModuleDetailsLayout =
    BCRecordLayout<SWIFT_INTERFACE_MODULE_DETAILS_NODE, // ID
                   FileIDField,                         // outputFilePath
                   FileIDField,                         // swiftInterfaceFile
                   FileIDArrayIDField,                  // compiledModuleCandidates
                   FlagIDArrayIDField,                  // buildCommandLine
                   FlagIDArrayIDField,                  // extraPCMArgs
                   ContextHashIDField,                  // contextHash
                   IsFrameworkField,                    // isFramework
                   FileIDField,                         // bridgingHeaderFile
                   FileIDArrayIDField,                  // sourceFiles
                   FileIDArrayIDField,                  // bridgingSourceFiles
                   FileIDArrayIDField,                  // bridgingModuleDependencies
                   DependencyIDArrayIDField,            // swiftOverlayDependencies
                   IdentifierIDField,                   // CASFileSystemRootID
                   IdentifierIDField,                   // bridgingHeaderIncludeTree
                   IdentifierIDField                    // moduleCacheKey
                   >;

using SwiftSourceModuleDetailsLayout =
    BCRecordLayout<SWIFT_SOURCE_MODULE_DETAILS_NODE, // ID
                   FlagIDArrayIDField,               // extraPCMArgs
                   FileIDField,                      // bridgingHeaderFile
                   FileIDArrayIDField,               // sourceFiles
                   FileIDArrayIDField,               // bridgingSourceFiles
                   FileIDArrayIDField,               // bridgingModuleDependencies
                   DependencyIDArrayIDField,         // swiftOverlayDependencies
                   IdentifierIDField,                // CASFileSystemRootID
                   IdentifierIDField,                // bridgingHeaderIncludeTree
                   FlagIDArrayIDField,               // buildCommandLine
                   FlagIDArrayIDField                // bridgingHeaderBuildCommandLine
                   >;

using SwiftBinaryModuleDetailsLayout =
    BCRecordLayout<SWIFT_BINARY_MODULE_DETAILS_NODE, // ID
                   FileIDField,                      // compiledModulePath
                   FileIDField,                      // moduleDocPath
                   FileIDField,                      // moduleSourceInfoPath
                   ImportArrayIDField,               // headerImports
                   IsFrameworkField,                 // isFramework
                   IdentifierIDField                 // moduleCacheKey
                   >;

using SwiftPlaceholderModuleDetailsLayout =
    BCRecordLayout<SWIFT_PLACEHOLDER_MODULE_DETAILS_NODE, // ID
                   FileIDField,                           // compiledModulePath
                   FileIDField,                           // moduleDocPath
                   FileIDField                            // moduleSourceInfoPath
                   >;

using ClangModuleDetailsLayout =
    BCRecordLayout<CLANG_MODULE_DETAILS_NODE, // ID
                   FileIDField,               // pcmOutputPath
                   FileIDField,               // moduleMapPath
                   ContextHashIDField,        // contextHash
                   FlagIDArrayIDField,        // commandLine
                   FileIDArrayIDField,        // fileDependencies
                   FlagIDArrayIDField,        // capturedPCMArgs
                   IdentifierIDField,         // CASFileSystemRootID
                   IdentifierIDField,         // clangIncludeTreeRoot
                   IdentifierIDField          // moduleCacheKey
                   >;
} // namespace graph_block

/// Tries to read the dependency graph from the given buffer.
/// Returns \c true if there was an error.
bool readInterModuleDependenciesCache(llvm::MemoryBuffer &buffer,
                                      SwiftDependencyScanningService &cache);

/// Tries to read the dependency graph from the given path name.
/// Returns true if there was an error.
bool readInterModuleDependenciesCache(llvm::StringRef path,
                                      SwiftDependencyScanningService &cache);

/// Tries to write the dependency graph to the given path name.
/// Returns true if there was an error.
bool writeInterModuleDependenciesCache(DiagnosticEngine &diags,
                                       llvm::vfs::OutputBackend &backend,
                                       llvm::StringRef path,
                                       const SwiftDependencyScanningService &cache);

/// Tries to write out the given dependency cache with the given
/// bitstream writer.
void writeInterModuleDependenciesCache(llvm::BitstreamWriter &Out,
                                       const SwiftDependencyScanningService &cache);

} // end namespace module_dependency_cache_serialization
} // end namespace dependencies
} // end namespace swift

#endif
