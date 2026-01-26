/*****************************************************************************/
/*  LibreDWG - free implementation of the DWG file format                    */
/*                                                                           */
/*  Copyright (C) 2009-2025 Free Software Foundation, Inc.                   */
/*  Copyright (C) 2010 Thien-Thi Nguyen                                      */
/*                                                                           */
/*  This library is free software, licensed under the terms of the GNU       */
/*  General Public License as published by the Free Software Foundation,     */
/*  either version 3 of the License, or (at your option) any later version.  */
/*  You should have received a copy of the GNU General Public License        */
/*  along with this program.  If not, see <http://www.gnu.org/licenses/>.    */
/*****************************************************************************/

/*
 * dwg2SVG.c: convert a DWG to SVG
 * written by Felipe Corrêa da Silva Sances
 * modified by Rodrigo Rodrigues da Silva
 * modified by Thien-Thi Nguyen
 * modified by Reini Urban
 *
 * TODO: all entities: 3DSOLID, SHAPE, ARC_DIMENSION, ATTRIB, DIMENSION*,
 *         *SURFACE, GEOPOSITIONMARKER/CAMERA/LIGHT, HELIX,
 *         WIPEOUT/UNDERLAY, LEADER, MESH, MINSERT, MLINE, MTEXT,
 * MULTILEADER, OLE2FRAME, OLEFRAME, POLYLINE_3D, POLYLINE_MESH,
 * POLYLINE_PFACE, RAY, XLINE, SPLINE, TABLE, TOLERANCE, VIEWPORT?
 *       common_entity_data: ltype, ltype_scale.
 *       PLINE: widths, bulges.
 */

#define _GNU_SOURCE /* make musl expose strcasestr */
#include "../src/config.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_STRCASESTR
#  undef __DARWIN_C_LEVEL
#  define __DARWIN_C_LEVEL __DARWIN_C_FULL
#  ifndef __USE_GNU
#    define __USE_GNU
#  endif
#  ifndef __BSD_VISIBLE
#    define __BSD_VISIBLE 1
#  endif
#endif
#include <string.h>
#include <ctype.h>
#ifdef HAVE_UNISTD_H
#  include <unistd.h>
#endif
#include "my_getopt.h"
#include <math.h>
#ifdef HAVE_VALGRIND_VALGRIND_H
#  include <valgrind/valgrind.h>
#endif

#include <dwg.h>
#include <dwg_api.h>
#include "bits.h"
#include "common.h"
#include "escape.h"
#include "geom.h"
#include "suffix.inc"
#include "my_getopt.h"

static int opts = 0;
static int mspace = 0; // only mspace, even when pspace is defined
static int in_block_definition = 0; // 1 when outputting block symbol entities

// Case-insensitive prefix match
static int
strncasecmp_prefix (const char *str, const char *prefix)
{
  while (*prefix)
    {
      if (tolower ((unsigned char)*str) != tolower ((unsigned char)*prefix))
        return 1;
      str++;
      prefix++;
    }
  return 0;
}

// Case-insensitive substring search (portable strcasestr)
static char *
strcasestr_compat (const char *haystack, const char *needle)
{
#ifdef HAVE_STRCASESTR
  return strcasestr (haystack, needle);
#else
  size_t needle_len;
  const char *h;
  const char *n;
  
  if (!haystack || !needle)
    return NULL;
  needle_len = strlen (needle);
  if (needle_len == 0)
    return (char *)haystack;
  for (; *haystack; haystack++)
    {
      if (tolower ((unsigned char)*haystack) == tolower ((unsigned char)*needle))
        {
          h = haystack;
          n = needle;
          while (*h && *n && tolower ((unsigned char)*h) == tolower ((unsigned char)*n))
            {
              h++;
              n++;
            }
          if (*n == '\0')
            return (char *)haystack;
        }
    }
  return NULL;
#endif
}
static double block_base_x = 0.0, block_base_y = 0.0; // current block's base_pt
Dwg_Data g_dwg;
double model_xmin, model_ymin, model_xmax, model_ymax;
double page_width, page_height, scale;

// Extents calculation structure
typedef struct _Extents
{
  double xmin, ymin, xmax, ymax;
  int initialized;
} Extents;

static void
extents_init (Extents *ext)
{
  ext->xmin = INFINITY;
  ext->ymin = INFINITY;
  ext->xmax = -INFINITY;
  ext->ymax = -INFINITY;
  ext->initialized = 0;
}

static void
extents_add_point (Extents *ext, double x, double y)
{
  if (isnan (x) || isnan (y))
    return;
  if (x < ext->xmin)
    ext->xmin = x;
  if (x > ext->xmax)
    ext->xmax = x;
  if (y < ext->ymin)
    ext->ymin = y;
  if (y > ext->ymax)
    ext->ymax = y;
  ext->initialized = 1;
}

static void
extents_add_circle (Extents *ext, double cx, double cy, double radius)
{
  if (isnan (cx) || isnan (cy) || isnan (radius))
    return;
  extents_add_point (ext, cx - radius, cy - radius);
  extents_add_point (ext, cx + radius, cy + radius);
}

// Forward declarations
static void output_SVG (Dwg_Data *dwg);
static void compute_entity_extents (Extents *ext, Dwg_Object *obj);
static void compute_block_extents (Extents *ext, Dwg_Object_Ref *ref);

#ifndef DWG2SVG_NO_MAIN
static int
usage (void)
{
  printf ("\nUsage: dwg2SVG [-v[0-9]] DWGFILE >SVGFILE\n");
  return 1;
}
static int
opt_version (void)
{
  printf ("dwg2SVG %s\n", PACKAGE_VERSION);
  return 0;
}
static int
help (void)
{
  printf ("\nUsage: dwg2SVG [OPTION]... DWGFILE >SVGFILE\n");
  printf ("Converts some 2D elements of the DWG to a SVG.\n"
          "\n");
#ifdef HAVE_GETOPT_LONG
  printf ("  -v[0-9], --verbose [0-9]  verbosity\n");
  printf ("           --mspace         only model-space, no paper-space\n");
  printf ("           --force-free     force free\n");
  printf ("           --help           display this help and exit\n");
  printf ("           --version        output version information and exit\n"
          "\n");
#else
  printf ("  -v[0-9]     verbosity\n");
  printf ("  -m          only model-space, no paper-space\n");
  printf ("  -h          display this help and exit\n");
  printf ("  -i          output version information and exit\n"
          "\n");
#endif
  printf ("GNU LibreDWG online manual: "
          "<https://www.gnu.org/software/libredwg/>\n");
  return 0;
}
#endif /* !DWG2SVG_NO_MAIN */

static double
transform_ANGLE (double angle)
{
  return 180 - angle;
}

static double
transform_X (double x)
{
  if (in_block_definition)
    return x; // raw DWG coords, INSERT handles positioning
  return x - model_xmin;
}

static double
transform_Y (double y)
{
  if (in_block_definition)
    return y; // raw DWG coords, INSERT handles positioning and Y flip
  return page_height - (y - model_ymin);
}

static bool
isnan_2BD (BITCODE_2BD pt)
{
  return isnan (pt.x) || isnan (pt.y);
}

static bool
isnan_2pt (dwg_point_2d pt)
{
  return isnan (pt.x) || isnan (pt.y);
}

static bool
isnan_3BD (BITCODE_3BD pt)
{
  return isnan (pt.x) || isnan (pt.y) || isnan (pt.z);
}

static bool
entity_invisible (Dwg_Object *obj)
{
  BITCODE_BS invisible = obj->tio.entity->invisible;
  Dwg_Object *layer;
  Dwg_Object_LAYER *_obj;
  if (invisible)
    return true;

  if (!obj->tio.entity->layer || !obj->tio.entity->layer->obj)
    return false;
  layer = obj->tio.entity->layer->obj;
  if (layer->fixedtype != DWG_TYPE_LAYER)
    return false;
  _obj = layer->tio.object->tio.LAYER;
  // layer off or frozen
  if (_obj->off || _obj->frozen)
    return true;
  return false;
}

static double
entity_lweight (Dwg_Object_Entity *ent)
{
  // stroke-width in px. lweights are in 100th of a mm
  int lw = dxf_cvt_lweight (ent->linewt);

  // BYLAYER (-1): look up layer's lineweight
  if (lw == -1 && ent->layer && ent->layer->obj
      && ent->layer->obj->fixedtype == DWG_TYPE_LAYER)
    {
      Dwg_Object_LAYER *layer = ent->layer->obj->tio.object->tio.LAYER;
      lw = dxf_cvt_lweight (layer->linewt);
    }

  // Default/ByBlock/negative: use minimum visible width
  if (lw <= 0)
    return 0.1;

  lw = (double)(lw * 0.001);
  if (lw <= 0.1)
    return 0.1;

  return lw;
}

static char *
aci_color (unsigned int index)
{
  if (index >= 8 && index < 256)
    {
      const Dwg_RGB_Palette *palette = dwg_rgb_palette ();
      const Dwg_RGB_Palette *rgb = &palette[index];
      char *s = (char *)malloc (8);
      sprintf (s, "#%02x%02x%02x", rgb->r, rgb->g, rgb->b);
      return s;
    }
  else
    switch (index)
      {
      case 1:
        return (char *)"red";
      case 2:
        return (char *)"yellow";
      case 3:
        return (char *)"green";
      case 4:
        return (char *)"cyan";
      case 5:
        return (char *)"blue";
      case 6:
        return (char *)"magenta";
      case 7:
        return (char *)"white";
      case 0:   // ByBlock
      default:
        return (char *)"black";
      }
}

static char *
cmc_color (BITCODE_CMC *color)
{
  if (color->index >= 1 && color->index < 256)
    {
      return aci_color (color->index);
    }
  else if (color->flag & 0x80 && !(color->flag & 0x40))
    {
      char *s = (char *)malloc (8);
      sprintf (s, "#%06x", color->rgb & 0x00ffffff);
      return s;
    }
  else if (color->index == 256 && (color->rgb >> 24) == 0xc3)
    {
      // ACI stored in low byte of rgb (layer color encoding)
      return aci_color (color->rgb & 0xff);
    }
  return (char *)"black";
}

