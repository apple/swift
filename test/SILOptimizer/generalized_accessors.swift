// RUN: %target-swift-frontend -emit-sil -primary-file %s -o /dev/null -verify
//
// Tests for yield-once diagnostics emitted for generalized accessors.

struct TestNoYield {
  var computed: Int {
    _read {
    } // expected-error {{accessor must yield before returning}}

    _modify {
    } // expected-error {{accessor must yield before returning}}
  }
}

struct TestReturnPathWithoutYield {
  var stored: Int
  var flag: Bool

  var computed: Int {
    mutating _read {
      if flag {   // expected-note {{missing yield when the condition is false}}
        yield stored
      }
      flag = true
    } // expected-error {{accessor must yield on all paths before returning}}

    _modify {
      // Diagnostics should attach a note to the earliest conflicting branch.
      if flag {   // expected-note {{missing yield when the condition is false}}
        yield &stored
      }

      if !flag {
        flag = true
      }
    } // expected-error {{accessor must yield on all paths before returning}}
  }
}

struct TestIfElseWithoutYield {
  var stored: Int
  var flag: Bool

  var computed: Int {
    mutating _read {
      if flag {       // expected-note {{missing yield when the condition is false}}
        yield stored
      } else {
        flag = true
      }
    } // expected-error {{accessor must yield on all paths before returning}}

    _modify {
      if flag {       // expected-note {{missing yield when the condition is true}}
        flag = true
      } else {
        yield &stored
      }
    } // expected-error {{accessor must yield on all paths before returning}}
  }
}

struct TestNestedIfs {
  var stored: Int
  var flag: Bool

  // Diagnostics should attach a note to the innermost conflicting branch, which
  // is the point of first conflict.
  var computed: Int {
    _read {
      if flag {
        if stored > 0 { // expected-note {{missing yield when the condition is false}}
          yield stored
        }
      }
    } // expected-error {{accessor must yield on all paths before returning}}

    _modify {
      if flag {
        if stored > 0 { // expected-note {{missing yield when the condition is true}}
        } else {
          yield &stored
        }
      }
      flag = true
    } // expected-error {{accessor must yield on all paths before returning}}
  }
}

struct TestMultipleYields {
  var stored: Int
  var flag: Bool
  var computed: Int {
    _read {
      if flag {
        yield stored // expected-note {{previous yield was here}}
      }
      yield stored // expected-error {{accessor must not yield more than once}}
    }

    _modify {
      yield &stored // expected-note {{previous yield was here}}
      if flag {
        yield &stored // expected-error {{accessor must not yield more than once}}
      }
    }
  }
}

struct TestMultipleYields2 {
  var stored: Int
  var flag: Bool
  var computed: Int {
    _read {
      if flag {
        yield stored
        return
      }
      yield stored
    }

    _modify {
      if flag {
        yield &stored
      } else {
        yield &stored
      }
    }
  }
}

 struct TestConvolutedPaths {
  var flag: Bool
  var stored : Int

  var computed: Int {
    _read {
      if flag {
        yield stored // expected-note {{previous yield was here}}
      }
      if !flag {
        yield stored // expected-error {{accessor must not yield more than once}}
      }
    }

    _modify {
      if flag {
        yield &stored // expected-note {{previous yield was here}}
      }
      if !flag {
        yield &stored // expected-error {{accessor must not yield more than once}}
      }
    }
  }
}

struct TestYieldInLoops {
  var stored: Int
  var count: Int
  var computed: Int {
    _read {
      for _ in 0..<count {
        yield stored // expected-error {{accessor must not yield more than once}}
                     // expected-note@-1 {{previous yield was here}}
      }
    }

    _modify {
      yield &stored // expected-note {{previous yield was here}}
      for _ in 0..<count {
        yield &stored // expected-error {{accessor must not yield more than once}}
      }
    }
  }
}

