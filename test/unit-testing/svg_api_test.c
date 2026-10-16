/* ex: ft=c: -*- mode: c; -*- */
/*****************************************************************************/
/*  LibreDWG - free implementation of the DWG file format                    */
/*                                                                           */
/*  Copyright (C) 2026 Red River Software                                    */
/*                                                                           */
/*  This library is free software, licensed under the terms of the GNU       */
/*  General Public License as published by the Free Software Foundation,     */
/*  either version 3 of the License, or (at your option) any later version.  */
/*  You should have received a copy of the GNU General Public License        */
/*  along with this program.  If not, see <http://www.gnu.org/licenses/>.    */
/*****************************************************************************/

/* SVG output of in-memory drawings through dwg_data_to_svg, the library API
   to dwg2SVG's renderer:
   - a rotated TEXT with a width factor is anchored at its insertion point,
     at the top level and in a block;
   - one write longer than dwg_svg_printf's 8 KB stack buffer (a long TEXT
     value) reaches the SVG whole;
   - MTEXT comes out as UTF-8 whichever way the document holds its text: in
     the codepage (read from a pre-r2007 DWG) or in UTF-16 (read from an
     r2007+ DWG), at the top level and in a block, or in UTF-8 (built with
     the add API), at the top level; and word wrap counts characters, not
     bytes. indxf_test checks DXF imports. */

#define SVG_API_TEST_C
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// the library sets loglevel from the document's opts; ok needs ours
static unsigned int test_loglevel;

#include "config.h"
#include "dwg.h"
#include "tests_common.h"
#include "dwg_api.h"
#include "dwg_svg_api.h"
#include "codepages.h"

/* The SVG transform matrix(a b c d e f): x' = a x + c y + e,
   y' = b x + d y + f. */
typedef struct
{
  double a, b, c, d, e, f;
} Affine;

static const Affine identity = { 1.0, 0.0, 0.0, 1.0, 0.0, 0.0 };

/* m after n: the transform list "m n" applies n to a point first. */
static Affine
affine_mul (Affine m, Affine n)
{
  Affine r;
  r.a = m.a * n.a + m.c * n.b;
  r.b = m.b * n.a + m.d * n.b;
  r.c = m.a * n.c + m.c * n.d;
  r.d = m.b * n.c + m.d * n.d;
  r.e = m.a * n.e + m.c * n.f + m.e;
  r.f = m.b * n.e + m.d * n.f + m.f;
  return r;
}

/* Parses an SVG transform list made of matrix, translate, rotate and scale.
   Returns 0 on anything else. */
static int
parse_transform (const char *s, Affine *out)
{
  Affine m = identity;
  while (*s)
    {
      char name[16];
      double v[6];
      int n = 0, len = 0;
      Affine t = identity;
      while (*s == ' ' || *s == ',')
        s++;
      if (!*s)
        break;
      while (s[len] && s[len] != '(' && len < (int)sizeof (name) - 1)
        {
          name[len] = s[len];
          len++;
        }
      name[len] = '\0';
      if (s[len] != '(')
        return 0;
      s += len + 1;
      while (*s && *s != ')')
        {
          char *next;
          if (*s == ' ' || *s == ',')
            {
              s++;
              continue;
            }
          if (n == 6)
            return 0;
          v[n++] = strtod (s, &next);
          if (next == s)
            return 0;
          s = next;
        }
      if (*s != ')')
        return 0;
      s++;
      if (!strcmp (name, "matrix") && n == 6)
        {
          t.a = v[0];
          t.b = v[1];
          t.c = v[2];
          t.d = v[3];
          t.e = v[4];
          t.f = v[5];
        }
      else if (!strcmp (name, "translate") && (n == 1 || n == 2))
        {
          t.e = v[0];
          t.f = n == 2 ? v[1] : 0.0;
        }
      else if (!strcmp (name, "scale") && (n == 1 || n == 2))
        {
          t.a = v[0];
          t.d = n == 2 ? v[1] : v[0];
        }
      else if (!strcmp (name, "rotate") && (n == 1 || n == 3))
        {
          double rad = v[0] * M_PI / 180.0;
          double cx = n == 3 ? v[1] : 0.0;
          double cy = n == 3 ? v[2] : 0.0;
          t.a = cos (rad);
          t.b = sin (rad);
          t.c = -t.b;
          t.d = t.a;
          // rotate(a cx cy) is translate(cx cy) rotate(a) translate(-cx -cy)
          t.e = cx - t.a * cx - t.c * cy;
          t.f = cy - t.b * cx - t.d * cy;
        }
      else
        return 0;
      m = affine_mul (m, t);
    }
  *out = m;
  return 1;
}