static char *
entity_color (Dwg_Object *obj)
{
  Dwg_Object_Entity *ent = obj->tio.entity;

  if (ent->color.index == 256) // ByLayer
    {
      if (ent->layer && ent->layer->obj
          && ent->layer->obj->fixedtype == DWG_TYPE_LAYER)
        {
          Dwg_Object_LAYER *layer = ent->layer->obj->tio.object->tio.LAYER;
          return cmc_color (&layer->color);
        }
    }
  return cmc_color (&ent->color);
}

static void
common_entity (Dwg_Object *obj)
{
  double lweight;
  char *color;
  lweight = entity_lweight (obj->tio.entity);
  color = entity_color (obj);
  printf ("      style=\"fill:none;stroke:%s;stroke-width:%.2fpx\" />\n",
          color, lweight);
  if (*color == '#')
    free (color);
}

// Get font family and cap height ratio from a STYLE object
static void
get_font_info (Dwg_Object_STYLE *style, Dwg_Object *o,
               const char **fontfamily, double *cap_height_ratio)
{
  if (style && o && o->fixedtype == DWG_TYPE_STYLE && style->font_file
      && *style->font_file && strcasestr_compat (style->font_file, ".ttf"))
    {
      if (strcasestr_compat (style->font_file, "arial"))
        {
          *fontfamily = "Arial";
          *cap_height_ratio = 0.716;
        }
      else if (strcasestr_compat (style->font_file, "times"))
        {
          *fontfamily = "Times New Roman";
          *cap_height_ratio = 0.662;
        }
      // Swiss 721 Black Extended (swissek.ttf)
      else if (strcasestr_compat (style->font_file, "swissek"))
        {
          *fontfamily = "Swis721 BlkEx BT, Helvetica, Arial";
          *cap_height_ratio = 0.716;
        }
      // Swiss 721 (swiss.ttf)
      else if (strcasestr_compat (style->font_file, "swiss"))
        {
          *fontfamily = "Swis721 BT, Helvetica, Arial";
          *cap_height_ratio = 0.716;
        }
      else if (strcasestr_compat (style->font_file, "lucon"))
        {
          *fontfamily = "Lucida Console";
          *cap_height_ratio = 0.692;
        }
      else
        {
          *fontfamily = "Verdana";
          *cap_height_ratio = 0.727;
        }
    }
  else
    {
      // SHX or missing font - use monospace
      *fontfamily = "Courier";
      *cap_height_ratio = 0.616;
    }
}

// Get SVG text-anchor from horizontal alignment
static const char *
get_text_anchor (BITCODE_BS horiz_alignment)
{
  switch (horiz_alignment)
    {
    case 1: // Center
    case 4: // Middle (fit)
      return "middle";
    case 2: // Right
      return "end";
    default: // Left (0), Aligned (3), Fit (5)
      return "start";
    }
}

// Get SVG dominant-baseline from vertical alignment
static const char *
get_dominant_baseline (BITCODE_BS vert_alignment)
{
  switch (vert_alignment)
    {
    case 1: // Bottom
      return "text-after-edge";
    case 2: // Middle
      return "central";
    case 3: // Top
      return "text-before-edge";
    default: // Baseline (0)
      return "auto";
    }
}

// Output a <text> SVG element with optional rotation and width scaling
static void
output_text_element (Dwg_Object *obj, double x, double y,
                     const char *fontfamily, double font_size,
                     const char *color, const char *text_anchor,
                     const char *dominant_baseline, double rotation_deg,
                     double width_factor, const char *escaped)
{
  int has_rotation = fabs (rotation_deg) > 0.001;
  int has_scale = fabs (width_factor - 1.0) > 0.001;
  double tx = has_scale ? x / width_factor : x;

  printf ("\t<text id=\"dwg-object-%d\" x=\"%f\" y=\"%f\" "
          "font-family=\"%s\" font-size=\"%f\" fill=\"%s\" "
          "text-anchor=\"%s\" dominant-baseline=\"%s\"",
          obj->index, tx, y, fontfamily, font_size, color,
          text_anchor, dominant_baseline);

  if (has_rotation && has_scale)
    printf (" transform=\"rotate(%f %f %f) scale(%f 1)\"",
            -rotation_deg, tx, y, width_factor);
  else if (has_rotation)
    printf (" transform=\"rotate(%f %f %f)\"", -rotation_deg, tx, y);
  else if (has_scale)
    printf (" transform=\"scale(%f 1)\"", width_factor);

  printf (">%s</text>\n", escaped ? escaped : "");
}

// TODO: MTEXT
static void
output_TEXT (Dwg_Object *obj)
{
  Dwg_Data *dwg = obj->parent;
  Dwg_Entity_TEXT *text = obj->tio.entity->tio.TEXT;
  char *escaped;
  const char *fontfamily;
  double cap_height_ratio;
  BITCODE_H style_ref = text->style;
  Dwg_Object *o = style_ref ? dwg_ref_object_silent (dwg, style_ref) : NULL;
  Dwg_Object_STYLE *style = o ? o->tio.object->tio.STYLE : NULL;
  BITCODE_2DPOINT pt;
  double wf;

  if (!text->text_value || entity_invisible (obj))
    return;
  if (isnan_2BD (text->ins_pt) || isnan_3BD (text->extrusion))
    return;
  if (dwg->header.version >= R_2007)
    escaped = htmlwescape ((BITCODE_TU)text->text_value);
  else
    escaped = htmlescape (text->text_value, dwg->header.codepage);

  get_font_info (style, o, &fontfamily, &cap_height_ratio);

  if (text->horiz_alignment != 0 || text->vert_alignment != 0)
    transform_OCS_2d (&pt, text->alignment_pt, text->extrusion);
  else
    transform_OCS_2d (&pt, text->ins_pt, text->extrusion);

  wf = text->width_factor;
  if (wf == 0.0 && style)
    wf = style->width_factor;
  if (wf == 0.0)
    wf = 1.0;

  output_text_element (obj, transform_X (pt.x), transform_Y (pt.y),
                       fontfamily, text->height / cap_height_ratio,
                       entity_color (obj),
                       get_text_anchor (text->horiz_alignment),
                       get_dominant_baseline (text->vert_alignment),
                       0.0, wf, escaped);

  if (escaped)
    free (escaped);
}

static void
output_ATTDEF (Dwg_Object *obj)
{
  Dwg_Data *dwg = obj->parent;
  Dwg_Entity_ATTDEF *attdef = obj->tio.entity->tio.ATTDEF;
  char *escaped;
  const char *fontfamily;
  double cap_height_ratio;
  BITCODE_H style_ref = attdef->style;
  Dwg_Object *o = style_ref ? dwg_ref_object_silent (dwg, style_ref) : NULL;
  Dwg_Object_STYLE *style = o ? o->tio.object->tio.STYLE : NULL;
  BITCODE_2DPOINT pt;
  double rotation_deg, wf;

  if (!attdef->tag || entity_invisible (obj))
    return;
  if (isnan_2BD (attdef->ins_pt) || isnan_3BD (attdef->extrusion))
    return;
  if (dwg->header.version >= R_2007)
    escaped = htmlwescape ((BITCODE_TU)attdef->tag);
  else
    escaped = htmlescape (attdef->tag, dwg->header.codepage);

  get_font_info (style, o, &fontfamily, &cap_height_ratio);

  if (attdef->horiz_alignment != 0 || attdef->vert_alignment != 0)
    transform_OCS_2d (&pt, attdef->alignment_pt, attdef->extrusion);
  else
    transform_OCS_2d (&pt, attdef->ins_pt, attdef->extrusion);
  rotation_deg = attdef->rotation * 180.0 / M_PI;

  wf = attdef->width_factor;
  if (wf == 0.0 && style)
    wf = style->width_factor;
  if (wf == 0.0)
    wf = 1.0;

  output_text_element (obj, transform_X (pt.x), transform_Y (pt.y),
                       fontfamily, attdef->height / cap_height_ratio,
                       entity_color (obj),
                       get_text_anchor (attdef->horiz_alignment),
                       get_dominant_baseline (attdef->vert_alignment),
                       rotation_deg, wf, escaped);

  if (escaped)
    free (escaped);
}

static void
output_LINE (Dwg_Object *obj)
{
  Dwg_Entity_LINE *line = obj->tio.entity->tio.LINE;
  BITCODE_3DPOINT start, end;

  if (isnan_3BD (line->start) || isnan_3BD (line->end)
      || isnan_3BD (line->extrusion) || entity_invisible (obj))
    return;
  transform_OCS (&start, line->start, line->extrusion);
  transform_OCS (&end, line->end, line->extrusion);
  printf ("\t<!-- line-%d -->\n", obj->index);
  printf ("\t<path id=\"dwg-object-%d\" d=\"M %f,%f L %f,%f\"\n\t", obj->index,
          transform_X (start.x), transform_Y (start.y), transform_X (end.x),
          transform_Y (end.y));
  common_entity (obj);
}

