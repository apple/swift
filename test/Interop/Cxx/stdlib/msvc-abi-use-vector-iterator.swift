// RUN: %target-run-simple-swift(-I %S/Inputs -Xfrontend -enable-experimental-cxx-interop -Xcc -std=c++17)

// REQUIRES: OS=windows-msvc
// REQUIRES: executable_test

import MsvcUseVecIt

// CHECK:   call void @"?begin@?$vector@V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@V?$allocator@V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@2@@std@@QEBA?AV?$_Vector_const_iterator@V?$_Vector_val@U?$_Simple_types@V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@std@@@std@@@2@XZ"(ptr {{.*}}, ptr noalias nocapture sret

func test() -> Bool {
    let result = f()
    let begin = result.pointee.providers.__beginUnsafe()
    let end = result.pointee.providers.__endUnsafe()
    return begin != end
}

let _ = test()
