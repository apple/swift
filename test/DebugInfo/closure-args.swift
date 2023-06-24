// RUN: %target-swift-frontend %s -emit-ir -g -o - | %FileCheck %s

import Swift

func main() -> Void
{
    // I am line 6.
    var random_string = "b"
    var random_int = 5
    var out_only = 2013

    var backward_ptr  =
    // CHECK: define internal {{.*}} i1 @"$s4mainAAyyFSbSS_SStcfU_"(
    // CHECK: %[[RANDOM_STR_ADDR:.*]] = alloca ptr, align {{(4|8)}}

    // FIXME(TODO: JIRA): i386 String is temporarily larger, and that causes the
    // value to be by-address. When that is fixed, remove the optional
    // DW_OP_deref below.
    //
    // CHECK-NEXT: call void @llvm.dbg.declare(metadata ptr %[[RANDOM_STR_ADDR]], metadata !{{.*}}, metadata !DIExpression({{(DW_OP_deref)?}})), !dbg

    // CHECK: store ptr %{{.*}}, ptr %[[RANDOM_STR_ADDR]], align {{(4|8)}}
    // CHECK-DAG: !DILocalVariable(name: "lhs",{{.*}} line: [[@LINE+5]],
    // CHECK-DAG: !DILocalVariable(name: "rhs",{{.*}} line: [[@LINE+4]],
    // CHECK-DAG: !DILocalVariable(name: "random_string",{{.*}} line: 8,
    // CHECK-DAG: !DILocalVariable(name: "random_int",{{.*}} line: 9,
    // CHECK-DAG: !DILocalVariable(name: "out_only",{{.*}} line: 10,
        { (lhs : String, rhs : String) -> Bool in
            if rhs == random_string
               || rhs.unicodeScalars.count == random_int
            {
            // Ensure the two local_vars are in different lexical scopes.
            // CHECK-DAG: !DILocalVariable(name: "local_var", scope: ![[THENSCOPE:[0-9]+]],{{.*}} line: [[@LINE+2]],
            // CHECK-DAG: ![[THENSCOPE]] = distinct !DILexicalBlock({{.*}} line: [[@LINE+1]]
                var local_var : Int64 = 10
                print("I have an int here \(local_var).\n", terminator: "")
                return false
            }
            else
            {
            // CHECK-DAG: !DILocalVariable(name: "local_var", scope: ![[ELSESCOPE:[0-9]+]],{{.*}} line: [[@LINE+2]]
            // CHECK-DAG: ![[ELSESCOPE]] = distinct !DILexicalBlock({{.*}} line: [[@LINE+1]],
                var local_var : String = "g"
                print("I have another string here \(local_var).\n", terminator: "")
                // Assign to all the captured variables to inhibit capture promotion.
                random_string = "c"
                random_int = -1
                out_only = 333
                return rhs < lhs
            }
        }

    var bool = backward_ptr("a" , "b")
}

main()

