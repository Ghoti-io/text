@page format_encoding Encoding

# Encoding

The **Encoding Standard** (WHATWG, https://encoding.spec.whatwg.org/) says what the names of character encodings are, which names mean which
encoding, and how bytes become characters and characters become bytes in each of them - for the web, and for everything that has to agree with the
web: HTML's encoding sniffing, forms, URLs, `TextDecoder`. This module implements it: `gtext_encoding_for_label()` (the label table),
`GTEXT_Decoder` (a streaming decoder to UTF-8) and `GTEXT_Encoder` (a streaming encoder from UTF-8), with the standard's error handling, in
`<ghoti.io/text/encoding.h>`. It stands alone: nothing in it reads a locale, calls `iconv` or guesses from the bytes. Back to
\ref text_format_references "Formats".

**This release reads and writes** UTF-8, UTF-16LE and UTF-16BE (a decoder only), `replacement` (a decoder only), `x-user-defined` and all 28
single-byte encodings (`ibm866`, ISO-8859-2 to -16, `koi8-r`, `koi8-u`, `macintosh`, `windows-874`, `windows-1250` to `windows-1258`,
`x-mac-cyrillic`, and ISO-8859-8-I). **The seven multi-byte legacy encodings** - GBK, gb18030, Big5, EUC-JP, ISO-2022-JP, Shift_JIS and EUC-KR - are
the next stage: their labels resolve and their names print, and a decoder or an encoder for one is `GTEXT_ENCODING_E_UNSUPPORTED`
(`gtext_encoding_is_supported()` says in advance). HTML's encoding sniffing and its use of this module are the stage after that.

## Quick start

```c
#include <ghoti.io/text/encoding.h>
#include <stdio.h>
#include <string.h>

/* A page labelled "latin1" holds the bytes of "café €5". The standard's latin1 is windows-1252, in which 0x80 is the euro sign. */
static void quick_start(void) {
  const char * label = " Latin1 ";   /* white space around a label is ignored, and so is case */
  const char * bytes = "caf\xE9 \x80" "5";

  GTEXT_Encoding encoding = gtext_encoding_for_label(label, strlen(label));
  if (encoding == GTEXT_ENCODING_NONE) {
    printf("no such encoding\n");
    return;
  }
  printf("%s\n", gtext_encoding_name(encoding));

  char * utf8 = NULL;
  size_t length = 0;
  GTEXT_Decoder_Result result;
  if (gtext_encoding_decode_buffer(encoding, NULL, bytes, strlen(bytes), &utf8, &length, &result) != GTEXT_ENCODING_OK) {
    printf("not decoded: %s\n", result.message);
    return;
  }
  printf("%s (%zu bytes of UTF-8, %zu replaced)\n", utf8, length, result.replacements);
  gtext_encoding_buffer_free(utf8);
}
```

`gtext_encoding_for_label(label, len)` is the standard's "get an encoding": the white space around the label is removed, the rest is compared
without regard to ASCII case with the 228 labels of the standard's table, and anything else is `GTEXT_ENCODING_NONE`. Note what it said: `latin1` is **windows-1252**.
On the web, and in any software that wants to agree with it, the labels `latin1`, `iso-8859-1` and `ascii` all name windows-1252, in which `0x80` is the euro sign,
not a control character. `gtext_encoding_decode_buffer()` is the one-shot form: it returns a NUL-terminated UTF-8 buffer that
`gtext_encoding_buffer_free()` releases (with the allocator that made it), and the result says what happened.

## Labels

The table is the standard's section 4.2: 40 encodings, 228 labels. The rules of the lookup are the standard's and are tested against it:

- **Leading and trailing ASCII white space is removed**: U+0009, U+000A, U+000C, U+000D and U+0020. The Infra standard: "ASCII whitespace is U+0009 TAB,
  U+000A LF, U+000C FF, U+000D CR, or U+0020 SPACE." A vertical tab (U+000B) is not, and neither are U+00A0, U+2003 or a byte order mark; `"\vutf-8"` names nothing.
- **ASCII case is ignored and nothing else is**: `UTF-8`, `Utf-8` and `utf-8` are one label; the Kelvin sign is not a `k`.
- **An unknown label, an empty one, and one with an embedded NUL are `GTEXT_ENCODING_NONE`.** The length is the argument, not a terminator.
- **A label for `replacement` resolves.** `hz-gb-2312`, `iso-2022-cn`, `iso-2022-cn-ext`, `iso-2022-kr`, `csiso2022kr` and `replacement` name the encoding
  `replacement`, whose decoder yields one U+FFFD for the whole stream (so that a page in one of them cannot be used to smuggle markup past a filter that
  reads another encoding). The Encoding API's `TextDecoder` refuses these labels - "If encoding is failure or replacement, then throw a RangeError." - but that is the
  constructor of an API, not the lookup, and HTML needs the lookup to say `replacement`.
- **`utf-16` is UTF-16LE**, as the standard has it, to match deployed content. There is no UTF-32 and no UTF-7: `utf-32` and `utf-7` are unknown labels.
- Every name lowercased is one of its labels. `gtext_encoding_name()` gives the standard's spelling (`UTF-8`, `windows-1252`, `Shift_JIS`, `gb18030`).

`gtext_encoding_is_ascii_compatible()` is false for UTF-16BE, UTF-16LE, ISO-2022-JP and `replacement` and true for the rest (`x-user-defined` included);
`gtext_encoding_output_encoding()` is the standard's "get an output encoding" - UTF-8 for `replacement` and the UTF-16 encodings, so that a caller that
reads a label and writes with it (URL parsing, form submission) asks for an encoder that exists. `gtext_encoding_has_encoder()` says which encodings have an encoder in this release (UTF-8, the single-byte encodings and `x-user-defined`; not `replacement`, not UTF-16, not yet the multi-byte ones). `gtext_encoding_label_count()` and `gtext_encoding_label_at()` list the table, in order.

## Decoding

A `GTEXT_Decoder` reads the bytes of one encoding in pieces of any size and gives UTF-8 to a **sink**: a function called with each chunk of output (of up to 4 KiB, cut only
between code points). The streaming calls allocate nothing; making the decoder does, and so does the one-shot buffer form, for its output.

```c
#include <ghoti.io/text/encoding.h>
#include <stdio.h>
#include <string.h>

/* A sink is a function that is given each chunk of output. This one prints it. */
static bool print_chunk(void * user, const char * bytes, size_t length) {
  (void)user;
  printf("[%.*s]", (int)length, bytes);
  return true;
}

/* UTF-8 arrives in pieces cut anywhere, here in the middle of the euro sign. The output is the same however it is cut. */
static void streaming(void) {
  const char piece1[] = "price \xE2\x82";
  const char piece2[] = "\xAC 5";
  const GTEXT_Encoding_Sink sink = {print_chunk, NULL};

  GTEXT_Decoder * decoder = NULL;
  if (gtext_decoder_new(GTEXT_ENCODING_UTF_8, NULL, &decoder) != GTEXT_ENCODING_OK) {
    return;
  }
  gtext_decoder_decode(decoder, piece1, sizeof(piece1) - 1, false, &sink, NULL);   /* not the end: the euro sign is held back */
  gtext_decoder_decode(decoder, piece2, sizeof(piece2) - 1, true, &sink, NULL);    /* the end */
  printf("\n");
  gtext_decoder_free(decoder);
}
```

- **Cutting the input never changes the output.** A sequence may be split between two calls - between the bytes of a UTF-8 character, between the two bytes of a UTF-16 unit,
  after the first byte of a byte order mark - and the output is what it would have been had the input come whole. This is tested at every way of cutting every short input, and by feeding long ones a byte at a time (see Measured).
- **`flush` is the end of the input**, the standard's end-of-queue. The call decodes what was held back (a truncated sequence is an error, a held byte order mark is text) and the decoder
  starts over, so one decoder can read any number of streams. **After a failure it starts over as well** (an argument error, `GTEXT_ENCODING_E_INVALID`, is the exception: it is a caller's mistake and does not touch the stream): the Encoding API's decoder is specified so, and the standard's tests check
  that "error seen with fatal does not prevent future decodes". `gtext_decoder_reset()` abandons a stream in progress.
- **Error handling is the standard's "replacement" mode, or "fatal".** A sequence that is not well-formed becomes U+FFFD - for UTF-8 *one for each maximal subpart of an ill-formed
  sequence* (the Unicode standard's "best practice", which the Encoding Standard makes the only permitted behaviour: `E0 80 80` is three, `F0 9F` at the end of the input is one, and an ASCII
  byte is never swallowed by an error), for UTF-16 one for each unpaired surrogate and one for a byte left over at the end (a leading surrogate and a byte after it are one error, not two) - and is counted in
  `result.replacements`. In fatal mode the call stops with `GTEXT_ENCODING_E_MALFORMED`, and `result.error_offset` is the offset in the stream of the byte at which the error was found
  (or the stream's length, if it ended in the middle of a sequence):

```c
#include <ghoti.io/text/encoding.h>
#include <stdio.h>
#include <string.h>

/* The standard's default is to replace what is not well-formed with U+FFFD. Fatal mode stops instead, and says where. */
static void fatal(void) {
  const char * bytes = "ok \xE2\x82 oops";   /* a truncated euro sign */
  char * utf8 = NULL;
  size_t length = 0;
  GTEXT_Decoder_Result result;

  GTEXT_Decoder_Options options = gtext_decoder_options_default();
  GTEXT_Encoding_Status status = gtext_encoding_decode_buffer(GTEXT_ENCODING_UTF_8, &options, bytes, strlen(bytes), &utf8, &length, &result);
  printf("replacement mode: %s, %zu replaced\n", gtext_encoding_status_string(status), result.replacements);
  gtext_encoding_buffer_free(utf8);

  options.error_mode = GTEXT_DECODER_FATAL;
  status = gtext_encoding_decode_buffer(GTEXT_ENCODING_UTF_8, &options, bytes, strlen(bytes), &utf8, &length, &result);
  printf("fatal mode: %s at byte %zu\n", gtext_encoding_status_string(status), result.error_offset);
}
```

- **The single-byte encodings** decode a byte below 0x80 as itself and a byte above by the standard's index (`indexes.json`): 27 indexes for 28 encodings, ISO-8859-8-I using ISO-8859-8's.
  Some indexes leave bytes without a code point (ISO-8859-3 has 7, ISO-8859-6 45, ISO-8859-7 3, ISO-8859-8 36, windows-874 8, windows-1253 3, windows-1255 10, windows-1257 2): those are errors, so U+FFFD or fatal.
  The others are total, and **windows-1252 maps 0x81, 0x8D, 0x8F, 0x90 and 0x9D to the C1 controls U+0081 and so on** - there is no gap, unlike in Python's `cp1252`.
- **`replacement`** decodes any non-empty stream as a single U+FFFD, and an empty one as nothing. **`x-user-defined`** maps 0x80 to 0xFF to U+F780 to U+F7FF and is the only decoder that never fails.
- **Byte order marks** are the standard's three variants, as two options. `bom_sniffing` is "decode": a UTF-8 (`EF BB BF`), UTF-16BE (`FE FF`) or UTF-16LE (`FF FE`) mark at the start of the stream
  chooses the encoding, whatever was asked for, and is taken out of the text; the encoding asked for is the one to use if there is none, and `result.encoding` and `result.bom_found` say what was done.
  `bom_removal` alone is "UTF-8 decode" and the Encoding API's default: a U+FEFF that is the first thing a UTF-8, UTF-16LE or UTF-16BE decoder writes is dropped (once - a second one is text, and so is
  one after an error). Neither is "UTF-8 decode without BOM", and the default, so a decoder you did not ask to treat a mark specially passes it through as U+FEFF:

```c
#include <ghoti.io/text/encoding.h>
#include <stdio.h>
#include <string.h>

/* With bom_sniffing a byte order mark chooses the encoding and is not part of the text: the standard's "decode". */
static void bom(void) {
  const char bytes[] = "\xFF\xFE" "h\0i\0";   /* UTF-16LE, with its mark */
  GTEXT_Decoder_Options options = gtext_decoder_options_default();
  options.bom_sniffing = true;

  char * utf8 = NULL;
  size_t length = 0;
  GTEXT_Decoder_Result result;
  /* The encoding asked for is the one to use when there is no mark. */
  if (gtext_encoding_decode_buffer(GTEXT_ENCODING_WINDOWS_1252, &options, bytes, sizeof(bytes) - 1, &utf8, &length, &result) == GTEXT_ENCODING_OK) {
    printf("%s, from a byte order mark: %d, text: %s\n", gtext_encoding_name(result.encoding), result.bom_found, utf8);
    gtext_encoding_buffer_free(utf8);
  }
}
```

- **Limits.** `max_output_bytes` (0 selects the default of 256 MiB; it never means unlimited, and `SIZE_MAX` is how to remove the cap) bounds the output of one stream, over all its calls. Exceeding it is `GTEXT_ENCODING_E_LIMIT_OUTPUT` - its own status -
  and the sink has then received the longest prefix of the output that ends between code points and is within the limit, and no more:

```c
#include <ghoti.io/text/encoding.h>
#include <stdio.h>
#include <string.h>

/* The output of a stream has a limit, 256 MiB unless the options say otherwise; 0 is never "no limit". */
static void limits(void) {
  const char * bytes = "\xC3\xA9\xC3\xA9\xC3\xA9";   /* three letters e with an acute accent */
  GTEXT_Decoder_Options options = gtext_decoder_options_default();
  options.max_output_bytes = 5;

  char * utf8 = NULL;
  size_t length = 0;
  GTEXT_Decoder_Result result;
  GTEXT_Encoding_Status status = gtext_encoding_decode_buffer(GTEXT_ENCODING_UTF_8, &options, bytes, strlen(bytes), &utf8, &length, &result);
  /* The sink would have had the first two letters (4 bytes), never half of the third. */
  printf("%s after %zu bytes of output\n", gtext_encoding_status_string(status), result.output_bytes);
}
```

- **`result.input_bytes` says where a failure was found**: the number of bytes of the call's input up to and including the one at which the limit or the sink failed (or the whole input, if what failed was the last delivery), exact inside a run of ASCII as anywhere. `result.steps` counts the handler steps of the call, the work, at most four per input byte.
- **The sink may refuse** (return `false`): the call stops with `GTEXT_ENCODING_E_WRITE`, the sink is not called again, and the decoder is ready for the next stream. Sinks get what was produced before a failure, too.
- **Work is linear**, with no recursion: each byte is looked at a bounded number of times (an error puts back at most four bytes, and each is put back once), and runs of ASCII are copied without
  a function call per byte. Measured once, by hand, on the development machine with other work running (not asserted by any test): about 3.9 GB/s of ASCII in UTF-8 or windows-1252, 220 MB/s of three-byte UTF-8 characters, 300 MB/s of UTF-16, 150 MB/s of windows-1252
  high bytes.

## Encoding

A `GTEXT_Encoder` takes UTF-8 and writes the bytes of an encoding: UTF-8, the 28 single-byte encodings and `x-user-defined`. `replacement` and the UTF-16 encodings have **no encoder** in the standard
(`GTEXT_ENCODING_E_NO_ENCODER`); apply `gtext_encoding_output_encoding()` to a label's encoding first, as a form or a URL does.

```c
#include <ghoti.io/text/encoding.h>
#include <stdio.h>
#include <string.h>

/* The input is UTF-8. A code point the encoding cannot write is, in HTML mode, a numeric character reference: what an HTML form sends. */
static void encode(void) {
  const char * text = "price \xE2\x82\xAC" "5";   /* "price €5" */
  GTEXT_Encoder_Options options = gtext_encoder_options_default();
  options.error_mode = GTEXT_ENCODER_HTML;

  char * bytes = NULL;
  size_t length = 0;
  GTEXT_Encoder_Result result;
  /* ISO-8859-2 has no euro sign ... */
  if (gtext_encoding_encode_buffer(GTEXT_ENCODING_ISO_8859_2, &options, text, strlen(text), &bytes, &length, &result) == GTEXT_ENCODING_OK) {
    printf("ISO-8859-2: %s (%zu written as a reference)\n", bytes, result.replacements);
    gtext_encoding_buffer_free(bytes);
  }
  /* ... and windows-1252, which is what the label "iso-8859-1" names, has it at 0x80. */
  if (gtext_encoding_encode_buffer(GTEXT_ENCODING_WINDOWS_1252, &options, text, strlen(text), &bytes, &length, &result) == GTEXT_ENCODING_OK) {
    printf("windows-1252: the euro sign is byte 0x%02X, %zu references\n", (unsigned char)bytes[6], result.replacements);
    gtext_encoding_buffer_free(bytes);
  }
  /* The default mode refuses what cannot be written. */
  options.error_mode = GTEXT_ENCODER_FATAL;
  GTEXT_Encoding_Status status = gtext_encoding_encode_buffer(GTEXT_ENCODING_ISO_8859_2, &options, text, strlen(text), &bytes, &length, &result);
  printf("fatal: %s, U+%04X at byte %zu\n", gtext_encoding_status_string(status), (unsigned)result.error_code_point, result.error_offset);
}
```

- **Error modes.** A code point the encoding cannot write is, by `error_mode`: `GTEXT_ENCODER_FATAL` (the default, because the other two lose information without saying so) `GTEXT_ENCODING_E_UNENCODABLE`, with the code point and its offset in the result;
  `GTEXT_ENCODER_HTML` (the standard's "html" mode, what HTML forms do) `&#`, the code point in decimal, `;`; or `GTEXT_ENCODER_REPLACEMENT`, `?`. **The last is not a mode of the standard**, whose encoders are only run in
  "html" or "fatal" mode (its processing steps assert it); it is the replacement every other encoding API offers, and is here because callers ask for it. Note what HTML mode cannot do: `&#8364;` written
  to a page is indistinguishable from a page that said so, which the standard warns of ("silent data loss").
- **The input is UTF-8 and need not be well-formed.** The standard's encoders take scalar values; UTF-8 that is not well-formed - a stray byte, an overlong form, an encoded surrogate, a truncated tail - is handled as the Encoding API
  handles a JavaScript string that is not: each maximal subpart becomes U+FFFD (then encoded, or written as a reference, like any other code point). In fatal mode it is `GTEXT_ENCODING_E_MALFORMED` instead.
  A cut between the bytes of a character is not an error.
- **Which byte a code point is** is "the index pointer for a code point": the *first* pointer whose entry is that code point, and the generated tables are searched by binary search. No single-byte index repeats a code point, so there is no choice to make today; the multi-byte stage inherits the rule.
- **Limits, sinks and failures** are those of the decoder, and the limit never ends inside a character or a reference.

## Statuses

| Status | When |
|---|---|
| `GTEXT_ENCODING_OK` | success |
| `GTEXT_ENCODING_E_INVALID` | an argument is unusable: NULL where a pointer is required, data NULL with a length, an encoding that is none |
| `GTEXT_ENCODING_E_OOM` | out of memory; nothing is leaked, and a buffer form returns no buffer |
| `GTEXT_ENCODING_E_OPTIONS` | an error mode that is none of the enum |
| `GTEXT_ENCODING_E_UNSUPPORTED` | an encoding of the standard this release has no codec for (the seven multi-byte ones) |
| `GTEXT_ENCODING_E_NO_ENCODER` | an encoder for `replacement` or a UTF-16 encoding |
| `GTEXT_ENCODING_E_MALFORMED` | fatal mode: input that is not well-formed in the encoding (decoder) or not UTF-8 (encoder) |
| `GTEXT_ENCODING_E_UNENCODABLE` | fatal mode: a code point the encoding cannot represent |
| `GTEXT_ENCODING_E_LIMIT_OUTPUT` | more than `max_output_bytes` of output; the sink has a prefix |
| `GTEXT_ENCODING_E_WRITE` | the sink returned `false` |
| `GTEXT_ENCODING_E_INTERNAL` | a defect of this library (a codec asked the drivers for more than they give); never content; please report it |

Zeroed options are the defaults. The options structs have no size field, as none of this library's do: a field may be added when the library's version changes (the symbol namespace changes with it), and a program is built against the headers of the version it runs with. Decoders and encoders are not shared between threads;
any number may be used at once, one to a thread, and the label table is read-only data. The allocator option applies to the decoder or encoder and to the buffer of the one-shot form; the streaming calls
allocate nothing.

## How this differs from iconv, Python, Node and the web

| | The Encoding Standard (this module) | Elsewhere |
|---|---|---|
| `latin1`, `iso-8859-1`, `ascii` | all windows-1252: byte 0x80 is U+20AC | Python and glibc: ISO-8859-1 and ASCII proper (0x80 is U+0080, or an error). **Node 20.19.2 decodes `latin1` as ISO-8859-1** (see below). |
| Bytes a legacy code page leaves undefined | windows-1252 0x81, 0x8D, 0x8F, 0x90, 0x9D are U+0081 etc.; windows-125x has gaps only where the index does | Python's `cp1250` to `cp1258` and `cp874` leave between none (cp1256) and 31 bytes undefined (cp874; cp1255 23, cp1253 17, cp1257 12, cp1258 9, cp1254 7, cp1250 and cp1252 5, cp1251 1) with a `UnicodeDecodeError`, **of which the standard defines 1 to 23** (cp874 23, cp1253 14, cp1255 13, cp1257 10, cp1258 9, cp1254 7, cp1250 and cp1252 5, cp1251 1: the rest are undefined in the index too). Measured with Python 3.13.5. |
| Errors in UTF-8 | the Unicode "maximal subpart" rule, one U+FFFD each, never swallowing ASCII | Python since 3.3 agrees; many libraries and older Node/ICU did not (the standard's tests include the cases). |
| UTF-16 at the end | an odd byte, or a leading surrogate, is one error; a leading surrogate plus a byte is also one | Python agrees (one `truncated data` error). |
| Labels | 228, ASCII white space trimmed, ASCII case folded | iconv: whatever the system knows, locale-dependent. Python: its own alias table (`latin-1` yes, `hz-gb-2312` as a real codec). |
| `replacement`, `x-user-defined` | decoders in the standard | not in iconv or Python; Node refuses them (RangeError, and no ICU converter for `x-user-defined`). |
| Byte order marks | a decode option; the mark is never guessed from content | iconv's UTF-16 and Python's `utf-16` consume one and pick the order from it; neither detects UTF-8 BOMs unless asked. |
| An encoder meeting a code point it cannot write | `&#N;` (the web's forms), `?`, or an error | iconv: `//TRANSLIT` is locale-dependent; Python: `errors=` handlers such as `xmlcharrefreplace` (hexadecimal or decimal by version). |

The differences from the two references that were *measured* (Node's `TextDecoder` and Python's codecs) are the 31 classes of `tools/oracle/encoding_divergences.txt`, each with the sentence or the index entry that
decides it; the table of them is under "Measured".

## What is not here yet, and how it arrives

The multi-byte encodings (GBK, gb18030, Big5, EUC-JP, ISO-2022-JP, Shift_JIS, EUC-KR) are the next stage, and **the interface they need is already the one above**: the public API names no algorithm, only encodings, and
`GTEXT_Encoding`'s values for all 40 encodings, `gtext_encoding_is_supported()`, `GTEXT_ENCODING_E_UNSUPPORTED` and the state of a decoder or encoder being private are what the stage fills in. What it adds is internal: a row for each encoding
in `src/encoding/` - a decoder handler and an encoder handler written as the standard writes them (a byte in, "continue", one or two code points, an error that may put up to four bytes back; a code point in, bytes, an error, or "the
same code point again"), their indexes generated by `tools/encoding/gen_tables.py` from the `indexes.json` that is already pinned - and the removal of `E_UNSUPPORTED` for them. No public function, type or option changes, no caller
changes, and the drivers (chunking, the two error modes, the limit, the byte order mark options, the ASCII fast path) are shared. HTML's use of the module (the `<meta>` prescan, the BOM sniff of the standard's "decode" with a
transport label, `Content-Type`, the parse of a document in another encoding) is the stage after, and the HTML parser's labels (`UTF-8` only today) will then be this module's.

## Measured

The numbers below are what the gates print, and the ones a list file or a test fixes (`encoding_wpt_expected.txt`, `encoding_divergences.txt`, the `EXPECT`s of the tests) are asserted there; the figures for throughput under "Decoding" were measured once, by hand, on the development machine (an x86-64 Linux box with other work running) and are not asserted by anything. The commands are in the last section.

- **The tables.** 40 encodings, 228 labels (the table of section 4.2 as served on 2026-10-12, revision 21 May 2026, is identical to `encodings.json`), 27 single-byte indexes of 128 entries; generated by `tools/encoding/gen_tables.py` from `encodings.json` and `indexes.json`,
  pinned by SHA-256 (`tools/encoding/fetch.sh` fails and leaves no file if one changes); `make check-encoding-tables` fails when the committed tables are not what the generator writes.
- **Unit tests** (`tests/test-encoding-*.cpp`): every label in 14 spellings and 3,000 near-misses; **UTF-8 and both UTF-16 decoders against reference decoders written another way, over every one of the 16,843,008 sequences of one to three bytes** in replacement and fatal mode;
  every Unicode scalar value round trips; every byte of the 28 single-byte encodings against `indexes.json` read by the test, in both modes; every code point of the Basic Multilingual Plane through every single-byte encoder in the three error modes, against a model of the index; BOM options; fatal offsets;
  the output limit at every value; a sink that refuses every call; the allocator that fails every allocation of every entry point; thread safety; **chunk independence at every way of cutting every short input** (13.3 million decodes and 15.3 million encodes) and a byte at a time on long ones.
- **The standard's own tests.** The pinned web-platform-tests `encoding/` directory (`tools/encoding/WPT_COMMIT`), run by `make check-encoding-oracle` in Node against this library through a pair of pipes: **21 files, 11,325 tests; 11,249 pass, 76 are about the multi-byte encodings and are deferred to the next stage, none fail.**
- **Node and Python.** Node 20.19.2 with ICU 76.1 and Python 3.13.5, in the image `containers/IMAGES` pins as `encoding`. The differential compares the library with their decoders and encoders over **77 million requests**: the 228 labels in 14 spellings; every byte of every single-byte encoding in two modes; every code point of the BMP in each single-byte encoder (Python);
  **every sequence of one to three bytes in UTF-8, UTF-16LE and UTF-16BE (Node, replacement mode; fatal mode: all of one and two, one in 13 of three, and once all of them) - no difference**; 60,000 random and mutated strings cut at random places (Node, streaming); 15,000 UTF-8 encodes; byte order mark sniffing against Python; the encoders' modes.
  **There are 841 differences in 31 classes and the standard's own data decides every one in this library's favour** (`indexes.json` and `encodings.json` are read by the script and the library's tables are generated from them by a different program):

  | Reference | What it does differently | Where the standard says otherwise |
  |---|---|---|
  | Node | refuses the six `replacement` labels, ISO-8859-16 and x-user-defined (ICU has no converter for the last two) | the label table; section 14.5 |
  | Node | decodes `latin1`/windows-1252 as ISO-8859-1: 0x80 to 0x9F are C1 controls (27 bytes) | "any software following this standard will, for example, decode 0x80 as U+20AC (€) when asked for the \"Latin1\" or \"ASCII\" decoding of that byte." |
  | Node | ICU's IBM866 swaps 0x1A, 0x1C and 0x7F | "If byte is an ASCII byte, then return a code point whose value is byte." |
  | Node | KOI8-U 0xAE and 0xBE are box drawing | index koi8-u: U+045E, U+040E |
  | Node | windows-1253 0xAA, windows-1255 0xCA, windows-874 0xDB to 0xDE and 0xFC to 0xFF differ from the index (a code point where it has none, none where it has one, private use) | the indexes |
  | Node | with `ignoreBOM: true` drops a leading 0xFF of windows-1252 | the flag is read for UTF-8 and UTF-16 only |
  | Python | `cp1250` to `cp1258`, `cp874` leave 5 to 23 bytes undefined that the index maps to the C1 controls, and cannot encode those code points; `koi8_u` has box drawing where the index has U+045E and U+040E | the indexes |

  Nothing else differs: UTF-8, UTF-16, the label trim and case rules, `stream: true`, fatal mode and BOM removal all agree with Node, and the byte order mark sniffing agrees with Python's codecs.
- **The gates are made to fail.** `make check-encoding-oracle` also corrupts the runner, the counts, the answers, the list and the standard's data, and compiles variants of this library each with one rule undone (one for each line of `tools/oracle/ENCODING_ORACLE_PLANTS`) (a boundary of the UTF-8 decoder, the order of restored bytes, a table entry,
  the byte order mark logic, the label trim); every one must be caught, and is.
- **The fuzzer** `fuzz_encoding` (all 40 encodings and the values that are none, decoders and encoders, chunking, the three error modes, limits, a refusing sink and a failing allocator, against the reference decoders and a model of the encoders) has 980 seeds and 15 reproducers
  replayed by `make test` and 19 planted defects (13 replayed from recorded inputs, 6 searched for) that `make check-encoding-fuzz-plants` shows it finds.

## Departures from the standard

- The encoder's `GTEXT_ENCODER_REPLACEMENT` mode is an addition (see above). Its default mode is fatal; the standard's `encode` hook uses "html".
- The decoder and encoder take and give UTF-8 where the standard's algorithms work on code points; the standard's decoders return code points and so never a surrogate, and this module's output is therefore always well-formed UTF-8.
- "BOM removal" is the Encoding API's rule (the first code point written) applied to the three Unicode encodings, which for UTF-8 is the standard's "UTF-8 decode" hook and for UTF-16 is the API's.
- The Encoding API's argument handling (a label for `replacement` is a RangeError; a fatal error is a TypeError) is not here; the web-platform-tests runner supplies it in a few lines of JavaScript, and the library's side is the lookup, the decoder and the status.

## Specification

WHATWG Encoding Standard, Living Standard, last updated 21 May 2026 (retrieved 2026-10-12; commit snapshot a985b62a9b45c17da3e17a9f0a0b4e30c34c4a8a); `encodings.json` SHA-256 `078212b3...`, `indexes.json` SHA-256 `b7c89610...`. The WHATWG Infra Standard for
ASCII white space. The tests: web-platform-tests `encoding/` at the commit in `tools/encoding/WPT_COMMIT`.

To check it all: `make test` (unit tests, the table gate, the fuzz seeds, the examples above), `make check-encoding-oracle` (Node, Python and the standard's tests, with their controls), `make check-encoding-fuzz-plants`.
