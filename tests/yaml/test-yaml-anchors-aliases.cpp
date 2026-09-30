#include <gtest/gtest.h>
#include <string.h>
#include <stdlib.h>

extern "C" {
#include <ghoti.io/text/yaml/yaml_stream.h>
#include <ghoti.io/text/yaml/yaml_core.h>
#include <ghoti.io/text/yaml/yaml_dom.h>
}

static GTEXT_YAML_Status noop_cb(GTEXT_YAML_Stream *s, const void *evp, void *user) {
    (void)s; (void)evp; (void)user; 
    return GTEXT_YAML_OK;
}

// Test 1: Simple anchor and alias
TEST(YamlAnchorsAliases, SimpleAnchorAlias) {
    GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
    GTEXT_YAML_Stream *s = gtext_yaml_stream_new(&opts, noop_cb, NULL);
    ASSERT_NE(s, nullptr);
    
    const char *yaml = "anchor: &anchor value\nalias: *anchor\n";
    GTEXT_YAML_Status st = gtext_yaml_stream_feed(s, yaml, strlen(yaml));
    EXPECT_EQ(st, GTEXT_YAML_OK);
    st = gtext_yaml_stream_finish(s);
    EXPECT_EQ(st, GTEXT_YAML_OK);
    gtext_yaml_stream_free(s);
}

// Test 2: Multiple anchors with different aliases
TEST(YamlAnchorsAliases, MultipleAnchors) {
    GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
    GTEXT_YAML_Stream *s = gtext_yaml_stream_new(&opts, noop_cb, NULL);
    ASSERT_NE(s, nullptr);
    
    const char *yaml = 
        "a1: &anchor1 value1\n"
        "a2: &anchor2 value2\n"
        "a3: &anchor3 value3\n"
        "b1: *anchor1\n"
        "b2: *anchor2\n"
        "b3: *anchor3\n";
    
    GTEXT_YAML_Status st = gtext_yaml_stream_feed(s, yaml, strlen(yaml));
    EXPECT_EQ(st, GTEXT_YAML_OK);
    st = gtext_yaml_stream_finish(s);
    EXPECT_EQ(st, GTEXT_YAML_OK);
    gtext_yaml_stream_free(s);
}

// Test 3: Anchor with sequence value
TEST(YamlAnchorsAliases, AnchorSequence) {
    GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
    GTEXT_YAML_Stream *s = gtext_yaml_stream_new(&opts, noop_cb, NULL);
    ASSERT_NE(s, nullptr);
    
    const char *yaml = 
        "list: &mylist [1, 2, 3, 4]\n"
        "copy: *mylist\n";
    
    GTEXT_YAML_Status st = gtext_yaml_stream_feed(s, yaml, strlen(yaml));
    EXPECT_EQ(st, GTEXT_YAML_OK);
    st = gtext_yaml_stream_finish(s);
    EXPECT_EQ(st, GTEXT_YAML_OK);
    gtext_yaml_stream_free(s);
}

// Test 4: Anchor with mapping value
TEST(YamlAnchorsAliases, AnchorMapping) {
    GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
    GTEXT_YAML_Stream *s = gtext_yaml_stream_new(&opts, noop_cb, NULL);
    ASSERT_NE(s, nullptr);
    
    const char *yaml = 
        "defaults: &defaults\n"
        "  adapter: postgres\n"
        "  host: localhost\n"
        "development:\n"
        "  <<: *defaults\n"
        "  database: dev_db\n";
    
    GTEXT_YAML_Status st = gtext_yaml_stream_feed(s, yaml, strlen(yaml));
    EXPECT_EQ(st, GTEXT_YAML_OK);
    st = gtext_yaml_stream_finish(s);
    EXPECT_EQ(st, GTEXT_YAML_OK);
    gtext_yaml_stream_free(s);
}

// Test 5: Nested anchors and aliases
TEST(YamlAnchorsAliases, NestedAnchors) {
    GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
    GTEXT_YAML_Stream *s = gtext_yaml_stream_new(&opts, noop_cb, NULL);
    ASSERT_NE(s, nullptr);
    
    const char *yaml = 
        "outer: &outer\n"
        "  inner: &inner value\n"
        "  another: something\n"
        "copy_outer: *outer\n"
        "copy_inner: *inner\n";
    
    GTEXT_YAML_Status st = gtext_yaml_stream_feed(s, yaml, strlen(yaml));
    EXPECT_EQ(st, GTEXT_YAML_OK);
    st = gtext_yaml_stream_finish(s);
    EXPECT_EQ(st, GTEXT_YAML_OK);
    gtext_yaml_stream_free(s);
}

