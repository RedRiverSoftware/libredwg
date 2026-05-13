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

/* Per-call rendering state.  Marked _Thread_local so concurrent calls to
   the SVG entry points in dwg_svg_api.c don't trample each other — each
   thread gets its own copy.  This replaced a process-wide mutex that
   previously serialised all renderer calls. */
static _Thread_local int opts = 0;
static _Thread_local int mspace = 0; // only mspace, even when pspace is defined
static _Thread_local int in_block_definition = 0; // 1 when outputting block symbol entities
static _Thread_local int paper_space_bg = 0; // 1 when rendering onto a white paper-space background

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
static _Thread_local double block_base_x = 0.0, block_base_y = 0.0; // current block's base_pt
_Thread_local Dwg_Data g_dwg;
_Thread_local double model_xmin, model_ymin, model_xmax, model_ymax;
_Thread_local double page_width, page_height, scale;

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
static void output_bulge_arc (double x1, double y1, double x2, double y2,
                              double bulge);
static int output_object (Dwg_Object *obj);
static char *layer_safe_id (const char *attr_name);
static char *insert_effective_layer (Dwg_Object *obj, Dwg_Data *dwg,
                                     const char *parent_eff_layer);

/* Thread-local effective-layer state used by output_INSERT and
   output_BLOCK_HEADER to drive the layer-0-in-block inheritance rule.
   The BlockCombo machinery below assigns these before each <defs>
   clone emission and resets them to NULL afterwards. */
static _Thread_local const char *current_eff_layer = NULL;
static _Thread_local const char *current_eff_layer_safe = NULL;

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

/* For entities WITH an explicit lineweight (set on the entity or its
   layer), the paper-space mm value is multiplied by this factor so the
   stroke is clearly visible on screen.  At this value a 0.25mm lineweight
   (the common default) renders at ~1 viewport-unit — same visual thickness
   as DEFAULT_STROKE_WIDTH_PX — so thin explicit lineweights look fine,
   while thicker weights (0.5mm, 0.7mm, 1.0mm) scale up proportionally.
   Because these strokes do NOT get non-scaling-stroke, they also inflate
   with any enclosing block transform, matching AutoCAD's semantics for
   entities that have a deliberate drawing-unit thickness. */
#define LINEWEIGHT_DISPLAY_SCALE 4.0

/* For entities with default / ByBlock / unset lineweight we render at a
   fixed minimum stroke width in the root viewport's user units.  These
   strokes DO use non-scaling-stroke, so they stay consistent across the
   drawing regardless of block transforms. */
#define DEFAULT_STROKE_WIDTH_PX 1.0

/* Resolved lineweight in paper-space millimetres, or 0.0 when the entity
   uses default / ByBlock / unset lineweight.  Resolves ByLayer. */
static double
entity_lweight_mm (Dwg_Object_Entity *ent)
{
  int lw = dxf_cvt_lweight (ent->linewt);
  if (lw == -1 && ent->layer && ent->layer->obj
      && ent->layer->obj->fixedtype == DWG_TYPE_LAYER)
    {
      Dwg_Object_LAYER *layer = ent->layer->obj->tio.object->tio.LAYER;
      lw = dxf_cvt_lweight (layer->linewt);
    }
  return lw > 0 ? (double)lw / 100.0 : 0.0;
}

/* Whether the entity has an explicit (non-default) lineweight.  Used to
   decide if the SVG stroke should be given vector-effect="non-scaling-stroke". */
static int
entity_lweight_is_explicit (Dwg_Object_Entity *ent)
{
  return entity_lweight_mm (ent) > 0.0 ? 1 : 0;
}

