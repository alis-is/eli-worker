#include "eli_worker_internal.h"

#include "eruntime.h"
#include "lauxlib.h"
#include "lenv.h"
#include "los.h"
#include "lualib.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <pthread.h>
#include <signal.h>
#endif

#define ELI_LOCALE_ORIG "eli.worker.locale.orig"
/* The destination decodes every argument onto its Lua stack, which is capped
 * at LUAI_MAXSTACK (1,000,000); larger requests can never be called. */
#define ELI_WORKER_MAX_ARGS 1000000

int luaopen_eli_worker(lua_State *L);

typedef enum eli_env_kind {
	ELI_ENV_LUA = 0,
	ELI_ENV_ELI,
	ELI_ENV_EMPTY,
	ELI_ENV_TABLE
} eli_env_kind;

struct eli_spawn_data {
	eli_env_kind kind;
	eli_xfer_packet *fn;
	eli_xfer_packet *args;
	eli_xfer_packet *env;
	eli_xfer_packet *metadata;
	char *path;
	char *cpath;
};

typedef struct worker_ud {
	eli_worker *worker;
} worker_ud;

struct eli_worker {
	mtx_t lock;
	cnd_t cond;
	int started;
	int done;
	int failed;
	int refs;
	_Atomic int joinable;
	thrd_t thread;
	char *error;
	eli_xfer_packet *results;
	eli_spawn_data *spawn;
};

static once_flag g_active_once = ONCE_FLAG_INIT;
static mtx_t g_active_lock;
/* Non-zero while os.setlocale's Lua runs. Checked under g_active_lock so a
 * worker can never activate between the guard's check and its call. */
static _Atomic int g_locale_busy = 0;
static int g_active_workers = 0;
static int g_active_runtime_ok = 0;

/* Non-zero while this thread runs Lua inside the locale guard. Starting a
 * worker from that Lua is rejected so the caller gets a Lua error instead of
 * relying on the cross-thread g_locale_busy rejection. */
static thread_local int g_locale_depth = 0;

static void active_runtime_init(void)
{
	g_active_runtime_ok = mtx_init(&g_active_lock, mtx_plain) == thrd_success;
}

/* A failed initialization permanently completes the once_flag; every caller
 * must check before touching the lock. */
static int active_runtime_ready(void)
{
	call_once(&g_active_once, active_runtime_init);
	return g_active_runtime_ok;
}

/* Returns 1 on success, 0 when the runtime is unavailable, -1 while
 * os.setlocale's Lua is running. Never blocks on the locale phase: a hook
 * waiting on a thread that spawns must fail the spawn, not deadlock. */
static int active_increment(void)
{
	if (!active_runtime_ready()) {
		return 0;
	}
	mtx_lock(&g_active_lock);
	if (atomic_load(&g_locale_busy)) {
		mtx_unlock(&g_active_lock);
		return -1;
	}
	g_active_workers++;
	mtx_unlock(&g_active_lock);
	return 1;
}

static void active_decrement(void)
{
	if (!active_runtime_ready()) {
		return;
	}
	mtx_lock(&g_active_lock);
	g_active_workers--;
	mtx_unlock(&g_active_lock);
}

int eli_worker_active_count(void)
{
	int count;

	if (!active_runtime_ready()) {
		return 0;
	}
	mtx_lock(&g_active_lock);
	count = g_active_workers;
	mtx_unlock(&g_active_lock);
	return count;
}

/* ---- locale guard ---- */

