#include "dvb_text.h"

/* --- character tables ---------------------------------------------------
 *
 * Only the upper half is ever tabulated: every table below agrees with
 * ASCII from 0x20 to 0x7E, and the C0/C1 control ranges are turned into
 * spaces before a table is consulted. */

#define HIGH_FIRST 0xA0u

/* ISO 8859-1 is the identity, so a table for it would be 96 entries of
 * "itself"; the tables that follow are the ones that differ. 0 means the
 * position is unassigned and the byte is dropped. */

/* ISO 8859-9 (Latin-5). Latin-1 with six Turkish letters in place of the
 * Icelandic ones -- the whole reason this file exists. */
static const struct { uint8_t byte; unsigned code; } g_latin5[] = {
    { 0xD0u, 0x011Eu },   /* G with breve      */
    { 0xDDu, 0x0130u },   /* I with dot above  */
    { 0xDEu, 0x015Eu },   /* S with cedilla    */
    { 0xF0u, 0x011Fu },
    { 0xFDu, 0x0131u },   /* dotless i         */
    { 0xFEu, 0x015Fu }
};

/* ISO 8859-15 (Latin-9): Latin-1 with the euro and a handful of letters. */
static const struct { uint8_t byte; unsigned code; } g_latin9[] = {
    { 0xA4u, 0x20ACu }, { 0xA6u, 0x0160u }, { 0xA8u, 0x0161u },
    { 0xB4u, 0x017Du }, { 0xB8u, 0x017Eu }, { 0xBCu, 0x0152u },
    { 0xBDu, 0x0153u }, { 0xBEu, 0x0178u }
};

/* ISO 8859-7 (Greek). Regular from 0xB6 on, with punctuation below it. */
static const unsigned g_greek_low[] = {
    /* A0 */ 0x00A0u, 0x2018u, 0x2019u, 0x00A3u, 0x20ACu, 0x20AFu, 0x00A6u,
    /* A7 */ 0x00A7u, 0x00A8u, 0x00A9u, 0x037Au, 0x00ABu, 0x00ACu, 0x00ADu,
    /* AE */ 0u,      0x2015u,
    /* B0 */ 0x00B0u, 0x00B1u, 0x00B2u, 0x00B3u, 0x0384u, 0x0385u
};

/* ISO 6937, the table a broadcaster gets when it sends no selector at all.
 * 0xC1 to 0xCF are not letters but marks that apply to the character that
 * follows them; they are handled apart from this table. */
static const unsigned g_iso6937_a0[] = {
    /* A0 */ 0x00A0u, 0x00A1u, 0x00A2u, 0x00A3u, 0x20ACu, 0x00A5u, 0u,
    /* A7 */ 0x00A7u, 0x00A4u, 0x2018u, 0x201Cu, 0x00ABu, 0x2190u, 0x2191u,
    /* AE */ 0x2192u, 0x2193u,
    /* B0 */ 0x00B0u, 0x00B1u, 0x00B2u, 0x00B3u, 0x00D7u, 0x00B5u, 0x00B6u,
    /* B7 */ 0x00B7u, 0x00F7u, 0x2019u, 0x201Du, 0x00BBu, 0x00BCu, 0x00BDu,
    /* BE */ 0x00BEu, 0x00BFu
};

static const unsigned g_iso6937_d0[] = {
    /* D0 */ 0x2015u, 0x00B9u, 0x00AEu, 0x00A9u, 0x2122u, 0x266Au, 0x00ACu,
    /* D7 */ 0x00A6u, 0u,      0u,      0u,      0u,      0x215Bu, 0x215Cu,
    /* DE */ 0x215Du, 0x215Eu,
    /* E0 */ 0x2126u, 0x00C6u, 0x0110u, 0x00AAu, 0x0126u, 0u,      0x0132u,
    /* E7 */ 0x013Fu, 0x0141u, 0x00D8u, 0x0152u, 0x00BAu, 0x00DEu, 0x0166u,
    /* EE */ 0x014Au, 0x0149u,
    /* F0 */ 0x0138u, 0x00E6u, 0x0111u, 0x00F0u, 0x0127u, 0x0131u, 0x0133u,
    /* F7 */ 0x0140u, 0x0142u, 0x00F8u, 0x0153u, 0x00DFu, 0x00FEu, 0x0167u,
    /* FE */ 0x014Bu, 0x00ADu
};

