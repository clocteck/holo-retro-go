#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/reent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "../include/module_abi.h"
#include "holo_catalog.h"
#include "holo_port.h"

#define MALLOC_CAP_EXEC (1u << 0)
#define MALLOC_CAP_32BIT (1u << 1)
#define MALLOC_CAP_8BIT (1u << 2)
#define MALLOC_CAP_DMA (1u << 3)
#define MALLOC_CAP_SPIRAM (1u << 10)
#define MALLOC_CAP_INTERNAL (1u << 11)

typedef struct multi_heap_info_t {
    size_t total_free_bytes;
    size_t total_allocated_bytes;
    size_t largest_free_block;
    size_t minimum_free_bytes;
    size_t allocated_blocks;
    size_t free_blocks;
    size_t total_blocks;
} multi_heap_info_t;

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
typedef void *QueueHandle_t;
typedef uint32_t TickType_t;
typedef int32_t BaseType_t;
typedef uint32_t UBaseType_t;
typedef int esp_reset_reason_t;

#define ESP_RST_UNKNOWN 0
#define pdPASS 1
#define pdFAIL 0
#define pdTRUE 1
#define pdFALSE 0

void *heap_caps_malloc(size_t size, uint32_t caps);
void heap_caps_free(void *ptr);
QueueHandle_t xQueueCreateMutex(uint8_t queue_type);
void vTaskDelay(const TickType_t ticks);

static int s_errno_value;
static struct _reent s_reent;
char _ctype_[257];

typedef struct holo_file_t {
    uint32_t magic;
    void *file;
    int eof;
    int error;
} holo_file_t;

#define HOLO_FILE_MAGIC 0x48464C45u
#define HOLO_MAX_OPEN_FILES 16u
#define HOLO_QUEUE_MAGIC 0x48514C51u
#define HOLO_QUEUE_WAIT_FOREVER 0xffffffffu

static holo_file_t *s_open_files[HOLO_MAX_OPEN_FILES];

typedef uint32_t holo_mem_word_t __attribute__((__may_alias__));

static void holo_memcpy_forward(unsigned char *d, const unsigned char *s, size_t n)
{
    if (n >= 16 && ((((uintptr_t)d) ^ ((uintptr_t)s)) & (sizeof(holo_mem_word_t) - 1u)) == 0) {
        while (n && (((uintptr_t)d) & (sizeof(holo_mem_word_t) - 1u))) {
            *d++ = *s++;
            --n;
        }

        holo_mem_word_t *dw = (holo_mem_word_t *)d;
        const holo_mem_word_t *sw = (const holo_mem_word_t *)s;

        while (n >= 16) {
            dw[0] = sw[0];
            dw[1] = sw[1];
            dw[2] = sw[2];
            dw[3] = sw[3];
            dw += 4;
            sw += 4;
            n -= 16;
        }
        while (n >= sizeof(holo_mem_word_t)) {
            *dw++ = *sw++;
            n -= sizeof(holo_mem_word_t);
        }

        d = (unsigned char *)dw;
        s = (const unsigned char *)sw;
    }

    while (n--) {
        *d++ = *s++;
    }
}

static void holo_memmove_backward(unsigned char *d, const unsigned char *s, size_t n)
{
    d += n;
    s += n;

    if (n >= 16 && ((((uintptr_t)d) ^ ((uintptr_t)s)) & (sizeof(holo_mem_word_t) - 1u)) == 0) {
        while (n && (((uintptr_t)d) & (sizeof(holo_mem_word_t) - 1u))) {
            *--d = *--s;
            --n;
        }

        holo_mem_word_t *dw = (holo_mem_word_t *)d;
        const holo_mem_word_t *sw = (const holo_mem_word_t *)s;

        while (n >= 16) {
            dw -= 4;
            sw -= 4;
            dw[3] = sw[3];
            dw[2] = sw[2];
            dw[1] = sw[1];
            dw[0] = sw[0];
            n -= 16;
        }
        while (n >= sizeof(holo_mem_word_t)) {
            *--dw = *--sw;
            n -= sizeof(holo_mem_word_t);
        }

        d = (unsigned char *)dw;
        s = (const unsigned char *)sw;
    }

    while (n--) {
        *--d = *--s;
    }
}

