/**
 * filededup.c — File Deduplication ADT implementation
 *
 * Algorithm overview
 * ------------------
 * 1. Pre-filter by file size: files of different sizes cannot be equal.
 * 2. Hash each file with tabulation hashing (32 rows × 256 cols of
 *    64-bit random values, XOR-folded with a row rotation per byte):
 *    files with different hashes cannot be equal.
 * 3. Within each (size, hash) bucket, do exact byte-by-byte comparison
 *    to resolve the (astronomically rare) hash collisions and build the
 *    final equivalence classes.
 *
 * Data structures
 * ---------------
 * - A hash table of buckets, each keyed by (size, hash).  Open
 *   addressing with linear probing; load factor kept ≤ 0.5.
 * - Each bucket holds a dynamic array of file-path strings.
 *
 * Complexity
 * ----------
 * - FDCheck  : O(S) amortised, where S is the file size.
 * - FDDump   : O(N × S) worst case for the byte comparison pass, where
 *              N is the number of registered paths.  In practice the
 *              size+hash pre-filter reduces real work dramatically.
 */

/* Request POSIX.1-2008 to expose strdup() under -std=c11 */
#define _POSIX_C_SOURCE 200809L

#include "filededup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
/* ------------------------------------------------------------------ */
/* Tabulation hash parameters                                           */
/* ------------------------------------------------------------------ */

#define TAB_ROWS 32
#define TAB_COLS 256   /* one entry per possible byte value since the byte is [0 ; 255] */

/* Precomputed tabulation table – filled once by tab_init(). */
static uint64_t tab_table[TAB_ROWS][TAB_COLS];
static int      tab_ready = 0;   /* 1 after first initialisation */

/**
 * tab_init – fill the tabulation table with pseudo-random 64-bit values
 * using a simple xorshift64 PRNG seeded with a fixed constant.
 * Called once at FDInit time.
 */
static void tab_init(void)
{
    if (tab_ready) return;

    uint64_t state = UINT64_C(0xdeadbeefcafebabe);
    for (int r = 0; r < TAB_ROWS; r++) {
        for (int c = 0; c < TAB_COLS; c++) {
            /* xorshift64 step */
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            tab_table[r][c] = state;
        }
    }
    tab_ready = 1;
}

/**
 * tab_hash_file – compute the tabulation hash of an open binary stream.
 *
 * Reads the stream from its current position to EOF, XOR-ing each byte's
 * contribution from the table.  Row is advanced modulo TAB_ROWS after
 * every byte.
 */
static uint64_t tab_hash_file(FILE *f) {
    uint64_t h = 0;
    int row = 0;
    unsigned char buf[65536]; 
    size_t n;
    
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < n; i++) {
            h ^= tab_table[row][buf[i]];
            row = (row + 1) & 31; /* Safe bitwise rotation */
        }
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* Bucket: a group of paths sharing the same (size, hash) key          */
/* ------------------------------------------------------------------ */

#define BUCKET_INIT_CAP 4

typedef struct {
    long     file_size;    /* -1 marks an empty slot in the hash table */
    uint64_t hash;
    char   **paths;        /* dynamic array of owned C strings          */
    int      count;
    int      cap;
} Bucket;

