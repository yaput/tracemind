/**
 * TraceMind - Output Formatter
 * 
 * Formats analysis results for CLI, Markdown, and JSON output.
 */

#include "internal/common.h"
#include "internal/output.h"
#include <sys/ioctl.h>
#include <unistd.h>
#include <time.h>
#include <jansson.h>

/* ============================================================================
 * Formatter Context
 * ========================================================================== */

tm_formatter_t *tm_formatter_new(tm_output_format_t format, bool colors)
{
    tm_formatter_t *fmt = tm_calloc(1, sizeof(tm_formatter_t));
    
    fmt->format = format;
    fmt->use_colors = colors && tm_supports_colors(stdout);
    fmt->verbose = false;
    fmt->terminal_width = tm_terminal_width();
    fmt->output = stdout;
    
    return fmt;
}

void tm_formatter_free(tm_formatter_t *fmt)
{
    free(fmt);
}

/* ============================================================================
 * Utility Functions
 * ========================================================================== */

bool tm_supports_colors(FILE *stream)
{
    if (!stream) return false;
    
    int fd = fileno(stream);
    return isatty(fd) != 0;
}

int tm_terminal_width(void)
{
    struct winsize w;
    
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) {
        return w.ws_col;
    }
    
    return 80;  /* Default */
}

char *tm_wrap_text(const char *text, int width)
{
    if (!text || width <= 0) return tm_strdup(text ? text : "");
    
    tm_strbuf_t sb;
    tm_strbuf_init(&sb);
    
    int col = 0;
    const char *word_start = text;
    
    for (const char *p = text; ; p++) {
        if (*p == ' ' || *p == '\n' || *p == '\0') {
            int word_len = (int)(p - word_start);
            
            if (col + word_len > width && col > 0) {
                tm_strbuf_append(&sb, "\n");
                col = 0;
            }
            
            if (word_len > 0) {
                char *word = tm_strndup(word_start, (size_t)word_len);
                if (col > 0) {
                    tm_strbuf_append(&sb, " ");
                    col++;
                }
                tm_strbuf_append(&sb, word);
                col += word_len;
                TM_FREE(word);
            }
            
            if (*p == '\n') {
                tm_strbuf_append(&sb, "\n");
                col = 0;
            }
            
            if (*p == '\0') break;
            
            word_start = p + 1;
        }
    }
    
    return tm_strbuf_finish(&sb);
}

char *tm_truncate_string(const char *str, size_t max_len)
{
    if (!str) return NULL;
    
    size_t len = strlen(str);
    if (len <= max_len) {
        return tm_strdup(str);
    }
    
    char *result = tm_malloc(max_len + 1);
    strncpy(result, str, max_len - 3);
    result[max_len - 3] = '\0';
    strcat(result, "...");
    
    return result;
}

char *tm_json_escape(const char *str)
{
    if (!str) return tm_strdup("null");
    
    tm_strbuf_t sb;
    tm_strbuf_init(&sb);
    
    for (const char *p = str; *p; p++) {
        switch (*p) {
        case '"':  tm_strbuf_append(&sb, "\\\""); break;
        case '\\': tm_strbuf_append(&sb, "\\\\"); break;
        case '\b': tm_strbuf_append(&sb, "\\b"); break;
        case '\f': tm_strbuf_append(&sb, "\\f"); break;
        case '\n': tm_strbuf_append(&sb, "\\n"); break;
        case '\r': tm_strbuf_append(&sb, "\\r"); break;
        case '\t': tm_strbuf_append(&sb, "\\t"); break;
        default:
            if ((unsigned char)*p < 32) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)*p);
                tm_strbuf_append(&sb, buf);
            } else {
                char c[2] = { *p, '\0' };
                tm_strbuf_append(&sb, c);
            }
        }
    }
    
    return tm_strbuf_finish(&sb);
}

char *tm_format_duration(int64_t ms)
{
    char *buf = tm_malloc(64);
    
    if (ms < 1000) {
        snprintf(buf, 64, "%lldms", (long long)ms);
    } else if (ms < 60000) {
        snprintf(buf, 64, "%.1fs", ms / 1000.0);
    } else {
        int mins = (int)(ms / 60000);
        int secs = (int)((ms % 60000) / 1000);
        snprintf(buf, 64, "%dm %ds", mins, secs);
    }
    
    return buf;
}

char *tm_format_relative_time(int64_t timestamp)
{
    char *buf = tm_malloc(64);
    
    time_t now = time(NULL);
    int64_t diff = (int64_t)now - timestamp;
    
    if (diff < 0) diff = 0;
    
    if (diff < 60) {
        snprintf(buf, 64, "just now");
    } else if (diff < 3600) {
        snprintf(buf, 64, "%lld min ago", (long long)(diff / 60));
    } else if (diff < 86400) {
        snprintf(buf, 64, "%lld hours ago", (long long)(diff / 3600));
    } else if (diff < 604800) {
        snprintf(buf, 64, "%lld days ago", (long long)(diff / 86400));
    } else {
        snprintf(buf, 64, "%lld weeks ago", (long long)(diff / 604800));
    }
    
    return buf;
}

