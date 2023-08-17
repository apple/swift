#include "CFOptions.h"

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

