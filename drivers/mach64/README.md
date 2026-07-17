# MainDOB ATI Mach64 video driver — v30 "CRTC_OFF_PITCH bug fix"

Target: **Rage Mobility-P** (PCI 1002:4C4D) on the **Compaq Armada E500**.

## Audit finding — bug present since v15

Following the methodology "controlla i driver ufficiali, cerca cosa
facciamo di diverso", an offset-pitch macro was audited against
`set_off_pitch()` in `drivers/video/fbdev/aty/atyfb_base.c`:

### atyfb (correct, real hardware)

```c
u32 pitch = info->fix.line_length / 8;   /* line_length in BYTES */
aty_st_le32(CRTC_OFF_PITCH, line | (pitch << 22), par);
```

For 32 bpp at 1024 px: `line_length = 4096 bytes`, `pitch = 512` qword.

### MainDOB v15..v29 (buggy)

```c
#define M64_OFF_PITCH(byte_off, pitch_pixels) \
    (((uint32_t)(byte_off) >> 3) | (((uint32_t)(pitch_pixels) >> 3) << 22))
```

For 32 bpp at 1024 px: `M64_OFF_PITCH(0, 1024) = 128` qword. **Wrong by
factor of 4** — the macro assumed 1 px = 1 byte (true only for 8 bpp).

### What the chip saw

With pitch field = 128 qword:
- 128 qword/scanline = **1024 bytes per fb scanline read**
- At 32 bpp: only **256 pixels per scanline** read from the framebuffer
- Panel scans 1024 px → chip shows 4 horizontal slices of 256 px each,
  each slice from a different fb row, on every panel scanline

This dominated every photo from v22 onward. It explains:
- The "wide white area" on the left (= 256 px replicated of bar 0)
- The "yellow thin lines" (= small slices of bar 1)
- The "compressed bars" on the right (= replicated slices of later bars)
- Apparent moiré (panel showing 4× compressed information)

## v30 — minimal fix, maximum isolation

This iteration applies **only** the pitch fix and **removes** all PLL
reprogram code introduced in v28/v29. Rationale:

1. The pitch bug was so dominant that any effect from PLL writes was
   masked. We can't tell whether v28/v29 PLL writes had any effect
   until pitch is correct.
2. Single change → unambiguous diagnostic. If v30 changes the pattern
   significantly, the pitch was the primary issue.
3. v31 will re-add VCLK reprogram if v30 still shows V-blank evidence.

What stays: CRTC envelope kill/restart, 1024×768 H/V_TOTAL_DISP write,
bit-11 BYTE_PIX_ORDER clear, LCD stretcher disable, 32 bpp, DAC 8-bit,
identity palette, EBU 8-bar pattern.

## What we expect

| Observation                                | Diagnosis                                     |
|--------------------------------------------|-----------------------------------------------|
| 8 clean equal-width EBU bars full panel    | 🎉 Pitch was THE bug — bring-up done          |
| 8 bars visible but image scrolls vertically| Pitch correct, V_TOTAL slightly off           |
| 8 bars but mid-screen V-blank line stays   | Pitch fixed; VCLK still wrong → v31 PLL reprog|
| Pattern still has 4× replication           | Pitch macro not the bug, or didn't take       |
| Panel goes dark                            | Pitch×4 increase puts chip past memory range  |

## Math check

For 1024×768 32 bpp:
- Bytes/scanline = 1024 × 4 = 4096
- Qword/scanline = 4096 / 8 = 512
- Total fb size = 4096 × 768 = 3,145,728 bytes = 3 MB (fits in 8 MB BAR)
- `M64_OFF_PITCH(0, g_fb_w * 4u) = M64_OFF_PITCH(0, 4096)`
- Result: `(0 >> 3) | ((4096 >> 3) << 22) = 0 | (512 << 22) = 0x80000000`

Match what atyfb computes: ✓
