//===--- DependencyScanJSON.cpp -- JSON output for dependencies -----------===//
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
#include "swift/DependencyScan/DependencyScanJSON.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/Defer.h"
#include "swift/DependencyScan/DependencyScanImpl.h"
#include "swift/DependencyScan/StringUtils.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"

using namespace swift;
using namespace swift::dependencies;
using namespace swift::c_string_utils;
using namespace llvm;

namespace {
std::string quote(StringRef unquoted) {
  llvm::SmallString<128> buffer;
  llvm::raw_svector_ostream os(buffer);
  for (const auto ch : unquoted) {
    if (ch == '\\')
      os << '\\';
    if (ch == '"')
      os << '\\';
    os << ch;
  }
  return buffer.str().str();
}
} // namespace

/// Write a single JSON field.
template <typename T>
void writeJSONSingleField(llvm::raw_ostream &out, StringRef fieldName,
                          const T &value, unsigned indentLevel,
                          bool trailingComma, bool nested = false);

/// Write a string value as JSON.
void writeJSONValue(llvm::raw_ostream &out, StringRef value,
                    unsigned indentLevel) {
  out << "\"";
  out << quote(value);
  out << "\"";
}

void writeJSONValue(llvm::raw_ostream &out, swiftscan_string_ref_t value,
                    unsigned indentLevel) {
  out << "\"";
  out << quote(get_C_string(value));
  out << "\"";
}

void writeJSONValue(llvm::raw_ostream &out, swiftscan_string_set_t *value_set,
                    unsigned indentLevel) {
  out << "[\n";

  for (size_t i = 0; i < value_set->count; ++i) {
    out.indent((indentLevel + 1) * 2);

    writeJSONValue(out, value_set->strings[i], indentLevel + 1);

    if (i != value_set->count - 1) {
      out << ",";
    }
    out << "\n";
  }

  out.indent(indentLevel * 2);
  out << "]";
}

void writeEncodedModuleIdJSONValue(llvm::raw_ostream &out,
                                   swiftscan_string_ref_t value,
                                   unsigned indentLevel) {
  out << "{\n";
  static const std::string textualPrefix("swiftTextual");
  static const std::string binaryPrefix("swiftBinary");
  static const std::string placeholderPrefix("swiftPlaceholder");
  static const std::string clangPrefix("clang");
  std::string valueStr = get_C_string(value);
  std::string moduleKind;
  std::string moduleName;
  if (!valueStr.compare(0, textualPrefix.size(), textualPrefix)) {
    moduleKind = "swift";
    moduleName = valueStr.substr(textualPrefix.size() + 1);
  } else if (!valueStr.compare(0, binaryPrefix.size(), binaryPrefix)) {
    // FIXME: rename to be consistent in the clients (swift-driver)
    moduleKind = "swiftPrebuiltExternal";
    moduleName = valueStr.substr(binaryPrefix.size() + 1);
  } else if (!valueStr.compare(0, placeholderPrefix.size(),
                               placeholderPrefix)) {
    moduleKind = "swiftPlaceholder";
    moduleName = valueStr.substr(placeholderPrefix.size() + 1);
  } else {
    moduleKind = "clang";
    moduleName = valueStr.substr(clangPrefix.size() + 1);
  }
  writeJSONSingleField(out, moduleKind, moduleName, indentLevel + 1,
                       /*trailingComma=*/false);
  out.indent(indentLevel * 2);
  out << "}";
}

/// Write a boolean value as JSON.
void writeJSONValue(llvm::raw_ostream &out, bool value, unsigned indentLevel) {
  out.write_escaped(value ? "true" : "false");
}

/// Write a JSON array.
template <typename T>
void writeJSONValue(llvm::raw_ostream &out, ArrayRef<T> values,
                    unsigned indentLevel) {
  out << "[\n";

  for (const auto &value : values) {

    out.indent((indentLevel + 1) * 2);

    writeJSONValue(out, value, indentLevel + 1);

    if (&value != &values.back()) {
      out << ",";
    }
    out << "\n";
  }

  out.indent(indentLevel * 2);
  out << "]";
}

/// Write a JSON array.
template <typename T>
void writeJSONValue(llvm::raw_ostream &out, const std::vector<T> &values,
                    unsigned indentLevel) {
  writeJSONValue(out, llvm::ArrayRef(values), indentLevel);
}

