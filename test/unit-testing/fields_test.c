/* ex: ft=c: -*- mode: c; -*- */
/*****************************************************************************/
/*  LibreDWG - free implementation of the DWG file format                    */
/*                                                                           */
/*  Copyright (C) 2026 Free Software Foundation, Inc.                        */
/*                                                                           */
/*  This library is free software, licensed under the terms of the GNU       */
/*  General Public License as published by the Free Software Foundation,     */
/*  either version 3 of the License, or (at your option) any later version.  */
/*  You should have received a copy of the GNU General Public License        */
/*  along with this program.  If not, see <http://www.gnu.org/licenses/>.    */
/*****************************************************************************/

/* Classes whose specs misread AutoCAD's layout, so that written from their
   fields, as a copy writes them, AutoCAD refused the drawing or the object
   had another size than AutoCAD's. In AutoCAD-saved fixtures, each object
   decodes to the fields AutoCAD's DXF of the same drawing shows. Every object
   of these classes, written from its fields as its own DWG version and read
   again, has AutoCAD's object size and data stream size, and for the classes
   LibreDWG keeps the raw bits of, AutoCAD's bits. Classes not yet written as
   AutoCAD writes them are TODOs that name the follow-up task fixing them. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int tracelevel;
// the library sets loglevel from each document's opts
static int test_loglevel;

#include "config.h"
#ifdef HAVE_UNISTD_H
#  include <unistd.h>
#endif
#include "dwg.h"
#include "tests_common.h"
#include "dwg_api.h"
#include "bits.h"
#include "out_dxf.h"

#define MAX_FIXTURE_CLASSES 6

typedef struct
{
  const char *name;
  unsigned count; // objects of the class in the fixture
  /* Not yet written as AutoCAD writes it: the follow-up task that fixes it
     and turns its TODO back into a strict check. */
  const char *todo;
} Fixture_Class;

typedef struct
{
  const char *file; // under test/test-data
  Fixture_Class classes[MAX_FIXTURE_CLASSES];
} Fixture_File;

/* ASSOC2DCONSTRAINTGROUP: the decoder can't read its constraint graph.
   ASSOCGEOMDEPENDENCY: an AcDbAssocSimplePersSubentId's three values (DXF
   90 90 90) aren't read. */
#define TODO_CONSTRAINTGROUP "P3-07-FU1"
#define TODO_GEOMDEPENDENCY "P3-07-FU3"

static const Fixture_File fixture_files[] = {
  { "example_2004.dwg",
    { { "ASSOCOSNAPPOINTREFACTIONPARAM", 1, NULL },
      { "3DSOLID", 1, NULL },
      { "REGION", 2, NULL },
      { "ASSOCGEOMDEPENDENCY", 1, TODO_GEOMDEPENDENCY } } },
  { "example_2000.dwg",
    { { "ASSOCOSNAPPOINTREFACTIONPARAM", 1, NULL },
      { "3DSOLID", 1, NULL },
      { "REGION", 2, NULL } } },
  { "2004/Surface.dwg",
    { { "ASSOCREVOLVEDSURFACEACTIONBODY", 1, NULL },
      { "ASSOCPLANESURFACEACTIONBODY", 1, NULL },
      { "PLANESURFACE", 1, NULL } } },
  { "2004/Underlay.dwg", { { "PDFUNDERLAY", 3, NULL } } },
  { "2004/Dynblocks.dwg",
    { { "BLOCKSCALEACTION", 1, NULL },
      { "ASSOCVALUEDEPENDENCY", 18, NULL },
      { "ASSOCVARIABLE", 22, NULL },
      { "ASSOC2DCONSTRAINTGROUP", 2, TODO_CONSTRAINTGROUP },
      { "ASSOCGEOMDEPENDENCY", 28, TODO_GEOMDEPENDENCY } } },
  { "2004/Constraints.dwg",
    { { "ASSOC2DCONSTRAINTGROUP", 1, TODO_CONSTRAINTGROUP } } },
};
#define NUM_FIXTURE_FILES (sizeof (fixture_files) / sizeof (fixture_files[0]))

