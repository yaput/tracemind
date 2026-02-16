/**
 * TraceMind - AI Root Cause Assistant
 * 
 * Main public API header. All client code should include only this file.
 * 
 * Copyright (c) 2026 TraceMind
 * SPDX-License-Identifier: MIT
 */

#ifndef TRACEMIND_H
#define TRACEMIND_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Version Information
 * ========================================================================== */

#define TRACEMIND_VERSION_MAJOR 0
#define TRACEMIND_VERSION_MINOR 1
#define TRACEMIND_VERSION_PATCH 0
#define TRACEMIND_VERSION_STRING "0.1.0"

/* ============================================================================
 * Error Codes
 * ========================================================================== */

typedef enum {
    TM_OK = 0,
    TM_ERR_NOMEM = -1,
    TM_ERR_INVALID_ARG = -2,
    TM_ERR_IO = -3,
    TM_ERR_PARSE = -4,
    TM_ERR_GIT = -5,
    TM_ERR_LLM = -6,
    TM_ERR_TIMEOUT = -7,
    TM_ERR_NOT_FOUND = -8,
    TM_ERR_UNSUPPORTED = -9,
    TM_ERR_INTERNAL = -10
} tm_error_t;

/**
 * Get human-readable error message for an error code.
 */
const char *tm_strerror(tm_error_t err);

/* ============================================================================
 * Language Support
 * ========================================================================== */

typedef enum {
    TM_LANG_UNKNOWN = 0,
    TM_LANG_PYTHON,
    TM_LANG_GO,
    TM_LANG_NODEJS,
    TM_LANG_JAVA,      /* Future */
    TM_LANG_RUST,      /* Future */
    TM_LANG_CPP        /* Future */
} tm_language_t;

/**
 * Detect language from file extension or stack trace pattern.
 */
tm_language_t tm_detect_language(const char *input);

/**
 * Get language name as string.
 */
const char *tm_language_name(tm_language_t lang);

/* ============================================================================
 * Stack Frame & Trace Structures
 * ========================================================================== */

/**
 * A single frame in a stack trace.
 */
typedef struct {
    char *function;       /* Function name (owned) */
    char *file;           /* File path (owned) */
    int line;             /* Line number (0 if unknown) */
    int column;           /* Column number (0 if unknown) */
    char *module;         /* Module/package name (owned, nullable) */
    char *context;        /* Source context around the line (owned, nullable) */
    bool is_stdlib;       /* True if frame is from standard library */
    bool is_third_party;  /* True if frame is from third-party code */
} tm_stack_frame_t;

/**
 * Complete parsed stack trace.
 */
typedef struct {
    tm_language_t language;
    char *error_type;     /* Exception/error type (owned) */
    char *error_message;  /* Error message (owned) */
    tm_stack_frame_t *frames;
    size_t frame_count;
    size_t frame_capacity;
    char *raw_trace;      /* Original input (owned) */
} tm_stack_trace_t;

/* ============================================================================
 * Call Graph Structures
 * ========================================================================== */

/**
 * A node in the call graph representing a function.
 */
typedef struct tm_call_node {
    char *name;                   /* Fully qualified function name */
    char *file;                   /* Source file path */
    int start_line;               /* Definition start line */
    int end_line;                 /* Definition end line */
    char *signature;              /* Function signature */
    struct tm_call_node **callers;   /* Functions that call this */
    size_t caller_count;
    struct tm_call_node **callees;   /* Functions called by this */
    size_t callee_count;
    uint32_t complexity;          /* Cyclomatic complexity estimate */
} tm_call_node_t;

/**
 * Call graph for a set of related functions.
 */
typedef struct {
    tm_call_node_t **nodes;
    size_t node_count;
    size_t node_capacity;
    size_t edge_count;            /* Number of edges (caller->callee relationships) */
    tm_call_node_t *entry_point;  /* The failing function */
} tm_call_graph_t;

/* ============================================================================
 * Git Context Structures
 * ========================================================================== */

/**
 * A single git commit affecting relevant files.
 */
