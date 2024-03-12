//===--- LangOptions.cpp - Language & configuration options ---------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file defines the LangOptions class, which provides various
//  language and configuration flags.
//
//===----------------------------------------------------------------------===//

#include "swift/Basic/LangOptions.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/Basic/Feature.h"
#include "swift/Basic/Platform.h"
#include "swift/Basic/PlaygroundOption.h"
#include "swift/Basic/Range.h"
#include "swift/Config.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/raw_ostream.h"
#include <limits.h>
#include <optional>

using namespace swift;

LangOptions::LangOptions() {
  // Add all promoted language features
#define LANGUAGE_FEATURE(FeatureName, SENumber, Description)                   \
  Features.insert(Feature::FeatureName);
#define UPCOMING_FEATURE(FeatureName, SENumber, Version)
#define EXPERIMENTAL_FEATURE(FeatureName, AvailableInProd)
#include "swift/Basic/Features.def"

  // Special case: remove macro support if the compiler wasn't built with a
  // host Swift.
#if !SWIFT_BUILD_SWIFT_SYNTAX
  Features.removeAll({Feature::Macros, Feature::FreestandingExpressionMacros,
                      Feature::AttachedMacros, Feature::ExtensionMacros});
#endif

  // Note: Introduce default-on language options here.

  // Enable any playground options that are enabled by default.
#define PLAYGROUND_OPTION(OptionName, Description, DefaultOn, HighPerfOn) \
  if (DefaultOn) \
    PlaygroundOptions.insert(PlaygroundOption::OptionName);
#include "swift/Basic/PlaygroundOptions.def"
}

struct SupportedConditionalValue {
  StringRef value;

  /// If the value has been deprecated, the new value to replace it with.
  StringRef replacement = "";

  SupportedConditionalValue(const char *value) : value(value) {}
  SupportedConditionalValue(const char *value, const char *replacement)
    : value(value), replacement(replacement) {}
};

static const SupportedConditionalValue SupportedConditionalCompilationOSs[] = {
  "OSX",
  "macOS",
  "tvOS",
  "watchOS",
  "iOS",
  "Linux",
  "FreeBSD",
  "OpenBSD",
  "Windows",
  "Android",
  "PS4",
  "Cygwin",
  "Haiku",
  "WASI",
  "none",
};

static const SupportedConditionalValue SupportedConditionalCompilationArches[] = {
  "arm",
  "arm64",
  "arm64_32",
  "i386",
  "x86_64",
  "powerpc",
  "powerpc64",
  "powerpc64le",
  "s390x",
  "wasm32",
  "riscv64",
};

static const SupportedConditionalValue SupportedConditionalCompilationEndianness[] = {
  "little",
  "big"
};

static const SupportedConditionalValue SupportedConditionalCompilationPointerBitWidths[] = {
  "_32",
  "_64"
};

static const SupportedConditionalValue SupportedConditionalCompilationRuntimes[] = {
  "_ObjC",
  "_Native",
};

static const SupportedConditionalValue SupportedConditionalCompilationTargetEnvironments[] = {
  "simulator",
  { "macabi", "macCatalyst" },
  "macCatalyst", // A synonym for "macabi" when compiling for iOS
};

static const SupportedConditionalValue SupportedConditionalCompilationPtrAuthSchemes[] = {
  "_none",
  "_arm64e",
};

static const SupportedConditionalValue SupportedConditionalCompilationHasAtomicBitWidths[] = {
  "_8",
  "_16",
  "_32",
  "_64",
  "_128"
};

static const PlatformConditionKind AllPublicPlatformConditionKinds[] = {
#define PLATFORM_CONDITION(LABEL, IDENTIFIER) PlatformConditionKind::LABEL,
#define PLATFORM_CONDITION_(LABEL, IDENTIFIER)
#include "swift/AST/PlatformConditionKinds.def"
};

