#include "core/yamltree.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static ynode *parse(const char *text, char *err, size_t n) { return puddle_yaml_parse(text, strlen(text), err, n); }

int main(void)
{
    char err[256];

    /* the shapes we use: scalars, quotes, comments, lists of maps, block text, flow lists */
    ynode *r = parse(
        "# comment\n"
        "address: \"::1\"   # trailing comment\n"
        "port: 8443\n"
        "empty:\n"
        "tilde: ~\n"
        "quoted_empty: \"\"\n"
        "profiles:\n"
        "  - id: work\n"
        "    about: |\n"
        "      line one\n"
        "      line two\n"
        "    color:\n"
        "      gradient: [\"#ff0000\", \"#00ff00\"]\n"
        "  - id: home\n", err, sizeof err);
    CHECK(r != NULL);
    CHECK(ynode_kind(r) == Y_MAP && ynode_count(r) == 6);
    CHECK(strcmp(ynode_scalar(ynode_get(r, "address")), "::1") == 0);
    CHECK(strcmp(ynode_scalar(ynode_get(r, "port")), "8443") == 0);
    CHECK(ynode_is_null(ynode_get(r, "empty")) && ynode_is_null(ynode_get(r, "tilde")));
    CHECK(!ynode_is_null(ynode_get(r, "quoted_empty")));
    CHECK(ynode_get(r, "missing") == NULL);
    const ynode *list = ynode_get(r, "profiles");
    CHECK(ynode_kind(list) == Y_SEQ && ynode_count(list) == 2);
    const ynode *work = ynode_item(list, 0);
    CHECK(strcmp(ynode_scalar(ynode_get(work, "id")), "work") == 0);
    CHECK(strcmp(ynode_scalar(ynode_get(work, "about")), "line one\nline two\n") == 0);
    const ynode *grad = ynode_get(ynode_get(work, "color"), "gradient");
    CHECK(ynode_kind(grad) == Y_SEQ && ynode_count(grad) == 2);
    CHECK(strcmp(ynode_scalar(ynode_item(grad, 1)), "#00ff00") == 0);
    CHECK(ynode_line(ynode_get(work, "about")) == 9);
    CHECK(strcmp(ynode_key(r, 0), "address") == 0);
    puddle_yaml_free(r);

    /* IPv6 needs quotes in some forms: "[::1]" and "::" are syntax errors, "::1" happens to be fine */
    CHECK(parse("address: [::1]\n", err, sizeof err) == NULL && strstr(err, "line 1"));
    CHECK(parse("address: ::\n", err, sizeof err) == NULL);
    r = parse("a: ::1\nb: \"::\"\nc: [x, y]\n", err, sizeof err);
    CHECK(r && strcmp(ynode_scalar(ynode_get(r, "a")), "::1") == 0 && strcmp(ynode_scalar(ynode_get(r, "b")), "::") == 0);
    CHECK(ynode_kind(ynode_get(r, "c")) == Y_SEQ);
    puddle_yaml_free(r);

    /* empty files are fine */
    r = parse("", err, sizeof err);
    CHECK(r && ynode_kind(r) == Y_MAP && ynode_count(r) == 0);
    puddle_yaml_free(r);
    r = parse("# only a comment\n", err, sizeof err);
    CHECK(r && ynode_count(r) == 0);
    puddle_yaml_free(r);

    /* things we refuse, with line numbers */
    CHECK(parse("a: 1\nb: 2\na: 3\n", err, sizeof err) == NULL && strstr(err, "line 3") && strstr(err, "duplicate key 'a'"));
    CHECK(parse("a: &x 1\nb: *x\n", err, sizeof err) == NULL && strstr(err, "anchors"));
    CHECK(parse("a: *x\n", err, sizeof err) == NULL && strstr(err, "aliases"));
    CHECK(parse("a: 1\n---\nb: 2\n", err, sizeof err) == NULL && strstr(err, "one YAML document"));
    CHECK(parse("a: [1, 2\nb: 3\n", err, sizeof err) == NULL && strstr(err, "line "));
    CHECK(parse("a:\n\t- x\n", err, sizeof err) == NULL); /* tabs */
    CHECK(parse("? [a, b]\n: c\n", err, sizeof err) == NULL && strstr(err, "keys"));

    /* nesting limit */
    char deep[200] = "";
    for (int i = 0; i < 40; i++) strcat(deep, "[");
    CHECK(parse(deep, err, sizeof err) == NULL);

    CHECK(puddle_yaml_parse_file("/nonexistent/file.yaml", err, sizeof err) == NULL && strstr(err, "cannot read"));

    return failures ? 1 : 0;
}
