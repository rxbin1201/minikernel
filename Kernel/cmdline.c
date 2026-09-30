#include "cmdline.h"
#include "string.h"

#define MAX_WORDS 16

static char  buf[256];
static char *words[MAX_WORDS];
static int   word_count;

void cmdline_init(const char *line)
{
    size_t n = 0;
    for (; line[n] && n < sizeof(buf) - 1; n++)
        buf[n] = line[n];
    buf[n] = 0;

    word_count = 0;
    for (char *c = buf; *c && word_count < MAX_WORDS;) {
        while (*c == ' ')
            *c++ = 0;
        if (!*c)
            break;
        words[word_count++] = c;
        while (*c && *c != ' ')
            c++;
    }
}

int cmdline_has(const char *word)
{
    size_t len = strlen(word);
    for (int i = 0; i < word_count; i++)
        if (memcmp(words[i], word, len) == 0 && (words[i][len] == 0 || words[i][len] == '='))
            return 1;
    return 0;
}

const char *cmdline_get(const char *key)
{
    size_t len = strlen(key);
    for (int i = 0; i < word_count; i++)
        if (memcmp(words[i], key, len) == 0 && words[i][len] == '=')
            return words[i] + len + 1;
    return 0;
}