static int locale_guard(lua_State *L)
{
	int argument_count = lua_gettop(L);
	int i;

	/* Every argument is forwarded, plus the original function. */
	luaL_checkstack(L, argument_count + 1, "os.setlocale arguments");

	if (!lua_isnoneornil(L, 1) && active_runtime_ready()) {
		int status;
		/* trylock: a debug hook or finalizer may re-enter os.setlocale
		 * while the original call is still running. */
		if (atomic_exchange(&g_locale_busy, 1)) {
			return luaL_error(
			   L, "another locale change is already in progress");
		}
		mtx_lock(&g_active_lock);
		if (g_active_workers > 0) {
			mtx_unlock(&g_active_lock);
			atomic_store(&g_locale_busy, 0);
			return luaL_error(
			   L, "cannot change the locale while workers are active");
		}
		mtx_unlock(&g_active_lock);
		lua_getfield(L, LUA_REGISTRYINDEX, ELI_LOCALE_ORIG);
		for (i = 1; i <= argument_count; i++) {
			lua_pushvalue(L, i);
		}
		/* Lua code run by the original setlocale (hooks, finalizers) may
		 * re-enter the worker API, so g_active_lock must not be held. */
		g_locale_depth++;
		status = lua_pcall(L, argument_count, LUA_MULTRET, 0);
		g_locale_depth--;
		atomic_store(&g_locale_busy, 0);
		if (status != LUA_OK) {
			return lua_error(L);
		}
		return lua_gettop(L) - argument_count;
	}
	lua_getfield(L, LUA_REGISTRYINDEX, ELI_LOCALE_ORIG);
	for (i = 1; i <= argument_count; i++) {
		lua_pushvalue(L, i);
	}
	lua_call(L, argument_count, LUA_MULTRET);
	return lua_gettop(L) - argument_count;
}

void eli_worker_install_locale_guard(lua_State *L)
{
	lua_getglobal(L, "os");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return;
	}
	lua_getfield(L, -1, "setlocale");
	/* Detect the guard directly instead of a separate marker: the marker
	 * store can fail after the guard is published, and a retry must not
	 * mistake the guard for the original function. */
	if (lua_tocfunction(L, -1) == locale_guard) {
		lua_pop(L, 2);
		return;
	}
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 2);
		return;
	}
	lua_pushvalue(L, -1);
	/* This store can raise before os.setlocale is touched. */
	lua_setfield(L, LUA_REGISTRYINDEX, ELI_LOCALE_ORIG);
	lua_pushcfunction(L, locale_guard);
	lua_replace(L, -2);
	lua_setfield(L, -2, "setlocale");
	lua_pop(L, 1);
}

/* ---- spawn data ---- */

static void spawn_data_free(eli_spawn_data *data)
{
	if (data == NULL) {
		return;
	}
	eli_xfer_packet_free(data->fn);
	eli_xfer_packet_free(data->args);
	eli_xfer_packet_free(data->env);
	eli_xfer_packet_free(data->metadata);
	free(data->path);
	free(data->cpath);
	free(data);
}

static void set_worker_error(eli_worker *worker, const char *message)
{
	free(worker->error);
	worker->error = message == NULL ? NULL : strdup(message);
	worker->failed = 1;
}

static void worker_free(eli_worker *worker)
{
	if (worker == NULL) {
		return;
	}
	if (worker->joinable) {
		/* thrd_detach failed in worker_spawn; reclaim the handle here. The
		 * worker thread can only be the caller in this case, and cannot
		 * join itself. Win32 thrd_t is the thread ID, so it compares with
		 * == directly; the bundled header has no thrd_equal there. */
#ifdef _WIN32
		if (thrd_current() == worker->thread) {
#else
		if (pthread_equal(thrd_current(), worker->thread)) {
#endif
			thrd_detach(worker->thread);
		} else {
			thrd_join(worker->thread, NULL);
		}
	}
	spawn_data_free(worker->spawn);
	eli_xfer_packet_free(worker->results);
	free(worker->error);
	cnd_destroy(&worker->cond);
	mtx_destroy(&worker->lock);
	free(worker);
}

static void worker_release(eli_worker *worker)
{
	int free_now;

	if (worker == NULL) {
		return;
	}
	mtx_lock(&worker->lock);
	free_now = --worker->refs <= 0;
	mtx_unlock(&worker->lock);
	if (free_now) {
		worker_free(worker);
	}
}

/* ---- environments ---- */

static int worker_os_exit(lua_State *L)
{
	return luaL_error(
	   L, "os.exit is not available inside a worker; return from the worker function instead");
}

static void install_exit_guard(lua_State *L)
{
	lua_getglobal(L, "os");
	if (lua_istable(L, -1)) {
		lua_pushcfunction(L, worker_os_exit);
		lua_setfield(L, -2, "exit");
	}
	lua_pop(L, 1);
}

static void copy_package_paths(lua_State *L, const eli_spawn_data *data)
{
	if (data->path == NULL && data->cpath == NULL) {
		return;
	}
	lua_getglobal(L, "package");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return;
	}
	if (data->path != NULL) {
		lua_pushstring(L, data->path);
		lua_setfield(L, -2, "path");
	}
	if (data->cpath != NULL) {
		lua_pushstring(L, data->cpath);
		lua_setfield(L, -2, "cpath");
	}
	lua_pop(L, 1);
}

