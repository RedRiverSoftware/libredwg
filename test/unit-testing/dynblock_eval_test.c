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

/* The dynamic block API of dwg_dynblock_api.h on test-data/2004/Dynblocks.dwg,
   whose placements AutoCAD 2027 set and computed: the properties of its
   definitions, the values stored on its placements, the evaluator, and the
   check of the evaluator against AutoCAD's representation blocks. The
   handles and values below are the fixture's, as AutoCAD lists them. */

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

#define FIXTURE "test/test-data/2004/Dynblocks.dwg"
#define WRITTEN "dynblock_eval_test.dwg"
#define TOLERANCE 1e-9

/* The definitions */
#define ALTEZZA 0x9931
#define TREES 0xB45B
#define TOILET 0xB8F7
#define DOOR 0xBAC4
#define DOOR_ELEVATION 0xBB0C /* constraint-based */
#define WINDOW 0xBBC1

/* The fixture reads with the errors of its constraint-based block only. */
#define KNOWN_ERRORS (DWG_ERR_UNHANDLEDCLASS | DWG_ERR_VALUEOUTOFBOUNDS)

/* A refusal of the API: DWG_ERR_INVALIDDWG with the bit for the reason. */
#define REFUSED(bit) (DWG_ERR_INVALIDDWG | (bit))

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

static void
free_dwg (Dwg_Data *dwg)
{
  loglevel = tracelevel;
  dwg_free (dwg);
  loglevel = test_loglevel;
}

static Dwg_Object_BLOCK_HEADER *
block_header (Dwg_Data *dwg, const BITCODE_RLL handle)
{
  Dwg_Object *obj = dwg_resolve_handle (dwg, handle);
  return obj && obj->fixedtype == DWG_TYPE_BLOCK_HEADER
             ? obj->tio.object->tio.BLOCK_HEADER
             : NULL;
}

static Dwg_Entity_INSERT *
insert_entity (Dwg_Data *dwg, const BITCODE_RLL handle)
{
  Dwg_Object *obj = dwg_resolve_handle (dwg, handle);
  return obj && obj->fixedtype == DWG_TYPE_INSERT
             ? obj->tio.entity->tio.INSERT
             : NULL;
}

static BITCODE_RLL
block_handle (const Dwg_Object_BLOCK_HEADER *blk)
{
  const Dwg_Data *dwg = blk->parent->dwg;
  return dwg->object[blk->parent->objid].handle.value;
}

/* The items of entity obj's EED group with the APPID named appname: the
   index of its 71 item and the value of its 1005 item. Returns 0 when it
   has no such group. */
static int
etag (const Dwg_Data *dwg, const Dwg_Object *obj, BITCODE_RL *indexp,
      BITCODE_RLL *handlep)
{
  const Dwg_Object_Entity *ent = obj->tio.entity;
  int in_group = 0, found = 0;
  for (BITCODE_BL i = 0; ent->eed && i < ent->num_eed; i++)
    {
      const Dwg_Eed_Data *data = ent->eed[i].data;
      if (ent->eed[i].size)
        {
          const Dwg_Object *appid
              = dwg_resolve_handle (dwg, ent->eed[i].handle.value);
          in_group = appid && appid->fixedtype == DWG_TYPE_APPID
                     && !strcmp (appid->tio.object->tio.APPID->name,
                                 "AcDbBlockRepETag");
        }
      if (!in_group || !data)
        continue;
      found = 1;
      // Dwg_Eed_Data is packed
      if (data->code == 71)
        memcpy (indexp, &data->u.eed_71.rl, sizeof (*indexp));
      else if (data->code == 5)
        memcpy (handlep, &data->u.eed_5.entity, sizeof (*handlep));
    }
  return found;
}

static const Dwg_DynParam *
find_param (const Dwg_DynParam *params, const BITCODE_BL count,
            const char *name)
{
  for (BITCODE_BL i = 0; i < count; i++)
    if (!strcmp (params[i].name, name))
      return &params[i];
  return NULL;
}

/* A property the definition has, as AutoCAD's Properties palette shows it,
   with the definition's value. */
typedef struct
{
  const char *name;
  Dwg_DynParam_Kind kind;
  double default_value;
  const char *default_state;
  BITCODE_BL num_states;
  BITCODE_BL value_flags;
  BITCODE_BL num_values;
} Expected_Param;

