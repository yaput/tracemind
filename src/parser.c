/**
 * TraceMind - Stack Trace Parser Implementation
 */

#include "internal/common.h"
#include "internal/parser.h"
#include <regex.h>
#include <ctype.h>

/* ============================================================================
 * Stack Frame Management
 * ========================================================================== */

tm_stack_frame_t *tm_frame_new(const char *function,
                               const char *file,
                               int line,
                               int column)
{
    tm_stack_frame_t *frame = tm_calloc(1, sizeof(tm_stack_frame_t));
    
    frame->function = function ? tm_strdup(function) : NULL;
    frame->file = file ? tm_strdup(file) : NULL;
    frame->line = line;
    frame->column = column;
    frame->module = NULL;
    frame->context = NULL;
    frame->is_stdlib = false;
    frame->is_third_party = false;
    
    return frame;
}

void tm_frame_free_contents(tm_stack_frame_t *frame)
{
    if (!frame) return;
    
    TM_FREE(frame->function);
    TM_FREE(frame->file);
    TM_FREE(frame->module);
    TM_FREE(frame->context);
}

tm_stack_trace_t *tm_trace_new(void)
{
    tm_stack_trace_t *trace = tm_calloc(1, sizeof(tm_stack_trace_t));
    
    trace->language = TM_LANG_UNKNOWN;
    trace->error_type = NULL;
    trace->error_message = NULL;
    trace->frames = NULL;
    trace->frame_count = 0;
    trace->frame_capacity = 0;
    trace->raw_trace = NULL;
    
    return trace;
}

