#ifndef SYS_MMAN_STUB_H
#define SYS_MMAN_STUB_H

#include <cstddef>
#include <cstdint>

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define MAP_PRIVATE   0x02
#define MAP_ANON      0x20
#define MAP_FAILED    ((void*)(intptr_t)-1)

inline void* mmap(void* /*addr*/, size_t /*length*/, int /*prot*/,
                   int /*flags*/, int /*fd*/, size_t /*offset*/) {
    return MAP_FAILED;
}

inline int munmap(void* /*addr*/, size_t /*length*/) { return -1; }

#endif
