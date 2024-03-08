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

#include "PluginServer.h"

#if defined(_WIN32)
#include <stdio.h>
#include <io.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#include <unistd.h>
#endif

#include <errno.h>
#include <string.h>

namespace {
struct ConnectionHandle {
  int inputFD;
  int outputFD;

  ConnectionHandle(int inputFD, int outputFD)
      : inputFD(inputFD), outputFD(outputFD) {}
};
} // namespace

const void *PluginServer_createConnection(const char **errorMessage) {
#if defined(_WIN32)
  struct unique_fd {
    unique_fd(int fd) : fd_(fd) {}
    unique_fd(const unique_fd &) = delete;
    unique_fd &operator=(const unique_fd &) = delete;
    unique_fd &operator=(unique_fd &&) = delete;
    unique_fd(unique_fd &&uf) : fd_(uf.fd_) { uf.fd_ = -1; }
    ~unique_fd() { if (fd_ > 0) _close(fd_); }

    int operator*() const { return fd_; }
    int release() { int fd = fd_; fd_ = -1; return fd; }

  private:
    int fd_;
  };

  unique_fd ifd{_dup(_fileno(stdin))};
  if (*ifd < 0) {
    *errorMessage = _strerror(nullptr);
    return nullptr;
  }

  if (_close(_fileno(stdin)) < 0) {
    *errorMessage = _strerror(nullptr);
    return nullptr;
  }

  unique_fd ofd{_dup(_fileno(stdout))};
  if (*ofd < 0) {
    *errorMessage = _strerror(nullptr);
    return nullptr;
  }

  if (_dup2(_fileno(stderr), _fileno(stdout)) < 0) {
    *errorMessage = _strerror(nullptr);
    return nullptr;
  }

  return new ConnectionHandle(ifd.release(), ofd.release());
#else
  // Duplicate the `stdin` file descriptor, which we will then use for
  // receiving messages from the plugin host.
  auto inputFD = dup(STDIN_FILENO);
  if (inputFD < 0) {
    *errorMessage = strerror(errno);
    return nullptr;
  }

  // Having duplicated the original standard-input descriptor, we close
  // `stdin` so that attempts by the plugin to read console input (which
  // are usually a mistake) return errors instead of blocking.
  if (close(STDIN_FILENO) < 0) {
    *errorMessage = strerror(errno);
    return nullptr;
  }

  // Duplicate the `stdout` file descriptor, which we will then use for
  // sending messages to the plugin host.
  auto outputFD = dup(STDOUT_FILENO);
  if (outputFD < 0) {
    *errorMessage = strerror(errno);
    return nullptr;
  }

  // Having duplicated the original standard-output descriptor, redirect
  // `stdout` to `stderr` so that all free-form text output goes there.
  if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
    *errorMessage = strerror(errno);
    return nullptr;
  }

  // Open a message channel for communicating with the plugin host.
  return new ConnectionHandle(inputFD, outputFD);
#endif
}

void PluginServer_destroyConnection(const void *server) {
  delete static_cast<const ConnectionHandle *>(server);
}

ptrdiff_t PluginServer_read(const void *server, void *data, size_t nbyte) {
  const auto *connection = static_cast<const ConnectionHandle *>(server);
#if defined(_WIN32)
  return _read(connection->inputFD, data, nbyte);
#else
  return ::read(connection->inputFD, data, nbyte);
#endif
}

ptrdiff_t PluginServer_write(const void *server, const void *data,
                             size_t nbyte) {
  const auto *connection = static_cast<const ConnectionHandle *>(server);
#if defined(_WIN32)
  return _write(connection->outputFD, data, nbyte);
#else
  return ::write(connection->outputFD, data, nbyte);
#endif
}
