//===--- ModuleFileSharedCore.cpp - Core of a serialized module -----------===//
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

#include "ModuleFileSharedCore.h"
#include "BCReadingExtras.h"
#include "DeserializationErrors.h"
#include "ModuleFileCoreTableInfo.h"
#include "swift/Basic/LangOptions.h"
#include "swift/Parse/ParseVersion.h"
#include "swift/Strings.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/OnDiskHashTable.h"
#include "llvm/Support/PrettyStackTrace.h"

using namespace swift;
using namespace swift::serialization;
using namespace llvm::support;
using llvm::Expected;

static bool checkModuleSignature(llvm::BitstreamCursor &cursor,
                                 ArrayRef<unsigned char> signature) {
  for (unsigned char byte : signature) {
    if (cursor.AtEndOfStream())
      return false;
    if (Expected<llvm::SimpleBitstreamCursor::word_t> maybeRead =
            cursor.Read(8)) {
      if (maybeRead.get() != byte)
        return false;
    } else {
      consumeError(maybeRead.takeError());
      return false;
    }
  }
  return true;
}

static bool enterTopLevelModuleBlock(llvm::BitstreamCursor &cursor,
                                     unsigned ID,
                                     bool shouldReadBlockInfo = true) {
  Expected<llvm::BitstreamEntry> maybeNext = cursor.advance();
  if (!maybeNext) {
    // FIXME this drops the error on the floor.
    consumeError(maybeNext.takeError());
    return false;
  }
  llvm::BitstreamEntry next = maybeNext.get();

  if (next.Kind != llvm::BitstreamEntry::SubBlock)
    return false;

  if (next.ID == llvm::bitc::BLOCKINFO_BLOCK_ID) {
    if (shouldReadBlockInfo) {
      if (!cursor.ReadBlockInfoBlock())
        return false;
    } else {
      if (cursor.SkipBlock())
        return false;
    }
    return enterTopLevelModuleBlock(cursor, ID, false);
  }

  if (next.ID != ID)
    return false;

  if (llvm::Error Err = cursor.EnterSubBlock(ID)) {
    // FIXME this drops the error on the floor.
    consumeError(std::move(Err));
    return false;
  }

  return true;
}

/// Populate \p extendedInfo with the data from the options block.
///
/// Returns true on success.
static bool readOptionsBlock(llvm::BitstreamCursor &cursor,
                             SmallVectorImpl<uint64_t> &scratch,
                             ExtendedValidationInfo &extendedInfo,
                             PathObfuscator &pathRecoverer) {
  while (!cursor.AtEndOfStream()) {
    Expected<llvm::BitstreamEntry> maybeEntry = cursor.advance();
    if (!maybeEntry) {
      // FIXME this drops the error on the floor.
      consumeError(maybeEntry.takeError());
      return false;
    }
    llvm::BitstreamEntry entry = maybeEntry.get();
    if (entry.Kind == llvm::BitstreamEntry::EndBlock)
      break;

    if (entry.Kind == llvm::BitstreamEntry::Error)
      return false;

    if (entry.Kind == llvm::BitstreamEntry::SubBlock) {
      // Unknown metadata sub-block, possibly for use by a future version of
      // the module format.
      if (cursor.SkipBlock())
        return false;
      continue;
    }

    scratch.clear();
    StringRef blobData;
    Expected<unsigned> maybeKind =
        cursor.readRecord(entry.ID, scratch, &blobData);
    if (!maybeKind) {
      // FIXME this drops the error on the floor.
      consumeError(maybeKind.takeError());
      return false;
    }
    unsigned kind = maybeKind.get();
    switch (kind) {
    case options_block::SDK_PATH:
      extendedInfo.setSDKPath(pathRecoverer.recover(blobData));
      break;
    case options_block::XCC:
      extendedInfo.addExtraClangImporterOption(blobData);
      break;
    case options_block::PLUGIN_SEARCH_OPTION: {
      unsigned kind;
      options_block::ResilienceStrategyLayout::readRecord(scratch, kind);
      PluginSearchOption::Kind optKind;
      switch (PluginSearchOptionKind(kind)) {
      case PluginSearchOptionKind::PluginPath:
        optKind = PluginSearchOption::Kind::PluginPath;
        break;
      case PluginSearchOptionKind::ExternalPluginPath:
        optKind = PluginSearchOption::Kind::ExternalPluginPath;
        break;
      case PluginSearchOptionKind::LoadPluginLibrary:
        optKind = PluginSearchOption::Kind::LoadPluginLibrary;
        break;
      case PluginSearchOptionKind::LoadPluginExecutable:
        optKind = PluginSearchOption::Kind::LoadPluginExecutable;
        break;
      }
      extendedInfo.addPluginSearchOption({optKind, blobData});
      break;
    }
    case options_block::IS_SIB:
      bool IsSIB;
      options_block::IsSIBLayout::readRecord(scratch, IsSIB);
      extendedInfo.setIsSIB(IsSIB);
      break;
    case options_block::IS_STATIC_LIBRARY:
      extendedInfo.setIsStaticLibrary(true);
      break;
    case options_block::HAS_HERMETIC_SEAL_AT_LINK:
      extendedInfo.setHasHermeticSealAtLink(true);
      break;
    case options_block::IS_TESTABLE:
      extendedInfo.setIsTestable(true);
      break;
    case options_block::ARE_PRIVATE_IMPORTS_ENABLED:
      extendedInfo.setPrivateImportsEnabled(true);
      break;
    case options_block::IS_IMPLICIT_DYNAMIC_ENABLED:
      extendedInfo.setImplicitDynamicEnabled(true);
      break;
    case options_block::RESILIENCE_STRATEGY:
      unsigned Strategy;
      options_block::ResilienceStrategyLayout::readRecord(scratch, Strategy);
      extendedInfo.setResilienceStrategy(ResilienceStrategy(Strategy));
      break;
    case options_block::IS_BUILT_FROM_INTERFACE:
      extendedInfo.setIsBuiltFromInterface(true);
      break;
    case options_block::IS_ALLOW_MODULE_WITH_COMPILER_ERRORS_ENABLED:
      extendedInfo.setAllowModuleWithCompilerErrorsEnabled(true);
      break;
    case options_block::MODULE_ABI_NAME:
      extendedInfo.setModuleABIName(blobData);
      break;
    case options_block::IS_CONCURRENCY_CHECKED:
      extendedInfo.setIsConcurrencyChecked(true);
      break;
    case options_block::MODULE_PACKAGE_NAME:
      extendedInfo.setModulePackageName(blobData);
      break;
    case options_block::MODULE_EXPORT_AS_NAME:
      extendedInfo.setExportAsName(blobData);
      break;
    case options_block::HAS_CXX_INTEROPERABILITY_ENABLED:
      extendedInfo.setHasCxxInteroperability(true);
      break;
    default:
      // Unknown options record, possibly for use by a future version of the
      // module format.
      break;
    }
  }

  return true;
}