/* Copies the value of attribute name in the start tag tag into out. */
static int
attr_value (const char *tag, const char *name, char *out, size_t size)
{
  char key[32];
  const char *p, *end;
  snprintf (key, sizeof (key), " %s=\"", name);
  p = strstr (tag, key);
  if (!p)
    return 0;
  p += strlen (key);
  end = strchr (p, '"');
  if (!end || (size_t)(end - p) >= size)
    return 0;
  memcpy (out, p, end - p);
  out[end - p] = '\0';
  return 1;
}

/* The point the <text> element dwg2SVG wrote for text is anchored at: its
   own x and y, through its transform. */
static int
text_anchor (const char *svg, const Dwg_Entity_TEXT *text, double *x,
             double *y)
{
  char id[64], tag[1024], value[256];
  const char *start, *end;
  double tx, ty;
  Affine m = identity;

  snprintf (id, sizeof (id), "<text id=\"dwg-object-%u\" ",
            (unsigned)text->parent->objid);
  start = strstr (svg, id);
  if (!start)
    return 0;
  end = strchr (start, '>');
  if (!end || (size_t)(end - start) >= sizeof (tag))
    return 0;
  memcpy (tag, start, end - start);
  tag[end - start] = '\0';

  if (!attr_value (tag, "x", value, sizeof (value)))
    return 0;
  tx = strtod (value, NULL);
  if (!attr_value (tag, "y", value, sizeof (value)))
    return 0;
  ty = strtod (value, NULL);
  if (attr_value (tag, "transform", value, sizeof (value))
      && !parse_transform (value, &m))
    return 0;
  *x = m.a * tx + m.c * ty + m.e;
  *y = m.b * tx + m.d * ty + m.f;
  return 1;
}

static char *
render_mspace (Dwg_Data *dwg, size_t *len)
{
  char *svg = NULL;
  int error = dwg_data_to_svg (dwg, &svg, len, 1);
  loglevel = test_loglevel;
  if (error || !svg)
    {
      fail ("dwg_data_to_svg: error %d", error);
      return NULL;
    }
  return svg;
}

/* The model space of a new document, or NULL, after a failed check. Sets
   *dwgp in both cases, to the document or NULL, for the caller to free. */
static Dwg_Object_BLOCK_HEADER *
new_document (Dwg_Data **dwgp)
{
  Dwg_Data *dwg = dwg_new_Document (R_2000, 0, 0);
  const Dwg_Object *ms = dwg ? dwg_model_space_object (dwg) : NULL;
  loglevel = test_loglevel;
  *dwgp = dwg;
  if (!ms)
    {
      fail ("dwg_new_Document: %s", dwg ? "no model space" : "no document");
      return NULL;
    }
  return ms->tio.object->tio.BLOCK_HEADER;
}

static Dwg_Entity_TEXT *
add_text (Dwg_Object_BLOCK_HEADER *bh, const char *value,
          const dwg_point_3d *ins_pt, double rotation_deg,
          double width_factor)
{
  Dwg_Entity_TEXT *text = dwg_add_TEXT (bh, value, ins_pt, 2.5);
  loglevel = test_loglevel;
  if (!text)
    {
      fail ("dwg_add_TEXT \"%s\" failed", value);
      return NULL;
    }
  text->rotation = rotation_deg * M_PI / 180.0;
  text->width_factor = width_factor;
  return text;
}

#define NUM_CASES 4

/* TEXTs at one insertion point: a plain one, which the others must be
   anchored with, then the rotation and width factor cases. Returns 0 when
   one isn't added. */
static int
add_texts (Dwg_Object_BLOCK_HEADER *bh, const dwg_point_3d *ins_pt,
           Dwg_Entity_TEXT **plain, Dwg_Entity_TEXT **cases)
{
  *plain = add_text (bh, "PLAIN", ins_pt, 0.0, 1.0);
  cases[0] = add_text (bh, "ROTATED", ins_pt, 30.0, 1.0);
  cases[1] = add_text (bh, "WIDE", ins_pt, 0.0, 0.8);
  cases[2] = add_text (bh, "ROTATED WIDE", ins_pt, 30.0, 0.8);
  cases[3] = add_text (bh, "ROTATED STRETCHED", ins_pt, 135.0, 1.5);
  return *plain && cases[0] && cases[1] && cases[2] && cases[3];
}

