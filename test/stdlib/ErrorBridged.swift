// RUN: %empty-directory(%t)
// RUN: %target-build-swift -o %t/ErrorBridged -DPTR_SIZE_%target-ptrsize -module-name main %s
// RUN: %target-codesign %t/ErrorBridged
// RUN: %target-run %t/ErrorBridged
// REQUIRES: executable_test
// REQUIRES: objc_interop

import StdlibUnittest


import Foundation
import CoreLocation
import Darwin

var ErrorBridgingTests = TestSuite("ErrorBridging")

var NoisyErrorLifeCount = 0
var NoisyErrorDeathCount = 0
var CanaryHandle = 0

protocol OtherProtocol {
  var otherProperty: String { get }
}

protocol OtherClassProtocol : AnyObject {
  var otherClassProperty: String { get }
}

class NoisyError : Error, OtherProtocol, OtherClassProtocol {
  init() { NoisyErrorLifeCount += 1 }
  deinit { NoisyErrorDeathCount += 1 }

  let _domain = "NoisyError"
  let _code = 123

  let otherProperty = "otherProperty"
  let otherClassProperty = "otherClassProperty"
}

@objc enum EnumError : Int, Error {
  case BadError = 9000
  case ReallyBadError = 9001
}

ErrorBridgingTests.test("NSError") {
  NoisyErrorLifeCount = 0
  NoisyErrorDeathCount = 0
  autoreleasepool {
    let ns = NSError(domain: "SomeDomain", code: 321, userInfo: nil)

    objc_setAssociatedObject(ns, &CanaryHandle, NoisyError(),
                             .OBJC_ASSOCIATION_RETAIN_NONATOMIC)

    let e: Error = ns
    expectEqual(e._domain, "SomeDomain")
    expectEqual(e._code, 321)

    let ns2 = e as NSError
    expectTrue(ns === ns2)
    expectEqual(ns2._domain, "SomeDomain")
    expectEqual(ns2._code, 321)
  }
  expectEqual(NoisyErrorDeathCount, NoisyErrorLifeCount)
}

ErrorBridgingTests.test("NSCopying") {
  autoreleasepool {
    let orig = EnumError.ReallyBadError as NSError
    let copy = orig.copy() as! NSError
    expectEqual(orig, copy)
  }
}

// Gated on the availability of NSKeyedArchiver.archivedData(withRootObject:).
@available(macOS 10.11, iOS 9.0, tvOS 9.0, watchOS 2.0, *)
func archiveAndUnarchiveObject<T: NSCoding>(
  _ object: T
) -> T?
where T: NSObject {
  let unarchiver = NSKeyedUnarchiver(forReadingWith:
    NSKeyedArchiver.archivedData(withRootObject: object)
  )
  unarchiver.requiresSecureCoding = true
  return unarchiver.decodeObject(of: T.self, forKey: "root")
}
ErrorBridgingTests.test("NSCoding") {
  if #available(macOS 10.11, iOS 9.0, tvOS 9.0, watchOS 2.0, *) {
    autoreleasepool {
      let orig = EnumError.ReallyBadError as NSError
      let unarchived = archiveAndUnarchiveObject(orig)!
      expectEqual(orig, unarchived)
      expectTrue(type(of: unarchived) == NSError.self)
    }
  }
}

