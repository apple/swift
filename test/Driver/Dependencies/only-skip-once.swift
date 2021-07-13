// XFAIL: linux, freebsd, openbsd, windows

// RUN: %empty-directory(%t)
// RUN: cp -r %S/Inputs/only-skip-once/* %t
// RUN: touch -t 201401240005 %t/*

// RUN: cd %t && %target-swiftc_driver -driver-show-job-lifecycle -output-file-map %t/output-file-map.json -incremental main.swift file1.swift file2.swift -j1 2>&1 | %FileCheck -check-prefix=CHECK-INITIAL %s

// CHECK-INITIAL: {{(Job finished: {compile: main.o <= main.swift}|Finished Compiling main.swift)}}
// CHECK-INITIAL: {{(Job finished: {compile: file1.o <= file1.swift}|Finished Compiling file1.swift)}}
// CHECK-INITIAL: {{(Job finished: {compile: file2.o <= file2.swift}|Finished Compiling file2.swift)}}
// CHECK-INITIAL: {{(Job finished: {link: main <= main.o file1.o file2.o}|Finished Linking main)}}

// RUN: touch -t 201401240006 %t/file2.swift
// RUN: cd %t && %target-swiftc_driver -driver-show-job-lifecycle -output-file-map %t/output-file-map.json -incremental main.swift file1.swift file2.swift -j1 2>&1 |  %FileCheck -check-prefix=CHECK-REBUILD %s

// We should skip the main and file1 rebuilds here, but we should only note skipping them _once_
// CHECK-REBUILD-DAG: {{(Job finished: {compile: file2.o <= file2.swift}|Finished Compiling file2.swift)}}
// CHECK-REBUILD-DAG: {{(Job skipped: {compile: main.o <= main.swift}|Skipped Compiling main.swift)}}
// CHECK-REBUILD-DAG: {{(Job skipped: {compile: file1.o <= file1.swift}|Skipped Compiling file1.swift)}}
// CHECK-REBUILD-DAG: {{(Job finished: {link: main <= main.o file1.o file2.o}|Finished Linking main)}}
// CHECK-REBUILD-NOT: {{(Job skipped:|Skipped)}}
