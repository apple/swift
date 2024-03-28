//===--- DependencyScan.h - C API for Swift Dependency Scanning ---*- C -*-===//
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
// This C API is primarily intended to serve as the Swift Driver's
// dependency scanning facility (https://github.com/apple/swift-driver).
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_C_DEPENDENCY_SCAN_H
#define SWIFT_C_DEPENDENCY_SCAN_H

#include "DependencyScanMacros.h"
#include "swift-c/CommonString/CommonString.h"

/// The version constants for the SwiftDependencyScan C API.
/// SWIFTSCAN_VERSION_MINOR should increase when there are API additions.
/// SWIFTSCAN_VERSION_MAJOR is intended for "major" source/ABI breaking changes.
#define SWIFTSCAN_VERSION_MAJOR 0
#define SWIFTSCAN_VERSION_MINOR 8

SWIFTSCAN_BEGIN_DECLS

//=== Public Scanner Data Types -------------------------------------------===//

typedef enum {
  // This dependency info encodes two ModuleDependencyKind types:
  // SwiftInterface and SwiftSource.
  SWIFTSCAN_DEPENDENCY_INFO_SWIFT_TEXTUAL = 0,
  SWIFTSCAN_DEPENDENCY_INFO_SWIFT_BINARY = 1,
  SWIFTSCAN_DEPENDENCY_INFO_SWIFT_PLACEHOLDER = 2,
  SWIFTSCAN_DEPENDENCY_INFO_CLANG = 3
} swiftscan_dependency_info_kind_t;

/// Opaque container of the details specific to a given module dependency.
typedef struct swiftscan_module_details_s *swiftscan_module_details_t;

/// Opaque container to a dependency info of a given module.
typedef struct swiftscan_dependency_info_s *swiftscan_dependency_info_t;

/// Opaque container to an overall result of a dependency scan.
typedef struct swiftscan_dependency_graph_s *swiftscan_dependency_graph_t;

/// Opaque container to contain the result of a dependency prescan.
typedef struct swiftscan_import_set_s *swiftscan_import_set_t;

/// Opaque container to contain the info of a diagnostics emitted by the scanner.
typedef struct swiftscan_diagnostic_info_s *swiftscan_diagnostic_info_t;

/// Full Dependency Graph (Result)
typedef struct {
  swiftscan_dependency_info_t *modules;
  size_t count;
} swiftscan_dependency_set_t;

typedef enum {
  SWIFTSCAN_DIAGNOSTIC_SEVERITY_ERROR = 0,
  SWIFTSCAN_DIAGNOSTIC_SEVERITY_WARNING = 1,
  SWIFTSCAN_DIAGNOSTIC_SEVERITY_NOTE = 2,
  SWIFTSCAN_DIAGNOSTIC_SEVERITY_REMARK = 3
} swiftscan_diagnostic_severity_t;

typedef struct {
  swiftscan_diagnostic_info_t *diagnostics;
  size_t count;
} swiftscan_diagnostic_set_t;

//=== Batch Scan Input Specification --------------------------------------===//

/// Opaque container to a container of batch scan entry information.
typedef struct swiftscan_batch_scan_entry_s *swiftscan_batch_scan_entry_t;

typedef struct {
  swiftscan_batch_scan_entry_t *modules;
  size_t count;
} swiftscan_batch_scan_input_t;

typedef struct {
  swiftscan_dependency_graph_t *results;
  size_t count;
} swiftscan_batch_scan_result_t;

//=== Scanner Invocation Specification ------------------------------------===//

/// Opaque container of all relevant context required to launch a dependency
/// scan (command line arguments, working directory, etc.)
typedef struct swiftscan_scan_invocation_s *swiftscan_scan_invocation_t;

//=== Dependency Result Functions -----------------------------------------===//

SWIFTSCAN_PUBLIC swiftscan_string_ref_t
swiftscan_dependency_graph_get_main_module_name(
    swiftscan_dependency_graph_t result);

SWIFTSCAN_PUBLIC swiftscan_dependency_set_t *
swiftscan_dependency_graph_get_dependencies(
    swiftscan_dependency_graph_t result);

// Return value disposed of together with the dependency_graph
// using `swiftscan_dependency_graph_dispose`
SWIFTSCAN_PUBLIC swiftscan_diagnostic_set_t *
swiftscan_dependency_graph_get_diagnostics(
    swiftscan_dependency_graph_t result);

