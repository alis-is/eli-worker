#include "eli_worker_internal.h"

#include "eruntime.h"
#include "lauxlib.h"
#include "lenv.h"
#include "lualib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <pthread.h>
#include <signal.h>
#endif

#define ELI_LOCALE_ORIG "eli.worker.locale.orig"
#define ELI_LOCALE_INSTALLED "eli.worker.locale.installed"

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
	int done;
	int refs;
	thrd_t thread;
	char *error;
	eli_xfer_packet *results;
	eli_spawn_data *spawn;
};

static once_flag g_active_once = ONCE_FLAG_INIT;
static mtx_t g_active_lock;
static int g_active_workers = 0;

static void active_runtime_init(void)
{
	mtx_init(&g_active_lock, mtx_plain);
}

static void active_increment(void)
{
	call_once(&g_active_once, active_runtime_init);
	mtx_lock(&g_active_lock);
	g_active_workers++;
	mtx_unlock(&g_active_lock);
}

static void active_decrement(void)
{
	call_once(&g_active_once, active_runtime_init);
	mtx_lock(&g_active_lock);
	g_active_workers--;
	mtx_unlock(&g_active_lock);
}

int eli_worker_active_count(void)
{
	call_once(&g_active_once, active_runtime_init);
	return g_active_workers;
}

/* ---- locale guard ---- */

static int locale_guard(lua_State *L)
{
	int argument_count = lua_gettop(L);
	int i;

	if (!lua_isnoneornil(L, 1) && eli_worker_active_count() > 0) {
		return luaL_error(
		   L, "cannot change the locale while workers are active");
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
	lua_getfield(L, LUA_REGISTRYINDEX, ELI_LOCALE_INSTALLED);
	if (!lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return;
	}
	lua_pop(L, 1);

	lua_getglobal(L, "os");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return;
	}
	lua_getfield(L, -1, "setlocale");
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 2);
		return;
	}
	lua_setfield(L, LUA_REGISTRYINDEX, ELI_LOCALE_ORIG);
	lua_pushcfunction(L, locale_guard);
	lua_setfield(L, -2, "setlocale");
	lua_pushboolean(L, 1);
	lua_setfield(L, LUA_REGISTRYINDEX, ELI_LOCALE_INSTALLED);
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
}

static void worker_free(eli_worker *worker)
{
	if (worker == NULL) {
		return;
	}
	spawn_data_free(worker->spawn);
	eli_xfer_packet_free(worker->results);
	free(worker->error);
	cnd_destroy(&worker->cond);
	mtx_destroy(&worker->lock);
	free(worker);
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

static void setup_metadata_globals(lua_State *L, const eli_spawn_data *data,
				   char *error, size_t errlen)
{
	static const char *names[] = {
	   "arg",         "APP_ROOT",  "APP_ROOT_SCRIPT", "INTERPRETER",
	   "ELI_VERSION", "ELI_LIB_VERSION", NULL};
	int i;

	if (data->metadata == NULL) {
		return;
	}
	if (eli_xfer_decode(L, data->metadata, error, errlen) != 0) {
		return; /* reported by caller through stack error */
	}
	for (i = 0; names[i] != NULL; i++) {
		lua_getfield(L, -1, names[i]);
		if (lua_isnil(L, -1)) {
			lua_pop(L, 1);
			continue;
		}
		lua_setglobal(L, names[i]);
	}
	lua_pop(L, 1);
}

static void setup_environment(lua_State *L, const eli_spawn_data *data)
{
	char error[256];

	switch (data->kind) {
	case ELI_ENV_LUA:
		luaL_openselectedlibs(L, ~LUA_DBLIBK, LUA_DBLIBK);
		copy_package_paths(L, data);
		load_debug_library(L);
		install_exit_guard(L);
		break;
	case ELI_ENV_ELI: {
		luaL_openselectedlibs(L, ~LUA_DBLIBK, LUA_DBLIBK);
		copy_package_paths(L, data);
		load_debug_library(L);
		setup_metadata_globals(L, data, error, sizeof(error));
		if (error[0] != '\0') {
			luaL_error(L, "%s", error);
		}
		lua_getglobal(L, "require");
		lua_pushstring(L, "eli.elify");
		lua_call(L, 1, 1);
		lua_getfield(L, -1, "elify");
		lua_call(L, 0, 0);
		lua_pop(L, 1);
		install_exit_guard(L);
		break;
	}
	case ELI_ENV_EMPTY:
		lua_pushglobaltable(L);
		lua_pushvalue(L, -1);
		lua_setfield(L, -2, "_G");
		lua_pop(L, 1);
		break;
	case ELI_ENV_TABLE:
		if (data->env != NULL) {
			if (eli_xfer_decode(L, data->env, error, sizeof(error)) !=
			    0) {
				luaL_error(L, "%s", error);
			}
			lua_pushglobaltable(L);
			lua_pushnil(L);
			while (lua_next(L, -3) != 0) {
				lua_pushvalue(L, -2);
				lua_insert(L, -2);
				lua_rawset(L, -4);
			}
			lua_pop(L, 1); /* global table */
			lua_pop(L, 1); /* environment table */
		}
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
	return lua_gettop(L);
}

static void worker_run(eli_worker *worker)
{
	lua_State *L = luaL_newstate();
	char error[256];
	int status;
	int result_count;
	int i;

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
		lua_close(L);
		return;
	}
	lua_remove(L, 1); /* drop the message handler */

	result_count = lua_gettop(L);
	if (result_count > 0) {
		eli_xfer_packet *packet = eli_xfer_packet_new();
		if (packet == NULL) {
			set_worker_error(worker, "out of memory");
			lua_close(L);
			return;
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
				set_worker_error(worker, message);
				lua_close(L);
				return;
			}
		}
		worker->results = packet;
	}
	lua_close(L);
}

