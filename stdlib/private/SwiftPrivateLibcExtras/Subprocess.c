//===--- Subprocess.c - Subprocess Stubs ----------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

// posix_spawn is not available on Android, HAIKU, WASI or Windows (MSVC).
#if !defined(__ANDROID__) && !defined(__HAIKU__) && (!defined(_WIN32) || defined(__CYGWIN__)) && !defined(__wasi__)

#include "swift/Runtime/Config.h"

// NOTE: preprocess away the availability information to allow use of
// unsupported APIs on certain targets (i.e. tvOS)
#define availability(...)
#include <spawn.h>
#include <sys/types.h>

SWIFT_CC
int _stdlib_posix_spawn_file_actions_init(
    posix_spawn_file_actions_t *file_actions) {
  return posix_spawn_file_actions_init(file_actions);
}

SWIFT_CC
int _stdlib_posix_spawn_file_actions_destroy(
    posix_spawn_file_actions_t *file_actions) {
  return posix_spawn_file_actions_destroy(file_actions);
}

SWIFT_CC
int _stdlib_posix_spawn_file_actions_addclose(
    posix_spawn_file_actions_t *file_actions, int filedes) {
  return posix_spawn_file_actions_addclose(file_actions, filedes);
}

SWIFT_CC
int _stdlib_posix_spawn_file_actions_adddup2(
    posix_spawn_file_actions_t *file_actions, int filedes, int newfiledes) {
  return posix_spawn_file_actions_adddup2(file_actions, filedes, newfiledes);
}

SWIFT_CC
int _stdlib_posix_spawn(pid_t *__restrict pid, const char * __restrict path,
                      const posix_spawn_file_actions_t *file_actions,
                      const posix_spawnattr_t *__restrict attrp,
                      char *const argv[__restrict],
                      char *const envp[__restrict]) {
  return posix_spawn(pid, path, file_actions, attrp, argv, envp);
}

#endif // !defined(__ANDROID__) && !defined(__HAIKU__) && (!defined(_WIN32) || defined(__CGYWIN__)) && !defined(__wasi__)

