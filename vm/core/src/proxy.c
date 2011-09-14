#include <nullvm.h>
#include <unwind.h>
#include "private.h"
#include "uthash.h"
#include "utlist.h"

typedef struct LookupEntry {
    void* lookup;
    ProxyMethod* method;
    UT_hash_handle hh;
} LookupEntry;

typedef struct ProxyClassData {
    ProxyHandler handler;
    LookupEntry* lookupsHash;
} ProxyClassData;

typedef struct ProxyClassList {
    struct ProxyClassList* next;
    Class* clazz;
} ProxyClassList;

ProxyClassList* proxyClasses = NULL;

static jboolean hasMethod(Env* env, Class* clazz, char* name, char* desc) {
    Method* method;
    for (method = clazz->methods->first; method != NULL; method = method->next) {
        if (!strcmp(method->name, name) && !strcmp(method->desc, desc)) {
            return TRUE;
        }
    }
    return FALSE;
}

static jboolean addProxyMethods(Env* env, Class* proxyClass, Class* clazz, ProxyClassData* proxyClassData) {
    // Add constructors from the super class and override all overridable methods. Constructors will use 
    // the same impl as the superclass. Overridden methods will have _nvmProxy0 as its impl.

    if (clazz->superclass) {
        if (!addProxyMethods(env, proxyClass, clazz->superclass, proxyClassData)) return FALSE;
    }

    Method* method;
    for (method = clazz->methods->first; method != NULL; method = method->next) {
        if (!METHOD_IS_STATIC(method) && !METHOD_IS_PRIVATE(method) && !METHOD_IS_FINAL(method) 
                && (!METHOD_IS_CONSTRUCTOR(method) || clazz == proxyClass->superclass)) {

            void* impl = NULL;
            jint access = method->access & (~ACC_ABSTRACT);
            if (METHOD_IS_CONSTRUCTOR(method)) {
                impl = method->impl;
                // TODO: For now we make all constructors public to satisfy java.lang.reflect.Proxy. 
                access = ACC_PUBLIC;
            } else {
                impl = _proxy0;
            }
            if (METHOD_IS_CONSTRUCTOR(method) || !hasMethod(env, proxyClass, method->name, method->desc)) { 
                ProxyMethod* proxyMethod = addProxyMethod(env, proxyClass, method, access, impl);
                if (!proxyMethod) return FALSE;
                if (!METHOD_IS_CONSTRUCTOR(method)) {
                    // Record the lookup function in proxyClassData
                    LookupEntry* entry = nvmAllocateMemory(env, sizeof(LookupEntry));
                    if (!entry) return FALSE;
                    entry->lookup = proxyMethod->method.lookup;
                    entry->method = proxyMethod;
                    HASH_ADD_PTR(proxyClassData->lookupsHash, lookup, entry);
                }
            }
        }
    }

    return TRUE;
}

/**
 * Implements all abstract methods in {@code proxyClass}. This should be called after
 * {@link #addProxyMethods()} which will override all methods defined by the proxy's
 * ancestor classes (abstract or concrete).
 */
static jboolean implementAbstractInterfaceMethods(Env* env, Class* proxyClass, Interface* interface, ProxyClassData* proxyClassData) {
    if (!interface) return TRUE;

    Method* method;
    for (method = interface->interface->methods->first; method != NULL; method = method->next) {
        if (!hasMethod(env, proxyClass, method->name, method->desc)) { 
            jint access = method->access & (~ACC_ABSTRACT);
            ProxyMethod* proxyMethod = addProxyMethod(env, proxyClass, method, access, _proxy0);
            if (!proxyMethod) return FALSE;
            // Record the lookup function in proxyClassData
            LookupEntry* entry = nvmAllocateMemory(env, sizeof(LookupEntry));
            if (!entry) return FALSE;
            entry->lookup = proxyMethod->method.lookup;
            entry->method = proxyMethod;
            HASH_ADD_PTR(proxyClassData->lookupsHash, lookup, entry);
        }
    }

    if (!implementAbstractInterfaceMethods(env, proxyClass, interface->next, proxyClassData)) return FALSE;
    if (!implementAbstractInterfaceMethods(env, proxyClass, interface->interface->interfaces, proxyClassData)) return FALSE;

    return TRUE;
}

Method* lookupProxiedMethod(void* lookup) {
    ProxyClassList* listEntry;
    LL_FOREACH(proxyClasses, listEntry) {
        Class* proxyClass = listEntry->clazz;
        ProxyClassData* proxyClassData = (ProxyClassData*) proxyClass->data;
        LookupEntry* entry;
        HASH_FIND_PTR(proxyClassData->lookupsHash, &lookup, entry);
        if (entry) return (Method*) entry->method;
    }

    return NULL;
}