static int host_heap_usable(const module_host_api_v1 *host)
{
    const size_t need_heap = offsetof(module_host_api_v1, heap) + sizeof(module_heap_api_t);
    const uint32_t module_major = MODULE_ABI_VERSION & 0xFFFF0000u;

    if (!host) {
        return 0;
    }
    if ((host->abi_version & 0xFFFF0000u) != module_major || host->abi_version < 0x00020002u) {
        return 0;
    }
    if (host->size < need_heap || host->heap.size < sizeof(module_heap_api_t)) {
        return 0;
    }
    return host->heap.malloc && host->heap.calloc && host->heap.realloc && host->heap.free;
}

static int plausible_heap_ptr(const void *ptr)
{
    uintptr_t p = (uintptr_t)ptr;
    return p == 0 || (p >= 0x3C000000u && p < 0x60000000u);
}

static uint64_t holo_u64_divmod(uint64_t num, uint64_t den, uint64_t *rem)
{
    uint64_t quo = 0;

    if (den == 0) {
        if (rem) {
            *rem = num;
        }
        return 0;
    }

    for (int bit = 63; bit >= 0; --bit) {
        if ((num >> bit) >= den) {
            num -= den << bit;
            quo |= 1ULL << bit;
        }
    }

    if (rem) {
        *rem = num;
    }
    return quo;
}

static uint64_t holo_i64_abs_u64(int64_t value)
{
    uint64_t bits = (uint64_t)value;
    return value < 0 ? (~bits + 1ULL) : bits;
}

uint64_t __udivdi3(uint64_t num, uint64_t den) __attribute__((used, noinline));
uint64_t __umoddi3(uint64_t num, uint64_t den) __attribute__((used, noinline));
int64_t __divdi3(int64_t num, int64_t den) __attribute__((used, noinline));
int64_t __moddi3(int64_t num, int64_t den) __attribute__((used, noinline));

uint64_t __udivdi3(uint64_t num, uint64_t den)
{
    return holo_u64_divmod(num, den, NULL);
}

uint64_t __umoddi3(uint64_t num, uint64_t den)
{
    uint64_t rem;
    (void)holo_u64_divmod(num, den, &rem);
    return rem;
}

int64_t __divdi3(int64_t num, int64_t den)
{
    const int negative = (num < 0) ^ (den < 0);
    uint64_t quo = holo_u64_divmod(holo_i64_abs_u64(num), holo_i64_abs_u64(den), NULL);
    if (negative) {
        quo = ~quo + 1ULL;
    }
    return (int64_t)quo;
}

int64_t __moddi3(int64_t num, int64_t den)
{
    uint64_t rem;
    (void)holo_u64_divmod(holo_i64_abs_u64(num), holo_i64_abs_u64(den), &rem);
    if (num < 0) {
        rem = ~rem + 1ULL;
    }
    return (int64_t)rem;
}

typedef struct holo_queue_t {
    uint32_t magic;
    UBaseType_t length;
    UBaseType_t item_size;
    UBaseType_t head;
    UBaseType_t tail;
    UBaseType_t count;
    uint8_t *storage;
} holo_queue_t;

int *__errno(void)
{
    return &s_errno_value;
}

struct _reent *__getreent(void)
{
    return &s_reent;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    if (n != 0 && dst != src) {
        holo_memcpy_forward((unsigned char *)dst, (const unsigned char *)src, n);
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) {
        return dst;
    }
    if (d < s) {
        holo_memcpy_forward(d, s, n);
    } else {
        holo_memmove_backward(d, s, n);
    }
    return dst;
}

void *memset(void *dst, int value, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char byte = (unsigned char)value;
    const holo_mem_word_t word = (holo_mem_word_t)byte * 0x01010101u;

    if (n >= 16) {
        while (n && (((uintptr_t)d) & (sizeof(holo_mem_word_t) - 1u))) {
            *d++ = byte;
            --n;
        }

        holo_mem_word_t *dw = (holo_mem_word_t *)d;

        while (n >= 16) {
            dw[0] = word;
            dw[1] = word;
            dw[2] = word;
            dw[3] = word;
            dw += 4;
            n -= 16;
        }
        while (n >= sizeof(holo_mem_word_t)) {
            *dw++ = word;
            n -= sizeof(holo_mem_word_t);
        }

        d = (unsigned char *)dw;
    }

    while (n--) {
        *d++ = byte;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    while (n--) {
        if (*pa != *pb) {
            return (int)*pa - (int)*pb;
        }
        ++pa;
        ++pb;
    }
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    if (!s) {
        return 0;
    }
    while (*p) {
        ++p;
    }
    return (size_t)(p - s);
}

char *strcpy(char *dst, const char *src)
{
    char *out = dst;
    while ((*dst++ = *src++) != '\0') {
    }
    return out;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; ++i) {
        dst[i] = src[i];
    }
    for (; i < n; ++i) {
        dst[i] = '\0';
    }
    return dst;
}

