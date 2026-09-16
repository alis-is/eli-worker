#ifndef ELI_XFER_H
#define ELI_XFER_H

/*
 * Versioned native userdata-transfer API.
 *
 * Finished packets are native-owned snapshots of a Lua value graph with no
 * borrowed Lua pointers, movable between independent Lua states (worker
 * threads). Adapters are immutable, process-lifetime
 * definitions; each state registers its userdata metatable explicitly.
 */

#include "lua.h"

#include <stddef.h>

#define ELI_XFER_API_VERSION 1

typedef struct eli_xfer_packet eli_xfer_packet;

typedef struct eli_xfer_adapter {
	unsigned int api_version; /* must be ELI_XFER_API_VERSION */
	const char *type_name;    /* unique, process lifetime */

	/* Export userdata at 'index' into a native payload. The payload is
	 * owned by the packet and released through cleanup_fn. Return 0 on
	 * success; on failure push an error string and return nonzero. */
	int (*export_fn)(lua_State *L, int index, void **data, size_t *size);

	/* Create the destination-local userdata (including its own metatable)
	 * and push it. Return 0 on success; on failure push an error string
	 * and return nonzero. */
	int (*import_fn)(lua_State *L, const void *data, size_t size);

	/* Free a payload. Never calls Lua. */
	void (*cleanup_fn)(void *data, size_t size);
} eli_xfer_adapter;

/* Register 'adapter' for the userdata metatable at 'metatable_index'.
 * Raises on invalid adapter definitions. */
int eli_xfer_register(lua_State *L, int metatable_index,
		      const eli_xfer_adapter *adapter);

typedef struct eli_xfer_payload_hooks {
	void (*begin)(void);
	void (*end)(void);
} eli_xfer_payload_hooks;

eli_xfer_packet *eli_xfer_packet_new(void);
/* Optional hooks invoked around the destruction of a packet's payloads.
 * They let an adapter batch process-wide bookkeeping across the many
 * payloads of one packet (for example channel cycle collection). Both
 * fields must be set. 'hooks' must have static storage duration; it is
 * published atomically and may be installed while other threads already
 * free packets. Set once for the process lifetime. */
void eli_xfer_set_payload_hooks(const eli_xfer_payload_hooks *hooks);
/* End encoding on the source thread before handoff or closing its Lua state.
 * Releases source roots and identity map; idempotent, never allocates/raises.
 * Requires one spare stack slot on the source Lua thread. No further encoding
 * is allowed. Free also finishes an unfinished packet, and must therefore run
 * on its source thread while that state is alive. */
void eli_xfer_encode_finish(eli_xfer_packet *packet);
void eli_xfer_packet_free(eli_xfer_packet *packet);
size_t eli_xfer_packet_roots(const eli_xfer_packet *packet);

/* Visit every native userdata payload in a packet. The visitor must not free
 * or otherwise mutate the payload. */
typedef void (*eli_xfer_userdata_visitor)(const eli_xfer_adapter *adapter,
					  void *data, size_t size, void *context);
void eli_xfer_packet_visit_userdata(const eli_xfer_packet *packet,
				    eli_xfer_userdata_visitor visitor, void *context);

/* Encode 'count' stack values (at the given absolute or relative indices)
 * as new roots of 'packet'. Object identity (tables, functions, registered
 * userdata) is preserved across all calls sharing one packet. Calls must use
 * the same Lua thread; mapped objects and that thread are rooted until finish.
 * Returns 0 on success. On failure writes an error message to 'errbuf' and
 * returns nonzero; the packet may hold partially built data and must still
 * be freed. */
int eli_xfer_encode(lua_State *L, eli_xfer_packet *packet, const int *indices,
		    size_t count, char *errbuf, size_t errlen);

/* Decode every root of 'packet' onto the stack. Returns 0 on success. On
 * failure writes an error message to 'errbuf' and returns nonzero; the
 * caller must reset the stack. */
int eli_xfer_decode(lua_State *L, const eli_xfer_packet *packet, char *errbuf,
		    size_t errlen);

/* Like eli_xfer_decode, but merges root tables into the table at
 * 'target_index' instead of creating new ones, so references back to the
 * root keep their identity when the target is the destination globals
 * table. Non-table roots are decoded onto the stack as usual. */
int eli_xfer_decode_into(lua_State *L, const eli_xfer_packet *packet,
			 int target_index, char *errbuf, size_t errlen);

#endif /* ELI_XFER_H */