//=== Dependency Module Info Functions ------------------------------------===//

SWIFTSCAN_PUBLIC swiftscan_string_ref_t
swiftscan_module_info_get_module_name(swiftscan_dependency_info_t info);

SWIFTSCAN_PUBLIC swiftscan_string_ref_t
swiftscan_module_info_get_module_path(swiftscan_dependency_info_t info);

SWIFTSCAN_PUBLIC swiftscan_string_set_t *
swiftscan_module_info_get_source_files(swiftscan_dependency_info_t info);

SWIFTSCAN_PUBLIC swiftscan_string_set_t *
swiftscan_module_info_get_direct_dependencies(swiftscan_dependency_info_t info);

SWIFTSCAN_PUBLIC swiftscan_module_details_t
swiftscan_module_info_get_details(swiftscan_dependency_info_t info);

//=== Dependency Module Info Details Functions ----------------------------===//

SWIFTSCAN_PUBLIC swiftscan_dependency_info_kind_t
swiftscan_module_detail_get_kind(swiftscan_module_details_t details);

//=== Swift Textual Module Details query APIs -----------------------------===//
SWIFTSCAN_PUBLIC swiftscan_string_ref_t
swiftscan_swift_textual_detail_get_module_interface_path(
    swiftscan_module_details_t details);

SWIFTSCAN_PUBLIC swiftscan_string_set_t *
swiftscan_swift_textual_detail_get_compiled_module_candidates(
    swiftscan_module_details_t details);

SWIFTSCAN_PUBLIC swiftscan_string_ref_t
swiftscan_swift_textual_detail_get_bridging_header_path(
    swiftscan_module_details_t details);

SWIFTSCAN_PUBLIC swiftscan_string_set_t *
swiftscan_swift_textual_detail_get_bridging_source_files(
    swiftscan_module_details_t details);

SWIFTSCAN_PUBLIC swiftscan_string_set_t *
swiftscan_swift_textual_detail_get_bridging_module_dependencies(
    swiftscan_module_details_t details);

SWIFTSCAN_PUBLIC swiftscan_string_set_t *
swiftscan_swift_textual_detail_get_command_line(
    swiftscan_module_details_t details);

SWIFTSCAN_PUBLIC swiftscan_string_set_t *
swiftscan_swift_textual_detail_get_bridging_pch_command_line(
    swiftscan_module_details_t details);

SWIFTSCAN_PUBLIC swiftscan_string_set_t *
swiftscan_swift_textual_detail_get_extra_pcm_args(
    swiftscan_module_details_t details);

SWIFTSCAN_PUBLIC swiftscan_string_ref_t
swiftscan_swift_textual_detail_get_context_hash(
    swiftscan_module_details_t details);

SWIFTSCAN_PUBLIC bool swiftscan_swift_textual_detail_get_is_framework(
    swiftscan_module_details_t details);

SWIFTSCAN_PUBLIC swiftscan_string_set_t *
swiftscan_swift_textual_detail_get_swift_overlay_dependencies(
    swiftscan_module_details_t details);

SWIFTSCAN_PUBLIC swiftscan_string_ref_t
swiftscan_swift_textual_detail_get_cas_fs_root_id(
    swiftscan_module_details_t details);

SWIFTSCAN_PUBLIC swiftscan_string_ref_t
swiftscan_swift_textual_detail_get_module_cache_key(
    swiftscan_module_details_t details);

//=== Swift Binary Module Details query APIs ------------------------------===//

SWIFTSCAN_PUBLIC swiftscan_string_ref_t
swiftscan_swift_binary_detail_get_compiled_module_path(
    swiftscan_module_details_t details);

SWIFTSCAN_PUBLIC swiftscan_string_ref_t
swiftscan_swift_binary_detail_get_module_doc_path(
    swiftscan_module_details_t details);

SWIFTSCAN_PUBLIC swiftscan_string_ref_t
swiftscan_swift_binary_detail_get_module_source_info_path(
    swiftscan_module_details_t details);

SWIFTSCAN_PUBLIC swiftscan_string_set_t *
swiftscan_swift_binary_detail_get_swift_overlay_dependencies(
    swiftscan_module_details_t details);

