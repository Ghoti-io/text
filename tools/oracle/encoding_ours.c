/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Text.
 *
 * Ghoti.io Text is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Text is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
/* This library's side of the Encoding differentials (tools/oracle/encoding_diff.py) and of the web-platform-tests
 * runner (tools/oracle/encoding_wpt.py, whose JavaScript shim talks to this through a pair of named pipes).
 *
 *   encoding-ours serve [<requests> <answers>]     one request per line from stdin / the named pipe, one answer per line
 *   encoding-ours enum <label> <n> <flags> <start> <count>   write the records of the decodes of the n-byte sequences
 *                                                  start .. start+count-1 (their bytes the big-endian digits of the number)
 *
 * Every string is hex, "-" for none. Flags are letters, "-" for none: f the decoder's fatal mode, s `bom_sniffing`,
 * r `bom_removal`. Requests:
 *
 *   L <label>                       the encoding a label names                    -> L <lowercase name | none>
 *   ND <id> <label> <flags>         make a decoder (ids are the caller's, 0..63)  -> OK <name> | ERR <status>
 *   D <id> <flush 0|1> <bytes>      decode the next piece                         -> OUT <utf8> <replacements> <mark 0|1>
 *                                                                                     | ERR <status> <offset> <utf8 so far>
 *   NE <id> <label> <mode F|H|R>    make an encoder                               -> OK <name> | ERR <status>
 *   E <id> <flush 0|1> <utf8>       encode the next piece                         -> OUT <bytes> <replacements>
 *                                                                                     | ERR <status> <offset> <code point> <bytes so far>
 *   FREE <id>                       release a decoder or an encoder
 *   BUF <label> <flags> <bytes>     a whole decode with the buffer form           -> as D
 *   LIMIT <label> <max> <bytes>     a whole decode with an output limit           -> as D
 *   PING                            -> PONG
 *
 * An enum record is one byte, the length of the output (255 for a failure in fatal mode), and the output.
 *
 * Copyright 2026 by Corey Pennycuff
 */
#include <ghoti.io/text/encoding.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char * status_name(GTEXT_Encoding_Status s) {
  switch (s) {
  case GTEXT_ENCODING_OK: return "OK";
  case GTEXT_ENCODING_E_INVALID: return "E_INVALID";
  case GTEXT_ENCODING_E_OOM: return "E_OOM";
  case GTEXT_ENCODING_E_OPTIONS: return "E_OPTIONS";
  case GTEXT_ENCODING_E_UNSUPPORTED: return "E_UNSUPPORTED";
  case GTEXT_ENCODING_E_NO_ENCODER: return "E_NO_ENCODER";
  case GTEXT_ENCODING_E_MALFORMED: return "E_MALFORMED";
  case GTEXT_ENCODING_E_UNENCODABLE: return "E_UNENCODABLE";
  case GTEXT_ENCODING_E_LIMIT_OUTPUT: return "E_LIMIT_OUTPUT";
  case GTEXT_ENCODING_E_WRITE: return "E_WRITE";
  case GTEXT_ENCODING_E_INTERNAL: return "E_INTERNAL";
  }
  return "E_UNKNOWN";
}

typedef struct {
  unsigned char * data;
  size_t len, cap;
} Bytes;

static void bytes_add(Bytes * b, const void * p, size_t n) {
  if (n == 0) {
    return; /* (memcpy from a null pointer, even of nothing, is undefined) */
  }
  if (b->len + n > b->cap) {
    b->cap = (b->len + n) * 2 + 64;
    b->data = realloc(b->data, b->cap);
    if (!b->data) {
      abort();
    }
  }
  memcpy(b->data + b->len, p, n);
  b->len += n;
}

static bool sink_write(void * user, const char * bytes, size_t len) {
  bytes_add(user, bytes, len);
  return true;
}