char *strcat(char *dst, const char *src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    if (n == 0) {
        return 0;
    }
    while (--n && *a && *a == *b) {
        ++a;
        ++b;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int ascii_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? (c + ('a' - 'A')) : c;
}

int strcasecmp(const char *a, const char *b)
{
    int ca;
    int cb;
    do {
        ca = ascii_lower((unsigned char)*a++);
        cb = ascii_lower((unsigned char)*b++);
    } while (ca && ca == cb);
    return ca - cb;
}

char *strchr(const char *s, int c)
{
    char ch = (char)c;
    while (*s) {
        if (*s == ch) {
            return (char *)s;
        }
        ++s;
    }
    return ch == '\0' ? (char *)s : NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    char ch = (char)c;
    do {
        if (*s == ch) {
            last = s;
        }
    } while (*s++);
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    size_t needle_len = strlen(needle);
    if (needle_len == 0) {
        return (char *)haystack;
    }
    for (; *haystack; ++haystack) {
        if (*haystack == *needle && memcmp(haystack, needle, needle_len) == 0) {
            return (char *)haystack;
        }
    }
    return NULL;
}

long strtol(const char *nptr, char **endptr, int base)
{
    const char *p = nptr;
    long sign = 1;
    long value = 0;
    if (base == 0) {
        base = 10;
    }
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        ++p;
    }
    if (*p == '-' || *p == '+') {
        sign = (*p == '-') ? -1 : 1;
        ++p;
    }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    }
    while (*p) {
        int digit;
        if (*p >= '0' && *p <= '9') {
            digit = *p - '0';
        } else if (*p >= 'a' && *p <= 'z') {
            digit = *p - 'a' + 10;
        } else if (*p >= 'A' && *p <= 'Z') {
            digit = *p - 'A' + 10;
        } else {
            break;
        }
        if (digit >= base) {
            break;
        }
        value = value * base + digit;
        ++p;
    }
    if (endptr) {
        *endptr = (char *)p;
    }
    return value * sign;
}

char *strdup(const char *s)
{
    size_t len = strlen(s) + 1;
    char *copy = (char *)malloc(len);
    if (copy) {
        memcpy(copy, s, len);
    }
    return copy;
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    if (a == b || n == 0) {
        return 0;
    }
    if (!a) {
        return -1;
    }
    if (!b) {
        return 1;
    }
    for (size_t i = 0; i < n; ++i) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = (unsigned char)(ca + ('a' - 'A'));
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (unsigned char)(cb + ('a' - 'A'));
        }
        if (ca != cb || ca == 0 || cb == 0) {
            return (int)ca - (int)cb;
        }
    }
    return 0;
}

void *malloc(size_t size)
{
    const module_host_api_v1 *host = holo_port_host();
    void *ptr = NULL;
    if (size == 0) {
        size = 1;
    }
    if (host_heap_usable(host)) {
        ptr = host->heap.malloc(size, MODULE_HEAP_PSRAM | MODULE_HEAP_8BIT);
        if (!ptr) {
            ptr = host->heap.malloc(size, MODULE_HEAP_DEFAULT);
        }
        if (plausible_heap_ptr(ptr)) {
            return ptr;
        }
    }
    ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return ptr;
}