SWIFTSCAN_PUBLIC swiftscan_string_ref_t
swiftscan_swift_binary_detail_get_header_dependency(
    swiftscan_module_details_t details);

SWIFTSCAN_PUBLIC swiftscan_string_set_t *
swiftscan_swift_binary_detail_get_header_dependency_module_dependencies(
    swiftscan_module_details_t details);

SWIFTSCAN_PUBLIC bool
swiftscan_swift_binary_detail_get_is_framework(
    swiftscan_module_details_t details);

SWIFTSCAN_PUBLIC swiftscan_string_ref_t
swiftscan_swift_binary_detail_get_module_cache_key(
    swiftscan_module_details_t details);

//=== Swift Placeholder Module Details query APIs -------------------------===//

SWIFTSCAN_PUBLIC swiftscan_string_ref_t
swiftscan_swift_placeholder_detail_get_compiled_module_path(
    swiftscan_module_details_t details);

SWIFTSCAN_PUBLIC swiftscan_string_ref_t
swiftscan_swift_placeholder_detail_get_module_doc_path(
    swiftscan_module_details_t details);

SWIFTSCAN_PUBLIC swiftscan_string_ref_t
swiftscan_swift_placeholder_detail_get_module_source_info_path(
    swiftscan_module_details_t details);

//=== Clang Module Details query APIs -------------------------------------===//

SWIFTSCAN_PUBLIC swiftscan_string_ref_t
swiftscan_clang_detail_get_module_map_path(swiftscan_module_details_t details);

SWIFTSCAN_PUBLIC swiftscan_string_ref_t
swiftscan_clang_detail_get_context_hash(swiftscan_module_details_t details);

SWIFTSCAN_PUBLIC swiftscan_string_set_t *
swiftscan_clang_detail_get_command_line(swiftscan_module_details_t details);

SWIFTSCAN_PUBLIC swiftscan_string_set_t *
swiftscan_clang_detail_get_captured_pcm_args(swiftscan_module_details_t details);

SWIFTSCAN_PUBLIC swiftscan_string_ref_t
swiftscan_clang_detail_get_cas_fs_root_id(swiftscan_module_details_t details);

SWIFTSCAN_PUBLIC swiftscan_string_ref_t
swiftscan_clang_detail_get_module_cache_key(swiftscan_module_details_t details);

//=== Batch Scan Input Functions ------------------------------------------===//

/// Create an \c swiftscan_batch_scan_input_t instance.
/// The returned \c swiftscan_batch_scan_input_t is owned by the caller and must be disposed
/// of using \c swiftscan_batch_scan_input_dispose .
SWIFTSCAN_PUBLIC swiftscan_batch_scan_input_t *
swiftscan_batch_scan_input_create();

SWIFTSCAN_PUBLIC void
swiftscan_batch_scan_input_set_modules(swiftscan_batch_scan_input_t *input,
                                       int count,
                                       swiftscan_batch_scan_entry_t *modules);

//=== Batch Scan Entry Functions ------------------------------------------===//

/// Create an \c swiftscan_batch_scan_entry_t instance.
/// The returned \c swiftscan_batch_scan_entry_t is owned by the caller and must be disposed
/// of using \c swiftscan_batch_scan_entry_dispose .
SWIFTSCAN_PUBLIC swiftscan_batch_scan_entry_t
swiftscan_batch_scan_entry_create();

SWIFTSCAN_PUBLIC void
swiftscan_batch_scan_entry_set_module_name(swiftscan_batch_scan_entry_t entry,
                                           const char *name);

SWIFTSCAN_PUBLIC void
swiftscan_batch_scan_entry_set_arguments(swiftscan_batch_scan_entry_t entry,
                                         const char *arguments);

SWIFTSCAN_PUBLIC void
swiftscan_batch_scan_entry_set_is_swift(swiftscan_batch_scan_entry_t entry,
                                        bool is_swift);

SWIFTSCAN_PUBLIC swiftscan_string_ref_t
swiftscan_batch_scan_entry_get_module_name(swiftscan_batch_scan_entry_t entry);

SWIFTSCAN_PUBLIC swiftscan_string_ref_t
swiftscan_batch_scan_entry_get_arguments(swiftscan_batch_scan_entry_t entry);

SWIFTSCAN_PUBLIC bool
swiftscan_batch_scan_entry_get_is_swift(swiftscan_batch_scan_entry_t entry);

