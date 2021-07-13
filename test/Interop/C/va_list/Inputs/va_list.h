#include <stdarg.h>

typedef va_list __gnuc_va_list;
typedef va_list __isoc_va_list;
typedef va_list __va_list;

va_list va;
__gnuc_va_list gnu;
__isoc_va_list isoC;
__va_list underscore;
