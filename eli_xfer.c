#include "eli_xfer.h"

#include "lauxlib.h"

#include <limits.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ELI_XFER_REGISTRY "eli.xfer.adapters"
#define ELI_XFER_MAX_DEPTH 200
/* Headroom for an adapter's import_fn, which runs in this C frame: the import
 * installs metatables and registers adapters before pushing its userdata. Not
 * routed through a Lua call, so it does not get the usual C-function reserve. */
#define ELI_XFER_IMPORT_STACK LUA_MINSTACK

typedef enum eli_wire_kind {
	ELI_WIRE_NIL = 0,
	ELI_WIRE_BOOL,
	ELI_WIRE_INT,
	ELI_WIRE_FLOAT,
	ELI_WIRE_STRING,
	ELI_WIRE_TABLE,
	ELI_WIRE_FUNCTION,
	ELI_WIRE_USERDATA
} eli_wire_kind;

typedef struct eli_wire_entry {
	int key;
	int value;
} eli_wire_entry;

typedef struct eli_wire_node {
	eli_wire_kind kind;
	int env_upvalue; /* function _ENV upvalue index, 0 when absent */
	union {
		int boolean;
		lua_Integer integer;
		lua_Number number;
		struct {
			char *data;
			size_t len;
		} bytes; /* strings and function bytecode */
		struct {
			eli_wire_entry *entries;
			size_t count;
		} table;
		struct {
			const eli_xfer_adapter *adapter;
			void *data;
			size_t size;
		} userdata;
	} u;
} eli_wire_node;

struct eli_xfer_packet {
	eli_wire_node *nodes;
	size_t node_count;
	size_t node_cap;
	int *roots;
	size_t root_count;
	size_t root_cap;
	/* transient encode state: maps Lua object identity to node index */
	const void **map_ptrs;
	int *map_nodes;
	size_t map_count;
	size_t map_cap;
	lua_State *source;
	int source_roots;
	int finished;
};

static void set_error(char *errbuf, size_t errlen, const char *format, ...)
{
	va_list args;

	if (errbuf == NULL || errlen == 0) {
		return;
	}
	va_start(args, format);
	vsnprintf(errbuf, errlen, format, args);
	va_end(args);
}

eli_xfer_packet *eli_xfer_packet_new(void)
{
	eli_xfer_packet *packet = (eli_xfer_packet *)calloc(1, sizeof(*packet));
	return packet;
}

/* Published as one pointer so a packet destruction can never observe a
 * half-installed begin/end pair. The hooks structure is immutable and
 * process-lifetime. */
static _Atomic(const eli_xfer_payload_hooks *) payload_hooks;

void eli_xfer_set_payload_hooks(const eli_xfer_payload_hooks *hooks)
{
	atomic_store_explicit(&payload_hooks, hooks, memory_order_release);
}

static void wire_node_clear(eli_wire_node *node)
{
	switch (node->kind) {
	case ELI_WIRE_STRING:
	case ELI_WIRE_FUNCTION:
		free(node->u.bytes.data);
		break;
	case ELI_WIRE_TABLE:
		free(node->u.table.entries);
		break;
	case ELI_WIRE_USERDATA:
		if (node->u.userdata.adapter != NULL &&
		    node->u.userdata.adapter->cleanup_fn != NULL) {
			node->u.userdata.adapter->cleanup_fn(node->u.userdata.data,
							     node->u.userdata.size);
		}
		break;
	default:
		break;
	}
	node->kind = ELI_WIRE_NIL;
}

void eli_xfer_encode_finish(eli_xfer_packet *packet)
{
	if (packet == NULL || packet->finished) {
		return;
	}
	if (packet->source != NULL) {
		luaL_unref(packet->source, LUA_REGISTRYINDEX, packet->source_roots);
		packet->source = NULL;
	}
	free(packet->map_ptrs);
	free(packet->map_nodes);
	packet->map_ptrs = NULL;
	packet->map_nodes = NULL;
	packet->map_count = packet->map_cap = 0;
	packet->finished = 1;
}

