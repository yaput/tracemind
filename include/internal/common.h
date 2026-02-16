/**
 * TraceMind - Internal Common Utilities
 * 
 * Shared macros, memory management, and internal utilities.
 */

#ifndef TM_INTERNAL_COMMON_H
#define TM_INTERNAL_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include "tracemind.h"

/* ============================================================================
 * Memory Management
 * ========================================================================== */

/**
 * Allocation wrapper with null check.
 */
static inline void *tm_malloc(size_t size)
{
    void *ptr = malloc(size);
    if (!ptr && size > 0) {
        fprintf(stderr, "tracemind: fatal: out of memory (requested %zu bytes)\n", size);
        abort();
    }
    return ptr;
}

/**
 * Calloc wrapper with null check.
 */
static inline void *tm_calloc(size_t nmemb, size_t size)
{
    void *ptr = calloc(nmemb, size);
    if (!ptr && nmemb > 0 && size > 0) {
        fprintf(stderr, "tracemind: fatal: out of memory\n");
        abort();
    }
    return ptr;
}

/**
 * Realloc wrapper with null check.
 */
static inline void *tm_realloc(void *ptr, size_t size)
{
    void *new_ptr = realloc(ptr, size);
    if (!new_ptr && size > 0) {
        fprintf(stderr, "tracemind: fatal: out of memory (realloc %zu bytes)\n", size);
        abort();
    }
    return new_ptr;
}

/**
 * String duplication with null check.
 */
static inline char *tm_strdup(const char *s)
{
    if (!s) return NULL;
    char *dup = strdup(s);
    if (!dup) {
        fprintf(stderr, "tracemind: fatal: out of memory (strdup)\n");
        abort();
    }
    return dup;
}

/**
 * String duplication with length limit.
 */
static inline char *tm_strndup(const char *s, size_t n)
{
    if (!s) return NULL;
    size_t len = strnlen(s, n);
    char *dup = tm_malloc(len + 1);
    memcpy(dup, s, len);
    dup[len] = '\0';
    return dup;
}

/**
 * Free and nullify pointer.
 */
#define TM_FREE(ptr) do { free(ptr); (ptr) = NULL; } while(0)

/**
 * Safe free that handles NULL.
 */
static inline void tm_free(void *ptr)
{
    free(ptr);
}

/* ============================================================================
 * Dynamic Array (Vector) Macros
 * ========================================================================== */

#define TM_VEC_INIT_CAP 8

#define TM_VEC_PUSH(arr, count, cap, item) do {                     \
    if ((count) >= (cap)) {                                         \
        (cap) = (cap) == 0 ? TM_VEC_INIT_CAP : (cap) * 2;          \
        (arr) = tm_realloc((arr), sizeof(*(arr)) * (cap));         \
    }                                                               \
    (arr)[(count)++] = (item);                                     \
} while(0)

#define TM_VEC_FREE(arr, count, free_fn) do {                       \
    for (size_t _i = 0; _i < (count); _i++) {                      \
        free_fn((arr)[_i]);                                        \
    }                                                               \
    free(arr);                                                      \
    (arr) = NULL;                                                   \
    (count) = 0;                                                    \
} while(0)

/* ============================================================================
 * String Utilities
 * ========================================================================== */

/**
 * Case-insensitive substring search.
 */
static inline const char *tm_strcasestr(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return NULL;
    if (!*needle) return haystack;
    
    size_t needle_len = strlen(needle);
    for (; *haystack; haystack++) {
        /* Manual case-insensitive comparison */
        const char *h = haystack;
        const char *n = needle;
        size_t i = 0;
        while (i < needle_len && *h) {
            char hc = (*h >= 'A' && *h <= 'Z') ? (*h + 32) : *h;
            char nc = (*n >= 'A' && *n <= 'Z') ? (*n + 32) : *n;
            if (hc != nc) break;
            h++; n++; i++;
        }
        if (i == needle_len) return haystack;
    }
    return NULL;
}

/**
 * String builder for efficient concatenation.
 */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} tm_strbuf_t;

/**
 * Initialize string buffer.
 */
