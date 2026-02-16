/**
 * TraceMind - Input Format Detection and Extraction
 * 
 * Handles structured log formats (JSON, CSV) from cloud providers:
 * - GCP Cloud Logging (Log Explorer exports)
 * - AWS CloudWatch Logs
 * - Generic JSON lines format
 * - CSV/TSV exports
 */

#ifndef TM_INTERNAL_INPUT_FORMAT_H
#define TM_INTERNAL_INPUT_FORMAT_H

#include "tracemind.h"

/* ============================================================================
 * Input Format Types
 * ========================================================================== */

/**
 * Detected input format (internal enum with extended types).
 */
typedef enum {
    TM_IFMT_AUTO = 0,         /* Auto-detect */
    TM_IFMT_RAW,              /* Plain text stack trace */
    TM_IFMT_JSON,             /* JSON lines (NDJSON) */
    TM_IFMT_JSON_ARRAY,       /* JSON array of objects */
    TM_IFMT_CSV,              /* CSV with headers */
    TM_IFMT_TSV               /* TSV with headers */
} tm_ifmt_t;

/**
 * Known field names for extracting stack traces from structured logs.
 * Supports GCP, AWS, and generic formats.
 */
typedef struct {
    /* GCP Cloud Logging fields */
    const char *text_payload;        /* "textPayload" */
    const char *json_payload;        /* "jsonPayload" */
    const char *message;             /* "message" or "jsonPayload.message" */
    const char *stack_trace;         /* "stack_trace" or "exception" */
    const char *timestamp;           /* "timestamp" */
    const char *severity;            /* "severity" */
    
    /* AWS CloudWatch fields */
    const char *log_events;          /* "@message" or "message" */
    
    /* Common error fields */
    const char *error;               /* "error" */
    const char *exception;           /* "exception" */
    const char *traceback;           /* "traceback" */
} tm_log_fields_t;

/* Default GCP field mappings */
extern const tm_log_fields_t TM_GCP_LOG_FIELDS;

/* Default AWS field mappings */
extern const tm_log_fields_t TM_AWS_LOG_FIELDS;

/* ============================================================================
 * Format Detection
 * ========================================================================== */

/**
 * Detect input format from content.
 * 
 * @param content  Raw input content (null-terminated)
 * @param len      Content length
 * @return         Detected format
 */
tm_ifmt_t tm_detect_input_format(const char *content, size_t len);

/**
 * Get format name as string.
 */
const char *tm_input_format_name(tm_ifmt_t fmt);

/* ============================================================================
 * Stack Trace Extraction
 * ========================================================================== */

/**
 * Extracted log entries containing potential stack traces.
 */
typedef struct {
    char *text;              /* Extracted text (stack trace or message) */
    char *timestamp;         /* Timestamp if available */
    char *severity;          /* Log level if available */
    char *source;            /* Source file/line if available */
} tm_log_entry_t;

/**
 * Collection of extracted log entries.
 */
typedef struct {
    tm_log_entry_t *entries;
    size_t count;
    size_t capacity;
} tm_log_entries_t;

/**
 * Extract stack traces from JSON lines input (NDJSON).
 * 
 * @param content       JSON lines content
 * @param len           Content length
 * @param fields        Field mappings (NULL for defaults)
 * @return              Extracted entries (caller owns)
 */
tm_log_entries_t *tm_extract_from_json(const char *content, 
                                       size_t len,
                                       const tm_log_fields_t *fields);

/**
 * Extract stack traces from JSON array input.
 * 
 * @param content       JSON array content
 * @param len           Content length
 * @param fields        Field mappings (NULL for defaults)
 * @return              Extracted entries (caller owns)
 */
tm_log_entries_t *tm_extract_from_json_array(const char *content,
                                              size_t len,
                                              const tm_log_fields_t *fields);

/**
 * Extract stack traces from CSV input.
 * 
 * @param content       CSV content with headers
 * @param len           Content length
 * @param delimiter     Field delimiter (',' for CSV, '\t' for TSV)
 * @return              Extracted entries (caller owns)
 */
tm_log_entries_t *tm_extract_from_csv(const char *content,
                                      size_t len,
                                      char delimiter);

/**
 * Free log entries collection.
 */
void tm_log_entries_free(tm_log_entries_t *entries);

/* ============================================================================
 * High-Level API
 * ========================================================================== */

/**
 * Extract raw stack trace text from structured input.
 * Auto-detects format and extracts stack traces.
 * 
 * @param content       Raw input content
 * @param len           Content length
 * @param format_hint   Format hint (TM_IFMT_AUTO for detection)
 * @return              Concatenated stack trace text (caller owns)
 */
char *tm_extract_stack_traces(const char *content,
                              size_t len,
                              tm_ifmt_t format_hint);

/**
 * Check if content looks like a structured log format.
 * Quick heuristic check before expensive parsing.
 */
bool tm_is_structured_log(const char *content, size_t len);

#endif /* TM_INTERNAL_INPUT_FORMAT_H */
