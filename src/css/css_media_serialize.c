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
 * CSSOM's "serialize a media query list" for the node array of css_media_internal.h. The walk keeps its own stack, so a query nested 100,000
 * parentheses deep is written without recursion.
 */

#include <ghoti.io/text/css/css_math.h>
#include <ghoti.io/text/css/css_media.h>
#include <ghoti.io/text/css/css_serialize.h>
#include <ghoti.io/text/css/css_value.h>

#include <string.h>

#include "css_media_internal.h"

typedef struct {
  const GTEXT_Markup_Sink * sink;
  GTEXT_CSS_Status st;
} out_t;

static void put(out_t * o, const char * s, size_t n) {
  if (o->st == GTEXT_CSS_OK && n) {
    if (o->sink->write(o->sink->user, s, n) != GTEXT_MARKUP_OK) {
      o->st = GTEXT_CSS_E_WRITE;
    }
  }
}

static void puts_(out_t * o, const char * s) {
  put(o, s, strlen(s));
}

static void put_number(out_t * o, double v, bool integer) {
  char buf[340];
  size_t n = css_cssom_number(v, integer, buf);
  put(o, buf, n);
}

static void put_value(out_t * o, const mq_value * v, int feature) {
  switch ((mq_value_kind)v->kind) {
  case MQV_NUMBER:
    put_number(o, v->a, v->integer);
    break;
  case MQV_DIMENSION: {
    put_number(o, v->a, false);
    const char * u = gtext_css_unit_name(v->unit);
    if (u) {
      puts_(o, u);
    }
    break;
  }
  case MQV_RATIO:
    if (v->math && o->st == GTEXT_CSS_OK) {
      GTEXT_CSS_Status st = gtext_css_math_serialize(v->math, NULL, o->sink);
      if (st != GTEXT_CSS_OK) {
        o->st = st;
      }
    } else {
      put_number(o, v->a, false);
    }
    puts_(o, " / ");
    if (v->math2 && o->st == GTEXT_CSS_OK) {
      GTEXT_CSS_Status st = gtext_css_math_serialize(v->math2, NULL, o->sink);
      if (st != GTEXT_CSS_OK) {
        o->st = st;
      }
    } else {
      put_number(o, v->b, false);
    }
    break;
  case MQV_IDENT: {
    const css_mq_feature_row * r = &css_mq_feature_rows[feature];
    puts_(o, css_mq_keywords[r->kw_first + v->keyword]);
    break;
  }
  case MQV_INFINITE:
    puts_(o, "infinite");
    break;
  case MQV_MATH:
    if (o->st == GTEXT_CSS_OK) {
      GTEXT_CSS_Status st = gtext_css_math_serialize(v->math, NULL, o->sink);
      if (st != GTEXT_CSS_OK) {
        o->st = st;
      }
    }
    break;
  }
}

static const char * op_text(unsigned op) {
  switch (op) {
  case MQOP_LT: return "<";
  case MQOP_LE: return "<=";
  case MQOP_GT: return ">";
  case MQOP_GE: return ">=";
  default: return "=";
  }
}

static void put_name(out_t * o, const char * pre, const char * post) {
  puts_(o, pre);
  puts_(o, post);
}