/* The objects whose fields are checked against AutoCAD's DXF. */
typedef struct
{
  const char *file;
  BITCODE_RLL handle;
  const char *name;
} Fixture_Object;

static const Fixture_Object fixture_objects[] = {
  { "example_2004.dwg", 0x393, "ASSOCOSNAPPOINTREFACTIONPARAM" },
  { "example_2000.dwg", 0x393, "ASSOCOSNAPPOINTREFACTIONPARAM" },
  { "2004/Surface.dwg", 0x368, "ASSOCREVOLVEDSURFACEACTIONBODY" },
  { "2004/Surface.dwg", 0x50A, "PLANESURFACE" },
  { "2004/Surface.dwg", 0x50C, "ASSOCPLANESURFACEACTIONBODY" },
  { "2004/Underlay.dwg", 0x276, "PDFUNDERLAY" },
  { "2004/Underlay.dwg", 0x405, "PDFUNDERLAY" },
  { "2004/Underlay.dwg", 0x406, "PDFUNDERLAY" },
  // Door - Imperial's scale action
  { "2004/Dynblocks.dwg", 0xBAE6, "BLOCKSCALEACTION" },
  // Door Elevation's d4 constraint: the variable and its value dependency
  { "2004/Dynblocks.dwg", 0xBB10, "ASSOCVARIABLE" },
  { "2004/Dynblocks.dwg", 0xBB1C, "ASSOCVALUEDEPENDENCY" },
};
#define NUM_FIXTURE_OBJECTS                                                   \
  (sizeof (fixture_objects) / sizeof (fixture_objects[0]))

/* What an object looked like in the fixture, before it was written. */
typedef struct
{
  BITCODE_RLL handle;
  const Fixture_Class *klass;
  BITCODE_RL size;
  BITCODE_RL bitsize;
  BITCODE_RL num_unknown_bits;
  BITCODE_TF unknown_bits;
} Source_Object;

static int
find_fixture (const char *name, char *path, size_t size)
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

static int
read_dwg (const char *file, Dwg_Data *dwg)
{
  int error;
  memset (dwg, 0, sizeof (Dwg_Data));
  dwg->opts = tracelevel;
  error = dwg_read_file (file, dwg);
  loglevel = test_loglevel;
  return error;
}

static void
free_dwg (Dwg_Data *dwg)
{
  loglevel = tracelevel;
  dwg_free (dwg);
  loglevel = test_loglevel;
}

static int
write_dwg (const char *file, Dwg_Data *dwg)
{
  int error;
  // dwg_write_file refuses to overwrite a file
  unlink (file);
  loglevel = tracelevel;
  error = dwg_write_file (file, dwg);
  loglevel = test_loglevel;
  return error;
}

static Dwg_Object *
find_object (Dwg_Data *dwg, BITCODE_RLL handle, const char *name)
{
  Dwg_Object *obj = dwg_resolve_handle (dwg, handle);
  loglevel = test_loglevel;
  if (!obj || !obj->name || strcmp (obj->name, name))
    return NULL;
  return obj;
}

/* Bit i of raw bits as bit_read_bits stores them: MSB first, the bits of a
   last partial byte too. */
static int
bit_at (const BITCODE_TF bits, BITCODE_RL i)
{
  return (bits[i / 8] >> (7 - i % 8)) & 1;
}

/* Raw bits run to the end of the object's last byte. AutoCAD pads the bits
   after the handle stream with ones, LibreDWG with zeros: the bits are the
   same if they only differ there. */
static int
same_bits_but_padding (const BITCODE_TF autocad, const BITCODE_TF libredwg,
                       BITCODE_RL num_bits)
{
  BITCODE_RL i = 0;
  while (i < num_bits && bit_at (autocad, i) == bit_at (libredwg, i))
    i++;
  if (num_bits - i > 7)
    return 0;
  for (; i < num_bits; i++)
    if (!bit_at (autocad, i) || bit_at (libredwg, i))
      return 0;
  return 1;
}

