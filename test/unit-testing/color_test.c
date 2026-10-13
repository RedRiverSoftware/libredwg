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

/* CMC colour methods as AutoCAD writes them: 0xc0 ByLayer, 0xc1 ByBlock,
   0xc2 true colour (RGB in the low 24 bits), 0xc3 ACI (the palette index
   in the low byte), 0xc8 None. */

#define COLOR_TEST_C
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tracelevel;
// the library sets loglevel from the document's opts (on read, write and
// free); ok and fail need ours
static unsigned int test_loglevel;

#include "config.h"
#ifdef HAVE_UNISTD_H
#  include <unistd.h>
#endif
#include "dwg.h"
#include "tests_common.h"
#include "dwg_api.h"
#include "bits.h"
#include "out_dxf.h"
#include "out_json.h"
#ifndef DISABLE_DXF
#  include "decode.h"
#  include "in_dxf.h"
#endif

/* An r2004 CMC: BS index 0, BL rgb, RC flag 0 */
static void
cmc_chain (Bit_Chain *dat, const BITCODE_BL rgb)
{
  memset (dat, 0, sizeof (Bit_Chain));
  bit_chain_init (dat, 16);
  dat->from_version = dat->version = R_2004;
  bit_write_BS (dat, 0);
  bit_write_BL (dat, rgb);
  bit_write_RC (dat, 0);
  bit_set_position (dat, 0);
}

static void
test_read_CMC (void)
{
  static const struct
  {
    BITCODE_BL rgb;
    BITCODE_BSd index;
    const char *what;
  } cases[] = {
    { 0xc0000000, 256, "ByLayer" },
    { 0xc1000000, 0, "ByBlock" },
    { 0xc8000000, 257, "None" },
    { 0xc3000007, 7, "ACI 7" },
    { 0xc30000ff, 255, "ACI 255" },
    { 0xc3000000, 0, "ACI form of ByBlock" },
    { 0xc3000100, 256, "ACI form of ByLayer" },
    { 0xc3000101, 257, "ACI form of None" },
    // true colour: the index is the palette entry with that RGB, as before
    { 0xc2ff0000, 1, "true colour red" },
  };
  for (unsigned i = 0; i < sizeof (cases) / sizeof (cases[0]); i++)
    {
      Bit_Chain dat;
      Dwg_Color color;
      cmc_chain (&dat, cases[i].rgb);
      bit_read_CMC (&dat, &dat, &color);
      if (color.index == cases[i].index && color.rgb == cases[i].rgb
          && color.method == cases[i].rgb >> 24)
        ok ("bit_read_CMC 0x%08x %s: index %d", (unsigned)cases[i].rgb,
            cases[i].what, color.index);
      else
        fail ("bit_read_CMC 0x%08x %s: index %d, method 0x%x, expected %d",
              (unsigned)cases[i].rgb, cases[i].what, color.index,
              (unsigned)color.method, cases[i].index);
      bit_chain_free (&dat);
    }
}

