//===--- FrontendInputsAndOutputs.cpp -------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/Frontend/FrontendInputsAndOutputs.h"

#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/FileTypes.h"
#include "swift/Basic/PrimarySpecificPaths.h"
#include "swift/Basic/Range.h"
#include "swift/Frontend/FrontendOptions.h"
#include "swift/Option/Options.h"
#include "swift/Parse/Lexer.h"
#include "swift/Strings.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/LineIterator.h"
#include "llvm/Support/Path.h"

using namespace swift;
using namespace llvm::opt;

// Constructors

FrontendInputsAndOutputs::FrontendInputsAndOutputs(
    const FrontendInputsAndOutputs &other) {
  for (InputFile input : other.AllInputs)
    addInput(input);
  IsSingleThreadedWMO = other.IsSingleThreadedWMO;
  ShouldRecoverMissingInputs = other.ShouldRecoverMissingInputs;
  PrincipalOutputType = other.PrincipalOutputType;
}

FrontendInputsAndOutputs &FrontendInputsAndOutputs::
operator=(const FrontendInputsAndOutputs &other) {
  clearInputs();
  for (InputFile input : other.AllInputs)
    addInput(input);
  IsSingleThreadedWMO = other.IsSingleThreadedWMO;
  ShouldRecoverMissingInputs = other.ShouldRecoverMissingInputs;
  PrincipalOutputType = other.PrincipalOutputType;
  return *this;
}

// All inputs:

std::vector<std::string> FrontendInputsAndOutputs::getInputFilenames() const {
  std::vector<std::string> filenames;
  for (auto &input : AllInputs) {
    filenames.push_back(input.getFileName());
  }
  return filenames;
}

bool FrontendInputsAndOutputs::isReadingFromStdin() const {
  return hasSingleInput() && getFilenameOfFirstInput() == "-";
}

const std::string &FrontendInputsAndOutputs::getFilenameOfFirstInput() const {
  assert(hasInputs());
  const InputFile &inp = AllInputs[0];
  const std::string &f = inp.getFileName();
  assert(!f.empty());
  return f;
}

bool FrontendInputsAndOutputs::forEachInput(
    llvm::function_ref<bool(const InputFile &)> fn) const {
  for (const InputFile &input : AllInputs)
    if (fn(input))
      return true;
  return false;
}

// Primaries:

const InputFile &FrontendInputsAndOutputs::firstPrimaryInput() const {
  assert(!PrimaryInputsInOrder.empty());
  return AllInputs[PrimaryInputsInOrder.front()];
}

const InputFile &FrontendInputsAndOutputs::lastPrimaryInput() const {
  assert(!PrimaryInputsInOrder.empty());
  return AllInputs[PrimaryInputsInOrder.back()];
}

bool FrontendInputsAndOutputs::forEachPrimaryInput(
    llvm::function_ref<bool(const InputFile &)> fn) const {
  for (unsigned i : PrimaryInputsInOrder)
    if (fn(AllInputs[i]))
      return true;
  return false;
}

bool FrontendInputsAndOutputs::forEachPrimaryInputWithIndex(
    llvm::function_ref<bool(const InputFile &, unsigned index)> fn) const {
  for (unsigned i : PrimaryInputsInOrder)
    if (fn(AllInputs[i], i))
      return true;
  return false;
}

bool FrontendInputsAndOutputs::forEachNonPrimaryInput(
    llvm::function_ref<bool(const InputFile &)> fn) const {
  return forEachInput([&](const InputFile &f) -> bool {
    return f.isPrimary() ? false : fn(f);
  });
}

void FrontendInputsAndOutputs::assertMustNotBeMoreThanOnePrimaryInput() const {
  assert(!hasMultiplePrimaryInputs() &&
         "have not implemented >1 primary input yet");
}

void FrontendInputsAndOutputs::
    assertMustNotBeMoreThanOnePrimaryInputUnlessBatchModeChecksHaveBeenBypassed()
        const {
  if (!areBatchModeChecksBypassed())
    assertMustNotBeMoreThanOnePrimaryInput();
}

const InputFile *FrontendInputsAndOutputs::getUniquePrimaryInput() const {
  assertMustNotBeMoreThanOnePrimaryInput();
  return PrimaryInputsInOrder.empty()
             ? nullptr
             : &AllInputs[PrimaryInputsInOrder.front()];
}

const InputFile &
FrontendInputsAndOutputs::getRequiredUniquePrimaryInput() const {
  if (const auto *input = getUniquePrimaryInput())
    return *input;
  llvm_unreachable("No primary when one is required");
}

