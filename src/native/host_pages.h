#ifndef MR_HOST_PAGES_H
#define MR_HOST_PAGES_H

#include <stddef.h>

// Build-time and runtime ELF allocation use platform-native private pages.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static inline void *mr_host_pages(size_t length) {
    return VirtualAlloc(NULL, length, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

static inline void mr_host_pages_free(void *address, size_t length) {
    (void)length;
    if (address) VirtualFree(address, 0, MEM_RELEASE);
}

static inline long mr_host_page_size(void) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return (long)info.dwPageSize;
}
#else
#include <sys/mman.h>
#include <unistd.h>

static inline void *mr_host_pages(size_t length) {
    void *address = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return address == MAP_FAILED ? NULL : address;
}

static inline void mr_host_pages_free(void *address, size_t length) {
    if (address) munmap(address, length);
}

static inline long mr_host_page_size(void) {
    return sysconf(_SC_PAGESIZE);
}
#endif

#endif
