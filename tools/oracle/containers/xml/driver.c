/*
 * The XML reference driver: libxml2 and expat, asked about one document at a
 * time, in the framed and hex-encoded protocol of the other drivers here.
 *
 * Read `<len>\n` then that many bytes, repeated; `-1\n` ends the stream. Every
 * byte string in the output is hex (`-` for absent, `.` for empty), so a document
 * with a newline, a NUL or invalid UTF-8 cannot corrupt the framing.
 *
 * **Two references, and they share no code.** libxml2 is GNOME's C parser and
 * expat is James Clark's; neither includes the other, and they do not agree with
 * each other everywhere (which is the point of asking both). Per document:
 *
 *     BEGIN
 *     XML2 <wellformed 0|1> <namespace-wellformed 0|1> <bad namespace URI 0|1> <first error message>
 *     [DUMP ... ENDDUMP]            only when both flags are 1
 *     EXPAT-NS ok | EXPAT-NS err <code> <message> <byte index of the error>
 *     [DUMP ... ENDDUMP]            only on ok
 *     EXPAT-PLAIN ok | EXPAT-PLAIN err <code> <name>
 *     END
 *
 * `EXPAT-PLAIN` is expat with namespace processing off. It is asked so that a
 * disagreement can be classified: a document ours refuses as a namespace error
 * and that expat accepts without namespaces, is a namespace refusal and not a
 * well-formedness one. No dump is taken from it.
 *
 * **libxml2 runs with recovery off**, which is the only mode in which its
 * well-formedness verdict means anything, and with no entity substitution, no
 * DTD loading, no validation and no network. Its "namespace-wellformed" flag is
 * separate from its well-formed one (a namespace error is reported but does not
 * stop the parse), so both are read.
 *
 * The dump is a flat event list the Python side normalizes (tools/oracle/
 * xml_diff.py), so that normalization is written once and applied to both
 * sides. Lines, all fields hex:
 *
 *     X <version | -> <standalone -1|0|1>
 *     D <name> <public id | -> <system id | ->
 *     S <namespace uri | -> <local name> <prefix | ->     start of an element
 *     N <prefix | -> <uri | .>                            a namespace declaration
 *     A <namespace uri | -> <local name> <prefix | -> <value>
 *     E                                                   end of an element
 *     T <text>   C <cdata>   M <comment>   P <target> <data>
 *     R <name>                                            an entity reference node
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define _POSIX_C_SOURCE 200809L

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlversion.h>
#include <expat.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GTEXT_DRIVER_SHA
#define GTEXT_DRIVER_SHA "unknown"
#endif
#ifndef GTEXT_XML_PACKAGES
#define GTEXT_XML_PACKAGES "unknown"
#endif

static void put_hex(const char *bytes, size_t len) {
  if (!bytes) {
    fputs("-", stdout);
    return;
  }
  if (!len) {
    fputs(".", stdout);
    return;
  }
  for (size_t i = 0; i < len; i++) {
    printf("%02x", (unsigned char)bytes[i]);
  }
}

static void put_str(const char *s) {
  put_hex(s, s ? strlen(s) : 0);
}

/* ---- libxml2 ------------------------------------------------------------ */

