//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift open source project
//
// Copyright (c) 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_PLUGINSERVER_PLUGINSERVER_H
#define SWIFT_PLUGINSERVER_PLUGINSERVER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

//===----------------------------------------------------------------------===//
// Inter-process communication.
//===----------------------------------------------------------------------===//

/// Create an IPC communication handle.
const void *PluginServer_createConnection(const char **errorMessage);

/// Destroy an IPC communication handle created by
/// 'PluginServer_createConnection'.
void PluginServer_destroyConnection(const void *connHandle);

/// Read bytes from the IPC communication handle.
ptrdiff_t PluginServer_read(const void *connHandle, void *data, size_t nbyte);

/// Write bytes to the IPC communication handle.
ptrdiff_t PluginServer_write(const void *connHandle, const void *data,
                             size_t nbyte);

#ifdef __cplusplus
}
#endif

#endif /* SWIFT_PLUGINSERVER_PLUGINSERVER_H */
