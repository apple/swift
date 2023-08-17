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

#include "swift/IDE/FuzzyStringMatcher.h"
#include "swift/Basic/Compiler.h"
#include "gtest/gtest.h"

using FuzzyStringMatcher = swift::ide::FuzzyStringMatcher;

TEST(FuzzyStringMatcher, BasicMatching) {
  {
    FuzzyStringMatcher m("ASDF");
    EXPECT_TRUE(m.matchesCandidate("ASDF"));
    EXPECT_TRUE(m.matchesCandidate("asdf"));
    EXPECT_TRUE(m.matchesCandidate("xASDF"));
    EXPECT_TRUE(m.matchesCandidate("ASDFX"));
    EXPECT_TRUE(m.matchesCandidate("ASDFX"));
    EXPECT_TRUE(m.matchesCandidate("a_s_d_f"));
    EXPECT_FALSE(m.matchesCandidate("asd"));
    EXPECT_FALSE(m.matchesCandidate(""));
  }
  {
    FuzzyStringMatcher m("asDf");
    EXPECT_TRUE(m.matchesCandidate("ASDF"));
    EXPECT_TRUE(m.matchesCandidate("asdf"));
    EXPECT_TRUE(m.matchesCandidate("xASDF"));
    EXPECT_TRUE(m.matchesCandidate("ASDFX"));
    EXPECT_TRUE(m.matchesCandidate("ASDFX"));
    EXPECT_TRUE(m.matchesCandidate("a_s_d_f"));
    EXPECT_FALSE(m.matchesCandidate("asd"));
    EXPECT_FALSE(m.matchesCandidate(""));
  }
}

TEST(FuzzyStringMatcher, SingleCharacterMatching) {
  EXPECT_TRUE(FuzzyStringMatcher("A").matchesCandidate("a"));
  EXPECT_TRUE(FuzzyStringMatcher("a").matchesCandidate("a"));
  EXPECT_FALSE(FuzzyStringMatcher("a").matchesCandidate("b"));
  EXPECT_FALSE(FuzzyStringMatcher("A").matchesCandidate("b"));
  EXPECT_TRUE(FuzzyStringMatcher("a").matchesCandidate("bA"));
  EXPECT_TRUE(FuzzyStringMatcher("A").matchesCandidate("bA"));
  EXPECT_TRUE(FuzzyStringMatcher("A").matchesCandidate("ba"));
  EXPECT_FALSE(FuzzyStringMatcher("a").matchesCandidate(""));
}

TEST(FuzzyStringMatcher, UnicodeMatching) {
  // Single code point matching.
  EXPECT_TRUE(FuzzyStringMatcher(SWIFT_UTF8("\u2602a\U0002000Bz"))
                  .matchesCandidate(SWIFT_UTF8("\u2602A\U0002000BZ")));

  // Same-order combining marks.
  EXPECT_TRUE(FuzzyStringMatcher(SWIFT_UTF8("a\u0323\u0307"))
                  .matchesCandidate(SWIFT_UTF8("A\u0323\u0307")));

  // FIXME: Canonical equivalence. These should be the same.
  EXPECT_FALSE(FuzzyStringMatcher(SWIFT_UTF8("a\u0307\u0323"))
                   .matchesCandidate(SWIFT_UTF8("A\u0323\u0307")));
  EXPECT_FALSE(FuzzyStringMatcher(SWIFT_UTF8("a\u00C5"))
                   .matchesCandidate(SWIFT_UTF8("A\u030A")));

  // FIXME: Compatibility equivalence.  It would be good to make these the same
  // too, since we're fuzzy matching.
  EXPECT_FALSE(FuzzyStringMatcher(SWIFT_UTF8("fi"))
                   .matchesCandidate(SWIFT_UTF8("\uFB01")));
  EXPECT_FALSE(FuzzyStringMatcher(SWIFT_UTF8("25"))
                   .matchesCandidate(SWIFT_UTF8("2\u2075")));

  // FIXME: Case-insensitivity in non-ASCII characters.
  EXPECT_FALSE(FuzzyStringMatcher(SWIFT_UTF8("\u00E0"))
                   .matchesCandidate(SWIFT_UTF8("\u00C0")));
  EXPECT_FALSE(FuzzyStringMatcher(SWIFT_UTF8("ss"))
                   .matchesCandidate(SWIFT_UTF8("\u00DF")));
}