ErrorBridgingTests.test("NSError-to-enum bridging") {
  NoisyErrorLifeCount = 0
  NoisyErrorDeathCount = 0
  let testURL = URL(string: "https://swift.org")!

  autoreleasepool {
    let underlyingError = CocoaError(.fileLocking)
      as Error as NSError
    let ns = NSError(domain: NSCocoaErrorDomain,
                     code: NSFileNoSuchFileError,
                     userInfo: [
                       NSFilePathErrorKey: "/dev/null",
                       NSStringEncodingErrorKey: /*ASCII=*/1,
                       NSUnderlyingErrorKey: underlyingError,
                       NSURLErrorKey: testURL
                     ])

    objc_setAssociatedObject(ns, &CanaryHandle, NoisyError(),
                             .OBJC_ASSOCIATION_RETAIN_NONATOMIC)
  
    let e: Error = ns

    let cocoaCode: Int?
    switch e {
    case let x as CocoaError:
      cocoaCode = x._code
      expectTrue(x.isFileError)
      expectEqual(x.code, .fileNoSuchFile)
    default:
      cocoaCode = nil
    }

    expectEqual(NSFileNoSuchFileError, cocoaCode)

    let cocoaCode2: Int? = (ns as? CocoaError)?._code
    expectEqual(NSFileNoSuchFileError, cocoaCode2)

    let isNoSuchFileError: Bool
    switch e {
    case CocoaError.fileNoSuchFile:
      isNoSuchFileError = true
    default:
      isNoSuchFileError = false
    }

    expectTrue(isNoSuchFileError)

    // Check the contents of the error.
    let cocoaError = e as! CocoaError
    expectEqual("/dev/null", cocoaError.filePath)
    expectEqual(String.Encoding.ascii, cocoaError.stringEncoding)
    expectEqual(underlyingError, cocoaError.underlying.map { $0 as NSError })
    expectEqual(testURL, cocoaError.url)

    // URLError domain
    let nsURL = NSError(domain: NSURLErrorDomain,
                        code: NSURLErrorBadURL,
                        userInfo: [NSURLErrorFailingURLErrorKey: testURL])
    let eURL: Error = nsURL
    let isBadURLError: Bool
    switch eURL {
    case URLError.badURL:
      isBadURLError = true
    default:
      isBadURLError = false
    }

    expectTrue(isBadURLError)

    let urlError = eURL as! URLError
    expectEqual(testURL, urlError.failingURL)
    expectNil(urlError.failureURLPeerTrust)

    // CoreLocation error domain
    let nsCL = NSError(domain: kCLErrorDomain,
                       code: CLError.headingFailure.rawValue,
                       userInfo: [NSURLErrorKey: testURL])
    let eCL: Error = nsCL
    let isHeadingFailure: Bool
    switch eCL {
    case CLError.headingFailure:
      isHeadingFailure = true
    default:
      isHeadingFailure = false
    }

    let isCLError: Bool
    switch eCL {
    case let error as CLError:
      isCLError = true
      expectEqual(testURL, (error as NSError).userInfo[NSURLErrorKey] as? URL)
      expectEqual(testURL, error.userInfo[NSURLErrorKey] as? URL)
    default:
      isCLError = false
    }

    expectTrue(isCLError)

    // NSPOSIXError domain
    let nsPOSIX = NSError(domain: NSPOSIXErrorDomain,
                          code: Int(EDEADLK),
                          userInfo: [:])
    let ePOSIX: Error = nsPOSIX
    let isDeadlock: Bool
    switch ePOSIX {
    case POSIXError.EDEADLK:
      isDeadlock = true
    default:
      isDeadlock = false
    }

    expectTrue(isDeadlock)

    // NSMachError domain
    let nsMach = NSError(domain: NSMachErrorDomain,
                         code: Int(KERN_MEMORY_FAILURE),
                         userInfo: [:])
    let eMach: Error = nsMach
    let isMemoryFailure: Bool
    switch eMach {
    case MachError.memoryFailure:
      isMemoryFailure = true
    default:
      isMemoryFailure = false
    }

    expectTrue(isMemoryFailure)
  }
  
  expectEqual(NoisyErrorDeathCount, NoisyErrorLifeCount)
}

func opaqueUpcastToAny<T>(_ x: T) -> Any {
  return x
}

struct StructError: Error {
  var _domain: String { return "StructError" }
  var _code: Int { return 4812 }
}

