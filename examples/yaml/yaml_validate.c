/**
 * @file
 *
 * Example: validate a YAML document against a JSON Schema, and point at the
 * line that is wrong.
 *
 * There is no YAML schema language in use - YAML 1.2's own "schemas" are about
 * implicit typing, and Kwalify and Rx never caught on - so this validates
 * against JSON Schema, which is what Kubernetes, OpenAPI and GitHub Actions
 * describe YAML with. The schema here is itself written in YAML, because that
 * is how they are usually written.
 *
 * The thing to look at in the output is the **line and column**. Converting to
 * JSON and calling the JSON Schema validator gives the same verdict; what it
 * cannot give is a position in a file the user actually wrote, because a JSON
 * Schema failure names a place in the converted document.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/text/json.h>
#include <ghoti.io/text/yaml.h>
#include <stdio.h>
#include <string.h>

static const char * kSchema =
    "type: object\n"
    "required: [name, port]\n"
    "properties:\n"
    "  name:\n"
    "    type: string\n"
    "    minLength: 1\n"
    "  port:\n"
    "    type: integer\n"
    "    minimum: 1\n"
    "    maximum: 65535\n"
    "  tags:\n"
    "    type: array\n"
    "    items:\n"
    "      type: string\n";

/* Four documents: one that passes and three that fail in different places, so
   that the position in each report can be checked against the text above it. */
static const char * kDocuments[] = {
    "name: ghoti\n"
    "port: 8080\n"
    "tags: [http, public]\n",

    "name: ghoti\n"
    "port: not-a-number\n",

    "name: ghoti\n"
    "port: 1\n"
    "tags:\n"
    "  - fine\n"
    "  - 7\n"
    "  - also fine\n",

    "port: 8080\n",
};

static void report(const char * text, GTEXT_YAML_Status status,
    const GTEXT_YAML_Error * err) {
  printf("--- document ---\n%s", text);
  if (status == GTEXT_YAML_OK) {
    printf("    valid\n\n");
    return;
  }
  if (status == GTEXT_YAML_E_SCHEMA) {
    /* The line and column are of the YAML, not of the converted JSON. A
       line of 0 means there was nothing to point at - `required` on the root
       of a document parsed without source locations, for instance. */
    printf("    invalid at line %d, column %d: %s\n\n", err->line, err->col,
        err->message ? err->message : "(no message)");
    return;
  }
  /* Not a schema failure: the document could not be expressed as JSON at all,
     so nothing has been said about the schema. Reporting the two alike is the
     mistake this status exists to prevent. */
  printf("    could not be converted: %s\n\n",
      err->message ? err->message : "(no message)");
}

int main(void) {
  GTEXT_YAML_Error err;
  memset(&err, 0, sizeof(err));

  GTEXT_YAML_Document * schema_doc =
      gtext_yaml_parse(kSchema, strlen(kSchema), NULL, &err);
  if (!schema_doc) {
    fprintf(stderr, "the schema is not valid YAML: %s\n",
        err.message ? err.message : "?");
    gtext_yaml_error_free(&err);
    return 1;
  }
  gtext_yaml_error_free(&err);

  memset(&err, 0, sizeof(err));
  GTEXT_JSON_Schema * schema =
      gtext_yaml_schema_compile(schema_doc, NULL, &err);
  if (!schema) {
    fprintf(stderr, "the schema does not compile: %s\n",
        err.message ? err.message : "?");
    gtext_yaml_error_free(&err);
    gtext_yaml_free(schema_doc);
    return 1;
  }
  gtext_yaml_error_free(&err);

  /* The compiled schema is independent of both documents it came from:
     gtext_json_schema_compile() clones what it needs for `$ref`. So the YAML
     can go now, before anything is validated. */
  gtext_yaml_free(schema_doc);

  printf("=== schema ===\n%s\n", kSchema);

  int failures = 0;
  for (size_t i = 0; i < sizeof(kDocuments) / sizeof(kDocuments[0]); i++) {
    memset(&err, 0, sizeof(err));
    GTEXT_YAML_Document * doc =
        gtext_yaml_parse(kDocuments[i], strlen(kDocuments[i]), NULL, &err);
    if (!doc) {
      printf("--- document ---\n%s    not valid YAML: %s\n\n", kDocuments[i],
          err.message ? err.message : "?");
      gtext_yaml_error_free(&err);
      failures++;
      continue;
    }
    gtext_yaml_error_free(&err);

    memset(&err, 0, sizeof(err));
    const GTEXT_YAML_Status status =
        gtext_yaml_validate(doc, schema, NULL, &err);
    report(kDocuments[i], status, &err);
    if (status != GTEXT_YAML_OK) {
      failures++;
    }
    gtext_yaml_error_free(&err);
    gtext_yaml_free(doc);
  }

  gtext_json_schema_free(schema);
  printf("%d of 4 documents failed, which is what this example expects.\n",
      failures);
  return 0;
}
