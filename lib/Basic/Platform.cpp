//===--- Platform.cpp - Implement platform-related helpers ----------------===//
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

#include "swift/Basic/Platform.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Support/VersionTuple.h"

using namespace swift;

bool swift::tripleIsiOSSimulator(const llvm::Triple &triple) {
  return (triple.isiOS() &&
          !tripleIsMacCatalystEnvironment(triple) &&
          triple.isSimulatorEnvironment());
}

bool swift::tripleIsAppleTVSimulator(const llvm::Triple &triple) {
  return (triple.isTvOS() && triple.isSimulatorEnvironment());
}

bool swift::tripleIsWatchSimulator(const llvm::Triple &triple) {
  return (triple.isWatchOS() && triple.isSimulatorEnvironment());
}

bool swift::tripleIsMacCatalystEnvironment(const llvm::Triple &triple) {
  return triple.isiOS() && !triple.isTvOS() &&
      triple.getEnvironment() == llvm::Triple::MacABI;
}

bool swift::tripleInfersSimulatorEnvironment(const llvm::Triple &triple) {
  switch (triple.getOS()) {
  case llvm::Triple::IOS:
  case llvm::Triple::TvOS:
  case llvm::Triple::WatchOS:
    return !triple.hasEnvironment() &&
        (triple.getArch() == llvm::Triple::x86 ||
         triple.getArch() == llvm::Triple::x86_64) &&
        !tripleIsMacCatalystEnvironment(triple);

  default:
    return false;
  }
}

bool swift::triplesAreValidForZippering(const llvm::Triple &target,
                                        const llvm::Triple &targetVariant) {
  // The arch and vendor must match.
  if (target.getArchName() != targetVariant.getArchName() ||
      target.getArch() != targetVariant.getArch() ||
      target.getSubArch() != targetVariant.getSubArch() ||
      target.getVendor() != targetVariant.getVendor()) {
    return false;
  }

  // Allow a macOS target and an iOS-macabi target variant
  // This is typically the case when zippering a library originally
  // developed for macOS.
  if (target.isMacOSX() && tripleIsMacCatalystEnvironment(targetVariant)) {
    return true;
  }

  // Allow an iOS-macabi target and a macOS target variant. This would
  // be the case when zippering a library originally developed for
  // iOS.
  if (targetVariant.isMacOSX() && tripleIsMacCatalystEnvironment(target)) {
    return true;
  }

  return false;
}

const llvm::Optional<llvm::VersionTuple>
swift::minimumAvailableOSVersionForTriple(const llvm::Triple &triple) {
  if (triple.isMacOSX())
    return llvm::VersionTuple(10, 10, 0);

  // Mac Catalyst was introduced with an iOS deployment target of 13.1.
  if (tripleIsMacCatalystEnvironment(triple))
    return llvm::VersionTuple(13, 1);
  
  // Note: this must come before checking iOS since that returns true for
  // both iOS and tvOS.
  if (triple.isTvOS())
    return llvm::VersionTuple(9, 0);

  if (triple.isiOS())
    return llvm::VersionTuple(8, 0);

  if (triple.isWatchOS())
    return llvm::VersionTuple(2, 0);

  return llvm::None;
}

bool swift::tripleRequiresRPathForSwiftLibrariesInOS(
    const llvm::Triple &triple) {
  if (triple.isMacOSX()) {
    // macOS versions before 10.14.4 don't have Swift in the OS
    // (the linker still uses an rpath-based install name until 10.15).
    // macOS versions before 12.0 don't have _Concurrency in the OS.
    return triple.isMacOSXVersionLT(12, 0);
  }

  if (triple.isiOS()) {
    // iOS versions before 12.2 don't have Swift in the OS.
    // iOS versions before 15.0 don't have _Concurrency in the OS.
    return triple.isOSVersionLT(15, 0);
  }

  if (triple.isWatchOS()) {
    // watchOS versions before 5.2 don't have Swift in the OS.
    // watchOS versions before 8.0 don't have _Concurrency in the OS.
    return triple.isOSVersionLT(8, 0);
  }

  // Other platforms don't have Swift installed as part of the OS by default.
  return false;
}

