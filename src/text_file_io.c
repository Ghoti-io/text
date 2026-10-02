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

/**
 * @file
 *
 * Shared file I/O helpers, internal to the library.
 *
 * This is a thin mapping onto ghoti.io-cutil's file module. It used to be the
 * implementation as well - a whole-file reader, a temporary-file creator with
 * a `#ifdef _MSC_VER` arm, and a rename wrapper - and src/yaml/yaml_file_io.c
 * carried a second, slightly different copy of the same three things. Reading
 * a file and replacing one atomically are not text-format problems; cutil owns
 * them now for the same reason it owns the allocator, and what is left here is
 * the part that genuinely belongs to this library: turning cutil's result
 * codes into the status each format maps onto its own.
 */

#include <stdio.h>
#include <stdlib.h>

#include <ghoti.io/cutil/file.h>
#include <ghoti.io/cutil/path.h>

#include "text_file_io_internal.h"

/**
 * @brief Map a cutil file result onto this library's status.
 *
 * GCU_FILE_ERR_IO covers open, read, write, rename and sync alike, so the
 * caller says which of those it was doing: the distinction between "could not
 * open it" and "it failed part way through" is one the formats' error
 * messages make, and it is not recoverable from the result code alone.
 *
 * Hence two fallbacks rather than one. @p before_status is what a failure
 * decided *before* any byte moved means for this operation, and @p
 * during_status is what GCU_FILE_ERR_IO means for it. Collapsing them into a
 * single parameter is what left GTEXT_FILE_E_READ with no producer anywhere in
 * the library: the reader passed GTEXT_FILE_E_OPEN for every cutil failure, so
 * a file that opened and then would not read - a directory, a disk that gave
 * up half way - was reported to five formats as "could not open the file", and
 * the five `case GTEXT_FILE_E_READ:` arms sat there unreachable.
 *
 * The codes beside ERR_IO are all decided before the transfer, which is what
 * lets them be told apart: cutil derives NOT_FOUND, ACCESS, EXISTS and
 * NOT_EMPTY from errno at the open, and ERR_INVALID from its own argument
 * checking. Every code cutil can return is named, and this switch deliberately
 * has **no `default:` arm**, so that cutil growing another one is a
 * -Wswitch error here - which is to say an error, under -Werror - rather than
 * a silent reclassification. That is how the four it has already grown came to
 * be lumped in with ERR_IO: a `default:` opts a switch out of the one warning
 * that would have reported them.
 *
 * What stays approximate, said plainly: an *open* that fails for a reason
 * outside cutil's errno table - EMFILE, ENXIO on a FIFO - also arrives as
 * ERR_IO, and is reported here as a read failure. The alternative is to ask
 * the filesystem a second question afterwards, which is a race, and the
 * question cutil's result codes exist to avoid.
 */
static gtext_file_status gtext_file_map(GCU_File_Result result,
    gtext_file_status before_status, gtext_file_status during_status) {
  switch (result) {
    case GCU_FILE_OK:
      return GTEXT_FILE_OK;
    case GCU_FILE_ERR_OOM:
      return GTEXT_FILE_E_OOM;
    case GCU_FILE_ERR_LIMIT:
      return GTEXT_FILE_E_LIMIT;
    case GCU_FILE_ERR_NOT_FOUND:
      return GTEXT_FILE_E_NOT_FOUND;
    case GCU_FILE_ERR_ACCESS:
      return GTEXT_FILE_E_ACCESS;
    case GCU_FILE_ERR_EXISTS:
    case GCU_FILE_ERR_NOT_EMPTY:
    case GCU_FILE_ERR_INVALID:
      return before_status;
    case GCU_FILE_ERR_IO:
    case GCU_FILE_RESULT_COUNT:
      return during_status;
  }
  /* Out here rather than in a `default:` arm, and that is the whole point:
     -Wswitch reports a missing enumerator only where there is no default, so
     writing one would opt this switch out of the single instrument that would
     have caught cutil growing NOT_FOUND, EXISTS, ACCESS and NOT_EMPTY. This
     line is for a value outside the enum, which C permits through a cast and
     an enum's own range does not forbid. */
  return during_status;
}

GTEXT_INTERNAL_API gtext_file_status gtext_file_read_all(const char * path,
    size_t max_bytes, const GTEXT_Allocator * alloc, char ** out_data,
    size_t * out_len) {
  if (!path || !out_data || !out_len) {
    return GTEXT_FILE_E_OPEN;
  }
  *out_data = NULL;
  *out_len = 0;

  /* GCU_FILE_UNLIMITED is 0, which is the same "no limit" spelling this
   * library's callers already use, so max_bytes passes straight through. */
  /* GTEXT_Allocator is a typedef for GCU_Allocator, so the caller's allocator
   * is handed to cutil as it stands - there is nothing to bridge. This read
   * passed NULL here for as long as the option existed, which left the bytes of
   * the file itself outside the one promise the option makes. */
  void * data = NULL;
  GCU_File_Result result =
      gcu_file_read(path, max_bytes, alloc, &data, out_len);
  if (result != GCU_FILE_OK) {
    /* GTEXT_FILE_E_READ for an ERR_IO, because gcu_file_read() reports the
       open through errno - NOT_FOUND or ACCESS - and reaches ERR_IO from the
       transfer. A directory is the ordinary way to get here: fopen() on one
       succeeds and the first fread() sets the error flag. */
    return gtext_file_map(result, GTEXT_FILE_E_OPEN, GTEXT_FILE_E_READ);
  }

  *out_data = (char *)data;
  return GTEXT_FILE_OK;
}

