/**
 * TraceMind - Configuration Management
 */

#include "internal/common.h"
#include "tracemind.h"
#include <jansson.h>
#include <pwd.h>
#include <strings.h>
#include <sys/stat.h>

/* ============================================================================
 * Default Values
 * ========================================================================== */

#define DEFAULT_LLM_PROVIDER TM_LLM_OPENAI
#define DEFAULT_MODEL "gpt-4o"
#define DEFAULT_TIMEOUT_MS 60000
#define DEFAULT_TEMPERATURE 0.3f
#define DEFAULT_MAX_COMMITS 20
#define DEFAULT_MAX_CALL_DEPTH 5

/* ============================================================================
 * Configuration Creation
 * ========================================================================== */

tm_config_t *tm_config_new(void)
{
    tm_config_t *cfg = tm_calloc(1, sizeof(tm_config_t));
    
    /* LLM defaults */
    cfg->llm_provider = DEFAULT_LLM_PROVIDER;
    cfg->api_key = NULL;
    cfg->api_endpoint = NULL;
    cfg->model_name = tm_strdup(DEFAULT_MODEL);
    cfg->timeout_ms = DEFAULT_TIMEOUT_MS;
    cfg->temperature = DEFAULT_TEMPERATURE;
    
    /* Analysis defaults */
    cfg->max_commits = DEFAULT_MAX_COMMITS;
    cfg->max_call_depth = DEFAULT_MAX_CALL_DEPTH;
    cfg->include_stdlib = false;
    cfg->include_tests = false;
    
    /* Output defaults */
    cfg->output_format = TM_OUTPUT_CLI;
    cfg->color_output = true;
    cfg->verbose = false;
    
    /* Paths */
    cfg->repo_path = NULL;
    cfg->cache_dir = NULL;
    
    return cfg;
}

void tm_config_free(tm_config_t *cfg)
{
    if (!cfg) return;
    
    TM_FREE(cfg->api_key);
    TM_FREE(cfg->api_endpoint);
    TM_FREE(cfg->model_name);
    TM_FREE(cfg->repo_path);
    TM_FREE(cfg->cache_dir);
    free(cfg);
}

/* ============================================================================
 * Environment Variable Loading
 * ========================================================================== */

tm_error_t tm_config_load_env(tm_config_t *cfg)
{
    TM_CHECK_NULL(cfg, TM_ERR_INVALID_ARG);
    
    /* API Keys */
    const char *openai_key = getenv("OPENAI_API_KEY");
    const char *anthropic_key = getenv("ANTHROPIC_API_KEY");
    
    if (openai_key && strlen(openai_key) > 0) {
        TM_FREE(cfg->api_key);
        cfg->api_key = tm_strdup(openai_key);
        cfg->llm_provider = TM_LLM_OPENAI;
        TM_DEBUG("Using OpenAI API key from environment");
    } else if (anthropic_key && strlen(anthropic_key) > 0) {
        TM_FREE(cfg->api_key);
        cfg->api_key = tm_strdup(anthropic_key);
        cfg->llm_provider = TM_LLM_ANTHROPIC;
        TM_DEBUG("Using Anthropic API key from environment");
    }
    
    /* Model override */
    const char *model = getenv("TRACEMIND_MODEL");
    if (model && strlen(model) > 0) {
        TM_FREE(cfg->model_name);
        cfg->model_name = tm_strdup(model);
        TM_DEBUG("Using model from environment: %s", model);
    }
    
    /* Endpoint override */
    const char *endpoint = getenv("TRACEMIND_ENDPOINT");
    if (endpoint && strlen(endpoint) > 0) {
        TM_FREE(cfg->api_endpoint);
        cfg->api_endpoint = tm_strdup(endpoint);
        TM_DEBUG("Using endpoint from environment: %s", endpoint);
    }
    
    /* Provider override */
    const char *provider = getenv("TRACEMIND_PROVIDER");
    if (provider) {
        if (strcasecmp(provider, "openai") == 0) {
            cfg->llm_provider = TM_LLM_OPENAI;
        } else if (strcasecmp(provider, "anthropic") == 0) {
            cfg->llm_provider = TM_LLM_ANTHROPIC;
        } else if (strcasecmp(provider, "local") == 0) {
            cfg->llm_provider = TM_LLM_LOCAL;
        }
    }
    
    /* Timeout override */
    const char *timeout = getenv("TRACEMIND_TIMEOUT");
    if (timeout) {
        int val = atoi(timeout);
        if (val > 0) {
            cfg->timeout_ms = val;
        }
    }
    
    /* Verbosity */
    const char *debug = getenv("TRACEMIND_DEBUG");
    if (debug && (strcmp(debug, "1") == 0 || strcasecmp(debug, "true") == 0)) {
        cfg->verbose = true;
        g_log_level = TM_LOG_DEBUG;
    }
    
    return TM_OK;
}

/* ============================================================================
 * File-Based Configuration Loading
 * ========================================================================== */

/**
 * Get the default config file path (~/.config/tracemind/config.json)
 */