static int
test_params_of (Dwg_Data *dwg, const BITCODE_RLL def, const char *what,
                const Expected_Param *expected, const BITCODE_BL count)
{
  Dwg_DynParam *params = NULL;
  BITCODE_BL n = 0;
  int error = dwg_dynblock_get_params (block_header (dwg, def), &params, &n);
  int failures = 0;
  loglevel = test_loglevel;
  if (error || n != count)
    {
      fail ("get_params %s: 0x%x, %u properties, expected %u", what, error,
            (unsigned)n, (unsigned)count);
      dwg_dynblock_free_params (params, n);
      return 1;
    }
  for (BITCODE_BL i = 0; i < count; i++)
    {
      const Expected_Param *e = &expected[i];
      const Dwg_DynParam *p = &params[i];
      if (strcmp (p->name, e->name) || p->kind != e->kind
          || fabs (p->default_value - e->default_value) > TOLERANCE
          || (e->default_state
              && (!p->default_state
                  || strcmp (p->default_state, e->default_state)))
          || (!e->default_state && p->default_state)
          || p->num_states != e->num_states
          || p->value_flags != e->value_flags
          || p->num_values != e->num_values || !p->handle || !p->nodeid)
        {
          fail ("get_params %s [%u]: '%s' %s %.17g '%s' %u states, flags %u, "
                "%u values; expected '%s' %s %.17g '%s' %u, %u, %u",
                what, (unsigned)i, p->name, dwg_dynparam_kind_name (p->kind),
                p->default_value,
                p->default_state ? p->default_state : "",
                (unsigned)p->num_states, (unsigned)p->value_flags,
                (unsigned)p->num_values, e->name,
                dwg_dynparam_kind_name (e->kind), e->default_value,
                e->default_state ? e->default_state : "",
                (unsigned)e->num_states, (unsigned)e->value_flags,
                (unsigned)e->num_values);
          failures++;
        }
    }
  if (!failures)
    ok ("get_params %s: %u properties", what, (unsigned)count);
  dwg_dynblock_free_params (params, n);
  return failures;
}

static int
test_params (Dwg_Data *dwg)
{
  // flags: 8 a list of values, 1 minimum, 2 maximum
  static const Expected_Param window[]
      = { { "Window Width", DWG_DYNPARAM_LINEAR, 36.0, NULL, 0, 8, 7 },
          { "Window Height", DWG_DYNPARAM_LINEAR, 36.0, NULL, 0, 8, 7 } };
  static const Expected_Param door[]
      = { { "Door Size", DWG_DYNPARAM_LINEAR, 30.0, NULL, 0, 8, 5 },
          { "Wall Thickness", DWG_DYNPARAM_LINEAR, 5.0, NULL, 0, 8, 2 },
          { "Hinge", DWG_DYNPARAM_FLIP, 0.0, NULL, 2, 0, 0 },
          { "Swing", DWG_DYNPARAM_FLIP, 0.0, NULL, 2, 0, 0 },
          // U+00BA, the masculine ordinal
          { "Opening Angle", DWG_DYNPARAM_VISIBILITY, 0.0,
            "Open 30\xc2\xba", 5, 0, 0 } };
  static const Expected_Param toilet[]
      = { { "Angle", DWG_DYNPARAM_ROTATION, 0.0, NULL, 0, 3, 0 },
          { "Flip view", DWG_DYNPARAM_FLIP, 0.0, NULL, 2, 0, 0 },
          { "Type (view)", DWG_DYNPARAM_VISIBILITY, 0.0, "Elongated (Front)",
            9, 0, 0 } };
  static const Expected_Param trees[] = { { "Type (view)",
                                            DWG_DYNPARAM_VISIBILITY, 0.0,
                                            "Palm (Plan)", 12, 0, 0 } };
  static const Expected_Param altezza[] = { { "Type (view)",
                                              DWG_DYNPARAM_VISIBILITY, 0.0,
                                              "Honda [Top]", 22, 0, 0 } };
  static const Expected_Param door_elevation[]
      = { { "Flip Door", DWG_DYNPARAM_FLIP, 0.0, NULL, 2, 0, 0 } };
  Dwg_DynParam *params = NULL;
  BITCODE_BL n = 0;
  int error, failures = 0;

  failures += test_params_of (dwg, WINDOW, "Window", window, 2);
  failures += test_params_of (dwg, DOOR, "Door", door, 5);
  failures += test_params_of (dwg, TOILET, "Toilet", toilet, 3);
  failures += test_params_of (dwg, TREES, "Trees", trees, 1);
  failures += test_params_of (dwg, ALTEZZA, "alteZZa", altezza, 1);
  failures += test_params_of (dwg, DOOR_ELEVATION, "Door Elevation",
                              door_elevation, 1);

  // the value set, the states, and the flip labels
  error = dwg_dynblock_get_params (block_header (dwg, DOOR), &params, &n);
  loglevel = test_loglevel;
  if (!error)
    {
      const Dwg_DynParam *size = find_param (params, n, "Door Size");
      const Dwg_DynParam *hinge = find_param (params, n, "Hinge");
      const Dwg_DynParam *angle = find_param (params, n, "Opening Angle");
      if (size && size->values && size->values[0] == 24.0
          && size->values[4] == 40.0 && hinge && hinge->states
          && !strcmp (hinge->states[0], "Left")
          && !strcmp (hinge->states[1], "Right") && angle && angle->states
          && !strcmp (angle->states[4], "Closed"))
        ok ("get_params Door: values, flip labels and states");
      else
        {
          fail ("get_params Door: values, flip labels or states");
          failures++;
        }
    }
  dwg_dynblock_free_params (params, n);
  params = NULL;
  n = 0;

  // a block that isn't a dynamic block definition
  error = dwg_dynblock_get_params (
      block_header (dwg, dwg_model_space_ref (dwg)->absolute_ref), &params,
      &n);
  loglevel = test_loglevel;
  if (error == REFUSED (DWG_ERR_INVALIDTYPE) && !params && !n)
    ok ("get_params model space: refused, 0x%x", error);
  else
    {
      fail ("get_params model space: 0x%x", error);
      failures++;
    }
  dwg_dynblock_free_params (params, n);
  return failures;
}

