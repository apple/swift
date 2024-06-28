//===--- UUID.cpp - UUID generation ---------------------------------------===//
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
//
// This is an interface over the standard OSF uuid library that gives UUIDs
// sound value semantics and operators.
//
//===----------------------------------------------------------------------===//

#include "swift/Basic/Assertions.h"
#include "swift/Basic/UUID.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"

// WIN32 doesn't natively support <uuid/uuid.h>. Instead, we use Win32 APIs.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <objbase.h>
#include <string>
#include <algorithm>
#else
#include <uuid/uuid.h>
#endif

using namespace swift;

swift::UUID::UUID(FromRandom_t) {
#if defined(_WIN32)
  ::UUID uuid;
  ::CoCreateGuid(&uuid);

  memcpy(Value, &uuid, Size);
#else
  uuid_generate_random(Value);
#endif
}

swift::UUID::UUID(FromTime_t) {
#if defined(_WIN32)
  ::UUID uuid;
  ::CoCreateGuid(&uuid);

  memcpy(Value, &uuid, Size);
#else
  uuid_generate_time(Value);
#endif
}

swift::UUID::UUID() {
#if defined(_WIN32)
  ::UUID uuid = *((::UUID *)&Value);
  UuidCreateNil(&uuid);

  memcpy(Value, &uuid, Size);
#else
  uuid_clear(Value);
#endif
}

std::optional<swift::UUID> swift::UUID::fromString(const char *s) {
#if defined(_WIN32)
  RPC_CSTR t = const_cast<RPC_CSTR>(reinterpret_cast<const unsigned char*>(s));

  ::UUID uuid;
  RPC_STATUS status = UuidFromStringA(t, &uuid);
  if (status == RPC_S_INVALID_STRING_UUID) {
    return std::nullopt;
  }

  swift::UUID result = UUID();
  memcpy(result.Value, &uuid, Size);
  return result;
#else
  swift::UUID result;
  if (uuid_parse(s, result.Value))
    return std::nullopt;
  return result;
#endif
}

void swift::UUID::toString(llvm::SmallVectorImpl<char> &out) const {
  out.resize(UUID::StringBufferSize);
#if defined(_WIN32)
  ::UUID uuid;
  memcpy(&uuid, Value, Size);

  RPC_CSTR str;
  UuidToStringA(&uuid, &str);

  char* signedStr = reinterpret_cast<char*>(str);
  memcpy(out.data(), signedStr, StringBufferSize);
  llvm::transform(out, std::begin(out), toupper);
#else
  uuid_unparse_upper(Value, out.data());
#endif
  // Pop off the null terminator.
  assert(out.back() == '\0' && "did not null-terminate?!");
  out.pop_back();
}

int swift::UUID::compare(UUID y) const {
#if defined(_WIN32)
  RPC_STATUS s;
  ::UUID uuid1;
  memcpy(&uuid1, Value, Size);

  ::UUID uuid2;
  memcpy(&uuid2, y.Value, Size);

  return UuidCompare(&uuid1, &uuid2, &s);
#else
  return uuid_compare(Value, y.Value);
#endif
}

llvm::raw_ostream &swift::operator<<(llvm::raw_ostream &os, UUID uuid) {
  llvm::SmallString<UUID::StringBufferSize> buf;
  uuid.toString(buf);
  os << buf;
  return os;
}
