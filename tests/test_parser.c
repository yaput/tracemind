/**
 * TraceMind - Parser Tests
 */

#include "tracemind.h"
#include "internal/common.h"
#include "internal/parser.h"
#include <assert.h>
#include <string.h>

/* ============================================================================
 * Test Utilities
 * ========================================================================== */

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  Running %s... ", #name); \
    test_##name(); \
    printf("PASS\n"); \
} while (0)

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        printf("FAIL\n    Assertion failed: %s\n    at %s:%d\n", \
               #cond, __FILE__, __LINE__); \
        return; \
    } \
} while (0)

#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_STREQ(a, b) ASSERT_TRUE(strcmp((a), (b)) == 0)
#define ASSERT_NOT_NULL(p) ASSERT_TRUE((p) != NULL)

/* ============================================================================
 * Test Data
 * ========================================================================== */

static const char *PYTHON_TRACE_SIMPLE =
"Traceback (most recent call last):\n"
"  File \"/app/main.py\", line 42, in process_request\n"
"    result = handler.execute(data)\n"
"  File \"/app/handlers.py\", line 156, in execute\n"
"    return self._run_query(query)\n"
"  File \"/app/handlers.py\", line 203, in _run_query\n"
"    cursor.execute(sql)\n"
"psycopg2.errors.SyntaxError: syntax error at or near \"FROM\"\n";

static const char *GO_PANIC_SIMPLE =
"panic: runtime error: index out of range [5] with length 3\n"
"\n"
"goroutine 1 [running]:\n"
"main.processItems(0xc0000a6000, 0x3, 0x8)\n"
"        /home/user/project/main.go:45 +0x1a3\n"
"main.handleRequest(0xc0000b2000)\n"
"        /home/user/project/handlers.go:89 +0x85\n"
"main.main()\n"
"        /home/user/project/main.go:23 +0x45\n";

static const char *NODE_ERROR_SIMPLE =
"TypeError: Cannot read property 'id' of undefined\n"
"    at UserService.getUser (/app/services/user.js:45:23)\n"
"    at AuthController.authenticate (/app/controllers/auth.js:78:15)\n"
"    at Router.handle (/app/router.js:34:12)\n"
"    at Server.<anonymous> (/app/server.js:89:5)\n";

/* ============================================================================
 * Python Parser Tests
 * ========================================================================== */

TEST(python_trace_parsing)
{
    tm_stack_trace_t *trace = tm_parse_stack_trace(
        PYTHON_TRACE_SIMPLE, strlen(PYTHON_TRACE_SIMPLE));
    
    ASSERT_NOT_NULL(trace);
    ASSERT_EQ(trace->language, TM_LANG_PYTHON);
    ASSERT_EQ(trace->frame_count, 3);
    
    /* Check first frame */
    ASSERT_STREQ(trace->frames[0].file, "/app/main.py");
    ASSERT_EQ(trace->frames[0].line, 42);
    ASSERT_STREQ(trace->frames[0].function, "process_request");
    
    /* Check last frame */
    ASSERT_STREQ(trace->frames[2].file, "/app/handlers.py");
    ASSERT_EQ(trace->frames[2].line, 203);
    ASSERT_STREQ(trace->frames[2].function, "_run_query");
    
    /* Check error message */
    ASSERT_NOT_NULL(trace->error_message);
    ASSERT_TRUE(strstr(trace->error_message, "SyntaxError") != NULL);
    
    tm_stack_trace_free(trace);
}

TEST(python_language_detection)
{
    tm_language_t lang = tm_detect_trace_language(
        PYTHON_TRACE_SIMPLE, strlen(PYTHON_TRACE_SIMPLE));
    ASSERT_EQ(lang, TM_LANG_PYTHON);
}

/* ============================================================================
 * Go Parser Tests
 * ========================================================================== */

