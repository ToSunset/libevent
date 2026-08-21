/* thread_compat.h - 跨平台线程兼容层（C11 threads 风格）
 *
 * 目的：让源码在 Linux 与 Windows(MSVC/MinGW) 上都能编译，Windows 不再依赖
 *       pthread.h（MSVC 不提供该头文件）。
 *   - Linux   : 内部直接映射到原生 pthread
 *   - Windows : 映射到 SRWLock / CONDITION_VARIABLE / _beginthreadex
 *
 * 用法（返回 0 = 成功，与 pthread 风格一致）：
 *   mtx_t   mtx_init / mtx_destroy / mtx_lock / mtx_unlock
 *   cnd_t   cnd_init / cnd_destroy / cnd_broadcast
 *           cnd_timedwait(c, m, timeout_ms)：0=被唤醒，1=超时
 *   thrd_t  thrd_create(t, fn, arg) / thrd_join(t, res) / thrd_detach(t)
 *           未创建的线程句柄为 0，可直接作布尔判断。
 */

#ifndef THREAD_COMPAT_H
#define THREAD_COMPAT_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L   /* 保证 clock_gettime 等 POSIX 接口可见 */
#endif

#include <stdint.h>
#include <stdlib.h>

#ifdef _WIN32

#include <winsock2.h>   /* 必须先于 windows.h，避免 winsock 版本冲突 */
#include <windows.h>
#include <process.h>    /* _beginthreadex */

typedef SRWLOCK            mtx_t;
typedef CONDITION_VARIABLE cnd_t;
typedef uintptr_t          thrd_t;   /* 0 = 未创建 */

static inline int mtx_init(mtx_t *m)      { InitializeSRWLock(m); return 0; }
static inline void mtx_destroy(mtx_t *m)  { (void)m; }
static inline void mtx_lock(mtx_t *m)     { AcquireSRWLockExclusive(m); }
static inline void mtx_unlock(mtx_t *m)   { ReleaseSRWLockExclusive(m); }

static inline int cnd_init(cnd_t *c)      { InitializeConditionVariable(c); return 0; }
static inline void cnd_destroy(cnd_t *c)  { (void)c; }
static inline void cnd_broadcast(cnd_t *c){ WakeAllConditionVariable(c); }

/* 超时单位毫秒：返回 0=被唤醒，1=超时 */
static inline int cnd_timedwait(cnd_t *c, mtx_t *m, long timeout_ms)
{
    if (SleepConditionVariableSRW(c, m, (DWORD)timeout_ms, 0))
        return 0;
    return (GetLastError() == ERROR_TIMEOUT) ? 1 : 0;
}

/* _beginthreadex 的入口签名与 pthread 不同，用临时结构体做桥接 */
typedef struct {
    void *(*fn)(void *);
    void *arg;
} thrd_start_t;

static unsigned __stdcall thrd_wrapper(void *p)
{
    thrd_start_t *s = (thrd_start_t *)p;
    s->fn(s->arg);
    free(s);
    return 0;
}

static inline int thrd_create(thrd_t *t, void *(*fn)(void *), void *arg)
{
    thrd_start_t *s = (thrd_start_t *)malloc(sizeof(thrd_start_t));
    uintptr_t h;

    if (!s)
        return 1;
    s->fn  = fn;
    s->arg = arg;
    h = (uintptr_t)_beginthreadex(NULL, 0, thrd_wrapper, s, 0, NULL);
    if (h == 0) {
        free(s);
        return 1;
    }
    *t = h;
    return 0;
}

static inline int thrd_join(thrd_t t, void **res)
{
    WaitForSingleObject((HANDLE)t, INFINITE);
    CloseHandle((HANDLE)t);
    if (res)
        *res = NULL;
    return 0;
}

/* 分离线程：不再 join，线程结束自动回收资源 */
static inline void thrd_detach(thrd_t t)
{
    CloseHandle((HANDLE)t);
}

#else  /* !_WIN32：POSIX */

#include <pthread.h>
#include <time.h>

typedef pthread_mutex_t mtx_t;
typedef pthread_cond_t  cnd_t;
typedef pthread_t       thrd_t;

static inline int mtx_init(mtx_t *m)      { return pthread_mutex_init(m, NULL); }
static inline void mtx_destroy(mtx_t *m)  { pthread_mutex_destroy(m); }
static inline void mtx_lock(mtx_t *m)     { pthread_mutex_lock(m); }
static inline void mtx_unlock(mtx_t *m)   { pthread_mutex_unlock(m); }

static inline int cnd_init(cnd_t *c)      { return pthread_cond_init(c, NULL); }
static inline void cnd_destroy(cnd_t *c)  { pthread_cond_destroy(c); }
static inline void cnd_broadcast(cnd_t *c){ pthread_cond_broadcast(c); }

/* 超时单位毫秒：返回 0=被唤醒，1=超时（内部转成 CLOCK_REALTIME 绝对时间） */
static inline int cnd_timedwait(cnd_t *c, mtx_t *m, long timeout_ms)
{
    struct timespec ts;
    int r;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    r = pthread_cond_timedwait(c, m, &ts);
    return (r == ETIMEDOUT) ? 1 : 0;
}

static inline int thrd_create(thrd_t *t, void *(*fn)(void *), void *arg)
{
    return pthread_create(t, NULL, fn, arg);
}

static inline int thrd_join(thrd_t t, void **res)
{
    void *r = NULL;
    int rc = pthread_join(t, &r);
    if (res)
        *res = r;
    return rc;
}

static inline void thrd_detach(thrd_t t)
{
    pthread_detach(t);
}

#endif /* _WIN32 */

#endif /* THREAD_COMPAT_H */