static ValidationInfo validateControlBlock(
    llvm::BitstreamCursor &cursor, SmallVectorImpl<uint64_t> &scratch,
    std::pair<uint16_t, uint16_t> expectedVersion, bool requiresOSSAModules,
    bool requiresRevisionMatch,
    StringRef requiredSDK,
    ExtendedValidationInfo *extendedInfo,
    PathObfuscator &pathRecoverer) {
  // The control block is malformed until we've at least read a major version
  // number.
  ValidationInfo result;
  bool versionSeen = false;
  bool revisionSeen = false;

  while (!cursor.AtEndOfStream()) {
    Expected<llvm::BitstreamEntry> maybeEntry = cursor.advance();
    if (!maybeEntry) {
      // FIXME this drops the error on the floor.
      consumeError(maybeEntry.takeError());
      result.status = Status::Malformed;
      return result;
    }
    llvm::BitstreamEntry entry = maybeEntry.get();
    if (entry.Kind == llvm::BitstreamEntry::EndBlock)
      break;

    if (entry.Kind == llvm::BitstreamEntry::Error) {
      result.status = Status::Malformed;
      return result;
    }

    if (entry.Kind == llvm::BitstreamEntry::SubBlock) {
      if (entry.ID == OPTIONS_BLOCK_ID && extendedInfo) {
        if (llvm::Error Err = cursor.EnterSubBlock(OPTIONS_BLOCK_ID)) {
          // FIXME this drops the error on the floor.
          consumeError(std::move(Err));
          result.status = Status::Malformed;
          return result;
        }
        if (!readOptionsBlock(cursor, scratch, *extendedInfo, pathRecoverer)) {
          result.status = Status::Malformed;
          return result;
        }
      } else {
        // Unknown metadata sub-block, possibly for use by a future version of
        // the module format.
        if (cursor.SkipBlock()) {
          result.status = Status::Malformed;
          return result;
        }
      }
      continue;
    }

    scratch.clear();
    StringRef blobData;
    Expected<unsigned> maybeKind =
        cursor.readRecord(entry.ID, scratch, &blobData);
    if (!maybeKind) {
      // FIXME this drops the error on the floor.
      consumeError(maybeKind.takeError());
      result.status = Status::Malformed;
      return result;
    }
    unsigned kind = maybeKind.get();
    switch (kind) {
    case control_block::METADATA: {
      if (versionSeen) {
        result.status = Status::Malformed;
        break;
      }

      uint16_t versionMajor = scratch[0];
      if (versionMajor > expectedVersion.first)
        result.status = Status::FormatTooNew;
      else if (versionMajor < expectedVersion.first)
        result.status = Status::FormatTooOld;
      else
        result.status = Status::Valid;

      // Major version 0 does not have stable minor versions.
      if (versionMajor == 0) {
        uint16_t versionMinor = scratch[1];
        if (versionMinor != expectedVersion.second) {
          if (versionMinor < expectedVersion.second)
            result.status = Status::FormatTooOld;
          else
            result.status = Status::FormatTooNew;
        }
      }

      // These fields were added later; be resilient against their absence.
      switch (scratch.size()) {
      default:
      // Add new cases here, in descending order.
      case 8:
      case 7:
      case 6:
      case 5: {
        auto subMinor = 0;
        auto build = 0;
        // case 7 and 8 were added after case 5 and 6, so we need to have this
        // special handling to make sure we can still load the module without
        // case 7 and case 8 successfully.
        if (scratch.size() >= 8) {
          subMinor = scratch[6];
          build = scratch[7];
        }
        result.userModuleVersion = llvm::VersionTuple(scratch[4], scratch[5],
                                                      subMinor, build);
        LLVM_FALLTHROUGH;
      }
      case 4:
        if (scratch[3] != 0) {
          result.compatibilityVersion = *VersionParser::parseVersionString(
              blobData.substr(scratch[2] + 1, scratch[3]), SourceLoc(),
              nullptr);
        }
        LLVM_FALLTHROUGH;
      case 3:
        result.shortVersion = blobData.slice(0, scratch[2]);
        LLVM_FALLTHROUGH;
      case 2:
      case 1:
      case 0:
        break;
      }

      result.miscVersion = blobData;
      versionSeen = true;
      break;
    }
    case control_block::MODULE_NAME:
      result.name = blobData;
      break;
    case control_block::TARGET:
      result.targetTriple = blobData;
      break;
    case control_block::ALLOWABLE_CLIENT_NAME:
      result.allowableClients.push_back(blobData);
      break;
    case control_block::SDK_NAME: {
      result.sdkName = blobData;

      // Enable this check for tagged compiler or when the
      // env var is set (for testing).
      static const char* forceDebugPreSDKRestriction =
        ::getenv("SWIFT_DEBUG_FORCE_SWIFTMODULE_PER_SDK");
      if (!version::isCurrentCompilerTagged() &&
          !forceDebugPreSDKRestriction) {
        break;
      }

      // The loaded module was built with a compatible SDK if either:
      // * it was the same SDK
      // * or one who's name is a prefix of the clients' SDK name. This expects
      // that a module built with macOS11 can be used with the macOS11.secret SDK.
      // This is generally the case as SDKs with suffixes are a superset of the
      // short SDK name equivalent. While this is accepted, this is still not a
      // recommended configuration and may lead to unreadable swiftmodules.
      StringRef moduleSDK = blobData;
      if (!moduleSDK.empty() && !requiredSDK.empty() &&
          !requiredSDK.startswith(moduleSDK)) {
        result.status = Status::SDKMismatch;
        return result;
      }

      break;
    }
    case control_block::REVISION: {
      revisionSeen = true;

      // Tagged compilers should only load modules if they were
      // produced by the exact same compiler tag.

      // Disable this restriction for compiler testing by setting this
      // env var to any value.
      static const char* ignoreRevision =
        ::getenv("SWIFT_IGNORE_SWIFTMODULE_REVISION");
      if (ignoreRevision)
        break;

      // Override this env var for testing, forcing the behavior of a tagged
      // compiler and using the env var value to override this compiler's
      // revision.
      static const char* forcedDebugRevision =
        ::getenv("SWIFT_DEBUG_FORCE_SWIFTMODULE_REVISION");

      StringRef moduleRevision = blobData;
      if (forcedDebugRevision ||
          (requiresRevisionMatch && version::isCurrentCompilerTagged())) {
        StringRef compilerRevision = forcedDebugRevision ?
          forcedDebugRevision : version::getCurrentCompilerSerializationTag();
        if (moduleRevision != compilerRevision) {
          // The module versions are mismatching, record it and diagnose later.
          result.problematicRevision = moduleRevision;

          // Reject the module only it still mismatches without the last digit.
          StringRef compilerRevisionHead = compilerRevision.rsplit('.').first;
          StringRef moduleRevisionHead = moduleRevision.rsplit('.').first;
          if (moduleRevisionHead != compilerRevisionHead) {
            result.status = Status::RevisionIncompatible;

            // We can't trust the module format at this point.
            return result;
          }
        }
      }
      break;
    }
    case control_block::IS_OSSA: {
      auto isModuleInOSSA = scratch[0];
      if (requiresOSSAModules && !isModuleInOSSA)
        result.status = Status::NotInOSSA;
      break;
    }
    default:
      // Unknown metadata record, possibly for use by a future version of the
      // module format.
      break;
    }
  }

  // Last resort check in cases where the format is broken enough that
  // we didn't read the REVISION block, report such a case as incompatible.
  if (requiresRevisionMatch &&
      !revisionSeen &&
      result.status == Status::Valid)
    result.status = Status::RevisionIncompatible;

  return result;
}

static bool validateInputBlock(
    llvm::BitstreamCursor &cursor, SmallVectorImpl<uint64_t> &scratch,
    SmallVectorImpl<SerializationOptions::FileDependency> *dependencies,
    SmallVectorImpl<SearchPath> *searchPaths) {
  SmallVector<StringRef, 4> dependencyDirectories;
  SmallString<256> dependencyFullPathBuffer;

  while (!cursor.AtEndOfStream()) {
    Expected<llvm::BitstreamEntry> maybeEntry = cursor.advance();
    if (!maybeEntry) {
      // FIXME this drops the error on the floor.
      consumeError(maybeEntry.takeError());
      return true;
    }
    llvm::BitstreamEntry entry = maybeEntry.get();
    if (entry.Kind == llvm::BitstreamEntry::EndBlock)
      break;

    if (entry.Kind == llvm::BitstreamEntry::Error)
      return true;

    scratch.clear();
    StringRef blobData;
    Expected<unsigned> maybeKind =
        cursor.readRecord(entry.ID, scratch, &blobData);
    if (!maybeKind) {
      // FIXME this drops the error on the floor.
      consumeError(maybeKind.takeError());
      return true;
    }
    unsigned kind = maybeKind.get();
    switch (kind) {
    case input_block::FILE_DEPENDENCY:
      if (dependencies) {
        bool isHashBased = scratch[2] != 0;
        bool isSDKRelative = scratch[3] != 0;

        StringRef path = blobData;
        size_t directoryIndex = scratch[4];
        if (directoryIndex != 0) {
          if (directoryIndex > dependencyDirectories.size())
            return true;
          dependencyFullPathBuffer = dependencyDirectories[directoryIndex - 1];
          llvm::sys::path::append(dependencyFullPathBuffer, blobData);
          path = dependencyFullPathBuffer;
        }

        if (isHashBased)
          dependencies->push_back(
              SerializationOptions::FileDependency::hashBased(
                  path, isSDKRelative, scratch[0], scratch[1]));
        else
          dependencies->push_back(
              SerializationOptions::FileDependency::modTimeBased(
                  path, isSDKRelative, scratch[0], scratch[1]));
      }
      break;
    case input_block::DEPENDENCY_DIRECTORY:
      if (dependencies)
        dependencyDirectories.push_back(blobData);
      break;
    case input_block::SEARCH_PATH:
      if (searchPaths) {
        bool isFramework;
        bool isSystem;
        input_block::SearchPathLayout::readRecord(scratch, isFramework,
                                                  isSystem);
        searchPaths->push_back({std::string(blobData), isFramework, isSystem});
      }
      break;
    default:
      // Unknown metadata record, possibly for use by a future version of the
      // module format.
      break;
    }
  }
  return false;
}

