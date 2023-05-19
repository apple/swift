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

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

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

namespace {

struct Token {
  unsigned Offset;
  unsigned Length;
  UIdent Kind;
  bool IsSystem;
};

class TestConsumer : public EditorConsumer {
public:
  UIdent DiagStage;
  std::vector<DiagnosticEntryInfo> Diags;
  std::vector<Token> Annotations;

private:
  void handleRequestError(const char *Description) override {
    llvm_unreachable("unexpected error");
  }

  bool syntaxMapEnabled() override { return true; }

  void handleSyntaxMap(unsigned Offset, unsigned Length, UIdent Kind) override {
  }

  void handleSemanticAnnotation(unsigned Offset, unsigned Length, UIdent Kind,
                                bool isSystem) override {
    Annotations.push_back({Offset, Length, Kind, isSystem});
  }

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

  void recordFormattedText(StringRef Text) override {}

  bool diagnosticsEnabled() override { return true; }

  void handleDiagnostics(ArrayRef<DiagnosticEntryInfo> DiagInfos,
                         UIdent DiagStage) override {
    this->DiagStage = DiagStage;
    Diags.insert(Diags.end(), DiagInfos.begin(), DiagInfos.end());
  }

  void handleSourceText(StringRef Text) override {}
};

struct DocUpdateMutexState {
  std::mutex Mtx;
  std::condition_variable CV;
  bool HasUpdate = false;
};

class EditTest : public ::testing::Test {
  SourceKit::Context *Ctx;
  std::shared_ptr<DocUpdateMutexState> DocUpdState;

public:
  EditTest() {
    // This is avoiding destroying \p SourceKit::Context because another
    // thread may be active trying to use it to post notifications.
    // FIXME: Use shared_ptr ownership to avoid such issues.
    Ctx = new SourceKit::Context(getSwiftExecutablePath(),
                                 getRuntimeLibPath(),
                                 /*diagnosticDocumentationPath*/ "",
                                 SourceKit::createSwiftLangSupport,
                                 /*dispatchOnMain=*/false);
    auto localDocUpdState = std::make_shared<DocUpdateMutexState>();
    Ctx->getNotificationCenter()->addDocumentUpdateNotificationReceiver(
        [localDocUpdState](StringRef docName) {
          std::unique_lock<std::mutex> lk(localDocUpdState->Mtx);
          localDocUpdState->HasUpdate = true;
          localDocUpdState->CV.notify_one();
        });
    DocUpdState = localDocUpdState;
  }

  LangSupport &getLang() { return Ctx->getSwiftLangSupport(); }

  void SetUp() override {
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmPrinters();
    llvm::InitializeAllAsmParsers();
  }

  void addNotificationReceiver(DocumentUpdateNotificationReceiver Receiver) {
    Ctx->getNotificationCenter()->addDocumentUpdateNotificationReceiver(Receiver);
  }

  bool waitForDocUpdate(bool reset = false) {
    std::chrono::seconds secondsToWait(20);
    std::unique_lock<std::mutex> lk(DocUpdState->Mtx);
    auto when = std::chrono::system_clock::now() + secondsToWait;
    auto result = !DocUpdState->CV.wait_until(
        lk, when, [&]() { return DocUpdState->HasUpdate; });
    if (reset)
      DocUpdState->HasUpdate = false;
    return result;
  }

  void open(const char *DocName, StringRef Text, ArrayRef<const char *> CArgs,
            EditorConsumer &Consumer) {
    auto Args = makeArgs(DocName, CArgs);
    auto Buf = MemoryBuffer::getMemBufferCopy(Text, DocName);
    getLang().editorOpen(DocName, Buf.get(), Consumer, Args, None);
  }

  void close(const char *DocName) {
    getLang().editorClose(DocName, /*removeCache=*/false);
  }

  void replaceText(StringRef DocName, unsigned Offset, unsigned Length,
                   StringRef Text, EditorConsumer &Consumer) {
    auto Buf = MemoryBuffer::getMemBufferCopy(Text, DocName);
    getLang().editorReplaceText(DocName, Buf.get(), Offset, Length, Consumer);
  }