static void xml2_dump(xmlDocPtr doc) {
  printf("DUMP\n");
  {
    const int sa = doc->standalone;
    printf("X ");
    put_str((const char *)doc->version);
    printf(" %d\n", sa < 0 ? -1 : sa);
  }
  xmlNodePtr n = doc->children;
  while (n) {
    switch (n->type) {
      case XML_DTD_NODE: {
        xmlDtdPtr d = (xmlDtdPtr)n;
        printf("D ");
        put_str((const char *)d->name);
        printf(" ");
        put_str((const char *)d->ExternalID);
        printf(" ");
        put_str((const char *)d->SystemID);
        printf("\n");
        break;
      }
      case XML_ELEMENT_NODE: {
        printf("S ");
        put_str(n->ns ? (const char *)n->ns->href : NULL);
        printf(" ");
        put_str((const char *)n->name);
        printf(" ");
        put_str(n->ns ? (const char *)n->ns->prefix : NULL);
        printf("\n");
        for (xmlNsPtr ns = n->nsDef; ns; ns = ns->next) {
          printf("N ");
          put_str((const char *)ns->prefix);
          printf(" ");
          put_str(ns->href ? (const char *)ns->href : "");
          printf("\n");
        }
        for (xmlAttrPtr a = n->properties; a; a = a->next) {
          xmlChar *v = xmlNodeGetContent((xmlNodePtr)a);
          printf("A ");
          put_str(a->ns ? (const char *)a->ns->href : NULL);
          printf(" ");
          put_str((const char *)a->name);
          printf(" ");
          put_str(a->ns ? (const char *)a->ns->prefix : NULL);
          printf(" ");
          put_str(v ? (const char *)v : "");
          printf("\n");
          xmlFree(v);
        }
        break;
      }
      case XML_TEXT_NODE:
        printf("T ");
        put_str((const char *)n->content);
        printf("\n");
        break;
      case XML_CDATA_SECTION_NODE:
        printf("C ");
        put_str((const char *)n->content);
        printf("\n");
        break;
      case XML_COMMENT_NODE:
        printf("M ");
        put_str((const char *)n->content);
        printf("\n");
        break;
      case XML_PI_NODE:
        printf("P ");
        put_str((const char *)n->name);
        printf(" ");
        put_str(n->content ? (const char *)n->content : "");
        printf("\n");
        break;
      case XML_ENTITY_REF_NODE:
        printf("R ");
        put_str((const char *)n->name);
        printf("\n");
        break;
      default:
        printf("R ");
        put_str("?");
        printf("\n");
        break;
    }
    /* Depth-first without recursion: a document is attacker-shaped and this
     * walk must not be the thing that overflows the stack. */
    if (n->type == XML_ELEMENT_NODE && n->children) {
      n = n->children;
      continue;
    }
    for (;;) {
      if (n->type == XML_ELEMENT_NODE) {
        /* A childless element ends where it started; a parent ends on the way
         * back up (below). */
        if (!n->children) {
          printf("E\n");
        }
      }
      if (n->next) {
        n = n->next;
        break;
      }
      n = n->parent;
      if (!n || n == (xmlNodePtr)doc) {
        n = NULL;
        break;
      }
      printf("E\n");
    }
  }
  printf("ENDDUMP\n");
}

static void xml2_generic_error(void *ctx, const char *msg, ...) {
  (void)ctx;
  (void)msg;
}

/* The first message of the parse, for the report: it is prose that a
 * comparison must not depend on, and the verdict is the two flags. */
static char xml2_first[256];
static int xml2_ns_error; /* a namespace constraint was violated */
static int xml2_uri_error; /* a namespace name is not a valid URI */

static void xml2_structured_error(void *ctx, xmlError *e) {
  (void)ctx;
  if (e && e->domain == XML_FROM_NAMESPACE) {
    if (e->code == XML_WAR_NS_URI || e->code == XML_WAR_NS_URI_RELATIVE) {
      xml2_uri_error = 1;
    }
    else if (e->level >= XML_ERR_ERROR) {
      xml2_ns_error = 1;
    }
  }
  if (!xml2_first[0] && e && e->message && e->level >= XML_ERR_ERROR) {
    snprintf(xml2_first, sizeof(xml2_first), "%d %s", e->code, e->message);
    for (char *c = xml2_first; *c; c++) {
      if (*c == '\n') {
        *c = ' ';
      }
    }
  }
}

static xmlParserInputPtr xml2_no_loader(const char *url, const char *id,
    xmlParserCtxtPtr ctxt) {
  (void)url;
  (void)id;
  (void)ctxt;
  return NULL;
}

static void ask_xml2(const char *data, size_t len) {
  int wf = 0, nswf = 0;
  xml2_first[0] = '\0';
  xml2_ns_error = 0;
  xml2_uri_error = 0;
  xmlParserCtxtPtr ctxt = xmlNewParserCtxt();
  if (!ctxt) {
    printf("XML2 0 0 0 -\n");
    return;
  }
  xmlDocPtr doc = NULL;
  if (len > 0) {
    doc = xmlCtxtReadMemory(ctxt, data, (int)len, NULL, NULL,
        XML_PARSE_NONET | XML_PARSE_NOWARNING | XML_PARSE_HUGE);
  }
  if (doc && ctxt->wellFormed) {
    wf = 1;
    /*
     * libxml2 clears nsWellFormed for a namespace name that is not a valid URI
     * as well as for a violation of Namespaces in XML. The first is a quality
     * check the specification does not require, so it is reported on its own
     * field and the verdict is the second alone. A document that clears the flag
     * for neither reason is read as not namespace-well-formed.
     */
    nswf = ctxt->nsWellFormed ? 1 : (xml2_ns_error ? 0 : (xml2_uri_error ? 1 : 0));
  }
  printf("XML2 %d %d %d ", wf, nswf, wf && xml2_uri_error ? 1 : 0);
  put_hex(xml2_first, strlen(xml2_first));
  printf("\n");
  if (doc && wf && nswf) {
    xml2_dump(doc);
  }
  if (doc) {
    xmlFreeDoc(doc);
  }
  xmlFreeParserCtxt(ctxt);
}

