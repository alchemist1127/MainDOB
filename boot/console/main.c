/* MainDOB Console Server — VGA Text Mode Driver Bubble
 *
 * This is a proper Dob bubble: it maps VGA framebuffer memory via MMIO
 * (sys_mmap_phys) and writes directly. No VGA syscalls in the kernel.
 * Programs write to the console through DobConsole Entry Point stubs.
 *
 * The old design had VGA_CLEAR/VGA_PUTCHAR as kernel syscalls — that's
 * the opposite of Dob. In Dob, hardware access goes through isolated
 * driver bubbles. If this server crashes, hotplug restarts it. The
 * kernel VGA code (kprintf) is only used for early boot and panics.
 *
 * Protocol:
 *   code=1 WRITE       payload=text
 *   code=2 CLEAR
 *   code=3 SET_COLOR   arg0=fg arg1=bg
 */

#include <unistd.h>
#include <string.h>
#include <dob/server.h>
#include <dob/ipc.h>
#include <dob/types.h>

/* VGA text mode constants */
#define VGA_PHYS_ADDR   0xB8000
#define VGA_WIDTH       80
#define VGA_HEIGHT      25
#define VGA_SIZE        (VGA_WIDTH * VGA_HEIGHT * 2)

/* CGA/VGA I/O ports for cursor control */
#define VGA_CTRL_PORT   0x3D4
#define VGA_DATA_PORT   0x3D5

/* VGA framebuffer mapped into our address space */
static volatile uint16_t *vga_buf = NULL;
static uint32_t vga_row = 0;
static uint32_t vga_col = 0;
static uint8_t  vga_attr = 0x07;  /* light grey on black */

/* === VGA hardware tools === */

static uint16_t vga_entry(char c, uint8_t attr)
{
    return (uint16_t)c | ((uint16_t)attr << 8);
}

static void vga_update_cursor(void)
{
    uint16_t pos = vga_row * VGA_WIDTH + vga_col;
    io_outb(VGA_CTRL_PORT, 0x0F);
    io_outb(VGA_DATA_PORT, (uint8_t)(pos & 0xFF));
    io_outb(VGA_CTRL_PORT, 0x0E);
    io_outb(VGA_DATA_PORT, (uint8_t)((pos >> 8) & 0xFF));
}

static void vga_scroll(void)
{
    for (uint32_t y = 0; y < VGA_HEIGHT - 1; y++)
        for (uint32_t x = 0; x < VGA_WIDTH; x++)
            vga_buf[y * VGA_WIDTH + x] = vga_buf[(y + 1) * VGA_WIDTH + x];

    for (uint32_t x = 0; x < VGA_WIDTH; x++)
        vga_buf[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ', vga_attr);
}

static void vga_putchar(char c)
{
    if (!vga_buf) return;

    if (c == '\n')
    {
        vga_col = 0;
        vga_row++;
    }
    else if (c == '\r')
    {
        vga_col = 0;
    }
    else if (c == '\b')
    {
        if (vga_col > 0)
        {
            vga_col--;
            vga_buf[vga_row * VGA_WIDTH + vga_col] = vga_entry(' ', vga_attr);
        }
    }
    else if (c == '\t')
    {
        vga_col = (vga_col + 8) & ~7;
        if (vga_col >= VGA_WIDTH) { vga_col = 0; vga_row++; }
    }
    else
    {
        vga_buf[vga_row * VGA_WIDTH + vga_col] = vga_entry(c, vga_attr);
        vga_col++;
        if (vga_col >= VGA_WIDTH) { vga_col = 0; vga_row++; }
    }

    while (vga_row >= VGA_HEIGHT)
    {
        vga_scroll();
        vga_row--;
    }
}

static void vga_write_str(const char *str, uint32_t len)
{
    for (uint32_t i = 0; i < len && str[i]; i++)
        vga_putchar(str[i]);
    vga_update_cursor();
}

static void vga_clear(void)
{
    if (!vga_buf) return;
    for (uint32_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        vga_buf[i] = vga_entry(' ', vga_attr);
    vga_row = 0;
    vga_col = 0;
    vga_update_cursor();
}

static void vga_set_color(int fg, int bg)
{
    vga_attr = (uint8_t)((bg & 0x0F) << 4 | (fg & 0x0F));
}

/* === Init hardware === */

static int vga_init_hw(void)
{
    /* Map VGA framebuffer into our address space via MMIO.
     * This requires driver status (granted by hotplug or init). */
    void *virt = mmap_phys(VGA_PHYS_ADDR, VGA_SIZE);
    if (!virt)
    {
        debug_print("[console] FATAL: Cannot map VGA framebuffer\n");
        return -1;
    }
    vga_buf = (volatile uint16_t *)virt;

    /* Read current cursor position from hardware */
    io_outb(VGA_CTRL_PORT, 0x0F);
    uint16_t pos = io_inb(VGA_DATA_PORT);
    io_outb(VGA_CTRL_PORT, 0x0E);
    pos |= (uint16_t)io_inb(VGA_DATA_PORT) << 8;

    vga_row = pos / VGA_WIDTH;
    vga_col = pos % VGA_WIDTH;
    if (vga_row >= VGA_HEIGHT) vga_row = VGA_HEIGHT - 1;

    return 0;
}

/* === IPC handler === */

static dob_status_t handle_message(dob_msg_t *msg, dob_msg_t *reply)
{
    (void)reply;
    switch (msg->code)
    {
        case 1: /* WRITE */
            if (msg->payload && msg->payload_size > 0)
                vga_write_str((const char *)msg->payload, msg->payload_size);
            return DOB_OK;

        case 2: /* CLEAR */
            vga_clear();
            return DOB_OK;

        case 3: /* SET_COLOR */
            vga_set_color((int)msg->arg0, (int)msg->arg1);
            return DOB_OK;

        default:
            if (msg->payload && msg->payload_size > 0)
                vga_write_str((const char *)msg->payload, msg->payload_size);
            return DOB_OK;
    }
}

/* === Main algorithm === */

int main(void)
{
    debug_print("[console] Starting console VGA bubble...\n");

    if (vga_init_hw() < 0)
        _exit(1);

    dob_server_init("console");
    dob_server_register(handle_message);

    debug_print("[console] Console bubble ready (VGA MMIO mapped).\n");
    dob_server_loop();
    return 0;
}
