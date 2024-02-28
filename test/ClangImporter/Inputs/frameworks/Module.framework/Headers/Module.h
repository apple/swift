
#ifndef MODULE_H
#define MODULE_H
const char *getModuleVersion(void);

@interface Module
+(const char *)version; // retrieve module version
+alloc;
@end

@protocol ModuleProto @end

#define MODULE_H_MACRO 1
#__private_macro MODULE_H_MACRO

#include <Module/Sub.h>
#include <Module/Buried/Treasure.h>

__asm("foo");

#endif // MODULE_H
