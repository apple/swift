// REQUIRES: objc_interop

// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/mods)
// RUN: split-file %s %t

// RUN: %target-swift-ide-test -print-indexed-symbols -enable-objc-interop -source-filename %t/ObjcUser.swift -Xcc -fmodule-map-file=%t/module.modulemap | %FileCheck -dump-input=always %t/ObjcUser.swift

//--- objc_decls.h
#import <Foundation/Foundation.h>
@protocol MemberAdding<NSObject>
@property int protocolAddedProperty;
-(void)protocolAddedMethod;
@end

@interface ObjCClass : NSObject
@property int baseClassProperty;
@end

@interface ObjCClass (category) <MemberAdding>
@property int categoryAddedProperty;
@end

//--- module.modulemap
module objc_decls {
  header "objc_decls.h"
  export *
}

//--- ObjcUser.swift
import objc_decls
func test() { // CHECK: [[@LINE]]:6 | function/Swift | test() | [[s:.*]] | Def | rel: 0
  let c = ObjCClass()
  let _ = c.baseClassProperty
  // CHECK: [[@LINE-1]]:13 | instance-property/Swift | baseClassProperty | c:objc(cs)ObjCClass(py)baseClassProperty | Ref,Read,Dyn,RelRec,RelCont | rel: 2
  let _ = c.categoryAddedProperty
  // CHECK: [[@LINE-1]]:13 | instance-property/Swift | categoryAddedProperty | c:objc(cs)ObjCClass(py)categoryAddedProperty | Ref,Read,Dyn,RelRec,RelCont | rel: 2
  // CHECK: [[@LINE-2]]:13 | instance-method/acc-get/Swift | getter:categoryAddedProperty | c:@CM@objc_decls@@objc(cs)ObjCClass(im)categoryAddedProperty | Ref,Call,Dyn,Impl,RelRec,RelCall,RelCont | rel: 2
  let _ = c.protocolAddedProperty
  // CHECK: [[@LINE-1]]:13 | instance-property/Swift | protocolAddedProperty | c:objc(pl)MemberAdding(py)protocolAddedProperty | Ref,Read,Dyn,RelRec,RelCont | rel: 2
  // CHECK: [[@LINE-2]]:13 | instance-method/acc-get/Swift | getter:protocolAddedProperty | c:@CM@objc_decls@@objc(cs)ObjCClass(im)protocolAddedProperty | Ref,Call,Dyn,Impl,RelRec,RelCall,RelCont | rel: 2 
  c.protocolAddedMethod()
  // CHECK: [[@LINE-1]]:5 | instance-method/Swift | protocolAddedMethod() | c:objc(pl)MemberAdding(im)protocolAddedMethod | Ref,Call,Dyn,RelRec,RelCall,RelCont | rel: 2 
}