/* ---- expat -------------------------------------------------------------- */

#define NS_SEP '\x01'

static XML_Parser g_parser;

typedef struct {
  int ns;
  int dump;
  int pending_n;
  struct {
    char *prefix;
    char *uri;
  } pending[256];
} expat_state;

/* Split `uri\x01local\x01prefix` (triplet mode), or a bare name. */
static void put_triplet(const char *name, int ns) {
  if (!ns || !strchr(name, NS_SEP)) {
    printf("- ");
    put_str(name);
    printf(" -");
    return;
  }
  const char *a = name;
  const char *b = strchr(a, NS_SEP);
  const char *c = strchr(b + 1, NS_SEP);
  /* uri \x01 local [\x01 prefix] */
  put_hex(a, (size_t)(b - a));
  printf(" ");
  if (c) {
    put_hex(b + 1, (size_t)(c - b - 1));
    printf(" ");
    put_str(c + 1);
  }
  else {
    put_str(b + 1);
    printf(" -");
  }
}

static void XMLCALL ex_start_ns(void *u, const XML_Char *prefix, const XML_Char *uri) {
  expat_state *s = (expat_state *)u;
  if (s->pending_n < 256) {
    s->pending[s->pending_n].prefix = prefix ? strdup(prefix) : NULL;
    s->pending[s->pending_n].uri = strdup(uri ? uri : "");
    s->pending_n++;
  }
  else {
    /* More declarations on one element than the table holds. Dropping the rest would
       make every document with that many look like a disagreement about namespaces,
       so the driver stops and the run fails with this on its stderr. */
    fprintf(stderr, "xml-driver: more than 256 namespace declarations on one element\n");
    exit(3);
  }
}

static void XMLCALL ex_start(void *u, const XML_Char *name, const XML_Char **atts) {
  expat_state *s = (expat_state *)u;
  printf("S ");
  put_triplet(name, 1);
  printf("\n");
  for (int i = 0; i < s->pending_n; i++) {
    printf("N ");
    put_str(s->pending[i].prefix);
    printf(" ");
    put_str(s->pending[i].uri);
    printf("\n");
    free(s->pending[i].prefix);
    free(s->pending[i].uri);
  }
  s->pending_n = 0;
  int specified = XML_GetSpecifiedAttributeCount(g_parser);
  for (int i = 0; atts[i] && i < specified; i += 2) {
    printf("A ");
    put_triplet(atts[i], 1);
    printf(" ");
    put_str(atts[i + 1]);
    printf("\n");
  }
}

static void XMLCALL ex_end(void *u, const XML_Char *name) {
  (void)u;
  (void)name;
  printf("E\n");
}

static int in_cdata;
/* Comments and instructions inside the document type declaration are the
 * declaration's, not the document's: libxml2 and this parser's tree do not carry
 * them, and expat reports them through the same handlers as the document's own. */
static int in_dtd;

static void XMLCALL ex_chars(void *u, const XML_Char *s, int len) {
  (void)u;
  printf(in_cdata ? "C " : "T ");
  put_hex(s, (size_t)len);
  printf("\n");
}

static void XMLCALL ex_cdata_start(void *u) {
  (void)u;
  in_cdata = 1;
}

static void XMLCALL ex_cdata_end(void *u) {
  (void)u;
  in_cdata = 0;
}

static void XMLCALL ex_comment(void *u, const XML_Char *data) {
  (void)u;
  if (in_dtd) {
    return;
  }
  printf("M ");
  put_str(data);
  printf("\n");
}

static void XMLCALL ex_pi(void *u, const XML_Char *target, const XML_Char *data) {
  (void)u;
  if (in_dtd) {
    return;
  }
  printf("P ");
  put_str(target);
  printf(" ");
  put_str(data ? data : "");
  printf("\n");
}

static void XMLCALL ex_xmldecl(void *u, const XML_Char *version,
    const XML_Char *encoding, int standalone) {
  (void)u;
  (void)encoding;
  printf("X ");
  put_str(version);
  printf(" %d\n", standalone);
}

static void XMLCALL ex_doctype(void *u, const XML_Char *name, const XML_Char *sysid,
    const XML_Char *pubid, int internal) {
  (void)u;
  (void)internal;
  in_dtd = 1;
  printf("D ");
  put_str(name);
  printf(" ");
  put_str(pubid);
  printf(" ");
  put_str(sysid);
  printf("\n");
}

static void XMLCALL ex_doctype_end(void *u) {
  (void)u;
  in_dtd = 0;
}

