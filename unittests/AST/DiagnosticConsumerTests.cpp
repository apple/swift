//===--- DiagnosticConsumerTests.cpp --------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/AST/DiagnosticConsumer.h"
#include "swift/Basic/Located.h"
#include "swift/Basic/SourceManager.h"
#include "gtest/gtest.h"

using namespace swift;

namespace {
  using ExpectedDiagnostic = Located<StringRef>;

  class ExpectationDiagnosticConsumer: public DiagnosticConsumer {
    ExpectationDiagnosticConsumer *previous;
    SmallVector<ExpectedDiagnostic, 4> expected;
    bool hasFinished = false;

  public:
    ExpectationDiagnosticConsumer(ExpectationDiagnosticConsumer *previous,
                                  ArrayRef<ExpectedDiagnostic> expected)
      : previous(previous), expected(expected.begin(), expected.end()) {}

    ~ExpectationDiagnosticConsumer() override {
      EXPECT_TRUE(hasFinished);
      EXPECT_TRUE(expected.empty());
    }

    void handleDiagnostic(SourceManager &SM,
                          const DiagnosticInfo &Info) override {
      ASSERT_FALSE(expected.empty());
      EXPECT_EQ(Located<StringRef>(Info.FormatString, Info.Loc), expected.front());
      expected.erase(expected.begin());
    }

    bool finishProcessing() override {
      EXPECT_FALSE(hasFinished);
      if (previous)
        EXPECT_TRUE(previous->hasFinished);
      hasFinished = true;
      return false;
    }
  };

  DiagnosticInfo testDiagnosticInfo(SourceLoc Loc, const char *Message,
                                    DiagnosticKind Kind) {
    return DiagnosticInfo(DiagID(0), Loc, Kind, Message, /*args*/ {},
                          /*category*/ StringRef(),
                          /*indirectBuffer*/ SourceLoc(), /*childInfo*/ {},
                          /*ranges*/ {}, /*fixIts*/ {}, /*isChild*/ false);
  }

} // end anonymous namespace

TEST(FileSpecificDiagnosticConsumer, SubconsumersFinishInOrder) {
  SourceManager sourceMgr;
  (void)sourceMgr.addMemBufferCopy("abcde", "A");
  (void)sourceMgr.addMemBufferCopy("vwxyz", "B");

  auto consumerA =
      std::make_unique<ExpectationDiagnosticConsumer>(nullptr, llvm::None);
  auto consumerUnaffiliated = std::make_unique<ExpectationDiagnosticConsumer>(
      consumerA.get(), llvm::None);

  SmallVector<FileSpecificDiagnosticConsumer::Subconsumer, 2> consumers;
  consumers.emplace_back("A", std::move(consumerA));
  consumers.emplace_back("", std::move(consumerUnaffiliated));

  auto topConsumer =
      FileSpecificDiagnosticConsumer::consolidateSubconsumers(consumers);
  topConsumer->finishProcessing();
}

TEST(FileSpecificDiagnosticConsumer, InvalidLocDiagsGoToEveryConsumer) {
  SourceManager sourceMgr;
  (void)sourceMgr.addMemBufferCopy("abcde", "A");
  (void)sourceMgr.addMemBufferCopy("vwxyz", "B");

  ExpectedDiagnostic expected[] = { Located<StringRef>("dummy", SourceLoc()) };
  auto consumerA = std::make_unique<ExpectationDiagnosticConsumer>(
      nullptr, expected);
  auto consumerUnaffiliated = std::make_unique<ExpectationDiagnosticConsumer>(
      consumerA.get(), expected);

  SmallVector<FileSpecificDiagnosticConsumer::Subconsumer, 2> consumers;
  consumers.emplace_back("A", std::move(consumerA));
  consumers.emplace_back("", std::move(consumerUnaffiliated));

  auto topConsumer =
      FileSpecificDiagnosticConsumer::consolidateSubconsumers(consumers);
  topConsumer->handleDiagnostic(
      sourceMgr,
      testDiagnosticInfo(SourceLoc(), "dummy", DiagnosticKind::Error));
  topConsumer->finishProcessing();
}

