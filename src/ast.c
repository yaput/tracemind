/**
 * TraceMind - AST Analysis & Call Graph Builder
 * 
 * Uses Tree-sitter for language-agnostic AST parsing.
 * Compiles as stub when HAVE_TREE_SITTER is not defined.
 */

#include "internal/common.h"
#include "internal/ast.h"
#include "internal/parser.h"

#ifdef HAVE_TREE_SITTER
#include <tree_sitter/api.h>

/* External Tree-sitter language declarations */
extern const TSLanguage *tree_sitter_python(void);
extern const TSLanguage *tree_sitter_go(void);
extern const TSLanguage *tree_sitter_javascript(void);
extern const TSLanguage *tree_sitter_typescript(void);
#endif /* HAVE_TREE_SITTER */

/* ============================================================================
 * Initialization
 * ========================================================================== */

static bool g_ast_initialized = false;

tm_error_t tm_ast_init(void)
{
    if (g_ast_initialized) return TM_OK;
    
    g_ast_initialized = true;
    TM_DEBUG("AST module initialized");
    return TM_OK;
}

void tm_ast_cleanup(void)
{
    g_ast_initialized = false;
    TM_DEBUG("AST module cleaned up");
}

#ifdef HAVE_TREE_SITTER
/* ============================================================================
 * Tree-sitter Language Mapping
 * ========================================================================== */

const TSLanguage *tm_ts_language(tm_language_t lang)
{
    switch (lang) {
    case TM_LANG_PYTHON:
        return tree_sitter_python();
    case TM_LANG_GO:
        return tree_sitter_go();
    case TM_LANG_NODEJS:
        return tree_sitter_javascript();  /* TypeScript uses JS parser for basics */
    default:
        return NULL;
    }
}

/* ============================================================================
 * Source File Parsing
 * ========================================================================== */

