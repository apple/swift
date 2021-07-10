//===--- TestsUtils.swift -------------------------------------------------===//
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

#if os(Linux)
import Glibc
#elseif os(Windows)
import MSVCRT
#else
import Darwin
#endif

public enum BenchmarkCategory : String {
  // Validation "micro" benchmarks test a specific operation or critical path that
  // we know is important to measure.
  case validation
  // subsystems to validate and their subcategories.
  case api, Array, String, Dictionary, Codable, Set, Data, IndexPath, SIMD
  case sdk
  case runtime, refcount, metadata
  // Other general areas of compiled code validation.
  case abstraction, safetychecks, exceptions, bridging, concurrency, existential
  case exclusivity, differentiation

  // Algorithms are "micro" that test some well-known algorithm in isolation:
  // sorting, searching, hashing, fibonaci, crypto, etc.
  case algorithm

  // Miniapplications are contrived to mimic some subset of application behavior
  // in a way that can be easily measured. They are larger than micro-benchmarks,
  // combining multiple APIs, data structures, or algorithms. This includes small
  // standardized benchmarks, pieces of real applications that have been extracted
  // into a benchmark, important functionality like JSON parsing, etc.
  case miniapplication

  // Regression benchmarks is a catch-all for less important "micro"
  // benchmarks. This could be a random piece of code that was attached to a bug
  // report. We want to make sure the optimizer as a whole continues to handle
  // this case, but don't know how applicable it is to general Swift performance
  // relative to the other micro-benchmarks. In particular, these aren't weighted
  // as highly as "validation" benchmarks and likely won't be the subject of
  // future investigation unless they significantly regress.
  case regression

  // Most benchmarks are assumed to be "stable" and will be regularly tracked at
  // each commit. A handful may be marked unstable if continually tracking them is
  // counterproductive.
  case unstable

  // CPU benchmarks represent instrinsic Swift performance. They are useful for
  // measuring a fully baked Swift implementation across different platforms and
  // hardware. The benchmark should also be reasonably applicable to real Swift
  // code--it should exercise a known performance critical area. Typically these
  // will be drawn from the validation benchmarks once the language and standard
  // library implementation of the benchmark meets a reasonable efficiency
  // baseline. A benchmark should only be tagged "cpubench" after a full
  // performance investigation of the benchmark has been completed to determine
  // that it is a good representation of future Swift performance. Benchmarks
  // should not be tagged if they make use of an API that we plan on
  // reimplementing or call into code paths that have known opportunities for
  // significant optimization.
  case cpubench

  // Explicit skip marker
  case skip
}

extension BenchmarkCategory : CustomStringConvertible {
  public var description: String {
    return self.rawValue
  }
}

extension BenchmarkCategory : Comparable {
    public static func < (lhs: BenchmarkCategory, rhs: BenchmarkCategory) -> Bool {
        return lhs.rawValue < rhs.rawValue
    }
}

public struct BenchmarkPlatformSet : OptionSet {
  public let rawValue: Int

  public init(rawValue: Int) {
    self.rawValue = rawValue
  }

  public static let darwin = BenchmarkPlatformSet(rawValue: 1 << 0)
  public static let linux = BenchmarkPlatformSet(rawValue: 1 << 1)

  public static var currentPlatform: BenchmarkPlatformSet {
    #if os(Linux)
      return .linux
    #else
      return .darwin
    #endif
  }

  public static var allPlatforms: BenchmarkPlatformSet {
    return [.darwin, .linux]
  }
}

public struct BenchmarkInfo {
  /// The name of the benchmark that should be displayed by the harness.
  public var name: String

  /// Shadow static variable for runFunction.
  private var _runFunction: (Int) async -> ()

  /// A function that invokes the specific benchmark routine.
  public var runFunction: ((Int) async -> ())? {
    if !shouldRun {
      return nil
    }
    return _runFunction
  }

  /// A set of category tags that describe this benchmark. This is used by the
  /// harness to allow for easy slicing of the set of benchmarks along tag
  /// boundaries, e.x.: run all string benchmarks or ref count benchmarks, etc.
  public var tags: Set<BenchmarkCategory>

  /// The platforms that this benchmark supports. This is an OptionSet.
  private var unsupportedPlatforms: BenchmarkPlatformSet

  /// Shadow variable for setUpFunction.
  private var _setUpFunction: (() -> ())?

  /// An optional function that if non-null is run before benchmark samples
  /// are timed.
  public var setUpFunction : (() -> ())? {
    if !shouldRun {
      return nil
    }
    return _setUpFunction
  }

  /// Shadow static variable for computed property tearDownFunction.
  private var _tearDownFunction: (() -> ())?

  /// An optional function that if non-null is run after samples are taken.
  public var tearDownFunction: (() -> ())? {
    if !shouldRun {
      return nil
    }
    return _tearDownFunction
  }

  /// DON'T USE ON NEW BENCHMARKS!
  /// Optional `legacyFactor` is a multiplication constant applied to runtime
  /// statistics reported in the benchmark summary (it doesn’t affect the
  /// individual sample times reported in `--verbose` mode).
  ///
  /// It enables the migration of benchmark suite to smaller workloads (< 1 ms),
  /// which are more robust to measurement errors from system under load,
  /// while maintaining the continuity of longterm benchmark tracking.
  ///
  /// Most legacy benchmarks had workloads artificially inflated in their main
  /// `for` loops with a constant integer factor and the migration consisted of
  /// dividing it so that the optimized runtime (-O) was less than 1000 μs and
  /// storing the divisor in `legacyFactor`. This effectively only increases the
  /// frequency of measurement, gathering more samples that are much less likely
  /// to be interrupted by a context switch.
  public var legacyFactor: Int?