static double
entity_lweight (Dwg_Object_Entity *ent)
{
  double mm = entity_lweight_mm (ent);
  if (mm <= 0.0)
    return DEFAULT_STROKE_WIDTH_PX;
  return mm * LINEWEIGHT_DISPLAY_SCALE;
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
        // ACI 7 is the "foreground" colour — it contrasts with the background.
        // White on a dark model-space canvas, black on a light paper-space page.
        return (char *)(paper_space_bg ? "black" : "white");
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

  /* ByLayer (256) and ByBlock (0) both resolve to the layer's color for
     top-level entities.  AutoCAD treats top-level ByBlock entities as
     ByLayer because there is no enclosing block reference to inherit from;
     the calling INSERT's color isn't propagated through dwg2SVG's block
     expansion either, so block-defined ByBlock entities also fall back to
     the layer's color.  This is closer to AutoCAD's display than returning
     literal black.  Don't redirect when the entity carries an explicit
     truecolor (`flag & 0x80`) — that's a deliberate per-entity RGB value. */
  if ((ent->color.index == 256 || ent->color.index == 0)
      && !(ent->color.flag & 0x80))
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

/* Returns the resolved ACI index for an entity (0-255), or -1 for truecolor.
   Resolves ByLayer (256) and ByBlock (0) to the layer's ACI index — see
   entity_color() for the reasoning. */
static int
entity_aci_index (Dwg_Object *obj)
{
  Dwg_Object_Entity *ent = obj->tio.entity;
  BITCODE_CMC *color;

  if ((ent->color.index == 256 || ent->color.index == 0)
      && !(ent->color.flag & 0x80))
    {
      if (ent->layer && ent->layer->obj
          && ent->layer->obj->fixedtype == DWG_TYPE_LAYER)
        {
          Dwg_Object_LAYER *layer
              = ent->layer->obj->tio.object->tio.LAYER;
          color = &layer->color;
        }
      else
        color = &ent->color;
    }
  else
    color = &ent->color;

  /* Truecolor: flag bit 0x80 set, bit 0x40 clear => RGB, not ACI */
  if (color->flag & 0x80 && !(color->flag & 0x40))
    return -1;

  /* ACI stored in low byte of rgb with method 0xc3 (layer encoding) */
  if (color->index == 256 && (color->rgb >> 24) == 0xc3)
    return (int)(color->rgb & 0xff);

  if (color->index >= 0 && color->index < 256)
    return (int)color->index;

  return 7; /* fallback to foreground */
}

/* Resolve an entity's effective linetype, following ByLayer if needed.
   Returns NULL for Continuous (solid) lines. */
static Dwg_Object_LTYPE *
entity_ltype (Dwg_Object *obj)
{
  Dwg_Data *dwg = obj->parent;
  Dwg_Object_Entity *ent = obj->tio.entity;
  BITCODE_H ltype_h = NULL;

  /* ltype_flags: 0=ByLayer, 1=ByBlock, 2=Continuous, 3=explicit handle */
  if (ent->ltype_flags == 2)
    return NULL; /* Continuous — solid line */
  if (ent->ltype_flags == 3 && ent->ltype)
    ltype_h = ent->ltype;
  else if (ent->ltype_flags == 0) /* ByLayer */
    {
      if (ent->layer && ent->layer->obj
          && ent->layer->obj->fixedtype == DWG_TYPE_LAYER)
        {
          Dwg_Object_LAYER *layer
              = ent->layer->obj->tio.object->tio.LAYER;
          ltype_h = layer->ltype;
        }
    }
  /* ByBlock (1) and unresolved: treat as Continuous */
  if (!ltype_h)
    return NULL;

  {
    Dwg_Object *o = dwg_ref_object_silent (dwg, ltype_h);
    if (o && o->fixedtype == DWG_TYPE_LTYPE)
      {
        Dwg_Object_LTYPE *lt = o->tio.object->tio.LTYPE;
        /* Continuous / ByBlock linetypes have numdashes == 0 */
        if (lt->numdashes > 0)
          return lt;
      }
  }
  return NULL;
}

/* Build an SVG stroke-dasharray string from a linetype's dash pattern.
   Caller must free() the returned string.  Returns NULL for solid lines. */
static char *
entity_dasharray (Dwg_Object *obj)
{
  Dwg_Object_LTYPE *lt = entity_ltype (obj);
  Dwg_Object_Entity *ent;
  double lt_scale;
  char buf[512];
  int pos = 0;
  int i;

  if (!lt)
    return NULL;

  ent = obj->tio.entity;
  /* Effective scale = entity ltype_scale * global LTSCALE
     When PSLTSCALE=1 (the default in modern DWGs) linetypes in paper
     space render at their defined paper-space length regardless of the
     global LTSCALE, so we skip the LTSCALE multiplier in that case. */
  lt_scale = ent->ltype_scale > 0.0 ? ent->ltype_scale : 1.0;
  {
    Dwg_Data *dwg = obj->parent;
    double global_ltscale = dwg->header_vars.LTSCALE;
    int psltscale = dwg->header_vars.PSLTSCALE;
    if (!psltscale && global_ltscale > 0.0)
      lt_scale *= global_ltscale;
  }

  for (i = 0; i < lt->numdashes && pos < (int)sizeof (buf) - 20; i++)
    {
      double len = lt->dashes[i].length * lt_scale;
      if (len < 0.0)
        len = -len; /* gaps are stored as negative */
      if (len < 0.01)
        len = 0.01; /* SVG needs non-zero for dots */
      if (pos > 0)
        pos += sprintf (buf + pos, " ");
      pos += sprintf (buf + pos, "%.2f", len);
    }

  if (pos == 0)
    return NULL;

  {
    char *result = (char *)malloc ((size_t)(pos + 1));
    memcpy (result, buf, (size_t)(pos + 1));
    return result;
  }
}

/* Emit the closing attributes and style for a stroked SVG element. */
static void
common_entity (Dwg_Object *obj)
{
  double lweight;
  char *color;
  char *dashes;
  int aci;
  const char *ve_attr;
  lweight = entity_lweight (obj->tio.entity);
  color = entity_color (obj);
  aci = entity_aci_index (obj);
  dashes = entity_dasharray (obj);
  /* Only default-lineweight strokes get non-scaling-stroke, so they stay
     at a uniform minimum thickness even inside non-uniformly scaled INSERT
     transforms.  Explicit lineweights are meant to be prominent and are
     allowed to scale with their enclosing transform (typically they're
     only used on paper-space entities that aren't inside such blocks). */
  ve_attr = entity_lweight_is_explicit (obj->tio.entity)
                ? ""
                : " vector-effect=\"non-scaling-stroke\"";
  if (dashes)
    printf ("      data-aci=\"%d\"%s"
            " style=\"fill:none;stroke:%s;stroke-width:%.2fpx;"
            "stroke-dasharray:%s;stroke-linecap:round\" />\n",
            aci, ve_attr, color, lweight, dashes);
  else
    printf ("      data-aci=\"%d\"%s"
            " style=\"fill:none;stroke:%s;stroke-width:%.2fpx\" />\n",
            aci, ve_attr, color, lweight);
  if (*color == '#')
    free (color);
  free (dashes);
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
                     double width_factor, const char *escaped, int aci)
{
  int has_rotation = fabs (rotation_deg) > 0.001;
  int has_scale = fabs (width_factor - 1.0) > 0.001;
  double tx = has_scale ? x / width_factor : x;
  /* When inside a block definition the INSERT transform applies a Y-flip
     (matrix with -sy) which mirrors text.  Counter this by rendering text
     with scale(1,-1) and negated Y so the double-flip produces readable
     text while keeping the position correct. */
  double render_y = in_block_definition ? -y : y;
  int ys = in_block_definition ? -1 : 1;

  printf ("\t<text id=\"dwg-object-%d\" x=\"%f\" y=\"%f\" "
          "font-family=\"%s\" font-size=\"%f\" fill=\"%s\" "
          "text-anchor=\"%s\" dominant-baseline=\"%s\" data-aci=\"%d\"",
          obj->index, tx, render_y, fontfamily, font_size, color,
          text_anchor, dominant_baseline, aci);

  if (has_rotation && in_block_definition)
    {
      /* When text is rotated inside a block definition, the naive
         rotate(angle, cx, cy) scale(1, -1) produces wrong positions:
         the scale moves the text away from the rotation centre and the
         rotation amplifies that offset, scattering text diagonally.
         Instead, compute a single matrix that combines rotation, Y-flip
         (for parent INSERT Y-flip compensation), and optional width
         scaling while preserving the text's anchor position.

         Derivation:
           T = translate(x, y) * rotate(α) * scale(wf, -1)
                 * translate(-x/wf, y)
         which simplifies to the matrix below. */
      double rot_rad = rotation_deg * M_PI / 180.0;
      double c = cos (rot_rad);
      double s = sin (rot_rad);
      double wf = has_scale ? width_factor : 1.0;
      printf (" transform=\"matrix(%f %f %f %f %f %f)\"",
              c * wf, s * wf, s, -c,
              x * (1 - c) + s * y,
              y * (1 - c) - s * x);
    }
  else if (has_rotation && has_scale)
    printf (" transform=\"rotate(%f %f %f) scale(%f %d)\"",
            -rotation_deg, tx, render_y, width_factor, ys);
  else if (has_rotation)
    printf (" transform=\"rotate(%f %f %f)\"", -rotation_deg, tx, render_y);
  else if (has_scale)
    printf (" transform=\"scale(%f %d)\"", width_factor, ys);
  else if (in_block_definition)
    printf (" transform=\"scale(1 -1)\"");

  printf (">%s</text>\n", escaped ? escaped : "");
}

/* ===========================================================================
   MTEXT inline-formatting parser.
   ---------------------------------------------------------------------------
   AutoCAD MTEXT embeds formatting codes in the text string:
     \P                   paragraph break  -> tspan on a new line
     \~                   non-breaking space (U+00A0)
     \\                   literal backslash
     \{ \}                literal braces
     {...}                grouping (push/pop style)
     \L ... \l            underline on/off
     \O ... \o            overline on/off
     \K ... \k            strike-through on/off
     \C<n>;               ACI color (256 = revert to base, ByLayer)
     \H<n>;  \H<n>x;      text height (absolute or relative to base)
     \f<face>|b<0|1>|i<0|1>|c<n>|p<n>;
                          font face, bold, italic (codepage/pitch ignored)
     \F<file>;            font file (extension stripped)
     \W<n>; \Q<n>; \T<n>; \A<n>; \p<args>;
                          width / oblique / tracking / vertical-align /
                          paragraph indent — consumed but not rendered
                          (negligible visual impact in the corpus)
     \S<num>^<den>;       stacked fraction (rendered flat as num/den)
     \U+XXXX              Unicode codepoint
     %%c %%d %%p          diameter / degree / plus-minus symbols
     %%u %%o              underline / overline toggles

   The parser produces a list of `Mtext_Run` segments, each with a snapshot
   of the active style.  output_MTEXT walks the list emitting one <tspan>
   per run, advancing dy on paragraph breaks. */

/* Escape an MTEXT run's text for direct embedding in SVG.  Unlike the
   shared htmlescape() which interprets each byte through the DWG codepage
   (turning UTF-8 multi-byte sequences like ⌀ E2 8C 80 into garbled
   &#xE2;&#x152;&#x20AC;), this function treats the input as UTF-8 — what
   the parser already produces — and only escapes XML metacharacters.  The
   SVG declares encoding="UTF-8" so the multi-byte bytes pass through
   untouched and the browser decodes them correctly.  Caller frees. */
static char *
mtext_escape_utf8 (const char *src)
{
  size_t len, i, pos = 0, cap;
  char *out;
  if (!src)
    return strdup ("");
  len = strlen (src);
  /* Worst case: every byte expands to "&quot;" (6 chars). */
  cap = len * 6 + 1;
  out = (char *)malloc (cap);
  if (!out)
    return NULL;
  for (i = 0; i < len; i++)
    {
      unsigned char c = (unsigned char)src[i];
      const char *replacement = NULL;
      size_t rlen = 0;
      switch (c)
        {
        case '&':  replacement = "&amp;";  rlen = 5; break;
        case '<':  replacement = "&lt;";   rlen = 4; break;
        case '>':  replacement = "&gt;";   rlen = 4; break;
        case '"':  replacement = "&quot;"; rlen = 6; break;
        case '\'': replacement = "&#39;";  rlen = 5; break;
        case '{':  replacement = "&#123;"; rlen = 6; break;
        case '}':  replacement = "&#125;"; rlen = 6; break;
        default: break;
        }
      if (replacement)
        {
          memcpy (out + pos, replacement, rlen);
          pos += rlen;
        }
      else
        {
          /* Pass the byte through.  ASCII (< 0x80) is fine; high bytes
             are part of UTF-8 multi-byte sequences and are decoded by the
             SVG renderer. */
          out[pos++] = (char)c;
        }
    }
  out[pos] = '\0';
  return out;
}

#define MTEXT_GROUP_STACK_MAX 16

typedef struct
{
  int    underline;
  int    overline;
  int    strike;
  int    bold;
  int    italic;
  char  *font_family;     /* malloc'd, NULL = inherit base */
  int    has_color;       /* 1 when \C<n>; with n != 256/0 is active */
  int    aci_color_idx;
  double height_scale;    /* multiplier on base font_size; 1.0 = inherit */
} Mtext_Style;

typedef struct
{
  char        *text;            /* malloc'd UTF-8, escape codes resolved.
                                   Never empty for content runs; emitted only
                                   when the parser flushes a non-empty buf. */
  int          newlines_before; /* 0 = continue current line, 1 = next line,
                                   2 = next line plus 1 blank line, etc.
                                   Renderer emits (n - 1) blank tspans before
                                   the content tspan when n >= 1. */
  Mtext_Style  style;           /* snapshot of style active for this run */
  char        *denom;           /* non-NULL for \S stacked fractions */
} Mtext_Run;

typedef struct
{
  char  *data;
  size_t len;
  size_t cap;
} Mtext_Buf;

typedef struct
{
  Mtext_Run *runs;
  size_t     count;
  size_t     cap;
} Mtext_Runs;

static void
mtext_style_init (Mtext_Style *s)
{
  s->underline = 0;
  s->overline = 0;
  s->strike = 0;
  s->bold = 0;
  s->italic = 0;
  s->font_family = NULL;
  s->has_color = 0;
  s->aci_color_idx = 0;
  s->height_scale = 1.0;
}

/* Deep-copy `src` into `dst`.  `dst` must be uninitialised or just freed. */
static void
mtext_style_copy (Mtext_Style *dst, const Mtext_Style *src)
{
  *dst = *src;
  if (src->font_family)
    dst->font_family = strdup (src->font_family);
}

static void
mtext_style_free_owned (Mtext_Style *s)
{
  if (s->font_family)
    {
      free (s->font_family);
      s->font_family = NULL;
    }
}

static void
mtext_buf_init (Mtext_Buf *b)
{
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

static void
mtext_buf_append (Mtext_Buf *b, const char *src, size_t n)
{
  if (b->len + n + 1 > b->cap)
    {
      size_t newcap = b->cap ? b->cap : 64;
      char *p;
      while (newcap < b->len + n + 1)
        newcap *= 2;
      /* OOM: keep the existing buffer and drop this append.  The caller
         continues with whatever was already accumulated; the worst-case
         result is truncated text, never a NULL deref. */
      p = (char *)realloc (b->data, newcap);
      if (!p)
        return;
      b->data = p;
      b->cap = newcap;
    }
  memcpy (b->data + b->len, src, n);
  b->len += n;
  b->data[b->len] = '\0';
}

static void
mtext_buf_append_char (Mtext_Buf *b, char c)
{
  mtext_buf_append (b, &c, 1);
}

/* Encode a Unicode codepoint as UTF-8 into `b`. */
static void
mtext_buf_append_codepoint (Mtext_Buf *b, unsigned int cp)
{
  if (cp < 0x80)
    {
      char c = (char)cp;
      mtext_buf_append (b, &c, 1);
    }
  else if (cp < 0x800)
    {
      char s[2];
      s[0] = (char)(0xC0 | (cp >> 6));
      s[1] = (char)(0x80 | (cp & 0x3F));
      mtext_buf_append (b, s, 2);
    }
  else if (cp < 0x10000)
    {
      char s[3];
      s[0] = (char)(0xE0 | (cp >> 12));
      s[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
      s[2] = (char)(0x80 | (cp & 0x3F));
      mtext_buf_append (b, s, 3);
    }
  else
    {
      char s[4];
      s[0] = (char)(0xF0 | (cp >> 18));
      s[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
      s[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
      s[3] = (char)(0x80 | (cp & 0x3F));
      mtext_buf_append (b, s, 4);
    }
}

/* Take ownership of the buffer's data, returning a NUL-terminated string.
   Resets the buffer to empty.  Always returns non-NULL. */
static char *
mtext_buf_steal (Mtext_Buf *b)
{
  char *out;
  if (!b->data)
    return strdup ("");
  out = b->data;
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
  return out;
}

static void
mtext_buf_reset (Mtext_Buf *b)
{
  if (b->data)
    free (b->data);
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

static void
mtext_runs_append (Mtext_Runs *rs, Mtext_Run r)
{
  if (rs->count == rs->cap)
    {
      size_t newcap = rs->cap ? rs->cap * 2 : 8;
      Mtext_Run *p = (Mtext_Run *)realloc (rs->runs,
                                           newcap * sizeof (Mtext_Run));
      if (!p)
        {
          /* OOM: free the run we just took ownership of (text/style/denom
             were transferred from the parser's working buffers) and
             continue with what's already collected. */
          free (r.text);
          mtext_style_free_owned (&r.style);
          if (r.denom)
            free (r.denom);
          return;
        }
      rs->runs = p;
      rs->cap = newcap;
    }
  rs->runs[rs->count++] = r;
}

static void
mtext_runs_free (Mtext_Runs *rs)
{
  size_t i;
  for (i = 0; i < rs->count; i++)
    {
      free (rs->runs[i].text);
      mtext_style_free_owned (&rs->runs[i].style);
      if (rs->runs[i].denom)
        free (rs->runs[i].denom);
    }
  if (rs->runs)
    free (rs->runs);
  rs->runs = NULL;
  rs->count = 0;
  rs->cap = 0;
}

/* Parser state shared between flush calls. */
typedef struct
{
  Mtext_Runs   runs;
  Mtext_Buf    buf;
  int          pending_newlines; /* incremented by \P; consumed by next flush */
} Mtext_Parser;

/* Emit a run only if `buf` has accumulated content.  `pending_newlines`
   transfers onto the run and resets to 0; if `buf` is empty the call is a
   no-op (so style-change handlers don't litter the SVG with empty tspans). */
static void
mtext_flush (Mtext_Parser *p, const Mtext_Style *cur)
{
  Mtext_Run r;
  if (p->buf.len == 0)
    return;
  r.text = mtext_buf_steal (&p->buf);
  r.newlines_before = p->pending_newlines;
  mtext_style_init (&r.style);
  mtext_style_copy (&r.style, cur);
  r.denom = NULL;
  mtext_runs_append (&p->runs, r);
  p->pending_newlines = 0;
}

/* Parse `src` into a list of styled runs.  `base_text_height` is used to
   normalise absolute \H<n>; values into a scale factor.  Returns an empty
   list on NULL input.  Caller frees with mtext_runs_free(). */
static Mtext_Runs
mtext_parse (const char *src, double base_text_height)
{
  Mtext_Parser p;
  Mtext_Style  cur;
  Mtext_Style  stack[MTEXT_GROUP_STACK_MAX];
  int          stack_depth = 0;
  size_t       i, len;

  p.runs.runs = NULL;
  p.runs.count = 0;
  p.runs.cap = 0;
  mtext_buf_init (&p.buf);
  p.pending_newlines = 0;
  mtext_style_init (&cur);

  if (!src)
    return p.runs;
  len = strlen (src);

  for (i = 0; i < len;)
    {
      char c = src[i];

      /* {} grouping */
      if (c == '{')
        {
          if (stack_depth < MTEXT_GROUP_STACK_MAX)
            mtext_style_copy (&stack[stack_depth++], &cur);
          i++;
          continue;
        }
      if (c == '}')
        {
          mtext_flush (&p, &cur);
          if (stack_depth > 0)
            {
              mtext_style_free_owned (&cur);
              cur = stack[--stack_depth];
            }
          i++;
          continue;
        }

      /* %%X codes */
      if (c == '%' && i + 2 < len && src[i + 1] == '%')
        {
          char m = src[i + 2];
          if (m == 'c' || m == 'C')
            {
              mtext_buf_append (&p.buf, "\xe2\x8c\x80", 3); /* U+2300 ⌀ */
              i += 3;
              continue;
            }
          if (m == 'd' || m == 'D')
            {
              mtext_buf_append (&p.buf, "\xc2\xb0", 2); /* U+00B0 ° */
              i += 3;
              continue;
            }
          if (m == 'p' || m == 'P')
            {
              mtext_buf_append (&p.buf, "\xc2\xb1", 2); /* U+00B1 ± */
              i += 3;
              continue;
            }
          if (m == 'u' || m == 'U')
            {
              mtext_flush (&p, &cur);
              cur.underline = !cur.underline;
              i += 3;
              continue;
            }
          if (m == 'o' || m == 'O')
            {
              mtext_flush (&p, &cur);
              cur.overline = !cur.overline;
              i += 3;
              continue;
            }
          /* Unknown %% sequence — fall through to literal '%' */
        }

      /* Backslash escapes */
      if (c == '\\' && i + 1 < len)
        {
          char n = src[i + 1];
          switch (n)
            {
            case 'P':
              mtext_flush (&p, &cur);
              p.pending_newlines++;
              i += 2;
              continue;
            case '~':
              mtext_buf_append (&p.buf, "\xc2\xa0", 2); /* U+00A0 NBSP */
              i += 2;
              continue;
            case '\\':
              mtext_buf_append_char (&p.buf, '\\');
              i += 2;
              continue;
            case '{':
            case '}':
              mtext_buf_append_char (&p.buf, n);
              i += 2;
              continue;
            case 'L':
            case 'l':
            case 'O':
            case 'o':
            case 'K':
            case 'k':
              mtext_flush (&p, &cur);
              switch (n)
                {
                case 'L': cur.underline = 1; break;
                case 'l': cur.underline = 0; break;
                case 'O': cur.overline = 1; break;
                case 'o': cur.overline = 0; break;
                case 'K': cur.strike = 1; break;
                case 'k': cur.strike = 0; break;
                default: break; /* unreachable */
                }
              i += 2;
              continue;
            case 'C':
              {
                /* \C<n>; */
                size_t j = i + 2;
                long val = 0;
                int has_digit = 0;
                while (j < len && src[j] >= '0' && src[j] <= '9')
                  {
                    val = val * 10 + (src[j] - '0');
                    has_digit = 1;
                    j++;
                  }
                if (j < len && src[j] == ';')
                  j++;
                mtext_flush (&p, &cur);
                if (has_digit)
                  {
                    if (val == 0 || val == 256)
                      cur.has_color = 0; /* ByBlock / ByLayer => use base */
                    else
                      {
                        cur.has_color = 1;
                        cur.aci_color_idx = (int)val;
                      }
                  }
                i = j;
                continue;
              }
            case 'H':
              {
                /* \H<n>; (absolute, in drawing units) or \H<n>x; (multiplier) */
                size_t j = i + 2;
                double val = 0.0;
                double frac = 0.0;
                double frac_scale = 1.0;
                int relative = 0;
                int has_digit = 0;
                int in_frac = 0;
                while (j < len)
                  {
                    char d = src[j];
                    if (d >= '0' && d <= '9')
                      {
                        if (in_frac)
                          {
                            frac_scale *= 0.1;
                            frac += (d - '0') * frac_scale;
                          }
                        else
                          val = val * 10.0 + (d - '0');
                        has_digit = 1;
                        j++;
                      }
                    else if (d == '.' && !in_frac)
                      {
                        in_frac = 1;
                        j++;
                      }
                    else
                      break;
                  }
                val += frac;
                if (j < len && src[j] == 'x')
                  {
                    relative = 1;
                    j++;
                  }
                if (j < len && src[j] == ';')
                  j++;
                mtext_flush (&p, &cur);
                if (has_digit && val > 0.0)
                  {
                    if (relative)
                      cur.height_scale = val;
                    else if (base_text_height > 0.0)
                      cur.height_scale = val / base_text_height;
                  }
                i = j;
                continue;
              }
            case 'f':
              {
                /* \f<face>|b<0|1>|i<0|1>|c<n>|p<n>; */
                size_t j = i + 2;
                size_t name_start = j;
                size_t name_end;
                while (j < len && src[j] != '|' && src[j] != ';')
                  j++;
                name_end = j;
                mtext_flush (&p, &cur);
                mtext_style_free_owned (&cur);
                cur.bold = 0;
                cur.italic = 0;
                if (name_end > name_start)
                  {
                    size_t nlen = name_end - name_start;
                    cur.font_family = (char *)malloc (nlen + 1);
                    memcpy (cur.font_family, src + name_start, nlen);
                    cur.font_family[nlen] = '\0';
                  }
                while (j < len && src[j] == '|')
                  {
                    char tag;
                    j++;
                    if (j >= len)
                      break;
                    tag = src[j];
                    j++;
                    if (tag == 'b' && j < len && src[j] >= '0' && src[j] <= '9')
                      {
                        cur.bold = (src[j] != '0');
                        j++;
                      }
                    else if (tag == 'i' && j < len && src[j] >= '0' && src[j] <= '9')
                      {
                        cur.italic = (src[j] != '0');
                        j++;
                      }
                    /* skip up to next '|' or ';' */
                    while (j < len && src[j] != '|' && src[j] != ';')
                      j++;
                  }
                if (j < len && src[j] == ';')
                  j++;
                i = j;
                continue;
              }
            case 'F':
              {
                /* \F<file>;  — font file, strip extension */
                size_t j = i + 2;
                size_t name_start = j;
                size_t name_end;
                size_t nlen;
                while (j < len && src[j] != ';')
                  j++;
                name_end = j;
                if (j < len)
                  j++;
                mtext_flush (&p, &cur);
                mtext_style_free_owned (&cur);
                cur.bold = 0;
                cur.italic = 0;
                nlen = name_end - name_start;
                if (nlen > 0)
                  {
                    size_t k;
                    for (k = nlen; k > 0; k--)
                      {
                        if (src[name_start + k - 1] == '.')
                          {
                            nlen = k - 1;
                            break;
                          }
                      }
                    if (nlen > 0)
                      {
                        cur.font_family = (char *)malloc (nlen + 1);
                        memcpy (cur.font_family, src + name_start, nlen);
                        cur.font_family[nlen] = '\0';
                      }
                  }
                i = j;
                continue;
              }
            case 'W':
            case 'Q':
            case 'T':
            case 'A':
            case 'p':
              {
                /* Consume + ignore — the corpus shows these have negligible
                   visual impact; revisit if a real test case demands it. */
                size_t j = i + 2;
                while (j < len && src[j] != ';')
                  j++;
                if (j < len)
                  j++;
                i = j;
                continue;
              }
            case 'S':
              {
                /* \S<num>^<den>;  or  \S<num>/<den>;
                   Stored as a single run with `denom` set; the renderer
                   currently emits this flat as "num/den" but the structured
                   data is preserved for a future stacked-tspan upgrade. */
                size_t j = i + 2;
                Mtext_Buf num_buf, den_buf;
                Mtext_Run sr;
                mtext_buf_init (&num_buf);
                mtext_buf_init (&den_buf);
                while (j < len && src[j] != '^' && src[j] != '/'
                       && src[j] != ';')
                  {
                    mtext_buf_append_char (&num_buf, src[j]);
                    j++;
                  }
                if (j < len && (src[j] == '^' || src[j] == '/'))
                  {
                    j++;
                    while (j < len && src[j] != ';')
                      {
                        mtext_buf_append_char (&den_buf, src[j]);
                        j++;
                      }
                  }
                if (j < len && src[j] == ';')
                  j++;
                mtext_flush (&p, &cur);
                sr.text = mtext_buf_steal (&num_buf);
                sr.denom = mtext_buf_steal (&den_buf);
                sr.newlines_before = p.pending_newlines;
                mtext_style_init (&sr.style);
                mtext_style_copy (&sr.style, &cur);
                mtext_runs_append (&p.runs, sr);
                p.pending_newlines = 0;
                mtext_buf_reset (&num_buf);
                mtext_buf_reset (&den_buf);
                i = j;
                continue;
              }
            case 'U':
              {
                /* \U+XXXX */
                if (i + 7 <= len && src[i + 2] == '+')
                  {
                    unsigned int cp = 0;
                    int valid = 1;
                    int k;
                    for (k = 0; k < 4; k++)
                      {
                        char d = src[i + 3 + k];
                        cp <<= 4;
                        if (d >= '0' && d <= '9')
                          cp |= (unsigned)(d - '0');
                        else if (d >= 'a' && d <= 'f')
                          cp |= (unsigned)(d - 'a' + 10);
                        else if (d >= 'A' && d <= 'F')
                          cp |= (unsigned)(d - 'A' + 10);
                        else
                          {
                            valid = 0;
                            break;
                          }
                      }
                    if (valid)
                      {
                        mtext_buf_append_codepoint (&p.buf, cp);
                        i += 7;
                        continue;
                      }
                  }
                /* Malformed — fall through to default */
                mtext_buf_append_char (&p.buf, n);
                i += 2;
                continue;
              }
            default:
              /* Unknown escape — drop the backslash, keep the next char.
                 Matches the legacy stripper's behaviour. */
              mtext_buf_append_char (&p.buf, n);
              i += 2;
              continue;
            }
        }

      if (c == '\t')
        {
          /* Tabs aren't reliable in SVG.  Substitute four spaces — matches
             the visual width that engineering-note MTEXTs are built around
             (e.g. "SC3-1 & 2:[TAB]CT CABLING…"). */
          mtext_buf_append (&p.buf, "    ", 4);
          i++;
          continue;
        }
      mtext_buf_append_char (&p.buf, c);
      i++;
    }

  mtext_flush (&p, &cur);
  mtext_buf_reset (&p.buf);
  mtext_style_free_owned (&cur);
  while (stack_depth > 0)
    mtext_style_free_owned (&stack[--stack_depth]);

  return p.runs;
}

/* Word-wrap pass.  Walks `in` and splits any run whose text would push the
   visual line width past `rect_width` (in drawing units), inserting wrap-
   induced line breaks (newlines_before = 1) at the most recent whitespace.

   `cwf` is a char-width factor — proportional fonts ≈ 0.55, monospace ≈ 0.6.
   Width estimation is rough (we don't run the real font metrics), so the
   factor errs on the early-wrap side rather than the overflow side.

   Stacked-fraction runs (`denom != NULL`) are treated atomically.

   Ownership: takes `in` by value, frees it before returning, and returns a
   fresh `Mtext_Runs`.  Caller frees the returned value with mtext_runs_free.
   If `rect_width <= 0` the input is returned untouched (no copy). */
static Mtext_Runs
mtext_runs_wrap (Mtext_Runs in, double font_size, double rect_width,
                 double cwf)
{
  Mtext_Runs out = { NULL, 0, 0 };
  size_t i;
  double cur_line_width = 0.0;

  if (rect_width <= 0.0 || font_size <= 0.0 || cwf <= 0.0)
    return in;

  for (i = 0; i < in.count; i++)
    {
      Mtext_Run *src = &in.runs[i];
      double hscale = src->style.height_scale > 0.0
                          ? src->style.height_scale
                          : 1.0;
      double cw = font_size * hscale * cwf;
      const char *text = src->text;
      size_t pos = 0;
      size_t tlen;
      int first_emit = 1;
      int pending_nl = src->newlines_before;

      if (src->newlines_before > 0)
        cur_line_width = 0.0;

      /* Stacked fractions and empty-text runs are emitted atomically. */
      if (src->denom || !text || *text == '\0')
        {
          Mtext_Run nr;
          nr.text = strdup (text ? text : "");
          nr.newlines_before = pending_nl;
          nr.denom = src->denom ? strdup (src->denom) : NULL;
          mtext_style_init (&nr.style);
          mtext_style_copy (&nr.style, &src->style);
          mtext_runs_append (&out, nr);
          if (text)
            cur_line_width += strlen (text) * cw;
          continue;
        }

      tlen = strlen (text);

      while (pos < tlen)
        {
          double avail = rect_width - cur_line_width;
          size_t max_chars = (avail > 0.0 && cw > 0.0)
                                 ? (size_t)(avail / cw)
                                 : 0;
          size_t end = pos + max_chars;
          size_t break_at;
          size_t rlen;
          Mtext_Run nr;

          if (end >= tlen)
            {
              /* Rest of the run fits on the current line. */
              rlen = tlen - pos;
              nr.text = (char *)malloc (rlen + 1);
              memcpy (nr.text, text + pos, rlen);
              nr.text[rlen] = '\0';
              nr.newlines_before = first_emit ? pending_nl : 1;
              nr.denom = NULL;
              mtext_style_init (&nr.style);
              mtext_style_copy (&nr.style, &src->style);
              mtext_runs_append (&out, nr);
              cur_line_width += rlen * cw;
              break;
            }

          /* Need a wrap point.  Prefer the last whitespace at or before
             `end`; if there isn't one within the current run, hard-break
             at `end` (or pos+1 to guarantee progress). */
          break_at = end;
          while (break_at > pos && text[break_at - 1] != ' ')
            break_at--;
          if (break_at <= pos)
            {
              break_at = (max_chars > 0) ? end : pos + 1;
              if (break_at > tlen)
                break_at = tlen;
              if (break_at == pos)
                break_at = pos + 1;
            }

          rlen = break_at - pos;
          while (rlen > 0 && text[pos + rlen - 1] == ' ')
            rlen--;

          nr.text = (char *)malloc (rlen + 1);
          if (rlen > 0)
            memcpy (nr.text, text + pos, rlen);
          nr.text[rlen] = '\0';
          nr.newlines_before = first_emit ? pending_nl : 1;
          nr.denom = NULL;
          mtext_style_init (&nr.style);
          mtext_style_copy (&nr.style, &src->style);
          mtext_runs_append (&out, nr);

          /* Skip leading whitespace for the wrapped continuation. */
          pos = break_at;
          while (pos < tlen && text[pos] == ' ')
            pos++;
          cur_line_width = 0.0;
          first_emit = 0;
        }
    }

  mtext_runs_free (&in);
  return out;
}

/* True when the run carries no style overrides relative to the base text
   element — used to decide whether to take the legacy single-element fast
   path (which produces byte-equal output to the old renderer). */
static int
mtext_run_is_plain (const Mtext_Run *r)
{
  if (r->newlines_before > 0 || r->denom)
    return 0;
  if (r->style.underline || r->style.overline || r->style.strike)
    return 0;
  if (r->style.bold || r->style.italic || r->style.font_family)
    return 0;
  if (r->style.has_color)
    return 0;
  if (r->style.height_scale != 1.0)
    return 0;
  return 1;
}

static void
output_MTEXT (Dwg_Object *obj)
{
  Dwg_Data *dwg = obj->parent;
  Dwg_Entity_MTEXT *mtext = obj->tio.entity->tio.MTEXT;
  Mtext_Runs runs;
  const char *fontfamily;
  double cap_height_ratio;
  double font_size;
  double line_dy;        /* em units; sign flipped inside block defs */
  BITCODE_H style_ref;
  Dwg_Object *o;
  Dwg_Object_STYLE *style;
  BITCODE_2DPOINT pt_in, pt;
  double rotation_rad, rotation_deg;
  int horiz, vert;
  double tx, ty;
  char *base_color;
  int base_aci;

  if (!mtext->text || entity_invisible (obj))
    return;
  if (isnan_3BD (mtext->ins_pt) || isnan_3BD (mtext->extrusion))
    return;

  style_ref = mtext->style;
  o = style_ref ? dwg_ref_object_silent (dwg, style_ref) : NULL;
  style = o ? o->tio.object->tio.STYLE : NULL;
  get_font_info (style, o, &fontfamily, &cap_height_ratio);

  font_size = mtext->text_height / cap_height_ratio;

  pt_in.x = mtext->ins_pt.x;
  pt_in.y = mtext->ins_pt.y;
  transform_OCS_2d (&pt, pt_in, mtext->extrusion);

  /* MTEXT rotation comes from x_axis_dir, not an explicit angle. */
  if (!isnan (mtext->x_axis_dir.x) && !isnan (mtext->x_axis_dir.y)
      && (mtext->x_axis_dir.x != 0.0 || mtext->x_axis_dir.y != 0.0))
    rotation_rad = atan2 (mtext->x_axis_dir.y, mtext->x_axis_dir.x);
  else
    rotation_rad = 0.0;
  rotation_deg = rotation_rad * 180.0 / M_PI;

  /* Attachment is a 1-9 grid: TL, TC, TR, ML, MC, MR, BL, BC, BR. */
  switch (mtext->attachment)
    {
    case 2: case 5: case 8: horiz = 1; break; /* centre */
    case 3: case 6: case 9: horiz = 2; break; /* right */
    default:                horiz = 0; break; /* left (1,4,7) */
    }
  switch (mtext->attachment)
    {
    case 1: case 2: case 3: vert = 3; break; /* top */
    case 4: case 5: case 6: vert = 2; break; /* middle */
    default:                vert = 0; break; /* baseline (7,8,9) */
    }

  /* Line-spacing factor (default 1.0) × the SVG 1.2em convention. */
  {
    double lsf = mtext->linespace_factor;
    if (lsf <= 0.0)
      lsf = 1.0;
    line_dy = 1.2 * lsf;
    /* In block definitions the parent transform="scale(1 -1)" flips the
       y-axis, so dy advances upward in display space.  Negate to keep
       successive lines visually below each other. */
    if (in_block_definition)
      line_dy = -line_dy;
  }

  runs = mtext_parse (mtext->text, mtext->text_height);
  /* Word-wrap at the MTEXT box width.  Engineering-drawing MTEXT is usually
     ALL CAPS in Arial-family fonts where the average glyph occupies ~0.6 of
     the em-square; 0.65 for monospace.  Without real font metrics we err on
     the early-wrap side so text never overflows the box. */
  {
    int monospace = (fontfamily && (strcmp (fontfamily, "Courier") == 0
                                    || strcmp (fontfamily, "Lucida Console")
                                           == 0));
    double cwf = monospace ? 0.65 : 0.6;
    runs = mtext_runs_wrap (runs, font_size, mtext->rect_width, cwf);
  }
  base_color = entity_color (obj);
  base_aci = entity_aci_index (obj);

  tx = transform_X (pt.x);
  ty = transform_Y (pt.y);

  /* Fast path: a single run with no style overrides produces byte-equal
     output to the legacy renderer.  Most simple labels (e.g. NEWCL.dwg's
     "CL") and ByLayer-only J607 notes hit this path. */
  if (runs.count <= 1
      && (runs.count == 0 || mtext_run_is_plain (&runs.runs[0])))
    {
      const char *body = runs.count == 1 ? runs.runs[0].text : "";
      /* Parser output is UTF-8 — escape XML metachars only, leave high
         bytes untouched.  htmlescape would mis-interpret each byte through
         the document codepage. */
      char *escaped = mtext_escape_utf8 (body);
      output_text_element (obj, tx, ty, fontfamily, font_size, base_color,
                           get_text_anchor (horiz),
                           get_dominant_baseline (vert),
                           rotation_deg, 1.0, escaped, base_aci);
      if (escaped)
        free (escaped);
    }
  else
    {
      double render_y = in_block_definition ? -ty : ty;
      const char *anchor = get_text_anchor (horiz);
      const char *baseline = get_dominant_baseline (vert);
      int has_rotation = fabs (rotation_deg) > 0.001;
      size_t i;

      printf ("\t<text id=\"dwg-object-%d\" x=\"%f\" y=\"%f\" "
              "font-family=\"%s\" font-size=\"%f\" fill=\"%s\" "
              "text-anchor=\"%s\" dominant-baseline=\"%s\" "
              "data-aci=\"%d\" xml:space=\"preserve\"",
              obj->index, tx, render_y, fontfamily, font_size, base_color,
              anchor, baseline, base_aci);

      if (has_rotation && in_block_definition)
        {
          double rot_rad = rotation_deg * M_PI / 180.0;
          double c = cos (rot_rad);
          double s = sin (rot_rad);
          printf (" transform=\"matrix(%f %f %f %f %f %f)\"",
                  c, s, s, -c,
                  tx * (1 - c) + s * ty,
                  ty * (1 - c) - s * tx);
        }
      else if (has_rotation)
        printf (" transform=\"rotate(%f %f %f)\"",
                -rotation_deg, tx, render_y);
      else if (in_block_definition)
        printf (" transform=\"scale(1 -1)\"");

      printf (">");

      for (i = 0; i < runs.count; i++)
        {
          const Mtext_Run *r = &runs.runs[i];
          char *escaped;
          int k;

          /* For each \P beyond the one that opens this run's line, emit a
             placeholder tspan containing &#160; (NBSP) so the browser
             actually advances dy — empty tspans don't reliably do so. */
          for (k = 1; k < r->newlines_before; k++)
            printf ("<tspan x=\"%f\" dy=\"%fem\">&#160;</tspan>",
                    tx, line_dy);

          printf ("<tspan");
          if (r->newlines_before > 0)
            printf (" x=\"%f\" dy=\"%fem\"", tx, line_dy);
          if (r->style.font_family)
            {
              /* The font name comes from the MTEXT \f<face>|...; code in
                 the source DWG and is therefore caller-controlled.  Run
                 it through the XML escape so a malicious face name like
                 `Arial" onload="evil()` can't break out of the attribute
                 and inject hostile attributes into the SVG. */
              char *escaped_ff = mtext_escape_utf8 (r->style.font_family);
              if (escaped_ff)
                {
                  printf (" font-family=\"%s\"", escaped_ff);
                  free (escaped_ff);
                }
            }
          if (r->style.bold)
            printf (" font-weight=\"bold\"");
          if (r->style.italic)
            printf (" font-style=\"italic\"");
          if (r->style.height_scale > 0.0
              && r->style.height_scale != 1.0)
            printf (" font-size=\"%fem\"", r->style.height_scale);
          {
            int has_under = r->style.underline;
            int has_over = r->style.overline;
            int has_strike = r->style.strike;
            if (has_under || has_over || has_strike)
              {
                printf (" text-decoration=\"");
                if (has_under)
                  printf ("underline");
                if (has_over)
                  printf ("%soverline", has_under ? " " : "");
                if (has_strike)
                  printf ("%sline-through",
                          (has_under || has_over) ? " " : "");
                printf ("\"");
              }
          }
          if (r->style.has_color)
            {
              char *cc = aci_color ((unsigned int)r->style.aci_color_idx);
              printf (" fill=\"%s\" data-aci=\"%d\"", cc,
                      r->style.aci_color_idx);
              if (cc && *cc == '#')
                free (cc);
            }
          printf (">");

          if (r->denom)
            {
              /* Stacked fraction — flat "num/den" for now (matches the
                 legacy renderer's output). */
              size_t need = strlen (r->text) + 1 + strlen (r->denom) + 1;
              char *combined = (char *)malloc (need);
              snprintf (combined, need, "%s/%s", r->text, r->denom);
              escaped = mtext_escape_utf8 (combined);
              free (combined);
            }
          else
            {
              escaped = mtext_escape_utf8 (r->text);
            }
          if (escaped)
            {
              printf ("%s", escaped);
              free (escaped);
            }

          printf ("</tspan>");
        }

      printf ("</text>\n");
    }

  if (base_color && *base_color == '#')
    free (base_color);
  mtext_runs_free (&runs);
}

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

  {
    char *color = entity_color (obj);
    output_text_element (obj, transform_X (pt.x), transform_Y (pt.y),
                         fontfamily, text->height / cap_height_ratio,
                         color,
                         get_text_anchor (text->horiz_alignment),
                         get_dominant_baseline (text->vert_alignment),
                         text->rotation * 180.0 / M_PI, wf, escaped,
                         entity_aci_index (obj));
    if (*color == '#')
      free (color);
  }

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

  {
    char *color = entity_color (obj);
    output_text_element (obj, transform_X (pt.x), transform_Y (pt.y),
                         fontfamily, attdef->height / cap_height_ratio,
                         color,
                         get_text_anchor (attdef->horiz_alignment),
                         get_dominant_baseline (attdef->vert_alignment),
                         rotation_deg, wf, escaped,
                         entity_aci_index (obj));
    if (*color == '#')
      free (color);
  }

  if (escaped)
    free (escaped);
}

static void
output_ATTRIB (Dwg_Object *obj)
{
  Dwg_Data *dwg = obj->parent;
  Dwg_Entity_ATTRIB *attrib = obj->tio.entity->tio.ATTRIB;
  char *escaped;
  const char *fontfamily;
  double cap_height_ratio;
  BITCODE_H style_ref = attrib->style;
  Dwg_Object *o = style_ref ? dwg_ref_object_silent (dwg, style_ref) : NULL;
  Dwg_Object_STYLE *style = o ? o->tio.object->tio.STYLE : NULL;
  BITCODE_2DPOINT pt;
  double rotation_deg, wf;

  if (!attrib->text_value || entity_invisible (obj))
    return;
  /* flags bit 0 = invisible attribute */
  if (attrib->flags & 1)
    return;
  if (isnan_2BD (attrib->ins_pt) || isnan_3BD (attrib->extrusion))
    return;
  if (dwg->header.version >= R_2007)
    escaped = htmlwescape ((BITCODE_TU)attrib->text_value);
  else
    escaped = htmlescape (attrib->text_value, dwg->header.codepage);

  get_font_info (style, o, &fontfamily, &cap_height_ratio);

  if (attrib->horiz_alignment != 0 || attrib->vert_alignment != 0)
    transform_OCS_2d (&pt, attrib->alignment_pt, attrib->extrusion);
  else
    transform_OCS_2d (&pt, attrib->ins_pt, attrib->extrusion);
  rotation_deg = attrib->rotation * 180.0 / M_PI;

  wf = attrib->width_factor;
  if (wf == 0.0 && style)
    wf = style->width_factor;
  if (wf == 0.0)
    wf = 1.0;

  {
    char *color = entity_color (obj);
    output_text_element (obj, transform_X (pt.x), transform_Y (pt.y),
                         fontfamily, attrib->height / cap_height_ratio,
                         color,
                         get_text_anchor (attrib->horiz_alignment),
                         get_dominant_baseline (attrib->vert_alignment),
                         rotation_deg, wf, escaped,
                         entity_aci_index (obj));
    if (*color == '#')
      free (color);
  }

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
      BITCODE_2DPOINT pt, prev_pt, ptin;
      dwg_point_2d *pts = dwg_ent_lwpline_get_points (pline, &error);
      BITCODE_BD *bulges = NULL;
      BITCODE_BL num_bulges = 0;
      BITCODE_RL j;

      if (error || isnan_2pt (pts[0]) || isnan_3BD (pline->extrusion))
        return;

      if (pline->num_bulges > 0 && pline->bulges)
        {
          bulges = pline->bulges;
          num_bulges = pline->num_bulges;
        }

      ptin.x = pts[0].x;
      ptin.y = pts[0].y;
      transform_OCS_2d (&prev_pt, ptin, pline->extrusion);
      printf ("\t<!-- lwpolyline-%d -->\n", obj->index);
      printf ("\t<path id=\"dwg-object-%d\" d=\"M %f,%f", obj->index,
              transform_X (prev_pt.x), transform_Y (prev_pt.y));
      for (j = 1; j < numpts; j++)
        {
          ptin.x = pts[j].x;
          ptin.y = pts[j].y;
          if (isnan_2BD (ptin))
            continue;
          transform_OCS_2d (&pt, ptin, pline->extrusion);
          if (bulges && (j - 1) < num_bulges
              && fabs (bulges[j - 1]) > 1e-6)
            output_bulge_arc (prev_pt.x, prev_pt.y, pt.x, pt.y,
                              bulges[j - 1]);
          else
            printf (" L %f,%f", transform_X (pt.x), transform_Y (pt.y));
          prev_pt = pt;
        }
      if (pline->flag & 512) // closed
        {
          /* Close with an arc if the last vertex has a bulge */
          ptin.x = pts[0].x;
          ptin.y = pts[0].y;
          transform_OCS_2d (&pt, ptin, pline->extrusion);
          if (bulges && (numpts - 1) < num_bulges
              && fabs (bulges[numpts - 1]) > 1e-6)
            output_bulge_arc (prev_pt.x, prev_pt.y, pt.x, pt.y,
                              bulges[numpts - 1]);
          else
            printf (" Z");
        }
      printf ("\"\n\t");

      /* Use polyline width for stroke-width when available.
         const_width > 0 means uniform width; otherwise check per-vertex
         widths and use the maximum. */
      {
        double pw = pline->const_width;
        if (pw <= 0.0 && pline->num_widths > 0 && pline->widths)
          {
            BITCODE_BL wi;
            for (wi = 0; wi < pline->num_widths; wi++)
              {
                if (pline->widths[wi].start > pw)
                  pw = pline->widths[wi].start;
                if (pline->widths[wi].end > pw)
                  pw = pline->widths[wi].end;
              }
          }
        if (pw > 0.0)
          {
            char *color = entity_color (obj);
            char *dashes = entity_dasharray (obj);
            int aci = entity_aci_index (obj);
            /* A polyline const_width / vertex width is a deliberate
               geometric thickness in drawing units set by the drawing
               author (e.g. 0.97mm).  Unlike the small fraction-of-a-mm
               lineweight indices, these values are already at a visible
               scale so they're used directly — no display boost.  No
               non-scaling-stroke either: the width is geometry and should
               inflate with any enclosing block transform, matching
               AutoCAD's rendering. */
            double lweight = pw;
            if (lweight < DEFAULT_STROKE_WIDTH_PX)
              lweight = DEFAULT_STROKE_WIDTH_PX;
            if (dashes)
              printf ("      data-aci=\"%d\""
                      " style=\"fill:none;stroke:%s;stroke-width:%.2fpx;"
                      "stroke-dasharray:%s;stroke-linecap:round\" />\n",
                      aci, color, lweight, dashes);
            else
              printf ("      data-aci=\"%d\""
                      " style=\"fill:none;stroke:%s;stroke-width:%.2fpx\" />\n",
                      aci, color, lweight);
            if (*color == '#')
              free (color);
            free (dashes);
          }
        else
          common_entity (obj);
      }
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
  /* Positive bulge = CCW in DWG (Y-up).  The sweep-flag must be chosen
     relative to the path's local coordinate system:
       - in_block_definition == 1: coords are raw DWG, parent <g> applies
         a Y-flip via matrix(…,-s,…) which reflects the rendering space —
         that reflection inverts the visual sweep direction, so we emit the
         sweep value that's "opposite" to what the final appearance needs.
       - in_block_definition == 0: transform_Y already Y-flipped the
         coordinate values (no reflection in the render matrix), so the
         sweep-flag is interpreted directly against the final visual. */
  int sweep = in_block_definition ? (bulge > 0 ? 1 : 0)
                                   : (bulge > 0 ? 0 : 1);
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
  {
    int aci = entity_aci_index (obj);

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
        printf ("\"\n\t      data-aci=\"%d\""
                " style=\"fill:%s;stroke:none;fill-rule:evenodd\" />\n",
                aci, fill_color);
      }
    else
      {
        const char *ve_attr
            = entity_lweight_is_explicit (obj->tio.entity)
                  ? ""
                  : " vector-effect=\"non-scaling-stroke\"";
        for (i = 0; i < hatch->num_paths; i++)
          {
            printf ("\t<path id=\"dwg-object-%d-path-%d\" d=\"", obj->index, i);
            output_hatch_path_data (&hatch->paths[i]);
            printf ("\"\n\t      data-aci=\"%d\"%s"
                    " style=\"fill:none;stroke:%s;stroke-width:%.1fpx\" />\n",
                    aci, ve_attr, fill_color, lweight);
          }
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
        if (in_block_definition)
          {
            /* Raw DWG coords — the parent transform (viewport matrix or
               enclosing INSERT's <use>) handles the final mapping. */
            tx = ins_pt.x - sx * base_x;
            ty = ins_pt.y - sy * base_y;
          }
        else
          {
            tx = ins_pt.x - sx * base_x - model_xmin;
            ty = page_height - ins_pt.y + sy * base_y + model_ymin;
          }
      }

      {
        /* In block definitions / viewports the parent transform provides the
           Y-flip, so the INSERT itself must NOT negate sy.  In top-level
           rendering there is no parent flip, so INSERT does it. */
        double y_scale = in_block_definition ? insert->scale.y
                                             : -insert->scale.y;
        Dwg_Data *dwg = obj->parent;

        /* Effective layer of THIS INSERT — used to pick the matching
           block clone in <defs>.  Own layer wins unless it's "0" and a
           parent effective layer is in scope (nested INSERT inheritance). */
        char *eff_layer
            = insert_effective_layer (obj, dwg, current_eff_layer);
        char *eff_safe = eff_layer ? layer_safe_id (eff_layer) : NULL;
        const char *suffix = eff_safe ? eff_safe : "_0";

        printf ("\t<!-- insert-%d -->\n", obj->index);
        if (fabs (insert->rotation) < 0.0001)
          {
            printf ("\t<use id=\"dwg-object-%d\" transform=\"matrix(%f 0 0 %f %f %f)\" "
                    "xlink:href=\"#symbol-" FORMAT_HV "-%s\" />"
                    "<!-- block_header->handleref: " FORMAT_H " -->\n",
                    obj->index, insert->scale.x, y_scale, tx, ty,
                    insert->block_header->absolute_ref, suffix,
                    ARGS_H (insert->block_header->handleref));
          }
        else
          {
            printf ("\t<use id=\"dwg-object-%d\" transform=\"translate(%f %f) "
                    "rotate(%f) scale(%f %f)\" xlink:href=\"#symbol-" FORMAT_HV
                    "-%s\" />"
                    "<!-- block_header->handleref: " FORMAT_H " -->\n",
                    obj->index, tx, ty,
                    rotation_deg, insert->scale.x, y_scale,
                    insert->block_header->absolute_ref, suffix,
                    ARGS_H (insert->block_header->handleref));
          }
        free (eff_layer);
        free (eff_safe);
      }
    }
  else
    {
      printf ("\n\n<!-- WRONG INSERT(" FORMAT_H ") -->\n",
              ARGS_H (obj->handle));
    }

  /* Attached entities (ATTRIBs etc.) are NOT emitted here.  They carry
     their own layer assignment distinct from the INSERT's, so emitting
     them inline would collapse them into the parent INSERT's layer group.
     The caller (output_BLOCK_HEADER's per-layer pass, or
     output_INSERT_attribs for the unnamed-block fallback) is responsible
     for emitting them in the correct layer group. */
}

/* Emit ATTRIBs (and other attached entities like LWPOLYLINE revision
   clouds) of an INSERT.  These have entmode=2 and are not part of the
   block's entity chain, so get_next_owned_entity skips them.

   Layer-aware callers should iterate insert->attribs themselves and emit
   only those whose layer matches the current group.  This helper exists
   for the unnamed-block fallback path that does not group by layer. */
static void
output_INSERT_attribs (Dwg_Object *obj)
{
  Dwg_Entity_INSERT *insert = obj->tio.entity->tio.INSERT;
  Dwg_Data *dwg;
  BITCODE_BL i;
  if (!insert->has_attribs || insert->num_owned == 0 || !insert->attribs)
    return;
  dwg = obj->parent;
  for (i = 0; i < insert->num_owned; i++)
    {
      Dwg_Object *aobj = dwg_ref_object_silent (dwg, insert->attribs[i]);
      if (!aobj)
        continue;
      if (aobj->fixedtype == DWG_TYPE_SEQEND)
        continue;
      output_object (aobj);
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
      /* ATTDEFs are attribute templates inside block definitions.
         When the block is INSERTed, ATTRIBs replace them with actual values.
         Skip ATTDEFs in block definitions to avoid overlapping text. */
      if (!in_block_definition)
        output_ATTDEF (obj);
      break;
    case DWG_TYPE_ATTRIB:
      output_ATTRIB (obj);
      break;
    case DWG_TYPE_MTEXT:
      output_MTEXT (obj);
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

/* ── Layer handle→name lookup table ─────────────────────────────────────────
   Built once per SVG generation from the LAYER_CONTROL table.  Provides a
   reliable fallback when ent->layer->obj is NULL and dwg_ref_object_silent
   also fails — common in certain DWG encodings.  Same data source that
   dwg_get_layers() uses, so it works even when entity handle refs don't. */

typedef struct
{
  BITCODE_RLL handle_value; /* LAYER object's handle.value */
  char *name;               /* HTML-escaped layer name (heap) */
} LayerHandleEntry;

static _Thread_local LayerHandleEntry *g_layer_htbl = NULL;
static _Thread_local unsigned int g_layer_htbl_n = 0;

static void
build_layer_handle_table (Dwg_Data *dwg)
{
  unsigned int i;
  Dwg_Object *ctrl;
  Dwg_Object_LAYER_CONTROL *_ctrl;
  unsigned int num_layers;

  /* Free previous table if any */
  for (i = 0; i < g_layer_htbl_n; i++)
    free (g_layer_htbl[i].name);
  free (g_layer_htbl);
  g_layer_htbl = NULL;
  g_layer_htbl_n = 0;

  ctrl = dwg_get_first_object (dwg, DWG_TYPE_LAYER_CONTROL);
  if (!ctrl || !ctrl->tio.object || !ctrl->tio.object->tio.LAYER_CONTROL)
    return;
  _ctrl = ctrl->tio.object->tio.LAYER_CONTROL;
  num_layers = _ctrl->num_entries;
  if (!num_layers)
    return;

  g_layer_htbl
      = (LayerHandleEntry *)calloc (num_layers, sizeof (LayerHandleEntry));
  if (!g_layer_htbl)
    return;

  for (i = 0; i < num_layers; i++)
    {
      Dwg_Object *obj = dwg_ref_object (dwg, _ctrl->entries[i]);
      if (obj && obj->fixedtype == DWG_TYPE_LAYER)
        {
          Dwg_Object_LAYER *layer = obj->tio.object->tio.LAYER;
          char *escaped = NULL;
          if (layer->name)
            {
              if (dwg->header.version >= R_2007)
                escaped = htmlwescape ((BITCODE_TU)layer->name);
              else
                escaped = htmlescape (layer->name, dwg->header.codepage);
            }
          g_layer_htbl[g_layer_htbl_n].handle_value = obj->handle.value;
          g_layer_htbl[g_layer_htbl_n].name
              = escaped ? escaped : strdup ("0");
          g_layer_htbl_n++;
        }
    }
}

static void
free_layer_handle_table (void)
{
  unsigned int i;
  for (i = 0; i < g_layer_htbl_n; i++)
    free (g_layer_htbl[i].name);
  free (g_layer_htbl);
  g_layer_htbl = NULL;
  g_layer_htbl_n = 0;
}

/* Look up layer name by absolute_ref in the handle table.
   Returns a heap-allocated copy, or NULL if not found. */
static char *
layer_name_from_handle_table (BITCODE_RLL abs_ref)
{
  unsigned int i;
  for (i = 0; i < g_layer_htbl_n; i++)
    {
      if (g_layer_htbl[i].handle_value == abs_ref)
        return strdup (g_layer_htbl[i].name);
    }
  return NULL;
}

/* Layer grouping helpers */
typedef struct
{
  char *attr_name;     /* HTML-escaped name — identity AND data-layer attribute */
  char *safe_id;       /* sanitized for use as XML id suffix */
} LayerEntry;

static char *
layer_safe_id (const char *attr_name)
{
  /* Build a version of the layer name safe for use in an XML id attribute.
     Must start with a letter or underscore; only letters, digits, hyphens,
     underscores and dots are kept; everything else becomes an underscore. */
  size_t len = strlen (attr_name) + 2;
  char *safe = (char *)malloc (len);
  char *out = safe;
  const char *in = attr_name;

  if (!isalpha ((unsigned char)*in) && *in != '_')
    *out++ = '_';
  while (*in)
    {
      char c = *in++;
      *out++ = (isalnum ((unsigned char)c) || c == '-' || c == '_' || c == '.') ? c : '_';
    }
  *out = '\0';
  return safe;
}

static char *
layer_get_attr_name (Dwg_Object *layer_obj, Dwg_Data *dwg)
{
  /* Returns a heap-allocated, HTML-escaped UTF-8 layer name.
     Caller must free. Returns strdup("0") for missing/invalid layers. */
  Dwg_Object_LAYER *layer;
  char *escaped;
  if (!layer_obj || layer_obj->fixedtype != DWG_TYPE_LAYER)
    return strdup ("0");
  layer = layer_obj->tio.object->tio.LAYER;
  if (!layer->name)
    return strdup ("0");
  if (dwg->header.version >= R_2007)
    escaped = htmlwescape ((BITCODE_TU)layer->name);
  else
    escaped = htmlescape (layer->name, dwg->header.codepage);
  return escaped ? escaped : strdup ("0");
}

/* Returns heap-allocated HTML-escaped layer name for an entity.
   Resolution order:
     1. ent->layer->obj (pre-resolved pointer)
     2. dwg_ref_object_silent (handle-based lookup)
     3. g_layer_htbl (brute-force handle table built from LAYER_CONTROL)
   Caller must free. */
static char *
entity_layer_name (Dwg_Object *obj, Dwg_Data *dwg)
{
  Dwg_Object_Entity *ent = obj->tio.entity;
  Dwg_Object *lobj = NULL;

  if (ent->layer)
    {
      if (ent->layer->obj && ent->layer->obj->fixedtype == DWG_TYPE_LAYER)
        lobj = ent->layer->obj;
      else
        {
          /* Handle not pre-resolved — force lookup now */
          Dwg_Object *resolved = dwg_ref_object_silent (dwg, ent->layer);
          if (resolved && resolved->fixedtype == DWG_TYPE_LAYER)
            lobj = resolved;
        }

      /* Third fallback: match absolute_ref against the pre-built handle table.
         This succeeds even when the entity's handle encoding prevents normal
         resolution — the LAYER_CONTROL entries always resolve correctly. */
      if (!lobj && ent->layer->absolute_ref)
        {
          char *name = layer_name_from_handle_table (ent->layer->absolute_ref);
          if (name)
            return name;
        }
    }
  return layer_get_attr_name (lobj, dwg);
}

/* ── Block-clone state for layer-0-in-block inheritance ───────────────────
   AutoCAD's "layer 0 inside a block inherits the INSERT's layer" rule means
   the same block, referenced from INSERTs on different layers, must produce
   different visibility groupings.  We implement that by emitting a separate
   <g id="symbol-{handle}-{eff_layer_safe}"> per distinct effective layer
   the block is referenced from, with each clone's layer-0 entities tagged
   with the effective layer instead of the literal "0". */

typedef struct
{
  BITCODE_RLL block_handle_value; /* BLOCK_HEADER ref->absolute_ref */
  char *eff_layer;                /* HTML-escaped effective layer name (heap) */
  char *eff_layer_safe_id;        /* sanitised for use as XML id suffix (heap) */
} BlockCombo;

static _Thread_local BlockCombo *g_block_combos = NULL;
static _Thread_local unsigned int g_block_combos_n = 0;
static _Thread_local unsigned int g_block_combos_cap = 0;

/* current_eff_layer / current_eff_layer_safe are defined at file scope
   near the forward declarations so output_INSERT can read them — see
   the top of this file. */

static void
free_block_combos (void)
{
  unsigned int i;
  for (i = 0; i < g_block_combos_n; i++)
    {
      free (g_block_combos[i].eff_layer);
      free (g_block_combos[i].eff_layer_safe_id);
    }
  free (g_block_combos);
  g_block_combos = NULL;
  g_block_combos_n = 0;
  g_block_combos_cap = 0;
}

/* Add a (handle, eff_layer) combo if not already present.  Takes ownership
   of eff_layer_dup and eff_layer_safe_dup on insertion; the caller must
   free them otherwise. */
static int
add_block_combo (BITCODE_RLL handle, const char *eff_layer)
{
  unsigned int i;
  char *dup_layer;
  char *dup_safe;
  for (i = 0; i < g_block_combos_n; i++)
    {
      if (g_block_combos[i].block_handle_value == handle
          && strcmp (g_block_combos[i].eff_layer, eff_layer) == 0)
        return 0; /* already present */
    }
  if (g_block_combos_n >= g_block_combos_cap)
    {
      unsigned int new_cap = g_block_combos_cap == 0
                                 ? 16
                                 : g_block_combos_cap * 2;
      BlockCombo *grow = (BlockCombo *)realloc (
          g_block_combos, (size_t)new_cap * sizeof (BlockCombo));
      if (!grow)
        return 0;
      g_block_combos = grow;
      g_block_combos_cap = new_cap;
    }
  dup_layer = strdup (eff_layer);
  dup_safe = layer_safe_id (eff_layer);
  if (!dup_layer || !dup_safe)
    {
      free (dup_layer);
      free (dup_safe);
      return 0;
    }
  g_block_combos[g_block_combos_n].block_handle_value = handle;
  g_block_combos[g_block_combos_n].eff_layer = dup_layer;
  g_block_combos[g_block_combos_n].eff_layer_safe_id = dup_safe;
  g_block_combos_n++;
  return 1;
}

/* Find the BLOCK_HEADER ref in dwg->block_control.entries whose
   absolute_ref matches the given handle value.  Returns NULL on miss. */
static Dwg_Object_Ref *
find_block_ref_by_handle (Dwg_Data *dwg, BITCODE_RLL handle)
{
  BITCODE_BL i;
  if (!dwg->block_control.entries)
    return NULL;
  for (i = 0; i < dwg->block_control.num_entries; i++)
    {
      Dwg_Object_Ref *ref = dwg->block_control.entries[i];
      if (ref && ref->absolute_ref == handle)
        return ref;
    }
  return NULL;
}

/* Returns the data-layer string to use for an entity inside the current
   emission context.  Honours layer-0 inheritance: when emitting block
   contents under a non-NULL current_eff_layer, an entity on layer 0 is
   tagged with the effective layer instead.  Caller must free. */
static char *
effective_layer_name (Dwg_Object *obj, Dwg_Data *dwg)
{
  char *own = entity_layer_name (obj, dwg);
  if (current_eff_layer && own && strcmp (own, "0") == 0)
    {
      free (own);
      return strdup (current_eff_layer);
    }
  return own;
}

/* Compute the effective layer of an INSERT given the current emission
   context.  Caller must free.  The result is used both for routing the
   <use xlink:href> to the correct block clone AND as the parent_eff_layer
   when recursively collecting combos for the block's contents.

   - Top-level (current_eff_layer == NULL): the INSERT's own layer.
   - Inside a block clone (current_eff_layer != NULL): own layer if it is
     not "0"; otherwise the parent's effective layer. */
static char *
insert_effective_layer (Dwg_Object *obj, Dwg_Data *dwg,
                        const char *parent_eff_layer)
{
  char *own = entity_layer_name (obj, dwg);
  if (parent_eff_layer && own && strcmp (own, "0") == 0)
    {
      free (own);
      return strdup (parent_eff_layer);
    }
  return own;
}

/* Walk the entities of `container_obj` (a BLOCK_HEADER) and gather all
   (target_block_handle, target_block_effective_layer) combinations needed
   to satisfy layer-0 inheritance.  Recurses into the target block of each
   INSERT it finds with the INSERT's effective layer as parent. */
static void
collect_block_combos_recursive (Dwg_Data *dwg, Dwg_Object *container_obj,
                                const char *parent_eff_layer)
{
  Dwg_Object *obj;
  if (!container_obj)
    return;
  obj = get_first_owned_entity (container_obj);
  while (obj)
    {
      if (obj->fixedtype == DWG_TYPE_INSERT)
        {
          Dwg_Entity_INSERT *ins = obj->tio.entity->tio.INSERT;
          if (ins->block_header && ins->block_header->obj
              && ins->block_header->obj->fixedtype == DWG_TYPE_BLOCK_HEADER)
            {
              Dwg_Object *target = ins->block_header->obj;
              char *eff = insert_effective_layer (obj, dwg,
                                                  parent_eff_layer);
              if (eff)
                {
                  BITCODE_RLL target_handle
                      = ins->block_header->absolute_ref;
                  int newly_added
                      = add_block_combo (target_handle, eff);
                  if (newly_added)
                    collect_block_combos_recursive (dwg, target, eff);
                  free (eff);
                }
            }
        }
      obj = get_next_owned_entity (container_obj, obj);
    }
}

/* Build the set of (block_handle, effective_layer) combos that must be
   emitted in <defs>.  Walks top-level model space and paper space, then
   recurses into each referenced block. */
static void
collect_block_combos (Dwg_Data *dwg)
{
  Dwg_Object_Ref *ref;
  /* Fresh start each invocation. */
  free_block_combos ();
  if ((ref = dwg_model_space_ref (dwg)) && ref->obj)
    collect_block_combos_recursive (dwg, ref->obj, NULL);
  if ((ref = dwg_paper_space_ref (dwg)) && ref->obj)
    collect_block_combos_recursive (dwg, ref->obj, NULL);
}

static int
output_BLOCK_HEADER (Dwg_Object_Ref *ref)
{
  Dwg_Object *obj;
  Dwg_Object_BLOCK_HEADER *hdr;
  int is_g = 0;
  int is_main_space = 0;
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
          const char *suffix
              = current_eff_layer_safe ? current_eff_layer_safe : "_0";
          is_g = 1;
          // Set block definition mode with block's base point
          in_block_definition = 1;
          block_base_x = hdr->base_pt.x;
          block_base_y = hdr->base_pt.y;
          printf ("\t<g id=\"symbol-" FORMAT_HV "-%s\" >\n\t\t<!-- %s -->\n",
                  ref->absolute_ref, suffix, escaped ? escaped : "");
        }
      else
        {
          is_main_space = 1;
          printf ("\t<!-- %s -->\n", escaped);
        }
      if (escaped)
        free (escaped);
    }

  if (is_main_space || is_g)
    {
      /* Group entities by layer. Two passes:
         1. collect unique layer objects used by entities in this block
         2. for each layer emit a <g data-layer="..."> wrapping its entities
         Applied to both the main rendering space (paper/model) AND named block
         definitions in <defs>, so the FE can toggle layer visibility globally
         by setting display:none on [data-layer="X"] — changes propagate through
         all <use> references that reference a block containing that layer. */
      Dwg_Data *dwg = ref->obj->parent;
      LayerEntry *layers = NULL;
      int nlayers = 0, layer_cap = 0;
      int i, li;

      /* Pass 1: collect unique data-layer names for non-INSERT entities
         and INSERT attribs.  INSERTs themselves are emitted outside any
         data-layer group (transparent containers), so their own layer is
         not collected here.  ATTRIBs of INSERTs carry their own layer
         assignment (no layer-0 inheritance — they are attached to the
         INSERT, not contents of the block definition), so they use
         entity_layer_name directly.  Non-INSERT block contents use
         effective_layer_name to apply layer-0 inheritance when we are
         emitting a block clone (current_eff_layer != NULL). */
      obj = get_first_owned_entity (ref->obj);
      while (obj)
        {
          if (obj->fixedtype != DWG_TYPE_INSERT)
            {
              char *name = effective_layer_name (obj, dwg);
              int found = 0;
              for (i = 0; i < nlayers; i++)
                if (strcmp (layers[i].attr_name, name) == 0)
                  {
                    found = 1;
                    break;
                  }
              if (!found)
                {
                  if (nlayers >= layer_cap)
                    {
                      layer_cap = layer_cap == 0 ? 8 : layer_cap * 2;
                      layers = (LayerEntry *)realloc (
                          layers,
                          (size_t)layer_cap * sizeof (LayerEntry));
                    }
                  layers[nlayers].attr_name = name;
                  layers[nlayers].safe_id = layer_safe_id (name);
                  nlayers++;
                  name = NULL; /* ownership transferred */
                }
              if (name)
                free (name);
            }
          else
            {
              Dwg_Entity_INSERT *insert = obj->tio.entity->tio.INSERT;
              if (insert->has_attribs && insert->num_owned > 0
                  && insert->attribs)
                {
                  BITCODE_BL ai;
                  for (ai = 0; ai < insert->num_owned; ai++)
                    {
                      Dwg_Object *aobj;
                      char *aname;
                      int afound = 0;
                      aobj = dwg_ref_object_silent (dwg,
                                                    insert->attribs[ai]);
                      if (!aobj || aobj->fixedtype == DWG_TYPE_SEQEND
                          || aobj->supertype != DWG_SUPERTYPE_ENTITY)
                        continue;
                      aname = entity_layer_name (aobj, dwg);
                      for (i = 0; i < nlayers; i++)
                        if (strcmp (layers[i].attr_name, aname) == 0)
                          {
                            afound = 1;
                            break;
                          }
                      if (!afound)
                        {
                          if (nlayers >= layer_cap)
                            {
                              layer_cap
                                  = layer_cap == 0 ? 8 : layer_cap * 2;
                              layers = (LayerEntry *)realloc (
                                  layers,
                                  (size_t)layer_cap * sizeof (LayerEntry));
                            }
                          layers[nlayers].attr_name = aname;
                          layers[nlayers].safe_id = layer_safe_id (aname);
                          nlayers++;
                          aname = NULL; /* ownership transferred */
                        }
                      if (aname)
                        free (aname);
                    }
                }
            }
          obj = get_next_owned_entity (ref->obj, obj);
        }

      /* Pass 2a: emit INSERT <use> elements with no data-layer wrapper.
         INSERTs are transparent placement references — visibility of an
         INSERT is decided by what its referenced block actually renders,
         where each block-internal entity sits inside its own
         <g data-layer="..."> and CSS [data-layer="X"] { display:none }
         cascades through the <use> shadow tree.  Emitted BEFORE the
         per-layer groups so they stack visually under direct entities
         and ATTRIBs (annotations sit on top of symbols). */
      obj = get_first_owned_entity (ref->obj);
      while (obj)
        {
          if (obj->fixedtype == DWG_TYPE_INSERT)
            num += output_object (obj);
          obj = get_next_owned_entity (ref->obj, obj);
        }

      /* Pass 2b: emit non-INSERT entities and INSERT ATTRIBs grouped by
         layer.  Each layer's <g data-layer="..."> drives the front-end
         visibility toggle (CSS rule on the data-layer attribute). */
      for (li = 0; li < nlayers; li++)
        {
          printf ("\t<g data-layer=\"%s\" id=\"layer-%s\">\n",
                  layers[li].attr_name, layers[li].safe_id);
          obj = get_first_owned_entity (ref->obj);
          while (obj)
            {
              if (obj->fixedtype != DWG_TYPE_INSERT)
                {
                  char *name = effective_layer_name (obj, dwg);
                  if (strcmp (name, layers[li].attr_name) == 0)
                    num += output_object (obj);
                  free (name);
                }
              else
                {
                  /* INSERT itself was emitted in Pass 2a; its attached
                     ATTRIBs belong in their own data-layer groups. */
                  Dwg_Entity_INSERT *insert = obj->tio.entity->tio.INSERT;
                  if (insert->has_attribs && insert->num_owned > 0
                      && insert->attribs)
                    {
                      BITCODE_BL ai;
                      for (ai = 0; ai < insert->num_owned; ai++)
                        {
                          Dwg_Object *aobj;
                          char *aname;
                          aobj = dwg_ref_object_silent (
                              dwg, insert->attribs[ai]);
                          if (!aobj || aobj->fixedtype == DWG_TYPE_SEQEND
                              || aobj->supertype != DWG_SUPERTYPE_ENTITY)
                            continue;
                          aname = entity_layer_name (aobj, dwg);
                          if (strcmp (aname, layers[li].attr_name) == 0)
                            num += output_object (aobj);
                          free (aname);
                        }
                    }
                }
              obj = get_next_owned_entity (ref->obj, obj);
            }
          printf ("\t</g>\n");
        }

      for (i = 0; i < nlayers; i++)
        {
          free (layers[i].attr_name);
          free (layers[i].safe_id);
        }
      free (layers);
    }
  else
    {
      /* Unnamed blocks: output entities sequentially.
         Attribs are no longer emitted by output_INSERT itself, so we
         emit them inline here to preserve the legacy behaviour for this
         path (no layer grouping). */
      obj = get_first_owned_entity (ref->obj);
      while (obj)
        {
          num += output_object (obj);
          if (obj->fixedtype == DWG_TYPE_INSERT)
            output_INSERT_attribs (obj);
          obj = get_next_owned_entity (ref->obj, obj);
        }
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

  /* Compute extents only from the space that will actually be rendered.
     Never mix paper-space and model-space extents: their coordinate systems
     are typically unrelated, so combining them produces a wrong page_height
     and causes the Y-flip to map content to the wrong region (upside-down). */
  if (!mspace && (ref = dwg_paper_space_ref (dwg)))
    compute_block_extents (&ext, ref);

  /* Fall back to model space only when paper space contributed nothing. */
  if (!ext.initialized && (ref = dwg_model_space_ref (dwg)))
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

  // Build the layer handle→name lookup table (used by entity_layer_name)
  build_layer_handle_table (dwg);

  /* Collect (block_handle, effective_layer) combinations driven by every
     INSERT (top-level and nested).  We emit one <g id="symbol-{handle}-
     {eff_layer_safe_id}"> in <defs> per combo, with each clone's
     layer-0 entities tagged with the effective layer — implementing
     AutoCAD's layer-0-in-block inheritance rule. */
  collect_block_combos (dwg);

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

  /* Enumerate every layer defined in the DWG layer table so FE JS has a
     complete list — including layers whose geometry lives inside block defs.
     Format is intentionally simple for regex:
       <!-- dwg-layers-count:N -->
       <!-- dwg-layer:LayerName --> */
  {
    unsigned int num_all_layers = dwg_get_layer_count (dwg);
    Dwg_Object_LAYER **all_layers = dwg_get_layers (dwg);
    unsigned int li;
    printf ("\t<!-- dwg-layers-count:%u -->\n", num_all_layers);
    for (li = 0; li < num_all_layers; li++)
      {
        if (all_layers && all_layers[li] && all_layers[li]->name)
          {
            char *escaped;
            if (dwg->header.version >= R_2007)
              escaped = htmlwescape ((BITCODE_TU)all_layers[li]->name);
            else
              escaped = htmlescape (all_layers[li]->name,
                                    dwg->header.codepage);
            if (escaped)
              {
                char *s;
                while ((s = strstr (escaped, "--")))
                  { *s = '_'; *(s + 1) = '_'; }
                printf ("\t<!-- dwg-layer:%s -->\n", escaped);
                free (escaped);
              }
          }
      }
    free (all_layers);
  }

  if (!mspace && (ref = dwg_paper_space_ref (dwg)))
    {
      paper_space_bg = 1; // paper-space has a white background
      /* White paper rectangle so the SVG is self-contained.  Viewers
         can set the area outside the viewBox to grey (#8A8A8A) to
         match the AutoCAD paper-space chrome. */
      printf ("\t<rect x=\"%f\" y=\"%f\" width=\"%f\" height=\"%f\" "
              "fill=\"white\" />\n",
              0.0, 0.0, page_width, page_height);
      num = output_BLOCK_HEADER (
          ref); // how many paper-space entities we did print
      if (!num)
        paper_space_bg = 0; // paper space was empty, falling back to model
    }
  if (!num && (ref = dwg_model_space_ref (dwg)))
    output_BLOCK_HEADER (ref);

  /* Render Model_Space content through Paper_Space VIEWPORTs.
     Each VIEWPORT defines a window from Paper_Space into Model_Space.
     We render Model_Space content with a transform that maps it
     to the viewport's region in Paper_Space (SVG coordinates). */
  if (num && !mspace)
    {
      Dwg_Object_Ref *ms_ref = dwg_model_space_ref (dwg);
      Dwg_Object_Ref *ps_ref = dwg_paper_space_ref (dwg);
      if (ms_ref && ms_ref->obj && ps_ref && ps_ref->obj)
        {
          /* Find VIEWPORTs in Paper_Space.  Skip the first one encountered —
             it is the paper-space frame viewport (covers the whole page)
             and should not render Model_Space content. */
          Dwg_Object *vpobj = get_first_owned_entity (ps_ref->obj);
          int vp_seen = 0;
          while (vpobj)
            {
              if (vpobj->fixedtype == DWG_TYPE_VIEWPORT)
                {
                  Dwg_Entity_VIEWPORT *vp
                      = vpobj->tio.entity->tio.VIEWPORT;
                  vp_seen++;
                  /* The first VIEWPORT is the paper-space frame — skip it.
                     Only render Model_Space through subsequent viewports. */
                  if (vp_seen > 1 && vp->VIEWSIZE > 0.0
                      && vp->width > 0.0 && vp->height > 0.0)
                    {
                      double s = vp->height / vp->VIEWSIZE;
                      double tx = vp->center.x - s * vp->VIEWCTR.x
                                  - model_xmin;
                      double ty = page_height - vp->center.y
                                  + s * vp->VIEWCTR.y + model_ymin;

                      /* Clip rectangle in SVG paper-space coordinates */
                      double clip_x = vp->center.x - vp->width / 2.0
                                      - model_xmin;
                      double clip_y = page_height
                                      - (vp->center.y + vp->height / 2.0)
                                      + model_ymin;

                      printf ("\t<defs><clipPath id=\"vp-clip-%d\">"
                              "<rect x=\"%f\" y=\"%f\" "
                              "width=\"%f\" height=\"%f\"/>"
                              "</clipPath></defs>\n",
                              vpobj->index, clip_x, clip_y,
                              vp->width, vp->height);

                      printf ("\t<g clip-path=\"url(#vp-clip-%d)\">\n",
                              vpobj->index);
                      printf ("\t\t<g transform=\"matrix(%f 0 0 %f %f %f)\">"
                              "<!-- viewport-%d -->\n",
                              s, -s, tx, ty, vpobj->index);

                      /* Render Model_Space content with raw coordinates */
                      {
                        int save_ibd = in_block_definition;
                        in_block_definition = 1;
                        output_BLOCK_HEADER (ms_ref);
                        in_block_definition = save_ibd;
                      }

                      printf ("\t\t</g>\n");
                      printf ("\t</g>\n");
                    }
                }
              vpobj = get_next_owned_entity (ps_ref->obj, vpobj);
            }
        }
    }

  /* Emit one block clone per (block_handle, effective_layer) combo.
     The same block may appear multiple times in <defs> — once per
     distinct effective layer it's referenced from — so its layer-0
     contents render with the correct inherited layer.  Each clone is
     identified by its suffixed id "symbol-{handle}-{eff_layer_safe_id}",
     which output_INSERT references via xlink:href. */
  printf ("\t<defs>\n");
  {
    unsigned int ci;
    for (ci = 0; ci < g_block_combos_n; ci++)
      {
        Dwg_Object_Ref *blk_ref = find_block_ref_by_handle (
            dwg, g_block_combos[ci].block_handle_value);
        if (!blk_ref)
          continue;
        current_eff_layer = g_block_combos[ci].eff_layer;
        current_eff_layer_safe = g_block_combos[ci].eff_layer_safe_id;
        output_BLOCK_HEADER (blk_ref);
        current_eff_layer = NULL;
        current_eff_layer_safe = NULL;
      }
  }
  printf ("\t</defs>\n");

  /* Diagnostic comment: extents used for coordinate mapping */
  printf ("\t<!-- dwg-extents: xmin=%f ymin=%f xmax=%f ymax=%f "
          "page=%fx%f space=%s htbl_layers=%u -->\n",
          model_xmin, model_ymin, model_xmax, model_ymax,
          page_width, page_height,
          num ? "paper" : "model",
          g_layer_htbl_n);

  /* Emit the CTB stylesheet from the layout of the space actually rendered.
     The frontend uses this to select the correct CTB for print mode, so when
     paper space was requested but empty and we fell back to model space
     (num == 0), the CTB source must also switch to model space. */
  {
    Dwg_Object_Ref *ps_ref = (mspace || !num)
                                 ? dwg_model_space_ref (dwg)
                                 : dwg_paper_space_ref (dwg);
    if (!ps_ref)
      ps_ref = dwg_model_space_ref (dwg);
    if (ps_ref && ps_ref->obj
        && ps_ref->obj->fixedtype == DWG_TYPE_BLOCK_HEADER)
      {
        Dwg_Object_BLOCK_HEADER *bh
            = ps_ref->obj->tio.object->tio.BLOCK_HEADER;
        if (bh->layout && bh->layout->obj
            && bh->layout->obj->fixedtype == DWG_TYPE_LAYOUT)
          {
            Dwg_Object_LAYOUT *lo
                = bh->layout->obj->tio.object->tio.LAYOUT;
            if (lo->plotsettings.stylesheet
                && *(lo->plotsettings.stylesheet))
              {
                /* Convert TU (UTF-16) to UTF-8 for R_2007+ and entity-escape,
                   then neutralise any "--" which is illegal inside an XML
                   comment (same pattern as dwg-layer emission above). */
                char *escaped;
                if (dwg->header.version >= R_2007)
                  escaped
                      = htmlwescape ((BITCODE_TU)lo->plotsettings.stylesheet);
                else
                  escaped = htmlescape (lo->plotsettings.stylesheet,
                                        dwg->header.codepage);
                if (escaped)
                  {
                    char *s;
                    while ((s = strstr (escaped, "--")))
                      { *s = '_'; *(s + 1) = '_'; }
                    printf ("\t<!-- dwg-ctb:%s -->\n", escaped);
                    free (escaped);
                  }
              }
          }
      }
  }

  printf ("</svg>\n");
  fflush (stdout);
  free_layer_handle_table ();
  free_block_combos ();
  current_eff_layer = NULL;
  current_eff_layer_safe = NULL;
  paper_space_bg = 0; // reset for next call when used as a library
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
  /* Not static: &opts is no longer a constant expression because `opts` is
     _Thread_local.  Auto storage allows runtime initializers. */
  struct option long_options[]
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
