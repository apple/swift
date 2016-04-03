// RUN: not %target-build-swift -parse %s 2>&1 | FileCheck -check-prefix=CHECK-%target-os -check-prefix=CHECK-BOTH %s
// REQUIRES: executable_test

struct IntWrapper {
  let value: Int
}

class IBActionWrapperTy {
  @IBAction func nullary() {}
  // CHECK-ios-NOT: attr_ibaction_ios.swift:[[@LINE-1]]
  // CHECK-macosx: attr_ibaction_ios.swift:[[@LINE-2]]:18: error: @IBAction methods must have a single argument
  // CHECK-watchos-NOT: attr_ibaction_ios.swift:[[@LINE-3]]
  
  @IBAction func reqReq(_ _: AnyObject, _: AnyObject) {}
  @IBAction func reqOpt(_ _: AnyObject, _: AnyObject?) {}
  @IBAction func reqImp(_ _: AnyObject, _: AnyObject!) {}
  @IBAction func optReq(_ _: AnyObject?, _: AnyObject) {}
  @IBAction func optOpt(_ _: AnyObject?, _: AnyObject?) {}
  @IBAction func optImp(_ _: AnyObject?, _: AnyObject!) {}
  @IBAction func impReq(_ _: AnyObject!, _: AnyObject) {}
  @IBAction func impOpt(_ _: AnyObject!, _: AnyObject?) {}
  @IBAction func impImp(_ _: AnyObject!, _: AnyObject!) {}
  // CHECK-ios-NOT: attr_ibaction_ios.swift:[[@LINE-9]]
  // CHECK-ios-NOT: attr_ibaction_ios.swift:[[@LINE-9]]
  // CHECK-ios-NOT: attr_ibaction_ios.swift:[[@LINE-9]]
  // CHECK-ios-NOT: attr_ibaction_ios.swift:[[@LINE-9]]
  // CHECK-ios-NOT: attr_ibaction_ios.swift:[[@LINE-9]]
  // CHECK-ios-NOT: attr_ibaction_ios.swift:[[@LINE-9]]
  // CHECK-ios-NOT: attr_ibaction_ios.swift:[[@LINE-9]]
  // CHECK-ios-NOT: attr_ibaction_ios.swift:[[@LINE-9]]
  // CHECK-ios-NOT: attr_ibaction_ios.swift:[[@LINE-9]]
  // CHECK-macosx: attr_ibaction_ios.swift:[[@LINE-18]]:18: error: @IBAction methods must have a single argument
  // CHECK-macosx: attr_ibaction_ios.swift:[[@LINE-18]]:18: error: @IBAction methods must have a single argument
  // CHECK-macosx: attr_ibaction_ios.swift:[[@LINE-18]]:18: error: @IBAction methods must have a single argument
  // CHECK-macosx: attr_ibaction_ios.swift:[[@LINE-18]]:18: error: @IBAction methods must have a single argument
  // CHECK-macosx: attr_ibaction_ios.swift:[[@LINE-18]]:18: error: @IBAction methods must have a single argument
  // CHECK-macosx: attr_ibaction_ios.swift:[[@LINE-18]]:18: error: @IBAction methods must have a single argument
  // CHECK-macosx: attr_ibaction_ios.swift:[[@LINE-18]]:18: error: @IBAction methods must have a single argument
  // CHECK-macosx: attr_ibaction_ios.swift:[[@LINE-18]]:18: error: @IBAction methods must have a single argument
  // CHECK-macosx: attr_ibaction_ios.swift:[[@LINE-18]]:18: error: @IBAction methods must have a single argument
  // CHECK-watchos-NOT: attr_ibaction_ios.swift:[[@LINE-27]]
  // CHECK-watchos-NOT: attr_ibaction_ios.swift:[[@LINE-27]]
  // CHECK-watchos-NOT: attr_ibaction_ios.swift:[[@LINE-27]]
  // CHECK-watchos-NOT: attr_ibaction_ios.swift:[[@LINE-27]]
  // CHECK-watchos-NOT: attr_ibaction_ios.swift:[[@LINE-27]]
  // CHECK-watchos-NOT: attr_ibaction_ios.swift:[[@LINE-27]]
  // CHECK-watchos-NOT: attr_ibaction_ios.swift:[[@LINE-27]]
  // CHECK-watchos-NOT: attr_ibaction_ios.swift:[[@LINE-27]]
  // CHECK-watchos-NOT: attr_ibaction_ios.swift:[[@LINE-27]]

  @IBAction func reqBad(_ _: AnyObject, _: IBActionWrapperTy) {}
  // CHECK-ios: attr_ibaction_ios.swift:[[@LINE-1]]:18: error: argument to @IBAction method cannot have non-'@objc' class type
  // CHECK-macosx: attr_ibaction_ios.swift:[[@LINE-2]]:18: error: @IBAction methods must have a single argument
  // CHECK-watch: attr_ibaction_ios.swift:[[@LINE-3]]:18: error: argument to @IBAction method cannot have non-'@objc' class type

  @IBAction func badReq(_ _: Int, _: AnyObject) {}
  // CHECK-ios: attr_ibaction_ios.swift:[[@LINE-1]]:18: error: argument to @IBAction method cannot have non-object type
  // CHECK-macosx: attr_ibaction_ios.swift:[[@LINE-2]]:18: error: @IBAction methods must have a single argument

  @IBAction func badBad(_ _: Int, _: IBActionWrapperTy) {}
  // CHECK-ios: attr_ibaction_ios.swift:[[@LINE-1]]:18: error: argument to @IBAction method cannot have non-object type
  // CHECK-ios: attr_ibaction_ios.swift:[[@LINE-2]]:18: error: argument to @IBAction method cannot have non-'@objc' class type
  // CHECK-macosx: attr_ibaction_ios.swift:[[@LINE-3]]:18: error: @IBAction methods must have a single argument

  @IBAction func tooManyArgs(_ _: AnyObject, _: AnyObject, _: AnyObject) {}
  // CHECK-ios: attr_ibaction_ios.swift:[[@LINE-1]]:18: error: @IBAction methods can only have up to 2 arguments
  // CHECK-macosx: attr_ibaction_ios.swift:[[@LINE-2]]:18: error: @IBAction methods must have a single argument

  @IBAction func watchKitLike(_ _: Int) {}
  // CHECK-ios-NOT: attr_ibaction_ios.swift:[[@LINE-1]]
  // CHECK-macosx: attr_ibaction_ios.swift:[[@LINE-2]]:18: error: argument to @IBAction method cannot have non-object type

  @IBAction func watchKitLikeBad(_ _: IntWrapper) {}
  // CHECK-BOTH: attr_ibaction_ios.swift:[[@LINE-1]]:18: error: argument to @IBAction method cannot have non-object type

  @IBAction func watchKitLikeOpt(_ _: Int?) {}
  // CHECK-BOTH: attr_ibaction_ios.swift:[[@LINE-1]]:18: error: argument to @IBAction method cannot have non-object type
}