/* ============================================================================
 * CLI Output Helpers
 * ========================================================================== */

static void print_colored(const tm_formatter_t *fmt, const char *color, const char *text)
{
    if (fmt->use_colors) {
        fprintf(fmt->output, "%s%s%s", color, text, TM_COLOR_RESET);
    } else {
        fprintf(fmt->output, "%s", text);
    }
}

void tm_cli_header(const tm_formatter_t *fmt, const char *title)
{
    fprintf(fmt->output, "\n");
    
    if (fmt->use_colors) {
        fprintf(fmt->output, "%s%s %s %s\n",
                TM_COLOR_BOLD, TM_COLOR_CYAN, title, TM_COLOR_RESET);
    } else {
        fprintf(fmt->output, "=== %s ===\n", title);
    }
    
    fprintf(fmt->output, "\n");
}

void tm_cli_divider(const tm_formatter_t *fmt)
{
    int width = fmt->terminal_width > 0 ? TM_MIN(fmt->terminal_width, 80) : 80;
    
    if (fmt->use_colors) {
        fprintf(fmt->output, "%s", TM_COLOR_DIM);
    }
    
    for (int i = 0; i < width; i++) {
        fprintf(fmt->output, "─");
    }
    
    if (fmt->use_colors) {
        fprintf(fmt->output, "%s", TM_COLOR_RESET);
    }
    
    fprintf(fmt->output, "\n");
}

void tm_cli_confidence_bar(const tm_formatter_t *fmt, int confidence)
{
    int filled = confidence / 10;
    int empty = 10 - filled;
    
    const char *color;
    if (confidence >= 70) {
        color = TM_COLOR_GREEN;
    } else if (confidence >= 40) {
        color = TM_COLOR_YELLOW;
    } else {
        color = TM_COLOR_RED;
    }
    
    fprintf(fmt->output, "[");
    
    if (fmt->use_colors) {
        fprintf(fmt->output, "%s", color);
    }
    
    for (int i = 0; i < filled; i++) fprintf(fmt->output, "█");
    
    if (fmt->use_colors) {
        fprintf(fmt->output, "%s", TM_COLOR_DIM);
    }
    
    for (int i = 0; i < empty; i++) fprintf(fmt->output, "░");
    
    if (fmt->use_colors) {
        fprintf(fmt->output, "%s", TM_COLOR_RESET);
    }
    
    fprintf(fmt->output, "] %d%%", confidence);
}

void tm_status(const tm_formatter_t *fmt, const char *icon, const char *message)
{
    if (fmt->use_colors) {
        fprintf(fmt->output, "%s %s\n", icon, message);
    } else {
        fprintf(fmt->output, "* %s\n", message);
    }
}

void tm_error_msg(const tm_formatter_t *fmt, const char *message)
{
    if (fmt->use_colors) {
        fprintf(fmt->output, "%s%s✖ Error: %s%s\n",
                TM_COLOR_BOLD, TM_COLOR_RED, message, TM_COLOR_RESET);
    } else {
        fprintf(fmt->output, "Error: %s\n", message);
    }
}

void tm_warning_msg(const tm_formatter_t *fmt, const char *message)
{
    if (fmt->use_colors) {
        fprintf(fmt->output, "%s⚠ Warning: %s%s\n",
                TM_COLOR_YELLOW, message, TM_COLOR_RESET);
    } else {
        fprintf(fmt->output, "Warning: %s\n", message);
    }
}

void tm_success_msg(const tm_formatter_t *fmt, const char *message)
{
    if (fmt->use_colors) {
        fprintf(fmt->output, "%s✔ %s%s\n",
                TM_COLOR_GREEN, message, TM_COLOR_RESET);
    } else {
        fprintf(fmt->output, "OK: %s\n", message);
    }
}

/* ============================================================================
 * CLI Hypothesis Output
 * ========================================================================== */