tm_error_t tm_parse_source_file(const char *path, tm_source_file_t **result)
{
    TM_CHECK_NULL(path, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(result, TM_ERR_INVALID_ARG);
    
    *result = NULL;
    
    /* Read file content */
    size_t source_len = 0;
    char *source = tm_read_file(path, &source_len);
    if (!source) {
        TM_ERROR("Failed to read source file: %s", path);
        return TM_ERR_IO;
    }
    
    /* Detect language */
    tm_language_t lang = tm_detect_language(path);
    if (lang == TM_LANG_UNKNOWN) {
        /* Try detecting from content */
        lang = tm_detect_language(source);
    }
    
    if (lang == TM_LANG_UNKNOWN) {
        TM_WARN("Could not detect language for: %s", path);
        TM_FREE(source);
        return TM_ERR_UNSUPPORTED;
    }
    
    /* Get Tree-sitter language */
    const TSLanguage *ts_lang = tm_ts_language(lang);
    if (!ts_lang) {
        TM_ERROR("No Tree-sitter grammar for: %s", tm_language_name(lang));
        TM_FREE(source);
        return TM_ERR_UNSUPPORTED;
    }
    
    /* Create parser */
    TSParser *parser = ts_parser_new();
    if (!ts_parser_set_language(parser, ts_lang)) {
        TM_ERROR("Failed to set Tree-sitter language");
        ts_parser_delete(parser);
        TM_FREE(source);
        return TM_ERR_INTERNAL;
    }
    
    /* Parse source */
    TSTree *tree = ts_parser_parse_string(parser, NULL, source, (uint32_t)source_len);
    if (!tree) {
        TM_ERROR("Tree-sitter parsing failed for: %s", path);
        ts_parser_delete(parser);
        TM_FREE(source);
        return TM_ERR_PARSE;
    }
    
    /* Build result */
    tm_source_file_t *file = tm_calloc(1, sizeof(tm_source_file_t));
    file->path = tm_strdup(path);
    file->source = source;
    file->source_len = source_len;
    file->tree = tree;
    file->parser = parser;
    file->language = lang;
    
    *result = file;
    TM_DEBUG("Parsed source file: %s (%zu bytes, %s)", 
             path, source_len, tm_language_name(lang));
    
    return TM_OK;
}

void tm_source_file_free(tm_source_file_t *file)
{
    if (!file) return;
    
    if (file->tree) ts_tree_delete(file->tree);
    if (file->parser) ts_parser_delete(file->parser);
    TM_FREE(file->path);
    TM_FREE(file->source);
    free(file);
}

/* ============================================================================
 * Tree-sitter Query Strings
 * ========================================================================== */

const char *tm_query_function_defs(tm_language_t lang)
{
    switch (lang) {
    case TM_LANG_PYTHON:
        return "(function_definition"
               "  name: (identifier) @name"
               "  parameters: (parameters) @params"
               ") @func";
    case TM_LANG_GO:
        return "(function_declaration"
               "  name: (identifier) @name"
               "  parameters: (parameter_list) @params"
               ") @func";
    case TM_LANG_NODEJS:
        return "["
               "  (function_declaration"
               "    name: (identifier) @name"
               "    parameters: (formal_parameters) @params"
               "  ) @func"
               "  (method_definition"
               "    name: (property_identifier) @name"
               "    parameters: (formal_parameters) @params"
               "  ) @func"
               "  (arrow_function"
               "    parameters: (formal_parameters) @params"
               "  ) @func"
               "]";
    default:
        return NULL;
    }
}

const char *tm_query_function_calls(tm_language_t lang)
{
    switch (lang) {
    case TM_LANG_PYTHON:
        return "(call"
               "  function: [(identifier) @name"
               "             (attribute attribute: (identifier) @name)]"
               ") @call";
    case TM_LANG_GO:
        return "(call_expression"
               "  function: [(identifier) @name"
               "             (selector_expression field: (field_identifier) @name)]"
               ") @call";
    case TM_LANG_NODEJS:
        return "(call_expression"
               "  function: [(identifier) @name"
               "             (member_expression property: (property_identifier) @name)]"
               ") @call";
    default:
        return NULL;
    }
}

const char *tm_query_imports(tm_language_t lang)
{
    switch (lang) {
    case TM_LANG_PYTHON:
        return "["
               "  (import_statement) @import"
               "  (import_from_statement) @import"
               "]";
    case TM_LANG_GO:
        return "(import_declaration) @import";
    case TM_LANG_NODEJS:
        return "["
               "  (import_statement) @import"
               "  (call_expression"
               "    function: (identifier) @func (#eq? @func \"require\")"
               "  ) @import"
               "]";
    default:
        return NULL;
    }
}

/* ============================================================================
 * Function Extraction
 * ========================================================================== */

/**
 * Helper to get text from a TSNode.
 */
static char *node_text(const tm_source_file_t *file, TSNode node)
{
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    
    if (end <= start || end > file->source_len) return NULL;
    
    return tm_strndup(file->source + start, end - start);
}

/**
 * Recursive function finder using manual traversal.
 */
static void find_functions_recursive(const tm_source_file_t *file,
                                     TSNode node,
                                     tm_function_def_t **funcs,
                                     size_t *count,
                                     size_t *capacity)
{
    const char *type = ts_node_type(node);
    
    /* Check if this node is a function definition */
    bool is_func = false;
    const char *name_field = "name";
    
    switch (file->language) {
    case TM_LANG_PYTHON:
        is_func = strcmp(type, "function_definition") == 0;
        break;
    case TM_LANG_GO:
        is_func = strcmp(type, "function_declaration") == 0 ||
                  strcmp(type, "method_declaration") == 0;
        break;
    case TM_LANG_NODEJS:
        is_func = strcmp(type, "function_declaration") == 0 ||
                  strcmp(type, "method_definition") == 0;
        if (strcmp(type, "method_definition") == 0) {
            name_field = "name";
        }
        break;
    default:
        break;
    }
    
    if (is_func) {
        TSNode name_node = ts_node_child_by_field_name(node, name_field, strlen(name_field));
        
        if (!ts_node_is_null(name_node)) {
            /* Ensure capacity */
            if (*count >= *capacity) {
                *capacity = *capacity == 0 ? 16 : *capacity * 2;
                *funcs = tm_realloc(*funcs, *capacity * sizeof(tm_function_def_t));
            }
            
            tm_function_def_t *func = &(*funcs)[*count];
            memset(func, 0, sizeof(tm_function_def_t));
            
            func->name = node_text(file, name_node);
            func->qualified_name = tm_strdup(func->name);  /* TODO: Add module prefix */
            
            TSPoint start = ts_node_start_point(node);
            TSPoint end = ts_node_end_point(node);
            
            func->start_line = (int)start.row + 1;
            func->end_line = (int)end.row + 1;
            func->start_col = (int)start.column;
            func->end_col = (int)end.column;
            func->node = node;
            
            /* Extract signature */
            TSNode params_node = ts_node_child_by_field_name(node, "parameters", 10);
            if (!ts_node_is_null(params_node)) {
                char *params = node_text(file, params_node);
                size_t sig_len = strlen(func->name) + (params ? strlen(params) : 2) + 1;
                func->signature = tm_malloc(sig_len);
                snprintf(func->signature, sig_len, "%s%s", func->name, params ? params : "()");
                TM_FREE(params);
            } else {
                func->signature = tm_strdup(func->name);
            }
            
            (*count)++;
        }
    }
    
    /* Recurse into children */
    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(node, i);
        find_functions_recursive(file, child, funcs, count, capacity);
    }
}