static BITCODE_RLL
ref_value (const BITCODE_H ref)
{
  return ref ? ref->absolute_ref : 0;
}

/* The fields of each object as AutoCAD's DXF of its drawing has them
   (test-data/example_2004.dxf, 2004/Surface.dxf, 2004/Underlay.dxf,
   2018/Dynblocks.dxf). */
static void
check_fields (const char *file, const Dwg_Object *obj)
{
  const char *what = file;
  if (obj->fixedtype == DWG_TYPE_ASSOCOSNAPPOINTREFACTIONPARAM)
    {
      const Dwg_Object_ASSOCOSNAPPOINTREFACTIONPARAM *o
          = obj->tio.object->tio.ASSOCOSNAPPOINTREFACTIONPARAM;
      // 90 0, 90 0, 330 0 after the params, then 90 0, 90 1, 40 -1.0
      if (o->num_params == 1 && ref_value (o->params[0]) == 0x394
          && o->child_status == 0 && o->child_id == 0
          && ref_value (o->child_param) == 0 && o->status == 0
          && o->osnap_mode == 1 && o->param == -1.0)
        ok ("%s: %s " FORMAT_HV " fields", what, obj->name,
            obj->handle.value);
      else
        fail ("%s: %s " FORMAT_HV " child_status %u child_id %u status %u "
              "osnap_mode %u param %g",
              what, obj->name, obj->handle.value, (unsigned)o->child_status,
              (unsigned)o->child_id, (unsigned)o->status,
              (unsigned)o->osnap_mode, o->param);
    }
  else if (obj->fixedtype == DWG_TYPE_ASSOCREVOLVEDSURFACEACTIONBODY)
    {
      const Dwg_Object_ASSOCREVOLVEDSURFACEACTIONBODY *o
          = obj->tio.object->tio.ASSOCREVOLVEDSURFACEACTIONBODY;
      // the surface body's 90 4, then 90 0 and 290 0 at the end
      if (o->pab.num_values == 1 && o->sab.l2 == 4
          && ref_value (o->sab.assocdep) == 0x369 && o->class_version == 0
          && o->b1 == 0)
        ok ("%s: %s " FORMAT_HV " fields", what, obj->name,
            obj->handle.value);
      else
        fail ("%s: %s " FORMAT_HV " num_values %u sab.l2 %u class_version %u",
              what, obj->name, obj->handle.value,
              (unsigned)o->pab.num_values, (unsigned)o->sab.l2,
              (unsigned)o->class_version);
    }
  else if (obj->fixedtype == DWG_TYPE_ASSOCPLANESURFACEACTIONBODY)
    {
      const Dwg_Object_ASSOCPLANESURFACEACTIONBODY *o
          = obj->tio.object->tio.ASSOCPLANESURFACEACTIONBODY;
      // no values, then the surface body: 90 0, 330 50D, 290 0, 90 7
      if (o->pab.num_values == 0 && o->sab.version == 0
          && ref_value (o->sab.assocdep) == 0x50D && o->sab.l2 == 7
          && o->pbsab_status == 0 && o->class_version == 0)
        ok ("%s: %s " FORMAT_HV " fields", what, obj->name,
            obj->handle.value);
      else
        fail ("%s: %s " FORMAT_HV " num_values %u sab.version %u sab.l2 %u",
              what, obj->name, obj->handle.value,
              (unsigned)o->pab.num_values, (unsigned)o->sab.version,
              (unsigned)o->sab.l2);
    }
  else if (obj->fixedtype == DWG_TYPE_PLANESURFACE)
    {
      const Dwg_Entity_PLANESURFACE *o = obj->tio.entity->tio.PLANESURFACE;
      // 100 AcDbSurface, 71 6, 72 6
      if (o->u_isolines == 6 && o->v_isolines == 6)
        ok ("%s: %s " FORMAT_HV " isolines", what, obj->name,
            obj->handle.value);
      else
        fail ("%s: %s " FORMAT_HV " isolines %u %u", what, obj->name,
              obj->handle.value, (unsigned)o->u_isolines,
              (unsigned)o->v_isolines);
    }
  else if (obj->fixedtype == DWG_TYPE_BLOCKSCALEACTION)
    {
      const Dwg_Object_BLOCKSCALEACTION *o
          = obj->tio.object->tio.BLOCKSCALEACTION;
      // AcDbBlockActionWithBasePt's 92-93 and 301-302, then 94-96 and
      // 303-305 (test-data/2018/Dynblocks.dxf)
      const char *const names[5]
          = { "UpdatedBaseX", "UpdatedBaseY", "Scale", "XScale", "YScale" };
      int same = o->xy_type == 0;
      for (int i = 0; i < 5; i++)
        if (o->conn_pts[i].code != 1 || !o->conn_pts[i].name
            || strcmp (o->conn_pts[i].name, names[i]))
          same = 0;
      if (same)
        ok ("%s: %s " FORMAT_HV " connection points", what, obj->name,
            obj->handle.value);
      else
        fail ("%s: %s " FORMAT_HV " connection points %s %s %s %s %s, "
              "xy_type %u",
              what, obj->name, obj->handle.value,
              o->conn_pts[0].name ? o->conn_pts[0].name : "NULL",
              o->conn_pts[1].name ? o->conn_pts[1].name : "NULL",
              o->conn_pts[2].name ? o->conn_pts[2].name : "NULL",
              o->conn_pts[3].name ? o->conn_pts[3].name : "NULL",
              o->conn_pts[4].name ? o->conn_pts[4].name : "NULL",
              (unsigned)o->xy_type);
    }
  else if (obj->fixedtype == DWG_TYPE_ASSOCVARIABLE)
    {
      const Dwg_Object_ASSOCVARIABLE *o = obj->tio.object->tio.ASSOCVARIABLE;
      // 1 d4, 1 4, 1 AcDbCalc:1.0, 1 "", 90 4, then 290 0, 290 0, 90 0
      if (o->name && !strcmp (o->name, "d4") && o->value.code == 90
          && o->value.u.bl == 4 && !o->has_t78 && !o->t78 && !o->b290
          && o->bl90 == 0)
        ok ("%s: %s " FORMAT_HV " fields", what, obj->name,
            obj->handle.value);
      else
        fail ("%s: %s " FORMAT_HV " name %s value %u/%u has_t78 %u b290 %u",
              what, obj->name, obj->handle.value,
              o->name ? o->name : "NULL", (unsigned)o->value.code,
              (unsigned)o->value.u.bl, (unsigned)o->has_t78,
              (unsigned)o->b290);
    }
  else if (obj->fixedtype == DWG_TYPE_ASSOCVALUEDEPENDENCY)
    {
      const Dwg_Object_ASSOCVALUEDEPENDENCY *o
          = obj->tio.object->tio.ASSOCVALUEDEPENDENCY;
      // after the dependency: 90 0, 1 "", 90 4, the value of d4
      if (ref_value (o->assocdep.dep_on) == 0xBB10 && o->class_version == 0
          && o->value_name && !*o->value_name && o->value.code == 90
          && o->value.u.bl == 4)
        ok ("%s: %s " FORMAT_HV " fields", what, obj->name,
            obj->handle.value);
      else
        fail ("%s: %s " FORMAT_HV " class_version %u value %u/%u", what,
              obj->name, obj->handle.value, (unsigned)o->class_version,
              (unsigned)o->value.code, (unsigned)o->value.u.bl);
    }
  else if (obj->fixedtype == DWG_TYPE_PDFUNDERLAY)
    {
      const Dwg_Entity_PDFUNDERLAY *o = obj->tio.entity->tio.PDFUNDERLAY;
      // flag 280 with 16 set (clip inverted), the clip boundary, no 170
      if ((o->flag & 16) && o->num_clip_verts >= 6
          && o->num_clip_inverts == 0 && !o->clip_inverts)
        ok ("%s: %s " FORMAT_HV " flag %u, %u clip vertices", what,
            obj->name, obj->handle.value, (unsigned)o->flag,
            (unsigned)o->num_clip_verts);
      else
        fail ("%s: %s " FORMAT_HV " flag %u, %u clip vertices, %u inverts",
              what, obj->name, obj->handle.value, (unsigned)o->flag,
              (unsigned)o->num_clip_verts, (unsigned)o->num_clip_inverts);
    }
}

