/**
 * TraceMind - Main Analysis Orchestrator
 * 
 * Coordinates the full analysis pipeline:
 * 1. Parse stack trace input
 * 2. Build call graph via AST analysis
 * 3. Collect git context
 * 4. Generate hypotheses via LLM
 * 5. Format and present results
 */

#include "internal/common.h"
#include "internal/parser.h"
#include "internal/input_format.h"
#include "internal/ast.h"
#include "internal/git.h"
#include "internal/llm.h"
#include "internal/output.h"
#include "tracemind.h"
#include <sys/time.h>

/* ============================================================================
 * Analyzer Context
 * ========================================================================== */

struct tm_analyzer {
    tm_config_t *config;
    tm_llm_client_t *llm;
    tm_formatter_t *formatter;
    
    /* Timing */
    struct timeval start_time;
    struct timeval end_time;
    
    /* Progress callback */
    tm_progress_cb progress_cb;
    void *progress_ctx;
};

/* ============================================================================
 * Progress Reporting
 * ========================================================================== */

static void report_progress(tm_analyzer_t *analyzer, const char *stage, float pct)
{
    if (analyzer->progress_cb) {
        analyzer->progress_cb(stage, pct, analyzer->progress_ctx);
    }
}

/* ============================================================================
 * Analyzer Lifecycle
 * ========================================================================== */

tm_analyzer_t *tm_analyzer_new(tm_config_t *config)
{
    if (!config) {
        TM_ERROR("Config is required");
        return NULL;
    }
    
    tm_analyzer_t *a = tm_calloc(1, sizeof(tm_analyzer_t));
    a->config = config;
    
    /* Create LLM client */
    a->llm = tm_llm_client_new(config);
    if (!a->llm) {
        TM_ERROR("Failed to create LLM client");
        free(a);
        return NULL;
    }
    
    /* Create output formatter */
    a->formatter = tm_formatter_new(config->output_format, config->color_output);
    if (!a->formatter) {
        TM_ERROR("Failed to create formatter");
        tm_llm_client_free(a->llm);
        free(a);
        return NULL;
    }
    
    return a;
}

void tm_analyzer_free(tm_analyzer_t *analyzer)
{
    if (!analyzer) return;
    
    tm_llm_client_free(analyzer->llm);
    tm_formatter_free(analyzer->formatter);
    free(analyzer);
}

void tm_analyzer_set_progress_callback(tm_analyzer_t *analyzer,
                                       tm_progress_cb callback,
                                       void *ctx)
{
    if (analyzer) {
        analyzer->progress_cb = callback;
        analyzer->progress_ctx = ctx;
    }
}

/* ============================================================================
 * Analysis Result Management
 * ========================================================================== */

static tm_analysis_result_t *result_new(void)
{
    tm_analysis_result_t *r = tm_calloc(1, sizeof(tm_analysis_result_t));
    r->hypothesis_count = 0;
    r->hypotheses = NULL;
    return r;
}

void tm_result_free(tm_analysis_result_t *result)
{
    if (!result) return;
    
    /* Free stack trace */
    if (result->trace) {
        tm_stack_trace_free(result->trace);
    }
    
    /* Free call graph */
    if (result->call_graph) {
        tm_call_graph_free(result->call_graph);
    }
    
    /* Free git context */
    if (result->git_ctx) {
        tm_git_context_free(result->git_ctx);
    }
    
    /* Free hypotheses */
    for (size_t i = 0; i < result->hypothesis_count; i++) {
        tm_hypothesis_free(result->hypotheses[i]);
    }
    TM_FREE(result->hypotheses);
    
    TM_FREE(result->error_message);
    free(result);
}

/* ============================================================================
 * Input Detection and Reading
 * ========================================================================== */

/**
 * Detect input type (file path, stdin marker, or raw trace)
 */
typedef enum {
    INPUT_FILE,
    INPUT_STDIN,
    INPUT_RAW
} input_type_t;

