//===--- Evaluator.h - Request Evaluator ------------------------*- C++ -*-===//
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
// This file defines the Evaluator class that evaluates and caches
// requests.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_EVALUATOR_H
#define SWIFT_AST_EVALUATOR_H

#include "swift/AST/AnyRequest.h"
#include "swift/AST/EvaluatorDependencies.h"
#include "swift/AST/RequestCache.h"
#include "swift/Basic/AnyValue.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/LangOptions.h"
#include "swift/Basic/Statistic.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/PrettyStackTrace.h"
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

namespace llvm {
class raw_ostream;
}

namespace swift {

using llvm::ArrayRef;

class DiagnosticEngine;
class Evaluator;
class UnifiedStatsReporter;

/// An "abstract" request function pointer, which is the storage type
/// used for each of the
using AbstractRequestFunction = void(void);

/// Form the specific request function for the given request type.
template<typename Request>
using RequestFunction =
  typename Request::OutputType(const Request &, Evaluator &);

/// Pretty stack trace handler for an arbitrary request.
template<typename Request>
class PrettyStackTraceRequest : public llvm::PrettyStackTraceEntry {
  const Request &request;

public:
  PrettyStackTraceRequest(const Request &request) : request(request) { }

  void print(llvm::raw_ostream &out) const override {
    out << "While evaluating request ";
    simple_display(out, request);
    out << "\n";
  }
};

/// An llvm::ErrorInfo container for a request in which a cycle was detected
/// and diagnosed.
template <typename Request>
struct CyclicalRequestError : 
  public llvm::ErrorInfo<CyclicalRequestError<Request>> {
public:
  static char ID;
  const Request &request;
  const Evaluator &evaluator;

  CyclicalRequestError(const Request &request, const Evaluator &evaluator)
    : request(request), evaluator(evaluator) {}

  virtual void log(llvm::raw_ostream &out) const override;

  virtual std::error_code convertToErrorCode() const override {
    // This is essentially unused, but is a temporary requirement for
    // llvm::ErrorInfo subclasses.
    llvm_unreachable("shouldn't get std::error_code from CyclicalRequestError");
  }
};

template <typename Request>
char CyclicalRequestError<Request>::ID = '\0';

/// Evaluates a given request or returns a default value if a cycle is detected.
template <typename Request>
typename Request::OutputType
evaluateOrDefault(
  Evaluator &eval, Request req, typename Request::OutputType def) {
  auto result = eval(req);
  if (auto err = result.takeError()) {
    llvm::handleAllErrors(std::move(err),
      [](const CyclicalRequestError<Request> &E) {
        // cycle detected
      });
    return def;
  }
  return *result;
}

/// Report that a request of the given kind is being evaluated, so it
/// can be recorded by the stats reporter.
template<typename Request>
void reportEvaluatedRequest(UnifiedStatsReporter &stats,
                            const Request &request) { }

/// Evaluation engine that evaluates and caches "requests", checking for cyclic
/// dependencies along the way.
///
/// Each request is a function object that accepts a reference to the evaluator
/// itself (through which it can request other values) and produces a
/// value. That value can then be cached by the evaluator for subsequent access,
/// using a policy dictated by the request itself.
///
/// The evaluator keeps track of all in-flight requests so that it can detect
/// and diagnose cyclic dependencies.
///
/// Each request should be its own function object, supporting the following
/// API:
///
///   - Copy constructor
///   - Equality operator (==)
///   - Hashing support (hash_value)
///   - TypeID support (see swift/Basic/TypeID.h)
///   - The output type (described via a nested type OutputType), which
///     must itself by a value type that supports TypeID.
///   - Evaluation via the function call operator:
///       OutputType operator()(Evaluator &evaluator) const;
///   - Cycle breaking and diagnostics operations:
///
///       void diagnoseCycle(DiagnosticEngine &diags) const;
///       void noteCycleStep(DiagnosticEngine &diags) const;
///   - Caching policy:
///
///     static const bool isEverCached;
///
///       When false, the request's result will never be cached. When true,
///       the result will be cached on completion. How it is cached depends on
///       the following.
///
///     bool isCached() const;
///
///       Dynamically indicates whether to cache this particular instance of the
///       request, so that (for example) requests for which a quick check
///       usually suffices can avoid caching a trivial result.
///
///     static const bool hasExternalCache;
///
///       When false, the results will be cached within the evaluator and
///       cannot be accessed except through the evaluator. This is the
///       best approach, because it ensures that all accesses to the result
///       are tracked.
///
///       When true, the request itself must provide an way to cache the
///       results, e.g., in some external data structure. External caching
///       should only be used when staging in the use of the evaluator into
///       existing mutable data structures; new computations should not depend
///       on it. Externally-cached requests must provide additional API:
///
///         Optional<OutputType> getCachedResult() const;
///
///           Retrieve the cached result, or \c None if there is no such
///           result.
///
///         void cacheResult(OutputType value) const;
///
///            Cache the given result.
class Evaluator {
  /// The diagnostics engine through which any cyclic-dependency
  /// diagnostics will be emitted.
  DiagnosticEngine &diags;

