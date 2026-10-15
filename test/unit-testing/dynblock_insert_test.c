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

/* dwg_dynblock_insert () and dwg_add_INSERT_block (). The dynamic blocks of
   test-data/2004/Dynblocks.dwg, whose placements AutoCAD 2027 set and
   computed, are copied into a new r2004 document and placed there: as
   AutoCAD's own placements are, whose stored values the new ones must
   repeat, and at the definitions' values and at other visibility states.
   The document is written and read back, and the evaluator's check of every
   placement against its representation block must match. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
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
#include "dwg_dynblock_api.h"

#define OK(...)                                                               \
  do                                                                          \
    {                                                                         \
      loglevel = test_loglevel;                                               \
      ok (__VA_ARGS__);                                                       \
    }                                                                         \
  while (0)
#define FAIL(...)                                                             \
  do                                                                          \
    {                                                                         \
      loglevel = test_loglevel;                                               \
      fail (__VA_ARGS__);                                                     \
    }                                                                         \
  while (0)

#define FIXTURE "test/test-data/2004/Dynblocks.dwg"
#define WRITTEN "dynblock_insert_test.dwg"
#define WRITTEN_ATTRIBS "dynblock_insert_test_attribs.dwg"
#define WRITTEN_BLOCK "dynblock_insert_test_block.dwg"
#define WRITTEN_SORT "dynblock_insert_test_sort.dwg"
#define WRITTEN_MINSERT "dynblock_insert_test_minsert.dwg"

/* The fixture's definitions */
#define ALTEZZA 0x9931
#define TREES 0xB45B
#define TOILET 0xB8F7
#define DOOR 0xBAC4
#define DOOR_ELEVATION 0xBB0C /* constraint-based */
#define WINDOW 0xBBC1

/* The fixture reads with the errors of its constraint-based block only. */
#define KNOWN_ERRORS (DWG_ERR_UNHANDLEDCLASS | DWG_ERR_VALUEOUTOFBOUNDS)

/* A refusal: DWG_ERR_INVALIDDWG with the bit for the reason. */
#define REFUSED(bit) (DWG_ERR_INVALIDDWG | (bit))

/* AutoCAD's placements whose values the evaluator supports: two at the
   definition's values, which AutoCAD stores as representation blocks too,
   and the others at a visibility state. */
static const BITCODE_RLL autocad_placements[]
    = { 0xBFD0, 0xBFD8, 0xC145, 0xC14C, 0xC17F,
        0xC8D7, 0xC8DA, 0xC8E0, 0xC8E1, 0xC8E2 };

/* The visibility states each definition is placed at besides. Door's other
   states have a character outside ASCII, which a document built with the
   add API loses when written (runbook task P3-06, tested in copy_test). */
typedef struct
{
  BITCODE_RLL def;
  const char *property;
  const char *states[4];
} Test_States;

static const Test_States test_states[] = {
  { ALTEZZA,
    "Type (view)",
    { "Honda [Front]", "Truck (Side)", "AlteZZa [LeFt]", NULL } },
  { TREES,
    "Type (view)",
    { "Shrub (Plan)", "Type 1 Evergreen (Elevation)", NULL, NULL } },
  { TOILET,
    "Type (view)",
    { "Round (Plan)", "Pullchain (Plan)", NULL, NULL } },
  { DOOR, "Opening Angle", { "Closed", NULL, NULL, NULL } },
};

static char fixture[512];

static int
find_fixture (void)
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
      snprintf (fixture, sizeof (fixture), "%s/%s", dirs[i], FIXTURE);
      if (!stat (fixture, &attrib))
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

static void
free_dwg (Dwg_Data *dwg)
{
  loglevel = tracelevel;
  dwg_free (dwg);
  loglevel = test_loglevel;
}

static Dwg_Object *
object (const Dwg_Data *dwg, const BITCODE_RLL handle)
{
  return handle ? dwg_resolve_handle (dwg, handle) : NULL;
}

static BITCODE_RLL
ref (const Dwg_Object_Ref *r)
{
  return r ? r->absolute_ref : 0;
}

static BITCODE_RLL
handle_of (const Dwg_Data *dwg, const void *_obj)
{
  const Dwg_Object_Object *o = *(Dwg_Object_Object *const *)_obj;
  return dwg->object[o->objid].handle.value;
}

static Dwg_Object_BLOCK_HEADER *
block_header (const Dwg_Data *dwg, const BITCODE_RLL handle)
{
  Dwg_Object *obj = object (dwg, handle);
  return obj && obj->fixedtype == DWG_TYPE_BLOCK_HEADER
             ? obj->tio.object->tio.BLOCK_HEADER
             : NULL;
}

static Dwg_Object_BLOCK_HEADER *
model_space (Dwg_Data *dwg)
{
  return dwg_model_space_object (dwg)->tio.object->tio.BLOCK_HEADER;
}

static const Dwg_Object *
dict_item (const Dwg_Data *dwg, const Dwg_Object *dict, const char *name)
{
  const Dwg_Object_DICTIONARY *d;
  if (!dict || dict->fixedtype != DWG_TYPE_DICTIONARY)
    return NULL;
  d = dict->tio.object->tio.DICTIONARY;
  for (BITCODE_BL i = 0; d->texts && d->itemhandles && i < d->numitems; i++)
    if (d->texts[i] && !strcmp (d->texts[i], name))
      return object (dwg, ref (d->itemhandles[i]));
  return NULL;
}

static const Dwg_Object *
xdic (const Dwg_Data *dwg, const Dwg_Object *obj)
{
  if (obj->supertype == DWG_SUPERTYPE_ENTITY)
    return obj->tio.entity->is_xdic_missing
               ? NULL
               : object (dwg, ref (obj->tio.entity->xdicobjhandle));
  return obj->tio.object->is_xdic_missing
             ? NULL
             : object (dwg, ref (obj->tio.object->xdicobjhandle));
}

/* The ACAD_ENHANCEDBLOCKDATA dictionary of placement insert, or NULL. */
static const Dwg_Object *
enhanced_block_data (const Dwg_Data *dwg, const Dwg_Object *insert)
{
  const Dwg_Object *rep
      = dict_item (dwg, xdic (dwg, insert), "AcDbBlockRepresentation");
  return dict_item (dwg, dict_item (dwg, rep, "AppDataCache"),
                    "ACAD_ENHANCEDBLOCKDATA");
}

/* The 1005 handle and the 1071 index in obj's EED group of the APPID
   named appname. Returns 0 when it has no such group. */
static int
eed_tag (const Dwg_Data *dwg, const Dwg_Object *obj, const char *appname,
         BITCODE_RLL *handlep, BITCODE_RL *indexp)
{
  const Dwg_Eed *eed = obj->supertype == DWG_SUPERTYPE_ENTITY
                           ? obj->tio.entity->eed
                           : obj->tio.object->eed;
  const BITCODE_BL num_eed = obj->supertype == DWG_SUPERTYPE_ENTITY
                                 ? obj->tio.entity->num_eed
                                 : obj->tio.object->num_eed;
  int in_group = 0, found = 0;
  for (BITCODE_BL i = 0; eed && i < num_eed; i++)
    {
      if (eed[i].size)
        {
          const Dwg_Object *appid = object (dwg, eed[i].handle.value);
          in_group = appid && appid->fixedtype == DWG_TYPE_APPID
                     && !strcmp (appid->tio.object->tio.APPID->name, appname);
          found |= in_group;
        }
      // Dwg_Eed_Data is packed
      if (in_group && eed[i].data && eed[i].data->code == 5)
        memcpy (handlep, &eed[i].data->u.eed_5.entity, sizeof (*handlep));
      if (in_group && eed[i].data && eed[i].data->code == 71 && indexp)
        memcpy (indexp, &eed[i].data->u.eed_71.rl, sizeof (*indexp));
    }
  return found;
}

/*--------------------------------------------------------------------------------
 * The placements and their checks
 */

/* A placement made, with the values it was given. */
typedef struct
{
  BITCODE_RLL insert;
  BITCODE_RLL def;
  BITCODE_RLL autocad; /* the fixture's placement it repeats, or 0 */
  int at_defaults;
  Dwg_DynValue values[8];
  BITCODE_BL num_values;
} Placement;

#define MAX_PLACEMENTS 40

static Placement placements[MAX_PLACEMENTS];
static unsigned num_placements;

/* Two XRECORDs hold the same items. */
static int
same_xrecord (const Dwg_Object_XRECORD *a, const Dwg_Object_XRECORD *b,
              char *what, const size_t size)
{
  const Dwg_Resbuf *ra = a->xdata, *rb = b->xdata;
  unsigned i = 0;
  if (a->num_xdata != b->num_xdata || a->xdata_size != b->xdata_size
      || a->cloning != b->cloning)
    {
      snprintf (what, size, "%u items of %u bytes, cloning %u: AutoCAD's %u "
                "of %u, cloning %u",
                (unsigned)b->num_xdata, (unsigned)b->xdata_size,
                (unsigned)b->cloning, (unsigned)a->num_xdata,
                (unsigned)a->xdata_size, (unsigned)a->cloning);
      return 0;
    }
  for (; ra && rb; ra = ra->nextrb, rb = rb->nextrb, i++)
    {
      int same = ra->type == rb->type;
      if (same)
        switch (dwg_resbuf_value_type (ra->type))
          {
          case DWG_VT_STRING:
            same = ra->value.str.size == rb->value.str.size
                   && !memcmp (ra->value.str.u.data, rb->value.str.u.data,
                               ra->value.str.size);
            break;
          case DWG_VT_POINT3D:
            same = !memcmp (ra->value.pt, rb->value.pt, sizeof (ra->value.pt));
            break;
          case DWG_VT_REAL:
            same = ra->value.dbl == rb->value.dbl;
            break;
          case DWG_VT_INT16:
            same = ra->value.i16 == rb->value.i16;
            break;
          case DWG_VT_INT32:
            same = ra->value.i32 == rb->value.i32;
            break;
          default:
            same = 0;
            break;
          }
      if (!same)
        {
          snprintf (what, size, "item %u (code %d) differs", i, rb->type);
          return 0;
        }
    }
  return 1;
}

/* The ACAD_ENHANCEDBLOCKDATA of placement ours, in dwg, has the records
   that AutoCAD stored for its placement theirs, in fixture: the same
   names, in the same order, each XRECORD with the same items. */
static int
check_records (const char *what, const Dwg_Data *dwg, const Dwg_Object *ours,
               const Dwg_Data *fixture_dwg, const Dwg_Object *theirs)
{
  const Dwg_Object *a = enhanced_block_data (fixture_dwg, theirs);
  const Dwg_Object *b = enhanced_block_data (dwg, ours);
  const Dwg_Object_DICTIONARY *da, *db;
  char why[256] = "";
  if (!a || !b)
    {
      FAIL ("%s: ACAD_ENHANCEDBLOCKDATA %s", what,
            a ? "missing" : "missing in AutoCAD's placement");
      return 1;
    }
  da = a->tio.object->tio.DICTIONARY;
  db = b->tio.object->tio.DICTIONARY;
  if (da->numitems != db->numitems || da->cloning != db->cloning
      || da->is_hardowner != db->is_hardowner)
    {
      FAIL ("%s: %u records, AutoCAD's %u", what, (unsigned)db->numitems,
            (unsigned)da->numitems);
      return 1;
    }
  for (BITCODE_BL i = 0; i < da->numitems; i++)
    {
      const Dwg_Object *xa = object (fixture_dwg, ref (da->itemhandles[i]));
      const Dwg_Object *xb = object (dwg, ref (db->itemhandles[i]));
      if (strcmp (da->texts[i], db->texts[i]) || !xa || !xb
          || xa->fixedtype != DWG_TYPE_XRECORD
          || xb->fixedtype != DWG_TYPE_XRECORD
          || !same_xrecord (xa->tio.object->tio.XRECORD,
                            xb->tio.object->tio.XRECORD, why, sizeof (why)))
        {
          FAIL ("%s: record '%s' differs from AutoCAD's record '%s': %s",
                what, db->texts[i], da->texts[i], why);
          return 1;
        }
    }
  OK ("%s: its %u records are AutoCAD's", what, (unsigned)da->numitems);
  return 0;
}

/* The structure AutoCAD gives a placement, which the INSERT draws: its
   definition itself at the definition's values; otherwise a new
   anonymous block that AcDbBlockRepBTag names def in, with def's entity
   count, hasattrs and sort table, and an INSERT extension dictionary
   AcDbBlockRepresentation, with AcDbRepData naming def and AppDataCache
   holding ACAD_ENHANCEDBLOCKDATA. */
