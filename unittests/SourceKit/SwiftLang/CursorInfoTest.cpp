//===----------------------------------------------------------------------===//
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

#include "SourceKit/Core/Context.h"
#include "SourceKit/Core/LangSupport.h"
#include "SourceKit/Core/NotificationCenter.h"
#include "SourceKit/Support/Concurrency.h"
#include "SourceKit/SwiftLang/Factory.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TargetSelect.h"
#include "gtest/gtest.h"

using namespace SourceKit;
using namespace llvm;

static StringRef getRuntimeLibPath() {
  return sys::path::parent_path(SWIFTLIB_DIR);
}

static SmallString<128> getSwiftExecutablePath() {
  SmallString<128> path = sys::path::parent_path(getRuntimeLibPath());
  sys::path::append(path, "bin", "swift-frontend");
  return path;
}

static void *createCancellationToken() {
  static std::atomic<size_t> handle(1000);
  return reinterpret_cast<void *>(
      handle.fetch_add(1, std::memory_order_relaxed));
}

namespace {

class NullEditorConsumer : public EditorConsumer {
  bool needsSemanticInfo() override { return needsSema; }

  void handleRequestError(const char *Description) override {
    llvm_unreachable("unexpected error");
  }

  bool syntaxMapEnabled() override { return true; }

  void handleSyntaxMap(unsigned Offset, unsigned Length, UIdent Kind) override {
  }

  void handleSemanticAnnotation(unsigned Offset, unsigned Length, UIdent Kind,
                                bool isSystem) override {}

  bool documentStructureEnabled() override { return false; }

  void beginDocumentSubStructure(unsigned Offset, unsigned Length,
                                 UIdent Kind, UIdent AccessLevel,
                                 UIdent SetterAccessLevel,
                                 unsigned NameOffset,
                                 unsigned NameLength,
                                 unsigned BodyOffset,
                                 unsigned BodyLength,
                                 unsigned DocOffset,
                                 unsigned DocLength,
                                 StringRef DisplayName,
                                 StringRef TypeName,
                                 StringRef RuntimeName,
                                 StringRef SelectorName,
                                 ArrayRef<StringRef> InheritedTypes,
                                 ArrayRef<std::tuple<UIdent, unsigned, unsigned>> Attrs) override {
  }

  void endDocumentSubStructure() override {}

  void handleDocumentSubStructureElement(UIdent Kind, unsigned Offset,
                                         unsigned Length) override {}

  void recordAffectedRange(unsigned Offset, unsigned Length) override {}

  void recordAffectedLineRange(unsigned Line, unsigned Length) override {}

  bool diagnosticsEnabled() override { return false; }

  void handleDiagnostics(ArrayRef<DiagnosticEntryInfo> DiagInfos,
                         UIdent DiagStage) override {}
  void recordFormattedText(StringRef Text) override {}

  void handleSourceText(StringRef Text) override {}

public:
  bool needsSema = false;
};

struct TestCursorInfo {
  // Empty if no error.
  std::string Error;
  std::string InternalDiagnostic;
  std::string Name;
  std::string Typename;
  std::string Filename;
  unsigned Offset;
  unsigned Length;
};

class CursorInfoTest : public ::testing::Test {
  SourceKit::Context &Ctx;
  std::atomic<int> NumTasks;
  NullEditorConsumer Consumer;

public:
  SourceKit::Context &getContext() { return Ctx; }
  LangSupport &getLang() { return getContext().getSwiftLangSupport(); }

  void SetUp() override {
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmPrinters();
    llvm::InitializeAllAsmParsers();
    NumTasks = 0;
  }

  CursorInfoTest()
      : Ctx(*new SourceKit::Context(getSwiftExecutablePath(),
                                    getRuntimeLibPath(),
                                    /*diagnosticDocumentationPath*/ "",
                                    SourceKit::createSwiftLangSupport,
                                    /*dispatchOnMain=*/false)) {
    // This is avoiding destroying \p SourceKit::Context because another
    // thread may be active trying to use it to post notifications.
    // FIXME: Use shared_ptr ownership to avoid such issues.
  }

  void addNotificationReceiver(DocumentUpdateNotificationReceiver Receiver) {
    Ctx.getNotificationCenter()->addDocumentUpdateNotificationReceiver(Receiver);
  }