//=== Prescan Result Functions --------------------------------------------===//

SWIFTSCAN_PUBLIC swiftscan_string_set_t *
swiftscan_import_set_get_imports(swiftscan_import_set_t result);

// Return value disposed of together with the dependency_graph
// using `swiftscan_import_set_dispose`
SWIFTSCAN_PUBLIC swiftscan_diagnostic_set_t *
swiftscan_import_set_get_diagnostics(swiftscan_import_set_t result);

//=== Scanner Invocation Functions ----------------------------------------===//

/// Create an \c swiftscan_scan_invocation_t instance.
/// The returned \c swiftscan_scan_invocation_t is owned by the caller and must be disposed
/// of using \c swiftscan_scan_invocation_dispose .
SWIFTSCAN_PUBLIC swiftscan_scan_invocation_t swiftscan_scan_invocation_create();

SWIFTSCAN_PUBLIC void swiftscan_scan_invocation_set_working_directory(
    swiftscan_scan_invocation_t invocation, const char *working_directory);

SWIFTSCAN_PUBLIC void
swiftscan_scan_invocation_set_argv(swiftscan_scan_invocation_t invocation,
                                   int argc, const char **argv);

SWIFTSCAN_PUBLIC swiftscan_string_ref_t
swiftscan_scan_invocation_get_working_directory(
    swiftscan_scan_invocation_t invocation);

SWIFTSCAN_PUBLIC int
swiftscan_scan_invocation_get_argc(swiftscan_scan_invocation_t invocation);

SWIFTSCAN_PUBLIC swiftscan_string_set_t *
swiftscan_scan_invocation_get_argv(swiftscan_scan_invocation_t invocation);

//=== Cleanup Functions ---------------------------------------------------===//

SWIFTSCAN_PUBLIC void
swiftscan_string_set_dispose(swiftscan_string_set_t *set);

SWIFTSCAN_PUBLIC void
swiftscan_string_dispose(swiftscan_string_ref_t string);

SWIFTSCAN_PUBLIC void
swiftscan_dependency_graph_dispose(swiftscan_dependency_graph_t result);

SWIFTSCAN_PUBLIC void
swiftscan_import_set_dispose(swiftscan_import_set_t result);

SWIFTSCAN_PUBLIC void
swiftscan_batch_scan_entry_dispose(swiftscan_batch_scan_entry_t entry);

SWIFTSCAN_PUBLIC void
swiftscan_batch_scan_input_dispose(swiftscan_batch_scan_input_t *input);

SWIFTSCAN_PUBLIC void
swiftscan_batch_scan_result_dispose(swiftscan_batch_scan_result_t *result);

SWIFTSCAN_PUBLIC void
swiftscan_scan_invocation_dispose(swiftscan_scan_invocation_t invocation);

//=== Feature-Query Functions ---------------------------------------------===//
SWIFTSCAN_PUBLIC swiftscan_string_set_t *
swiftscan_compiler_supported_arguments_query();

SWIFTSCAN_PUBLIC swiftscan_string_set_t *
swiftscan_compiler_supported_features_query();

//=== Target-Info Functions -----------------------------------------------===//
SWIFTSCAN_PUBLIC swiftscan_string_ref_t
swiftscan_compiler_target_info_query(swiftscan_scan_invocation_t invocation);
SWIFTSCAN_PUBLIC swiftscan_string_ref_t
swiftscan_compiler_target_info_query_v2(swiftscan_scan_invocation_t invocation,
                                        const char *main_executable_path);

//=== Scanner Functions ---------------------------------------------------===//

/// Container of the configuration state and shared cache for dependency
/// scanning.
typedef void *swiftscan_scanner_t;

/// Create an \c swiftscan_scanner_t instance.
/// The returned \c swiftscan_scanner_t is owned by the caller and must be disposed
/// of using \c swiftscan_scanner_dispose .
SWIFTSCAN_PUBLIC swiftscan_scanner_t swiftscan_scanner_create(void);
SWIFTSCAN_PUBLIC void swiftscan_scanner_dispose(swiftscan_scanner_t);

/// Invoke a dependency scan using arguments specified in the \c
/// swiftscan_scan_invocation_t argument. The returned \c
/// swiftscan_dependency_graph_t is owned by the caller and must be disposed of
/// using \c swiftscan_dependency_graph_dispose .
SWIFTSCAN_PUBLIC swiftscan_dependency_graph_t swiftscan_dependency_graph_create(
    swiftscan_scanner_t scanner, swiftscan_scan_invocation_t invocation);