/* r13-r2000 palette index to r2004 */
static void
test_upconvert_CMC (void)
{
  static const struct
  {
    BITCODE_BSd index;
    BITCODE_BL rgb_in; // with its method, or 0
    BITCODE_BL rgb;
    const char *what;
  } cases[] = {
    { 7, 0, 0xc3000007, "ACI 7" },
    // a layer that is off has a negative index
    { -7, 0, 0xc3000007, "ACI 7, layer off" },
    { -255, 0, 0xc30000ff, "ACI 255, layer off" },
    { 256, 0, 0xc0000000, "ByLayer" },
    { 0, 0, 0xc1000000, "ByBlock" },
    { 257, 0, 0xc8000000, "None" },
    // e.g. from a DXF 420 group
    { 148, 0xc2123456, 0xc2123456, "true colour" },
    { 256, 0xc3000100, 0xc3000100, "ACI form of ByLayer" },
    { 257, 0xc3000101, 0xc3000101, "ACI form of None" },
  };
  for (unsigned i = 0; i < sizeof (cases) / sizeof (cases[0]); i++)
    {
      Bit_Chain dat = { 0 };
      Dwg_Color color;
      dat.from_version = R_2000;
      dat.version = R_2004;
      memset (&color, 0, sizeof (color));
      color.index = cases[i].index;
      color.rgb = cases[i].rgb_in;
      color.method = cases[i].rgb_in >> 24;
      bit_upconvert_CMC (&dat, &color);
      if (color.rgb == cases[i].rgb)
        ok ("bit_upconvert_CMC %d %s: 0x%08x", cases[i].index, cases[i].what,
            (unsigned)color.rgb);
      else
        fail ("bit_upconvert_CMC %d %s: 0x%08x, expected 0x%08x",
              cases[i].index, cases[i].what, (unsigned)color.rgb,
              (unsigned)cases[i].rgb);
    }
}

/* r2004 to r13-r2000 */
static void
test_downconvert_CMC (void)
{
  static const struct
  {
    BITCODE_BL rgb;
    BITCODE_BSd index;
    const char *what;
  } cases[] = {
    // the low byte of these is the RGB of another palette entry (178, 176,
    // 174, 172 and 5), which a palette lookup returns
    { 0xc300004f, 79, "ACI 79" },
    { 0xc3000068, 104, "ACI 104" },
    { 0xc3000081, 129, "ACI 129" },
    { 0xc30000bd, 189, "ACI 189" },
    { 0xc30000ff, 255, "ACI 255" },
    { 0xc3000007, 7, "ACI 7" },
    { 0xc3000100, 256, "ACI form of ByLayer" },
    { 0xc3000101, 0, "ACI form of None, as None" },
    { 0xc0000000, 256, "ByLayer" },
    { 0xc8000000, 0, "None" },
  };
  for (unsigned i = 0; i < sizeof (cases) / sizeof (cases[0]); i++)
    {
      Bit_Chain dat = { 0 };
      Dwg_Color color;
      dat.from_version = R_2004;
      dat.version = R_2000;
      memset (&color, 0, sizeof (color));
      color.rgb = cases[i].rgb;
      color.method = cases[i].rgb >> 24;
      // as bit_read_CMC reads it
      if (color.method == 0xc3)
        color.index = cases[i].rgb & 0xff;
      bit_downconvert_CMC (&dat, &color);
      if (color.index == cases[i].index)
        ok ("bit_downconvert_CMC 0x%08x %s: %d", (unsigned)cases[i].rgb,
            cases[i].what, color.index);
      else
        fail ("bit_downconvert_CMC 0x%08x %s: %d, expected %d",
              (unsigned)cases[i].rgb, cases[i].what, color.index,
              cases[i].index);
    }
}

/* bit_write_CMC of a palette index only, as the add API sets it, and
   bit_read_CMC agree */
static void
test_write_read_CMC (void)
{
  static const struct
  {
    BITCODE_BSd index;
    BITCODE_BL rgb;
    BITCODE_BSd read;
    const char *what;
  } cases[] = {
    { 256, 0xc0000000, 256, "ByLayer" },
    { 7, 0xc3000007, 7, "ACI 7" },
    { 79, 0xc300004f, 79, "ACI 79" },
    // r2004 keeps a layer's off state in its flags, not in the colour
    { -7, 0xc3000007, 7, "ACI 7, layer off" },
    { 257, 0xc8000000, 257, "None" },
    { 0, 0xc1000000, 0, "ByBlock" },
  };
  for (unsigned i = 0; i < sizeof (cases) / sizeof (cases[0]); i++)
    {
      Bit_Chain dat = { 0 };
      Dwg_Color color, read;
      bit_chain_init (&dat, 16);
      dat.from_version = dat.version = R_2004;
      memset (&color, 0, sizeof (color));
      color.index = cases[i].index;
      bit_write_CMC (&dat, &dat, &color);
      bit_set_position (&dat, 0);
      bit_read_CMC (&dat, &dat, &read);
      if (read.rgb == cases[i].rgb && read.index == cases[i].read)
        ok ("bit_write_CMC r2004 %d %s: 0x%08x", cases[i].index,
            cases[i].what, (unsigned)read.rgb);
      else
        fail ("bit_write_CMC r2004 %d %s: 0x%08x, read back %d, expected "
              "0x%08x",
              cases[i].index, cases[i].what, (unsigned)read.rgb, read.index,
              (unsigned)cases[i].rgb);
      bit_chain_free (&dat);
    }
}

