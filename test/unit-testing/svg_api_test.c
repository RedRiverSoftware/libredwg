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
     value) reaches the SVG whole. */

#define SVG_API_TEST_C
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// the library sets loglevel from the document's opts; ok and fail need ours
static unsigned int test_loglevel;

#include "config.h"
#include "dwg.h"
#include "tests_common.h"
#include "dwg_api.h"
#include "dwg_svg_api.h"

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

static Dwg_Entity_TEXT *
add_text (Dwg_Object_BLOCK_HEADER *bh, const char *value,
          const dwg_point_3d *ins_pt, double rotation_deg,
          double width_factor)
{
  Dwg_Entity_TEXT *text = dwg_add_TEXT (bh, value, ins_pt, 2.5);
  text->rotation = rotation_deg * M_PI / 180.0;
  text->width_factor = width_factor;
  return text;
}

#define NUM_CASES 4

/* TEXTs at one insertion point: a plain one, which the others must be
   anchored with, then the rotation and width factor cases. */
static void
add_texts (Dwg_Object_BLOCK_HEADER *bh, const dwg_point_3d *ins_pt,
           Dwg_Entity_TEXT **plain, Dwg_Entity_TEXT **cases)
{
  *plain = add_text (bh, "PLAIN", ins_pt, 0.0, 1.0);
  cases[0] = add_text (bh, "ROTATED", ins_pt, 30.0, 1.0);
  cases[1] = add_text (bh, "WIDE", ins_pt, 0.0, 0.8);
  cases[2] = add_text (bh, "ROTATED WIDE", ins_pt, 30.0, 0.8);
  cases[3] = add_text (bh, "ROTATED STRETCHED", ins_pt, 135.0, 1.5);
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
  Dwg_Data *dwg = dwg_new_Document (R_2000, 0, 0);
  Dwg_Object_BLOCK_HEADER *ms
      = dwg_model_space_object (dwg)->tio.object->tio.BLOCK_HEADER;
  Dwg_Object_BLOCK_HEADER *blk;
  Dwg_Entity_TEXT *top_plain, *top_cases[NUM_CASES];
  Dwg_Entity_TEXT *blk_plain, *blk_cases[NUM_CASES];
  dwg_point_3d p0 = { 0.0, 0.0, 0.0 }, p1 = { 400.0, 200.0, 0.0 };
  dwg_point_3d ins_pt = { 100.0, 50.0, 0.0 }, blk_pt = { 20.0, 10.0, 0.0 };
  dwg_point_3d insert_pt = { 300.0, 100.0, 0.0 };
  char *svg;
  size_t len;

  // the extents start at the origin, so the insertion point is far from
  // the SVG's origin and a wrong pivot moves the text visibly
  dwg_add_LINE (ms, &p0, &p1);
  add_texts (ms, &ins_pt, &top_plain, top_cases);

  blk = dwg_add_BLOCK_HEADER (dwg, "LABELS");
  dwg_add_BLOCK (blk, "LABELS");
  add_texts (blk, &blk_pt, &blk_plain, blk_cases);
  dwg_add_ENDBLK (blk);
  dwg_add_INSERT (ms, &insert_pt, "LABELS", 1.0, 1.0, 1.0, 0.0);

  svg = render_mspace (dwg, &len);
  if (svg)
    {
      check_anchors (svg, "top level", top_plain, top_cases);
      check_anchors (svg, "block", blk_plain, blk_cases);
      dwg_free_svg (svg);
    }
  dwg_free (dwg);
  free (dwg);
  loglevel = test_loglevel;
}

/* "A&" escapes to "A&amp;", so 2000 of them make the TEXT one write of about
   12 KB: more than dwg_svg_printf's 8 KB stack buffer, from a value that
   fits in it. */
#define LONG_REPEAT 2000

static void
test_long_write (void)
{
  Dwg_Data *dwg = dwg_new_Document (R_2000, 0, 0);
  Dwg_Object_BLOCK_HEADER *ms
      = dwg_model_space_object (dwg)->tio.object->tio.BLOCK_HEADER;
  dwg_point_3d ins_pt = { 10.0, 10.0, 0.0 };
  char *value = (char *)malloc (2 * LONG_REPEAT + 1);
  char *expected = (char *)malloc (6 * LONG_REPEAT + sizeof ("</text>"));
  char *svg;
  size_t len;

  if (!value || !expected)
    {
      fail ("out of memory");
      free (value);
      free (expected);
      dwg_free (dwg);
      free (dwg);
      return;
    }
  for (int i = 0; i < LONG_REPEAT; i++)
    {
      memcpy (value + 2 * i, "A&", 2);
      memcpy (expected + 6 * i, "A&amp;", 6);
    }
  value[2 * LONG_REPEAT] = '\0';
  strcpy (expected + 6 * LONG_REPEAT, "</text>");
  dwg_add_TEXT (ms, value, &ins_pt, 2.5);

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
  free (value);
  free (expected);
  dwg_free (dwg);
  free (dwg);
  loglevel = test_loglevel;
}

int
main (void)
{
  loglevel = test_loglevel = is_make_silent () ? 0 : 2;

  test_rotated_text_anchor ();
  test_long_write ();
  return numfailed ();
}
