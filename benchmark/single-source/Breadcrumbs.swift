//===--- Breadcrumbs.swift ------------------------------------*- swift -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import TestsUtils

// Tests the performance of String's memoized UTF-8 to UTF-16 index conversion
// breadcrumbs. These are used to speed up range- and positional access through
// conventional NSString APIs.

public let benchmarks: [BenchmarkInfo] = [
  UTF16ToIdx(workload: longASCIIWorkload, count: 5_000).info,
  UTF16ToIdx(workload: longMixedWorkload, count: 5_000).info,
  IdxToUTF16(workload: longASCIIWorkload, count: 5_000).info,
  IdxToUTF16(workload: longMixedWorkload, count: 5_000).info,

  UTF16ToIdxRange(workload: longASCIIWorkload, count: 1_000).info,
  UTF16ToIdxRange(workload: longMixedWorkload, count: 1_000).info,
  IdxToUTF16Range(workload: longASCIIWorkload, count: 1_000).info,
  IdxToUTF16Range(workload: longMixedWorkload, count: 1_000).info,

  CopyUTF16CodeUnits(workload: asciiWorkload, count: 500).info,
  CopyUTF16CodeUnits(workload: mixedWorkload, count: 500).info,
  CopyUTF16CodeUnits(workload: longMixedWorkload, count: 50).info,
  
  CopyAllUTF16CodeUnits(workload: asciiWorkload, count: 1_000).info,
  CopyAllUTF16CodeUnits(workload: mixedWorkload, count: 100).info,
  CopyAllUTF16CodeUnits(workload: longASCIIWorkload, count: 7).info,
  CopyAllUTF16CodeUnits(workload: longMixedWorkload, count: 1).info,

  MutatedUTF16ToIdx(workload: asciiWorkload, count: 50).info,
  MutatedUTF16ToIdx(workload: mixedWorkload, count: 50).info,
  MutatedIdxToUTF16(workload: asciiWorkload, count: 50).info,
  MutatedIdxToUTF16(workload: mixedWorkload, count: 50).info,
]

extension String {
  func forceNativeCopy() -> String {
    var result = String()
    result.reserveCapacity(64)
    result.append(self)
    return result
  }
}

extension Collection {
  /// Returns a randomly ordered array of random non-overlapping index ranges
  /// that cover this collection entirely.
  ///
  /// Note: some of the returned ranges may be empty.
  func randomIndexRanges<R: RandomNumberGenerator>(
    count: Int,
    using generator: inout R
  ) -> [Range<Index>] {
    // Load indices into a buffer to prevent quadratic performance with
    // forward-only collections. FIXME: Eliminate this if Self conforms to RAC.
    let indices = Array(self.indices)
    var cuts: [Index] = (0 ..< count - 1).map { _ in
      indices.randomElement(using: &generator)!
    }
    cuts.append(self.startIndex)
    cuts.append(self.endIndex)
    cuts.sort()
    let ranges = (0 ..< count).map { cuts[$0] ..< cuts[$0 + 1] }
    return ranges.shuffled(using: &generator)
  }
}

struct Workload {
  let name: String
  let string: String
}

class BenchmarkBase {
  let name: String
  let workload: Workload

  var inputString: String = ""

  init(name: String, workload: Workload) {
    self.name = name
    self.workload = workload
  }

  var label: String {
    return "\(name).\(workload.name)"
  }

  func setUp() {
    self.inputString = workload.string.forceNativeCopy()
  }

  func tearDown() {
    self.inputString = ""
  }

  func run(iterations: Int) {
    fatalError("unimplemented abstract method")
  }

  var info: BenchmarkInfo {
    return BenchmarkInfo(
      name: self.label,
      runFunction: self.run(iterations:),
      tags: [.validation, .api, .String],
      setUpFunction: self.setUp,
      tearDownFunction: self.tearDown)
  }
}

//==============================================================================
// Workloads
//==============================================================================

