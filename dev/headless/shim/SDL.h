// Minimal SDL stand-in for the headless test harness (threads, timing): no SDL library needed, any target (incl. qemu-user).
#pragma once
#include <stdint.h>
typedef uint32_t Uint32; typedef uint8_t Uint8; typedef int16_t Sint16; typedef uint16_t Uint16;
typedef struct SDL_Thread SDL_Thread; typedef struct SDL_sem SDL_sem; typedef struct SDL_cond SDL_cond; typedef struct SDL_mutex SDL_mutex;
typedef int (*SDL_ThreadFunction)(void *);
typedef unsigned long SDL_threadID;
enum { SDL_THREAD_PRIORITY_LOW, SDL_THREAD_PRIORITY_NORMAL, SDL_THREAD_PRIORITY_HIGH, SDL_THREAD_PRIORITY_TIME_CRITICAL };
#define SDL_MESSAGEBOX_ERROR 0x10
#ifdef __cplusplus
extern "C" {
#endif
void SDL_Delay(Uint32 ms); Uint32 SDL_GetTicks(void); const char *SDL_GetError(void);
SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *data); void SDL_WaitThread(SDL_Thread *t, int *status);
SDL_threadID SDL_GetThreadID(SDL_Thread *t); int SDL_SetThreadPriority(int p);
SDL_sem *SDL_CreateSemaphore(Uint32 v); void SDL_DestroySemaphore(SDL_sem *s); int SDL_SemWait(SDL_sem *s); int SDL_SemPost(SDL_sem *s); Uint32 SDL_SemValue(SDL_sem *s);
SDL_cond *SDL_CreateCond(void); void SDL_DestroyCond(SDL_cond *c); int SDL_CondWait(SDL_cond *c, SDL_mutex *m); int SDL_CondSignal(SDL_cond *c); int SDL_CondBroadcast(SDL_cond *c);
SDL_mutex *SDL_CreateMutex(void); void SDL_DestroyMutex(SDL_mutex *m); int SDL_LockMutex(SDL_mutex *m); int SDL_UnlockMutex(SDL_mutex *m);
#define SDL_mutexP(m) SDL_LockMutex(m)
#define SDL_mutexV(m) SDL_UnlockMutex(m)
int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *msg, void *win);
#ifdef __cplusplus
}
#endif