/* The combining marks of ISO 6937, in table order from 0xC1. A mark with no
 * meaning is 0 and is skipped along with whatever follows it. */
static const unsigned g_iso6937_marks[] = {
    /* C1 */ 0x0300u,  /* grave        */
    /* C2 */ 0x0301u,  /* acute        */
    /* C3 */ 0x0302u,  /* circumflex   */
    /* C4 */ 0x0303u,  /* tilde        */
    /* C5 */ 0x0304u,  /* macron       */
    /* C6 */ 0x0306u,  /* breve        */
    /* C7 */ 0x0307u,  /* dot above    */
    /* C8 */ 0x0308u,  /* diaeresis    */
    /* C9 */ 0u,
    /* CA */ 0x030Au,  /* ring above   */
    /* CB */ 0x0327u,  /* cedilla      */
    /* CC */ 0u,
    /* CD */ 0x030Bu,  /* double acute */
    /* CE */ 0x0328u,  /* ogonek       */
    /* CF */ 0x030Cu   /* caron        */
};

/* Mark plus letter to the single character that means the same thing.
 * Emitting the two separately is valid Unicode but renders unevenly and
 * makes an ugly file name, so the combinations that actually turn up in
 * this footprint are composed here. Anything not listed falls back to the
 * two-character form. */
