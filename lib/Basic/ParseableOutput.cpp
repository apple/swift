//===-------- ParseableOutput.cpp - Helpers for parseable output ----------===//
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

#include "swift/Basic/ParseableOutput.h"
#include "swift/Basic/FileTypes.h"
#include "swift/Basic/JSONSerialization.h"
#include "swift/Basic/TaskQueue.h"
#include "swift/Driver/Action.h"
#include "swift/Driver/Job.h"
#include "llvm/Option/Arg.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <sstream>

using namespace swift::parseable_output;
using namespace swift::driver;
using namespace swift::sys;
using namespace swift;

namespace swift {
namespace json {
template <> struct ScalarTraits<CommandInput> {
  static void output(const CommandInput &value, llvm::raw_ostream &os) {
    os << value.Path;
  }
  static bool mustQuote(StringRef) { return true; }
};

template <> struct ScalarEnumerationTraits<file_types::ID> {
  static void enumeration(Output &out, file_types::ID &value) {
    file_types::forAllTypes([&](file_types::ID ty) {
      std::string typeName = file_types::getTypeName(ty).str();
      out.enumCase(value, typeName.c_str(), ty);
    });
    out.enumCase(value, "unknown", file_types::ID::TY_INVALID);
  }
};

template <> struct ObjectTraits<std::pair<file_types::ID, std::string>> {
  static void mapping(Output &out,
                      std::pair<file_types::ID, std::string> &value) {
    out.mapRequired("type", value.first);
    out.mapRequired("path", value.second);
  }
};

template <typename T, unsigned N> struct ArrayTraits<SmallVector<T, N>> {
  static size_t size(Output &out, SmallVector<T, N> &seq) { return seq.size(); }

  static T &element(Output &out, SmallVector<T, N> &seq, size_t index) {
    if (index >= seq.size())
      seq.resize(index + 1);
    return seq[index];
  }
};
} // namespace json
} // namespace swift

namespace {

class Message {
  std::string Kind;
  std::string Name;

public:
  Message(StringRef Kind, StringRef Name) : Kind(Kind), Name(Name) {}
  virtual ~Message() = default;

  virtual void provideMapping(swift::json::Output &out) {
    out.mapRequired("kind", Kind);
    out.mapRequired("name", Name);
  }
};

class DetailedMessage : public Message {
  DetailedTaskDescription TascDesc;

public:
  DetailedMessage(StringRef Kind, StringRef Name,
                  DetailedTaskDescription TascDesc)
      : Message(Kind, Name), TascDesc(TascDesc) {}

  void provideMapping(swift::json::Output &out) override {
    Message::provideMapping(out);
    out.mapRequired("command",
                    TascDesc.CommandLine); // Deprecated, do not document
    out.mapRequired("command_executable", TascDesc.Executable);
    out.mapRequired("command_arguments", TascDesc.Arguments);
    out.mapOptional("inputs", TascDesc.Inputs);
    out.mapOptional("outputs", TascDesc.Outputs);
  }
};


class BeganMessage : public DetailedMessage {
  int64_t Pid;
  sys::TaskProcessInformation ProcInfo;

public:
  BeganMessage(StringRef Name, DetailedTaskDescription TascDesc,
               int64_t Pid, sys::TaskProcessInformation ProcInfo)
      : DetailedMessage("began", Name, TascDesc),
        Pid(Pid), ProcInfo(ProcInfo) {}

  void provideMapping(swift::json::Output &out) override {
    DetailedMessage::provideMapping(out);
    out.mapRequired("pid", Pid);
    out.mapRequired("process", ProcInfo);
  }
};

class SkippedMessage : public DetailedMessage {
public:
  SkippedMessage(StringRef Name, DetailedTaskDescription TascDesc)
      : DetailedMessage("skipped", Name, TascDesc) {}
};

class TaskOutputMessage : public Message {
  std::string Output;
  int64_t Pid;
  sys::TaskProcessInformation ProcInfo;

public:
  TaskOutputMessage(StringRef Kind, StringRef Name, std::string Output,
                    int64_t Pid, sys::TaskProcessInformation ProcInfo)
      : Message(Kind, Name),
        Output(Output), Pid(Pid), ProcInfo(ProcInfo) {}

  void provideMapping(swift::json::Output &out) override {
    Message::provideMapping(out);
    out.mapRequired("pid", Pid);
    out.mapOptional("output", Output, std::string());
    out.mapRequired("process", ProcInfo);
  }
};

class SignalledMessage : public TaskOutputMessage {
  std::string ErrorMsg;
  llvm::Optional<int> Signal;

public:
  SignalledMessage(StringRef Name, std::string Output, int64_t Pid,
                   sys::TaskProcessInformation ProcInfo, StringRef ErrorMsg,
                   llvm::Optional<int> Signal)
      : TaskOutputMessage("signalled", Name, Output, Pid, ProcInfo),
        ErrorMsg(ErrorMsg), Signal(Signal) {}

  void provideMapping(swift::json::Output &out) override {
    TaskOutputMessage::provideMapping(out);
    out.mapOptional("error-message", ErrorMsg, std::string());
    out.mapOptional("signal", Signal);
  }
};

class FinishedMessage : public TaskOutputMessage {
  int ExitStatus;
public:
  FinishedMessage(StringRef Name, std::string Output,
                  int64_t Pid, sys::TaskProcessInformation ProcInfo,
                  int ExitStatus)
      : TaskOutputMessage("finished", Name, Output, Pid, ProcInfo),
        ExitStatus(ExitStatus) {}

  void provideMapping(swift::json::Output &out) override {
    TaskOutputMessage::provideMapping(out);
    out.mapRequired("exit-status", ExitStatus);
  }
};

} // end anonymous namespace

namespace swift {
namespace json {

template <> struct ObjectTraits<Message> {
  static void mapping(Output &out, Message &msg) { msg.provideMapping(out); }
};

} // namespace json
} // namespace swift

static void emitMessage(raw_ostream &os, Message &msg) {
  std::string JSONString;
  llvm::raw_string_ostream BufferStream(JSONString);
  json::Output yout(BufferStream);
  yout << msg;
  BufferStream.flush();
  os << JSONString.length() << '\n';
  os << JSONString << '\n';
}

/// Emits a "began" message to the given stream.
void parseable_output::emitBeganMessage(raw_ostream &os, StringRef Name,
                                        DetailedTaskDescription TascDesc,
                                        int64_t Pid,
                                        sys::TaskProcessInformation ProcInfo) {
  BeganMessage msg(Name, TascDesc, Pid, ProcInfo);
  emitMessage(os, msg);
}

void parseable_output::emitFinishedMessage(
    raw_ostream &os, StringRef Name, std::string Output, int ExitStatus,
    int64_t Pid, sys::TaskProcessInformation ProcInfo) {
  FinishedMessage msg(Name, Output, Pid, ProcInfo, ExitStatus);
  emitMessage(os, msg);
}

void parseable_output::emitSignalledMessage(
    raw_ostream &os, StringRef Name, StringRef ErrorMsg, StringRef Output,
    llvm::Optional<int> Signal, int64_t Pid, TaskProcessInformation ProcInfo) {
  SignalledMessage msg(Name, Output.str(), Pid, ProcInfo, ErrorMsg, Signal);
  emitMessage(os, msg);
}

void parseable_output::emitSkippedMessage(raw_ostream &os, StringRef Name,
                                          DetailedTaskDescription TascDesc) {
  SkippedMessage msg(Name, TascDesc);
  emitMessage(os, msg);
}