ErrorBridgingTests.test("Error-to-NSError bridging") {
  NoisyErrorLifeCount = 0
  NoisyErrorDeathCount = 0
  autoreleasepool {
    let e: Error = NoisyError()
    let ns = e as NSError
    let ns2 = e as NSError
    expectTrue(ns === ns2)
    expectEqual(ns._domain, "NoisyError")
    expectEqual(ns._code, 123)

    let e3: Error = ns
    expectEqual(e3._domain, "NoisyError")
    expectEqual(e3._code, 123)
    let ns3 = e3 as NSError
    expectTrue(ns === ns3)

    let nativeNS = NSError(domain: NSCocoaErrorDomain,
                           code: NSFileNoSuchFileError,
                           userInfo: nil)

    objc_setAssociatedObject(ns, &CanaryHandle, NoisyError(),
                             .OBJC_ASSOCIATION_RETAIN_NONATOMIC)

    let nativeE: Error = nativeNS
    let nativeNS2 = nativeE as NSError
    expectTrue(nativeNS === nativeNS2)
    expectEqual(nativeNS2._domain, NSCocoaErrorDomain)
    expectEqual(nativeNS2._code, NSFileNoSuchFileError)

    let any: Any = NoisyError()
    let ns4 = any as! NSError
    expectEqual(ns4._domain, "NoisyError")
    expectEqual(ns4._code, 123)

    let ao: AnyObject = NoisyError()
    let ns5 = ao as! NSError
    expectEqual(ns5._domain, "NoisyError")
    expectEqual(ns5._code, 123)

    let any2: Any = StructError()
    let ns6 = any2 as! NSError
    expectEqual(ns6._domain, "StructError")
    expectEqual(ns6._code, 4812)
  }
  expectEqual(NoisyErrorDeathCount, NoisyErrorLifeCount)
}

ErrorBridgingTests.test("NSError-to-error bridging in bridged container") {
  autoreleasepool {
    let error = NSError(domain: "domain", code: 42, userInfo: nil)
    let nsdictionary = ["error": error] as NSDictionary
    let dictionary = nsdictionary as? Dictionary<String, Error>
    expectNotNil(dictionary)
    expectEqual(error, dictionary?["error"] as NSError?)
  }
}

ErrorBridgingTests.test("enum-to-NSError round trip") {
  autoreleasepool {
    // Emulate throwing an error from Objective-C.
    func throwNSError(_ error: EnumError) throws {
      throw NSError(domain: "main.EnumError", code: error.rawValue,
                    userInfo: [:])
    }

    var caughtError: Bool

    caughtError = false
    do {
      try throwNSError(.BadError)
      expectUnreachable()
    } catch let error as EnumError {
      expectEqual(.BadError, error)
      caughtError = true
    } catch _ {
      expectUnreachable()
    }
    expectTrue(caughtError)

    caughtError = false
    do {
      try throwNSError(.ReallyBadError)
      expectUnreachable()
    } catch EnumError.BadError {
      expectUnreachable()
    } catch EnumError.ReallyBadError {
      caughtError = true
    } catch _ {
      expectUnreachable()
    }
    expectTrue(caughtError)
  }
}

class SomeNSErrorSubclass: NSError {}


ErrorBridgingTests.test("Thrown NSError identity is preserved") {
  do {
    let e = NSError(domain: "ClericalError", code: 219,
                    userInfo: ["yeah": "yeah"])
    do {
      throw e
    } catch let e2 as NSError {
      expectTrue(e === e2)
      expectTrue(e2.userInfo["yeah"] as? String == "yeah")
    } catch {
      expectUnreachable()
    }
  }

  do {
    let f = SomeNSErrorSubclass(domain: "ClericalError", code: 219,
                                userInfo: ["yeah": "yeah"])
    do {
      throw f
    } catch let f2 as NSError {
      expectTrue(f === f2)
      expectTrue(f2.userInfo["yeah"] as? String == "yeah")
    } catch {
      expectUnreachable()
    }
  }
}

