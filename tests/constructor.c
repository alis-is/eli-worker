/* White-box regressions for worker construction.  Build with ASan/LSan: the
 * failed allocation case used to leave native worker ownership unreachable. */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "lua.h"
#include "lauxlib.h"
#include "lobject.h" /* LUA_TPROTO: target compilation, not library setup */
#include "c11threads.h"

/* Fault-inject the process-global runtime mutex initialization. The failing
 * case runs in a forked child so this process's once state stays usable. */
static int fail_mtx_init;
static int eli_test_mtx_init(mtx_t *mtx, int type)
{
	if (fail_mtx_init) {
		return thrd_error;
	}
	return mtx_init(mtx, type);
}
#define mtx_init eli_test_mtx_init

/* Keep the test coupled to the constructor sequence it protects. */
#include "../eli_worker.c"
#undef mtx_init

typedef struct fail_alloc {
	lua_Alloc original;
	void *original_ud;
	int fail_after;
} fail_alloc;

static void *fail_after(void *ud, void *ptr, size_t old_size, size_t size)
{
	fail_alloc *state = (fail_alloc *)ud;

	if (size != 0 && (ptr == NULL || size > old_size) &&
	    state->fail_after-- <= 0) {
		return NULL;
	}
	return state->original(state->original_ud, ptr, old_size, size);
}

static void push_options(lua_State *L)
{
	assert(luaL_loadstring(L, "return function() return 42 end") == LUA_OK);
	assert(lua_pcall(L, 0, 1, 0) == LUA_OK);
	lua_createtable(L, 0, 2);
	lua_insert(L, -2);
	lua_setfield(L, -2, "fn");
	lua_pushliteral(L, "empty");
	lua_setfield(L, -2, "environment");
}

static int spawn(lua_State *L, int fail_at, int results)
{
	fail_alloc state;
	void *ud;
	int status;

	push_options(L);
	lua_pushcfunction(L, worker_spawn);
	lua_insert(L, -2);
	assert(lua_checkstack(L, 2));
	if (fail_at < 0) {
		return lua_pcall(L, 1, results, 0);
	}
	state.original = lua_getallocf(L, &ud);
	state.original_ud = ud;
	state.fail_after = fail_at;
	lua_setallocf(L, fail_after, &state);
	status = lua_pcall(L, 1, results, 0);
	lua_setallocf(L, state.original, state.original_ud);
	return status;
}

#ifndef _WIN32
/* Initialization failure permanently completes the once_flag, so the failing
 * case must not share the parent's runtime state. */
static void test_failed_runtime_init(void)
{
	pid_t pid;
	int status = 0;

	fail_mtx_init = 1;
	pid = fork();
	assert(pid >= 0);
	if (pid == 0) {
		lua_State *L = luaL_newstate();

		if (L == NULL) {
			_exit(2);
		}
		lua_pushcfunction(L, luaopen_eli_worker);
		if (lua_pcall(L, 0, 1, 0) != LUA_ERRRUN) {
			_exit(3);
		}
		if (eli_worker_active_count() != 0) {
			_exit(4);
		}
		if (active_increment()) {
			_exit(5);
		}
		_exit(0);
	}
	assert(waitpid(pid, &status, 0) == pid);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	fail_mtx_init = 0;
}
#endif

/* While os.setlocale's Lua is running, activation must fail fast instead of
 * blocking on a lock that Lua may wait on. */
static void test_activation_rejected_while_locale_busy(void)
{
	assert(active_increment() == 1);
	active_decrement();
	atomic_store(&g_locale_busy, 1);
	assert(active_increment() == -1);
	assert(eli_worker_active_count() == 0);
	atomic_store(&g_locale_busy, 0);
	assert(active_increment() == 1);
	active_decrement();
}

static void test_main_state_lifecycle(void)
{
	lua_State *first = luaL_newstate();
	lua_State *second;
	lua_State *loser;

	assert(first != NULL);
	assert(luaopen_eli_worker(first) == 1);
	lua_pop(first, 1);
	assert(eli_runtime_is_main_state(first));

	/* A rejected claim's sentinel must not free the role when collected. */
	loser = luaL_newstate();
	assert(loser != NULL);
	eli_runtime_set_main_state(loser);
	assert(!eli_runtime_is_main_state(loser));
	lua_gc(loser, LUA_GCCOLLECT, 0);
	lua_gc(loser, LUA_GCCOLLECT, 0);
	assert(eli_runtime_is_main_state(first));
	lua_close(loser);

	/* closing the owning state must release the role */
	lua_close(first);

	second = luaL_newstate();
	assert(second != NULL);
	assert(luaopen_eli_worker(second) == 1);
	lua_pop(second, 1);
	assert(eli_runtime_is_main_state(second));
	lua_close(second);
}