static void load_debug_library(lua_State *L)
{
	luaL_requiref(L, LUA_DBLIBNAME, luaopen_debug, 0);
	lua_pop(L, 1);
}

/* Copied by worker_spawn_setup and restored by setup_metadata_globals; one
 * list so the read and write sides cannot drift. */
static const char *const eli_worker_metadata_names[] = {
	"arg",   "APP_ROOT", "APP_ROOT_SCRIPT", "INTERPRETER",
	"ELI_VERSION", "ELI_LIB_VERSION", NULL};

static void setup_metadata_globals(lua_State *L, const eli_spawn_data *data,
				   char *error, size_t errlen)
{
	int i;

	if (data->metadata == NULL) {
		return;
	}
	if (eli_xfer_decode(L, data->metadata, error, errlen) != 0) {
		return; /* reported by caller through stack error */
	}
	for (i = 0; eli_worker_metadata_names[i] != NULL; i++) {
		lua_getfield(L, -1, eli_worker_metadata_names[i]);
		if (lua_isnil(L, -1)) {
			lua_pop(L, 1);
			continue;
		}
		lua_setglobal(L, eli_worker_metadata_names[i]);
	}
	lua_pop(L, 1);
}

static int open_library_set(lua_State *L)
{
	luaL_openselectedlibs(L, ~LUA_DBLIBK, LUA_DBLIBK);
	return 0;
}

static int open_environment_libraries(lua_State *L)
{
	luaL_requiref(L, LUA_LOADLIBNAME, luaopen_package, 1);
	lua_pop(L, 1);
	luaL_requiref(L, LUA_OSLIBNAME, luaopen_os, 1);
	lua_pop(L, 1);
	return 0;
}

static void open_standard_libraries(lua_State *L)
{
	int status;

	/* luaL_openselectedlibs runs the embedded Lua loader, so it must not
	 * hold the environment lock. Open package (raw getenv) and os under the
	 * lock, install the getenv guard, then run the Lua part outside. */
	eli_env_lock();
	lua_pushcfunction(L, open_environment_libraries);
	status = lua_pcall(L, 0, 0, 0);
	eli_env_unlock();
	if (status != LUA_OK) {
		lua_error(L);
	}
	eli_env_install(L);
	luaL_requiref(L, "eli.os.extra", luaopen_eli_os_extra, 0);
	lua_pop(L, 1);
	lua_pushcfunction(L, open_library_set);
	status = lua_pcall(L, 0, 0, 0);
	if (status != LUA_OK) {
		lua_error(L);
	}
}

static void setup_environment(lua_State *L, const eli_spawn_data *data)
{
	char error[256] = {0};

	switch (data->kind) {
	case ELI_ENV_LUA:
		open_standard_libraries(L);
		copy_package_paths(L, data);
		load_debug_library(L);
		install_exit_guard(L);
		break;
	case ELI_ENV_ELI: {
		open_standard_libraries(L);
		copy_package_paths(L, data);
		load_debug_library(L);
		install_exit_guard(L);
		setup_metadata_globals(L, data, error, sizeof(error));
		if (error[0] != '\0') {
			luaL_error(L, "%s", error);
		}
		/* Register worker preloads before elify iterates package.preload. */
		luaL_requiref(L, "eli.worker", luaopen_eli_worker, 0);
		lua_pop(L, 1);
		lua_getglobal(L, "require");
		lua_pushstring(L, "eli.elify");
		lua_call(L, 1, 1);
		lua_getfield(L, -1, "elify");
		lua_call(L, 0, 0);
		lua_pop(L, 1);
		break;
	}
	case ELI_ENV_EMPTY:
		lua_pushglobaltable(L);
		lua_pushvalue(L, -1);
		lua_setfield(L, -2, "_G");
		lua_pop(L, 1);
		break;
	case ELI_ENV_TABLE:
		lua_pushglobaltable(L);
		if (data->env != NULL &&
		    eli_xfer_decode_into(L, data->env, -1, error,
					 sizeof(error)) != 0) {
			luaL_error(L, "%s", error);
		}
		lua_pop(L, 1); /* global table */
		lua_pushglobaltable(L);
		lua_pushvalue(L, -1);
		lua_setfield(L, -2, "_G");
		lua_pop(L, 1);
		install_exit_guard(L);
		break;
	}
	eli_env_install(L);
	eli_worker_install_locale_guard(L);
}

