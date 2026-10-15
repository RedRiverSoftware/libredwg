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

/* DXF import: fields the importer sets where the free spec did not look for
   them, an r2000 LIGHT's colour, and what it keeps of a TABLESTYLE's cell
   styles. Under LeakSanitizer, make check also finds what it would leak. */

#define INDXF_TEST_C
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tracelevel;
// the library sets loglevel from the document's opts (on read, write and
// free); ok prints only from loglevel 2, so it needs ours back
static unsigned int test_loglevel;

#include "config.h"
#ifdef HAVE_UNISTD_H
#  include <unistd.h>
#endif
#include <sys/stat.h>
#include "dwg.h"
#include "tests_common.h"
#include "dwg_api.h"
#include "bits.h"
#ifndef DISABLE_DXF
#  include "out_dxf.h"
#  include "decode.h"
#  include "in_dxf.h"
#endif

#ifndef DISABLE_DXF

// finds name under test/test-data, from top_srcdir or the build directory
static int
find_test_data (const char *name, char *path, size_t size)
{
  const char *dirs[3];
  struct stat attrib;
  dirs[0] = getenv ("top_srcdir");
  dirs[1] = "../..";
  dirs[2] = "../../..";
  for (size_t i = 0; i < sizeof (dirs) / sizeof (dirs[0]); i++)
    {
      if (!dirs[i])
        continue;
      snprintf (path, size, "%s/test/test-data/%s", dirs[i], name);
      if (!stat (path, &attrib))
        return 1;
    }
  return 0;
}

static Dwg_Object *
first_object (Dwg_Data *dwg, const Dwg_Object_Type type)
{
  for (BITCODE_BL i = 0; i < dwg->num_objects; i++)
    if (dwg->object[i].fixedtype == type)
      return &dwg->object[i];
  return NULL;
}

// imports dxf, written to file, as dxf2dwg does: for an r2000 DWG
static int
import_dxf (const char *file, const char *dxf, Dwg_Data *dwg)
{
  FILE *fp;
  int error;
  // the callers free dwg on any critical error
  memset (dwg, 0, sizeof (Dwg_Data));
  fp = fopen (file, "wb");
  if (!fp)
    {
      fail ("fopen %s", file);
      return DWG_ERR_IOERROR;
    }
  fwrite (dxf, 1, strlen (dxf), fp);
  fclose (fp);
  dwg->opts = tracelevel;
  dwg->header.version = R_2000;
  error = dxf_read_file (file, dwg);
  loglevel = test_loglevel;
  if (error >= DWG_ERR_CRITICAL)
    fail ("dxf_read_file %s 0x%x", file, error);
  return error;
}

/* An r12 BLOCK with an xref path: the importer sets xref_pname, without
   the entity's opts bit the r12 free looks at. */
static void
test_r12_block_xref_pname (void)
{
  static const char dxf[]
      = "  0\nSECTION\n  2\nHEADER\n  9\n$ACADVER\n  1\nAC1009\n"
        "  9\n$INSBASE\n 10\n0.0\n 20\n0.0\n 30\n0.0\n"
        "  9\n$EXTMIN\n 10\n0.0\n 20\n0.0\n 30\n0.0\n  0\nENDSEC\n"
        "  0\nSECTION\n  2\nBLOCKS\n"
        "  0\nBLOCK\n  8\n0\n  2\nXREFBLK\n 70\n     4\n"
        " 10\n0.0\n 20\n0.0\n 30\n0.0\n  3\nXREFBLK\n  1\nxref.dwg\n"
        "  0\nENDBLK\n  8\n0\n  0\nENDSEC\n  0\nEOF\n";
  const char *file = "indxf_test_block.dxf";
  Dwg_Data dwg;
  Dwg_Object *obj;
  if (import_dxf (file, dxf, &dwg) >= DWG_ERR_CRITICAL)
    {
      dwg_free (&dwg);
      return;
    }
  obj = first_object (&dwg, DWG_TYPE_BLOCK);
  if (!obj || !obj->tio.entity->tio.BLOCK->xref_pname
      || strcmp (obj->tio.entity->tio.BLOCK->xref_pname, "xref.dwg"))
    fail ("r12 BLOCK.xref_pname");
  else
    ok ("r12 BLOCK.xref_pname xref.dwg");
  dwg_free (&dwg);
  if (!numfailed ())
    unlink (file);
}

/* An MTEXT with two 46 groups: the importer allocates column_heights at the
   first and keeps that one array for the second. */
