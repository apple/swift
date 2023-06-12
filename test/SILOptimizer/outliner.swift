// RUN: %target-swift-frontend -Osize -import-objc-header %S/Inputs/Outliner.h %s -emit-sil -enforce-exclusivity=unchecked -enable-copy-propagation -enable-lexical-borrow-scopes=false | %FileCheck %s
// RUN: %target-swift-frontend -Osize -g -import-objc-header %S/Inputs/Outliner.h %s -emit-sil -enforce-exclusivity=unchecked -enable-copy-propagation -enable-lexical-borrow-scopes=false | %FileCheck %s

// REQUIRES: objc_interop
// REQUIRES: optimized_stdlib
// REQUIRES: swift_in_compiler

import Foundation

public class MyGizmo {
   private var gizmo : Gizmo
   private var optionalGizmo : Gizmo?

   init() {
     gizmo = Gizmo()
  }

   // CHECK-LABEL: sil {{.*}}@$s8outliner7MyGizmoC11usePropertyyyF :
   // CHECK: [[A_FUN:%.*]] = function_ref @$sSo5GizmoC14stringPropertySSSgvgToTeab_
   // CHECK: apply [[A_FUN]]({{.*}}) : $@convention(thin) (@in_guaranteed Gizmo) -> @owned Optional<String>
   // CHECK-NOT: return
   // CHECK: [[P_FUN:%.*]] = function_ref @$sSo5GizmoC14stringPropertySSSgvgToTeob_
   // CHECK: apply [[P_FUN]]({{.*}}) : $@convention(thin) (@owned Gizmo) -> @owned Optional<String>
   // CHECK: return
   // CHECK: } // end sil function '$s8outliner7MyGizmoC11usePropertyyyF'
   public func useProperty() {
     print(gizmo.stringProperty)
     print(optionalGizmo!.stringProperty)
   }
}

// CHECK-LABEL: sil @$s8outliner13testOutliningyyF :
// CHECK:  [[FUN:%.*]] = function_ref @$sSo5GizmoC14stringPropertySSSgvgToTepb_
// CHECK:  apply [[FUN]](%{{.*}}) : $@convention(thin) (Gizmo) -> @owned Optional<String>
// CHECK:  apply [[FUN]](%{{.*}}) : $@convention(thin) (Gizmo) -> @owned Optional<String>
// CHECK:  [[FUN:%.*]] = function_ref @$sSo5GizmoC14stringPropertySSSgvsToTembnn_
// CHECK:  apply [[FUN]]({{.*}}) : $@convention(thin) (@owned String, Gizmo) -> ()
// CHECK:  apply [[FUN]]({{.*}}) : $@convention(thin) (@owned String, Gizmo) -> ()
// CHECK:  [[FUN:%.*]] = function_ref @$sSo5GizmoC12modifyString_10withNumber0D6FoobarSSSgAF_SiypSgtFToTembnnnb_
// CHECK:  apply [[FUN]]({{.*}}) : $@convention(thin) (@owned String, Int, Optional<AnyObject>, Gizmo) -> @owned Optional<String>
// CHECK:  apply [[FUN]]({{.*}}) : $@convention(thin) (@owned String, Int, Optional<AnyObject>, Gizmo) -> @owned Optional<String>
// CHECK:  [[FUN:%.*]] = function_ref @$sSo5GizmoC11doSomethingyypSgSaySSGSgFToTembgnn_
// CHECK:  apply [[FUN]]({{.*}}) : $@convention(thin) (@guaranteed Array<String>, Gizmo) -> @owned Optional<AnyObject>
// CHECK:  [[FUN:%.*]] = function_ref @$sSo5GizmoC11doSomethingyypSgSaySSGSgFToTembnn_
// CHECK:  apply [[FUN]]({{.*}}) : $@convention(thin) (@owned Array<String>, Gizmo) -> @owned Optional<AnyObject>
// CHECK: return
// CHECK: } // end sil function '$s8outliner13testOutliningyyF'
public func testOutlining() {
  let gizmo = Gizmo()
  let foobar = Gizmo()
  print(gizmo.stringProperty)
  print(gizmo.stringProperty)

  gizmo.stringProperty = "foobar"
  gizmo.stringProperty = "foobar2"
  gizmo.modifyString("hello", withNumber:1, withFoobar: foobar)
  gizmo.modifyString("hello", withNumber:1, withFoobar: foobar)
  let arr = [ "foo", "bar"]
  gizmo.doSomething(arr)
  gizmo.doSomething(arr)
}

