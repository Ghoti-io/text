/*
 * The Win32 profile-API reference driver, cross-compiled for win64 and run under
 * wine inside this image.
 *
 * **Two references in one program**, which is the whole reason this driver asks
 * three different questions instead of one. `GetPrivateProfileString`,
 * `GetPrivateProfileSection` and `GetPrivateProfileSectionNames` read the same
 * file and do not agree about it: a `;`-led line is absent from the section
 * enumeration and retrievable by name through the string API. So the harness
 * scores against both and states where they part.
 *
 * Protocol, on stdin:
 *
 *     DOC <len>\n<len bytes>      the document the following asks read
 *     ASK <n>\n                   then n lines, each one of:
 *         NAMES
 *         SECT <hex section>
 *         GET <hex section> <hex key>
 *
 * and on stdout, per DOC/ASK pair: `BEGIN`, n answer lines, `END`.
 *
 * **And a second, opposite direction**, which exists to answer a question about
 * the *corpus* rather than about a document:
 *
 *     MAKE <n>\n                  then n lines of
 *         SET <hex section> <hex key> <hex value>
 *     MAKEW <n>\n                 the same through the W entry points
 *
 * answered with `FILE <hex of the whole file>`.
 *
 * The reason is provenance. The corpus gate reads this machine's real `.ini`
 * files, and this machine has exactly two that a Windows application wrote - the
 * rest belong to freedesktop and Python, which makes them a valid population for
 * "do we agree about real bytes" and no population at all for "is the format used
 * this way". Files that a Windows application wrote cannot be conjured, but files
 * that **the profile API itself wrote** can: `WritePrivateProfileString` is the
 * other half of the same reference, and what it emits is a `.ini` file of exactly
 * the right provenance by construction. So this verb turns the reference into an
 * author, and the gate then asks our reader to read what the reference wrote.
 *
 * MAKEW is how a UTF-16LE `.ini` gets into the population at all. The `W` entry
 * points create a UTF-16LE file with a byte-order mark when the file does not yet
 * exist and the section name is passed as wide text, which is the encoding this
 * module refuses by default and decodes on request - and until this verb existed
 * that decode was asserted against a hand-built buffer rather than measured
 * against anything.
 *
 * Answers are hex. `.` is the empty string, `-` is "the API filled nothing",
 * `MISSING` is a GET whose key was not found. MISSING is decided by a one-byte
 * 0x01 default rather than by the return value, because the return value cannot
 * tell "absent" from "present and empty" - both copy zero characters.
 *
 * A NAMES or SECT answer is the API's buffer hexed **whole, NULs included**, and
 * split by the harness. An encoder that stopped at the first NUL could not tell
 * "no sections" from "one section whose name is empty", which is exactly the
 * question `[]` asks.
 *
 * The document is written to a file and read back through the API, because the
 * profile API has no string entry point: a file is what an `.ini` document is
 * here, with none of the choice configparser's two channels presented.
 *
 * Copyright 2026 by Corey Pennycuff
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <io.h>

#ifndef GTEXT_DRIVER_SHA
#define GTEXT_DRIVER_SHA "unknown"
#endif
#ifndef GTEXT_WINE_VERSION
#define GTEXT_WINE_VERSION "unknown"
#endif

#define VBUF 65536
#define SBUF 1048576

static char doc_path[MAX_PATH];

static int unhex(const char * in, char * out, int cap) {
  int n = 0;
  /* An empty argument still has to terminate the buffer. Returning 0 without
   * doing so left a caller passing an uninitialized stack buffer as a section
   * name, which read as MISSING in a warm process and as the right answer in a
   * cold one - a disagreement that looked like a profile cache and was this. */
  if (in[0] == '.' && (in[1] == 0 || in[1] == ' ' || in[1] == '\t' ||
                          in[1] == '\r' || in[1] == '\n')) {
    out[0] = 0;
    return 0;
  }
  for (; in[0] && in[0] != ' ' && in[0] != '\t' && in[0] != '\r' &&
         in[0] != '\n';
       in += 2) {
    unsigned v;
    if (!in[1] || n >= cap - 1) return -1;
    if (sscanf(in, "%2x", &v) != 1) return -1;
    out[n++] = (char) v;
  }
  out[n] = 0;
  return n;
}

