//===--- GenCoverage.cpp - IR Generation for coverage ---------------------===//
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
//  This file implements IR generation for the initialization of
//  coverage related variables.
//
//===----------------------------------------------------------------------===//

#include "IRGenModule.h"
#include "SwiftTargetInfo.h"

#include "swift/AST/IRGenOptions.h"
#include "swift/SIL/SILModule.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/ProfileData/Coverage/CoverageMappingWriter.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/Support/FileSystem.h"

using namespace swift;
using namespace irgen;

using llvm::coverage::CounterMappingRegion;
using llvm::coverage::CovMapVersion;

// This affects the coverage mapping format defined when `InstrProfData.inc`
// is textually included. Note that it means 'version >= 3', not 'version == 3'.
#define COVMAP_V3

/// This assert is here to make sure we make all the necessary code generation
/// changes that are needed to support the new coverage mapping format. Note we
/// cannot pin our version, as it must remain in sync with the version Clang is
/// using.
/// Do not bump without at least filing a bug and pinging a coverage maintainer.
static_assert(CovMapVersion::CurrentVersion == CovMapVersion::Version6,
              "Coverage mapping emission needs updating");

static std::string getInstrProfSection(IRGenModule &IGM,
                                       llvm::InstrProfSectKind SK) {
  return llvm::getInstrProfSectionName(SK, IGM.Triple.getObjectFormat());
}

