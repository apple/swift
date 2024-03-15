// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/includes)

// Build support Protocols module
// RUN: %target-build-swift %S/Inputs/PreservedConformanceProtocols.swift -parse-as-library -emit-module -emit-library -module-name PreservedConformanceProtocols -o %t/includes/PreservedConformanceProtocols.o

// Build the test into a binary
// RUN: %target-build-swift %s -parse-as-library -emit-module -emit-library -module-name PreservedConformances -O -whole-module-optimization -I %t/includes -o %t/PreservedConformances -Xlinker %t/includes/PreservedConformanceProtocols.o

// RUN: %target-swift-reflection-dump %t/PreservedConformances | %FileCheck %s

import PreservedConformanceProtocols

struct internalTestEntity : TestEntity {
    struct internalNestedTestEntity : TestEntity {}
}
private struct privateTestEntity : TestEntity {
    private struct privateNestedTestEntity : TestEntity {}
}
fileprivate struct filePrivateTestEntity : TestEntity {}
public struct publicTestEntity : TestEntity {}

// CHECK: CONFORMANCES:
// CHECK: =============
// CHECK-DAG: 21PreservedConformances16publicTestEntityV (PreservedConformances.publicTestEntity) : PreservedConformanceProtocols.TestEntity
// CHECK-DAG: 21PreservedConformances21filePrivateTestEntity5${{[0-9a-f]+}}LLV (PreservedConformances.(filePrivateTestEntity in ${{[0-9a-f]+}})) : PreservedConformanceProtocols.TestEntity
// CHECK-DAG: 21PreservedConformances17privateTestEntity5${{[0-9a-f]+}}LLV (PreservedConformances.(privateTestEntity in ${{[0-9a-f]+}})) : PreservedConformanceProtocols.TestEntity
// CHECK-DAG: 21PreservedConformances17privateTestEntity5${{[0-9a-f]+}}LLV0c6NesteddE0V (PreservedConformances.(privateTestEntity in ${{[0-9a-f]+}}).privateNestedTestEntity) : PreservedConformanceProtocols.TestEntity
// CHECK-DAG: 21PreservedConformances18internalTestEntityV (PreservedConformances.internalTestEntity) : PreservedConformanceProtocols.TestEntity
// CHECK-DAG: 21PreservedConformances18internalTestEntityV0c6NesteddE0V (PreservedConformances.internalTestEntity.internalNestedTestEntity) : PreservedConformanceProtocols.TestEntity
