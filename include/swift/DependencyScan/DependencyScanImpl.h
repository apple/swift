//===-------------- DependencyScanImpl.h - Swift Compiler -----------------===//
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
// Implementation details of the dependency scanning C API
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_C_DEPENDENCY_SCAN_IMPL_H
#define SWIFT_C_DEPENDENCY_SCAN_IMPL_H

#include "swift-c/DependencyScan/DependencyScan.h"

namespace swift {
namespace dependencies {
class DependencyScanningTool;
}
} // namespace swift

struct swiftscan_dependency_graph_s {
  /// The name of the main module for this dependency graph (root node)
  swiftscan_string_ref_t main_module_name;

  /// The complete list of modules discovered
  swiftscan_dependency_set_t *dependencies;
};

struct swiftscan_dependency_info_s {
  /// The module's name
  /// The format is:
  /// `<module-kind>:<module-name>`
  /// where `module-kind` is one of:
  /// "swiftInterface"
  /// "swiftSource"
  /// "swiftBinary"
  /// "swiftPlaceholder"
  /// "clang""
  swiftscan_string_ref_t module_name;

  /// The path for the module.
  swiftscan_string_ref_t module_path;

  /// The source files used to build this module.
  swiftscan_string_set_t *source_files;

  /**
   * The list of modules which this module direct depends on.
   * The format is:
   * `<module-kind>:<module-name>`
   */
  swiftscan_string_set_t *direct_dependencies;

  /// Specific details of a particular kind of module.
  swiftscan_module_details_t details;
};

/// Swift modules to be built from a module interface, may have a bridging
/// header.
typedef struct {
  /// The module interface from which this module was built, if any.
  swiftscan_string_ref_t module_interface_path;

  /// The paths of potentially ready-to-use compiled modules for the interface.
  swiftscan_string_set_t *compiled_module_candidates;

  /// The bridging header, if any.
  swiftscan_string_ref_t bridging_header_path;

  /// The source files referenced by the bridging header.
  swiftscan_string_set_t *bridging_source_files;

  /// (Clang) modules on which the bridging header depends.
  swiftscan_string_set_t *bridging_module_dependencies;

  /// (Swift) module dependencies by means of being overlays of
  /// Clang module dependencies
  swiftscan_string_set_t *swift_overlay_module_dependencies;

  /// Options to the compile command required to build this module interface
  swiftscan_string_set_t *command_line;

  /// To build a PCM to be used by this Swift module, we need to append these
  /// arguments to the generic PCM build arguments reported from the dependency
  /// graph.
  swiftscan_string_set_t *extra_pcm_args;

  /// The hash value that will be used for the generated module
  swiftscan_string_ref_t context_hash;

  /// A flag to indicate whether or not this module is a framework.
  bool is_framework;
} swiftscan_swift_textual_details_t;

/// Swift modules with only a binary module file.
typedef struct {
  /// The path to the pre-compiled binary module
  swiftscan_string_ref_t compiled_module_path;

  /// The path to the .swiftModuleDoc file.
  swiftscan_string_ref_t module_doc_path;

  /// The path to the .swiftSourceInfo file.
  swiftscan_string_ref_t module_source_info_path;

  /// A flag to indicate whether or not this module is a framework.
  bool is_framework;
} swiftscan_swift_binary_details_t;

/// Swift placeholder modules carry additional details that specify their
/// module doc path and source info paths.
typedef struct {
  /// The path to the pre-compiled binary module
  swiftscan_string_ref_t compiled_module_path;

  /// The path to the .swiftModuleDoc file.
  swiftscan_string_ref_t module_doc_path;

  /// The path to the .swiftSourceInfo file.
  swiftscan_string_ref_t module_source_info_path;
} swiftscan_swift_placeholder_details_t;

/// Clang modules are built from a module map file.
typedef struct {
  /// The path to the module map used to build this module.
  swiftscan_string_ref_t module_map_path;

  /// clang-generated context hash
  swiftscan_string_ref_t context_hash;

  /// Options to the compile command required to build this clang modulemap
  swiftscan_string_set_t *command_line;

  /// The swift-specific PCM arguments captured by this dependencies object
  swiftscan_string_set_t *captured_pcm_args;
} swiftscan_clang_details_t;

struct swiftscan_module_details_s {
  swiftscan_dependency_info_kind_t kind;
  union {
    swiftscan_swift_textual_details_t swift_textual_details;
    swiftscan_swift_binary_details_t swift_binary_details;
    swiftscan_swift_placeholder_details_t swift_placeholder_details;
    swiftscan_clang_details_t clang_details;
  };
};

struct swiftscan_batch_scan_entry_s {
  swiftscan_string_ref_t module_name;
  swiftscan_string_ref_t arguments;
  bool is_swift;
};

struct swiftscan_import_set_s {
  /// The complete list of imports discovered
  swiftscan_string_set_t *imports;
};

struct swiftscan_scan_invocation_s {
  swiftscan_string_ref_t working_directory;
  swiftscan_string_set_t *argv;
};

struct swiftscan_diagnostic_info_s {
  swiftscan_string_ref_t message;
  swiftscan_diagnostic_severity_t severity;
  // TODO: SourceLoc
};

#endif // SWIFT_C_DEPENDENCY_SCAN_IMPL_H
