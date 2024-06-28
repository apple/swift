//===-- LocalizationFormat.cpp - Format for Diagnostic Messages -*- C++ -*-===//
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
//
// This file implements the format for localized diagnostic messages.
//
//===----------------------------------------------------------------------===//

#include "swift/Localization/LocalizationFormat.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/Range.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Bitstream/BitstreamReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>

namespace {

enum LocalDiagID : uint32_t {
#define DIAG(KIND, ID, Options, Text, Signature) ID,
#include "swift/AST/DiagnosticsAll.def"
  NumDiags
};

static constexpr const char *const diagnosticNameStrings[] = {
#define DIAG(KIND, ID, Options, Text, Signature) " [" #ID "]",
#include "swift/AST/DiagnosticsAll.def"
    "<not a diagnostic>",
};

} // namespace

namespace swift {
namespace diag {

void SerializedLocalizationWriter::insert(swift::DiagID id,
                                          llvm::StringRef translation) {
  generator.insert(static_cast<uint32_t>(id), translation);
}

bool SerializedLocalizationWriter::emit(llvm::StringRef filePath) {
  assert(llvm::sys::path::extension(filePath) == ".db");
  std::error_code error;
  llvm::raw_fd_ostream OS(filePath, error, llvm::sys::fs::OF_None);
  if (OS.has_error()) {
    return true;
  }

  offset_type offset;
  {
    llvm::support::endian::write<offset_type>(OS, 0, llvm::support::little);
    offset = generator.Emit(OS);
  }
  OS.seek(0);
  llvm::support::endian::write(OS, offset, llvm::support::little);
  OS.close();

  return OS.has_error();
}

void LocalizationProducer::initializeIfNeeded() {
  if (state != NotInitialized)
    return;

  if (initializeImpl())
    state = Initialized;
  else
    state = FailedInitialization;
}

llvm::StringRef
LocalizationProducer::getMessageOr(swift::DiagID id,
                                   llvm::StringRef defaultMessage) {
  initializeIfNeeded();
  if (getState() == FailedInitialization) {
    return defaultMessage;
  }

  auto localizedMessage = getMessage(id);
  if (localizedMessage.empty())
    return defaultMessage;
  if (printDiagnosticNames) {
    llvm::StringRef diagnosticName(diagnosticNameStrings[(unsigned)id]);
    auto localizedDebugDiagnosticMessage =
        localizationSaver.save(localizedMessage.str() + diagnosticName.str());
    return localizedDebugDiagnosticMessage;
  }
  return localizedMessage;
}

LocalizationProducerState LocalizationProducer::getState() const {
  return state;
}

SerializedLocalizationProducer::SerializedLocalizationProducer(
    std::unique_ptr<llvm::MemoryBuffer> buffer, bool printDiagnosticNames)
    : LocalizationProducer(printDiagnosticNames), Buffer(std::move(buffer)) {
}

bool SerializedLocalizationProducer::initializeImpl() {
  auto base =
      reinterpret_cast<const unsigned char *>(Buffer.get()->getBufferStart());
  auto tableOffset = endian::read<offset_type>(base, little);
  SerializedTable.reset(SerializedLocalizationTable::Create(
      base + tableOffset, base + sizeof(offset_type), base));
  return true;
}

llvm::StringRef
SerializedLocalizationProducer::getMessage(swift::DiagID id) const {
  auto value = SerializedTable.get()->find(id);
  if (value.getDataLen() == 0)
    return llvm::StringRef();
  return {(const char *)value.getDataPtr(), value.getDataLen()};
}

std::unique_ptr<LocalizationProducer>
LocalizationProducer::producerFor(llvm::StringRef locale, llvm::StringRef path,
                                  bool printDiagnosticNames) {
  llvm::SmallString<128> filePath(path);
  llvm::sys::path::append(filePath, locale);
  llvm::sys::path::replace_extension(filePath, ".db");

  // If the serialized diagnostics file not available,
  // fallback to the `.strings` file.
  if (llvm::sys::fs::exists(filePath)) {
    if (auto file = llvm::MemoryBuffer::getFile(filePath)) {
      return std::make_unique<diag::SerializedLocalizationProducer>(
          std::move(file.get()), printDiagnosticNames);
    }
  } else {
    llvm::sys::path::replace_extension(filePath, ".strings");
    if (llvm::sys::fs::exists(filePath)) {
      return std::make_unique<diag::StringsLocalizationProducer>(
          filePath.str(), printDiagnosticNames);
    }
  }

  return std::unique_ptr<LocalizationProducer>();
}

void DefToStringsConverter::convert(llvm::raw_ostream &out) {
  // "<id>" = "<msg>";
  for (auto i : swift::indices(IDs)) {
    out << "\"" << IDs[i] << "\"";
    out << " = ";

    const std::string &msg = Messages[i];

    out << "\"";
    for (unsigned j = 0; j < msg.length(); ++j) {
      // Escape '"' found in the message.
      if (msg[j] == '"')
        out << '\\';

      out << msg[j];
    }

    out << "\";\r\n";
  }
}

bool StringsLocalizationProducer::initializeImpl() {
  auto FileBufOrErr = llvm::MemoryBuffer::getFileOrSTDIN(filePath);
  llvm::MemoryBuffer *document = FileBufOrErr->get();
  readStringsFile(document, diagnostics);
  return true;
}

llvm::StringRef
StringsLocalizationProducer::getMessage(swift::DiagID id) const {
  return diagnostics[(unsigned)id];
}

void StringsLocalizationProducer::forEachAvailable(
    llvm::function_ref<void(swift::DiagID, llvm::StringRef)> callback) {
  initializeIfNeeded();
  if (getState() == FailedInitialization) {
    return;
  }

  for (uint32_t i = 0, n = diagnostics.size(); i != n; ++i) {
    auto translation = diagnostics[i];
    if (!translation.empty())
      callback(static_cast<swift::DiagID>(i), translation);
  }
}

void StringsLocalizationProducer::readStringsFile(
    llvm::MemoryBuffer *in, std::vector<std::string> &diagnostics) {
  std::map<std::string, unsigned> diagLocs;
#define DIAG(KIND, ID, Options, Text, Signature)                               \
  diagLocs[#ID] = static_cast<unsigned>(LocalDiagID::ID);
#include "swift/AST/DiagnosticsAll.def"
#undef DIAG

  // Allocate enough slots to fit all the possible diagnostics
  // this helps to identify which diagnostics are missing.
  diagnostics.resize(LocalDiagID::NumDiags);

  // The format is as follows:
  //
  // - comment: /* ... */
  // - translation: "<id>" = "<message>";
  auto buffer = in->getBuffer();
  while (!buffer.empty()) {
    // consume comment.
    if (buffer.starts_with("/*")) {
      auto endOfComment = buffer.find("*/");
      assert(endOfComment != std::string::npos);
      // Consume the comment and trailing `*/`
      buffer = buffer.drop_front(endOfComment + 2).ltrim();
      continue;
    }

    assert(buffer.starts_with("\"") && "malformed diagnostics file");

    // Consume leading `"`
    buffer = buffer.drop_front();

    // Valid diagnostic id cannot have any `"` in it.
    auto idSize = buffer.find_first_of('\"');
    assert(idSize != std::string::npos);

    std::string id(buffer.data(), idSize);

    // consume id and `" = "`. There could be a variable number of
    // spaces on each side of `=`.
    {
      // Consume id, trailing `"`, and all spaces before `=`
      buffer = buffer.drop_front(idSize + 1).ltrim(' ');

      // Consume `=` and all trailing spaces until `"`
      {
        assert(!buffer.empty() && buffer.front() == '=');
        buffer = buffer.drop_front().ltrim(' ');
      }

      // Consume `"` at the beginning of the diagnostic message.
      {
        assert(!buffer.empty() && buffer.front() == '\"');
        buffer = buffer.drop_front();
      }
    }

    llvm::SmallString<64> msg;
    {
      bool isValid = false;
      // Look for `";` which denotes the end of message
      for (unsigned i = 0, n = buffer.size(); i != n; ++i) {
        if (buffer[i] != '\"') {
          msg.push_back(buffer[i]);
          continue;
        }

        // Leading `"` has been comsumed.
        assert(i > 0);

        // Let's check whether this `"` is escaped, and if so - continue
        // because `"` is part of the message.
        if (buffer[i - 1] == '\\') {
          // Drop `\` added for escaping.
          msg.pop_back();
          msg.push_back(buffer[i]);
          continue;
        }

        // If current `"` was not escaped and it's followed by `;` -
        // we have reached the end of the message, otherwise
        // the input is malformed.
        if (i + 1 < n && buffer[i + 1] == ';') {
          // Consume the message and its trailing info.
          buffer = buffer.drop_front(i + 2).ltrim();
          // Mark message as valid.
          isValid = true;
          break;
        } else {
          llvm_unreachable("malformed diagnostics file");
        }
      }

      assert(isValid && "malformed diagnostic message");
    }

    // Check whether extracted diagnostic still exists in the
    // system and if not - record as unknown.
    {
      auto existing = diagLocs.find(id);
      if (existing != diagLocs.end()) {
        diagnostics[existing->second] = std::string(msg);
      } else {
        llvm::errs() << "[!] Unknown diagnostic: " << id << '\n';
      }
    }
  }
}

} // namespace diag
} // namespace swift
