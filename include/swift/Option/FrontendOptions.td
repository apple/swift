//===--- FrontendOptions.td - Options for swift -frontend -----------------===//
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
//
//  This file defines the options accepted by swift -frontend.
//
//===----------------------------------------------------------------------===//

let Flags = [FrontendOption, NoDriverOption] in {

def triple : Separate<["-"], "triple">, Alias<target>;

def primary_file : Separate<["-"], "primary-file">,
  HelpText<"Produce output for this file, not the whole module">;

def filelist : Separate<["-"], "filelist">,
  HelpText<"Specify source inputs in a file rather than on the command line">;
def primary_filelist : Separate<["-"], "primary-filelist">,
  HelpText<"Specify primary inputs in a file rather than on the command line">;
def output_filelist : Separate<["-"], "output-filelist">,
  HelpText<"Specify outputs in a file rather than on the command line">;
def supplementary_output_file_map : Separate<["-"], "supplementary-output-file-map">,
  HelpText<"Specify supplementary outputs in a file rather than on the command line">;

def frontend_parseable_output : Flag<["-"], "frontend-parseable-output">,
  HelpText<"Emit textual output in a parseable format">;

def emit_module_doc : Flag<["-"], "emit-module-doc">,
  HelpText<"Emit a module documentation file based on documentation "
           "comments">;
def emit_module_doc_path
  : Separate<["-"], "emit-module-doc-path">, MetaVarName<"<path>">,
    HelpText<"Output module documentation file <path>">;

def emit_module_source_info : Flag<["-"], "emit-module-source-info">,
  HelpText<"Output module source info file">;

def ignore_module_source_info : Flag<["-"], "ignore-module-source-info">,
  HelpText<"Avoid getting source location from .swiftsourceinfo files">;

def merge_modules
  : Flag<["-"], "merge-modules">, ModeOpt,
    HelpText<"Merge the input modules without otherwise processing them">;

def emit_dependencies_path
  : Separate<["-"], "emit-dependencies-path">, MetaVarName<"<path>">,
    HelpText<"Output basic Make-compatible dependencies file to <path>">;

def emit_reference_dependencies : Flag<["-"], "emit-reference-dependencies">,
  HelpText<"Emit a Swift-style dependencies file">;
def emit_reference_dependencies_path
  : Separate<["-"], "emit-reference-dependencies-path">, MetaVarName<"<path>">,
    HelpText<"Output Swift-style dependencies file to <path>">;

def emit_fixits_path
  : Separate<["-"], "emit-fixits-path">, MetaVarName<"<path>">,
    HelpText<"Output compiler fixits as source edits to <path>">;

def emit_abi_descriptor_path
  : Separate<["-"], "emit-abi-descriptor-path">, MetaVarName<"<path>">,
    HelpText<"Output the ABI descriptor of current module to <path>">;

def emit_module_semantic_info_path
  : Separate<["-"], "emit-module-semantic-info-path">, MetaVarName<"<path>">,
    HelpText<"Output semantic info of current module to <path>">;

def serialize_module_interface_dependency_hashes
  : Flag<["-"], "serialize-module-interface-dependency-hashes">,
    Flags<[HelpHidden]>;

def serialize_parseable_module_interface_dependency_hashes
  : Flag<["-"], "serialize-parseable-module-interface-dependency-hashes">,
    Alias<serialize_module_interface_dependency_hashes>,
    Flags<[HelpHidden]>;

def tbd_install_name
  : Separate<["-"], "tbd-install_name">, MetaVarName<"<path>">,
    HelpText<"The install_name to use in an emitted TBD file">;

def tbd_install_name_EQ : Joined<["-"], "tbd-install_name=">,
  Alias<tbd_install_name>;

def tbd_current_version
  : Separate<["-"], "tbd-current-version">, MetaVarName<"<version>">,
  HelpText<"The current_version to use in an emitted TBD file">;

def tbd_current_version_EQ : Joined<["-"], "tbd-current-version=">,
  Alias<tbd_current_version>;

def tbd_compatibility_version
  : Separate<["-"], "tbd-compatibility-version">, MetaVarName<"<version>">,
  HelpText<"The compatibility_version to use in an emitted TBD file">;

def tbd_compatibility_version_EQ : Joined<["-"], "tbd-compatibility-version=">,
  Alias<tbd_compatibility_version>;

def tbd_is_installapi: Flag<["-"], "tbd-is-installapi">,
  HelpText<"If the TBD file should indicate it's being generated during "
           "InstallAPI">;

def verify : Flag<["-"], "verify">,
  HelpText<"Verify diagnostics against expected-{error|warning|note} "
           "annotations">;
def verify_additional_file : Separate<["-"], "verify-additional-file">,
  HelpText<"Verify diagnostics in this file in addition to source files">;
def verify_apply_fixes : Flag<["-"], "verify-apply-fixes">,
  HelpText<"Like -verify, but updates the original source file">;
def verify_ignore_unknown: Flag<["-"], "verify-ignore-unknown">,
  HelpText<"Allow diagnostics for '<unknown>' location in verify mode">;
def verify_generic_signatures : Separate<["-"], "verify-generic-signatures">,
  MetaVarName<"<module-name>">,
  HelpText<"Verify the generic signatures in the given module">;

def show_diagnostics_after_fatal : Flag<["-"], "show-diagnostics-after-fatal">,
  HelpText<"Keep emitting subsequent diagnostics after a fatal error">;
  
def enable_cross_import_overlays : Flag<["-"], "enable-cross-import-overlays">,
  HelpText<"Automatically import declared cross-import overlays.">;
def disable_cross_import_overlays : Flag<["-"], "disable-cross-import-overlays">,
  HelpText<"Do not automatically import declared cross-import overlays.">;
  
def diagnostic_documentation_path
  : Separate<["-"], "diagnostic-documentation-path">, MetaVarName<"<path>">,
  HelpText<"Path to diagnostic documentation resources">;

def enable_testable_attr_requires_testable_module :
  Flag<["-"], "enable-testable-attr-requires-testable-module">,
  HelpText<"Enable checking of @testable">;

def disable_testable_attr_requires_testable_module :
  Flag<["-"], "disable-testable-attr-requires-testable-module">,
  HelpText<"Disable checking of @testable">;

def disable_clang_spi :
  Flag<["-"], "disable-clang-spi">,
  HelpText<"Don't import Clang SPIs as Swift SPIs">;

def enable_target_os_checking :
  Flag<["-"], "enable-target-os-checking">,
  HelpText<"Enable checking the target OS of serialized modules">;

def disable_target_os_checking :
  Flag<["-"], "disable-target-os-checking">,
  HelpText<"Disable checking the target OS of serialized modules">;
  
def crosscheck_unqualified_lookup : Flag<["-"], "crosscheck-unqualified-lookup">,
  HelpText<"Compare legacy DeclContext- to ASTScope-based unqualified name lookup (for debugging)">;

def use_clang_function_types : Flag<["-"], "use-clang-function-types">,
  HelpText<"Use stored Clang function types for computing canonical types.">;

def print_clang_stats : Flag<["-"], "print-clang-stats">,
  HelpText<"Print Clang importer statistics">;

def serialize_debugging_options : Flag<["-"], "serialize-debugging-options">,
  HelpText<"Always serialize options for debugging (default: only for apps)">;

def serialized_path_obfuscate : Separate<["-"], "serialized-path-obfuscate">,
    HelpText<"Remap source paths in debug info">, MetaVarName<"<prefix=replacement>">;

def empty_abi_descriptor : Flag<["-"], "empty-abi-descriptor">,
  HelpText<"Avoid printing actual module content into ABI descriptor file">;

def no_serialize_debugging_options :
  Flag<["-"], "no-serialize-debugging-options">,
  HelpText<"Never serialize options for debugging (default: only for apps)">;

def autolink_library : Separate<["-"], "autolink-library">,
  HelpText<"Add dependent library">, Flags<[FrontendOption]>;

def public_autolink_library : Separate<["-"], "public-autolink-library">,
  HelpText<"Add public dependent library">, Flags<[FrontendOption]>;

def disable_typo_correction : Flag<["-"], "disable-typo-correction">,
  HelpText<"Disable typo correction">;

def disable_implicit_swift_modules: Flag<["-"], "disable-implicit-swift-modules">,
  HelpText<"Disable building Swift modules implicitly by the compiler">;

def swift_module_file: Joined<["-"], "swift-module-file=">,
  MetaVarName<"<name>=<path>">,
  HelpText<"Specify Swift module input explicitly built from textual interface">;

def explicit_swift_module_map
  : Separate<["-"], "explicit-swift-module-map-file">, MetaVarName<"<path>">,
    HelpText<"Specify a JSON file containing information of explicit Swift modules">;

def const_gather_protocols_file
  : Separate<["-"], "const-gather-protocols-file">, MetaVarName<"<path>">,
    HelpText<"Specify a list of protocols for extraction of conformances' const values'">;

def placeholder_dependency_module_map
  : Separate<["-"], "placeholder-dependency-module-map-file">, MetaVarName<"<path>">,
    HelpText<"Specify a JSON file containing information of external Swift module dependencies">;

def batch_scan_input_file
  : Separate<["-"], "batch-scan-input-file">, MetaVarName<"<path>">,
    HelpText<"Specify a JSON file containing modules to perform batch dependencies scanning">;

def import_prescan : Flag<["-"], "import-prescan">,
   HelpText<"When performing a dependency scan, only identify all imports of the main Swift module sources">;

def serialize_dependency_scan_cache : Flag<["-"], "serialize-dependency-scan-cache">,
   HelpText<"After performing a dependency scan, serialize the scanner's internal state.">;

def reuse_dependency_scan_cache : Flag<["-"], "load-dependency-scan-cache">,
   HelpText<"After performing a dependency scan, serialize the scanner's internal state.">;

def dependency_scan_cache_path : Separate<["-"], "dependency-scan-cache-path">,
  HelpText<"The path to output the dependency scanner's internal state.">;

def dependency_scan_cache_remarks : Flag<["-"], "Rdependency-scan-cache">,
  HelpText<"Emit remarks indicating use of the serialized module dependency scanning cache.">;

def enable_copy_propagation : Flag<["-"], "enable-copy-propagation">,
  HelpText<"Run SIL copy propagation with lexical lifetimes to shorten object "
           "lifetimes while preserving variable lifetimes.">;
def copy_propagation_state_EQ :
  Joined<["-"], "enable-copy-propagation=">,
  HelpText<"Whether to enable copy propagation">,
  MetaVarName<"true|requested-passes-only|false">;

def enable_infer_public_concurrent_value : Flag<["-"], "enable-infer-public-sendable">,
    HelpText<"Enable inference of Sendable conformances for public structs and enums">;

def disable_infer_public_concurrent_value : Flag<["-"], "disable-infer-public-sendable">,
    HelpText<"Disable inference of Sendable conformances for public structs and enums">;

def Raccess_note : Separate<["-"], "Raccess-note">,
    MetaVarName<"none|failures|all|all-validate">,
    HelpText<"Control access note remarks (default: all)">;
def Raccess_note_EQ : Joined<["-"], "Raccess-note=">,
    Alias<Raccess_note>;

def block_list_file
  : Separate<["-"], "blocklist-file">, MetaVarName<"<path>">,
    HelpText<"The path to a blocklist configuration file">;
} // end let Flags = [FrontendOption, NoDriverOption]