// Check errors customized via protocol.
struct MyCustomizedError : Error {
  let code: Int
}

extension MyCustomizedError : LocalizedError {
  var errorDescription: String? {
    return NSLocalizedString("something went horribly wrong", comment: "")
  }

  var failureReason: String? {
    return NSLocalizedString("because someone wrote 'throw'", comment: "")
  }

  var recoverySuggestion: String? {
    return NSLocalizedString("delete the 'throw'", comment: "")
  }

  var helpAnchor: String? {
    return NSLocalizedString("there is no help when writing tests", comment: "")
  }
}

extension MyCustomizedError : CustomNSError {
  static var errorDomain: String {
    return "custom"
  }

  /// The error code within the given domain.
  var errorCode: Int {
    return code
  }

  /// The user-info dictionary.
  var errorUserInfo: [String : Any] {
    return [NSURLErrorKey : URL(string: "https://swift.org")!]
  }
}

extension MyCustomizedError : RecoverableError {
  var recoveryOptions: [String] {
    return ["Delete 'throw'", "Disable the test" ]
  }

  func attemptRecovery(optionIndex recoveryOptionIndex: Int) -> Bool {
    return recoveryOptionIndex == 0
  }
}

/// An error type that provides localization and recovery, but doesn't
/// customize NSError directly.
enum MySwiftCustomizedError : Error {
  case failed
  static var errorDescriptionCount = 0
}

extension MySwiftCustomizedError : LocalizedError {
  var errorDescription: String? {
    MySwiftCustomizedError.errorDescriptionCount =
      MySwiftCustomizedError.errorDescriptionCount + 1
    return NSLocalizedString("something went horribly wrong", comment: "")
  }

  var failureReason: String? {
    return NSLocalizedString("because someone wrote 'throw'", comment: "")
  }

  var recoverySuggestion: String? {
    return NSLocalizedString("delete the 'throw'", comment: "")
  }

  var helpAnchor: String? {
    return NSLocalizedString("there is no help when writing tests", comment: "")
  }
}

extension MySwiftCustomizedError : RecoverableError {
  var recoveryOptions: [String] {
    return ["Delete 'throw'", "Disable the test" ]
  }

  func attemptRecovery(optionIndex recoveryOptionIndex: Int) -> Bool {
    return recoveryOptionIndex == 0
  }
}

/// Fake definition of the informal protocol
/// "NSErrorRecoveryAttempting" that we use to poke at the object
/// produced for a RecoverableError.
@objc protocol FakeNSErrorRecoveryAttempting {
  @objc(attemptRecoveryFromError:optionIndex:delegate:didRecoverSelector:contextInfo:)
  func attemptRecovery(fromError nsError: Error,
                       optionIndex recoveryOptionIndex: Int,
                       delegate: AnyObject?,
                       didRecoverSelector: Selector,
                       contextInfo: UnsafeMutableRawPointer?)

  @objc(attemptRecoveryFromError:optionIndex:)
  func attemptRecovery(fromError nsError: Error,
                       optionIndex recoveryOptionIndex: Int) -> Bool
}

class RecoveryDelegate {
  let expectedSuccess: Bool
  let expectedContextInfo: UnsafeMutableRawPointer?
  var called = false

  init(expectedSuccess: Bool,
       expectedContextInfo: UnsafeMutableRawPointer?) {
    self.expectedSuccess = expectedSuccess
    self.expectedContextInfo = expectedContextInfo
  }

  @objc func recover(success: Bool, contextInfo: UnsafeMutableRawPointer?) {
    expectEqual(expectedSuccess, success)
    expectEqual(expectedContextInfo, contextInfo)
    called = true
  }
}