std::string FrontendInputsAndOutputs::getStatsFileMangledInputName() const {
  // Use the first primary, even if there are multiple primaries.
  // That's enough to keep the file names unique.
  return isWholeModule() ? "all" : firstPrimaryInput().getFileName();
}

const InputFile &
FrontendInputsAndOutputs::getFirstOutputProducingInput() const {
  // Get the first input file that produces the output file. That is currently
  // used to compute with input should the cached diagnostics be associated
  // with. The first output producing input file is the first input if using
  // whole module, or first primary input if not using whole module.
  return isWholeModule() ? firstInput() : firstPrimaryInput();
}

unsigned FrontendInputsAndOutputs::getIndexOfFirstOutputProducingInput() const {
  return isWholeModule() ? 0 : PrimaryInputsInOrder[0];
}

bool FrontendInputsAndOutputs::isInputPrimary(StringRef file) const {
  return primaryInputNamed(file) != nullptr;
}

unsigned FrontendInputsAndOutputs::numberOfPrimaryInputsEndingWith(
    StringRef extension) const {
  unsigned n = 0;
  (void)forEachPrimaryInput([&](const InputFile &input) -> bool {
    if (llvm::sys::path::extension(input.getFileName()).ends_with(extension))
      ++n;
    return false;
  });
  return n;
}

// Input queries

bool FrontendInputsAndOutputs::shouldTreatAsLLVM() const {
  if (hasSingleInput()) {
    StringRef InputExt = llvm::sys::path::extension(getFilenameOfFirstInput());
    switch (file_types::lookupTypeForExtension(InputExt)) {
    case file_types::TY_LLVM_BC:
    case file_types::TY_LLVM_IR:
      return true;
    default:
      return false;
    }
  }
  return false;
}

bool FrontendInputsAndOutputs::shouldTreatAsModuleInterface() const {
  if (!hasSingleInput())
    return false;

  StringRef InputExt = llvm::sys::path::extension(getFilenameOfFirstInput());
  file_types::ID InputType = file_types::lookupTypeForExtension(InputExt);
  return InputType == file_types::TY_SwiftModuleInterfaceFile;
}

bool FrontendInputsAndOutputs::shouldTreatAsNonPackageModuleInterface() const {
  if (!hasSingleInput())
    return false;

  file_types::ID InputType =
      file_types::lookupTypeFromFilename(getFilenameOfFirstInput());
  return InputType == file_types::TY_SwiftModuleInterfaceFile ||
         InputType == file_types::TY_PrivateSwiftModuleInterfaceFile;
}

bool FrontendInputsAndOutputs::shouldTreatAsSIL() const {
  if (hasSingleInput()) {
    // If we have exactly one input filename, and its extension is "sil",
    // treat the input as SIL.
    StringRef extension = llvm::sys::path::extension(getFilenameOfFirstInput());
    return file_types::lookupTypeForExtension(extension) == file_types::TY_SIL;
  }
  // If we have one primary input and it's a filename with extension "sil",
  // treat the input as SIL.
  const unsigned silPrimaryCount = numberOfPrimaryInputsEndingWith(
      file_types::getExtension(file_types::TY_SIL));
  if (silPrimaryCount == 0)
    return false;
  if (silPrimaryCount == primaryInputCount()) {
    // Not clear what to do someday with multiple primaries
    assertMustNotBeMoreThanOnePrimaryInput();
    return true;
  }
  llvm_unreachable("Either all primaries or none must end with .sil");
}

bool FrontendInputsAndOutputs::shouldTreatAsObjCHeader() const {
  if (hasSingleInput()) {
    StringRef InputExt = llvm::sys::path::extension(getFilenameOfFirstInput());
    switch (file_types::lookupTypeForExtension(InputExt)) {
    case file_types::TY_ClangHeader:
      return true;
    default:
      return false;
    }
  }
  return false;
}

bool FrontendInputsAndOutputs::areAllNonPrimariesSIB() const {
  for (const InputFile &input : AllInputs) {
    if (input.isPrimary())
      continue;
    StringRef extension = llvm::sys::path::extension(input.getFileName());
    if (file_types::lookupTypeForExtension(extension) != file_types::TY_SIB) {
      return false;
    }
  }
  return true;
}