  void open(const char *DocName, StringRef Text,
            Optional<ArrayRef<const char *>> CArgs = llvm::None) {
    auto Args = CArgs.has_value() ? makeArgs(DocName, *CArgs)
                                  : std::vector<const char *>{};
    auto Buf = MemoryBuffer::getMemBufferCopy(Text, DocName);
    getLang().editorOpen(DocName, Buf.get(), Consumer, Args, None);
  }

  void replaceText(StringRef DocName, unsigned Offset, unsigned Length,
                   StringRef Text) {
    auto Buf = MemoryBuffer::getMemBufferCopy(Text, DocName);
    getLang().editorReplaceText(DocName, Buf.get(), Offset, Length, Consumer);
  }

  TestCursorInfo
  getCursor(const char *DocName, unsigned Offset, ArrayRef<const char *> CArgs,
            SourceKitCancellationToken CancellationToken = nullptr,
            bool CancelOnSubsequentRequest = false) {
    auto Args = makeArgs(DocName, CArgs);
    Semaphore sema(0);

    TestCursorInfo TestInfo;
    getLang().getCursorInfo(
        DocName, DocName, Offset, /*Length=*/0, /*Actionables=*/false,
        /*SymbolGraph=*/false, CancelOnSubsequentRequest, Args,
        /*vfsOptions=*/None, CancellationToken,
        [&](const RequestResult<CursorInfoData> &Result) {
          assert(!Result.isCancelled());
          if (Result.isError()) {
            TestInfo.Error = Result.getError().str();
            sema.signal();
            return;
          }
          const CursorInfoData &Info = Result.value();
          TestInfo.InternalDiagnostic = Info.InternalDiagnostic.str();
          if (!Info.Symbols.empty()) {
            const CursorSymbolInfo &MainSymbol = Info.Symbols[0];
            TestInfo.Name = std::string(MainSymbol.Name.str());
            TestInfo.Typename = MainSymbol.TypeName.str();
            TestInfo.Filename = MainSymbol.Location.Filename.str();
            TestInfo.Offset = MainSymbol.Location.Offset;
            TestInfo.Length = MainSymbol.Location.Length;
          }
          sema.signal();
        });

    bool expired = sema.wait(60 * 1000);
    if (expired)
      llvm::report_fatal_error("check took too long");
    return TestInfo;
  }

  unsigned findOffset(StringRef Val, StringRef Text) {
    auto pos = Text.find(Val);
    assert(pos != StringRef::npos);
    return pos;
  }

  void setNeedsSema(bool needsSema) { Consumer.needsSema = needsSema; }

private:
  std::vector<const char *> makeArgs(const char *DocName,
                                     ArrayRef<const char *> CArgs) {
    std::vector<const char *> Args = CArgs;
    Args.push_back(DocName);
    return Args;
  }
};

} // anonymous namespace

TEST_F(CursorInfoTest, FileNotExist) {
  const char *DocName = "test.swift";
  const char *Contents =
    "let foo = 0\n";
  const char *Args[] = { "<not-existent-file>" };

  open(DocName, Contents);
  auto FooOffs = findOffset("foo =", Contents);
  auto Info = getCursor(DocName, FooOffs, Args);
  EXPECT_STREQ("foo", Info.Name.c_str());
  EXPECT_STREQ("Int", Info.Typename.c_str());
}

static const char *ExpensiveInit =
    "[0:0,0:0,0:0,0:0,0:0,0:0,0:0]";