static void
output_XLINE (Dwg_Object *obj)
{
  Dwg_Entity_XLINE *xline = obj->tio.entity->tio.XLINE;
  BITCODE_3DPOINT invvec;
  static BITCODE_3DPOINT box[2];
  int sign[3];
  double txmin, txmax, tymin, tymax, tzmin, tzmax;

  if (isnan_3BD (xline->point) || isnan_3BD (xline->vector)
      || entity_invisible (obj))
    return;

  invvec.x = 1.0 / xline->vector.x;
  invvec.y = 1.0 / xline->vector.y;
  invvec.z = 1.0 / xline->vector.z;
  sign[0] = (invvec.x < 0.0);
  sign[1] = (invvec.y < 0.0);
  sign[2] = (invvec.z < 0.0);
  box[0].x = model_xmin;
  box[0].y = model_ymin;
  box[1].x = model_xmax;
  box[1].y = model_ymin;
  printf ("\t<!-- xline-%d -->\n", obj->index);

  // untested!
  /* intersect xline with model_xmin, model_ymin, model_xmax, model_ymax */
  txmin = (box[sign[0]].x - xline->point.x) * invvec.x;
  txmax = (box[1 - sign[0]].x - xline->point.x) * invvec.x;
  tymin = (box[sign[1]].x - xline->point.y) * invvec.y;
  tymax = (box[1 - sign[1]].x - xline->point.y) * invvec.y;
  if ((txmin > tymax) || (tymin > txmax))
    return;
  if (tymin > txmin)
    txmin = tymin;
  if (tymax > txmax)
    txmax = tymax;
  tzmin = (box[sign[0]].z - xline->point.z) * invvec.z;
  tzmax = (box[1 - sign[0]].z - xline->point.z) * invvec.z;
  if ((txmin > tzmax) || (tzmin > txmax))
    return;

  printf ("\t<path id=\"dwg-object-%d\" d=\"M %f,%f L %f,%f\"\n\t", obj->index,
          txmin, tymin, txmax, tymax);
  common_entity (obj);
}

static void
output_RAY (Dwg_Object *obj)
{
  Dwg_Entity_XLINE *xline = obj->tio.entity->tio.RAY;
  BITCODE_3DPOINT point, invvec;
  static BITCODE_3DPOINT box[2];
  int sign[3];
  double txmin, txmax, tymin, tymax, tzmin, tzmax;

  if (isnan_3BD (xline->point) || isnan_3BD (xline->vector)
      || entity_invisible (obj))
    return;

  invvec.x = 1.0 / xline->vector.x;
  invvec.y = 1.0 / xline->vector.y;
  invvec.z = 1.0 / xline->vector.z;
  sign[0] = (invvec.x < 0.0);
  sign[1] = (invvec.y < 0.0);
  sign[2] = (invvec.z < 0.0);
  box[0].x = model_xmin;
  box[0].y = model_ymin;
  box[1].x = model_xmax;
  box[1].y = model_ymin;
  printf ("\t<!-- ray-%d -->\n", obj->index);

  // untested!
  /* intersect ray from point with box (model_xmin, model_ymin, model_xmax,
   * model_ymax) */
  txmin = (box[sign[0]].x - xline->point.x) * invvec.x;
  txmax = (box[1 - sign[0]].x - xline->point.x) * invvec.x;
  tymin = (box[sign[1]].x - xline->point.y) * invvec.y;
  tymax = (box[1 - sign[1]].x - xline->point.y) * invvec.y;
  if ((txmin > tymax) || (tymin > txmax))
    return;
  if (tymin > txmin)
    txmin = tymin;
  if (tymax > txmax)
    txmax = tymax;
  point.x = (xline->point.x > txmax) ? txmax : xline->point.x;
  if (point.x < txmin)
    point.x = txmin;
  point.y = (xline->point.y > tymax) ? tymax : xline->point.y;
  if (point.y < tymin)
    point.y = tymin;

  tzmin = (box[sign[0]].z - xline->point.z) * invvec.z;
  tzmax = (box[1 - sign[0]].z - xline->point.z) * invvec.z;
  if ((txmin > tzmax) || (tzmin > txmax))
    return;

  printf ("\t<path id=\"dwg-object-%d\" d=\"M %f,%f L %f,%f\"\n\t", obj->index,
          point.x, point.y, txmax, tymax);
  common_entity (obj);
}

static void
output_CIRCLE (Dwg_Object *obj)
{
  Dwg_Entity_CIRCLE *circle = obj->tio.entity->tio.CIRCLE;
  BITCODE_3DPOINT center;

  if (isnan_3BD (circle->center) || isnan_3BD (circle->extrusion)
      || isnan (circle->radius) || entity_invisible (obj))
    return;
  transform_OCS (&center, circle->center, circle->extrusion);
  printf ("\t<!-- circle-%d -->\n", obj->index);
  printf ("\t<circle id=\"dwg-object-%d\" cx=\"%f\" cy=\"%f\" r=\"%f\"\n\t",
          obj->index, transform_X (center.x), transform_Y (center.y),
          circle->radius);
  common_entity (obj);
}

// CIRCLE with radius 0.1
static void
output_POINT (Dwg_Object *obj)
{
  Dwg_Entity_POINT *point = obj->tio.entity->tio.POINT;
  BITCODE_3DPOINT pt, pt1;

  pt.x = point->x;
  pt.y = point->y;
  pt.z = point->z;
  if (isnan_3BD (pt) || isnan_3BD (point->extrusion) || entity_invisible (obj))
    return;
  transform_OCS (&pt1, pt, point->extrusion);
  printf ("\t<!-- point-%d -->\n", obj->index);
  printf ("\t<circle id=\"dwg-object-%d\" cx=\"%f\" cy=\"%f\" r=\"0.1\"\n\t",
          obj->index, transform_X (pt1.x), transform_Y (pt1.y));
  common_entity (obj);
}

static void
output_ARC (Dwg_Object *obj)
{
  Dwg_Entity_ARC *arc = obj->tio.entity->tio.ARC;
  BITCODE_3DPOINT center;
  double x_start, y_start, x_end, y_end;
  int large_arc;

  if (isnan_3BD (arc->center) || isnan_3BD (arc->extrusion)
      || isnan (arc->radius) || isnan (arc->start_angle)
      || isnan (arc->end_angle) || entity_invisible (obj))
    return;
  transform_OCS (&center, arc->center, arc->extrusion);

  x_start = center.x + arc->radius * cos (arc->start_angle);
  y_start = center.y + arc->radius * sin (arc->start_angle);
  x_end = center.x + arc->radius * cos (arc->end_angle);
  y_end = center.y + arc->radius * sin (arc->end_angle);
  // Assuming clockwise arcs.
  large_arc = (arc->end_angle - arc->start_angle < M_PI) ? 0 : 1;

  printf ("\t<!-- arc-%d -->\n", obj->index);
  printf (
      "\t<path id=\"dwg-object-%d\" d=\"M %f,%f A %f,%f 0 %d,0 %f,%f\"\n\t",
      obj->index, transform_X (x_start), transform_Y (y_start), arc->radius,
      arc->radius, large_arc, transform_X (x_end), transform_Y (y_end));
  common_entity (obj);
}

// FIXME
static void
output_ELLIPSE (Dwg_Object *obj)
{
  Dwg_Entity_ELLIPSE *ell = obj->tio.entity->tio.ELLIPSE;
  BITCODE_2DPOINT radius;
  double angle_rad, angle_dec;
  // BITCODE_3DPOINT center, sm_axis;
  // double x_start, y_start, x_end, y_end;

  if (isnan_3BD (ell->center) || isnan_3BD (ell->extrusion)
      || isnan_3BD (ell->sm_axis) || isnan (ell->axis_ratio)
      || isnan (ell->start_angle) || isnan (ell->end_angle)
      || entity_invisible (obj))
    return;
  /* The 2 points are already WCS */
  // transform_OCS (&center, ell->center, ell->extrusion);
  // transform_OCS (&sm_axis, ell->sm_axis, ell->extrusion);
  radius.x = sqrt (ell->sm_axis.x * ell->sm_axis.x + ell->sm_axis.y * ell->sm_axis.y);
  radius.y = radius.x * ell->axis_ratio;

  /*
  x_start = ell->center.x + radius.x * cos (ell->start_angle);
  y_start = ell->center.y + radius.y * sin (ell->start_angle);
  x_end = ell->center.x + radius.x * cos (ell->end_angle);
  y_end = ell->center.y + radius.y * sin (ell->end_angle);
  */

  angle_rad = atan2(ell->sm_axis.y, ell->sm_axis.x);
  angle_dec = angle_rad * 180.0 / M_PI;

  // TODO: start,end_angle => pathLength
  printf ("\t<!-- ellipse-%d -->\n", obj->index);
  printf ("\t<!-- sm_axis=(%f,%f,%f) axis_ratio=%f start_angle=%f "
          "end_angle=%f-->\n",
          ell->sm_axis.x, ell->sm_axis.y, ell->sm_axis.z, ell->axis_ratio,
          ell->start_angle, ell->end_angle);
  printf ("\t<ellipse id=\"dwg-object-%d\" cx=\"%f\" cy=\"%f\" rx=\"%f\" "
          "ry=\"%f\" transform=\"rotate(%f %f %f)\"\n\t",
          obj->index, transform_X (ell->center.x), transform_Y (ell->center.y),
          radius.x, radius.y,
          transform_ANGLE (angle_dec), transform_X (ell->center.x), transform_Y (ell->center.y));
  common_entity (obj);
}

// untested
static void
output_SOLID (Dwg_Object *obj)
{
  Dwg_Entity_SOLID *sol = obj->tio.entity->tio.SOLID;
  BITCODE_2DPOINT c1, c2, c3, c4;
  BITCODE_2DPOINT s1, s2, s3, s4;

  memcpy (&s1, &sol->corner1, sizeof s1);
  memcpy (&s2, &sol->corner2, sizeof s1);
  memcpy (&s3, &sol->corner3, sizeof s1);
  memcpy (&s4, &sol->corner4, sizeof s1);
  if (isnan_2BD (s1) || isnan_2BD (s2) || isnan_2BD (s3) || isnan_2BD (s4)
      || entity_invisible (obj))
    return;
  transform_OCS_2d (&c1, s1, sol->extrusion);
  transform_OCS_2d (&c2, s2, sol->extrusion);
  transform_OCS_2d (&c3, s3, sol->extrusion);
  transform_OCS_2d (&c4, s4, sol->extrusion);

  printf ("\t<!-- solid-%d -->\n", obj->index);
  printf ("\t<polygon id=\"dwg-object-%d\" "
          "points=\"%f,%f %f,%f %f,%f %f,%f\"\n\t",
          obj->index, transform_X (c1.x), transform_Y (c1.y),
          transform_X (c2.x), transform_Y (c2.y), transform_X (c3.x),
          transform_Y (c3.y), transform_X (c4.x), transform_Y (c4.y));
  common_entity (obj);
}