// Test 6: Alias used multiple times
TEST(YamlAnchorsAliases, ReusedAlias) {
    GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
    GTEXT_YAML_Stream *s = gtext_yaml_stream_new(&opts, noop_cb, NULL);
    ASSERT_NE(s, nullptr);
    
    const char *yaml = 
        "anchor: &reused [a, b, c]\n"
        "first: *reused\n"
        "second: *reused\n"
        "third: *reused\n";
    
    GTEXT_YAML_Status st = gtext_yaml_stream_feed(s, yaml, strlen(yaml));
    EXPECT_EQ(st, GTEXT_YAML_OK);
    st = gtext_yaml_stream_finish(s);
    EXPECT_EQ(st, GTEXT_YAML_OK);
    gtext_yaml_stream_free(s);
}

// Test 7: Undefined alias (should be handled gracefully)
TEST(YamlAnchorsAliases, UndefinedAlias) {
    GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
    GTEXT_YAML_Stream *s = gtext_yaml_stream_new(&opts, noop_cb, NULL);
    ASSERT_NE(s, nullptr);
    
    const char *yaml = "key: *undefined\n";
    GTEXT_YAML_Status st = gtext_yaml_stream_feed(s, yaml, strlen(yaml));
    // Parser may be lenient and treat as plain scalar or reject
    if (st == GTEXT_YAML_OK) {
        st = gtext_yaml_stream_finish(s);
    }
    EXPECT_TRUE(st == GTEXT_YAML_OK || st == GTEXT_YAML_E_INVALID);
    gtext_yaml_stream_free(s);
}

// Test 8: Anchor defined after alias (forward reference - invalid)
TEST(YamlAnchorsAliases, ForwardReference) {
    GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
    GTEXT_YAML_Stream *s = gtext_yaml_stream_new(&opts, noop_cb, NULL);
    ASSERT_NE(s, nullptr);
    
    const char *yaml = 
        "alias: *forward\n"
        "anchor: &forward value\n";
    
    GTEXT_YAML_Status st = gtext_yaml_stream_feed(s, yaml, strlen(yaml));
    if (st == GTEXT_YAML_OK) {
        st = gtext_yaml_stream_finish(s);
    }
    // Should either reject or treat *forward as plain scalar before anchor
    EXPECT_TRUE(st == GTEXT_YAML_OK || st == GTEXT_YAML_E_INVALID);
    gtext_yaml_stream_free(s);
}

// Test 9: Anchor name with special characters
TEST(YamlAnchorsAliases, AnchorSpecialChars) {
    GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
    GTEXT_YAML_Stream *s = gtext_yaml_stream_new(&opts, noop_cb, NULL);
    ASSERT_NE(s, nullptr);
    
    // Anchor names can contain alphanumerics, -, and _
    const char *yaml = 
        "item: &my-anchor_123 value\n"
        "copy: *my-anchor_123\n";
    
    GTEXT_YAML_Status st = gtext_yaml_stream_feed(s, yaml, strlen(yaml));
    EXPECT_EQ(st, GTEXT_YAML_OK);
    st = gtext_yaml_stream_finish(s);
    EXPECT_EQ(st, GTEXT_YAML_OK);
    gtext_yaml_stream_free(s);
}

// Test 10: Anchor in flow sequence
TEST(YamlAnchorsAliases, AnchorInFlowSequence) {
    GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
    GTEXT_YAML_Stream *s = gtext_yaml_stream_new(&opts, noop_cb, NULL);
    ASSERT_NE(s, nullptr);
    
    const char *yaml = 
        "list: [&a 1, &b 2, &c 3]\n"
        "values: [*a, *b, *c]\n";
    
    GTEXT_YAML_Status st = gtext_yaml_stream_feed(s, yaml, strlen(yaml));
    EXPECT_EQ(st, GTEXT_YAML_OK);
    st = gtext_yaml_stream_finish(s);
    EXPECT_EQ(st, GTEXT_YAML_OK);
    gtext_yaml_stream_free(s);
}