static void test_worker_main_state_takeover(void)
{
	lua_State *L = luaL_newstate();
	lua_State *replacement;
	eli_channel *gate = eli_channel_new(0);
	eli_worker *worker;
	struct timespec deadline;

	assert(L != NULL && gate != NULL);
	assert(luaopen_eli_worker(L) == 1);
	lua_pop(L, 1);
	assert(eli_runtime_is_main_state(L));
	assert(luaL_loadstring(L,
	   "return function(gate) "
	   "gate:receive() "
	   "package.loaded['eli.worker'] = nil "
	   "require'eli.worker' "
	   "local signal = require'eli.os.extra'.signal "
	   "local ok, err = pcall(signal.poll, 2000) "
	   "assert(not ok and err:find('main state', 1, true), tostring(err)) "
	   "end") == LUA_OK);
	assert(lua_pcall(L, 0, 1, 0) == LUA_OK);
	lua_createtable(L, 0, 2);
	lua_insert(L, -2);
	lua_setfield(L, -2, "fn");
	lua_createtable(L, 1, 0);
	eli_channel_push(L, gate);
	lua_rawseti(L, -2, 1);
	lua_setfield(L, -2, "args");
	lua_pushcfunction(L, worker_spawn);
	lua_insert(L, -2);
	assert(lua_pcall(L, 1, 1, 0) == LUA_OK);
	worker = ((worker_ud *)luaL_checkudata(L, -1, ELI_WORKER_MT))->worker;
	mtx_lock(&worker->lock);
	worker->refs++; /* Native test ownership survives parent handle teardown. */
	mtx_unlock(&worker->lock);
	lua_close(L);
	eli_channel_close(gate); /* Only now may the detached child load modules. */
	eli_channel_release(gate);

	eli_worker_deadline(10000, &deadline);
	mtx_lock(&worker->lock);
	while (!worker->done) {
		assert(cnd_timedwait(&worker->cond, &worker->lock, &deadline) == thrd_success);
	}
	if (worker->failed) {
		fprintf(stderr, "%s\n", worker->error);
	}
	assert(!worker->failed);
	mtx_unlock(&worker->lock);
	worker_release(worker);

	/* All registration callers, including coroutines, share the exclusion. */
	L = luaL_newstate();
	assert(L != NULL);
	eli_runtime_set_worker_state(L);
	eli_runtime_set_main_state(lua_newthread(L));
	assert(!eli_runtime_is_main_state(L));
	replacement = luaL_newstate();
	assert(replacement != NULL);
	eli_runtime_set_main_state(replacement);
	assert(eli_runtime_is_main_state(replacement));
	lua_close(L);
	assert(eli_runtime_is_main_state(replacement));
	lua_close(replacement);
}

static void test_failed_userdata_allocation(void)
{
	lua_State *L = luaL_newstate();

	assert(L != NULL);
	assert(luaopen_eli_worker(L) == 1);
	lua_pop(L, 1);
	assert(spawn(L, 0, 1) == LUA_ERRMEM);
	lua_settop(L, 0);
	lua_gc(L, LUA_GCCOLLECT, 0);
	assert(eli_worker_active_count() == 0);
	lua_close(L);
}

static void *fail_proto(void *ud, void *ptr, size_t old_size, size_t size)
{
	fail_alloc *state = (fail_alloc *)ud;

	/* Deny retries too. The embedded preload chunk is the first Lua code
	 * compiled by setup_environment; opening the C libraries cannot hit this. */
	if (ptr == NULL && size != 0 && old_size == LUA_TPROTO) {
		state->fail_after++;
		return NULL;
	}
	return state->original(state->original_ud, ptr, old_size, size);
}

static int setup_test_environment(lua_State *L)
{
	eli_spawn_data data = {0};
	data.kind = (eli_env_kind)lua_tointeger(L, 1);
	lua_settop(L, 0);
	setup_environment(L, &data);
	return 0;
}