TEST(FileSpecificDiagnosticConsumer, ErrorsWithLocationsGoToExpectedConsumers) {
  SourceManager sourceMgr;
  //                                             01234
  unsigned bufferA = sourceMgr.addMemBufferCopy("abcde", "A");
  unsigned bufferB = sourceMgr.addMemBufferCopy("vwxyz", "B");

  SourceLoc frontOfA = sourceMgr.getLocForOffset(bufferA, 0);
  SourceLoc middleOfA = sourceMgr.getLocForOffset(bufferA, 2);
  SourceLoc backOfA = sourceMgr.getLocForOffset(bufferA, 4);

  SourceLoc frontOfB = sourceMgr.getLocForOffset(bufferB, 0);
  SourceLoc middleOfB = sourceMgr.getLocForOffset(bufferB, 2);
  SourceLoc backOfB = sourceMgr.getLocForOffset(bufferB, 4);

  ExpectedDiagnostic expectedA[] = {
    {"front", frontOfA},
    {"middle", middleOfA},
    {"back", backOfA},
  };
  ExpectedDiagnostic expectedB[] = {
    {"front", frontOfB},
    {"middle", middleOfB},
    {"back", backOfB}
  };

  auto consumerA = std::make_unique<ExpectationDiagnosticConsumer>(
      nullptr, expectedA);
  auto consumerB = std::make_unique<ExpectationDiagnosticConsumer>(
      consumerA.get(), expectedB);

  SmallVector<FileSpecificDiagnosticConsumer::Subconsumer, 2> consumers;
  consumers.emplace_back("A", std::move(consumerA));
  consumers.emplace_back("B", std::move(consumerB));

  auto topConsumer =
      FileSpecificDiagnosticConsumer::consolidateSubconsumers(consumers);
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(frontOfA, "front", DiagnosticKind::Error));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(frontOfB, "front", DiagnosticKind::Error));
  topConsumer->handleDiagnostic(
      sourceMgr,
      testDiagnosticInfo(middleOfA, "middle", DiagnosticKind::Error));
  topConsumer->handleDiagnostic(
      sourceMgr,
      testDiagnosticInfo(middleOfB, "middle", DiagnosticKind::Error));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(backOfA, "back", DiagnosticKind::Error));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(backOfB, "back", DiagnosticKind::Error));
  topConsumer->finishProcessing();
}

TEST(FileSpecificDiagnosticConsumer,
     ErrorsInUnaffiliatedFilesGoToEveryConsumer) {
  SourceManager sourceMgr;
  //                                             01234
  unsigned bufferA = sourceMgr.addMemBufferCopy("abcde", "A");
  unsigned bufferB = sourceMgr.addMemBufferCopy("vwxyz", "B");

  SourceLoc frontOfA = sourceMgr.getLocForOffset(bufferA, 0);
  SourceLoc middleOfA = sourceMgr.getLocForOffset(bufferA, 2);
  SourceLoc backOfA = sourceMgr.getLocForOffset(bufferA, 4);

  SourceLoc frontOfB = sourceMgr.getLocForOffset(bufferB, 0);
  SourceLoc middleOfB = sourceMgr.getLocForOffset(bufferB, 2);
  SourceLoc backOfB = sourceMgr.getLocForOffset(bufferB, 4);

  ExpectedDiagnostic expectedA[] = {
    {"front", frontOfA},
    {"front", frontOfB},
    {"middle", middleOfA},
    {"middle", middleOfB},
    {"back", backOfA},
    {"back", backOfB}
  };
  ExpectedDiagnostic expectedUnaffiliated[] = {
    {"front", frontOfB},
    {"middle", middleOfB},
    {"back", backOfB}
  };

  auto consumerA = std::make_unique<ExpectationDiagnosticConsumer>(
      nullptr, expectedA);
  auto consumerUnaffiliated = std::make_unique<ExpectationDiagnosticConsumer>(
      consumerA.get(), expectedUnaffiliated);

  SmallVector<FileSpecificDiagnosticConsumer::Subconsumer, 2> consumers;
  consumers.emplace_back("A", std::move(consumerA));
  consumers.emplace_back("", std::move(consumerUnaffiliated));

  auto topConsumer =
      FileSpecificDiagnosticConsumer::consolidateSubconsumers(consumers);
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(frontOfA, "front", DiagnosticKind::Error));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(frontOfB, "front", DiagnosticKind::Error));
  topConsumer->handleDiagnostic(
      sourceMgr,
      testDiagnosticInfo(middleOfA, "middle", DiagnosticKind::Error));
  topConsumer->handleDiagnostic(
      sourceMgr,
      testDiagnosticInfo(middleOfB, "middle", DiagnosticKind::Error));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(backOfA, "back", DiagnosticKind::Error));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(backOfB, "back", DiagnosticKind::Error));
  topConsumer->finishProcessing();
}

