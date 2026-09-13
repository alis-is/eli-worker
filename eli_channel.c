#include "eli_worker_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct channel_ud {
	eli_channel *channel;
} channel_ud;

struct eli_chan_wait {
	eli_chan_wait *next;
	eli_xfer_packet *packet;
	int done;
	int closed;
};

eli_channel *eli_channel_new(size_t capacity)
{
	eli_channel *channel = (eli_channel *)calloc(1, sizeof(*channel));

	if (channel == NULL) {
		return NULL;
	}
	if (mtx_init(&channel->lock, mtx_plain) != thrd_success) {
		free(channel);
		return NULL;
	}
	if (cnd_init(&channel->cond) != thrd_success) {
		mtx_destroy(&channel->lock);
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
			mtx_destroy(&channel->lock);
			free(channel);
			return NULL;
		}
	}
	return channel;
}

void eli_channel_retain(eli_channel *channel)
{
	if (channel == NULL) {
		return;
	}
	mtx_lock(&channel->lock);
	channel->refs++;
	mtx_unlock(&channel->lock);
}

void eli_channel_release(eli_channel *channel)
{
	int destroy = 0;

	if (channel == NULL) {
		return;
	}
	mtx_lock(&channel->lock);
	channel->refs--;
	destroy = channel->refs <= 0;
	mtx_unlock(&channel->lock);
	if (!destroy) {
		return;
	}
	while (channel->count > 0) {
		eli_xfer_packet *packet = channel->buffer[channel->head];
		channel->head = (channel->head + 1) % channel->capacity;
		channel->count--;
		eli_xfer_packet_free(packet);
	}
	free(channel->buffer);
	cnd_destroy(&channel->cond);
	mtx_destroy(&channel->lock);
	free(channel);
}