/// Invoke the scan for an input batch of modules specified in the
/// \c swiftscan_batch_scan_input_t argument. The returned
/// \c swiftscan_batch_scan_result_t is owned by the caller and must be disposed
/// of using \c swiftscan_batch_scan_result_dispose .
SWIFTSCAN_PUBLIC swiftscan_batch_scan_result_t *
swiftscan_batch_scan_result_create(swiftscan_scanner_t scanner,
                                   swiftscan_batch_scan_input_t *batch_input,
                                   swiftscan_scan_invocation_t invocation);

/// Invoke the import prescan using arguments specified in the \c
/// swiftscan_scan_invocation_t argument. The returned \c swiftscan_import_set_t
/// is owned by the caller and must be disposed of using \c
/// swiftscan_import_set_dispose .
SWIFTSCAN_PUBLIC swiftscan_import_set_t swiftscan_import_set_create(
    swiftscan_scanner_t scanner, swiftscan_scan_invocation_t invocation);


//=== Scanner Diagnostics -------------------------------------------------===//
/// For the specified \c scanner instance, query all insofar emitted diagnostics
SWIFTSCAN_PUBLIC swiftscan_diagnostic_set_t*
swiftscan_scanner_diagnostics_query(swiftscan_scanner_t scanner);

/// For the specified \c scanner instance, reset its diagnostic state
SWIFTSCAN_PUBLIC void
swiftscan_scanner_diagnostics_reset(swiftscan_scanner_t scanner);

SWIFTSCAN_PUBLIC swiftscan_string_ref_t
swiftscan_diagnostic_get_message(swiftscan_diagnostic_info_t diagnostic);

SWIFTSCAN_PUBLIC swiftscan_diagnostic_severity_t
swiftscan_diagnostic_get_severity(swiftscan_diagnostic_info_t diagnostic);

SWIFTSCAN_PUBLIC void
swiftscan_diagnostics_set_dispose(swiftscan_diagnostic_set_t* diagnostics);

//=== Scanner Cache Operations --------------------------------------------===//
// The following operations expose an implementation detail of the dependency
// scanner: its module dependencies cache. This is done in order
// to allow clients to perform incremental dependency scans by having the
// scanner's state be serializable and re-usable.

/// For the specified \c scanner instance, serialize its state to the specified file-system \c path .
SWIFTSCAN_PUBLIC void
swiftscan_scanner_cache_serialize(swiftscan_scanner_t scanner,
                                  const char * path);

/// For the specified \c scanner instance, load in scanner state from a file at
/// the specified file-system \c path .
SWIFTSCAN_PUBLIC bool
swiftscan_scanner_cache_load(swiftscan_scanner_t scanner,
                             const char * path);

/// For the specified \c scanner instance, reset its internal state, ensuring subsequent
/// scanning queries are done "from-scratch".
SWIFTSCAN_PUBLIC void
swiftscan_scanner_cache_reset(swiftscan_scanner_t scanner);

/// An entry point to invoke the compiler via a library call.
SWIFTSCAN_PUBLIC int invoke_swift_compiler(int argc, const char **argv);

//=== Scanner CAS Operations ----------------------------------------------===//

/// Opaque container for a CASOptions that describe how CAS should be created.
typedef struct swiftscan_cas_options_s *swiftscan_cas_options_t;

/// Opaque container for a CAS instance that includes both ObjectStore and
/// ActionCache.
typedef struct swiftscan_cas_s *swiftscan_cas_t;

/// Opaque container for a cached compilation.
typedef struct swiftscan_cached_compilation_s *swiftscan_cached_compilation_t;

/// Opaque container for a cached compilation output.
typedef struct swiftscan_cached_output_s *swiftscan_cached_output_t;

/// Opaque type for a cache replay instance.
typedef struct swiftscan_cache_replay_instance_s
    *swiftscan_cache_replay_instance_t;

/// Opaque container for a cached compilation replay result.
typedef struct swiftscan_cache_replay_result_s *swiftscan_cache_replay_result_t;

/// Opaque type for a cancellation token for async cache operations.
typedef struct swiftscan_cache_cancellation_token_s
    *swiftscan_cache_cancellation_token_t;

