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

@inline(never) func count_scalars(_ s: String.UnicodeScalarView) {
  var count = 0
  for _ in s {
    count += 1
  }
  blackHole(count)
}
@inline(never) func count_chars(_ s: String) {
  var count = 0
  for _ in s {
    count += 1
  }
  blackHole(count)
}
@inline(never) func count_scalars_rev(
  _ s: ReversedCollection<String.UnicodeScalarView>
) {
  var count = 0
  for _ in s {
    count += 1
  }
  blackHole(count)
}
@inline(never) func count_chars_rev(
  _ s: ReversedCollection<String>
) {
  var count = 0
  for _ in s {
    count += 1
  }
  blackHole(count)
}
@inline(never) func count_charsIndexed(_ s: String) {
  var idx = s.startIndex
  while idx != s.endIndex {
    blackHole(s[idx])
    s.formIndex(after: &idx)
  }
}

@inline(never) func count_charsIndexed_rev(
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

var ascii: String? = nil
var emoji: String?  = nil
var utf16: String?  = nil
var japanese: String?  = nil
var chinese: String?  = nil
var korean: String?  = nil
var russian: String?  = nil
var punctuated: String?  = nil
var punctuatedJapanese: String?  = nil
var tweet: String? = nil

func initializeStrings() {
  ascii = String(
    repeating: "siebenhundertsiebenundsiebzigtausendsiebenhundertsiebenundsiebenzig",
    count: 50
  )

  emoji = String(
    repeating: "👍👩‍👩‍👧‍👧👨‍👨‍👦‍👦🇺🇸🇨🇦🇲🇽👍🏻👍🏼👍🏽👍🏾👍🏿",
    count: 25
  )

  utf16 = emoji! + "the quick brown fox" + String(emoji!.reversed())

  japanese = String(
    repeating: "今回のアップデートでSwiftに大幅な改良が施され、安定していてしかも直感的に使うことができるAppleプラットフォーム向けプログラミング言語になりました。",
    count: 50
  )

  chinese = String(
    repeating: "Swift 是面向 Apple 平台的编程语言，功能强大且直观易用，而本次更新对其进行了全面优化。",
    count: 50
  )

  korean = String(
    repeating: "이번 업데이트에서는 강력하면서도 직관적인 Apple 플랫폼용 프로그래밍 언어인 Swift를 완벽히 개선하였습니다.",
    count: 50
  )

  russian = String(
    repeating: "в чащах юга жил-был цитрус? да, но фальшивый экземпляр",
    count: 50
  )

  punctuated = String(
    repeating: "\u{201c}Hello\u{2010}world\u{2026}\u{201d}",
    count: 100
  )
  punctuatedJapanese = String(
    repeating: "\u{300c}\u{300e}今日は\u{3001}世界\u{3002}\u{300f}\u{300d}",
    count: 100
  )
  // A workload that's mostly Latin characters, with occasional emoji
  // interspersed. Common for tweets.
  tweet = String(
    repeating: "Worst thing about working on String is that it breaks *everything*. Asserts, debuggers, and *especially* printf-style debugging 😭",
    count: 40
  )
}
//
// Benchmarks
//

// Pre-commit benchmark: simple scalar walk
@inline(never)
public func run_StringWalk(_ n: Int) {
  let oldAscii = "siebenhundertsiebenundsiebzigtausendsiebenhundertsiebenundsiebenzig"
  for _ in 1...n*250 {
    count_scalars(oldAscii.unicodeScalars)
  }
}

// Extended String benchmarks:
let unicodeScalarsMultiplier = 5
let stringWalkWarmup = 5


// An extended benchmark suite exercising finer-granularity behavior of our
// Strings.
public let benchmarks = [
  BenchmarkInfo(
    name: "StringWalk",
    runFunction: run_StringWalk,
    tags: [.validation, .api, .String],
    legacyFactor: 40),


  BenchmarkInfo(
    name: "StringWalk.ascii.scalars",
    runFunction: run_StringWalk_ascii_scalars,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_ascii_scalars(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.ascii.chars",
    runFunction: run_StringWalk_ascii_chars,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_ascii_chars(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.ascii.charsIndexed",
    runFunction: run_StringWalk_ascii_charsIndexed,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_ascii_charsIndexed(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.ascii.scalarsBackwards",
    runFunction: run_StringWalk_ascii_scalarsBackwards,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_ascii_scalarsBackwards(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.ascii.charsBackwards",
    runFunction: run_StringWalk_ascii_charsBackwards,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_ascii_charsBackwards(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.ascii.charsIndexedBackwards",
    runFunction: run_StringWalk_ascii_charsIndexedBackwards,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_ascii_charsIndexedBackwards(stringWalkWarmup)
    },
    legacyFactor: 1),

  BenchmarkInfo(
    name: "CharIteration_ascii_unicodeScalars",
    runFunction: run_CharIteration_ascii_unicodeScalars,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_ascii_unicodeScalars",
    runFunction: run_CharIndexing_ascii_unicodeScalars,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIteration_ascii_unicodeScalars_Backwards",
    runFunction: run_CharIteration_ascii_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_ascii_unicodeScalars_Backwards",
    runFunction: run_CharIndexing_ascii_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "StringWalk.utf16.scalars",
    runFunction: run_StringWalk_utf16_scalars,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_utf16_scalars(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.utf16.chars",
    runFunction: run_StringWalk_utf16_chars,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_utf16_chars(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.utf16.charsIndexed",
    runFunction: run_StringWalk_utf16_charsIndexed,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_utf16_charsIndexed(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.utf16.scalarsBackwards",
    runFunction: run_StringWalk_utf16_scalarsBackwards,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_utf16_scalarsBackwards(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.utf16.charsBackwards",
    runFunction: run_StringWalk_utf16_charsBackwards,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_utf16_charsBackwards(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.utf16.charsIndexedBackwards",
    runFunction: run_StringWalk_utf16_charsIndexedBackwards,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_utf16_charsIndexedBackwards(stringWalkWarmup)
    },
    legacyFactor: 1),

  BenchmarkInfo(
    name: "CharIteration_utf16_unicodeScalars",
    runFunction: run_CharIteration_utf16_unicodeScalars,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_utf16_unicodeScalars",
    runFunction: run_CharIndexing_utf16_unicodeScalars,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIteration_utf16_unicodeScalars_Backwards",
    runFunction: run_CharIteration_utf16_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_utf16_unicodeScalars_Backwards",
    runFunction: run_CharIndexing_utf16_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "StringWalk.tweet.scalars",
    runFunction: run_StringWalk_tweet_scalars,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_tweet_scalars(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.tweet.chars",
    runFunction: run_StringWalk_tweet_chars,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_tweet_chars(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.tweet.charsIndexed",
    runFunction: run_StringWalk_tweet_charsIndexed,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_tweet_charsIndexed(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.tweet.scalarsBackwards",
    runFunction: run_StringWalk_tweet_scalarsBackwards,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_tweet_scalarsBackwards(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.tweet.charsBackwards",
    runFunction: run_StringWalk_tweet_charsBackwards,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_tweet_charsBackwards(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.tweet.charsIndexedBackwards",
    runFunction: run_StringWalk_tweet_charsIndexedBackwards,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_tweet_charsIndexedBackwards(stringWalkWarmup)
    },
    legacyFactor: 1),

  BenchmarkInfo(
    name: "CharIteration_tweet_unicodeScalars",
    runFunction: run_CharIteration_tweet_unicodeScalars,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_tweet_unicodeScalars",
    runFunction: run_CharIndexing_tweet_unicodeScalars,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIteration_tweet_unicodeScalars_Backwards",
    runFunction: run_CharIteration_tweet_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_tweet_unicodeScalars_Backwards",
    runFunction: run_CharIndexing_tweet_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "StringWalk.japanese.scalars",
    runFunction: run_StringWalk_japanese_scalars,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_japanese_scalars(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.japanese.chars",
    runFunction: run_StringWalk_japanese_chars,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_japanese_chars(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.japanese.charsIndexed",
    runFunction: run_StringWalk_japanese_charsIndexed,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_japanese_charsIndexed(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.japanese.scalarsBackwards",
    runFunction: run_StringWalk_japanese_scalarsBackwards,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_japanese_scalarsBackwards(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.japanese.charsBackwards",
    runFunction: run_StringWalk_japanese_charsBackwards,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_japanese_charsBackwards(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.japanese.charsIndexedBackwards",
    runFunction: run_StringWalk_japanese_charsIndexedBackwards,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_japanese_charsIndexedBackwards(stringWalkWarmup)
    },
    legacyFactor: 1),

  BenchmarkInfo(
    name: "CharIteration_japanese_unicodeScalars",
    runFunction: run_CharIteration_japanese_unicodeScalars,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_japanese_unicodeScalars",
    runFunction: run_CharIndexing_japanese_unicodeScalars,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIteration_japanese_unicodeScalars_Backwards",
    runFunction: run_CharIteration_japanese_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_japanese_unicodeScalars_Backwards",
    runFunction: run_CharIndexing_japanese_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "StringWalk.chinese.scalars",
    runFunction: run_StringWalk_chinese_scalars,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_chinese_scalars(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.chinese.chars",
    runFunction: run_StringWalk_chinese_chars,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_chinese_chars(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.chinese.charsIndexed",
    runFunction: run_StringWalk_chinese_charsIndexed,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_chinese_charsIndexed(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.chinese.scalarsBackwards",
    runFunction: run_StringWalk_chinese_scalarsBackwards,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_chinese_scalarsBackwards(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.chinese.charsBackwards",
    runFunction: run_StringWalk_chinese_charsBackwards,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_chinese_charsBackwards(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.chinese.charsIndexedBackwards",
    runFunction: run_StringWalk_chinese_charsIndexedBackwards,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_chinese_charsIndexedBackwards(stringWalkWarmup)
    },
    legacyFactor: 1),

  BenchmarkInfo(
    name: "CharIteration_chinese_unicodeScalars",
    runFunction: run_CharIteration_chinese_unicodeScalars,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_chinese_unicodeScalars",
    runFunction: run_CharIndexing_chinese_unicodeScalars,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIteration_chinese_unicodeScalars_Backwards",
    runFunction: run_CharIteration_chinese_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_chinese_unicodeScalars_Backwards",
    runFunction: run_CharIndexing_chinese_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "StringWalk.korean.scalars",
    runFunction: run_StringWalk_korean_scalars,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_korean_scalars(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.korean.chars",
    runFunction: run_StringWalk_korean_chars,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_korean_chars(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.korean.charsIndexed",
    runFunction: run_StringWalk_korean_charsIndexed,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_korean_charsIndexed(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.korean.scalarsBackwards",
    runFunction: run_StringWalk_korean_scalarsBackwards,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_korean_scalarsBackwards(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.korean.charsBackwards",
    runFunction: run_StringWalk_korean_charsBackwards,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_korean_charsBackwards(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.korean.charsIndexedBackwards",
    runFunction: run_StringWalk_korean_charsIndexedBackwards,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_korean_charsIndexedBackwards(stringWalkWarmup)
    },
    legacyFactor: 1),

  BenchmarkInfo(
    name: "CharIteration_korean_unicodeScalars",
    runFunction: run_CharIteration_korean_unicodeScalars,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_korean_unicodeScalars",
    runFunction: run_CharIndexing_korean_unicodeScalars,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIteration_korean_unicodeScalars_Backwards",
    runFunction: run_CharIteration_korean_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_korean_unicodeScalars_Backwards",
    runFunction: run_CharIndexing_korean_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "StringWalk.russian.scalars",
    runFunction: run_StringWalk_russian_scalars,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_russian_scalars(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.russian.chars",
    runFunction: run_StringWalk_russian_chars,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_russian_chars(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.russian.charsIndexed",
    runFunction: run_StringWalk_russian_charsIndexed,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_russian_charsIndexed(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.russian.scalarsBackwards",
    runFunction: run_StringWalk_russian_scalarsBackwards,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_russian_scalarsBackwards(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.russian.charsBackwards",
    runFunction: run_StringWalk_russian_charsBackwards,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_russian_charsBackwards(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.russian.charsIndexedBackwards",
    runFunction: run_StringWalk_russian_charsIndexedBackwards,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_russian_charsIndexedBackwards(stringWalkWarmup)
    },
    legacyFactor: 1),

  BenchmarkInfo(
    name: "CharIteration_russian_unicodeScalars",
    runFunction: run_CharIteration_russian_unicodeScalars,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_russian_unicodeScalars",
    runFunction: run_CharIndexing_russian_unicodeScalars,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIteration_russian_unicodeScalars_Backwards",
    runFunction: run_CharIteration_russian_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_russian_unicodeScalars_Backwards",
    runFunction: run_CharIndexing_russian_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "StringWalk.punctuated.scalars",
    runFunction: run_StringWalk_punctuated_scalars,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_punctuated_scalars(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.punctuated.chars",
    runFunction: run_StringWalk_punctuated_chars,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_punctuated_chars(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.punctuated.charsIndexed",
    runFunction: run_StringWalk_punctuated_charsIndexed,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_punctuated_charsIndexed(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.punctuated.scalarsBackwards",
    runFunction: run_StringWalk_punctuated_scalarsBackwards,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_punctuated_scalarsBackwards(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.punctuated.charsBackwards",
    runFunction: run_StringWalk_punctuated_charsBackwards,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_punctuated_charsBackwards(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.punctuated.charsIndexedBackwards",
    runFunction: run_StringWalk_punctuated_charsIndexedBackwards,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_punctuated_charsIndexedBackwards(stringWalkWarmup)
    },
    legacyFactor: 1),

  BenchmarkInfo(
    name: "CharIteration_punctuated_unicodeScalars",
    runFunction: run_CharIteration_punctuated_unicodeScalars,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_punctuated_unicodeScalars",
    runFunction: run_CharIndexing_punctuated_unicodeScalars,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIteration_punctuated_unicodeScalars_Backwards",
    runFunction: run_CharIteration_punctuated_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_punctuated_unicodeScalars_Backwards",
    runFunction: run_CharIndexing_punctuated_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "StringWalk.punctuatedJapanese.scalars",
    runFunction: run_StringWalk_punctuatedJapanese_scalars,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_punctuatedJapanese_scalars(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.punctuatedJapanese.chars",
    runFunction: run_StringWalk_punctuatedJapanese_chars,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_punctuatedJapanese_chars(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.punctuatedJapanese.charsIndexed",
    runFunction: run_StringWalk_punctuatedJapanese_charsIndexed,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_punctuatedJapanese_charsIndexed(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.punctuatedJapanese.scalarsBackwards",
    runFunction: run_StringWalk_punctuatedJapanese_scalarsBackwards,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_punctuatedJapanese_scalarsBackwards(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.punctuatedJapanese.charsBackwards",
    runFunction: run_StringWalk_punctuatedJapanese_charsBackwards,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_punctuatedJapanese_charsBackwards(stringWalkWarmup)
    },
    legacyFactor: 1),


  BenchmarkInfo(
    name: "StringWalk.punctuatedJapanese.charsIndexedBackwards",
    runFunction: run_StringWalk_punctuatedJapanese_charsIndexedBackwards,
    tags: [.api, .String],
    setUpFunction: {
      initializeStrings()
      run_StringWalk_punctuatedJapanese_charsIndexedBackwards(stringWalkWarmup)
    },
    legacyFactor: 1),

  BenchmarkInfo(
    name: "CharIteration_punctuatedJapanese_unicodeScalars",
    runFunction: run_CharIteration_punctuatedJapanese_unicodeScalars,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_punctuatedJapanese_unicodeScalars",
    runFunction: run_CharIndexing_punctuatedJapanese_unicodeScalars,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIteration_punctuatedJapanese_unicodeScalars_Backwards",
    runFunction: run_CharIteration_punctuatedJapanese_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),

  BenchmarkInfo(
    name: "CharIndexing_punctuatedJapanese_unicodeScalars_Backwards",
    runFunction: run_CharIndexing_punctuatedJapanese_unicodeScalars_Backwards,
    tags: [.validation, .api, .String],
    setUpFunction: initializeStrings,
    legacyFactor: 40),
]


@inline(never)
public func run_StringWalk_ascii_scalars(_ n: Int) {
  for _ in 1...n*10 {
    count_scalars(ascii!.unicodeScalars)
  }
}

@inline(never)
public func run_StringWalk_ascii_scalarsBackwards(_ n: Int) {
  for _ in 1...n*10 {
    count_scalars_rev(ascii!.unicodeScalars.reversed())
  }
}


@inline(never)
public func run_StringWalk_ascii_chars(_ n: Int) {
  for _ in 1...n*1 {
    count_chars(ascii!)
  }
}

@inline(never)
public func run_StringWalk_ascii_charsBackwards(_ n: Int) {
  for _ in 1...n*1 {
    count_chars_rev(ascii!.reversed())
  }
}


@inline(never)
public func run_StringWalk_ascii_charsIndexed(_ n: Int) {
  for _ in 1...n*1 {
    count_charsIndexed(ascii!)
  }
}

@inline(never)
public func run_StringWalk_ascii_charsIndexedBackwards(_ n: Int) {
  for _ in 1...n*1 {
    count_charsIndexed_rev(ascii!.reversed())
  }
}


let asciiCharacters = Array(ascii!)

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
public func run_StringWalk_utf16_scalars(_ n: Int) {
  for _ in 1...n*10 {
    count_scalars(utf16!.unicodeScalars)
  }
}

@inline(never)
public func run_StringWalk_utf16_scalarsBackwards(_ n: Int) {
  for _ in 1...n*10 {
    count_scalars_rev(utf16!.unicodeScalars.reversed())
  }
}


@inline(never)
public func run_StringWalk_utf16_chars(_ n: Int) {
  for _ in 1...n*1 {
    count_chars(utf16!)
  }
}

@inline(never)
public func run_StringWalk_utf16_charsBackwards(_ n: Int) {
  for _ in 1...n*1 {
    count_chars_rev(utf16!.reversed())
  }
}


@inline(never)
public func run_StringWalk_utf16_charsIndexed(_ n: Int) {
  for _ in 1...n*1 {
    count_charsIndexed(utf16!)
  }
}

@inline(never)
public func run_StringWalk_utf16_charsIndexedBackwards(_ n: Int) {
  for _ in 1...n*1 {
    count_charsIndexed_rev(utf16!.reversed())
  }
}


let utf16Characters = Array(utf16!)

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
public func run_StringWalk_tweet_scalars(_ n: Int) {
  for _ in 1...n*10 {
    count_scalars(tweet!.unicodeScalars)
  }
}

@inline(never)
public func run_StringWalk_tweet_scalarsBackwards(_ n: Int) {
  for _ in 1...n*10 {
    count_scalars_rev(tweet!.unicodeScalars.reversed())
  }
}


@inline(never)
public func run_StringWalk_tweet_chars(_ n: Int) {
  for _ in 1...n*1 {
    count_chars(tweet!)
  }
}

@inline(never)
public func run_StringWalk_tweet_charsBackwards(_ n: Int) {
  for _ in 1...n*1 {
    count_chars_rev(tweet!.reversed())
  }
}


@inline(never)
public func run_StringWalk_tweet_charsIndexed(_ n: Int) {
  for _ in 1...n*1 {
    count_charsIndexed(tweet!)
  }
}

@inline(never)
public func run_StringWalk_tweet_charsIndexedBackwards(_ n: Int) {
  for _ in 1...n*1 {
    count_charsIndexed_rev(tweet!.reversed())
  }
}


let tweetCharacters = Array(tweet!)

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
public func run_StringWalk_japanese_scalars(_ n: Int) {
  for _ in 1...n*10 {
    count_scalars(japanese!.unicodeScalars)
  }
}

@inline(never)
public func run_StringWalk_japanese_scalarsBackwards(_ n: Int) {
  for _ in 1...n*10 {
    count_scalars_rev(japanese!.unicodeScalars.reversed())
  }
}


@inline(never)
public func run_StringWalk_japanese_chars(_ n: Int) {
  for _ in 1...n*1 {
    count_chars(japanese!)
  }
}

@inline(never)
public func run_StringWalk_japanese_charsBackwards(_ n: Int) {
  for _ in 1...n*1 {
    count_chars_rev(japanese!.reversed())
  }
}


@inline(never)
public func run_StringWalk_japanese_charsIndexed(_ n: Int) {
  for _ in 1...n*1 {
    count_charsIndexed(japanese!)
  }
}

@inline(never)
public func run_StringWalk_japanese_charsIndexedBackwards(_ n: Int) {
  for _ in 1...n*1 {
    count_charsIndexed_rev(japanese!.reversed())
  }
}


let japaneseCharacters = Array(japanese!)

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
public func run_StringWalk_chinese_scalars(_ n: Int) {
  for _ in 1...n*10 {
    count_scalars(chinese!.unicodeScalars)
  }
}

@inline(never)
public func run_StringWalk_chinese_scalarsBackwards(_ n: Int) {
  for _ in 1...n*10 {
    count_scalars_rev(chinese!.unicodeScalars.reversed())
  }
}


@inline(never)
public func run_StringWalk_chinese_chars(_ n: Int) {
  for _ in 1...n*1 {
    count_chars(chinese!)
  }
}

@inline(never)
public func run_StringWalk_chinese_charsBackwards(_ n: Int) {
  for _ in 1...n*1 {
    count_chars_rev(chinese!.reversed())
  }
}


@inline(never)
public func run_StringWalk_chinese_charsIndexed(_ n: Int) {
  for _ in 1...n*1 {
    count_charsIndexed(chinese!)
  }
}

@inline(never)
public func run_StringWalk_chinese_charsIndexedBackwards(_ n: Int) {
  for _ in 1...n*1 {
    count_charsIndexed_rev(chinese!.reversed())
  }
}


let chineseCharacters = Array(chinese!)

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
public func run_StringWalk_korean_scalars(_ n: Int) {
  for _ in 1...n*10 {
    count_scalars(korean!.unicodeScalars)
  }
}

@inline(never)
public func run_StringWalk_korean_scalarsBackwards(_ n: Int) {
  for _ in 1...n*10 {
    count_scalars_rev(korean!.unicodeScalars.reversed())
  }
}


@inline(never)
public func run_StringWalk_korean_chars(_ n: Int) {
  for _ in 1...n*1 {
    count_chars(korean!)
  }
}

@inline(never)
public func run_StringWalk_korean_charsBackwards(_ n: Int) {
  for _ in 1...n*1 {
    count_chars_rev(korean!.reversed())
  }
}


@inline(never)
public func run_StringWalk_korean_charsIndexed(_ n: Int) {
  for _ in 1...n*1 {
    count_charsIndexed(korean!)
  }
}

@inline(never)
public func run_StringWalk_korean_charsIndexedBackwards(_ n: Int) {
  for _ in 1...n*1 {
    count_charsIndexed_rev(korean!.reversed())
  }
}


let koreanCharacters = Array(korean!)

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
public func run_StringWalk_russian_scalars(_ n: Int) {
  for _ in 1...n*10 {
    count_scalars(russian!.unicodeScalars)
  }
}

@inline(never)
public func run_StringWalk_russian_scalarsBackwards(_ n: Int) {
  for _ in 1...n*10 {
    count_scalars_rev(russian!.unicodeScalars.reversed())
  }
}


@inline(never)
public func run_StringWalk_russian_chars(_ n: Int) {
  for _ in 1...n*1 {
    count_chars(russian!)
  }
}

@inline(never)
public func run_StringWalk_russian_charsBackwards(_ n: Int) {
  for _ in 1...n*1 {
    count_chars_rev(russian!.reversed())
  }
}


@inline(never)
public func run_StringWalk_russian_charsIndexed(_ n: Int) {
  for _ in 1...n*1 {
    count_charsIndexed(russian!)
  }
}

@inline(never)
public func run_StringWalk_russian_charsIndexedBackwards(_ n: Int) {
  for _ in 1...n*1 {
    count_charsIndexed_rev(russian!.reversed())
  }
}


let russianCharacters = Array(russian!)

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
public func run_StringWalk_punctuated_scalars(_ n: Int) {
  for _ in 1...n*10 {
    count_scalars(punctuated!.unicodeScalars)
  }
}

@inline(never)
public func run_StringWalk_punctuated_scalarsBackwards(_ n: Int) {
  for _ in 1...n*10 {
    count_scalars_rev(punctuated!.unicodeScalars.reversed())
  }
}


@inline(never)
public func run_StringWalk_punctuated_chars(_ n: Int) {
  for _ in 1...n*1 {
    count_chars(punctuated!)
  }
}

@inline(never)
public func run_StringWalk_punctuated_charsBackwards(_ n: Int) {
  for _ in 1...n*1 {
    count_chars_rev(punctuated!.reversed())
  }
}


@inline(never)
public func run_StringWalk_punctuated_charsIndexed(_ n: Int) {
  for _ in 1...n*1 {
    count_charsIndexed(punctuated!)
  }
}

@inline(never)
public func run_StringWalk_punctuated_charsIndexedBackwards(_ n: Int) {
  for _ in 1...n*1 {
    count_charsIndexed_rev(punctuated!.reversed())
  }
}


let punctuatedCharacters = Array(punctuated!)

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
public func run_StringWalk_punctuatedJapanese_scalars(_ n: Int) {
  for _ in 1...n*10 {
    count_scalars(punctuatedJapanese!.unicodeScalars)
  }
}

@inline(never)
public func run_StringWalk_punctuatedJapanese_scalarsBackwards(_ n: Int) {
  for _ in 1...n*10 {
    count_scalars_rev(punctuatedJapanese!.unicodeScalars.reversed())
  }
}


@inline(never)
public func run_StringWalk_punctuatedJapanese_chars(_ n: Int) {
  for _ in 1...n*1 {
    count_chars(punctuatedJapanese!)
  }
}

@inline(never)
public func run_StringWalk_punctuatedJapanese_charsBackwards(_ n: Int) {
  for _ in 1...n*1 {
    count_chars_rev(punctuatedJapanese!.reversed())
  }
}


@inline(never)
public func run_StringWalk_punctuatedJapanese_charsIndexed(_ n: Int) {
  for _ in 1...n*1 {
    count_charsIndexed(punctuatedJapanese!)
  }
}

@inline(never)
public func run_StringWalk_punctuatedJapanese_charsIndexedBackwards(_ n: Int) {
  for _ in 1...n*1 {
    count_charsIndexed_rev(punctuatedJapanese!.reversed())
  }
}


let punctuatedJapaneseCharacters = Array(punctuatedJapanese!)

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