// CHECK-LABEL: sil @$s8outliner9dontCrash1ayyp_tF : $@convention(thin) (@in_guaranteed Any) -> () {
// CHECK:  [[OBJ:%.*]] = open_existential_ref {{.*}} : $AnyObject to $@opened("{{.*}}", AnyObject) Self
// CHECK:  [[METH:%.*]] = objc_method [[OBJ]] : $@opened("{{.*}}", AnyObject) Self, #Treeish.treeishChildren!foreign : <Self where Self : Treeish> (Self) -> () -> [Any]?
// CHECK:  [[RES:%.*]] = apply [[METH]]([[OBJ]]) : $@convention(objc_method)
// CHECK: } // end sil function '$s8outliner9dontCrash1ayyp_tF'

// CHECK-LABEL: sil shared [noinline] @$sSo5GizmoC14stringPropertySSSgvgToTeab_ : {{.*}} {
// CHECK:       {{bb[0-9]+}}([[ADDR:%[^,]+]] : $*Gizmo):
// CHECK:         [[INSTANCE:%[^,]+]] = load [[ADDR]]
// CHECK:         [[CONSUMING_OUTLINED_BRIDGED_PROPERTY:%[^,]+]] = function_ref @$sSo5GizmoC14stringPropertySSSgvgToTeob_
// CHECK:         strong_retain [[INSTANCE]]
// CHECK:         [[RETVAL:%[^,]+]] = apply [[CONSUMING_OUTLINED_BRIDGED_PROPERTY]]([[INSTANCE]])
// CHECK:         return [[RETVAL]]
// CHECK: } // end sil function '$sSo5GizmoC14stringPropertySSSgvgToTeab_'

// CHECK-LABEL: sil shared [noinline] @$sSo5GizmoC14stringPropertySSSgvgToTeob_ : {{.*}} {
// CHECK:       {{bb[0-9]+}}([[INSTANCE:%[^,]+]] :
// CHECK:         [[GIZME_STRINGPROPERTY_GETTER:%[^,]+]] = objc_method [[INSTANCE]] : $Gizmo, #Gizmo.stringProperty!getter.foreign 
// CHECK:         [[MAYBE_NSSTRING:%[^,]+]] = apply [[GIZME_STRINGPROPERTY_GETTER]]([[INSTANCE]])
// CHECK:         strong_release [[INSTANCE]]
// CHECK:         switch_enum [[MAYBE_NSSTRING]] : $Optional<NSString>, case #Optional.some!enumelt: [[SOME_BLOCK:bb[0-9]+]], case #Optional.none!enumelt: [[NONE_BLOCK:bb[0-9]+]]
// CHECK:       [[SOME_BLOCK]]([[REGISTER_5:%[^,]+]] : $NSString):                              
// CHECK:         [[STRING_FROM_NSSTRING:%[^,]+]] = function_ref @$sSS10FoundationE36_unconditionallyBridgeFromObjectiveCySSSo8NSStringCSgFZ
// CHECK:         [[STRING_TYPE:%[^,]+]] = metatype $@thin String.Type
// CHECK:         [[STRING:%[^,]+]] = apply [[STRING_FROM_NSSTRING]]([[MAYBE_NSSTRING]], [[STRING_TYPE]])
// CHECK:         release_value [[MAYBE_NSSTRING]]
// CHECK:         [[SOME_STRING:%[^,]+]] = enum $Optional<String>, #Optional.some!enumelt, [[STRING]]
// CHECK:         br [[EXIT:bb[0-9]+]]([[SOME_STRING]] : $Optional<String>)
// CHECK:       [[NONE_BLOCK]]:                                              
// CHECK:         [[NONE_STRING:%[^,]+]] = enum $Optional<String>, #Optional.none!enumelt
// CHECK:         br [[EXIT]]([[NONE_STRING]] : $Optional<String>)
// CHECK:       [[EXIT]]([[MAYBE_STRING:%[^,]+]] :
// CHECK:         return [[MAYBE_STRING]]
// CHECK-LABEL: } // end sil function '$sSo5GizmoC14stringPropertySSSgvgToTeob_'

