/**
 * TraceMind - Common Utilities Implementation
 */

#include "internal/common.h"
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <limits.h>
#include <strings.h>  /* For strcasecmp on POSIX */

/* Global log level */
tm_log_level_t g_log_level = TM_LOG_WARN;

/* ============================================================================
 * Logging
 * ========================================================================== */

void tm_log(tm_log_level_t level, const char *fmt, ...)
{
    if (level > g_log_level) return;
    
    static const char *level_names[] = {"ERROR", "WARN", "INFO", "DEBUG"};
    
    fprintf(stderr, "[%s] ", level_names[level]);
    
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    
    fprintf(stderr, "\n");
}

/* ============================================================================
 * Error Messages
 * ========================================================================== */

const char *tm_strerror(tm_error_t err)
{
    switch (err) {
    case TM_OK:              return "Success";
    case TM_ERR_NOMEM:       return "Out of memory";
    case TM_ERR_INVALID_ARG: return "Invalid argument";
    case TM_ERR_IO:          return "I/O error";
    case TM_ERR_PARSE:       return "Parse error";
    case TM_ERR_GIT:         return "Git operation failed";
    case TM_ERR_LLM:         return "LLM request failed";
    case TM_ERR_TIMEOUT:     return "Operation timed out";
    case TM_ERR_NOT_FOUND:   return "Not found";
    case TM_ERR_UNSUPPORTED: return "Unsupported operation";
    case TM_ERR_INTERNAL:    return "Internal error";
    default:                 return "Unknown error";
    }
}

/* ============================================================================
 * Language Detection
 * ========================================================================== */

static const struct {
    const char *ext;
    tm_language_t lang;
} ext_map[] = {
    { ".py",   TM_LANG_PYTHON },
    { ".pyw",  TM_LANG_PYTHON },
    { ".go",   TM_LANG_GO },
    { ".js",   TM_LANG_NODEJS },
    { ".mjs",  TM_LANG_NODEJS },
    { ".cjs",  TM_LANG_NODEJS },
    { ".ts",   TM_LANG_NODEJS },
    { ".tsx",  TM_LANG_NODEJS },
    { ".jsx",  TM_LANG_NODEJS },
    { NULL, TM_LANG_UNKNOWN }
};

tm_language_t tm_detect_language(const char *input)
{
    if (!input) return TM_LANG_UNKNOWN;
    
    /* Check for Python traceback */
    if (strstr(input, "Traceback (most recent call last)") ||
        strstr(input, "File \"") ||
        strstr(input, ".py\", line")) {
        return TM_LANG_PYTHON;
    }
    
    /* Check for Go panic/stack */
    if (strstr(input, "panic:") ||
        strstr(input, "goroutine ") ||
        strstr(input, ".go:")) {
        return TM_LANG_GO;
    }
    
    /* Check for Node.js/JavaScript */
    if (strstr(input, "at ") && 
        (strstr(input, ".js:") || strstr(input, ".ts:") ||
         strstr(input, "Error:") || strstr(input, "TypeError:"))) {
        return TM_LANG_NODEJS;
    }
    
    /* Try file extension detection */
    const char *ext = strrchr(input, '.');
    if (ext) {
        for (int i = 0; ext_map[i].ext; i++) {
            if (strcasecmp(ext, ext_map[i].ext) == 0) {
                return ext_map[i].lang;
            }
        }
    }
    
    return TM_LANG_UNKNOWN;
}

const char *tm_language_name(tm_language_t lang)
{
    switch (lang) {
    case TM_LANG_PYTHON:  return "Python";
    case TM_LANG_GO:      return "Go";
    case TM_LANG_NODEJS:  return "Node.js";
    case TM_LANG_JAVA:    return "Java";
    case TM_LANG_RUST:    return "Rust";
    case TM_LANG_CPP:     return "C++";
    default:              return "Unknown";
    }
}

/* ============================================================================
 * Path Utilities
 * ========================================================================== */

bool tm_is_stdlib_path(const char *path, tm_language_t lang)
{
    if (!path) return false;
    
    switch (lang) {
    case TM_LANG_PYTHON:
        return strstr(path, "/lib/python") != NULL &&
               strstr(path, "/site-packages/") == NULL &&
               strstr(path, "/dist-packages/") == NULL;
    case TM_LANG_GO:
        return tm_str_starts_with(path, "/usr/local/go/src/") ||
               strstr(path, "GOROOT") != NULL;
    case TM_LANG_NODEJS:
        return strstr(path, "internal/") != NULL ||
               tm_str_starts_with(path, "node:");
    default:
        return false;
    }
}

