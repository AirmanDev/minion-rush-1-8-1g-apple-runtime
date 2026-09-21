
#include "jni_bridge.h"
#include "shim_libc.h"
#include "host_time.h"
#include "localization.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>

// JNINativeInterface table size in JNI 1.6.
#define JNI_SLOTS 233

// Supported table slots.
enum {
    S_GetVersion = 4,
    S_FindClass = 6,
    S_ExceptionOccurred = 15,
    S_ExceptionDescribe = 16,
    S_ExceptionClear = 17,
    S_PushLocalFrame = 19,
    S_PopLocalFrame = 20,
    S_NewGlobalRef = 21,
    S_DeleteGlobalRef = 22,
    S_DeleteLocalRef = 23,
    S_IsSameObject = 24,
    S_NewLocalRef = 25,
    S_EnsureLocalCapacity = 26,
    S_GetObjectClass = 31,
    S_GetMethodID = 33,
    S_CallObjectMethod = 34,
    S_CallBooleanMethod = 37,
    S_CallBooleanMethodV = 38,
    S_CallBooleanMethodA = 39,
    S_CallIntMethod = 49,
    S_CallIntMethodV = 50,
    S_CallIntMethodA = 51,
    S_CallFloatMethod = 55,
    S_CallFloatMethodV = 56,
    S_CallFloatMethodA = 57,
    S_CallVoidMethod = 61,
    S_CallVoidMethodV = 62,
    S_CallVoidMethodA = 63,
    S_GetFieldID = 94,
    S_CallObjectMethodV = 35,
    S_CallObjectMethodA = 36,
    S_GetStaticMethodID = 113,
    S_CallStaticObjectMethod = 114,
    S_CallStaticObjectMethodV = 115,
    S_CallStaticObjectMethodA = 116,
    S_CallStaticBooleanMethod = 117,
    S_CallStaticBooleanMethodV = 118,
    S_CallStaticBooleanMethodA = 119,
    S_CallStaticIntMethod = 129,
    S_CallStaticIntMethodV = 130,
    S_CallStaticIntMethodA = 131,
    S_CallStaticFloatMethod = 135,
    S_CallStaticFloatMethodV = 136,
    S_CallStaticFloatMethodA = 137,
    S_CallStaticVoidMethod = 141,
    S_CallStaticVoidMethodV = 142,
    S_CallStaticVoidMethodA = 143,
    S_GetStaticFieldID = 144,
    S_GetStaticObjectField = 145,
    S_NewStringUTF = 167,
    S_GetStringUTFLength = 168,
    S_GetStringUTFChars = 169,
    S_ReleaseStringUTFChars = 170,
    S_GetArrayLength = 171,
    S_NewByteArray = 176,
    S_NewIntArray = 179,
    S_GetIntArrayElements = 187,
    S_ReleaseIntArrayElements = 195,
    S_GetByteArrayElements = 184,
    S_ReleaseByteArrayElements = 192,
    S_RegisterNatives = 215,
    S_MonitorEnter = 217,
    S_MonitorExit = 218,
    S_GetJavaVM = 219,
    S_ExceptionCheck = 228,
};

// Registries.

#define MAX_NAMES 512

typedef struct {
    char *cls, *name, *sig;
} jmethod;

static char *CLASSES[MAX_NAMES];
static int CLASS_COUNT;
static jmethod METHODS[MAX_NAMES];
static int METHOD_COUNT;

static uint32_t ENV_PTR, VM_PTR, TABLE_BASE;
static uint32_t SCRATCH, SCRATCH_END, SCRATCH_NEXT;
static int LOG_UNKNOWN;

#define H_CLASS(i) (0x4C000000u + (uint32_t)(i))
#define H_METHOD(i) (0x4D000000u + (uint32_t)(i))

static uint32_t scratch_alloc(uint32_t n) {
    if (n > UINT32_MAX - 7u) return 0;
    n = (n + 7u) & ~7u;
    if (n > SCRATCH_END - SCRATCH) return 0;
    if (SCRATCH_NEXT + n > SCRATCH_END) SCRATCH_NEXT = SCRATCH;
    uint32_t p = SCRATCH_NEXT;
    SCRATCH_NEXT += n;
    return p;
}

static int intern_class(const char *name) {
    for (int i = 0; i < CLASS_COUNT; i++)
        if (strcmp(CLASSES[i], name) == 0) return i;
    if (CLASS_COUNT >= MAX_NAMES) return -1;
    char *copy = strdup(name);
    if (!copy) return -1;
    CLASSES[CLASS_COUNT] = copy;
    return CLASS_COUNT++;
}