/// Helper for testing a customized error.
func testCustomizedError(error: Error, nsError: NSError) {
  // LocalizedError
  if #available(OSX 10.11, iOS 9.0, tvOS 9.0, watchOS 2.0, *) {
    expectNil(nsError.userInfo[NSLocalizedDescriptionKey])
    expectNil(nsError.userInfo[NSLocalizedFailureReasonErrorKey])
    expectNil(nsError.userInfo[NSLocalizedRecoverySuggestionErrorKey])
    expectNil(nsError.userInfo[NSHelpAnchorErrorKey])
  } else {
    expectEqual("something went horribly wrong",
      nsError.userInfo[NSLocalizedDescriptionKey] as? String)
    expectEqual("because someone wrote 'throw'",
      nsError.userInfo[NSLocalizedFailureReasonErrorKey] as? String)
    expectEqual("delete the 'throw'",
      nsError.userInfo[NSLocalizedRecoverySuggestionErrorKey] as? String)
    expectEqual("there is no help when writing tests",
      nsError.userInfo[NSHelpAnchorErrorKey] as? String)
  }
  expectEqual("something went horribly wrong", error.localizedDescription)
  expectEqual("something went horribly wrong", nsError.localizedDescription)
  expectEqual("because someone wrote 'throw'", nsError.localizedFailureReason)
  expectEqual("delete the 'throw'", nsError.localizedRecoverySuggestion)
  expectEqual("there is no help when writing tests", nsError.helpAnchor)

  // RecoverableError
  if #available(OSX 10.11, iOS 9.0, tvOS 9.0, watchOS 2.0, *) {
    expectNil(nsError.userInfo[NSLocalizedRecoveryOptionsErrorKey])
  } else {
    expectEqual(["Delete 'throw'", "Disable the test" ],
      nsError.userInfo[NSLocalizedRecoveryOptionsErrorKey] as? [String])
  }
  expectEqual(["Delete 'throw'", "Disable the test" ],
    nsError.localizedRecoveryOptions)

  // Directly recover.
  let ctx = UnsafeMutableRawPointer(bitPattern:0x1234567)
  let attempter: AnyObject
  if #available(OSX 10.11, iOS 9.0, tvOS 9.0, watchOS 2.0, *) {
    expectNil(nsError.userInfo[NSRecoveryAttempterErrorKey])
    attempter = nsError.recoveryAttempter! as AnyObject
  } else {
    attempter = nsError.userInfo[NSRecoveryAttempterErrorKey]! as AnyObject
  }
  expectEqual(attempter.attemptRecovery(fromError: nsError, optionIndex: 0), true)
  expectEqual(attempter.attemptRecovery(fromError: nsError, optionIndex: 1), false)

  // Recover through delegate.
  let rd1 = RecoveryDelegate(expectedSuccess: true, expectedContextInfo: ctx)
  expectEqual(false, rd1.called)
  attempter.attemptRecovery(
    fromError: nsError,
    optionIndex: 0,
    delegate: rd1,
    didRecoverSelector: #selector(RecoveryDelegate.recover(success:contextInfo:)),
    contextInfo: ctx)
  expectEqual(true, rd1.called)

  let rd2 = RecoveryDelegate(expectedSuccess: false, expectedContextInfo: nil)
  expectEqual(false, rd2.called)
  attempter.attemptRecovery(
    fromError: nsError,
    optionIndex: 1,
    delegate: rd2,
    didRecoverSelector: #selector(RecoveryDelegate.recover(success:contextInfo:)),
    contextInfo: nil)
  expectEqual(true, rd2.called)
}

ErrorBridgingTests.test("Customizing NSError via protocols") {
  let error = MyCustomizedError(code: 12345)
  let nsError = error as NSError

  // CustomNSError
  expectEqual("custom", nsError.domain)
  expectEqual(12345, nsError.code)
  expectEqual(URL(string: "https://swift.org"),
    nsError.userInfo[NSURLErrorKey] as? URL)

  testCustomizedError(error: error, nsError: nsError)
}

ErrorBridgingTests.test("Customizing localization/recovery via protocols") {
  let error = MySwiftCustomizedError.failed
  let nsError = error as NSError
  testCustomizedError(error: error, nsError: nsError)
}