void tm_cli_hypothesis(const tm_formatter_t *fmt, const tm_hypothesis_t *hyp)
{
    if (!hyp) return;
    
    /* Rank and title */
    if (fmt->use_colors) {
        const char *rank_color;
        switch (hyp->rank) {
        case 1: rank_color = TM_COLOR_BRED; break;
        case 2: rank_color = TM_COLOR_BYELLOW; break;
        default: rank_color = TM_COLOR_BBLUE; break;
        }
        
        fprintf(fmt->output, "%s%s#%d: %s%s\n",
                TM_COLOR_BOLD, rank_color, hyp->rank,
                hyp->title ? hyp->title : "(No title)",
                TM_COLOR_RESET);
    } else {
        fprintf(fmt->output, "#%d: %s\n",
                hyp->rank, hyp->title ? hyp->title : "(No title)");
    }
    
    fprintf(fmt->output, "\n");
    
    /* Confidence */
    fprintf(fmt->output, "  Confidence: ");
    tm_cli_confidence_bar(fmt, hyp->confidence);
    fprintf(fmt->output, "\n\n");
    
    /* Explanation */
    if (hyp->explanation) {
        print_colored(fmt, TM_COLOR_BOLD, "  Explanation:\n");
        
        char *wrapped = tm_wrap_text(hyp->explanation, fmt->terminal_width - 6);
        char *line = strtok(wrapped, "\n");
        while (line) {
            fprintf(fmt->output, "    %s\n", line);
            line = strtok(NULL, "\n");
        }
        TM_FREE(wrapped);
        fprintf(fmt->output, "\n");
    }
    
    /* Evidence */
    if (hyp->evidence) {
        print_colored(fmt, TM_COLOR_BOLD, "  Evidence:\n");
        
        if (fmt->use_colors) {
            fprintf(fmt->output, "    %s%s%s\n",
                    TM_COLOR_DIM, hyp->evidence, TM_COLOR_RESET);
        } else {
            fprintf(fmt->output, "    %s\n", hyp->evidence);
        }
        fprintf(fmt->output, "\n");
    }
    
    /* Next step */
    if (hyp->next_step) {
        if (fmt->use_colors) {
            fprintf(fmt->output, "  %s→ Next Step:%s %s\n",
                    TM_COLOR_GREEN, TM_COLOR_RESET, hyp->next_step);
        } else {
            fprintf(fmt->output, "  → Next Step: %s\n", hyp->next_step);
        }
        fprintf(fmt->output, "\n");
    }
    
    /* Related files */
    if (hyp->related_file_count > 0) {
        print_colored(fmt, TM_COLOR_DIM, "  Related files: ");
        for (size_t i = 0; i < hyp->related_file_count; i++) {
            if (i > 0) fprintf(fmt->output, ", ");
            fprintf(fmt->output, "%s", hyp->related_files[i] ? hyp->related_files[i] : "?");
        }
        fprintf(fmt->output, "\n");
    }
}

void tm_cli_trace_summary(const tm_formatter_t *fmt, const tm_stack_trace_t *trace)
{
    if (!trace) return;
    
    tm_cli_header(fmt, "STACK TRACE SUMMARY");
    
    if (trace->error_type) {
        fprintf(fmt->output, "  Error: ");
        print_colored(fmt, TM_COLOR_RED, trace->error_type);
        fprintf(fmt->output, "\n");
    }
    
    if (trace->error_message) {
        fprintf(fmt->output, "  Message: %s\n", trace->error_message);
    }
    
    fprintf(fmt->output, "  Language: %s\n", tm_language_name(trace->language));
    fprintf(fmt->output, "  Frames: %zu\n", trace->frame_count);
    
    if (trace->frame_count > 0) {
        fprintf(fmt->output, "\n  Top frames:\n");
        
        for (size_t i = 0; i < TM_MIN(trace->frame_count, 5); i++) {
            const tm_stack_frame_t *f = &trace->frames[i];
            
            if (fmt->use_colors) {
                fprintf(fmt->output, "    %s%zu.%s %s%s%s at ",
                        TM_COLOR_DIM, i + 1, TM_COLOR_RESET,
                        TM_COLOR_CYAN,
                        f->function ? f->function : "<unknown>",
                        TM_COLOR_RESET);
                fprintf(fmt->output, "%s%s:%d%s",
                        TM_COLOR_YELLOW,
                        f->file ? f->file : "<unknown>",
                        f->line,
                        TM_COLOR_RESET);
            } else {
                fprintf(fmt->output, "    %zu. %s at %s:%d",
                        i + 1,
                        f->function ? f->function : "<unknown>",
                        f->file ? f->file : "<unknown>",
                        f->line);
            }
            
            if (f->is_stdlib) {
                print_colored(fmt, TM_COLOR_DIM, " [stdlib]");
            }
            
            fprintf(fmt->output, "\n");
        }
    }
}

