//===--- Job.cpp - Command to Execute -------------------------------------===//
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

#include "swift/Basic/STLExtras.h"
#include "swift/Driver/Job.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Option/Arg.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;
using namespace swift::driver;

void CommandOutput::addAdditionalOutputForType(types::ID type,
                                               StringRef OutputFilename) {
  // FIXME: dmu called multiple times for same type for BatchMode
  // xxx uses an additional lookup, should not
  if (AdditionalOutputsMap.count(type) == 0)
    AdditionalOutputsMap[type] = std::vector<std::string>();
  else
    assert(doesBatchModeProduceMultiples(type) &&
           "Even batch mode only supplies one of these types");

  AdditionalOutputsMap[type].push_back(OutputFilename);
}

ArrayRef<std::string>
CommandOutput::getAdditionalOutputsForType(types::ID type) const {
  auto iter = AdditionalOutputsMap.find(type);
  if (iter != AdditionalOutputsMap.end())
    return iter->second;

  static const std::vector<std::string> empty;
  return empty;
}

const std::string &CommandOutput::getAdditionalDependenciesOutput() const {
  auto deps = getAdditionalOutputsForType(types::TY_SwiftDeps);
  assert(deps.size() < 2 && "should never have >1 dependencies output");
  static const std::string empty;
  return deps.empty() ? empty : deps[0];
}
const std::string &
CommandOutput::getAdditionalSerializedDiagnosticsOutput() const {
  auto deps = getAdditionalOutputsForType(types::TY_SerializedDiagnostics);
  assert(deps.size() < 2 &&
         "should never have >1 serialized diagnostics output");
  static const std::string empty;
  return deps.empty() ? empty : deps[0];
}

const std::string &
CommandOutput::getAnyOutputForType(types::ID type) const {
  if (PrimaryOutputType == type)
    return PrimaryOutputFilenames[0];
  static const std::string empty;
  const auto &outs = getAdditionalOutputsForType(type);
  return outs.empty() ? empty : outs[0];
}

void CommandOutput::forEachOutputOfType(
    types::ID type, llvm::function_ref<void(const std::string &)> fn) const {
  if (PrimaryOutputType == type) {
    for (const auto &f : PrimaryOutputFilenames) {
      fn(f);
    }
  }
  for (const auto &f : getAdditionalOutputsForType(type)) {
    fn(f);
  }
}

bool CommandOutput::doesBatchModeProduceMultiples(types::ID type) {
  return type != types::TY_SwiftDeps && type != types::TY_SerializedDiagnostics;
}

static void escapeAndPrintString(llvm::raw_ostream &os, StringRef Str) {
  if (Str.empty()) {
    // Special-case the empty string.
    os << "\"\"";
    return;
  }

  bool NeedsEscape = Str.find_first_of(" \"\\$") != StringRef::npos;

  if (!NeedsEscape) {
    // This string doesn't have anything we need to escape, so print it directly
    os << Str;
    return;
  }

  // Quote and escape. This isn't really complete, but is good enough, and
  // matches how Clang's Command handles escaping arguments.
  os << '"';
  for (const char c : Str) {
    switch (c) {
    case '"':
    case '\\':
    case '$':
      // These characters need to be escaped.
      os << '\\';
      // Fall-through to the default case, since we still need to print the
      // character.
      LLVM_FALLTHROUGH;
    default:
      os << c;
    }
  }
  os << '"';
}

void Job::printArguments(raw_ostream &os,
                         const llvm::opt::ArgStringList &Args) {
  interleave(Args,
             [&](const char *Arg) { escapeAndPrintString(os, Arg); },
             [&] { os << ' '; });
}

void Job::dump() const {
  printCommandLineAndEnvironment(llvm::errs());
}

void Job::printCommandLineAndEnvironment(raw_ostream &Stream,
                                         StringRef Terminator) const {
  printCommandLine(Stream, /*Terminator=*/"");
  if (!ExtraEnvironment.empty()) {
    Stream << "  #";
    for (auto &pair : ExtraEnvironment) {
      Stream << " " << pair.first << "=" << pair.second;
    }
  }
  Stream << "\n";
}

void Job::printCommandLine(raw_ostream &os, StringRef Terminator) const {
  escapeAndPrintString(os, Executable);
  os << ' ';
  printArguments(os, Arguments);
  os << Terminator;
}

void Job::printSummary(raw_ostream &os) const {
  // Deciding how to describe our inputs is a bit subtle; if we are a Job built
  // from a JobAction that itself has InputActions sources, then we collect
  // those up. Otherwise it's more correct to talk about our inputs as the
  // outputs of our input-jobs.
  SmallVector<std::string, 4> Inputs;

  for (const Action *A : getSource().getInputs())
    if (const auto *IA = dyn_cast<InputAction>(A))
      Inputs.push_back(IA->getInputArg().getValue());

  for (const Job *J : getInputs())
    for (const std::string &f : J->getOutput().getPrimaryOutputFilenames())
      Inputs.push_back(f);

  size_t limit = 3;
  size_t actual = Inputs.size();
  if (actual > limit) {
    Inputs.erase(Inputs.begin() + limit, Inputs.end());
  }

  os << "{" << getSource().getClassName() << ": ";
  interleave(getOutput().getPrimaryOutputFilenames(),
             [&](const std::string &Arg) {
               os << llvm::sys::path::filename(Arg);
             },
             [&] { os << ' '; });
  os << " <= ";
  interleave(Inputs,
             [&](const std::string &Arg) {
               os << llvm::sys::path::filename(Arg);
             },
             [&] { os << ' '; });
  if (actual > limit) {
    os << " ... " << (actual-limit) << " more";
  }
  os << "}";
}