typedef struct {
    char sha[41];             /* Full commit SHA */
    char *author;             /* Author name (owned) */
    char *email;              /* Author email (owned) */
    int64_t timestamp;        /* Unix timestamp */
    char *message;            /* Commit message (owned) */
    char **files_changed;     /* List of changed files (owned) */
    size_t file_count;
    int additions;            /* Total lines added */
    int deletions;            /* Total lines deleted */
    bool touches_config;      /* True if touches config files */
    bool touches_schema;      /* True if touches DB schema */
} tm_git_commit_t;

/**
 * Blame information for a specific line.
 */
typedef struct {
    char sha[41];
    char *author;
    int64_t timestamp;
    char *line_content;
} tm_git_blame_t;

/**
 * Complete git context for analysis.
 */
typedef struct {
    char *repo_root;              /* Repository root path (owned) */
    char *current_branch;         /* Current branch name (owned) */
    char *head_sha;               /* HEAD commit SHA (owned) */
    tm_git_commit_t *commits;     /* Recent relevant commits */
    size_t commit_count;
    tm_git_blame_t **blames;      /* Blame info for error lines */
    size_t blame_count;
} tm_git_context_t;

/* ============================================================================
 * Hypothesis & Analysis Result
 * ========================================================================== */

/**
 * A single root cause hypothesis.
 */
typedef struct {
    int rank;                     /* 1-3, with 1 being most likely */
    int confidence;               /* 0-100 percentage */
    char *title;                  /* Short description (owned) */
    char *explanation;            /* Detailed explanation (owned) */
    char *evidence;               /* Supporting evidence (owned) */
    char *next_step;              /* Recommended validation step (owned) */
    char **related_files;         /* Related file paths (owned) */
    size_t related_file_count;
    char **related_commits;       /* Related commit SHAs (owned) */
    size_t related_commit_count;
} tm_hypothesis_t;

/**
 * Complete analysis result.
 */
typedef struct {
    tm_stack_trace_t *trace;      /* Parsed stack trace */
    tm_call_graph_t *call_graph;  /* Call graph analysis */
    tm_git_context_t *git_ctx;    /* Git context */
    tm_hypothesis_t **hypotheses; /* Ranked hypotheses (array of pointers) */
    size_t hypothesis_count;
    int analysis_time_ms;         /* Total analysis duration in ms */
    char *error_message;          /* Error message if analysis failed */
} tm_analysis_result_t;

/* ============================================================================
 * Configuration
 * ========================================================================== */

/**
 * LLM provider configuration.
 */
typedef enum {
    TM_LLM_OPENAI = 0,
    TM_LLM_ANTHROPIC,
    TM_LLM_LOCAL        /* For local models via Ollama, etc. */
} tm_llm_provider_t;

/**
 * Output format options.
 */
typedef enum {
    TM_OUTPUT_CLI = 0,    /* Formatted CLI table */
    TM_OUTPUT_MARKDOWN,   /* Markdown report */
    TM_OUTPUT_JSON        /* Machine-readable JSON */
} tm_output_format_t;

/**
 * TraceMind configuration.
 */
typedef struct {
    /* LLM Settings */
    tm_llm_provider_t llm_provider;
    char *api_key;            /* API key (owned, nullable for local) */
    char *api_endpoint;       /* Custom endpoint (owned, nullable) */
    char *model_name;         /* Model identifier (owned) */
    int timeout_ms;           /* Request timeout in milliseconds */
    float temperature;        /* LLM temperature (0.0-1.0) */
    
    /* Analysis Settings */
    int max_commits;          /* Max commits to analyze (default: 20) */
    int max_call_depth;       /* Max call graph depth (default: 5) */
    bool include_stdlib;      /* Include stdlib in analysis */
    bool include_tests;       /* Include test files in analysis */
    
    /* Output Settings */
    tm_output_format_t output_format;
    bool color_output;        /* Enable ANSI colors */
    bool verbose;             /* Verbose output */
    
    /* Paths */
    char *repo_path;          /* Repository path (default: cwd) */
    char *cache_dir;          /* Cache directory (owned, nullable) */
} tm_config_t;