// CHECK-LABEL: sil shared [noinline] @$sSo5GizmoC14stringPropertySSSgvgToTepb_ : $@convention(thin) (Gizmo) -> @owned Optional<String>
// CHECK: bb0(%0 : $Gizmo):
// CHECK:  %1 = objc_method %0 : $Gizmo, #Gizmo.stringProperty!getter.foreign : (Gizmo) -> () -> String?
// CHECK:  %2 = apply %1(%0) : $@convention(objc_method) (Gizmo) -> @autoreleased Optional<NSString>
// CHECK:  switch_enum %2 : $Optional<NSString>, case #Optional.some!enumelt: bb1, case #Optional.none!enumelt: bb2
// CHECK:bb1(%4 : $NSString):
// CHECK:  %5 = function_ref @$sSS10FoundationE36_unconditionallyBridgeFromObjectiveCySSSo8NSStringCSgFZ : $@convention(method) (@guaranteed Optional<NSString>, @thin String.Type) -> @owned String
// CHECK:  %6 = metatype $@thin String.Type
// CHECK:  %7 = apply %5(%2, %6) : $@convention(method) (@guaranteed Optional<NSString>, @thin String.Type) -> @owned String
// CHECK:  release_value %2 : $Optional<NSString>
// CHECK:  %9 = enum $Optional<String>, #Optional.some!enumelt, %7 : $String
// CHECK:  br bb3(%9 : $Optional<String>)
// CHECK:bb2:
// CHECK:  %11 = enum $Optional<String>, #Optional.none!enumelt
// CHECK:  br bb3(%11 : $Optional<String>)
// CHECK:bb3(%13 : $Optional<String>):
// CHECK:  return %13 : $Optional<String>
// CHECK: } // end sil function '$sSo5GizmoC14stringPropertySSSgvgToTepb_'

// CHECK-LABEL: sil shared [noinline] @$sSo5GizmoC14stringPropertySSSgvsToTembnn_ : $@convention(thin) (@owned String, Gizmo) -> () {
// CHECK: bb0(%0 : $String, %1 : $Gizmo):
// CHECK:   %2 = objc_method %1 : $Gizmo, #Gizmo.stringProperty!setter.foreign : (Gizmo) -> (String?) -> ()
// CHECK:   %3 = function_ref @$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF : $@convention(method) (@guaranteed String) -> @owned NSString
// CHECK:   %4 = apply %3(%0) : $@convention(method) (@guaranteed String) -> @owned NSString
// CHECK:   release_value %0 : $String
// CHECK:   %6 = enum $Optional<NSString>, #Optional.some!enumelt, %4 : $NSString
// CHECK:   %7 = apply %2(%6, %1) : $@convention(objc_method) (Optional<NSString>, Gizmo) -> ()
// CHECK:   strong_release %4 : $NSString
// CHECK:   return %7 : $()
// CHECK: } // end sil function '$sSo5GizmoC14stringPropertySSSgvsToTembnn_'

// CHECK-LABEL: sil shared [noinline] @$sSo5GizmoC12modifyString_10withNumber0D6FoobarSSSgAF_SiypSgtFToTembnnnb_ : $@convention(thin) (@owned String, Int, Optional<AnyObject>, Gizmo) -> @owned Optional<String> {
// CHECK: bb0(%0 : $String, %1 : $Int, %2 : $Optional<AnyObject>, %3 : $Gizmo):
// CHECK:   %4 = objc_method %3 : $Gizmo, #Gizmo.modifyString!foreign : (Gizmo) -> (String?, Int, Any?) -> String?
// CHECK:   %5 = function_ref @$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF : $@convention(method) (@guaranteed String) -> @owned NSString
// CHECK:   %6 = apply %5(%0) : $@convention(method) (@guaranteed String) -> @owned NSString
// CHECK:   release_value %0 : $String
// CHECK:   %8 = enum $Optional<NSString>, #Optional.some!enumelt, %6 : $NSString
// CHECK:   %9 = apply %4(%8, %1, %2, %3) : $@convention(objc_method) (Optional<NSString>, Int, Optional<AnyObject>, Gizmo) -> @autoreleased Optional<NSString>
// CHECK:   strong_release %6 : $NSString
// CHECK:   switch_enum %9 : $Optional<NSString>, case #Optional.some!enumelt: bb2, case #Optional.none!enumelt: bb1
//
// CHECK: bb1:
// CHECK:   %12 = enum $Optional<String>, #Optional.none!enumelt
// CHECK:   br bb3(%12 : $Optional<String>)
//
// CHECK: bb2(%14 : $NSString):
// CHECK:   %15 = function_ref @$sSS10FoundationE36_unconditionallyBridgeFromObjectiveCySSSo8NSStringCSgFZ : $@convention(method) (@guaranteed Optional<NSString>, @thin String.Type) -> @owned String
// CHECK:   %16 = metatype $@thin String.Type
// CHECK:   %17 = apply %15(%9, %16) : $@convention(method) (@guaranteed Optional<NSString>, @thin String.Type) -> @owned String
// CHECK:   release_value %9 : $Optional<NSString>
// CHECK:   %19 = enum $Optional<String>, #Optional.some!enumelt, %17 : $String
// CHECK:   br bb3(%19 : $Optional<String>)
//
// CHECK: bb3(%21 : $Optional<String>):
// CHECK:   return %21 : $Optional<String>
// CHECK: } // end sil function '$sSo5GizmoC12modifyString_10withNumber0D6FoobarSSSgAF_SiypSgtFToTembnnnb_'

