/**
 * TraceMind - AST Analysis & Call Graph Builder
 * 
 * Uses Tree-sitter for language-agnostic AST parsing.
 * If HAVE_TREE_SITTER is not defined, uses stub implementations.
 */

#ifndef TM_INTERNAL_AST_H
#define TM_INTERNAL_AST_H

#include "tracemind.h"

#ifdef HAVE_TREE_SITTER
#include <tree_sitter/api.h>
#else
/* Stub types when Tree-sitter is not available */
typedef void TSLanguage;
typedef void TSTree;
typedef void TSParser;
typedef struct { uint32_t id[4]; void *tree; } TSNode;
typedef struct { uint32_t row; uint32_t column; } TSPoint;
#endif

/* ============================================================================
 * Tree-sitter Grammar Management
 * ========================================================================== */

/**
 * Get Tree-sitter language for a TraceMind language.
 */
const TSLanguage *tm_ts_language(tm_language_t lang);

/**
 * Initialize Tree-sitter parsers for all supported languages.
 * Call once at startup.
 */
tm_error_t tm_ast_init(void);

/**
 * Cleanup Tree-sitter resources.
 */
void tm_ast_cleanup(void);

/* ============================================================================
 * Source File Parsing
 * ========================================================================== */

/**
 * Parsed source file context.
 */
typedef struct {
    char *path;               /* File path */
    char *source;             /* Source content */
    size_t source_len;        /* Source length */
    TSTree *tree;             /* Parsed AST */
    TSParser *parser;         /* Parser instance */
    tm_language_t language;   /* Detected language */
} tm_source_file_t;

/**
 * Parse a source file.
 */
tm_error_t tm_parse_source_file(const char *path, tm_source_file_t **file);

/**
 * Free parsed source file.
 */
void tm_source_file_free(tm_source_file_t *file);

/* ============================================================================
 * Function Extraction
 * ========================================================================== */

/**
 * Extracted function definition.
 */
typedef struct {
    char *name;               /* Function name */
    char *qualified_name;     /* Fully qualified name (module.class.func) */
    char *signature;          /* Parameter signature */
    int start_line;           /* Start line (1-indexed) */
    int end_line;             /* End line (1-indexed) */
    int start_col;            /* Start column */
    int end_col;              /* End column */
    TSNode node;              /* AST node */
} tm_function_def_t;

/**
 * Extract all function definitions from a source file.
 */
tm_error_t tm_extract_functions(const tm_source_file_t *file,
                                tm_function_def_t ***funcs,
                                size_t *count);

/**
 * Free function definitions array.
 */
void tm_functions_free(tm_function_def_t *funcs, size_t count);

/**
 * Find function definition by name in a file.
 */
tm_error_t tm_find_function(const tm_source_file_t *file,
                            const char *name,
                            tm_function_def_t **func);

/**
 * Find function definition by line number.
 */
tm_error_t tm_find_function_at_line(const tm_source_file_t *file,
                                    int line,
                                    tm_function_def_t **func);

/* ============================================================================
 * Call Site Extraction
 * ========================================================================== */

/**
 * A function call site.
 */
typedef struct {
    char *callee_name;        /* Name of called function */
    int line;                 /* Line number of call */
    int column;               /* Column of call */
    TSNode node;              /* AST node */
} tm_call_site_t;

/**
 * Extract all call sites within a function.
 */
tm_error_t tm_extract_call_sites(const tm_source_file_t *file,
                                 const tm_function_def_t *func,
                                 tm_call_site_t ***sites,
                                 size_t *count);

/**
 * Free call sites array.
 */
void tm_call_sites_free(tm_call_site_t *sites, size_t count);

/* ============================================================================
 * Call Graph Construction
 * ========================================================================== */

/**
 * Context for call graph building.
 */
typedef struct {
    char *repo_path;                  /* Repository root */
    tm_source_file_t **files;         /* Parsed files cache */
    size_t file_count;
    size_t file_capacity;
    int max_depth;                    /* Maximum traversal depth */
    bool include_stdlib;              /* Include stdlib functions */
    bool include_tests;               /* Include test files */
} tm_graph_builder_t;

/* Simplified AST builder for analyzer */
typedef tm_graph_builder_t tm_ast_builder_t;

/**
 * Create a new graph builder.
 */
tm_ast_builder_t *tm_ast_builder_new(void);

/**
 * Free graph builder.
 */
void tm_ast_builder_free(tm_ast_builder_t *builder);

/**
 * Add a file to the AST builder for analysis.
 */
tm_error_t tm_ast_add_file(tm_ast_builder_t *builder, const char *path);

/**
 * Build call graph from an entry function.
 */
tm_call_graph_t *tm_ast_build_call_graph(tm_ast_builder_t *builder,
                                         const char *entry_function,
                                         int max_depth);

/**
 * Legacy graph builder constructor.
 */
tm_graph_builder_t *tm_graph_builder_new(const char *repo_path, int max_depth);

/**
 * Free graph builder.
 */
void tm_graph_builder_free(tm_graph_builder_t *builder);

/**
 * Get or parse a source file (caches result).
 */
tm_error_t tm_graph_builder_get_file(tm_graph_builder_t *builder,
                                     const char *path,
                                     tm_source_file_t **file);

/**
 * Build call graph starting from stack trace frames.
 */
tm_error_t tm_graph_builder_build(tm_graph_builder_t *builder,
                                  const tm_stack_trace_t *trace,
                                  tm_call_graph_t **graph);

/* ============================================================================
 * Call Node Management
 * ========================================================================== */

/**
 * Create a new call node.
 */
tm_call_node_t *tm_call_node_new(const char *name,
                                 const char *file,
                                 int start_line,
                                 int end_line);

/**
 * Free a call node (including callees/callers arrays, but not the nodes themselves).
 */
void tm_call_node_free(tm_call_node_t *node);

/**
 * Add caller to node.
 */
tm_error_t tm_call_node_add_caller(tm_call_node_t *node, tm_call_node_t *caller);

/**
 * Add callee to node.
 */
tm_error_t tm_call_node_add_callee(tm_call_node_t *node, tm_call_node_t *callee);

/* ============================================================================
 * Language-Specific AST Helpers
 * ========================================================================== */

/**
 * Get Tree-sitter query for function definitions.
 */
const char *tm_query_function_defs(tm_language_t lang);

/**
 * Get Tree-sitter query for function calls.
 */
const char *tm_query_function_calls(tm_language_t lang);

/**
 * Get Tree-sitter query for imports.
 */
const char *tm_query_imports(tm_language_t lang);

/* ============================================================================
 * Complexity Analysis
 * ========================================================================== */

/**
 * Compute cyclomatic complexity for a function.
 */
uint32_t tm_compute_complexity(const tm_source_file_t *file,
                               const tm_function_def_t *func);

#endif /* TM_INTERNAL_AST_H */