// untested
static void
output_3DFACE (Dwg_Object *obj)
{
  Dwg_Entity__3DFACE *ent = obj->tio.entity->tio._3DFACE;

  if (isnan_3BD (ent->corner1) || isnan_3BD (ent->corner2)
      || isnan_3BD (ent->corner3) || isnan_3BD (ent->corner4)
      || entity_invisible (obj))
    return;
  printf ("\t<!-- 3dface-%d -->\n", obj->index);
  if (ent->invis_flags)
    {
      // move to 1
      printf ("\t<path id=\"dwg-object-%d\" d=\"M %f,%f", obj->index,
              ent->corner1.x, ent->corner1.y);
      printf (" %s %f,%f", ent->invis_flags & 1 ? "M" : "L", ent->corner2.x,
              ent->corner2.y);
      printf (" %s %f,%f", ent->invis_flags & 2 ? "M" : "L", ent->corner3.x,
              ent->corner3.y);
      printf (" %s %f,%f", ent->invis_flags & 4 ? "M" : "L", ent->corner4.x,
              ent->corner4.y);
      printf (" %s %f,%f\"\n\t", ent->invis_flags & 8 ? "M" : "L",
              ent->corner1.x, ent->corner1.y);
    }
  else
    printf ("\t<polygon id=\"dwg-object-%d\" "
            "points=\"%f,%f %f,%f %f,%f %f,%f\"\n\t",
            obj->index, ent->corner1.x, ent->corner1.y, ent->corner2.x,
            ent->corner2.y, ent->corner3.x, ent->corner3.y, ent->corner4.x,
            ent->corner4.y);
  common_entity (obj);
}

static void
output_POLYLINE_2D (Dwg_Object *obj)
{
  Dwg_Data *dwg = obj->parent;
  Dwg_Entity_POLYLINE_2D *pline = obj->tio.entity->tio.POLYLINE_2D;
  BITCODE_BL i, num_owned;
  bool first = true;

  if (entity_invisible (obj))
    return;
  if (isnan_3BD (pline->extrusion))
    return;

  // N.B. we can't use dwg_object_polyline_2d_get_[num]points, because it returns all
  // points without flags, so we can't filter out spline frame control points 
  num_owned = pline->num_owned;
  if (!num_owned)
    return;

  printf ("\t<!-- polyline_2d-%d -->\n", obj->index);
  printf ("\t<path id=\"dwg-object-%d\" d=\"", obj->index);

  for (i = 0; i < num_owned; i++)
    {
      Dwg_Object *vobj = dwg_ref_object (dwg, pline->vertex[i]);
      Dwg_Entity_VERTEX_2D *vertex;
      BITCODE_2DPOINT pt, ptin;

      if (!vobj || vobj->fixedtype != DWG_TYPE_VERTEX_2D)
        continue;
      vertex = vobj->tio.entity->tio.VERTEX_2D;
      if (!vertex)
        continue;
      // Skip spline frame control points (flag 16)
      if (vertex->flag & 16)
        continue;

      ptin.x = vertex->point.x;
      ptin.y = vertex->point.y;
      if (isnan_2BD (ptin))
        continue;
      transform_OCS_2d (&pt, ptin, pline->extrusion);

      if (first)
        {
          printf ("M %f,%f", transform_X (pt.x), transform_Y (pt.y));
          first = false;
        }
      else
        {
          printf (" L %f,%f", transform_X (pt.x), transform_Y (pt.y));
        }
    }

  if (pline->flag & 1) // closed
    printf (" Z");
  printf ("\"\n\t");
  common_entity (obj);
}

static void
output_LWPOLYLINE (Dwg_Object *obj)
{
  int error;
  Dwg_Entity_LWPOLYLINE *pline = obj->tio.entity->tio.LWPOLYLINE;
  BITCODE_RL numpts;

  if (entity_invisible (obj))
    return;
  numpts = dwg_ent_lwpline_get_numpoints (pline, &error);
  if (numpts && !error)
    {
      BITCODE_2DPOINT pt, ptin;
      dwg_point_2d *pts = dwg_ent_lwpline_get_points (pline, &error);
      BITCODE_RL j;

      if (error || isnan_2pt (pts[0]) || isnan_3BD (pline->extrusion))
        return;
      ptin.x = pts[0].x;
      ptin.y = pts[0].y;
      transform_OCS_2d (&pt, ptin, pline->extrusion);
      printf ("\t<!-- lwpolyline-%d -->\n", obj->index);
      printf ("\t<path id=\"dwg-object-%d\" d=\"M %f,%f", obj->index,
              transform_X (pt.x), transform_Y (pt.y));
      // TODO curve_types, C for Bezier https://svgwg.org/specs/paths/#PathData
      for (j = 1; j < numpts; j++)
        {
          ptin.x = pts[j].x;
          ptin.y = pts[j].y;
          if (isnan_2BD (ptin))
            continue;
          transform_OCS_2d (&pt, ptin, pline->extrusion);
          // TODO bulge -> arc, widths
          printf (" L %f,%f", transform_X (pt.x), transform_Y (pt.y));
        }
      if (pline->flag & 512) // closed
        printf (" Z");
      printf ("\"\n\t");
      common_entity (obj);
      free (pts);
    }
}

// Output an SVG arc command for a polyline segment with bulge
// bulge = tan(arc_angle/4), where arc_angle is the included angle
// Positive bulge = CCW arc in DWG (Y-up), negative bulge = CW arc
// Since SVG Y is inverted (Y-down), we flip the sweep direction
static void
output_bulge_arc (double x1, double y1, double x2, double y2, double bulge)
{
  double dx = x2 - x1;
  double dy = y2 - y1;
  double chord = sqrt (dx * dx + dy * dy);
  double sagitta = fabs (bulge) * chord / 2.0;
  double radius = (chord * chord / 4.0 + sagitta * sagitta) / (2.0 * sagitta);
  int large_arc = fabs (bulge) > 1.0 ? 1 : 0;
  // Positive bulge = CCW in DWG, but with Y-flip becomes CW in SVG (sweep=1)
  int sweep = bulge > 0 ? 1 : 0;
  printf (" A %f,%f 0 %d,%d %f,%f", radius, radius, large_arc, sweep,
          transform_X (x2), transform_Y (y2));
}

// Output SVG path data for a single hatch path (polyline or segments)
static void
output_hatch_path_data (Dwg_HATCH_Path *path)
{
  BITCODE_BL j;
  int is_polyline = path->flag & 2;

  if (is_polyline && path->polyline_paths)
    {
      for (j = 0; j < path->num_segs_or_paths; j++)
        {
          Dwg_HATCH_PolylinePath *pp = &path->polyline_paths[j];
          double x = pp->point.x;
          double y = pp->point.y;
          if (isnan (x) || isnan (y))
            continue;
          if (j == 0)
            printf ("M %f,%f", transform_X (x), transform_Y (y));
          else
            {
              Dwg_HATCH_PolylinePath *prev = &path->polyline_paths[j - 1];
              if (path->bulges_present && fabs (prev->bulge) > 1e-6)
                output_bulge_arc (prev->point.x, prev->point.y, x, y, prev->bulge);
              else
                printf (" L %f,%f", transform_X (x), transform_Y (y));
            }
        }
      if (path->closed && path->num_segs_or_paths > 0)
        {
          Dwg_HATCH_PolylinePath *last = &path->polyline_paths[path->num_segs_or_paths - 1];
          Dwg_HATCH_PolylinePath *first = &path->polyline_paths[0];
          if (path->bulges_present && fabs (last->bulge) > 1e-6)
            output_bulge_arc (last->point.x, last->point.y,
                              first->point.x, first->point.y, last->bulge);
          else
            printf (" Z");
        }
    }
  else if (path->segs)
    {
      int first_point = 1;
      for (j = 0; j < path->num_segs_or_paths; j++)
        {
          Dwg_HATCH_PathSeg *seg = &path->segs[j];
          switch (seg->curve_type)
            {
            case 1: // LINE
              {
                double x1 = seg->first_endpoint.x;
                double y1 = seg->first_endpoint.y;
                double x2 = seg->second_endpoint.x;
                double y2 = seg->second_endpoint.y;
                if (isnan (x1) || isnan (y1) || isnan (x2) || isnan (y2))
                  continue;
                if (first_point)
                  {
                    printf ("M %f,%f", transform_X (x1), transform_Y (y1));
                    first_point = 0;
                  }
                printf (" L %f,%f", transform_X (x2), transform_Y (y2));
              }
              break;
            case 2: // CIRCULAR ARC
              {
                double cx = seg->center.x;
                double cy = seg->center.y;
                double r = seg->radius;
                double sa = seg->start_angle;
                double ea = seg->end_angle;
                double x1, y1, x2, y2;
                int large_arc, sweep;
                if (isnan (cx) || isnan (cy) || isnan (r) || isnan (sa)
                    || isnan (ea))
                  continue;
                x1 = cx + r * cos (sa);
                y1 = cy + r * sin (sa);
                x2 = cx + r * cos (ea);
                y2 = cy + r * sin (ea);
                large_arc = fabs (ea - sa) > M_PI ? 1 : 0;
                sweep = seg->is_ccw ? 1 : 0;
                if (first_point)
                  {
                    printf ("M %f,%f", transform_X (x1), transform_Y (y1));
                    first_point = 0;
                  }
                printf (" A %f,%f 0 %d,%d %f,%f", r, r, large_arc, sweep,
                        transform_X (x2), transform_Y (y2));
              }
              break;
            case 3: // ELLIPTICAL ARC
              {
                double cx = seg->center.x;
                double cy = seg->center.y;
                double rx = sqrt (seg->endpoint.x * seg->endpoint.x
                                  + seg->endpoint.y * seg->endpoint.y);
                double ry = rx * seg->minor_major_ratio;
                double rot = atan2 (seg->endpoint.y, seg->endpoint.x)
                             * 180.0 / M_PI;
                double sa = seg->start_angle;
                double ea = seg->end_angle;
                double x1, y1, x2, y2;
                int large_arc, sweep;
                if (isnan (cx) || isnan (cy) || isnan (rx) || isnan (ry)
                    || isnan (sa) || isnan (ea))
                  continue;
                x1 = cx + rx * cos (sa);
                y1 = cy + ry * sin (sa);
                x2 = cx + rx * cos (ea);
                y2 = cy + ry * sin (ea);
                large_arc = fabs (ea - sa) > M_PI ? 1 : 0;
                sweep = seg->is_ccw ? 1 : 0;
                if (first_point)
                  {
                    printf ("M %f,%f", transform_X (x1), transform_Y (y1));
                    first_point = 0;
                  }
                printf (" A %f,%f %f %d,%d %f,%f", rx, ry, rot, large_arc,
                        sweep, transform_X (x2), transform_Y (y2));
              }
              break;
            case 4: // SPLINE - approximate with polyline through control points
              {
                BITCODE_BL k;
                if (seg->num_control_points && seg->control_points)
                  {
                    for (k = 0; k < seg->num_control_points; k++)
                      {
                        double x = seg->control_points[k].point.x;
                        double y = seg->control_points[k].point.y;
                        if (isnan (x) || isnan (y))
                          continue;
                        if (first_point)
                          {
                            printf ("M %f,%f", transform_X (x), transform_Y (y));
                            first_point = 0;
                          }
                        else
                          printf (" L %f,%f", transform_X (x), transform_Y (y));
                      }
                  }
                else if (seg->num_fitpts && seg->fitpts)
                  {
                    for (k = 0; k < seg->num_fitpts; k++)
                      {
                        double x = seg->fitpts[k].x;
                        double y = seg->fitpts[k].y;
                        if (isnan (x) || isnan (y))
                          continue;
                        if (first_point)
                          {
                            printf ("M %f,%f", transform_X (x), transform_Y (y));
                            first_point = 0;
                          }
                        else
                          printf (" L %f,%f", transform_X (x), transform_Y (y));
                      }
                  }
              }
              break;
            default:
              break;
            }
        }
      printf (" Z");
    }
}

