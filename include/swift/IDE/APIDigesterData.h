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
#include "swift/Basic/JSONSerialization.h"
#include "swift/IDE/Utils.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/raw_ostream.h"

namespace swift {
class DiagnosticEngine;

namespace ide {
namespace api {

enum class KeyKind {
#define KEY(NAME) KK_##NAME,
#include "swift/IDE/DigesterEnums.def"
};

// The node kind appearing in the tree that describes the content of the SDK
enum class SDKNodeKind: uint8_t {
#define NODE_KIND(NAME, VALUE) NAME,
#include "DigesterEnums.def"
};

llvm::Optional<SDKNodeKind> parseSDKNodeKind(StringRef Content);

enum class NodeAnnotation: uint8_t{
#define NODE_ANNOTATION(NAME) NAME,
#include "DigesterEnums.def"
};

NodeAnnotation parseSDKNodeAnnotation(StringRef Content);

enum class SpecialCaseId: uint8_t{
#define SPECIAL_CASE_ID(NAME) NAME,
#include "DigesterEnums.def"
};

SpecialCaseId parseSpecialCaseId(StringRef Content);

enum class APIDiffItemKind: uint8_t{
#define DIFF_ITEM_KIND(NAME) ADK_##NAME,
#include "DigesterEnums.def"
};

// Redefine << so that we can output the name of the annotation kind.
raw_ostream &operator<<(raw_ostream &Out, const NodeAnnotation Value);

// Redefine << so that we can output the name of the node kind.
raw_ostream &operator<<(raw_ostream &Out, const SDKNodeKind Value);

StringRef getDeclKindStr(const DeclKind Value, bool lower);

// Redefine << so that we can output the name of decl kind.
raw_ostream &operator<<(raw_ostream &Out, const DeclKind Value);

struct APIDiffItem {
  virtual void streamDef(llvm::raw_ostream &S) const = 0;
  virtual APIDiffItemKind getKind() const = 0;
  virtual StringRef getKey() const = 0;
  virtual ~APIDiffItem() = default;
  bool operator==(const APIDiffItem &Other) const;
};

// CommonDiffItem describes how an element in SDK evolves in a way that migrator can
// read conveniently. Each CommonDiffItem corresponds to one JSON element and contains
// sub fields explaining how migrator can assist client code to cope with such
// SDK change. For instance, the following first JSON element describes an unwrap
// optional change in the first parameter of function "c:@F@CTTextTabGetOptions".
// Similarly, the second JSON element describes a type parameter down cast in the
// second parameter of function "c:objc(cs)NSXMLDocument(im)insertChildren:atIndex:".
// We keep both usrs because in the future this may support auto-rename.
struct CommonDiffItem: public APIDiffItem {
  SDKNodeKind NodeKind;
  NodeAnnotation DiffKind;
  StringRef ChildIndex;
private:
  llvm::SmallVector<uint8_t, 4> ChildIndexPieces;
public:
  StringRef LeftUsr;
  StringRef RightUsr;
  StringRef LeftComment;
  StringRef RightComment;
  StringRef ModuleName;

  CommonDiffItem(SDKNodeKind NodeKind, NodeAnnotation DiffKind,
                 StringRef ChildIndex, StringRef LeftUsr, StringRef RightUsr,
                 StringRef LeftComment, StringRef RightComment,
                 StringRef ModuleName);

  ArrayRef<uint8_t> getChildIndices() { return ChildIndexPieces; }
  static StringRef head();
  bool operator<(CommonDiffItem Other) const;
  static bool classof(const APIDiffItem *D);
  static void describe(llvm::raw_ostream &os);
  static void undef(llvm::raw_ostream &os);
  void streamDef(llvm::raw_ostream &S) const override;
  StringRef getKey() const override { return LeftUsr; }
  bool isRename() const {
    return DiffKind == NodeAnnotation::Rename ||
      DiffKind == NodeAnnotation::ModernizeEnum;
  }

  bool isTypeChange() const {
    switch (DiffKind) {
    case NodeAnnotation::WrapOptional:
    case NodeAnnotation::UnwrapOptional:
    case NodeAnnotation::ImplicitOptionalToOptional:
    case NodeAnnotation::OptionalToImplicitOptional:
    case NodeAnnotation::WrapImplicitOptional:
    case NodeAnnotation::TypeRewritten:
      return true;
    default:
      return false;
    }
  }