static const Fixture_Class *
fixture_class (const Fixture_File *f, const char *name)
{
  for (unsigned c = 0; c < MAX_FIXTURE_CLASSES && f->classes[c].name; c++)
    if (name && !strcmp (f->classes[c].name, name))
      return &f->classes[c];
  return NULL;
}

/* Reads one fixture: its checked objects decode to AutoCAD's fields. Then
   writes every object of the fixture's classes from its fields and reads them
   again: AutoCAD's sizes, and AutoCAD's raw bits where the fixture has them,
   for each one. */
static int
test_file (const Fixture_File *f, unsigned index)
{
  char path[512], out[64];
  Source_Object *source;
  unsigned num_source = 0, num_class[MAX_FIXTURE_CLASSES] = { 0 };
  unsigned num_same[MAX_FIXTURE_CLASSES] = { 0 };
  unsigned num_size = 0, num_raw = 0;
  Dwg_Data dwg, read;
  int error, read_error;

  failed = 0;
  if (!find_fixture (f->file, path, sizeof (path)))
    {
      fail ("%s not found", f->file);
      return 1;
    }
  read_error = read_dwg (path, &dwg);
  if (read_error >= DWG_ERR_CRITICAL)
    {
      fail ("%s: dwg_read_file 0x%x", f->file, read_error);
      free_dwg (&dwg);
      return numfailed ();
    }
  for (unsigned i = 0; i < NUM_FIXTURE_OBJECTS; i++)
    {
      const Fixture_Object *o = &fixture_objects[i];
      Dwg_Object *obj;
      if (strcmp (o->file, f->file))
        continue;
      obj = find_object (&dwg, o->handle, o->name);
      if (obj)
        check_fields (f->file, obj);
      else
        fail ("%s: no %s " FORMAT_HV, f->file, o->name, o->handle);
    }

  source = (Source_Object *)calloc (dwg.num_objects, sizeof (Source_Object));
  if (!source)
    {
      fail ("%s: calloc", f->file);
      free_dwg (&dwg);
      return numfailed ();
    }
  for (BITCODE_BL i = 0; i < dwg.num_objects; i++)
    {
      Dwg_Object *obj = &dwg.object[i];
      const Fixture_Class *c = fixture_class (f, obj->name);
      Source_Object *s;
      if (!c)
        continue;
      num_class[c - f->classes]++;
      s = &source[num_source++];
      s->handle = obj->handle.value;
      s->klass = c;
      s->size = obj->size;
      s->bitsize = obj->bitsize;
      // written from the fields: the encoder writes raw bits back as they are
      s->num_unknown_bits = obj->num_unknown_bits;
      s->unknown_bits = obj->unknown_bits;
      obj->unknown_bits = NULL;
      obj->num_unknown_bits = 0;
    }
  for (unsigned c = 0; c < MAX_FIXTURE_CLASSES && f->classes[c].name; c++)
    if (num_class[c] != f->classes[c].count)
      fail ("%s: %u %s, expected %u", f->file, num_class[c],
            f->classes[c].name, f->classes[c].count);

  snprintf (out, sizeof (out), "fields_test_%u.dwg", index);
  error = write_dwg (out, &dwg);
  free_dwg (&dwg);
  if (error >= DWG_ERR_CRITICAL)
    {
      fail ("%s: dwg_write_file 0x%x", f->file, error);
      for (unsigned i = 0; i < num_source; i++)
        free (source[i].unknown_bits);
      free (source);
      return numfailed ();
    }
  error = read_dwg (out, &read);
  if (error >= DWG_ERR_CRITICAL || (error & ~read_error))
    fail ("%s: written, dwg_read_file 0x%x, the fixture 0x%x", f->file,
          error, read_error);
  else
    ok ("%s: written and read with no new errors", f->file);
  if (error < DWG_ERR_CRITICAL)
    {
      for (unsigned i = 0; i < NUM_FIXTURE_OBJECTS; i++)
        {
          const Fixture_Object *o = &fixture_objects[i];
          Dwg_Object *obj;
          if (strcmp (o->file, f->file))
            continue;
          obj = find_object (&read, o->handle, o->name);
          if (obj)
            check_fields (out, obj);
          else
            fail ("%s: written, no %s " FORMAT_HV, f->file, o->name,
                  o->handle);
        }
      for (unsigned i = 0; i < num_source; i++)
        {
          const Source_Object *s = &source[i];
          const Fixture_Class *c = s->klass;
          Dwg_Object *obj = find_object (&read, s->handle, c->name);
          int same_size, same_bits;
          if (!obj)
            {
              fail ("%s: written, no %s " FORMAT_HV, f->file, c->name,
                    s->handle);
              continue;
            }
          same_size = obj->size == s->size && obj->bitsize == s->bitsize;
          same_bits = !s->num_unknown_bits
                      || (obj->num_unknown_bits == s->num_unknown_bits
                          && obj->unknown_bits
                          && same_bits_but_padding (s->unknown_bits,
                                                    obj->unknown_bits,
                                                    s->num_unknown_bits));
          if (same_size && same_bits)
            {
              num_same[c - f->classes]++;
              if (!c->todo)
                {
                  num_size++;
                  if (s->num_unknown_bits)
                    num_raw++;
                }
            }
          else if (c->todo)
            todo ("%s: %s " FORMAT_HV " size %u, bitsize %u; AutoCAD's %u, "
                  "%u (%s)",
                  f->file, c->name, s->handle, (unsigned)obj->size,
                  (unsigned)obj->bitsize, (unsigned)s->size,
                  (unsigned)s->bitsize, c->todo);
          else if (!same_size)
            fail ("%s: %s " FORMAT_HV " size %u, bitsize %u; AutoCAD's %u, "
                  "%u",
                  f->file, c->name, s->handle, (unsigned)obj->size,
                  (unsigned)obj->bitsize, (unsigned)s->size,
                  (unsigned)s->bitsize);
          else
            fail ("%s: %s " FORMAT_HV " bits differ from AutoCAD's", f->file,
                  c->name, s->handle);
        }
      for (unsigned c = 0; c < MAX_FIXTURE_CLASSES && f->classes[c].name;
           c++)
        if (f->classes[c].todo && num_class[c]
            && num_same[c] == num_class[c])
          fail ("%s: every %s matches AutoCAD's now: make its check strict "
                "(%s)",
                f->file, f->classes[c].name, f->classes[c].todo);
      if (num_size)
        ok ("%s: %u objects with AutoCAD's sizes, %u of them with AutoCAD's "
            "raw bits",
            f->file, num_size, num_raw);
    }
  free_dwg (&read);
  for (unsigned i = 0; i < num_source; i++)
    free (source[i].unknown_bits);
  free (source);
  if (!numfailed ())
    unlink (out);
  return numfailed ();
}

