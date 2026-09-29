/*
 * The git config reference driver: git itself, asked about one document at a
 * time through `git config --file - --list -z`.
 *
 * **One reference, unlike the Desktop Entry pin, and that is not an oversight.**
 * git config has exactly one implementation and no validator beside it: the
 * question "is this document legal" and the question "what is this value" are
 * both answered by the same program, because git refuses a file outright rather
 * than warning about it. So there is no second reference to disagree with, and
 * the differential's honesty comes instead from the generator's own intent being
 * a third reading - see tools/oracle/ini_git_diff.py.
 *
 * Protocol: a batch, because a container invocation per document would be tens
 * of thousands of them. Read `<len>\n` then that many bytes, repeated; `-1\n`
 * ends the stream. **Every byte string in the output is hex**, so a document
 * containing a newline, a NUL or invalid UTF-8 cannot corrupt the framing.
 *
 * Output per document, mirroring tools/oracle/ini_git_ours.c:
 *
 *     BEGIN
 *     git ok | git err <exit-status>
 *     V <hex canonical name> <hex value | - for absent>
 *     END
 *
 * `git config --list -z` writes `name\nvalue\0` for a variable with a value and
 * `name\0` for one without, which is the only way to tell git's valueless key
 * from its empty one. Reading `--list` without `-z` would lose that, and would
 * also split a value containing a newline into two records.
 *
 * Copyright 2026 by Corey Pennycuff
 */

/*
 * popen(), pclose() and mkstemp() are POSIX rather than C17, and this is
 * compiled as C17. Asking for the feature set by name is better than relaxing
 * the standard to gnu17: it says which functions are wanted and why.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef GTEXT_DRIVER_SHA
#define GTEXT_DRIVER_SHA "unknown"
#endif
#ifndef GTEXT_GIT_VERSION
#define GTEXT_GIT_VERSION "unknown"
#endif

/** Write bytes as lowercase hex, or `-` for an absent string. */
static void put_hex(const char * bytes, size_t len) {
  if (!bytes) {
    fputs("-", stdout);
    return;
  }
  if (!len) {
    fputs(".", stdout); /* Empty is not absent, and the two must not collide. */
    return;
  }
  for (size_t i = 0; i < len; i++) {
    printf("%02x", (unsigned char) bytes[i]);
  }
}

/** The captured output of one `git config` run. */
typedef struct {
  char * data;
  size_t len;
  int status;
} git_result;

/**
 * Run `git config --file <path> --list -z` and capture all of stdout.
 *
 * Reads the pipe as bytes rather than lines: the output is NUL-delimited and
 * contains embedded newlines by design, so any line-oriented read would corrupt
 * it.
 */
static git_result ask_git(const char * path) {
  git_result r;
  memset(&r, 0, sizeof(r));
  char command[512];
  /*
   * stderr to /dev/null: git's message is prose that a differential must not
   * depend on, and the exit status is the verdict. The three GIT_CONFIG_*
   * settings make the answer a function of this file and nothing else - without
   * them a stray /etc/gitconfig in the image would join every document silently,
   * and the comparison would be against the file plus whatever the image ships.
   */
  int n = snprintf(command, sizeof(command),
      "GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null "
      "GIT_CONFIG_SYSTEM=/dev/null "
      "git config --file %s --list -z 2>/dev/null",
      path);
  if (n < 0 || (size_t) n >= sizeof(command)) {
    r.status = -1;
    return r;
  }
  FILE * pipe = popen(command, "r");
  if (!pipe) {
    r.status = -1;
    return r;
  }
  size_t capacity = 4096;
  r.data = malloc(capacity);
  if (!r.data) {
    pclose(pipe);
    r.status = -1;
    return r;
  }
  for (;;) {
    if (r.len == capacity) {
      size_t want = capacity * 2;
      char * grown = realloc(r.data, want);
      if (!grown) {
        free(r.data);
        r.data = NULL;
        pclose(pipe);
        r.status = -1;
        return r;
      }
      r.data = grown;
      capacity = want;
    }
    size_t got = fread(r.data + r.len, 1, capacity - r.len, pipe);
    if (!got) break;
    r.len += got;
  }
  r.status = pclose(pipe);
  return r;
}

int main(int argc, char ** argv) {
  if (argc > 1 && strcmp(argv[1], "--version") == 0) {
    /* The gate checks this line against containers/IMAGES, so a Debian point
     * release that moved git fails the check rather than quietly changing what
     * the comparison means. */
    printf("git %s, driver %s\n", GTEXT_GIT_VERSION, GTEXT_DRIVER_SHA);
    return 0;
  }

  char header[64];
  while (fgets(header, sizeof(header), stdin)) {
    long len = strtol(header, NULL, 10);
    if (len < 0) break;
    char * data = malloc((size_t) len + 1);
    if (!data) return 3;
    if (len && fread(data, 1, (size_t) len, stdin) != (size_t) len) {
      free(data);
      return 3;
    }

    /*
     * A temporary file rather than `--file -`: git reads a config file by path
     * and seeks in it, and a document is at most a few hundred bytes here, so
     * there is nothing to gain from streaming and a real risk of git treating
     * `-` as a literal filename.
     */
    char path[] = "/tmp/gtext-git-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
      free(data);
      return 3;
    }
    if (len && write(fd, data, (size_t) len) != (ssize_t) len) {
      close(fd);
      unlink(path);
      free(data);
      return 3;
    }
    close(fd);

    printf("BEGIN\n");
    git_result r = ask_git(path);
    unlink(path);
    if (r.status != 0) {
      printf("git err %d\n", r.status);
    }
    else {
      printf("git ok\n");
      /* `name\nvalue\0` per variable, or `name\0` when it has no value. */
      size_t start = 0;
      while (start < r.len) {
        size_t end = start;
        while (end < r.len && r.data[end] != '\0') end++;
        const char * rec = r.data + start;
        size_t rec_len = end - start;
        const char * nl = memchr(rec, '\n', rec_len);
        printf("V ");
        if (nl) {
          put_hex(rec, (size_t) (nl - rec));
          printf(" ");
          put_hex(nl + 1, rec_len - (size_t) (nl - rec) - 1);
        }
        else {
          put_hex(rec, rec_len);
          printf(" -");
        }
        printf("\n");
        start = end + 1;
      }
    }
    printf("END\n");
    free(r.data);
    free(data);
  }
  fflush(stdout);
  return 0;
}
