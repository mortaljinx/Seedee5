/* See arc7z.h. */
#include "arc7z.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "7z.h"
#include "7zAlloc.h"
#include "7zCrc.h"
#include "7zFile.h"
#include "Lzma2Dec.h"
#include "LzmaDec.h"

#define K_COPY 0x00
#define K_LZMA 0x030101
#define K_LZMA2 0x21
#define IN_BUF (1u << 16)
#define OUT_BUF (1u << 20)

struct arc7z {
    CFileInStream file;
    CLookToRead2 look;
    CSzArEx db;
    ISzAlloc alloc, alloc_temp;
    Byte *look_buf;
    int opened;
    uint64_t total;
    int nfiles;
};

static ISzAlloc g_alloc_sz = {SzAlloc, SzFree};

static void set_err(char *why, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_err(char *why, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, n, fmt, ap);
    va_end(ap);
}

arc7z *arc7z_open(const char *path, char *why, size_t why_len)
{
    arc7z *a = calloc(1, sizeof *a);
    if (!a) {
        set_err(why, why_len, "out of memory");
        return NULL;
    }
    a->alloc.Alloc = SzAlloc;
    a->alloc.Free = SzFree;
    a->alloc_temp.Alloc = SzAllocTemp;
    a->alloc_temp.Free = SzFreeTemp;
    CrcGenerateTable();
    FileInStream_CreateVTable(&a->file);
    a->file.wres = 0;
    LookToRead2_CreateVTable(&a->look, False);
    a->look.buf = (Byte *)malloc(IN_BUF);
    a->look_buf = a->look.buf;
    if (!a->look.buf) {
        set_err(why, why_len, "out of memory");
        free(a);
        return NULL;
    }
    a->look.bufSize = IN_BUF;
    a->look.realStream = &a->file.vt;
    LookToRead2_INIT(&a->look);
    if (InFile_Open(&a->file.file, path) != 0) {
        set_err(why, why_len, "cannot open %s", path);
        free(a->look_buf);
        free(a);
        return NULL;
    }
    SzArEx_Init(&a->db);
    SRes r = SzArEx_Open(&a->db, &a->look.vt, &a->alloc, &a->alloc_temp);
    if (r != SZ_OK) {
        set_err(why, why_len, r == SZ_ERROR_UNSUPPORTED ? "this 7z archive uses a feature that is not supported"
                              : r == SZ_ERROR_MEM ? "not enough memory to read this archive"
                              : r == SZ_ERROR_NO_ARCHIVE ? "this is not a 7z archive"
                              : "the 7z archive is damaged (error %d)", (int)r);
        File_Close(&a->file.file);
        SzArEx_Free(&a->db, &a->alloc);
        free(a->look_buf);
        free(a);
        return NULL;
    }
    a->opened = 1;
    for (UInt32 i = 0; i < a->db.NumFiles; i++)
        if (!SzArEx_IsDir(&a->db, i)) {
            a->total += SzArEx_GetFileSize(&a->db, i);
            a->nfiles++;
        }
    return a;
}

void arc7z_close(arc7z *a)
{
    if (!a)
        return;
    if (a->opened) {
        SzArEx_Free(&a->db, &a->alloc);
        File_Close(&a->file.file);
    }
    free(a->look_buf);
    free(a);
}

uint64_t arc7z_total(const arc7z *a) { return a->total; }
int arc7z_files(const arc7z *a) { return a->nfiles; }

