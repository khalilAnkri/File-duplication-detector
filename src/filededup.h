/**
 * filededup.h — File Deduplication ADT
 *
 * Identifies sets of files with identical byte-for-byte content.
 * Uses tabulation hashing + size pre-filtering to avoid O(n^2) comparisons.
 *
 * Public API
 * ----------
 *   FDInit()   – allocate and initialise a new detector instance
 *   FDCheck()  – register a file path; returns 1 on success, 0 on error
 *   FDDump()   – return all duplicate groups as a NULL-separated array
 */

#ifndef FILEDEDUP_H
#define FILEDEDUP_H

/* Opaque handle to a FILEDEDUP instance */
typedef struct filededup_s *FILEDEDUP;

/**
 * FDInit – create a new, empty FILEDEDUP instance.
 *
 * Returns a freshly allocated FILEDEDUP handle, or NULL if memory
 * allocation fails.
 */
FILEDEDUP FDInit(void);

/**
 * FDCheck – register the file at `filepath` with the detector.
 *
 * The file's size and tabulation hash are computed; the path is then
 * stored in a bucket keyed by (size, hash).  If the file does not
 * exist, cannot be opened, or any allocation fails, 0 is returned and
 * the detector state is left unchanged.
 *
 * Returns 1 on success, 0 on error.
 */
int FDCheck(FILEDEDUP fd,  char *filepath);

/**
 * FDDump – retrieve all groups of duplicate files.
 *
 * Returns a heap-allocated array of C strings.  Files within the same
 * duplicate group appear consecutively and are followed by a NULL
 * sentinel.  Groups are therefore separated — and terminated — by NULL
 * pointers.  The total element count (strings + sentinels) is written
 * through `length`.
 *
 * Only groups that contain at least two files are included; unique
 * files are omitted.
 *
 * The caller is responsible for freeing every non-NULL string in the
 * returned array and then the array itself.
 *
 * Returns NULL (and sets *length to 0) if there are no duplicates or
 * on allocation failure.
 */
char **FDDump(FILEDEDUP fd, int *length);


/**
 * FDDestroy – release all resources held by the detector.
 *
 * Frees the hash table, all bucket structures, and all duplicated
 * file paths. Safely handles a NULL fd pointer.
 */
void FDDestroy(FILEDEDUP fd);

#endif /* FILEDEDUP_H */