enum CompassPoint {
  case north
  case south
  case east
  case west
}

struct TestYieldInSwitch {
  var stored: Int
  var cp: CompassPoint

  var computed: Int {
    get { return stored }
    _modify {
      switch cp {
      case .north:
        yield &stored
      case .south:
        stored = 10
      case .east:
        yield &stored
      case .west:
        stored = 12 // expected-note {{missing yield in the 'west' case}}
      }
      cp = .north
    } // expected-error {{accessor must yield on all paths before returning}}
  }
}

struct TestYieldInSwitchWithFallThrough {
  var stored: Int
  var cp: CompassPoint

  var computed: Int {
    _read {
      switch cp {
      case .north:
        fallthrough
      case .south:
        fallthrough
      case .east:
        fallthrough
      case .west:
        yield stored
      }
    }

    _modify {
      switch cp {
      case .north:
        fallthrough
      case .south:
        yield &stored
      case .east:
        fallthrough  // expected-note {{missing yield in the 'east' case}}
      case .west:
        stored = 12
      }
    } // expected-error {{accessor must yield on all paths before returning}}
  }
}

struct TestYieldInSwitchWithoutCaseNames {
  var stored: Int
  var cp: CompassPoint

  var computed: Int {
    _read {
      switch cp {
      case .north:
        yield stored
      case _:
        break // expected-note {{missing yield in this case}}
      }
    } // expected-error {{accessor must yield on all paths before returning}}

    _modify {
      switch cp {
      case .north:
        yield &stored
      case _ where stored < 0: // expected-note {{missing yield in this case}}
        stored = 12
      case .south:
        fallthrough
      case .east:
        fallthrough
      case .west:
        yield &stored
      }
    } // expected-error {{accessor must yield on all paths before returning}}
  }

  var computed2: Int {
    _read {
      switch cp {
      case .north:
        yield stored
      case _ where stored < 0: // expected-note {{missing yield when the condition is false}}
        yield stored
      default:
        break
      }
    } // expected-error {{accessor must yield on all paths before returning}}

    _modify {
      switch cp {
      case .north:
        yield &stored
      case _ where stored < 0: // expected-note {{missing yield in this case}}
        break
      default:
        yield &stored
      }
    } // expected-error {{accessor must yield on all paths before returning}}
  }

  var computed3: Int {
    _read {
      switch cp {
      case .north:
        yield stored
      case _ where stored < 0: // expected-note {{missing yield in this case}}
        break
      case .south:
        fallthrough
      case .east:
        yield stored
      case .west:
        break
      }
    } // expected-error {{accessor must yield on all paths before returning}}

    _modify {
      switch cp {
      case .north:
        break
      case _ where stored < 0: // expected-note {{missing yield when the condition is true}}
        break
      default:
        yield &stored
      }
    } // expected-error {{accessor must yield on all paths before returning}}
  }
}

struct TestExplicitReturn {
  var stored: Int
  var flag: Bool

  var computed: Int {
    mutating _read {
      if stored > 0 {  // expected-note {{missing yield when the condition is true}}
        return
      }
      if flag {
        yield stored
      } else {
        yield stored
      }
      flag = true
    } // expected-error {{accessor must yield on all paths before returning}}

    _modify {
      if stored > 0 {
        return
      }
      if stored == 0 { // expected-note {{missing yield when the condition is false}}
        if flag {
          yield &stored
        } else {
          yield &stored
        }
        return
      }
      flag = true
    } // expected-error {{accessor must yield on all paths before returning}}
  }

  var anotherProp: Int {
    mutating _read {
      if flag {
        stored = 2
      }
      return; // expected-error {{accessor must yield before returning}}

      if !flag { // expected-warning {{code after 'return' will never be executed}}
        stored = 3
      }
    }
  }
}

struct TestYieldsInGuards {
  var storedOpt: Int?
  var defaultStorage: Int