static int intern_method(const char *cls, const char *name, const char *sig) {
    for (int i = 0; i < METHOD_COUNT; i++)
        if (strcmp(METHODS[i].cls, cls ? cls : "?") == 0 && strcmp(METHODS[i].name, name) == 0 &&
            strcmp(METHODS[i].sig, sig) == 0)
            return i;
    if (METHOD_COUNT >= MAX_NAMES) return -1;
    char *class_copy = strdup(cls ? cls : "?");
    char *name_copy = strdup(name);
    char *signature_copy = strdup(sig);
    if (!class_copy || !name_copy || !signature_copy) {
        free(class_copy);
        free(name_copy);
        free(signature_copy);
        return -1;
    }
    METHODS[METHOD_COUNT] = (jmethod){class_copy, name_copy, signature_copy};
    return METHOD_COUNT++;
}

// Arguments.

// cpu.h provides the shared ARM32 argument reader.
static const char *gstr(mr_cpu *c, uint32_t a) {
    return a ? mr_guest_cstr(c, a) : "";
}

#define A(i) mr_guest_arg32(c, (unsigned)(i))
#define RET(x) (c->r[0] = (uint32_t)(x))

#define ANDROID_FILES MR_GUEST_ROOT

static char GPU_NAME[128] = "Apple GPU";

void mr_jni_set_gpu_name(const char *s) {
    if (s && *s) snprintf(GPU_NAME, sizeof(GPU_NAME), "%s", s);
}

// Java exposes both clock values as decimal millisecond strings.
static const char *clock_string(char *buffer, size_t size, long long ms) {
    snprintf(buffer, size, "%lld", ms);
    return buffer;
}

// Java counterparts: Game.java (GetPhone*) and SUtils.java (getSDFolder).
static const char *device_string(const char *n) {
    if (strcmp(n, "getSDFolder") == 0) return ANDROID_FILES;
    if (strcmp(n, "getSaveFolder") == 0) return ANDROID_FILES;
    if (strcmp(n, "getUserFolder") == 0) return ANDROID_FILES;
    if (strcmp(n, "GetPhoneManufacturer") == 0) return "Apple";
    if (strcmp(n, "GetPhoneModel") == 0) return "Mac";
    if (strcmp(n, "GetPhoneCPUName") == 0) return "Apple Silicon";
    if (strcmp(n, "GetPhoneGPUName") == 0) return GPU_NAME;
    if (strcmp(n, "GetDeviceFirmware") == 0) return "10";
    if (strcmp(n, "GetPhoneLanguage") == 0) return mr_localization_engine_query_code();
    if (strcmp(n, "MyGetPhoneLanguage") == 0) return mr_localization_engine_query_code();
    if (strcmp(n, "getDeviceLanguage") == 0) return mr_localization_engine_query_code();
    if (strcmp(n, "GetPhoneCountry") == 0) return mr_localization_country_code();
    if (strcmp(n, "GetPhoneRegion") == 0) return mr_localization_country_code();
    if (strcmp(n, "getSimcardCountry") == 0) return mr_localization_country_code();
    if (strcmp(n, "getDeviceIdentifier") == 0) return "0123456789abcdef";
    if (strcmp(n, "getMac") == 0) return "00:00:00:00:00:00";

    if (strcmp(n, "getDeviceName") == 0) return "Mac";
    if (strcmp(n, "getDeviceFirmware") == 0) return "10";
    if (strcmp(n, "getDeviceCountry") == 0) return mr_localization_country_code();
    if (strcmp(n, "getDeviceCarrier") == 0) return "none";
    if (strcmp(n, "getIdentifier") == 0) return "0123456789abcdef";

    if (strcmp(n, "GetDeviceBootTime") == 0) {
        static char boot_time[32];
        struct timeval boot;
        size_t size = sizeof boot;
        if (sysctlbyname("kern.boottime", &boot, &size, NULL, 0) != 0) return "0";
        return clock_string(boot_time, sizeof boot_time,
                            (long long)boot.tv_sec * 1000 + boot.tv_usec / 1000);
    }

    if (strcmp(n, "GetElapsedRealtime") == 0) {
        static char elapsed[32];
        return clock_string(elapsed, sizeof elapsed, (long long)(mr_mono_ns() / 1000000ull));
    }
    return NULL;
}

static uint32_t guest_string(mr_cpu *c, const char *s) {
    size_t n = strlen(s) + 1;
    uint32_t p = scratch_alloc((uint32_t)n);
    if (p) memcpy(mr_mem(c, p), s, n);
    return p;
}

