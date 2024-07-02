// REQUIRES: swift_swift_parser
// REQUIRES: OS=macosx

// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/plugins)
// RUN: split-file %s %t

// RUN: touch %t/plugins/libTestPlugin.dylib

// RUN: not %target-swift-frontend \
// RUN:   -typecheck \
// RUN:   -swift-version 5 \
// RUN:   -external-plugin-path %t/plugins#%swift-plugin-server \
// RUN:   -module-name MyApp \
// RUN:   -serialize-diagnostics-path %t/macro_expand.dia \
// RUN:   %t/test.swift

// RUN: c-index-test -read-diagnostics %t/macro_expand.dia 2>&1 | %FileCheck -check-prefix SERVER %s

// SERVER-NOT: {{error|warning}}
// SERVER: test.swift:1:33: warning: external macro implementation type 'TestPlugin.FooMacro' could not be found for macro 'fooMacro'; failed to load library plugin 'BUILD_DIR/{{.*}}/libTestPlugin.dylib' in plugin server 'BUILD_DIR/{{.*}}/swift-plugin-server'; loader error: dlopen(BUILD_DIR/{{.*}}/libTestPlugin.dylib, {{.*}}): tried: 'BUILD_DIR/{{.*}}/plugins/libTestPlugin.dylib'
// SERVER: test.swift:4:7: error: external macro implementation type 'TestPlugin.FooMacro' could not be found for macro 'fooMacro'; failed to load library plugin 'BUILD_DIR/{{.*}}/libTestPlugin.dylib' in plugin server 'BUILD_DIR/{{.*}}/swift-plugin-server'; loader error: dlopen(BUILD_DIR/{{.*}}/libTestPlugin.dylib, {{.*}}): tried: 'BUILD_DIR/{{.*}}/plugins/libTestPlugin.dylib'
// SERVER: test.swift:1:33: note: 'fooMacro' declared here
// SERVER-NOT: {{error|warning}}

// RUN: not %target-swift-frontend \
// RUN:   -typecheck \
// RUN:   -swift-version 5 \
// RUN:   -plugin-path %t/plugins \
// RUN:   -module-name MyApp \
// RUN:   -serialize-diagnostics-path %t/macro_expand_inproc.dia \
// RUN:   %t/test.swift

// RUN: c-index-test -read-diagnostics %t/macro_expand_inproc.dia 2>&1 | %FileCheck -check-prefix INPROC %s

// INPROC-NOT: {{error|warning}}
// INPROC: test.swift:1:33: warning: external macro implementation type 'TestPlugin.FooMacro' could not be found for macro 'fooMacro'; failed to load library plugin 'BUILD_DIR/{{.*}}/libTestPlugin.dylib' in plugin server 'BUILD_DIR/{{.*}}/libSwiftInProcPluginServer.dylib'; loader error: dlopen(BUILD_DIR/{{.*}}/libTestPlugin.dylib, 0x0005): tried: 'BUILD_DIR/{{.*}}/libTestPlugin.dylib'
// INPROC: test.swift:4:7: error: external macro implementation type 'TestPlugin.FooMacro' could not be found for macro 'fooMacro'; failed to load library plugin 'BUILD_DIR/{{.*}}/libTestPlugin.dylib' in plugin server 'BUILD_DIR/{{.*}}/libSwiftInProcPluginServer.dylib'; loader error: dlopen(BUILD_DIR/{{.*}}/libTestPlugin.dylib, 0x0005): tried: 'BUILD_DIR/{{.*}}/libTestPlugin.dylib'
// INPROC: test.swift:1:33: note: 'fooMacro' declared here
// INPROC-NOT: {{error|warning}}

//--- test.swift
@freestanding(expression) macro fooMacro(_: Any) -> String = #externalMacro(module: "TestPlugin", type: "FooMacro")

func test() {
  _ = #fooMacro(1)
}
