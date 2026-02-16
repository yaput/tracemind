/**
 * TraceMind - LLM Hypothesis Engine
 * 
 * HTTP client for LLM providers (OpenAI, Anthropic, Local).
 */

#ifndef TM_INTERNAL_LLM_H
#define TM_INTERNAL_LLM_H

#include "tracemind.h"
#include <curl/curl.h>

/* ============================================================================
 * HTTP Client Initialization
 * ========================================================================== */

/**
 * Initialize CURL globally.
 * Call once at startup.
 */
tm_error_t tm_http_init(void);

/**
 * Cleanup CURL.
 */
void tm_http_cleanup(void);

/* ============================================================================
 * LLM Client
 * ========================================================================== */

/**
 * LLM client instance.
 */
typedef struct {
    tm_llm_provider_t provider;
    char *api_key;
    char *endpoint;
    char *model;
    int timeout_ms;
    float temperature;
    CURL *curl;
} tm_llm_client_t;

/**
 * Create a new LLM client.
 */
tm_llm_client_t *tm_llm_client_new(const tm_config_t *cfg);

/**
 * Free LLM client.
 */
void tm_llm_client_free(tm_llm_client_t *client);

/* ============================================================================
 * Prompt Building
 * ========================================================================== */

/**
 * Analysis context for prompt building.
 */
typedef struct {
    const tm_stack_trace_t *trace;
    const tm_call_graph_t *call_graph;
    const tm_git_context_t *git_ctx;
    const char *additional_context;   /* Optional user-provided context */
} tm_analysis_context_t;

/**
 * Build the analysis prompt from context.
 * Returns allocated string (caller must free).
 */
char *tm_build_analysis_prompt(const tm_analysis_context_t *ctx);

/**
 * Build a system prompt for root cause analysis.
 * Returns allocated string (caller must free).
 */
char *tm_build_system_prompt(void);

/* ============================================================================
 * LLM API Calls
 * ========================================================================== */

/**
 * Message role for chat completions.
 */
typedef enum {
    TM_ROLE_SYSTEM = 0,
    TM_ROLE_USER,
    TM_ROLE_ASSISTANT
} tm_message_role_t;

/**
 * Chat message.
 */
typedef struct {
    tm_message_role_t role;
    char *content;
} tm_chat_message_t;

/**
 * Chat completion request.
 */
typedef struct {
    tm_chat_message_t *messages;
    size_t message_count;
    int max_tokens;
    float temperature;
} tm_chat_request_t;

/**
 * Chat completion response.
 */
typedef struct {
    char *content;                    /* Response content (owned) */
    int prompt_tokens;                /* Tokens in prompt */
    int completion_tokens;            /* Tokens in completion */
    char *model;                      /* Model used (owned) */
    char *finish_reason;              /* stop, length, etc. (owned) */
} tm_chat_response_t;

/**
 * Send chat completion request.
 */
tm_error_t tm_llm_chat(tm_llm_client_t *client,
                       const tm_chat_request_t *request,
                       tm_chat_response_t **response);

/**
 * Free chat response.
 */
void tm_chat_response_free(tm_chat_response_t *response);

/* ============================================================================
 * Response Parsing
 * ========================================================================== */

/**
 * Parse LLM response into structured hypotheses.
 */
tm_error_t tm_parse_hypotheses(const char *response_text,
                               tm_hypothesis_t ***hypotheses,
                               size_t *count);

/**
 * Generate hypotheses from analysis context.
 * Main entry point for LLM-based root cause analysis.
 */
tm_error_t tm_llm_generate_hypotheses(tm_llm_client_t *client,
                                      const tm_stack_trace_t *trace,
                                      const tm_call_graph_t *call_graph,
                                      const tm_git_context_t *git_ctx,
                                      tm_hypothesis_t ***hypotheses,
                                      size_t *count);

/**
 * Free a single hypothesis.
 */
void tm_hypothesis_free(tm_hypothesis_t *h);

/**
 * Free hypotheses array.
 */
void tm_hypotheses_free(tm_hypothesis_t **hypotheses, size_t count);

/* ============================================================================
 * Provider-Specific Implementations
 * ========================================================================== */

/**
 * Build request body for OpenAI API.
 */
char *tm_openai_build_request(const tm_chat_request_t *request,
                              const char *model);

/**
 * Parse OpenAI API response.
 */
tm_error_t tm_openai_parse_response(const char *json,
                                    tm_chat_response_t **response);

/**
 * Build request body for Anthropic API.
 */
char *tm_anthropic_build_request(const tm_chat_request_t *request,
                                 const char *model);

/**
 * Parse Anthropic API response.
 */
tm_error_t tm_anthropic_parse_response(const char *json,
                                       tm_chat_response_t **response);

/* ============================================================================
 * Structured Output
 * ========================================================================== */

/**
 * Expected JSON schema for hypothesis response.
 * Used for structured output modes.
 */
extern const char *TM_HYPOTHESIS_SCHEMA;

/**
 * Validate hypothesis JSON against schema.
 */
bool tm_validate_hypothesis_json(const char *json);

/* ============================================================================
 * Retry & Rate Limiting
 * ========================================================================== */

/**
 * Retry configuration.
 */
typedef struct {
    int max_retries;
    int initial_delay_ms;
    int max_delay_ms;
    float backoff_multiplier;
} tm_retry_config_t;

/**
 * Get default retry configuration.
 */
tm_retry_config_t tm_default_retry_config(void);

/**
 * Execute request with retry logic.
 */
tm_error_t tm_llm_chat_with_retry(tm_llm_client_t *client,
                                  const tm_chat_request_t *request,
                                  const tm_retry_config_t *retry_cfg,
                                  tm_chat_response_t **response);

/* ============================================================================
 * Token Estimation
 * ========================================================================== */

/**
 * Estimate token count for a string.
 * Uses simple heuristic (not exact).
 */
int tm_estimate_tokens(const char *text);

/**
 * Truncate text to fit within token limit.
 * Returns allocated string (caller must free).
 */
char *tm_truncate_to_tokens(const char *text, int max_tokens);

#endif /* TM_INTERNAL_LLM_H */