bool serialization::isSerializedAST(StringRef data) {
  StringRef signatureStr(reinterpret_cast<const char *>(SWIFTMODULE_SIGNATURE),
                         std::size(SWIFTMODULE_SIGNATURE));
  return data.startswith(signatureStr);
}

ValidationInfo serialization::validateSerializedAST(
    StringRef data, bool requiresOSSAModules, StringRef requiredSDK,
    bool requiresRevisionMatch, ExtendedValidationInfo *extendedInfo,
    SmallVectorImpl<SerializationOptions::FileDependency> *dependencies,
    SmallVectorImpl<SearchPath> *searchPaths) {
  ValidationInfo result;

  // Check 32-bit alignment.
  if (data.size() % SWIFTMODULE_ALIGNMENT != 0 ||
      reinterpret_cast<uintptr_t>(data.data()) % SWIFTMODULE_ALIGNMENT != 0)
    return result;

  llvm::BitstreamCursor cursor(data);
  SmallVector<uint64_t, 32> scratch;

  if (!checkModuleSignature(cursor, SWIFTMODULE_SIGNATURE) ||
      !enterTopLevelModuleBlock(cursor, MODULE_BLOCK_ID, false))
    return result;

  llvm::BitstreamEntry topLevelEntry;

  while (!cursor.AtEndOfStream()) {
    Expected<llvm::BitstreamEntry> maybeEntry =
        cursor.advance(AF_DontPopBlockAtEnd);
    if (!maybeEntry) {
      // FIXME this drops the error on the floor.
      consumeError(maybeEntry.takeError());
      result.status = Status::Malformed;
      return result;
    }
    topLevelEntry = maybeEntry.get();
    if (topLevelEntry.Kind != llvm::BitstreamEntry::SubBlock)
      break;

    if (topLevelEntry.ID == CONTROL_BLOCK_ID) {
      if (llvm::Error Err = cursor.EnterSubBlock(CONTROL_BLOCK_ID)) {
        // FIXME this drops the error on the floor.
        consumeError(std::move(Err));
        result.status = Status::Malformed;
        return result;
      }
      PathObfuscator localObfuscator;
      result = validateControlBlock(
          cursor, scratch,
          {SWIFTMODULE_VERSION_MAJOR, SWIFTMODULE_VERSION_MINOR},
          requiresOSSAModules, requiresRevisionMatch,
          requiredSDK,
          extendedInfo, localObfuscator);
      if (result.status != Status::Valid)
        return result;
    } else if ((dependencies || searchPaths) &&
               result.status == Status::Valid &&
               topLevelEntry.ID == INPUT_BLOCK_ID) {
      if (llvm::Error Err = cursor.EnterSubBlock(INPUT_BLOCK_ID)) {
        // FIXME this drops the error on the floor.
        consumeError(std::move(Err));
        result.status = Status::Malformed;
        return result;
      }
      if (validateInputBlock(cursor, scratch, dependencies, searchPaths)) {
        result.status = Status::Malformed;
        return result;
      }
    } else {
      if (cursor.SkipBlock()) {
        result.status = Status::Malformed;
        return result;
      }
    }
  }

  if (topLevelEntry.Kind == llvm::BitstreamEntry::EndBlock) {
    cursor.ReadBlockEnd();
    assert(cursor.GetCurrentBitNo() % CHAR_BIT == 0);
    result.bytes = cursor.GetCurrentBitNo() / CHAR_BIT;
  } else {
    result.status = Status::Malformed;
  }

  return result;
}

std::string ModuleFileSharedCore::Dependency::getPrettyPrintedPath() const {
  StringRef pathWithoutScope = RawPath;
  if (isScoped()) {
    size_t splitPoint = pathWithoutScope.find_last_of('\0');
    pathWithoutScope = pathWithoutScope.slice(0, splitPoint);
  }
  std::string output = pathWithoutScope.str();
  std::replace(output.begin(), output.end(), '\0', '.');
  return output;
}

void ModuleFileSharedCore::fatal(llvm::Error error) const {
  llvm::SmallString<0> errorStr;
  llvm::raw_svector_ostream out(errorStr);

  out << "*** DESERIALIZATION FAILURE ***\n";
  out << "*** If any module named here was modified in the SDK, please delete the ***\n";
  out << "*** new swiftmodule files from the SDK and keep only swiftinterfaces.   ***\n";
  outputDiagnosticInfo(out);
  out << "\n";
  if (error) {
    handleAllErrors(std::move(error), [&](const llvm::ErrorInfoBase &ei) {
      ei.log(out);
      out << "\n";
    });
  }

  llvm::PrettyStackTraceString trace(errorStr.c_str());
  abort();
}

void ModuleFileSharedCore::outputDiagnosticInfo(llvm::raw_ostream &os) const {
  bool resilient = ResilienceStrategy(Bits.ResilienceStrategy) ==
                   ResilienceStrategy::Resilient;
  os << "module '" << Name
     << "', builder version '" << MiscVersion
     << "', built from "
     << (Bits.IsBuiltFromInterface? "swiftinterface": "source")
     << ", " << (resilient? "resilient": "non-resilient");
  if (Bits.IsAllowModuleWithCompilerErrorsEnabled)
    os << ", built with -experimental-allow-module-with-compiler-errors";
  if (ModuleInputBuffer)
    os << ", loaded from '" << ModuleInputBuffer->getBufferIdentifier() << "'";
}

ModuleFileSharedCore::~ModuleFileSharedCore() { }

std::unique_ptr<ModuleFileSharedCore::SerializedDeclTable>
ModuleFileSharedCore::readDeclTable(ArrayRef<uint64_t> fields,
                              StringRef blobData) const {
  uint32_t tableOffset;
  index_block::DeclListLayout::readRecord(fields, tableOffset);
  auto base = reinterpret_cast<const uint8_t *>(blobData.data());

  using OwnedTable = std::unique_ptr<SerializedDeclTable>;
  return OwnedTable(SerializedDeclTable::Create(base + tableOffset,
                                                base + sizeof(uint32_t), base));
}

std::unique_ptr<ModuleFileSharedCore::SerializedExtensionTable>
ModuleFileSharedCore::readExtensionTable(ArrayRef<uint64_t> fields,
                                   StringRef blobData) const {
  uint32_t tableOffset;
  index_block::DeclListLayout::readRecord(fields, tableOffset);
  auto base = reinterpret_cast<const uint8_t *>(blobData.data());

  using OwnedTable = std::unique_ptr<SerializedExtensionTable>;
  return OwnedTable(SerializedExtensionTable::Create(base + tableOffset,
    base + sizeof(uint32_t), base, ExtensionTableInfo(*this)));
}

