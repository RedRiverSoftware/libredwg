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
   repeat, at changed lengths and angles too, and at the definitions' values
   and at other visibility states. The document is written and read back,
   and the evaluator's check of every placement against its representation
   block must match. */

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
#include "bits.h"
#include "tests_common.h"
#include "dwg_api.h"
#include "dwg_dynblock_api.h"

#define OK(...)                                                              \
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
#define WRITTEN_NON_ASCII "dynblock_insert_test_non_ascii.dwg"
#define WRITTEN_ATTRIBUTE_TEXT "dynblock_insert_test_attribute_text.dwg"
#define WRITTEN_BLOCK "dynblock_insert_test_block.dwg"
#define WRITTEN_SORT "dynblock_insert_test_sort.dwg"
#define WRITTEN_MINSERT "dynblock_insert_test_minsert.dwg"
#define WRITTEN_R11 "dynblock_insert_test_r11.dwg"
#define WRITTEN_WIDTHS "dynblock_insert_test_widths.dwg"
#define WRITTEN_COUNTER "dynblock_insert_test_counter.dwg"

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

/* AutoCAD's placements whose values the evaluator supports, all of them but
   the Door's at another Door Size, which drives a scale action, and Door
   Elevation's, which is constraint-based: two at the definition's values,
   which AutoCAD stores as representation blocks too, eight at a visibility
   state only, and eleven at changed lengths or angles, whose records hold
   the parameters' points as the evaluation moves them. The Window at other
   widths and heights (C8D0-C8D3), the Door at another Wall Thickness, which
   moves its flip parameter 'swing' (C8D5, C8D6, C8D8), and the Toilet at
   other Angles, which turn its alignment parameter (C8DB-C8DE). */
static const BITCODE_RLL autocad_placements[]
    = { 0xBFD0, 0xBFD8, 0xC145, 0xC14C, 0xC17F, 0xC8D0, 0xC8D1,
        0xC8D2, 0xC8D3, 0xC8D5, 0xC8D6, 0xC8D7, 0xC8D8, 0xC8DA,
        0xC8DB, 0xC8DC, 0xC8DD, 0xC8DE, 0xC8E0, 0xC8E1, 0xC8E2 };

/* How far an evaluated point of ours may be from AutoCAD's: AutoCAD's turns
   leave residues such as 3e-14 for 0, and signed zeros. */
#define RECORD_TOLERANCE 1e-9

/* The visibility states each definition is placed at besides. Door's
   "Open 45\xC2\xBA" and "Open 90\xC2\xBA" have a character outside ASCII,
   in UTF-8 in the document built with the add API, as in the API, and in
   codepage 1252 in the fixture and the read back. Its own state, placed
   at its values, is "Open 30\xC2\xBA". */
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
  { DOOR,
    "Opening Angle",
    { "Closed", "Open 45\xC2\xBA", "Open 90\xC2\xBA", NULL } },
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
  /* a LINEAR or ROTATION value differs from the definition's: the records
     hold evaluated points, which compare within RECORD_TOLERANCE */
  int evaluated;
  Dwg_DynValue values[8];
  BITCODE_BL num_values;
} Placement;

#define MAX_PLACEMENTS 48

static Placement placements[MAX_PLACEMENTS];
static unsigned num_placements;

/* An XRECORD string of a document that holds its codepage, in UTF-8, as a
   document built with the add API holds it: a new allocation of *lenp
   bytes, or NULL. */
static char *
xrecord_string_utf8 (const Dwg_Resbuf *r, size_t *lenp)
{
  char *s = (char *)malloc ((size_t)r->value.str.size + 1);
  char *u;
  if (!s)
    return NULL;
  memcpy (s, r->value.str.u.data, r->value.str.size);
  s[r->value.str.size] = '\0';
  u = bit_TV_to_utf8 (s, r->value.str.codepage);
  if (u != s)
    free (s);
  if (u)
    *lenp = strlen (u);
  return u;
}

/* Two XRECORDs hold the same items: the same bytes, or with a tolerance
   the reals and points within it, which *exactp then says aren't all the
   same bytes. With b_utf8, b is in a document that holds UTF-8 and a in
   one that holds its codepage: b's strings are a's in UTF-8, and b's
   xdata_size is a's and the bytes that adds. */
static int
same_xrecord (const Dwg_Object_XRECORD *a, const Dwg_Object_XRECORD *b,
              const int b_utf8, const double tolerance, int *exactp,
              char *what, const size_t size)
{
  const Dwg_Resbuf *ra = a->xdata, *rb = b->xdata;
  unsigned i = 0;
  size_t utf8_extra = 0;
  *exactp = 1;
  if (a->num_xdata != b->num_xdata || a->cloning != b->cloning
      || (!b_utf8 && a->xdata_size != b->xdata_size))
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
            if (b_utf8 && ra->value.str.u.data)
              {
                size_t len = 0;
                char *u = xrecord_string_utf8 (ra, &len);
                same = u && rb->value.str.size == len
                       && !memcmp (u, rb->value.str.u.data, len);
                if (u && len > ra->value.str.size)
                  utf8_extra += len - ra->value.str.size;
                free (u);
              }
            else
              same = ra->value.str.size == rb->value.str.size
                     && !memcmp (ra->value.str.u.data, rb->value.str.u.data,
                                 ra->value.str.size);
            break;
          case DWG_VT_POINT3D:
            same = !memcmp (ra->value.pt, rb->value.pt, sizeof (ra->value.pt));
            if (!same && tolerance > 0.0)
              {
                *exactp = 0;
                same = 1;
                for (int k = 0; k < 3; k++)
                  if (!(fabs (ra->value.pt[k] - rb->value.pt[k])
                        <= tolerance))
                    same = 0;
              }
            break;
          case DWG_VT_REAL:
            same = !memcmp (&ra->value.dbl, &rb->value.dbl,
                            sizeof (ra->value.dbl));
            if (!same && tolerance > 0.0)
              {
                *exactp = 0;
                same = fabs (ra->value.dbl - rb->value.dbl) <= tolerance;
              }
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
          if (dwg_resbuf_value_type (rb->type) == DWG_VT_POINT3D)
            snprintf (what, size,
                      "item %u (code %d) is (%.17g, %.17g, %.17g), AutoCAD's "
                      "(%.17g, %.17g, %.17g)",
                      i, rb->type, rb->value.pt[0], rb->value.pt[1],
                      rb->value.pt[2], ra->value.pt[0], ra->value.pt[1],
                      ra->value.pt[2]);
          else
            snprintf (what, size, "item %u (code %d) differs", i, rb->type);
          return 0;
        }
    }
  if (b_utf8 && b->xdata_size != a->xdata_size + utf8_extra)
    {
      snprintf (what, size, "%u bytes in UTF-8: AutoCAD's %u and %u for its "
                "strings in UTF-8",
                (unsigned)b->xdata_size, (unsigned)a->xdata_size,
                (unsigned)utf8_extra);
      return 0;
    }
  return 1;
}

/* The ACAD_ENHANCEDBLOCKDATA of placement ours, in dwg, has the records
   that AutoCAD stored for its placement theirs, in fixture: the same
   names, in the same order, each XRECORD with the same items, their bytes
   the same, or with a tolerance the reals and points within it. A
   document built with the add API holds the strings in UTF-8, and writes
   them in the codepage. */
static int
check_records (const char *what, const Dwg_Data *dwg, const Dwg_Object *ours,
               const Dwg_Data *fixture_dwg, const Dwg_Object *theirs,
               const double tolerance)
{
  const Dwg_Object *a = enhanced_block_data (fixture_dwg, theirs);
  const Dwg_Object *b = enhanced_block_data (dwg, ours);
  const Dwg_Object_DICTIONARY *da, *db;
  const int b_utf8 = (dwg->opts & DWG_OPTS_INJSON)
                     && !(fixture_dwg->opts & DWG_OPTS_INJSON);
  unsigned exact = 0;
  char why[512] = "";
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
      int same_bytes = 0;
      if (strcmp (da->texts[i], db->texts[i]) || !xa || !xb
          || xa->fixedtype != DWG_TYPE_XRECORD
          || xb->fixedtype != DWG_TYPE_XRECORD
          || !same_xrecord (xa->tio.object->tio.XRECORD,
                            xb->tio.object->tio.XRECORD, b_utf8, tolerance,
                            &same_bytes, why, sizeof (why)))
        {
          FAIL ("%s: record '%s' differs from AutoCAD's record '%s': %s",
                what, db->texts[i], da->texts[i], why);
          return 1;
        }
      exact += same_bytes;
    }
  if (exact == da->numitems)
    OK ("%s: its %u records are AutoCAD's, byte for byte%s", what,
        (unsigned)da->numitems, b_utf8 ? ", strings in UTF-8" : "");
  else
    OK ("%s: its %u records are AutoCAD's, %u byte for byte and the others "
        "within %g%s",
        what, (unsigned)da->numitems, exact, tolerance,
        b_utf8 ? ", strings in UTF-8" : "");
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

/* Places in dst, at AutoCAD's values for its placement autocad of src,
   the copy of its definition, at ins_pt, scale and rotation, or, with
   ins_pt NULL, where AutoCAD placed it. The records, which are in the
   block's coordinates, must be AutoCAD's either way. */
static int
repeat_placement (Dwg_Data *dst, Dwg_Data *src, BITCODE_RLL map[][2],
                  const unsigned num_map, const BITCODE_RLL autocad,
                  const BITCODE_3BD *ins_pt, const double scale,
                  const double rotation)
{
  const Dwg_Object *ins = object (src, autocad);
  const Dwg_Entity_INSERT *_ins;
  Dwg_Object_BLOCK_HEADER *def = NULL;
  Dwg_DynValue *values = NULL;
  BITCODE_BL count = 0;
  Dwg_DynParam *params = NULL;
  BITCODE_BL num_params = 0;
  const unsigned n = num_placements;
  int at_defaults = 1, evaluated = 0, failures = 0, error;
  char what[256];
  if (!ins || ins->fixedtype != DWG_TYPE_INSERT)
    {
      FAIL ("AutoCAD's placement " FORMAT_HV " not found", autocad);
      return 1;
    }
  _ins = ins->tio.entity->tio.INSERT;
  error = dwg_dynblock_get_values (_ins, &def, &values, &count);
  error |= dwg_dynblock_get_params (def, &params, &num_params);
  loglevel = test_loglevel;
  for (BITCODE_BL j = 0; !error && j < count; j++)
    if (values[j].state ? strcmp (values[j].state, params[j].default_state)
                        : fabs (values[j].value - params[j].default_value)
                              > 1e-9)
      {
        at_defaults = 0;
        evaluated |= params[j].kind == DWG_DYNPARAM_LINEAR
                     || params[j].kind == DWG_DYNPARAM_ROTATION;
      }
  if (ins_pt)
    snprintf (what, sizeof (what),
              "AutoCAD's placement " FORMAT_HV " at scale %g, rotation %g",
              autocad, scale, rotation);
  else
    snprintf (what, sizeof (what), "AutoCAD's placement " FORMAT_HV,
              autocad);
  if (error)
    {
      FAIL ("%s: its values: 0x%x", what, error);
      failures++;
    }
  else
    failures += place (dst, copy_of (map, num_map, handle_of (src, def)),
                       autocad, ins_pt ? ins_pt : &_ins->ins_pt,
                       ins_pt ? scale : _ins->scale.x,
                       ins_pt ? rotation : _ins->rotation, values, count,
                       at_defaults, what);
  if (num_placements > n)
    placements[n].evaluated = evaluated;
  dwg_dynblock_free_values (values, count);
  dwg_dynblock_free_params (params, num_params);
  return failures;
}

/* The fixture's definitions placed as AutoCAD placed them, as AutoCAD did
   and elsewhere, scaled and rotated; at their own values; and at other
   visibility states, scaled and rotated; and their checks, before and
   after a write and read back. */
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
    failures += repeat_placement (dst, src, map, num_map,
                                  autocad_placements[i], NULL, 1.0, 0.0);
  // and some at changed lengths and angles elsewhere, scaled and rotated
  {
    const BITCODE_3BD at = { -300.0, 250.0, 0.0 };
    failures += repeat_placement (dst, src, map, num_map, 0xC8D3, &at, 2.5,
                                  M_PI / 3);
    failures += repeat_placement (dst, src, map, num_map, 0xC8D8, &at, 0.5,
                                  -M_PI / 2);
    failures += repeat_placement (dst, src, map, num_map, 0xC8DB, &at, 3.0,
                                  2.0);
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
                                   object (src, pl->autocad),
                                   pl->evaluated ? RECORD_TOLERANCE : 0.0);
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
            failures += check_records (
                what, &readback, object (&readback, pl->insert), src,
                object (src, pl->autocad),
                pl->evaluated ? RECORD_TOLERANCE : 0.0);
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

static int test_accepted (Dwg_Data *dwg, const BITCODE_RLL def,
                          const double scale, const Dwg_DynValue *values,
                          const BITCODE_BL n, const char *what);
static Dwg_Object *graph_node (const Dwg_Data *dwg, const BITCODE_RLL def,
                               const Dwg_Object_Type type, BITCODE_BL *indexp);

/* A LINEAR or ROTATION value other than the definition's that its value
   set doesn't allow is refused, as AutoCAD would move it to one it allows
   on its next edit: Window Width 14, not in its list (12, 24, 30, ...),
   while 24 is; and, with the value set changed, a Window Width below its
   minimum, above its maximum or off its increment, and an Angle off its
   increment of 30 degrees, around the circle too. */