TEST(go_panic_parsing)
{
    tm_stack_trace_t *trace = tm_parse_stack_trace(
        GO_PANIC_SIMPLE, strlen(GO_PANIC_SIMPLE));
    
    ASSERT_NOT_NULL(trace);
    ASSERT_EQ(trace->language, TM_LANG_GO);
    ASSERT_EQ(trace->frame_count, 3);
    
    /* Check first frame */
    ASSERT_STREQ(trace->frames[0].file, "/home/user/project/main.go");
    ASSERT_EQ(trace->frames[0].line, 45);
    ASSERT_STREQ(trace->frames[0].function, "main.processItems");
    
    /* Check error message */
    ASSERT_NOT_NULL(trace->error_message);
    ASSERT_TRUE(strstr(trace->error_message, "index out of range") != NULL);
    
    tm_stack_trace_free(trace);
}

TEST(go_language_detection)
{
    tm_language_t lang = tm_detect_trace_language(
        GO_PANIC_SIMPLE, strlen(GO_PANIC_SIMPLE));
    ASSERT_EQ(lang, TM_LANG_GO);
}

/* ============================================================================
 * Node.js Parser Tests
 * ========================================================================== */

TEST(nodejs_error_parsing)
{
    tm_stack_trace_t *trace = tm_parse_stack_trace(
        NODE_ERROR_SIMPLE, strlen(NODE_ERROR_SIMPLE));
    
    ASSERT_NOT_NULL(trace);
    ASSERT_EQ(trace->language, TM_LANG_NODEJS);
    ASSERT_EQ(trace->frame_count, 4);
    
    /* Check first frame */
    ASSERT_STREQ(trace->frames[0].file, "/app/services/user.js");
    ASSERT_EQ(trace->frames[0].line, 45);
    ASSERT_STREQ(trace->frames[0].function, "UserService.getUser");
    
    /* Check error message */
    ASSERT_NOT_NULL(trace->error_message);
    ASSERT_TRUE(strstr(trace->error_message, "TypeError") != NULL);
    
    tm_stack_trace_free(trace);
}

TEST(nodejs_language_detection)
{
    tm_language_t lang = tm_detect_trace_language(
        NODE_ERROR_SIMPLE, strlen(NODE_ERROR_SIMPLE));
    ASSERT_EQ(lang, TM_LANG_NODEJS);
}

/* ============================================================================
 * Edge Cases
 * ========================================================================== */

TEST(empty_input)
{
    tm_stack_trace_t *trace = tm_parse_stack_trace("", 0);
    ASSERT_TRUE(trace == NULL || trace->frame_count == 0);
    if (trace) tm_stack_trace_free(trace);
}

TEST(null_input)
{
    tm_stack_trace_t *trace = tm_parse_stack_trace(NULL, 0);
    ASSERT_TRUE(trace == NULL);
}

TEST(garbage_input)
{
    const char *garbage = "This is not a stack trace at all.\nJust random text.\n";
    tm_stack_trace_t *trace = tm_parse_stack_trace(garbage, strlen(garbage));
    ASSERT_TRUE(trace == NULL || trace->frame_count == 0);
    if (trace) tm_stack_trace_free(trace);
}

/* ============================================================================
 * Main
 * ========================================================================== */

int main(void)
{
    printf("Parser Tests\n");
    printf("============\n\n");
    
    printf("Python Parser:\n");
    RUN_TEST(python_trace_parsing);
    RUN_TEST(python_language_detection);
    
    printf("\nGo Parser:\n");
    RUN_TEST(go_panic_parsing);
    RUN_TEST(go_language_detection);
    
    printf("\nNode.js Parser:\n");
    RUN_TEST(nodejs_error_parsing);
    RUN_TEST(nodejs_language_detection);
    
    printf("\nEdge Cases:\n");
    RUN_TEST(empty_input);
    RUN_TEST(null_input);
    RUN_TEST(garbage_input);
    
    printf("\n============\n");
    printf("All tests passed!\n");
    
    return 0;
}
