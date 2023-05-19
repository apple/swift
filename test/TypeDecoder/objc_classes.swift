// RUN: %empty-directory(%t)

// RUN: %target-build-swift -emit-executable %s -g -o %t/objc_classes -emit-module

// RUN: sed -ne '/\/\/ *DEMANGLE-TYPE: /s/\/\/ *DEMANGLE-TYPE: *//p' < %s > %t/input
// RUN: %lldb-moduleimport-test-with-sdk %t/objc_classes -type-from-mangled=%t/input | %FileCheck %s --check-prefix=CHECK-TYPE --match-full-lines

// RUN: sed -ne '/\/\/ *DEMANGLE-DECL: /s/\/\/ *DEMANGLE-DECL: *//p' < %s > %t/input
// RUN: %lldb-moduleimport-test-with-sdk %t/objc_classes -decl-from-mangled=%t/input | %FileCheck %s --check-prefix=CHECK-DECL --match-full-lines

// REQUIRES: objc_interop

import Foundation

func blackHole(_: Any...) {}

@objc protocol OurObjCProtocol {}
class OurObjCClass : NSObject, OurObjCProtocol {}

do {
  let x1: NSSet = NSSet()
  let x2: NSFastEnumeration = x1
  let x3: OurObjCProtocol = OurObjCClass()
  let x4: NSCache = NSCache<NSNumber, NSString>()
  let x5: PropertyListSerialization.WriteOptions = 0

  blackHole(x1, x2, x3, x4, x5)
}

do {
  let x1: NSSet.Type = NSSet.self
  let x2: NSFastEnumeration.Type = x1
  let x3: OurObjCProtocol.Type = OurObjCClass.self
  let x4: NSCache.Type = NSCache<NSNumber, NSString>.self
  let x5: PropertyListSerialization.WriteOptions = 0

  blackHole(x1, x2, x3, x4, x5)
}

do {
  let x1: NSFastEnumeration.Protocol = NSFastEnumeration.self
  let x2: OurObjCProtocol.Protocol = OurObjCProtocol.self

  blackHole(x1, x2)
}

// DEMANGLE-TYPE: $sSo5NSSetCD
// DEMANGLE-TYPE: $sSo17NSFastEnumeration_pD
// DEMANGLE-TYPE: $s12objc_classes15OurObjCProtocol_pD
// DEMANGLE-TYPE: $sSo7NSCacheCySo8NSNumberCSo8NSStringCGD
// DEMANGLE-TYPE: $sSo26NSPropertyListWriteOptionsaD

// CHECK-TYPE: NSSet
// FIXME(https://github.com/apple/swift/issues/65879): Should be 'any NSFastEnumeration'
// CHECK-TYPE: NSFastEnumeration
// FIXME(https://github.com/apple/swift/issues/65879): Should be 'any OurObjCProtocol'
// CHECK-TYPE: OurObjCProtocol
// CHECK-TYPE: NSCache<NSNumber, NSString>
// CHECK-TYPE: PropertyListSerialization.WriteOptions

// DEMANGLE-TYPE: $sSo5NSSetCmD
// DEMANGLE-TYPE: $sSo5NSSetCXMTD
// DEMANGLE-TYPE: $sSo17NSFastEnumeration_pXpD
// DEMANGLE-TYPE: $s12objc_classes15OurObjCProtocol_pXpD
// DEMANGLE-TYPE: $sSo7NSCacheCySo8NSNumberCSo8NSStringCGmD
// DEMANGLE-TYPE: $sSo26NSPropertyListWriteOptionsamD

// CHECK-TYPE: NSSet.Type
// CHECK-TYPE: @thick NSSet.Type
// CHECK-TYPE: any NSFastEnumeration.Type
// CHECK-TYPE: any OurObjCProtocol.Type
// CHECK-TYPE: NSCache<NSNumber, NSString>.Type
// CHECK-TYPE: PropertyListSerialization.WriteOptions.Type

// DEMANGLE-TYPE: $sSo17NSFastEnumeration_pmD
// DEMANGLE-TYPE: $s12objc_classes15OurObjCProtocol_pmD

// FIXME(https://github.com/apple/swift/issues/65880): Should be '(any NSFastEnumeration).Type'
// CHECK-TYPE: NSFastEnumeration.Type
// FIXME(https://github.com/apple/swift/issues/65880): Should be '(any OurObjCProtocol).Type'
// CHECK-TYPE: OurObjCProtocol.Type


// DEMANGLE-DECL: $sSo5NSSetC
// DEMANGLE-DECL: $sSo17NSFastEnumerationP
// DEMANGLE-DECL: $s12objc_classes15OurObjCProtocolP
// DEMANGLE-DECL: $sSo7NSCacheCySo8NSNumberCSo8NSStringCG
// DEMANGLE-DECL: $sSo26NSPropertyListWriteOptionsa

// CHECK-DECL: Foundation.(file).NSSet
// CHECK-DECL: Foundation.(file).NSFastEnumeration
// CHECK-DECL: objc_classes.(file).OurObjCProtocol
// CHECK-DECL: Foundation.(file).NSCache
// CHECK-DECL: Foundation.(file).PropertyListSerialization extension.WriteOptions
