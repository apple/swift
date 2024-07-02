// RUN: %target-swiftxx-frontend -I %S/Inputs %s -emit-ir | %FileCheck %s

import CustomNewOperator

var x = callsCustomNew()

// Make sure the definition of `operator new` is emitted.
// CHECK: define {{.*}} @{{_ZnwmPv15container_new_t|"\?\?2@YAPEAX_KPEAXUcontainer_new_t@@@Z"}}
