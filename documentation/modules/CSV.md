@page csv_module CSV

# CSV

This document describes the **full‑featured CSV parsing/writing library in C** implemented in the `text` library in the `ghoti.io` family. The implementation is **cross‑platform** and prioritizes **correctness** and **predictability** over convenience shortcuts. It is not dependency‑free: the library requires [ghoti.io-cutil](https://github.com/Ghoti-io/cutil), [ghoti.io-chron](https://github.com/Ghoti-io/chron) and [ghoti.io-unicode](https://github.com/Ghoti-io/unicode); the first two appear in *public* headers and unicode is a link dependency only. See the [Dependencies](README.md#dependencies) section of the README.

CSV in the wild is not a single format. This module supports a **strict RFC 4180 mode** plus an explicit, configurable **dialect system** to handle common variants (TSV, custom delimiters, different quoting rules, Excel‑style behaviors, etc.).

---

@note This page documents the **API**. For the specification-level view -
RFC 4180 conformance, what each dialect option actually means, the reach of
`validate_utf8`, and what evidence backs each claim - see
\ref format_csv "CSV" under
\ref text_format_references "Format and specification references".

## 1. Overview

The CSV module provides comprehensive CSV processing capabilities with support for strict RFC 4180 compliance, configurable dialects, multiple parsing and writing models, and advanced features like header processing and zero-copy parsing.

### Core Capabilities

- **Strict CSV parsing** per RFC 4180 with well-defined behavior
- **Dialect configuration**: delimiter, quote char, escaping rules, whitespace rules, newline rules
- **Two parsing models**: DOM/table and streaming/SAX
- **Two writing models**: DOM serialization and streaming writer
- **High-quality error diagnostics** with position information and context snippets
- **Round-trip correctness** under a chosen dialect (parse → write → parse stable)
- **Optional header processing** and column name mapping
- **In-situ / zero-copy parsing** for performance optimization

---

## 2. Parsing Modes

### 2.1 DOM Parsing

DOM parsing builds a complete in-memory table structure of the CSV data. This mode is ideal when you need to:

- Navigate and query the CSV structure
- Access values multiple times
- Work with the entire document at once

The DOM is allocated from an arena, making cleanup simple with a single `gtext_csv_free_table()` call.

### 2.2 Streaming Parsing

Streaming parsing processes CSV incrementally, emitting events as records and fields are encountered. This mode is ideal when you need to:

- Process large CSV files with minimal memory usage
- Handle CSV from network streams or files
- Transform CSV on-the-fly without building a full DOM

The streaming parser accepts input in chunks and maintains state between calls, making it suitable for network or file I/O scenarios.

---

## 3. Writing Modes

### 3.1 DOM Serialization

DOM serialization writes a complete CSV table to output. This is the simplest mode for converting a table back to CSV text.

### 3.2 Streaming Writer

The streaming writer allows you to construct CSV incrementally with structural enforcement. The writer maintains internal state to ensure valid CSV output (e.g., preventing fields without records).

---

## 4. Parse Options

The library provides extensive configuration options for parsing behavior:

### 4.1 Dialect Configuration

The dialect defines the exact format rules for parsing and writing CSV:

- **`delimiter`**: Field delimiter — **Default: `','`**
- **`quote`**: Quote character — **Default: the double-quote character**
- **`escape`**: Escape mode:
  - `GTEXT_CSV_ESCAPE_DOUBLED_QUOTE`: Escape quotes by doubling (`""`) — **Default**
  - `GTEXT_CSV_ESCAPE_BACKSLASH`: Escape quotes with a backslash
  - `GTEXT_CSV_ESCAPE_NONE`: No escaping
- **`newline_in_quotes`**: Allow newlines inside quoted fields — **Default: `true`**
- **`accept_lf`**: Accept LF (`\n`) as newline — **Default: `true`**
- **`accept_crlf`**: Accept CRLF as newline — **Default: `true`**
- **`accept_cr`**: Accept a bare CR as newline — **Default: `false`**
- **`trim_unquoted_fields`**: Trim whitespace from unquoted fields — **Default: `false`**
- **`allow_space_after_delimiter`**: Allow spaces after delimiter — **Default: `false`**
- **`allow_unquoted_quotes`**: Allow quotes in unquoted fields — **Default: `false`**
- **`allow_unquoted_newlines`**: Allow newlines in unquoted fields — **Default: `false`**
- **`allow_comments`**: Allow comment lines — **Default: `false`**
- **`comment_prefix`**: Comment prefix string — **Default: `"#"`**
- **`treat_first_row_as_header`**: Treat first row as header — **Default: `false`**
- **`header_dup_mode`**: Duplicate column name handling:
  - `GTEXT_CSV_DUPCOL_ERROR`: Fail parse on duplicate column name
  - `GTEXT_CSV_DUPCOL_FIRST_WINS`: Use first occurrence — **Default** (duplicates allowed, first match returned)
  - `GTEXT_CSV_DUPCOL_LAST_WINS`: Use last occurrence
  - `GTEXT_CSV_DUPCOL_COLLECT`: Store all indices for duplicate columns

### 4.2 Unicode / Input Handling

- **`validate_utf8`**: Validate UTF-8 sequences in input — **Default: `true`**
- **`in_situ_mode`**: Zero-copy mode that references input buffer directly — **Default: `false`**
- **`keep_bom`**: Keep UTF-8 BOM at the start of input — **Default: `false`** (strips BOM if false)

### 4.3 Resource Limits

All limits use `0` to indicate library defaults:

- **`max_rows`**: Maximum number of rows (default: 10M)
- **`max_cols`**: Maximum number of columns per row (default: 100k)
- **`max_field_bytes`**: Maximum field size in bytes (default: 16MB)
- **`max_record_bytes`**: Maximum record size in bytes (default: 64MB)
- **`max_total_bytes`**: Maximum total input size (default: 1GB)

### 4.4 Error Context

- **`enable_context_snippet`**: Generate context snippet for errors — **Default: `true`**. When false no snippet is allocated and `GTEXT_CSV_Error::context_snippet` stays `NULL`.
- **`context_radius_bytes`**: Bytes before and after the error position to include in the snippet — **Default: `40`**, and `0` means the library default, as it does for the other size limits here.

---

## 5. Write Options

The library provides extensive configuration options for output formatting:

### 5.1 Quoting and Escaping

- **`quote_all_fields`**: Quote all fields — **Default: `false`**
- **`quote_empty_fields`**: Quote empty fields — **Default: `true`**
- **`quote_if_needed`**: Quote fields containing delimiter/quote/newline — **Default: `true`**
- **`quoting`**: The policy as a whole — **Default: `GTEXT_CSV_QUOTE_MINIMAL`**, which is zero. `GTEXT_CSV_QUOTE_ALL`, `GTEXT_CSV_QUOTE_NONNUMERIC` and `GTEXT_CSV_QUOTE_NONE` are the other three, named after Python's. `NONNUMERIC` asks whether the field's text spells a number, not what type it is; `NONE` refuses a field unquoted bytes cannot carry. `quote_all_fields` still wins over this when set.
- **`always_escape_quotes`**: What to do with a quote character in a field that is *not* being quoted — **Default: `true`**, which refuses it with `GTEXT_CSV_E_UNQUOTABLE_FIELD`. RFC 4180 gives a quote inside an unquoted field no special meaning, so unquoted `a""b` is four characters rather than <tt>a&quot;b</tt>, and a reader at the default `allow_unquoted_quotes` refuses those bytes. Clearing it emits the quote verbatim, which is readable with `allow_unquoted_quotes` on and is left to the caller. Quotes inside quoted fields are always escaped regardless, since leaving one unescaped would end the field early. No effect when the dialect's escape mode is `GTEXT_CSV_ESCAPE_NONE`. Only observable with `quote_if_needed` cleared, because otherwise a field containing a quote is quoted.

### 5.2 Formatting

- **`newline`**: Newline string for output — **Default: `"\n"`** (can use `"\r\n"`)
- **`trailing_newline`**: Terminate the final record with a newline — **Default: `true`**. Records are always separated by one; this decides whether the last is followed by one too. An empty table writes nothing either way. It was documented as defaulting to `false` and was read by no code, so every non-empty table got a trailing newline regardless; the default now states what the writer does.

### 5.3 Field Trimming

- **`trim_trailing_empty_fields`**: Trim trailing empty fields from rows — **Default: `false`**

When enabled, the writer automatically detects and omits trailing empty fields from each row during output. This is useful for reducing file size when trailing empty fields are not meaningful.

**Behavior:**
- When `trim_trailing_empty_fields == false` (default): All fields are written, including trailing empty fields
- When `trim_trailing_empty_fields == true`: Only fields up to and including the last non-empty field are written

**Empty Field Detection:**
A field is considered empty if:
- The field has zero length (`field->length == 0`), or
- The field contains only a null terminator (`field->data[0] == '\0'`)

Quoted empty fields (e.g., `""`) are also considered empty for trimming purposes.

**Examples:**

**Example 1: Basic Trimming**
```c
// Table with trailing empty fields:
// Row 0: ["Name", "Age", "City", "", ""]
// Row 1: ["Alice", "30", "NYC", "", ""]
// Row 2: ["Bob", "25", "", ""]

gtext_csv_write_options opts = gtext_csv_write_options_default();
opts.trim_trailing_empty_fields = true;

// Output (with trimming):
// Name,Age,City
// Alice,30,NYC
// Bob,25
```

**Example 2: Empty Field in Middle (Not Trimmed)**
```c
// Table:
// Row 0: ["Name", "", "Age", "City", ""]
// Row 1: ["Alice", "", "30", "NYC", ""]

opts.trim_trailing_empty_fields = true;

// Output (empty field in middle is preserved):
// Name,,Age,City
// Alice,,30,NYC
```

**Example 3: All Fields Empty**
```c
// Table:
// Row 0: ["", "", ""]

opts.trim_trailing_empty_fields = true;

// Output (empty line):
//
```

**Round-Trip Implications:**

⚠️ **Warning**: When `trim_trailing_empty_fields` is enabled, trailing empty fields are **not written** to the output. This means that parsing and writing a table may **not preserve trailing empty fields exactly**.

**Example of Data Loss:**
```c
// Parse CSV with trailing empty fields
const char* csv = "Name,Age,City,,\nAlice,30,NYC,,\n";
gtext_csv_table* table = gtext_csv_parse_table(csv, strlen(csv), &parse_opts, NULL);
// Table has 5 columns per row (including 2 trailing empty fields)

// Write with trimming enabled
gtext_csv_write_options write_opts = gtext_csv_write_options_default();
write_opts.trim_trailing_empty_fields = true;
gtext_csv_write_table(&sink, &write_opts, table);

// Output: "Name,Age,City\nAlice,30,NYC\n"
// Trailing empty fields are lost!

// If you parse the output again:
gtext_csv_table* table2 = gtext_csv_parse_table(output, strlen(output), &parse_opts, NULL);
// Table2 has only 3 columns per row (trailing empty fields are gone)
```

**When to Use:**
- ✅ Use when trailing empty fields are not meaningful and file size reduction is desired
- ✅ Use when output will not be parsed again (one-way export)
- ✅ Use when you explicitly want to normalize output by removing trailing empty fields
- ❌ **Do not use** if you need round-trip preservation of trailing empty fields
- ❌ **Do not use** if trailing empty fields carry semantic meaning (e.g., placeholder positions)

**Consistency:**
The trimming option applies consistently to both header rows and data rows. If a header row has trailing empty fields, they will be trimmed just like data rows.

---

## 6. Output Sinks

The writer supports multiple output destinations through a sink abstraction:

- **Growable buffer**: Dynamically-growing buffer for complete output
- **Fixed buffer**: Fixed-size buffer with truncation detection
- **Callback sink**: Custom write function for any destination (files, network, etc.)

---

## 7. DOM Operations

### 7.1 Value Access

The DOM provides accessors for CSV data:

- **Row count**: `gtext_csv_row_count()` — returns number of data rows (excluding header if present)
- **Column count**: `gtext_csv_col_count()` — returns number of columns in a row
- **Field access**: `gtext_csv_field()` — returns field data and length

### 7.2 Header Operations

When header processing is enabled:

- **Header lookup**: `gtext_csv_header_index()` — get column index by header name (returns first match for duplicate headers)
- **Header iteration**: `gtext_csv_header_index_next()` — get next column index for a header name after a given index (for iterating through duplicate headers)
- **Header uniqueness check**: `gtext_csv_can_have_unique_headers()` — check if table has headers and all header names are unique
- **Require unique headers**: `gtext_csv_set_require_unique_headers()` — enable/disable uniqueness requirement for mutation operations
- **Toggle header row**: `gtext_csv_set_header_row()` — enable or disable header row processing after parsing
- Header row is excluded from row count but accessible via adjusted indices

**Default Behavior:**
- By default, duplicate header names are **allowed** during parsing (`header_dup_mode` defaults to `GTEXT_CSV_DUPCOL_FIRST_WINS`)
- By default, mutation operations (column append, insert, rename) **allow** duplicate header names (`require_unique_headers` defaults to `false`)
- To enforce uniqueness, set `require_unique_headers` to `true` or use `GTEXT_CSV_DUPCOL_ERROR` mode during parsing

### 7.3 Table Mutation Operations

The CSV module provides a comprehensive set of mutation operations for modifying CSV tables in memory. All mutation operations are **atomic** — they either complete successfully or leave the table unchanged.

#### 7.3.1 Table Creation

**Create Empty Table:**
```c
gtext_csv_table* table = gtext_csv_new_table();
```

Creates a new empty table with initialized context and arena. The table starts with a default row capacity of 16 rows. No columns are defined until the first row is added.

**Create Table With Headers:**
```c
const char* headers[] = {"Name", "Age", "City"};
gtext_csv_table* table = gtext_csv_new_table_with_headers(headers, NULL, 3);
```

Creates a new table with specified column headers. Headers are treated as the first row and are excluded from the row count. A header map is built for fast column name lookup. Duplicate header names are not allowed.

#### 7.3.2 Row Operations

**Append Row:**
```c
const char* fields[] = {"Alice", "30", "New York"};
gtext_csv_row_append(table, fields, NULL, 3);
```

Adds a new row with the specified field values to the end of the table. The first row added sets the column count for the table. Subsequent rows must have the same number of fields (strict validation). All field data is copied to the arena.

**Insert Row:**
```c
const char* fields[] = {"Bob", "25", "San Francisco"};
gtext_csv_row_insert(table, 1, fields, NULL, 3);  // Insert at index 1
```

Inserts a new row at the specified index, shifting existing rows right. The index can equal `row_count`, which is equivalent to appending.

**Remove Row:**
```c
gtext_csv_row_remove(table, 0);  // Remove first data row
```

Removes the row at the specified index, shifting remaining rows left. If the table has headers, the header row (index 0) cannot be removed.

**Replace Row:**
```c
const char* fields[] = {"Charlie", "35", "Chicago"};
gtext_csv_row_set(table, 0, fields, NULL, 3);  // Replace first data row
```

Replaces the row at the specified index with new field values. The field count must match the table's column count.

**Clear Table:**
```c
gtext_csv_table_clear(table);
```

Removes all data rows from the table while preserving the table structure (headers if present, column count). This function automatically compacts the table to free memory from cleared rows.

#### 7.3.3 Column Operations

**Append Column:**
```c
gtext_csv_column_append(table, "Country", 0);  // Add "Country" column (null-terminated)
```

Adds a new column to the end of all rows. If the table has headers, the `header_name` parameter is required. All existing rows get an empty field added at the end.

**Append Column with Initial Values:**
```c
const char* values[] = {"Country", "USA", "Canada", "Mexico"};  // Header + 3 data rows
gtext_csv_column_append_with_values(table, NULL, 0, values, NULL);
```

Adds a new column to the end of all rows with initial values for each row. The number of values must exactly match the number of rows:
- If the table has headers: value count must match `row_count + 1` (header row + data rows)
- If the table has no headers: value count must match `row_count`
- If `values` is NULL, creates an empty column (uses `header_name` for header if table has headers)

When the table has headers and `values` is provided, `values[0]` is used for both the header field value and the header map entry. The `header_name` parameter is ignored in this case.

**Insert Column:**
```c
gtext_csv_column_insert(table, 1, "MiddleName", 0);  // Insert at index 1
```

Inserts a new column at the specified index, shifting existing columns right. The index can equal the column count, which is equivalent to appending. When headers are present, all header map entries after the insertion point are automatically reindexed.

**Insert Column with Initial Values:**
```c
const char* values[] = {"MiddleName", "John", "Jane", "Bob"};  // Header + 3 data rows
gtext_csv_column_insert_with_values(table, 1, NULL, 0, values, NULL);  // Insert at index 1
```

Inserts a new column at the specified index with initial values for each row. Same value count requirements as `gtext_csv_column_append_with_values()`. The index can equal the column count, which is equivalent to appending.

**Remove Column:**
```c
gtext_csv_column_remove(table, 0);  // Remove first column
```

Removes the column at the specified index from all rows, shifting remaining columns left. When headers are present, the header map entry is removed and remaining entries are reindexed.

**Rename Column:**
```c
gtext_csv_column_rename(table, 0, "FullName", 0);  // Rename first column
```

Renames a column header. This function only works if the table has headers. By default, duplicate header names are allowed. If `require_unique_headers` is `true`, the new header name must not duplicate an existing header name.

#### 7.3.4 Field Operations

**Set Field Value:**
```c
gtext_csv_field_set(table, 0, 1, "31", 0);  // Set row 0, column 1 to "31"
```

Sets the value of a field at specified row and column indices. The field data is copied to the arena. If the field was previously in-situ (referencing the input buffer), it will be copied to the arena. If `field_length` is 0 and `field_data` is not NULL, it is assumed to be a null-terminated string.

#### 7.3.5 Header Management Operations

**Set Header Row:**
```c
gtext_csv_set_header_row(table, true);   // Enable headers (first row becomes header)
gtext_csv_set_header_row(table, false);  // Disable headers (header becomes first data row)
```

Enables or disables header row processing after parsing. When enabling headers:
- The first data row becomes the header row
- A header map is built for column name lookup
- If `require_unique_headers` is `true`, validates that all header names are unique
- Column count is adjusted if the first row has a different number of columns

When disabling headers:
- The header row becomes the first data row
- The header map is cleared
- Row count increases by 1 (header row becomes a data row)

**Set Require Unique Headers:**
```c
gtext_csv_set_require_unique_headers(table, true);   // Enforce uniqueness
gtext_csv_set_require_unique_headers(table, false);  // Allow duplicates (default)
```

Controls whether mutation operations (column append, insert, rename) enforce uniqueness of header names. When set to `true`, these operations will fail if they would create duplicate header names. This flag only affects mutation operations; parsing behavior is controlled by `header_dup_mode` in parse options.

**Check Unique Headers:**
```c
bool can_have_unique = gtext_csv_can_have_unique_headers(table);
```

Returns `true` if the table has headers and all header names are currently unique. Returns `false` if the table has no headers or contains duplicate header names. Useful for checking if a table is in a state where unique headers can be enforced.

**Iterate Through Duplicate Headers:**
```c
size_t idx;
if (gtext_csv_header_index(table, "Name", &idx) == GTEXT_CSV_OK) {
    // Process first match
    do {
        // Process column at idx
        // ...
    } while (gtext_csv_header_index_next(table, "Name", idx, &idx) == GTEXT_CSV_OK);
}
```

Iterates through all columns with the same header name. First call `gtext_csv_header_index()` to get the first match, then repeatedly call `gtext_csv_header_index_next()` until it returns `GTEXT_CSV_E_INVALID` (no more matches).

#### 7.3.6 Utility Operations

**Clone Table:**
```c
gtext_csv_table* clone = gtext_csv_clone(table);
```

Creates a deep copy of the table, allocating all memory from a new arena. The cloned table is completely independent of the original.

**Compact Table:**
```c
gtext_csv_table_compact(table);
```

Moves all current table data to a new arena and frees the old arena. This releases memory from old allocations that may have been left behind due to repeated modifications. This function is automatically called by `gtext_csv_table_clear()`, but can also be called independently.

#### 7.3.7 Performance Characteristics

**Row Operations:**
- **Append**: O(1) amortized (O(n) when capacity grows)
- **Insert**: O(n) where n is the number of rows after insertion point
- **Remove**: O(n) where n is the number of rows after removal point
- **Set**: O(1) per field
- **Clear**: O(1) (compaction is O(n) but amortized)

**Column Operations:**
- **Append**: O(n) where n is the number of rows
- **Insert**: O(n×m) where n is the number of rows and m is the number of columns after insertion point
- **Remove**: O(n×m) where n is the number of rows and m is the number of columns after removal point
- **Rename**: O(1) (header map lookup is O(1) average case)

**Field Operations:**
- **Set**: O(1) per field

**Utility Operations:**
- **Clone**: O(n×m) where n is the number of rows and m is the average number of columns
- **Compact**: O(n×m) where n is the number of rows and m is the average number of columns

All mutation operations are atomic — they either complete successfully or leave the table unchanged. Memory allocations are performed before any state changes, ensuring no partial modifications.

---

## 8. Irregular Rows Support

The CSV module supports tables with irregular row lengths (rows with different numbers of columns). This is useful when working with CSV data that doesn't conform to a strict rectangular structure.

### 8.1 Parsing Irregular Rows

By default, the parser **accepts and preserves** irregular row lengths during parsing. Each row stores its own `field_count`, allowing tables to contain rows with varying numbers of columns.

**Example:**
```c
// CSV with irregular rows:
// name,age,city
// Alice,30
// Bob,25,LA,extra
// Charlie

const char* csv = "name,age,city\nAlice,30\nBob,25,LA,extra\nCharlie\n";
gtext_csv_table* table = gtext_csv_parse_table(csv, strlen(csv), &opts, NULL);

// Table structure:
// Row 0 (header): 3 columns ["name", "age", "city"]
// Row 1: 2 columns ["Alice", "30"]
// Row 2: 4 columns ["Bob", "25", "LA", "extra"]
// Row 3: 1 column ["Charlie"]
```

You can query each row's actual column count using `gtext_csv_col_count(table, row)`.

### 8.2 Mutation Operations and Irregular Rows

By default, mutation operations (row append, insert, replace) enforce a **strict rectangular structure** — all rows must have the same number of columns. However, you can enable irregular rows support for mutation operations.

#### 8.2.1 Enabling Irregular Rows Mode

Use `gtext_csv_set_allow_irregular_rows()` to enable irregular rows for mutation operations:

```c
// Enable irregular rows mode
gtext_csv_set_allow_irregular_rows(table, true);

// Now you can append rows with any field count
const char* fields1[] = {"Alice", "30"};  // 2 fields
gtext_csv_row_append(table, fields1, NULL, 2);

const char* fields2[] = {"Bob", "25", "LA", "extra"};  // 4 fields
gtext_csv_row_append(table, fields2, NULL, 4);
```

**Behavior Differences:**

| Operation | Strict Mode (default) | Irregular Mode |
|-----------|----------------------|----------------|
| **Row Append** | Must match `column_count` | Accepts any field count, updates `column_count` to max |
| **Row Insert** | Must match `column_count` | Accepts any field count, updates `column_count` to max |
| **Row Replace** | Must match `column_count` | Accepts any field count, updates `column_count` to max |
| **Column Insert** | Fails if `col_idx > row->field_count` | Pads short rows with empty fields |
| **Column Count** | Represents expected count | Represents maximum count across all rows |

#### 8.2.2 Column Count Tracking

When irregular rows are enabled, `column_count` represents the **maximum** field count across all rows. When disabled (strict mode), `column_count` represents the **expected** count that all rows must match.

```c
// Enable irregular rows
gtext_csv_set_allow_irregular_rows(table, true);

// Append rows with different field counts
gtext_csv_row_append(table, (const char*[]){"A", "B"}, NULL, 2);
gtext_csv_row_append(table, (const char*[]){"X", "Y", "Z"}, NULL, 3);
gtext_csv_row_append(table, (const char*[]){"P"}, NULL, 1);

// column_count is now 3 (maximum across all rows)
size_t max_cols = gtext_csv_col_count(table, 0);  // Returns 3
```

#### 8.2.3 Column Insertion with Padding

When irregular rows are enabled, column insertion can pad short rows with empty fields before the insertion point:

```c
// Table with irregular rows:
// Row 0: ["A", "B"]        (2 columns)
// Row 1: ["X"]             (1 column)

gtext_csv_set_allow_irregular_rows(table, true);

// Insert column at index 2
gtext_csv_column_insert(table, 2, "NewCol", 0);

// Result:
// Row 0: ["A", "B", "NewCol"]     (3 columns)
// Row 1: ["X", "", "NewCol"]     (3 columns, padded with empty field)
```

### 8.3 Normalization

You can normalize irregular rows to a uniform column count using the normalization functions:

#### 8.3.1 Normalize to Maximum

Normalize all rows to match the longest row:

```c
// Table with irregular rows:
// Row 0: ["A", "B"]        (2 columns)
// Row 1: ["X", "Y", "Z"]   (3 columns)
// Row 2: ["P"]             (1 column)

gtext_csv_normalize_to_max(table);

// Result (all rows padded to 3 columns):
// Row 0: ["A", "B", ""]     (3 columns)
// Row 1: ["X", "Y", "Z"]   (3 columns)
// Row 2: ["P", "", ""]      (3 columns)
```

#### 8.3.2 Normalize to Specific Count

Normalize to a specific column count with optional truncation:

```c
// Normalize to 4 columns, truncate long rows
gtext_csv_normalize_rows(table, 4, true);

// Normalize to 4 columns, error if any row exceeds 4
gtext_csv_normalize_rows(table, 4, false);
```

**Special Values:**
- `target_column_count = 0`: Normalize to maximum column count
- `target_column_count = SIZE_MAX`: Normalize to minimum column count

**Performance Considerations:**
- Normalization is an **atomic operation** — either all rows are normalized or the table remains unchanged
- Time complexity: O(n×m) where n is the number of rows and m is the average number of columns
- Memory: All new field arrays are pre-allocated before any modifications (ensuring atomicity)
- Optimization: If the table is already normalized to the target count, the function performs a no-op
- All field data is allocated from the table's arena for efficient memory management

**Example: Normalizing Parsed Irregular CSV**

```c
// Parse CSV that may have irregular rows
const char* csv = "name,age,city\nAlice,30\nBob,25,LA,extra\nCharlie\n";
gtext_csv_table* table = gtext_csv_parse_table(csv, strlen(csv), &opts, NULL);

// Check if normalization is needed
if (gtext_csv_has_irregular_rows(table)) {
    // Normalize to maximum (pad short rows)
    GTEXT_CSV_Status status = gtext_csv_normalize_to_max(table);
    if (status != GTEXT_CSV_OK) {
        // Handle error
    }

    // Now all rows have the same column count
    // Row 0: ["name", "age", "city", ""]     (4 columns, header padded)
    // Row 1: ["Alice", "30", "", ""]         (4 columns, padded)
    // Row 2: ["Bob", "25", "LA", "extra"]    (4 columns)
    // Row 3: ["Charlie", "", "", ""]          (4 columns, padded)
}

// Or normalize to a specific count with truncation
gtext_csv_normalize_rows(table, 3, true);  // Truncate to 3 columns
```

### 8.4 Validation Functions

Query functions are available to inspect table structure:

```c
// Check if table has irregular rows
bool has_irregular = gtext_csv_has_irregular_rows(table);

// Get maximum column count across all rows
size_t max_cols = gtext_csv_max_col_count(table);

// Get minimum column count across all rows
size_t min_cols = gtext_csv_min_col_count(table);

// Comprehensive table validation
GTEXT_CSV_Status status = gtext_csv_validate_table(table);
```

### 8.5 When to Use Irregular Rows

**Use Irregular Rows Mode When:**
- ✅ Working with CSV data that has inconsistent column counts
- ✅ Parsing CSV files with missing or extra fields
- ✅ Building tables incrementally where column counts may vary
- ✅ Processing data where trailing fields are optional

**Use Strict Mode (Default) When:**
- ✅ Working with structured, rectangular data
- ✅ Data integrity requires all rows to have the same column count
- ✅ You want early validation of data structure
- ✅ Performance is critical (strict mode has slightly less overhead)

**Example: Handling Parsed Irregular Data**

```c
// Parse CSV that may have irregular rows
gtext_csv_table* table = gtext_csv_parse_table(csv_data, csv_len, &opts, NULL);

// Check if table has irregular rows
if (gtext_csv_has_irregular_rows(table)) {
    // Option 1: Normalize to maximum
    gtext_csv_normalize_to_max(table);

    // Option 2: Enable irregular mode and work with irregular data
    gtext_csv_set_allow_irregular_rows(table, true);
    // ... perform mutations ...

    // Option 3: Query each row's column count
    for (size_t i = 0; i < gtext_csv_row_count(table); i++) {
        size_t cols = gtext_csv_col_count(table, i);
        // Process row with cols columns
    }
}
```

---

## 9. Edge Cases and Safety Guarantees

This section documents edge cases, memory safety guarantees, atomicity guarantees, and error handling behavior for the CSV module, with special attention to irregular rows functionality.

### 9.1 Edge Cases for Irregular Rows

#### 9.1.1 Empty Tables

- **Empty table creation**: An empty table has `column_count = 0` and `row_count = 0`
- **First row sets column count**: The first row appended to an empty table sets the `column_count` for the table
- **Normalization of empty table**: Normalization functions return `GTEXT_CSV_E_INVALID` for empty tables (no rows to normalize)
- **Validation of empty table**: `gtext_csv_validate_table()` returns `GTEXT_CSV_OK` for empty tables (empty is valid)
- **Query functions on empty table**: `gtext_csv_max_col_count()` and `gtext_csv_min_col_count()` return `0` for empty tables

#### 9.1.2 Single Row Tables

- **Single row sets column count**: A table with only one row has `column_count` equal to that row's `field_count`
- **Normalization of single row**: Normalizing a single-row table is a no-op (already normalized)
- **Irregular rows check**: A single-row table never has irregular rows (only one row to compare)

#### 9.1.3 All Rows Same Length

- **Irregular rows check**: Returns `false` even if `allow_irregular_rows` is enabled (all rows have same length)
- **Column count**: Represents the uniform column count across all rows
- **Normalization**: Is a no-op (table already normalized)

#### 9.1.4 Maximum Column Count Row Removal

When `allow_irregular_rows` is enabled and a row with the maximum `field_count` is removed:
- **Column count recalculation**: `column_count` is automatically recalculated to find the new maximum
- **Optimization**: Recalculation only occurs if the removed row had `field_count == column_count` (otherwise max is unchanged)

#### 9.1.5 Column Insertion Beyond Row Length

When `allow_irregular_rows` is enabled and inserting a column at an index beyond a row's length:
- **Padding behavior**: Short rows are padded with empty fields from their current length to the insertion index
- **Empty field creation**: Padding uses `csv_setup_empty_field()` which allocates from the arena
- **No data loss**: Existing fields are preserved, only empty fields are added

#### 9.1.6 Normalization Edge Cases

- **Normalize to maximum (target = 0)**: All rows padded to match longest row
- **Normalize to minimum (target = SIZE_MAX)**: All rows truncated to match shortest row
- **Normalize to specific count with truncation disabled**: Returns error if any row exceeds target count
- **Normalize to specific count with truncation enabled**: Long rows truncated, short rows padded
- **Already normalized table**: Optimization performs no-op if table is already normalized to target count

#### 9.1.7 Header Row Handling

- **Header row field count**: Header row's `field_count` is included in `column_count` calculation when irregular rows are enabled
- **Header row protection**: Header row cannot be removed via `gtext_csv_row_remove()`
- **Header row normalization**: Header row is normalized along with data rows
- **Column insertion with headers**: Header row is padded/updated consistently with data rows

#### 9.1.8 Write Trimming Edge Cases

- **All fields empty**: When all fields in a row are empty, trimming results in an empty line (no fields written)
- **Empty field in middle**: Only trailing empty fields are trimmed; empty fields in the middle are preserved
- **Quoted empty field**: `""` is considered an empty field and will be trimmed if trailing
- **Round-trip loss**: Trailing empty fields are permanently lost when trimming is enabled (cannot be recovered by re-parsing)

### 9.2 Memory Safety Guarantees

#### 9.2.1 Allocation Strategy

- **Arena allocation**: All table data (rows, fields, field data) is allocated from a single arena
- **Single free point**: All memory is freed with a single call to `gtext_csv_free_table()`
- **No memory leaks**: Proper cleanup on all error paths ensures no memory leaks

#### 9.2.2 Bounds Checking

- **Row index validation**: All row access functions validate row indices against `row_count`
- **Column index validation**: All column access functions validate column indices against each row's `field_count`
- **Array bounds**: All array accesses are bounds-checked before dereferencing
- **Header map consistency**: Header map indices are validated against `column_count`

#### 9.2.3 Overflow Protection

- **Size calculations**: All size calculations (field counts, row counts, buffer sizes) are checked for overflow
- **Integer overflow**: Arithmetic operations are checked to prevent integer overflow
- **Capacity growth**: Table capacity growth uses safe arithmetic to prevent overflow

#### 9.2.4 Null Pointer Safety

- **NULL table checks**: All public API functions check for NULL table pointers
- **NULL field data**: Field data pointers are validated before dereferencing
- **NULL error parameter**: Error parameters are optional (can be NULL) and handled safely

#### 9.2.5 Use-After-Free Prevention

- **Arena lifetime**: All arena-allocated memory is valid for the lifetime of the table
- **No dangling pointers**: Field data pointers remain valid until table is freed
- **In-situ mode**: When in-situ mode is used, input buffer must remain valid for table lifetime

#### 9.2.6 Double-Free Prevention

- **Single ownership**: Each table has a single arena that is freed exactly once
- **Error cleanup**: Failed operations do not leave partially-allocated structures
- **Table free**: `gtext_csv_free_table()` handles NULL tables safely (no-op)

### 9.3 Atomicity Guarantees

#### 9.3.1 Definition

An operation is **atomic** if:
1. All state changes happen atomically (all-or-nothing)
2. If any step fails, the table state is unchanged from before the operation
3. No intermediate inconsistent states are visible

#### 9.3.2 Atomic Operations

All mutation operations are atomic:
- **Row operations**: `gtext_csv_row_append()`, `gtext_csv_row_insert()`, `gtext_csv_row_set()`, `gtext_csv_row_remove()`
- **Column operations**: `gtext_csv_column_append()`, `gtext_csv_column_insert()`, `gtext_csv_column_remove()`, `gtext_csv_column_rename()`
- **Normalization**: `gtext_csv_normalize_rows()`, `gtext_csv_normalize_to_max()`
- **Field operations**: `gtext_csv_field_set()`
- **Header operations**: `gtext_csv_set_header_row()`, `gtext_csv_set_require_unique_headers()`

#### 9.3.3 Atomicity Implementation

- **Pre-allocation**: All memory allocations are performed before any state changes
- **Validation before mutation**: All validation checks occur before any modifications
- **Rollback on failure**: If any step fails, no state changes are committed
- **Consistent state**: Table is always in a consistent, valid state

#### 9.3.4 Non-Atomic Operations

The following operations are **not** atomic (but are safe):
- **Query operations**: All read-only operations (no state changes)
- **Validation**: `gtext_csv_validate_table()` (read-only)
- **Table creation**: `gtext_csv_new_table()`, `gtext_csv_new_table_with_headers()` (creates new table, no existing state to preserve)

### 9.4 Error Handling Behavior

#### 9.4.1 Error Codes

The CSV module uses `GTEXT_CSV_Status` enum for error codes:
- **`GTEXT_CSV_OK`**: Operation succeeded
- **`GTEXT_CSV_E_INVALID`**: Invalid input (NULL pointer, out of bounds, validation failure)
- **`GTEXT_CSV_E_MEMORY`**: Memory allocation failure
- **`GTEXT_CSV_E_PARSE`**: Parse error (syntax error, encoding error)
- **`GTEXT_CSV_E_WRITE`**: Write error (sink error, buffer full)

#### 9.4.2 Error Reporting

- **Error structure**: `GTEXT_CSV_Error` structure contains detailed error information:
  - `message`: Human-readable error message
  - `line`, `column`: Position information (1-based)
  - `context_snippet`: Context around error location
  - `row_index`, `col_index`: Table indices (for mutation errors)
- **Optional error parameter**: Error parameter is optional (can be NULL)
- **Error message allocation**: Error messages are dynamically allocated and must be freed with `gtext_csv_error_free()`

#### 9.4.3 Error Recovery

- **No partial state**: Failed operations leave table unchanged (atomicity)
- **Error cleanup**: All error paths properly clean up allocated resources
- **Continue after error**: Table remains usable after a failed operation (state unchanged)

#### 9.4.4 Validation Errors

When validation fails:
- **Field count mismatch**: Error includes expected vs actual field count
- **Row index included**: Error includes the row index where validation failed
- **Clear error messages**: Error messages explain what went wrong and where

#### 9.4.5 Memory Errors

When memory allocation fails:
- **No state change**: Table state is unchanged (atomicity)
- **Error code**: Returns `GTEXT_CSV_E_MEMORY`
- **Cleanup**: Any pre-allocated memory is freed before returning

### 9.5 Thread Safety

- **Not thread-safe**: The CSV module is **not** thread-safe
- **Single-threaded use**: Each table should be used by a single thread
- **Concurrent access**: Concurrent access to the same table is not supported and may cause undefined behavior
- **Multiple tables**: Different tables can be used concurrently by different threads (no shared state)

### 9.6 Performance Considerations

#### 9.6.1 Time Complexity

- **Row append**: O(1) amortized (O(n) when capacity grows)
- **Row insert**: O(n) where n is number of rows after insertion point
- **Row remove**: O(n) where n is number of rows after removal point
- **Column insert**: O(n×m) where n is number of rows and m is number of columns after insertion point
- **Normalization**: O(n×m) where n is number of rows and m is average number of columns
- **Validation**: O(n×m) where n is number of rows and m is average number of columns

#### 9.6.2 Space Complexity

- **Table overhead**: O(1) per table (fixed overhead for structure)
- **Row storage**: O(n) where n is number of rows
- **Field storage**: O(n×m) where n is number of rows and m is average number of columns
- **Arena allocation**: All data allocated from single arena (efficient memory management)

#### 9.6.3 Optimization Opportunities

- **Normalization optimization**: No-op if table already normalized to target count
- **Column count recalculation**: Only recalculates when necessary (removed row had maximum field count)
- **Arena compaction**: `gtext_csv_table_compact()` can be called to free unused memory

---

## 10. Streaming Parser Events

The streaming parser emits events for:

- **Structure events**: `RECORD_BEGIN`, `RECORD_END`, `END`
- **Data events**: `FIELD` (with field data and length)

Each event includes position information (row index, column index) for error reporting and context.

---

## 11. In-Situ / Zero-Copy Parsing

An optional in-situ parsing mode allows the DOM to reference slices of the input buffer directly, avoiding copies for fields that don't require transformation. This mode requires the input buffer to remain valid for the lifetime of the DOM.

**When In-Situ Mode Works:**

In-situ mode is used for a field when **all** of the following conditions are met:

1. **`in_situ_mode` is enabled** in parse options
2. **`validate_utf8` is disabled** (`false`) — UTF-8 validation requires copying
3. **Single-chunk parsing** — the entire CSV data is provided in one `gtext_csv_parse_table()` call
4. **No transformation needed** — the field doesn't require unescaping (no doubled quotes or backslash escapes)
5. **Field not buffered** — the field didn't span chunks or contain newlines in quotes

**When In-Situ Mode Falls Back to Copying:**

Fields are automatically copied to arena-allocated memory when:

- UTF-8 validation is enabled
- Field requires unescaping (doubled quotes or backslash escapes)
- Field was buffered (spans chunks or contains newlines in quotes)
- Multi-chunk streaming parsing (fields are buffered)

**Lifetime Requirements:**

⚠️ **Critical**: When in-situ mode is enabled, the input buffer (`data` parameter to `gtext_csv_parse_table()`) **must remain valid for the entire lifetime of the table**. The table may contain pointers directly into this buffer.

**Example Usage:**

```c
// Input buffer that will remain valid
const char* csv_data = "Name,Age\nJohn,30\nJane,25";
size_t csv_len = strlen(csv_data);

// Enable in-situ mode
gtext_csv_parse_options opts = gtext_csv_parse_options_default();
opts.in_situ_mode = true;
opts.validate_utf8 = false;  // Required for in-situ mode

gtext_csv_table* table = gtext_csv_parse_table(csv_data, csv_len, &opts, NULL);

// Fields may point directly into csv_data
const char* name = gtext_csv_field(table, 0, 0, NULL);

// CRITICAL: csv_data must remain valid until table is freed
gtext_csv_free_table(table);
```

---

## 12. Error Reporting

The library provides comprehensive error information:

- **Error codes**: Stable error codes for programmatic handling
- **Human-readable messages**: Descriptive error messages
- **Position information**: Byte offset, line number, and column number
- **Row/column indices**: Row and column indices (0-based)
- **Enhanced diagnostics** (optional):
  - Context snippet showing the error location
  - Caret positioning within the snippet

Error context snippets are dynamically allocated and must be freed via `gtext_csv_error_free()`.

Common error cases:

- Unterminated quoted field (EOF inside quotes)
- Invalid escape sequence (for backslash-escape dialect)
- Unexpected quote in unquoted field (when disallowed)
- Record/field size limit exceeded
- UTF-8 validation failure (when enabled)
- Too many columns/rows

---

## 13. Round-Trip Correctness

The library guarantees round-trip stability: when parsing and writing with the same dialect, parse → write → parse results in identical field values. This is critical for applications that need to preserve exact CSV data.

**Exception: Write Trimming**

⚠️ **Note**: When `trim_trailing_empty_fields` is enabled in write options, trailing empty fields are not written, which breaks round-trip preservation for those fields. If you need exact round-trip preservation, keep `trim_trailing_empty_fields` set to `false` (the default). See Section 5.3 for details.

---

## 14. Design Philosophy

The library prioritizes **correctness over simplicity**:

1. **Strict by default**: Strict CSV compliance is the default; extensions are explicit opt-ins
2. **Dialect is part of correctness**: Parsing without a dialect is undefined; safe defaults are provided
3. **Streaming-first capability**: Handle huge inputs with bounded memory
4. **No hidden allocations**: Explicit arena ownership and clear lifetimes
5. **Round-trip stability**: Output must re-parse to the same fields under the same dialect
6. **Precise errors**: Make it obvious *where* and *why* parsing failed

---

## 15. Getting Started

Include the umbrella header:

```c
#include <ghoti.io/text/csv.h>
```

For fine-grained control, include specific headers:

```c
#include <ghoti.io/text/csv/csv_core.h>   // Core types and options
#include <ghoti.io/text/csv/csv_table.h>  // DOM parsing and manipulation
#include <ghoti.io/text/csv/csv_stream.h> // Streaming parser
#include <ghoti.io/text/csv/csv_writer.h> // Writer
```

Comprehensive usage examples are provided in the `examples/` directory.

---

## 16. Not in this module

Dialect presets, `gtext_csv_sniff()`, the pull reader and
`GTEXT_CSV_Parse_Options::allocator` are implemented. What this module does
not do, on purpose:

- **RFC 7111 fragment identifiers.** A URI feature, not a parse.
- **Type inference.** Fields are bytes. Nothing converts them to numbers or dates.