static input_type_t detect_input_type(const char *input)
{
    if (!input || strlen(input) == 0) return INPUT_STDIN;
    if (strcmp(input, "-") == 0) return INPUT_STDIN;
    
    /* Check if it's a file path */
    struct stat st;
    if (stat(input, &st) == 0 && S_ISREG(st.st_mode)) {
        return INPUT_FILE;
    }
    
    /* Check if it looks like a trace (has newlines, keywords) */
    if (strchr(input, '\n') != NULL) return INPUT_RAW;
    if (strstr(input, "Traceback") != NULL) return INPUT_RAW;
    if (strstr(input, "panic:") != NULL) return INPUT_RAW;
    if (strstr(input, "Error:") != NULL) return INPUT_RAW;
    
    return INPUT_FILE;  /* Assume file path */
}

/**
 * Read input from various sources
 */
static char *read_input(const char *input, size_t *size)
{
    input_type_t type = detect_input_type(input);
    
    switch (type) {
        case INPUT_STDIN: {
            TM_DEBUG("Reading from stdin");
            tm_strbuf_t buf;
            tm_strbuf_init(&buf);
            
            char line[4096];
            while (fgets(line, sizeof(line), stdin)) {
                tm_strbuf_append(&buf, line);
            }
            
            *size = buf.len;
            return buf.data;
        }
        
        case INPUT_FILE: {
            TM_DEBUG("Reading from file: %s", input);
            return tm_read_file(input, size);
        }
        
        case INPUT_RAW: {
            TM_DEBUG("Using raw input");
            *size = strlen(input);
            return tm_strdup(input);
        }
    }
    
    return NULL;
}

/* ============================================================================
 * Repository Detection
 * ========================================================================== */

/**
 * Try to find the repository root from file paths in stack trace
 */
static char *find_repo_from_trace(tm_stack_trace_t *trace)
{
    for (size_t i = 0; i < trace->frame_count; i++) {
        const char *file = trace->frames[i].file;
        if (!file || file[0] != '/') continue;
        
        /* Walk up the directory tree looking for .git */
        char *path = tm_strdup(file);
        char *slash = strrchr(path, '/');
        
        while (slash && slash != path) {
            *slash = '\0';
            
            char git_dir[PATH_MAX];
            snprintf(git_dir, sizeof(git_dir), "%s/.git", path);
            
            struct stat st;
            if (stat(git_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
                TM_DEBUG("Found repo root: %s", path);
                return path;
            }
            
            slash = strrchr(path, '/');
        }
        
        free(path);
    }
    
    /* Try current directory */
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) {
        char git_dir[PATH_MAX];
        snprintf(git_dir, sizeof(git_dir), "%s/.git", cwd);
        
        struct stat st;
        if (stat(git_dir, &st) == 0) {
            TM_DEBUG("Using current directory as repo: %s", cwd);
            return tm_strdup(cwd);
        }
    }
    
    return NULL;
}

/* ============================================================================
 * File Collection for AST Analysis
 * ========================================================================== */

/**
 * Collect unique files from stack trace for AST analysis
 */
static char **collect_trace_files(tm_stack_trace_t *trace, 
                                  const char *repo_root,
                                  size_t *count)
{
    if (!trace || trace->frame_count == 0) {
        *count = 0;
        return NULL;
    }
    
    /* Allocate array for unique files */
    char **files = tm_malloc(trace->frame_count * sizeof(char *));
    size_t n = 0;
    
    for (size_t i = 0; i < trace->frame_count; i++) {
        const char *file = trace->frames[i].file;
        if (!file) continue;
        
        /* Build full path */
        char full_path[PATH_MAX];
        if (file[0] == '/') {
            snprintf(full_path, sizeof(full_path), "%s", file);
        } else if (repo_root) {
            snprintf(full_path, sizeof(full_path), "%s/%s", repo_root, file);
        } else {
            continue;
        }
        
        /* Check if file exists */
        struct stat st;
        if (stat(full_path, &st) != 0) continue;
        
        /* Check for duplicates */
        bool dup = false;
        for (size_t j = 0; j < n; j++) {
            if (strcmp(files[j], full_path) == 0) {
                dup = true;
                break;
            }
        }
        
        if (!dup) {
            files[n++] = tm_strdup(full_path);
        }
    }
    
    *count = n;
    return files;
}

/* ============================================================================
 * Main Analysis Pipeline
 * ========================================================================== */