static void
test_mtext_46 (void)
{
  static const char dxf[]
      = "  0\nSECTION\n  2\nHEADER\n  9\n$ACADVER\n  1\nAC1021\n"
        "  9\n$INSBASE\n 10\n0.0\n 20\n0.0\n 30\n0.0\n"
        "  9\n$HANDSEED\n  5\nFFFF\n  0\nENDSEC\n"
        "  0\nSECTION\n  2\nENTITIES\n"
        "  0\nMTEXT\n  5\n20\n100\nAcDbEntity\n  8\n0\n100\nAcDbMText\n"
        " 10\n0.0\n 20\n0.0\n 30\n0.0\n 40\n2.5\n 41\n10.0\n"
        " 46\n5.0\n 46\n6.0\n  1\ntext\n"
        "  0\nENDSEC\n  0\nEOF\n";
  const char *file = "indxf_test_mtext.dxf";
  Dwg_Data dwg;
  if (import_dxf (file, dxf, &dwg) >= DWG_ERR_CRITICAL)
    {
      dwg_free (&dwg);
      return;
    }
  if (!first_object (&dwg, DWG_TYPE_MTEXT))
    fail ("MTEXT with two 46 groups");
  else
    ok ("MTEXT with two 46 groups");
  dwg_free (&dwg);
  if (!numfailed ())
    unlink (file);
}

/* An r2000 LIGHT gives its colour as a second 90 group, after the
   class_version (as example_2000.dxf does). Read as the class_version, it
   was out of bounds, and dwg_free stopped before the name. An r2000 DWG
   keeps only the colour's palette index. */
static void
test_light_r2000_color (void)
{
  static const char dxf[]
      = "  0\nSECTION\n  2\nHEADER\n  9\n$ACADVER\n  1\nAC1015\n"
        "  9\n$INSBASE\n 10\n0.0\n 20\n0.0\n 30\n0.0\n"
        "  9\n$HANDSEED\n  5\nFFFF\n  0\nENDSEC\n"
        "  0\nSECTION\n  2\nCLASSES\n"
        "  0\nCLASS\n  1\nLIGHT\n  2\nAcDbLight\n  3\nSCENEOE\n"
        " 90\n     1025\n280\n     0\n281\n     1\n  0\nENDSEC\n"
        "  0\nSECTION\n  2\nENTITIES\n"
        "  0\nLIGHT\n  5\n20\n100\nAcDbEntity\n  8\n0\n100\nAcDbLight\n"
        " 90\n        1\n  1\nSpotlight1\n 70\n     3\n290\n     1\n"
        " 90\n-1023410171\n291\n     0\n 40\n5.4\n"
        " 10\n0.0\n 20\n0.0\n 30\n0.0\n 11\n1.0\n 21\n0.0\n 31\n0.0\n"
        "  0\nENDSEC\n  0\nEOF\n";
  const char *file = "indxf_test_light.dxf";
  const char *dwgfile = "indxf_test_light.dwg";
  Dwg_Data dwg, written;
  Dwg_Object *obj;
  Dwg_Entity_LIGHT *light;
  int error;
  if (import_dxf (file, dxf, &dwg) >= DWG_ERR_CRITICAL)
    {
      dwg_free (&dwg);
      return;
    }
  obj = first_object (&dwg, DWG_TYPE_LIGHT);
  light = obj ? obj->tio.entity->tio.LIGHT : NULL;
  if (!light)
    fail ("r2000 LIGHT: not imported");
  else if (light->class_version != 1)
    fail ("r2000 LIGHT.class_version %u, expected 1",
          (unsigned)light->class_version);
  else if (light->light_color.rgb != 0xc3000005
           || light->light_color.method != 0xc3
           || light->light_color.index != 5)
    fail ("r2000 LIGHT.light_color.rgb 0x%08x, method 0x%x, index %d, "
          "expected 0xc3000005, 0xc3, 5",
          (unsigned)light->light_color.rgb,
          (unsigned)light->light_color.method,
          (int)light->light_color.index);
  else if (!light->name || strcmp (light->name, "Spotlight1"))
    fail ("r2000 LIGHT.name %s, expected Spotlight1",
          light->name ? light->name : "(null)");
  else
    ok ("r2000 LIGHT: class_version 1, light_color 0xc3000005, index 5");

  // as dxf2dwg writes it
  unlink (dwgfile);
  error = dwg_write_file (dwgfile, &dwg);
  dwg_free (&dwg);
  loglevel = test_loglevel;
  if (error >= DWG_ERR_CRITICAL)
    {
      fail ("dwg_write_file %s 0x%x", dwgfile, error);
      return;
    }
  memset (&written, 0, sizeof (Dwg_Data));
  written.opts = tracelevel;
  error = dwg_read_file (dwgfile, &written);
  loglevel = test_loglevel;
  if (error >= DWG_ERR_CRITICAL)
    fail ("dwg_read_file %s 0x%x", dwgfile, error);
  else
    {
      obj = first_object (&written, DWG_TYPE_LIGHT);
      light = obj ? obj->tio.entity->tio.LIGHT : NULL;
      if (!light)
        fail ("r2000 LIGHT: not in %s", dwgfile);
      else if (light->light_color.index != 5)
        fail ("r2000 LIGHT.light_color.index %d in %s, expected 5",
              (int)light->light_color.index, dwgfile);
      else
        ok ("r2000 LIGHT: light_color index 5 in %s", dwgfile);
    }
  dwg_free (&written);
  if (!numfailed ())
    {
      unlink (file);
      unlink (dwgfile);
    }
}