/* ---- worker thread ---- */

static int worker_traceback(lua_State *L)
{
	if (lua_isstring(L, 1)) {
		luaL_traceback(L, L, lua_tostring(L, 1), 1);
	} else {
		luaL_tolstring(L, 1, NULL);
		luaL_traceback(L, L, lua_tostring(L, -1), 1);
	}
	return 1;
}

static int worker_entry(lua_State *L)
{
	eli_worker *worker = (eli_worker *)lua_touserdata(L, 1);
	eli_spawn_data *data = worker->spawn;
	char error[256];
	size_t argument_count;

	eli_runtime_set_worker_state(L);
	setup_environment(L, data);

	lua_remove(L, 1);
	if (eli_xfer_decode(L, data->fn, error, sizeof(error)) != 0) {
		return luaL_error(L, "%s", error);
	}
	argument_count = eli_xfer_packet_roots(data->args);
	if (argument_count > 0) {
		if (eli_xfer_decode(L, data->args, error, sizeof(error)) != 0) {
			return luaL_error(L, "%s", error);
		}
	}

	worker->spawn = NULL;
	spawn_data_free(data);

	lua_call(L, (int)argument_count, LUA_MULTRET);
	{
		int result_count = lua_gettop(L);
		int i;

		if (result_count > 0) {
			eli_xfer_packet *packet = eli_xfer_packet_new();
			if (packet == NULL) {
				return luaL_error(L, "out of memory");
			}
			for (i = 1; i <= result_count; i++) {
				int index = i;
				if (eli_xfer_encode(L, packet, &index, 1, error,
						    sizeof(error)) != 0) {
					char message[320];
					snprintf(message, sizeof(message),
						 "cannot transfer worker result: %s",
						 error);
					eli_xfer_packet_free(packet);
					return luaL_error(L, "%s", message);
				}
			}
			eli_xfer_encode_finish(packet);
			worker->results = packet;
		}
	}
	return 0;
}

static void worker_run(eli_worker *worker)
{
	lua_State *L = luaL_newstate();
	int status;

	if (L == NULL) {
		set_worker_error(worker, "failed to create worker Lua state");
		return;
	}
	lua_pushcfunction(L, worker_traceback);
	lua_pushcfunction(L, worker_entry);
	lua_pushlightuserdata(L, worker);
	status = lua_pcall(L, 1, LUA_MULTRET, 1);
	if (status != LUA_OK) {
		const char *message = lua_tostring(L, -1);
		set_worker_error(worker,
				 message != NULL ? message : "worker failed");
	}
	lua_close(L);
}

#ifndef _WIN32
static void worker_signal_mask(sigset_t *mask)
{
	/* Block asynchronous signals before creation, leaving synchronous faults
	 * available for the platform's normal crash handling. */
	sigfillset(mask);
	sigdelset(mask, SIGSEGV);
	sigdelset(mask, SIGBUS);
	sigdelset(mask, SIGFPE);
	sigdelset(mask, SIGILL);
	sigdelset(mask, SIGABRT);
#ifdef SIGTRAP
	sigdelset(mask, SIGTRAP);
#endif
#ifdef SIGSYS
	sigdelset(mask, SIGSYS);
#endif
}
#endif