ErrorBridgingTests.test("Customizing localization/recovery laziness") {
  let countBefore = MySwiftCustomizedError.errorDescriptionCount
  let error = MySwiftCustomizedError.failed
  let nsError = error as NSError

  // RecoverableError
  if #available(OSX 10.11, iOS 9.0, tvOS 9.0, watchOS 2.0, *) {
    expectNil(nsError.userInfo[NSLocalizedRecoveryOptionsErrorKey])
  } else {
    expectEqual(["Delete 'throw'", "Disable the test" ],
      nsError.userInfo[NSLocalizedRecoveryOptionsErrorKey] as? [String])
  }
  expectEqual(["Delete 'throw'", "Disable the test" ], nsError.localizedRecoveryOptions)

  // None of the operations above should affect the count
  if #available(OSX 10.11, iOS 9.0, tvOS 9.0, watchOS 2.0, *) {
    expectEqual(countBefore, MySwiftCustomizedError.errorDescriptionCount)
  }

  // This one does affect the count.
  expectEqual("something went horribly wrong", error.localizedDescription)

  // Check that we did get a call to errorDescription.
  if #available(OSX 10.11, iOS 9.0, tvOS 9.0, watchOS 2.0, *) {
    expectEqual(countBefore+1, MySwiftCustomizedError.errorDescriptionCount)
  }
}

enum DefaultCustomizedError1 : CustomNSError {
  case bad
  case worse
}

enum DefaultCustomizedError2 : Int, CustomNSError {
  case bad = 7
  case worse = 13
}

enum DefaultCustomizedError3 : UInt, CustomNSError {
  case bad = 9
  case worse = 115

  #if PTR_SIZE_64
  case dreadful = 0xFFFFFFFFFFFFFFFF
#elseif PTR_SIZE_32
  case dreadful = 0xFFFFFFFF
#else
#error ("Unknown pointer size")
#endif
  
  static var errorDomain: String {
    return "customized3"
  }
}

enum DefaultCustomizedParent {
    enum ChildError: CustomNSError {
        case foo
    }
}

ErrorBridgingTests.test("Default-customized via CustomNSError") {
  expectEqual(1, (DefaultCustomizedError1.worse as NSError).code)
  expectEqual(13, (DefaultCustomizedError2.worse as NSError).code)
  expectEqual(115, (DefaultCustomizedError3.worse as NSError).code)
  expectEqual(-1, (DefaultCustomizedError3.dreadful as NSError).code)
  expectEqual("main.DefaultCustomizedError1", (DefaultCustomizedError1.worse as NSError).domain)
  expectEqual("customized3", (DefaultCustomizedError3.worse as NSError).domain)
  expectEqual("main.DefaultCustomizedParent.ChildError", (DefaultCustomizedParent.ChildError.foo as NSError).domain)
}

class MyNSError : NSError {  }

ErrorBridgingTests.test("NSError subclass identity") {
  let myNSError: Error = MyNSError(domain: "MyNSError", code: 0, userInfo: [:])
  let nsError = myNSError as NSError
  expectTrue(type(of: nsError) == MyNSError.self)
}

ErrorBridgingTests.test("Wrapped NSError identity") {
  let nsError = NSError(domain: NSCocoaErrorDomain,
                   code: NSFileNoSuchFileError,
                   userInfo: [
                     NSFilePathErrorKey : "/dev/null",
                     NSStringEncodingErrorKey : /*ASCII=*/1,
                   ])

  let error: Error = nsError
  let nsError2: NSError = error as NSError
  expectTrue(nsError === nsError2)

  // Extracting the NSError via the runtime.
  let cocoaErrorAny: Any = error as! CocoaError
  let nsError3: NSError = cocoaErrorAny as! NSError
  expectTrue(nsError === nsError3)

  if let cocoaErrorAny2: Any = error as? CocoaError {
    let nsError4: NSError = cocoaErrorAny2 as! NSError
    expectTrue(nsError === nsError4)
  } else {
    expectUnreachable()
  }

  // Extracting the NSError via direct call.
  let cocoaError = error as! CocoaError
  let nsError5: NSError = cocoaError as NSError
  expectTrue(nsError === nsError5)

  if error is CocoaError {
    let nsError6: NSError = cocoaError as NSError
    expectTrue(nsError === nsError6)
  } else {
    expectUnreachable()
  }
}

