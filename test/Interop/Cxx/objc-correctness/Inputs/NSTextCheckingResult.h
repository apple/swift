#define __CF_OPTIONS_ATTRIBUTES __attribute__((flag_enum,enum_extensibility(open)))
#if (__cplusplus)
#define CF_OPTIONS(_type, _name) __attribute__((availability(swift,unavailable))) _type _name; enum __CF_OPTIONS_ATTRIBUTES : _name
#else
#define CF_OPTIONS(_type, _name) enum __CF_OPTIONS_ATTRIBUTES _name : _type _name; enum _name : _type
#endif

typedef CF_OPTIONS(unsigned int, NSTextCheckingType) {
  NSTextCheckingTypeOrthography           = 1ULL << 0,
  NSTextCheckingTypeSpelling              = 1ULL << 1,
  // ...
};