/* A value AutoCAD lists for a placement. */
typedef struct
{
  const char *name;
  double value;
  const char *state;
} Expected_Value;

static int
test_values_of (Dwg_Data *dwg, const BITCODE_RLL insert, const BITCODE_RLL def,
                const Expected_Value *expected, const BITCODE_BL count)
{
  Dwg_Object_BLOCK_HEADER *defp = NULL;
  Dwg_DynValue *values = NULL;
  BITCODE_BL n = 0;
  int error = dwg_dynblock_get_values (insert_entity (dwg, insert), &defp,
                                       &values, &n);
  int failures = 0;
  loglevel = test_loglevel;
  if (error || n != count || !defp || block_handle (defp) != def)
    {
      fail ("get_values " FORMAT_HV ": 0x%x, %u values, definition " FORMAT_HV,
            insert, error, (unsigned)n, defp ? block_handle (defp) : 0);
      dwg_dynblock_free_values (values, n);
      return 1;
    }
  for (BITCODE_BL i = 0; i < count; i++)
    {
      const Expected_Value *e = &expected[i];
      const Dwg_DynValue *v = &values[i];
      // AutoCAD reads the angles set back to within 1e-15
      if (strcmp (v->name, e->name)
          || fabs (v->value - e->value) > TOLERANCE
          || (e->state && (!v->state || strcmp (v->state, e->state)))
          || (!e->state && v->state))
        {
          fail ("get_values " FORMAT_HV " [%u]: '%s' %.17g '%s', expected "
                "'%s' %.17g '%s'",
                insert, (unsigned)i, v->name, v->value,
                v->state ? v->state : "", e->name, e->value,
                e->state ? e->state : "");
          failures++;
        }
    }
  if (!failures)
    ok ("get_values " FORMAT_HV ": %u values", insert, (unsigned)count);
  dwg_dynblock_free_values (values, n);
  return failures;
}

static int
test_values (Dwg_Data *dwg)
{
  // W0: at the defaults, drawing the definition itself
  static const Expected_Value w0[]
      = { { "Window Width", 36.0, NULL }, { "Window Height", 36.0, NULL } };
  // W4: Window Width 24, then Window Height 54
  static const Expected_Value w4[]
      = { { "Window Width", 24.0, NULL }, { "Window Height", 54.0, NULL } };
  // D4: Opening Angle Open 90º, then Wall Thickness 6
  static const Expected_Value d4[]
      = { { "Door Size", 30.0, NULL },
          { "Wall Thickness", 6.0, NULL },
          { "Hinge", 0.0, NULL },
          { "Swing", 0.0, NULL },
          { "Opening Angle", 0.0, "Open 90\xc2\xba" } };
  // T4: Type (view) Pullchain (Plan), then Angle 225°
  static const Expected_Value t4[]
      = { { "Angle", 3.9269908169872414, NULL },
          { "Flip view", 0.0, NULL },
          { "Type (view)", 0.0, "Pullchain (Plan)" } };
  // R2: Type (view) Type 1 Evergreen (Elevation)
  static const Expected_Value r2[]
      = { { "Type (view)", 0.0, "Type 1 Evergreen (Elevation)" } };
  Dwg_Object_BLOCK_HEADER *defp = NULL;
  Dwg_DynValue *values = NULL;
  BITCODE_BL n = 0;
  int error, failures = 0;

  failures += test_values_of (dwg, 0xC8CF, WINDOW, w0, 2);
  failures += test_values_of (dwg, 0xC8D3, WINDOW, w4, 2);
  failures += test_values_of (dwg, 0xC8D8, DOOR, d4, 5);
  failures += test_values_of (dwg, 0xC8DD, TOILET, t4, 3);
  failures += test_values_of (dwg, 0xC8E1, TREES, r2, 1);

  // Door Elevation's only property is a flip, which is read; its being
  // constraint-based is the evaluator's to refuse
  error = dwg_dynblock_get_values (insert_entity (dwg, 0xC174), &defp,
                                   &values, &n);
  loglevel = test_loglevel;
  if (!error && n == 1 && defp && block_handle (defp) == DOOR_ELEVATION)
    ok ("get_values C174: Door Elevation's flip");
  else
    {
      fail ("get_values C174: 0x%x, %u values", error, (unsigned)n);
      failures++;
    }
  dwg_dynblock_free_values (values, n);
  return failures;
}

/* Calls dwg_dynblock_evaluate () and checks that it refuses with
   expected_error, and adds nothing to dwg. */
static int
test_refusal (Dwg_Data *dwg, const BITCODE_RLL def, const Dwg_DynValue *values,
              const BITCODE_BL n, const int expected_error, const char *what)
{
  const BITCODE_BL num_objects = dwg->num_objects;
  int error = 0;
  Dwg_Object_BLOCK_HEADER *rep
      = dwg_dynblock_evaluate (block_header (dwg, def), values, n, &error);
  loglevel = test_loglevel;
  if (!rep && error == expected_error && dwg->num_objects == num_objects)
    {
      ok ("evaluate refuses %s: 0x%x", what, error);
      return 0;
    }
  fail ("evaluate %s: %s, 0x%x, expected 0x%x, %u objects added", what,
        rep ? "a block" : "NULL", error, expected_error,
        (unsigned)(dwg->num_objects - num_objects));
  return 1;
}

