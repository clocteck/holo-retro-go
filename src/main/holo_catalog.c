#include "holo_catalog.h"

#include <stdlib.h>
#include <string.h>

#ifndef HOLO_CATALOG_PATH_MAX
#define HOLO_CATALOG_PATH_MAX 255
#endif

typedef struct holo_catalog_entry_t {
    char path[HOLO_CATALOG_PATH_MAX + 1];
    char dirname[HOLO_CATALOG_PATH_MAX + 1];
    char basename[96];
    size_t size;
    time_t mtime;
    uint8_t is_dir;
} holo_catalog_entry_t;

static holo_catalog_entry_t *s_entries;
static size_t s_count;
static size_t s_capacity;
static size_t s_dirs;
static size_t s_files;

static size_t text_len(const char *text)
{
    return text ? strlen(text) : 0;
}

static void copy_text(char *dst, size_t dst_size, const char *src, size_t src_len)
{
    size_t n;
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    n = src_len;
    if (n + 1 > dst_size) {
        n = dst_size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int same_path(const char *a, const char *b)
{
    size_t alen = text_len(a);
    size_t blen = text_len(b);

    while (alen > 1 && a[alen - 1] == '/') {
        --alen;
    }
    while (blen > 1 && b[blen - 1] == '/') {
        --blen;
    }
    return alen == blen && strncmp(a, b, alen) == 0;
}

static int is_direct_child(const char *dir, const char *path)
{
    size_t dir_len = text_len(dir);
    const char *rest;

    while (dir_len > 1 && dir[dir_len - 1] == '/') {
        --dir_len;
    }

    if (strncmp(dir, path, dir_len) != 0) {
        return 0;
    }
    if (path[dir_len] != '/') {
        return 0;
    }
    rest = path + dir_len + 1;
    return rest[0] != '\0' && strchr(rest, '/') == NULL;
}

static int is_descendant(const char *dir, const char *path)
{
    size_t dir_len = text_len(dir);

    while (dir_len > 1 && dir[dir_len - 1] == '/') {
        --dir_len;
    }

    return strncmp(dir, path, dir_len) == 0 && path[dir_len] == '/' && path[dir_len + 1] != '\0';
}

static void split_path(holo_catalog_entry_t *entry)
{
    const char *slash;
    if (!entry) {
        return;
    }
    slash = strrchr(entry->path, '/');
    if (!slash) {
        entry->dirname[0] = '\0';
        copy_text(entry->basename, sizeof(entry->basename), entry->path, text_len(entry->path));
        return;
    }
    copy_text(entry->dirname, sizeof(entry->dirname), entry->path, (size_t)(slash - entry->path));
    copy_text(entry->basename, sizeof(entry->basename), slash + 1, text_len(slash + 1));
    if (entry->dirname[0] == '\0') {
        copy_text(entry->dirname, sizeof(entry->dirname), "/", 1);
    }
}

static const char *entry_extension(const holo_catalog_entry_t *entry)
{
    const char *dot;
    if (!entry || entry->is_dir) {
        return "";
    }
    dot = strrchr(entry->basename, '.');
    return dot ? dot + 1 : "";
}

static const char *path_basename_ptr(const char *path)
{
    const char *slash = path ? strrchr(path, '/') : NULL;
    return slash ? slash + 1 : path;
}

static int reserve_entries(size_t needed)
{
    holo_catalog_entry_t *next;
    size_t next_capacity = s_capacity ? s_capacity : 64;
    size_t next_bytes;

    while (next_capacity < needed) {
        if (next_capacity > ((size_t)-1) / 2u) {
            return 0;
        }
        next_capacity *= 2;
    }
    if (next_capacity == s_capacity) {
        return 1;
    }
    if (next_capacity > ((size_t)-1) / sizeof(*s_entries)) {
        return 0;
    }
    next_bytes = next_capacity * sizeof(*s_entries);
    next = (holo_catalog_entry_t *)malloc(next_bytes);
    if (!next) {
        return 0;
    }
    if (s_entries && s_count) {
        memcpy(next, s_entries, s_count * sizeof(*s_entries));
    }
    free(s_entries);
    s_entries = next;
    s_capacity = next_capacity;
    return 1;
}

static uint64_t parse_uint64(const char *text, size_t len)
{
    uint64_t value = 0;
    size_t i;
    for (i = 0; i < len; ++i) {
        if (text[i] < '0' || text[i] > '9') {
            break;
        }
        value = value * 10u + (uint64_t)(text[i] - '0');
    }
    return value;
}

static int append_line(const char *line, size_t len)
{
    const char *fields[4] = {0};
    size_t field_lens[4] = {0};
    size_t field_count = 0;
    size_t start = 0;
    size_t i;
    holo_catalog_entry_t *entry;

    if (len < 3 || (line[0] != 'F' && line[0] != 'D') || line[1] != '\t') {
        return 1;
    }

    fields[field_count] = line + 2;
    start = 2;
    for (i = 2; i <= len && field_count < 4; ++i) {
        if (i == len || line[i] == '\t') {
            field_lens[field_count] = i - start;
            ++field_count;
            if (i + 1 < len) {
                fields[field_count] = line + i + 1;
                start = i + 1;
            }
        }
    }

    if (field_count < 1 || field_lens[0] == 0) {
        return 1;
    }
    if (!reserve_entries(s_count + 1)) {
        return 0;
    }

    entry = &s_entries[s_count++];
    memset(entry, 0, sizeof(*entry));
    entry->is_dir = (line[0] == 'D');
    copy_text(entry->path, sizeof(entry->path), fields[0], field_lens[0]);
    if (field_count > 1) {
        entry->size = (size_t)parse_uint64(fields[1], field_lens[1]);
    }
    if (field_count > 2) {
        entry->mtime = (time_t)parse_uint64(fields[2], field_lens[2]);
    }
    split_path(entry);

    if (entry->is_dir) {
        ++s_dirs;
    } else {
        ++s_files;
    }
    return 1;
}

void holo_catalog_clear(void)
{
    free(s_entries);
    s_entries = NULL;
    s_count = 0;
    s_capacity = 0;
    s_dirs = 0;
    s_files = 0;
}

int holo_catalog_load_blob(const char *blob, size_t len)
{
    size_t start = 0;
    size_t i;

    holo_catalog_clear();
    if (!blob || len == 0) {
        return 1;
    }

    for (i = 0; i <= len; ++i) {
        if (i == len || blob[i] == '\n') {
            size_t line_len = i - start;
            if (line_len && blob[start + line_len - 1] == '\r') {
                --line_len;
            }
            if (!append_line(blob + start, line_len)) {
                holo_catalog_clear();
                return 0;
            }
            start = i + 1;
        }
    }
    return 1;
}

int holo_catalog_ready(void)
{
    return s_count > 0;
}

void holo_catalog_info(size_t *entries, size_t *dirs, size_t *files)
{
    if (entries) {
        *entries = s_count;
    }
    if (dirs) {
        *dirs = s_dirs;
    }
    if (files) {
        *files = s_files;
    }
}

int holo_catalog_stat(const char *path, rg_stat_t *out)
{
    size_t i;
    if (!path || !out) {
        return 0;
    }
    for (i = 0; i < s_count; ++i) {
        const holo_catalog_entry_t *entry = &s_entries[i];
        if (same_path(entry->path, path)) {
            memset(out, 0, sizeof(*out));
            out->basename = entry->basename;
            out->extension = entry_extension(entry);
            out->size = entry->size;
            out->mtime = entry->mtime;
            out->is_dir = entry->is_dir != 0;
            out->is_file = entry->is_dir == 0;
            out->exists = true;
            return 1;
        }
    }
    return 0;
}

int holo_catalog_exists(const char *path)
{
    rg_stat_t st;
    return holo_catalog_stat(path, &st);
}

bool holo_catalog_scandir(const char *path, rg_scandir_cb_t *callback, void *arg, uint32_t flags)
{
    size_t i;
    uint32_t types = flags & (RG_SCANDIR_FILES | RG_SCANDIR_DIRS);
    int recursive = (flags & RG_SCANDIR_RECURSIVE) != 0;
    rg_scandir_t result;

    if (!path || !callback) {
        return false;
    }
    if (!s_count) {
        return false;
    }
    if (!types) {
        types = RG_SCANDIR_FILES | RG_SCANDIR_DIRS;
    }

    for (i = 0; i < s_count; ++i) {
        const holo_catalog_entry_t *entry = &s_entries[i];
        int include_type = entry->is_dir ? (types & RG_SCANDIR_DIRS) : (types & RG_SCANDIR_FILES);
        int include_path = recursive ? is_descendant(path, entry->path) : is_direct_child(path, entry->path);
        int ret;

        if (!include_type || !include_path) {
            continue;
        }

        memset(&result, 0, sizeof(result));
        copy_text(result.path, sizeof(result.path), entry->path, text_len(entry->path));
        result.basename = path_basename_ptr(result.path);
        result.dirname = path;
        result.size = entry->size;
        result.mtime = entry->mtime;
        result.is_dir = entry->is_dir != 0;
        result.is_file = entry->is_dir == 0;

        ret = callback(&result, arg);
        if (ret == RG_SCANDIR_STOP) {
            break;
        }
    }

    return true;
}