static void test_embedded_loader_allocation_failure(void)
{
	const int kinds[] = {ELI_ENV_LUA, ELI_ENV_ELI};
	size_t i;

	for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
		lua_State *L = luaL_newstate();
		fail_alloc state;
		int status;
		assert(L != NULL);
		lua_pushcfunction(L, setup_test_environment);
		lua_pushinteger(L, kinds[i]);
		assert(lua_checkstack(L, 2));
		state.original = lua_getallocf(L, &state.original_ud);
		state.fail_after = 0;
		lua_setallocf(L, fail_proto, &state);
		status = lua_pcall(L, 1, 0, 0);
		lua_setallocf(L, state.original, state.original_ud);
		assert(state.fail_after > 0); /* must reach luaL_loadbuffer */
		assert(status == LUA_ERRMEM);
		assert(lua_isstring(L, -1));
		lua_settop(L, 0);
		/* Retry through the same environment lock and loader after recovery. */
		lua_pushcfunction(L, setup_test_environment);
		lua_pushinteger(L, kinds[i]);
		assert(lua_pcall(L, 1, 0, 0) == LUA_OK);
		lua_close(L);
	}
}

static void test_failed_channel_userdata_allocation(void)
{
	lua_State *L = luaL_newstate();
	fail_alloc state;
	void *ud;
	size_t live = eli_channel_live_count();
	int status;

	assert(L != NULL);
	assert(luaopen_eli_worker(L) == 1);
	lua_pop(L, 1);
	lua_pushcfunction(L, worker_channel);
	lua_pushinteger(L, 1);
	assert(lua_checkstack(L, 2));
	state.original = lua_getallocf(L, &ud);
	state.original_ud = ud;
	state.fail_after = 0;
	lua_setallocf(L, fail_after, &state);
	status = lua_pcall(L, 1, 1, 0);
	lua_setallocf(L, state.original, state.original_ud);
	assert(status == LUA_ERRMEM);
	lua_settop(L, 0);
	lua_gc(L, LUA_GCCOLLECT, 0);
	assert(eli_channel_live_count() == live);
	lua_close(L);
}

static int install_channel_call(lua_State *L)
{
	eli_worker_install_channel(L);
	return 0;
}

/* An allocation failure while the channel metatable is being built must not
 * leave a published but incomplete metatable behind: a retry has to complete
 * it, otherwise later channel userdata never gets a __gc and leaks. */
static void test_channel_metatable_retry_after_allocation_failure(void)
{
	size_t live = eli_channel_live_count();
	int fail_at;

	for (fail_at = 0; fail_at < 64; fail_at++) {
		lua_State *L = luaL_newstate();
		fail_alloc state;
		void *ud;
		int status;

		assert(L != NULL);
		lua_pushcfunction(L, install_channel_call);
		assert(lua_checkstack(L, 1));
		state.original = lua_getallocf(L, &ud);
		state.original_ud = ud;
		state.fail_after = fail_at;
		lua_setallocf(L, fail_after, &state);
		status = lua_pcall(L, 0, 0, 0);
		lua_setallocf(L, state.original, state.original_ud);
		assert(status == LUA_OK || status == LUA_ERRMEM);
		lua_settop(L, 0);

		lua_pushcfunction(L, install_channel_call);
		assert(lua_checkstack(L, 1));
		assert(lua_pcall(L, 0, 0, 0) == LUA_OK);
		lua_settop(L, 0);

		lua_pushcfunction(L, worker_channel);
		lua_pushinteger(L, 0);
		assert(lua_checkstack(L, 2));
		assert(lua_pcall(L, 1, 1, 0) == LUA_OK);
		lua_settop(L, 0);
		lua_close(L);
		assert(eli_channel_live_count() == live);
	}
}

static int install_locale_guard_call(lua_State *L)
{
	eli_worker_install_locale_guard(L);
	return 0;
}

/* Reinstalling must detect the guard by identity and keep the original
 * setlocale in the registry; capturing the guard as the original would
 * recurse until stack overflow. */
static void test_locale_guard_reinstall(void)
{
	lua_State *L = luaL_newstate();

	assert(L != NULL);
	luaL_openlibs(L);
	lua_settop(L, 0);
	lua_pushcfunction(L, install_locale_guard_call);
	assert(lua_checkstack(L, 1));
	assert(lua_pcall(L, 0, 0, 0) == LUA_OK);
	lua_settop(L, 0);

	lua_pushcfunction(L, install_locale_guard_call);
	assert(lua_checkstack(L, 1));
	assert(lua_pcall(L, 0, 0, 0) == LUA_OK);
	lua_settop(L, 0);
	lua_getglobal(L, "os");
	lua_getfield(L, -1, "setlocale");
	assert(lua_tocfunction(L, -1) == locale_guard);
	lua_getfield(L, LUA_REGISTRYINDEX, ELI_LOCALE_ORIG);
	assert(lua_tocfunction(L, -1) != locale_guard);
	lua_pop(L, 3);
	assert(luaL_dostring(L,
	   "assert(os.setlocale(nil)) assert(os.setlocale('C'))") == LUA_OK);
	lua_close(L);
}

