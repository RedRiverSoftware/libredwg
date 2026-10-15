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
   handles and values below are the fixture's, as AutoCAD lists them.

   The fixture's Window has three linear properties' stretch actions,
   'Stretch Width', 'Stretch Divider' (at a distance multiplier of 0.5) and
   'Stretch Height'; its Door has stretch actions at angle offsets of 0 to
   90 degrees, a scale action and two move actions. Further cases are made
   by adding entities to those actions, or changing them, in memory. */

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
#include "classes.h"
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

/* The errors the fixture reads with, those of its constraint-based
   block. */
#define KNOWN_ERRORS (DWG_ERR_UNHANDLEDCLASS | DWG_ERR_VALUEOUTOFBOUNDS)

/* A refusal of the API: DWG_ERR_INVALIDDWG with the bit for the reason. */
#define REFUSED(bit) (DWG_ERR_INVALIDDWG | (bit))

/* ok () at the test's log level, which the library's calls set to the
   document's. */
#define OK(...)                                                               \
  do                                                                          \
    {                                                                         \
      loglevel = test_loglevel;                                               \
      ok (__VA_ARGS__);                                                       \
    }                                                                         \
  while (0)

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

/* Reads the fixture into dwg. Returns 0, or 1 when it doesn't read with
   exactly its known errors. */
