// RUN: %empty-directory(%t)
// RUN: %target-build-swift -g %s -o %t/StringIndex
// RUN: %target-codesign %t/StringIndex
// RUN: env %env-SWIFT_BINARY_COMPATIBILITY_VERSION=0x050700 %target-run %t/StringIndex %S/Inputs/

// Note: the environment variable above forces the stdlib's bincompat version to
// 5.7 so that we can test new behavior even if the SDK we're using predates it.

// REQUIRES: executable_test
// REQUIRES: optimized_stdlib
// UNSUPPORTED: freestanding

import StdlibUnittest
#if _runtime(_ObjC)
import Foundation
import StdlibUnicodeUnittest
#endif

var suite = TestSuite("StringIndexTests")
defer { runAllTests() }

enum SimpleString: String {
  case smallASCII = "abcdefg"
  case smallUnicode = "abéÏ𓀀"
  case largeASCII = "012345678901234567890"
  case largeUnicode = "abéÏ012345678901234567890𓀀"
  case emoji = "😀😃🤢🤮👩🏿‍🎤🧛🏻‍♂️🧛🏻‍♂️👩‍👩‍👦‍👦"
}

let simpleStrings: [String] = [
  SimpleString.smallASCII.rawValue,
  SimpleString.smallUnicode.rawValue,
  SimpleString.largeASCII.rawValue,
  SimpleString.largeUnicode.rawValue,
  SimpleString.emoji.rawValue,
  "",
]

suite.test("basic sanity checks")
.forEach(in: simpleStrings) { s in
  let utf8 = Array(s.utf8)
  let subUTF8 = Array(s[...].utf8)
  let utf16 = Array(s.utf16)
  let subUTF16 = Array(s[...].utf16)
  let utf32 = Array(s.unicodeScalars.map { $0.value })
  let subUTF32 = Array(s[...].unicodeScalars.map { $0.value })

  expectEqual(s, String(decoding: utf8, as: UTF8.self))
  expectEqual(s, String(decoding: subUTF8, as: UTF8.self))
  expectEqual(s, String(decoding: utf16, as: UTF16.self))
  expectEqual(s, String(decoding: subUTF16, as: UTF16.self))
  expectEqual(s, String(decoding: utf32, as: UTF32.self))
  expectEqual(s, String(decoding: subUTF32, as: UTF32.self))
}

suite.test("view counts")
.forEach(in: simpleStrings) { s in
  func validateViewCount<View: BidirectionalCollection>(
    _ view: View, for string: String,
    stackTrace: SourceLocStack = SourceLocStack(),
    showFrame: Bool = true,
    file: String = #file, line: UInt = #line
  ) where View.Element: Equatable, View.Index == String.Index {

    let stackTrace = stackTrace.pushIf(showFrame, file: file, line: line)

    let count = view.count
    func expect(_ i: Int,
      file: String = #file, line: UInt = #line
    ) {
      expectEqual(count, i, "for String: \(string)",
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
        showFrame: false)
    }

    let reversedView = view.reversed()

    expect(Array(view).count)
    expect(view.indices.count)
    expect(view.indices.reversed().count)
    expect(reversedView.indices.count)
    expect(view.distance(from: view.startIndex, to: view.endIndex))
    expect(reversedView.distance(
      from: reversedView.startIndex, to: reversedView.endIndex))

    // Access the elements from the indices
    expectEqual(Array(view), view.indices.map { view[$0] })
    expectEqual(
      Array(reversedView), reversedView.indices.map { reversedView[$0] })

    let indicesArray = Array<String.Index>(view.indices)
    for i in 0..<indicesArray.count {
      var idx = view.startIndex
      idx = view.index(idx, offsetBy: i)
      expectEqual(indicesArray[i], idx)
    }
  }

  validateViewCount(s, for: s)
  validateViewCount(s.utf8, for: s)
  validateViewCount(s.utf16, for: s)
  validateViewCount(s.unicodeScalars, for: s)

  validateViewCount(s[...], for: s)
  validateViewCount(s[...].utf8, for: s)
  validateViewCount(s[...].utf16, for: s)
  validateViewCount(s[...].unicodeScalars, for: s)
}

suite.test("interchange")
.forEach(in: simpleStrings) { s in
  // Basic index alignment

  for idx in s.utf8.indices {
    let char = s.utf8[idx]

    // ASCII or leading code unit in the scalar
    if char <= 0x7F || char >= 0b1100_0000 {
      expectEqual(idx, idx.samePosition(in: s.unicodeScalars))
      expectEqual(idx, idx.samePosition(in: s.utf16))

      // ASCII
      if char <= 0x7F {
        expectEqual(UInt16(char), s.utf16[idx])
        expectEqual(UInt32(char), s.unicodeScalars[idx].value)
      }
    } else {
      // Continuation code unit
      assert(char & 0b1100_0000 == 0b1000_0000)
      expectNil(idx.samePosition(in: s))
      expectNil(idx.samePosition(in: s.utf16))
      expectNil(idx.samePosition(in: s.unicodeScalars))
    }
  }
}

