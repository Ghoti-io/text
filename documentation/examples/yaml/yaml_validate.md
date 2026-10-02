@page example_yaml_validate YAML: validating against a JSON Schema

# YAML: validating against a JSON Schema

`examples/yaml/yaml_validate.c` is in the tree and `make examples` builds it.

This example checks four documents against one schema. The schema is written
in YAML, which is how they are normally written, and compiled with
`gtext_yaml_schema_compile()`.

## What to look at in the output

The **line and column**. Converting the document with `gtext_yaml_to_json()`
and calling `gtext_json_schema_validate()` gives the same verdict; what it
cannot give is a position in a file the user wrote, because a JSON Schema
failure names a pointer into the converted instance - a document that does not
exist as far as the author is concerned.

```
--- document ---
name: ghoti
port: not-a-number
    invalid at line 2, column 7: Value type does not match schema type

--- document ---
name: ghoti
port: 1
tags:
  - fine
  - 7
  - also fine
    invalid at line 5, column 5: Value type does not match schema type

--- document ---
port: 8080
    invalid at line 1, column 1: Required property is missing
```

Three things to notice:

- Column 7 on line 2 is where `not-a-number` begins, not where the line or the
  mapping begins.
- The sequence failure is reported at line 5, the entry, not at line 3, the
  `tags:` key - which is what reporting the parent node would give.
- `required` is about the mapping and not about the name it lacks, so there is
  no node for the missing key; the root's own position is reported instead of
  nothing.

## Why there are three statuses and not two

`GTEXT_YAML_E_SCHEMA` means the document converted and does not satisfy the
schema. Any other failure means it could not be converted to JSON at all, so
the schema was never applied - a complex mapping key, say. The two have
different fixes, and a caller that treats every non-`OK` status alike will tell
the user to correct a constraint that was never checked.

## What is demonstrated

- Compiling a schema that is itself YAML
- Freeing the schema's source document before validating anything, which is
  safe because `gtext_json_schema_compile()` clones what it needs for `$ref`
- Reporting a failure at the line and column of the YAML
- Keeping a conversion refusal distinct from a schema failure

Back to @ref yaml_examples "YAML examples".