void *calloc(size_t n, size_t size)
{
    const module_host_api_v1 *host = holo_port_host();
    if (host_heap_usable(host)) {
        void *ptr = host->heap.calloc(n, size, MODULE_HEAP_PSRAM | MODULE_HEAP_8BIT);
        if (!ptr) {
            ptr = host->heap.calloc(n, size, MODULE_HEAP_DEFAULT);
        }
        if (plausible_heap_ptr(ptr)) {
            return ptr;
        }
    }
    if (size && n > ((size_t)-1) / size) {
        return NULL;
    }
    size_t total = n * size;
    void *ptr = malloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void *realloc(void *ptr, size_t size)
{
    const module_host_api_v1 *host = holo_port_host();
    if (!ptr) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    if (host_heap_usable(host)) {
        void *next = host->heap.realloc(ptr, size, MODULE_HEAP_PSRAM | MODULE_HEAP_8BIT);
        if (!next) {
            next = host->heap.realloc(ptr, size, MODULE_HEAP_DEFAULT);
        }
        return plausible_heap_ptr(next) ? next : NULL;
    }
    return NULL;
}

void free(void *ptr)
{
    const module_host_api_v1 *host = holo_port_host();
    if (!ptr) {
        return;
    }
    if (host_heap_usable(host)) {
        host->heap.free(ptr);
        return;
    }
    heap_caps_free(ptr);
}

void heap_caps_malloc_extmem_enable(size_t limit)
{
    (void)limit;
}

size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    (void)caps;
    return 0;
}

void heap_caps_get_info(multi_heap_info_t *info, uint32_t caps)
{
    const module_host_api_v1 *host = holo_port_host();
    if (!info) {
        return;
    }
    memset(info, 0, sizeof(*info));
    if (host && host->heap.free_size) {
        uint32_t module_caps = (caps & MALLOC_CAP_INTERNAL) ? MODULE_HEAP_INTERNAL : MODULE_HEAP_DEFAULT;
        info->total_free_bytes = host->heap.free_size(module_caps);
    }
}

int access(const char *path, int mode)
{
    struct stat st;
    (void)mode;
    return stat(path, &st);
}