DarwinPlatformKind swift::getDarwinPlatformKind(const llvm::Triple &triple) {
  if (triple.isiOS()) {
    if (triple.isTvOS()) {
      if (tripleIsAppleTVSimulator(triple))
        return DarwinPlatformKind::TvOSSimulator;
      return DarwinPlatformKind::TvOS;
    }

    if (tripleIsiOSSimulator(triple))
      return DarwinPlatformKind::IPhoneOSSimulator;

    return DarwinPlatformKind::IPhoneOS;
  }

  if (triple.isWatchOS()) {
    if (tripleIsWatchSimulator(triple))
      return DarwinPlatformKind::WatchOSSimulator;
    return DarwinPlatformKind::WatchOS;
  }

  if (triple.isMacOSX())
    return DarwinPlatformKind::MacOS;

  llvm_unreachable("Unsupported Darwin platform");
}

static StringRef getPlatformNameForDarwin(const DarwinPlatformKind platform) {
  switch (platform) {
  case DarwinPlatformKind::MacOS:
    return "macosx";
  case DarwinPlatformKind::IPhoneOS:
    return "iphoneos";
  case DarwinPlatformKind::IPhoneOSSimulator:
    return "iphonesimulator";
  case DarwinPlatformKind::TvOS:
    return "appletvos";
  case DarwinPlatformKind::TvOSSimulator:
    return "appletvsimulator";
  case DarwinPlatformKind::WatchOS:
    return "watchos";
  case DarwinPlatformKind::WatchOSSimulator:
    return "watchsimulator";
  }
  llvm_unreachable("Unsupported Darwin platform");
}

StringRef swift::getPlatformNameForTriple(const llvm::Triple &triple) {
  switch (triple.getOS()) {
  case llvm::Triple::ZOS:
  case llvm::Triple::Ananas:
  case llvm::Triple::CloudABI:
  case llvm::Triple::DragonFly:
  case llvm::Triple::DriverKit:
  case llvm::Triple::Emscripten:
  case llvm::Triple::Fuchsia:
  case llvm::Triple::KFreeBSD:
  case llvm::Triple::Lv2:
  case llvm::Triple::NetBSD:
  case llvm::Triple::PS5:
  case llvm::Triple::ShaderModel:
  case llvm::Triple::Solaris:
  case llvm::Triple::Minix:
  case llvm::Triple::RTEMS:
  case llvm::Triple::NaCl:
  case llvm::Triple::AIX:
  case llvm::Triple::CUDA:
  case llvm::Triple::NVCL:
  case llvm::Triple::AMDHSA:
  case llvm::Triple::ELFIAMCU:
  case llvm::Triple::Mesa3D:
  case llvm::Triple::Contiki:
  case llvm::Triple::AMDPAL:
  case llvm::Triple::HermitCore:
  case llvm::Triple::Hurd:
    return "";
  case llvm::Triple::Darwin:
  case llvm::Triple::MacOSX:
  case llvm::Triple::IOS:
  case llvm::Triple::TvOS:
  case llvm::Triple::WatchOS:
    return getPlatformNameForDarwin(getDarwinPlatformKind(triple));
  case llvm::Triple::Linux:
    return triple.isAndroid() ? "android" : "linux";
  case llvm::Triple::FreeBSD:
    return "freebsd";
  case llvm::Triple::OpenBSD:
    return "openbsd";
  case llvm::Triple::Win32:
    switch (triple.getEnvironment()) {
    case llvm::Triple::Cygnus:
      return "cygwin";
    case llvm::Triple::GNU:
      return "mingw";
    case llvm::Triple::MSVC:
    case llvm::Triple::Itanium:
      return "windows";
    default:
      llvm_unreachable("unsupported Windows environment");
    }
  case llvm::Triple::PS4:
    return "ps4";
  case llvm::Triple::Haiku:
    return "haiku";
  case llvm::Triple::WASI:
    return "wasi";
  case llvm::Triple::UnknownOS:
    return "none";
  }
  llvm_unreachable("unsupported OS");
}

StringRef swift::getMajorArchitectureName(const llvm::Triple &Triple) {
  if (Triple.isOSLinux()) {
    switch (Triple.getSubArch()) {
    case llvm::Triple::SubArchType::ARMSubArch_v7:
      return "armv7";
    case llvm::Triple::SubArchType::ARMSubArch_v6:
      return "armv6";
    case llvm::Triple::SubArchType::ARMSubArch_v5:
      return "armv5";
    default:
      break;
    }
  }
  return Triple.getArchName();
}

