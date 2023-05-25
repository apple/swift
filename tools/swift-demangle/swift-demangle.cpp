//===--- swift-demangle.cpp - Swift Demangler app -------------------------===//
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
// This is the entry point.
//
//===----------------------------------------------------------------------===//

#include "swift/Demangling/Demangle.h"
#include "swift/Demangling/ManglingMacros.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/raw_ostream.h"

// For std::rand, to work around a bug if main()'s first function call passes
// argv[0].
#if defined(__CYGWIN__)
#include <cstdlib>
#endif

#include <iostream>

using namespace swift::Demangle;

static llvm::cl::opt<bool>
ExpandMode("expand",
               llvm::cl::desc("Expand mode (show node structure of the demangling)"));

static llvm::cl::opt<bool>
CompactMode("compact",
          llvm::cl::desc("Compact mode (only emit the demangled names)"));

static llvm::cl::opt<bool>
TreeOnly("tree-only",
           llvm::cl::desc("Tree-only mode (do not show the demangled string)"));

static llvm::cl::opt<bool>
StripSpecialization("strip-specialization",
           llvm::cl::desc("Remangle the origin of a specialized function"));

static llvm::cl::opt<bool>
RemangleMode("test-remangle",
           llvm::cl::desc("Remangle test mode (show the remangled string)"));

static llvm::cl::opt<bool>
RemangleRtMode("remangle-objc-rt",
           llvm::cl::desc("Remangle to the ObjC runtime name mangling scheme"));

static llvm::cl::opt<bool>
RemangleNew("remangle-new",
           llvm::cl::desc("Remangle the symbol with new mangling scheme"));

static llvm::cl::opt<bool>
DisableSugar("no-sugar",
           llvm::cl::desc("No sugar mode (disable common language idioms such as ? and [] from the output)"));

static llvm::cl::opt<bool>
Simplified("simplified",
           llvm::cl::desc("Don't display module names or implicit self types"));

static llvm::cl::opt<bool>
Classify("classify",
           llvm::cl::desc("Display symbol classification characters"));

/// Options that are primarily used for testing.
/// \{
static llvm::cl::opt<bool> DisplayLocalNameContexts(
    "display-local-name-contexts", llvm::cl::init(true),
    llvm::cl::desc("Qualify local names"),
    llvm::cl::Hidden);

static llvm::cl::opt<bool> DisplayStdlibModule(
    "display-stdlib-module", llvm::cl::init(true),
    llvm::cl::desc("Qualify types originating from the Swift standard library"),
    llvm::cl::Hidden);

static llvm::cl::opt<bool> DisplayObjCModule(
    "display-objc-module", llvm::cl::init(true),
    llvm::cl::desc("Qualify types originating from the __ObjC module"),
    llvm::cl::Hidden);

static llvm::cl::opt<std::string> HidingModule(
    "hiding-module",
    llvm::cl::desc("Don't qualify types originating from this module"),
    llvm::cl::Hidden);
/// \}


static llvm::cl::list<std::string>
InputNames(llvm::cl::Positional, llvm::cl::desc("[mangled name...]"),
               llvm::cl::ZeroOrMore);

static llvm::StringRef substrBefore(llvm::StringRef whole,
                                    llvm::StringRef part) {
  return whole.slice(0, part.data() - whole.data());
}

static llvm::StringRef substrAfter(llvm::StringRef whole,
                                   llvm::StringRef part) {
  return whole.substr((part.data() - whole.data()) + part.size());
}

static void stripSpecialization(NodePointer Node) {
  if (Node->getKind() != Node::Kind::Global)
    return;
  switch (Node->getFirstChild()->getKind()) {
    case Node::Kind::FunctionSignatureSpecialization:
    case Node::Kind::GenericSpecialization:
    case Node::Kind::GenericSpecializationPrespecialized:
    case Node::Kind::GenericSpecializationNotReAbstracted:
    case Node::Kind::GenericPartialSpecialization:
    case Node::Kind::GenericPartialSpecializationNotReAbstracted:
      Node->removeChildAt(0);
      break;
    default:
      break;
  }
}