static void block_signals_in_worker(void)
{
#ifndef _WIN32
	/* Keep handled signals deliverable only on the main thread; signals
	 * raised by workers are process directed and land there. */
	sigset_t mask;
	sigfillset(&mask);
	pthread_sigmask(SIG_BLOCK, &mask, NULL);
#endif
}

static int worker_thread_main(void *argument)
{
	eli_worker *worker = (eli_worker *)argument;
	int free_now;

	block_signals_in_worker();
	worker_run(worker);
	active_decrement();

	mtx_lock(&worker->lock);
	worker->done = 1;
	cnd_broadcast(&worker->cond);
	worker->refs--;
	free_now = worker->refs <= 0;
	mtx_unlock(&worker->lock);
	if (free_now) {
		worker_free(worker);
	}
	return 0;
}

/* ---- Lua API ---- */

static int worker_channel(lua_State *L)
{
	lua_Integer capacity = 0;
	eli_channel *channel;

	if (!lua_isnoneornil(L, 1)) {
		capacity = luaL_checkinteger(L, 1);
		if (capacity < 0) {
			return luaL_argerror(
			   L, 1, "channel capacity must be non-negative");
		}
	}
	channel = eli_channel_new((size_t)capacity);
	if (channel == NULL) {
		return luaL_error(L, "out of memory");
	}
	eli_channel_push(L, channel);
	eli_channel_release(channel);
	return 1;
}

static int worker_active(lua_State *L)
{
	lua_pushinteger(L, eli_worker_active_count());
	return 1;
}

static void read_package_field(lua_State *L, const char *field,
			       char **destination)
{
	lua_getfield(L, -1, field);
	if (lua_isstring(L, -1)) {
		const char *value = lua_tostring(L, -1);
		*destination = strdup(value);
	}
	lua_pop(L, 1);
}

