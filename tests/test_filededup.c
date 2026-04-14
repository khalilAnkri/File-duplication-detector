/**
 * test_filededup.c — Test suite for the FILEDEDUP ADT
 *
 * Compile & run:
 *   gcc -Wall -pedantic -std=c11 -O3 test_filededup.c filededup.c -o test_fd && ./test_fd
 *
 * Each test prints PASS or FAIL. Exit code is 0 if all tests pass, 1 otherwise.
 */

#define _POSIX_C_SOURCE 200809L

#include "filededup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* unlink() */

/* ------------------------------------------------------------------ */
/* Minimal test framework                                               */
/* ------------------------------------------------------------------ */

static int tests_run    = 0;
static int tests_passed = 0;

#define PASS() do { tests_run++; tests_passed++; \
    printf("  PASS  %s\n", __func__); } while(0)
#define FAIL(msg) do { tests_run++; \
    printf("  FAIL  %s — %s\n", __func__, msg); } while(0)
#define ASSERT(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

/** Write `content` to `path`. Returns 1 on success. */
static int write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fputs(content, f);
    fclose(f);
    return 1;
}

/** Write `len` bytes of value `byte` to `path`. */
static int write_bytes(const char *path, unsigned char byte, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    for (size_t i = 0; i < len; i++) fputc(byte, f);
    fclose(f);
    return 1;
}