/* A solid's history node is ByLayer, as AutoCAD writes it */
static void
test_history_node_color (void)
{
  const dwg_point_3d origin = { 0.0, 0.0, 0.0 };
  Dwg_Data *dwg = dwg_new_Document (R_2004, 0, tracelevel);
  Dwg_Object_BLOCK_HEADER *ms
      = dwg_object_to_BLOCK_HEADER (dwg_model_space_object (dwg));
  unsigned found = 0;

  dwg_add_BOX (ms, &origin, NULL, 10.0, 10.0, 10.0);
  loglevel = test_loglevel;
  for (BITCODE_BL i = 0; i < dwg->num_objects; i++)
    {
      Dwg_Object *obj = &dwg->object[i];
      if (obj->fixedtype == DWG_TYPE_ACSH_BOX_CLASS)
        {
          const Dwg_Color *color
              = &obj->tio.object->tio.ACSH_BOX_CLASS->history_node.color;
          found++;
          if (color->index == 256 && color->rgb == 0xc0000000
              && color->method == 0xc0)
            ok ("dwg_add_BOX: history node ByLayer");
          else
            fail ("dwg_add_BOX: history node %d 0x%08x", color->index,
                  (unsigned)color->rgb);
        }
    }
  if (!found)
    fail ("dwg_add_BOX: no ACSH_BOX_CLASS");
  dwg_free (dwg);
  free (dwg);
}

#ifndef DISABLE_JSON
/* GeoJSON writes a true colour as "#rrggbb", and not an ACI colour */
static void
test_geojson_color (void)
{
  const char *file = "color_test.geojson";
  const dwg_point_3d p1 = { 0.0, 0.0, 0.0 };
  const dwg_point_3d p2 = { 100.0, 50.0, 0.0 };
  Dwg_Data *dwg = dwg_new_Document (R_2004, 0, tracelevel);
  Dwg_Object_BLOCK_HEADER *ms
      = dwg_object_to_BLOCK_HEADER (dwg_model_space_object (dwg));
  Dwg_Entity_LINE *line;
  Bit_Chain dat = { 0 };
  static char buf[65536];
  size_t size;
  FILE *fp;

  line = dwg_add_LINE (ms, &p1, &p2);
  line->parent->color.index = 256;
  line->parent->color.method = 0xc2;
  line->parent->color.rgb = 0xc2123456;
  // the ACI form of ByLayer
  line = dwg_add_LINE (ms, &p2, &p1);
  line->parent->color.index = 256;
  line->parent->color.method = 0xc3;
  line->parent->color.rgb = 0xc3000100;

  dat.version = dat.from_version = dwg->header.version;
  dat.codepage = dwg->header.codepage;
  unlink (file);
  dat.fh = fopen (file, "w");
  if (!dat.fh)
    {
      fail ("fopen %s", file);
      dwg_free (dwg);
      free (dwg);
      return;
    }
  dwg_write_geojson (&dat, dwg);
  fclose (dat.fh);
  dwg_free (dwg);
  free (dwg);
  loglevel = test_loglevel;

  fp = fopen (file, "rb");
  size = fp ? fread (buf, 1, sizeof (buf) - 1, fp) : 0;
  if (fp)
    fclose (fp);
  buf[size] = '\0';
  if (strstr (buf, "\"#123456\"") && !strstr (buf, "\"#000100\""))
    ok ("GeoJSON: true colour #123456, no #000100 for ACI");
  else
    fail ("GeoJSON colours: %s", buf);
  if (!numfailed ())
    unlink (file);
}
#endif