GTEXT_INTERNAL_API void gtext_file_free(
    const GTEXT_Allocator * alloc, char * data) {
  gcu_file_free(alloc, data);
}

/** Write one buffer to the stream cutil opened for the temporary file. */
static int gtext_file_fwrite(void * user, const char * bytes, size_t len) {
  FILE * file = (FILE *)user;
  if (len == 0) {
    return 0;
  }
  return fwrite(bytes, 1, len, file) == len ? 0 : 1;
}

GTEXT_INTERNAL_API gtext_file_status gtext_file_write_atomic(const char * path,
    const GTEXT_Allocator * alloc,
    int (*emit)(void * ctx, gtext_file_write_cb write, void * write_user),
    void * user) {
  if (!path || !emit) {
    return GTEXT_FILE_E_WRITE;
  }

  /*
   * The temporary file goes in the destination's own directory, because the
   * commit is a rename and a rename across filesystems is a copy - which is
   * not atomic, and is the whole point of doing this. A path with no
   * directory part yields ".", which is where the destination is too.
   */
  size_t directory_len = 0;
  if (gcu_path_dirname(GCU_PATH_NATIVE, path, NULL, 0, &directory_len)
      != GCU_PATH_OK) {
    return GTEXT_FILE_E_OPEN;
  }
  char * directory =
      (char *)gtext_allocator_malloc(alloc, directory_len + 1);
  if (!directory) {
    return GTEXT_FILE_E_OOM;
  }
  if (gcu_path_dirname(
          GCU_PATH_NATIVE, path, directory, directory_len + 1, NULL)
      != GCU_PATH_OK) {
    gtext_allocator_free(alloc, directory);
    return GTEXT_FILE_E_OPEN;
  }

  GCU_File_Temp temp;
  GCU_File_Result result =
      gcu_file_temp_create(&temp, directory, "gtext", NULL);
  gtext_allocator_free(alloc, directory);
  if (result != GCU_FILE_OK) {
    /* The same status either way, and that is the finding rather than an
       oversight: gcu_file_temp_create() calls mkstemp() and returns a flat
       GCU_FILE_ERR_IO for every reason it can fail, a missing directory
       included. So there is no "part way through" to distinguish here - the
       temporary either came into being or did not. */
    return gtext_file_map(result, GTEXT_FILE_E_OPEN, GTEXT_FILE_E_OPEN);
  }

  int failed = emit(user, gtext_file_fwrite, gcu_file_temp_stream(&temp));
  if (failed) {
    gcu_file_temp_abort(&temp);
    return GTEXT_FILE_E_WRITE;
  }

  /*
   * GCU_FILE_SYNC_FULL, which commits the bytes before the rename rather than
   * leaving them to writeback. This is stronger than what the hand-written
   * version did - it flushed stdio and renamed - and it is the promise the
   * header here already made: the file these parsers are usually pointed at is
   * a configuration file, and "the old one survived, but the new one is empty"
   * is not a way for one of those to come back from a power loss.
   *
   * GCU_FILE_PERMS_PRESERVE, for the same reason and from the same sentence.
   * The destination of an atomic write is a renamed temporary, and a
   * temporary is owner-only; until cutil grew this argument the mode came
   * from that and nobody chose it, so every save of a 644 configuration file
   * quietly narrowed it to 600. PRESERVE keeps whatever the destination
   * already had and falls back to what an ordinary fopen() would have given
   * when there is no destination yet, which is what the hand-written version
   * did before any of this.
   *
   * The two wrong answers are wrong in opposite directions, and one of them
   * is the one a caller reaches by reflex: PRIVATE is the zero value and
   * narrows a config nobody asked to narrow, while DEFAULT widens one
   * somebody deliberately ran chmod 600 on. Replacing a file is not the same
   * act as creating it.
   */
  result = gcu_file_temp_commit(
      &temp, path, GCU_FILE_SYNC_FULL, GCU_FILE_PERMS_PRESERVE);
  /* Both fallbacks are GTEXT_FILE_E_WRITE: everything this call can fail at -
     the sync, the rename, reading the destination's permissions - happens
     after the bytes were accepted, so "it failed part way through" is the only
     honest reading of any of them. */
  return gtext_file_map(result, GTEXT_FILE_E_WRITE, GTEXT_FILE_E_WRITE);
}
