//===--- ToolChains.cpp - Job invocations (general and per-platform) ------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "ToolChains.h"

#include "swift/AST/DiagnosticsDriver.h"
#include "swift/Basic/Dwarf.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/Platform.h"
#include "swift/Basic/Range.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Basic/TaskQueue.h"
#include "swift/Config.h"
#include "swift/Driver/Compilation.h"
#include "swift/Driver/Driver.h"
#include "swift/Driver/Job.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Option/Options.h"
#include "clang/Basic/Version.h"
#include "clang/Driver/Util.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"

using namespace swift;
using namespace swift::driver;
using namespace llvm::opt;

bool ToolChain::JobContext::shouldUseInputFileList() const {
  return getTopLevelInputFiles().size() > C.getFilelistThreshold();
}

bool ToolChain::JobContext::shouldUsePrimaryInputFileListInFrontendInvocation()
    const {
  return InputActions.size() > C.getFilelistThreshold();
}

bool ToolChain::JobContext::shouldUseMainOutputFileListInFrontendInvocation()
    const {
  return Output.getPrimaryOutputFilenames().size() > C.getFilelistThreshold();
}

bool ToolChain::JobContext::
    shouldUseSupplementaryOutputFileMapInFrontendInvocation() const {
  static const unsigned UpperBoundOnSupplementaryOutputFileTypes =
      file_types::TY_INVALID;
  return InputActions.size() * UpperBoundOnSupplementaryOutputFileTypes >
         C.getFilelistThreshold();
}

bool ToolChain::JobContext::shouldFilterFrontendInputsByType() const {
  // FIXME: SingleCompile has not filtered its inputs in the past and now people
  // rely upon that. But we would like the compilation modes to be consistent.
  return OI.CompilerMode != OutputInfo::Mode::SingleCompile;
}

void ToolChain::addInputsOfType(ArgStringList &Arguments,
                                ArrayRef<const Action *> Inputs,
                                file_types::ID InputType,
                                const char *PrefixArgument) const {
  for (auto &Input : Inputs) {
    if (Input->getType() != InputType)
      continue;
    if (PrefixArgument)
      Arguments.push_back(PrefixArgument);
    Arguments.push_back(cast<InputAction>(Input)->getInputArg().getValue());
  }
}

void ToolChain::addInputsOfType(ArgStringList &Arguments,
                                ArrayRef<const Job *> Jobs,
                                const llvm::opt::ArgList &Args,
                                file_types::ID InputType,
                                const char *PrefixArgument) const {
  for (const Job *Cmd : Jobs) {
    auto output = Cmd->getOutput().getAnyOutputForType(InputType);
    if (!output.empty()) {
      if (PrefixArgument)
        Arguments.push_back(PrefixArgument);
      Arguments.push_back(Args.MakeArgString(output));
    }
  }
}

void ToolChain::addPrimaryInputsOfType(ArgStringList &Arguments,
                                       ArrayRef<const Job *> Jobs,
                                       const llvm::opt::ArgList &Args,
                                       file_types::ID InputType,
                                       const char *PrefixArgument) const {
  for (const Job *Cmd : Jobs) {
    auto &outputInfo = Cmd->getOutput();
    if (outputInfo.getPrimaryOutputType() == InputType) {
      for (auto Output : outputInfo.getPrimaryOutputFilenames()) {
        if (PrefixArgument)
          Arguments.push_back(PrefixArgument);
        Arguments.push_back(Args.MakeArgString(Output));
      }
    }
  }
}

static bool addOutputsOfType(ArgStringList &Arguments,
                             CommandOutput const &Output,
                             const llvm::opt::ArgList &Args,
                             file_types::ID OutputType,
                             const char *PrefixArgument = nullptr) {
  bool Added = false;
  for (auto Output : Output.getAdditionalOutputsForType(OutputType)) {
    assert(!Output.empty());
    if (PrefixArgument)
      Arguments.push_back(PrefixArgument);
    Arguments.push_back(Args.MakeArgString(Output));
    Added = true;
  }
  return Added;
}

static void addLTOArgs(const OutputInfo &OI, ArgStringList &arguments) {
  switch (OI.LTOVariant) {
  case OutputInfo::LTOKind::None:
    break;
  case OutputInfo::LTOKind::LLVMThin:
    arguments.push_back("-lto=llvm-thin");
    break;
  case OutputInfo::LTOKind::LLVMFull:
    arguments.push_back("-lto=llvm-full");
    break;
  }
}


