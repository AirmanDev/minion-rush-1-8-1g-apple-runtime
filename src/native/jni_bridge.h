#ifndef MR_JNI_BRIDGE_H
#define MR_JNI_BRIDGE_H

#include "cpu.h"
#include "elf_loader.h"

typedef uint32_t (*mr_thunk_reg)(const char *name, mr_thunk_fn fn, void *user);

// Builds the synthetic JNIEnv and JavaVM in the guest range [base, base+size).
// Returns the guest address of JNIEnv*.
uint32_t mr_jni_init(mr_cpu *cpu, uint32_t base, uint32_t size, mr_thunk_reg reg, void *user);

// Supplies the actual GPU name requested by the engine.
void mr_jni_set_gpu_name(const char *name);

uint32_t mr_jni_vm(void);
uint32_t mr_jni_end(void);
void mr_jni_report(void);
void mr_jni_shutdown(void);

#endif