static int
test_value_sets (Dwg_Data *dst, BITCODE_RLL map[][2], const unsigned num_map)
{
  const BITCODE_RLL window = copy_of (map, num_map, WINDOW);
  const BITCODE_RLL toilet = copy_of (map, num_map, TOILET);
  Dwg_Object *width = graph_node (dst, window, DWG_TYPE_BLOCKLINEARPARAMETER,
                                  NULL);
  Dwg_Object *angle
      = graph_node (dst, toilet, DWG_TYPE_BLOCKROTATIONPARAMETER, NULL);
  const BITCODE_3BD pt = { 0.0, 0.0, 0.0 };
  Dwg_BLOCKPARAMVALUESET kept, *vs;
  Dwg_DynValue v;
  int failures = 0;
  memset (&v, 0, sizeof (v));
  if (!width || !angle)
    {
      FAIL ("value sets: the Window's or the Toilet's parameter not found");
      return 1;
    }
  // the Window's first linear parameter is Window Width
  vs = &width->tio.object->tio.BLOCKLINEARPARAMETER->value_set;
  v.name = (char *)"Window Width";
  v.value = 14.0;
  failures += test_refusal (dst, model_space (dst),
                            block_header (dst, window), &pt, 1.0, 0.0, &v, 1,
                            NULL, 0, REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "a linear value not in its value set's list");
  v.value = 24.0;
  failures += test_accepted (dst, window, 1.0, &v, 1,
                             "a linear value in its value set's list");
  // found again, as the placement added objects
  width = graph_node (dst, window, DWG_TYPE_BLOCKLINEARPARAMETER, NULL);
  vs = &width->tio.object->tio.BLOCKLINEARPARAMETER->value_set;
  kept = *vs;
  vs->flags = DWG_DYNPARAM_HAS_MINIMUM | DWG_DYNPARAM_HAS_MAXIMUM
              | DWG_DYNPARAM_HAS_INCREMENT;
  vs->minimum = 12.0;
  vs->maximum = 60.0;
  vs->increment = 6.0;
  v.value = 6.0;
  failures += test_refusal (dst, model_space (dst),
                            block_header (dst, window), &pt, 1.0, 0.0, &v, 1,
                            NULL, 0, REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "a linear value below its minimum");
  v.value = 66.0;
  failures += test_refusal (dst, model_space (dst),
                            block_header (dst, window), &pt, 1.0, 0.0, &v, 1,
                            NULL, 0, REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "a linear value above its maximum");
  v.value = 20.0;
  failures += test_refusal (dst, model_space (dst),
                            block_header (dst, window), &pt, 1.0, 0.0, &v, 1,
                            NULL, 0, REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "a linear value off its increment");
  v.value = 18.0;
  failures += test_accepted (dst, window, 1.0, &v, 1,
                             "a linear value on its increment, from its "
                             "minimum");
  // a huge value a quarter off an increment of 1, or off the one value of
  // a list: the tolerance stays far below the spacing
  width = graph_node (dst, window, DWG_TYPE_BLOCKLINEARPARAMETER, NULL);
  vs = &width->tio.object->tio.BLOCKLINEARPARAMETER->value_set;
  vs->flags = DWG_DYNPARAM_HAS_MINIMUM | DWG_DYNPARAM_HAS_INCREMENT;
  vs->minimum = 0.0;
  vs->increment = 1.0;
  v.value = 1e9 + 0.25;
  failures += test_refusal (dst, model_space (dst),
                            block_header (dst, window), &pt, 1.0, 0.0, &v, 1,
                            NULL, 0, REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "a huge linear value off its increment");
  v.value = 1e9;
  failures += test_accepted (dst, window, 1.0, &v, 1,
                             "a huge linear value on its increment");
  {
    double one[1] = { 1e9 };
    width = graph_node (dst, window, DWG_TYPE_BLOCKLINEARPARAMETER, NULL);
    vs = &width->tio.object->tio.BLOCKLINEARPARAMETER->value_set;
    vs->flags = DWG_DYNPARAM_HAS_LIST;
    vs->valuelist = one;
    vs->num_valuelist = 1;
    v.value = 1e9 + 0.25;
    failures += test_refusal (dst, model_space (dst),
                              block_header (dst, window), &pt, 1.0, 0.0, &v,
                              1, NULL, 0, REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                              "a huge linear value a quarter off its list's");
    v.value = 1e9;
    failures += test_accepted (dst, window, 1.0, &v, 1,
                               "a huge linear value in its list");
    width = graph_node (dst, window, DWG_TYPE_BLOCKLINEARPARAMETER, NULL);
    width->tio.object->tio.BLOCKLINEARPARAMETER->value_set = kept;
  }

  // found again, as the placements added objects
  angle = graph_node (dst, toilet, DWG_TYPE_BLOCKROTATIONPARAMETER, NULL);
  vs = &angle->tio.object->tio.BLOCKROTATIONPARAMETER->angle_value_set;
  kept = *vs;
  vs->flags = DWG_DYNPARAM_HAS_MINIMUM | DWG_DYNPARAM_HAS_MAXIMUM
              | DWG_DYNPARAM_HAS_INCREMENT;
  vs->minimum = 0.0;
  vs->maximum = 2.0 * M_PI;
  vs->increment = M_PI / 6;
  v.name = (char *)"Angle";
  v.value = 0.3;
  failures += test_refusal (dst, model_space (dst),
                            block_header (dst, toilet), &pt, 1.0, 0.0, &v, 1,
                            NULL, 0, REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "a rotation value off its increment");
  v.value = M_PI / 3 + 4.0 * M_PI;
  failures += test_accepted (dst, toilet, 1.0, &v, 1,
                             "a rotation value on its increment, two turns "
                             "on");
  v.value = -M_PI / 6;
  failures += test_accepted (dst, toilet, 1.0, &v, 1,
                             "a rotation value on its increment, below 0");
  // with limits pi/2 to 2 pi and the Toilet's Angle made pi, 2 pi is at
  // the maximum, though it is 0 reduced, and pi/4 is below the minimum
  angle = graph_node (dst, toilet, DWG_TYPE_BLOCKROTATIONPARAMETER, NULL);
  {
    Dwg_Object_BLOCKROTATIONPARAMETER *_angle
        = angle->tio.object->tio.BLOCKROTATIONPARAMETER;
    const BITCODE_3BD endpt = _angle->def_endpt;
    _angle->def_endpt.x = 2.0 * _angle->def_basept.x
                          - _angle->def_base_angle_pt.x;
    _angle->def_endpt.y = 2.0 * _angle->def_basept.y
                          - _angle->def_base_angle_pt.y;
    vs = &_angle->angle_value_set;
    vs->flags = DWG_DYNPARAM_HAS_MINIMUM | DWG_DYNPARAM_HAS_MAXIMUM;
    vs->minimum = M_PI / 2;
    vs->maximum = 2.0 * M_PI;
    v.value = 2.0 * M_PI;
    failures += test_accepted (dst, toilet, 1.0, &v, 1,
                               "a rotation value at its maximum of 2 pi");
    v.value = M_PI / 4;
    failures += test_refusal (dst, model_space (dst),
                              block_header (dst, toilet), &pt, 1.0, 0.0, &v,
                              1, NULL, 0, REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                              "a rotation value below its minimum");
    _angle->def_endpt = endpt;
  }
  angle = graph_node (dst, toilet, DWG_TYPE_BLOCKROTATIONPARAMETER, NULL);
  angle->tio.object->tio.BLOCKROTATIONPARAMETER->angle_value_set = kept;
  return failures;
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

  // the evaluation doesn't flip
  v[0].name = (char *)"Flip view";
  v[0].value = 1.0;
  failures += test_refusal (dst, ms, toilet, &pt, 1.0, 0.0, v, 1, NULL, 0,
                            REFUSED (DWG_ERR_NOTYETSUPPORTED), "a flip");
  v[0].value = 0.5;
  failures += test_refusal (dst, ms, toilet, &pt, 1.0, 0.0, v, 1, NULL, 0,
                            REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "a flip value of 0.5");
  // a value the evaluation refuses
  v[0].name = (char *)"Window Width";
  v[0].value = -1.0;
  failures += test_refusal (dst, ms, window, &pt, 1.0, 0.0, v, 1, NULL, 0,
                            REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "a linear value below 0");
  failures += test_value_sets (dst, map, num_map);
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

/* dwg_add_INSERT_block () before r13, whose INSERTs flag the optional
   fields they store and keep Z as the entity's elevation: an INSERT at an
   elevation of 0 and one above it are written, and read back with their
   points, scales and rotations as given, drawing the block given. */
static int
test_add_insert_block_r11 (void)
{
  Dwg_Data *dwg = dwg_new_Document (R_11, 0, tracelevel);
  const dwg_point_3d start = { 0.0, 0.0, 0.0 }, end = { 5.0, 5.0, 0.0 };
  const dwg_point_3d pts[2] = { { 1.0, 2.0, 0.0 }, { 4.0, 5.0, 3.0 } };
  Dwg_Object_BLOCK_HEADER *blk = NULL;
  Dwg_Data readback;
  unsigned found = 0;
  int failures = 0, error, added = 0;

  if (dwg)
    blk = dwg_add_BLOCK_HEADER (dwg, "B");
  if (blk && dwg_add_BLOCK (blk, "B") && dwg_add_LINE (blk, &start, &end)
      && dwg_add_ENDBLK (blk))
    for (int k = 0; k < 2; k++)
      added += !!dwg_add_INSERT_block (model_space (dwg), &pts[k], blk, 2.0,
                                       3.0, 4.0, 0.5);
  loglevel = test_loglevel;
  if (added != 2)
    {
      FAIL ("add_INSERT_block r11: adding the block or INSERTs failed");
      if (dwg)
        {
          free_dwg (dwg);
          free (dwg);
        }
      return 1;
    }
  error = write_dwg (WRITTEN_R11, dwg);
  if (error < DWG_ERR_CRITICAL)
    error = read_dwg (WRITTEN_R11, &readback);
  if (error >= DWG_ERR_CRITICAL)
    {
      FAIL ("add_INSERT_block r11: write or read back 0x%x", error);
      failures++;
    }
  else
    {
      for (BITCODE_BL i = 0; i < readback.num_objects; i++)
        {
          const Dwg_Object *obj = &readback.object[i];
          const Dwg_Entity_INSERT *_ins;
          const dwg_point_3d *pt;
          double z;
          char *drawn;
          if (obj->fixedtype != DWG_TYPE_INSERT || found++ >= 2)
            continue;
          pt = &pts[found - 1];
          _ins = obj->tio.entity->tio.INSERT;
          // the decoder leaves an r11 INSERT's Z in the entity's elevation
          z = obj->tio.entity->flag_r11 & FLAG_R11_HAS_ELEVATION
                  ? obj->tio.entity->elevation_r11
                  : _ins->ins_pt.z;
          // before r13 the INSERT names its block by its place in the table
          drawn = dwg_handle_name (&readback, "BLOCK", _ins->block_header);
          loglevel = test_loglevel;
          if (_ins->ins_pt.x == pt->x && _ins->ins_pt.y == pt->y
              && z == pt->z && _ins->scale.x == 2.0 && _ins->scale.y == 3.0
              && _ins->scale.z == 4.0 && _ins->rotation == 0.5 && drawn
              && !strcmp (drawn, "B"))
            OK ("add_INSERT_block r11: the INSERT at elevation %g reads back "
                "with its point, scales and rotation, drawing the block "
                "given",
                pt->z);
          else
            {
              FAIL ("add_INSERT_block r11: reads back at (%g, %g, %g), "
                    "scales (%g, %g, %g), rotation %g, drawing %s, placed at "
                    "(%g, %g, %g)",
                    _ins->ins_pt.x, _ins->ins_pt.y, z, _ins->scale.x,
                    _ins->scale.y, _ins->scale.z, _ins->rotation,
                    drawn ? drawn : "no block", pt->x, pt->y, pt->z);
              failures++;
            }
          free (drawn);
        }
      if (found != 2)
        {
          FAIL ("add_INSERT_block r11: %u INSERTs read back", found);
          failures++;
        }
      free_dwg (&readback);
    }
  if (!failures)
    unlink (WRITTEN_R11);
  free_dwg (dwg);
  free (dwg);
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

/* Appends n EED items to entity ent, each with zeroed data, the first
   starting a group of the APPID with handle appid that takes size bytes in
   a file. Returns the first, or NULL. */
static Dwg_Eed *
append_eed (Dwg_Object_Entity *ent, const BITCODE_RLL appid,
            const BITCODE_BS size, const BITCODE_BL n)
{
  const BITCODE_BL first = ent->num_eed;
  Dwg_Eed *eed
      = (Dwg_Eed *)realloc (ent->eed, (first + n) * sizeof (Dwg_Eed));
  if (!eed)
    return NULL;
  ent->eed = eed;
  memset (&eed[first], 0, n * sizeof (Dwg_Eed));
  for (BITCODE_BL i = 0; i < n; i++)
    {
      eed[first + i].data = (Dwg_Eed_Data *)calloc (1, sizeof (Dwg_Eed_Data));
      if (!eed[first + i].data)
        return NULL;
      ent->num_eed++;
    }
  eed[first].size = size;
  eed[first].handle.code = 5;
  for (BITCODE_RLL v = appid; v; v >>= 8)
    eed[first].handle.size++;
  eed[first].handle.value = appid;
  return &eed[first];
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
  Dwg_Eed *eed
      = appid ? append_eed (ent, appid->absolute_ref, 3 + 5 + 9, 3) : NULL;
  if (!eed)
    return 1;
  eed[0].data->code = 70;
  eed[0].data->u.eed_70.rs = 1;
  eed[1].data->code = 71;
  // Dwg_Eed_Data is packed
  memcpy (&eed[1].data->u.eed_71.rl, &index, sizeof (index));
  eed[2].data->code = 5;
  memcpy (&eed[2].data->u.eed_5.entity, &zero, sizeof (zero));
  return 0;
}

/* Gives entity ent an AcDbAttr EED group, adding its APPID to dwg when
   dwg has none: 1070 0, then an item with the given code, 70 (1070) or 71
   (1071), and value. AutoCAD 2007 and later write 1070 0, 1070 1 there for
   an attribute whose position is locked, in files before r2007. */
static int
add_attr_eed (Dwg_Data *dwg, Dwg_Object_Entity *ent, const BITCODE_RC code,
              const BITCODE_RS value)
{
  BITCODE_H found = dwg_find_tablehandle (dwg, "AcDbAttr", "APPID");
  BITCODE_RLL appid = found ? found->absolute_ref : 0;
  const BITCODE_RS zero = 0;
  const BITCODE_RL value71 = value;
  Dwg_Eed *eed;
  if (!appid)
    {
      Dwg_Object_APPID *_appid = dwg_add_APPID (dwg, "AcDbAttr");
      if (!_appid)
        return 1;
      appid = handle_of (dwg, _appid);
    }
  // a 1070 takes 3 bytes in a file, a 1071 5
  eed = append_eed (ent, appid, code == 71 ? 8 : 6, 2);
  if (!eed)
    return 1;
  eed[0].data->code = 70;
  // Dwg_Eed_Data is packed
  memcpy (&eed[0].data->u.eed_70.rs, &zero, sizeof (zero));
  eed[1].data->code = code;
  if (code == 71)
    memcpy (&eed[1].data->u.eed_71.rl, &value71, sizeof (value71));
  else
    memcpy (&eed[1].data->u.eed_70.rs, &value, sizeof (value));
  return 0;
}

/* The values of obj's AcDbAttr EED group, of 1070 items, into values, up
   to two of them. Returns how many items the group has, -1 when obj has
   no such group, or -2 when it has two, or an item that isn't a 1070. */
static int
attr_eed (const Dwg_Data *dwg, const Dwg_Object *obj, BITCODE_RS values[2])
{
  const Dwg_Eed *eed = obj->tio.entity->eed;
  int n = -1, in_group = 0;
  for (BITCODE_BL i = 0; eed && i < obj->tio.entity->num_eed; i++)
    {
      if (eed[i].size)
        {
          const Dwg_Object *appid = object (dwg, eed[i].handle.value);
          in_group = appid && appid->fixedtype == DWG_TYPE_APPID
                     && !strcmp (appid->tio.object->tio.APPID->name,
                                 "AcDbAttr");
          if (in_group && n >= 0)
            return -2;
          if (in_group)
            n = 0;
        }
      if (!in_group)
        continue;
      if (!eed[i].data || eed[i].data->code != 70)
        return -2;
      if (n < 2)
        // Dwg_Eed_Data is packed
        memcpy (&values[n], &eed[i].data->u.eed_70.rs, sizeof (values[n]));
      n++;
    }
  return n;
}

/* An expected ATTRIB: its tag and value, and whether it has an AcDbAttr
   EED group, 1070 0 and 1070 lock. */
typedef struct
{
  const char *tag;
  const char *value;
  int has_attr_eed;
  BITCODE_RS lock;
} Expected_Attrib;

/* The ATTRIBs of placement insert, placed at ins_pt, scale and rotation in
   block (base point 0): one per attribute definition that isn't constant,
   in order, each with its definition's properties placed by the INSERT.
   Text left-aligned on the baseline doesn't use its alignment point: its
   ATTRIB keeps it (0, 0) and doesn't store it (dataflags 0x02), as
   AutoCAD's don't. Other ATTRIBs are aligned at their definition's
   alignment point, placed; dataflags 0x02 stands for an alignment point of
   (0, 0), as AutoCAD reads it. Each has the AcDbAttr EED group expected,
   which its definition has too, or neither has one. */
static int
check_attribs (const char *what, const Dwg_Data *dwg, const BITCODE_RLL insert,
               const BITCODE_RLL block, const BITCODE_3BD *ins_pt,
               const double scale, const double rotation,
               const Expected_Attrib *expected, const unsigned num_expected,
               const int etags)
{
  const Dwg_Entity_INSERT *_ins = object (dwg, insert)->tio.entity->tio.INSERT;
  const Dwg_Object_BLOCK_HEADER *blk = block_header (dwg, block);
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
      const Dwg_Object *defobj = object (dwg, ref (blk->entities[i]));
      const Dwg_Entity_ATTDEF *d;
      const Dwg_Object *attobj;
      const Dwg_Entity_ATTRIB *a;
      double x, y, ax, ay, aax, aay;
      int left;
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
      left = !d->horiz_alignment && !d->vert_alignment;
      aax = (a->dataflags & 0x02) ? 0.0 : a->alignment_pt.x;
      aay = (a->dataflags & 0x02) ? 0.0 : a->alignment_pt.y;
      x = ins_pt->x
          + scale
                * ((d->ins_pt.x - base->x) * c - (d->ins_pt.y - base->y) * s);
      y = ins_pt->y
          + scale
                * ((d->ins_pt.x - base->x) * s + (d->ins_pt.y - base->y) * c);
      ax = ins_pt->x
           + scale
                 * ((d->alignment_pt.x - base->x) * c
                    - (d->alignment_pt.y - base->y) * s);
      ay = ins_pt->y
           + scale
                 * ((d->alignment_pt.x - base->x) * s
                    + (d->alignment_pt.y - base->y) * c);
      has_etag = eed_tag (dwg, attobj, "AcDbBlockRepETag", &etag_handle,
                          &etag_index);
      // at the definition's values, an ATTRIB of a definition with one
      def_has_etag = eed_tag (dwg, defobj, "AcDbBlockRepETag", &def_etag,
                              &def_index);
      if (strcmp (a->tag, d->tag) || strcmp (a->tag, expected[k].tag)
          || strcmp (a->text_value, expected[k].value)
          || fabs (a->ins_pt.x - x) > 1e-9 || fabs (a->ins_pt.y - y) > 1e-9
          || fabs (a->elevation - ins_pt->z - scale * d->elevation) > 1e-9
          || (left ? !(a->dataflags & 0x02)
                   : fabs (aax - ax) > 1e-9 || fabs (aay - ay) > 1e-9)
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
                ", ETag %d, expected '%s' at (%g, %g) %s (%g, %g), h %g r "
                "%g, ETag %d",
                what, a->tag, a->text_value, a->ins_pt.x, a->ins_pt.y, aax,
                aay, (unsigned)a->dataflags, a->height, a->rotation,
                ref (attobj->tio.entity->layer), has_etag, expected[k].value,
                x, y,
                left ? "with no alignment point stored, not" : "aligned at",
                ax, ay, d->height * scale,
                fmod (d->rotation + rotation, 2.0 * M_PI), etags);
          failures++;
        }
      // the definition's AcDbAttr group, copied
      {
        BITCODE_RS av[2] = { 9, 9 }, dv[2] = { 9, 9 };
        const int an = attr_eed (dwg, attobj, av);
        const int dn = attr_eed (dwg, defobj, dv);
        const int want = expected[k].has_attr_eed ? 2 : -1;
        if (an != want || dn != want
            || (want == 2
                && (av[0] != 0 || av[1] != expected[k].lock || dv[0] != 0
                    || dv[1] != expected[k].lock)))
          {
            FAIL ("%s: ATTRIB %s has %d AcDbAttr items (%u, %u), its "
                  "definition %d (%u, %u), expected %d (0, %u)",
                  what, a->tag, an, (unsigned)av[0], (unsigned)av[1], dn,
                  (unsigned)dv[0], (unsigned)dv[1], want,
                  (unsigned)expected[k].lock);
            failures++;
          }
      }
      k++;
    }
  if (!failures)
    OK ("%s: %u ATTRIBs, as their definitions placed by the INSERT, each "
        "with the AcDbAttr EED expected",
        what, k);
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

/* An attribute definition for test_attribute_widths and the value placed
   for it. The offsets are where text starts, its insertion point, from its
   alignment point, along and across its baseline, in text heights: where
   AutoCAD 2027 starts TEXT in Arial at a height of 2.5, unless said
   otherwise. A width is as a caller measures it: the sum of the value's
   Arial advance widths over the height of Arial's A, 1466 units. AutoCAD
   centres text on its first character's outline where that starts left of
   the text's start, as Arial's A does by 3 units (overhang), which a width
   doesn't hold: such a centred value starts half the overhang further
   back than AutoCAD's, or further forward for backward text. */
typedef struct
{
  const char *tag;
  BITCODE_BS horiz, vert, generation;
  double rotation, oblique, width_factor;
  const char *dflt;               // the definition's default value
  double dflt_along, dflt_across; // where the definition's text starts
  const char *value;              // NULL for none given
  double width;
  double along, across; // where the ATTRIB starts, but for the overhang
  double overhang;      // of the value's first character, in font units
  int vertical;         // in a vertical text style
} Width_Case;

static const Width_Case width_cases[] = {
  // centred on the middle, a longer value
  { "CENTRED", 1, 2, 0, 0.0, 0.0, 1.0, "AB", -0.9307639836, -0.5,
    "ABCDEFGHIJKLMNOPQRST-0001", 32548.0 / 1466.0, -11.0999317872, -0.5, 3.0,
    0 },
  // centred on the baseline, a shorter value without an overhang
  { "SHORT", 1, 0, 0, 0.0, 0.0, 1.0, "AB", -0.9307639836, 0.0, "iii",
    1365.0 / 1466.0, -0.4655525239, 0.0, 0.0, 0 },
  // right on the top, turned, at a width factor of 0.8, a shorter value
  { "RIGHT", 2, 3, 0, 0.3, 0.0, 0.8, "ABCDEFGHIJ", -7.0788540246, -1.0, "AB",
    2732.0 / 1466.0, -1.4908594816, -1.0, 0.0, 0 },
  // the same, oblique by 15 degrees, as AutoCAD lays it out turned 0.5
  { "OBLIQUE", 2, 3, 0, 0.5, 0.2617993878, 0.8, "ABCDEFGHIJ", -7.0788540246,
    -1.0, "AB", 2732.0 / 1466.0, -1.4908594816, -1.0, 0.0, 0 },
  // middle, a longer value of capitals
  { "MIDDLE", 4, 0, 0, 0.0, 0.0, 1.0, "AB", -0.9307639836, -0.5,
    "ABCDEFGHIJ", 12972.0 / 1466.0, -4.4232605730, -0.5, 3.0, 0 },
  // middle, a value with descenders: AutoCAD's across is -0.3530013642,
  // the middle of its characters; the definition's is kept
  { "LOW", 4, 0, 0, 0.0, 0.0, 1.0, "AB", -0.9307639836, -0.5, "ABgy",
    4895.0 / 1466.0, -1.6684856753, -0.5, 3.0, 0 },
  // centred and backward
  { "BACKWARD", 1, 0, 2, 0.0, 0.0, 1.0, "AB", 0.9307639836, 0.0, "ABgy",
    4895.0 / 1466.0, 1.6684856753, 0.0, 3.0, 0 },
  // right-aligned and backward: forward of its alignment point by the
  // whole width
  { "RIGHTBACK", 2, 0, 2, 0.0, 0.0, 1.0, "AB", 1.8635743520, 0.0, "ABgy",
    4895.0 / 1466.0, 3.3390177353, 0.0, 0.0, 0 },
  // no width: the definition's start, though the value is longer
  { "NOWIDTH", 1, 2, 0, 0.0, 0.0, 1.0, "AB", -0.9307639836, -0.5,
    "ABCDEFGHIJ", 0.0, -0.9307639836, -0.5, 0.0, 0 },
  // no value: the definition's default and start
  { "DEFAULT", 2, 0, 0, 0.0, 0.0, 1.0, "AB", -1.8635743516, 0.0, NULL, 0.0,
    -1.8635743516, 0.0, 0.0, 0 },
  // left on the middle, aligned (spanning the definition's advance width)
  // and in a vertical style, they start where the definitions do
  { "LEFTMID", 0, 2, 0, 0.0, 0.0, 1.0, "AB", 0.0, -0.5, "ABCDEFGHIJ",
    12972.0 / 1466.0, 0.0, -0.5, 0.0, 0 },
  { "ALIGNED", 3, 0, 0, 0.0, 0.0, 1.0, "AB", -1.8635743516, 0.0,
    "ABCDEFGHIJ", 12972.0 / 1466.0, -1.8635743516, 0.0, 0.0, 0 },
  { "VERTICAL", 1, 2, 0, 0.0, 0.0, 1.0, "AB", -0.9307639836, -0.5,
    "ABCDEFGHIJ", 12972.0 / 1466.0, -0.9307639836, -0.5, 0.0, 1 },
};
#define NUM_WIDTH_CASES (sizeof (width_cases) / sizeof (width_cases[0]))

/* The ATTRIBs of placement insert, at ins_pt, scale and rotation in block,
   holding width_cases' definitions at height 2.5: each aligned at its
   definition's alignment point placed, with its value, and starting where
   the case says. */
static int
check_widths (const char *what, const Dwg_Data *dwg, const BITCODE_RLL insert,
              const BITCODE_RLL block, const BITCODE_3BD *ins_pt,
              const double scale, const double rotation)
{
  const Dwg_Object *insobj = object (dwg, insert);
  const Dwg_Object_BLOCK_HEADER *blk = block_header (dwg, block);
  const Dwg_Entity_INSERT *_ins;
  const double c = cos (rotation), s = sin (rotation);
  const double height = 2.5 * scale;
  int failures = 0;
  if (!insobj || insobj->fixedtype != DWG_TYPE_INSERT || !blk)
    {
      FAIL ("%s: no INSERT " FORMAT_HV " or block " FORMAT_HV, what, insert,
            block);
      return 1;
    }
  _ins = insobj->tio.entity->tio.INSERT;
  if (_ins->num_owned != NUM_WIDTH_CASES)
    {
      FAIL ("%s: %u ATTRIBs, expected %u", what, (unsigned)_ins->num_owned,
            (unsigned)NUM_WIDTH_CASES);
      return 1;
    }
  for (unsigned k = 0; k < NUM_WIDTH_CASES; k++)
    {
      const Width_Case *wc = &width_cases[k];
      const Dwg_Entity_ATTDEF *d = NULL;
      const Dwg_Entity_ATTRIB *a = NULL;
      const double r = wc->rotation + rotation;
      // a width alone starts text half the overhang back, or forward when
      // backward
      const double along
          = wc->along
            + (wc->generation & 2 ? 0.5 : -0.5) * wc->overhang / 1466.0
                  * wc->width_factor;
      double ax, ay, x, y;
      for (BITCODE_BL i = 0; !d && i < blk->num_owned; i++)
        {
          const Dwg_Object *ent = object (dwg, ref (blk->entities[i]));
          if (ent && ent->fixedtype == DWG_TYPE_ATTDEF
              && !strcmp (ent->tio.entity->tio.ATTDEF->tag, wc->tag))
            d = ent->tio.entity->tio.ATTDEF;
        }
      for (BITCODE_BL i = 0; !a && i < _ins->num_owned; i++)
        {
          const Dwg_Object *att = object (dwg, ref (_ins->attribs[i]));
          if (att && att->fixedtype == DWG_TYPE_ATTRIB
              && !strcmp (att->tio.entity->tio.ATTRIB->tag, wc->tag))
            a = att->tio.entity->tio.ATTRIB;
        }
      if (!d || !a)
        {
          FAIL ("%s: %s has no %s", what, wc->tag,
                d ? "ATTRIB" : "attribute definition");
          failures++;
          continue;
        }
      ax = ins_pt->x
           + scale
                 * ((d->alignment_pt.x - blk->base_pt.x) * c
                    - (d->alignment_pt.y - blk->base_pt.y) * s);
      ay = ins_pt->y
           + scale
                 * ((d->alignment_pt.x - blk->base_pt.x) * s
                    + (d->alignment_pt.y - blk->base_pt.y) * c);
      x = ax + height * (along * cos (r) - wc->across * sin (r));
      y = ay + height * (along * sin (r) + wc->across * cos (r));
      // AutoCAD's offsets have 10 decimals
      if (strcmp (a->text_value, wc->value ? wc->value : wc->dflt)
          || fabs (a->alignment_pt.x - ax) > 1e-9
          || fabs (a->alignment_pt.y - ay) > 1e-9
          || fabs (a->ins_pt.x - x) > 1e-8 || fabs (a->ins_pt.y - y) > 1e-8
          || fabs (a->height - height) > 1e-9
          || a->generation != wc->generation
          || a->horiz_alignment != wc->horiz || a->vert_alignment != wc->vert)
        {
          FAIL ("%s: %s '%s' starts at (%.9f, %.9f), aligned at (%g, %g), "
                "h %g, expected '%s' at (%.9f, %.9f), aligned at (%g, %g), "
                "h %g",
                what, wc->tag, a->text_value, a->ins_pt.x, a->ins_pt.y,
                a->alignment_pt.x, a->alignment_pt.y, a->height,
                wc->value ? wc->value : wc->dflt, x, y, ax, ay, height);
          failures++;
        }
    }
  if (!failures)
    OK ("%s: %u ATTRIBs start where AutoCAD starts them for their widths, "
        "or where their definitions do",
        what, (unsigned)NUM_WIDTH_CASES);
  return failures;
}

/* Attribute values given with their widths start where AutoCAD starts
   them: centred, right-aligned, middle, and backward text centred or
   right-aligned, long and short
   values, turned definitions, oblique and with a width factor; without a
   width, without a value, left on the middle, aligned or in a vertical
   style, where the definition's text starts; on a copy of Trees, placed at
   its values, unscaled, and at Shrub (Plan), scaled and turned, and read
   back. */
static int
test_attribute_widths (Dwg_Data *src)
{
  BITCODE_RLL def_h = 0;
  Dwg_Data *dst = document_with (src, TREES, &def_h);
  Dwg_Object_BLOCK_HEADER *def;
  Dwg_Object_STYLE *vertical;
  Dwg_AttValue given[NUM_WIDTH_CASES];
  BITCODE_BL num_given = 0;
  const BITCODE_3BD at1 = { 10.0, 20.0, 0.0 }, at2 = { -30.0, 15.0, 0.0 };
  Dwg_DynValue v;
  Dwg_Entity_INSERT *ins1, *ins2;
  BITCODE_RLL h1, h2, block2;
  Dwg_Data readback;
  int failures = 0, error = 0;

  if (!dst)
    {
      FAIL ("attribute widths: copying Trees failed");
      return 1;
    }
  def = block_header (dst, def_h);
  vertical = def ? dwg_add_STYLE (dst, "VERTICAL") : NULL;
  if (!vertical)
    {
      FAIL ("attribute widths: %s failed",
            def ? "adding a text style" : "finding the copy of Trees");
      free_dwg (dst);
      free (dst);
      return 1;
    }
  vertical->is_vertical = 1;
  memset (given, 0, sizeof (given));
  for (unsigned k = 0; k < NUM_WIDTH_CASES; k++)
    {
      const Width_Case *wc = &width_cases[k];
      // each aligned at a point of its own, in a column
      const double ax = def->base_pt.x + 5.0, ay = def->base_pt.y - 10.0 * k;
      const double c = cos (wc->rotation), s = sin (wc->rotation);
      const dwg_point_3d ins
          = { ax + 2.5 * (wc->dflt_along * c - wc->dflt_across * s),
              ay + 2.5 * (wc->dflt_along * s + wc->dflt_across * c), 0.0 };
      Dwg_Entity_ATTDEF *attdef = dwg_add_ATTDEF (def, 2.5, 0, "Prompt", &ins,
                                                  wc->tag, wc->dflt);
      if (!attdef)
        {
          FAIL ("attribute widths: adding %s failed", wc->tag);
          free_dwg (dst);
          free (dst);
          return 1;
        }
      attdef->horiz_alignment = wc->horiz;
      attdef->vert_alignment = wc->vert;
      attdef->generation = wc->generation;
      attdef->rotation = wc->rotation;
      attdef->oblique_angle = wc->oblique;
      attdef->width_factor = wc->width_factor;
      attdef->alignment_pt.x = ax;
      attdef->alignment_pt.y = ay;
      if (wc->vertical)
        attdef->style
            = dwg_add_handleref (dst, 5, handle_of (dst, vertical), NULL);
      if (wc->value)
        {
          given[num_given].tag = (char *)wc->tag;
          given[num_given].value = (char *)wc->value;
          given[num_given].width = wc->width;
          num_given++;
        }
    }

  loglevel = tracelevel;
  ins1 = dwg_dynblock_insert (model_space (dst), def, &at1, 1.0, 0.0, NULL, 0,
                              given, num_given, &error);
  memset (&v, 0, sizeof (v));
  v.name = (char *)"Type (view)";
  v.state = (char *)"Shrub (Plan)";
  if (ins1 && !error)
    ins2 = dwg_dynblock_insert (model_space (dst), def, &at2, 2.0, 0.5, &v, 1,
                                given, num_given, &error);
  else
    ins2 = NULL;
  loglevel = test_loglevel;
  if (!ins1 || !ins2 || error)
    {
      FAIL ("attribute widths: placing failed: 0x%x", error);
      free_dwg (dst);
      free (dst);
      return 1;
    }
  h1 = handle_of (dst, ins1);
  h2 = handle_of (dst, ins2);
  if (!object (dst, h2))
    {
      FAIL ("attribute widths: the INSERT at Shrub (Plan) not found");
      free_dwg (dst);
      free (dst);
      return 1;
    }
  block2 = ref (object (dst, h2)->tio.entity->tio.INSERT->block_header);
  failures += check_widths ("attribute widths at the definition's values",
                            dst, h1, def_h, &at1, 1.0, 0.0);
  failures += check_widths ("attribute widths at Shrub (Plan)", dst, h2,
                            block2, &at2, 2.0, 0.5);

  error = write_dwg (WRITTEN_WIDTHS, dst);
  if (error < DWG_ERR_CRITICAL)
    error = read_dwg (WRITTEN_WIDTHS, &readback);
  if (error >= DWG_ERR_CRITICAL)
    {
      FAIL ("attribute widths: write or read back 0x%x", error);
      failures++;
    }
  else
    {
      failures += check_widths ("read back attribute widths at the "
                                "definition's values",
                                &readback, h1, def_h, &at1, 1.0, 0.0);
      failures += check_widths ("read back attribute widths at Shrub (Plan)",
                                &readback, h2, block2, &at2, 2.0, 0.5);
      free_dwg (&readback);
    }
  if (!failures)
    unlink (WRITTEN_WIDTHS);
  free_dwg (dst);
  free (dst);
  return failures;
}

/* Attributes, on a copy of Trees given attribute definitions: a plain one,
   left-aligned with no alignment point stored (dataflags 0x02), as AutoCAD
   stores one; a constant one, which gets no ATTRIB; one centred, rotated,
   on a layer of its own and shown in the first visibility state only; one
   left-aligned as dwg_add_ATTDEF () makes it, with an alignment point of
   0 and dataflags 0; and one centred at (0, 0), which it doesn't store
   (dataflags 0x02). The plain one has AcDbAttr EED of 1070 0 and 1070 1
   (locked) before its AcDbBlockRepETag, and the rotated one 1070 0 and
   1070 0 after it, which their ATTRIBs copy as they are; the others have
   none, nor do their ATTRIBs. Placed at the definition's values and at
   another state, scaled and rotated, with one value given. */
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
  const Dwg_AttValue given[] = { { (char *)"NUMBER", (char *)"EL-7", 0.0 } };
  const Expected_Attrib expected1[] = { { "NUMBER", "EL-7", 1, 1 },
                                        { "RATING", "10W", 1, 0 },
                                        { "LABEL", "L1", 0, 0 },
                                        { "MARK", "M", 0, 0 } };
  const Expected_Attrib expected2[] = { { "NUMBER", "#", 1, 1 },
                                        { "RATING", "10W", 1, 0 },
                                        { "LABEL", "L1", 0, 0 },
                                        { "MARK", "M", 0, 0 } };
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
      // AcDbAttr before and after another group, which its copy mustn't
      // take in
      || add_attr_eed (dst, number->parent, 70, 1)
      || add_etag (dst, number->parent, 1001)
      || add_etag (dst, centred->parent, 1002)
      || add_attr_eed (dst, centred->parent, 70, 0))
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
  // no alignment point stored: (0, 0), as the decoder leaves it
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
  failures += check_attribs ("attributes at the definition's values", dst, h1,
                             def_h, &at1, 2.0, 0.5, expected1, 4, 1);
  failures += check_attribs ("attributes at Shrub (Plan)", dst, h2, block2,
                             &at2, 1.5, 2.0 * M_PI - 0.25, expected2, 4, 0);
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

  error = write_dwg (WRITTEN_ATTRIBS, dst);
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
                                 &readback, h1, def_h, &at1, 2.0, 0.5,
                                 expected1, 4, 1);
      failures += check_attribs ("read back attributes at Shrub (Plan)",
                                 &readback, h2, block2, &at2, 1.5,
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

/* Attribute definitions whose default values have characters outside
   ASCII, in a document built with the add API, which holds them in UTF-8,
   placed at their defaults: one added to Trees in a fixture of its own,
   which holds it in codepage 1252, and copied with Trees, the other added
   to the copy with dwg_add_ATTDEF (). The ATTRIBs hold the defaults as
   given in UTF-8, and the r2004 file written holds them in codepage
   1252. */
static int
test_attribute_text (void)
{
  static const char dflt[] = "30\xC2\xB0 caf\xC3\xA9";
  static const char dflt_1252[] = "30\xB0 caf\xE9";
  static const char state[] = "Open 30\xC2\xBA";
  static const char state_1252[] = "Open 30\xBA";
  // the add API gives neither definition an AcDbAttr group
  const Expected_Attrib utf8[]
      = { { "SIZE", dflt, 0, 0 }, { "STATE", state, 0, 0 } };
  const Expected_Attrib cp1252[]
      = { { "SIZE", dflt_1252, 0, 0 }, { "STATE", state_1252, 0, 0 } };
  const dwg_point_3d p1 = { 1.0, 2.0, 0.0 }, p2 = { -3.0, 4.0, 0.0 };
  const BITCODE_3BD at = { 0.0, 0.0, 0.0 };
  Dwg_Data src, readback;
  Dwg_Data *dst = NULL;
  BITCODE_RLL def_h = 0, h;
  const Dwg_Object *defobj = NULL;
  Dwg_Object_BLOCK_HEADER *def;
  Dwg_Entity_ATTDEF *attdef, *added;
  Dwg_Entity_INSERT *ins;
  int error, failures = 0;

  error = read_dwg (fixture, &src);
  if (error & ~KNOWN_ERRORS)
    {
      FAIL ("attribute text: dwg_read_file 0x%x", error);
      free_dwg (&src);
      return 1;
    }
  attdef = dwg_add_ATTDEF (block_header (&src, TREES), 2.5, 0, "Size", &p1,
                           "SIZE", dflt);
  loglevel = test_loglevel;
  if (!attdef || !attdef->default_value
      || strcmp (attdef->default_value, dflt_1252))
    {
      FAIL ("attribute text: the definition not added in codepage 1252");
      free_dwg (&src);
      return 1;
    }
  // the add API leaves the width factor 0
  attdef->width_factor = 1.0;
  dst = document_with (&src, TREES, &def_h);
  if (!dst)
    {
      FAIL ("attribute text: copying Trees failed");
      free_dwg (&src);
      return 1;
    }
  def = block_header (dst, def_h);
  // the copy of the definition, the block's last entity
  if (def->num_owned && def->entities)
    defobj = object (dst, ref (def->entities[def->num_owned - 1]));
  if (defobj && defobj->fixedtype == DWG_TYPE_ATTDEF
      && !strcmp (defobj->tio.entity->tio.ATTDEF->default_value, dflt))
    OK ("attribute text: the definition copied in UTF-8");
  else
    {
      FAIL ("attribute text: the definition not copied in UTF-8");
      failures++;
    }
  added = dwg_add_ATTDEF (def, 2.5, 0, "State", &p2, "STATE", state);
  loglevel = test_loglevel;
  if (added && added->default_value && !strcmp (added->default_value, state))
    {
      OK ("attribute text: a definition added to the copy in UTF-8");
      added->width_factor = 1.0;
    }
  else
    {
      FAIL ("attribute text: the definition not added to the copy in "
            "UTF-8");
      free_dwg (dst);
      free (dst);
      free_dwg (&src);
      return failures + 1;
    }

  error = 0;
  loglevel = tracelevel;
  ins = dwg_dynblock_insert (model_space (dst), def, &at, 1.0, 0.0, NULL, 0,
                             NULL, 0, &error);
  loglevel = test_loglevel;
  if (!ins || error)
    {
      FAIL ("attribute text: placing failed: 0x%x", error);
      free_dwg (dst);
      free (dst);
      free_dwg (&src);
      return failures + 1;
    }
  h = handle_of (dst, ins);
  failures += check_attribs ("attribute text, in UTF-8", dst, h, def_h, &at,
                             1.0, 0.0, utf8, 2, 1);

  error = write_dwg (WRITTEN_ATTRIBUTE_TEXT, dst);
  if (error < DWG_ERR_CRITICAL)
    error = read_dwg (WRITTEN_ATTRIBUTE_TEXT, &readback);
  if (error >= DWG_ERR_CRITICAL)
    {
      FAIL ("attribute text: write or read back 0x%x", error);
      failures++;
    }
  else
    {
      if (readback.header.codepage != CP_ANSI_1252)
        {
          FAIL ("attribute text: read back with codepage %u",
                (unsigned)readback.header.codepage);
          failures++;
        }
      failures += check_attribs ("read back attribute text, in codepage "
                                 "1252",
                                 &readback, h, def_h, &at, 1.0, 0.0, cp1252,
                                 2, 1);
      free_dwg (&readback);
    }
  if (!failures)
    unlink (WRITTEN_ATTRIBUTE_TEXT);
  free_dwg (dst);
  free (dst);
  free_dwg (&src);
  return failures;
}

/* Attribute values given in UTF-8 with characters outside ASCII, one ending
   in a character whose codepage 1252 byte, 0xE9, starts a 3-byte UTF-8
   sequence. The document built with the add API keeps them in UTF-8, and
   the r2004 file written holds them in codepage 1252. */
static int
test_attributes_non_ascii (Dwg_Data *src)
{
  BITCODE_RLL def_h = 0;
  Dwg_Data *dst = document_with (src, TREES, &def_h);
  Dwg_Object_BLOCK_HEADER *def;
  const dwg_point_3d p1 = { 1.0, 2.0, 0.0 }, p2 = { -3.0, 4.0, 0.0 };
  Dwg_Entity_ATTDEF *cable, *note;
  const BITCODE_3BD at = { 10.0, 20.0, 0.0 };
  const Dwg_AttValue given[]
      = { { (char *)"CABLE", (char *)"10 mm\xC2\xB2 caf\xC3\xA9", 0.0 },
          { (char *)"NOTE",
            (char *)"30\xC2\xB0 Stra\xC3\x9F"
                    "e \xE2\x82\xAC 5",
            0.0 } };
  const Expected_Attrib utf8[]
      = { { "CABLE", "10 mm\xC2\xB2 caf\xC3\xA9", 0, 0 },
          { "NOTE",
            "30\xC2\xB0 Stra\xC3\x9F"
            "e \xE2\x82\xAC 5",
            0, 0 } };
  const Expected_Attrib cp1252[] = { { "CABLE", "10 mm\xB2 caf\xE9", 0, 0 },
                                     { "NOTE",
                                       "30\xB0 Stra\xDF"
                                       "e \x80 5",
                                       0, 0 } };
  Dwg_Entity_INSERT *ins;
  BITCODE_RLL h;
  Dwg_Data readback;
  int failures = 0, error = 0;

  if (!dst)
    {
      FAIL ("non-ASCII attributes: copying Trees failed");
      return 1;
    }
  def = block_header (dst, def_h);
  if (!(cable = dwg_add_ATTDEF (def, 2.5, 0, "Cable", &p1, "CABLE", "#"))
      || !(note = dwg_add_ATTDEF (def, 2.5, 0, "Note", &p2, "NOTE", "-")))
    {
      FAIL ("non-ASCII attributes: adding the definitions failed");
      free_dwg (dst);
      free (dst);
      return 1;
    }
  // the add API leaves the width factor 0
  cable->width_factor = note->width_factor = 1.0;

  loglevel = tracelevel;
  ins = dwg_dynblock_insert (model_space (dst), def, &at, 1.0, 0.0, NULL, 0,
                             given, 2, &error);
  loglevel = test_loglevel;
  if (!ins || error)
    {
      FAIL ("non-ASCII attributes: placing failed: 0x%x", error);
      free_dwg (dst);
      free (dst);
      return 1;
    }
  h = handle_of (dst, ins);
  failures += check_attribs ("non-ASCII attributes, in UTF-8", dst, h, def_h,
                             &at, 1.0, 0.0, utf8, 2, 1);

  error = write_dwg (WRITTEN_NON_ASCII, dst);
  if (error < DWG_ERR_CRITICAL)
    error = read_dwg (WRITTEN_NON_ASCII, &readback);
  if (error >= DWG_ERR_CRITICAL)
    {
      FAIL ("non-ASCII attributes: write or read back 0x%x", error);
      failures++;
    }
  else
    {
      if (readback.header.codepage != CP_ANSI_1252)
        {
          FAIL ("non-ASCII attributes: read back with codepage %u",
                (unsigned)readback.header.codepage);
          failures++;
        }
      failures += check_attribs ("read back non-ASCII attributes, in "
                                 "codepage 1252",
                                 &readback, h, def_h, &at, 1.0, 0.0, cp1252,
                                 2, 1);
      free_dwg (&readback);
    }
  if (!failures)
    unlink (WRITTEN_NON_ASCII);
  free_dwg (dst);
  free (dst);
  return failures;
}

/* An AcDbAttr EED group that isn't all 1070 items, or whose items don't
   hold all the bytes read for it, is left out of its ATTRIB, with
   DWG_ERR_INVALIDEED and the INSERT returned, and the ATTRIBs after it
   still get theirs: on a copy of Trees given three attribute definitions,
   with 1070 0 and 1071 1, with 1070 0 and 1070 1, and with 1070 0 and
   1070 1 read from 7 bytes, placed at the definition's values. */
static int
test_attr_eed_left_out (Dwg_Data *src)
{
  BITCODE_RLL def_h = 0;
  Dwg_Data *dst = document_with (src, TREES, &def_h);
  Dwg_Object_BLOCK_HEADER *def = dst ? block_header (dst, def_h) : NULL;
  const dwg_point_3d p1 = { 1.0, 2.0, 0.0 }, p2 = { 1.0, -2.0, 0.0 },
                     p3 = { 1.0, -6.0, 0.0 };
  const BITCODE_3BD at = { 10.0, 20.0, 0.0 };
  const char *const tags[3] = { "BAD", "GOOD", "SHORT" };
  const int want[3] = { -1, 2, -1 };
  Dwg_Entity_ATTDEF *bad = NULL, *good = NULL, *cut = NULL;
  Dwg_Entity_INSERT *ins;
  int failures = 0, error = 0;

  if (!def || !(bad = dwg_add_ATTDEF (def, 2.5, 0, "Bad", &p1, "BAD", "B"))
      || !(good = dwg_add_ATTDEF (def, 2.5, 0, "Good", &p2, "GOOD", "G"))
      || !(cut = dwg_add_ATTDEF (def, 2.5, 0, "Short", &p3, "SHORT", "S"))
      || add_attr_eed (dst, bad->parent, 71, 1)
      || add_attr_eed (dst, good->parent, 70, 1)
      || add_attr_eed (dst, cut->parent, 70, 1)
      // as read from a file, with a byte more than its items hold
      || !(cut->parent->eed[0].raw = (BITCODE_TF)calloc (7, 1)))
    {
      FAIL ("AcDbAttr left out: adding the definitions failed");
      if (dst)
        {
          free_dwg (dst);
          free (dst);
        }
      return 1;
    }
  cut->parent->eed[0].size = 7;

  loglevel = tracelevel;
  ins = dwg_dynblock_insert (model_space (dst), def, &at, 1.0, 0.0, NULL, 0,
                             NULL, 0, &error);
  loglevel = test_loglevel;
  if (!ins || error != DWG_ERR_INVALIDEED || ins->num_owned != 3)
    {
      FAIL ("AcDbAttr left out: 0x%x, %u ATTRIBs, expected 0x%x and 3",
            error, ins ? (unsigned)ins->num_owned : 0u,
            (unsigned)DWG_ERR_INVALIDEED);
      free_dwg (dst);
      free (dst);
      return 1;
    }
  for (unsigned k = 0; k < 3; k++)
    {
      const Dwg_Object *att = object (dst, ref (ins->attribs[k]));
      BITCODE_RS v[2] = { 9, 9 };
      const int n = att && att->fixedtype == DWG_TYPE_ATTRIB
                        ? attr_eed (dst, att, v)
                        : -3;
      if (n == want[k] && (n != 2 || (v[0] == 0 && v[1] == 1))
          && !strcmp (att->tio.entity->tio.ATTRIB->tag, tags[k]))
        OK ("AcDbAttr left out: ATTRIB %s has %s", tags[k],
            n == 2 ? "1070 0, 1070 1" : "no AcDbAttr group");
      else
        {
          FAIL ("AcDbAttr left out: ATTRIB %u (%s) has %d AcDbAttr items "
                "(%u, %u), expected %d",
                k, tags[k], n, (unsigned)v[0], (unsigned)v[1], want[k]);
          failures++;
        }
    }
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

/* Places def at the origin at scale with atts, and gets the insertion
   point of its only ATTRIB into *x and *y. Returns 0 when it is placed. */
static int
attrib_start (Dwg_Data *dwg, Dwg_Object_BLOCK_HEADER *def, const double scale,
              const Dwg_AttValue *atts, const BITCODE_BL natts, double *x,
              double *y)
{
  const BITCODE_3BD pt = { 0.0, 0.0, 0.0 };
  int error = 0;
  const Dwg_Entity_INSERT *ins;
  const Dwg_Object *att = NULL;
  loglevel = tracelevel;
  ins = dwg_dynblock_insert (model_space (dwg), def, &pt, scale, 0.0, NULL, 0,
                             atts, natts, &error);
  loglevel = test_loglevel;
  if (ins && !error && ins->num_owned == 1)
    att = object (dwg, ref (ins->attribs[0]));
  if (!att || att->fixedtype != DWG_TYPE_ATTRIB)
    return 1;
  *x = att->tio.entity->tio.ATTRIB->ins_pt.x;
  *y = att->tio.entity->tio.ATTRIB->ins_pt.y;
  return 0;
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

/* Places the Toilet def, its Angle's end point turned 1 from its base
   angle point, at Angle 2 pi 2^60, a turn of 0, with the n values v: its
   rotation parameter's record holds the end point turned back onto the
   base angle point's direction, and the Angle reads back as 0. */
static int
test_huge_rotation (Dwg_Data *dwg, const BITCODE_RLL def,
                    const Dwg_DynValue *v, const BITCODE_BL n,
                    const char *what)
{
  const BITCODE_3BD pt = { 0.0, 0.0, 0.0 };
  const Dwg_Object *obj
      = graph_node (dwg, def, DWG_TYPE_BLOCKROTATIONPARAMETER, NULL);
  const Dwg_Object_BLOCKROTATIONPARAMETER *rotation;
  BITCODE_RLL param;
  BITCODE_BL nodeid;
  BITCODE_3BD base, base_angle;
  Dwg_Entity_INSERT *insert;
  Dwg_Object_BLOCK_HEADER *read_def = NULL;
  Dwg_DynValue *values = NULL;
  BITCODE_BL count = 0;
  const Dwg_Object *data, *xrec;
  const Dwg_Resbuf *rb;
  char key[16];
  double angle = -1.0;
  int error = 0;
  if (!obj)
    {
      FAIL ("insert %s: the rotation parameter not found", what);
      return 1;
    }
  rotation = obj->tio.object->tio.BLOCKROTATIONPARAMETER;
  param = obj->handle.value;
  nodeid = rotation->evalexpr.nodeid;
  base = rotation->def_basept;
  base_angle = rotation->def_base_angle_pt;
  loglevel = tracelevel;
  insert = dwg_dynblock_insert (model_space (dwg), block_header (dwg, def),
                                &pt, 1.0, 0.0, v, n, NULL, 0, &error);
  loglevel = test_loglevel;
  if (!insert || error)
    {
      FAIL ("insert %s: 0x%x", what, error);
      return 1;
    }
  data = enhanced_block_data (dwg, object (dwg, handle_of (dwg, insert)));
  snprintf (key, sizeof (key), "%u", (unsigned)nodeid);
  xrec = dict_item (dwg, data, key);
  // 1071, 1071, 70, 70, then the base and end points
  rb = xrec && xrec->fixedtype == DWG_TYPE_XRECORD
           ? xrec->tio.object->tio.XRECORD->xdata
           : NULL;
  for (int i = 0; rb && i < 5; i++)
    rb = rb->nextrb;
  if (!dwg_dynblock_get_values (insert, &read_def, &values, &count))
    for (BITCODE_BL i = 0; i < count; i++)
      if (!strcmp (values[i].name, "Angle"))
        angle = values[i].value;
  dwg_dynblock_free_values (values, count);
  loglevel = test_loglevel;
  // the end point is the base angle point, as both are as far from the
  // base point
  if (rb && rb->type == 10
      && fabs (rb->value.pt[0] - base_angle.x) <= 1e-9
      && fabs (rb->value.pt[1] - base_angle.y) <= 1e-9
      && fabs (fmin (angle, 2.0 * M_PI - angle)) <= 1e-9)
    {
      OK ("insert %s: the end point turns onto the base angle point's "
          "direction, and the Angle reads back as 0",
          what);
      return 0;
    }
  FAIL ("insert %s: the end point is (%g, %g), the base angle point (%g, "
        "%g) about (%g, %g); the Angle reads back as %g (parameter " FORMAT_HV
        ")",
        what, rb ? rb->value.pt[0] : 0.0, rb ? rb->value.pt[1] : 0.0,
        base_angle.x, base_angle.y, base.x, base.y, angle, param);
  return 1;
}

/* Refusals of what a definition holds, each on a copy of the definition
   in a document of its own: a rotate or scale action whose base point is
   not (0, 0, 0), a graph node of a class whose stored value is not known,
   and a DIMENSION, which draws a block of its own, all only at other
   values than the definition's; and attribute definitions off the XY
   plane, attribute values given twice or without a tag or value, widths
   that are negative or aren't finite, a width that starts the text beyond
   what a double holds, and scales at which an attribute's placed point,
   height or thickness is beyond what a double holds. Accepted besides:
   widths whose starts a double holds though a product on the way to them
   doesn't, and a width on left-aligned text, which doesn't move it. */
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
        a[0].value = (char *)"1";
        a[0].width = -1.0;
        failures += test_refusal (dst, model_space (dst), def, &pt, 1.0, 0.0,
                                  NULL, 0, a, 1,
                                  REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                                  "a negative width");
        a[0].width = (double)NAN;
        failures += test_refusal (dst, model_space (dst), def, &pt, 1.0, 0.0,
                                  NULL, 0, a, 1,
                                  REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                                  "a width that isn't a number");
        a[0].width = (double)INFINITY;
        failures += test_refusal (dst, model_space (dst), def, &pt, 1.0, 0.0,
                                  NULL, 0, a, 1,
                                  REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                                  "an infinite width");
        // centred, a width that starts the text beyond a double
        attdef->horiz_alignment = 1;
        a[0].width = 1e308;
        failures += test_refusal (dst, model_space (dst), def, &pt, 10.0, 0.0,
                                  NULL, 0, a, 1,
                                  REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                                  "an attribute started beyond a double by "
                                  "its width");
        // centred, starts that a double holds, though a product on the
        // way to them doesn't: aligned at the origin, half the width at
        // the ATTRIB's height back
        attdef->alignment_pt.x = at.x;
        attdef->alignment_pt.y = at.y;
        {
          double x = 0.0, y = 1.0;
          // the ATTRIB 1e306 high, 10 wide
          attdef->height = 1e308;
          a[0].width = 10.0;
          if (!attrib_start (dst, def, 0.01, a, 1, &x, &y)
              && fabs (x + 5e306) <= 5e294 && y == 0.0)
            OK ("insert starts a 1e306 high attribute value 10 wide at %g",
                x);
          else
            {
              FAIL ("insert of a 1e306 high attribute value 10 wide: (%g, "
                    "%g), expected (-5e306, 0)",
                    x, y);
              failures++;
            }
          // 2 high, 1e308 wide
          attdef->height = 2.0;
          a[0].width = 1e308;
          x = 0.0;
          y = 1.0;
          if (!attrib_start (dst, def, 1.0, a, 1, &x, &y)
              && fabs (x + 1e308) <= 1e296 && y == 0.0)
            OK ("insert starts a 2 high attribute value 1e308 wide at %g",
                x);
          else
            {
              FAIL ("insert of a 2 high attribute value 1e308 wide: (%g, "
                    "%g), expected (-1e308, 0)",
                    x, y);
              failures++;
            }
          // right-aligned and turned 45 degrees, 2 high and 1e308 wide:
          // the offset, 2e308, isn't held, but each coordinate is
          attdef->horiz_alignment = 2;
          attdef->rotation = M_PI / 4.0;
          x = y = 0.0;
          if (!attrib_start (dst, def, 1.0, a, 1, &x, &y)
              && fabs (x + 1e308 * (2.0 * cos (M_PI / 4.0))) <= 2e296
              && fabs (y + 1e308 * (2.0 * sin (M_PI / 4.0))) <= 2e296)
            OK ("insert starts a turned 2 high attribute value 1e308 wide "
                "at (%g, %g)",
                x, y);
          else
            {
              FAIL ("insert of a turned 2 high attribute value 1e308 wide: "
                    "(%g, %g), expected (-1.4142e308, -1.4142e308)",
                    x, y);
              failures++;
            }
          attdef->horiz_alignment = 1;
          attdef->rotation = 0.0;
          attdef->height = 1.0;
        }
        attdef->alignment_pt.x = attdef->alignment_pt.y = 0.0;
        // left-aligned, a width doesn't move it
        attdef->horiz_alignment = 0;
        {
          double x = 1.0, y = 1.0;
          if (!attrib_start (dst, def, 10.0, a, 1, &x, &y) && x == pt.x
              && y == pt.y)
            OK ("insert accepts a width on left-aligned text, which starts "
                "where its definition does");
          else
            {
              FAIL ("insert of a width on left-aligned text: its ATTRIB at "
                    "(%g, %g), expected (%g, %g)",
                    x, y, pt.x, pt.y);
              failures++;
            }
        }
        a[0].width = 0.0;

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

  // A rotation given as a huge angle, against a default of 1: the Toilet's
  // Angle, its end point turned 1 from the base angle point. 2 pi 2^60 is
  // a turn of 0, so a change, which turns the end point back onto the base
  // angle point's direction; 1 + 4 pi is the default.
  dst = document_with (src, TOILET, &def_h);
  {
    Dwg_Object *obj
        = dst ? graph_node (dst, def_h, DWG_TYPE_BLOCKROTATIONPARAMETER, NULL)
              : NULL;
    Dwg_Object_BLOCKROTATIONPARAMETER *rotation
        = obj ? obj->tio.object->tio.BLOCKROTATIONPARAMETER : NULL;
    Dwg_DynValue rv[2];
    if (rotation)
      {
        const double dx
            = rotation->def_base_angle_pt.x - rotation->def_basept.x;
        const double dy
            = rotation->def_base_angle_pt.y - rotation->def_basept.y;
        rotation->def_endpt.x
            = rotation->def_basept.x + dx * cos (1.0) - dy * sin (1.0);
        rotation->def_endpt.y
            = rotation->def_basept.y + dx * sin (1.0) + dy * cos (1.0);
        memset (rv, 0, sizeof (rv));
        rv[0].name = (char *)"Angle";
        rv[0].value = ldexp (2.0 * M_PI, 60);
        failures += test_huge_rotation (dst, def_h, rv, 1,
                                        "a rotation of 2 pi 2^60, from a "
                                        "default of 1");
        rv[1].name = (char *)"Type (view)";
        rv[1].state = (char *)"Round (Plan)";
        failures += test_huge_rotation (dst, def_h, rv, 2,
                                        "a rotation of 2 pi 2^60, from a "
                                        "default of 1, at another state");
        rv[0].value = 1.0 + 4.0 * M_PI;
        failures += test_accepted (dst, def_h, 1.0, rv, 1,
                                   "a rotation of 1 + 4 pi, the default of "
                                   "1");
      }
    else
      {
        FAIL ("the Toilet has no rotation parameter");
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

/*--------------------------------------------------------------------------------
 * Changed lengths and angles: the parameters' points the records hold
 */

/* The node of def's evaluation graph of class type with the given name. */
static Dwg_Object *
named_node (const Dwg_Data *dwg, const BITCODE_RLL def,
            const Dwg_Object_Type type, const char *name)
{
  const Dwg_Object_EVALUATION_GRAPH *g = evaluation_graph (dwg, def);
  for (BITCODE_BL i = 0; g && g->nodes && i < g->num_nodes; i++)
    {
      Dwg_Object *node = object (dwg, ref (g->nodes[i].evalexpr));
      // every node starts with BLOCKELEMENT_fields, and so with its name
      if (node && node->fixedtype == type
          && node->tio.object->tio.BLOCKMOVEACTION->name
          && !strcmp (node->tio.object->tio.BLOCKMOVEACTION->name, name))
        return node;
    }
  return NULL;
}

static BITCODE_BL
nodeid_of (const Dwg_Object *node)
{
  // every node starts with BLOCKELEMENT_fields
  return node->tio.object->tio.BLOCKMOVEACTION->evalexpr.nodeid;
}

/* Appends to stretch action a the parameter with node id nodeid, with the
   n stretch points indexes. Returns 0 when memory runs out. */
static int
add_stretch_code (Dwg_Object_BLOCKSTRETCHACTION *a, const BITCODE_BL nodeid,
                  const BITCODE_BL *indexes, const BITCODE_BS n)
{
  Dwg_BLOCKSTRETCHACTION_codes *codes, *c;
  codes = (Dwg_BLOCKSTRETCHACTION_codes *)realloc (
      a->codes, (a->num_codes + 1) * sizeof (Dwg_BLOCKSTRETCHACTION_codes));
  if (!codes)
    return 0;
  a->codes = codes;
  c = &codes[a->num_codes];
  memset (c, 0, sizeof (*c));
  c->parent = a;
  c->bl95 = nodeid;
  c->indexes = (BITCODE_BL *)malloc (n * sizeof (BITCODE_BL));
  if (!c->indexes)
    return 0;
  memcpy (c->indexes, indexes, n * sizeof (BITCODE_BL));
  c->num_indexes = n;
  a->num_codes++;
  return 1;
}

/* Removes the parameter add_stretch_code () appended last. */
static void
pop_stretch_code (Dwg_Object_BLOCKSTRETCHACTION *a)
{
  a->num_codes--;
  free (a->codes[a->num_codes].indexes);
  a->codes[a->num_codes].indexes = NULL;
}

/* Appends an entity to the list of stretch action a, with the n stretch
   points indexes. Returns 0 when memory runs out. */
static int
add_stretch_entity (Dwg_Data *dwg, Dwg_Object_BLOCKSTRETCHACTION *a,
                    const BITCODE_RLL handle, const BITCODE_BL *indexes,
                    const BITCODE_BS n)
{
  Dwg_BLOCKSTRETCHACTION_handles *hdls, *h;
  hdls = (Dwg_BLOCKSTRETCHACTION_handles *)realloc (
      a->hdls, (a->num_hdls + 1) * sizeof (Dwg_BLOCKSTRETCHACTION_handles));
  if (!hdls)
    return 0;
  a->hdls = hdls;
  h = &hdls[a->num_hdls];
  memset (h, 0, sizeof (*h));
  h->parent = a;
  h->hdl = dwg_add_handleref (dwg, 4, handle, NULL);
  h->indexes = (BITCODE_BL *)malloc (n * sizeof (BITCODE_BL));
  if (!h->indexes)
    return 0;
  memcpy (h->indexes, indexes, n * sizeof (BITCODE_BL));
  h->num_indexes = n;
  a->num_hdls++;
  return 1;
}

/* Appends handle to the selection *deps of *num_deps objects of an
   action, or node id nodeid to its list *actions of *num_actions. */
static int
add_dep (Dwg_Data *dwg, BITCODE_H **deps, BITCODE_BL *num_deps,
         const BITCODE_RLL handle)
{
  BITCODE_H *more
      = (BITCODE_H *)realloc (*deps, (*num_deps + 1) * sizeof (BITCODE_H));
  if (!more)
    return 0;
  *deps = more;
  more[(*num_deps)++] = dwg_add_handleref (dwg, 4, handle, NULL);
  return 1;
}

static int
add_action (BITCODE_BL **actions, BITCODE_BL *num_actions,
            const BITCODE_BL nodeid)
{
  BITCODE_BL *more = (BITCODE_BL *)realloc (
      *actions, (*num_actions + 1) * sizeof (BITCODE_BL));
  if (!more)
    return 0;
  *actions = more;
  more[(*num_actions)++] = nodeid;
  return 1;
}

/* dwg_dynblock_insert () refuses def at the n values v, as the stored
   state of a placement at them is not known, adding nothing, while
   dwg_dynblock_evaluate () evaluates them. */
static int
test_unknown (Dwg_Data *dwg, const BITCODE_RLL def, const Dwg_DynValue *v,
              const BITCODE_BL n, const char *what)
{
  const BITCODE_3BD pt = { 0.0, 0.0, 0.0 };
  int failures, error = 0;
  failures = test_refusal (dwg, model_space (dwg), block_header (dwg, def),
                           &pt, 1.0, 0.0, v, n, NULL, 0,
                           REFUSED (DWG_ERR_NOTYETSUPPORTED), what);
  loglevel = tracelevel;
  if (dwg_dynblock_evaluate (block_header (dwg, def), v, n, &error)
      && error < DWG_ERR_CRITICAL)
    OK ("%s: evaluated all the same: 0x%x", what, error);
  else
    {
      FAIL ("%s: not evaluated: 0x%x", what, error);
      failures++;
    }
  loglevel = test_loglevel;
  return failures;
}

/* The Toilet's rotation parameter, and with with_action its rotate action
   'Rotate Plan', moved into the Window's evaluation graph under node ids
   it doesn't use, the action turning only the Window's LINE turned, as
   in dynblock_eval_test. */
typedef struct
{
  Dwg_Object_EVALUATION_GRAPH *graph;
  BITCODE_BL num_nodes;
  Dwg_Object_BLOCKROTATIONPARAMETER *param;
  Dwg_Object_BLOCKROTATEACTION *action;
  BITCODE_BL param_id, action_id;
  BITCODE_BL codes[3];
  BITCODE_H *deps;
  BITCODE_BL num_deps;
  BITCODE_BL *actions;
  BITCODE_BL num_actions;
  BITCODE_H line;
  BITCODE_BL action_list[1];
} Graft;

#define GRAFT_PARAM 905
#define GRAFT_ACTION 920
#define GRAFT_BASEPOINT 930

static int
graft_rotation (Dwg_Data *dwg, const BITCODE_RLL window,
                const BITCODE_RLL toilet, const BITCODE_RLL turned,
                const int with_action, Graft *g)
{
  Dwg_Object *param = graph_node (dwg, toilet, DWG_TYPE_BLOCKROTATIONPARAMETER,
                                  NULL);
  Dwg_Object *action
      = named_node (dwg, toilet, DWG_TYPE_BLOCKROTATEACTION, "Rotate Plan");
  Dwg_EVAL_Node *nodes;
  memset (g, 0, sizeof (*g));
  g->graph = evaluation_graph (dwg, window);
  if (!param || !action || !g->graph)
    return 0;
  nodes = (Dwg_EVAL_Node *)realloc (
      g->graph->nodes, (g->graph->num_nodes + 2) * sizeof (Dwg_EVAL_Node));
  if (!nodes)
    return 0;
  g->graph->nodes = nodes;
  memset (&nodes[g->graph->num_nodes], 0, 2 * sizeof (Dwg_EVAL_Node));
  g->num_nodes = g->graph->num_nodes;
  nodes[g->graph->num_nodes++].evalexpr
      = dwg_add_handleref (dwg, 5, param->handle.value, NULL);
  if (with_action)
    nodes[g->graph->num_nodes++].evalexpr
        = dwg_add_handleref (dwg, 5, action->handle.value, NULL);
  g->param = param->tio.object->tio.BLOCKROTATIONPARAMETER;
  g->action = action->tio.object->tio.BLOCKROTATEACTION;
  g->param_id = g->param->evalexpr.nodeid;
  g->action_id = g->action->evalexpr.nodeid;
  g->param->evalexpr.nodeid = GRAFT_PARAM;
  g->action->evalexpr.nodeid = GRAFT_ACTION;
  for (int i = 0; i < 3; i++)
    {
      g->codes[i] = g->action->conn_pts[i].code;
      g->action->conn_pts[i].code = GRAFT_PARAM;
    }
  g->deps = g->action->deps;
  g->num_deps = g->action->num_deps;
  g->actions = g->action->actions;
  g->num_actions = g->action->num_actions;
  g->line = dwg_add_handleref (dwg, 4, turned, NULL);
  g->action->deps = &g->line;
  g->action->num_deps = 1;
  g->action->actions = g->action_list;
  g->action->num_actions = 0;
  return 1;
}

static void
ungraft_rotation (Graft *g)
{
  g->graph->num_nodes = g->num_nodes;
  g->param->evalexpr.nodeid = g->param_id;
  g->action->evalexpr.nodeid = g->action_id;
  for (int i = 0; i < 3; i++)
    g->action->conn_pts[i].code = g->codes[i];
  g->action->deps = g->deps;
  g->action->num_deps = g->num_deps;
  g->action->actions = g->actions;
  g->action->num_actions = g->num_actions;
}

/* The copy, in dst, of the fixture's entity handle of definition def. */
static BITCODE_RLL
entity_copy (const Dwg_Data *dst, const Dwg_Data *src, const BITCODE_RLL def,
             const BITCODE_RLL copy, const BITCODE_RLL handle)
{
  const Dwg_Object_BLOCK_HEADER *a = block_header (src, def);
  const Dwg_Object_BLOCK_HEADER *b = block_header (dst, copy);
  for (BITCODE_BL i = 0; a && b && i < a->num_owned && i < b->num_owned; i++)
    if (ref (a->entities[i]) == handle)
      return ref (b->entities[i]);
  return 0;
}

/* Placements whose stored state AutoCAD's placements have not shown, on
   copies of the fixture's definitions changed for the purpose: an action
   listing its own parameter among those it moves; a parameter an action
   selects without listing it; a stretch point a parameter's record
   doesn't hold, or one listed twice; a rotation parameter that another
   action moves; a base point parameter that a stretch, move or rotate
   action moves; and a parameter that a rotate action turns and another
   action moves. dwg_dynblock_insert () refuses each with nothing added,
   while dwg_dynblock_evaluate () evaluates the geometry, which doesn't
   depend on them. */
static int
test_unknown_states (Dwg_Data *src)
{
  Dwg_Data *dst = dwg_new_Document (R_2004, 0, tracelevel);
  BITCODE_RLL map[8][2];
  unsigned num_map;
  BITCODE_RLL window, door, toilet;
  Dwg_Object *width, *height, *wall, *swing, *angle, *basept, *obj;
  Dwg_Object_BLOCKSTRETCHACTION *stretch_width = NULL, *stretch_wall = NULL;
  Dwg_Object_BLOCKMOVEACTION *move_swing = NULL;
  Dwg_Object_BLOCKROTATEACTION *rotate_plan = NULL;
  Dwg_Object_BLOCKBASEPOINTPARAMETER *_basept;
  Dwg_DynValue v[2];
  BITCODE_BL width_id, height_id, wall_id, swing_id, angle_id, basept_id;
  BITCODE_RLL height_h, basept_h;
  Graft g;
  int failures;

  loglevel = test_loglevel;
  failures = copy_definitions (dst, src, map, &num_map);
  window = copy_of (map, num_map, WINDOW);
  door = copy_of (map, num_map, DOOR);
  toilet = copy_of (map, num_map, TOILET);
  width = named_node (dst, window, DWG_TYPE_BLOCKLINEARPARAMETER, "winwidth");
  height
      = named_node (dst, window, DWG_TYPE_BLOCKLINEARPARAMETER, "winheight");
  wall = named_node (dst, door, DWG_TYPE_BLOCKLINEARPARAMETER, "wall");
  swing = named_node (dst, door, DWG_TYPE_BLOCKFLIPPARAMETER, "swing");
  angle = graph_node (dst, toilet, DWG_TYPE_BLOCKROTATIONPARAMETER, NULL);
  // which no action of the fixture moves
  basept = graph_node (dst, door, DWG_TYPE_BLOCKBASEPOINTPARAMETER, NULL);
  if ((obj = named_node (dst, window, DWG_TYPE_BLOCKSTRETCHACTION,
                         "Stretch Width")))
    stretch_width = obj->tio.object->tio.BLOCKSTRETCHACTION;
  if ((obj = named_node (dst, door, DWG_TYPE_BLOCKSTRETCHACTION, "Stretch")))
    stretch_wall = obj->tio.object->tio.BLOCKSTRETCHACTION;
  if ((obj = named_node (dst, door, DWG_TYPE_BLOCKMOVEACTION, "MoveSwing")))
    move_swing = obj->tio.object->tio.BLOCKMOVEACTION;
  if ((obj = named_node (dst, toilet, DWG_TYPE_BLOCKROTATEACTION,
                         "Rotate Plan")))
    rotate_plan = obj->tio.object->tio.BLOCKROTATEACTION;
  if (!width || !height || !wall || !swing || !angle || !basept
      || !stretch_width || !stretch_wall || !move_swing || !rotate_plan)
    {
      FAIL ("unknown states: fixture objects not found");
      free_document (dst);
      return failures + 1;
    }
  // the objects may move as placements and evaluations add objects
  width_id = nodeid_of (width);
  height_id = nodeid_of (height);
  wall_id = nodeid_of (wall);
  swing_id = nodeid_of (swing);
  angle_id = nodeid_of (angle);
  basept_id = nodeid_of (basept);
  height_h = height->handle.value;
  basept_h = basept->handle.value;
  _basept = basept->tio.object->tio.BLOCKBASEPOINTPARAMETER;
  memset (v, 0, sizeof (v));

  // the Window at Window Width 24, which 'Stretch Width' takes
  v[0].name = (char *)"Window Width";
  v[0].value = 24.0;
  {
    const BITCODE_BL end = 1;
    if (add_stretch_code (stretch_width, width_id, &end, 1))
      {
        failures += test_unknown (dst, window, v, 1,
                                  "a stretch listing its own parameter");
        pop_stretch_code (stretch_width);
      }
    else
      failures++;
  }
  {
    const BITCODE_BL twice[2] = { 1, 1 };
    if (add_stretch_code (stretch_width, height_id, twice, 2))
      {
        failures += test_unknown (dst, window, v, 1,
                                  "a stretch listing a point of a linear "
                                  "parameter twice");
        pop_stretch_code (stretch_width);
      }
    else
      failures++;
  }
  if (add_dep (dst, &stretch_width->deps, &stretch_width->num_deps,
               height_h))
    {
      failures += test_unknown (dst, window, v, 1,
                                "a stretch selecting, not listing, another "
                                "parameter");
      stretch_width->num_deps--;
    }
  else
    failures++;

  // the Door at Wall Thickness 6, which 'Stretch' and MoveSwing take
  v[0].name = (char *)"Wall Thickness";
  v[0].value = 6.0;
  {
    const BITCODE_BL beyond = 2, twice[2] = { 1, 1 };
    if (add_stretch_code (stretch_wall, swing_id, &beyond, 1))
      {
        failures += test_unknown (dst, door, v, 1,
                                  "a stretch of a third point of a flip "
                                  "parameter");
        pop_stretch_code (stretch_wall);
      }
    else
      failures++;
    if (add_stretch_code (stretch_wall, swing_id, twice, 2))
      {
        failures += test_unknown (dst, door, v, 1,
                                  "a stretch listing a point twice");
        pop_stretch_code (stretch_wall);
      }
    else
      failures++;
    // the same point, in two entries
    if (add_stretch_code (stretch_wall, swing_id, &twice[0], 1)
        && add_stretch_code (stretch_wall, swing_id, &twice[1], 1))
      {
        failures += test_unknown (dst, door, v, 1,
                                  "a stretch listing a parameter in two "
                                  "entries");
        pop_stretch_code (stretch_wall);
        pop_stretch_code (stretch_wall);
      }
    else
      failures++;
  }
  // MoveSwing lists swing already
  if (add_action (&move_swing->actions, &move_swing->num_actions, swing_id))
    {
      failures += test_unknown (dst, door, v, 1,
                                "a move listing a parameter twice");
      move_swing->num_actions--;
    }
  else
    failures++;
  if (add_action (&move_swing->actions, &move_swing->num_actions, wall_id))
    {
      failures += test_unknown (dst, door, v, 1,
                                "a move listing its own parameter");
      move_swing->num_actions--;
    }
  else
    failures++;
  // the Door's base point parameter, whose one point is its point 0
  {
    const BITCODE_BL point = 0;
    if (add_stretch_code (stretch_wall, basept_id, &point, 1))
      {
        failures += test_unknown (dst, door, v, 1,
                                  "a stretch moving a base point parameter");
        pop_stretch_code (stretch_wall);
      }
    else
      failures++;
  }
  if (add_action (&move_swing->actions, &move_swing->num_actions, basept_id))
    {
      failures += test_unknown (dst, door, v, 1,
                                "a move moving a base point parameter");
      move_swing->num_actions--;
    }
  else
    failures++;

  // the Toilet at Angle 1.2, which 'Rotate Plan' takes
  v[0].name = (char *)"Angle";
  v[0].value = 1.2;
  if (add_action (&rotate_plan->actions, &rotate_plan->num_actions,
                  angle_id))
    {
      failures += test_unknown (dst, toilet, v, 1,
                                "a rotate action listing its own parameter");
      rotate_plan->num_actions--;
    }
  else
    failures++;
  // the Toilet's Angle with its end point at its base point, which no turn
  // moves, so that the record would read back as another angle
  angle = graph_node (dst, toilet, DWG_TYPE_BLOCKROTATIONPARAMETER, NULL);
  if (angle)
    {
      Dwg_Object_BLOCKROTATIONPARAMETER *_angle
          = angle->tio.object->tio.BLOCKROTATIONPARAMETER;
      const BITCODE_3BD endpt = _angle->def_endpt;
      _angle->def_endpt = _angle->def_basept;
      failures += test_unknown (dst, toilet, v, 1,
                                "a rotation parameter with its end point at "
                                "its base point");
      _angle->def_endpt = endpt;
    }
  else
    {
      FAIL ("unknown states: the Toilet's rotation parameter not found");
      failures++;
    }
  // the Door's base point parameter in the Toilet's graph, under a node id
  // the Toilet doesn't use, turned by 'Rotate Plan'
  {
    Dwg_Object_EVALUATION_GRAPH *graph = evaluation_graph (dst, toilet);
    Dwg_EVAL_Node *nodes
        = graph ? (Dwg_EVAL_Node *)realloc (graph->nodes,
                                            (graph->num_nodes + 1)
                                                * sizeof (Dwg_EVAL_Node))
                : NULL;
    if (nodes)
      {
        graph->nodes = nodes;
        memset (&nodes[graph->num_nodes], 0, sizeof (Dwg_EVAL_Node));
        nodes[graph->num_nodes++].evalexpr
            = dwg_add_handleref (dst, 5, basept_h, NULL);
        _basept->evalexpr.nodeid = GRAFT_BASEPOINT;
        if (add_action (&rotate_plan->actions, &rotate_plan->num_actions,
                        GRAFT_BASEPOINT))
          {
            failures += test_unknown (dst, toilet, v, 1,
                                      "a rotate action turning a base point "
                                      "parameter");
            rotate_plan->num_actions--;
          }
        else
          failures++;
        _basept->evalexpr.nodeid = basept_id;
        graph->num_nodes--;
      }
    else
      {
        FAIL ("unknown states: grafting the Door's base point parameter "
              "failed");
        failures++;
      }
  }

  // the Toilet's Angle in the Window's graph, 'Stretch Width' moving its
  // base point, at Window Width 24 and no Angle
  v[0].name = (char *)"Window Width";
  v[0].value = 24.0;
  if (graft_rotation (dst, window, toilet,
                      entity_copy (dst, src, WINDOW, window, 0xBBD5), 0, &g))
    {
      const BITCODE_BL base = 0;
      if (add_stretch_code (stretch_width, GRAFT_PARAM, &base, 1))
        {
          failures += test_unknown (dst, window, v, 1,
                                    "a stretch moving a rotation parameter");
          pop_stretch_code (stretch_width);
        }
      else
        failures++;
      ungraft_rotation (&g);
    }
  else
    {
      FAIL ("unknown states: grafting the Toilet's Angle failed");
      failures++;
    }

  // and 'Rotate Plan', turning the Window's LINE BBD5 and Window Height's
  // parameter, whose end point 'Stretch Width' moves, at Angle 1.2
  v[1].name = (char *)"Angle";
  v[1].value = 1.2;
  if (graft_rotation (dst, window, toilet,
                      entity_copy (dst, src, WINDOW, window, 0xBBD5), 1, &g))
    {
      const BITCODE_BL end = 1;
      g.action_list[0] = height_id;
      g.action->num_actions = 1;
      if (test_accepted (dst, window, 1.0, &v[1], 1,
                         "the grafted Angle, turning Window Height's "
                         "parameter")
          == 0
          && add_stretch_code (stretch_width, height_id, &end, 1))
        {
          failures += test_unknown (dst, window, v, 2,
                                    "a parameter moved by a stretch and "
                                    "turned by a rotate action");
          pop_stretch_code (stretch_width);
        }
      else
        failures++;
      ungraft_rotation (&g);
    }
  else
    {
      FAIL ("unknown states: grafting the Toilet's Angle failed");
      failures++;
    }
  free_document (dst);
  return failures;
}

/* The point i of the record placement insert stores for the parameter
   with node id nodeid into *pt. Returns 0 when it has none. */
static int
record_point (const Dwg_Data *dwg, const BITCODE_RLL insert,
              const BITCODE_BL nodeid, const int i, BITCODE_3BD *pt)
{
  const Dwg_Object *xrec;
  const Dwg_Resbuf *rb;
  char key[16];
  int n = 0;
  snprintf (key, sizeof (key), "%u", (unsigned)nodeid);
  xrec = dict_item (dwg, enhanced_block_data (dwg, object (dwg, insert)),
                    key);
  if (!xrec || xrec->fixedtype != DWG_TYPE_XRECORD)
    return 0;
  // after 1071, 1071, 70, 70
  for (rb = xrec->tio.object->tio.XRECORD->xdata; rb; rb = rb->nextrb)
    if (rb->type == 10 && n++ == i)
      {
        pt->x = rb->value.pt[0];
        pt->y = rb->value.pt[1];
        pt->z = rb->value.pt[2];
        return 1;
      }
  return 0;
}

static int
is_at (const BITCODE_3BD *pt, const double x, const double y, const double z)
{
  return fabs (pt->x - x) <= RECORD_TOLERANCE
         && fabs (pt->y - y) <= RECORD_TOLERANCE
         && fabs (pt->z - z) <= RECORD_TOLERANCE;
}

/* A value whose parameter's points after the evaluation round to points
   that don't hold it: Window Width's points moved to x 1e16 and 1e16 + 36,
   where a double holds every second integer, so that a width of 1 puts the
   end point 0 or 2 from the base point, while 24 is held; and the Toilet's
   Angle with its base point at (1e16, 1e16) and its end point 2 from it,
   which a turn of 1 puts on the grid's diagonal. Each is refused before
   anything is added. */
static int
test_round_trip (Dwg_Data *src)
{
  const BITCODE_3BD pt = { 0.0, 0.0, 0.0 };
  BITCODE_RLL def_h = 0;
  Dwg_Data *dst = document_with (src, WINDOW, &def_h);
  Dwg_Object *obj
      = dst ? named_node (dst, def_h, DWG_TYPE_BLOCKLINEARPARAMETER,
                          "winwidth")
            : NULL;
  Dwg_DynValue v;
  int failures = 0;
  memset (&v, 0, sizeof (v));
  if (!obj)
    {
      FAIL ("round trip: the Window's Window Width not found");
      free_document (dst);
      return 1;
    }
  {
    Dwg_Object_BLOCKLINEARPARAMETER *width
        = obj->tio.object->tio.BLOCKLINEARPARAMETER;
    width->value_set.flags = 0;
    width->def_basept.x = 1e16;
    width->def_endpt.x = 1e16 + 36.0;
  }
  v.name = (char *)"Window Width";
  v.value = 1.0;
  failures += test_refusal (dst, model_space (dst), block_header (dst, def_h),
                            &pt, 1.0, 0.0, &v, 1, NULL, 0,
                            REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "a linear value its rounded points don't hold");
  v.value = 24.0;
  failures += test_accepted (dst, def_h, 1.0, &v, 1,
                             "a linear value its points far out hold");
  free_document (dst);

  dst = document_with (src, TOILET, &def_h);
  obj = dst ? graph_node (dst, def_h, DWG_TYPE_BLOCKROTATIONPARAMETER, NULL)
            : NULL;
  if (!obj)
    {
      FAIL ("round trip: the Toilet's Angle not found");
      free_document (dst);
      return failures + 1;
    }
  {
    Dwg_Object_BLOCKROTATIONPARAMETER *angle
        = obj->tio.object->tio.BLOCKROTATIONPARAMETER;
    angle->def_basept.x = angle->def_basept.y = 1e16;
    angle->def_endpt = angle->def_basept;
    angle->def_endpt.x += 2.0;
    angle->def_base_angle_pt = angle->def_endpt;
  }
  v.name = (char *)"Angle";
  v.value = 1.0;
  failures += test_refusal (dst, model_space (dst), block_header (dst, def_h),
                            &pt, 1.0, 0.0, &v, 1, NULL, 0,
                            REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "a rotation value its rounded points don't hold");
  free_document (dst);
  return failures;
}

/* An ATTDEF at x -2^90, its position locked, that two of the Window's
   stretch actions move, by 2^90 and then by 2^32 ('Stretch Divider' made
   to take 2^-58 of the change), at Window Width 2^90 + 36: it ends at
   2^32, while the sum of the moves rounds to 2^90 and would put it at 0.
   Placed at a scale of 1e300 it is beyond what a double holds, which is
   refused before anything is added. */
static int
test_attdef_moved_twice (Dwg_Data *src)
{
  BITCODE_RLL def_h = 0;
  Dwg_Data *dst = document_with (src, WINDOW, &def_h);
  const BITCODE_3BD origin = { 0.0, 0.0, 0.0 };
  const dwg_point_3d at = { -ldexp (1.0, 90), 10.0, 0.0 };
  const BITCODE_BL point = 0;
  Dwg_Object *width, *obj;
  Dwg_Object_BLOCKSTRETCHACTION *stretch, *divider;
  Dwg_Entity_ATTDEF *attdef;
  BITCODE_RLL attdef_h;
  Dwg_DynValue v;
  int failures = 0;
  memset (&v, 0, sizeof (v));
  attdef = dst ? dwg_add_ATTDEF (block_header (dst, def_h), 2.5, 0, "Mark",
                                 &at, "MARK", "M")
               : NULL;
  loglevel = test_loglevel;
  width = attdef ? named_node (dst, def_h, DWG_TYPE_BLOCKLINEARPARAMETER,
                               "winwidth")
                 : NULL;
  if (!width)
    {
      FAIL ("ATTDEF moved twice: adding an ATTDEF to the Window failed");
      free_document (dst);
      return 1;
    }
  attdef_h = handle_of (dst, attdef);
  width->tio.object->tio.BLOCKLINEARPARAMETER->value_set.flags = 0;
  if (add_attr_eed (dst, attdef->parent, 70, 1))
    {
      FAIL ("ATTDEF moved twice: locking the ATTDEF failed");
      free_document (dst);
      return 1;
    }
  obj = named_node (dst, def_h, DWG_TYPE_BLOCKSTRETCHACTION, "Stretch Width");
  stretch = obj ? obj->tio.object->tio.BLOCKSTRETCHACTION : NULL;
  obj = named_node (dst, def_h, DWG_TYPE_BLOCKSTRETCHACTION,
                    "Stretch Divider");
  divider = obj ? obj->tio.object->tio.BLOCKSTRETCHACTION : NULL;
  if (!stretch || !divider
      || !add_dep (dst, &stretch->deps, &stretch->num_deps, attdef_h)
      || !add_stretch_entity (dst, stretch, attdef_h, &point, 1)
      || !add_dep (dst, &divider->deps, &divider->num_deps, attdef_h)
      || !add_stretch_entity (dst, divider, attdef_h, &point, 1))
    {
      FAIL ("ATTDEF moved twice: adding it to the stretch actions failed");
      free_document (dst);
      return 1;
    }
  divider->action_offset_x = ldexp (1.0, -58);
  v.name = (char *)"Window Width";
  v.value = ldexp (1.0, 90) + 36.0;
  failures += test_refusal (dst, model_space (dst), block_header (dst, def_h),
                            &origin, 1e300, 0.0, &v, 1, NULL, 0,
                            REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "an ATTDEF moved twice to beyond a double");
  free_document (dst);
  return failures;
}

/* Parameters the fixture's actions don't move, made to: the Door's
   'Stretch' moving its visibility parameter's point (index 0), and two
   ATTDEFs added to the Window in 'Stretch Width''s frame and selection,
   one with its position locked. At Wall Thickness 6 the visibility
   parameter's record holds its point moved 1 down, as Wall Thickness's
   end point moves; at Window Width 24 the locked ATTDEF's ATTRIB is where
   the evaluation moved its ATTDEF, 12 to the left, and, centred with a
   width given, starts back by half the width from where the evaluation
   moved its alignment point, while the other's stays where the
   definition's ATTDEF is, though the stretch moves the representation
   block's copy, as AutoCAD leaves an ATTRIB that isn't locked; and at a
   huge scale one placed beyond a double is refused before anything is
   added, while the definition's own place is not. */
static int
test_moved_parameters (Dwg_Data *src)
{
  BITCODE_RLL def_h = 0;
  Dwg_Data *dst = document_with (src, DOOR, &def_h);
  const BITCODE_3BD at = { 100.0, 200.0, 0.0 };
  const dwg_point_3d attdef_pt = { 35.0, 10.0, 0.0 };
  Dwg_Object *obj, *vis;
  Dwg_Object_BLOCKSTRETCHACTION *stretch;
  Dwg_Entity_INSERT *insert;
  Dwg_DynValue v;
  BITCODE_3BD pt = { 0.0, 0.0, 0.0 }, def_pt;
  BITCODE_BL vis_id;
  int failures = 0, error = 0;

  memset (&v, 0, sizeof (v));
  obj = dst ? named_node (dst, def_h, DWG_TYPE_BLOCKSTRETCHACTION, "Stretch")
            : NULL;
  vis = dst ? graph_node (dst, def_h, DWG_TYPE_BLOCKVISIBILITYPARAMETER, NULL)
            : NULL;
  if (!obj || !vis)
    {
      FAIL ("moved parameters: the Door's objects not found");
      free_document (dst);
      return 1;
    }
  stretch = obj->tio.object->tio.BLOCKSTRETCHACTION;
  def_pt = vis->tio.object->tio.BLOCKVISIBILITYPARAMETER->def_pt;
  // the object may move as the placement adds objects
  vis_id = nodeid_of (vis);
  {
    const BITCODE_BL point = 0;
    if (!add_stretch_code (stretch, vis_id, &point, 1))
      {
        free_document (dst);
        return 1;
      }
  }
  v.name = (char *)"Wall Thickness";
  v.value = 6.0;
  loglevel = tracelevel;
  insert = dwg_dynblock_insert (model_space (dst), block_header (dst, def_h),
                                &at, 2.0, 1.0, &v, 1, NULL, 0, &error);
  loglevel = test_loglevel;
  if (insert && !error
      && record_point (dst, handle_of (dst, insert), vis_id, 0, &pt)
      && is_at (&pt, def_pt.x, def_pt.y - 1.0, def_pt.z))
    OK ("moved parameters: the visibility parameter's record holds its "
        "point moved 1 down");
  else
    {
      FAIL ("moved parameters: the visibility parameter's point: 0x%x, "
            "(%g, %g), expected (%g, %g)",
            error, insert ? pt.x : 0.0, insert ? pt.y : 0.0, def_pt.x,
            def_pt.y - 1.0);
      failures++;
    }
  // the flip parameter 'swing', which MoveSwing moves by half of Wall
  // Thickness's change, made to lie at y -1.7e308: a Wall Thickness of
  // 1e308, which the value set is made to allow, would move it beyond what
  // a double holds, while no entity goes that far
  {
    Dwg_Object *swing
        = named_node (dst, def_h, DWG_TYPE_BLOCKFLIPPARAMETER, "swing");
    Dwg_Object *wall
        = named_node (dst, def_h, DWG_TYPE_BLOCKLINEARPARAMETER, "wall");
    if (swing && wall)
      {
        Dwg_Object_BLOCKFLIPPARAMETER *_swing
            = swing->tio.object->tio.BLOCKFLIPPARAMETER;
        wall->tio.object->tio.BLOCKLINEARPARAMETER->value_set.flags = 0;
        _swing->def_basept.y = _swing->def_endpt.y = -1.7e308;
        v.value = 1e308;
        failures += test_refusal (dst, model_space (dst),
                                  block_header (dst, def_h), &at, 1.0, 0.0,
                                  &v, 1, NULL, 0,
                                  REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                                  "a value moving a parameter's point beyond "
                                  "a double");
      }
    else
      {
        FAIL ("moved parameters: the Door's swing or wall not found");
        failures++;
      }
  }
  free_document (dst);

  dst = document_with (src, WINDOW, &def_h);
  obj = dst ? named_node (dst, def_h, DWG_TYPE_BLOCKSTRETCHACTION,
                          "Stretch Width")
            : NULL;
  if (obj)
    {
      const dwg_point_3d note_pt = { 35.0, 12.0, 0.0 };
      Dwg_Entity_ATTDEF *attdef
          = dwg_add_ATTDEF (block_header (dst, def_h), 2.5, 0, "Mark",
                            &attdef_pt, "MARK", "M");
      Dwg_Entity_ATTDEF *note
          = attdef ? dwg_add_ATTDEF (block_header (dst, def_h), 2.5, 0,
                                     "Note", &note_pt, "NOTE", "N")
                   : NULL;
      const BITCODE_RLL attdef_h = attdef ? handle_of (dst, attdef) : 0;
      const BITCODE_RLL note_h = note ? handle_of (dst, note) : 0;
      const BITCODE_BL point = 0;
      const BITCODE_3BD origin = { 0.0, 0.0, 0.0 };
      loglevel = test_loglevel;
      if (!note || add_attr_eed (dst, attdef->parent, 70, 1))
        {
          FAIL ("moved parameters: adding the ATTDEFs to the Window failed");
          free_document (dst);
          return failures + 1;
        }
      // found again, as adding the ATTDEFs may have moved the objects
      obj = named_node (dst, def_h, DWG_TYPE_BLOCKSTRETCHACTION,
                        "Stretch Width");
      stretch = obj->tio.object->tio.BLOCKSTRETCHACTION;
      if (!add_dep (dst, &stretch->deps, &stretch->num_deps, attdef_h)
          || !add_stretch_entity (dst, stretch, attdef_h, &point, 1)
          || !add_dep (dst, &stretch->deps, &stretch->num_deps, note_h)
          || !add_stretch_entity (dst, stretch, note_h, &point, 1))
        {
          FAIL ("moved parameters: adding an ATTDEF to the Window failed");
          free_document (dst);
          return failures + 1;
        }
      v.name = (char *)"Window Width";
      v.value = 24.0;
      loglevel = tracelevel;
      insert = dwg_dynblock_insert (model_space (dst),
                                    block_header (dst, def_h), &at, 1.0, 0.0,
                                    &v, 1, NULL, 0, &error);
      loglevel = test_loglevel;
      if (!block_header (dst, def_h))
        {
          FAIL ("moved parameters: the Window's copy not found");
          free_document (dst);
          return failures + 1;
        }
      {
        const BITCODE_3BD base = block_header (dst, def_h)->base_pt;
        const Dwg_Object *attrib
            = insert && insert->num_owned
                  ? object (dst, ref (insert->attribs[0]))
                  : NULL;
        const Dwg_Entity_ATTRIB *_attrib
            = attrib && attrib->fixedtype == DWG_TYPE_ATTRIB
                  ? attrib->tio.entity->tio.ATTRIB
                  : NULL;
        const Dwg_Object *note_attrib
            = insert && insert->num_owned == 2
                  ? object (dst, ref (insert->attribs[1]))
                  : NULL;
        const Dwg_Entity_ATTRIB *_note
            = note_attrib && note_attrib->fixedtype == DWG_TYPE_ATTRIB
                  ? note_attrib->tio.entity->tio.ATTRIB
                  : NULL;
        const Dwg_Object_BLOCK_HEADER *rep
            = insert ? block_header (dst, ref (insert->block_header)) : NULL;
        const Dwg_Entity_ATTDEF *rep_note = NULL;
        if (_attrib && !error
            && fabs (_attrib->ins_pt.x
                     - (at.x + attdef_pt.x - 12.0 - base.x))
                   <= RECORD_TOLERANCE
            && fabs (_attrib->ins_pt.y - (at.y + attdef_pt.y - base.y))
                   <= RECORD_TOLERANCE)
          OK ("moved parameters: the ATTRIB is where the evaluation moved "
              "its locked ATTDEF");
        else
          {
            FAIL ("moved parameters: the ATTRIB at (%g, %g): 0x%x",
                  _attrib ? _attrib->ins_pt.x : 0.0,
                  _attrib ? _attrib->ins_pt.y : 0.0, error);
            failures++;
          }
        for (BITCODE_BL i = 0; rep && rep->entities && i < rep->num_owned;
             i++)
          {
            const Dwg_Object *e = object (dst, ref (rep->entities[i]));
            if (e && e->fixedtype == DWG_TYPE_ATTDEF
                && !strcmp (e->tio.entity->tio.ATTDEF->tag, "NOTE"))
              rep_note = e->tio.entity->tio.ATTDEF;
          }
        if (_note && rep_note && !error
            && fabs (rep_note->ins_pt.x - (note_pt.x - 12.0))
                   <= RECORD_TOLERANCE
            && fabs (_note->ins_pt.x - (at.x + note_pt.x - base.x))
                   <= RECORD_TOLERANCE
            && fabs (_note->ins_pt.y - (at.y + note_pt.y - base.y))
                   <= RECORD_TOLERANCE)
          OK ("moved parameters: the stretch moves the ATTDEF that isn't "
              "locked, and its ATTRIB stays where the definition's is");
        else
          {
            FAIL ("moved parameters: the unlocked ATTDEF at x %g in the "
                  "representation block, expected %g; its ATTRIB at (%g, "
                  "%g), expected (%g, %g): 0x%x",
                  rep_note ? rep_note->ins_pt.x : 0.0, note_pt.x - 12.0,
                  _note ? _note->ins_pt.x : 0.0,
                  _note ? _note->ins_pt.y : 0.0, at.x + note_pt.x - base.x,
                  at.y + note_pt.y - base.y, error);
            failures++;
          }
      }
      // centred and given a width of 2, its ATTRIB is aligned where the
      // evaluation moved the ATTDEF's alignment point, and starts half the
      // width at height 2.5 back from it
      attdef->horiz_alignment = 1;
      attdef->alignment_pt.x = attdef_pt.x;
      attdef->alignment_pt.y = attdef_pt.y;
      {
        const BITCODE_3BD base = block_header (dst, def_h)->base_pt;
        const double ax = at.x + attdef_pt.x - 12.0 - base.x;
        const double ay = at.y + attdef_pt.y - base.y;
        const Dwg_Object *attrib;
        const Dwg_Entity_ATTRIB *_attrib;
        Dwg_AttValue a;
        memset (&a, 0, sizeof (a));
        a.tag = (char *)"MARK";
        a.value = (char *)"MM";
        a.width = 2.0;
        loglevel = tracelevel;
        insert = dwg_dynblock_insert (model_space (dst),
                                      block_header (dst, def_h), &at, 1.0,
                                      0.0, &v, 1, &a, 1, &error);
        loglevel = test_loglevel;
        attrib = insert && insert->num_owned
                     ? object (dst, ref (insert->attribs[0]))
                     : NULL;
        _attrib = attrib && attrib->fixedtype == DWG_TYPE_ATTRIB
                      ? attrib->tio.entity->tio.ATTRIB
                      : NULL;
        if (_attrib && !error
            && fabs (_attrib->alignment_pt.x - ax) <= RECORD_TOLERANCE
            && fabs (_attrib->alignment_pt.y - ay) <= RECORD_TOLERANCE
            && fabs (_attrib->ins_pt.x - (ax - 2.5)) <= RECORD_TOLERANCE
            && fabs (_attrib->ins_pt.y - ay) <= RECORD_TOLERANCE)
          OK ("moved parameters: a centred ATTRIB with a width starts back "
              "from where the evaluation moved its alignment point");
        else
          {
            FAIL ("moved parameters: the centred ATTRIB at (%g, %g) "
                  "aligned at (%g, %g), expected (%g, %g) aligned at (%g, "
                  "%g): 0x%x",
                  _attrib ? _attrib->ins_pt.x : 0.0,
                  _attrib ? _attrib->ins_pt.y : 0.0,
                  _attrib ? _attrib->alignment_pt.x : 0.0,
                  _attrib ? _attrib->alignment_pt.y : 0.0, ax - 2.5, ay, ax,
                  ay, error);
            failures++;
          }
      }
      attdef->horiz_alignment = 0;
      attdef->alignment_pt.x = attdef->alignment_pt.y = 0.0;
      // at a scale of 1e300, the ATTDEF's own place is held, and one moved
      // 1e10 to the right isn't, with Window Width's value set made to
      // allow that value
      failures += test_accepted (dst, def_h, 1e300, NULL, 0,
                                 "an ATTDEF at its own place, at a scale of "
                                 "1e300");
      {
        Dwg_Object *width
            = named_node (dst, def_h, DWG_TYPE_BLOCKLINEARPARAMETER,
                          "winwidth");
        if (width)
          width->tio.object->tio.BLOCKLINEARPARAMETER->value_set.flags = 0;
        else
          {
            FAIL ("moved parameters: the Window's Window Width not found");
            failures++;
          }
      }
      v.value = 36.0 + 1e10;
      failures += test_refusal (dst, model_space (dst),
                                block_header (dst, def_h), &origin, 1e300,
                                0.0, &v, 1, NULL, 0,
                                REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                                "an ATTDEF moved and placed beyond a double");
      // with Mark's lock taken off too, neither ATTRIB starts where the
      // stretch moved its ATTDEF, but where the definition's does, which a
      // double holds at that scale
      {
        const Dwg_Object *mark = object (dst, attdef_h);
        const BITCODE_RS zero = 0;
        BITCODE_RS lock[2] = { 9, 9 };
        if (mark && mark->tio.entity->num_eed == 2
            && attr_eed (dst, mark, lock) == 2 && lock[1] == 1)
          {
            // Dwg_Eed_Data is packed
            memcpy (&mark->tio.entity->eed[1].data->u.eed_70.rs, &zero,
                    sizeof (zero));
            failures += test_accepted (dst, def_h, 1e300, &v, 1,
                                       "ATTDEFs that aren't locked, moved to "
                                       "where a locked one's ATTRIB is "
                                       "beyond a double");
          }
        else
          {
            FAIL ("moved parameters: Mark's AcDbAttr group not found");
            failures++;
          }
      }
    }
  else
    {
      FAIL ("moved parameters: the Window's 'Stretch Width' not found");
      failures++;
    }
  free_document (dst);
  failures += test_attdef_moved_twice (src);
  failures += test_round_trip (src);
  return failures;
}

/*--------------------------------------------------------------------------------
 * The definition's AcDbBlockRepETag counter
 */

/* Whether EED item eed starts a group of the APPID AcDbBlockRepETag. */
static int
is_etag_group (const Dwg_Data *dwg, const Dwg_Eed *eed)
{
  const Dwg_Object *appid = object (dwg, eed->handle.value);
  return appid && appid->fixedtype == DWG_TYPE_APPID
         && !strcmp (appid->tio.object->tio.APPID->name, "AcDbBlockRepETag");
}

/* What the AcDbBlockRepETag EED of an object holds: how many groups, and
   of the first, its size in bytes, its items' codes, its 1070 and its
   1071. */
typedef struct
{
  unsigned num_groups;
  BITCODE_BS size;
  unsigned num_items;
  BITCODE_RC codes[4];
  BITCODE_RS value;
  BITCODE_RL counter;
} Etag_Group;

static void
etag_group (const Dwg_Data *dwg, const BITCODE_RLL handle, Etag_Group *g)
{
  const Dwg_Object *obj = object (dwg, handle);
  const Dwg_Eed *eed = obj ? obj->tio.object->eed : NULL;
  const BITCODE_BL num_eed = obj ? obj->tio.object->num_eed : 0;
  int in_group = 0;
  memset (g, 0, sizeof (*g));
  for (BITCODE_BL i = 0; eed && i < num_eed; i++)
    {
      const Dwg_Eed_Data *data = eed[i].data;
      if (eed[i].size)
        {
          in_group = is_etag_group (dwg, &eed[i]);
          if (in_group && ++g->num_groups == 1)
            g->size = eed[i].size;
        }
      if (!in_group || g->num_groups != 1 || !data)
        continue;
      if (g->num_items < 4)
        g->codes[g->num_items] = data->code;
      g->num_items++;
      // Dwg_Eed_Data is packed
      if (data->code == 70)
        memcpy (&g->value, &data->u.eed_70.rs, sizeof (g->value));
      else if (data->code == 71)
        memcpy (&g->counter, &data->u.eed_71.rl, sizeof (g->counter));
    }
}

/* Definition def has one AcDbBlockRepETag group, as AutoCAD writes it on a
   definition: 8 bytes, 1070 1 and 1071 counter, with no 1005 handle. */
static int
check_counter (const char *what, const Dwg_Data *dwg, const BITCODE_RLL def,
               const BITCODE_RL counter)
{
  Etag_Group g;
  etag_group (dwg, def, &g);
  if (g.num_groups == 1 && g.size == 8 && g.num_items == 2 && g.codes[0] == 70
      && g.codes[1] == 71 && g.value == 1 && g.counter == counter)
    {
      OK ("%s: the definition's AcDbBlockRepETag group is 1070 1, 1071 %u",
          what, (unsigned)counter);
      return 0;
    }
  FAIL ("%s: %u AcDbBlockRepETag groups, the first of %u bytes with %u "
        "items (%u, %u), 1070 %u, 1071 %u; expected one of 8 bytes, 1070 1, "
        "1071 %u",
        what, g.num_groups, (unsigned)g.size, g.num_items,
        (unsigned)g.codes[0], (unsigned)g.codes[1], (unsigned)g.value,
        (unsigned)g.counter, (unsigned)counter);
  return 1;
}

static int
check_no_counter (const char *what, const Dwg_Data *dwg,
                  const BITCODE_RLL def)
{
  Etag_Group g;
  etag_group (dwg, def, &g);
  if (!g.num_groups)
    {
      OK ("%s: the definition has no AcDbBlockRepETag group", what);
      return 0;
    }
  FAIL ("%s: the definition has %u AcDbBlockRepETag groups, expected none",
        what, g.num_groups);
  return 1;
}

/* Removes the AcDbBlockRepETag groups from the EED eed of *num_eedp items.
   Returns how many items it removed. */
static unsigned
remove_etag_eed (const Dwg_Data *dwg, Dwg_Eed *eed, BITCODE_BL *num_eedp)
{
  BITCODE_BL kept = 0;
  unsigned removed = 0;
  int in_group = 0;
  for (BITCODE_BL i = 0; eed && i < *num_eedp; i++)
    {
      if (eed[i].size)
        in_group = is_etag_group (dwg, &eed[i]);
      if (in_group)
        {
          free (eed[i].data);
          free (eed[i].raw);
          removed++;
        }
      else
        eed[kept++] = eed[i];
    }
  *num_eedp = kept;
  return removed;
}

/* Removes definition def's AcDbBlockRepETag group, the counter, as a
   definition AutoCAD has not evaluated has none. Returns 0 when it has
   none to remove. */
static unsigned
remove_counter (Dwg_Data *dwg, const BITCODE_RLL def)
{
  Dwg_Object_Object *o = object (dwg, def)->tio.object;
  return remove_etag_eed (dwg, o->eed, &o->num_eed);
}

/* Sets the 1071 of definition def's AcDbBlockRepETag group to counter, with
   the bytes read for the group dropped, so that they agree. Returns 0 when
   it has no such 1071. */
static int
set_counter (Dwg_Data *dwg, const BITCODE_RLL def, const BITCODE_RL counter)
{
  Dwg_Object_Object *o = object (dwg, def)->tio.object;
  Dwg_Eed *group = NULL;
  for (BITCODE_BL i = 0; o->eed && i < o->num_eed; i++)
    {
      if (o->eed[i].size)
        group = is_etag_group (dwg, &o->eed[i]) ? &o->eed[i] : NULL;
      if (group && o->eed[i].data && o->eed[i].data->code == 71)
        {
          // Dwg_Eed_Data is packed
          memcpy (&o->eed[i].data->u.eed_71.rl, &counter, sizeof (counter));
          free (group->raw);
          group->raw = NULL;
          return 1;
        }
    }
  return 0;
}

/* The highest AcDbBlockRepETag index of the entities of block def. */
static BITCODE_RL
highest_index (const Dwg_Data *dwg, const BITCODE_RLL def)
{
  const Dwg_Object_BLOCK_HEADER *blk = block_header (dwg, def);
  BITCODE_RL highest = 0;
  for (BITCODE_BL i = 0; blk && blk->entities && i < blk->num_owned; i++)
    {
      const Dwg_Object *ent = object (dwg, ref (blk->entities[i]));
      BITCODE_RLL handle = 0;
      BITCODE_RL index = 0;
      if (ent && eed_tag (dwg, ent, "AcDbBlockRepETag", &handle, &index)
          && index > highest)
        highest = index;
    }
  return highest;
}

/* Adds to block def a LINE with the AcDbBlockRepETag index given. Returns
   0 when it is added. */
static int
add_indexed_line (Dwg_Data *dwg, const BITCODE_RLL def, const BITCODE_RL index)
{
  const dwg_point_3d p1 = { 0.0, 0.0, 0.0 }, p2 = { 1.0, 1.0, 0.0 };
  Dwg_Entity_LINE *line = dwg_add_LINE (block_header (dwg, def), &p1, &p2);
  loglevel = test_loglevel;
  return !line || add_etag (dwg, line->parent, index);
}

/* Places def at the origin with the n values v: at the definition's values
   (at_defaults), drawing def, or else drawing a representation block. */
static int
place_at_origin (Dwg_Data *dwg, const BITCODE_RLL def, const Dwg_DynValue *v,
                 const BITCODE_BL n, const int at_defaults,
                 const int expected_error, const char *what)
{
  const BITCODE_3BD at = { 0.0, 0.0, 0.0 };
  Dwg_Entity_INSERT *ins;
  int error = 0;
  loglevel = tracelevel;
  ins = dwg_dynblock_insert (model_space (dwg), block_header (dwg, def), &at,
                             1.0, 0.0, v, n, NULL, 0, &error);
  loglevel = test_loglevel;
  if (ins && error == expected_error
      && (ref (ins->block_header) == def) == at_defaults)
    return 0;
  FAIL ("%s: placing failed: 0x%x, expected 0x%x, drawing %s", what, error,
        expected_error,
        !ins                             ? "nothing"
        : ref (ins->block_header) == def ? "the definition"
                                         : "a representation block");
  return 1;
}

/* A placement at other values gives a definition without an
   AcDbBlockRepETag group, the counter AutoCAD keeps on a definition once
   it has evaluated it, one: 1070 1 and 1071 one more than the highest
   index of its entities, neither their count nor the last one's plus 1,
   with their indexes kept. A copy of Trees, whose entities have the
   indexes 0 to 581, has its group removed and gets two LINEs with the
   indexes 1000 and then 900. A placement at the definition's values adds
   no group; one at another visibility state adds 1071 1001, which a
   second one leaves alone, and which the document written and read back
   holds, its placements still matching their evaluation. A copy of the
   Door, whose entities have the indexes 0 to 10, with its group's 1071
   set to 50 instead of 11, keeps 50. A definition
   whose entities have no index gets no group, nor, with
   DWG_ERR_INVALIDEED, does one whose highest index is 0x7FFFFFFF, as a
   1071 of 0x80000000 is negative. */
static int
test_etag_counter (Dwg_Data *src)
{
  BITCODE_RLL def = 0;
  Dwg_Data *dst = document_with (src, TREES, &def);
  Dwg_Data readback;
  Dwg_DynValue shrub, wall;
  int failures = 0, error;

  memset (&shrub, 0, sizeof (shrub));
  shrub.name = (char *)"Type (view)";
  shrub.state = (char *)"Shrub (Plan)";
  memset (&wall, 0, sizeof (wall));
  wall.name = (char *)"Wall Thickness";
  wall.value = 6.0;

  if (!dst || !remove_counter (dst, def) || add_indexed_line (dst, def, 1000)
      || add_indexed_line (dst, def, 900))
    {
      FAIL ("counter: preparing the copy of Trees failed");
      free_document (dst);
      return 1;
    }
  if (highest_index (dst, def) != 1000)
    {
      FAIL ("counter: Trees' highest index is %u, expected 1000",
            (unsigned)highest_index (dst, def));
      free_document (dst);
      return 1;
    }
  failures += place_at_origin (dst, def, NULL, 0, 1, 0,
                               "counter at the definition's values");
  failures += check_no_counter ("counter at the definition's values", dst,
                                def);
  failures += place_at_origin (dst, def, &shrub, 1, 0, 0,
                               "counter at another state");
  failures += check_counter ("counter at another state", dst, def, 1001);
  failures += place_at_origin (dst, def, &shrub, 1, 0, 0,
                               "counter at another state again");
  failures
      += check_counter ("counter at another state again", dst, def, 1001);

  error = write_dwg (WRITTEN_COUNTER, dst);
  if (error >= DWG_ERR_CRITICAL)
    {
      FAIL ("counter: write r2004: 0x%x", error);
      failures++;
    }
  else
    {
      error = read_dwg (WRITTEN_COUNTER, &readback);
      if (error >= DWG_ERR_CRITICAL)
        {
          FAIL ("counter: read back: 0x%x", error);
          failures++;
        }
      else
        {
          Dwg_DynBlock_Check_Report *report;
          failures
              += check_counter ("counter read back", &readback, def, 1001);
          loglevel = tracelevel;
          report = dwg_dynblock_check (&readback, 1e-9);
          loglevel = test_loglevel;
          if (report && report->num_checks == 2 && report->num_match == 2)
            OK ("counter read back: the check matches both representation "
                "blocks");
          else
            {
              FAIL ("counter read back: %u checks, %u match, expected 2",
                    report ? (unsigned)report->num_checks : 0,
                    report ? (unsigned)report->num_match : 0);
              failures++;
            }
          dwg_dynblock_free_check_report (report);
        }
      free_dwg (&readback);
    }
  free_document (dst);

  dst = document_with (src, DOOR, &def);
  if (!dst || highest_index (dst, def) != 10 || !set_counter (dst, def, 50))
    {
      FAIL ("counter: preparing the copy of the Door failed");
      free_document (dst);
      return failures + 1;
    }
  failures += place_at_origin (dst, def, &wall, 1, 0, 0,
                               "counter of 50 at another Wall Thickness");
  failures += check_counter ("counter of 50 at another Wall Thickness", dst,
                             def, 50);
  free_document (dst);

  // the Door with no index on its entities
  dst = document_with (src, DOOR, &def);
  if (dst && remove_counter (dst, def))
    {
      const Dwg_Object_BLOCK_HEADER *blk = block_header (dst, def);
      for (BITCODE_BL i = 0; blk->entities && i < blk->num_owned; i++)
        {
          Dwg_Object *ent = object (dst, ref (blk->entities[i]));
          if (ent && ent->supertype == DWG_SUPERTYPE_ENTITY)
            remove_etag_eed (dst, ent->tio.entity->eed,
                             &ent->tio.entity->num_eed);
        }
      failures += place_at_origin (dst, def, &wall, 1, 0, 0,
                                   "counter with no index");
      failures += check_no_counter ("counter with no index", dst, def);
    }
  else
    {
      FAIL ("counter: preparing the copy of the Door failed");
      failures++;
    }
  free_document (dst);

  dst = document_with (src, TREES, &def);
  if (!dst || !remove_counter (dst, def)
      || add_indexed_line (dst, def, 0x7FFFFFFF))
    {
      FAIL ("counter: preparing the copy of Trees failed");
      free_document (dst);
      return failures + 1;
    }
  failures += place_at_origin (dst, def, &shrub, 1, 0, DWG_ERR_INVALIDEED,
                               "counter with no room");
  failures += check_no_counter ("counter with no room", dst, def);
  free_document (dst);
  if (!failures)
    unlink (WRITTEN_COUNTER);
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
  failures += test_add_insert_block_r11 ();
  failures += test_attribute_widths (&src);
  failures += test_placements (&src);
  failures += test_refusals (&src);
  failures += test_attributes (&src);
  failures += test_attribute_text ();
  failures += test_attributes_non_ascii (&src);
  failures += test_attr_eed_left_out (&src);
  failures += test_image_reactors (&src);
  failures += test_sortents_entries (&src);
  failures += test_sortents_order (&src);
  failures += test_without_sortents (&src);
  failures += test_entity_count (&src);
  failures += test_definition_refusals (&src);
  failures += test_minsert (&src);
  failures += test_unknown_states (&src);
  failures += test_moved_parameters (&src);
  failures += test_etag_counter (&src);
  free_dwg (&src);
  return failures ? 1 : 0;
}