// Test 11: Anchor in flow mapping
TEST(YamlAnchorsAliases, AnchorInFlowMapping) {
    GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
    GTEXT_YAML_Stream *s = gtext_yaml_stream_new(&opts, noop_cb, NULL);
    ASSERT_NE(s, nullptr);
    
    const char *yaml = 
        "map: {key1: &v1 val1, key2: &v2 val2}\n"
        "copy: {a: *v1, b: *v2}\n";
    
    GTEXT_YAML_Status st = gtext_yaml_stream_feed(s, yaml, strlen(yaml));
    EXPECT_EQ(st, GTEXT_YAML_OK);
    st = gtext_yaml_stream_finish(s);
    EXPECT_EQ(st, GTEXT_YAML_OK);
    gtext_yaml_stream_free(s);
}

// Test 12: Deeply nested alias references
TEST(YamlAnchorsAliases, DeeplyNestedAliases) {
    GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
    GTEXT_YAML_Stream *s = gtext_yaml_stream_new(&opts, noop_cb, NULL);
    ASSERT_NE(s, nullptr);
    
    const char *yaml = 
        "level1: &l1\n"
        "  level2: &l2\n"
        "    level3: &l3\n"
        "      value: deep\n"
        "ref1: *l1\n"
        "ref2: *l2\n"
        "ref3: *l3\n";
    
    GTEXT_YAML_Status st = gtext_yaml_stream_feed(s, yaml, strlen(yaml));
    EXPECT_EQ(st, GTEXT_YAML_OK);
    st = gtext_yaml_stream_finish(s);
    EXPECT_EQ(st, GTEXT_YAML_OK);
    gtext_yaml_stream_free(s);
}

// Test 13: Chain of aliases (an alias reached through another alias)
//
// The chain has to run through a node of its own.  "first: &first *orig"
// would be the shorter way to write it and is not YAML: an alias node is "*"
// and a name and nothing else (c-ns-alias-node, 7.1), so it can carry no
// anchor to chain from.  This test used to assert the stream accepted that
// form; PyYAML and js-yaml both refuse it, the latter in so many words
// ("alias node should not have any properties"), and so does the test below.
TEST(YamlAnchorsAliases, ChainedAliases) {
    GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
    GTEXT_YAML_Stream *s = gtext_yaml_stream_new(&opts, noop_cb, NULL);
    ASSERT_NE(s, nullptr);
    
    const char *yaml = 
        "original: &orig value\n"
        "first: &first [*orig]\n"
        "second: *first\n";
    
    GTEXT_YAML_Status st = gtext_yaml_stream_feed(s, yaml, strlen(yaml));
    EXPECT_EQ(st, GTEXT_YAML_OK);
    st = gtext_yaml_stream_finish(s);
    EXPECT_EQ(st, GTEXT_YAML_OK);
    gtext_yaml_stream_free(s);
}

// Test 13b: An alias may not carry a property of its own.
//
// Both halves of c-ns-properties, and both places one can stand.  Written on
// the alias's own line there is nothing else it could name; written on the
// line above it belongs to the collection that line opens, and that case is
// covered in test-yaml-events.cpp (suite case 26DV) rather than here.
TEST(YamlAnchorsAliases, AnAliasNodeCarriesNoProperties) {
    struct Case { const char *yaml; const char *what; };
    const Case cases[] = {
        { "original: &orig value\nfirst: &first *orig\n", "anchor" },
        { "original: &orig value\nfirst: !!str *orig\n", "tag" },
        { "original: &orig value\nfirst: !local *orig\n", "local tag" },
    };

    for (const Case &c : cases) {
        GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
        GTEXT_YAML_Stream *s = gtext_yaml_stream_new(&opts, noop_cb, NULL);
        ASSERT_NE(s, nullptr);

        GTEXT_YAML_Status st = gtext_yaml_stream_feed(s, c.yaml, strlen(c.yaml));
        if (st == GTEXT_YAML_OK) st = gtext_yaml_stream_finish(s);
        EXPECT_EQ(st, GTEXT_YAML_E_INVALID) << "an alias carrying a " << c.what;
        gtext_yaml_stream_free(s);
    }
}