def debug_crash_Group : OptionGroup<"<automatic crashing options>">;
class DebugCrashOpt : Group<debug_crash_Group>;

let Flags = [FrontendOption, NoDriverOption, HelpHidden, ModuleInterfaceOptionIgnorable] in {
  def enable_lexical_lifetimes :
    Joined<["-"], "enable-lexical-lifetimes=">,
    HelpText<"Whether to enable lexical lifetimes">,
    MetaVarName<"true|false">;
  def enable_lexical_lifetimes_noArg :
    Flag<["-"], "enable-lexical-lifetimes">,
    HelpText<"Enable lexical lifetimes">;
  def enable_destroy_hoisting :
    Joined<["-"], "enable-destroy-hoisting=">,
    HelpText<"Whether to enable destroy hoisting">,
    MetaVarName<"true|false">;
}

// Flags that are saved into module interfaces
let Flags = [FrontendOption, NoDriverOption, HelpHidden, ModuleInterfaceOption] in {
  def enable_objc_interop :
    Flag<["-"], "enable-objc-interop">,
    HelpText<"Enable Objective-C interop code generation and config directives">;

  def disable_objc_interop :
    Flag<["-"], "disable-objc-interop">,
    HelpText<"Disable Objective-C interop code generation and config directives">;

  def enable_objc_attr_requires_foundation_module :
    Flag<["-"], "enable-objc-attr-requires-foundation-module">,
    HelpText<"Enable requiring uses of @objc to require importing the "
             "Foundation module">;

  def disable_objc_attr_requires_foundation_module :
    Flag<["-"], "disable-objc-attr-requires-foundation-module">,
    HelpText<"Disable requiring uses of @objc to require importing the "
             "Foundation module">;

def enable_experimental_concurrency :
  Flag<["-"], "enable-experimental-concurrency">,
  HelpText<"Enable experimental concurrency model">;

def enable_lexical_borrow_scopes :
  Joined<["-"], "enable-lexical-borrow-scopes=">,
  HelpText<"Whether to emit lexical borrow scopes (default: true)">,
  MetaVarName<"true|false">;

def enable_experimental_move_only :
  Flag<["-"], "enable-experimental-move-only">,
  HelpText<"Enable experimental move only">;

def enable_experimental_distributed :
  Flag<["-"], "enable-experimental-distributed">,
  HelpText<"Enable experimental 'distributed' actors and functions">;

def enable_experimental_flow_sensitive_concurrent_captures :
  Flag<["-"], "enable-experimental-flow-sensitive-concurrent-captures">,
  HelpText<"Enable flow-sensitive concurrent captures">;

def disable_experimental_clang_importer_diagnostics :
  Flag<["-"], "disable-experimental-clang-importer-diagnostics">,
  HelpText<"Disable experimental diagnostics when importing C, C++, and Objective-C libraries">;

def enable_experimental_eager_clang_module_diagnostics :
  Flag<["-"], "enable-experimental-eager-clang-module-diagnostics">,
  HelpText<"Enable experimental eager diagnostics reporting on the importability of all referenced C, C++, and Objective-C libraries">;

def enable_experimental_pairwise_build_block :
  Flag<["-"], "enable-experimental-pairwise-build-block">,
  HelpText<"Enable experimental pairwise 'buildBlock' for result builders">;

def enable_resilience : Flag<["-"], "enable-resilience">,
     HelpText<"Deprecated, use -enable-library-evolution instead">;

def enable_experimental_async_top_level :
  Flag<["-"], "enable-experimental-async-top-level">,
  HelpText<"Enable experimental concurrency in top-level code">;
}