void tm_cli_git_summary(const tm_formatter_t *fmt, const tm_git_context_t *ctx)
{
    if (!ctx) return;
    
    tm_cli_header(fmt, "GIT CONTEXT");
    
    fprintf(fmt->output, "  Branch: %s\n", ctx->current_branch ? ctx->current_branch : "unknown");
    fprintf(fmt->output, "  HEAD: %.12s\n", ctx->head_sha ? ctx->head_sha : "unknown");
    fprintf(fmt->output, "  Recent commits: %zu\n", ctx->commit_count);
    
    if (ctx->commit_count > 0) {
        fprintf(fmt->output, "\n  Recent changes:\n");
        
        for (size_t i = 0; i < TM_MIN(ctx->commit_count, 5); i++) {
            const tm_git_commit_t *c = &ctx->commits[i];
            
            /* Truncate message to first line */
            const char *msg = c->message;
            const char *nl = msg ? strchr(msg, '\n') : NULL;
            size_t msg_len = nl ? (size_t)(nl - msg) : (msg ? strlen(msg) : 0);
            if (msg_len > 50) msg_len = 50;
            
            char *time_str = tm_format_relative_time(c->timestamp);
            
            if (fmt->use_colors) {
                fprintf(fmt->output, "    %s%.7s%s ",
                        TM_COLOR_YELLOW, c->sha, TM_COLOR_RESET);
            } else {
                fprintf(fmt->output, "    %.7s ", c->sha);
            }
            
            if (msg) {
                fprintf(fmt->output, "%.*s", (int)msg_len, msg);
                if (msg_len == 50) fprintf(fmt->output, "...");
            }
            
            if (c->touches_config || c->touches_schema) {
                fprintf(fmt->output, " ");
                if (c->touches_config) print_colored(fmt, TM_COLOR_RED, "[CONFIG]");
                if (c->touches_schema) print_colored(fmt, TM_COLOR_RED, "[SCHEMA]");
            }
            
            print_colored(fmt, TM_COLOR_DIM, " (");
            fprintf(fmt->output, "%s", time_str);
            print_colored(fmt, TM_COLOR_DIM, ")");
            
            fprintf(fmt->output, "\n");
            TM_FREE(time_str);
        }
    }
}

void tm_cli_call_graph_summary(const tm_formatter_t *fmt, const tm_call_graph_t *graph)
{
    if (!graph || graph->node_count == 0) return;
    
    tm_cli_header(fmt, "CALL GRAPH");
    
    fprintf(fmt->output, "  Functions analyzed: %zu\n", graph->node_count);
    
    fprintf(fmt->output, "\n  Call chain:\n");
    
    for (size_t i = 0; i < TM_MIN(graph->node_count, 8); i++) {
        const tm_call_node_t *node = graph->nodes[i];
        
        fprintf(fmt->output, "    ");
        
        if (i > 0) {
            print_colored(fmt, TM_COLOR_DIM, "└─ ");
        }
        
        if (fmt->use_colors) {
            fprintf(fmt->output, "%s%s%s()",
                    TM_COLOR_CYAN, node->name ? node->name : "?", TM_COLOR_RESET);
            fprintf(fmt->output, " %s%s:%d%s",
                    TM_COLOR_DIM,
                    node->file ? node->file : "?",
                    node->start_line,
                    TM_COLOR_RESET);
        } else {
            fprintf(fmt->output, "%s() at %s:%d",
                    node->name ? node->name : "?",
                    node->file ? node->file : "?",
                    node->start_line);
        }
        
        fprintf(fmt->output, "\n");
    }
}

/* ============================================================================
 * Full CLI Output
 * ========================================================================== */

char *tm_format_cli(const tm_formatter_t *fmt, const tm_analysis_result_t *result)
{
    if (!result) return tm_strdup("");
    
    tm_strbuf_t sb;
    tm_strbuf_init(&sb);
    
    /* Header banner */
    tm_strbuf_append(&sb, "\n");
    if (fmt->use_colors) {
        tm_strbuf_appendf(&sb, "%s%s", TM_COLOR_BOLD, TM_COLOR_MAGENTA);
        tm_strbuf_append(&sb, "╔══════════════════════════════════════════════════════════════════╗\n");
        tm_strbuf_append(&sb, "║                    TRACEMIND ANALYSIS REPORT                     ║\n");
        tm_strbuf_append(&sb, "╚══════════════════════════════════════════════════════════════════╝\n");
        tm_strbuf_appendf(&sb, "%s", TM_COLOR_RESET);
    } else {
        tm_strbuf_append(&sb, "====================================================================\n");
        tm_strbuf_append(&sb, "                    TRACEMIND ANALYSIS REPORT                       \n");
        tm_strbuf_append(&sb, "====================================================================\n");
    }
    
    /* Error message if any */
    if (result->error_message) {
        if (fmt->use_colors) {
            tm_strbuf_appendf(&sb, "%sWarning: %s%s\n", TM_COLOR_RED, result->error_message, TM_COLOR_RESET);
        } else {
            tm_strbuf_appendf(&sb, "Warning: %s\n", result->error_message);
        }
    }
    
    char *duration = tm_format_duration(result->analysis_time_ms);
    if (fmt->use_colors) {
        tm_strbuf_appendf(&sb, "%sAnalysis time: %s%s\n", TM_COLOR_DIM, duration, TM_COLOR_RESET);
    } else {
        tm_strbuf_appendf(&sb, "Analysis time: %s\n", duration);
    }
    TM_FREE(duration);
    
    /* Trace summary */
    if (result->trace) {
        tm_strbuf_append(&sb, "\n--- Stack Trace ---\n");
        tm_strbuf_appendf(&sb, "Language: %s\n", tm_language_name(result->trace->language));
        tm_strbuf_appendf(&sb, "Frames: %zu\n", result->trace->frame_count);
        if (result->trace->error_message) {
            tm_strbuf_appendf(&sb, "Error: %s\n", result->trace->error_message);
        }
    }
    
    /* Git summary */
    if (result->git_ctx) {
        tm_strbuf_append(&sb, "\n--- Git Context ---\n");
        tm_strbuf_appendf(&sb, "Branch: %s\n", result->git_ctx->current_branch ? result->git_ctx->current_branch : "(unknown)");
        tm_strbuf_appendf(&sb, "Commits analyzed: %zu\n", result->git_ctx->commit_count);
    }
    
    /* Call graph summary */
    if (result->call_graph) {
        tm_strbuf_append(&sb, "\n--- Call Graph ---\n");
        tm_strbuf_appendf(&sb, "Functions: %zu\n", result->call_graph->node_count);
    }
    
    /* Hypotheses */
    tm_strbuf_append(&sb, "\n=== ROOT CAUSE HYPOTHESES ===\n\n");
    
    if (result->hypothesis_count == 0) {
        tm_strbuf_append(&sb, "(No hypotheses generated)\n");
    } else {
        for (size_t i = 0; i < result->hypothesis_count; i++) {
            const tm_hypothesis_t *h = result->hypotheses[i];
            tm_strbuf_appendf(&sb, "#%d: %s (%d%% confidence)\n",
                              h->rank, h->title ? h->title : "(untitled)", h->confidence);
            if (h->explanation) {
                tm_strbuf_appendf(&sb, "  %s\n", h->explanation);
            }
            if (h->next_step) {
                tm_strbuf_appendf(&sb, "  Next step: %s\n", h->next_step);
            }
            tm_strbuf_append(&sb, "\n");
        }
    }
    
    /* Footer */
    tm_strbuf_append(&sb, "--------------------------------------------------------------------\n");
    tm_strbuf_append(&sb, "TraceMind v" TRACEMIND_VERSION_STRING " | github.com/tracemind/tracemind\n");
    tm_strbuf_append(&sb, "\n");
    
    return tm_strbuf_finish(&sb);
}

