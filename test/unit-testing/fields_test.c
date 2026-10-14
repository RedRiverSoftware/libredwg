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

/* Objects written from their fields, as a copy writes them, against the
   objects AutoCAD wrote. A class whose spec misreads AutoCAD's layout writes
   its objects at another size than AutoCAD's, and AutoCAD can refuse such a
   drawing.

   Every object of every r2004 drawing in test-data/2004, and of
   example_2004, all saved by AutoCAD, is written from its fields, its EED
   from its items, as r2004, and read again. Each must have AutoCAD's object
   size and data stream size, and the objects LibreDWG keeps the raw bits of
   must have AutoCAD's bits. Objects of classes LibreDWG doesn't decode have
   no fields to write (a copy refuses them): they keep their raw bits, which
   must come back as AutoCAD's. example_2000 (r2000) is checked for its
   listed classes only: AutoCAD's r2000 strings end with a NUL that LibreDWG
   doesn't write, so its other objects are a byte shorter per string.
   AutoCAD reads both.

   The objects in fixture_objects, of classes whose specs misread AutoCAD's
   layout, must also decode to the fields AutoCAD's DXF of the same drawing
   shows, before and after the write.

   Differences not fixed yet are TODOs that name the follow-up task fixing
   them. The test fails once one goes away, so that its TODO is removed. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <sys/stat.h>
#ifndef _MSC_VER
#  include <dirent.h>
#endif

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

/* ASSOC2DCONSTRAINTGROUP: the decoder can't read its constraint graph. */
#define TODO_CONSTRAINTGROUP "P3-07-FU1"
/* ASSOCGEOMDEPENDENCY: the persistent subentity id after the dependency
   isn't read: the three 90s of example_2004's AcDbAssocSimplePersSubentId,
   and of the AcDbAssocEdgePersSubentIds in 2004/Dynblocks.dwg. Its objects
   with an AcDbAssocSingleEdgePersSubentId, which has no values, match. */
#define TODO_GEOMDEPENDENCY "P3-07-FU3"
/* bit_write_DD writes a value within 1e-12 of its default as the default,
   where AutoCAD keeps the value: LINE and LWPOLYLINE coordinates. An object
   that doesn't match AutoCAD's must be shorter by exactly the bits that
   costs AutoCAD (dd_rounded_bits). */
#define TODO_DD_ROUNDING "P3-08-FU1"

#define MAX_FIXTURE_CLASSES 8

typedef enum
{
  CLASS_CHECKED,  // r2000: the objects of the listed classes are compared
  CLASS_TODO,     // not yet written as AutoCAD writes it
  CLASS_UNDECODED // LibreDWG doesn't decode it: written from its raw bits
} Class_Kind;

typedef struct
{
  const char *name; // CLASS_UNDECODED: the class's DXF name
  unsigned count;   // objects of the class in the drawing
  Class_Kind kind;
  /* CLASS_TODO: the follow-up task that fixes it and turns its TODO back
     into a strict check. */
  const char *todo;
} Fixture_Class;

/* Which objects of a fixture are written from their fields and compared. */
#define EVERY_OBJECT 1
#define LISTED_CLASSES 0 // those of the classes listed for the fixture

typedef struct
{
  const char *file;     // under test/test-data
  int objects;          // EVERY_OBJECT (r2004) or LISTED_CLASSES (r2000)
  unsigned num_objects; // EVERY_OBJECT: in the drawing
  /* Objects written from their fields that LibreDWG keeps raw bits of, so
     that the bit comparison can't quietly become a size comparison. */
  unsigned num_raw_bits;
  // LINE and LWPOLYLINE objects shorter than AutoCAD's: TODO_DD_ROUNDING
  unsigned num_dd_rounded;
  Fixture_Class classes[MAX_FIXTURE_CLASSES];
} Fixture_File;

// a fixture with no listed classes
#define NO_CLASSES { { NULL, 0, CLASS_CHECKED, NULL } }