static void
output_HATCH (Dwg_Object *obj)
{
  Dwg_Entity_HATCH *hatch = obj->tio.entity->tio.HATCH;
  BITCODE_BL i;
  char *fill_color;
  double lweight;

  if (entity_invisible (obj))
    return;
  if (!hatch->num_paths)
    return;

  fill_color = entity_color (obj);
  lweight = entity_lweight (obj->tio.entity);

  printf ("\t<!-- hatch-%d -->\n", obj->index);

  if (hatch->is_solid_fill)
    {
      printf ("\t<path id=\"dwg-object-%d\" d=\"", obj->index);
      for (i = 0; i < hatch->num_paths; i++)
        {
          output_hatch_path_data (&hatch->paths[i]);
          if (i < hatch->num_paths - 1)
            printf (" ");
        }
      printf ("\"\n\t      style=\"fill:%s;stroke:none;fill-rule:evenodd\" />\n",
              fill_color);
    }
  else
    {
      for (i = 0; i < hatch->num_paths; i++)
        {
          printf ("\t<path id=\"dwg-object-%d-path-%d\" d=\"", obj->index, i);
          output_hatch_path_data (&hatch->paths[i]);
          printf ("\"\n\t      style=\"fill:none;stroke:%s;stroke-width:%.1fpx\" />\n",
                  fill_color, lweight);
        }
    }

  if (*fill_color == '#')
    free (fill_color);
}

// TODO: MINSERT
static void
output_INSERT (Dwg_Object *obj)
{
  Dwg_Entity_INSERT *insert = obj->tio.entity->tio.INSERT;
  if (entity_invisible (obj))
    return;
  if (insert->block_header && insert->block_header->handleref.value
      && insert->block_header->obj)
    {
      BITCODE_3DPOINT ins_pt;
      double rotation_deg;
      double tx, ty;
      Dwg_Object *blk_obj = insert->block_header->obj;
      Dwg_Object_BLOCK_HEADER *hdr;

      if (blk_obj->fixedtype != DWG_TYPE_BLOCK_HEADER)
        return;
      hdr = blk_obj->tio.object->tio.BLOCK_HEADER;

      if (isnan_3BD (insert->ins_pt) || isnan_3BD (insert->extrusion)
          || isnan (insert->rotation) || isnan_3BD (insert->scale))
        return;
      transform_OCS (&ins_pt, insert->ins_pt, insert->extrusion);

      // Negate rotation for SVG coordinate system (Y flipped)
      rotation_deg = -(180.0 / M_PI) * insert->rotation;

      // Symbol has geometry at raw DWG coords (x, y).
      // We need to transform to SVG coords:
      //   Final model X = ins_pt.x - base_pt.x + scale.x * (geom.x - base_pt.x) 
      //   Final model Y = ins_pt.y - base_pt.y + scale.y * (geom.y - base_pt.y)
      // But symbol stores raw geom coords, so:
      //   Final model X = ins_pt.x - base_pt.x + scale.x * geom.x - scale.x * base_pt.x
      //                 = ins_pt.x - base_pt.x * (1 + scale.x) + scale.x * geom.x
      // Actually simpler: for symbols with raw coords, INSERT needs to:
      //   1. Translate symbol so base_pt is at origin: subtract base_pt
      //   2. Apply scale and rotation  
      //   3. Translate to ins_pt
      //   4. Transform to SVG coords
      //
      // Using matrix(a, b, c, d, e, f): (x, y) -> (ax + cy + e, bx + dy + f)
      // We want rotation=0 case first:
      //   X' = sx * (geom.x - base_pt.x) + ins_pt.x
      //      = sx * geom.x + (ins_pt.x - sx * base_pt.x)
      //   In SVG: X' - model_xmin = sx * geom.x + (ins_pt.x - sx * base_pt.x - model_xmin)
      //
      //   Y' = sy * (geom.y - base_pt.y) + ins_pt.y
      //   In SVG: page_height - (Y' - model_ymin)
      //         = page_height - sy * geom.y - ins_pt.y + sy * base_pt.y + model_ymin
      //         = -sy * geom.y + (page_height - ins_pt.y + sy * base_pt.y + model_ymin)
      //
      // So matrix is: (sx, 0, 0, -sy, tx, ty) where
      //   tx = ins_pt.x - sx * base_pt.x - model_xmin
      //   ty = page_height - ins_pt.y + sy * base_pt.y + model_ymin
      {
        double sx = insert->scale.x;
        double sy = insert->scale.y;
        double base_x = hdr->base_pt.x;
        double base_y = hdr->base_pt.y;
        tx = ins_pt.x - sx * base_x - model_xmin;
        ty = page_height - ins_pt.y + sy * base_y + model_ymin;
      }

      printf ("\t<!-- insert-%d -->\n", obj->index);
      // Using matrix for precise control. For rotation=0:
      // matrix(sx, 0, 0, -sy, tx, ty)
      if (fabs (insert->rotation) < 0.0001)
        {
          printf ("\t<use id=\"dwg-object-%d\" transform=\"matrix(%f 0 0 %f %f %f)\" "
                  "xlink:href=\"#symbol-" FORMAT_HV "\" />"
                  "<!-- block_header->handleref: " FORMAT_H " -->\n",
                  obj->index, insert->scale.x, -insert->scale.y, tx, ty,
                  insert->block_header->absolute_ref,
                  ARGS_H (insert->block_header->handleref));
        }
      else
        {
          // With rotation, need full matrix calculation
          // For now, use translate+rotate+scale (may need refinement)
          printf ("\t<use id=\"dwg-object-%d\" transform=\"translate(%f %f) "
                  "rotate(%f) scale(%f %f)\" xlink:href=\"#symbol-" FORMAT_HV
                  "\" />"
                  "<!-- block_header->handleref: " FORMAT_H " -->\n",
                  obj->index, tx, ty,
                  rotation_deg, insert->scale.x, -insert->scale.y,
                  insert->block_header->absolute_ref,
                  ARGS_H (insert->block_header->handleref));
        }
    }
  else
    {
      printf ("\n\n<!-- WRONG INSERT(" FORMAT_H ") -->\n",
              ARGS_H (obj->handle));
    }
}