static void
check_anchors (const char *svg, const char *frame,
               const Dwg_Entity_TEXT *plain, Dwg_Entity_TEXT **cases)
{
  double px, py;
  if (!text_anchor (svg, plain, &px, &py))
    {
      fail ("%s: no <text> for the plain TEXT", frame);
      return;
    }
  for (int i = 0; i < NUM_CASES; i++)
    {
      const Dwg_Entity_TEXT *text = cases[i];
      double rotation_deg = text->rotation * 180.0 / M_PI;
      double x, y;
      if (!text_anchor (svg, text, &x, &y))
        fail ("%s: no <text> for the TEXT rotated %g with width factor %g",
              frame, rotation_deg, text->width_factor);
      else if (fabs (x - px) < 1e-3 && fabs (y - py) < 1e-3)
        ok ("%s: TEXT rotated %g with width factor %g anchored at (%f, %f)",
            frame, rotation_deg, text->width_factor, x, y);
      else
        fail ("%s: TEXT rotated %g with width factor %g anchored at "
              "(%f, %f), not at its insertion point (%f, %f)",
              frame, rotation_deg, text->width_factor, x, y, px, py);
    }
}

/* A TEXT turns about its insertion point, whatever its width factor. A
   rotated TEXT with a width factor gets rotate() and scale() at the top
   level, where the output is Y-down, and one matrix() in a block. */
static void
test_rotated_text_anchor (void)
{
  Dwg_Data *dwg;
  Dwg_Object_BLOCK_HEADER *ms = new_document (&dwg);
  Dwg_Object_BLOCK_HEADER *blk;
  Dwg_Entity_TEXT *top_plain, *top_cases[NUM_CASES];
  Dwg_Entity_TEXT *blk_plain, *blk_cases[NUM_CASES];
  dwg_point_3d p0 = { 0.0, 0.0, 0.0 }, p1 = { 400.0, 200.0, 0.0 };
  dwg_point_3d ins_pt = { 100.0, 50.0, 0.0 }, blk_pt = { 20.0, 10.0, 0.0 };
  dwg_point_3d insert_pt = { 300.0, 100.0, 0.0 };
  char *svg;
  size_t len;

  if (!ms)
    goto done;
  // the extents start at the origin, so the insertion point is far from
  // the SVG's origin and a wrong pivot moves the text visibly
  dwg_add_LINE (ms, &p0, &p1);
  if (!add_texts (ms, &ins_pt, &top_plain, top_cases))
    goto done;

  blk = dwg_add_BLOCK_HEADER (dwg, "LABELS");
  loglevel = test_loglevel;
  if (!blk)
    {
      fail ("dwg_add_BLOCK_HEADER \"LABELS\" failed");
      goto done;
    }
  dwg_add_BLOCK (blk, "LABELS");
  if (!add_texts (blk, &blk_pt, &blk_plain, blk_cases))
    goto done;
  dwg_add_ENDBLK (blk);
  dwg_add_INSERT (ms, &insert_pt, "LABELS", 1.0, 1.0, 1.0, 0.0);

  svg = render_mspace (dwg, &len);
  if (svg)
    {
      check_anchors (svg, "top level", top_plain, top_cases);
      check_anchors (svg, "block", blk_plain, blk_cases);
      dwg_free_svg (svg);
    }
done:
  if (dwg)
    {
      dwg_free (dwg);
      free (dwg);
    }
  loglevel = test_loglevel;
}

/* "A&" escapes to "A&amp;", so 2000 of them make the TEXT one write of about
   12 KB: more than dwg_svg_printf's 8 KB stack buffer, from a value that
   fits in it. */
#define LONG_REPEAT 2000

static void
test_long_write (void)
{
  // before any check, so that done: frees both whichever check fails
  char *value = (char *)malloc (2 * LONG_REPEAT + 1);
  char *expected = (char *)malloc (6 * LONG_REPEAT + sizeof ("</text>"));
  Dwg_Data *dwg = NULL;
  Dwg_Object_BLOCK_HEADER *ms;
  dwg_point_3d ins_pt = { 10.0, 10.0, 0.0 };
  char *svg;
  size_t len;

  if (!value || !expected)
    {
      fail ("out of memory");
      goto done;
    }
  ms = new_document (&dwg);
  if (!ms)
    goto done;
  for (int i = 0; i < LONG_REPEAT; i++)
    {
      memcpy (value + 2 * i, "A&", 2);
      memcpy (expected + 6 * i, "A&amp;", 6);
    }
  value[2 * LONG_REPEAT] = '\0';
  strcpy (expected + 6 * LONG_REPEAT, "</text>");
  if (!dwg_add_TEXT (ms, value, &ins_pt, 2.5))
    {
      loglevel = test_loglevel;
      fail ("dwg_add_TEXT of %d bytes failed", 2 * LONG_REPEAT);
      goto done;
    }

  svg = render_mspace (dwg, &len);
  if (svg)
    {
      if (len == strlen (svg))
        ok ("long write: svg_len %lu is the string's length",
            (unsigned long)len);
      else
        fail ("long write: svg_len %lu, the string is %lu long",
              (unsigned long)len, (unsigned long)strlen (svg));
      if (strstr (svg, expected))
        ok ("long write: the escaped TEXT value is whole, then </text>");
      else
        fail ("long write: the escaped TEXT value of %d bytes is cut short",
              6 * LONG_REPEAT);
      if (strstr (svg, "</svg>"))
        ok ("long write: the SVG is closed");
      else
        fail ("long write: no </svg>");
      dwg_free_svg (svg);
    }
done:
  free (value);
  free (expected);
  if (dwg)
    {
      dwg_free (dwg);
      free (dwg);
    }
  loglevel = test_loglevel;
}