bool FrontendInputsAndOutputs::verifyInputs(DiagnosticEngine &diags,
                                            bool treatAsSIL,
                                            bool isREPLRequested,
                                            bool isNoneRequested) const {
  if (isREPLRequested) {
    if (hasInputs()) {
      diags.diagnose(SourceLoc(), diag::error_repl_requires_no_input_files);
      return true;
    }
  } else if (treatAsSIL) {
    if (isWholeModule()) {
      if (inputCount() != 1) {
        diags.diagnose(SourceLoc(), diag::error_mode_requires_one_input_file);
        return true;
      }
    } else {
      assertMustNotBeMoreThanOnePrimaryInput();
      // If we have the SIL as our primary input, we can waive the one file
      // requirement as long as all the other inputs are SIBs.
      if (!areAllNonPrimariesSIB()) {
        diags.diagnose(SourceLoc(),
                       diag::error_mode_requires_one_sil_multi_sib);
        return true;
      }
    }
  } else if (!isNoneRequested && !hasInputs()) {
    diags.diagnose(SourceLoc(), diag::error_mode_requires_an_input_file);
    return true;
  }
  return false;
}

// Changing inputs

void FrontendInputsAndOutputs::clearInputs() {
  AllInputs.clear();
  PrimaryInputsByName.clear();
  PrimaryInputsInOrder.clear();
}

void FrontendInputsAndOutputs::addInput(const InputFile &input) {
  const unsigned index = AllInputs.size();
  AllInputs.push_back(input);
  if (input.isPrimary()) {
    PrimaryInputsInOrder.push_back(index);
    PrimaryInputsByName.insert({AllInputs.back().getFileName(), index});
  }
}

void FrontendInputsAndOutputs::addInputFile(StringRef file,
                                            llvm::MemoryBuffer *buffer) {
  addInput(InputFile(file, false, buffer));
}

void FrontendInputsAndOutputs::addPrimaryInputFile(StringRef file,
                                                   llvm::MemoryBuffer *buffer) {
  addInput(InputFile(file, true, buffer));
}

// Outputs

unsigned FrontendInputsAndOutputs::countOfInputsProducingMainOutputs() const {
  return isSingleThreadedWMO()
             ? 1
             : hasPrimaryInputs() ? primaryInputCount() : inputCount();
}

const InputFile &FrontendInputsAndOutputs::firstInputProducingOutput() const {
  return isSingleThreadedWMO()
             ? firstInput()
             : hasPrimaryInputs() ? firstPrimaryInput() : firstInput();
}

const InputFile &FrontendInputsAndOutputs::lastInputProducingOutput() const {
  return isSingleThreadedWMO()
             ? firstInput()
             : hasPrimaryInputs() ? lastPrimaryInput() : lastInput();
}

bool FrontendInputsAndOutputs::forEachInputProducingAMainOutputFile(
    llvm::function_ref<bool(const InputFile &)> fn) const {
  return isSingleThreadedWMO()
             ? fn(firstInput())
             : hasPrimaryInputs() ? forEachPrimaryInput(fn) : forEachInput(fn);
}

void FrontendInputsAndOutputs::setMainAndSupplementaryOutputs(
    ArrayRef<std::string> outputFiles,
    ArrayRef<SupplementaryOutputPaths> supplementaryOutputs,
    ArrayRef<std::string> outputFilesForIndexUnits) {
  if (outputFilesForIndexUnits.empty())
    outputFilesForIndexUnits = outputFiles;

  assert(outputFiles.size() == outputFilesForIndexUnits.size() &&
         "Must have one index unit output path per main output");

  if (AllInputs.empty()) {
    assert(outputFiles.empty() && "Cannot have outputs without inputs");
    assert(supplementaryOutputs.empty() &&
           "Cannot have supplementary outputs without inputs");
    return;
  }
  if (hasPrimaryInputs()) {
    const auto N = primaryInputCount();
    assert(outputFiles.size() == N && "Must have one main output per primary");
    assert(supplementaryOutputs.size() == N &&
           "Must have one set of supplementary outputs per primary");
    (void)N;

    unsigned i = 0;
    for (auto &input : AllInputs) {
      if (input.isPrimary()) {
        input.setPrimarySpecificPaths(PrimarySpecificPaths(
            outputFiles[i], outputFilesForIndexUnits[i], input.getFileName(),
            supplementaryOutputs[i]));
        ++i;
      }
    }
    return;
  }
  assert(supplementaryOutputs.size() == 1 &&
         "WMO only ever produces one set of supplementary outputs");
  if (outputFiles.size() == 1) {
    AllInputs.front().setPrimarySpecificPaths(PrimarySpecificPaths(
        outputFiles.front(), outputFilesForIndexUnits.front(),
        firstInputProducingOutput().getFileName(),
        supplementaryOutputs.front()));
    return;
  }
  assert(outputFiles.size() == AllInputs.size() &&
         "Multi-threaded WMO requires one main output per input");
  for (auto i : indices(AllInputs))
    AllInputs[i].setPrimarySpecificPaths(PrimarySpecificPaths(
        outputFiles[i], outputFilesForIndexUnits[i], outputFiles[i],
        i == 0 ? supplementaryOutputs.front() : SupplementaryOutputPaths()));
}

