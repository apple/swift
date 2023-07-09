//===--- DriverUtils.swift ------------------------------------------------===//
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

#if canImport(Glibc)
import Glibc
#elseif canImport(Musl)
import Musl
#elseif os(Windows)
import MSVCRT
#else
import Darwin
import LibProc
#endif

import TestsUtils

struct MeasurementMetadata {
  // Note: maxRSS and pages subtract the RSS measured
  // after the benchmark driver setup has finished.
  let maxRSS: Int /// Maximum Resident Set Size (B)
  let pages: Int /// Maximum Resident Set Size (pages)
  let ics: Int /// Involuntary Context Switches
  let vcs: Int /// Voluntary Context Switches
  let yields: Int /// Yield Count
}

struct BenchResults {
  let samples: [Double]
  let meta: MeasurementMetadata?
  let iters: Int

  init(_ samples: [Double], _ metadata: MeasurementMetadata?, _ iters: Int) {
    self.samples = samples
    self.meta = metadata
    self.iters = iters
  }
}

public var registeredBenchmarks: [BenchmarkInfo] = []

public func register(_ benchmark: BenchmarkInfo) {
  registeredBenchmarks.append(benchmark)
}

public func register<S: Sequence>(_ benchmarks: S)
where S.Element == BenchmarkInfo {
  registeredBenchmarks.append(contentsOf: benchmarks)
}

enum TestAction {
  case run
  case listTests
}

struct TestConfig {
  /// Duration of the test measurement in seconds.
  ///
  /// Used to compute the number of iterations, if no fixed amount is specified.
  /// This is useful when one wishes for a test to run for a
  /// longer amount of time to perform performance analysis on the test in
  /// instruments.
  let sampleTime: Double

  /// Number of iterations averaged in the sample.
  /// When not specified, we'll compute the number of iterations to be averaged
  /// in the sample from the actual runtime and the desired `sampleTime`.
  let numIters: Int?

  /// The number of samples we should take of each test.
  let numSamples: Int?

  /// The minimum number of samples we should take of each test.
  let minSamples: Int?

  /// Is verbose output enabled?
  let verbose: Bool

  // Should we log the test's memory usage?
  let logMemory: Bool

  // Should we log the measurement metadata?
  let logMeta: Bool

  // Allow running with nondeterministic hashing?
  var allowNondeterministicHashing: Bool

  // Use machine-readable output format (JSON)?
  var jsonOutput: Bool

  /// After we run the tests, should the harness sleep to allow for utilities
  /// like leaks that require a PID to run on the test harness.
  let afterRunSleep: UInt32?

  /// The list of tests to run.
  let tests: [(index: Int, info: BenchmarkInfo)]

  /// Number of characters in the longest test name (for formatting)
  let testNameLength: Int

  let action: TestAction