extension Error {
  func asNSError() -> NSError {
    return self as NSError
  }
}

func unconditionalCast<T>(_ x: Any, to: T.Type) -> T {
  return x as! T
}

func conditionalCast<T>(_ x: Any, to: T.Type) -> T? {
  return x as? T
}

// https://github.com/apple/swift/issues/44171
ErrorBridgingTests.test("Error archetype identity") {
  let myError = NSError(domain: "myErrorDomain", code: 0,
                        userInfo: [ "one" : 1 ])
  expectTrue(myError === myError.asNSError())

  expectTrue(unconditionalCast(myError, to: Error.self) as NSError
     === myError)
  expectTrue(conditionalCast(myError, to: Error.self)! as NSError
     === myError)

  let nsError = NSError(domain: NSCocoaErrorDomain,
                        code: NSFileNoSuchFileError,
                        userInfo: [
                          NSFilePathErrorKey : "/dev/null",
                          NSStringEncodingErrorKey : /*ASCII=*/1,
                        ])
  let cocoaError = nsError as Error as! CocoaError
  expectTrue(cocoaError.asNSError() === nsError)
  expectTrue(unconditionalCast(cocoaError, to: Error.self) as NSError
    === nsError)
  expectTrue(conditionalCast(cocoaError, to: Error.self)! as NSError
    === nsError)
}

// https://github.com/apple/swift/issues/51855

class ParentA: NSObject {
  @objc(ParentAError) enum Error: Int, Swift.Error {
    case failed
  }
}
class ParentB: NSObject {
  @objc(ParentBError) enum Error: Int, Swift.Error {
    case failed
  }
}
private class NonPrintAsObjCClass: NSObject {
  @objc enum Error: Int, Swift.Error {
    case foo
  }
}
@objc private enum NonPrintAsObjCError: Int, Error {
  case bar
}

ErrorBridgingTests.test("@objc error domains for nested types") {
  // Domain strings should correspond to Swift types, including parent types.
  expectEqual(ParentA.Error.failed._domain, "main.ParentA.Error")
  expectEqual(ParentB.Error.failed._domain, "main.ParentB.Error")

  func makeNSError(like error: Error) -> NSError {
    return NSError(domain: error._domain, code: error._code)
  }

  // NSErrors corresponding to Error types with the same name but nested in
  // different enclosing types should not be castable to the wrong error type.
  expectTrue(makeNSError(like: ParentA.Error.failed) is ParentA.Error)
  expectFalse(makeNSError(like: ParentA.Error.failed) is ParentB.Error)
  expectFalse(makeNSError(like: ParentB.Error.failed) is ParentA.Error)
  expectTrue(makeNSError(like: ParentB.Error.failed) is ParentB.Error)

  // If an @objc enum error is not eligible for PrintAsObjC, we should treat it
  // as though it inherited the default implementation, which calls
  // String(reflecting:).
  expectEqual(NonPrintAsObjCClass.Error.foo._domain,
              String(reflecting: NonPrintAsObjCClass.Error.self))
  expectEqual(NonPrintAsObjCError.bar._domain,
              String(reflecting: NonPrintAsObjCError.self))
}