  public init(name: String, runFunction: @escaping (Int) async -> (), tags: [BenchmarkCategory],
              setUpFunction: (() -> ())? = nil,
              tearDownFunction: (() -> ())? = nil,
              unsupportedPlatforms: BenchmarkPlatformSet = [],
              legacyFactor: Int? = nil) {
    self.name = name
    self._runFunction = runFunction
    self.tags = Set(tags)
    self._setUpFunction = setUpFunction
    self._tearDownFunction = tearDownFunction
    self.unsupportedPlatforms = unsupportedPlatforms
    self.legacyFactor = legacyFactor
  }

  /// Returns true if this benchmark should be run on the current platform.
  var shouldRun: Bool {
    return !unsupportedPlatforms.contains(.currentPlatform)
  }
}

extension BenchmarkInfo : Comparable {
  public static func < (lhs: BenchmarkInfo, rhs: BenchmarkInfo) -> Bool {
    return lhs.name < rhs.name
  }
  public static func == (lhs: BenchmarkInfo, rhs: BenchmarkInfo) -> Bool {
    return lhs.name == rhs.name
  }
}

extension BenchmarkInfo : Hashable {
  public func hash(into hasher: inout Hasher) {
    hasher.combine(name)
  }
}

// Linear function shift register.
//
// This is just to drive benchmarks. I don't make any claim about its
// strength. According to Wikipedia, it has the maximal period for a
// 32-bit register.
struct LFSR {
  // Set the register to some seed that I pulled out of a hat.
  var lfsr : UInt32 = 0xb78978e7

  mutating func shift() {
    lfsr = (lfsr >> 1) ^ (UInt32(bitPattern: -Int32((lfsr & 1))) & 0xD0000001)
  }
  mutating func randInt() -> Int64 {
    var result : UInt32 = 0
    for _ in 0..<32 {
      result = (result << 1) | (lfsr & 1)
      shift()
    }
    return Int64(bitPattern: UInt64(result))
  }
}

var lfsrRandomGenerator = LFSR()

// Start the generator from the beginning
public func SRand() {
  lfsrRandomGenerator = LFSR()
}

public func Random() -> Int64 {
  return lfsrRandomGenerator.randInt()
}

// This is a fixed-increment version of Java 8's SplittableRandom generator.
// It is a very fast generator passing BigCrush, with 64 bits of state.
// See http://dx.doi.org/10.1145/2714064.2660195 and
// http://docs.oracle.com/javase/8/docs/api/java/util/SplittableRandom.html
//
// Derived from public domain C implementation by Sebastiano Vigna
// See http://xoshiro.di.unimi.it/splitmix64.c
public struct SplitMix64: RandomNumberGenerator {
    private var state: UInt64

    public init(seed: UInt64) {
        self.state = seed
    }

    public mutating func next() -> UInt64 {
        self.state &+= 0x9e3779b97f4a7c15
        var z: UInt64 = self.state
        z = (z ^ (z &>> 30)) &* 0xbf58476d1ce4e5b9
        z = (z ^ (z &>> 27)) &* 0x94d049bb133111eb
        return z ^ (z &>> 31)
    }
}

@inlinable // FIXME(inline-always)
@inline(__always)
public func CheckResults(
    _ resultsMatch: Bool,
    file: StaticString = #file,
    function: StaticString = #function,
    line: Int = #line
    ) {
    guard _fastPath(resultsMatch) else {
        print("Incorrect result in \(function), \(file):\(line)")
        abort()
    }
}

#if !_runtime(_ObjC)
// If we do not have an objc-runtime, then we do not have a definition for
// autoreleasepool. Add in our own fake autoclosure for it that is inline
// always. That should be able to be eaten through by the optimizer no problem.
@inlinable // FIXME(inline-always)
@inline(__always)
public func autoreleasepool<Result>(
  invoking body: () throws -> Result
) rethrows -> Result {
  return try body()
}
#endif

public func False() -> Bool { return false }

/// This is a dummy protocol to test the speed of our protocol dispatch.
public protocol SomeProtocol { func getValue() -> Int }
struct MyStruct : SomeProtocol {
  init() {}
  func getValue() -> Int { return 1 }
}
public func someProtocolFactory() -> SomeProtocol { return MyStruct() }

// Just consume the argument.
// It's important that this function is in another module than the tests
// which are using it.
@inline(never)
public func blackHole<T>(_ x: T) {
}

// Return the passed argument without letting the optimizer know that.
@inline(never)
public func identity<T>(_ x: T) -> T {
  return x
}

// Return the passed argument without letting the optimizer know that.
// It's important that this function is in another module than the tests
// which are using it.
@inline(never)
public func getInt(_ x: Int) -> Int { return x }

// The same for String.
@inline(never)
public func getString(_ s: String) -> String { return s }

// The same for Substring.
@inline(never)
public func getSubstring(_ s: Substring) -> Substring { return s }