static void make_deadline(int64_t timeout_ms, struct timespec *deadline)
{
	timespec_get(deadline, TIME_UTC);
	deadline->tv_sec += (time_t)(timeout_ms / 1000);
	deadline->tv_nsec += (long)((timeout_ms % 1000) * 1000000L);
	if (deadline->tv_nsec >= 1000000000L) {
		deadline->tv_nsec -= 1000000000L;
		deadline->tv_sec += 1;
	}
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

	mtx_lock(&channel->lock);
	for (;;) {
		if (channel->closed) {
			mtx_unlock(&channel->lock);
			return ELI_CHAN_CLOSED;
		}
		if (channel->recv_head != NULL) {
			eli_chan_wait *receiver =
			   dequeue(&channel->recv_head, &channel->recv_tail);
			receiver->packet = packet;
			receiver->done = 1;
			cnd_signal(&channel->cond);
			mtx_unlock(&channel->lock);
			return ELI_CHAN_OK;
		}
		if (channel->count < channel->capacity) {
			channel->buffer[(channel->head + channel->count) %
					channel->capacity] = packet;
			channel->count++;
			cnd_signal(&channel->cond);
			mtx_unlock(&channel->lock);
			return ELI_CHAN_OK;
		}
		if (timeout_ms == 0) {
			mtx_unlock(&channel->lock);
			return ELI_CHAN_TIMEOUT;
		}

		waiter = (eli_chan_wait *)calloc(1, sizeof(*waiter));
		if (waiter == NULL) {
			mtx_unlock(&channel->lock);
			return ELI_CHAN_TIMEOUT; /* treated as a failure by callers */
		}
		waiter->packet = packet;
		enqueue(&channel->send_head, &channel->send_tail, waiter);
		if (timeout_ms > 0) {
			make_deadline(timeout_ms, &deadline);
		}

		while (!waiter->done && !waiter->closed) {
			int status;
			if (timeout_ms < 0) {
				status = cnd_wait(&channel->cond,
						  &channel->lock);
			} else {
				status = cnd_timedwait(&channel->cond,
						       &channel->lock,
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
		mtx_unlock(&channel->lock);
		return result;
	}
}

eli_chan_status eli_channel_receive(eli_channel *channel,
				    eli_xfer_packet **packet,
				    int64_t timeout_ms)
{
	struct timespec deadline;
	eli_chan_wait *waiter;
	eli_chan_status result;

	mtx_lock(&channel->lock);
	for (;;) {
		if (channel->count > 0) {
			*packet = channel->buffer[channel->head];
			channel->head = (channel->head + 1) % channel->capacity;
			channel->count--;
			if (channel->capacity > 0 && channel->send_head != NULL) {
				eli_chan_wait *sender = dequeue(
				   &channel->send_head, &channel->send_tail);
				channel->buffer[(channel->head +
						 channel->count) %
						channel->capacity] =
				   sender->packet;
				channel->count++;
				sender->done = 1;
			}
			cnd_signal(&channel->cond);
			mtx_unlock(&channel->lock);
			return ELI_CHAN_OK;
		}
		if (channel->send_head != NULL) {
			eli_chan_wait *sender = dequeue(&channel->send_head,
							&channel->send_tail);
			*packet = sender->packet;
			sender->done = 1;
			cnd_signal(&channel->cond);
			mtx_unlock(&channel->lock);
			return ELI_CHAN_OK;
		}
		if (channel->closed) {
			mtx_unlock(&channel->lock);
			return ELI_CHAN_CLOSED;
		}
		if (timeout_ms == 0) {
			mtx_unlock(&channel->lock);
			return ELI_CHAN_TIMEOUT;
		}

		waiter = (eli_chan_wait *)calloc(1, sizeof(*waiter));
		if (waiter == NULL) {
			mtx_unlock(&channel->lock);
			return ELI_CHAN_TIMEOUT;
		}
		enqueue(&channel->recv_head, &channel->recv_tail, waiter);
		if (timeout_ms > 0) {
			make_deadline(timeout_ms, &deadline);
		}

		while (!waiter->done && !waiter->closed) {
			int status;
			if (timeout_ms < 0) {
				status = cnd_wait(&channel->cond,
						  &channel->lock);
			} else {
				status = cnd_timedwait(&channel->cond,
						       &channel->lock,
						       &deadline);
			}
			if (status != thrd_success) {
				break;
			}
		}
		if (waiter->done) {
			*packet = waiter->packet;
			free(waiter);
			mtx_unlock(&channel->lock);
			return ELI_CHAN_OK;
		}
		{
			int closed = waiter->closed;
			unlink_waiter(channel, waiter);
			free(waiter);
			mtx_unlock(&channel->lock);
			return closed ? ELI_CHAN_CLOSED : ELI_CHAN_TIMEOUT;
		}
	}
}

void eli_channel_close(eli_channel *channel)
{
	if (channel == NULL) {
		return;
	}
	mtx_lock(&channel->lock);
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
	mtx_unlock(&channel->lock);
}

/* ---- adapter ---- */

static int channel_export(lua_State *L, int index, void **data, size_t *size)
{
	channel_ud *ud = (channel_ud *)luaL_checkudata(L, index, ELI_CHANNEL_MT);
	/* the packet takes ownership of a new reference, released by
	 * channel_cleanup on every cleanup path */
	eli_channel_retain(ud->channel);
	*data = ud->channel;
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

static int64_t check_timeout(lua_State *L, int index, int64_t fallback)
{
	if (lua_isnoneornil(L, index)) {
		return fallback;
	}
	{
		lua_Number value = luaL_checknumber(L, index);
		if (value < 0) {
			return luaL_argerror(
			   L, index, "timeout must be a non-negative number");
		}
		return (int64_t)value;
	}
}

static int channel_send(lua_State *L)
{
	eli_channel *channel =
	   ((channel_ud *)luaL_checkudata(L, 1, ELI_CHANNEL_MT))->channel;
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
	status = eli_channel_send(channel, packet, -1);
	if (status != ELI_CHAN_OK) {
		eli_xfer_packet_free(packet);
		lua_pushboolean(L, 0);
		lua_pushstring(L, "closed");
		return 2;
	}
	lua_pushboolean(L, 1);
	return 1;
}

static int channel_try_send(lua_State *L)
{
	eli_channel *channel =
	   ((channel_ud *)luaL_checkudata(L, 1, ELI_CHANNEL_MT))->channel;
	eli_xfer_packet *packet;
	char error[256];
	int index = 2;
	int64_t timeout = check_timeout(L, 3, 0);
	eli_chan_status status;

	packet = eli_xfer_packet_new();
	if (packet == NULL) {
		return luaL_error(L, "out of memory");
	}
	if (eli_xfer_encode(L, packet, &index, 1, error, sizeof(error)) != 0) {
		eli_xfer_packet_free(packet);
		return luaL_error(L, "%s", error);
	}
	status = eli_channel_send(channel, packet, timeout);
	if (status != ELI_CHAN_OK) {
		eli_xfer_packet_free(packet);
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
	eli_channel *channel =
	   ((channel_ud *)luaL_checkudata(L, 1, ELI_CHANNEL_MT))->channel;
	eli_xfer_packet *packet = NULL;
	char error[256];
	eli_chan_status status = eli_channel_receive(channel, &packet, -1);

	if (status != ELI_CHAN_OK) {
		lua_pushnil(L);
		lua_pushboolean(L, 0);
		lua_pushstring(L, "closed");
		return 3;
	}
	if (eli_xfer_decode(L, packet, error, sizeof(error)) != 0) {
		eli_xfer_packet_free(packet);
		return luaL_error(L, "%s", error);
	}
	eli_xfer_packet_free(packet);
	lua_pushboolean(L, 1);
	return 2;
}

static int channel_try_receive(lua_State *L)
{
	eli_channel *channel =
	   ((channel_ud *)luaL_checkudata(L, 1, ELI_CHANNEL_MT))->channel;
	eli_xfer_packet *packet = NULL;
	char error[256];
	int64_t timeout = check_timeout(L, 2, 0);
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
	if (eli_xfer_decode(L, packet, error, sizeof(error)) != 0) {
		eli_xfer_packet_free(packet);
		return luaL_error(L, "%s", error);
	}
	eli_xfer_packet_free(packet);
	lua_pushboolean(L, 1);
	return 2;
}

static int channel_close(lua_State *L)
{
	eli_channel *channel =
	   ((channel_ud *)luaL_checkudata(L, 1, ELI_CHANNEL_MT))->channel;
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
	if (luaL_newmetatable(L, ELI_CHANNEL_MT)) {
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
	}
	lua_pop(L, 1);
}

int eli_channel_push(lua_State *L, eli_channel *channel)
{
	channel_ud *ud;

	eli_worker_install_channel(L);
	ud = (channel_ud *)lua_newuserdatauv(L, sizeof(*ud), 0);
	ud->channel = channel;
	eli_channel_retain(channel);
	luaL_setmetatable(L, ELI_CHANNEL_MT);
	return 0;
}

eli_channel *eli_channel_check(lua_State *L, int index)
{
	channel_ud *ud = (channel_ud *)luaL_checkudata(L, index, ELI_CHANNEL_MT);
	return ud->channel;
}
