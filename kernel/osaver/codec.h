/*
 * osaver/codec.h - the OSaver number language (NovaOS 2.5, item 13).
 *
 * Pure C with no kernel dependencies (only memcpy/memset/strlen/strcmp), so
 * the same file is unit-tested on the host: tools/osaver_codec_test.c.
 *
 * A code looks like this (one entry per raw line, the SystemData value on a
 * single line, no trailing newline):
 *
 *     13224:2.5
 *     4351991:((5))
 *     91235422:
 *     3332198:((B...-W...-D...-F...-E...))
 *
 * KEYS are words written with the chunk table below: every chunk of a word has
 * a number (vers = 132, ion = 24, so version = 13224). The table is prefix-free
 * (no chunk number is the start of another one), so a run of digits splits
 * into chunks one way only. On the device a hidden separator (OSC_HS) sits
 * between the chunks of a key ("132<HS>24"); it is drawn as nothing, and Copy
 * removes it.
 *
 * VALUES go after the colon:
 *     version     digits '.' digits        "2.5"  ("0.0" in the placeholder)
 *     files       "((" N "))"               number of files saved, "((-0))" = nothing saved
 *     wallpaper   empty                     (pretty print fills in "Custom" or the file target)
 *     SystemData  "((" inner "))"           never run through the chunk decoder
 *
 * RAW LINES: an empty raw line is a new line in the decoded output; raw lines
 * with no empty line between them continue the same line. Because keys are
 * prefix-free and the known keys are recognised, entries may share one decoded
 * line ("13224:2.54351991:((5))..." decodes the same as the four-line form).
 * Spaces, tabs, CR and the hidden separator are ignored everywhere.
 *
 * SYSTEMDATA (inner) uses only digits, letters and '-': tokens separated by
 * '-'. A record starts with a tag letter fused to its first number field:
 *
 *     B<bootid>-<savetime>-<bootkind>-<nfiles>-<ndirs>-<databytes>   boot record (first)
 *     W0-<index>-<name>                built-in wallpaper (index, name)
 *     W1-<path>-<size>-<data>          custom wallpaper picture (size 0: saved as a file at path)
 *     D<id>-<parent>-<name>            directory = file location ID (0 = "/")
 *     F<dir>-<name>-<type>-<size>-<data>   file in location <dir>; name without the
 *                                      extension, type = extension ("" = none)
 *     E<crc>                           end: CRC-32 of every inner character before the 'E'
 *
 * Numbers are decimal. <name>/<type>/<path>/<data> are bytes in base 62
 * (0-9 A-Z a-z): every 4 bytes (big endian) become 6 digits, a tail of 1, 2
 * or 3 bytes becomes 2, 3 or 5 digits. So everything round-trips, including
 * binary files, and the inner text never contains ':' '(' or ')'.
 */
#ifndef OSAVER_CODEC_H
#define OSAVER_CODEC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define OSC_HS        '\x1f'                /* hidden chunk separator on the device */
#define OSC_MAX_CODE  (256u * 1024u * 1024u) /* codes above 256 MB are refused     */

/* ---- chunk table ------------------------------------------------------------------- */
typedef struct { const char *code; const char *text; } osc_chunk_t;

int  osc_chunk_count(void);
const osc_chunk_t *osc_chunk_get(int i);
/* Check the table: digits only, unique texts per code, prefix-free. Returns NULL if
   fine, otherwise a static description of the first problem. */
const char *osc_chunk_table_check(void);
/* Encode a word with greedy longest-chunk matching. hidden: put OSC_HS between
   chunks. Returns the length written (NUL-terminated) or -1. */
int  osc_key_encode(const char *word, char *out, int cap, bool hidden);
/* Decode a run of digits (OSC_HS and spaces are skipped) into the word.
   Returns the word length or -1 if the digits do not split into whole chunks. */
int  osc_key_decode(const char *digits, int n, char *out, int cap);
/* Decode and report the chunk numbers and texts ("132 24" / "vers ion"). */
int  osc_key_split(const char *digits, int n, char *codes, int ccap, char *texts, int tcap);

/* ---- keys ------------------------------------------------------------------------------ */
enum { OSC_K_VERSION, OSC_K_FILES, OSC_K_WALLPAPER, OSC_K_SYSDATA, OSC_K_COUNT };
const char *osc_key_name(int k);          /* "version", "files", "wallpaper", "SystemData" */
int  osc_key_lookup(const char *word);    /* OSC_K_* or -1 */

/* ---- base 62 / CRC-32 ------------------------------------------------------------------ */
uint32_t osc_b62_len(uint32_t nbytes);                       /* digits needed           */
uint32_t osc_b62_encode(const uint8_t *in, uint32_t n, char *out);   /* returns digits  */
/* Decode a whole base-62 string; returns bytes or -1 (bad digit / bad length). */
int      osc_b62_decode(const char *in, uint32_t n, uint8_t *out, uint32_t cap);
int      osc_b62_digit(int c);                              /* 0..61 or -1             */
uint32_t osc_crc32(uint32_t crc, const void *p, uint32_t n); /* start with 0            */

