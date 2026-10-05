@page example_yaml_config_parser YAML: config

# YAML: config

This example demonstrates building a structured configuration from YAML events: collecting key-value pairs, handling nested sections, and constructing a usable config object.

@note This page is a guide, not a walkthrough of a file in the tree.
The standalone program `examples/yaml/yaml_config_parser.c` has not been written yet, so
the code below is meant to be read and adapted rather than built. The one
YAML example that does exist is
\ref example_yaml_writer_formatting "yaml_writer_formatting.c".


## What This Example Demonstrates

- **Stateful event processing** - Track parser state across events
- **Building data structures** - Convert YAML events to C structs
- **Nested sections** - Handle hierarchical configuration
- **Practical config parsing** - Real-world configuration file patterns
- **Key-value extraction** - Parse mapping keys and values

## Use Case

Parse application configuration files like:

```yaml
# app.yaml
application:
  name: MyWebService
  version: 2.1.0
  
server:
  host: 0.0.0.0
  port: 8080
  workers: 4
  
database:
  host: db.example.com
  port: 5432
  name: myapp_prod
  username: dbuser
  ssl: true
  
logging:
  level: info
  file: /var/log/myapp.log
```

## Conceptual Source Code

```c
#include <ghoti.io/text/yaml.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Configuration structure
typedef struct {
    char app_name[64];
    char app_version[16];
    char server_host[256];
    int server_port;
    int server_workers;
    char db_host[256];
    int db_port;
    char db_name[64];
    char log_level[16];
    char log_file[256];
} AppConfig;

// Parser state
typedef struct {
    AppConfig *config;
    char current_section[64];
    char current_key[64];
    int in_mapping;
    int expect_value;
} ParserState;

GTEXT_YAML_Status config_event_callback(
    GTEXT_YAML_Stream *s,
    const void *event_payload,
    void *user
) {
    ParserState *state = (ParserState *)user;
    const GTEXT_YAML_Event *ev = (const GTEXT_YAML_Event *)event_payload;
    
    switch (ev->type) {
        case GTEXT_YAML_EVENT_SCALAR: {
            char value[256];
            size_t len = ev->data.scalar.len < 255 ? 
                         ev->data.scalar.len : 255;
            memcpy(value, ev->data.scalar.ptr, len);
            value[len] = '\0';
            
            if (state->expect_value) {
                // This scalar is a value - store it based on current key
                if (strcmp(state->current_section, "application") == 0) {
                    if (strcmp(state->current_key, "name") == 0) {
                        strncpy(state->config->app_name, value, 63);
                    } else if (strcmp(state->current_key, "version") == 0) {
                        strncpy(state->config->app_version, value, 15);
                    }
                } else if (strcmp(state->current_section, "server") == 0) {
                    if (strcmp(state->current_key, "host") == 0) {
                        strncpy(state->config->server_host, value, 255);
                    } else if (strcmp(state->current_key, "port") == 0) {
                        state->config->server_port = atoi(value);
                    } else if (strcmp(state->current_key, "workers") == 0) {
                        state->config->server_workers = atoi(value);
                    }
                } else if (strcmp(state->current_section, "database") == 0) {
                    if (strcmp(state->current_key, "host") == 0) {
                        strncpy(state->config->db_host, value, 255);
                    } else if (strcmp(state->current_key, "port") == 0) {
                        state->config->db_port = atoi(value);
                    } else if (strcmp(state->current_key, "name") == 0) {
                        strncpy(state->config->db_name, value, 63);
                    }
                } else if (strcmp(state->current_section, "logging") == 0) {
                    if (strcmp(state->current_key, "level") == 0) {
                        strncpy(state->config->log_level, value, 15);
                    } else if (strcmp(state->current_key, "file") == 0) {
                        strncpy(state->config->log_file, value, 255);
                    }
                }
                
                state->expect_value = 0;
                state->current_key[0] = '\0';
                
            } else {
                // This scalar is a key - save it
                strncpy(state->current_key, value, 63);
                state->current_key[63] = '\0';
                
                // Check if this is a section name (top-level key)
                if (!state->in_mapping || 
                    strlen(state->current_section) == 0) {
                    strncpy(state->current_section, value, 63);
                    state->current_section[63] = '\0';
                }
            }
            break;
        }
        
        case GTEXT_YAML_EVENT_INDICATOR:
            if (ev->data.indicator == ':') {
                state->expect_value = 1;
            }
            break;
            
        case GTEXT_YAML_EVENT_MAPPING_START:
            state->in_mapping++;
            break;
            
        case GTEXT_YAML_EVENT_MAPPING_END:
            state->in_mapping--;
            if (state->in_mapping == 0) {
                state->current_section[0] = '\0';
            }
            break;
            
        default:
            break;
    }
    
    return GTEXT_YAML_OK;
}

int parse_config_file(const char *filename, AppConfig *config) {
    // Initialize config with defaults
    memset(config, 0, sizeof(AppConfig));
    strcpy(config->server_host, "localhost");
    config->server_port = 8080;
    config->server_workers = 1;
    strcpy(config->db_host, "localhost");
    config->db_port = 5432;
    strcpy(config->log_level, "info");
    
    // Read file
    FILE *f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open %s\n", filename);
        return 1;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *buffer = malloc(size + 1);
    if (!buffer) {
        fclose(f);
        return 1;
    }
    
    fread(buffer, 1, size, f);
    buffer[size] = '\0';
    fclose(f);
    
    // Parse with security limits
    GTEXT_YAML_Parse_Options opts = {0};
    opts.max_depth = 16;
    opts.max_total_bytes = 256 * 1024;  // 256 KB max
    opts.max_alias_expansion = 100;
    
    // Initialize parser state
    ParserState state = {0};
    state.config = config;
    
    GTEXT_YAML_Stream *parser = gtext_yaml_stream_new(
        &opts,
        config_event_callback,
        &state
    );
    
    if (!parser) {
        free(buffer);
        return 1;
    }
    
    GTEXT_YAML_Status status = gtext_yaml_stream_feed(parser, buffer, size);
    free(buffer);
    
    if (status != GTEXT_YAML_OK) {
        fprintf(stderr, "Parse error: %d\n", status);
        gtext_yaml_stream_free(parser);
        return 1;
    }
    
    status = gtext_yaml_stream_finish(parser);
    gtext_yaml_stream_free(parser);
    
    if (status != GTEXT_YAML_OK) {
        fprintf(stderr, "Finish error: %d\n", status);
        return 1;
    }
    
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <config.yaml>\n", argv[0]);
        return 1;
    }
    
    AppConfig config;
    if (parse_config_file(argv[1], &config) != 0) {
        return 1;
    }
    
    // Print parsed configuration
    printf("Application Configuration:\n");
    printf("  Name: %s\n", config.app_name);
    printf("  Version: %s\n", config.app_version);
    printf("\n");
    printf("Server:\n");
    printf("  Host: %s\n", config.server_host);
    printf("  Port: %d\n", config.server_port);
    printf("  Workers: %d\n", config.server_workers);
    printf("\n");
    printf("Database:\n");
    printf("  Host: %s\n", config.db_host);
    printf("  Port: %d\n", config.db_port);
    printf("  Name: %s\n", config.db_name);
    printf("\n");
    printf("Logging:\n");
    printf("  Level: %s\n", config.log_level);
    printf("  File: %s\n", config.log_file);
    
    return 0;
}
```

