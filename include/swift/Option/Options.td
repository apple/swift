//===--- Options.td - Options for swift driver ----------------------------===//
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
//  This file defines the options accepted by the swift driver.
//
//===----------------------------------------------------------------------===//

// Include the common option parsing interfaces.
include "llvm/Option/OptParser.td"

/////////
// Flags

// The option should be accepted by swift -frontend.
def FrontendOption : OptionFlag;

// The option should be accepted by swift-autolink-extract
def AutolinkExtractOption : OptionFlag;

// The option should be accepted by swift -modulewrap
def ModuleWrapOption : OptionFlag;

// The option should be accepted by swift-indent
def SwiftIndentOption : OptionFlag;

// The option should not be accepted by the driver.
def NoDriverOption : OptionFlag;

// Some options should not be available depending on whether this is the
// interactive driver 'swift', or the batch compiler 'swiftc'.
def NoInteractiveOption : OptionFlag;
def NoBatchOption : OptionFlag;

// The option should not force a full rebuild if added, changed, or removed.
def DoesNotAffectIncrementalBuild : OptionFlag;

// The option's argument is a file-system path that may be affected by the
// current working directory.
def ArgumentIsPath : OptionFlag;

// The option should be written into a .swiftinterface module interface file,
// and read/parsed from there when reconstituting a .swiftmodule from it.
def ModuleInterfaceOption : OptionFlag;

// The option should be written into a .swiftinterface module interface file,
// and read/parsed from there when reconstituting a .swiftmodule from it.
// The option can be safely ignored by the older compiler.
def ModuleInterfaceOptionIgnorable : OptionFlag;

// The option should be written into a .private.swiftinterface module interface file,
// and read/parsed from there when reconstituting a .swiftmodule from it.
// The option can be safely ignored by the older compiler.
def ModuleInterfaceOptionIgnorablePrivate : OptionFlag;

// The option causes the output of a supplementary output, or is the path option
// for a supplementary output. E.g., `-emit-module` and `-emit-module-path`.
def SupplementaryOutput : OptionFlag;

// The option should be accepted by swift-api-extract.
def SwiftAPIExtractOption : OptionFlag;

// The option should be accepted by swift-symbolgraph-extract.
def SwiftSymbolGraphExtractOption : OptionFlag;

// The option should be accepted by swift-api-digester.
def SwiftAPIDigesterOption : OptionFlag;

// The option only functions in the new driver.
def NewDriverOnlyOption : OptionFlag;

/////////
// Options

// The internal option ID must be a valid C++ identifier and results in a
// swift::driver::options::OPT_XX enum constant for XX.
//
// We want to unambiguously be able to refer to options from the driver source
// code, for this reason the option name is mangled into an ID. This mangling
// isn't guaranteed to have an inverse, but for practical purposes it does.
//
// The mangling scheme is to ignore the leading '-', and perform the following
// substitutions:
//   _ => __
//   - => _
//   / => _SLASH
//   # => _HASH
//   ? => _QUESTION
//   , => _COMMA
//   = => _EQ
//   C++ => CXX
//   . => _

def internal_Group : OptionGroup<"<swift internal options>">;
def internal_debug_Group :
  OptionGroup<"<swift debug/development internal options>">,
  Group<internal_Group>, HelpText<"DEBUG/DEVELOPMENT OPTIONS">;

class InternalDebugOpt : Group<internal_debug_Group>,
  Flags<[HelpHidden, DoesNotAffectIncrementalBuild]>;

def driver_print_actions : Flag<["-"], "driver-print-actions">,
  InternalDebugOpt, HelpText<"Dump list of actions to perform">;
def driver_print_output_file_map : Flag<["-"], "driver-print-output-file-map">,
  InternalDebugOpt, HelpText<"Dump the contents of the output file map">;
def driver_print_derived_output_file_map : Flag<["-"], "driver-print-derived-output-file-map">,
  InternalDebugOpt, HelpText<"Dump the contents of the derived output file map">;
def driver_print_bindings : Flag<["-"], "driver-print-bindings">,
  InternalDebugOpt, HelpText<"Dump list of job inputs and outputs">;
def driver_print_jobs : Flag<["-"], "driver-print-jobs">, InternalDebugOpt,
  HelpText<"Dump list of jobs to execute">;
def _HASH_HASH_HASH : Flag<["-"], "###">, Alias<driver_print_jobs>;
def driver_skip_execution : Flag<["-"], "driver-skip-execution">,
  InternalDebugOpt,
  HelpText<"Skip execution of subtasks when performing compilation">;
def driver_use_frontend_path : Separate<["-"], "driver-use-frontend-path">,
  InternalDebugOpt,
  HelpText<"Use the given executable to perform compilations. Arguments can be passed as a ';' separated list">;
def driver_show_incremental : Flag<["-"], "driver-show-incremental">,
  InternalDebugOpt,
  HelpText<"With -v, dump information about why files are being rebuilt">;
def driver_show_job_lifecycle : Flag<["-"], "driver-show-job-lifecycle">,
  InternalDebugOpt,
  HelpText<"Show every step in the lifecycle of driver jobs">;
def driver_use_filelists : Flag<["-"], "driver-use-filelists">,
  InternalDebugOpt, HelpText<"Pass input files as filelists whenever possible">;
def driver_filelist_threshold : Separate<["-"], "driver-filelist-threshold">,
  InternalDebugOpt, HelpText<"Pass input or output file names as filelists if there are more than <n>">,
  MetaVarName<"<n>">;
def driver_filelist_threshold_EQ : Joined<["-"], "driver-filelist-threshold=">,
  Alias<driver_filelist_threshold>;
def driver_batch_seed : Separate<["-"], "driver-batch-seed">,
  InternalDebugOpt,
  HelpText<"Use the given seed value to randomize batch-mode partitions">;
def driver_batch_count : Separate<["-"], "driver-batch-count">,
  InternalDebugOpt,
  HelpText<"Use the given number of batch-mode partitions, rather than partitioning dynamically">;
def driver_batch_size_limit : Separate<["-"], "driver-batch-size-limit">,
  InternalDebugOpt,
  HelpText<"Use the given number as the upper limit on dynamic batch-mode partition size">;

def driver_force_response_files : Flag<["-"], "driver-force-response-files">,
  InternalDebugOpt,
  HelpText<"Force the use of response files for testing">;

def driver_always_rebuild_dependents :
  Flag<["-"], "driver-always-rebuild-dependents">, InternalDebugOpt,
  HelpText<"Always rebuild dependents of files that have been modified">;

def enable_only_one_dependency_file :
Flag<["-"], "enable-only-one-dependency-file">, Flags<[DoesNotAffectIncrementalBuild]>,
 HelpText<"Enables incremental build optimization that only produces one dependencies file">;

def disable_only_one_dependency_file :
Flag<["-"], "disable-only-one-dependency-file">, Flags<[DoesNotAffectIncrementalBuild]>,
 HelpText<"Disables incremental build optimization that only produces one dependencies file">;

def driver_verify_fine_grained_dependency_graph_after_every_import :
Flag<["-"], "driver-verify-fine-grained-dependency-graph-after-every-import">,
InternalDebugOpt,
HelpText<"Debug DriverGraph by verifying it after every import">;

def verify_incremental_dependencies :
  Flag<["-"], "verify-incremental-dependencies">,
  Flags<[FrontendOption, HelpHidden]>,
  HelpText<"Enable the dependency verifier for each frontend job">;
  
def strict_implicit_module_context :
  Flag<["-"], "strict-implicit-module-context">,
  Flags<[FrontendOption, HelpHidden]>,
  HelpText<"Enable the strict forwarding of compilation context to downstream implicit module dependencies">;
  
def no_strict_implicit_module_context :
  Flag<["-"], "no-strict-implicit-module-context">,
  Flags<[FrontendOption, HelpHidden]>,
  HelpText<"Disable the strict forwarding of compilation context to downstream implicit module dependencies">;

def validate_clang_modules_once :
  Flag<["-"], "validate-clang-modules-once">,
  Flags<[FrontendOption]>,
  HelpText<"Don't verify input files for Clang modules if the module has been successfully validated or loaded during this build session">;

def clang_build_session_file : Separate<["-"], "clang-build-session-file">,
  Flags<[FrontendOption, ArgumentIsPath]>,
  HelpText<"Use the last modification time of <file> as the underlying Clang build session timestamp">;

def disallow_forwarding_driver :
  Flag<["-"], "disallow-use-new-driver">, Flags<[]>,
  HelpText<"Disable using new swift-driver">;

def driver_emit_fine_grained_dependency_dot_file_after_every_import :
Flag<["-"], "driver-emit-fine-grained-dependency-dot-file-after-every-import">,
InternalDebugOpt,
HelpText<"Emit dot files every time driver imports an fine-grained swiftdeps file.">;

def emit_fine_grained_dependency_sourcefile_dot_files :
Flag<["-"], "emit-fine-grained-dependency-sourcefile-dot-files">,
Flags<[FrontendOption, HelpHidden]>,
HelpText<"Emit dot files for every source file.">;

def driver_mode : Joined<["--"], "driver-mode=">, Flags<[HelpHidden]>,
  HelpText<"Set the driver mode to either 'swift' or 'swiftc'">;

def help : Flag<["-", "--"], "help">,
  Flags<[FrontendOption, AutolinkExtractOption, ModuleWrapOption,
         SwiftIndentOption, SwiftAPIExtractOption,
         SwiftSymbolGraphExtractOption, SwiftAPIDigesterOption]>,
  HelpText<"Display available options">;
def h : Flag<["-"], "h">, Alias<help>;
def help_hidden : Flag<["-", "--"], "help-hidden">,
  Flags<[HelpHidden, FrontendOption]>,
  HelpText<"Display available options, including hidden options">;

def v : Flag<["-"], "v">,
  Flags<[DoesNotAffectIncrementalBuild, SwiftSymbolGraphExtractOption, SwiftAPIDigesterOption]>,
  HelpText<"Show commands to run and use verbose output">;
def version : Flag<["-", "--"], "version">, Flags<[FrontendOption]>,
  HelpText<"Print version information and exit">;

def parseable_output : Flag<["-"], "parseable-output">,
  Flags<[NoInteractiveOption, DoesNotAffectIncrementalBuild]>,
  HelpText<"Emit textual output in a parseable format">;

// Windows Options

def windows_sdk_root : Separate<["-"], "windows-sdk-root">,
  Flags<[ArgumentIsPath, FrontendOption, SwiftAPIExtractOption,
         SwiftSymbolGraphExtractOption, SwiftAPIDigesterOption]>,
  HelpText<"Windows SDK Root">, MetaVarName<"<root>">;
def windows_sdk_version : Separate<["-"], "windows-sdk-version">,
  Flags<[ArgumentIsPath, FrontendOption, SwiftAPIExtractOption,
         SwiftSymbolGraphExtractOption, SwiftAPIDigesterOption]>,
  HelpText<"Windows SDK Version">, MetaVarName<"<version>">;
def visualc_tools_root : Separate<["-"], "visualc-tools-root">,
  Flags<[ArgumentIsPath, FrontendOption, SwiftAPIExtractOption,
         SwiftSymbolGraphExtractOption, SwiftAPIDigesterOption]>,
  HelpText<"VisualC++ Tools Root">, MetaVarName<"<root>">;
def visualc_tools_version : Separate<["-"], "visualc-tools-version">,
  Flags<[ArgumentIsPath, FrontendOption, SwiftAPIExtractOption,
         SwiftSymbolGraphExtractOption, SwiftAPIDigesterOption]>,
  HelpText<"VisualC++ ToolSet Version">, MetaVarName<"<version>">;