/**
 * Create default configuration.
 * Caller must free with tm_config_free().
 */
tm_config_t *tm_config_new(void);

/**
 * Load configuration from file.
 */
tm_error_t tm_config_load(tm_config_t *cfg, const char *path);

/**
 * Load configuration from environment variables.
 */
tm_error_t tm_config_load_env(tm_config_t *cfg);

/**
 * Free configuration.
 */
void tm_config_free(tm_config_t *cfg);

/* ============================================================================
 * Main Analysis API
 * ========================================================================== */

/**
 * Opaque analyzer context.
 */
typedef struct tm_analyzer tm_analyzer_t;

/**
 * Create a new analyzer instance.
 */
tm_analyzer_t *tm_analyzer_new(tm_config_t *cfg);

/**
 * Analyze a stack trace and produce hypotheses.
 * 
 * @param analyzer  The analyzer instance
 * @param input     Input (file path, "-" for stdin, or raw trace)
 * @return          Analysis result (caller owns), or NULL on failure
 */
tm_analysis_result_t *tm_analyze(tm_analyzer_t *analyzer, const char *input);

/**
 * Convenience function for one-shot analysis with default config.
 */
tm_analysis_result_t *tm_analyze_quick(const char *input);

/**
 * Free analyzer instance.
 */
void tm_analyzer_free(tm_analyzer_t *analyzer);

/**
 * Progress callback function type.
 */
typedef void (*tm_progress_cb)(const char *stage, float progress, void *ctx);

/**
 * Set progress callback for the analyzer.
 */
void tm_analyzer_set_progress_callback(tm_analyzer_t *analyzer,
                                       tm_progress_cb callback,
                                       void *ctx);

/* ============================================================================
 * Result Management
 * ========================================================================== */

/**
 * Free analysis result and all owned resources.
 */
void tm_result_free(tm_analysis_result_t *result);

/**
 * Format result for output.
 * Returns allocated string (caller must free).
 */
char *tm_format_result(tm_analyzer_t *analyzer, tm_analysis_result_t *result);

/**
 * Print result to stdout.
 */
void tm_print_result(tm_analyzer_t *analyzer, tm_analysis_result_t *result);

/* ============================================================================
 * Stack Trace API (for direct use)
 * ========================================================================== */

/**
 * Parse a raw stack trace string.
 * Automatically detects the language.
 */
tm_stack_trace_t *tm_parse_stack_trace(const char *input, size_t len);

/**
 * Detect trace language from input.
 */
tm_language_t tm_detect_trace_language(const char *input, size_t len);

/**
 * Free a stack trace.
 */
void tm_stack_trace_free(tm_stack_trace_t *trace);

/* ============================================================================
 * Call Graph API (for direct use)
 * ========================================================================== */

/**
 * Free a call graph.
 */
void tm_call_graph_free(tm_call_graph_t *graph);

/* ============================================================================
 * Git Context API (for direct use)
 * ========================================================================== */

/**
 * Collect git context for relevant files.
 */
tm_git_context_t *tm_git_collect_context(const char *repo_path,
                                         const char **files,
                                         size_t file_count,
                                         int max_commits);

/**
 * Free git context.
 */
void tm_git_context_free(tm_git_context_t *ctx);

/* ============================================================================
 * Hypothesis API
 * ========================================================================== */

/**
 * Free a hypothesis.
 */
void tm_hypothesis_free(tm_hypothesis_t *h);

/* ============================================================================
 * Utility Functions
 * ========================================================================== */

/**
 * Read entire file into string.
 * Caller must free returned string.
 */
char *tm_read_file(const char *path, size_t *size);

/**
 * Generate a UUID v4 string.
 * Caller must free returned string.
 */
char *tm_generate_uuid(void);

/**
 * Get current timestamp in milliseconds.
 */
int64_t tm_timestamp_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* TRACEMIND_H */