TEST(FileSpecificDiagnosticConsumer, WarningsAndRemarksAreTreatedLikeErrors) {
  SourceManager sourceMgr;
  //                                             01234
  unsigned bufferA = sourceMgr.addMemBufferCopy("abcde", "A");
  unsigned bufferB = sourceMgr.addMemBufferCopy("vwxyz", "B");

  SourceLoc frontOfA = sourceMgr.getLocForBufferStart(bufferA);
  SourceLoc frontOfB = sourceMgr.getLocForBufferStart(bufferB);

  ExpectedDiagnostic expectedA[] = {
    {"warning", frontOfA},
    {"warning", frontOfB},
    {"remark", frontOfA},
    {"remark", frontOfB},
  };
  ExpectedDiagnostic expectedUnaffiliated[] = {
    {"warning", frontOfB},
    {"remark", frontOfB},
  };

  auto consumerA = std::make_unique<ExpectationDiagnosticConsumer>(
      nullptr, expectedA);
  auto consumerUnaffiliated = std::make_unique<ExpectationDiagnosticConsumer>(
      consumerA.get(), expectedUnaffiliated);

  SmallVector<FileSpecificDiagnosticConsumer::Subconsumer, 2> consumers;
  consumers.emplace_back("A", std::move(consumerA));
  consumers.emplace_back("", std::move(consumerUnaffiliated));

  auto topConsumer =
      FileSpecificDiagnosticConsumer::consolidateSubconsumers(consumers);
  topConsumer->handleDiagnostic(
      sourceMgr,
      testDiagnosticInfo(frontOfA, "warning", DiagnosticKind::Warning));
  topConsumer->handleDiagnostic(
      sourceMgr,
      testDiagnosticInfo(frontOfB, "warning", DiagnosticKind::Warning));
  topConsumer->handleDiagnostic(
      sourceMgr,
      testDiagnosticInfo(frontOfA, "remark", DiagnosticKind::Remark));
  topConsumer->handleDiagnostic(
      sourceMgr,
      testDiagnosticInfo(frontOfB, "remark", DiagnosticKind::Remark));
  topConsumer->finishProcessing();
}