int stat(const char *path, struct stat *st)
{
    const module_host_api_v1 *host = holo_port_host();
    rg_stat_t rgst;
    if (!path || !st) {
        s_errno_value = 22;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    if (holo_catalog_stat(path, &rgst)) {
        st->st_size = (off_t)rgst.size;
        st->st_mtime = rgst.mtime;
        st->st_mode = rgst.is_dir ? (S_IFDIR | 0777) : (S_IFREG | 0666);
        return 0;
    }
    if (host && host->sd.exists && host->sd.exists(path)) {
        st->st_mode = S_IFREG | 0666;
        return 0;
    }
    s_errno_value = 2;
    return -1;
}

int mkdir(const char *path, mode_t mode)
{
    const module_host_api_v1 *host = holo_port_host();
    (void)mode;
    if (host && host->sd.mkdir) {
        return host->sd.mkdir(path) == MODULE_OK ? 0 : -1;
    }
    return -1;
}

int remove(const char *path)
{
    const module_host_api_v1 *host = holo_port_host();
    if (host && host->sd.remove) {
        return host->sd.remove(path) == MODULE_OK ? 0 : -1;
    }
    return -1;
}

int rename(const char *from, const char *to)
{
    const module_host_api_v1 *host = holo_port_host();
    if (host && host->sd.rename) {
        return host->sd.rename(from, to) == MODULE_OK ? 0 : -1;
    }
    return -1;
}

int rmdir(const char *path)
{
    return remove(path);
}

static holo_file_t *lookup_file(FILE *stream)
{
    if (!stream) {
        return NULL;
    }
    for (size_t i = 0; i < HOLO_MAX_OPEN_FILES; ++i) {
        if ((FILE *)s_open_files[i] == stream && s_open_files[i]->magic == HOLO_FILE_MAGIC) {
            return s_open_files[i];
        }
    }
    return NULL;
}

static int register_file(holo_file_t *file)
{
    for (size_t i = 0; i < HOLO_MAX_OPEN_FILES; ++i) {
        if (!s_open_files[i]) {
            s_open_files[i] = file;
            return 1;
        }
    }
    return 0;
}

static void unregister_file(holo_file_t *file)
{
    for (size_t i = 0; i < HOLO_MAX_OPEN_FILES; ++i) {
        if (s_open_files[i] == file) {
            s_open_files[i] = NULL;
            return;
        }
    }
}

static uint32_t mode_from_string(const char *mode)
{
    uint32_t flags = 0;
    if (!mode || !mode[0]) {
        return MODULE_FILE_READ;
    }
    if (mode[0] == 'r') {
        flags |= MODULE_FILE_READ;
    } else if (mode[0] == 'w') {
        flags |= MODULE_FILE_WRITE | MODULE_FILE_CREATE | MODULE_FILE_TRUNC;
    } else if (mode[0] == 'a') {
        flags |= MODULE_FILE_WRITE | MODULE_FILE_CREATE | MODULE_FILE_APPEND;
    }
    if (strchr(mode, '+')) {
        flags |= MODULE_FILE_READ | MODULE_FILE_WRITE;
    }
    return flags ? flags : MODULE_FILE_READ;
}

FILE *fopen(const char *path, const char *mode)
{
    const module_host_api_v1 *host = holo_port_host();
    void *host_file = NULL;
    holo_file_t *file;

    if (!host || !host->sd.open || !host->file.close || !path) {
        s_errno_value = 2;
        return NULL;
    }
    if (host->sd.open(path, mode_from_string(mode), &host_file) != MODULE_OK || !host_file) {
        s_errno_value = 2;
        return NULL;
    }

    file = (holo_file_t *)calloc(1, sizeof(*file));
    if (!file) {
        host->file.close(host_file);
        s_errno_value = 12;
        return NULL;
    }
    file->magic = HOLO_FILE_MAGIC;
    file->file = host_file;
    if (!register_file(file)) {
        host->file.close(host_file);
        free(file);
        s_errno_value = 24;
        return NULL;
    }
    return (FILE *)file;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    const module_host_api_v1 *host = holo_port_host();
    holo_file_t *file = lookup_file(stream);
    size_t total;
    size_t out_read = 0;

    if (!ptr || size == 0 || nmemb == 0) {
        return 0;
    }
    total = size * nmemb;
    if (!host || !host->file.read || !file || !file->file) {
        return 0;
    }
    if (host->file.read(file->file, ptr, total, &out_read) != MODULE_OK) {
        file->error = 1;
        return 0;
    }
    if (out_read < total) {
        file->eof = 1;
    }
    return out_read / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    const module_host_api_v1 *host = holo_port_host();
    holo_file_t *file = lookup_file(stream);
    size_t total;
    size_t out_written = 0;

    if (!ptr || size == 0 || nmemb == 0) {
        return 0;
    }
    total = size * nmemb;
    if (host && file && file->file && host->file.write) {
        if (host->file.write(file->file, ptr, total, &out_written) != MODULE_OK) {
            file->error = 1;
            return 0;
        }
        return out_written / size;
    }

    if (host && host->serial.write) {
        int32_t written = host->serial.write(ptr, total);
        return written > 0 ? (size_t)written / size : 0;
    }
    if (host && host->serial.print) {
        char tmp[257];
        size_t n = total < 256u ? total : 256u;
        memcpy(tmp, ptr, n);
        tmp[n] = '\0';
        host->serial.print(tmp);
        return n / size;
    }
    return 0;
}

int fclose(FILE *stream)
{
    const module_host_api_v1 *host = holo_port_host();
    holo_file_t *file = lookup_file(stream);
    int ret = 0;

    if (!file) {
        return 0;
    }
    unregister_file(file);
    if (host && host->file.close && file->file) {
        ret = host->file.close(file->file) == MODULE_OK ? 0 : -1;
    }
    file->magic = 0;
    free(file);
    return ret;
}

int fseek(FILE *stream, long offset, int whence)
{
    const module_host_api_v1 *host = holo_port_host();
    holo_file_t *file = lookup_file(stream);
    uint32_t mode = MODULE_SEEK_SET;

    if (!host || !host->file.seek || !file || !file->file) {
        return -1;
    }
    if (whence == SEEK_CUR) {
        mode = MODULE_SEEK_CUR;
    } else if (whence == SEEK_END) {
        mode = MODULE_SEEK_END;
    }
    file->eof = 0;
    return host->file.seek(file->file, offset, mode) == MODULE_OK ? 0 : -1;
}

long ftell(FILE *stream)
{
    const module_host_api_v1 *host = holo_port_host();
    holo_file_t *file = lookup_file(stream);
    uint64_t pos = 0;

    if (!host || !host->file.position || !file || !file->file) {
        return -1;
    }
    return host->file.position(file->file, &pos) == MODULE_OK ? (long)pos : -1;
}

int fflush(FILE *stream)
{
    const module_host_api_v1 *host = holo_port_host();
    holo_file_t *file = lookup_file(stream);

    if (host && file && file->file && host->file.flush) {
        return host->file.flush(file->file) == MODULE_OK ? 0 : -1;
    }
    if (host && host->serial.flush) {
        host->serial.flush();
    }
    return 0;
}

char *fgets(char *s, int size, FILE *stream)
{
    int i = 0;
    if (!s || size <= 0 || !stream) {
        return NULL;
    }
    while (i + 1 < size) {
        char ch;
        if (fread(&ch, 1, 1, stream) != 1) {
            break;
        }
        s[i++] = ch;
        if (ch == '\n') {
            break;
        }
    }
    if (i == 0) {
        return NULL;
    }
    s[i] = '\0';
    return s;
}

int feof(FILE *stream)
{
    holo_file_t *file = lookup_file(stream);
    return file ? file->eof : 0;
}

int fputs(const char *s, FILE *stream)
{
    size_t len = strlen(s);
    return fwrite(s, 1, len, stream) == len ? (int)len : -1;
}

int fputc(int c, FILE *stream)
{
    unsigned char ch = (unsigned char)c;
    return fwrite(&ch, 1, 1, stream) == 1 ? c : -1;
}

static int serial_vprintf(const char *fmt, va_list ap)
{
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    const module_host_api_v1 *host = holo_port_host();
    if (host && host->serial.print) {
        host->serial.print(buf);
    }
    return n;
}

int printf(const char *fmt, ...)
{
    int n;
    va_list ap;
    va_start(ap, fmt);
    n = serial_vprintf(fmt, ap);
    va_end(ap);
    return n;
}

int fprintf(FILE *stream, const char *fmt, ...)
{
    char buf[256];
    int n;
    va_list ap;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (stream) {
        fwrite(buf, 1, strlen(buf), stream);
    } else {
        const module_host_api_v1 *host = holo_port_host();
        if (host && host->serial.print) {
            host->serial.print(buf);
        }
    }
    return n;
}

int puts(const char *s)
{
    const module_host_api_v1 *host = holo_port_host();
    if (host && host->serial.println) {
        host->serial.println(s ? s : "");
    }
    return s ? (int)strlen(s) : 0;
}

static uint32_t s_rand_state = 1;

int rand(void)
{
    s_rand_state = s_rand_state * 1103515245u + 12345u;
    return (int)((s_rand_state >> 16) & 0x7fffu);
}

static void swap_bytes(unsigned char *a, unsigned char *b, size_t size)
{
    while (size--) {
        unsigned char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *))
{
    unsigned char *bytes = (unsigned char *)base;
    if (!base || !compar || size == 0) {
        return;
    }
    for (size_t i = 1; i < nmemb; ++i) {
        size_t j = i;
        while (j > 0 && compar(bytes + (j - 1) * size, bytes + j * size) > 0) {
            swap_bytes(bytes + (j - 1) * size, bytes + j * size, size);
            --j;
        }
    }
}

time_t time(time_t *out)
{
    const module_host_api_v1 *host = holo_port_host();
    time_t now = 0;
    if (host && host->time.millis) {
        now = (time_t)(host->time.millis() / 1000u);
    }
    if (out) {
        *out = now;
    }
    return now;
}

double difftime(time_t end, time_t beginning)
{
    return (double)(end - beginning);
}

struct tm *localtime(const time_t *timer)
{
    static struct tm tm_value;
    time_t t = timer ? *timer : 0;
    memset(&tm_value, 0, sizeof(tm_value));
    tm_value.tm_mday = 1;
    tm_value.tm_year = 70;
    tm_value.tm_sec = (int)(t % 60);
    tm_value.tm_min = (int)((t / 60) % 60);
    tm_value.tm_hour = (int)((t / 3600) % 24);
    return &tm_value;
}

char *asctime(const struct tm *tm)
{
    static char text[32];
    (void)tm;
    strcpy(text, "Thu Jan  1 00:00:00 1970\n");
    return text;
}

size_t strftime(char *s, size_t max, const char *format, const struct tm *tm)
{
    const char *text = "1970-01-01 00:00:00";
    size_t len = strlen(text);
    (void)format;
    (void)tm;
    if (!s || max == 0) {
        return 0;
    }
    if (len >= max) {
        return 0;
    }
    memcpy(s, text, len + 1);
    return len;
}

int settimeofday(const struct timeval *tv, const struct timezone *tz)
{
    (void)tv;
    (void)tz;
    return 0;
}

void tzset(void)
{
}

char *getenv(const char *name)
{
    (void)name;
    return NULL;
}

int setenv(const char *name, const char *value, int overwrite)
{
    (void)name;
    (void)value;
    (void)overwrite;
    return 0;
}

int64_t esp_timer_get_time(void)
{
    const module_host_api_v1 *host = holo_port_host();
    if (host && host->time.micros) {
        return (int64_t)host->time.micros();
    }
    return 0;
}

esp_reset_reason_t esp_reset_reason(void)
{
    return ESP_RST_UNKNOWN;
}

void esp_restart(void)
{
    holo_runtime_request_stop();
    for (;;) {
        vTaskDelay(1000);
    }
}

void *esp_partition_find_first(int type, int subtype, const char *label)
{
    (void)type;
    (void)subtype;
    (void)label;
    return NULL;
}

void vPortYield(void)
{
    const module_host_api_v1 *host = holo_port_host();
    if (host && host->task.yield) {
        host->task.yield();
    } else if (host && host->time.yield) {
        host->time.yield();
    }
}

void vTaskDelay(const TickType_t ticks)
{
    const module_host_api_v1 *host = holo_port_host();
    uint32_t ms = (uint32_t)ticks;
    if (host && host->task.delay) {
        host->task.delay(ms);
    } else if (host && host->time.delay) {
        host->time.delay(ms);
    }
}

void vTaskDelete(TaskHandle_t task)
{
    const module_host_api_v1 *host = holo_port_host();
    if (host && host->task.remove) {
        host->task.remove(task);
    }
}

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task)
{
    (void)task;
    return 8192;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return NULL;
}

