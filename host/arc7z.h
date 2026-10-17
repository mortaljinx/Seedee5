/* Unpacks a .7z archive into a folder, streaming (so a 700 MB disc image needs only the LZMA
 * dictionary in memory, not the whole file). Uses the public-domain 7z reader from the LZMA SDK.
 * Supported: LZMA and LZMA2 (what 7-Zip uses by default), solid or not. Every file is checked
 * against its CRC. Other methods (PPMd, BZip2, filters, encryption) are refused with a message. */
#ifndef ARC7Z_H
#define ARC7Z_H

#include <stdint.h>
#include <stddef.h>

typedef struct arc7z arc7z;

/* Opens the archive and reads its file list. Returns NULL and fills `why` on failure. */
arc7z *arc7z_open(const char *path, char *why, size_t why_len);
void arc7z_close(arc7z *a);

/* Total size of the files inside (bytes) and how many there are. */
uint64_t arc7z_total(const arc7z *a);
int arc7z_files(const arc7z *a);

/* Unpacks everything into `dest_dir` (created as needed; files are written to name.part and renamed
 * when their CRC checks out). *done grows as data is written; the work stops if *cancel becomes
 * non-zero. If `current` is not NULL it receives the name of the file being unpacked.
 * Returns 0 on success, 1 if cancelled, -1 on error (`why` filled in). */
int arc7z_extract(arc7z *a, const char *dest_dir, volatile uint64_t *done, volatile int *cancel,
                  char *current, size_t current_len, char *why, size_t why_len);

#endif