TEST(FileSpecificDiagnosticConsumer, NotesAreAttachedToErrors) {
  SourceManager sourceMgr;
  //                                             01234
  unsigned bufferA = sourceMgr.addMemBufferCopy("abcde", "A");
  unsigned bufferB = sourceMgr.addMemBufferCopy("vwxyz", "B");

  SourceLoc frontOfA = sourceMgr.getLocForOffset(bufferA, 0);
  SourceLoc middleOfA = sourceMgr.getLocForOffset(bufferA, 2);
  SourceLoc backOfA = sourceMgr.getLocForOffset(bufferA, 4);

  SourceLoc frontOfB = sourceMgr.getLocForOffset(bufferB, 0);
  SourceLoc middleOfB = sourceMgr.getLocForOffset(bufferB, 2);
  SourceLoc backOfB = sourceMgr.getLocForOffset(bufferB, 4);

  ExpectedDiagnostic expectedA[] = {
    {"error", frontOfA},
    {"note", middleOfA},
    {"note", backOfA},
    {"error", frontOfB},
    {"note", middleOfB},
    {"note", backOfB},
    {"error", frontOfA},
    {"note", middleOfA},
    {"note", backOfA},
  };
  ExpectedDiagnostic expectedUnaffiliated[] = {
    {"error", frontOfB},
    {"note", middleOfB},
    {"note", backOfB},
  };

  auto consumerA = std::make_unique<ExpectationDiagnosticConsumer>(
      nullptr, expectedA);
  auto consumerUnaffiliated = std::make_unique<ExpectationDiagnosticConsumer>(
      consumerA.get(), expectedUnaffiliated);

  SmallVector<FileSpecificDiagnosticConsumer::Subconsumer, 2> consumers;
  consumers.emplace_back("A", std::move(consumerA));
  consumers.emplace_back("", std::move(consumerUnaffiliated));

  auto topConsumer =
      FileSpecificDiagnosticConsumer::consolidateSubconsumers(consumers);
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(frontOfA, "error", DiagnosticKind::Error));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(middleOfA, "note", DiagnosticKind::Note));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(backOfA, "note", DiagnosticKind::Note));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(frontOfB, "error", DiagnosticKind::Error));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(middleOfB, "note", DiagnosticKind::Note));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(backOfB, "note", DiagnosticKind::Note));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(frontOfA, "error", DiagnosticKind::Error));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(middleOfA, "note", DiagnosticKind::Note));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(backOfA, "note", DiagnosticKind::Note));
  topConsumer->finishProcessing();
}

TEST(FileSpecificDiagnosticConsumer, NotesAreAttachedToWarningsAndRemarks) {
  SourceManager sourceMgr;
  //                                             01234
  unsigned bufferA = sourceMgr.addMemBufferCopy("abcde", "A");
  unsigned bufferB = sourceMgr.addMemBufferCopy("vwxyz", "B");

  SourceLoc frontOfA = sourceMgr.getLocForOffset(bufferA, 0);
  SourceLoc middleOfA = sourceMgr.getLocForOffset(bufferA, 2);
  SourceLoc backOfA = sourceMgr.getLocForOffset(bufferA, 4);

  SourceLoc frontOfB = sourceMgr.getLocForOffset(bufferB, 0);
  SourceLoc middleOfB = sourceMgr.getLocForOffset(bufferB, 2);
  SourceLoc backOfB = sourceMgr.getLocForOffset(bufferB, 4);

  ExpectedDiagnostic expectedA[] = {
    {"warning", frontOfA},
    {"note", middleOfA},
    {"note", backOfA},
    {"warning", frontOfB},
    {"note", middleOfB},
    {"note", backOfB},
    {"remark", frontOfA},
    {"note", middleOfA},
    {"note", backOfA},
  };
  ExpectedDiagnostic expectedUnaffiliated[] = {
    {"warning", frontOfB},
    {"note", middleOfB},
    {"note", backOfB},
  };

  auto consumerA = std::make_unique<ExpectationDiagnosticConsumer>(
      nullptr, expectedA);
  auto consumerUnaffiliated = std::make_unique<ExpectationDiagnosticConsumer>(
      consumerA.get(), expectedUnaffiliated);

  SmallVector<FileSpecificDiagnosticConsumer::Subconsumer, 2> consumers;
  consumers.emplace_back("A", std::move(consumerA));
  consumers.emplace_back("", std::move(consumerUnaffiliated));

  auto topConsumer =
      FileSpecificDiagnosticConsumer::consolidateSubconsumers(consumers);
  topConsumer->handleDiagnostic(
      sourceMgr,
      testDiagnosticInfo(frontOfA, "warning", DiagnosticKind::Warning));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(middleOfA, "note", DiagnosticKind::Note));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(backOfA, "note", DiagnosticKind::Note));
  topConsumer->handleDiagnostic(
      sourceMgr,
      testDiagnosticInfo(frontOfB, "warning", DiagnosticKind::Warning));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(middleOfB, "note", DiagnosticKind::Note));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(backOfB, "note", DiagnosticKind::Note));
  topConsumer->handleDiagnostic(
      sourceMgr,
      testDiagnosticInfo(frontOfA, "remark", DiagnosticKind::Remark));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(middleOfA, "note", DiagnosticKind::Note));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(backOfA, "note", DiagnosticKind::Note));
  topConsumer->finishProcessing();
}