tm_analysis_result_t *tm_analyze(tm_analyzer_t *analyzer, const char *input)
{
    if (!analyzer) return NULL;
    
    tm_analysis_result_t *result = result_new();
    gettimeofday(&analyzer->start_time, NULL);
    
    TM_INFO("Starting analysis");
    
    /* ========== Phase 1: Parse Input (Format-Agnostic) ========== */
    report_progress(analyzer, "Parsing input", 0.0f);
    
    size_t input_size = 0;
    char *raw_input = read_input(input, &input_size);
    if (!raw_input) {
        result->error_message = tm_strdup("Failed to read input");
        result->analysis_time_ms = 0;
        TM_ERROR("Failed to read input");
        return result;
    }
    
    /* Use unified parsing to auto-detect mode */
    tm_analysis_mode_t mode = TM_MODE_AUTO;
    tm_stack_trace_t *trace = NULL;
    tm_generic_log_t *generic_log = NULL;
    
    tm_error_t parse_err = tm_unified_parse(raw_input, input_size, &mode, &trace, &generic_log);
    TM_FREE(raw_input);
    
    if (parse_err != TM_OK) {
        result->error_message = tm_strdup("Failed to parse input - not a recognized log format");
        TM_ERROR("Failed to parse input");
        return result;
    }
    
    /* Store results based on mode */
    result->trace = trace;
    
    /* Branch based on analysis mode */
    bool is_generic_mode = (mode == TM_MODE_GENERIC_LOG && generic_log != NULL);
    
    if (is_generic_mode) {
        TM_INFO("Analysis mode: GENERIC LOG (%s format, %zu entries, %zu errors)", 
                generic_log->format_description,
                generic_log->count,
                generic_log->total_errors);
        report_progress(analyzer, "Log parsed (generic mode)", 0.15f);
    } else if (result->trace && result->trace->frame_count > 0) {
        TM_INFO("Analysis mode: STACK TRACE (%zu frames, %s)", 
                result->trace->frame_count,
                tm_language_name(result->trace->language));
        report_progress(analyzer, "Stack trace parsed", 0.15f);
    } else {
        /* Neither mode succeeded */
        tm_generic_log_free(generic_log);
        result->error_message = tm_strdup("Failed to parse input as stack trace or log");
        TM_ERROR("Failed to parse input");
        return result;
    }
    
    /* ========== Phase 2: Find Repository ========== */
    char *repo_path = NULL;
    
    if (!is_generic_mode && result->trace) {
        repo_path = analyzer->config->repo_path 
            ? tm_strdup(analyzer->config->repo_path)
            : find_repo_from_trace(result->trace);
    } else if (analyzer->config->repo_path) {
        repo_path = tm_strdup(analyzer->config->repo_path);
    }
    
    if (!repo_path) {
        TM_DEBUG("No repository root found (optional for generic log mode)");
    } else {
        TM_DEBUG("Using repository: %s", repo_path);
    }
    
    /* ========== Phase 3: Build Call Graph (Stack Trace Mode Only) ========== */
    if (!is_generic_mode && result->trace && result->trace->frame_count > 0) {
        report_progress(analyzer, "Analyzing code structure", 0.20f);
        
        if (repo_path) {
            tm_ast_builder_t *ast = tm_ast_builder_new();
            if (ast) {
                /* Collect files from trace */
                size_t file_count = 0;
                char **files = collect_trace_files(result->trace, repo_path, &file_count);
                
                if (files && file_count > 0) {
                    TM_DEBUG("Analyzing %zu files", file_count);
                    
                    for (size_t i = 0; i < file_count; i++) {
                        tm_ast_add_file(ast, files[i]);
                        TM_FREE(files[i]);
                    }
                    TM_FREE(files);
                    
                    /* Build call graph focused on crash location */
                    const char *entry = result->trace->frames[0].function;
                    result->call_graph = tm_ast_build_call_graph(
                        ast, entry, analyzer->config->max_call_depth);
                }
                
                tm_ast_builder_free(ast);
            }
        }
        
        if (result->call_graph) {
            TM_INFO("Built call graph with %zu functions, %zu edges",
                    result->call_graph->node_count,
                    result->call_graph->edge_count);
        }
        
        report_progress(analyzer, "Code structure analyzed", 0.40f);
    } else {
        report_progress(analyzer, "Skipping code analysis (generic mode)", 0.40f);
    }
    
    /* ========== Phase 4: Collect Git Context ========== */
    report_progress(analyzer, "Collecting git history", 0.45f);
    
    if (repo_path && !is_generic_mode && result->trace) {
        /* Stack trace mode: collect files involved in trace */
        size_t file_count = 0;
        char **files = collect_trace_files(result->trace, repo_path, &file_count);
        
        if (files && file_count > 0) {
            /* Convert to const char** */
            const char **const_files = (const char **)files;
            
            result->git_ctx = tm_git_collect_context(
                repo_path,
                const_files,
                file_count,
                analyzer->config->max_commits);
            
            for (size_t i = 0; i < file_count; i++) {
                TM_FREE(files[i]);
            }
            TM_FREE(files);
        }
    } else if (repo_path && is_generic_mode) {
        /* Generic mode: collect recent commits (no specific files) */
        result->git_ctx = tm_git_collect_context(
            repo_path,
            NULL,
            0,
            analyzer->config->max_commits);
    }
    
    if (result->git_ctx) {
        TM_INFO("Collected %zu commits, %zu blame entries",
                result->git_ctx->commit_count,
                result->git_ctx->blame_count);
    }
    
    report_progress(analyzer, "Git history collected", 0.60f);
    
    /* ========== Phase 5: Generate Hypotheses ========== */
    report_progress(analyzer, "Generating hypotheses (LLM)", 0.65f);
    
    /* Check API key */
    if (!analyzer->config->api_key || strlen(analyzer->config->api_key) == 0) {
        TM_WARN("No API key configured - skipping LLM analysis");
        result->error_message = tm_strdup("No LLM API key configured");
    } else {
        tm_error_t err;
        
        if (is_generic_mode) {
            /* Generic log analysis */
            TM_INFO("Using generic log analysis mode");
            err = tm_llm_generate_generic_hypotheses(
                analyzer->llm,
                generic_log,
                result->git_ctx,
                &result->hypotheses,
                &result->hypothesis_count);
        } else {
            /* Stack trace analysis */
            err = tm_llm_generate_hypotheses(
                analyzer->llm,
                result->trace,
                result->call_graph,
                result->git_ctx,
                &result->hypotheses,
                &result->hypothesis_count);
        }
        
        if (err != TM_OK) {
            TM_ERROR("LLM hypothesis generation failed: %s", tm_strerror(err));
            if (!result->error_message) {
                result->error_message = tm_strdup("LLM analysis failed");
            }
        } else {
            TM_INFO("Generated %zu hypotheses", result->hypothesis_count);
        }
    }
    
    report_progress(analyzer, "Analysis complete", 1.0f);
    
    /* ========== Finalize ========== */
    gettimeofday(&analyzer->end_time, NULL);
    
    result->analysis_time_ms = 
        (analyzer->end_time.tv_sec - analyzer->start_time.tv_sec) * 1000 +
        (analyzer->end_time.tv_usec - analyzer->start_time.tv_usec) / 1000;
    
    TM_INFO("Analysis completed in %d ms", result->analysis_time_ms);
    
    /* Cleanup */
    TM_FREE(repo_path);
    tm_generic_log_free(generic_log);
    
    return result;
}