TEST(FuzzyStringMatcher, BasicScoring) {
  FuzzyStringMatcher m("ASDF");
  EXPECT_GT(m.scoreCandidate("ASDF"), m.scoreCandidate("ASDF_"));  // exact
  EXPECT_GT(m.scoreCandidate("ASDF"), m.scoreCandidate("asdf"));   // case
  EXPECT_GT(m.scoreCandidate("asdF"), m.scoreCandidate("asdf"));   // case
  EXPECT_GT(m.scoreCandidate("asdfz"), m.scoreCandidate("zasdf")); // earlier
}

TEST(FuzzyStringMatcher, BestMatchNotFirstMatch) {
  {
    FuzzyStringMatcher m("AS");
    EXPECT_GT(m.scoreCandidate("abs_as"), m.scoreCandidate("abs_xx"));
  }
  {
    FuzzyStringMatcher m("ASDF");
    EXPECT_GT(m.scoreCandidate("absadef_asdf"),
              m.scoreCandidate("absadef_xxxx"));
    EXPECT_GT(m.scoreCandidate("asdf_ASDF"), m.scoreCandidate("asdf_asdf"));
  }
}

TEST(FuzzyStringMatcher, CamelCaseScoring) {
  // Camel case matches should have higher priority.
  {
    FuzzyStringMatcher m("mkav");
    EXPECT_GT(m.scoreCandidate("MKAnnotationView"),
              m.scoreCandidate("MKMapView"));
  }
  {
    FuzzyStringMatcher m("MKAV");
    EXPECT_GT(m.scoreCandidate("MKAnnotationView"),
              m.scoreCandidate("MKMapView"));
  }
  {
    FuzzyStringMatcher m("moc");
    EXPECT_GT(m.scoreCandidate("NSManagedObjectContext"),
              m.scoreCandidate("NSManagedobjectcontext"));

    EXPECT_GT(m.scoreCandidate("my_one_cat"), m.scoreCandidate("myonecat"));
    EXPECT_GT(m.scoreCandidate("my_one_cat"), m.scoreCandidate("aa_bbb_moc"));
    EXPECT_GT(m.scoreCandidate("my_one_cat"),
              m.scoreCandidate("not_my_one_cat"));

    EXPECT_GT(m.scoreCandidate("NSManagedObject+Context"),
              m.scoreCandidate("NSManagedobjectcontext"));
    EXPECT_GT(m.scoreCandidate("NSManagedObject-Context"),
              m.scoreCandidate("NSManagedobjectcontext"));
    EXPECT_GT(m.scoreCandidate("NSManagedObjectContextaaa"),
              m.scoreCandidate("NSManagedobjectcontextmoc"));
  }
  {
    FuzzyStringMatcher m("m_o_c");
    EXPECT_GT(m.scoreCandidate("my_one_cat"),
              m.scoreCandidate("not_my_one_cat"));
    EXPECT_GT(m.scoreCandidate("my_one_cat"), m.scoreCandidate("my_eno_cat"));
    EXPECT_GT(m.scoreCandidate("my_one_cat"),
              m.scoreCandidate("not_my_eno_cat"));
  }
  {
    FuzzyStringMatcher m("nsad");
    EXPECT_GT(m.scoreCandidate("NSAppDelegate"),
              m.scoreCandidate("NSappdelegate"));
  }
  {
    FuzzyStringMatcher m("mws");
    EXPECT_GT(m.scoreCandidate("methodWithSelector:"),
              m.scoreCandidate("methodwishes:"));
  }
  {
    FuzzyStringMatcher m("cancelDownload");
    EXPECT_GT(m.scoreCandidate("cancelDownload"),
              m.scoreCandidate("canCancelDownload"));
  }
  {
    FuzzyStringMatcher m("cancelDownload");
    EXPECT_GT(m.scoreCandidate("cancelDownload"),
              m.scoreCandidate("cancelDownload:"));
  }
  {
    FuzzyStringMatcher m("cancelDownload");
    EXPECT_GT(m.scoreCandidate("cancelDownload"),
              m.scoreCandidate("setCanCancelDownload"));
  }
  {
    FuzzyStringMatcher m("cancelDownload");
    EXPECT_GT(m.scoreCandidate("cancelDownload"),
              m.scoreCandidate("setCancelDownloadURL"));
  }
}

