//===--- sourcekitd.h - -----------------------------------------*- C++ -*-===//
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

#ifndef LLVM_SOURCEKITD_SOURCEKITD_H
#define LLVM_SOURCEKITD_SOURCEKITD_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/// The version constants for the sourcekitd API.
/// SOURCEKITD_VERSION_MINOR should increase when there are API additions.
/// SOURCEKITD_VERSION_MAJOR is intended for "major" source/ABI breaking
/// changes.
///
/// The policy about the sourcekitd API is to keep it source and ABI compatible,
/// thus SOURCEKITD_VERSION_MAJOR is expected to remain stable.
#define SOURCEKITD_VERSION_MAJOR 0
#define SOURCEKITD_VERSION_MINOR 4

#define SOURCEKITD_VERSION_ENCODE(major, minor) ( \
      ((major) * 10000)                           \
    + ((minor) *     1))

#define SOURCEKITD_VERSION SOURCEKITD_VERSION_ENCODE( \
    SOURCEKITD_VERSION_MAJOR,                         \
    SOURCEKITD_VERSION_MINOR)

#define SOURCEKITD_VERSION_STRINGIZE_(major, minor)   \
    #major"."#minor
#define SOURCEKITD_VERSION_STRINGIZE(major, minor)    \
    SOURCEKITD_VERSION_STRINGIZE_(major, minor)

#define SOURCEKITD_VERSION_STRING SOURCEKITD_VERSION_STRINGIZE( \
    SOURCEKITD_VERSION_MAJOR,                                   \
    SOURCEKITD_VERSION_MINOR)

#ifdef  __cplusplus
# define SOURCEKITD_BEGIN_DECLS  extern "C" {
# define SOURCEKITD_END_DECLS    }
#else
# define SOURCEKITD_BEGIN_DECLS
# define SOURCEKITD_END_DECLS
#endif

#if defined(_WIN32)
# if defined(sourcekitd_EXPORTS)
#   define SOURCEKITD_PUBLIC __declspec(dllexport)
# else
#   define SOURCEKITD_PUBLIC __declspec(dllimport)
# endif
#else
# define SOURCEKITD_PUBLIC
#endif

#ifndef __has_feature
# define __has_feature(x) 0
#endif

#if __has_feature(blocks)
# define SOURCEKITD_HAS_BLOCKS 1
#else
# define SOURCEKITD_HAS_BLOCKS 0
#endif

#if defined(__clang__) || defined(__GNUC__)
# define SOURCEKITD_WARN_RESULT __attribute__((__warn_unused_result__))
# define SOURCEKITD_NONNULL1 __attribute__((__nonnull__(1)))
# define SOURCEKITD_NONNULL2 __attribute__((__nonnull__(2)))
# define SOURCEKITD_NONNULL3 __attribute__((__nonnull__(3)))
# define SOURCEKITD_NONNULL_ALL __attribute__((__nonnull__))
# define SOURCEKITD_DEPRECATED(m) __attribute__((deprecated(m)))
#else
# define SOURCEKITD_WARN_RESULT
# define SOURCEKITD_NONNULL1
# define SOURCEKITD_NONNULL2
# define SOURCEKITD_NONNULL3
# define SOURCEKITD_NONNULL_ALL
#endif

SOURCEKITD_BEGIN_DECLS

/// Initializes structures needed across the rest of the sourcekitd API.
///
/// Must be called before any other sourcekitd call.
/// Can be called multiple times as long as it is matched with a
/// \c sourcekitd_shutdown call.
/// Calling \c sourcekitd_initialize a second time without an intervening
/// \c sourcekitd_shutdown is undefined.
/// \c sourcekitd_initialize does not need to be called again even if the
/// service crashes.
SOURCEKITD_PUBLIC
void
sourcekitd_initialize(void);

