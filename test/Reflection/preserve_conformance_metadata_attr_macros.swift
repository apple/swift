// REQUIRES: objc_interop
// REQUIRES: OS=macosx
// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/includes)

// Build support Protocols module
// RUN: %target-build-swift %S/Inputs/PreservedConformanceProtocols.swift -parse-as-library -emit-module -emit-library -module-name PreservedConformanceProtocols -o %t/includes/PreservedConformanceProtocols.o

// Build the macro library
// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroDefinition) -module-name=MacroDefinition %S/Inputs/Macros.swift -g -no-toolchain-stdlib-rpath

// Build the test into a binary
// RUN: %target-build-swift %s -parse-as-library -emit-module -emit-library -module-name PreservedConformances -O -whole-module-optimization -I %t/includes -o %t/PreservedConformances -Xlinker %t/includes/PreservedConformanceProtocols.o -load-plugin-library %t/%target-library-name(MacroDefinition)

// RUN: %target-swift-reflection-dump %t/PreservedConformances | %FileCheck %s

import PreservedConformanceProtocols

@freestanding(declaration, names: named(MacroAddedStruct))
macro AddMacroAddedStruct() = #externalMacro(module: "MacroDefinition", type: "AddStructDeclMacro")

@attached(peer, names: prefixed(_Peer_))
macro AddPeerStruct() = #externalMacro(module: "MacroDefinition", type: "AddPeerStructMacro")

@attached(extension, conformances: TestEntity, names: prefixed(_extension_), named(_Extension_TestEntity))
macro AddExtension() = #externalMacro(module: "MacroDefinition", type: "AddExtensionMacro")

#AddMacroAddedStruct

struct internalTestEntity : TestEntity {}
public struct publicTestEntity : TestEntity {}
@AddPeerStruct
struct internalMacroAidedEntityHelper {}
@AddExtension
struct internalMacroExtensionAidedEntityHelper {}
// CHECK: CONFORMANCES:
// CHECK: =============
// CHECK-DAG: 21PreservedConformances16publicTestEntityV (PreservedConformances.publicTestEntity) : PreservedConformanceProtocols.TestEntity
// CHECK-DAG: 21PreservedConformances18internalTestEntityV (PreservedConformances.internalTestEntity) : PreservedConformanceProtocols.TestEntity
// CHECK-DAG: 21PreservedConformances16MacroAddedStructV (PreservedConformances.MacroAddedStruct) : PreservedConformanceProtocols.TestEntity
// CHECK-DAG: 21PreservedConformances36_Peer_internalMacroAidedEntityHelperV (PreservedConformances._Peer_internalMacroAidedEntityHelper) : PreservedConformanceProtocols.TestEntity
// CHECK-DAG: 21PreservedConformances39internalMacroExtensionAidedEntityHelperV (PreservedConformances.internalMacroExtensionAidedEntityHelper) : PreservedConformanceProtocols.TestEntity