TEST_F(CursorInfoTest, EditAfter) {
  const char *DocName = "test.swift";
  const char *Contents =
    "let value = foo\n"
    "let foo = 0\n";
  const char *Args[] = { "-parse-as-library" };

  open(DocName, Contents);
  auto FooRefOffs = findOffset("foo", Contents);
  auto FooOffs = findOffset("foo =", Contents);
  auto Info = getCursor(DocName, FooRefOffs, Args);
  EXPECT_STREQ("foo", Info.Name.c_str());
  EXPECT_STREQ("Int", Info.Typename.c_str());
  EXPECT_STREQ(DocName, Info.Filename.c_str());
  EXPECT_EQ(FooOffs, Info.Offset);
  EXPECT_EQ(strlen("foo"), Info.Length);

  StringRef TextToReplace = "0";
  replaceText(DocName, findOffset(TextToReplace, Contents), TextToReplace.size(),
              ExpensiveInit);
  // Insert a space in front of 'foo' decl.
  replaceText(DocName, FooOffs, 0, " ");
  ++FooOffs;

  // Should not wait for the new AST, it should give the previous answer.
  Info = getCursor(DocName, FooRefOffs, Args);
  EXPECT_STREQ("foo", Info.Name.c_str());
  EXPECT_STREQ("Int", Info.Typename.c_str());
  EXPECT_STREQ(DocName, Info.Filename.c_str());
  EXPECT_EQ(FooOffs, Info.Offset);
  EXPECT_EQ(strlen("foo"), Info.Length);
}

TEST_F(CursorInfoTest, EditBefore) {
  const char *DocName = "test.swift";
  const char *Contents =
    "let foo = 0\n"
    "let value = foo;\n";
  const char *Args[] = { "-parse-as-library" };

  open(DocName, Contents);
  auto FooRefOffs = findOffset("foo;", Contents);
  auto FooOffs = findOffset("foo =", Contents);
  auto Info = getCursor(DocName, FooRefOffs, Args);
  EXPECT_STREQ("", Info.Error.c_str());
  EXPECT_STREQ("", Info.InternalDiagnostic.c_str());
  EXPECT_STREQ("foo", Info.Name.c_str());
  EXPECT_STREQ("Int", Info.Typename.c_str());
  EXPECT_STREQ(DocName, Info.Filename.c_str());
  EXPECT_EQ(FooOffs, Info.Offset);
  EXPECT_EQ(strlen("foo"), Info.Length);

  StringRef TextToReplace = "0";
  replaceText(DocName, findOffset(TextToReplace, Contents), TextToReplace.size(),
              ExpensiveInit);
  FooRefOffs += StringRef(ExpensiveInit).size() - TextToReplace.size();
  // Insert a space in front of 'foo' decl.
  replaceText(DocName, FooOffs, 0, " ");
  ++FooOffs;
  ++FooRefOffs;

  // Should not wait for the new AST, it should give the previous answer.
  Info = getCursor(DocName, FooRefOffs, Args);
  EXPECT_STREQ("", Info.Error.c_str());
  EXPECT_STREQ("", Info.InternalDiagnostic.c_str());
  EXPECT_STREQ("foo", Info.Name.c_str());
  EXPECT_STREQ("Int", Info.Typename.c_str());
  EXPECT_STREQ(DocName, Info.Filename.c_str());
  EXPECT_EQ(FooOffs, Info.Offset);
  EXPECT_EQ(strlen("foo"), Info.Length);
}

TEST_F(CursorInfoTest, CursorInfoMustWaitDueDeclLoc) {
  const char *DocName = "test.swift";
  const char *Contents =
    "let value = foo\n"
    "let foo = 0\n";
  const char *Args[] = { "-parse-as-library" };

  open(DocName, Contents);
  auto FooRefOffs = findOffset("foo", Contents);
  auto FooOffs = findOffset("foo =", Contents);
  auto Info = getCursor(DocName, FooRefOffs, Args);
  EXPECT_STREQ("", Info.Error.c_str());
  EXPECT_STREQ("", Info.InternalDiagnostic.c_str());
  EXPECT_STREQ("foo", Info.Name.c_str());
  EXPECT_STREQ("Int", Info.Typename.c_str());

  StringRef TextToReplace = "0";
  replaceText(DocName, findOffset(TextToReplace, Contents), TextToReplace.size(),
              ExpensiveInit);
  // Edit over the 'foo' decl.
  replaceText(DocName, FooOffs, strlen("foo"), "foo");

  // Should wait for the new AST, because the declaration location for the 'foo'
  // reference has been edited out.
  Info = getCursor(DocName, FooRefOffs, Args);
  EXPECT_STREQ("", Info.Error.c_str());
  EXPECT_STREQ("", Info.InternalDiagnostic.c_str());
  EXPECT_STREQ("foo", Info.Name.c_str());
  EXPECT_STREQ("[Int : Int]", Info.Typename.c_str());
  EXPECT_EQ(FooOffs, Info.Offset);
  EXPECT_EQ(strlen("foo"), Info.Length);
}