/// Create a \c CASOptions for creating CAS inside scanner specified.
SWIFTSCAN_PUBLIC swiftscan_cas_options_t swiftscan_cas_options_create(void);

/// Dispose \c CASOptions.
SWIFTSCAN_PUBLIC void
swiftscan_cas_options_dispose(swiftscan_cas_options_t options);

/// Set on-disk path for the \c cas.
SWIFTSCAN_PUBLIC void
swiftscan_cas_options_set_ondisk_path(swiftscan_cas_options_t options,
                                      const char *path);

/// Set plugin path for the \c cas.
SWIFTSCAN_PUBLIC void
swiftscan_cas_options_set_plugin_path(swiftscan_cas_options_t options,
                                      const char *path);

/// Set option using a name/value pair. Return true if error.
/// If error happens, the error message is returned via `error` parameter, and
/// caller needs to free the error message via `swiftscan_string_dispose`.
SWIFTSCAN_PUBLIC bool
swiftscan_cas_options_set_plugin_option(swiftscan_cas_options_t options,
                                        const char *name, const char *value,
                                        swiftscan_string_ref_t *error);

/// Create a \c cas instance from plugin. Return NULL if error.
/// If error happens, the error message is returned via `error` parameter, and
/// caller needs to free the error message via `swiftscan_string_dispose`.
SWIFTSCAN_PUBLIC swiftscan_cas_t swiftscan_cas_create_from_options(
    swiftscan_cas_options_t options, swiftscan_string_ref_t *error);

/// Store content into CAS. Return \c CASID as string. Return NULL on error.
/// If error happens, the error message is returned via `error` parameter, and
/// caller needs to free the error message via `swiftscan_string_dispose`.
SWIFTSCAN_PUBLIC swiftscan_string_ref_t
swiftscan_cas_store(swiftscan_cas_t cas, uint8_t *data, unsigned size,
                    swiftscan_string_ref_t *error);

/// Get the local storage size for the CAS in bytes. Return the local storage
/// size of the CAS/cache data, or -1 if the implementation does not support
/// reporting such size, or -2 if an error occurred.
/// If error happens, the error message is returned via `error` parameter, and
/// caller needs to free the error message via `swiftscan_string_dispose`.
SWIFTSCAN_PUBLIC int64_t
swiftscan_cas_get_ondisk_size(swiftscan_cas_t, swiftscan_string_ref_t *error);

/// Set the size for the limiting disk storage size for CAS. \c size_limit is
/// the maximum size limit in bytes (0 means no limit, negative is invalid).
/// Return true if error. If error happens, the error message is returned via
/// `error` parameter, and caller needs to free the error message via
/// `swiftscan_string_dispose`.
SWIFTSCAN_PUBLIC bool
swiftscan_cas_set_ondisk_size_limit(swiftscan_cas_t, int64_t size_limit,
                                    swiftscan_string_ref_t *error);

/// Prune local CAS storage according to the size limit. Return true if error.
/// If error happens, the error message is returned via `error` parameter, and
/// caller needs to free the error message via `swiftscan_string_dispose`.
SWIFTSCAN_PUBLIC bool
swiftscan_cas_prune_ondisk_data(swiftscan_cas_t, swiftscan_string_ref_t *error);

/// Dispose the \c cas instance.
SWIFTSCAN_PUBLIC void swiftscan_cas_dispose(swiftscan_cas_t cas);

/// Compute \c CacheKey for the outputs of a primary input file from a compiler
/// invocation with command-line \c argc and \c argv. When primary input file
/// is not available for compilation, e.g., using WMO, primary file is the first
/// swift input on the command-line by convention. Return \c CacheKey as string.
/// If error happens, the error message is returned via `error` parameter, and
/// caller needs to free the error message via `swiftscan_string_dispose`.
/// This API is DEPRECATED and in favor of using
/// `swiftscan_cache_compute_key_from_input_index`.
SWIFTSCAN_PUBLIC swiftscan_string_ref_t
swiftscan_cache_compute_key(swiftscan_cas_t cas, int argc, const char **argv,
                            const char *input, swiftscan_string_ref_t *error);

