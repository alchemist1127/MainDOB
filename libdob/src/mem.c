/* libdob mem — .mem (PIC ELF shared object) loader.
 *
 * dob_mem_load asks the kernel to load an ELF ET_DYN i386 shared
 * object into the caller's address space and resolve the well-known
 * exports symbol __mem_exports.  The kernel handles VA allocation,
 * relocation (R_386_RELATIVE), and runs __mem_init if present. */

#include <dob/mem.h>
#include <sys/syscall.h>

typedef void (*mem_init_t)(void);

void *dob_mem_load(const void *data, size_t size)
{
    if (!data || size == 0) return 0;

    uint32_t init_addr = 0;
    int32_t  exports_addr = syscall3(SYS_MEM_LOAD,
                                     (int)data, (int)size, (int)&init_addr);
    if (exports_addr == 0) return 0;

    if (init_addr)
        ((mem_init_t)init_addr)();

    return (void *)exports_addr;
}