ArrayRef<SupportedConditionalValue> getSupportedConditionalCompilationValues(const PlatformConditionKind &Kind) {
  switch (Kind) {
  case PlatformConditionKind::OS:
    return SupportedConditionalCompilationOSs;
  case PlatformConditionKind::Arch:
    return SupportedConditionalCompilationArches;
  case PlatformConditionKind::Endianness:
    return SupportedConditionalCompilationEndianness;
  case PlatformConditionKind::PointerBitWidth:
    return SupportedConditionalCompilationPointerBitWidths;
  case PlatformConditionKind::Runtime:
    return SupportedConditionalCompilationRuntimes;
  case PlatformConditionKind::CanImport:
    return { };
  case PlatformConditionKind::TargetEnvironment:
    return SupportedConditionalCompilationTargetEnvironments;
  case PlatformConditionKind::PtrAuth:
    return SupportedConditionalCompilationPtrAuthSchemes;
  case PlatformConditionKind::HasAtomicBitWidth:
    return SupportedConditionalCompilationHasAtomicBitWidths;
  }
  llvm_unreachable("Unhandled PlatformConditionKind in switch");
}

PlatformConditionKind suggestedPlatformConditionKind(PlatformConditionKind Kind, const StringRef &V,
                                                     std::vector<StringRef> &suggestedValues) {
  std::string lower = V.lower();
  for (const PlatformConditionKind& candidateKind : AllPublicPlatformConditionKinds) {
    if (candidateKind != Kind) {
      auto supportedValues = getSupportedConditionalCompilationValues(candidateKind);
      for (const SupportedConditionalValue& candidateValue : supportedValues) {
        if (candidateValue.value.lower() == lower) {
          suggestedValues.clear();
          if (candidateValue.value != V) {
            suggestedValues.emplace_back(candidateValue.value);
          }
          return candidateKind;
        }
      }
    }
  }
  return Kind;
}

bool isMatching(PlatformConditionKind Kind, const StringRef &V,
                PlatformConditionKind &suggestedKind, std::vector<StringRef> &suggestions) {
  // Compare against known values, ignoring case to avoid penalizing
  // characters with incorrect case.
  unsigned minDistance = std::numeric_limits<unsigned>::max();
  std::string lower = V.lower();
  auto supportedValues = getSupportedConditionalCompilationValues(Kind);
  for (const SupportedConditionalValue& candidate : supportedValues) {
    if (candidate.value == V) {
      suggestedKind = Kind;
      suggestions.clear();
      if (!candidate.replacement.empty())
        suggestions.push_back(candidate.replacement);
      return true;
    }
    unsigned distance = StringRef(lower).edit_distance(candidate.value.lower());
    if (distance < minDistance) {
      suggestions.clear();
      minDistance = distance;
    }
    if (distance == minDistance)
      suggestions.emplace_back(candidate.value);
  }
  suggestedKind = suggestedPlatformConditionKind(Kind, V, suggestions);
  return false;
}

bool LangOptions::
checkPlatformConditionSupported(PlatformConditionKind Kind, StringRef Value,
                                PlatformConditionKind &suggestedKind,
                                std::vector<StringRef> &suggestedValues) {
  switch (Kind) {
  case PlatformConditionKind::OS:
  case PlatformConditionKind::Arch:
  case PlatformConditionKind::Endianness:
  case PlatformConditionKind::PointerBitWidth:
  case PlatformConditionKind::Runtime:
  case PlatformConditionKind::TargetEnvironment:
  case PlatformConditionKind::PtrAuth:
  case PlatformConditionKind::HasAtomicBitWidth:
    return isMatching(Kind, Value, suggestedKind, suggestedValues);
  case PlatformConditionKind::CanImport:
    // All importable names are valid.
    // FIXME: Perform some kind of validation of the string?
    return true;
  }
  llvm_unreachable("Unhandled enum value");
}

StringRef
LangOptions::getPlatformConditionValue(PlatformConditionKind Kind) const {
  // Last one wins.
  for (auto &Opt : llvm::reverse(PlatformConditionValues)) {
    if (Opt.first == Kind)
      return Opt.second;
  }
  return StringRef();
}

