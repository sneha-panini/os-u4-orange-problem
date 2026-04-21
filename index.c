// index.c — Staging area implementation

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// Forward declaration — defined in object.c
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                st.st_size  != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;

            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1;
                    break;
                }
            }
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

int index_load(Index *index) {
    index->count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0; // No index yet — empty, not an error

    char hex[HASH_HEX_SIZE + 1];
    unsigned int mode;
    unsigned long long mtime, size;
    char path[512];

    while (fscanf(f, "%o %64s %llu %llu %511s\n",
                  &mode, hex, &mtime, &size, path) == 5) {
        if (index->count >= MAX_INDEX_ENTRIES) break;
        IndexEntry *e = &index->entries[index->count];
        e->mode      = (uint32_t)mode;
        e->mtime_sec = (uint64_t)mtime;
        e->size      = (uint32_t)size;
        snprintf(e->path, sizeof(e->path), "%s", path);
        if (hex_to_hash(hex, &e->hash) != 0) { fclose(f); return -1; }
        index->count++;
    }

    fclose(f);
    return 0;
}

static int compare_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

int index_save(const Index *index) {
    // Sort entries by path — sort directly on a heap copy to avoid stack overflow
    // (Index struct is ~5.4MB, too large for stack)
    IndexEntry *sorted = malloc(index->count * sizeof(IndexEntry));
    if (!sorted && index->count > 0) return -1;
    if (index->count > 0)
        memcpy(sorted, index->entries, index->count * sizeof(IndexEntry));
    qsort(sorted, index->count, sizeof(IndexEntry), compare_entries);

    char tmp_path[] = INDEX_FILE ".tmp";
    FILE *f = fopen(tmp_path, "w");
    if (!f) { free(sorted); return -1; }

    char hex[HASH_HEX_SIZE + 1];
    for (int i = 0; i < index->count; i++) {
        hash_to_hex(&sorted[i].hash, hex);
        fprintf(f, "%o %s %llu %llu %s\n",
                sorted[i].mode, hex,
                (unsigned long long)sorted[i].mtime_sec,
                (unsigned long long)sorted[i].size,
                sorted[i].path);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);
    free(sorted);

    return rename(tmp_path, INDEX_FILE);
}

int index_add(Index *index, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    size_t sz = (file_size > 0) ? (size_t)file_size : 0;
    void *contents = malloc(sz > 0 ? sz : 1);
    if (!contents) { fclose(f); return -1; }

    size_t bytes_read = sz > 0 ? fread(contents, 1, sz, f) : 0;
    fclose(f);

    // Write blob to object store
    ObjectID blob_id;
    if (object_write(OBJ_BLOB, contents, bytes_read, &blob_id) != 0) {
        free(contents); return -1;
    }
    free(contents);

    // Get file metadata
    struct stat st;
    if (lstat(path, &st) != 0) return -1;

    uint32_t mode;
    if (S_ISDIR(st.st_mode))       mode = 0040000;
    else if (st.st_mode & S_IXUSR) mode = 0100755;
    else                            mode = 0100644;

    // Update existing entry or insert new one
    IndexEntry *existing = index_find(index, path);
    if (existing) {
        existing->hash     = blob_id;
        existing->mode     = mode;
        existing->mtime_sec = (uint64_t)st.st_mtime;
        existing->size     = (uint32_t)st.st_size;
    } else {
        if (index->count >= MAX_INDEX_ENTRIES) {
            fprintf(stderr, "error: index full\n"); return -1;
        }
        IndexEntry *e = &index->entries[index->count++];
        e->hash      = blob_id;
        e->mode      = mode;
        e->mtime_sec = (uint64_t)st.st_mtime;
        e->size      = (uint32_t)st.st_size;
        snprintf(e->path, sizeof(e->path), "%s", path);
    }

    return index_save(index);
}