std::vector<std::string> FrontendInputsAndOutputs::copyOutputFilenames() const {
  std::vector<std::string> outputs;
  (void)forEachInputProducingAMainOutputFile(
      [&](const InputFile &input) -> bool {
        outputs.push_back(input.outputFilename());
        return false;
      });
  return outputs;
}

std::vector<std::string>
FrontendInputsAndOutputs::copyIndexUnitOutputFilenames() const {
  std::vector<std::string> outputs;
  (void)forEachInputProducingAMainOutputFile(
      [&](const InputFile &input) -> bool {
        outputs.push_back(input.indexUnitOutputFilename());
        return false;
      });
  return outputs;
}

void FrontendInputsAndOutputs::forEachOutputFilename(
    llvm::function_ref<void(StringRef)> fn) const {
  (void)forEachInputProducingAMainOutputFile(
      [&](const InputFile &input) -> bool {
        fn(input.outputFilename());
        return false;
      });
}

std::string FrontendInputsAndOutputs::getSingleOutputFilename() const {
  assertMustNotBeMoreThanOnePrimaryInputUnlessBatchModeChecksHaveBeenBypassed();
  return hasInputs() ? lastInputProducingOutput().outputFilename()
                     : std::string();
}

std::string FrontendInputsAndOutputs::getSingleIndexUnitOutputFilename() const {
  assertMustNotBeMoreThanOnePrimaryInputUnlessBatchModeChecksHaveBeenBypassed();
  return hasInputs() ? lastInputProducingOutput().indexUnitOutputFilename()
                     : std::string();
}

bool FrontendInputsAndOutputs::isOutputFilenameStdout() const {
  return getSingleOutputFilename() == "-";
}

bool FrontendInputsAndOutputs::isOutputFileDirectory() const {
  return hasNamedOutputFile() &&
         llvm::sys::fs::is_directory(getSingleOutputFilename());
}

bool FrontendInputsAndOutputs::hasNamedOutputFile() const {
  return hasInputs() && !isOutputFilenameStdout();
}

// Supplementary outputs

unsigned
FrontendInputsAndOutputs::countOfFilesProducingSupplementaryOutput() const {
  return hasPrimaryInputs() ? primaryInputCount() : hasInputs() ? 1 : 0;
}

bool FrontendInputsAndOutputs::forEachInputProducingSupplementaryOutput(
    llvm::function_ref<bool(const InputFile &)> fn) const {
  return hasPrimaryInputs() ? forEachPrimaryInput(fn)
                            : hasInputs() ? fn(firstInput()) : false;
}

bool FrontendInputsAndOutputs::hasSupplementaryOutputPath(
    llvm::function_ref<const std::string &(const SupplementaryOutputPaths &)>
        extractorFn) const {
  return forEachInputProducingSupplementaryOutput([&](const InputFile &input)
                                                      -> bool {
    return !extractorFn(input.getPrimarySpecificPaths().SupplementaryOutputs)
                .empty();
  });
}

bool FrontendInputsAndOutputs::hasDependencyTrackerPath() const {
  return hasDependenciesFilePath() || hasReferenceDependenciesFilePath() ||
         hasLoadedModuleTracePath();
}

const PrimarySpecificPaths &
FrontendInputsAndOutputs::getPrimarySpecificPathsForAtMostOnePrimary() const {
  assertMustNotBeMoreThanOnePrimaryInputUnlessBatchModeChecksHaveBeenBypassed();
  static auto emptyPaths = PrimarySpecificPaths();
  return hasInputs() ? firstInputProducingOutput().getPrimarySpecificPaths()
                     : emptyPaths;
}

const PrimarySpecificPaths &
FrontendInputsAndOutputs::getPrimarySpecificPathsForPrimary(
    StringRef filename) const {
  const InputFile *f = primaryInputNamed(filename);
  return f->getPrimarySpecificPaths();
}

const InputFile *
FrontendInputsAndOutputs::primaryInputNamed(StringRef name) const {
  assert(!name.empty() && "input files have names");
  StringRef correctedFile =
      InputFile::convertBufferNameFromLLVM_getFileOrSTDIN_toSwiftConventions(
          name);
  auto iterator = PrimaryInputsByName.find(correctedFile);
  if (iterator == PrimaryInputsByName.end())
    return nullptr;
  const InputFile *f = &AllInputs[iterator->second];
  assert(f->isPrimary() && "PrimaryInputsByName should only include primaries");
  return f;
}