/* ============================================================================
 * Markdown Output
 * ========================================================================== */

void tm_md_hypothesis(tm_strbuf_t *sb, const tm_hypothesis_t *hyp)
{
    tm_strbuf_appendf(sb, "### #%d: %s\n\n",
                      hyp->rank, hyp->title ? hyp->title : "(No title)");
    
    tm_strbuf_appendf(sb, "**Confidence:** %d%%\n\n", hyp->confidence);
    
    if (hyp->explanation) {
        tm_strbuf_append(sb, "**Explanation:**\n");
        tm_strbuf_append(sb, hyp->explanation);
        tm_strbuf_append(sb, "\n\n");
    }
    
    if (hyp->evidence) {
        tm_strbuf_append(sb, "**Evidence:**\n> ");
        tm_strbuf_append(sb, hyp->evidence);
        tm_strbuf_append(sb, "\n\n");
    }
    
    if (hyp->next_step) {
        tm_strbuf_append(sb, "**Next Step:**\n");
        tm_strbuf_append(sb, "- [ ] ");
        tm_strbuf_append(sb, hyp->next_step);
        tm_strbuf_append(sb, "\n\n");
    }
    
    if (hyp->related_file_count > 0) {
        tm_strbuf_append(sb, "**Related Files:** ");
        for (size_t i = 0; i < hyp->related_file_count; i++) {
            if (i > 0) tm_strbuf_append(sb, ", ");
            tm_strbuf_appendf(sb, "`%s`", hyp->related_files[i] ? hyp->related_files[i] : "?");
        }
        tm_strbuf_append(sb, "\n\n");
    }
}

void tm_md_trace(tm_strbuf_t *sb, const tm_stack_trace_t *trace)
{
    tm_strbuf_append(sb, "## Stack Trace\n\n");
    
    if (trace->error_type) {
        tm_strbuf_appendf(sb, "**Error:** `%s`\n", trace->error_type);
    }
    if (trace->error_message) {
        tm_strbuf_appendf(sb, "**Message:** %s\n", trace->error_message);
    }
    tm_strbuf_appendf(sb, "**Language:** %s\n\n", tm_language_name(trace->language));
    
    tm_strbuf_append(sb, "```\n");
    for (size_t i = 0; i < trace->frame_count; i++) {
        const tm_stack_frame_t *f = &trace->frames[i];
        tm_strbuf_appendf(sb, "%zu. %s() at %s:%d\n",
                          i + 1,
                          f->function ? f->function : "<unknown>",
                          f->file ? f->file : "<unknown>",
                          f->line);
    }
    tm_strbuf_append(sb, "```\n\n");
}

