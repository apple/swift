// RUN: %target-build-swift -sanitize=thread %s -o %t_binary
// RUN: %target-codesign %t_binary
// RUN: %env-TSAN_OPTIONS=halt_on_error=1 %target-run %t_binary
// REQUIRES: executable_test
// REQUIRES: stress_test
// REQUIRES: tsan_runtime
// REQUIRES: foundation

// Check that TSan ignores the retain count update locks in the runtime.

import Foundation

public class Dummy {
  func doNothing() {
  }
}

public class DummyNSObject : NSObject {
  func doNothing() {
  }
}

public class RaceRunner : NSObject {
  static var g: Int = 0
  static var staticSwiftMember: Dummy = Dummy()
  static var staticNSObjectMember: DummyNSObject = DummyNSObject()
  var swiftMember: Dummy
  var nsObjectMember: DummyNSObject

  override init() {
    swiftMember = Dummy()
    nsObjectMember = DummyNSObject()
  }
  func triggerRace() {
    // Add accesses that potentially lock in Swift runtime.
    swiftMember.doNothing()
    nsObjectMember.doNothing()
    RaceRunner.staticSwiftMember.doNothing()
    RaceRunner.staticNSObjectMember.doNothing()

    RaceRunner.g = RaceRunner.g + 1

    // Add accesses that potentially lock in Swift runtime.
    swiftMember.doNothing()
    nsObjectMember.doNothing()
    RaceRunner.staticSwiftMember.doNothing()
    RaceRunner.staticNSObjectMember.doNothing()
  }
}

// Execute concurrently.
import StdlibUnittest
var RaceTestSuite = TestSuite("t")

RaceTestSuite
  .test("test_tsan_ignores_arc_locks")
  .crashOutputMatches("ThreadSanitizer: data race")
  .code {
    expectCrashLater()
    runRaceTest(trials: 1) {
      let RaceRunnerInstance: RaceRunner = RaceRunner()
      RaceRunnerInstance.triggerRace()
    }
  }

runAllTests()