bool tm_is_third_party_path(const char *path, tm_language_t lang)
{
    if (!path) return false;
    
    switch (lang) {
    case TM_LANG_PYTHON:
        return strstr(path, "/site-packages/") != NULL ||
               strstr(path, "/dist-packages/") != NULL;
    case TM_LANG_GO:
        return strstr(path, "/pkg/mod/") != NULL ||
               strstr(path, "vendor/") != NULL;
    case TM_LANG_NODEJS:
        return strstr(path, "/node_modules/") != NULL;
    default:
        return false;
    }
}

char *tm_normalize_path(const char *path)
{
    if (!path) return NULL;
    
    char resolved[PATH_MAX];
    char *result = realpath(path, resolved);
    
    if (result) {
        return tm_strdup(resolved);
    }
    
    /* If realpath fails (file doesn't exist), do basic normalization */
    char *normalized = tm_strdup(path);
    /* TODO: Implement manual normalization for non-existent paths */
    return normalized;
}

char *tm_relative_path(const char *base, const char *path)
{
    if (!base || !path) return tm_strdup(path);
    
    size_t base_len = strlen(base);
    
    /* Ensure base ends with / */
    if (base_len > 0 && base[base_len - 1] == '/') {
        base_len--;
    }
    
    if (strncmp(path, base, base_len) == 0 && path[base_len] == '/') {
        return tm_strdup(path + base_len + 1);
    }
    
    return tm_strdup(path);
}

/* ============================================================================
 * File I/O
 * ========================================================================== */

char *tm_read_file(const char *path, size_t *size)
{
    if (!path) return NULL;
    
    FILE *f = fopen(path, "rb");
    if (!f) {
        TM_DEBUG("Failed to open file: %s (%s)", path, strerror(errno));
        return NULL;
    }
    
    /* Get file size */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (fsize < 0) {
        fclose(f);
        return NULL;
    }
    
    char *content = tm_malloc((size_t)fsize + 1);
    size_t read_size = fread(content, 1, (size_t)fsize, f);
    fclose(f);
    
    content[read_size] = '\0';
    
    if (size) {
        *size = read_size;
    }
    
    return content;
}

/* ============================================================================
 * UUID Generation
 * ========================================================================== */

char *tm_generate_uuid(void)
{
    char *uuid = tm_malloc(37);
    
    /* Use random data for UUID v4 */
    unsigned char random_bytes[16];
    FILE *urandom = fopen("/dev/urandom", "rb");
    
    if (urandom) {
        size_t result = fread(random_bytes, 1, sizeof(random_bytes), urandom);
        fclose(urandom);
        if (result != sizeof(random_bytes)) {
            /* Fallback to time-based randomness */
            struct timeval tv;
            gettimeofday(&tv, NULL);
            srand((unsigned int)(tv.tv_sec ^ tv.tv_usec ^ getpid()));
            for (size_t i = 0; i < sizeof(random_bytes); i++) {
                random_bytes[i] = (unsigned char)(rand() & 0xFF);
            }
        }
    } else {
        /* Fallback to time-based randomness */
        struct timeval tv;
        gettimeofday(&tv, NULL);
        srand((unsigned int)(tv.tv_sec ^ tv.tv_usec ^ getpid()));
        for (size_t i = 0; i < sizeof(random_bytes); i++) {
            random_bytes[i] = (unsigned char)(rand() & 0xFF);
        }
    }
    
    /* Set version (4) and variant (RFC 4122) */
    random_bytes[6] = (random_bytes[6] & 0x0F) | 0x40;
    random_bytes[8] = (random_bytes[8] & 0x3F) | 0x80;
    
    snprintf(uuid, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             random_bytes[0], random_bytes[1], random_bytes[2], random_bytes[3],
             random_bytes[4], random_bytes[5],
             random_bytes[6], random_bytes[7],
             random_bytes[8], random_bytes[9],
             random_bytes[10], random_bytes[11], random_bytes[12],
             random_bytes[13], random_bytes[14], random_bytes[15]);
    
    return uuid;
}

/* ============================================================================
 * Timestamp
 * ========================================================================== */

int64_t tm_timestamp_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
