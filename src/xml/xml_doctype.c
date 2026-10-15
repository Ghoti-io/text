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
 * The document type declaration: recognized, not processed.
 *
 * XML 1.0 (Fifth Edition) [28] doctypedecl, [75] ExternalID, [11]
 * SystemLiteral, [12] PubidLiteral, [28a]-[29] and the markup declarations of
 * the internal subset.
 *
 * The name and the external identifiers are kept, in a doctype node. The
 * internal subset is **skipped by a bounded scanner**: it knows where each kind
 * of token ends - a comment at "-->", a processing instruction at "?>", a
 * declaration at the first '>' outside a quoted literal - so a ']' inside a
 * literal or a comment does not end the subset, and it moves forward on every
 * step, so it is linear in the input. It does not check a declaration's
 * grammar; it checks the characters, that each declaration is well delimited,
 * and that nothing that cannot occur in one (a '<', a ']') appears outside a
 * literal. Nothing it skips has any effect on the parse: not an entity, not a
 * default attribute.
 */

#include <ghoti.io/text/macros.h>

#include <string.h>

#include "xml_internal.h"

/* The specific code for plain syntax, the generic ones when the character
   itself is the trouble. */
static GTEXT_XML_Status bad(xml_ctx * p, const char * message) {
  uint32_t cp = 0;
  size_t n = 0;
  if (xml_decode(p, p->pos, &cp, &n) == XML_CHAR_OK) {
    return xml_fail(p, GTEXT_XML_E_DOCTYPE, message);
  }
  return xml_unexpected(p, message);
}

static int pubid_char(unsigned char c, unsigned char quote) {
  if (c == quote) {
    return 0;
  }
  return c == 0x20 || c == 0x0D || c == 0x0A || (c >= 'a' && c <= 'z')
      || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
      || (c != 0 && strchr("-'()+,./:=?;!*#@$_%", c) != NULL);
}

/* A quoted literal at the cursor. Appends its value to `out` - line ends
   normalized, and for a public identifier white space collapsed and trimmed as
   the specification asks processors to - and leaves the cursor after the
   closing quote. */
static GTEXT_XML_Status literal(xml_ctx * p, markup_buf * out, int is_public) {
  const unsigned char quote = p->pos < p->length ? p->in[p->pos] : 0;
  if (quote != '"' && quote != '\'') {
    return bad(p, "a quoted literal was expected");
  }
  XML_CONSUME_OR_RETURN(p, 1);
  /* An empty literal is present and empty, which is not the same as absent:
     make sure there is a string to hand over. */
  GTEXT_XML_Status status = xml_append(p, out, "", 0);
  if (status != GTEXT_XML_OK) {
    return status;
  }
  const size_t start = p->pos;
  size_t i = start;
  while (i < p->length && p->in[i] != quote) {
    GTEXT_XML_Status work = xml_charge_work(p, 1);
    if (work != GTEXT_XML_OK) {
      return work;
    }
    const unsigned char b = p->in[i];
    if (is_public) {
      if (!pubid_char(b, quote)) {
        XML_CONSUME_OR_RETURN(p, i - p->pos);
        return bad(p, "character not allowed in a public identifier");
      }
      i++;
      continue;
    }
    if (b >= 0x20 && b < 0x80
        && !(b == 0x7F && p->version == XML_VERSION_11)) {
      i++;
      continue;
    }
    uint32_t cp = 0;
    size_t n = 0;
    if (xml_decode(p, i, &cp, &n) != XML_CHAR_OK) {
      XML_CONSUME_OR_RETURN(p, i - p->pos);
      return xml_unexpected(p, "invalid character");
    }
    i += n;
  }
  if (i >= p->length) {
    XML_CONSUME_OR_RETURN(p, i - p->pos);
    return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "unterminated literal");
  }
  if (is_public) {
    size_t words = 0;
    size_t j = start;
    while (j < i) {
      status = xml_charge_work(p, 1);
      if (status != GTEXT_XML_OK) return status;
      while (j < i) {
        const size_t whitespace = xml_space_bytes(p, j);
        if (whitespace == 0 || j + whitespace > i) {
          break;
        }
        j += whitespace;
      }
      const size_t word = j;
      while (j < i && xml_space_bytes(p, j) == 0) {
        j++;
      }
      if (j > word) {
        if (words++ > 0) {
          status = xml_append(p, out, " ", 1);
          if (status != GTEXT_XML_OK) {
            return status;
          }
        }
        status = xml_append(p, out, (const char *)p->in + word, j - word);
        if (status != GTEXT_XML_OK) {
          return status;
        }
      }
    }
  }
  else {
    status = xml_append_normalized(p, out, start, i);
    if (status != GTEXT_XML_OK) {
      return status;
    }
  }
  XML_CONSUME_OR_RETURN(p, i + 1 - p->pos);
  return GTEXT_XML_OK;
}

/* The body of a markup declaration, after its keyword, up to and including the
   '>' that ends it. Literals are skipped whole. */
