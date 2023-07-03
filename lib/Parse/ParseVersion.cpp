//===--- ParseVersion.cpp - Parse Swift Version Numbers -------------------===//
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

#include "swift/Parse/ParseVersion.h"
#include "swift/AST/DiagnosticsParse.h"
#include "llvm/Support/FormatVariadic.h"

using namespace swift;
using namespace swift::version;

swift::version::Version version::getCurrentCompilerVersion() {
#ifdef SWIFT_COMPILER_VERSION
  auto currentVersion = VersionParser::parseVersionString(
      SWIFT_COMPILER_VERSION, SourceLoc(), nullptr);
  assert(currentVersion.hasValue() &&
         "Embedded Swift language version couldn't be parsed: "
         "'" SWIFT_COMPILER_VERSION "'");
  return currentVersion.getValue();
#else
  return Version();
#endif
}

static void splitVersionComponents(
    SmallVectorImpl<std::pair<StringRef, SourceRange>> &SplitComponents,
    StringRef &VersionString, SourceLoc Loc, bool skipQuote = false) {
  SourceLoc Start = (Loc.isValid() && skipQuote) ? Loc.getAdvancedLoc(1) : Loc;
  SourceLoc End = Start;

  // Split the version string into tokens separated by the '.' character.
  while (!VersionString.empty()) {
    StringRef SplitComponent, Rest;
    std::tie(SplitComponent, Rest) = VersionString.split('.');

    if (Loc.isValid()) {
      End = End.getAdvancedLoc(SplitComponent.size());
    }
    auto Range = Loc.isValid() ? SourceRange(Start, End) : SourceRange();
    if (Loc.isValid())
      End = End.getAdvancedLoc(1);
    Start = End;
    SplitComponents.push_back({SplitComponent, Range});
    VersionString = Rest;
  }
}

