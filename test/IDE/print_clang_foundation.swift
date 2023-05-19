// RUN: %empty-directory(%t)

// RUN: %target-swift-ide-test -print-module -source-filename %s -module-to-print=Foundation.NSArray -function-definitions=false > %t/Foundation.NSArray.printed.txt
// RUN: %FileCheck -input-file %t/Foundation.NSArray.printed.txt -check-prefix=CHECK_NSARRAY %s

// REQUIRES: objc_interop

// CHECK_NSARRAY: class NSArray
// CHECK_NSARRAY: init()

// CHECK_NSARRAY: {{^}}  var sortedArrayHint: Data { get }

// CHECK_NSARRAY: convenience init?(contentsOfFile path: String)
// CHECK_NSARRAY: convenience init?(contentsOf url: URL)


// CHECK_NSARRAY: class NSMutableArray : NSArray
// CHECK_NSARRAY:   func setArray(_ otherArray: [Any])

// RUN: %target-swift-ide-test -print-module -source-filename %s -module-to-print=Foundation.NSKeyValueCoding -function-definitions=false -print-regular-comments > %t/Foundation.NSKeyValueCoding.printed.txt
// RUN: %FileCheck -input-file %t/Foundation.NSKeyValueCoding.printed.txt -check-prefix=CHECK2 %s

// CHECK2: extension NSObject

// RUN: %target-swift-ide-test -print-module -source-filename %s -module-to-print=Foundation.NSString -function-definitions=false > %t/Foundation.NSString.printed.txt
// RUN: %FileCheck -input-file %t/Foundation.NSString.printed.txt -check-prefix=CHECK_NSSTRING %s
// RUN: %FileCheck -input-file %t/Foundation.NSString.printed.txt -check-prefix=CHECK_DICTIONARY %s

// Make sure that we don't qualify 'NSErrorPointer'.
// CHECK_NSSTRING: init(contentsOfFile path: String, encoding enc: UInt) throws

// CHECK_DICTIONARY: func propertyListFromStringsFileFormat() -> [AnyHashable : Any]

// RUN: %target-swift-ide-test -print-module -source-filename %s -module-to-print=Foundation -function-definitions=false > %t/Foundation.printed.txt
// RUN: %FileCheck -input-file %t/Foundation.printed.txt -check-prefix=CHECK_DUP %s

// CHECK_DUP: import CoreFoundation{{$}}
// CHECK_DUP-NOT: import CoreFoundation{{$}}

// RUN: %target-swift-ide-test -print-module -source-filename %s -module-to-print=Foundation.NSTimeZone -function-definitions=false > %t/Foundation.NSTimeZone.printed.txt
// RUN: %FileCheck -input-file %t/Foundation.NSTimeZone.printed.txt -check-prefix=CHECK_NSTIMEZONE %s

// CHECK_NSTIMEZONE: func secondsFromGMT(for aDate: Date) -> Int
// CHECK_NSTIMEZONE: func abbreviation(for aDate: Date) -> String?
// CHECK_NSTIMEZONE: var nextDaylightSavingTimeTransition: Date? { get }

// RUN: %target-swift-ide-test -print-module -source-filename %s -module-to-print=Foundation.NSCalendar -function-definitions=false > %t/Foundation.NSCalendar.printed.txt
// RUN: %FileCheck -input-file %t/Foundation.NSCalendar.printed.txt -check-prefix=CHECK_NSCALENDAR %s

// CHECK_NSCALENDAR: func date(from comps: DateComponents) -> Date?

// RUN: %target-swift-ide-test -print-module -source-filename %s -module-to-print=Foundation.NSDateInterval -function-definitions=false > %t/Foundation.NSDateInterval.printed.txt
// RUN: %FileCheck -input-file %t/Foundation.NSDateInterval.printed.txt -check-prefix=CHECK_NSDATEINTERVAL %s

// CHECK_NSDATEINTERVAL: func intersection(with dateInterval: DateInterval) -> DateInterval?

// RUN: %target-swift-ide-test -print-module -source-filename %s -module-to-print=Foundation.NSCoder -function-definitions=false > %t/Foundation.NSCoder.printed.txt
// RUN: %FileCheck -input-file %t/Foundation.NSCoder.printed.txt -check-prefix=CHECK_NSCODER %s

// CHECK_NSCODER: func failWithError(_ error: any Error)
// CHECK_NSCODER: {{^}}  var error: (any Error)? { get }
