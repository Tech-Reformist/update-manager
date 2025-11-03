#include <glib.h>
#include <gio/gio.h>
#include <ostree.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Opens the local OSTree repository at the specified path. */
OstreeRepo*
open_repo(const char *path, GError **error) {
    GFile *repo_path = g_file_new_for_path(path);
    OstreeRepo *repo = ostree_repo_new(repo_path);

    if (!ostree_repo_open(repo, NULL, error)) {
        g_object_unref(repo_path);
        g_object_unref(repo);
        return NULL;
    }

    g_object_unref(repo_path);
    return repo;
}

/* Lists the configured remotes in the given repository. */
char**
list_remote(
    OstreeRepo *repo,
    GError **error
) {
    guint n_repos = 0;
    char **remote_list = ostree_repo_remote_list(repo, &n_repos);
    if (!remote_list) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
        "Failed to list remotes");
    }
    return remote_list;
}

/* Adds a new remote to the local repository*/
gboolean
add_remote(
    OstreeRepo *repo,
    const char *remote_name,
    const char *remote_url,
    GError **error
) {
    return ostree_repo_remote_add(repo,remote_name, remote_url, NULL, NULL, error);
}

/* Pulls commits and metadata from the specified remote repository. */
gboolean
pull_remote(
    OstreeRepo *repo,
    const char *remote_name,
    char **refs,
    GError **error
) {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&builder, "{sv}", "refs", g_variant_new_strv((const char * const *)refs, -1));
    GVariant *options = g_variant_builder_end(&builder);
    g_variant_builder_clear(&builder);
    
    gboolean result = ostree_repo_pull_with_options(repo,remote_name, options,NULL,NULL,error);
    
    g_variant_unref(options);

    return result;
}

/* Resolves a reference name (branch) to its latest commit checksum. */
gboolean
resolve_rev(
    OstreeRepo *repo,
    const char *ref,
    char **new_commit,
    GError **error
) {
    return  ostree_repo_resolve_rev(repo, ref, 0, new_commit, error);
}

/* Loads the sysroot and refreshes information about all deployments,
 * including the booted, pending, and rollback states. */
gboolean
load_deployments(
    OstreeSysroot *sysroot,
    GError **error
) {
    return ostree_sysroot_load(sysroot, NULL,  error);
}

/* Creates an origin (refspec) file for the specified remote and reference. */
gboolean
create_origin(
    OstreeSysroot *sysroot,
    char *refspec_str,
    const char *remote_name,
    const char *ref,
    GKeyFile **origin
) {
    g_snprintf(refspec_str,sizeof(refspec_str), "%s:%s", remote_name, ref);
    *origin = ostree_sysroot_origin_new_from_refspec(sysroot, refspec_str);
    return *origin != NULL;
}

/* Deploys the specified commit and stages it for boot on the next restart. */
gboolean
deploy_tree(
    OstreeSysroot *sysroot,
    const char *osname,
    char *new_commit,
    GKeyFile *origin,
    GError **error
) {
    return ostree_sysroot_stage_tree_with_options(sysroot, osname, new_commit, origin, NULL, NULL, NULL, NULL, error);
}

int 
main() {
    const char *osname = "myos";
    const char *remote_name = "linuxmint";
    const char *remote_url = "https://updates.myserver.com/ostreerepo";
    GError *error = NULL;
    const char *ref = "myOS/amd64/stable";
    char *refs[] = { (char*)ref, NULL}; 
    int status = EXIT_SUCCESS;

    OstreeRepo *repo = NULL;
    if (!(repo = open_repo("/sysroot/ostree/repo", &error))) {
        g_printerr("Failed to open repo: %s\n", error->message);
        status = EXIT_FAILURE;
        goto cleanup;
    }
    g_print("Repository opened successfully!\n");

    char **remote_list = NULL;
    if (!(remote_list = list_remote(repo, &error))) {
        g_printerr("Failed to list remotes: %s\n", error->message);
        status = EXIT_FAILURE;
        goto cleanup;
    }
    for (int i = 0; remote_list[i] != NULL; i++) {
        g_print("Remote %d: %s\n", i + 1, remote_list[i]);
    }

    if (remote_list) {
        gboolean exist = FALSE;

        for (guint i = 0; remote_list[i] != NULL; i++) {
            if (g_strcmp0(remote_list[i], remote_name) == 0) {
                exist = TRUE;
                break;
            }
        }

        if (exist) {
            g_print("Remote '%s' already exists.\n", remote_name);
        } else {
            if (!add_remote(repo, remote_name, remote_url, &error)) {
                g_printerr("Failed to add remote: %s\n", error->message);
                status = EXIT_FAILURE;
                goto cleanup;
            }
            g_print("Remote '%s' added successfully.\n", remote_name);
        }

    } else {
        if (!add_remote(repo, remote_name, remote_url, &error)) {
            g_printerr("Failed to add remote: %s\n", error->message);
            status = EXIT_FAILURE;
            goto cleanup;
        }
        g_print("Remote '%s' added successfully.\n", remote_name);
    }

    if (!pull_remote(repo, remote_name, refs, &error)) {
        g_print("Failed to pull refs: %s\n", error->message);
        status = EXIT_FAILURE;
        goto cleanup;
    }
    g_print("Pull from remote '%s' completed successfully.\n", remote_name);

    char *new_commit = NULL;
    if (!resolve_rev(repo, ref, &new_commit,  &error)) {
        g_printerr("Failed to resolve commits: %s\n", error->message);
        status = EXIT_FAILURE;
        goto cleanup;
    }
    g_print("Resolved commit: %s\n", new_commit);

    OstreeSysroot *sysroot = ostree_sysroot_new_default();
    if (!load_deployments(sysroot, &error)) {
        g_printerr("Failed to load deployments: %s\n", error->message);
        status = EXIT_FAILURE;
        goto cleanup;
    }
    g_print("Sysroot deployments loaded successfully.\n");
    
    GKeyFile *origin = NULL;
    char refspec_str[128];
    if (!create_origin(sysroot, refspec_str, remote_name, ref, &origin)) {
        g_printerr("Failed to create an .origin file: %s\n", error->message);
        status = EXIT_FAILURE;
        goto cleanup;
    }
    g_print("Origin file created successfully.\n");

    if (!deploy_tree(sysroot, osname, new_commit, origin, &error)) {
        g_printerr("Failed to deploy tree: %s\n", error->message);
        status = EXIT_FAILURE;
        goto cleanup;
    }
    g_print("Deployed new commit %s. It will boot on next restart.\n", new_commit);

    if (!ostree_sysroot_cleanup(sysroot, NULL, &error)) {
        g_printerr("OSTree cleanup failed: %s\n", error->message);
        status = EXIT_FAILURE;
        goto cleanup;
    } else {
        g_print("OSTree cleanup completed successfully.\n");
    }

    cleanup:
        g_clear_error(&error);
        if (repo) {
            g_object_unref(repo);
        }
        if (remote_list) {
            g_strfreev(remote_list);
        }
        if (new_commit) {
            g_free(new_commit);
        }
        if (sysroot) {
            g_object_unref(sysroot);
        }
        if (origin) {
            g_object_unref(origin);
        }
        if (status == EXIT_SUCCESS) {
            g_print("All operations completed successfully.\n");
        } else {
            g_printerr("Update failed\n");
        }

        return status;
}