void ToolChain::addCommonFrontendArgs(const OutputInfo &OI,
                                      const CommandOutput &output,
                                      const ArgList &inputArgs,
                                      ArgStringList &arguments) const {
  // Only pass -target to the REPL or immediate modes if it was explicitly
  // specified on the command line.
  switch (OI.CompilerMode) {
  case OutputInfo::Mode::REPL:
  case OutputInfo::Mode::Immediate:
    if (!inputArgs.hasArg(options::OPT_target))
      break;
    LLVM_FALLTHROUGH;
  case OutputInfo::Mode::StandardCompile:
  case OutputInfo::Mode::SingleCompile:
  case OutputInfo::Mode::BatchModeCompile:
    arguments.push_back("-target");
    arguments.push_back(inputArgs.MakeArgString(Triple.str()));
    break;
  }

  if (const Arg *variant = inputArgs.getLastArg(options::OPT_target_variant)) {
    arguments.push_back("-target-variant");
    std::string normalized = llvm::Triple::normalize(variant->getValue());
    arguments.push_back(inputArgs.MakeArgString(normalized));
  }

  // Enable address top-byte ignored in the ARM64 backend.
  if (Triple.getArch() == llvm::Triple::aarch64 ||
      Triple.getArch() == llvm::Triple::aarch64_32) {
    arguments.push_back("-Xllvm");
    arguments.push_back("-aarch64-use-tbi");
  }

  if (output.getPrimaryOutputType() == file_types::TY_SwiftModuleFile) {
    arguments.push_back("-warn-on-potentially-unavailable-enum-case");
  }

  // Enable or disable ObjC interop appropriately for the platform
  if (Triple.isOSDarwin()) {
    arguments.push_back("-enable-objc-interop");
  } else {
    arguments.push_back("-disable-objc-interop");
  }

  // Add flags for C++ interop.
  if (const Arg *arg =
          inputArgs.getLastArg(options::OPT_experimental_cxx_stdlib)) {
    arguments.push_back("-Xcc");
    arguments.push_back(
        inputArgs.MakeArgString(Twine("-stdlib=") + arg->getValue()));
  }

  if (inputArgs.hasArg(options::OPT_experimental_hermetic_seal_at_link)) {
    arguments.push_back("-enable-llvm-vfe");
    arguments.push_back("-enable-llvm-wme");
    arguments.push_back("-conditional-runtime-records");
    arguments.push_back("-internalize-at-link");
  }

  // Handle the CPU and its preferences.
  inputArgs.AddLastArg(arguments, options::OPT_target_cpu);

  if (!OI.SDKPath.empty()) {
    arguments.push_back("-sdk");
    arguments.push_back(inputArgs.MakeArgString(OI.SDKPath));
  }

  if (const Arg *A = inputArgs.getLastArg(options::OPT_windows_sdk_root)) {
    arguments.push_back("-windows-sdk-root");
    arguments.push_back(inputArgs.MakeArgString(A->getValue()));
  }
  if (const Arg *A = inputArgs.getLastArg(options::OPT_windows_sdk_version)) {
    arguments.push_back("-windows-sdk-version");
    arguments.push_back(inputArgs.MakeArgString(A->getValue()));
  }
  if (const Arg *A = inputArgs.getLastArg(options::OPT_visualc_tools_root)) {
    arguments.push_back("-visualc-tools-root");
    arguments.push_back(inputArgs.MakeArgString(A->getValue()));
  }
  if (const Arg *A = inputArgs.getLastArg(options::OPT_visualc_tools_version)) {
    arguments.push_back("-visualc-tools-version");
    arguments.push_back(inputArgs.MakeArgString(A->getValue()));
  }


  if (llvm::sys::Process::StandardErrHasColors()) {
    arguments.push_back("-color-diagnostics");
  }

  inputArgs.AddAllArgs(arguments, options::OPT_I);
  inputArgs.AddAllArgs(arguments, options::OPT_F, options::OPT_Fsystem);
  inputArgs.AddAllArgs(arguments, options::OPT_vfsoverlay);
  inputArgs.AddAllArgs(arguments, options::OPT_plugin_path);
  inputArgs.AddAllArgs(arguments, options::OPT_external_plugin_path);

  inputArgs.AddLastArg(arguments, options::OPT_AssertConfig);
  inputArgs.AddLastArg(arguments, options::OPT_autolink_force_load);
  inputArgs.AddLastArg(arguments,
                       options::OPT_color_diagnostics,
                       options::OPT_no_color_diagnostics);
  inputArgs.AddLastArg(arguments, options::OPT_fixit_all);
  inputArgs.AddLastArg(arguments,
                       options::OPT_warn_swift3_objc_inference_minimal,
                       options::OPT_warn_swift3_objc_inference_complete);
  inputArgs.AddLastArg(arguments,
                       options::OPT_enable_actor_data_race_checks,
                       options::OPT_disable_actor_data_race_checks);
  inputArgs.AddLastArg(arguments, options::OPT_warn_concurrency);
  inputArgs.AddLastArg(arguments, options::OPT_strict_concurrency);
  inputArgs.AddAllArgs(arguments, options::OPT_enable_experimental_feature);
  inputArgs.AddAllArgs(arguments, options::OPT_enable_upcoming_feature);
  inputArgs.AddLastArg(arguments, options::OPT_warn_implicit_overrides);
  inputArgs.AddLastArg(arguments, options::OPT_typo_correction_limit);
  inputArgs.AddLastArg(arguments, options::OPT_enable_app_extension);
  inputArgs.AddLastArg(arguments, options::OPT_enable_library_evolution);
  inputArgs.AddLastArg(arguments, options::OPT_require_explicit_availability);
  inputArgs.AddLastArg(arguments, options::OPT_require_explicit_availability_target);
  inputArgs.AddLastArg(arguments, options::OPT_require_explicit_availability_EQ);
  inputArgs.AddLastArg(arguments, options::OPT_require_explicit_sendable);
  inputArgs.AddLastArg(arguments, options::OPT_check_api_availability_only);
  inputArgs.AddLastArg(arguments, options::OPT_enable_testing);
  inputArgs.AddLastArg(arguments, options::OPT_enable_private_imports);
  inputArgs.AddLastArg(arguments, options::OPT_g_Group);
  inputArgs.AddLastArg(arguments, options::OPT_debug_info_format);
  inputArgs.AddLastArg(arguments, options::OPT_import_underlying_module);
  inputArgs.AddLastArg(arguments, options::OPT_module_cache_path);
  inputArgs.AddLastArg(arguments, options::OPT_module_link_name);
  inputArgs.AddLastArg(arguments, options::OPT_module_abi_name);
  inputArgs.AddLastArg(arguments, options::OPT_package_name);
  inputArgs.AddLastArg(arguments, options::OPT_export_as);
  inputArgs.AddLastArg(arguments, options::OPT_nostdimport);
  inputArgs.AddLastArg(arguments, options::OPT_parse_stdlib);
  inputArgs.AddLastArg(arguments, options::OPT_resource_dir);
  inputArgs.AddLastArg(arguments, options::OPT_solver_memory_threshold);
  inputArgs.AddLastArg(arguments, options::OPT_value_recursion_threshold);
  inputArgs.AddLastArg(arguments, options::OPT_warn_swift3_objc_inference);
  inputArgs.AddLastArg(arguments, options::OPT_Rpass_EQ);
  inputArgs.AddLastArg(arguments, options::OPT_Rpass_missed_EQ);
  inputArgs.AddLastArg(arguments, options::OPT_suppress_warnings);
  inputArgs.AddLastArg(arguments, options::OPT_suppress_remarks);
  inputArgs.AddLastArg(arguments, options::OPT_profile_generate);
  inputArgs.AddLastArg(arguments, options::OPT_profile_use);
  inputArgs.AddLastArg(arguments, options::OPT_profile_coverage_mapping);
  inputArgs.AddAllArgs(arguments, options::OPT_warnings_as_errors,
                       options::OPT_no_warnings_as_errors);
  inputArgs.AddLastArg(arguments, options::OPT_sanitize_EQ);
  inputArgs.AddLastArg(arguments, options::OPT_sanitize_recover_EQ);
  inputArgs.AddLastArg(arguments,
                       options::OPT_sanitize_address_use_odr_indicator);
  inputArgs.AddLastArg(arguments, options::OPT_sanitize_coverage_EQ);
  inputArgs.AddLastArg(arguments, options::OPT_static);
  inputArgs.AddLastArg(arguments, options::OPT_swift_version);
  inputArgs.AddLastArg(arguments, options::OPT_enforce_exclusivity_EQ);
  inputArgs.AddLastArg(arguments, options::OPT_stats_output_dir);
  inputArgs.AddLastArg(arguments, options::OPT_tools_directory);
  inputArgs.AddLastArg(arguments, options::OPT_trace_stats_events);
  inputArgs.AddLastArg(arguments, options::OPT_profile_stats_events);
  inputArgs.AddLastArg(arguments, options::OPT_profile_stats_entities);
  inputArgs.AddLastArg(arguments,
                       options::OPT_solver_shrink_unsolved_threshold);
  inputArgs.AddLastArg(arguments, options::OPT_O_Group);
  inputArgs.AddLastArg(arguments, options::OPT_RemoveRuntimeAsserts);
  inputArgs.AddLastArg(arguments, options::OPT_AssumeSingleThreaded);
  inputArgs.AddLastArg(arguments,
                       options::OPT_emit_fine_grained_dependency_sourcefile_dot_files);
  inputArgs.AddLastArg(arguments, options::OPT_package_description_version);
  inputArgs.AddLastArg(arguments, options::OPT_locale);
  inputArgs.AddLastArg(arguments, options::OPT_localization_path);
  inputArgs.AddLastArg(arguments, options::OPT_serialize_diagnostics_path);
  inputArgs.AddLastArg(arguments, options::OPT_debug_diagnostic_names);
  inputArgs.AddLastArg(arguments, options::OPT_print_educational_notes);
  inputArgs.AddLastArg(arguments, options::OPT_diagnostic_style);
  inputArgs.AddLastArg(arguments,
                       options::OPT_enable_experimental_concise_pound_file);
  inputArgs.AddLastArg(arguments,
                       options::OPT_verify_incremental_dependencies);
  inputArgs.AddLastArg(arguments, options::OPT_access_notes_path);
  inputArgs.AddLastArg(arguments, options::OPT_library_level);
  inputArgs.AddLastArg(arguments, options::OPT_enable_bare_slash_regex);
  inputArgs.AddLastArg(arguments, options::OPT_enable_experimental_cxx_interop);
  inputArgs.AddLastArg(arguments, options::OPT_cxx_interoperability_mode);
  inputArgs.AddLastArg(arguments, options::OPT_load_plugin_library);
  inputArgs.AddLastArg(arguments, options::OPT_load_plugin_executable);
  inputArgs.AddLastArg(arguments, options::OPT_enable_builtin_module);

  // Pass on any build config options
  inputArgs.AddAllArgs(arguments, options::OPT_D);

  // Pass on file paths that should be remapped in debug info.
  inputArgs.AddAllArgs(arguments, options::OPT_debug_prefix_map,
                                  options::OPT_coverage_prefix_map,
                                  options::OPT_file_prefix_map);

  std::string globalRemapping = getGlobalDebugPathRemapping();
  if (!globalRemapping.empty()) {
    arguments.push_back("-debug-prefix-map");
    arguments.push_back(inputArgs.MakeArgString(globalRemapping));
  }

  // Pass through the values passed to -Xfrontend.
  inputArgs.AddAllArgValues(arguments, options::OPT_Xfrontend);

  // Pass on module names whose symbols should be embedded in tbd.
  inputArgs.AddAllArgs(arguments, options::OPT_embed_tbd_for_module);

  if (auto *A = inputArgs.getLastArg(options::OPT_working_directory)) {
    // Add -Xcc -working-directory before any other -Xcc options to ensure it is
    // overridden by an explicit -Xcc -working-directory, although having a
    // different working directory is probably incorrect.
    SmallString<128> workingDirectory(A->getValue());
    llvm::sys::fs::make_absolute(workingDirectory);
    arguments.push_back("-Xcc");
    arguments.push_back("-working-directory");
    arguments.push_back("-Xcc");
    arguments.push_back(inputArgs.MakeArgString(workingDirectory));
  }

  addLTOArgs(OI, arguments);

  // -g implies -enable-anonymous-context-mangled-names, because the extra
  // metadata aids debugging.
  if (inputArgs.hasArg(options::OPT_g)) {
    // But don't add the option in optimized builds: it would prevent dead code
    // stripping of unused metadata.
    auto OptArg = inputArgs.getLastArgNoClaim(options::OPT_O_Group);
    if (!OptArg || OptArg->getOption().matches(options::OPT_Onone))
      arguments.push_back("-enable-anonymous-context-mangled-names");

    // TODO: Should we support -fcoverage-compilation-dir?
    inputArgs.AddAllArgs(arguments, options::OPT_file_compilation_dir);
  }

  // Add plugin path options.
  {
    SmallString<64> pluginPath;
    auto programPath = getDriver().getSwiftProgramPath();
    CompilerInvocation::computeRuntimeResourcePathFromExecutablePath(
        programPath, /*shared=*/true, pluginPath);

    auto defaultPluginPath = pluginPath;
    llvm::sys::path::append(defaultPluginPath, "host", "plugins");

    // Default plugin path.
    arguments.push_back("-plugin-path");
    arguments.push_back(inputArgs.MakeArgString(defaultPluginPath));

    // Local plugin path.
    llvm::sys::path::remove_filename(pluginPath); // Remove "swift"
    llvm::sys::path::remove_filename(pluginPath); // Remove "lib"
    llvm::sys::path::append(pluginPath, "local", "lib");
    llvm::sys::path::append(pluginPath, "swift");
    llvm::sys::path::append(pluginPath, "host", "plugins");
    arguments.push_back("-plugin-path");
    arguments.push_back(inputArgs.MakeArgString(pluginPath));
  }

  // Pass through any subsystem flags.
  inputArgs.AddAllArgs(arguments, options::OPT_Xllvm);
  inputArgs.AddAllArgs(arguments, options::OPT_Xcc);
}

