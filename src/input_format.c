/**
 * TraceMind - Input Format Detection and Extraction
 * 
 * Handles JSON/CSV log formats from GCP, AWS, and generic exports.
 */

#include "internal/common.h"
#include "internal/input_format.h"
#include <jansson.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>

/* ============================================================================
 * Default Field Mappings
 * ========================================================================== */

const tm_log_fields_t TM_GCP_LOG_FIELDS = {
    .text_payload = "textPayload",
    .json_payload = "jsonPayload",
    .message = "message",
    .stack_trace = "stack_trace",
    .timestamp = "timestamp",
    .severity = "severity",
    .log_events = NULL,
    .error = "error",
    .exception = "exception",
    .traceback = "traceback"
};

const tm_log_fields_t TM_AWS_LOG_FIELDS = {
    .text_payload = NULL,
    .json_payload = NULL,
    .message = "@message",
    .stack_trace = NULL,
    .timestamp = "@timestamp",
    .severity = NULL,
    .log_events = "logEvents",
    .error = "errorMessage",
    .exception = "exception",
    .traceback = "stackTrace"
};

/* ============================================================================
 * Format Detection
 * ========================================================================== */

const char *tm_input_format_name(tm_ifmt_t fmt)
{
    switch (fmt) {
        case TM_IFMT_AUTO:       return "auto";
        case TM_IFMT_RAW:        return "raw";
        case TM_IFMT_JSON:       return "json";
        case TM_IFMT_JSON_ARRAY: return "json-array";
        case TM_IFMT_CSV:        return "csv";
        case TM_IFMT_TSV:        return "tsv";
        default:                         return "unknown";
    }
}

/**
 * Skip whitespace and return pointer to first non-whitespace.
 */
static const char *skip_whitespace(const char *s, const char *end)
{
    while (s < end && isspace((unsigned char)*s)) s++;
    return s;
}

tm_ifmt_t tm_detect_input_format(const char *content, size_t len)
{
    if (!content || len == 0) return TM_IFMT_RAW;
    
    const char *end = content + len;
    const char *p = skip_whitespace(content, end);
    
    if (p >= end) return TM_IFMT_RAW;
    
    /* JSON array starts with [ */
    if (*p == '[') {
        return TM_IFMT_JSON_ARRAY;
    }
    
    /* JSON lines (NDJSON) starts with { */
    if (*p == '{') {
        return TM_IFMT_JSON;
    }
    
    /* Check for CSV/TSV by looking at first line */
    const char *newline = memchr(p, '\n', (size_t)(end - p));
    if (newline) {
        size_t first_line_len = (size_t)(newline - p);
        
        /* Count tabs and commas in first line */
        int tabs = 0, commas = 0;
        for (size_t i = 0; i < first_line_len; i++) {
            if (p[i] == '\t') tabs++;
            else if (p[i] == ',') commas++;
        }
        
        /* TSV: multiple tabs, or more tabs than commas */
        if (tabs >= 2 || (tabs > 0 && tabs >= commas)) {
            /* Check if first line looks like headers */
            if (tm_strcasestr(p, "timestamp") || 
                tm_strcasestr(p, "severity") ||
                tm_strcasestr(p, "message") ||
                tm_strcasestr(p, "textPayload")) {
                return TM_IFMT_TSV;
            }
        }
        
        /* CSV: multiple commas */
        if (commas >= 2) {
            /* Check if first line looks like headers */
            if (tm_strcasestr(p, "timestamp") || 
                tm_strcasestr(p, "severity") ||
                tm_strcasestr(p, "message") ||
                tm_strcasestr(p, "textPayload")) {
                return TM_IFMT_CSV;
            }
        }
    }
    
    return TM_IFMT_RAW;
}

bool tm_is_structured_log(const char *content, size_t len)
{
    tm_ifmt_t fmt = tm_detect_input_format(content, len);
    return fmt != TM_IFMT_RAW;
}

/* ============================================================================
 * Log Entry Management
 * ========================================================================== */

static tm_log_entries_t *log_entries_new(void)
{
    tm_log_entries_t *entries = tm_calloc(1, sizeof(tm_log_entries_t));
    entries->capacity = 16;
    entries->entries = tm_calloc(entries->capacity, sizeof(tm_log_entry_t));
    return entries;
}

static void log_entries_add(tm_log_entries_t *entries, 
                           const char *text,
                           const char *timestamp,
                           const char *severity,
                           const char *source)
{
    if (!entries || !text) return;
    
    /* Grow if needed */
    if (entries->count >= entries->capacity) {
        entries->capacity *= 2;
        entries->entries = tm_realloc(entries->entries, 
                                      entries->capacity * sizeof(tm_log_entry_t));
    }
    
    tm_log_entry_t *entry = &entries->entries[entries->count++];
    entry->text = tm_strdup(text);
    entry->timestamp = timestamp ? tm_strdup(timestamp) : NULL;
    entry->severity = severity ? tm_strdup(severity) : NULL;
    entry->source = source ? tm_strdup(source) : NULL;
}

void tm_log_entries_free(tm_log_entries_t *entries)
{
    if (!entries) return;
    
    for (size_t i = 0; i < entries->count; i++) {
        TM_FREE(entries->entries[i].text);
        TM_FREE(entries->entries[i].timestamp);
        TM_FREE(entries->entries[i].severity);
        TM_FREE(entries->entries[i].source);
    }
    TM_FREE(entries->entries);
    free(entries);
}

/* ============================================================================
 * JSON Extraction
 * ========================================================================== */

/**
 * Check if string contains stack trace patterns.
 */
static bool looks_like_stack_trace(const char *text)
{
    if (!text) return false;
    
    /* Python patterns */
    if (strstr(text, "Traceback (most recent call last)")) return true;
    if (strstr(text, "File \"") && strstr(text, ", line ")) return true;
    
    /* Go patterns */
    if (strstr(text, "panic:") || strstr(text, "goroutine ")) return true;
    if (strstr(text, ".go:") && strstr(text, "+0x")) return true;
    
    /* Node.js/JavaScript patterns */
    if (strstr(text, "    at ") && (strstr(text, ".js:") || strstr(text, ".ts:"))) return true;
    
    /* Java patterns */
    if (strstr(text, "at ") && strstr(text, ".java:")) return true;
    if (strstr(text, "Exception") && strstr(text, "\n\tat ")) return true;
    
    /* Generic error patterns */
    if (strstr(text, "Error:") || strstr(text, "Exception:")) {
        if (strstr(text, "\n\t") || strstr(text, "\n    at ")) return true;
    }
    
    return false;
}

