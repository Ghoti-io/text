@page text_modules Modules


This section contains detailed documentation for each module in the Ghoti.io
Text library. These pages document the **API**: the types, the functions, the
options and what they do.

@subpage core_module

@subpage json_module

@subpage csv_module

@subpage yaml_module

@subpage toml_module

@subpage ini_module

## Format conformance

The module pages above describe how to call the library. \ref text_format_references
"Format and specification references" describes what the library will do with
a given byte sequence: which specification each parser implements, which
clauses, where it deviates from other parsers, and what evidence backs each
claim.

- \ref format_json "JSON" — RFC 8259, plus Pointer, JSONPath, Patch, Merge Patch and Schema
- \ref format_csv "CSV" — RFC 4180 and the dialect options
- \ref format_yaml "YAML" — YAML 1.2.2
- \ref format_toml "TOML" — TOML v1.0.0
- \ref format_ini "INI (Desktop Entry)" — Desktop Entry Specification 1.5
- \ref text_format_adding "Adding a format" — the checklist for a new parser

## Quick Links

- [Function Index](@ref text_functions_index) - Browse all library functions
- [Examples](@ref text_examples) - Example programs demonstrating library usage