void eli_xfer_packet_free(eli_xfer_packet *packet)
{
	const eli_xfer_payload_hooks *hooks;
	size_t i;

	if (packet == NULL) {
		return;
	}
	/* Load once: begin and end must come from the same published pair. */
	hooks = atomic_load_explicit(&payload_hooks, memory_order_acquire);
	eli_xfer_encode_finish(packet);
	if (hooks != NULL) {
		hooks->begin();
	}
	for (i = 0; i < packet->node_count; i++) {
		wire_node_clear(&packet->nodes[i]);
	}
	if (hooks != NULL) {
		hooks->end();
	}
	free(packet->nodes);
	free(packet->roots);
	free(packet);
}

size_t eli_xfer_packet_roots(const eli_xfer_packet *packet)
{
	return packet == NULL ? 0 : packet->root_count;
}

void eli_xfer_packet_visit_userdata(const eli_xfer_packet *packet,
				    eli_xfer_userdata_visitor visitor, void *context)
{
	size_t i;

	if (packet == NULL || visitor == NULL) {
		return;
	}
	for (i = 0; i < packet->node_count; i++) {
		const eli_wire_node *node = &packet->nodes[i];
		if (node->kind == ELI_WIRE_USERDATA) {
			visitor(node->u.userdata.adapter, node->u.userdata.data,
				node->u.userdata.size, context);
		}
	}
}

/* ---- adapter registry ---- */

int eli_xfer_register(lua_State *L, int metatable_index,
		      const eli_xfer_adapter *adapter)
{
	if (adapter == NULL || adapter->api_version != ELI_XFER_API_VERSION ||
	    adapter->type_name == NULL || adapter->export_fn == NULL ||
	    adapter->import_fn == NULL || adapter->cleanup_fn == NULL) {
		return luaL_error(L, "invalid transfer adapter definition");
	}
	metatable_index = lua_absindex(L, metatable_index);

	lua_getfield(L, LUA_REGISTRYINDEX, ELI_XFER_REGISTRY);
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_setfield(L, LUA_REGISTRYINDEX, ELI_XFER_REGISTRY);
	}
	lua_pushvalue(L, metatable_index);
	lua_pushlightuserdata(L, (void *)adapter);
	lua_rawset(L, -3);
	lua_pop(L, 1);
	return 0;
}

static const eli_xfer_adapter *find_adapter(lua_State *L, int index)
{
	const eli_xfer_adapter *adapter = NULL;

	if (!lua_getmetatable(L, index)) {
		return NULL;
	}
	/* stack: metatable */
	lua_getfield(L, LUA_REGISTRYINDEX, ELI_XFER_REGISTRY);
	if (!lua_istable(L, -1)) {
		lua_pop(L, 2);
		return NULL;
	}
	/* stack: metatable, registry */
	lua_pushvalue(L, -2);
	lua_rawget(L, -2);
	adapter = (const eli_xfer_adapter *)lua_touserdata(L, -1);
	lua_pop(L, 3);
	return adapter;
}

/* ---- encode ---- */

typedef struct encode_ctx {
	eli_xfer_packet *packet;
	char *errbuf;
	size_t errlen;
	size_t depth;
	int failed;
} encode_ctx;

/* Open-addressed identity map. Object pointers are at least pointer-aligned,
 * so mix away the low bits to spread power-of-two buckets. */
static size_t packet_map_hash(const void *pointer)
{
	uintptr_t value = (uintptr_t)pointer;

	value ^= value >> 17;
	value *= (uintptr_t)0xed5ad4bbU;
	value ^= value >> 11;
	return (size_t)value;
}

static size_t packet_map_slot(const eli_xfer_packet *packet,
			      const void *pointer)
{
	size_t mask = packet->map_cap - 1;
	size_t slot = packet_map_hash(pointer) & mask;

	while (packet->map_ptrs[slot] != NULL &&
	       packet->map_ptrs[slot] != pointer) {
		slot = (slot + 1) & mask;
	}
	return slot;
}