/**
 * Get nested JSON value using dot notation (e.g., "jsonPayload.message").
 */
static json_t *json_get_nested(json_t *obj, const char *path)
{
    if (!obj || !path) return NULL;
    
    char *path_copy = tm_strdup(path);
    char *saveptr = NULL;
    char *token = strtok_r(path_copy, ".", &saveptr);
    json_t *current = obj;
    
    while (token && current) {
        if (!json_is_object(current)) {
            current = NULL;
            break;
        }
        current = json_object_get(current, token);
        token = strtok_r(NULL, ".", &saveptr);
    }
    
    TM_FREE(path_copy);
    return current;
}

/**
 * Extract string value from JSON, returning copy or NULL.
 */
static char *json_get_string_copy(json_t *obj, const char *key)
{
    json_t *val = json_get_nested(obj, key);
    if (val && json_is_string(val)) {
        return tm_strdup(json_string_value(val));
    }
    return NULL;
}

/**
 * Extract message from GCP-style jsonPayload (handles nested message.message).
 */
static char *extract_gcp_message(json_t *obj)
{
    if (!obj) return NULL;
    
    /* Try jsonPayload.message.message (GCP structured logging) */
    json_t *jp = json_object_get(obj, "jsonPayload");
    if (jp && json_is_object(jp)) {
        json_t *msg = json_object_get(jp, "message");
        if (msg && json_is_object(msg)) {
            json_t *inner = json_object_get(msg, "message");
            if (inner && json_is_string(inner)) {
                return tm_strdup(json_string_value(inner));
            }
        }
        /* Try jsonPayload.message as string */
        if (msg && json_is_string(msg)) {
            return tm_strdup(json_string_value(msg));
        }
        /* Try jsonPayload.msg */
        msg = json_object_get(jp, "msg");
        if (msg && json_is_string(msg)) {
            return tm_strdup(json_string_value(msg));
        }
    }
    
    /* Try textPayload */
    json_t *tp = json_object_get(obj, "textPayload");
    if (tp && json_is_string(tp)) {
        return tm_strdup(json_string_value(tp));
    }
    
    /* Try top-level message */
    json_t *msg = json_object_get(obj, "message");
    if (msg && json_is_string(msg)) {
        return tm_strdup(json_string_value(msg));
    }
    
    return NULL;
}

/**
 * Check if JSON object has GCP sourceLocation.
 */
static bool has_source_location(json_t *obj)
{
    json_t *sl = json_object_get(obj, "sourceLocation");
    return sl && json_is_object(sl);
}

/**
 * Try to extract stack trace text from a JSON log object.
 */
static char *extract_trace_from_json_obj(json_t *obj, const tm_log_fields_t *fields)
{
    if (!obj || !json_is_object(obj)) return NULL;
    
    const tm_log_fields_t *f = fields ? fields : &TM_GCP_LOG_FIELDS;
    char *result = NULL;
    
    /* Priority 1: textPayload (GCP) - usually contains full stack trace */
    if (f->text_payload) {
        result = json_get_string_copy(obj, f->text_payload);
        if (result && looks_like_stack_trace(result)) {
            return result;
        }
        TM_FREE(result);
        result = NULL;
    }
    
    /* Priority 2: Explicit stack_trace field */
    if (f->stack_trace) {
        result = json_get_string_copy(obj, f->stack_trace);
        if (result && strlen(result) > 0) {
            return result;
        }
        TM_FREE(result);
        result = NULL;
    }
    
    /* Priority 3: exception/traceback fields */
    const char *trace_fields[] = {"exception", "traceback", "stacktrace", 
                                   "stack_trace", "error.stack", "err.stack"};
    for (size_t i = 0; i < sizeof(trace_fields)/sizeof(trace_fields[0]); i++) {
        result = json_get_string_copy(obj, trace_fields[i]);
        if (result && looks_like_stack_trace(result)) {
            return result;
        }
        TM_FREE(result);
        result = NULL;
    }
    
    /* Priority 4: jsonPayload.message or message */
    if (f->json_payload) {
        char nested_key[256];
        snprintf(nested_key, sizeof(nested_key), "%s.%s", 
                 f->json_payload, f->message ? f->message : "message");
        result = json_get_string_copy(obj, nested_key);
        if (result && looks_like_stack_trace(result)) {
            return result;
        }
        TM_FREE(result);
        result = NULL;
    }
    
    if (f->message) {
        result = json_get_string_copy(obj, f->message);
        if (result && looks_like_stack_trace(result)) {
            return result;
        }
        TM_FREE(result);
        result = NULL;
    }
    
    /* Priority 5: error field */
    if (f->error) {
        result = json_get_string_copy(obj, f->error);
        if (result && looks_like_stack_trace(result)) {
            return result;
        }
        TM_FREE(result);
    }
    
    return NULL;
}

/**
 * Build a synthetic Go-style stack trace from GCP log entries with sourceLocation.
 * This handles the common case where logs have file/line/function metadata.
 */