// Standard Options
def _DASH_DASH : Option<["--"], "", KIND_REMAINING_ARGS>,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild]>;

def o : JoinedOrSeparate<["-"], "o">,
  Flags<[FrontendOption, AutolinkExtractOption, ModuleWrapOption,
         NoInteractiveOption, SwiftIndentOption, ArgumentIsPath,
         SwiftAPIExtractOption, SwiftAPIDigesterOption]>,
  HelpText<"Write output to <file>">, MetaVarName<"<file>">;

def j : JoinedOrSeparate<["-"], "j">, Flags<[DoesNotAffectIncrementalBuild]>,
  HelpText<"Number of commands to execute in parallel">, MetaVarName<"<n>">;

def sdk : Separate<["-"], "sdk">,
  Flags<[FrontendOption, ArgumentIsPath, SwiftAPIExtractOption, SwiftSymbolGraphExtractOption,
         SwiftAPIDigesterOption]>,
  HelpText<"Compile against <sdk>">, MetaVarName<"<sdk>">;

def swift_version : Separate<["-"], "swift-version">,
  Flags<[FrontendOption, ModuleInterfaceOption, SwiftAPIExtractOption, SwiftSymbolGraphExtractOption,
         SwiftAPIDigesterOption]>,
  HelpText<"Interpret input according to a specific Swift language version number">,
  MetaVarName<"<vers>">;

def package_description_version: Separate<["-"], "package-description-version">,
  Flags<[FrontendOption, HelpHidden, ModuleInterfaceOption]>,
  HelpText<"The version number to be applied on the input for the PackageDescription availability kind">,
  MetaVarName<"<vers>">;

def tools_directory : Separate<["-"], "tools-directory">,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild,
         ArgumentIsPath]>,
  HelpText<"Look for external executables (ld, clang, binutils) in <directory>">, MetaVarName<"<directory>">;

def D : JoinedOrSeparate<["-"], "D">, Flags<[FrontendOption]>,
  HelpText<"Marks a conditional compilation flag as true">;
  
def e : Separate<["-"], "e">, Flags<[NewDriverOnlyOption]>,
  HelpText<"Executes a line of code provided on the command line">;

def F : JoinedOrSeparate<["-"], "F">,
  Flags<[FrontendOption, ArgumentIsPath, SwiftAPIExtractOption, SwiftSymbolGraphExtractOption, SwiftAPIDigesterOption]>,
  HelpText<"Add directory to framework search path">;
def F_EQ : Joined<["-"], "F=">, Flags<[FrontendOption, ArgumentIsPath]>,
  Alias<F>;

def Fsystem : Separate<["-"], "Fsystem">,
  Flags<[FrontendOption, ArgumentIsPath, SwiftAPIExtractOption, SwiftSymbolGraphExtractOption]>,
  HelpText<"Add directory to system framework search path">;

def I : JoinedOrSeparate<["-"], "I">,
  Flags<[FrontendOption, ArgumentIsPath, SwiftAPIExtractOption, SwiftSymbolGraphExtractOption, SwiftAPIDigesterOption]>,
  HelpText<"Add directory to the import search path">;
def I_EQ : Joined<["-"], "I=">, Flags<[FrontendOption, ArgumentIsPath]>,
  Alias<I>;

def plugin_path : Separate<["-"], "plugin-path">,
  Flags<[FrontendOption, ArgumentIsPath, SwiftAPIExtractOption, SwiftSymbolGraphExtractOption, SwiftAPIDigesterOption]>,
  HelpText<"Add directory to the plugin search path">;

def external_plugin_path : Separate<["-"], "external-plugin-path">,
  Flags<[FrontendOption, ArgumentIsPath, SwiftAPIExtractOption, SwiftSymbolGraphExtractOption, SwiftAPIDigesterOption]>,
  HelpText<"Add directory to the plugin search path with a plugin server executable">,
  MetaVarName<"<path>#<plugin-server-path>">;

def import_underlying_module : Flag<["-"], "import-underlying-module">,
  Flags<[FrontendOption, NoInteractiveOption]>,
  HelpText<"Implicitly imports the Objective-C half of a module">;

def import_objc_header : Separate<["-"], "import-objc-header">,
  Flags<[FrontendOption, HelpHidden, ArgumentIsPath]>,
  HelpText<"Implicitly imports an Objective-C header file">;

def pch_output_dir: Separate<["-"], "pch-output-dir">,
  Flags<[FrontendOption, HelpHidden, ArgumentIsPath]>,
  HelpText<"Directory to persist automatically created precompiled bridging headers">;

// FIXME: Unhide this once it doesn't depend on an output file map.
def incremental : Flag<["-"], "incremental">,
  Flags<[NoInteractiveOption, HelpHidden, DoesNotAffectIncrementalBuild]>,
  HelpText<"Perform an incremental build if possible">;

def nostdimport : Flag<["-"], "nostdimport">, Flags<[FrontendOption]>,
  HelpText<"Don't search the standard library import path for modules">;

def output_file_map : Separate<["-"], "output-file-map">,
  Flags<[NoInteractiveOption, ArgumentIsPath]>,
  HelpText<"A file which specifies the location of outputs">,
  MetaVarName<"<path>">;
def output_file_map_EQ : Joined<["-"], "output-file-map=">,
  Flags<[NoInteractiveOption, ArgumentIsPath]>,
  Alias<output_file_map>;

def save_temps : Flag<["-"], "save-temps">,
  Flags<[NoInteractiveOption,DoesNotAffectIncrementalBuild]>,
  HelpText<"Save intermediate compilation results">;
def driver_time_compilation : Flag<["-"], "driver-time-compilation">,
  Flags<[NoInteractiveOption,DoesNotAffectIncrementalBuild]>,
  HelpText<"Prints the total time it took to execute all compilation tasks">;
def stats_output_dir: Separate<["-"], "stats-output-dir">,
  Flags<[FrontendOption, HelpHidden, ArgumentIsPath]>,
  HelpText<"Directory to write unified compilation-statistics files to">;
def trace_stats_events: Flag<["-"], "trace-stats-events">,
  Flags<[FrontendOption, HelpHidden]>,
  HelpText<"Trace changes to stats in -stats-output-dir">;
def experimental_skip_non_inlinable_function_bodies:
  Flag<["-"], "experimental-skip-non-inlinable-function-bodies">,
  Flags<[FrontendOption, HelpHidden]>,
  HelpText<"Skip type-checking and SIL generation for non-inlinable function bodies">;
def experimental_skip_non_inlinable_function_bodies_without_types:
  Flag<["-"], "experimental-skip-non-inlinable-function-bodies-without-types">,
  Flags<[FrontendOption, HelpHidden]>,
  HelpText<"Skip work on non-inlinable function bodies that do not declare nested types">;
def profile_stats_events: Flag<["-"], "profile-stats-events">,
  Flags<[FrontendOption, HelpHidden]>,
  HelpText<"Profile changes to stats in -stats-output-dir">;
def profile_stats_entities: Flag<["-"], "profile-stats-entities">,
  Flags<[FrontendOption, HelpHidden]>,
  HelpText<"Profile changes to stats in -stats-output-dir, subdivided by source entity">;

def emit_dependencies : Flag<["-"], "emit-dependencies">,
  Flags<[FrontendOption, NoInteractiveOption, SupplementaryOutput]>,
  HelpText<"Emit basic Make-compatible dependencies files">;
def track_system_dependencies : Flag<["-"], "track-system-dependencies">,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>,
  HelpText<"Track system dependencies while emitting Make-style dependencies">;

def emit_loaded_module_trace : Flag<["-"], "emit-loaded-module-trace">,
  Flags<[FrontendOption, NoInteractiveOption, SupplementaryOutput]>,
  HelpText<"Emit a JSON file containing information about what modules were loaded">;
def emit_loaded_module_trace_path : Separate<["-"], "emit-loaded-module-trace-path">,
  Flags<[FrontendOption, NoInteractiveOption, ArgumentIsPath,
         SupplementaryOutput]>,
  HelpText<"Emit the loaded module trace JSON to <path>">,
  MetaVarName<"<path>">;
def emit_loaded_module_trace_path_EQ : Joined<["-"], "emit-loaded-module-trace-path=">,
  Flags<[FrontendOption, NoInteractiveOption, ArgumentIsPath,
         SupplementaryOutput]>,
  Alias<emit_loaded_module_trace_path>;
def emit_cross_import_remarks : Flag<["-"], "Rcross-import">,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild]>,
  HelpText<"Emit a remark if a cross-import of a module is triggered.">;

def remark_loading_module : Flag<["-"], "Rmodule-loading">,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild]>,
  HelpText<"Emit a remark and file path of each loaded module">;

def remark_indexing_system_module : Flag<["-"], "Rindexing-system-module">,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild]>,
  HelpText<"Emit a remark when indexing a system module">;

def remark_skip_explicit_interface_build : Flag<["-"], "Rskip-explicit-interface-build">,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild]>,
  HelpText<"Emit a remark if an explicit module interface invocation has an early exit because the expected output is up-to-date">;

def emit_tbd : Flag<["-"], "emit-tbd">,
  HelpText<"Emit a TBD file">,
  Flags<[FrontendOption, NoInteractiveOption, SupplementaryOutput]>;
def emit_tbd_path : Separate<["-"], "emit-tbd-path">,
  Flags<[FrontendOption, NoInteractiveOption, ArgumentIsPath,
         SupplementaryOutput]>,
  HelpText<"Emit the TBD file to <path>">,
  MetaVarName<"<path>">;
def emit_tbd_path_EQ : Joined<["-"], "emit-tbd-path=">,
  Flags<[FrontendOption, NoInteractiveOption, ArgumentIsPath,
         SupplementaryOutput]>,
  Alias<emit_tbd_path>;
def embed_tbd_for_module : Separate<["-"], "embed-tbd-for-module">,
  Flags<[FrontendOption]>,
  HelpText<"Embed symbols from the module in the emitted tbd file">;

def serialize_diagnostics : Flag<["-"], "serialize-diagnostics">,
  Flags<[FrontendOption, NoInteractiveOption, SupplementaryOutput]>,
  HelpText<"Serialize diagnostics in a binary format">;
def serialize_diagnostics_path : Separate<["-"], "serialize-diagnostics-path">,
  Flags<[FrontendOption, SwiftAPIDigesterOption, NoBatchOption,
         ArgumentIsPath, SupplementaryOutput]>,
  HelpText<"Emit a serialized diagnostics file to <path>">,
  MetaVarName<"<path>">;
def serialize_diagnostics_path_EQ: Joined<["-"], "serialize-diagnostics-path=">,
  Flags<[FrontendOption, SwiftAPIDigesterOption, NoBatchOption,
         ArgumentIsPath, SupplementaryOutput]>,
  Alias<serialize_diagnostics_path>;
def color_diagnostics : Flag<["-"], "color-diagnostics">,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild]>,
  HelpText<"Print diagnostics in color">;
def no_color_diagnostics : Flag<["-"], "no-color-diagnostics">,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild]>,
  HelpText<"Do not print diagnostics in color">;
def debug_diagnostic_names : Flag<["-"], "debug-diagnostic-names">,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild, HelpHidden]>,
  HelpText<"Include diagnostic names when printing">;
def print_educational_notes : Flag<["-"], "print-educational-notes">,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild]>,
  HelpText<"Include educational notes in printed diagnostic output, if available">;