static int
check_structure (const char *what, const Dwg_Data *dwg, const Placement *pl)
{
  const Dwg_Object *ins = object (dwg, pl->insert);
  const Dwg_Object *def = object (dwg, pl->def);
  const Dwg_Object *blk, *rep, *repdata, *cache, *data;
  const Dwg_Object_BLOCK_HEADER *_blk, *_def;
  BITCODE_RLL named = 0;
  int sortents;
  if (!ins || ins->fixedtype != DWG_TYPE_INSERT || !def)
    {
      FAIL ("%s: INSERT " FORMAT_HV " missing", what, pl->insert);
      return 1;
    }
  blk = object (dwg, ref (ins->tio.entity->tio.INSERT->block_header));
  _def = def->tio.object->tio.BLOCK_HEADER;
  if (pl->at_defaults)
    {
      if (blk == def && !xdic (dwg, ins))
        {
          OK ("%s: draws the definition itself, with no stored values",
              what);
          return 0;
        }
      FAIL ("%s: draws " FORMAT_HV " %s stored values, not the definition",
            what, blk ? blk->handle.value : 0,
            xdic (dwg, ins) ? "with" : "without");
      return 1;
    }
  if (!blk || blk == def || blk->fixedtype != DWG_TYPE_BLOCK_HEADER)
    {
      FAIL ("%s: draws " FORMAT_HV ", not a representation block", what,
            blk ? blk->handle.value : 0);
      return 1;
    }
  _blk = blk->tio.object->tio.BLOCK_HEADER;
  sortents = !!dict_item (dwg, xdic (dwg, def), "ACAD_SORTENTS");
  if (!_blk->anonymous || strcmp (_blk->name, "*U")
      || !eed_tag (dwg, blk, "AcDbBlockRepBTag", &named, NULL)
      || named != pl->def || _blk->num_owned != _def->num_owned
      || _blk->hasattrs != _def->hasattrs || !_blk->is_xref_ref
      || !!dict_item (dwg, xdic (dwg, blk), "ACAD_SORTENTS") != sortents)
    {
      FAIL ("%s: representation block " FORMAT_HV ": anonymous %d, BTag "
            FORMAT_HV ", %u entities of %u, hasattrs %d of %d, sort table "
            "%s",
            what, blk->handle.value, _blk->anonymous, named,
            (unsigned)_blk->num_owned, (unsigned)_def->num_owned,
            _blk->hasattrs, _def->hasattrs,
            sortents ? "missing" : "unexpected or missing");
      return 1;
    }
  if (sortents)
    {
      const Dwg_Object *table
          = dict_item (dwg, xdic (dwg, blk), "ACAD_SORTENTS");
      if (table->fixedtype != DWG_TYPE_SORTENTSTABLE
          || ref (table->tio.object->tio.SORTENTSTABLE->block_owner)
                 != blk->handle.value
          || ref (table->tio.object->ownerhandle)
                 != ref (blk->tio.object->xdicobjhandle))
        {
          FAIL ("%s: the sort table isn't the representation block's", what);
          return 1;
        }
    }
  rep = dict_item (dwg, xdic (dwg, ins), "AcDbBlockRepresentation");
  repdata = dict_item (dwg, rep, "AcDbRepData");
  cache = dict_item (dwg, rep, "AppDataCache");
  data = dict_item (dwg, cache, "ACAD_ENHANCEDBLOCKDATA");
  if (!rep || !repdata || !cache || !data
      || rep->tio.object->tio.DICTIONARY->numitems != 2
      || cache->tio.object->tio.DICTIONARY->numitems != 1
      || repdata->fixedtype != DWG_TYPE_BLOCKREPRESENTATION
      || repdata->tio.object->tio.BLOCKREPRESENTATION->flag != 1
      || ref (repdata->tio.object->tio.BLOCKREPRESENTATION->block)
             != pl->def
      || ref (rep->tio.object->ownerhandle)
             != ref (ins->tio.entity->xdicobjhandle)
      || ref (data->tio.object->ownerhandle) != cache->handle.value)
    {
      FAIL ("%s: the INSERT's AcDbBlockRepresentation is not as AutoCAD "
            "stores it",
            what);
      return 1;
    }
  OK ("%s: draws representation block " FORMAT_HV ", with %u records", what,
      blk->handle.value, (unsigned)data->tio.object->tio.DICTIONARY->numitems);
  return 0;
}

/* The values read back from placement pl are those it was given, and the
   others the definition's. */
static int
check_values (const char *what, const Dwg_Data *dwg, const Placement *pl)
{
  const Dwg_Object *ins = object (dwg, pl->insert);
  Dwg_Object_BLOCK_HEADER *def = NULL;
  Dwg_DynValue *values = NULL;
  BITCODE_BL count = 0;
  int error, failures = 0;
  error = dwg_dynblock_get_values (ins->tio.entity->tio.INSERT, &def, &values,
                                   &count);
  loglevel = test_loglevel;
  if (error || !def || handle_of (dwg, def) != pl->def)
    {
      FAIL ("%s: reading the values: 0x%x", what, error);
      dwg_dynblock_free_values (values, count);
      return 1;
    }
  for (BITCODE_BL i = 0; i < pl->num_values; i++)
    {
      const Dwg_DynValue *v = &pl->values[i];
      int found = 0;
      for (BITCODE_BL j = 0; j < count; j++)
        if (!strcmp (values[j].name, v->name))
          {
            found = 1;
            if (v->state
                    ? !values[j].state || strcmp (values[j].state, v->state)
                    : fabs (values[j].value - v->value) > 1e-9)
              {
                FAIL ("%s: '%s' reads back %s, given %s", what, v->name,
                      values[j].state ? values[j].state : "a value",
                      v->state ? v->state : "a value");
                failures++;
              }
          }
      if (!found)
        {
          FAIL ("%s: '%s' doesn't read back", what, v->name);
          failures++;
        }
    }
  if (!failures)
    OK ("%s: its %u values read back as given", what, (unsigned)count);
  dwg_dynblock_free_values (values, count);
  return failures;
}

/* Each INSERT and MINSERT of dwg is listed by the block it draws, and each
   block lists only INSERTs and MINSERTs that draw it. */
static int
check_inserts (const char *what, const Dwg_Data *dwg)
{
  unsigned listed = 0, wrong = 0, missing = 0;
  for (BITCODE_BL i = 0; i < dwg->num_objects; i++)
    {
      const Dwg_Object *obj = &dwg->object[i];
      if (obj->fixedtype == DWG_TYPE_BLOCK_HEADER)
        {
          const Dwg_Object_BLOCK_HEADER *blk
              = obj->tio.object->tio.BLOCK_HEADER;
          for (BITCODE_BL k = 0; blk->inserts && k < blk->num_inserts; k++)
            {
              const Dwg_Object *ins = object (dwg, ref (blk->inserts[k]));
              if (ins && ins->fixedtype == DWG_TYPE_INSERT
                  && ref (ins->tio.entity->tio.INSERT->block_header)
                         == obj->handle.value)
                listed++;
              else if (ins && ins->fixedtype == DWG_TYPE_MINSERT
                       && ref (ins->tio.entity->tio.MINSERT->block_header)
                              == obj->handle.value)
                listed++;
              else
                wrong++;
            }
        }
      else if (obj->fixedtype == DWG_TYPE_INSERT
               || obj->fixedtype == DWG_TYPE_MINSERT)
        {
          const BITCODE_RLL drawn
              = obj->fixedtype == DWG_TYPE_INSERT
                    ? ref (obj->tio.entity->tio.INSERT->block_header)
                    : ref (obj->tio.entity->tio.MINSERT->block_header);
          const Dwg_Object_BLOCK_HEADER *blk = block_header (dwg, drawn);
          int found = 0;
          for (BITCODE_BL k = 0; blk && blk->inserts && k < blk->num_inserts;
               k++)
            found |= ref (blk->inserts[k]) == obj->handle.value;
          missing += !found;
        }
    }
  if (!wrong && !missing)
    {
      OK ("%s: the blocks list their %u INSERTs", what, listed);
      return 0;
    }
  FAIL ("%s: %u INSERTs listed by the wrong block, %u not listed", what,
        wrong, missing);
  return 1;
}

/* Every reference of dwg's objects resolves, but for the NULL ones.
   Handles with code 0 are values, not references (HANDSEED, sort keys). */
static int
check_refs (const char *what, Dwg_Data *dwg)
{
  unsigned dangling = 0;
  for (BITCODE_BL i = 0; i < dwg->num_object_refs; i++)
    {
      const Dwg_Object_Ref *r = dwg->object_ref[i];
      if (r && r->absolute_ref && r->handleref.code
          && !dwg_resolve_handle (dwg, r->absolute_ref))
        {
          if (!dangling)
            printf ("  # %s: " FORMAT_REF " dangles\n", what, ARGS_REF (r));
          dangling++;
        }
    }
  if (!dangling)
    {
      OK ("%s: %u references resolve", what, (unsigned)dwg->num_object_refs);
      return 0;
    }
  FAIL ("%s: %u references dangle", what, dangling);
  return 1;
}

/* Places def, a copy in dst of the fixture's definition fixture_def, with
   the given values, and checks the result. */
static int
place (Dwg_Data *dst, const BITCODE_RLL def, const BITCODE_RLL autocad,
       const BITCODE_3BD *ins_pt, const double scale, const double rotation,
       const Dwg_DynValue *values, const BITCODE_BL num_values,
       const int at_defaults, const char *what)
{
  Placement *pl;
  Dwg_Entity_INSERT *insert;
  int error = 0;
  if (num_placements == MAX_PLACEMENTS || num_values > 8)
    {
      FAIL ("%s: too many placements", what);
      return 1;
    }
  loglevel = tracelevel;
  insert = dwg_dynblock_insert (model_space (dst), block_header (dst, def),
                                ins_pt, scale, rotation, values, num_values,
                                NULL, 0, &error);
  loglevel = test_loglevel;
  if (!insert || error)
    {
      FAIL ("%s: error 0x%x", what, error);
      return 1;
    }
  pl = &placements[num_placements++];
  memset (pl, 0, sizeof (*pl));
  pl->insert = handle_of (dst, insert);
  pl->def = def;
  pl->autocad = autocad;
  pl->at_defaults = at_defaults;
  pl->num_values = num_values;
  for (BITCODE_BL i = 0; i < num_values; i++)
    {
      pl->values[i] = values[i];
      pl->values[i].name = strdup (values[i].name);
      pl->values[i].state = values[i].state ? strdup (values[i].state) : NULL;
    }
  if (insert->ins_pt.x != ins_pt->x || insert->ins_pt.y != ins_pt->y
      || insert->ins_pt.z != ins_pt->z || insert->scale.x != scale
      || insert->scale.y != scale || insert->scale.z != scale
      || fabs (insert->rotation - fmod (rotation + 2.0 * M_PI, 2.0 * M_PI))
             > 1e-12)
    {
      FAIL ("%s: the INSERT isn't at the point, scale and rotation given",
            what);
      return 1;
    }
  return check_structure (what, dst, pl);
}

static void
free_placements (void)
{
  for (unsigned i = 0; i < num_placements; i++)
    for (BITCODE_BL j = 0; j < placements[i].num_values; j++)
      {
        free (placements[i].values[j].name);
        free (placements[i].values[j].state);
      }
  num_placements = 0;
}

/* Copies the fixture's definitions into dst, into copies[] by fixture
   handle. */
static int
copy_definitions (Dwg_Data *dst, Dwg_Data *src, BITCODE_RLL map[][2],
                  unsigned *num_map)
{
  const BITCODE_RLL defs[]
      = { ALTEZZA, TREES, TOILET, DOOR, WINDOW, DOOR_ELEVATION };
  int failures = 0;
  *num_map = 0;
  for (size_t i = 0; i < sizeof (defs) / sizeof (defs[0]); i++)
    {
      Dwg_Object_BLOCK_HEADER *copy;
      int error = 0;
      loglevel = defs[i] == DOOR_ELEVATION ? 0 : tracelevel;
      copy = dwg_dynblock_copy_block (dst, src, block_header (src, defs[i]),
                                      &error);
      loglevel = test_loglevel;
      if (defs[i] == DOOR_ELEVATION)
        continue; // constraint-based, refused
      if (!copy || error)
        {
          FAIL ("copy " FORMAT_HV ": 0x%x", defs[i], error);
          failures++;
          continue;
        }
      map[*num_map][0] = defs[i];
      map[*num_map][1] = handle_of (dst, copy);
      (*num_map)++;
    }
  return failures;
}

static BITCODE_RLL
copy_of (BITCODE_RLL map[][2], const unsigned num_map, const BITCODE_RLL def)
{
  for (unsigned i = 0; i < num_map; i++)
    if (map[i][0] == def)
      return map[i][1];
  return 0;
}

/* The fixture's definitions placed as AutoCAD placed them, at their own
   values, and at other visibility states, scaled and rotated; and their
   checks, before and after a write and read back. */