static const struct { unsigned mark; char base; unsigned code; }
g_iso6937_compose[] = {
    { 0x0300u, 'A', 0x00C0u }, { 0x0300u, 'E', 0x00C8u },
    { 0x0300u, 'I', 0x00CCu }, { 0x0300u, 'O', 0x00D2u },
    { 0x0300u, 'U', 0x00D9u }, { 0x0300u, 'a', 0x00E0u },
    { 0x0300u, 'e', 0x00E8u }, { 0x0300u, 'i', 0x00ECu },
    { 0x0300u, 'o', 0x00F2u }, { 0x0300u, 'u', 0x00F9u },

    { 0x0301u, 'A', 0x00C1u }, { 0x0301u, 'C', 0x0106u },
    { 0x0301u, 'E', 0x00C9u }, { 0x0301u, 'I', 0x00CDu },
    { 0x0301u, 'N', 0x0143u }, { 0x0301u, 'O', 0x00D3u },
    { 0x0301u, 'S', 0x015Au }, { 0x0301u, 'U', 0x00DAu },
    { 0x0301u, 'Y', 0x00DDu }, { 0x0301u, 'Z', 0x0179u },
    { 0x0301u, 'a', 0x00E1u }, { 0x0301u, 'c', 0x0107u },
    { 0x0301u, 'e', 0x00E9u }, { 0x0301u, 'i', 0x00EDu },
    { 0x0301u, 'n', 0x0144u }, { 0x0301u, 'o', 0x00F3u },
    { 0x0301u, 's', 0x015Bu }, { 0x0301u, 'u', 0x00FAu },
    { 0x0301u, 'y', 0x00FDu }, { 0x0301u, 'z', 0x017Au },

    { 0x0302u, 'A', 0x00C2u }, { 0x0302u, 'E', 0x00CAu },
    { 0x0302u, 'I', 0x00CEu }, { 0x0302u, 'O', 0x00D4u },
    { 0x0302u, 'U', 0x00DBu }, { 0x0302u, 'a', 0x00E2u },
    { 0x0302u, 'e', 0x00EAu }, { 0x0302u, 'i', 0x00EEu },
    { 0x0302u, 'o', 0x00F4u }, { 0x0302u, 'u', 0x00FBu },

    { 0x0303u, 'A', 0x00C3u }, { 0x0303u, 'N', 0x00D1u },
    { 0x0303u, 'O', 0x00D5u }, { 0x0303u, 'a', 0x00E3u },
    { 0x0303u, 'n', 0x00F1u }, { 0x0303u, 'o', 0x00F5u },

    { 0x0306u, 'A', 0x0102u }, { 0x0306u, 'G', 0x011Eu },
    { 0x0306u, 'a', 0x0103u }, { 0x0306u, 'g', 0x011Fu },

    { 0x0307u, 'I', 0x0130u }, { 0x0307u, 'Z', 0x017Bu },
    { 0x0307u, 'z', 0x017Cu },

    { 0x0308u, 'A', 0x00C4u }, { 0x0308u, 'E', 0x00CBu },
    { 0x0308u, 'I', 0x00CFu }, { 0x0308u, 'O', 0x00D6u },
    { 0x0308u, 'U', 0x00DCu }, { 0x0308u, 'Y', 0x0178u },
    { 0x0308u, 'a', 0x00E4u }, { 0x0308u, 'e', 0x00EBu },
    { 0x0308u, 'i', 0x00EFu }, { 0x0308u, 'o', 0x00F6u },
    { 0x0308u, 'u', 0x00FCu }, { 0x0308u, 'y', 0x00FFu },

    { 0x030Au, 'A', 0x00C5u }, { 0x030Au, 'U', 0x016Eu },
    { 0x030Au, 'a', 0x00E5u }, { 0x030Au, 'u', 0x016Fu },

    { 0x030Bu, 'O', 0x0150u }, { 0x030Bu, 'U', 0x0170u },
    { 0x030Bu, 'o', 0x0151u }, { 0x030Bu, 'u', 0x0171u },

    { 0x030Cu, 'C', 0x010Cu }, { 0x030Cu, 'D', 0x010Eu },
    { 0x030Cu, 'E', 0x011Au }, { 0x030Cu, 'N', 0x0147u },
    { 0x030Cu, 'R', 0x0158u }, { 0x030Cu, 'S', 0x0160u },
    { 0x030Cu, 'T', 0x0164u }, { 0x030Cu, 'Z', 0x017Du },
    { 0x030Cu, 'c', 0x010Du }, { 0x030Cu, 'd', 0x010Fu },
    { 0x030Cu, 'e', 0x011Bu }, { 0x030Cu, 'n', 0x0148u },
    { 0x030Cu, 'r', 0x0159u }, { 0x030Cu, 's', 0x0161u },
    { 0x030Cu, 't', 0x0165u }, { 0x030Cu, 'z', 0x017Eu },

    { 0x0327u, 'C', 0x00C7u }, { 0x0327u, 'G', 0x0122u },
    { 0x0327u, 'S', 0x015Eu }, { 0x0327u, 'T', 0x0162u },
    { 0x0327u, 'c', 0x00E7u }, { 0x0327u, 'g', 0x0123u },
    { 0x0327u, 's', 0x015Fu }, { 0x0327u, 't', 0x0163u },

    { 0x0328u, 'A', 0x0104u }, { 0x0328u, 'E', 0x0118u },
    { 0x0328u, 'a', 0x0105u }, { 0x0328u, 'e', 0x0119u }
};

enum dvb_table {
    DVB_ISO6937,
    DVB_LATIN1,
    DVB_LATIN5,        /* 8859-9  */
    DVB_LATIN9,        /* 8859-15 */
    DVB_CYRILLIC,      /* 8859-5  */
    DVB_GREEK,         /* 8859-7  */
    DVB_UTF8,
    DVB_UTF16BE
};

/* --- output ------------------------------------------------------------- */

/* Appends one code point. Returns 0 when it did not fit, which ends the
 * conversion rather than writing half a character. */
static int put_utf8(char *out, size_t size, size_t *used, unsigned code)
{
    size_t need = code < 0x80u ? 1u : code < 0x800u ? 2u
                : code < 0x10000u ? 3u : 4u;
    size_t at = *used;
    if (!code || at + need + 1u > size)
        return code ? 0 : 1;
    if (need == 1u) {
        out[at++] = (char)code;
    } else if (need == 2u) {
        out[at++] = (char)(0xC0u | (code >> 6));
        out[at++] = (char)(0x80u | (code & 0x3Fu));
    } else if (need == 3u) {
        out[at++] = (char)(0xE0u | (code >> 12));
        out[at++] = (char)(0x80u | ((code >> 6) & 0x3Fu));
        out[at++] = (char)(0x80u | (code & 0x3Fu));
    } else {
        out[at++] = (char)(0xF0u | (code >> 18));
        out[at++] = (char)(0x80u | ((code >> 12) & 0x3Fu));
        out[at++] = (char)(0x80u | ((code >> 6) & 0x3Fu));
        out[at++] = (char)(0x80u | (code & 0x3Fu));
    }
    *used = at;
    return 1;
}

