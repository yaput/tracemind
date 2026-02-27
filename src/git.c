/**
 * TraceMind - Git Context Collector
 * 
 * Uses libgit2 for repository analysis.
 * Compiles as stub when HAVE_LIBGIT2 is not defined.
 */

#include "internal/common.h"
#include "internal/git.h"
#include <time.h>

#ifdef HAVE_LIBGIT2

/* Suppress missing-field-initializer warnings from libgit2's init macros */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

#include <git2.h>

/* ============================================================================
 * Initialization
 * ========================================================================== */

static bool g_git_initialized = false;

tm_error_t tm_git_init(void)
{
    if (g_git_initialized) return TM_OK;
    
    int err = git_libgit2_init();
    if (err < 0) {
        TM_ERROR("Failed to initialize libgit2: %d", err);
        return TM_ERR_GIT;
    }
    
    g_git_initialized = true;
    TM_DEBUG("Git module initialized (libgit2 v%s)", LIBGIT2_VERSION);
    return TM_OK;
}

void tm_git_cleanup(void)
{
    if (g_git_initialized) {
        git_libgit2_shutdown();
        g_git_initialized = false;
    }
}

/* ============================================================================
 * Error Handling Helper
 * ========================================================================== */

static tm_error_t git_error_to_tm(int git_err)
{
    if (git_err == 0) return TM_OK;
    
    const git_error *e = git_error_last();
    if (e) {
        TM_ERROR("Git error: %s", e->message);
    }
    
    switch (git_err) {
    case GIT_ENOTFOUND:  return TM_ERR_NOT_FOUND;
    case GIT_EINVALIDSPEC: return TM_ERR_INVALID_ARG;
    default: return TM_ERR_GIT;
    }
}

/* ============================================================================
 * Repository Management
 * ========================================================================== */