static uint32_t new_array(mr_cpu *c, uint32_t length, uint32_t elem_size) {
    uint64_t bytes = (uint64_t)length * elem_size;
    if (bytes > UINT32_MAX - 4u) return 0;
    uint32_t p = scratch_alloc((uint32_t)bytes + 4u);
    if (!p) return 0;
    mr_st32(c, p, length);
    if (bytes) memset(mr_mem(c, p + 4u), 0, (size_t)bytes);
    return p;
}

static const char *preference(const char *key) {
    if (strcmp(key, "SDFolder") == 0) return ANDROID_FILES;
    return "";
}

// Static Java fields referenced by the engine.
static const char *static_field(const char *name) {
    if (strcmp(name, "mPreferencesName") == 0) return "GamePrefs";
    return NULL;
}

static int method_float(const char *n, float *out) {
    if (strcmp(n, "GetPhoneCPUFreq") == 0) {
        *out = 2400.0f;
        return 1;
    }
    return 0;
}

// Method implementations.

static uint32_t call_method(mr_cpu *c, uint32_t mid) {
    uint32_t idx = mid - 0x4D000000u;
    if (idx >= (uint32_t)METHOD_COUNT) return 0;
    const char *n = METHODS[idx].name;

    const char *s = device_string(n);
    if (s) return guest_string(c, s);

    if (strcmp(n, "getPreferenceString") == 0) return guest_string(c, preference(gstr(c, A(3))));

    if (strcmp(n, "GetBarrels") == 0) {
        uint32_t p = scratch_alloc(8);
        if (p) mr_st32(c, p, 0); // Length is zero.
        return p;
    }

    if (strcmp(n, "GetPhoneMemTotal") == 0) return 2048;
    if (strcmp(n, "GetPhoneMemAvailable") == 0) return 1536;

    if (strcmp(n, "isNetworkAvailable") == 0) return 0;
    if (strcmp(n, "isWifiAvailable") == 0) return 0;
    if (strcmp(n, "hasNetwork") == 0) return 0;
    if (strcmp(n, "HasConnectivity") == 0) return 0;

    if (strcmp(n, "enableOrientation") == 0) return 0;
    if (strcmp(n, "UserIsInRestrictedAccount") == 0) return 0;

    if (strcmp(n, "getFreeSpaceInKBytes") == 0) return 4u * 1024 * 1024; // 4 GB.

    if (strcmp(n, "startWelcomeScreen") == 0) return 0;
    if (strcmp(n, "triggerAlert") == 0) return 0;
    if (strcmp(n, "UpdateIAPLanguage") == 0) return 0;

    if (LOG_UNKNOWN)
        fprintf(stderr, "[jni] unimplemented method: %s.%s%s\n", METHODS[idx].cls, n,
                METHODS[idx].sig);

    const char *sig = METHODS[idx].sig;
    const char *close = strrchr(sig, ')');
    if (close && strcmp(close + 1, "Ljava/lang/String;") == 0) return guest_string(c, "");

    return 0;
}

// Single dispatcher keyed by table slot.

static int SLOT_BASE_THUNK = -1;