static int
test_placements (Dwg_Data *src)
{
  Dwg_Data *dst = dwg_new_Document (R_2004, 0, tracelevel);
  Dwg_Data readback;
  BITCODE_RLL map[8][2];
  unsigned num_map;
  BITCODE_BL first;
  Dwg_DynBlock_Check_Report *report;
  unsigned reps = 0;
  int failures, error;
  char what[256];

  loglevel = test_loglevel;
  failures = copy_definitions (dst, src, map, &num_map);
  first = dst->num_objects;

  // AutoCAD's placements, repeated
  for (size_t i = 0; i < sizeof (autocad_placements) / sizeof (BITCODE_RLL);
       i++)
    {
      const Dwg_Object *ins = object (src, autocad_placements[i]);
      const Dwg_Entity_INSERT *_ins = ins->tio.entity->tio.INSERT;
      Dwg_Object_BLOCK_HEADER *def = NULL;
      Dwg_DynValue *values = NULL;
      BITCODE_BL count = 0;
      Dwg_DynParam *params = NULL;
      BITCODE_BL num_params = 0;
      int at_defaults = 1;
      error = dwg_dynblock_get_values (_ins, &def, &values, &count);
      error |= dwg_dynblock_get_params (def, &params, &num_params);
      loglevel = test_loglevel;
      for (BITCODE_BL j = 0; !error && j < count; j++)
        if (values[j].state ? strcmp (values[j].state, params[j].default_state)
                            : fabs (values[j].value - params[j].default_value)
                                  > 1e-9)
          at_defaults = 0;
      snprintf (what, sizeof (what), "AutoCAD's placement " FORMAT_HV,
                autocad_placements[i]);
      if (error)
        {
          FAIL ("%s: its values: 0x%x", what, error);
          failures++;
        }
      else
        failures += place (dst, copy_of (map, num_map, handle_of (src, def)),
                           autocad_placements[i], &_ins->ins_pt,
                           _ins->scale.x, _ins->rotation, values, count,
                           at_defaults, what);
      dwg_dynblock_free_values (values, count);
      dwg_dynblock_free_params (params, num_params);
    }
  // at the definitions' values, with no values given
  for (unsigned i = 0; i < num_map; i++)
    {
      const BITCODE_3BD pt = { 100.0 * i, -500.0, 0.0 };
      snprintf (what, sizeof (what), "place " FORMAT_HV " at its values",
                map[i][0]);
      failures += place (dst, map[i][1], 0, &pt, 1.0, 0.0, NULL, 0, 1, what);
    }
  // at other states, scaled and rotated
  for (size_t i = 0; i < sizeof (test_states) / sizeof (test_states[0]); i++)
    for (unsigned s = 0; s < 4 && test_states[i].states[s]; s++)
      {
        const BITCODE_3BD pt = { 200.0 * s, -1000.0 - 200.0 * i, 0.0 };
        Dwg_DynValue v;
        memset (&v, 0, sizeof (v));
        v.name = (char *)test_states[i].property;
        v.state = (char *)test_states[i].states[s];
        snprintf (what, sizeof (what), "place " FORMAT_HV " at '%s'",
                  test_states[i].def, v.state);
        failures += place (dst, copy_of (map, num_map, test_states[i].def), 0,
                           &pt, 1.0 + s, -M_PI / 4 * s, &v, 1, 0, what);
      }

  // before writing
  for (unsigned i = 0; i < num_placements; i++)
    {
      const Placement *pl = &placements[i];
      reps += !pl->at_defaults;
      snprintf (what, sizeof (what), "placement " FORMAT_HV, pl->insert);
      failures += check_values (what, dst, pl);
      if (pl->autocad && !pl->at_defaults)
        failures += check_records (what, dst, object (dst, pl->insert), src,
                                   object (src, pl->autocad));
    }
  failures += check_inserts ("placements", dst);
  failures += check_refs ("placements", dst);

  error = write_dwg (WRITTEN, dst);
  if (error >= DWG_ERR_CRITICAL)
    {
      FAIL ("write r2004: 0x%x", error);
      free_dwg (dst);
      free (dst);
      free_placements ();
      return failures + 1;
    }
  error = read_dwg (WRITTEN, &readback);
  if (error >= DWG_ERR_CRITICAL)
    {
      FAIL ("read back: 0x%x", error);
      failures++;
    }
  else
    {
      OK ("write r2004 and read back: 0x%x, %u placements", error,
          num_placements);
      for (unsigned i = 0; i < num_placements; i++)
        {
          const Placement *pl = &placements[i];
          snprintf (what, sizeof (what), "read back placement " FORMAT_HV,
                    pl->insert);
          failures += check_structure (what, &readback, pl);
          failures += check_values (what, &readback, pl);
          if (pl->autocad && !pl->at_defaults)
            failures += check_records (what, &readback,
                                       object (&readback, pl->insert), src,
                                       object (src, pl->autocad));
        }
      failures += check_inserts ("read back", &readback);
      failures += check_refs ("read back", &readback);
      // the evaluation of each placement's stored values is its block
      loglevel = tracelevel;
      report = dwg_dynblock_check (&readback, 1e-9);
      loglevel = test_loglevel;
      if (report && report->num_checks == reps && report->num_match == reps)
        OK ("read back: the check matches each of the %u representation "
            "blocks",
            reps);
      else
        {
          FAIL ("read back: the check of %u representation blocks: %u "
                "checks, %u match, %u mismatch, %u unsupported, %u failed",
                reps, report ? (unsigned)report->num_checks : 0,
                report ? (unsigned)report->num_match : 0,
                report ? (unsigned)report->num_mismatch : 0,
                report ? (unsigned)report->num_unsupported : 0,
                report ? (unsigned)report->num_failed : 0);
          for (BITCODE_BL i = 0; report && i < report->num_checks; i++)
            if (report->checks[i].status != DWG_DYNBLOCK_CHECK_MATCH)
              printf ("  # " FORMAT_HV ": %s\n", report->checks[i].insert,
                      report->checks[i].reason ? report->checks[i].reason
                                               : "");
          failures++;
        }
      dwg_dynblock_free_check_report (report);
    }
  free_dwg (&readback);
  if (!failures)
    unlink (WRITTEN);
  free_dwg (dst);
  free (dst);
  free_placements ();
  (void)first;
  return failures;
}

/*--------------------------------------------------------------------------------
 * Refusals
 */

static int
test_refusal (Dwg_Data *dwg, Dwg_Object_BLOCK_HEADER *owner,
              Dwg_Object_BLOCK_HEADER *def, const BITCODE_3BD *pt,
              const double scale, const double rotation,
              const Dwg_DynValue *values, const BITCODE_BL n,
              const Dwg_AttValue *atts, const BITCODE_BL natts,
              const int expected, const char *what)
{
  const BITCODE_BL num_objects = dwg->num_objects;
  int error = 0;
  Dwg_Entity_INSERT *insert;
  loglevel = 0; // a refusal logs an error
  insert = dwg_dynblock_insert (owner, def, pt, scale, rotation, values, n,
                                atts, natts, &error);
  loglevel = test_loglevel;
  if (!insert && error == expected && dwg->num_objects == num_objects)
    {
      OK ("insert refuses %s: 0x%x", what, error);
      return 0;
    }
  FAIL ("insert %s: %s, 0x%x, expected 0x%x, %u objects added", what,
        insert ? "an INSERT" : "NULL", error, expected,
        (unsigned)(dwg->num_objects - num_objects));
  return 1;
}

static int
test_refusals (Dwg_Data *src)
{
  Dwg_Data *dst = dwg_new_Document (R_2004, 0, tracelevel);
  Dwg_Data *other = dwg_new_Document (R_2004, 0, tracelevel);
  BITCODE_RLL map[8][2];
  unsigned num_map;
  Dwg_Object_BLOCK_HEADER *ms, *window, *toilet, *trees, *altezza;
  const BITCODE_3BD pt = { 0.0, 0.0, 0.0 },
                    nan_pt = { 0.0, (double)NAN, 0.0 };
  Dwg_DynValue v[2];
  Dwg_AttValue a[2];
  int failures;

  loglevel = test_loglevel;
  failures = copy_definitions (dst, src, map, &num_map);
  ms = model_space (dst);
  window = block_header (dst, copy_of (map, num_map, WINDOW));
  toilet = block_header (dst, copy_of (map, num_map, TOILET));
  trees = block_header (dst, copy_of (map, num_map, TREES));
  altezza = block_header (dst, copy_of (map, num_map, ALTEZZA));
  memset (v, 0, sizeof (v));
  memset (a, 0, sizeof (a));

  {
    int error = 0;
    if (!dwg_dynblock_insert (ms, NULL, &pt, 1.0, 0.0, NULL, 0, NULL, 0,
                              &error)
        && error == DWG_ERR_INTERNALERROR
        && !dwg_dynblock_insert (ms, trees, NULL, 1.0, 0.0, NULL, 0, NULL, 0,
                                 &error)
        && error == DWG_ERR_INTERNALERROR
        && !dwg_dynblock_insert (ms, trees, &pt, 1.0, 0.0, NULL, 1, NULL, 0,
                                 &error)
        && error == DWG_ERR_INTERNALERROR)
      OK ("insert refuses NULL arguments: 0x%x", error);
    else
      {
        FAIL ("insert NULL arguments: 0x%x", error);
        failures++;
      }
  }
  {
    // it links to Trees' object, but the document doesn't hold it
    Dwg_Object_BLOCK_HEADER struct_copy = *trees;
    failures += test_refusal (dst, ms, &struct_copy, &pt, 1.0, 0.0, NULL, 0,
                              NULL, 0, REFUSED (DWG_ERR_INVALIDHANDLE),
                              "a copy of a block's struct");
  }
  failures += test_refusal (dst, model_space (other), trees, &pt, 1.0, 0.0,
                            NULL, 0, NULL, 0, REFUSED (DWG_ERR_INVALIDHANDLE),
                            "an owner of another document");
  failures += test_refusal (dst, ms, ms, &pt, 1.0, 0.0, NULL, 0, NULL, 0,
                            REFUSED (DWG_ERR_INVALIDTYPE),
                            "a block that isn't dynamic");
  failures += test_refusal (dst, trees, trees, &pt, 1.0, 0.0, NULL, 0, NULL, 0,
                            REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "the definition as the owner");
  {
    // alteZZa inserts block 't'
    Dwg_Object_BLOCK_HEADER *t = NULL;
    for (BITCODE_BL i = 0; !t && i < altezza->num_owned; i++)
      {
        Dwg_Object *ent = object (dst, ref (altezza->entities[i]));
        if (ent && ent->fixedtype == DWG_TYPE_INSERT)
          t = block_header (dst,
                            ref (ent->tio.entity->tio.INSERT->block_header));
      }
    if (t)
      failures += test_refusal (dst, t, altezza, &pt, 1.0, 0.0, NULL, 0, NULL,
                                0, REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                                "an owner the definition inserts");
    else
      {
        FAIL ("alteZZa inserts no block");
        failures++;
      }
  }
  failures += test_refusal (dst, ms, trees, &nan_pt, 1.0, 0.0, NULL, 0, NULL,
                            0, REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "a point that isn't finite");
  failures += test_refusal (dst, ms, trees, &pt, 0.0, 0.0, NULL, 0, NULL, 0,
                            REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "a scale of 0");
  failures += test_refusal (dst, ms, trees, &pt, 1.0, (double)INFINITY, NULL,
                            0, NULL, 0, REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "a rotation that isn't finite");

  v[0].name = (char *)"Window Width";
  v[0].value = 12.0;
  failures += test_refusal (dst, ms, window, &pt, 1.0, 0.0, v, 1, NULL, 0,
                            REFUSED (DWG_ERR_NOTYETSUPPORTED),
                            "a changed linear value");
  v[0].name = (char *)"Angle";
  v[0].value = M_PI / 6;
  failures += test_refusal (dst, ms, toilet, &pt, 1.0, 0.0, v, 1, NULL, 0,
                            REFUSED (DWG_ERR_NOTYETSUPPORTED),
                            "a changed rotation value");
  v[0].name = (char *)"Flip view";
  v[0].value = 1.0;
  failures += test_refusal (dst, ms, toilet, &pt, 1.0, 0.0, v, 1, NULL, 0,
                            REFUSED (DWG_ERR_NOTYETSUPPORTED), "a flip");
  v[0].value = 0.5;
  failures += test_refusal (dst, ms, toilet, &pt, 1.0, 0.0, v, 1, NULL, 0,
                            REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "a flip value of 0.5");
  v[0].name = (char *)"Window Width";
  v[0].value = (double)NAN;
  failures += test_refusal (dst, ms, window, &pt, 1.0, 0.0, v, 1, NULL, 0,
                            REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "a value that isn't finite");
  v[0].name = (char *)"Width";
  v[0].value = 36.0;
  failures += test_refusal (dst, ms, window, &pt, 1.0, 0.0, v, 1, NULL, 0,
                            REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "an unknown property");
  v[0].name = (char *)"Type (view)";
  v[0].state = (char *)"Oak (Plan)";
  failures += test_refusal (dst, ms, trees, &pt, 1.0, 0.0, v, 1, NULL, 0,
                            REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "an unknown state");
  v[0].state = (char *)"Shrub (Plan)";
  v[1] = v[0];
  failures += test_refusal (dst, ms, trees, &pt, 1.0, 0.0, v, 2, NULL, 0,
                            REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "a property given twice");
  a[0].tag = (char *)"NONE";
  a[0].value = (char *)"x";
  failures += test_refusal (dst, ms, trees, &pt, 1.0, 0.0, NULL, 0, a, 1,
                            REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "an attribute the block hasn't");
  // a full block table, whose 16-bit count the new block would wrap
  {
    Dwg_Object_BLOCK_CONTROL *ctrl
        = dwg_get_first_object (dst, DWG_TYPE_BLOCK_CONTROL)
              ->tio.object->tio.BLOCK_CONTROL;
    const BITCODE_BS num_entries = ctrl->num_entries;
    ctrl->num_entries = 0xFFFF;
    failures += test_refusal (dst, ms, trees, &pt, 1.0, 0.0, v, 1, NULL, 0,
                              REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                              "a full block table");
    ctrl->num_entries = num_entries;
  }
  failures += test_refusal (dst, ms, trees, &pt, -1.0, 0.0, NULL, 0, NULL, 0,
                            REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "a negative scale, which mirrors");
  // a tag AutoCAD allows, but dwg_add_ATTRIB () doesn't: refused before
  // the INSERT is added
  {
    const dwg_point_3d at = { 0.0, 0.0, 0.0 };
    Dwg_Entity_ATTDEF *attdef
        = dwg_add_ATTDEF (toilet, 2.5, 0, "Cable", &at, "CABLE", "x");
    loglevel = test_loglevel;
    if (attdef)
      {
        free (attdef->tag);
        attdef->tag = strdup ("CABLE.NO");
        failures += test_refusal (dst, ms, toilet, &pt, 1.0, 0.0, NULL, 0,
                                  NULL, 0, REFUSED (DWG_ERR_NOTYETSUPPORTED),
                                  "a tag dwg_add_ATTRIB () refuses");
      }
    else
      {
        FAIL ("adding an attribute definition to Toilet failed");
        failures++;
      }
  }
  free_dwg (dst);
  free (dst);
  free_dwg (other);
  free (other);
  return failures;
}

/*--------------------------------------------------------------------------------
 * Blocks, the add API, attributes and owned objects
 */

/* AutoCAD sets is_xref_ref on every BLOCK_HEADER it writes in an r2004
   file: definitions, representation blocks and layouts alike. It is the
   table record's 64 flag, which r2007 and later don't store and read as 1,
   so the add API is right to set it on the blocks it inserts. */