/// Compute \c CacheKey for the outputs of a primary input file from a compiler
/// invocation with command-line \c argc and \c argv and the index for the
/// input. The index of the input is computed from the position of the input
/// file from all input files. When primary input file is not available for
/// compilation, e.g., using WMO, primary file is the first swift input on the
/// command-line by convention. Return \c CacheKey as string. If error happens,
/// the error message is returned via `error` parameter, and caller needs to
/// free the error message via `swiftscan_string_dispose`.
SWIFTSCAN_PUBLIC swiftscan_string_ref_t
swiftscan_cache_compute_key_from_input_index(swiftscan_cas_t cas, int argc,
                                             const char **argv,
                                             unsigned input_index,
                                             swiftscan_string_ref_t *error);

/// Query the result of the compilation using the output cache key. \c globally
/// suggests if the lookup should check remote cache if such operation exists.
/// Returns the cached compilation of the result if found, or nullptr if output
/// is not found or an error occurs. When an error occurs, the error message is
/// returned via \c error parameter and its caller needs to free the message
/// using `swiftscan_string_dispose`. The returned cached compilation needs to
/// be freed via `swiftscan_cached_compilation_dispose`.
SWIFTSCAN_PUBLIC swiftscan_cached_compilation_t
swiftscan_cache_query(swiftscan_cas_t cas, const char *key, bool globally,
                      swiftscan_string_ref_t *error);

/// Async version of `swiftscan_cache_query` where result is returned via
/// callback. Both cache_result enum and cached compilation will be provided to
/// callback. \c ctx is an opaque value that passed to the callback and \c
/// swiftscan_cache_cancellation_token_t will return an token that can be used
/// to cancel the async operation. The token needs to be freed by caller using
/// `swiftscan_cache_cancellation_token_dispose`. If no token is needed, nullptr
/// can be passed and no token will be returned.
SWIFTSCAN_PUBLIC void swiftscan_cache_query_async(
    swiftscan_cas_t cas, const char *key, bool globally, void *ctx,
    void (*callback)(void *ctx, swiftscan_cached_compilation_t,
                     swiftscan_string_ref_t error),
    swiftscan_cache_cancellation_token_t *);

/// Query the number of outputs from a cached compilation.
SWIFTSCAN_PUBLIC unsigned swiftscan_cached_compilation_get_num_outputs(
    swiftscan_cached_compilation_t);

/// Get the cached output for the given index in the cached compilation.
SWIFTSCAN_PUBLIC swiftscan_cached_output_t
swiftscan_cached_compilation_get_output(swiftscan_cached_compilation_t,
                                        unsigned idx);

/// Check if the requested cached compilation is uncacheable. That means the
/// compiler decides to skip caching its output even the compilation is
/// successful.
SWIFTSCAN_PUBLIC bool
    swiftscan_cached_compilation_is_uncacheable(swiftscan_cached_compilation_t);

/// Make the cache compilation available globally. \c callback will be called
/// on completion.
/// \c swiftscan_cache_cancellation_token_t will return an token that can be
/// used to cancel the async operation. The token needs to be freed by caller
/// using `swiftscan_cache_cancellation_token_dispose`. If no token is needed,
/// nullptr can be passed and no token will be returned.
SWIFTSCAN_PUBLIC void swiftscan_cached_compilation_make_global_async(
    swiftscan_cached_compilation_t, void *ctx,
    void (*callback)(void *ctx, swiftscan_string_ref_t error),
    swiftscan_cache_cancellation_token_t *);

/// Dispose a cached compilation.
SWIFTSCAN_PUBLIC
void swiftscan_cached_compilation_dispose(swiftscan_cached_compilation_t);

/// Download and materialize the cached output if needed from a remote CAS.
/// Return true if load is successful, else false if not found or error. If
/// error, the error message is returned via \c error parameter and its caller
/// needs to free the message using `swiftscan_string_dispose`.
SWIFTSCAN_PUBLIC bool
swiftscan_cached_output_load(swiftscan_cached_output_t,
                             swiftscan_string_ref_t *error);

/// Async version of `swiftscan_cached_output_load` where result is
/// returned via callback. \c ctx is an opaque value that passed to the callback
/// and \c swiftscan_cache_cancellation_token_t will return an token that can be
/// used to cancel the async operation. The token needs to be freed by caller
/// using `swiftscan_cache_cancellation_token_dispose`. If no token is needed,
/// nullptr can be passed and no token will be returned.
SWIFTSCAN_PUBLIC void swiftscan_cached_output_load_async(
    swiftscan_cached_output_t, void *ctx,
    void (*callback)(void *ctx, bool success, swiftscan_string_ref_t error),
    swiftscan_cache_cancellation_token_t *);