let asciiBase = #"""
    * Debugger support. Swift has a `-g` command line switch that turns on
    debug info for the compiled output. Using the standard lldb debugger
    this will allow single-stepping through Swift programs, printing
    backtraces, and navigating through stack frames; all in sync with
    the corresponding Swift source code. An unmodified lldb cannot
    inspect any variables.

    Example session:

    ```
    $ echo 'println("Hello World")' >hello.swift
    $ swift hello.swift -c -g -o hello.o
    $ ld hello.o "-dynamic" "-arch" "x86_64" "-macosx_version_min" "10.9.0" \
    -framework Foundation lib/swift/libswift_stdlib_core.dylib \
    lib/swift/libswift_stdlib_posix.dylib -lSystem -o hello
    $ lldb hello
    Current executable set to 'hello' (x86_64).
    (lldb) b top_level_code
    Breakpoint 1: where = hello`top_level_code + 26 at hello.swift:1, addre...
    (lldb) r
    Process 38592 launched: 'hello' (x86_64)
    Process 38592 stopped
    * thread #1: tid = 0x1599fb, 0x0000000100000f2a hello`top_level_code + ...
    frame #0: 0x0000000100000f2a hello`top_level_code + 26 at hello.shi...
    -> 1         println("Hello World")
    (lldb) bt
    * thread #1: tid = 0x1599fb, 0x0000000100000f2a hello`top_level_code + ...
    frame #0: 0x0000000100000f2a hello`top_level_code + 26 at hello.shi...
    frame #1: 0x0000000100000f5c hello`main + 28
    frame #2: 0x00007fff918605fd libdyld.dylib`start + 1
    frame #3: 0x00007fff918605fd libdyld.dylib`start + 1
    ```

    Also try `s`, `n`, `up`, `down`.

    * `nil` can now be used without explicit casting. Previously, `nil` had
    type `NSObject`, so one would have to write (e.g.) `nil as! NSArray`
    to create a `nil` `NSArray`. Now, `nil` picks up the type of its
    context.

    * `POSIX.EnvironmentVariables` and `swift.CommandLineArguments` global variables
    were merged into a `swift.Process` variable.  Now you can access command line
    arguments with `Process.arguments`.  In order to access environment variables
    add `import POSIX` and use `Process.environmentVariables`.

    func _toUTF16Offsets(_ indices: Range<Index>) -> Range<Int> {
      let lowerbound = _toUTF16Offset(indices.lowerBound)
      let length = self.utf16.distance(
      from: indices.lowerBound, to: indices.upperBound)
      return Range(
        uncheckedBounds: (lower: lowerbound, upper: lowerbound + length))
    }
    0  swift                    0x00000001036b5f58 llvm::sys::PrintStackTrace(llvm::raw_ostream&) + 40
    1  swift                    0x00000001036b50f8 llvm::sys::RunSignalHandlers() + 248
    2  swift                    0x00000001036b6572 SignalHandler(int) + 258
    3  libsystem_platform.dylib 0x00007fff64010b5d _sigtramp + 29
    4  libsystem_platform.dylib 0x0000000100000000 _sigtramp + 2617177280
    5  libswiftCore.dylib       0x0000000107b5d135 $sSh8_VariantV7element2atxSh5IndexVyx_G_tF + 613
    6  libswiftCore.dylib       0x0000000107c51449 $sShyxSh5IndexVyx_Gcig + 9
    7  libswiftCore.dylib       0x00000001059d60be $sShyxSh5IndexVyx_Gcig + 4258811006
    8  swift                    0x000000010078801d llvm::MCJIT::runFunction(llvm::Function*, llvm::ArrayRef<llvm::GenericValue>) + 381
    9  swift                    0x000000010078b0a4 llvm::ExecutionEngine::runFunctionAsMain(llvm::Function*, std::__1::vector<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char> >, std::__1::allocator<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char> > > > const&, char const* const*) + 1268
    10 swift                    0x00000001000e048c REPLEnvironment::executeSwiftSource(llvm::StringRef, std::__1::vector<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char> >, std::__1::allocator<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char> > > > const&) + 1532
    11 swift                    0x00000001000dbbd3 swift::runREPL(swift::CompilerInstance&, std::__1::vector<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char> >, std::__1::allocator<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char> > > > const&, bool) + 1443
    12 swift                    0x00000001000b5341 performCompile(swift::CompilerInstance&, swift::CompilerInvocation&, llvm::ArrayRef<char const*>, int&, swift::FrontendObserver*, swift::UnifiedStatsReporter*) + 2865
    13 swift                    0x00000001000b38f4 swift::performFrontend(llvm::ArrayRef<char const*>, char const*, void*, swift::FrontendObserver*) + 3028
    14 swift                    0x000000010006ca44 main + 660
    15 libdyld.dylib            0x00007fff63e293f1 start + 1
    16 libdyld.dylib            0x0000000000000008 start + 2619173912
    Illegal instruction: 4

  """#
let asciiWorkload = Workload(
  name: "ASCII",
  string: asciiBase)
let longASCIIWorkload = Workload(
  name: "longASCII",
  string: String(repeating: asciiBase, count: 100))

let mixedBase = """
    siebenhundertsiebenundsiebzigtausendsiebenhundertsiebenundsiebzig
    👍👩‍👩‍👧‍👧👨‍👨‍👦‍👦🇺🇸🇨🇦🇲🇽👍🏻👍🏼👍🏽👍🏾👍🏿
    siebenhundertsiebenundsiebzigtausendsiebenhundertsiebenundsiebzig
    👍👩‍👩‍👧‍👧👨‍👨‍👦‍👦🇺🇸🇨🇦🇲🇽👍🏻👍🏼👍🏽👍🏾👍🏿the quick brown fox👍🏿👍🏾👍🏽👍🏼👍🏻🇲🇽🇨🇦🇺🇸👨‍👨‍👦‍👦👩‍👩‍👧‍👧👍
    siebenhundertsiebenundsiebzigtausendsiebenhundertsiebenundsiebzig
    今回のアップデートでSwiftに大幅な改良が施され、安定していてしかも直感的に使うことができるAppleプラットフォーム向けプログラミング言語になりました。
    Worst thing about working on String is that it breaks *everything*. Asserts, debuggers, and *especially* printf-style debugging 😭
    Swift 是面向 Apple 平台的编程语言，功能强大且直观易用，而本次更新对其进行了全面优化。
    siebenhundertsiebenundsiebzigtausendsiebenhundertsiebenundsiebzig
    이번 업데이트에서는 강력하면서도 직관적인 Apple 플랫폼용 프로그래밍 언어인 Swift를 완벽히 개선하였습니다.
    Worst thing about working on String is that it breaks *everything*. Asserts, debuggers, and *especially* printf-style debugging 😭
    в чащах юга жил-был цитрус? да, но фальшивый экземпляр
    siebenhundertsiebenundsiebzigtausendsiebenhundertsiebenundsiebzig
    \u{201c}Hello\u{2010}world\u{2026}\u{201d}
    \u{300c}\u{300e}今日は\u{3001}世界\u{3002}\u{300f}\u{300d}
    Worst thing about working on String is that it breaks *everything*. Asserts, debuggers, and *especially* printf-style debugging 😭

  """
let mixedWorkload = Workload(
  name: "Mixed",
  string: mixedBase)
let longMixedWorkload = Workload(
  name: "longMixed",
  string: String(repeating: mixedBase, count: 100))


//==============================================================================
// Benchmarks
//==============================================================================

/// Convert `count` random UTF-16 offsets into String indices.
class UTF16ToIdx: BenchmarkBase {
  let count: Int
  var inputOffsets: [Int] = []

  init(workload: Workload, count: Int) {
    self.count = count
    super.init(name: "Breadcrumbs.UTF16ToIdx", workload: workload)
  }

  override func setUp() {
    super.setUp()
    var rng = SplitMix64(seed: 42)
    let range = 0 ..< inputString.utf16.count
    inputOffsets = Array(range.shuffled(using: &rng).prefix(count))
  }

  override func tearDown() {
    super.tearDown()
    inputOffsets = []
  }

  @inline(never)
  override func run(iterations: Int) {
    for _ in 0 ..< iterations {
      for offset in inputOffsets {
        blackHole(inputString._toUTF16Index(offset))
      }
    }
  }
}

/// Convert `count` random String indices into UTF-16 offsets.
class IdxToUTF16: BenchmarkBase {
  let count: Int
  var inputIndices: [String.Index] = []

  init(workload: Workload, count: Int) {
    self.count = count
    super.init(name: "Breadcrumbs.IdxToUTF16", workload: workload)
  }

  override func setUp() {
    super.setUp()
    var rng = SplitMix64(seed: 42)
    inputIndices = Array(inputString.indices.shuffled(using: &rng).prefix(count))
  }

  override func tearDown() {
    super.tearDown()
    inputIndices = []
  }

  @inline(never)
  override func run(iterations: Int) {
    for _ in 0 ..< iterations {
      for index in inputIndices {
        blackHole(inputString._toUTF16Offset(index))
      }
    }
  }
}

/// Split a string into `count` random slices and convert their UTF-16 offsets
/// into String index ranges.
class UTF16ToIdxRange: BenchmarkBase {
  let count: Int
  var inputOffsets: [Range<Int>] = []

  init(workload: Workload, count: Int) {
    self.count = count
    super.init(name: "Breadcrumbs.UTF16ToIdxRange", workload: workload)
  }

  override func setUp() {
    super.setUp()
    var rng = SplitMix64(seed: 42)
    inputOffsets = (
      0 ..< inputString.utf16.count
    ).randomIndexRanges(count: count, using: &rng)
  }

  override func tearDown() {
    super.tearDown()
    inputOffsets = []
  }

  @inline(never)
  override func run(iterations: Int) {
    for _ in 0 ..< iterations {
      for range in inputOffsets {
        blackHole(inputString._toUTF16Indices(range))
      }
    }
  }
}

/// Split a string into `count` random slices and convert their index ranges
/// into UTF-16 offset pairs.
class IdxToUTF16Range: BenchmarkBase {
  let count: Int
  var inputIndices: [Range<String.Index>] = []

  init(workload: Workload, count: Int) {
    self.count = count
    super.init(name: "Breadcrumbs.IdxToUTF16Range", workload: workload)
  }

  override func setUp() {
    super.setUp()
    var rng = SplitMix64(seed: 42)
    inputIndices = self.inputString.randomIndexRanges(count: count, using: &rng)
  }

  override func tearDown() {
    super.tearDown()
    inputIndices = []
  }

  @inline(never)
  override func run(iterations: Int) {
    for _ in 0 ..< iterations {
      for range in inputIndices {
        blackHole(inputString._toUTF16Offsets(range))
      }
    }
  }
}


class CopyUTF16CodeUnits: BenchmarkBase {
  let count: Int
  var inputIndices: [Range<Int>] = []
  var outputBuffer: [UInt16] = []

  init(workload: Workload, count: Int) {
    self.count = count
    super.init(name: "Breadcrumbs.CopyUTF16CodeUnits", workload: workload)
  }
  
  init(name: String, workload: Workload, count: Int) {
    self.count = count
    super.init(name: name, workload: workload)
  }

  override func setUp() {
    super.setUp()
    var rng = SplitMix64(seed: 42)
    inputIndices = (
      0 ..< inputString.utf16.count
    ).randomIndexRanges(count: count, using: &rng)
    outputBuffer = Array(repeating: 0, count: inputString.utf16.count)
  }

  override func tearDown() {
    super.tearDown()
    inputIndices = []
  }

  @inline(never)
  override func run(iterations: Int) {
    outputBuffer.withUnsafeMutableBufferPointer { buffer in
      for _ in 0 ..< iterations {
        for range in inputIndices {
          inputString._copyUTF16CodeUnits(
            into: UnsafeMutableBufferPointer(rebasing: buffer[range]),
            range: range)
        }
      }
    }
  }
}

class CopyAllUTF16CodeUnits : CopyUTF16CodeUnits {
  override init(workload: Workload, count: Int) {
    super.init(
      name: "Breadcrumbs.CopyAllUTF16CodeUnits",
      workload: workload,
      count: count
    )
  }
  
  override func setUp() {
    super.setUp()
    inputIndices = Array(repeating: 0 ..< inputString.utf16.count, count: count)
  }
}

/// This is like `UTF16ToIdx` but appends to the string after every index
/// conversion. In effect, this tests breadcrumb creation performance.
class MutatedUTF16ToIdx: BenchmarkBase {
  let count: Int
  var inputOffsets: [Int] = []

  init(workload: Workload, count: Int) {
    self.count = count
    super.init(
      name: "Breadcrumbs.MutatedUTF16ToIdx",
      workload: workload)
  }

  override func setUp() {
    super.setUp()
    var generator = SplitMix64(seed: 42)
    let range = 0 ..< inputString.utf16.count
    inputOffsets = Array(range.shuffled(using: &generator).prefix(count))
  }

  override func tearDown() {
    super.tearDown()
    inputOffsets = []
  }

  @inline(never)
  override func run(iterations: Int) {
    var flag = true
    for _ in 0 ..< iterations {
      var string = inputString
      for offset in inputOffsets {
        blackHole(string._toUTF16Index(offset))
        if flag {
          string.append(" ")
        } else {
          string.removeLast()
        }
        flag.toggle()
      }
    }
  }
}


/// This is like `IdxToUTF16` but appends to the string after every index
/// conversion. In effect, this tests breadcrumb creation performance.
class MutatedIdxToUTF16: BenchmarkBase {
  let count: Int
  var inputIndices: [String.Index] = []

  init(workload: Workload, count: Int) {
    self.count = count
    super.init(
      name: "Breadcrumbs.MutatedIdxToUTF16",
      workload: workload)
  }

  override func setUp() {
    super.setUp()
    var rng = SplitMix64(seed: 42)
    inputIndices = Array(inputString.indices.shuffled(using: &rng).prefix(count))
  }

  override func tearDown() {
    super.tearDown()
    inputIndices = []
  }

  @inline(never)
  override func run(iterations: Int) {
    var flag = true
    for _ in 0 ..< iterations {
      var string = inputString
      for index in inputIndices {
        blackHole(string._toUTF16Offset(index))
        if flag {
          string.append(" ")
          flag = false
        } else {
          string.removeLast()
          flag = true
        }
      }
    }
  }
}
