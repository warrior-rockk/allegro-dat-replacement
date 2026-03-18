/* src/dat_cli.c  (v3.3 - Full Compatibility) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#include "allegro_dat_structs.h"
#include "dat_loader_bmp.h"
#include "dat_loader_data.h"
#include "dat_loader_font.h"
#include "dat_loader_pal.h"
#include "dat_writer.h"
#include "midi_to_allegro.h"
#include "wav_to_allegro.h"

static char* dupstr(const char* s) {
    size_t n = strlen(s);
    char* d = (char*)malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static void set_prop(Property* p, const char type4[4], const char* value) {
    memcpy(p->magic, "prop", 4);
    memcpy(p->type, type4, 4);
    p->len_body = (u32)strlen(value);
    p->body = dupstr(value);
}

static const char* basename_portable(const char* path) {
    const char* s = strrchr(path, '/');
    const char* s2 = strrchr(path, '\\');
    if (!s || (s2 && s2 > s)) s = s2;
    return s ? s + 1 : path;
}

/* Convierte "musica.mid" en "MUSICA_MID" para que Allegro lo encuentre */
static void sanitize_allegro_name(char* dest, const char* path) {
    const char* b = basename_portable(path);
    int i = 0;
    while (b[i] && i < 31) {
        if (b[i] == '.') dest[i] = '_';
        else dest[i] = (char)toupper((unsigned char)b[i]);
        i++;
    }
    dest[i] = '\0';
}

/* Formato de fecha exacto de small.dat: "d-mm-yyyy, H:MM" */
static void now_datestr(char* buf, size_t n) {
    time_t t = time(NULL);
    struct tm* tmv = localtime(&t);
    if (!tmv) {
        if (n)
            buf[0] = '\0';
        return;
    }
    /* Spec format: "m-dd-yyyy, h:mm" - month and hour without leading zero */
    strftime(buf, n, "%m-%d-%Y, %H:%M", tmv);
    /* Remove leading zeros from month and hour as per spec ("3-03-2026, 9:26") */
    if (buf[0] == '0') memmove(buf, buf + 1, strlen(buf));
    {
        char *colon = strchr(buf, ',');
        if (colon && colon[2] == '0') memmove(colon + 2, colon + 3, strlen(colon + 3) + 1);
    }
}

static void usage(void) {
    printf("\nAllegro 4 DAT creator (ANSI C) - Full Support\n\n");
    printf("Usage:\n");
    printf("  dat create out.dat\n");
    printf("      [--bmp file.bmp]*\n");
    printf("      [--rle file.rle]* [--midi file.mid]*\n");
    printf("      [--font8-bmp f.bmp]* [--font16-bmp f.bmp]*\n");
    printf("      [--data file.bin]* [--wav file.wav]*\n");
    printf("      [--flic file.fli/flc]*\n");
    printf("      [--pal file.act]* [--pal-bmp file.bmp]*\n\n");
    printf("      [--h file.h] (creates header definition)*\n\n");
    printf("  dat list in.dat\n\n");
}

/* ------------------------------------------------------------------ */
/* dat_list: lee y muestra el contenido de un fichero .dat            */
/* ------------------------------------------------------------------ */

/* Lee un u32 big-endian del buffer en la posicion *pos y avanza */
static u32 read_be32_buf(const u8 *buf, u32 bufsz, u32 *pos) {
    u32 v;
    if (*pos + 4 > bufsz) return 0;
    v = ((u32)buf[*pos] << 24) | ((u32)buf[*pos+1] << 16)
      | ((u32)buf[*pos+2] <<  8) |  (u32)buf[*pos+3];
    *pos += 4;
    return v;
}

/* Devuelve el valor de una propiedad NAME de un objeto (NULL si no existe).
   El puntero devuelto apunta dentro de 'buf', valido mientras buf este vivo. */