std::unique_ptr<ModuleFileSharedCore::SerializedLocalDeclTable>
ModuleFileSharedCore::readLocalDeclTable(ArrayRef<uint64_t> fields,
                                   StringRef blobData) const {
  uint32_t tableOffset;
  index_block::DeclListLayout::readRecord(fields, tableOffset);
  auto base = reinterpret_cast<const uint8_t *>(blobData.data());

  using OwnedTable = std::unique_ptr<SerializedLocalDeclTable>;
  return OwnedTable(SerializedLocalDeclTable::Create(base + tableOffset,
    base + sizeof(uint32_t), base));
}

std::unique_ptr<ModuleFileSharedCore::SerializedNestedTypeDeclsTable>
ModuleFileSharedCore::readNestedTypeDeclsTable(ArrayRef<uint64_t> fields,
                                     StringRef blobData) const {
  uint32_t tableOffset;
  index_block::NestedTypeDeclsLayout::readRecord(fields, tableOffset);
  auto base = reinterpret_cast<const uint8_t *>(blobData.data());

  using OwnedTable = std::unique_ptr<SerializedNestedTypeDeclsTable>;
  return OwnedTable(SerializedNestedTypeDeclsTable::Create(base + tableOffset,
      base + sizeof(uint32_t), base));
}

std::unique_ptr<ModuleFileSharedCore::SerializedDeclMemberNamesTable>
ModuleFileSharedCore::readDeclMemberNamesTable(ArrayRef<uint64_t> fields,
                                     StringRef blobData) const {
  uint32_t tableOffset;
  index_block::DeclMemberNamesLayout::readRecord(fields, tableOffset);
  auto base = reinterpret_cast<const uint8_t *>(blobData.data());

  using OwnedTable = std::unique_ptr<SerializedDeclMemberNamesTable>;
  return OwnedTable(SerializedDeclMemberNamesTable::Create(base + tableOffset,
      base + sizeof(uint32_t), base));
}

std::unique_ptr<ModuleFileSharedCore::SerializedDeclMembersTable>
ModuleFileSharedCore::readDeclMembersTable(ArrayRef<uint64_t> fields,
                                     StringRef blobData) const {
  uint32_t tableOffset;
  decl_member_tables_block::DeclMembersLayout::readRecord(fields,
                                                          tableOffset);
  auto base = reinterpret_cast<const uint8_t *>(blobData.data());

  using OwnedTable = std::unique_ptr<SerializedDeclMembersTable>;
  return OwnedTable(SerializedDeclMembersTable::Create(base + tableOffset,
      base + sizeof(uint32_t), base));
}

std::unique_ptr<ModuleFileSharedCore::SerializedDeclFingerprintsTable>
ModuleFileSharedCore::readDeclFingerprintsTable(ArrayRef<uint64_t> fields,
                                                StringRef blobData) const {
  uint32_t tableOffset;
  index_block::DeclFingerprintsLayout::readRecord(fields, tableOffset);
  auto base = reinterpret_cast<const uint8_t *>(blobData.data());

  using OwnedTable = std::unique_ptr<SerializedDeclFingerprintsTable>;
  return OwnedTable(SerializedDeclFingerprintsTable::Create(
      base + tableOffset, base + sizeof(uint32_t), base));
}

std::unique_ptr<ModuleFileSharedCore::SerializedObjCMethodTable>
ModuleFileSharedCore::readObjCMethodTable(ArrayRef<uint64_t> fields,
                                    StringRef blobData) const {
  uint32_t tableOffset;
  index_block::ObjCMethodTableLayout::readRecord(fields, tableOffset);
  auto base = reinterpret_cast<const uint8_t *>(blobData.data());

  using OwnedTable = std::unique_ptr<SerializedObjCMethodTable>;
  return OwnedTable(
           SerializedObjCMethodTable::Create(base + tableOffset,
                                             base + sizeof(uint32_t), base));
}

std::unique_ptr<ModuleFileSharedCore::SerializedDerivativeFunctionConfigTable>
ModuleFileSharedCore::readDerivativeFunctionConfigTable(
    ArrayRef<uint64_t> fields, StringRef blobData) const {
  uint32_t tableOffset;
  index_block::DerivativeFunctionConfigTableLayout::readRecord(fields,
                                                               tableOffset);
  auto base = reinterpret_cast<const uint8_t *>(blobData.data());

  using OwnedTable = std::unique_ptr<SerializedDerivativeFunctionConfigTable>;
  return OwnedTable(SerializedDerivativeFunctionConfigTable::Create(
      base + tableOffset, base + sizeof(uint32_t), base));
}

bool ModuleFileSharedCore::readIndexBlock(llvm::BitstreamCursor &cursor) {
  if (llvm::Error Err = cursor.EnterSubBlock(INDEX_BLOCK_ID)) {
    // FIXME this drops the error on the floor.
    consumeError(std::move(Err));
    return false;
  }

  SmallVector<uint64_t, 4> scratch;
  StringRef blobData;

  while (!cursor.AtEndOfStream()) {
    Expected<llvm::BitstreamEntry> maybeEntry = cursor.advance();
    if (!maybeEntry) {
      // FIXME this drops the error on the floor.
      consumeError(maybeEntry.takeError());
      return false;
    }
    llvm::BitstreamEntry entry = maybeEntry.get();
    switch (entry.Kind) {
    case llvm::BitstreamEntry::EndBlock:
      return true;

    case llvm::BitstreamEntry::Error:
      return false;

    case llvm::BitstreamEntry::SubBlock:
      if (entry.ID == DECL_MEMBER_TABLES_BLOCK_ID) {
        DeclMemberTablesCursor = cursor;
        if (llvm::Error Err = DeclMemberTablesCursor.EnterSubBlock(
                DECL_MEMBER_TABLES_BLOCK_ID)) {
          // FIXME this drops the error on the floor.
          consumeError(std::move(Err));
          return false;
        }
        llvm::BitstreamEntry subentry;
        do {
          // Scan forward, to load the cursor with any abbrevs we'll need while
          // seeking inside this block later.
          Expected<llvm::BitstreamEntry> maybeEntry =
              DeclMemberTablesCursor.advance(
                  llvm::BitstreamCursor::AF_DontPopBlockAtEnd);
          if (!maybeEntry) {
            // FIXME this drops the error on the floor.
            consumeError(maybeEntry.takeError());
            return false;
          }
          subentry = maybeEntry.get();
        } while (!DeclMemberTablesCursor.AtEndOfStream() &&
                 subentry.Kind != llvm::BitstreamEntry::Record &&
                 subentry.Kind != llvm::BitstreamEntry::EndBlock);
      }
      if (cursor.SkipBlock())
        return false;
      break;

    case llvm::BitstreamEntry::Record:
      scratch.clear();
      blobData = {};
      Expected<unsigned> maybeKind =
          cursor.readRecord(entry.ID, scratch, &blobData);
      if (!maybeKind) {
        // FIXME this drops the error on the floor.
        consumeError(maybeKind.takeError());
        return false;
      }
      unsigned kind = maybeKind.get();

      switch (kind) {
      case index_block::DECL_OFFSETS:
        assert(blobData.empty());
        allocateBuffer(Decls, scratch);
        break;
      case index_block::TYPE_OFFSETS:
        assert(blobData.empty());
        allocateBuffer(Types, scratch);
        break;
      case index_block::CLANG_TYPE_OFFSETS:
        assert(blobData.empty());
        allocateBuffer(ClangTypes, scratch);
        break;
      case index_block::IDENTIFIER_OFFSETS:
        assert(blobData.empty());
        allocateBuffer(Identifiers, scratch);
        break;
      case index_block::TOP_LEVEL_DECLS:
        TopLevelDecls = readDeclTable(scratch, blobData);
        break;
      case index_block::OPERATORS:
        OperatorDecls = readDeclTable(scratch, blobData);
        break;
      case index_block::PRECEDENCE_GROUPS:
        PrecedenceGroupDecls = readDeclTable(scratch, blobData);
        break;
      case index_block::EXTENSIONS:
        ExtensionDecls = readExtensionTable(scratch, blobData);
        break;
      case index_block::CLASS_MEMBERS_FOR_DYNAMIC_LOOKUP:
        ClassMembersForDynamicLookup = readDeclTable(scratch, blobData);
        break;
      case index_block::OPERATOR_METHODS:
        OperatorMethodDecls = readDeclTable(scratch, blobData);
        break;
      case index_block::OBJC_METHODS:
        ObjCMethods = readObjCMethodTable(scratch, blobData);
        break;
      case index_block::DERIVATIVE_FUNCTION_CONFIGURATIONS:
        DerivativeFunctionConfigurations =
            readDerivativeFunctionConfigTable(scratch, blobData);
        break;
      case index_block::ENTRY_POINT:
        assert(blobData.empty());
        setEntryPointClassID(scratch.front());
        break;
      case index_block::ORDERED_TOP_LEVEL_DECLS:
        allocateBuffer(OrderedTopLevelDecls, scratch);
        break;
      case index_block::EXPORTED_PRESPECIALIZATION_DECLS:
        allocateBuffer(ExportedPrespecializationDecls, scratch);
        break;
      case index_block::LOCAL_TYPE_DECLS:
        LocalTypeDecls = readLocalDeclTable(scratch, blobData);
        break;
      case index_block::OPAQUE_RETURN_TYPE_DECLS:
        OpaqueReturnTypeDecls = readLocalDeclTable(scratch, blobData);
        break;
      case index_block::NESTED_TYPE_DECLS:
        NestedTypeDecls = readNestedTypeDeclsTable(scratch, blobData);
        break;
      case index_block::DECL_MEMBER_NAMES:
        DeclMemberNames = readDeclMemberNamesTable(scratch, blobData);
        break;
      case index_block::DECL_FINGERPRINTS:
        DeclFingerprints = readDeclFingerprintsTable(scratch, blobData);
        break;
      case index_block::LOCAL_DECL_CONTEXT_OFFSETS:
        assert(blobData.empty());
        allocateBuffer(LocalDeclContexts, scratch);
        break;
      case index_block::GENERIC_SIGNATURE_OFFSETS:
        assert(blobData.empty());
        allocateBuffer(GenericSignatures, scratch);
        break;
      case index_block::GENERIC_ENVIRONMENT_OFFSETS:
        assert(blobData.empty());
        allocateBuffer(GenericEnvironments, scratch);
        break;
      case index_block::SUBSTITUTION_MAP_OFFSETS:
        assert(blobData.empty());
        allocateBuffer(SubstitutionMaps, scratch);
        break;
      case index_block::PROTOCOL_CONFORMANCE_OFFSETS:
        assert(blobData.empty());
        allocateBuffer(Conformances, scratch);
        break;
      case index_block::PACK_CONFORMANCE_OFFSETS:
        assert(blobData.empty());
        allocateBuffer(PackConformances, scratch);
        break;
      case index_block::SIL_LAYOUT_OFFSETS:
        assert(blobData.empty());
        allocateBuffer(SILLayouts, scratch);
        break;

      default:
        // Unknown index kind, which this version of the compiler won't use.
        break;
      }
      break;
    }
  }

  return false;
}