static GTEXT_XML_Status skip_declaration(xml_ctx * p, int is_entity) {
  for (;;) {
    if (p->err.code != GTEXT_XML_OK) {
      return p->err.code;
    }
    size_t i = p->pos;
    while (i < p->length) {
      GTEXT_XML_Status work = xml_charge_work(p, 1);
      if (work != GTEXT_XML_OK) {
        return work;
      }
      const unsigned char b = p->in[i];
      if (b == '"' || b == '\'' || b == '>' || b == '<' || b == ']' || b == '%') {
        break;
      }
      if (b >= 0x20 && b < 0x80
          && !(b == 0x7F && p->version == XML_VERSION_11)) {
        i++;
        continue;
      }
      if (b == '\t' || b == '\n' || b == '\r') {
        i++;
        continue;
      }
      uint32_t cp = 0;
      size_t n = 0;
      if (xml_decode(p, i, &cp, &n) != XML_CHAR_OK) {
        XML_CONSUME_OR_RETURN(p, i - p->pos);
        return xml_unexpected(p, "invalid character");
      }
      i += n;
    }
    XML_CONSUME_OR_RETURN(p, i - p->pos);
    if (p->pos >= p->length) {
      return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF,
          "unterminated declaration in the internal subset");
    }
    const unsigned char b = p->in[p->pos];
    if (b == '>') {
      XML_CONSUME_OR_RETURN(p, 1);
      return GTEXT_XML_OK;
    }
    if (b == '<' || b == ']') {
      return xml_fail(p, GTEXT_XML_E_DOCTYPE,
          "declaration in the internal subset is not terminated by '>'");
    }
    if (b == '%' && p->partial && p->pos + 1 >= p->length) {
      return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "unexpected end of input");
    }
    if (b == '%' && is_entity && p->pos + 1 < p->length
        && xml_space_bytes(p, p->pos + 1) > 0) {
      /* `<!ENTITY % name ...>`: the percent sign that makes it a parameter
         entity declaration, not a reference. */
      XML_CONSUME_OR_RETURN(p, 1);
      continue;
    }
    if (b == '%') {
      /* WFC: PEs in Internal Subset - a parameter entity reference occurs
         between declarations, never inside one. */
      return xml_fail(p, GTEXT_XML_E_DOCTYPE,
          "a parameter entity reference cannot occur inside a declaration in the internal subset");
    }
    /* A quoted literal: find its end, checking what is inside. */
    const unsigned char quote = b;
    XML_CONSUME_OR_RETURN(p, 1);
    i = p->pos;
    while (i < p->length && p->in[i] != quote) {
      GTEXT_XML_Status work = xml_charge_work(p, 1);
      if (work != GTEXT_XML_OK) {
        return work;
      }
      const unsigned char c = p->in[i];
      if ((c >= 0x20 && c < 0x80
              && !(c == 0x7F && p->version == XML_VERSION_11))
          || c == '\t' || c == '\n' || c == '\r') {
        i++;
        continue;
      }
      uint32_t cp = 0;
      size_t n = 0;
      if (xml_decode(p, i, &cp, &n) != XML_CHAR_OK) {
        XML_CONSUME_OR_RETURN(p, i - p->pos);
        return xml_unexpected(p, "invalid character");
      }
      i += n;
    }
    XML_CONSUME_OR_RETURN(p, i - p->pos);
    if (p->pos >= p->length) {
      return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF,
          "unterminated literal in the internal subset");
    }
    XML_CONSUME_OR_RETURN(p, 1);
  }
}

/* Past the '[' to past the ']'. */
static GTEXT_XML_Status skip_internal_subset(xml_ctx * p) {
  static const char * const keywords[] = {
      "<!ELEMENT", "<!ATTLIST", "<!ENTITY", "<!NOTATION"};
  for (;;) {
    if (p->err.code != GTEXT_XML_OK) {
      return p->err.code;
    }
    xml_skip_space(p);
    if (p->err.code != GTEXT_XML_OK) {
      return p->err.code;
    }
    if (p->pos >= p->length) {
      return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF,
          "unterminated internal subset");
    }
    const unsigned char b = p->in[p->pos];
    GTEXT_XML_Status status;
    if (b == ']') {
      XML_CONSUME_OR_RETURN(p, 1);
      return GTEXT_XML_OK;
    }
    if (b == '%') {
      status = xml_charge(p, &p->references, p->max_references, 1,
          GTEXT_XML_E_LIMIT_REFERENCES, "XML reference limit exceeded");
      if (status != GTEXT_XML_OK) {
        return status;
      }
      XML_CONSUME_OR_RETURN(p, 1);
      size_t length = 0;
      status = xml_scan_name(p, &length);
      if (status != GTEXT_XML_OK) {
        return status;
      }
      XML_CONSUME_OR_RETURN(p, length);
      status = xml_expect(p, ";", 1, "a parameter entity reference ends with ';'");
      if (status != GTEXT_XML_OK) {
        return status;
      }
      continue;
    }
    if (b != '<') {
      return bad(p, "unexpected content in the internal subset");
    }
    if (xml_cut_literal(p, "<!--", 4) || xml_cut_literal(p, "<?", 2)) {
      return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "unexpected end of input");
    }
    if (xml_looking_at(p, "<!--", 4)) {
      status = xml_parse_comment(p, 0);
    }
    else if (xml_looking_at(p, "<?", 2)) {
      status = xml_parse_pi(p, 0);
    }
    else {
      size_t k = 0;
      for (; k < sizeof(keywords) / sizeof(keywords[0]); k++) {
        if (xml_looking_at(p, keywords[k], strlen(keywords[k]))) {
          break;
        }
        if (xml_cut_literal(p, keywords[k], strlen(keywords[k]))) {
          return xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "unexpected end of input");
        }
      }
      if (k == sizeof(keywords) / sizeof(keywords[0])) {
        return xml_fail(p, GTEXT_XML_E_DOCTYPE,
            "not a declaration, comment or processing instruction");
      }
      status = xml_charge(p, &p->declarations, p->max_declarations, 1,
          GTEXT_XML_E_LIMIT_DECLARATIONS, "DTD declaration limit exceeded");
      if (status != GTEXT_XML_OK) {
        return status;
      }
      XML_CONSUME_OR_RETURN(p, strlen(keywords[k]));
      if (p->pos >= p->length || xml_space_bytes(p, p->pos) == 0) {
        return bad(p, "white space is required after the declaration keyword");
      }
      status = skip_declaration(p, k == 2);
    }
    if (status != GTEXT_XML_OK) {
      return status;
    }
  }
}