static void jni_handler(mr_cpu *c) {
    uint32_t slot = MR_THUNK_INDEX(c->r[15]) - (uint32_t)SLOT_BASE_THUNK;
    if (slot >= JNI_SLOTS) {
        RET(0);
        return;
    }

    switch (slot) {
    case S_GetVersion:
        RET(0x00010006);
        return; // JNI 1.6

    case S_FindClass: {
        const char *name = gstr(c, A(1));
        int index = intern_class(name);
        RET(index >= 0 ? H_CLASS(index) : 0);
        return;
    }

    case S_GetMethodID:
    case S_GetStaticMethodID: {
        uint32_t cls = A(1);
        uint32_t ci = cls - 0x4C000000u;
        const char *cn = (ci < (uint32_t)CLASS_COUNT) ? CLASSES[ci] : "?";
        int index = intern_method(cn, gstr(c, A(2)), gstr(c, A(3)));
        RET(index >= 0 ? H_METHOD(index) : 0);
        return;
    }

    case S_GetFieldID:
    case S_GetStaticFieldID: {
        int index = intern_method("?", gstr(c, A(2)), gstr(c, A(3)));
        RET(index >= 0 ? H_METHOD(index) : 0);
        return;
    }

    case S_GetObjectClass:
        RET(H_CLASS(0));
        return;

    // The method ID is argument two for both instance and static calls.
    case S_CallVoidMethod:
    case S_CallVoidMethodV:
    case S_CallVoidMethodA:
    case S_CallStaticVoidMethod:
    case S_CallStaticVoidMethodV:
    case S_CallStaticVoidMethodA:
        call_method(c, A(2));
        return;
    // Zero is the correct default for numeric and Boolean calls.
    case S_CallIntMethod:
    case S_CallIntMethodV:
    case S_CallIntMethodA:
    case S_CallStaticIntMethod:
    case S_CallStaticIntMethodV:
    case S_CallStaticIntMethodA:
    case S_CallBooleanMethod:
    case S_CallBooleanMethodV:
    case S_CallBooleanMethodA:
    case S_CallStaticBooleanMethod:
    case S_CallStaticBooleanMethodV:
    case S_CallStaticBooleanMethodA:
        RET(call_method(c, A(2)));
        return;

    case S_CallObjectMethod:
    case S_CallObjectMethodV:
    case S_CallObjectMethodA:
    case S_CallStaticObjectMethod:
    case S_CallStaticObjectMethodV:
    case S_CallStaticObjectMethodA: {
        uint32_t r = call_method(c, A(2));
        if (!r) {
            if (LOG_UNKNOWN) {
                uint32_t mi = A(2) - 0x4D000000u;
                fprintf(stderr, "[jni] null object (slot %u, mid 0x%08x): %s%s\n", slot, A(2),
                        mi < (uint32_t)METHOD_COUNT ? METHODS[mi].name : "?",
                        mi < (uint32_t)METHOD_COUNT ? METHODS[mi].sig : "");
            }
            r = guest_string(c, "");
        }
        RET(r);
        return;
    }
    case S_CallFloatMethod:
    case S_CallFloatMethodV:
    case S_CallFloatMethodA:
    case S_CallStaticFloatMethod:
    case S_CallStaticFloatMethodV:
    case S_CallStaticFloatMethodA: {
        uint32_t mi = A(2) - 0x4D000000u;
        float f = 0.0f;
        if (mi < (uint32_t)METHOD_COUNT && method_float(METHODS[mi].name, &f)) {
            uint32_t bits;
            memcpy(&bits, &f, 4); // softfp returns the float bit pattern in r0.
            RET(bits);
        } else {
            call_method(c, A(2)); // Used for diagnostics.
            RET(0);
        }
        return;
    }

    // Static-field read.
    case S_GetStaticObjectField: {
        uint32_t fi = A(2) - 0x4D000000u;
        const char *fn = fi < (uint32_t)METHOD_COUNT ? METHODS[fi].name : "?";
        const char *v = static_field(fn);
        if (v) {
            RET(guest_string(c, v));
            return;
        }
        if (LOG_UNKNOWN) fprintf(stderr, "[jni] static field: %s\n", fn);
        RET(0);
        return;
    }

    // References are identities because there is no garbage collector.
    case S_NewGlobalRef:
    case S_NewLocalRef:
        RET(A(1));
        return;
    case S_DeleteGlobalRef:
    case S_DeleteLocalRef:
    case S_PushLocalFrame:
    case S_EnsureLocalCapacity:
    case S_MonitorEnter:
    case S_MonitorExit:
        RET(0);
        return;
    case S_PopLocalFrame:
        RET(A(1));
        return;
    case S_IsSameObject:
        RET(A(1) == A(2));
        return;

    // Java exceptions are never raised by this bridge.
    case S_ExceptionOccurred:
        RET(0);
        return;
    case S_ExceptionCheck:
        RET(0);
        return;
    case S_ExceptionClear:
    case S_ExceptionDescribe:
        RET(0);
        return;

    case S_NewStringUTF: {
        const char *s = gstr(c, A(1));
        size_t n = strlen(s) + 1;
        uint32_t p = scratch_alloc((uint32_t)n);
        if (p) memcpy(mr_mem(c, p), s, n);
        RET(p);
        return;
    }
    case S_GetStringUTFChars: {
        uint32_t str = A(1), isCopy = A(2);
        if (isCopy) mr_st8(c, isCopy, 0);
        RET(str);
        return;
    }
    case S_ReleaseStringUTFChars:
        RET(0);
        return;
    case S_GetStringUTFLength: {
        const char *s = gstr(c, A(1));
        RET((uint32_t)strlen(s));
        return;
    }

    // Arrays, primarily byte arrays used by the engine.
    case S_NewIntArray:
        RET(new_array(c, A(1), 4));
        return;
    case S_NewByteArray:
        RET(new_array(c, A(1), 1));
        return;
    case S_GetArrayLength:
        RET(A(1) ? mr_ld32(c, A(1)) : 0);
        return;
    case S_GetIntArrayElements:
    case S_GetByteArrayElements: {
        uint32_t isCopy = A(2);
        if (isCopy) mr_st8(c, isCopy, 0);
        RET(A(1) ? A(1) + 4 : 0);
        return;
    }
    case S_ReleaseIntArrayElements:
    case S_ReleaseByteArrayElements:
        RET(0);
        return;

    case S_GetJavaVM:
        if (A(1)) mr_st32(c, A(1), VM_PTR);
        RET(0);
        return;

    case S_RegisterNatives:
        RET(0);
        return;

    default:
        if (LOG_UNKNOWN) fprintf(stderr, "[jni] unknown table slot: %u\n", slot);
        RET(0);
        return;
    }
}