/* --- tables ------------------------------------------------------------- */

static unsigned high_byte_code(enum dvb_table table, uint8_t byte)
{
    size_t i;
    switch (table) {
    case DVB_LATIN5:
        for (i = 0; i < sizeof(g_latin5) / sizeof(g_latin5[0]); ++i)
            if (g_latin5[i].byte == byte)
                return g_latin5[i].code;
        return byte;
    case DVB_LATIN9:
        for (i = 0; i < sizeof(g_latin9) / sizeof(g_latin9[0]); ++i)
            if (g_latin9[i].byte == byte)
                return g_latin9[i].code;
        return byte;
    case DVB_CYRILLIC:
        /* Regular enough to compute: the alphabet runs unbroken from 0xB0,
         * with the accented capitals in front of it. */
        if (byte == 0xA0u) return 0x00A0u;
        if (byte == 0xADu) return 0x00ADu;
        if (byte == 0xF0u) return 0x2116u;   /* numero sign */
        if (byte == 0xFDu) return 0x00A7u;
        if (byte >= 0xA1u && byte <= 0xACu) return 0x0401u + (byte - 0xA1u);
        if (byte >= 0xAEu && byte <= 0xAFu) return 0x040Eu + (byte - 0xAEu);
        if (byte >= 0xB0u && byte <= 0xEFu) return 0x0410u + (byte - 0xB0u);
        if (byte >= 0xF1u && byte <= 0xFCu) return 0x0451u + (byte - 0xF1u);
        return 0x045Eu + (byte - 0xFEu);
    case DVB_GREEK:
        if (byte <= 0xB5u)
            return g_greek_low[byte - HIGH_FIRST];
        if (byte == 0xB6u) return 0x0386u;
        if (byte == 0xB7u) return 0x00B7u;
        if (byte >= 0xB8u && byte <= 0xBAu) return 0x0388u + (byte - 0xB8u);
        if (byte == 0xBBu) return 0x00BBu;
        if (byte == 0xBCu) return 0x038Cu;
        if (byte == 0xBDu) return 0x00BDu;
        if (byte >= 0xBEu) return 0x038Eu + (byte - 0xBEu);
        return byte;
    default:
        return byte;
    }
}

static unsigned compose(unsigned mark, uint8_t base)
{
    size_t i;
    for (i = 0; i < sizeof(g_iso6937_compose) / sizeof(g_iso6937_compose[0]);
         ++i)
        if (g_iso6937_compose[i].mark == mark &&
            (uint8_t)g_iso6937_compose[i].base == base)
            return g_iso6937_compose[i].code;
    return 0;
}

/* --- the selector ------------------------------------------------------- */

/* Reads the character-table selector, if there is one, and reports where
 * the text itself starts. */
static enum dvb_table pick_table(const uint8_t *source, size_t size,
                                 size_t *start)
{
    *start = 0;
    if (!size || source[0] >= 0x20u)
        return DVB_ISO6937;          /* no selector: the default table */
    *start = 1;
    switch (source[0]) {
    case 0x01u: return DVB_CYRILLIC;         /* 8859-5  */
    case 0x03u: return DVB_GREEK;            /* 8859-7  */
    case 0x05u: return DVB_LATIN5;           /* 8859-9  */
    case 0x0Bu: return DVB_LATIN9;           /* 8859-15 */
    case 0x10u:
        /* Three bytes: 0x10 0x00 nn selects ISO 8859-nn. */
        if (size >= 3u) {
            *start = 3;
            switch (source[2]) {
            case 0x05u: return DVB_CYRILLIC;
            case 0x07u: return DVB_GREEK;
            case 0x09u: return DVB_LATIN5;
            case 0x0Fu: return DVB_LATIN9;
            default:    return DVB_LATIN1;
            }
        }
        return DVB_LATIN1;
    case 0x11u: return DVB_UTF16BE;
    case 0x15u: return DVB_UTF8;
    case 0x1Fu:
        /* An encoding_type_id follows and names something this does not
         * decode; treating the rest as Latin-1 keeps the output valid. */
        if (size >= 2u)
            *start = 2;
        return DVB_LATIN1;
    default:
        /* The remaining selectors are single-byte tables that are read as
         * Latin-1: the letters that differ come out wrong, but nothing
         * downstream is handed a byte it cannot read. */
        return DVB_LATIN1;
    }
}

