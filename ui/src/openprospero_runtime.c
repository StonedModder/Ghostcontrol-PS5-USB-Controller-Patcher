/* SPDX-License-Identifier: GPL-3.0-or-later
 * Explicit bootstrap uses the same SDK client as application autostart.
 * Override its weak configuration so both paths address the packaged daemon.
 */
#include <openprospero/companion.h>

#ifndef GC_OPENPROSPERO_SERVICE_PORT
#define GC_OPENPROSPERO_SERVICE_PORT 9069
#endif
uint16_t op_companion_service_port = GC_OPENPROSPERO_SERVICE_PORT;
const char *op_companion_payload_path = "/app0/openprospero/companion.bin";

/* Mesa's optional TLS initializer has no work for this static runtime. */
void gc_glapi_tls_init(void) __asm__("_ZTH23_mesa_glapi_tls_Context");
void gc_glapi_tls_init(void) {}

/* The OpenProspero CRT does not hold the splash-screen API behind a wrapper.
 * The kit still calls this optional release hook before the real API. */
void hui_release_splash(void) {}