/* UTF-16LE name of file i as a safe relative UTF-8 path; returns 0 if the name is not acceptable. */
static int file_name(const arc7z *a, UInt32 i, char *out, size_t cap)
{
    size_t len = SzArEx_GetFileNameUtf16(&a->db, i, NULL);
    if (len == 0 || len > 2000)
        return 0;
    UInt16 *w = malloc(len * sizeof *w);
    if (!w)
        return 0;
    SzArEx_GetFileNameUtf16(&a->db, i, w);
    size_t o = 0;
    for (size_t k = 0; k + 1 < len; k++) {
        uint32_t c = w[k];
        if (c >= 0xD800 && c < 0xDC00 && k + 2 < len && w[k + 1] >= 0xDC00 && w[k + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (w[k + 1] - 0xDC00);
            k++;
        }
        if (c == '\\')
            c = '/';
        if (o + 5 >= cap) {
            free(w);
            return 0;
        }
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = (char)(0xC0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { out[o++] = (char)(0xE0 | (c >> 12)); out[o++] = (char)(0x80 | ((c >> 6) & 63)); out[o++] = (char)(0x80 | (c & 63)); }
        else { out[o++] = (char)(0xF0 | (c >> 18)); out[o++] = (char)(0x80 | ((c >> 12) & 63)); out[o++] = (char)(0x80 | ((c >> 6) & 63)); out[o++] = (char)(0x80 | (c & 63)); }
    }
    free(w);
    out[o] = 0;
    if (!o || out[0] == '/' || strstr(out, "../") || strstr(out, "/..") || !strcmp(out, "..") || strchr(out, ':'))
        return 0;
    return 1;
}

static void mkdirs_path(const char *dir)
{
    char tmp[1400];
    snprintf(tmp, sizeof tmp, "%s", dir);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    mkdir(tmp, 0777);
}

typedef struct {
    FILE *f;
    char tmp[1560], final[1500];
    uint64_t left;  /* bytes still to come for the current file */
    uint32_t crc;
    int has_crc;
    uint32_t want_crc;
} writer_t;

/* Where the next bytes go: the files of one folder, in order. */
typedef struct {
    arc7z *a;
    const char *dest;
    UInt32 file;       /* index of the current file in the archive */
    UInt32 last;       /* one past the last file index of this folder */
    writer_t w;
    volatile uint64_t *done;
    char *current;
    size_t current_len;
    char *why;
    size_t why_len;
} sink_t;

static int sink_finish_file(sink_t *s)
{
    if (!s->w.f)
        return 0;
    int ok = fclose(s->w.f) == 0;
    s->w.f = NULL;
    if (!ok) {
        set_err(s->why, s->why_len, "cannot write %s (disk full?)", s->w.tmp);
        remove(s->w.tmp);
        return -1;
    }
    if (s->w.has_crc && s->w.crc != s->w.want_crc) {
        set_err(s->why, s->why_len, "the archive is damaged: bad CRC in %s", s->w.final);
        remove(s->w.tmp);
        return -1;
    }
    if (rename(s->w.tmp, s->w.final) != 0) {
        set_err(s->why, s->why_len, "cannot finish %s (errno %d)", s->w.final, errno);
        remove(s->w.tmp);
        return -1;
    }
    return 0;
}

/* Moves to the next file with data (creating empty files and skipping folders on the way). */
static int sink_next(sink_t *s)
{
    while (s->file < s->last) {
        UInt32 i = s->file++;
        char name[1100];
        if (SzArEx_IsDir(&s->a->db, i))
            continue;
        if (!file_name(s->a, i, name, sizeof name)) {
            set_err(s->why, s->why_len, "the archive holds a file name that is not allowed");
            return -1;
        }
        const uint64_t size = SzArEx_GetFileSize(&s->a->db, i);
        snprintf(s->w.final, sizeof s->w.final, "%.300s/%.1000s", s->dest, name);
        snprintf(s->w.tmp, sizeof s->w.tmp, "%s.part", s->w.final);
        char dir[1500];
        snprintf(dir, sizeof dir, "%s", s->w.final);
        char *sl = strrchr(dir, '/');
        if (sl) { *sl = 0; mkdirs_path(dir); }
        s->w.f = fopen(s->w.tmp, "wb");
        if (!s->w.f) {
            set_err(s->why, s->why_len, "cannot create %s (errno %d)", s->w.final, errno);
            return -1;
        }
        s->w.left = size;
        s->w.crc = CRC_INIT_VAL;
        s->w.has_crc = SzBitWithVals_Check(&s->a->db.CRCs, i);
        s->w.want_crc = s->w.has_crc ? s->a->db.CRCs.Vals[i] : 0;
        if (s->current)
            snprintf(s->current, s->current_len, "%s", name);
        if (size == 0) { /* an empty file has no data in the stream */
            s->w.crc = CRC_INIT_VAL;
            s->w.crc = CRC_GET_DIGEST(s->w.crc);
            if (sink_finish_file(s) != 0)
                return -1;
            continue;
        }
        return 0;
    }
    return 0;
}

/* Hands decoded bytes to the files. */
static int sink_write(sink_t *s, const Byte *p, size_t n)
{
    while (n) {
        if (!s->w.f && sink_next(s) != 0)
            return -1;
        if (!s->w.f) {
            set_err(s->why, s->why_len, "the archive holds more data than its file list says");
            return -1;
        }
        size_t take = n < s->w.left ? n : (size_t)s->w.left;
        if (fwrite(p, 1, take, s->w.f) != take) {
            set_err(s->why, s->why_len, "cannot write %s (disk full?)", s->w.tmp);
            return -1;
        }
        s->w.crc = CrcUpdate(s->w.crc, p, take);
        s->w.left -= take;
        p += take;
        n -= take;
        *s->done += take;
        if (s->w.left == 0) {
            s->w.crc = CRC_GET_DIGEST(s->w.crc);
            if (sink_finish_file(s) != 0)
                return -1;
        }
    }
    return 0;
}

int arc7z_extract(arc7z *a, const char *dest_dir, volatile uint64_t *done, volatile int *cancel,
                  char *current, size_t current_len, char *why, size_t why_len)
{
    int rc = -1;
    Byte *out = malloc(OUT_BUF);
    if (!out) {
        set_err(why, why_len, "out of memory");
        return -1;
    }
    mkdirs_path(dest_dir);

    for (UInt32 f = 0; f < a->db.db.NumFolders; f++) {
        CSzFolder folder;
        CSzData sd;
        sd.Data = a->db.db.CodersData + a->db.db.FoCodersOffsets[f];
        sd.Size = a->db.db.FoCodersOffsets[(size_t)f + 1] - a->db.db.FoCodersOffsets[f];
        if (SzGetNextFolderItem(&folder, &sd) != SZ_OK || folder.NumCoders != 1 || folder.NumPackStreams != 1) {
            set_err(why, why_len, "this 7z uses filters or several compression steps, which are not supported (use LZMA2)");
            goto done;
        }
        const CSzCoderInfo *coder = &folder.Coders[0];
        if (coder->MethodID != K_LZMA2 && coder->MethodID != K_LZMA && coder->MethodID != K_COPY) {
            set_err(why, why_len, "this 7z uses a compression method that is not supported (use LZMA2 or LZMA)");
            goto done;
        }
        const Byte *props = a->db.db.CodersData + a->db.db.FoCodersOffsets[f] + coder->PropsOffset;
        const UInt64 pack_start = a->db.dataPos + a->db.db.PackPositions[a->db.db.FoStartPackStreamIndex[f]];
        UInt64 pack_left = a->db.db.PackPositions[a->db.db.FoStartPackStreamIndex[f] + 1] -
                           a->db.db.PackPositions[a->db.db.FoStartPackStreamIndex[f]];
        UInt64 unpack_left = SzAr_GetFolderUnpackSize(&a->db.db, f);

        if (LookInStream_SeekTo(&a->look.vt, pack_start) != SZ_OK) {
            set_err(why, why_len, "cannot read the archive");
            goto done;
        }

        sink_t sink = {a, dest_dir, a->db.FolderToFile[f], 0, {0}, done, current, current_len, why, why_len};
        /* the folder's files: from the first file that lives in it to the one before the next folder's */
        {
            UInt32 first = (UInt32)-1, last = 0;
            for (UInt32 i = 0; i < a->db.NumFiles; i++)
                if (a->db.FileToFolder[i] == f) {
                    if (first == (UInt32)-1) first = i;
                    last = i + 1;
                }
            sink.file = first == (UInt32)-1 ? 0 : first;
            sink.last = last;
        }

        CLzma2Dec lzma2;
        CLzmaDec lzma;
        int kind = coder->MethodID == K_LZMA2 ? 2 : coder->MethodID == K_LZMA ? 1 : 0;
        SRes res = SZ_OK;
        if (kind == 2) {
            Lzma2Dec_CONSTRUCT(&lzma2);
            res = coder->PropsSize == 1 ? Lzma2Dec_Allocate(&lzma2, props[0], &g_alloc_sz) : SZ_ERROR_UNSUPPORTED;
            if (res == SZ_OK) Lzma2Dec_Init(&lzma2);
        } else if (kind == 1) {
            LzmaDec_CONSTRUCT(&lzma);
            res = LzmaDec_Allocate(&lzma, props, coder->PropsSize, &g_alloc_sz);
            if (res == SZ_OK) LzmaDec_Init(&lzma);
        }
        if (res != SZ_OK) {
            set_err(why, why_len, res == SZ_ERROR_MEM ? "not enough memory for the decompression dictionary of this archive"
                                                       : "unsupported compression settings");
            goto done;
        }

        while (unpack_left > 0) {
            if (cancel && *cancel) {
                rc = 1;
                if (kind == 2) Lzma2Dec_Free(&lzma2, &g_alloc_sz); else if (kind == 1) LzmaDec_Free(&lzma, &g_alloc_sz);
                if (sink.w.f) { fclose(sink.w.f); remove(sink.w.tmp); }
                goto done;
            }
#ifdef CDIHOST_TEST_DUMP
            { const char *slow = getenv("CDIHOST_TEST_SLOW_US"); if (slow) usleep((useconds_t)atoi(slow)); } /* test only: a slow disk */
#endif
            const void *inbuf = NULL;
            size_t insize = IN_BUF; /* in: how much to look at, out: how much there is */
            if (ILookInStream_Look(&a->look.vt, &inbuf, &insize) != SZ_OK) {
                set_err(why, why_len, "cannot read the archive");
                res = SZ_ERROR_READ;
            } else {
                if (insize > pack_left) insize = (size_t)pack_left;
                size_t outsize = OUT_BUF < unpack_left ? OUT_BUF : (size_t)unpack_left;
                size_t srclen = insize;
                if (kind == 0) { /* stored */
                    outsize = srclen < outsize ? srclen : outsize;
                    memcpy(out, inbuf, outsize);
                    srclen = outsize;
                } else {
                    ELzmaStatus st;
                    res = kind == 2 ? Lzma2Dec_DecodeToBuf(&lzma2, out, &outsize, (const Byte *)inbuf, &srclen, LZMA_FINISH_ANY, &st)
                                    : LzmaDec_DecodeToBuf(&lzma, out, &outsize, (const Byte *)inbuf, &srclen, LZMA_FINISH_ANY, &st);
                }
                if (res == SZ_OK) {
                    ILookInStream_Skip(&a->look.vt, srclen);
                    pack_left -= srclen;
                    if (outsize == 0 && srclen == 0) {
                        set_err(why, why_len, "the archive is damaged (decoder stalled)");
                        res = SZ_ERROR_DATA;
                    } else if (sink_write(&sink, out, outsize) != 0) {
                        res = SZ_ERROR_FAIL; /* message already set */
                    }
                    unpack_left -= outsize;
                } else if (!why[0]) {
                    set_err(why, why_len, "the archive is damaged (decoder error %d)", (int)res);
                }
            }
            if (res != SZ_OK) {
                if (kind == 2) Lzma2Dec_Free(&lzma2, &g_alloc_sz); else if (kind == 1) LzmaDec_Free(&lzma, &g_alloc_sz);
                if (sink.w.f) { fclose(sink.w.f); remove(sink.w.tmp); }
                goto done;
            }
        }
        if (kind == 2) Lzma2Dec_Free(&lzma2, &g_alloc_sz); else if (kind == 1) LzmaDec_Free(&lzma, &g_alloc_sz);
        if (sink.w.f) {
            set_err(why, why_len, "the archive ended before %s was complete", sink.w.final);
            fclose(sink.w.f);
            remove(sink.w.tmp);
            goto done;
        }
        /* empty files that come after the last data file in this folder */
        if (sink_next(&sink) != 0)
            goto done;
    }
    /* files with no data stream at all (empty files) that belong to no folder */
    for (UInt32 i = 0; i < a->db.NumFiles; i++)
        if (a->db.FileToFolder[i] == (UInt32)-1 && !SzArEx_IsDir(&a->db, i)) {
            char name[1100], p[1500], d[1500];
            if (!file_name(a, i, name, sizeof name))
                continue;
            snprintf(p, sizeof p, "%.300s/%.1000s", dest_dir, name);
            snprintf(d, sizeof d, "%s", p);
            char *sl = strrchr(d, '/');
            if (sl) { *sl = 0; mkdirs_path(d); }
            FILE *e = fopen(p, "wb");
            if (e) fclose(e);
        }
    rc = 0;
done:
    free(out);
    return rc;
}