/// Deallocates structures needed across the rest of the sourcekitd API.
///
/// If there are response handlers still waiting for a response, they will
/// receive a SOURCEKITD_ERROR_REQUEST_CANCELLED response.
///
/// Calling \c sourcekitd_shutdown without a matching \c sourcekitd_initialize
/// is undefined.
SOURCEKITD_PUBLIC
void
sourcekitd_shutdown(void);

#if SOURCEKITD_HAS_BLOCKS

typedef void(^sourcekitd_interrupted_connection_handler_t)(void);

/// Sets the handler which should be called whenever the connection to
/// SourceKit is interrupted.
///
/// The handler should reestablish any necessary state, such as re-opening any
/// documents which were open before the connection was interrupted.
///
/// It is not necessary to call \c sourcekitd_initialize; the connection will
/// automatically be reestablished when sending the next request.
///
/// \param handler Interrupted connection handler to use. Pass NULL to remove
/// the handler.
SOURCEKITD_PUBLIC SOURCEKITD_NONNULL_ALL
void
sourcekitd_set_interrupted_connection_handler(
                           sourcekitd_interrupted_connection_handler_t handler);

#endif

/// A "unique identifier" utilized by the request/response protocol.
///
/// A \c sourcekitd_uid_t object is associated with a string and is uniqued for
/// the lifetime of the process. Its usefulness is in providing an "infinite
/// namespace" of identifiers.
/// A \c sourcekitd_uid_t object remains valid even if the service crashes.
typedef struct sourcekitd_uid_s *sourcekitd_uid_t;

/// Create a \c sourcekitd_uid_t from a C string.
SOURCEKITD_PUBLIC SOURCEKITD_NONNULL1 SOURCEKITD_WARN_RESULT
sourcekitd_uid_t
sourcekitd_uid_get_from_cstr(const char *string);

/// Create a \c sourcekitd_uid_t from a string buffer.
SOURCEKITD_PUBLIC SOURCEKITD_NONNULL1 SOURCEKITD_WARN_RESULT
sourcekitd_uid_t
sourcekitd_uid_get_from_buf(const char *buf, size_t length);

/// Get the length of the string associated with a \c sourcekitd_uid_t.
SOURCEKITD_PUBLIC SOURCEKITD_NONNULL1 SOURCEKITD_WARN_RESULT
size_t
sourcekitd_uid_get_length(sourcekitd_uid_t obj);

/// Get the C string pointer associated with a \c sourcekitd_uid_t.
SOURCEKITD_PUBLIC SOURCEKITD_NONNULL1 SOURCEKITD_WARN_RESULT
const char *
sourcekitd_uid_get_string_ptr(sourcekitd_uid_t obj);

/// \defgroup Request API
///
/// @{
///
/// Used for constructing a request object.
typedef void *sourcekitd_object_t;

SOURCEKITD_PUBLIC SOURCEKITD_NONNULL1
sourcekitd_object_t
sourcekitd_request_retain(sourcekitd_object_t object);

SOURCEKITD_PUBLIC SOURCEKITD_NONNULL1
void
sourcekitd_request_release(sourcekitd_object_t object);

SOURCEKITD_PUBLIC SOURCEKITD_WARN_RESULT
sourcekitd_object_t
sourcekitd_request_dictionary_create(const sourcekitd_uid_t *keys,
                                     const sourcekitd_object_t *values,
                                     size_t count);

SOURCEKITD_PUBLIC SOURCEKITD_NONNULL_ALL
void
sourcekitd_request_dictionary_set_value(sourcekitd_object_t dict,
                                        sourcekitd_uid_t key,
                                        sourcekitd_object_t value);

SOURCEKITD_PUBLIC SOURCEKITD_NONNULL_ALL
void
sourcekitd_request_dictionary_set_string(sourcekitd_object_t dict,
                                         sourcekitd_uid_t key,
                                         const char *string);

SOURCEKITD_PUBLIC SOURCEKITD_NONNULL_ALL
void
sourcekitd_request_dictionary_set_stringbuf(sourcekitd_object_t dict,
                                            sourcekitd_uid_t key,
                                            const char *buf, size_t length);