  var computed: Int {
    _read {
      guard let stored = storedOpt else {
        yield defaultStorage
        return
      }
      yield stored
    }
  }
}

struct TestYieldsInGuards2 {
  var flag: Bool
  var defaultStorage: Int

  var computed: Int {
    _read {
      guard flag else { // expected-note {{missing yield when the condition is false}}
        return
      }
      yield defaultStorage
    } // expected-error {{accessor must yield on all paths before returning}}

    _modify {
      guard flag else { // expected-note {{missing yield when the condition is true}}
        yield &defaultStorage
        return
      }
      defaultStorage += 1
    } // expected-error {{accessor must yield on all paths before returning}}
  }
}

struct TestYieldsInLetPatterns {
  var storedOpt: Int?
  var defaultStorage: Int

  var computed: Int {
    _read {
      if let stored = storedOpt { // expected-note {{missing yield in the nil case}}
        yield stored
      }
    } // expected-error {{accessor must yield on all paths before returning}}

    _modify {
      if var stored = storedOpt { // expected-note {{missing yield in the non-nil case}}
        stored += 1
        return
      }
      yield &defaultStorage
    } // expected-error {{accessor must yield on all paths before returning}}
  }

  enum Either<L, R> {
    case left(L)
    case right(R)
  }

  var intOrBool: Either<Int, Bool>
  var computed2: Int {
    _read {
      if case .left(let x) = intOrBool { // expected-note {{missing yield in the 'right' case}}
        yield x
      }
    } // expected-error {{accessor must yield on all paths before returning}}

    _modify {
      guard let stored = storedOpt else { // expected-note {{missing yield in the non-nil case}}
        yield &defaultStorage
        return
      }
      storedOpt = stored + 1
    } // expected-error {{accessor must yield on all paths before returning}}
  }

  var cp: CompassPoint
  var computed3: Int {
    _read {
      if case .north = cp  { // expected-note {{missing yield in this case}}
        yield 0
      }
    } // expected-error {{accessor must yield on all paths before returning}}
  }
}

enum SoftError : Error {
  case Ignorable
}
func aThrowingFunction() throws {
  throw SoftError.Ignorable
}

struct TestYieldInDoCatch {
  var stored: Int
  var computed: Int {
    _read {
      do {
        try aThrowingFunction()
        yield stored
      } catch {
        yield stored
      }
    }
  }
}

struct TestYieldInDoCatch2 {
  var stored: Int
  var computed: Int {
    _read {
      do {
        try aThrowingFunction() // expected-note {{missing yield when error is thrown}}
        yield stored
      } catch {
      }
    } // expected-error {{accessor must yield on all paths before returning}}

    _modify {
      do {
        try aThrowingFunction() // expected-note {{missing yield when error is not thrown}}
      }
      catch {
        yield &stored
      }
    } // expected-error {{accessor must yield on all paths before returning}}
  }
}

enum Binary {
  case one
  case two
}

struct TestMultipleTries {
  var stored: Int
  var computed: Int {
    _read {
      do {
        try aThrowingFunction()
        yield stored // expected-note {{previous yield was here}}

        try aThrowingFunction()
        yield stored // expected-error {{accessor must not yield more than once}}
      } catch {
      }
    }

    _modify {
      do {
        try aThrowingFunction()
        yield &stored

        try aThrowingFunction() // expected-note {{missing yield when error is thrown}}
      }
      catch {
      }
    } // expected-error {{accessor must yield on all paths before returning}}
  }

  var flag: Bool
  var bin: Binary

  var computed2: Int {
    _read {
      do {
        if flag {
          try aThrowingFunction()
        } else {
          try aThrowingFunction() // expected-note {{missing yield when error is thrown}}
        }

        yield stored
      } catch {
      }
    } // expected-error {{accessor must yield on all paths before returning}}

    _modify {
      do {
        switch bin {
        case .one:
          try aThrowingFunction()
          yield &stored

        case .two:
          try aThrowingFunction() // expected-note {{missing yield when error is thrown}}
          yield &stored
        }
      } catch {
      }
    } // expected-error {{accessor must yield on all paths before returning}}
  }

