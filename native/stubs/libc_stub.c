/*
 * mst2-cluster-nav - link stub of the unit's libc.so.3.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 *
 * Only the names matter: linking against this stub records DT_NEEDED libc.so.3 and leaves the
 * symbols for the QNX run-time linker, which binds them to the real libc on the unit. Nothing
 * here runs.
 */
#define STUB(name) \
    void name(void) \
    { \
    }

STUB(open)
STUB(close)
STUB(lseek)
STUB(write)
STUB(read)
STUB(getpid)
STUB(getenv)
STUB(dlopen)
STUB(dlsym)
STUB(strlen)
STUB(strstr)
STUB(strcmp)
STUB(memcmp)
STUB(memcpy)
STUB(memset)
STUB(malloc)
STUB(free)
STUB(abort)
STUB(snprintf)
STUB(usleep)
STUB(clock_gettime)
STUB(pthread_create)
STUB(pthread_detach)
STUB(pthread_join)
STUB(pthread_mutex_init)
STUB(pthread_mutex_lock)
STUB(pthread_mutex_unlock)
STUB(poll)
STUB(shm_open)
STUB(ftruncate)
STUB(mmap)
STUB(munmap)
STUB(msync)
STUB(mem_offset)
STUB(unlink)
STUB(rename)
STUB(opendir)
STUB(readdir)
STUB(closedir)
STUB(sem_init)
STUB(sem_post)
STUB(sem_timedwait_monotonic)
STUB(__get_errno_ptr)