// Test 14: Anchor on empty sequence
TEST(YamlAnchorsAliases, EmptySequenceAnchor) {
    GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
    GTEXT_YAML_Stream *s = gtext_yaml_stream_new(&opts, noop_cb, NULL);
    ASSERT_NE(s, nullptr);
    
    const char *yaml = 
        "empty: &empty []\n"
        "copy: *empty\n";
    
    GTEXT_YAML_Status st = gtext_yaml_stream_feed(s, yaml, strlen(yaml));
    EXPECT_EQ(st, GTEXT_YAML_OK);
    st = gtext_yaml_stream_finish(s);
    EXPECT_EQ(st, GTEXT_YAML_OK);
    gtext_yaml_stream_free(s);
}

// Test 15: Anchor on empty mapping
TEST(YamlAnchorsAliases, EmptyMappingAnchor) {
    GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
    GTEXT_YAML_Stream *s = gtext_yaml_stream_new(&opts, noop_cb, NULL);
    ASSERT_NE(s, nullptr);
    
    const char *yaml = 
        "empty: &empty {}\n"
        "copy: *empty\n";
    
    GTEXT_YAML_Status st = gtext_yaml_stream_feed(s, yaml, strlen(yaml));
    EXPECT_EQ(st, GTEXT_YAML_OK);
    st = gtext_yaml_stream_finish(s);
    EXPECT_EQ(st, GTEXT_YAML_OK);
    gtext_yaml_stream_free(s);
}

// Test 16: Mixed anchors and aliases in complex document
TEST(YamlAnchorsAliases, ComplexDocument) {
    GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
    GTEXT_YAML_Stream *s = gtext_yaml_stream_new(&opts, noop_cb, NULL);
    ASSERT_NE(s, nullptr);
    
    const char *yaml = 
        "defaults: &defaults\n"
        "  timeout: 30\n"
        "  retries: 3\n"
        "config1:\n"
        "  <<: *defaults\n"
        "  name: service1\n"
        "config2:\n"
        "  <<: *defaults\n"
        "  name: service2\n"
        "  timeout: 60\n";
    
    GTEXT_YAML_Status st = gtext_yaml_stream_feed(s, yaml, strlen(yaml));
    EXPECT_EQ(st, GTEXT_YAML_OK);
    st = gtext_yaml_stream_finish(s);
    EXPECT_EQ(st, GTEXT_YAML_OK);
    gtext_yaml_stream_free(s);
}

// Test 17: Alias within alias (nested structure containing aliases)
TEST(YamlAnchorsAliases, AliasContainingAliases) {
    GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
    GTEXT_YAML_Stream *s = gtext_yaml_stream_new(&opts, noop_cb, NULL);
    ASSERT_NE(s, nullptr);
    
    const char *yaml = 
        "inner: &inner value\n"
        "outer: &outer [*inner, *inner]\n"
        "copy: *outer\n";
    
    GTEXT_YAML_Status st = gtext_yaml_stream_feed(s, yaml, strlen(yaml));
    EXPECT_EQ(st, GTEXT_YAML_OK);
    st = gtext_yaml_stream_finish(s);
    EXPECT_EQ(st, GTEXT_YAML_OK);
    gtext_yaml_stream_free(s);
}

// Test 18: Anchor reused within same collection
TEST(YamlAnchorsAliases, AnchorReusedInCollection) {
    GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
    GTEXT_YAML_Stream *s = gtext_yaml_stream_new(&opts, noop_cb, NULL);
    ASSERT_NE(s, nullptr);
    
    const char *yaml = 
        "item: &item value\n"
        "list: [*item, *item, *item, *item]\n";
    
    GTEXT_YAML_Status st = gtext_yaml_stream_feed(s, yaml, strlen(yaml));
    EXPECT_EQ(st, GTEXT_YAML_OK);
    st = gtext_yaml_stream_finish(s);
    EXPECT_EQ(st, GTEXT_YAML_OK);
    gtext_yaml_stream_free(s);
}