static void test_successful_spawn(void)
{
	lua_State *L = NULL;
	int fail_at;
#ifndef _WIN32
	sigset_t before, after;

	assert(pthread_sigmask(SIG_SETMASK, NULL, &before) == 0);
#endif

	/* Sweep constructor allocations until all of them are permitted. */
	for (fail_at = 1; fail_at < 64; fail_at++) {
		int status;

		L = luaL_newstate();
		assert(L != NULL);
		assert(luaopen_eli_worker(L) == 1);
		lua_pop(L, 1);
		status = spawn(L, fail_at, 1);
		if (status == LUA_OK) {
			break;
		}
		assert(status == LUA_ERRMEM);
		lua_settop(L, 0);
		lua_gc(L, LUA_GCCOLLECT, 0);
		lua_close(L);
		L = NULL;
	}
	assert(L != NULL);
#ifndef _WIN32
	assert(pthread_sigmask(SIG_SETMASK, NULL, &after) == 0);
	assert(sigismember(&before, SIGTERM) == sigismember(&after, SIGTERM));
	assert(sigismember(&before, SIGUSR1) == sigismember(&after, SIGUSR1));
#endif
	lua_pushcfunction(L, worker_join);
	lua_pushvalue(L, -2);
	assert(lua_pcall(L, 1, LUA_MULTRET, 0) == LUA_OK);
	assert(lua_toboolean(L, -2));
	assert(lua_tointeger(L, -1) == 42);
	lua_close(L);
}

extern int luaopen_eli_worker_test(lua_State *L);

static int finalize_handle_hook(lua_State *L)
{
	lua_pushvalue(L, lua_upvalueindex(1));
	worker_gc(L);
	return 0;
}

static void test_finalizer_during_join_decode(void)
{
	lua_State *L = luaL_newstate();

	assert(L != NULL);
	assert(luaopen_eli_worker(L) == 1);
	lua_pop(L, 1);
	luaL_requiref(L, "eli.worker.test", luaopen_eli_worker_test, 0);
	lua_pop(L, 1);

	assert(luaL_loadstring(L,
			      "return function() "
			      "require'eli.worker' "
			      "return require'eli.worker.test'.box(7) "
			      "end") == LUA_OK);
	assert(lua_pcall(L, 0, 1, 0) == LUA_OK);
	lua_createtable(L, 0, 2);
	lua_pushvalue(L, -2);
	lua_setfield(L, -2, "fn");
	lua_pushliteral(L, "lua");
	lua_setfield(L, -2, "environment");
	lua_pushcfunction(L, worker_spawn);
	lua_insert(L, -2);
	assert(lua_pcall(L, 1, 1, 0) == LUA_OK);
	lua_remove(L, 1);
	assert(lua_gettop(L) == 1);
	assert(luaL_testudata(L, -1, ELI_WORKER_MT) != NULL);

	/* The hook finalizes the handle while worker_join is decoding results. */
	lua_pushvalue(L, -1);
	lua_pushcclosure(L, finalize_handle_hook, 1);
	lua_setfield(L, LUA_REGISTRYINDEX, "eli.worker.test.import_hook");

	lua_pushcfunction(L, worker_join);
	lua_insert(L, -2);
	assert(lua_pcall(L, 1, LUA_MULTRET, 0) == LUA_OK);
	assert(lua_gettop(L) == 2);
	assert(lua_toboolean(L, 1) == 1);
	assert(luaL_testudata(L, -1, "eli.worker.test.box") != NULL);

	lua_pushnil(L);
	lua_setfield(L, LUA_REGISTRYINDEX, "eli.worker.test.import_hook");
	lua_settop(L, 0);
	lua_gc(L, LUA_GCCOLLECT, 0);
	lua_close(L);
}

/* Raise an existing long error, then deny every subsequent allocation. */
static int fail_long_import(lua_State *L)
{
	fail_alloc *state = (fail_alloc *)lua_touserdata(L, lua_upvalueindex(1));
	lua_pushvalue(L, lua_upvalueindex(2));
	state->fail_after = 0;
	lua_setallocf(L, fail_after, state);
	return lua_error(L);
}