/* Finds test/test-data/name from the build or source directory. */
static int
test_file (const char *name, char *path, const size_t size)
{
  struct stat st;
  const char *top_srcdir = getenv ("top_srcdir");
  const char *const prefixes[]
      = { "../test-data", "../../test/test-data", "../../../test/test-data",
          "../../../../test/test-data", NULL };
  if (top_srcdir)
    {
      snprintf (path, size, "%s/test/test-data/%s", top_srcdir, name);
      if (!stat (path, &st))
        return 1;
    }
  for (const char *const *p = prefixes; *p; p++)
    {
      snprintf (path, size, "%s/%s", *p, name);
      if (!stat (path, &st))
        return 1;
    }
  return 0;
}

/* Reads test/test-data/name into dwg as dwg2SVG reads a drawing. Returns 0,
   after a failed check, when it can't; dwg is for the caller to free either
   way. */
static int
read_test_dwg (const char *name, Dwg_Data *dwg)
{
  char path[1024];
  int error;
  memset (dwg, 0, sizeof (Dwg_Data));
  if (!test_file (name, path, sizeof (path)))
    {
      fail ("test-data/%s not found", name);
      return 0;
    }
  error = dwg_read_file (path, dwg);
  loglevel = test_loglevel;
  if (error >= DWG_ERR_CRITICAL)
    {
      fail ("dwg_read_file %s: error 0x%x", name, error);
      return 0;
    }
  return 1;
}

static void
free_test_dwg (Dwg_Data *dwg)
{
  dwg_free (dwg);
  loglevel = test_loglevel;
}

/* 1 when s is well-formed UTF-8: no overlong forms, surrogates or code
   points above U+10FFFF. */
static int
utf8_valid (const char *s)
{
  const unsigned char *p = (const unsigned char *)s;
  while (*p)
    {
      int n;
      // the range of the second byte of a multi-byte sequence
      unsigned char lo = 0x80, hi = 0xBF;
      if (*p < 0x80)
        n = 0;
      else if (*p >= 0xC2 && *p <= 0xDF)
        n = 1;
      else if (*p >= 0xE0 && *p <= 0xEF)
        {
          n = 2;
          if (*p == 0xE0)
            lo = 0xA0;
          else if (*p == 0xED)
            hi = 0x9F;
        }
      else if (*p >= 0xF0 && *p <= 0xF4)
        {
          n = 3;
          if (*p == 0xF0)
            lo = 0x90;
          else if (*p == 0xF4)
            hi = 0x8F;
        }
      else
        return 0;
      p++;
      if (n && (*p < lo || *p > hi))
        return 0;
      while (n--)
        {
          if ((*p & 0xC0) != 0x80)
            return 0;
          p++;
        }
    }
  return 1;
}

/* utf8_valid, which the checks below rely on. */
static void
test_utf8_valid (void)
{
  static const struct
  {
    const char *s;
    int valid;
    const char *what;
  } cases[] = {
    { "caf\xC3\xA9 \xE2\x82\xAC", 1, "2 and 3 bytes" },
    { "\xF4\x8F\xBF\xBF", 1, "U+10FFFF" },
    { "caf\xE9", 0, "a codepage byte" },
    { "\xC3", 0, "a cut sequence" },
    { "\xA9", 0, "a lone continuation byte" },
    { "\xC0\xAF", 0, "an overlong 2 bytes" },
    { "\xE0\x80\x80", 0, "an overlong 3 bytes" },
    { "\xED\xA0\x80", 0, "a surrogate" },
    { "\xF0\x80\x80\x80", 0, "an overlong 4 bytes" },
    { "\xF4\x90\x80\x80", 0, "above U+10FFFF" },
  };
  for (size_t i = 0; i < sizeof (cases) / sizeof (cases[0]); i++)
    if (utf8_valid (cases[i].s) == cases[i].valid)
      ok ("utf8_valid: %s", cases[i].what);
    else
      fail ("utf8_valid: %s taken as %s", cases[i].what,
            cases[i].valid ? "not UTF-8" : "UTF-8");
}