  init(_ registeredBenchmarks: [BenchmarkInfo]) {

    struct PartialTestConfig {
      var tags, skipTags: Set<BenchmarkCategory>?
      var numSamples: UInt?
      var minSamples: UInt?
      var numIters: UInt?
      var afterRunSleep: UInt32?
      var sampleTime: Double?
      var verbose: Bool?
      var logMemory: Bool?
      var logMeta: Bool?
      var allowNondeterministicHashing: Bool?
      var jsonOutput: Bool?
      var action: TestAction?
      var tests: [String]?
    }

    // Custom value type parsers
    func tags(tags: String) throws -> Set<BenchmarkCategory> {
      // We support specifying multiple tags by splitting on comma, i.e.:
      //  --tags=Array,Dictionary
      //  --skip-tags=Array,Set,unstable,skip
      return Set(
        try tags.split(separator: ",").map(String.init).map {
          try checked({ BenchmarkCategory(rawValue: $0) }, $0) })
    }
    func finiteDouble(value: String) -> Double? {
      return Double(value).flatMap { $0.isFinite ? $0 : nil }
    }

    // Configure the command line argument parser
    let p = ArgumentParser(into: PartialTestConfig())
    p.addArgument("--num-samples", \.numSamples,
                  help: "number of samples to take per benchmark;\n" +
                        "default: 1 or auto-scaled to measure for\n" +
                        "`sample-time` if num-iters is also specified\n",
                  parser: { UInt($0) })
    p.addArgument("--min-samples", \.minSamples,
                  help: "minimum number of samples to take per benchmark\n",
                  parser: { UInt($0) })
    p.addArgument("--num-iters", \.numIters,
                  help: "number of iterations averaged in the sample;\n" +
                        "default: auto-scaled to measure for `sample-time`",
                  parser: { UInt($0) })
    p.addArgument("--sample-time", \.sampleTime,
                  help: "duration of test measurement in seconds\ndefault: 1",
                  parser: finiteDouble)
    p.addArgument("--verbose", \.verbose, defaultValue: true,
                  help: "increase output verbosity")
    p.addArgument("--memory", \.logMemory, defaultValue: true,
                  help: "log the change in maximum resident set size (MAX_RSS)")
    p.addArgument("--meta", \.logMeta, defaultValue: true,
                  help: "log the metadata (memory usage, context switches)")
    p.addArgument("--tags", \PartialTestConfig.tags,
                  help: "run tests matching all the specified categories",
                  parser: tags)
    p.addArgument("--skip-tags", \PartialTestConfig.skipTags, defaultValue: [],
                  help: "don't run tests matching any of the specified\n" +
                        "categories; default: unstable,skip",
                  parser: tags)
    p.addArgument("--sleep", \.afterRunSleep,
                  help: "number of seconds to sleep after benchmarking",
                  parser: { UInt32($0) })
    p.addArgument("--list", \.action, defaultValue: .listTests,
                  help: "don't run the tests, just log the list of test \n" +
                        "numbers, names and tags (respects specified filters)")
    p.addArgument("--allow-nondeterministic-hashing",
                  \.allowNondeterministicHashing, defaultValue: true,
                  help: "Don't trap when running without the \n" +
                        "SWIFT_DETERMINISTIC_HASHING=1 environment variable")
    p.addArgument("--json",
                  \.jsonOutput, defaultValue: true,
                  help: "Use JSON output (suitable for consumption by scripts)")
    p.addArgument(nil, \.tests) // positional arguments

    let c = p.parse()

    // Configure from the command line arguments, filling in the defaults.
    sampleTime = c.sampleTime ?? 1.0
    numIters = c.numIters.map { Int($0) }
    numSamples = c.numSamples.map { Int($0) }
    minSamples = c.minSamples.map { Int($0) }
    verbose = c.verbose ?? false
    logMemory = c.logMemory ?? false
    logMeta = c.logMeta ?? false
    afterRunSleep = c.afterRunSleep
    action = c.action ?? .run
    allowNondeterministicHashing = c.allowNondeterministicHashing ?? false
    jsonOutput = c.jsonOutput ?? false
    tests = TestConfig.filterTests(registeredBenchmarks,
                                    tests: c.tests ?? [],
                                    tags: c.tags ?? [],
                                    skipTags: c.skipTags ?? [.unstable, .skip])

    if tests.count > 0 {
      testNameLength = tests.map{$0.info.name.count}.sorted().reversed().first!
    } else {
      testNameLength = 0
    }

    if logMemory && tests.count > 1 && !jsonOutput {
      print(
      """
      warning: The memory usage of a test, reported as the change in MAX_RSS,
               is based on measuring the peak memory used by the whole process.
               These results are meaningful only when running a single test,
               not in the batch mode!
      """)
    }

    if verbose {
      let testList = tests.map({ $0.1.name }).joined(separator: ", ")
      print("""
        --- CONFIG ---
        NumSamples: \(numSamples ?? 0)
        MinSamples: \(minSamples ?? 0)
        Verbose: \(verbose)
        LogMemory: \(logMemory)
        LogMeta: \(logMeta)
        SampleTime: \(sampleTime)
        NumIters: \(numIters ?? 0)
        Tests Filter: \(c.tests ?? [])
        Tests to run: \(testList)

        --- DATA ---
        """)
    }
  }