  unsigned findOffset(StringRef Val, StringRef Text) {
    auto pos = Text.find(Val);
    assert(pos != StringRef::npos);
    return pos;
  }

  void reset(TestConsumer &Consumer) {
    Consumer.Diags.clear();
    Consumer.DiagStage = UIdent();
    Consumer.Annotations.clear();
    std::unique_lock<std::mutex> lk(DocUpdState->Mtx);
    DocUpdState->HasUpdate = false;
  }

  void doubleOpenWithDelay(std::chrono::microseconds delay, bool close);

  void setupThreeAnnotations(const char *DocName, TestConsumer &Consumer) {
    // The following is engineered so that the references to `mem` are at
    // offsets 60, 70, and 80 for convenience. They're on the same line so
    // that tests do not accidentally depend on line separation.
    const char *Contents =
    "struct S {\n"
    "  var mem: Int = 0\n"
    "  func test() {\n"
    "          _ = mem;  _ = mem;  _ = mem\n"
    "  }\n"
    "}\n";
    const char *Args[] = { "-parse-as-library" };

    open(DocName, Contents, Args, Consumer);
    ASSERT_FALSE(waitForDocUpdate()) << "timed out";
    reset(Consumer);
  }

private:
  std::vector<const char *> makeArgs(const char *DocName,
                                     ArrayRef<const char *> CArgs) {
    std::vector<const char *> Args = CArgs;
    Args.push_back(DocName);
    return Args;
  }
};

} // anonymous namespace

static UIdent SemaDiagStage("source.diagnostic.stage.swift.sema");
static UIdent ParseDiagStage("source.diagnostic.stage.swift.parse");

TEST_F(EditTest, DiagsAfterEdit) {
  const char *DocName = "test.swift";
  const char *Contents =
    "func foo {}\n"
    "let v = 0\n";
  const char *Args[] = { "-parse-as-library" };

  TestConsumer Consumer;
  open(DocName, Contents, Args, Consumer);
  ASSERT_EQ(1u, Consumer.Diags.size());
  EXPECT_STREQ("expected '(' in argument list of function declaration", Consumer.Diags[0].Description.c_str());

  reset(Consumer);
  replaceText(DocName, findOffset("func foo", Contents)+strlen("func foo"), 0, "()", Consumer);
  EXPECT_TRUE(Consumer.Diags.empty());

  bool expired = waitForDocUpdate();
  ASSERT_FALSE(expired);

  reset(Consumer);
  replaceText(DocName, 0, 0, StringRef(), Consumer);
  EXPECT_TRUE(Consumer.Diags.empty());

  // If typecheck completed for the edit we'll get 'sema stage', otherwise
  // we'll get 'parser stage' and there will be another doc-update.
  if (Consumer.DiagStage == ParseDiagStage) {
    bool expired = waitForDocUpdate();
    ASSERT_FALSE(expired);

    reset(Consumer);
    replaceText(DocName, 0, 0, StringRef(), Consumer);
    EXPECT_TRUE(Consumer.Diags.empty());
  }
  EXPECT_EQ(SemaDiagStage, Consumer.DiagStage);
}

