@page yaml_examples YAML: examples

# YAML: examples

This page provides practical examples demonstrating the YAML module's streaming parser API.

## Available Examples

### Getting Started

- @subpage example_yaml_streaming_basic "yaml_streaming_basic.c" — Basic streaming YAML parser
  - Event-driven parsing with callbacks
  - Handling chunk boundaries
  - Event types and source locations
  - **Start here** if you're new to the YAML module

### Security and Validation

- @subpage example_yaml_security "yaml_security.c" — Security limits and validation
  - Depth, byte, and alias expansion limits
  - Preventing decompression bomb attacks
  - Error handling for limit violations
  - **Essential reading** for parsing untrusted input

### Practical Applications

- @subpage example_yaml_config_parser "yaml_config_parser.c" — Configuration file parser
  - Building structured data from events
  - Stateful event processing
  - Nested sections and key-value extraction
  - Real-world config file patterns

### Writer Output

- @subpage example_yaml_writer_formatting "yaml_writer_formatting.c" — Writer formatting options
    - Pretty/block output with custom indentation
    - Scalar style selection and line-width folding
    - Buffer sink output

### Validation

- @subpage example_yaml_validate "yaml_validate.c" — Validating against a JSON Schema
    - A schema written in YAML, compiled with gtext_yaml_schema_compile()
    - A failure reported at the line and column of the YAML, not at a pointer
      into the converted instance
    - A conversion refusal kept distinct from a schema failure

## Example Status

**Current Status (February 2026):**
- ✅ Streaming parser examples available
- ⏳ DOM parser examples (planned - awaiting DOM-specific examples)
- ✅ Writer examples available
- ✅ Schema validation example available

The examples focus on the **streaming parser API**, which is currently implemented. DOM and writer examples will be added as those features are completed.

## Common Patterns

### Basic Parser Setup

```c
#include <ghoti.io/text/yaml.h>

GTEXT_YAML_Status my_callback(
    GTEXT_YAML_Stream *s,
    const void *event_payload,
    void *user
) {
    const GTEXT_YAML_Event *ev = event_payload;
    // Process event...
    return GTEXT_YAML_OK;
}

int main(void) {
    GTEXT_YAML_Stream *parser = gtext_yaml_stream_new(
        NULL,        // NULL = default options
        my_callback,
        NULL         // user context
    );
    
    gtext_yaml_stream_feed(parser, yaml_input, input_len);
    gtext_yaml_stream_finish(parser);
    gtext_yaml_stream_free(parser);
    
    return 0;
}
```

### Security Limits (Essential for Untrusted Input)

```c
GTEXT_YAML_Parse_Options opts = {0};
opts.max_depth = 32;                    // Max nesting
opts.max_total_bytes = 1024 * 1024;     // 1 MB
opts.max_alias_expansion = 10000;       // Prevent bombs

GTEXT_YAML_Stream *parser = gtext_yaml_stream_new(&opts, cb, NULL);
```

### Event Processing

```c
switch (ev->type) {
    case GTEXT_YAML_EVENT_SCALAR:
        printf("Scalar: %.*s\n", 
               (int)ev->data.scalar.len, 
               ev->data.scalar.ptr);
        break;
        
    case GTEXT_YAML_EVENT_INDICATOR:
        printf("Indicator: %c\n", ev->data.indicator);
        break;
        
    case GTEXT_YAML_EVENT_MAPPING_START:
        printf("Mapping start\n");
        break;
        
    // ... handle other event types
}
```

## Learning Path

1. **Start with basics**: [yaml_streaming_basic.c](@ref example_yaml_streaming_basic)
   - Understand event-driven parsing
   - Learn event types and structure
   
2. **Add security**: [yaml_security.c](@ref example_yaml_security)
   - Learn about security threats
   - Configure appropriate limits
   - Handle limit violations
   
3. **Build applications**: [yaml_config_parser.c](@ref example_yaml_config_parser)
   - Convert events to data structures
   - Implement stateful processing
   - Handle nested sections

## Compiling Examples

To compile an example:

```bash
gcc -o yaml_example yaml_example.c -lghoti-text -I/usr/local/include
./yaml_example input.yaml
```

Or with pkg-config:

```bash
gcc -o yaml_example yaml_example.c $(pkg-config --cflags --libs ghoti.io-text)
./yaml_example input.yaml
```

## See Also

- [YAML Module Documentation](@ref yaml_module) - Complete module reference
- [Examples Overview](@ref text_examples) - All library examples
- [API Reference](@ref text_functions_index) - Complete function reference

## Contributing Examples

Have a useful YAML parsing pattern? Consider contributing an example! Examples should:
- Demonstrate a clear use case
- Include complete working code
- Explain key concepts
- Handle errors properly
- Follow security best practices