static void demangle(llvm::raw_ostream &os, llvm::StringRef name,
                     swift::Demangle::Context &DCtx,
                     const swift::Demangle::DemangleOptions &options) {
  bool hadLeadingUnderscore = false;
  if (name.startswith("__")) {
    hadLeadingUnderscore = true;
    name = name.substr(1);
  }
  swift::Demangle::NodePointer pointer = DCtx.demangleSymbolAsNode(name);
  if (ExpandMode || TreeOnly) {
    os << "Demangling for " << name << '\n';
    os << getNodeTreeAsString(pointer);
  }
  if (RemangleMode) {
    std::string remangled;
    if (!pointer || !(name.startswith(MANGLING_PREFIX_STR) ||
                      name.startswith("_S"))) {
      // Just reprint the original mangled name if it didn't demangle or is in
      // the old mangling scheme.
      // This makes it easier to share the same database between the
      // mangling and demangling tests.
      remangled = name.str();
    } else {
      auto mangling = swift::Demangle::mangleNode(pointer);
      if (!mangling.isSuccess()) {
        llvm::errs() << "Error: (" << mangling.error().code << ":"
                     << mangling.error().line << ") unable to re-mangle "
                     << name << '\n';
        exit(1);
      }
      remangled = mangling.result();
      unsigned prefixLen = swift::Demangle::getManglingPrefixLength(remangled);
      assert(prefixLen > 0);
      // Replace the prefix if we remangled with a different prefix.
      if (!name.startswith(remangled.substr(0, prefixLen))) {
        unsigned namePrefixLen =
          swift::Demangle::getManglingPrefixLength(name);
        assert(namePrefixLen > 0);
        remangled = name.take_front(namePrefixLen).str() +
                      remangled.substr(prefixLen);
      }
      if (name != remangled) {
        llvm::errs() << "Error: re-mangled name \n  " << remangled
                     << "\ndoes not match original name\n  " << name << '\n';
        exit(1);
      }
    }
    if (hadLeadingUnderscore) os << '_';
    os << remangled;
    return;
  } else if (RemangleRtMode) {
    std::string remangled = name.str();
    if (pointer) {
      auto mangling = swift::Demangle::mangleNodeOld(pointer);
      if (!mangling.isSuccess()) {
        llvm::errs() << "Error: (" << mangling.error().code << ":"
                     << mangling.error().line << ") unable to re-mangle "
                     << name << '\n';
        exit(1);
      }
      remangled = mangling.result();
    }
    os << remangled;
    return;
  }
  if (!TreeOnly) {
    if (RemangleNew) {
      if (!pointer) {
        llvm::errs() << "Error: unable to de-mangle " << name << '\n';
        exit(1);
      }
      auto mangling = swift::Demangle::mangleNode(pointer);
      if (!mangling.isSuccess()) {
        llvm::errs() << "Error: (" << mangling.error().code << ":"
                     << mangling.error().line << ") unable to re-mangle "
                     << name << '\n';
        exit(1);
      }
      std::string remangled = mangling.result();
      os << remangled;
      return;
    }
    if (StripSpecialization) {
      stripSpecialization(pointer);
      auto mangling = swift::Demangle::mangleNode(pointer);
      if (!mangling.isSuccess()) {
        llvm::errs() << "Error: (" << mangling.error().code << ":"
                     << mangling.error().line << ") unable to re-mangle "
                     << name << '\n';
        exit(1);
      }
      std::string remangled = mangling.result();
      os << remangled;
      return;
    }
    std::string string = swift::Demangle::nodeToString(pointer, options);
    if (!CompactMode)
      os << name << " ---> ";

    if (Classify) {
      std::string Classifications;
      if (!swift::Demangle::isSwiftSymbol(name))
        Classifications += 'N';
      if (DCtx.isThunkSymbol(name)) {
        if (!Classifications.empty())
          Classifications += ',';
        Classifications += "T:";
        Classifications += DCtx.getThunkTarget(name);
      } else {
        assert(DCtx.getThunkTarget(name).empty());
      }
      if (pointer && !DCtx.hasSwiftCallingConvention(name)) {
        if (!Classifications.empty())
          Classifications += ',';
        Classifications += 'C';
      }
      if (!Classifications.empty())
        os << '{' << Classifications << "} ";
    }
    os << (string.empty() ? name : llvm::StringRef(string));
  }
  DCtx.clear();
}

static bool isValidInMangling(char ch) {
  return (ch == '_' || ch == '$' || ch == '.'
          || (ch >= 'a' && ch <= 'z')
          || (ch >= 'A' && ch <= 'Z')
          || (ch >= '0' && ch <= '9'));
}

