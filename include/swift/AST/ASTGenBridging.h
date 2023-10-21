//===--- ASTGenBridging.h - header for the swift ASTGenBridging module ----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_ASTGENBRIDGING_H
#define SWIFT_AST_ASTGENBRIDGING_H

// Workaround to avoid a compiler error because `cas::ObjectRef` is not defined
// when including VirtualFileSystem.h
#include <cassert>
#include "llvm/CAS/CASReference.h"

#include "swift/AST/ASTGenBridgingWrappers.h"
#include "swift/Basic/Compiler.h"
#include "swift/Basic/Nullability.h"
#include "swift/Basic/SourceLoc.h"

// Workaround to avoid an error on `VarDecl::getPropertyWrapperMutability`, for
// some reason C++ interop insists on instantiating the template there.
#include "swift/AST/Type.h"
#include "swift/AST/PropertyWrappers.h"

#include "swift/AST/Decl.h"
#include "swift/AST/DeclContext.h"
#include "swift/AST/Expr.h"
#include "swift/AST/GenericParamList.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/TypeRepr.h"

#endif // SWIFT_AST_ASTGENBRIDGING_H