static int
test_is_xref_ref (const Dwg_Data *src)
{
  unsigned blocks = 0, set = 0;
  for (BITCODE_BL i = 0; i < src->num_objects; i++)
    if (src->object[i].fixedtype == DWG_TYPE_BLOCK_HEADER)
      {
        blocks++;
        set += src->object[i].tio.object->tio.BLOCK_HEADER->is_xref_ref;
      }
  if (blocks && set == blocks)
    {
      OK ("is_xref_ref: set on all %u of AutoCAD's blocks", blocks);
      return 0;
    }
  FAIL ("is_xref_ref: set on %u of AutoCAD's %u blocks", set, blocks);
  return 1;
}

/* Adds an anonymous block, named "*U", holding a line. */
static Dwg_Object_BLOCK_HEADER *
add_anonymous_block (Dwg_Data *dwg)
{
  const dwg_point_3d start = { 0.0, 0.0, 0.0 }, end = { 5.0, 5.0, 0.0 };
  Dwg_Object_BLOCK_HEADER *blk = dwg_add_BLOCK_HEADER (dwg, "*U");
  if (!blk || !dwg_add_BLOCK (blk, "*U") || !dwg_add_LINE (blk, &start, &end)
      || !dwg_add_ENDBLK (blk))
    return NULL;
  blk->anonymous = 1;
  return blk;
}

/* dwg_add_INSERT_block () places the block it is given, where
   dwg_add_INSERT () places the first block of the name it is given. */
static int
test_add_insert_block (void)
{
  Dwg_Data *dwg = dwg_new_Document (R_2004, 0, tracelevel);
  Dwg_Data *other = dwg_new_Document (R_2004, 0, tracelevel);
  const dwg_point_3d pt = { 1.0, 2.0, 3.0 };
  Dwg_Object_BLOCK_HEADER *first, *second, *foreign;
  Dwg_Entity_INSERT *by_name, *by_ref;
  BITCODE_RLL h_first, h_second, h_insert = 0;
  BITCODE_BL num_objects;
  Dwg_Data readback;
  int failures = 0, error;

  first = add_anonymous_block (dwg);
  second = add_anonymous_block (dwg);
  foreign = add_anonymous_block (other);
  loglevel = 0; // dwg_add_INSERT warns that "*U" isn't a valid name
  by_name = first && second
                ? dwg_add_INSERT (model_space (dwg), &pt, "*U", 1.0, 1.0,
                                  1.0, 0.0)
                : NULL;
  loglevel = tracelevel;
  by_ref = by_name ? dwg_add_INSERT_block (model_space (dwg), &pt, second,
                                           2.0, 2.0, 2.0, 0.5)
                   : NULL;
  loglevel = test_loglevel;
  if (!by_ref || !foreign)
    {
      FAIL ("add_INSERT_block: adding the blocks or INSERTs failed");
      free_dwg (dwg);
      free (dwg);
      free_dwg (other);
      free (other);
      return 1;
    }
  h_first = handle_of (dwg, first);
  h_second = handle_of (dwg, second);
  h_insert = handle_of (dwg, by_ref);
  if (ref (by_name->block_header) == h_first)
    OK ("add_INSERT: by the name \"*U\", the first anonymous block");
  else
    {
      FAIL ("add_INSERT: by the name \"*U\", " FORMAT_HV,
            ref (by_name->block_header));
      failures++;
    }
  if (ref (by_ref->block_header) == h_second && second->num_inserts == 1
      && ref (second->inserts[0]) == h_insert && second->used == 1
      && second->is_xref_ref && by_ref->ins_pt.x == pt.x
      && by_ref->ins_pt.y == pt.y && by_ref->ins_pt.z == pt.z
      && by_ref->scale.x == 2.0 && by_ref->scale.y == 2.0
      && by_ref->scale.z == 2.0 && by_ref->rotation == 0.5)
    OK ("add_INSERT_block: places the block given, which lists it");
  else
    {
      FAIL ("add_INSERT_block: draws " FORMAT_HV ", the block " FORMAT_HV
            " lists %u INSERTs",
            ref (by_ref->block_header), h_second,
            (unsigned)second->num_inserts);
      failures++;
    }
  num_objects = dwg->num_objects;
  loglevel = 0;
  if (!dwg_add_INSERT_block (second, &pt, second, 1.0, 1.0, 1.0, 0.0)
      && dwg->num_objects == num_objects)
    OK ("add_INSERT_block: refuses a block inserting itself");
  else
    {
      FAIL ("add_INSERT_block: a block inserting itself: %u objects added",
            (unsigned)(dwg->num_objects - num_objects));
      failures++;
    }
  if (!dwg_add_INSERT_block (model_space (dwg), &pt, foreign, 1.0, 1.0, 1.0,
                             0.0)
      && dwg->num_objects == num_objects)
    OK ("add_INSERT_block: refuses a block of another document");
  else
    {
      FAIL ("add_INSERT_block: a block of another document: %u objects "
            "added",
            (unsigned)(dwg->num_objects - num_objects));
      failures++;
    }
  {
    // it links to model space's object, but the document doesn't hold it
    Dwg_Object_BLOCK_HEADER *ms = model_space (dwg);
    Dwg_Object_BLOCK_HEADER owner_copy = *ms;
    const BITCODE_BL num_owned = ms->num_owned;
    BITCODE_H *const entities = ms->entities;
    if (!dwg_add_INSERT_block (&owner_copy, &pt, second, 1.0, 1.0, 1.0, 0.0)
        && dwg->num_objects == num_objects && second->num_inserts == 1
        && owner_copy.num_owned == num_owned
        && owner_copy.entities == entities && ms->num_owned == num_owned)
      OK ("add_INSERT_block: refuses a copy of the owner's struct");
    else
      {
        FAIL ("add_INSERT_block: a copy of the owner's struct: %u objects "
              "added, the copy owns %u entities",
              (unsigned)(dwg->num_objects - num_objects),
              (unsigned)owner_copy.num_owned);
        failures++;
      }
  }
  {
    Dwg_Object_BLOCK_HEADER block_copy = *second;
    if (!dwg_add_INSERT_block (model_space (dwg), &pt, &block_copy, 1.0, 1.0,
                               1.0, 0.0)
        && dwg->num_objects == num_objects && second->num_inserts == 1)
      OK ("add_INSERT_block: refuses a copy of the block's struct");
    else
      {
        FAIL ("add_INSERT_block: a copy of the block's struct: %u objects "
              "added",
              (unsigned)(dwg->num_objects - num_objects));
        failures++;
      }
  }
  loglevel = test_loglevel;

  error = write_dwg (WRITTEN_BLOCK, dwg);
  if (error < DWG_ERR_CRITICAL)
    error = read_dwg (WRITTEN_BLOCK, &readback);
  if (error >= DWG_ERR_CRITICAL)
    {
      FAIL ("add_INSERT_block: write or read back 0x%x", error);
      failures++;
    }
  else
    {
      const Dwg_Object *ins = object (&readback, h_insert);
      if (ins && ins->fixedtype == DWG_TYPE_INSERT
          && ref (ins->tio.entity->tio.INSERT->block_header) == h_second)
        OK ("add_INSERT_block: reads back drawing the block given");
      else
        {
          FAIL ("add_INSERT_block: reads back drawing another block");
          failures++;
        }
      failures += check_inserts ("add_INSERT_block, read back", &readback);
      free_dwg (&readback);
    }
  if (!failures)
    unlink (WRITTEN_BLOCK);
  free_dwg (dwg);
  free (dwg);
  free_dwg (other);
  free (other);
  return failures;
}

/* Copies the fixture's definition def into a new r2004 document, returning
   the copy's handle in *copyp. */
static Dwg_Data *
document_with (Dwg_Data *src, const BITCODE_RLL def, BITCODE_RLL *copyp)
{
  Dwg_Data *dst = dwg_new_Document (R_2004, 0, tracelevel);
  Dwg_Object_BLOCK_HEADER *copy;
  int error = 0;
  copy = dwg_dynblock_copy_block (dst, src, block_header (src, def), &error);
  loglevel = test_loglevel;
  if (!copy || error)
    {
      free_dwg (dst);
      free (dst);
      return NULL;
    }
  *copyp = handle_of (dst, copy);
  return dst;
}

/* The ATTRIBs of a placement, which dwg_add_ATTRIB () adds, are linked as
   AutoCAD links them: the INSERT owns its ATTRIBs and SEQEND, with
   hard-owner references to its ATTRIBs and entmode 0 on them, and still
   draws block. */
static int
check_attrib_links (const char *what, const Dwg_Data *dwg,
                    const BITCODE_RLL insert, const BITCODE_RLL block)
{
  const Dwg_Object *ins = object (dwg, insert);
  const Dwg_Entity_INSERT *_ins = ins->tio.entity->tio.INSERT;
  const BITCODE_RLL drawn = ref (_ins->block_header);
  const Dwg_Object *seqend = object (dwg, ref (_ins->seqend));
  unsigned owned_by_insert = 0, n = 0, hard = 0, entmode0 = 0;
  int failures = 0;
  for (BITCODE_BL i = 0; i < _ins->num_owned; i++)
    {
      const Dwg_Object *sub = object (dwg, ref (_ins->attribs[i]));
      if (!sub || sub->fixedtype != DWG_TYPE_ATTRIB)
        continue;
      n++;
      if (ref (sub->tio.entity->ownerhandle) == insert)
        owned_by_insert++;
      if (_ins->attribs[i]->handleref.code == 3)
        hard++;
      if (sub->tio.entity->entmode == 0)
        entmode0++;
    }
  if (n != _ins->num_owned)
    {
      FAIL ("%s: %u of its %u ATTRIBs resolve", what, n,
            (unsigned)_ins->num_owned);
      failures++;
    }
  if (hard == n)
    OK ("%s: hard-owns its %u ATTRIBs", what, n);
  else
    {
      FAIL ("%s: %u of %u ATTRIBs hard-owned", what, hard, n);
      failures++;
    }
  if (entmode0 == n)
    OK ("%s: its ATTRIBs have entmode 0", what);
  else
    {
      FAIL ("%s: %u of %u ATTRIBs with entmode 0", what, entmode0, n);
      failures++;
    }
  if (seqend && seqend->fixedtype == DWG_TYPE_SEQEND
      && ref (seqend->tio.entity->ownerhandle) == insert)
    OK ("%s: owns its SEQEND", what);
  else
    {
      FAIL ("%s: its SEQEND " FORMAT_HV " isn't its own", what,
            ref (_ins->seqend));
      failures++;
    }
  if (drawn == block)
    OK ("%s: still draws its block", what);
  else
    {
      FAIL ("%s: draws " FORMAT_HV ", not " FORMAT_HV, what, drawn, block);
      failures++;
    }
  if (owned_by_insert == n)
    OK ("%s: owns its %u ATTRIBs", what, n);
  else
    {
      FAIL ("%s: %u of %u ATTRIBs owned by the INSERT", what,
            owned_by_insert, n);
      failures++;
    }
  return failures;
}

/* Gives entity ent the AcDbBlockRepETag EED AutoCAD gives the entities of
   a dynamic block definition: 1070 1, the index, and a 1005 handle, of 0
   on an ATTDEF. */
static int
add_etag (Dwg_Data *dwg, Dwg_Object_Entity *ent, const BITCODE_RL index)
{
  BITCODE_H appid
      = dwg_find_tablehandle (dwg, "AcDbBlockRepETag", "APPID");
  const BITCODE_RLL zero = 0;
  Dwg_Eed *eed = (Dwg_Eed *)calloc (3, sizeof (Dwg_Eed));
  if (!appid || !eed || ent->num_eed)
    {
      free (eed);
      return 1;
    }
  eed[0].size = 3 + 5 + 9;
  eed[0].handle.code = 5;
  eed[0].handle.size = 2;
  eed[0].handle.value = appid->absolute_ref;
  for (int i = 0; i < 3; i++)
    eed[i].data = (Dwg_Eed_Data *)calloc (1, sizeof (Dwg_Eed_Data));
  if (!eed[0].data || !eed[1].data || !eed[2].data)
    return 1;
  eed[0].data->code = 70;
  eed[0].data->u.eed_70.rs = 1;
  eed[1].data->code = 71;
  // Dwg_Eed_Data is packed
  memcpy (&eed[1].data->u.eed_71.rl, &index, sizeof (index));
  eed[2].data->code = 5;
  memcpy (&eed[2].data->u.eed_5.entity, &zero, sizeof (zero));
  ent->eed = eed;
  ent->num_eed = 3;
  return 0;
}

/* An expected ATTRIB: its tag and value. */
typedef struct
{
  const char *tag;
  const char *value;
} Expected_Attrib;

/* The ATTRIBs of placement insert in dwg, placed at ins_pt, scale and
   rotation in block (base point 0) of defs, which is dwg or the document
   dwg was written from: one per attribute definition that isn't constant,
   in order, each with its definition's properties placed by the INSERT.
   Text that is left-aligned on the baseline, and a definition that doesn't
   store its alignment point (dataflags 0x02, which the decoder leaves 0),
   take the insertion point as the alignment point, and the ATTRIB then
   doesn't store it either. */
