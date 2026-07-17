#ifndef MAINDOB_DOB_REGISTRY_H
#define MAINDOB_DOB_REGISTRY_H

#include <dob/types.h>

/* Find registered service by name.
 * Return service IPC port ID, or 0 if not found. */
uint32_t dob_registry_find(const char *service_name);

/* Find service, waiting up to timeout_ms if not yet registered.
 * Returns port ID, or 0 on timeout. */
uint32_t dob_registry_wait(const char *service_name, uint32_t timeout_ms);

/* Register current program as service with given name.
 * port_id: IPC port it listens on. */
dob_status_t dob_registry_register(const char *name, uint32_t port_id);

/* Unregister a service. Only the owning process can unregister. */
dob_status_t dob_registry_unregister(const char *name);

#endif /* MAINDOB_DOB_REGISTRY_H */