// The code below is responsible for normalizing target triples into the form
// used to name target-specific swiftmodule, swiftinterface, and swiftdoc files.
// If two triples have incompatible ABIs or can be distinguished by Swift #if
// declarations, they should normalize to different values.
//
// This code is only really used on platforms with toolchains supporting fat
// binaries (a single binary containing multiple architectures). On these
// platforms, this code should strip unnecessary details from target triple
// components and map synonyms to canonical values. Even values which don't need
// any special canonicalization should be documented here as comments.
//
// (Fallback behavior does not belong here; it should be implemented in code
// that calls this function, most importantly in SerializedModuleLoaderBase.)
//
// If you're trying to refer to this code to understand how Swift behaves and
// you're unfamiliar with LLVM internals, here's a cheat sheet for reading it:
//
// * llvm::Triple is the type for a target name. It's a bit of a misnomer,
//   because it can contain three or four values: arch-vendor-os[-environment].
//
// * In .Cases and .Case, the last argument is the value the arguments before it
//   map to. That is, `.Cases("bar", "baz", "foo")` will return "foo" if it sees
//   "bar" or "baz".
//
// * llvm::Optional is similar to a Swift Optional: it either contains a value
//   or represents the absence of one. `None` is equivalent to `nil`; leading
//   `*` is equivalent to trailing `!`; conversion to `bool` is a not-`None`
//   check.

static StringRef
getArchForAppleTargetSpecificModuleTriple(const llvm::Triple &triple) {
  auto tripleArchName = triple.getArchName();

  return llvm::StringSwitch<StringRef>(tripleArchName)
              .Cases("arm64", "aarch64", "arm64")
              .Cases("arm64_32", "aarch64_32", "arm64_32")
              .Cases("x86_64", "amd64", "x86_64")
              .Cases("i386", "i486", "i586", "i686", "i786", "i886", "i986",
                     "i386")
              .Cases("unknown", "", "unknown")
  // These values are also supported, but are handled by the default case below:
  //          .Case ("armv7s", "armv7s")
  //          .Case ("armv7k", "armv7k")
  //          .Case ("armv7", "armv7")
  //          .Case ("arm64e", "arm64e")
              .Default(tripleArchName);
}

static StringRef
getVendorForAppleTargetSpecificModuleTriple(const llvm::Triple &triple) {
  // We unconditionally normalize to "apple" because it's relatively common for
  // build systems to omit the vendor name or use an incorrect one like
  // "unknown". Most parts of the compiler ignore the vendor, so you might not
  // notice such a mistake.
  //
  // Please don't depend on this behavior--specify 'apple' if you're building
  // for an Apple platform.

  assert(triple.isOSDarwin() &&
         "shouldn't normalize non-Darwin triple to 'apple'");

  return "apple";
}

static StringRef
getOSForAppleTargetSpecificModuleTriple(const llvm::Triple &triple) {
  auto tripleOSName = triple.getOSName();

  // Truncate the OS name before the first digit. "Digit" here is ASCII '0'-'9'.
  auto tripleOSNameNoVersion = tripleOSName.take_until(llvm::isDigit);

  return llvm::StringSwitch<StringRef>(tripleOSNameNoVersion)
              .Cases("macos", "macosx", "darwin", "macos")
              .Cases("unknown", "", "unknown")
  // These values are also supported, but are handled by the default case below:
  //          .Case ("ios", "ios")
  //          .Case ("tvos", "tvos")
  //          .Case ("watchos", "watchos")
              .Default(tripleOSNameNoVersion);
}

static llvm::Optional<StringRef>
getEnvironmentForAppleTargetSpecificModuleTriple(const llvm::Triple &triple) {
  auto tripleEnvironment = triple.getEnvironmentName();
  return llvm::StringSwitch<llvm::Optional<StringRef>>(tripleEnvironment)
      .Cases("unknown", "", llvm::None)
      // These values are also supported, but are handled by the default case
      // below:
      //          .Case ("simulator", StringRef("simulator"))
      //          .Case ("macabi", StringRef("macabi"))
      .Default(tripleEnvironment);
}

