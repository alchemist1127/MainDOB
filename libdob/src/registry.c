/* MainDOB Registry Client Library
 *
 * Communicates with the in-kernel registry via syscalls.
 * No IPC, no polling, no userspace registry process.
 */

#include <dob/registry.h>
#include <sys/syscall.h>

uint32_t dob_registry_find(const char *service_name)
{
    if (!service_name) return 0;
    return (uint32_t)syscall2(SYS_REG_FIND, (int)service_name, 0);
}

uint32_t dob_registry_wait(const char *service_name, uint32_t timeout_ms)
{
    if (!service_name) return 0;
    return (uint32_t)syscall2(SYS_REG_WAIT, (int)service_name, (int)timeout_ms);
}

dob_status_t dob_registry_register(const char *name, uint32_t port_id)
{
    if (!name) return DOB_ERR_INVALID;
    int32_t ret = syscall2(SYS_REG_REGISTER, (int)name, (int)port_id);
    return ret == 0 ? DOB_OK : DOB_ERR_INTERNAL;
}

dob_status_t dob_registry_unregister(const char *name)
{
    if (!name) return DOB_ERR_INVALID;
    int32_t ret = syscall2(SYS_REG_UNREGISTER, (int)name, 0);
    return ret == 0 ? DOB_OK : DOB_ERR_NOT_FOUND;
}