static int
test_refusals (Dwg_Data *dwg)
{
  Dwg_DynValue v[2];
  int failures = 0;
  memset (v, 0, sizeof (v));

  v[0].name = (char *)"Window Width";
  v[0].value = 12.0;
  failures += test_refusal (dwg, WINDOW, v, 1,
                            REFUSED (DWG_ERR_NOTYETSUPPORTED),
                            "a changed linear value");
  v[0].name = (char *)"Angle";
  v[0].value = M_PI / 6;
  failures += test_refusal (dwg, TOILET, v, 1,
                            REFUSED (DWG_ERR_NOTYETSUPPORTED),
                            "a changed rotation value");
  v[0].name = (char *)"Flip view";
  v[0].value = 1.0;
  failures += test_refusal (dwg, TOILET, v, 1,
                            REFUSED (DWG_ERR_NOTYETSUPPORTED), "a flip");
  v[0].value = 0.5;
  failures += test_refusal (dwg, TOILET, v, 1,
                            REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "a flip value of 0.5");
  failures += test_refusal (dwg, DOOR_ELEVATION, NULL, 0,
                            REFUSED (DWG_ERR_NOTYETSUPPORTED),
                            "a constraint-based block");
  v[0].name = (char *)"Window Width";
  v[0].value = NAN;
  failures += test_refusal (dwg, WINDOW, v, 1,
                            REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "a value that isn't finite");
  v[0].name = (char *)"Width";
  v[0].value = 36.0;
  failures += test_refusal (dwg, WINDOW, v, 1,
                            REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "an unknown property");
  v[0].name = (char *)"Type (view)";
  v[0].state = (char *)"Oak (Plan)";
  failures += test_refusal (dwg, TREES, v, 1,
                            REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "an unknown state");
  v[0].state = (char *)"Shrub (Plan)";
  v[1] = v[0];
  failures += test_refusal (dwg, TREES, v, 2,
                            REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "a property given twice");
  failures += test_refusal (dwg, dwg_model_space_ref (dwg)->absolute_ref,
                            NULL, 0, REFUSED (DWG_ERR_INVALIDTYPE),
                            "a block that isn't dynamic");
  // the copy refuses an entity listed twice, before the block is made
  {
    Dwg_Object_BLOCK_HEADER *window = block_header (dwg, WINDOW);
    BITCODE_H second = window->entities[1];
    window->entities[1] = window->entities[0];
    failures += test_refusal (dwg, WINDOW, NULL, 0,
                              REFUSED (DWG_ERR_INVALIDHANDLE),
                              "an entity the definition lists twice");
    // and an entity that doesn't resolve
    window->entities[1] = dwg_add_handleref (dwg, 3, 0xFFFFFF, NULL);
    failures += test_refusal (dwg, WINDOW, NULL, 0,
                              REFUSED (DWG_ERR_INVALIDHANDLE),
                              "an entity that doesn't resolve");
    window->entities[1] = second;
  }
  // an INSERT of a definition owning an object that isn't an entity
  {
    const Dwg_Object_BLOCK_HEADER *altezza = block_header (dwg, ALTEZZA);
    Dwg_Entity_INSERT *insert = NULL;
    for (BITCODE_BL i = 0; !insert && i < altezza->num_owned; i++)
      {
        Dwg_Object *obj = dwg_ref_object (dwg, altezza->entities[i]);
        if (obj && obj->fixedtype == DWG_TYPE_INSERT)
          insert = obj->tio.entity->tio.INSERT;
      }
    if (insert)
      {
        BITCODE_H *attribs = insert->attribs;
        const BITCODE_BL num_owned = insert->num_owned;
        BITCODE_H owned[1];
        // alteZZa's extension dictionary
        owned[0] = altezza->parent->xdicobjhandle;
        insert->attribs = owned;
        insert->num_owned = 1;
        failures += test_refusal (dwg, ALTEZZA, NULL, 0,
                                  REFUSED (DWG_ERR_INVALIDHANDLE),
                                  "an owned object that isn't an entity");
        insert->attribs = attribs;
        insert->num_owned = num_owned;
      }
    else
      {
        fail ("alteZZa has no INSERT");
        failures++;
      }
  }
  return failures;
}

/* The evaluated block's structure: def's entities, in its order, each with
   the source's AcDbBlockRepETag index and a 1005 handle naming itself,
   owned by the block, with no extension dictionary, and the invisible
   flags of the state. */
