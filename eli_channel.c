#include "eli_worker_internal.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct channel_ud {
	eli_channel *channel;
} channel_ud;

struct eli_chan_wait {
	eli_chan_wait *next;
	eli_xfer_packet *packet;
	int done;
	int closed;
};

static once_flag channel_once = ONCE_FLAG_INIT;
static mtx_t channel_lock;
static eli_channel *channels;
static size_t channel_count;
static int channel_collecting;
static int channel_dirty;
static int channel_runtime_ok;
static int channel_batch_depth;

static void channel_collect(void);
static void channel_release_batch_begin(void);
static void channel_release_batch_end(void);

static const eli_xfer_payload_hooks channel_payload_hooks = {
	channel_release_batch_begin,
	channel_release_batch_end,
};

static void channel_runtime_init(void)
{
	channel_runtime_ok = mtx_init(&channel_lock, mtx_plain) == thrd_success;
	if (channel_runtime_ok) {
		eli_xfer_set_payload_hooks(&channel_payload_hooks);
	}
}

static int lock_channels_raw(void)
{
	call_once(&channel_once, channel_runtime_init);
	if (!channel_runtime_ok) return 0;
	mtx_lock(&channel_lock);
	return 1;
}

/* Public entry points drain deferred releases before acting. A GC sweep of N
 * channels then costs one graph scan instead of one scan per finalizer. */
static int lock_channels(void)
{
	if (!lock_channels_raw()) return 0;
	if (!channel_collecting && channel_batch_depth == 0 && channel_dirty) {
		channel_dirty = 0;
		channel_collect();
	}
	return 1;
}

/* Packet destruction releases every channel payload in one go. Defer the
 * graph scans to the outermost batch so an N-channel packet costs one scan
 * instead of N. */
static void channel_release_batch_begin(void)
{
	if (!lock_channels_raw()) return;
	channel_batch_depth++;
	mtx_unlock(&channel_lock);
}

static void channel_release_batch_end(void)
{
	if (!lock_channels_raw()) return;
	if (channel_batch_depth > 0) channel_batch_depth--;
	if (channel_batch_depth == 0 && !channel_collecting && channel_dirty) {
		channel_dirty = 0;
		channel_collect();
	}
	mtx_unlock(&channel_lock);
}

static void channel_edge(const eli_xfer_adapter *adapter, void *data,
			 size_t size, void *context)
{
	eli_channel *channel = (eli_channel *)data;
	(void)size;
	(void)context;
	if (adapter == eli_worker_channel_adapter() && channel != NULL) {
		channel->gc_refs--;
	}
}

static void mark_edges(const eli_xfer_adapter *adapter, void *data,
		       size_t size, void *context)
{
	eli_channel *channel = (eli_channel *)data;
	eli_channel **worklist = (eli_channel **)context;
	(void)size;
	if (adapter == eli_worker_channel_adapter() && channel != NULL &&
	    !channel->gc_mark) {
		channel->gc_mark = 1;
		channel->gc_link = *worklist;
		*worklist = channel;
	}
}

eli_channel *eli_channel_new(size_t capacity)
{
	eli_channel *channel = (eli_channel *)calloc(1, sizeof(*channel));

	if (channel == NULL) {
		return NULL;
	}
	if (cnd_init(&channel->cond) != thrd_success) {
		free(channel);
		return NULL;
	}
	channel->capacity = capacity;
	channel->refs = 1;
	if (capacity > 0) {
		channel->buffer = (eli_xfer_packet **)calloc(
		   capacity, sizeof(*channel->buffer));
		if (channel->buffer == NULL) {
			cnd_destroy(&channel->cond);
			free(channel);
			return NULL;
		}
	}
	if (!lock_channels()) {
		free(channel->buffer);
		cnd_destroy(&channel->cond);
		free(channel);
		return NULL;
	}
	channel->next = channels;
	channel->prev = NULL;
	if (channels != NULL) {
		channels->prev = channel;
	}
	channels = channel;
	channel_count++;
	mtx_unlock(&channel_lock);
	return channel;
}

void eli_channel_retain(eli_channel *channel)
{
	if (channel == NULL || !lock_channels()) {
		return;
	}
	channel->refs++;
	mtx_unlock(&channel_lock);
}

