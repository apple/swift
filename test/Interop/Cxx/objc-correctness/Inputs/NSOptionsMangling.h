#define __CF_OPTIONS_ATTRIBUTES __attribute__((flag_enum,enum_extensibility(open)))
#if (__cplusplus)
#define CF_OPTIONS(_type, _name) __attribute__((availability(swift,unavailable))) _type _name; enum __CF_OPTIONS_ATTRIBUTES : _name
#else
#define CF_OPTIONS(_type, _name) enum __CF_OPTIONS_ATTRIBUTES _name : _type _name; enum _name : _type
#endif

typedef CF_OPTIONS(unsigned, UIControlState) { UIControlStateNormal = 0 };

#ifdef __cplusplus
#define UIKIT_EXTERN extern "C" __attribute__((visibility ("default")))
#else
#define UIKIT_EXTERN extern __attribute__((visibility ("default")))
#endif

@interface UIView
@end

UIKIT_EXTERN
@interface UIControl : UIView
@end