SOURCEKITD_PUBLIC SOURCEKITD_NONNULL1 SOURCEKITD_NONNULL2
void
sourcekitd_request_dictionary_set_int64(sourcekitd_object_t dict,
                                        sourcekitd_uid_t key, int64_t val);

SOURCEKITD_PUBLIC SOURCEKITD_NONNULL_ALL
void
sourcekitd_request_dictionary_set_uid(sourcekitd_object_t dict,
                                      sourcekitd_uid_t key,
                                      sourcekitd_uid_t uid);

#define SOURCEKITD_ARRAY_APPEND ((size_t)(-1))

SOURCEKITD_PUBLIC SOURCEKITD_WARN_RESULT
sourcekitd_object_t
sourcekitd_request_array_create(const sourcekitd_object_t *objects,
                                size_t count);

SOURCEKITD_PUBLIC SOURCEKITD_NONNULL1 SOURCEKITD_NONNULL3
void
sourcekitd_request_array_set_value(sourcekitd_object_t array, size_t index,
                                   sourcekitd_object_t value);

SOURCEKITD_PUBLIC SOURCEKITD_NONNULL1 SOURCEKITD_NONNULL3
void
sourcekitd_request_array_set_string(sourcekitd_object_t array, size_t index,
                                    const char *string);

SOURCEKITD_PUBLIC SOURCEKITD_NONNULL1 SOURCEKITD_NONNULL3
void
sourcekitd_request_array_set_stringbuf(sourcekitd_object_t array, size_t index,
                                       const char *buf, size_t length);

SOURCEKITD_PUBLIC SOURCEKITD_NONNULL1
void
sourcekitd_request_array_set_int64(sourcekitd_object_t array, size_t index,
                                   int64_t val);

SOURCEKITD_PUBLIC SOURCEKITD_NONNULL1 SOURCEKITD_NONNULL3
void
sourcekitd_request_array_set_uid(sourcekitd_object_t array, size_t index,
                                 sourcekitd_uid_t uid);

SOURCEKITD_PUBLIC SOURCEKITD_WARN_RESULT
sourcekitd_object_t
sourcekitd_request_int64_create(int64_t val);

SOURCEKITD_PUBLIC SOURCEKITD_NONNULL1 SOURCEKITD_WARN_RESULT
sourcekitd_object_t
sourcekitd_request_string_create(const char *string);

SOURCEKITD_PUBLIC SOURCEKITD_NONNULL1 SOURCEKITD_WARN_RESULT
sourcekitd_object_t
sourcekitd_request_uid_create(sourcekitd_uid_t uid);

/// Creates a request object by parsing the provided string in YAML
/// format.
///
/// \param yaml The string in YAML format.
///
/// \param error A pointer to store a C string of the error description if
/// parsing fails. This string should be disposed of with \c free when done.
/// Can be NULL.
///
/// \returns A sourcekitd_object_t instance or NULL if parsing fails.
SOURCEKITD_PUBLIC SOURCEKITD_NONNULL1 SOURCEKITD_WARN_RESULT
sourcekitd_object_t
sourcekitd_request_create_from_yaml(const char *yaml, char **error);

/// Prints to stderr a string representation of the request object in
/// YAML format.
SOURCEKITD_PUBLIC SOURCEKITD_NONNULL1
void
sourcekitd_request_description_dump(sourcekitd_object_t obj);

/// Copies a string representation of the request object in YAML format.
/// \returns A string representation of the request object. This string should
/// be disposed of with \c free when done.
SOURCEKITD_PUBLIC SOURCEKITD_NONNULL1
char *
sourcekitd_request_description_copy(sourcekitd_object_t obj);

/// @}

/// \defgroup Response API
///
/// @{

/// The result of a request.
///
/// If the request failed \c sourcekitd_response_t will be an error response and
/// will contain information about the error, otherwise it will contain the
/// resulting values of the request.
typedef void *sourcekitd_response_t;