ErrorBridgingTests.test("error-to-NSObject casts") {
  let error = MyCustomizedError(code: 12345)

  if #available(SwiftStdlib 5.2, *) {
    // Unconditional cast
    let nsErrorAsObject1 = unconditionalCast(error, to: NSObject.self)
    let nsError1 = unconditionalCast(nsErrorAsObject1, to: NSError.self)
    expectEqual("custom", nsError1.domain)
    expectEqual(12345, nsError1.code)

    // Conditional cast
    let nsErrorAsObject2 = conditionalCast(error, to: NSObject.self)!
    let nsError2 = unconditionalCast(nsErrorAsObject2, to: NSError.self)
    expectEqual("custom", nsError2.domain)
    expectEqual(12345, nsError2.code)

    // "is" check
    expectTrue(error is NSObject)

    // Unconditional cast to a dictionary.
    let dict = ["key" : NoisyError()]
    let anyOfDict = dict as AnyObject
    let dict2 = anyOfDict as! [String: NSObject]
  }
}

// https://github.com/apple/swift-corelibs-foundation/issues/3701
// Casting 'CFError' or 'NSError' to 'Error' results in a memory leak
ErrorBridgingTests.test("NSError-to-Error casts") {
  func should_not_leak_nserror() {
    let something: Any? = NSError(domain: "Foo", code: 1)
    expectTrue(something is Error)
  }

  if #available(SwiftStdlib 5.2, *) {
    // TODO: Wrap some leak checking around this
    // Until then, this is a helpful debug tool
		should_not_leak_nserror()
  }
}

ErrorBridgingTests.test("CFError-to-Error casts") {
  func should_not_leak_cferror() {
    let something: Any? = CFErrorCreate(kCFAllocatorDefault, kCFErrorDomainCocoa, 1, [:] as CFDictionary)
    expectTrue(something is Error)
  }

  if #available(SwiftStdlib 5.2, *) {
    // TODO: Wrap some leak checking around this
    // Until then, this is a helpful debug tool
		should_not_leak_cferror()
  }
}

// https://github.com/apple/swift/issues/51697

enum MyError: Error {
  case someThing
}

ErrorBridgingTests.test("Crash in failed cast to 'NSError'") {

  if #available(SwiftStdlib 5.2, *) {
    let error = MyError.someThing
    let foundationError = error as NSError

    if let urlError = foundationError as? URLError {
      expectUnreachable()
    }
  }
}

// https://github.com/apple/swift/issues/50193

enum SwiftError: Error, CustomStringConvertible {
  case something
  var description: String { return "Something" }
}

ErrorBridgingTests.test("Swift Error bridged to NSError description") {
  func checkDescription() {
    let bridgedError = SwiftError.something as NSError
    expectEqual("Something", bridgedError.description)
  }

  if #available(SwiftStdlib 5.3, *) {
    checkDescription()
  }
}

struct SwiftError2: Error, CustomStringConvertible {
  var description: String
}

struct SwiftErrorLarge: Error, CustomStringConvertible {
  var description: String
  var makeItLarge = (1, 2, 3, 4, 5, 6, 7, 8, 9, 10)
}

ErrorBridgingTests.test("Swift Error description memory management") {
  func checkDescription() {
    // Generate a non-small, non-constant NSString bridged to String.
    let str = (["""
      There once was a gigantic genie
      Who turned out to be a real meanie
      I wished for flight
      And with all his might
      He gave me a propellor beanie
    """] as NSArray).description
    let error = SwiftError2(description: str)
    let bridgedError = error as NSError

    // Ensure that the bridged NSError description method doesn't overrelease
    // the error value.
    for _ in 0 ..< 10 {
      autoreleasepool {
        expectEqual(str, bridgedError.description)
      }
    }

    // Make sure large structs also work.
    let largeError = SwiftErrorLarge(description: str)
    let largeBridgedError = largeError as NSError
    for _ in 0 ..< 10 {
      autoreleasepool {
        expectEqual(str, largeBridgedError.description)
      }
    }
  }

  if #available(SwiftStdlib 5.3, *) {
    checkDescription()
  }
}

runAllTests()
