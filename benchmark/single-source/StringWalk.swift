//===--- StringWalk.swift -------------------------------------*- swift -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

////////////////////////////////////////////////////////////////////////////////
// WARNING: This file is manually generated from .gyb template and should not
// be directly modified. Instead, make changes to StringWalk.swift.gyb and run
// scripts/generate_harness/generate_harness.py to regenerate this file.
////////////////////////////////////////////////////////////////////////////////

//
// Test String iteration performance over a variety of workloads, languages,
// and symbols.
//

import TestsUtils

//
// Helper functionality
//

@inline(never) func count_unicodeScalars(_ s: String.UnicodeScalarView) {
  var count = 0
  for _ in s {
    count += 1
  }
  blackHole(count)
}
@inline(never) func count_characters(_ s: String) {
  var count = 0
  for _ in s {
    count += 1
  }
  blackHole(count)
}
@inline(never) func count_unicodeScalars_rev(
  _ s: ReversedCollection<String.UnicodeScalarView>
) {
  var count = 0
  for _ in s {
    count += 1
  }
  blackHole(count)
}
@inline(never) func count_characters_rev(
  _ s: ReversedCollection<String>
) {
  var count = 0
  for _ in s {
    count += 1
  }
  blackHole(count)
}
@inline(never) func count_characters_by_index(_ s: String) {
  var idx = s.startIndex
  while idx != s.endIndex {
    blackHole(s[idx])
    s.formIndex(after: &idx)
  }
}

@inline(never) func count_characters_by_index_rev(
    _ s: ReversedCollection<String>
  ) {
  var idx = s.endIndex
  s.formIndex(before: &idx)
  while idx != s.startIndex {
    blackHole(s[idx])
    s.formIndex(before: &idx)
  }
}

//
// Workloads
//
let ascii = String(
  repeating: "siebenhundertsiebenundsiebzigtausendsiebenhundertsiebenundsiebenzig",
  count: 50
)

let emoji = String(
  repeating: "👍👩‍👩‍👧‍👧👨‍👨‍👦‍👦🇺🇸🇨🇦🇲🇽👍🏻👍🏼👍🏽👍🏾👍🏿",
  count: 25
)

let utf16 = emoji + "the quick brown fox" + String(emoji.reversed())

let japanese = String(
  repeating: "今回のアップデートでSwiftに大幅な改良が施され、安定していてしかも直感的に使うことができるAppleプラットフォーム向けプログラミング言語になりました。",
  count: 50
)

let chinese = String(
  repeating: "Swift 是面向 Apple 平台的编程语言，功能强大且直观易用，而本次更新对其进行了全面优化。",
  count: 50
)

let korean = String(
  repeating: "이번 업데이트에서는 강력하면서도 직관적인 Apple 플랫폼용 프로그래밍 언어인 Swift를 완벽히 개선하였습니다.",
  count: 50
)

let russian = String(
  repeating: "в чащах юга жил-был цитрус? да, но фальшивый экземпляр",
  count: 50
)

let punctuated = String(
  repeating: "\u{201c}Hello\u{2010}world\u{2026}\u{201d}",
  count: 50
)
let punctuatedJapanese = String(
  repeating: "\u{300c}\u{300e}今日は\u{3001}世界\u{3002}\u{300f}\u{300d}",
  count: 50
)

// A workload that's mostly Latin characters, with occasional emoji
// interspersed. Common for tweets.
let tweet = String(
  repeating: "Worst thing about working on String is that it breaks *everything*. Asserts, debuggers, and *especially* printf-style debugging 😭",
  count: 40
)

//
// Benchmarks
//

// Pre-commit benchmark: simple scalar walk
@inline(never)
public func run_StringWalk(_ n: Int) {
  return run_StringWalk_ascii_unicodeScalars(n)
}

// Extended String benchmarks:
let unicodeScalarsMultiplier = 5
let stringWalkWarmupAmount = 5