/* ============================================================================
 * Result Output
 * ========================================================================== */

char *tm_format_result(tm_analyzer_t *analyzer, tm_analysis_result_t *result)
{
    if (!analyzer || !result) return NULL;
    
    switch (analyzer->config->output_format) {
        case TM_OUTPUT_CLI:
            return tm_format_cli(analyzer->formatter, result);
            
        case TM_OUTPUT_MARKDOWN:
            return tm_format_markdown(analyzer->formatter, result);
            
        case TM_OUTPUT_JSON:
            return tm_format_json(analyzer->formatter, result);
            
        default:
            return tm_format_cli(analyzer->formatter, result);
    }
}

void tm_print_result(tm_analyzer_t *analyzer, tm_analysis_result_t *result)
{
    char *output = tm_format_result(analyzer, result);
    if (output) {
        printf("%s", output);
        TM_FREE(output);
    }
}

/* ============================================================================
 * Convenience Functions
 * ========================================================================== */

/**
 * One-shot analysis with default config
 */
tm_analysis_result_t *tm_analyze_quick(const char *input)
{
    tm_config_t *config = tm_config_new();
    tm_config_load_env(config);
    tm_config_load(config, NULL);
    
    tm_analyzer_t *analyzer = tm_analyzer_new(config);
    if (!analyzer) {
        tm_config_free(config);
        return NULL;
    }
    
    tm_analysis_result_t *result = tm_analyze(analyzer, input);
    
    tm_analyzer_free(analyzer);
    tm_config_free(config);
    
    return result;
}

