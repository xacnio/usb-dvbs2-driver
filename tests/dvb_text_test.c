/* DVB SI text decoding. The point of the file under test is that a service
 * name or a programme title reaches the rest of the program as UTF-8
 * whatever table the broadcaster used, so every case here checks the exact
 * bytes: "looks right" is what the old pass-through appeared to do until a
 * Turkish capital dotted I reached a file name. */
#include <stdio.h>
#include <string.h>

#include "dvb_text.h"

static int g_failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition); \
        ++g_failures; \
    } \
} while (0)

static void expect(const char *what, const unsigned char *in, size_t size,
                   const char *want)
{
    char out[256];
    dtv_dvb_text_to_utf8(out, sizeof(out), in, size);
    if (strcmp(out, want) != 0) {
        printf("FAIL %s: got \"%s\", wanted \"%s\"\n", what, out, want);
        ++g_failures;
    }
}

int main(void)
{
    /* ISO 8859-9, the table Turksat announces with selector 0x05. 0xDD is
     * the capital dotted I and is not valid UTF-8 by itself. */
    {
        const unsigned char in[] = { 0x05, 'P', 0xDD, 'D', 'E' };
        expect("8859-9 capital dotted I", in, sizeof(in), "P\xC4\xB0" "DE");
    }
    {
        const unsigned char in[] = { 0x05, 0xDE, 'a', 'r', 'k', 0xFD };
        expect("8859-9 s-cedilla and dotless i", in, sizeof(in),
               "\xC5\x9E" "ark\xC4\xB1");
    }
    /* No selector means ISO 6937, where the same letter is a dot-above mark
     * in front of a plain I. */
    {
        const unsigned char in[] = { 'P', 0xC7, 'I', 'D', 'E' };
        expect("6937 dot above + I", in, sizeof(in), "P\xC4\xB0" "DE");
    }
    {
        const unsigned char in[] = { 0xCB, 'S', 'a', 0xC6, 'g' };
        expect("6937 cedilla and breve", in, sizeof(in),
               "\xC5\x9E" "a\xC4\x9F");
    }
    /* A mark this does not compose still says what was sent, as the letter
     * followed by the combining character. */
    {
        const unsigned char in[] = { 0xC5, 'w' };
        expect("6937 uncomposed pair", in, sizeof(in), "w\xCC\x84");
    }
    /* Single-byte ISO 6937 letters, including the dotless i at 0xF5. */
    {
        const unsigned char in[] = { 'a', 0xF5, 'b', 0xFB };
        expect("6937 single bytes", in, sizeof(in),
               "a\xC4\xB1" "b\xC3\x9F");
    }
    /* The selectors that hand over text needing no table at all. */
    {
        const unsigned char in[] = { 0x15, 'P', 0xC4, 0xB0 };
        expect("utf-8 selector", in, sizeof(in), "P\xC4\xB0");
    }
    {
        const unsigned char in[] = { 0x11, 0x00, 'P', 0x01, 0x30 };
        expect("utf-16be selector", in, sizeof(in), "P\xC4\xB0");
    }
    /* Three-byte form: 0x10 0x00 0x09 is ISO 8859-9 again. */
    {
        const unsigned char in[] = { 0x10, 0x00, 0x09, 'P', 0xDD };
        expect("8859-9 through the long selector", in, sizeof(in),
               "P\xC4\xB0");
    }
    /* ISO 8859-5: the alphabet runs unbroken from 0xB0. */
    {
        const unsigned char in[] = { 0x01, 0xBC, 0xDE, 0xE1, 0xDA, 0xD2,
                                     0xD0 };
        expect("8859-5 cyrillic", in, sizeof(in),
               "\xD0\x9C\xD0\xBE\xD1\x81\xD0\xBA\xD0\xB2\xD0\xB0");
    }
    /* Control codes are separators, not text. */
    {
        const unsigned char in[] = { 0x05, 'a', 0x8A, 'b' };
        expect("control byte becomes a space", in, sizeof(in), "a b");
    }
    /* Nothing may be written past the end, and never half a character. */
    {
        const unsigned char in[] = { 0x05, 'a', 0xDD, 'b' };
        char out[3];
        size_t used = dtv_dvb_text_to_utf8(out, sizeof(out), in, sizeof(in));
        CHECK(used < sizeof(out));
        CHECK(out[used] == 0);
        CHECK(strcmp(out, "a") == 0);
    }
    {
        char out[8];
        CHECK(dtv_dvb_text_to_utf8(out, sizeof(out), NULL, 0) == 0);
        CHECK(out[0] == 0);
    }

    if (g_failures) {
        printf("dvb-text: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("dvb-text: every table decodes to utf-8 -- ok\n");
    return 0;
}