/* LibreDWG writes an r2010+ TABLESTYLE as the table's cell style, then its
   override. The importer reads them back into sty and ovr. */
static void
test_tablestyle_cellstyles (void)
{
  const char *dwgfile = "example_2010.dwg";
  const char *dxffile = "indxf_test_tablestyle.dxf";
  char path[1024];
  Bit_Chain dat = { 0 };
  Dwg_Data dwg, imported;
  Dwg_Object *obj;
  Dwg_Object_TABLESTYLE *ts;
  int error;

  if (!find_test_data (dwgfile, path, sizeof (path)))
    {
      fail ("%s not found", dwgfile);
      return;
    }
  memset (&dwg, 0, sizeof (Dwg_Data));
  dwg.opts = tracelevel;
  error = dwg_read_file (path, &dwg);
  loglevel = test_loglevel;
  if (error >= DWG_ERR_CRITICAL)
    {
      fail ("dwg_read_file %s 0x%x", path, error);
      dwg_free (&dwg);
      return;
    }
  obj = first_object (&dwg, DWG_TYPE_TABLESTYLE);
  if (!obj || !obj->tio.object->tio.TABLESTYLE->numoverrides)
    {
      fail ("%s: no TABLESTYLE with an override", dwgfile);
      dwg_free (&dwg);
      return;
    }

  dat.version = dwg.header.version;
  dat.from_version = dwg.header.from_version;
  dat.opts = dwg.opts;
  unlink (dxffile);
  dat.fh = fopen (dxffile, "wb");
  if (!dat.fh)
    {
      fail ("fopen %s", dxffile);
      dwg_free (&dwg);
      return;
    }
  error = dwg_write_dxf (&dat, &dwg);
  fclose (dat.fh);
  dwg_free (&dwg);
  loglevel = test_loglevel;
  if (error >= DWG_ERR_CRITICAL)
    {
      fail ("dwg_write_dxf %s 0x%x", dxffile, error);
      return;
    }

  // as dxf2dwg, which writes r2000 by default
  memset (&imported, 0, sizeof (Dwg_Data));
  imported.opts = tracelevel;
  imported.header.version = R_2000;
  error = dxf_read_file (dxffile, &imported);
  loglevel = test_loglevel;
  if (error >= DWG_ERR_CRITICAL)
    {
      fail ("dxf_read_file %s 0x%x", dxffile, error);
      dwg_free (&imported);
      return;
    }
  obj = first_object (&imported, DWG_TYPE_TABLESTYLE);
  ts = obj ? obj->tio.object->tio.TABLESTYLE : NULL;
  // example_2010.dwg's TABLESTYLE: the cell style Table, and _TITLE
  if (!ts)
    fail ("%s: no TABLESTYLE", dxffile);
  else if (!ts->sty.name || strcmp (ts->sty.name, "Table"))
    fail ("TABLESTYLE.sty.name %s, expected Table",
          ts->sty.name ? ts->sty.name : "(null)");
  else if (ts->numoverrides != 1 || !ts->ovr.name
           || strcmp (ts->ovr.name, "_TITLE"))
    fail ("TABLESTYLE.numoverrides %u, ovr.name %s, expected 1, _TITLE",
          (unsigned)ts->numoverrides, ts->ovr.name ? ts->ovr.name : "(null)");
  else if (ts->sty.id != 4 || ts->ovr.id != 1)
    fail ("TABLESTYLE.sty.id %u, ovr.id %u, expected 4, 1",
          (unsigned)ts->sty.id, (unsigned)ts->ovr.id);
  else
    ok ("TABLESTYLE: cell style Table and its override _TITLE");
  dwg_free (&imported);
  if (!numfailed ())
    unlink (dxffile);
}
#endif

int
main (void)
{
  char *trace = getenv ("LIBREDWG_TRACE");
  loglevel = test_loglevel = is_make_silent () ? 0 : 2;
  tracelevel = trace ? atoi (trace) : 0;

#ifndef DISABLE_DXF
  test_r12_block_xref_pname ();
  test_mtext_46 ();
  test_light_r2000_color ();
  test_tablestyle_cellstyles ();
#else
  return 77;
#endif
  return numfailed ();
}