static void addRuntimeLibraryFlags(const OutputInfo &OI,
                                   ArgStringList &Arguments) {
  if (!OI.RuntimeVariant)
    return;

  const OutputInfo::MSVCRuntime RT = OI.RuntimeVariant.value();

  Arguments.push_back("-autolink-library");
  Arguments.push_back("oldnames");

  Arguments.push_back("-autolink-library");
  switch (RT) {
  case OutputInfo::MSVCRuntime::MultiThreaded:
    Arguments.push_back("libcmt");
    break;

  case OutputInfo::MSVCRuntime::MultiThreadedDebug:
    Arguments.push_back("libcmtd");
    break;

  case OutputInfo::MSVCRuntime::MultiThreadedDLL:
    Arguments.push_back("msvcrt");
    break;

  case OutputInfo::MSVCRuntime::MultiThreadedDebugDLL:
    Arguments.push_back("msvcrtd");
    break;
  }

  // NOTE(compnerd) we do not support /ML and /MLd
  Arguments.push_back("-Xcc");
  Arguments.push_back("-D_MT");

  if (RT == OutputInfo::MSVCRuntime::MultiThreadedDLL ||
      RT == OutputInfo::MSVCRuntime::MultiThreadedDebugDLL) {
    Arguments.push_back("-Xcc");
    Arguments.push_back("-D_DLL");
  }
}

