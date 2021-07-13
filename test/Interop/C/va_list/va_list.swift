// RUN: %target-swift-ide-test -print-module -module-to-print=ImportCVAList -I %S/Inputs -source-filename=x | %FileCheck %s

// CHECK: var va: CVaListPointer
// CHECK: var gnu: CVaListPointer
// CHECK: var isoC: CVaListPointer
// CHECK: var underscore: CVaListPointer