/* The characters of the <text> element dwg2SVG wrote for the object with
   index objid, its tags left out, into out, and the number of <tspan>
   elements in it into *tspans. Returns 0 when there is none. */
static int
text_content (const char *svg, unsigned objid, char *out, size_t size,
              int *tspans)
{
  char id[64];
  const char *p, *end;
  size_t n = 0;

  snprintf (id, sizeof (id), "<text id=\"dwg-object-%u\" ", objid);
  p = strstr (svg, id);
  if (!p)
    return 0;
  p = strchr (p, '>');
  end = p ? strstr (p, "</text>") : NULL;
  if (!end)
    return 0;
  *tspans = 0;
  for (p++; p < end; p++)
    {
      if (*p == '<')
        {
          if (!strncmp (p, "<tspan", 6))
            (*tspans)++;
          p = strchr (p, '>');
          if (!p || p > end)
            return 0;
          continue;
        }
      if (n + 1 >= size)
        return 0;
      out[n++] = *p;
    }
  out[n] = '\0';
  return 1;
}

/* Whether the TEXT or MTEXT string t starts with the ASCII prefix, read as
   UTF-16 when the document holds text that way. */
static int
text_starts_with (const Dwg_Data *dwg, const char *t, const char *prefix)
{
  if (!t)
    return 0;
  if (dwg->header.from_version >= R_2007 && !(dwg->opts & DWG_OPTS_IN))
    {
      const uint16_t *tu = (const uint16_t *)t;
      size_t i;
      for (i = 0; prefix[i]; i++)
        if (tu[i] != (unsigned char)prefix[i])
          return 0;
      return 1;
    }
  return !strncmp (t, prefix, strlen (prefix));
}

/* The MTEXT of example_2004.dwg and example_2010.dwg, in model space. */
static Dwg_Entity_MTEXT *
example_mtext (Dwg_Data *dwg)
{
  for (BITCODE_BL i = 0; i < dwg->num_objects; i++)
    {
      Dwg_Object *obj = &dwg->object[i];
      if (obj->fixedtype == DWG_TYPE_MTEXT
          && text_starts_with (dwg, obj->tio.entity->tio.MTEXT->text,
                               "Teksto granda"))
        return obj->tio.entity->tio.MTEXT;
    }
  fail ("no MTEXT \"Teksto granda ...\"");
  return NULL;
}

/* The block of example_2004.dwg and example_2010.dwg that model space
   inserts with attributes. */
static Dwg_Object_BLOCK_HEADER *
example_block (Dwg_Data *dwg)
{
  for (BITCODE_BL i = 0; i < dwg->num_objects; i++)
    {
      Dwg_Object *obj = &dwg->object[i];
      if (obj->fixedtype == DWG_TYPE_INSERT
          && obj->tio.entity->tio.INSERT->has_attribs)
        {
          Dwg_Object *blk = dwg_ref_object (
              dwg, obj->tio.entity->tio.INSERT->block_header);
          if (blk && blk->fixedtype == DWG_TYPE_BLOCK_HEADER)
            return blk->tio.object->tio.BLOCK_HEADER;
        }
    }
  fail ("no INSERT with attributes");
  return NULL;
}

/* Renders model space and checks the MTEXT's <text> holds expected: whole,
   as UTF-8, in a well-formed UTF-8 SVG. Sets *tspans if not NULL. */
static void
check_mtext (Dwg_Data *dwg, const Dwg_Entity_MTEXT *mtext,
             const char *expected, const char *what, int *tspans)
{
  char content[512];
  int n = 0;
  size_t len;
  char *svg = render_mspace (dwg, &len);
  if (!svg)
    return;
  if (!utf8_valid (svg))
    fail ("%s: the SVG is not well-formed UTF-8", what);
  else if (!text_content (svg, mtext->parent->objid, content,
                          sizeof (content), &n))
    fail ("%s: no <text> for the MTEXT", what);
  else if (strcmp (content, expected))
    fail ("%s: \"%s\", not \"%s\"", what, content, expected);
  else
    ok ("%s: \"%s\"", what, content);
  if (tspans)
    *tspans = n;
  dwg_free_svg (svg);
}