/** Write an empty file. */
static int write_empty(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/**
 * find_group – scan the FDDump output and return the index of the group
 * that contains `path`, or -1 if not found.
 */
static int find_group(char **dump, int length, const char *path)
{
    int group = 0;
    for (int i = 0; i < length; i++) {
        if (dump[i] == NULL) { group++; continue; }
        if (strcmp(dump[i], path) == 0) return group;
    }
    return -1;
}

/** Count the number of non-NULL entries (paths) in a dump array. */
static int count_paths(char **dump, int length)
{
    int n = 0;
    for (int i = 0; i < length; i++) if (dump[i] != NULL) n++;
    return n;
}

/** Count the number of NULL sentinels (= number of groups) in a dump. */
static int count_groups(char **dump, int length)
{
    int n = 0;
    for (int i = 0; i < length; i++) if (dump[i] == NULL) n++;
    return n;
}

/** Free all strings and the array returned by FDDump. */
static void free_dump(char **dump, int length)
{
    if (!dump) return;
    for (int i = 0; i < length; i++) free(dump[i]);
    free(dump);
}

/* ------------------------------------------------------------------ */
/* Tests                                                                */
/* ------------------------------------------------------------------ */

/* --- FDInit -------------------------------------------------------- */

static void test_init_returns_nonnull(void)
{
    FILEDEDUP fd = FDInit();
    ASSERT(fd != NULL, "FDInit() returned NULL");
    PASS();
}

static void test_init_independent_instances(void)
{
    FILEDEDUP a = FDInit();
    FILEDEDUP b = FDInit();
    ASSERT(a != NULL && b != NULL, "FDInit() returned NULL");
    ASSERT(a != b, "two FDInit() calls returned the same pointer");
    PASS();
}

/* --- FDCheck error handling --------------------------------------- */

static void test_check_null_fd(void)
{
    int r = FDCheck(NULL, "./fd_dummy.txt");
    ASSERT(r == 0, "FDCheck with NULL fd should return 0");
    PASS();
}

static void test_check_null_path(void)
{
    FILEDEDUP fd = FDInit();
    ASSERT(fd != NULL, "FDInit failed");
    int r = FDCheck(fd, NULL);
    ASSERT(r == 0, "FDCheck with NULL path should return 0");
    PASS();
}

static void test_check_nonexistent_file(void)
{
    FILEDEDUP fd = FDInit();
    ASSERT(fd != NULL, "FDInit failed");
    int r = FDCheck(fd, "./fd_this_file_does_not_exist_xyz123.txt");
    ASSERT(r == 0, "FDCheck with non-existent file should return 0");
    PASS();
}

static void test_check_existing_file(void)
{
    const char *path = "./fd_check_ok.txt";
    write_file(path, "hello");
    FILEDEDUP fd = FDInit();
    ASSERT(fd != NULL, "FDInit failed");
    int r = FDCheck(fd, (char *)path);
    ASSERT(r == 1, "FDCheck with valid file should return 1");
    unlink(path);
    PASS();
}

/* --- FDDump with no duplicates ------------------------------------ */

static void test_dump_empty_detector(void)
{
    FILEDEDUP fd = FDInit();
    ASSERT(fd != NULL, "FDInit failed");
    int len = 42;
    char **dump = FDDump(fd, &len);
    ASSERT(dump == NULL, "FDDump on empty detector should return NULL");
    ASSERT(len == 0,     "FDDump on empty detector should set length=0");
    PASS();
}

static void test_dump_single_unique_file(void)
{
    const char *p = "./fd_unique1.txt";
    write_file(p, "I am unique");
    FILEDEDUP fd = FDInit();
    FDCheck(fd, (char *)p);
    int len = 99;
    char **dump = FDDump(fd, &len);
    ASSERT(dump == NULL || count_paths(dump, len) == 0,
           "single unique file should not appear in dump");
    free_dump(dump, len);
    unlink(p);
    PASS();
}

static void test_dump_many_unique_files(void)
{
    const char *paths[] = {
        "./fd_u1.txt", "./fd_u2.txt", "./fd_u3.txt",
        "./fd_u4.txt", "./fd_u5.txt"
    };
    const char *contents[] = { "alpha", "beta", "gamma", "delta", "epsilon" };
    int n = 5;
    FILEDEDUP fd = FDInit();
    for (int i = 0; i < n; i++) {
        write_file(paths[i], contents[i]);
        FDCheck(fd, (char *)paths[i]);
    }
    int len = 99;
    char **dump = FDDump(fd, &len);
    ASSERT(count_paths(dump, len) == 0,
           "all-unique files should not appear in dump");
    free_dump(dump, len);
    for (int i = 0; i < n; i++) unlink(paths[i]);
    PASS();
}

/* --- FDDump with one pair of duplicates --------------------------- */

static void test_dump_one_pair(void)
{
    const char *a = "./fd_dup_a.txt";
    const char *b = "./fd_dup_b.txt";
    write_file(a, "duplicate content");
    write_file(b, "duplicate content");

    FILEDEDUP fd = FDInit();
    FDCheck(fd, (char *)a);
    FDCheck(fd, (char *)b);

    int len = 0;
    char **dump = FDDump(fd, &len);

    ASSERT(dump != NULL,                 "FDDump should not be NULL");
    ASSERT(count_paths(dump, len) == 2,  "should have exactly 2 paths");
    ASSERT(count_groups(dump, len) == 1, "should have exactly 1 group");
    int ga = find_group(dump, len, a);
    int gb = find_group(dump, len, b);
    ASSERT(ga != -1 && gb != -1, "both paths should appear in dump");
    ASSERT(ga == gb,             "both paths should be in the same group");

    free_dump(dump, len);
    unlink(a); unlink(b);
    PASS();
}

/* --- FDDump with multiple disjoint groups ------------------------- */

static void test_dump_two_independent_pairs(void)
{
    const char *a1 = "./fd_g1a.txt", *a2 = "./fd_g1b.txt";
    const char *b1 = "./fd_g2a.txt", *b2 = "./fd_g2b.txt";
    write_file(a1, "group one content");
    write_file(a2, "group one content");
    write_file(b1, "group two content!");
    write_file(b2, "group two content!");

    FILEDEDUP fd = FDInit();
    FDCheck(fd, (char *)a1); FDCheck(fd, (char *)a2);
    FDCheck(fd, (char *)b1); FDCheck(fd, (char *)b2);

    int len = 0;
    char **dump = FDDump(fd, &len);

    ASSERT(dump != NULL,                 "FDDump should not be NULL");
    ASSERT(count_paths(dump, len) == 4,  "should have 4 paths total");
    ASSERT(count_groups(dump, len) == 2, "should have 2 groups");
    ASSERT(find_group(dump, len, a1) == find_group(dump, len, a2),
           "a1 and a2 should be in the same group");
    ASSERT(find_group(dump, len, b1) == find_group(dump, len, b2),
           "b1 and b2 should be in the same group");
    ASSERT(find_group(dump, len, a1) != find_group(dump, len, b1),
           "group 1 and group 2 should be different");

    free_dump(dump, len);
    unlink(a1); unlink(a2); unlink(b1); unlink(b2);
    PASS();
}

static void test_dump_mixed_unique_and_duplicates(void)
{
    const char *dup1 = "./fd_m1.txt", *dup2 = "./fd_m2.txt";
    const char *uniq = "./fd_m3.txt";
    write_file(dup1, "shared");
    write_file(dup2, "shared");
    write_file(uniq, "lonely");

    FILEDEDUP fd = FDInit();
    FDCheck(fd, (char *)dup1);
    FDCheck(fd, (char *)dup2);
    FDCheck(fd, (char *)uniq);

    int len = 0;
    char **dump = FDDump(fd, &len);

    ASSERT(count_paths(dump, len) == 2,     "only the duplicate pair should appear");
    ASSERT(count_groups(dump, len) == 1,    "should be exactly 1 group");
    ASSERT(find_group(dump, len, uniq) == -1, "unique file must not appear");

    free_dump(dump, len);
    unlink(dup1); unlink(dup2); unlink(uniq);
    PASS();
}

/* --- Same size, different content (must NOT be grouped) ----------- */

static void test_same_size_different_content(void)
{
    const char *a = "./fd_ss1.txt";
    const char *b = "./fd_ss2.txt";
    write_file(a, "AAAA");   /* 4 bytes */
    write_file(b, "BBBB");   /* 4 bytes, same size */

    FILEDEDUP fd = FDInit();
    FDCheck(fd, (char *)a);
    FDCheck(fd, (char *)b);

    int len = 0;
    char **dump = FDDump(fd, &len);

    /* They may share the same hash bucket but must end up in different groups
       (or both be unique if hash differs too). */
    int ga = find_group(dump, len, a);
    int gb = find_group(dump, len, b);
    ASSERT(ga == -1 || gb == -1 || ga != gb,
           "files with same size but different content must not be grouped");

    free_dump(dump, len);
    unlink(a); unlink(b);
    PASS();
}

/* --- Empty files --------------------------------------------------- */

static void test_empty_files_are_duplicates(void)
{
    const char *a = "./fd_empty1.txt";
    const char *b = "./fd_empty2.txt";
    write_empty(a);
    write_empty(b);

    FILEDEDUP fd = FDInit();
    FDCheck(fd, (char *)a);
    FDCheck(fd, (char *)b);

    int len = 0;
    char **dump = FDDump(fd, &len);

    ASSERT(dump != NULL, "two empty files should be reported as duplicates");
    ASSERT(find_group(dump, len, a) != -1, "empty file a should appear");
    ASSERT(find_group(dump, len, b) != -1, "empty file b should appear");
    ASSERT(find_group(dump, len, a) == find_group(dump, len, b),
           "empty files should be in the same group");

    free_dump(dump, len);
    unlink(a); unlink(b);
    PASS();
}

/* --- Large files --------------------------------------------------- */

static void test_large_identical_files(void)
{
    const char *a = "./fd_large1.bin";
    const char *b = "./fd_large2.bin";
    /* 1 MB of 0xAB bytes */
    write_bytes(a, 0xAB, 1024 * 1024);
    write_bytes(b, 0xAB, 1024 * 1024);

    FILEDEDUP fd = FDInit();
    int ra = FDCheck(fd, (char *)a);
    int rb = FDCheck(fd, (char *)b);
    ASSERT(ra == 1 && rb == 1, "FDCheck should succeed for large files");

    int len = 0;
    char **dump = FDDump(fd, &len);
    ASSERT(dump != NULL, "large duplicate files should be detected");
    ASSERT(find_group(dump, len, a) == find_group(dump, len, b),
           "large duplicate files should be in the same group");

    free_dump(dump, len);
    unlink(a); unlink(b);
    PASS();
}

static void test_large_different_files(void)
{
    const char *a = "./fd_ldiff1.bin";
    const char *b = "./fd_ldiff2.bin";
    write_bytes(a, 0x11, 1024 * 1024);
    write_bytes(b, 0x22, 1024 * 1024);  /* same size, different content */

    FILEDEDUP fd = FDInit();
    FDCheck(fd, (char *)a);
    FDCheck(fd, (char *)b);

    int len = 0;
    char **dump = FDDump(fd, &len);
    int ga = find_group(dump, len, a);
    int gb = find_group(dump, len, b);
    ASSERT(ga == -1 || gb == -1 || ga != gb,
           "large files with same size but different content must not be grouped");

    free_dump(dump, len);
    unlink(a); unlink(b);
    PASS();
}

/* --- Stress: many files ------------------------------------------- */

#define STRESS_N 200

static void test_stress_all_unique(void)
{
    char path[64];
    char content[64];
    FILEDEDUP fd = FDInit();
    ASSERT(fd != NULL, "FDInit failed");

    for (int i = 0; i < STRESS_N; i++) {
        snprintf(path,    sizeof(path),    "./fd_stress_u%d.txt", i);
        snprintf(content, sizeof(content), "unique file number %d !!!", i);
        write_file(path, content);
        FDCheck(fd, path);
    }

    int len = 0;
    char **dump = FDDump(fd, &len);
    ASSERT(count_paths(dump, len) == 0, "all-unique stress: no paths expected");
    free_dump(dump, len);

    for (int i = 0; i < STRESS_N; i++) {
        snprintf(path, sizeof(path), "./fd_stress_u%d.txt", i);
        unlink(path);
    }
    PASS();
}

static void test_stress_all_duplicates(void)
{
    /* All STRESS_N files have identical content → one group of STRESS_N */
    char path[64];
    FILEDEDUP fd = FDInit();
    ASSERT(fd != NULL, "FDInit failed");

    for (int i = 0; i < STRESS_N; i++) {
        snprintf(path, sizeof(path), "./fd_stress_d%d.txt", i);
        write_file(path, "same content for everyone");
        FDCheck(fd, path);
    }

    int len = 0;
    char **dump = FDDump(fd, &len);
    ASSERT(dump != NULL, "stress all-dup: dump should not be NULL");
    ASSERT(count_paths(dump, len)  == STRESS_N,
           "stress all-dup: all paths should appear");
    ASSERT(count_groups(dump, len) == 1,
           "stress all-dup: should be exactly 1 group");
    free_dump(dump, len);

    for (int i = 0; i < STRESS_N; i++) {
        snprintf(path, sizeof(path), "./fd_stress_d%d.txt", i);
        unlink(path);
    }
    PASS();
}

/* --- FDDump idempotency: calling twice gives consistent results ---- */

static void test_dump_idempotent(void)
{
    const char *a = "./fd_idem_a.txt";
    const char *b = "./fd_idem_b.txt";
    write_file(a, "idempotent test");
    write_file(b, "idempotent test");

    FILEDEDUP fd = FDInit();
    FDCheck(fd, (char *)a);
    FDCheck(fd, (char *)b);

    int len1 = 0, len2 = 0;
    char **d1 = FDDump(fd, &len1);
    char **d2 = FDDump(fd, &len2);

    ASSERT(len1 == len2, "two FDDump calls should return the same length");
    ASSERT(count_groups(d1, len1) == count_groups(d2, len2),
           "two FDDump calls should report the same number of groups");

    free_dump(d1, len1);
    free_dump(d2, len2);
    unlink(a); unlink(b);
    PASS();
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("\n=== FILEDEDUP test suite ===\n\n");

    printf("-- FDInit --\n");
    test_init_returns_nonnull();
    test_init_independent_instances();

    printf("\n-- FDCheck error handling --\n");
    test_check_null_fd();
    test_check_null_path();
    test_check_nonexistent_file();
    test_check_existing_file();

    printf("\n-- FDDump: no duplicates --\n");
    test_dump_empty_detector();
    test_dump_single_unique_file();
    test_dump_many_unique_files();

    printf("\n-- FDDump: duplicate detection --\n");
    test_dump_one_pair();
    test_dump_two_independent_pairs();
    test_dump_mixed_unique_and_duplicates();
    test_same_size_different_content();
    test_empty_files_are_duplicates();

    printf("\n-- Large files --\n");
    test_large_identical_files();
    test_large_different_files();

    printf("\n-- Stress tests --\n");
    test_stress_all_unique();
    test_stress_all_duplicates();

    printf("\n-- Misc --\n");
    test_dump_idempotent();

    printf("\n===========================\n");
    printf("Results: %d / %d passed\n\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