TEST_F(CursorInfoTest, CursorInfoMustWaitDueOffset) {
  const char *DocName = "test.swift";
  const char *Contents =
    "let value = foo\n"
    "let foo = 0\n";
  const char *Args[] = { "-parse-as-library" };

  open(DocName, Contents);
  auto FooRefOffs = findOffset("foo", Contents);
  auto FooOffs = findOffset("foo =", Contents);
  auto Info = getCursor(DocName, FooRefOffs, Args);
  EXPECT_STREQ("foo", Info.Name.c_str());
  EXPECT_STREQ("Int", Info.Typename.c_str());

  StringRef TextToReplace = "0";
  replaceText(DocName, findOffset(TextToReplace, Contents), TextToReplace.size(),
              ExpensiveInit);
  // Edit over the 'foo' reference.
  replaceText(DocName, FooRefOffs, strlen("foo"), "foo");

  // Should wait for the new AST, because the cursor location has been edited
  // out.
  Info = getCursor(DocName, FooRefOffs, Args);
  EXPECT_STREQ("foo", Info.Name.c_str());
  EXPECT_STREQ("[Int : Int]", Info.Typename.c_str());
  EXPECT_EQ(FooOffs, Info.Offset);
  EXPECT_EQ(strlen("foo"), Info.Length);
}

TEST_F(CursorInfoTest, CursorInfoMustWaitDueToken) {
  const char *DocName = "test.swift";
  const char *Contents =
    "let value = foo\n"
    "let foo = 0\n";
  const char *Args[] = { "-parse-as-library" };

  open(DocName, Contents);
  auto FooRefOffs = findOffset("foo", Contents);
  auto FooOffs = findOffset("foo =", Contents);
  auto Info = getCursor(DocName, FooRefOffs, Args);
  EXPECT_STREQ("foo", Info.Name.c_str());
  EXPECT_STREQ("Int", Info.Typename.c_str());

  StringRef TextToReplace = "0";
  replaceText(DocName, findOffset(TextToReplace, Contents), TextToReplace.size(),
              ExpensiveInit);
  // Change 'foo' to 'fog' by replacing the last character.
  replaceText(DocName, FooOffs+2, 1, "g");
  replaceText(DocName, FooRefOffs+2, 1, "g");

  // Should wait for the new AST, because the cursor location points to a
  // different token.
  Info = getCursor(DocName, FooRefOffs, Args);
  EXPECT_STREQ("fog", Info.Name.c_str());
  EXPECT_STREQ("[Int : Int]", Info.Typename.c_str());
  EXPECT_EQ(FooOffs, Info.Offset);
  EXPECT_EQ(strlen("fog"), Info.Length);
}

TEST_F(CursorInfoTest, CursorInfoMustWaitDueTokenRace) {
  const char *DocName = "test.swift";
  const char *Contents = "let value = foo\n"
                         "let foo = 0\n";
  const char *Args[] = {"-parse-as-library"};

  auto FooRefOffs = findOffset("foo", Contents);
  auto FooOffs = findOffset("foo =", Contents);

  // Open with args, kicking off an ast build. The hope of this tests is for
  // this AST to still be in the process of building when we start the cursor
  // info, to ensure the ASTManager doesn't try to handle this cursor info with
  // the wrong AST.
  setNeedsSema(true);
  open(DocName, Contents, llvm::makeArrayRef(Args));
  // Change 'foo' to 'fog' by replacing the last character.
  replaceText(DocName, FooOffs + 2, 1, "g");
  replaceText(DocName, FooRefOffs + 2, 1, "g");

  // Should wait for the new AST, because the cursor location points to a
  // different token.
  auto Info = getCursor(DocName, FooRefOffs, Args);
  EXPECT_STREQ("fog", Info.Name.c_str());
  EXPECT_STREQ("Int", Info.Typename.c_str());
  EXPECT_EQ(FooOffs, Info.Offset);
  EXPECT_EQ(strlen("fog"), Info.Length);
}