std::unique_ptr<ModuleFileSharedCore::SerializedDeclCommentTable>
ModuleFileSharedCore::readDeclCommentTable(ArrayRef<uint64_t> fields,
                                 StringRef blobData) const {
  if (fields.empty() || blobData.empty())
    return nullptr;
  uint32_t tableOffset = static_cast<uint32_t>(fields.front());
  auto base = reinterpret_cast<const uint8_t *>(blobData.data());

  return std::unique_ptr<SerializedDeclCommentTable>(
    SerializedDeclCommentTable::Create(base + tableOffset,
                                       base + sizeof(uint32_t), base));
}

std::unique_ptr<ModuleFileSharedCore::GroupNameTable>
ModuleFileSharedCore::readGroupTable(ArrayRef<uint64_t> Fields,
                                     StringRef BlobData) const {
  auto pMap = std::make_unique<llvm::DenseMap<unsigned, StringRef>>();
  auto Data = reinterpret_cast<const uint8_t *>(BlobData.data());
  unsigned GroupCount = endian::readNext<uint32_t, little, unaligned>(Data);
  for (unsigned I = 0; I < GroupCount; ++I) {
    auto RawSize = endian::readNext<uint32_t, little, unaligned>(Data);
    auto RawText = StringRef(reinterpret_cast<const char *>(Data), RawSize);
    Data += RawSize;
    (*pMap)[I] = RawText;
  }
  return std::unique_ptr<ModuleFileSharedCore::GroupNameTable>(pMap.release());
}

bool ModuleFileSharedCore::readCommentBlock(llvm::BitstreamCursor &cursor) {
  if (llvm::Error Err = cursor.EnterSubBlock(COMMENT_BLOCK_ID)) {
    // FIXME this drops the error on the floor.
    consumeError(std::move(Err));
    return false;
  }

  SmallVector<uint64_t, 4> scratch;
  StringRef blobData;

  while (!cursor.AtEndOfStream()) {
    Expected<llvm::BitstreamEntry> maybeEntry = cursor.advance();
    if (!maybeEntry) {
      // FIXME this drops the error on the floor.
      consumeError(maybeEntry.takeError());
      return false;
    }
    llvm::BitstreamEntry entry = maybeEntry.get();
    switch (entry.Kind) {
    case llvm::BitstreamEntry::EndBlock:
      return true;

    case llvm::BitstreamEntry::Error:
      return false;

    case llvm::BitstreamEntry::SubBlock:
      // Unknown sub-block, which this version of the compiler won't use.
      if (cursor.SkipBlock())
        return false;
      break;

    case llvm::BitstreamEntry::Record:
      scratch.clear();
      Expected<unsigned> maybeKind =
          cursor.readRecord(entry.ID, scratch, &blobData);
      if (!maybeKind) {
        // FIXME this drops the error on the floor.
        consumeError(maybeKind.takeError());
        return false;
      }
      unsigned kind = maybeKind.get();

      switch (kind) {
      case comment_block::DECL_COMMENTS:
        DeclCommentTable = readDeclCommentTable(scratch, blobData);
        break;
      case comment_block::GROUP_NAMES:
        GroupNamesMap = readGroupTable(scratch, blobData);
        break;
      default:
        // Unknown index kind, which this version of the compiler won't use.
        break;
      }
      break;
    }
  }

  return false;
}

static llvm::Optional<swift::LibraryKind>
getActualLibraryKind(unsigned rawKind) {
  auto stableKind = static_cast<serialization::LibraryKind>(rawKind);
  if (stableKind != rawKind)
    return llvm::None;

  switch (stableKind) {
  case serialization::LibraryKind::Library:
    return swift::LibraryKind::Library;
  case serialization::LibraryKind::Framework:
    return swift::LibraryKind::Framework;
  }

  // If there's a new case value in the module file, ignore it.
  return llvm::None;
}

static llvm::Optional<ModuleDecl::ImportFilterKind>
getActualImportControl(unsigned rawValue) {
  // We switch on the raw value rather than the enum in order to handle future
  // values.
  switch (rawValue) {
  case static_cast<unsigned>(serialization::ImportControl::Normal):
    return ModuleDecl::ImportFilterKind::Default;
  case static_cast<unsigned>(serialization::ImportControl::Exported):
    return ModuleDecl::ImportFilterKind::Exported;
  case static_cast<unsigned>(serialization::ImportControl::ImplementationOnly):
    return ModuleDecl::ImportFilterKind::ImplementationOnly;
  case static_cast<unsigned>(serialization::ImportControl::InternalOrBelow):
    return ModuleDecl::ImportFilterKind::InternalOrBelow;
  case static_cast<unsigned>(serialization::ImportControl::PackageOnly):
    return ModuleDecl::ImportFilterKind::PackageOnly;
  default:
    return llvm::None;
  }
}