static int worker_thread_main(void *argument)
{
	eli_worker *worker = (eli_worker *)argument;

	worker_run(worker);
	active_decrement();

	mtx_lock(&worker->lock);
	worker->done = 1;
	cnd_broadcast(&worker->cond);
	mtx_unlock(&worker->lock);
	worker_release(worker);
	return 0;
}

/* ---- Lua API ---- */

static int worker_channel(lua_State *L)
{
	lua_Integer capacity = 0;

	if (!lua_isnoneornil(L, 1)) {
		capacity = luaL_checkinteger(L, 1);
		if (capacity < 0) {
			return luaL_argerror(
			   L, 1, "channel capacity must be non-negative");
		}
		if ((lua_Unsigned)capacity >
		    (lua_Unsigned)(SIZE_MAX / sizeof(eli_xfer_packet *))) {
			return luaL_argerror(L, 1,
					     "channel capacity is too large");
		}
	}
	return eli_channel_create(L, (size_t)capacity);
}

static int worker_mutex(lua_State *L)
{
	return eli_mutex_create(L);
}

static int worker_active(lua_State *L)
{
	lua_pushinteger(L, eli_worker_active_count());
	return 1;
}

static int read_package_field(lua_State *L, const char *field,
			       char **destination)
{
	lua_getfield(L, -1, field);
	if (lua_isstring(L, -1)) {
		const char *value = lua_tostring(L, -1);
		*destination = strdup(value);
		if (*destination == NULL) {
			lua_pop(L, 1);
			return 0;
		}
	}
	lua_pop(L, 1);
	return 1;
}

static int worker_spawn_setup(lua_State *L)
{
	eli_spawn_data *data;
	eli_worker *worker = (eli_worker *)lua_touserdata(L, 6);
	char error[256];
	const char *environment = lua_isstring(L, 3) ? lua_tostring(L, 3) : NULL;
	int fn_index = 2;
	int env_index = 3;
	int args_index = 4;

	data = (eli_spawn_data *)calloc(1, sizeof(*data));
	if (data == NULL) {
		return luaL_error(L, "out of memory");
	}
	worker->spawn = data;
	if (lua_istable(L, env_index)) {
		data->kind = ELI_ENV_TABLE;
	} else if (environment == NULL || strcmp(environment, "lua") == 0) {
		data->kind = ELI_ENV_LUA;
	} else if (strcmp(environment, "eli") == 0) {
		data->kind = ELI_ENV_ELI;
	} else {
		data->kind = ELI_ENV_EMPTY;
	}

	data->fn = eli_xfer_packet_new();
	data->args = eli_xfer_packet_new();
	if (data->fn == NULL || data->args == NULL) {
		return luaL_error(L, "out of memory");
	}
	if (eli_xfer_encode(L, data->fn, &fn_index, 1, error,
			    sizeof(error)) != 0) {
		return luaL_error(L, "%s", error);
	}

	if (lua_istable(L, args_index)) {
		lua_Integer count;
		lua_Integer i;
		lua_getfield(L, args_index, "n");
		if (lua_isinteger(L, -1)) {
			count = lua_tointeger(L, -1);
			if (count < 0) {
				count = 0;
			}
		} else {
			count = (lua_Integer)lua_rawlen(L, args_index);
		}
		lua_pop(L, 1);
		if (count > ELI_WORKER_MAX_ARGS) {
			return luaL_error(L, "worker argument count is too large");
		}
		for (i = 1; i <= count; i++) {
			int index;
			lua_rawgeti(L, args_index, i);
			index = lua_gettop(L);
			if (eli_xfer_encode(L, data->args, &index, 1, error,
					    sizeof(error)) != 0) {
				return luaL_error(L, "%s", error);
			}
			lua_pop(L, 1);
		}
	}

	if (data->kind == ELI_ENV_TABLE) {
		data->env = eli_xfer_packet_new();
		if (data->env == NULL) {
			return luaL_error(L, "out of memory");
		}
		if (eli_xfer_encode(L, data->env, &env_index, 1, error,
				    sizeof(error)) != 0) {
			return luaL_error(L, "%s", error);
		}
	}

	if (data->kind == ELI_ENV_ELI) {
		int i;
		int metadata_index;
		data->metadata = eli_xfer_packet_new();
		if (data->metadata == NULL) {
			return luaL_error(L, "out of memory");
		}
		lua_newtable(L);
		for (i = 0; eli_worker_metadata_names[i] != NULL; i++) {
			lua_getglobal(L, eli_worker_metadata_names[i]);
			if (lua_isnil(L, -1)) {
				lua_pop(L, 1);
				continue;
			}
			lua_setfield(L, -2, eli_worker_metadata_names[i]);
		}
		metadata_index = lua_gettop(L);
		if (eli_xfer_encode(L, data->metadata, &metadata_index, 1,
				    error, sizeof(error)) != 0) {
			return luaL_error(L, "%s", error);
		}
		lua_pop(L, 1);
	}

	lua_getglobal(L, "package");
	if (lua_istable(L, -1)) {
		if (!read_package_field(L, "path", &data->path) ||
		    !read_package_field(L, "cpath", &data->cpath)) {
			lua_pop(L, 1);
			return luaL_error(L, "out of memory");
		}
	}
	lua_pop(L, 1);

	eli_xfer_encode_finish(data->fn);
	eli_xfer_encode_finish(data->args);
	eli_xfer_encode_finish(data->env);
	eli_xfer_encode_finish(data->metadata);
	return 0;
}