ToolChain::InvocationInfo
ToolChain::constructInvocation(const CompileJobAction &job,
                               const JobContext &context) const {
  InvocationInfo II{SWIFT_EXECUTABLE_NAME};
  ArgStringList &Arguments = II.Arguments;
  II.allowsResponseFiles = true;

  for (auto &s : getDriver().getSwiftProgramArgs())
    Arguments.push_back(s.c_str());
  Arguments.push_back("-frontend");

  {
    // Determine the frontend mode option.
    const char *FrontendModeOption = context.computeFrontendModeForCompile();
    assert(FrontendModeOption != nullptr &&
           "No frontend mode option specified!");
    Arguments.push_back(FrontendModeOption);
  }

  context.addFrontendInputAndOutputArguments(Arguments, II.FilelistInfos);

  // Forward migrator flags.
  if (auto DataPath =
          context.Args.getLastArg(options::OPT_api_diff_data_file)) {
    Arguments.push_back("-api-diff-data-file");
    Arguments.push_back(DataPath->getValue());
  }
  if (auto DataDir = context.Args.getLastArg(options::OPT_api_diff_data_dir)) {
    Arguments.push_back("-api-diff-data-dir");
    Arguments.push_back(DataDir->getValue());
  }
  if (context.Args.hasArg(options::OPT_dump_usr)) {
    Arguments.push_back("-dump-usr");
  }

  if (context.Args.hasArg(options::OPT_parse_stdlib))
    Arguments.push_back("-disable-objc-attr-requires-foundation-module");

  addCommonFrontendArgs(context.OI, context.Output, context.Args, Arguments);
  addRuntimeLibraryFlags(context.OI, Arguments);

  // Pass along an -import-objc-header arg, replacing the argument with the name
  // of any input PCH to the current action if one is present.
  if (context.Args.hasArgNoClaim(options::OPT_import_objc_header)) {
    bool ForwardAsIs = true;
    bool bridgingPCHIsEnabled =
        context.Args.hasFlag(options::OPT_enable_bridging_pch,
                             options::OPT_disable_bridging_pch, true);
    bool usePersistentPCH = bridgingPCHIsEnabled &&
                            context.Args.hasArg(options::OPT_pch_output_dir);
    if (!usePersistentPCH) {
      for (auto *IJ : context.Inputs) {
        if (!IJ->getOutput().getAnyOutputForType(file_types::TY_PCH).empty()) {
          Arguments.push_back("-import-objc-header");
          addInputsOfType(Arguments, context.Inputs, context.Args,
                          file_types::TY_PCH);
          ForwardAsIs = false;
          break;
        }
      }
    }
    if (ForwardAsIs) {
      context.Args.AddLastArg(Arguments, options::OPT_import_objc_header);
    }
    if (usePersistentPCH) {
      context.Args.AddLastArg(Arguments, options::OPT_pch_output_dir);
      switch (context.OI.CompilerMode) {
      case OutputInfo::Mode::StandardCompile:
      case OutputInfo::Mode::BatchModeCompile:
        // In the 'multiple invocations for each file' mode we don't need to
        // validate the PCH every time, it has been validated with the initial
        // -emit-pch invocation.
        Arguments.push_back("-pch-disable-validation");
        break;

      case OutputInfo::Mode::Immediate:
      case OutputInfo::Mode::REPL:
      case OutputInfo::Mode::SingleCompile:
        break;
      }
    }
  }

  if (context.Args.hasArg(options::OPT_parse_as_library) ||
      context.Args.hasArg(options::OPT_emit_library))
    Arguments.push_back("-parse-as-library");

  context.Args.AddLastArg(Arguments, options::OPT_parse_sil);

  Arguments.push_back("-module-name");
  Arguments.push_back(context.Args.MakeArgString(context.OI.ModuleName));

  if (context.Args.hasArg(options::OPT_CrossModuleOptimization)) {
    Arguments.push_back("-cross-module-optimization");
  }
                                 
  if (context.Args.hasArg(options::OPT_ExperimentalPerformanceAnnotations)) {
    Arguments.push_back("-experimental-performance-annotations");
  }

  file_types::ID remarksFileType = file_types::TY_YAMLOptRecord;
  // If a specific format is specified for the remarks, forward that as is.
  if (auto remarksFormat =
          context.Args.getLastArg(options::OPT_save_optimization_record_EQ)) {
    Arguments.push_back(context.Args.MakeArgString(
        Twine("-save-optimization-record=") + remarksFormat->getValue()));
    // If that's the case, add the proper output file for the type.
    if (llvm::Expected<file_types::ID> fileType =
            remarkFileTypeFromArgs(context.Args))
      remarksFileType = *fileType;
    else
      consumeError(fileType.takeError()); // Don't report errors here. This will
                                          // be reported later anyway.
  }
  addOutputsOfType(Arguments, context.Output, context.Args, remarksFileType,
                   "-save-optimization-record-path");

  if (auto remarksFilter = context.Args.getLastArg(
          options::OPT_save_optimization_record_passes)) {
    Arguments.push_back("-save-optimization-record-passes");
    Arguments.push_back(remarksFilter->getValue());
  }

  if (context.Args.hasArg(options::OPT_migrate_keep_objc_visibility)) {
    Arguments.push_back("-migrate-keep-objc-visibility");
  }

  addOutputsOfType(Arguments, context.Output, context.Args,
                   file_types::TY_Remapping, "-emit-remap-file-path");

  if (context.OI.numThreads > 0) {
    Arguments.push_back("-num-threads");
    Arguments.push_back(
        context.Args.MakeArgString(Twine(context.OI.numThreads)));
  }

  // Add the output file argument if necessary.
  if (context.Output.getPrimaryOutputType() != file_types::TY_Nothing) {
    auto IndexUnitOutputs = context.Output.getIndexUnitOutputFilenames();

    if (context.shouldUseMainOutputFileListInFrontendInvocation()) {
      Arguments.push_back("-output-filelist");
      Arguments.push_back(context.getTemporaryFilePath("outputs", ""));
      II.FilelistInfos.push_back({Arguments.back(),
                                  context.Output.getPrimaryOutputType(),
                                  FilelistInfo::WhichFiles::Output});
      if (!IndexUnitOutputs.empty()) {
        Arguments.push_back("-index-unit-output-path-filelist");
        Arguments.push_back(context.getTemporaryFilePath("index-unit-outputs",
                                                         ""));
        II.FilelistInfos.push_back({
          Arguments.back(), file_types::TY_IndexUnitOutputPath,
          FilelistInfo::WhichFiles::IndexUnitOutputPaths});
      }
    } else {
      for (auto FileName : context.Output.getPrimaryOutputFilenames()) {
        Arguments.push_back("-o");
        Arguments.push_back(context.Args.MakeArgString(FileName));
      }

      for (auto FileName : IndexUnitOutputs) {
        Arguments.push_back("-index-unit-output-path");
        Arguments.push_back(context.Args.MakeArgString(FileName));
      }
    }
  }

  if (context.Args.hasArg(options::OPT_embed_bitcode_marker))
    Arguments.push_back("-embed-bitcode-marker");

  // For `-index-file` mode add `-disable-typo-correction`, since the errors
  // will be ignored and it can be expensive to do typo-correction.
  if (job.getType() == file_types::TY_IndexData) {
    Arguments.push_back("-disable-typo-correction");
  }

  if (context.Args.hasArg(options::OPT_index_store_path)) {
    context.Args.AddLastArg(Arguments, options::OPT_index_store_path);
    if (!context.Args.hasArg(options::OPT_index_ignore_system_modules))
      Arguments.push_back("-index-system-modules");
    context.Args.AddLastArg(Arguments, options::OPT_index_ignore_clang_modules);
    context.Args.AddLastArg(Arguments, options::OPT_index_include_locals);
  }

  if (context.Args.hasArg(options::OPT_debug_info_store_invocation) ||
      shouldStoreInvocationInDebugInfo()) {
    Arguments.push_back("-debug-info-store-invocation");
  }

  if (context.Args.hasArg(
                      options::OPT_disable_autolinking_runtime_compatibility)) {
    Arguments.push_back("-disable-autolinking-runtime-compatibility");
  }

  if (auto arg = context.Args.getLastArg(
                                  options::OPT_runtime_compatibility_version)) {
    Arguments.push_back("-runtime-compatibility-version");
    Arguments.push_back(arg->getValue());
  }
  if (context.Args.hasArg(options::OPT_track_system_dependencies)) {
    Arguments.push_back("-track-system-dependencies");
  }

  if (context.Args.hasFlag(options::OPT_static_executable,
                           options::OPT_no_static_executable, false) ||
      context.Args.hasFlag(options::OPT_static_stdlib,
                           options::OPT_no_static_stdlib, false)) {
    Arguments.push_back("-use-static-resource-dir");
  }

  context.Args.AddLastArg(
      Arguments,
      options::
          OPT_disable_autolinking_runtime_compatibility_dynamic_replacements);
  context.Args.AddLastArg(
      Arguments,
      options::OPT_disable_autolinking_runtime_compatibility_concurrency);

  if (context.OI.CompilerMode == OutputInfo::Mode::SingleCompile) {
    context.Args.AddLastArg(Arguments, options::OPT_emit_symbol_graph);
    context.Args.AddLastArg(Arguments, options::OPT_emit_symbol_graph_dir);
  }
  context.Args.AddLastArg(Arguments, options::OPT_include_spi_symbols);
  context.Args.AddLastArg(Arguments, options::OPT_emit_extension_block_symbols,
                          options::OPT_omit_extension_block_symbols);
  context.Args.AddLastArg(Arguments, options::OPT_symbol_graph_minimum_access_level);

  return II;
}

const char *ToolChain::JobContext::computeFrontendModeForCompile() const {
  switch (OI.CompilerMode) {
  case OutputInfo::Mode::StandardCompile:
  case OutputInfo::Mode::SingleCompile:
  case OutputInfo::Mode::BatchModeCompile:
    break;
  case OutputInfo::Mode::Immediate:
  case OutputInfo::Mode::REPL:
    llvm_unreachable("REPL and immediate modes handled elsewhere");
  }
  switch (Output.getPrimaryOutputType()) {
  case file_types::TY_Object:
    return "-c";
  case file_types::TY_PCH:
    return "-emit-pch";
  case file_types::TY_ASTDump:
    return "-dump-ast";
  case file_types::TY_RawSIL:
    return "-emit-silgen";
  case file_types::TY_SIL:
    return "-emit-sil";
  case file_types::TY_RawSIB:
    return "-emit-sibgen";
  case file_types::TY_SIB:
    return "-emit-sib";
  case file_types::TY_LLVM_IR:
    return "-emit-ir";
  case file_types::TY_LLVM_BC:
    return "-emit-bc";
  case file_types::TY_ClangModuleFile:
    return "-emit-pcm";
  case file_types::TY_Assembly:
    return "-S";
  case file_types::TY_SwiftModuleFile:
    // Since this is our primary output, we need to specify the option here.
    return "-emit-module";
  case file_types::TY_ImportedModules:
    return "-emit-imported-modules";
  case file_types::TY_JSONDependencies:
    return "-scan-dependencies";
  case file_types::TY_JSONFeatures:
    return "-emit-supported-features";
  case file_types::TY_IndexData:
    return "-typecheck";
  case file_types::TY_Remapping:
    return "-update-code";
  case file_types::TY_Nothing:
    // We were told to output nothing, so get the last mode option and use that.
    if (const Arg *A = Args.getLastArg(options::OPT_modes_Group))
      return A->getSpelling().data();
    else
      llvm_unreachable("We were told to perform a standard compile, "
                       "but no mode option was passed to the driver.");
  case file_types::TY_Swift:
  case file_types::TY_dSYM:
  case file_types::TY_AutolinkFile:
  case file_types::TY_Dependencies:
  case file_types::TY_SwiftModuleDocFile:
  case file_types::TY_SerializedDiagnostics:
  case file_types::TY_ClangHeader:
  case file_types::TY_Image:
  case file_types::TY_SwiftDeps:
  case file_types::TY_ExternalSwiftDeps:
  case file_types::TY_ModuleTrace:
  case file_types::TY_TBD:
  case file_types::TY_YAMLOptRecord:
  case file_types::TY_BitstreamOptRecord:
  case file_types::TY_SwiftModuleInterfaceFile:
  case file_types::TY_PrivateSwiftModuleInterfaceFile:
  case file_types::TY_SwiftModuleSummaryFile:
  case file_types::TY_SwiftSourceInfoFile:
  case file_types::TY_SwiftCrossImportDir:
  case file_types::TY_SwiftOverlayFile:
  case file_types::TY_IndexUnitOutputPath:
  case file_types::TY_SwiftABIDescriptor:
  case file_types::TY_ConstValues:
  case file_types::TY_SwiftFixIt:
  case file_types::TY_ModuleSemanticInfo:
  case file_types::TY_CachedDiagnostics:
    llvm_unreachable("Output type can never be primary output.");
  case file_types::TY_INVALID:
    llvm_unreachable("Invalid type ID");
  }
  llvm_unreachable("unhandled output type");
}

