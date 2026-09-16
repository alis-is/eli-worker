/* Small native adapter fixture used by the worker test-suite to verify the
 * userdata-transfer API (export/import/cleanup, identity and rejection). */

#include "lua.h"
#include "lauxlib.h"
#include "eli_xfer.h"
#include "eli_worker_internal.h"

#include <stdlib.h>

#define ELI_WORKER_TEST_BOX_MT "eli_worker.test.box"
#define ELI_WORKER_TEST_IMPORT_HOOK "eli_worker.test.import_hook"

typedef struct test_box {
	lua_Integer value;
} test_box;

static void ensure_box_metatable(lua_State *L);

static int box_export(lua_State *L, int index, void **data, size_t *size)
{
	test_box *box = (test_box *)luaL_checkudata(L, index,
						    ELI_WORKER_TEST_BOX_MT);
	lua_Integer *payload = (lua_Integer *)malloc(sizeof(*payload));
	if (payload == NULL) {
		lua_pushliteral(L, "out of memory");
		return 1;
	}
	*payload = box->value;
	*data = payload;
	*size = sizeof(*payload);
	return 0;
}

static void box_import_locked(lua_State *L, const lua_Integer *payload)
{
	test_box *box;
	ensure_box_metatable(L);
	box = (test_box *)lua_newuserdatauv(L, sizeof(*box), 0);
	box->value = *payload;
	luaL_setmetatable(L, ELI_WORKER_TEST_BOX_MT);
}

static void run_import_hook(lua_State *L)
{
	lua_getfield(L, LUA_REGISTRYINDEX, ELI_WORKER_TEST_IMPORT_HOOK);
	if (lua_isfunction(L, -1)) {
		lua_call(L, 0, 0); /* raises are caught by the protected decode */
	} else {
		lua_pop(L, 1);
	}
}

static int box_import(lua_State *L, const void *data, size_t size)
{
	if (size != sizeof(lua_Integer)) {
		lua_pushliteral(L, "corrupt box payload");
		return 1;
	}
	run_import_hook(L);
	box_import_locked(L, (const lua_Integer *)data);
	return 0;
}

static void box_cleanup(void *data, size_t size)
{
	(void)size;
	free(data);
}

static const eli_xfer_adapter box_adapter = {
	ELI_XFER_API_VERSION,
	ELI_WORKER_TEST_BOX_MT,
	box_export,
	box_import,
	box_cleanup,
};

static void ensure_box_metatable(lua_State *L)
{
	/* Same rule as the channel installer: publish only a complete table. */
	lua_getfield(L, LUA_REGISTRYINDEX, ELI_WORKER_TEST_BOX_MT);
	if (!lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return;
	}
	lua_pop(L, 1);

	lua_newtable(L);
	lua_pushstring(L, ELI_WORKER_TEST_BOX_MT);
	lua_setfield(L, -2, "__name");
	eli_xfer_register(L, -1, &box_adapter);
	lua_setfield(L, LUA_REGISTRYINDEX, ELI_WORKER_TEST_BOX_MT);
}

static int test_box_new(lua_State *L)
{
	lua_Integer value = luaL_checkinteger(L, 1);
	test_box *box;
	ensure_box_metatable(L);
	box = (test_box *)lua_newuserdatauv(L, sizeof(*box), 0);
	box->value = value;
	luaL_setmetatable(L, ELI_WORKER_TEST_BOX_MT);
	return 1;
}

static int test_box_value(lua_State *L)
{
	test_box *box = (test_box *)luaL_checkudata(L, 1,
						    ELI_WORKER_TEST_BOX_MT);
	lua_pushinteger(L, box->value);
	return 1;
}

static int test_channel_count(lua_State *L)
{
	lua_pushinteger(L, (lua_Integer)eli_channel_live_count());
	return 1;
}

static int test_set_import_hook(lua_State *L)
{
	if (!lua_isnoneornil(L, 1)) {
		luaL_checktype(L, 1, LUA_TFUNCTION);
		lua_pushvalue(L, 1);
	} else {
		lua_pushnil(L);
	}
	lua_setfield(L, LUA_REGISTRYINDEX, ELI_WORKER_TEST_IMPORT_HOOK);
	return 0;
}

int luaopen_eli_worker_test(lua_State *L)
{
	lua_newtable(L);
	lua_pushcfunction(L, test_box_new);
	lua_setfield(L, -2, "box");
	lua_pushcfunction(L, test_box_value);
	lua_setfield(L, -2, "value");
	lua_pushcfunction(L, test_channel_count);
	lua_setfield(L, -2, "channel_count");
	lua_pushcfunction(L, test_set_import_hook);
	lua_setfield(L, -2, "set_import_hook");
	return 1;
}