static int packet_map_find(const eli_xfer_packet *packet, const void *pointer)
{
	size_t slot;

	if (packet->map_cap == 0) {
		return -1;
	}
	slot = packet_map_slot(packet, pointer);
	if (packet->map_ptrs[slot] == NULL) {
		return -1;
	}
	return packet->map_nodes[slot];
}

static int packet_map_grow(eli_xfer_packet *packet)
{
	size_t new_cap = packet->map_cap == 0 ? 16 : packet->map_cap * 2;
	const void **ptrs = (const void **)calloc(new_cap, sizeof(*ptrs));
	int *nodes = (int *)malloc(new_cap * sizeof(*nodes));
	const void **old_ptrs = packet->map_ptrs;
	int *old_nodes = packet->map_nodes;
	size_t old_cap = packet->map_cap;
	size_t i;

	if (ptrs == NULL || nodes == NULL) {
		free(ptrs);
		free(nodes);
		return 0;
	}
	packet->map_ptrs = ptrs;
	packet->map_nodes = nodes;
	packet->map_cap = new_cap;
	for (i = 0; i < old_cap; i++) {
		size_t slot;
		if (old_ptrs[i] == NULL) {
			continue;
		}
		slot = packet_map_slot(packet, old_ptrs[i]);
		packet->map_ptrs[slot] = old_ptrs[i];
		packet->map_nodes[slot] = old_nodes[i];
	}
	free(old_ptrs);
	free(old_nodes);
	return 1;
}

static int packet_map_add(eli_xfer_packet *packet, const void *pointer,
			  int node_index)
{
	size_t slot;

	if (packet->map_cap == 0 ||
	    packet->map_count + 1 > packet->map_cap - packet->map_cap / 4) {
		if (!packet_map_grow(packet)) {
			return 0;
		}
	}
	slot = packet_map_slot(packet, pointer);
	packet->map_ptrs[slot] = pointer;
	packet->map_nodes[slot] = node_index;
	packet->map_count++;
	return 1;
}

static int packet_new_node(eli_xfer_packet *packet, eli_wire_kind kind,
			   char *errbuf, size_t errlen)
{
	eli_wire_node *nodes;
	int index;

	if (packet->node_count == packet->node_cap) {
		size_t cap = packet->node_cap == 0 ? 16 : packet->node_cap * 2;
		nodes = (eli_wire_node *)realloc(packet->nodes,
						 cap * sizeof(*nodes));
		if (nodes == NULL) {
			set_error(errbuf, errlen, "out of memory");
			return -1;
		}
		packet->nodes = nodes;
		packet->node_cap = cap;
	}
	if (packet->node_count > (size_t)INT_MAX) {
		set_error(errbuf, errlen, "too many transferable values");
		return -1;
	}
	index = (int)packet->node_count;
	memset(&packet->nodes[index], 0, sizeof(packet->nodes[index]));
	packet->nodes[index].kind = kind;
	packet->node_count++;
	return index;
}

static int packet_add_root(eli_xfer_packet *packet, int node_index,
			   char *errbuf, size_t errlen)
{
	if (packet->root_count == packet->root_cap) {
		size_t cap = packet->root_cap == 0 ? 4 : packet->root_cap * 2;
		int *roots =
		   (int *)realloc(packet->roots, cap * sizeof(*roots));
		if (roots == NULL) {
			set_error(errbuf, errlen, "out of memory");
			return 0;
		}
		packet->roots = roots;
		packet->root_cap = cap;
	}
	packet->roots[packet->root_count++] = node_index;
	return 1;
}

static int encode_value(encode_ctx *ctx, lua_State *L, int index);

typedef struct lua_dump_buffer {
	eli_wire_node *node;
	size_t cap;
	int failed;
} lua_dump_buffer;