tm_error_t tm_trace_add_frame(tm_stack_trace_t *trace, tm_stack_frame_t *frame)
{
    TM_CHECK_NULL(trace, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(frame, TM_ERR_INVALID_ARG);
    
    if (trace->frame_count >= trace->frame_capacity) {
        size_t new_cap = trace->frame_capacity == 0 ? 16 : trace->frame_capacity * 2;
        trace->frames = tm_realloc(trace->frames, new_cap * sizeof(tm_stack_frame_t));
        trace->frame_capacity = new_cap;
    }
    
    trace->frames[trace->frame_count++] = *frame;
    free(frame);  /* We copied the contents, free the container */
    
    return TM_OK;
}

void tm_trace_free(tm_stack_trace_t *trace)
{
    if (!trace) return;
    
    TM_FREE(trace->error_type);
    TM_FREE(trace->error_message);
    TM_FREE(trace->raw_trace);
    
    for (size_t i = 0; i < trace->frame_count; i++) {
        tm_frame_free_contents(&trace->frames[i]);
    }
    TM_FREE(trace->frames);
    
    free(trace);
}

/* ============================================================================
 * Language Detection Scoring
 * ========================================================================== */

void tm_score_languages(const char *input, tm_lang_score_t scores[], size_t *count)
{
    if (!input || !scores || !count) return;
    
    *count = 3;  /* Python, Go, Node.js */
    
    /* Initialize scores */
    scores[0] = (tm_lang_score_t){ TM_LANG_PYTHON, 0 };
    scores[1] = (tm_lang_score_t){ TM_LANG_GO, 0 };
    scores[2] = (tm_lang_score_t){ TM_LANG_NODEJS, 0 };
    
    /* Python indicators */
    if (strstr(input, "Traceback (most recent call last)")) scores[0].score += 50;
    if (strstr(input, "File \"")) scores[0].score += 20;
    if (strstr(input, ".py\", line")) scores[0].score += 30;
    if (strstr(input, "ModuleNotFoundError")) scores[0].score += 20;
    if (strstr(input, "ImportError")) scores[0].score += 15;
    if (strstr(input, "AttributeError")) scores[0].score += 15;
    if (strstr(input, "KeyError")) scores[0].score += 15;
    
    /* Go indicators */
    if (strstr(input, "panic:")) scores[1].score += 40;
    if (strstr(input, "goroutine ")) scores[1].score += 30;
    if (strstr(input, ".go:")) scores[1].score += 20;
    if (strstr(input, "+0x")) scores[1].score += 10;
    if (strstr(input, "runtime.")) scores[1].score += 15;
    
    /* Node.js indicators */
    if (strstr(input, "    at ")) scores[2].score += 25;
    if (strstr(input, ".js:")) scores[2].score += 20;
    if (strstr(input, ".ts:")) scores[2].score += 20;
    if (strstr(input, "TypeError:")) scores[2].score += 20;
    if (strstr(input, "ReferenceError:")) scores[2].score += 20;
    if (strstr(input, "SyntaxError:")) scores[2].score += 15;
    if (strstr(input, "node_modules")) scores[2].score += 10;
    
    /* Cap at 100 */
    for (size_t i = 0; i < *count; i++) {
        if (scores[i].score > 100) scores[i].score = 100;
    }
}

/* ============================================================================
 * Python Parser
 * ========================================================================== */

bool tm_is_python_traceback_header(const char *line)
{
    return strstr(line, "Traceback (most recent call last)") != NULL;
}

tm_error_t tm_parse_python_trace(const char *input, tm_stack_trace_t *trace)
{
    TM_CHECK_NULL(input, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(trace, TM_ERR_INVALID_ARG);
    
    trace->language = TM_LANG_PYTHON;
    trace->raw_trace = tm_strdup(input);
    
    /* Compile regex patterns */
    regex_t frame_re, error_re;
    int ret;
    
    /* Pattern: File "path", line N, in function */
    ret = regcomp(&frame_re, 
                  "File \"([^\"]+)\", line ([0-9]+)(, in ([^[:space:]]+))?",
                  REG_EXTENDED);
    if (ret != 0) {
        TM_ERROR("Failed to compile Python frame regex");
        return TM_ERR_INTERNAL;
    }
    
    /* Pattern: ExceptionType: message */
    ret = regcomp(&error_re,
                  "^([A-Za-z][A-Za-z0-9_]*(Error|Exception|Warning)): (.*)$",
                  REG_EXTENDED | REG_NEWLINE);
    if (ret != 0) {
        regfree(&frame_re);
        TM_ERROR("Failed to compile Python error regex");
        return TM_ERR_INTERNAL;
    }
    
    /* Parse frames */
    const char *cursor = input;
    regmatch_t matches[5];
    
    while (regexec(&frame_re, cursor, 5, matches, 0) == 0) {
        char *file = tm_strndup(cursor + matches[1].rm_so,
                                (size_t)(matches[1].rm_eo - matches[1].rm_so));
        
        char line_str[32];
        size_t line_len = (size_t)(matches[2].rm_eo - matches[2].rm_so);
        if (line_len >= sizeof(line_str)) line_len = sizeof(line_str) - 1;
        strncpy(line_str, cursor + matches[2].rm_so, line_len);
        line_str[line_len] = '\0';
        int line = atoi(line_str);
        
        char *function = NULL;
        if (matches[4].rm_so != -1) {
            function = tm_strndup(cursor + matches[4].rm_so,
                                  (size_t)(matches[4].rm_eo - matches[4].rm_so));
        } else {
            function = tm_strdup("<module>");
        }
        
        tm_stack_frame_t *frame = tm_frame_new(function, file, line, 0);
        frame->is_stdlib = tm_is_stdlib_path(file, TM_LANG_PYTHON);
        frame->is_third_party = tm_is_third_party_path(file, TM_LANG_PYTHON);
        
        tm_trace_add_frame(trace, frame);
        
        TM_FREE(file);
        TM_FREE(function);
        
        cursor += matches[0].rm_eo;
    }
    
    /* Parse error type and message */
    cursor = input;
    while (regexec(&error_re, cursor, 4, matches, 0) == 0) {
        /* Keep only the last match (final exception) */
        TM_FREE(trace->error_type);
        TM_FREE(trace->error_message);
        
        trace->error_type = tm_strndup(cursor + matches[1].rm_so,
                                       (size_t)(matches[1].rm_eo - matches[1].rm_so));
        trace->error_message = tm_strndup(cursor + matches[3].rm_so,
                                          (size_t)(matches[3].rm_eo - matches[3].rm_so));
        
        cursor += matches[0].rm_eo;
    }
    
    regfree(&frame_re);
    regfree(&error_re);
    
    if (trace->frame_count == 0) {
        TM_WARN("No frames found in Python trace");
        return TM_ERR_PARSE;
    }
    
    TM_DEBUG("Parsed %zu Python frames", trace->frame_count);
    return TM_OK;
}

/* ============================================================================
 * Go Parser
 * ========================================================================== */

bool tm_is_go_panic_header(const char *line)
{
    return strstr(line, "panic:") != NULL || strstr(line, "goroutine ") != NULL;
}

tm_error_t tm_parse_go_trace(const char *input, tm_stack_trace_t *trace)
{
    TM_CHECK_NULL(input, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(trace, TM_ERR_INVALID_ARG);
    
    trace->language = TM_LANG_GO;
    trace->raw_trace = tm_strdup(input);
    
    /* Compile regex patterns */
    regex_t func_re, loc_re, panic_re;
    int ret;
    
    /* Pattern: package.function(args) - match function name until '(' at start of line */
    ret = regcomp(&func_re,
                  "^([^[:space:](]+)\\(",
                  REG_EXTENDED | REG_NEWLINE);
    if (ret != 0) {
        TM_ERROR("Failed to compile Go function regex");
        return TM_ERR_INTERNAL;
    }
    
    /* Pattern: /path/file.go:line +0xNN (accepts tab or spaces for indentation) */
    ret = regcomp(&loc_re,
                  "^[[:space:]]+([^:]+\\.go):([0-9]+)",
                  REG_EXTENDED | REG_NEWLINE);
    if (ret != 0) {
        regfree(&func_re);
        TM_ERROR("Failed to compile Go location regex");
        return TM_ERR_INTERNAL;
    }
    
    /* Pattern: panic: message or Error: message */
    ret = regcomp(&panic_re,
                  "^(panic|Error|error): (.*)$",
                  REG_EXTENDED | REG_NEWLINE);
    if (ret != 0) {
        regfree(&func_re);
        regfree(&loc_re);
        TM_ERROR("Failed to compile Go panic regex");
        return TM_ERR_INTERNAL;
    }
    
    /* Parse panic/error message */
    regmatch_t matches[4];
    if (regexec(&panic_re, input, 3, matches, 0) == 0) {
        trace->error_type = tm_strndup(input + matches[1].rm_so,
                                       (size_t)(matches[1].rm_eo - matches[1].rm_so));
        trace->error_message = tm_strndup(input + matches[2].rm_so,
                                          (size_t)(matches[2].rm_eo - matches[2].rm_so));
    }
    
    /* Parse stack frames - Go outputs function then location on next line */
    const char *cursor = input;
    char *pending_function = NULL;
    
    while (*cursor) {
        /* Try to match function line */
        if (regexec(&func_re, cursor, 2, matches, 0) == 0) {
            TM_FREE(pending_function);
            pending_function = tm_strndup(cursor + matches[1].rm_so,
                                          (size_t)(matches[1].rm_eo - matches[1].rm_so));
        }
        
        /* Try to match location line */
        if (regexec(&loc_re, cursor, 3, matches, 0) == 0 && pending_function) {
            char *file = tm_strndup(cursor + matches[1].rm_so,
                                    (size_t)(matches[1].rm_eo - matches[1].rm_so));
            
            char line_str[32];
            size_t line_len = (size_t)(matches[2].rm_eo - matches[2].rm_so);
            if (line_len >= sizeof(line_str)) line_len = sizeof(line_str) - 1;
            strncpy(line_str, cursor + matches[2].rm_so, line_len);
            line_str[line_len] = '\0';
            int line = atoi(line_str);
            
            tm_stack_frame_t *frame = tm_frame_new(pending_function, file, line, 0);
            frame->is_stdlib = tm_is_stdlib_path(file, TM_LANG_GO);
            frame->is_third_party = tm_is_third_party_path(file, TM_LANG_GO);
            
            tm_trace_add_frame(trace, frame);
            
            TM_FREE(file);
            TM_FREE(pending_function);
            pending_function = NULL;
        }
        
        /* Move to next line */
        const char *newline = strchr(cursor, '\n');
        if (newline) {
            cursor = newline + 1;
        } else {
            break;
        }
    }
    
    TM_FREE(pending_function);
    regfree(&func_re);
    regfree(&loc_re);
    regfree(&panic_re);
    
    if (trace->frame_count == 0) {
        TM_WARN("No frames found in Go trace");
        return TM_ERR_PARSE;
    }
    
    TM_DEBUG("Parsed %zu Go frames", trace->frame_count);
    return TM_OK;
}

/* ============================================================================
 * Node.js Parser
 * ========================================================================== */

bool tm_is_nodejs_error_header(const char *line)
{
    return (strstr(line, "Error:") != NULL ||
            strstr(line, "TypeError:") != NULL ||
            strstr(line, "ReferenceError:") != NULL);
}

tm_error_t tm_parse_nodejs_trace(const char *input, tm_stack_trace_t *trace)
{
    TM_CHECK_NULL(input, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(trace, TM_ERR_INVALID_ARG);
    
    trace->language = TM_LANG_NODEJS;
    trace->raw_trace = tm_strdup(input);
    
    /* Compile regex patterns */
    regex_t frame_re, frame_bare_re, error_re;
    int ret;
    
    /* Pattern: at function (path:line:col) */
    ret = regcomp(&frame_re,
                  "at ([^ ]+) \\(([^:]+):([0-9]+):([0-9]+)\\)",
                  REG_EXTENDED);
    if (ret != 0) {
        TM_ERROR("Failed to compile Node.js frame regex");
        return TM_ERR_INTERNAL;
    }
    
    /* Pattern: at path:line:col (no function name) */
    ret = regcomp(&frame_bare_re,
                  "at ([^:]+):([0-9]+):([0-9]+)",
                  REG_EXTENDED);
    if (ret != 0) {
        regfree(&frame_re);
        TM_ERROR("Failed to compile Node.js bare frame regex");
        return TM_ERR_INTERNAL;
    }
    
    /* Pattern: ErrorType: message */
    ret = regcomp(&error_re,
                  "^([A-Za-z]+Error|[A-Za-z]+Exception): (.*)$",
                  REG_EXTENDED | REG_NEWLINE);
    if (ret != 0) {
        regfree(&frame_re);
        regfree(&frame_bare_re);
        TM_ERROR("Failed to compile Node.js error regex");
        return TM_ERR_INTERNAL;
    }
    
    /* Parse error type and message */
    regmatch_t matches[5];
    if (regexec(&error_re, input, 3, matches, 0) == 0) {
        trace->error_type = tm_strndup(input + matches[1].rm_so,
                                       (size_t)(matches[1].rm_eo - matches[1].rm_so));
        trace->error_message = tm_strndup(input + matches[2].rm_so,
                                          (size_t)(matches[2].rm_eo - matches[2].rm_so));
    }
    
    /* Parse frames */
    const char *cursor = input;
    
    while (*cursor) {
        /* Try full format first */
        if (regexec(&frame_re, cursor, 5, matches, 0) == 0) {
            char *function = tm_strndup(cursor + matches[1].rm_so,
                                        (size_t)(matches[1].rm_eo - matches[1].rm_so));
            char *file = tm_strndup(cursor + matches[2].rm_so,
                                    (size_t)(matches[2].rm_eo - matches[2].rm_so));
            
            char line_str[32], col_str[32];
            size_t len;
            
            len = (size_t)(matches[3].rm_eo - matches[3].rm_so);
            if (len >= sizeof(line_str)) len = sizeof(line_str) - 1;
            strncpy(line_str, cursor + matches[3].rm_so, len);
            line_str[len] = '\0';
            
            len = (size_t)(matches[4].rm_eo - matches[4].rm_so);
            if (len >= sizeof(col_str)) len = sizeof(col_str) - 1;
            strncpy(col_str, cursor + matches[4].rm_so, len);
            col_str[len] = '\0';
            
            tm_stack_frame_t *frame = tm_frame_new(function, file, 
                                                    atoi(line_str), atoi(col_str));
            frame->is_stdlib = tm_is_stdlib_path(file, TM_LANG_NODEJS);
            frame->is_third_party = tm_is_third_party_path(file, TM_LANG_NODEJS);
            
            tm_trace_add_frame(trace, frame);
            
            TM_FREE(function);
            TM_FREE(file);
            
            cursor += matches[0].rm_eo;
            continue;
        }
        
        /* Try bare format (anonymous function) */
        if (regexec(&frame_bare_re, cursor, 4, matches, 0) == 0) {
            char *file = tm_strndup(cursor + matches[1].rm_so,
                                    (size_t)(matches[1].rm_eo - matches[1].rm_so));
            
            /* Skip if this looks like a module path in a function call match */
            if (strstr(cursor + matches[0].rm_so - 5, "at ") == NULL) {
                TM_FREE(file);
                cursor++;
                continue;
            }
            
            char line_str[32], col_str[32];
            size_t len;
            
            len = (size_t)(matches[2].rm_eo - matches[2].rm_so);
            if (len >= sizeof(line_str)) len = sizeof(line_str) - 1;
            strncpy(line_str, cursor + matches[2].rm_so, len);
            line_str[len] = '\0';
            
            len = (size_t)(matches[3].rm_eo - matches[3].rm_so);
            if (len >= sizeof(col_str)) len = sizeof(col_str) - 1;
            strncpy(col_str, cursor + matches[3].rm_so, len);
            col_str[len] = '\0';
            
            tm_stack_frame_t *frame = tm_frame_new("<anonymous>", file,
                                                    atoi(line_str), atoi(col_str));
            frame->is_stdlib = tm_is_stdlib_path(file, TM_LANG_NODEJS);
            frame->is_third_party = tm_is_third_party_path(file, TM_LANG_NODEJS);
            
            tm_trace_add_frame(trace, frame);
            
            TM_FREE(file);
            
            cursor += matches[0].rm_eo;
            continue;
        }
        
        cursor++;
    }
    
    regfree(&frame_re);
    regfree(&frame_bare_re);
    regfree(&error_re);
    
    if (trace->frame_count == 0) {
        TM_WARN("No frames found in Node.js trace");
        return TM_ERR_PARSE;
    }
    
    TM_DEBUG("Parsed %zu Node.js frames", trace->frame_count);
    return TM_OK;
}

/* ============================================================================
 * Parser Registry
 * ========================================================================== */

tm_parser_fn tm_get_parser(tm_language_t lang)
{
    switch (lang) {
    case TM_LANG_PYTHON:  return tm_parse_python_trace;
    case TM_LANG_GO:      return tm_parse_go_trace;
    case TM_LANG_NODEJS:  return tm_parse_nodejs_trace;
    default:              return NULL;
    }
}

/* ============================================================================
 * Main Parse Entry Point
 * ========================================================================== */

tm_error_t tm_parse_trace(const char *input, 
                          tm_language_t hint,
                          tm_stack_trace_t **result)
{
    TM_CHECK_NULL(input, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(result, TM_ERR_INVALID_ARG);
    
    *result = NULL;
    
    /* Auto-detect language if not specified */
    tm_language_t lang = hint;
    if (lang == TM_LANG_UNKNOWN) {
        lang = tm_detect_language(input);
        if (lang == TM_LANG_UNKNOWN) {
            TM_ERROR("Could not detect stack trace language");
            return TM_ERR_UNSUPPORTED;
        }
        TM_INFO("Auto-detected language: %s", tm_language_name(lang));
    }
    
    /* Get parser for language */
    tm_parser_fn parser = tm_get_parser(lang);
    if (!parser) {
        TM_ERROR("No parser available for language: %s", tm_language_name(lang));
        return TM_ERR_UNSUPPORTED;
    }
    
    /* Create trace and parse */
    tm_stack_trace_t *trace = tm_trace_new();
    tm_error_t err = parser(input, trace);
    
    if (err != TM_OK) {
        tm_trace_free(trace);
        return err;
    }
    
    *result = trace;
    return TM_OK;
}

/* ============================================================================
 * Utility Functions
 * ========================================================================== */

const char *tm_get_extension(const char *path)
{
    if (!path) return NULL;
    
    const char *dot = strrchr(path, '.');
    if (!dot || dot == path) return NULL;
    
    return dot;
}

int tm_parse_line_number(const char *str)
{
    if (!str) return -1;
    
    /* Skip leading whitespace */
    while (isspace((unsigned char)*str)) str++;
    
    /* Check for valid number */
    if (!isdigit((unsigned char)*str)) return -1;
    
    long val = strtol(str, NULL, 10);
    if (val <= 0 || val > INT_MAX) return -1;
    
    return (int)val;
}

/* ============================================================================
 * Public API Wrappers
 * ========================================================================== */

tm_stack_trace_t *tm_parse_stack_trace(const char *input, size_t len)
{
    if (!input || len == 0) return NULL;
    
    /* Create null-terminated copy if needed */
    char *buf = tm_strndup(input, len);
    if (!buf) return NULL;
    
    tm_stack_trace_t *result = NULL;
    tm_error_t err = tm_parse_trace(buf, TM_LANG_UNKNOWN, &result);
    
    tm_free(buf);
    
    if (err != TM_OK) {
        return NULL;
    }
    
    return result;
}

tm_language_t tm_detect_trace_language(const char *input, size_t len)
{
    if (!input || len == 0) return TM_LANG_UNKNOWN;
    
    /* Create null-terminated copy if needed */
    char *buf = tm_strndup(input, len);
    if (!buf) return TM_LANG_UNKNOWN;
    
    tm_language_t lang = tm_detect_language(buf);
    
    tm_free(buf);
    return lang;
}

void tm_stack_trace_free(tm_stack_trace_t *trace)
{
    tm_trace_free(trace);
}
