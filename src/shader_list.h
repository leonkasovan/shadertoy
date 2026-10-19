/*
 * Shared shader directory scanning.
 *
 * The viewer and the MCP server both need the same answer to "which shaders
 * exist?", so the scan lives here and grows dynamically. Earlier revisions
 * kept a fixed 256-entry array in each file and silently dropped anything
 * beyond it; that is now impossible by construction.
 */

#ifndef SHADER_LIST_H
#define SHADER_LIST_H

/* An ordered, owned list of shader paths. `items` is a NULL-terminated array
 * of malloc'd strings, sorted lexicographically so indices are stable across
 * runs and match `ls` order. */
typedef struct {
    char **items;
    int    count;
} shader_list;

/* Initialise an empty list. */
void shader_list_init(shader_list *l);

/* Free every path and reset the list. Safe on an already-empty list. */
void shader_list_free(shader_list *l);

/* Append one path (strdup'd; the caller keeps ownership of `path`). Returns 0
 * on success, -1 on allocation failure. */
int shader_list_add(shader_list *l, const char *path);

/* Scan `dir` for *.glsl files, replacing any existing contents. The list is
 * sorted afterwards. Returns the number of shaders found, or -1 if the
 * directory could not be opened (in which case the list is left empty).
 *
 * If allocation fails partway through, the partial list is kept and a warning
 * is printed to stderr rather than silently truncating. */
int shader_list_scan(shader_list *l, const char *dir);

/* Basename of a path, handling both '/' and '\\'. */
const char *shader_list_basename(const char *path);

#endif /* SHADER_LIST_H */