void ToolChain::JobContext::addFrontendInputAndOutputArguments(
    ArgStringList &Arguments, std::vector<FilelistInfo> &FilelistInfos) const {

  switch (OI.CompilerMode) {
  case OutputInfo::Mode::StandardCompile:
    assert(InputActions.size() == 1 &&
           "Standard-compile mode takes exactly one input (the primary file)");
    break;
  case OutputInfo::Mode::BatchModeCompile:
  case OutputInfo::Mode::SingleCompile:
    break;
  case OutputInfo::Mode::Immediate:
  case OutputInfo::Mode::REPL:
    llvm_unreachable("REPL and immediate modes handled elsewhere");
  }

  const bool UseFileList = shouldUseInputFileList();
  const bool MayHavePrimaryInputs = OI.mightHaveExplicitPrimaryInputs(Output);
  const bool UsePrimaryFileList =
      MayHavePrimaryInputs &&
      shouldUsePrimaryInputFileListInFrontendInvocation();
  const bool FilterInputsByType = shouldFilterFrontendInputsByType();
  const bool UseSupplementaryOutputFileList =
      shouldUseSupplementaryOutputFileMapInFrontendInvocation();

  assert((C.getFilelistThreshold() != Compilation::NEVER_USE_FILELIST ||
          !UseFileList && !UsePrimaryFileList &&
              !UseSupplementaryOutputFileList) &&
         "No filelists are used if FilelistThreshold=NEVER_USE_FILELIST");

  if (UseFileList) {
    Arguments.push_back("-filelist");
    Arguments.push_back(getAllSourcesPath());
  }
  if (UsePrimaryFileList) {
    Arguments.push_back("-primary-filelist");
    Arguments.push_back(getTemporaryFilePath("primaryInputs", ""));
    FilelistInfos.push_back({Arguments.back(), file_types::TY_Swift,
                             FilelistInfo::WhichFiles::SourceInputActions});
  }
  if (!UseFileList || !UsePrimaryFileList) {
    addFrontendCommandLineInputArguments(MayHavePrimaryInputs, UseFileList,
                                         UsePrimaryFileList, FilterInputsByType,
                                         Arguments);
  }

  if (UseSupplementaryOutputFileList) {
    Arguments.push_back("-supplementary-output-file-map");
    Arguments.push_back(getTemporaryFilePath("supplementaryOutputs", ""));
    FilelistInfos.push_back({Arguments.back(), file_types::TY_INVALID,
                             FilelistInfo::WhichFiles::SupplementaryOutput});
  } else {
    addFrontendSupplementaryOutputArguments(Arguments);
  }
}

void ToolChain::JobContext::addFrontendCommandLineInputArguments(
    const bool mayHavePrimaryInputs, const bool useFileList,
    const bool usePrimaryFileList, const bool filterByType,
    ArgStringList &arguments) const {
  llvm::DenseSet<StringRef> primaries;

  if (mayHavePrimaryInputs) {
    for (const Action *A : InputActions) {
      const auto *IA = cast<InputAction>(A);
      const llvm::opt::Arg &InArg = IA->getInputArg();
      primaries.insert(InArg.getValue());
    }
  }
  // -index-file compilations are weird. They are processed as SingleCompiles
  // (WMO), but must indicate that there is one primary file, designated by
  // -index-file-path.
  if (Arg *A = Args.getLastArg(options::OPT_index_file_path)) {
    assert(primaries.empty() &&
           "index file jobs should be treated as single (WMO) compiles");
    primaries.insert(A->getValue());
  }
  for (auto inputPair : getTopLevelInputFiles()) {
    if (filterByType && !file_types::isPartOfSwiftCompilation(inputPair.first))
      continue;
    const char *inputName = inputPair.second->getValue();
    const bool isPrimary = primaries.count(inputName);
    if (isPrimary && !usePrimaryFileList) {
      arguments.push_back("-primary-file");
      arguments.push_back(inputName);
    }
    if ((!isPrimary || usePrimaryFileList) && !useFileList)
      arguments.push_back(inputName);
  }
}

void ToolChain::JobContext::addFrontendSupplementaryOutputArguments(
    ArgStringList &arguments) const {
  // FIXME: Get these and other argument strings from the same place for both
  // driver and frontend.
  addOutputsOfType(arguments, Output, Args, file_types::ID::TY_SwiftModuleFile,
                   "-emit-module-path");

  addOutputsOfType(arguments, Output, Args, file_types::TY_SwiftModuleDocFile,
                   "-emit-module-doc-path");

  addOutputsOfType(arguments, Output, Args, file_types::TY_SwiftSourceInfoFile,
                   "-emit-module-source-info-path");

  addOutputsOfType(arguments, Output, Args,
                   file_types::ID::TY_SwiftModuleInterfaceFile,
                   "-emit-module-interface-path");

  addOutputsOfType(arguments, Output, Args,
                   file_types::ID::TY_PrivateSwiftModuleInterfaceFile,
                   "-emit-private-module-interface-path");

  addOutputsOfType(arguments, Output, Args,
                   file_types::TY_SerializedDiagnostics,
                   "-serialize-diagnostics-path");

  if (addOutputsOfType(arguments, Output, Args, file_types::ID::TY_ClangHeader,
                       "-emit-objc-header-path")) {
    assert(OI.CompilerMode == OutputInfo::Mode::SingleCompile &&
           "The Swift tool should only emit an Obj-C header in single compile"
           "mode!");
  }

  addOutputsOfType(arguments, Output, Args, file_types::TY_Dependencies,
                   "-emit-dependencies-path");
  addOutputsOfType(arguments, Output, Args, file_types::TY_SwiftDeps,
                   "-emit-reference-dependencies-path");
  addOutputsOfType(arguments, Output, Args, file_types::TY_ModuleTrace,
                   "-emit-loaded-module-trace-path");
  addOutputsOfType(arguments, Output, Args, file_types::TY_TBD,
                   "-emit-tbd-path");
  addOutputsOfType(arguments, Output, Args,
                   file_types::TY_SwiftModuleSummaryFile,
                   "-emit-module-summary-path");
}

ToolChain::InvocationInfo
ToolChain::constructInvocation(const InterpretJobAction &job,
                               const JobContext &context) const {
  assert(context.OI.CompilerMode == OutputInfo::Mode::Immediate);

  InvocationInfo II{SWIFT_EXECUTABLE_NAME};
  ArgStringList &Arguments = II.Arguments;
  II.allowsResponseFiles = true;

  for (auto &s : getDriver().getSwiftProgramArgs())
    Arguments.push_back(s.c_str());
  Arguments.push_back("-frontend");
  Arguments.push_back("-interpret");

  assert(context.Inputs.empty() &&
         "The Swift frontend does not expect to be fed any input Jobs!");

  for (const Action *A : context.InputActions) {
    cast<InputAction>(A)->getInputArg().render(context.Args, Arguments);
  }

  if (context.Args.hasArg(options::OPT_parse_stdlib))
    Arguments.push_back("-disable-objc-attr-requires-foundation-module");

  addCommonFrontendArgs(context.OI, context.Output, context.Args, Arguments);
  addRuntimeLibraryFlags(context.OI, Arguments);

  context.Args.AddLastArg(Arguments, options::OPT_import_objc_header);

  context.Args.AddLastArg(Arguments, options::OPT_parse_sil);

  Arguments.push_back("-module-name");
  Arguments.push_back(context.Args.MakeArgString(context.OI.ModuleName));

  context.Args.AddAllArgs(Arguments, options::OPT_framework);
  ToolChain::addLinkedLibArgs(context.Args, Arguments);

  // The immediate arguments must be last.
  context.Args.AddLastArg(Arguments, options::OPT__DASH_DASH);

  return II;
}