static void channel_drain(eli_channel *channel)
{
	while (channel->count > 0) {
		eli_xfer_packet *packet = channel->buffer[channel->head];
		channel->head = (channel->head + 1) % channel->capacity;
		channel->count--;
		eli_xfer_packet_free(packet);
	}
}

static void channel_free(eli_channel *channel)
{
	channel_drain(channel);
	free(channel->buffer);
	cnd_destroy(&channel->cond);
	free(channel);
}

static void channel_unlink(eli_channel *channel)
{
	if (channel->prev != NULL) {
		channel->prev->next = channel->next;
	} else {
		channels = channel->next;
	}
	if (channel->next != NULL) {
		channel->next->prev = channel->prev;
	}
	channel_count--;
}

static void channel_collect(void)
{
	eli_channel *channel, **link, *dead = NULL;
	eli_channel *worklist = NULL;


again:
	/* ponytail: one global lock; shard only if channel contention matters. */
	if (channel_collecting) return;
	channel_collecting = 1;
	for (channel = channels; channel != NULL; channel = channel->next) {
		channel->gc_refs = channel->refs;
		channel->gc_mark = 0;
	}
	for (channel = channels; channel != NULL; channel = channel->next) {
		size_t i;
		for (i = 0; i < channel->count; i++)
			eli_xfer_packet_visit_userdata(channel->buffer[
				(channel->head + i) % channel->capacity], channel_edge, NULL);
	}
	for (channel = channels; channel != NULL; channel = channel->next) {
		if (channel->gc_refs > 0) {
			channel->gc_mark = 1;
			channel->gc_link = worklist;
			worklist = channel;
		}
	}
	/* Explicit worklist: each reachable channel and its packets are visited
	 * once, instead of rescanning every channel until marking stops. */
	while (worklist != NULL) {
		size_t i;
		channel = worklist;
		worklist = channel->gc_link;
		for (i = 0; i < channel->count; i++)
			eli_xfer_packet_visit_userdata(channel->buffer[
				(channel->head + i) % channel->capacity], mark_edges, &worklist);
	}
	link = &channels;
	while ((channel = *link) != NULL) {
		if (channel->gc_mark) {
			link = &channel->next;
			continue;
		}
		*link = channel->next;
		if (channel->next != NULL) {
			channel->next->prev = channel->prev;
		}
		channel->next = dead;
		channel->collecting = 1;
		dead = channel;
		channel_count--;
	}
	mtx_unlock(&channel_lock);
	/* A collected channel's packets can reference other collected channels.
	 * Draining a packet releases those references, so every packet must be
	 * freed while all channel shells are still allocated; freeing a shell
	 * first would let a release write through freed memory. */
	for (channel = dead; channel != NULL; channel = channel->next) {
		channel_drain(channel);
	}
	while ((channel = dead) != NULL) {
		dead = channel->next;
		channel_free(channel);
	}
	lock_channels_raw();
	channel_collecting = 0;
	if (channel_dirty) {
		channel_dirty = 0;
		goto again;
	}
}

void eli_channel_release(eli_channel *channel)
{
	if (channel == NULL || !lock_channels_raw()) return;
	channel->refs--;
	if (channel->collecting || channel_collecting ||
	    channel_batch_depth > 0) {
		channel_dirty = 1;
		mtx_unlock(&channel_lock);
		return;
	}
	if (channel->refs <= 0) {
		/* ponytail: no reference remains, so no cycle can keep it alive;
		 * skip the graph scan. */
		channel_unlink(channel);
		channel->collecting = 1;
		channel_collecting = 1;
		mtx_unlock(&channel_lock);
		channel_free(channel);
		lock_channels_raw();
		channel_collecting = 0;
		if (channel_dirty) {
			channel_dirty = 0;
			channel_collect();
		}
		mtx_unlock(&channel_lock);
		return;
	}
	/* The shell may now belong to an unreachable cycle. Defer the scan; the
	 * next channel entry point collects all deferred releases at once. */
	channel_dirty = 1;
	mtx_unlock(&channel_lock);
}

size_t eli_channel_live_count(void)
{
	size_t count;

	if (!lock_channels()) {
		return 0;
	}
	count = channel_count;
	mtx_unlock(&channel_lock);
	return count;
}