  /// Returns the list of tests to run.
  ///
  /// - Parameters:
  ///   - registeredBenchmarks: List of all performance tests to be filtered.
  ///   - specifiedTests: List of explicitly specified tests to run. These can
  ///     be specified either by a test name or a test number.
  ///   - tags: Run tests tagged with all of these categories.
  ///   - skipTags: Don't run tests tagged with any of these categories.
  /// - Returns: An array of test number and benchmark info tuples satisfying
  ///     specified filtering conditions.
  static func filterTests(
    _ registeredBenchmarks: [BenchmarkInfo],
    tests: [String],
    tags: Set<BenchmarkCategory>,
    skipTags: Set<BenchmarkCategory>
  ) -> [(index: Int, info: BenchmarkInfo)] {
    var t = tests
    /// TODO: Make the following less weird by using a simple `filter` operation
    let filtersIndex = t.partition { $0.hasPrefix("+") || $0.hasPrefix("-") }
    let excludesIndex = t[filtersIndex...].partition { $0.hasPrefix("-") }
    let specifiedTests = Set(t[..<filtersIndex])
    let includes = t[filtersIndex..<excludesIndex].map { $0.dropFirst() }
    let excludes = t[excludesIndex...].map { $0.dropFirst() }
    let allTests = registeredBenchmarks.sorted()
    let indices = Dictionary(uniqueKeysWithValues:
      zip(allTests.map { $0.name },
          (1...).lazy))

    func byTags(b: BenchmarkInfo) -> Bool {
      return b.tags.isSuperset(of: tags) &&
        b.tags.isDisjoint(with: skipTags)
    }
    func byNamesOrIndices(b: BenchmarkInfo) -> Bool {
      return specifiedTests.contains(b.name) ||
        // !! "`allTests` have been assigned an index"
        specifiedTests.contains(indices[b.name]!.description) ||
        (includes.contains { b.name.contains($0) } &&
          excludes.allSatisfy { !b.name.contains($0) } )
    }
    return allTests
      .filter(tests.isEmpty ? byTags : byNamesOrIndices)
      .map { (index: indices[$0.name]!, info: $0) }
  }
}

extension String {
  func contains(_ str: Substring) -> Bool {
    guard let c = str.first else { return false }
    var s = self[...]
    repeat {
      s = s[(s.firstIndex(of: c) ?? s.endIndex)...]
      if s.starts(with: str) { return true }
      s = s.dropFirst()
    } while s.startIndex != s.endIndex
    return false
  }
}

#if SWIFT_RUNTIME_ENABLE_LEAK_CHECKER

@_silgen_name("_swift_leaks_startTrackingObjects")
func startTrackingObjects(_: UnsafePointer<CChar>) -> ()
@_silgen_name("_swift_leaks_stopTrackingObjects")
func stopTrackingObjects(_: UnsafePointer<CChar>) -> Int

#endif

final class Timer {
#if os(Linux)
  typealias TimeT = timespec

  func getTime() -> TimeT {
    var ts = timespec(tv_sec: 0, tv_nsec: 0)
    clock_gettime(CLOCK_REALTIME, &ts)
    return ts
  }

  func diffTimeInNanoSeconds(from start: TimeT, to end: TimeT) -> UInt64 {
    let oneSecond = 1_000_000_000 // ns
    var elapsed = timespec(tv_sec: 0, tv_nsec: 0)
    if end.tv_nsec - start.tv_nsec < 0 {
      elapsed.tv_sec = end.tv_sec - start.tv_sec - 1
      elapsed.tv_nsec = end.tv_nsec - start.tv_nsec + oneSecond
    } else {
      elapsed.tv_sec = end.tv_sec - start.tv_sec
      elapsed.tv_nsec = end.tv_nsec - start.tv_nsec
    }
    return UInt64(elapsed.tv_sec) * UInt64(oneSecond) + UInt64(elapsed.tv_nsec)
  }
#else
  typealias TimeT = UInt64
  var info = mach_timebase_info_data_t(numer: 0, denom: 0)

  init() {
    mach_timebase_info(&info)
  }

  func getTime() -> TimeT {
    return mach_absolute_time()
  }

  func diffTimeInNanoSeconds(from start: TimeT, to end: TimeT) -> UInt64 {
    let elapsed = end - start
    return elapsed * UInt64(info.numer) / UInt64(info.denom)
  }
#endif
}

extension UInt64 {
  var microseconds: Int { return Int(self / 1000) }
}

/// Performance test runner that measures benchmarks and reports the results.
final class TestRunner {
  let c: TestConfig
  let timer = Timer()
  var start, end, lastYield: Timer.TimeT
  let baseline = TestRunner.getResourceUtilization()
  let schedulerQuantum = UInt64(10_000_000) // nanoseconds (== 10ms, macos)
  var yieldCount = 0