void EditTest::doubleOpenWithDelay(std::chrono::microseconds delay,
                                   bool closeDoc) {
  const char *DocName = "test.swift";
  const char *Contents =
    "func foo() { _ = unknown_name }\n";
  const char *Args[] = { "-parse-as-library" };

  TestConsumer Consumer;
  open(DocName, Contents, Args, Consumer);
  ASSERT_LE(Consumer.Diags.size(), 1u);
  if (Consumer.Diags.size() > 0) {
    EXPECT_EQ(SemaDiagStage, Consumer.DiagStage);
    Consumer.Diags.clear();
    Consumer.DiagStage = UIdent();
  }

  // Open again without closing; this reinitializes the semantic info on the doc
  if (delay > std::chrono::microseconds(0))
    std::this_thread::sleep_for(delay);
  if (closeDoc)
    close(DocName);
  reset(Consumer);

  open(DocName, Contents, Args, Consumer);
  ASSERT_LE(Consumer.Diags.size(), 1u);
  if (Consumer.Diags.size() > 0) {
    EXPECT_EQ(SemaDiagStage, Consumer.DiagStage);
    Consumer.Diags.clear();
    Consumer.DiagStage = UIdent();
  }

  // Wait for the document update from the second time we open the document. We
  // may or may not get a notification from the first time it was opened, but
  // only the second time will there be any semantic information available to
  // be queried, since the semantic info from the first open is unreachable.
  for (int i = 0; i < 2; ++i) {
    bool expired = waitForDocUpdate(/*reset=*/true);
    ASSERT_FALSE(expired) << "no " << (i ? "second " : "") << "notification";
    replaceText(DocName, 0, 0, StringRef(), Consumer);
    if (!Consumer.Diags.empty())
      break;
    ASSERT_EQ(0, i) << "no diagnostics after second notification";
  }

  ASSERT_EQ(1u, Consumer.Diags.size());
  EXPECT_STREQ("cannot find 'unknown_name' in scope", Consumer.Diags[0].Description.c_str());

  close(DocName);
}

// This test is failing occasionally in CI: rdar://45644449
TEST_F(EditTest, DISABLED_DiagsAfterCloseAndReopen) {
  // Attempt to open the same file twice in a row. This tests (subject to
  // timing) cases where:
  // * the 2nd open happens before the first AST starts building
  // * the 2nd open happens after the first AST starts building
  // * the 2nd open happens after the AST finishes

  // The middle case in particular verifies the ASTManager is only calling the
  // correct ASTConsumers.

  doubleOpenWithDelay(std::chrono::microseconds(0), true);
  doubleOpenWithDelay(std::chrono::milliseconds(1), true);
  doubleOpenWithDelay(std::chrono::milliseconds(10), true);
  doubleOpenWithDelay(std::chrono::milliseconds(100), true);
}

TEST_F(EditTest, DiagsAfterReopen) {
  // See description of DiagsAfterCloseAndReopen, but in this case we don't
  // close the original document, causing it to reinitialize instead of create
  // a fresh document.

  doubleOpenWithDelay(std::chrono::microseconds(0), false);
  doubleOpenWithDelay(std::chrono::milliseconds(1), false);
  doubleOpenWithDelay(std::chrono::milliseconds(10), false);
  doubleOpenWithDelay(std::chrono::milliseconds(100), false);
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const Token &Tok) {
  OS << "[off=" << Tok.Offset << " len=" << Tok.Length << " " << Tok.Kind.getName();
  if (Tok.IsSystem)
    OS << " system";
  OS << "]";
  return OS;
}

template<typename Collection>
std::string dumpStrings(const Collection &Annotations) {
  std::string tmp;
  llvm::raw_string_ostream OS(tmp);
  for (auto &tok : Annotations) {
    OS << tok << "\n";
  }
  return OS.str();
}

void checkTokens(llvm::ArrayRef<Token> Annotations, llvm::ArrayRef<const char *> Expected) {
  EXPECT_EQ(Annotations.size(), Expected.size()) <<
      dumpStrings(Annotations) << "  vs\n" << dumpStrings(Expected);
  if (Annotations.size() != Expected.size())
    return;

  for (unsigned i = 0, e = Annotations.size(); i != e; ++i) {
    std::string tok;
    {
      llvm::raw_string_ostream OS(tok);
      OS << Annotations[i];
    }
    EXPECT_EQ(tok, Expected[i]);
  }
}