static int worker_spawn(lua_State *L)
{
	eli_worker *worker;
	worker_ud *ud;
	const char *environment;
	int start_status;
	int i;
#ifndef _WIN32
	sigset_t mask, old_mask;
#endif

	luaL_checktype(L, 1, LUA_TTABLE);
	if (lua_gettop(L) != 1) {
		return luaL_error(L, "worker.spawn expects a single options table");
	}
	lua_getfield(L, 1, "fn");
	if (!lua_isfunction(L, -1)) {
		return luaL_error(L, "worker.spawn requires a 'fn' function");
	}
	lua_getfield(L, 1, "environment");
	if (!lua_isnil(L, -1) && !lua_isstring(L, -1) && !lua_istable(L, -1)) {
		return luaL_error(L, "worker environment must be \"lua\", \"eli\", \"empty\", or a table");
	}
	environment = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
	if (environment != NULL && strcmp(environment, "lua") != 0 &&
	    strcmp(environment, "eli") != 0 && strcmp(environment, "empty") != 0) {
		return luaL_error(L,
		   "unknown worker environment '%s' (expected \"lua\", \"eli\", or \"empty\")",
		   environment);
	}
	lua_getfield(L, 1, "args");
	if (!lua_isnil(L, -1)) {
		luaL_checktype(L, -1, LUA_TTABLE);
	}
	/* Reserve setup arguments before the handle becomes visible to finalizers. */
	luaL_checkstack(L, 8, "worker setup");
	ud = (worker_ud *)lua_newuserdatauv(L, sizeof(*ud), 0);
	ud->worker = NULL;
	luaL_setmetatable(L, ELI_WORKER_MT);
	worker = (eli_worker *)calloc(1, sizeof(*worker));
	if (worker == NULL) {
		return luaL_error(L, "out of memory");
	}
	if (mtx_init(&worker->lock, mtx_plain) != thrd_success) {
		free(worker);
		return luaL_error(L, "failed to initialize worker synchronization");
	}
	if (cnd_init(&worker->cond) != thrd_success) {
		mtx_destroy(&worker->lock);
		free(worker);
		return luaL_error(L, "failed to initialize worker synchronization");
	}
	worker->refs = 2; /* handle + independent constructor ownership */
	ud->worker = worker;
	lua_pushcfunction(L, worker_spawn_setup);
	for (i = 1; i <= 5; i++) {
		lua_pushvalue(L, i);
	}
	lua_pushlightuserdata(L, worker);
	start_status = lua_pcall(L, 6, 0, 0);
	if (start_status != LUA_OK || ud->worker == NULL) {
		/* A NULL handle means a finalizer already released the handle
		 * reference, so only the constructor reference remains. */
		if (ud->worker != NULL) {
			ud->worker = NULL;
			worker_release(worker);
		}
		worker_release(worker);
		if (start_status != LUA_OK) {
			return lua_error(L);
		}
		return luaL_error(L, "attempt to use a finalized worker handle");
	}

	/* Spawning from a hook running inside os.setlocale is rejected here
	 * with a precise message; the signal mask has not been touched yet. */
	if (g_locale_depth > 0) {
		ud->worker = NULL;
		worker_free(worker);
		lua_pushnil(L);
		lua_pushliteral(
		   L, "cannot start a worker from inside os.setlocale");
		return 2;
	}

#ifndef _WIN32
	worker_signal_mask(&mask);
	start_status = pthread_sigmask(SIG_BLOCK, &mask, &old_mask);
	if (start_status != 0) {
		ud->worker = NULL;
		worker_free(worker);
		lua_pushnil(L);
		lua_pushliteral(L, "failed to block signals before starting worker");
		return 2;
	}
#endif
	/* No more Lua calls: constructor ownership passes to the new thread. */
	start_status = active_increment();
	if (start_status <= 0) {
#ifndef _WIN32
		pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
#endif
		ud->worker = NULL;
		worker_free(worker);
		lua_pushnil(L);
		if (start_status < 0) {
			lua_pushliteral(
			   L, "cannot start a worker while the locale is changing");
		} else {
			lua_pushliteral(L, "failed to initialize worker runtime");
		}
		return 2;
	}
	start_status = thrd_create(&worker->thread, worker_thread_main, worker);
#ifndef _WIN32
	/* The new thread inherits the blocked mask from its first instruction. */
	pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
#endif
	if (start_status != thrd_success) {
		active_decrement();
		ud->worker = NULL;
		worker_free(worker);
		lua_pushnil(L);
		lua_pushliteral(L, "failed to start worker thread");
		return 2;
	}
	mtx_lock(&worker->lock);
	worker->started = 1;
	mtx_unlock(&worker->lock);
	if (thrd_detach(worker->thread) != thrd_success) {
		/* Keep the handle joinable: reclaim it in worker_free instead of
		 * leaking the thread handle. */
		worker->joinable = 1;
	}
	return 1;
}

