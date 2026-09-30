#include "fs/vfs.h"
#include "mm/heap.h"
#include "lib/kprintf.h"
#include "lib/string.h"

static VfsNode *nodes;
static unsigned node_count;
static VfsNode  root_node = {"/", 1, 0, 0};

/* Normalisiert `in` zu "/a/b": ohne "./", ohne doppelte und abschliessende '/', ".." nimmt die letzte Komponente
 * weg (an der Wurzel bleibt es die Wurzel). Leerer Pfad -> "/". */
void vfs_normalize(const char *in, char *out, size_t max)
{
    size_t n = 0;
    while (*in) {
        while (*in == '/')
            in++;
        if (!*in)
            break;
        const char *start = in;
        while (*in && *in != '/')
            in++;
        size_t len = (size_t)(in - start);
        if (len == 1 && start[0] == '.')
            continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            while (n > 0 && out[n - 1] != '/')
                n--;
            if (n > 0)
                n--; /* das '/' vor der Komponente */
            continue;
        }
        if (n + 1 + len < max) {
            out[n++] = '/';
            memcpy(out + n, start, len);
            n += len;
        }
    }
    if (n == 0)
        out[n++] = '/';
    out[n] = 0;
}

#define normalize vfs_normalize

static uint64_t parse_octal(const char *s, int len)
{
    uint64_t v = 0;
    for (int i = 0; i < len && s[i] >= '0' && s[i] <= '7'; i++)
        v = v * 8 + (s[i] - '0');
    return v;
}

/* Laeuft ueber das Archiv; fuellt `out` (falls nicht NULL) und liefert die Anzahl der Eintraege. */
static unsigned scan(const uint8_t *tar, uint64_t size, VfsNode *out)
{
    unsigned count = 0;
    for (uint64_t off = 0; off + 512 <= size;) {
        const char *h = (const char *)(tar + off);
        if (h[0] == 0)
            break; /* Ende: leerer Header */

        uint64_t fsize = parse_octal(h + 124, 12);
        char type = h[156];

        /* Name = prefix + "/" + name (ustar) */
        char full[VFS_PATH_MAX * 2];
        size_t n = 0;
        if (memcmp(h + 257, "ustar", 5) == 0 && h[345]) {
            for (int i = 0; i < 155 && h[345 + i] && n < sizeof(full) - 2; i++)
                full[n++] = h[345 + i];
            full[n++] = '/';
        }
        for (int i = 0; i < 100 && h[i] && n < sizeof(full) - 1; i++)
            full[n++] = h[i];
        full[n] = 0;

        char path[VFS_PATH_MAX];
        normalize(full, path, sizeof(path));

        int is_dir = type == '5';
        int is_file = type == '0' || type == 0;
        if ((is_dir || is_file) && !(path[0] == '/' && path[1] == 0)) {
            if (out) {
                VfsNode *node = &out[count];
                memcpy(node->path, path, strlen(path) + 1);
                node->is_dir = is_dir;
                node->data = tar + off + 512;
                node->size = is_dir ? 0 : fsize;
            }
            count++;
        }
        off += 512 + ((fsize + 511) & ~511ULL);
    }
    return count;
}

int vfs_init(const void *tar, uint64_t size)
{
    if (!tar || size < 512)
        return -1;
    unsigned count = scan(tar, size, 0);
    nodes = kcalloc(count ? count : 1, sizeof(VfsNode));
    if (!nodes)
        return -1;
    node_count = scan(tar, size, nodes);
    return (int)node_count;
}

int vfs_count(void)
{
    return (int)node_count;
}

const VfsNode *vfs_lookup(const char *path)
{
    char norm[VFS_PATH_MAX];
    normalize(path, norm, sizeof(norm));
    if (norm[0] == '/' && norm[1] == 0)
        return &root_node;
    for (unsigned i = 0; i < node_count; i++)
        if (strcmp(nodes[i].path, norm) == 0)
            return &nodes[i];
    return 0;
}

const char *vfs_basename(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' && p[1])
            base = p + 1;
    return base;
}

const VfsNode *vfs_readdir(const char *dir, unsigned index)
{
    char norm[VFS_PATH_MAX];
    normalize(dir, norm, sizeof(norm));
    size_t dlen = strlen(norm);
    if (dlen == 1)
        dlen = 0; /* Wurzel: Praefix ist leer, Kinder beginnen direkt mit '/' */

    unsigned seen = 0;
    for (unsigned i = 0; i < node_count; i++) {
        const char *p = nodes[i].path;
        if (memcmp(p, norm, dlen) != 0 || p[dlen] != '/')
            continue;
        const char *rest = p + dlen + 1;
        if (!*rest)
            continue;
        int direct = 1;
        for (const char *c = rest; *c; c++)
            if (*c == '/')
                direct = 0;
        if (!direct)
            continue;
        if (seen++ == index)
            return &nodes[i];
    }
    return 0;
}

uint64_t vfs_total_size(void)
{
    uint64_t total = 0;
    for (unsigned i = 0; i < node_count; i++)
        total += nodes[i].size;
    return total;
}