  /// Whether to dump detailed debug info for cycles.
  bool debugDumpCycles;

  /// Used to report statistics about which requests were evaluated, if
  /// non-null.
  UnifiedStatsReporter *stats = nullptr;

  /// A vector containing the abstract request functions that can compute
  /// the result of a particular request within a given zone. The
  /// \c uint8_t is the zone number of the request, and the array is
  /// indexed by the index of the request type within that zone. Each
  /// entry is a function pointer that will be reinterpret_cast'd to
  ///
  ///   RequestType::OutputType (*)(const RequestType &request,
  ///                               Evaluator &evaluator);
  /// and called to satisfy the request.
  std::vector<std::pair<uint8_t, ArrayRef<AbstractRequestFunction *>>>
    requestFunctionsByZone;

  /// A vector containing all of the active evaluation requests, which
  /// is treated as a stack and is used to detect cycles.
  llvm::SetVector<ActiveRequest> activeRequests;

  /// A cache that stores the results of requests.
  evaluator::RequestCache cache;

  evaluator::DependencyRecorder recorder;

  /// Retrieve the request function for the given zone and request IDs.
  AbstractRequestFunction *getAbstractRequestFunction(uint8_t zoneID,
                                                      uint8_t requestID) const;

  /// Retrieve the request function for the given request type.
  template<typename Request>
  auto getRequestFunction() const -> RequestFunction<Request> * {
    auto abstractFn = getAbstractRequestFunction(TypeID<Request>::zoneID,
                                                 TypeID<Request>::localID);
    assert(abstractFn && "No request function for request");
    return reinterpret_cast<RequestFunction<Request> *>(abstractFn);
  }

public:
  /// Construct a new evaluator that can emit cyclic-dependency
  /// diagnostics through the given diagnostics engine.
  Evaluator(DiagnosticEngine &diags, const LangOptions &opts);

  /// Emit GraphViz output visualizing the request graph.
  void emitRequestEvaluatorGraphViz(llvm::StringRef graphVizPath);

  /// Set the unified stats reporter through which evaluated-request
  /// statistics will be recorded.
  void setStatsReporter(UnifiedStatsReporter *stats) { this->stats = stats; }

  /// Register the set of request functions for the given zone.
  ///
  /// These functions will be called to evaluate any requests within that
  /// zone.
  void registerRequestFunctions(Zone zone,
                                ArrayRef<AbstractRequestFunction *> functions);

  void enumerateReferencesInFile(
      const SourceFile *SF,
      evaluator::DependencyRecorder::ReferenceEnumerator f) const {
    return recorder.enumerateReferencesInFile(SF, f);
  }

  /// Retrieve the result produced by evaluating a request that can
  /// be cached.
  template<typename Request,
           typename std::enable_if<Request::isEverCached>::type * = nullptr>
  llvm::Expected<typename Request::OutputType>
  operator()(const Request &request) {
    // The request can be cached, but check a predicate to determine
    // whether this particular instance is cached. This allows more
    // fine-grained control over which instances get cache.
    if (request.isCached())
      return getResultCached(request);

    return getResultUncached(request);
  }

  /// Retrieve the result produced by evaluating a request that
  /// will never be cached.
  template<typename Request,
           typename std::enable_if<!Request::isEverCached>::type * = nullptr>
  llvm::Expected<typename Request::OutputType>
  operator()(const Request &request) {
    return getResultUncached(request);
  }

  /// Evaluate a set of requests and return their results as a tuple.
  ///
  /// Use this to describe cases where there are multiple (known)
  /// requests that all need to be satisfied.
  template<typename ...Requests>
  std::tuple<llvm::Expected<typename Requests::OutputType>...>
  operator()(const Requests &...requests) {
    return std::tuple<llvm::Expected<typename Requests::OutputType>...>(
      (*this)(requests)...);
  }

  /// Cache a precomputed value for the given request, so that it will not
  /// be computed.
  template<typename Request,
           typename std::enable_if<Request::hasExternalCache>::type* = nullptr>
  void cacheOutput(const Request &request,
                   typename Request::OutputType &&output) {
    request.cacheResult(std::move(output));
  }

  /// Cache a precomputed value for the given request, so that it will not
  /// be computed.
  template<typename Request,
           typename std::enable_if<!Request::hasExternalCache>::type* = nullptr>
  void cacheOutput(const Request &request,
                   typename Request::OutputType &&output) {
    cache.insert<Request>(request, std::move(output));
  }

  template<typename Request,
           typename std::enable_if<!Request::hasExternalCache>::type* = nullptr>
  bool hasCachedResult(const Request &request) {
    return cache.find_as(request) != cache.end<Request>();
  }

