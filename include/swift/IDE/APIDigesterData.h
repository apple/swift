//===--- APIDigesterData.h - Declaration of api digester data ---*- C++ -*-===//
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

#ifndef SWIFT_IDE_APIDIGESTERDATA_H
#define SWIFT_IDE_APIDIGESTERDATA_H

#include "swift/Basic/LLVM.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

namespace swift {
namespace ide {
namespace api {

// The node kind appearing in the tree that describes the content of the SDK
enum class SDKNodeKind: uint8_t {
#define NODE_KIND(NAME) NAME,
#include "DigesterEnums.def"
};

enum class NodeAnnotation: uint8_t{
#define NODE_ANNOTATION(NAME) NAME,
#include "DigesterEnums.def"
};

// Redefine << so that we can output the name of the annotation kind.
raw_ostream &operator<<(raw_ostream &Out, const NodeAnnotation Value);

// Redefine << so that we can output the name of the node kind.
raw_ostream &operator<<(raw_ostream &Out, const SDKNodeKind Value);

// DiffItem describes how an element in SDK evolves in a way that migrator can
// read conveniently. Each DiffItem corresponds to one JSON element and contains
// sub fields explaining how migrator can assist client code to cope with such
// SDK change. For instance, the following first JSON element describes an unwrap
// optional change in the first parameter of function "c:@F@CTTextTabGetOptions".
// Similarly, the second JSON element describes a type parameter down cast in the
// second parameter of function "c:objc(cs)NSXMLDocument(im)insertChildren:atIndex:".
// We keep both usrs because in the future this may support auto-rename.
struct DiffItem {
  SDKNodeKind NodeKind;
  NodeAnnotation DiffKind;
  StringRef ChildIndex;
  StringRef LeftUsr;
  StringRef RightUsr;
  StringRef LeftComment;
  StringRef RightComment;
  StringRef ModuleName;

  DiffItem(SDKNodeKind NodeKind, NodeAnnotation DiffKind, StringRef ChildIndex,
           StringRef LeftUsr, StringRef RightUsr, StringRef LeftComment,
           StringRef RightComment, StringRef ModuleName);

  static StringRef head();
  bool operator<(DiffItem Other) const;
  static void describe(llvm::raw_ostream &os);
  static void undef(llvm::raw_ostream &os);
  void streamDef(llvm::raw_ostream &S) const;
};


// TypeMemberDiffItem stores info about movements of functions to type members
//
// Outputs:
//
// SDK_CHANGE_TYPE_MEMBER(USR, new type context name, new printed name, self
//                        index, old printed name)
//
// Examples:
//----------------------------------------------------------------------------//
// Init:
//
//  CGAffineTransformMakeScale(_:_:)
//    ==>
//  SDK_CHANGE_TYPE_MEMBER("c:@F@CGAffineTransformMakeScale",
//                         "CGAffineTransform", "init(scaleX:y:)", ,
//                         "CGAffineTransformMakeScale(_:_:)")
//
//  Meaning that source should transform like:
//  let myAffineTransform = CGAffineTransformMakeScale(myX, myY)
//    ==>
//  let myAffineTransform = CGAffineTransform(scaleX: myX, y: myY)
//
//
//----------------------------------------------------------------------------//
// Static/Class Method:
//
//  CGColorGetConstantColor(_:)
//    ==>
//  SDK_CHANGE_TYPE_MEMBER("c:@F@CGColorGetConstantColor", "CGColor",
//                         "constantColor(forName:)", ,
//                         "CGColorGetConstantColor(_:)")
//
// Meaning that source should transform like:
//  CGColorGetConstantColor(nameOfWhiteColor)
//    ==>
//  CGColor.constantColor(forName: nameOfWhiteColor)
//
//
//----------------------------------------------------------------------------//
// Instance Method:
//
//  CGEventPost(_:_:)
//    ==>
//  SDK_CHANGE_TYPE_MEMBER("c:@F@CGEventPost", "CGEvent", "post(tap:)", 1,
//  "CGEventPost(_:_:)")
//
// Meaning that source should transform like:
//  CGEventPost(myTap, myEvent)
//    ==>
//  myEvent.post(tap: myTap)
//
//
//----------------------------------------------------------------------------//
// Static/Class Stored Variable:
//
//  kCGColorWhite
//    ==>
//  SDK_CHANGE_TYPE_MEMBER("c:@kCGColorWhite", "CGColor", "white", ,
//                         "kCGColorWhite")
//
// Meaning that source should transform like:
//  let colorName = kCGColorWhite
//    ==>
//  let colorName = CGColor.white
//
//
//----------------------------------------------------------------------------//
// Instance Computed Property
//
//
//  CGColorGetComponents(_:)
//    ==>
//  SDK_CHANGE_TYPE_MEMBER("c:@F@CGColorGetComponents", "CGColor",
//                         "components", 0, "CGColorGetComponents(_:)")
//
// Meaning that source should transform like:
//  CGColorGetComponents(myColor)
//    ==>
//  myColor.components
//
//
struct TypeMemberDiffItem {
  StringRef usr;
  StringRef newTypeName;
  StringRef newPrintedName;
  Optional<uint8_t> selfIndex;
  StringRef oldPrintedName;

  static StringRef head();
  static void describe(llvm::raw_ostream &os);
  static void undef(llvm::raw_ostream &os);
  void streamDef(llvm::raw_ostream &os) const;
  bool operator<(TypeMemberDiffItem Other) const;
};

struct NoEscapeFuncParam {
  StringRef Usr;
  unsigned Index;

  NoEscapeFuncParam(StringRef Usr, unsigned Index) : Usr(Usr), Index(Index) {}
  static StringRef head();
  static void describe(llvm::raw_ostream &os);
  static void undef(llvm::raw_ostream &os);
  void streamDef(llvm::raw_ostream &os) const;
  bool operator<(NoEscapeFuncParam Other) const;
};

/// This info is about functions meet the following criteria:
///   - This function is a member function of a type.
///   - This function is overloaded.
struct OverloadedFuncInfo {
  StringRef Usr;

  OverloadedFuncInfo(StringRef Usr) : Usr(Usr) {}
  static StringRef head();
  static void describe(llvm::raw_ostream &os);
  static void undef(llvm::raw_ostream &os);
  void streamDef(llvm::raw_ostream &os) const;
  bool operator<(OverloadedFuncInfo Other) const;
};
}
}
}
#endif // SWIFT_IDE_APIDIGESTERDATA_H
