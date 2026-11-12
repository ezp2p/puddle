#ifndef PUDDLE_YAMLTREE_H
#define PUDDLE_YAMLTREE_H

/* A small read-only tree over libyaml, for config and profile files.
 * Supports one document of maps, lists and scalars. Anchors, aliases and
 * duplicate keys are errors rather than being quietly interpreted. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ynode ynode;
typedef enum { Y_SCALAR, Y_MAP, Y_SEQ } ykind;

/* Parse YAML text. An empty document is an empty map. NULL + "line N: message" in `err` on error. */
ynode *puddle_yaml_parse(const char *text, size_t len, char *err, size_t errlen);

/* Read and parse a file (at most 1 MiB). Errors are prefixed with the path. */
ynode *puddle_yaml_parse_file(const char *path, char *err, size_t errlen);

void puddle_yaml_free(ynode *n);

ykind ynode_kind(const ynode *n);
int ynode_line(const ynode *n);                      /* 1-based line where the node starts */
const char *ynode_scalar(const ynode *n);            /* the text, or NULL if not a scalar */
int ynode_is_null(const ynode *n);                   /* empty, ~ or null (unquoted) */
size_t ynode_count(const ynode *n);                  /* items in a list, or pairs in a map */
const ynode *ynode_item(const ynode *n, size_t i);   /* list item, or map value i */
const char *ynode_key(const ynode *n, size_t i);     /* map key i */
const ynode *ynode_get(const ynode *map, const char *key); /* NULL if absent */

#ifdef __cplusplus
}
#endif
#endif
