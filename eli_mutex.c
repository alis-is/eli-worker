#include "eli_worker_internal.h"

#include <stdatomic.h>
#include <stdlib.h>

/* One box per Lua state: a state that receives the same mutex again gets a
 * fresh box that does not inherit the previous holder's acquisition. */
typedef struct mutex_ud {
	eli_mutex *mutex;
	int held; /* the lock was acquired through this box */
} mutex_ud;

struct eli_mutex {
	mtx_t lock; /* guards locked and owner */
	cnd_t cond;
	int locked;
	thrd_t owner; /* valid while locked */
	_Atomic int refs;
};

static _Atomic size_t mutex_count;

static int mutex_owned_by_current(const eli_mutex *mutex)
{
	/* The bundled header's Win32 backend returns the thread ID from
	 * thrd_current() and does not implement thrd_equal. */
#ifdef C11THREADS_WIN32
	return mutex->owner == thrd_current();
#else
	return thrd_equal(mutex->owner, thrd_current()) != 0;
#endif
}

static eli_mutex *mutex_new(void)
{
	eli_mutex *mutex = (eli_mutex *)calloc(1, sizeof(*mutex));

	if (mutex == NULL) {
		return NULL;
	}
	if (mtx_init(&mutex->lock, mtx_plain) != thrd_success) {
		free(mutex);
		return NULL;
	}
	if (cnd_init(&mutex->cond) != thrd_success) {
		mtx_destroy(&mutex->lock);
		free(mutex);
		return NULL;
	}
	mutex->refs = 1;
	atomic_fetch_add(&mutex_count, 1);
	return mutex;
}

static void mutex_retain(eli_mutex *mutex)
{
	if (mutex != NULL) {
		atomic_fetch_add(&mutex->refs, 1);
	}
}

static void mutex_release(eli_mutex *mutex)
{
	if (mutex == NULL) {
		return;
	}
	if (atomic_fetch_sub(&mutex->refs, 1) == 1) {
		cnd_destroy(&mutex->cond);
		mtx_destroy(&mutex->lock);
		atomic_fetch_sub(&mutex_count, 1);
		free(mutex);
	}
}

size_t eli_mutex_live_count(void)
{
	return atomic_load(&mutex_count);
}

/* ---- Lua API ---- */

static mutex_ud *mutex_box_check(lua_State *L, int index)
{
	mutex_ud *ud = (mutex_ud *)luaL_checkudata(L, index, ELI_MUTEX_MT);

	if (ud->mutex == NULL) {
		luaL_error(L, "attempt to use a finalized mutex");
		return NULL;
	}
	return ud;
}

/* Releases the native lock. Never raises so it is safe during error unwinding;
 * callers only invoke it for a box whose held flag proves this thread is the
 * owner. */
static void mutex_unlock_native(eli_mutex *mutex)
{
	mtx_lock(&mutex->lock);
	if (mutex->locked && mutex_owned_by_current(mutex)) {
		mutex->locked = 0;
		cnd_broadcast(&mutex->cond);
	}
	mtx_unlock(&mutex->lock);
}

static int mutex_lock(lua_State *L)
{
	mutex_ud *ud = mutex_box_check(L, 1);
	eli_mutex *mutex = ud->mutex;

	if (ud->held) {
		return luaL_error(L, "mutex is already locked by this thread");
	}
	mtx_lock(&mutex->lock);
	if (mutex->locked && mutex_owned_by_current(mutex)) {
		/* Held by this thread through another box; waiting would deadlock. */
		mtx_unlock(&mutex->lock);
		return luaL_error(L, "mutex is already locked by this thread");
	}
	while (mutex->locked) {
		cnd_wait(&mutex->cond, &mutex->lock);
	}
	mutex->locked = 1;
	mutex->owner = thrd_current();
	mtx_unlock(&mutex->lock);
	ud->held = 1;
	lua_pushboolean(L, 1);
	return 1;
}

static int mutex_try_lock(lua_State *L)
{
	mutex_ud *ud = mutex_box_check(L, 1);
	eli_mutex *mutex = ud->mutex;
	int acquired;

	mtx_lock(&mutex->lock);
	acquired = !mutex->locked;
	if (acquired) {
		mutex->locked = 1;
		mutex->owner = thrd_current();
	}
	mtx_unlock(&mutex->lock);
	if (acquired) {
		ud->held = 1;
	}
	lua_pushboolean(L, acquired);
	return 1;
}