static int
check_attribs (const char *what, const Dwg_Data *dwg, const Dwg_Data *defs,
               const BITCODE_RLL insert, const BITCODE_RLL block,
               const BITCODE_3BD *ins_pt, const double scale,
               const double rotation, const Expected_Attrib *expected,
               const unsigned num_expected, const int etags)
{
  const Dwg_Entity_INSERT *_ins = object (dwg, insert)->tio.entity->tio.INSERT;
  const Dwg_Object_BLOCK_HEADER *blk = block_header (defs, block);
  const BITCODE_3BD *base = &blk->base_pt;
  unsigned k = 0;
  int failures = 0;
  const double c = cos (rotation), s = sin (rotation);
  if (!_ins->has_attribs || _ins->num_owned != num_expected)
    {
      FAIL ("%s: %u ATTRIBs, expected %u", what, (unsigned)_ins->num_owned,
            num_expected);
      return 1;
    }
  for (BITCODE_BL i = 0; i < blk->num_owned; i++)
    {
      const Dwg_Object *defobj = object (defs, ref (blk->entities[i]));
      const Dwg_Entity_ATTDEF *d;
      const Dwg_Object *attobj;
      const Dwg_Entity_ATTRIB *a;
      const BITCODE_2DPOINT *dalign, *aalign;
      double x, y, ax, ay;
      BITCODE_RLL etag_handle = 1, def_etag = 0;
      BITCODE_RL etag_index = 0, def_index = 1;
      int has_etag, def_has_etag;
      if (!defobj || defobj->fixedtype != DWG_TYPE_ATTDEF
          || (defobj->tio.entity->tio.ATTDEF->flags & 2))
        continue;
      d = defobj->tio.entity->tio.ATTDEF;
      attobj = object (dwg, ref (_ins->attribs[k]));
      if (!attobj || attobj->fixedtype != DWG_TYPE_ATTRIB)
        {
          FAIL ("%s: ATTRIB %u missing", what, k);
          return failures + 1;
        }
      a = attobj->tio.entity->tio.ATTRIB;
      dalign = (d->dataflags & 0x02)
                       || (!d->horiz_alignment && !d->vert_alignment)
                   ? &d->ins_pt
                   : &d->alignment_pt;
      aalign = (a->dataflags & 0x02) ? &a->ins_pt : &a->alignment_pt;
      x = ins_pt->x
          + scale
                * ((d->ins_pt.x - base->x) * c - (d->ins_pt.y - base->y) * s);
      y = ins_pt->y
          + scale
                * ((d->ins_pt.x - base->x) * s + (d->ins_pt.y - base->y) * c);
      ax = ins_pt->x
           + scale
                 * ((dalign->x - base->x) * c - (dalign->y - base->y) * s);
      ay = ins_pt->y
           + scale
                 * ((dalign->x - base->x) * s + (dalign->y - base->y) * c);
      has_etag = eed_tag (dwg, attobj, "AcDbBlockRepETag", &etag_handle,
                          &etag_index);
      // at the definition's values, an ATTRIB of a definition with one
      def_has_etag = eed_tag (defs, defobj, "AcDbBlockRepETag", &def_etag,
                              &def_index);
      if (strcmp (a->tag, d->tag) || strcmp (a->tag, expected[k].tag)
          || strcmp (a->text_value, expected[k].value)
          || fabs (a->ins_pt.x - x) > 1e-9 || fabs (a->ins_pt.y - y) > 1e-9
          || fabs (a->elevation - ins_pt->z - scale * d->elevation) > 1e-9
          || fabs (aalign->x - ax) > 1e-9 || fabs (aalign->y - ay) > 1e-9
          || !(a->dataflags & 0x02) != (ax != x || ay != y)
          || fabs (a->height - d->height * scale) > 1e-9
          || fabs (a->rotation - fmod (d->rotation + rotation, 2.0 * M_PI))
                 > 1e-9
          || a->width_factor != d->width_factor
          || a->horiz_alignment != d->horiz_alignment
          || a->vert_alignment != d->vert_alignment || a->flags != d->flags
          || ref (a->style) != ref (d->style)
          || ref (attobj->tio.entity->layer) != ref (defobj->tio.entity->layer)
          || attobj->tio.entity->color.index != defobj->tio.entity->color.index
          || has_etag != (etags && def_has_etag)
          || (has_etag && (etag_handle != 0 || etag_index != def_index)))
        {
          FAIL ("%s: ATTRIB %s '%s' at (%g, %g) aligned at (%g, %g), "
                "dataflags 0x%x, h %g r %g, layer " FORMAT_HV
                ", ETag %d, expected '%s' at (%g, %g) aligned at (%g, %g), h "
                "%g r %g, ETag %d",
                what, a->tag, a->text_value, a->ins_pt.x, a->ins_pt.y,
                aalign->x, aalign->y, (unsigned)a->dataflags, a->height,
                a->rotation, ref (attobj->tio.entity->layer), has_etag,
                expected[k].value, x, y, ax, ay, d->height * scale,
                fmod (d->rotation + rotation, 2.0 * M_PI), etags);
          failures++;
        }
      k++;
    }
  if (!failures)
    OK ("%s: %u ATTRIBs, as their definitions placed by the INSERT", what, k);
  return failures;
}

/* Makes entity handle one that Trees' visibility parameter shows in state
   0, Palm (Plan), only. Returns 0 on success. */
static int
show_in_first_state (Dwg_Data *dwg, const BITCODE_RLL def,
                     const BITCODE_RLL handle)
{
  const Dwg_Object *graph = dict_item (dwg, xdic (dwg, object (dwg, def)),
                                       "ACAD_ENHANCEDBLOCK");
  const Dwg_Object_EVALUATION_GRAPH *g
      = graph ? graph->tio.object->tio.EVALUATION_GRAPH : NULL;
  for (BITCODE_BL i = 0; g && i < g->num_nodes; i++)
    {
      const Dwg_Object *node = object (dwg, ref (g->nodes[i].evalexpr));
      Dwg_Object_BLOCKVISIBILITYPARAMETER *vis;
      Dwg_BLOCKVISIBILITYPARAMETER_state *state;
      if (!node || node->fixedtype != DWG_TYPE_BLOCKVISIBILITYPARAMETER)
        continue;
      vis = node->tio.object->tio.BLOCKVISIBILITYPARAMETER;
      state = &vis->states[0];
      vis->blocks = (BITCODE_H *)realloc (
          vis->blocks, (vis->num_blocks + 1) * sizeof (BITCODE_H));
      state->blocks = (BITCODE_H *)realloc (
          state->blocks, (state->num_blocks + 1) * sizeof (BITCODE_H));
      if (!vis->blocks || !state->blocks)
        return 1;
      vis->blocks[vis->num_blocks++]
          = dwg_add_handleref (dwg, 4, handle, NULL);
      state->blocks[state->num_blocks++]
          = dwg_add_handleref (dwg, 4, handle, NULL);
      return 0;
    }
  return 1;
}

/* Attributes, on a copy of Trees given attribute definitions: a plain one
   that doesn't store its alignment point, as a decoded one with dataflags
   0x02; a constant one, which gets no ATTRIB; one centred, rotated, on a
   layer of its own and shown in the first visibility state only; one
   left-aligned, as dwg_add_ATTDEF () makes it, with an alignment point of
   0; and one centred that doesn't store its alignment point. Placed at the
   definition's values and at another state, scaled and rotated, with one
   value given. */
static int
test_attributes (Dwg_Data *src)
{
  BITCODE_RLL def_h = 0;
  Dwg_Data *dst = document_with (src, TREES, &def_h);
  Dwg_Object_BLOCK_HEADER *def;
  const dwg_point_3d p1 = { 1.0, 2.0, 0.0 }, p2 = { -3.0, 4.0, 0.0 },
                     p3 = { 0.0, -6.0, 0.0 }, p4 = { 7.0, -1.0, 0.0 },
                     p5 = { -5.0, -2.0, 0.0 };
  Dwg_Entity_ATTDEF *number, *constant, *centred, *label, *mark;
  Dwg_Object_LAYER *layer;
  const BITCODE_3BD at1 = { 50.0, 60.0, 0.0 }, at2 = { -20.0, 10.0, 5.0 };
  const Dwg_AttValue given[] = { { (char *)"NUMBER", (char *)"EL-7" } };
  const Expected_Attrib expected1[] = { { "NUMBER", "EL-7" },
                                        { "RATING", "10W" },
                                        { "LABEL", "L1" },
                                        { "MARK", "M" } };
  const Expected_Attrib expected2[] = { { "NUMBER", "#" },
                                        { "RATING", "10W" },
                                        { "LABEL", "L1" },
                                        { "MARK", "M" } };
  Dwg_DynValue v;
  Dwg_Entity_INSERT *ins1, *ins2;
  BITCODE_RLL h1, h2, block2;
  Placement pl1, pl2;
  Dwg_Data readback;
  int failures = 0, error = 0;

  if (!dst)
    {
      FAIL ("attributes: copying Trees failed");
      return 1;
    }
  def = block_header (dst, def_h);
  layer = dwg_add_LAYER (dst, "ATTRIBUTES");
  if (!layer
      || !(number
           = dwg_add_ATTDEF (def, 2.5, 0, "Number", &p1, "NUMBER", "#"))
      || !(constant
           = dwg_add_ATTDEF (def, 2.5, 2, "Constant", &p2, "KIND", "TREE"))
      || !(centred
           = dwg_add_ATTDEF (def, 1.5, 0, "Rating", &p3, "RATING", "10W"))
      || !(label = dwg_add_ATTDEF (def, 1.0, 0, "Label", &p4, "LABEL", "L1"))
      || !(mark = dwg_add_ATTDEF (def, 1.0, 0, "Mark", &p5, "MARK", "M"))
      || add_etag (dst, number->parent, 1001))
    {
      FAIL ("attributes: adding the definitions failed");
      free_dwg (dst);
      free (dst);
      return 1;
    }
  // the add API leaves the flags and width factor 0
  constant->flags = 2;
  number->width_factor = constant->width_factor = 1.0;
  centred->width_factor = 0.8;
  centred->horiz_alignment = 1;
  centred->alignment_pt.x = 4.0;
  centred->alignment_pt.y = -6.0;
  centred->rotation = 0.1;
  centred->parent->layer
      = dwg_add_handleref (dst, 5, handle_of (dst, layer), NULL);
  label->width_factor = mark->width_factor = 1.0;
  // the alignment point is the insertion point, so not stored
  number->dataflags = 0x02;
  mark->dataflags = 0x02;
  mark->horiz_alignment = 1;
  if (show_in_first_state (dst, def_h, handle_of (dst, centred)))
    {
      FAIL ("attributes: Trees has no visibility parameter");
      free_dwg (dst);
      free (dst);
      return 1;
    }

  loglevel = tracelevel;
  ins1 = dwg_dynblock_insert (model_space (dst), def, &at1, 2.0, 0.5, NULL, 0,
                              given, 1, &error);
  memset (&v, 0, sizeof (v));
  v.name = (char *)"Type (view)";
  v.state = (char *)"Shrub (Plan)";
  if (ins1 && !error)
    ins2 = dwg_dynblock_insert (model_space (dst), def, &at2, 1.5, -0.25, &v,
                                1, NULL, 0, &error);
  else
    ins2 = NULL;
  loglevel = test_loglevel;
  if (!ins1 || !ins2 || error)
    {
      FAIL ("attributes: placing failed: 0x%x", error);
      free_dwg (dst);
      free (dst);
      return 1;
    }
  h1 = handle_of (dst, ins1);
  h2 = handle_of (dst, ins2);
  block2 = ref (object (dst, h2)->tio.entity->tio.INSERT->block_header);
  memset (&pl1, 0, sizeof (pl1));
  pl1.insert = h1;
  pl1.def = def_h;
  pl1.at_defaults = 1;
  memset (&pl2, 0, sizeof (pl2));
  pl2.insert = h2;
  pl2.def = def_h;
  pl2.values[0] = v;
  pl2.num_values = 1;
  failures += check_structure ("attributes at the definition's values", dst,
                               &pl1);
  failures += check_structure ("attributes at Shrub (Plan)", dst, &pl2);
  failures
      += check_values ("attributes at the definition's values", dst, &pl1);
  failures += check_values ("attributes at Shrub (Plan)", dst, &pl2);
  failures += check_attribs ("attributes at the definition's values", dst, dst,
                             h1, def_h, &at1, 2.0, 0.5, expected1, 4, 1);
  failures += check_attribs ("attributes at Shrub (Plan)", dst, dst, h2,
                             block2, &at2, 1.5, 2.0 * M_PI - 0.25, expected2,
                             4, 0);
  failures += check_attrib_links ("attributes at the definition's values",
                                  dst, h1, def_h);
  failures
      += check_attrib_links ("attributes at Shrub (Plan)", dst, h2, block2);
  // At Shrub (Plan) the state hides RATING's definition, but not the
  // ATTRIB: AutoCAD keeps its ATTRIB's invisible flag clear then, as its own
  // evaluation of such a placement does.
  {
    const Dwg_Object_BLOCK_HEADER *rep = block_header (dst, block2);
    const Dwg_Object *rating = NULL;
    const Dwg_Entity_INSERT *_ins2 = object (dst, h2)->tio.entity->tio.INSERT;
    // RATING's is the second ATTRIB
    const Dwg_Object *attrib
        = _ins2->num_owned == 4 ? object (dst, ref (_ins2->attribs[1])) : NULL;
    for (BITCODE_BL i = 0; rep && i < rep->num_owned; i++)
      {
        const Dwg_Object *ent = object (dst, ref (rep->entities[i]));
        if (ent && ent->fixedtype == DWG_TYPE_ATTDEF
            && !strcmp (ent->tio.entity->tio.ATTDEF->tag, "RATING"))
          rating = ent;
      }
    if (rating && rating->fixedtype == DWG_TYPE_ATTDEF
        && rating->tio.entity->invisible && attrib
        && !attrib->tio.entity->invisible)
      OK ("attributes at Shrub (Plan): RATING's definition is hidden, its "
          "ATTRIB not");
    else
      {
        FAIL ("attributes at Shrub (Plan): RATING's definition %s, its "
              "ATTRIB %s",
              rating && rating->tio.entity->invisible ? "hidden" : "shown",
              attrib && !attrib->tio.entity->invisible ? "shown"
                                                        : "hidden or missing");
        failures++;
      }
  }
  if (def->hasattrs && block_header (dst, block2)->hasattrs)
    OK ("attributes: the representation block has the definition's "
        "hasattrs");
  else
    {
      FAIL ("attributes: hasattrs %d, the representation block's %d",
            def->hasattrs, block_header (dst, block2)->hasattrs);
      failures++;
    }

  // The encoder works out the dataflags of a document built with the add
  // API again, so NUMBER and MARK, and their copies in the representation
  // block, are written with their alignment points of 0 stored. The
  // ATTRIBs read back are checked against the definitions as they were
  // placed.
  {
    const BITCODE_BL n = dst->num_objects;
    BITCODE_RC *flags = (BITCODE_RC *)calloc (n, 1);
    for (BITCODE_BL i = 0; flags && i < n; i++)
      if (dst->object[i].fixedtype == DWG_TYPE_ATTDEF)
        flags[i] = dst->object[i].tio.entity->tio.ATTDEF->dataflags;
    error = flags ? write_dwg (WRITTEN_ATTRIBS, dst) : DWG_ERR_OUTOFMEM;
    for (BITCODE_BL i = 0; flags && i < n; i++)
      if (dst->object[i].fixedtype == DWG_TYPE_ATTDEF)
        dst->object[i].tio.entity->tio.ATTDEF->dataflags = flags[i];
    free (flags);
  }
  if (error < DWG_ERR_CRITICAL)
    error = read_dwg (WRITTEN_ATTRIBS, &readback);
  if (error >= DWG_ERR_CRITICAL)
    {
      FAIL ("attributes: write or read back 0x%x", error);
      failures++;
    }
  else
    {
      failures += check_structure ("read back attributes at the "
                                   "definition's values",
                                   &readback, &pl1);
      failures += check_structure ("read back attributes at Shrub (Plan)",
                                   &readback, &pl2);
      failures += check_values ("read back attributes at the definition's "
                                "values",
                                &readback, &pl1);
      failures += check_values ("read back attributes at Shrub (Plan)",
                                &readback, &pl2);
      failures += check_attribs ("read back attributes at the definition's "
                                 "values",
                                 &readback, dst, h1, def_h, &at1, 2.0, 0.5,
                                 expected1, 4, 1);
      failures += check_attribs ("read back attributes at Shrub (Plan)",
                                 &readback, dst, h2, block2, &at2, 1.5,
                                 2.0 * M_PI - 0.25, expected2, 4, 0);
      failures += check_attrib_links ("read back attributes at the "
                                      "definition's values",
                                      &readback, h1, def_h);
      failures += check_attrib_links ("read back attributes at Shrub (Plan)",
                                      &readback, h2, block2);
      failures += check_inserts ("read back attributes", &readback);
      failures += check_refs ("read back attributes", &readback);
      free_dwg (&readback);
    }
  if (!failures)
    unlink (WRITTEN_ATTRIBS);
  free_dwg (dst);
  free (dst);
  return failures;
}