def diagnostic_style : Separate<["-"], "diagnostic-style">,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild]>,
  MetaVarName<"<style>">,
  HelpText<"The formatting style used when printing diagnostics ('swift' or 'llvm')">;
def diagnostic_style_EQ : Joined<["-"], "diagnostic-style=">,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild]>,
  MetaVarName<"<style>">, Alias<diagnostic_style>;
def locale : Separate<["-"], "locale">,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild]>,
  HelpText<"Choose a language for diagnostic messages">,
  MetaVarName<"<locale-code>">;
def localization_path : Separate<["-"], "localization-path">,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild, ArgumentIsPath]>,
  HelpText<"Path to localized diagnostic messages directory">,
  MetaVarName<"<path>">;

def module_cache_path : Separate<["-"], "module-cache-path">,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild, ArgumentIsPath, SwiftAPIExtractOption,
         SwiftSymbolGraphExtractOption, SwiftAPIDigesterOption]>,
  HelpText<"Specifies the Clang module cache path">;

def enable_library_evolution : Flag<["-"], "enable-library-evolution">,
  Flags<[FrontendOption, ModuleInterfaceOption]>,
  HelpText<"Build the module to allow binary-compatible library evolution">;

def require_explicit_availability : Flag<["-"], "require-explicit-availability">,
  Flags<[FrontendOption, NoInteractiveOption]>,
  HelpText<"Warn on public declarations without an availability attribute">;

def require_explicit_availability_EQ : Joined<["-"], "require-explicit-availability=">,
  MetaVarName<"<error,warn,ignore>">,
  Flags<[FrontendOption, NoInteractiveOption]>,
  HelpText<"Set diagnostic level to report public declarations without an availability attribute">;

def require_explicit_availability_target : Separate<["-"], "require-explicit-availability-target">,
  Flags<[FrontendOption, NoInteractiveOption]>,
  HelpText<"Suggest fix-its adding @available(<target>, *) to public declarations without availability">,
  MetaVarName<"<target>">;

def require_explicit_sendable : Flag<["-"], "require-explicit-sendable">,
  Flags<[FrontendOption, NoInteractiveOption]>,
  HelpText<"Require explicit Sendable annotations on public declarations">;

def define_availability : Separate<["-"], "define-availability">,
  Flags<[FrontendOption, NoInteractiveOption]>,
  HelpText<"Define an availability macro in the format 'macroName : iOS 13.0, macOS 10.15'">,
  MetaVarName<"<macro>">;

def check_api_availability_only : Flag<["-"], "check-api-availability-only">,
  Flags<[HelpHidden, FrontendOption, NoInteractiveOption]>,
  HelpText<"Deprecated, has no effect">;

def unavailable_decl_optimization_EQ : Joined<["-"], "unavailable-decl-optimization=">,
  MetaVarName<"<complete,none>">,
  Flags<[FrontendOption, NoInteractiveOption]>,
  HelpText<"Specify the optimization mode for unavailable declarations. The "
           "value may be 'none' (no optimization) or 'complete' (code is not "
           "generated at all unavailable declarations)">;

def library_level : Separate<["-"], "library-level">,
  MetaVarName<"<level>">,
  Flags<[HelpHidden, FrontendOption, ModuleInterfaceOption]>,
  HelpText<"Library distribution level 'api', 'spi' or 'other' (the default)">;
def library_level_EQ : Joined<["-"], "library-level=">,
  MetaVarName<"<level>">,
  Flags<[HelpHidden, FrontendOption, ModuleInterfaceOption]>,
  Alias<library_level>;

def module_name : Separate<["-"], "module-name">,
  Flags<[FrontendOption, ModuleInterfaceOption, SwiftAPIExtractOption,
         SwiftSymbolGraphExtractOption]>,
  HelpText<"Name of the module to build">;
def module_name_EQ : Joined<["-"], "module-name=">, Flags<[FrontendOption]>,
  Alias<module_name>;

def module_alias : Separate<["-"], "module-alias">,
  Flags<[FrontendOption, ModuleInterfaceOption]>,
  MetaVarName<"<alias_name=underlying_name>">,
  HelpText<"If a source file imports or references module <alias_name>, the <underlying_name> is used for the contents of the file">;

def module_link_name : Separate<["-"], "module-link-name">,
  Flags<[FrontendOption, ModuleInterfaceOption]>,
  HelpText<"Library to link against when using this module">;
def module_link_name_EQ : Joined<["-"], "module-link-name=">,
  Flags<[FrontendOption]>, Alias<module_link_name>;
def autolink_force_load : Flag<["-"], "autolink-force-load">,
  Flags<[FrontendOption, ModuleInterfaceOption, HelpHidden]>,
  HelpText<"Force ld to link against this module even if no symbols are used">;
def module_abi_name : Separate<["-"], "module-abi-name">,
  Flags<[FrontendOption, ModuleInterfaceOption]>,
  HelpText<"ABI name to use for the contents of this module">;
def package_name : Separate<["-"], "package-name">,
  Flags<[FrontendOption, ModuleInterfaceOptionIgnorablePrivate]>,
  HelpText<"Name of the package the module belongs to">;
def export_as : Separate<["-"], "export-as">,
  Flags<[FrontendOption, ModuleInterfaceOptionIgnorable]>,
  HelpText<"Module name to use when referenced in clients module interfaces">;

def emit_module : Flag<["-"], "emit-module">,
  Flags<[FrontendOption, NoInteractiveOption, SupplementaryOutput]>,
  HelpText<"Emit an importable module">;
def emit_module_path : Separate<["-"], "emit-module-path">,
  Flags<[FrontendOption, NoInteractiveOption, ArgumentIsPath,
         SupplementaryOutput]>,
  HelpText<"Emit an importable module to <path>">,
  MetaVarName<"<path>">;
def emit_module_path_EQ : Joined<["-"], "emit-module-path=">,
  Flags<[FrontendOption, NoInteractiveOption, ArgumentIsPath,
         SupplementaryOutput]>,
  Alias<emit_module_path>;

def emit_module_summary :
  Flag<["-"], "emit-module-summary">,
  Flags<[NoInteractiveOption, SupplementaryOutput]>,
  HelpText<"Output module summary file">;
def emit_module_summary_path :
  Separate<["-"], "emit-module-summary-path">,
  Flags<[FrontendOption, NoInteractiveOption, ArgumentIsPath,
         SupplementaryOutput]>,
  MetaVarName<"<path>">, HelpText<"Output module summary file to <path>">;

def emit_module_interface :
  Flag<["-"], "emit-module-interface">,
  Flags<[NoInteractiveOption, SupplementaryOutput]>,
  HelpText<"Output module interface file">;
def emit_module_interface_path :
  Separate<["-"], "emit-module-interface-path">,
  Flags<[FrontendOption, NoInteractiveOption, ArgumentIsPath,
         SupplementaryOutput]>,
  MetaVarName<"<path>">, HelpText<"Output module interface file to <path>">;

def emit_private_module_interface_path :
  Separate<["-"], "emit-private-module-interface-path">,
  Flags<[FrontendOption, NoInteractiveOption, HelpHidden,
         ArgumentIsPath, SupplementaryOutput]>,
  MetaVarName<"<path>">, HelpText<"Output private module interface file to <path>">;

def verify_emitted_module_interface :
  Flag<["-"], "verify-emitted-module-interface">,
  Flags<[NoInteractiveOption, DoesNotAffectIncrementalBuild]>,
  HelpText<"Check that module interfaces emitted during compilation typecheck">;
def no_verify_emitted_module_interface :
  Flag<["-"], "no-verify-emitted-module-interface">,
  Flags<[NoInteractiveOption, DoesNotAffectIncrementalBuild]>,
  HelpText<"Don't check that module interfaces emitted during compilation typecheck">;

def avoid_emit_module_source_info :
  Flag<["-"], "avoid-emit-module-source-info">,
  Flags<[NoInteractiveOption, DoesNotAffectIncrementalBuild]>,
  HelpText<"don't emit Swift source info file">;

def emit_module_source_info_path :
  Separate<["-"], "emit-module-source-info-path">,
  Flags<[FrontendOption, NoInteractiveOption, ArgumentIsPath,
         SupplementaryOutput]>,
  MetaVarName<"<path>">, HelpText<"Output module source info file to <path>">;

def emit_parseable_module_interface :
  Flag<["-"], "emit-parseable-module-interface">,
  Alias<emit_module_interface>,
  Flags<[NoInteractiveOption, HelpHidden, SupplementaryOutput]>;
def emit_parseable_module_interface_path :
  Separate<["-"], "emit-parseable-module-interface-path">,
  Alias<emit_module_interface_path>,
  Flags<[FrontendOption, NoInteractiveOption, HelpHidden, ArgumentIsPath,
         SupplementaryOutput]>;

def emit_const_values :
  Flag<["-"], "emit-const-values">,
  Flags<[NoInteractiveOption, SupplementaryOutput]>;
def emit_const_values_path : Separate<["-"], "emit-const-values-path">,
  Flags<[FrontendOption, NoInteractiveOption, ArgumentIsPath,
         SupplementaryOutput]>,
  MetaVarName<"<path>">, HelpText<"Emit the extracted compile-time known values to <path>">;

def emit_objc_header : Flag<["-"], "emit-objc-header">,
  Flags<[FrontendOption, NoInteractiveOption, SupplementaryOutput]>,
  HelpText<"Emit an Objective-C header file">;
def emit_objc_header_path : Separate<["-"], "emit-objc-header-path">,
  Flags<[FrontendOption, NoInteractiveOption, ArgumentIsPath,
         SupplementaryOutput]>,
  MetaVarName<"<path>">, HelpText<"Emit an Objective-C header file to <path>">;
def emit_clang_header_nonmodular_includes : Flag<["-"], "emit-clang-header-nonmodular-includes">,
  Flags<[FrontendOption, NoInteractiveOption, SupplementaryOutput]>,
  HelpText<"Augment emitted Objective-C header with textual imports for every included modular import">;

def emit_clang_header_path : Separate<["-"], "emit-clang-header-path">,
  Flags<[FrontendOption, NoDriverOption, NoInteractiveOption, ArgumentIsPath,
         SupplementaryOutput]>,
  HelpText<"Emit an Objective-C and C++ header file to <path>">,
  Alias<emit_objc_header_path>;

def static : Flag<["-"], "static">,
  Flags<[FrontendOption, ModuleInterfaceOption, NoInteractiveOption]>,
  HelpText<"Make this module statically linkable and make the output of -emit-library a static library.">;

def import_cf_types : Flag<["-"], "import-cf-types">,
  Flags<[FrontendOption, HelpHidden]>,
  HelpText<"Recognize and import CF types as class types">;

def solver_memory_threshold : Separate<["-"], "solver-memory-threshold">,
  Flags<[FrontendOption, HelpHidden, DoesNotAffectIncrementalBuild]>,
  HelpText<"Set the upper bound for memory consumption, in bytes, by the constraint solver">;

def solver_shrink_unsolved_threshold : Separate<["-"], "solver-shrink-unsolved-threshold">,
Flags<[FrontendOption, HelpHidden, DoesNotAffectIncrementalBuild]>,
HelpText<"Set The upper bound to number of sub-expressions unsolved before termination of the shrink phrase">;

def value_recursion_threshold : Separate<["-"], "value-recursion-threshold">,
  Flags<[FrontendOption, HelpHidden, DoesNotAffectIncrementalBuild]>,
  HelpText<"Set the maximum depth for direct recursion in value types">;

