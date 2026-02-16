/**
 * TraceMind CLI - AI-Powered Root Cause Analysis
 * 
 * Usage:
 *   tracemind analyze <trace_file>
 *   tracemind analyze -              # Read from stdin
 *   cat trace.txt | tracemind analyze
 * 
 * Options:
 *   -p, --provider <name>    LLM provider (openai, anthropic, local)
 *   -m, --model <name>       Model name (e.g., gpt-4o, claude-sonnet-4-20250514)
 *   -o, --output <format>    Output format (cli, markdown, json)
 *   -r, --repo <path>        Repository path
 *   --no-color               Disable colored output
 *   -v, --verbose            Enable verbose output
 *   -h, --help               Show help
 *   --version                Show version
 */

#include "tracemind.h"
#include "internal/common.h"
#include "internal/output.h"
#include <getopt.h>
#include <signal.h>
#include <strings.h>
#include <sys/ioctl.h>

/* ============================================================================
 * Version and Help
 * ========================================================================== */

#define TRACEMIND_VERSION "0.1.0"

static const char *HELP_TEXT =
"TraceMind - AI-Powered Root Cause Analysis\n"
"\n"
"USAGE:\n"
"    tracemind analyze <file>         Analyze stack trace from file\n"
"    tracemind analyze -              Read stack trace from stdin\n"
"    cat trace.txt | tracemind        Pipe stack trace to analyzer\n"
"\n"
"COMMANDS:\n"
"    analyze     Analyze a stack trace and generate hypotheses\n"
"    config      Show current configuration\n"
"    version     Show version information\n"
"\n"
"OPTIONS:\n"
"    -p, --provider <name>    LLM provider: openai, anthropic, local\n"
"    -m, --model <name>       Model name (e.g., gpt-4o, claude-sonnet-4-20250514)\n"
"    -k, --api-key <key>      API key (or use OPENAI_API_KEY env var)\n"
"    -o, --output <format>    Output: cli (default), markdown, json\n"
"    -r, --repo <path>        Repository path (auto-detected if omitted)\n"
"    -c, --config <file>      Config file path\n"
"    --no-color               Disable colored output\n"
"    -v, --verbose            Enable verbose/debug output\n"
"    -h, --help               Show this help message\n"
"    --version                Show version\n"
"\n"
"ENVIRONMENT VARIABLES:\n"
"    OPENAI_API_KEY           OpenAI API key\n"
"    ANTHROPIC_API_KEY        Anthropic API key  \n"
"    TRACEMIND_MODEL          Default model name\n"
"    TRACEMIND_PROVIDER       Default provider\n"
"    TRACEMIND_DEBUG          Enable debug output (1 or true)\n"
"\n"
"EXAMPLES:\n"
"    tracemind analyze crash.log\n"
"    python app.py 2>&1 | tracemind analyze -\n"
"    tracemind analyze trace.txt -o markdown > report.md\n"
"    tracemind analyze trace.txt --provider anthropic -m claude-sonnet-4-20250514\n"
"\n"
"For more information: https://github.com/tracemind/tracemind\n";

static void print_version(void)
{
    printf("TraceMind %s\n", TRACEMIND_VERSION);
    printf("AI-Powered Root Cause Analysis\n");
    printf("Built with: Tree-sitter, libgit2, libcurl\n");
}

static void print_help(void)
{
    printf("%s", HELP_TEXT);
}

/* ============================================================================
 * CLI Options
 * ========================================================================== */

static struct option long_options[] = {
    {"provider",  required_argument, 0, 'p'},
    {"model",     required_argument, 0, 'm'},
    {"api-key",   required_argument, 0, 'k'},
    {"output",    required_argument, 0, 'o'},
    {"repo",      required_argument, 0, 'r'},
    {"config",    required_argument, 0, 'c'},
    {"no-color",  no_argument,       0, 'n'},
    {"verbose",   no_argument,       0, 'v'},
    {"help",      no_argument,       0, 'h'},
    {"version",   no_argument,       0, 'V'},
    {0, 0, 0, 0}
};

typedef struct {
    const char *command;
    const char *input_file;
    const char *provider;
    const char *model;
    const char *api_key;
    const char *output_format;
    const char *repo_path;
    const char *config_path;
    bool no_color;
    bool verbose;
    bool help;
    bool version;
} cli_args_t;