static uint32_t task_stack_caps_for_name(const char *name)
{
    if (name && (strcmp(name, "rg_input") == 0 || strcmp(name, "rg_sysmon") == 0 ||
                 strcmp(name, "gwen_aout") == 0 ||
                 strcmp(name, "pce_sound") == 0)) {
        return MODULE_HEAP_PSRAM | MODULE_HEAP_8BIT;
    }
    return MODULE_HEAP_INTERNAL | MODULE_HEAP_8BIT;
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t entry,
                                   const char *name,
                                   const uint32_t stack_depth,
                                   void *arg,
                                   UBaseType_t priority,
                                   TaskHandle_t *out_task,
                                   const BaseType_t core_id)
{
    const module_host_api_v1 *host = holo_port_host();
    void *task = NULL;
    const char *task_name = name ? name : "retrogo";
    if (host && host->task.create_ex &&
        host->task.create_ex(task_name, entry, arg, stack_depth, priority, core_id,
                             task_stack_caps_for_name(task_name), &task) == MODULE_OK) {
        if (out_task) {
            *out_task = (TaskHandle_t)task;
        }
        return pdPASS;
    }
    if (host && host->task.create &&
        host->task.create(task_name, entry, arg, stack_depth, priority, core_id, &task) == MODULE_OK) {
        if (out_task) {
            *out_task = (TaskHandle_t)task;
        }
        return pdPASS;
    }
    if (out_task) {
        *out_task = NULL;
    }
    return pdFAIL;
}