llvm::Triple swift::getTargetSpecificModuleTriple(const llvm::Triple &triple) {
  // isOSDarwin() returns true for all Darwin-style OSes, including macOS, iOS,
  // etc.
  if (triple.isOSDarwin()) {
    StringRef newArch = getArchForAppleTargetSpecificModuleTriple(triple);

    StringRef newVendor = getVendorForAppleTargetSpecificModuleTriple(triple);

    StringRef newOS = getOSForAppleTargetSpecificModuleTriple(triple);

    llvm::Optional<StringRef> newEnvironment =
        getEnvironmentForAppleTargetSpecificModuleTriple(triple);

    if (!newEnvironment)
      // Generate an arch-vendor-os triple.
      return llvm::Triple(newArch, newVendor, newOS);

    // Generate an arch-vendor-os-environment triple.
    return llvm::Triple(newArch, newVendor, newOS, *newEnvironment);
  }

  // android - drop the API level.  That is not pertinent to the module; the API
  // availability is handled by the clang importer.
  if (triple.isAndroid()) {
    StringRef environment =
        llvm::Triple::getEnvironmentTypeName(triple.getEnvironment());

    return llvm::Triple(triple.getArchName(), triple.getVendorName(),
                        triple.getOSName(), environment);
  }

  // Other platforms get no normalization.
  return triple;
}

llvm::Triple swift::getUnversionedTriple(const llvm::Triple &triple) {
  StringRef unversionedOSName = triple.getOSName().take_until(llvm::isDigit);
  if (triple.getEnvironment()) {
    StringRef environment =
        llvm::Triple::getEnvironmentTypeName(triple.getEnvironment());

    return llvm::Triple(triple.getArchName(), triple.getVendorName(),
                        unversionedOSName, environment);
  }

  return llvm::Triple(triple.getArchName(), triple.getVendorName(),
                      unversionedOSName);
}

llvm::Optional<llvm::VersionTuple>
swift::getSwiftRuntimeCompatibilityVersionForTarget(
    const llvm::Triple &Triple) {
#define MAX(a, b) ((a) > (b) ? (a) : (b))

  if (Triple.isMacOSX()) {
    llvm::VersionTuple OSVersion;
    Triple.getMacOSXVersion(OSVersion);
    unsigned Major = OSVersion.getMajor();
    unsigned Minor = OSVersion.getMinor().value_or(0);

    auto floorFor64 = [&Triple](llvm::VersionTuple v) {
      if (!Triple.isAArch64()) return v;
      // macOS got first arm64(e) support in 11.0, i.e. VersionTuple(5, 3)
      return MAX(v, llvm::VersionTuple(5, 3));
    };

    if (Major == 10) {
      if (Triple.isAArch64() && Minor <= 16)
        return floorFor64(llvm::VersionTuple(5, 3));

      if (Minor <= 14) {
        return floorFor64(llvm::VersionTuple(5, 0));
      } else if (Minor <= 15) {
        if (OSVersion.getSubminor().value_or(0) <= 3) {
          return floorFor64(llvm::VersionTuple(5, 1));
        } else {
          return floorFor64(llvm::VersionTuple(5, 2));
        }
      }
    } else if (Major == 11) {
      if (Minor <= 2)
        return floorFor64(llvm::VersionTuple(5, 3));
      return floorFor64(llvm::VersionTuple(5, 4));
    } else if (Major == 12) {
      if (Minor <= 2)
        return floorFor64(llvm::VersionTuple(5, 5));
      return floorFor64(llvm::VersionTuple(5, 6));
    } else if (Major == 13) {
      return floorFor64(llvm::VersionTuple(5, 7));
    }
  } else if (Triple.isiOS()) { // includes tvOS
    llvm::VersionTuple OSVersion = Triple.getiOSVersion();
    unsigned Major = OSVersion.getMajor();
    unsigned Minor = OSVersion.getMinor().value_or(0);

    auto floorForArchitecture = [&Triple, Major](llvm::VersionTuple v) {
      // arm64 simulators and macCatalyst are introduced in iOS 14.0/tvOS 14.0
      // with Swift 5.3
      if (Triple.isAArch64() && Major <= 14 &&
          (Triple.isSimulatorEnvironment() ||
           Triple.isMacCatalystEnvironment()))
        return MAX(v, llvm::VersionTuple(5, 3));

      if (Triple.getArchName() != "arm64e") return v;

      // iOS got first arm64e support in 12.0, which has a Swift runtime version
      // older than 5.0, so let's floor at VersionTuple(5, 0) instead.
      return MAX(v, llvm::VersionTuple(5, 0));
    };

    if (Major <= 12) {
      return floorForArchitecture(llvm::VersionTuple(5, 0));
    } else if (Major <= 13) {
      if (Minor <= 3) {
        return floorForArchitecture(llvm::VersionTuple(5, 1));
      } else {
        return floorForArchitecture(llvm::VersionTuple(5, 2));
      }
    } else if (Major <= 14) {
      if (Minor <= 4)
        return floorForArchitecture(llvm::VersionTuple(5, 3));

      return floorForArchitecture(llvm::VersionTuple(5, 4));
    } else if (Major <= 15) {
      if (Minor <= 3)
        return floorForArchitecture(llvm::VersionTuple(5, 5));
      return floorForArchitecture(llvm::VersionTuple(5, 6));
    } else if (Major <= 16) {
      return floorForArchitecture(llvm::VersionTuple(5, 7));
    }
  } else if (Triple.isWatchOS()) {
    llvm::VersionTuple OSVersion = Triple.getWatchOSVersion();
    unsigned Major = OSVersion.getMajor();
    unsigned Minor = OSVersion.getMinor().value_or(0);

    auto floorFor64bits = [&Triple](llvm::VersionTuple v) {
      if (!Triple.isArch64Bit()) return v;
      // 64-bit watchOS was introduced with Swift 5.3
      return MAX(v, llvm::VersionTuple(5, 3));
    };

    if (Major <= 5) {
      return floorFor64bits(llvm::VersionTuple(5, 0));
    } else if (Major <= 6) {
      if (Minor <= 1) {
        return floorFor64bits(llvm::VersionTuple(5, 1));
      } else {
        return floorFor64bits(llvm::VersionTuple(5, 2));
      }
    } else if (Major <= 7) {
      if (Minor <= 3)
        return floorFor64bits(llvm::VersionTuple(5, 3));

      return floorFor64bits(llvm::VersionTuple(5, 4));
    } else if (Major <= 8) {
      if (Minor <= 4)
        return floorFor64bits(llvm::VersionTuple(5, 5));
      return floorFor64bits(llvm::VersionTuple(5, 6));
    } else if (Major <= 9) {
      return floorFor64bits(llvm::VersionTuple(5, 7));
    }
  }

  return llvm::None;
}

