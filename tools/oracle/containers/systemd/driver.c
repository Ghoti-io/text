/*
 * The systemd reference driver: `systemd-analyze verify`, asked about a batch of
 * documents in one invocation.
 *
 * **The exit status is not the verdict here, and that is the whole design.**
 * `systemd-analyze verify` exits 0 on a syntax error - it warns and skips the
 * offending line - and reserves a non-zero status for *semantic* failure, such as a
 * service with no `ExecStart=`. So this driver reports the **diagnostics**, and
 * tools/oracle/ini_sd_diff.py decides which of them are about the grammar. A gate
 * built on the exit status would have scored every syntactically broken document as
 * legal.
 *
 * Protocol: a batch, because a container invocation per document would be tens of
 * thousands of them, and because `systemd-analyze` spends most of its time starting
 * up. Read `<len>\n` then that many bytes, repeated; `-1\n` ends the stream. Each
 * batch of documents is written into one temporary directory and verified in one
 * run.
 *
 * Output per batch:
 *
 *     BATCH <first index> <count> <hex directory path>
 *     OUT <hex of the whole combined stdout and stderr>
 *
 * **The attribution is deliberately left to the caller.** A diagnostic can carry a
 * newline - a decoded `\n` inside a reported value reaches the message as a real
 * one - so splitting the output into messages is not a line-oriented job, and doing
 * it here in C would put the fiddliest part of the instrument in the least testable
 * place. The directory path is reported so the caller knows the prefix to split on.
 *
 * **Two things this channel does to a value, both measured and neither avoidable
 * from here:** it rewrites a CR to an LF, and it truncates a long message. The
 * driver does not try to correct either; the diff excludes the documents that would
 * hit them and counts the exclusions, because an oracle's limits belong in the
 * denominator rather than in a workaround.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef GTEXT_DRIVER_SHA
#define GTEXT_DRIVER_SHA "unknown"
#endif
#ifndef GTEXT_SD_VERSION
#define GTEXT_SD_VERSION "unknown"
#endif

/** How many documents one `systemd-analyze verify` run covers. */
#define BATCH 100

/** Write bytes as lowercase hex; `.` for empty. */
static void put_hex(const char * bytes, size_t len) {
  if (!len) {
    fputs(".", stdout);
    return;
  }
  for (size_t i = 0; i < len; i++) {
    printf("%02x", (unsigned char) bytes[i]);
  }
}

/** One document's bytes. */
typedef struct {
  char * data;
  size_t len;
} document;

/** Read `<len>\n<len bytes>`. Returns 0 at end of stream. */
static int read_block(document * out) {
  char header[64];
  if (!fgets(header, sizeof(header), stdin)) return 0;
  long len = strtol(header, NULL, 10);
  if (len < 0) return 0;
  out->data = malloc((size_t) len + 1);
  if (!out->data) exit(3);
  if (len && fread(out->data, 1, (size_t) len, stdin) != (size_t) len) exit(3);
  out->data[len] = '\0';
  out->len = (size_t) len;
  return 1;
}

/** Run one batch, printing its directory and the whole of its output. */
static void run_batch(document * docs, size_t count, size_t first_index) {
  char dir[] = "/tmp/gtext-sd-XXXXXX";
  if (!mkdtemp(dir)) exit(3);

  size_t command_size = 256 + count * 64;
  char * command = malloc(command_size);
  if (!command) exit(3);
  size_t at = (size_t) snprintf(command, command_size, "systemd-analyze verify");
  for (size_t i = 0; i < count; i++) {
    char path[512];
    snprintf(path, sizeof(path), "%s/u%04zu.service", dir, i);
    FILE * f = fopen(path, "wb");
    if (!f) exit(3);
    if (docs[i].len && fwrite(docs[i].data, 1, docs[i].len, f) != docs[i].len) {
      exit(3);
    }
    fclose(f);
    at += (size_t) snprintf(command + at, command_size - at, " %s", path);
  }
  snprintf(command + at, command_size - at, " 2>&1");

  FILE * pipe = popen(command, "r");
  if (!pipe) exit(3);
  size_t capacity = 1 << 16;
  size_t len = 0;
  char * blob = malloc(capacity);
  if (!blob) exit(3);
  for (;;) {
    if (len == capacity) {
      capacity *= 2;
      char * grown = realloc(blob, capacity);
      if (!grown) exit(3);
      blob = grown;
    }
    size_t got = fread(blob + len, 1, capacity - len, pipe);
    if (!got) break;
    len += got;
  }
  pclose(pipe);

  printf("BATCH %zu %zu ", first_index, count);
  put_hex(dir, strlen(dir));
  printf("\nOUT ");
  put_hex(blob, len);
  printf("\n");
  fflush(stdout);

  for (size_t i = 0; i < count; i++) {
    char path[512];
    snprintf(path, sizeof(path), "%s/u%04zu.service", dir, i);
    unlink(path);
  }
  rmdir(dir);
  free(blob);
  free(command);
}

int main(int argc, char ** argv) {
  if (argc > 1 && strcmp(argv[1], "--version") == 0) {
    /* The gate checks this line against containers/IMAGES, so a Debian point
     * release that moved systemd fails the check rather than quietly changing what
     * the comparison means. */
    printf("systemd %s, driver %s\n", GTEXT_SD_VERSION, GTEXT_DRIVER_SHA);
    return 0;
  }

  document batch[BATCH];
  size_t count = 0;
  size_t index = 0;
  for (;;) {
    if (!read_block(&batch[count])) break;
    count++;
    if (count == BATCH) {
      run_batch(batch, count, index);
      for (size_t i = 0; i < count; i++) free(batch[i].data);
      index += count;
      count = 0;
    }
  }
  if (count) {
    run_batch(batch, count, index);
    for (size_t i = 0; i < count; i++) free(batch[i].data);
  }
  return 0;
}