static int
read_fixture (Dwg_Data *dwg)
{
  const int error = read_dwg (fixture, dwg);
  if (error != KNOWN_ERRORS)
    {
      fail ("dwg_read_file %s: 0x%x, expected 0x%x", fixture, error,
            KNOWN_ERRORS);
      return 1;
    }
  return 0;
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

/* The handle of model space's block, or 0. */
static BITCODE_RLL
model_space (Dwg_Data *dwg)
{
  const Dwg_Object_Ref *ref = dwg_model_space_ref (dwg);
  return ref ? ref->absolute_ref : 0;
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

/* The entry of DICTIONARY dict named name, or NULL. */
static Dwg_Object *
dict_entry (Dwg_Data *dwg, const Dwg_Object *dict, const char *name)
{
  const Dwg_Object_DICTIONARY *_dict;
  if (!dict || dict->fixedtype != DWG_TYPE_DICTIONARY)
    return NULL;
  _dict = dict->tio.object->tio.DICTIONARY;
  for (BITCODE_BL i = 0;
       _dict->texts && _dict->itemhandles && i < _dict->numitems; i++)
    if (_dict->texts[i] && !strcmp (_dict->texts[i], name))
      return dwg_ref_object (dwg, _dict->itemhandles[i]);
  return NULL;
}

/* The EVALUATION_GRAPH of definition def, or NULL. */
static Dwg_Object_EVALUATION_GRAPH *
definition_graph (Dwg_Data *dwg, const BITCODE_RLL def)
{
  const Dwg_Object *obj = dwg_resolve_handle (dwg, def);
  const Dwg_Object *graph;
  if (!obj || obj->fixedtype != DWG_TYPE_BLOCK_HEADER)
    return NULL;
  graph = dict_entry (dwg,
                      dwg_ref_object (dwg, obj->tio.object->xdicobjhandle),
                      "ACAD_ENHANCEDBLOCK");
  return graph && graph->fixedtype == DWG_TYPE_EVALUATION_GRAPH
             ? graph->tio.object->tio.EVALUATION_GRAPH
             : NULL;
}

/* The node of definition def's evaluation graph of the given type with the
   given block element name, or NULL. */
static Dwg_Object *
graph_node (Dwg_Data *dwg, const BITCODE_RLL def, const Dwg_Object_Type type,
            const char *name)
{
  const Dwg_Object_EVALUATION_GRAPH *graph = definition_graph (dwg, def);
  for (BITCODE_BL i = 0; graph && graph->nodes && i < graph->num_nodes; i++)
    {
      Dwg_Object *obj = dwg_ref_object (dwg, graph->nodes[i].evalexpr);
      // every parameter and action starts with BLOCKELEMENT_fields
      if (obj && obj->fixedtype == type
          && obj->tio.object->tio.BLOCKMOVEACTION->name
          && !strcmp (obj->tio.object->tio.BLOCKMOVEACTION->name, name))
        return obj;
    }
  return NULL;
}

static Dwg_Object_BLOCKSTRETCHACTION *
stretch_action (Dwg_Data *dwg, const BITCODE_RLL def, const char *name)
{
  Dwg_Object *obj = graph_node (dwg, def, DWG_TYPE_BLOCKSTRETCHACTION, name);
  return obj ? obj->tio.object->tio.BLOCKSTRETCHACTION : NULL;
}

static Dwg_Object_BLOCKMOVEACTION *
move_action (Dwg_Data *dwg, const BITCODE_RLL def, const char *name)
{
  Dwg_Object *obj = graph_node (dwg, def, DWG_TYPE_BLOCKMOVEACTION, name);
  return obj ? obj->tio.object->tio.BLOCKMOVEACTION : NULL;
}

/* The BLOCKLINEARPARAMETER of definition def whose property is named
   name, or NULL. */
static Dwg_Object_BLOCKLINEARPARAMETER *
linear_param (Dwg_Data *dwg, const BITCODE_RLL def, const char *name)
{
  const Dwg_Object_EVALUATION_GRAPH *graph = definition_graph (dwg, def);
  for (BITCODE_BL i = 0; graph && graph->nodes && i < graph->num_nodes; i++)
    {
      Dwg_Object *obj = dwg_ref_object (dwg, graph->nodes[i].evalexpr);
      if (obj && obj->fixedtype == DWG_TYPE_BLOCKLINEARPARAMETER
          && obj->tio.object->tio.BLOCKLINEARPARAMETER->distance_name
          && !strcmp (obj->tio.object->tio.BLOCKLINEARPARAMETER->distance_name,
                      name))
        return obj->tio.object->tio.BLOCKLINEARPARAMETER;
    }
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
    OK ("get_params %s: %u properties", what, (unsigned)count);
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
  {
    const Dwg_DynParam *size
        = error ? NULL : find_param (params, n, "Door Size");
    const Dwg_DynParam *hinge
        = error ? NULL : find_param (params, n, "Hinge");
    const Dwg_DynParam *angle
        = error ? NULL : find_param (params, n, "Opening Angle");
    if (size && size->values && size->num_values == 5
        && size->values[0] == 24.0 && size->values[4] == 40.0 && hinge
        && hinge->states && hinge->num_states == 2
        && !strcmp (hinge->states[0], "Left")
        && !strcmp (hinge->states[1], "Right") && angle && angle->states
        && angle->num_states == 5 && !strcmp (angle->states[4], "Closed"))
      OK ("get_params Door: values, flip labels and states");
    else
      {
        fail ("get_params Door: 0x%x, values, flip labels or states", error);
        failures++;
      }
  }
  dwg_dynblock_free_params (params, n);
  params = NULL;
  n = 0;

  // a block that isn't a dynamic block definition
  error = dwg_dynblock_get_params (block_header (dwg, model_space (dwg)),
                                   &params, &n);
  loglevel = test_loglevel;
  if (error == REFUSED (DWG_ERR_INVALIDTYPE) && !params && !n)
    OK ("get_params model space: refused, 0x%x", error);
  else
    {
      fail ("get_params model space: 0x%x", error);
      failures++;
    }
  dwg_dynblock_free_params (params, n);
  return failures;
}

/* A rotation parameter whose end point is turned a hair clockwise from its
   base angle has the angle 0, not 2 pi: here the Toilet's Angle, its end
   point turned by -1e-17 radians, which plus 2 pi rounds to 2 pi. */
static int
test_rotation_default (Dwg_Data *dwg)
{
  const Dwg_Object_EVALUATION_GRAPH *graph = definition_graph (dwg, TOILET);
  Dwg_Object_BLOCKROTATIONPARAMETER *rotation = NULL;
  BITCODE_3BD basept, endpt, base_angle_pt;
  Dwg_DynParam *params = NULL;
  const Dwg_DynParam *angle;
  BITCODE_BL n = 0;
  volatile double tiny = -1e-17;
  int error, failures = 0;
  for (BITCODE_BL i = 0; graph && graph->nodes && i < graph->num_nodes; i++)
    {
      Dwg_Object *obj = dwg_ref_object (dwg, graph->nodes[i].evalexpr);
      if (obj && obj->fixedtype == DWG_TYPE_BLOCKROTATIONPARAMETER)
        rotation = obj->tio.object->tio.BLOCKROTATIONPARAMETER;
    }
  loglevel = test_loglevel;
  if (!rotation)
    {
      fail ("rotation default: the Toilet's rotation parameter not found");
      return 1;
    }
  if (tiny + 2.0 * M_PI < 2.0 * M_PI)
    {
      fail ("rotation default: %g plus 2 pi doesn't round to 2 pi",
            (double)tiny);
      return 1;
    }
  basept = rotation->def_basept;
  endpt = rotation->def_endpt;
  base_angle_pt = rotation->def_base_angle_pt;
  memset (&rotation->def_basept, 0, sizeof (BITCODE_3BD));
  memset (&rotation->def_base_angle_pt, 0, sizeof (BITCODE_3BD));
  rotation->def_base_angle_pt.x = 1.0;
  rotation->def_endpt = rotation->def_base_angle_pt;
  rotation->def_endpt.y = tiny;
  error = dwg_dynblock_get_params (block_header (dwg, TOILET), &params, &n);
  loglevel = test_loglevel;
  angle = error ? NULL : find_param (params, n, "Angle");
  if (angle && angle->kind == DWG_DYNPARAM_ROTATION
      && angle->default_value == 0.0)
    OK ("get_params: a rotation a hair below 0 is 0, not 2 pi");
  else
    {
      fail ("get_params Toilet: 0x%x, Angle %.17g, expected 0", error,
            angle ? angle->default_value : -1.0);
      failures++;
    }
  dwg_dynblock_free_params (params, n);
  rotation->def_basept = basept;
  rotation->def_endpt = endpt;
  rotation->def_base_angle_pt = base_angle_pt;
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
    OK ("get_values " FORMAT_HV ": %u values", insert, (unsigned)count);
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
    OK ("get_values C174: Door Elevation's flip");
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
      OK ("evaluate refuses %s: 0x%x", what, error);
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
  Dwg_Object_BLOCK_HEADER *window = block_header (dwg, WINDOW);
  const Dwg_Object_BLOCK_HEADER *altezza = block_header (dwg, ALTEZZA);
  Dwg_Object_EVALUATION_GRAPH *graph = definition_graph (dwg, WINDOW);
  int failures = 0;
  memset (v, 0, sizeof (v));
  if (!window || !window->entities || window->num_owned < 2 || !altezza
      || !graph || !graph->nodes || !graph->num_nodes)
    {
      fail ("refusals: fixture objects not found");
      return 1;
    }

  // Door Size drives the Door's scale action, ScaleArc
  v[0].name = (char *)"Door Size";
  v[0].value = 40.0;
  failures += test_refusal (dwg, DOOR, v, 1,
                            REFUSED (DWG_ERR_NOTYETSUPPORTED),
                            "a linear value driving a scale action");
  v[0].name = (char *)"Window Width";
  v[0].value = -1.0;
  failures += test_refusal (dwg, WINDOW, v, 1,
                            REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "a linear value below 0");
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
  v[0].value = nan ("");
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
  failures += test_refusal (dwg, model_space (dwg), NULL, 0,
                            REFUSED (DWG_ERR_INVALIDTYPE),
                            "a block that isn't dynamic");
  {
    BITCODE_H second = window->entities[1];
    // the copy refuses an entity listed twice, before the block is made
    window->entities[1] = window->entities[0];
    failures += test_refusal (dwg, WINDOW, NULL, 0,
                              REFUSED (DWG_ERR_INVALIDHANDLE),
                              "an entity the definition lists twice");
    // the evaluation refuses an entry of the definition that doesn't
    // resolve, or isn't an entity, before it copies anything
    window->entities[1] = dwg_add_handleref (dwg, 3, 0xFFFFFF, NULL);
    failures += test_refusal (dwg, WINDOW, NULL, 0,
                              REFUSED (DWG_ERR_INVALIDHANDLE),
                              "an entity that doesn't resolve");
    // the Window's extension dictionary
    window->entities[1] = window->parent->xdicobjhandle;
    failures += test_refusal (dwg, WINDOW, NULL, 0,
                              REFUSED (DWG_ERR_INVALIDHANDLE),
                              "an entry that isn't an entity");
    window->entities[1] = second;
  }
  // a node of the evaluation graph that doesn't resolve
  {
    BITCODE_H node = graph->nodes[0].evalexpr;
    Dwg_DynParam *params = NULL;
    BITCODE_BL n = 0;
    int error;
    graph->nodes[0].evalexpr = dwg_add_handleref (dwg, 4, 0xFFFFFE, NULL);
    failures += test_refusal (dwg, WINDOW, NULL, 0,
                              REFUSED (DWG_ERR_INVALIDHANDLE),
                              "a graph node that doesn't resolve");
    error = dwg_dynblock_get_params (window, &params, &n);
    loglevel = test_loglevel;
    if (error == REFUSED (DWG_ERR_INVALIDHANDLE) && !params && !n)
      OK ("get_params refuses a graph node that doesn't resolve: 0x%x",
          error);
    else
      {
        fail ("get_params of a graph node that doesn't resolve: 0x%x, %u "
              "properties",
              error, (unsigned)n);
        failures++;
      }
    dwg_dynblock_free_params (params, n);
    // W4's values, which name the Window
    {
      Dwg_Object_BLOCK_HEADER *defp = NULL;
      Dwg_DynValue *values = NULL;
      BITCODE_BL count = 0;
      error = dwg_dynblock_get_values (insert_entity (dwg, 0xC8D3), &defp,
                                       &values, &count);
      loglevel = test_loglevel;
      if (error == REFUSED (DWG_ERR_INVALIDHANDLE) && !defp && !values
          && !count)
        OK ("get_values refuses a graph node that doesn't resolve: 0x%x",
            error);
      else
        {
          fail ("get_values of a graph node that doesn't resolve: 0x%x, %u "
                "values",
                error, (unsigned)count);
          failures++;
        }
      dwg_dynblock_free_values (values, count);
    }
    graph->nodes[0].evalexpr = node;
  }
  // an INSERT of a definition owning an object that isn't an entity
  {
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
   owned by the block, with no extension dictionary; and the number of
   them that show, which the state's invisible flags give. */
static int
check_evaluated (Dwg_Data *dwg, const BITCODE_RLL def_handle,
                 const BITCODE_RLL rep_handle, const BITCODE_BL expected_shown,
                 const char *what)
{
  const Dwg_Object_BLOCK_HEADER *def = block_header (dwg, def_handle);
  const Dwg_Object_BLOCK_HEADER *rep = block_header (dwg, rep_handle);
  BITCODE_BL shown = 0;
  if (!def)
    {
      fail ("%s: definition " FORMAT_HV " not found", what, def_handle);
      return 1;
    }
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
  OK ("%s: %u entities, %u shown", what, (unsigned)def->num_owned,
      (unsigned)shown);
  return 0;
}

static int
test_evaluate (Dwg_Data *dwg)
{
  Dwg_DynValue v[4];
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
    OK ("evaluate Trees adds 585 objects, 584 of them entities");
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

  // D3: Door at Closed, which shows 3 of its 11, with Wall Thickness and
  // the flips given at the definition's values
  v[0].name = (char *)"Opening Angle";
  v[0].state = (char *)"Closed";
  v[1].name = (char *)"Wall Thickness";
  v[1].value = 5.0;
  v[2].name = (char *)"Hinge";
  v[3].name = (char *)"Swing";
  rep = dwg_dynblock_evaluate (block_header (dwg, DOOR), v, 4, &error);
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
      OK ("write r2004 and read back: 0x%x", error);
      failures += check_evaluated (&readback, TREES, shrub, 52,
                                   "read back Trees at Shrub (Plan)");
      failures += check_evaluated (&readback, DOOR, door_closed, 3,
                                   "read back Door at Closed");
    }
  free_dwg (&readback);
  unlink (WRITTEN);
  return failures;
}

static BITCODE_RLL
entity_handle (const Dwg_Data *dwg, const Dwg_Object_Entity *ent)
{
  return dwg->object[ent->objid].handle.value;
}

/* Makes the entity ref names owned by the object with handle owner, for
   entities moved from the object that owned them to another. */
static void
set_owner (Dwg_Data *dwg, const BITCODE_H ref, const BITCODE_RLL owner)
{
  Dwg_Object *obj = ref ? dwg_ref_object (dwg, ref) : NULL;
  if (obj && obj->supertype == DWG_SUPERTYPE_ENTITY)
    obj->tio.entity->ownerhandle = dwg_add_handleref (dwg, 4, owner, NULL);
}

/* Whether ref names a copy, not source, of an entity of the given type,
   owned by owner. */
static int
is_owned_copy (Dwg_Data *dwg, const BITCODE_H ref, const Dwg_Object_Type type,
               const BITCODE_RLL owner, const BITCODE_H source)
{
  const Dwg_Object *obj = ref ? dwg_ref_object (dwg, ref) : NULL;
  return obj && source && obj->fixedtype == type
         && obj->handle.value != source->absolute_ref
         && obj->supertype == DWG_SUPERTYPE_ENTITY
         && obj->tio.entity->ownerhandle
         && obj->tio.entity->ownerhandle->absolute_ref == owner;
}

/* The copy, in evaluated block rep, of entity handle of definition def. */
static Dwg_Object *
copy_of (Dwg_Data *dwg, const Dwg_Object_BLOCK_HEADER *def,
         const Dwg_Object_BLOCK_HEADER *rep, const BITCODE_RLL handle)
{
  for (BITCODE_BL i = 0; def->entities && rep->entities && i < def->num_owned
                         && i < rep->num_owned;
       i++)
    if (def->entities[i] && def->entities[i]->absolute_ref == handle)
      return dwg_ref_object (dwg, rep->entities[i]);
  return NULL;
}

/* Turns the INSERT at handle, with its ATTRIBs, into a TABLE of no rows,
   as the add API makes no TABLE. Returns NULL on failure. */
static Dwg_Entity_TABLE *
insert_to_table (Dwg_Data *dwg, const BITCODE_RLL handle)
{
  const int type
      = dwg_require_class (dwg, "ACAD_TABLE", strlen ("ACAD_TABLE"));
  Dwg_Object *obj = dwg_resolve_handle (dwg, handle);
  Dwg_Entity_INSERT *insert;
  Dwg_Entity_TABLE *table;
  if (!obj || obj->fixedtype != DWG_TYPE_INSERT || type < 500)
    return NULL;
  insert = obj->tio.entity->tio.INSERT;
  table = (Dwg_Entity_TABLE *)calloc (1, sizeof (Dwg_Entity_TABLE));
  if (!table)
    return NULL;
  table->ins_pt = insert->ins_pt;
  table->scale = insert->scale;
  table->extrusion = insert->extrusion;
  table->block_header = insert->block_header;
  table->has_attribs = insert->has_attribs;
  table->num_owned = insert->num_owned;
  table->attribs = insert->attribs;
  table->seqend = insert->seqend;
  table->horiz_direction.x = 1.0;
  table->parent = obj->tio.entity;
  // the TABLE took the pointers the INSERT held
  free (insert);
  obj->tio.entity->tio.TABLE = table;
  obj->type = (BITCODE_BS)type;
  obj->fixedtype = DWG_TYPE_TABLE;
  obj->name = (char *)"TABLE";
  obj->dxfname = (char *)"ACAD_TABLE";
  return table;
}

/* Entities that own others, added to a definition: an INSERT with an
   ATTRIB, a POLYLINE_2D with its VERTEXes, and a TABLE with an ATTRIB.
   Their copies own copies of what they own, which stay out of the block's
   entity list. A MINSERT is copied too, and an ATTDEF gives the
   definition, and so the evaluated block, the flag for attribute
   definitions. */
static int
test_evaluate_owned (Dwg_Data *dwg)
{
  Dwg_Object_BLOCK_HEADER *def = block_header (dwg, TREES);
  const dwg_point_3d pt = { 0.0, 0.0, 0.0 };
  const dwg_point_2d pts[3] = { { 0.0, 0.0 }, { 1.0, 0.0 }, { 1.0, 1.0 } };
  Dwg_Entity_INSERT *insert = NULL, *donor = NULL;
  Dwg_Entity_POLYLINE_2D *pline = NULL;
  Dwg_Entity_TABLE *table = NULL;
  Dwg_Entity_MINSERT *minsert = NULL;
  Dwg_Entity_ATTDEF *attdef = NULL;
  Dwg_Object_BLOCK_HEADER *rep;
  Dwg_Object *copy;
  BITCODE_RLL h_insert, h_pline, h_table, h_minsert;
  int error = 0, failures = 0;
  if (!def)
    {
      fail ("evaluate owned: Trees not found");
      return 1;
    }
  // 't' is a block alteZZa's INSERTs draw
  insert = dwg_add_INSERT (def, &pt, "t", 1.0, 1.0, 1.0, 0.0);
  if (insert && !dwg_add_ATTRIB (insert, 1.0, 0, &pt, "TAG", "value"))
    insert = NULL;
  pline = dwg_add_POLYLINE_2D (def, 3, pts);
  donor = dwg_add_INSERT (def, &pt, "t", 1.0, 1.0, 1.0, 0.0);
  if (donor && !dwg_add_ATTRIB (donor, 1.0, 0, &pt, "TAG2", "value2"))
    donor = NULL;
  minsert = dwg_add_MINSERT (def, &pt, "t", 1.0, 1.0, 1.0, 0.0, 2, 2, 1.0,
                             1.0);
  attdef = dwg_add_ATTDEF (def, 1.0, 0, "Prompt", &pt, "ATAG", "default");
  loglevel = test_loglevel;
  if (!insert || !pline || !donor || !minsert || !attdef
      || pline->num_owned != 3 || !pline->vertex)
    {
      fail ("evaluate owned: could not add the entities");
      return 1;
    }
  h_insert = entity_handle (dwg, insert->parent);
  h_pline = entity_handle (dwg, pline->parent);
  h_table = entity_handle (dwg, donor->parent);
  h_minsert = entity_handle (dwg, minsert->parent);
  table = insert_to_table (dwg, h_table);
  if (!table)
    {
      fail ("evaluate owned: could not make a TABLE");
      return 1;
    }

  rep = dwg_dynblock_evaluate (def, NULL, 0, &error);
  loglevel = test_loglevel;
  if (!rep || error || rep->num_owned != def->num_owned)
    {
      fail ("evaluate owned: 0x%x, %u entities, expected %u", error,
            rep ? (unsigned)rep->num_owned : 0, (unsigned)def->num_owned);
      return 1;
    }

  copy = copy_of (dwg, def, rep, h_insert);
  if (copy && copy->fixedtype == DWG_TYPE_INSERT
      && copy->tio.entity->tio.INSERT->num_owned == 1
      && copy->tio.entity->tio.INSERT->attribs
      && is_owned_copy (dwg, copy->tio.entity->tio.INSERT->attribs[0],
                        DWG_TYPE_ATTRIB, copy->handle.value,
                        insert->attribs[0])
      && is_owned_copy (dwg, copy->tio.entity->tio.INSERT->seqend,
                        DWG_TYPE_SEQEND, copy->handle.value, insert->seqend))
    OK ("evaluate owned: the ATTRIB and SEQEND copies are the INSERT copy's");
  else
    {
      fail ("evaluate owned: the INSERT copy " FORMAT_HV
            " doesn't own copies of its ATTRIB and SEQEND",
            copy ? copy->handle.value : 0);
      failures++;
    }

  copy = copy_of (dwg, def, rep, h_pline);
  {
    const Dwg_Entity_POLYLINE_2D *_copy
        = copy && copy->fixedtype == DWG_TYPE_POLYLINE_2D
              ? copy->tio.entity->tio.POLYLINE_2D
              : NULL;
    int owned = _copy && _copy->num_owned == 3 && _copy->vertex
                && is_owned_copy (dwg, _copy->seqend, DWG_TYPE_SEQEND,
                                  copy->handle.value, pline->seqend);
    for (BITCODE_BL i = 0; owned && i < 3; i++)
      owned = is_owned_copy (dwg, _copy->vertex[i], DWG_TYPE_VERTEX_2D,
                             copy->handle.value, pline->vertex[i]);
    if (owned)
      OK ("evaluate owned: the VERTEX and SEQEND copies are the POLYLINE "
          "copy's");
    else
      {
        fail ("evaluate owned: the POLYLINE_2D copy " FORMAT_HV
              " doesn't own copies of its VERTEXes and SEQEND",
              copy ? copy->handle.value : 0);
        failures++;
      }
  }

  copy = copy_of (dwg, def, rep, h_table);
  if (copy && copy->fixedtype == DWG_TYPE_TABLE
      && copy->tio.entity->tio.TABLE->num_owned == 1
      && copy->tio.entity->tio.TABLE->attribs
      && is_owned_copy (dwg, copy->tio.entity->tio.TABLE->attribs[0],
                        DWG_TYPE_ATTRIB, copy->handle.value,
                        table->attribs[0])
      && is_owned_copy (dwg, copy->tio.entity->tio.TABLE->seqend,
                        DWG_TYPE_SEQEND, copy->handle.value, table->seqend))
    OK ("evaluate owned: the ATTRIB and SEQEND copies are the TABLE copy's");
  else
    {
      fail ("evaluate owned: the TABLE copy " FORMAT_HV
            " doesn't own copies of its ATTRIB and SEQEND",
            copy ? copy->handle.value : 0);
      failures++;
    }

  copy = copy_of (dwg, def, rep, h_minsert);
  if (copy && copy->fixedtype == DWG_TYPE_MINSERT
      && copy->handle.value != h_minsert && copy->tio.entity->ownerhandle
      && copy->tio.entity->ownerhandle->absolute_ref == block_handle (rep))
    OK ("evaluate owned: the MINSERT copy is the block's");
  else
    {
      fail ("evaluate owned: no MINSERT copy owned by the block");
      failures++;
    }

  if (def->hasattrs && rep->hasattrs && (rep->flag & 2))
    OK ("evaluate owned: the block has attribute definitions, as Trees "
        "with an ATTDEF");
  else
    {
      fail ("evaluate owned: hasattrs %u, flag 0x%x; the definition's %u",
            (unsigned)rep->hasattrs, (unsigned)rep->flag,
            (unsigned)def->hasattrs);
      failures++;
    }
  return failures;
}

/*--------------------------------------------------------------------------------
 * Linear properties, and their stretch and move actions
 */

static Dwg_Object *rep_entity (Dwg_Data *dwg, const BITCODE_RLL blk_handle,
                               const BITCODE_RL idx);

/* Appends an entity to the list of stretch action a, with the given stretch
   points, or none. Returns 0 when memory runs out. */
static int
add_stretch_entity (Dwg_Data *dwg, Dwg_Object_BLOCKSTRETCHACTION *a,
                    const BITCODE_RLL handle, const BITCODE_BL *indexes,
                    const BITCODE_BS num_indexes)
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
  if (num_indexes)
    {
      h->indexes = (BITCODE_BL *)malloc (num_indexes * sizeof (BITCODE_BL));
      if (!h->indexes)
        return 0;
      memcpy (h->indexes, indexes, num_indexes * sizeof (BITCODE_BL));
      h->num_indexes = num_indexes;
    }
  a->num_hdls++;
  return 1;
}

/* Removes the entity add_stretch_entity () appended last. */
static void
pop_stretch_entity (Dwg_Object_BLOCKSTRETCHACTION *a)
{
  a->num_hdls--;
  free (a->hdls[a->num_hdls].indexes);
}

/* The index add_stretch_code () takes for no stretch point. */
#define NO_POINT 0xFFFFFFFF

/* Appends to stretch action a the parameter with node id nodeid, with one
   stretch point (0 its base, 1 its end), or with none when index is
   NO_POINT. Returns 0 when memory runs out. */
static int
add_stretch_code (Dwg_Object_BLOCKSTRETCHACTION *a, const BITCODE_BL nodeid,
                  const BITCODE_BL index)
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
  if (index != NO_POINT)
    {
      c->indexes = (BITCODE_BL *)malloc (sizeof (BITCODE_BL));
      if (!c->indexes)
        return 0;
      c->indexes[0] = index;
      c->num_indexes = 1;
    }
  a->num_codes++;
  return 1;
}

static void
pop_stretch_code (Dwg_Object_BLOCKSTRETCHACTION *a)
{
  a->num_codes--;
  free (a->codes[a->num_codes].indexes);
}

/* Appends an object to the selection *deps of *num_deps objects of an
   action. */
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

/* Appends an entity to the selection of move action a. */
static int
add_move_entity (Dwg_Data *dwg, Dwg_Object_BLOCKMOVEACTION *a,
                 const BITCODE_RLL handle)
{
  return add_dep (dwg, &a->deps, &a->num_deps, handle);
}

static int
is_point (const double x, const double y, const double ex, const double ey)
{
  return fabs (x - ex) <= TOLERANCE && fabs (y - ey) <= TOLERANCE;
}

/* Evaluates def at the n values v, and returns the block, or NULL with a
   failure. */
static Dwg_Object_BLOCK_HEADER *
evaluate (Dwg_Data *dwg, const BITCODE_RLL def, const Dwg_DynValue *v,
          const BITCODE_BL n, const char *what)
{
  int error = 0;
  Dwg_Object_BLOCK_HEADER *rep
      = dwg_dynblock_evaluate (block_header (dwg, def), v, n, &error);
  loglevel = test_loglevel;
  if (!rep || error)
    {
      fail ("%s: evaluate 0x%x", what, error);
      return NULL;
    }
  return rep;
}

/* Window at Window Width 24: the stretch points in 'Stretch Width''s frame
   move by -12 in x, those in 'Stretch Divider''s by -6. LINE BBD8 crosses
   both frames, its start in the first and its end in the second;
   LWPOLYLINE BBD3, the frame, has its vertices 2 and 3 in the first. At
   Window Height 54 alone, BBD3's vertices 1 and 2 move up by 18. */
static int
test_linear (Dwg_Data *dwg)
{
  const Dwg_Object_BLOCK_HEADER *def = block_header (dwg, WINDOW);
  Dwg_Object_BLOCK_HEADER *rep;
  Dwg_DynValue v[2];
  const Dwg_Object *line, *frame;
  int failures = 0;
  memset (v, 0, sizeof (v));
  if (!def)
    {
      fail ("linear: Window not found");
      return 1;
    }
  v[0].name = (char *)"Window Width";
  v[0].value = 24.0;
  rep = evaluate (dwg, WINDOW, v, 1, "linear Width 24");
  if (!rep)
    return 1;
  line = copy_of (dwg, def, rep, 0xBBD8);
  frame = copy_of (dwg, def, rep, 0xBBD3);
  if (line && frame && line->fixedtype == DWG_TYPE_LINE
      && frame->fixedtype == DWG_TYPE_LWPOLYLINE
      && frame->tio.entity->tio.LWPOLYLINE->num_points == 4
      && is_point (line->tio.entity->tio.LINE->start.x,
                   line->tio.entity->tio.LINE->start.y, 22.5, 1.5)
      && is_point (line->tio.entity->tio.LINE->end.x,
                   line->tio.entity->tio.LINE->end.y, 12.0, 1.5)
      && is_point (frame->tio.entity->tio.LWPOLYLINE->points[0].x,
                   frame->tio.entity->tio.LWPOLYLINE->points[0].y, 0.0, 0.0)
      && is_point (frame->tio.entity->tio.LWPOLYLINE->points[1].x,
                   frame->tio.entity->tio.LWPOLYLINE->points[1].y, 0.0, 36.0)
      && is_point (frame->tio.entity->tio.LWPOLYLINE->points[2].x,
                   frame->tio.entity->tio.LWPOLYLINE->points[2].y, 24.0, 36.0)
      && is_point (frame->tio.entity->tio.LWPOLYLINE->points[3].x,
                   frame->tio.entity->tio.LWPOLYLINE->points[3].y, 24.0, 0.0))
    OK ("linear: Width 24 stretches the frame and moves the divider by "
        "half");
  else
    {
      fail ("linear: Width 24 doesn't stretch the LINE and LWPOLYLINE as "
            "expected");
      failures++;
    }

  v[0].name = (char *)"Window Height";
  v[0].value = 54.0;
  rep = evaluate (dwg, WINDOW, v, 1, "linear Height 54");
  if (!rep)
    return failures + 1;
  frame = copy_of (dwg, def, rep, 0xBBD3);
  if (frame && frame->fixedtype == DWG_TYPE_LWPOLYLINE
      && frame->tio.entity->tio.LWPOLYLINE->num_points == 4
      && is_point (frame->tio.entity->tio.LWPOLYLINE->points[1].x,
                   frame->tio.entity->tio.LWPOLYLINE->points[1].y, 0.0, 54.0)
      && is_point (frame->tio.entity->tio.LWPOLYLINE->points[2].x,
                   frame->tio.entity->tio.LWPOLYLINE->points[2].y, 36.0, 54.0)
      && is_point (frame->tio.entity->tio.LWPOLYLINE->points[3].x,
                   frame->tio.entity->tio.LWPOLYLINE->points[3].y, 36.0, 0.0))
    OK ("linear: Height 54 alone leaves Width at the definition's");
  else
    {
      fail ("linear: Height 54 doesn't stretch the frame as expected");
      failures++;
    }
  return failures;
}

/* A parameter that another property's action moves is evaluated after it,
   though the graph lists it first: here 'Stretch Height' also moves the
   end point of Window Width up by Height's change, so Width 24 then
   shortens a parameter from (0, 0) to (36, 18). Left out of the values,
   Width is not set, and its stretch doesn't run. */
static int
test_linear_order (Dwg_Data *dwg)
{
  const Dwg_Object_BLOCK_HEADER *def = block_header (dwg, WINDOW);
  Dwg_Object_BLOCKSTRETCHACTION *height
      = stretch_action (dwg, WINDOW, "Stretch Height");
  Dwg_Object_BLOCKLINEARPARAMETER *width
      = linear_param (dwg, WINDOW, "Window Width");
  Dwg_Object_BLOCK_HEADER *rep;
  Dwg_DynValue v[2];
  const Dwg_Object *frame;
  const double len = sqrt (36.0 * 36.0 + 18.0 * 18.0);
  int failures = 0;
  memset (v, 0, sizeof (v));
  if (!def || !height || !width
      || !add_stretch_code (height, width->evalexpr.nodeid, 1))
    {
      fail ("linear order: fixture objects not found");
      return 1;
    }
  v[0].name = (char *)"Window Width";
  v[0].value = 24.0;
  v[1].name = (char *)"Window Height";
  v[1].value = 54.0;
  rep = evaluate (dwg, WINDOW, v, 2, "linear order");
  frame = rep ? copy_of (dwg, def, rep, 0xBBD3) : NULL;
  // vertex 3, (36, 0), is in Width's frame only
  if (frame && frame->fixedtype == DWG_TYPE_LWPOLYLINE
      && frame->tio.entity->tio.LWPOLYLINE->num_points == 4
      && is_point (frame->tio.entity->tio.LWPOLYLINE->points[3].x,
                   frame->tio.entity->tio.LWPOLYLINE->points[3].y,
                   36.0 * 24.0 / len, 18.0 * 24.0 / len - 18.0))
    OK ("linear order: Width is evaluated after the action moving it");
  else
    {
      fail ("linear order: vertex 3 at (%g, %g)",
            frame ? frame->tio.entity->tio.LWPOLYLINE->points[3].x : 0.0,
            frame ? frame->tio.entity->tio.LWPOLYLINE->points[3].y : 0.0);
      failures++;
    }

  rep = evaluate (dwg, WINDOW, &v[1], 1, "linear order, Height alone");
  frame = rep ? copy_of (dwg, def, rep, 0xBBD3) : NULL;
  if (frame && frame->fixedtype == DWG_TYPE_LWPOLYLINE
      && frame->tio.entity->tio.LWPOLYLINE->num_points == 4
      && is_point (frame->tio.entity->tio.LWPOLYLINE->points[3].x,
                   frame->tio.entity->tio.LWPOLYLINE->points[3].y, 36.0, 0.0))
    OK ("linear order: Width left out is not set");
  else
    {
      fail ("linear order: Width left out moved vertex 3");
      failures++;
    }
  pop_stretch_code (height);
  return failures;
}

/* Actions that move each other's parameters: 'Stretch Width' made to move
   the end point of Window Height, and 'Stretch Height' that of Window
   Width. Only the actions of the properties given a value run, so the
   Window evaluates with no values, and with Width alone, whose stretch
   then moves Height's end point; with both changed, the order AutoCAD
   evaluates them in isn't known, and the evaluator refuses them. */
static int
test_linear_cycle (Dwg_Data *dwg)
{
  const Dwg_Object_BLOCK_HEADER *def = block_header (dwg, WINDOW);
  Dwg_Object_BLOCKSTRETCHACTION *width_stretch
      = stretch_action (dwg, WINDOW, "Stretch Width");
  Dwg_Object_BLOCKSTRETCHACTION *height_stretch
      = stretch_action (dwg, WINDOW, "Stretch Height");
  Dwg_Object_BLOCKLINEARPARAMETER *width
      = linear_param (dwg, WINDOW, "Window Width");
  Dwg_Object_BLOCKLINEARPARAMETER *height
      = linear_param (dwg, WINDOW, "Window Height");
  const Dwg_Object_BLOCK_HEADER *rep;
  const Dwg_Object *frame;
  Dwg_DynValue v[2];
  int failures = 0;
  memset (v, 0, sizeof (v));
  if (!def || !width_stretch || !height_stretch || !width || !height)
    {
      fail ("linear cycle: fixture objects not found");
      return 1;
    }
  if (!add_stretch_code (width_stretch, height->evalexpr.nodeid, 1))
    {
      fail ("linear cycle: out of memory");
      return 1;
    }
  if (!add_stretch_code (height_stretch, width->evalexpr.nodeid, 1))
    {
      fail ("linear cycle: out of memory");
      pop_stretch_code (width_stretch);
      return 1;
    }
  v[0].name = (char *)"Window Width";
  v[0].value = 24.0;
  v[1].name = (char *)"Window Height";
  v[1].value = 54.0;

  rep = evaluate (dwg, WINDOW, NULL, 0, "linear cycle, no values");
  frame = rep ? copy_of (dwg, def, rep, 0xBBD3) : NULL;
  if (frame && frame->fixedtype == DWG_TYPE_LWPOLYLINE
      && frame->tio.entity->tio.LWPOLYLINE->num_points == 4
      && is_point (frame->tio.entity->tio.LWPOLYLINE->points[2].x,
                   frame->tio.entity->tio.LWPOLYLINE->points[2].y, 36.0, 36.0))
    OK ("linear cycle: the Window evaluates with no values");
  else if (rep)
    {
      fail ("linear cycle: no values moved the frame");
      failures++;
    }
  else
    failures++;

  // as in test_linear: the frame's vertices 2 and 3 move to x 24
  rep = evaluate (dwg, WINDOW, v, 1, "linear cycle, Width alone");
  frame = rep ? copy_of (dwg, def, rep, 0xBBD3) : NULL;
  if (frame && frame->fixedtype == DWG_TYPE_LWPOLYLINE
      && frame->tio.entity->tio.LWPOLYLINE->num_points == 4
      && is_point (frame->tio.entity->tio.LWPOLYLINE->points[1].x,
                   frame->tio.entity->tio.LWPOLYLINE->points[1].y, 0.0, 36.0)
      && is_point (frame->tio.entity->tio.LWPOLYLINE->points[2].x,
                   frame->tio.entity->tio.LWPOLYLINE->points[2].y, 24.0, 36.0)
      && is_point (frame->tio.entity->tio.LWPOLYLINE->points[3].x,
                   frame->tio.entity->tio.LWPOLYLINE->points[3].y, 24.0, 0.0))
    OK ("linear cycle: Width alone runs only Width's stretch");
  else if (rep)
    {
      fail ("linear cycle: Width alone doesn't stretch the frame as "
            "expected");
      failures++;
    }
  else
    failures++;

  failures += test_refusal (dwg, WINDOW, v, 2,
                            REFUSED (DWG_ERR_NOTYETSUPPORTED),
                            "linear values whose actions move each other's "
                            "parameters");
  pop_stretch_code (height_stretch);
  pop_stretch_code (width_stretch);
  return failures;
}

/* What the evaluator refuses of linear properties and their actions, here
   the Window's changed in memory, at Window Width 24. */
static int
test_linear_refusals (Dwg_Data *dwg)
{
  Dwg_Object_BLOCKSTRETCHACTION *stretch
      = stretch_action (dwg, WINDOW, "Stretch Width");
  Dwg_Object_BLOCKSTRETCHACTION *height_stretch
      = stretch_action (dwg, WINDOW, "Stretch Height");
  Dwg_Object_BLOCKLINEARPARAMETER *width
      = linear_param (dwg, WINDOW, "Window Width");
  Dwg_Object_BLOCKLINEARPARAMETER *height
      = linear_param (dwg, WINDOW, "Window Height");
  Dwg_Object *frame = dwg_resolve_handle (dwg, 0xBBD3);
  Dwg_DynValue v;
  int failures = 0;
  memset (&v, 0, sizeof (v));
  if (!stretch || !stretch->hdls || !stretch->num_hdls
      || !stretch->hdls[0].indexes || !stretch->hdls[0].num_indexes
      || !height_stretch || !width || !height || !frame
      || frame->fixedtype != DWG_TYPE_LWPOLYLINE)
    {
      fail ("linear refusals: fixture objects not found");
      return 1;
    }
  v.name = (char *)"Window Width";
  v.value = 24.0;

  width->parameter_base_location = 1;
  failures += test_refusal (dwg, WINDOW, &v, 1,
                            REFUSED (DWG_ERR_NOTYETSUPPORTED),
                            "a linear parameter with its base in its middle");
  width->parameter_base_location = 0;

  {
    const BITCODE_3BD endpt = width->def_endpt;
    const Dwg_Object_BLOCK_HEADER *def = block_header (dwg, WINDOW);
    const Dwg_Object_BLOCK_HEADER *rep;
    const Dwg_Object *copy;
    width->def_endpt = width->def_basept;
    failures += test_refusal (dwg, WINDOW, &v, 1, REFUSED (0),
                              "a linear parameter of no length");
    // a huge value on a short parameter: the frame's vertex 3 moves to
    // about 1e308, and stays at y 0
    width->def_endpt.x = width->def_basept.x + 0.5;
    v.value = 1e308;
    rep = evaluate (dwg, WINDOW, &v, 1, "linear 1e308");
    copy = rep ? copy_of (dwg, def, rep, 0xBBD3) : NULL;
    if (copy && copy->fixedtype == DWG_TYPE_LWPOLYLINE
        && copy->tio.entity->tio.LWPOLYLINE->num_points == 4
        && isfinite (copy->tio.entity->tio.LWPOLYLINE->points[3].x)
        && copy->tio.entity->tio.LWPOLYLINE->points[3].x > 1e307
        && copy->tio.entity->tio.LWPOLYLINE->points[3].y == 0.0)
      OK ("linear: 1e308 on a parameter 0.5 long moves points by about it");
    else
      {
        fail ("linear: 1e308 on a parameter 0.5 long");
        failures++;
      }
    // a parameter 1e200 long set to 2e200 extends, though squaring its
    // length overflows
    width->def_endpt.x = width->def_basept.x + 1e200;
    v.value = 2e200;
    rep = evaluate (dwg, WINDOW, &v, 1, "linear 2e200");
    copy = rep ? copy_of (dwg, def, rep, 0xBBD3) : NULL;
    if (copy && copy->fixedtype == DWG_TYPE_LWPOLYLINE
        && copy->tio.entity->tio.LWPOLYLINE->num_points == 4
        && fabs (copy->tio.entity->tio.LWPOLYLINE->points[3].x / 1e200 - 1.0)
               < 1e-9)
      OK ("linear: a parameter 1e200 long set to 2e200 extends by 1e200");
    else
      {
        fail ("linear: a parameter 1e200 long set to 2e200");
        failures++;
      }
    // a parameter longer than a double holds, whose stretch at half its
    // change would keep the points in range
    width->def_endpt.x = width->def_basept.x + 1.3e308;
    width->def_endpt.y = width->def_basept.y + 1.3e308;
    stretch->action_offset_x = 0.5;
    v.value = 1e308;
    failures += test_refusal (dwg, WINDOW, &v, 1,
                              REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                              "a parameter longer than a double holds");
    stretch->action_offset_x = 1.0;
    width->def_endpt = endpt;
    // 4 times as much is more than a double holds
    stretch->action_offset_x = 4.0;
    v.value = 1e308;
    failures += test_refusal (dwg, WINDOW, &v, 1,
                              REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                              "a value an action can't move points by");
    stretch->action_offset_x = 1.0;
    // 1.5e308 moves the frame's vertices 2 and 3 once each
    v.value = 1.5e308;
    rep = evaluate (dwg, WINDOW, &v, 1, "linear 1.5e308");
    copy = rep ? copy_of (dwg, def, rep, 0xBBD3) : NULL;
    if (copy && copy->fixedtype == DWG_TYPE_LWPOLYLINE
        && copy->tio.entity->tio.LWPOLYLINE->num_points == 4
        && isfinite (copy->tio.entity->tio.LWPOLYLINE->points[3].x)
        && copy->tio.entity->tio.LWPOLYLINE->points[3].x > 1e308)
      OK ("linear: 1.5e308 moves each stretched vertex once");
    else
      {
        fail ("linear: 1.5e308");
        failures++;
      }
    // LINE BBD7, wholly in 'Stretch Width''s frame, made to start at 1e308
    {
      Dwg_Object *line = dwg_resolve_handle (dwg, 0xBBD7);
      const double x = line && line->fixedtype == DWG_TYPE_LINE
                           ? line->tio.entity->tio.LINE->start.x
                           : 0.0;
      if (line && line->fixedtype == DWG_TYPE_LINE)
        {
          line->tio.entity->tio.LINE->start.x = 1e308;
          v.value = 1e308;
          failures += test_refusal (dwg, WINDOW, &v, 1,
                                    REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                                    "a move beyond what a double holds");
          line->tio.entity->tio.LINE->start.x = x;
        }
      else
        {
          fail ("linear: LINE BBD7 not found");
          failures++;
        }
    }
    // and listed whole by 'Stretch Divider' too: its moves add up
    {
      Dwg_Object_BLOCKSTRETCHACTION *divider
          = stretch_action (dwg, WINDOW, "Stretch Divider");
      static const BITCODE_BL ends[] = { 0, 1 };
      if (divider && add_stretch_entity (dwg, divider, 0xBBD7, ends, 2))
        {
          v.value = 1.2e308;
          failures += test_refusal (dwg, WINDOW, &v, 1,
                                    REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                                    "moves adding up beyond a double");
          pop_stretch_entity (divider);
        }
    }
    v.value = 24.0;
  }

  // a parameter that another action moves far: Height 1e200 moves Width's
  // end point to about (36, 1e200), so Width 2e200 then extends it
  if (add_stretch_code (height_stretch, width->evalexpr.nodeid, 1))
    {
      const Dwg_Object_BLOCK_HEADER *def = block_header (dwg, WINDOW);
      const Dwg_Object_BLOCK_HEADER *rep;
      const Dwg_Object *copy;
      Dwg_DynValue w[2];
      memset (w, 0, sizeof (w));
      w[0].name = (char *)"Window Height";
      w[0].value = 1e200;
      w[1].name = (char *)"Window Width";
      w[1].value = 2e200;
      rep = evaluate (dwg, WINDOW, w, 2, "linear moved far");
      copy = rep ? copy_of (dwg, def, rep, 0xBBD3) : NULL;
      // vertex 3, (36, 0), in Width's frame only, moves by about (36, 1e200)
      if (copy && copy->fixedtype == DWG_TYPE_LWPOLYLINE
          && copy->tio.entity->tio.LWPOLYLINE->num_points == 4
          && fabs (copy->tio.entity->tio.LWPOLYLINE->points[3].x - 72.0)
                 < 1e-6
          && fabs (copy->tio.entity->tio.LWPOLYLINE->points[3].y / 1e200
                   - 1.0)
                 < 1e-9)
        OK ("linear: a parameter moved far by another action extends");
      else
        {
          fail ("linear: a parameter moved far by another action");
          failures++;
        }
      pop_stretch_code (height_stretch);
    }

  // Width's stretch moves Height's end point, and Height chains its actions
  if (add_stretch_code (stretch, height->evalexpr.nodeid, 1))
    {
      height->chain_actions = 1;
      failures += test_refusal (dwg, WINDOW, &v, 1,
                                REFUSED (DWG_ERR_NOTYETSUPPORTED),
                                "a parameter chaining its actions");
      height->chain_actions = 0;
      pop_stretch_code (stretch);
    }

  stretch->xy_type = 1;
  failures += test_refusal (dwg, WINDOW, &v, 1,
                            REFUSED (DWG_ERR_NOTYETSUPPORTED),
                            "an action with an XY type");
  stretch->xy_type = 0;

  {
    BITCODE_T name = stretch->conn_pts[0].name;
    stretch->conn_pts[0].name = (char *)"UpdatedX";
    failures += test_refusal (dwg, WINDOW, &v, 1,
                              REFUSED (DWG_ERR_NOTYETSUPPORTED),
                              "an action taking another point's change");
    stretch->conn_pts[0].name = name;
  }

  {
    const BITCODE_BL index = stretch->hdls[0].indexes[0];
    stretch->hdls[0].indexes[0] = 9;
    failures += test_refusal (dwg, WINDOW, &v, 1, REFUSED (0),
                              "a stretch point an entity doesn't have");
    stretch->hdls[0].indexes[0] = index;
  }

  {
    // the INSERT of placement W1, in model space
    BITCODE_H hdl = stretch->hdls[0].hdl;
    stretch->hdls[0].hdl = dwg_add_handleref (dwg, 4, 0xC8D0, NULL);
    failures += test_refusal (dwg, WINDOW, &v, 1,
                              REFUSED (DWG_ERR_INVALIDHANDLE),
                              "a stretch of another block's entity");
    stretch->hdls[0].hdl = hdl;
  }

  // the frame, BBD3, which 'Stretch Width' stretches, along X
  {
    Dwg_Entity_LWPOLYLINE *_frame = frame->tio.entity->tio.LWPOLYLINE;
    const BITCODE_BE extrusion = _frame->extrusion;
    _frame->extrusion.x = 1.0;
    _frame->extrusion.y = 0.0;
    _frame->extrusion.z = 0.0;
    failures += test_refusal (dwg, WINDOW, &v, 1,
                              REFUSED (DWG_ERR_NOTYETSUPPORTED),
                              "a stretch out of an LWPOLYLINE's plane");
    _frame->extrusion = extrusion;
  }
  return failures;
}

/* The Door's stretch actions at angle offsets of 30, 45, 60 and 90 degrees
   stretch its leaves in the open states. The Door Size placement C15A
   also drives the scale action ScaleArc, so the evaluator refuses it; with
   ScaleArc disconnected, every entity outside its selection matches
   AutoCAD's block of C15A. */
static int
test_angle_offset (Dwg_Data *dwg)
{
  Dwg_Object *scale = graph_node (dwg, DOOR, DWG_TYPE_BLOCKSCALEACTION,
                                  "ScaleArc");
  const Dwg_Object_BLOCK_HEADER *acad = block_header (dwg, 0xBAFC);
  Dwg_Object_BLOCKSCALEACTION *_scale;
  Dwg_Object_BLOCK_HEADER *rep;
  Dwg_DynValue v;
  BITCODE_BL codes[5], compared = 0;
  int failures = 0;
  memset (&v, 0, sizeof (v));
  if (!scale || !acad)
    {
      fail ("angle offset: fixture objects not found");
      return 1;
    }
  _scale = scale->tio.object->tio.BLOCKSCALEACTION;
  for (int i = 0; i < 5; i++)
    {
      codes[i] = _scale->conn_pts[i].code;
      _scale->conn_pts[i].code = 0;
    }
  v.name = (char *)"Door Size";
  v.value = 40.0;
  rep = evaluate (dwg, DOOR, &v, 1, "angle offset");
  for (int i = 0; i < 5; i++)
    _scale->conn_pts[i].code = codes[i];
  if (!rep)
    return 1;
  for (BITCODE_BL i = 0; acad->entities && i < acad->num_owned; i++)
    {
      const Dwg_Object *a = dwg_ref_object (dwg, acad->entities[i]);
      BITCODE_RL idx = 0;
      BITCODE_RLL self = 0;
      const Dwg_Object *src, *b;
      int scaled = 0;
      if (!a || !etag (dwg, a, &idx, &self))
        continue;
      src = rep_entity (dwg, DOOR, idx);
      for (BITCODE_BL j = 0; src && _scale->deps && j < _scale->num_deps; j++)
        scaled |= _scale->deps[j]->absolute_ref == src->handle.value;
      if (scaled)
        continue;
      b = rep_entity (dwg, block_handle (rep), idx);
      if (!b || b->fixedtype != a->fixedtype
          || (a->fixedtype != DWG_TYPE_LINE
              && a->fixedtype != DWG_TYPE_LWPOLYLINE))
        {
          fail ("angle offset: AutoCAD's %s " FORMAT_HV
                " (index %u) has no LINE or LWPOLYLINE to compare",
                dwg_type_name (a->fixedtype), a->handle.value, (unsigned)idx);
          failures++;
          continue;
        }
      if (a->fixedtype == DWG_TYPE_LINE)
        {
          const Dwg_Entity_LINE *la = a->tio.entity->tio.LINE;
          const Dwg_Entity_LINE *lb = b->tio.entity->tio.LINE;
          if (!is_point (lb->start.x, lb->start.y, la->start.x, la->start.y)
              || !is_point (lb->end.x, lb->end.y, la->end.x, la->end.y))
            {
              fail ("angle offset: LINE " FORMAT_HV " differs",
                    a->handle.value);
              failures++;
            }
        }
      else
        {
          const Dwg_Entity_LWPOLYLINE *pa = a->tio.entity->tio.LWPOLYLINE;
          const Dwg_Entity_LWPOLYLINE *pb = b->tio.entity->tio.LWPOLYLINE;
          int same = pa->num_points == pb->num_points;
          for (BITCODE_BL j = 0; same && j < pa->num_points; j++)
            same = is_point (pb->points[j].x, pb->points[j].y,
                             pa->points[j].x, pa->points[j].y);
          if (!same)
            {
              fail ("angle offset: LWPOLYLINE " FORMAT_HV " differs",
                    a->handle.value);
              failures++;
            }
        }
      compared++;
    }
  // the 5 leaves and the 2 jamb lines
  if (!failures && compared == 7)
    OK ("angle offset: Door Size 40 matches AutoCAD's 7 lines and leaves");
  else if (!failures)
    {
      fail ("angle offset: %u entities compared, expected 7",
            (unsigned)compared);
      failures++;
    }
  return failures;
}

/* An entity added to definition def, for the evaluation to move: its handle,
   and the stretch points to list. */
typedef struct
{
  BITCODE_RLL handle;
  BITCODE_BL num_points;
  const char *what;
} Added;

static Added
added (Dwg_Data *dwg, const Dwg_Object_Entity *ent, const BITCODE_BL n,
       const char *what)
{
  Added a;
  a.handle = ent ? entity_handle (dwg, ent) : 0;
  a.num_points = n;
  a.what = what;
  return a;
}

/* Whether text b, a copy of a TEXT, ATTDEF or ATTRIB a, moved by dx, dy:
   its insertion point, and the alignment point of text that isn't
   left-aligned on the baseline, with dataflags 0x02 set only for an
   alignment point of (0,0), as AutoCAD writes it. Left-aligned text keeps
   its alignment point and flag. */
#define IS_MOVED_TEXT(a, b, dx, dy)                                           \
  (is_point ((b)->ins_pt.x, (b)->ins_pt.y, (a)->ins_pt.x + (dx),              \
             (a)->ins_pt.y + (dy))                                            \
   && ((a)->horiz_alignment || (a)->vert_alignment                            \
           ? is_point ((b)->alignment_pt.x, (b)->alignment_pt.y,              \
                       (a)->alignment_pt.x + (dx), (a)->alignment_pt.y + (dy)) \
                 && !((b)->dataflags & 0x02)                                  \
                        == ((b)->alignment_pt.x != 0.0                        \
                            || (b)->alignment_pt.y != 0.0)                    \
           : is_point ((b)->alignment_pt.x, (b)->alignment_pt.y,              \
                       (a)->alignment_pt.x, (a)->alignment_pt.y)              \
                 && ((b)->dataflags & 0x02) == ((a)->dataflags & 0x02)))

/* Whether copy, of src, moved by dx, dy in its own coordinates: every
   point of it, and the ATTRIBs of an INSERT. */
static int
is_moved (Dwg_Data *dwg, const Dwg_Object *src, const Dwg_Object *copy,
          const double dx, const double dy)
{
  if (!src || !copy || src->fixedtype != copy->fixedtype)
    return 0;
  switch (src->fixedtype)
    {
    case DWG_TYPE_LINE:
      {
        const Dwg_Entity_LINE *a = src->tio.entity->tio.LINE;
        const Dwg_Entity_LINE *b = copy->tio.entity->tio.LINE;
        return is_point (b->start.x, b->start.y, a->start.x + dx,
                         a->start.y + dy)
               && is_point (b->end.x, b->end.y, a->end.x + dx, a->end.y + dy);
      }
    case DWG_TYPE_LWPOLYLINE:
      {
        const Dwg_Entity_LWPOLYLINE *a = src->tio.entity->tio.LWPOLYLINE;
        const Dwg_Entity_LWPOLYLINE *b = copy->tio.entity->tio.LWPOLYLINE;
        int same = a->num_points == b->num_points;
        for (BITCODE_BL i = 0; same && i < a->num_points; i++)
          same = is_point (b->points[i].x, b->points[i].y,
                           a->points[i].x + dx, a->points[i].y + dy);
        return same;
      }
    case DWG_TYPE_ARC:
      return is_point (copy->tio.entity->tio.ARC->center.x,
                       copy->tio.entity->tio.ARC->center.y,
                       src->tio.entity->tio.ARC->center.x + dx,
                       src->tio.entity->tio.ARC->center.y + dy);
    case DWG_TYPE_CIRCLE:
      return is_point (copy->tio.entity->tio.CIRCLE->center.x,
                       copy->tio.entity->tio.CIRCLE->center.y,
                       src->tio.entity->tio.CIRCLE->center.x + dx,
                       src->tio.entity->tio.CIRCLE->center.y + dy);
    case DWG_TYPE_ELLIPSE:
      return is_point (copy->tio.entity->tio.ELLIPSE->center.x,
                       copy->tio.entity->tio.ELLIPSE->center.y,
                       src->tio.entity->tio.ELLIPSE->center.x + dx,
                       src->tio.entity->tio.ELLIPSE->center.y + dy);
    case DWG_TYPE_SPLINE:
      {
        const Dwg_Entity_SPLINE *a = src->tio.entity->tio.SPLINE;
        const Dwg_Entity_SPLINE *b = copy->tio.entity->tio.SPLINE;
        int same = a->num_fit_pts == b->num_fit_pts && a->num_fit_pts;
        for (BITCODE_BS i = 0; same && i < a->num_fit_pts; i++)
          same = is_point (b->fit_pts[i].x, b->fit_pts[i].y,
                           a->fit_pts[i].x + dx, a->fit_pts[i].y + dy);
        return same;
      }
    case DWG_TYPE_TEXT:
      return IS_MOVED_TEXT (src->tio.entity->tio.TEXT,
                            copy->tio.entity->tio.TEXT, dx, dy);
    case DWG_TYPE_ATTDEF:
      return IS_MOVED_TEXT (src->tio.entity->tio.ATTDEF,
                            copy->tio.entity->tio.ATTDEF, dx, dy);
    case DWG_TYPE_ATTRIB:
      return IS_MOVED_TEXT (src->tio.entity->tio.ATTRIB,
                            copy->tio.entity->tio.ATTRIB, dx, dy);
    case DWG_TYPE_MTEXT:
      return is_point (copy->tio.entity->tio.MTEXT->ins_pt.x,
                       copy->tio.entity->tio.MTEXT->ins_pt.y,
                       src->tio.entity->tio.MTEXT->ins_pt.x + dx,
                       src->tio.entity->tio.MTEXT->ins_pt.y + dy);
    case DWG_TYPE_INSERT:
      {
        const Dwg_Entity_INSERT *a = src->tio.entity->tio.INSERT;
        const Dwg_Entity_INSERT *b = copy->tio.entity->tio.INSERT;
        int same = is_point (b->ins_pt.x, b->ins_pt.y, a->ins_pt.x + dx,
                             a->ins_pt.y + dy)
                   && a->num_owned == b->num_owned;
        for (BITCODE_BL i = 0; same && a->attribs && i < a->num_owned; i++)
          same = is_moved (dwg, dwg_ref_object (dwg, a->attribs[i]),
                           dwg_ref_object (dwg, b->attribs[i]), dx, dy);
        return same;
      }
    case DWG_TYPE_HATCH:
      {
        const Dwg_Entity_HATCH *a = src->tio.entity->tio.HATCH;
        const Dwg_Entity_HATCH *b = copy->tio.entity->tio.HATCH;
        return a->num_paths == 1 && b->num_paths == 1
               && a->paths[0].num_segs_or_paths == 1
               && b->paths[0].num_segs_or_paths == 1
               && is_point (b->paths[0].segs[0].center.x,
                            b->paths[0].segs[0].center.y,
                            a->paths[0].segs[0].center.x + dx,
                            a->paths[0].segs[0].center.y + dy);
      }
    default:
      return 0;
    }
}

/* Entities of each type the evaluator moves whole, added to the Window and
   listed with 'Stretch Width', with all their stretch points: at Window
   Width 24 they move by -12 in x. A mirrored CIRCLE, whose OCS x axis
   points the other way, moves by +12 in its OCS. Then what the evaluator
   refuses: part of an ARC, an entity of another type, a HATCH crossing the
   frame. */
static int
test_stretch_whole (Dwg_Data *dwg)
{
  Dwg_Object_BLOCK_HEADER *def = block_header (dwg, WINDOW);
  Dwg_Object_BLOCKSTRETCHACTION *stretch
      = stretch_action (dwg, WINDOW, "Stretch Width");
  static const BITCODE_BL points[] = { 0, 1, 2, 3, 4 };
  const dwg_point_2d pts[3]
      = { { 33.0, 34.0 }, { 34.0, 35.0 }, { 35.0, 34.0 } };
  const dwg_point_3d fit[3]
      = { { 33.0, 18.0, 0.0 }, { 34.0, 19.0, 0.0 }, { 35.0, 18.0, 0.0 } };
  const dwg_point_3d tan = { 1.0, 0.0, 0.0 };
  const dwg_point_3d line_end = { 34.0, 6.0, 0.0 };
  dwg_point_3d pt;
  Added list[16];
  int n = 0, failures = 0;
  Dwg_Entity_ARC *arc;
  Dwg_Entity_CIRCLE *circle, *mirrored, *boundary, *crossing;
  Dwg_Entity_ELLIPSE *ellipse, *elliptical_arc;
  Dwg_Entity_SPLINE *spline;
  Dwg_Entity_TEXT *text, *centred, *unstored;
  Dwg_Entity_ATTDEF *attdef;
  Dwg_Entity_MTEXT *mtext;
  Dwg_Entity_INSERT *insert;
  Dwg_Entity_ATTRIB *attrib;
  Dwg_Entity_LWPOLYLINE *lwpline;
  Dwg_Entity_LINE *line;
  Dwg_Entity_HATCH *hatch, *crossing_hatch;
  Dwg_Entity_POINT *point;
  Dwg_Object_BLOCK_HEADER *rep;
  Dwg_DynValue v;
  BITCODE_RLL h_mirrored;
  memset (&v, 0, sizeof (v));
  if (!def || !stretch)
    {
      fail ("stretch whole: fixture objects not found");
      return 1;
    }
  pt.x = 33.0;
  pt.y = 5.0;
  pt.z = 0.0;
  line = dwg_add_LINE (def, &pt, &line_end);
  pt.x = 35.0;
  pt.y = 8.0;
  arc = dwg_add_ARC (def, &pt, 0.5, 0.0, M_PI / 2.0);
  pt.y = 10.0;
  circle = dwg_add_CIRCLE (def, &pt, 0.5);
  // at x 35 in the block, -35 in its OCS
  pt.x = -35.0;
  pt.y = 12.0;
  mirrored = dwg_add_CIRCLE (def, &pt, 0.5);
  pt.x = 35.0;
  pt.y = 14.0;
  ellipse = dwg_add_ELLIPSE (def, &pt, 0.5, 0.5);
  pt.y = 16.0;
  elliptical_arc = dwg_add_ELLIPSE (def, &pt, 0.5, 0.5);
  spline = dwg_add_SPLINE (def, 3, fit, &tan, &tan);
  pt.y = 20.0;
  text = dwg_add_TEXT (def, "left", &pt, 1.0);
  pt.y = 21.0;
  unstored = dwg_add_TEXT (def, "unstored", &pt, 1.0);
  pt.y = 22.0;
  centred = dwg_add_TEXT (def, "centred", &pt, 1.0);
  pt.y = 24.0;
  attdef = dwg_add_ATTDEF (def, 1.0, 0, "Prompt", &pt, "TAG", "default");
  pt.y = 26.0;
  mtext = dwg_add_MTEXT (def, &pt, 2.0, "mtext");
  pt.y = 28.0;
  insert = dwg_add_INSERT (def, &pt, "t", 1.0, 1.0, 1.0, 0.0);
  attrib = insert ? dwg_add_ATTRIB (insert, 1.0, 0, &pt, "TAG", "value")
                  : NULL;
  pt.y = 31.0;
  boundary = dwg_add_CIRCLE (def, &pt, 0.5);
  lwpline = dwg_add_LWPOLYLINE (def, 3, pts);
  // crossing the frame's right edge, at x 39
  pt.x = 39.0;
  pt.y = 31.0;
  crossing = dwg_add_CIRCLE (def, &pt, 1.0);
  pt.x = 35.0;
  pt.y = 36.0;
  point = dwg_add_POINT (def, &pt);
  loglevel = test_loglevel;
  if (!line || !arc || !circle || !mirrored || !ellipse || !elliptical_arc
      || !spline || !text || !unstored || !centred || !attdef || !mtext
      || !insert || !attrib || !boundary || !lwpline || !crossing || !point)
    {
      fail ("stretch whole: could not add the entities");
      return 1;
    }
  {
    const Dwg_Object *pathobjs[1];
    pathobjs[0] = &dwg->object[boundary->parent->objid];
    hatch = dwg_add_HATCH (def, 1, "SOLID", 0, 1, pathobjs);
    pathobjs[0] = &dwg->object[crossing->parent->objid];
    crossing_hatch = dwg_add_HATCH (def, 1, "SOLID", 0, 1, pathobjs);
    loglevel = test_loglevel;
    if (!hatch || !crossing_hatch)
      {
        fail ("stretch whole: could not add the HATCHes");
        return 1;
      }
    hatch->extrusion.z = crossing_hatch->extrusion.z = 1.0;
  }
  mirrored->extrusion.z = -1.0;
  elliptical_arc->start_angle = 0.0;
  elliptical_arc->end_angle = M_PI;
  spline->scenario = 2;
  // left-aligned, as AutoCAD writes it: alignment point (0,0), not stored.
  // It stays so in the copy.
  text->dataflags = 0x02;
  attdef->dataflags = 0x02;
  centred->horiz_alignment = 1;
  centred->alignment_pt.x = 35.5;
  centred->alignment_pt.y = 22.0;
  // centred on (0,0), which AutoCAD doesn't store (dataflags 0x02): the
  // copy's alignment point moves with it, and is then stored
  unstored->horiz_alignment = 1;
  unstored->dataflags = 0x02;
  // the same for a middle-aligned ATTRIB, moved with its INSERT
  attrib->horiz_alignment = 4;
  attrib->dataflags = 0x02;

  list[n++] = added (dwg, line->parent, 2, "LINE");
  list[n++] = added (dwg, arc->parent, 2, "ARC");
  list[n++] = added (dwg, circle->parent, 1, "CIRCLE");
  list[n++] = added (dwg, ellipse->parent, 5, "ELLIPSE");
  list[n++] = added (dwg, elliptical_arc->parent, 4, "elliptical arc");
  list[n++] = added (dwg, spline->parent, 3, "SPLINE");
  list[n++] = added (dwg, text->parent, 1, "TEXT");
  list[n++] = added (dwg, centred->parent, 1, "centred TEXT");
  list[n++] = added (dwg, unstored->parent, 1,
                     "centred TEXT with no alignment point stored");
  list[n++] = added (dwg, attdef->parent, 1, "ATTDEF");
  list[n++] = added (dwg, mtext->parent, 5, "MTEXT");
  list[n++] = added (dwg, insert->parent, 1, "INSERT with an ATTRIB");
  list[n++] = added (dwg, boundary->parent, 1, "HATCH's CIRCLE");
  list[n++] = added (dwg, hatch->parent, 1, "HATCH");
  list[n++] = added (dwg, lwpline->parent, 3, "LWPOLYLINE");
  for (int i = 0; i < n; i++)
    if (!add_stretch_entity (dwg, stretch, list[i].handle, points,
                             (BITCODE_BS)list[i].num_points))
      {
        fail ("stretch whole: out of memory");
        return 1;
      }
  if (!add_stretch_entity (dwg, stretch, entity_handle (dwg, mirrored->parent),
                           points, 1))
    {
      fail ("stretch whole: out of memory");
      return 1;
    }

  v.name = (char *)"Window Width";
  v.value = 24.0;
  rep = evaluate (dwg, WINDOW, &v, 1, "stretch whole");
  if (!rep)
    return 1;
  for (int i = 0; i < n; i++)
    {
      const Dwg_Object *src = dwg_resolve_handle (dwg, list[i].handle);
      if (is_moved (dwg, src, copy_of (dwg, def, rep, list[i].handle), -12.0,
                    0.0))
        OK ("stretch whole: the %s moves by -12", list[i].what);
      else
        {
          fail ("stretch whole: the %s doesn't move by -12", list[i].what);
          failures++;
        }
    }
  h_mirrored = entity_handle (dwg, mirrored->parent);
  if (is_moved (dwg, dwg_resolve_handle (dwg, h_mirrored),
                copy_of (dwg, def, rep, h_mirrored), 12.0, 0.0))
    OK ("stretch whole: the mirrored CIRCLE moves by +12 in its OCS");
  else
    {
      fail ("stretch whole: the mirrored CIRCLE doesn't move by +12 in its "
            "OCS");
      failures++;
    }

  // part of an ARC
  if (add_stretch_entity (dwg, stretch, list[1].handle, points, 1))
    {
      failures += test_refusal (dwg, WINDOW, &v, 1,
                                REFUSED (DWG_ERR_NOTYETSUPPORTED),
                                "a stretch of part of an ARC");
      pop_stretch_entity (stretch);
    }
  if (add_stretch_entity (dwg, stretch, entity_handle (dwg, point->parent),
                          points, 1))
    {
      failures += test_refusal (dwg, WINDOW, &v, 1,
                                REFUSED (DWG_ERR_NOTYETSUPPORTED),
                                "a stretch of a POINT");
      pop_stretch_entity (stretch);
    }
  if (add_stretch_entity (dwg, stretch,
                          entity_handle (dwg, crossing_hatch->parent), points,
                          1))
    {
      failures += test_refusal (dwg, WINDOW, &v, 1,
                                REFUSED (DWG_ERR_NOTYETSUPPORTED),
                                "a HATCH crossing the frame");
      pop_stretch_entity (stretch);
    }
  return failures;
}

/* AutoCAD moves whole an entity or parameter of a stretch action's
   selection that lies wholly outside its frame, with none of its points
   listed; the evaluator refuses one, before adding anything. Here a LINE
   added to the Window outside 'Stretch Width''s frame joins the action's
   selection with no stretch point listed, or listed with none; and the
   action lists Window Height with none of its points. The parameter and
   grip the fixture's selection holds are allowed, as test_linear shows. */
static int
test_stretch_selection (Dwg_Data *dwg)
{
  Dwg_Object_BLOCK_HEADER *def = block_header (dwg, WINDOW);
  Dwg_Object_BLOCKSTRETCHACTION *stretch
      = stretch_action (dwg, WINDOW, "Stretch Width");
  Dwg_Object_BLOCKLINEARPARAMETER *height
      = linear_param (dwg, WINDOW, "Window Height");
  const dwg_point_3d start = { 50.0, 0.0, 0.0 };
  const dwg_point_3d end = { 50.0, 10.0, 0.0 };
  Dwg_Entity_LINE *line;
  BITCODE_RLL h_line;
  Dwg_DynValue v;
  int failures = 0;
  memset (&v, 0, sizeof (v));
  if (!def || !stretch || !height)
    {
      fail ("stretch selection: fixture objects not found");
      return 1;
    }
  line = dwg_add_LINE (def, &start, &end);
  loglevel = test_loglevel;
  if (!line)
    {
      fail ("stretch selection: could not add the LINE");
      return 1;
    }
  h_line = entity_handle (dwg, line->parent);
  v.name = (char *)"Window Width";
  v.value = 24.0;

  if (!add_dep (dwg, &stretch->deps, &stretch->num_deps, h_line))
    {
      fail ("stretch selection: out of memory");
      return 1;
    }
  failures += test_refusal (dwg, WINDOW, &v, 1,
                            REFUSED (DWG_ERR_NOTYETSUPPORTED),
                            "a selected LINE with no stretch point listed");
  if (add_stretch_entity (dwg, stretch, h_line, NULL, 0))
    {
      failures += test_refusal (dwg, WINDOW, &v, 1,
                                REFUSED (DWG_ERR_NOTYETSUPPORTED),
                                "a selected LINE listed with no stretch "
                                "point");
      pop_stretch_entity (stretch);
    }
  else
    {
      fail ("stretch selection: out of memory");
      failures++;
    }
  stretch->num_deps--;
  // listed with no stretch point, though not selected
  if (add_stretch_entity (dwg, stretch, h_line, NULL, 0))
    {
      failures += test_refusal (dwg, WINDOW, &v, 1,
                                REFUSED (DWG_ERR_NOTYETSUPPORTED),
                                "a LINE listed with no stretch point");
      pop_stretch_entity (stretch);
    }
  if (add_stretch_code (stretch, height->evalexpr.nodeid, NO_POINT))
    {
      failures += test_refusal (dwg, WINDOW, &v, 1,
                                REFUSED (DWG_ERR_NOTYETSUPPORTED),
                                "a selected parameter with none of its "
                                "points listed");
      pop_stretch_code (stretch);
    }
  // the Window's extension dictionary, neither an entity of it nor a node
  if (add_dep (dwg, &stretch->deps, &stretch->num_deps,
               def->parent->xdicobjhandle->absolute_ref))
    {
      failures += test_refusal (dwg, WINDOW, &v, 1,
                                REFUSED (DWG_ERR_INVALIDHANDLE),
                                "a stretch selecting an object outside the "
                                "definition");
      stretch->num_deps--;
    }
  if (evaluate (dwg, WINDOW, &v, 1, "stretch selection, restored"))
    OK ("stretch selection: the Window evaluates again as AutoCAD lists it");
  else
    failures++;
  return failures;
}

/* The Door's move action MoveSwing moves its selection by half Wall
   Thickness's change: entities added to it, at Wall Thickness 6, move by
   -0.5 in y. A POINT in it is refused. */
static int
test_move (Dwg_Data *dwg)
{
  Dwg_Object_BLOCK_HEADER *def = block_header (dwg, DOOR);
  Dwg_Object_BLOCKMOVEACTION *move = move_action (dwg, DOOR, "MoveSwing");
  dwg_point_3d pt = { 10.0, -2.0, 0.0 };
  const dwg_point_3d mc_pt = { -2.3, -1.25, 0.0 };
  Dwg_Entity_TEXT *text, *centred;
  Dwg_Entity_INSERT *insert;
  Dwg_Entity_POINT *point;
  Dwg_Object_BLOCK_HEADER *rep;
  Dwg_DynValue v;
  BITCODE_RLL h_text, h_centred, h_insert;
  int failures = 0;
  memset (&v, 0, sizeof (v));
  if (!def || !move)
    {
      fail ("move: fixture objects not found");
      return 1;
    }
  text = dwg_add_TEXT (def, "caption", &pt, 1.0);
  centred = dwg_add_TEXT (def, "MC", &mc_pt, 2.5);
  insert = dwg_add_INSERT (def, &pt, "t", 1.0, 1.0, 1.0, 0.0);
  if (insert && !dwg_add_ATTRIB (insert, 1.0, 0, &pt, "TAG", "value"))
    insert = NULL;
  point = dwg_add_POINT (def, &pt);
  loglevel = test_loglevel;
  if (!text || !centred || !insert || !point)
    {
      fail ("move: could not add the entities");
      return 1;
    }
  text->dataflags = 0x02;
  // middle-centred on the block's base point, as AutoCAD writes it: the
  // alignment point (0,0) isn't stored
  centred->horiz_alignment = 1;
  centred->vert_alignment = 2;
  centred->dataflags = 0x3f;
  h_text = entity_handle (dwg, text->parent);
  h_centred = entity_handle (dwg, centred->parent);
  h_insert = entity_handle (dwg, insert->parent);
  if (!add_move_entity (dwg, move, h_text)
      || !add_move_entity (dwg, move, h_centred)
      || !add_move_entity (dwg, move, h_insert))
    {
      fail ("move: out of memory");
      return 1;
    }
  v.name = (char *)"Wall Thickness";
  v.value = 6.0;
  rep = evaluate (dwg, DOOR, &v, 1, "move");
  if (rep
      && is_moved (dwg, dwg_resolve_handle (dwg, h_text),
                   copy_of (dwg, def, rep, h_text), 0.0, -0.5)
      && is_moved (dwg, dwg_resolve_handle (dwg, h_insert),
                   copy_of (dwg, def, rep, h_insert), 0.0, -0.5))
    OK ("move: the TEXT and the INSERT with its ATTRIB move by -0.5");
  else
    {
      fail ("move: the TEXT and the INSERT don't move by -0.5");
      failures++;
    }
  if (rep
      && is_moved (dwg, dwg_resolve_handle (dwg, h_centred),
                   copy_of (dwg, def, rep, h_centred), 0.0, -0.5))
    OK ("move: the TEXT centred on (0,0) moves its alignment point by -0.5, "
        "then stored");
  else
    {
      fail ("move: the TEXT centred on (0,0) doesn't move its alignment "
            "point by -0.5, stored");
      failures++;
    }
  if (add_move_entity (dwg, move, entity_handle (dwg, point->parent)))
    {
      failures += test_refusal (dwg, DOOR, &v, 1,
                                REFUSED (DWG_ERR_NOTYETSUPPORTED),
                                "a move of a POINT");
      move->num_deps--;
    }
  // the Door's extension dictionary, neither an entity of it nor a node
  if (add_move_entity (dwg, move, def->parent->xdicobjhandle->absolute_ref))
    {
      failures += test_refusal (dwg, DOOR, &v, 1,
                                REFUSED (DWG_ERR_INVALIDHANDLE),
                                "a move of an object outside the definition");
      move->num_deps--;
    }
  return failures;
}

/* Appends node id nodeid to the parameters move action a moves. */
static int
add_move_node (Dwg_Object_BLOCKMOVEACTION *a, const BITCODE_BL nodeid)
{
  BITCODE_BL *actions = (BITCODE_BL *)realloc (
      a->actions, (a->num_actions + 1) * sizeof (BITCODE_BL));
  if (!actions)
    return 0;
  a->actions = actions;
  actions[a->num_actions++] = nodeid;
  return 1;
}

/* The nodes of the Door's graph that its actions select: an id the graph
   doesn't have is refused, and a graph entry that is no element of the
   graph, here its PLACEHOLDER, is not taken for one. A parameter of
   another kind than linear that an action moves, and that chains its
   actions, would run them: here the Door's flip parameter 'swing', which
   MoveSwing moves, and which 'Stretch' is made to move. */
static int
test_selected_nodes (Dwg_Data *dwg)
{
  Dwg_Object *swing
      = graph_node (dwg, DOOR, DWG_TYPE_BLOCKFLIPPARAMETER, "swing");
  Dwg_Object_BLOCKMOVEACTION *move = move_action (dwg, DOOR, "MoveSwing");
  Dwg_Object_BLOCKSTRETCHACTION *stretch
      = stretch_action (dwg, DOOR, "Stretch");
  Dwg_Object_EVALUATION_GRAPH *graph = definition_graph (dwg, DOOR);
  Dwg_Object *placeholder = NULL;
  Dwg_Object_BLOCKFLIPPARAMETER *_swing;
  Dwg_DynValue v;
  int failures = 0;
  memset (&v, 0, sizeof (v));
  for (BITCODE_BL i = 0; i < dwg->num_objects && !placeholder; i++)
    if (dwg->object[i].fixedtype == DWG_TYPE_PLACEHOLDER)
      placeholder = &dwg->object[i];
  if (!swing || !move || !stretch || !graph || !graph->nodes
      || !placeholder)
    {
      fail ("selected nodes: fixture objects not found");
      return 1;
    }
  v.name = (char *)"Wall Thickness";
  v.value = 6.0;

  if (add_stretch_code (stretch, 99999, 1))
    {
      failures += test_refusal (dwg, DOOR, &v, 1,
                                REFUSED (DWG_ERR_INVALIDHANDLE),
                                "a stretch of a node the graph doesn't have");
      pop_stretch_code (stretch);
    }
  if (add_move_node (move, 99999))
    {
      failures += test_refusal (dwg, DOOR, &v, 1,
                                REFUSED (DWG_ERR_INVALIDHANDLE),
                                "a move of a node the graph doesn't have");
      move->num_actions--;
    }

  // a flip grip, made the PLACEHOLDER
  {
    BITCODE_BL grip = graph->num_nodes;
    for (BITCODE_BL i = 0; i < graph->num_nodes && grip == graph->num_nodes;
         i++)
      {
        const Dwg_Object *obj = dwg_ref_object (dwg, graph->nodes[i].evalexpr);
        if (obj && obj->fixedtype == DWG_TYPE_BLOCKFLIPGRIP)
          grip = i;
      }
    if (grip < graph->num_nodes)
      {
        BITCODE_H node = graph->nodes[grip].evalexpr;
        graph->nodes[grip].evalexpr
            = dwg_add_handleref (dwg, 4, placeholder->handle.value, NULL);
        if (evaluate (dwg, DOOR, &v, 1, "a graph entry that is no element"))
          OK ("evaluate takes a PLACEHOLDER in the graph for no element");
        else
          failures++;
        graph->nodes[grip].evalexpr = node;
      }
    else
      {
        fail ("selected nodes: the Door has no flip grip");
        failures++;
      }
  }

  _swing = swing->tio.object->tio.BLOCKFLIPPARAMETER;
  _swing->chain_actions = 1;
  failures += test_refusal (dwg, DOOR, &v, 1,
                            REFUSED (DWG_ERR_NOTYETSUPPORTED),
                            "a move of a flip parameter chaining its actions");
  if (add_stretch_code (stretch, _swing->evalexpr.nodeid, 1))
    {
      // MoveSwing then moves nothing that chains
      const BITCODE_BL num_actions = move->num_actions;
      move->num_actions = 0;
      failures += test_refusal (
          dwg, DOOR, &v, 1, REFUSED (DWG_ERR_NOTYETSUPPORTED),
          "a stretch of a flip parameter chaining its actions");
      move->num_actions = num_actions;
      pop_stretch_code (stretch);
    }
  _swing->chain_actions = 0;
  return failures;
}

/* What the check reports for each placement of the fixture: the
   definition, and the number of entities compared. AutoCAD computed the
   placements at the defaults, the visibility ones and the linear ones,
   which the evaluator matches; the others change kinds it refuses, the
   Door's Door Size drives a scale action, and Door Elevation is
   constraint-based. */
typedef struct
{
  BITCODE_RLL insert;
  BITCODE_RLL block;
  Dwg_DynBlock_Check_Status status;
  BITCODE_BL kinds;
  BITCODE_RLL definition;
  BITCODE_BL num_entities;
} Expected_Check;

#define KIND(k) (1U << DWG_DYNPARAM_##k)
#define MATCH DWG_DYNBLOCK_CHECK_MATCH
#define UNSUPPORTED DWG_DYNBLOCK_CHECK_UNSUPPORTED

static const Expected_Check expected_checks[] = {
  { 0xBFD0, 0x9D5E, MATCH, KIND (VISIBILITY), ALTEZZA, 1058 },
  { 0xBFD8, 0xAC8C, MATCH, KIND (VISIBILITY), ALTEZZA, 1058 },
  { 0xC145, 0xB6AC, MATCH, 0, TREES, 582 },
  { 0xC14C, 0xB9E8, MATCH, KIND (VISIBILITY), TOILET, 215 },
  { 0xC15A, 0xBAFC, UNSUPPORTED, KIND (LINEAR), DOOR, 0 },
  { 0xC174, 0xBB72, UNSUPPORTED, 0, DOOR_ELEVATION, 0 },
  { 0xC17F, 0xBBDC, MATCH, 0, WINDOW, 10 },
  { 0xC8D0, 0xC96E, MATCH, KIND (LINEAR), WINDOW, 10 },
  { 0xC8D1, 0xC98C, MATCH, KIND (LINEAR), WINDOW, 10 },
  { 0xC8D2, 0xC9AA, MATCH, KIND (LINEAR), WINDOW, 10 },
  { 0xC8D3, 0xC9C8, MATCH, KIND (LINEAR), WINDOW, 10 },
  { 0xC8D5, 0xC9F2, MATCH, KIND (LINEAR), DOOR, 11 },
  { 0xC8D6, 0xCA2F, MATCH, KIND (LINEAR), DOOR, 11 },
  { 0xC8D7, 0xCA6C, MATCH, KIND (VISIBILITY), DOOR, 11 },
  { 0xC8D8, 0xCAA9, MATCH, KIND (LINEAR) | KIND (VISIBILITY), DOOR, 11 },
  { 0xC8DA, 0xCB0F, MATCH, KIND (VISIBILITY), TOILET, 215 },
  { 0xC8DB, 0xCC00, UNSUPPORTED, KIND (ROTATION) | KIND (VISIBILITY), TOILET,
    0 },
  { 0xC8DC, 0xCD02, UNSUPPORTED, KIND (ROTATION) | KIND (VISIBILITY), TOILET,
    0 },
  { 0xC8DD, 0xCE04, UNSUPPORTED, KIND (ROTATION) | KIND (VISIBILITY), TOILET,
    0 },
  { 0xC8DE, 0xCF06, UNSUPPORTED, KIND (ROTATION), TOILET, 0 },
  { 0xC8E0, 0xCFF7, MATCH, KIND (VISIBILITY), TREES, 582 },
  { 0xC8E1, 0xD24A, MATCH, KIND (VISIBILITY), TREES, 582 },
  { 0xC8E2, 0xD49D, MATCH, KIND (VISIBILITY), TREES, 582 },
};

#define NUM_CHECKS (sizeof (expected_checks) / sizeof (expected_checks[0]))

/* The number of expected checks with the given status. */
static BITCODE_BL
expected (const Dwg_DynBlock_Check_Status status)
{
  BITCODE_BL n = 0;
  for (size_t i = 0; i < NUM_CHECKS; i++)
    n += expected_checks[i].status == status;
  return n;
}

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
  Dwg_DynBlock_Check_Report *report = dwg_dynblock_check (dwg, TOLERANCE);
  int failures = 0;
  loglevel = test_loglevel;
  if (!report)
    {
      fail ("check: no report");
      return 1;
    }
  if (report->num_checks != NUM_CHECKS
      || report->num_match != expected (MATCH)
      || report->num_unsupported != expected (UNSUPPORTED)
      || report->num_mismatch || report->num_no_values || report->num_failed)
    {
      fail ("check: %u checks: %u match, %u mismatch, %u unsupported, %u no "
            "values, %u failed; expected %u: %u match, %u unsupported",
            (unsigned)report->num_checks, (unsigned)report->num_match,
            (unsigned)report->num_mismatch, (unsigned)report->num_unsupported,
            (unsigned)report->num_no_values, (unsigned)report->num_failed,
            (unsigned)NUM_CHECKS, (unsigned)expected (MATCH),
            (unsigned)expected (UNSUPPORTED));
      failures++;
    }
  for (size_t i = 0; i < NUM_CHECKS; i++)
    {
      const Expected_Check *e = &expected_checks[i];
      const Dwg_DynBlock_Check *c = find_check (report, e->insert, e->block);
      if (!c || c->status != e->status || c->kinds != e->kinds
          || c->definition != e->definition
          || c->num_entities != e->num_entities
          || (e->status == MATCH
              && (c->num_differing || c->num_equivalent || c->reason
                  || !c->evaluated))
          || (e->status != MATCH && !c->reason))
        {
          fail ("check " FORMAT_HV ": status %d, kinds 0x%x, definition "
                FORMAT_HV ", %u entities, %u differ: %s; expected status %d, "
                "kinds 0x%x, " FORMAT_HV ", %u entities",
                e->insert, c ? (int)c->status : -1, c ? (unsigned)c->kinds : 0,
                c ? c->definition : 0, c ? (unsigned)c->num_entities : 0,
                c ? (unsigned)c->num_differing : 0,
                c && c->reason ? c->reason : "", (int)e->status,
                (unsigned)e->kinds, e->definition,
                (unsigned)e->num_entities);
          failures++;
        }
      else
        OK ("check " FORMAT_HV ": %s, %u entities%s%s", e->insert,
            e->status == MATCH ? "match" : "unsupported",
            (unsigned)c->num_entities, c->reason ? ": " : "",
            c->reason ? c->reason : "");
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
  Dwg_Object_BLOCK_HEADER *c17f_block = block_header (dwg, 0xBBDC);
  Dwg_Object_BLOCK_HEADER *bfd8_block = block_header (dwg, 0xAC8C);
  Dwg_DynBlock_Check_Report *report;
  const Dwg_DynBlock_Check *c;
  int failures = 0;
  if (!line || !hidden || !ellipse || !r2 || !r3 || !c17f_block
      || !c17f_block->entities || !c17f_block->num_owned || !bfd8_block)
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
  c17f_block->entities[0] = dwg_add_handleref (dwg, 3, 0xFFFFFF, NULL);
  // BFD8's block: its base point
  bfd8_block->base_pt.x += 1.0;
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
    OK ("check finds a moved LINE: %s", c->reason);
  else
    {
      fail ("check of a moved LINE: status %d, %s", c ? (int)c->status : -1,
            c && c->reason ? c->reason : "");
      failures++;
    }
  c = find_check (report, 0xC8DA, 0xCB0F);
  if (c && c->status == DWG_DYNBLOCK_CHECK_MISMATCH && c->num_differing == 1
      && c->reason && strstr (c->reason, "invisible"))
    OK ("check finds a visibility flag: %s", c->reason);
  else
    {
      fail ("check of a visibility flag: status %d, %s",
            c ? (int)c->status : -1, c && c->reason ? c->reason : "");
      failures++;
    }
  c = find_check (report, 0xC17F, 0xBBDC);
  if (c && c->status == DWG_DYNBLOCK_CHECK_MISMATCH && c->reason
      && strstr (c->reason, "doesn't resolve"))
    OK ("check finds an entity that doesn't resolve: %s", c->reason);
  else
    {
      fail ("check of an entity that doesn't resolve: status %d, %s",
            c ? (int)c->status : -1, c && c->reason ? c->reason : "");
      failures++;
    }
  c = find_check (report, 0xBFD8, 0xAC8C);
  if (c && c->status == DWG_DYNBLOCK_CHECK_MISMATCH && !c->num_differing
      && c->reason && strstr (c->reason, "base_pt"))
    OK ("check finds a moved base point: %s", c->reason);
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
    OK ("check finds a transparency: %s", c->reason);
  else
    {
      fail ("check of a transparency: status %d, %s", c ? (int)c->status : -1,
            c && c->reason ? c->reason : "");
      failures++;
    }
  c = find_check (report, 0xBFD0, 0x9D5E);
  if (c && c->status == DWG_DYNBLOCK_CHECK_MATCH && c->num_equivalent == 1)
    OK ("check matches a reversed ELLIPSE as the same curve");
  else
    {
      fail ("check of a reversed ELLIPSE: status %d, %u equivalent, %s",
            c ? (int)c->status : -1, c ? (unsigned)c->num_equivalent : 0,
            c && c->reason ? c->reason : "");
      failures++;
    }
  c = find_check (report, 0xC8E1, 0xD24A);
  if (c && c->status == DWG_DYNBLOCK_CHECK_NO_VALUES
      && c->definition == TREES)
    OK ("check: no values on an INSERT without AcDbBlockRepresentation");
  else
    {
      fail ("check of an INSERT without values: status %d, definition "
            FORMAT_HV,
            c ? (int)c->status : -1, c ? c->definition : 0);
      failures++;
    }
  c = find_check (report, 0, 0xD49D);
  if (c && c->status == DWG_DYNBLOCK_CHECK_NO_VALUES
      && c->definition == TREES && !find_check (report, 0xC8E2, 0xD49D))
    OK ("check: no values for a block no INSERT draws");
  else
    {
      fail ("check of an unplaced block: status %d", c ? (int)c->status : -1);
      failures++;
    }
  // R1, T1, C17F, BFD8 and C145 mismatch; R2 and R3 have no values
  if (report->num_match == expected (MATCH) - 7 && report->num_mismatch == 5
      && report->num_no_values == 2
      && report->num_unsupported == expected (UNSUPPORTED))
    OK ("check finds: %u match, 5 mismatch, 2 no values, %u unsupported",
        (unsigned)report->num_match, (unsigned)report->num_unsupported);
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

/* A placement whose evaluation left out data, here an EED group of a Trees
   entity that couldn't be decoded, which the copy drops with
   DWG_ERR_INVALIDEED, is not a match, though the comparison, which doesn't
   compare EED, finds no difference. */
static int
test_check_copy_warning (Dwg_Data *dwg)
{
  Dwg_Object_BLOCK_HEADER *trees = block_header (dwg, TREES);
  Dwg_Object_BLOCK_HEADER *r1_block = block_header (dwg, 0xCFF7);
  BITCODE_H acad = dwg_find_tablehandle (dwg, "ACAD", "APPID");
  Dwg_Object *first;
  Dwg_Object_Entity *ent;
  Dwg_Eed *eed;
  Dwg_DynBlock_Check_Report *report;
  char bits[16];
  BITCODE_BL num_trees = 0;
  int failures = 0;
  loglevel = test_loglevel;
  first = trees && trees->entities && trees->num_owned
              ? dwg_ref_object (dwg, trees->entities[0])
              : NULL;
  if (!first || first->supertype != DWG_SUPERTYPE_ENTITY || !r1_block
      || !acad)
    {
      fail ("check copy warning: fixture objects not found");
      return 1;
    }
  // an EED group with a size and no data, as the decoder leaves one it
  // can't decode
  ent = first->tio.entity;
  eed = (Dwg_Eed *)realloc (ent->eed, (ent->num_eed + 1) * sizeof (Dwg_Eed));
  if (!eed)
    {
      fail ("check copy warning: out of memory");
      return 1;
    }
  ent->eed = eed;
  memset (&eed[ent->num_eed], 0, sizeof (Dwg_Eed));
  eed[ent->num_eed].size = 4;
  eed[ent->num_eed].handle = acad->handleref;
  ent->num_eed++;
  // R1's block: its base point, a difference besides the copy's
  r1_block->base_pt.x += 1.0;
  snprintf (bits, sizeof (bits), "(0x%x)", (unsigned)DWG_ERR_INVALIDEED);

  report = dwg_dynblock_check (dwg, TOLERANCE);
  loglevel = test_loglevel;
  if (!report)
    {
      fail ("check copy warning: no report");
      return 1;
    }
  for (size_t i = 0; i < NUM_CHECKS; i++)
    {
      const Expected_Check *e = &expected_checks[i];
      const Dwg_DynBlock_Check *c = find_check (report, e->insert, e->block);
      if (e->status != MATCH)
        continue;
      if (c && c->definition == TREES)
        {
          // the comparison ran and found no entity differing
          if (c->status == DWG_DYNBLOCK_CHECK_MISMATCH && c->evaluated
              && c->num_entities == e->num_entities && !c->num_differing
              && c->reason
              && strstr (c->reason, bits)
              && (e->insert != 0xC8E0 || strstr (c->reason, "base_pt")))
            OK ("check copy warning " FORMAT_HV ": %s", e->insert, c->reason);
          else
            {
              fail ("check copy warning " FORMAT_HV ": status %d, %u "
                    "entities, %u differ: %s; expected a mismatch naming %s",
                    e->insert, (int)c->status, (unsigned)c->num_entities,
                    (unsigned)c->num_differing, c->reason ? c->reason : "",
                    bits);
              failures++;
            }
          num_trees++;
        }
      else if (!c || c->status != DWG_DYNBLOCK_CHECK_MATCH)
        {
          fail ("check copy warning " FORMAT_HV ": status %d, %s; expected a "
                "match",
                e->insert, c ? (int)c->status : -1,
                c && c->reason ? c->reason : "");
          failures++;
        }
    }
  // R1, R2 and R3 among them
  if (num_trees >= 3 && report->num_mismatch == num_trees
      && report->num_match == expected (MATCH) - num_trees)
    OK ("check copy warning: %u Trees placements mismatch, %u match",
        (unsigned)num_trees, (unsigned)report->num_match);
  else
    {
      fail ("check copy warning: %u Trees placements, %u mismatch, %u match",
            (unsigned)num_trees, (unsigned)report->num_mismatch,
            (unsigned)report->num_match);
      failures++;
    }
  dwg_dynblock_free_check_report (report);
  return failures;
}

/* Turns LINE obj into a proxy entity holding numbits bits of data, with
   data_size 0, as the decoder leaves one. Returns 0 when obj is not a LINE
   or memory runs out. */
static int
line_to_proxy (Dwg_Object *obj, const unsigned char *data,
               const BITCODE_BL numbits)
{
  const size_t size = ((size_t)numbits + 7) / 8;
  Dwg_Entity_PROXY_ENTITY *proxy;
  if (!obj || obj->fixedtype != DWG_TYPE_LINE)
    return 0;
  proxy = (Dwg_Entity_PROXY_ENTITY *)calloc (1,
                                             sizeof (Dwg_Entity_PROXY_ENTITY));
  if (!proxy)
    return 0;
  proxy->data = (BITCODE_RC *)malloc (size);
  if (!proxy->data)
    {
      free (proxy);
      return 0;
    }
  memcpy (proxy->data, data, size);
  proxy->data_numbits = numbits;
  proxy->proxy_id = DWG_TYPE_PROXY_ENTITY;
  proxy->parent = obj->tio.entity;
  // a LINE holds no pointer but its parent
  free (obj->tio.entity->tio.LINE);
  obj->tio.entity->tio.PROXY_ENTITY = proxy;
  obj->type = obj->fixedtype = DWG_TYPE_PROXY_ENTITY;
  obj->name = (char *)"PROXY_ENTITY";
  obj->dxfname = (char *)"ACAD_PROXY_ENTITY";
  return 1;
}

/* A proxy entity's data, which the decoder keeps as data_numbits bits with
   data_size 0, is compared byte by byte. Here a LINE of Trees, and its
   copy in each Trees placement's block, becomes a proxy entity with 21
   bits of data: the same data, but for a full byte in R1's block and a bit
   of the partial last byte in R2's. bit_read_bits puts the 5 bits of that
   byte at its high end, 0xf8, which holds the bit, 0x10. */
static int
test_check_proxy (Dwg_Data *dwg)
{
  static const unsigned char data[] = { 0x5a, 0xc3, 0x18 };
  static const unsigned char r1_data[] = { 0x5b, 0xc3, 0x18 };
  static const unsigned char r2_data[] = { 0x5a, 0xc3, 0x08 };
  Dwg_Object *r1_line = first_of_type (dwg, 0xCFF7, DWG_TYPE_LINE);
  Dwg_DynBlock_Check_Report *report;
  BITCODE_RL idx = 0;
  BITCODE_RLL self = 0;
  BITCODE_BL num_trees = 0, num_found = 0;
  int failures = 0;
  loglevel = test_loglevel;
  if (!r1_line || !etag (dwg, r1_line, &idx, &self)
      || !line_to_proxy (rep_entity (dwg, TREES, idx), data, 21))
    {
      fail ("check proxy: fixture LINE of Trees not found");
      return 1;
    }
  for (size_t i = 0; i < NUM_CHECKS; i++)
    {
      const Expected_Check *e = &expected_checks[i];
      Dwg_Object_BLOCK_HEADER *def = NULL;
      Dwg_DynValue *values = NULL;
      BITCODE_BL count = 0;
      const unsigned char *d = e->block == 0xCFF7   ? r1_data
                               : e->block == 0xD24A ? r2_data
                                                    : data;
      int error = dwg_dynblock_get_values (insert_entity (dwg, e->insert),
                                           &def, &values, &count);
      dwg_dynblock_free_values (values, count);
      if (error >= DWG_ERR_CRITICAL || !def || block_handle (def) != TREES)
        continue;
      if (!line_to_proxy (rep_entity (dwg, e->block, idx), d, 21))
        {
          fail ("check proxy: " FORMAT_HV " has no LINE with index %u",
                e->block, (unsigned)idx);
          return 1;
        }
      num_trees++;
    }

  report = dwg_dynblock_check (dwg, TOLERANCE);
  loglevel = test_loglevel;
  if (!report)
    {
      fail ("check proxy: no report");
      return 1;
    }
  for (size_t i = 0; i < NUM_CHECKS; i++)
    {
      const Expected_Check *e = &expected_checks[i];
      const Dwg_DynBlock_Check *c = find_check (report, e->insert, e->block);
      const int differs = e->block == 0xCFF7 || e->block == 0xD24A;
      if (!c || c->definition != TREES)
        continue;
      if (differs
              ? c->status == DWG_DYNBLOCK_CHECK_MISMATCH
                    && c->num_differing == 1 && c->reason
                    && strstr (c->reason, "PROXY_ENTITY")
                    && strstr (c->reason, "data differs")
              : c->status == DWG_DYNBLOCK_CHECK_MATCH && !c->num_differing)
        {
          OK ("check proxy " FORMAT_HV ": %s", e->insert,
              c->reason ? c->reason : "match");
          num_found += differs;
        }
      else
        {
          fail ("check proxy " FORMAT_HV ": status %d, %u differ, %s; "
                "expected %s",
                e->insert, (int)c->status, (unsigned)c->num_differing,
                c->reason ? c->reason : "",
                differs ? "the data to differ" : "a match");
          failures++;
        }
    }
  // R1, R2 and R3 among them
  if (num_trees >= 3 && num_found == 2 && report->num_mismatch == 2
      && report->num_match == expected (MATCH) - 2)
    OK ("check proxy: %u Trees placements, 2 differ in their data",
        (unsigned)num_trees);
  else
    {
      fail ("check proxy: %u Trees placements, %u mismatch, %u match",
            (unsigned)num_trees, (unsigned)report->num_mismatch,
            (unsigned)report->num_match);
      failures++;
    }
  dwg_dynblock_free_check_report (report);
  return failures;
}

/* An ELLIPSE AutoCAD stored reversed matches by its sweep, whichever side
   of 0 its parameters lie: here alteZZa's first ellipse runs from 5.5 to
   0.3, through 0, and AutoCAD's blocks of BFD0 and BFD8 hold it reversed,
   from 5.5 - pi to 0.3 + pi. Its second is a full ellipse, which BFD0's
   block holds reversed, and BFD8's as reversed but of no length, which
   differs. */
static int
test_check_ellipse (Dwg_Data *dwg)
{
  const Dwg_Object *first = first_of_type (dwg, 0x9D5E, DWG_TYPE_ELLIPSE);
  const Dwg_Object_BLOCK_HEADER *bfd0 = block_header (dwg, 0x9D5E);
  Dwg_Object *second = NULL;
  BITCODE_RL idx[2];
  BITCODE_RLL self;
  Dwg_DynBlock_Check_Report *report;
  const Dwg_DynBlock_Check *c;
  int failures = 0;
  // the next ELLIPSE of BFD0's block
  for (BITCODE_BL i = 0; first && bfd0 && i < bfd0->num_owned; i++)
    {
      Dwg_Object *obj = dwg_ref_object (dwg, bfd0->entities[i]);
      if (obj && obj != first && obj->fixedtype == DWG_TYPE_ELLIPSE)
        {
          second = obj;
          break;
        }
    }
  if (!second || !etag (dwg, first, &idx[0], &self)
      || !etag (dwg, second, &idx[1], &self))
    {
      fail ("check ellipse: fixture ELLIPSEs not found");
      return 1;
    }
  for (int k = 0; k < 2; k++)
    {
      static const BITCODE_RLL blocks[] = { 0x9D5E, 0xAC8C };
      Dwg_Object *src = rep_entity (dwg, ALTEZZA, idx[k]);
      Dwg_Entity_ELLIPSE *e;
      if (!src || src->fixedtype != DWG_TYPE_ELLIPSE)
        {
          fail ("check ellipse: alteZZa's ELLIPSE %u not found",
                (unsigned)idx[k]);
          return 1;
        }
      e = src->tio.entity->tio.ELLIPSE;
      e->start_angle = k ? 0.0 : 5.5;
      e->end_angle = k ? 2.0 * M_PI : 0.3;
      for (int b = 0; b < 2; b++)
        {
          Dwg_Object *obj = rep_entity (dwg, blocks[b], idx[k]);
          Dwg_Entity_ELLIPSE *r;
          if (!obj || obj->fixedtype != DWG_TYPE_ELLIPSE)
            {
              fail ("check ellipse: " FORMAT_HV " has no ELLIPSE %u",
                    blocks[b], (unsigned)idx[k]);
              return 1;
            }
          r = obj->tio.entity->tio.ELLIPSE;
          r->center = e->center;
          r->extrusion = e->extrusion;
          r->axis_ratio = e->axis_ratio;
          r->sm_axis.x = -e->sm_axis.x;
          r->sm_axis.y = -e->sm_axis.y;
          r->sm_axis.z = -e->sm_axis.z;
          r->start_angle = e->start_angle + (k ? M_PI : -M_PI);
          // BFD8's second, reversed but of no length
          r->end_angle = k && b ? r->start_angle : e->end_angle + M_PI;
        }
    }

  report = dwg_dynblock_check (dwg, TOLERANCE);
  loglevel = test_loglevel;
  if (!report)
    {
      fail ("check ellipse: no report");
      return 1;
    }
  c = find_check (report, 0xBFD0, 0x9D5E);
  if (c && c->status == MATCH && c->num_equivalent == 2)
    OK ("check ellipse: BFD0 matches 2 reversed ELLIPSEs, one through 0");
  else
    {
      fail ("check ellipse BFD0: status %d, %u equivalent, %s",
            c ? (int)c->status : -1, c ? (unsigned)c->num_equivalent : 0,
            c && c->reason ? c->reason : "");
      failures++;
    }
  c = find_check (report, 0xBFD8, 0xAC8C);
  if (c && c->status == DWG_DYNBLOCK_CHECK_MISMATCH && c->num_equivalent == 1
      && c->num_differing == 1 && c->reason && strstr (c->reason, "ELLIPSE"))
    OK ("check ellipse: BFD8's ellipse of no length differs: %s", c->reason);
  else
    {
      fail ("check ellipse BFD8: status %d, %u equivalent, %u differ, %s",
            c ? (int)c->status : -1, c ? (unsigned)c->num_equivalent : 0,
            c ? (unsigned)c->num_differing : 0,
            c && c->reason ? c->reason : "");
      failures++;
    }
  dwg_dynblock_free_check_report (report);
  return failures;
}

/* Turns LINE obj into a MULTILEADER of class number type whose context
   holds text content width wide, or block content with transform t.
   Returns 0 when obj is not a LINE or memory runs out. */
static int
line_to_mleader (Dwg_Object *obj, const int type, const double width,
                 const double *t)
{
  Dwg_Entity_MULTILEADER *mleader;
  if (!obj || obj->fixedtype != DWG_TYPE_LINE)
    return 0;
  mleader = (Dwg_Entity_MULTILEADER *)calloc (
      1, sizeof (Dwg_Entity_MULTILEADER));
  if (!mleader)
    return 0;
  if (t)
    {
      mleader->ctx.has_content_blk = 1;
      mleader->ctx.content.blk.transform
          = (BITCODE_BD *)malloc (16 * sizeof (BITCODE_BD));
      if (!mleader->ctx.content.blk.transform)
        {
          free (mleader);
          return 0;
        }
      memcpy (mleader->ctx.content.blk.transform, t, 16 * sizeof (double));
    }
  else
    {
      mleader->ctx.has_content_txt = 1;
      mleader->ctx.content.txt.width = width;
    }
  mleader->parent = obj->tio.entity;
  // a LINE holds no pointer but its parent
  free (obj->tio.entity->tio.LINE);
  obj->tio.entity->tio.MULTILEADER = mleader;
  obj->type = (BITCODE_BS)type;
  obj->fixedtype = DWG_TYPE_MULTILEADER;
  obj->name = (char *)"MULTILEADER";
  obj->dxfname = (char *)"MULTILEADER";
  return 1;
}

/* A multileader's content is a union, of text or block content, which the
   check compares as the context's flags say. Here two LINEs of Trees, and
   their copies in each Trees placement's block, become MULTILEADERs: one
   with text content, whose width R1's block changes, and one with block
   content, whose transform R2's block changes. */
static int
test_check_mleader (Dwg_Data *dwg)
{
  double t[16], r2_t[16];
  const Dwg_Object_BLOCK_HEADER *r1 = block_header (dwg, 0xCFF7);
  const int type
      = dwg_require_class (dwg, "MULTILEADER", strlen ("MULTILEADER"));
  Dwg_DynBlock_Check_Report *report;
  BITCODE_RL idx[2];
  BITCODE_BL num_lines = 0, num_trees = 0;
  int failures = 0;
  loglevel = test_loglevel;
  for (int i = 0; i < 16; i++)
    t[i] = r2_t[i] = i % 5 ? 0.0 : 1.0;
  r2_t[5] = 2.0;
  // the first two LINEs of R1's block
  for (BITCODE_BL i = 0; r1 && i < r1->num_owned && num_lines < 2; i++)
    {
      Dwg_Object *obj = dwg_ref_object (dwg, r1->entities[i]);
      BITCODE_RLL self;
      if (obj && obj->fixedtype == DWG_TYPE_LINE
          && etag (dwg, obj, &idx[num_lines], &self))
        num_lines++;
    }
  if (num_lines < 2 || type < 500
      || !line_to_mleader (rep_entity (dwg, TREES, idx[0]), type, 3.5, NULL)
      || !line_to_mleader (rep_entity (dwg, TREES, idx[1]), type, 0.0, t))
    {
      fail ("check mleader: fixture LINEs of Trees not found");
      return 1;
    }
  for (size_t i = 0; i < NUM_CHECKS; i++)
    {
      const Expected_Check *e = &expected_checks[i];
      if (e->definition != TREES)
        continue;
      if (!line_to_mleader (rep_entity (dwg, e->block, idx[0]), type,
                            e->block == 0xCFF7 ? 4.5 : 3.5, NULL)
          || !line_to_mleader (rep_entity (dwg, e->block, idx[1]), type, 0.0,
                               e->block == 0xD24A ? r2_t : t))
        {
          fail ("check mleader: " FORMAT_HV " has no LINEs %u and %u",
                e->block, (unsigned)idx[0], (unsigned)idx[1]);
          return 1;
        }
      num_trees++;
    }

  report = dwg_dynblock_check (dwg, TOLERANCE);
  loglevel = test_loglevel;
  if (!report)
    {
      fail ("check mleader: no report");
      return 1;
    }
  for (size_t i = 0; i < NUM_CHECKS; i++)
    {
      const Expected_Check *e = &expected_checks[i];
      const Dwg_DynBlock_Check *c = find_check (report, e->insert, e->block);
      const char *differs = e->block == 0xCFF7   ? "txt.width"
                            : e->block == 0xD24A ? "blk.transform[5]"
                                                 : NULL;
      if (e->definition != TREES)
        continue;
      if (c
          && (differs ? c->status == DWG_DYNBLOCK_CHECK_MISMATCH
                            && c->num_differing == 1 && c->reason
                            && strstr (c->reason, "MULTILEADER")
                            && strstr (c->reason, differs)
                      : c->status == MATCH && !c->num_differing))
        OK ("check mleader " FORMAT_HV ": %s", e->insert,
            c->reason ? c->reason : "match");
      else
        {
          fail ("check mleader " FORMAT_HV ": status %d, %u differ, %s; "
                "expected %s",
                e->insert, c ? (int)c->status : -1,
                c ? (unsigned)c->num_differing : 0,
                c && c->reason ? c->reason : "",
                differs ? differs : "a match");
          failures++;
        }
    }
  if (num_trees < 4)
    {
      fail ("check mleader: %u Trees placements", (unsigned)num_trees);
      failures++;
    }
  dwg_dynblock_free_check_report (report);
  return failures;
}

/* Turns LINE obj into a POLYLINE_2D owning the VERTEXes and SEQEND of a new
   POLYLINE_2D through pts, added to model space and left with none.
   Returns 0 on failure. */
static int
line_to_polyline (Dwg_Data *dwg, const BITCODE_RLL handle,
                  const dwg_point_2d *pts)
{
  Dwg_Object_BLOCK_HEADER *mspace = block_header (dwg, model_space (dwg));
  Dwg_Entity_POLYLINE_2D *donor
      = mspace ? dwg_add_POLYLINE_2D (mspace, 3, pts) : NULL;
  Dwg_Entity_POLYLINE_2D *pline;
  // found after the add, which may move dwg->object
  Dwg_Object *obj = dwg_resolve_handle (dwg, handle);
  loglevel = test_loglevel;
  if (!donor || donor->num_owned != 3 || !donor->vertex || !obj
      || obj->fixedtype != DWG_TYPE_LINE)
    return 0;
  pline = (Dwg_Entity_POLYLINE_2D *)calloc (
      1, sizeof (Dwg_Entity_POLYLINE_2D));
  if (!pline)
    return 0;
  pline->has_vertex = 1;
  pline->num_owned = donor->num_owned;
  pline->vertex = donor->vertex;
  pline->seqend = donor->seqend;
  pline->extrusion.z = 1.0;
  donor->num_owned = 0;
  donor->vertex = NULL;
  donor->seqend = NULL;
  for (BITCODE_BL i = 0; i < pline->num_owned; i++)
    set_owner (dwg, pline->vertex[i], handle);
  set_owner (dwg, pline->seqend, handle);
  pline->parent = obj->tio.entity;
  // a LINE holds no pointer but its parent
  free (obj->tio.entity->tio.LINE);
  obj->tio.entity->tio.POLYLINE_2D = pline;
  obj->type = obj->fixedtype = DWG_TYPE_POLYLINE_2D;
  obj->name = (char *)"POLYLINE_2D";
  obj->dxfname = (char *)"POLYLINE";
  return 1;
}

/* Turns the INSERT at handle into a MINSERT of one row and column. */
static int
insert_to_minsert (Dwg_Data *dwg, const BITCODE_RLL handle)
{
  Dwg_Object *obj = dwg_resolve_handle (dwg, handle);
  Dwg_Entity_INSERT *insert;
  Dwg_Entity_MINSERT *minsert;
  if (!obj || obj->fixedtype != DWG_TYPE_INSERT)
    return 0;
  insert = obj->tio.entity->tio.INSERT;
  if (insert->num_owned || insert->attribs)
    return 0;
  minsert = (Dwg_Entity_MINSERT *)calloc (1, sizeof (Dwg_Entity_MINSERT));
  if (!minsert)
    return 0;
  minsert->ins_pt = insert->ins_pt;
  minsert->scale_flag = insert->scale_flag;
  minsert->scale = insert->scale;
  minsert->rotation = insert->rotation;
  minsert->extrusion = insert->extrusion;
  minsert->block_header = insert->block_header;
  minsert->num_cols = minsert->num_rows = 1;
  minsert->parent = obj->tio.entity;
  free (insert);
  obj->tio.entity->tio.MINSERT = minsert;
  obj->type = obj->fixedtype = DWG_TYPE_MINSERT;
  obj->name = (char *)"MINSERT";
  obj->dxfname = (char *)"INSERT";
  return 1;
}

/* Owned entities, and a MINSERT placement: a LINE of Trees, and its copy
   in each Trees placement's block, becomes a POLYLINE_2D with 3 VERTEXes,
   one of which R2's block moves; and R1's INSERT becomes a MINSERT, which
   the check finds and matches. */
static int
test_check_owned (Dwg_Data *dwg)
{
  static const dwg_point_2d pts[3]
      = { { 0.0, 0.0 }, { 1.0, 0.0 }, { 1.0, 1.0 } };
  static const dwg_point_2d r2_pts[3]
      = { { 0.0, 0.0 }, { 1.0, 0.5 }, { 1.0, 1.0 } };
  const Dwg_Object *r1_line = first_of_type (dwg, 0xCFF7, DWG_TYPE_LINE);
  BITCODE_RLL lines[8];
  const dwg_point_2d *line_pts[8];
  Dwg_DynBlock_Check_Report *report;
  BITCODE_RL idx = 0;
  BITCODE_RLL self = 0;
  int n = 0, failures = 0;
  const Dwg_Object *line;
  loglevel = test_loglevel;
  if (!r1_line || !etag (dwg, r1_line, &idx, &self)
      || !(line = rep_entity (dwg, TREES, idx)))
    {
      fail ("check owned: fixture LINE of Trees not found");
      return 1;
    }
  // the handles first, as adding objects may move dwg->object
  lines[n] = line->handle.value;
  line_pts[n++] = pts;
  for (size_t i = 0; i < NUM_CHECKS && n < 8; i++)
    {
      const Expected_Check *e = &expected_checks[i];
      if (e->definition != TREES)
        continue;
      line = rep_entity (dwg, e->block, idx);
      if (!line)
        {
          fail ("check owned: " FORMAT_HV " has no LINE %u", e->block,
                (unsigned)idx);
          return 1;
        }
      lines[n] = line->handle.value;
      line_pts[n++] = e->block == 0xD24A ? r2_pts : pts;
    }
  for (int i = 0; i < n; i++)
    if (!line_to_polyline (dwg, lines[i], line_pts[i]))
      {
        fail ("check owned: could not make LINE " FORMAT_HV " a POLYLINE",
              lines[i]);
        return 1;
      }
  if (!insert_to_minsert (dwg, 0xC8E0))
    {
      fail ("check owned: could not make R1's INSERT a MINSERT");
      return 1;
    }

  report = dwg_dynblock_check (dwg, TOLERANCE);
  loglevel = test_loglevel;
  if (!report)
    {
      fail ("check owned: no report");
      return 1;
    }
  for (size_t i = 0; i < NUM_CHECKS; i++)
    {
      const Expected_Check *e = &expected_checks[i];
      const Dwg_DynBlock_Check *c = find_check (report, e->insert, e->block);
      const int differs = e->block == 0xD24A;
      if (e->definition != TREES)
        continue;
      if (c
          && (differs ? c->status == DWG_DYNBLOCK_CHECK_MISMATCH
                            && c->num_differing == 1 && c->reason
                            && strstr (c->reason, "POLYLINE_2D")
                            && strstr (c->reason, "point")
                      : c->status == MATCH && !c->num_differing))
        OK ("check owned " FORMAT_HV "%s: %s", e->insert,
            e->insert == 0xC8E0 ? " (a MINSERT)" : "",
            c->reason ? c->reason : "match");
      else
        {
          fail ("check owned " FORMAT_HV ": status %d, %u differ, %s; "
                "expected %s",
                e->insert, c ? (int)c->status : -1,
                c ? (unsigned)c->num_differing : 0,
                c && c->reason ? c->reason : "",
                differs ? "a VERTEX to differ" : "a match");
          failures++;
        }
    }
  dwg_dynblock_free_check_report (report);
  return failures;
}

/* Turns LINE obj into a TEXT of string text, in the drawing's codepage.
   Returns 0 when obj is not a LINE or memory runs out. */
static int
line_to_text (Dwg_Object *obj, const char *text)
{
  Dwg_Entity_TEXT *_text;
  if (!obj || obj->fixedtype != DWG_TYPE_LINE)
    return 0;
  _text = (Dwg_Entity_TEXT *)calloc (1, sizeof (Dwg_Entity_TEXT));
  if (!_text)
    return 0;
  _text->text_value = strdup (text);
  if (!_text->text_value)
    {
      free (_text);
      return 0;
    }
  _text->extrusion.z = 1.0;
  _text->height = 1.0;
  _text->width_factor = 1.0;
  _text->parent = obj->tio.entity;
  // a LINE holds no pointer but its parent
  free (obj->tio.entity->tio.LINE);
  obj->tio.entity->tio.TEXT = _text;
  obj->type = obj->fixedtype = DWG_TYPE_TEXT;
  obj->name = (char *)"TEXT";
  obj->dxfname = (char *)"TEXT";
  return 1;
}

/* Whether s is whole UTF-8 characters. */
static int
is_utf8 (const char *s)
{
  const unsigned char *p = (const unsigned char *)s;
  while (*p)
    {
      const int n = *p < 0x80             ? 0
                    : (*p & 0xE0) == 0xC0 ? 1
                    : (*p & 0xF0) == 0xE0 ? 2
                    : (*p & 0xF8) == 0xF0 ? 3
                                          : -1;
      if (n < 0)
        return 0;
      p++;
      for (int i = 0; i < n; i++, p++)
        if ((*p & 0xC0) != 0x80)
          return 0;
    }
  return 1;
}

/* The length of reason after the text AutoCAD's string starts with, or
   -1. */
static int
acad_string_length (const char *reason)
{
  static const char start[] = "AutoCAD's \"";
  const char *s = reason ? strstr (reason, start) : NULL;
  return s ? (int)strlen (s + strlen (start)) : -1;
}

/* The check gives the strings it finds differing in UTF-8, from the
   drawing's codepage, Windows-1252 here. A LINE of Trees, and its copy in
   each Trees placement's block, becomes a TEXT: "abc" in the definition,
   but "caf\xe9" (e acute) in R1's block, and 200 e acutes in R2's and R3's,
   after one and two letters, which the reason cuts short. One of the two
   cuts falls inside a two-byte character, which the reason leaves out, so
   that it ends at a whole character. */
static int
test_check_utf8 (Dwg_Data *dwg)
{
  const Dwg_Object *r1_line = first_of_type (dwg, 0xCFF7, DWG_TYPE_LINE);
  char long_text[2][204];
  Dwg_DynBlock_Check_Report *report;
  const Dwg_DynBlock_Check *c;
  BITCODE_RL idx = 0;
  BITCODE_RLL self = 0;
  BITCODE_BL num_trees = 0;
  int len[2], failures = 0;
  loglevel = test_loglevel;
  for (int k = 0; k < 2; k++)
    {
      memset (long_text[k], 'x', (size_t)k + 1);
      memset (&long_text[k][k + 1], 0xE9, 200);
      long_text[k][k + 201] = '\0';
    }
  if (dwg->header.codepage != 30 // ANSI_1252
      || !r1_line || !etag (dwg, r1_line, &idx, &self)
      || !line_to_text (rep_entity (dwg, TREES, idx), "abc"))
    {
      fail ("check utf8: fixture LINE of Trees not found, or codepage %u",
            (unsigned)dwg->header.codepage);
      return 1;
    }
  for (size_t i = 0; i < NUM_CHECKS; i++)
    {
      const Expected_Check *e = &expected_checks[i];
      const char *text = e->block == 0xCFF7   ? "caf\xe9"
                         : e->block == 0xD24A ? long_text[0]
                         : e->block == 0xD49D ? long_text[1]
                                              : "abc";
      if (e->definition != TREES)
        continue;
      if (!line_to_text (rep_entity (dwg, e->block, idx), text))
        {
          fail ("check utf8: " FORMAT_HV " has no LINE %u", e->block,
                (unsigned)idx);
          return 1;
        }
      num_trees++;
    }

  report = dwg_dynblock_check (dwg, TOLERANCE);
  loglevel = test_loglevel;
  if (!report)
    {
      fail ("check utf8: no report");
      return 1;
    }
  c = find_check (report, 0xC8E0, 0xCFF7);
  if (c && c->status == DWG_DYNBLOCK_CHECK_MISMATCH && c->reason
      && strstr (c->reason, "text_value is \"abc\", AutoCAD's "
                            "\"caf\xc3\xa9\""))
    OK ("check utf8: R1's string in UTF-8: %s", c->reason);
  else
    {
      fail ("check utf8 R1: status %d, %s", c ? (int)c->status : -1,
            c && c->reason ? c->reason : "");
      failures++;
    }
  for (int k = 0; k < 2; k++)
    {
      c = find_check (report, k ? 0xC8E2 : 0xC8E1, k ? 0xD49D : 0xD24A);
      len[k] = c ? acad_string_length (c->reason) : -1;
      if (c && c->status == DWG_DYNBLOCK_CHECK_MISMATCH && c->reason
          && is_utf8 (c->reason) && strstr (c->reason, "\xc3\xa9")
          && len[k] > 0 && len[k] < 2 * 200)
        OK ("check utf8: R%d's reason is cut short at a whole character",
            k + 2);
      else
        {
          fail ("check utf8 R%d: status %d, %s", k + 2,
                c ? (int)c->status : -1, c && c->reason ? c->reason : "");
          failures++;
        }
    }
  if (abs (len[0] - len[1]) == 1)
    OK ("check utf8: one of the cuts left out a part character");
  else
    {
      fail ("check utf8: AutoCAD's strings cut to %d and %d bytes", len[0],
            len[1]);
      failures++;
    }
  c = find_check (report, 0xC145, 0xB6AC);
  if (num_trees < 4 || !c || c->status != MATCH)
    {
      fail ("check utf8: %u Trees placements, C145 status %d",
            (unsigned)num_trees, c ? (int)c->status : -1);
      failures++;
    }
  dwg_dynblock_free_check_report (report);
  return failures;
}

/* The object or entity obj's EED, of *countp entries. */
static Dwg_Eed *
object_eed (Dwg_Object *obj, BITCODE_BL *countp)
{
  if (obj->supertype == DWG_SUPERTYPE_ENTITY)
    {
      *countp = obj->tio.entity->num_eed;
      return obj->tio.entity->eed;
    }
  *countp = obj->tio.object->num_eed;
  return obj->tio.object->eed;
}

/* Makes the AcDbBlockRepBTag EED of representation block blk name
   definition def. Returns 0 when blk has none. */
static int
set_btag (Dwg_Data *dwg, const BITCODE_RLL blk, const BITCODE_RLL def)
{
  Dwg_Object *obj = dwg_resolve_handle (dwg, blk);
  BITCODE_BL num_eed = 0;
  Dwg_Eed *eed = obj ? object_eed (obj, &num_eed) : NULL;
  int in_group = 0;
  for (BITCODE_BL i = 0; eed && i < num_eed; i++)
    {
      if (eed[i].size)
        {
          const Dwg_Object *appid
              = dwg_resolve_handle (dwg, eed[i].handle.value);
          in_group = appid && appid->fixedtype == DWG_TYPE_APPID
                     && !strcmp (appid->tio.object->tio.APPID->name,
                                 "AcDbBlockRepBTag");
        }
      // Dwg_Eed_Data is packed
      if (in_group && eed[i].data && eed[i].data->code == 5)
        {
          memcpy (&eed[i].data->u.eed_5.entity, &def, sizeof (def));
          return 1;
        }
    }
  return 0;
}

/* The AppDataCache DICTIONARY of placement insert, or NULL. */
static Dwg_Object *
app_data_cache (Dwg_Data *dwg, const BITCODE_RLL insert)
{
  Dwg_Object *obj = dwg_resolve_handle (dwg, insert);
  if (!obj || obj->supertype != DWG_SUPERTYPE_ENTITY)
    return NULL;
  return dict_entry (
      dwg,
      dict_entry (dwg,
                  dwg_ref_object (dwg, obj->tio.entity->xdicobjhandle),
                  "AcDbBlockRepresentation"),
      "AppDataCache");
}

/* The name placement insert's AppDataCache lists its
   ACAD_ENHANCEDBLOCKDATA by, or NULL. */
static char *
enhanced_data_name (Dwg_Data *dwg, const BITCODE_RLL insert)
{
  Dwg_Object *cache = app_data_cache (dwg, insert);
  const Dwg_Object_DICTIONARY *_cache;
  if (!cache || cache->fixedtype != DWG_TYPE_DICTIONARY)
    return NULL;
  _cache = cache->tio.object->tio.DICTIONARY;
  for (BITCODE_BL i = 0; _cache->texts && i < _cache->numitems; i++)
    if (_cache->texts[i]
        && !strcmp (_cache->texts[i], "ACAD_ENHANCEDBLOCKDATA"))
      return _cache->texts[i];
  return NULL;
}

/* The XRECORD of a placement's stored values for the graph node with node
   id nodeid, or NULL. */
static Dwg_Object_XRECORD *
stored_value (Dwg_Data *dwg, const BITCODE_RLL insert, const BITCODE_BL nodeid)
{
  Dwg_Object *obj;
  Dwg_Object *data
      = dict_entry (dwg, app_data_cache (dwg, insert), "ACAD_ENHANCEDBLOCKDATA");
  char key[16];
  snprintf (key, sizeof (key), "%u", (unsigned)nodeid);
  obj = dict_entry (dwg, data, key);
  return obj && obj->fixedtype == DWG_TYPE_XRECORD
             ? obj->tio.object->tio.XRECORD
             : NULL;
}

/* How the check reports a placement whose values can't be read: as failed
   when they are damaged, here W4's stored Window Width, whose first point
   item (10) is made 11, W3's, whose ACAD_ENHANCEDBLOCKDATA is renamed, and
   the Door's placements, whose definition has a graph node that doesn't
   resolve; as unsupported when the drawing isn't r2004. Each names the
   definition the placement's values name, here the Window for W3 and the
   Door for D1, whose blocks' AcDbBlockRepBTag are made to name the
   other. */
static int
test_check_status (Dwg_Data *dwg)
{
  Dwg_Object_BLOCKLINEARPARAMETER *width
      = linear_param (dwg, WINDOW, "Window Width");
  Dwg_Object_EVALUATION_GRAPH *door_graph = definition_graph (dwg, DOOR);
  Dwg_Object_XRECORD *xrecord
      = width ? stored_value (dwg, 0xC8D3, width->evalexpr.nodeid) : NULL;
  char *w3_data = enhanced_data_name (dwg, 0xC8D2);
  Dwg_DynBlock_Check_Report *report;
  const Dwg_DynBlock_Check *c;
  const Dwg_Version_Type version = dwg->header.from_version;
  BITCODE_BL num_door = 0, num_failed = 0;
  int damaged = 0, failures = 0;
  loglevel = test_loglevel;
  for (Dwg_Resbuf *rb = xrecord ? xrecord->xdata : NULL; rb && !damaged;
       rb = rb->nextrb)
    if (rb->type == 10)
      {
        rb->type = 11;
        damaged = 1;
      }
  if (!damaged || !w3_data || !door_graph || !door_graph->nodes
      || !door_graph->num_nodes || !set_btag (dwg, 0xC9F2, WINDOW)
      || !set_btag (dwg, 0xC9AA, DOOR))
    {
      fail ("check status: fixture objects not found");
      return 1;
    }
  door_graph->nodes[0].evalexpr = dwg_add_handleref (dwg, 4, 0xFFFFFE, NULL);
  w3_data[0] = 'X';

  report = dwg_dynblock_check (dwg, TOLERANCE);
  loglevel = test_loglevel;
  if (!report)
    {
      fail ("check status: no report");
      return 1;
    }
  c = find_check (report, 0xC8D3, 0xC9C8);
  if (c && c->status == DWG_DYNBLOCK_CHECK_FAILED && c->definition == WINDOW
      && !c->evaluated && c->reason
      && strstr (c->reason, "not in the layout AutoCAD writes"))
    OK ("check status: damaged values fail, naming the Window: %s",
        c->reason);
  else
    {
      fail ("check status W4: status %d, definition " FORMAT_HV ", %s",
            c ? (int)c->status : -1, c ? c->definition : 0,
            c && c->reason ? c->reason : "");
      failures++;
    }
  c = find_check (report, 0xC8D2, 0xC9AA);
  if (c && c->status == DWG_DYNBLOCK_CHECK_FAILED && c->definition == WINDOW
      && !c->evaluated && c->reason
      && strstr (c->reason, "has no ACAD_ENHANCEDBLOCKDATA"))
    OK ("check status: values missing fail, naming the Window: %s",
        c->reason);
  else
    {
      fail ("check status W3: status %d, definition " FORMAT_HV ", %s",
            c ? (int)c->status : -1, c ? c->definition : 0,
            c && c->reason ? c->reason : "");
      failures++;
    }
  for (size_t i = 0; i < NUM_CHECKS; i++)
    {
      const Expected_Check *e = &expected_checks[i];
      if (e->definition != DOOR)
        continue;
      c = find_check (report, e->insert, e->block);
      num_door++;
      if (c && c->status == DWG_DYNBLOCK_CHECK_FAILED
          && c->definition == DOOR && c->reason
          && strstr (c->reason, "doesn't resolve"))
        OK ("check status " FORMAT_HV ": a graph node that doesn't resolve "
            "fails, naming the Door",
            e->insert);
      else
        {
          fail ("check status " FORMAT_HV ": status %d, definition " FORMAT_HV
                ", %s",
                e->insert, c ? (int)c->status : -1, c ? c->definition : 0,
                c && c->reason ? c->reason : "");
          failures++;
        }
    }
  num_failed = report->num_failed;
  if (num_door != 5 || num_failed != num_door + 2)
    {
      fail ("check status: %u failed, expected %u", (unsigned)num_failed,
            (unsigned)num_door + 2);
      failures++;
    }
  dwg_dynblock_free_check_report (report);
  w3_data[0] = 'A';

  // reading the values refuses a drawing other than r2004
  dwg->header.from_version = R_2000;
  report = dwg_dynblock_check (dwg, TOLERANCE);
  dwg->header.from_version = version;
  loglevel = test_loglevel;
  if (!report)
    {
      fail ("check status: no report");
      return failures + 1;
    }
  c = find_check (report, 0xC8D5, 0xC9F2);
  if (report->num_unsupported == NUM_CHECKS && !report->num_failed && c
      && c->definition == DOOR && c->reason && strstr (c->reason, "r2004"))
    OK ("check status: a drawing other than r2004 is unsupported, naming "
        "the definitions: %s",
        c->reason);
  else
    {
      fail ("check status, not r2004: %u unsupported, %u failed, D1 "
            "definition " FORMAT_HV ", %s",
            (unsigned)report->num_unsupported, (unsigned)report->num_failed,
            c ? c->definition : 0, c && c->reason ? c->reason : "");
      failures++;
    }
  dwg_dynblock_free_check_report (report);
  return failures;
}

/* The check reports the placements of a block whose linear properties'
   actions move each other's parameters as unsupported, not failed: here
   the Window with test_linear_cycle's codes, whose placements each store
   both its linear values, which the check gives the evaluator. */
static int
test_check_cycle (Dwg_Data *dwg)
{
  Dwg_Object_BLOCKSTRETCHACTION *width_stretch
      = stretch_action (dwg, WINDOW, "Stretch Width");
  Dwg_Object_BLOCKSTRETCHACTION *height_stretch
      = stretch_action (dwg, WINDOW, "Stretch Height");
  Dwg_Object_BLOCKLINEARPARAMETER *width
      = linear_param (dwg, WINDOW, "Window Width");
  Dwg_Object_BLOCKLINEARPARAMETER *height
      = linear_param (dwg, WINDOW, "Window Height");
  Dwg_DynBlock_Check_Report *report;
  BITCODE_BL num_window = 0;
  int failures = 0;
  loglevel = test_loglevel;
  if (!width_stretch || !height_stretch || !width || !height
      || !add_stretch_code (width_stretch, height->evalexpr.nodeid, 1)
      || !add_stretch_code (height_stretch, width->evalexpr.nodeid, 1))
    {
      fail ("check cycle: fixture objects not found");
      return 1;
    }
  report = dwg_dynblock_check (dwg, TOLERANCE);
  loglevel = test_loglevel;
  if (!report)
    {
      fail ("check cycle: no report");
      return 1;
    }
  for (size_t i = 0; i < NUM_CHECKS; i++)
    {
      const Expected_Check *e = &expected_checks[i];
      const Dwg_DynBlock_Check *c;
      if (e->definition != WINDOW)
        continue;
      c = find_check (report, e->insert, e->block);
      num_window++;
      if (c && c->status == UNSUPPORTED && c->definition == WINDOW
          && c->reason && strstr (c->reason, "cycle"))
        OK ("check cycle " FORMAT_HV ": unsupported: %s", e->insert,
            c->reason);
      else
        {
          fail ("check cycle " FORMAT_HV ": status %d, %s", e->insert,
                c ? (int)c->status : -1, c && c->reason ? c->reason : "");
          failures++;
        }
    }
  if (num_window != 5 || report->num_failed
      || report->num_unsupported != expected (UNSUPPORTED) + num_window
      || report->num_match != expected (MATCH) - num_window)
    {
      fail ("check cycle: %u Window placements, %u match, %u unsupported, "
            "%u failed",
            (unsigned)num_window, (unsigned)report->num_match,
            (unsigned)report->num_unsupported, (unsigned)report->num_failed);
      failures++;
    }
  dwg_dynblock_free_check_report (report);
  return failures;
}

/* Reads the fixture into dwg, runs test on it, and frees it. */
static int
on_fixture (int (*test) (Dwg_Data *))
{
  Dwg_Data dwg;
  int failures;
  if (read_fixture (&dwg))
    {
      free_dwg (&dwg);
      return 1;
    }
  failures = test (&dwg);
  free_dwg (&dwg);
  return failures;
}

/* The evaluations of linear properties, on one reading of the fixture: the
   ones that change it come last. */
static int
test_linear_all (Dwg_Data *dwg)
{
  int failures = test_linear (dwg);
  failures += test_linear_order (dwg);
  failures += test_linear_cycle (dwg);
  failures += test_linear_refusals (dwg);
  failures += test_angle_offset (dwg);
  failures += test_stretch_whole (dwg);
  failures += test_stretch_selection (dwg);
  failures += test_move (dwg);
  failures += test_selected_nodes (dwg);
  return failures;
}

static int
test_evaluate_all (Dwg_Data *dwg)
{
  int failures = test_params (dwg);
  failures += test_rotation_default (dwg);
  failures += test_values (dwg);
  failures += test_refusals (dwg);
  failures += test_evaluate (dwg);
  failures += test_evaluate_owned (dwg);
  return failures;
}

int
main (int argc, char *argv[])
{
  int failures = 0;
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

  failures += on_fixture (test_evaluate_all);
  failures += on_fixture (test_linear_all);
  // each check adds its evaluations to the document: a fresh one each
  failures += on_fixture (test_check);
  failures += on_fixture (test_check_finds);
  failures += on_fixture (test_check_copy_warning);
  failures += on_fixture (test_check_proxy);
  failures += on_fixture (test_check_ellipse);
  failures += on_fixture (test_check_mleader);
  failures += on_fixture (test_check_owned);
  failures += on_fixture (test_check_utf8);
  failures += on_fixture (test_check_status);
  failures += on_fixture (test_check_cycle);
  return failures ? 1 : 0;
}