bool ModuleFileSharedCore::readModuleDocIfPresent(PathObfuscator &pathRecoverer) {
  if (!this->ModuleDocInputBuffer)
    return true;

  llvm::BitstreamCursor docCursor{ModuleDocInputBuffer->getMemBufferRef()};
  if (!checkModuleSignature(docCursor, SWIFTDOC_SIGNATURE) ||
      !enterTopLevelModuleBlock(docCursor, MODULE_DOC_BLOCK_ID)) {
    return false;
  }

  SmallVector<uint64_t, 64> scratch;
  llvm::BitstreamEntry topLevelEntry;

  bool hasValidControlBlock = false;
  ValidationInfo info;

  while (!docCursor.AtEndOfStream()) {
    Expected<llvm::BitstreamEntry> maybeEntry =
        docCursor.advance(AF_DontPopBlockAtEnd);
    if (!maybeEntry) {
      // FIXME this drops the error on the floor.
      consumeError(maybeEntry.takeError());
      return false;
    }
    topLevelEntry = maybeEntry.get();
    if (topLevelEntry.Kind != llvm::BitstreamEntry::SubBlock)
      break;

    switch (topLevelEntry.ID) {
    case CONTROL_BLOCK_ID: {
      if (llvm::Error Err = docCursor.EnterSubBlock(CONTROL_BLOCK_ID)) {
        // FIXME this drops the error on the floor.
        consumeError(std::move(Err));
        return false;
      }

      info = validateControlBlock(
          docCursor, scratch, {SWIFTDOC_VERSION_MAJOR, SWIFTDOC_VERSION_MINOR},
          RequiresOSSAModules, /*requiresRevisionMatch*/false,
          /*requiredSDK*/StringRef(), /*extendedInfo*/nullptr, pathRecoverer);
      if (info.status != Status::Valid)
        return false;
      // Check that the swiftdoc is actually for this module.
      if (info.name != Name)
        return false;
      hasValidControlBlock = true;
      break;
    }

    case COMMENT_BLOCK_ID: {
      if (!hasValidControlBlock || !readCommentBlock(docCursor))
        return false;
      break;
    }

    default:
      // Unknown top-level block, possibly for use by a future version of the
      // module format.
      if (docCursor.SkipBlock())
        return false;
      break;
    }
  }

  if (topLevelEntry.Kind != llvm::BitstreamEntry::EndBlock)
    return false;

  return true;
}

std::unique_ptr<ModuleFileSharedCore::SerializedDeclUSRTable>
ModuleFileSharedCore::readDeclUSRsTable(ArrayRef<uint64_t> fields,
                                  StringRef blobData) const {
  if (fields.empty() || blobData.empty())
    return nullptr;
  uint32_t tableOffset = static_cast<uint32_t>(fields.front());
  auto base = reinterpret_cast<const uint8_t *>(blobData.data());
  return std::unique_ptr<SerializedDeclUSRTable>(
    SerializedDeclUSRTable::Create(base + tableOffset, base + sizeof(uint32_t),
                                   base));
}

bool ModuleFileSharedCore::readDeclLocsBlock(llvm::BitstreamCursor &cursor) {
  if (llvm::Error Err = cursor.EnterSubBlock(CONTROL_BLOCK_ID)) {
    consumeError(std::move(Err));
    return false;
  }

  SmallVector<uint64_t, 4> scratch;
  StringRef blobData;

  while (!cursor.AtEndOfStream()) {
    Expected<llvm::BitstreamEntry> entry = cursor.advance();
    if (!entry) {
      consumeError(entry.takeError());
      return false;
    }
    switch (entry->Kind) {
    case llvm::BitstreamEntry::EndBlock:
      return true;

    case llvm::BitstreamEntry::Error:
      return false;

    case llvm::BitstreamEntry::SubBlock:
      // Unknown sub-block, which this version of the compiler won't use.
      if (cursor.SkipBlock())
        return false;
      break;

    case llvm::BitstreamEntry::Record:
      scratch.clear();
      Expected<unsigned> kind =
          cursor.readRecord(entry->ID, scratch, &blobData);
      if (!kind) {
        consumeError(kind.takeError());
        return false;
      }
      switch (*kind) {
      case decl_locs_block::SOURCE_FILE_LIST:
        SourceFileListData = blobData;
        break;
      case decl_locs_block::BASIC_DECL_LOCS:
        BasicDeclLocsData = blobData;
        break;
      case decl_locs_block::TEXT_DATA:
        SourceLocsTextData = blobData;
        break;
      case decl_locs_block::DECL_USRS:
        DeclUSRsTable = readDeclUSRsTable(scratch, blobData);
        break;
      case decl_locs_block::DOC_RANGES:
        DocRangesData = blobData;
        break;
      default:
        // Unknown index kind, which this version of the compiler won't use.
        break;
      }
      break;
    }
  }

  return false;
}

bool ModuleFileSharedCore::readModuleSourceInfoIfPresent(PathObfuscator &pathRecoverer) {
  if (!this->ModuleSourceInfoInputBuffer)
    return true;

  llvm::BitstreamCursor infoCursor{
      ModuleSourceInfoInputBuffer->getMemBufferRef()};
  if (!checkModuleSignature(infoCursor, SWIFTSOURCEINFO_SIGNATURE) ||
      !enterTopLevelModuleBlock(infoCursor, MODULE_SOURCEINFO_BLOCK_ID)) {
    return false;
  }

  SmallVector<uint64_t, 64> scratch;

  bool hasValidControlBlock = false;
  ValidationInfo info;
  unsigned kind = llvm::BitstreamEntry::Error;

  while (!infoCursor.AtEndOfStream()) {
    Expected<llvm::BitstreamEntry> topLevelEntry =
        infoCursor.advance(AF_DontPopBlockAtEnd);
    if (!topLevelEntry) {
      consumeError(topLevelEntry.takeError());
      return false;
    }
    kind = topLevelEntry->Kind;
    if (kind != llvm::BitstreamEntry::SubBlock)
      break;

    switch (topLevelEntry->ID) {
    case CONTROL_BLOCK_ID: {
      if (llvm::Error Err = infoCursor.EnterSubBlock(CONTROL_BLOCK_ID)) {
        consumeError(std::move(Err));
        return false;
      }
      info = validateControlBlock(
          infoCursor, scratch,
          {SWIFTSOURCEINFO_VERSION_MAJOR, SWIFTSOURCEINFO_VERSION_MINOR},
          RequiresOSSAModules, /*requiresRevisionMatch*/false,
          /*requiredSDK*/StringRef(), /*extendedInfo*/nullptr, pathRecoverer);
      if (info.status != Status::Valid)
        return false;
      // Check that the swiftsourceinfo is actually for this module.
      if (info.name != Name)
        return false;
      hasValidControlBlock = true;
      break;
    }

    case DECL_LOCS_BLOCK_ID: {
      if (!hasValidControlBlock || !readDeclLocsBlock(infoCursor))
        return false;
      break;
    }

    default:
      // Unknown top-level block, possibly for use by a future version of the
      // module format.
      if (infoCursor.SkipBlock())
        return false;
      break;
    }
  }

  if (kind != llvm::BitstreamEntry::EndBlock)
    return false;

  return true;
}

StringRef ModuleFileSharedCore::getModuleNameFromID(ModuleID MID) const {
  if (MID < NUM_SPECIAL_IDS) {
    switch (static_cast<SpecialIdentifierID>(static_cast<uint8_t>(MID))) {
    case BUILTIN_MODULE_ID:
      return BUILTIN_NAME;
    case CURRENT_MODULE_ID:
      return Name;
    case OBJC_HEADER_MODULE_ID:
      return CLANG_HEADER_MODULE_NAME;
    case SUBSCRIPT_ID:
    case CONSTRUCTOR_ID:
    case DESTRUCTOR_ID:
      llvm_unreachable("Modules cannot be named with special names");
    case NUM_SPECIAL_IDS:
      llvm_unreachable("implementation detail only");
    }
  }
  return getIdentifierText(MID);
}

StringRef ModuleFileSharedCore::getIdentifierText(IdentifierID IID) const {
  if (IID == 0)
    return StringRef();

  assert(IID >= NUM_SPECIAL_IDS);

  size_t rawID = IID - NUM_SPECIAL_IDS;
  assert(rawID < Identifiers.size() && "invalid identifier ID");
  auto offset = Identifiers[rawID];

  assert(!IdentifierData.empty() && "no identifier data in module");

  StringRef rawStrPtr = IdentifierData.substr(offset);
  size_t terminatorOffset = rawStrPtr.find('\0');
  assert(terminatorOffset != StringRef::npos &&
         "unterminated identifier string data");
  return rawStrPtr.slice(0, terminatorOffset);
}