def disable_swift_bridge_attr : Flag<["-"], "disable-swift-bridge-attr">,
  Flags<[FrontendOption, HelpHidden]>,
  HelpText<"Disable using the swift bridge attribute">;

def enable_bridging_pch : Flag<["-"], "enable-bridging-pch">,
  Flags<[HelpHidden]>,
  HelpText<"Enable automatic generation of bridging PCH files">;

def disable_bridging_pch : Flag<["-"], "disable-bridging-pch">,
  Flags<[HelpHidden]>,
  HelpText<"Disable automatic generation of bridging PCH files">;

def lto : Joined<["-"], "lto=">,
  Flags<[FrontendOption, NoInteractiveOption]>,
  HelpText<"Specify the LTO type to either 'llvm-thin' or 'llvm-full'">;

def lto_library : Separate<["-"], "lto-library">,
  Flags<[FrontendOption, ArgumentIsPath, NoInteractiveOption]>,
  HelpText<"Perform LTO with <lto-library>">, MetaVarName<"<lto-library>">;

def access_notes_path : Separate<["-"], "access-notes-path">,
  Flags<[FrontendOption, ArgumentIsPath]>,
  HelpText<"Specify YAML file to override attributes on Swift declarations in this module">;
def access_notes_path_EQ : Joined<["-"], "access-notes-path=">,
  Flags<[FrontendOption, ArgumentIsPath]>,
  Alias<access_notes_path>;

// Experimental feature options

// Note: this flag will be removed when JVP/differential generation in the
// differentiation transform is robust.
def enable_experimental_forward_mode_differentiation :
  Flag<["-"], "enable-experimental-forward-mode-differentiation">,
  Flags<[FrontendOption]>,
  HelpText<"Enable experimental forward mode differentiation">;

def enable_experimental_additive_arithmetic_derivation :
  Flag<["-"], "enable-experimental-additive-arithmetic-derivation">,
  Flags<[FrontendOption]>,
  HelpText<"Enable experimental 'AdditiveArithmetic' derived conformances">;

def enable_experimental_concise_pound_file : Flag<["-"],
    "enable-experimental-concise-pound-file">,
  Flags<[FrontendOption, ModuleInterfaceOption]>,
  HelpText<"Enable experimental concise '#file' identifier">;

def enable_experimental_cxx_interop :
  Flag<["-"], "enable-experimental-cxx-interop">,
  Flags<[NoDriverOption, FrontendOption, HelpHidden, ModuleInterfaceOption]>,
  HelpText<"Enable experimental C++ interop code generation and config directives">;

def cxx_interoperability_mode :
  Joined<["-"], "cxx-interoperability-mode=">,
  Flags<[FrontendOption, ModuleInterfaceOption]>,
  HelpText<"Enables C++ interoperability; pass 'default' to enable or 'off' to disable">;

def experimental_c_foreign_reference_types :
  Flag<["-"], "experimental-c-foreign-reference-types">,
  Flags<[FrontendOption, HelpHidden, ModuleInterfaceOption]>,
  HelpText<"Enable experimental C foreign references types (with reference coutning).">;

def experimental_cxx_stdlib :
  Separate<["-"], "experimental-cxx-stdlib">,
  Flags<[HelpHidden]>,
  HelpText<"C++ standard library to use; forwarded to Clang's -stdlib flag">;

def experimental_hermetic_seal_at_link:
  Flag<["-"], "experimental-hermetic-seal-at-link">,
  Flags<[FrontendOption, HelpHidden]>,
  HelpText<"Library code can assume that all clients are visible at linktime, and aggressively strip unused code">;

// Diagnostic control options
def suppress_warnings : Flag<["-"], "suppress-warnings">,
  Flags<[FrontendOption]>,
  HelpText<"Suppress all warnings">;

def warnings_as_errors : Flag<["-"], "warnings-as-errors">,
  Flags<[FrontendOption]>,
  HelpText<"Treat warnings as errors">;

def no_warnings_as_errors : Flag<["-"], "no-warnings-as-errors">,
  Flags<[FrontendOption]>,
  HelpText<"Don't treat warnings as errors">;
  
def suppress_remarks : Flag<["-"], "suppress-remarks">,
  Flags<[FrontendOption]>,
  HelpText<"Suppress all remarks">;

def continue_building_after_errors : Flag<["-"], "continue-building-after-errors">,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild]>,
  HelpText<"Continue building, even after errors are encountered">;

def warn_swift3_objc_inference_complete :
  Flag<["-"], "warn-swift3-objc-inference-complete">,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild]>,
  HelpText<"Warn about deprecated @objc inference in Swift 3 for every declaration that will no longer be inferred as @objc in Swift 4">;

def warn_swift3_objc_inference_minimal :
  Flag<["-"], "warn-swift3-objc-inference-minimal">,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild]>,
  HelpText<"Warn about deprecated @objc inference in Swift 3 based on direct uses of the Objective-C entrypoint">;

def enable_actor_data_race_checks :
  Flag<["-"], "enable-actor-data-race-checks">,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild]>,
  HelpText<"Emit runtime checks for actor data races">;

def disable_actor_data_race_checks :
  Flag<["-"], "disable-actor-data-race-checks">,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild]>,
  HelpText<"Disable runtime checks for actor data races">;

def enable_bare_slash_regex : Flag<["-"], "enable-bare-slash-regex">,
  Flags<[FrontendOption, ModuleInterfaceOptionIgnorable]>,
  HelpText<"Enable the use of forward slash regular-expression literal syntax">;

def warn_implicit_overrides :
  Flag<["-"], "warn-implicit-overrides">,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild]>,
  HelpText<"Warn about implicit overrides of protocol members">;

def typo_correction_limit : Separate<["-"], "typo-correction-limit">,
  Flags<[FrontendOption, HelpHidden]>,
  MetaVarName<"<n>">,
  HelpText<"Limit the number of times the compiler will attempt typo correction to <n>">;

def warn_swift3_objc_inference : Flag<["-"], "warn-swift3-objc-inference">,
  Alias<warn_swift3_objc_inference_complete>,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild, HelpHidden]>;

def warn_concurrency : Flag<["-"], "warn-concurrency">,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild]>,
  HelpText<"Warn about code that is unsafe according to the Swift Concurrency "
           "model and will become ill-formed in a future language version">;

def strict_concurrency : Joined<["-"], "strict-concurrency=">,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild]>,
  HelpText<"Specify the how strict concurrency checking will be. The value may "
           "be 'explicit' (most 'Sendable' checking is disabled), "
           "'targeted' ('Sendable' checking is enabled in code that uses the "
           "concurrency model, or 'complete' ('Sendable' and other checking is "
           "enabled for all code in the module)">;

def enable_experimental_feature :
  Separate<["-"], "enable-experimental-feature">,
  Flags<[FrontendOption, ModuleInterfaceOptionIgnorable]>,
  HelpText<"Enable an experimental feature">;

def enable_upcoming_feature : Separate<["-"], "enable-upcoming-feature">,
  Flags<[FrontendOption, ModuleInterfaceOptionIgnorable]>,
  HelpText<"Enable a feature that will be introduced in an upcoming language "
           "version">;

def Rpass_EQ : Joined<["-"], "Rpass=">,
  Flags<[FrontendOption]>,
  HelpText<"Report performed transformations by optimization passes whose "
           "name matches the given POSIX regular expression">;

def Rpass_missed_EQ : Joined<["-"], "Rpass-missed=">,
  Flags<[FrontendOption]>,
  HelpText<"Report missed transformations by optimization passes whose "
           "name matches the given POSIX regular expression">;

def save_optimization_record : Flag<["-"], "save-optimization-record">,
  Flags<[FrontendOption]>, HelpText<"Generate a YAML optimization record file">;
def save_optimization_record_EQ : Joined<["-"], "save-optimization-record=">,
  Flags<[FrontendOption]>,
  HelpText<"Generate an optimization record file in a specific format "
           "(default: YAML)">, MetaVarName<"<format>">;
def save_optimization_record_path :
  Separate<["-"], "save-optimization-record-path">,
  Flags<[FrontendOption, ArgumentIsPath]>,
  HelpText<"Specify the file name of any generated optimization record">;
def save_optimization_record_passes :
  Separate<["-"], "save-optimization-record-passes">,
  Flags<[FrontendOption]>,
  HelpText<"Only include passes which match a specified regular expression in "
           "the generated optimization record "
           "(by default, include all passes)">, MetaVarName<"<regex>">;

// Platform options.
def enable_app_extension : Flag<["-"], "application-extension">,
  Flags<[FrontendOption, NoInteractiveOption]>,
  HelpText<"Restrict code to those available for App Extensions">;

def libc : Separate<["-"], "libc">, HelpText<"libc runtime library to use">;

// Linker options

def linker_option_Group : OptionGroup<"<linker-specific options>">;

def l : JoinedOrSeparate<["-"], "l">, Group<linker_option_Group>,
    Flags<[FrontendOption, DoesNotAffectIncrementalBuild]>,
    HelpText<"Specifies a library which should be linked against">;
def framework : Separate<["-"], "framework">, Group<linker_option_Group>,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild]>,
  HelpText<"Specifies a framework which should be linked against">;

def L : JoinedOrSeparate<["-"], "L">, Group<linker_option_Group>,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild, ArgumentIsPath, SwiftAPIExtractOption,
         SwiftSymbolGraphExtractOption]>,
  HelpText<"Add directory to library link search path">;
def L_EQ : Joined<["-"], "L=">, Group<linker_option_Group>,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild, ArgumentIsPath]>,
  Alias<L>;

// Accept but ignore these flags.  They were once used to control
// linking of arclite, which is no longer used on Darwin.
def link_objc_runtime : Flag<["-"], "link-objc-runtime">,
  Flags<[DoesNotAffectIncrementalBuild]>,
  HelpText<"Deprecated">;
def no_link_objc_runtime : Flag<["-"], "no-link-objc-runtime">,
  Flags<[HelpHidden, DoesNotAffectIncrementalBuild]>,
  HelpText<"Deprecated">;

def static_stdlib: Flag<["-"], "static-stdlib">,
  Flags<[DoesNotAffectIncrementalBuild]>,
  HelpText<"Statically link the Swift standard library">;
def no_static_stdlib: Flag<["-"], "no-static-stdlib">,
  Flags<[HelpHidden,DoesNotAffectIncrementalBuild]>,
  HelpText<"Don't statically link the Swift standard library">;
def toolchain_stdlib_rpath: Flag<["-"], "toolchain-stdlib-rpath">,
  Flags<[HelpHidden,DoesNotAffectIncrementalBuild]>,
  HelpText<"Add an rpath entry for the toolchain's standard library, rather than the OS's">;
def no_toolchain_stdlib_rpath: Flag<["-"], "no-toolchain-stdlib-rpath">,
  Flags<[HelpHidden,DoesNotAffectIncrementalBuild]>,
  HelpText<"Do not add an rpath entry for the toolchain's standard library (default)">;
def no_stdlib_rpath: Flag<["-"], "no-stdlib-rpath">,
  Flags<[HelpHidden,DoesNotAffectIncrementalBuild]>,
  HelpText<"Don't add any rpath entries.">;

def static_executable : Flag<["-"], "static-executable">,
  HelpText<"Statically link the executable">;
def no_static_executable : Flag<["-"], "no-static-executable">,
  Flags<[HelpHidden]>,
  HelpText<"Don't statically link the executable">;

def use_ld : Joined<["-"], "use-ld=">,
  Flags<[DoesNotAffectIncrementalBuild]>,
  HelpText<"Specifies the linker to be used">;

