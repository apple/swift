// RUN: %cxx-all-targets(-I %S/Inputs %s -emit-ir | %FileCheck %s)

// REQUIRES: rdar67257133

import PartiallyPreDefinedClassTemplate

public func getWrappedMagicInt() -> CInt {
  let myInt = IntWrapper(value: 7)
  var magicInt = PartiallyPreDefinedMagicallyWrappedInt(t: myInt)
  return magicInt.getValuePlusArg(13)
}

// CHECK: define {{(protected |dllexport )?}}swiftcc i32 @"$s4main18getWrappedMagicInts5Int32VyF"()
// CHECK: %magicInt = alloca %TSo037__CxxTemplateInst12MagicWrapperI10IntE1EV, align 4
// CHECK: %magicInt.t = getelementptr inbounds %TSo037__CxxTemplateInst12MagicWrapperI10IntE1EV, %TSo037__CxxTemplateInst12MagicWrapperI10IntE1EV* %magicInt, i32 0, i32 0
// CHECK: [[MAGIC_WRAPPER:%.*]] = bitcast %TSo037__CxxTemplateInst12MagicWrapperI10IntE1EV* %magicInt to %struct.MagicWrapper*
// CHECK: call i32 @{{_ZNK12MagicWrapperI10IntWrapperE15getValuePlusArgEi|"\?getValuePlusArg@\?\$MagicWrapper@UIntWrapper@@@@QEBAHH@Z"}}(%struct.MagicWrapper* [[MAGIC_WRAPPER]], i32 13)

// CHECK: define weak_odr{{( dso_local)?}} i32 @{{_ZNK12MagicWrapperI10IntWrapperE15getValuePlusArgEi|"\?getValuePlusArg@\?\$MagicWrapper@UIntWrapper@@@@QEBAHH@Z"}}(%struct.MagicWrapper* %this, i32 %arg)
// CHECK: %this.addr = alloca %struct.MagicWrapper*, align {{4|8}}
// CHECK: store %struct.MagicWrapper* %this, %struct.MagicWrapper** %this.addr, align {{4|8}}
// CHECK: %this1 = load %struct.MagicWrapper*, %struct.MagicWrapper** %this.addr, align {{4|8}}
// CHECK: %t = getelementptr inbounds %struct.MagicWrapper, %struct.MagicWrapper* %this1, i32 0, i32 0
// CHECK: %call = call i32 @{{_ZNK10IntWrapper8getValueEv|"\?getValue@IntWrapper@@QEBAHXZ"}}(%struct.IntWrapper* %t)
// CHECK: [[ARG:%.*]] = load i32, i32* %arg.addr, align 4
// CHECK: %add = add nsw i32 %call, [[ARG]]
// CHECK: ret i32 %add

// CHECK: define linkonce_odr{{( dso_local)?}} i32 @{{_ZNK10IntWrapper8getValueEv|"\?getValue@IntWrapper@@QEBAHXZ"}}(%struct.IntWrapper* %this)
// CHECK: %this.addr = alloca %struct.IntWrapper*, align {{4|8}}
// CHECK: store %struct.IntWrapper* %this, %struct.IntWrapper** %this.addr, align {{4|8}}
// CHECK: %this1 = load %struct.IntWrapper*, %struct.IntWrapper** %this.addr, align {{4|8}}
// CHECK: %value = getelementptr inbounds %struct.IntWrapper, %struct.IntWrapper* %this1, i32 0, i32 0
// CHECK: [[VALUE:%.*]] = load i32, i32* %value, align 4
// CHECK: ret i32 [[VALUE]]