ModuleFileSharedCore::ModuleFileSharedCore(
    std::unique_ptr<llvm::MemoryBuffer> moduleInputBuffer,
    std::unique_ptr<llvm::MemoryBuffer> moduleDocInputBuffer,
    std::unique_ptr<llvm::MemoryBuffer> moduleSourceInfoInputBuffer,
    bool isFramework, bool requiresOSSAModules, StringRef requiredSDK,
    serialization::ValidationInfo &info, PathObfuscator &pathRecoverer)
    : ModuleInputBuffer(std::move(moduleInputBuffer)),
      ModuleDocInputBuffer(std::move(moduleDocInputBuffer)),
      ModuleSourceInfoInputBuffer(std::move(moduleSourceInfoInputBuffer)),
      RequiresOSSAModules(requiresOSSAModules) {
  assert(!hasError());
  Bits.IsFramework = isFramework;

  PrettyStackTraceModuleFileCore stackEntry(*this);

  llvm::BitstreamCursor cursor{ModuleInputBuffer->getMemBufferRef()};

  if (!checkModuleSignature(cursor, SWIFTMODULE_SIGNATURE) ||
      !enterTopLevelModuleBlock(cursor, MODULE_BLOCK_ID)) {
    info.status = error(Status::Malformed);
    return;
  }

  // Future-proofing: make sure we validate the control block before we try to
  // read any other blocks.
  bool hasValidControlBlock = false;
  SmallVector<uint64_t, 64> scratch;

  llvm::BitstreamEntry topLevelEntry;

  while (!cursor.AtEndOfStream()) {
    Expected<llvm::BitstreamEntry> maybeEntry =
        cursor.advance(AF_DontPopBlockAtEnd);
    if (!maybeEntry) {
      // FIXME this drops the error diagnostic on the floor.
      consumeError(maybeEntry.takeError());
      info.status = error(Status::Malformed);
      return;
    }
    topLevelEntry = maybeEntry.get();
    if (topLevelEntry.Kind != llvm::BitstreamEntry::SubBlock)
      break;

    switch (topLevelEntry.ID) {
    case CONTROL_BLOCK_ID: {
      if (llvm::Error Err = cursor.EnterSubBlock(CONTROL_BLOCK_ID)) {
        // FIXME this drops the error on the floor.
        consumeError(std::move(Err));
        info.status = error(Status::Malformed);
        return;
      }

      ExtendedValidationInfo extInfo;
      info = validateControlBlock(
          cursor, scratch,
          {SWIFTMODULE_VERSION_MAJOR, SWIFTMODULE_VERSION_MINOR},
          RequiresOSSAModules, /*requiresRevisionMatch=*/true, requiredSDK,
          &extInfo, pathRecoverer);
      if (info.status != Status::Valid) {
        error(info.status);
        return;
      }
      Name = info.name;
      TargetTriple = info.targetTriple;
      SDKName = info.sdkName;
      CompatibilityVersion = info.compatibilityVersion;
      UserModuleVersion = info.userModuleVersion;
      AllowableClientNames = info.allowableClients;
      Bits.ArePrivateImportsEnabled = extInfo.arePrivateImportsEnabled();
      Bits.IsSIB = extInfo.isSIB();
      Bits.IsStaticLibrary = extInfo.isStaticLibrary();
      Bits.HasHermeticSealAtLink = extInfo.hasHermeticSealAtLink();
      Bits.IsTestable = extInfo.isTestable();
      Bits.ResilienceStrategy = unsigned(extInfo.getResilienceStrategy());
      Bits.IsImplicitDynamicEnabled = extInfo.isImplicitDynamicEnabled();
      Bits.IsBuiltFromInterface = extInfo.isBuiltFromInterface();
      Bits.IsAllowModuleWithCompilerErrorsEnabled =
          extInfo.isAllowModuleWithCompilerErrorsEnabled();
      Bits.IsConcurrencyChecked = extInfo.isConcurrencyChecked();
      Bits.HasCxxInteroperability = extInfo.hasCxxInteroperability();
      MiscVersion = info.miscVersion;
      ModuleABIName = extInfo.getModuleABIName();
      ModulePackageName = extInfo.getModulePackageName();
      ModuleExportAsName = extInfo.getExportAsName();

      hasValidControlBlock = true;
      break;
    }

    case INPUT_BLOCK_ID: {
      if (!hasValidControlBlock) {
        info.status = error(Status::Malformed);
        return;
      }

      if (llvm::Error Err = cursor.EnterSubBlock(INPUT_BLOCK_ID)) {
        // FIXME this drops the error on the floor.
        consumeError(std::move(Err));
        info.status = error(Status::Malformed);
        return;
      }

      Expected<llvm::BitstreamEntry> maybeNext = cursor.advance();
      if (!maybeNext) {
        // FIXME this drops the error on the floor.
        consumeError(maybeNext.takeError());
        info.status = error(Status::Malformed);
        return;
      }
      llvm::BitstreamEntry next = maybeNext.get();
      while (next.Kind == llvm::BitstreamEntry::Record) {
        scratch.clear();
        StringRef blobData;
        Expected<unsigned> maybeKind =
            cursor.readRecord(next.ID, scratch, &blobData);
        if (!maybeKind) {
          // FIXME this drops the error on the floor.
          consumeError(maybeKind.takeError());
          info.status = error(Status::Malformed);
          return;
        }
        unsigned kind = maybeKind.get();
        switch (kind) {
        case input_block::IMPORTED_MODULE: {
          unsigned rawImportControl;
          bool scoped;
          bool hasSPI;
          input_block::ImportedModuleLayout::readRecord(scratch,
                                                        rawImportControl,
                                                        scoped, hasSPI);
          auto importKind = getActualImportControl(rawImportControl);
          if (!importKind) {
            // We don't know how to import this dependency.
            info.status = error(Status::Malformed);
            return;
          }

          StringRef spiBlob;
          if (hasSPI) {
            scratch.clear();

            llvm::BitstreamEntry entry =
                fatalIfUnexpected(cursor.advance(AF_DontPopBlockAtEnd));
            unsigned recordID = fatalIfUnexpected(
                cursor.readRecord(entry.ID, scratch, &spiBlob));
            assert(recordID == input_block::IMPORTED_MODULE_SPIS);
            input_block::ImportedModuleLayoutSPI::readRecord(scratch);
            (void) recordID;
          } else {
            spiBlob = StringRef();
          }

          Dependencies.push_back(
              {blobData, spiBlob, importKind.value(), scoped});
          break;
        }
        case input_block::LINK_LIBRARY: {
          uint8_t rawKind;
          bool shouldForceLink;
          input_block::LinkLibraryLayout::readRecord(scratch, rawKind,
                                                     shouldForceLink);
          if (Bits.IsStaticLibrary)
            shouldForceLink = false;
          if (auto libKind = getActualLibraryKind(rawKind))
            LinkLibraries.push_back({blobData, *libKind, shouldForceLink});
          // else ignore the dependency...it'll show up as a linker error.
          break;
        }
        case input_block::IMPORTED_HEADER: {
          assert(!importedHeaderInfo.fileSize && "only one header allowed");
          bool exported;
          input_block::ImportedHeaderLayout::readRecord(scratch,
            exported, importedHeaderInfo.fileSize,
            importedHeaderInfo.fileModTime);
          Dependencies.push_back(Dependency::forHeader(blobData, exported));
          break;
        }
        case input_block::IMPORTED_HEADER_CONTENTS: {
          assert(Dependencies.back().isHeader() && "must follow header record");
          assert(importedHeaderInfo.contents.empty() &&
                 "contents seen already");
          importedHeaderInfo.contents = blobData;
          break;
        }
        case input_block::SEARCH_PATH: {
          bool isFramework;
          bool isSystem;
          input_block::SearchPathLayout::readRecord(scratch, isFramework,
                                                    isSystem);
          SearchPaths.push_back({pathRecoverer.recover(blobData), isFramework,
            isSystem});
          break;
        }
        case input_block::MODULE_INTERFACE_PATH: {
          ModuleInterfacePath = blobData;
          break;
        }
        default:
          // Unknown input kind, possibly for use by a future version of the
          // module format.
          // FIXME: Should we warn about this?
          break;
        }

        maybeNext = cursor.advance();
        if (!maybeNext) {
          // FIXME this drops the error on the floor.
          consumeError(maybeNext.takeError());
          info.status = error(Status::Malformed);
          return;
        }
        next = maybeNext.get();
      }

      if (next.Kind != llvm::BitstreamEntry::EndBlock)
        info.status = error(Status::Malformed);

      break;
    }

    case DECLS_AND_TYPES_BLOCK_ID: {
      if (!hasValidControlBlock) {
        info.status = error(Status::Malformed);
        return;
      }

      // The decls-and-types block is lazily loaded. Save the cursor and load
      // any abbrev records at the start of the block.
      DeclTypeCursor = cursor;
      if (llvm::Error Err =
              DeclTypeCursor.EnterSubBlock(DECLS_AND_TYPES_BLOCK_ID)) {
        // FIXME this drops the error on the floor.
        consumeError(std::move(Err));
        info.status = error(Status::Malformed);
        return;
      }

      Expected<llvm::BitstreamEntry> maybeCursor = DeclTypeCursor.advance();
      if (!maybeCursor) {
        // FIXME this drops the error on the floor.
        consumeError(maybeCursor.takeError());
        info.status = error(Status::Malformed);
        return;
      }
      if (maybeCursor.get().Kind == llvm::BitstreamEntry::Error)
        info.status = error(Status::Malformed);

      // With the main cursor, skip over the block and continue.
      if (cursor.SkipBlock()) {
        info.status = error(Status::Malformed);
        return;
      }
      break;
    }

    case IDENTIFIER_DATA_BLOCK_ID: {
      if (!hasValidControlBlock) {
        info.status = error(Status::Malformed);
        return;
      }

      if (llvm::Error Err = cursor.EnterSubBlock(IDENTIFIER_DATA_BLOCK_ID)) {
        // FIXME this drops the error on the floor.
        consumeError(std::move(Err));
        info.status = error(Status::Malformed);
        return;
      }

      Expected<llvm::BitstreamEntry> maybeNext =
          cursor.advanceSkippingSubblocks();
      if (!maybeNext) {
        // FIXME this drops the error on the floor.
        consumeError(maybeNext.takeError());
        info.status = error(Status::Malformed);
        return;
      }
      llvm::BitstreamEntry next = maybeNext.get();
      while (next.Kind == llvm::BitstreamEntry::Record) {
        scratch.clear();
        StringRef blobData;
        Expected<unsigned> maybeKind =
            cursor.readRecord(next.ID, scratch, &blobData);
        if (!maybeKind) {
          // FIXME this drops the error on the floor.
          consumeError(maybeKind.takeError());
          info.status = error(Status::Malformed);
          return;
        }
        unsigned kind = maybeKind.get();

        switch (kind) {
        case identifier_block::IDENTIFIER_DATA:
          assert(scratch.empty());
          IdentifierData = blobData;
          break;
        default:
          // Unknown identifier data, which this version of the compiler won't
          // use.
          break;
        }

        maybeNext = cursor.advanceSkippingSubblocks();
        if (!maybeNext) {
          // FIXME this drops the error on the floor.
          consumeError(maybeNext.takeError());
          info.status = error(Status::Malformed);
          return;
        }
        next = maybeNext.get();
      }

      if (next.Kind != llvm::BitstreamEntry::EndBlock) {
        info.status = error(Status::Malformed);
        return;
      }

      break;
    }

    case INDEX_BLOCK_ID: {
      if (!hasValidControlBlock || !readIndexBlock(cursor)) {
        info.status = error(Status::Malformed);
        return;
      }
      break;
    }

    case SIL_INDEX_BLOCK_ID: {
      // Save the cursor.
      SILIndexCursor = cursor;
      if (llvm::Error Err = SILIndexCursor.EnterSubBlock(SIL_INDEX_BLOCK_ID)) {
        // FIXME this drops the error on the floor.
        consumeError(std::move(Err));
        info.status = error(Status::Malformed);
        return;
      }

      // With the main cursor, skip over the block and continue.
      if (cursor.SkipBlock()) {
        info.status = error(Status::Malformed);
        return;
      }
      break;
    }

    case SIL_BLOCK_ID: {
      // Save the cursor.
      SILCursor = cursor;
      if (llvm::Error Err = SILCursor.EnterSubBlock(SIL_BLOCK_ID)) {
        // FIXME this drops the error on the floor.
        consumeError(std::move(Err));
        info.status = error(Status::Malformed);
        return;
      }

      // With the main cursor, skip over the block and continue.
      if (cursor.SkipBlock()) {
        info.status = error(Status::Malformed);
        return;
      }
      break;
    }

    case INCREMENTAL_INFORMATION_BLOCK_ID: {
      HasIncrementalInfo = true;
      // Skip incremental info if present. The Frontend currently doesn't do
      // anything with this.
      if (cursor.SkipBlock()) {
        info.status = error(Status::Malformed);
        return;
      }
      break;
    }

    default:
      // Unknown top-level block, possibly for use by a future version of the
      // module format.
      if (cursor.SkipBlock()) {
        info.status = error(Status::Malformed);
        return;
      }
      break;
    }
  }

  if (topLevelEntry.Kind != llvm::BitstreamEntry::EndBlock) {
    info.status = error(Status::Malformed);
    return;
  }
  // Read source info file.
  readModuleSourceInfoIfPresent(pathRecoverer);
  if (!readModuleDocIfPresent(pathRecoverer)) {
    info.status = error(Status::MalformedDocumentation);
    return;
  }
}

