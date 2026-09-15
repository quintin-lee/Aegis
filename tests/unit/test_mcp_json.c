/**
 * @file test_mcp_json.c
 * @brief Unit tests for the aegis_json value DOM parser.
 */
#include "mcp_json.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static aegis_json_value_t* parse(const char* text)
{
    aegis_json_value_t* v = NULL;
    assert(aegis_json_parse(text, &v) == AEGIS_OK);
    return v;
}

static void test_scalar_object(void)
{
    aegis_json_value_t* v = parse("{\"a\":1,\"b\":\"x\",\"c\":[true,null,2.5]}");
    assert(aegis_json_object_get(v, "a")->type == AEGIS_JSON_INT);
    assert(aegis_json_int(aegis_json_object_get(v, "a")) == 1);
    assert(strcmp(aegis_json_string(aegis_json_object_get(v, "b")), "x") == 0);
    const aegis_json_value_t* c = aegis_json_object_get(v, "c");
    assert(aegis_json_array_len(c) == 3);
    assert(aegis_json_array_at(c, 0)->type == AEGIS_JSON_BOOL);
    assert(aegis_json_bool(aegis_json_array_at(c, 0)));
    assert(aegis_json_array_at(c, 1)->type == AEGIS_JSON_NULL);
    assert(aegis_json_array_at(c, 2)->type == AEGIS_JSON_FLOAT);
    assert(aegis_json_float(aegis_json_array_at(c, 2)) == 2.5);
    aegis_json_value_destroy(v);
    printf("scalar_object PASS\n");
}

static void test_nested_object_array(void)
{
    const char* text =
        "{\"tools\":[{\"name\":\"echo\",\"inputSchema\":{\"type\":\"object\"}},null]}";
    aegis_json_value_t*       v     = parse(text);
    const aegis_json_value_t* tools = aegis_json_object_get(v, "tools");
    assert(aegis_json_array_len(tools) == 2);
    const aegis_json_value_t* first = aegis_json_array_at(tools, 0);
    assert(strcmp(aegis_json_string(aegis_json_object_get(first, "name")), "echo") == 0);
    aegis_json_value_destroy(v);
    printf("nested_object_array PASS\n");
}

static void test_escapes(void)
{
    aegis_json_value_t* v = parse("\"line1\\nline2\\t\\\"quoted\\\"\"");
    assert(strcmp(aegis_json_string(v), "line1\nline2\t\"quoted\"") == 0);
    aegis_json_value_destroy(v);
    printf("escapes PASS\n");
}

static void test_corrupt_input(void)
{
    aegis_json_value_t* v = NULL;
    assert(aegis_json_parse("{\"a\":}", &v) == AEGIS_ERR_INVALID);
    assert(v == NULL);
    assert(aegis_json_parse("[1,2]", &v) == AEGIS_OK);
    aegis_json_value_destroy(v);
    assert(aegis_json_parse("{\"a\":1}garbage", &v) == AEGIS_ERR_INVALID);
    assert(aegis_json_parse("", &v) == AEGIS_ERR_INVALID);
    printf("corrupt_input PASS\n");
}

static void test_empty_collections(void)
{
    aegis_json_value_t* v = parse("{}");
    assert(v->type == AEGIS_JSON_OBJECT && aegis_json_object_get(v, "x") == NULL);
    aegis_json_value_destroy(v);
    v = parse("[]");
    assert(v->type == AEGIS_JSON_ARRAY && aegis_json_array_len(v) == 0);
    aegis_json_value_destroy(v);
    printf("empty_collections PASS\n");
}

int main(void)
{
    test_scalar_object();
    test_nested_object_array();
    test_escapes();
    test_corrupt_input();
    test_empty_collections();
    printf("All mcp_json tests PASS\n");
    return 0;
}