TEST_F(EditTest, AnnotationsAfterOpen) {
  const char *DocName = "test.swift";
  const char *Contents =
    "struct S {\n"
    "  var mem: Int = 0\n"
    "  func test() {\n"
    "    _ = self.mem\n"
    "  }\n"
    "}\n";
  const char *Args[] = { "-parse-as-library" };

  TestConsumer Consumer;
  open(DocName, Contents, Args, Consumer);
  ASSERT_FALSE(waitForDocUpdate()) << "timed out";
  ASSERT_EQ(ParseDiagStage, Consumer.DiagStage);
  reset(Consumer);
  replaceText(DocName, 0, 0, "", Consumer);

  ASSERT_EQ(0u, Consumer.Diags.size());
  checkTokens(Consumer.Annotations, {
    "[off=22 len=3 source.lang.swift.ref.struct system]",
    "[off=59 len=3 source.lang.swift.ref.var.instance]",
  });

  reset(Consumer);
  replaceText(DocName, 0, 0, "", Consumer);
  // FIXME: we currently "take" the annotations instead of "get"ing them.
  EXPECT_EQ(0u, Consumer.Annotations.size());

  close(DocName);
}

TEST_F(EditTest, AnnotationsAfterEdit) {
  const char *DocName = "test.swift";
  const char *Contents =
    "struct S {\n"
    "  var mem: Int = 0\n"
    "  func test() {\n"
    "    _ = self.me\n"
    "  }\n"
    "}\n";
  const char *Args[] = { "-parse-as-library" };

  TestConsumer Consumer;
  open(DocName, Contents, Args, Consumer);
  ASSERT_FALSE(waitForDocUpdate()) << "timed out";
  ASSERT_EQ(ParseDiagStage, Consumer.DiagStage);
  reset(Consumer);
  replaceText(DocName, 0, 0, "", Consumer);
  checkTokens(Consumer.Annotations, {
    "[off=22 len=3 source.lang.swift.ref.struct system]",
  });

  reset(Consumer);
  replaceText(DocName, 61, 0, "m", Consumer);
  ASSERT_FALSE(waitForDocUpdate()) << "timed out";

  if (Consumer.DiagStage == SemaDiagStage) {
    // AST already built, annotations are on Consumer.
  } else {
    // Re-query.
    reset(Consumer);
    replaceText(DocName, 0, 0, "", Consumer);
  }

  ASSERT_EQ(0u, Consumer.Diags.size());
  checkTokens(Consumer.Annotations, {
    "[off=22 len=3 source.lang.swift.ref.struct system]",
    "[off=59 len=3 source.lang.swift.ref.var.instance]",
  });

  close(DocName);
}