static int
check_evaluated (Dwg_Data *dwg, const BITCODE_RLL def_handle,
                 const BITCODE_RLL rep_handle, const BITCODE_BL expected_shown,
                 const char *what)
{
  const Dwg_Object_BLOCK_HEADER *def = block_header (dwg, def_handle);
  const Dwg_Object_BLOCK_HEADER *rep = block_header (dwg, rep_handle);
  BITCODE_BL shown = 0;
  if (!rep || !rep->anonymous || !rep->name || strcmp (rep->name, "*U")
      || rep->num_owned != def->num_owned || !rep->block_entity
      || !rep->endblk_entity
      || memcmp (&rep->base_pt, &def->base_pt, sizeof (rep->base_pt)))
    {
      fail ("%s: the block " FORMAT_HV " is not an anonymous block like the "
            "definition",
            what, rep_handle);
      return 1;
    }
  for (BITCODE_BL i = 0; i < def->num_owned; i++)
    {
      const Dwg_Object *src = dwg_ref_object (dwg, def->entities[i]);
      const Dwg_Object *copy = dwg_ref_object (dwg, rep->entities[i]);
      BITCODE_RL src_index = 0, copy_index = 0;
      BITCODE_RLL src_self = 0, copy_self = 0;
      if (!src || !copy || copy->fixedtype != src->fixedtype
          || !etag (dwg, src, &src_index, &src_self)
          || !etag (dwg, copy, &copy_index, &copy_self)
          || copy_index != src_index || copy_self != copy->handle.value
          || !copy->tio.entity->ownerhandle
          || copy->tio.entity->ownerhandle->absolute_ref != rep_handle
          || !copy->tio.entity->is_xdic_missing
          || copy->tio.entity->xdicobjhandle)
        {
          fail ("%s: entity %u " FORMAT_HV " is not a copy of " FORMAT_HV
                " owned by the block with its AcDbBlockRepETag",
                what, (unsigned)i, copy ? copy->handle.value : 0,
                src ? src->handle.value : 0);
          return 1;
        }
      if (!copy->tio.entity->invisible)
        shown++;
    }
  loglevel = test_loglevel;
  if (shown != expected_shown)
    {
      fail ("%s: %u entities shown, expected %u", what, (unsigned)shown,
            (unsigned)expected_shown);
      return 1;
    }
  ok ("%s: %u entities, %u shown", what, (unsigned)def->num_owned,
      (unsigned)shown);
  return 0;
}

static int
test_evaluate (Dwg_Data *dwg)
{
  Dwg_DynValue v[2];
  Dwg_Object_BLOCK_HEADER *rep;
  BITCODE_RLL shrub = 0, door_closed = 0;
  Dwg_Data readback;
  BITCODE_BL num_objects, num_entities;
  int error = 0, failures = 0;
  memset (v, 0, sizeof (v));

  // R1: Trees at Shrub (Plan), which shows 52 of its 582 entities
  v[0].name = (char *)"Type (view)";
  v[0].state = (char *)"Shrub (Plan)";
  num_objects = dwg->num_objects;
  num_entities = dwg->num_entities;
  rep = dwg_dynblock_evaluate (block_header (dwg, TREES), v, 1, &error);
  loglevel = test_loglevel;
  // the block header, its BLOCK and ENDBLK, and the 582 copies
  if (rep && dwg->num_objects - num_objects == 585
      && dwg->num_entities - num_entities == 584)
    ok ("evaluate Trees adds 585 objects, 584 of them entities");
  else
    {
      fail ("evaluate Trees adds %u objects, %u entities",
            (unsigned)(dwg->num_objects - num_objects),
            (unsigned)(dwg->num_entities - num_entities));
      failures++;
    }
  if (rep && !error)
    {
      shrub = block_handle (rep);
      failures += check_evaluated (dwg, TREES, shrub, 52,
                                   "evaluate Trees at Shrub (Plan)");
    }
  else
    {
      fail ("evaluate Trees at Shrub (Plan): 0x%x", error);
      failures++;
    }

  // D3: Door at Closed, which shows 3 of its 11, with its linear and flip
  // properties given at the definition's values
  v[0].name = (char *)"Opening Angle";
  v[0].state = (char *)"Closed";
  v[1].name = (char *)"Wall Thickness";
  v[1].value = 5.0;
  rep = dwg_dynblock_evaluate (block_header (dwg, DOOR), v, 2, &error);
  loglevel = test_loglevel;
  if (rep && !error)
    {
      door_closed = block_handle (rep);
      failures += check_evaluated (dwg, DOOR, door_closed, 3,
                                   "evaluate Door at Closed");
    }
  else
    {
      fail ("evaluate Door at Closed: 0x%x", error);
      failures++;
    }

  // at the defaults: a copy of the definition, showing its first state
  rep = dwg_dynblock_evaluate (block_header (dwg, TOILET), NULL, 0, &error);
  loglevel = test_loglevel;
  if (rep && !error)
    failures += check_evaluated (dwg, TOILET, block_handle (rep), 22,
                                 "evaluate Toilet at the defaults");
  else
    {
      fail ("evaluate Toilet at the defaults: 0x%x", error);
      failures++;
    }
  if (!shrub || !door_closed)
    return failures + 1;

  // Written as r2004 and read back, the blocks keep their structure.
  unlink (WRITTEN);
  loglevel = tracelevel;
  error = dwg_write_file (WRITTEN, dwg);
  loglevel = test_loglevel;
  if (error >= DWG_ERR_CRITICAL)
    {
      fail ("write r2004: 0x%x", error);
      return failures + 1;
    }
  error = read_dwg (WRITTEN, &readback);
  if (error & ~KNOWN_ERRORS)
    {
      fail ("read back: 0x%x", error);
      failures++;
    }
  else
    {
      ok ("write r2004 and read back: 0x%x", error);
      failures += check_evaluated (&readback, TREES, shrub, 52,
                                   "read back Trees at Shrub (Plan)");
      failures += check_evaluated (&readback, DOOR, door_closed, 3,
                                   "read back Door at Closed");
    }
  free_dwg (&readback);
  unlink (WRITTEN);
  return failures;
}

