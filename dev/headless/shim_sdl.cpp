// pthread implementation of the SDL subset declared in shim/SDL.h.
#include "shim/SDL.h"
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <unistd.h>
#include <cstdio>
struct SDL_Thread { pthread_t t; SDL_ThreadFunction fn; void *data; int ret; };
struct SDL_sem { pthread_mutex_t m; pthread_cond_t c; Uint32 v; };
struct SDL_cond { pthread_cond_t c; };
struct SDL_mutex { pthread_mutex_t m; };
extern "C" {
void SDL_Delay(Uint32 ms) { usleep(ms * 1000u); }
Uint32 SDL_GetTicks(void) { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (Uint32)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u); }
const char *SDL_GetError(void) { return ""; }
static void *tramp(void *p) { SDL_Thread *t = (SDL_Thread *)p; t->ret = t->fn(t->data); return nullptr; }
SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char *, void *data) {
	SDL_Thread *t = new SDL_Thread{}; t->fn = fn; t->data = data;
	pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8u << 20);
	if (pthread_create(&t->t, &a, tramp, t)) { delete t; return nullptr; } return t;
}
void SDL_WaitThread(SDL_Thread *t, int *st) { if (!t) return; pthread_join(t->t, nullptr); if (st) *st = t->ret; delete t; }
SDL_threadID SDL_GetThreadID(SDL_Thread *t) { return t ? (SDL_threadID)t->t : (SDL_threadID)pthread_self(); }
int SDL_SetThreadPriority(int) { return 0; }
SDL_sem *SDL_CreateSemaphore(Uint32 v) { SDL_sem *s = new SDL_sem; pthread_mutex_init(&s->m, 0); pthread_cond_init(&s->c, 0); s->v = v; return s; }
void SDL_DestroySemaphore(SDL_sem *s) { delete s; }
int SDL_SemWait(SDL_sem *s) { pthread_mutex_lock(&s->m); while (!s->v) pthread_cond_wait(&s->c, &s->m); s->v--; pthread_mutex_unlock(&s->m); return 0; }
int SDL_SemPost(SDL_sem *s) { pthread_mutex_lock(&s->m); s->v++; pthread_cond_signal(&s->c); pthread_mutex_unlock(&s->m); return 0; }
Uint32 SDL_SemValue(SDL_sem *s) { return s->v; }
SDL_cond *SDL_CreateCond(void) { SDL_cond *c = new SDL_cond; pthread_cond_init(&c->c, 0); return c; }
void SDL_DestroyCond(SDL_cond *c) { delete c; }
int SDL_CondWait(SDL_cond *c, SDL_mutex *m) { return pthread_cond_wait(&c->c, &m->m); }
int SDL_CondSignal(SDL_cond *c) { return pthread_cond_signal(&c->c); }
int SDL_CondBroadcast(SDL_cond *c) { return pthread_cond_broadcast(&c->c); }
SDL_mutex *SDL_CreateMutex(void) { SDL_mutex *m = new SDL_mutex; pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE); pthread_mutex_init(&m->m, &a); return m; }
void SDL_DestroyMutex(SDL_mutex *m) { delete m; }
int SDL_LockMutex(SDL_mutex *m) { return pthread_mutex_lock(&m->m); }
int SDL_UnlockMutex(SDL_mutex *m) { return pthread_mutex_unlock(&m->m); }
int SDL_ShowSimpleMessageBox(Uint32, const char *t, const char *m, void *) { fprintf(stderr, "[msgbox] %s: %s\n", t, m); return 0; }
}