ToolChain::InvocationInfo
ToolChain::constructInvocation(const BackendJobAction &job,
                               const JobContext &context) const {
  assert(context.Args.hasArg(options::OPT_embed_bitcode));
  ArgStringList Arguments;

  for (auto &s : getDriver().getSwiftProgramArgs())
    Arguments.push_back(s.c_str());
  Arguments.push_back("-frontend");

  // Determine the frontend mode option.
  const char *FrontendModeOption = nullptr;
  switch (context.OI.CompilerMode) {
  case OutputInfo::Mode::StandardCompile:
  case OutputInfo::Mode::SingleCompile: {
    switch (context.Output.getPrimaryOutputType()) {
    case file_types::TY_Object:
      FrontendModeOption = "-c";
      break;
    case file_types::TY_LLVM_IR:
      FrontendModeOption = "-emit-ir";
      break;
    case file_types::TY_LLVM_BC:
      FrontendModeOption = "-emit-bc";
      break;
    case file_types::TY_Assembly:
      FrontendModeOption = "-S";
      break;
    case file_types::TY_Nothing:
      // We were told to output nothing, so get the last mode option and use
      // that.
      if (const Arg *A = context.Args.getLastArg(options::OPT_modes_Group))
        FrontendModeOption = A->getSpelling().data();
      else
        llvm_unreachable("We were told to perform a standard compile, "
                         "but no mode option was passed to the driver.");
      break;

    case file_types::TY_ImportedModules:
    case file_types::TY_TBD:
    case file_types::TY_SwiftModuleFile:
    case file_types::TY_ASTDump:
    case file_types::TY_RawSIL:
    case file_types::TY_RawSIB:
    case file_types::TY_SIL:
    case file_types::TY_SIB:
    case file_types::TY_PCH:
    case file_types::TY_ClangModuleFile:
    case file_types::TY_IndexData:
    case file_types::TY_JSONDependencies:
    case file_types::TY_JSONFeatures:
      llvm_unreachable("Cannot be output from backend job");
    case file_types::TY_Swift:
    case file_types::TY_dSYM:
    case file_types::TY_AutolinkFile:
    case file_types::TY_Dependencies:
    case file_types::TY_SwiftModuleDocFile:
    case file_types::TY_SerializedDiagnostics:
    case file_types::TY_ClangHeader:
    case file_types::TY_Image:
    case file_types::TY_SwiftDeps:
    case file_types::TY_ExternalSwiftDeps:
    case file_types::TY_Remapping:
    case file_types::TY_ModuleTrace:
    case file_types::TY_YAMLOptRecord:
    case file_types::TY_BitstreamOptRecord:
    case file_types::TY_SwiftModuleInterfaceFile:
    case file_types::TY_PrivateSwiftModuleInterfaceFile:
    case file_types::TY_SwiftModuleSummaryFile:
    case file_types::TY_SwiftSourceInfoFile:
    case file_types::TY_SwiftCrossImportDir:
    case file_types::TY_SwiftOverlayFile:
    case file_types::TY_IndexUnitOutputPath:
    case file_types::TY_SwiftABIDescriptor:
    case file_types::TY_ConstValues:
    case file_types::TY_SwiftFixIt:
    case file_types::TY_ModuleSemanticInfo:
    case file_types::TY_CachedDiagnostics:
      llvm_unreachable("Output type can never be primary output.");
    case file_types::TY_INVALID:
      llvm_unreachable("Invalid type ID");
    }
    break;
  }
  case OutputInfo::Mode::BatchModeCompile:
  case OutputInfo::Mode::Immediate:
  case OutputInfo::Mode::REPL:
    llvm_unreachable("invalid mode for backend job");
  }

  assert(FrontendModeOption != nullptr && "No frontend mode option specified!");

  Arguments.push_back(FrontendModeOption);

  // Add input arguments.
  switch (context.OI.CompilerMode) {
  case OutputInfo::Mode::StandardCompile: {
    assert(context.Inputs.size() == 1 && "The backend expects one input!");
    Arguments.push_back("-primary-file");
    const Job *Cmd = context.Inputs.front();
    Arguments.push_back(context.Args.MakeArgString(
        Cmd->getOutput().getPrimaryOutputFilename()));
    break;
  }
  case OutputInfo::Mode::SingleCompile: {
    assert(context.Inputs.size() == 1 && "The backend expects one input!");
    Arguments.push_back("-primary-file");
    const Job *Cmd = context.Inputs.front();

    // In multi-threaded compilation, the backend job must select the correct
    // output file of the compilation job.
    auto OutNames = Cmd->getOutput().getPrimaryOutputFilenames();
    Arguments.push_back(
        context.Args.MakeArgString(OutNames[job.getInputIndex()]));
    break;
  }
  case OutputInfo::Mode::BatchModeCompile:
  case OutputInfo::Mode::Immediate:
  case OutputInfo::Mode::REPL:
    llvm_unreachable("invalid mode for backend job");
  }

  // Add flags implied by -embed-bitcode.
  Arguments.push_back("-embed-bitcode");

  // -embed-bitcode only supports a restricted set of flags.
  Arguments.push_back("-target");
  Arguments.push_back(context.Args.MakeArgString(getTriple().str()));

  // Enable address top-byte ignored in the ARM64 backend.
  if (getTriple().getArch() == llvm::Triple::aarch64 ||
      getTriple().getArch() == llvm::Triple::aarch64_32) {
    Arguments.push_back("-Xllvm");
    Arguments.push_back("-aarch64-use-tbi");
  }

  // Handle the CPU and its preferences.
  context.Args.AddLastArg(Arguments, options::OPT_target_cpu);

  // Enable optimizations, but disable all LLVM-IR-level transformations.
  context.Args.AddLastArg(Arguments, options::OPT_O_Group);
  Arguments.push_back("-disable-llvm-optzns");

  context.Args.AddLastArg(Arguments, options::OPT_parse_stdlib);

  Arguments.push_back("-module-name");
  Arguments.push_back(context.Args.MakeArgString(context.OI.ModuleName));

  // Add the output file argument if necessary.
  if (context.Output.getPrimaryOutputType() != file_types::TY_Nothing) {
    for (auto FileName : context.Output.getPrimaryOutputFilenames()) {
      Arguments.push_back("-o");
      Arguments.push_back(context.Args.MakeArgString(FileName));
    }
  }

  return {SWIFT_EXECUTABLE_NAME, Arguments};
}

ToolChain::InvocationInfo
ToolChain::constructInvocation(const MergeModuleJobAction &job,
                               const JobContext &context) const {
  InvocationInfo II{SWIFT_EXECUTABLE_NAME};
  ArgStringList &Arguments = II.Arguments;
  II.allowsResponseFiles = true;

  for (auto &s : getDriver().getSwiftProgramArgs())
    Arguments.push_back(s.c_str());
  Arguments.push_back("-frontend");

  Arguments.push_back("-merge-modules");
  Arguments.push_back("-emit-module");

  if (context.shouldUseInputFileList()) {
    Arguments.push_back("-filelist");
    Arguments.push_back(context.getTemporaryFilePath("inputs", ""));
    II.FilelistInfos.push_back({Arguments.back(),
                                file_types::TY_SwiftModuleFile,
                                FilelistInfo::WhichFiles::InputJobs});

    addInputsOfType(Arguments, context.InputActions,
                    file_types::TY_SwiftModuleFile);
  } else {
    size_t origLen = Arguments.size();
    (void)origLen;
    addInputsOfType(Arguments, context.Inputs, context.Args,
                    file_types::TY_SwiftModuleFile);
    addInputsOfType(Arguments, context.InputActions,
                    file_types::TY_SwiftModuleFile);
    assert(Arguments.size() - origLen >=
               context.Inputs.size() + context.InputActions.size() ||
           context.OI.CompilerOutputType == file_types::TY_Nothing);
    assert((Arguments.size() - origLen == context.Inputs.size() ||
            context.OI.CompilerOutputType == file_types::TY_Nothing ||
            !context.InputActions.empty()) &&
           "every input to MergeModule must generate a swiftmodule");
  }

  // Tell all files to parse as library, which is necessary to load them as
  // serialized ASTs.
  Arguments.push_back("-parse-as-library");

  // Disable SIL optimization passes; we've already optimized the code in each
  // partial mode.
  Arguments.push_back("-disable-diagnostic-passes");
  Arguments.push_back("-disable-sil-perf-optzns");

  addCommonFrontendArgs(context.OI, context.Output, context.Args, Arguments);
  addRuntimeLibraryFlags(context.OI, Arguments);

  addOutputsOfType(Arguments, context.Output, context.Args,
                   file_types::TY_SwiftModuleDocFile, "-emit-module-doc-path");
  addOutputsOfType(Arguments, context.Output, context.Args,
                   file_types::TY_SwiftSourceInfoFile,
                   "-emit-module-source-info-path");
  addOutputsOfType(Arguments, context.Output, context.Args,
                   file_types::ID::TY_SwiftModuleInterfaceFile,
                   "-emit-module-interface-path");
  addOutputsOfType(Arguments, context.Output, context.Args,
                   file_types::ID::TY_PrivateSwiftModuleInterfaceFile,
                   "-emit-private-module-interface-path");
  addOutputsOfType(Arguments, context.Output, context.Args,
                   file_types::TY_SerializedDiagnostics,
                   "-serialize-diagnostics-path");
  addOutputsOfType(Arguments, context.Output, context.Args,
                   file_types::TY_ClangHeader, "-emit-objc-header-path");
  addOutputsOfType(Arguments, context.Output, context.Args, file_types::TY_TBD,
                   "-emit-tbd-path");

  context.Args.AddLastArg(Arguments, options::OPT_emit_symbol_graph);
  context.Args.AddLastArg(Arguments, options::OPT_emit_symbol_graph_dir);
  context.Args.AddLastArg(Arguments, options::OPT_include_spi_symbols);
  context.Args.AddLastArg(Arguments, options::OPT_emit_extension_block_symbols,
                          options::OPT_omit_extension_block_symbols);
  context.Args.AddLastArg(Arguments, options::OPT_symbol_graph_minimum_access_level);

  context.Args.AddLastArg(Arguments, options::OPT_import_objc_header);

  context.Args.AddLastArg(Arguments, options::OPT_disable_incremental_imports);

  Arguments.push_back("-module-name");
  Arguments.push_back(context.Args.MakeArgString(context.OI.ModuleName));

  assert(context.Output.getPrimaryOutputType() ==
             file_types::TY_SwiftModuleFile &&
         "The MergeModule tool only produces swiftmodule files!");

  Arguments.push_back("-o");
  Arguments.push_back(
      context.Args.MakeArgString(context.Output.getPrimaryOutputFilename()));

  return II;
}