#ifndef DISABLE_DXF

static const Dwg_Object *
find_in (const Dwg_Data *dwg, BITCODE_RLL handle, const char *name)
{
  for (BITCODE_BL i = 0; i < dwg->num_objects; i++)
    if (dwg->object[i].handle.value == handle)
      return dwg->object[i].name && !strcmp (dwg->object[i].name, name)
                 ? &dwg->object[i]
                 : NULL;
  return NULL;
}

/* LibreDWG's DXF of 2004/Dynblocks.dwg reads back with its block actions'
   connection points, which it writes with AutoCAD's group codes:
   BLOCKROTATEACTION's third point as 94 and 303, BLOCKSCALEACTION's three
   as 94-96 and 303-305. */
static int
test_dxf_roundtrip (void)
{
  const char *name = "2004/Dynblocks.dwg";
  const char *out = "fields_test_dxf.dxf";
  char path[512];
  Dwg_Data dwg, dxf;
  Bit_Chain dat = { 0 };
  const Dwg_Object *obj;
  int error;

  failed = 0;
  if (!find_fixture (name, path, sizeof (path)))
    {
      fail ("%s not found", name);
      return 1;
    }
  error = read_dwg (path, &dwg);
  if (error >= DWG_ERR_CRITICAL)
    {
      fail ("dxf: %s: dwg_read_file 0x%x", name, error);
      free_dwg (&dwg);
      return numfailed ();
    }
  unlink (out);
  dat.fh = fopen (out, "wb");
  if (!dat.fh)
    {
      fail ("dxf: fopen %s", out);
      free_dwg (&dwg);
      return numfailed ();
    }
  dat.version = dwg.header.version;
  dat.from_version = dwg.header.from_version;
  dat.codepage = dwg.header.codepage;
  dat.opts = dwg.opts;
  loglevel = tracelevel;
  error = dwg_write_dxf (&dat, &dwg);
  loglevel = test_loglevel;
  fclose (dat.fh);
  free_dwg (&dwg);
  if (error >= DWG_ERR_CRITICAL)
    {
      fail ("dxf: %s: dwg_write_dxf 0x%x", name, error);
      return numfailed ();
    }

  memset (&dxf, 0, sizeof (dxf));
  dxf.opts = tracelevel;
  error = dxf_read_file (out, &dxf);
  loglevel = test_loglevel;
  if (error >= DWG_ERR_CRITICAL)
    {
      fail ("dxf: %s: dxf_read_file 0x%x", out, error);
      free_dwg (&dxf);
      return numfailed ();
    }
  // Door - Imperial's scale action, and Door Elevation's rotate action
  obj = find_in (&dxf, 0xBAE6, "BLOCKSCALEACTION");
  if (obj)
    check_fields (out, obj);
  else
    fail ("dxf: no BLOCKSCALEACTION BAE6");
  obj = find_in (&dxf, 0xB90C, "BLOCKROTATEACTION");
  if (obj)
    {
      const Dwg_Object_BLOCKROTATEACTION *o
          = obj->tio.object->tio.BLOCKROTATEACTION;
      if (o->conn_pts[2].code == 5 && o->conn_pts[2].name
          && !strcmp (o->conn_pts[2].name, "AngleDelta"))
        ok ("%s: BLOCKROTATEACTION B90C connection point 5 AngleDelta", out);
      else
        fail ("%s: BLOCKROTATEACTION B90C connection point %u %s", out,
              (unsigned)o->conn_pts[2].code,
              o->conn_pts[2].name ? o->conn_pts[2].name : "NULL");
    }
  else
    fail ("dxf: no BLOCKROTATEACTION B90C");
  free_dwg (&dxf);
  if (!numfailed ())
    unlink (out);
  return numfailed ();
}
#endif

int
main (int argc, char *argv[])
{
  int error = 0;
  char *trace = getenv ("LIBREDWG_TRACE");
  test_loglevel = is_make_silent () ? 0 : 2; // print ok
  tracelevel = trace ? atoi (trace) : 0;
  loglevel = test_loglevel;
  (void)argc;
  (void)argv;

  for (unsigned i = 0; i < NUM_FIXTURE_FILES; i++)
    error += test_file (&fixture_files[i], i);
#ifndef DISABLE_DXF
  error += test_dxf_roundtrip ();
#endif
  return error ? 1 : 0;
}