/* Every drawing in test-data/2004 must be here (test_fixture_list). */
static const Fixture_File fixture_files[] = {
  { "example_2004.dwg",
    EVERY_OBJECT,
    735,
    18,
    6,
    { { "ASSOCGEOMDEPENDENCY", 1, CLASS_TODO, TODO_GEOMDEPENDENCY },
      { "ACDBASSOCPERSSUBENTMANAGER", 1, CLASS_UNDECODED, NULL },
      { "ACDBPERSSUBENTMANAGER", 1, CLASS_UNDECODED, NULL },
      { "ACSH_SWEEP_CLASS", 1, CLASS_UNDECODED, NULL },
      { "ACDBASSOCALIGNEDDIMACTIONBODY", 1, CLASS_UNDECODED, NULL },
      { "ACAD_TABLE", 1, CLASS_UNDECODED, NULL },
      { "TABLECONTENT", 1, CLASS_UNDECODED, NULL },
      { "DATATABLE", 1, CLASS_UNDECODED, NULL } } },
  { "example_2000.dwg",
    LISTED_CLASSES,
    0,
    1,
    0,
    { { "ASSOCOSNAPPOINTREFACTIONPARAM", 1, CLASS_CHECKED, NULL },
      { "3DSOLID", 1, CLASS_CHECKED, NULL },
      { "REGION", 2, CLASS_CHECKED, NULL } } },
  { "2004/Arc.dwg", EVERY_OBJECT, 215, 1, 0, NO_CLASSES },
  { "2004/Constraints.dwg",
    EVERY_OBJECT,
    227,
    2,
    0,
    { { "ASSOC2DCONSTRAINTGROUP", 1, CLASS_TODO, TODO_CONSTRAINTGROUP } } },
  { "2004/ConstructionLine.dwg", EVERY_OBJECT, 216, 1, 0, NO_CLASSES },
  { "2004/Donut.dwg", EVERY_OBJECT, 215, 1, 0, NO_CLASSES },
  { "2004/Dynblocks.dwg",
    EVERY_OBJECT,
    51451,
    135,
    257,
    { { "ASSOC2DCONSTRAINTGROUP", 2, CLASS_TODO, TODO_CONSTRAINTGROUP },
      { "ASSOCGEOMDEPENDENCY", 28, CLASS_TODO, TODO_GEOMDEPENDENCY },
      { "ACDB_DYNAMICBLOCKPROXYNODE", 1, CLASS_UNDECODED, NULL } } },
  { "2004/Ellipse.dwg", EVERY_OBJECT, 215, 1, 0, NO_CLASSES },
  { "2004/HatchG.dwg", EVERY_OBJECT, 229, 1, 0, NO_CLASSES },
  { "2004/Helix.dwg", EVERY_OBJECT, 215, 2, 0, NO_CLASSES },
  { "2004/Leader.dwg", EVERY_OBJECT, 251, 3, 0, NO_CLASSES },
  { "2004/Line.dwg", EVERY_OBJECT, 215, 1, 0, NO_CLASSES },
  { "2004/Multiline.dwg", EVERY_OBJECT, 215, 1, 0, NO_CLASSES },
  { "2004/Point.dwg", EVERY_OBJECT, 216, 1, 0, NO_CLASSES },
  { "2004/PolyLine3D.dwg", EVERY_OBJECT, 222, 1, 0, NO_CLASSES },
  { "2004/Polygon.dwg", EVERY_OBJECT, 215, 1, 0, NO_CLASSES },
  { "2004/Polyline.dwg", EVERY_OBJECT, 215, 1, 0, NO_CLASSES },
  { "2004/RAY.dwg", EVERY_OBJECT, 215, 1, 0, NO_CLASSES },
  { "2004/Spline.dwg", EVERY_OBJECT, 215, 1, 0, NO_CLASSES },
  { "2004/Surface.dwg",
    EVERY_OBJECT,
    287,
    27,
    0,
    { { "EXTRUDEDSURFACE", 1, CLASS_UNDECODED, NULL },
      { "LOFTEDSURFACE", 1, CLASS_UNDECODED, NULL },
      { "REVOLVEDSURFACE", 1, CLASS_UNDECODED, NULL },
      { "SWEPTSURFACE", 1, CLASS_UNDECODED, NULL },
      { "ACDBASSOCPERSSUBENTMANAGER", 1, CLASS_UNDECODED, NULL },
      { "ACDBASSOCEDGEACTIONPARAM", 7, CLASS_UNDECODED, NULL },
      { "ACDBASSOCSWEPTSURFACEACTIONBODY", 1, CLASS_UNDECODED, NULL } } },
  { "2004/Text.dwg", EVERY_OBJECT, 216, 1, 0, NO_CLASSES },
  { "2004/Underlay.dwg", EVERY_OBJECT, 217, 1, 0, NO_CLASSES },
  { "2004/circle.dwg", EVERY_OBJECT, 215, 1, 0, NO_CLASSES },
  { "2004/material.dwg", EVERY_OBJECT, 264, 1, 0, NO_CLASSES },
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
  Dwg_Object_Type fixedtype;
  char name[48];
  const Fixture_Class *klass; // NULL if not listed
  BITCODE_RL size;
  BITCODE_RL bitsize;
  BITCODE_RL num_unknown_bits;
  BITCODE_TF unknown_bits;
  unsigned dd_rounded_bits;
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

/* The bits AutoCAD spends on a DD value that bit_write_DD writes as its
   default, 2 bits, because it is within 1e-12 of it (TODO_DD_ROUNDING).
   AutoCAD patches the default's low 4 or 6 bytes, or writes the whole
   double, as bit_write_DD does for a value further away. */
static unsigned
dd_rounded_bits (double value, double dflt)
{
  uint64_t v, d;
  if (!memcmp (&value, &dflt, sizeof (double))
      || !(fabs (value - dflt) < 1e-12))
    return 0;
  memcpy (&v, &value, sizeof (v));
  memcpy (&d, &dflt, sizeof (d));
  if (v >> 32 == d >> 32)
    return 32;
  if (v >> 48 == d >> 48)
    return 48;
  return 64;
}

/* The bits by which LibreDWG writes an r2000+ object shorter than AutoCAD,
   from the DD values it rounds to their default. */
static unsigned
object_dd_rounded_bits (const Dwg_Object *obj)
{
  unsigned bits = 0;
  if (obj->fixedtype == DWG_TYPE_LINE)
    {
      const Dwg_Entity_LINE *o = obj->tio.entity->tio.LINE;
      bits += dd_rounded_bits (o->end.x, o->start.x);
      bits += dd_rounded_bits (o->end.y, o->start.y);
      // the encoder leaves out both z when they are 0.0
      if (o->start.z != 0.0 || o->end.z != 0.0)
        bits += dd_rounded_bits (o->end.z, o->start.z);
    }
  else if (obj->fixedtype == DWG_TYPE_LWPOLYLINE)
    {
      const Dwg_Entity_LWPOLYLINE *o = obj->tio.entity->tio.LWPOLYLINE;
      // each point after the first defaults to the one before
      for (BITCODE_BL i = 1; o->points && i < o->num_points; i++)
        {
          bits += dd_rounded_bits (o->points[i].x, o->points[i - 1].x);
          bits += dd_rounded_bits (o->points[i].y, o->points[i - 1].y);
        }
    }
  return bits;
}

static BITCODE_RLL
ref_value (const BITCODE_H ref)
{
  return ref ? ref->absolute_ref : 0;
}

/* The fields of each object as AutoCAD's DXF of its drawing has them
   (test-data/example_2004.dxf, example_2000.dxf, 2004/Surface.dxf,
   2004/Underlay.dxf, 2018/Dynblocks.dxf). */
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

static int
is_undecoded (const Dwg_Object *obj)
{
  return obj->fixedtype == DWG_TYPE_UNKNOWN_OBJ
         || obj->fixedtype == DWG_TYPE_UNKNOWN_ENT;
}

/* An object's class name: for one LibreDWG doesn't decode, its DXF name. */
static const char *
class_name (const Dwg_Data *dwg, const Dwg_Object *obj)
{
  if (is_undecoded (obj))
    {
      if (obj->type >= 500 && obj->type - 500u < dwg->num_classes
          && dwg->dwg_class[obj->type - 500].dxfname)
        return dwg->dwg_class[obj->type - 500].dxfname;
      return "?";
    }
  return obj->name ? obj->name : "?";
}

static const Fixture_Class *
fixture_class (const Fixture_File *f, const char *name, int undecoded)
{
  for (unsigned c = 0; c < MAX_FIXTURE_CLASSES && f->classes[c].name; c++)
    if (!strcmp (f->classes[c].name, name)
        && (f->classes[c].kind == CLASS_UNDECODED) == undecoded)
      return &f->classes[c];
  return NULL;
}

/* Clears an object's raw bits and the raw bytes of its EED, as a copy does,
   so that the encoder writes it from its fields and its EED from its items.
   Returns 0 if part of it has nothing else to be written from. */
static int
clear_raw_data (Dwg_Object *obj)
{
  int complete = 1;
  free (obj->unknown_rest);
  obj->unknown_rest = NULL;
  obj->num_unknown_rest = 0;
  for (BITCODE_BL i = 0; i < obj->tio.object->num_eed; i++)
    {
      Dwg_Eed *eed = &obj->tio.object->eed[i];
      // the first entry of an APPID's group holds the group's raw bytes
      if (!eed->size)
        continue;
      if (!eed->data)
        complete = 0;
      else
        {
          free (eed->raw);
          eed->raw = NULL;
        }
    }
  return complete;
}

/* Reads one fixture: its checked objects decode to AutoCAD's fields. Then
   writes its objects from their fields, every object of an r2004 fixture,
   and reads them again: AutoCAD's sizes, and AutoCAD's raw bits where the
   fixture has them, for each one. */
static int
test_file (const Fixture_File *f, unsigned index)
{
  char path[512], out[64];
  Source_Object *source;
  unsigned num_source = 0, num_class[MAX_FIXTURE_CLASSES] = { 0 };
  unsigned num_same[MAX_FIXTURE_CLASSES] = { 0 };
  unsigned num_raw = 0, num_same_fields = 0, num_same_raw = 0;
  unsigned num_dd = 0, num_same_undecoded = 0, num_undecoded = 0;
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
  if (f->objects == EVERY_OBJECT && dwg.num_objects != f->num_objects)
    fail ("%s: %u objects, expected %u", f->file, (unsigned)dwg.num_objects,
          f->num_objects);

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
      const int undecoded = is_undecoded (obj);
      const char *name = class_name (&dwg, obj);
      const Fixture_Class *c = fixture_class (f, name, undecoded);
      Source_Object *s;
      if (!c && f->objects != EVERY_OBJECT)
        continue;
      if (c)
        num_class[c - f->classes]++;
      if (!obj->tio.object)
        {
          fail ("%s: %s " FORMAT_HV " has no fields", f->file, name,
                obj->handle.value);
          continue;
        }
      if (undecoded && !c)
        fail ("%s: %s " FORMAT_HV " isn't decoded", f->file, name,
              obj->handle.value);
      s = &source[num_source++];
      s->handle = obj->handle.value;
      s->fixedtype = obj->fixedtype;
      snprintf (s->name, sizeof (s->name), "%s", name);
      s->klass = c;
      s->size = obj->size;
      s->bitsize = obj->bitsize;
      s->num_unknown_bits = obj->num_unknown_bits;
      if (undecoded)
        {
          // written back from its raw bits: compared with a copy of them
          num_undecoded++;
          if (obj->num_unknown_bits)
            {
              const size_t len = (obj->num_unknown_bits + 7) / 8;
              s->unknown_bits = (BITCODE_TF)malloc (len);
              if (s->unknown_bits)
                memcpy (s->unknown_bits, obj->unknown_bits, len);
              else
                {
                  fail ("%s: malloc", f->file);
                  s->num_unknown_bits = 0;
                }
            }
          continue;
        }
      if (s->num_unknown_bits)
        num_raw++;
      s->unknown_bits = obj->unknown_bits;
      obj->unknown_bits = NULL;
      obj->num_unknown_bits = 0;
      if (!clear_raw_data (obj))
        fail ("%s: %s " FORMAT_HV " EED not decoded", f->file, s->name,
              s->handle);
      if (dwg.header.version >= R_2000)
        s->dd_rounded_bits = object_dd_rounded_bits (obj);
    }
  for (unsigned c = 0; c < MAX_FIXTURE_CLASSES && f->classes[c].name; c++)
    if (num_class[c] != f->classes[c].count)
      fail ("%s: %u %s, expected %u", f->file, num_class[c],
            f->classes[c].name, f->classes[c].count);
  if (num_raw != f->num_raw_bits)
    fail ("%s: %u objects with raw bits, expected %u", f->file, num_raw,
          f->num_raw_bits);

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
          Dwg_Object *obj = dwg_resolve_handle (&read, s->handle);
          int same_size, same_bits;
          loglevel = test_loglevel;
          if (!obj || obj->fixedtype != s->fixedtype)
            {
              fail ("%s: written, no %s " FORMAT_HV, f->file, s->name,
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
              if (c)
                num_same[c - f->classes]++;
              if (is_undecoded (obj))
                num_same_undecoded++;
              else
                {
                  num_same_fields++;
                  if (s->num_unknown_bits)
                    num_same_raw++;
                }
            }
          else if (s->dd_rounded_bits)
            {
              /* AutoCAD's data stream, less the rounded values. As the
                 handle stream is the same, the object is shorter by whole
                 bytes. */
              if (obj->bitsize + s->dd_rounded_bits == s->bitsize
                  && obj->size * 8 + s->dd_rounded_bits == s->size * 8)
                num_dd++;
              else
                fail ("%s: %s " FORMAT_HV " size %u, bitsize %u; AutoCAD's "
                      "%u, %u, less %u bits of rounded DD values",
                      f->file, s->name, s->handle, (unsigned)obj->size,
                      (unsigned)obj->bitsize, (unsigned)s->size,
                      (unsigned)s->bitsize, s->dd_rounded_bits);
            }
          else if (c && c->kind == CLASS_TODO)
            todo ("%s: %s " FORMAT_HV " size %u, bitsize %u; AutoCAD's %u, "
                  "%u (%s)",
                  f->file, s->name, s->handle, (unsigned)obj->size,
                  (unsigned)obj->bitsize, (unsigned)s->size,
                  (unsigned)s->bitsize, c->todo);
          else if (!same_size)
            fail ("%s: %s " FORMAT_HV " size %u, bitsize %u; AutoCAD's %u, "
                  "%u",
                  f->file, s->name, s->handle, (unsigned)obj->size,
                  (unsigned)obj->bitsize, (unsigned)s->size,
                  (unsigned)s->bitsize);
          else
            fail ("%s: %s " FORMAT_HV " bits differ from AutoCAD's", f->file,
                  s->name, s->handle);
        }
      for (unsigned c = 0; c < MAX_FIXTURE_CLASSES && f->classes[c].name;
           c++)
        if (f->classes[c].kind == CLASS_TODO && num_class[c]
            && num_same[c] == num_class[c])
          fail ("%s: every %s matches AutoCAD's now: make its check strict "
                "(%s)",
                f->file, f->classes[c].name, f->classes[c].todo);
      if (num_dd != f->num_dd_rounded)
        fail ("%s: %u LINE and LWPOLYLINE objects shorter by their rounded "
              "DD values, expected %u%s (%s)",
              f->file, num_dd, f->num_dd_rounded,
              num_dd ? "" : ": make their check strict", TODO_DD_ROUNDING);
      else if (num_dd)
        todo ("%s: %u LINE and LWPOLYLINE objects shorter than AutoCAD's "
              "by DD values within 1e-12 of their default, written as the "
              "default (%s)",
              f->file, num_dd, TODO_DD_ROUNDING);
      if (num_same_fields)
        ok ("%s: %u objects written from their fields with AutoCAD's "
            "sizes, %u of them with AutoCAD's raw bits",
            f->file, num_same_fields, num_same_raw);
      if (num_undecoded && num_same_undecoded == num_undecoded)
        ok ("%s: %u objects LibreDWG doesn't decode written back with "
            "AutoCAD's bits",
            f->file, num_undecoded);
    }
  free_dwg (&read);
  for (unsigned i = 0; i < num_source; i++)
    free (source[i].unknown_bits);
  free (source);
  if (!numfailed ())
    unlink (out);
  return numfailed ();
}

