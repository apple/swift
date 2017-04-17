// The tests in this file need to be audited and revised or removed for the new
// string design
//
// Run the tests for the whole String prototype
// RUN: %target-run-simple-swift
// REQUIRES: executable_test

import Swift
import Darwin
import SwiftShims
import StdlibUnittest

//
// WIP: for debugging use within the prototype, only
//
var _printDebugging = false
func _debug(_ s: Swift.String) {
  guard _printDebugging else { return }
  print(s)
}
func _withDebugging(_ f: ()->()) {
  let prior = _printDebugging
  _printDebugging = true
  f()
  _printDebugging = prior
}

// var testSuite = TestSuite("t")

// testSuite.test("basic") {
//   do {
//     // We happen to know that alphabet is non-ASCII, but we're not going to say
//     // anything about that.
//     let alphabet = String(latin1: Latin1String(codeUnits: s8.prefix(27), encodedWith: Latin1.self))
//     expectTrue(alphabet.isASCII(scan: true))
//     expectFalse(alphabet.isASCII(scan: false))

//     // We know that if you interpret s8 as Latin1, it has a lot of non-ASCII
//     let nonASCII = String(latin1: Latin1String(codeUnits: s8, encodedWith: Latin1.self))
//     expectFalse(nonASCII.isASCII(scan: true))
//     expectFalse(nonASCII.isASCII(scan: false))
//   }
// }

// testSuite.test("SwiftCanonicalString") {
//   let s = "abcdefghijklmnopqrstuvwxyz\n"
//   + "🇸🇸🇬🇱🇱🇸🇩🇯🇺🇸\n"
//   + "Σὲ 👥🥓γνωρίζω ἀπὸ τὴν κόψη χαῖρε, ὦ χαῖρε, ᾿Ελευθεριά!\n"
//   + "Οὐχὶ ταὐτὰ παρίσταταί μοι γιγνώσκειν, ὦ ἄνδρες ᾿Αθηναῖοι,\n"
//   + "გთხოვთ ახლავე გაიაროთ რეგისტრაცია Unicode-ის მეათე საერთაშორისო\n"
//   + "Зарегистрируйтесь сейчас на Десятую Международную Конференцию по\n"
//   + "  ๏ แผ่นดินฮั่นเสื่อมโทรมแสนสังเวช  พระปกเกศกองบู๊กู้ขึ้นใหม่\n"
//   + "ᚻᛖ ᚳᚹᚫᚦ ᚦᚫᛏ ᚻᛖ ᛒᚢᛞᛖ ᚩᚾ ᚦᚫᛗ ᛚᚪᚾᛞᛖ ᚾᚩᚱᚦᚹᛖᚪᚱᛞᚢᛗ ᚹᛁᚦ ᚦᚪ ᚹᛖᛥᚫ"
//   let s32 = s.unicodeScalars.lazy.map { $0.value }
//   let s16 = Array(s.utf16)
//   let s8 = Array(s.utf8)
//   let s16to32 = _UnicodeViews.TranscodedView(s16, from: UTF16.self, to: UTF32.self)
//   let s16to8 = _UnicodeViews.TranscodedView(s16, from: UTF16.self, to: UTF8.self)
//   let s8to16 = _UnicodeViews.TranscodedView(s8, from: UTF8.self, to: UTF16.self)
//   let _ = _UnicodeViews.TranscodedView(s8, from: ValidUTF8.self, to: UTF16.self)

//   let sncFrom32 = String(canonical: SwiftCanonicalString(
//     codeUnits: Array(s32), encodedWith: UTF32.self
//   ))
//   let sncFrom16 = String(canonical: SwiftCanonicalString(
//     codeUnits: s16, encodedWith: UTF16.self
//   ))
//   let sncFrom8 = String(canonical: SwiftCanonicalString(
//     codeUnits: Array(s8), encodedWith: UTF8.self
//   ))
//   let sncFrom16to32 = String(canonical: SwiftCanonicalString(
//     codeUnits: Array(s16to32), encodedWith: UTF32.self
//   ))
//   let sncFrom16to8 = String(canonical: SwiftCanonicalString(
//     codeUnits: Array(s16to8), encodedWith: UTF8.self
//   ))
//   let sncFrom8to16 = String(canonical: SwiftCanonicalString(
//     codeUnits: Array(s8to16), encodedWith: UTF16.self
//   ))