static int dump_writer(lua_State *L, const void *data, size_t size, void *ud)
{
	lua_dump_buffer *buffer = (lua_dump_buffer *)ud;
	(void)L;

	if (buffer->failed) {
		return 1;
	}
	/* Lua 5.5 ends a dump with a NULL, zero-length write. */
	if (size == 0) {
		return 0;
	}
	if (buffer->node->u.bytes.len + size > buffer->cap) {
		size_t cap = buffer->cap == 0 ? 256 : buffer->cap;
		char *grown;
		while (cap < buffer->node->u.bytes.len + size) {
			cap *= 2;
		}
		grown = (char *)realloc(buffer->node->u.bytes.data, cap);
		if (grown == NULL) {
			buffer->failed = 1;
			return 1;
		}
		/* lua_dump can raise: the packet must already own this allocation. */
		buffer->node->u.bytes.data = grown;
		buffer->cap = cap;
	}
	memcpy(buffer->node->u.bytes.data + buffer->node->u.bytes.len, data, size);
	buffer->node->u.bytes.len += size;
	return 0;
}

static int packed_string(encode_ctx *ctx, eli_wire_node *node,
			 const char *data, size_t len)
{
	node->u.bytes.data = (char *)malloc(len + 1);
	if (node->u.bytes.data == NULL) {
		set_error(ctx->errbuf, ctx->errlen, "out of memory");
		return 0;
	}
	memcpy(node->u.bytes.data, data, len);
	node->u.bytes.data[len] = '\0';
	node->u.bytes.len = len;
	return 1;
}

static int encode_function(encode_ctx *ctx, lua_State *L, int index,
			   int node_index, const void *pointer)
{
	eli_xfer_packet *packet = ctx->packet;
	eli_wire_node *node = &packet->nodes[node_index];
	lua_Debug ar;
	int upvalue_count;
	int env_upvalue = 0;
	int i;

	if (lua_iscfunction(L, index)) {
		set_error(ctx->errbuf, ctx->errlen,
			  "cannot transfer a C function");
		return 0;
	}

	lua_pushvalue(L, index);
	lua_getinfo(L, ">u", &ar); /* '>' pops the function */
	upvalue_count = ar.nups;

	for (i = 1; i <= upvalue_count; i++) {
		const char *name = lua_getupvalue(L, index, i);
		if (name == NULL) {
			set_error(ctx->errbuf, ctx->errlen,
				  "cannot transfer a function whose upvalue names were stripped");
			return 0;
		}
		if (strcmp(name, "_ENV") == 0) {
			int same;
			lua_pushglobaltable(L);
			same = lua_rawequal(L, -1, -2);
			lua_pop(L, 2);
			if (!same) {
				set_error(ctx->errbuf, ctx->errlen,
					  "cannot transfer a function with a custom _ENV; only the source globals are supported");
				return 0;
			}
			env_upvalue = i;
		} else {
			set_error(ctx->errbuf, ctx->errlen,
				  "cannot transfer a function that captures the local '%s'",
				  name);
			lua_pop(L, 1);
			return 0;
		}
	}

	{
		lua_dump_buffer buffer = {node, 0, 0};
		int status;
		lua_pushvalue(L, index);
		status = lua_dump(L, dump_writer, &buffer, 0);
		lua_pop(L, 1);
		if (status != 0 || buffer.failed) {
			set_error(ctx->errbuf, ctx->errlen,
				  "failed to dump function bytecode");
			return 0;
		}
		node->env_upvalue = env_upvalue;
	}
	(void)pointer;
	return 1;
}

static int encode_table(encode_ctx *ctx, lua_State *L, int index,
			int node_index)
{
	eli_xfer_packet *packet = ctx->packet;
	eli_wire_entry *entries;
	size_t count = 0;
	size_t i = 0;

	if (++ctx->depth > ELI_XFER_MAX_DEPTH) {
		set_error(ctx->errbuf, ctx->errlen,
			  "table is too deeply nested to transfer");
		return 0;
	}
	if (!lua_checkstack(L, 3)) {
		set_error(ctx->errbuf, ctx->errlen,
			  "out of Lua stack while transferring a table");
		return 0;
	}
	index = lua_absindex(L, index);

	lua_pushnil(L);
	while (lua_next(L, index) != 0) {
		count++;
		lua_pop(L, 1);
	}

	entries = count == 0
		     ? NULL
		     : (eli_wire_entry *)calloc(count, sizeof(*entries));
	if (count > 0 && entries == NULL) {
		set_error(ctx->errbuf, ctx->errlen, "out of memory");
		return 0;
	}
	packet->nodes[node_index].u.table.entries = entries;
	packet->nodes[node_index].u.table.count = count;

	lua_pushnil(L);
	while (lua_next(L, index) != 0) {
		int key;
		int value;
		if (i >= count) {
			set_error(ctx->errbuf, ctx->errlen,
				  "table mutated during transfer");
			lua_pop(L, 1);
			return 0;
		}
		key = encode_value(ctx, L, -2);
		value = key < 0 ? -1 : encode_value(ctx, L, -1);
		lua_pop(L, 1);
		if (key < 0 || value < 0) {
			return 0;
		}
		entries[i].key = key;
		entries[i].value = value;
		i++;
	}
	packet->nodes[node_index].u.table.count = i;
	ctx->depth--;
	return 1;
}