suite.test("UTF-16 Offsets")
.forEach(in: simpleStrings) { s in
  let end = s.endIndex
  let utf16Count = s.utf16.count

  expectEqual(end, String.Index(utf16Offset: utf16Count, in: s))
  expectEqual(end, String.Index(utf16Offset: utf16Count, in: s[...]))

  let pastEnd = String.Index(utf16Offset: utf16Count+1, in: s)

  expectNotEqual(end, pastEnd)
  expectEqual(pastEnd, String.Index(utf16Offset: utf16Count+1, in: s[...]))
  expectEqual(pastEnd, String.Index(utf16Offset: utf16Count+2, in: s))
  expectEqual(pastEnd, String.Index(utf16Offset: -1, in: s))
  expectEqual(
    pastEnd, String.Index(utf16Offset: Swift.max(1, utf16Count), in: s.dropFirst()))

  let utf16Indices = Array(s.utf16.indices)
  expectEqual(utf16Count, utf16Indices.count)
  for i in 0..<utf16Indices.count {
    let idx = String.Index(utf16Offset: i, in: s)
    expectEqual(utf16Indices[i], idx)
    expectEqual(i, idx.utf16Offset(in: s))
    expectEqual(i, idx.utf16Offset(in: s[...]))

    if i < s.dropLast().utf16.count {
      expectEqual(
        utf16Indices[i], String.Index(utf16Offset: i, in: s.dropLast()))
      expectEqual(i, idx.utf16Offset(in: s.dropLast()))
    } else if i == s.dropLast().utf16.count {
      expectEqual(
        utf16Indices[i], String.Index(utf16Offset: i, in: s.dropLast()))
    } else {
      expectNotEqual(
        utf16Indices[i], String.Index(utf16Offset: i, in: s.dropLast()))
    }
  }
}

func swift5ScalarAlign(_ idx: String.Index, in str: String) -> String.Index {
  var idx = idx
  while str.utf8[idx] & 0xC0 == 0x80 { str.utf8.formIndex(before: &idx) }
  return idx
}

suite.test("Scalar Align UTF-8 indices") {
  // TODO: Test a new aligning API when we add it. For now, we
  // test scalar-aligning UTF-8 indices

  let str = "a😇"
  let subScalarIdx = str.utf8.index(str.utf8.startIndex, offsetBy: 2)

  let roundedIdx = swift5ScalarAlign(subScalarIdx, in: str)
  expectEqual(1, roundedIdx.utf16Offset(in: str))

  let roundedIdx2 = str.utf8[...subScalarIdx].lastIndex { $0 & 0xC0 != 0x80 }
  expectEqual(roundedIdx, roundedIdx2)

  var roundedIdx3 = subScalarIdx
  while roundedIdx3.samePosition(in: str.unicodeScalars) == nil {
    str.utf8.formIndex(before: &roundedIdx3)
  }
  expectEqual(roundedIdx, roundedIdx3)
}

#if _runtime(_ObjC)
suite.test("String.Index(_:within) / Range<String.Index>(_:in:)") {
  guard #available(SwiftStdlib 5.1, *) else {
    return
  }

  let str = simpleStrings.joined()
  let substr = str[...]
  for idx in str.utf8.indices {
    expectEqual(
      String.Index(idx, within: str), String.Index(idx, within: substr))
  }

  expectNil(String.Index(str.startIndex, within: str.dropFirst()))
  expectNil(String.Index(str.endIndex, within: str.dropLast()))
  expectNotNil(String.Index(str.startIndex, within: str))
  expectNotNil(String.Index(str.endIndex, within: str))

  let utf16Count = str.utf16.count
  let utf16Indices = Array(str.utf16.indices) + [str.utf16.endIndex]
  for location in 0..<utf16Count {
    for length in 0...(utf16Count - location) {
      let strLB = String.Index(utf16Indices[location], within: str)
      let substrLB = String.Index(utf16Indices[location], within: substr)
      let strUB = String.Index(utf16Indices[location+length], within: str)
      let substrUB = String.Index(utf16Indices[location+length], within: substr)
      expectEqual(strLB, substrLB)
      expectEqual(strUB, substrUB)

      let nsRange = NSRange(location: location, length: length)
      let strRange = Range<String.Index>(nsRange, in: str)
      let substrRange = Range<String.Index>(nsRange, in: substr)

      expectEqual(strRange, substrRange)
      guard strLB != nil && strUB != nil else {
        // TODO: rdar://112643333
        //expectNil(strRange)
        continue
      }
      expectEqual(strRange, Range(uncheckedBounds: (strLB!, strUB!)))
    }
  }
}
#endif

#if _runtime(_ObjC)
suite.test("Misaligned") {
  // Misaligned indices were fixed in 5.1
  guard _hasSwift_5_1() else { return }

  func doIt(_ str: String) {
    let characterIndices = Array(str.indices)
    let scalarIndices = Array(str.unicodeScalars.indices) + [str.endIndex]
    let utf8Indices = Array(str.utf8.indices)

    var lastScalarI = 0
    for i in 1..<utf8Indices.count {
      let idx = utf8Indices[i]

      // Skip aligned indices
      guard idx < scalarIndices[lastScalarI + 1] else {
        assert(idx == scalarIndices[lastScalarI + 1])
        lastScalarI += 1
        continue
      }
      expectTrue(UTF8.isContinuation(str.utf8[idx]))

      let lastScalarIdx = scalarIndices[lastScalarI]

      // Check aligning-down
      expectEqual(str[lastScalarIdx], str[idx])
      expectEqual(str.utf16[lastScalarIdx], str.utf16[idx])
      expectEqual(str.unicodeScalars[lastScalarIdx], str.unicodeScalars[idx])

      // Check distance
      let (start, end) = (str.startIndex, str.endIndex)
      if characterIndices.contains(lastScalarIdx) {
        expectEqual(0, str.distance(from: lastScalarIdx, to: idx))
        expectEqual(str[..<idx].count, str.distance(from: start, to: idx))
        expectEqual(str[idx...].count, str.distance(from: idx, to: end))
      }
      expectEqual(
        0, str.unicodeScalars.distance(from: lastScalarIdx, to: idx))
      expectEqual(
        str.unicodeScalars[..<idx].count,
        str.unicodeScalars.distance(from: start, to: idx))
      expectEqual(
        str.unicodeScalars[idx...].count,
        str.unicodeScalars.distance(from: idx, to: end))

      expectEqual(0, str.utf16.distance(from: lastScalarIdx, to: idx))
      expectEqual(
        str.utf16[..<idx].count, str.utf16.distance(from: start, to: idx))
      expectEqual(
        str.utf16[idx...].count, str.utf16.distance(from: idx, to: end))
    }
  }

  let nsstring: NSString = "aодиde\u{301}日🧟‍♀️"
  doIt(nsstring as String)

  let string = "aодиde\u{301}日🧟‍♀️"
  doIt(string)
}
#endif