static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* A hex token, or "-" (empty). false if it is not hex. */
static bool unhex(const char * tok, Bytes * out) {
  out->len = 0;
  if (strcmp(tok, "-") == 0) {
    return true;
  }
  const size_t n = strlen(tok);
  if (n % 2) {
    return false;
  }
  for (size_t i = 0; i < n; i += 2) {
    const int a = hexval(tok[i]), b = hexval(tok[i + 1]);
    if (a < 0 || b < 0) {
      return false;
    }
    const unsigned char c = (unsigned char)(a * 16 + b);
    bytes_add(out, &c, 1);
  }
  return true;
}

static void put_hex(FILE * f, const unsigned char * p, size_t n) {
  if (n == 0) {
    fputc('-', f);
    return;
  }
  for (size_t i = 0; i < n; i++) {
    fprintf(f, "%02x", p[i]);
  }
}

static GTEXT_Decoder_Options decoder_flags(const char * flags) {
  GTEXT_Decoder_Options o = gtext_decoder_options_default();
  for (const char * p = flags; *p; p++) {
    if (*p == 'f') o.error_mode = GTEXT_DECODER_FATAL;
    if (*p == 's') o.bom_sniffing = true;
    if (*p == 'r') o.bom_removal = true;
  }
  return o;
}

static GTEXT_Decoder * g_dec[64];
static GTEXT_Encoder * g_enc[64];

static GTEXT_Encoding encoding_of_label(const char * hexlabel) {
  Bytes b = {0};
  if (!unhex(hexlabel, &b)) {
    free(b.data);
    return GTEXT_ENCODING_NONE;
  }
  const GTEXT_Encoding e = gtext_encoding_for_label((const char *)b.data, b.len);
  free(b.data);
  return e;
}