static char *build_trace_from_gcp_logs(json_t *arr)
{
    if (!arr || !json_is_array(arr)) return NULL;
    
    tm_strbuf_t trace;
    tm_strbuf_init(&trace);
    
    /* Find error entries first */
    bool found_error = false;
    size_t idx;
    json_t *obj;
    
    json_array_foreach(arr, idx, obj) {
        json_t *sev = json_object_get(obj, "severity");
        if (sev && json_is_string(sev)) {
            const char *severity = json_string_value(sev);
            if (strcasecmp(severity, "ERROR") == 0 || 
                strcasecmp(severity, "CRITICAL") == 0 ||
                strcasecmp(severity, "FATAL") == 0) {
                
                if (!found_error) {
                    /* Extract error message */
                    char *msg = extract_gcp_message(obj);
                    if (msg) {
                        tm_strbuf_append(&trace, "Error: ");
                        tm_strbuf_append(&trace, msg);
                        tm_strbuf_append(&trace, "\n\n");
                        TM_FREE(msg);
                    }
                    
                    /* Check for error details in jsonPayload.message.variables.err */
                    json_t *jp = json_object_get(obj, "jsonPayload");
                    if (jp) {
                        json_t *msg_obj = json_object_get(jp, "message");
                        if (msg_obj && json_is_object(msg_obj)) {
                            json_t *vars = json_object_get(msg_obj, "variables");
                            if (vars && json_is_object(vars)) {
                                json_t *err = json_object_get(vars, "err");
                                if (err && json_is_string(err)) {
                                    tm_strbuf_append(&trace, "Cause: ");
                                    tm_strbuf_append(&trace, json_string_value(err));
                                    tm_strbuf_append(&trace, "\n\n");
                                }
                            }
                        }
                    }
                    found_error = true;
                }
            }
        }
    }
    
    /* If no explicit errors, check for error-like messages */
    if (!found_error) {
        json_array_foreach(arr, idx, obj) {
            char *msg = extract_gcp_message(obj);
            if (msg && (strstr(msg, "error") || strstr(msg, "Error") || 
                        strstr(msg, "fail") || strstr(msg, "Fail"))) {
                tm_strbuf_append(&trace, "Error: ");
                tm_strbuf_append(&trace, msg);
                tm_strbuf_append(&trace, "\n\n");
                TM_FREE(msg);
                found_error = true;
                break;
            }
            TM_FREE(msg);
        }
    }
    
    /* Build Go-style stack trace from sourceLocation entries */
    tm_strbuf_append(&trace, "goroutine 1 [running]:\n");
    
    size_t frame_count = 0;
    json_array_foreach(arr, idx, obj) {
        if (!has_source_location(obj)) continue;
        
        json_t *sl = json_object_get(obj, "sourceLocation");
        json_t *func = json_object_get(sl, "function");
        json_t *file = json_object_get(sl, "file");
        json_t *line = json_object_get(sl, "line");
        
        if (func && json_is_string(func) && file && json_is_string(file)) {
            /* Function line */
            tm_strbuf_append(&trace, json_string_value(func));
            tm_strbuf_append(&trace, "(...)\n");
            
            /* File:line */
            tm_strbuf_append(&trace, "\t");
            tm_strbuf_append(&trace, json_string_value(file));
            tm_strbuf_append(&trace, ":");
            if (line && json_is_string(line)) {
                tm_strbuf_append(&trace, json_string_value(line));
            } else if (line && json_is_integer(line)) {
                char line_str[32];
                snprintf(line_str, sizeof(line_str), "%lld", json_integer_value(line));
                tm_strbuf_append(&trace, line_str);
            } else {
                tm_strbuf_append(&trace, "0");
            }
            tm_strbuf_append(&trace, " +0x0\n");
            
            frame_count++;
            
            /* Limit to reasonable number of frames */
            if (frame_count >= 50) break;
        }
    }
    
    if (frame_count == 0 && !found_error) {
        tm_strbuf_free(&trace);
        return NULL;
    }
    
    TM_DEBUG("Built synthetic trace with %zu frames from GCP logs", frame_count);
    return tm_strbuf_finish(&trace);
}

tm_log_entries_t *tm_extract_from_json(const char *content, 
                                       size_t len,
                                       const tm_log_fields_t *fields)
{
    if (!content || len == 0) return NULL;
    
    tm_log_entries_t *entries = log_entries_new();
    const tm_log_fields_t *f = fields ? fields : &TM_GCP_LOG_FIELDS;
    
    /* Parse line by line (NDJSON format) */
    const char *line_start = content;
    const char *end = content + len;
    
    while (line_start < end) {
        /* Find end of line */
        const char *line_end = memchr(line_start, '\n', (size_t)(end - line_start));
        if (!line_end) line_end = end;
        
        /* Skip empty lines */
        size_t line_len = (size_t)(line_end - line_start);
        if (line_len == 0 || (line_len == 1 && *line_start == '\r')) {
            line_start = line_end + 1;
            continue;
        }
        
        /* Parse JSON object */
        json_error_t error;
        json_t *obj = json_loadb(line_start, line_len, 0, &error);
        
        if (obj && json_is_object(obj)) {
            char *trace = extract_trace_from_json_obj(obj, f);
            if (trace) {
                char *timestamp = f->timestamp ? 
                    json_get_string_copy(obj, f->timestamp) : NULL;
                char *severity = f->severity ?
                    json_get_string_copy(obj, f->severity) : NULL;
                
                log_entries_add(entries, trace, timestamp, severity, NULL);
                
                TM_FREE(trace);
                TM_FREE(timestamp);
                TM_FREE(severity);
            }
            json_decref(obj);
        }
        
        line_start = line_end + 1;
    }
    
    if (entries->count == 0) {
        tm_log_entries_free(entries);
        return NULL;
    }
    
    TM_DEBUG("Extracted %zu stack traces from JSON", entries->count);
    return entries;
}

tm_log_entries_t *tm_extract_from_json_array(const char *content,
                                              size_t len,
                                              const tm_log_fields_t *fields)
{
    if (!content || len == 0) return NULL;
    
    json_error_t error;
    json_t *root = json_loadb(content, len, 0, &error);
    
    if (!root) {
        TM_WARN("Failed to parse JSON array: %s", error.text);
        return NULL;
    }
    
    if (!json_is_array(root)) {
        json_decref(root);
        return NULL;
    }
    
    tm_log_entries_t *entries = log_entries_new();
    const tm_log_fields_t *f = fields ? fields : &TM_GCP_LOG_FIELDS;
    
    size_t idx;
    json_t *obj;
    json_array_foreach(root, idx, obj) {
        if (!json_is_object(obj)) continue;
        
        char *trace = extract_trace_from_json_obj(obj, f);
        if (trace) {
            char *timestamp = f->timestamp ? 
                json_get_string_copy(obj, f->timestamp) : NULL;
            char *severity = f->severity ?
                json_get_string_copy(obj, f->severity) : NULL;
            
            log_entries_add(entries, trace, timestamp, severity, NULL);
            
            TM_FREE(trace);
            TM_FREE(timestamp);
            TM_FREE(severity);
        }
    }
    
    /* If no traditional stack traces found, try GCP sourceLocation extraction */
    if (entries->count == 0) {
        TM_DEBUG("No stack traces found, trying GCP sourceLocation extraction");
        char *gcp_trace = build_trace_from_gcp_logs(root);
        if (gcp_trace) {
            log_entries_add(entries, gcp_trace, NULL, "ERROR", NULL);
            TM_FREE(gcp_trace);
        }
    }
    
    json_decref(root);
    
    if (entries->count == 0) {
        tm_log_entries_free(entries);
        return NULL;
    }
    
    TM_DEBUG("Extracted %zu stack traces from JSON array", entries->count);
    return entries;
}