static const char *find_prop(const u8 *buf, u32 obj_start, u32 bufsz,
                              const char want[4]) {
    u32 pos = obj_start;
    while (pos + 12 <= bufsz && memcmp(buf + pos, "prop", 4) == 0) {
        u32  plen;
        pos += 4; /* skip "prop" */
        if (memcmp(buf + pos, want, 4) == 0) {
            pos += 4;
            plen = read_be32_buf(buf, bufsz, &pos);
            /* return pointer to property body (NOT null-terminated in file,
               but we know its length) */
            return (plen > 0 && pos + plen <= bufsz) ? (const char*)(buf + pos) : NULL;
        }
        pos += 4; /* skip type */
        plen = read_be32_buf(buf, bufsz, &pos);
        pos += plen; /* skip body */
    }
    return NULL;
}

/* Retorna la longitud de la prop NAME, o 0 */
static u32 find_prop_len(const u8 *buf, u32 obj_start, u32 bufsz,
                         const char want[4]) {
    u32 pos = obj_start;
    while (pos + 12 <= bufsz && memcmp(buf + pos, "prop", 4) == 0) {
        u32 plen;
        pos += 4;
        if (memcmp(buf + pos, want, 4) == 0) {
            pos += 4;
            plen = read_be32_buf(buf, bufsz, &pos);
            return plen;
        }
        pos += 4;
        plen = read_be32_buf(buf, bufsz, &pos);
        pos += plen;
    }
    return 0;
}

/* Descripcion legible del tipo de objeto */
static const char *type_description(const char tag[4]) {
    if (memcmp(tag, "BMP ", 4) == 0) return "Bitmap";
    if (memcmp(tag, "PAL ", 4) == 0) return "Palette";
    if (memcmp(tag, "RLE ", 4) == 0) return "RLE Sprite";
    if (memcmp(tag, "CMP ", 4) == 0) return "Compiled Sprite";
    if (memcmp(tag, "XCMP", 4) == 0) return "Mode-X Sprite";
    if (memcmp(tag, "FONT", 4) == 0) return "Font";
    if (memcmp(tag, "SAMP", 4) == 0) return "Sample";
    if (memcmp(tag, "MIDI", 4) == 0) return "MIDI";
    if (memcmp(tag, "FLIC", 4) == 0) return "FLI/FLC Anim";
    if (memcmp(tag, "DATA", 4) == 0) return "Data";
    if (memcmp(tag, "FILE", 4) == 0) return "Sub-datafile";
    if (memcmp(tag, "PAT ", 4) == 0) return "Gravis Patch";
    if (memcmp(tag, "info", 4) == 0) return "(grabber info)";
    return "Unknown";
}