let _examples: [StaticString] = [
  "abc\r\ndefg",
  "ab\r\ncдe\u{301}日🧟‍♀️x🧟x🏳️‍🌈🇺🇸🇨🇦",
]

let examples: [String] = _examples.flatMap { s in
  let str = "\(s)"
  #if _runtime(_ObjC)
  let unichars = Array(str.utf16)
  let nsstr = NSString(characters: unichars, length: unichars.count)
  return [str, nsstr as String]
  #else
  return [str]
  #endif
}


suite.test("Trivial index conversion cases/characters")
.forEach(in: examples) { s in
  for i in s.indices {
    let j = String.Index(i, within: s)
    expectNotNil(j, "i: \(i)")
    expectEqual(j, i)
  }
}

suite.test("Trivial index conversion cases/scalars")
.forEach(in: examples) { s in
  for i in s.unicodeScalars.indices {
    let j = String.Index(i, within: s.unicodeScalars)
    expectNotNil(j, "i: \(i)")
    expectEqual(j, i)
  }
}

suite.test("Trivial index conversion cases/UTF-8")
.forEach(in: examples) { s in
  for i in s.utf8.indices {
    let j = String.Index(i, within: s.utf8)
    expectNotNil(j, "i: \(i)")
    expectEqual(j, i)
  }
}

suite.test("Trivial index conversion cases/UTF-16")
.forEach(in: examples) { s in
  guard #available(SwiftStdlib 5.7, *) else {
    // Prior to 5.7, String.Index.init(_:within:) used to incorrectly reject
    // indices pointing to trailing surrogates.
    return
  }

  for i in s.utf16.indices {
    let j = String.Index(i, within: s.utf16)
    expectNotNil(j, "i: \(i)")
    expectEqual(j, i)
  }
}

#if _runtime(_ObjC)
suite.test("Exhaustive Index Interchange")
.forEach(in: examples) { str in
  guard #available(SwiftStdlib 5.1, *) else {
    return
  }

  //str.dumpIndices()

  var curCharIdx = str.startIndex
  var curScalarIdx = str.startIndex
  var curUTF8Idx = str.startIndex
  var curUTF16Idx = str.startIndex

  while curCharIdx < str.endIndex {
    let curChar = str[curCharIdx]
    expectEqual(curChar, str[curScalarIdx])
    expectEqual(curChar, str[curUTF8Idx])
    expectEqual(curChar, str[curUTF16Idx])

    // Advance the character index once and have the scalar index catch up
    str.formIndex(after: &curCharIdx)

    let scalarStartIdx = curScalarIdx
    defer {
      let sub = str[scalarStartIdx..<curScalarIdx]
      expectEqual(sub.count, 1)
      expectEqual(sub.first, curChar)
      expectEqual(str.distance(from: scalarStartIdx, to: curScalarIdx), 1)
    }

    while curScalarIdx < curCharIdx {
      let scalarStartIdx = curScalarIdx
      let curScalar = str.unicodeScalars[curScalarIdx]
      let curSubChar = str[curScalarIdx]

      // If there is a Character prior to this scalar, remember it and check
      // that misaligned code unit indices also produce it.
      let scalarPriorCharacter: Character?
      if scalarStartIdx == str.startIndex {
        scalarPriorCharacter = nil
      } else {
        scalarPriorCharacter = str[str.index(before: scalarStartIdx)]
      }

      // Advance the scalar index once and have the code unit indices catch up
      str.unicodeScalars.formIndex(after: &curScalarIdx)


      let utf8StartIdx = curUTF8Idx
      defer {
        let sub = str.unicodeScalars[utf8StartIdx..<curUTF8Idx]
        expectEqual(sub.count, 1)
        expectEqual(sub.first, curScalar)
        expectEqual(
          str.unicodeScalars.distance(from: utf8StartIdx, to: curUTF8Idx),
          1)
        expectEqual(
          str.utf8.distance(from: utf8StartIdx, to: curUTF8Idx),
          curScalar.utf8.count)
      }

      while curUTF8Idx < curScalarIdx {
        expectEqual(curScalar, str.unicodeScalars[curUTF8Idx])
        expectEqual(curSubChar, str[curUTF8Idx])
        expectFalse(UTF16.isTrailSurrogate(str.utf16[curUTF8Idx]))
        expectEqual(utf8StartIdx, str[curUTF8Idx...].startIndex)
        expectTrue(str[utf8StartIdx..<curUTF8Idx].isEmpty)
        expectEqual(str.utf16.distance(from: utf8StartIdx, to: curUTF8Idx), 0)

        if let scalarPrior = scalarPriorCharacter {
          expectEqual(scalarPrior, str[str.index(before: curUTF8Idx)])
        }

        str.utf8.formIndex(after: &curUTF8Idx)
      }
      expectEqual(curUTF8Idx, curScalarIdx)

      var utf8RevIdx = curUTF8Idx
      while utf8RevIdx > utf8StartIdx {
        str.utf8.formIndex(before: &utf8RevIdx)

        expectEqual(curScalar, str.unicodeScalars[utf8RevIdx])
        expectEqual(curSubChar, str[utf8RevIdx])
        expectFalse(UTF16.isTrailSurrogate(str.utf16[utf8RevIdx]))
        expectEqual(utf8StartIdx, str[utf8RevIdx...].startIndex)
        expectTrue(str[utf8StartIdx..<utf8RevIdx].isEmpty)
        expectEqual(str.utf16.distance(from: utf8StartIdx, to: utf8RevIdx), 0)
      }
      expectEqual(utf8RevIdx, utf8StartIdx)

      let utf16StartIdx = curUTF16Idx
      defer {
        let sub = str.unicodeScalars[utf16StartIdx..<curUTF16Idx]
        expectEqual(sub.count, 1)
        expectEqual(sub.first, curScalar)
        expectEqual(
          str.unicodeScalars.distance(from: utf16StartIdx, to: curUTF16Idx),
          1)
        expectEqual(
          str.utf16.distance(from: utf16StartIdx, to: curUTF16Idx),
          curScalar.utf16.count)
      }

      while curUTF16Idx < curScalarIdx {
        expectEqual(curScalar, str.unicodeScalars[curUTF16Idx])
        expectEqual(curSubChar, str[curUTF16Idx])
        expectFalse(UTF8.isContinuation(str.utf8[curUTF16Idx]))
        expectEqual(utf16StartIdx, str[curUTF16Idx...].startIndex)
        expectTrue(str[utf16StartIdx..<curUTF16Idx].isEmpty)
        expectEqual(str.utf8.distance(from: utf16StartIdx, to: curUTF16Idx), 0)

        if let scalarPrior = scalarPriorCharacter {
          expectEqual(scalarPrior, str[str.index(before: curUTF16Idx)])
        }

        str.utf16.formIndex(after: &curUTF16Idx)
      }
      expectEqual(curUTF16Idx, curScalarIdx)

      var utf16RevIdx = curUTF16Idx
      while utf16RevIdx > utf16StartIdx {
        str.utf16.formIndex(before: &utf16RevIdx)

        expectEqual(curScalar, str.unicodeScalars[utf16RevIdx])
        expectEqual(curSubChar, str[utf16RevIdx])
        expectFalse(UTF8.isContinuation(str.utf8[utf16RevIdx]))
        expectEqual(utf16StartIdx, str[utf16RevIdx...].startIndex)
        expectTrue(str[utf16StartIdx..<utf16RevIdx].isEmpty)
        expectEqual(str.utf8.distance(from: utf16StartIdx, to: utf16RevIdx), 0)
      }
      expectEqual(utf16RevIdx, utf16StartIdx)
    }
  }
}
#endif

