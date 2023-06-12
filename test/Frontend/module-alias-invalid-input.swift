// Tests for invalid module alias format and values.

// RUN: not %target-swift-frontend -emit-silgen -parse-as-library %s -module-name foo -module-alias foo=bar 2>&1 | %FileCheck -check-prefix=INVALID_MODULE_ALIAS %s
// INVALID_MODULE_ALIAS: error: invalid module alias "foo"; make sure the alias differs from the module name, module ABI name, module link name, and a standard library name

// RUN: not %target-swift-frontend -emit-silgen -parse-as-library %s -module-name foo -module-alias Swift=Bar 2>&1 | %FileCheck -check-prefix=INVALID_MODULE_ALIAS1 %s
// INVALID_MODULE_ALIAS1: error: invalid module alias "Swift"; make sure the alias differs from the module name, module ABI name, module link name, and a standard library name
// RUN: %target-swift-frontend -emit-silgen -parse-as-library %s -module-name foo -module-alias Bar=Swift -verify

// RUN: not %target-swift-frontend -emit-silgen -parse-as-library %s -module-name foo -module-alias bar=bar 2>&1 | %FileCheck -check-prefix=INVALID_MODULE_ALIAS2 %s
// INVALID_MODULE_ALIAS2: error: duplicate module alias; the name "bar" is already used for an alias or a real name

// RUN: not %target-swift-frontend -emit-silgen -parse-as-library %s -module-name foo -module-alias bar=baz -module-alias baz=cat 2>&1 | %FileCheck -check-prefix=INVALID_MODULE_ALIAS3 %s
// INVALID_MODULE_ALIAS3: error: duplicate module alias; the name "baz" is already used for an alias or a real name

// RUN: not %target-swift-frontend -emit-silgen -parse-as-library %s -module-name foo -module-alias bar 2>&1 | %FileCheck -check-prefix=INVALID_MODULE_ALIAS4 %s
// INVALID_MODULE_ALIAS4: error: invalid module alias format "bar"; make sure to use the format '-module-alias alias_name=real_name'

// RUN: not %target-swift-frontend -emit-silgen -parse-as-library %s -module-name foo -module-alias bar=c-a.t 2>&1 | %FileCheck -check-prefix=INVALID_MODULE_NAME %s
// INVALID_MODULE_NAME: error: module name "c-a.t" is not a valid identifier

// These should succeed.
// RUN: %target-swift-frontend -emit-silgen %s > /dev/null
// RUN: %target-swift-frontend -emit-silgen -parse-as-library -module-name foo %s -module-alias bar=cat > /dev/null
// RUN: %target-swift-frontend -typecheck -parse-as-library -module-name foo %s -module-alias bar=cat

public class Logger {
  public init() {}
  public func startLogging() {}
}
