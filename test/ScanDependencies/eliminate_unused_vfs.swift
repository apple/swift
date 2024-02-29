// REQUIRES: objc_interop
// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/module-cache)
// RUN: %empty-directory(%t/redirects)
// RUN: split-file %s %t

// RUN: sed -e "s|OUT_DIR|%t/redirects|g"  -e "s|IN_DIR|%S/Inputs/CHeaders|g" %t/overlay_template.yaml > %t/overlay.yaml

// RUN: %target-swift-frontend -scan-dependencies -module-load-mode prefer-interface -module-cache-path %t/module-cache %t/test.swift -o %t/deps.json -I %S/Inputs/CHeaders -I %S/Inputs/Swift -disable-implicit-concurrency-module-import -disable-implicit-string-processing-module-import -Xcc -ivfsoverlay -Xcc %t/overlay.yaml
// RUN: %validate-json %t/deps.json | %FileCheck %s

//--- overlay_template.yaml
{
  'version': 0,
  'use-external-names': false,
  'roots': [
    {
      'name': 'IN_DIR', 'type': 'directory',
      'contents': [
      ]
    },
  ]
}

//--- test.swift
import F

// CHECK: "mainModuleName": "deps"
/// --------Main module
// CHECK-LABEL: "modulePath": "deps.swiftmodule",
// CHECK-NEXT: sourceFiles
// CHECK-NEXT: test.swift
// CHECK-NEXT: ],
// CHECK-NEXT: "directDependencies": [
// CHECK-DAG:     "swift": "F"
// CHECK-DAG:     "swift": "Swift"
// CHECK-DAG:     "swift": "SwiftOnoneSupport"
// CHECK: ],

// Ensure that the VFS overlay command-line flag is pruned on the Swift module dependency
// that uses a Clang module which has optimized it away as un-used.
/// --------Swift module F
// CHECK-LABEL: "modulePath": "{{.*}}{{/|\\}}F-{{.*}}.swiftmodule",

// CHECK: "directDependencies": [
// CHECK-NEXT:   {
// CHECK-DAG:     "clang": "F"
// CHECK-DAG:     "swift": "Swift"
// CHECK-DAG:     "swift": "SwiftOnoneSupport"
// CHECK-NEXT:   }
// CHECK-NEXT: ],

// CHECK: "commandLine": [
// CHECK: "-compile-module-from-interface"
// CHECK-NOT: "-ivfsoverlay",
// CHECK-NOT: "{{.*}}{{/|\\}}preserve_used_vfs.swift.tmp{{/|\\}}overlay.yaml",
// CHECK: ],

/// --------Clang module F
// CHECK-LABEL: "modulePath": "{{.*}}{{/|\\}}F-{{.*}}.pcm",
// CHECK-NOT: "-ivfsoverlay",
// CHECK-NOT: "{{.*}}{{/|\\}}preserve_used_vfs.swift.tmp{{/|\\}}overlay.yaml",
