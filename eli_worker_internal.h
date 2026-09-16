#ifndef ELI_WORKER_INTERNAL_H
#define ELI_WORKER_INTERNAL_H

#include "lua.h"
#include "lauxlib.h"
#include "eli_xfer.h"

#include "c11threads.h"

#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <time.h>

#define ELI_CHANNEL_MT "eli.worker.channel"
#define ELI_WORKER_MT "eli.worker.handle"

/* Use the actual integer type's limit, without signed shifts or width guesses. */
#define ELI_WORKER_TIME_MAX _Generic((time_t)0, \
	char: CHAR_MAX, signed char: SCHAR_MAX, unsigned char: UCHAR_MAX, \
	short: SHRT_MAX, unsigned short: USHRT_MAX, \
	int: INT_MAX, unsigned int: UINT_MAX, \
	long: LONG_MAX, unsigned long: ULONG_MAX, \
	long long: LLONG_MAX, unsigned long long: ULLONG_MAX)

/* C11 cnd_timedwait takes an absolute TIME_UTC deadline. timeout_ms >= 0. */
static inline void eli_worker_deadline(int64_t timeout_ms,
				       struct timespec *deadline)
{
	uintmax_t seconds = (uintmax_t)(timeout_ms / 1000);
	const time_t maximum = ELI_WORKER_TIME_MAX;

	if (timespec_get(deadline, TIME_UTC) != TIME_UTC) {
		/* An unusable clock must fail safe: time out immediately rather
		 * than compute a deadline from uninitialized fields. */
		deadline->tv_sec = 0;
		deadline->tv_nsec = 0;
	}
	deadline->tv_nsec += (long)((timeout_ms % 1000) * 1000000L);
	if (deadline->tv_nsec >= 1000000000L) {
		deadline->tv_nsec -= 1000000000L;
		seconds++;
	}
	/* Saturate before narrowing seconds, including the nanosecond carry. */
	if (seconds > (uintmax_t)maximum ||
	    deadline->tv_sec > maximum - (time_t)seconds) {
		deadline->tv_sec = maximum;
		deadline->tv_nsec = 999999999L;
	} else {
		deadline->tv_sec += (time_t)seconds;
	}
}

int64_t eli_worker_check_timeout(lua_State *L, int index, int64_t fallback);

/* ---- channels ---- */

typedef enum eli_chan_status {
	ELI_CHAN_OK = 0,
	ELI_CHAN_CLOSED,
	ELI_CHAN_TIMEOUT,
	ELI_CHAN_ERROR
} eli_chan_status;

typedef struct eli_chan_wait eli_chan_wait;

typedef struct eli_channel {
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
	struct eli_channel *next;
	struct eli_channel *prev;
	int gc_refs;
	int gc_mark;
	int collecting;
	struct eli_channel *gc_link; /* mark worklist link */
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
size_t eli_channel_live_count(void);

int eli_channel_push(lua_State *L, eli_channel *channel);
int eli_channel_create(lua_State *L, size_t capacity);
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
