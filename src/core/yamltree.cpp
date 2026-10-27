#include "core/yamltree.h"

#include <yaml.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FILE_BYTES (1 << 20)
#define MAX_DEPTH 32

struct ynode {
    ykind kind;
    int line;
    char *scalar;  /* Y_SCALAR */
    int plain;     /* unquoted scalar */
    char **keys;   /* Y_MAP */
    ynode **vals;  /* Y_MAP values, or Y_SEQ items */
    size_t n, cap;
};

static void seterr(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

static ynode *node_new(ykind kind, int line)
{
    ynode *n = (ynode *)calloc(1, sizeof *n);
    if (n) {
        n->kind = kind;
        n->line = line;
    }
    return n;
}

void puddle_yaml_free(ynode *n)
{
    if (!n)
        return;
    free(n->scalar);
    for (size_t i = 0; i < n->n; i++) {
        if (n->keys)
            free(n->keys[i]);
        puddle_yaml_free(n->vals[i]);
    }
    free(n->keys);
    free(n->vals);
    free(n);
}

/* append a child; for maps `key` is taken over. 0 ok, -1 out of memory */
static int node_push(ynode *parent, char *key, ynode *child)
{
    if (parent->n == parent->cap) {
        size_t ncap = parent->cap ? parent->cap * 2 : 8;
        ynode **nv = (ynode **)realloc(parent->vals, ncap * sizeof *nv);
        if (!nv)
            return -1;
        parent->vals = nv;
        if (parent->kind == Y_MAP) {
            char **nk = (char **)realloc(parent->keys, ncap * sizeof *nk);
            if (!nk)
                return -1;
            parent->keys = nk;
        }
        parent->cap = ncap;
    }
    if (parent->kind == Y_MAP)
        parent->keys[parent->n] = key;
    parent->vals[parent->n++] = child;
    return 0;
}

typedef struct {
    ynode *node;
    char *pending_key; /* maps: a key waiting for its value */
} frame;

ynode *puddle_yaml_parse(const char *text, size_t len, char *err, size_t errlen)
{
    yaml_parser_t parser;
    yaml_event_t ev;
    frame stack[MAX_DEPTH];
    int depth = 0;
    ynode *root = NULL;
    int docs = 0, done = 0;
    char msg[256] = "";
    int line = 0;

    if (!yaml_parser_initialize(&parser)) {
        seterr(err, errlen, "out of memory");
        return NULL;
    }
    yaml_parser_set_input_string(&parser, (const unsigned char *)text, len);

    while (!done) {
        if (!yaml_parser_parse(&parser, &ev)) {
            line = (int)parser.problem_mark.line + 1;
            snprintf(msg, sizeof msg, "%s", parser.problem ? parser.problem : "invalid YAML");
            goto fail;
        }
        line = (int)ev.start_mark.line + 1;

        ynode *made = NULL; /* a finished scalar, or a container just opened */
        int is_container = 0;

        switch (ev.type) {
        case YAML_DOCUMENT_START_EVENT:
            if (++docs > 1) {
                snprintf(msg, sizeof msg, "only one YAML document is supported");
                yaml_event_delete(&ev);
                goto fail;
            }
            break;
        case YAML_STREAM_END_EVENT:
            done = 1;
            break;
        case YAML_ALIAS_EVENT:
            snprintf(msg, sizeof msg, "YAML aliases are not supported");
            yaml_event_delete(&ev);
            goto fail;
        case YAML_SCALAR_EVENT:
            if (ev.data.scalar.anchor) {
                snprintf(msg, sizeof msg, "YAML anchors are not supported");
                yaml_event_delete(&ev);
                goto fail;
            }
            made = node_new(Y_SCALAR, line);
            if (made) {
                made->scalar = (char *)malloc(ev.data.scalar.length + 1);
                if (made->scalar) {
                    memcpy(made->scalar, ev.data.scalar.value, ev.data.scalar.length);
                    made->scalar[ev.data.scalar.length] = '\0';
                }
                made->plain = ev.data.scalar.style == YAML_PLAIN_SCALAR_STYLE;
            }
            break;
        case YAML_MAPPING_START_EVENT:
        case YAML_SEQUENCE_START_EVENT: {
            int is_map = ev.type == YAML_MAPPING_START_EVENT;
            if ((is_map ? ev.data.mapping_start.anchor : ev.data.sequence_start.anchor)) {
                snprintf(msg, sizeof msg, "YAML anchors are not supported");
                yaml_event_delete(&ev);
                goto fail;
            }
            if (depth >= MAX_DEPTH) {
                snprintf(msg, sizeof msg, "nesting is too deep");
                yaml_event_delete(&ev);
                goto fail;
            }
            made = node_new(is_map ? Y_MAP : Y_SEQ, line);
            is_container = 1;
            break;
        }
        case YAML_MAPPING_END_EVENT:
        case YAML_SEQUENCE_END_EVENT:
            depth--; /* the node was attached when it opened */
            break;
        default:
            break;
        }
        yaml_event_delete(&ev);

        if (!made)
            continue;
        if ((made->kind == Y_SCALAR && !made->scalar)) {
            puddle_yaml_free(made);
            snprintf(msg, sizeof msg, "out of memory");
            goto fail;
        }

        /* attach `made` to its parent (or make it the root) */
        if (depth == 0) {
            if (root) {
                puddle_yaml_free(made);
                snprintf(msg, sizeof msg, "unexpected extra content");
                goto fail;
            }
            root = made;
        } else {
            frame *f = &stack[depth - 1];
            if (f->node->kind == Y_SEQ) {
                if (node_push(f->node, NULL, made) != 0) {
                    puddle_yaml_free(made);
                    snprintf(msg, sizeof msg, "out of memory");
                    goto fail;
                }
            } else if (!f->pending_key) { /* this node is a key */
                if (made->kind != Y_SCALAR) {
                    puddle_yaml_free(made);
                    snprintf(msg, sizeof msg, "map keys must be plain text");
                    goto fail;
                }
                for (size_t i = 0; i < f->node->n; i++)
                    if (strcmp(f->node->keys[i], made->scalar) == 0) {
                        snprintf(msg, sizeof msg, "duplicate key '%s'", made->scalar);
                        puddle_yaml_free(made);
                        goto fail;
                    }
                f->pending_key = made->scalar;
                made->scalar = NULL; /* the frame owns the text now */
                puddle_yaml_free(made);
                continue;
            } else {
                if (node_push(f->node, f->pending_key, made) != 0) {
                    puddle_yaml_free(made);
                    snprintf(msg, sizeof msg, "out of memory");
                    goto fail;
                }
                f->pending_key = NULL;
            }
        }
        if (is_container) {
            stack[depth].node = made;
            stack[depth].pending_key = NULL;
            depth++;
        }
    }

    yaml_parser_delete(&parser);
    if (!root)
        root = node_new(Y_MAP, 1); /* empty file */
    if (!root)
        seterr(err, errlen, "out of memory");
    return root;

fail:
    for (int i = 0; i < depth; i++)
        free(stack[i].pending_key);
    yaml_parser_delete(&parser);
    puddle_yaml_free(root);
    seterr(err, errlen, "line %d: %s", line, msg);
    return NULL;
}

ynode *puddle_yaml_parse_file(const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        seterr(err, errlen, "cannot read %s: %s", path, strerror(errno));
        return NULL;
    }
    char *buf = (char *)malloc(MAX_FILE_BYTES + 1);
    if (!buf) {
        fclose(f);
        seterr(err, errlen, "out of memory");
        return NULL;
    }
    size_t n = fread(buf, 1, MAX_FILE_BYTES + 1, f);
    fclose(f);
    if (n > MAX_FILE_BYTES) {
        free(buf);
        seterr(err, errlen, "%s is larger than 1 MiB", path);
        return NULL;
    }
    char inner[400];
    ynode *root = puddle_yaml_parse(buf, n, inner, sizeof inner);
    free(buf);
    if (!root)
        seterr(err, errlen, "%s: %s", path, inner);
    return root;
}