typedef struct join_decode {
	eli_xfer_packet *packet;
	const char *message;
	char *error;
	size_t error_size;
} join_decode;

static int decode_join_results(lua_State *L)
{
	join_decode *guard = (join_decode *)lua_touserdata(L, 1);
	int top = lua_gettop(L);

	if (guard->message != NULL) {
		lua_pushstring(L, guard->message);
		return 1;
	}
	if (eli_xfer_decode(L, guard->packet, guard->error,
			    guard->error_size) != 0) {
		return luaL_error(L, "%s", guard->error);
	}
	return lua_gettop(L) - top;
}

static int worker_join(lua_State *L)
{
	worker_ud *ud = (worker_ud *)luaL_checkudata(L, 1, ELI_WORKER_MT);
	eli_worker *worker = ud->worker;
	int64_t timeout = eli_worker_check_timeout(L, 2, -1);
	int argument_count = lua_gettop(L);
	int timed_out = 0;
	int failed;
	char *message;
	eli_xfer_packet *results;

	if (worker == NULL) {
		return luaL_error(L, "attempt to use a finalized worker handle");
	}
	mtx_lock(&worker->lock);
	if (!worker->started && !worker->done) {
		/* The handle is visible to constructor callbacks before thrd_create;
		 * waiting would deadlock on a thread that cannot start yet. A done
		 * worker is joinable even if the caller never observed the start. */
		mtx_unlock(&worker->lock);
		return luaL_error(L, "worker thread has not started yet");
	}
	if (!worker->done) {
		if (timeout == 0) {
			timed_out = 1;
		} else if (timeout < 0) {
			while (!worker->done) {
				cnd_wait(&worker->cond, &worker->lock);
			}
		} else {
			struct timespec deadline;
			eli_worker_deadline(timeout, &deadline);
			while (!worker->done) {
				if (cnd_timedwait(&worker->cond, &worker->lock,
						  &deadline) != thrd_success) {
					break;
				}
			}
			if (!worker->done) {
				timed_out = 1;
			}
		}
	}
	if (timed_out) {
		mtx_unlock(&worker->lock);
		lua_pushboolean(L, 0);
		lua_pushstring(L, "timeout");
		return 2;
	}
	message = worker->error;
	results = worker->results;
	failed = worker->failed;
	/* Decoding and copying errors can run finalizers or raise. Protect both
	 * while retaining the borrowed packet/error independently of the handle. */
	worker->refs++;
	mtx_unlock(&worker->lock);

	lua_pushboolean(L, !failed);
	if (failed || results != NULL) {
		join_decode guard;
		char error[256];
		int status;

		guard.packet = results;
		guard.message = failed ? (message != NULL ? message : "worker failed") : NULL;
		guard.error = error;
		guard.error_size = sizeof(error);
		lua_pushcfunction(L, decode_join_results);
		lua_pushlightuserdata(L, &guard);
		status = lua_pcall(L, 1, LUA_MULTRET, 0);
		if (status != LUA_OK) {
			worker_release(worker);
			lua_pushboolean(L, 0);
			if (lua_type(L, -2) == LUA_TSTRING) {
				/* Reuse even long errors: copying can allocate and raise. */
				lua_pushvalue(L, -2);
			} else {
				lua_pushliteral(L, "cannot decode worker results");
			}
			return 2;
		}
	}
	worker_release(worker);
	return lua_gettop(L) - argument_count;
}