func fullyExhaustiveIndexInterchange(_ string: String) {
  guard #available(SwiftStdlib 5.7, *) else {
    // Index navigation in 5.7 always rounds input indices down to the nearest
    // Character, so that we always have a well-defined distance between
    // indices, even if they aren't valid.
    //
    // 5.6 and below did not behave consistently in this case.
    return
  }

  //string.dumpIndices()

  let scalarMap = string.scalarMap()
  let characterMap = string.characterMap()
  let allIndices = string.allIndices()

  func referenceCharacterDistance(
    from i: String.Index, to j: String.Index
  ) -> Int {
    let ci = characterMap[i]!
    let cj = characterMap[j]!
    return cj.offset - ci.offset
  }

  func referenceScalarDistance(
    from i: String.Index, to j: String.Index
  ) -> Int {
    let si = scalarMap[i]!
    let sj = scalarMap[j]!
    return sj.offset - si.offset
  }

  for i in allIndices {
    for j in allIndices {
      let characterDistance = referenceCharacterDistance(from: i, to: j)
      let scalarDistance = referenceScalarDistance(from: i, to: j)

      // Check distance calculations.
      expectEqual(
        string.distance(from: i, to: j),
        characterDistance,
        """
        string: \(string.debugDescription)
        i:      \(i)
        j:      \(j)
        """)
      expectEqual(
        string.unicodeScalars.distance(from: i, to: j),
        scalarDistance,
        """
        string: \(string.debugDescription)
        i:      \(i)
        j:      \(j)
        """)

      if i <= j {
        // The substring `string[i..<j]` does not round its bounds to
        // `Character` boundaries, so it may have a different count than what
        // the base string reports.
        let si = scalarMap[i]!.index
        let sj = scalarMap[j]!.index
        let s = String.UnicodeScalarView(string.unicodeScalars[si ..< sj])
        let substringDistance = String(s).count

        let substring = string[i ..< j]
        let subscalars = string.unicodeScalars[i ..< j]

        expectEqual(substring.count, substringDistance,
          """
          string: \(string.debugDescription)
          i:      \(i)
          j:      \(j)
          """)

        // The `Character` alignment consideration above doesn't apply to
        // Unicode scalars in a substring.
        expectEqual(subscalars.count, scalarDistance,
          """
          string: \(string.debugDescription)
          i:      \(i)
          j:      \(j)
          """)

        // Check reachability of substring bounds.
        var i = substring.startIndex
        for _ in 0 ..< substringDistance {
          substring.formIndex(after: &i)
        }
        expectEqual(i, substring.endIndex)

        expectEqual(
          substring.index(substring.startIndex, offsetBy: substringDistance),
          substring.endIndex,
          """
          string:   \(string.debugDescription)
          i:        \(i)
          j:        \(j)
          distance: \(characterDistance)
          """)
        expectEqual(
          substring.index(substring.endIndex, offsetBy: -substringDistance),
          substring.startIndex,
          """
          string:   \(string.debugDescription)
          i:        \(i)
          j:        \(j)
          distance: \(-characterDistance)
          """)

        expectEqual(
          subscalars.index(subscalars.startIndex, offsetBy: scalarDistance),
          subscalars.endIndex,
          """
          string:   \(string.debugDescription)
          i:        \(i)
          j:        \(j)
          distance: \(scalarDistance)
          """)
        expectEqual(
          subscalars.index(subscalars.endIndex, offsetBy: -scalarDistance),
          subscalars.startIndex,
          """
          string:   \(string.debugDescription)
          i:        \(i)
          j:        \(j)
          distance: \(-scalarDistance)
          """)
      }

      // Check `String.index(_:offsetBy:limitedBy:)`.
      for limit in allIndices {
        let dest = characterMap[j]!.index
        let expectHit = (
          characterDistance > 0 && i <= limit && dest > limit ? true
          : characterDistance < 0 && i >= limit && dest < limit ? true
          : false)
        expectEqual(
          string.index(i, offsetBy: characterDistance, limitedBy: limit),
          expectHit ? nil : dest,
          """
          string: \(string.debugDescription)
          i:      \(i)
          j:      \(j)   (distance: \(characterDistance))
          limit:  \(limit)
          """)
      }
    }
  }
}

