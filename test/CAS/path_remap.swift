// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend -scan-dependencies -module-name Test -module-cache-path %t/clang-module-cache -O \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   %t/main.swift -o %t/deps.json -swift-version 5 -cache-compile-job -cas-path %t/cas -I %t/include \
// RUN:   -scanner-prefix-map %swift_src_root=/^src -scanner-prefix-map %t=/^tmp

// RUN: %{python} %S/Inputs/BuildCommandExtractor.py %t/deps.json A > %t/A.cmd
// RUN: %swift_frontend_plain @%t/A.cmd

// RUN: %{python} %S/Inputs/GenerateExplicitModuleMap.py %t/deps.json > %t/map.json
// RUN: llvm-cas --cas %t/cas --make-blob --data %t/map.json > %t/map.casid
// RUN: %{python} %S/Inputs/BuildCommandExtractor.py %t/deps.json Test > %t/MyApp.cmd

// RUN: %target-swift-frontend \
// RUN:   -c -o %t/main.o -cache-compile-job -cas-path %t/cas \
// RUN:   -swift-version 5 -disable-implicit-swift-modules \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   -module-name Test -explicit-swift-module-map-file @%t/map.casid \
// RUN:   -cache-replay-prefix-map /^src=%swift_src_root -cache-replay-prefix-map /^tmp=%t \
// RUN:   /^tmp/main.swift @%t/MyApp.cmd

// RUN: %swift-scan-test -action compute_cache_key_from_index -cas-path %t/cas -input 0 -- \
// RUN:   %target-swift-frontend \
// RUN:   -c -o %t/main.o -cache-compile-job -cas-path %t/cas \
// RUN:   -swift-version 5 -disable-implicit-swift-modules \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   -module-name Test -explicit-swift-module-map-file @%t/map.casid \
// RUN:   -cache-replay-prefix-map /^src=%swift_src_root -cache-replay-prefix-map /^tmp=%t \
// RUN:   /^tmp/main.swift @%t/MyApp.cmd > %t/key.casid

// RUN: %swift-scan-test -action replay_result -cas-path %t/cas -id @%t/key.casid -- \
// RUN:   %target-swift-frontend \
// RUN:   -c -o %t/main.o -cache-compile-job -cas-path %t/cas \
// RUN:   -swift-version 5 -disable-implicit-swift-modules \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib \
// RUN:   -module-name Test -explicit-swift-module-map-file @%t/map.casid \
// RUN:   -cache-replay-prefix-map /^src=%swift_src_root -cache-replay-prefix-map /^tmp=%t \
// RUN:   /^tmp/main.swift @%t/MyApp.cmd

//--- main.swift
import A

#warning("This is a warning")

//--- include/A.swiftinterface
// swift-interface-format-version: 1.0
// swift-module-flags: -module-name A -O -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import -parse-stdlib -user-module-version 1.0
public func b() { }