  var computed3: Int {
    _read {
      do {
        if flag {
          try aThrowingFunction()
        } else {
          try aThrowingFunction() // expected-note {{missing yield when error is not thrown}}
        }
      } catch {
        yield stored
      }
    } // expected-error {{accessor must yield on all paths before returning}}

    _modify {
      do {
        switch bin {
        case .one:
          try aThrowingFunction()
        case .two:
          try aThrowingFunction() // expected-note {{missing yield when error is not thrown}}
        }
      } catch {
        yield &stored
      }
    } // expected-error {{accessor must yield on all paths before returning}}
  }
}

struct TestMultipleCatches {
  enum NumError: Error {
    case One
    case Two
    case Three
  }

  var stored: Int
  var computed: Int {
    _read {
      // This is a very interesting test, where the error is on the try.
      // It could have been better if it is on the switch case in the
      // error case.
      do {
        try aThrowingFunction() // expected-note {{missing yield when error is thrown}}
        yield stored
      } catch NumError.One {
        yield stored
      } catch NumError.Two {
        yield stored
      } catch NumError.Three {
      } catch {
        yield stored
      }
    }  // expected-error {{accessor must yield on all paths before returning}}

    _modify {
      do {
        try aThrowingFunction() // expected-note {{missing yield when error is not thrown}}
      } catch NumError.One {
        yield &stored
      } catch NumError.Two {
        yield &stored
      } catch NumError.Three {
      } catch {
        yield &stored
      }
    }  // expected-error {{accessor must yield on all paths before returning}}
  }
}

// Test for labeled breaks.

struct TestYieldWithLabeledBreaks {
  var stored: Int
  var flag: Bool
  var bin: Binary

  var computed: Int {
    _read {
      ifstmt: if flag {
        switch bin {
        case .one:
          yield stored
        case .two:
          break ifstmt // expected-note {{missing yield in the 'two' case}}
        }
      }
    } // expected-error {{accessor must yield on all paths before returning}}

    _modify {
      loop: while flag {
        switch bin {
        case .one:
          yield &stored // expected-error {{accessor must not yield more than once}}
                        // expected-note@-1 {{previous yield was here}}
        case .two:
          break loop
        }
      }
    }
  }
}

// Test switches over non-enum values.

struct TestYieldInSwitchWhere {
  var stored: Int
  var computed: Int {
    _read {
      switch stored {
      case let x where x > 0:
        yield stored
      case let x where x < 0: // expected-note {{missing yield when the condition is true}}
        return
      default:
        yield 3
      }
    } // expected-error {{accessor must yield on all paths before returning}}
  }
}

struct TestYieldInSwitchValue {
  var stored: Bool
  var computed: Int {
    _read {
      switch stored {
      case true:
        yield 0
      case false:
        return    // expected-note {{missing yield in the 2nd case}}
      }
    } // expected-error {{accessor must yield on all paths before returning}}
  }
}

struct TestYieldInSwitchEnumAddr<T> {
  var storedOpt: T?
  var computed: T {
    _read {
      if let stored = storedOpt {
        yield stored
      }                   // expected-note {{missing yield in the nil case}}
    } // expected-error {{accessor must yield on all paths before returning}}
  }
}

// Test checked casts.

struct TestYieldInCheckedCast<T> {
  var stored: T
  var computed: Int {
    _read {
      if let x = (stored as? Int) {
        yield x
      }                   // expected-note {{missing yield in the nil case}}
    } // expected-error {{accessor must yield on all paths before returning}}
  }
}

struct TestYieldInForcedUnwrappingt<T> {
  var stored: T?
  var computed: T {
    _read {
      yield stored!
    }
  }
}