TEST(FileSpecificDiagnosticConsumer, NotesAreAttachedToErrorsEvenAcrossFiles) {
  SourceManager sourceMgr;
  //                                             01234
  unsigned bufferA = sourceMgr.addMemBufferCopy("abcde", "A");
  unsigned bufferB = sourceMgr.addMemBufferCopy("vwxyz", "B");

  SourceLoc frontOfA = sourceMgr.getLocForOffset(bufferA, 0);
  SourceLoc middleOfA = sourceMgr.getLocForOffset(bufferA, 2);
  SourceLoc backOfA = sourceMgr.getLocForOffset(bufferA, 4);

  SourceLoc frontOfB = sourceMgr.getLocForOffset(bufferB, 0);
  SourceLoc middleOfB = sourceMgr.getLocForOffset(bufferB, 2);
  SourceLoc backOfB = sourceMgr.getLocForOffset(bufferB, 4);

  ExpectedDiagnostic expectedA[] = {
    {"error", frontOfA},
    {"note", middleOfB},
    {"note", backOfA},
    {"error", frontOfA},
    {"note", middleOfB},
    {"note", backOfA},
  };
  ExpectedDiagnostic expectedB[] = {
    {"error", frontOfB},
    {"note", middleOfA},
    {"note", backOfB},
  };

  auto consumerA = std::make_unique<ExpectationDiagnosticConsumer>(
      nullptr, expectedA);
  auto consumerB = std::make_unique<ExpectationDiagnosticConsumer>(
      consumerA.get(), expectedB);

  SmallVector<FileSpecificDiagnosticConsumer::Subconsumer, 2> consumers;
  consumers.emplace_back("A", std::move(consumerA));
  consumers.emplace_back("B", std::move(consumerB));

  auto topConsumer =
      FileSpecificDiagnosticConsumer::consolidateSubconsumers(consumers);
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(frontOfA, "error", DiagnosticKind::Error));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(middleOfB, "note", DiagnosticKind::Note));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(backOfA, "note", DiagnosticKind::Note));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(frontOfB, "error", DiagnosticKind::Error));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(middleOfA, "note", DiagnosticKind::Note));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(backOfB, "note", DiagnosticKind::Note));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(frontOfA, "error", DiagnosticKind::Error));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(middleOfB, "note", DiagnosticKind::Note));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(backOfA, "note", DiagnosticKind::Note));
  topConsumer->finishProcessing();
}

