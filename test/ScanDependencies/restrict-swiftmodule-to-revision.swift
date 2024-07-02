// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: env SWIFT_DEBUG_FORCE_SWIFTMODULE_REVISION=1.0.0.0.1 \
// RUN:   %target-swift-frontend -emit-module -module-name Foo -o %t/match/Foo.swiftmodule -swift-version 5 \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   -emit-module-interface-path %t/match/Foo.swiftinterface -enable-library-evolution %t/foo.swift

// RUN: env SWIFT_DEBUG_FORCE_SWIFTMODULE_REVISION=1.0.0.1 \
// RUN:   %target-swift-frontend -emit-module -module-name Foo -o %t/new/Foo.swiftmodule -swift-version 5 \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   -emit-module-interface-path %t/new/Foo.swiftinterface -enable-library-evolution %t/foo.swift

// RUN: env SWIFT_DEBUG_FORCE_SWIFTMODULE_REVISION=1.0.0.0.2 \
// RUN:   %target-swift-frontend -emit-module -module-name Foo -o %t/revision/Foo.swiftmodule -swift-version 5 \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   -emit-module-interface-path %t/revision/Foo.swiftinterface -enable-library-evolution %t/foo.swift

// RUN: env SWIFT_DEBUG_FORCE_SWIFTMODULE_REVISION=1.0.0.0.1 \
// RUN:   %target-swift-frontend -scan-dependencies -module-name Test -O -module-load-mode prefer-binary \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   %t/main.swift -o %t/deps-1.json -swift-version 5 -I %t/match

// RUN: %FileCheck %s --check-prefix=BINARY --input-file=%t/deps-1.json

// RUN: env SWIFT_DEBUG_FORCE_SWIFTMODULE_REVISION=1.0.0.0.1 \
// RUN:   %target-swift-frontend -scan-dependencies -module-name Test -O -module-load-mode prefer-binary \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   %t/main.swift -o %t/deps-2.json -swift-version 5 -I %t/new

// RUN: %FileCheck %s --check-prefix=TEXTUAL --input-file=%t/deps-2.json

// RUN: env SWIFT_DEBUG_FORCE_SWIFTMODULE_REVISION=1.0.0.0.1 \
// RUN:   %target-swift-frontend -scan-dependencies -module-name Test -O -module-load-mode prefer-binary \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   %t/main.swift -o %t/deps-3.json -swift-version 5 -I %t/revision

// RUN: %FileCheck %s --check-prefix=BINARY --input-file=%t/deps-3.json

// BINARY:      "directDependencies": [
// BINARY-NEXT:   {
// BINARY-NEXT:     "swiftPrebuiltExternal": "Foo"
// BINARY-NEXT:   }
// BINARY-NEXT: ]

// TEXTUAL:      "directDependencies": [
// TEXTUAL-NEXT:   {
// TEXTUAL-NEXT:     "swift": "Foo"
// TEXTUAL-NEXT:   }
// TEXTUAL-NEXT: ]

//--- foo.swift
public func foo() {}

//--- main.swift
import Foo