tm_error_t tm_extract_functions(const tm_source_file_t *file,
                                tm_function_def_t **funcs,
                                size_t *count)
{
    TM_CHECK_NULL(file, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(funcs, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(count, TM_ERR_INVALID_ARG);
    
    *funcs = NULL;
    *count = 0;
    size_t capacity = 0;
    
    TSNode root = ts_tree_root_node(file->tree);
    find_functions_recursive(file, root, funcs, count, &capacity);
    
    TM_DEBUG("Extracted %zu functions from %s", *count, file->path);
    return TM_OK;
}

void tm_functions_free(tm_function_def_t *funcs, size_t count)
{
    if (!funcs) return;
    
    for (size_t i = 0; i < count; i++) {
        TM_FREE(funcs[i].name);
        TM_FREE(funcs[i].qualified_name);
        TM_FREE(funcs[i].signature);
    }
    free(funcs);
}

tm_error_t tm_find_function(const tm_source_file_t *file,
                            const char *name,
                            tm_function_def_t **result)
{
    TM_CHECK_NULL(file, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(name, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(result, TM_ERR_INVALID_ARG);
    
    *result = NULL;
    
    tm_function_def_t *funcs = NULL;
    size_t count = 0;
    
    tm_error_t err = tm_extract_functions(file, &funcs, &count);
    if (err != TM_OK) return err;
    
    for (size_t i = 0; i < count; i++) {
        if (strcmp(funcs[i].name, name) == 0) {
            /* Copy the found function */
            *result = tm_malloc(sizeof(tm_function_def_t));
            **result = funcs[i];
            (*result)->name = tm_strdup(funcs[i].name);
            (*result)->qualified_name = tm_strdup(funcs[i].qualified_name);
            (*result)->signature = tm_strdup(funcs[i].signature);
            
            /* Free the rest */
            for (size_t j = 0; j < count; j++) {
                if (j != i) {
                    TM_FREE(funcs[j].name);
                    TM_FREE(funcs[j].qualified_name);
                    TM_FREE(funcs[j].signature);
                }
            }
            free(funcs);
            return TM_OK;
        }
    }
    
    tm_functions_free(funcs, count);
    return TM_ERR_NOT_FOUND;
}

tm_error_t tm_find_function_at_line(const tm_source_file_t *file,
                                    int line,
                                    tm_function_def_t **result)
{
    TM_CHECK_NULL(file, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(result, TM_ERR_INVALID_ARG);
    
    *result = NULL;
    
    tm_function_def_t *funcs = NULL;
    size_t count = 0;
    
    tm_error_t err = tm_extract_functions(file, &funcs, &count);
    if (err != TM_OK) return err;
    
    /* Find the innermost function containing the line */
    tm_function_def_t *best = NULL;
    int best_span = INT_MAX;
    
    for (size_t i = 0; i < count; i++) {
        if (line >= funcs[i].start_line && line <= funcs[i].end_line) {
            int span = funcs[i].end_line - funcs[i].start_line;
            if (span < best_span) {
                best = &funcs[i];
                best_span = span;
            }
        }
    }
    
    if (best) {
        *result = tm_malloc(sizeof(tm_function_def_t));
        **result = *best;
        (*result)->name = tm_strdup(best->name);
        (*result)->qualified_name = tm_strdup(best->qualified_name);
        (*result)->signature = tm_strdup(best->signature);
    }
    
    tm_functions_free(funcs, count);
    return best ? TM_OK : TM_ERR_NOT_FOUND;
}

/* ============================================================================
 * Call Site Extraction
 * ========================================================================== */

static void find_calls_in_range(const tm_source_file_t *file,
                                TSNode node,
                                int start_line,
                                int end_line,
                                tm_call_site_t **sites,
                                size_t *count,
                                size_t *capacity)
{
    TSPoint point = ts_node_start_point(node);
    int line = (int)point.row + 1;
    
    /* Skip nodes outside our range */
    if (line < start_line) {
        TSPoint end_point = ts_node_end_point(node);
        if ((int)end_point.row + 1 < start_line) return;
    }
    if (line > end_line) return;
    
    const char *type = ts_node_type(node);
    
    /* Check for call expression */
    bool is_call = false;
    switch (file->language) {
    case TM_LANG_PYTHON:
        is_call = strcmp(type, "call") == 0;
        break;
    case TM_LANG_GO:
    case TM_LANG_NODEJS:
        is_call = strcmp(type, "call_expression") == 0;
        break;
    default:
        break;
    }
    
    if (is_call && line >= start_line && line <= end_line) {
        TSNode func_node = ts_node_child_by_field_name(node, "function", 8);
        if (!ts_node_is_null(func_node)) {
            /* Get the actual function name */
            const char *func_type = ts_node_type(func_node);
            char *callee_name = NULL;
            
            if (strcmp(func_type, "identifier") == 0) {
                callee_name = node_text(file, func_node);
            } else if (strcmp(func_type, "attribute") == 0 ||
                       strcmp(func_type, "member_expression") == 0 ||
                       strcmp(func_type, "selector_expression") == 0) {
                /* Get the last part (method name) */
                uint32_t child_count = ts_node_child_count(func_node);
                if (child_count > 0) {
                    TSNode last = ts_node_child(func_node, child_count - 1);
                    callee_name = node_text(file, last);
                }
            }
            
            if (callee_name) {
                /* Ensure capacity */
                if (*count >= *capacity) {
                    *capacity = *capacity == 0 ? 16 : *capacity * 2;
                    *sites = tm_realloc(*sites, *capacity * sizeof(tm_call_site_t));
                }
                
                tm_call_site_t *site = &(*sites)[*count];
                site->callee_name = callee_name;
                site->line = line;
                site->column = (int)point.column;
                site->node = node;
                
                (*count)++;
            }
        }
    }
    
    /* Recurse */
    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(node, i);
        find_calls_in_range(file, child, start_line, end_line, sites, count, capacity);
    }
}

tm_error_t tm_extract_call_sites(const tm_source_file_t *file,
                                 const tm_function_def_t *func,
                                 tm_call_site_t **sites,
                                 size_t *count)
{
    TM_CHECK_NULL(file, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(func, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(sites, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(count, TM_ERR_INVALID_ARG);
    
    *sites = NULL;
    *count = 0;
    size_t capacity = 0;
    
    TSNode root = ts_tree_root_node(file->tree);
    find_calls_in_range(file, root, func->start_line, func->end_line,
                        sites, count, &capacity);
    
    TM_DEBUG("Found %zu call sites in function %s", *count, func->name);
    return TM_OK;
}

void tm_call_sites_free(tm_call_site_t *sites, size_t count)
{
    if (!sites) return;
    
    for (size_t i = 0; i < count; i++) {
        TM_FREE(sites[i].callee_name);
    }
    free(sites);
}

/* ============================================================================
 * Call Graph Builder
 * ========================================================================== */

tm_graph_builder_t *tm_graph_builder_new(const char *repo_path, int max_depth)
{
    tm_graph_builder_t *builder = tm_calloc(1, sizeof(tm_graph_builder_t));
    
    builder->repo_path = tm_strdup(repo_path);
    builder->files = NULL;
    builder->file_count = 0;
    builder->file_capacity = 0;
    builder->max_depth = max_depth > 0 ? max_depth : 5;
    builder->include_stdlib = false;
    builder->include_tests = false;
    
    return builder;
}

void tm_graph_builder_free(tm_graph_builder_t *builder)
{
    if (!builder) return;
    
    for (size_t i = 0; i < builder->file_count; i++) {
        tm_source_file_free(builder->files[i]);
    }
    TM_FREE(builder->files);
    TM_FREE(builder->repo_path);
    free(builder);
}

tm_error_t tm_graph_builder_get_file(tm_graph_builder_t *builder,
                                     const char *path,
                                     tm_source_file_t **file)
{
    TM_CHECK_NULL(builder, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(path, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(file, TM_ERR_INVALID_ARG);
    
    /* Check cache */
    for (size_t i = 0; i < builder->file_count; i++) {
        if (strcmp(builder->files[i]->path, path) == 0) {
            *file = builder->files[i];
            return TM_OK;
        }
    }
    
    /* Parse new file */
    tm_source_file_t *new_file = NULL;
    tm_error_t err = tm_parse_source_file(path, &new_file);
    if (err != TM_OK) return err;
    
    /* Add to cache */
    if (builder->file_count >= builder->file_capacity) {
        builder->file_capacity = builder->file_capacity == 0 ? 8 : builder->file_capacity * 2;
        builder->files = tm_realloc(builder->files, 
                                    builder->file_capacity * sizeof(tm_source_file_t *));
    }
    builder->files[builder->file_count++] = new_file;
    
    *file = new_file;
    return TM_OK;
}

/* ============================================================================
 * Call Node Management
 * ========================================================================== */

tm_call_node_t *tm_call_node_new(const char *name,
                                 const char *file,
                                 int start_line,
                                 int end_line)
{
    tm_call_node_t *node = tm_calloc(1, sizeof(tm_call_node_t));
    
    node->name = name ? tm_strdup(name) : NULL;
    node->file = file ? tm_strdup(file) : NULL;
    node->start_line = start_line;
    node->end_line = end_line;
    node->signature = NULL;
    node->callers = NULL;
    node->caller_count = 0;
    node->callees = NULL;
    node->callee_count = 0;
    node->complexity = 0;
    
    return node;
}

void tm_call_node_free(tm_call_node_t *node)
{
    if (!node) return;
    
    TM_FREE(node->name);
    TM_FREE(node->file);
    TM_FREE(node->signature);
    TM_FREE(node->callers);
    TM_FREE(node->callees);
    free(node);
}

tm_error_t tm_call_node_add_caller(tm_call_node_t *node, tm_call_node_t *caller)
{
    TM_CHECK_NULL(node, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(caller, TM_ERR_INVALID_ARG);
    
    /* Check for duplicate */
    for (size_t i = 0; i < node->caller_count; i++) {
        if (node->callers[i] == caller) return TM_OK;
    }
    
    size_t new_count = node->caller_count + 1;
    node->callers = tm_realloc(node->callers, new_count * sizeof(tm_call_node_t *));
    node->callers[node->caller_count++] = caller;
    
    return TM_OK;
}

tm_error_t tm_call_node_add_callee(tm_call_node_t *node, tm_call_node_t *callee)
{
    TM_CHECK_NULL(node, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(callee, TM_ERR_INVALID_ARG);
    
    /* Check for duplicate */
    for (size_t i = 0; i < node->callee_count; i++) {
        if (node->callees[i] == callee) return TM_OK;
    }
    
    size_t new_count = node->callee_count + 1;
    node->callees = tm_realloc(node->callees, new_count * sizeof(tm_call_node_t *));
    node->callees[node->callee_count++] = callee;
    
    return TM_OK;
}

/* ============================================================================
 * Call Graph Construction
 * ========================================================================== */

void tm_call_graph_free(tm_call_graph_t *graph)
{
    if (!graph) return;
    
    for (size_t i = 0; i < graph->node_count; i++) {
        tm_call_node_free(graph->nodes[i]);
    }
    TM_FREE(graph->nodes);
    free(graph);
}

tm_error_t tm_graph_builder_build(tm_graph_builder_t *builder,
                                  const tm_stack_trace_t *trace,
                                  tm_call_graph_t **result)
{
    TM_CHECK_NULL(builder, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(trace, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(result, TM_ERR_INVALID_ARG);
    
    *result = NULL;
    
    tm_call_graph_t *graph = tm_calloc(1, sizeof(tm_call_graph_t));
    graph->nodes = NULL;
    graph->node_count = 0;
    graph->node_capacity = 0;
    graph->entry_point = NULL;
    
    /* Process each frame in the stack trace */
    for (size_t i = 0; i < trace->frame_count; i++) {
        const tm_stack_frame_t *frame = &trace->frames[i];
        
        /* Skip stdlib and third-party if configured */
        if (!builder->include_stdlib && frame->is_stdlib) continue;
        if (frame->is_third_party) continue;
        if (!frame->file) continue;
        
        /* Build full path */
        char full_path[PATH_MAX];
        if (frame->file[0] == '/') {
            strncpy(full_path, frame->file, PATH_MAX - 1);
        } else {
            snprintf(full_path, PATH_MAX, "%s/%s", builder->repo_path, frame->file);
        }
        full_path[PATH_MAX - 1] = '\0';
        
        /* Try to get/parse the source file */
        tm_source_file_t *src_file = NULL;
        if (tm_graph_builder_get_file(builder, full_path, &src_file) != TM_OK) {
            TM_DEBUG("Skipping unavailable file: %s", full_path);
            continue;
        }
        
        /* Find the function at this frame's line */
        tm_function_def_t *func = NULL;
        if (frame->function && tm_find_function(src_file, frame->function, &func) != TM_OK) {
            /* Try by line number */
            tm_find_function_at_line(src_file, frame->line, &func);
        }
        
        if (!func) {
            TM_DEBUG("Could not find function for frame: %s:%d", 
                     frame->file, frame->line);
            continue;
        }
        
        /* Create call node */
        tm_call_node_t *node = tm_call_node_new(func->name, frame->file,
                                                func->start_line, func->end_line);
        node->signature = tm_strdup(func->signature);
        
        /* Add to graph */
        if (graph->node_count >= graph->node_capacity) {
            graph->node_capacity = graph->node_capacity == 0 ? 16 : graph->node_capacity * 2;
            graph->nodes = tm_realloc(graph->nodes, 
                                      graph->node_capacity * sizeof(tm_call_node_t *));
        }
        graph->nodes[graph->node_count++] = node;
        
        /* First node is the entry point (error location) */
        if (i == 0 || graph->entry_point == NULL) {
            graph->entry_point = node;
        }
        
        /* Link to previous node (caller -> callee relationship) */
        if (graph->node_count >= 2) {
            tm_call_node_t *prev = graph->nodes[graph->node_count - 2];
            tm_call_node_add_callee(prev, node);
            tm_call_node_add_caller(node, prev);
        }
        
        /* Free function def */
        TM_FREE(func->name);
        TM_FREE(func->qualified_name);
        TM_FREE(func->signature);
        free(func);
    }
    
    TM_DEBUG("Built call graph with %zu nodes", graph->node_count);
    *result = graph;
    return TM_OK;
}

tm_error_t tm_build_call_graph(const tm_stack_trace_t *trace,
                               const char *repo_path,
                               int max_depth,
                               tm_call_graph_t **graph)
{
    TM_CHECK_NULL(trace, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(graph, TM_ERR_INVALID_ARG);
    
    const char *path = repo_path ? repo_path : ".";
    
    tm_graph_builder_t *builder = tm_graph_builder_new(path, max_depth);
    tm_error_t err = tm_graph_builder_build(builder, trace, graph);
    tm_graph_builder_free(builder);
    
    return err;
}

/* ============================================================================
 * Complexity Analysis
 * ========================================================================== */

static uint32_t count_complexity_nodes(const tm_source_file_t *file,
                                       TSNode node,
                                       int start_line,
                                       int end_line)
{
    TSPoint point = ts_node_start_point(node);
    int line = (int)point.row + 1;
    
    if (line < start_line || line > end_line) return 0;
    
    uint32_t complexity = 0;
    const char *type = ts_node_type(node);
    
    /* Count decision points */
    if (strcmp(type, "if_statement") == 0 ||
        strcmp(type, "elif_clause") == 0 ||
        strcmp(type, "for_statement") == 0 ||
        strcmp(type, "while_statement") == 0 ||
        strcmp(type, "for_in_statement") == 0 ||
        strcmp(type, "try_statement") == 0 ||
        strcmp(type, "except_clause") == 0 ||
        strcmp(type, "case_clause") == 0 ||
        strcmp(type, "switch_statement") == 0 ||
        strcmp(type, "conditional_expression") == 0 ||
        strcmp(type, "ternary_expression") == 0 ||
        strcmp(type, "and_expression") == 0 ||
        strcmp(type, "or_expression") == 0 ||
        strcmp(type, "&&") == 0 ||
        strcmp(type, "||") == 0) {
        complexity++;
    }
    
    /* Recurse */
    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; i++) {
        complexity += count_complexity_nodes(file, ts_node_child(node, i),
                                             start_line, end_line);
    }
    
    return complexity;
}

uint32_t tm_compute_complexity(const tm_source_file_t *file,
                               const tm_function_def_t *func)
{
    if (!file || !func) return 0;
    
    TSNode root = ts_tree_root_node(file->tree);
    
    /* Base complexity is 1 */
    return 1 + count_complexity_nodes(file, root, func->start_line, func->end_line);
}

#else /* !HAVE_TREE_SITTER - Stub implementations */

/* ============================================================================
 * Stub Implementations (No Tree-sitter Available)
 * ========================================================================== */

const TSLanguage *tm_ts_language(tm_language_t lang)
{
    (void)lang;
    return NULL;
}

tm_error_t tm_parse_source_file(const char *path, tm_source_file_t **result)
{
    (void)path;
    *result = NULL;
    TM_DEBUG("AST analysis unavailable (no tree-sitter)");
    return TM_ERR_UNSUPPORTED;
}

void tm_source_file_free(tm_source_file_t *file)
{
    if (file) {
        TM_FREE(file->path);
        TM_FREE(file->source);
        free(file);
    }
}

tm_error_t tm_extract_functions(const tm_source_file_t *file,
                                tm_function_def_t ***funcs,
                                size_t *count)
{
    (void)file;
    *funcs = NULL;
    *count = 0;
    return TM_ERR_UNSUPPORTED;
}

tm_error_t tm_find_function(const tm_source_file_t *file,
                            const char *name,
                            tm_function_def_t **func)
{
    (void)file;
    (void)name;
    *func = NULL;
    return TM_ERR_NOT_FOUND;
}

tm_error_t tm_find_function_at_line(const tm_source_file_t *file,
                                    int line,
                                    tm_function_def_t **func)
{
    (void)file;
    (void)line;
    *func = NULL;
    return TM_ERR_NOT_FOUND;
}

tm_error_t tm_extract_call_sites(const tm_source_file_t *file,
                                 const tm_function_def_t *func,
                                 tm_call_site_t ***sites,
                                 size_t *count)
{
    (void)file;
    (void)func;
    *sites = NULL;
    *count = 0;
    return TM_ERR_UNSUPPORTED;
}

void tm_call_site_free(tm_call_site_t *site)
{
    if (site) {
        TM_FREE(site->callee_name);
        free(site);
    }
}

tm_ast_builder_t *tm_ast_builder_new(void)
{
    TM_DEBUG("AST builder unavailable (no tree-sitter)");
    return NULL;
}

void tm_ast_builder_free(tm_ast_builder_t *builder)
{
    (void)builder;
}

tm_error_t tm_ast_add_file(tm_ast_builder_t *builder, const char *path)
{
    (void)builder;
    (void)path;
    return TM_ERR_UNSUPPORTED;
}

tm_call_graph_t *tm_ast_build_call_graph(tm_ast_builder_t *builder,
                                         const char *entry_function,
                                         int max_depth)
{
    (void)builder;
    (void)entry_function;
    (void)max_depth;
    TM_DEBUG("Call graph building unavailable (no tree-sitter)");
    return NULL;
}

void tm_call_graph_free(tm_call_graph_t *graph)
{
    if (!graph) return;
    
    for (size_t i = 0; i < graph->node_count; i++) {
        if (graph->nodes[i]) {
            TM_FREE(graph->nodes[i]->name);
            TM_FREE(graph->nodes[i]->file);
            TM_FREE(graph->nodes[i]->signature);
            TM_FREE(graph->nodes[i]->callers);
            TM_FREE(graph->nodes[i]->callees);
            free(graph->nodes[i]);
        }
    }
    TM_FREE(graph->nodes);
    free(graph);
}

tm_graph_builder_t *tm_graph_builder_new(const char *repo_path, int max_depth)
{
    (void)repo_path;
    (void)max_depth;
    return NULL;
}

void tm_graph_builder_free(tm_graph_builder_t *builder)
{
    (void)builder;
}

tm_error_t tm_graph_builder_build(tm_graph_builder_t *builder,
                                  const tm_stack_trace_t *trace,
                                  tm_call_graph_t **result)
{
    (void)builder;
    (void)trace;
    *result = NULL;
    return TM_ERR_UNSUPPORTED;
}

tm_error_t tm_build_call_graph(const tm_stack_trace_t *trace,
                               const char *repo_path,
                               int max_depth,
                               tm_call_graph_t **graph)
{
    (void)trace;
    (void)repo_path;
    (void)max_depth;
    *graph = NULL;
    return TM_ERR_UNSUPPORTED;
}

uint32_t tm_compute_complexity(const tm_source_file_t *file,
                               const tm_function_def_t *func)
{
    (void)file;
    (void)func;
    return 0;
}

#endif /* HAVE_TREE_SITTER */