static void unlink_one(eli_chan_wait **head, eli_chan_wait **tail,
		       eli_chan_wait *waiter)
{
	eli_chan_wait **link = head;

	while (*link != NULL && *link != waiter) {
		link = &(*link)->next;
	}
	if (*link != waiter) {
		return; /* already handed off */
	}
	*link = waiter->next;
	if (*tail == waiter) {
		*tail = NULL;
		for (link = head; *link != NULL; link = &(*link)->next) {
			*tail = *link;
		}
	}
}

static void unlink_waiter(eli_channel *channel, eli_chan_wait *waiter)
{
	unlink_one(&channel->send_head, &channel->send_tail, waiter);
	unlink_one(&channel->recv_head, &channel->recv_tail, waiter);
}

static void enqueue(eli_chan_wait **head, eli_chan_wait **tail,
		    eli_chan_wait *waiter)
{
	waiter->next = NULL;
	if (*tail == NULL) {
		*head = waiter;
	} else {
		(*tail)->next = waiter;
	}
	*tail = waiter;
}

static eli_chan_wait *dequeue(eli_chan_wait **head, eli_chan_wait **tail)
{
	eli_chan_wait *waiter = *head;

	if (waiter == NULL) {
		return NULL;
	}
	*head = waiter->next;
	if (*head == NULL) {
		*tail = NULL;
	}
	waiter->next = NULL;
	return waiter;
}

eli_chan_status eli_channel_send(eli_channel *channel, eli_xfer_packet *packet,
				 int64_t timeout_ms)
{
	struct timespec deadline;
	eli_chan_wait *waiter;
	eli_chan_status result;

	if (!lock_channels()) {
		return ELI_CHAN_ERROR;
	}
	for (;;) {
		if (channel->closed) {
			mtx_unlock(&channel_lock);
			return ELI_CHAN_CLOSED;
		}
		if (channel->recv_head != NULL) {
			eli_chan_wait *receiver =
			   dequeue(&channel->recv_head, &channel->recv_tail);
			receiver->packet = packet;
			receiver->done = 1;
			cnd_broadcast(&channel->cond);
			mtx_unlock(&channel_lock);
			return ELI_CHAN_OK;
		}
		if (channel->count < channel->capacity) {
			channel->buffer[(channel->head + channel->count) %
					channel->capacity] = packet;
			channel->count++;
			cnd_broadcast(&channel->cond);
			mtx_unlock(&channel_lock);
			return ELI_CHAN_OK;
		}
		if (timeout_ms == 0) {
			mtx_unlock(&channel_lock);
			return ELI_CHAN_TIMEOUT;
		}

		waiter = (eli_chan_wait *)calloc(1, sizeof(*waiter));
		if (waiter == NULL) {
			mtx_unlock(&channel_lock);
			return ELI_CHAN_ERROR;
		}
		waiter->packet = packet;
		enqueue(&channel->send_head, &channel->send_tail, waiter);
		if (timeout_ms > 0) {
			eli_worker_deadline(timeout_ms, &deadline);
		}

		while (!waiter->done && !waiter->closed) {
			int status;
			if (timeout_ms < 0) {
				status = cnd_wait(&channel->cond,
						  &channel_lock);
			} else {
				status = cnd_timedwait(&channel->cond,
						       &channel_lock,
						       &deadline);
			}
			if (status != thrd_success) {
				break;
			}
		}
		if (waiter->done) {
			result = ELI_CHAN_OK;
		} else {
			unlink_waiter(channel, waiter);
			result = waiter->closed ? ELI_CHAN_CLOSED
						: ELI_CHAN_TIMEOUT;
		}
		free(waiter);
		mtx_unlock(&channel_lock);
		return result;
	}
}