// JavaVM.

enum {
    V_DestroyJavaVM = 3,
    V_AttachCurrentThread = 4,
    V_DetachCurrentThread = 5,
    V_GetEnv = 6,
    V_AttachCurrentThreadAsDaemon = 7
};
#define VM_SLOTS 8

static int VM_BASE_THUNK = -1;

static void vm_handler(mr_cpu *c) {
    uint32_t slot = MR_THUNK_INDEX(c->r[15]) - (uint32_t)VM_BASE_THUNK;
    switch (slot) {
    case V_GetEnv:
    case V_AttachCurrentThread:
    case V_AttachCurrentThreadAsDaemon:
        if (A(1)) mr_st32(c, A(1), ENV_PTR);
        RET(0); // JNI_OK
        return;
    default:
        RET(0);
        return;
    }
}

// Construction.

uint32_t mr_jni_init(mr_cpu *c, uint32_t base, uint32_t size, mr_thunk_reg reg, void *user) {
    mr_jni_shutdown();
    LOG_UNKNOWN = getenv("MR_JNI_LOG") ? 1 : 0;

    uint32_t p = (base + 7u) & ~7u;

    TABLE_BASE = p;
    p += JNI_SLOTS * 4;

    ENV_PTR = p;
    mr_st32(c, ENV_PTR, TABLE_BASE);
    p += 8;

    // Build the JavaVM table and object.
    uint32_t vm_table = p;
    p += VM_SLOTS * 4;
    VM_PTR = p;
    mr_st32(c, VM_PTR, vm_table);
    p += 8;

    // Reserve workspace for strings and arrays.
    SCRATCH = p;
    SCRATCH_END = base + size;
    SCRATCH_NEXT = SCRATCH;

    // Register thunks and populate the tables.
    for (int i = 0; i < JNI_SLOTS; i++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "jni:%d", i);
        uint32_t addr = reg(nm, jni_handler, user);
        if (i == 0) SLOT_BASE_THUNK = (int)MR_THUNK_INDEX(addr);
        mr_st32(c, TABLE_BASE + (uint32_t)i * 4, addr);
    }
    for (int i = 0; i < VM_SLOTS; i++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "jvm:%d", i);
        uint32_t addr = reg(nm, vm_handler, user);
        if (i == 0) VM_BASE_THUNK = (int)MR_THUNK_INDEX(addr);
        mr_st32(c, vm_table + (uint32_t)i * 4, addr);
    }

    return ENV_PTR;
}

uint32_t mr_jni_vm(void) {
    return VM_PTR;
}
uint32_t mr_jni_end(void) {
    return SCRATCH_END;
}

void mr_jni_report(void) {
    printf("JNI bridge: %d classes, %d methods resolved\n", CLASS_COUNT, METHOD_COUNT);
    for (int i = 0; i < CLASS_COUNT && i < 12; i++)
        printf("   class: %s\n", CLASSES[i]);
}

void mr_jni_shutdown(void) {
    for (int i = 0; i < CLASS_COUNT; i++) {
        free(CLASSES[i]);
        CLASSES[i] = NULL;
    }
    for (int i = 0; i < METHOD_COUNT; i++) {
        free(METHODS[i].cls);
        free(METHODS[i].name);
        free(METHODS[i].sig);
        METHODS[i] = (jmethod){0};
    }
    CLASS_COUNT = 0;
    METHOD_COUNT = 0;
    ENV_PTR = 0;
    VM_PTR = 0;
    TABLE_BASE = 0;
    SCRATCH = 0;
    SCRATCH_END = 0;
    SCRATCH_NEXT = 0;
    SLOT_BASE_THUNK = -1;
    VM_BASE_THUNK = -1;
}