static int worker_spawn(lua_State *L)
{
	eli_spawn_data *data;
	eli_worker *worker;
	char error[256];
	const char *environment;
	int fn_index = 2;
	int env_index = 3;
	int args_index = 4;

	luaL_checktype(L, 1, LUA_TTABLE);
	lua_getfield(L, 1, "fn");
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 1);
		return luaL_error(L, "worker.spawn requires a 'fn' function");
	}

	lua_getfield(L, 1, "environment");
	if (!lua_isnil(L, -1) && !lua_isstring(L, -1) &&
	    !lua_istable(L, -1)) {
		return luaL_error(
		   L, "worker environment must be \"lua\", \"eli\", \"empty\", or a table");
	}
	environment = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
	if (environment != NULL && strcmp(environment, "lua") != 0 &&
	    strcmp(environment, "eli") != 0 &&
	    strcmp(environment, "empty") != 0) {
		return luaL_error(
		   L, "unknown worker environment '%s' (expected \"lua\", \"eli\", or \"empty\")",
		   environment);
	}

	lua_getfield(L, 1, "args");
	if (!lua_isnil(L, -1)) {
		luaL_checktype(L, -1, LUA_TTABLE);
	}

	data = (eli_spawn_data *)calloc(1, sizeof(*data));
	if (data == NULL) {
		return luaL_error(L, "out of memory");
	}
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
		spawn_data_free(data);
		return luaL_error(L, "out of memory");
	}
	if (eli_xfer_encode(L, data->fn, &fn_index, 1, error,
			    sizeof(error)) != 0) {
		spawn_data_free(data);
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
		for (i = 1; i <= count; i++) {
			int index;
			lua_rawgeti(L, args_index, i);
			index = lua_gettop(L);
			if (eli_xfer_encode(L, data->args, &index, 1, error,
					    sizeof(error)) != 0) {
				spawn_data_free(data);
				return luaL_error(L, "%s", error);
			}
			lua_pop(L, 1);
		}
	}

	if (data->kind == ELI_ENV_TABLE) {
		data->env = eli_xfer_packet_new();
		if (data->env == NULL) {
			spawn_data_free(data);
			return luaL_error(L, "out of memory");
		}
		if (eli_xfer_encode(L, data->env, &env_index, 1, error,
				    sizeof(error)) != 0) {
			spawn_data_free(data);
			return luaL_error(L, "%s", error);
		}
	}

	if (data->kind == ELI_ENV_ELI) {
		static const char *metadata_names[] = {
		   "arg",   "APP_ROOT", "APP_ROOT_SCRIPT", "INTERPRETER",
		   "ELI_VERSION", "ELI_LIB_VERSION", NULL};
		int i;
		int metadata_index;
		data->metadata = eli_xfer_packet_new();
		if (data->metadata == NULL) {
			spawn_data_free(data);
			return luaL_error(L, "out of memory");
		}
		lua_newtable(L);
		for (i = 0; metadata_names[i] != NULL; i++) {
			lua_getglobal(L, metadata_names[i]);
			if (lua_isnil(L, -1)) {
				lua_pop(L, 1);
				continue;
			}
			lua_setfield(L, -2, metadata_names[i]);
		}
		metadata_index = lua_gettop(L);
		if (eli_xfer_encode(L, data->metadata, &metadata_index, 1,
				    error, sizeof(error)) != 0) {
			spawn_data_free(data);
			return luaL_error(L, "%s", error);
		}
		lua_pop(L, 1);
	}

	lua_getglobal(L, "package");
	if (lua_istable(L, -1)) {
		read_package_field(L, "path", &data->path);
		read_package_field(L, "cpath", &data->cpath);
	}
	lua_pop(L, 1);

	worker = (eli_worker *)calloc(1, sizeof(*worker));
	if (worker == NULL) {
		spawn_data_free(data);
		return luaL_error(L, "out of memory");
	}
	if (mtx_init(&worker->lock, mtx_plain) != thrd_success ||
	    cnd_init(&worker->cond) != thrd_success) {
		cnd_destroy(&worker->cond);
		mtx_destroy(&worker->lock);
		free(worker);
		spawn_data_free(data);
		return luaL_error(L, "failed to initialize worker synchronization");
	}
	worker->spawn = data;
	worker->refs = 2; /* handle + thread */
	active_increment();
	if (thrd_create(&worker->thread, worker_thread_main, worker) !=
	    thrd_success) {
		active_decrement();
		worker->refs = 1;
		spawn_data_free(data);
		cnd_destroy(&worker->cond);
		mtx_destroy(&worker->lock);
		free(worker);
		lua_pushnil(L);
		lua_pushstring(L, "failed to start worker thread");
		return 2;
	}
	thrd_detach(worker->thread);

	{
		worker_ud *ud =
		   (worker_ud *)lua_newuserdatauv(L, sizeof(*ud), 0);
		ud->worker = worker;
		luaL_setmetatable(L, ELI_WORKER_MT);
	}
	return 1;
}