//   expectEqual(sncFrom32, sncFrom16)
//   expectEqual(sncFrom16, sncFrom8)
//   expectEqual(sncFrom8, sncFrom16to32)
//   expectEqual(sncFrom16to32, sncFrom16to8)
//   expectEqual(sncFrom16to8, sncFrom8to16)
// }

// testSuite.test("substring") {
//   let s: String = "hello world"
//   let worldRange: Range = s.index(s.startIndex, offsetBy: 6)..<s.endIndex
//   expectEqualSequence("world" as String, s[worldRange] as Substring)
//   expectEqualSequence("world" as String, s[worldRange] as String)

//   var tail = s.dropFirst()
//   expectType(Substring.self, &tail)
//   expectEqualSequence("ello world", tail)
// }

// typealias _String = Swift.String

// testSuite.test("fcc-normalized-view") {
//   let a: UInt16 = 0x0061
//   let aTic: UInt16 = 0x00e0
//   let aBackTic: UInt16 = 0x00e1
//   typealias UTF16String = _UnicodeViews<[UInt16], UTF16>
//   typealias NormalizedView = FCCNormalizedUTF16View_2<[UInt16], UTF16>

//   // Helper old/new functions, eagerly forms arrays of the forwards and reverse
//   // FCC normalized UTF16 code units
//   func oldFCCNormView(_ codeUnits: [UInt16])
//     -> (forward: [UInt16], reversed: [UInt16]) {
//     let view = UTF16String(codeUnits).fccNormalizedUTF16
//     return (forward: Array(view),
//             reversed: Array(view.reversed()))
//   }
//   func newFCCNormView(_ codeUnits: [UInt16])
//     -> (forward: [UInt16], reversed: [UInt16]) {
//     let view = NormalizedView(FCCNormalizedLazySegments(UTF16String(codeUnits)))
//     return (forward: Array(view),
//             reversed: Array(view.reversed()))
//   }

//   // Test canonical equivalence for:
//   //   1) a + ̀ + ́ == à + ́
//   //   2) a + ́ + ̀ == á + ̀
//   // BUT, the two are distinct, #1 != #2
//   do {
//     let str1form1 = [a, 0x0300, 0x0301]
//     let str1form2 = [aTic, 0x0301]
//     let str2form1 = [a, 0x0301, 0x0300]
//     let str2form2 = [aBackTic, 0x0300]

//     let (norm1_1, norm1_1rev) = oldFCCNormView(str1form1)
//     let (norm1_2, norm1_2rev) = oldFCCNormView(str1form2)
//     let (norm2_1, norm2_1rev) = oldFCCNormView(str2form1)
//     let (norm2_2, norm2_2rev) = oldFCCNormView(str2form2)

//     expectEqualSequence(norm1_1, norm1_2)
//     expectEqualSequence(norm2_1, norm2_2)
//     for (cu1, cu2) in zip(norm1_1, norm2_1) {
//       expectNotEqual(cu1, cu2)
//     }

//     let (newNorm1_1, newNorm1_1rev) = newFCCNormView(str1form1)
//     let (newNorm1_2, newNorm1_2rev) = newFCCNormView(str1form2)
//     let (newNorm2_1, newNorm2_1rev) = newFCCNormView(str2form1)
//     let (newNorm2_2, newNorm2_2rev) = newFCCNormView(str2form2)

//     expectEqualSequence(newNorm1_1, newNorm1_2)
//     expectEqualSequence(newNorm1_1, norm1_1)
//     expectEqualSequence(newNorm2_1, newNorm2_2)
//     expectEqualSequence(newNorm2_1, norm2_1)

//     // Test other direction
//     expectEqualSequence(newNorm1_1rev, newNorm1_2rev)
//     expectEqualSequence(newNorm1_1rev, norm1_1rev)
//     expectEqualSequence(newNorm2_1rev, newNorm2_2rev)
//     expectEqualSequence(newNorm2_1rev, norm2_1rev)
//   }

//   // Test canonical equivalence, and non-combining-ness of FCC for:
//   //   1) a + ̖ + ̀ == à + ̖ == a + ̀ + ̖
//   //   All will normalize under FCC as a + ̖ + ̀
//   do {
//     let form1 = [a, 0x0316, 0x0300]
//     let form2 = [a, 0x0300, 0x0316]
//     let form3 = [aTic, 0x0316]

