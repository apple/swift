//===--- MakeStyleDependencies.cpp -- Emit make-style dependencies --------===//
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

#include "Dependencies.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/Frontend/FrontendOptions.h"
#include "swift/Frontend/InputFile.h"
#include "swift/FrontendTool/FrontendTool.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/VirtualOutputBackend.h"

using namespace swift;

StringRef
swift::frontend::utils::escapeForMake(StringRef raw,
                                      llvm::SmallVectorImpl<char> &buffer) {
  buffer.clear();

  // The escaping rules for GNU make are complicated due to the various
  // substitutions and use of the tab in the leading position for recipes.
  // Various symbols have significance in different contexts.  It is not
  // possible to correctly quote all characters in Make (as of 3.7).  Match
  // gcc and clang's behaviour for the escaping which covers only a subset of
  // characters.
  for (unsigned I = 0, E = raw.size(); I != E; ++I) {
    switch (raw[I]) {
    case '#': // Handle '#' the broken GCC way
      buffer.push_back('\\');
      break;

    case ' ':
      for (unsigned J = I; J && raw[J - 1] == '\\'; --J)
        buffer.push_back('\\');
      buffer.push_back('\\');
      break;

    case '$': // $ is escaped by $
      buffer.push_back('$');
      break;
    }
    buffer.push_back(raw[I]);
  }
  buffer.push_back('\0');

  return buffer.data();
}

/// This sorting function is used to stabilize the order in which dependencies
/// are emitted into \c .d files that are consumed by external build systems.
/// This serves to eliminate order as a source of non-determinism in these
/// outputs.
///
/// The exact sorting predicate is not important. Currently, it is a
/// lexicographic comparison that reverses the provided strings before applying
/// the sorting predicate. This has the benefit of being somewhat
/// invariant with respect to the installation location of various system
/// components. e.g. on two systems, the same file identified by two different
/// paths differing only in their relative install location such as
///
/// /Applications/MyXcode.app/Path/To/A/Framework/In/The/SDK/Header.h
/// /Applications/Xcodes/AnotherXcode.app/Path/To/A/Framework/In/The/SDK/Header.h
///
/// should appear in roughly the same order relative to other paths. Ultimately,
/// this makes it easier to test the contents of the emitted files with tools
/// like FileCheck.
template <typename Container>
static std::vector<std::string>
reversePathSortedFilenames(const Container &elts) {
  std::vector<std::string> tmp(elts.begin(), elts.end());
  std::sort(tmp.begin(), tmp.end(),
            [](const std::string &a, const std::string &b) -> bool {
              return std::lexicographical_compare(a.rbegin(), a.rend(),
                                                  b.rbegin(), b.rend());
            });
  return tmp;
}

/// Emits a Make-style dependencies file.
bool swift::emitMakeDependenciesIfNeeded(DiagnosticEngine &diags,
                                         DependencyTracker *depTracker,
                                         const FrontendOptions &opts,
                                         const InputFile &input,
                                         llvm::vfs::OutputBackend &backend) {
  auto dependenciesFilePath = input.getDependenciesFilePath();
  if (dependenciesFilePath.empty())
    return false;

  auto out = backend.createFile(dependenciesFilePath);
  if (!out) {
    diags.diagnose(SourceLoc(), diag::error_opening_output,
                   dependenciesFilePath, toString(out.takeError()));
    return true;
  }

  llvm::SmallString<256> buffer;

  // collect everything in memory to avoid redundant work
  // when there are multiple targets
  std::string dependencyString;

  // First include all other files in the module. Make-style dependencies
  // need to be conservative!
  auto inputPaths =
      reversePathSortedFilenames(opts.InputsAndOutputs.getInputFilenames());
  for (auto const &path : inputPaths) {
    dependencyString.push_back(' ');
    dependencyString.append(frontend::utils::escapeForMake(path, buffer).str());
  }
  // Then print dependencies we've picked up during compilation.
  auto dependencyPaths =
      reversePathSortedFilenames(depTracker->getDependencies());
  for (auto const &path : dependencyPaths) {
    dependencyString.push_back(' ');
    dependencyString.append(frontend::utils::escapeForMake(path, buffer).str());
  }
  auto incrementalDependencyPaths =
      reversePathSortedFilenames(depTracker->getIncrementalDependencyPaths());
  for (auto const &path : incrementalDependencyPaths) {
    dependencyString.push_back(' ');
    dependencyString.append(frontend::utils::escapeForMake(path, buffer).str());
  }

  // FIXME: Xcode can't currently handle multiple targets in a single
  // dependency line.
  opts.forAllOutputPaths(input, [&](const StringRef targetName) {
    auto targetNameEscaped = frontend::utils::escapeForMake(targetName, buffer);
    *out << targetNameEscaped << " :" << dependencyString << '\n';
  });

  if (auto error = out->keep()) {
    diags.diagnose(SourceLoc(), diag::error_opening_output,
                   dependenciesFilePath, toString(std::move(error)));
    return true;
  }

  return false;
}
