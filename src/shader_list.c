/*
 * Shared shader directory scanning. See shader_list.h.
 */

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "shader_list.h"

static int compare_strings(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

void shader_list_init(shader_list *l)
{
    l->items = NULL;
    l->count = 0;
}

void shader_list_free(shader_list *l)
{
    if (!l->items) {
        l->count = 0;
        return;
    }
    for (int i = 0; i < l->count; i++)
        free(l->items[i]);
    free(l->items);
    l->items = NULL;
    l->count = 0;
}

int shader_list_add(shader_list *l, const char *path)
{
    if (!path) return -1;

    /* Keep a trailing NULL so callers can treat items as a C array. */
    char **grown = realloc(l->items, ((size_t)l->count + 2) * sizeof *grown);
    if (!grown) return -1;

    char *copy = malloc(strlen(path) + 1);
    if (!copy) return -1;
    strcpy(copy, path);

    l->items = grown;
    l->items[l->count++] = copy;
    l->items[l->count] = NULL;
    return 0;
}

int shader_list_scan(shader_list *l, const char *dir)
{
    shader_list_free(l);

    DIR *d = opendir(dir);
    if (!d) return -1;

    struct dirent *ent;
    int alloc_failed = 0;

    while ((ent = readdir(d)) != NULL) {
        const char *nm = ent->d_name;
        size_t nlen = strlen(nm);
        if (nlen <= 5 || strcmp(nm + nlen - 5, ".glsl") != 0)
            continue;

        size_t plen = strlen(dir) + 1 + nlen + 1;
        char *path = malloc(plen);
        if (!path) {
            alloc_failed = 1;
            break;
        }
        snprintf(path, plen, "%s/%s", dir, nm);

        if (shader_list_add(l, path) != 0) {
            free(path);
            alloc_failed = 1;
            break;
        }
        free(path);
    }
    closedir(d);

    /* Sorting only the prefix we actually filled keeps the list consistent
     * even if allocation failed midway. */
    if (l->count > 1)
        qsort(l->items, (size_t)l->count, sizeof *l->items, compare_strings);

    if (alloc_failed)
        fprintf(stderr,
                "Warning: out of memory while scanning '%s'; "
                "listing only the first %d shader(s).\n",
                dir, l->count);

    return l->count;
}

const char *shader_list_basename(const char *path)
{
    if (!path) return "";
    const char *p = strrchr(path, '/');
    const char *q = strrchr(path, '\\');
    if (q && (!p || q > p)) p = q;
    return p ? p + 1 : path;
}
