// RUN: env SWIFT_FORCE_MODULE_LOADING=prefer-interface %host_swift_compiler -typecheck %s -resource-dir %swift-lib-dir/swift %mcp_opt

// REQUIRES: VENDOR=apple

import Swift
