//===--- ArgsToFrontendInputsConverter.cpp --------------------------------===//
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

#include "ArgsToFrontendInputsConverter.h"

#include "ArgsToFrontendOutputsConverter.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/Defer.h"
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

ArgsToFrontendInputsConverter::ArgsToFrontendInputsConverter(
    DiagnosticEngine &diags, const ArgList &args)
    : Diags(diags), Args(args),
      FilelistPathArg(args.getLastArg(options::OPT_filelist)),
      PrimaryFilelistPathArg(args.getLastArg(options::OPT_primary_filelist)),
      BadFileDescriptorRetryCountArg(
        args.getLastArg(options::OPT_bad_file_descriptor_retry_count)) {}

std::optional<FrontendInputsAndOutputs> ArgsToFrontendInputsConverter::convert(
    SmallVectorImpl<std::unique_ptr<llvm::MemoryBuffer>> *buffers) {
  SWIFT_DEFER {
    if (buffers) {
      std::move(ConfigurationFileBuffers.begin(),
                ConfigurationFileBuffers.end(),
                std::back_inserter(*buffers));
      // Clearing the original list of buffers isn't strictly necessary, but
      // makes the behavior more sensible if we were to call convert() again.
      ConfigurationFileBuffers.clear();
    }
  };

  if (enforceFilelistExclusion())
    return std::nullopt;

  if (FilelistPathArg ? readInputFilesFromFilelist()
                      : readInputFilesFromCommandLine())
    return std::nullopt;
  std::optional<std::set<StringRef>> primaryFiles = readPrimaryFiles();
  if (!primaryFiles)
    return std::nullopt;

  FrontendInputsAndOutputs result;
  std::set<StringRef> unusedPrimaryFiles;
  std::tie(result, unusedPrimaryFiles) =
      createInputFilesConsumingPrimaries(*primaryFiles);

  if (diagnoseUnusedPrimaryFiles(unusedPrimaryFiles))
    return std::nullopt;

  // Must be set before iterating over inputs needing outputs.
  result.setBypassBatchModeChecks(
      Args.hasArg(options::OPT_bypass_batch_mode_checks));

  return std::move(result);
}

bool ArgsToFrontendInputsConverter::enforceFilelistExclusion() {
  if (Args.hasArg(options::OPT_INPUT) && FilelistPathArg) {
    Diags.diagnose(SourceLoc(),
                   diag::error_cannot_have_input_files_with_file_list);
    return true;
  }
  // The following is not strictly necessary, but the restriction makes
  // it easier to understand a given command line:
  if (Args.hasArg(options::OPT_primary_file) && PrimaryFilelistPathArg) {
    Diags.diagnose(
        SourceLoc(),
        diag::error_cannot_have_primary_files_with_primary_file_list);
    return true;
  }
  return false;
}

bool ArgsToFrontendInputsConverter::readInputFilesFromCommandLine() {
  bool hadDuplicates = false;
  for (const Arg *A :
       Args.filtered(options::OPT_INPUT, options::OPT_primary_file)) {
    hadDuplicates = addFile(A->getValue()) || hadDuplicates;
  }
  return false; // FIXME: Don't bail out for duplicates, too many tests depend
  // on it.
}

bool ArgsToFrontendInputsConverter::readInputFilesFromFilelist() {
  bool hadDuplicates = false;
  bool hadError =
      forAllFilesInFilelist(FilelistPathArg, [&](StringRef file) -> void {
        hadDuplicates = addFile(file) || hadDuplicates;
      });
  if (hadError)
    return true;
  return false; // FIXME: Don't bail out for duplicates, too many tests depend
                // on it.
}

bool ArgsToFrontendInputsConverter::forAllFilesInFilelist(
    Arg const *const pathArg, llvm::function_ref<void(StringRef)> fn) {
  if (!pathArg)
    return false;
  StringRef path = pathArg->getValue();

  // Honor -bad-file-descriptor-retry-count from the argument list
  unsigned RetryCount = 0;
  if (BadFileDescriptorRetryCountArg &&
      StringRef(BadFileDescriptorRetryCountArg->getValue())
        .getAsInteger(10,RetryCount)) {
    Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                   BadFileDescriptorRetryCountArg->getAsString(Args),
                   BadFileDescriptorRetryCountArg->getValue());
    return true;
  }

  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> filelistBufferOrError = nullptr;
  for (unsigned I = 0; I < RetryCount + 1; ++I) {
    filelistBufferOrError = llvm::MemoryBuffer::getFile(path);
    if (filelistBufferOrError)
      break;
    if (filelistBufferOrError.getError().value() != EBADF)
      break;
  }
  if (!filelistBufferOrError) {
    Diags.diagnose(SourceLoc(), diag::cannot_open_file, path,
                   filelistBufferOrError.getError().message());
    return true;
  }
  for (auto file :
       llvm::make_range(llvm::line_iterator(*filelistBufferOrError->get()),
                        llvm::line_iterator()))
    fn(file);
  ConfigurationFileBuffers.push_back(std::move(*filelistBufferOrError));
  return false;
}

bool ArgsToFrontendInputsConverter::addFile(StringRef file) {
  if (Files.insert(file))
    return false;
  Diags.diagnose(SourceLoc(), diag::error_duplicate_input_file, file);
  return true;
}

std::optional<std::set<StringRef>>
ArgsToFrontendInputsConverter::readPrimaryFiles() {
  std::set<StringRef> primaryFiles;
  for (const Arg *A : Args.filtered(options::OPT_primary_file))
    primaryFiles.insert(A->getValue());
  if (forAllFilesInFilelist(
          PrimaryFilelistPathArg,
          [&](StringRef file) -> void { primaryFiles.insert(file); }))
    return std::nullopt;
  return primaryFiles;
}

std::pair<FrontendInputsAndOutputs, std::set<StringRef>>
ArgsToFrontendInputsConverter::createInputFilesConsumingPrimaries(
    std::set<StringRef> primaryFiles) {
  bool hasAnyPrimaryFiles = !primaryFiles.empty();

  FrontendInputsAndOutputs result;
  for (auto &file : Files) {
    bool isPrimary = primaryFiles.count(file) > 0;
    result.addInput(InputFile(file, isPrimary));
    if (isPrimary)
      primaryFiles.erase(file);
  }

  if (!Files.empty() && !hasAnyPrimaryFiles) {
    std::optional<std::vector<std::string>> userSuppliedNamesOrErr =
        OutputFilesComputer::getOutputFilenamesFromCommandLineOrFilelist(
            Args, Diags, options::OPT_o, options::OPT_output_filelist);
    if (userSuppliedNamesOrErr && userSuppliedNamesOrErr->size() == 1)
      result.setIsSingleThreadedWMO(true);
  }

  return {std::move(result), std::move(primaryFiles)};
}

bool ArgsToFrontendInputsConverter::diagnoseUnusedPrimaryFiles(
    std::set<StringRef> primaryFiles) {
  for (auto &file : primaryFiles) {
    // Catch "swiftc -frontend -c -filelist foo -primary-file
    // some-file-not-in-foo".
    assert(FilelistPathArg && "Unused primary with no filelist");
    Diags.diagnose(SourceLoc(), diag::error_primary_file_not_found, file,
                   FilelistPathArg->getValue());
  }
  return !primaryFiles.empty();
}