/* Detalles extra segun tipo (dimensiones, freq, etc.) */
static void print_type_detail(const char tag[4], const u8 *body, u32 body_sz) {
    if (body_sz == 0) return;

    if ((memcmp(tag, "BMP ", 4) == 0 ||
         memcmp(tag, "CMP ", 4) == 0 ||
         memcmp(tag, "XCMP", 4) == 0) && body_sz >= 6) {
        s16 bits = (s16)(((u16)body[0] << 8) | body[1]);
        u16 w    = (u16)(((u16)body[2] << 8) | body[3]);
        u16 h    = (u16)(((u16)body[4] << 8) | body[5]);
        printf("  %dx%d, %d bpp", w, h, (int)bits);
        return;
    }
    if (memcmp(tag, "RLE ", 4) == 0 && body_sz >= 8) {
        s16 bits = (s16)(((u16)body[0] << 8) | body[1]);
        u16 w    = (u16)(((u16)body[2] << 8) | body[3]);
        u16 h    = (u16)(((u16)body[4] << 8) | body[5]);
        printf("  %dx%d, %d bpp", w, h, (int)bits);
        return;
    }
    if (memcmp(tag, "SAMP", 4) == 0 && body_sz >= 8) {
        s16 bits = (s16)(((u16)body[0] << 8) | body[1]);
        u16 freq = (u16)(((u16)body[2] << 8) | body[3]);
        s32 len  = (s32)(((u32)body[4] << 24) | ((u32)body[5] << 16)
                       | ((u32)body[6] <<  8) |  (u32)body[7]);
        printf("  %d Hz, %d-bit, %s, %d frames",
               (int)freq, (int)(bits < 0 ? -bits : bits),
               bits < 0 ? "stereo" : "mono", (int)len);
        return;
    }
    if (memcmp(tag, "MIDI", 4) == 0 && body_sz >= 2) {
        s16 div = (s16)(((u16)body[0] << 8) | body[1]);
        /* Walk sequentially: format is s16 div + 32x(s32 len + data[len]) */
        int tracks = 0, t;
        u32 mpos = 2;
        for (t = 0; t < 32 && mpos + 4 <= body_sz; t++) {
            s32 tlen = (s32)(((u32)body[mpos] << 24) | ((u32)body[mpos+1] << 16)
                           | ((u32)body[mpos+2] <<  8) |  (u32)body[mpos+3]);
            mpos += 4;
            if (tlen > 0) {
                tracks++;
                mpos += (u32)tlen;
            }
        }
        printf("  %d tracks, %d ticks/beat", tracks, (int)div);
        return;
    }
    if (memcmp(tag, "FONT", 4) == 0 && body_sz >= 2) {
        s16 sz = (s16)(((u16)body[0] << 8) | body[1]);
        if (sz == 8)       printf("  8x8 bitmap");
        else if (sz == 16) printf("  8x16 bitmap");
        else if (sz == 0)  printf("  proportional (3.9+ format)");
        else if (sz == -1) printf("  proportional (legacy)");
        else               printf("  size=%d", (int)sz);
        return;
    }
    if (memcmp(tag, "FLIC", 4) == 0 && body_sz >= 6) {
        /* FLI/FLC header: u32 size, u16 magic, u16 frames, u16 w, u16 h ... */
        u16 magic  = (u16)(body[4] | ((u16)body[5] << 8)); /* little-endian */
        u16 frames = (u16)(body[6] | ((u16)body[7] << 8));
        u16 w      = (u16)(body[8] | ((u16)body[9] << 8));
        u16 h      = (u16)(body[10]| ((u16)body[11]<< 8));
        printf("  %dx%d, %d frames (%s)",
               (int)w, (int)h, (int)frames,
               magic == 0xAF12 ? "FLC" : "FLI");
        return;
    }
}