static int encode_value(encode_ctx *ctx, lua_State *L, int index)
{
	int value_type;
	eli_xfer_packet *packet = ctx->packet;
	int node_index;

	index = lua_absindex(L, index);
	/* Adapter lookup, function inspection and metatable access push up to
	 * four transient values without reserving stack space themselves. */
	if (!lua_checkstack(L, 4)) {
		set_error(ctx->errbuf, ctx->errlen,
			  "out of Lua stack while transferring a value");
		return -1;
	}
	value_type = lua_type(L, index);

	switch (value_type) {
	case LUA_TNIL:
		return packet_new_node(packet, ELI_WIRE_NIL, ctx->errbuf,
				       ctx->errlen);
	case LUA_TBOOLEAN:
		node_index = packet_new_node(packet, ELI_WIRE_BOOL,
					     ctx->errbuf, ctx->errlen);
		if (node_index >= 0) {
			packet->nodes[node_index].u.boolean =
			   lua_toboolean(L, index);
		}
		return node_index;
	case LUA_TNUMBER:
		if (lua_isinteger(L, index)) {
			node_index = packet_new_node(packet, ELI_WIRE_INT,
						     ctx->errbuf,
						     ctx->errlen);
			if (node_index >= 0) {
				packet->nodes[node_index].u.integer =
				   lua_tointeger(L, index);
			}
		} else {
			node_index = packet_new_node(packet, ELI_WIRE_FLOAT,
						     ctx->errbuf,
						     ctx->errlen);
			if (node_index >= 0) {
				packet->nodes[node_index].u.number =
				   lua_tonumber(L, index);
			}
		}
		return node_index;
	case LUA_TSTRING: {
		size_t len;
		const char *data = lua_tolstring(L, index, &len);
		node_index = packet_new_node(packet, ELI_WIRE_STRING,
					     ctx->errbuf, ctx->errlen);
		if (node_index >= 0 &&
		    !packed_string(ctx, &packet->nodes[node_index], data, len)) {
			return -1;
		}
		return node_index;
	}
	case LUA_TTABLE:
	case LUA_TFUNCTION:
	case LUA_TUSERDATA: {
		const void *pointer = lua_topointer(L, index);
		int existing = packet_map_find(packet, pointer);
		if (existing >= 0) {
			return existing;
		}
		/* Root before adapters or recursive encoding can run GC. The map
		 * survives encode calls, so stack arguments alone are insufficient. */
		lua_rawgeti(L, LUA_REGISTRYINDEX, packet->source_roots);
		lua_pushvalue(L, index);
		lua_rawsetp(L, -2, pointer);
		lua_pop(L, 1);
		if (value_type == LUA_TTABLE) {
			if (lua_getmetatable(L, index)) {
				lua_pop(L, 1);
				set_error(ctx->errbuf, ctx->errlen,
					  "cannot transfer a table with a metatable");
				return -1;
			}
			node_index = packet_new_node(packet, ELI_WIRE_TABLE,
						     ctx->errbuf,
						     ctx->errlen);
			if (node_index < 0) {
				return -1;
			}
			if (!packet_map_add(packet, pointer, node_index)) {
				set_error(ctx->errbuf, ctx->errlen,
					  "out of memory");
				return -1;
			}
			if (!encode_table(ctx, L, index, node_index)) {
				return -1;
			}
			return node_index;
		}
		if (value_type == LUA_TFUNCTION) {
			node_index = packet_new_node(packet, ELI_WIRE_FUNCTION,
						     ctx->errbuf,
						     ctx->errlen);
			if (node_index < 0) {
				return -1;
			}
			if (!packet_map_add(packet, pointer, node_index)) {
				set_error(ctx->errbuf, ctx->errlen,
					  "out of memory");
				return -1;
			}
			if (!encode_function(ctx, L, index, node_index,
					     pointer)) {
				return -1;
			}
			return node_index;
		}
		/* userdata */
		{
			const eli_xfer_adapter *adapter =
			   find_adapter(L, index);
			void *data = NULL;
			size_t size = 0;
			if (adapter == NULL) {
				if (lua_getmetatable(L, index)) {
					lua_getfield(L, -1, "__name");
					if (lua_isstring(L, -1)) {
						set_error(ctx->errbuf,
							  ctx->errlen,
							  "cannot transfer userdata of type '%s' (no transfer adapter is registered)",
							  lua_tostring(L, -1));
						lua_pop(L, 2);
						return -1;
					}
					lua_pop(L, 2);
				}
				set_error(ctx->errbuf, ctx->errlen,
					  "cannot transfer unregistered userdata (no transfer adapter is registered for its metatable)");
				return -1;
			}
			if (adapter->export_fn(L, index, &data, &size) != 0) {
				const char *message = lua_tostring(L, -1);
				set_error(ctx->errbuf, ctx->errlen,
					  "cannot transfer userdata: %s",
					  message != NULL ? message
							  : "export failed");
				lua_pop(L, 1);
				return -1;
			}
			node_index = packet_new_node(packet, ELI_WIRE_USERDATA,
						     ctx->errbuf,
						     ctx->errlen);
			if (node_index < 0) {
				if (adapter->cleanup_fn != NULL) {
					adapter->cleanup_fn(data, size);
				}
				return -1;
			}
			if (!packet_map_add(packet, pointer, node_index)) {
				set_error(ctx->errbuf, ctx->errlen,
					  "out of memory");
				if (adapter->cleanup_fn != NULL) {
					adapter->cleanup_fn(data, size);
				}
				return -1;
			}
			packet->nodes[node_index].u.userdata.adapter = adapter;
			packet->nodes[node_index].u.userdata.data = data;
			packet->nodes[node_index].u.userdata.size = size;
			return node_index;
		}
	}
	default:
		set_error(ctx->errbuf, ctx->errlen,
			  "cannot transfer a value of type '%s'",
			  luaL_typename(L, index));
		return -1;
	}
}