static void put_feature(out_t * o, const struct GTEXT_CSS_Media_List_Internal * in, const mq_node * n, bool bare) {
  const css_mq_feature_row * r = &css_mq_feature_rows[n->feature];
  const char * pre = "";
  const char * post = r->name;
  if (n->flags & MQF_LOGICAL) {
    post = n->feature == CSS_MQF_WIDTH ? "inline-size" : "block-size";
  }
  if (n->prefix != MQP_NONE) {
    if (r->name[0] == '-') {
      pre = n->prefix == MQP_MIN ? "-webkit-min-device-pixel-ratio" : "-webkit-max-device-pixel-ratio";
      post = "";
    } else {
      pre = n->prefix == MQP_MIN ? "min-" : "max-";
    }
  }
  if (!bare) {
    puts_(o, "(");
  }
  switch ((mq_form)n->form) {
  case MQFORM_BOOLEAN:
    put_name(o, pre, post);
    break;
  case MQFORM_PLAIN:
    put_name(o, pre, post);
    puts_(o, ": ");
    put_value(o, &in->values[n->v1], n->feature);
    break;
  case MQFORM_RANGE:
    if (n->pos == MQPOS_LEFT) {
      put_name(o, pre, post);
      puts_(o, " ");
      puts_(o, op_text(n->op1));
      puts_(o, " ");
      put_value(o, &in->values[n->v1], n->feature);
    } else if (n->pos == MQPOS_RIGHT) {
      put_value(o, &in->values[n->v1], n->feature);
      puts_(o, " ");
      puts_(o, op_text(n->op1));
      puts_(o, " ");
      put_name(o, pre, post);
    } else {
      put_value(o, &in->values[n->v1], n->feature);
      puts_(o, " ");
      puts_(o, op_text(n->op1));
      puts_(o, " ");
      put_name(o, pre, post);
      puts_(o, " ");
      puts_(o, op_text(n->op2));
      puts_(o, " ");
      put_value(o, &in->values[n->v2], n->feature);
    }
    break;
  }
  if (!bare) {
    puts_(o, ")");
  }
}

/// A feature of `style()`, `scroll-state()` or `anchored()`.
static void put_subfeature(out_t * o, const mq_node * n, bool bare) {
  if (!bare) {
    puts_(o, "(");
  }
  switch ((mq_form)n->form) {
  case MQFORM_BOOLEAN:
    put(o, n->text, n->text_length);
    break;
  case MQFORM_PLAIN:
    put(o, n->text, n->text_length);
    puts_(o, ": ");
    put(o, n->value, n->value_length);
    break;
  case MQFORM_RANGE:
    put(o, n->text, n->text_length);
    puts_(o, " ");
    puts_(o, op_text(n->op1));
    puts_(o, " ");
    if (n->name) {
      put(o, n->name, n->name_length);
      puts_(o, " ");
      puts_(o, op_text(n->op2));
      puts_(o, " ");
    }
    put(o, n->value, n->value_length);
    break;
  }
  if (!bare) {
    puts_(o, ")");
  }
}

typedef struct {
  uint32_t node;
  uint32_t idx;
  bool bare;
} sframe;