static void lowercase_name(GTEXT_Encoding e, char * out, size_t cap) {
  const char * n = gtext_encoding_name(e);
  size_t i = 0;
  for (; n && n[i] && i + 1 < cap; i++) {
    const char c = n[i];
    out[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
  }
  out[i] = 0;
}

static int run_enum(const char * label, int n, const char * flags, uint64_t start, uint64_t count) {
  Bytes lb = {0};
  unhex(label, &lb);
  const GTEXT_Encoding e = gtext_encoding_for_label((const char *)lb.data, lb.len);
  GTEXT_Decoder_Options o = decoder_flags(flags);
  GTEXT_Decoder * d = NULL;
  if (e == GTEXT_ENCODING_NONE || gtext_decoder_new(e, &o, &d) != GTEXT_ENCODING_OK) {
    fprintf(stderr, "encoding-ours enum: no decoder for the label\n");
    return 2;
  }
  Bytes out = {0};
  const GTEXT_Encoding_Sink sink = {sink_write, &out};
  Bytes rec = {0};
  for (uint64_t v = start; v < start + count; v++) {
    unsigned char in[8];
    for (int k = 0; k < n; k++) {
      in[k] = (unsigned char)(v >> (8 * (n - 1 - k)));
    }
    out.len = 0;
    const GTEXT_Encoding_Status s = gtext_decoder_decode(d, in, (size_t)n, true, &sink, NULL);
    if (s != GTEXT_ENCODING_OK) {
      const unsigned char x = 255;
      bytes_add(&rec, &x, 1);
    } else {
      const unsigned char len = (unsigned char)out.len;
      bytes_add(&rec, &len, 1);
      bytes_add(&rec, out.data, out.len);
    }
    if (rec.len > (1 << 20)) {
      fwrite(rec.data, 1, rec.len, stdout);
      rec.len = 0;
    }
  }
  fwrite(rec.data, 1, rec.len, stdout);
  fflush(stdout);
  gtext_decoder_free(d);
  return 0;
}

static void answer_decode(FILE * out, GTEXT_Encoding_Status s, const Bytes * b, const GTEXT_Decoder_Result * r) {
  if (s == GTEXT_ENCODING_OK) {
    fputs("OUT ", out);
    put_hex(out, b->data, b->len);
    fprintf(out, " %zu %d\n", r->replacements, r->bom_found ? 1 : 0);
  } else {
    fprintf(out, "ERR %s %zu ", status_name(s), r->error_offset);
    put_hex(out, b->data, b->len);
    fputc('\n', out);
  }
}

static int serve(FILE * in, FILE * out) {
  char * line = NULL;
  size_t cap = 0;
  while (getline(&line, &cap, in) > 0) {
    char * tok[8];
    int nt = 0;
    for (char * p = strtok(line, " \r\n"); p && nt < 8; p = strtok(NULL, " \r\n")) {
      tok[nt++] = p;
    }
    if (nt == 0) {
      continue;
    }
    const char * cmd = tok[0];
    if (strcmp(cmd, "PING") == 0) {
      fputs("PONG\n", out);
    } else if (strcmp(cmd, "L") == 0 && nt == 2) {
      const GTEXT_Encoding e = encoding_of_label(tok[1]);
      char name[32];
      lowercase_name(e, name, sizeof name);
      fprintf(out, "L %s\n", e == GTEXT_ENCODING_NONE ? "none" : name);
    } else if (strcmp(cmd, "ND") == 0 && nt == 4) {
      const int id = atoi(tok[1]);
      const GTEXT_Encoding e = encoding_of_label(tok[2]);
      if (id < 0 || id >= 64 || e == GTEXT_ENCODING_NONE) {
        fputs("ERR E_INVALID\n", out);
      } else {
        gtext_decoder_free(g_dec[id]);
        g_dec[id] = NULL;
        const GTEXT_Decoder_Options o = decoder_flags(tok[3]);
        const GTEXT_Encoding_Status s = gtext_decoder_new(e, &o, &g_dec[id]);
        if (s == GTEXT_ENCODING_OK) {
          char name[32];
          lowercase_name(e, name, sizeof name);
          fprintf(out, "OK %s\n", name);
        } else {
          fprintf(out, "ERR %s\n", status_name(s));
        }
      }
    } else if (strcmp(cmd, "D") == 0 && nt == 4) {
      const int id = atoi(tok[1]);
      Bytes in_bytes = {0};
      if (id < 0 || id >= 64 || !g_dec[id] || !unhex(tok[3], &in_bytes)) {
        fputs("ERR E_INVALID 0 -\n", out);
      } else {
        Bytes got = {0};
        const GTEXT_Encoding_Sink sink = {sink_write, &got};
        GTEXT_Decoder_Result r;
        const GTEXT_Encoding_Status s = gtext_decoder_decode(g_dec[id], in_bytes.data, in_bytes.len, atoi(tok[2]) != 0, &sink, &r);
        answer_decode(out, s, &got, &r);
        free(got.data);
      }
      free(in_bytes.data);
    } else if ((strcmp(cmd, "BUF") == 0 && nt == 4) || (strcmp(cmd, "LIMIT") == 0 && nt == 4)) {
      const bool limit = cmd[0] == 'L';
      const GTEXT_Encoding e = encoding_of_label(tok[1]);
      Bytes in_bytes = {0};
      if (e == GTEXT_ENCODING_NONE || !unhex(tok[3], &in_bytes)) {
        fputs("ERR E_INVALID 0 -\n", out);
      } else {
        GTEXT_Decoder_Options o = decoder_flags(limit ? "-" : tok[2]);
        if (limit) {
          o.max_output_bytes = (size_t)strtoull(tok[2], NULL, 10);
        }
        char * buf = NULL;
        size_t n = 0;
        GTEXT_Decoder_Result r;
        const GTEXT_Encoding_Status s = gtext_encoding_decode_buffer(e, &o, in_bytes.data, in_bytes.len, &buf, &n, &r);
        Bytes got = {0};
        if (s == GTEXT_ENCODING_OK) {
          bytes_add(&got, buf, n);
          gtext_encoding_buffer_free(buf);
        }
        answer_decode(out, s, &got, &r);
        free(got.data);
      }
      free(in_bytes.data);
    } else if (strcmp(cmd, "NE") == 0 && nt == 4) {
      const int id = atoi(tok[1]);
      const GTEXT_Encoding e = encoding_of_label(tok[2]);
      if (id < 0 || id >= 64 || e == GTEXT_ENCODING_NONE) {
        fputs("ERR E_INVALID\n", out);
      } else {
        gtext_encoder_free(g_enc[id]);
        g_enc[id] = NULL;
        GTEXT_Encoder_Options o = gtext_encoder_options_default();
        o.error_mode = tok[3][0] == 'H' ? GTEXT_ENCODER_HTML : tok[3][0] == 'R' ? GTEXT_ENCODER_REPLACEMENT : GTEXT_ENCODER_FATAL;
        const GTEXT_Encoding_Status s = gtext_encoder_new(gtext_encoding_output_encoding(e), &o, &g_enc[id]);
        if (s == GTEXT_ENCODING_OK) {
          char name[32];
          lowercase_name(gtext_encoding_output_encoding(e), name, sizeof name);
          fprintf(out, "OK %s\n", name);
        } else {
          fprintf(out, "ERR %s\n", status_name(s));
        }
      }
    } else if (strcmp(cmd, "E") == 0 && nt == 4) {
      const int id = atoi(tok[1]);
      Bytes in_bytes = {0};
      if (id < 0 || id >= 64 || !g_enc[id] || !unhex(tok[3], &in_bytes)) {
        fputs("ERR E_INVALID 0 0 -\n", out);
      } else {
        Bytes got = {0};
        const GTEXT_Encoding_Sink sink = {sink_write, &got};
        GTEXT_Encoder_Result r;
        const GTEXT_Encoding_Status s = gtext_encoder_encode(g_enc[id], (const char *)in_bytes.data, in_bytes.len, atoi(tok[2]) != 0, &sink, &r);
        if (s == GTEXT_ENCODING_OK) {
          fputs("OUT ", out);
          put_hex(out, got.data, got.len);
          fprintf(out, " %zu\n", r.replacements);
        } else {
          fprintf(out, "ERR %s %zu %u ", status_name(s), r.error_offset, (unsigned)r.error_code_point);
          put_hex(out, got.data, got.len);
          fputc('\n', out);
        }
        free(got.data);
      }
      free(in_bytes.data);
    } else if (strcmp(cmd, "FREE") == 0 && nt == 2) {
      const int id = atoi(tok[1]);
      if (id >= 0 && id < 64) {
        gtext_decoder_free(g_dec[id]);
        gtext_encoder_free(g_enc[id]);
        g_dec[id] = NULL;
        g_enc[id] = NULL;
      }
      fputs("OK\n", out);
    } else {
      fputs("ERR E_REQUEST\n", out);
    }
    fflush(out);
  }
  free(line);
  return 0;
}

int main(int argc, char ** argv) {
  if (argc >= 2 && strcmp(argv[1], "enum") == 0 && argc == 7) {
    const int n = atoi(argv[3]);
    if (n < 1 || n > 8) {
      fprintf(stderr, "encoding-ours enum: the length of a sequence is 1 to 8\n");
      return 2;
    }
    return run_enum(argv[2], n, argv[4], strtoull(argv[5], NULL, 10), strtoull(argv[6], NULL, 10));
  }
  if (argc >= 2 && strcmp(argv[1], "serve") == 0) {
    if (argc == 4) {
      FILE * in = fopen(argv[2], "r");
      FILE * out = fopen(argv[3], "w");
      if (!in || !out) {
        fprintf(stderr, "encoding-ours serve: cannot open the pipes\n");
        return 2;
      }
      return serve(in, out);
    }
    return serve(stdin, stdout);
  }
  fprintf(stderr, "usage: encoding-ours serve [<requests> <answers>] | enum <label> <n> <flags> <start> <count>\n");
  return 2;
}