static int encode_protected(lua_State *L)
{
	encode_ctx *ctx = (encode_ctx *)lua_touserdata(L, 1);
	int count = lua_gettop(L);
	int i;

	if (ctx->packet->source == NULL) {
		lua_newtable(L);
		lua_pushthread(L);
		lua_rawseti(L, -2, 0);
		ctx->packet->source_roots = luaL_ref(L, LUA_REGISTRYINDEX);
		ctx->packet->source = L;
	}
	for (i = 2; i <= count; i++) {
		int node = encode_value(ctx, L, i);
		if (node < 0 || !packet_add_root(ctx->packet, node,
					       ctx->errbuf, ctx->errlen)) {
			ctx->failed = 1;
			break;
		}
	}
	return 0;
}

int eli_xfer_encode(lua_State *L, eli_xfer_packet *packet, const int *indices,
		    size_t count, char *errbuf, size_t errlen)
{
	encode_ctx ctx = {packet, errbuf, errlen, 0, 0};
	int top = lua_gettop(L);
	size_t i;

	if (errbuf != NULL && errlen > 0) {
		errbuf[0] = '\0';
	}
	if (packet->finished || (packet->source != NULL && packet->source != L)) {
		set_error(errbuf, errlen, "transfer encoding session is finished or belongs to another Lua thread");
		return 1;
	}
	if (count > INT_MAX - 2 || !lua_checkstack(L, (int)count + 2)) {
		set_error(errbuf, errlen, "out of Lua stack while transferring values");
		return 1;
	}
	lua_pushcfunction(L, encode_protected);
	lua_pushlightuserdata(L, &ctx);
	for (i = 0; i < count; i++) {
		int index = indices[i];
		if (index < 0 && index > LUA_REGISTRYINDEX) index += top + 1;
		lua_pushvalue(L, index);
	}
	/* Both Lua's dumper and adapters can raise; callers must regain control
	 * to free the partially encoded packet on every error path. */
	if (lua_pcall(L, (int)count + 1, 0, 0) != LUA_OK) {
		set_error(errbuf, errlen, "%s", lua_type(L, -1) == LUA_TSTRING
			  ? lua_tostring(L, -1) : "error while transferring values");
		ctx.failed = 1;
	}
	lua_settop(L, top);
	return ctx.failed;
}