static const llvm::VersionTuple minimumMacCatalystDeploymentTarget() {
  return llvm::VersionTuple(13, 1);
}

llvm::VersionTuple swift::getTargetSDKVersion(clang::DarwinSDKInfo &SDKInfo,
                                              const llvm::Triple &triple) {
  // Retrieve the SDK version.
  auto SDKVersion = SDKInfo.getVersion();

  // For the Mac Catalyst environment, we have a macOS SDK with a macOS
  // SDK version. Map that to the corresponding iOS version number to pass
  // down to the linker.
  if (tripleIsMacCatalystEnvironment(triple)) {
    if (const auto *MacOStoMacCatalystMapping = SDKInfo.getVersionMapping(
            clang::DarwinSDKInfo::OSEnvPair::macOStoMacCatalystPair())) {
      return MacOStoMacCatalystMapping
          ->map(SDKVersion, minimumMacCatalystDeploymentTarget(), llvm::None)
          .value_or(llvm::VersionTuple(0, 0, 0));
    }
    return llvm::VersionTuple(0, 0, 0);
  }

  return SDKVersion;
}

static std::string getPlistEntry(const llvm::Twine &Path, StringRef KeyName) {
  auto BufOrErr = llvm::MemoryBuffer::getFile(Path);
  if (!BufOrErr) {
    // FIXME: diagnose properly
    return {};
  }

  std::string Key = "<key>";
  Key += KeyName;
  Key += "</key>";

  StringRef Lines = BufOrErr.get()->getBuffer();
  while (!Lines.empty()) {
    StringRef CurLine;
    std::tie(CurLine, Lines) = Lines.split('\n');
    if (CurLine.find(Key) != StringRef::npos) {
      std::tie(CurLine, Lines) = Lines.split('\n');
      unsigned Begin = CurLine.find("<string>") + strlen("<string>");
      unsigned End = CurLine.find("</string>");
      return CurLine.substr(Begin, End - Begin).str();
    }
  }

  return {};
}

std::string swift::getSDKBuildVersionFromPlist(StringRef Path) {
  return getPlistEntry(Path, "ProductBuildVersion");
}

std::string swift::getSDKBuildVersion(StringRef Path) {
  return getSDKBuildVersionFromPlist((llvm::Twine(Path) +
    "/System/Library/CoreServices/SystemVersion.plist").str());
}

std::string swift::getSDKName(StringRef Path) {
  std::string Name = getPlistEntry(llvm::Twine(Path)+"/SDKSettings.plist",
                                   "CanonicalName");
  if (Name.empty() && Path.endswith(".sdk")) {
    Name = llvm::sys::path::filename(Path).drop_back(strlen(".sdk")).str();
  }
  return Name;
}