/* Replaces the MTEXT's text with a copy of the n bytes at text, a string
   as the document holds it. */
static int
set_mtext (Dwg_Entity_MTEXT *mtext, const void *text, size_t n)
{
  char *copy = (char *)malloc (n);
  if (!copy)
    {
      fail ("out of memory");
      return 0;
    }
  memcpy (copy, text, n);
  free (mtext->text);
  mtext->text = copy;
  return 1;
}

/* example_2004.dwg holds text as AutoCAD wrote it, in its codepage. */
static void
test_mtext_codepage (void)
{
  static const struct
  {
    BITCODE_RS codepage;
    const char *text;
    const char *expected;
    const char *what;
  } cases[] = {
    { CP_ANSI_1252, "caf\xE9 30\xB0 10 mm\xB2 \x80 5",
      "caf\xC3\xA9 30\xC2\xB0 10 mm\xC2\xB2 \xE2\x82\xAC 5",
      "r2004 ANSI_1252" },
    { CP_ANSI_1252, "\\U+03B4 %%d %%c caf\xE9",
      "\xCE\xB4 \xC2\xB0 \xE2\x8C\x80 caf\xC3\xA9",
      "r2004 ANSI_1252 with \\U+, %%d and %%c" },
    { CP_ANSI_1252, "a\x81z", "a\xEF\xBF\xBDz",
      "r2004 ANSI_1252 byte it doesn't define" },
    { CP_ANSI_1251, "\xCF\xF0\xE8\xE2\xE5\xF2",
      "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82", "r2004 ANSI_1251" },
    { CP_ANSI_936, "\xB7\xBF\xBC\xE4 2", "\xE6\x88\xBF\xE9\x97\xB4 2",
      "r2004 ANSI_936 two-byte characters" },
    { CP_ANSI_936, "A\xB7", "A\xEF\xBF\xBD",
      "r2004 ANSI_936 lead byte at the end" },
    { CP_ANSI_936, "\x80" "A \xB7 B", "\xE2\x82\xAC" "A \xEF\xBF\xBD B",
      "r2004 ANSI_936 euro sign and a lead byte without its second" },
    { CP_JOHAB, "\x88\x61 A", "\xEA\xB0\x80 A", "r2004 JOHAB" },
    { CP_ANSI_1361, "\x88\x61 A", "\xEA\xB0\x80 A", "r2004 ANSI_1361" },
    { CP_ANSI_932, "\x95\x5C" "P1", "\xE8\xA1\xA8P1",
      "r2004 ANSI_932 second byte a backslash, not \\P" },
    { CP_ISO_8859_1, "caf\xE9", "caf\xC3\xA9", "r2004 ISO_8859_1" },
    { CP_US_ASCII, "caf\xE9", "caf\xEF\xBF\xBD", "r2004 US_ASCII" },
    { CP_UTF8, "caf\xC3\xA9", "caf\xC3\xA9", "r2004 UTF8" },
    // the tables map 0x5C (CP932) and 0x25 (CP864) to other characters
    { CP_CP932, "A\\PB %%d", "AB \xC2\xB0", "r2004 CP932 keeps \\P and %%d" },
    { CP_CP864, "50%%d", "50\xC2\xB0", "r2004 CP864 keeps %%d" },
  };
  Dwg_Data dwg;
  Dwg_Entity_MTEXT *mtext, *added;
  Dwg_Object_BLOCK_HEADER *blk;
  const dwg_point_3d pt = { 1.0, 1.0, 0.0 };
  BITCODE_RS codepage;

  if (!read_test_dwg ("example_2004.dwg", &dwg))
    goto done;
  if (dwg.header.codepage != CP_ANSI_1252)
    {
      fail ("example_2004.dwg: codepage %u, not ANSI_1252",
            (unsigned)dwg.header.codepage);
      goto done;
    }
  mtext = example_mtext (&dwg);
  if (!mtext)
    goto done;
  // its box wraps it after "por", and the space at the break isn't drawn
  check_mtext (&dwg, mtext,
               "Teksto granda nur portesti.Alia linio.Alia pli.",
               "r2004 example", NULL);

  mtext->rect_width = 0.0; // one line
  codepage = dwg.header.codepage;
  for (size_t i = 0; i < sizeof (cases) / sizeof (cases[0]); i++)
    {
      dwg.header.codepage = cases[i].codepage;
      if (set_mtext (mtext, cases[i].text, strlen (cases[i].text) + 1))
        check_mtext (&dwg, mtext, cases[i].expected, cases[i].what, NULL);
    }
  dwg.header.codepage = codepage;

  // the add API converts UTF-8 to the codepage for a document read from a
  // file
  blk = example_block (&dwg);
  if (!blk)
    goto done;
  added = dwg_add_MTEXT (blk, &pt, 0.0, "Stra\xC3\x9F" "e caf\xC3\xA9");
  loglevel = test_loglevel;
  if (!added)
    fail ("dwg_add_MTEXT in the block failed");
  else if (strcmp (added->text, "Stra\xDF" "e caf\xE9"))
    fail ("dwg_add_MTEXT in the block: not held in ANSI_1252");
  else
    check_mtext (&dwg, added, "Stra\xC3\x9F" "e caf\xC3\xA9",
                 "r2004 MTEXT added to a block", NULL);
done:
  free_test_dwg (&dwg);
}