suite.test("Fully exhaustive index interchange")
.forEach(in: examples) { string in
  fullyExhaustiveIndexInterchange(string)
}

#if _runtime(_ObjC)
suite.test("Fully exhaustive index interchange/GraphemeBreakTests") {
  for test in graphemeBreakTests {
    fullyExhaustiveIndexInterchange(test.string)
  }
}
#endif

suite.test("Global vs local grapheme cluster boundaries") {
  guard #available(SwiftStdlib 5.7, *) else {
    // Index navigation in 5.7 always rounds input indices down to the nearest
    // Character, so that we always have a well-defined distance between
    // indices, even if they aren't valid.
    //
    // 5.6 and below did not behave consistently in this case.
    return
  }

  let str = "a🇺🇸🇨🇦b"
  // U+0061 LATIN SMALL LETTER A
  // U+1F1FA REGIONAL INDICATOR SYMBOL LETTER U
  // U+1F1F8 REGIONAL INDICATOR SYMBOL LETTER S
  // U+1F1E8 REGIONAL INDICATOR SYMBOL LETTER C
  // U+1F1E6 REGIONAL INDICATOR SYMBOL LETTER A
  // U+0062 LATIN SMALL LETTER B

  let c = Array(str.indices) + [str.endIndex]
  let s = Array(str.unicodeScalars.indices) + [str.unicodeScalars.endIndex]
  let u8 = Array(str.utf8.indices) + [str.utf8.endIndex]
  let u16 = Array(str.utf16.indices) + [str.utf16.endIndex]

  // Index navigation must always round the input index down to the nearest
  // Character.

  expectEqual(str.count, 4)
  expectEqual(str.index(after: c[0]), c[1])
  expectEqual(str.index(after: c[1]), c[2])
  expectEqual(str.index(after: c[2]), c[3])
  expectEqual(str.index(after: c[3]), c[4])

  expectEqual(str.index(before: c[4]), c[3])
  expectEqual(str.index(before: c[3]), c[2])
  expectEqual(str.index(before: c[2]), c[1])
  expectEqual(str.index(before: c[1]), c[0])

  // Scalars
  expectEqual(str.unicodeScalars.count, 6)
  expectEqual(str.index(after: s[0]), s[1])
  expectEqual(str.index(after: s[1]), s[3])
  expectEqual(str.index(after: s[2]), s[3]) // s[2] ≅ s[1]
  expectEqual(str.index(after: s[3]), s[5])
  expectEqual(str.index(after: s[4]), s[5]) // s[4] ≅ s[3]
  expectEqual(str.index(after: s[5]), s[6])

  expectEqual(str.index(before: s[6]), s[5])
  expectEqual(str.index(before: s[5]), s[3])
  expectEqual(str.index(before: s[4]), s[1]) // s[4] ≅ s[3]
  expectEqual(str.index(before: s[3]), s[1])
  expectEqual(str.index(before: s[2]), s[0]) // s[2] ≅ s[1]
  expectEqual(str.index(before: s[1]), s[0])

  // UTF-8
  expectEqual(str.utf8.count, 18)
  expectEqual(str.index(after: u8[0]), u8[1])
  for i in 1 ..< 9 { // s[i] ≅ s[1]
    expectEqual(str.index(after: u8[i]), u8[9])
  }
  for i in 9 ..< 17 { // s[i] ≅ s[9]
    expectEqual(str.index(after: u8[i]), u8[17])
  }
  expectEqual(str.index(after: u8[17]), u8[18])

  // UTF-16
  expectEqual(str.utf16.count, 10)
  expectEqual(str.index(after: u16[0]), u16[1])
  expectEqual(str.index(after: u16[1]), u16[5])
  expectEqual(str.index(after: u16[2]), u16[5]) // s[2] ≅ s[1]
  expectEqual(str.index(after: u16[3]), u16[5]) // s[3] ≅ s[1]
  expectEqual(str.index(after: u16[4]), u16[5]) // s[4] ≅ s[1]
  expectEqual(str.index(after: u16[5]), u16[9])
  expectEqual(str.index(after: u16[6]), u16[9]) // s[6] ≅ s[5]
  expectEqual(str.index(after: u16[7]), u16[9]) // s[7] ≅ s[5]
  expectEqual(str.index(after: u16[8]), u16[9]) // s[8] ≅ s[5]
  expectEqual(str.index(after: u16[9]), u16[10])

  let i = s[2] // second scalar of US flag
  let j = s[4] // second scalar of CA flag
  // However, subscripting should only round down to the nearest scalar.
  // Per SE-0180, `s[i..<j]` should be the Seychelles flag (country code SC)
  let slice = str[i ..< j]
  expectEqual(slice, "\u{1F1F8}\u{1F1E8}")
  expectEqual(slice.first, "\u{1F1F8}\u{1F1E8}" as Character)
  expectEqual(slice.count, 1)

  // Index navigation within the substring must work as if we were in a string
  // containing exactly the same scalars. Substring bounds must be reachable,
  // even if they aren't reachable in the base string.
  expectEqual(slice.startIndex, i)
  expectEqual(slice.endIndex, j)
  expectEqual(slice.index(after: slice.startIndex), j)
  expectEqual(slice.index(before: slice.endIndex), i)

  expectEqual(slice.unicodeScalars.count, 2)
  expectEqual(slice.utf8.count, 8)
  expectEqual(slice.utf16.count, 4)
}