//     let (norm1, norm1rev) = oldFCCNormView(form1)
//     let (norm2, norm2rev) = oldFCCNormView(form2)
//     let (norm3, norm3rev) = oldFCCNormView(form3)

//     // Sanity check existing normal form yields same results
//     expectEqualSequence(norm1, norm2)
//     expectEqualSequence(norm2, norm3)

//     // Form 1 is already in FCC
//     expectEqualSequence(norm3, form1)

//     // Test the new one
//     let (newNorm1, newNorm1rev) = newFCCNormView(form1)
//     let (newNorm2, newNorm2rev) = newFCCNormView(form2)
//     let (newNorm3, newNorm3rev) = newFCCNormView(form3)

//     expectEqualSequence(newNorm1, newNorm2)
//     expectEqualSequence(newNorm2, newNorm3)
//     expectEqualSequence(newNorm3, norm3)

//     // And in reverse
//     expectEqualSequence(newNorm1rev, newNorm2rev)
//     expectEqualSequence(newNorm2rev, newNorm3rev)
//     expectEqualSequence(newNorm3rev, norm3rev)
//   }

//   // Test non-start first scalars
//   do {
//     let form1 = [0x0300, a, 0x0300]
//     let form2 = [0x0300, aTic] // In FCC normal form
//     let (norm1, norm1rev) = oldFCCNormView(form1)
//     let (norm2, norm2rev) = oldFCCNormView(form2)

//     // Sanity check existing impl
//     expectEqualSequence(norm1, norm2)
//     expectEqualSequence(norm1rev, norm2rev)
//     expectEqualSequence(norm1, form2)

//     // Test the new one
//     let (newNorm1, newNorm1rev) = newFCCNormView(form1)
//     let (newNorm2, newNorm2rev) = newFCCNormView(form2)
//     expectEqualSequence(newNorm1, newNorm2)
//     expectEqualSequence(newNorm2, norm2)
//     expectEqualSequence(newNorm1rev, newNorm2rev)
//     expectEqualSequence(newNorm2rev, norm2rev)
//   }

//   do {
//     // Test that the new normalizer is same result as old normalizer
//     let s = "abcdefghijklmnopqrstuvwxyz\n"
//     + "🇸🇸🇬🇱🇱🇸🇩🇯🇺🇸\n"
//     + "Σὲ 👥🥓γνωρίζω ἀπὸ τὴν κόψη χαῖρε, ὦ χαῖρε, ᾿Ελευθεριά!\n"
//     + "Οὐχὶ ταὐτὰ παρίσταταί μοι γιγνώσκειν, ὦ ἄνδρες ᾿Αθηναῖοι,\n"
//     + "გთხოვთ ახლავე გაიაროთ რეგისტრაცია Unicode-ის მეათე საერთაშორისო\n"
//     + "Зарегистрируйтесь сейчас на Десятую Международную Конференцию по\n"
//     + "  ๏ แผ่นดินฮั่นเสื่อมโทรมแสนสังเวช  พระปกเกศกองบู๊กู้ขึ้นใหม่\n"
//     + "ᚻᛖ ᚳᚹᚫᚦ ᚦᚫᛏ ᚻᛖ ᛒᚢᛞᛖ ᚩᚾ ᚦᚫᛗ ᛚᚪᚾᛞᛖ ᚾᚩᚱᚦᚹᛖᚪᚱᛞᚢᛗ ᚹᛁᚦ ᚦᚪ ᚹᛖᛥᚫ"
//     let s16 = Array(s.utf16)

//     let (norm1, norm1rev) = oldFCCNormView(s16)
//     let (newNorm1, newNorm1rev) = newFCCNormView(s16)

//     expectEqualSequence(norm1, newNorm1)
//     expectEqualSequence(norm1rev, newNorm1rev)

//     expectTrue(norm1 != newNorm1rev)
//   }
// }

// import Foundation
// testSuite.test("bridging") {
//   defer { _debugLogging = false }
//   _debugLogging = false
//   let s : main.String
//     = "abc\n🇸🇸🇬🇱🇱🇸🇩🇯🇺🇸\nΣὲ 👥🥓γνωρίζω\nგთხოვთ\nงบู๊กู้ขึ้นม่\nᚹᛖᛥᚫ"
//   let n = s as NSString
//   let s2 = n as main.String
//   expectEqual(s, s2)
// }

// runAllTests()
