/**
 * TraceMind - LLM Hypothesis Engine
 * 
 * HTTP client for LLM providers (OpenAI, Anthropic, Local).
 */

#include "internal/common.h"
#include "internal/llm.h"
#include <curl/curl.h>
#include <jansson.h>
#include <unistd.h>

/* ============================================================================
 * Constants
 * ========================================================================== */

#define OPENAI_ENDPOINT "https://api.openai.com/v1/chat/completions"
#define ANTHROPIC_ENDPOINT "https://api.anthropic.com/v1/messages"
#define DEFAULT_MAX_TOKENS 4096

/* Expected JSON schema for hypothesis response */
const char *TM_HYPOTHESIS_SCHEMA = 
    "{\n"
    "  \"type\": \"object\",\n"
    "  \"properties\": {\n"
    "    \"hypotheses\": {\n"
    "      \"type\": \"array\",\n"
    "      \"items\": {\n"
    "        \"type\": \"object\",\n"
    "        \"properties\": {\n"
    "          \"rank\": {\"type\": \"integer\"},\n"
    "          \"confidence\": {\"type\": \"integer\"},\n"
    "          \"title\": {\"type\": \"string\"},\n"
    "          \"explanation\": {\"type\": \"string\"},\n"
    "          \"evidence\": {\"type\": \"string\"},\n"
    "          \"next_step\": {\"type\": \"string\"},\n"
    "          \"fix_suggestion\": {\"type\": \"string\"},\n"
    "          \"debug_commands\": {\"type\": \"array\", \"items\": {\"type\": \"string\"}},\n"
    "          \"similar_errors\": {\"type\": \"string\"}\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "}";

/* ============================================================================
 * HTTP Client Initialization
 * ========================================================================== */

static bool g_http_initialized = false;

tm_error_t tm_http_init(void)
{
    if (g_http_initialized) return TM_OK;
    
    CURLcode res = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (res != CURLE_OK) {
        TM_ERROR("Failed to initialize CURL: %s", curl_easy_strerror(res));
        return TM_ERR_INTERNAL;
    }
    
    g_http_initialized = true;
    TM_DEBUG("HTTP module initialized");
    return TM_OK;
}

void tm_http_cleanup(void)
{
    if (g_http_initialized) {
        curl_global_cleanup();
        g_http_initialized = false;
    }
}

/* ============================================================================
 * LLM Client
 * ========================================================================== */

tm_llm_client_t *tm_llm_client_new(const tm_config_t *cfg)
{
    if (!cfg) return NULL;
    
    tm_error_t err = tm_http_init();
    if (err != TM_OK) return NULL;
    
    tm_llm_client_t *client = tm_calloc(1, sizeof(tm_llm_client_t));
    
    client->provider = cfg->llm_provider;
    client->api_key = cfg->api_key ? tm_strdup(cfg->api_key) : NULL;
    client->model = cfg->model_name ? tm_strdup(cfg->model_name) : tm_strdup("gpt-4o");
    client->timeout_ms = cfg->timeout_ms > 0 ? cfg->timeout_ms : 60000;
    client->temperature = cfg->temperature >= 0 ? cfg->temperature : 0.3f;
    
    /* Set endpoint */
    if (cfg->api_endpoint) {
        client->endpoint = tm_strdup(cfg->api_endpoint);
    } else {
        switch (cfg->llm_provider) {
        case TM_LLM_OPENAI:
            client->endpoint = tm_strdup(OPENAI_ENDPOINT);
            break;
        case TM_LLM_ANTHROPIC:
            client->endpoint = tm_strdup(ANTHROPIC_ENDPOINT);
            break;
        case TM_LLM_LOCAL:
            client->endpoint = tm_strdup("http://localhost:11434/api/chat");
            break;
        }
    }
    
    /* Create CURL handle */
    client->curl = curl_easy_init();
    if (!client->curl) {
        TM_ERROR("Failed to create CURL handle");
        tm_llm_client_free(client);
        return NULL;
    }
    
    return client;
}

void tm_llm_client_free(tm_llm_client_t *client)
{
    if (!client) return;
    
    if (client->curl) curl_easy_cleanup(client->curl);
    TM_FREE(client->api_key);
    TM_FREE(client->endpoint);
    TM_FREE(client->model);
    free(client);
}

/* ============================================================================
 * System & Analysis Prompts
 * ========================================================================== */

char *tm_build_system_prompt(void)
{
    return tm_strdup(
        "You are TraceMind, an expert backend debugging assistant. Your role is to analyze "
        "stack traces, code context, and git history to identify the most probable root causes "
        "of errors and provide actionable fixes.\n\n"
        
        "CRITICAL RULES:\n"
        "1. Output EXACTLY 3 hypotheses, ranked by probability\n"
        "2. Each hypothesis must have a confidence percentage (0-100)\n"
        "3. Be specific — reference actual file names, line numbers, and function names\n"
        "4. Focus on the most recent code changes when relevant\n"
        "5. Consider configuration and schema changes as high-priority suspects\n"
        "6. ALWAYS provide a concrete fix_suggestion with code/config changes\n"
        "7. ALWAYS provide debug_commands — shell commands to investigate further\n"
        "8. ALWAYS provide similar_errors — common causes for this error pattern\n\n"
        
        "OUTPUT FORMAT (JSON):\n"
        "{\n"
        "  \"hypotheses\": [\n"
        "    {\n"
        "      \"rank\": 1,\n"
        "      \"confidence\": 85,\n"
        "      \"title\": \"Short descriptive title\",\n"
        "      \"explanation\": \"Detailed explanation of why this might be the cause\",\n"
        "      \"evidence\": \"Specific evidence from the trace/code/git history\",\n"
        "      \"next_step\": \"Specific action to validate this hypothesis\",\n"
        "      \"fix_suggestion\": \"Concrete code change or config fix. Include file:line and a diff/patch snippet if possible.\",\n"
        "      \"debug_commands\": [\"git log --oneline -5 -- path/to/file.py\", "
                                    "\"grep -n 'pattern' file.py\", "
                                    "\"curl -v http://localhost:8080/health\"],\n"
        "      \"similar_errors\": \"This error commonly occurs when X. Other causes include Y and Z.\",\n"
        "      \"related_files\": [\"file1.py\", \"file2.py\"],\n"
        "      \"related_commits\": [\"abc123\"]\n"
        "    }\n"
        "  ]\n"
        "}\n\n"
        
        "FIX SUGGESTION GUIDELINES:\n"
        "- Include the exact file and line to change\n"
        "- Show a before/after code diff when possible\n"
        "- If the fix is a config change, show the exact config key and value\n"
        "- If the fix requires a migration, outline the steps\n\n"
        
        "DEBUG COMMANDS GUIDELINES:\n"
        "- Provide 2-4 shell commands the user can run immediately\n"
        "- Include commands to verify the hypothesis (grep, git log, curl, etc.)\n"
        "- Include commands to check system state (ps, netstat, df, etc.)\n"
        "- Prefer standard POSIX tools\n\n"
        
        "ANALYSIS PRIORITIES:\n"
        "1. Exact error location and type\n"
        "2. Recent commits touching error-adjacent code\n"
        "3. Configuration or environment changes\n"
        "4. Third-party dependency issues\n"
        "5. Race conditions or state management issues"
    );
}

