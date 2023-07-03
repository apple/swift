//===--- EditorPlaceholderTest.cpp ----------------------------------------===//
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

#include "swift/Basic/EditorPlaceholder.h"
#include "llvm/ADT/Optional.h"
#include "gtest/gtest.h"

using namespace swift;

TEST(EditorPlaceholder, EditorPlaceholders) {
  const char *Text = "<#this is simple#>";
  EditorPlaceholderData Data = *parseEditorPlaceholder(Text);
  EXPECT_EQ(EditorPlaceholderKind::Basic, Data.Kind);
  EXPECT_EQ("this is simple", Data.Display);
  EXPECT_TRUE(Data.Type.empty());
  EXPECT_TRUE(Data.TypeForExpansion.empty());

  Text = "<#T##x: Int##Int#>";
  Data = *parseEditorPlaceholder(Text);
  EXPECT_EQ(EditorPlaceholderKind::Typed, Data.Kind);
  EXPECT_EQ("x: Int", Data.Display);
  EXPECT_EQ("Int", Data.Type);
  EXPECT_EQ("Int", Data.TypeForExpansion);

  Text = "<#T##x: Int##Blah##()->Int#>";
  Data = *parseEditorPlaceholder(Text);
  EXPECT_EQ(EditorPlaceholderKind::Typed, Data.Kind);
  EXPECT_EQ("x: Int", Data.Display);
  EXPECT_EQ("Blah", Data.Type);
  EXPECT_EQ("()->Int", Data.TypeForExpansion);

  Text = "<#T##Int#>";
  Data = *parseEditorPlaceholder(Text);
  EXPECT_EQ(EditorPlaceholderKind::Typed, Data.Kind);
  EXPECT_EQ("Int", Data.Display);
  EXPECT_EQ("Int", Data.Type);
  EXPECT_EQ("Int", Data.TypeForExpansion);
}

TEST(EditorPlaceholder, InvalidEditorPlaceholders) {
  const char *Text = "<#foo";
  llvm::Optional<EditorPlaceholderData> DataOpt = parseEditorPlaceholder(Text);
  EXPECT_FALSE(DataOpt.has_value());

  Text = "foo#>";
  DataOpt = parseEditorPlaceholder(Text);
  EXPECT_FALSE(DataOpt.has_value());

  Text = "#foo#>";
  DataOpt = parseEditorPlaceholder(Text);
  EXPECT_FALSE(DataOpt.has_value());

  Text = " <#foo#>";
  DataOpt = parseEditorPlaceholder(Text);
  EXPECT_FALSE(DataOpt.has_value());
}