/* ============================================================================
 * CSV Extraction
 * ========================================================================== */

/**
 * Parse a single CSV field, handling quotes.
 */
static char *parse_csv_field(const char **cursor, const char *end, char delim)
{
    const char *p = *cursor;
    
    if (p >= end) return NULL;
    
    /* Handle quoted field */
    if (*p == '"') {
        p++;
        const char *start = p;
        tm_strbuf_t buf;
        tm_strbuf_init(&buf);
        
        while (p < end) {
            if (*p == '"') {
                /* Check for escaped quote */
                if (p + 1 < end && *(p + 1) == '"') {
                    tm_strbuf_append_len(&buf, start, (size_t)(p - start + 1));
                    p += 2;
                    start = p;
                } else {
                    /* End of quoted field */
                    tm_strbuf_append_len(&buf, start, (size_t)(p - start));
                    p++;
                    /* Skip delimiter or newline */
                    if (p < end && (*p == delim || *p == '\n' || *p == '\r')) {
                        if (*p == '\r' && p + 1 < end && *(p + 1) == '\n') p++;
                        p++;
                    }
                    *cursor = p;
                    return buf.data;
                }
            } else {
                p++;
            }
        }
        
        /* Unterminated quote - return what we have */
        tm_strbuf_append_len(&buf, start, (size_t)(p - start));
        *cursor = p;
        return buf.data;
    }
    
    /* Unquoted field */
    const char *start = p;
    while (p < end && *p != delim && *p != '\n' && *p != '\r') {
        p++;
    }
    
    size_t len = (size_t)(p - start);
    char *result = tm_strndup(start, len);
    
    /* Skip delimiter */
    if (p < end && *p == delim) p++;
    else if (p < end && *p == '\r') {
        p++;
        if (p < end && *p == '\n') p++;
    } else if (p < end && *p == '\n') {
        p++;
    }
    
    *cursor = p;
    return result;
}

/**
 * Find column index for a field name (case-insensitive).
 */