void tm_md_git_context(tm_strbuf_t *sb, const tm_git_context_t *ctx)
{
    tm_strbuf_append(sb, "## Git Context\n\n");
    
    tm_strbuf_appendf(sb, "- **Branch:** %s\n", ctx->current_branch ? ctx->current_branch : "unknown");
    tm_strbuf_appendf(sb, "- **HEAD:** `%.12s`\n\n", ctx->head_sha ? ctx->head_sha : "unknown");
    
    if (ctx->commit_count > 0) {
        tm_strbuf_append(sb, "### Recent Commits\n\n");
        tm_strbuf_append(sb, "| SHA | Message | Changes |\n");
        tm_strbuf_append(sb, "|-----|---------|--------|\n");
        
        for (size_t i = 0; i < TM_MIN(ctx->commit_count, 10); i++) {
            const tm_git_commit_t *c = &ctx->commits[i];
            
            const char *msg = c->message;
            const char *nl = msg ? strchr(msg, '\n') : NULL;
            size_t msg_len = nl ? (size_t)(nl - msg) : (msg ? strlen(msg) : 0);
            if (msg_len > 60) msg_len = 60;
            
            tm_strbuf_appendf(sb, "| `%.7s` | %.*s%s | +%d/-%d |\n",
                              c->sha,
                              (int)msg_len, msg ? msg : "",
                              msg_len == 60 ? "..." : "",
                              c->additions, c->deletions);
        }
        tm_strbuf_append(sb, "\n");
    }
}

char *tm_format_markdown(const tm_formatter_t *fmt, const tm_analysis_result_t *result)
{
    (void)fmt;  /* Not used currently */
    if (!result) return tm_strdup("# Error: No result\n");
    
    tm_strbuf_t sb;
    tm_strbuf_init(&sb);
    
    tm_strbuf_append(&sb, "# TraceMind Analysis Report\n\n");
    
    if (result->error_message) {
        tm_strbuf_appendf(&sb, "> Warning: %s\n", result->error_message);
    }
    
    char *duration = tm_format_duration(result->analysis_time_ms);
    tm_strbuf_appendf(&sb, "> Analysis time: %s\n\n", duration);
    TM_FREE(duration);
    
    tm_strbuf_append(&sb, "---\n\n");
    
    if (result->trace) {
        tm_md_trace(&sb, result->trace);
    }
    
    if (result->git_ctx) {
        tm_md_git_context(&sb, result->git_ctx);
    }
    
    tm_strbuf_append(&sb, "## Root Cause Hypotheses\n\n");
    
    for (size_t i = 0; i < result->hypothesis_count; i++) {
        tm_md_hypothesis(&sb, result->hypotheses[i]);
    }
    
    tm_strbuf_append(&sb, "---\n");
    tm_strbuf_append(&sb, "*Generated by TraceMind v" TRACEMIND_VERSION_STRING "*\n");
    
    return tm_strbuf_finish(&sb);
}

/* ============================================================================
 * JSON Output
 * ========================================================================== */

char *tm_json_hypothesis(const tm_hypothesis_t *hyp)
{
    json_t *obj = json_object();
    
    json_object_set_new(obj, "rank", json_integer(hyp->rank));
    json_object_set_new(obj, "confidence", json_integer(hyp->confidence));
    json_object_set_new(obj, "title", json_string(hyp->title ? hyp->title : ""));
    json_object_set_new(obj, "explanation", json_string(hyp->explanation ? hyp->explanation : ""));
    json_object_set_new(obj, "evidence", json_string(hyp->evidence ? hyp->evidence : ""));
    json_object_set_new(obj, "next_step", json_string(hyp->next_step ? hyp->next_step : ""));
    
    json_t *files = json_array();
    for (size_t i = 0; i < hyp->related_file_count; i++) {
        json_array_append_new(files, json_string(hyp->related_files[i] ? hyp->related_files[i] : ""));
    }
    json_object_set_new(obj, "related_files", files);
    
    json_t *commits = json_array();
    for (size_t i = 0; i < hyp->related_commit_count; i++) {
        json_array_append_new(commits, json_string(hyp->related_commits[i] ? hyp->related_commits[i] : ""));
    }
    json_object_set_new(obj, "related_commits", commits);
    
    char *result = json_dumps(obj, JSON_INDENT(2));
    json_decref(obj);
    
    return result;
}

char *tm_json_trace(const tm_stack_trace_t *trace)
{
    json_t *obj = json_object();
    
    json_object_set_new(obj, "language", json_string(tm_language_name(trace->language)));
    json_object_set_new(obj, "error_type", json_string(trace->error_type ? trace->error_type : ""));
    json_object_set_new(obj, "error_message", json_string(trace->error_message ? trace->error_message : ""));
    
    json_t *frames = json_array();
    for (size_t i = 0; i < trace->frame_count; i++) {
        const tm_stack_frame_t *f = &trace->frames[i];
        json_t *frame = json_object();
        
        json_object_set_new(frame, "function", json_string(f->function ? f->function : ""));
        json_object_set_new(frame, "file", json_string(f->file ? f->file : ""));
        json_object_set_new(frame, "line", json_integer(f->line));
        json_object_set_new(frame, "column", json_integer(f->column));
        json_object_set_new(frame, "is_stdlib", json_boolean(f->is_stdlib));
        json_object_set_new(frame, "is_third_party", json_boolean(f->is_third_party));
        
        json_array_append_new(frames, frame);
    }
    json_object_set_new(obj, "frames", frames);
    
    char *result = json_dumps(obj, JSON_INDENT(2));
    json_decref(obj);
    
    return result;
}