  init(_ config: TestConfig) {
    self.c = config
    let now = timer.getTime()
    (start, end, lastYield) = (now, now, now)
  }

  /// Offer to yield CPU to other processes and return current time on resume.
  func yield() -> Timer.TimeT {
    sched_yield()
    yieldCount += 1
    return timer.getTime()
  }

#if os(Linux)
  private static func getExecutedInstructions() -> UInt64 {
    // FIXME: there is a Linux PMC API you can use to get this, but it's
    // not quite so straightforward.
    return 0
  }
#else
  private static func getExecutedInstructions() -> UInt64 {
    if #available(OSX 10.9, iOS 7.0, *) {
      var u = rusage_info_v4()
      withUnsafeMutablePointer(to: &u) { p in
        p.withMemoryRebound(to: Optional<rusage_info_t>.self, capacity: 1) { up in
          let _ = proc_pid_rusage(getpid(), RUSAGE_INFO_V4, up)
        }
      }
      return u.ri_instructions
    } else {
      return 0
    }
  }
#endif

  private static func getResourceUtilization() -> rusage {
#if canImport(Darwin)
   let rusageSelf = RUSAGE_SELF
#else
   let rusageSelf = RUSAGE_SELF.rawValue
#endif
    var u = rusage(); getrusage(rusageSelf, &u); return u
  }

  static let pageSize: Int = {
    #if canImport(Darwin)
        let pageSize = _SC_PAGESIZE
    #else
        let pageSize = Int32(_SC_PAGESIZE)
    #endif
        return sysconf(pageSize)
  }()

  /// Returns metadata about the measurement, such as memory usage and number
  /// of context switches.
  ///
  /// This method of estimating memory usage is valid only for executing single
  /// benchmark. That's why we don't worry about resetting the `baseline` in
  /// `resetMeasurements`.
  ///
  /// FIXME: This current implementation doesn't work on Linux. It is disabled
  /// permanently to avoid linker errors. Feel free to fix.
  func collectMetadata() -> MeasurementMetadata? {
#if os(Linux)
    return nil
#else
    let current = TestRunner.getResourceUtilization()
    func delta(_ stat: KeyPath<rusage, Int>) -> Int {
      return current[keyPath: stat] - baseline[keyPath: stat]
    }
    let maxRSS = delta(\rusage.ru_maxrss)
    let pages = maxRSS / TestRunner.pageSize
    func deltaEquation(_ stat: KeyPath<rusage, Int>) -> String {
      let b = baseline[keyPath: stat], c = current[keyPath: stat]
      return "\(c) - \(b) = \(c - b)"
    }
    logVerbose(
        """
            MAX_RSS \(deltaEquation(\rusage.ru_maxrss)) (\(pages) pages)
            ICS \(deltaEquation(\rusage.ru_nivcsw))
            VCS \(deltaEquation(\rusage.ru_nvcsw))
            yieldCount \(yieldCount)
        """)
    return MeasurementMetadata(
      maxRSS: maxRSS,
      pages: pages,
      ics: delta(\rusage.ru_nivcsw),
      vcs: delta(\rusage.ru_nvcsw),
      yields: yieldCount
    )
#endif
  }

  private func startMeasurement() {
    let spent = timer.diffTimeInNanoSeconds(from: lastYield, to: end)
    let nextSampleEstimate = UInt64(Double(lastSampleTime) * 1.5)

    if (spent + nextSampleEstimate < schedulerQuantum) {
        start = timer.getTime()
    } else {
        logVerbose("    Yielding after ~\(spent.microseconds) μs")
        let now = yield()
        (start, lastYield) = (now, now)
    }
  }

  private func stopMeasurement() {
    end = timer.getTime()
  }

  private func resetMeasurements() {
    let now = yield()
    (start, end, lastYield) = (now, now, now)
    yieldCount = 0
  }

  /// Time in nanoseconds spent running the last function
  var lastSampleTime: UInt64 {
    return timer.diffTimeInNanoSeconds(from: start, to: end)
  }

  /// Measure the `fn` and return the average sample time per iteration (μs).
  func measure(_ name: String, fn: (Int) -> Void, numIters: Int) -> Double {
#if SWIFT_RUNTIME_ENABLE_LEAK_CHECKER
    name.withCString { p in startTrackingObjects(p) }
#endif

    startMeasurement()
    fn(numIters)
    stopMeasurement()

#if SWIFT_RUNTIME_ENABLE_LEAK_CHECKER
    name.withCString { p in stopTrackingObjects(p) }
#endif

    return Double(lastSampleTime.microseconds) / Double(numIters)
  }

  func logVerbose(_ msg: @autoclosure () -> String) {
    if c.verbose { print(msg()) }
  }

  /// Run the benchmark and return the measured results.
  func run(_ test: BenchmarkInfo) -> BenchResults? {
    // Before we do anything, check that we actually have a function to
    // run. If we don't it is because the benchmark is not supported on
    // the platform and we should skip it.
    guard let testFn = test.runFunction else {
      logVerbose("Skipping unsupported benchmark \(test.name)!")
      return nil
    }
    logVerbose("Running \(test.name)")

    var samples: [Double] = []

    func addSample(_ time: Double) {
      logVerbose("    Sample \(samples.count),\(time)")
      samples.append(time)
    }

    resetMeasurements()
    if let setUp = test.setUpFunction {
      setUp()
      stopMeasurement()
      logVerbose("    SetUp \(lastSampleTime.microseconds)")
      resetMeasurements()
    }

    // Determine number of iterations for testFn to run for desired time.
    func iterationsPerSampleTime() -> (numIters: Int, oneIter: Double) {
      let oneIter = measure(test.name, fn: testFn, numIters: 1)
      if oneIter > 0 {
        let timePerSample = c.sampleTime * 1_000_000.0 // microseconds (μs)
        return (max(Int(timePerSample / oneIter), 1), oneIter)
      } else {
        return (1, oneIter)
      }
    }

    // Determine the scale of measurements. Re-use the calibration result if
    // it is just one measurement.
    func calibrateMeasurements() -> Int {
      let (numIters, oneIter) = iterationsPerSampleTime()
      if numIters == 1 { addSample(oneIter) }
      else { resetMeasurements() } // for accurate yielding reports
      return numIters
    }

    let numIters = min( // Cap to prevent overflow on 32-bit systems when scaled
      Int.max / 10_000, // by the inner loop multiplier inside the `testFn`.
      c.numIters ?? calibrateMeasurements())

    let numSamples = c.numSamples ??
      // Compute the number of samples to measure for `sample-time`,
      // clamped in (`min-samples`, 200) range, if the `num-iters` are fixed.
      max(c.minSamples ?? 1, min(200, c.numIters == nil ? 1 :
        calibrateMeasurements()))

    samples.reserveCapacity(numSamples)
    logVerbose("    Collecting \(numSamples) samples.")
    logVerbose("    Measuring with scale \(numIters).")
    for _ in samples.count..<numSamples {
      addSample(measure(test.name, fn: testFn, numIters: numIters))
    }

    test.tearDownFunction?()
    if let lf = test.legacyFactor {
      logVerbose("    Applying legacy factor: \(lf)")
      samples = samples.map { $0 * Double(lf) }
    }

    return BenchResults(samples, collectMetadata(), numIters)
  }

  func printJSON(index: Int, info: BenchmarkInfo, results: BenchResults?) {
    // Write the results for a single test as a one-line JSON object
    // This allows a script to easily consume the results by JSON-decoding
    // each line separately.

    // To avoid relying on Foundation, construct the JSON naively.  This is
    // actually pretty robust, since almost everything is a number; the only
    // brittle assumption is that test.name must not have \ or " in it.
    var out = [
      "\"number\":\(index)",
      "\"name\":\"\(info.name)\""
    ]

    if let results = results {
      let samples = results.samples.sorted().map({$0.description}).joined(separator: ",")
      out.append("\"samples\":[\(samples)]")
      out.append("\"iters\":\(results.iters)")
      if let meta = results.meta {
	if c.logMemory {
	  out += [
	    "\"max_rss\":\(meta.maxRSS)",
	    "\"pages\":\(meta.pages)",
	  ]
	}
	if c.logMeta {
          out += [
	    "\"ics\":\(meta.ics)",
	    "\"yields\":\(meta.yields)",
	  ]
	}
      }
    }
    print("{ " + out.joined(separator: ", ") + " }")
    fflush(stdout)
  }


  enum Justification {
  case left, right
  }
  func printSpaces(_ width: Int) {
    for _ in 0..<width {
      print(" ", terminator: "")
    }
  }
  func printToWidth(_ s: String, width: Int, justify: Justification = .left) {
    var pad = width - 1 - s.count
    if pad <= 0 {
      pad = 1
    }
    if justify == .right {
      printSpaces(pad)
    }
    print(s, terminator: "")
    if justify == .left {
      printSpaces(pad)
    }
  }
  func printDoubleToWidth(_ d: Double, fractionDigits: Int = 3, width: Int) {
    let digits = ["0", "1", "2", "3", "4", "5", "6", "7", "8", "9"]
    // Handle up to 8 fraction digits
    let scales = [1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000]
    let scale = scales[fractionDigits]
    let i = Int(d * Double(scale) + 0.5)
    let intPart = i / scale
    let fraction = i % scale
    var s = intPart.description + "."
    var f = fraction
    for _ in 0..<fractionDigits {
      f *= 10
      s += digits[(f / scale) % 10]
    }
    printToWidth(s, width: width, justify: .right)
  }

  func printText(index: Int, info: BenchmarkInfo, results: BenchResults?) {
    printToWidth(index.description, width: 4, justify: .right)
    printSpaces(1)
    printToWidth(info.name, width: c.testNameLength)

    if let results = results {
      printToWidth(String(describing:results.samples.count), width: 10, justify: .right)
      if results.samples.count > 0 {
	let sorted = results.samples.sorted()
	let min = sorted.first!
	let max = sorted.last!
	let median = sorted[sorted.count / 2]
	printDoubleToWidth(min, width: 10)
	printDoubleToWidth(median, width: 10)
	printDoubleToWidth(max, width: 10)
      }
    }
    print()
    fflush(stdout)
  }

  func printTextHeading() {
    printToWidth("#", width: 4, justify: .right)
    printSpaces(1)
    printToWidth("TEST", width: c.testNameLength, justify: .left)
    printToWidth("SAMPLES", width: 10, justify: .right)
    printToWidth("MIN", width: 10, justify: .right)
    printToWidth("MEDIAN", width: 10, justify: .right)
    printToWidth("MAX", width: 10, justify: .right)
    print()
  }

  /// Run each benchmark and emit the results in JSON
  func runBenchmarks() {
    var testCount = 0
    if !c.jsonOutput {
      printTextHeading()
    }
    for (index, info) in c.tests {
      if c.jsonOutput {
	printJSON(index: index, info: info, results: run(info))
      } else {
	printText(index: index, info: info, results: run(info))
      }
      testCount += 1
    }

    if !c.jsonOutput {
      print("\nTotal performance tests executed: \(testCount)")
    }
  }
}

