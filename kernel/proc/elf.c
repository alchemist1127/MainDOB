#include "proc/elf.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "mm/vregion.h"
#include "lib/string.h"
#include "console/console.h"
#include "krt/uaccess.h"   /* USER_SPACE_LIMIT */
#include "kernel.h"

#define ELF_MAGIC       0x464C457Fu     /* 0x7F 'E' 'L' 'F'               */
#define ET_EXEC         2
#define EM_386          3
#define PT_LOAD         1

typedef struct __attribute__((packed))
{
    uint32_t magic;
    uint8_t  class;         /* 1 = 32-bit                                 */
    uint8_t  data;
    uint8_t  version;
    uint8_t  osabi;
    uint8_t  pad[8];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
} elf_header_t;

typedef struct __attribute__((packed))
{
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
} elf_phdr_t;

/* === Verbi ============================================================== */

static bool header_is_valid(const elf_header_t *h, uint32_t size)
{
    if (size < sizeof(elf_header_t))
    {
        return false;
    }
    if (h->magic != ELF_MAGIC || h->class != 1)
    {
        return false;
    }
    if (h->type != ET_EXEC || h->machine != EM_386)
    {
        return false;
    }
    if (h->phoff + (uint32_t)h->phnum * sizeof(elf_phdr_t) > size)
    {
        return false;
    }
    return true;
}

static bool segment_is_sane(const elf_phdr_t *ph, uint32_t file_size)
{
    if (ph->vaddr + ph->memsz < ph->vaddr)
    {
        return false;                   /* wrap 32-bit                    */
    }
    if (ph->vaddr + ph->memsz > USER_SPACE_LIMIT)
    {
        return false;                   /* sconfina nel kernel            */
    }
    if (ph->filesz > ph->memsz)
    {
        return false;
    }
    if ((uint64_t)ph->offset + ph->filesz > file_size)
    {
        return false;
    }
    return true;
}

/* Mappa e popola un segmento SENZA cli lungo: ogni pagina e' allocata,
 * mappata con paging_map_in (CR3 commutato per la sola PTE), e riempita
 * scrivendo nel PD del figlio via una finestra temporanea kernel. */
static bool load_segment(process_t *proc, const elf_phdr_t *ph,
                         const uint8_t *file)
{
    uint32_t seg_start = ALIGN_DOWN(ph->vaddr, PAGE_SIZE);
    uint32_t seg_end   = ALIGN_UP(ph->vaddr + ph->memsz, PAGE_SIZE);
    uint32_t pages     = (seg_end - seg_start) / PAGE_SIZE;

    vregion_insert(&proc->vm_regions, seg_start, pages,
                   VREG_CODE | VREG_USER_RW | VREG_EXEC);

    for (uint32_t off = 0; off < pages * PAGE_SIZE; off += PAGE_SIZE)
    {
        uint32_t frame = pmm_alloc_frame(PMM_ZONE_ANY);
        if (frame == 0)
        {
            return false;
        }

        /* Azzera + copia attraverso il direct-map kernel del frame,
         * poi mappalo nel figlio. Nessun CR3 del figlio caricato mentre
         * copiamo dati (che dipendono dall'input): rispetta D6. */
        uint8_t *kwin = (uint8_t *)(frame + KERNEL_VMA);
        memset(kwin, 0, PAGE_SIZE);

        uint32_t page_vaddr = seg_start + off;
        if (page_vaddr + PAGE_SIZE > ph->vaddr &&
            page_vaddr < ph->vaddr + ph->filesz)
        {
            uint32_t copy_start = (page_vaddr > ph->vaddr)
                                ? page_vaddr : ph->vaddr;
            uint32_t copy_end   = ph->vaddr + ph->filesz;
            if (copy_end > page_vaddr + PAGE_SIZE)
            {
                copy_end = page_vaddr + PAGE_SIZE;
            }
            uint32_t dst_off = copy_start - page_vaddr;
            uint32_t src_off = copy_start - ph->vaddr;
            memcpy(kwin + dst_off, file + ph->offset + src_off,
                   copy_end - copy_start);
        }

        paging_map_in(proc->page_directory, page_vaddr, frame,
                      PTE_PRESENT | PTE_WRITABLE | PTE_USER);
    }
    return true;
}

/* === API ================================================================ */