/* example_2010.dwg holds text in UTF-16, as every r2007+ DWG does. The same
   drawing as r2007, example_2007.dwg, can't be read on big-endian hosts. */
static void
test_mtext_utf16 (void)
{
  // "caf", e-acute, " 30", degree, " ", delta, " ", euro; then the MTEXT
  // escapes for delta and degree
  static const uint16_t cafe[] = { 'c', 'a', 'f',   0xE9, ' ', '3',    '0',
                                   0xB0, ' ', 0x3B4, ' ',  0x20AC, 0 };
  static const uint16_t escapes[]
      = { '\\', 'U', '+', '0', '3', 'B', '4', ' ', '%', '%', 'd', 0 };
  Dwg_Data dwg;
  Dwg_Entity_MTEXT *mtext, *added;
  Dwg_Object_BLOCK_HEADER *blk;
  const dwg_point_3d pt = { 1.0, 1.0, 0.0 };

  if (!read_test_dwg ("example_2010.dwg", &dwg))
    goto done;
  mtext = example_mtext (&dwg);
  if (!mtext)
    goto done;
  // every character, not the first only
  check_mtext (&dwg, mtext,
               "Teksto granda nur portesti.Alia linio.Alia pli.",
               "r2010 example", NULL);

  mtext->rect_width = 0.0; // one line
  if (set_mtext (mtext, cafe, sizeof (cafe)))
    check_mtext (&dwg, mtext,
                 "caf\xC3\xA9 30\xC2\xB0 \xCE\xB4 \xE2\x82\xAC", "r2010",
                 NULL);
  if (set_mtext (mtext, escapes, sizeof (escapes)))
    check_mtext (&dwg, mtext, "\xCE\xB4 \xC2\xB0",
                 "r2010 with \\U+ and %%d", NULL);

  // the add API converts UTF-8 to UTF-16 for a document read from a file
  blk = example_block (&dwg);
  if (!blk)
    goto done;
  added = dwg_add_MTEXT (blk, &pt, 0.0, "Stra\xC3\x9F" "e caf\xC3\xA9");
  loglevel = test_loglevel;
  if (!added)
    fail ("dwg_add_MTEXT in the block failed");
  else
    check_mtext (&dwg, added, "Stra\xC3\x9F" "e caf\xC3\xA9",
                 "r2010 MTEXT added to a block", NULL);
done:
  free_test_dwg (&dwg);
}

/* gh109_1.dwg, an r2013 drawing with Chinese MTEXT. */
static void
test_mtext_utf16_cjk (void)
{
  Dwg_Data dwg;
  char *svg;
  size_t len;

  if (!read_test_dwg ("2013/gh109_1.dwg", &dwg))
    goto done;
  svg = render_mspace (&dwg, &len);
  if (svg)
    {
      if (!utf8_valid (svg))
        fail ("gh109_1: the SVG is not well-formed UTF-8");
      // U+623F U+95F4
      else if (!strstr (svg, ">\xE6\x88\xBF\xE9\x97\xB4</tspan>"))
        fail ("gh109_1: no MTEXT \"\xE6\x88\xBF\xE9\x97\xB4\"");
      else
        ok ("gh109_1: MTEXT \"\xE6\x88\xBF\xE9\x97\xB4\"");
      dwg_free_svg (svg);
    }
done:
  free_test_dwg (&dwg);
}

/* A document built with the add API (DWG_OPTS_INJSON) holds text in UTF-8,
   as the writer reads it, and the renderer takes it as it is. */