/* ---- decode ---- */

typedef struct decode_ctx {
	const eli_xfer_packet *packet;
	char *errbuf;
	size_t errlen;
	size_t depth;
	int seen; /* absolute stack index of the identity table */
} decode_ctx;

static int decode_value(decode_ctx *ctx, lua_State *L, int node_index);

static int decode_table_entries(decode_ctx *ctx, lua_State *L,
				const eli_wire_node *node, int table_index)
{
	size_t i;

	if (++ctx->depth > ELI_XFER_MAX_DEPTH) {
		set_error(ctx->errbuf, ctx->errlen,
			  "packet is too deeply nested to decode");
		return 0;
	}
	if (!lua_checkstack(L, 3)) {
		set_error(ctx->errbuf, ctx->errlen,
			  "out of Lua stack while decoding a table");
		return 0;
	}
	table_index = lua_absindex(L, table_index);
	for (i = 0; i < node->u.table.count; i++) {
		const eli_wire_entry *entry = &node->u.table.entries[i];
		if (!decode_value(ctx, L, entry->key)) {
			return 0;
		}
		if (!decode_value(ctx, L, entry->value)) {
			return 0;
		}
		lua_rawset(L, table_index);
	}
	ctx->depth--;
	return 1;
}

static int decode_value(decode_ctx *ctx, lua_State *L, int node_index)
{
	const eli_wire_node *node;

	if (node_index < 0 || (size_t)node_index >= ctx->packet->node_count) {
		set_error(ctx->errbuf, ctx->errlen, "corrupt worker packet");
		return 0;
	}
	node = &ctx->packet->nodes[node_index];

	switch (node->kind) {
	case ELI_WIRE_NIL:
		lua_pushnil(L);
		return 1;
	case ELI_WIRE_BOOL:
		lua_pushboolean(L, node->u.boolean);
		return 1;
	case ELI_WIRE_INT:
		lua_pushinteger(L, node->u.integer);
		return 1;
	case ELI_WIRE_FLOAT:
		lua_pushnumber(L, node->u.number);
		return 1;
	case ELI_WIRE_STRING:
		lua_pushlstring(L, node->u.bytes.data, node->u.bytes.len);
		return 1;
	case ELI_WIRE_TABLE: {
		lua_rawgeti(L, ctx->seen, node_index);
		if (!lua_isnil(L, -1)) {
			return 1;
		}
		lua_pop(L, 1);
		lua_createtable(L, 0, (int)node->u.table.count);
		lua_pushvalue(L, -1);
		lua_rawseti(L, ctx->seen, node_index);
		return decode_table_entries(ctx, L, node, -1);
	}
	case ELI_WIRE_FUNCTION: {
		int function_index;
		int status;

		lua_rawgeti(L, ctx->seen, node_index);
		if (!lua_isnil(L, -1)) {
			return 1;
		}
		lua_pop(L, 1);
		status = luaL_loadbufferx(L, node->u.bytes.data,
					  node->u.bytes.len, "=worker", "b");
		if (status != LUA_OK) {
			const char *message = lua_tostring(L, -1);
			set_error(ctx->errbuf, ctx->errlen,
				  "cannot load transferred function: %s",
				  message != NULL ? message : "invalid bytecode");
			lua_pop(L, 1);
			return 0;
		}
		function_index = lua_gettop(L);
		lua_pushvalue(L, function_index);
		lua_rawseti(L, ctx->seen, node_index);
		if (node->env_upvalue >= 1) {
			lua_pushglobaltable(L);
			if (lua_setupvalue(L, function_index,
					   node->env_upvalue) == NULL) {
				set_error(ctx->errbuf, ctx->errlen,
					  "cannot bind transferred function environment");
				lua_pop(L, 1);
				return 0;
			}
		}
		return 1;
	}
	case ELI_WIRE_USERDATA: {
		int status;

		lua_rawgeti(L, ctx->seen, node_index);
		if (!lua_isnil(L, -1)) {
			return 1;
		}
		lua_pop(L, 1);
		if (!lua_checkstack(L, ELI_XFER_IMPORT_STACK)) {
			set_error(ctx->errbuf, ctx->errlen,
				  "out of Lua stack while importing transferred userdata");
			return 0;
		}
		status = node->u.userdata.adapter->import_fn(
		   L, node->u.userdata.data, node->u.userdata.size);
		if (status != 0) {
			const char *message = lua_tostring(L, -1);
			set_error(ctx->errbuf, ctx->errlen,
				  "cannot import transferred userdata: %s",
				  message != NULL ? message : "import failed");
			lua_pop(L, 1);
			return 0;
		}
		lua_pushvalue(L, -1);
		lua_rawseti(L, ctx->seen, node_index);
		return 1;
	}
	default:
		set_error(ctx->errbuf, ctx->errlen, "corrupt worker packet");
		return 0;
	}
}

