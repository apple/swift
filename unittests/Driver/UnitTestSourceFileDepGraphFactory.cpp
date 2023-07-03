//===--- UnitTestSourceFileDepGraphFactory.cpp ----------------------------===//
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

#include "UnitTestSourceFileDepGraphFactory.h"
#include "MockingFineGrainedDependencyGraphs.h"

using namespace swift;
using namespace swift::fine_grained_dependencies;
using namespace swift::fine_grained_dependencies::
    mocking_fine_grained_dependency_graphs;

//==============================================================================
// MARK: UnitTestSourceFileDepGraphFactory - adding collections of Decls
//==============================================================================

void UnitTestSourceFileDepGraphFactory::forEachEntry(
    function_ref<void(NodeKind kind, StringRef entry)> fn) {
  for (const auto &kindAndEntries : dependencyDescriptions) {
    for (StringRef s : kindAndEntries.second)
      fn(kindAndEntries.first, s);
  }
}

void UnitTestSourceFileDepGraphFactory::addAllDefinedDecls() {
  forEachEntry([&](NodeKind kind, StringRef s) {
    if (isADefinedDecl(s))
      addADefinedDecl(s, kind);
  });
}

void UnitTestSourceFileDepGraphFactory::addAllUsedDecls() {
  forEachEntry([&](NodeKind kind, StringRef s) {
    if (!isADefinedDecl(s))
      addAUsedDecl(s, kind);
  });
}

//==============================================================================
// MARK: UnitTestSourceFileDepGraphFactory - adding individual Decls
//==============================================================================

void UnitTestSourceFileDepGraphFactory::addADefinedDecl(StringRef s,
                                                        const NodeKind kind) {
  const llvm::Optional<DependencyKey> key =
      parseADefinedDecl(s, kind, DeclAspect::interface);
  if (!key)
    return;
  auto fingerprintString = s.split(fingerprintSeparator).second.str();
  const llvm::Optional<Fingerprint> fingerprint =
      swift::mockFingerprintFromString(fingerprintString);

  AbstractSourceFileDepGraphFactory::addADefinedDecl(key.value(),
                                                     fingerprint);
}

void UnitTestSourceFileDepGraphFactory::addAUsedDecl(const StringRef s,
                                                     const NodeKind kind) {
  const auto defAndUseKeys = parseAUsedDecl(s, kind);
  if (defAndUseKeys) {
    AbstractSourceFileDepGraphFactory::addAUsedDecl(defAndUseKeys->first,
                                                    defAndUseKeys->second);
  }
}

DependencyKey
UnitTestSourceFileDepGraphFactory::computeUseKey(StringRef s,
                                                 const bool isCascadingUse) {
  // For now, in unit tests, mock uses are always nominal
  static const NodeKind kindOfUse = NodeKind::nominal;
  const DeclAspect aspectOfUse =
      isCascadingUse ? DeclAspect::interface : DeclAspect::implementation;

  if (!s.empty())
    return parseADefinedDecl(s, kindOfUse, aspectOfUse).value();
  StringRef swiftDepsRef(swiftDeps);
  return DependencyKey::Builder(NodeKind::sourceFileProvide, aspectOfUse)
          .withName(swiftDepsRef)
          .build();
}

//==============================================================================
// MARK: AbstractSourceFileDepGraphFactory - parsing
//==============================================================================

const char *UnitTestSourceFileDepGraphFactory::defUseSeparator = "->";

bool UnitTestSourceFileDepGraphFactory::isADefinedDecl(StringRef s) {
  return s.find(defUseSeparator) == StringRef::npos;
}

llvm::Optional<DependencyKey>
UnitTestSourceFileDepGraphFactory::parseADefinedDecl(StringRef s,
                                                     const NodeKind kind,
                                                     const DeclAspect aspect) {
  static const char *privatePrefix = "#";

  s.consume_front(privatePrefix);
  const std::string context =
      parseContext(s.split(fingerprintSeparator).first, kind);
  std::string name = parseName(s.split(fingerprintSeparator).first, kind);

  return DependencyKey(kind, aspect, context, name);
}

llvm::Optional<std::pair<DependencyKey, DependencyKey>>
UnitTestSourceFileDepGraphFactory::parseAUsedDecl(const StringRef argString,
                                                  const NodeKind kind) {
  static const char *noncascadingPrefix = "#";
  static const char *privateHolderPrefix = "~";

  StringRef s = argString;
  const bool isCascadingUse = !s.consume_front(noncascadingPrefix);
  // Someday, we might differentiate.
  const DeclAspect aspectOfDefUsed = DeclAspect::interface;

  s.consume_front(privateHolderPrefix);
  const auto defUseStrings = s.split(defUseSeparator);
  const auto context = parseContext(defUseStrings.first, kind);
  const auto name = parseName(defUseStrings.first, kind);

  const DependencyKey defKey =
      DependencyKey(kind, aspectOfDefUsed, context, name);

  return std::make_pair(defKey,
                        computeUseKey(defUseStrings.second, isCascadingUse));
}

llvm::Optional<bool>
UnitTestSourceFileDepGraphFactory::singleNameIsContext(const NodeKind kind) {
  switch (kind) {
  case NodeKind::nominal:
  case NodeKind::potentialMember:
    return true;
  case NodeKind::topLevel:
  case NodeKind::dynamicLookup:
  case NodeKind::externalDepend:
  case NodeKind::sourceFileProvide:
    return false;
  case NodeKind::member:
    return llvm::None;
  case NodeKind::kindCount:
    llvm_unreachable("impossible case");
  }
}

std::string
UnitTestSourceFileDepGraphFactory::parseContext(const StringRef s,
                                                const NodeKind kind) {
  return !singleNameIsContext(kind)
             ? s.split(nameContextSeparator).first.str()
             : singleNameIsContext(kind).value() ? s.str() : "";
}

std::string UnitTestSourceFileDepGraphFactory::parseName(const StringRef s,
                                                         const NodeKind kind) {
  const std::string name =
      !singleNameIsContext(kind)
          ? s.split(nameContextSeparator).second.str()
          : singleNameIsContext(kind).value() ? "" : s.str();
  assert(kind != NodeKind::member ||
         !name.empty() && "Should be a potentialMember");
  return name;
}