static void
output_IMAGE (Dwg_Object *obj)
{
  Dwg_Entity_IMAGE *img = obj->tio.entity->tio.IMAGE;
  Dwg_Object_IMAGEDEF *imagedef = NULL;
  double x, y, width, height;
  double ux, uy, vx, vy;
  double a, b, c, d, e, f;
  char *file_path = NULL;
  Dwg_Data *dwg = obj->parent;

  if (entity_invisible (obj))
    return;
  if (isnan_3BD (img->pt0) || isnan_3BD (img->uvec) || isnan_3BD (img->vvec)
      || isnan (img->image_size.x) || isnan (img->image_size.y))
    return;

  // Get IMAGEDEF to retrieve the file path
  if (img->imagedef && img->imagedef->obj
      && img->imagedef->obj->fixedtype == DWG_TYPE_IMAGEDEF)
    {
      imagedef = img->imagedef->obj->tio.object->tio.IMAGEDEF;
      if (imagedef && imagedef->file_path)
        {
          if (dwg->header.version >= R_2007)
            file_path = htmlwescape ((BITCODE_TU)imagedef->file_path);
          else
            file_path = htmlescape (imagedef->file_path, dwg->header.codepage);
        }
    }

  // Calculate the SVG transform matrix
  // The image is defined by:
  //   pt0: insertion point (lower-left corner in WCS)
  //   uvec: vector for one pixel in U direction (scaled by image width gives full width)
  //   vvec: vector for one pixel in V direction (scaled by image height gives full height)
  //
  // For SVG <image>, we need to transform from image space (0,0 at top-left) to model space
  // The transform matrix maps the image (width x height pixels) to model coordinates

  width = img->image_size.x;
  height = img->image_size.y;

  // uvec and vvec are per-pixel vectors, so full size vectors are:
  ux = img->uvec.x * width;
  uy = img->uvec.y * width;
  vx = img->vvec.x * height;
  vy = img->vvec.y * height;

  // SVG image origin is top-left, DWG pt0 is at bottom-left
  // So the top-left corner in model space is: pt0 + vvec * height
  x = img->pt0.x + vx;
  y = img->pt0.y + vy;

  // Build affine transform matrix for SVG
  // SVG matrix(a,b,c,d,e,f) transforms as:
  //   x' = a*x + c*y + e
  //   y' = b*x + d*y + f
  //
  // We want to map image pixels (0..width, 0..height) to model coordinates
  // In image space: u goes right (0 to width), v goes down (0 to height)
  // In model space: u maps to uvec direction, v maps to -vvec direction (since SVG y is flipped)
  //
  // After transform_X/transform_Y, model coords become SVG coords

  // The per-pixel vectors in model space:
  // u_per_pixel = uvec
  // v_per_pixel = vvec (but v in image goes down, model vvec goes up, so we negate)

  // Matrix elements (before Y flip):
  // a = uvec.x (x change per image-u)
  // b = uvec.y (y change per image-u)
  // c = -vvec.x (x change per image-v, negated because image-v is down)
  // d = -vvec.y (y change per image-v, negated)
  // e = x (x origin in model space)
  // f = y (y origin in model space)

  // Apply coordinate transformation (Y flip: y' = page_height - (y - model_ymin))
  a = img->uvec.x;
  b = -img->uvec.y; // Y flip
  c = -img->vvec.x;
  d = img->vvec.y;  // Y flip (double negative)
  e = transform_X (x);
  f = transform_Y (y);

  printf ("\t<!-- image-%d -->\n", obj->index);
  printf ("\t<image id=\"dwg-object-%d\" "
          "width=\"%f\" height=\"%f\" "
          "transform=\"matrix(%f %f %f %f %f %f)\" "
          "xlink:href=\"%s\" "
          "preserveAspectRatio=\"none\" />\n",
          obj->index, width, height, a, b, c, d, e, f,
          file_path ? file_path : "");

  if (file_path)
    free (file_path);
}

static int
output_object (Dwg_Object *obj)
{
  int num = 1;
  if (!obj)
    {
      fprintf (stderr, "object is NULL\n");
      return 0;
    }

  switch (obj->fixedtype)
    {
    case DWG_TYPE_IMAGE:
      output_IMAGE (obj);
      break;
    case DWG_TYPE_INSERT:
      output_INSERT (obj);
      break;
    case DWG_TYPE_LINE:
      output_LINE (obj);
      break;
    case DWG_TYPE_CIRCLE:
      output_CIRCLE (obj);
      break;
    case DWG_TYPE_TEXT:
      output_TEXT (obj);
      break;
    case DWG_TYPE_ATTDEF:
      output_ATTDEF (obj);
      break;
    case DWG_TYPE_ARC:
      output_ARC (obj);
      break;
    case DWG_TYPE_POINT:
      output_POINT (obj);
      break;
    case DWG_TYPE_ELLIPSE:
      output_ELLIPSE (obj);
      break;
    case DWG_TYPE_SOLID:
      output_SOLID (obj);
      break;
    case DWG_TYPE__3DFACE:
      output_3DFACE (obj);
      break;
    case DWG_TYPE_POLYLINE_2D:
      output_POLYLINE_2D (obj);
      break;
    case DWG_TYPE_LWPOLYLINE:
      output_LWPOLYLINE (obj);
      break;
    case DWG_TYPE_RAY:
      output_RAY (obj);
      break;
    case DWG_TYPE_XLINE:
      output_XLINE (obj);
      break;
    case DWG_TYPE_HATCH:
      output_HATCH (obj);
      break;
    case DWG_TYPE_SEQEND:
    case DWG_TYPE_VIEWPORT:
      num = 0; // These don't produce geometry
      break;
    default:
      num = 0;
      if (obj->supertype == DWG_SUPERTYPE_ENTITY)
        fprintf (stderr, "%s ignored\n", obj->name);
      // all other non-graphical objects are silently ignored
      break;
    }
  return num;
}

static int
output_BLOCK_HEADER (Dwg_Object_Ref *ref)
{
  Dwg_Object *obj;
  Dwg_Object_BLOCK_HEADER *hdr;
  int is_g = 0;
  int num = 0;

  if (!ref) // silently ignore empty pspaces
    return 0;
  if (!ref->obj)
    return 0;
  obj = ref->obj;
  if (obj->type != DWG_TYPE_BLOCK_HEADER)
    {
      fprintf (stderr, "Argument not a BLOCK_HEADER reference\n");
      return 0;
    }
  if (!obj->tio.object)
    { // TODO could be an assert also
      fprintf (stderr, "Found null obj->tio.object\n");
      return 0;
    }
  if (!obj->tio.object->tio.BLOCK_HEADER)
    { // TODO could be an assert also
      fprintf (stderr, "Found null obj->tio.object->tio.BLOCK_HEADER\n");
      return 0;
    }

  hdr = obj->tio.object->tio.BLOCK_HEADER;
  if (hdr->name)
    {
      char *escaped;
      Dwg_Data *dwg = obj->parent;
      if (dwg->header.version >= R_2007)
        escaped = htmlwescape ((BITCODE_TU)hdr->name);
      else
        escaped = htmlescape (hdr->name, dwg->header.codepage);
      // fatal: The string "--" is not permitted within comments.
      if (escaped && strstr (escaped, "--"))
        {
          char *s;
          while ((s = strstr (escaped, "--")))
            {
              *s = '_';
              *(s + 1) = '_';
            }
        }
      // don't group *Model_Space or *Paper_Space (case-insensitive)
      if (!escaped || (strcasecmp (escaped, "*Model_Space") != 0
                       && strncasecmp_prefix (escaped, "*Paper_Space") != 0))
        {
          is_g = 1;
          // Set block definition mode with block's base point
          in_block_definition = 1;
          block_base_x = hdr->base_pt.x;
          block_base_y = hdr->base_pt.y;
          printf ("\t<g id=\"symbol-" FORMAT_HV "\" >\n\t\t<!-- %s -->\n",
                  ref->absolute_ref, escaped ? escaped : "");
        }
      else
        printf ("\t<!-- %s -->\n", escaped);
      if (escaped)
        free (escaped);
    }

  obj = get_first_owned_entity (ref->obj);
  while (obj)
    {
      num += output_object (obj);
      obj = get_next_owned_entity (ref->obj, obj);
    }

  if (is_g)
    {
      printf ("\t</g>\n");
      in_block_definition = 0; // restore normal mode
    }
  return num;
}