static cli_args_t parse_args(int argc, char **argv)
{
    cli_args_t args = {0};
    
    int opt;
    int option_index = 0;
    
    while ((opt = getopt_long(argc, argv, "p:m:k:o:r:c:nvhV", 
                              long_options, &option_index)) != -1) {
        switch (opt) {
            case 'p': args.provider = optarg; break;
            case 'm': args.model = optarg; break;
            case 'k': args.api_key = optarg; break;
            case 'o': args.output_format = optarg; break;
            case 'r': args.repo_path = optarg; break;
            case 'c': args.config_path = optarg; break;
            case 'n': args.no_color = true; break;
            case 'v': args.verbose = true; break;
            case 'h': args.help = true; break;
            case 'V': args.version = true; break;
            default:
                break;
        }
    }
    
    /* Get command and input file */
    if (optind < argc) {
        args.command = argv[optind++];
    }
    
    if (optind < argc) {
        args.input_file = argv[optind++];
    }
    
    return args;
}

/* ============================================================================
 * Progress Display
 * ========================================================================== */

static bool g_tty_output = false;
static int g_spinner_frame = 0;
static const char *SPINNER_FRAMES[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
#define SPINNER_FRAME_COUNT 10

static void progress_callback(const char *stage, float progress, void *ctx)
{
    (void)ctx;
    
    if (!g_tty_output) {
        /* Non-interactive: just print stage changes */
        static const char *last_stage = NULL;
        if (stage != last_stage) {
            fprintf(stderr, "• %s\n", stage);
            last_stage = stage;
        }
        return;
    }
    
    /* Interactive: show spinner + progress bar */
    const char *spinner = SPINNER_FRAMES[g_spinner_frame % SPINNER_FRAME_COUNT];
    g_spinner_frame++;
    
    int bar_width = 30;
    int filled = (int)(progress * bar_width);
    
    fprintf(stderr, "\r%s %s [", spinner, stage);
    for (int i = 0; i < bar_width; i++) {
        if (i < filled) fprintf(stderr, "█");
        else fprintf(stderr, "░");
    }
    fprintf(stderr, "] %3d%%", (int)(progress * 100));
    
    if (progress >= 1.0f) {
        fprintf(stderr, "\n");
    }
    
    fflush(stderr);
}

/* ============================================================================
 * Signal Handling
 * ========================================================================== */

static volatile sig_atomic_t g_interrupted = 0;

static void signal_handler(int sig)
{
    (void)sig;
    g_interrupted = 1;
    fprintf(stderr, "\nInterrupted.\n");
}

static void setup_signals(void)
{
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

/* ============================================================================
 * Commands
 * ========================================================================== */

static int cmd_analyze(cli_args_t *args)
{
    /* Create and configure */
    tm_config_t *config = tm_config_new();
    
    /* Load config file */
    tm_config_load(config, args->config_path);
    
    /* Load environment */
    tm_config_load_env(config);
    
    /* Apply CLI overrides */
    if (args->provider) {
        if (strcasecmp(args->provider, "openai") == 0) {
            config->llm_provider = TM_LLM_OPENAI;
        } else if (strcasecmp(args->provider, "anthropic") == 0) {
            config->llm_provider = TM_LLM_ANTHROPIC;
        } else if (strcasecmp(args->provider, "local") == 0) {
            config->llm_provider = TM_LLM_LOCAL;
        } else {
            fprintf(stderr, "Unknown provider: %s\n", args->provider);
            tm_config_free(config);
            return 1;
        }
    }
    
    if (args->model) {
        TM_FREE(config->model_name);
        config->model_name = tm_strdup(args->model);
    }
    
    if (args->api_key) {
        TM_FREE(config->api_key);
        config->api_key = tm_strdup(args->api_key);
    }
    
    if (args->output_format) {
        if (strcasecmp(args->output_format, "cli") == 0) {
            config->output_format = TM_OUTPUT_CLI;
        } else if (strcasecmp(args->output_format, "markdown") == 0) {
            config->output_format = TM_OUTPUT_MARKDOWN;
        } else if (strcasecmp(args->output_format, "json") == 0) {
            config->output_format = TM_OUTPUT_JSON;
        } else {
            fprintf(stderr, "Unknown output format: %s\n", args->output_format);
            tm_config_free(config);
            return 1;
        }
    }
    
    if (args->repo_path) {
        TM_FREE(config->repo_path);
        config->repo_path = tm_strdup(args->repo_path);
    }
    
    if (args->no_color) {
        config->color_output = false;
    }
    
    if (args->verbose) {
        config->verbose = true;
        g_log_level = TM_LOG_DEBUG;
    }
    
    /* Check for API key */
    if (!config->api_key || strlen(config->api_key) == 0) {
        fprintf(stderr, "Error: No API key configured.\n");
        fprintf(stderr, "Set OPENAI_API_KEY or ANTHROPIC_API_KEY environment variable,\n");
        fprintf(stderr, "or use --api-key option.\n");
        tm_config_free(config);
        return 1;
    }
    
    /* Create analyzer */
    tm_analyzer_t *analyzer = tm_analyzer_new(config);
    if (!analyzer) {
        fprintf(stderr, "Error: Failed to initialize analyzer.\n");
        tm_config_free(config);
        return 1;
    }
    
    /* Set up progress reporting */
    g_tty_output = isatty(STDERR_FILENO);
    tm_analyzer_set_progress_callback(analyzer, progress_callback, NULL);
    
    /* Determine input */
    const char *input = args->input_file;
    if (!input && !isatty(STDIN_FILENO)) {
        input = "-";  /* Read from stdin pipe */
    }
    
    if (!input) {
        fprintf(stderr, "Error: No input specified.\n");
        fprintf(stderr, "Usage: tracemind analyze <file>\n");
        tm_analyzer_free(analyzer);
        tm_config_free(config);
        return 1;
    }
    
    /* Run analysis */
    if (g_tty_output) {
        fprintf(stderr, "\n");
    }
    
    tm_analysis_result_t *result = tm_analyze(analyzer, input);
    
    if (g_tty_output) {
        fprintf(stderr, "\n");
    }
    
    if (!result) {
        fprintf(stderr, "Error: Analysis failed.\n");
        tm_analyzer_free(analyzer);
        tm_config_free(config);
        return 1;
    }
    
    /* Output results */
    tm_print_result(analyzer, result);
    
    /* Exit status based on result */
    int exit_code = 0;
    if (result->hypothesis_count == 0) {
        exit_code = 2;  /* No hypotheses generated */
    }
    if (result->error_message) {
        exit_code = 1;  /* Error during analysis */
    }
    
    /* Cleanup */
    tm_result_free(result);
    tm_analyzer_free(analyzer);
    tm_config_free(config);
    
    return exit_code;
}

static int cmd_config(cli_args_t *args)
{
    tm_config_t *config = tm_config_new();
    tm_config_load(config, args->config_path);
    tm_config_load_env(config);
    
    printf("TraceMind Configuration\n");
    printf("=======================\n\n");
    
    printf("LLM Settings:\n");
    printf("  Provider:    %s\n", 
           config->llm_provider == TM_LLM_OPENAI ? "OpenAI" :
           config->llm_provider == TM_LLM_ANTHROPIC ? "Anthropic" : "Local");
    printf("  Model:       %s\n", config->model_name ? config->model_name : "(default)");
    printf("  API Key:     %s\n", config->api_key ? "***configured***" : "(not set)");
    printf("  Endpoint:    %s\n", config->api_endpoint ? config->api_endpoint : "(default)");
    printf("  Timeout:     %d ms\n", config->timeout_ms);
    printf("  Temperature: %.2f\n", config->temperature);
    printf("\n");
    
    printf("Analysis Settings:\n");
    printf("  Max Commits:     %d\n", config->max_commits);
    printf("  Max Call Depth:  %d\n", config->max_call_depth);
    printf("  Include Stdlib:  %s\n", config->include_stdlib ? "yes" : "no");
    printf("  Include Tests:   %s\n", config->include_tests ? "yes" : "no");
    printf("\n");
    
    printf("Output Settings:\n");
    printf("  Format:  %s\n",
           config->output_format == TM_OUTPUT_CLI ? "CLI" :
           config->output_format == TM_OUTPUT_MARKDOWN ? "Markdown" : "JSON");
    printf("  Color:   %s\n", config->color_output ? "enabled" : "disabled");
    printf("  Verbose: %s\n", config->verbose ? "enabled" : "disabled");
    printf("\n");
    
    tm_config_free(config);
    return 0;
}

/* ============================================================================
 * Main Entry Point
 * ========================================================================== */

int main(int argc, char **argv)
{
    setup_signals();
    
    /* Parse arguments */
    cli_args_t args = parse_args(argc, argv);
    
    /* Handle global options */
    if (args.help) {
        print_help();
        return 0;
    }
    
    if (args.version) {
        print_version();
        return 0;
    }
    
    /* Handle commands */
    if (!args.command) {
        /* No command - check if data on stdin */
        if (!isatty(STDIN_FILENO)) {
            args.command = "analyze";
            args.input_file = "-";
        } else {
            print_help();
            return 1;
        }
    }
    
    if (strcmp(args.command, "analyze") == 0) {
        return cmd_analyze(&args);
    }
    
    if (strcmp(args.command, "config") == 0) {
        return cmd_config(&args);
    }
    
    if (strcmp(args.command, "version") == 0) {
        print_version();
        return 0;
    }
    
    if (strcmp(args.command, "help") == 0) {
        print_help();
        return 0;
    }
    
    fprintf(stderr, "Unknown command: %s\n", args.command);
    fprintf(stderr, "Run 'tracemind help' for usage.\n");
    return 1;
}