/* ---- writer (encoder side) ------------------------------------------------------------- */
typedef struct osc_writer {
    int  (*write)(void *ctx, const char *s, uint32_t n);    /* nonzero = abort      */
    void *ctx;
    uint32_t crc;          /* CRC of the inner text written so far */
    uint64_t total;        /* inner characters written            */
    bool first;            /* no record written yet               */
    uint8_t carry[4];      /* data field: bytes waiting for a full group */
    int  ncarry;
    int  err;
} osc_writer_t;

void osc_w_init(osc_writer_t *w, int (*write)(void *, const char *, uint32_t), void *ctx);
void osc_w_record(osc_writer_t *w, char tag, uint32_t first);   /* "-T123" ("T123" first) */
void osc_w_num(osc_writer_t *w, uint32_t v);                    /* "-123"                 */
void osc_w_bytes(osc_writer_t *w, const void *p, uint32_t n);   /* "-<base62>"            */
/* Streamed data field ("-<base62>"): begin, any number of parts of any size, end. */
void osc_w_data_begin(osc_writer_t *w);
void osc_w_data_part(osc_writer_t *w, const void *p, uint32_t n);
void osc_w_data_end(osc_writer_t *w);
void osc_w_end(osc_writer_t *w);                                /* "-E<crc>"              */

/* The text before / after the inner SystemData text. version e.g. "2.5"; files < 0
   writes the placeholder "((-0))". hidden: OSC_HS between key chunks. Returns length. */
int  osc_head(char *out, int cap, const char *version, long files, bool hidden);
#define OSC_TAIL "))"

/* The whole placeholder code ("nothing saved yet"). */
int  osc_placeholder(char *out, int cap, bool hidden);

/* ---- parser (decoder side, streaming) ---------------------------------------------- */
#define OSC_SMAX 255                      /* longest name / type / path field (bytes) */

typedef struct osc_rec {
    char     tag;                         /* 'B' 'W' 'D' 'F' 'E'                  */
    uint32_t num[6];                      /* number fields in order               */
    int      nnum;
    char     str[2][OSC_SMAX + 1];        /* string fields in order (NUL-terminated) */
    uint16_t slen[2];
    int      nstr;
    uint32_t size;                        /* data size (W1, F)                    */
} osc_rec_t;

typedef struct osc_cb {
    void *ctx;
    /* a head value (version / files / wallpaper text as written) */
    int (*entry)(void *ctx, int key, const char *value, int len);
    /* a record's fields are complete; data (if any) follows */
    int (*rec)(void *ctx, const osc_rec_t *r);
    int (*data)(void *ctx, const uint8_t *p, uint32_t n);
    /* the record (including its data) is complete */
    int (*rec_end)(void *ctx, const osc_rec_t *r);
} osc_cb_t;

typedef struct osc_parser {
    osc_cb_t cb;
    int      st;                          /* head state                           */
    int      nl;                          /* pending newlines                     */
    char     kbuf[48]; int klen;          /* key digits                           */
    char     vbuf[80]; int vlen;          /* version value (+ following key)      */
    int      sub;                         /* sub state inside (( ))               */
    char     fbuf[16]; int flen;          /* files value                          */
    bool     seen[OSC_K_COUNT];
    char     keydig[OSC_K_COUNT][48];     /* each key's digits as written         */
    char     version[40];                 /* version value                        */
    char     filesval[16];                /* files value inside (( ))             */
    long     files;                       /* files value                          */
    bool     files_neg0;                  /* "((-0))": nothing saved              */
    bool     sys_done;
    /* inner */
    int      ist;                         /* inner token state                    */
    int      fi;                          /* field index in the record            */
    osc_rec_t rec;
    uint64_t numv; int ndig;              /* number token                         */
    char     sbuf[OSC_SMAX * 3 / 2 + 8]; int sblen;   /* string token           */
    char     grp[6]; int ng;              /* data token: pending base-62 group    */
    uint8_t  obuf[4096]; uint32_t olen;   /* data token: decoded bytes to flush   */
    uint32_t dgot;                        /* data bytes decoded in this record    */
    uint32_t icrc, crc_at_e;
    uint64_t inner_len;
    uint32_t nrec, nfile, ndir;
    bool     got_b, got_e, got_w;
    uint64_t consumed;                    /* raw bytes fed                        */
    int      err;
    char     errmsg[96];
} osc_parser_t;

void osc_parser_init(osc_parser_t *p, const osc_cb_t *cb);   /* cb may be NULL */
int  osc_parser_feed(osc_parser_t *p, const char *s, uint32_t n);   /* 0 or <0 */
int  osc_parser_finish(osc_parser_t *p);                     /* 0 if a whole valid code */
/* All four keys seen and the input stopped at a clean entry boundary. */
bool osc_parser_complete(const osc_parser_t *p);
const char *osc_parser_error(const osc_parser_t *p);

/* Validate a whole code held in memory (head, structure, record fields, counts, CRC). */
bool osc_validate(const char *s, uint32_t n, char *why, int whycap);

/* Pretty print from what the parser saw (call after the head has been parsed):
       {
       "version":"2.5"
       "files":((5))
       "wallpaper":"Custom"
       SystemData:((HiddenForPrettyPrint))
       }
   The key names come from decoding the key digits with the chunk table.
   wall_text: "Custom", the system wallpaper's file target, or "" (placeholder). */
int  osc_pretty(char *out, int cap, const osc_parser_t *p, const char *wall_text);

#endif