/// Write the condition tree rooted at `root`.
static GTEXT_CSS_Status write_condition(const struct GTEXT_CSS_Media_List_Internal * in, uint32_t root, out_t * o, sframe * stack) {
  size_t sp = 0;
  stack[sp].node = root;
  stack[sp].idx = 0;
  stack[sp].bare = false;
  sp++;
  while (sp && o->st == GTEXT_CSS_OK) {
    sframe * f = &stack[sp - 1];
    const mq_node * n = &in->nodes[f->node];
    switch ((mq_node_kind)n->kind) {
    case MQN_NOT:
      if (f->idx == 0) {
        puts_(o, "not ");
        f->idx = 1;
        stack[sp].node = in->kids[n->kid_first];
        stack[sp].idx = 0;
        stack[sp].bare = false;
        sp++;
      } else {
        sp--;
      }
      break;
    case MQN_AND:
    case MQN_OR:
      if (f->idx < n->kid_count) {
        if (f->idx > 0) {
          puts_(o, n->kind == MQN_AND ? " and " : " or ");
        }
        uint32_t k = in->kids[n->kid_first + f->idx];
        f->idx++;
        stack[sp].node = k;
        stack[sp].idx = 0;
        stack[sp].bare = false;
        sp++;
      } else {
        sp--;
      }
      break;
    case MQN_PAREN:
      if (f->idx == 0) {
        puts_(o, "(");
        f->idx = 1;
        stack[sp].node = in->kids[n->kid_first];
        stack[sp].idx = 0;
        stack[sp].bare = false;
        sp++;
      } else {
        puts_(o, ")");
        sp--;
      }
      break;
    case MQN_FEATURE:
      put_feature(o, in, n, f->bare);
      sp--;
      break;
    case MQN_SUBFEATURE:
      put_subfeature(o, n, f->bare);
      sp--;
      break;
    case MQN_TEST_MEDIA:
    case MQN_TEST_SUPPORTS:
    case MQN_CQ_FN:
      if (f->idx == 0) {
        puts_(o, n->kind == MQN_TEST_MEDIA ? "media(" : (n->kind == MQN_TEST_SUPPORTS ? "supports(" :
            (n->type_id == MQ_SUB_STYLE ? "style(" : (n->type_id == MQ_SUB_SCROLL ? "scroll-state(" : "anchored("))));
        f->idx = 1;
        const bool bare = (n->flags & MQF_BARE) != 0 || n->kind == MQN_TEST_MEDIA;
        stack[sp].node = in->kids[n->kid_first];
        stack[sp].idx = 0;
        stack[sp].bare = bare;
        sp++;
      } else {
        puts_(o, ")");
        sp--;
      }
      break;
    case MQN_ENCLOSED:
      put(o, n->text, n->text_length);
      sp--;
      break;
    case MQN_CUSTOM:
      if (!f->bare) {
        puts_(o, "(");
      }
      if (o->st == GTEXT_CSS_OK) {
        GTEXT_CSS_Status st = gtext_css_serialize_identifier(n->text, n->text_length, o->sink);
        if (st != GTEXT_CSS_OK) {
          o->st = st;
        }
      }
      if (!f->bare) {
        puts_(o, ")");
      }
      sp--;
      break;
    case MQN_DECL:
      if (!f->bare) {
        puts_(o, "(");
      }
      put(o, n->text, n->text_length);
      puts_(o, ":");
      if (n->value_length) {
        puts_(o, " ");
        put(o, n->value, n->value_length);
      }
      if (n->flags & MQF_IMPORTANT) {
        puts_(o, " !important");
      }
      if (!f->bare) {
        puts_(o, ")");
      }
      sp--;
      break;
    case MQN_SELECTOR:
    case MQN_FONT_TECH:
    case MQN_FONT_FORMAT:
    case MQN_AT_RULE:
    case MQN_NAMED:
    case MQN_ENV:
      put(o, n->value, n->value_length);
      sp--;
      break;
    case MQN_QUERY:
      sp--;
      break;
    }
  }
  return o->st;
}

static GTEXT_CSS_Status write_query(const struct GTEXT_CSS_Media_List_Internal * in, uint32_t qi, out_t * o, sframe * stack) {
  const mq_node * q = &in->nodes[qi];
  if (q->flags & MQF_REPLACED) {
    puts_(o, "not all");
    return o->st;
  }
  bool has_cond = q->kid_count > 0;
  if (!(q->flags & MQF_HAS_TYPE)) {
    if (q->text) {
      /* a container condition: the name, then the query */
      if (o->st == GTEXT_CSS_OK) {
        GTEXT_CSS_Status st = gtext_css_serialize_identifier(q->text, q->text_length, o->sink);
        if (st != GTEXT_CSS_OK) {
          o->st = st;
        }
      }
      if (has_cond) {
        puts_(o, " ");
      }
    }
    if (has_cond) {
      return write_condition(in, in->kids[q->kid_first], o, stack);
    }
    return o->st;
  }
  bool omit_type = has_cond && !(q->flags & (MQF_NEGATED | MQF_ONLY)) && q->type_id == MQT_ID_ALL;
  if (!omit_type) {
    if (q->flags & MQF_NEGATED) {
      puts_(o, "not ");
    }
    if (q->flags & MQF_ONLY) {
      puts_(o, "only ");
    }
    if (o->st == GTEXT_CSS_OK) {
      GTEXT_CSS_Status st = gtext_css_serialize_identifier(q->text, q->text_length, o->sink);
      if (st != GTEXT_CSS_OK) {
        o->st = st;
      }
    }
    if (has_cond) {
      puts_(o, " and ");
    }
  }
  if (has_cond) {
    return write_condition(in, in->kids[q->kid_first], o, stack);
  }
  return o->st;
}