  bool isToPropertyChange() const {
    switch (DiffKind) {
    case NodeAnnotation::GetterToProperty:
    case NodeAnnotation::SetterToProperty:
      return true;
    default:
      return false;
    }
  }

  bool isStringRepresentableChange() const {
    switch(DiffKind) {
    case NodeAnnotation::DictionaryKeyUpdate:
    case NodeAnnotation::OptionalDictionaryKeyUpdate:
    case NodeAnnotation::ArrayMemberUpdate:
    case NodeAnnotation::OptionalArrayMemberUpdate:
    case NodeAnnotation::SimpleStringRepresentableUpdate:
    case NodeAnnotation::SimpleOptionalStringRepresentableUpdate:
      return true;
    default:
      return false;
    }
  }

  StringRef getNewName() const { assert(isRename()); return RightComment; }
  APIDiffItemKind getKind() const override {
    return APIDiffItemKind::ADK_CommonDiffItem;
  }

  bool rightCommentUnderscored() const {
    DeclNameViewer Viewer(RightComment);
    auto HasUnderScore = [](StringRef S) { return S.contains('_'); };
    auto Args = Viewer.args();
    return HasUnderScore(Viewer.base()) ||
        std::any_of(Args.begin(), Args.end(), HasUnderScore);
  }
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
enum class TypeMemberDiffItemSubKind {
  SimpleReplacement,
  QualifiedReplacement,
  GlobalFuncToStaticProperty,
  HoistSelfOnly,
  HoistSelfAndRemoveParam,
  HoistSelfAndUseProperty,
  FuncRename,
};

struct TypeMemberDiffItem: public APIDiffItem {
  StringRef usr;
  StringRef newTypeName;
  StringRef newPrintedName;
  llvm::Optional<uint8_t> selfIndex;
  llvm::Optional<uint8_t> removedIndex;
  StringRef oldTypeName;
  StringRef oldPrintedName;
private:
  DeclNameViewer OldNameViewer;
  DeclNameViewer NewNameViewer;
  std::string NewTypeDot;
public:
  TypeMemberDiffItemSubKind Subkind;

public:
  TypeMemberDiffItem(StringRef usr, StringRef newTypeName,
                     StringRef newPrintedName,
                     llvm::Optional<uint8_t> selfIndex,
                     llvm::Optional<uint8_t> removedIndex,
                     StringRef oldTypeName, StringRef oldPrintedName)
      : usr(usr), newTypeName(newTypeName), newPrintedName(newPrintedName),
        selfIndex(selfIndex), removedIndex(removedIndex),
        oldTypeName(oldTypeName), oldPrintedName(oldPrintedName),
        OldNameViewer(oldPrintedName), NewNameViewer(newPrintedName),
        NewTypeDot(isNewNameGlobal() ? ""
                                     : (llvm::Twine(newTypeName) + ".").str()),
        Subkind(getSubKind()) {}
  static StringRef head();
  static void describe(llvm::raw_ostream &os);
  static void undef(llvm::raw_ostream &os);
  void streamDef(llvm::raw_ostream &os) const override;
  bool operator<(TypeMemberDiffItem Other) const;
  static bool classof(const APIDiffItem *D);
  StringRef getKey() const override { return usr; }
  const DeclNameViewer &getOldName() const { return OldNameViewer; }
  const DeclNameViewer &getNewName() const { return NewNameViewer; }
  StringRef getNewTypeAndDot() const { return NewTypeDot; }
  APIDiffItemKind getKind() const override {
    return APIDiffItemKind::ADK_TypeMemberDiffItem;
  }
  bool isNewNameGlobal() const { return newTypeName.empty(); }
private:
  TypeMemberDiffItemSubKind getSubKind() const;
};

/// This is an authored item to associate a USR with a specially handled case.
/// It is up to the migrator to interpret the special case Id and apply proper
/// transformation on an entity.
struct SpecialCaseDiffItem: public APIDiffItem {
  StringRef usr;
  SpecialCaseId caseId;
public:
  SpecialCaseDiffItem(StringRef usr, StringRef caseId): usr(usr),
    caseId(parseSpecialCaseId(caseId)) {}
  StringRef getKey() const override { return usr; }
  void streamDef(llvm::raw_ostream &S) const override {};
  static bool classof(const APIDiffItem *D);
  APIDiffItemKind getKind() const override {
    return APIDiffItemKind::ADK_SpecialCaseDiffItem;
  }
};

struct NoEscapeFuncParam: public APIDiffItem {
  StringRef Usr;
  unsigned Index;