/* --- conversion --------------------------------------------------------- */

size_t dtv_dvb_text_to_utf8(char *out, size_t size,
                            const uint8_t *source, size_t source_size)
{
    size_t at, used = 0;
    enum dvb_table table;
    if (!out || !size)
        return 0;
    out[0] = 0;
    if (!source)
        return 0;
    table = pick_table(source, source_size, &at);

    if (table == DVB_UTF8) {
        /* Already what is wanted. Control bytes still become spaces, and a
         * truncated multi-byte character at the end is dropped rather than
         * copied half. */
        while (at < source_size && used + 1u < size) {
            uint8_t ch = source[at];
            size_t length = ch < 0x80u ? 1u : (ch & 0xE0u) == 0xC0u ? 2u
                          : (ch & 0xF0u) == 0xE0u ? 3u
                          : (ch & 0xF8u) == 0xF0u ? 4u : 0u;
            if (!length || at + length > source_size)
                break;
            if (length == 1u) {
                out[used++] = ch < 0x20u || ch == 0x7Fu ? ' ' : (char)ch;
                ++at;
                continue;
            }
            if (used + length + 1u > size)
                break;
            while (length--)
                out[used++] = (char)source[at++];
        }
        out[used] = 0;
        return used;
    }

    if (table == DVB_UTF16BE) {
        while (at + 1u < source_size) {
            unsigned code = (unsigned)(source[at] << 8) | source[at + 1];
            at += 2;
            if (code < 0x20u || code == 0x7Fu)
                code = ' ';
            if (!put_utf8(out, size, &used, code))
                break;
        }
        out[used] = 0;
        return used;
    }

    while (at < source_size) {
        uint8_t ch = source[at++];
        unsigned code;
        /* The C0 and C1 control ranges carry no text. C1 is where DVB puts
         * its own formatting codes, and 0x8A is a line break. */
        if (ch < 0x20u || (ch >= 0x7Fu && ch < HIGH_FIRST)) {
            if (!put_utf8(out, size, &used, ' '))
                break;
            continue;
        }
        if (ch < HIGH_FIRST) {
            if (!put_utf8(out, size, &used, ch))
                break;
            continue;
        }
        if (table != DVB_ISO6937) {
            code = high_byte_code(table, ch);
            if (code && !put_utf8(out, size, &used, code))
                break;
            continue;
        }
        /* ISO 6937 from here down. */
        if (ch >= 0xC1u && ch <= 0xCFu) {
            unsigned mark = g_iso6937_marks[ch - 0xC1u];
            uint8_t base = at < source_size ? source[at++] : 0;
            unsigned composed;
            if (!mark || !base)
                continue;
            composed = compose(mark, base);
            if (composed) {
                if (!put_utf8(out, size, &used, composed))
                    break;
                continue;
            }
            /* Not a pair this knows: the letter and the mark separately
             * still say what was sent. */
            if (!put_utf8(out, size, &used, base))
                break;
            if (!put_utf8(out, size, &used, mark))
                break;
            continue;
        }
        code = ch < 0xC0u ? g_iso6937_a0[ch - HIGH_FIRST]
             : ch < 0xD0u ? 0u
                          : g_iso6937_d0[ch - 0xD0u];
        if (code && !put_utf8(out, size, &used, code))
            break;
    }
    out[used] = 0;
    return used;
}