/// A value of the response object.
///
/// Its lifetime is tied to the sourcekitd_response_t object that it came from.
typedef struct {
  uint64_t data[3];
} sourcekitd_variant_t;

typedef enum {
  SOURCEKITD_VARIANT_TYPE_NULL = 0,
  SOURCEKITD_VARIANT_TYPE_DICTIONARY = 1,
  SOURCEKITD_VARIANT_TYPE_ARRAY = 2,
  SOURCEKITD_VARIANT_TYPE_INT64 = 3,
  SOURCEKITD_VARIANT_TYPE_STRING = 4,
  SOURCEKITD_VARIANT_TYPE_UID = 5,
  SOURCEKITD_VARIANT_TYPE_BOOL = 6,
  // Reserved for future addition
  // SOURCEKITD_VARIANT_TYPE_DOUBLE = 7,
  SOURCEKITD_VARIANT_TYPE_DATA = 8,
} sourcekitd_variant_type_t;

typedef enum {
  SOURCEKITD_ERROR_CONNECTION_INTERRUPTED = 1,
  SOURCEKITD_ERROR_REQUEST_INVALID = 2,
  SOURCEKITD_ERROR_REQUEST_FAILED = 3,
  SOURCEKITD_ERROR_REQUEST_CANCELLED = 4
} sourcekitd_error_t;

SOURCEKITD_PUBLIC SOURCEKITD_NONNULL1
void
sourcekitd_response_dispose(sourcekitd_response_t obj);

/// Returns true if the given response is an error.
SOURCEKITD_PUBLIC SOURCEKITD_NONNULL_ALL SOURCEKITD_WARN_RESULT
bool
sourcekitd_response_is_error(sourcekitd_response_t obj);

/// Returns the error kind given a response error.
///
/// Passing a response object that is not an error will result in undefined
/// behavior.
SOURCEKITD_PUBLIC SOURCEKITD_NONNULL_ALL SOURCEKITD_WARN_RESULT
sourcekitd_error_t
sourcekitd_response_error_get_kind(sourcekitd_response_t err);

/// Returns a C string of the error description.
///
/// Passing a response object that is not an error will result in undefined
/// behavior.
SOURCEKITD_PUBLIC SOURCEKITD_NONNULL_ALL SOURCEKITD_WARN_RESULT
const char *
sourcekitd_response_error_get_description(sourcekitd_response_t err);

/// Returns the value contained in the response.
///
/// If the response is an error it will return a null variant.
SOURCEKITD_PUBLIC SOURCEKITD_NONNULL1 SOURCEKITD_WARN_RESULT
sourcekitd_variant_t
sourcekitd_response_get_value(sourcekitd_response_t resp);

SOURCEKITD_PUBLIC SOURCEKITD_WARN_RESULT
sourcekitd_variant_type_t
sourcekitd_variant_get_type(sourcekitd_variant_t obj);

SOURCEKITD_PUBLIC SOURCEKITD_NONNULL2 SOURCEKITD_WARN_RESULT
sourcekitd_variant_t
sourcekitd_variant_dictionary_get_value(sourcekitd_variant_t dict,
                                        sourcekitd_uid_t key);

/// The underlying C string for the specified key. NULL if the value for the
/// specified key is not a C string value or if there is no value for the
/// specified key.
SOURCEKITD_PUBLIC SOURCEKITD_NONNULL_ALL SOURCEKITD_WARN_RESULT
const char *
sourcekitd_variant_dictionary_get_string(sourcekitd_variant_t dict,
                                         sourcekitd_uid_t key);

/// The underlying \c int64 value for the specified key. 0 if the
/// value for the specified key is not an integer value or if there is no
/// value for the specified key.
SOURCEKITD_PUBLIC SOURCEKITD_NONNULL_ALL SOURCEKITD_WARN_RESULT
int64_t
sourcekitd_variant_dictionary_get_int64(sourcekitd_variant_t dict,
                                        sourcekitd_uid_t key);