/* ============================================================================
 * Explain Command — Quick Error Explanation
 * ========================================================================== */

tm_analysis_result_t *tm_explain(tm_analyzer_t *analyzer, const char *error_msg)
{
    if (!analyzer || !error_msg) return NULL;
    
    tm_analysis_result_t *result = result_new();
    gettimeofday(&analyzer->start_time, NULL);
    
    report_progress(analyzer, "Explaining error", 0.1f);
    
    /* No parsing phase — send the error string directly to LLM */
    tm_error_t err = tm_llm_explain_error(
        analyzer->llm,
        error_msg,
        &result->hypotheses,
        &result->hypothesis_count);
    
    if (err != TM_OK) {
        result->error_message = tm_strdup("Failed to generate explanation");
        TM_ERROR("Explain failed: %s", tm_strerror(err));
    }
    
    report_progress(analyzer, "Done", 1.0f);
    
    gettimeofday(&analyzer->end_time, NULL);
    result->analysis_time_ms =
        (analyzer->end_time.tv_sec - analyzer->start_time.tv_sec) * 1000 +
        (analyzer->end_time.tv_usec - analyzer->start_time.tv_usec) / 1000;
    
    return result;
}

/* ============================================================================
 * Interactive Follow-Up Mode
 * ========================================================================== */

void tm_interactive(tm_analyzer_t *analyzer, tm_analysis_result_t *result)
{
    if (!analyzer || !result) return;
    
    bool use_colors = analyzer->config->color_output && isatty(STDOUT_FILENO);
    
    fprintf(stdout, "\n");
    if (use_colors) {
        fprintf(stdout, "\033[1;36m");
    }
    fprintf(stdout, "=== Interactive Mode ===");
    if (use_colors) {
        fprintf(stdout, "\033[0m");
    }
    fprintf(stdout, "\n");
    fprintf(stdout, "Ask follow-up questions about the analysis.\n");
    fprintf(stdout, "Type 'q' to quit, or a number (1-%zu) to drill into a hypothesis.\n\n",
            result->hypothesis_count);
    
    char line[4096];
    
    while (1) {
        if (use_colors) {
            fprintf(stdout, "\033[1;33m> \033[0m");
        } else {
            fprintf(stdout, "> ");
        }
        fflush(stdout);
        
        if (!fgets(line, sizeof(line), stdin)) {
            break;  /* EOF */
        }
        
        /* Trim newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
            len--;
        }
        
        if (len == 0) continue;
        
        /* Quit */
        if (strcmp(line, "q") == 0 || strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
            break;
        }
        
        /* Number: drill into hypothesis */
        char *end;
        long num = strtol(line, &end, 10);
        if (*end == '\0' && num >= 1 && (size_t)num <= result->hypothesis_count) {
            const tm_hypothesis_t *h = result->hypotheses[num - 1];
            fprintf(stdout, "\n");
            if (h->fix_suggestion) {
                fprintf(stdout, "  Suggested Fix:\n    %s\n\n", h->fix_suggestion);
            }
            if (h->debug_command_count > 0) {
                fprintf(stdout, "  Debug Commands:\n");
                for (size_t i = 0; i < h->debug_command_count; i++) {
                    fprintf(stdout, "    $ %s\n", h->debug_commands[i]);
                }
                fprintf(stdout, "\n");
            }
            if (h->similar_errors) {
                fprintf(stdout, "  Similar Errors:\n    %s\n\n", h->similar_errors);
            }
            continue;
        }
        
        /* Free-form follow-up question */
        fprintf(stdout, "Thinking...\n");
        
        char *response = NULL;
        tm_error_t err = tm_llm_followup(analyzer->llm, result, line, &response);
        
        if (err == TM_OK && response) {
            fprintf(stdout, "\n%s\n\n", response);
            TM_FREE(response);
        } else {
            fprintf(stderr, "  (Failed to get response: %s)\n\n", tm_strerror(err));
        }
    }
    
    fprintf(stdout, "\nExiting interactive mode.\n");
}