ToolChain::InvocationInfo
ToolChain::constructInvocation(const VerifyModuleInterfaceJobAction &job,
                               const JobContext &context) const {
  InvocationInfo II{SWIFT_EXECUTABLE_NAME};
  ArgStringList &Arguments = II.Arguments;
  II.allowsResponseFiles = true;

  for (auto &s : getDriver().getSwiftProgramArgs())
    Arguments.push_back(s.c_str());
  Arguments.push_back("-frontend");

  Arguments.push_back("-typecheck-module-from-interface");

  size_t sizeBefore = Arguments.size();
  addInputsOfType(Arguments, context.Inputs, context.Args, job.getInputType());

  (void)sizeBefore;
  assert(Arguments.size() - sizeBefore == 1 &&
         "should verify exactly one module interface per job");

  addCommonFrontendArgs(context.OI, context.Output, context.Args, Arguments);
  addRuntimeLibraryFlags(context.OI, Arguments);

  addOutputsOfType(Arguments, context.Output, context.Args,
                   file_types::TY_SerializedDiagnostics,
                   "-serialize-diagnostics-path");

  context.Args.AddLastArg(Arguments, options::OPT_import_objc_header);

  Arguments.push_back("-module-name");
  Arguments.push_back(context.Args.MakeArgString(context.OI.ModuleName));

  return II;
}

ToolChain::InvocationInfo
ToolChain::constructInvocation(const ModuleWrapJobAction &job,
                               const JobContext &context) const {
  InvocationInfo II{SWIFT_EXECUTABLE_NAME};
  ArgStringList &Arguments = II.Arguments;
  II.allowsResponseFiles = true;

  for (auto &s : getDriver().getSwiftProgramArgs())
    Arguments.push_back(s.c_str());
  Arguments.push_back("-modulewrap");

  addInputsOfType(Arguments, context.Inputs, context.Args,
                  file_types::TY_SwiftModuleFile);
  addInputsOfType(Arguments, context.InputActions,
                  file_types::TY_SwiftModuleFile);
  assert(Arguments.size() == 2 &&
         "ModuleWrap expects exactly one merged swiftmodule as input");

  assert(context.Output.getPrimaryOutputType() == file_types::TY_Object &&
         "The -modulewrap mode only produces object files");

  Arguments.push_back("-target");
  Arguments.push_back(context.Args.MakeArgString(getTriple().str()));

  Arguments.push_back("-o");
  Arguments.push_back(
      context.Args.MakeArgString(context.Output.getPrimaryOutputFilename()));

  return II;
}

ToolChain::InvocationInfo
ToolChain::constructInvocation(const REPLJobAction &job,
                               const JobContext &context) const {
  assert(context.Inputs.empty());
  assert(context.InputActions.empty());

  bool useLLDB;

  switch (job.getRequestedMode()) {
  case REPLJobAction::Mode::Integrated:
    useLLDB = false;
    break;
  case REPLJobAction::Mode::RequireLLDB:
    useLLDB = true;
    break;
  case REPLJobAction::Mode::PreferLLDB:
    useLLDB = !findProgramRelativeToSwift("lldb").empty();
    break;
  }

  ArgStringList FrontendArgs;
  for (auto &s : getDriver().getSwiftProgramArgs())
    FrontendArgs.push_back(s.c_str());

  addCommonFrontendArgs(context.OI, context.Output, context.Args, FrontendArgs);
  addRuntimeLibraryFlags(context.OI, FrontendArgs);

  context.Args.AddLastArg(FrontendArgs, options::OPT_import_objc_header);
  context.Args.AddAllArgs(FrontendArgs, options::OPT_framework, options::OPT_L);
  ToolChain::addLinkedLibArgs(context.Args, FrontendArgs);

  if (!useLLDB) {
    FrontendArgs.insert(FrontendArgs.begin(), {"-frontend", "-repl"});
    FrontendArgs.push_back("-module-name");
    FrontendArgs.push_back(context.Args.MakeArgString(context.OI.ModuleName));
    return {SWIFT_EXECUTABLE_NAME, FrontendArgs};
  }

  // Squash important frontend options into a single argument for LLDB.
  std::string SingleArg = "--repl=";
  {
    llvm::raw_string_ostream os(SingleArg);
    Job::printArguments(os, FrontendArgs);
  }

  ArgStringList Arguments;
  Arguments.push_back(context.Args.MakeArgString(std::move(SingleArg)));

  return {"lldb", Arguments};
}

ToolChain::InvocationInfo
ToolChain::constructInvocation(const GenerateDSYMJobAction &job,
                               const JobContext &context) const {
  assert(context.Inputs.size() == 1);
  assert(context.InputActions.empty());
  assert(context.Output.getPrimaryOutputType() == file_types::TY_dSYM);

  ArgStringList Arguments;

  auto inputPath =
      context.Inputs.front()->getOutput().getPrimaryOutputFilename();
  Arguments.push_back(context.Args.MakeArgString(inputPath));

  Arguments.push_back("-o");
  Arguments.push_back(
      context.Args.MakeArgString(context.Output.getPrimaryOutputFilename()));

  return {"dsymutil", Arguments};
}

ToolChain::InvocationInfo
ToolChain::constructInvocation(const VerifyDebugInfoJobAction &job,
                               const JobContext &context) const {
  assert(context.Inputs.size() == 1);
  assert(context.InputActions.empty());

  // This mirrors the clang driver's --verify-debug-info option.
  ArgStringList Arguments;
  Arguments.push_back("--verify");
  Arguments.push_back("--debug-info");
  Arguments.push_back("--eh-frame");
  Arguments.push_back("--quiet");

  auto inputPath =
      context.Inputs.front()->getOutput().getPrimaryOutputFilename();
  Arguments.push_back(context.Args.MakeArgString(inputPath));

  return {"dwarfdump", Arguments};
}

