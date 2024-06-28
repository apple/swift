//===--- Mangler.cpp - Base class for Swift name mangling -----------------===//
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

#include "swift/Basic/Assertions.h"
#include "swift/Basic/Mangler.h"
#include "swift/Demangling/Demangler.h"
#include "swift/Demangling/Punycode.h"
#include "swift/Demangling/ManglingMacros.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/CommandLine.h"
#include <algorithm>

using namespace swift;
using namespace Mangle;

#ifndef NDEBUG

llvm::cl::opt<bool> PrintSwiftManglingStats(
    "print-swift-mangling-stats", llvm::cl::init(false),
    llvm::cl::desc("Print statistics about Swift symbol mangling"));

namespace {

struct SizeStatEntry {
  int sizeDiff;
  std::string Old;
  std::string New;
};

static std::vector<SizeStatEntry> SizeStats;

static int numSmaller = 0;
static int numEqual = 0;
static int numLarger = 0;
static int totalOldSize = 0;
static int totalNewSize = 0;
static int mergedSubsts = 0;
static int numLargeSubsts = 0;

struct OpStatEntry {
  OpStatEntry() : num(0), size(0) { }
  
  int num;
  int size;
};

static llvm::StringMap<OpStatEntry> OpStats;

} // end anonymous namespace

void Mangler::recordOpStatImpl(StringRef op, size_t OldPos) {
  if (PrintSwiftManglingStats) {
    OpStatEntry &E = OpStats[op];
    ++E.num;
    E.size += Storage.size() - OldPos;
  }
}

#endif // NDEBUG

void Mangle::printManglingStats() {
#ifndef NDEBUG
  if (!PrintSwiftManglingStats)
    return;

  std::sort(SizeStats.begin(), SizeStats.end(),
    [](const SizeStatEntry &LHS, const SizeStatEntry &RHS) {
      return LHS.sizeDiff < RHS.sizeDiff;
    });

  llvm::outs() << "Mangling size stats:\n"
                  "  num smaller: " << numSmaller << "\n"
                  "  num larger:  " << numLarger << "\n"
                  "  num equal:   " << numEqual << "\n"
                  "  total old size: " << totalOldSize << "\n"
                  "  total new size: " << totalNewSize << "\n"
                  "  new - old size: " << (totalNewSize - totalOldSize) << "\n"
                  "List or larger:\n";
  for (const SizeStatEntry &E : SizeStats) {
    llvm::outs() << "  delta " << E.sizeDiff << ": " << E.Old << " - " << E.New
                 << '\n';
  }
  
  llvm::outs() << "Mangling operator stats:\n";

  using MapEntry = llvm::StringMapEntry<OpStatEntry>;
  std::vector<const MapEntry *> SortedOpStats;
  for (const MapEntry &ME : OpStats) {
    SortedOpStats.push_back(&ME);
  }
  std::sort(SortedOpStats.begin(), SortedOpStats.end(),
    [](const MapEntry *LHS, const MapEntry *RHS) {
      return LHS->getKey() < RHS->getKey();
    });

  for (const MapEntry *E : SortedOpStats) {
    llvm::outs() << "  " << E->getKey() << ": num = " << E->getValue().num
                 << ", size = " << E->getValue().size << '\n';
  }
  llvm::outs() << "  merged substitutions: " << mergedSubsts << "\n"
                  "  large substitutions: " << numLargeSubsts << "\n";
#endif
}

void Mangler::beginManglingWithoutPrefix() {
  Storage.clear();
  Substitutions.clear();
  StringSubstitutions.clear();
  NextSubstitutionIndex = 0;
  Words.clear();
  SubstMerging.clear();
}

void Mangler::beginMangling() {
  beginManglingWithoutPrefix();
  Buffer << MANGLING_PREFIX_STR;
}

/// Finish the mangling of the symbol and return the mangled name.
std::string Mangler::finalize() {
  assert(Storage.size() && "Mangling an empty name");
  std::string result = Storage.str().str();
  Storage.clear();

#ifndef NDEBUG
  if (StringRef(result).starts_with(MANGLING_PREFIX_STR))
    verify(result);
#endif

  return result;
}

/// Finish the mangling of the symbol and write the mangled name into
/// \p stream.
void Mangler::finalize(llvm::raw_ostream &stream) {
  std::string result = finalize();
  stream.write(result.data(), result.size());
}

LLVM_ATTRIBUTE_UNUSED
static bool treeContains(Demangle::NodePointer Nd, Demangle::Node::Kind Kind) {
  if (Nd->getKind() == Kind)
    return true;

  for (auto Child : *Nd) {
    if (treeContains(Child, Kind))
      return true;
  }
  return false;
}

void Mangler::verify(StringRef nameStr) {
#ifndef NDEBUG
  SmallString<128> buffer;
  if (!nameStr.starts_with(MANGLING_PREFIX_STR) &&
      !nameStr.starts_with("_Tt") &&
      !nameStr.starts_with("_S")) {
    // This list is the set of prefixes recognized by Demangler::demangleSymbol.
    // It should be kept in sync.
    assert(StringRef(MANGLING_PREFIX_STR) != "_S" && "redundant check");
    buffer += MANGLING_PREFIX_STR;
    buffer += nameStr;
    nameStr = buffer.str();
  }

  Demangler Dem;
  NodePointer Root = Dem.demangleSymbol(nameStr);
  if (!Root || treeContains(Root, Node::Kind::Suffix)) {
    llvm::errs() << "Can't demangle: " << nameStr << '\n';
    abort();
  }
  auto mangling = mangleNode(Root);
  if (!mangling.isSuccess()) {
    llvm::errs() << "Can't remangle: " << nameStr << '\n';
    abort();
  }
  std::string Remangled = mangling.result();
  if (Remangled == nameStr)
    return;

  llvm::errs() << "Remangling failed:\n"
                  "original     = " << nameStr << "\n"
                  "remangled    = " << Remangled << "\n";
  abort();
#endif
}

void Mangler::appendIdentifier(StringRef ident) {
  auto Iter = StringSubstitutions.find(ident);
  if (Iter != StringSubstitutions.end())
    return mangleSubstitution(Iter->second);

  size_t OldPos = Storage.size();
  addSubstitution(ident);

  mangleIdentifier(*this, ident);

  recordOpStat("<identifier>", OldPos);
}

void Mangler::dump() const {
  // FIXME: const_casting because llvm::raw_svector_ostream::str() is
  // incorrectly not marked const.
  llvm::errs() << const_cast<Mangler*>(this)->Buffer.str() << '\n';
}

bool Mangler::tryMangleSubstitution(const void *ptr) {
  auto ir = Substitutions.find(ptr);
  if (ir == Substitutions.end())
    return false;

  mangleSubstitution(ir->second);
  return true;
}

void Mangler::mangleSubstitution(unsigned Idx) {
  if (Idx >= 26) {
#ifndef NDEBUG
    ++numLargeSubsts;
#endif
    return appendOperator("A", Index(Idx - 26));
  }

  char SubstChar = Idx + 'A';
  StringRef Subst(&SubstChar, 1);
  if (SubstMerging.tryMergeSubst(*this, Subst, /*isStandardSubst*/ false)) {
#ifndef NDEBUG
    ++mergedSubsts;
#endif
  } else {
    appendOperator("A", Subst);
  }
}