char *tm_build_analysis_prompt(const tm_analysis_context_t *ctx)
{
    if (!ctx) return NULL;
    
    tm_strbuf_t sb;
    tm_strbuf_init(&sb);
    
    /* Stack trace section */
    tm_strbuf_append(&sb, "## STACK TRACE\n\n");
    
    if (ctx->trace) {
        if (ctx->trace->error_type) {
            tm_strbuf_appendf(&sb, "**Error Type:** %s\n", ctx->trace->error_type);
        }
        if (ctx->trace->error_message) {
            tm_strbuf_appendf(&sb, "**Message:** %s\n", ctx->trace->error_message);
        }
        tm_strbuf_appendf(&sb, "**Language:** %s\n\n", 
                          tm_language_name(ctx->trace->language));
        
        tm_strbuf_append(&sb, "**Frames:**\n```\n");
        for (size_t i = 0; i < ctx->trace->frame_count && i < 20; i++) {
            const tm_stack_frame_t *f = &ctx->trace->frames[i];
            tm_strbuf_appendf(&sb, "%zu. %s() at %s:%d",
                              i + 1,
                              f->function ? f->function : "<unknown>",
                              f->file ? f->file : "<unknown>",
                              f->line);
            if (f->is_stdlib) tm_strbuf_append(&sb, " [stdlib]");
            if (f->is_third_party) tm_strbuf_append(&sb, " [third-party]");
            tm_strbuf_append(&sb, "\n");
        }
        tm_strbuf_append(&sb, "```\n\n");
    }
    
    /* Call graph section */
    if (ctx->call_graph && ctx->call_graph->node_count > 0) {
        tm_strbuf_append(&sb, "## CALL GRAPH\n\n");
        tm_strbuf_append(&sb, "**Functions in error path:**\n");
        
        for (size_t i = 0; i < ctx->call_graph->node_count && i < 10; i++) {
            const tm_call_node_t *node = ctx->call_graph->nodes[i];
            tm_strbuf_appendf(&sb, "- `%s` (%s:%d-%d)",
                              node->name ? node->name : "<unknown>",
                              node->file ? node->file : "<unknown>",
                              node->start_line, node->end_line);
            if (node->complexity > 5) {
                tm_strbuf_appendf(&sb, " [complexity: %u]", node->complexity);
            }
            tm_strbuf_append(&sb, "\n");
        }
        tm_strbuf_append(&sb, "\n");
    }
    
    /* Git context section */
    if (ctx->git_ctx) {
        tm_strbuf_append(&sb, "## GIT CONTEXT\n\n");
        tm_strbuf_appendf(&sb, "**Branch:** %s\n", 
                          ctx->git_ctx->current_branch ? ctx->git_ctx->current_branch : "unknown");
        tm_strbuf_appendf(&sb, "**HEAD:** %s\n\n", 
                          ctx->git_ctx->head_sha ? ctx->git_ctx->head_sha : "unknown");
        
        if (ctx->git_ctx->commit_count > 0) {
            tm_strbuf_append(&sb, "**Recent commits affecting error files:**\n");
            
            for (size_t i = 0; i < ctx->git_ctx->commit_count && i < 10; i++) {
                const tm_git_commit_t *c = &ctx->git_ctx->commits[i];
                
                /* Get first line of message */
                const char *msg = c->message;
                const char *newline = msg ? strchr(msg, '\n') : NULL;
                size_t msg_len = newline ? (size_t)(newline - msg) : (msg ? strlen(msg) : 0);
                if (msg_len > 80) msg_len = 80;
                
                tm_strbuf_appendf(&sb, "- `%.7s` ", c->sha);
                if (msg && msg_len > 0) {
                    char *truncated = tm_strndup(msg, msg_len);
                    tm_strbuf_append(&sb, truncated);
                    TM_FREE(truncated);
                }
                
                if (c->touches_config) tm_strbuf_append(&sb, " **[CONFIG]**");
                if (c->touches_schema) tm_strbuf_append(&sb, " **[SCHEMA]**");
                
                tm_strbuf_appendf(&sb, " (+%d/-%d)\n", c->additions, c->deletions);
            }
            tm_strbuf_append(&sb, "\n");
        }
        
        if (ctx->git_ctx->blame_count > 0) {
            tm_strbuf_append(&sb, "**Blame info for error lines:**\n");
            for (size_t i = 0; i < ctx->git_ctx->blame_count; i++) {
                const tm_git_blame_t *b = ctx->git_ctx->blames[i];
                if (b) {
                    tm_strbuf_appendf(&sb, "- Line by %s (commit %.7s)\n",
                                      b->author ? b->author : "unknown",
                                      b->sha);
                }
            }
            tm_strbuf_append(&sb, "\n");
        }
    }
    
    /* Additional context */
    if (ctx->additional_context) {
        tm_strbuf_append(&sb, "## ADDITIONAL CONTEXT\n\n");
        tm_strbuf_append(&sb, ctx->additional_context);
        tm_strbuf_append(&sb, "\n\n");
    }
    
    tm_strbuf_append(&sb, "---\n\n");
    tm_strbuf_append(&sb, "Analyze the above information and provide your root cause hypotheses "
                          "in the specified JSON format.");
    
    return tm_strbuf_finish(&sb);
}