TEST_F(EditTest, AnnotationsRangeShiftingAfterEditInsertStart) {
  const char *DocName = "test.swift";
  TestConsumer Consumer;
  setupThreeAnnotations(DocName, Consumer);
  replaceText(DocName, 70, 0, "X", Consumer);
  checkTokens(Consumer.Annotations, {
    "[off=22 len=3 source.lang.swift.ref.struct system]",
    "[off=60 len=3 source.lang.swift.ref.var.instance]",
    // Removed, touches edit.
    // "[off=70 len=3 source.lang.swift.ref.var.instance]",
    // Shifted by 1
    "[off=81 len=3 source.lang.swift.ref.var.instance]",
  });

  // Re-sync
  ASSERT_FALSE(waitForDocUpdate()) << "timed out";
  close(DocName);
}
TEST_F(EditTest, AnnotationsRangeShiftingAfterEditReplaceStart) {
  const char *DocName = "test.swift";
  TestConsumer Consumer;
  setupThreeAnnotations(DocName, Consumer);
  replaceText(DocName, 70, 1, "XYZ", Consumer);
  checkTokens(Consumer.Annotations, {
    "[off=22 len=3 source.lang.swift.ref.struct system]",
    "[off=60 len=3 source.lang.swift.ref.var.instance]",
    // Removed, touches edit.
    // "[off=70 len=3 source.lang.swift.ref.var.instance]",

    // Shifted 3-1 = 2
    "[off=82 len=3 source.lang.swift.ref.var.instance]",
  });

  // Re-sync
  ASSERT_FALSE(waitForDocUpdate()) << "timed out";
  close(DocName);
}
TEST_F(EditTest, AnnotationsRangeShiftingAfterEditDeleteStart) {
  const char *DocName = "test.swift";
  TestConsumer Consumer;
  setupThreeAnnotations(DocName, Consumer);
  replaceText(DocName, 70, 2, "", Consumer);
  checkTokens(Consumer.Annotations, {
    "[off=22 len=3 source.lang.swift.ref.struct system]",
    "[off=60 len=3 source.lang.swift.ref.var.instance]",
    // Removed, touches edit.
    // "[off=70 len=3 source.lang.swift.ref.var.instance]",

    // Shifted -2
    "[off=78 len=3 source.lang.swift.ref.var.instance]",
  });

  // Re-sync
  ASSERT_FALSE(waitForDocUpdate()) << "timed out";
  close(DocName);
}
TEST_F(EditTest, AnnotationsRangeShiftingAfterEditDeleteMiddle) {
  const char *DocName = "test.swift";
  TestConsumer Consumer;
  setupThreeAnnotations(DocName, Consumer);
  replaceText(DocName, 71, 2, "", Consumer);
  checkTokens(Consumer.Annotations, {
    "[off=22 len=3 source.lang.swift.ref.struct system]",
    "[off=60 len=3 source.lang.swift.ref.var.instance]",
    // Removed, touches edit.
    // "[off=70 len=3 source.lang.swift.ref.var.instance]",

    // Shifted -2
    "[off=78 len=3 source.lang.swift.ref.var.instance]",
  });

  // Re-sync
  ASSERT_FALSE(waitForDocUpdate()) << "timed out";
  close(DocName);
}
TEST_F(EditTest, AnnotationsRangeShiftingAfterEditInsertMiddle) {
  const char *DocName = "test.swift";
  TestConsumer Consumer;
  setupThreeAnnotations(DocName, Consumer);
  replaceText(DocName, 71, 0, "XY", Consumer);
  checkTokens(Consumer.Annotations, {
    "[off=22 len=3 source.lang.swift.ref.struct system]",
    "[off=60 len=3 source.lang.swift.ref.var.instance]",
    // Removed, touches edit.
    // "[off=70 len=3 source.lang.swift.ref.var.instance]",

    // Shifted 2
    "[off=82 len=3 source.lang.swift.ref.var.instance]",
  });

  // Re-sync
  ASSERT_FALSE(waitForDocUpdate()) << "timed out";
  close(DocName);
}
TEST_F(EditTest, AnnotationsRangeShiftingAfterEditInsertEnd) {
  const char *DocName = "test.swift";
  TestConsumer Consumer;
  setupThreeAnnotations(DocName, Consumer);
  replaceText(DocName, 73, 0, "X", Consumer);
  checkTokens(Consumer.Annotations, {
    "[off=22 len=3 source.lang.swift.ref.struct system]",
    "[off=60 len=3 source.lang.swift.ref.var.instance]",
    // Removed, touches edit.
    // "[off=70 len=3 source.lang.swift.ref.var.instance]",

    // Shifted 1
    "[off=81 len=3 source.lang.swift.ref.var.instance]",
  });

  // Re-sync
  ASSERT_FALSE(waitForDocUpdate()) << "timed out";
  close(DocName);
}
TEST_F(EditTest, AnnotationsRangeShiftingAfterEditReplaceEnd) {
  const char *DocName = "test.swift";
  TestConsumer Consumer;
  setupThreeAnnotations(DocName, Consumer);
  replaceText(DocName, 72, 1, "X", Consumer);
  checkTokens(Consumer.Annotations, {
    "[off=22 len=3 source.lang.swift.ref.struct system]",
    "[off=60 len=3 source.lang.swift.ref.var.instance]",
    // Removed, touches edit.
    // "[off=70 len=3 source.lang.swift.ref.var.instance]",

    "[off=80 len=3 source.lang.swift.ref.var.instance]",
  });

  // Re-sync
  ASSERT_FALSE(waitForDocUpdate()) << "timed out";
  close(DocName);
}
TEST_F(EditTest, AnnotationsRangeShiftingAfterEditDeleteEnd) {
  const char *DocName = "test.swift";
  TestConsumer Consumer;
  setupThreeAnnotations(DocName, Consumer);
  replaceText(DocName, 72, 1, "", Consumer);
  checkTokens(Consumer.Annotations, {
    "[off=22 len=3 source.lang.swift.ref.struct system]",
    "[off=60 len=3 source.lang.swift.ref.var.instance]",
    // Removed, touches edit.
    // "[off=70 len=3 source.lang.swift.ref.var.instance]",

    // Shifted -1
    "[off=79 len=3 source.lang.swift.ref.var.instance]",
  });

  // Re-sync
  ASSERT_FALSE(waitForDocUpdate()) << "timed out";
  close(DocName);
}