#if _runtime(_ObjC)
suite.test("Index encoding correction/UTF-16→8/subscripts") {
  guard #available(SwiftStdlib 5.7, *) else {
    // String indices did not track their encoding until 5.7.
    return
  }
  // This tests a special case in String's index validation where we allow
  // UTF-16-encoded indices to be applied to UTF-8 strings, by transcoding the
  // offset of the index. Applying such indices is always an error, but it
  // happens relatively often when someone is erroneously holding on to an index
  // of a UTF-16-encoded bridged NSString value through a mutation. Mutating a
  // bridged string converts it to a UTF-8 native string, changing the meaning
  // of the offset value. (Even if the mutation wouldn't otherwise affect the
  // original index.)
  //
  // Before 5.7, the stdlib did not track the offset encoding of String indices,
  // so they simply used the UTF-16 offset to access UTF-8 storage. This
  // generally produces nonsensical results, but if the string happens to be
  // ASCII, then this actually did work "fine".
  //
  // To avoid breaking binaries that rely on this behavior, the 5.7 stdlib
  // automatically converts UTF-16-encoded indices to UTF-8 when needed.
  // This can be quite slow, but it always produces the "right" results.
  //
  // This conversion also works in the other direction, and it does not account
  // for the effect of the actual mutation. If the mutation's effect included
  // the data addressed by the original index, then we may still get nonsensical
  // results.
  var s = ("🫱🏼‍🫲🏽 a 🧑🏽‍🌾 b" as NSString) as String
  //s.dumpIndices()

  let originals = s.allIndices(includingEnd: false).map {
    ($0, s[$0], s.unicodeScalars[$0], s.utf8[$0], s.utf16[$0])
  }

  s.makeContiguousUTF8()
  //s.dumpIndices()

  for (i, char, scalar, u8, u16) in originals {
    expectEqual(s[i], char, "i: \(i)")
    expectEqual(s.unicodeScalars[i], scalar, "i: \(i)")
    expectEqual(s.utf8[i], u8, "i: \(i)")
    expectEqual(s.utf16[i], u16, "i: \(i)")
  }
}
#endif

#if _runtime(_ObjC)
suite.test("Index encoding correction/UTF-8→16/subscripts") {
  guard #available(SwiftStdlib 5.7, *) else {
    // String indices did not track their encoding until 5.7.
    return
  }
  // This tests the UTF-8 to UTF-16 direction of the special case in String's
  // index validation above.
  let native = "🫱🏼‍🫲🏽 a 🧑🏽‍🌾 b"
  let cocoa = ("🫱🏼‍🫲🏽 a 🧑🏽‍🌾 b" as NSString) as String

  let nativeIndices = native.allIndices(includingEnd: false).map {
    (
      $0,
      native[$0],
      native.unicodeScalars[$0],
      native.utf8[$0],
      native.utf16[$0])
  }

  for (i, char, scalar, u8, u16) in nativeIndices {
    expectEqual(cocoa[i], char, "i: \(i)")
    expectEqual(cocoa.unicodeScalars[i], scalar, "i: \(i)")
    expectEqual(cocoa.utf8[i], u8, "i: \(i)")
    expectEqual(cocoa.utf16[i], u16, "i: \(i)")
  }
}
#endif

#if _runtime(_ObjC)
suite.test("Index encoding correction/UTF-16→8/conversions/characters") {
  guard #available(SwiftStdlib 5.7, *) else {
    // String indices did not track their encoding until 5.7.
    return
  }
  var s = ("🫱🏼‍🫲🏽 a 🧑🏽‍🌾 b" as NSString) as String

  let chars = s.indices.map { ($0, s[$0]) }

  s.makeContiguousUTF8()

  for (i, char) in chars {
    let j = String.Index(i, within: s)
    expectNotNil(j, "i: \(i)")
    expectEqual(j.map { s[$0] }, char, "i: \(i)")
  }
}
#endif

#if _runtime(_ObjC)
suite.test("Index encoding correction/UTF-8→16/conversions/characters") {
  guard #available(SwiftStdlib 5.7, *) else {
    // String indices did not track their encoding until 5.7.
    return
  }
  let native = "🫱🏼‍🫲🏽 a 🧑🏽‍🌾 b"
  let cocoa = ("🫱🏼‍🫲🏽 a 🧑🏽‍🌾 b" as NSString) as String

  let chars = native.indices.map { ($0, native[$0]) }

  for (i, char) in chars {
    let j = String.Index(i, within: cocoa)
    expectNotNil(j, "i: \(i)")
    expectEqual(j.map { cocoa[$0] }, char, "i: \(i)")
  }
}
#endif

#if _runtime(_ObjC)
suite.test("Index encoding correction/UTF-16→8/conversions/scalars") {
  guard #available(SwiftStdlib 5.7, *) else {
    // String indices did not track their encoding until 5.7.
    return
  }
  var s = ("🫱🏼‍🫲🏽 a 🧑🏽‍🌾 b" as NSString) as String

  let scalars = s.unicodeScalars.indices.map { ($0, s.unicodeScalars[$0]) }

  s.makeContiguousUTF8()

  for (i, scalar) in scalars {
    let j = String.Index(i, within: s.unicodeScalars)
    expectNotNil(j, "i: \(i)")
    expectEqual(j.map { s.unicodeScalars[$0] }, scalar, "i: \(i)")
  }
}
#endif