/// The underlying \c bool value for the specified key. false if the
/// value for the specified key is not a Boolean value or if there is no
/// value for the specified key.
SOURCEKITD_PUBLIC SOURCEKITD_NONNULL_ALL SOURCEKITD_WARN_RESULT
bool
sourcekitd_variant_dictionary_get_bool(sourcekitd_variant_t dict,
                                       sourcekitd_uid_t key);

/// The underlying \c sourcekitd_uid_t value for the specified key. NULL if the
/// value for the specified key is not a uid value or if there is no
/// value for the specified key.
SOURCEKITD_PUBLIC SOURCEKITD_NONNULL_ALL SOURCEKITD_WARN_RESULT
sourcekitd_uid_t
sourcekitd_variant_dictionary_get_uid(sourcekitd_variant_t dict,
                                      sourcekitd_uid_t key);

#if SOURCEKITD_HAS_BLOCKS
/// A block to be invoked for every key/value pair in the dictionary.
///
/// \param key The current key in the iteration.
///
/// \param value The current value in the iteration.
///
/// \returns true to indicate that iteration should continue.
typedef bool (^sourcekitd_variant_dictionary_applier_t)(sourcekitd_uid_t key,
                                                    sourcekitd_variant_t value);

/// Invokes the given block for every key/value pair in the dictionary.
///
/// \returns true to indicate that iteration of the dictionary completed
/// successfully. Iteration will only fail if the applier block returns false.
SOURCEKITD_PUBLIC SOURCEKITD_NONNULL_ALL
bool
sourcekitd_variant_dictionary_apply(sourcekitd_variant_t dict,
                               sourcekitd_variant_dictionary_applier_t applier);
#endif

/// A function to be invoked for every key/value pair in the dictionary.
///
/// \param key The current key in the iteration.
///
/// \param value The current value in the iteration.
///
/// \returns true to indicate that iteration should continue.
typedef bool (*sourcekitd_variant_dictionary_applier_f_t)(sourcekitd_uid_t key,
                                                    sourcekitd_variant_t value,
                                                    void *context);

/// Invokes the given function for every key/value pair in the
/// dictionary.
///
/// \returns true to indicate that iteration of the dictionary completed
/// successfully. Iteration will only fail if the applier block returns 0.
SOURCEKITD_PUBLIC SOURCEKITD_NONNULL2
bool
sourcekitd_variant_dictionary_apply_f(sourcekitd_variant_t dict,
                              sourcekitd_variant_dictionary_applier_f_t applier,
                              void *context);

SOURCEKITD_PUBLIC SOURCEKITD_WARN_RESULT
size_t
sourcekitd_variant_array_get_count(sourcekitd_variant_t array);

SOURCEKITD_PUBLIC SOURCEKITD_WARN_RESULT
sourcekitd_variant_t
sourcekitd_variant_array_get_value(sourcekitd_variant_t array, size_t index);

SOURCEKITD_PUBLIC SOURCEKITD_WARN_RESULT
const char *
sourcekitd_variant_array_get_string(sourcekitd_variant_t array, size_t index);

SOURCEKITD_PUBLIC SOURCEKITD_WARN_RESULT
int64_t
sourcekitd_variant_array_get_int64(sourcekitd_variant_t array, size_t index);

SOURCEKITD_PUBLIC SOURCEKITD_WARN_RESULT
bool
sourcekitd_variant_array_get_bool(sourcekitd_variant_t array, size_t index);

SOURCEKITD_PUBLIC SOURCEKITD_WARN_RESULT
sourcekitd_uid_t
sourcekitd_variant_array_get_uid(sourcekitd_variant_t array, size_t index);

#if SOURCEKITD_HAS_BLOCKS
/// A block to be invoked for every value in the array.
///
/// \param index The current index in the iteration.
///
/// \param value The current value in the iteration.
///
/// \returns true to indicate that iteration should continue.
typedef bool (^sourcekitd_variant_array_applier_t)(size_t index,
                                                   sourcekitd_variant_t value);