char *tm_json_git_context(const tm_git_context_t *ctx)
{
    json_t *obj = json_object();
    
    json_object_set_new(obj, "repo_root", json_string(ctx->repo_root ? ctx->repo_root : ""));
    json_object_set_new(obj, "branch", json_string(ctx->current_branch ? ctx->current_branch : ""));
    json_object_set_new(obj, "head_sha", json_string(ctx->head_sha ? ctx->head_sha : ""));
    
    json_t *commits = json_array();
    for (size_t i = 0; i < ctx->commit_count; i++) {
        const tm_git_commit_t *c = &ctx->commits[i];
        json_t *commit = json_object();
        
        json_object_set_new(commit, "sha", json_string(c->sha));
        json_object_set_new(commit, "author", json_string(c->author ? c->author : ""));
        json_object_set_new(commit, "timestamp", json_integer(c->timestamp));
        json_object_set_new(commit, "additions", json_integer(c->additions));
        json_object_set_new(commit, "deletions", json_integer(c->deletions));
        json_object_set_new(commit, "touches_config", json_boolean(c->touches_config));
        json_object_set_new(commit, "touches_schema", json_boolean(c->touches_schema));
        
        json_array_append_new(commits, commit);
    }
    json_object_set_new(obj, "commits", commits);
    
    char *result = json_dumps(obj, JSON_INDENT(2));
    json_decref(obj);
    
    return result;
}

char *tm_format_json(const tm_formatter_t *fmt, const tm_analysis_result_t *result)
{
    (void)fmt;  /* Not used currently */
    if (!result) return tm_strdup("{\"error\": \"No result\"}");
    
    json_t *root = json_object();
    
    json_object_set_new(root, "version", json_string(TRACEMIND_VERSION_STRING));
    json_object_set_new(root, "analysis_time_ms", json_integer(result->analysis_time_ms));
    if (result->error_message) {
        json_object_set_new(root, "error", json_string(result->error_message));
    }
    
    /* Trace */
    if (result->trace) {
        json_t *trace_obj = json_object();
        json_object_set_new(trace_obj, "language", json_string(tm_language_name(result->trace->language)));
        json_object_set_new(trace_obj, "error_type", json_string(result->trace->error_type ? result->trace->error_type : ""));
        json_object_set_new(trace_obj, "error_message", json_string(result->trace->error_message ? result->trace->error_message : ""));
        json_object_set_new(trace_obj, "frame_count", json_integer((json_int_t)result->trace->frame_count));
        json_object_set_new(root, "trace", trace_obj);
    }
    
    /* Hypotheses */
    json_t *hyp_array = json_array();
    for (size_t i = 0; i < result->hypothesis_count; i++) {
        const tm_hypothesis_t *h = result->hypotheses[i];
        json_t *hyp = json_object();
        
        json_object_set_new(hyp, "rank", json_integer(h->rank));
        json_object_set_new(hyp, "confidence", json_integer(h->confidence));
        json_object_set_new(hyp, "title", json_string(h->title ? h->title : ""));
        json_object_set_new(hyp, "explanation", json_string(h->explanation ? h->explanation : ""));
        json_object_set_new(hyp, "evidence", json_string(h->evidence ? h->evidence : ""));
        json_object_set_new(hyp, "next_step", json_string(h->next_step ? h->next_step : ""));
        
        json_array_append_new(hyp_array, hyp);
    }
    json_object_set_new(root, "hypotheses", hyp_array);
    
    char *json_str = json_dumps(root, JSON_INDENT(2));
    json_decref(root);
    
    return json_str;
}

/* ============================================================================
 * Generic Export
 * ========================================================================== */

char *tm_result_export(const tm_analysis_result_t *result, tm_output_format_t format)
{
    tm_formatter_t *fmt = tm_formatter_new(format, false);
    if (!fmt) return NULL;
    
    char *output = NULL;
    switch (format) {
    case TM_OUTPUT_MARKDOWN:
        output = tm_format_markdown(fmt, result);
        break;
    case TM_OUTPUT_JSON:
        output = tm_format_json(fmt, result);
        break;
    case TM_OUTPUT_CLI:
    default:
        /* CLI is printed directly, but we can return a plain text version */
        output = tm_format_markdown(fmt, result);
        break;
    }
    
    tm_formatter_free(fmt);
    return output;
}

/* ============================================================================
 * Progress Indicators
 * ========================================================================== */

static const char *spinner_frames[] = { "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏" };
static int spinner_index = 0;