static inline void tm_strbuf_init(tm_strbuf_t *sb)
{
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

/**
 * Append to string buffer.
 */
static inline void tm_strbuf_append(tm_strbuf_t *sb, const char *str)
{
    if (!str) return;
    size_t add_len = strlen(str);
    if (sb->len + add_len + 1 > sb->cap) {
        size_t new_cap = sb->cap == 0 ? 64 : sb->cap * 2;
        while (new_cap < sb->len + add_len + 1) {
            new_cap *= 2;
        }
        sb->data = tm_realloc(sb->data, new_cap);
        sb->cap = new_cap;
    }
    memcpy(sb->data + sb->len, str, add_len + 1);
    sb->len += add_len;
}

/**
 * Append fixed-length data to string buffer.
 */
static inline void tm_strbuf_append_len(tm_strbuf_t *sb, const char *str, size_t add_len)
{
    if (!str || add_len == 0) return;
    if (sb->len + add_len + 1 > sb->cap) {
        size_t new_cap = sb->cap == 0 ? 64 : sb->cap * 2;
        while (new_cap < sb->len + add_len + 1) {
            new_cap *= 2;
        }
        sb->data = tm_realloc(sb->data, new_cap);
        sb->cap = new_cap;
    }
    memcpy(sb->data + sb->len, str, add_len);
    sb->len += add_len;
    sb->data[sb->len] = '\0';
}

/**
 * Append formatted string to buffer.
 */
static inline void tm_strbuf_appendf(tm_strbuf_t *sb, const char *fmt, ...)
{
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    tm_strbuf_append(sb, buf);
}

/**
 * Get string and free buffer (transfers ownership).
 */
static inline char *tm_strbuf_finish(tm_strbuf_t *sb)
{
    char *result = sb->data ? sb->data : tm_strdup("");
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    return result;
}

/**
 * Free string buffer.
 */
static inline void tm_strbuf_free(tm_strbuf_t *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

/* ============================================================================
 * Logging
 * ========================================================================== */

typedef enum {
    TM_LOG_ERROR = 0,
    TM_LOG_WARN,
    TM_LOG_INFO,
    TM_LOG_DEBUG
} tm_log_level_t;

extern tm_log_level_t g_log_level;

void tm_log(tm_log_level_t level, const char *fmt, ...);

#define TM_ERROR(...) tm_log(TM_LOG_ERROR, __VA_ARGS__)
#define TM_WARN(...)  tm_log(TM_LOG_WARN, __VA_ARGS__)
#define TM_INFO(...)  tm_log(TM_LOG_INFO, __VA_ARGS__)
#define TM_DEBUG(...) tm_log(TM_LOG_DEBUG, __VA_ARGS__)

/* ============================================================================
 * Error Handling Helpers
 * ========================================================================== */

#define TM_CHECK_NULL(ptr, ret) do {                                \
    if (!(ptr)) {                                                   \
        TM_ERROR("null pointer: %s", #ptr);                         \
        return (ret);                                               \
    }                                                               \
} while(0)

#define TM_CHECK_ERR(expr, cleanup) do {                            \
    tm_error_t _err = (expr);                                       \
    if (_err != TM_OK) {                                            \
        cleanup;                                                    \
        return _err;                                                \
    }                                                               \
} while(0)

/* ============================================================================
 * Miscellaneous
 * ========================================================================== */

#define TM_ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define TM_MIN(a, b) ((a) < (b) ? (a) : (b))
#define TM_MAX(a, b) ((a) > (b) ? (a) : (b))

/**
 * Check if string starts with prefix.
 */
static inline bool tm_str_starts_with(const char *str, const char *prefix)
{
    if (!str || !prefix) return false;
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

/**
 * Check if string ends with suffix.
 */
static inline bool tm_str_ends_with(const char *str, const char *suffix)
{
    if (!str || !suffix) return false;
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > str_len) return false;
    return strcmp(str + str_len - suffix_len, suffix) == 0;
}

/**
 * Trim whitespace from both ends (modifies in place).
 */
static inline char *tm_str_trim(char *str)
{
    if (!str) return NULL;
    
    /* Trim leading */
    while (*str && (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r')) {
        str++;
    }
    
    if (*str == '\0') return str;
    
    /* Trim trailing */
    char *end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        *end-- = '\0';
    }
    
    return str;
}

/**
 * Check if path looks like stdlib.
 */
bool tm_is_stdlib_path(const char *path, tm_language_t lang);

/**
 * Check if path looks like third-party dependency.
 */
bool tm_is_third_party_path(const char *path, tm_language_t lang);

/**
 * Normalize file path (resolve . and ..).
 */
char *tm_normalize_path(const char *path);

/**
 * Make path relative to base.
 */
char *tm_relative_path(const char *base, const char *path);

#endif /* TM_INTERNAL_COMMON_H */