static void
test_mtext_utf8 (void)
{
  static const char text[]
      = "caf\xC3\xA9 30\xC2\xB0 10 mm\xC2\xB2 \xE2\x82\xAC 5";
  Dwg_Data *dwg;
  Dwg_Object_BLOCK_HEADER *ms = new_document (&dwg);
  Dwg_Entity_MTEXT *mtext;
  const dwg_point_3d pt = { 1.0, 1.0, 0.0 };

  if (!ms)
    goto done;
  mtext = dwg_add_MTEXT (ms, &pt, 0.0, text);
  loglevel = test_loglevel;
  if (!mtext)
    fail ("dwg_add_MTEXT failed");
  else if (strcmp (mtext->text, text))
    fail ("dwg_add_MTEXT: the text is not held in UTF-8");
  else
    check_mtext (dwg, mtext, text, "add API r2000 UTF-8", NULL);
done:
  if (dwg)
    {
      dwg_free (dwg);
      free (dwg);
    }
  loglevel = test_loglevel;
}

#define E10 "eeeeeeeeee"
#define CP_E10 "\xE9\xE9\xE9\xE9\xE9\xE9\xE9\xE9\xE9\xE9"
#define U8_E10                                                                \
  "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"  \
  "\xC3\xA9"

/* Word wrap counts characters: an MTEXT of e-acute wraps where one of 'e'
   does, also after a run that ends part of the way along a line, and a
   break never splits a character. The text is set in the codepage, and the
   <text> holds it without the spaces at the breaks, in <tspan>s. */
static void
test_mtext_wrap (void)
{
  static const struct
  {
    const char *ascii, *ascii_expected, *codepage, *utf8_expected, *what;
  } cases[] = {
    { E10 E10 E10 E10, E10 E10 E10 E10, CP_E10 CP_E10 CP_E10 CP_E10,
      U8_E10 U8_E10 U8_E10 U8_E10, "wrap inside a word" },
    { "cafe cafe cafe cafe cafe cafe cafe cafe",
      "cafecafecafecafecafecafecafecafe",
      "caf\xE9 caf\xE9 caf\xE9 caf\xE9 caf\xE9 caf\xE9 caf\xE9 caf\xE9",
      "caf\xC3\xA9" "caf\xC3\xA9" "caf\xC3\xA9" "caf\xC3\xA9" "caf\xC3\xA9"
      "caf\xC3\xA9" "caf\xC3\xA9" "caf\xC3\xA9",
      "wrap between words" },
    { "{\\C1;ee}" E10, "ee" E10, "{\\C1;\xE9\xE9}" CP_E10,
      "\xC3\xA9\xC3\xA9" U8_E10, "wrap after a coloured run" },
    { "\\See^2;" E10, "ee/2" E10, "\\S\xE9\xE9^2;" CP_E10,
      "\xC3\xA9\xC3\xA9/2" U8_E10, "wrap after a stacked fraction" },
  };
  Dwg_Data dwg;
  Dwg_Entity_MTEXT *mtext;

  if (!read_test_dwg ("example_2004.dwg", &dwg))
    goto done;
  mtext = example_mtext (&dwg);
  if (!mtext)
    goto done;
  // a few characters a line
  mtext->rect_width = 5.0 * mtext->text_height;
  for (size_t i = 0; i < sizeof (cases) / sizeof (cases[0]); i++)
    {
      int ascii_lines = 0, lines = 0;
      char what[64];
      snprintf (what, sizeof (what), "%s, ASCII", cases[i].what);
      if (!set_mtext (mtext, cases[i].ascii, strlen (cases[i].ascii) + 1))
        continue;
      check_mtext (&dwg, mtext, cases[i].ascii_expected, what, &ascii_lines);
      if (ascii_lines < 2)
        fail ("%s: %d lines, the test needs it wrapped", what, ascii_lines);
      snprintf (what, sizeof (what), "%s, ANSI_1252", cases[i].what);
      if (!set_mtext (mtext, cases[i].codepage,
                      strlen (cases[i].codepage) + 1))
        continue;
      check_mtext (&dwg, mtext, cases[i].utf8_expected, what, &lines);
      if (lines == ascii_lines)
        ok ("%s: %d lines, as ASCII", what, lines);
      else
        fail ("%s: %d lines, ASCII has %d", what, lines, ascii_lines);
    }
done:
  free_test_dwg (&dwg);
}

int
main (void)
{
  loglevel = test_loglevel = is_make_silent () ? 0 : 2;

  test_rotated_text_anchor ();
  test_long_write ();
  test_utf8_valid ();
  test_mtext_codepage ();
  test_mtext_utf16 ();
  test_mtext_utf16_cjk ();
  test_mtext_utf8 ();
  test_mtext_wrap ();
  return numfailed ();
}