void tm_progress_spinner(const tm_formatter_t *fmt, const char *message)
{
    if (fmt->use_colors) {
        fprintf(fmt->output, "\r%s%s%s %s",
                TM_COLOR_CYAN,
                spinner_frames[spinner_index % 10],
                TM_COLOR_RESET,
                message);
    } else {
        fprintf(fmt->output, "\r[%c] %s",
                "-\\|/"[spinner_index % 4],
                message);
    }
    fflush(fmt->output);
    spinner_index++;
}

void tm_progress_bar(const tm_formatter_t *fmt, 
                     const char *label, 
                     int current, 
                     int total)
{
    if (total <= 0) return;
    
    int percent = (current * 100) / total;
    int bar_width = 30;
    int filled = (percent * bar_width) / 100;
    
    fprintf(fmt->output, "\r%s [", label);
    
    if (fmt->use_colors) {
        fprintf(fmt->output, "%s", TM_COLOR_GREEN);
    }
    
    for (int i = 0; i < filled; i++) fprintf(fmt->output, "█");
    
    if (fmt->use_colors) {
        fprintf(fmt->output, "%s", TM_COLOR_DIM);
    }
    
    for (int i = filled; i < bar_width; i++) fprintf(fmt->output, "░");
    
    if (fmt->use_colors) {
        fprintf(fmt->output, "%s", TM_COLOR_RESET);
    }
    
    fprintf(fmt->output, "] %d%%", percent);
    fflush(fmt->output);
}

/* ============================================================================
 * Table Formatting
 * ========================================================================== */

tm_table_t *tm_table_new(const tm_table_col_t *cols, size_t col_count)
{
    tm_table_t *table = tm_calloc(1, sizeof(tm_table_t));
    
    table->columns = tm_malloc(col_count * sizeof(tm_table_col_t));
    memcpy(table->columns, cols, col_count * sizeof(tm_table_col_t));
    table->col_count = col_count;
    table->rows = NULL;
    table->row_count = 0;
    table->row_capacity = 0;
    
    return table;
}

void tm_table_add_row(tm_table_t *table, ...)
{
    if (table->row_count >= table->row_capacity) {
        table->row_capacity = table->row_capacity == 0 ? 8 : table->row_capacity * 2;
        table->rows = tm_realloc(table->rows, table->row_capacity * sizeof(char **));
    }
    
    char **row = tm_malloc(table->col_count * sizeof(char *));
    
    va_list args;
    va_start(args, table);
    
    for (size_t i = 0; i < table->col_count; i++) {
        const char *val = va_arg(args, const char *);
        row[i] = val ? tm_strdup(val) : tm_strdup("");
    }
    
    va_end(args);
    
    table->rows[table->row_count++] = row;
}

void tm_table_print(const tm_formatter_t *fmt, const tm_table_t *table)
{
    if (!table || table->col_count == 0) return;
    
    /* Calculate column widths */
    int *widths = tm_calloc(table->col_count, sizeof(int));
    
    for (size_t i = 0; i < table->col_count; i++) {
        widths[i] = table->columns[i].width > 0 ? table->columns[i].width 
                    : (int)strlen(table->columns[i].header);
    }
    
    for (size_t r = 0; r < table->row_count; r++) {
        for (size_t c = 0; c < table->col_count; c++) {
            int len = (int)strlen(table->rows[r][c]);
            if (len > widths[c] && table->columns[c].width == 0) {
                widths[c] = TM_MIN(len, 50);
            }
        }
    }
    
    /* Print header */
    for (size_t i = 0; i < table->col_count; i++) {
        if (fmt->use_colors) {
            fprintf(fmt->output, "%s%-*s%s",
                    TM_COLOR_BOLD,
                    widths[i] + 2,
                    table->columns[i].header,
                    TM_COLOR_RESET);
        } else {
            fprintf(fmt->output, "%-*s", widths[i] + 2, table->columns[i].header);
        }
    }
    fprintf(fmt->output, "\n");
    
    /* Print separator */
    for (size_t i = 0; i < table->col_count; i++) {
        for (int j = 0; j < widths[i] + 2; j++) {
            fprintf(fmt->output, "-");
        }
    }
    fprintf(fmt->output, "\n");
    
    /* Print rows */
    for (size_t r = 0; r < table->row_count; r++) {
        for (size_t c = 0; c < table->col_count; c++) {
            char *truncated = tm_truncate_string(table->rows[r][c], (size_t)widths[c]);
            fprintf(fmt->output, "%-*s", widths[c] + 2, truncated);
            TM_FREE(truncated);
        }
        fprintf(fmt->output, "\n");
    }
    
    TM_FREE(widths);
}

void tm_table_free(tm_table_t *table)
{
    if (!table) return;
    
    for (size_t r = 0; r < table->row_count; r++) {
        for (size_t c = 0; c < table->col_count; c++) {
            TM_FREE(table->rows[r][c]);
        }
        free(table->rows[r]);
    }
    TM_FREE(table->rows);
    TM_FREE(table->columns);
    free(table);
}
