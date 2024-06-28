//===--- BlockList.cpp - BlockList utilities ------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/YAMLParser.h"
#include "llvm/Support/YAMLTraits.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/BlockList.h"
#include "swift/Basic/SourceManager.h"

struct swift::BlockListStore::Implementation {
  SourceManager SM;
  llvm::StringMap<std::vector<BlockListAction>> ModuleActionDict;
  llvm::StringMap<std::vector<BlockListAction>> ProjectActionDict;
  void addConfigureFilePath(StringRef path);
  bool hasBlockListAction(StringRef key, BlockListKeyKind keyKind,
                          BlockListAction action);
  void collectBlockList(llvm::yaml::Node *N, BlockListAction action);

  llvm::StringMap<std::vector<BlockListAction>> *getDictToUse(BlockListKeyKind kind) {
    switch (kind) {
    case BlockListKeyKind::ModuleName:
      return &ModuleActionDict;
    case BlockListKeyKind::ProjectName:
      return &ProjectActionDict;
    case BlockListKeyKind::Undefined:
      return nullptr;
    }
  }
  static std::string getScalaString(llvm::yaml::Node *N) {
    llvm::SmallString<64> Buffer;
    if (auto *scala = dyn_cast<llvm::yaml::ScalarNode>(N)) {
      return scala->getValue(Buffer).str();
    }
    return std::string();
  }
};

swift::BlockListStore::BlockListStore(): Impl(*new Implementation()) {}

swift::BlockListStore::~BlockListStore() { delete &Impl; }

bool swift::BlockListStore::hasBlockListAction(StringRef key,
    BlockListKeyKind keyKind, BlockListAction action) {
  return Impl.hasBlockListAction(key, keyKind, action);
}

void swift::BlockListStore::addConfigureFilePath(StringRef path) {
  Impl.addConfigureFilePath(path);
}

bool swift::BlockListStore::Implementation::hasBlockListAction(StringRef key,
    BlockListKeyKind keyKind, BlockListAction action) {
  auto *dict = getDictToUse(keyKind);
  assert(dict);
  auto it = dict->find(key);
  if (it == dict->end())
    return false;
  return llvm::is_contained(it->second, action);
}

void swift::BlockListStore::Implementation::collectBlockList(llvm::yaml::Node *N,
                                                      BlockListAction action) {
  namespace yaml = llvm::yaml;
  auto *pair = dyn_cast<yaml::KeyValueNode>(N);
  if (!pair)
    return;
  std::string rawKey = getScalaString(pair->getKey());
  auto keyKind = llvm::StringSwitch<BlockListKeyKind>(rawKey)
#define CASE(X) .Case(#X, BlockListKeyKind::X)
    CASE(ModuleName)
    CASE(ProjectName)
#undef CASE
    .Default(BlockListKeyKind::Undefined);
  if (keyKind == BlockListKeyKind::Undefined)
    return;
  auto *dictToUse = getDictToUse(keyKind);
  assert(dictToUse);
  auto *seq = dyn_cast<yaml::SequenceNode>(pair->getValue());
  if (!seq)
    return;
  for (auto &node: *seq) {
    std::string name = getScalaString(&node);
    dictToUse->insert({name, std::vector<BlockListAction>()})
      .first->second.push_back(action);
  }
}

void swift::BlockListStore::Implementation::addConfigureFilePath(StringRef path) {
  namespace yaml = llvm::yaml;

  // Load the input file.
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> FileBufOrErr =
    vfs::getFileOrSTDIN(*SM.getFileSystem(), path,
                        /*FileSize*/-1, /*RequiresNullTerminator*/true,
                        /*IsVolatile*/false, /*RetryCount*/30);
  if (!FileBufOrErr) {
    return;
  }
  StringRef Buffer = FileBufOrErr->get()->getBuffer();
  yaml::Stream Stream(llvm::MemoryBufferRef(Buffer, path),
                      SM.getLLVMSourceMgr());
  for (auto DI = Stream.begin(); DI != Stream.end(); ++ DI) {
    assert(DI != Stream.end() && "Failed to read a document");
    yaml::Node *N = DI->getRoot();
    for (auto &pair: *dyn_cast<yaml::MappingNode>(N)) {
      std::string key = getScalaString(pair.getKey());
      auto action = llvm::StringSwitch<BlockListAction>(key)
#define BLOCKLIST_ACTION(X) .Case(#X, BlockListAction::X)
#include "swift/Basic/BlockListAction.def"
        .Default(BlockListAction::Undefined);
      if (action == BlockListAction::Undefined)
        continue;
      auto *map = dyn_cast<yaml::MappingNode>(pair.getValue());
      if (!map)
        continue;
      for (auto &innerPair: *map) {
        collectBlockList(&innerPair, action);
      }
    }
  }
}