static bool findMaybeMangled(llvm::StringRef input, llvm::StringRef &match) {
  const char *ptr = input.data();
  size_t len = input.size();
  const char *end = ptr + len;
  enum {
    Start,
    SeenUnderscore,
    SeenDollar,
    FoundPrefix
  } state = Start;
  const char *matchStart = nullptr;

  // Find _T, $S, $s, _$S, _$s, @__swiftmacro_ followed by a valid mangled string
  while (ptr < end) {
    switch (state) {
    case Start:
      while (ptr < end) {
        char ch = *ptr++;

        if (ch == '_') {
          state = SeenUnderscore;
          matchStart = ptr - 1;
          break;
        } else if (ch == '$') {
          state = SeenDollar;
          matchStart = ptr - 1;
          break;
        } else if (ch == '@' &&
                   llvm::StringRef(ptr, end - ptr).startswith("__swiftmacro_")){
          matchStart = ptr - 1;
          ptr = ptr + strlen("__swiftmacro_");
          state = FoundPrefix;
          break;
        }
      }
      break;

    case SeenUnderscore:
      while (ptr < end) {
        char ch = *ptr++;

        if (ch == 'T') {
          state = FoundPrefix;
          break;
        } else if (ch == '$') {
          state = SeenDollar;
          break;
        } else if (ch == '_') {
          matchStart = ptr - 1;
        } else {
          state = Start;
          break;
        }
      }
      break;

    case SeenDollar:
      while (ptr < end) {
        char ch = *ptr++;

        if (ch == 'S' || ch == 's') {
          state = FoundPrefix;
          break;
        } else if (ch == '_') {
          state = SeenUnderscore;
          matchStart = ptr - 1;
          break;
        } else if (ch == '$') {
          matchStart = ptr - 1;
        } else {
          state = Start;
          break;
        }
      }
      break;

    case FoundPrefix:
      {
        const char *mangled = ptr;

        while (ptr < end && isValidInMangling(*ptr))
          ++ptr;

        if (ptr == mangled) {
          state = Start;
          break;
        }

        match = llvm::StringRef(matchStart, ptr - matchStart);
        return true;
      }
    }
  }

  return false;
}

static int demangleSTDIN(const swift::Demangle::DemangleOptions &options) {
  swift::Demangle::Context DCtx;
  for (std::string mangled; std::getline(std::cin, mangled);) {
    llvm::StringRef inputContents(mangled);
    llvm::StringRef match;

    while (findMaybeMangled(inputContents, match)) {
      llvm::outs() << substrBefore(inputContents, match);
      demangle(llvm::outs(), match, DCtx, options);
      inputContents = substrAfter(inputContents, match);
    }

    llvm::outs() << inputContents << '\n';
  }

  return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
#if defined(__CYGWIN__)
  // Cygwin clang 3.5.2 with '-O3' generates CRASHING BINARY,
  // if main()'s first function call is passing argv[0].
  std::rand();
#endif
  llvm::cl::ParseCommandLineOptions(argc, argv);

  swift::Demangle::DemangleOptions options;
  options.SynthesizeSugarOnTypes = !DisableSugar;
  if (Simplified)
    options = swift::Demangle::DemangleOptions::SimplifiedUIDemangleOptions();
  options.DisplayStdlibModule = DisplayStdlibModule;
  options.DisplayObjCModule = DisplayObjCModule;
  options.HidingCurrentModule = HidingModule;
  options.DisplayLocalNameContexts = DisplayLocalNameContexts;

  if (InputNames.empty()) {
    CompactMode = true;
    return demangleSTDIN(options);
  } else {
    swift::Demangle::Context DCtx;
    for (llvm::StringRef name : InputNames) {
      if (name == "_") {
        llvm::errs() << "warning: input symbol '_' is likely the result of "
                        "variable expansion by the shell. Ensure the argument "
                        "is quoted or escaped.\n";
        continue;
      }
      if (name.startswith("S") || name.startswith("s") ) {
        std::string correctedName = std::string("$") + name.str();
        demangle(llvm::outs(), correctedName, DCtx, options);
      } else {
        demangle(llvm::outs(), name, DCtx, options);
      }
      llvm::outs() << '\n';
    }

    return EXIT_SUCCESS;
  }
}