/// Write a single JSON field.
template <typename T>
void writeJSONSingleField(llvm::raw_ostream &out, StringRef fieldName,
                          const T &value, unsigned indentLevel,
                          bool trailingComma, bool nested) {
  out.indent(indentLevel * 2);
  writeJSONValue(out, fieldName, indentLevel);
  out << ": ";
  auto updatedIndentLevel = indentLevel;
  
  if (nested) {
    // This is a hack to "fix" a format for a value that should be a nested
    // set of strings. Currently only capturedPCMArgs (clang) is expected to
    // in the nested format, which supposedly only contains one set of strings.
    // Adjust the indentation to account for the nested brackets.
    updatedIndentLevel += 1;
    out << "[\n";
    out.indent(updatedIndentLevel * 2);
  }

  writeJSONValue(out, value, updatedIndentLevel);

  if (nested) {
    // If nested, add an extra closing brack with a correct indentation.
    out << "\n";
    out.indent(indentLevel * 2);
    out << "]";
  }

  if (trailingComma)
    out << ",";
  out << "\n";
}

static void writeDependencies(llvm::raw_ostream &out,
                              const swiftscan_string_set_t *dependencies,
                              std::string dependenciesKind,
                              unsigned indentLevel, bool trailingComma) {
  out.indent(indentLevel * 2);
  out << "\"" + dependenciesKind + "\": ";
  out << "[\n";

  for (size_t i = 0; i < dependencies->count; ++i) {
    out.indent((indentLevel + 1) * 2);
    writeEncodedModuleIdJSONValue(out, dependencies->strings[i],
                                  indentLevel + 1);
    if (i != dependencies->count - 1) {
      out << ",";
    }
    out << "\n";
  }

  out.indent(indentLevel * 2);
  out << "]";

  if (trailingComma)
    out << ",";
  out << "\n";
}

void writeLinkLibraries(llvm::raw_ostream &out,
                        const swiftscan_link_library_set_t *link_libraries,
                        unsigned indentLevel, bool trailingComma) {
  out.indent(indentLevel * 2);
  out << "\"linkLibraries\": ";
  out << "[\n";

  for (size_t i = 0; i < link_libraries->count; ++i) {
    const auto &llInfo = *link_libraries->link_libraries[i];
    out.indent((indentLevel + 1) * 2);
    out << "{\n";
    auto entryIndentLevel = ((indentLevel + 2) * 2);
    out.indent(entryIndentLevel);
    out << "\"linkName\": ";
    writeJSONValue(out, llInfo.name, indentLevel);
    out << ",\n";
    out.indent(entryIndentLevel);
    out << "\"isFramework\": ";
    writeJSONValue(out, llInfo.isFramework, entryIndentLevel);
    out << ",\n";
    out.indent(entryIndentLevel);
    out << "\"shouldForceLoad\": ";
    writeJSONValue(out, llInfo.forceLoad, entryIndentLevel);
    out << "\n";
    out.indent((indentLevel + 1) * 2);
    out << "}";
    if (i != link_libraries->count - 1) {
      out << ",";
    }
    out << "\n";
  }

  out.indent(indentLevel * 2);
  out << "]";

  if (trailingComma)
    out << ",";
  out << "\n";
}

static const swiftscan_swift_textual_details_t *
getAsTextualDependencyModule(swiftscan_module_details_t details) {
  if (details->kind == SWIFTSCAN_DEPENDENCY_INFO_SWIFT_TEXTUAL)
    return &details->swift_textual_details;
  return nullptr;
}

static const swiftscan_swift_placeholder_details_t *
getAsPlaceholderDependencyModule(swiftscan_module_details_t details) {
  if (details->kind == SWIFTSCAN_DEPENDENCY_INFO_SWIFT_PLACEHOLDER)
    return &details->swift_placeholder_details;
  return nullptr;
}

static const swiftscan_swift_binary_details_t *
getAsBinaryDependencyModule(swiftscan_module_details_t details) {
  if (details->kind == SWIFTSCAN_DEPENDENCY_INFO_SWIFT_BINARY)
    return &details->swift_binary_details;
  return nullptr;
}

static const swiftscan_clang_details_t *
getAsClangDependencyModule(swiftscan_module_details_t details) {
  if (details->kind == SWIFTSCAN_DEPENDENCY_INFO_CLANG)
    return &details->clang_details;
  return nullptr;
}