/// Invokes the given block for every value in the array.
///
/// \returns true to indicate that iteration of the array completed
/// successfully. Iteration will only fail if the applier block returns false.
SOURCEKITD_PUBLIC SOURCEKITD_NONNULL_ALL
bool
sourcekitd_variant_array_apply(sourcekitd_variant_t array,
                               sourcekitd_variant_array_applier_t applier);
#endif

/// A function to be invoked for every value in the array.
///
/// \param index The current index in the iteration.
///
/// \param value The current value in the iteration.
///
/// \returns true to indicate that iteration should continue.
typedef bool (*sourcekitd_variant_array_applier_f_t)(size_t index,
                                                     sourcekitd_variant_t value,
                                                     void *context);

/// Invokes the given function for every value in the array.
///
/// \returns true to indicate that iteration of the array completed
/// successfully. Iteration will only fail if the applier block returns false.
SOURCEKITD_PUBLIC SOURCEKITD_NONNULL2
bool
sourcekitd_variant_array_apply_f(sourcekitd_variant_t array,
                                 sourcekitd_variant_array_applier_f_t applier,
                                 void *context);

SOURCEKITD_PUBLIC SOURCEKITD_WARN_RESULT
int64_t
sourcekitd_variant_int64_get_value(sourcekitd_variant_t obj);

SOURCEKITD_PUBLIC SOURCEKITD_WARN_RESULT
bool
sourcekitd_variant_bool_get_value(sourcekitd_variant_t obj);

SOURCEKITD_PUBLIC SOURCEKITD_WARN_RESULT
size_t
sourcekitd_variant_string_get_length(sourcekitd_variant_t obj);

SOURCEKITD_PUBLIC SOURCEKITD_WARN_RESULT
const char *
sourcekitd_variant_string_get_ptr(sourcekitd_variant_t obj);

SOURCEKITD_PUBLIC SOURCEKITD_WARN_RESULT
size_t
sourcekitd_variant_data_get_size(sourcekitd_variant_t obj);

SOURCEKITD_PUBLIC SOURCEKITD_WARN_RESULT
const void *
sourcekitd_variant_data_get_ptr(sourcekitd_variant_t obj);

SOURCEKITD_PUBLIC SOURCEKITD_WARN_RESULT
sourcekitd_uid_t
sourcekitd_variant_uid_get_value(sourcekitd_variant_t obj);

/// Prints to stderr a string representation of the response object in
/// YAML format.
SOURCEKITD_PUBLIC SOURCEKITD_NONNULL1
void
sourcekitd_response_description_dump(sourcekitd_response_t resp);

/// Prints to the given file descriptor a string representation of the
/// response object.
SOURCEKITD_PUBLIC SOURCEKITD_NONNULL1
void
sourcekitd_response_description_dump_filedesc(sourcekitd_response_t resp,
                                              int fd);

/// Copies a string representation of the response object in YAML format.
/// \returns A string representation of the response object. This string should
/// be disposed of with \c free when done.
SOURCEKITD_PUBLIC SOURCEKITD_NONNULL1
char *
sourcekitd_response_description_copy(sourcekitd_response_t resp);

/// Prints to stderr a string representation of the variant object in
/// YAML format.
SOURCEKITD_PUBLIC
void
sourcekitd_variant_description_dump(sourcekitd_variant_t obj);

/// Prints to the given file descriptor a string representation of the
/// variant object.
SOURCEKITD_PUBLIC
void
sourcekitd_variant_description_dump_filedesc(sourcekitd_variant_t obj, int fd);

/// Copies a string representation of the variant object in YAML format.
/// \returns A string representation of the variant object. This string should
/// be disposed of with \c free when done.
SOURCEKITD_PUBLIC
char *
sourcekitd_variant_description_copy(sourcekitd_variant_t obj);