GTEXT_XML_Status xml_parse_doctype(xml_ctx * p) {
  const GTEXT_Markup_Location start = xml_here(p);
  markup_buf name;
  markup_buf pub;
  markup_buf sys;
  markup_buf_init(&name, p->allocator);
  markup_buf_init(&pub, p->allocator);
  markup_buf_init(&sys, p->allocator);
  int has_public = 0;
  int has_system = 0;
  GTEXT_XML_Status status = GTEXT_XML_OK;
  size_t length = 0;

  status = xml_consume(p, 9); /* "<!DOCTYPE" */
  if (status != GTEXT_XML_OK) goto done;
  if (xml_skip_space(p) == 0) {
    status = bad(p, "white space is required after <!DOCTYPE");
    goto done;
  }
  status = xml_scan_name(p, &length);
  if (status != GTEXT_XML_OK) {
    goto done;
  }
  status = xml_append(p, &name, (const char *)p->in + p->pos, length);
  if (status != GTEXT_XML_OK) {
    goto done;
  }
  status = xml_consume(p, length);
  if (status != GTEXT_XML_OK) goto done;
  size_t space = xml_skip_space(p);

  if (xml_cut_literal(p, "SYSTEM", 6) || xml_cut_literal(p, "PUBLIC", 6)) {
    status = xml_fail(p, GTEXT_XML_E_UNEXPECTED_EOF, "unexpected end of input");
    goto done;
  }
  const int is_system = xml_looking_at(p, "SYSTEM", 6);
  const int is_public = !is_system && xml_looking_at(p, "PUBLIC", 6);
  if (is_system || is_public) {
    if (space == 0) {
      status = bad(p, "white space is required before the external identifier");
      goto done;
    }
    status = xml_consume(p, 6);
    if (status != GTEXT_XML_OK) goto done;
    if (xml_skip_space(p) == 0) {
      status = bad(p, "white space is required after SYSTEM or PUBLIC");
      goto done;
    }
    if (is_public) {
      status = literal(p, &pub, 1);
      if (status != GTEXT_XML_OK) {
        goto done;
      }
      has_public = 1;
      if (xml_skip_space(p) == 0) {
        status = bad(p, "white space is required before the system identifier");
        goto done;
      }
    }
    status = literal(p, &sys, 0);
    if (status != GTEXT_XML_OK) {
      goto done;
    }
    has_system = 1;
    space = xml_skip_space(p);
  }
  (void)space;

  if (p->pos < p->length && p->in[p->pos] == '[') {
    status = xml_consume(p, 1);
    if (status != GTEXT_XML_OK) goto done;
    status = skip_internal_subset(p);
    if (status != GTEXT_XML_OK) {
      goto done;
    }
    xml_skip_space(p);
  }
  if (p->pos >= p->length || p->in[p->pos] != '>') {
    status = bad(p, "'>' was expected to end the document type declaration");
    goto done;
  }
  status = xml_consume(p, 1);
  if (status != GTEXT_XML_OK) goto done;

  if (p->stream) {
    status = xml_emit_doctype(p, &start, name.data,
        has_public ? pub.data : NULL, has_system ? sys.data : NULL);
  }
  else {
    GTEXT_Markup_Node * node = NULL;
    status = xml_from_markup(p,
        gtext_markup_create_doctype(p->doc, name.data,
            has_public ? pub.data : NULL, has_system ? sys.data : NULL, &node));
    if (status == GTEXT_XML_OK) {
      xml_attach(p->doc->root, node, &start);
    }
  }

done:
  markup_buf_free(&name);
  markup_buf_free(&pub);
  markup_buf_free(&sys);
  return status;
}