// HIDDEN FLAGS
let Flags = [FrontendOption, NoDriverOption, HelpHidden] in {

def debug_constraints : Flag<["-"], "debug-constraints">,
  HelpText<"Debug the constraint-based type checker">;

def debug_constraints_attempt : Separate<["-"], "debug-constraints-attempt">,
  HelpText<"Debug the constraint solver at a given attempt">;

def debug_constraints_on_line : Separate<["-"], "debug-constraints-on-line">,
  HelpText<"Debug the constraint solver for expressions on <line>">,
  MetaVarName<"<line>">;
def debug_constraints_on_line_EQ : Joined<["-"], "debug-constraints-on-line=">,
  Alias<debug_constraints_on_line>;

def disable_named_lazy_member_loading : Flag<["-"], "disable-named-lazy-member-loading">,
  HelpText<"Disable per-name lazy member loading">;

def dump_requirement_machine : Flag<["-"], "dump-requirement-machine">,
  HelpText<"Enables dumping rewrite systems from the generics implementation">;

def debug_requirement_machine : Joined<["-"], "debug-requirement-machine=">,
  HelpText<"Fine-grained debug output from the generics implementation">;

def dump_macro_expansions : Flag<["-"], "dump-macro-expansions">,
  HelpText<"Dumps the results of each macro expansion">;
def emit_macro_expansion_files : Separate<["-"], "emit-macro-expansion-files">,
  HelpText<"Specify when to emit macro expansion file: 'none', 'debug', or 'diagnostics'">;

def analyze_requirement_machine : Flag<["-"], "analyze-requirement-machine">,
  Flags<[FrontendOption, HelpHidden, DoesNotAffectIncrementalBuild]>,
  HelpText<"Print out requirement machine statistics at the end of the compilation job">;

def requirement_machine_max_rule_count : Joined<["-"], "requirement-machine-max-rule-count=">,
  Flags<[FrontendOption, HelpHidden, DoesNotAffectIncrementalBuild]>,
  HelpText<"Set the maximum number of rules before giving up">;

def requirement_machine_max_rule_length : Joined<["-"], "requirement-machine-max-rule-length=">,
  Flags<[FrontendOption, HelpHidden, DoesNotAffectIncrementalBuild]>,
  HelpText<"Set the maximum rule length before giving up">;

def requirement_machine_max_concrete_nesting : Joined<["-"], "requirement-machine-max-concrete-nesting=">,
  Flags<[FrontendOption, HelpHidden, DoesNotAffectIncrementalBuild]>,
  HelpText<"Set the maximum concrete type nesting depth before giving up">;

def requirement_machine_max_split_concrete_equiv_class_attempts : Joined<["-"], "requirement-machine-max-split-concrete-equiv-class-attempts=">,
  Flags<[FrontendOption, HelpHidden, DoesNotAffectIncrementalBuild]>,
  HelpText<"Set the maximum concrete number of attempts at splitting "
           "concrete equivalence classes before giving up. There should "
           "never be a reason to change this">;

def disable_requirement_machine_concrete_contraction : Flag<["-"], "disable-requirement-machine-concrete-contraction">,
  HelpText<"Disable preprocessing pass to eliminate conformance requirements "
           "on generic parameters which are made concrete">;

def disable_requirement_machine_loop_normalization : Flag<["-"], "disable-requirement-machine-loop-normalization">,
  HelpText<"Disable stronger minimization algorithm, for debugging only">;

def disable_requirement_machine_reuse : Flag<["-"], "disable-requirement-machine-reuse">,
  HelpText<"Disable re-use of requirement machines for minimization, for debugging only">;

def enable_requirement_machine_opaque_archetypes : Flag<["-"], "enable-requirement-machine-opaque-archetypes">,
  HelpText<"Enable more correct opaque archetype support, which is off by default because it might fail to produce a convergent rewrite system">;

def warn_redundant_requirements : Flag<["-"], "warn-redundant-requirements">,
  HelpText<"Emit warnings for redundant requirements in generic signatures">;

def dump_type_witness_systems : Flag<["-"], "dump-type-witness-systems">,
  HelpText<"Enables dumping type witness systems from associated type inference">;

def enable_round_trip_debug_types : Flag<["-"], "enable-round-trip-debug-types">,
  HelpText<"Enables verification of debug info mangling">;

def disable_round_trip_debug_types : Flag<["-"], "disable-round-trip-debug-types">,
  HelpText<"Disables verification of debug info mangling">;

def debug_generic_signatures : Flag<["-"], "debug-generic-signatures">,
  HelpText<"Debug generic signatures">;

def debug_forbid_typecheck_prefix : Separate<["-"], "debug-forbid-typecheck-prefix">,
  HelpText<"Triggers llvm fatal_error if typechecker tries to typecheck a decl "
           "with the provided prefix name">;

def debug_emit_invalid_swiftinterface_syntax : Flag<["-"], "debug-emit-invalid-swiftinterface-syntax">,
  HelpText<"Write an invalid declaration into swiftinterface files">;

def debug_cycles : Flag<["-"], "debug-cycles">,
  HelpText<"Print out debug dumps when cycles are detected in evaluation">;

def debug_time_function_bodies : Flag<["-"], "debug-time-function-bodies">,
  HelpText<"Dumps the time it takes to type-check each function body">;
def debug_time_expression_type_checking : Flag<["-"], "debug-time-expression-type-checking">,
  HelpText<"Dumps the time it takes to type-check each expression">;

def debug_assert_immediately : Flag<["-"], "debug-assert-immediately">,
  DebugCrashOpt, HelpText<"Force an assertion failure immediately">;
def debug_assert_after_parse : Flag<["-"], "debug-assert-after-parse">,
  DebugCrashOpt, HelpText<"Force an assertion failure after parsing">;
def debug_crash_immediately : Flag<["-"], "debug-crash-immediately">,
  DebugCrashOpt, HelpText<"Force a crash immediately">;
def debug_crash_after_parse : Flag<["-"], "debug-crash-after-parse">,
  DebugCrashOpt, HelpText<"Force a crash after parsing">;

def debug_test_dependency_scan_cache_serialization: Flag<["-"], "test-dependency-scan-cache-serialization">,
   HelpText<"After performing a dependency scan, serialize and then deserialize the scanner's internal state.">;

def debugger_support : Flag<["-"], "debugger-support">,
  HelpText<"Process swift code as if running in the debugger">;

def disable_clangimporter_source_import : Flag<["-"],
  "disable-clangimporter-source-import">,
  HelpText<"Disable ClangImporter and forward all requests straight the DWARF importer.">;

def disable_implicit_concurrency_module_import : Flag<["-"],
  "disable-implicit-concurrency-module-import">,
  HelpText<"Disable the implicit import of the _Concurrency module.">;

def disable_implicit_string_processing_module_import : Flag<["-"],
  "disable-implicit-string-processing-module-import">,
  HelpText<"Disable the implicit import of the _StringProcessing module.">;

def enable_implicit_backtracing_module_import : Flag<["-"],
  "enable-implicit-backtracing-module-import">,
  HelpText<"Enable the implicit import of the _Backtracing module.">;

def disable_implicit_backtracing_module_import : Flag<["-"],
  "disable-implicit-backtracing-module-import">,
  HelpText<"Disable the implicit import of the _Backtracing module.">;

def disable_arc_opts : Flag<["-"], "disable-arc-opts">,
  HelpText<"Don't run SIL ARC optimization passes.">;
def disable_ossa_opts : Flag<["-"], "disable-ossa-opts">,
  HelpText<"Don't run SIL OSSA optimization passes.">;

def disable_sil_partial_apply : Flag<["-"], "disable-sil-partial-apply">,
  HelpText<"Disable use of partial_apply in SIL generation">;

def enable_spec_devirt : Flag<["-"], "enable-spec-devirt">,
  HelpText<"Enable speculative devirtualization pass.">;

def disable_access_control : Flag<["-"], "disable-access-control">,
  HelpText<"Don't respect access control restrictions">;
def enable_access_control : Flag<["-"], "enable-access-control">,
  HelpText<"Respect access control restrictions">;

def code_complete_inits_in_postfix_expr : Flag<["-"], "code-complete-inits-in-postfix-expr">,
  HelpText<"Include initializers when completing a postfix expression">;
def code_complete_call_pattern_heuristics : Flag<["-"], "code-complete-call-pattern-heuristics">,
  HelpText<"Use heuristics to guess whether we want call pattern completions">;

def disable_autolink_framework : Separate<["-"],"disable-autolink-framework">,
  HelpText<"Disable autolinking against the provided framework">;

def disable_diagnostic_passes : Flag<["-"], "disable-diagnostic-passes">,
  HelpText<"Don't run diagnostic passes">;

def disable_llvm_optzns : Flag<["-"], "disable-llvm-optzns">,
  HelpText<"Don't run LLVM optimization passes">;

def disable_sil_perf_optzns : Flag<["-"], "disable-sil-perf-optzns">,
  HelpText<"Don't run SIL performance optimization passes">;

def disable_swift_specific_llvm_optzns : Flag<["-"], "disable-swift-specific-llvm-optzns">,
  HelpText<"Don't run Swift specific LLVM optimization passes.">;

def disable_llvm_verify : Flag<["-"], "disable-llvm-verify">,
  HelpText<"Don't run the LLVM IR verifier.">;

def disable_llvm_value_names : Flag<["-"], "disable-llvm-value-names">,
  HelpText<"Don't add names to local values in LLVM IR">;

def dump_jit : JoinedOrSeparate<["-"], "dump-jit">,
  HelpText<"Dump JIT contents">;

def enable_llvm_value_names : Flag<["-"], "enable-llvm-value-names">,
  HelpText<"Add names to local values in LLVM IR">;

def enable_anonymous_context_mangled_names :
  Flag<["-"], "enable-anonymous-context-mangled-names">,
  HelpText<"Enable emission of mangled names in anonymous context descriptors">;

def disable_reflection_metadata : Flag<["-"], "disable-reflection-metadata">,
  HelpText<"Disable emission of reflection metadata for nominal types">;

def reflection_metadata_for_debugger_only : Flag<["-"], "reflection-metadata-for-debugger-only">,
  HelpText<"Emit reflection metadata for debugger only, don't make them available at runtime">;

def disable_reflection_names : Flag<["-"], "disable-reflection-names">,
  HelpText<"Disable emission of names of stored properties and enum cases in"
           "reflection metadata">;

def function_sections: Flag<["-"], "function-sections">,
  Flags<[FrontendOption, NoInteractiveOption]>,
  HelpText<"Emit functions to separate sections.">;

def enable_single_module_llvm_emission: Flag<["-"], "enable-single-module-llvm-emission">,
  Flags<[FrontendOption, NoInteractiveOption]>,
  HelpText<"Emit LLVM IR into a single LLVM module in multithreaded mode.">;

def stack_promotion_checks : Flag<["-"], "emit-stack-promotion-checks">,
  HelpText<"Emit runtime checks for correct stack promotion of objects.">;

def stack_promotion_limit : Separate<["-"], "stack-promotion-limit">,
  HelpText<"Limit the size of stack promoted objects to the provided number "
           "of bytes.">;

def dump_clang_diagnostics : Flag<["-"], "dump-clang-diagnostics">,
  HelpText<"Dump Clang diagnostics to stderr">;

def disable_modules_validate_system_headers : Flag<["-"], "disable-modules-validate-system-headers">,
  HelpText<"Disable validating system headers in the Clang importer">;

def emit_verbose_sil : Flag<["-"], "emit-verbose-sil">,
  HelpText<"Emit locations during SIL emission">;

def emit_pch : Flag<["-"], "emit-pch">,
  HelpText<"Emit PCH for imported Objective-C header file">, ModeOpt;

def pch_disable_validation : Flag<["-"], "pch-disable-validation">,
  HelpText<"Disable validating the persistent PCH">;

def disable_sil_ownership_verifier : Flag<["-"], "disable-sil-ownership-verifier">,
  HelpText<"Do not verify ownership invariants during SIL Verification ">;

def suppress_static_exclusivity_swap : Flag<["-"], "suppress-static-exclusivity-swap">,
  HelpText<"Suppress static violations of exclusive access with swap()">;

def enable_experimental_static_assert :
  Flag<["-"], "enable-experimental-static-assert">,
  HelpText<"Enable experimental #assert">;

def disable_subst_sil_function_types :
  Flag<["-"], "disable-subst-sil-function-types">,
  HelpText<"Disable substituted function types for SIL type lowering of function values">;

def enable_experimental_named_opaque_types :
  Flag<["-"], "enable-experimental-named-opaque-types">,
  HelpText<"Enable experimental support for named opaque result types">;

def enable_explicit_existential_types :
  Flag<["-"], "enable-explicit-existential-types">,
  HelpText<"Enable experimental support for explicit existential types">;

def enable_experimental_opened_existential_types :
  Flag<["-"], "enable-experimental-opened-existential-types">,
  HelpText<"Enable experimental support for implicitly opened existentials">;

def disable_experimental_opened_existential_types :
  Flag<["-"], "disable-experimental-opened-existential-types">,
  HelpText<"Disable experimental support for implicitly opened existentials">;

def enable_deserialization_recovery :
  Flag<["-"], "enable-deserialization-recovery">,
  HelpText<"Attempt to recover from missing xrefs (etc) in swiftmodules">;
def disable_deserialization_recovery :
  Flag<["-"], "disable-deserialization-recovery">,
  HelpText<"Don't attempt to recover from missing xrefs (etc) in swiftmodules">;

def enable_deserialization_safety :
  Flag<["-"], "enable-deserialization-safety">,
  HelpText<"Avoid reading potentially unsafe decls in swiftmodules">;
def disable_deserialization_safety :
  Flag<["-"], "disable-deserialization-safety">,
  HelpText<"Don't avoid reading potentially unsafe decls in swiftmodules">;

def force_workaround_broken_modules :
  Flag<["-"], "experimental-force-workaround-broken-modules">,
  HelpText<"Attempt unsafe recovery for imported modules with broken modularization">;

def enable_experimental_string_processing :
  Flag<["-"], "enable-experimental-string-processing">,
  HelpText<"Enable experimental string processing">;
def disable_experimental_string_processing :
  Flag<["-"], "disable-experimental-string-processing">,
  HelpText<"Disable experimental string processing">;

def enable_experimental_associated_type_inference :
  Flag<["-"], "enable-experimental-associated-type-inference">,
  HelpText<"Enable experimental associated type inference via type witness systems">;

def disable_availability_checking : Flag<["-"],
  "disable-availability-checking">,
  HelpText<"Disable checking for potentially unavailable APIs">;

def enable_conformance_availability_errors : Flag<["-"],
  "enable-conformance-availability-errors">,
  HelpText<"Diagnose conformance availability violations as errors">;

def disable_conformance_availability_errors : Flag<["-"],
  "disable-conformance-availability-errors">,
  HelpText<"Diagnose conformance availability violations as warnings">;

def warn_on_potentially_unavailable_enum_case : Flag<["-"],
  "warn-on-potentially-unavailable-enum-case">,
  HelpText<"Downgrade potential unavailability of enum case to a warning">;

def warn_on_editor_placeholder : Flag<["-"],
  "warn-on-editor-placeholder">,
  HelpText<"Downgrade the editor placeholder error to a warning">;

def report_errors_to_debugger : Flag<["-"], "report-errors-to-debugger">,
  HelpText<"Deprecated, will be removed in future versions.">;

def enable_swift3_objc_inference : Flag<["-"], "enable-swift3-objc-inference">,
  Flags<[FrontendOption, HelpHidden]>,
HelpText<"Enable Swift 3's @objc inference rules for NSObject-derived classes and 'dynamic' members (emulates Swift 3 behavior)">;

def disable_swift3_objc_inference :
  Flag<["-"], "disable-swift3-objc-inference">,
  Flags<[FrontendOption, HelpHidden]>,
  HelpText<"Disable Swift 3's @objc inference rules for NSObject-derived classes and 'dynamic' members (emulates Swift 4 behavior)">;

def enable_implicit_dynamic : Flag<["-"], "enable-implicit-dynamic">,
  Flags<[FrontendOption, NoInteractiveOption, HelpHidden]>,
  HelpText<"Add 'dynamic' to all declarations">;
  
def enable_experimental_opaque_type_erasure : Flag<["-"], "enable-experimental-opaque-type-erasure">,
  Flags<[FrontendOption, NoInteractiveOption, HelpHidden]>,
  HelpText<"Type-erases opaque types that conform to @_typeEraser protocols">;

def enable_llvm_vfe : Flag<["-"], "enable-llvm-vfe">,
  Flags<[FrontendOption, NoInteractiveOption, HelpHidden]>,
  HelpText<"Use LLVM IR Virtual Function Elimination on Swift class virtual tables">;

def enable_llvm_wme : Flag<["-"], "enable-llvm-wme">,
  Flags<[FrontendOption, NoInteractiveOption, HelpHidden]>,
  HelpText<"Use LLVM IR Witness Method Elimination on Swift protocol witness tables">;

def internalize_at_link : Flag<["-"], "internalize-at-link">,
  Flags<[FrontendOption, NoInteractiveOption, HelpHidden]>,
  HelpText<"Allow internalizing public symbols and vtables at link time (assume"
  " all client code of public types is part of the same link unit, or that"
  " external symbols are explicitly requested via -exported_symbols_list)">;

def conditional_runtime_records : Flag<["-"], "conditional-runtime-records">,
  Flags<[FrontendOption, NoInteractiveOption, HelpHidden]>,
  HelpText<"Allow removal of runtime metadata records (public types, protocol conformances) based on whether they're used or unused">;

def disable_preallocated_instantiation_caches : Flag<["-"], "disable-preallocated-instantiation-caches">,
  Flags<[FrontendOption, NoInteractiveOption, HelpHidden]>,
  HelpText<"Avoid preallocating metadata instantiation caches in globals">;

def disable_readonly_static_objects : Flag<["-"], "disable-readonly-static-objects">,
  Flags<[FrontendOption, NoInteractiveOption, HelpHidden]>,
  HelpText<"Avoid allocating static objects in a read-only data section">;

def trap_function
  : Separate<["-"], "trap-function">, MetaVarName<"<name>">,
    HelpText<"Lower traps to calls to this function instead of trap instructions">;

def disable_previous_implementation_calls_in_dynamic_replacements :
  Flag<["-"], "disable-previous-implementation-calls-in-dynamic-replacements">,
  Flags<[FrontendOption, NoInteractiveOption, HelpHidden]>,
  HelpText<"Disable calling the previous implementation in dynamic replacements">;

def enable_dynamic_replacement_chaining :
  Flag<["-"], "enable-dynamic-replacement-chaining">,
  Flags<[FrontendOption, NoInteractiveOption, HelpHidden]>,
  HelpText<"Enable chaining of dynamic replacements">;

def enable_nskeyedarchiver_diagnostics :
  Flag<["-"], "enable-nskeyedarchiver-diagnostics">,
  HelpText<"Diagnose classes with unstable mangled names adopting NSCoding">;
def disable_nskeyedarchiver_diagnostics :
  Flag<["-"], "disable-nskeyedarchiver-diagnostics">,
  HelpText<"Allow classes with unstable mangled names to adopt NSCoding">;

def enable_nonfrozen_enum_exhaustivity_diagnostics :
  Flag<["-"], "enable-nonfrozen-enum-exhaustivity-diagnostics">,
  HelpText<"Diagnose switches over non-frozen enums without catch-all cases">;
def disable_nonfrozen_enum_exhaustivity_diagnostics :
  Flag<["-"], "disable-nonfrozen-enum-exhaustivity-diagnostics">,
  HelpText<"Allow switches over non-frozen enums without catch-all cases">;

def warn_long_function_bodies : Separate<["-"], "warn-long-function-bodies">,
  MetaVarName<"<n>">,
  HelpText<"Warns when type-checking a function takes longer than <n> ms">;
def warn_long_function_bodies_EQ : Joined<["-"], "warn-long-function-bodies=">,
  Alias<warn_long_function_bodies>;

def warn_long_expression_type_checking : Separate<["-"], "warn-long-expression-type-checking">,
  MetaVarName<"<n>">,
  HelpText<"Warns when type-checking a function takes longer than <n> ms">;
def warn_long_expression_type_checking_EQ : Joined<["-"], "warn-long-expression-type-checking=">,
  Alias<warn_long_expression_type_checking>;

def Rmodule_interface_rebuild : Flag<["-"], "Rmodule-interface-rebuild">,
  HelpText<"Emits a remark if an imported module needs to be re-compiled from its module interface">;

def downgrade_typecheck_interface_error : Flag<["-"], "downgrade-typecheck-interface-error">,
  HelpText<"Downgrade error to warning when typechecking emitted module interfaces">;

def enable_volatile_modules : Flag<["-"], "enable-volatile-modules">,
  HelpText<"Load Swift modules in memory">;

def solver_expression_time_threshold_EQ : Joined<["-"], "solver-expression-time-threshold=">;

def solver_disable_shrink :
  Flag<["-"], "solver-disable-shrink">,
  HelpText<"Disable the shrink phase of expression type checking">;

def disable_constraint_solver_performance_hacks : Flag<["-"], "disable-constraint-solver-performance-hacks">,
  HelpText<"Disable all the hacks in the constraint solver">;

def enable_operator_designated_types :
  Flag<["-"], "enable-operator-designated-types">,
  HelpText<"Enable operator designated types">;

def enable_invalid_ephemeralness_as_error :
  Flag<["-"], "enable-invalid-ephemeralness-as-error">,
  HelpText<"Diagnose invalid ephemeral to non-ephemeral conversions as errors">;

def disable_invalid_ephemeralness_as_error :
  Flag<["-"], "disable-invalid-ephemeralness-as-error">,
  HelpText<"Diagnose invalid ephemeral to non-ephemeral conversions as warnings">;

def switch_checking_invocation_threshold_EQ : Joined<["-"],
    "switch-checking-invocation-threshold=">;

def enable_new_operator_lookup :
  Flag<["-"], "enable-new-operator-lookup">,
  HelpText<"Enable the new operator decl and precedencegroup lookup behavior">;

def disable_new_operator_lookup :
  Flag<["-"], "disable-new-operator-lookup">,
  HelpText<"Disable the new operator decl and precedencegroup lookup behavior">;

def enable_source_import : Flag<["-"], "enable-source-import">,
  HelpText<"Enable importing of Swift source files">;

def enable_throw_without_try : Flag<["-"], "enable-throw-without-try">,
  HelpText<"Allow throwing function calls without 'try'">;

def import_module : Separate<["-"], "import-module">,
  HelpText<"Implicitly import the specified module">;

def testable_import_module : Separate<["-"], "testable-import-module">,
  HelpText<"Implicitly import the specified module with @testable">;

def print_stats : Flag<["-"], "print-stats">,
  HelpText<"Print various statistics">;

def check_onone_completeness : Flag<["-"], "check-onone-completeness">,
  HelpText<"Print errors if the compile OnoneSupport module is missing symbols">;

def debugger_testing_transform : Flag<["-"], "debugger-testing-transform">,
  HelpText<"Instrument the code with calls to an intrinsic that record the expected values of "
           "local variables so they can be compared against the results from the debugger.">;

def disable_debugger_shadow_copies : Flag<["-"], "disable-debugger-shadow-copies">,
  HelpText<"Disable debugger shadow copies of local variables."
           "This option is only useful for testing the compiler.">,
  Flags<[FrontendOption, HelpHidden]>;

def disable_concrete_type_metadata_mangled_name_accessors : Flag<["-"], "disable-concrete-type-metadata-mangled-name-accessors">,
  HelpText<"Disable concrete type metadata access by mangled name">,
  Flags<[FrontendOption, HelpHidden]>;

def disable_standard_substitutions_in_reflection_mangling : Flag<["-"], "disable-standard-substitutions-in-reflection-mangling">,
  HelpText<"Disable referencing stdlib symbols via mangled names in reflection mangling">,
  Flags<[FrontendOption, HelpHidden]>;
  
def playground : Flag<["-"], "playground">,
  HelpText<"Apply the playground semantics and transformation">;

def playground_high_performance : Flag<["-"], "playground-high-performance">,
  HelpText<"Omit instrumentation that has a high runtime performance impact">;

def disable_playground_transform : Flag<["-"], "disable-playground-transform">,
  HelpText<"Disable playground transformation">;

def pc_macro : Flag<["-"], "pc-macro">,
  HelpText<"Apply the 'program counter simulation' macro">;

def no_clang_module_breadcrumbs : Flag<["-"], "no-clang-module-breadcrumbs">,
  HelpText<"Don't emit DWARF skeleton CUs for imported Clang modules. "
           "Use this when building a redistributable static archive.">;

def use_jit : Flag<["-"], "use-jit">,
  HelpText<"Register Objective-C classes as if the JIT were in use">;

def sil_inline_threshold : Separate<["-"], "sil-inline-threshold">,
  MetaVarName<"<50>">,
  HelpText<"Controls the aggressiveness of performance inlining">;

def sil_inline_caller_benefit_reduction_factor : Separate<["-"], "sil-inline-caller-benefit-reduction-factor">,
  MetaVarName<"<2>">,
  HelpText<"Controls the aggressiveness of performance inlining in -Osize "
          "mode by reducing the base benefits of a caller (lower value "
          "permits more inlining!)">;

def sil_unroll_threshold : Separate<["-"], "sil-unroll-threshold">,
  MetaVarName<"<250>">,
  HelpText<"Controls the aggressiveness of loop unrolling">;

def sil_verify_all : Flag<["-"], "sil-verify-all">,
  HelpText<"Verify SIL after each transform">;
  
def sil_verify_none : Flag<["-"], "sil-verify-none">,
  HelpText<"Completely disable SIL verification">;
  
def verify_all_substitution_maps : Flag<["-"], "verify-all-substitution-maps">,
  HelpText<"Verify all SubstitutionMaps on construction">;

def sil_debug_serialization : Flag<["-"], "sil-debug-serialization">,
  HelpText<"Do not eliminate functions in Mandatory Inlining/SILCombine dead "
           "functions. (for debugging only)">;

def sil_stop_optzns_before_lowering_ownership :
  Flag<["-"], "sil-stop-optzns-before-lowering-ownership">,
  HelpText<"Stop optimizing at SIL time before we lower ownership from SIL. "
           "Intended only for SIL ossa tests">;

def print_inst_counts : Flag<["-"], "print-inst-counts">,
  HelpText<"Before IRGen, count all the various SIL instructions. Must be used "
           "in conjunction with -print-stats.">;

def debug_on_sil : Flag<["-"], "sil-based-debuginfo">,
  HelpText<"Write the SIL into a file and generate debug-info to debug on SIL "
           " level.">;
def legacy_gsil : Flag<["-"], "gsil">,
  HelpText<"Deprecated, use '-sil-based-debuginfo' instead">;

def print_llvm_inline_tree : Flag<["-"], "print-llvm-inline-tree">,
  HelpText<"Print the LLVM inline tree.">;

def disable_incremental_llvm_codegeneration :
  Flag<["-"], "disable-incremental-llvm-codegen">,
       HelpText<"Disable incremental llvm code generation.">;

def ignore_always_inline : Flag<["-"], "ignore-always-inline">,
  HelpText<"Ignore @inline(__always) attributes.">;

def emit_sorted_sil : Flag<["-"], "emit-sorted-sil">,
  HelpText<"When printing SIL, print out all sil entities sorted by name to "
           "ease diffing">;

def cxx_interop_getters_setters_as_properties :
  Flag<["-"], "cxx-interop-getters-setters-as-properties">,
  HelpText<"Import getters and setters as computed properties in Swift">,
  Flags<[FrontendOption, HelpHidden]>;

def cxx_interop_disable_requirement_at_import :
  Flag<["-"], "disable-cxx-interop-requirement-at-import">,
  HelpText<"Do not require C++ interoperability to be enabled when importing a Swift module that enables C++ interoperability">,
  Flags<[FrontendOption, HelpHidden]>;

def use_malloc : Flag<["-"], "use-malloc">,
  HelpText<"Allocate internal data structures using malloc "
           "(for memory debugging)">;

def interpret : Flag<["-"], "interpret">, HelpText<"Immediate mode">, ModeOpt;

def verify_type_layout : JoinedOrSeparate<["-"], "verify-type-layout">,
  HelpText<"Verify compile-time and runtime type layout information for type">,
  MetaVarName<"<type>">;

def external_pass_pipeline_filename : Separate<["-"], "external-pass-pipeline-filename">,
    HelpText<"Use the pass pipeline defined by <pass_pipeline_file>">,
    MetaVarName<"<pass_pipeline_file>">;

def index_system_modules : Flag<["-"], "index-system-modules">,
  HelpText<"Emit index data for imported serialized swift system modules">;

def index_ignore_stdlib :
  Flag<["-"], "index-ignore-stdlib">,
  HelpText<"Avoid emitting index data for the standard library.">;

def index_unit_output_path_filelist : Separate<["-"], "index-unit-output-path-filelist">,
  HelpText<"Specify index unit output paths in a file rather than on the command line">;

def dump_interface_hash : Flag<["-"], "dump-interface-hash">,
   HelpText<"Parse input file(s) and dump interface token hash(es)">,
   ModeOpt;

def compile_module_from_interface :
  Flag<["-"], "compile-module-from-interface">,
  HelpText<"Treat the (single) input as a swiftinterface and produce a module">,
  ModeOpt;

def explicit_interface_module_build :
  Flag<["-"], "explicit-interface-module-build">,
  HelpText<"Use the specified command-line to build the module from interface, instead of flags specified in the interface">;

def direct_clang_cc1_module_build :
  Flag<["-"], "direct-clang-cc1-module-build">,
  HelpText<"Use the specified -Xcc options to build a PCM by using Clang frontend directly, bypassing the Clang driver">;

def build_module_from_parseable_interface :
  Flag<["-"], "build-module-from-parseable-interface">,
  Alias<compile_module_from_interface>,
  ModeOpt;

def typecheck_module_from_interface :
  Flag<["-"], "typecheck-module-from-interface">,
  HelpText<"Treat the (single) input as a swiftinterface and typecheck it">,
  ModeOpt;

def module_interface_preserve_types_as_written :
  Flag<["-"], "module-interface-preserve-types-as-written">,
  HelpText<"When emitting a module interface, preserve types as they were "
           "written in the source">;

def alias_module_names_in_module_interface :
  Flag<["-"], "alias-module-names-in-module-interface">,
  HelpText<"When emitting a module interface, disambiguate modules using "
           "distinct alias names">;
def disable_alias_module_names_in_module_interface :
  Flag<["-"], "disable-alias-module-names-in-module-interface">,
  HelpText<"When emitting a module interface, disable disambiguating modules "
           "using distinct alias names">;

def experimental_spi_imports :
  Flag<["-"], "experimental-spi-imports">,
  HelpText<"Enable experimental support for SPI imports">;

def disable_print_missing_imports_in_module_interface :
  Flag<["-"], "disable-print-missing-imports-in-module-interface">,
  HelpText<"Disable adding to the module interface imports used from API and "
           "missing from the sources">;

// [FIXME: Clang-type-plumbing] Make this a SIL-only option once we start
// unconditionally emitting non-canonical Clang types in swiftinterfaces.
def experimental_print_full_convention :
 Flag<["-"], "experimental-print-full-convention">,
 HelpText<"When emitting a module interface or SIL, emit additional @convention"
          " arguments, regardless of whether they were written in the source."
          " Also requires -use-clang-function-types to be enabled.">;

def experimental_one_way_closure_params :
  Flag<["-"], "experimental-one-way-closure-params">,
  HelpText<"Enable experimental support for one-way closure parameters">;

def prebuilt_module_cache_path :
  Separate<["-"], "prebuilt-module-cache-path">,
  HelpText<"Directory of prebuilt modules for loading module interfaces">;
def prebuilt_module_cache_path_EQ :
  Joined<["-"], "prebuilt-module-cache-path=">,
  Alias<prebuilt_module_cache_path>;

def backup_module_interface_path :
  Separate<["-"], "backup-module-interface-path">,
  HelpText<"Directory of module interfaces as backups to those from SDKs">;
def backup_module_interface_path_EQ :
  Joined<["-"], "backup-module-interface-path=">,
  Alias<backup_module_interface_path>;

def force_public_linkage : Flag<["-"], "force-public-linkage">,
   HelpText<"Force public linkage for private symbols. Used by LLDB.">;

def dump_api_path : Separate<["-"], "dump-api-path">,
  HelpText<"The path to output swift interface files for the compiled source files">;

def group_info_path : Separate<["-"], "group-info-path">,
  HelpText<"The path to collect the group information of the compiled module">;

def diagnostics_editor_mode : Flag<["-"], "diagnostics-editor-mode">,
HelpText<"Diagnostics will be used in editor">;

def validate_tbd_against_ir_EQ: Joined<["-"], "validate-tbd-against-ir=">,
    HelpText<"Compare the symbols in the IR against the TBD file that would be generated.">,
    MetaVarName<"<level>">;

def bypass_batch_mode_checks: Flag<["-"], "bypass-batch-mode-checks">,
    HelpText<"Bypass checks for batch-mode errors.">;

def enable_verify_exclusivity : Flag<["-"], "enable-verify-exclusivity">,
  HelpText<"Enable verification of access markers used to enforce exclusivity.">;

def disable_verify_exclusivity : Flag<["-"], "disable-verify-exclusivity">,
  HelpText<"Disable verification of access markers used to enforce exclusivity.">;

def disable_legacy_type_info : Flag<["-"], "disable-legacy-type-info">,
  HelpText<"Completely disable legacy type layout">;

def disable_generic_metadata_prespecialization : Flag<["-"], "disable-generic-metadata-prespecialization">,
  HelpText<"Do not statically specialize metadata for generic types at types "
           "that are known to be used in source.">;

def prespecialize_generic_metadata : Flag<["-"], "prespecialize-generic-metadata">,
   HelpText<"Statically specialize metadata for generic types at types that "
            "are known to be used in source.">;

def read_legacy_type_info_path_EQ : Joined<["-"], "read-legacy-type-info-path=">,
  HelpText<"Read legacy type layout from the given path instead of default path">;

def type_info_dump_filter_EQ : Joined<["-"], "type-info-dump-filter=">,
  Flags<[FrontendOption]>,
  HelpText<"One of 'all', 'resilient' or 'fragile'">;

def swift_async_frame_pointer_EQ : Joined<["-"], "swift-async-frame-pointer=">,
  Flags<[FrontendOption]>,
  HelpText<"One of 'auto', 'always' or 'never'">;

def previous_module_installname_map_file
  : Separate<["-"], "previous-module-installname-map-file">, MetaVarName<"<path>">,
    HelpText<"Path to a Json file indicating module name to installname map for @_originallyDefinedIn">;

def enable_type_layouts : Flag<["-"], "enable-type-layout">,
  HelpText<"Enable type layout based lowering">;

def disable_type_layouts : Flag<["-"], "disable-type-layout">,
  HelpText<"Disable type layout based lowering">;

def force_struct_type_layouts : Flag<["-"], "force-struct-type-layouts">,
  HelpText<"Force type layout based lowering for structs">;

def enable_layout_string_value_witnesses : Flag<["-"], "enable-layout-string-value-witnesses">,
  HelpText<"Enable layout string based value witnesses">;

let Flags = [FrontendOption, NoDriverOption, HelpHidden, ModuleInterfaceOptionIgnorable] in {
  def enable_pack_metadata_stack_promotion :
    Joined<["-"], "enable-pack-metadata-stack-promotion=">,
    HelpText<"Whether to skip heapifying stack metadata packs when possible.">,
    MetaVarName<"true|false">;
  def enable_pack_metadata_stack_promotion_noArg :
    Flag<["-"], "enable-pack-metadata-stack-promotion">,
    HelpText<"Skip heapifying stack metadata packs when possible.">;
}

def disable_layout_string_value_witnesses : Flag<["-"], "disable-layout-string-value-witnesses">,
  HelpText<"Disable layout string based value witnesses">;

def enable_layout_string_value_witnesses_instantiation : Flag<["-"], "enable-layout-string-value-witnesses-instantiation">,
  HelpText<"Enable runtime instantiation of layout string value witnesses for generic types">;

def disable_layout_string_value_witnesses_instantiation : Flag<["-"], "disable-layout-string-value-witnesses-instantiation">,
  HelpText<"Disable runtime instantiation of layout string value witnesses for generic types">;

def disable_interface_lockfile : Flag<["-"], "disable-interface-lock">,
  HelpText<"Don't lock interface file when building module">;

def bridging_header_directory_for_print: Separate<["-"], "bridging-header-directory-for-print">, MetaVarName<"<path>">,
    HelpText<"Directory for bridging header to be printed in compatibility header">;

def entry_point_function_name: Separate<["-"], "entry-point-function-name">, MetaVarName<"<string>">, 
    HelpText<"Name of the entry point function">;

def target_sdk_version : Separate<["-"], "target-sdk-version">,
  HelpText<"The version of target SDK used for compilation">;

def target_variant_sdk_version : Separate<["-"], "target-variant-sdk-version">,
  HelpText<"The version of target variant SDK used for compilation">;

def target_sdk_name : Separate<["-"], "target-sdk-name">,
  HelpText<"Canonical name of the target SDK used for compilation">;

def extra_clang_options_only : Flag<["-"], "only-use-extra-clang-opts">,
  HelpText<"Options passed via -Xcc are sufficient for Clang configuration">;

def candidate_module_file
  : Separate<["-"], "candidate-module-file">, MetaVarName<"<path>">,
    HelpText<"Specify Swift module may be ready to use for an interface">;

def use_static_resource_dir
  : Flag<["-"], "use-static-resource-dir">,
    HelpText<"Use resources in the static resource directory">;

def disable_building_interface
  : Flag<["-"], "disable-building-interface">,
    HelpText<"Disallow building binary module from textual interface">;

def experimental_skip_all_function_bodies:
  Flag<["-"], "experimental-skip-all-function-bodies">,
  HelpText<"Skip type-checking function bodies and all SIL generation">;

def experimental_allow_module_with_compiler_errors:
  Flag<["-"], "experimental-allow-module-with-compiler-errors">,
  Flags<[HelpHidden]>,
  HelpText<"Attempt to output .swiftmodule, regardless of compilation errors">;

def bad_file_descriptor_retry_count:
  Separate<["-"], "bad-file-descriptor-retry-count">,
  HelpText<"Number of retrying opening a file if previous open returns a bad "
           "file descriptor error.">;

def enable_ast_verifier : Flag<["-"], "enable-ast-verifier">,
  HelpText<"Run the AST verifier during compilation. NOTE: This lets the user "
           "override the default behavior on whether or not the ASTVerifier "
           "is run. The default behavior is to run the verifier when asserts "
           "are enabled and not run it when asserts are disabled. NOTE: "
           "Can not be used if disable-ast-verifier is used as well">;

def disable_ast_verifier : Flag<["-"], "disable-ast-verifier">,
  HelpText<"Do not run the AST verifier during compilation. NOTE: This lets "
           "the user override the default behavior on whether or not the "
           "ASTVerifier is run. The default behavior is to run the verifier "
           "when asserts are enabled and not run it when asserts are "
           "disabled. NOTE: Can not be used if enable-ast-verifier is used "
           "as well">;

def enable_ossa_modules : Flag<["-"], "enable-ossa-modules">,
  HelpText<"Always serialize SIL in ossa form. If this flag is not passed in, "
           "when optimizing ownership will be lowered before serializing SIL">;

def enable_sil_opaque_values : Flag<["-"], "enable-sil-opaque-values">,
  HelpText<"Enable SIL Opaque Values">;

def new_driver_path
  : Separate<["-"], "new-driver-path">, MetaVarName<"<path>">,
  HelpText<"Path of the new driver to be used">;

def skip_import_in_public_interface:
  Separate<["-"], "skip-import-in-public-interface">,
  HelpText<"Skip the import statement corresponding to a module name "
           "when printing the public interface.">;

def clang_header_expose_decls:
  Joined<["-"], "clang-header-expose-decls=">,
  HelpText<"Which declarations should be exposed in the generated clang header.">,
  Flags<[FrontendOption, HelpHidden]>,
  MetaVarName<"all-public|has-expose-attr">;

def clang_header_expose_module:
  Separate<["-"], "clang-header-expose-module">,
  HelpText<"Allow the compiler to assume that APIs from the specified module are exposed to C/C++/Objective-C in another generated header, so that APIs in the current module that depend on declarations from the specified module can be exposed in the generated header.">,
  Flags<[FrontendOption, HelpHidden]>,
  MetaVarName<"<imported-module-name>=<generated-header-name>">;

def weak_link_at_target :
  Flag<["-"], "weak-link-at-target">,
  HelpText<"Weakly link symbols for declarations that were introduced at the "
           "deployment target. Symbols introduced before the deployment "
           "target are still strongly linked.">;

def concurrency_model :
    Separate<["-"], "concurrency-model">,
    HelpText<"Which concurrency model is used.  Defaults to standard.">,
    MetaVarName<"standard|task-to-thread">;
def concurrency_model_EQ :
    Joined<["-"], "concurrency-model=">,
    Alias<concurrency_model>;

def enable_stack_protector :
  Flag<["-"], "enable-stack-protector">,
  HelpText<"Enable the stack protector">;
def disable_stack_protector :
  Flag<["-"], "disable-stack-protector">,
  HelpText<"Disable the stack-protector">;
def enable_move_inout_stack_protector :
  Flag<["-"], "enable-move-inout-stack-protector">,
  HelpText<"Enable the stack protector by moving values to temporaries">;

def enable_import_ptrauth_field_function_pointers :
  Flag<["-"], "enable-import-ptrauth-field-function-pointers">,
  HelpText<"Enable import of custom ptrauth qualified field function pointers">;
def disable_import_ptrauth_field_function_pointers :
  Flag<["-"], "disable-import-ptrauth-field-function-pointers">,
  HelpText<"Disable import of custom ptrauth qualified field function pointers">;

def enable_collocate_metadata_functions :
  Flag<["-"], "enable-collocate-metadata-functions">,
  HelpText<"Enable collocate metadata functions">;
def disable_collocate_metadata_functions :
  Flag<["-"], "disable-collocate-metadata-functions">,
  HelpText<"Disable collocate metadata functions">;

def enable_colocate_type_descriptors :
  Flag<["-"], "enable-colocate-type-descriptors">,
  HelpText<"Enable colocate type descriptors">;
def disable_colocate_type_descriptors :
  Flag<["-"], "disable-colocate-type-descriptors">,
  HelpText<"Disable colocate type descriptors">;

def enable_relative_protocol_witness_tables :
  Flag<["-"], "enable-relative-protocol-witness-tables">,
  HelpText<"Enable relative protocol witness tables">;
def disable_relative_protocol_witness_tables :
  Flag<["-"], "disable-relative-protocol-witness-tables">,
  HelpText<"Disable relative protocol witness tables">;

def enable_new_llvm_pass_manager :
  Flag<["-"], "enable-new-llvm-pass-manager">,
  HelpText<"Enable the new llvm pass manager">;
def disable_new_llvm_pass_manager :
  Flag<["-"], "disable-new-llvm-pass-manager">,
  HelpText<"Disable the new llvm pass manager">;

def disable_emit_generic_class_ro_t_list :
  Flag<["-"], "disable-emit-generic-class-ro_t-list">,
  HelpText<"Disable emission of a section with references to class_ro_t of "
           "generic class patterns">;
def enable_emit_generic_class_ro_t_list :
  Flag<["-"], "enable-emit-generic-class-ro_t-list">,
  HelpText<"Enable emission of a section with references to class_ro_t of "
           "generic class patterns">;

def enable_deterministic_check :
  Flag<["-"], "enable-deterministic-check">,
  HelpText<"Check compiler output determinisim by running it twice">;
def always_compile_output_files :
  Flag<["-"], "always-compile-output-files">,
  HelpText<"Always compile output files even it might not change the results">;

def experimental_spi_only_imports :
  Flag<["-"], "experimental-spi-only-imports">,
  HelpText<"Enable use of @_spiOnly imports">;

def enable_ossa_complete_lifetimes :
  Flag<["-"], "enable-ossa-complete-lifetimes">,
  HelpText<"Require linear OSSA lifetimes after SILGen">;

def platform_c_calling_convention :
    Separate<["-"], "experimental-platform-c-calling-convention">,
    HelpText<"Which calling convention is used to perform non-swift calls. "
             "Defaults to llvm's standard C calling convention.">;
def platform_c_calling_convention_EQ :
    Joined<["-"], "experimental-platform-c-calling-convention=">,
    Alias<platform_c_calling_convention>;
} // end let Flags = [FrontendOption, NoDriverOption, HelpHidden]
