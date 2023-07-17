// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/cas
// RUN: split-file %s %t
// RUN: %target-swift-emit-pcm -module-cache-path %t/clang-module-cache -module-name SwiftShims %swift-lib-dir/swift/shims/module.modulemap -o %t/SwiftShims.pcm
// RUN: %target-swift-emit-pcm -module-cache-path %t/clang-module-cache -module-name _SwiftConcurrencyShims %swift-lib-dir/swift/shims/module.modulemap -o %t/_SwiftConcurrencyShims.pcm
// RUN: %target-swift-frontend -emit-module -module-cache-path %t/clang-module-cache %t/A.swift -o %t/A.swiftmodule -swift-version 5
// RUN: %target-swift-frontend -emit-module -module-cache-path %t/clang-module-cache %t/B.swift -o %t/B.swiftmodule -I %t -swift-version 5
// RUN: %target-swift-frontend -scan-dependencies -module-cache-path %t/clang-module-cache %t/Test.swift -o %t/deps.json -I %t -swift-version 5 -cache-compile-job -cas-path %t/cas
// RUN: %validate-json %t/deps.json &>/dev/null

// RUN: %S/Inputs/SwiftDepsExtractor.py %t/deps.json swiftPrebuiltExternal:A moduleCacheKey | tr -d '\n' > %t/A.key
// RUN: %S/Inputs/SwiftDepsExtractor.py %t/deps.json swiftPrebuiltExternal:B moduleCacheKey | tr -d '\n' > %t/B.key

/// Prepare the cas objects that can be used to construct CompileJobResultSchema object.
// RUN: llvm-cas --cas %t/cas --get-cache-result @%t/A.key > %t/A.result
// RUN: llvm-cas --cas %t/cas --ls-node-refs @%t/A.result | tail -n 1 > %t/schema.casid
// RUN: llvm-cas --cas %t/cas --cat-blob @%t/A.result > %t/kind.blob

/// Make keys for module loads. The result casid construction is tied with the actual structure of CompilerJobResultSchema.
// RUN: llvm-cas --cas %t/cas --make-blob --data %stdlib_module | tr -d '\n' > %t/Swift.key
// RUN: llvm-cas --cas %t/cas --make-node --data %t/kind.blob @%t/Swift.key @%t/schema.casid  > %t/Swift.casid
// RUN: llvm-cas --cas %t/cas --put-cache-key @%t/Swift.key @%t/Swift.casid

// RUN: llvm-cas --cas %t/cas --make-blob --data %ononesupport_module | tr -d '\n' > %t/ONone.key
// RUN: llvm-cas --cas %t/cas --make-node --data %t/kind.blob @%t/ONone.key @%t/schema.casid > %t/ONone.casid
// RUN: llvm-cas --cas %t/cas --put-cache-key @%t/ONone.key @%t/ONone.casid

// RUN: llvm-cas --cas %t/cas --make-blob --data %concurrency_module | tr -d '\n' > %t/Concurrency.key
// RUN: llvm-cas --cas %t/cas --make-node --data %t/kind.blob @%t/Concurrency.key @%t/schema.casid > %t/Concurrency.casid
// RUN: llvm-cas --cas %t/cas --put-cache-key @%t/Concurrency.key @%t/Concurrency.casid

// RUN: llvm-cas --cas %t/cas --make-blob --data %string_processing_module | tr -d '\n' > %t/String.key
// RUN: llvm-cas --cas %t/cas --make-node --data %t/kind.blob @%t/String.key @%t/schema.casid > %t/String.casid
// RUN: llvm-cas --cas %t/cas --put-cache-key @%t/String.key @%t/String.casid

// RUN: llvm-cas --cas %t/cas --make-blob --data %t/SwiftShims.pcm | tr -d '\n' > %t/Shims.key
// RUN: llvm-cas --cas %t/cas --make-node --data %t/kind.blob @%t/Shims.key @%t/schema.casid > %t/Shims.casid
// RUN: llvm-cas --cas %t/cas --put-cache-key @%t/Shims.key @%t/Shims.casid

// RUN: llvm-cas --cas %t/cas --make-blob --data %t/_SwiftConcurrencyShims.pcm | tr -d '\n' > %t/ConcurrencyShims.key
// RUN: llvm-cas --cas %t/cas --make-node --data %t/kind.blob @%t/ConcurrencyShims.key @%t/schema.casid > %t/ConcurrencyShims.casid
// RUN: llvm-cas --cas %t/cas --put-cache-key @%t/ConcurrencyShims.key @%t/ConcurrencyShims.casid

