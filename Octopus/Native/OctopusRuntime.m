#import "OctopusRuntime.h"
#include <dlfcn.h>
#include <string.h>
#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#endif
static Method ownMethod(Class cls, SEL selector) {
    unsigned count=0; Method found=NULL; Method *methods=class_copyMethodList(cls,&count);
    for(unsigned i=0;i<count;i++)if(sel_isEqual(method_getName(methods[i]),selector)){found=methods[i];break;}
    free(methods);return found;
}
BOOL OCTRuntimeCheck(const char *className,const char *selector,const char *encoding,NSString **reason) {
    Class cls=objc_getClass(className);Method method=cls?ownMethod(cls,sel_registerName(selector)):NULL;
    if(!method || strcmp(method_getTypeEncoding(method),encoding)) {
        if(reason)*reason=[NSString stringWithFormat:@"ABI mismatch %s %s: expected %s, found %s",className,selector,encoding,method?method_getTypeEncoding(method):"missing"];
        return NO;
    }
#ifndef OCT_NATIVE_TEST
    Dl_info info={0};
    IMP implementation=method_getImplementation(method);
    const void *address=(const void*)implementation;
#if __has_feature(ptrauth_calls)
    address=ptrauth_strip(address,ptrauth_key_function_pointer);
#endif
    if(!dladdr(address,&info)||!info.dli_fname||strcmp(info.dli_fname,"/usr/libexec/ptpd")) {
        if(reason)*reason=[NSString stringWithFormat:@"Non-original implementation %s %s",className,selector];return NO;
    }
#endif
    return YES;
}
BOOL OCTRuntimeHook(const char *className,const char *selector,const char *encoding,IMP replacement,IMP *original,NSString **reason) {
    if(!OCTRuntimeCheck(className,selector,encoding,reason))return NO;
    Method method=ownMethod(objc_getClass(className),sel_registerName(selector));
    *original=method_getImplementation(method); // initialized before any thread can enter replacement
    IMP replaced=method_setImplementation(method,replacement);
    if(replaced!=*original){*original=replaced;if(reason)*reason=@"Concurrent method change: integration remains inactive";return NO;}
    return YES;
}
