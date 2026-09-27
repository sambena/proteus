/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A few threads that share a frame's work. The thread that asks takes its part of it, and
 * has the result when px_pool_run returns. */
#include "fx.h"

#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
typedef CRITICAL_SECTION   pool_lock;
typedef CONDITION_VARIABLE pool_cond;
typedef HANDLE             pool_thread;
#define lock_init(l)    InitializeCriticalSection(l)
#define lock_free(l)    DeleteCriticalSection(l)
#define lock_take(l)    EnterCriticalSection(l)
#define lock_give(l)    LeaveCriticalSection(l)
#define cond_init(c)    InitializeConditionVariable(c)
#define cond_free(c)    ((void)(c))
#define cond_wait(c, l) SleepConditionVariableCS(c, l, INFINITE)
#define cond_all(c)     WakeAllConditionVariable(c)
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_mutex_t pool_lock;
typedef pthread_cond_t  pool_cond;
typedef pthread_t       pool_thread;
#define lock_init(l)    pthread_mutex_init(l, NULL)
#define lock_free(l)    pthread_mutex_destroy(l)
#define lock_take(l)    pthread_mutex_lock(l)
#define lock_give(l)    pthread_mutex_unlock(l)
#define cond_init(c)    pthread_cond_init(c, NULL)
#define cond_free(c)    pthread_cond_destroy(c)
#define cond_wait(c, l) pthread_cond_wait(c, l)
#define cond_all(c)     pthread_cond_broadcast(c)
#endif

struct px_pool
{
   pool_lock lock;
   pool_cond work;      /* there are parts to take, or it is time to go */
   pool_cond done;      /* the last part is done */
   pool_thread threads[PX_POOL_MAX];
   unsigned thread_count;

   px_pool_job job;
   void *ctx;
   unsigned count;      /* parts of the job */
   unsigned next;       /* the first part nobody has taken */
   unsigned pending;    /* parts not done */
   bool quit;
};

/* Takes parts until there are none; called with the lock held, returns with it held. */
static void take_parts(px_pool *p)
{
   while (p->next < p->count)
   {
      unsigned i = p->next++, count = p->count;
      px_pool_job job = p->job;
      void *ctx = p->ctx;
      lock_give(&p->lock);
      job(ctx, i, count);
      lock_take(&p->lock);
      if (--p->pending == 0)
         cond_all(&p->done);
   }
}

#ifdef _WIN32
static DWORD WINAPI worker(LPVOID arg)
#else
static void *worker(void *arg)
#endif
{
   px_pool *p = (px_pool*)arg;
   lock_take(&p->lock);
   while (!p->quit)
   {
      if (p->next < p->count)
         take_parts(p);
      else
         cond_wait(&p->work, &p->lock);
   }
   lock_give(&p->lock);
   return 0;
}

unsigned px_pool_parts(const px_pool *p)
{
   return p ? p->thread_count + 1 : 1;
}

px_pool *px_pool_new(void)
{
   px_pool *p = (px_pool*)calloc(1, sizeof(*p));
   unsigned cores = 1, wanted;
#ifdef _WIN32
   SYSTEM_INFO info;
   GetSystemInfo(&info);
   cores = info.dwNumberOfProcessors;
#else
   long n = sysconf(_SC_NPROCESSORS_ONLN);
   cores = n > 0 ? (unsigned)n : 1;
#endif
   if (!p)
      return NULL;
   lock_init(&p->lock);
   cond_init(&p->work);
   cond_init(&p->done);

   /* The emulator and the frontend have their work too. */
   wanted = cores > 2 ? cores / 2 : 1;
   if (wanted > PX_POOL_MAX + 1)
      wanted = PX_POOL_MAX + 1;
   for (unsigned i = 0; i + 1 < wanted; i++)
   {
#ifdef _WIN32
      HANDLE t = CreateThread(NULL, 0, worker, p, 0, NULL);
      if (!t)
         break;
      p->threads[p->thread_count++] = t;
#else
      if (pthread_create(&p->threads[p->thread_count], NULL, worker, p))
         break;
      p->thread_count++;
#endif
   }
   return p;
}

void px_pool_free(px_pool *p)
{
   if (!p)
      return;
   lock_take(&p->lock);
   p->quit = true;
   cond_all(&p->work);
   lock_give(&p->lock);
   for (unsigned i = 0; i < p->thread_count; i++)
   {
#ifdef _WIN32
      WaitForSingleObject(p->threads[i], INFINITE);
      CloseHandle(p->threads[i]);
#else
      pthread_join(p->threads[i], NULL);
#endif
   }
   cond_free(&p->work);
   cond_free(&p->done);
   lock_free(&p->lock);
   free(p);
}

void px_pool_run(px_pool *p, px_pool_job job, void *ctx, unsigned count)
{
   if (!count)
      return;
   if (!p || !p->thread_count || count == 1)
   {
      for (unsigned i = 0; i < count; i++)
         job(ctx, i, count);
      return;
   }
   lock_take(&p->lock);
   p->job     = job;
   p->ctx     = ctx;
   p->count   = count;
   p->next    = 0;
   p->pending = count;
   cond_all(&p->work);
   take_parts(p);
   while (p->pending)
      cond_wait(&p->done, &p->lock);
   p->count = p->next = 0;
   lock_give(&p->lock);
}