llvm::Optional<Version> VersionParser::parseCompilerVersionString(
    StringRef VersionString, SourceLoc Loc, DiagnosticEngine *Diags) {

  Version CV;
  SmallString<16> digits;
  llvm::raw_svector_ostream OS(digits);
  SmallVector<std::pair<StringRef, SourceRange>, 5> SplitComponents;

  splitVersionComponents(SplitComponents, VersionString, Loc,
                         /*skipQuote=*/true);

  uint64_t ComponentNumber;
  bool isValidVersion = true;

  auto checkVersionComponent = [&](unsigned Component, SourceRange Range) {
    unsigned limit = CV.Components.empty() ? 9223371 : 999;

    if (Component > limit) {
      if (Diags)
        Diags->diagnose(Range.Start,
                        diag::compiler_version_component_out_of_range, limit);
      isValidVersion = false;
    }
  };

  for (size_t i = 0; i < SplitComponents.size(); ++i) {
    StringRef SplitComponent;
    SourceRange Range;
    std::tie(SplitComponent, Range) = SplitComponents[i];

    // Version components can't be empty.
    if (SplitComponent.empty()) {
      if (Diags)
        Diags->diagnose(Range.Start, diag::empty_version_component);
      isValidVersion = false;
      continue;
    }

    // The second version component isn't used for comparison.
    if (i == 1) {
      if (!SplitComponent.equals("*")) {
        if (Diags) {
          // Majors 600-1300 were used for Swift 1.0-5.5 (based on clang
          // versions), but then we reset the numbering based on Swift versions,
          // so 5.6 had major 5. We assume that majors below 600 use the new
          // scheme and equal/above it use the old scheme.
          bool firstComponentLooksNew = CV.Components[0] < 600;

          auto diag = Diags->diagnose(Range.Start,
                                      diag::unused_compiler_version_component,
                                      firstComponentLooksNew);

          if (firstComponentLooksNew &&
              !SplitComponent.getAsInteger(10, ComponentNumber)) {
            // Fix-it version like "5.7.1.2.3" to "5007.*.1.2.3".
            auto newDigits =
                llvm::formatv("{0}{1,0+3}.*", CV.Components[0], ComponentNumber)
                    .str();
            diag.fixItReplaceChars(SplitComponents[0].second.Start, Range.End,
                                   newDigits);
          } else {
            diag.fixItReplaceChars(Range.Start, Range.End, "*");
          }
        }
      }

      CV.Components.push_back(0);
      continue;
    }

    // All other version components must be numbers.
    if (!SplitComponent.getAsInteger(10, ComponentNumber)) {
      checkVersionComponent(ComponentNumber, Range);
      CV.Components.push_back(ComponentNumber);
      continue;
    } else {
      if (Diags)
        Diags->diagnose(Range.Start, diag::version_component_not_number);
      isValidVersion = false;
    }
  }

  if (CV.Components.size() > 5) {
    if (Diags)
      Diags->diagnose(Loc, diag::compiler_version_too_many_components);
    isValidVersion = false;
  }

  // In the beginning, '_compiler_version(string-literal)' was designed for a
  // different version scheme where the major was fairly large and the minor
  // was ignored; now we use one where the minor is significant and major and
  // minor match the Swift language version. See the comment above on
  // `firstComponentLooksNew` for details.
  //
  // However, we want the string literal variant of '_compiler_version' to
  // maintain source compatibility with old checks; that means checks for new
  // versions have to be written so that old compilers will think they represent
  // newer versions, while new compilers have to interpret old version number
  // strings in a way that will compare correctly to the new versions compiled
  // into them.
  //
  // To achieve this, modern compilers divide the major by 1000 and overwrite
  // the wildcard component with the remainder, effectively shifting the last
  // three digits of the major into the minor, before comparing it to the
  // compiler version:
  //
  //     _compiler_version("5007.*.1.2.3") -> 5.7.1.2.3
  //     _compiler_version("1300.*.1.2.3") -> 1.300.1.2.3 (smaller than 5.6)
  //     _compiler_version( "600.*.1.2.3") -> 0.600.1.2.3 (smaller than 5.6)
  //
  // So if you want to specify a 5.7.z.a.b version, we ask users to either write
  // it as 5007.*.z.a.b, or to use the new '_compiler_version(>= version)'
  // syntax instead, which does not perform this conversion.
  if (!CV.Components.empty()) {
    if (CV.Components.size() == 1)
      CV.Components.push_back(0);
    CV.Components[1] = CV.Components[0] % 1000;
    CV.Components[0] = CV.Components[0] / 1000;
  }

  return isValidVersion ? llvm::Optional<Version>(CV) : llvm::None;
}

llvm::Optional<Version>
VersionParser::parseVersionString(StringRef VersionString, SourceLoc Loc,
                                  DiagnosticEngine *Diags) {
  Version TheVersion;
  SmallString<16> digits;
  llvm::raw_svector_ostream OS(digits);
  SmallVector<std::pair<StringRef, SourceRange>, 5> SplitComponents;
  // Skip over quote character in string literal.

  if (VersionString.empty()) {
    if (Diags)
      Diags->diagnose(Loc, diag::empty_version_string);
    return llvm::None;
  }

  splitVersionComponents(SplitComponents, VersionString, Loc, Diags);

  uint64_t ComponentNumber;
  bool isValidVersion = true;

  for (size_t i = 0; i < SplitComponents.size(); ++i) {
    StringRef SplitComponent;
    SourceRange Range;
    std::tie(SplitComponent, Range) = SplitComponents[i];

    // Version components can't be empty.
    if (SplitComponent.empty()) {
      if (Diags)
        Diags->diagnose(Range.Start, diag::empty_version_component);

      isValidVersion = false;
      continue;
    }

    // All other version components must be numbers.
    if (!SplitComponent.getAsInteger(10, ComponentNumber)) {
      TheVersion.Components.push_back(ComponentNumber);
      continue;
    } else {
      if (Diags)
        Diags->diagnose(Range.Start, diag::version_component_not_number);
      isValidVersion = false;
    }
  }

  return isValidVersion ? llvm::Optional<Version>(TheVersion) : llvm::None;
}
