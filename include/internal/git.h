/**
 * TraceMind - Git Context Collector
 * 
 * Uses libgit2 for repository analysis.
 * Compiles as stub when HAVE_LIBGIT2 is not defined.
 */

#ifndef TM_INTERNAL_GIT_H
#define TM_INTERNAL_GIT_H

#include "tracemind.h"

#ifdef HAVE_LIBGIT2
#include <git2.h>
#else
/* Stub types when libgit2 is not available */
typedef void git_repository;
typedef void git_commit;
typedef void git_blame;
typedef void git_diff;
typedef void git_oid;
#endif

/* ============================================================================
 * Library Initialization
 * ========================================================================== */

/**
 * Initialize libgit2.
 * Call once at startup.
 */
tm_error_t tm_git_init(void);

/**
 * Cleanup libgit2.
 */
void tm_git_cleanup(void);

/* ============================================================================
 * Repository Management
 * ========================================================================== */

/**
 * Git repository wrapper.
 */
typedef struct {
    git_repository *repo;
    char *root_path;
    char *branch;
    char head_sha[41];
} tm_git_repo_t;

/**
 * Open a repository.
 */
tm_error_t tm_git_repo_open(const char *path, tm_git_repo_t **repo);

/**
 * Free repository wrapper.
 */
void tm_git_repo_free(tm_git_repo_t *repo);

/**
 * Find repository root from a path inside it.
 */
tm_error_t tm_git_find_root(const char *path, char **root);

/* ============================================================================
 * Commit History
 * ========================================================================== */

/**
 * Options for commit collection.
 */
typedef struct {
    int max_commits;              /* Maximum commits to retrieve */
    const char **file_paths;      /* Filter to commits touching these files */
    size_t file_path_count;
    int64_t since_timestamp;      /* Only commits after this time (0 = no limit) */
    bool include_merges;          /* Include merge commits */
} tm_commit_opts_t;

/**
 * Get recent commits matching options.
 */
tm_error_t tm_git_get_commits(const tm_git_repo_t *repo,
                              const tm_commit_opts_t *opts,
                              tm_git_commit_t **commits,
                              size_t *count);

/**
 * Free commits array.
 */
void tm_git_commits_free(tm_git_commit_t *commits, size_t count);

/**
 * Get a single commit by SHA.
 */
tm_error_t tm_git_get_commit(const tm_git_repo_t *repo,
                             const char *sha,
                             tm_git_commit_t **commit);

/* ============================================================================
 * Blame Analysis
 * ========================================================================== */

/**
 * Options for blame operation.
 */
typedef struct {
    int start_line;              /* Start line (1-indexed) */
    int end_line;                /* End line (1-indexed, 0 = EOF) */
    const char *newest_commit;   /* Stop at this commit (NULL = HEAD) */
} tm_blame_opts_t;

/**
 * Get blame info for a file.
 */
tm_error_t tm_git_blame_file(const tm_git_repo_t *repo,
                             const char *file_path,
                             const tm_blame_opts_t *opts,
                             tm_git_blame_t **blames,
                             size_t *count);

/**
 * Free blame array.
 */
void tm_git_blames_free(tm_git_blame_t *blames, size_t count);

/**
 * Get blame for a specific line.
 */
tm_error_t tm_git_blame_line(const tm_git_repo_t *repo,
                             const char *file_path,
                             int line,
                             tm_git_blame_t **blame);

/* ============================================================================
 * Diff Analysis
 * ========================================================================== */

/**
 * Diff hunk information.
 */
typedef struct {
    int old_start;
    int old_lines;
    int new_start;
    int new_lines;
    char *content;               /* Diff content (owned) */
} tm_diff_hunk_t;

/**
 * File diff information.
 */
typedef struct {
    char *old_path;
    char *new_path;
    int additions;
    int deletions;
    tm_diff_hunk_t *hunks;
    size_t hunk_count;
    bool is_binary;
    bool is_renamed;
    bool is_deleted;
    bool is_new;
} tm_file_diff_t;

/**
 * Get diff for a commit.
 */
tm_error_t tm_git_commit_diff(const tm_git_repo_t *repo,
                              const char *sha,
                              tm_file_diff_t **diffs,
                              size_t *count);

/**
 * Free file diffs array.
 */
void tm_git_diffs_free(tm_file_diff_t *diffs, size_t count);

/**
 * Get diff between two commits for specific files.
 */
tm_error_t tm_git_diff_commits(const tm_git_repo_t *repo,
                               const char *old_sha,
                               const char *new_sha,
                               const char **files,
                               size_t file_count,
                               tm_file_diff_t **diffs,
                               size_t *diff_count);

/* ============================================================================
 * File History
 * ========================================================================== */

/**
 * File change entry.
 */
typedef struct {
    char sha[41];
    int64_t timestamp;
    int additions;
    int deletions;
    char *message_first_line;     /* First line of commit message */
} tm_file_change_t;

/**
 * Get change history for a file.
 */
tm_error_t tm_git_file_history(const tm_git_repo_t *repo,
                               const char *file_path,
                               int max_entries,
                               tm_file_change_t **changes,
                               size_t *count);

/**
 * Free file changes array.
 */
void tm_git_file_changes_free(tm_file_change_t *changes, size_t count);

/* ============================================================================
 * Context Collection (High-Level)
 * ========================================================================== */

/* Note: tm_git_collect_context is declared in tracemind.h as:
 * tm_git_context_t *tm_git_collect_context(const char *repo_path,
 *                                          const char **files,
 *                                          size_t file_count,
 *                                          int max_commits);
 */

/* ============================================================================
 * Helper Functions
 * ========================================================================== */

/**
 * Check if a file path is a config file.
 */
bool tm_git_is_config_file(const char *path);

/**
 * Check if a file path is a schema/migration file.
 */
bool tm_git_is_schema_file(const char *path);

/**
 * Format timestamp as ISO 8601 string.
 */
char *tm_git_format_timestamp(int64_t timestamp);

/**
 * Parse short SHA to full SHA.
 */
tm_error_t tm_git_resolve_sha(const tm_git_repo_t *repo,
                              const char *short_sha,
                              char full_sha[41]);

#endif /* TM_INTERNAL_GIT_H */