TEST_F(EditTest, AnnotationsRangeShiftingAfterEditLast) {
  const char *DocName = "test.swift";
  TestConsumer Consumer;
  setupThreeAnnotations(DocName, Consumer);
  replaceText(DocName, 80, 0, "X", Consumer);
  checkTokens(Consumer.Annotations, {
    "[off=22 len=3 source.lang.swift.ref.struct system]",
    "[off=60 len=3 source.lang.swift.ref.var.instance]",
    "[off=70 len=3 source.lang.swift.ref.var.instance]",
    // Removed, touches edit.
    // "[off=79 len=3 source.lang.swift.ref.var.instance]",
  });

  // Re-sync
  ASSERT_FALSE(waitForDocUpdate()) << "timed out";
  close(DocName);
}

TEST_F(EditTest, AnnotationsRangeShiftingAfterEditLast2) {
  const char *DocName = "test.swift";
  TestConsumer Consumer;
  setupThreeAnnotations(DocName, Consumer);
  replaceText(DocName, 83, 0, "X", Consumer);
  checkTokens(Consumer.Annotations, {
    "[off=22 len=3 source.lang.swift.ref.struct system]",
    "[off=60 len=3 source.lang.swift.ref.var.instance]",
    "[off=70 len=3 source.lang.swift.ref.var.instance]",
    // Removed, touches edit.
    // "[off=80 len=3 source.lang.swift.ref.var.instance]",
  });

  // Re-sync
  ASSERT_FALSE(waitForDocUpdate()) << "timed out";
  close(DocName);
}

TEST_F(EditTest, AnnotationsRangeShiftingAfterEditTouchMultiple) {
  const char *DocName = "test.swift";
  TestConsumer Consumer;
  setupThreeAnnotations(DocName, Consumer);
  replaceText(DocName, 63, 17, "X", Consumer);
  checkTokens(Consumer.Annotations, {
    "[off=22 len=3 source.lang.swift.ref.struct system]",
    // Removed, touches edit.
    // "[off=60 len=3 source.lang.swift.ref.var.instance]",
    // "[off=70 len=3 source.lang.swift.ref.var.instance]",
    // "[off=80 len=3 source.lang.swift.ref.var.instance]",
  });

  // Re-sync
  ASSERT_FALSE(waitForDocUpdate()) << "timed out";
  close(DocName);
}

TEST_F(EditTest, AnnotationsRangeShiftingAfterMultipleEdits) {
  const char *DocName = "test.swift";
  TestConsumer Consumer;
  setupThreeAnnotations(DocName, Consumer);
  replaceText(DocName, 83, 0, "X", Consumer);
  checkTokens(Consumer.Annotations, {
    "[off=22 len=3 source.lang.swift.ref.struct system]",
    "[off=60 len=3 source.lang.swift.ref.var.instance]",
    "[off=70 len=3 source.lang.swift.ref.var.instance]",
    // Removed, touches edit.
    // "[off=80 len=3 source.lang.swift.ref.var.instance]",
  });

  // Re-sync
  ASSERT_FALSE(waitForDocUpdate()) << "timed out";
  close(DocName);
}
