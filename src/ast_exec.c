/* SPDX-License-Identifier: 0BSD */

/* posish - AST execution helpers */

#include "ast_exec.h"

#include "redir.h"

int ast_exec_run_group_with_redirections(struct shell_state *state,
                                         const char *body,
                                         const struct redir_vec *redirs,
                                         ast_exec_body_runner run_body) {
    struct fd_backup_vec backups;
    struct redir_vec runtime_redirs;
    int status;

    backups.items = NULL;
    backups.len = 0;
    runtime_redirs.items = NULL;
    runtime_redirs.len = 0;
    if (prepare_runtime_redirections(state, redirs, &runtime_redirs) != 0) {
        return 1;
    }
    if (apply_redirections(state, &runtime_redirs, true, state->noclobber, false,
                           &backups) != 0) {
        fd_backup_restore(&backups);
        redir_vec_free(&runtime_redirs);
        return 1;
    }

    status = run_body(state, body);
    fd_backup_restore(&backups);
    redir_vec_free(&runtime_redirs);
    return status;
}

int ast_exec_run_subshell_group(struct shell_state *state,
                                const struct ast_node *node,
                                ast_exec_body_runner run_subshell_body) {
    return ast_exec_run_group_with_redirections(state, node->data.group.body,
                                                &node->data.group.redirs,
                                                run_subshell_body);
}

int ast_exec_run_brace_group(struct shell_state *state,
                             const struct ast_node *node,
                             ast_exec_body_runner run_brace_body) {
    return ast_exec_run_group_with_redirections(state, node->data.group.body,
                                                &node->data.group.redirs,
                                                run_brace_body);
}

int ast_exec_run_if(struct shell_state *state, const struct ast_node *node,
                    ast_exec_node_runner run_node,
                    ast_exec_errexit_hook maybe_trigger_errexit,
                    ast_exec_flow_control_pred has_pending_flow_control) {
    struct fd_backup_vec backups;
    struct redir_vec runtime_redirs;
    bool redir_applied;
    bool saved_errexit;
    int status;

    backups.items = NULL;
    backups.len = 0;
    runtime_redirs.items = NULL;
    runtime_redirs.len = 0;
    redir_applied = node->data.if_cmd.redirs.len != 0;

    if (redir_applied &&
        (prepare_runtime_redirections(state, &node->data.if_cmd.redirs,
                                      &runtime_redirs) != 0 ||
         apply_redirections(state, &runtime_redirs, true,
                            state->noclobber, false, &backups) != 0)) {
        fd_backup_restore(&backups);
        redir_vec_free(&runtime_redirs);
        return 1;
    }

    saved_errexit = state->errexit;
    state->errexit = false;
    status = run_node(state, node->data.if_cmd.cond_node, true);
    state->errexit = saved_errexit;
    if (!state->should_exit && !state->return_requested &&
        !has_pending_flow_control(state)) {
        if (status == 0) {
            status = run_node(state, node->data.if_cmd.then_node, true);
            maybe_trigger_errexit(state, status);
        } else if (node->data.if_cmd.else_node != NULL) {
            status = run_node(state, node->data.if_cmd.else_node, true);
            maybe_trigger_errexit(state, status);
        } else {
            status = 0;
        }
    }

    if (redir_applied) {
        fd_backup_restore(&backups);
        redir_vec_free(&runtime_redirs);
    }
    return status;
}

int ast_exec_run_loop(struct shell_state *state, const struct ast_node *node,
                      bool is_until, ast_exec_node_runner run_node,
                      ast_exec_errexit_hook maybe_trigger_errexit) {
    struct fd_backup_vec backups;
    struct redir_vec runtime_redirs;
    bool redir_applied;
    int status;

    status = 0;
    backups.items = NULL;
    backups.len = 0;
    runtime_redirs.items = NULL;
    runtime_redirs.len = 0;
    redir_applied = node->data.loop.redirs.len != 0;

    if (redir_applied &&
        (prepare_runtime_redirections(state, &node->data.loop.redirs,
                                      &runtime_redirs) != 0 ||
         apply_redirections(state, &runtime_redirs, true, state->noclobber,
                            false, &backups) != 0)) {
        fd_backup_restore(&backups);
        redir_vec_free(&runtime_redirs);
        return 1;
    }

    state->loop_depth++;
    while (!state->should_exit) {
        int cond_status;
        bool saved_errexit;

        saved_errexit = state->errexit;
        state->errexit = false;
        cond_status = run_node(state, node->data.loop.cond_node, true);
        state->errexit = saved_errexit;
        if (state->should_exit || state->return_requested) {
            break;
        }
        if ((!is_until && cond_status != 0) || (is_until && cond_status == 0)) {
            break;
        }
        status = run_node(state, node->data.loop.body_node, true);
        maybe_trigger_errexit(state, status);
        if (state->should_exit || state->return_requested) {
            break;
        }
        if (state->break_levels > 0) {
            state->break_levels--;
            status = 0;
            break;
        }
        if (state->continue_levels > 0) {
            state->continue_levels--;
            status = 0;
            if (state->continue_levels > 0) {
                break;
            }
            continue;
        }
    }
    state->loop_depth--;

    if (redir_applied) {
        fd_backup_restore(&backups);
        redir_vec_free(&runtime_redirs);
    }
    return status;
}
