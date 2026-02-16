/**
 * TraceMind - Output Formatter
 * 
 * Formats analysis results for CLI, Markdown, and JSON output.
 */

#ifndef TM_INTERNAL_OUTPUT_H
#define TM_INTERNAL_OUTPUT_H

#include "tracemind.h"

/* ============================================================================
 * ANSI Color Codes
 * ========================================================================== */

#define TM_COLOR_RESET    "\033[0m"
#define TM_COLOR_BOLD     "\033[1m"
#define TM_COLOR_DIM      "\033[2m"
#define TM_COLOR_RED      "\033[31m"
#define TM_COLOR_GREEN    "\033[32m"
#define TM_COLOR_YELLOW   "\033[33m"
#define TM_COLOR_BLUE     "\033[34m"
#define TM_COLOR_MAGENTA  "\033[35m"
#define TM_COLOR_CYAN     "\033[36m"
#define TM_COLOR_WHITE    "\033[37m"

/* Bright variants */
#define TM_COLOR_BRED     "\033[91m"
#define TM_COLOR_BGREEN   "\033[92m"
#define TM_COLOR_BYELLOW  "\033[93m"
#define TM_COLOR_BBLUE    "\033[94m"
#define TM_COLOR_BMAGENTA "\033[95m"
#define TM_COLOR_BCYAN    "\033[96m"

/* Background */
#define TM_BG_RED         "\033[41m"
#define TM_BG_GREEN       "\033[42m"
#define TM_BG_YELLOW      "\033[43m"
#define TM_BG_BLUE        "\033[44m"

/* ============================================================================
 * Formatter Context
 * ========================================================================== */

/**
 * Output formatter context.
 */
typedef struct {
    tm_output_format_t format;
    bool use_colors;
    bool verbose;
    int terminal_width;           /* 0 = auto-detect */
    FILE *output;                 /* Output stream (default: stdout) */
} tm_formatter_t;

/**
 * Create a new formatter.
 */
tm_formatter_t *tm_formatter_new(tm_output_format_t format, bool colors);

/**
 * Free formatter.
 */
void tm_formatter_free(tm_formatter_t *fmt);

/* ============================================================================
 * CLI Output
 * ========================================================================== */

/**
 * Format analysis result for CLI output.
 * Returns allocated string (caller must free).
 */
char *tm_format_cli(const tm_formatter_t *fmt, const tm_analysis_result_t *result);

/**
 * Print header section.
 */
void tm_cli_header(const tm_formatter_t *fmt, const char *title);

/**
 * Print a hypothesis in CLI format.
 */
void tm_cli_hypothesis(const tm_formatter_t *fmt, const tm_hypothesis_t *hyp);

/**
 * Print stack trace summary.
 */
void tm_cli_trace_summary(const tm_formatter_t *fmt, const tm_stack_trace_t *trace);

/**
 * Print git context summary.
 */
void tm_cli_git_summary(const tm_formatter_t *fmt, const tm_git_context_t *ctx);

/**
 * Print call graph summary.
 */
void tm_cli_call_graph_summary(const tm_formatter_t *fmt, const tm_call_graph_t *graph);

/**
 * Print a horizontal divider.
 */
void tm_cli_divider(const tm_formatter_t *fmt);

/**
 * Print confidence bar (visual representation).
 */
void tm_cli_confidence_bar(const tm_formatter_t *fmt, int confidence);

/* ============================================================================
 * Markdown Output
 * ========================================================================== */

/**
 * Generate Markdown report.
 * Returns allocated string (caller must free).
 */
char *tm_format_markdown(const tm_formatter_t *fmt, const tm_analysis_result_t *result);

/**
 * Format hypothesis as Markdown.
 */
void tm_md_hypothesis(tm_strbuf_t *sb, const tm_hypothesis_t *hyp);

/**
 * Format stack trace as Markdown.
 */
void tm_md_trace(tm_strbuf_t *sb, const tm_stack_trace_t *trace);

/**
 * Format git context as Markdown.
 */
void tm_md_git_context(tm_strbuf_t *sb, const tm_git_context_t *ctx);

/* ============================================================================
 * JSON Output
 * ========================================================================== */

/**
 * Generate JSON output.
 * Returns allocated string (caller must free).
 */
char *tm_format_json(const tm_formatter_t *fmt, const tm_analysis_result_t *result);

/**
 * Serialize hypothesis to JSON.
 */
char *tm_json_hypothesis(const tm_hypothesis_t *hyp);

/**
 * Serialize stack trace to JSON.
 */
char *tm_json_trace(const tm_stack_trace_t *trace);

/**
 * Serialize git context to JSON.
 */
char *tm_json_git_context(const tm_git_context_t *ctx);

/* ============================================================================
 * Progress & Status Output
 * ========================================================================== */

/* Progress callback is defined in tracemind.h as:
 * typedef void (*tm_progress_cb)(const char *stage, float progress, void *ctx);
 */

/**
 * Print progress spinner.
 */
void tm_progress_spinner(const tm_formatter_t *fmt, const char *message);

/**
 * Print progress bar.
 */
void tm_progress_bar(const tm_formatter_t *fmt, 
                     const char *label, 
                     int current, 
                     int total);

/**
 * Print status message.
 */
void tm_status(const tm_formatter_t *fmt, 
               const char *icon, 
               const char *message);

/**
 * Print error message.
 */
void tm_error_msg(const tm_formatter_t *fmt, const char *message);

/**
 * Print warning message.
 */
void tm_warning_msg(const tm_formatter_t *fmt, const char *message);

/**
 * Print success message.
 */
void tm_success_msg(const tm_formatter_t *fmt, const char *message);

/* ============================================================================
 * Table Formatting
 * ========================================================================== */

/**
 * Table column definition.
 */
typedef struct {
    const char *header;
    int width;                    /* 0 = auto */
    int align;                    /* -1 = left, 0 = center, 1 = right */
} tm_table_col_t;

/**
 * Table context.
 */
typedef struct {
    tm_table_col_t *columns;
    size_t col_count;
    char ***rows;
    size_t row_count;
    size_t row_capacity;
} tm_table_t;

/**
 * Create a new table.
 */
tm_table_t *tm_table_new(const tm_table_col_t *cols, size_t col_count);

/**
 * Add row to table.
 */
void tm_table_add_row(tm_table_t *table, ...);

/**
 * Print table to formatter.
 */
void tm_table_print(const tm_formatter_t *fmt, const tm_table_t *table);

/**
 * Free table.
 */
void tm_table_free(tm_table_t *table);

/* ============================================================================
 * Utility Functions
 * ========================================================================== */

/**
 * Detect if output supports colors.
 */
bool tm_supports_colors(FILE *stream);

/**
 * Get terminal width.
 */
int tm_terminal_width(void);

/**
 * Wrap text to width.
 * Returns allocated string (caller must free).
 */
char *tm_wrap_text(const char *text, int width);

/**
 * Truncate string to length with ellipsis.
 * Returns allocated string (caller must free).
 */
char *tm_truncate_string(const char *str, size_t max_len);

/**
 * Escape string for JSON.
 * Returns allocated string (caller must free).
 */
char *tm_json_escape(const char *str);

/**
 * Format duration in human-readable form.
 * Returns allocated string (caller must free).
 */
char *tm_format_duration(int64_t ms);

/**
 * Format relative time (e.g., "2 hours ago").
 * Returns allocated string (caller must free).
 */
char *tm_format_relative_time(int64_t timestamp);

#endif /* TM_INTERNAL_OUTPUT_H */