/* What the check reports for each placement of the fixture. AutoCAD
   computed the visibility-only placements and those at the defaults,
   which the evaluator matches; the others change kinds it refuses, and
   Door Elevation is constraint-based. */
typedef struct
{
  BITCODE_RLL insert;
  BITCODE_RLL block;
  Dwg_DynBlock_Check_Status status;
  BITCODE_BL kinds;
} Expected_Check;

#define KIND(k) (1U << DWG_DYNPARAM_##k)

static const Expected_Check expected_checks[] = {
  { 0xBFD0, 0x9D5E, DWG_DYNBLOCK_CHECK_MATCH, KIND (VISIBILITY) },
  { 0xBFD8, 0xAC8C, DWG_DYNBLOCK_CHECK_MATCH, KIND (VISIBILITY) },
  { 0xC145, 0xB6AC, DWG_DYNBLOCK_CHECK_MATCH, 0 },
  { 0xC14C, 0xB9E8, DWG_DYNBLOCK_CHECK_MATCH, KIND (VISIBILITY) },
  { 0xC15A, 0xBAFC, DWG_DYNBLOCK_CHECK_UNSUPPORTED, KIND (LINEAR) },
  { 0xC174, 0xBB72, DWG_DYNBLOCK_CHECK_UNSUPPORTED, 0 },
  { 0xC17F, 0xBBDC, DWG_DYNBLOCK_CHECK_MATCH, 0 },
  { 0xC8D0, 0xC96E, DWG_DYNBLOCK_CHECK_UNSUPPORTED, KIND (LINEAR) },
  { 0xC8D1, 0xC98C, DWG_DYNBLOCK_CHECK_UNSUPPORTED, KIND (LINEAR) },
  { 0xC8D2, 0xC9AA, DWG_DYNBLOCK_CHECK_UNSUPPORTED, KIND (LINEAR) },
  { 0xC8D3, 0xC9C8, DWG_DYNBLOCK_CHECK_UNSUPPORTED, KIND (LINEAR) },
  { 0xC8D5, 0xC9F2, DWG_DYNBLOCK_CHECK_UNSUPPORTED, KIND (LINEAR) },
  { 0xC8D6, 0xCA2F, DWG_DYNBLOCK_CHECK_UNSUPPORTED, KIND (LINEAR) },
  { 0xC8D7, 0xCA6C, DWG_DYNBLOCK_CHECK_MATCH, KIND (VISIBILITY) },
  { 0xC8D8, 0xCAA9, DWG_DYNBLOCK_CHECK_UNSUPPORTED,
    KIND (LINEAR) | KIND (VISIBILITY) },
  { 0xC8DA, 0xCB0F, DWG_DYNBLOCK_CHECK_MATCH, KIND (VISIBILITY) },
  { 0xC8DB, 0xCC00, DWG_DYNBLOCK_CHECK_UNSUPPORTED,
    KIND (ROTATION) | KIND (VISIBILITY) },
  { 0xC8DC, 0xCD02, DWG_DYNBLOCK_CHECK_UNSUPPORTED,
    KIND (ROTATION) | KIND (VISIBILITY) },
  { 0xC8DD, 0xCE04, DWG_DYNBLOCK_CHECK_UNSUPPORTED,
    KIND (ROTATION) | KIND (VISIBILITY) },
  { 0xC8DE, 0xCF06, DWG_DYNBLOCK_CHECK_UNSUPPORTED, KIND (ROTATION) },
  { 0xC8E0, 0xCFF7, DWG_DYNBLOCK_CHECK_MATCH, KIND (VISIBILITY) },
  { 0xC8E1, 0xD24A, DWG_DYNBLOCK_CHECK_MATCH, KIND (VISIBILITY) },
  { 0xC8E2, 0xD49D, DWG_DYNBLOCK_CHECK_MATCH, KIND (VISIBILITY) },
};

static const Dwg_DynBlock_Check *
find_check (const Dwg_DynBlock_Check_Report *report, const BITCODE_RLL insert,
            const BITCODE_RLL block)
{
  for (BITCODE_BL i = 0; i < report->num_checks; i++)
    if (report->checks[i].insert == insert
        && report->checks[i].block == block)
      return &report->checks[i];
  return NULL;
}