/// Copies a string representation of the variant object in JSON format.
/// \returns A string representation of the variant object. This string should
/// be disposed of with \c free when done.
SOURCEKITD_PUBLIC
char *
sourcekitd_variant_json_description_copy(sourcekitd_variant_t obj);

/// @}

/// Invoke a request synchronously.
///
/// The caller accepts ownership of the returned sourcekitd_response_t object
/// and should invoke \c sourcekitd_response_dispose on it when it is done with
/// it.
SOURCEKITD_PUBLIC SOURCEKITD_NONNULL_ALL SOURCEKITD_WARN_RESULT
sourcekitd_response_t
sourcekitd_send_request_sync(sourcekitd_object_t req);

/// Used to cancel a request that has been invoked asynchronously.
typedef const void *sourcekitd_request_handle_t;

#if SOURCEKITD_HAS_BLOCKS
/// Receives the response of an asynchronous request or notification.
///
/// The receiver accepts ownership of the response object and should invoke
/// \c sourcekitd_response_dispose on it when it is done with it.
typedef void (^sourcekitd_response_receiver_t)(sourcekitd_response_t resp);

/// Invoke a request asynchronously.
///
/// \param req the request object.
///
/// \param out_handle the address where the associated
/// \c sourcekitd_request_handle_t will be stored. Can be NULL.
///
/// \param receiver the block that will receive the response object.
SOURCEKITD_PUBLIC SOURCEKITD_NONNULL1
void
sourcekitd_send_request(sourcekitd_object_t req,
                        sourcekitd_request_handle_t *out_handle,
                        sourcekitd_response_receiver_t receiver);
#endif

/// Cancel a request using the associated request handle returned by
/// \c sourcekitd_send_request.
///
/// It is not guaranteed that invoking \c sourcekitd_cancel_request will cancel
/// the request. If the request gets cancelled, the receiver will get a
/// \c SOURCEKITD_ERROR_REQUEST_CANCELLED response error.
///
/// Calling \c sourcekitd_cancel_request after the response object has been
/// delivered will have no effect.
SOURCEKITD_PUBLIC
void
sourcekitd_cancel_request(sourcekitd_request_handle_t handle);

/// Dispose a request handle returned by \c sourcekitd_send_request.
SOURCEKITD_PUBLIC
void sourcekitd_request_handle_dispose(sourcekitd_request_handle_t handle);

#if SOURCEKITD_HAS_BLOCKS

/// Sets the handler which should be called to receive notifications.
/// The block will be set to be executed in the main thread queue.
///
/// If the connection to SourceKit is interrupted the handler will receive an
/// error response object of kind \c SOURCEKITD_ERROR_CONNECTION_INTERRUPTED.
/// Any subsequent requests will immediately fail with the same error until
/// the service is restored.
/// When the service is restored the handler will receive an empty response
/// object.
///
/// \param receiver Notification handler block to use. Pass NULL to remove the
/// previous handler that was set.
SOURCEKITD_PUBLIC
void
sourcekitd_set_notification_handler(sourcekitd_response_receiver_t receiver);

typedef sourcekitd_uid_t(^sourcekitd_uid_handler_t)(const char* uidStr);

SOURCEKITD_PUBLIC SOURCEKITD_DEPRECATED("use sourcekitd_set_uid_handlers")
void sourcekitd_set_uid_handler(sourcekitd_uid_handler_t handler);

typedef sourcekitd_uid_t(^sourcekitd_uid_from_str_handler_t)(const char* uidStr);
typedef const char *(^sourcekitd_str_from_uid_handler_t)(sourcekitd_uid_t uid);

SOURCEKITD_PUBLIC
void
sourcekitd_set_uid_handlers(sourcekitd_uid_from_str_handler_t uid_from_str,
                            sourcekitd_str_from_uid_handler_t str_from_uid);

#endif

SOURCEKITD_END_DECLS

#endif