static void
test_method_names (void)
{
  if (DWG_COLOR_METHOD_TRUECOLOR == 0xc2 && DWG_COLOR_METHOD_ACI == 0xc3)
    ok ("DWG_COLOR_METHOD_TRUECOLOR 0xc2, DWG_COLOR_METHOD_ACI 0xc3");
  else
    fail ("DWG_COLOR_METHOD_TRUECOLOR 0x%x, DWG_COLOR_METHOD_ACI 0x%x",
          (unsigned)DWG_COLOR_METHOD_TRUECOLOR,
          (unsigned)DWG_COLOR_METHOD_ACI);
  if (!strcmp (dwg_color_method_name (0xc2), "Truecolor")
      && !strcmp (dwg_color_method_name (0xc3), "ACI"))
    ok ("dwg_color_method_name 0xc2 Truecolor, 0xc3 ACI");
  else
    fail ("dwg_color_method_name 0xc2 %s, 0xc3 %s",
          dwg_color_method_name (0xc2), dwg_color_method_name (0xc3));
}

#ifndef DISABLE_DXF

struct layer_color
{
  const char *name;
  BITCODE_BSd index;
};
static const struct layer_color layer_colors[] = {
  { "0", 7 },
  { "L2", 2 },
  { "L4", 4 },
  { "L79", 79 },
  // a layer that is off has a negative index in DXF
  { "OFF5", -5 },
};

static Dwg_Object_LAYER *
find_layer (Dwg_Data *dwg, const char *name)
{
  for (BITCODE_BL i = 0; i < dwg->num_objects; i++)
    {
      Dwg_Object *obj = &dwg->object[i];
      if (obj->fixedtype == DWG_TYPE_LAYER)
        {
          Dwg_Object_LAYER *layer = obj->tio.object->tio.LAYER;
          if (layer->name && !strcmp (layer->name, name))
            return layer;
        }
    }
  return NULL;
}

static Dwg_Entity_LINE *
find_line (Dwg_Data *dwg)
{
  for (BITCODE_BL i = 0; i < dwg->num_objects; i++)
    if (dwg->object[i].fixedtype == DWG_TYPE_LINE)
      return dwg->object[i].tio.entity->tio.LINE;
  return NULL;
}

static Dwg_Object_DIMSTYLE *
find_dimstyle (Dwg_Data *dwg, const char *name)
{
  for (BITCODE_BL i = 0; i < dwg->num_objects; i++)
    {
      Dwg_Object *obj = &dwg->object[i];
      if (obj->fixedtype == DWG_TYPE_DIMSTYLE)
        {
          Dwg_Object_DIMSTYLE *dimstyle = obj->tio.object->tio.DIMSTYLE;
          if (dimstyle->name && !strcmp (dimstyle->name, name))
            return dimstyle;
        }
    }
  return NULL;
}

/* DXF export of the ACI forms of ByLayer and None (0xc3 with 0x100 and
   0x101), then DXF import, then r2004: layer colours (DXF 62) become ACI,
   $CECOLOR 256 becomes ByLayer and an entity's true colour (DXF 420) stays a
   true colour, in the form AutoCAD writes them, and read back so. */