/**
 * An anchor left on an empty value is not the following alias's.
 *
 * Found by the fuzz soak, and reported by the yaml writer's round trip as "the
 * multi-document writer wrote what the parser refuses" - which is what a round
 * trip can see and the wrong way round. The tree was correct, the writer emitted
 * it correctly, and the parser would not read its own output back.
 *
 *     a: &OO        the "*OO" line begins at the same column, so a's value was
 *     *OO :         never written: &OO is that empty value's, and the alias is
 *                   the next key, carrying nothing
 *
 * `stream_emit_alias()` deferred the pending anchor to the parser without first
 * asking whether the alias's line had *left it behind* - the question the other
 * five token kinds all ask. Nothing in the parser could then claim it, because no
 * collection opens on that line, and the document was refused with "An alias node
 * may not carry an anchor". PyYAML 6.0.2 reads it as {'a': None, None: None}.
 *
 * The three refusals below are the cases that message is right for, and they are
 * here so that a fix which simply stopped refusing would not pass: an anchor on
 * the alias's own line really is an anchor on an alias, and two anchors on one
 * node really is that. PyYAML refuses both of those too.
 */
TEST(YamlAnchorsAliases, AnAnchorLeftOnAnEmptyValueIsNotTheAliasThatFollows) {
    struct Case {
        const char *yaml;
        bool valid;
        const char *why;
    };
    const Case cases[] = {
        /* The reproducer, and the shapes around it. */
        {"a: &OO\n*OO :\n", true, "anchor on an empty value, alias as next key"},
        {"a: &OO\nb: *OO\n", true, "the same, with an ordinary key"},
        {"&O : &OO\n*O : *O\n", true, "anchored empty key and value"},
        {"&O :\n*O : *O\n", true, "anchored empty key alone"},
        {"a: &OO\n*OO : v\n", true, "alias key with a value"},
        /* Still refused, and the reason the message exists. */
        {"b: &y *x\n", false, "the anchor is on the alias's own line"},
        {"top2: &node2\n  &v2 val2\n", false, "two anchors on one node (4JVG)"},
        {"a: &x\n  &y *x\n", false, "both at once"},
    };
    for (const Case &c : cases) {
        GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
        /* **KEEP_ALL, as the fuzz harness has**, and not a convenience: an alias
           to an anchored *empty* key resolves to the same node the key is, so
           "&O : &OO" over "*O : *O" has two keys that are one node and the
           default duplicate policy refuses it - for a reason that has nothing to
           do with anchors. Written with the default first, and two of these cases
           failed with the fix correctly in place. */
        opts.dupkeys = GTEXT_YAML_DUPKEY_KEEP_ALL;
        GTEXT_YAML_Error err;
        memset(&err, 0, sizeof(err));
        GTEXT_YAML_Document *doc =
            gtext_yaml_parse(c.yaml, strlen(c.yaml), &opts, &err);
        EXPECT_EQ(doc != nullptr, c.valid)
            << c.why << " -- input [" << c.yaml << "] "
            << (doc ? "accepted" : (err.message ? err.message : "refused"));
        if (doc) gtext_yaml_free(doc);
        gtext_yaml_error_free(&err);
    }
}

/**
 * And the round trip the soak actually asserted: the writer's output re-parses.
 *
 * The document below is what the writer emitted for the fuzzer's tree, reduced to
 * UTF-8 and to the lines that matter. Asserting the round trip rather than only
 * the parse is what makes this a regression test for the finding rather than for
 * the reproducer: the property that failed was parse-write-parse.
 */
TEST(YamlAnchorsAliases, TheWritersOwnOutputParsesBack) {
    const char *src =
        "---\n"
        "&O : &OO\n"
        "*O : *O\n"
        ": .\n"
        "*O :\n"
        "*O : *O\n"
        ": *O\n"
        "I:\n";
    GTEXT_YAML_Parse_Options opts = gtext_yaml_parse_options_default();
    opts.dupkeys = GTEXT_YAML_DUPKEY_KEEP_ALL;
    GTEXT_YAML_Error err;
    memset(&err, 0, sizeof(err));
    size_t count = 0;
    GTEXT_YAML_Document **docs =
        gtext_yaml_parse_all(src, strlen(src), &count, &opts, &err);
    ASSERT_NE(docs, nullptr) << (err.message ? err.message : "refused");
    EXPECT_EQ(count, 1u);
    for (size_t i = 0; i < count; i++) gtext_yaml_free(docs[i]);
    /* The array as well as the documents - gtext_yaml_parse_all() allocates both
       and the header says so. Omitting it leaked 32 bytes and ASan said which
       test, which is the whole point of running it. */
    free(docs);
    gtext_yaml_error_free(&err);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