elf_load_result_t elf_load(process_t *proc, const void *data, uint32_t size)
{
    elf_load_result_t result = { false, 0, 0 };

    const elf_header_t *h = (const elf_header_t *)data;
    if (!header_is_valid(h, size))
    {
        kprintf("[ELF ] header non valido\n");
        return result;
    }

    const uint8_t *file = (const uint8_t *)data;
    const elf_phdr_t *ph = (const elf_phdr_t *)(file + h->phoff);
    uint32_t brk = 0;

    for (uint16_t i = 0; i < h->phnum; i++)
    {
        if (ph[i].type != PT_LOAD || ph[i].memsz == 0)
        {
            continue;
        }
        if (!segment_is_sane(&ph[i], size))
        {
            kprintf("[ELF ] segmento %u non valido\n", i);
            return result;
        }
        if (!load_segment(proc, &ph[i], file))
        {
            kprintf("[ELF ] OOM caricando il segmento %u\n", i);
            return result;
        }
        uint32_t end = ph[i].vaddr + ph[i].memsz;
        if (end > brk)
        {
            brk = end;
        }
    }

    result.success     = true;
    result.entry_point = h->entry;
    result.brk         = ALIGN_UP(brk, PAGE_SIZE);
    return result;
}

/* === Oggetti condivisi .mem (ET_DYN) ==================================== */

#define ET_DYN          3
#define PT_DYNAMIC      2
#define DT_NULL         0
#define DT_HASH         4
#define DT_STRTAB       5
#define DT_SYMTAB       6
#define DT_STRSZ        10
#define DT_REL          17
#define DT_RELSZ        18
#define DT_RELENT       19
#define R_386_NONE      0
#define R_386_RELATIVE  8
#define SHARED_VA_LO    0x60000000u
#define SHARED_VA_HI    0x6FFFFFFFu
#define ELF32_R_TYPE(i) ((uint8_t)((i) & 0xFF))

typedef struct __attribute__((packed))
{
    int32_t  d_tag;
    uint32_t d_val;
} elf32_dyn_t;

typedef struct __attribute__((packed))
{
    uint32_t r_offset;
    uint32_t r_info;
} elf32_rel_t;

typedef struct __attribute__((packed))
{
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
} elf32_sym_t;

/* === Verbi (mappano/scrivono nel PD del figlio via paging_map_in) ======= */

static bool shared_span(const elf_header_t *h, const uint8_t *file,
                        uint32_t size, uint32_t *min_v, uint32_t *max_v)
{
    const elf_phdr_t *ph = (const elf_phdr_t *)(file + h->phoff);
    uint32_t lo = UINT32_MAX, hi = 0;

    for (uint16_t i = 0; i < h->phnum; i++)
    {
        if (ph[i].type != PT_LOAD || ph[i].memsz == 0)
        {
            continue;
        }
        if ((uint64_t)ph[i].offset + ph[i].filesz > size)
        {
            return false;
        }
        if (ph[i].vaddr < lo)
        {
            lo = ph[i].vaddr;
        }
        if (ph[i].vaddr + ph[i].memsz > hi)
        {
            hi = ph[i].vaddr + ph[i].memsz;
        }
    }
    if (lo == UINT32_MAX || hi <= lo)
    {
        return false;
    }
    *min_v = ALIGN_DOWN(lo, PAGE_SIZE);
    *max_v = ALIGN_UP(hi, PAGE_SIZE);
    return true;
}

static bool shared_map_segments(process_t *proc, const elf_header_t *h,
                                const uint8_t *file, uint32_t load_bias)
{
    const elf_phdr_t *ph = (const elf_phdr_t *)(file + h->phoff);

    for (uint16_t i = 0; i < h->phnum; i++)
    {
        if (ph[i].type != PT_LOAD || ph[i].memsz == 0)
        {
            continue;
        }
        uint32_t actual = ph[i].vaddr + load_bias;
        uint32_t start  = ALIGN_DOWN(actual, PAGE_SIZE);
        uint32_t end    = ALIGN_UP(actual + ph[i].memsz, PAGE_SIZE);

        for (uint32_t off = 0; start + off < end; off += PAGE_SIZE)
        {
            uint32_t va = start + off;
            if (paging_get_physical_in(proc->page_directory, va) != 0)
            {
                continue;               /* segmenti sovrapposti in pagina */
            }
            uint32_t frame = pmm_alloc_frame(PMM_ZONE_ANY);
            if (frame == 0)
            {
                return false;
            }
            memset((void *)(frame + KERNEL_VMA), 0, PAGE_SIZE);
            paging_map_in(proc->page_directory, va, frame,
                          PTE_PRESENT | PTE_WRITABLE | PTE_USER);
        }

        /* Copia filesz nel frame via il suo alias fisico kernel (mai col
         * CR3 del figlio caricato durante una copia input-dipendente: D6). */
        for (uint32_t done = 0; done < ph[i].filesz; )
        {
            uint32_t va   = actual + done;
            uint32_t frame = paging_get_physical_in(proc->page_directory,
                                                    va & ~0xFFFu);
            uint32_t poff = va & 0xFFFu;
            uint32_t chunk = PAGE_SIZE - poff;
            if (chunk > ph[i].filesz - done)
            {
                chunk = ph[i].filesz - done;
            }
            memcpy((uint8_t *)((frame & ~0xFFFu) + KERNEL_VMA) + poff,
                   file + ph[i].offset + done, chunk);
            done += chunk;
        }
    }
    return true;
}

