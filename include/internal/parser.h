/**
 * TraceMind - Stack Trace Parser
 * 
 * Language-specific stack trace parsing implementations.
 */

#ifndef TM_INTERNAL_PARSER_H
#define TM_INTERNAL_PARSER_H

#include "tracemind.h"

/* ============================================================================
 * Parser Registry
 * ========================================================================== */

/**
 * Parser function signature.
 * Each language implements this interface.
 */
typedef tm_error_t (*tm_parser_fn)(const char *input, tm_stack_trace_t *trace);

/**
 * Get parser for a specific language.
 */
tm_parser_fn tm_get_parser(tm_language_t lang);

/* ============================================================================
 * Language-Specific Parsers
 * ========================================================================== */

/**
 * Python traceback parser.
 * 
 * Handles formats:
 *   Traceback (most recent call last):
 *     File "path.py", line N, in function
 *       code
 *   ExceptionType: message
 */
tm_error_t tm_parse_python_trace(const char *input, tm_stack_trace_t *trace);

/**
 * Go panic/stack trace parser.
 * 
 * Handles formats:
 *   panic: message
 *   
 *   goroutine N [status]:
 *   package.function(args)
 *       /path/file.go:N +0xNN
 */
tm_error_t tm_parse_go_trace(const char *input, tm_stack_trace_t *trace);

/**
 * Node.js/JavaScript error parser.
 * 
 * Handles formats:
 *   Error: message
 *       at function (path:line:col)
 *       at path:line:col
 */
tm_error_t tm_parse_nodejs_trace(const char *input, tm_stack_trace_t *trace);

/* ============================================================================
 * Auto-Detection
 * ========================================================================== */

/**
 * Confidence score for language detection.
 */
typedef struct {
    tm_language_t language;
    int score;  /* 0-100 */
} tm_lang_score_t;

/**
 * Score all supported languages for a given input.
 * Returns array of TM_LANG_* entries with confidence scores.
 */
void tm_score_languages(const char *input, tm_lang_score_t scores[], size_t *count);

/* ============================================================================
 * Stack Frame Utilities
 * ========================================================================== */

/**
 * Create a new stack frame with copied values.
 */
tm_stack_frame_t *tm_frame_new(const char *function,
                               const char *file,
                               int line,
                               int column);

/**
 * Free a single stack frame's contents.
 */
void tm_frame_free_contents(tm_stack_frame_t *frame);

/**
 * Add frame to trace (trace takes ownership).
 */
tm_error_t tm_trace_add_frame(tm_stack_trace_t *trace, tm_stack_frame_t *frame);

/**
 * Create new empty trace.
 */
tm_stack_trace_t *tm_trace_new(void);

/* ============================================================================
 * Parsing Helpers
 * ========================================================================== */

/**
 * Extract file extension from path.
 */
const char *tm_get_extension(const char *path);

/**
 * Parse line number from string.
 * Returns -1 on failure.
 */
int tm_parse_line_number(const char *str);

/**
 * Check if line matches Python traceback header.
 */
bool tm_is_python_traceback_header(const char *line);

/**
 * Check if line matches Go panic header.
 */
bool tm_is_go_panic_header(const char *line);

/**
 * Check if line matches Node.js error header.
 */
bool tm_is_nodejs_error_header(const char *line);

#endif /* TM_INTERNAL_PARSER_H */
