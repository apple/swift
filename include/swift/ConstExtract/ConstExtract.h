//===---- ConstExtract.h -- Gather Compile-Time-Known Values ----*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_CONST_EXTRACT_H
#define SWIFT_CONST_EXTRACT_H

#include "swift/AST/ConstTypeInfo.h"
#include "llvm/ADT/ArrayRef.h"
#include <string>
#include <unordered_set>
#include <vector>

namespace llvm {
class StringRef;
class raw_fd_ostream;
}

namespace swift {
class SourceFile;
class DiagnosticEngine;
class ModuleDecl;
} // namespace swift

namespace swift {
/// Parse a list of string identifiers from a file at the given path,
/// representing names of protocols.
bool
parseProtocolListFromFile(llvm::StringRef protocolListFilePath,
                          DiagnosticEngine &diags,
                          std::unordered_set<std::string> &protocols);

/// Gather compile-time-known values of properties in nominal type declarations
/// in this file, of types which conform to one of the protocols listed in
/// \c Protocols
std::vector<ConstValueTypeInfo>
gatherConstValuesForPrimary(const std::unordered_set<std::string> &Protocols,
                            const SourceFile *File);

/// Gather compile-time-known values of properties in nominal type declarations
/// in this module, of types which conform to one of the protocols listed in
/// \c Protocols
std::vector<ConstValueTypeInfo>
gatherConstValuesForModule(const std::unordered_set<std::string> &Protocols,
                           ModuleDecl *Module);

/// Serialize a collection of \c ConstValueInfos to JSON at the
/// provided output stream.
bool writeAsJSONToFile(const std::vector<ConstValueTypeInfo> &ConstValueInfos,
                       llvm::raw_ostream &OS);
} // namespace swift

#endif