## Key Concepts

### Stateful Event Processing

The parser needs to track state across events:
```c
typedef struct {
    AppConfig *config;        // Target structure
    char current_section[64]; // Current top-level section
    char current_key[64];     // Current mapping key
    int in_mapping;           // Nesting level
    int expect_value;         // Next scalar is a value (not key)
} ParserState;
```

### Key-Value Pattern Recognition

YAML mappings emit events in this sequence:
1. `SCALAR` (key)
2. `INDICATOR` (`:`)
3. `SCALAR` (value)

The callback tracks this pattern:
```c
if (ev->data.indicator == ':') {
    state->expect_value = 1;  // Next scalar is the value
}
```

### Nested Sections

Track nesting depth to know which section you're in:
```c
case GTEXT_YAML_EVENT_MAPPING_START:
    state->in_mapping++;
    break;
    
case GTEXT_YAML_EVENT_MAPPING_END:
    state->in_mapping--;
    if (state->in_mapping == 0) {
        state->current_section[0] = '\0';  // Exited top level
    }
    break;
```

## Common Patterns

### Default Values

Set defaults before parsing:
```c
strcpy(config->server_host, "localhost");
config->server_port = 8080;
config->server_workers = 1;
```

### Type Conversion

Convert string scalars to appropriate types:
```c
config->server_port = atoi(value);           // String to int
config->enable_ssl = strcmp(value, "true") == 0;  // String to bool
config->timeout = atof(value);               // String to float
```

### Error Handling

Check parse status at every step:
```c
GTEXT_YAML_Status status = gtext_yaml_stream_feed(parser, buffer, size);
if (status != GTEXT_YAML_OK) {
    fprintf(stderr, "Parse error at offset %zu\n", /* offset */);
    return 1;
}
```

## Production Considerations

### Validation

Add validation after parsing:
```c
if (config->server_port < 1 || config->server_port > 65535) {
    fprintf(stderr, "Invalid port: %d\n", config->server_port);
    return 1;
}

if (config->server_workers < 1 || config->server_workers > 128) {
    fprintf(stderr, "Invalid worker count: %d\n", config->server_workers);
    return 1;
}
```

### Security of untrusted input

Always use limits for config files from untrusted sources:
```c
opts.max_depth = 16;              // Config files rarely need deep nesting
opts.max_total_bytes = 256 * 1024; // 256 KB is generous for config
opts.max_alias_expansion = 100;    // Config files rarely use aliases
```

### Schema Validation

Consider validating required fields:
```c
if (strlen(config->app_name) == 0) {
    fprintf(stderr, "Missing required field: application.name\n");
    return 1;
}
```

## Related Examples

- [yaml_streaming_basic.c](@ref example_yaml_streaming_basic) - Basic streaming parser
- [yaml_security.c](@ref example_yaml_security) - Security limits and validation
- [Examples Overview](@ref text_examples) - Return to examples index
