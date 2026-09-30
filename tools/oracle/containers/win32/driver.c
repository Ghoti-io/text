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
