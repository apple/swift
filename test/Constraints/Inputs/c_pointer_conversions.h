@import Foundation;

#include "stdint.h"

void char_ptr_func(char *  _Nonnull buffer);
void const_char_ptr_func(const char *  _Nonnull buffer);

void opt_char_ptr_func(char * _Nullable buffer);
void const_opt_char_ptr_func(const char * _Nullable buffer);

void unsigned_char_ptr_func(unsigned char * _Nonnull buffer);
void const_unsigned_char_ptr_func(const unsigned char * _Nonnull buffer);

void opt_unsigned_char_ptr_func(char * _Nullable buffer);
void const_opt_unsigned_char_ptr_func(const char * _Nullable buffer);

void int_16_ptr_func(int16_t * _Nonnull buffer);
void int_32_ptr_func(int32_t * _Nonnull buffer);
void int_64_ptr_func(int64_t * _Nonnull buffer);

void opt_int_16_ptr_func(int16_t * _Nullable buffer);
void opt_int_32_ptr_func(int32_t * _Nullable buffer);
void opt_int_64_ptr_func(int64_t * _Nullable buffer);

void const_int_16_ptr_func(const int16_t * _Nonnull buffer);
void const_int_32_ptr_func(const int32_t * _Nonnull buffer);
void const_int_64_ptr_func(const int64_t * _Nonnull buffer);

void const_opt_int_16_ptr_func(const int16_t * _Nullable buffer);
void const_opt_int_32_ptr_func(const int32_t * _Nullable buffer);
void const_opt_int_64_ptr_func(const int64_t * _Nullable buffer);

void uint_16_ptr_func(uint16_t * _Nonnull buffer);
void uint_32_ptr_func(uint32_t * _Nonnull buffer);
void uint_64_ptr_func(uint64_t * _Nonnull buffer);

void opt_uint_16_ptr_func(uint16_t * _Nullable buffer);
void opt_uint_32_ptr_func(uint32_t * _Nullable buffer);
void opt_uint_64_ptr_func(uint64_t * _Nullable buffer);

void const_uint_16_ptr_func(const uint16_t * _Nonnull buffer);
void const_uint_32_ptr_func(const uint32_t * _Nonnull buffer);
void const_uint_64_ptr_func(const uint64_t * _Nonnull buffer);

void const_opt_uint_16_ptr_func(const uint16_t * _Nullable buffer);
void const_opt_uint_32_ptr_func(const uint32_t * _Nullable buffer);
void const_opt_uint_64_ptr_func(const uint64_t * _Nullable buffer);