static void test_long_join_error_allocation_failure(void)
{
	lua_State *L = luaL_newstate();
	eli_xfer_packet *packet = eli_xfer_packet_new();
	eli_worker *worker;
	worker_ud *handle;
	fail_alloc state;
	char error[256];
	char message[4096];
	int index = -1;
	int status;

	assert(L != NULL && packet != NULL);
	luaopen_eli_worker(L);
	lua_pop(L, 1);
	luaopen_eli_worker_test(L);
	lua_getfield(L, -1, "box");
	lua_pushinteger(L, 7);
	assert(lua_pcall(L, 1, 1, 0) == LUA_OK);
	assert(eli_xfer_encode(L, packet, &index, 1, error, sizeof(error)) == 0);
	eli_xfer_encode_finish(packet);
	lua_settop(L, 0);

	/* A completed worker without thread scheduling makes the ref check exact. */
	handle = (worker_ud *)lua_newuserdatauv(L, sizeof(*handle), 0);
	handle->worker = NULL;
	luaL_setmetatable(L, ELI_WORKER_MT);
	worker = (eli_worker *)calloc(1, sizeof(*worker));
	assert(worker != NULL);
	assert(mtx_init(&worker->lock, mtx_plain) == thrd_success);
	assert(cnd_init(&worker->cond) == thrd_success);
	worker->refs = 1;
	worker->done = 1;
	worker->results = packet;
	handle->worker = worker;
	memset(message, 'x', sizeof(message));
	lua_pushlstring(L, message, sizeof(message));
	state.original = lua_getallocf(L, &state.original_ud);
	lua_pushlightuserdata(L, &state);
	lua_pushvalue(L, 2);
	lua_pushcclosure(L, fail_long_import, 2);
	lua_setfield(L, LUA_REGISTRYINDEX, "eli.worker.test.import_hook");
	lua_pushcfunction(L, worker_join);
	lua_pushvalue(L, 1);
	status = lua_pcall(L, 1, 2, 0);
	lua_setallocf(L, state.original, state.original_ud);
	assert(status == LUA_OK);
	assert(!lua_toboolean(L, -2));
	assert(lua_rawequal(L, 2, -1));
	assert(worker->refs == 1);
	lua_pushnil(L);
	lua_setfield(L, LUA_REGISTRYINDEX, "eli.worker.test.import_hook");
	lua_close(L);
}

/* A state closed while holding a lock must release it. The main state has no
 * worker-exit hook, so __gc is the only path that can unlock the box; the
 * packet reference keeps the native mutex inspectable after lua_close. */
static void test_state_close_releases_held_mutex(void)
{
	lua_State *L = luaL_newstate();
	lua_State *receiver = luaL_newstate();
	eli_xfer_packet *packet = eli_xfer_packet_new();
	char error[256];
	size_t live = eli_mutex_live_count();
	int index = -1;

	assert(L != NULL && receiver != NULL && packet != NULL);
	assert(luaopen_eli_worker(L) == 1);
	lua_pop(L, 1);
	assert(eli_mutex_create(L) == 1);
	lua_getfield(L, -1, "lock");
	lua_pushvalue(L, -2);
	assert(lua_pcall(L, 1, 0, 0) == LUA_OK);
	assert(eli_xfer_encode(L, packet, &index, 1, error, sizeof(error)) == 0);
	eli_xfer_encode_finish(packet);
	lua_close(L);

	assert(luaopen_eli_worker(receiver) == 1);
	lua_pop(receiver, 1);
	assert(eli_xfer_decode(receiver, packet, error, sizeof(error)) == 0);
	lua_getfield(receiver, -1, "try_lock");
	lua_pushvalue(receiver, -2);
	assert(lua_pcall(receiver, 1, 1, 0) == LUA_OK);
	assert(lua_toboolean(receiver, -1) == 1);
	lua_pop(receiver, 1);
	eli_xfer_packet_free(packet);
	lua_close(receiver);
	assert(eli_mutex_live_count() == live);
}

static int untransferable_probe(lua_State *L)
{
	(void)L;
	return 0;
}

/* An encode that fails after the mutex node was added must still release the
 * packet's reference when the packet is freed. */