static int dat_list(const char *filename) {
    u8   *buf;
    u32   bufsz;
    u32   pos;
    u32   pack_magic, dat_magic, num_objects;
    u32   idx;
    int   name_w = 32; /* column width for name */

    /* Load whole file */
    if (!load_file_bytes(filename, &buf, &bufsz)) {
        fprintf(stderr, "Error: cannot open '%s'\n", filename);
        return 1;
    }

    if (bufsz < 12) { free(buf); fprintf(stderr, "Error: file too small\n"); return 1; }

    pos = 0;
    pack_magic  = read_be32_buf(buf, bufsz, &pos);
    dat_magic   = read_be32_buf(buf, bufsz, &pos);
    num_objects = read_be32_buf(buf, bufsz, &pos);

    /* Validate magic numbers */
    if (pack_magic == 0x736C682Eu && dat_magic == 0x414C4C2Eu) {
        /* F_NOPACK_MAGIC + DAT_MAGIC - uncompressed, OK */
    } else if (pack_magic == 0x736C6821u) {
        /* F_PACK_MAGIC - LZSS compressed, not supported */
        fprintf(stderr, "Error: '%s' is compressed with LZSS (not supported)\n", filename);
        free(buf); return 1;
    } else {
        fprintf(stderr, "Error: '%s' is not a valid Allegro DAT file\n", filename);
        free(buf); return 1;
    }

    printf("File: %s\n", filename);
    printf("Objects: %u\n\n", num_objects);
    printf("%-4s  %-*s  %-14s  %10s  %s\n",
           "#", name_w, "Name", "Type", "Size", "Details");
    printf("%-4s  %-*s  %-14s  %10s  %s\n",
           "----", name_w, "--------------------------------",
           "--------------", "----------", "-------");

    for (idx = 0; idx < num_objects; idx++) {
        u32         obj_start = pos;
        const char *name_ptr;
        u32         name_len;
        const char *date_ptr;
        u32         date_len;
        char        name_buf[64];
        char        date_buf[32];
        char        type_tag[5];
        u32         len_compressed;
        u32         len_uncompressed;
        const u8   *body;

        /* Skip properties to find type tag, but remember obj_start for prop lookup */
        {
            u32 scan = pos;
            while (scan + 12 <= bufsz && memcmp(buf + scan, "prop", 4) == 0) {
                u32 plen;
                scan += 8;
                plen = read_be32_buf(buf, bufsz, &scan);
                scan += plen;
            }
            pos = scan;
        }

        if (pos + 12 > bufsz) break;

        /* Read type tag */
        memcpy(type_tag, buf + pos, 4);
        type_tag[4] = '\0';
        pos += 4;

        len_compressed   = read_be32_buf(buf, bufsz, &pos);
        len_uncompressed = read_be32_buf(buf, bufsz, &pos);

        body = buf + pos;

        /* Resolve NAME property */
        name_ptr = find_prop(buf, obj_start, bufsz, "NAME");
        name_len = find_prop_len(buf, obj_start, bufsz, "NAME");
        if (name_ptr && name_len > 0) {
            u32 copy = name_len < 63 ? name_len : 63;
            memcpy(name_buf, name_ptr, copy);
            name_buf[copy] = '\0';
        } else {
            strcpy(name_buf, "(sin nombre)");
        }

        /* Resolve DATE property */
        date_ptr = find_prop(buf, obj_start, bufsz, "DATE");
        date_len = find_prop_len(buf, obj_start, bufsz, "DATE");
        if (date_ptr && date_len > 0) {
            u32 copy = date_len < 31 ? date_len : 31;
            memcpy(date_buf, date_ptr, copy);
            date_buf[copy] = '\0';
        } else {
            date_buf[0] = '\0';
        }

        /* Print row */
        printf("%-4u  %-*s  %-14s  %10u",
               idx + 1, name_w, name_buf,
               type_description(type_tag),
               len_uncompressed);

        /* Extra detail from body */
        {
            u32 safe_body_sz = len_uncompressed;
            if (pos + safe_body_sz > bufsz)
                safe_body_sz = bufsz - pos;
            print_type_detail(type_tag, body, safe_body_sz);
        }

        if (date_buf[0]) printf("  [%s]", date_buf);
        printf("\n");

        /* Advance past body (use compressed size for on-disk advance) */
        pos += len_compressed;
        (void)len_uncompressed;
    }

    printf("\n");
    free(buf);
    return 0;
}