/* An IMAGE in a definition: its copy in the representation block gets an
   image definition reactor of its own, which the image definition lists,
   while the definition's IMAGE keeps its own. */
static int
test_image_reactors (Dwg_Data *src)
{
  BITCODE_RLL def_h = 0;
  Dwg_Data *dst = document_with (src, TREES, &def_h);
  const dwg_point_3d pt = { 1.0, 1.0, 0.0 };
  const BITCODE_3BD at = { 0.0, 0.0, 0.0 };
  Dwg_Object_BLOCK_HEADER *def, *rep;
  Dwg_Entity_IMAGE *image;
  Dwg_DynValue v;
  Dwg_Entity_INSERT *ins;
  BITCODE_RLL image_h, reactor_h, rep_image_h, rep_reactor_h, imagedef_h;
  const Dwg_Object *rep_image, *reactor, *imagedef;
  BITCODE_BL index;
  int failures = 0, error = 0, listed = 0;

  if (!dst)
    {
      FAIL ("image reactors: copying Trees failed");
      return 1;
    }
  def = block_header (dst, def_h);
  image = dwg_add_IMAGE (def, "tree.png", &pt, 1.0, 0.0);
  if (!image)
    {
      FAIL ("image reactors: adding the IMAGE failed");
      free_dwg (dst);
      free (dst);
      return 1;
    }
  image_h = handle_of (dst, image);
  reactor_h = ref (image->imagedefreactor);
  imagedef_h = ref (image->imagedef);
  index = def->num_owned - 1;
  memset (&v, 0, sizeof (v));
  v.name = (char *)"Type (view)";
  v.state = (char *)"Shrub (Plan)";
  loglevel = tracelevel;
  ins = dwg_dynblock_insert (model_space (dst), def, &at, 1.0, 0.0, &v, 1,
                             NULL, 0, &error);
  loglevel = test_loglevel;
  rep = ins ? block_header (dst, ref (ins->block_header)) : NULL;
  if (!rep || error || rep->num_owned != def->num_owned)
    {
      FAIL ("image reactors: placing failed: 0x%x", error);
      free_dwg (dst);
      free (dst);
      return 1;
    }
  rep_image_h = ref (rep->entities[index]);
  rep_image = object (dst, rep_image_h);
  rep_reactor_h = rep_image && rep_image->fixedtype == DWG_TYPE_IMAGE
                      ? ref (rep_image->tio.entity->tio.IMAGE->imagedefreactor)
                      : 0;
  reactor = object (dst, rep_reactor_h);
  imagedef = object (dst, imagedef_h);
  for (BITCODE_BL i = 0; imagedef && i < imagedef->tio.object->num_reactors;
       i++)
    listed |= ref (imagedef->tio.object->reactors[i]) == rep_reactor_h;
  if (reactor && rep_reactor_h != reactor_h
      && reactor->fixedtype == DWG_TYPE_IMAGEDEF_REACTOR
      && ref (reactor->tio.object->ownerhandle) == rep_image_h
      && ref (object (dst, reactor_h)->tio.object->ownerhandle) == image_h
      && ref (rep_image->tio.entity->tio.IMAGE->imagedef) == imagedef_h
      && listed)
    OK ("image reactors: the representation block's IMAGE has its own, "
        "which the image definition lists");
  else
    {
      FAIL ("image reactors: the copy's reactor " FORMAT_HV
            ", the definition's " FORMAT_HV ", listed %d",
            rep_reactor_h, reactor_h, listed);
      failures++;
    }
  failures += check_refs ("image reactors", dst);
  free_dwg (dst);
  free (dst);
  return failures;
}

/* A sort table's entries are mapped to the representation block's copies,
   and one for an entity that isn't the definition's is left out: Trees'
   empty table is given an entry for its first entity, and one for a line
   in model space. */
static int
test_sortents_entries (Dwg_Data *src)
{
  BITCODE_RLL def_h = 0;
  Dwg_Data *dst = document_with (src, TREES, &def_h);
  const BITCODE_3BD at = { 0.0, 0.0, 0.0 };
  const dwg_point_3d p1 = { 0.0, 0.0, 0.0 }, p2 = { 1.0, 1.0, 0.0 };
  Dwg_Object_BLOCK_HEADER *def;
  const Dwg_Object *table, *rep_table;
  Dwg_Object_SORTENTSTABLE *t;
  Dwg_Entity_LINE *line;
  Dwg_Entity_INSERT *ins;
  Dwg_Object_BLOCK_HEADER *rep;
  Dwg_DynValue v;
  BITCODE_RLL first_h, line_h;
  int failures = 0, error = 0;

  if (!dst)
    {
      FAIL ("sort table entries: copying Trees failed");
      return 1;
    }
  def = block_header (dst, def_h);
  table = dict_item (dst, xdic (dst, object (dst, def_h)), "ACAD_SORTENTS");
  line = dwg_add_LINE (model_space (dst), &p1, &p2);
  loglevel = test_loglevel;
  if (!table || !line || table->tio.object->tio.SORTENTSTABLE->num_ents)
    {
      FAIL ("sort table entries: Trees' empty sort table not found");
      free_dwg (dst);
      free (dst);
      return 1;
    }
  first_h = ref (def->entities[0]);
  line_h = handle_of (dst, line);
  t = table->tio.object->tio.SORTENTSTABLE;
  t->num_ents = 2;
  t->ents = (BITCODE_H *)calloc (2, sizeof (BITCODE_H));
  t->sort_ents = (BITCODE_H *)calloc (2, sizeof (BITCODE_H));
  t->ents[0] = dwg_add_handleref (dst, 4, first_h, NULL);
  t->ents[1] = dwg_add_handleref (dst, 4, line_h, NULL);
  t->sort_ents[0] = dwg_add_handleref (dst, 0, first_h, NULL);
  t->sort_ents[1] = dwg_add_handleref (dst, 0, line_h, NULL);

  memset (&v, 0, sizeof (v));
  v.name = (char *)"Type (view)";
  v.state = (char *)"Shrub (Plan)";
  loglevel = tracelevel;
  ins = dwg_dynblock_insert (model_space (dst), def, &at, 1.0, 0.0, &v, 1,
                             NULL, 0, &error);
  loglevel = test_loglevel;
  rep = ins ? block_header (dst, ref (ins->block_header)) : NULL;
  rep_table
      = rep ? dict_item (dst, xdic (dst, &dst->object[rep->parent->objid]),
                         "ACAD_SORTENTS")
            : NULL;
  if (rep_table && !error
      && rep_table->tio.object->tio.SORTENTSTABLE->num_ents == 1
      && ref (rep_table->tio.object->tio.SORTENTSTABLE->ents[0])
             == ref (rep->entities[0])
      && ref (rep_table->tio.object->tio.SORTENTSTABLE->sort_ents[0])
             == ref (rep->entities[0]))
    OK ("sort table entries: mapped to the copies, the one outside the "
        "definition left out");
  else
    {
      FAIL ("sort table entries: 0x%x, %u entries", error,
            rep_table ? (unsigned)rep_table->tio.object->tio.SORTENTSTABLE
                            ->num_ents
                      : 0);
      failures++;
    }
  free_dwg (dst);
  free (dst);
  return failures;
}

/* The places of blk's entities, at most max, in the order blk draws them
   by its sort table t: an entity by the key of its entry, or else by its
   own handle. Returns the number of places. */
static BITCODE_BL
draw_order (const Dwg_Object_BLOCK_HEADER *blk,
            const Dwg_Object_SORTENTSTABLE *t, BITCODE_BL *places,
            BITCODE_RLL *keys, const BITCODE_BL max)
{
  const BITCODE_BL n = blk->num_owned < max ? blk->num_owned : max;
  for (BITCODE_BL i = 0; i < n; i++)
    {
      keys[i] = ref (blk->entities[i]);
      for (BITCODE_BL j = 0; t->ents && t->sort_ents && j < t->num_ents; j++)
        if (ref (t->ents[j]) == ref (blk->entities[i]))
          {
            keys[i] = ref (t->sort_ents[j]);
            break;
          }
      places[i] = i;
    }
  // by key, then by place
  for (BITCODE_BL i = 1; i < n; i++)
    for (BITCODE_BL j = i; j > 0 && keys[places[j - 1]] > keys[places[j]];
         j--)
      {
        const BITCODE_BL place = places[j];
        places[j] = places[j - 1];
        places[j - 1] = place;
      }
  return n;
}

#define MAX_SORTED 4096

/* Checks that rep, the representation block of def, draws its copies in
   the order def draws its entities, and that each of its sort keys is the
   handle of one of its copies. */
static int
check_draw_order (const char *what, const Dwg_Data *dwg,
                  const BITCODE_RLL def, const BITCODE_RLL rep)
{
  static BITCODE_BL def_places[MAX_SORTED], rep_places[MAX_SORTED];
  static BITCODE_RLL def_keys[MAX_SORTED], rep_keys[MAX_SORTED];
  const Dwg_Object_BLOCK_HEADER *defblk = block_header (dwg, def);
  const Dwg_Object_BLOCK_HEADER *repblk = block_header (dwg, rep);
  const Dwg_Object *def_table
      = defblk ? dict_item (dwg, xdic (dwg, object (dwg, def)),
                            "ACAD_SORTENTS")
               : NULL;
  const Dwg_Object *rep_table
      = repblk ? dict_item (dwg, xdic (dwg, object (dwg, rep)),
                            "ACAD_SORTENTS")
               : NULL;
  const Dwg_Object_SORTENTSTABLE *dt, *rt;
  BITCODE_BL n, i, foreign = 0;

  if (!def_table || !rep_table
      || def_table->fixedtype != DWG_TYPE_SORTENTSTABLE
      || rep_table->fixedtype != DWG_TYPE_SORTENTSTABLE
      || defblk->num_owned != repblk->num_owned
      || defblk->num_owned > MAX_SORTED)
    {
      FAIL ("%s: no sort tables, or the blocks differ in size", what);
      return 1;
    }
  dt = def_table->tio.object->tio.SORTENTSTABLE;
  rt = rep_table->tio.object->tio.SORTENTSTABLE;
  n = draw_order (defblk, dt, def_places, def_keys, MAX_SORTED);
  draw_order (repblk, rt, rep_places, rep_keys, MAX_SORTED);
  for (i = 0; i < n && def_places[i] == rep_places[i]; i++)
    ;
  for (BITCODE_BL j = 0; j < rt->num_ents; j++)
    {
      const Dwg_Object *ent = object (dwg, ref (rt->sort_ents[j]));
      if (!ent || ent->supertype != DWG_SUPERTYPE_ENTITY
          || ref (ent->tio.entity->ownerhandle) != rep)
        foreign++;
    }
  if (i == n && !foreign)
    {
      OK ("%s: drawn in the definition's order, %u entries", what,
          (unsigned)rt->num_ents);
      return 0;
    }
  if (i < n)
    FAIL ("%s: draws entity %u where the definition draws entity %u", what,
          (unsigned)rep_places[i], (unsigned)def_places[i]);
  if (foreign)
    FAIL ("%s: %u sort keys aren't the handles of its entities", what,
          (unsigned)foreign);
  return 1;
}

