#ifndef ELI_XFER_H
#define ELI_XFER_H

/*
 * Versioned native userdata-transfer API.
 *
 * Packets are native-owned snapshots of a Lua value graph. They contain no
 * borrowed Lua pointers and can therefore be moved between independent Lua
 * states (worker threads). Adapters are immutable, process-lifetime
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

eli_xfer_packet *eli_xfer_packet_new(void);
void eli_xfer_packet_free(eli_xfer_packet *packet);
size_t eli_xfer_packet_roots(const eli_xfer_packet *packet);

/* Encode 'count' stack values (at the given absolute or relative indices)
 * as new roots of 'packet'. Object identity (tables, functions, registered
 * userdata) is preserved across all calls sharing one packet.
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

#endif /* ELI_XFER_H */
