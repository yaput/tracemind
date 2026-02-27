/**
 * TraceMind - Input Format Detection and Extraction
 * 
 * Handles structured log formats (JSON, CSV) from cloud providers:
 * - GCP Cloud Logging (Log Explorer exports)
 * - AWS CloudWatch Logs
 * - Generic JSON lines format
 * - CSV/TSV exports
 * 
 * Also supports format-agnostic generic log analysis for:
 * - NGINX, Apache, Syslog, custom application logs
 * - Any text-based log format via LLM-assisted interpretation
 */

#ifndef TM_INTERNAL_INPUT_FORMAT_H
#define TM_INTERNAL_INPUT_FORMAT_H

#include "tracemind.h"
#include <jansson.h>

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
    TM_IFMT_TSV,              /* TSV with headers */
    TM_IFMT_GENERIC           /* Generic log format (LLM-assisted) */
} tm_ifmt_t;

/* ============================================================================
 * Analysis Mode (uses public API enum from tracemind.h)
 * ========================================================================== */

/* Analysis mode constants - map to public API enum:
 * TM_MODE_AUTO        = TM_ANALYSIS_AUTO
 * TM_MODE_STACK_TRACE = TM_ANALYSIS_TRACE  
 * TM_MODE_GENERIC_LOG = TM_ANALYSIS_LOG
 */
#define TM_MODE_AUTO         TM_ANALYSIS_AUTO
#define TM_MODE_STACK_TRACE  TM_ANALYSIS_TRACE
#define TM_MODE_GENERIC_LOG  TM_ANALYSIS_LOG

/* ============================================================================
 * Unified Log Model (Format-Agnostic)
 * ========================================================================== */

/**
 * Detected log format family (for format-aware prompts).
 */
typedef enum {
    TM_LOG_FMT_UNKNOWN = 0,
    TM_LOG_FMT_STACKTRACE,    /* Python/Go/JS exception trace */
    TM_LOG_FMT_NGINX,         /* NGINX access/error logs */
    TM_LOG_FMT_APACHE,        /* Apache access/error logs */
    TM_LOG_FMT_SYSLOG,        /* RFC 3164/5424 syslog */
    TM_LOG_FMT_DOCKER,        /* Docker container logs */
    TM_LOG_FMT_KUBERNETES,    /* K8s pod logs */
    TM_LOG_FMT_JSON_STRUCT,   /* Structured JSON logging */
    TM_LOG_FMT_CUSTOM         /* Application-specific format */
} tm_log_format_t;

/**
 * Get format family name as string.
 */
const char *tm_log_format_name(tm_log_format_t fmt);

/**
 * Generic log entry (format-agnostic representation).
 */
typedef struct {
    char *timestamp;              /* Timestamp string (any format) */
    char *severity;               /* Log level (ERROR/WARN/INFO/DEBUG/etc) */
    char *message;                /* Primary message content */
    char *source;                 /* Source identifier (file, service, host) */
    
    /* Extracted context (optional, populated when detected) */
    tm_stack_trace_t *trace;      /* Stack trace if found within entry */
    json_t *metadata;             /* Arbitrary key-value pairs from structured logs */
    
    /* Analysis markers (populated during analysis phase) */
    bool is_error;                /* Entry contains error indicators */
    bool is_anomaly;              /* Entry flagged as anomalous */
    float relevance_score;        /* 0.0-1.0 relevance to analysis */
    
    /* Raw data */
    char *raw_line;               /* Original unparsed line */
    size_t line_number;           /* Line number in source */
} tm_generic_log_entry_t;

/**
 * Collection of generic log entries with analysis metadata.
 */
typedef struct {
    /* Entries */
    tm_generic_log_entry_t *entries;
    size_t count;
    size_t capacity;
    
    /* Format detection results */
    tm_log_format_t detected_format;
    char *format_description;     /* Human-readable format description */
    
    /* Aggregate analysis */
    char **error_signatures;      /* Unique error patterns */
    size_t error_signature_count;
    
    char **anomaly_patterns;      /* Detected anomaly patterns */
    size_t anomaly_pattern_count;
    
    /* Time range */
    char *time_range_start;       /* First timestamp */
    char *time_range_end;         /* Last timestamp */
    
    /* Statistics */
    size_t total_errors;
    size_t total_warnings;
    size_t total_info;
} tm_generic_log_t;

/**
 * Create new generic log container.
 */
tm_generic_log_t *tm_generic_log_new(void);

/**
 * Add entry to generic log (takes ownership of fields).
 */
void tm_generic_log_add_entry(tm_generic_log_t *log,
                               const char *timestamp,
                               const char *severity,
                               const char *message,
                               const char *source,
                               const char *raw_line,
                               size_t line_number);

/**
 * Free generic log and all entries.
 */
void tm_generic_log_free(tm_generic_log_t *log);

/**
 * Free a single generic log entry's contents.
 */
void tm_generic_log_entry_free_contents(tm_generic_log_entry_t *entry);

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
 * Generic Log Analysis (Format-Agnostic)
 * ========================================================================== */

/**
 * Detect the log format family from content.
 * Uses heuristics to identify common log patterns.
 * 
 * @param content       Raw log content
 * @param len           Content length
 * @return              Detected log format family
 */
tm_log_format_t tm_detect_log_format(const char *content, size_t len);

/**
 * Determine recommended analysis mode from content.
 * Decides whether to use stack trace analysis or generic log analysis.
 * 
 * @param content       Raw input content
 * @param len           Content length
 * @return              Recommended analysis mode
 */
tm_analysis_mode_t tm_detect_analysis_mode(const char *content, size_t len);

/**
 * Parse generic logs into unified model.
 * Works with any text-based log format.
 * 
 * @param content       Raw log content
 * @param len           Content length
 * @param format_hint   Format hint (TM_LOG_FMT_UNKNOWN for auto-detect)
 * @return              Parsed generic log (caller owns)
 */
tm_generic_log_t *tm_parse_generic_log(const char *content, 
                                        size_t len,
                                        tm_log_format_t format_hint);

/**
 * Extract error entries from generic log.
 * Filters to only ERROR/FATAL/CRITICAL severity entries.
 * 
 * @param log           Parsed generic log
 * @return              Filtered log containing only errors (caller owns)
 */
tm_generic_log_t *tm_extract_errors(const tm_generic_log_t *log);

/**
 * Score entry relevance based on error keywords and patterns.
 * Updates the relevance_score field on entries.
 * 
 * @param log           Generic log to score
 */
void tm_score_entry_relevance(tm_generic_log_t *log);

/**
 * Check if content contains recognizable stack trace patterns.
 */
bool tm_has_stack_trace_patterns(const char *content, size_t len);

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

/**
 * Unified analysis entry point.
 * Auto-detects format and mode, returning either stack trace or generic log.
 * 
 * @param content       Raw input content
 * @param len           Content length
 * @param[out] mode     Detected/used analysis mode
 * @param[out] trace    Stack trace result (if mode is STACK_TRACE)
 * @param[out] log      Generic log result (if mode is GENERIC_LOG)
 * @return              TM_OK on success
 */
tm_error_t tm_unified_parse(const char *content,
                            size_t len,
                            tm_analysis_mode_t *mode,
                            tm_stack_trace_t **trace,
                            tm_generic_log_t **log);

#endif /* TM_INTERNAL_INPUT_FORMAT_H */
