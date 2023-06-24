//===--- SerializationOptions.h - Control swiftmodule emission --*- C++ -*-===//
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

#ifndef SWIFT_SERIALIZATION_SERIALIZATIONOPTIONS_H
#define SWIFT_SERIALIZATION_SERIALIZATIONOPTIONS_H

#include "swift/AST/SearchPathOptions.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/PathRemapper.h"
#include "llvm/Support/VersionTuple.h"

namespace swift {

  class SerializationOptions {
    SerializationOptions(const SerializationOptions &) = delete;
    void operator=(const SerializationOptions &) = delete;

  public:
    SerializationOptions() = default;
    SerializationOptions(SerializationOptions &&) = default;
    SerializationOptions &operator=(SerializationOptions &&) = default;
    ~SerializationOptions() = default;

    StringRef OutputPath;
    StringRef DocOutputPath;
    StringRef SourceInfoOutputPath;
    std::string ABIDescriptorPath;
    bool emptyABIDescriptor = false;
    llvm::VersionTuple UserModuleVersion;
    std::set<std::string> AllowableClients;
    std::string SDKName;

    StringRef GroupInfoPath;
    StringRef ImportedHeader;
    StringRef ModuleLinkName;
    StringRef ModuleInterface;
    std::vector<std::string> ExtraClangOptions;
    std::vector<swift::PluginSearchOption> PluginSearchOptions;

    /// Path prefixes that should be rewritten in debug info.
    PathRemapper DebuggingOptionsPrefixMap;

    /// Obfuscate the serialized paths so we don't have the actual paths encoded
    /// in the .swiftmodule file.
    PathObfuscator PathObfuscator;

    /// Describes a single-file dependency for this module, along with the
    /// appropriate strategy for how to verify if it's up-to-date.
    class FileDependency {
      /// The size of the file on disk, in bytes.
      uint64_t Size : 62;

      /// A dependency can be either hash-based or modification-time-based.
      bool IsHashBased : 1;

      /// The dependency path can be absolute or relative to the SDK
      bool IsSDKRelative : 1;

      union {
        /// The last modification time of the file.
        uint64_t ModificationTime;

        /// The xxHash of the full contents of the file.
        uint64_t ContentHash;
      };

      /// The path to the dependency.
      std::string Path;

      FileDependency(uint64_t size, bool isHash, uint64_t hashOrModTime,
                     StringRef path, bool isSDKRelative):
        Size(size), IsHashBased(isHash), IsSDKRelative(isSDKRelative),
        ModificationTime(hashOrModTime), Path(path) {}
    public:
      FileDependency() = delete;

      /// Creates a new hash-based file dependency.
      static FileDependency
      hashBased(StringRef path, bool isSDKRelative, uint64_t size, uint64_t hash) {
        return FileDependency(size, /*isHash*/true, hash, path, isSDKRelative);
      }

      /// Creates a new modification time-based file dependency.
      static FileDependency
      modTimeBased(StringRef path, bool isSDKRelative, uint64_t size, uint64_t mtime) {
        return FileDependency(size, /*isHash*/false, mtime, path, isSDKRelative);
      }

      /// Updates the last-modified time of this dependency.
      /// If the dependency is a hash-based dependency, it becomes
      /// modification time-based.
      void setLastModificationTime(uint64_t mtime) {
        IsHashBased = false;
        ModificationTime = mtime;
      }

      /// Updates the content hash of this dependency.
      /// If the dependency is a modification time-based dependency, it becomes
      /// hash-based.
      void setContentHash(uint64_t hash) {
        IsHashBased = true;
        ContentHash = hash;
      }

      /// Determines if this dependency is hash-based and should be validated
      /// based on content hash.
      bool isHashBased() const { return IsHashBased; }

      /// Determines if this dependency is absolute or relative to the SDK.
      bool isSDKRelative() const { return IsSDKRelative; }

      /// Determines if this dependency is hash-based and should be validated
      /// based on modification time.
      bool isModificationTimeBased() const { return !IsHashBased; }

      /// Gets the modification time, if this is a modification time-based
      /// dependency.
      uint64_t getModificationTime() const {
        assert(isModificationTimeBased() &&
               "cannot get modification time for hash-based dependency");
        return ModificationTime;
      }

      /// Gets the content hash, if this is a hash-based
      /// dependency.
      uint64_t getContentHash() const {
        assert(isHashBased() &&
               "cannot get content hash for mtime-based dependency");
        return ContentHash;
      }

      StringRef getPath() const { return Path; }
      uint64_t getSize() const { return Size; }
    };
    ArrayRef<FileDependency> Dependencies;
    ArrayRef<std::string> PublicDependentLibraries;

    bool AutolinkForceLoad = false;
    bool SerializeAllSIL = false;
    bool SerializeOptionsForDebugging = false;
    bool IsSIB = false;
    bool DisableCrossModuleIncrementalInfo = false;
    bool StaticLibrary = false;
    bool HermeticSealAtLink = false;
    bool IsOSSA = false;
  };

} // end namespace swift
#endif