static holo_queue_t *queue_from_handle(QueueHandle_t queue)
{
    holo_queue_t *q = (holo_queue_t *)queue;
    if (!q || q->magic != HOLO_QUEUE_MAGIC) {
        return NULL;
    }
    return q;
}

static BaseType_t queue_wait_for_count(holo_queue_t *queue, TickType_t ticks, int want_item)
{
    TickType_t waited = 0;
    while (queue) {
        int ready = want_item ? (queue->count > 0) : (queue->count < queue->length);
        if (ready) {
            return pdTRUE;
        }
        if (ticks == 0) {
            return pdFALSE;
        }
        vTaskDelay(1);
        if (ticks != HOLO_QUEUE_WAIT_FOREVER && ++waited >= ticks) {
            return pdFALSE;
        }
    }
    return pdFALSE;
}

static uint8_t *queue_slot(holo_queue_t *queue, UBaseType_t index)
{
    return queue->storage + ((size_t)index * (size_t)queue->item_size);
}

QueueHandle_t xQueueGenericCreate(const UBaseType_t length,
                                  const UBaseType_t item_size,
                                  const uint8_t queue_type)
{
    holo_queue_t *queue;
    (void)queue_type;

    if (length == 0) {
        return NULL;
    }
    if (item_size == 0) {
        return xQueueCreateMutex(queue_type);
    }

    if ((size_t)item_size > ((size_t)-1) / (size_t)length) {
        return NULL;
    }

    queue = (holo_queue_t *)calloc(1, sizeof(*queue));
    if (!queue) {
        return NULL;
    }

    queue->storage = (uint8_t *)malloc((size_t)length * (size_t)item_size);
    if (!queue->storage) {
        free(queue);
        return NULL;
    }

    queue->magic = HOLO_QUEUE_MAGIC;
    queue->length = length;
    queue->item_size = item_size;
    return (QueueHandle_t)queue;
}

