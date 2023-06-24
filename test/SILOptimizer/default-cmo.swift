
// RUN: %empty-directory(%t) 

// RUN: %target-build-swift -O -wmo -Xfrontend -enable-default-cmo -parse-as-library -emit-module -emit-module-path=%t/Submodule.swiftmodule -module-name=Submodule %S/Inputs/cross-module/default-submodule.swift -c -o %t/submodule.o
// RUN: %target-build-swift -O -wmo -Xfrontend -enable-default-cmo -parse-as-library -emit-module -emit-module-path=%t/Module.swiftmodule -module-name=Module -I%t -I%S/Inputs/cross-module %S/Inputs/cross-module/default-module.swift -c -o %t/module.o
// RUN: %target-build-swift -O -wmo -Xfrontend -enable-default-cmo -parse-as-library -emit-tbd -emit-tbd-path %t/ModuleTBD.tbd -emit-module -emit-module-path=%t/ModuleTBD.swiftmodule -module-name=ModuleTBD -I%t -I%S/Inputs/cross-module %S/Inputs/cross-module/default-module.swift -c -o %t/moduletbd.o

// RUN: %target-build-swift -O -wmo -module-name=Main -I%t -I%S/Inputs/cross-module %s -emit-sil | %FileCheck %s

// REQUIRES: swift_in_compiler

import Module
import ModuleTBD

// CHECK-LABEL: sil_global public_external @$s6Module0A6StructV22privateFunctionPointeryS2icvpZ : $@callee_guaranteed (Int) -> Int{{$}}

public func callPublicFunctionPointer(_ x: Int) -> Int {
  return Module.ModuleStruct.publicFunctionPointer(x)
}

// CHECK-LABEL: sil @$s4Main25callPublicFunctionPointeryS2iF :
// CHECK:         global_addr
// CHECK:         load
// CHECK:         apply
// CHECK:       } // end sil function '$s4Main25callPublicFunctionPointeryS2iF'
public func callPrivateFunctionPointer(_ x: Int) -> Int {
  return Module.ModuleStruct.privateFunctionPointer(x)
}

// CHECK-LABEL: sil @$s4Main24callPrivateCFuncInModuleSiyF : $@convention(thin) () -> Int {
// CHECK:         function_ref @$s6Module16callPrivateCFuncSiyF
// CHECK:       } // end sil function '$s4Main24callPrivateCFuncInModuleSiyF'
public func callPrivateCFuncInModule() -> Int {
  return Module.callPrivateCFunc()
}

// CHECK-LABEL: sil @$s4Main22usePrivateCVarInModuleSiyF : $@convention(thin) () -> Int {
// CHECK:         function_ref @$s6Module14usePrivateCVarSiyF
// CHECK:       } // end sil function '$s4Main22usePrivateCVarInModuleSiyF'
public func usePrivateCVarInModule() -> Int {
  return Module.usePrivateCVar()
}

// CHECK-LABEL: sil @$s4Main11doIncrementyS2iF
// CHECK-NOT:     function_ref 
// CHECK-NOT:     apply 
// CHECK:       } // end sil function '$s4Main11doIncrementyS2iF'
public func doIncrement(_ x: Int) -> Int {
  return Module.incrementByThree(x)
}

// CHECK-LABEL: sil @$s4Main19doIncrementWithCallyS2iF
// CHECK:         function_ref @$s9Submodule19incrementByOneNoCMOyS2iF
// CHECK:       } // end sil function '$s4Main19doIncrementWithCallyS2iF'
public func doIncrementWithCall(_ x: Int) -> Int {
  return Module.incrementByThreeWithCall(x)
}

// CHECK-LABEL: sil @$s4Main14doIncrementTBDyS2iF
// CHECK-NOT:     function_ref 
// CHECK-NOT:     apply 
// CHECK:       } // end sil function '$s4Main14doIncrementTBDyS2iF'
public func doIncrementTBD(_ x: Int) -> Int {
  return ModuleTBD.incrementByThree(x)
}

// CHECK-LABEL: sil @$s4Main22doIncrementTBDWithCallyS2iF
// CHECK:         function_ref @$s9ModuleTBD24incrementByThreeWithCallyS2iF
// CHECK:       } // end sil function '$s4Main22doIncrementTBDWithCallyS2iF'
public func doIncrementTBDWithCall(_ x: Int) -> Int {
  return ModuleTBD.incrementByThreeWithCall(x)
}

// CHECK-LABEL: sil @$s4Main23getSubmoduleKlassMemberSiyF
// CHECK-NOT:     function_ref 
// CHECK-NOT:     apply 
// CHECK:       } // end sil function '$s4Main23getSubmoduleKlassMemberSiyF'
public func getSubmoduleKlassMember() -> Int {
  return Module.submoduleKlassMember()
}

// CHECK-LABEL: sil @$s4Main26getSubmoduleKlassMemberTBDSiyF
// CHECK-NOT:     function_ref 
// CHECK-NOT:     apply 
// CHECK:       } // end sil function '$s4Main26getSubmoduleKlassMemberTBDSiyF'
public func getSubmoduleKlassMemberTBD() -> Int {
  return ModuleTBD.submoduleKlassMember()
}

// CHECK-LABEL: sil @$s4Main20getModuleKlassMemberSiyF
// CHECK-NOT:     function_ref 
// CHECK-NOT:     apply 
// CHECK:       } // end sil function '$s4Main20getModuleKlassMemberSiyF'
public func getModuleKlassMember() -> Int {
  return Module.moduleKlassMember()
}

// CHECK-LABEL: sil @$s4Main23getModuleKlassMemberTBDSiyF
// CHECK-NOT:     function_ref 
// CHECK-NOT:     apply 
// CHECK:       } // end sil function '$s4Main23getModuleKlassMemberTBDSiyF'
public func getModuleKlassMemberTBD() -> Int {
  return ModuleTBD.moduleKlassMember()
}