// Compute bounding box for a single entity (no output, just extents)
static void
compute_entity_extents (Extents *ext, Dwg_Object *obj)
{
  if (!ext || !obj || obj->supertype != DWG_SUPERTYPE_ENTITY)
    return;
  if (entity_invisible (obj))
    return;

  switch (obj->fixedtype)
    {
    case DWG_TYPE_LINE:
      {
        Dwg_Entity_LINE *line = obj->tio.entity->tio.LINE;
        BITCODE_3DPOINT start, end;
        if (isnan_3BD (line->start) || isnan_3BD (line->end)
            || isnan_3BD (line->extrusion))
          break;
        transform_OCS (&start, line->start, line->extrusion);
        transform_OCS (&end, line->end, line->extrusion);
        extents_add_point (ext, start.x, start.y);
        extents_add_point (ext, end.x, end.y);
      }
      break;

    case DWG_TYPE_CIRCLE:
      {
        Dwg_Entity_CIRCLE *circle = obj->tio.entity->tio.CIRCLE;
        BITCODE_3DPOINT center;
        if (isnan_3BD (circle->center) || isnan_3BD (circle->extrusion)
            || isnan (circle->radius))
          break;
        transform_OCS (&center, circle->center, circle->extrusion);
        extents_add_circle (ext, center.x, center.y, circle->radius);
      }
      break;

    case DWG_TYPE_ARC:
      {
        Dwg_Entity_ARC *arc = obj->tio.entity->tio.ARC;
        BITCODE_3DPOINT center;
        if (isnan_3BD (arc->center) || isnan_3BD (arc->extrusion)
            || isnan (arc->radius))
          break;
        transform_OCS (&center, arc->center, arc->extrusion);
        // Conservative: use full circle bounds for arc
        extents_add_circle (ext, center.x, center.y, arc->radius);
      }
      break;

    case DWG_TYPE_POINT:
      {
        Dwg_Entity_POINT *point = obj->tio.entity->tio.POINT;
        BITCODE_3DPOINT pt, pt1;
        pt.x = point->x;
        pt.y = point->y;
        pt.z = point->z;
        if (isnan_3BD (pt) || isnan_3BD (point->extrusion))
          break;
        transform_OCS (&pt1, pt, point->extrusion);
        extents_add_point (ext, pt1.x, pt1.y);
      }
      break;

    case DWG_TYPE_ELLIPSE:
      {
        Dwg_Entity_ELLIPSE *ell = obj->tio.entity->tio.ELLIPSE;
        BITCODE_2DPOINT radius;
        double max_r;
        if (isnan_3BD (ell->center) || isnan_3BD (ell->sm_axis)
            || isnan (ell->axis_ratio))
          break;
        radius.x = sqrt (ell->sm_axis.x * ell->sm_axis.x
                         + ell->sm_axis.y * ell->sm_axis.y);
        radius.y = radius.x * ell->axis_ratio;
        // Conservative: axis-aligned bounding box of ellipse
        max_r = radius.x > radius.y ? radius.x : radius.y;
        extents_add_circle (ext, ell->center.x, ell->center.y, max_r);
      }
      break;

    case DWG_TYPE_TEXT:
      {
        Dwg_Entity_TEXT *text = obj->tio.entity->tio.TEXT;
        BITCODE_2DPOINT pt;
        if (!text->text_value || isnan_2BD (text->ins_pt)
            || isnan_3BD (text->extrusion))
          break;
        transform_OCS_2d (&pt, text->ins_pt, text->extrusion);
        extents_add_point (ext, pt.x, pt.y);
        // Approximate text extent (height-based)
        extents_add_point (ext, pt.x + text->height * 5, pt.y + text->height);
      }
      break;

    case DWG_TYPE_ATTDEF:
      {
        Dwg_Entity_ATTDEF *attdef = obj->tio.entity->tio.ATTDEF;
        BITCODE_2DPOINT pt;
        if (!attdef->tag || isnan_2BD (attdef->ins_pt)
            || isnan_3BD (attdef->extrusion))
          break;
        transform_OCS_2d (&pt, attdef->ins_pt, attdef->extrusion);
        extents_add_point (ext, pt.x, pt.y);
        // Approximate text extent (height-based)
        extents_add_point (ext, pt.x + attdef->height * 5, pt.y + attdef->height);
      }
      break;

    case DWG_TYPE_SOLID:
      {
        Dwg_Entity_SOLID *sol = obj->tio.entity->tio.SOLID;
        BITCODE_2DPOINT c1, c2, c3, c4;
        BITCODE_2DPOINT s1, s2, s3, s4;
        memcpy (&s1, &sol->corner1, sizeof s1);
        memcpy (&s2, &sol->corner2, sizeof s1);
        memcpy (&s3, &sol->corner3, sizeof s1);
        memcpy (&s4, &sol->corner4, sizeof s1);
        if (isnan_2BD (s1) || isnan_2BD (s2) || isnan_2BD (s3)
            || isnan_2BD (s4))
          break;
        transform_OCS_2d (&c1, s1, sol->extrusion);
        transform_OCS_2d (&c2, s2, sol->extrusion);
        transform_OCS_2d (&c3, s3, sol->extrusion);
        transform_OCS_2d (&c4, s4, sol->extrusion);
        extents_add_point (ext, c1.x, c1.y);
        extents_add_point (ext, c2.x, c2.y);
        extents_add_point (ext, c3.x, c3.y);
        extents_add_point (ext, c4.x, c4.y);
      }
      break;

    case DWG_TYPE__3DFACE:
      {
        Dwg_Entity__3DFACE *ent = obj->tio.entity->tio._3DFACE;
        if (isnan_3BD (ent->corner1) || isnan_3BD (ent->corner2)
            || isnan_3BD (ent->corner3) || isnan_3BD (ent->corner4))
          break;
        extents_add_point (ext, ent->corner1.x, ent->corner1.y);
        extents_add_point (ext, ent->corner2.x, ent->corner2.y);
        extents_add_point (ext, ent->corner3.x, ent->corner3.y);
        extents_add_point (ext, ent->corner4.x, ent->corner4.y);
      }
      break;

    case DWG_TYPE_POLYLINE_2D:
      {
        int error;
        Dwg_Entity_POLYLINE_2D *pline = obj->tio.entity->tio.POLYLINE_2D;
        BITCODE_RL numpts = dwg_object_polyline_2d_get_numpoints (obj, &error);
        if (numpts && !error)
          {
            dwg_point_2d *pts
                = dwg_object_polyline_2d_get_points (obj, &error);
            if (!error && pts)
              {
                BITCODE_RL j;
                for (j = 0; j < numpts; j++)
                  {
                    BITCODE_2DPOINT ptin, pt;
                    ptin.x = pts[j].x;
                    ptin.y = pts[j].y;
                    if (isnan_2BD (ptin))
                      continue;
                    transform_OCS_2d (&pt, ptin, pline->extrusion);
                    extents_add_point (ext, pt.x, pt.y);
                  }
                free (pts);
              }
          }
      }
      break;

    case DWG_TYPE_LWPOLYLINE:
      {
        int error;
        Dwg_Entity_LWPOLYLINE *pline = obj->tio.entity->tio.LWPOLYLINE;
        BITCODE_RL numpts = dwg_ent_lwpline_get_numpoints (pline, &error);
        if (numpts && !error)
          {
            dwg_point_2d *pts = dwg_ent_lwpline_get_points (pline, &error);
            if (!error && pts)
              {
                BITCODE_RL j;
                for (j = 0; j < numpts; j++)
                  {
                    BITCODE_2DPOINT ptin, pt;
                    ptin.x = pts[j].x;
                    ptin.y = pts[j].y;
                    if (isnan_2BD (ptin))
                      continue;
                    transform_OCS_2d (&pt, ptin, pline->extrusion);
                    extents_add_point (ext, pt.x, pt.y);
                  }
                free (pts);
              }
          }
      }
      break;

    case DWG_TYPE_INSERT:
      {
        Dwg_Entity_INSERT *insert = obj->tio.entity->tio.INSERT;
        BITCODE_3DPOINT ins_pt;
        Dwg_Object *blk_obj;
        Dwg_Object_BLOCK_HEADER *hdr;
        Extents block_ext;
        double sx, sy, base_x, base_y, cos_r, sin_r;
        double corners[4][2];
        int i;

        if (!insert->block_header || !insert->block_header->handleref.value
            || !insert->block_header->obj)
          break;
        if (isnan_3BD (insert->ins_pt) || isnan_3BD (insert->extrusion)
            || isnan_3BD (insert->scale) || isnan (insert->rotation))
          break;

        blk_obj = insert->block_header->obj;
        if (blk_obj->fixedtype != DWG_TYPE_BLOCK_HEADER)
          break;
        hdr = blk_obj->tio.object->tio.BLOCK_HEADER;

        transform_OCS (&ins_pt, insert->ins_pt, insert->extrusion);

        // Compute extents of the block's geometry
        extents_init (&block_ext);
        compute_block_extents (&block_ext, insert->block_header);

        if (!block_ext.initialized)
          {
            // Fallback: just add insertion point if block is empty
            extents_add_point (ext, ins_pt.x, ins_pt.y);
            break;
          }

        // Transform block extents by INSERT's scale, rotation, and position
        sx = insert->scale.x;
        sy = insert->scale.y;
        base_x = hdr->base_pt.x;
        base_y = hdr->base_pt.y;
        cos_r = cos (insert->rotation);
        sin_r = sin (insert->rotation);

        // Four corners of block bounding box (relative to base point)
        corners[0][0] = block_ext.xmin - base_x;
        corners[0][1] = block_ext.ymin - base_y;
        corners[1][0] = block_ext.xmax - base_x;
        corners[1][1] = block_ext.ymin - base_y;
        corners[2][0] = block_ext.xmax - base_x;
        corners[2][1] = block_ext.ymax - base_y;
        corners[3][0] = block_ext.xmin - base_x;
        corners[3][1] = block_ext.ymax - base_y;

        // Transform each corner: scale, rotate, translate to insertion point
        for (i = 0; i < 4; i++)
          {
            double lx = corners[i][0] * sx;
            double ly = corners[i][1] * sy;
            double rx = lx * cos_r - ly * sin_r;
            double ry = lx * sin_r + ly * cos_r;
            extents_add_point (ext, ins_pt.x + rx, ins_pt.y + ry);
          }
      }
      break;

    case DWG_TYPE_HATCH:
      {
        Dwg_Entity_HATCH *hatch = obj->tio.entity->tio.HATCH;
        BITCODE_BL i, j;
        if (!hatch->num_paths)
          break;
        for (i = 0; i < hatch->num_paths; i++)
          {
            Dwg_HATCH_Path *path = &hatch->paths[i];
            int is_polyline = path->flag & 2;
            if (is_polyline && path->polyline_paths)
              {
                for (j = 0; j < path->num_segs_or_paths; j++)
                  {
                    double x = path->polyline_paths[j].point.x;
                    double y = path->polyline_paths[j].point.y;
                    if (!isnan (x) && !isnan (y))
                      extents_add_point (ext, x, y);
                  }
              }
            else if (path->segs)
              {
                for (j = 0; j < path->num_segs_or_paths; j++)
                  {
                    Dwg_HATCH_PathSeg *seg = &path->segs[j];
                    switch (seg->curve_type)
                      {
                      case 1: // LINE
                        if (!isnan (seg->first_endpoint.x)
                            && !isnan (seg->first_endpoint.y))
                          extents_add_point (ext, seg->first_endpoint.x,
                                             seg->first_endpoint.y);
                        if (!isnan (seg->second_endpoint.x)
                            && !isnan (seg->second_endpoint.y))
                          extents_add_point (ext, seg->second_endpoint.x,
                                             seg->second_endpoint.y);
                        break;
                      case 2: // CIRCULAR ARC
                        if (!isnan (seg->center.x) && !isnan (seg->center.y)
                            && !isnan (seg->radius))
                          extents_add_circle (ext, seg->center.x, seg->center.y,
                                              seg->radius);
                        break;
                      case 3: // ELLIPTICAL ARC
                        {
                          double rx = sqrt (seg->endpoint.x * seg->endpoint.x
                                            + seg->endpoint.y * seg->endpoint.y);
                          double ry = rx * seg->minor_major_ratio;
                          double max_r = rx > ry ? rx : ry;
                          if (!isnan (seg->center.x) && !isnan (seg->center.y)
                              && !isnan (max_r))
                            extents_add_circle (ext, seg->center.x,
                                                seg->center.y, max_r);
                        }
                        break;
                      case 4: // SPLINE
                        {
                          BITCODE_BL k;
                          if (seg->num_control_points && seg->control_points)
                            {
                              for (k = 0; k < seg->num_control_points; k++)
                                {
                                  double x = seg->control_points[k].point.x;
                                  double y = seg->control_points[k].point.y;
                                  if (!isnan (x) && !isnan (y))
                                    extents_add_point (ext, x, y);
                                }
                            }
                          if (seg->num_fitpts && seg->fitpts)
                            {
                              for (k = 0; k < seg->num_fitpts; k++)
                                {
                                  double x = seg->fitpts[k].x;
                                  double y = seg->fitpts[k].y;
                                  if (!isnan (x) && !isnan (y))
                                    extents_add_point (ext, x, y);
                                }
                            }
                        }
                        break;
                      default:
                        break;
                      }
                  }
              }
          }
      }
      break;

    case DWG_TYPE_IMAGE:
      {
        Dwg_Entity_IMAGE *img = obj->tio.entity->tio.IMAGE;
        double width, height;
        double ux, uy, vx, vy;
        double x0, y0, x1, y1, x2, y2, x3, y3;

        if (isnan_3BD (img->pt0) || isnan_3BD (img->uvec)
            || isnan_3BD (img->vvec) || isnan (img->image_size.x)
            || isnan (img->image_size.y))
          break;

        width = img->image_size.x;
        height = img->image_size.y;

        // Full size vectors
        ux = img->uvec.x * width;
        uy = img->uvec.y * width;
        vx = img->vvec.x * height;
        vy = img->vvec.y * height;

        // Four corners of the image in model space
        // pt0 is the lower-left corner
        x0 = img->pt0.x;
        y0 = img->pt0.y;
        x1 = img->pt0.x + ux;
        y1 = img->pt0.y + uy;
        x2 = img->pt0.x + ux + vx;
        y2 = img->pt0.y + uy + vy;
        x3 = img->pt0.x + vx;
        y3 = img->pt0.y + vy;

        extents_add_point (ext, x0, y0);
        extents_add_point (ext, x1, y1);
        extents_add_point (ext, x2, y2);
        extents_add_point (ext, x3, y3);
      }
      break;

    default:
      break;
    }
}