bool LangOptions::
checkPlatformCondition(PlatformConditionKind Kind, StringRef Value) const {
  // Check a special case that "macOS" is an alias of "OSX".
  if (Kind == PlatformConditionKind::OS && Value == "macOS")
    return checkPlatformCondition(Kind, "OSX");

  // When compiling for iOS we consider "macCatalyst" to be a
  // synonym of "macabi". This enables the use of
  // #if targetEnvironment(macCatalyst) as a compilation
  // condition for macCatalyst.

  if (Kind == PlatformConditionKind::TargetEnvironment &&
      Value == "macCatalyst" && Target.isiOS()) {
    return checkPlatformCondition(Kind, "macabi");
  }

  for (auto &Opt : llvm::reverse(PlatformConditionValues)) {
    if (Opt.first == Kind)
      if (Opt.second == Value)
        return true;
  }

  if (Kind == PlatformConditionKind::HasAtomicBitWidth) {
    for (auto bitWidth : AtomicBitWidths) {
      if (bitWidth == Value) {
        return true;
      }
    }
  }

  return false;
}

bool LangOptions::isCustomConditionalCompilationFlagSet(StringRef Name) const {
  return std::find(CustomConditionalCompilationFlags.begin(),
                   CustomConditionalCompilationFlags.end(), Name)
      != CustomConditionalCompilationFlags.end();
}

bool LangOptions::hasFeature(Feature feature) const {
  if (Features.contains(feature))
    return true;

  if (auto version = getFeatureLanguageVersion(feature))
    return isSwiftVersionAtLeast(*version);

  return false;
}