// CHECK-LABEL: sil shared [noinline] @$sSo5GizmoC11doSomethingyypSgSaySSGSgFToTembgnn_ : $@convention(thin) (@guaranteed Array<String>, Gizmo) -> @owned Optional<AnyObject> {
// CHECK: bb0(%0 : $Array<String>, %1 : $Gizmo):
// CHECK:   %2 = objc_method %1 : $Gizmo, #Gizmo.doSomething!foreign : (Gizmo) -> ([String]?) -> Any?
// CHECK:   %3 = function_ref @$sSa10FoundationE19_bridgeToObjectiveCSo7NSArrayCyF : $@convention(method) <{{.*}}> (@guaranteed Array<{{.*}}>) -> @owned NSArray
// CHECK:   %4 = apply %3<String>(%0) : $@convention(method) <{{.*}}> (@guaranteed Array<{{.*}}>) -> @owned NSArray
// CHECK-NOT:   release_value
// CHECK:   %5 = enum $Optional<NSArray>, #Optional.some!enumelt, %4 : $NSArray
// CHECK:   %6 = apply %2(%5, %1) : $@convention(objc_method) (Optional<NSArray>, Gizmo) -> @autoreleased Optional<AnyObject>
// CHECK:   strong_release %4 : $NSArray
// CHECK:   return %6 : $Optional<AnyObject>
// CHECK: } // end sil function '$sSo5GizmoC11doSomethingyypSgSaySSGSgFToTembgnn_'

public func dontCrash<T: Proto>(x : Gizmo2<T>) {
  let s = x.doSomething()
  print(s)

}

public func dontCrash2(_ c: SomeGenericClass) -> Bool {
  guard let str = c.version else {
      return false
  }
  guard let str2 = c.doSomething() else {
      return false
  }

  let arr = [ "foo", "bar"]
  c.doSomething2(arr)

  return true
}

func dontCrash3() -> String? {
  let bundle = Bundle.main
  let resource = "common parameter"

  return bundle.path(forResource: resource, ofType: "png")
      ?? bundle.path(forResource: resource, ofType: "apng")
}

extension Operation {
  func dontCrash4() {
    if completionBlock != nil {
      completionBlock = { }
    }
  }
}

public func dontCrash(a: Any)  {
  (a as AnyObject).treeishChildren()
}

public class Foo : NSObject {
  var x: MyView? = nil

  public func dontCrash(_ pt: MyPoint) -> Bool {
    guard let somView = x,
          let window = somView.window else {
      return false
    }

    guard let event = MyEvent.mouseEvent(with: .A,
                                         location: pt,
                                         windowNumber: window.windowNumber,
                                         context: nil,
                                         eventNumber: 0,
                                         clickCount: 1,
                                         pressure:1) else {
      print("failure")
      return false
    }
    print(event)
    return true
  }
}

public func testCalendar() {
   let formatter = DateFormatter()
   formatter.calendar = Calendar(identifier: .gregorian)
}

open class Test
{
   @inline(never)
   public func getWindow() -> MyWindow
   {
       return MyWindow()
   }

   public func testDontCrash() -> MyView
   {

     let view = MyView()
     view.window2 = getWindow()
     return view
   }
}

internal extension NSError {
   convenience init(myError code: Int) {
     self.init(domain: "error", code: code, userInfo: [:])
   }
}

public class AnotherTest {
  public let obj: MyObject
  public init(obj: MyObject) {
    self.obj = obj
  }

  public func dontCrash() {
      self.obj.error = NSError(myError: 10)
  }
}