  NoEscapeFuncParam(StringRef Usr, unsigned Index) : Usr(Usr), Index(Index) {}
  static StringRef head();
  static void describe(llvm::raw_ostream &os);
  static void undef(llvm::raw_ostream &os);
  void streamDef(llvm::raw_ostream &os) const override;
  bool operator<(NoEscapeFuncParam Other) const;
  static bool classof(const APIDiffItem *D);
  StringRef getKey() const override { return Usr; }
  APIDiffItemKind getKind() const override {
    return APIDiffItemKind::ADK_NoEscapeFuncParam;
  }
};

/// This info is about functions meet the following criteria:
///   - This function is a member function of a type.
///   - This function is overloaded.
struct OverloadedFuncInfo: public APIDiffItem {
  StringRef Usr;

  OverloadedFuncInfo(StringRef Usr) : Usr(Usr) {}
  static StringRef head();
  static void describe(llvm::raw_ostream &os);
  static void undef(llvm::raw_ostream &os);
  void streamDef(llvm::raw_ostream &os) const override;
  bool operator<(OverloadedFuncInfo Other) const;
  static bool classof(const APIDiffItem *D);
  StringRef getKey() const override { return Usr; }
  APIDiffItemKind getKind() const override {
    return APIDiffItemKind::ADK_OverloadedFuncInfo;
  }
};

struct NameCorrectionInfo {
  StringRef OriginalName;
  StringRef CorrectedName;
  StringRef ModuleName;
  NameCorrectionInfo(StringRef OriginalName, StringRef CorrectedName,
    StringRef ModuleName): OriginalName(OriginalName),
    CorrectedName(CorrectedName), ModuleName(ModuleName) {}
  bool operator<(NameCorrectionInfo Other) const {
    if (ModuleName != Other.ModuleName)
      return ModuleName.compare(Other.ModuleName) < 0;
    else
      return OriginalName.compare(Other.OriginalName) < 0;
  }
};

/// APIDiffItem store is the interface that migrator should communicates with;
/// Given a key, usually the usr of the system entity under migration, the store
/// should return a slice of related changes in the same format of
/// swift-api-digester. This struct also handles the serialization and
/// deserialization of all kinds of API diff items declared above.
struct APIDiffItemStore {
  struct Implementation;
  Implementation &Impl;
  static void serialize(llvm::raw_ostream &os, ArrayRef<APIDiffItem*> Items);
  static void serialize(llvm::raw_ostream &os, ArrayRef<NameCorrectionInfo> Items);
  APIDiffItemStore(const APIDiffItemStore& that) = delete;
  APIDiffItemStore(DiagnosticEngine &Diags);
  ~APIDiffItemStore();
  ArrayRef<APIDiffItem*> getDiffItems(StringRef Key) const;
  ArrayRef<APIDiffItem*> getAllDiffItems() const;
  void printIncomingUsr(bool print = true);

  /// Add a path of a JSON file dumped from swift-api-digester that contains
  /// API changes we care about. Calling this can be heavy since the procedure
  /// will parse and index the data inside of the given file.
  void addStorePath(StringRef Path);
};
}
}
namespace json {
template<>
struct ScalarEnumerationTraits<ide::api::SDKNodeKind> {
  static void enumeration(Output &out, ide::api::SDKNodeKind &value) {
#define NODE_KIND(KEY, VALUE)                                                 \
    out.enumCase(value, #VALUE, ide::api::SDKNodeKind::KEY);
#include "swift/IDE/DigesterEnums.def"
  }
};
}
}
#endif // SWIFT_IDE_APIDIGESTERDATA_H