static void ask_expat(const char *data, size_t len, int ns) {
  expat_state st;
  memset(&st, 0, sizeof(st));
  st.ns = ns;
  XML_Parser p = ns ? XML_ParserCreateNS(NULL, NS_SEP) : XML_ParserCreate(NULL);
  if (!p) {
    printf("%s err 0 4e4f4d454d -1\n", ns ? "EXPAT-NS" : "EXPAT-PLAIN");
    return;
  }
  g_parser = p;
  in_cdata = 0;
  in_dtd = 0;
  XML_SetUserData(p, &st);
  if (ns) {
    XML_SetReturnNSTriplet(p, 1);
    XML_SetNamespaceDeclHandler(p, ex_start_ns, NULL);
  }
  /* Output is buffered so that a refused document prints no dump. */
  fflush(stdout);
  char *buf = NULL;
  size_t buflen = 0;
  FILE *mem = NULL;
  FILE *saved = stdout;
  if (ns) {
    mem = open_memstream(&buf, &buflen);
    stdout = mem;
    XML_SetElementHandler(p, ex_start, ex_end);
    XML_SetCharacterDataHandler(p, ex_chars);
    XML_SetCdataSectionHandler(p, ex_cdata_start, ex_cdata_end);
    XML_SetCommentHandler(p, ex_comment);
    XML_SetProcessingInstructionHandler(p, ex_pi);
    XML_SetXmlDeclHandler(p, ex_xmldecl);
    XML_SetStartDoctypeDeclHandler(p, ex_doctype);
    XML_SetEndDoctypeDeclHandler(p, ex_doctype_end);
  }
  enum XML_Status r = XML_Parse(p, data, (int)len, 1);
  enum XML_Error code = XML_GetErrorCode(p);
  const long at = (long)XML_GetErrorByteIndex(p);
  if (mem) {
    fclose(mem);
    stdout = saved;
  }
  for (int i = 0; i < st.pending_n; i++) {
    free(st.pending[i].prefix);
    free(st.pending[i].uri);
  }
  const char *tag = ns ? "EXPAT-NS" : "EXPAT-PLAIN";
  if (r == XML_STATUS_OK) {
    printf("%s ok\n", tag);
    if (ns) {
      printf("DUMP\n");
      fwrite(buf, 1, buflen, stdout);
      printf("ENDDUMP\n");
    }
  }
  else {
    const char *what = XML_ErrorString(code);
    printf("%s err %d ", tag, (int)code);
    put_str(what);
    printf(" %ld\n", at);
  }
  free(buf);
  XML_ParserFree(p);
}

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "--version") == 0) {
    xmlCheckVersion(LIBXML_VERSION);
    /* expat reports "expat_2.8.3"; the pin names the number, so the prefix is checked
       and not assumed. */
    const char *ev = XML_ExpatVersion();
    if (strncmp(ev, "expat_", 6) != 0) {
      fprintf(stderr, "xml-driver: expat reports its version as '%s', not 'expat_N'\n", ev);
      return 3;
    }
    printf("libxml2 %s, expat %s, %s, driver %s\n", LIBXML_DOTTED_VERSION,
        ev + 6, GTEXT_XML_PACKAGES, GTEXT_DRIVER_SHA);
    return 0;
  }
  xmlSetGenericErrorFunc(NULL, xml2_generic_error);
  xmlSetStructuredErrorFunc(NULL, xml2_structured_error);
  xmlSetExternalEntityLoader(xml2_no_loader);

  char header[64];
  while (fgets(header, sizeof(header), stdin)) {
    /* "<decimal length>\n" exactly: a header that is cut, has trailing junk, is
       negative other than the -1 that ends the stream, or does not fit an int is a
       broken stream, and answering it would score garbage. */
    char *end = NULL;
    const size_t hl = strlen(header);
    if (hl == 0 || header[hl - 1] != '\n') {
      fprintf(stderr, "xml-driver: a header line with no newline\n");
      return 3;
    }
    const long len = strtol(header, &end, 10);
    if (end == header || *end != '\n') {
      fprintf(stderr, "xml-driver: a header line that is not a number\n");
      return 3;
    }
    if (len == -1) {
      break;
    }
    if (len < 0 || len > INT_MAX) {
      fprintf(stderr, "xml-driver: a length of %ld\n", len);
      return 3;
    }
    char *data = malloc((size_t)len + 1);
    if (!data) {
      return 3;
    }
    if (len && fread(data, 1, (size_t)len, stdin) != (size_t)len) {
      free(data);
      return 3;
    }
    data[len] = '\0';
    printf("BEGIN\n");
    ask_xml2(data, (size_t)len);
    ask_expat(data, (size_t)len, 1);
    ask_expat(data, (size_t)len, 0);
    printf("END\n");
    fflush(stdout);
    free(data);
  }
  return 0;
}
