//===--- Evaluator.cpp - Request Evaluator Implementation -----------------===//
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
// This file implements the Evaluator class that evaluates and caches
// requests.
//
//===----------------------------------------------------------------------===//
#include "swift/AST/Evaluator.h"
#include "swift/AST/DeclContext.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/TypeCheckRequests.h" // for ResolveMacroRequest
#include "swift/Basic/LangOptions.h"
#include "swift/Basic/Range.h"
#include "swift/Basic/SourceManager.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/SaveAndRestore.h"
#include <vector>

using namespace swift;

AbstractRequestFunction *
Evaluator::getAbstractRequestFunction(uint8_t zoneID, uint8_t requestID) const {
  for (const auto &zone : requestFunctionsByZone) {
    if (zone.first == zoneID) {
      if (requestID < zone.second.size())
        return zone.second[requestID];

      return nullptr;
    }
  }

  return nullptr;
}

void Evaluator::registerRequestFunctions(
                               Zone zone,
                               ArrayRef<AbstractRequestFunction *> functions) {
  uint8_t zoneID = static_cast<uint8_t>(zone);
#ifndef NDEBUG
  for (const auto &zone : requestFunctionsByZone) {
    assert(zone.first != zoneID);
  }
#endif

  requestFunctionsByZone.push_back({zoneID, functions});
}

Evaluator::Evaluator(DiagnosticEngine &diags, const LangOptions &opts)
    : diags(diags),
      debugDumpCycles(opts.DebugDumpCycles),
      recorder(opts.RecordRequestReferences) {}

bool Evaluator::checkDependency(const ActiveRequest &request) {
  // Record this as an active request.
  if (activeRequests.insert(request)) {
    return false;
  }

  // Diagnose cycle.
  diagnoseCycle(request);
  return true;
}

void Evaluator::finishedRequest(const ActiveRequest &request) {
  assert(activeRequests.back() == request);
  activeRequests.pop_back();
}

void Evaluator::diagnoseCycle(const ActiveRequest &request) {
  if (debugDumpCycles) {
    const auto printIndent = [](llvm::raw_ostream &OS, unsigned indent) {
      OS.indent(indent);
      OS << "`--";
    };

    unsigned indent = 1;
    auto &OS = llvm::errs();

    OS << "===CYCLE DETECTED===\n";
    for (const auto &step : activeRequests) {
      printIndent(OS, indent);
      if (step == request) {
        OS.changeColor(llvm::raw_ostream::GREEN);
        simple_display(OS, step);
        OS.resetColor();
      } else {
        simple_display(OS, step);
      }
      OS << "\n";
      indent += 4;
    }

    printIndent(OS, indent);
    OS.changeColor(llvm::raw_ostream::GREEN);
    simple_display(OS, request);

    OS.changeColor(llvm::raw_ostream::RED);
    OS << " (cyclic dependency)";
    OS.resetColor();

    OS << "\n";
  }

  request.diagnoseCycle(diags);
  for (const auto &step : llvm::reverse(activeRequests)) {
    if (step == request) return;

    step.noteCycleStep(diags);
  }

  llvm_unreachable("Diagnosed a cycle but it wasn't represented in the stack");
}

void evaluator::DependencyRecorder::recordDependency(
    const DependencyCollector::Reference &ref) {
  if (activeRequestReferences.empty())
    return;

  activeRequestReferences.back().insert(ref);
}

evaluator::DependencyCollector::DependencyCollector(
    evaluator::DependencyRecorder &parent) : parent(parent) {
#ifndef NDEBUG
  assert(!parent.isRecording &&
         "Probably not a good idea to allow nested recording");
  parent.isRecording = true;
#endif
}

evaluator::DependencyCollector::~DependencyCollector() {
#ifndef NDEBUG
  assert(parent.isRecording &&
         "Should have been recording this whole time");
  parent.isRecording = false;
#endif
}

void evaluator::DependencyCollector::addUsedMember(DeclContext *subject,
                                                   DeclBaseName name) {
  assert(subject->isTypeContext());
  return parent.recordDependency(Reference::usedMember(subject, name));
}

void evaluator::DependencyCollector::addPotentialMember(DeclContext *subject) {
  assert(subject->isTypeContext());
  return parent.recordDependency(Reference::potentialMember(subject));
}

void evaluator::DependencyCollector::addTopLevelName(DeclBaseName name) {
  return parent.recordDependency(Reference::topLevel(name));
}

void evaluator::DependencyCollector::addDynamicLookupName(DeclBaseName name) {
  return parent.recordDependency(Reference::dynamic(name));
}

void evaluator::DependencyRecorder::enumerateReferencesInFile(
    const SourceFile *SF, ReferenceEnumerator f) const {
  auto entry = fileReferences.find(SF);
  if (entry == fileReferences.end()) {
    return;
  }

  for (const auto &ref : entry->getSecond()) {
    switch (ref.kind) {
    case DependencyCollector::Reference::Kind::Empty:
    case DependencyCollector::Reference::Kind::Tombstone:
      llvm_unreachable("Cannot enumerate dead reference!");
    case DependencyCollector::Reference::Kind::UsedMember:
    case DependencyCollector::Reference::Kind::PotentialMember:
    case DependencyCollector::Reference::Kind::TopLevel:
    case DependencyCollector::Reference::Kind::Dynamic:
      f(ref);
    }
  }
}
