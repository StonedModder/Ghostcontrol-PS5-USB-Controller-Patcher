/* Private resident process correlation, not physical admission. */
#ifndef GC_SERVICE_SESSION_PRIVATE_H
#define GC_SERVICE_SESSION_PRIVATE_H
#include <stddef.h>
int gc_service_inject_process_session(char *out, size_t capacity);
#endif