def Xlinker : Separate<["-"], "Xlinker">,
  Flags<[DoesNotAffectIncrementalBuild]>,
  HelpText<"Specifies an option which should be passed to the linker">;

// Optimization levels

def O_Group : OptionGroup<"<optimization level options>">;

def Onone : Flag<["-"], "Onone">, Group<O_Group>,
  Flags<[FrontendOption, ModuleInterfaceOption]>,
  HelpText<"Compile without any optimization">;
def O : Flag<["-"], "O">, Group<O_Group>,
  Flags<[FrontendOption, ModuleInterfaceOption]>,
  HelpText<"Compile with optimizations">;
def Osize : Flag<["-"], "Osize">, Group<O_Group>,
  Flags<[FrontendOption, ModuleInterfaceOption]>,
  HelpText<"Compile with optimizations and target small code size">;
def Ounchecked : Flag<["-"], "Ounchecked">, Group<O_Group>,
  Flags<[FrontendOption, ModuleInterfaceOption]>,
  HelpText<"Compile with optimizations and remove runtime safety checks">;
def Oplayground : Flag<["-"], "Oplayground">, Group<O_Group>,
  Flags<[HelpHidden, FrontendOption, ModuleInterfaceOption]>,
  HelpText<"Compile with optimizations appropriate for a playground">;

def EnbaleDefaultCMO : Flag<["-"], "enable-default-cmo">,
  Flags<[HelpHidden, FrontendOption]>,
  HelpText<"Perform conservative cross-module optimization">;

def CrossModuleOptimization : Flag<["-"], "cross-module-optimization">,
  Flags<[HelpHidden, FrontendOption]>,
  HelpText<"Perform cross-module optimization">;

def disableCrossModuleOptimization : Flag<["-"], "disable-cmo">,
  Flags<[HelpHidden, FrontendOption]>,
  HelpText<"Disable cross-module optimization">;

def ExperimentalPerformanceAnnotations : Flag<["-"], "experimental-performance-annotations">,
  Flags<[HelpHidden, FrontendOption]>,
  HelpText<"Deprecated, has no effect">;

def RemoveRuntimeAsserts : Flag<["-"], "remove-runtime-asserts">,
  Flags<[FrontendOption]>,
  HelpText<"Remove runtime safety checks.">;
def AssumeSingleThreaded : Flag<["-"], "assume-single-threaded">,
  Flags<[FrontendOption, HelpHidden]>,
  HelpText<"Assume that code will be executed in a single-threaded "
           "environment">;

// Debug info options

def g_Group : OptionGroup<"<debug info options>">;

def g : Flag<["-"], "g">, Group<g_Group>, Flags<[FrontendOption]>,
  HelpText<"Emit debug info. "
           "This is the preferred setting for debugging with LLDB.">;
def gnone : Flag<["-"], "gnone">, Group<g_Group>, Flags<[FrontendOption]>,
  HelpText<"Don't emit debug info">;
def gline_tables_only : Flag<["-"], "gline-tables-only">,
  Group<g_Group>, Flags<[FrontendOption]>,
  HelpText<"Emit minimal debug info for backtraces only">;
def gdwarf_types : Flag<["-"], "gdwarf-types">,
  Group<g_Group>, Flags<[FrontendOption]>,
  HelpText<"Emit full DWARF type info.">;
def debug_prefix_map : Separate<["-"], "debug-prefix-map">,
    Flags<[FrontendOption]>,
    HelpText<"Remap source paths in debug info">, MetaVarName<"<prefix=replacement>">;
def coverage_prefix_map : Separate<["-"], "coverage-prefix-map">,
  Flags<[FrontendOption]>,
  HelpText<"Remap source paths in coverage info">, MetaVarName<"<prefix=replacement>">;
def file_prefix_map : Separate<["-"], "file-prefix-map">,
  Flags<[FrontendOption]>,
  HelpText<"Remap source paths in debug, coverage, and index info">, MetaVarName<"<prefix=replacement>">;

def file_compilation_dir : Separate<["-"], "file-compilation-dir">,
  Flags<[FrontendOption]>, MetaVarName<"<path>">,
  HelpText<"The compilation directory to embed in the debug info. Coverage mapping is not supported yet.">;

def debug_info_format : Joined<["-"], "debug-info-format=">,
  Flags<[FrontendOption]>,
  HelpText<"Specify the debug info format type to either 'dwarf' or 'codeview'">;

def prefix_serialized_debugging_options : Flag<["-"], "prefix-serialized-debugging-options">,
  Flags<[FrontendOption]>,
  HelpText<"Apply debug prefix mappings to serialized debug info in Swiftmodule files">;

// Verify debug info
def verify_debug_info : Flag<["-"], "verify-debug-info">,
  Flags<[NoInteractiveOption, DoesNotAffectIncrementalBuild]>,
  HelpText<"Verify the binary representation of debug output.">;

def debug_info_store_invocation : Flag<["-"], "debug-info-store-invocation">,
  Flags<[FrontendOption]>,
  HelpText<"Emit the compiler invocation in the debug info.">;

// Assert configuration identifiers.
def AssertConfig : Separate<["-"], "assert-config">,
  Flags<[FrontendOption, ModuleInterfaceOption]>,
  HelpText<"Specify the assert_configuration replacement. "
           "Possible values are Debug, Release, Unchecked, DisableReplacement.">;

// Code formatting options

def code_formatting_Group : OptionGroup<"<code formatting options>">;

def use_tabs : Flag<["-"], "use-tabs">, Group<code_formatting_Group>,
  Flags<[NoInteractiveOption, NoBatchOption, SwiftIndentOption]>,
  HelpText<"Use tabs for indentation.">;

def indent_switch_case : Flag<["-"], "indent-switch-case">,
  Group<code_formatting_Group>,
  Flags<[NoInteractiveOption, NoBatchOption, SwiftIndentOption]>,
  HelpText<"Indent cases in switch statements.">;

def in_place : Flag<["-"], "in-place">, Group<code_formatting_Group>,
  Flags<[NoInteractiveOption, NoBatchOption, SwiftIndentOption]>,
  HelpText<"Overwrite input file with formatted file.">;

def tab_width : Separate<["-"], "tab-width">, Group<code_formatting_Group>,
  Flags<[NoInteractiveOption, NoBatchOption, SwiftIndentOption]>,
  HelpText<"Width of tab character.">, MetaVarName<"<n>">;

def indent_width : Separate<["-"], "indent-width">, Group<code_formatting_Group>,
  Flags<[NoInteractiveOption, NoBatchOption, SwiftIndentOption]>,
  HelpText<"Number of characters to indent.">, MetaVarName<"<n>">;

def line_range : Separate<["-"], "line-range">, Group<code_formatting_Group>,
  Flags<[NoInteractiveOption, NoBatchOption, SwiftIndentOption]>,
  HelpText<"<start line>:<end line>. Formats a range of lines (1-based). "
           "Can only be used with one input file.">, MetaVarName<"<n:n>">;

// Migrator Options

def update_code : Flag<["-"], "update-code">,
  HelpText<"Update Swift code">,
  Flags<[FrontendOption, HelpHidden, NoInteractiveOption, DoesNotAffectIncrementalBuild]>;

def migrate_keep_objc_visibility: Flag<["-"], "migrate-keep-objc-visibility">,
  Flags<[FrontendOption, NoInteractiveOption]>,
  HelpText<"When migrating, add '@objc' to declarations that would've been implicitly visible in Swift 3">;

def disable_migrator_fixits: Flag<["-"], "disable-migrator-fixits">,
  Flags<[FrontendOption, NoInteractiveOption]>,
  HelpText<"Disable the Migrator phase which automatically applies fix-its">;

def emit_remap_file_path: Separate<["-"], "emit-remap-file-path">,
  Flags<[FrontendOption, NoDriverOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>,
  HelpText<"Emit the replacement map describing Swift Migrator changes to <path>">,
  MetaVarName<"<path>">;

def emit_migrated_file_path: Separate<["-"], "emit-migrated-file-path">,
  Flags<[FrontendOption, NoDriverOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>,
  HelpText<"Emit the migrated source file to <path>">,
  MetaVarName<"<path>">;

def dump_migration_states_dir: Separate<["-"], "dump-migration-states-dir">,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild,
         ArgumentIsPath]>,
  HelpText<"Dump the input text, output text, and states for migration to <path>">,
  MetaVarName<"<path>">;

def api_diff_data_file: Separate<["-"], "api-diff-data-file">,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild,
         ArgumentIsPath]>,
  HelpText<"API migration data is from <path>">,
  MetaVarName<"<path>">;

def api_diff_data_dir: Separate<["-"], "api-diff-data-dir">,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild,
         ArgumentIsPath]>,
  HelpText<"Load platform and version specific API migration data files from <path>. "
      "Ignored if -api-diff-data-file is specified.">,
  MetaVarName<"<path>">;

def dump_usr: Flag<["-"], "dump-usr">,
  Flags<[FrontendOption, NoInteractiveOption]>,
  HelpText<"Dump USR for each declaration reference">;

// FIXME: Remove these once Xcode stops appending these flags.
// rdar://problem/31844718

def migrator_update_sdk: Flag<["-"], "migrator-update-sdk">,
  Flags<[FrontendOption, NoInteractiveOption]>,
  HelpText<"Does nothing. Temporary compatibility flag for Xcode.">;

def migrator_update_swift: Flag<["-"], "migrator-update-swift">,
  Flags<[FrontendOption, NoInteractiveOption]>,
  HelpText<"Does nothing. Temporary compatibility flag for Xcode.">;

// File types

def parse_as_library : Flag<["-"], "parse-as-library">,
  Flags<[FrontendOption, NoInteractiveOption]>,
  HelpText<"Parse the input file(s) as libraries, not scripts">;
def parse_sil : Flag<["-"], "parse-sil">,
  Flags<[FrontendOption, NoInteractiveOption]>,
  HelpText<"Parse the input file as SIL code, not Swift source">;
def parse_stdlib : Flag<["-"], "parse-stdlib">,
  Flags<[FrontendOption, HelpHidden, ModuleInterfaceOption]>,
  HelpText<"Parse the input file(s) as the Swift standard library">;

def enable_builtin_module : Flag<["-"], "enable-builtin-module">,
  Flags<[FrontendOption, ModuleInterfaceOption]>,
  HelpText<"Enables the explicit import of the Builtin module">;

def modes_Group : OptionGroup<"<mode options>">, HelpText<"MODES">;

class ModeOpt : Group<modes_Group>;

// Output Modes
def emit_executable : Flag<["-"], "emit-executable">,
  HelpText<"Emit a linked executable">, ModeOpt,
  Flags<[NoInteractiveOption, DoesNotAffectIncrementalBuild]>;
def emit_library : Flag<["-"], "emit-library">,
  HelpText<"Emit a linked library">, ModeOpt,
  Flags<[NoInteractiveOption]>;
def emit_object : Flag<["-"], "emit-object">,
  HelpText<"Emit object file(s) (-c)">, ModeOpt,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>;
def emit_assembly : Flag<["-"], "emit-assembly">,
  HelpText<"Emit assembly file(s) (-S)">, ModeOpt,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>;
def emit_bc : Flag<["-"], "emit-bc">,
  HelpText<"Emit LLVM BC file(s)">, ModeOpt,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>;
def emit_irgen : Flag<["-"], "emit-irgen">,
  HelpText<"Emit LLVM IR file(s) before LLVM optimizations">, ModeOpt,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>;