bool LangOptions::hasFeature(llvm::StringRef featureName) const {
  auto feature = llvm::StringSwitch<std::optional<Feature>>(featureName)
#define LANGUAGE_FEATURE(FeatureName, SENumber, Description)                   \
  .Case(#FeatureName, Feature::FeatureName)
#include "swift/Basic/Features.def"
                     .Default(std::nullopt);
  if (feature)
    return hasFeature(*feature);

  return false;
}

void LangOptions::setHasAtomicBitWidth(llvm::Triple triple) {
  // We really want to use Clang's getMaxAtomicInlineWidth(), but that requires
  // a Clang::TargetInfo and we're setting up lang opts very early in the
  // pipeline before any ASTContext or any ClangImporter instance where we can
  // access the target's info.

  switch (triple.getArch()) {
  // ARM is only a 32 bit arch and all archs besides the microcontroller profile
  // ones have double word atomics.
  case llvm::Triple::ArchType::arm:
  case llvm::Triple::ArchType::thumb:
    switch (triple.getSubArch()) {
    case llvm::Triple::SubArchType::ARMSubArch_v6m:
    case llvm::Triple::SubArchType::ARMSubArch_v7m:
      setMaxAtomicBitWidth(32);
      break;

    default:
      setMaxAtomicBitWidth(64);
      break;
    }
    break;

  // AArch64 (arm64) supports double word atomics on all archs besides the
  // microcontroller profiles.
  case llvm::Triple::ArchType::aarch64:
    switch (triple.getSubArch()) {
    case llvm::Triple::SubArchType::ARMSubArch_v8m_baseline:
    case llvm::Triple::SubArchType::ARMSubArch_v8m_mainline:
    case llvm::Triple::SubArchType::ARMSubArch_v8_1m_mainline:
      setMaxAtomicBitWidth(64);
      break;

    default:
      setMaxAtomicBitWidth(128);
      break;
    }
    break;

  // arm64_32 has 32 bit pointer words, but it has the same architecture as
  // arm64 and supports 128 bit atomics.
  case llvm::Triple::ArchType::aarch64_32:
    setMaxAtomicBitWidth(128);
    break;

  // PowerPC does not support double word atomics.
  case llvm::Triple::ArchType::ppc:
    setMaxAtomicBitWidth(32);
    break;

  // All of the 64 bit PowerPC flavors do not support double word atomics.
  case llvm::Triple::ArchType::ppc64:
  case llvm::Triple::ArchType::ppc64le:
    setMaxAtomicBitWidth(64);
    break;

  // SystemZ (s390x) does not support double word atomics.
  case llvm::Triple::ArchType::systemz:
    setMaxAtomicBitWidth(64);
    break;

  // Wasm32 supports double word atomics.
  case llvm::Triple::ArchType::wasm32:
    setMaxAtomicBitWidth(64);
    break;

  // x86 supports double word atomics.
  //
  // Technically, this is incorrect. However, on all x86 platforms where Swift
  // is deployed this is true.
  case llvm::Triple::ArchType::x86:
    setMaxAtomicBitWidth(64);
    break;

  // x86_64 supports double word atomics.
  //
  // Technically, this is incorrect. However, on all x86_64 platforms where Swift
  // is deployed this is true. If the ClangImporter ever stops unconditionally
  // adding '-mcx16' to its Clang instance, then be sure to update this below.
  case llvm::Triple::ArchType::x86_64:
    setMaxAtomicBitWidth(128);
    break;

  default:
    // Some exotic architectures may not support atomics at all. If that's the
    // case please update the switch with your flavor of arch. Otherwise assume
    // every arch supports at least word atomics.

    if (triple.isArch32Bit()) {
      setMaxAtomicBitWidth(32);
    }

    if (triple.isArch64Bit()) {
      setMaxAtomicBitWidth(64);
    }
  }
}

std::pair<bool, bool> LangOptions::setTarget(llvm::Triple triple) {
  clearAllPlatformConditionValues();
  clearAtomicBitWidths();

  if (triple.getOS() == llvm::Triple::Darwin &&
      triple.getVendor() == llvm::Triple::Apple) {
    // Rewrite darwinX.Y triples to macosx10.X'.Y ones.
    // It affects code generation on our platform.
    llvm::SmallString<16> osxBuf;
    llvm::raw_svector_ostream osx(osxBuf);
    osx << llvm::Triple::getOSTypeName(llvm::Triple::MacOSX);

    llvm::VersionTuple OSVersion;
    triple.getMacOSXVersion(OSVersion);

    osx << OSVersion.getMajor() << "." << OSVersion.getMinor().value_or(0);
    if (auto Subminor = OSVersion.getSubminor())
      osx << "." << *Subminor;

    triple.setOSName(osx.str());
  }
  Target = std::move(triple);

  bool UnsupportedOS = false;

  // Set the "os" platform condition.
  switch (Target.getOS()) {
  case llvm::Triple::Darwin:
  case llvm::Triple::MacOSX:
    addPlatformConditionValue(PlatformConditionKind::OS, "OSX");
    break;
  case llvm::Triple::TvOS:
    addPlatformConditionValue(PlatformConditionKind::OS, "tvOS");
    break;
  case llvm::Triple::WatchOS:
    addPlatformConditionValue(PlatformConditionKind::OS, "watchOS");
    break;
  case llvm::Triple::IOS:
    addPlatformConditionValue(PlatformConditionKind::OS, "iOS");
    break;
  case llvm::Triple::Linux:
    if (Target.getEnvironment() == llvm::Triple::Android)
      addPlatformConditionValue(PlatformConditionKind::OS, "Android");
    else
      addPlatformConditionValue(PlatformConditionKind::OS, "Linux");
    break;
  case llvm::Triple::FreeBSD:
    addPlatformConditionValue(PlatformConditionKind::OS, "FreeBSD");
    break;
  case llvm::Triple::OpenBSD:
    addPlatformConditionValue(PlatformConditionKind::OS, "OpenBSD");
    break;
  case llvm::Triple::Win32:
    if (Target.getEnvironment() == llvm::Triple::Cygnus)
      addPlatformConditionValue(PlatformConditionKind::OS, "Cygwin");
    else
      addPlatformConditionValue(PlatformConditionKind::OS, "Windows");
    break;
  case llvm::Triple::PS4:
    if (Target.getVendor() == llvm::Triple::SCEI)
      addPlatformConditionValue(PlatformConditionKind::OS, "PS4");
    else
      UnsupportedOS = false;
    break;
  case llvm::Triple::Haiku:
    addPlatformConditionValue(PlatformConditionKind::OS, "Haiku");
    break;
  case llvm::Triple::WASI:
    addPlatformConditionValue(PlatformConditionKind::OS, "WASI");
    break;
  case llvm::Triple::UnknownOS:
    if (Target.getOSName() == "none") {
      addPlatformConditionValue(PlatformConditionKind::OS, "none");
      break;
    }
    LLVM_FALLTHROUGH;
  default:
    UnsupportedOS = true;
    break;
  }

  bool UnsupportedArch = false;

  // Set the "arch" platform condition.
  switch (Target.getArch()) {
  case llvm::Triple::ArchType::arm:
  case llvm::Triple::ArchType::thumb:
    addPlatformConditionValue(PlatformConditionKind::Arch, "arm");
    break;
  case llvm::Triple::ArchType::aarch64:
  case llvm::Triple::ArchType::aarch64_32:
    if (Target.getArchName() == "arm64_32") {
      addPlatformConditionValue(PlatformConditionKind::Arch, "arm64_32");
    } else {
      addPlatformConditionValue(PlatformConditionKind::Arch, "arm64");
    }
    break;
  case llvm::Triple::ArchType::ppc:
    addPlatformConditionValue(PlatformConditionKind::Arch, "powerpc");
    break;
  case llvm::Triple::ArchType::ppc64:
    addPlatformConditionValue(PlatformConditionKind::Arch, "powerpc64");
    break;
  case llvm::Triple::ArchType::ppc64le:
    addPlatformConditionValue(PlatformConditionKind::Arch, "powerpc64le");
    break;
  case llvm::Triple::ArchType::x86:
    addPlatformConditionValue(PlatformConditionKind::Arch, "i386");
    break;
  case llvm::Triple::ArchType::x86_64:
    addPlatformConditionValue(PlatformConditionKind::Arch, "x86_64");
    break;
  case llvm::Triple::ArchType::systemz:
    addPlatformConditionValue(PlatformConditionKind::Arch, "s390x");
    break;
  case llvm::Triple::ArchType::wasm32:
    addPlatformConditionValue(PlatformConditionKind::Arch, "wasm32");
    break;
  case llvm::Triple::ArchType::riscv64:
    addPlatformConditionValue(PlatformConditionKind::Arch, "riscv64");
    break;
  default:
    UnsupportedArch = true;

    if (Target.getOSName() == "none") {
      if (Target.getArch() != llvm::Triple::ArchType::UnknownArch) {
        auto ArchName = llvm::Triple::getArchTypeName(Target.getArch());
        addPlatformConditionValue(PlatformConditionKind::Arch, ArchName);
        UnsupportedArch = false;
      }
    }
  }

  if (UnsupportedOS || UnsupportedArch)
    return { UnsupportedOS, UnsupportedArch };

  // Set the "_endian" platform condition.
  if (Target.isLittleEndian()) {
    addPlatformConditionValue(PlatformConditionKind::Endianness, "little");
  } else {
    addPlatformConditionValue(PlatformConditionKind::Endianness, "big");
  }

  // Set the "_pointerBitWidth" platform condition.
  if (Target.isArch32Bit()) {
    addPlatformConditionValue(PlatformConditionKind::PointerBitWidth, "_32");
  } else if (Target.isArch64Bit()) {
    addPlatformConditionValue(PlatformConditionKind::PointerBitWidth, "_64");
  }

  // Set the "runtime" platform condition.
  addPlatformConditionValue(PlatformConditionKind::Runtime,
                            EnableObjCInterop ? "_ObjC" : "_Native");

  // Set the pointer authentication scheme.
  if (Target.getArchName() == "arm64e") {
    addPlatformConditionValue(PlatformConditionKind::PtrAuth, "_arm64e");
  } else {
    addPlatformConditionValue(PlatformConditionKind::PtrAuth, "_none");
  }

  // Set the "targetEnvironment" platform condition if targeting a simulator
  // environment. Otherwise _no_ value is present for targetEnvironment; it's
  // an optional disambiguating refinement of the triple.
  if (Target.isSimulatorEnvironment())
    addPlatformConditionValue(PlatformConditionKind::TargetEnvironment,
                              "simulator");

  if (tripleIsMacCatalystEnvironment(Target))
    addPlatformConditionValue(PlatformConditionKind::TargetEnvironment,
                              "macabi");

  // Set the "_hasHasAtomicBitWidth" platform condition.
  setHasAtomicBitWidth(triple);

  // If you add anything to this list, change the default size of
  // PlatformConditionValues to not require an extra allocation
  // in the common case.

  return { false, false };
}

llvm::StringRef swift::getFeatureName(Feature feature) {
  switch (feature) {
#define LANGUAGE_FEATURE(FeatureName, SENumber, Description)                   \
  case Feature::FeatureName:                                                   \
    return #FeatureName;
#include "swift/Basic/Features.def"
  }
  llvm_unreachable("covered switch");
}

bool swift::isFeatureAvailableInProduction(Feature feature) {
  switch (feature) {
#define LANGUAGE_FEATURE(FeatureName, SENumber, Description)                   \
  case Feature::FeatureName:                                                   \
    return true;
#define EXPERIMENTAL_FEATURE(FeatureName, AvailableInProd) \
  case Feature::FeatureName: return AvailableInProd;
#include "swift/Basic/Features.def"
  }
  llvm_unreachable("covered switch");
}

std::optional<Feature> swift::getUpcomingFeature(llvm::StringRef name) {
  return llvm::StringSwitch<std::optional<Feature>>(name)
#define LANGUAGE_FEATURE(FeatureName, SENumber, Description)
#define UPCOMING_FEATURE(FeatureName, SENumber, Version) \
                   .Case(#FeatureName, Feature::FeatureName)
#include "swift/Basic/Features.def"
      .Default(std::nullopt);
}

std::optional<Feature> swift::getExperimentalFeature(llvm::StringRef name) {
  return llvm::StringSwitch<std::optional<Feature>>(name)
#define LANGUAGE_FEATURE(FeatureName, SENumber, Description)
#define EXPERIMENTAL_FEATURE(FeatureName, AvailableInProd) \
                   .Case(#FeatureName, Feature::FeatureName)
#include "swift/Basic/Features.def"
      .Default(std::nullopt);
}

std::optional<unsigned> swift::getFeatureLanguageVersion(Feature feature) {
  switch (feature) {
#define LANGUAGE_FEATURE(FeatureName, SENumber, Description)
#define UPCOMING_FEATURE(FeatureName, SENumber, Version) \
  case Feature::FeatureName: return Version;
#include "swift/Basic/Features.def"
  default:
    return std::nullopt;
  }
}

bool swift::includeInModuleInterface(Feature feature) {
  switch (feature) {
#define LANGUAGE_FEATURE(FeatureName, SENumber, Description)                   \
  case Feature::FeatureName:                                                   \
    return true;
#define EXPERIMENTAL_FEATURE_EXCLUDED_FROM_MODULE_INTERFACE(FeatureName, AvailableInProd) \
  case Feature::FeatureName: return false;
#include "swift/Basic/Features.def"
  }
  llvm_unreachable("covered switch");
}

llvm::StringRef swift::getPlaygroundOptionName(PlaygroundOption option) {
  switch (option) {
#define PLAYGROUND_OPTION(OptionName, Description, DefaultOn, HighPerfOn) \
  case PlaygroundOption::OptionName: return #OptionName;
#include "swift/Basic/PlaygroundOptions.def"
  }
  llvm_unreachable("covered switch");
}

std::optional<PlaygroundOption>
swift::getPlaygroundOption(llvm::StringRef name) {
  return llvm::StringSwitch<std::optional<PlaygroundOption>>(name)
#define PLAYGROUND_OPTION(OptionName, Description, DefaultOn, HighPerfOn) \
  .Case(#OptionName, PlaygroundOption::OptionName)
#include "swift/Basic/PlaygroundOptions.def"
      .Default(std::nullopt);
}

DiagnosticBehavior LangOptions::getAccessNoteFailureLimit() const {
  switch (AccessNoteBehavior) {
  case AccessNoteDiagnosticBehavior::Ignore:
    return DiagnosticBehavior::Ignore;

  case AccessNoteDiagnosticBehavior::RemarkOnFailure:
  case AccessNoteDiagnosticBehavior::RemarkOnFailureOrSuccess:
    return DiagnosticBehavior::Remark;

  case AccessNoteDiagnosticBehavior::ErrorOnFailureRemarkOnSuccess:
    return DiagnosticBehavior::Error;
  }
  llvm_unreachable("covered switch");
}

namespace {
  constexpr std::array<std::string_view, 16> knownSearchPathPrefiexes =
       {"-I",
        "-F",
        "-fmodule-map-file=",
        "-iquote",
        "-idirafter",
        "-iframeworkwithsysroot",
        "-iframework",
        "-iprefix",
        "-iwithprefixbefore",
        "-iwithprefix",
        "-isystemafter",
        "-isystem",
        "-isysroot",
        "-ivfsoverlay",
        "-working-directory=",
        "-working-directory"};

constexpr std::array<std::string_view, 15> knownClangDependencyIgnorablePrefiexes =
     {"-I",
      "-F",
      "-fmodule-map-file=",
      "-iquote",
      "-idirafter",
      "-iframeworkwithsysroot",
      "-iframework",
      "-iprefix",
      "-iwithprefixbefore",
      "-iwithprefix",
      "-isystemafter",
      "-isystem",
      "-isysroot",
      "-working-directory=",
      "-working-directory"};
}

std::vector<std::string> ClangImporterOptions::getRemappedExtraArgs(
    std::function<std::string(StringRef)> pathRemapCallback) const {
  auto consumeIncludeOption = [](StringRef &arg, StringRef &prefix) {
    for (const auto &option : knownSearchPathPrefiexes)
      if (arg.consume_front(option)) {
        prefix = option;
        return true;
      }
    return false;
  };

  // true if the previous argument was the dash-option of an option pair
  bool remap_next = false;
  std::vector<std::string> args;
  for (auto A : ExtraArgs) {
    StringRef prefix;
    StringRef arg(A);

    if (remap_next) {
      remap_next = false;
      args.push_back(pathRemapCallback(arg));
    } else if (consumeIncludeOption(arg, prefix)) {
      if (arg.empty()) {
        // Option pair
        remap_next = true;
        args.push_back(prefix.str());
      } else {
        // Combine prefix with remapped path value
        args.push_back(prefix.str() + pathRemapCallback(arg));
      }
    } else {
      args.push_back(A);
    }
  }
  return args;
}

std::vector<std::string>
ClangImporterOptions::getReducedExtraArgsForSwiftModuleDependency() const {
  auto matchIncludeOption = [](StringRef &arg) {
    for (const auto &option : knownClangDependencyIgnorablePrefiexes)
      if (arg.consume_front(option))
        return true;
    return false;
  };

  std::vector<std::string> filtered_args;
  bool skip_next = false;
  std::vector<std::string> args;
  for (auto A : ExtraArgs) {
    StringRef arg(A);
    if (skip_next) {
      skip_next = false;
      continue;
    } else if (matchIncludeOption(arg)) {
      if (arg.empty()) {
        // Option pair
        skip_next = true;
      } // else non-pair option e.g. '-I/search/path'
      continue;
    } else {
      filtered_args.push_back(A);
    }
  }

  return filtered_args;
}