static int
test_check (Dwg_Data *dwg)
{
  const size_t n = sizeof (expected_checks) / sizeof (expected_checks[0]);
  Dwg_DynBlock_Check_Report *report = dwg_dynblock_check (dwg, TOLERANCE);
  int failures = 0;
  loglevel = test_loglevel;
  if (!report)
    {
      fail ("check: no report");
      return 1;
    }
  if (report->num_checks != n || report->num_match != 10
      || report->num_unsupported != 13 || report->num_mismatch
      || report->num_no_values || report->num_failed)
    {
      fail ("check: %u checks: %u match, %u mismatch, %u unsupported, %u no "
            "values, %u failed; expected %u: 10 match, 13 unsupported",
            (unsigned)report->num_checks, (unsigned)report->num_match,
            (unsigned)report->num_mismatch, (unsigned)report->num_unsupported,
            (unsigned)report->num_no_values, (unsigned)report->num_failed,
            (unsigned)n);
      failures++;
    }
  for (size_t i = 0; i < n; i++)
    {
      const Expected_Check *e = &expected_checks[i];
      const Dwg_DynBlock_Check *c = find_check (report, e->insert, e->block);
      if (!c || c->status != e->status || c->kinds != e->kinds
          || (e->status == DWG_DYNBLOCK_CHECK_MATCH
              && (!c->num_entities || c->num_differing || c->num_equivalent
                  || c->reason || !c->evaluated))
          || (e->status != DWG_DYNBLOCK_CHECK_MATCH && !c->reason))
        {
          fail ("check " FORMAT_HV ": status %d, kinds 0x%x, %u entities, %u "
                "differ: %s; expected status %d, kinds 0x%x",
                e->insert, c ? (int)c->status : -1, c ? (unsigned)c->kinds : 0,
                c ? (unsigned)c->num_entities : 0,
                c ? (unsigned)c->num_differing : 0,
                c && c->reason ? c->reason : "", (int)e->status,
                (unsigned)e->kinds);
          failures++;
        }
      else
        ok ("check " FORMAT_HV ": %s%s%s", e->insert,
            e->status == DWG_DYNBLOCK_CHECK_MATCH ? "match" : "unsupported",
            c->reason ? ": " : "", c->reason ? c->reason : "");
    }
  dwg_dynblock_free_check_report (report);
  return failures;
}

/* The entity of representation block blk with the given AcDbBlockRepETag
   index. */
static Dwg_Object *
rep_entity (Dwg_Data *dwg, const BITCODE_RLL blk_handle, const BITCODE_RL idx)
{
  const Dwg_Object_BLOCK_HEADER *blk = block_header (dwg, blk_handle);
  for (BITCODE_BL i = 0; blk && i < blk->num_owned; i++)
    {
      Dwg_Object *obj = dwg_ref_object (dwg, blk->entities[i]);
      BITCODE_RL index = 0;
      BITCODE_RLL self = 0;
      if (obj && etag (dwg, obj, &index, &self) && index == idx)
        return obj;
    }
  return NULL;
}

/* The first entity of the given type in representation block blk. */
static Dwg_Object *
first_of_type (Dwg_Data *dwg, const BITCODE_RLL blk_handle,
               const Dwg_Object_Type type)
{
  const Dwg_Object_BLOCK_HEADER *blk = block_header (dwg, blk_handle);
  for (BITCODE_BL i = 0; blk && i < blk->num_owned; i++)
    {
      Dwg_Object *obj = dwg_ref_object (dwg, blk->entities[i]);
      if (obj && obj->fixedtype == type)
        return obj;
    }
  return NULL;
}

/* The check finds what differs from AutoCAD's blocks, here AutoCAD's blocks
   changed in memory, and reports what has no values to evaluate. */