eli_chan_status eli_channel_receive(eli_channel *channel,
				    eli_xfer_packet **packet,
				    int64_t timeout_ms)
{
	struct timespec deadline;
	eli_chan_wait *waiter;

	if (!lock_channels()) {
		return ELI_CHAN_ERROR;
	}
	for (;;) {
		if (channel->count > 0) {
			*packet = channel->buffer[channel->head];
			channel->head = (channel->head + 1) % channel->capacity;
			channel->count--;
			if (!channel->closed && channel->capacity > 0 &&
			    channel->send_head != NULL) {
				eli_chan_wait *sender = dequeue(
				   &channel->send_head, &channel->send_tail);
				channel->buffer[(channel->head +
						 channel->count) %
						channel->capacity] =
				   sender->packet;
				channel->count++;
				sender->done = 1;
			}
			cnd_broadcast(&channel->cond);
			mtx_unlock(&channel_lock);
			return ELI_CHAN_OK;
		}
		if (channel->closed) {
			mtx_unlock(&channel_lock);
			return ELI_CHAN_CLOSED;
		}
		if (channel->send_head != NULL) {
			eli_chan_wait *sender = dequeue(&channel->send_head,
							&channel->send_tail);
			*packet = sender->packet;
			sender->done = 1;
			cnd_broadcast(&channel->cond);
			mtx_unlock(&channel_lock);
			return ELI_CHAN_OK;
		}
		if (timeout_ms == 0) {
			mtx_unlock(&channel_lock);
			return ELI_CHAN_TIMEOUT;
		}

		waiter = (eli_chan_wait *)calloc(1, sizeof(*waiter));
		if (waiter == NULL) {
			mtx_unlock(&channel_lock);
			return ELI_CHAN_ERROR;
		}
		enqueue(&channel->recv_head, &channel->recv_tail, waiter);
		if (timeout_ms > 0) {
			eli_worker_deadline(timeout_ms, &deadline);
		}

		while (!waiter->done && !waiter->closed) {
			int status;
			if (timeout_ms < 0) {
				status = cnd_wait(&channel->cond,
						  &channel_lock);
			} else {
				status = cnd_timedwait(&channel->cond,
						       &channel_lock,
						       &deadline);
			}
			if (status != thrd_success) {
				break;
			}
		}
		if (waiter->done) {
			*packet = waiter->packet;
			free(waiter);
			mtx_unlock(&channel_lock);
			return ELI_CHAN_OK;
		}
		{
			int closed = waiter->closed;
			unlink_waiter(channel, waiter);
			free(waiter);
			mtx_unlock(&channel_lock);
			return closed ? ELI_CHAN_CLOSED : ELI_CHAN_TIMEOUT;
		}
	}
}

void eli_channel_close(eli_channel *channel)
{
	if (channel == NULL || !lock_channels()) {
		return;
	}
	if (!channel->closed) {
		eli_chan_wait *waiter;
		channel->closed = 1;
		for (waiter = channel->send_head; waiter != NULL;
		     waiter = waiter->next) {
			waiter->closed = 1;
		}
		for (waiter = channel->recv_head; waiter != NULL;
		     waiter = waiter->next) {
			waiter->closed = 1;
		}
		cnd_broadcast(&channel->cond);
	}
	mtx_unlock(&channel_lock);
}

/* ---- adapter ---- */

static int channel_export(lua_State *L, int index, void **data, size_t *size)
{
	eli_channel *channel = eli_channel_check(L, index);
	/* the packet takes ownership of a new reference, released by
	 * channel_cleanup on every cleanup path */
	eli_channel_retain(channel);
	*data = channel;
	*size = 0;
	return 0;
}

static int channel_import(lua_State *L, const void *data, size_t size)
{
	(void)size;
	return eli_channel_push(L, (eli_channel *)data);
}

static void channel_cleanup(void *data, size_t size)
{
	(void)size;
	eli_channel_release((eli_channel *)data);
}

static const eli_xfer_adapter channel_adapter = {
	ELI_XFER_API_VERSION,
	ELI_CHANNEL_MT,
	channel_export,
	channel_import,
	channel_cleanup,
};

const eli_xfer_adapter *eli_worker_channel_adapter(void)
{
	return &channel_adapter;
}

/* ---- userdata ---- */

static int channel_gc(lua_State *L)
{
	channel_ud *ud = (channel_ud *)luaL_checkudata(L, 1, ELI_CHANNEL_MT);
	eli_channel_release(ud->channel);
	ud->channel = NULL;
	return 0;
}

int64_t eli_worker_check_timeout(lua_State *L, int index, int64_t fallback)
{
	if (lua_isnoneornil(L, index)) {
		return fallback;
	}
	{
		lua_Number value = luaL_checknumber(L, index);
		if (!isfinite(value) || value < 0 || value >= (lua_Number)INT64_MAX) {
			return luaL_argerror(
			   L, index, "timeout must be finite, non-negative, and less than INT64_MAX");
		}
		return (int64_t)value;
	}
}

typedef struct decode_guard {
	eli_xfer_packet *packet;
	char *error;
	size_t error_size;
} decode_guard;

