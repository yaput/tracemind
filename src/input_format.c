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