def emit_ir : Flag<["-"], "emit-ir">,
  HelpText<"Emit LLVM IR file(s) after LLVM optimizations">, ModeOpt,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>;
def emit_sil : Flag<["-"], "emit-sil">,
  HelpText<"Emit canonical SIL file(s)">, ModeOpt,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>;
def emit_silgen : Flag<["-"], "emit-silgen">,
  HelpText<"Emit raw SIL file(s)">, ModeOpt,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>;
def emit_sib : Flag<["-"], "emit-sib">,
  HelpText<"Emit serialized AST + canonical SIL file(s)">, ModeOpt,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>;
def emit_sibgen : Flag<["-"], "emit-sibgen">,
  HelpText<"Emit serialized AST + raw SIL file(s)">, ModeOpt,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>;
def emit_imported_modules : Flag<["-"], "emit-imported-modules">,
  HelpText<"Emit a list of the imported modules">, ModeOpt,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>;
def emit_pcm : Flag<["-"], "emit-pcm">,
  HelpText<"Emit a precompiled Clang module from a module map">, ModeOpt,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>;

def c : Flag<["-"], "c">, Alias<emit_object>,
  Flags<[FrontendOption, NoInteractiveOption]>, ModeOpt;
def S: Flag<["-"], "S">, Alias<emit_assembly>,
  Flags<[FrontendOption, NoInteractiveOption]>, ModeOpt;

def fixit_all : Flag<["-"], "fixit-all">,
  HelpText<"Apply all fixits from diagnostics without any filtering">,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>;

// No Output Modes
def parse: Flag<["-"], "parse">,
  HelpText<"Parse input file(s)">, ModeOpt,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>;
def resolve_imports : Flag<["-"], "resolve-imports">,
  HelpText<"Parse and resolve imports in input file(s)">, ModeOpt,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>;
def typecheck : Flag<["-"], "typecheck">,
  HelpText<"Parse and type-check input file(s)">, ModeOpt,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>;
def dump_parse : Flag<["-"], "dump-parse">,
  HelpText<"Parse input file(s) and dump AST(s)">, ModeOpt,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>;
def emit_parse : Flag<["-"], "emit-parse">, Alias<dump_parse>,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>;
def dump_ast : Flag<["-"], "dump-ast">,
  HelpText<"Parse and type-check input file(s) and dump AST(s)">, ModeOpt,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>;
def emit_ast : Flag<["-"], "emit-ast">, Alias<dump_ast>,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>;
def dump_scope_maps : Separate<["-"], "dump-scope-maps">,
  HelpText<"Parse and type-check input file(s) and dump the scope map(s)">,
  MetaVarName<"<expanded-or-list-of-line:column>">,
  ModeOpt,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>;
def dump_type_refinement_contexts :
  Flag<["-"], "dump-type-refinement-contexts">,
  HelpText<"Type-check input file(s) and dump type refinement contexts(s)">,
  ModeOpt,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>;
def dump_type_info : Flag<["-"], "dump-type-info">,
  HelpText<"Output YAML dump of fixed-size types from all imported modules">,
  ModeOpt,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>;
def print_ast : Flag<["-"], "print-ast">,
  HelpText<"Parse and type-check input file(s) and pretty print AST(s)">,
  ModeOpt,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>;
def print_ast_decl : Flag<["-"], "print-ast-decl">,
  HelpText<"Parse and type-check input file(s) and pretty print declarations from AST(s)">,
  ModeOpt,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>;
def dump_pcm : Flag<["-"], "dump-pcm">,
  HelpText<"Dump debugging information about a precompiled Clang module">,
  ModeOpt,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>;

// Other Modes
def repl : Flag<["-"], "repl">,
  HelpText<"REPL mode (the default if there is no input file)">,
  Flags<[FrontendOption, NoBatchOption, HelpHidden]>, ModeOpt;
def lldb_repl : Flag<["-"], "lldb-repl">, HelpText<"LLDB-enhanced REPL mode">,
  Flags<[NoBatchOption, HelpHidden]>, ModeOpt;
def deprecated_integrated_repl : Flag<["-"], "deprecated-integrated-repl">,
  Flags<[FrontendOption, NoBatchOption]>, ModeOpt;
def i : Flag<["-"], "i">, ModeOpt; // only used to provide diagnostics.

def whole_module_optimization : Flag<["-"], "whole-module-optimization">,
  HelpText<"Optimize input files together instead of individually">,
  Flags<[FrontendOption, NoInteractiveOption]>;

def no_whole_module_optimization : Flag<["-"], "no-whole-module-optimization">,
  HelpText<"Disable optimizing input files together instead of individually">,
  Flags<[FrontendOption, NoInteractiveOption]>;

def enable_batch_mode : Flag<["-"], "enable-batch-mode">,
  Flags<[FrontendOption, NoInteractiveOption, HelpHidden]>,
  HelpText<"Enable combining frontend jobs into batches">;

def disable_batch_mode : Flag<["-"], "disable-batch-mode">,
  Flags<[FrontendOption, NoInteractiveOption, HelpHidden]>,
  HelpText<"Disable combining frontend jobs into batches">;

def wmo : Flag<["-"], "wmo">, Alias<whole_module_optimization>,
  Flags<[FrontendOption, NoInteractiveOption, HelpHidden]>;

def force_single_frontend_invocation :
  Flag<["-"], "force-single-frontend-invocation">,
  Alias<whole_module_optimization>,
  Flags<[FrontendOption, NoInteractiveOption, HelpHidden]>;

def num_threads : Separate<["-"], "num-threads">,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild]>,
  HelpText<"Enable multi-threading and specify number of threads">,
  MetaVarName<"<n>">;

def Xfrontend : Separate<["-"], "Xfrontend">, Flags<[HelpHidden]>,
  MetaVarName<"<arg>">, HelpText<"Pass <arg> to the Swift frontend">;

def Xcc : Separate<["-"], "Xcc">, Flags<[FrontendOption, SwiftSymbolGraphExtractOption]>,
  MetaVarName<"<arg>">,
  HelpText<"Pass <arg> to the C/C++/Objective-C compiler">;

def Xclang_linker : Separate<["-"], "Xclang-linker">, Flags<[HelpHidden]>,
  MetaVarName<"<arg>">, HelpText<"Pass <arg> to Clang when it is use for linking.">;

def Xllvm : Separate<["-"], "Xllvm">,
  Flags<[FrontendOption, HelpHidden]>,
  MetaVarName<"<arg>">, HelpText<"Pass <arg> to LLVM.">;

def resource_dir : Separate<["-"], "resource-dir">,
  Flags<[FrontendOption, SwiftSymbolGraphExtractOption, SwiftAPIDigesterOption, HelpHidden, ArgumentIsPath]>,
  MetaVarName<"</usr/lib/swift>">,
  HelpText<"The directory that holds the compiler resource files">;

def target : Separate<["-"], "target">,
  Flags<[FrontendOption, ModuleWrapOption, ModuleInterfaceOption, SwiftAPIExtractOption, SwiftSymbolGraphExtractOption,
         SwiftAPIDigesterOption]>,
  HelpText<"Generate code for the given target <triple>, such as x86_64-apple-macos10.9">, MetaVarName<"<triple>">;
def target_legacy_spelling : Joined<["--"], "target=">,
  Flags<[FrontendOption]>, Alias<target>;
def print_target_info : Flag<["-"], "print-target-info">,
    Flags<[FrontendOption]>,
    HelpText<"Print target information for the given target <triple>, such as x86_64-apple-macos10.9">, MetaVarName<"<triple>">;

def target_cpu : Separate<["-"], "target-cpu">, Flags<[FrontendOption, ModuleInterfaceOption]>,
  HelpText<"Generate code for a particular CPU variant">;

def target_variant : Separate<["-"], "target-variant">,
  Flags<[FrontendOption]>,
  HelpText<"Generate 'zippered' code for macCatalyst that can run on the specified"
           " variant target triple in addition to the main -target triple">;

def clang_target : Separate<["-"], "clang-target">,
  Flags<[FrontendOption, SwiftAPIExtractOption, SwiftSymbolGraphExtractOption, SwiftAPIDigesterOption]>,
  HelpText<"Separately set the target we should use for internal Clang instance">;

def disable_clang_target : Flag<["-"], "disable-clang-target">,
  Flags<[NewDriverOnlyOption]>,
  HelpText<"Disable a separately specified target triple for Clang instance to use">;

def min_inlining_target_version : Separate<["-"], "target-min-inlining-version">,
  Flags<[FrontendOption, ModuleInterfaceOptionIgnorable]>,
  HelpText<"Require inlinable code with no '@available' attribute to back-deploy "
           "to this version of the '-target' OS">;

def profile_generate : Flag<["-"], "profile-generate">,
  Flags<[FrontendOption, NoInteractiveOption]>,
  HelpText<"Generate instrumented code to collect execution counts">;

def profile_use : CommaJoined<["-"], "profile-use=">,
  Flags<[FrontendOption, NoInteractiveOption, ArgumentIsPath]>,
  MetaVarName<"<profdata>">,
  HelpText<"Supply a profdata file to enable profile-guided optimization">;

def profile_coverage_mapping : Flag<["-"], "profile-coverage-mapping">,
  Flags<[FrontendOption, NoInteractiveOption]>,
  HelpText<"Generate coverage data for use with profiled execution counts">;

def embed_bitcode : Flag<["-"], "embed-bitcode">,
  Flags<[FrontendOption, NoInteractiveOption]>,
  HelpText<"Embed LLVM IR bitcode as data">;

def embed_bitcode_marker : Flag<["-"], "embed-bitcode-marker">,
  Flags<[FrontendOption, NoInteractiveOption]>,
  HelpText<"Embed placeholder LLVM IR data as a marker">;

def enable_testing : Flag<["-"], "enable-testing">,
  Flags<[FrontendOption, NoInteractiveOption, HelpHidden]>,
  HelpText<"Allows this module's internal API to be accessed for testing">;

def enable_private_imports : Flag<["-"], "enable-private-imports">,
  Flags<[FrontendOption, NoInteractiveOption, HelpHidden]>,
  HelpText<"Allows this module's internal and private API to be accessed">;

def sanitize_EQ : CommaJoined<["-"], "sanitize=">,
  Flags<[FrontendOption, NoInteractiveOption]>, MetaVarName<"<check>">,
  HelpText<"Turn on runtime checks for erroneous behavior.">;

def sanitize_recover_EQ
    : CommaJoined<["-"], "sanitize-recover=">,
      Flags<[FrontendOption, NoInteractiveOption]>,
      MetaVarName<"<check>">,
      HelpText<"Specify which sanitizer runtime checks (see -sanitize=) will "
               "generate instrumentation that allows error recovery. Listed "
               "checks should be comma separated. Default behavior is to not "
               "allow error recovery.">;

def sanitize_address_use_odr_indicator
    : Flag<["-"], "sanitize-address-use-odr-indicator">,
      Flags<[FrontendOption, NoInteractiveOption, HelpHidden]>,
      HelpText<"When using AddressSanitizer enable ODR indicator globals to "
               "avoid false ODR violation reports in partially sanitized "
               "programs at the cost of an increase in binary size">;

def sanitize_coverage_EQ : CommaJoined<["-"], "sanitize-coverage=">,
  Flags<[FrontendOption, NoInteractiveOption]>,
  MetaVarName<"<type>">,
  HelpText<"Specify the type of coverage instrumentation for Sanitizers and"
  " additional options separated by commas">;

def scan_dependencies : Flag<["-"], "scan-dependencies">,
  HelpText<"Scan dependencies of the given Swift sources">, ModeOpt,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>;

