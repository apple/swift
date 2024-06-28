// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend -swift-version 6 -I %t %s -emit-sil -o /dev/null -verify -parse-as-library

// REQUIRES: OS=macosx

import Foundation
@preconcurrency import Darwin

func mach_task_self() -> mach_port_t {
    return mach_task_self_
}