ykind ynode_kind(const ynode *n) { return n->kind; }
int ynode_line(const ynode *n) { return n->line; }
const char *ynode_scalar(const ynode *n) { return n->kind == Y_SCALAR ? n->scalar : NULL; }

int ynode_is_null(const ynode *n)
{
    if (n->kind != Y_SCALAR || !n->plain)
        return 0;
    return n->scalar[0] == '\0' || strcmp(n->scalar, "~") == 0 || strcmp(n->scalar, "null") == 0 ||
           strcmp(n->scalar, "Null") == 0 || strcmp(n->scalar, "NULL") == 0;
}

size_t ynode_count(const ynode *n) { return n->kind == Y_SCALAR ? 0 : n->n; }
const ynode *ynode_item(const ynode *n, size_t i) { return (n->kind != Y_SCALAR && i < n->n) ? n->vals[i] : NULL; }
const char *ynode_key(const ynode *n, size_t i) { return (n->kind == Y_MAP && i < n->n) ? n->keys[i] : NULL; }

const ynode *ynode_get(const ynode *map, const char *key)
{
    if (map->kind != Y_MAP)
        return NULL;
    for (size_t i = 0; i < map->n; i++)
        if (strcmp(map->keys[i], key) == 0)
            return map->vals[i];
    return NULL;
}