static int worker_gc(lua_State *L)
{
	worker_ud *ud = (worker_ud *)luaL_checkudata(L, 1, ELI_WORKER_MT);
	eli_worker *worker = ud->worker;

	ud->worker = NULL;
	worker_release(worker);
	return 0;
}

static int worker_tostring(lua_State *L)
{
	worker_ud *ud = (worker_ud *)luaL_checkudata(L, 1, ELI_WORKER_MT);
	lua_pushfstring(L, "worker.handle(%p)", (void *)ud->worker);
	return 1;
}

static const luaL_Reg worker_methods[] = {
	{"join", worker_join},
	{NULL, NULL},
};

void eli_worker_install_handle(lua_State *L)
{
	/* See eli_worker_install_channel: never publish a partially built
	 * metatable, or a retry after an allocation failure would yield worker
	 * handles without a __gc. */
	lua_getfield(L, LUA_REGISTRYINDEX, ELI_WORKER_MT);
	if (!lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return;
	}
	lua_pop(L, 1);

	lua_newtable(L);
	luaL_setfuncs(L, worker_methods, 0);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, worker_gc);
	lua_setfield(L, -2, "__gc");
	lua_pushcfunction(L, worker_tostring);
	lua_setfield(L, -2, "__tostring");
	lua_pushstring(L, "worker");
	lua_setfield(L, -2, "__type");
	lua_pushstring(L, ELI_WORKER_MT);
	lua_setfield(L, -2, "__name");
	lua_setfield(L, LUA_REGISTRYINDEX, ELI_WORKER_MT);
}

int luaopen_eli_worker(lua_State *L)
{
	if (!active_runtime_ready()) {
		return luaL_error(L, "failed to initialize worker runtime");
	}
	eli_runtime_set_main_state(L);
	eli_worker_install_channel(L);
	eli_worker_install_mutex(L);
	eli_worker_install_handle(L);
	eli_worker_install_locale_guard(L);

	lua_newtable(L);
	lua_pushcfunction(L, worker_spawn);
	lua_setfield(L, -2, "spawn");
	lua_pushcfunction(L, worker_channel);
	lua_setfield(L, -2, "channel");
	lua_pushcfunction(L, worker_mutex);
	lua_setfield(L, -2, "mutex");
	lua_pushcfunction(L, worker_active);
	lua_setfield(L, -2, "active_count");
#ifdef ELI_WORKER_TESTS
	{
		extern int luaopen_eli_worker_test(lua_State * L);
		lua_getglobal(L, "package");
		if (lua_istable(L, -1)) {
			lua_getfield(L, -1, "preload");
			if (lua_istable(L, -1)) {
				lua_pushcfunction(L, luaopen_eli_worker_test);
				lua_setfield(L, -2, "eli.worker.test");
			}
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
	}
#endif
	return 1;
}
