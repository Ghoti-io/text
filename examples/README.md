# Text Module Examples

This directory contains example programs demonstrating how to use the text module APIs.

## YAML Examples

### yaml_writer_formatting.c
Writer formatting options for YAML. Demonstrates:
- Writing a DOM to a sink
- Scalar style selection and line-width folding
- Custom indentation

### yaml_validate.c
Validating a YAML document against a JSON Schema. Demonstrates:
- Compiling a schema that is itself written in YAML
- A failure reported at the line and column of the YAML, not at a JSON Pointer
  into the converted instance
- Keeping "this cannot be expressed as JSON" distinct from "this does not
  satisfy the schema"

## JSON Examples

### json_basic.c
Basic JSON parsing and writing example. Demonstrates:
- Parsing JSON from a string
- Accessing values in the DOM
- Writing JSON to a buffer
- Error handling

### json_create.c
Creating JSON values programmatically. Demonstrates:
- Creating JSON values from scratch
- Building arrays and objects
- Mutating the DOM

### json_stream.c
Streaming parser example. Demonstrates:
- Using the streaming parser for incremental parsing
- Handling events from the streaming parser
- Processing large JSON documents without building a full DOM

### json_pointer.c
JSON Pointer (RFC 6901) example. Demonstrates:
- Using JSON Pointers to access nested values
- Reading and modifying values via pointers

### json_patch.c
JSON Patch (RFC 6902) and Merge Patch (RFC 7386) example. Demonstrates:
- Applying JSON Patch operations
- Applying JSON Merge Patch

### json_schema.c
JSON Schema validation example. Demonstrates:
- Compiling a JSON Schema
- Validating JSON values against a schema
- Handling validation errors

## Building Examples

`make examples` builds every example into `build/linux/release/apps/examples/`.
On Linux, run one with `LD_LIBRARY_PATH=build/linux/release/apps`.

Against an installed library:

```bash
cc -o json_basic examples/json/json_basic.c $(pkg-config --cflags --libs ghoti.io-text-0)
```

A line that only adds `-I include` cannot see cutil, chron or unicode.

## Requirements

- C17-compatible compiler (gcc, clang)
- The ghoti.io-text library, and cutil, chron and unicode, on `PKG_CONFIG_PATH`