  /// Do not introduce new callers of this function.
  template<typename Request,
           typename std::enable_if<!Request::hasExternalCache>::type* = nullptr>
  void clearCachedOutput(const Request &request) {
    cache.erase<Request>(request);
    recorder.clearRequest<Request>(request);
  }

  /// Clear the cache stored within this evaluator.
  ///
  /// Note that this does not clear the caches of requests that use external
  /// caching.
  void clearCache() { cache.clear(); }

  /// Is the given request, or an equivalent, currently being evaluated?
  template <typename Request>
  bool hasActiveRequest(const Request &request) const {
    return activeRequests.count(ActiveRequest(request));
  }

private:
  /// Diagnose a cycle detected in the evaluation of the given
  /// request.
  void diagnoseCycle(const ActiveRequest &request);

  /// Check the dependency from the current top of the stack to
  /// the given request, including cycle detection and diagnostics.
  ///
  /// \returns true if a cycle was detected, in which case this function has
  /// already diagnosed the cycle. Otherwise, returns \c false and adds this
  /// request to the \c activeRequests stack.
  bool checkDependency(const ActiveRequest &request);

  /// Note that we have finished this request, popping it from the
  /// \c activeRequests stack.
  void finishedRequest(const ActiveRequest &request);

  /// Produce the result of the request without caching.
  template<typename Request>
  llvm::Expected<typename Request::OutputType>
  getResultUncached(const Request &request) {
    auto activeReq = ActiveRequest(request);

    // Check for a cycle.
    if (checkDependency(activeReq)) {
      return llvm::Error(
          std::make_unique<CyclicalRequestError<Request>>(request, *this));
    }

    PrettyStackTraceRequest<Request> prettyStackTrace(request);

    // Trace and/or count statistics.
    FrontendStatsTracer statsTracer = make_tracer(stats, request);
    if (stats) reportEvaluatedRequest(*stats, request);

    recorder.beginRequest<Request>();

    auto &&result = getRequestFunction<Request>()(request, *this);

    recorder.endRequest<Request>(request);

    handleDependencySourceRequest<Request>(request);
    handleDependencySinkRequest<Request>(request, result);

    // Make sure we remove this from the set of active requests once we're
    // done.
    finishedRequest(activeReq);

    return std::move(result);
  }

  /// Get the result of a request, consulting an external cache
  /// provided by the request to retrieve previously-computed results
  /// and detect recursion.
  template<typename Request,
           typename std::enable_if<Request::hasExternalCache>::type * = nullptr>
  llvm::Expected<typename Request::OutputType>
  getResultCached(const Request &request) {
    // If there is a cached result, return it.
    if (auto cached = request.getCachedResult()) {
      recorder.replayCachedRequest(request);
      handleDependencySinkRequest<Request>(request, *cached);
      return *cached;
    }

    // Compute the result.
    auto result = getResultUncached(request);

    // Cache the result if applicable.
    if (!result)
      return result;

    request.cacheResult(*result);

    // Return it.
    return result;
  }

  /// Get the result of a request, consulting the general cache to
  /// retrieve previously-computed results and detect recursion.
  template<
      typename Request,
      typename std::enable_if<!Request::hasExternalCache>::type * = nullptr>
  llvm::Expected<typename Request::OutputType>
  getResultCached(const Request &request) {
    // If we already have an entry for this request in the cache, return it.
    auto known = cache.find_as<Request>(request);
    if (known != cache.end<Request>()) {
      auto result = known->second;
      recorder.replayCachedRequest(request);
      handleDependencySinkRequest<Request>(request, result);
      return result;
    }

    // Compute the result.
    auto result = getResultUncached(request);
    if (!result)
      return result;

    // Cache the result.
    cache.insert<Request>(request, *result);
    return result;
  }

private:
  template <typename Request, typename std::enable_if<
                                  !Request::isDependencySink>::type * = nullptr>
  void handleDependencySinkRequest(const Request &r,
                                   const typename Request::OutputType &o) {}

  template <typename Request,
            typename std::enable_if<Request::isDependencySink>::type * = nullptr>
  void handleDependencySinkRequest(const Request &r,
                                   const typename Request::OutputType &o) {
    evaluator::DependencyCollector collector(recorder);
    r.writeDependencySink(collector, o);
  }

  template <typename Request, typename std::enable_if<
                                  !Request::isDependencySource>::type * = nullptr>
  void handleDependencySourceRequest(const Request &r) {}

  template <typename Request,
            typename std::enable_if<Request::isDependencySource>::type * = nullptr>
  void handleDependencySourceRequest(const Request &r) {
    auto source = r.readDependencySource(recorder);
    if (!source.isNull() && source.get()->isPrimary()) {
      recorder.handleDependencySourceRequest(r, source.get());
    }
  }
};

template <typename Request>
void CyclicalRequestError<Request>::log(llvm::raw_ostream &out) const {
  out << "Cycle detected:\n";
  simple_display(out, request);
  out << "\n";
}

} // end namespace evaluator

#endif // SWIFT_AST_EVALUATOR_H
