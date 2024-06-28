//===--- Options.h - Option info & table ------------------------*- C++ -*-===//
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

#ifndef SWIFT_OPTION_OPTIONS_H
#define SWIFT_OPTION_OPTIONS_H

#include <memory>

namespace llvm {
namespace opt {
  class OptTable;
}
}

namespace swift {
namespace options {
  /// Flags specifically for Swift driver options.  Must not overlap with
  /// llvm::opt::DriverFlag.
  enum SwiftFlags {
    FrontendOption = (1 << 4),
    NoDriverOption = (1 << 5),
    NoInteractiveOption = (1 << 6),
    NoBatchOption = (1 << 7),
    DoesNotAffectIncrementalBuild = (1 << 8),
    AutolinkExtractOption = (1 << 9),
    ModuleWrapOption = (1 << 10),
    SwiftIndentOption = (1 << 11),
    ArgumentIsPath = (1 << 12),
    ModuleInterfaceOption = (1 << 13),
    SupplementaryOutput = (1 << 14),
    SwiftSymbolGraphExtractOption = (1 << 15),
    SwiftAPIDigesterOption = (1 << 16),
    NewDriverOnlyOption = (1 << 17),
    ModuleInterfaceOptionIgnorable = (1 << 18),
    ModuleInterfaceOptionIgnorablePrivate = (1 << 19),
    ArgumentIsFileList = (1 << 20),
    CacheInvariant = (1 << 21),
  };

  enum ID {
    OPT_INVALID = 0, // This is not an option ID.
#define OPTION(PREFIX, NAME, ID, KIND, GROUP, ALIAS, ALIASARGS, FLAGS, PARAM,  \
               HELPTEXT, METAVAR, VALUES)                                      \
    OPT_##ID,
#include "swift/Option/Options.inc"
    LastOption
#undef OPTION
  };
} //end namespace options

  std::unique_ptr<llvm::opt::OptTable> createSwiftOptTable();

} // end namespace swift

#endif