/* Legge/scrive una parola nel PD del figlio, via l'alias fisico kernel. */
static bool child_peek(process_t *proc, uint32_t va, uint32_t *out)
{
    uint32_t phys = paging_get_physical_in(proc->page_directory,
                                           va & ~0xFFFu);
    if (phys == 0)
    {
        return false;
    }
    memcpy(out, (const void *)(((phys & ~0xFFFu) + KERNEL_VMA) + (va & 0xFFFu)),
           4);
    return true;
}

static void child_poke_add(process_t *proc, uint32_t va, uint32_t addend)
{
    uint32_t phys = paging_get_physical_in(proc->page_directory,
                                           va & ~0xFFFu);
    if (phys == 0)
    {
        return;
    }
    uint32_t *slot =
        (uint32_t *)(((phys & ~0xFFFu) + KERNEL_VMA) + (va & 0xFFFu));
    *slot += addend;
}

elf_shared_result_t elf_load_shared(process_t *proc, const void *data,
                                    uint32_t size)
{
    elf_shared_result_t r = { 0, 0, 0, false };

    if (size < sizeof(elf_header_t))
    {
        return r;
    }
    const elf_header_t *h = (const elf_header_t *)data;
    const uint8_t *file = (const uint8_t *)data;

    if (h->magic != ELF_MAGIC || h->class != 1 || h->type != ET_DYN ||
        h->machine != EM_386 || h->phoff == 0 || h->phnum == 0)
    {
        kprintf("[MEM ] non e' un oggetto condiviso i386 valido\n");
        return r;
    }

    uint32_t min_v, max_v;
    if (!shared_span(h, file, size, &min_v, &max_v))
    {
        return r;
    }
    uint32_t pages = (max_v - min_v) / PAGE_SIZE;
    if (pages == 0 || pages > 4096)
    {
        return r;
    }

    /* Stesso range e stesso allocatore delle SHM: non collidono mai. */
    uint32_t base = vregion_alloc(&proc->vm_regions, pages,
                                  SHARED_VA_LO, SHARED_VA_HI,
                                  VREG_MMAP | VREG_USER_RW, -1);
    if (base == 0)
    {
        return r;
    }
    uint32_t load_bias = base - min_v;

    if (!shared_map_segments(proc, h, file, load_bias))
    {
        vregion_free(&proc->vm_regions, base);
        return r;
    }

    /* PT_DYNAMIC -> DT_REL/RELSZ + SYMTAB/STRTAB/HASH, tutti letti come
     * offset caricati (base + val). Solo R_386_RELATIVE, con bounds sul
     * target (dentro [base, base+span)): un .mem ostile non puo' far
     * scrivere in kernel space (siamo col PD del figlio, ring 0). */
    const elf_phdr_t *ph = (const elf_phdr_t *)(file + h->phoff);
    uint32_t dyn_va = 0, dyn_bytes = 0;
    for (uint16_t i = 0; i < h->phnum; i++)
    {
        if (ph[i].type == PT_DYNAMIC)
        {
            dyn_va = ph[i].vaddr + load_bias;
            dyn_bytes = ph[i].memsz;
            break;
        }
    }
    if (dyn_va == 0)
    {
        kprintf("[MEM ] nessun PT_DYNAMIC\n");
        vregion_free(&proc->vm_regions, base);
        return r;
    }

    uint32_t rel = 0, relsz = 0, relent = sizeof(elf32_rel_t);
    uint32_t symtab = 0, strtab = 0, strsz = 0, hash = 0;
    uint32_t dyn_count = dyn_bytes / sizeof(elf32_dyn_t);

    for (uint32_t i = 0; i < dyn_count; i++)
    {
        uint32_t tag, val;
        if (!child_peek(proc, dyn_va + i * 8u, &tag) ||
            !child_peek(proc, dyn_va + i * 8u + 4u, &val))
        {
            break;
        }
        if ((int32_t)tag == DT_NULL) break;
        switch ((int32_t)tag)
        {
            case DT_REL:    rel    = val + load_bias; break;
            case DT_RELSZ:  relsz  = val; break;
            case DT_RELENT: relent = val; break;
            case DT_SYMTAB: symtab = val + load_bias; break;
            case DT_STRTAB: strtab = val + load_bias; break;
            case DT_STRSZ:  strsz  = val; break;
            case DT_HASH:   hash   = val + load_bias; break;
        }
    }

    uint32_t reloc_lo = base;
    uint32_t reloc_hi = base + (max_v - min_v);
    if (rel != 0 && relsz > 0 && relent >= sizeof(elf32_rel_t) && relent <= 64)
    {
        for (uint32_t off = 0; off + relent <= relsz; off += relent)
        {
            uint32_t r_offset, r_info;
            if (!child_peek(proc, rel + off, &r_offset) ||
                !child_peek(proc, rel + off + 4u, &r_info))
            {
                break;
            }
            uint8_t type = ELF32_R_TYPE(r_info);
            if (type == R_386_NONE)
            {
                continue;
            }
            if (type != R_386_RELATIVE)
            {
                kprintf("[MEM ] reloc non supportata (tipo %u)\n", type);
                vregion_free(&proc->vm_regions, base);
                return r;
            }
            uint32_t target = r_offset + load_bias;
            if (target < reloc_lo || target > reloc_hi - 4u)
            {
                kprintf("[MEM ] reloc fuori range 0x%08x\n", target);
                vregion_free(&proc->vm_regions, base);
                return r;
            }
            child_poke_add(proc, target, load_bias);
        }
    }

    /* __mem_exports / __mem_init per scansione lineare dei simboli
     * (nsym da HASH[1]=nchain). Ogni st_name limitato da strsz e la
     * lunghezza capata: un offset ostile non fa uscire da strtab. */
    if (symtab == 0 || strtab == 0 || hash == 0 || strsz == 0)
    {
        kprintf("[MEM ] SYMTAB/STRTAB/HASH/STRSZ mancanti\n");
        vregion_free(&proc->vm_regions, base);
        return r;
    }
    uint32_t nsym = 0;
    child_peek(proc, hash + 4u, &nsym);         /* HASH[1] = nchain       */
    if (nsym == 0 || nsym > 16384)
    {
        vregion_free(&proc->vm_regions, base);
        return r;
    }

    uint32_t exports_off = 0, init_off = 0;
    for (uint32_t i = 1; i < nsym; i++)
    {
        uint32_t st_name, st_value;
        if (!child_peek(proc, symtab + i * 16u, &st_name) ||
            !child_peek(proc, symtab + i * 16u + 4u, &st_value))
        {
            break;
        }
        if (st_name == 0 || st_name >= strsz)
        {
            continue;
        }
        char name[32];
        uint32_t maxlen = strsz - st_name;
        if (maxlen > sizeof(name))
        {
            maxlen = sizeof(name);
        }
        for (uint32_t k = 0; k < maxlen; k++)
        {
            uint32_t word;
            child_peek(proc, (strtab + st_name + k) & ~3u, &word);
            name[k] = (char)(word >> (((strtab + st_name + k) & 3u) * 8u));
            if (name[k] == '\0')
            {
                break;
            }
        }
        name[sizeof(name) - 1] = '\0';

        if (strcmp(name, "__mem_exports") == 0)
        {
            exports_off = st_value;
        }
        else if (strcmp(name, "__mem_init") == 0)
        {
            init_off = st_value;
        }
    }

    if (exports_off == 0)
    {
        kprintf("[MEM ] __mem_exports non trovato\n");
        vregion_free(&proc->vm_regions, base);
        return r;
    }

    r.base           = base;
    r.exports_offset = exports_off;
    r.init_offset    = init_off;
    r.success        = true;
    kprintf("[MEM ] caricato @ 0x%08x (%u pagine), exports +0x%x\n",
            base, pages, exports_off);
    return r;
}