/** Append a copy of `path` to the bucket, growing as needed. */
static int bucket_push(Bucket *b, const char *path)
{
    if (b->count == b->cap) {
        int newcap = b->cap * 2;
        char **tmp = realloc(b->paths, (size_t)newcap * sizeof(char *));
        if (!tmp) return 0;
        b->paths = tmp;
        b->cap   = newcap;
    }
    b->paths[b->count] = strdup(path);
    if (!b->paths[b->count]) return 0;
    b->count++;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Hash table of buckets                                                */
/* ------------------------------------------------------------------ */

#define HT_INIT_CAP  8192     /* must be a power of two */
#define HT_MAX_LOAD  0.5    /* rehash when load exceeds this fraction  */

typedef struct {
    Bucket *slots;
    int     cap;    /* number of slots – always a power of two */
    int     used;   /* number of occupied slots                 */
} HashTable;

/**
 * ht_slot – compute the probe index for (size, hash) inside a table
 * of capacity `cap` (must be a power of two).
 */
static inline int ht_slot(long file_size, uint64_t hash, int cap)
{
    /* Mix size and hash into a single 64-bit value, then reduce. */
    uint64_t key = (uint64_t)file_size * UINT64_C(0x9e3779b97f4a7c15)
                   ^ hash;
    return (int)(key & (uint64_t)(cap - 1));
}

/** Initialise a HashTable with `cap` empty slots. */
static int ht_init(HashTable *ht, int cap)
{
    ht->slots = calloc((size_t)cap, sizeof(Bucket));
    if (!ht->slots) return 0;
    /* Mark every slot empty with file_size = -1 */
    for (int i = 0; i < cap; i++) ht->slots[i].file_size = -1;
    ht->cap  = cap;
    ht->used = 0;
    return 1;
}

/** Forward declaration needed for rehash. */
static int ht_insert(HashTable *ht, long file_size, uint64_t hash,
                     const char *path);

/** Double the hash table capacity and re-insert all existing entries. */
static int ht_grow(HashTable *ht)
{
    HashTable bigger;
    if (!ht_init(&bigger, ht->cap * 2)) return 0;

    for (int i = 0; i < ht->cap; i++) {
        Bucket *b = &ht->slots[i];
        if (b->file_size == -1) continue;
        for (int j = 0; j < b->count; j++) {
            if (!ht_insert(&bigger, b->file_size, b->hash, b->paths[j]))
                return 0;   /* OOM during rehash – caller will handle */
        }
        /* Release old paths (copies are now owned by bigger). */
        for (int j = 0; j < b->count; j++) free(b->paths[j]);
        free(b->paths);
    }
    free(ht->slots);
    *ht = bigger;
    return 1;
}

/**
 * ht_insert – find or create the bucket for (file_size, hash) and
 * append `path` to it.  Rehashes if the load factor is exceeded.
 */
static int ht_insert(HashTable *ht, long file_size, uint64_t hash, const char *path)
{
    /* Load factor check */
    if ((double)(ht->used + 1) > HT_MAX_LOAD * (double)ht->cap)
        if (!ht_grow(ht)) return 0;

    int idx = ht_slot(file_size, hash, ht->cap);
    while (ht->slots[idx].file_size != -1) {
        Bucket *b = &ht->slots[idx];
        if (b->file_size == file_size && b->hash == hash) {
            /* If bucket_push fails (OOM), return 0 (Failure) */
            if (!bucket_push(b, path)) return 0;
            return 1; /* Success: File added to existing bucket */
        }
        idx = (idx + 1) & (ht->cap - 1);
    }

    /* Empty slot found: create a new bucket */
    Bucket *b = &ht->slots[idx];
    b->file_size = file_size;
    b->hash = hash;
    b->cap = BUCKET_INIT_CAP;
    b->count = 0;
    b->paths = malloc((size_t)BUCKET_INIT_CAP * sizeof(char *));
    if (!b->paths) return 0;

    ht->used++;
    if (!bucket_push(b, path)) return 0;
    
    return 1; /* Success: File added to new bucket */
}
/* ------------------------------------------------------------------ */
/* Exact byte-by-byte file comparison                                   */
/* ------------------------------------------------------------------ */

#define CMP_BUF_SIZE 65536   /* 64 KiB read buffer */

/**
 * files_equal – return 1 iff the two files at the given paths are
 * identical byte-for-byte.  Assumes both files have the same size
 * (pre-checked by the caller via the hash-table key).
 */
static int files_equal(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb");
    FILE *fb = fopen(b, "rb");
    if (!fa || !fb) {
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        return 0;
    }

    static unsigned char bufa[CMP_BUF_SIZE];
    static unsigned char bufb[CMP_BUF_SIZE];
    int equal = 1;

    while (equal) {
        size_t ra = fread(bufa, 1, CMP_BUF_SIZE, fa);
        size_t rb = fread(bufb, 1, CMP_BUF_SIZE, fb);
        if (ra != rb || memcmp(bufa, bufb, ra) != 0) { equal = 0; break; }
        if (ra == 0) break;   /* EOF reached on both */
    }

    fclose(fa);
    fclose(fb);
    return equal;
}

/* ------------------------------------------------------------------ */
/* Equivalence-class refinement within a (size, hash) bucket           */
/* ------------------------------------------------------------------ */

/**
 * refine_bucket – split the paths in `b` into groups of truly equal
 * files using exact comparison.
 *
 * Algorithm: greedy grouping.  Iterate over paths; assign each to the
 * first existing group whose representative it matches, or start a new
 * group.  This is O(n^2) in the number of paths per bucket, but
 * buckets are expected to be very small (≤ a handful of paths in
 * practice, due to strong hashing).
 *
 * `groups`    – out: array of char** (each a NULL-terminated list of paths)
 * `ngroups`   – out: number of groups written
 * Returns 1 on success, 0 on allocation failure.
 */
static int refine_bucket(const Bucket *b,
                         char ****groups_out, int *ngroups_out)
{
    /* Each group: dynamic array of (char*) paths, NULL-terminated */
    char ***groups   = NULL;
    int    *gcounts  = NULL;   /* current element count per group        */
    int    *gcaps    = NULL;   /* allocated capacity per group           */
    int     ngroups  = 0;

    for (int i = 0; i < b->count; i++) {
        const char *path = b->paths[i];
        int placed = 0;

        for (int g = 0; g < ngroups && !placed; g++) {
            /* Compare against the representative (first member) */
            if (files_equal(groups[g][0], path)) {
                /* Grow this group if needed (+1 for NULL sentinel) */
                if (gcounts[g] + 1 >= gcaps[g]) {
                    int nc = gcaps[g] * 2;
                    char **tmp = realloc(groups[g],
                                        (size_t)(nc + 1) * sizeof(char *));
                    if (!tmp) goto oom;
                    groups[g] = tmp;
                    gcaps[g]  = nc;
                }
                groups[g][gcounts[g]++] = (char *)path;
                groups[g][gcounts[g]]   = NULL;
                placed = 1;
            }
        }

        if (!placed) {
            /* Start a new group */
            int nc = ngroups + 1;
            char ***gtmp  = realloc(groups,  (size_t)nc * sizeof(char **));
            int   *cttmp  = realloc(gcounts, (size_t)nc * sizeof(int));
            int   *cptmp  = realloc(gcaps,   (size_t)nc * sizeof(int));
            if (!gtmp || !cttmp || !cptmp) goto oom;
            groups  = gtmp;
            gcounts = cttmp;
            gcaps   = cptmp;

            int initcap = 4;
            groups[ngroups] = malloc((size_t)(initcap + 1) * sizeof(char *));
            if (!groups[ngroups]) goto oom;
            groups[ngroups][0]  = (char *)path;
            groups[ngroups][1]  = NULL;
            gcounts[ngroups]    = 1;
            gcaps[ngroups]      = initcap;
            ngroups++;
        }
    }

    free(gcounts);
    free(gcaps);
    *groups_out  = groups;
    *ngroups_out = ngroups;
    return 1;

oom:
    /* Best-effort cleanup; paths themselves are owned by the bucket */
    for (int g = 0; g < ngroups; g++) free(groups[g]);
    free(groups);
    free(gcounts);
    free(gcaps);
    return 0;
}

/* ------------------------------------------------------------------ */
/* FILEDEDUP struct and public API                                      */
/* ------------------------------------------------------------------ */

struct filededup_s {
    HashTable ht;
};

/* FDInit ------------------------------------------------------------ */
FILEDEDUP FDInit(void)
{
    tab_init();

    FILEDEDUP fd = malloc(sizeof(struct filededup_s));
    if (!fd) return NULL;

    if (!ht_init(&fd->ht, HT_INIT_CAP)) {
        free(fd);
        return NULL;
    }
    return fd;
}

/* FDCheck ----------------------------------------------------------- */
int FDCheck(FILEDEDUP fd, char *filepath)
{
    if (!fd || !filepath) return 0;

    /* OPTIMIZATION 1: Get size instantly via OS metadata */
    struct stat st;
    if (stat(filepath, &st) != 0) return 0;
    long size = st.st_size;

    FILE *f = fopen(filepath, "rb");
    if (!f) return 0;

    /* Compute tabulation hash */
    uint64_t h = tab_hash_file(f);
    fclose(f);

    return ht_insert(&fd->ht, size, h, filepath);
}

/* FDDump ------------------------------------------------------------ */
char **FDDump(FILEDEDUP fd, int *length)
{
    if (!fd || !length) return NULL;
    
    *length = 0;

    /* First pass: count total output slots needed across all buckets. */
    int total = 0;
    for (int i = 0; i < fd->ht.cap; i++) {
        Bucket *b = &fd->ht.slots[i];
        if (b->file_size == -1 || b->count < 2) continue;
        /* Each path + 1 NULL sentinel per group (worst case: 1 group) */
        total += b->count + 1;
    }
    if (total == 0) return NULL;

    /* Allocate the result array (may grow via realloc below). */
    int    cap    = total;
    int    used   = 0;
    char **result = malloc((size_t)cap * sizeof(char *));
    if (!result) return NULL;

    /* Second pass: refine each candidate bucket. */
    for (int i = 0; i < fd->ht.cap; i++) {
        Bucket *b = &fd->ht.slots[i];
        if (b->file_size == -1 || b->count < 2) continue;

        char ***groups  = NULL;
        int    ngroups  = 0;
        if (!refine_bucket(b, &groups, &ngroups)) {
            /* Allocation failure: free what we have and return NULL */
            for (int k = 0; k < used; k++) if (result[k]) free(result[k]);
            free(result);
            return NULL;
        }

        for (int g = 0; g < ngroups; g++) {
            /* Count members */
            int members = 0;
            while (groups[g][members]) members++;
            if (members < 2) { free(groups[g]); continue; }

            /* Ensure space: members paths + 1 NULL */
            int needed = used + members + 1;
            if (needed > cap) {
                int newcap = cap * 2 > needed ? cap * 2 : needed * 2;
                char **tmp = realloc(result, (size_t)newcap * sizeof(char *));
                if (!tmp) {
                    for (int k = 0; k < used; k++) if (result[k]) free(result[k]);
                    free(result);
                    for (int gg = g; gg < ngroups; gg++) free(groups[gg]);
                    free(groups);
                    return NULL;
                }
                result = tmp;
                cap    = newcap;
            }

            for (int m = 0; m < members; m++) {
                result[used] = strdup(groups[g][m]);
                if (!result[used]) {
                    for (int k = 0; k < used; k++) if (result[k]) free(result[k]);
                    free(result);
                    for (int gg = g; gg < ngroups; gg++) free(groups[gg]);
                    free(groups);
                    return NULL;
                }
                used++;
            }
            result[used++] = NULL;   /* sentinel between groups */
            free(groups[g]);
        }
        free(groups);
    }

    *length = used;
    if (used == 0) { free(result); return NULL; }
    return result;
}
