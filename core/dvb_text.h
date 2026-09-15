#ifndef DTV_DVB_TEXT_H
#define DTV_DVB_TEXT_H

#include <stddef.h>
#include <stdint.h>

/* DVB SI text into UTF-8 (ETSI EN 300 468 Annex A).
 *
 * Service names and programme titles arrive in whatever character table the
 * broadcaster picked, announced by an optional selector byte in front of the
 * string. Nothing here used to read that: the bytes were copied through and
 * everything downstream -- the screen, the database, the recording file name
 * -- reads them as UTF-8. A Turkish capital dotted I is 0xDD in ISO 8859-9
 * and is not valid UTF-8 on its own, so "KALENDER PIDE" reached the disk with
 * a replacement character where the I belonged.
 *
 * Supported: ISO 6937 (the default when no selector is sent, including its
 * combining diacritics), ISO 8859-1/-5/-7/-9/-15, UTF-8 and UTF-16BE. Any
 * other single-byte table is read as ISO 8859-1, which is wrong for some
 * accented letters but never produces invalid UTF-8.
 *
 * Control codes become spaces; the caller decides what to trim. Writes at
 * most size-1 bytes plus a terminator and returns the length written. */
size_t dtv_dvb_text_to_utf8(char *out, size_t size,
                            const uint8_t *source, size_t source_size);

#endif
