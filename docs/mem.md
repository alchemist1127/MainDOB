# .mem — Userspace Shared Objects

A `.mem` is a PIC ELF shared object that a MainDOB program loads into its
own address space at runtime.  Once loaded, calling a function inside the
`.mem` is a plain `call` — no IPC, no context switch, no kernel trip.

**A loaded `.mem` is fused with the process.**  Its code and data are
indistinguishable from anything else the process allocates.  There is no
unload: the `.mem` lives and dies with the process that loaded it.

This sits alongside the two mechanisms MainDOB already had:

| Mechanism | Isolation | Payload | Call cost | Typical use |
|-----------|-----------|---------|-----------|-------------|
| **EPS** (stubs + IPC) | Full, separate process | Arbitrary | Send/receive/reply | Services, drivers, servers |
| **.mem** (this doc) | None — fused with caller | Up to 16 MB | Direct `call` | Big libraries where IPC is overkill |

Pick `.mem` when you need a *library*, not a *service* — parsers, codecs,
filesystems above a block driver, compression, crypto.

## File layout

A `.mem` is expected to sit **in the same directory as the `.mdl` that
loads it**, and is passed to `dob_mem_load` as a buffer read by the
caller (typically through DobFileSystem).  So if your program lives at

    /SYSTEM/PROGRAMS/DobArchive/DobArchive.mdl

then `compression.mem` is at

    /SYSTEM/PROGRAMS/DobArchive/compression.mem

There is no global library path.  The per-program sandbox in
DobFileSystem already prevents a program from reading another program's
files, so every `.mem` is implicitly scoped to its owner.

## Contract

A `.mem` must export one mandatory symbol:

```c
/* Your public API struct */
typedef struct {
    int  (*compress)  (const void *in, uint32_t n, void *out, uint32_t *out_n);
    int  (*decompress)(const void *in, uint32_t n, void *out, uint32_t *out_n);
} compression_api_t;

__attribute__((visibility("default")))
compression_api_t __mem_exports = {
    .compress   = compress_impl,
    .decompress = decompress_impl,
};
```

Optionally one hook symbol:

```c
void __mem_init(void) { /* called once, right after load, before dob_mem_load returns */ }
```

That's it.  Everything the `.mem` references must be either defined
inside itself or inlined from headers — **no external symbols**.  The
kernel only applies `R_386_RELATIVE` relocations and will refuse a
`.mem` that contains anything else.

You can still make syscalls from inside a `.mem` (you're in ring 3 in
the caller's address space), so you can still talk to drivers and
services through IPC exactly like normal userspace.

## API

One function, returns the exports struct directly:

```c
void *dob_mem_load(const void *data, size_t size);
```

On success: returns a pointer to the `.mem`'s `__mem_exports`.  Cast it
to your API struct type and call.
On failure: returns `NULL`.

No handle, no unload, no state to carry around.  `dob_mem_load` is
idempotent in the sense that calling it twice with the same blob
produces two independent copies at two different addresses — by design.

## Minimal example: `greet.mem`

### Source — `programs/demo/greet.c`

```c
#include <sys/syscall.h>

/* Local debug helper — syscall3 is an inline from <sys/syscall.h>,
 * no external link dependency. */
static int dbg(const char *s) {
    int len = 0; while (s[len]) len++;
    return syscall2(SYS_DEBUG_PRINT, (int)s, len);
}

/* The API struct the caller will cast to. */
typedef struct {
    void (*hello)(void);
    int  (*add)(int a, int b);
} greet_api_t;

static void hello_impl(void) { dbg("[greet.mem] hello!\n"); }
static int  add_impl(int a, int b) { return a + b; }

__attribute__((visibility("default")))
greet_api_t __mem_exports = {
    .hello = hello_impl,
    .add   = add_impl,
};

/* Optional — runs once after load. */
void __mem_init(void) { dbg("[greet.mem] __mem_init\n"); }
```

### Makefile wiring

Add to `programs/Makefile`:

```make
$(BUILD_DIR)/programs/greet.mem: $(ROOT_DIR)/programs/demo/greet.c
$(BUILD_DIR)/programs/demo.mdl:  $(BUILD_DIR)/programs/greet.mem
```

The generic `%.mem` rule at the bottom of `programs/Makefile` handles
the compile (`-fPIC -shared -nostdlib`).

### Caller — `programs/demo/main.c`

```c
#include <stdio.h>
#include <stdlib.h>
#include <dob/mem.h>
#include <stubs/DobFileSystem.h>

typedef struct {
    void (*hello)(void);
    int  (*add)(int a, int b);
} greet_api_t;

static void *slurp(const char *path, size_t *out_size) {
    int fd = dobfs.Open(path, FS_READ);
    if (fd < 0) return NULL;
    uint32_t sz = dobfs.Size(fd);
    void *buf = malloc(sz);
    dobfs.Read(fd, buf, sz);
    dobfs.Close(fd);
    *out_size = sz;
    return buf;
}

int main(void) {
    size_t sz;
    void *blob = slurp("greet.mem", &sz);   /* relative to program's home_dir */
    if (!blob) return 1;

    greet_api_t *api = dob_mem_load(blob, sz);
    free(blob);                             /* kernel already copied it out */
    if (!api) return 2;

    api->hello();
    printf("2 + 3 = %d\n", api->add(2, 3));

    /* Nothing to release.  Program exits, .mem goes with it. */
    return 0;
}
```

## Limits and gotchas

**No fault isolation.** If the `.mem` dereferences a null pointer, the
process that loaded it dies.  The `.mem` has full access to the caller's
stack, heap, and globals.  This is the price of the direct call.

**No external symbols.** The `.mem` must be self-contained.  If you need
`memcpy`, either write it inside the `.mem` or lift the one from
`libc/src/string.c` into a header as `static inline`.

**No unload.** Once loaded, a `.mem` lives until the process exits.  If
you need to "replace" a `.mem` with a new version, exit the process and
spawn a fresh one — that's the only honest way to do it, since any
pointer into the old `.mem` could still be held somewhere in the
caller's state.

**Size cap: 16 MB.** Hardcoded in `kernel/proc/elf.c` (`MEM_MAX_BLOB`).
Raise it if you have a legitimate need and the RAM budget.

**Per-process mapping.** Two processes loading the same `.mem` each get
their own private copy of every page.  No shared text, no refcount
trickery.  If this becomes a pain we'll revisit — for now it keeps the
loader simple.

**No lazy binding.** Relocations are processed once at `dob_mem_load`
time.  The first call is not slower than the thousandth.

**`__mem_exports` is plain data.** The struct is laid out in the `.mem`'s
`.data` segment.  If you edit its fields at runtime, you're editing your
own private mapping — other processes that loaded the same `.mem` see
their own copies.

## Syscall surface

One new syscall:

| Number | Name | Arguments | Returns |
|--------|------|-----------|---------|
| 96 | `SYS_MEM_LOAD` | `ebx=data`, `ecx=size`, `edx=out_init_addr` (optional, or NULL) | absolute address of `__mem_exports`, or 0 on failure |

`out_init_addr` on success: absolute address of `__mem_init`, or 0 if
the `.mem` does not export one.  libdob calls it, if present, before
returning to the user.

## Cleanup

None to speak of.  A loaded `.mem` is ordinary process memory from the
kernel's point of view: its pages sit in the process's page directory,
its virtual range is a vregion like any other.  When the process exits,
`paging_destroy_directory` reclaims the frames and `vregion_destroy`
reclaims the metadata, same path as the program's own `.text` and heap.