// RUN: echo "[{" > %/t/map.json
// RUN: echo "\"moduleName\": \"A\"," >> %/t/map.json
// RUN: echo "\"modulePath\": \"A.swiftmodule\"," >> %/t/map.json
// RUN: echo -n "\"moduleCacheKey\": " >> %/t/map.json
// RUN: cat %t/A.key >> %/t/map.json
// RUN: echo "," >> %/t/map.json
// RUN: echo "\"isFramework\": false" >> %/t/map.json
// RUN: echo "}," >> %/t/map.json
// RUN: echo "{" >> %/t/map.json
// RUN: echo "\"moduleName\": \"B\"," >> %/t/map.json
// RUN: echo "\"modulePath\": \"B.swiftmodule\"," >> %/t/map.json
// RUN: echo -n "\"moduleCacheKey\": " >> %/t/map.json
// RUN: cat %t/B.key >> %/t/map.json
// RUN: echo "," >> %/t/map.json
// RUN: echo "\"isFramework\": false" >> %/t/map.json
// RUN: echo "}," >> %/t/map.json
// RUN: echo "{" >> %/t/map.json
// RUN: echo "\"moduleName\": \"Swift\"," >> %/t/map.json
// RUN: echo "\"modulePath\": \"Swift.swiftmodule\"," >> %/t/map.json
// RUN: echo -n "\"moduleCacheKey\": \"" >> %/t/map.json
// RUN: cat %t/Swift.key >> %/t/map.json
// RUN: echo "\"," >> %/t/map.json
// RUN: echo "\"isFramework\": false" >> %/t/map.json
// RUN: echo "}," >> %/t/map.json
// RUN: echo "{" >> %/t/map.json
// RUN: echo "\"moduleName\": \"SwiftOnoneSupport\"," >> %/t/map.json
// RUN: echo "\"modulePath\": \"SwiftOnoneSupport.swiftmodule\"," >> %/t/map.json
// RUN: echo -n "\"moduleCacheKey\": \"" >> %/t/map.json
// RUN: cat %t/ONone.key >> %/t/map.json
// RUN: echo "\"," >> %/t/map.json
// RUN: echo "\"isFramework\": false" >> %/t/map.json
// RUN: echo "}," >> %/t/map.json
// RUN: echo "{" >> %/t/map.json
// RUN: echo "\"moduleName\": \"_Concurrency\"," >> %/t/map.json
// RUN: echo "\"modulePath\": \"_Concurrency.swiftmodule\"," >> %/t/map.json
// RUN: echo -n "\"moduleCacheKey\": \"" >> %/t/map.json
// RUN: cat %t/Concurrency.key >> %/t/map.json
// RUN: echo "\"," >> %/t/map.json
// RUN: echo "\"isFramework\": false" >> %/t/map.json
// RUN: echo "}," >> %/t/map.json
// RUN: echo "{" >> %/t/map.json
// RUN: echo "\"moduleName\": \"_StringProcessing\"," >> %/t/map.json
// RUN: echo "\"modulePath\": \"_StringProcessing.swiftmodule\"," >> %/t/map.json
// RUN: echo -n "\"moduleCacheKey\": \"" >> %/t/map.json
// RUN: cat %t/String.key >> %/t/map.json
// RUN: echo "\"," >> %/t/map.json
// RUN: echo "\"isFramework\": false" >> %/t/map.json
// RUN: echo "}," >> %/t/map.json
// RUN: echo "{" >> %/t/map.json
// RUN: echo "\"moduleName\": \"SwiftShims\"," >> %/t/map.json
// RUN: echo "\"clangModulePath\": \"SwiftShims.pcm\"," >> %/t/map.json
// RUN: echo -n "\"clangModuleCacheKey\": \"" >> %/t/map.json
// RUN: cat %t/Shims.key >> %/t/map.json
// RUN: echo "\"," >> %/t/map.json
// RUN: echo "\"isFramework\": false" >> %/t/map.json
// RUN: echo "}," >> %/t/map.json
// RUN: echo "{" >> %/t/map.json
// RUN: echo "\"moduleName\": \"_SwiftConcurrencyShims\"," >> %/t/map.json
// RUN: echo "\"clangModulePath\": \"_SwiftConcurrency.pcm\"," >> %/t/map.json
// RUN: echo -n "\"clangModuleCacheKey\": \"" >> %/t/map.json
// RUN: cat %t/ConcurrencyShims.key >> %/t/map.json
// RUN: echo "\"," >> %/t/map.json
// RUN: echo "\"isFramework\": false" >> %/t/map.json
// RUN: echo "}]" >> %/t/map.json

// RUN: llvm-cas --cas %t/cas --make-blob --data %t/map.json > %t/map.casid

// RUN: %target-swift-frontend -emit-module -emit-module-path %t/Foo.swiftmodule -disable-implicit-swift-modules -module-cache-path %t.module-cache -explicit-swift-module-map-file @%t/map.casid -Rmodule-loading -Xcc -Rmodule-import %s -cache-compile-job -cas-path %t/cas -allow-unstable-cache-key-for-testing 2>&1 | %FileCheck %s

// CHECK-DAG: loaded module 'A'
// CHECK-DAG: loaded module 'B'
// CHECK-DAG: loaded module 'Swift'
// CHECK-DAG: loaded module '_StringProcessing'
// CHECK-DAG: loaded module '_Concurrency'
// CHECK-DAG: loaded module 'SwiftOnoneSupport'

//--- A.swift
func test() {}

//--- B.swift
import A
func myTest() {}

//--- Test.swift
import B