Class* nvmProxyCreateProxyClass(Env* env, Class* superclass, ClassLoader* classLoader, char* className, jint interfacesCount, Class** interfaces, jint instanceDataSize, ProxyHandler handler) {

    // Allocate the proxy class.
    Class* proxyClass = nvmAllocateClass(env, className, superclass, classLoader, ACC_PUBLIC | ACC_FINAL, sizeof(ProxyClassData), instanceDataSize);
    if (!proxyClass) return NULL;

    ProxyClassData* proxyClassData = (ProxyClassData*) proxyClass->data;
    proxyClassData->handler = handler;

    // Add interfaces
    jint i;
    for (i = 0; i < interfacesCount; i++) {
        if (!nvmAddInterface(env, proxyClass, (Class*) interfaces[i])) return NULL;
    }

    if (!addProxyMethods(env, proxyClass, superclass, proxyClassData)) return NULL;

    Class* c = proxyClass;
    while (c) {
        if (!implementAbstractInterfaceMethods(env, proxyClass, c->interfaces, proxyClassData)) return NULL;
        c = c->superclass;
    }

    ProxyClassList* listEntry = nvmAllocateMemory(env, sizeof(ProxyClassList));
    if (!listEntry) return NULL;

    if (!nvmRegisterClass(env, proxyClass)) return NULL;

    listEntry->clazz = proxyClass;
    LL_APPEND(proxyClasses, listEntry);

    return proxyClass;
}

void _nvmProxyHandler(CallInfo* callInfo) {
    Env* env = (Env*) proxy0NextPtr(callInfo);
    Object* receiver = (Object*) proxy0NextPtr(callInfo);
    Class* proxyClass = receiver->clazz;
    ProxyClassData* proxyClassData = (ProxyClassData*) proxyClass->data;

    void* lookup = _Unwind_FindEnclosingFunction(callInfo->returnAddress);
    LookupEntry* entry;

    HASH_FIND_PTR(proxyClassData->lookupsHash, &lookup, entry);

    if (!entry) {
        nvmAbort("Failed to determine which method was called on proxy class. proxyClass=%s, lookup=%p\n", proxyClass->name, lookup);
    }

    ProxyMethod* method = entry->method;

    jint argsCount = nvmGetParameterCount((Method*) method);
    jvalue *jvalueArgs = NULL;
    if (argsCount > 0) {
        jvalueArgs = (jvalue*) nvmAllocateMemory(env, sizeof(jvalue) * argsCount);
        if (!jvalueArgs) return;

        char* desc = method->method.desc;
        char* c;
        jint i = 0;
        while (c = nvmGetNextParameterType(&desc)) {
            switch (c[0]) {
            case 'B':
                jvalueArgs[i++].b = (jbyte) proxy0NextInt(callInfo);
                break;
            case 'Z':
                jvalueArgs[i++].z = (jboolean) proxy0NextInt(callInfo);
                break;
            case 'S':
                jvalueArgs[i++].s = (jshort) proxy0NextInt(callInfo);
                break;
            case 'C':
                jvalueArgs[i++].c = (jchar) proxy0NextInt(callInfo);
                break;
            case 'I':
                jvalueArgs[i++].i = proxy0NextInt(callInfo);
                break;
            case 'J':
                jvalueArgs[i++].j = proxy0NextLong(callInfo);
                break;
            case 'F':
                jvalueArgs[i++].f = proxy0NextFloat(callInfo);
                break;
            case 'D':
                jvalueArgs[i++].d = proxy0NextDouble(callInfo);
                break;
            case '[':
            case 'L':
                jvalueArgs[i++].l = (jobject) proxy0NextPtr(callInfo);
                break;
            }
        }
    }

    jvalue returnValue;
    proxyClassData->handler(env, receiver, method, jvalueArgs, &returnValue);

    if (nvmExceptionCheck(env)) {
        nvmRaiseException(env, nvmExceptionOccurred(env));
    }

    proxy0ReturnInt(callInfo, 0);
    switch (nvmGetReturnType(method->method.desc)[0]) {
    case 'B':
        proxy0ReturnInt(callInfo, (jint) returnValue.b);
        break;
    case 'Z':
        proxy0ReturnInt(callInfo, (jint) returnValue.z);
        break;
    case 'S':
        proxy0ReturnInt(callInfo, (jint) returnValue.s);
        break;
    case 'C':
        proxy0ReturnInt(callInfo, (jint) returnValue.c);
        break;
    case 'I':
        proxy0ReturnInt(callInfo, returnValue.i);
        break;
    case 'J':
        proxy0ReturnLong(callInfo, returnValue.j);
        break;
    case 'F':
        proxy0ReturnFloat(callInfo, returnValue.f);
        break;
    case 'D':
        proxy0ReturnDouble(callInfo, returnValue.d);
        break;
    case '[':
    case 'L':
        proxy0ReturnPtr(callInfo, returnValue.l);
        break;
    }
}