/* ============================================================================
 * Generic Log Analysis Prompts (Format-Agnostic)
 * ========================================================================== */

char *tm_build_generic_system_prompt(tm_log_format_t format)
{
    const char *format_name = tm_log_format_name(format);
    
    tm_strbuf_t sb;
    tm_strbuf_init(&sb);
    
    tm_strbuf_append(&sb,
        "You are TraceMind, an expert log analysis assistant. Your role is to analyze "
        "logs of any format to identify errors, anomalies, and root causes.\n\n"
        
        "DETECTED LOG FORMAT: ");
    tm_strbuf_append(&sb, format_name);
    tm_strbuf_append(&sb, "\n\n"
        
        "ANALYSIS MODES:\n"
        "1. ERROR DIAGNOSIS - Identify root cause of errors/failures\n"
        "2. ANOMALY DETECTION - Identify unusual patterns or behaviors\n"
        "3. CORRELATION - Link related events across log entries\n"
        "4. PERFORMANCE - Identify latency issues, resource problems\n\n"
        
        "CRITICAL RULES:\n"
        "1. Output EXACTLY 3 hypotheses (or findings), ranked by probability/impact\n"
        "2. Each must have a confidence percentage (0-100)\n"
        "3. Be specific - reference actual log entries, timestamps, and patterns\n"
        "4. Identify patterns across multiple entries when relevant\n"
        "5. Consider timing correlations and cascading failures\n"
        "6. Provide actionable 'Next Step' investigation suggestions\n\n"
        
        "OUTPUT FORMAT (JSON):\n"
        "{\n"
        "  \"analysis_type\": \"error_diagnosis|anomaly|correlation|performance\",\n"
        "  \"hypotheses\": [\n"
        "    {\n"
        "      \"rank\": 1,\n"
        "      \"confidence\": 85,\n"
        "      \"title\": \"Short descriptive title\",\n"
        "      \"explanation\": \"Detailed explanation of the finding\",\n"
        "      \"evidence\": \"Specific log entries/patterns that support this\",\n"
        "      \"next_step\": \"Specific action to investigate or fix\",\n"
        "      \"related_entries\": [1, 5, 12]\n"
        "    }\n"
        "  ]\n"
        "}\n\n"
        
        "ANALYSIS PRIORITIES:\n"
        "1. Error messages and their immediate context\n"
        "2. Timing patterns (rapid succession, periodic failures)\n"
        "3. Resource indicators (memory, connections, timeouts)\n"
        "4. Service dependencies and cascading effects\n"
        "5. Configuration or deployment indicators"
    );
    
    return tm_strbuf_finish(&sb);
}

char *tm_build_generic_log_prompt(const tm_generic_analysis_ctx_t *ctx)
{
    if (!ctx || !ctx->log) return NULL;
    
    const tm_generic_log_t *log = ctx->log;
    
    tm_strbuf_t sb;
    tm_strbuf_init(&sb);
    
    /* Log summary section */
    tm_strbuf_append(&sb, "## LOG SUMMARY\n\n");
    tm_strbuf_appendf(&sb, "**Format:** %s\n", 
                      log->format_description ? log->format_description : "unknown");
    tm_strbuf_appendf(&sb, "**Total Entries:** %zu\n", log->count);
    tm_strbuf_appendf(&sb, "**Errors:** %zu\n", log->total_errors);
    tm_strbuf_appendf(&sb, "**Warnings:** %zu\n", log->total_warnings);
    
    if (log->time_range_start && log->time_range_end) {
        tm_strbuf_appendf(&sb, "**Time Range:** %s to %s\n",
                          log->time_range_start, log->time_range_end);
    }
    tm_strbuf_append(&sb, "\n");
    
    /* Log entries section */
    tm_strbuf_append(&sb, "## LOG ENTRIES\n\n");
    
    size_t max_entries = ctx->max_entries > 0 ? ctx->max_entries : 100;
    size_t shown = 0;
    
    for (size_t i = 0; i < log->count && shown < max_entries; i++) {
        const tm_generic_log_entry_t *e = &log->entries[i];
        
        /* Filter to errors only if requested */
        if (ctx->errors_only && !e->is_error && !e->is_anomaly) {
            continue;
        }
        
        tm_strbuf_appendf(&sb, "**[%zu]**", e->line_number);
        
        if (e->timestamp) {
            tm_strbuf_appendf(&sb, " `%s`", e->timestamp);
        }
        if (e->severity) {
            tm_strbuf_appendf(&sb, " **%s**", e->severity);
        }
        if (e->source) {
            tm_strbuf_appendf(&sb, " (%s)", e->source);
        }
        
        /* Relevance indicator */
        if (e->is_error) {
            tm_strbuf_append(&sb, " [ERROR]");
        } else if (e->is_anomaly) {
            tm_strbuf_append(&sb, " [ANOMALY]");
        }
        
        tm_strbuf_append(&sb, "\n");
        
        /* Message or raw line */
        if (ctx->include_raw_lines && e->raw_line) {
            tm_strbuf_append(&sb, "```\n");
            tm_strbuf_append(&sb, e->raw_line);
            tm_strbuf_append(&sb, "\n```\n");
        } else if (e->message) {
            tm_strbuf_append(&sb, e->message);
            tm_strbuf_append(&sb, "\n");
        }
        
        tm_strbuf_append(&sb, "\n");
        shown++;
    }
    
    if (shown < log->count) {
        tm_strbuf_appendf(&sb, "*(... %zu more entries omitted)*\n\n", 
                          log->count - shown);
    }
    
    /* Git context if available */
    if (ctx->git_ctx && ctx->git_ctx->commit_count > 0) {
        tm_strbuf_append(&sb, "## RECENT CHANGES\n\n");
        tm_strbuf_appendf(&sb, "**Branch:** %s\n\n",
                          ctx->git_ctx->current_branch ? ctx->git_ctx->current_branch : "unknown");
        
        tm_strbuf_append(&sb, "**Recent commits:**\n");
        for (size_t i = 0; i < ctx->git_ctx->commit_count && i < 5; i++) {
            const tm_git_commit_t *c = &ctx->git_ctx->commits[i];
            const char *msg = c->message;
            const char *nl = msg ? strchr(msg, '\n') : NULL;
            size_t msg_len = nl ? (size_t)(nl - msg) : (msg ? strlen(msg) : 0);
            if (msg_len > 60) msg_len = 60;
            
            tm_strbuf_appendf(&sb, "- `%.7s` ", c->sha);
            if (msg && msg_len > 0) {
                char *truncated = tm_strndup(msg, msg_len);
                tm_strbuf_append(&sb, truncated);
                TM_FREE(truncated);
            }
            if (c->touches_config) tm_strbuf_append(&sb, " **[CONFIG]**");
            tm_strbuf_append(&sb, "\n");
        }
        tm_strbuf_append(&sb, "\n");
    }
    
    /* Additional context */
    if (ctx->additional_context) {
        tm_strbuf_append(&sb, "## ADDITIONAL CONTEXT\n\n");
        tm_strbuf_append(&sb, ctx->additional_context);
        tm_strbuf_append(&sb, "\n\n");
    }
    
    tm_strbuf_append(&sb, "---\n\n");
    tm_strbuf_append(&sb, "Analyze the above log entries and provide your findings/hypotheses "
                          "in the specified JSON format. Focus on identifying the root cause of "
                          "any errors and notable patterns.");
    
    return tm_strbuf_finish(&sb);
}