bool ModuleFileSharedCore::hasSourceInfo() const {
  return !!DeclUSRsTable;
}

ModuleLoadingBehavior
ModuleFileSharedCore::getTransitiveLoadingBehavior(
                                          const Dependency &dependency,
                                          bool debuggerMode,
                                          bool isPartialModule,
                                          StringRef packageName,
                                          bool forTestable) const {
  if (isPartialModule) {
    // Keep the merge-module behavior for legacy support. In that case
    // we load all transitive dependencies from partial modules and
    // error if it fails.
    return ModuleLoadingBehavior::Required;
  }

  bool moduleIsResilient = getResilienceStrategy() ==
                             ResilienceStrategy::Resilient;
  if (dependency.isImplementationOnly()) {
    // Implementation-only dependencies are not usually loaded from
    // transitive imports.
    if (debuggerMode || forTestable) {
      // In the debugger, try to load the module if possible.
      // Same in the case of a testable import, try to load the dependency
      // but don't fail if it's missing as this could be source breaking.
      return ModuleLoadingBehavior::Optional;
    } else {
      // When building normally, ignore transitive implementation-only
      // imports.
      return ModuleLoadingBehavior::Ignored;
    }
  }

  if (dependency.isInternalOrBelow()) {
    // Non-public imports are similar to implementation-only, the module
    // loading behavior differs on loading those dependencies
    // on testable imports.
    if (forTestable || !moduleIsResilient) {
      return ModuleLoadingBehavior::Required;
    } else if (debuggerMode) {
      return ModuleLoadingBehavior::Optional;
    } else {
      return ModuleLoadingBehavior::Ignored;
    }
  }

  if (dependency.isPackageOnly()) {
    // Package dependencies are usually loaded only for import from the same
    // package.
    if ((!packageName.empty() && packageName == getModulePackageName()) ||
        forTestable ||
        !moduleIsResilient) {
      return ModuleLoadingBehavior::Required;
    } else if (debuggerMode) {
      return ModuleLoadingBehavior::Optional;
    } else {
      return ModuleLoadingBehavior::Ignored;
    }
  }

  // By default, imports are required dependencies.
  return ModuleLoadingBehavior::Required;
}