def emit_supported_features : Flag<["-"], "emit-supported-features">,
  HelpText<"Emit a JSON file including all supported compiler features">, ModeOpt,
  Flags<[FrontendOption, NoInteractiveOption, DoesNotAffectIncrementalBuild]>;

def enable_incremental_imports :
  Flag<["-"], "enable-incremental-imports">,
  Flags<[FrontendOption]>,
  HelpText<"Enable cross-module incremental build metadata and "
           "driver scheduling for Swift modules">;

def disable_incremental_imports :
  Flag<["-"], "disable-incremental-imports">,
  Flags<[FrontendOption]>,
  HelpText<"Disable cross-module incremental build metadata and "
           "driver scheduling for Swift modules">;

def index_file : Flag<["-"], "index-file">,
  HelpText<"Produce index data for a source file">, ModeOpt,
  Flags<[NoInteractiveOption, DoesNotAffectIncrementalBuild]>;
def index_file_path : Separate<["-"], "index-file-path">,
  Flags<[NoInteractiveOption, DoesNotAffectIncrementalBuild, ArgumentIsPath]>,
  HelpText<"Produce index data for file <path>">,
  MetaVarName<"<path>">;

def index_store_path : Separate<["-"], "index-store-path">,
  Flags<[FrontendOption, ArgumentIsPath]>, MetaVarName<"<path>">,
  HelpText<"Store indexing data to <path>">;

def index_unit_output_path : Separate<["-"], "index-unit-output-path">,
  Flags<[FrontendOption, ArgumentIsPath]>, MetaVarName<"<path>">,
  HelpText<"Use <path> as the output path in the produced index data.">;

def index_ignore_clang_modules : Flag<["-"], "index-ignore-clang-modules">,
  Flags<[FrontendOption]>,
  HelpText<"Avoid indexing clang modules (pcms)">;

def index_include_locals : Flag<["-"], "index-include-locals">,
  Flags<[FrontendOption]>,
  HelpText<"Include local definitions/references in the produced index data.">;

def index_ignore_system_modules : Flag<["-"], "index-ignore-system-modules">,
  Flags<[NoInteractiveOption]>,
  HelpText<"Avoid indexing system modules">;

def enforce_exclusivity_EQ : Joined<["-"], "enforce-exclusivity=">,
  Flags<[FrontendOption, ModuleInterfaceOption]>,
  MetaVarName<"<enforcement>">,
  HelpText<"Enforce law of exclusivity">;

def working_directory : Separate<["-"], "working-directory">,
  HelpText<"Resolve file paths relative to the specified directory">,
  MetaVarName<"<path>">;
def working_directory_EQ : Joined<["-"], "working-directory=">,
  Alias<working_directory>;

def user_module_version : Separate<["-"], "user-module-version">,
  Flags<[FrontendOption, ModuleInterfaceOptionIgnorable, NewDriverOnlyOption]>,
  HelpText<"Module version specified from Swift module authors">,
  MetaVarName<"<vers>">;

def allowable_client : Separate<["-"], "allowable-client">,
  Flags<[FrontendOption, ModuleInterfaceOptionIgnorable, NewDriverOnlyOption]>,
  HelpText<"Module names that are allowed to import this module">,
  MetaVarName<"<vers>">;
// VFS

def vfsoverlay : JoinedOrSeparate<["-"], "vfsoverlay">,
  Flags<[FrontendOption, ArgumentIsPath, SwiftAPIExtractOption]>,
  HelpText<"Add directory to VFS overlay file">;
def vfsoverlay_EQ : Joined<["-"], "vfsoverlay=">,
  Alias<vfsoverlay>;

// Runtime compatibility version
def runtime_compatibility_version : Separate<["-"], "runtime-compatibility-version">,
  Flags<[FrontendOption]>,
  HelpText<"Link compatibility library for Swift runtime version, or 'none'">;

def disable_autolinking_runtime_compatibility : Flag<["-"], "disable-autolinking-runtime-compatibility">,
  Flags<[FrontendOption]>,
  HelpText<"Do not use autolinking for runtime compatibility libraries">;

def disable_autolinking_runtime_compatibility_dynamic_replacements
    : Flag<[ "-" ], "disable-autolinking-runtime-compatibility-dynamic-replacements">,
      Flags<[ FrontendOption ]>,
      HelpText<"Do not use autolinking for the dynamic replacement runtime "
               "compatibility library">;

def disable_autolinking_runtime_compatibility_concurrency
    : Flag<[ "-" ], "disable-autolinking-runtime-compatibility-concurrency">,
      Flags<[ FrontendOption ]>,
      HelpText<"Do not use autolinking for the concurrency runtime "
               "compatibility library">;

def enable_autolinking_runtime_compatibility_bytecode_layouts
    : Flag<[ "-" ], "enable-autolinking-runtime-compatibility-bytecode-layouts">,
      Flags<[ FrontendOption ]>,
      HelpText<"Enable autolinking for the bytecode layouts runtime "
               "compatibility library">;

def emit_symbol_graph: Flag<["-"], "emit-symbol-graph">,
  Flags<[FrontendOption, NoInteractiveOption, SupplementaryOutput, HelpHidden]>,
  HelpText<"Emit a symbol graph">;

def emit_symbol_graph_dir : Separate<["-"], "emit-symbol-graph-dir">,
  Flags<[FrontendOption, NoInteractiveOption, ArgumentIsPath,
         SupplementaryOutput, HelpHidden]>,
  HelpText<"Emit a symbol graph to directory <dir>">,
  MetaVarName<"<dir>">;

def symbol_graph_minimum_access_level: Separate<["-"], "symbol-graph-minimum-access-level">,
  Flags<[FrontendOption, NoInteractiveOption, SupplementaryOutput, HelpHidden]>,
  HelpText<"Include symbols with this access level or more when emitting a symbol graph">,
  MetaVarName<"<level>">;

def pretty_print: Flag<["-"], "pretty-print">,
  Flags<[SwiftAPIExtractOption, SwiftSymbolGraphExtractOption]>,
  HelpText<"Pretty-print the output JSON">;

def emit_extension_block_symbols: Flag<["-"], "emit-extension-block-symbols">,
  Flags<[SwiftSymbolGraphExtractOption, FrontendOption,
        NoInteractiveOption, SupplementaryOutput, HelpHidden]>,
  HelpText<"Emit 'swift.extension' symbols for extensions to external types instead of directly associating members and conformances with the extended nominal when generating symbol graphs">;

def omit_extension_block_symbols: Flag<["-"], "omit-extension-block-symbols">,
  Flags<[SwiftSymbolGraphExtractOption, FrontendOption,
        NoInteractiveOption, SupplementaryOutput, HelpHidden]>,
  HelpText<"Directly associate members and conformances with the extended nominal when generating symbol graphs instead of emitting 'swift.extension' symbols for extensions to external types">;

// swift-symbolgraph-extract-only options
def output_dir : Separate<["-"], "output-dir">,
  Flags<[NoDriverOption, SwiftSymbolGraphExtractOption, SwiftAPIDigesterOption,
         ArgumentIsPath]>,
  HelpText<"Output directory">,
  MetaVarName<"<dir>">;

def skip_synthesized_members: Flag<[ "-" ], "skip-synthesized-members">,
  Flags<[NoDriverOption, SwiftSymbolGraphExtractOption]>,
  HelpText<"Skip members inherited through classes or default implementations">;

def minimum_access_level : Separate<["-"], "minimum-access-level">,
  Flags<[NoDriverOption, SwiftSymbolGraphExtractOption]>,
  HelpText<"Include symbols with this access level or more">,
  MetaVarName<"<level>">;

def skip_inherited_docs : Flag<["-"], "skip-inherited-docs">,
  Flags<[SwiftSymbolGraphExtractOption, FrontendOption,
         NoInteractiveOption, SupplementaryOutput, HelpHidden]>,
  HelpText<"Skip emitting doc comments for members inherited through classes or "
           "default implementations">;

def include_spi_symbols : Flag<["-"], "include-spi-symbols">,
  Flags<[SwiftSymbolGraphExtractOption, FrontendOption,
         NoInteractiveOption, SupplementaryOutput, HelpHidden]>,
  HelpText<"Add symbols with SPI information to the symbol graph">;

def skip_protocol_implementations : Flag<["-"], "skip-protocol-implementations">,
  Flags<[SwiftSymbolGraphExtractOption, FrontendOption,
         NoInteractiveOption, SupplementaryOutput, HelpHidden]>,
  HelpText<"Skip emitting symbols that are implementations of protocol requirements or inherited from protocl extensions">;

// swift-api-digester-only options
def dump_sdk: Flag<["-", "--"], "dump-sdk">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption]>,
  HelpText<"Dump SDK content to JSON file">;

def generate_migration_script: Flag<["-", "--"], "generate-migration-script">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption]>,
  HelpText<"Compare SDK content in JSON file and generate migration script">;

def diagnose_sdk: Flag<["-", "--"], "diagnose-sdk">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption]>,
  HelpText<"Diagnose SDK content in JSON file">;

def deserialize_diff: Flag<["-", "--"], "deserialize-diff">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption]>,
  HelpText<"Deserialize diff items in a JSON file">;

def deserialize_sdk: Flag<["-", "--"], "deserialize-sdk">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption]>,
  HelpText<"Deserialize sdk digester in a JSON file">;

def find_usr: Flag<["-", "--"], "find-usr">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption]>,
  HelpText<"Find USR for decls by given condition">;

def generate_name_correction: Flag<["-", "--"], "generate-name-correction">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption]>,
  HelpText<"Generate name correction template">;

def generate_empty_baseline: Flag<["-", "--"], "generate-empty-baseline">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption]>,
  HelpText<"Generate an empty baseline">;

def empty_baseline: Flag<["-", "--"], "empty-baseline">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption]>,
  HelpText<"Use empty baseline for diagnostics">;

def ignored_usrs: Separate<["-", "--"], "ignored-usrs">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption, ArgumentIsPath]>,
  HelpText<"the file containing USRs of removed decls that the digester should ignore">,
  MetaVarName<"<path>">;

def ignore_spi_groups : Separate<["-", "--"], "ignore-spi-group">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption]>,
  HelpText<"SPI group name to not diagnose about">;

def protocol_requirement_allow_list: Separate<["-", "--"], "protocol-requirement-allow-list">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption, ArgumentIsPath]>,
  HelpText<"File containing a new-line separated list of protocol names">,
  MetaVarName<"<path>">;

def input_paths: Separate<["-", "--"], "input-paths">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption, ArgumentIsPath]>,
  HelpText<"The SDK contents under comparison">,
  MetaVarName<"<path>">;

def compiler_style_diags: Flag<["-", "--"], "compiler-style-diags">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption]>,
  HelpText<"Print compiler style diagnostics to stderr.">;

def json: Flag<["-", "--"], "json">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption]>,
  HelpText<"Print output in JSON format.">;

def avoid_location: Flag<["-", "--"], "avoid-location">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption]>,
  HelpText<"Avoid serializing the file paths of SDK nodes.">;

def location: Separate<["-", "--"], "location">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption]>,
  HelpText<"Filter nodes with the given location.">,
  MetaVarName<"<location>">;

def avoid_tool_args: Flag<["-", "--"], "avoid-tool-args">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption]>,
  HelpText<"Avoid serializing the arguments for invoking the tool.">;

def abi: Flag<["-", "--"], "abi">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption]>,
  HelpText<"Dumping ABI interface">;