#if _runtime(_ObjC)
suite.test("Index encoding correction/UTF-8→16/conversions/scalars") {
  guard #available(SwiftStdlib 5.7, *) else {
    // String indices did not track their encoding until 5.7.
    return
  }
  let native = "🫱🏼‍🫲🏽 a 🧑🏽‍🌾 b"
  let cocoa = ("🫱🏼‍🫲🏽 a 🧑🏽‍🌾 b" as NSString) as String

  let scalars = native.unicodeScalars.indices.map {
    ($0, native.unicodeScalars[$0])
  }

  for (i, scalar) in scalars {
    let j = String.Index(i, within: cocoa.unicodeScalars)
    expectNotNil(j, "i: \(i)")
    expectEqual(j.map { cocoa.unicodeScalars[$0] }, scalar, "i: \(i)")
  }
}
#endif

#if _runtime(_ObjC)
suite.test("Index encoding correction/UTF-16→8/conversions/UTF-8") {
  guard #available(SwiftStdlib 5.7, *) else {
    // String indices did not track their encoding until 5.7.
    return
  }
  var s = ("🫱🏼‍🫲🏽 a 🧑🏽‍🌾 b" as NSString) as String

  let utf8Units = s.utf8.indices.map { ($0, s.utf8[$0]) }

  s.makeContiguousUTF8()

  for (i, u8) in utf8Units {
    let j = String.Index(i, within: s.utf8)
    expectNotNil(j, "i: \(i)")
    expectEqual(j.map { s.utf8[$0] }, u8, "i: \(i)")
  }
}
#endif

#if _runtime(_ObjC)
suite.test("Index encoding correction/UTF-8→16/conversions/UTF-8") {
  guard #available(SwiftStdlib 5.7, *) else {
    // String indices did not track their encoding until 5.7.
    return
  }
  let native = "🫱🏼‍🫲🏽 a 🧑🏽‍🌾 b"
  let cocoa = ("🫱🏼‍🫲🏽 a 🧑🏽‍🌾 b" as NSString) as String

  let utf8Units = native.utf8.indices.map { ($0, native.utf8[$0]) }

  for (i, u8) in utf8Units {
    let j = String.Index(i, within: cocoa.utf8)
    expectNotNil(j, "i: \(i)")
    expectEqual(j.map { cocoa.utf8[$0] }, u8, "i: \(i)")
  }
}
#endif

#if _runtime(_ObjC)
suite.test("Index encoding correction/UTF-16→8/conversions/UTF-16") {
  guard #available(SwiftStdlib 5.7, *) else {
    // String indices did not track their encoding until 5.7.
    return
  }
  var s = ("🫱🏼‍🫲🏽 a 🧑🏽‍🌾 b" as NSString) as String

  let utf16Units = s.utf16.indices.map { ($0, s.utf16[$0]) }

  s.makeContiguousUTF8()

  for (i, u16) in utf16Units {
    let j = String.Index(i, within: s.utf16)
    expectNotNil(j, "i: \(i)")
    expectEqual(j.map { s.utf16[$0] }, u16, "i: \(i)")
  }
}
#endif

#if _runtime(_ObjC)
suite.test("Index encoding correction/UTF-8→16/conversions/UTF-16") {
  guard #available(SwiftStdlib 5.7, *) else {
    // String indices did not track their encoding until 5.7.
    return
  }
  let native = "🫱🏼‍🫲🏽 a 🧑🏽‍🌾 b"
  let cocoa = ("🫱🏼‍🫲🏽 a 🧑🏽‍🌾 b" as NSString) as String

  let utf16Units = native.utf16.indices.map { ($0, native.utf16[$0]) }

  for (i, u16) in utf16Units {
    let j = String.Index(i, within: cocoa.utf16)
    expectNotNil(j, "i: \(i)")
    expectEqual(j.map { cocoa.utf16[$0] }, u16, "i: \(i)")
  }
}
#endif

