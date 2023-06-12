//===--- CompileJobCacheKey.cpp - compile cache key methods ---------------===//
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
// This file contains utility methods for creating compile job cache keys.
//
//===----------------------------------------------------------------------===//

#include <swift/Frontend/CompileJobCacheKey.h>
#include <swift/Basic/Version.h>
#include <llvm/ADT/SmallString.h>
#include "llvm/ADT/STLExtras.h"
#include "llvm/CAS/HierarchicalTreeBuilder.h"
#include "llvm/CAS/ObjectStore.h"

using namespace swift;

// TODO: Rewrite this into CASNodeSchema.
llvm::Expected<llvm::cas::ObjectRef> swift::createCompileJobBaseCacheKey(
    llvm::cas::ObjectStore &CAS, ArrayRef<const char *> Args) {
  SmallString<256> CommandLine;

  // TODO: Improve this list.
  static const std::vector<std::string> removeArgAndNext = {
      "-o", "-supplementary-output-file-map", "-serialize-diagnostics-path",
      "-num-threads", "-cas-path"};

  // Don't count the `-frontend` in the first location since only frontend
  // invocation can have a cache key.
  if (Args.size() > 1 && StringRef(Args.front()) == "-frontend")
    Args = Args.drop_front();

  bool SkipNext = false;
  for (StringRef Arg : Args) {
    if (SkipNext) {
      SkipNext = false;
      continue;
    }
    if (llvm::is_contained(removeArgAndNext, Arg)) {
      SkipNext = true;
      continue;
    }
    // FIXME: Use a heuristic to remove all the flags that affect output paths.
    // Those should not affect compile cache key.
    if (Arg.startswith("-emit-")) {
      if (Arg.endswith("-path"))
        SkipNext = true;
      continue;
    }
    CommandLine.append(Arg);
    CommandLine.push_back(0);
  }

  llvm::cas::HierarchicalTreeBuilder Builder;
  auto CMD = CAS.storeFromString(None, CommandLine);
  if (!CMD)
    return CMD.takeError();
  Builder.push(*CMD, llvm::cas::TreeEntry::Regular, "command-line");

  // FIXME: The version is maybe insufficient...
  auto Version = CAS.storeFromString(None, version::getSwiftFullVersion());
  if (!Version)
    return Version.takeError();
  Builder.push(*Version, llvm::cas::TreeEntry::Regular, "version");

  if (auto Out = Builder.create(CAS))
    return Out->getRef();
  else
    return Out.takeError();
}

llvm::Expected<llvm::cas::ObjectRef> swift::createCompileJobCacheKeyForOutput(
    llvm::cas::ObjectStore &CAS, llvm::cas::ObjectRef BaseKey,
    StringRef ProducingInput, file_types::ID OutputType) {
  SmallString<256> OutputInfo;

  // Encode the OutputType as first byte, then append the input file path.
  OutputInfo.emplace_back((char)OutputType);
  OutputInfo.append(ProducingInput);

  return CAS.storeFromString({BaseKey}, OutputInfo);
}
