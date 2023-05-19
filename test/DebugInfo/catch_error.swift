// REQUIRES: objc_interop
// RUN: %target-swift-frontend -emit-sil -Xllvm -sil-print-debuginfo %s \
// RUN:  -parse-as-library | %FileCheck %s
import Foundation

open class Cache<T> {
  let _negativeCache: NSMutableDictionary = NSMutableDictionary()
  func cachedValue(creationBlock: () throws -> T) throws -> T {
    do {
      let value = try creationBlock()
      return value
    } catch {
      // CHECK:   debug_value {{.*}} : $any Error, let, name "error", implicit, loc "{{.*}}":[[@LINE-1]]:13, scope [[SCOPE:[0-9]+]]
      // CHECK: alloc_stack $@opened({{.*}}, any Error) Self, loc{{.*}}, scope [[SCOPE]]

      _negativeCache.setObject(error, forKey: NSNumber(1))
      throw error
    }
  }
}