static void test_partial_encode_releases_mutex_reference(void)
{
	lua_State *L = luaL_newstate();
	eli_xfer_packet *packet = eli_xfer_packet_new();
	char error[256];
	size_t live = eli_mutex_live_count();
	int indices[2] = {1, 2};

	assert(L != NULL && packet != NULL);
	assert(luaopen_eli_worker(L) == 1);
	lua_pop(L, 1);
	assert(eli_mutex_create(L) == 1);
	lua_pushcfunction(L, untransferable_probe);
	assert(eli_xfer_encode(L, packet, indices, 2, error, sizeof(error)) != 0);
	assert(strstr(error, "C function") != NULL);
	eli_xfer_packet_free(packet);
	lua_close(L);
	assert(eli_mutex_live_count() == live);
}

static void test_source_map_gc_lifetime(void)
{
	lua_State *L = luaL_newstate();
	lua_State *destination;
	eli_xfer_packet *packet = eli_xfer_packet_new();
	char error[256];
	int index = -1;
	int i;

	assert(L != NULL && packet != NULL);
	luaL_openlibs(L);
	/* Eli's library loader leaves the preload chunk's result on the stack. */
	lua_settop(L, 0);
	luaL_requiref(L, "eli.worker.test", luaopen_eli_worker_test, 0);
	lua_pop(L, 1);
	assert(luaL_dostring(L,
	   "local t = { {}, function() end, require'eli.worker.test'.box(9) } "
	   "return setmetatable({t, t[1], t[2], t[3]}, {__mode='v'}), t") == LUA_OK);
	assert(lua_gettop(L) == 2 && lua_istable(L, 1) && lua_istable(L, 2));
	assert(eli_xfer_encode(L, packet, &index, 1, error, sizeof(error)) == 0);
	/* Remove graph edges as well as the stack root: each mapped object must
	 * have its own root, not just the original argument table. */
	for (i = 1; i <= 3; i++) {
		lua_pushnil(L);
		lua_rawseti(L, 2, i);
	}
	lua_pop(L, 1);
	lua_gc(L, LUA_GCCOLLECT, 0);
	for (i = 1; i <= 4; i++) {
		lua_rawgeti(L, 1, i);
		assert(!lua_isnil(L, -1));
		assert(eli_xfer_encode(L, packet, &index, 1, error, sizeof(error)) == 0);
		lua_pop(L, 1);
	}
	eli_xfer_encode_finish(packet);
	eli_xfer_encode_finish(packet);
	lua_gc(L, LUA_GCCOLLECT, 0);
	for (i = 1; i <= 4; i++) {
		lua_rawgeti(L, 1, i);
		assert(lua_isnil(L, -1));
		lua_pop(L, 1);
	}
	assert(eli_xfer_encode(L, packet, &index, 1, error, sizeof(error)) != 0);
	{
		eli_xfer_packet *rejected = eli_xfer_packet_new();
		assert(rejected != NULL);
		lua_newtable(L);
		lua_newtable(L);
		lua_setmetatable(L, -2);
		lua_pushvalue(L, -1);
		lua_rawseti(L, 1, 1);
		assert(eli_xfer_encode(L, rejected, &index, 1, error, sizeof(error)) != 0);
		lua_pop(L, 1);
		/* Failed sessions are disposed on their source thread, too. */
		eli_xfer_packet_free(rejected);
		lua_gc(L, LUA_GCCOLLECT, 0);
		lua_rawgeti(L, 1, 1);
		assert(lua_isnil(L, -1));
		lua_pop(L, 1);
	}
	lua_close(L);

	destination = luaL_newstate();
	assert(destination != NULL);
	assert(eli_xfer_decode(destination, packet, error, sizeof(error)) == 0);
	assert(lua_gettop(destination) == 5);
	assert(lua_rawequal(destination, 1, 2));
	for (i = 1; i <= 3; i++) {
		lua_rawgeti(destination, 1, i);
		assert(lua_rawequal(destination, -1, i + 2));
		lua_pop(destination, 1);
	}
	eli_xfer_packet_free(packet);
	lua_close(destination);
}

int main(void)
{
#ifndef _WIN32
	test_failed_runtime_init();
#endif
	test_activation_rejected_while_locale_busy();
	test_main_state_lifecycle();
	test_worker_main_state_takeover();
	test_failed_userdata_allocation();
	test_embedded_loader_allocation_failure();
	test_failed_channel_userdata_allocation();
	test_channel_metatable_retry_after_allocation_failure();
	test_locale_guard_reinstall();
	test_successful_spawn();
	test_finalizer_during_join_decode();
	test_long_join_error_allocation_failure();
	test_state_close_releases_held_mutex();
	test_partial_encode_releases_mutex_reference();
	test_source_map_gc_lifetime();
	puts("worker constructor regressions passed");
	return 0;
}