/*
 * UTF-8 to UTF-16, because the harness speaks hex of UTF-8 bytes on both verbs
 * and only the W entry points need anything else. MultiByteToWideChar with CP_UTF8
 * rather than a hand-rolled loop: this is the Windows runtime's own answer, and a
 * driver that decoded UTF-8 differently from the API it is testing would be
 * measuring its own decoder.
 */
static void to_wide(const char * in, WCHAR * out, int cap) {
  int n = MultiByteToWideChar(CP_UTF8, 0, in, -1, out, cap);
  if (n <= 0) out[0] = 0;
}

/** The document path as wide text, for the W entry points. */
static const WCHAR * to_wide_path(const char * path) {
  static WCHAR wide[MAX_PATH];
  to_wide(path, wide, MAX_PATH);
  return wide;
}

static void put_hex(const char * p, int n) {
  if (n <= 0) {
    fputs(".", stdout);
    return;
  }
  for (int i = 0; i < n; i++) printf("%02x", (unsigned char) p[i]);
}

static void put_buffer(const char * p, int n) {
  if (n <= 0) {
    fputs("-", stdout);
    return;
  }
  for (int i = 0; i < n; i++) printf("%02x", (unsigned char) p[i]);
}

int main(int argc, char ** argv) {
  static char line[1 << 16];
  if (argc == 2 && !strcmp(argv[1], "--version")) {
    printf("driver %s wine %s\n", GTEXT_DRIVER_SHA, GTEXT_WINE_VERSION);
    return 0;
  }
  /* One path, reused, with the profile cache flushed between documents. A fresh
   * name per document would avoid the flush and would also make the run's cost
   * grow with the corpus. */
  /*
   * **Both streams to binary mode, and this is not a precaution.** This is a
   * Windows program, so stdin and stdout default to text mode and the C runtime
   * rewrites CRLF on the way in and LF on the way out. Without this the document
   * `[a]\r\nk=v\r\n` arrived with its CRs already removed, `fread` returned fewer
   * bytes than the length line promised, and the protocol desynchronised at the
   * first document containing a CR - 69 blocks for 83 documents. A differential
   * about line terminators whose own channel rewrites line terminators measures
   * nothing.
   */
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);

  if (!GetTempPathA(sizeof doc_path, doc_path)) return 2;
  strncat(doc_path, "gtext-w32.ini", sizeof doc_path - strlen(doc_path) - 1);

  while (fgets(line, sizeof line, stdin)) {
    long len;
    if (!strncmp(line, "MAKE ", 5) || !strncmp(line, "MAKEW ", 6)) {
      int wide = line[4] == 'W';
      long n = strtol(line + (wide ? 6 : 5), NULL, 10);
      long i;
      /*
       * Deleted rather than truncated, and the cache flushed first. The W entry
       * points choose the file's encoding when they *create* it, so a file left
       * over from a previous MAKE would be appended to in whatever encoding that
       * one picked - which would make the answer depend on the order of the run.
       */
      WritePrivateProfileStringA(NULL, NULL, NULL, doc_path);
      DeleteFileA(doc_path);
      for (i = 0; i < n; i++) {
        char sec[8192], key[8192], val[8192];
        char * sp1;
        char * sp2;
        if (!fgets(line, sizeof line, stdin)) return 3;
        if (strncmp(line, "SET ", 4)) return 3;
        sp1 = strchr(line + 4, ' ');
        if (!sp1) return 3;
        sp2 = strchr(sp1 + 1, ' ');
        if (!sp2) return 3;
        if (unhex(line + 4, sec, sizeof sec) < 0 ||
            unhex(sp1 + 1, key, sizeof key) < 0 ||
            unhex(sp2 + 1, val, sizeof val) < 0) {
          return 3;
        }
        if (wide) {
          WCHAR wsec[8192], wkey[8192], wval[8192];
          to_wide(sec, wsec, 8192);
          to_wide(key, wkey, 8192);
          to_wide(val, wval, 8192);
          WritePrivateProfileStringW(wsec, wkey, wval, to_wide_path(doc_path));
        }
        else {
          WritePrivateProfileStringA(sec, key, val, doc_path);
        }
      }
      /* Flushed again before reading the bytes: the API buffers writes, and the
       * file on disk is not the file until it has been told to stop caching it. */
      WritePrivateProfileStringA(NULL, NULL, NULL, doc_path);
      {
        static char buf[SBUF];
        size_t got = 0;
        FILE * f = fopen(doc_path, "rb");
        if (f) {
          got = fread(buf, 1, sizeof buf, f);
          fclose(f);
        }
        fputs("FILE ", stdout);
        put_hex(buf, (int) got);
        fputs("\n", stdout);
        fflush(stdout);
      }
      continue;
    }
    if (strncmp(line, "DOC ", 4)) continue;
    len = strtol(line + 4, NULL, 10);
    if (len < 0) break;
    {
      char * data = malloc((size_t) len + 1);
      FILE * f;
      if (!data) return 3;
      if (len && fread(data, 1, (size_t) len, stdin) != (size_t) len) return 3;
      /* Flush before the rewrite, not after: the API caches the most recently
       * used file, and one path reused across documents is exactly the case that
       * cache is wrong about. */
      WritePrivateProfileStringA(NULL, NULL, NULL, doc_path);
      f = fopen(doc_path, "wb");
      if (!f) return 3;
      if (len) fwrite(data, 1, (size_t) len, f);
      fclose(f);
      free(data);
    }
    if (!fgets(line, sizeof line, stdin) || strncmp(line, "ASK ", 4)) return 3;
    {
      long n = strtol(line + 4, NULL, 10);
      long i;
      printf("BEGIN\n");
      for (i = 0; i < n; i++) {
        if (!fgets(line, sizeof line, stdin)) return 3;
        if (!strncmp(line, "NAMES", 5)) {
          static char buf[SBUF];
          DWORD got = GetPrivateProfileSectionNamesA(buf, sizeof buf, doc_path);
          put_buffer(buf, (int) got);
          fputs("\n", stdout);
        }
        else if (!strncmp(line, "SECT ", 5)) {
          static char buf[SBUF];
          char sec[8192];
          DWORD got;
          if (unhex(line + 5, sec, sizeof sec) < 0) {
            fputs("BADJOB\n", stdout);
            continue;
          }
          got = GetPrivateProfileSectionA(sec, buf, sizeof buf, doc_path);
          put_buffer(buf, (int) got);
          fputs("\n", stdout);
        }
        else if (!strncmp(line, "GET ", 4)) {
          static char buf[VBUF];
          char sec[8192], key[8192];
          char * sp = strchr(line + 4, ' ');
          DWORD got;
          if (!sp) {
            fputs("BADJOB\n", stdout);
            continue;
          }
          if (unhex(line + 4, sec, sizeof sec) < 0 ||
              unhex(sp + 1, key, sizeof key) < 0) {
            fputs("BADJOB\n", stdout);
            continue;
          }
          got = GetPrivateProfileStringA(sec, key, "\x01", buf, sizeof buf,
              doc_path);
          if (got == 1 && buf[0] == '\x01') fputs("MISSING\n", stdout);
          else {
            put_hex(buf, (int) got);
            fputs("\n", stdout);
          }
        }
        else {
          fputs("BADJOB\n", stdout);
        }
      }
      printf("END\n");
      fflush(stdout);
    }
  }
  return 0;
}
