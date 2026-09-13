#ifndef ELI_WORKER_INTERNAL_H
#define ELI_WORKER_INTERNAL_H

#include "lua.h"
#include "lauxlib.h"
#include "eli_xfer.h"

#include "c11threads.h"

#include <stddef.h>
#include <stdint.h>

#define ELI_CHANNEL_MT "eli.worker.channel"
#define ELI_WORKER_MT "eli.worker.handle"

/* ---- channels ---- */

typedef enum eli_chan_status {
	ELI_CHAN_OK = 0,
	ELI_CHAN_CLOSED,
	ELI_CHAN_TIMEOUT
} eli_chan_status;

typedef struct eli_chan_wait eli_chan_wait;

typedef struct eli_channel {
	mtx_t lock;
	cnd_t cond;
	size_t capacity;
	size_t count;
	size_t head;
	eli_xfer_packet **buffer;
	int closed;
	int refs;
	eli_chan_wait *send_head;
	eli_chan_wait *send_tail;
	eli_chan_wait *recv_head;
	eli_chan_wait *recv_tail;
} eli_channel;

eli_channel *eli_channel_new(size_t capacity);
void eli_channel_retain(eli_channel *channel);
void eli_channel_release(eli_channel *channel);
eli_chan_status eli_channel_send(eli_channel *channel, eli_xfer_packet *packet,
				 int64_t timeout_ms);
eli_chan_status eli_channel_receive(eli_channel *channel,
				    eli_xfer_packet **packet,
				    int64_t timeout_ms);
void eli_channel_close(eli_channel *channel);

int eli_channel_push(lua_State *L, eli_channel *channel);
eli_channel *eli_channel_check(lua_State *L, int index);
int eli_worker_open_channel(lua_State *L, int upvalue);
void eli_worker_install_channel(lua_State *L);
const eli_xfer_adapter *eli_worker_channel_adapter(void);

/* ---- workers ---- */

typedef struct eli_worker eli_worker;
typedef struct eli_spawn_data eli_spawn_data;

/* Number of running workers, process wide. Used to guard process-global
 * mutations (locale) while workers are active. */
int eli_worker_active_count(void);
/* Replaces os.setlocale with a guard that rejects mutations while workers
 * are active; queries pass through. Safe to call repeatedly. */
void eli_worker_install_locale_guard(lua_State *L);

#endif /* ELI_WORKER_INTERNAL_H */