static int decode_packet_protected(lua_State *L)
{
	decode_guard *guard = (decode_guard *)lua_touserdata(L, 1);

	if (eli_xfer_decode(L, guard->packet, guard->error,
			    guard->error_size) != 0) {
		return luaL_error(L, "%s", guard->error);
	}
	return lua_gettop(L) - 1;
}

/* Decoding runs Lua code (allocation, adapters); the dequeued packet must be
 * freed even when that code raises. Raises on failure. */
static int channel_decode(lua_State *L, eli_xfer_packet *packet, char *error,
			  size_t error_size)
{
	decode_guard guard;
	int top = lua_gettop(L);
	int status;

	guard.packet = packet;
	guard.error = error;
	guard.error_size = error_size;
	lua_pushcfunction(L, decode_packet_protected);
	lua_pushlightuserdata(L, &guard);
	status = lua_pcall(L, 1, LUA_MULTRET, 0);
	eli_xfer_packet_free(packet);
	if (status != LUA_OK) {
		return lua_error(L);
	}
	return lua_gettop(L) - top;
}

/* Encoding runs Lua hooks and finalizers that may finalize the channel
 * userdata; the native pointer must be read again afterwards. */
static eli_channel *channel_after_encode(lua_State *L, int index)
{
	channel_ud *ud =
	   (channel_ud *)luaL_testudata(L, index, ELI_CHANNEL_MT);

	return ud != NULL ? ud->channel : NULL;
}

static int channel_send(lua_State *L)
{
	eli_channel *channel = eli_channel_check(L, 1);
	eli_xfer_packet *packet;
	char error[256];
	int index = 2;
	eli_chan_status status;

	packet = eli_xfer_packet_new();
	if (packet == NULL) {
		return luaL_error(L, "out of memory");
	}
	if (eli_xfer_encode(L, packet, &index, 1, error, sizeof(error)) != 0) {
		eli_xfer_packet_free(packet);
		return luaL_error(L, "%s", error);
	}
	eli_xfer_encode_finish(packet);
	channel = channel_after_encode(L, 1);
	if (channel == NULL) {
		eli_xfer_packet_free(packet);
		return luaL_error(L, "attempt to use a finalized channel");
	}
	status = eli_channel_send(channel, packet, -1);
	if (status != ELI_CHAN_OK) {
		eli_xfer_packet_free(packet);
		if (status == ELI_CHAN_ERROR) {
			return luaL_error(L, "out of memory");
		}
		lua_pushboolean(L, 0);
		lua_pushstring(L, "closed");
		return 2;
	}
	lua_pushboolean(L, 1);
	return 1;
}

static int channel_try_send(lua_State *L)
{
	eli_channel *channel = eli_channel_check(L, 1);
	eli_xfer_packet *packet;
	char error[256];
	int index = 2;
	int64_t timeout = eli_worker_check_timeout(L, 3, 0);
	eli_chan_status status;

	packet = eli_xfer_packet_new();
	if (packet == NULL) {
		return luaL_error(L, "out of memory");
	}
	if (eli_xfer_encode(L, packet, &index, 1, error, sizeof(error)) != 0) {
		eli_xfer_packet_free(packet);
		return luaL_error(L, "%s", error);
	}
	eli_xfer_encode_finish(packet);
	channel = channel_after_encode(L, 1);
	if (channel == NULL) {
		eli_xfer_packet_free(packet);
		return luaL_error(L, "attempt to use a finalized channel");
	}
	status = eli_channel_send(channel, packet, timeout);
	if (status != ELI_CHAN_OK) {
		eli_xfer_packet_free(packet);
		if (status == ELI_CHAN_ERROR) {
			return luaL_error(L, "out of memory");
		}
		lua_pushboolean(L, 0);
		lua_pushstring(L, status == ELI_CHAN_CLOSED ? "closed"
							    : "timeout");
		return 2;
	}
	lua_pushboolean(L, 1);
	return 1;
}

static int channel_receive(lua_State *L)
{
	eli_channel *channel = eli_channel_check(L, 1);
	eli_xfer_packet *packet = NULL;
	char error[256];
	eli_chan_status status = eli_channel_receive(channel, &packet, -1);

	if (status != ELI_CHAN_OK) {
		lua_pushnil(L);
		lua_pushboolean(L, 0);
		lua_pushstring(L, status == ELI_CHAN_ERROR ? "out of memory"
							   : "closed");
		return 3;
	}
	channel_decode(L, packet, error, sizeof(error));
	lua_pushboolean(L, 1);
	return 2;
}