static int find_column(char **headers, size_t count, const char *name)
{
    if (!name) return -1;
    
    for (size_t i = 0; i < count; i++) {
        if (headers[i] && strcasecmp(headers[i], name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

tm_log_entries_t *tm_extract_from_csv(const char *content,
                                      size_t len,
                                      char delimiter)
{
    if (!content || len == 0) return NULL;
    
    const char *end = content + len;
    const char *cursor = content;
    
    /* Parse header row */
    char **headers = NULL;
    size_t header_count = 0;
    size_t header_capacity = 16;
    headers = tm_calloc(header_capacity, sizeof(char *));
    
    while (cursor < end && *cursor != '\n' && *cursor != '\r') {
        char *field = parse_csv_field(&cursor, end, delimiter);
        if (field) {
            if (header_count >= header_capacity) {
                header_capacity *= 2;
                headers = tm_realloc(headers, header_capacity * sizeof(char *));
            }
            headers[header_count++] = field;
        }
    }
    
    /* Skip newline after headers */
    if (cursor < end && *cursor == '\r') cursor++;
    if (cursor < end && *cursor == '\n') cursor++;
    
    if (header_count == 0) {
        TM_FREE(headers);
        return NULL;
    }
    
    /* Find relevant columns */
    int text_col = find_column(headers, header_count, "textPayload");
    if (text_col < 0) text_col = find_column(headers, header_count, "message");
    if (text_col < 0) text_col = find_column(headers, header_count, "text");
    if (text_col < 0) text_col = find_column(headers, header_count, "log");
    
    int timestamp_col = find_column(headers, header_count, "timestamp");
    int severity_col = find_column(headers, header_count, "severity");
    
    if (text_col < 0) {
        /* No text column found - free headers and return */
        for (size_t i = 0; i < header_count; i++) {
            TM_FREE(headers[i]);
        }
        TM_FREE(headers);
        TM_WARN("No text/message column found in CSV");
        return NULL;
    }
    
    tm_log_entries_t *entries = log_entries_new();
    
    /* Parse data rows */
    while (cursor < end) {
        /* Parse row fields */
        char **fields = tm_calloc(header_count, sizeof(char *));
        size_t field_count = 0;
        
        while (cursor < end && *cursor != '\n' && *cursor != '\r' && 
               field_count < header_count) {
            fields[field_count++] = parse_csv_field(&cursor, end, delimiter);
        }
        
        /* Skip newline */
        if (cursor < end && *cursor == '\r') cursor++;
        if (cursor < end && *cursor == '\n') cursor++;
        
        /* Extract stack trace if present */
        if (field_count > (size_t)text_col && fields[text_col]) {
            if (looks_like_stack_trace(fields[text_col])) {
                const char *timestamp = (timestamp_col >= 0 && 
                                         field_count > (size_t)timestamp_col) ?
                    fields[timestamp_col] : NULL;
                const char *severity = (severity_col >= 0 && 
                                        field_count > (size_t)severity_col) ?
                    fields[severity_col] : NULL;
                
                log_entries_add(entries, fields[text_col], timestamp, severity, NULL);
            }
        }
        
        /* Free row fields */
        for (size_t i = 0; i < field_count; i++) {
            TM_FREE(fields[i]);
        }
        TM_FREE(fields);
    }
    
    /* Free headers */
    for (size_t i = 0; i < header_count; i++) {
        TM_FREE(headers[i]);
    }
    TM_FREE(headers);
    
    if (entries->count == 0) {
        tm_log_entries_free(entries);
        return NULL;
    }
    
    TM_DEBUG("Extracted %zu stack traces from CSV", entries->count);
    return entries;
}

/* ============================================================================
 * Generic Log Model Implementation
 * ========================================================================== */

const char *tm_log_format_name(tm_log_format_t fmt)
{
    switch (fmt) {
        case TM_LOG_FMT_UNKNOWN:     return "unknown";
        case TM_LOG_FMT_STACKTRACE:  return "stack-trace";
        case TM_LOG_FMT_NGINX:       return "nginx";
        case TM_LOG_FMT_APACHE:      return "apache";
        case TM_LOG_FMT_SYSLOG:      return "syslog";
        case TM_LOG_FMT_DOCKER:      return "docker";
        case TM_LOG_FMT_KUBERNETES:  return "kubernetes";
        case TM_LOG_FMT_JSON_STRUCT: return "json-structured";
        case TM_LOG_FMT_CUSTOM:      return "custom";
        default:                     return "unknown";
    }
}

tm_generic_log_t *tm_generic_log_new(void)
{
    tm_generic_log_t *log = tm_calloc(1, sizeof(tm_generic_log_t));
    log->capacity = 64;
    log->entries = tm_calloc(log->capacity, sizeof(tm_generic_log_entry_t));
    log->detected_format = TM_LOG_FMT_UNKNOWN;
    return log;
}

void tm_generic_log_entry_free_contents(tm_generic_log_entry_t *entry)
{
    if (!entry) return;
    TM_FREE(entry->timestamp);
    TM_FREE(entry->severity);
    TM_FREE(entry->message);
    TM_FREE(entry->source);
    TM_FREE(entry->raw_line);
    if (entry->trace) {
        tm_stack_trace_free(entry->trace);
        entry->trace = NULL;
    }
    if (entry->metadata) {
        json_decref(entry->metadata);
        entry->metadata = NULL;
    }
}

void tm_generic_log_free(tm_generic_log_t *log)
{
    if (!log) return;
    
    for (size_t i = 0; i < log->count; i++) {
        tm_generic_log_entry_free_contents(&log->entries[i]);
    }
    TM_FREE(log->entries);
    
    TM_FREE(log->format_description);
    
    for (size_t i = 0; i < log->error_signature_count; i++) {
        TM_FREE(log->error_signatures[i]);
    }
    TM_FREE(log->error_signatures);
    
    for (size_t i = 0; i < log->anomaly_pattern_count; i++) {
        TM_FREE(log->anomaly_patterns[i]);
    }
    TM_FREE(log->anomaly_patterns);
    
    TM_FREE(log->time_range_start);
    TM_FREE(log->time_range_end);
    
    free(log);
}

void tm_generic_log_add_entry(tm_generic_log_t *log,
                               const char *timestamp,
                               const char *severity,
                               const char *message,
                               const char *source,
                               const char *raw_line,
                               size_t line_number)
{
    if (!log || !message) return;
    
    /* Grow if needed */
    if (log->count >= log->capacity) {
        log->capacity *= 2;
        log->entries = tm_realloc(log->entries, 
                                  log->capacity * sizeof(tm_generic_log_entry_t));
    }
    
    tm_generic_log_entry_t *entry = &log->entries[log->count];
    memset(entry, 0, sizeof(*entry));
    
    entry->timestamp = timestamp ? tm_strdup(timestamp) : NULL;
    entry->severity = severity ? tm_strdup(severity) : NULL;
    entry->message = tm_strdup(message);
    entry->source = source ? tm_strdup(source) : NULL;
    entry->raw_line = raw_line ? tm_strdup(raw_line) : NULL;
    entry->line_number = line_number;
    entry->relevance_score = 0.0f;
    
    /* Auto-detect error status from severity */
    if (severity) {
        if (strcasecmp(severity, "ERROR") == 0 ||
            strcasecmp(severity, "FATAL") == 0 ||
            strcasecmp(severity, "CRITICAL") == 0 ||
            strcasecmp(severity, "EMERG") == 0 ||
            strcasecmp(severity, "ALERT") == 0) {
            entry->is_error = true;
            log->total_errors++;
        } else if (strcasecmp(severity, "WARN") == 0 ||
                   strcasecmp(severity, "WARNING") == 0) {
            log->total_warnings++;
        } else if (strcasecmp(severity, "INFO") == 0) {
            log->total_info++;
        }
    }
    
    /* Update time range */
    if (timestamp) {
        if (!log->time_range_start) {
            log->time_range_start = tm_strdup(timestamp);
        }
        TM_FREE(log->time_range_end);
        log->time_range_end = tm_strdup(timestamp);
    }
    
    log->count++;
}

/* ============================================================================
 * Generic Log Format Detection
 * ========================================================================== */

bool tm_has_stack_trace_patterns(const char *content, size_t len)
{
    if (!content || len == 0) return false;
    
    /* Create null-terminated copy for strstr (safe since we're doing pattern matching) */
    /* Note: For large inputs, we could use a bounded search, but strstr is sufficient here */
    (void)len;  /* Use len in bounds-checking logic if needed in future */
    
    /* Python */
    if (strstr(content, "Traceback (most recent call last)")) return true;
    if (strstr(content, "File \"") && strstr(content, ", line ")) return true;
    
    /* Go */
    if (strstr(content, "panic:")) return true;
    if (strstr(content, "goroutine ") && strstr(content, ".go:")) return true;
    
    /* Node.js/JS */
    if (strstr(content, "    at ")) {
        if (strstr(content, ".js:") || strstr(content, ".ts:")) return true;
    }
    
    /* Java */
    if (strstr(content, "\n\tat ") && strstr(content, ".java:")) return true;
    if (strstr(content, "Exception in thread")) return true;
    
    return false;
}

tm_log_format_t tm_detect_log_format(const char *content, size_t len)
{
    if (!content || len == 0) return TM_LOG_FMT_UNKNOWN;
    
    /* Check for stack trace patterns first (highest priority) */
    if (tm_has_stack_trace_patterns(content, len)) {
        return TM_LOG_FMT_STACKTRACE;
    }
    
    /* Sample first few lines for pattern detection */
    const char *p = content;
    const char *end = content + len;
    int json_lines = 0;
    int nginx_lines = 0;
    int syslog_lines = 0;
    int docker_lines = 0;
    int sample_count = 0;
    const int max_sample = 20;
    
    while (p < end && sample_count < max_sample) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        if (!line_end) line_end = end;
        size_t line_len = (size_t)(line_end - p);
        
        if (line_len > 0) {
            /* JSON structured logging (starts with {) */
            if (*p == '{' && line_len > 2) {
                json_lines++;
            }
            
            /* NGINX combined format: IP - - [timestamp] "METHOD /path" status size */
            if (line_len > 20) {
                const char *bracket = memchr(p, '[', line_len);
                const char *quote = memchr(p, '"', line_len);
                if (bracket && quote && bracket < quote &&
                    (memchr(p, '.', (size_t)(bracket - p)) != NULL)) {
                    nginx_lines++;
                }
            }
            
            /* Syslog: <priority>timestamp hostname tag: message */
            /* or: Mon DD HH:MM:SS hostname tag: message */
            if (line_len > 15) {
                if (*p == '<' || 
                    (isalpha((unsigned char)p[0]) && isalpha((unsigned char)p[1]) && 
                     isalpha((unsigned char)p[2]) && p[3] == ' ')) {
                    if (strstr(p, ": ")) {
                        syslog_lines++;
                    }
                }
            }
            
            /* Docker logs: timestamp stdout/stderr message */
            if (line_len > 30 && 
                (strncmp(p + 23, " stdout ", 8) == 0 || 
                 strncmp(p + 23, " stderr ", 8) == 0 ||
                 strstr(p, "docker") || strstr(p, "container"))) {
                docker_lines++;
            }
            
            sample_count++;
        }
        
        p = line_end + 1;
    }
    
    /* Determine format based on detection counts */
    if (json_lines > sample_count / 2) return TM_LOG_FMT_JSON_STRUCT;
    if (nginx_lines > sample_count / 2) return TM_LOG_FMT_NGINX;
    if (syslog_lines > sample_count / 2) return TM_LOG_FMT_SYSLOG;
    if (docker_lines > sample_count / 3) return TM_LOG_FMT_DOCKER;
    
    /* Check for Kubernetes patterns */
    if (strstr(content, "kube-") || strstr(content, "pod/") ||
        strstr(content, "namespace=") || strstr(content, "kubernetes")) {
        return TM_LOG_FMT_KUBERNETES;
    }
    
    return TM_LOG_FMT_CUSTOM;
}

tm_analysis_mode_t tm_detect_analysis_mode(const char *content, size_t len)
{
    if (!content || len == 0) return TM_MODE_GENERIC_LOG;
    
    tm_log_format_t fmt = tm_detect_log_format(content, len);
    
    if (fmt == TM_LOG_FMT_STACKTRACE) {
        return TM_MODE_STACK_TRACE;
    }
    
    /* Check for embedded stack traces in structured logs */
    if (fmt == TM_LOG_FMT_JSON_STRUCT) {
        if (tm_has_stack_trace_patterns(content, len)) {
            return TM_MODE_STACK_TRACE;
        }
    }
    
    return TM_MODE_GENERIC_LOG;
}

/* ============================================================================
 * Generic Log Parsing
 * ========================================================================== */

/**
 * Parse a syslog-style line.
 * Format: <priority>timestamp hostname tag[pid]: message
 * or: Mon DD HH:MM:SS hostname tag: message
 */
static bool parse_syslog_line(const char *line, size_t len,
                               char **timestamp, char **severity,
                               char **message, char **source)
{
    *timestamp = NULL;
    *severity = NULL;
    *message = NULL;
    *source = NULL;
    
    if (len < 10) return false;
    
    const char *p = line;
    const char *end = line + len;
    
    /* Optional priority: <N> */
    int priority = -1;
    if (*p == '<') {
        p++;
        char *pend;
        priority = (int)strtol(p, &pend, 10);
        if (*pend == '>') {
            p = pend + 1;
        }
    }
    
    /* Timestamp (BSD style: Mon DD HH:MM:SS or ISO) */
    const char *ts_start = p;
    const char *colon = strstr(p, ": ");
    if (!colon || colon > end) return false;
    
    /* Find where hostname/tag starts (after timestamp) */
    const char *space = memchr(p, ' ', (size_t)(colon - p));
    if (space) {
        /* Extract timestamp (up to ~15 chars for BSD format) */
        size_t ts_len = (space - ts_start);
        if (ts_len > 30) ts_len = 30;  /* Sanity limit */
        *timestamp = tm_strndup(ts_start, ts_len);
        p = space + 1;
        
        /* Skip to message after colon */
        *source = tm_strndup(p, (size_t)(colon - p));
    }
    
    /* Message is everything after ": " */
    *message = tm_strndup(colon + 2, (size_t)(end - colon - 2));
    
    /* Map priority to severity */
    if (priority >= 0) {
        int level = priority & 0x7;
        const char *levels[] = {"EMERG", "ALERT", "CRITICAL", "ERROR", 
                                "WARNING", "NOTICE", "INFO", "DEBUG"};
        if (level < 8) {
            *severity = tm_strdup(levels[level]);
        }
    }
    
    return true;
}

/**
 * Parse JSON structured log line.
 */
static bool parse_json_log_line(const char *line, size_t len,
                                 char **timestamp, char **severity,
                                 char **message, char **source,
                                 json_t **metadata)
{
    *timestamp = NULL;
    *severity = NULL;
    *message = NULL;
    *source = NULL;
    *metadata = NULL;
    
    json_error_t error;
    json_t *obj = json_loadb(line, len, 0, &error);
    if (!obj || !json_is_object(obj)) {
        if (obj) json_decref(obj);
        return false;
    }
    
    /* Common timestamp fields */
    const char *ts_fields[] = {"timestamp", "time", "@timestamp", "ts", "datetime", "date"};
    for (size_t i = 0; i < sizeof(ts_fields)/sizeof(ts_fields[0]); i++) {
        json_t *ts = json_object_get(obj, ts_fields[i]);
        if (ts && json_is_string(ts)) {
            *timestamp = tm_strdup(json_string_value(ts));
            break;
        }
    }
    
    /* Common severity/level fields */
    const char *sev_fields[] = {"level", "severity", "loglevel", "log_level", "lvl"};
    for (size_t i = 0; i < sizeof(sev_fields)/sizeof(sev_fields[0]); i++) {
        json_t *sev = json_object_get(obj, sev_fields[i]);
        if (sev && json_is_string(sev)) {
            *severity = tm_strdup(json_string_value(sev));
            break;
        }
    }
    
    /* Common message fields */
    const char *msg_fields[] = {"message", "msg", "@message", "text", "log"};
    for (size_t i = 0; i < sizeof(msg_fields)/sizeof(msg_fields[0]); i++) {
        json_t *msg = json_object_get(obj, msg_fields[i]);
        if (msg && json_is_string(msg)) {
            *message = tm_strdup(json_string_value(msg));
            break;
        }
    }
    
    /* Source/logger fields */
    const char *src_fields[] = {"source", "logger", "service", "component", "name"};
    for (size_t i = 0; i < sizeof(src_fields)/sizeof(src_fields[0]); i++) {
        json_t *src = json_object_get(obj, src_fields[i]);
        if (src && json_is_string(src)) {
            *source = tm_strdup(json_string_value(src));
            break;
        }
    }
    
    /* If no message found, stringify entire object */
    if (!*message) {
        *message = json_dumps(obj, JSON_COMPACT);
    }
    
    /* Keep metadata reference */
    *metadata = obj;
    
    return (*message != NULL);
}

/**
 * Parse generic log line with heuristics.
 */
static bool parse_generic_line(const char *line, size_t len,
                                char **timestamp, char **severity,
                                char **message, char **source)
{
    *timestamp = NULL;
    *severity = NULL;
    *message = NULL;
    *source = NULL;
    
    if (len < 3) return false;
    
    /* Try to extract timestamp (ISO 8601 or common formats) */
    const char *p = line;
    
    /* ISO 8601: 2024-01-15T10:30:00 or 2024-01-15 10:30:00 */
    if (len > 19 && p[4] == '-' && p[7] == '-' && 
        (p[10] == 'T' || p[10] == ' ') && p[13] == ':') {
        *timestamp = tm_strndup(p, 19);
        p += 19;
        while (p < line + len && (*p == ' ' || *p == 'Z' || *p == '+' || *p == '-' || isdigit((unsigned char)*p))) {
            if (*p == ' ' && *(p+1) != ' ') break;
            p++;
        }
        while (p < line + len && *p == ' ') p++;
    }
    
    /* Try to extract severity level */
    const char *levels[] = {"ERROR", "WARN", "WARNING", "INFO", "DEBUG", 
                           "FATAL", "CRITICAL", "TRACE", "NOTICE"};
    for (size_t i = 0; i < sizeof(levels)/sizeof(levels[0]); i++) {
        size_t llen = strlen(levels[i]);
        /* Look for [LEVEL] or LEVEL: patterns */
        const char *match = NULL;
        if (*p == '[') {
            if (strncasecmp(p + 1, levels[i], llen) == 0 && p[1 + llen] == ']') {
                *severity = tm_strdup(levels[i]);
                p += 2 + llen;
                while (p < line + len && *p == ' ') p++;
                match = p;
            }
        } else if (strncasecmp(p, levels[i], llen) == 0 && 
                   (p[llen] == ':' || p[llen] == ' ' || p[llen] == '\t')) {
            *severity = tm_strdup(levels[i]);
            p += llen;
            if (*p == ':') p++;
            while (p < line + len && *p == ' ') p++;
            match = p;
        }
        if (match) break;
    }
    
    /* Remainder is the message */
    size_t remaining = (size_t)(line + len - p);
    if (remaining > 0) {
        *message = tm_strndup(p, remaining);
    }
    
    return (*message != NULL);
}

tm_generic_log_t *tm_parse_generic_log(const char *content, 
                                        size_t len,
                                        tm_log_format_t format_hint)
{
    if (!content || len == 0) return NULL;
    
    tm_generic_log_t *log = tm_generic_log_new();
    
    /* Auto-detect format if not specified */
    tm_log_format_t fmt = format_hint;
    if (fmt == TM_LOG_FMT_UNKNOWN) {
        fmt = tm_detect_log_format(content, len);
    }
    log->detected_format = fmt;
    log->format_description = tm_strdup(tm_log_format_name(fmt));
    
    /* Parse line by line */
    const char *line_start = content;
    const char *end = content + len;
    size_t line_num = 0;
    
    while (line_start < end) {
        line_num++;
        
        const char *line_end = memchr(line_start, '\n', (size_t)(end - line_start));
        if (!line_end) line_end = end;
        
        size_t line_len = (size_t)(line_end - line_start);
        
        /* Skip empty lines */
        if (line_len == 0 || (line_len == 1 && *line_start == '\r')) {
            line_start = line_end + 1;
            continue;
        }
        
        /* Strip CR if present */
        if (line_len > 0 && line_start[line_len - 1] == '\r') {
            line_len--;
        }
        
        char *timestamp = NULL;
        char *severity = NULL;
        char *message = NULL;
        char *source = NULL;
        json_t *metadata = NULL;
        bool parsed = false;
        
        switch (fmt) {
            case TM_LOG_FMT_JSON_STRUCT:
                parsed = parse_json_log_line(line_start, line_len,
                                              &timestamp, &severity, &message, &source, &metadata);
                break;
                
            case TM_LOG_FMT_SYSLOG:
                parsed = parse_syslog_line(line_start, line_len,
                                            &timestamp, &severity, &message, &source);
                break;
                
            default:
                parsed = parse_generic_line(line_start, line_len,
                                             &timestamp, &severity, &message, &source);
                break;
        }
        
        if (parsed && message) {
            tm_generic_log_add_entry(log, timestamp, severity, message, source,
                                      tm_strndup(line_start, line_len), line_num);
            
            /* Store metadata for last entry if present */
            if (metadata && log->count > 0) {
                log->entries[log->count - 1].metadata = metadata;
            } else if (metadata) {
                json_decref(metadata);
            }
        } else {
            /* Failed to parse - store as raw message */
            tm_generic_log_add_entry(log, NULL, NULL, 
                                      tm_strndup(line_start, line_len),
                                      NULL, tm_strndup(line_start, line_len), line_num);
        }
        
        TM_FREE(timestamp);
        TM_FREE(severity);
        TM_FREE(message);
        TM_FREE(source);
        
        line_start = line_end + 1;
    }
    
    TM_DEBUG("Parsed %zu log entries (format: %s, errors: %zu)", 
             log->count, log->format_description, log->total_errors);
    
    return log;
}

void tm_score_entry_relevance(tm_generic_log_t *log)
{
    if (!log) return;
    
    /* Error keywords and their weights */
    static const struct { const char *pattern; float weight; } patterns[] = {
        {"error", 0.3f},
        {"exception", 0.4f},
        {"failed", 0.3f},
        {"failure", 0.3f},
        {"timeout", 0.25f},
        {"refused", 0.25f},
        {"denied", 0.2f},
        {"crash", 0.5f},
        {"panic", 0.5f},
        {"fatal", 0.5f},
        {"critical", 0.4f},
        {"segfault", 0.5f},
        {"oom", 0.4f},
        {"out of memory", 0.4f},
        {"connection reset", 0.3f},
        {"502", 0.35f},
        {"503", 0.35f},
        {"500", 0.3f},
        {NULL, 0}
    };
    
    for (size_t i = 0; i < log->count; i++) {
        tm_generic_log_entry_t *e = &log->entries[i];
        float score = 0.0f;
        
        /* Base score from severity */
        if (e->is_error) {
            score += 0.4f;
        } else if (e->severity && 
                   (strcasecmp(e->severity, "WARN") == 0 || 
                    strcasecmp(e->severity, "WARNING") == 0)) {
            score += 0.15f;
        }
        
        /* Pattern matching on message */
        if (e->message) {
            for (size_t j = 0; patterns[j].pattern; j++) {
                if (tm_strcasestr(e->message, patterns[j].pattern)) {
                    score += patterns[j].weight;
                }
            }
        }
        
        /* Cap at 1.0 */
        e->relevance_score = score > 1.0f ? 1.0f : score;
        
        /* Mark high-relevance entries as anomalies */
        if (e->relevance_score >= 0.5f) {
            e->is_anomaly = true;
        }
    }
}

tm_generic_log_t *tm_extract_errors(const tm_generic_log_t *log)
{
    if (!log) return NULL;
    
    tm_generic_log_t *filtered = tm_generic_log_new();
    filtered->detected_format = log->detected_format;
    filtered->format_description = log->format_description ? 
        tm_strdup(log->format_description) : NULL;
    
    for (size_t i = 0; i < log->count; i++) {
        const tm_generic_log_entry_t *e = &log->entries[i];
        if (e->is_error || e->is_anomaly) {
            tm_generic_log_add_entry(filtered, e->timestamp, e->severity,
                                      e->message, e->source, e->raw_line, e->line_number);
            
            /* Copy relevance score */
            if (filtered->count > 0) {
                filtered->entries[filtered->count - 1].relevance_score = e->relevance_score;
                filtered->entries[filtered->count - 1].is_error = e->is_error;
                filtered->entries[filtered->count - 1].is_anomaly = e->is_anomaly;
            }
        }
    }
    
    return filtered;
}

tm_error_t tm_unified_parse(const char *content,
                            size_t len,
                            tm_analysis_mode_t *mode,
                            tm_stack_trace_t **trace,
                            tm_generic_log_t **log)
{
    if (!content || len == 0) return TM_ERR_INVALID_ARG;
    if (!mode || !trace || !log) return TM_ERR_INVALID_ARG;
    
    *trace = NULL;
    *log = NULL;
    
    /* Detect appropriate analysis mode */
    *mode = tm_detect_analysis_mode(content, len);
    
    if (*mode == TM_MODE_STACK_TRACE) {
        /* Extract stack traces (handles structured input) */
        char *extracted = tm_extract_stack_traces(content, len, TM_IFMT_AUTO);
        if (extracted) {
            *trace = tm_parse_stack_trace(extracted, strlen(extracted));
            TM_FREE(extracted);
            
            if (*trace) {
                TM_INFO("Parsed as stack trace (language: %s)", 
                        tm_language_name((*trace)->language));
                return TM_OK;
            }
        }
        /* Fall through to generic parsing if stack trace parsing fails */
        *mode = TM_MODE_GENERIC_LOG;
    }
    
    /* Generic log parsing */
    *log = tm_parse_generic_log(content, len, TM_LOG_FMT_UNKNOWN);
    if (!*log || (*log)->count == 0) {
        tm_generic_log_free(*log);
        *log = NULL;
        return TM_ERR_PARSE;
    }
    
    /* Score relevance */
    tm_score_entry_relevance(*log);
    
    TM_INFO("Parsed as generic log (format: %s, entries: %zu, errors: %zu)",
            (*log)->format_description, (*log)->count, (*log)->total_errors);
    
    return TM_OK;
}

/* ============================================================================
 * High-Level API
 * ========================================================================== */

char *tm_extract_stack_traces(const char *content,
                              size_t len,
                              tm_ifmt_t format_hint)
{
    if (!content || len == 0) return NULL;
    
    tm_ifmt_t format = format_hint;
    if (format == TM_IFMT_AUTO) {
        format = tm_detect_input_format(content, len);
    }
    
    /* Raw format - return as-is */
    if (format == TM_IFMT_RAW) {
        return tm_strndup(content, len);
    }
    
    tm_log_entries_t *entries = NULL;
    
    switch (format) {
        case TM_IFMT_JSON:
        case TM_IFMT_JSON_ARRAY: {
            /* Try to detect array vs NDJSON - check first non-whitespace char */
            const char *p = content;
            while (p < content + len && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
            
            if (p < content + len && *p == '[') {
                entries = tm_extract_from_json_array(content, len, NULL);
            } else {
                entries = tm_extract_from_json(content, len, NULL);
            }
            break;
        }
            
        case TM_IFMT_CSV:
            entries = tm_extract_from_csv(content, len, ',');
            break;
            
        case TM_IFMT_TSV:
            entries = tm_extract_from_csv(content, len, '\t');
            break;
            
        default:
            return tm_strndup(content, len);
    }
    
    if (!entries || entries->count == 0) {
        tm_log_entries_free(entries);
        /* Fall back to raw if no stack traces found */
        TM_WARN("No stack traces found in structured log, using raw content");
        return tm_strndup(content, len);
    }
    
    /* Concatenate all extracted traces */
    tm_strbuf_t buf;
    tm_strbuf_init(&buf);
    
    for (size_t i = 0; i < entries->count; i++) {
        if (i > 0) {
            tm_strbuf_append(&buf, "\n\n--- Entry ");
            char num[32];
            snprintf(num, sizeof(num), "%zu", i + 1);
            tm_strbuf_append(&buf, num);
            if (entries->entries[i].timestamp) {
                tm_strbuf_append(&buf, " (");
                tm_strbuf_append(&buf, entries->entries[i].timestamp);
                tm_strbuf_append(&buf, ")");
            }
            tm_strbuf_append(&buf, " ---\n\n");
        }
        tm_strbuf_append(&buf, entries->entries[i].text);
    }
    
    tm_log_entries_free(entries);
    
    TM_INFO("Extracted stack traces from %s format", tm_input_format_name(format));
    return buf.data;
}