/// Check if cached output is materialized locally and can be accessed
/// without downloading.
SWIFTSCAN_PUBLIC bool
    swiftscan_cached_output_is_materialized(swiftscan_cached_output_t);

/// Return the casid for the cached output as \c swiftscan_string_ref_t and the
/// returned string needs to be freed using `swiftscan_string_dispose`. CASID
/// can be requested before loading/materializing.
SWIFTSCAN_PUBLIC swiftscan_string_ref_t
    swiftscan_cached_output_get_casid(swiftscan_cached_output_t);

/// Get the output name for cached compilation. The
/// returned name needs to be freed by `swiftscan_string_dispose`.
SWIFTSCAN_PUBLIC swiftscan_string_ref_t
    swiftscan_cached_output_get_name(swiftscan_cached_output_t);

/// Dispose a cached output.
SWIFTSCAN_PUBLIC
void swiftscan_cached_output_dispose(swiftscan_cached_output_t);

/// Cancel the async cache action that is associated with token.
SWIFTSCAN_PUBLIC void
    swiftscan_cache_action_cancel(swiftscan_cache_cancellation_token_t);

/// Dispose the cancellation token.
SWIFTSCAN_PUBLIC void swiftscan_cache_cancellation_token_dispose(
    swiftscan_cache_cancellation_token_t);

/// Async load CASObject using CASID. This is only to perform the download to
/// local CAS where result is returned via callback. \c ctx is an opaque value
/// that passed to the callback and \c swiftscan_cache_cancellation_token_t will
/// return an token that can be used to cancel the async operation. The token
/// needs to be freed by caller using
/// `swiftscan_cache_cancellation_token_dispose`. If no token is needed, nullptr
/// can be passed and no token will be returned.
SWIFTSCAN_PUBLIC void swiftscan_cache_download_cas_object_async(
    swiftscan_cas_t, const char *id, void *ctx,
    void (*callback)(void *ctx, bool success, swiftscan_string_ref_t error),
    swiftscan_cache_cancellation_token_t *);

/// Create a swift cached compilation replay instance with its command-line
/// invocation. Return nullptr when errors occurs and the error message is
/// returned via \c error parameter and its caller needs to free the message
/// using `swiftscan_string_dispose`.
SWIFTSCAN_PUBLIC swiftscan_cache_replay_instance_t
swiftscan_cache_replay_instance_create(int argc, const char **argv,
                                       swiftscan_string_ref_t *error);

/// Dispose swift cached compilation replay instance.
SWIFTSCAN_PUBLIC void
    swiftscan_cache_replay_instance_dispose(swiftscan_cache_replay_instance_t);

/// Replay the cached compilation using cached compliation replay instance.
/// Returns replay result or nullptr if output not found or error occurs. If
/// error, the error message is returned via \c error parameter and its caller
/// needs to free the message using `swiftscan_string_dispose`.
SWIFTSCAN_PUBLIC swiftscan_cache_replay_result_t
swiftscan_cache_replay_compilation(swiftscan_cache_replay_instance_t,
                                   swiftscan_cached_compilation_t,
                                   swiftscan_string_ref_t *error);

/// Get stdout from cached replay result. The returning swiftscan_string_ref_t
/// is owned by replay result and should not be disposed.
SWIFTSCAN_PUBLIC
swiftscan_string_ref_t
    swiftscan_cache_replay_result_get_stdout(swiftscan_cache_replay_result_t);

/// Get stderr from cached replay result. The returning swiftscan_string_ref_t
/// is owned by replay result and should not be disposed.
SWIFTSCAN_PUBLIC
swiftscan_string_ref_t
    swiftscan_cache_replay_result_get_stderr(swiftscan_cache_replay_result_t);

/// Dispose a cached replay result.
SWIFTSCAN_PUBLIC
void swiftscan_cache_replay_result_dispose(swiftscan_cache_replay_result_t);

//===----------------------------------------------------------------------===//

SWIFTSCAN_END_DECLS

#endif // SWIFT_C_DEPENDENCY_SCAN_H
