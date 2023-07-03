//===--- ArgsToFrontendOutputsConverter.h -----------------------*- C++ -*-===//
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

#ifndef SWIFT_FRONTEND_ARGSTOFRONTENDOUTPUTSCONVERTER_H
#define SWIFT_FRONTEND_ARGSTOFRONTENDOUTPUTSCONVERTER_H

#include "swift/AST/DiagnosticConsumer.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/SupplementaryOutputPaths.h"
#include "swift/Frontend/FrontendOptions.h"
#include "swift/Option/Options.h"
#include "llvm/Option/ArgList.h"

#include <vector>

namespace swift {
class OutputFileMap;

/// Given the command line arguments and information about the inputs,
/// Fill in all the information in FrontendInputsAndOutputs.

class ArgsToFrontendOutputsConverter {
  const llvm::opt::ArgList &Args;
  StringRef ModuleName;
  FrontendInputsAndOutputs &InputsAndOutputs;
  DiagnosticEngine &Diags;

public:
  ArgsToFrontendOutputsConverter(const llvm::opt::ArgList &args,
                                 StringRef moduleName,
                                 FrontendInputsAndOutputs &inputsAndOutputs,
                                 DiagnosticEngine &diags)
      : Args(args), ModuleName(moduleName), InputsAndOutputs(inputsAndOutputs),
        Diags(diags) {}

  bool convert(std::vector<std::string> &mainOutputs,
               std::vector<std::string> &mainOutputsForIndexUnits,
               std::vector<SupplementaryOutputPaths> &supplementaryOutputs);

  /// Try to read an output file list file.
  /// \returns `None` if it could not open the filelist.
  static llvm::Optional<std::vector<std::string>>
  readOutputFileList(StringRef filelistPath, DiagnosticEngine &diags);
};

struct OutputOptInfo {
  StringRef PrettyName;
  options::ID SingleID;
  options::ID FilelistID;
  StringRef SingleOptSpelling;
};

class OutputFilesComputer {
  DiagnosticEngine &Diags;
  const FrontendInputsAndOutputs &InputsAndOutputs;
  const std::vector<std::string> OutputFileArguments;
  const std::string OutputDirectoryArgument;
  const std::string FirstInput;
  const FrontendOptions::ActionType RequestedAction;
  const llvm::opt::Arg *const ModuleNameArg;
  const StringRef Suffix;
  const bool HasTextualOutput;
  const OutputOptInfo OutputInfo;

  OutputFilesComputer(DiagnosticEngine &diags,
                      const FrontendInputsAndOutputs &inputsAndOutputs,
                      std::vector<std::string> outputFileArguments,
                      StringRef outputDirectoryArgument, StringRef firstInput,
                      FrontendOptions::ActionType requestedAction,
                      const llvm::opt::Arg *moduleNameArg, StringRef suffix,
                      bool hasTextualOutput,
                      OutputOptInfo optInfo);

public:
  static llvm::Optional<OutputFilesComputer>
  create(const llvm::opt::ArgList &args, DiagnosticEngine &diags,
         const FrontendInputsAndOutputs &inputsAndOutputs,
         OutputOptInfo optInfo);

  /// \return the output filenames on the command line or in the output
  /// filelist. If there
  /// were neither -o's nor an output filelist, returns an empty vector.
  static llvm::Optional<std::vector<std::string>>
  getOutputFilenamesFromCommandLineOrFilelist(const llvm::opt::ArgList &args,
                                              DiagnosticEngine &diags,
                                              options::ID singleOpt,
                                              options::ID filelistOpt);

  llvm::Optional<std::vector<std::string>> computeOutputFiles() const;

private:
  llvm::Optional<std::string> computeOutputFile(StringRef outputArg,
                                                const InputFile &input) const;

  /// \return the correct output filename when none was specified.
  ///
  /// Such an absence should only occur when invoking the frontend
  /// without the driver,
  /// because the driver will always pass -o with an appropriate filename
  /// if output is required for the requested action.
  llvm::Optional<std::string>
  deriveOutputFileFromInput(const InputFile &input) const;

  /// \return the correct output filename when a directory was specified.
  ///
  /// Such a specification should only occur when invoking the frontend
  /// directly, because the driver will always pass -o with an appropriate
  /// filename if output is required for the requested action.
  llvm::Optional<std::string>
  deriveOutputFileForDirectory(const InputFile &input) const;

  std::string determineBaseNameOfOutput(const InputFile &input) const;

  std::string deriveOutputFileFromParts(StringRef dir, StringRef base) const;
};

class SupplementaryOutputPathsComputer {
  const llvm::opt::ArgList &Args;
  DiagnosticEngine &Diags;
  const FrontendInputsAndOutputs &InputsAndOutputs;
  ArrayRef<std::string> OutputFiles;
  StringRef ModuleName;

  const FrontendOptions::ActionType RequestedAction;

public:
  SupplementaryOutputPathsComputer(
      const llvm::opt::ArgList &args, DiagnosticEngine &diags,
      const FrontendInputsAndOutputs &inputsAndOutputs,
      ArrayRef<std::string> outputFiles, StringRef moduleName);
  llvm::Optional<std::vector<SupplementaryOutputPaths>>
  computeOutputPaths() const;

private:
  /// \Return a set of supplementary output paths for each input that might
  /// produce supplementary outputs, or None to signal an error.
  /// \note
  /// Batch-mode supports multiple primary inputs.
  /// \par
  /// The paths are derived from arguments
  /// such as -emit-module-path. These are not the final computed paths,
  /// merely the ones passed in via the command line.
  /// \note
  /// In the future, these will also include those passed in via whatever
  /// filelist scheme gets implemented to handle cases where the command line
  /// arguments become burdensome.
  llvm::Optional<std::vector<SupplementaryOutputPaths>>
  getSupplementaryOutputPathsFromArguments() const;

  /// Read a supplementary output file map file.
  /// \returns `None` if it could not open the file map.
  llvm::Optional<std::vector<SupplementaryOutputPaths>>
  readSupplementaryOutputFileMap() const;

  /// Given an ID corresponding to supplementary output argument
  /// (e.g. -emit-module-path), collect all such paths, and ensure
  /// there are the right number of them.
  llvm::Optional<std::vector<std::string>>
  getSupplementaryFilenamesFromArguments(options::ID pathID) const;

  llvm::Optional<SupplementaryOutputPaths> computeOutputPathsForOneInput(
      StringRef outputFilename,
      const SupplementaryOutputPaths &pathsFromFilelists,
      const InputFile &) const;

  StringRef deriveDefaultSupplementaryOutputPathExcludingExtension(
      StringRef outputFilename, const InputFile &) const;

  /// \return empty string if no output file.
  std::string determineSupplementaryOutputFilename(
      options::ID emitOpt, std::string pathFromArgumentsOrFilelists,
      file_types::ID type, StringRef mainOutputIfUsable,
      StringRef defaultSupplementaryOutputPathExcludingExtension) const;

  void deriveModulePathParameters(StringRef mainOutputFile,
                                  options::ID &emitOption,
                                  std::string &extension,
                                  std::string &mainOutputIfUsable) const;
};

} // namespace swift

#endif /* SWIFT_FRONTEND_ARGSTOFRONTENDOUTPUTSCONVERTER_H */