GTEXT_CSS_Status css_mq_serialize_node(const struct GTEXT_CSS_Media_List_Internal * in, uint32_t node, const GTEXT_Markup_Sink * sink) {
  out_t o = {sink, GTEXT_CSS_OK};
  sframe * stack = gtext_allocator_malloc(&in->alloc, (in->node_count + 1) * sizeof(sframe));
  if (!stack) {
    return GTEXT_CSS_E_OOM;
  }
  GTEXT_CSS_Status st = in->nodes[node].kind == MQN_QUERY ? write_query(in, node, &o, stack) : write_condition(in, node, &o, stack);
  gtext_allocator_free(&in->alloc, stack);
  return st;
}

GTEXT_CSS_Status gtext_css_media_list_serialize(const GTEXT_CSS_Media_List * list, const GTEXT_Markup_Sink * sink) {
  if (!list || !sink || !sink->write) {
    return GTEXT_CSS_E_INVALID;
  }
  const struct GTEXT_CSS_Media_List_Internal * in = &list->in;
  out_t o = {sink, GTEXT_CSS_OK};
  sframe * stack = gtext_allocator_malloc(&in->alloc, (in->node_count + 1) * sizeof(sframe));
  if (!stack) {
    return GTEXT_CSS_E_OOM;
  }
  for (size_t i = 0; i < in->root_count && o.st == GTEXT_CSS_OK; i++) {
    if (i) {
      puts_(&o, ", ");
    }
    write_query(in, in->roots[i], &o, stack);
  }
  gtext_allocator_free(&in->alloc, stack);
  return o.st;
}

typedef struct {
  const GTEXT_Allocator * alloc;
  char * data;
  size_t length, cap;
  bool failed;
} sbuf;

static GTEXT_Markup_Status sbuf_write(void * user, const char * bytes, size_t len) {
  sbuf * b = user;
  if (b->failed) {
    return GTEXT_MARKUP_E_OOM;
  }
  if (b->length + len + 1 > b->cap) {
    size_t nc = b->cap ? b->cap : 64;
    while (nc < b->length + len + 1) {
      nc *= 2;
    }
    char * nd = gtext_allocator_realloc(b->alloc, b->data, nc);
    if (!nd) {
      b->failed = true;
      return GTEXT_MARKUP_E_OOM;
    }
    b->data = nd;
    b->cap = nc;
  }
  memcpy(b->data + b->length, bytes, len);
  b->length += len;
  b->data[b->length] = 0;
  return GTEXT_MARKUP_OK;
}

GTEXT_CSS_Status gtext_css_media_list_serialize_buffer(const GTEXT_CSS_Media_List * list, const GTEXT_Allocator * allocator, char ** out, size_t * out_length) {
  if (!list || !out || !out_length) {
    return GTEXT_CSS_E_INVALID;
  }
  sbuf b = {allocator ? allocator : gtext_allocator_default(), NULL, 0, 0, false};
  GTEXT_Markup_Sink sink = {sbuf_write, &b};
  GTEXT_CSS_Status st = gtext_css_media_list_serialize(list, &sink);
  if (st == GTEXT_CSS_OK && !b.data) {
    sbuf_write(&b, "", 0);
    if (!b.data) {
      b.data = gtext_allocator_malloc(b.alloc, 1);
      if (b.data) {
        b.data[0] = 0;
      } else {
        b.failed = true;
      }
    }
  }
  if (b.failed) {
    st = GTEXT_CSS_E_OOM;
  }
  if (st != GTEXT_CSS_OK) {
    gtext_allocator_free(b.alloc, b.data);
    return st;
  }
  *out = b.data;
  *out_length = b.length;
  return GTEXT_CSS_OK;
}

void gtext_css_media_buffer_free(const GTEXT_Allocator * allocator, char * buffer) {
  gtext_allocator_free(allocator ? allocator : gtext_allocator_default(), buffer);
}