static int
test_check_finds (Dwg_Data *dwg)
{
  Dwg_Object *line = first_of_type (dwg, 0xCFF7, DWG_TYPE_LINE);
  Dwg_Object *hidden = rep_entity (dwg, 0xCB0F, 0);
  Dwg_Object *ellipse = first_of_type (dwg, 0x9D5E, DWG_TYPE_ELLIPSE);
  Dwg_Entity_INSERT *r2 = insert_entity (dwg, 0xC8E1);
  Dwg_Entity_INSERT *r3 = insert_entity (dwg, 0xC8E2);
  Dwg_DynBlock_Check_Report *report;
  const Dwg_DynBlock_Check *c;
  int failures = 0;
  if (!line || !hidden || !ellipse || !r2 || !r3)
    {
      fail ("check finds: fixture entities not found");
      return 1;
    }
  // R1's block: a LINE moved by 0.5
  line->tio.entity->tio.LINE->start.x += 0.5;
  // T1's block: entity 0's visibility flag
  hidden->tio.entity->invisible = !hidden->tio.entity->invisible;
  // BFD0's block: an ELLIPSE with its major axis reversed, the same curve
  {
    Dwg_Entity_ELLIPSE *_e = ellipse->tio.entity->tio.ELLIPSE;
    _e->sm_axis.x = -_e->sm_axis.x;
    _e->sm_axis.y = -_e->sm_axis.y;
    _e->sm_axis.z = -_e->sm_axis.z;
    _e->start_angle += M_PI;
    _e->end_angle += M_PI;
  }
  // C17F's block: an entity reference that doesn't resolve
  block_header (dwg, 0xBBDC)->entities[0]
      = dwg_add_handleref (dwg, 3, 0xFFFFFF, NULL);
  // BFD8's block: its base point
  block_header (dwg, 0xAC8C)->base_pt.x += 1.0;
  // C145's block: entity 0's transparency
  {
    Dwg_Object *first = rep_entity (dwg, 0xB6AC, 0);
    if (!first)
      {
        fail ("check finds: fixture entity not found");
        return 1;
      }
    first->tio.entity->color.alpha_type = 3;
    first->tio.entity->color.alpha = 0x80;
  }
  // R2 without its values, and R3 drawing the definition, which leaves its
  // block unplaced
  r2->parent->is_xdic_missing = 1;
  r3->block_header = dwg_add_handleref (dwg, 5, TREES, NULL);

  report = dwg_dynblock_check (dwg, TOLERANCE);
  loglevel = test_loglevel;
  if (!report)
    {
      fail ("check finds: no report");
      return 1;
    }
  c = find_check (report, 0xC8E0, 0xCFF7);
  if (c && c->status == DWG_DYNBLOCK_CHECK_MISMATCH && c->num_differing == 1
      && c->reason && strstr (c->reason, "LINE") && strstr (c->reason, "start"))
    ok ("check finds a moved LINE: %s", c->reason);
  else
    {
      fail ("check of a moved LINE: status %d, %s", c ? (int)c->status : -1,
            c && c->reason ? c->reason : "");
      failures++;
    }
  c = find_check (report, 0xC8DA, 0xCB0F);
  if (c && c->status == DWG_DYNBLOCK_CHECK_MISMATCH && c->num_differing == 1
      && c->reason && strstr (c->reason, "invisible"))
    ok ("check finds a visibility flag: %s", c->reason);
  else
    {
      fail ("check of a visibility flag: status %d, %s",
            c ? (int)c->status : -1, c && c->reason ? c->reason : "");
      failures++;
    }
  c = find_check (report, 0xC17F, 0xBBDC);
  if (c && c->status == DWG_DYNBLOCK_CHECK_MISMATCH && c->reason
      && strstr (c->reason, "doesn't resolve"))
    ok ("check finds an entity that doesn't resolve: %s", c->reason);
  else
    {
      fail ("check of an entity that doesn't resolve: status %d, %s",
            c ? (int)c->status : -1, c && c->reason ? c->reason : "");
      failures++;
    }
  c = find_check (report, 0xBFD8, 0xAC8C);
  if (c && c->status == DWG_DYNBLOCK_CHECK_MISMATCH && !c->num_differing
      && c->reason && strstr (c->reason, "base_pt"))
    ok ("check finds a moved base point: %s", c->reason);
  else
    {
      fail ("check of a moved base point: status %d, %u differ, %s",
            c ? (int)c->status : -1, c ? (unsigned)c->num_differing : 0,
            c && c->reason ? c->reason : "");
      failures++;
    }
  c = find_check (report, 0xC145, 0xB6AC);
  if (c && c->status == DWG_DYNBLOCK_CHECK_MISMATCH && c->num_differing == 1
      && c->reason && strstr (c->reason, "color"))
    ok ("check finds a transparency: %s", c->reason);
  else
    {
      fail ("check of a transparency: status %d, %s", c ? (int)c->status : -1,
            c && c->reason ? c->reason : "");
      failures++;
    }
  c = find_check (report, 0xBFD0, 0x9D5E);
  if (c && c->status == DWG_DYNBLOCK_CHECK_MATCH && c->num_equivalent == 1)
    ok ("check matches a reversed ELLIPSE as the same curve");
  else
    {
      fail ("check of a reversed ELLIPSE: status %d, %u equivalent, %s",
            c ? (int)c->status : -1, c ? (unsigned)c->num_equivalent : 0,
            c && c->reason ? c->reason : "");
      failures++;
    }
  c = find_check (report, 0xC8E1, 0xD24A);
  if (c && c->status == DWG_DYNBLOCK_CHECK_NO_VALUES)
    ok ("check: no values on an INSERT without AcDbBlockRepresentation");
  else
    {
      fail ("check of an INSERT without values: status %d",
            c ? (int)c->status : -1);
      failures++;
    }
  c = find_check (report, 0, 0xD49D);
  if (c && c->status == DWG_DYNBLOCK_CHECK_NO_VALUES
      && c->definition == TREES && !find_check (report, 0xC8E2, 0xD49D))
    ok ("check: no values for a block no INSERT draws");
  else
    {
      fail ("check of an unplaced block: status %d", c ? (int)c->status : -1);
      failures++;
    }
  if (report->num_match == 3 && report->num_mismatch == 5
      && report->num_no_values == 2 && report->num_unsupported == 13)
    ok ("check finds: 3 match, 5 mismatch, 2 no values, 13 unsupported");
  else
    {
      fail ("check finds: %u match, %u mismatch, %u no values, %u "
            "unsupported",
            (unsigned)report->num_match, (unsigned)report->num_mismatch,
            (unsigned)report->num_no_values,
            (unsigned)report->num_unsupported);
      failures++;
    }
  dwg_dynblock_free_check_report (report);
  return failures;
}

int
main (int argc, char *argv[])
{
  Dwg_Data dwg;
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
  if (strcmp (dwg_dynparam_kind_name (DWG_DYNPARAM_VISIBILITY), "visibility")
      || strcmp (dwg_dynparam_kind_name ((Dwg_DynParam_Kind)0), "unknown"))
    {
      fail ("dwg_dynparam_kind_name");
      failures++;
    }

  error = read_dwg (fixture, &dwg);
  if (error & ~KNOWN_ERRORS)
    {
      fail ("dwg_read_file 0x%x", error);
      return 1;
    }
  failures += test_params (&dwg);
  failures += test_values (&dwg);
  failures += test_refusals (&dwg);
  failures += test_evaluate (&dwg);
  free_dwg (&dwg);

  // each check adds its evaluations to the document: a fresh one each
  read_dwg (fixture, &dwg);
  failures += test_check (&dwg);
  free_dwg (&dwg);
  read_dwg (fixture, &dwg);
  failures += test_check_finds (&dwg);
  free_dwg (&dwg);
  return failures ? 1 : 0;
}