/* The sort keys of a definition's table may be handle values that name no
   entity of the definition, as the key of an erased entity does; the
   representation block still draws its copies in the definition's order.
   Trees' table is given three entries: its first entity sorted by its
   third's handle, its third by its first's, and its second by the handle
   of a line in model space, which follows all of Trees' handles and isn't
   copied, so that Trees draws its second entity last. */
static int
test_sortents_order (Dwg_Data *src)
{
  BITCODE_RLL def_h = 0;
  Dwg_Data *dst = document_with (src, TREES, &def_h);
  const BITCODE_3BD at = { 0.0, 0.0, 0.0 };
  const dwg_point_3d p1 = { 0.0, 0.0, 0.0 }, p2 = { 1.0, 1.0, 0.0 };
  static BITCODE_BL places[MAX_SORTED];
  static BITCODE_RLL keys[MAX_SORTED];
  Dwg_Object_BLOCK_HEADER *def;
  const Dwg_Object *table;
  Dwg_Object_SORTENTSTABLE *t;
  Dwg_Entity_LINE *line;
  Dwg_Entity_INSERT *ins;
  Dwg_DynValue v;
  Dwg_Data readback;
  BITCODE_RLL h[3], line_h, rep_h = 0;
  BITCODE_BL n;
  int failures = 0, error = 0;

  if (!dst)
    {
      FAIL ("sort order: copying Trees failed");
      return 1;
    }
  def = block_header (dst, def_h);
  table = dict_item (dst, xdic (dst, object (dst, def_h)), "ACAD_SORTENTS");
  line = dwg_add_LINE (model_space (dst), &p1, &p2);
  loglevel = test_loglevel;
  if (!table || !line || table->tio.object->tio.SORTENTSTABLE->num_ents
      || def->num_owned < 4)
    {
      FAIL ("sort order: Trees' empty sort table not found");
      free_dwg (dst);
      free (dst);
      return 1;
    }
  for (int i = 0; i < 3; i++)
    h[i] = ref (def->entities[i]);
  line_h = handle_of (dst, line);
  t = table->tio.object->tio.SORTENTSTABLE;
  t->num_ents = 3;
  t->ents = (BITCODE_H *)calloc (3, sizeof (BITCODE_H));
  t->sort_ents = (BITCODE_H *)calloc (3, sizeof (BITCODE_H));
  t->ents[0] = dwg_add_handleref (dst, 4, h[0], NULL);
  t->sort_ents[0] = dwg_add_handleref (dst, 0, h[2], NULL);
  t->ents[1] = dwg_add_handleref (dst, 4, h[2], NULL);
  t->sort_ents[1] = dwg_add_handleref (dst, 0, h[0], NULL);
  t->ents[2] = dwg_add_handleref (dst, 4, h[1], NULL);
  t->sort_ents[2] = dwg_add_handleref (dst, 0, line_h, NULL);
  // the setup: Trees draws its third entity first and its second last
  n = draw_order (def, t, places, keys, MAX_SORTED);
  if (n != def->num_owned || places[0] != 2 || places[1] != 0
      || places[n - 1] != 1)
    {
      FAIL ("sort order: Trees' draw order isn't the one set up");
      free_dwg (dst);
      free (dst);
      return 1;
    }

  memset (&v, 0, sizeof (v));
  v.name = (char *)"Type (view)";
  v.state = (char *)"Shrub (Plan)";
  loglevel = tracelevel;
  ins = dwg_dynblock_insert (model_space (dst), def, &at, 1.0, 0.0, &v, 1,
                             NULL, 0, &error);
  loglevel = test_loglevel;
  if (!ins || error)
    {
      FAIL ("sort order: placing failed: 0x%x", error);
      free_dwg (dst);
      free (dst);
      return 1;
    }
  rep_h = ref (ins->block_header);
  failures += check_draw_order ("sort order", dst, def_h, rep_h);

  error = write_dwg (WRITTEN_SORT, dst);
  if (error < DWG_ERR_CRITICAL)
    error = read_dwg (WRITTEN_SORT, &readback);
  if (error >= DWG_ERR_CRITICAL)
    {
      FAIL ("sort order: write or read back 0x%x", error);
      failures++;
    }
  else
    {
      failures
          += check_draw_order ("sort order, read back", &readback, def_h, rep_h);
      free_dwg (&readback);
    }
  if (!failures)
    unlink (WRITTEN_SORT);
  free_dwg (dst);
  free (dst);
  return failures;
}

/* A definition without a sort table gives a representation block without
   one: Trees' ACAD_SORTENTS entry is taken out of its dictionary. */
static int
test_without_sortents (Dwg_Data *src)
{
  BITCODE_RLL def_h = 0;
  Dwg_Data *dst = document_with (src, TREES, &def_h);
  const BITCODE_3BD at = { 0.0, 0.0, 0.0 };
  const Dwg_Object *dict;
  Dwg_Object_DICTIONARY *d;
  Dwg_DynValue v;
  Placement pl;
  Dwg_Entity_INSERT *ins;
  BITCODE_BL k;
  int failures = 0, error = 0;

  if (!dst)
    {
      FAIL ("no sort table: copying Trees failed");
      return 1;
    }
  dict = xdic (dst, object (dst, def_h));
  d = dict ? dict->tio.object->tio.DICTIONARY : NULL;
  for (k = 0; d && k < d->numitems; k++)
    if (!strcmp (d->texts[k], "ACAD_SORTENTS"))
      break;
  if (!d || k == d->numitems)
    {
      FAIL ("no sort table: Trees has no ACAD_SORTENTS");
      free_dwg (dst);
      free (dst);
      return 1;
    }
  // the entry moves to the end, out of numitems
  {
    BITCODE_T text = d->texts[k];
    BITCODE_H item = d->itemhandles[k];
    d->texts[k] = d->texts[d->numitems - 1];
    d->itemhandles[k] = d->itemhandles[d->numitems - 1];
    d->texts[d->numitems - 1] = text;
    d->itemhandles[d->numitems - 1] = item;
    d->numitems--;
  }
  memset (&v, 0, sizeof (v));
  v.name = (char *)"Type (view)";
  v.state = (char *)"Shrub (Plan)";
  loglevel = tracelevel;
  ins = dwg_dynblock_insert (model_space (dst), block_header (dst, def_h), &at,
                             1.0, 0.0, &v, 1, NULL, 0, &error);
  loglevel = test_loglevel;
  d->numitems++;
  if (!ins || error)
    {
      FAIL ("no sort table: placing failed: 0x%x", error);
      failures++;
    }
  else
    {
      d->numitems--;
      memset (&pl, 0, sizeof (pl));
      pl.insert = handle_of (dst, ins);
      pl.def = def_h;
      failures += check_structure ("no sort table", dst, &pl);
      d->numitems++;
    }
  free_dwg (dst);
  free (dst);
  return failures;
}

/* dwg->num_entities counts every entity the placements add: the
   representation block's, and the INSERTs, ATTRIBs and SEQENDs the add API
   adds. dwg_get_entities () sizes its array by it. */
static int
test_entity_count (Dwg_Data *src)
{
  Dwg_Data *dst = dwg_new_Document (R_2004, 0, tracelevel);
  BITCODE_RLL map[8][2];
  unsigned num_map, added = 0, attribs = 0, seqends = 0;
  BITCODE_BL counted, first;
  const BITCODE_3BD at = { 0.0, 0.0, 0.0 };
  const dwg_point_3d pt = { 1.0, 2.0, 0.0 };
  Dwg_Object_BLOCK_HEADER *trees;
  Dwg_DynValue v;
  int failures, error = 0;

  loglevel = test_loglevel;
  failures = copy_definitions (dst, src, map, &num_map);
  trees = block_header (dst, copy_of (map, num_map, TREES));
  // so that the placement of Trees adds an ATTRIB and a SEQEND
  if (!trees || !dwg_add_ATTDEF (trees, 2.5, 0, "Number", &pt, "NUMBER", "#"))
    {
      FAIL ("entity count: adding an attribute definition to Trees failed");
      free_dwg (dst);
      free (dst);
      return failures + 1;
    }
  counted = dst->num_entities;
  first = dst->num_objects;
  memset (&v, 0, sizeof (v));
  v.name = (char *)"Type (view)";
  v.state = (char *)"Shrub (Plan)";
  loglevel = tracelevel;
  if (!dwg_dynblock_insert (model_space (dst), trees, &at, 1.0, 0.0, &v, 1,
                            NULL, 0, &error)
      || !dwg_dynblock_insert (model_space (dst),
                               block_header (dst,
                                             copy_of (map, num_map, WINDOW)),
                               &at, 1.0, 0.0, NULL, 0, NULL, 0, &error))
    {
      loglevel = test_loglevel;
      FAIL ("entity count: placing failed: 0x%x", error);
      free_dwg (dst);
      free (dst);
      return failures + 1;
    }
  loglevel = test_loglevel;
  for (BITCODE_BL i = first; i < dst->num_objects; i++)
    if (dst->object[i].supertype == DWG_SUPERTYPE_ENTITY)
      {
        added++;
        attribs += dst->object[i].fixedtype == DWG_TYPE_ATTRIB;
        seqends += dst->object[i].fixedtype == DWG_TYPE_SEQEND;
      }
  if (dst->num_entities - counted == added && attribs == 1 && seqends == 1)
    OK ("entity count: the %u entities the placements add are counted, "
        "their ATTRIB and SEQEND too",
        added);
  else
    {
      FAIL ("entity count: %u entities added, %u counted, %u ATTRIBs and %u "
            "SEQENDs added, expected 1 of each",
            added, (unsigned)(dst->num_entities - counted), attribs, seqends);
      failures++;
    }
  // the array's last entry is the document's last entity
  {
    Dwg_Object_Entity **entities = dwg_get_entities (dst);
    const Dwg_Object_Entity *last = NULL;
    for (BITCODE_BL i = 0; i < dst->num_objects; i++)
      if (dst->object[i].supertype == DWG_SUPERTYPE_ENTITY)
        last = dst->object[i].tio.entity;
    if (entities && dst->num_entities && entities[dst->num_entities - 1] == last)
      OK ("entity count: dwg_get_entities () holds all %u entities",
          (unsigned)dst->num_entities);
    else
      {
        FAIL ("entity count: dwg_get_entities () doesn't end with the last "
              "entity");
        failures++;
      }
    free (entities);
  }
  free_dwg (dst);
  free (dst);
  return failures;
}

/* The evaluation graph of definition def. */
static Dwg_Object_EVALUATION_GRAPH *
evaluation_graph (const Dwg_Data *dwg, const BITCODE_RLL def)
{
  const Dwg_Object *graph = dict_item (dwg, xdic (dwg, object (dwg, def)),
                                       "ACAD_ENHANCEDBLOCK");
  return graph && graph->fixedtype == DWG_TYPE_EVALUATION_GRAPH
             ? graph->tio.object->tio.EVALUATION_GRAPH
             : NULL;
}

/* The first node of class type in def's evaluation graph, with its place
   in the graph in *indexp, or NULL. */
static Dwg_Object *
graph_node (const Dwg_Data *dwg, const BITCODE_RLL def,
            const Dwg_Object_Type type, BITCODE_BL *indexp)
{
  const Dwg_Object_EVALUATION_GRAPH *g = evaluation_graph (dwg, def);
  for (BITCODE_BL i = 0; g && g->nodes && i < g->num_nodes; i++)
    {
      Dwg_Object *node = object (dwg, ref (g->nodes[i].evalexpr));
      if (node && node->fixedtype == type)
        {
          if (indexp)
            *indexp = i;
          return node;
        }
    }
  return NULL;
}

/* Places def in dwg's model space with the given values, which must
   succeed. */
static int
test_accepted (Dwg_Data *dwg, const BITCODE_RLL def, const double scale,
               const Dwg_DynValue *values, const BITCODE_BL n,
               const char *what)
{
  const BITCODE_3BD pt = { 0.0, 0.0, 0.0 };
  int error = 0;
  Dwg_Entity_INSERT *insert;
  loglevel = tracelevel;
  insert = dwg_dynblock_insert (model_space (dwg), block_header (dwg, def),
                                &pt, scale, 0.0, values, n, NULL, 0, &error);
  loglevel = test_loglevel;
  if (insert && !error)
    {
      OK ("insert accepts %s", what);
      return 0;
    }
  FAIL ("insert %s: 0x%x", what, error);
  return 1;
}

static void
free_document (Dwg_Data *dwg)
{
  if (dwg)
    {
      free_dwg (dwg);
      free (dwg);
    }
}

/* Refusals of what a definition holds, each on a copy of the definition
   in a document of its own: a rotate or scale action whose base point is
   not (0, 0, 0), a graph node of a class whose stored value is not known,
   and a DIMENSION, which draws a block of its own, all only at other
   values than the definition's; and attribute definitions off the XY
   plane, attribute values given twice or without a tag or value, and
   scales at which an attribute's placed point, height or thickness is
   beyond what a double holds. */