QueueHandle_t xQueueCreateMutex(uint8_t queue_type)
{
    holo_queue_t *queue;
    (void)queue_type;

    queue = (holo_queue_t *)calloc(1, sizeof(*queue));
    if (!queue) {
        return NULL;
    }
    queue->magic = HOLO_QUEUE_MAGIC;
    queue->length = 1;
    queue->item_size = 0;
    return (QueueHandle_t)queue;
}

void vQueueDelete(QueueHandle_t queue)
{
    holo_queue_t *q = queue_from_handle(queue);
    if (!q) {
        return;
    }
    q->magic = 0;
    free(q->storage);
    free(q);
}

BaseType_t xQueueGenericSend(QueueHandle_t queue,
                             const void *item,
                             TickType_t ticks,
                             const BaseType_t copy_position)
{
    holo_queue_t *q = queue_from_handle(queue);
    if (!q) {
        return pdFAIL;
    }
    if (q->item_size == 0) {
        return pdPASS;
    }
    if (!item) {
        return pdFAIL;
    }

    if (!queue_wait_for_count(q, ticks, 0)) {
        if (copy_position != 2 || q->length == 0 || q->count == 0) {
            return pdFAIL;
        }
        memcpy(queue_slot(q, q->head), item, q->item_size);
        return pdPASS;
    }

    if (copy_position == 1) {
        q->head = (q->head == 0) ? (q->length - 1) : (q->head - 1);
        memcpy(queue_slot(q, q->head), item, q->item_size);
    } else {
        memcpy(queue_slot(q, q->tail), item, q->item_size);
        q->tail = (q->tail + 1) % q->length;
    }
    q->count++;
    return pdPASS;
}

BaseType_t xQueueSemaphoreTake(QueueHandle_t queue, TickType_t ticks)
{
    holo_queue_t *q = queue_from_handle(queue);
    (void)ticks;
    return q ? pdPASS : pdFAIL;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *buffer, TickType_t ticks)
{
    holo_queue_t *q = queue_from_handle(queue);
    if (!q) {
        return pdFALSE;
    }
    if (q->item_size == 0) {
        return pdPASS;
    }
    if (!buffer || !queue_wait_for_count(q, ticks, 1)) {
        return pdFALSE;
    }

    memcpy(buffer, queue_slot(q, q->head), q->item_size);
    q->head = (q->head + 1) % q->length;
    q->count--;
    return pdTRUE;
}

BaseType_t xQueuePeek(QueueHandle_t queue, void *buffer, TickType_t ticks)
{
    holo_queue_t *q = queue_from_handle(queue);
    if (!q) {
        return pdFALSE;
    }
    if (q->item_size == 0) {
        return pdPASS;
    }
    if (!buffer || !queue_wait_for_count(q, ticks, 1)) {
        return pdFALSE;
    }

    memcpy(buffer, queue_slot(q, q->head), q->item_size);
    return pdTRUE;
}

UBaseType_t uxQueueMessagesWaiting(const QueueHandle_t queue)
{
    holo_queue_t *q = queue_from_handle((QueueHandle_t)queue);
    if (!q || q->item_size == 0) {
        return 0;
    }
    return q->count;
}

uint32_t crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len)
{
    crc = ~crc;
    while (len--) {
        crc ^= *buf++;
        for (int i = 0; i < 8; ++i) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return ~crc;
}

void abort(void)
{
    holo_port_log("[retrogo.so] abort");
    holo_runtime_request_stop();
    holo_audio_end();
    holo_display_release();
    for (;;) {
        vTaskDelay(1000);
    }
}