TEST(FileSpecificDiagnosticConsumer,
     NotesAreAttachedToErrorsEvenAcrossFilesWithUnaffiliatedConsumer) {
  SourceManager sourceMgr;
  //                                             01234
  unsigned bufferA = sourceMgr.addMemBufferCopy("abcde", "A");
  unsigned bufferB = sourceMgr.addMemBufferCopy("vwxyz", "B");

  SourceLoc frontOfA = sourceMgr.getLocForOffset(bufferA, 0);
  SourceLoc middleOfA = sourceMgr.getLocForOffset(bufferA, 2);
  SourceLoc backOfA = sourceMgr.getLocForOffset(bufferA, 4);

  SourceLoc frontOfB = sourceMgr.getLocForOffset(bufferB, 0);
  SourceLoc middleOfB = sourceMgr.getLocForOffset(bufferB, 2);
  SourceLoc backOfB = sourceMgr.getLocForOffset(bufferB, 4);

  ExpectedDiagnostic expectedA[] = {
    {"error", frontOfA},
    {"note", middleOfB},
    {"note", backOfA},
    {"error", frontOfB},
    {"note", middleOfA},
    {"note", backOfB},
    {"error", frontOfA},
    {"note", middleOfB},
    {"note", backOfA},
  };
  ExpectedDiagnostic expectedUnaffiliated[] = {
    {"error", frontOfB},
    {"note", middleOfA},
    {"note", backOfB},
  };

  auto consumerA = std::make_unique<ExpectationDiagnosticConsumer>(
      nullptr, expectedA);
  auto consumerUnaffiliated = std::make_unique<ExpectationDiagnosticConsumer>(
      consumerA.get(), expectedUnaffiliated);

  SmallVector<FileSpecificDiagnosticConsumer::Subconsumer, 2> consumers;
  consumers.emplace_back("A", std::move(consumerA));
  consumers.emplace_back("", std::move(consumerUnaffiliated));

  auto topConsumer =
      FileSpecificDiagnosticConsumer::consolidateSubconsumers(consumers);
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(frontOfA, "error", DiagnosticKind::Error));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(middleOfB, "note", DiagnosticKind::Note));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(backOfA, "note", DiagnosticKind::Note));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(frontOfB, "error", DiagnosticKind::Error));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(middleOfA, "note", DiagnosticKind::Note));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(backOfB, "note", DiagnosticKind::Note));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(frontOfA, "error", DiagnosticKind::Error));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(middleOfB, "note", DiagnosticKind::Note));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(backOfA, "note", DiagnosticKind::Note));
  topConsumer->finishProcessing();
}


TEST(FileSpecificDiagnosticConsumer,
     NotesWithInvalidLocsAreStillAttachedToErrors) {
  SourceManager sourceMgr;
  //                                             01234
  unsigned bufferA = sourceMgr.addMemBufferCopy("abcde", "A");
  unsigned bufferB = sourceMgr.addMemBufferCopy("vwxyz", "B");

  SourceLoc frontOfA = sourceMgr.getLocForBufferStart(bufferA);
  SourceLoc frontOfB = sourceMgr.getLocForBufferStart(bufferB);

  ExpectedDiagnostic expectedA[] = {
    {"error", frontOfA},
    {"note", SourceLoc()},
    {"error", frontOfB},
    {"note", SourceLoc()},
    {"error", frontOfA},
    {"note", SourceLoc()},
  };
  ExpectedDiagnostic expectedUnaffiliated[] = {
    {"error", frontOfB},
    {"note", SourceLoc()},
  };

  auto consumerA = std::make_unique<ExpectationDiagnosticConsumer>(
      nullptr, expectedA);
  auto consumerUnaffiliated = std::make_unique<ExpectationDiagnosticConsumer>(
      consumerA.get(), expectedUnaffiliated);

  SmallVector<FileSpecificDiagnosticConsumer::Subconsumer, 2> consumers;
  consumers.emplace_back("A", std::move(consumerA));
  consumers.emplace_back("", std::move(consumerUnaffiliated));

  auto topConsumer =
      FileSpecificDiagnosticConsumer::consolidateSubconsumers(consumers);
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(frontOfA, "error", DiagnosticKind::Error));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(SourceLoc(), "note", DiagnosticKind::Note));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(frontOfB, "error", DiagnosticKind::Error));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(SourceLoc(), "note", DiagnosticKind::Note));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(frontOfA, "error", DiagnosticKind::Error));
  topConsumer->handleDiagnostic(
      sourceMgr, testDiagnosticInfo(SourceLoc(), "note", DiagnosticKind::Note));
  topConsumer->finishProcessing();
}