namespace swift::dependencies {
void writePrescanJSON(llvm::raw_ostream &out,
                      swiftscan_import_set_t importSet) {
  // Write out a JSON containing all main module imports.
  out << "{\n";
  SWIFT_DEFER { out << "}\n"; };

  writeJSONSingleField(out, "imports", importSet->imports, 0, false);
}

void writeJSON(llvm::raw_ostream &out,
               swiftscan_dependency_graph_t fullDependencies) {
  // Write out a JSON description of all of the dependencies.
  out << "{\n";
  SWIFT_DEFER { out << "}\n"; };
  // Name of the main module.
  writeJSONSingleField(out, "mainModuleName",
                       fullDependencies->main_module_name,
                       /*indentLevel=*/1, /*trailingComma=*/true);
  // Write out all of the modules.
  out << "  \"modules\": [\n";
  SWIFT_DEFER { out << "  ]\n"; };
  const auto module_set = fullDependencies->dependencies;
  for (size_t mi = 0; mi < module_set->count; ++mi) {
    const auto &moduleInfo = *module_set->modules[mi];
    auto &directDependencies = moduleInfo.direct_dependencies;
    // The module we are describing.
    out.indent(2 * 2);
    writeEncodedModuleIdJSONValue(out, moduleInfo.module_name, 2);
    out << ",\n";
    out.indent(2 * 2);
    out << "{\n";
    auto swiftPlaceholderDeps =
        getAsPlaceholderDependencyModule(moduleInfo.details);
    auto swiftTextualDeps = getAsTextualDependencyModule(moduleInfo.details);
    auto swiftBinaryDeps = getAsBinaryDependencyModule(moduleInfo.details);
    auto clangDeps = getAsClangDependencyModule(moduleInfo.details);

    // Module path.
    const char *modulePathSuffix = clangDeps ? ".pcm" : ".swiftmodule";

    std::string modulePath;
    std::string moduleKindAndName =
        std::string(get_C_string(moduleInfo.module_name));
    std::string moduleName =
        moduleKindAndName.substr(moduleKindAndName.find(":") + 1);
    if (swiftPlaceholderDeps)
      modulePath = get_C_string(swiftPlaceholderDeps->compiled_module_path);
    else if (swiftBinaryDeps)
      modulePath = get_C_string(swiftBinaryDeps->compiled_module_path);
    else if (clangDeps || swiftTextualDeps)
      modulePath = get_C_string(moduleInfo.module_path);
    else
      modulePath = moduleName + modulePathSuffix;

    writeJSONSingleField(out, "modulePath", modulePath, /*indentLevel=*/3,
                         /*trailingComma=*/true);

    // Source files.
    if (swiftTextualDeps || clangDeps) {
      writeJSONSingleField(out, "sourceFiles", moduleInfo.source_files, 3,
                           /*trailingComma=*/true);
    }

    // Direct dependencies.
    if (swiftTextualDeps || swiftBinaryDeps || clangDeps) {
      writeDependencies(out, directDependencies,
                        "directDependencies", 3,
                        /*trailingComma=*/true);
      writeLinkLibraries(out, moduleInfo.link_libraries,
                         3, /*trailingComma=*/true);
    }
    // Swift and Clang-specific details.
    out.indent(3 * 2);
    out << "\"details\": {\n";
    out.indent(4 * 2);
    if (swiftTextualDeps) {
      out << "\"swift\": {\n";
      /// Swift interface file, if there is one. The main module, for
      /// example, will not have an interface file.
      std::string moduleInterfacePath =
          swiftTextualDeps->module_interface_path.data
              ? get_C_string(swiftTextualDeps->module_interface_path)
              : "";
      if (!moduleInterfacePath.empty()) {
        writeJSONSingleField(out, "moduleInterfacePath", moduleInterfacePath, 5,
                             /*trailingComma=*/true);
        out.indent(5 * 2);
        out << "\"compiledModuleCandidates\": [\n";
        for (int i = 0,
                 count = swiftTextualDeps->compiled_module_candidates->count;
             i < count; ++i) {
          const auto &candidate = get_C_string(
              swiftTextualDeps->compiled_module_candidates->strings[i]);
          out.indent(6 * 2);
          out << "\"" << quote(candidate) << "\"";
          if (i != count - 1)
            out << ",";
          out << "\n";
        }
        out.indent(5 * 2);
        out << "],\n";
      }
      out.indent(5 * 2);
      out << "\"commandLine\": [\n";
      for (int i = 0, count = swiftTextualDeps->command_line->count; i < count;
           ++i) {
        const auto &arg =
            get_C_string(swiftTextualDeps->command_line->strings[i]);
        out.indent(6 * 2);
        out << "\"" << quote(arg) << "\"";
        if (i != count - 1)
          out << ",";
        out << "\n";
      }
      out.indent(5 * 2);
      out << "],\n";
      writeJSONSingleField(out, "contextHash", swiftTextualDeps->context_hash,
                           5,
                           /*trailingComma=*/true);
      bool hasBridgingHeaderPath =
          swiftTextualDeps->bridging_header_path.data &&
          get_C_string(swiftTextualDeps->bridging_header_path)[0] != '\0';
      bool hasOverlayDependencies =
          swiftTextualDeps->swift_overlay_module_dependencies &&
          swiftTextualDeps->swift_overlay_module_dependencies->count > 0;
      bool commaAfterBridgingHeaderPath = hasOverlayDependencies;
      bool commaAfterExtraPcmArgs =
          hasBridgingHeaderPath || commaAfterBridgingHeaderPath;
      bool commaAfterFramework =
          swiftTextualDeps->extra_pcm_args->count != 0 || commaAfterExtraPcmArgs;

      if (swiftTextualDeps->cas_fs_root_id.length != 0) {
        writeJSONSingleField(out, "casFSRootID",
                             swiftTextualDeps->cas_fs_root_id, 5,
                             /*trailingComma=*/true);
      }
      if (swiftTextualDeps->module_cache_key.length != 0) {
        writeJSONSingleField(out, "moduleCacheKey",
                             swiftTextualDeps->module_cache_key, 5,
                             /*trailingComma=*/true);
      }
      writeJSONSingleField(out, "isFramework", swiftTextualDeps->is_framework,
                           5, commaAfterFramework);
      if (swiftTextualDeps->extra_pcm_args->count != 0) {
        out.indent(5 * 2);
        out << "\"extraPcmArgs\": [\n";
        for (int i = 0, count = swiftTextualDeps->extra_pcm_args->count;
             i < count; ++i) {
          const auto &arg =
              get_C_string(swiftTextualDeps->extra_pcm_args->strings[i]);
          out.indent(6 * 2);
          out << "\"" << quote(arg) << "\"";
          if (i != count - 1)
            out << ",";
          out << "\n";
        }
        out.indent(5 * 2);
        out << (commaAfterExtraPcmArgs ? "],\n" : "]\n");
      }
      /// Bridging header and its source file dependencies, if any.
      if (hasBridgingHeaderPath) {
        out.indent(5 * 2);
        out << "\"bridgingHeader\": {\n";
        writeJSONSingleField(out, "path",
                             swiftTextualDeps->bridging_header_path, 6,
                             /*trailingComma=*/true);
        writeJSONSingleField(out, "sourceFiles",
                             swiftTextualDeps->bridging_source_files, 6,
                             /*trailingComma=*/true);
        if (swiftTextualDeps->bridging_header_include_tree.length != 0) {
          writeJSONSingleField(out, "includeTree",
                               swiftTextualDeps->bridging_header_include_tree,
                               6, /*trailingComma=*/true);
        }
        writeJSONSingleField(out, "moduleDependencies",
                             swiftTextualDeps->bridging_module_dependencies, 6,
                             /*trailingComma=*/true);
        out.indent(6 * 2);
        out << "\"commandLine\": [\n";
        for (int i = 0,
                 count = swiftTextualDeps->bridging_pch_command_line->count;
             i < count; ++i) {
          const auto &arg = get_C_string(
              swiftTextualDeps->bridging_pch_command_line->strings[i]);
          out.indent(7 * 2);
          out << "\"" << quote(arg) << "\"";
          if (i != count - 1)
            out << ",";
          out << "\n";
        }
        out.indent(6 * 2);
        out << "]\n";
        out.indent(5 * 2);
        out << (commaAfterBridgingHeaderPath ? "},\n" : "}\n");
      }
      if (hasOverlayDependencies) {
        writeDependencies(out, swiftTextualDeps->swift_overlay_module_dependencies,
                          "swiftOverlayDependencies", 5,
                          /*trailingComma=*/false);
      }
    } else if (swiftPlaceholderDeps) {
      out << "\"swiftPlaceholder\": {\n";

      // Module doc file
      if (swiftPlaceholderDeps->module_doc_path.data &&
          get_C_string(swiftPlaceholderDeps->module_doc_path)[0] != '\0')
        writeJSONSingleField(out, "moduleDocPath",
                             swiftPlaceholderDeps->module_doc_path,
                             /*indentLevel=*/5,
                             /*trailingComma=*/true);

      // Module Source Info file
      if (swiftPlaceholderDeps->module_source_info_path.data &&
          get_C_string(swiftPlaceholderDeps->module_source_info_path)[0] !=
              '\0')
        writeJSONSingleField(out, "moduleSourceInfoPath",
                             swiftPlaceholderDeps->module_source_info_path,
                             /*indentLevel=*/5,
                             /*trailingComma=*/false);
    } else if (swiftBinaryDeps) {
      bool hasOverlayDependencies =
        swiftBinaryDeps->swift_overlay_module_dependencies &&
        swiftBinaryDeps->swift_overlay_module_dependencies->count > 0;

      out << "\"swiftPrebuiltExternal\": {\n";
      assert(swiftBinaryDeps->compiled_module_path.data &&
             get_C_string(swiftBinaryDeps->compiled_module_path)[0] != '\0' &&
             "Expected .swiftmodule for a Binary Swift Module Dependency.");

      writeJSONSingleField(out, "compiledModulePath",
                           swiftBinaryDeps->compiled_module_path,
                           /*indentLevel=*/5,
                           /*trailingComma=*/true);
      // Module doc file
      if (swiftBinaryDeps->module_doc_path.data &&
          get_C_string(swiftBinaryDeps->module_doc_path)[0] != '\0')
        writeJSONSingleField(out, "moduleDocPath",
                             swiftBinaryDeps->module_doc_path,
                             /*indentLevel=*/5,
                             /*trailingComma=*/true);
      // Module Source Info file
      if (swiftBinaryDeps->module_source_info_path.data &&
          get_C_string(swiftBinaryDeps->module_source_info_path)[0] != '\0')
        writeJSONSingleField(out, "moduleSourceInfoPath",
                             swiftBinaryDeps->module_source_info_path,
                             /*indentLevel=*/5,
                             /*trailingComma=*/true);
      if (swiftBinaryDeps->module_cache_key.length != 0) {
        writeJSONSingleField(out, "moduleCacheKey",
                             swiftBinaryDeps->module_cache_key, 5,
                             /*trailingComma=*/true);
      }

      // Module Header Dependencies
      if (swiftBinaryDeps->header_dependency.length != 0)
        writeJSONSingleField(out, "headerDependency",
                             swiftBinaryDeps->header_dependency, 5,
                             /*trailingComma=*/true);

      // Module Header Module Dependencies
      if (swiftBinaryDeps->header_dependencies_module_dependnecies->count != 0)
        writeJSONSingleField(out, "headerModuleDependencies",
                             swiftBinaryDeps->header_dependencies_module_dependnecies, 5,
                             /*trailingComma=*/true);

      // Module Header Source Files
      if (swiftBinaryDeps->header_dependencies_source_files->count != 0)
        writeJSONSingleField(out, "headerDependenciesSourceFiles",
                             swiftBinaryDeps->header_dependencies_source_files, 5,
                             /*trailingComma=*/true);

      if (hasOverlayDependencies) {
        writeDependencies(out, swiftBinaryDeps->swift_overlay_module_dependencies,
                          "swiftOverlayDependencies", 5,
                          /*trailingComma=*/true);
      }

      writeJSONSingleField(out, "isFramework", swiftBinaryDeps->is_framework,
                           5, /*trailingComma=*/false);
    } else {
      out << "\"clang\": {\n";

      // Module map file.
      writeJSONSingleField(out, "moduleMapPath", clangDeps->module_map_path, 5,
                           /*trailingComma=*/true);

      // Context hash.
      writeJSONSingleField(out, "contextHash", clangDeps->context_hash, 5,
                           /*trailingComma=*/true);

      // Command line.
      writeJSONSingleField(out, "commandLine", clangDeps->command_line, 5,
                           /*trailingComma=*/true);

      if (clangDeps->cas_fs_root_id.length != 0)
        writeJSONSingleField(out, "casFSRootID", clangDeps->cas_fs_root_id, 5,
                             /*trailingComma=*/true);
      if (clangDeps->clang_include_tree.length != 0)
        writeJSONSingleField(out, "clangIncludeTree",
                             clangDeps->clang_include_tree, 5,
                             /*trailingComma=*/true);
      if (clangDeps->module_cache_key.length != 0)
        writeJSONSingleField(out, "moduleCacheKey", clangDeps->module_cache_key,
                             5,
                             /*trailingComma=*/true);

      // Captured PCM arguments.
      writeJSONSingleField(out, "capturedPCMArgs", clangDeps->captured_pcm_args, 5,
                           /*trailingComma=*/false, /*nested=*/true);

    }

    out.indent(4 * 2);
    out << "}\n";
    out.indent(3 * 2);
    out << "}\n";
    out.indent(2 * 2);
    out << "}";

    if (mi != module_set->count - 1)
      out << ",";
    out << "\n";
  }
}
} // namespace swift::dependencies
