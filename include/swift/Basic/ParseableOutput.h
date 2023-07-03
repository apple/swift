//===----- ParseableOutput.h - Helpers for parseable output ------- C++ -*-===//
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
/// Helpers for emitting the compiler's parseable output.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_PARSEABLEOUTPUT_H
#define SWIFT_PARSEABLEOUTPUT_H

#include "swift/Basic/LLVM.h"
#include "swift/Basic/TaskQueue.h"
#include "swift/Basic/FileTypes.h"

namespace swift {

namespace parseable_output {

/// Quasi-PIDs are _negative_ PID-like unique keys used to
/// masquerade batch job constituents as (quasi)processes, when writing
/// parseable output to consumers that don't understand the idea of a batch
/// job. They are negative in order to avoid possibly colliding with real
/// PIDs (which are always positive). We start at -1000 here as a crude but
/// harmless hedge against colliding with an errno value that might slip
/// into the stream of real PIDs (say, due to a TaskQueue bug).
const int QUASI_PID_START = -1000;

struct CommandInput {
  std::string Path;
  CommandInput() {}
  CommandInput(StringRef Path) : Path(Path) {}
};

using OutputPair = std::pair<file_types::ID, std::string>;

/// A client-agnostic (e.g. either the compiler driver or `swift-frontend`)
/// description of a task that is the subject of a parseable-output message.
struct DetailedTaskDescription {
  std::string Executable;
  SmallVector<std::string, 16> Arguments;
  std::string CommandLine;
  SmallVector<CommandInput, 4> Inputs;
  SmallVector<OutputPair, 8> Outputs;
};

/// Emits a "began" message to the given stream.
void emitBeganMessage(raw_ostream &os, StringRef Name,
                      DetailedTaskDescription TascDesc,
                      int64_t Pid, sys::TaskProcessInformation ProcInfo);

/// Emits a "finished" message to the given stream.
void emitFinishedMessage(raw_ostream &os, StringRef Name,
                         std::string Output, int ExitStatus,
                         int64_t Pid, sys::TaskProcessInformation ProcInfo);

/// Emits a "signalled" message to the given stream.
void emitSignalledMessage(raw_ostream &os, StringRef Name, StringRef ErrorMsg,
                          StringRef Output, llvm::Optional<int> Signal,
                          int64_t Pid, sys::TaskProcessInformation ProcInfo);

/// Emits a "skipped" message to the given stream.
void emitSkippedMessage(raw_ostream &os, StringRef Name,
                        DetailedTaskDescription TascDesc);

} // end namespace parseable_output
} // end namespace swift

#endif