static int channel_try_receive(lua_State *L)
{
	eli_channel *channel = eli_channel_check(L, 1);
	eli_xfer_packet *packet = NULL;
	char error[256];
	int64_t timeout = eli_worker_check_timeout(L, 2, 0);
	eli_chan_status status =
	   eli_channel_receive(channel, &packet, timeout);

	if (status == ELI_CHAN_TIMEOUT) {
		lua_pushnil(L);
		lua_pushboolean(L, 0);
		lua_pushstring(L, "timeout");
		return 3;
	}
	if (status == ELI_CHAN_CLOSED) {
		lua_pushnil(L);
		lua_pushboolean(L, 0);
		lua_pushstring(L, "closed");
		return 3;
	}
	if (status == ELI_CHAN_ERROR) {
		lua_pushnil(L);
		lua_pushboolean(L, 0);
		lua_pushstring(L, "out of memory");
		return 3;
	}
	channel_decode(L, packet, error, sizeof(error));
	lua_pushboolean(L, 1);
	return 2;
}

static int channel_close(lua_State *L)
{
	eli_channel *channel = eli_channel_check(L, 1);

	eli_channel_close(channel);
	lua_pushboolean(L, 1);
	return 1;
}

static int channel_tostring(lua_State *L)
{
	channel_ud *ud = (channel_ud *)luaL_checkudata(L, 1, ELI_CHANNEL_MT);
	lua_pushfstring(L, "worker.channel(%p)", (void *)ud->channel);
	return 1;
}

static const luaL_Reg channel_methods[] = {
	{"send", channel_send},
	{"receive", channel_receive},
	{"try_send", channel_try_send},
	{"try_receive", channel_try_receive},
	{"close", channel_close},
	{NULL, NULL},
};

void eli_worker_install_channel(lua_State *L)
{
	/* Publish only a complete metatable: luaL_newmetatable stores the table
	 * before its methods and __gc exist, so an allocation failure during a
	 * retry would leave every later channel userdata without a finalizer. */
	lua_getfield(L, LUA_REGISTRYINDEX, ELI_CHANNEL_MT);
	if (!lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return;
	}
	lua_pop(L, 1);

	lua_newtable(L);
	luaL_setfuncs(L, channel_methods, 0);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, channel_gc);
	lua_setfield(L, -2, "__gc");
	lua_pushcfunction(L, channel_gc);
	lua_setfield(L, -2, "__close");
	lua_pushcfunction(L, channel_tostring);
	lua_setfield(L, -2, "__tostring");
	lua_pushstring(L, "channel");
	lua_setfield(L, -2, "__type");
	lua_pushstring(L, ELI_CHANNEL_MT);
	lua_setfield(L, -2, "__name");
	eli_xfer_register(L, -1, &channel_adapter);
	lua_setfield(L, LUA_REGISTRYINDEX, ELI_CHANNEL_MT);
}

int eli_channel_push(lua_State *L, eli_channel *channel)
{
	channel_ud *ud;

	eli_worker_install_channel(L);
	ud = (channel_ud *)lua_newuserdatauv(L, sizeof(*ud), 0);
	ud->channel = NULL;
	luaL_setmetatable(L, ELI_CHANNEL_MT);
	eli_channel_retain(channel);
	ud->channel = channel;
	return 0;
}

int eli_channel_create(lua_State *L, size_t capacity)
{
	channel_ud *ud;
	eli_channel *channel;

	eli_worker_install_channel(L);
	ud = (channel_ud *)lua_newuserdatauv(L, sizeof(*ud), 0);
	ud->channel = NULL;
	luaL_setmetatable(L, ELI_CHANNEL_MT);
	channel = eli_channel_new(capacity);
	if (channel == NULL) {
		return luaL_error(L, "out of memory");
	}
	/* The initial reference now belongs to this fully installed userdata. */
	ud->channel = channel;
	return 1;
}

eli_channel *eli_channel_check(lua_State *L, int index)
{
	channel_ud *ud = (channel_ud *)luaL_checkudata(L, index, ELI_CHANNEL_MT);

	if (ud->channel == NULL) {
		luaL_error(L, "attempt to use a finalized channel");
		return NULL;
	}
	return ud->channel;
}
