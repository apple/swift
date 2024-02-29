// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/clang-module-cache

// RUN: %target-swift-frontend -scan-dependencies -module-load-mode prefer-interface -module-cache-path %t/clang-module-cache %s -o %t/deps.json -module-name Test

// RUN: %target-swift-frontend -scan-dependencies -module-load-mode prefer-interface -module-cache-path %t/clang-module-cache %s -o %t/deps_cache.json -module-name Test \
// RUN:    -cache-compile-job -cas-path %t/cas

// RUN: %{python} %S/Inputs/SwiftDepsExtractor.py %t/deps.json Swift modulePath > %t/path1
// RUN: %{python} %S/Inputs/SwiftDepsExtractor.py %t/deps_cache.json Swift modulePath > %t/path2
// RUN: not diff %t/path1 %t/path2

func test() {}