int eli_xfer_decode(lua_State *L, const eli_xfer_packet *packet, char *errbuf,
		    size_t errlen)
{
	decode_ctx ctx;
	size_t i;

	if (errbuf != NULL && errlen > 0) {
		errbuf[0] = '\0';
	}
	ctx.packet = packet;
	ctx.errbuf = errbuf;
	ctx.errlen = errlen;
	ctx.depth = 0;
	lua_newtable(L);
	ctx.seen = lua_gettop(L);
	for (i = 0; i < packet->root_count; i++) {
		if (!lua_checkstack(L, 3)) {
			set_error(ctx.errbuf, ctx.errlen,
				  "out of Lua stack while decoding values");
			return 1;
		}
		if (!decode_value(&ctx, L, packet->roots[i])) {
			return 1;
		}
	}
	lua_remove(L, ctx.seen);
	return 0;
}

int eli_xfer_decode_into(lua_State *L, const eli_xfer_packet *packet,
			 int target_index, char *errbuf, size_t errlen)
{
	decode_ctx ctx;
	size_t i;

	if (errbuf != NULL && errlen > 0) {
		errbuf[0] = '\0';
	}
	ctx.packet = packet;
	ctx.errbuf = errbuf;
	ctx.errlen = errlen;
	ctx.depth = 0;
	target_index = lua_absindex(L, target_index);
	lua_newtable(L);
	ctx.seen = lua_gettop(L);
	for (i = 0; i < packet->root_count; i++) {
		int node_index = packet->roots[i];
		const eli_wire_node *node;

		if (!lua_checkstack(L, 3)) {
			set_error(ctx.errbuf, ctx.errlen,
				  "out of Lua stack while decoding values");
			return 1;
		}
		if (node_index < 0 ||
		    (size_t)node_index >= packet->node_count) {
			set_error(ctx.errbuf, ctx.errlen,
				  "corrupt worker packet");
			return 1;
		}
		node = &packet->nodes[node_index];
		if (node->kind == ELI_WIRE_TABLE) {
			lua_pushvalue(L, target_index);
			lua_rawseti(L, ctx.seen, node_index);
			if (!decode_table_entries(&ctx, L, node,
						  target_index)) {
				return 1;
			}
		} else if (!decode_value(&ctx, L, node_index)) {
			return 1;
		}
	}
	lua_remove(L, ctx.seen);
	return 0;
}