TEST(FuzzyStringMatcher, LongerRunsInLongerCandidates) {
  {
    FuzzyStringMatcher m("ABCo");
    EXPECT_GT(m.scoreCandidate("ABCooldown"),
              m.scoreCandidate("ABCeedIcon"));
  }
  {
    FuzzyStringMatcher m("hasDet");
    EXPECT_GT(m.scoreCandidate("hasDetachedOccurrences"),
              m.scoreCandidate("bHasDesktopDoodle"));
  }
  {
    FuzzyStringMatcher m("ABProxim");
    EXPECT_GT(m.scoreCandidate("ABProximitySensorFoo"),
              m.scoreCandidate("ABProxyThatVim"));
  }
  {
    FuzzyStringMatcher m("UIControl");
    EXPECT_GT(m.scoreCandidate("UIControl"),
              m.scoreCandidate("ABUIController"));
  }
}

TEST(FuzzyStringMatcher, ShorterMatches) {
  {
    FuzzyStringMatcher m("calayer");
    EXPECT_GT(m.scoreCandidate("CALayer"), m.scoreCandidate("CALayer_Utility"));
    EXPECT_GT(m.scoreCandidate("CALayer"), m.scoreCandidate("CALayerHost"));
    EXPECT_GT(m.scoreCandidate("CALayer"), m.scoreCandidate("CALayerPrivate"));
  }
  {
    FuzzyStringMatcher m("NSFileMan");
    EXPECT_GT(m.scoreCandidate("NSFileManager"),
              m.scoreCandidate("NSFileManagerAdditions"));
    EXPECT_GT(m.scoreCandidate("NSFileManager"),
              m.scoreCandidate("WebNSFileManagerExtras"));
    EXPECT_GT(m.scoreCandidate("NSFileManager"),
              m.scoreCandidate("NSFileManagerAdditions"));
  }
  {
    FuzzyStringMatcher m("NSUR");
    EXPECT_GT(m.scoreCandidate("NSURL"), m.scoreCandidate("NSURLRequest"));
  }
  {
    FuzzyStringMatcher m("NSCac");
    EXPECT_GT(m.scoreCandidate("NSCache"),
              m.scoreCandidate("NSCachedImageRep"));
  }
}

TEST(FuzzyStringMatcher, SingleCharacterScoring) {
  { // Case sensitive uppercase first character.
    FuzzyStringMatcher m("A");
    EXPECT_GT(m.scoreCandidate("Aa"), m.scoreCandidate("aa"));
  }
  { // Match at start.
    FuzzyStringMatcher m("a");
    EXPECT_GT(m.scoreCandidate("ab"), m.scoreCandidate("ba"));
  }
  { // FIXME: non-first character matches are all the same.
    FuzzyStringMatcher m("A");
    EXPECT_EQ(m.scoreCandidate("bA"), m.scoreCandidate("ba"));
  }
  { // FIXME: non-matches are the same as non-first-character matches.
    FuzzyStringMatcher m("a");
    EXPECT_EQ(m.scoreCandidate("ba"), m.scoreCandidate("bb"));
    EXPECT_TRUE(m.matchesCandidate("ba"));
    EXPECT_FALSE(m.matchesCandidate("bb"));
  }
}

TEST(FuzzyStringMatcher, ScoringOddities) {
  { // foo doesn't actually match because we jump straight to the last 'O'.
    FuzzyStringMatcher m1("foo");
    FuzzyStringMatcher m2("el");
    EXPECT_GT(m2.scoreCandidate("felDodO"), m1.scoreCandidate("felDodO"));
  }

  { // The characters after "." are not counted as part of the % score.
    FuzzyStringMatcher m("st");
    EXPECT_EQ(m.scoreCandidate("st.A"), m.scoreCandidate("st.AAAAAAAA"));
  }

  {
    FuzzyStringMatcher m1("k_");
    FuzzyStringMatcher m2("ka");
    EXPECT_GE(m1.scoreCandidate("KU_KA"), m2.scoreCandidate("KU_KA"));
  }
  { // The _ after the k seems to have an effect.
    FuzzyStringMatcher m1("_k");
    FuzzyStringMatcher m2("_w");
    EXPECT_GT(m1.scoreCandidate("A_WO_K_"), m2.scoreCandidate("A_WO_K_"));
  }
}