/* Every drawing in test-data/2004 is a fixture, so that one added there is
   checked too. */
static int
test_fixture_list (void)
{
#ifndef _MSC_VER
  char dir[512];
  DIR *d;
  const struct dirent *e;
  unsigned num_drawings = 0;

  failed = 0;
  if (!find_fixture ("2004", dir, sizeof (dir)))
    {
      fail ("test-data/2004 not found");
      return 1;
    }
  d = opendir (dir);
  if (!d)
    {
      fail ("opendir %s", dir);
      return 1;
    }
  while ((e = readdir (d)))
    {
      const size_t len = strlen (e->d_name);
      char file[300];
      int listed = 0;
      if (len < 4 || strcmp (e->d_name + len - 4, ".dwg"))
        continue;
      num_drawings++;
      snprintf (file, sizeof (file), "2004/%s", e->d_name);
      for (unsigned i = 0; i < NUM_FIXTURE_FILES; i++)
        if (!strcmp (fixture_files[i].file, file))
          listed = 1;
      if (!listed)
        fail ("%s is not in fixture_files", file);
    }
  closedir (d);
  if (!num_drawings)
    fail ("no drawings in %s", dir);
  else if (!numfailed ())
    ok ("test-data/2004: all %u drawings are fixtures", num_drawings);
#endif
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

/* Reads a DXF line, without surrounding blanks. Only its start is kept if
   it is longer than the buffer. */
static int
read_line (FILE *fp, char *line, size_t size)
{
  size_t len, start;
  if (!fgets (line, (int)size, fp))
    return 0;
  len = strlen (line);
  if (len && line[len - 1] != '\n')
    {
      int c;
      while ((c = fgetc (fp)) != EOF && c != '\n')
        ;
    }
  while (len
         && (line[len - 1] == '\n' || line[len - 1] == '\r'
             || line[len - 1] == ' '))
    line[--len] = '\0';
  start = strspn (line, " ");
  memmove (line, line + start, len - start + 1);
  return 1;
}

/* Reads a DXF group: its code and its value. */
static int
read_group (FILE *fp, int *code, char *value, size_t size)
{
  char line[32];
  if (!read_line (fp, line, sizeof (line)) || !read_line (fp, value, size))
    return 0;
  *code = atoi (line);
  return 1;
}

/* AutoCAD's DXF follows the AcDbBlockRotationAction subclass marker with
   the action's third connection point as 94 and 303. */
static void
check_rotation_action_codes (const char *file, unsigned expected)
{
  FILE *fp = fopen (file, "r");
  char value[256];
  int code;
  unsigned num_actions = 0, num_autocad = 0;
  if (!fp)
    {
      fail ("dxf: fopen %s", file);
      return;
    }
  while (read_group (fp, &code, value, sizeof (value)))
    {
      if (code != 100 || strcmp (value, "AcDbBlockRotationAction"))
        continue;
      num_actions++;
      code = -1;
      if (read_group (fp, &code, value, sizeof (value)) && code == 94
          && read_group (fp, &code, value, sizeof (value)) && code == 303)
        num_autocad++;
      else
        fail ("%s: AcDbBlockRotationAction %u: group %d where AutoCAD has "
              "94 and 303",
              file, num_actions, code);
    }
  fclose (fp);
  if (!expected)
    fail ("%s: no BLOCKROTATEACTION in the drawing", file);
  else if (num_actions != expected)
    fail ("%s: %u AcDbBlockRotationAction, expected %u", file, num_actions,
          expected);
  else if (num_autocad == num_actions)
    ok ("%s: %u AcDbBlockRotationAction with AutoCAD's 94 and 303", file,
        num_actions);
}

/* LibreDWG's DXF of 2004/Dynblocks.dwg writes its block actions'
   connection points with AutoCAD's group codes, BLOCKROTATEACTION's third
   point as 94 and 303 and BLOCKSCALEACTION's three as 94-96 and 303-305,
   and reads them back. */
static int
test_dxf_roundtrip (void)
{
  const char *name = "2004/Dynblocks.dwg";
  const char *out = "fields_test_dxf.dxf";
  char path[512];
  Dwg_Data dwg, dxf;
  Bit_Chain dat = { 0 };
  const Dwg_Object *obj;
  unsigned num_rotate = 0;
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
  for (BITCODE_BL i = 0; i < dwg.num_objects; i++)
    if (dwg.object[i].fixedtype == DWG_TYPE_BLOCKROTATEACTION)
      num_rotate++;
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
  check_rotation_action_codes (out, num_rotate);

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

  error += test_fixture_list ();
  for (unsigned i = 0; i < NUM_FIXTURE_FILES; i++)
    error += test_file (&fixture_files[i], i);
#ifndef DISABLE_DXF
  error += test_dxf_roundtrip ();
#endif
  return error ? 1 : 0;
}