// Compute extents for all entities in a block
static void
compute_block_extents (Extents *ext, Dwg_Object_Ref *ref)
{
  Dwg_Object *obj;

  if (!ext || !ref || !ref->obj)
    return;
  if (ref->obj->type != DWG_TYPE_BLOCK_HEADER)
    return;

  obj = get_first_owned_entity (ref->obj);
  while (obj)
    {
      compute_entity_extents (ext, obj);
      obj = get_next_owned_entity (ref->obj, obj);
    }
}

// Compute actual geometry extents for the drawing
static void
compute_modelspace_extents (Dwg_Data *dwg)
{
  Extents ext;
  Dwg_Object_Ref *ref;

  extents_init (&ext);

  // Compute extents from paper space if available
  if (!mspace && (ref = dwg_paper_space_ref (dwg)))
    compute_block_extents (&ext, ref);

  // Always compute model space
  if ((ref = dwg_model_space_ref (dwg)))
    compute_block_extents (&ext, ref);

  // If we found geometry, use computed extents
  if (ext.initialized)
    {
      model_xmin = ext.xmin;
      model_ymin = ext.ymin;
      model_xmax = ext.xmax;
      model_ymax = ext.ymax;
    }
  else
    {
      // Fallback to header values
      model_xmin = dwg_model_x_min (dwg);
      model_ymin = dwg_model_y_min (dwg);
      model_xmax = dwg_model_x_max (dwg);
      model_ymax = dwg_model_y_max (dwg);
    }
}

static void
output_SVG (Dwg_Data *dwg)
{
  BITCODE_BS i;
  int num = 0;
  Dwg_Object *obj;
  Dwg_Object_Ref *ref;
  Dwg_Object_BLOCK_CONTROL *block_control;
  double dx, dy;

  // Compute actual geometry extents instead of using header values
  compute_modelspace_extents (dwg);

  dx = model_xmax - model_xmin;
  dy = model_ymax - model_ymin;
  // double scale_x = dx / (dwg_page_x_max(dwg) - dwg_page_x_min(dwg));
  // double scale_y = dy / (dwg_page_y_max(dwg) - dwg_page_y_min(dwg));
  scale = 25.4 / 72; // pt:mm
  if (isnan (dx) || dx <= 0.0)
    dx = 100.0;
  if (isnan (dy) || dy <= 0.0)
    dy = 100.0;
  page_width = dx;
  page_height = dy;
  // scale *= (scale_x > scale_y ? scale_x : scale_y);

  // optional, for xmllint
  // <!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.1//EN"
  //   "http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd">
  // But we use jing with relaxng, which is better. Just LaTeXML shipped a
  // broken rng
  printf ("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n"
          "<svg\n"
          "   xmlns:svg=\"http://www.w3.org/2000/svg\"\n"
          "   xmlns=\"http://www.w3.org/2000/svg\"\n"
          "   xmlns:xlink=\"http://www.w3.org/1999/xlink\"\n"
          "   data-gen-vers=\"2026-01-26a\"\n"
          "   version=\"1.1\" baseProfile=\"basic\"\n"
          "   width=\"100%%\" height=\"100%%\"\n"
          "   viewBox=\"%f %f %f %f\">\n",
          0.0, 0.0, page_width, page_height);

  if (!mspace && (ref = dwg_paper_space_ref (dwg)))
    num = output_BLOCK_HEADER (
        ref); // how many paper-space entities we did print
  if (!num && (ref = dwg_model_space_ref (dwg)))
    output_BLOCK_HEADER (ref);
  printf ("\t<defs>\n");
  for (i = 0; i < dwg->block_control.num_entries; i++)
    {
      if (dwg->block_control.entries && (ref = dwg->block_control.entries[i]))
        output_BLOCK_HEADER (ref);
    }
  printf ("\t</defs>\n");
  printf ("</svg>\n");
  fflush (stdout);
}

#ifndef DWG2SVG_NO_MAIN
int
main (int argc, char *argv[])
{
  int error;
  int force_free = 0;
  int i = 1;
  int c;
#ifdef HAVE_GETOPT_LONG
  int option_index = 0;
  static struct option long_options[]
      = { { "verbose", 1, &opts, 1 }, // optional
          { "mspace", 0, 0, 0 },      { "force-free", 0, 0, 0 },
          { "help", 0, 0, 0 },        { "version", 0, 0, 0 },
          { NULL, 0, NULL, 0 } };
#endif

  if (argc < 2)
    return usage ();

  while
#ifdef HAVE_GETOPT_LONG
      ((c = getopt_long (argc, argv, ":v:m::h", long_options, &option_index))
       != -1)
#else
      ((c = getopt (argc, argv, ":v:m::hi")) != -1)
#endif
    {
      if (c == -1)
        break;
      switch (c)
        {
        case ':': // missing arg
          if (optarg && !strcmp (optarg, "v"))
            {
              opts = 1;
              break;
            }
          fprintf (stderr, "%s: option '-%c' requires an argument\n", argv[0],
                   optopt);
          break;
#ifdef HAVE_GETOPT_LONG
        case 0:
          /* This option sets a flag */
          if (!strcmp (long_options[option_index].name, "verbose"))
            {
              if (opts < 0 || opts > 9)
                return usage ();
#  if defined(USE_TRACING) && defined(HAVE_SETENV)
              {
                char v[2];
                *v = opts + '0';
                *(v + 1) = 0;
                setenv ("LIBREDWG_TRACE", v, 1);
              }
#  endif
              break;
            }
          if (!strcmp (long_options[option_index].name, "version"))
            return opt_version ();
          if (!strcmp (long_options[option_index].name, "help"))
            return help ();
          if (!strcmp (long_options[option_index].name, "force-free"))
            force_free = 1;
          if (!strcmp (long_options[option_index].name, "mspace"))
            mspace = 1;
          break;
#else
        case 'i':
          return opt_version ();
#endif
        case 'v': // support -v3 and -v
          i = (optind > 0 && optind < argc) ? optind - 1 : 1;
          if (!memcmp (argv[i], "-v", 2))
            {
              opts = argv[i][2] ? argv[i][2] - '0' : 1;
            }
          if (opts < 0 || opts > 9)
            return usage ();
#if defined(USE_TRACING) && defined(HAVE_SETENV)
          {
            char v[2];
            *v = opts + '0';
            *(v + 1) = 0;
            setenv ("LIBREDWG_TRACE", v, 1);
          }
#endif
          break;
        case 'h':
          return help ();
        case '?':
          fprintf (stderr, "%s: invalid option '-%c' ignored\n", argv[0],
                   optopt);
          break;
        default:
          return usage ();
        }
    }
  i = optind;
  if (i >= argc)
    return usage ();

  memset (&g_dwg, 0, sizeof (Dwg_Data));
  g_dwg.opts = opts;
  error = dwg_read_file (argv[i], &g_dwg);

  if (opts)
    fprintf (stderr, "\nSVG\n===\n");
  if (error < DWG_ERR_CRITICAL)
    output_SVG (&g_dwg);

#if defined __SANITIZE_ADDRESS__ || __has_feature(address_sanitizer)
  {
    char *asanenv = getenv ("ASAN_OPTIONS");
    if (!asanenv)
      force_free = 1;
    // detect_leaks is enabled by default. see if it's turned off
    else if (strstr (asanenv, "detect_leaks=0") == NULL) /* not found */
      force_free = 1;
  }
#endif

  // forget about leaks. really huge DWG's need endlessly here.
  if ((g_dwg.header.version && g_dwg.num_objects < 1000) || force_free
#ifdef HAVE_VALGRIND_VALGRIND_H
      || (RUNNING_ON_VALGRIND)
#endif
  )
    {
      dwg_free (&g_dwg);
    }
  return error >= DWG_ERR_CRITICAL ? 1 : 0;
}
#endif /* !DWG2SVG_NO_MAIN */