/* ============================================================================
 * CURL Response Buffer
 * ========================================================================== */

typedef struct {
    char *data;
    size_t size;
} response_buffer_t;

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    response_buffer_t *buf = (response_buffer_t *)userp;
    
    buf->data = tm_realloc(buf->data, buf->size + realsize + 1);
    memcpy(buf->data + buf->size, contents, realsize);
    buf->size += realsize;
    buf->data[buf->size] = '\0';
    
    return realsize;
}

/* ============================================================================
 * Provider-Specific Request Building
 * ========================================================================== */

char *tm_openai_build_request(const tm_chat_request_t *request, const char *model)
{
    json_t *root = json_object();
    
    json_object_set_new(root, "model", json_string(model));
    json_object_set_new(root, "max_tokens", 
                        json_integer(request->max_tokens > 0 ? request->max_tokens : DEFAULT_MAX_TOKENS));
    json_object_set_new(root, "temperature", json_real(request->temperature));
    
    /* Response format for structured output */
    json_t *response_format = json_object();
    json_object_set_new(response_format, "type", json_string("json_object"));
    json_object_set_new(root, "response_format", response_format);
    
    /* Messages array */
    json_t *messages = json_array();
    for (size_t i = 0; i < request->message_count; i++) {
        json_t *msg = json_object();
        
        const char *role;
        switch (request->messages[i].role) {
        case TM_ROLE_SYSTEM: role = "system"; break;
        case TM_ROLE_USER: role = "user"; break;
        case TM_ROLE_ASSISTANT: role = "assistant"; break;
        default: role = "user"; break;
        }
        
        json_object_set_new(msg, "role", json_string(role));
        json_object_set_new(msg, "content", json_string(request->messages[i].content));
        json_array_append_new(messages, msg);
    }
    json_object_set_new(root, "messages", messages);
    
    char *result = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    
    return result;
}

char *tm_anthropic_build_request(const tm_chat_request_t *request, const char *model)
{
    json_t *root = json_object();
    
    json_object_set_new(root, "model", json_string(model));
    json_object_set_new(root, "max_tokens", 
                        json_integer(request->max_tokens > 0 ? request->max_tokens : DEFAULT_MAX_TOKENS));
    
    /* Extract system message */
    for (size_t i = 0; i < request->message_count; i++) {
        if (request->messages[i].role == TM_ROLE_SYSTEM) {
            json_object_set_new(root, "system", json_string(request->messages[i].content));
            break;
        }
    }
    
    /* Messages array (non-system) */
    json_t *messages = json_array();
    for (size_t i = 0; i < request->message_count; i++) {
        if (request->messages[i].role == TM_ROLE_SYSTEM) continue;
        
        json_t *msg = json_object();
        
        const char *role = request->messages[i].role == TM_ROLE_ASSISTANT ? "assistant" : "user";
        
        json_object_set_new(msg, "role", json_string(role));
        json_object_set_new(msg, "content", json_string(request->messages[i].content));
        json_array_append_new(messages, msg);
    }
    json_object_set_new(root, "messages", messages);
    
    char *result = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    
    return result;
}

/* ============================================================================
 * Provider-Specific Response Parsing
 * ========================================================================== */