static int
test_definition_refusals (Dwg_Data *src)
{
  const BITCODE_3BD pt = { 0.0, 0.0, 0.0 };
  const struct
  {
    BITCODE_RLL def;
    Dwg_Object_Type type;
    const char *property, *state, *what;
  } actions[] = {
    { TOILET, DWG_TYPE_BLOCKROTATEACTION, "Type (view)", "Round (Plan)",
      "a rotate action whose base point isn't (0, 0, 0)" },
    { DOOR, DWG_TYPE_BLOCKSCALEACTION, "Opening Angle", "Closed",
      "a scale action whose base point isn't (0, 0, 0)" },
  };
  Dwg_DynValue v;
  BITCODE_RLL def_h = 0;
  Dwg_Data *dst;
  int failures = 0;

  for (size_t i = 0; i < sizeof (actions) / sizeof (actions[0]); i++)
    {
      Dwg_Object *node;
      BITCODE_3BD *base;
      dst = document_with (src, actions[i].def, &def_h);
      node = dst ? graph_node (dst, def_h, actions[i].type, NULL) : NULL;
      if (!node)
        {
          FAIL ("%s: the definition has no such action", actions[i].what);
          failures++;
          free_document (dst);
          continue;
        }
      base = actions[i].type == DWG_TYPE_BLOCKROTATEACTION
                 ? &node->tio.object->tio.BLOCKROTATEACTION->base_pt
                 : &node->tio.object->tio.BLOCKSCALEACTION->base_pt;
      if (base->x != 0.0 || base->y != 0.0 || base->z != 0.0)
        {
          FAIL ("%s: AutoCAD's base point is not (0, 0, 0)",
                actions[i].what);
          failures++;
        }
      base->x = 1.0;
      memset (&v, 0, sizeof (v));
      v.name = (char *)actions[i].property;
      v.state = (char *)actions[i].state;
      failures += test_refusal (dst, model_space (dst),
                                block_header (dst, def_h), &pt, 1.0, 0.0, &v,
                                1, NULL, 0, REFUSED (DWG_ERR_NOTYETSUPPORTED),
                                actions[i].what);
      // at the definition's values no stored values are written
      {
        char what[256];
        snprintf (what, sizeof (what), "%s, at the definition's values",
                  actions[i].what);
        failures += test_accepted (dst, def_h, 1.0, NULL, 0, what);
      }
      free_document (dst);
    }

  memset (&v, 0, sizeof (v));
  v.name = (char *)"Type (view)";
  v.state = (char *)"Shrub (Plan)";
  // a graph node of a class with no known stored value: Trees' visibility
  // grip, made to name the definition's extension dictionary
  dst = document_with (src, TREES, &def_h);
  {
    BITCODE_BL index = 0;
    Dwg_Object_EVALUATION_GRAPH *g
        = dst ? evaluation_graph (dst, def_h) : NULL;
    if (g && graph_node (dst, def_h, DWG_TYPE_BLOCKVISIBILITYGRIP, &index))
      {
        g->nodes[index].evalexpr = dwg_add_handleref (
            dst, 4, ref (object (dst, def_h)->tio.object->xdicobjhandle),
            NULL);
        failures += test_refusal (
            dst, model_space (dst), block_header (dst, def_h), &pt, 1.0, 0.0,
            &v, 1, NULL, 0, REFUSED (DWG_ERR_NOTYETSUPPORTED),
            "a graph node of a class with no known stored value");
        failures += test_accepted (dst, def_h, 1.0, NULL, 0,
                                   "a graph node of a class with no known "
                                   "stored value, at the definition's "
                                   "values");
      }
    else
      {
        FAIL ("Trees has no visibility grip");
        failures++;
      }
  }
  free_document (dst);

  // a DIMENSION, which draws a block of its own
  dst = document_with (src, TREES, &def_h);
  {
    const dwg_point_3d p1 = { 0.0, 0.0, 0.0 }, p2 = { 10.0, 0.0, 0.0 },
                       p3 = { 5.0, 5.0, 0.0 };
    Dwg_Object_BLOCK_HEADER *blk = dst ? add_anonymous_block (dst) : NULL;
    Dwg_Entity_DIMENSION_LINEAR *dim
        = blk ? dwg_add_DIMENSION_LINEAR (block_header (dst, def_h), &p1, &p2,
                                          &p3, 0.0)
              : NULL;
    loglevel = test_loglevel;
    if (dim)
      {
        dim->block = dwg_add_handleref (dst, 5, handle_of (dst, blk), NULL);
        failures += test_refusal (dst, model_space (dst),
                                  block_header (dst, def_h), &pt, 1.0, 0.0,
                                  &v, 1, NULL, 0,
                                  REFUSED (DWG_ERR_NOTYETSUPPORTED),
                                  "a DIMENSION at another state");
        failures += test_accepted (dst, def_h, 1.0, NULL, 0,
                                   "a DIMENSION at the definition's values");
      }
    else
      {
        FAIL ("adding a DIMENSION to Trees failed");
        failures++;
      }
  }
  free_document (dst);

  // attribute definitions and values
  dst = document_with (src, TREES, &def_h);
  {
    Dwg_Object_BLOCK_HEADER *def = dst ? block_header (dst, def_h) : NULL;
    const dwg_point_3d at = { def ? def->base_pt.x : 0.0,
                              def ? def->base_pt.y : 0.0, 0.0 };
    Dwg_Entity_ATTDEF *attdef
        = def ? dwg_add_ATTDEF (def, 1.0, 0, "Number", &at, "NUMBER", "#")
              : NULL;
    Dwg_AttValue a[2];
    loglevel = test_loglevel;
    if (attdef)
      {
        memset (a, 0, sizeof (a));
        a[0].tag = a[1].tag = (char *)"NUMBER";
        a[0].value = (char *)"1";
        a[1].value = (char *)"2";
        failures += test_refusal (dst, model_space (dst), def, &pt, 1.0, 0.0,
                                  NULL, 0, a, 2,
                                  REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                                  "an attribute given twice");
        a[0].tag = NULL;
        failures += test_refusal (dst, model_space (dst), def, &pt, 1.0, 0.0,
                                  NULL, 0, a, 1,
                                  REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                                  "an attribute without a tag");
        a[0].tag = (char *)"NUMBER";
        a[0].value = NULL;
        failures += test_refusal (dst, model_space (dst), def, &pt, 1.0, 0.0,
                                  NULL, 0, a, 1,
                                  REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                                  "an attribute without a value");

        // at a scale of 1e300, a point 1e10 from the base point
        attdef->ins_pt.x = at.x + 1e10;
        attdef->height = 1e-10;
        failures += test_refusal (dst, model_space (dst), def, &pt, 1e300,
                                  0.0, NULL, 0, NULL, 0,
                                  REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                                  "an attribute placed beyond a double");
        attdef->ins_pt.x = at.x;
        attdef->height = 1e10;
        failures += test_refusal (dst, model_space (dst), def, &pt, 1e300,
                                  0.0, NULL, 0, NULL, 0,
                                  REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                                  "an attribute height beyond a double");
        attdef->height = 1.0;
        attdef->thickness = 1e10;
        failures += test_refusal (dst, model_space (dst), def, &pt, 1e300,
                                  0.0, NULL, 0, NULL, 0,
                                  REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                                  "an attribute thickness beyond a double");
        attdef->thickness = 0.0;
        failures += test_accepted (dst, def_h, 1e300, NULL, 0,
                                   "an attribute at the base point, at a "
                                   "scale of 1e300");

        attdef->extrusion.z = -1.0;
        failures += test_refusal (dst, model_space (dst), def, &pt, 1.0, 0.0,
                                  NULL, 0, NULL, 0,
                                  REFUSED (DWG_ERR_NOTYETSUPPORTED),
                                  "an attribute definition off the XY "
                                  "plane");
      }
    else
      {
        FAIL ("adding an attribute definition to Trees failed");
        failures++;
      }
  }
  free_document (dst);

  // a drawing other than r2004
  dst = document_with (src, TREES, &def_h);
  if (dst)
    {
      dst->header.from_version = R_2000;
      failures += test_refusal (dst, model_space (dst),
                                block_header (dst, def_h), &pt, 1.0, 0.0,
                                NULL, 0, NULL, 0,
                                REFUSED (DWG_ERR_NOTYETSUPPORTED),
                                "a drawing other than r2004");
      dst->header.from_version = R_2004;
      free_document (dst);
    }
  else
    {
      FAIL ("copying Trees failed");
      failures++;
    }
  return failures;
}

/* A definition holding a MINSERT, placed at another state: the
   representation block's copy of the MINSERT is listed by the block it
   draws, before and after a write and read back, and that block can't be
   the placement's owner. */
static int
test_minsert (Dwg_Data *src)
{
  BITCODE_RLL def_h = 0, grid_h = 0, minsert_h = 0, copy_h = 0;
  Dwg_Data *dst = document_with (src, TREES, &def_h);
  const dwg_point_3d start = { 0.0, 0.0, 0.0 }, end = { 1.0, 1.0, 0.0 },
                     at = { 2.0, 3.0, 0.0 };
  const BITCODE_3BD pt = { 0.0, 0.0, 0.0 };
  Dwg_Object_BLOCK_HEADER *grid;
  Dwg_Entity_MINSERT *minsert = NULL;
  Dwg_Entity_INSERT *ins;
  Dwg_DynValue v;
  Placement pl;
  Dwg_Data readback;
  int failures = 0, error = 0;

  if (!dst)
    {
      FAIL ("minsert: copying Trees failed");
      return 1;
    }
  grid = dwg_add_BLOCK_HEADER (dst, "GRID");
  if (grid && dwg_add_BLOCK (grid, "GRID")
      && dwg_add_LINE (grid, &start, &end) && dwg_add_ENDBLK (grid))
    {
      grid_h = handle_of (dst, grid);
      minsert = dwg_add_MINSERT (block_header (dst, def_h), &at, "GRID", 1.0,
                                 1.0, 1.0, 0.0, 2, 3, 10.0, 10.0);
    }
  loglevel = test_loglevel;
  if (!minsert)
    {
      FAIL ("minsert: adding the MINSERT to Trees failed");
      free_document (dst);
      return 1;
    }
  minsert_h = handle_of (dst, minsert);
  failures += test_refusal (dst, block_header (dst, grid_h),
                            block_header (dst, def_h), &pt, 1.0, 0.0, NULL, 0,
                            NULL, 0, REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "an owner the definition draws with a MINSERT");
  memset (&v, 0, sizeof (v));
  v.name = (char *)"Type (view)";
  v.state = (char *)"Shrub (Plan)";
  loglevel = tracelevel;
  ins = dwg_dynblock_insert (model_space (dst), block_header (dst, def_h), &pt,
                             1.0, 0.0, &v, 1, NULL, 0, &error);
  loglevel = test_loglevel;
  if (!ins || error)
    {
      FAIL ("minsert: placing failed: 0x%x", error);
      free_document (dst);
      return failures + 1;
    }
  memset (&pl, 0, sizeof (pl));
  pl.insert = handle_of (dst, ins);
  pl.def = def_h;
  pl.values[0] = v;
  pl.num_values = 1;
  failures += check_structure ("minsert", dst, &pl);
  // the MINSERT's copy, at its place in the representation block
  {
    const Dwg_Object_BLOCK_HEADER *def = block_header (dst, def_h);
    const Dwg_Object_BLOCK_HEADER *rep
        = block_header (dst, ref (ins->block_header));
    for (BITCODE_BL i = 0; rep && i < def->num_owned && i < rep->num_owned;
         i++)
      if (ref (def->entities[i]) == minsert_h)
        copy_h = ref (rep->entities[i]);
  }
  grid = block_header (dst, grid_h);
  if (copy_h && object (dst, copy_h)
      && object (dst, copy_h)->fixedtype == DWG_TYPE_MINSERT
      && grid->num_inserts == 2 && ref (grid->inserts[0]) == minsert_h
      && ref (grid->inserts[1]) == copy_h)
    OK ("minsert: GRID lists the definition's MINSERT and its copy");
  else
    {
      FAIL ("minsert: GRID lists %u MINSERTs, the copy " FORMAT_HV,
            (unsigned)grid->num_inserts, copy_h);
      failures++;
    }
  failures += check_inserts ("minsert", dst);

  error = write_dwg (WRITTEN_MINSERT, dst);
  if (error < DWG_ERR_CRITICAL)
    error = read_dwg (WRITTEN_MINSERT, &readback);
  if (error >= DWG_ERR_CRITICAL)
    {
      FAIL ("minsert: write or read back 0x%x", error);
      failures++;
    }
  else
    {
      failures += check_structure ("read back minsert", &readback, &pl);
      failures += check_inserts ("read back minsert", &readback);
      failures += check_refs ("read back minsert", &readback);
      free_dwg (&readback);
    }
  if (!failures)
    unlink (WRITTEN_MINSERT);
  free_document (dst);
  return failures;
}

int
main (int argc, char *argv[])
{
  Dwg_Data src;
  int error, failures = 0;
  char *trace = getenv ("LIBREDWG_TRACE");
  test_loglevel = is_make_silent () ? 0 : 2; // print ok
  tracelevel = trace ? atoi (trace) : 0;
  loglevel = test_loglevel;
  (void)argc;
  (void)argv;

  if (!find_fixture ())
    {
      fail ("%s not found", FIXTURE);
      return 1;
    }
  error = read_dwg (fixture, &src);
  if (error & ~KNOWN_ERRORS)
    {
      fail ("dwg_read_file 0x%x", error);
      return 1;
    }
  failures += test_is_xref_ref (&src);
  failures += test_add_insert_block ();
  failures += test_placements (&src);
  failures += test_refusals (&src);
  failures += test_attributes (&src);
  failures += test_image_reactors (&src);
  failures += test_sortents_entries (&src);
  failures += test_sortents_order (&src);
  failures += test_without_sortents (&src);
  failures += test_entity_count (&src);
  failures += test_definition_refusals (&src);
  failures += test_minsert (&src);
  free_dwg (&src);
  return failures ? 1 : 0;
}
