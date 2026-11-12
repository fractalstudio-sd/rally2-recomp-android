// Emulated TLS for 32-bit ARM Android (bionic before API 29 cannot give dlopen()ed libraries real TLS).
// Same contract as compiler-rt's __emutls_get_address, minimal version. GPLv3.
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uintptr_t size; uintptr_t align; union { uintptr_t index; void *ptr; } object; void *value; } emutls_control;
typedef struct { uintptr_t n; void *raw[1]; } emutls_tab;   // raw[i] = block base, data pointer is stored in ptr[i]

static pthread_key_t g_key; static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; static uintptr_t g_next = 1;

typedef struct { uintptr_t n; void *base[1]; } tab_t;   // per-thread table of malloc'ed blocks, indexed by control index

static void dtor(void *p) { tab_t *t = (tab_t *)p; if (!t) return; for (uintptr_t i = 1; i <= t->n; i++) free(t->base[i - 1]); free(t); }
static void mk(void) { pthread_key_create(&g_key, dtor); }

void *__emutls_get_address(emutls_control *c)
{
	pthread_once(&g_once, mk);
	uintptr_t idx = __atomic_load_n(&c->object.index, __ATOMIC_ACQUIRE);
	if (!idx) {
		pthread_mutex_lock(&g_mu);
		idx = c->object.index;
		if (!idx) { idx = g_next++; __atomic_store_n(&c->object.index, idx, __ATOMIC_RELEASE); }
		pthread_mutex_unlock(&g_mu);
	}
	tab_t *t = (tab_t *)pthread_getspecific(g_key);
	if (!t || t->n < idx) {
		uintptr_t nn = idx + 8;
		tab_t *nt = (tab_t *)calloc(1, sizeof(tab_t) + nn * sizeof(void *));
		if (!nt) abort();
		if (t) memcpy(nt->base, t->base, t->n * sizeof(void *));
		nt->n = nn; free(t); t = nt; pthread_setspecific(g_key, t);
	}
	// blocks hold: [raw malloc pointer is the table entry] data aligned inside; we keep data ptr = entry aligned up, so store block and compute again
	void *blk = t->base[idx - 1];
	uintptr_t al = c->align ? c->align : 1;
	if (!blk) {
		blk = malloc(c->size + al);
		if (!blk) abort();
		uintptr_t d = ((uintptr_t)blk + al - 1) & ~(al - 1);
		if (c->value) memcpy((void *)d, c->value, c->size); else memset((void *)d, 0, c->size);
		t->base[idx - 1] = blk;
	}
	return (void *)(((uintptr_t)blk + al - 1) & ~(al - 1));
}