static char *get_default_config_path(void)
{
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) {
            home = pw->pw_dir;
        }
    }
    
    if (!home) return NULL;
    
    char *path = tm_malloc(strlen(home) + 64);
    snprintf(path, strlen(home) + 64, "%s/.config/tracemind/config.json", home);
    
    return path;
}

tm_error_t tm_config_load(tm_config_t *cfg, const char *path)
{
    TM_CHECK_NULL(cfg, TM_ERR_INVALID_ARG);
    
    /* Use default path if not specified */
    char *config_path = NULL;
    if (path) {
        config_path = tm_strdup(path);
    } else {
        config_path = get_default_config_path();
    }
    
    if (!config_path) {
        TM_DEBUG("No config path available");
        return TM_OK;  /* Not an error, just no config file */
    }
    
    /* Check if file exists */
    struct stat st;
    if (stat(config_path, &st) != 0) {
        TM_DEBUG("Config file not found: %s", config_path);
        TM_FREE(config_path);
        return TM_OK;  /* Not an error */
    }
    
    /* Read file */
    size_t size = 0;
    char *content = tm_read_file(config_path, &size);
    if (!content) {
        TM_WARN("Could not read config file: %s", config_path);
        TM_FREE(config_path);
        return TM_ERR_IO;
    }
    
    TM_DEBUG("Loading config from: %s", config_path);
    TM_FREE(config_path);
    
    /* Parse JSON */
    json_error_t error;
    json_t *root = json_loads(content, 0, &error);
    TM_FREE(content);
    
    if (!root) {
        TM_ERROR("Config parse error: %s", error.text);
        return TM_ERR_PARSE;
    }
    
    /* Extract values */
    json_t *val;
    
    /* LLM settings */
    val = json_object_get(root, "provider");
    if (val && json_is_string(val)) {
        const char *p = json_string_value(val);
        if (strcasecmp(p, "openai") == 0) cfg->llm_provider = TM_LLM_OPENAI;
        else if (strcasecmp(p, "anthropic") == 0) cfg->llm_provider = TM_LLM_ANTHROPIC;
        else if (strcasecmp(p, "local") == 0) cfg->llm_provider = TM_LLM_LOCAL;
    }
    
    val = json_object_get(root, "api_key");
    if (val && json_is_string(val)) {
        TM_FREE(cfg->api_key);
        cfg->api_key = tm_strdup(json_string_value(val));
    }
    
    val = json_object_get(root, "model");
    if (val && json_is_string(val)) {
        TM_FREE(cfg->model_name);
        cfg->model_name = tm_strdup(json_string_value(val));
    }
    
    val = json_object_get(root, "endpoint");
    if (val && json_is_string(val)) {
        TM_FREE(cfg->api_endpoint);
        cfg->api_endpoint = tm_strdup(json_string_value(val));
    }
    
    val = json_object_get(root, "timeout_ms");
    if (val && json_is_integer(val)) {
        cfg->timeout_ms = (int)json_integer_value(val);
    }
    
    val = json_object_get(root, "temperature");
    if (val && json_is_real(val)) {
        cfg->temperature = (float)json_real_value(val);
    }
    
    /* Analysis settings */
    val = json_object_get(root, "max_commits");
    if (val && json_is_integer(val)) {
        cfg->max_commits = (int)json_integer_value(val);
    }
    
    val = json_object_get(root, "max_call_depth");
    if (val && json_is_integer(val)) {
        cfg->max_call_depth = (int)json_integer_value(val);
    }
    
    val = json_object_get(root, "include_stdlib");
    if (val && json_is_boolean(val)) {
        cfg->include_stdlib = json_boolean_value(val);
    }
    
    val = json_object_get(root, "include_tests");
    if (val && json_is_boolean(val)) {
        cfg->include_tests = json_boolean_value(val);
    }
    
    /* Output settings */
    val = json_object_get(root, "output_format");
    if (val && json_is_string(val)) {
        const char *f = json_string_value(val);
        if (strcasecmp(f, "markdown") == 0) cfg->output_format = TM_OUTPUT_MARKDOWN;
        else if (strcasecmp(f, "json") == 0) cfg->output_format = TM_OUTPUT_JSON;
        else cfg->output_format = TM_OUTPUT_CLI;
    }
    
    val = json_object_get(root, "color");
    if (val && json_is_boolean(val)) {
        cfg->color_output = json_boolean_value(val);
    }
    
    val = json_object_get(root, "verbose");
    if (val && json_is_boolean(val)) {
        cfg->verbose = json_boolean_value(val);
        if (cfg->verbose) g_log_level = TM_LOG_DEBUG;
    }
    
    /* Paths */
    val = json_object_get(root, "cache_dir");
    if (val && json_is_string(val)) {
        TM_FREE(cfg->cache_dir);
        cfg->cache_dir = tm_strdup(json_string_value(val));
    }
    
    json_decref(root);
    
    TM_INFO("Configuration loaded successfully");
    return TM_OK;
}
