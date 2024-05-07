/*
 * Copyright (C) 2023 Intel Corporation
 *
 * Under the Apache License v2.0 with LLVM Exceptions. See LICENSE.TXT.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
*/

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <umf/providers/provider_os_memory.h>

#include "provider_os_memory_internal.h"
#include "utils_log.h"

// maximum value of the off_t type
#define OFF_T_MAX                                                              \
    (sizeof(off_t) == sizeof(long long)                                        \
         ? LLONG_MAX                                                           \
         : (sizeof(off_t) == sizeof(long) ? LONG_MAX : INT_MAX))

umf_result_t os_translate_mem_protection_one_flag(unsigned in_protection,
                                                  unsigned *out_protection) {
    switch (in_protection) {
    case UMF_PROTECTION_NONE:
        *out_protection = PROT_NONE;
        return UMF_RESULT_SUCCESS;
    case UMF_PROTECTION_READ:
        *out_protection = PROT_READ;
        return UMF_RESULT_SUCCESS;
    case UMF_PROTECTION_WRITE:
        *out_protection = PROT_WRITE;
        return UMF_RESULT_SUCCESS;
    case UMF_PROTECTION_EXEC:
        *out_protection = PROT_EXEC;
        return UMF_RESULT_SUCCESS;
    }
    return UMF_RESULT_ERROR_INVALID_ARGUMENT;
}

umf_result_t os_translate_mem_visibility_flag(umf_memory_visibility_t in_flag,
                                              unsigned *out_flag) {
    switch (in_flag) {
    case UMF_MEM_MAP_PRIVATE:
        *out_flag = MAP_PRIVATE;
        return UMF_RESULT_SUCCESS;
    case UMF_MEM_MAP_SHARED:
#ifdef __APPLE__
        return UMF_RESULT_ERROR_NOT_SUPPORTED; // not supported on MacOSX
#else
        *out_flag = MAP_SHARED;
        return UMF_RESULT_SUCCESS;
#endif
    }
    return UMF_RESULT_ERROR_INVALID_ARGUMENT;
}

#ifndef __APPLE__
static int syscall_memfd_secret(void) {
    int fd = -1;
#ifdef __NR_memfd_secret
    // SYS_memfd_secret is supported since Linux 5.14
    fd = syscall(SYS_memfd_secret, 0);
    if (fd == -1) {
        LOG_PERR("memfd_secret() failed");
    }
    if (fd > 0) {
        LOG_DEBUG("anonymous file descriptor created using memfd_secret()");
    }
#endif /* __NR_memfd_secret */
    return fd;
}

static int syscall_memfd_create(void) {
    int fd = -1;
#ifdef __NR_memfd_create
    // SYS_memfd_create is supported since Linux 3.17, glibc 2.27
    fd = syscall(SYS_memfd_create, "anon_fd_name", 0);
    if (fd == -1) {
        LOG_PERR("memfd_create() failed");
    }
    if (fd > 0) {
        LOG_DEBUG("anonymous file descriptor created using memfd_create()");
    }
#endif /* __NR_memfd_create */
    return fd;
}
#endif /* __APPLE__ */

// create an anonymous file descriptor
int os_create_anonymous_fd(unsigned translated_memory_flag) {
#ifdef __APPLE__
    (void)translated_memory_flag; // unused
    return 0;                     // ignored on MacOSX
#else                             /* !__APPLE__ */
    // fd is created only for MAP_SHARED
    if (translated_memory_flag != MAP_SHARED) {
        return 0;
    }

    int fd = -1;

    if (!util_env_var_has_str("UMF_MEM_FD_FUNC", "memfd_create")) {
        fd = syscall_memfd_secret();
        if (fd > 0) {
            return fd;
        }
    }

    // The SYS_memfd_secret syscall can fail with errno == ENOTSYS (function not implemented).
    // We should try to call the SYS_memfd_create syscall in this case.

    fd = syscall_memfd_create();

#if !(defined __NR_memfd_secret) && !(defined __NR_memfd_create)
    if (fd == = 1) {
        LOG_ERR("cannot create an anonymous file descriptor - neither "
                "memfd_secret() nor memfd_create() are defined");
    }
#endif /* !(defined __NR_memfd_secret) && !(defined __NR_memfd_create) */

    return fd;

#endif /* !__APPLE__ */
}

size_t get_max_file_size(void) { return OFF_T_MAX; }

int os_set_file_size(int fd, size_t size) {
#ifdef __APPLE__
    (void)fd;   // unused
    (void)size; // unused
    return 0;   // ignored on MacOSX
#else
    errno = 0;
    int ret = ftruncate(fd, size);
    if (ret) {
        LOG_PERR("ftruncate(%i, %zu) failed", fd, size);
    }
    return ret;
#endif /* __APPLE__ */
}

umf_result_t os_translate_mem_protection_flags(unsigned in_protection,
                                               unsigned *out_protection) {
    // translate protection - combination of 'umf_mem_protection_flags_t' flags
    return os_translate_flags(in_protection, UMF_PROTECTION_MAX,
                              os_translate_mem_protection_one_flag,
                              out_protection);
}

static int os_translate_purge_advise(umf_purge_advise_t advise) {
    switch (advise) {
    case UMF_PURGE_LAZY:
        return MADV_FREE;
    case UMF_PURGE_FORCE:
        return MADV_DONTNEED;
    }
    assert(0);
    return -1;
}

void *os_mmap(void *hint_addr, size_t length, int prot, int flag, int fd,
              size_t fd_offset) {
    fd = (fd == 0) ? -1 : fd;
    if (fd == -1) {
        // MAP_ANONYMOUS - the mapping is not backed by any file
        flag |= MAP_ANONYMOUS;
    }

    void *ptr = mmap(hint_addr, length, prot, flag, fd, fd_offset);
    if (ptr == MAP_FAILED) {
        return NULL;
    }

    return ptr;
}

int os_munmap(void *addr, size_t length) { return munmap(addr, length); }

size_t os_get_page_size(void) { return sysconf(_SC_PAGE_SIZE); }

int os_purge(void *addr, size_t length, int advice) {
    return madvise(addr, length, os_translate_purge_advise(advice));
}

void os_strerror(int errnum, char *buf, size_t buflen) {
    strerror_r(errnum, buf, buflen);
}