tm_error_t tm_git_repo_open(const char *path, tm_git_repo_t **result)
{
    TM_CHECK_NULL(path, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(result, TM_ERR_INVALID_ARG);
    
    *result = NULL;
    
    /* Ensure git is initialized */
    tm_error_t init_err = tm_git_init();
    if (init_err != TM_OK) return init_err;
    
    tm_git_repo_t *repo = tm_calloc(1, sizeof(tm_git_repo_t));
    
    /* Open repository */
    int err = git_repository_open(&repo->repo, path);
    if (err != 0) {
        TM_ERROR("Failed to open repository at: %s", path);
        free(repo);
        return git_error_to_tm(err);
    }
    
    /* Get repository root */
    const char *workdir = git_repository_workdir(repo->repo);
    repo->root_path = workdir ? tm_strdup(workdir) : tm_strdup(path);
    
    /* Remove trailing slash if present */
    size_t len = strlen(repo->root_path);
    if (len > 0 && repo->root_path[len - 1] == '/') {
        repo->root_path[len - 1] = '\0';
    }
    
    /* Get current branch */
    git_reference *head = NULL;
    err = git_repository_head(&head, repo->repo);
    if (err == 0 && head) {
        const char *branch_name = NULL;
        if (git_branch_name(&branch_name, head) == 0 && branch_name) {
            repo->branch = tm_strdup(branch_name);
        } else {
            repo->branch = tm_strdup("HEAD");
        }
        
        /* Get HEAD SHA */
        const git_oid *oid = git_reference_target(head);
        if (oid) {
            git_oid_tostr(repo->head_sha, sizeof(repo->head_sha), oid);
        }
        
        git_reference_free(head);
    } else {
        repo->branch = tm_strdup("(detached)");
    }
    
    TM_DEBUG("Opened repository: %s (branch: %s)", repo->root_path, repo->branch);
    *result = repo;
    return TM_OK;
}

void tm_git_repo_free(tm_git_repo_t *repo)
{
    if (!repo) return;
    
    if (repo->repo) git_repository_free(repo->repo);
    TM_FREE(repo->root_path);
    TM_FREE(repo->branch);
    free(repo);
}

tm_error_t tm_git_find_root(const char *path, char **root)
{
    TM_CHECK_NULL(path, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(root, TM_ERR_INVALID_ARG);
    
    *root = NULL;
    
    tm_error_t init_err = tm_git_init();
    if (init_err != TM_OK) return init_err;
    
    git_buf buf = GIT_BUF_INIT;
    int err = git_repository_discover(&buf, path, 0, NULL);
    
    if (err != 0) {
        TM_ERROR("Could not find git repository from: %s", path);
        return git_error_to_tm(err);
    }
    
    *root = tm_strdup(buf.ptr);
    git_buf_dispose(&buf);
    
    /* Convert .git path to workdir path */
    size_t len = strlen(*root);
    if (len > 5 && strcmp(*root + len - 5, ".git/") == 0) {
        (*root)[len - 5] = '\0';
    }
    
    return TM_OK;
}

/* ============================================================================
 * Commit History
 * ========================================================================== */

static void free_commit_contents(tm_git_commit_t *commit)
{
    if (!commit) return;
    
    TM_FREE(commit->author);
    TM_FREE(commit->email);
    TM_FREE(commit->message);
    
    for (size_t i = 0; i < commit->file_count; i++) {
        TM_FREE(commit->files_changed[i]);
    }
    TM_FREE(commit->files_changed);
}

void tm_git_commits_free(tm_git_commit_t *commits, size_t count)
{
    if (!commits) return;
    
    for (size_t i = 0; i < count; i++) {
        free_commit_contents(&commits[i]);
    }
    free(commits);
}

/**
 * Check if commit touches any of the specified files.
 */
static bool commit_touches_files(git_repository *repo,
                                 git_commit *commit,
                                 const char **files,
                                 size_t file_count)
{
    if (!files || file_count == 0) return true;  /* No filter = all commits */
    
    git_tree *tree = NULL, *parent_tree = NULL;
    git_diff *diff = NULL;
    bool touches = false;
    
    if (git_commit_tree(&tree, commit) != 0) return false;
    
    /* Get parent tree (or empty for first commit) */
    if (git_commit_parentcount(commit) > 0) {
        git_commit *parent = NULL;
        if (git_commit_parent(&parent, commit, 0) == 0) {
            git_commit_tree(&parent_tree, parent);
            git_commit_free(parent);
        }
    }
    
    /* Get diff */
    git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
    if (git_diff_tree_to_tree(&diff, repo, parent_tree, tree, &opts) != 0) {
        goto cleanup;
    }
    
    /* Check each delta */
    size_t delta_count = git_diff_num_deltas(diff);
    for (size_t i = 0; i < delta_count && !touches; i++) {
        const git_diff_delta *delta = git_diff_get_delta(diff, i);
        
        for (size_t j = 0; j < file_count; j++) {
            if (delta->old_file.path && strstr(delta->old_file.path, files[j])) {
                touches = true;
                break;
            }
            if (delta->new_file.path && strstr(delta->new_file.path, files[j])) {
                touches = true;
                break;
            }
        }
    }
    
cleanup:
    if (diff) git_diff_free(diff);
    if (tree) git_tree_free(tree);
    if (parent_tree) git_tree_free(parent_tree);
    
    return touches;
}

/**
 * Get files changed in a commit.
 */
static void get_commit_files(git_repository *repo,
                             git_commit *commit,
                             char ***files,
                             size_t *count,
                             int *additions,
                             int *deletions,
                             bool *touches_config,
                             bool *touches_schema)
{
    *files = NULL;
    *count = 0;
    *additions = 0;
    *deletions = 0;
    *touches_config = false;
    *touches_schema = false;
    
    git_tree *tree = NULL, *parent_tree = NULL;
    git_diff *diff = NULL;
    
    if (git_commit_tree(&tree, commit) != 0) return;
    
    if (git_commit_parentcount(commit) > 0) {
        git_commit *parent = NULL;
        if (git_commit_parent(&parent, commit, 0) == 0) {
            git_commit_tree(&parent_tree, parent);
            git_commit_free(parent);
        }
    }
    
    git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
    if (git_diff_tree_to_tree(&diff, repo, parent_tree, tree, &opts) != 0) {
        goto cleanup;
    }
    
    /* Get stats */
    git_diff_stats *stats = NULL;
    if (git_diff_get_stats(&stats, diff) == 0) {
        *additions = (int)git_diff_stats_insertions(stats);
        *deletions = (int)git_diff_stats_deletions(stats);
        git_diff_stats_free(stats);
    }
    
    /* Collect file paths */
    size_t delta_count = git_diff_num_deltas(diff);
    if (delta_count > 0) {
        *files = tm_malloc(delta_count * sizeof(char *));
        
        for (size_t i = 0; i < delta_count; i++) {
            const git_diff_delta *delta = git_diff_get_delta(diff, i);
            const char *path = delta->new_file.path ? delta->new_file.path 
                                                     : delta->old_file.path;
            
            (*files)[*count] = tm_strdup(path);
            (*count)++;
            
            /* Check for config/schema files */
            if (tm_git_is_config_file(path)) *touches_config = true;
            if (tm_git_is_schema_file(path)) *touches_schema = true;
        }
    }
    
cleanup:
    if (diff) git_diff_free(diff);
    if (tree) git_tree_free(tree);
    if (parent_tree) git_tree_free(parent_tree);
}

tm_error_t tm_git_get_commits(const tm_git_repo_t *repo,
                              const tm_commit_opts_t *opts,
                              tm_git_commit_t **commits,
                              size_t *count)
{
    TM_CHECK_NULL(repo, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(commits, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(count, TM_ERR_INVALID_ARG);
    
    *commits = NULL;
    *count = 0;
    
    int max = opts ? opts->max_commits : 20;
    if (max <= 0) max = 20;
    
    /* Set up revwalk */
    git_revwalk *walk = NULL;
    int err = git_revwalk_new(&walk, repo->repo);
    if (err != 0) return git_error_to_tm(err);
    
    git_revwalk_sorting(walk, GIT_SORT_TIME);
    
    err = git_revwalk_push_head(walk);
    if (err != 0) {
        git_revwalk_free(walk);
        return git_error_to_tm(err);
    }
    
    /* Allocate result array */
    tm_git_commit_t *result = tm_calloc((size_t)max, sizeof(tm_git_commit_t));
    size_t collected = 0;
    
    git_oid oid;
    while (collected < (size_t)max && git_revwalk_next(&oid, walk) == 0) {
        git_commit *commit = NULL;
        if (git_commit_lookup(&commit, repo->repo, &oid) != 0) continue;
        
        /* Check file filter */
        if (opts && opts->file_paths && opts->file_path_count > 0) {
            if (!commit_touches_files(repo->repo, commit, 
                                      opts->file_paths, opts->file_path_count)) {
                git_commit_free(commit);
                continue;
            }
        }
        
        /* Check merge filter */
        if (opts && !opts->include_merges && git_commit_parentcount(commit) > 1) {
            git_commit_free(commit);
            continue;
        }
        
        /* Check timestamp filter */
        int64_t commit_time = (int64_t)git_commit_time(commit);
        if (opts && opts->since_timestamp > 0 && commit_time < opts->since_timestamp) {
            git_commit_free(commit);
            break;  /* Commits are sorted by time, so we can stop */
        }
        
        /* Fill in commit data */
        tm_git_commit_t *c = &result[collected];
        
        git_oid_tostr(c->sha, sizeof(c->sha), &oid);
        
        const git_signature *author = git_commit_author(commit);
        if (author) {
            c->author = tm_strdup(author->name);
            c->email = tm_strdup(author->email);
        }
        
        c->timestamp = commit_time;
        c->message = tm_strdup(git_commit_message(commit));
        
        /* Get changed files */
        get_commit_files(repo->repo, commit,
                         &c->files_changed, &c->file_count,
                         &c->additions, &c->deletions,
                         &c->touches_config, &c->touches_schema);
        
        git_commit_free(commit);
        collected++;
    }
    
    git_revwalk_free(walk);
    
    *commits = result;
    *count = collected;
    
    TM_DEBUG("Collected %zu commits", collected);
    return TM_OK;
}

tm_error_t tm_git_get_commit(const tm_git_repo_t *repo,
                             const char *sha,
                             tm_git_commit_t **result)
{
    TM_CHECK_NULL(repo, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(sha, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(result, TM_ERR_INVALID_ARG);
    
    *result = NULL;
    
    git_oid oid;
    if (git_oid_fromstrp(&oid, sha) != 0) {
        return TM_ERR_INVALID_ARG;
    }
    
    git_commit *commit = NULL;
    int err = git_commit_lookup(&commit, repo->repo, &oid);
    if (err != 0) return git_error_to_tm(err);
    
    tm_git_commit_t *c = tm_calloc(1, sizeof(tm_git_commit_t));
    
    git_oid_tostr(c->sha, sizeof(c->sha), &oid);
    
    const git_signature *author = git_commit_author(commit);
    if (author) {
        c->author = tm_strdup(author->name);
        c->email = tm_strdup(author->email);
    }
    
    c->timestamp = (int64_t)git_commit_time(commit);
    c->message = tm_strdup(git_commit_message(commit));
    
    get_commit_files(repo->repo, commit,
                     &c->files_changed, &c->file_count,
                     &c->additions, &c->deletions,
                     &c->touches_config, &c->touches_schema);
    
    git_commit_free(commit);
    
    *result = c;
    return TM_OK;
}

/* ============================================================================
 * Blame Analysis
 * ========================================================================== */

void tm_git_blames_free(tm_git_blame_t *blames, size_t count)
{
    if (!blames) return;
    
    for (size_t i = 0; i < count; i++) {
        TM_FREE(blames[i].author);
        TM_FREE(blames[i].line_content);
    }
    free(blames);
}

tm_error_t tm_git_blame_file(const tm_git_repo_t *repo,
                             const char *file_path,
                             const tm_blame_opts_t *opts,
                             tm_git_blame_t **blames,
                             size_t *count)
{
    TM_CHECK_NULL(repo, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(file_path, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(blames, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(count, TM_ERR_INVALID_ARG);
    
    *blames = NULL;
    *count = 0;
    
    git_blame_options blame_opts = GIT_BLAME_OPTIONS_INIT;
    
    if (opts) {
        if (opts->start_line > 0) {
            blame_opts.min_line = (uint32_t)opts->start_line;
        }
        if (opts->end_line > 0) {
            blame_opts.max_line = (uint32_t)opts->end_line;
        }
    }
    
    git_blame *blame = NULL;
    int err = git_blame_file(&blame, repo->repo, file_path, &blame_opts);
    if (err != 0) return git_error_to_tm(err);
    
    uint32_t hunk_count = git_blame_get_hunk_count(blame);
    if (hunk_count == 0) {
        git_blame_free(blame);
        return TM_OK;
    }
    
    /* Allocate result */
    tm_git_blame_t *result = tm_calloc(hunk_count, sizeof(tm_git_blame_t));
    
    for (uint32_t i = 0; i < hunk_count; i++) {
        const git_blame_hunk *hunk = git_blame_get_hunk_byindex(blame, i);
        if (!hunk) continue;
        
        tm_git_blame_t *b = &result[*count];
        
        git_oid_tostr(b->sha, sizeof(b->sha), &hunk->final_commit_id);
        
        if (hunk->final_signature) {
            b->author = tm_strdup(hunk->final_signature->name);
            b->timestamp = (int64_t)hunk->final_signature->when.time;
        }
        
        (*count)++;
    }
    
    git_blame_free(blame);
    *blames = result;
    
    return TM_OK;
}

tm_error_t tm_git_blame_line(const tm_git_repo_t *repo,
                             const char *file_path,
                             int line,
                             tm_git_blame_t **blame)
{
    tm_blame_opts_t opts = { .start_line = line, .end_line = line };
    size_t count = 0;
    
    tm_error_t err = tm_git_blame_file(repo, file_path, &opts, blame, &count);
    if (err != TM_OK) return err;
    
    if (count == 0) {
        *blame = NULL;
        return TM_ERR_NOT_FOUND;
    }
    
    return TM_OK;
}

/* ============================================================================
 * Diff Analysis
 * ========================================================================== */

void tm_git_diffs_free(tm_file_diff_t *diffs, size_t count)
{
    if (!diffs) return;
    
    for (size_t i = 0; i < count; i++) {
        TM_FREE(diffs[i].old_path);
        TM_FREE(diffs[i].new_path);
        
        for (size_t j = 0; j < diffs[i].hunk_count; j++) {
            TM_FREE(diffs[i].hunks[j].content);
        }
        TM_FREE(diffs[i].hunks);
    }
    free(diffs);
}

tm_error_t tm_git_commit_diff(const tm_git_repo_t *repo,
                              const char *sha,
                              tm_file_diff_t **diffs,
                              size_t *count)
{
    TM_CHECK_NULL(repo, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(sha, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(diffs, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(count, TM_ERR_INVALID_ARG);
    
    *diffs = NULL;
    *count = 0;
    
    git_oid oid;
    if (git_oid_fromstrp(&oid, sha) != 0) {
        return TM_ERR_INVALID_ARG;
    }
    
    git_commit *commit = NULL;
    int err = git_commit_lookup(&commit, repo->repo, &oid);
    if (err != 0) return git_error_to_tm(err);
    
    git_tree *tree = NULL, *parent_tree = NULL;
    git_diff *diff = NULL;
    
    if (git_commit_tree(&tree, commit) != 0) {
        git_commit_free(commit);
        return TM_ERR_GIT;
    }
    
    if (git_commit_parentcount(commit) > 0) {
        git_commit *parent = NULL;
        if (git_commit_parent(&parent, commit, 0) == 0) {
            git_commit_tree(&parent_tree, parent);
            git_commit_free(parent);
        }
    }
    
    git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
    opts.context_lines = 3;
    
    err = git_diff_tree_to_tree(&diff, repo->repo, parent_tree, tree, &opts);
    if (err != 0) {
        if (tree) git_tree_free(tree);
        if (parent_tree) git_tree_free(parent_tree);
        git_commit_free(commit);
        return git_error_to_tm(err);
    }
    
    /* Convert to our format */
    size_t delta_count = git_diff_num_deltas(diff);
    if (delta_count > 0) {
        *diffs = tm_calloc(delta_count, sizeof(tm_file_diff_t));
        
        for (size_t i = 0; i < delta_count; i++) {
            const git_diff_delta *delta = git_diff_get_delta(diff, i);
            tm_file_diff_t *d = &(*diffs)[*count];
            
            d->old_path = delta->old_file.path ? tm_strdup(delta->old_file.path) : NULL;
            d->new_path = delta->new_file.path ? tm_strdup(delta->new_file.path) : NULL;
            d->is_binary = (delta->flags & GIT_DIFF_FLAG_BINARY) != 0;
            d->is_renamed = delta->status == GIT_DELTA_RENAMED;
            d->is_deleted = delta->status == GIT_DELTA_DELETED;
            d->is_new = delta->status == GIT_DELTA_ADDED;
            
            /* Get patch for hunk details */
            git_patch *patch = NULL;
            if (git_patch_from_diff(&patch, diff, i) == 0) {
                size_t adds = 0, dels = 0;
                git_patch_line_stats(NULL, &adds, &dels, patch);
                d->additions = (int)adds;
                d->deletions = (int)dels;
                
                /* Extract hunks */
                size_t hunk_count = git_patch_num_hunks(patch);
                if (hunk_count > 0) {
                    d->hunks = tm_calloc(hunk_count, sizeof(tm_diff_hunk_t));
                    d->hunk_count = hunk_count;
                    
                    for (size_t h = 0; h < hunk_count; h++) {
                        const git_diff_hunk *git_hunk = NULL;
                        if (git_patch_get_hunk(&git_hunk, NULL, patch, h) == 0) {
                            d->hunks[h].old_start = git_hunk->old_start;
                            d->hunks[h].old_lines = git_hunk->old_lines;
                            d->hunks[h].new_start = git_hunk->new_start;
                            d->hunks[h].new_lines = git_hunk->new_lines;
                        }
                    }
                }
                
                git_patch_free(patch);
            }
            
            (*count)++;
        }
    }
    
    git_diff_free(diff);
    if (tree) git_tree_free(tree);
    if (parent_tree) git_tree_free(parent_tree);
    git_commit_free(commit);
    
    return TM_OK;
}

/* ============================================================================
 * File History
 * ========================================================================== */

void tm_git_file_changes_free(tm_file_change_t *changes, size_t count)
{
    if (!changes) return;
    
    for (size_t i = 0; i < count; i++) {
        TM_FREE(changes[i].message_first_line);
    }
    free(changes);
}

tm_error_t tm_git_file_history(const tm_git_repo_t *repo,
                               const char *file_path,
                               int max_entries,
                               tm_file_change_t **changes,
                               size_t *count)
{
    TM_CHECK_NULL(repo, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(file_path, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(changes, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(count, TM_ERR_INVALID_ARG);
    
    *changes = NULL;
    *count = 0;
    
    if (max_entries <= 0) max_entries = 10;
    
    /* Use revwalk with pathspec */
    git_revwalk *walk = NULL;
    int err = git_revwalk_new(&walk, repo->repo);
    if (err != 0) return git_error_to_tm(err);
    
    git_revwalk_sorting(walk, GIT_SORT_TIME);
    git_revwalk_push_head(walk);
    
    tm_file_change_t *result = tm_calloc((size_t)max_entries, sizeof(tm_file_change_t));
    
    git_oid oid;
    while (*count < (size_t)max_entries && git_revwalk_next(&oid, walk) == 0) {
        git_commit *commit = NULL;
        if (git_commit_lookup(&commit, repo->repo, &oid) != 0) continue;
        
        /* Check if commit touches our file */
        bool touches = commit_touches_files(repo->repo, commit, 
                                            (const char **)&file_path, 1);
        
        if (touches) {
            tm_file_change_t *c = &result[*count];
            
            git_oid_tostr(c->sha, sizeof(c->sha), &oid);
            c->timestamp = (int64_t)git_commit_time(commit);
            
            /* Get first line of message */
            const char *msg = git_commit_message(commit);
            if (msg) {
                const char *newline = strchr(msg, '\n');
                if (newline) {
                    c->message_first_line = tm_strndup(msg, (size_t)(newline - msg));
                } else {
                    c->message_first_line = tm_strdup(msg);
                }
            }
            
            (*count)++;
        }
        
        git_commit_free(commit);
    }
    
    git_revwalk_free(walk);
    *changes = result;
    
    return TM_OK;
}

/* ============================================================================
 * High-Level Context Collection
 * ========================================================================== */

void tm_git_context_free(tm_git_context_t *ctx)
{
    if (!ctx) return;
    
    TM_FREE(ctx->repo_root);
    TM_FREE(ctx->current_branch);
    TM_FREE(ctx->head_sha);
    
    tm_git_commits_free(ctx->commits, ctx->commit_count);
    
    for (size_t i = 0; i < ctx->blame_count; i++) {
        tm_git_blames_free(ctx->blames[i], 1);
    }
    TM_FREE(ctx->blames);
    
    free(ctx);
}

static tm_error_t git_collect_context_from_trace(const char *repo_path,
                                                 const tm_stack_trace_t *trace,
                                                 int max_commits,
                                                 tm_git_context_t **result)
{
    TM_CHECK_NULL(repo_path, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(trace, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(result, TM_ERR_INVALID_ARG);
    
    *result = NULL;
    
    /* Open repository */
    tm_git_repo_t *repo = NULL;
    tm_error_t err = tm_git_repo_open(repo_path, &repo);
    if (err != TM_OK) return err;
    
    tm_git_context_t *ctx = tm_calloc(1, sizeof(tm_git_context_t));
    ctx->repo_root = tm_strdup(repo->root_path);
    ctx->current_branch = tm_strdup(repo->branch);
    ctx->head_sha = tm_strdup(repo->head_sha);
    
    /* Collect file paths from trace */
    const char **file_paths = NULL;
    size_t file_count = 0;
    
    for (size_t i = 0; i < trace->frame_count; i++) {
        if (trace->frames[i].file && !trace->frames[i].is_stdlib) {
            /* Check for duplicates */
            bool found = false;
            for (size_t j = 0; j < file_count; j++) {
                if (strcmp(file_paths[j], trace->frames[i].file) == 0) {
                    found = true;
                    break;
                }
            }
            
            if (!found) {
                file_paths = tm_realloc(file_paths, (file_count + 1) * sizeof(char *));
                file_paths[file_count++] = trace->frames[i].file;
            }
        }
    }
    
    /* Get commits touching these files */
    tm_commit_opts_t opts = {
        .max_commits = max_commits > 0 ? max_commits : 20,
        .file_paths = file_paths,
        .file_path_count = file_count,
        .include_merges = false
    };
    
    tm_git_get_commits(repo, &opts, &ctx->commits, &ctx->commit_count);
    
    /* Get blame info for error lines */
    ctx->blames = NULL;
    ctx->blame_count = 0;
    
    for (size_t i = 0; i < trace->frame_count && i < 5; i++) {
        const tm_stack_frame_t *frame = &trace->frames[i];
        if (!frame->file || frame->is_stdlib || frame->line <= 0) continue;
        
        tm_git_blame_t *blame = NULL;
        if (tm_git_blame_line(repo, frame->file, frame->line, &blame) == TM_OK && blame) {
            ctx->blames = tm_realloc(ctx->blames, 
                                     (ctx->blame_count + 1) * sizeof(tm_git_blame_t *));
            ctx->blames[ctx->blame_count++] = blame;
        }
    }
    
    TM_FREE(file_paths);
    tm_git_repo_free(repo);
    
    TM_DEBUG("Collected git context: %zu commits, %zu blames", 
             ctx->commit_count, ctx->blame_count);
    
    *result = ctx;
    return TM_OK;
}

tm_git_context_t *tm_git_collect_context(const char *repo_path,
                                         const char **files,
                                         size_t file_count,
                                         int max_commits)
{
    const char *path = repo_path ? repo_path : ".";
    
    /* Create a minimal trace for the internal trace-based API */
    tm_stack_trace_t dummy_trace = {0};
    if (files && file_count > 0) {
        dummy_trace.frames = tm_calloc(file_count, sizeof(tm_stack_frame_t));
        dummy_trace.frame_count = file_count;
        for (size_t i = 0; i < file_count; i++) {
            dummy_trace.frames[i].file = tm_strdup(files[i]);
        }
    }
    
    tm_git_context_t *ctx = NULL;
    tm_error_t err = git_collect_context_from_trace(path, &dummy_trace, max_commits, &ctx);
    
    for (size_t i = 0; i < file_count; i++) {
        TM_FREE(dummy_trace.frames[i].file);
    }
    TM_FREE(dummy_trace.frames);
    
    if (err != TM_OK) {
        TM_WARN("Git context collection failed: %s", tm_strerror(err));
        return NULL;
    }
    
    return ctx;
}

/* ============================================================================
 * Helper Functions
 * ========================================================================== */

bool tm_git_is_config_file(const char *path)
{
    if (!path) return false;
    
    /* Common config file patterns */
    static const char *patterns[] = {
        ".env", ".yaml", ".yml", ".json", ".toml", ".ini",
        "config", "settings", "Dockerfile", "docker-compose",
        ".conf", ".cfg"
    };
    
    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        if (strstr(path, patterns[i])) return true;
    }
    
    return false;
}

bool tm_git_is_schema_file(const char *path)
{
    if (!path) return false;
    
    /* Database/schema file patterns */
    static const char *patterns[] = {
        "migration", "schema", ".sql", "alembic", "flyway",
        "prisma", "drizzle", "knex", "sequelize"
    };
    
    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        if (strstr(path, patterns[i])) return true;
    }
    
    return false;
}

char *tm_git_format_timestamp(int64_t timestamp)
{
    char *buf = tm_malloc(64);
    time_t t = (time_t)timestamp;
    struct tm *tm = gmtime(&t);
    
    strftime(buf, 64, "%Y-%m-%dT%H:%M:%SZ", tm);
    return buf;
}

tm_error_t tm_git_resolve_sha(const tm_git_repo_t *repo,
                              const char *short_sha,
                              char full_sha[41])
{
    TM_CHECK_NULL(repo, TM_ERR_INVALID_ARG);
    TM_CHECK_NULL(short_sha, TM_ERR_INVALID_ARG);
    
    git_oid oid;
    int err = git_oid_fromstrp(&oid, short_sha);
    if (err != 0) return git_error_to_tm(err);
    
    git_oid_tostr(full_sha, 41, &oid);
    return TM_OK;
}

#pragma GCC diagnostic pop

#else /* !HAVE_LIBGIT2 - Stub implementations */

/* ============================================================================
 * Stub Implementations (No libgit2 Available)
 * ========================================================================== */

static bool g_git_initialized = false;

tm_error_t tm_git_init(void)
{
    g_git_initialized = true;
    TM_DEBUG("Git module initialized (stub - no libgit2)");
    return TM_OK;
}

void tm_git_cleanup(void)
{
    g_git_initialized = false;
}

tm_error_t tm_git_repo_open(const char *path, tm_git_repo_t **repo)
{
    (void)path;
    *repo = NULL;
    TM_DEBUG("Git operations unavailable (no libgit2)");
    return TM_ERR_UNSUPPORTED;
}

void tm_git_repo_free(tm_git_repo_t *repo)
{
    if (repo) {
        TM_FREE(repo->root_path);
        TM_FREE(repo->branch);
        free(repo);
    }
}

const char *tm_git_repo_root(const tm_git_repo_t *repo)
{
    return repo ? repo->root_path : NULL;
}

const char *tm_git_repo_branch(const tm_git_repo_t *repo)
{
    return repo ? repo->branch : NULL;
}

const char *tm_git_repo_head_sha(const tm_git_repo_t *repo)
{
    return repo ? repo->head_sha : NULL;
}

tm_error_t tm_git_collect_commits(const tm_git_repo_t *repo,
                                  const char **files,
                                  size_t file_count,
                                  int max_commits,
                                  tm_git_commit_t **commits,
                                  size_t *commit_count)
{
    (void)repo;
    (void)files;
    (void)file_count;
    (void)max_commits;
    *commits = NULL;
    *commit_count = 0;
    return TM_ERR_UNSUPPORTED;
}

void tm_git_commit_free(tm_git_commit_t *commit)
{
    if (!commit) return;
    TM_FREE(commit->author);
    TM_FREE(commit->email);
    TM_FREE(commit->message);
    if (commit->files_changed) {
        for (size_t i = 0; i < commit->file_count; i++) {
            TM_FREE(commit->files_changed[i]);
        }
        TM_FREE(commit->files_changed);
    }
}

tm_error_t tm_git_collect_blame(const tm_git_repo_t *repo,
                                const char *file_path,
                                int start_line,
                                int end_line,
                                tm_git_blame_t ***blames,
                                size_t *blame_count)
{
    (void)repo;
    (void)file_path;
    (void)start_line;
    (void)end_line;
    *blames = NULL;
    *blame_count = 0;
    return TM_ERR_UNSUPPORTED;
}

void tm_git_blame_free(tm_git_blame_t *blame)
{
    if (!blame) return;
    TM_FREE(blame->author);
    TM_FREE(blame->line_content);
}

tm_error_t tm_git_get_diff(const tm_git_repo_t *repo,
                           const char *sha,
                           char **diff)
{
    (void)repo;
    (void)sha;
    *diff = NULL;
    return TM_ERR_UNSUPPORTED;
}

tm_error_t tm_git_get_file_at_commit(const tm_git_repo_t *repo,
                                     const char *sha,
                                     const char *file_path,
                                     char **content,
                                     size_t *content_len)
{
    (void)repo;
    (void)sha;
    (void)file_path;
    *content = NULL;
    *content_len = 0;
    return TM_ERR_UNSUPPORTED;
}

void tm_git_context_free(tm_git_context_t *ctx)
{
    if (!ctx) return;
    TM_FREE(ctx->repo_root);
    TM_FREE(ctx->current_branch);
    TM_FREE(ctx->head_sha);
    for (size_t i = 0; i < ctx->commit_count; i++) {
        tm_git_commit_free(&ctx->commits[i]);
    }
    TM_FREE(ctx->commits);
    for (size_t i = 0; i < ctx->blame_count; i++) {
        tm_git_blame_free(ctx->blames[i]);
        free(ctx->blames[i]);
    }
    TM_FREE(ctx->blames);
    free(ctx);
}

tm_git_context_t *tm_git_collect_context(const char *repo_path,
                                         const char **files,
                                         size_t file_count,
                                         int max_commits)
{
    (void)repo_path;
    (void)files;
    (void)file_count;
    (void)max_commits;
    TM_DEBUG("Git context collection unavailable (no libgit2)");
    return NULL;
}

bool tm_git_is_config_file(const char *path)
{
    if (!path) return false;
    static const char *patterns[] = {
        ".env", ".yaml", ".yml", ".toml", ".ini", ".cfg",
        ".json", ".config", "config.", "settings.", ".conf"
    };
    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        if (strstr(path, patterns[i])) return true;
    }
    return false;
}

bool tm_git_is_schema_file(const char *path)
{
    if (!path) return false;
    static const char *patterns[] = {
        "migration", "schema", ".sql", "alembic", "flyway",
        "prisma", "drizzle", "knex", "sequelize"
    };
    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        if (strstr(path, patterns[i])) return true;
    }
    return false;
}

char *tm_git_format_timestamp(int64_t timestamp)
{
    char *buf = tm_malloc(64);
    time_t t = (time_t)timestamp;
    struct tm *tm_info = gmtime(&t);
    strftime(buf, 64, "%Y-%m-%dT%H:%M:%SZ", tm_info);
    return buf;
}

tm_error_t tm_git_resolve_sha(const tm_git_repo_t *repo,
                              const char *short_sha,
                              char full_sha[41])
{
    (void)repo;
    if (short_sha) {
        strncpy(full_sha, short_sha, 40);
        full_sha[40] = '\0';
    }
    return TM_OK;
}

#endif /* HAVE_LIBGIT2 */