extension Hasher {
  static var isDeterministic: Bool {
    // This is a quick test for deterministic hashing.
    // When hashing uses a random seed, each `Set` value
    // contains its members in some unique, random order.
    let set1 = Set(0 ..< 100)
    let set2 = Set(0 ..< 100)
    return set1.elementsEqual(set2)
  }
}

public func main() {
  let config = TestConfig(registeredBenchmarks)
  switch (config.action) {
  case .listTests:
    if config.jsonOutput {
      for (index, t) in config.tests {
	let tags = t.tags.sorted().map({"\"\($0.description)\""}).joined(separator: ",")
        print("{\"number\":\(index), \"name\":\"\(t.name)\", \"tags\":[\(tags)]}")
      }
    } else {
      print("# Test [Tags]")
      for (index, t) in config.tests {
        let testDescription = [index.description, t.name, t.tags.sorted().description]
          .joined(separator: " ")
        print(testDescription)
      }
    }
  case .run:
    if !config.allowNondeterministicHashing && !Hasher.isDeterministic {
      fatalError("""
        Benchmark runs require deterministic hashing to be enabled.

        This prevents spurious regressions in hashed collection performance.
        You can do this by setting the SWIFT_DETERMINISTIC_HASHING environment
        variable to 1.

        If you know what you're doing, you can disable this check by passing
        the option '--allow-nondeterministic-hashing to the benchmarking executable.
        """)
    }
    TestRunner(config).runBenchmarks()
    if let x = config.afterRunSleep {
      sleep(x)
    }
  }
}