static void
test_dxf_to_r2004 (void)
{
  const char *dxffile = "color_test_2004.dxf";
  const char *dwgfile = "color_test_2004.dwg";
  const dwg_point_3d p1 = { 0.0, 0.0, 0.0 };
  const dwg_point_3d p2 = { 100.0, 50.0, 0.0 };
  Dwg_Data *dwg = dwg_new_Document (R_2004, 0, tracelevel);
  Dwg_Data imported, read;
  Dwg_Object_BLOCK_HEADER *ms
      = dwg_object_to_BLOCK_HEADER (dwg_model_space_object (dwg));
  Dwg_Entity_LINE *line;
  Dwg_Object_DIMSTYLE *dimstyle;
  Bit_Chain dat = { 0 };
  int error;

  for (unsigned i = 1; i < sizeof (layer_colors) / sizeof (layer_colors[0]);
       i++)
    {
      Dwg_Object_LAYER *layer = dwg_add_LAYER (dwg, layer_colors[i].name);
      layer->color.index = layer_colors[i].index;
      // out_dxf writes a negative 62 for a layer that is off (flag0 2)
      if (layer_colors[i].index < 0)
        layer->flag0 |= 2;
    }
  line = dwg_add_LINE (ms, &p1, &p2);
  line->parent->color.index = 256;
  line->parent->color.flag = 0x80;
  line->parent->color.rgb = 0xc2123456;
  dimstyle = dwg_add_DIMSTYLE (dwg, "CT");
  dimstyle->DIMCLRD.index = 256;
  dimstyle->DIMCLRD.method = 0xc3;
  dimstyle->DIMCLRD.rgb = 0xc3000100;
  dimstyle->DIMCLRE.index = 257;
  dimstyle->DIMCLRE.method = 0xc3;
  dimstyle->DIMCLRE.rgb = 0xc3000101;

  dat.version = dwg->header.version;
  dat.from_version = dwg->header.from_version;
  dat.opts = dwg->opts;
  unlink (dxffile);
  dat.fh = fopen (dxffile, "wb");
  if (!dat.fh)
    {
      fail ("fopen %s", dxffile);
      dwg_free (dwg);
      free (dwg);
      return;
    }
  error = dwg_write_dxf (&dat, dwg);
  fclose (dat.fh);
  dwg_free (dwg);
  free (dwg);
  loglevel = test_loglevel;
  if (error >= DWG_ERR_CRITICAL)
    {
      fail ("dwg_write_dxf %s 0x%x", dxffile, error);
      return;
    }

  // as dxf2dwg --as r2004
  memset (&imported, 0, sizeof (Dwg_Data));
  imported.opts = tracelevel;
  imported.header.version = R_2004;
  error = dxf_read_file (dxffile, &imported);
  loglevel = test_loglevel;
  if (error >= DWG_ERR_CRITICAL)
    {
      fail ("dxf_read_file %s 0x%x", dxffile, error);
      dwg_free (&imported);
      return;
    }
  for (unsigned i = 0; i < sizeof (layer_colors) / sizeof (layer_colors[0]);
       i++)
    {
      Dwg_Object_LAYER *layer = find_layer (&imported, layer_colors[i].name);
      const BITCODE_BL rgb
          = 0xc3000000 | (BITCODE_BL)abs (layer_colors[i].index);
      if (layer && layer->color.rgb == rgb && layer->color.method == 0xc3)
        ok ("DXF import: layer %s 0x%08x", layer_colors[i].name,
            (unsigned)rgb);
      else if (layer)
        fail ("DXF import: layer %s 0x%08x, expected 0x%08x",
              layer_colors[i].name, (unsigned)layer->color.rgb,
              (unsigned)rgb);
      else
        fail ("DXF import: no layer %s", layer_colors[i].name);
    }
  if (imported.header_vars.CECOLOR.index == 256
      && imported.header_vars.CECOLOR.rgb == 0xc0000000)
    ok ("DXF import: $CECOLOR ByLayer 0xc0000000");
  else
    fail ("DXF import: $CECOLOR %d 0x%08x",
          imported.header_vars.CECOLOR.index,
          (unsigned)imported.header_vars.CECOLOR.rgb);
  line = find_line (&imported);
  if (line && line->parent->color.rgb == 0xc2123456)
    ok ("DXF import: LINE true colour 0xc2123456");
  else
    fail ("DXF import: LINE colour 0x%08x",
          line ? (unsigned)line->parent->color.rgb : 0);
  dimstyle = find_dimstyle (&imported, "CT");
  if (dimstyle && dimstyle->DIMCLRD.rgb == 0xc0000000
      && dimstyle->DIMCLRE.rgb == 0xc8000000)
    ok ("DXF export and import: ACI forms of ByLayer and None");
  else if (dimstyle)
    fail ("DXF export and import: DIMCLRD 0x%08x, DIMCLRE 0x%08x",
          (unsigned)dimstyle->DIMCLRD.rgb, (unsigned)dimstyle->DIMCLRE.rgb);
  else
    fail ("DXF import: no DIMSTYLE CT");

  if (imported.header.from_version == R_INVALID)
    imported.header.from_version = imported.header.version;
  unlink (dwgfile);
  error = dwg_write_file (dwgfile, &imported);
  dwg_free (&imported);
  loglevel = test_loglevel;
  if (error >= DWG_ERR_CRITICAL)
    {
      fail ("dwg_write_file %s 0x%x", dwgfile, error);
      return;
    }
  memset (&read, 0, sizeof (Dwg_Data));
  read.opts = tracelevel;
  error = dwg_read_file (dwgfile, &read);
  loglevel = test_loglevel;
  if (error >= DWG_ERR_CRITICAL)
    {
      fail ("dwg_read_file %s 0x%x", dwgfile, error);
      dwg_free (&read);
      return;
    }
  for (unsigned i = 0; i < sizeof (layer_colors) / sizeof (layer_colors[0]);
       i++)
    {
      Dwg_Object_LAYER *layer = find_layer (&read, layer_colors[i].name);
      // r2004 keeps a layer's off state in its flags, not in the colour
      const int off = layer_colors[i].index < 0;
      if (layer && layer->color.index == abs (layer_colors[i].index)
          && layer->off == off)
        ok ("DXF to r2004: layer %s colour %d%s", layer_colors[i].name,
            layer->color.index, off ? ", off" : "");
      else if (layer)
        fail ("DXF to r2004: layer %s colour %d 0x%08x off %d, expected %d",
              layer_colors[i].name, layer->color.index,
              (unsigned)layer->color.rgb, (int)layer->off,
              layer_colors[i].index);
      else
        fail ("DXF to r2004: no layer %s", layer_colors[i].name);
    }
  if (read.header_vars.CECOLOR.index == 256
      && read.header_vars.CECOLOR.rgb == 0xc0000000)
    ok ("DXF to r2004: $CECOLOR ByLayer");
  else
    fail ("DXF to r2004: $CECOLOR %d 0x%08x", read.header_vars.CECOLOR.index,
          (unsigned)read.header_vars.CECOLOR.rgb);
  line = find_line (&read);
  if (line && line->parent->color.rgb == 0xc2123456
      && line->parent->color.flag & 0x80)
    ok ("DXF to r2004: LINE true colour 0xc2123456");
  else
    fail ("DXF to r2004: LINE colour 0x%08x",
          line ? (unsigned)line->parent->color.rgb : 0);
  dwg_free (&read);
  if (!numfailed ())
    {
      unlink (dxffile);
      unlink (dwgfile);
    }
}
#endif

int
main (void)
{
  char *trace = getenv ("LIBREDWG_TRACE");
  loglevel = test_loglevel = is_make_silent () ? 0 : 2;
  tracelevel = trace ? atoi (trace) : 0;

  test_read_CMC ();
  test_upconvert_CMC ();
  test_downconvert_CMC ();
  test_write_read_CMC ();
  test_method_names ();
  test_history_node_color ();
#ifndef DISABLE_JSON
  test_geojson_color ();
#endif
#ifndef DISABLE_DXF
  test_dxf_to_r2004 ();
#endif
  return numfailed ();
}