static int worker_join(lua_State *L)
{
	worker_ud *ud = (worker_ud *)luaL_checkudata(L, 1, ELI_WORKER_MT);
	eli_worker *worker = ud->worker;
	lua_Number timeout = -1;
	int timed_out = 0;
	char *message;
	eli_xfer_packet *results;

	if (!lua_isnoneornil(L, 2)) {
		timeout = luaL_checknumber(L, 2);
		if (timeout < 0) {
			return luaL_argerror(
			   L, 2, "timeout must be a non-negative number");
		}
	}

	mtx_lock(&worker->lock);
	if (!worker->done) {
		if (timeout == 0) {
			timed_out = 1;
		} else if (timeout < 0) {
			while (!worker->done) {
				cnd_wait(&worker->cond, &worker->lock);
			}
		} else {
			struct timespec deadline;
			timespec_get(&deadline, TIME_UTC);
			deadline.tv_sec += (time_t)((int64_t)timeout / 1000);
			deadline.tv_nsec +=
			   (long)(((int64_t)timeout % 1000) * 1000000L);
			if (deadline.tv_nsec >= 1000000000L) {
				deadline.tv_nsec -= 1000000000L;
				deadline.tv_sec += 1;
			}
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
	mtx_unlock(&worker->lock);

	if (message != NULL) {
		lua_pushboolean(L, 0);
		lua_pushstring(L, message);
		return 2;
	}
	lua_pushboolean(L, 1);
	if (results != NULL) {
		char error[256];
		if (eli_xfer_decode(L, results, error, sizeof(error)) != 0) {
			lua_settop(L, 0);
			lua_pushboolean(L, 0);
			lua_pushstring(L, error);
			return 2;
		}
	}
	return (int)eli_xfer_packet_roots(results) + 1;
}

static int worker_gc(lua_State *L)
{
	worker_ud *ud = (worker_ud *)luaL_checkudata(L, 1, ELI_WORKER_MT);
	eli_worker *worker = ud->worker;
	int free_now = 0;

	ud->worker = NULL;
	if (worker == NULL) {
		return 0;
	}
	mtx_lock(&worker->lock);
	worker->refs--;
	free_now = worker->refs <= 0;
	mtx_unlock(&worker->lock);
	if (free_now) {
		worker_free(worker);
	}
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
	if (luaL_newmetatable(L, ELI_WORKER_MT)) {
		luaL_setfuncs(L, worker_methods, 0);
		lua_pushvalue(L, -1);
		lua_setfield(L, -2, "__index");
		lua_pushcfunction(L, worker_gc);
		lua_setfield(L, -2, "__gc");
		lua_pushcfunction(L, worker_tostring);
		lua_setfield(L, -2, "__tostring");
		lua_pushstring(L, "worker");
		lua_setfield(L, -2, "__type");
	}
	lua_pop(L, 1);
}

int luaopen_eli_worker(lua_State *L);

int luaopen_eli_worker(lua_State *L)
{
	call_once(&g_active_once, active_runtime_init);
	eli_runtime_set_main_state(L);
	eli_worker_install_channel(L);
	eli_worker_install_handle(L);
	eli_worker_install_locale_guard(L);

	lua_newtable(L);
	lua_pushcfunction(L, worker_spawn);
	lua_setfield(L, -2, "spawn");
	lua_pushcfunction(L, worker_channel);
	lua_setfield(L, -2, "channel");
	lua_pushcfunction(L, worker_active);
	lua_setfield(L, -2, "active_count");
	{
		extern int luaopen_eli_worker_test(lua_State * L);
		lua_getglobal(L, "package");
		if (lua_istable(L, -1)) {
			lua_getfield(L, -1, "preload");
			if (lua_istable(L, -1)) {
				lua_pushcfunction(L, luaopen_eli_worker_test);
				lua_setfield(L, -2, "eli_worker.test");
			}
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
	}
	return 1;
}