static int mutex_unlock(lua_State *L)
{
	mutex_ud *ud = mutex_box_check(L, 1);

	if (!ud->held) {
		return luaL_error(L, "mutex is not locked through this handle");
	}
	/* held proves this thread acquired through this box, so the native
	 * owner check cannot refuse. */
	mutex_unlock_native(ud->mutex);
	ud->held = 0;
	lua_pushboolean(L, 1);
	return 1;
}

static int mutex_close(lua_State *L)
{
	mutex_ud *ud = (mutex_ud *)luaL_checkudata(L, 1, ELI_MUTEX_MT);
	eli_mutex *mutex = ud->mutex;

	ud->mutex = NULL;
	if (mutex == NULL) {
		return 0;
	}
	/* Serves both __gc and __close. A held box only becomes collectible once
	 * it is unreachable, so its owner can never unlock through it again;
	 * as __close it also ends the section at scope exit, including on error
	 * unwinding. Stranding the lock would deadlock every other state;
	 * releasing here also covers worker exit, main-state close, and handles
	 * dropped mid-section. */
	if (ud->held) {
		ud->held = 0;
		mutex_unlock_native(mutex);
	}
	mutex_release(mutex);
	return 0;
}

static int mutex_tostring(lua_State *L)
{
	mutex_ud *ud = (mutex_ud *)luaL_checkudata(L, 1, ELI_MUTEX_MT);
	lua_pushfstring(L, "worker.mutex(%p)", (void *)ud->mutex);
	return 1;
}

static const luaL_Reg mutex_methods[] = {
	{"lock", mutex_lock},
	{"try_lock", mutex_try_lock},
	{"unlock", mutex_unlock},
	{NULL, NULL},
};

int eli_mutex_push(lua_State *L, eli_mutex *mutex)
{
	mutex_ud *ud;

	eli_worker_install_mutex(L);
	ud = (mutex_ud *)lua_newuserdatauv(L, sizeof(*ud), 0);
	ud->mutex = NULL;
	ud->held = 0;
	luaL_setmetatable(L, ELI_MUTEX_MT);
	mutex_retain(mutex);
	ud->mutex = mutex;
	return 0;
}

int eli_mutex_create(lua_State *L)
{
	mutex_ud *ud;
	eli_mutex *mutex;

	eli_worker_install_mutex(L);
	ud = (mutex_ud *)lua_newuserdatauv(L, sizeof(*ud), 0);
	ud->mutex = NULL;
	ud->held = 0;
	luaL_setmetatable(L, ELI_MUTEX_MT);
	mutex = mutex_new();
	if (mutex == NULL) {
		return luaL_error(L, "out of memory");
	}
	/* The initial reference now belongs to this fully installed userdata. */
	ud->mutex = mutex;
	return 1;
}

eli_mutex *eli_mutex_check(lua_State *L, int index)
{
	return mutex_box_check(L, index)->mutex;
}

/* ---- adapter ---- */

static int mutex_export(lua_State *L, int index, void **data, size_t *size)
{
	eli_mutex *mutex = eli_mutex_check(L, index);
	/* the packet takes ownership of a new reference, released by
	 * mutex_cleanup on every cleanup path */
	mutex_retain(mutex);
	*data = mutex;
	*size = 0;
	return 0;
}

static int mutex_import(lua_State *L, const void *data, size_t size)
{
	(void)size;
	return eli_mutex_push(L, (eli_mutex *)data);
}

static void mutex_cleanup(void *data, size_t size)
{
	(void)size;
	mutex_release((eli_mutex *)data);
}

static const eli_xfer_adapter mutex_adapter = {
	ELI_XFER_API_VERSION,
	ELI_MUTEX_MT,
	mutex_export,
	mutex_import,
	mutex_cleanup,
};

void eli_worker_install_mutex(lua_State *L)
{
	/* See eli_worker_install_channel: never publish a partially built
	 * metatable, or a retry after an allocation failure would yield mutex
	 * userdata without a __gc. */
	lua_getfield(L, LUA_REGISTRYINDEX, ELI_MUTEX_MT);
	if (!lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return;
	}
	lua_pop(L, 1);

	lua_newtable(L);
	luaL_setfuncs(L, mutex_methods, 0);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, mutex_close);
	lua_setfield(L, -2, "__gc");
	lua_pushcfunction(L, mutex_close);
	lua_setfield(L, -2, "__close");
	lua_pushcfunction(L, mutex_tostring);
	lua_setfield(L, -2, "__tostring");
	lua_pushstring(L, "mutex");
	lua_setfield(L, -2, "__type");
	lua_pushstring(L, ELI_MUTEX_MT);
	lua_setfield(L, -2, "__name");
	eli_xfer_register(L, -1, &mutex_adapter);
	lua_setfield(L, LUA_REGISTRYINDEX, ELI_MUTEX_MT);
}