tm_error_t tm_openai_parse_response(const char *json_str, tm_chat_response_t **response)
{
    TM_CHECK_NULL(json_str, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(response, TM_ERR_INVALID_ARG);
    
    json_error_t error;
    json_t *root = json_loads(json_str, 0, &error);
    
    if (!root) {
        TM_ERROR("JSON parse error: %s", error.text);
        return TM_ERR_PARSE;
    }
    
    /* Check for error response */
    json_t *err_obj = json_object_get(root, "error");
    if (err_obj) {
        json_t *msg = json_object_get(err_obj, "message");
        if (msg && json_is_string(msg)) {
            TM_ERROR("OpenAI API error: %s", json_string_value(msg));
        }
        json_decref(root);
        return TM_ERR_LLM;
    }
    
    tm_chat_response_t *resp = tm_calloc(1, sizeof(tm_chat_response_t));
    
    /* Extract content from choices[0].message.content */
    json_t *choices = json_object_get(root, "choices");
    if (choices && json_array_size(choices) > 0) {
        json_t *choice = json_array_get(choices, 0);
        json_t *message = json_object_get(choice, "message");
        json_t *content = json_object_get(message, "content");
        
        if (content && json_is_string(content)) {
            resp->content = tm_strdup(json_string_value(content));
        }
        
        json_t *finish = json_object_get(choice, "finish_reason");
        if (finish && json_is_string(finish)) {
            resp->finish_reason = tm_strdup(json_string_value(finish));
        }
    }
    
    /* Extract usage */
    json_t *usage = json_object_get(root, "usage");
    if (usage) {
        json_t *prompt = json_object_get(usage, "prompt_tokens");
        json_t *completion = json_object_get(usage, "completion_tokens");
        
        if (prompt && json_is_integer(prompt)) {
            resp->prompt_tokens = (int)json_integer_value(prompt);
        }
        if (completion && json_is_integer(completion)) {
            resp->completion_tokens = (int)json_integer_value(completion);
        }
    }
    
    /* Extract model */
    json_t *model = json_object_get(root, "model");
    if (model && json_is_string(model)) {
        resp->model = tm_strdup(json_string_value(model));
    }
    
    json_decref(root);
    
    if (!resp->content) {
        tm_chat_response_free(resp);
        return TM_ERR_PARSE;
    }
    
    *response = resp;
    return TM_OK;
}

tm_error_t tm_anthropic_parse_response(const char *json_str, tm_chat_response_t **response)
{
    TM_CHECK_NULL(json_str, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(response, TM_ERR_INVALID_ARG);
    
    json_error_t error;
    json_t *root = json_loads(json_str, 0, &error);
    
    if (!root) {
        TM_ERROR("JSON parse error: %s", error.text);
        return TM_ERR_PARSE;
    }
    
    /* Check for error */
    json_t *err_type = json_object_get(root, "type");
    if (err_type && json_is_string(err_type) && 
        strcmp(json_string_value(err_type), "error") == 0) {
        json_t *err = json_object_get(root, "error");
        json_t *msg = json_object_get(err, "message");
        if (msg && json_is_string(msg)) {
            TM_ERROR("Anthropic API error: %s", json_string_value(msg));
        }
        json_decref(root);
        return TM_ERR_LLM;
    }
    
    tm_chat_response_t *resp = tm_calloc(1, sizeof(tm_chat_response_t));
    
    /* Extract content from content[0].text */
    json_t *content_arr = json_object_get(root, "content");
    if (content_arr && json_array_size(content_arr) > 0) {
        json_t *content_obj = json_array_get(content_arr, 0);
        json_t *text = json_object_get(content_obj, "text");
        
        if (text && json_is_string(text)) {
            resp->content = tm_strdup(json_string_value(text));
        }
    }
    
    /* Extract stop reason */
    json_t *stop = json_object_get(root, "stop_reason");
    if (stop && json_is_string(stop)) {
        resp->finish_reason = tm_strdup(json_string_value(stop));
    }
    
    /* Extract usage */
    json_t *usage = json_object_get(root, "usage");
    if (usage) {
        json_t *input = json_object_get(usage, "input_tokens");
        json_t *output = json_object_get(usage, "output_tokens");
        
        if (input && json_is_integer(input)) {
            resp->prompt_tokens = (int)json_integer_value(input);
        }
        if (output && json_is_integer(output)) {
            resp->completion_tokens = (int)json_integer_value(output);
        }
    }
    
    /* Extract model */
    json_t *model = json_object_get(root, "model");
    if (model && json_is_string(model)) {
        resp->model = tm_strdup(json_string_value(model));
    }
    
    json_decref(root);
    
    if (!resp->content) {
        tm_chat_response_free(resp);
        return TM_ERR_PARSE;
    }
    
    *response = resp;
    return TM_OK;
}

void tm_chat_response_free(tm_chat_response_t *response)
{
    if (!response) return;
    
    TM_FREE(response->content);
    TM_FREE(response->model);
    TM_FREE(response->finish_reason);
    free(response);
}

/* ============================================================================
 * Main LLM Chat Function
 * ========================================================================== */

tm_error_t tm_llm_chat(tm_llm_client_t *client,
                       const tm_chat_request_t *request,
                       tm_chat_response_t **response)
{
    TM_CHECK_NULL(client, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(client->curl, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(request, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(response, TM_ERR_INVALID_ARG);
    
    *response = NULL;
    
    /* Build request body */
    char *body = NULL;
    switch (client->provider) {
    case TM_LLM_OPENAI:
    case TM_LLM_LOCAL:
        body = tm_openai_build_request(request, client->model);
        break;
    case TM_LLM_ANTHROPIC:
        body = tm_anthropic_build_request(request, client->model);
        break;
    }
    
    if (!body) {
        TM_ERROR("Failed to build request body");
        return TM_ERR_INTERNAL;
    }
    
    TM_DEBUG("Request body: %.200s...", body);
    
    /* Set up CURL */
    CURL *curl = client->curl;
    curl_easy_reset(curl);
    
    /* Headers */
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    
    char auth_header[256];
    switch (client->provider) {
    case TM_LLM_OPENAI:
    case TM_LLM_LOCAL:
        if (client->api_key) {
            snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", client->api_key);
            headers = curl_slist_append(headers, auth_header);
        }
        break;
    case TM_LLM_ANTHROPIC:
        if (client->api_key) {
            snprintf(auth_header, sizeof(auth_header), "x-api-key: %s", client->api_key);
            headers = curl_slist_append(headers, auth_header);
        }
        headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
        break;
    }
    
    /* Response buffer */
    response_buffer_t buf = { .data = NULL, .size = 0 };
    
    curl_easy_setopt(curl, CURLOPT_URL, client->endpoint);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)client->timeout_ms);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    
    /* Perform request */
    CURLcode res = curl_easy_perform(curl);
    
    curl_slist_free_all(headers);
    TM_FREE(body);
    
    if (res != CURLE_OK) {
        TM_ERROR("CURL error: %s", curl_easy_strerror(res));
        TM_FREE(buf.data);
        
        if (res == CURLE_OPERATION_TIMEDOUT) {
            return TM_ERR_TIMEOUT;
        }
        return TM_ERR_LLM;
    }
    
    /* Check HTTP status */
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    
    TM_DEBUG("HTTP response: %ld, body: %.200s...", http_code, buf.data ? buf.data : "");
    
    if (http_code != 200) {
        TM_ERROR("HTTP error: %ld", http_code);
        TM_FREE(buf.data);
        return TM_ERR_LLM;
    }
    
    /* Parse response */
    tm_error_t err;
    switch (client->provider) {
    case TM_LLM_OPENAI:
    case TM_LLM_LOCAL:
        err = tm_openai_parse_response(buf.data, response);
        break;
    case TM_LLM_ANTHROPIC:
        err = tm_anthropic_parse_response(buf.data, response);
        break;
    default:
        err = TM_ERR_INTERNAL;
    }
    
    TM_FREE(buf.data);
    return err;
}

/* ============================================================================
 * Retry Logic
 * ========================================================================== */

tm_retry_config_t tm_default_retry_config(void)
{
    return (tm_retry_config_t){
        .max_retries = 3,
        .initial_delay_ms = 1000,
        .max_delay_ms = 30000,
        .backoff_multiplier = 2.0f
    };
}

tm_error_t tm_llm_chat_with_retry(tm_llm_client_t *client,
                                  const tm_chat_request_t *request,
                                  const tm_retry_config_t *retry_cfg,
                                  tm_chat_response_t **response)
{
    TM_CHECK_NULL(client, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(request, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(response, TM_ERR_INVALID_ARG);
    
    tm_retry_config_t cfg = retry_cfg ? *retry_cfg : tm_default_retry_config();
    
    int delay_ms = cfg.initial_delay_ms;
    tm_error_t err = TM_OK;
    
    for (int attempt = 0; attempt <= cfg.max_retries; attempt++) {
        if (attempt > 0) {
            TM_WARN("Retrying LLM request (attempt %d/%d) after %dms...",
                    attempt, cfg.max_retries, delay_ms);
            usleep((useconds_t)delay_ms * 1000);
            delay_ms = TM_MIN((int)(delay_ms * cfg.backoff_multiplier), cfg.max_delay_ms);
        }
        
        err = tm_llm_chat(client, request, response);
        
        if (err == TM_OK) return TM_OK;
        
        /* Don't retry on non-transient errors */
        if (err != TM_ERR_TIMEOUT && err != TM_ERR_LLM) {
            return err;
        }
    }
    
    return err;
}

/* ============================================================================
 * Hypothesis Parsing
 * ========================================================================== */

void tm_hypothesis_free(tm_hypothesis_t *h)
{
    if (!h) return;
    
    TM_FREE(h->title);
    TM_FREE(h->explanation);
    TM_FREE(h->evidence);
    TM_FREE(h->next_step);
    TM_FREE(h->fix_suggestion);
    TM_FREE(h->similar_errors);
    
    for (size_t j = 0; j < h->debug_command_count; j++) {
        TM_FREE(h->debug_commands[j]);
    }
    TM_FREE(h->debug_commands);
    
    for (size_t j = 0; j < h->related_file_count; j++) {
        TM_FREE(h->related_files[j]);
    }
    TM_FREE(h->related_files);
    
    for (size_t j = 0; j < h->related_commit_count; j++) {
        TM_FREE(h->related_commits[j]);
    }
    TM_FREE(h->related_commits);
    
    free(h);
}

tm_error_t tm_parse_hypotheses(const char *response_text,
                               tm_hypothesis_t ***hypotheses,
                               size_t *count)
{
    TM_CHECK_NULL(response_text, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(hypotheses, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(count, TM_ERR_INVALID_ARG);
    
    *hypotheses = NULL;
    *count = 0;
    
    json_error_t error;
    json_t *root = json_loads(response_text, 0, &error);
    
    if (!root) {
        TM_ERROR("Failed to parse hypothesis JSON: %s", error.text);
        return TM_ERR_PARSE;
    }
    
    json_t *hyp_array = json_object_get(root, "hypotheses");
    if (!hyp_array || !json_is_array(hyp_array)) {
        TM_ERROR("Missing 'hypotheses' array in response");
        json_decref(root);
        return TM_ERR_PARSE;
    }
    
    size_t array_size = json_array_size(hyp_array);
    if (array_size == 0) {
        json_decref(root);
        return TM_OK;
    }
    
    tm_hypothesis_t **result = tm_calloc(array_size, sizeof(tm_hypothesis_t *));
    
    for (size_t i = 0; i < array_size; i++) {
        json_t *hyp = json_array_get(hyp_array, i);
        tm_hypothesis_t *h = tm_calloc(1, sizeof(tm_hypothesis_t));
        result[i] = h;
        
        json_t *val;
        
        val = json_object_get(hyp, "rank");
        h->rank = val && json_is_integer(val) ? (int)json_integer_value(val) : (int)(i + 1);
        
        val = json_object_get(hyp, "confidence");
        h->confidence = val && json_is_integer(val) ? (int)json_integer_value(val) : 50;
        
        val = json_object_get(hyp, "title");
        h->title = val && json_is_string(val) ? tm_strdup(json_string_value(val)) : NULL;
        
        val = json_object_get(hyp, "explanation");
        h->explanation = val && json_is_string(val) ? tm_strdup(json_string_value(val)) : NULL;
        
        val = json_object_get(hyp, "evidence");
        h->evidence = val && json_is_string(val) ? tm_strdup(json_string_value(val)) : NULL;
        
        val = json_object_get(hyp, "next_step");
        h->next_step = val && json_is_string(val) ? tm_strdup(json_string_value(val)) : NULL;
        
        /* Fix suggestion */
        val = json_object_get(hyp, "fix_suggestion");
        h->fix_suggestion = val && json_is_string(val) ? tm_strdup(json_string_value(val)) : NULL;
        
        /* Debug commands */
        val = json_object_get(hyp, "debug_commands");
        if (val && json_is_array(val)) {
            h->debug_command_count = json_array_size(val);
            if (h->debug_command_count > 0) {
                h->debug_commands = tm_malloc(h->debug_command_count * sizeof(char *));
                for (size_t j = 0; j < h->debug_command_count; j++) {
                    json_t *cmd = json_array_get(val, j);
                    h->debug_commands[j] = json_is_string(cmd) ? tm_strdup(json_string_value(cmd)) : NULL;
                }
            }
        }
        
        /* Similar errors */
        val = json_object_get(hyp, "similar_errors");
        h->similar_errors = val && json_is_string(val) ? tm_strdup(json_string_value(val)) : NULL;
        
        /* Related files */
        val = json_object_get(hyp, "related_files");
        if (val && json_is_array(val)) {
            h->related_file_count = json_array_size(val);
            if (h->related_file_count > 0) {
                h->related_files = tm_malloc(h->related_file_count * sizeof(char *));
                for (size_t j = 0; j < h->related_file_count; j++) {
                    json_t *f = json_array_get(val, j);
                    h->related_files[j] = json_is_string(f) ? tm_strdup(json_string_value(f)) : NULL;
                }
            }
        }
        
        /* Related commits */
        val = json_object_get(hyp, "related_commits");
        if (val && json_is_array(val)) {
            h->related_commit_count = json_array_size(val);
            if (h->related_commit_count > 0) {
                h->related_commits = tm_malloc(h->related_commit_count * sizeof(char *));
                for (size_t j = 0; j < h->related_commit_count; j++) {
                    json_t *c = json_array_get(val, j);
                    h->related_commits[j] = json_is_string(c) ? tm_strdup(json_string_value(c)) : NULL;
                }
            }
        }
        
        (*count)++;
    }
    
    json_decref(root);
    *hypotheses = result;
    
    TM_DEBUG("Parsed %zu hypotheses", *count);
    return TM_OK;
}

void tm_hypotheses_free(tm_hypothesis_t **hypotheses, size_t count)
{
    if (!hypotheses) return;
    
    for (size_t i = 0; i < count; i++) {
        tm_hypothesis_free(hypotheses[i]);
    }
    free(hypotheses);
}

/* ============================================================================
 * Main Hypothesis Generation
 * ========================================================================== */

tm_error_t tm_llm_generate_hypotheses(tm_llm_client_t *client,
                                      const tm_stack_trace_t *trace,
                                      const tm_call_graph_t *call_graph,
                                      const tm_git_context_t *git_ctx,
                                      tm_hypothesis_t ***hypotheses,
                                      size_t *count)
{
    TM_CHECK_NULL(client, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(hypotheses, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(count, TM_ERR_INVALID_ARG);
    
    *hypotheses = NULL;
    *count = 0;
    
    /* Build analysis context */
    tm_analysis_context_t ctx = {
        .trace = trace,
        .call_graph = call_graph,
        .git_ctx = git_ctx,
        .additional_context = NULL
    };
    
    /* Build prompts */
    char *system_prompt = tm_build_system_prompt();
    char *user_prompt = tm_build_analysis_prompt(&ctx);
    
    if (!system_prompt || !user_prompt) {
        TM_FREE(system_prompt);
        TM_FREE(user_prompt);
        return TM_ERR_NOMEM;
    }
    
    /* Build chat request */
    tm_chat_message_t messages[2] = {
        { .role = TM_ROLE_SYSTEM, .content = system_prompt },
        { .role = TM_ROLE_USER, .content = user_prompt }
    };
    
    tm_chat_request_t request = {
        .messages = messages,
        .message_count = 2,
        .max_tokens = DEFAULT_MAX_TOKENS,
        .temperature = client->temperature
    };
    
    /* Send request with retry */
    tm_retry_config_t retry_cfg = tm_default_retry_config();
    tm_chat_response_t *response = NULL;
    
    tm_error_t err = tm_llm_chat_with_retry(client, &request, &retry_cfg, &response);
    
    TM_FREE(system_prompt);
    TM_FREE(user_prompt);
    
    if (err != TM_OK) {
        TM_ERROR("LLM request failed: %s", tm_strerror(err));
        return err;
    }
    
    if (!response || !response->content) {
        tm_chat_response_free(response);
        return TM_ERR_LLM;
    }
    
    TM_DEBUG("Received LLM response: %d tokens", response->completion_tokens);
    
    /* Parse hypotheses from response */
    err = tm_parse_hypotheses(response->content, hypotheses, count);
    
    tm_chat_response_free(response);
    
    return err;
}

/* ============================================================================
 * Generic Log Hypothesis Generation (Format-Agnostic)
 * ========================================================================== */

tm_error_t tm_llm_generate_generic_hypotheses(tm_llm_client_t *client,
                                              const tm_generic_log_t *log,
                                              const tm_git_context_t *git_ctx,
                                              tm_hypothesis_t ***hypotheses,
                                              size_t *count)
{
    TM_CHECK_NULL(client, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(log, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(hypotheses, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(count, TM_ERR_INVALID_ARG);
    
    *hypotheses = NULL;
    *count = 0;
    
    /* Build generic analysis context */
    tm_generic_analysis_ctx_t ctx = {
        .log = log,
        .git_ctx = git_ctx,
        .additional_context = NULL,
        .max_entries = 50,                /* Limit entries to control token usage */
        .include_raw_lines = true,        /* Include raw lines for context */
        .errors_only = (log->total_errors > 0)  /* Focus on errors if present */
    };
    
    /* Build format-aware prompts */
    char *system_prompt = tm_build_generic_system_prompt(log->detected_format);
    char *user_prompt = tm_build_generic_log_prompt(&ctx);
    
    if (!system_prompt || !user_prompt) {
        TM_FREE(system_prompt);
        TM_FREE(user_prompt);
        return TM_ERR_NOMEM;
    }
    
    TM_DEBUG("Generic log prompt: %d estimated tokens", tm_estimate_tokens(user_prompt));
    
    /* Build chat request */
    tm_chat_message_t messages[2] = {
        { .role = TM_ROLE_SYSTEM, .content = system_prompt },
        { .role = TM_ROLE_USER, .content = user_prompt }
    };
    
    tm_chat_request_t request = {
        .messages = messages,
        .message_count = 2,
        .max_tokens = DEFAULT_MAX_TOKENS,
        .temperature = client->temperature
    };
    
    /* Send request with retry */
    tm_retry_config_t retry_cfg = tm_default_retry_config();
    tm_chat_response_t *response = NULL;
    
    tm_error_t err = tm_llm_chat_with_retry(client, &request, &retry_cfg, &response);
    
    TM_FREE(system_prompt);
    TM_FREE(user_prompt);
    
    if (err != TM_OK) {
        TM_ERROR("LLM request failed for generic log: %s", tm_strerror(err));
        return err;
    }
    
    if (!response || !response->content) {
        tm_chat_response_free(response);
        return TM_ERR_LLM;
    }
    
    TM_INFO("Generic log analysis: %d tokens", response->completion_tokens);
    
    /* Parse hypotheses from response */
    err = tm_parse_hypotheses(response->content, hypotheses, count);
    
    tm_chat_response_free(response);
    
    return err;
}

/* ============================================================================
 * Token Estimation
 * ========================================================================== */

int tm_estimate_tokens(const char *text)
{
    if (!text) return 0;
    
    /* Rough estimate: ~4 characters per token for English text/code */
    size_t len = strlen(text);
    return (int)((len + 3) / 4);
}

char *tm_truncate_to_tokens(const char *text, int max_tokens)
{
    if (!text || max_tokens <= 0) return NULL;
    
    /* Approximate character limit */
    size_t max_chars = (size_t)max_tokens * 4;
    size_t len = strlen(text);
    
    if (len <= max_chars) {
        return tm_strdup(text);
    }
    
    /* Truncate with ellipsis */
    char *result = tm_malloc(max_chars + 10);
    strncpy(result, text, max_chars - 3);
    result[max_chars - 3] = '\0';
    strcat(result, "...");
    
    return result;
}

bool tm_validate_hypothesis_json(const char *json_str)
{
    if (!json_str) return false;
    
    json_error_t error;
    json_t *root = json_loads(json_str, 0, &error);
    if (!root) return false;
    
    bool valid = json_object_get(root, "hypotheses") != NULL;
    json_decref(root);
    
    return valid;
}

/* ============================================================================
 * Explain Error (freeform, no trace parsing)
 * ========================================================================== */

tm_error_t tm_llm_explain_error(tm_llm_client_t *client,
                                const char *error_msg,
                                tm_hypothesis_t ***hypotheses,
                                size_t *count)
{
    TM_CHECK_NULL(client, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(error_msg, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(hypotheses, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(count, TM_ERR_INVALID_ARG);
    
    *hypotheses = NULL;
    *count = 0;
    
    char *system_prompt = tm_build_system_prompt();
    if (!system_prompt) return TM_ERR_NOMEM;
    
    /* Build a concise user prompt for freeform error text */
    size_t msg_len = strlen(error_msg);
    size_t prompt_sz = msg_len + 512;
    char *user_prompt = tm_malloc(prompt_sz);
    if (!user_prompt) {
        TM_FREE(system_prompt);
        return TM_ERR_NOMEM;
    }
    
    snprintf(user_prompt, prompt_sz,
        "Analyze the following error message or log snippet and generate "
        "root cause hypotheses with fix suggestions and debug commands.\n\n"
        "Error:\n```\n%s\n```\n\n"
        "Respond with JSON matching the schema in your system prompt.",
        error_msg);
    
    tm_chat_message_t messages[2] = {
        { .role = TM_ROLE_SYSTEM, .content = system_prompt },
        { .role = TM_ROLE_USER,   .content = user_prompt }
    };
    
    tm_chat_request_t request = {
        .messages = messages,
        .message_count = 2,
        .max_tokens = DEFAULT_MAX_TOKENS,
        .temperature = client->temperature
    };
    
    tm_retry_config_t retry_cfg = tm_default_retry_config();
    tm_chat_response_t *response = NULL;
    
    tm_error_t err = tm_llm_chat_with_retry(client, &request, &retry_cfg, &response);
    
    TM_FREE(system_prompt);
    TM_FREE(user_prompt);
    
    if (err != TM_OK) {
        TM_ERROR("LLM explain request failed: %s", tm_strerror(err));
        return err;
    }
    
    if (!response || !response->content) {
        tm_chat_response_free(response);
        return TM_ERR_LLM;
    }
    
    err = tm_parse_hypotheses(response->content, hypotheses, count);
    tm_chat_response_free(response);
    
    return err;
}

/* ============================================================================
 * Follow-Up Question (interactive mode)
 * ========================================================================== */

tm_error_t tm_llm_followup(tm_llm_client_t *client,
                           const tm_analysis_result_t *result,
                           const char *question,
                           char **response_out)
{
    TM_CHECK_NULL(client, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(question, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(response_out, TM_ERR_INVALID_ARG);
    
    *response_out = NULL;
    
    /* Build context summary from prior analysis */
    tm_strbuf_t ctx;
    tm_strbuf_init(&ctx);
    
    tm_strbuf_append(&ctx, "You previously analyzed an error and produced these hypotheses:\n\n");
    
    if (result && result->hypotheses) {
        for (size_t i = 0; i < result->hypothesis_count; i++) {
            const tm_hypothesis_t *h = result->hypotheses[i];
            if (h && h->title) {
                tm_strbuf_appendf(&ctx, "%zu. [%d%%] %s\n", i + 1,
                                  h->confidence, h->title);
                if (h->explanation)
                    tm_strbuf_appendf(&ctx, "   %s\n", h->explanation);
            }
        }
    }
    
    tm_strbuf_appendf(&ctx, "\nUser follow-up question:\n%s\n\n"
                             "Answer concisely. If the question asks for commands, "
                             "provide copy-pasteable shell commands. "
                             "If it asks about root cause, be specific about code paths.",
                             question);
    
    char *context_str = tm_strbuf_finish(&ctx);
    if (!context_str) return TM_ERR_NOMEM;
    
    char *system_prompt = tm_strdup(
        "You are a debugging assistant. You previously analyzed an error. "
        "Now answer a follow-up question. Be direct and actionable.");
    
    if (!system_prompt) {
        TM_FREE(context_str);
        return TM_ERR_NOMEM;
    }
    
    tm_chat_message_t messages[2] = {
        { .role = TM_ROLE_SYSTEM, .content = system_prompt },
        { .role = TM_ROLE_USER,   .content = context_str }
    };
    
    tm_chat_request_t request = {
        .messages = messages,
        .message_count = 2,
        .max_tokens = DEFAULT_MAX_TOKENS,
        .temperature = client->temperature
    };
    
    tm_retry_config_t retry_cfg = tm_default_retry_config();
    tm_chat_response_t *response = NULL;
    
    tm_error_t err = tm_llm_chat_with_retry(client, &request, &retry_cfg, &response);
    
    TM_FREE(system_prompt);
    TM_FREE(context_str);
    
    if (err != TM_OK) {
        TM_ERROR("LLM follow-up request failed: %s", tm_strerror(err));
        return err;
    }
    
    if (!response || !response->content) {
        tm_chat_response_free(response);
        return TM_ERR_LLM;
    }
    
    *response_out = tm_strdup(response->content);
    tm_chat_response_free(response);
    
    return *response_out ? TM_OK : TM_ERR_NOMEM;
}
