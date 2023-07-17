// REQUIRES: objc_interop

// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/clang-module-cache
// RUN: mkdir -p %t/cas
// RUN: split-file %s %t
// RUN: %hmaptool write %t/hmap.json %t/empty.hmap

// RUN: %target-swift-frontend -scan-dependencies -module-cache-path %t/clang-module-cache \
// RUN:   %t/Test.swift -o %t/deps.json -cache-compile-job -cas-path %t/cas -clang-include-tree \
// RUN:   -Xcc -fmodule-map-file=%t/module.modulemap -Xcc -ivfsoverlay -Xcc %t/empty.yaml \
// RUN:   -Xcc -I%t/empty.hmap
// RUN: %validate-json %t/deps.json &>/dev/null

// RUN: %S/Inputs/SwiftDepsExtractor.py %t/deps.json deps casFSRootID > %t/fs.casid
// RUN: llvm-cas --cas %t/cas --ls-tree-recursive @%t/fs.casid | %FileCheck %s -DDIR=%basename_t -check-prefix FS_ROOT
// RUN: %S/Inputs/SwiftDepsExtractor.py %t/deps.json clang:Dummy clangIncludeTree > %t/tree.casid
// RUN: clang-cas-test --cas %t/cas --print-include-tree @%t/tree.casid | %FileCheck %s -DDIR=%basename_t -check-prefix INCLUDE_TREE

// FS_ROOT: [[DIR]].tmp/empty.hmap
// FS_ROOT: [[DIR]].tmp/empty.yaml

// INCLUDE_TREE: [[DIR]].tmp/Dummy.h

//--- Test.swift
import Dummy
func test() {}

//--- module.modulemap
module Dummy {
 umbrella header "Dummy.h"
}

//--- Dummy.h
void dummy(void);

//--- hmap.json
{
  "mappings": {} 
}

//--- empty.yaml
{
  "version": 0,
  "case-sensitive": "false",
  "use-external-names": true,
  "roots": []
}