TEST_F(CursorInfoTest, CursorInfoCancelsPreviousRequest) {
  // TODO: This test case relies on the following snippet being slow to type 
  // check so that the first cursor info request takes longer to execute than it 
  // takes time to schedule the second request. If that is fixed, we need to 
  // find a new way to cause slow type checking. rdar://80582770
  const char *SlowDocName = "slow.swift";
  const char *SlowContents = "func foo(x: Invalid1, y: Invalid2) {\n"
                             "    x / y / x / y / x / y / x / y\n"
                             "}\n";
  auto SlowOffset = findOffset("x", SlowContents);
  const char *Args[] = {"-parse-as-library"};
  std::vector<const char *> ArgsForSlow = llvm::makeArrayRef(Args).vec();
  ArgsForSlow.push_back(SlowDocName);

  const char *FastDocName = "fast.swift";
  const char *FastContents = "func bar() {\n"
                             "    let foo = 123\n"
                             "}\n";
  auto FastOffset = findOffset("foo", FastContents);
  std::vector<const char *> ArgsForFast = llvm::makeArrayRef(Args).vec();
  ArgsForFast.push_back(FastDocName);

  open(SlowDocName, SlowContents, llvm::makeArrayRef(Args));
  open(FastDocName, FastContents, llvm::makeArrayRef(Args));

  // Schedule a cursor info request that takes long to execute. This should be
  // cancelled as the next cursor info (which is faster) gets requested.
  Semaphore FirstCursorInfoSema(0);
  getLang().getCursorInfo(
      SlowDocName, SlowDocName, SlowOffset, /*Length=*/0, /*Actionables=*/false,
      /*SymbolGraph=*/false, /*CancelOnSubsequentRequest=*/true, ArgsForSlow,
      /*vfsOptions=*/None, /*CancellationToken=*/nullptr,
      [&](const RequestResult<CursorInfoData> &Result) {
        EXPECT_TRUE(Result.isCancelled());
        FirstCursorInfoSema.signal();
      });

  auto Info = getCursor(FastDocName, FastOffset, Args,
                        /*CancellationToken=*/nullptr,
                        /*CancelOnSubsequentRequest=*/true);
  EXPECT_STREQ("foo", Info.Name.c_str());
  EXPECT_STREQ("Int", Info.Typename.c_str());
  EXPECT_EQ(FastOffset, Info.Offset);
  EXPECT_EQ(strlen("foo"), Info.Length);

  bool expired = FirstCursorInfoSema.wait(30 * 1000);
  if (expired)
    llvm::report_fatal_error("Did not receive a response for the first request");
}

TEST_F(CursorInfoTest, CursorInfoCancellation) {
  // TODO: This test case relies on the following snippet being slow to type
  // check so that the first cursor info request takes longer to execute than it
  // takes time to schedule the second request. If that is fixed, we need to
  // find a new way to cause slow type checking. rdar://80582770
  const char *SlowDocName = "slow.swift";
  const char *SlowContents = "func foo(x: Invalid1, y: Invalid2) {\n"
                             "    x / y / x / y / x / y / x / y\n"
                             "}\n";
  auto SlowOffset = findOffset("x", SlowContents);
  const char *Args[] = {"-parse-as-library"};
  std::vector<const char *> ArgsForSlow = llvm::makeArrayRef(Args).vec();
  ArgsForSlow.push_back(SlowDocName);

  open(SlowDocName, SlowContents, llvm::makeArrayRef(Args));

  SourceKitCancellationToken CancellationToken = createCancellationToken();

  // Schedule a cursor info request that takes long to execute. This should be
  // cancelled as the next cursor info (which is faster) gets requested.
  Semaphore CursorInfoSema(0);
  getLang().getCursorInfo(
      SlowDocName, SlowDocName, SlowOffset, /*Length=*/0, /*Actionables=*/false,
      /*SymbolGraph=*/false, /*CancelOnSubsequentRequest=*/false, ArgsForSlow,
      /*vfsOptions=*/None, /*CancellationToken=*/CancellationToken,
      [&](const RequestResult<CursorInfoData> &Result) {
        EXPECT_TRUE(Result.isCancelled());
        CursorInfoSema.signal();
      });

  getContext().getRequestTracker()->cancel(CancellationToken);

  bool expired = CursorInfoSema.wait(30 * 1000);
  if (expired)
    llvm::report_fatal_error("Did not receive a response for the first request");
}
