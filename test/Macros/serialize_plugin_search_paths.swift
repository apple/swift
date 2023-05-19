// RUN: %empty-directory(%t)

// RUN: %target-build-swift %s -g -o %t/a.out \
// RUN:   -emit-executable -emit-module \
// RUN:   -Xfrontend -serialize-debugging-options \
// RUN:   -module-name MyApp \
// RUN:   -swift-version 5 -enable-experimental-feature Macros \
// RUN:   -plugin-path %t/plugins \
// RUN:   -external-plugin-path %t/plugins#%swift-plugin-server \
// RUN:   -load-plugin-library %t/%target-library-name(MacroDefinition) \
// RUN:   -load-plugin-executable %t/mock-plugin#TestPlugin

// RUN: %lldb-moduleimport-test -verbose -dump-module %t/a.out | %FileCheck %s
// CHECK: - Macro Search Paths:
// CHECK:     -plugin-path: {{.*}}plugins
// CHECK:     -plugin-path: {{.*}}plugins
// CHECK:     -plugin-path: {{.*}}plugins
// CHECK:     -external-plugin-path: {{.*}}plugins#{{.*}}swift-plugin-server
// CHECK:     -load-plugin-library: {{.*}}MacroDefinition.{{dylib|so|dll}}
// CHECK:     -load-plugin-executable: {{.*}}MacroDefinition.{{dylib|so|dll}}
// CHECK:     -load-plugin-executable: {{.*}}mock-plugin#TestPlugin
