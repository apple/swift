#if _runtime(_ObjC)
import Foundation
#endif
import StdlibUnittest

extension String: Error {}

func verifySmallString(_ small: _SmallString, _ input: String,
 file: String = #file, line: UInt = #line
) {
  let loc = SourceLoc(file, line, comment: "test data")
  expectEqual(_SmallString.capacity, small.count + small.unusedCapacity,
              stackTrace: SourceLocStack().with(loc))
  let tiny = Array(input.utf8)
  expectEqual(tiny.count, small.count, stackTrace: SourceLocStack().with(loc))
  for (lhs, rhs) in zip(tiny, small) {
    expectEqual(lhs, rhs, stackTrace: SourceLocStack().with(loc))
  }

  let smallFromUTF16 = _SmallString(Array(input.utf16))
  expectNotNil(smallFromUTF16,
               stackTrace: SourceLocStack().with(loc))
  expectEqualSequence(small, smallFromUTF16!,
                      stackTrace: SourceLocStack().with(loc))

  // Test slicing
  //
  for i in 0..<small.count {
    for j in i...small.count {
      expectEqualSequence(tiny[i..<j], small[i..<j],
                          stackTrace: SourceLocStack().with(loc))
      if j < small.count {
        expectEqualSequence(tiny[i...j], small[i...j],
                            stackTrace: SourceLocStack().with(loc))
      }
    }
  }

  // Test RAC and Mutable
  var copy = small
  for i in 0..<small.count / 2 {
    let tmp = copy[i]
    copy[i] = copy[copy.count - 1 - i]
    copy[copy.count - 1 - i] = tmp
  }
  expectEqualSequence(small.reversed(), copy,
                      stackTrace: SourceLocStack().with(loc))
}

// Testing helper inits
extension _SmallString {
  init?(_ codeUnits: Array<UInt8>) {
    guard let smol = codeUnits.withUnsafeBufferPointer({
      return _SmallString($0)
    }) else {
      return nil
    }
    self = smol
  }

  init?(_ codeUnits: Array<UInt16>) {
    let str = codeUnits.withUnsafeBufferPointer {
      return String._uncheckedFromUTF16($0)
    }
    if !str._guts.isSmall {
      return nil
    }
    self.init(str._guts._object)
  }

 
#if _runtime(_ObjC)
  init?(_cocoaString ns: NSString) {
#if _pointerBitWidth(_32)
    return nil
#else
    guard _isObjCTaggedPointer(ns) else { return nil }
    self = (ns as String)._guts.asSmall //regular tagged NSStrings are guaranteed to bridge to SmallStrings
    assert(_StringGuts(self).isSmall)
#endif
  }
#endif

  func _appending(_ other: _SmallString) -> _SmallString? {
    return _SmallString(self, appending: other)
  }

  func _repeated(_ n: Int) -> _SmallString? {
    var base = self
    let toAppend = self
    for _ in 0..<(n &- 1) {
      guard let s = _SmallString(
        base, appending: toAppend)
      else { return nil }
      base = s
    }
    return base
  }
}

func expectSmall(_ str: String,
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
 ) {
  switch str._classify()._form {
    case ._small: return
    default: expectationFailure("expected: small", trace: "",
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
  }
}

func expectCocoa(_ str: String,
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
 ) {
  switch str._classify()._form {
    case ._cocoa: return
    default: expectationFailure("expected: cocoa", trace: "",
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
  }
}