TEST(FuzzyStringMatcher, NormalizeSingleCharacterScore) {
  FuzzyStringMatcher m("A");
  m.normalize = true;
  EXPECT_EQ(1.0, m.scoreCandidate("A"));
  EXPECT_EQ(0.0, m.scoreCandidate("B"));
  EXPECT_GT(1.0, m.scoreCandidate("AB"));
  EXPECT_LT(0.0, m.scoreCandidate("AB"));
  EXPECT_GT(1.0, m.scoreCandidate("aB"));
  EXPECT_LT(0.0, m.scoreCandidate("aB"));
  EXPECT_GT(1.0, m.scoreCandidate("aBBBBBBBBBBBBBB"));
  EXPECT_LT(0.0, m.scoreCandidate("aBBBBBBBBBBBBBB"));
}

TEST(FuzzyStringMatcher, NormalizeScore) {
  FuzzyStringMatcher m("AB");
  m.normalize = true;
  EXPECT_DOUBLE_EQ(1.0, m.scoreCandidate("AB"));
  EXPECT_DOUBLE_EQ(0.0, m.scoreCandidate("BB"));
  EXPECT_GT(1.0, m.scoreCandidate("ab"));
  EXPECT_LT(0.0, m.scoreCandidate("ab"));
  EXPECT_GT(1.0, m.scoreCandidate("ABB"));
  EXPECT_LT(0.0, m.scoreCandidate("ABB"));
  EXPECT_GT(1.0, m.scoreCandidate("AAB"));
  EXPECT_LT(0.0, m.scoreCandidate("AAB"));
  EXPECT_GT(1.0, m.scoreCandidate("AaBb"));
  EXPECT_LT(0.0, m.scoreCandidate("AaBb"));
  EXPECT_GT(1.0, m.scoreCandidate("Aabb"));
  EXPECT_LT(0.0, m.scoreCandidate("Aabb"));

  FuzzyStringMatcher n("abc");
  n.normalize = true;
  EXPECT_DOUBLE_EQ(1.0, n.scoreCandidate("abc"));
  EXPECT_DOUBLE_EQ(0.83333333333333337, n.scoreCandidate("ABC"));
}

TEST(FuzzyStringMatcher, TokenizingCharacters) {
  FuzzyStringMatcher m("ab");
  EXPECT_GT(m.scoreCandidate("a/b"), m.scoreCandidate("acb"));
  EXPECT_GT(m.scoreCandidate("a.b"), m.scoreCandidate("acb"));
  EXPECT_GT(m.scoreCandidate("a_b"), m.scoreCandidate("acb"));
  EXPECT_GT(m.scoreCandidate("a+b"), m.scoreCandidate("acb"));
  EXPECT_GT(m.scoreCandidate("a-b"), m.scoreCandidate("acb"));
  EXPECT_GT(m.scoreCandidate("a:b"), m.scoreCandidate("acb"));
  EXPECT_GT(m.scoreCandidate("a,b"), m.scoreCandidate("acb"));
  EXPECT_GT(m.scoreCandidate("a b"), m.scoreCandidate("acb"));
  EXPECT_GT(m.scoreCandidate("a(b"), m.scoreCandidate("acb"));
  EXPECT_GT(m.scoreCandidate("a)b"), m.scoreCandidate("acb"));
  EXPECT_GT(m.scoreCandidate("a!b"), m.scoreCandidate("acb"));
  EXPECT_GT(m.scoreCandidate("a?b"), m.scoreCandidate("acb"));
}

TEST(FuzzyStringMatcher, ShortUnconnectedMatches) {
  FuzzyStringMatcher m("abcd");
  EXPECT_GT(m.scoreCandidate("xaxbxcdxxxxxx"), m.scoreCandidate("xaxbxcxd"));
  EXPECT_GT(m.scoreCandidate("xaxbxc_d"), m.scoreCandidate("xaxbxcxd"));
}
