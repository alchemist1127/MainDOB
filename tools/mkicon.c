/* mkicon.c — generate a .dobicon blob (or a C array) for MainDOB.
 *
 * MainDOB programs ship their launcher icon inside the .mdl as a
 * ".dobicon" ELF section. The Module Manager reads it at display
 * time. This host tool builds that blob from a simple input image.
 *
 * Input formats:
 *   - Binary PPM (P6), 8-bit, any size up to 64x64. Easiest to produce
 *     from any image with: `convert icon.png -resize 48x48 icon.ppm`
 *     (ImageMagick) — P6 is the default binary PPM.
 *
 * Output:
 *   - default: raw .dobicon blob (header + RGB565 pixels), suitable to
 *     drop into the program's source directory and embed via the
 *     linker (see --emit-c) or to incbin from assembly.
 *   - --emit-c <symbol>: writes a C source file declaring
 *         __attribute__((section(".dobicon"), used))
 *         const unsigned char <symbol>[] = { ... };
 *     Add that .c to the program and the section ships in the .mdl.
 *
 * Section payload (little-endian), matching load_dobicon() in the
 * Module Manager:
 *   u16 magic   = 0xD0B1
 *   u16 width
 *   u16 height
 *   u16 flags        bit0 = colour-key transparency present
 *   u16 colorkey     RGB565 value treated as transparent (if bit0)
 *   u16 pixels[w*h]  RGB565, row-major, top-down
 *
 * Transparency: pass --key RRGGBB (hex, 8-bit per channel) to mark a
 * source colour as transparent; it is converted to RGB565 and stored
 * as the colour key. Pixels matching it are skipped when blitted.
 *
 * Build:  cc -O2 -o mkicon mkicon.c
 * Usage:  mkicon [--key RRGGBB] [--emit-c SYMBOL] <in.ppm> <out>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define DOBICON_MAGIC     0xD0B1
#define DOBICON_FLAG_CKEY 0x0001

static uint16_t rgb888_to_565(int r, int g, int b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* Read a binary PPM (P6). Returns malloc'd RGB888 triplets, sets w/h. */
static unsigned char *read_ppm(const char *path, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror("open input"); return NULL; }

    char magic[3] = {0};
    if (fscanf(f, "%2s", magic) != 1 || strcmp(magic, "P6") != 0)
    {
        fprintf(stderr, "mkicon: input is not a binary PPM (P6)\n");
        fclose(f);
        return NULL;
    }

    /* Skip whitespace/comments, then read width height maxval. */
    int vals[3], vi = 0;
    while (vi < 3)
    {
        int c = fgetc(f);
        if (c == EOF) { fclose(f); return NULL; }
        if (c == '#') { while ((c = fgetc(f)) != '\n' && c != EOF); continue; }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        ungetc(c, f);
        if (fscanf(f, "%d", &vals[vi]) != 1) { fclose(f); return NULL; }
        vi++;
    }
    fgetc(f);   /* single whitespace after maxval */

    int width = vals[0], height = vals[1], maxval = vals[2];
    if (maxval != 255)
    {
        fprintf(stderr, "mkicon: only 8-bit PPM (maxval 255) supported\n");
        fclose(f);
        return NULL;
    }
    if (width < 1 || height < 1 || width > 64 || height > 64)
    {
        fprintf(stderr, "mkicon: size %dx%d out of range (1..64)\n", width, height);
        fclose(f);
        return NULL;
    }

    size_t n = (size_t)width * height * 3;
    unsigned char *px = malloc(n);
    if (!px) { fclose(f); return NULL; }
    if (fread(px, 1, n, f) != n)
    {
        fprintf(stderr, "mkicon: short read on pixel data\n");
        free(px); fclose(f); return NULL;
    }
    fclose(f);
    *w = width; *h = height;
    return px;
}

int main(int argc, char **argv)
{
    const char *key_hex = NULL;
    const char *emit_c  = NULL;
    const char *in = NULL, *out = NULL;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--key") == 0 && i + 1 < argc)
            key_hex = argv[++i];
        else if (strcmp(argv[i], "--emit-c") == 0 && i + 1 < argc)
            emit_c = argv[++i];
        else if (!in)  in = argv[i];
        else if (!out) out = argv[i];
    }

    if (!in || !out)
    {
        fprintf(stderr,
            "Usage: %s [--key RRGGBB] [--emit-c SYMBOL] <in.ppm> <out>\n", argv[0]);
        return 1;
    }

    int w, h;
    unsigned char *rgb = read_ppm(in, &w, &h);
    if (!rgb) return 1;

    uint16_t flags = 0, colorkey = 0;
    if (key_hex)
    {
        unsigned int v = (unsigned int)strtoul(key_hex, NULL, 16);
        colorkey = rgb888_to_565((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
        flags |= DOBICON_FLAG_CKEY;
    }

    /* Build the blob. */
    size_t npix = (size_t)w * h;
    size_t blob_sz = 12 + npix * 2;
    unsigned char *blob = malloc(blob_sz);
    if (!blob) { free(rgb); return 1; }

    #define PUT16(off, val) do { blob[(off)] = (val) & 0xFF; \
                                 blob[(off)+1] = ((val) >> 8) & 0xFF; } while (0)
    PUT16(0, DOBICON_MAGIC);
    PUT16(2, w);
    PUT16(4, h);
    PUT16(6, flags);
    PUT16(8, colorkey);
    /* bytes 10..11 reserved/padding to keep pixels 2-byte aligned */
    PUT16(10, 0);

    for (size_t i = 0; i < npix; i++)
    {
        int r = rgb[i*3+0], g = rgb[i*3+1], b = rgb[i*3+2];
        uint16_t c = rgb888_to_565(r, g, b);
        PUT16(12 + i*2, c);
    }
    free(rgb);

    if (emit_c)
    {
        FILE *f = fopen(out, "w");
        if (!f) { perror("open output"); free(blob); return 1; }
        fprintf(f, "/* Auto-generated by mkicon — .dobicon section for MainDOB. */\n");
        fprintf(f, "__attribute__((section(\".dobicon\"), used))\n");
        fprintf(f, "const unsigned char %s[] = {\n", emit_c);
        for (size_t i = 0; i < blob_sz; i++)
        {
            fprintf(f, "0x%02X,", blob[i]);
            if ((i % 12) == 11) fprintf(f, "\n");
        }
        fprintf(f, "\n};\n");
        fclose(f);
        fprintf(stderr, "mkicon: wrote C source %s (%dx%d, %zu bytes section)\n",
                out, w, h, blob_sz);
    }
    else
    {
        FILE *f = fopen(out, "wb");
        if (!f) { perror("open output"); free(blob); return 1; }
        fwrite(blob, 1, blob_sz, f);
        fclose(f);
        fprintf(stderr, "mkicon: wrote blob %s (%dx%d, %zu bytes)\n",
                out, w, h, blob_sz);
    }

    free(blob);
    return 0;
}