def swift_only: Flag<["-", "--"], "swift-only">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption]>,
  HelpText<"Only include APIs defined from Swift source">;

def disable_os_checks: Flag<["-", "--"], "disable-os-checks">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption]>,
  HelpText<"Skip OS related diagnostics">;

def disable_remove_deprecated_check: Flag<["-", "--"], "disable-remove-deprecated-check">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption]>,
  HelpText<"Skip diagnosing removal of deprecated symbols">;

def enable_remove_deprecated_check: Flag<["-", "--"], "enable-remove-deprecated-check">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption]>,
  HelpText<"Diagnosing removal of deprecated symbols">;

def print_module: Flag<["-", "--"], "print-module">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption]>,
  HelpText<"Print module names in diagnostics">;

def migrator: Flag<["-", "--"], "migrator">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption]>,
  HelpText<"Dump Json suitable for generating migration script">;

def abort_on_module_fail: Flag<["-", "--"], "abort-on-module-fail">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption]>,
  HelpText<"Abort if a module failed to load">;

def disable_fail_on_error: Flag<["-", "--"], "disable-fail-on-error">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption]>,
  HelpText<"Don't exit with a nonzero status if errors are emitted">;

def debug_mapping: Flag<["-", "--"], "debug-mapping">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption]>,
  HelpText<"Dumping information for debug purposes">;

def BF : JoinedOrSeparate<["-"], "BF">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption, ArgumentIsPath]>,
  HelpText<"add a directory to the baseline framework search path">;
def BF_EQ : Joined<["-"], "BF=">, Flags<[NoDriverOption, SwiftAPIDigesterOption]>,
  Alias<BF>;

def BI : JoinedOrSeparate<["-"], "BI">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption, ArgumentIsPath]>,
  HelpText<"add a module for baseline input">;
def BI_EQ : Joined<["-"], "BI=">, Flags<[NoDriverOption, SwiftAPIDigesterOption]>,
  Alias<BI>;

def iframework : JoinedOrSeparate<["-"], "iframework">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption, ArgumentIsPath]>,
  HelpText<"add a directory to the clang importer system framework search path">;

def baseline_path : JoinedOrSeparate<["-"], "baseline-path">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption, ArgumentIsPath]>,
  HelpText<"The path to the Json file that we should use as the baseline">;

def baseline_dir : JoinedOrSeparate<["-"], "baseline-dir">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption, ArgumentIsPath]>,
  HelpText<"The path to a directory containing baseline files: macos.json, iphoneos.json, appletvos.json, watchos.json, and iosmac.json">;

def breakage_allowlist_path : JoinedOrSeparate<["-"], "breakage-allowlist-path">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption, ArgumentIsPath]>,
  HelpText<"An allowlist of breakages to not complain about">;

def bsdk : JoinedOrSeparate<["-"], "bsdk">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption, ArgumentIsPath]>,
  HelpText<"path to the baseline SDK to import frameworks">;

def module_list_file : JoinedOrSeparate<["-"], "module-list-file">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption, ArgumentIsPath]>,
  HelpText<"File containing a new-line separated list of modules">;

def module: Separate<["-", "--"], "module">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption]>,
  HelpText<"Names of modules">,
  MetaVarName<"<name>">;

def use_interface_for_module: Separate<["-", "--"], "use-interface-for-module">,
  Flags<[NoDriverOption, SwiftAPIDigesterOption]>,
  HelpText<"Prefer loading these modules via interface">,
  MetaVarName<"<name>">;

// ONLY SUPPORTED IN NEW DRIVER

// These flags only exist here so that the old driver doesn't fail with unknown
// arguments when they're given. The option definitions (and ie. their flags)
// matter since they are synced to the new swift-driver. The options themselves
// are completely unimplemented in the old driver, however.

def driver_print_graphviz:
  Flag<["-"], "driver-print-graphviz">,
  Flags<[HelpHidden, DoesNotAffectIncrementalBuild, NewDriverOnlyOption]>,
  HelpText<"Write the job graph as a graphviz file">;

def driver_explicit_module_build:
  Flag<["-"], "explicit-module-build">,
  Flags<[HelpHidden, NewDriverOnlyOption]>,
  HelpText<"Prebuild module dependencies to make them explicit">;

def driver_experimental_explicit_module_build:
  Flag<["-"], "experimental-explicit-module-build">,
  Flags<[HelpHidden, NewDriverOnlyOption]>,
  Alias<driver_explicit_module_build>,
  HelpText<"Prebuild module dependencies to make them explicit">;

def driver_scan_dependencies_non_lib:
  Flag<["-"], "nonlib-dependency-scanner">,
  Flags<[HelpHidden, NewDriverOnlyOption]>,
  HelpText<"Use calls to `swift-frontend -scan-dependencies` instead of "
           "dedicated dependency scanning library">;

def driver_warn_unused_options:
  Flag<["-"], "driver-warn-unused-options">,
  Flags<[HelpHidden, NewDriverOnlyOption]>,
  HelpText<"Emit warnings for any provided options which are unused by the "
           "driver">;

def emit_module_separately:
  Flag<["-"], "experimental-emit-module-separately">,
  Flags<[HelpHidden, NewDriverOnlyOption]>,
  HelpText<"Emit module files as a distinct job">;

def no_emit_module_separately:
  Flag<["-"], "no-emit-module-separately">,
  Flags<[HelpHidden, NewDriverOnlyOption]>,
  HelpText<"Force using merge-module as the incremental build mode">;

def emit_module_separately_WMO:
  Flag<["-"], "emit-module-separately-wmo">,
  Flags<[HelpHidden, NewDriverOnlyOption]>,
  HelpText<"Emit module files as a distinct job in wmo builds">;

def no_emit_module_separately_WMO:
  Flag<["-"], "no-emit-module-separately-wmo">,
  Flags<[HelpHidden, NewDriverOnlyOption]>,
  HelpText<"Force emitting the swiftmodule in the same job in wmo builds">;

def emit_module_serialize_diagnostics_path:
  Separate<["-"], "emit-module-serialize-diagnostics-path">,
  Flags<[ArgumentIsPath, SupplementaryOutput, NewDriverOnlyOption]>,
  HelpText<"Emit a serialized diagnostics file for the emit-module task to "
           "<path>">,
  MetaVarName<"<path>">;

def emit_module_dependencies_path:
  Separate<["-"], "emit-module-dependencies-path">,
  Flags<[ArgumentIsPath, SupplementaryOutput, NewDriverOnlyOption]>,
  HelpText<"Emit a discovered dependencies file for the emit-module task to "
           "<path>">,
  MetaVarName<"<path>">;

def use_frontend_parseable_output:
  Flag<["-"], "use-frontend-parseable-output">,
  Flags<[HelpHidden, NewDriverOnlyOption]>,
  HelpText<"Emit parseable-output from swift-frontend jobs instead of from "
           "the driver">;

def print_explicit_dependency_graph:
  Flag<["-"], "print-explicit-dependency-graph">,
  Flags<[DoesNotAffectIncrementalBuild, HelpHidden, NewDriverOnlyOption]>,
  HelpText<"Print the result of module dependency scanning after external "
           "module resolution to output">;

def explicit_dependency_graph_format:
  Joined<["-"], "explicit-dependency-graph-format=">,
  Flags<[DoesNotAffectIncrementalBuild, HelpHidden, NewDriverOnlyOption]>,
  HelpText<"Specify the explicit dependency graph output format to either 'json' or 'dot'">;

def print_preprocessed_explicit_dependency_graph:
  Flag<["-"], "print-preprocessed-explicit-dependency-graph">,
  Flags<[HelpHidden, NewDriverOnlyOption]>,
  HelpText<"Print the result of module dependency scanning to output">;

def emit_digester_baseline:
  Flag<["-"], "emit-digester-baseline">,
  Flags<[NoInteractiveOption, SupplementaryOutput, NewDriverOnlyOption]>,
  HelpText<"Emit a baseline file for the module using the API digester">;

def emit_digester_baseline_path:
  Separate<["-"], "emit-digester-baseline-path">,
  Flags<[NoInteractiveOption, ArgumentIsPath, SupplementaryOutput,
         NewDriverOnlyOption]>,
  HelpText<"Emit a baseline file for the module to <path> using the API "
           "digester">,
  MetaVarName<"<path>">;

def compare_to_baseline_path:
  Separate<["-"], "compare-to-baseline-path">,
  Flags<[NoInteractiveOption, ArgumentIsPath, NewDriverOnlyOption]>,
  HelpText<"Compare the built module to the baseline at <path> and diagnose "
           "breaking changes using the API digester">,
  MetaVarName<"<path>">;

def serialize_breaking_changes_path:
  Separate<["-"], "serialize-breaking-changes-path">,
  Flags<[NoInteractiveOption, ArgumentIsPath, NewDriverOnlyOption]>,
  HelpText<"Serialize breaking changes found by the API digester to <path>">,
  MetaVarName<"<path>">;

def digester_breakage_allowlist_path:
  Separate<["-"], "digester-breakage-allowlist-path">,
  Flags<[NoInteractiveOption, ArgumentIsPath, NewDriverOnlyOption]>,
  HelpText<"The path to a list of permitted breaking changes the API digester "
           "should ignore">,
  MetaVarName<"<path>">;

def digester_mode:
  Separate<["-"], "digester-mode">,
  Flags<[NoInteractiveOption, NewDriverOnlyOption]>,
  HelpText<"Whether the API digester should run in API or ABI mode (defaults "
           "to API checking)">,
  MetaVarName<"<api|abi>">;

def nostartfiles:
  Flag<["-"], "nostartfiles">,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild, NoInteractiveOption,
         HelpHidden, NewDriverOnlyOption]>,
  HelpText<"Do not link in the Swift language startup routines">;

def gcc_toolchain: Separate<["-"], "gcc-toolchain">,
  Flags<[HelpHidden, NewDriverOnlyOption, ArgumentIsPath]>,
  MetaVarName<"<path>">,
  HelpText<"Specify a directory where the clang importer and clang linker can find headers and libraries">;

def cas_path: Separate<["-"], "cas-path">,
  Flags<[FrontendOption, NewDriverOnlyOption]>,
  HelpText<"Path to CAS">, MetaVarName<"<path>">;

def allow_unstable_cache_key_for_testing: Flag<["-"], "allow-unstable-cache-key-for-testing">,
  Flags<[FrontendOption, HelpHidden, NoDriverOption]>,
  HelpText<"Allow compilation caching with unstable inputs for testing purpose">;

// END ONLY SUPPORTED IN NEW DRIVER

def enable_cas: Flag<["-"], "enable-cas">,
  Flags<[FrontendOption, NoDriverOption]>,
  HelpText<"Enable CAS for swift-frontend">;

def cas_fs: Separate<["-"], "cas-fs">,
  Flags<[FrontendOption, NoDriverOption]>,
  HelpText<"Root CASID for CAS FileSystem">, MetaVarName<"<cas-id>">;

def load_plugin_library:
  Separate<["-"], "load-plugin-library">,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild, ArgumentIsPath]>,
  HelpText<"Path to a dynamic library containing compiler plugins such as "
           "macros">,
  MetaVarName<"<path>">;

def load_plugin_executable:
  Separate<["-"], "load-plugin-executable">,
  Flags<[FrontendOption, DoesNotAffectIncrementalBuild, ArgumentIsPath]>,
  HelpText<"Path to an executable compiler plugins and providing module names "
           "such as macros">,
  MetaVarName<"<path>#<module-names>">;

include "FrontendOptions.td"