int main(int argc, char** argv) {
    int i;
    const char* out;
    char datebuf[64];
    AllegroDat* dat;
    DatObject* objs;

    /* dispatch commands */
    if (argc >= 3 && strcmp(argv[1], "list") == 0) {
        return dat_list(argv[2]);
    }
    if (argc < 3 || strcmp(argv[1], "create") != 0) { usage(); return 1; }
    out = argv[2];
    now_datestr(datebuf, sizeof(datebuf));

    dat = (AllegroDat*)calloc(1, sizeof(AllegroDat));
    dat->pack_magic = 0x736C682Eu; /* 'slh.' */
    dat->dat_magic = 0x414C4C2Eu;  /* 'ALL.' */
    objs = (DatObject*)calloc(1024, sizeof(DatObject));
    dat->objects = objs;

    for (i = 3; i < argc; i++) {
        char clean_name[64];
        
        /* BMP */
        if (strcmp(argv[i], "--bmp") == 0 && i + 1 < argc)
        {
            //iterate until next argument isn't another flag or all files processed
            while ((i + 1) < argc && argv[i + 1][0] != '-') { 
                DatBitmap* bmp = NULL;
                if (load_bmp_to_dat_bitmap(argv[i+1], &bmp)) {
                    DatObject* o = &objs[dat->num_objects++];
                    memcpy(o->type, "BMP ", 4); o->body.bmp = bmp;
                    o->len_uncompressed = o->len_compressed = (s32)(2+2+2 + (bmp->width * bmp->height * ((u32)bmp->bits_per_pixel / 8u)));
                    o->num_properties = 3; o->properties = (Property*)calloc(3, sizeof(Property));
                    set_prop(&o->properties[0], "DATE", datebuf);
                    sanitize_allegro_name(clean_name, basename_portable(argv[i+1]));
                    set_prop(&o->properties[1], "NAME", clean_name);
                    set_prop(&o->properties[2], "ORIG", argv[i+1]);
                }
                                
                // if there's more than one file, next but only increments if isn't last file
                if ((i + 2) < argc && argv[i+2][0] != '-') 
                    i++;
                else 
                    break;                
            }
            continue;
        }

        /* PAL */
        if (strcmp(argv[i], "--pal") == 0 && i + 1 < argc) {
            //iterate until next argument isn't another flag or all files processed
            while ((i + 1) < argc && argv[i + 1][0] != '-') { 
                u8* pal = NULL;
                if (load_act_to_pal63(argv[i+1], &pal)) {
                    DatObject* o = &objs[dat->num_objects++];
                    memcpy(o->type, "PAL ", 4); o->body.pal = pal;
                    o->len_uncompressed = o->len_compressed = 256 * 4; /* Spec: 256 x {R,G,B,pad} */
                    o->num_properties = 3; o->properties = (Property*)calloc(3, sizeof(Property));
                    set_prop(&o->properties[0], "DATE", datebuf);
                    sanitize_allegro_name(clean_name, basename_portable(argv[i+1]));
                    set_prop(&o->properties[1], "NAME", clean_name);
                    set_prop(&o->properties[2], "ORIG", argv[i+1]);
                }      
                // if there's more than one file, next but only increments if isn't last file
                if (i + 2 < argc && argv[i+2][0] != '-') 
                    i++;
                else 
                    break;                
            }
            continue;
        }

        /* PAL desde BMP indexado (1/4/8 bpp) */
        if (strcmp(argv[i], "--pal-bmp") == 0 && i + 1 < argc) {
            //iterate until next argument isn't another flag or all files processed
            while ((i + 1) < argc && argv[i + 1][0] != '-') { 
                u8* pal = NULL;
                if (load_bmp_to_pal63(argv[i+1], &pal)) {
                    DatObject* o = &objs[dat->num_objects++];
                    memcpy(o->type, "PAL ", 4); o->body.pal = pal;
                    o->len_uncompressed = o->len_compressed = 256 * 4;
                    o->num_properties = 3; o->properties = (Property*)calloc(3, sizeof(Property));
                    set_prop(&o->properties[0], "DATE", datebuf);
                    sanitize_allegro_name(clean_name, basename_portable(argv[i+1]));
                    set_prop(&o->properties[1], "NAME", clean_name);
                    set_prop(&o->properties[2], "ORIG", argv[i+1]);
                }
                // if there's more than one file, next but only increments if isn't last file
                if (i + 2 < argc && argv[i+2][0] != '-') 
                    i++;
                else 
                    break;                
            }
            continue;
        }

        /* RLE */
        if (strcmp(argv[i], "--rle") == 0 && i + 1 < argc) {
            //iterate until next argument isn't another flag or all files processed
            while ((i + 1) < argc && argv[i + 1][0] != '-') { 
                u8* buf; u32 sz;
                if (load_file_bytes(argv[i+1], &buf, &sz)) {
                    DatRleSprite* r = (DatRleSprite*)calloc(1, sizeof(DatRleSprite));
                    r->bits_per_pixel = 8; r->len_image = sz; r->image = buf;
                    DatObject* o = &objs[dat->num_objects++];
                    memcpy(o->type, "RLE ", 4); o->body.rle = r;
                    o->len_uncompressed = o->len_compressed = (s32)(2+2+2+4) + (s32)sz;
                    o->num_properties = 3; o->properties = (Property*)calloc(3, sizeof(Property));
                    set_prop(&o->properties[0], "DATE", datebuf);
                    sanitize_allegro_name(clean_name, basename_portable(argv[i+1]));
                    set_prop(&o->properties[1], "NAME", clean_name);
                    set_prop(&o->properties[2], "ORIG", argv[i+1]);
                }
                // if there's more than one file, next but only increments if isn't last file
                if (i + 2 < argc && argv[i+2][0] != '-') 
                    i++;
                else 
                    break;                
            }
            continue;
        }

        /* FONT 8x8 y 8x16 */
        if ((strcmp(argv[i], "--font8-bmp") == 0 || strcmp(argv[i], "--font16-bmp") == 0) && i + 1 < argc) {
            //iterate until next argument isn't another flag or all files processed
            while ((i + 1) < argc && argv[i + 1][0] != '-') { 
                DatFont* font = NULL;
                int is16 = (argv[i][6] == '1');
                int ok = is16 ? build_font16_from_bmp(argv[i+1], 128, &font) : build_font8_from_bmp(argv[i+1], 128, &font);
                if (ok) {
                    DatObject* o = &objs[dat->num_objects++];
                    memcpy(o->type, "FONT", 4); o->body.font = font;
                    o->len_uncompressed = o->len_compressed = (2 + 95 * (is16 ? 16 : 8));
                    o->num_properties = 3; o->properties = (Property*)calloc(3, sizeof(Property));
                    set_prop(&o->properties[0], "DATE", datebuf);
                    sanitize_allegro_name(clean_name, basename_portable(argv[i+1]));
                    set_prop(&o->properties[1], "NAME", clean_name);
                    set_prop(&o->properties[2], "ORIG", argv[i+1]);
                }
                // if there's more than one file, next but only increments if isn't last file
                if (i + 2 < argc && argv[i+2][0] != '-') 
                    i++;
                else 
                    break;                
            }
            continue;
        }

        /* MIDI: convierte SMF (.mid) al formato interno de Allegro 4 */
        if (strcmp(argv[i], "--midi") == 0 && i + 1 < argc)
        {
            //iterate until next argument isn't another flag or all files processed
            while ((i + 1) < argc && argv[i + 1][0] != '-') { 
                u8* raw; u32 raw_sz;
                if (load_file_bytes(argv[i+1], &raw, &raw_sz)) {
                    u8* alg_buf = NULL;
                    unsigned int alg_sz = 0;
                    if (mid_to_allegro_dat(raw, raw_sz, &alg_buf, &alg_sz)) {
                        sanitize_allegro_name(clean_name, basename_portable(argv[i+1]));
                        DatObject* o = &objs[dat->num_objects++];
                        memcpy(o->type, "MIDI", 4);
                        o->body.any = alg_buf;
                        o->len_uncompressed = o->len_compressed = (s32)alg_sz;
                        o->num_properties = 3;
                        o->properties = (Property*)calloc(3, sizeof(Property));
                        set_prop(&o->properties[0], "DATE", datebuf);
                        set_prop(&o->properties[1], "NAME", clean_name);
                        set_prop(&o->properties[2], "ORIG", argv[i+1]);
                    } else {
                        fprintf(stderr, "Error: no se pudo convertir '%s' a formato MIDI de Allegro\n", argv[i+1]);
                    }
                    free(raw);
                }          
                // if there's more than one file, next but only increments if isn't last file
                if (i + 2 < argc && argv[i+2][0] != '-') 
                    i++;
                else 
                    break;                
            }
            continue;
        }

        /* WAV: convierte RIFF/PCM al formato interno SAMP de Allegro 4 */
        if (strcmp(argv[i], "--wav") == 0 && i + 1 < argc)
        {
            //iterate until next argument isn't another flag or all files processed
            while ((i + 1) < argc && argv[i + 1][0] != '-') { 
                u8* raw; u32 raw_sz;
                if (load_file_bytes(argv[i+1], &raw, &raw_sz)) {
                    u8* alg_buf = NULL;
                    unsigned int alg_sz = 0;
                    if (wav_to_allegro_samp(raw, raw_sz, &alg_buf, &alg_sz)) {
                        DatObject* o = &objs[dat->num_objects++];
                        memcpy(o->type, "SAMP", 4);
                        o->body.any = alg_buf;
                        o->len_uncompressed = o->len_compressed = (s32)alg_sz;
                        o->num_properties = 3;
                        o->properties = (Property*)calloc(3, sizeof(Property));
                        sanitize_allegro_name(clean_name, basename_portable(argv[i+1]));
                        set_prop(&o->properties[0], "DATE", datebuf);
                        set_prop(&o->properties[1], "NAME", clean_name);
                        set_prop(&o->properties[2], "ORIG", argv[i+1]);
                    } else {
                        fprintf(stderr, "Error: could not convert '%s' to Allegro SAMP format\n", argv[i+1]);
                    }
                    free(raw);
                }          
                // if there's more than one file, next but only increments if isn't last file
                if (i + 2 < argc && argv[i+2][0] != '-') 
                    i++;
                else 
                    break;                
            }
            continue;
}

        /* FLIC: animacion FLI/FLC, almacenada verbatim (spec: "standard format") */
        if (strcmp(argv[i], "--flic") == 0 && i + 1 < argc) {
            //iterate until next argument isn't another flag or all files processed
            while ((i + 1) < argc && argv[i + 1][0] != '-') { 
                u8* buf; u32 sz;
                if (load_file_bytes(argv[i+1], &buf, &sz)) {
                    /* Validar magic FLI (0xAF11) o FLC (0xAF12) en offset 4, little-endian */
                    if (sz >= 6 && ((buf[4] == 0x11 && buf[5] == 0xAF) ||
                                    (buf[4] == 0x12 && buf[5] == 0xAF))) {
                        DatObject* o = &objs[dat->num_objects++];
                        memcpy(o->type, "FLIC", 4); o->body.any = buf;
                        o->len_uncompressed = o->len_compressed = (s32)sz;
                        o->num_properties = 3; o->properties = (Property*)calloc(3, sizeof(Property));
                        sanitize_allegro_name(clean_name, basename_portable(argv[i+1]));
                        set_prop(&o->properties[0], "DATE", datebuf);
                        set_prop(&o->properties[1], "NAME", clean_name);
                        set_prop(&o->properties[2], "ORIG", argv[i+1]);
                    } else {
                        fprintf(stderr, "Error: '%s' is not a valid FLI/FLC file\n", argv[i+1]);
                        free(buf);
                    }
                }
                // if there's more than one file, next but only increments if isn't last file
                if (i + 2 < argc && argv[i+2][0] != '-') 
                    i++;
                else 
                    break;                
            }
            continue;
        }

        /* DATA: blob generico */
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            //iterate until next argument isn't another flag or all files processed
            while ((i + 1) < argc && argv[i + 1][0] != '-') { 
                u8* buf; u32 sz;
                if (load_file_bytes(argv[i+1], &buf, &sz)) {
                    DatObject* o = &objs[dat->num_objects++];
                    memcpy(o->type, "DATA", 4); o->body.any = buf;
                    o->len_uncompressed = o->len_compressed = (s32)sz;
                    o->num_properties = 3; o->properties = (Property*)calloc(3, sizeof(Property));
                    sanitize_allegro_name(clean_name, basename_portable(argv[i+1]));
                    set_prop(&o->properties[0], "DATE", datebuf);
                    set_prop(&o->properties[1], "NAME", clean_name);
                    set_prop(&o->properties[2], "ORIG", argv[i+1]);
                }       
                // if there's more than one file, next but only increments if isn't last file
                if (i + 2 < argc && argv[i+2][0] != '-') 
                    i++;
                else 
                    break;                
            }
            continue;   
        }

        /* create header */
        if (strcmp(argv[i], "--h") == 0 && i + 1 < argc) {            
            FILE *header;            
            header = fopen(argv[i+1], "w");            
            if (!header) {
                printf("Error: No se pudo abrir o crear el archivo .h.\n");                
            }
            else
            {
                for (int j = 0; j < dat->num_objects; j++)
                {
                    fprintf(header, "#define %-30s\t%-4d\t//%.4s\n", dat->objects[j].properties[1].body, j, dat->objects[j].type);
                }                
                fclose(header);   
            }
            i++; continue;
        }
    }

    /* Objeto final GrabberInfo */
    {
        DatObject* o = &objs[dat->num_objects++];
        memcpy(o->type, "info", 4);
        o->body.any = dupstr("For internal use by the grabber");
        o->len_uncompressed = o->len_compressed = (s32)strlen((char*)o->body.any);
        o->num_properties = 1; o->properties = (Property*)calloc(1, sizeof(Property));
        set_prop(&o->properties[0], "NAME", "GrabberInfo");
    }

    if (dat_write(out, dat)) printf("DAT created: %s (%u objects)\n", out, dat->num_objects);
    free_allegro_dat(dat);
    return 0;
}
