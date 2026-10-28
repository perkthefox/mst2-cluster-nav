/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 *
 * The few libc and libsocket declarations the hooks use, written from POSIX and the BSD
 * socket ABI QNX uses: the build needs no QNX SDK. Target: QNX 6.5 armle-v7, ARM EABI5,
 * soft-float (as the unit's libc.so.3). QNX values: <fcntl.h>, <sys/mman.h>, <time.h>.
 */
#ifndef MCN_QNX_H
#define MCN_QNX_H

typedef unsigned int size_t;
typedef int ssize_t;
typedef long off_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef int int32_t;
typedef short int16_t;
typedef long long int64_t;
typedef unsigned long long uint64_t;
typedef unsigned int uintptr_t;
typedef int pthread_t;

#define NULL ((void *)0)
#define RTLD_DEFAULT ((void *)-2)
#define RTLD_NEXT ((void *)-3)
#define RTLD_LAZY 0x0001
#define RTLD_GLOBAL 0x0100

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR 0x0002
#define O_APPEND 0x0008
#define O_CREAT 0x0100
#define O_TRUNC 0x0200

/* <sys/mman.h> values as the unit's showmem prints them (mQbEMU research notes). */
#define PROT_READ 0x0100
#define PROT_WRITE 0x0200
#define PROT_NOCACHE 0x0800
#define MAP_SHARED 0x0001
#define MAP_PRIVATE 0x0002
#define MAP_ANON 0x00080000
#define MAP_PHYS 0x00010000
#define MAP_FAILED ((void *)-1)
/* QNX: physical address of a mapping made with MAP_PHYS (for handing buffers to the GC320). */
int mem_offset(const void *addr, int fd, size_t len, off_t *offset, size_t *contig_len);
/* The calling thread's errno (QNX libc export, libc.so.3 0x62494). */
int *__get_errno_ptr(void);

#define CLOCK_MONOTONIC 2

struct timespec {
    long tv_sec;
    long tv_nsec;
};

/* BSD layout, as QNX's io-pkt: length byte, family byte. */
struct sockaddr_in {
    uint8_t sin_len;
    uint8_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    char sin_zero[8];
};

#define AF_INET 2
#define AF_INET6 24
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define IPPROTO_TCP 6
#define SOL_SOCKET 0xffff
#define SO_REUSEADDR 0x0004
#define SHUT_RDWR 2

int open(const char *path, int flags, ...);
int close(int fd);
off_t lseek(int fd, off_t off, int whence);
#define SEEK_END 2
#define SEEK_SET 0
ssize_t write(int fd, const void *buf, size_t n);
ssize_t read(int fd, void *buf, size_t n);
int getpid(void);
char *getenv(const char *name);
void *dlopen(const char *path, int mode);
void *dlsym(void *handle, const char *name);
size_t strlen(const char *s);
char *strstr(const char *haystack, const char *needle);
int strcmp(const char *a, const char *b);
int memcmp(const void *a, const void *b, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
void *malloc(size_t n);
void free(void *p);
void abort(void);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int usleep(unsigned usec);
int clock_gettime(int clock, struct timespec *ts);
int pthread_create(pthread_t *thread, const void *attr, void *(*fn)(void *), void *arg);
int pthread_detach(pthread_t thread);
int pthread_join(pthread_t thread, void **value);
/* QNX pthread_mutex_t is 8 bytes; VideoSink's Mutex at endpoint+0x4c is one (aa-video notes). */
typedef struct {
    int owner;
    unsigned count;
} pthread_mutex_t;
int pthread_mutex_init(pthread_mutex_t *mutex, const void *attr);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

struct pollfd {
    int fd;
    short events;
    short revents;
};
/* POLLRDNORM | POLLRDBAND: the unit's ping polls its socket with events 5. */
#define POLLIN 0x0005
int poll(struct pollfd *fds, unsigned n, int timeout_ms);
int shm_open(const char *name, int flags, int mode);
/* QNX sem_t is a sync_t: count and owner (sem_post reads +4; libc.so.3 sem_init 0x1e278). */
typedef struct {
    int count;
    unsigned owner;
} sem_t;
int sem_init(sem_t *sem, int pshared, unsigned value);
int sem_post(sem_t *sem);
int sem_timedwait_monotonic(sem_t *sem, const struct timespec *abs_timeout);
int ftruncate(int fd, off_t length);
int unlink(const char *path);
int rename(const char *from, const char *to);
/* QNX 6.5 struct dirent: 64-bit d_ino and d_offset, d_reclen at 0x10, d_namelen at 0x12, d_name
 * at 0x14 (libc.so.3 readdir 0x178f8 finds the entry's extra data at align8(0x14 + namelen + 1),
 * ev_16d6b0e7). */
typedef struct mcn_dir DIR;
DIR *opendir(const char *path);
void *readdir(DIR *dir);
int closedir(DIR *dir);
#define MCN_DIRENT_NAME(e) ((const char *)(e) + 0x14)
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);

/* libsocket.so.3 */
int socket(int domain, int type, int protocol);
int setsockopt(int fd, int level, int name, const void *value, unsigned len);
int bind(int fd, const void *addr, unsigned len);
int listen(int fd, int backlog);
int accept(int fd, void *addr, unsigned *len);
int getsockname(int fd, void *addr, unsigned *len);
ssize_t recv(int fd, void *buf, size_t n, int flags);
ssize_t sendto(int fd, const void *buf, size_t n, int flags, const void *addr, unsigned len);
int shutdown(int fd, int how);

static inline uint16_t mcn_htons(uint16_t v)
{
    return (uint16_t)((v << 8) | (v >> 8));
}

static inline unsigned mcn_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)ts.tv_sec * 1000u + (unsigned)ts.tv_nsec / 1000000u;
}

#endif