suite.test("UTF-16 breadcrumbs") {

  let string = #"""
    The powerful programming language that is also easy to learn.
    손쉽게 학습할 수 있는 강력한 프로그래밍 언어.
    🪙 A 🥞 short 🍰 piece 🫘 of 🌰 text 👨‍👨‍👧‍👧 with 👨‍👩‍👦 some 🚶🏽 emoji 🇺🇸🇨🇦 characters 🧈
    some🔩times 🛺 placed 🎣 in 🥌 the 🆘 mid🔀dle 🇦🇶or🏁 around 🏳️‍🌈 a 🍇 w🍑o🥒r🥨d
    Unicode is such fun!
    U̷n̷i̷c̷o̴d̴e̷ ̶i̸s̷ ̸s̵u̵c̸h̷ ̸f̵u̷n̴!̵
    U̴̡̲͋̾n̵̻̳͌ì̶̠̕c̴̭̈͘ǫ̷̯͋̊d̸͖̩̈̈́ḛ̴́ ̴̟͎͐̈i̴̦̓s̴̜̱͘ ̶̲̮̚s̶̙̞͘u̵͕̯̎̽c̵̛͕̜̓h̶̘̍̽ ̸̜̞̿f̵̤̽ṷ̴͇̎͘ń̷͓̒!̷͍̾̚
    U̷̢̢̧̨̼̬̰̪͓̞̠͔̗̼̙͕͕̭̻̗̮̮̥̣͉̫͉̬̲̺͍̺͊̂ͅ\#
    n̶̨̢̨̯͓̹̝̲̣̖̞̼̺̬̤̝̊̌́̑̋̋͜͝ͅ\#
    ḭ̸̦̺̺͉̳͎́͑\#
    c̵̛̘̥̮̙̥̟̘̝͙̤̮͉͔̭̺̺̅̀̽̒̽̏̊̆͒͌̂͌̌̓̈́̐̔̿̂͑͠͝͝ͅ\#
    """#

  print(string.utf16.count)
  let indices = Array(string.utf16.indices) + [string.utf16.endIndex]
  for i in 0 ..< indices.count {
    for j in 0 ..< indices.count {
      let distance = string.utf16.distance(from: indices[i], to: indices[j])
      expectEqual(distance, j - i,
        """
        i: \(i), indices[i]: \(indices[i]._description)
        j: \(j), indices[j]: \(indices[j]._description)
        """)

      let target = string.utf16.index(indices[i], offsetBy: j - i)
      expectEqual(target, indices[j],
        """
        i: \(i), indices[i]: \(indices[i]._description)
        j: \(j), indices[j]: \(indices[j]._description)
        target: \(target._description)
        """)
    }
  }
}

suite.test("String.replaceSubrange index validation")
.forEach(in: examples) { string in
  guard #available(SwiftStdlib 5.7, *) else {
    // Index navigation in 5.7 always rounds input indices down to the nearest
    // Character, so that we always have a well-defined distance between
    // indices, even if they aren't valid.
    //
    // 5.6 and below did not behave consistently in this case.
    return
  }

  //string.dumpIndices()

  let scalarMap = string.scalarMap()
  let allIndices = string.allIndices()

  for i in allIndices {
    for j in allIndices {
      guard i <= j else { continue }
      let si = scalarMap[i]!.index
      let sj = scalarMap[j]!.index

      // Check String.replaceSubrange(_:with:)
      do {
        let replacement = "x"

        var expected = "".unicodeScalars
        expected += string.unicodeScalars[..<si]
        expected += replacement.unicodeScalars
        expected += string.unicodeScalars[sj...]

        var actual = string
        actual.replaceSubrange(i ..< j, with: replacement)

        expectEqual(actual, String(expected),
          """
          string: \(string.debugDescription)
          i:      \(i)
          j:      \(j)
          """)
      }

      // Check String.unicodeScalars.replaceSubrange(_:with:)
      do {
        let replacement = "x".unicodeScalars

        var expected = "".unicodeScalars
        expected += string.unicodeScalars[..<si]
        expected += replacement
        expected += string.unicodeScalars[sj...]

        var actual = string
        actual.unicodeScalars.replaceSubrange(i ..< j, with: replacement)

        expectEqual(actual, String(expected),
          """
          string: \(string.debugDescription)
          i:      \(i)
          j:      \(j)
          """)
      }
    }
  }
}

suite.test("Substring.removeSubrange entire range") {
  guard #available(SwiftStdlib 5.8, *) else {
    // This was a regression in 5.7.0, fixed in 5.7.1+
    return
  }

  var a: Substring = "abcdef"
  let aStart = a.startIndex
  let aEnd = a.endIndex

  a.removeSubrange(aStart ..< aEnd)

  expectTrue(a.isEmpty)

#if _runtime(_ObjC)
  var b: Substring = ("å∫ç∂éƒ" as NSString) as Substring
  let bStart = b.startIndex
  let bEnd = b.endIndex

  b.removeSubrange(bStart ..< bEnd)

  expectTrue(b.isEmpty)
#endif
}

if #available(SwiftStdlib 5.8, *) {
  suite.test("String index rounding/Characters")
  .forEach(in: examples) { string in
    for index in string.allIndices(includingEnd: true) {
      let end = string.endIndex
      let expected = (index < end
        ? string.indices.lastIndex { $0 <= index }!
        : end)
      let actual = string._index(roundingDown: index)
      expectEqual(actual, expected,
        """
        index: \(index._description)
        actual: \(actual._description)
        expected: \(expected._description)
        """)
    }
  }
}

suite.test("String index rounding/Scalars")
.forEach(in: examples) { string in
  for index in string.allIndices(includingEnd: true) {
    let end = string.unicodeScalars.endIndex
    let expected = (index < end
      ? string.unicodeScalars.indices.lastIndex { $0 <= index }!
      : end)
    let actual = string.unicodeScalars._index(roundingDown: index)
    expectEqual(actual, expected,
      """
      index: \(index._description)
      actual: \(actual._description)
      expected: \(expected._description)
      """)
  }
}

suite.test("String index rounding/UTF-16")
.forEach(in: examples) { string in
  //string.dumpIndices()
  var utf16Indices = Set(string.utf16.indices)
  utf16Indices.insert(string.utf16.endIndex)

  for index in string.allIndices(includingEnd: true) {
    let expected: String.Index
    if utf16Indices.contains(index) {
      expected = index
    } else {
      // If the index isn't valid in the UTF-16 view, it gets rounded down
      // to the nearest scalar boundary. (Unintuitively, this is generally *not*
      // the closest valid index within the UTF-16 view.)
      expected = string.unicodeScalars.indices.lastIndex { $0 <= index }!
    }
    let actual = string.utf16._index(roundingDown: index)
    expectEqual(actual, expected,
      """
      index: \(index._description)
      actual: \(actual._description)
      expected: \(expected._description)
      """)
  }
}

suite.test("String index rounding/UTF-8")
.forEach(in: examples) { string in
  //string.dumpIndices()
  var utf8Indices = Set(string.utf8.indices)
  utf8Indices.insert(string.utf8.endIndex)
  for index in string.allIndices(includingEnd: true) {
    let expected: String.Index
    if utf8Indices.contains(index) {
      expected = index
    } else {
      // If the index isn't valid in the UTF-8 view, it gets rounded down
      // to the nearest scalar boundary. (Unintuitively, this is generally *not*
      // the closest valid index within the UTF-8 view.)
      expected = string.unicodeScalars.indices.lastIndex { $0 <= index }!
    }
    let actual = string.utf8._index(roundingDown: index)
    expectEqual(actual, expected,
      """
      index: \(index._description)
      actual: \(actual._description)
      expected: \(expected._description)
      """)
  }
}