ToolChain::InvocationInfo
ToolChain::constructInvocation(const GeneratePCHJobAction &job,
                               const JobContext &context) const {
  assert(context.Inputs.empty());
  assert(context.InputActions.size() == 1);
  assert((!job.isPersistentPCH() &&
          context.Output.getPrimaryOutputType() == file_types::TY_PCH) ||
         (job.isPersistentPCH() &&
          context.Output.getPrimaryOutputType() == file_types::TY_Nothing));

  InvocationInfo II{SWIFT_EXECUTABLE_NAME};
  ArgStringList &Arguments = II.Arguments;
  II.allowsResponseFiles = true;

  for (auto &s : getDriver().getSwiftProgramArgs())
    Arguments.push_back(s.c_str());
  Arguments.push_back("-frontend");

  addCommonFrontendArgs(context.OI, context.Output, context.Args, Arguments);
  addRuntimeLibraryFlags(context.OI, Arguments);

  addOutputsOfType(Arguments, context.Output, context.Args,
                   file_types::TY_SerializedDiagnostics,
                   "-serialize-diagnostics-path");

  addInputsOfType(Arguments, context.InputActions, file_types::TY_ClangHeader);
  context.Args.AddLastArg(Arguments, options::OPT_index_store_path);

  if (job.isPersistentPCH()) {
    Arguments.push_back("-emit-pch");
    Arguments.push_back("-pch-output-dir");
    Arguments.push_back(context.Args.MakeArgString(job.getPersistentPCHDir()));
  } else {
    Arguments.push_back("-emit-pch");
    Arguments.push_back("-o");
    Arguments.push_back(
        context.Args.MakeArgString(context.Output.getPrimaryOutputFilename()));
  }

  return II;
}

ToolChain::InvocationInfo
ToolChain::constructInvocation(const AutolinkExtractJobAction &job,
                               const JobContext &context) const {
  llvm_unreachable("autolink extraction not implemented for this toolchain");
}

ToolChain::InvocationInfo
ToolChain::constructInvocation(const DynamicLinkJobAction &job,
                               const JobContext &context) const {
  llvm_unreachable("linking not implemented for this toolchain");
}

ToolChain::InvocationInfo
ToolChain::constructInvocation(const StaticLinkJobAction &job,
                               const JobContext &context) const {
   llvm_unreachable("archiving not implemented for this toolchain");
}

void ToolChain::addPathEnvironmentVariableIfNeeded(
    Job::EnvironmentVector &env, const char *name, const char *separator,
    options::ID optionID, const ArgList &args,
    ArrayRef<std::string> extraEntries) const {
  auto linkPathOptions = args.filtered(optionID);
  if (linkPathOptions.begin() == linkPathOptions.end() && extraEntries.empty())
    return;

  std::string newPaths;
  interleave(linkPathOptions,
             [&](const Arg *arg) { newPaths.append(arg->getValue()); },
             [&] { newPaths.append(separator); });
  for (auto extraEntry : extraEntries) {
    if (!newPaths.empty())
      newPaths.append(separator);
    newPaths.append(extraEntry.data(), extraEntry.size());
  }
  if (auto currentPaths = llvm::sys::Process::GetEnv(name)) {
    newPaths.append(separator);
    newPaths.append(currentPaths.value());
  }
  env.emplace_back(name, args.MakeArgString(newPaths));
}

void ToolChain::addLinkRuntimeLib(const ArgList &Args, ArgStringList &Arguments,
                                  StringRef LibName) const {
  SmallString<128> P;
  getClangLibraryPath(Args, P);
  llvm::sys::path::append(P, LibName);
  Arguments.push_back(Args.MakeArgString(P));
}

void ToolChain::getClangLibraryPath(const ArgList &Args,
                                    SmallString<128> &LibPath) const {
  const llvm::Triple &T = getTriple();

  getResourceDirPath(LibPath, Args, /*Shared=*/true);
  // Remove platform name.
  llvm::sys::path::remove_filename(LibPath);
  llvm::sys::path::append(LibPath, "clang", "lib",
                          T.isOSDarwin() ? "darwin"
                                         : getPlatformNameForTriple(T));
}

/// Get the runtime library link path, which is platform-specific and found
/// relative to the compiler.
void ToolChain::getResourceDirPath(SmallVectorImpl<char> &resourceDirPath,
                                   const llvm::opt::ArgList &args,
                                   bool shared) const {
  if (const Arg *A = args.getLastArg(options::OPT_resource_dir)) {
    StringRef value = A->getValue();
    resourceDirPath.append(value.begin(), value.end());
  } else if (!getTriple().isOSDarwin() && args.hasArg(options::OPT_sdk)) {
    StringRef value = args.getLastArg(options::OPT_sdk)->getValue();
    resourceDirPath.append(value.begin(), value.end());
    llvm::sys::path::append(resourceDirPath, "usr");
    CompilerInvocation::appendSwiftLibDir(resourceDirPath, shared);
  } else {
    auto programPath = getDriver().getSwiftProgramPath();
    CompilerInvocation::computeRuntimeResourcePathFromExecutablePath(
        programPath, shared, resourceDirPath);
  }

  StringRef libSubDir = getPlatformNameForTriple(getTriple());
  if (tripleIsMacCatalystEnvironment(getTriple()))
    libSubDir = "maccatalyst";
  llvm::sys::path::append(resourceDirPath, libSubDir);
}

// Get the secondary runtime library link path given the primary path.
// The compiler will look for runtime libraries in the secondary path if they
// can't be found in the primary path.
void ToolChain::getSecondaryResourceDirPath(
    SmallVectorImpl<char> &secondaryResourceDirPath,
    StringRef primaryPath) const {
  if (!tripleIsMacCatalystEnvironment(getTriple()))
    return;

  // For macCatalyst, the secondary runtime library path is the macOS library
  // path. The compiler will find zippered libraries here.
  secondaryResourceDirPath.append(primaryPath.begin(), primaryPath.end());
  // Remove '/maccatalyst' and replace with 'macosx'.
  llvm::sys::path::remove_filename(secondaryResourceDirPath);
  llvm::sys::path::append(secondaryResourceDirPath, "macosx");
}

void ToolChain::getRuntimeLibraryPaths(SmallVectorImpl<std::string> &runtimeLibPaths,
                                       const llvm::opt::ArgList &args,
                                       StringRef SDKPath, bool shared) const {
  SmallString<128> scratchPath;
  getResourceDirPath(scratchPath, args, shared);
  runtimeLibPaths.push_back(std::string(scratchPath.str()));

  // If there's a secondary resource dir, add it too.
  scratchPath.clear();
  getSecondaryResourceDirPath(scratchPath, runtimeLibPaths[0]);
  if (!scratchPath.empty())
    runtimeLibPaths.push_back(std::string(scratchPath.str()));

  if (!SDKPath.empty()) {
    if (!scratchPath.empty()) {
      // If we added the secondary resource dir, we also need the iOSSupport
      // directory.
      scratchPath = SDKPath;
      llvm::sys::path::append(scratchPath, "System", "iOSSupport");
      llvm::sys::path::append(scratchPath, "usr", "lib", "swift");
      runtimeLibPaths.push_back(std::string(scratchPath.str()));
    }

    scratchPath = SDKPath;
    llvm::sys::path::append(scratchPath, "usr", "lib", "swift");
    runtimeLibPaths.push_back(std::string(scratchPath.str()));
  }
}

const char *ToolChain::getClangLinkerDriver(
    const llvm::opt::ArgList &Args) const {
  // We don't use `clang++` unconditionally because we want to avoid pulling in
  // a C++ standard library if it's not needed, in particular because the
  // standard library that `clang++` selects by default may not be the one that
  // is desired.
  const char *LinkerDriver =
      Args.hasArg(options::OPT_enable_experimental_cxx_interop) ? "clang++"
                                                                : "clang";
  if (const Arg *A = Args.getLastArg(options::OPT_tools_directory)) {
    StringRef toolchainPath(A->getValue());

    // If there is a linker driver in the toolchain folder, use that instead.
    if (auto tool = llvm::sys::findProgramByName(LinkerDriver, {toolchainPath}))
      LinkerDriver = Args.MakeArgString(tool.get());
  }

  return LinkerDriver;
}

bool ToolChain::sanitizerRuntimeLibExists(const ArgList &args,
                                          StringRef sanitizerName,
                                          bool shared) const {
  SmallString<128> sanitizerLibPath;
  getClangLibraryPath(args, sanitizerLibPath);
  llvm::sys::path::append(sanitizerLibPath,
                          sanitizerRuntimeLibName(sanitizerName, shared));
  return llvm::sys::fs::exists(sanitizerLibPath.str());
}