void IRGenModule::emitCoverageMaps(ArrayRef<const SILCoverageMap *> Mappings) {
  // If there aren't any coverage maps, there's nothing to emit.
  if (Mappings.empty())
    return;

  SmallVector<llvm::Constant *, 4> UnusedFuncNames;
  for (const auto *Mapping : Mappings) {
    auto FuncName = Mapping->getPGOFuncName();
    auto VarLinkage = llvm::GlobalValue::LinkOnceAnyLinkage;
    auto FuncNameVarName = llvm::getPGOFuncNameVarName(FuncName, VarLinkage);

    // Check whether this coverage mapping can reference its name data within
    // the profile symbol table. If the name global isn't there, this function
    // has been optimized out. We need to tell LLVM about it by emitting the
    // name data separately.
    if (!Module.getNamedGlobal(FuncNameVarName)) {
      auto *Var = llvm::createPGOFuncNameVar(Module, VarLinkage, FuncName);
      UnusedFuncNames.push_back(llvm::ConstantExpr::getBitCast(Var, Int8PtrTy));
    }
  }

  // Emit the name data for any unused functions.
  if (!UnusedFuncNames.empty()) {
    auto NamePtrsTy = llvm::ArrayType::get(Int8PtrTy, UnusedFuncNames.size());
    auto NamePtrs = llvm::ConstantArray::get(NamePtrsTy, UnusedFuncNames);

    // Note we don't mark this variable as used, as it doesn't need to be
    // present in the object file, it gets picked up by the LLVM instrumentation
    // lowering pass.
    new llvm::GlobalVariable(Module, NamePtrsTy, /*IsConstant*/ true,
                             llvm::GlobalValue::InternalLinkage, NamePtrs,
                             llvm::getCoverageUnusedNamesVarName());
  }

  llvm::DenseMap<StringRef, unsigned> RawFileIndices;
  llvm::SmallVector<StringRef, 8> RawFiles;
  for (const auto &M : Mappings) {
    auto Filename = M->getFilename();
    auto Inserted = RawFileIndices.insert({Filename, RawFiles.size()}).second;
    if (!Inserted)
      continue;
    RawFiles.push_back(Filename);
  }
  const auto &Remapper = getOptions().CoveragePrefixMap;

  llvm::SmallVector<std::string, 8> FilenameStrs;
  FilenameStrs.reserve(RawFiles.size() + 1);

  // First element needs to be the current working directory. Note if this
  // scheme ever changes, the FileID computation below will need updating.
  SmallString<256> WorkingDirectory;
  llvm::sys::fs::current_path(WorkingDirectory);
  FilenameStrs.emplace_back(Remapper.remapPath(WorkingDirectory));

  // Following elements are the filenames present. We use their relative path,
  // which llvm-cov will turn back into absolute paths using the working
  // directory element.
  for (auto Name : RawFiles)
    FilenameStrs.emplace_back(Remapper.remapPath(Name));

  // Encode the filenames.
  std::string Filenames;
  llvm::LLVMContext &Ctx = getLLVMContext();
  {
    llvm::raw_string_ostream OS(Filenames);
    llvm::coverage::CoverageFilenamesSectionWriter(FilenameStrs).write(OS);
  }
  auto *FilenamesVal =
      llvm::ConstantDataArray::getString(Ctx, Filenames, false);
  const int64_t FilenamesRef = llvm::IndexedInstrProf::ComputeHash(Filenames);
  const size_t FilenamesSize = Filenames.size();

  // Emit the function records.
  auto *Int32Ty = llvm::Type::getInt32Ty(Ctx);
  for (const auto &M : Mappings) {
    StringRef NameValue = M->getPGOFuncName();
    assert(!NameValue.empty() && "Expected a named record");
    uint64_t FuncHash = M->getHash();

    const uint64_t NameHash = llvm::IndexedInstrProf::ComputeHash(NameValue);
    std::string FuncRecordName = "__covrec_" + llvm::utohexstr(NameHash);

    // The file ID needs to be bumped by 1 to account for the working directory
    // as the first element.
    unsigned FileID = [&]() {
      auto Result = RawFileIndices.find(M->getFilename());
      assert(Result != RawFileIndices.end());
      return Result->second + 1;
    }();
    assert(FileID < FilenameStrs.size());

    std::vector<CounterMappingRegion> Regions;
    for (const auto &MR : M->getMappedRegions()) {
      // The SubFileID here is 0, because it's an index into VirtualFileMapping,
      // and we only ever have a single file associated for a function.
      Regions.emplace_back(CounterMappingRegion::makeRegion(
          MR.Counter, /*SubFileID*/ 0, MR.StartLine, MR.StartCol, MR.EndLine,
          MR.EndCol));
    }
    // Append each function's regions into the encoded buffer.
    ArrayRef<unsigned> VirtualFileMapping(FileID);
    llvm::coverage::CoverageMappingWriter W(VirtualFileMapping,
                                            M->getExpressions(), Regions);
    std::string CoverageMapping;
    {
      llvm::raw_string_ostream OS(CoverageMapping);
      W.write(OS);
    }

#define COVMAP_FUNC_RECORD(Type, LLVMType, Name, Init) LLVMType,
    llvm::Type *FunctionRecordTypes[] = {
#include "llvm/ProfileData/InstrProfData.inc"
    };
    auto *FunctionRecordTy =
        llvm::StructType::get(Ctx, makeArrayRef(FunctionRecordTypes),
                              /*isPacked=*/true);

    // Create the function record constant.
#define COVMAP_FUNC_RECORD(Type, LLVMType, Name, Init) Init,
    llvm::Constant *FunctionRecordVals[] = {
#include "llvm/ProfileData/InstrProfData.inc"
    };
    auto *FuncRecordConstant = llvm::ConstantStruct::get(
        FunctionRecordTy, makeArrayRef(FunctionRecordVals));

    // Create the function record global.
    auto *FuncRecord = new llvm::GlobalVariable(
        *getModule(), FunctionRecordTy, /*isConstant=*/true,
        llvm::GlobalValue::LinkOnceODRLinkage, FuncRecordConstant,
        FuncRecordName);
    FuncRecord->setVisibility(llvm::GlobalValue::HiddenVisibility);
    FuncRecord->setSection(getInstrProfSection(*this, llvm::IPSK_covfun));
    FuncRecord->setAlignment(llvm::Align(8));
    if (Triple.supportsCOMDAT())
      FuncRecord->setComdat(getModule()->getOrInsertComdat(FuncRecordName));

    // Make sure the data doesn't get deleted.
    addUsedGlobal(FuncRecord);
  }

  // Create the coverage data header.
  const unsigned NRecords = 0;
  const unsigned CoverageMappingSize = 0;
  llvm::Type *CovDataHeaderTypes[] = {
#define COVMAP_HEADER(Type, LLVMType, Name, Init) LLVMType,
#include "llvm/ProfileData/InstrProfData.inc"
  };
  auto CovDataHeaderTy =
      llvm::StructType::get(Ctx, makeArrayRef(CovDataHeaderTypes));
  llvm::Constant *CovDataHeaderVals[] = {
#define COVMAP_HEADER(Type, LLVMType, Name, Init) Init,
#include "llvm/ProfileData/InstrProfData.inc"
  };
  auto CovDataHeaderVal = llvm::ConstantStruct::get(
      CovDataHeaderTy, makeArrayRef(CovDataHeaderVals));

  // Create the coverage data record
  llvm::Type *CovDataTypes[] = {CovDataHeaderTy, FilenamesVal->getType()};
  auto CovDataTy = llvm::StructType::get(Ctx, makeArrayRef(CovDataTypes));
  llvm::Constant *TUDataVals[] = {CovDataHeaderVal, FilenamesVal};
  auto CovDataVal =
      llvm::ConstantStruct::get(CovDataTy, makeArrayRef(TUDataVals));
  auto CovData = new llvm::GlobalVariable(
      *getModule(), CovDataTy, true, llvm::GlobalValue::PrivateLinkage,
      CovDataVal, llvm::getCoverageMappingVarName());

  CovData->setSection(getInstrProfSection(*this, llvm::IPSK_covmap));
  CovData->setAlignment(llvm::Align(8));
  addUsedGlobal(CovData);
}

void IRGenerator::emitCoverageMapping() {
  if (SIL.getCoverageMaps().empty())
    return;

  // Shard the coverage maps across their designated IRGenModules. This is
  // necessary to ensure we don't output N copies of a coverage map when doing
  // parallel IRGen, where N is the number of output object files.
  //
  // Note we don't just dump all the coverage maps into the primary IGM as
  // that would require creating unecessary name data entries, since the name
  // data is likely to already be present in the IGM that contains the entity
  // being profiled (unless it has been optimized out). Matching the coverage
  // map to its originating SourceFile also matches the behavior of a debug
  // build where the files are compiled separately.
  llvm::DenseMap<IRGenModule *, std::vector<const SILCoverageMap *>> MapsToEmit;
  for (const auto &M : SIL.getCoverageMaps()) {
    auto &Mapping = M.second;
    auto *SF = Mapping->getParentSourceFile();
    MapsToEmit[getGenModule(SF)].push_back(Mapping);
  }
  for (auto &IGMPair : *this) {
    auto *IGM = IGMPair.second;
    IGM->emitCoverageMaps(MapsToEmit[IGM]);
  }
}
