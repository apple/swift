// REQUIRES: OS=linux-gnu || OS=freebsd
// RUN: %empty-directory(%t)
// RUN: %target-build-swift -Xfrontend -function-sections -emit-module -emit-library -static -parse-stdlib %S/Inputs/FunctionSections.swift
// RUN: %target-build-swift -Xlinker -v -Xlinker --gc-sections -Xlinker -Map=%t/../../FunctionSections.map -I%t/../.. -L%t/../.. -lFunctionSections %S/Inputs/FunctionSectionsUse.swift 2>&1 | sed 's/.*\(gold\|LLD\).*/\1/g' | tr "[:lower:]" "[:upper:]" > %t/../../Linker.txt
// RUN: %FileCheck --check-prefix $(cat %t/../../Linker.txt) %s < %t/../../FunctionSections.map

// GOLD: Discarded input sections
// GOLD: .text.$s16FunctionSections5func2yyF
// GOLD: Memory map
// GOLD: .text.$s16FunctionSections5func1yyF

// LLD: .text.$s16FunctionSections5func1yyF
// LLD-NOT: .text.$s16FunctionSections5func2yyF