// An extended benchmark suite exercising finer-granularity behavior of our
// Strings.
public let benchmarks = [
  BenchmarkInfo(
    name: "StringWalk",
    runFunction: run_StringWalk,
    tags: [.validation, .api, .String],
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_ascii_unicodeScalars",
    runFunction: run_StringWalk_ascii_unicodeScalars,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_ascii_unicodeScalars(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_ascii_characters",
    runFunction: run_StringWalk_ascii_characters,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_ascii_characters(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_ascii_characters_by_index",
    runFunction: run_StringWalk_ascii_characters_by_index,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_ascii_characters_by_index(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "CharIteration_ascii_unicodeScalars",
    runFunction: run_CharIteration_ascii_unicodeScalars,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_ascii_unicodeScalars",
    runFunction: run_CharIndexing_ascii_unicodeScalars,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "StringWalk_ascii_unicodeScalars_Backwards",
    runFunction: run_StringWalk_ascii_unicodeScalars_Backwards,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_ascii_unicodeScalars_Backwards(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_ascii_characters_Backwards",
    runFunction: run_StringWalk_ascii_characters_Backwards,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_ascii_characters_Backwards(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_ascii_characters_by_index_Backwards",
    runFunction: run_StringWalk_ascii_characters_by_index_Backwards,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_ascii_characters_by_index_Backwards(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "CharIteration_ascii_unicodeScalars_Backwards",
    runFunction: run_CharIteration_ascii_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_ascii_unicodeScalars_Backwards",
    runFunction: run_CharIndexing_ascii_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "StringWalk_utf16_unicodeScalars",
    runFunction: run_StringWalk_utf16_unicodeScalars,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_utf16_unicodeScalars(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_utf16_characters",
    runFunction: run_StringWalk_utf16_characters,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_utf16_characters(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_utf16_characters_by_index",
    runFunction: run_StringWalk_utf16_characters_by_index,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_utf16_characters_by_index(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "CharIteration_utf16_unicodeScalars",
    runFunction: run_CharIteration_utf16_unicodeScalars,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_utf16_unicodeScalars",
    runFunction: run_CharIndexing_utf16_unicodeScalars,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "StringWalk_utf16_unicodeScalars_Backwards",
    runFunction: run_StringWalk_utf16_unicodeScalars_Backwards,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_utf16_unicodeScalars_Backwards(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_utf16_characters_Backwards",
    runFunction: run_StringWalk_utf16_characters_Backwards,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_utf16_characters_Backwards(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_utf16_characters_by_index_Backwards",
    runFunction: run_StringWalk_utf16_characters_by_index_Backwards,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_utf16_characters_by_index_Backwards(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "CharIteration_utf16_unicodeScalars_Backwards",
    runFunction: run_CharIteration_utf16_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_utf16_unicodeScalars_Backwards",
    runFunction: run_CharIndexing_utf16_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "StringWalk_tweet_unicodeScalars",
    runFunction: run_StringWalk_tweet_unicodeScalars,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_tweet_unicodeScalars(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_tweet_characters",
    runFunction: run_StringWalk_tweet_characters,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_tweet_characters(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_tweet_characters_by_index",
    runFunction: run_StringWalk_tweet_characters_by_index,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_tweet_characters_by_index(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "CharIteration_tweet_unicodeScalars",
    runFunction: run_CharIteration_tweet_unicodeScalars,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_tweet_unicodeScalars",
    runFunction: run_CharIndexing_tweet_unicodeScalars,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "StringWalk_tweet_unicodeScalars_Backwards",
    runFunction: run_StringWalk_tweet_unicodeScalars_Backwards,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_tweet_unicodeScalars_Backwards(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_tweet_characters_Backwards",
    runFunction: run_StringWalk_tweet_characters_Backwards,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_tweet_characters_Backwards(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_tweet_characters_by_index_Backwards",
    runFunction: run_StringWalk_tweet_characters_by_index_Backwards,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_tweet_characters_by_index_Backwards(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "CharIteration_tweet_unicodeScalars_Backwards",
    runFunction: run_CharIteration_tweet_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_tweet_unicodeScalars_Backwards",
    runFunction: run_CharIndexing_tweet_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "StringWalk_japanese_unicodeScalars",
    runFunction: run_StringWalk_japanese_unicodeScalars,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_japanese_unicodeScalars(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_japanese_characters",
    runFunction: run_StringWalk_japanese_characters,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_japanese_characters(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_japanese_characters_by_index",
    runFunction: run_StringWalk_japanese_characters_by_index,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_japanese_characters_by_index(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "CharIteration_japanese_unicodeScalars",
    runFunction: run_CharIteration_japanese_unicodeScalars,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_japanese_unicodeScalars",
    runFunction: run_CharIndexing_japanese_unicodeScalars,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "StringWalk_japanese_unicodeScalars_Backwards",
    runFunction: run_StringWalk_japanese_unicodeScalars_Backwards,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_japanese_unicodeScalars_Backwards(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_japanese_characters_Backwards",
    runFunction: run_StringWalk_japanese_characters_Backwards,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_japanese_characters_Backwards(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_japanese_characters_by_index_Backwards",
    runFunction: run_StringWalk_japanese_characters_by_index_Backwards,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_japanese_characters_by_index_Backwards(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "CharIteration_japanese_unicodeScalars_Backwards",
    runFunction: run_CharIteration_japanese_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_japanese_unicodeScalars_Backwards",
    runFunction: run_CharIndexing_japanese_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "StringWalk_chinese_unicodeScalars",
    runFunction: run_StringWalk_chinese_unicodeScalars,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_chinese_unicodeScalars(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_chinese_characters",
    runFunction: run_StringWalk_chinese_characters,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_chinese_characters(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_chinese_characters_by_index",
    runFunction: run_StringWalk_chinese_characters_by_index,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_chinese_characters_by_index(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "CharIteration_chinese_unicodeScalars",
    runFunction: run_CharIteration_chinese_unicodeScalars,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_chinese_unicodeScalars",
    runFunction: run_CharIndexing_chinese_unicodeScalars,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "StringWalk_chinese_unicodeScalars_Backwards",
    runFunction: run_StringWalk_chinese_unicodeScalars_Backwards,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_chinese_unicodeScalars_Backwards(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_chinese_characters_Backwards",
    runFunction: run_StringWalk_chinese_characters_Backwards,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_chinese_characters_Backwards(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_chinese_characters_by_index_Backwards",
    runFunction: run_StringWalk_chinese_characters_by_index_Backwards,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_chinese_characters_by_index_Backwards(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "CharIteration_chinese_unicodeScalars_Backwards",
    runFunction: run_CharIteration_chinese_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_chinese_unicodeScalars_Backwards",
    runFunction: run_CharIndexing_chinese_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "StringWalk_korean_unicodeScalars",
    runFunction: run_StringWalk_korean_unicodeScalars,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_korean_unicodeScalars(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_korean_characters",
    runFunction: run_StringWalk_korean_characters,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_korean_characters(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_korean_characters_by_index",
    runFunction: run_StringWalk_korean_characters_by_index,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_korean_characters_by_index(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "CharIteration_korean_unicodeScalars",
    runFunction: run_CharIteration_korean_unicodeScalars,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_korean_unicodeScalars",
    runFunction: run_CharIndexing_korean_unicodeScalars,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "StringWalk_korean_unicodeScalars_Backwards",
    runFunction: run_StringWalk_korean_unicodeScalars_Backwards,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_korean_unicodeScalars_Backwards(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_korean_characters_Backwards",
    runFunction: run_StringWalk_korean_characters_Backwards,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_korean_characters_Backwards(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_korean_characters_by_index_Backwards",
    runFunction: run_StringWalk_korean_characters_by_index_Backwards,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_korean_characters_by_index_Backwards(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "CharIteration_korean_unicodeScalars_Backwards",
    runFunction: run_CharIteration_korean_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_korean_unicodeScalars_Backwards",
    runFunction: run_CharIndexing_korean_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "StringWalk_russian_unicodeScalars",
    runFunction: run_StringWalk_russian_unicodeScalars,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_russian_unicodeScalars(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_russian_characters",
    runFunction: run_StringWalk_russian_characters,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_russian_characters(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_russian_characters_by_index",
    runFunction: run_StringWalk_russian_characters_by_index,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_russian_characters_by_index(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "CharIteration_russian_unicodeScalars",
    runFunction: run_CharIteration_russian_unicodeScalars,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_russian_unicodeScalars",
    runFunction: run_CharIndexing_russian_unicodeScalars,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "StringWalk_russian_unicodeScalars_Backwards",
    runFunction: run_StringWalk_russian_unicodeScalars_Backwards,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_russian_unicodeScalars_Backwards(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_russian_characters_Backwards",
    runFunction: run_StringWalk_russian_characters_Backwards,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_russian_characters_Backwards(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_russian_characters_by_index_Backwards",
    runFunction: run_StringWalk_russian_characters_by_index_Backwards,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_russian_characters_by_index_Backwards(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "CharIteration_russian_unicodeScalars_Backwards",
    runFunction: run_CharIteration_russian_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_russian_unicodeScalars_Backwards",
    runFunction: run_CharIndexing_russian_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "StringWalk_punctuated_unicodeScalars",
    runFunction: run_StringWalk_punctuated_unicodeScalars,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_punctuated_unicodeScalars(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_punctuated_characters",
    runFunction: run_StringWalk_punctuated_characters,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_punctuated_characters(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_punctuated_characters_by_index",
    runFunction: run_StringWalk_punctuated_characters_by_index,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_punctuated_characters_by_index(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "CharIteration_punctuated_unicodeScalars",
    runFunction: run_CharIteration_punctuated_unicodeScalars,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_punctuated_unicodeScalars",
    runFunction: run_CharIndexing_punctuated_unicodeScalars,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "StringWalk_punctuated_unicodeScalars_Backwards",
    runFunction: run_StringWalk_punctuated_unicodeScalars_Backwards,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_punctuated_unicodeScalars_Backwards(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_punctuated_characters_Backwards",
    runFunction: run_StringWalk_punctuated_characters_Backwards,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_punctuated_characters_Backwards(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_punctuated_characters_by_index_Backwards",
    runFunction: run_StringWalk_punctuated_characters_by_index_Backwards,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_punctuated_characters_by_index_Backwards(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "CharIteration_punctuated_unicodeScalars_Backwards",
    runFunction: run_CharIteration_punctuated_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_punctuated_unicodeScalars_Backwards",
    runFunction: run_CharIndexing_punctuated_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "StringWalk_punctuatedJapanese_unicodeScalars",
    runFunction: run_StringWalk_punctuatedJapanese_unicodeScalars,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_punctuatedJapanese_unicodeScalars(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_punctuatedJapanese_characters",
    runFunction: run_StringWalk_punctuatedJapanese_characters,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_punctuatedJapanese_characters(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_punctuatedJapanese_characters_by_index",
    runFunction: run_StringWalk_punctuatedJapanese_characters_by_index,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_punctuatedJapanese_characters_by_index(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "CharIteration_punctuatedJapanese_unicodeScalars",
    runFunction: run_CharIteration_punctuatedJapanese_unicodeScalars,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_punctuatedJapanese_unicodeScalars",
    runFunction: run_CharIndexing_punctuatedJapanese_unicodeScalars,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "StringWalk_punctuatedJapanese_unicodeScalars_Backwards",
    runFunction: run_StringWalk_punctuatedJapanese_unicodeScalars_Backwards,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_punctuatedJapanese_unicodeScalars_Backwards(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_punctuatedJapanese_characters_Backwards",
    runFunction: run_StringWalk_punctuatedJapanese_characters_Backwards,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_punctuatedJapanese_characters_Backwards(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk_punctuatedJapanese_characters_by_index_Backwards",
    runFunction: run_StringWalk_punctuatedJapanese_characters_by_index_Backwards,
    tags: [.api, .String],
    setUpFunction: {run_StringWalk_punctuatedJapanese_characters_by_index_Backwards(stringWalkWarmupAmount)},
    legacyFactor: 40),


  BenchmarkInfo(
    name: "CharIteration_punctuatedJapanese_unicodeScalars_Backwards",
    runFunction: run_CharIteration_punctuatedJapanese_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_punctuatedJapanese_unicodeScalars_Backwards",
    runFunction: run_CharIndexing_punctuatedJapanese_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    legacyFactor: 40),
]


@inline(never)
public func run_StringWalk_ascii_unicodeScalars(_ n: Int) {
  for _ in 1...n {
    count_unicodeScalars(ascii.unicodeScalars)
  }
}

@inline(never)
public func run_StringWalk_ascii_unicodeScalars_Backwards(_ n: Int) {
  for _ in 1...n {
    count_unicodeScalars_rev(ascii.unicodeScalars.reversed())
  }
}


@inline(never)
public func run_StringWalk_ascii_characters(_ n: Int) {
  for _ in 1...n {
    count_characters(ascii)
  }
}

@inline(never)
public func run_StringWalk_ascii_characters_Backwards(_ n: Int) {
  for _ in 1...n {
    count_characters_rev(ascii.reversed())
  }
}


@inline(never)
public func run_StringWalk_ascii_characters_by_index(_ n: Int) {
  for _ in 1...n {
    count_characters_by_index(ascii)
  }
}

@inline(never)
public func run_StringWalk_ascii_characters_by_index_Backwards(_ n: Int) {
  for _ in 1...n {
    count_characters_by_index_rev(ascii.reversed())
  }
}


let asciiCharacters = Array(ascii)

@inline(never)
public func run_CharIteration_ascii_unicodeScalars(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in asciiCharacters {
      for u in c.unicodeScalars {
        count |= Int(u.value)
      }
    }
  }
  blackHole(count)
}

@inline(never)
public func run_CharIteration_ascii_unicodeScalars_Backwards(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in asciiCharacters {
      for u in c.unicodeScalars.reversed() {
        count |= Int(u.value)
      }
    }
  }
  blackHole(count)
}

@inline(never)
public func run_CharIndexing_ascii_unicodeScalars(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in asciiCharacters {
      let s = c.unicodeScalars
      for i in s.indices {
        count |= Int(s[i].value)
      }
    }
  }
  blackHole(count)
}

@inline(never)
public func run_CharIndexing_ascii_unicodeScalars_Backwards(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in asciiCharacters {
      let s = c.unicodeScalars
      for i in s.indices.reversed() {
        count |= Int(s[i].value)
      }
    }
  }
  blackHole(count)
}



@inline(never)
public func run_StringWalk_utf16_unicodeScalars(_ n: Int) {
  for _ in 1...n {
    count_unicodeScalars(utf16.unicodeScalars)
  }
}

@inline(never)
public func run_StringWalk_utf16_unicodeScalars_Backwards(_ n: Int) {
  for _ in 1...n {
    count_unicodeScalars_rev(utf16.unicodeScalars.reversed())
  }
}


@inline(never)
public func run_StringWalk_utf16_characters(_ n: Int) {
  for _ in 1...n {
    count_characters(utf16)
  }
}

@inline(never)
public func run_StringWalk_utf16_characters_Backwards(_ n: Int) {
  for _ in 1...n {
    count_characters_rev(utf16.reversed())
  }
}


@inline(never)
public func run_StringWalk_utf16_characters_by_index(_ n: Int) {
  for _ in 1...n {
    count_characters_by_index(utf16)
  }
}

@inline(never)
public func run_StringWalk_utf16_characters_by_index_Backwards(_ n: Int) {
  for _ in 1...n {
    count_characters_by_index_rev(utf16.reversed())
  }
}


let utf16Characters = Array(utf16)

@inline(never)
public func run_CharIteration_utf16_unicodeScalars(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in utf16Characters {
      for u in c.unicodeScalars {
        count |= Int(u.value)
      }
    }
  }
  blackHole(count)
}

@inline(never)
public func run_CharIteration_utf16_unicodeScalars_Backwards(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in utf16Characters {
      for u in c.unicodeScalars.reversed() {
        count |= Int(u.value)
      }
    }
  }
  blackHole(count)
}

@inline(never)
public func run_CharIndexing_utf16_unicodeScalars(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in utf16Characters {
      let s = c.unicodeScalars
      for i in s.indices {
        count |= Int(s[i].value)
      }
    }
  }
  blackHole(count)
}

@inline(never)
public func run_CharIndexing_utf16_unicodeScalars_Backwards(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in utf16Characters {
      let s = c.unicodeScalars
      for i in s.indices.reversed() {
        count |= Int(s[i].value)
      }
    }
  }
  blackHole(count)
}



@inline(never)
public func run_StringWalk_tweet_unicodeScalars(_ n: Int) {
  for _ in 1...n {
    count_unicodeScalars(tweet.unicodeScalars)
  }
}

@inline(never)
public func run_StringWalk_tweet_unicodeScalars_Backwards(_ n: Int) {
  for _ in 1...n {
    count_unicodeScalars_rev(tweet.unicodeScalars.reversed())
  }
}


@inline(never)
public func run_StringWalk_tweet_characters(_ n: Int) {
  for _ in 1...n {
    count_characters(tweet)
  }
}

@inline(never)
public func run_StringWalk_tweet_characters_Backwards(_ n: Int) {
  for _ in 1...n {
    count_characters_rev(tweet.reversed())
  }
}


@inline(never)
public func run_StringWalk_tweet_characters_by_index(_ n: Int) {
  for _ in 1...n {
    count_characters_by_index(tweet)
  }
}

@inline(never)
public func run_StringWalk_tweet_characters_by_index_Backwards(_ n: Int) {
  for _ in 1...n {
    count_characters_by_index_rev(tweet.reversed())
  }
}


let tweetCharacters = Array(tweet)

@inline(never)
public func run_CharIteration_tweet_unicodeScalars(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in tweetCharacters {
      for u in c.unicodeScalars {
        count |= Int(u.value)
      }
    }
  }
  blackHole(count)
}

@inline(never)
public func run_CharIteration_tweet_unicodeScalars_Backwards(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in tweetCharacters {
      for u in c.unicodeScalars.reversed() {
        count |= Int(u.value)
      }
    }
  }
  blackHole(count)
}

@inline(never)
public func run_CharIndexing_tweet_unicodeScalars(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in tweetCharacters {
      let s = c.unicodeScalars
      for i in s.indices {
        count |= Int(s[i].value)
      }
    }
  }
  blackHole(count)
}

@inline(never)
public func run_CharIndexing_tweet_unicodeScalars_Backwards(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in tweetCharacters {
      let s = c.unicodeScalars
      for i in s.indices.reversed() {
        count |= Int(s[i].value)
      }
    }
  }
  blackHole(count)
}



@inline(never)
public func run_StringWalk_japanese_unicodeScalars(_ n: Int) {
  for _ in 1...n {
    count_unicodeScalars(japanese.unicodeScalars)
  }
}

@inline(never)
public func run_StringWalk_japanese_unicodeScalars_Backwards(_ n: Int) {
  for _ in 1...n {
    count_unicodeScalars_rev(japanese.unicodeScalars.reversed())
  }
}


@inline(never)
public func run_StringWalk_japanese_characters(_ n: Int) {
  for _ in 1...n {
    count_characters(japanese)
  }
}

@inline(never)
public func run_StringWalk_japanese_characters_Backwards(_ n: Int) {
  for _ in 1...n {
    count_characters_rev(japanese.reversed())
  }
}


@inline(never)
public func run_StringWalk_japanese_characters_by_index(_ n: Int) {
  for _ in 1...n {
    count_characters_by_index(japanese)
  }
}

@inline(never)
public func run_StringWalk_japanese_characters_by_index_Backwards(_ n: Int) {
  for _ in 1...n {
    count_characters_by_index_rev(japanese.reversed())
  }
}


let japaneseCharacters = Array(japanese)

@inline(never)
public func run_CharIteration_japanese_unicodeScalars(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in japaneseCharacters {
      for u in c.unicodeScalars {
        count |= Int(u.value)
      }
    }
  }
  blackHole(count)
}

@inline(never)
public func run_CharIteration_japanese_unicodeScalars_Backwards(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in japaneseCharacters {
      for u in c.unicodeScalars.reversed() {
        count |= Int(u.value)
      }
    }
  }
  blackHole(count)
}

@inline(never)
public func run_CharIndexing_japanese_unicodeScalars(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in japaneseCharacters {
      let s = c.unicodeScalars
      for i in s.indices {
        count |= Int(s[i].value)
      }
    }
  }
  blackHole(count)
}

@inline(never)
public func run_CharIndexing_japanese_unicodeScalars_Backwards(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in japaneseCharacters {
      let s = c.unicodeScalars
      for i in s.indices.reversed() {
        count |= Int(s[i].value)
      }
    }
  }
  blackHole(count)
}



@inline(never)
public func run_StringWalk_chinese_unicodeScalars(_ n: Int) {
  for _ in 1...n {
    count_unicodeScalars(chinese.unicodeScalars)
  }
}

@inline(never)
public func run_StringWalk_chinese_unicodeScalars_Backwards(_ n: Int) {
  for _ in 1...n {
    count_unicodeScalars_rev(chinese.unicodeScalars.reversed())
  }
}


@inline(never)
public func run_StringWalk_chinese_characters(_ n: Int) {
  for _ in 1...n {
    count_characters(chinese)
  }
}

@inline(never)
public func run_StringWalk_chinese_characters_Backwards(_ n: Int) {
  for _ in 1...n {
    count_characters_rev(chinese.reversed())
  }
}


@inline(never)
public func run_StringWalk_chinese_characters_by_index(_ n: Int) {
  for _ in 1...n {
    count_characters_by_index(chinese)
  }
}

@inline(never)
public func run_StringWalk_chinese_characters_by_index_Backwards(_ n: Int) {
  for _ in 1...n {
    count_characters_by_index_rev(chinese.reversed())
  }
}


let chineseCharacters = Array(chinese)

@inline(never)
public func run_CharIteration_chinese_unicodeScalars(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in chineseCharacters {
      for u in c.unicodeScalars {
        count |= Int(u.value)
      }
    }
  }
  blackHole(count)
}

@inline(never)
public func run_CharIteration_chinese_unicodeScalars_Backwards(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in chineseCharacters {
      for u in c.unicodeScalars.reversed() {
        count |= Int(u.value)
      }
    }
  }
  blackHole(count)
}

@inline(never)
public func run_CharIndexing_chinese_unicodeScalars(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in chineseCharacters {
      let s = c.unicodeScalars
      for i in s.indices {
        count |= Int(s[i].value)
      }
    }
  }
  blackHole(count)
}

@inline(never)
public func run_CharIndexing_chinese_unicodeScalars_Backwards(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in chineseCharacters {
      let s = c.unicodeScalars
      for i in s.indices.reversed() {
        count |= Int(s[i].value)
      }
    }
  }
  blackHole(count)
}



@inline(never)
public func run_StringWalk_korean_unicodeScalars(_ n: Int) {
  for _ in 1...n {
    count_unicodeScalars(korean.unicodeScalars)
  }
}

@inline(never)
public func run_StringWalk_korean_unicodeScalars_Backwards(_ n: Int) {
  for _ in 1...n {
    count_unicodeScalars_rev(korean.unicodeScalars.reversed())
  }
}


@inline(never)
public func run_StringWalk_korean_characters(_ n: Int) {
  for _ in 1...n {
    count_characters(korean)
  }
}

@inline(never)
public func run_StringWalk_korean_characters_Backwards(_ n: Int) {
  for _ in 1...n {
    count_characters_rev(korean.reversed())
  }
}


@inline(never)
public func run_StringWalk_korean_characters_by_index(_ n: Int) {
  for _ in 1...n {
    count_characters_by_index(korean)
  }
}

@inline(never)
public func run_StringWalk_korean_characters_by_index_Backwards(_ n: Int) {
  for _ in 1...n {
    count_characters_by_index_rev(korean.reversed())
  }
}


let koreanCharacters = Array(korean)

@inline(never)
public func run_CharIteration_korean_unicodeScalars(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in koreanCharacters {
      for u in c.unicodeScalars {
        count |= Int(u.value)
      }
    }
  }
  blackHole(count)
}

@inline(never)
public func run_CharIteration_korean_unicodeScalars_Backwards(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in koreanCharacters {
      for u in c.unicodeScalars.reversed() {
        count |= Int(u.value)
      }
    }
  }
  blackHole(count)
}

@inline(never)
public func run_CharIndexing_korean_unicodeScalars(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in koreanCharacters {
      let s = c.unicodeScalars
      for i in s.indices {
        count |= Int(s[i].value)
      }
    }
  }
  blackHole(count)
}

@inline(never)
public func run_CharIndexing_korean_unicodeScalars_Backwards(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in koreanCharacters {
      let s = c.unicodeScalars
      for i in s.indices.reversed() {
        count |= Int(s[i].value)
      }
    }
  }
  blackHole(count)
}



@inline(never)
public func run_StringWalk_russian_unicodeScalars(_ n: Int) {
  for _ in 1...n {
    count_unicodeScalars(russian.unicodeScalars)
  }
}

@inline(never)
public func run_StringWalk_russian_unicodeScalars_Backwards(_ n: Int) {
  for _ in 1...n {
    count_unicodeScalars_rev(russian.unicodeScalars.reversed())
  }
}


@inline(never)
public func run_StringWalk_russian_characters(_ n: Int) {
  for _ in 1...n {
    count_characters(russian)
  }
}

@inline(never)
public func run_StringWalk_russian_characters_Backwards(_ n: Int) {
  for _ in 1...n {
    count_characters_rev(russian.reversed())
  }
}


@inline(never)
public func run_StringWalk_russian_characters_by_index(_ n: Int) {
  for _ in 1...n {
    count_characters_by_index(russian)
  }
}

@inline(never)
public func run_StringWalk_russian_characters_by_index_Backwards(_ n: Int) {
  for _ in 1...n {
    count_characters_by_index_rev(russian.reversed())
  }
}


let russianCharacters = Array(russian)

@inline(never)
public func run_CharIteration_russian_unicodeScalars(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in russianCharacters {
      for u in c.unicodeScalars {
        count |= Int(u.value)
      }
    }
  }
  blackHole(count)
}

@inline(never)
public func run_CharIteration_russian_unicodeScalars_Backwards(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in russianCharacters {
      for u in c.unicodeScalars.reversed() {
        count |= Int(u.value)
      }
    }
  }
  blackHole(count)
}

@inline(never)
public func run_CharIndexing_russian_unicodeScalars(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in russianCharacters {
      let s = c.unicodeScalars
      for i in s.indices {
        count |= Int(s[i].value)
      }
    }
  }
  blackHole(count)
}

@inline(never)
public func run_CharIndexing_russian_unicodeScalars_Backwards(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in russianCharacters {
      let s = c.unicodeScalars
      for i in s.indices.reversed() {
        count |= Int(s[i].value)
      }
    }
  }
  blackHole(count)
}



@inline(never)
public func run_StringWalk_punctuated_unicodeScalars(_ n: Int) {
  for _ in 1...n {
    count_unicodeScalars(punctuated.unicodeScalars)
  }
}

@inline(never)
public func run_StringWalk_punctuated_unicodeScalars_Backwards(_ n: Int) {
  for _ in 1...n {
    count_unicodeScalars_rev(punctuated.unicodeScalars.reversed())
  }
}


@inline(never)
public func run_StringWalk_punctuated_characters(_ n: Int) {
  for _ in 1...n {
    count_characters(punctuated)
  }
}

@inline(never)
public func run_StringWalk_punctuated_characters_Backwards(_ n: Int) {
  for _ in 1...n {
    count_characters_rev(punctuated.reversed())
  }
}


@inline(never)
public func run_StringWalk_punctuated_characters_by_index(_ n: Int) {
  for _ in 1...n {
    count_characters_by_index(punctuated)
  }
}

@inline(never)
public func run_StringWalk_punctuated_characters_by_index_Backwards(_ n: Int) {
  for _ in 1...n {
    count_characters_by_index_rev(punctuated.reversed())
  }
}


let punctuatedCharacters = Array(punctuated)

@inline(never)
public func run_CharIteration_punctuated_unicodeScalars(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in punctuatedCharacters {
      for u in c.unicodeScalars {
        count |= Int(u.value)
      }
    }
  }
  blackHole(count)
}

@inline(never)
public func run_CharIteration_punctuated_unicodeScalars_Backwards(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in punctuatedCharacters {
      for u in c.unicodeScalars.reversed() {
        count |= Int(u.value)
      }
    }
  }
  blackHole(count)
}

@inline(never)
public func run_CharIndexing_punctuated_unicodeScalars(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in punctuatedCharacters {
      let s = c.unicodeScalars
      for i in s.indices {
        count |= Int(s[i].value)
      }
    }
  }
  blackHole(count)
}

@inline(never)
public func run_CharIndexing_punctuated_unicodeScalars_Backwards(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in punctuatedCharacters {
      let s = c.unicodeScalars
      for i in s.indices.reversed() {
        count |= Int(s[i].value)
      }
    }
  }
  blackHole(count)
}



@inline(never)
public func run_StringWalk_punctuatedJapanese_unicodeScalars(_ n: Int) {
  for _ in 1...n {
    count_unicodeScalars(punctuatedJapanese.unicodeScalars)
  }
}

@inline(never)
public func run_StringWalk_punctuatedJapanese_unicodeScalars_Backwards(_ n: Int) {
  for _ in 1...n {
    count_unicodeScalars_rev(punctuatedJapanese.unicodeScalars.reversed())
  }
}


@inline(never)
public func run_StringWalk_punctuatedJapanese_characters(_ n: Int) {
  for _ in 1...n {
    count_characters(punctuatedJapanese)
  }
}

@inline(never)
public func run_StringWalk_punctuatedJapanese_characters_Backwards(_ n: Int) {
  for _ in 1...n {
    count_characters_rev(punctuatedJapanese.reversed())
  }
}


@inline(never)
public func run_StringWalk_punctuatedJapanese_characters_by_index(_ n: Int) {
  for _ in 1...n {
    count_characters_by_index(punctuatedJapanese)
  }
}

@inline(never)
public func run_StringWalk_punctuatedJapanese_characters_by_index_Backwards(_ n: Int) {
  for _ in 1...n {
    count_characters_by_index_rev(punctuatedJapanese.reversed())
  }
}


let punctuatedJapaneseCharacters = Array(punctuatedJapanese)

@inline(never)
public func run_CharIteration_punctuatedJapanese_unicodeScalars(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in punctuatedJapaneseCharacters {
      for u in c.unicodeScalars {
        count |= Int(u.value)
      }
    }
  }
  blackHole(count)
}

@inline(never)
public func run_CharIteration_punctuatedJapanese_unicodeScalars_Backwards(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in punctuatedJapaneseCharacters {
      for u in c.unicodeScalars.reversed() {
        count |= Int(u.value)
      }
    }
  }
  blackHole(count)
}

@inline(never)
public func run_CharIndexing_punctuatedJapanese_unicodeScalars(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in punctuatedJapaneseCharacters {
      let s = c.unicodeScalars
      for i in s.indices {
        count |= Int(s[i].value)
      }
    }
  }
  blackHole(count)
}

@inline(never)
public func run_CharIndexing_punctuatedJapanese_unicodeScalars_Backwards(_ n: Int) {
  var count = 0
  for _ in 1...unicodeScalarsMultiplier*n {
    for c in punctuatedJapaneseCharacters {
      let s = c.unicodeScalars
      for i in s.indices.reversed() {
        count |= Int(s[i].value)
      }
    }
  }
  blackHole(count)
}



// Local Variables:
// eval: (read-only-mode 1)
// End:
