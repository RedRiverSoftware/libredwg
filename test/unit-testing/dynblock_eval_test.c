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
   90 degrees, a scale action and two move actions; its Toilet has a
   rotation property, Angle, whose rotate action 'Rotate Plan' turns part
   of it about the origin. Further cases are made by adding entities to
   those actions, or changing them, in memory. */

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
  // a full block table, whose 16-bit count the new block would wrap
  {
    Dwg_Object_BLOCK_CONTROL *ctrl
        = dwg_get_first_object (dwg, DWG_TYPE_BLOCK_CONTROL)
              ->tio.object->tio.BLOCK_CONTROL;
    const BITCODE_BS num_entries = ctrl->num_entries;
    ctrl->num_entries = 0xFFFF;
    v[0].name = (char *)"Type (view)";
    v[0].state = (char *)"Shrub (Plan)";
    failures += test_refusal (dwg, TREES, v, 1,
                              REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                              "a full block table");
    ctrl->num_entries = num_entries;
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
   Width. Only the actions of the properties that run run, so the Window
   evaluates with no values, with Width alone, as Height isn't given and
   its stretch doesn't run, and with both given at their lengths, when
   neither runs. With Width changed and Height given, Width's stretch
   moves Height's end point, so Height runs too, and the order AutoCAD
   evaluates them in isn't known: the evaluator refuses them, as with both
   changed. */
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
  v[1].value = 36.0;
  failures += test_refusal (dwg, WINDOW, v, 2,
                            REFUSED (DWG_ERR_NOTYETSUPPORTED),
                            "a changed linear value whose action moves "
                            "another given one, whose action moves it");
  v[0].value = 36.0;
  rep = evaluate (dwg, WINDOW, v, 2, "linear cycle, both at their lengths");
  frame = rep ? copy_of (dwg, def, rep, 0xBBD3) : NULL;
  if (frame && frame->fixedtype == DWG_TYPE_LWPOLYLINE
      && frame->tio.entity->tio.LWPOLYLINE->num_points == 4
      && is_point (frame->tio.entity->tio.LWPOLYLINE->points[2].x,
                   frame->tio.entity->tio.LWPOLYLINE->points[2].y, 36.0, 36.0))
    OK ("linear cycle: both given at their lengths, neither runs");
  else if (rep)
    {
      fail ("linear cycle: both at their lengths moved the frame");
      failures++;
    }
  else
    failures++;
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
  const Dwg_Object *frame = dwg_resolve_handle (dwg, 0xBBD3);
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
    // a huge value on a short parameter: the frame's vertex 3, (36, 0),
    // moves by 1e308 - 0.5 to about 1e308, and stays at y 0
    width->def_endpt.x = width->def_basept.x + 0.5;
    v.value = 1e308;
    rep = evaluate (dwg, WINDOW, &v, 1, "linear 1e308");
    copy = rep ? copy_of (dwg, def, rep, 0xBBD3) : NULL;
    if (copy && copy->fixedtype == DWG_TYPE_LWPOLYLINE
        && copy->tio.entity->tio.LWPOLYLINE->num_points == 4
        && fabs (copy->tio.entity->tio.LWPOLYLINE->points[3].x / 1e308 - 1.0)
               < 1e-9
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
    // 1.5e308 moves the frame's vertices 2 and 3 once each: vertex 3,
    // (36, 0), by 1.5e308 - 36 to 1.5e308
    v.value = 1.5e308;
    rep = evaluate (dwg, WINDOW, &v, 1, "linear 1.5e308");
    copy = rep ? copy_of (dwg, def, rep, 0xBBD3) : NULL;
    if (copy && copy->fixedtype == DWG_TYPE_LWPOLYLINE
        && copy->tio.entity->tio.LWPOLYLINE->num_points == 4
        && fabs (copy->tio.entity->tio.LWPOLYLINE->points[3].x / 1.5e308
                 - 1.0)
               < 1e-9)
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

  // the frame, BBD3, which 'Stretch Width' stretches, along X; found again,
  // as the evaluations added objects
  {
    Dwg_Entity_LWPOLYLINE *_frame
        = dwg_resolve_handle (dwg, 0xBBD3)->tio.entity->tio.LWPOLYLINE;
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
    // copies of the boundaries' objects, which stay where they are when
    // dwg_add_HATCH () adds the HATCH, and may move dwg->object, before it
    // reads them
    const Dwg_Object boundary_obj = dwg->object[boundary->parent->objid];
    const Dwg_Object crossing_obj = dwg->object[crossing->parent->objid];
    const Dwg_Object *pathobjs[1];
    pathobjs[0] = &boundary_obj;
    hatch = dwg_add_HATCH (def, 1, "SOLID", 0, 1, pathobjs);
    pathobjs[0] = &crossing_obj;
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
  const dwg_point_3d mc_pt = { -2.3, -1.25, 0.0 },
                     ml_pt = { 0.0, -1.25, 0.0 },
                     aligned_pt = { 1.0, 1.0, 0.0 };
  Dwg_Entity_TEXT *text, *centred, *middle_left, *aligned;
  Dwg_Entity_INSERT *insert;
  Dwg_Entity_POINT *point;
  Dwg_Object_BLOCK_HEADER *rep;
  Dwg_DynValue v;
  BITCODE_RLL h_text, h_centred, h_middle_left, h_aligned, h_insert;
  int failures = 0;
  memset (&v, 0, sizeof (v));
  if (!def || !move)
    {
      fail ("move: fixture objects not found");
      return 1;
    }
  text = dwg_add_TEXT (def, "caption", &pt, 1.0);
  centred = dwg_add_TEXT (def, "MC", &mc_pt, 2.5);
  middle_left = dwg_add_TEXT (def, "ML", &ml_pt, 2.5);
  aligned = dwg_add_TEXT (def, "aligned", &aligned_pt, 1.0);
  insert = dwg_add_INSERT (def, &pt, "t", 1.0, 1.0, 1.0, 0.0);
  if (insert && !dwg_add_ATTRIB (insert, 1.0, 0, &pt, "TAG", "value"))
    insert = NULL;
  point = dwg_add_POINT (def, &pt);
  loglevel = test_loglevel;
  if (!text || !centred || !middle_left || !aligned || !insert || !point)
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
  // aligned only vertically, middle-left on the block's base point
  middle_left->vert_alignment = 2;
  middle_left->dataflags = 0x7f;
  // aligned between its two points, the second stored
  aligned->horiz_alignment = 3;
  aligned->alignment_pt.x = 6.0;
  aligned->alignment_pt.y = 1.0;
  aligned->dataflags = 0xbd;
  h_text = entity_handle (dwg, text->parent);
  h_centred = entity_handle (dwg, centred->parent);
  h_middle_left = entity_handle (dwg, middle_left->parent);
  h_aligned = entity_handle (dwg, aligned->parent);
  h_insert = entity_handle (dwg, insert->parent);
  if (!add_move_entity (dwg, move, h_text)
      || !add_move_entity (dwg, move, h_centred)
      || !add_move_entity (dwg, move, h_middle_left)
      || !add_move_entity (dwg, move, h_aligned)
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
  if (rep
      && is_moved (dwg, dwg_resolve_handle (dwg, h_middle_left),
                   copy_of (dwg, def, rep, h_middle_left), 0.0, -0.5))
    OK ("move: the TEXT middle-left on (0,0) moves its alignment point by "
        "-0.5, then stored");
  else
    {
      fail ("move: the TEXT middle-left on (0,0) doesn't move its alignment "
            "point by -0.5, stored");
      failures++;
    }
  if (rep
      && is_moved (dwg, dwg_resolve_handle (dwg, h_aligned),
                   copy_of (dwg, def, rep, h_aligned), 0.0, -0.5))
    OK ("move: the aligned TEXT moves both its points by -0.5");
  else
    {
      fail ("move: the aligned TEXT doesn't move both its points by -0.5");
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

  // found again, as the evaluations added objects
  swing = graph_node (dwg, DOOR, DWG_TYPE_BLOCKFLIPPARAMETER, "swing");
  if (!swing)
    {
      fail ("selected nodes: the Door's swing not found again");
      return failures + 1;
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

/* A parameter an action selects (deps) without listing it among those it
   moves (a stretch's codes, a move's node ids) would move with the
   selection, and run its actions if it chains them: the evaluator refuses
   one that chains its actions, other than the action's own parameter.
   Here Window Height, added to 'Stretch Width''s selection, and the
   Door's flip parameter hinge, added to MoveSwing's. */
static int
test_selected_deps (Dwg_Data *dwg)
{
  Dwg_Object_BLOCKSTRETCHACTION *stretch
      = stretch_action (dwg, WINDOW, "Stretch Width");
  Dwg_Object_BLOCKMOVEACTION *move = move_action (dwg, DOOR, "MoveSwing");
  Dwg_Object_BLOCKLINEARPARAMETER *width
      = linear_param (dwg, WINDOW, "Window Width");
  Dwg_Object_BLOCKLINEARPARAMETER *height
      = linear_param (dwg, WINDOW, "Window Height");
  Dwg_Object *hinge
      = graph_node (dwg, DOOR, DWG_TYPE_BLOCKFLIPPARAMETER, "hinge");
  Dwg_DynValue v;
  BITCODE_RLL h_height, h_hinge;
  int failures = 0;
  memset (&v, 0, sizeof (v));
  if (!stretch || !move || !width || !height || !hinge)
    {
      fail ("selected deps: fixture objects not found");
      return 1;
    }
  h_height = dwg->object[height->parent->objid].handle.value;
  h_hinge = hinge->handle.value;

  // its own parameter, Window Width, is in 'Stretch Width''s selection
  v.name = (char *)"Window Width";
  v.value = 24.0;
  width->chain_actions = 1;
  if (evaluate (dwg, WINDOW, &v, 1, "a stretch selecting its own chained "
                                    "parameter"))
    OK ("selected deps: a stretch's own parameter may chain its actions");
  else
    failures++;
  width->chain_actions = 0;

  if (add_dep (dwg, &stretch->deps, &stretch->num_deps, h_height))
    {
      if (evaluate (dwg, WINDOW, &v, 1, "a stretch selecting another "
                                        "parameter"))
        OK ("selected deps: a stretch may select another parameter that "
            "doesn't chain its actions");
      else
        failures++;
      height->chain_actions = 1;
      failures += test_refusal (dwg, WINDOW, &v, 1,
                                REFUSED (DWG_ERR_NOTYETSUPPORTED),
                                "a stretch selecting, not listing, a "
                                "parameter chaining its actions");
      height->chain_actions = 0;
      stretch->num_deps--;
    }
  else
    {
      fail ("selected deps: out of memory");
      failures++;
    }

  v.name = (char *)"Wall Thickness";
  v.value = 6.0;
  // found again, as the evaluations added objects
  hinge = dwg_resolve_handle (dwg, h_hinge);
  if (hinge && add_move_entity (dwg, move, h_hinge))
    {
      Dwg_Object_BLOCKFLIPPARAMETER *_hinge
          = hinge->tio.object->tio.BLOCKFLIPPARAMETER;
      _hinge->chain_actions = 1;
      failures += test_refusal (dwg, DOOR, &v, 1,
                                REFUSED (DWG_ERR_NOTYETSUPPORTED),
                                "a move selecting, not listing, a parameter "
                                "chaining its actions");
      _hinge->chain_actions = 0;
      move->num_deps--;
    }
  else
    {
      fail ("selected deps: out of memory");
      failures++;
    }
  return failures;
}

/*--------------------------------------------------------------------------------
 * Rotation properties, and their rotate actions
 */

/* The Toilet's rotation parameter, Angle, and its rotate action 'Rotate
   Plan', which turns 65 of its entities about the origin, its base
   point. */
#define TOILET_ANGLE 0xB8FE
#define ROTATE_PLAN 0xB90C

static Dwg_Object_BLOCKROTATEACTION *
rotate_action (Dwg_Data *dwg, const BITCODE_RLL def, const char *name)
{
  Dwg_Object *obj = graph_node (dwg, def, DWG_TYPE_BLOCKROTATEACTION, name);
  return obj ? obj->tio.object->tio.BLOCKROTATEACTION : NULL;
}

/* Whether angles a and b are the same, around the circle. */
static int
is_angle (const double a, const double b)
{
  const double d = fmod (fabs (a - b), 2.0 * M_PI);
  return fmin (d, 2.0 * M_PI - d) <= TOLERANCE;
}

/* Whether angle a is in [0, 2 pi), as AutoCAD keeps a turned one. */
static int
is_normal (const double a)
{
  return a >= 0.0 && a < 2.0 * M_PI;
}

/* Whether the point x, y is sx, sy turned about the origin by angle. Both
   are in the OCS of an entity whose extrusion has the sign sign: one
   along -Z has its OCS X axis along the block's -X. */
static int
is_turned_xy (const double x, const double y, const double sx,
              const double sy, const double sign, const double angle)
{
  const double bx = sign * sx;
  return is_point (sign * x, y, bx * cos (angle) - sy * sin (angle),
                   bx * sin (angle) + sy * cos (angle));
}

#define TURNED_2D(b, a, sign, angle)                                          \
  is_turned_xy ((b).x, (b).y, (a).x, (a).y, sign, angle)

static double
ocs_sign (const BITCODE_BE *extrusion)
{
  return extrusion->z < 0.0 ? -1.0 : 1.0;
}

/* Whether a TEXT or ATTRIB b is a turned by angle: its insertion point,
   the alignment point of text that isn't left-aligned on the baseline,
   with dataflags 0x02 set only for an alignment point of (0,0), and its
   rotation, which dataflags says is stored unless it is 0. Left-aligned
   text keeps its alignment point and flag. */
#define IS_TURNED_TEXT(b, a, angle)                                           \
  (TURNED_2D ((b)->ins_pt, (a)->ins_pt, ocs_sign (&(a)->extrusion), angle)    \
   && ((a)->horiz_alignment || (a)->vert_alignment                            \
           ? TURNED_2D ((b)->alignment_pt, (a)->alignment_pt,                 \
                        ocs_sign (&(a)->extrusion), angle)                    \
                 && !((b)->dataflags & 0x02)                                  \
                        == ((b)->alignment_pt.x != 0.0                        \
                            || (b)->alignment_pt.y != 0.0)                    \
           : is_point ((b)->alignment_pt.x, (b)->alignment_pt.y,              \
                       (a)->alignment_pt.x, (a)->alignment_pt.y)              \
                 && ((b)->dataflags & 0x02) == ((a)->dataflags & 0x02))       \
   && is_angle ((b)->rotation,                                                \
                (a)->rotation + ocs_sign (&(a)->extrusion) * (angle))         \
   && is_normal ((b)->rotation)                                               \
   && !((b)->dataflags & 0x08) == ((b)->rotation != 0.0))

/* Whether copy is src turned whole about the origin by angle, counter-
   clockwise in the block: every point and direction of it, and the
   ATTRIBs of an INSERT. Angle 0 checks that it is unchanged. */
static int
is_turned (Dwg_Data *dwg, const Dwg_Object *src, const Dwg_Object *copy,
           const double angle)
{
  if (!src || !copy || src->fixedtype != copy->fixedtype)
    return 0;
  switch (src->fixedtype)
    {
    case DWG_TYPE_LINE:
      {
        const Dwg_Entity_LINE *a = src->tio.entity->tio.LINE;
        const Dwg_Entity_LINE *b = copy->tio.entity->tio.LINE;
        return TURNED_2D (b->start, a->start, 1.0, angle)
               && TURNED_2D (b->end, a->end, 1.0, angle)
               && b->start.z == a->start.z && b->end.z == a->end.z;
      }
    case DWG_TYPE_LWPOLYLINE:
      {
        const Dwg_Entity_LWPOLYLINE *a = src->tio.entity->tio.LWPOLYLINE;
        const Dwg_Entity_LWPOLYLINE *b = copy->tio.entity->tio.LWPOLYLINE;
        int same = a->num_points == b->num_points;
        for (BITCODE_BL i = 0; same && i < a->num_points; i++)
          same = TURNED_2D (b->points[i], a->points[i],
                            ocs_sign (&a->extrusion), angle);
        return same;
      }
    case DWG_TYPE_ARC:
      {
        const Dwg_Entity_ARC *a = src->tio.entity->tio.ARC;
        const Dwg_Entity_ARC *b = copy->tio.entity->tio.ARC;
        const double sign = ocs_sign (&a->extrusion);
        return TURNED_2D (b->center, a->center, sign, angle)
               && is_angle (b->start_angle, a->start_angle + sign * angle)
               && is_angle (b->end_angle, a->end_angle + sign * angle)
               && is_normal (b->start_angle) && is_normal (b->end_angle)
               && b->radius == a->radius;
      }
    case DWG_TYPE_CIRCLE:
      return TURNED_2D (copy->tio.entity->tio.CIRCLE->center,
                        src->tio.entity->tio.CIRCLE->center,
                        ocs_sign (&src->tio.entity->tio.CIRCLE->extrusion),
                        angle);
    case DWG_TYPE_ELLIPSE:
      {
        const Dwg_Entity_ELLIPSE *a = src->tio.entity->tio.ELLIPSE;
        const Dwg_Entity_ELLIPSE *b = copy->tio.entity->tio.ELLIPSE;
        // the major axis turns as a vector, which about the origin is the
        // same as a point
        return TURNED_2D (b->center, a->center, 1.0, angle)
               && TURNED_2D (b->sm_axis, a->sm_axis, 1.0, angle)
               && b->start_angle == a->start_angle
               && b->end_angle == a->end_angle;
      }
    case DWG_TYPE_SPLINE:
      {
        const Dwg_Entity_SPLINE *a = src->tio.entity->tio.SPLINE;
        const Dwg_Entity_SPLINE *b = copy->tio.entity->tio.SPLINE;
        int same = a->num_fit_pts == b->num_fit_pts
                   && a->num_ctrl_pts == b->num_ctrl_pts
                   && TURNED_2D (b->beg_tan_vec, a->beg_tan_vec, 1.0, angle)
                   && TURNED_2D (b->end_tan_vec, a->end_tan_vec, 1.0, angle);
        for (BITCODE_BS i = 0; same && i < a->num_fit_pts; i++)
          same = TURNED_2D (b->fit_pts[i], a->fit_pts[i], 1.0, angle);
        for (BITCODE_BL i = 0; same && i < a->num_ctrl_pts; i++)
          same = TURNED_2D (b->ctrl_pts[i], a->ctrl_pts[i], 1.0, angle);
        return same;
      }
    case DWG_TYPE_TEXT:
      return IS_TURNED_TEXT (copy->tio.entity->tio.TEXT,
                             src->tio.entity->tio.TEXT, angle);
    case DWG_TYPE_ATTRIB:
      return IS_TURNED_TEXT (copy->tio.entity->tio.ATTRIB,
                             src->tio.entity->tio.ATTRIB, angle);
    case DWG_TYPE_MTEXT:
      return TURNED_2D (copy->tio.entity->tio.MTEXT->ins_pt,
                        src->tio.entity->tio.MTEXT->ins_pt, 1.0, angle)
             && TURNED_2D (copy->tio.entity->tio.MTEXT->x_axis_dir,
                           src->tio.entity->tio.MTEXT->x_axis_dir, 1.0,
                           angle);
    case DWG_TYPE_INSERT:
      {
        const Dwg_Entity_INSERT *a = src->tio.entity->tio.INSERT;
        const Dwg_Entity_INSERT *b = copy->tio.entity->tio.INSERT;
        const double sign = ocs_sign (&a->extrusion);
        int same = TURNED_2D (b->ins_pt, a->ins_pt, sign, angle)
                   && is_angle (b->rotation, a->rotation + sign * angle)
                   && is_normal (b->rotation) && a->num_owned == b->num_owned;
        for (BITCODE_BL i = 0; same && a->attribs && i < a->num_owned; i++)
          same = is_turned (dwg, dwg_ref_object (dwg, a->attribs[i]),
                            dwg_ref_object (dwg, b->attribs[i]), angle);
        return same;
      }
    case DWG_TYPE_HATCH:
      {
        const Dwg_Entity_HATCH *a = src->tio.entity->tio.HATCH;
        const Dwg_Entity_HATCH *b = copy->tio.entity->tio.HATCH;
        const double sign = ocs_sign (&a->extrusion);
        int same
            = a->num_paths == b->num_paths && a->num_seeds == b->num_seeds;
        for (BITCODE_BL i = 0; same && i < a->num_paths; i++)
          {
            const Dwg_HATCH_Path *pa = &a->paths[i], *pb = &b->paths[i];
            same = pa->num_segs_or_paths == pb->num_segs_or_paths;
            for (BITCODE_BL j = 0; same && j < pa->num_segs_or_paths; j++)
              same = pa->flag & 2
                         ? TURNED_2D (pb->polyline_paths[j].point,
                                      pa->polyline_paths[j].point, sign,
                                      angle)
                               && pb->polyline_paths[j].bulge
                                      == pa->polyline_paths[j].bulge
                         : TURNED_2D (pb->segs[j].first_endpoint,
                                      pa->segs[j].first_endpoint, sign, angle)
                               && TURNED_2D (pb->segs[j].second_endpoint,
                                             pa->segs[j].second_endpoint,
                                             sign, angle);
          }
        for (BITCODE_BL i = 0; same && i < a->num_seeds; i++)
          same = TURNED_2D (b->seeds[i], a->seeds[i], sign, angle);
        return same;
      }
    case DWG_TYPE_POINT:
      {
        const Dwg_Entity_POINT *a = src->tio.entity->tio.POINT;
        const Dwg_Entity_POINT *b = copy->tio.entity->tio.POINT;
        return is_turned_xy (b->x, b->y, a->x, a->y, 1.0, angle)
               && is_angle (b->x_ang,
                            a->x_ang + ocs_sign (&a->extrusion) * angle)
               && is_normal (b->x_ang);
      }
    default:
      return 0;
    }
}

/* Whether entity handle is in the selection of rotate action a. */
static int
is_selected (const Dwg_Object_BLOCKROTATEACTION *a, const BITCODE_RLL handle)
{
  for (BITCODE_BL i = 0; a->deps && i < a->num_deps; i++)
    if (a->deps[i] && a->deps[i]->absolute_ref == handle)
      return 1;
  return 0;
}

/* Checks that the Toilet evaluated at Angle angle, as block rep, has the
   entities of 'Rotate Plan''s selection turned by turn, and the others
   unchanged. */
static int
check_toilet (Dwg_Data *dwg, const Dwg_Object_BLOCK_HEADER *rep,
              const double turn, const char *what)
{
  const Dwg_Object_BLOCK_HEADER *def = block_header (dwg, TOILET);
  const Dwg_Object_BLOCKROTATEACTION *a
      = rotate_action (dwg, TOILET, "Rotate Plan");
  BITCODE_BL turned = 0, kept = 0;
  if (!def || !a || !rep || rep->num_owned != def->num_owned)
    {
      fail ("%s: no block like the Toilet", what);
      return 1;
    }
  for (BITCODE_BL i = 0; i < def->num_owned; i++)
    {
      const Dwg_Object *src = dwg_ref_object (dwg, def->entities[i]);
      const Dwg_Object *copy = dwg_ref_object (dwg, rep->entities[i]);
      const int selected = src && is_selected (a, src->handle.value);
      if (!is_turned (dwg, src, copy, selected ? turn : 0.0))
        {
          fail ("%s: %s " FORMAT_HV " is not %s", what,
                src ? dwg_type_name (src->fixedtype) : "?",
                src ? src->handle.value : 0,
                selected ? "turned" : "as it was");
          return 1;
        }
      if (selected)
        turned++;
      else
        kept++;
    }
  if (turned != 65 || kept != 150)
    {
      fail ("%s: %u entities turned, %u kept, expected 65 and 150", what,
            (unsigned)turned, (unsigned)kept);
      return 1;
    }
  if (turn != 0.0)
    OK ("%s: the 65 entities of the selection turned, 150 as they were",
        what);
  else
    OK ("%s: all 215 entities as they were", what);
  return 0;
}

/* The Toilet at Angle 1.2: 'Rotate Plan' turns its selection, LINEs, ARCs
   and LWPOLYLINEs, about the origin by 1.2, and leaves the rest. A value
   a whole turn away gives the same block, as AutoCAD keeps the angles a
   turn changes in [0, 2 pi); one at the definition's value, 0 or 2 pi,
   turns nothing; a huge one, 1e20, turns by its remainder, the points and
   the angles alike. A huge value whose remainder is 0, 2 pi times 2^60,
   with the definition's angle made 1, turns by -1: subtracting 1 from the
   value itself would round it away. */
static int
test_rotation (Dwg_Data *dwg)
{
  static const double values[] = { 1.2, 1.2 - 2.0 * M_PI, 1.2 + 4.0 * M_PI };
  Dwg_DynValue v;
  int failures = 0;
  memset (&v, 0, sizeof (v));
  v.name = (char *)"Angle";
  for (size_t i = 0; i < sizeof (values) / sizeof (values[0]); i++)
    {
      char what[64];
      snprintf (what, sizeof (what), "rotation Angle %.4f", values[i]);
      v.value = values[i];
      failures += check_toilet (dwg, evaluate (dwg, TOILET, &v, 1, what),
                                1.2, what);
    }
  v.value = 2.0 * M_PI;
  failures += check_toilet (
      dwg, evaluate (dwg, TOILET, &v, 1, "rotation Angle 2 pi"), 0.0,
      "rotation Angle 2 pi, the definition's");
  // a huge value turns points and angles alike, by its remainder
  v.value = 1e20;
  failures += check_toilet (
      dwg, evaluate (dwg, TOILET, &v, 1, "rotation Angle 1e20"),
      fmod (1e20, 2.0 * M_PI), "rotation Angle 1e20");
  {
    Dwg_Object *obj = dwg_resolve_handle (dwg, TOILET_ANGLE);
    Dwg_Object_BLOCKROTATIONPARAMETER *rotation
        = obj && obj->fixedtype == DWG_TYPE_BLOCKROTATIONPARAMETER
              ? obj->tio.object->tio.BLOCKROTATIONPARAMETER
              : NULL;
    BITCODE_3BD endpt;
    double dx, dy;
    if (!rotation)
      {
        fail ("rotation: the Toilet's Angle not found");
        return failures + 1;
      }
    // the end point turned 1 from the base angle point, about the base
    endpt = rotation->def_endpt;
    dx = rotation->def_base_angle_pt.x - rotation->def_basept.x;
    dy = rotation->def_base_angle_pt.y - rotation->def_basept.y;
    rotation->def_endpt.x = rotation->def_basept.x + dx * cos (1.0)
                            - dy * sin (1.0);
    rotation->def_endpt.y = rotation->def_basept.y + dx * sin (1.0)
                            + dy * cos (1.0);
    v.value = ldexp (2.0 * M_PI, 60);
    failures += check_toilet (
        dwg, evaluate (dwg, TOILET, &v, 1, "rotation Angle 2 pi 2^60"), -1.0,
        "rotation Angle 2 pi 2^60, from 1");
    rotation->def_endpt = endpt;
  }
  return failures;
}

/* Entities of each type the evaluator turns, added to the Toilet and to
   'Rotate Plan''s selection: at Angle 1.2 each turns by 1.2 about the
   origin, an entity whose extrusion is -Z the other way in its OCS. A
   TEXT whose rotation the decoder left unstored (dataflags 0x08) gets it
   stored, and keeps it written as r2004 and read back. Then what the
   evaluator refuses to turn. */
static int
test_turn_whole (Dwg_Data *dwg)
{
  Dwg_Object_BLOCK_HEADER *def = block_header (dwg, TOILET);
  Dwg_Object_BLOCKROTATEACTION *act
      = rotate_action (dwg, TOILET, "Rotate Plan");
  const dwg_point_2d square[4]
      = { { 40.0, 0.0 }, { 42.0, 0.0 }, { 42.0, 2.0 }, { 40.0, 2.0 } };
  const dwg_point_2d tri[3] = { { -29.0, 15.0 }, { -31.0, 15.0 },
                                { -31.0, 17.0 } };
  const dwg_point_3d fit[3] = { { 29.0, 27.0, 0.0 },
                                { 30.0, 28.5, 0.0 },
                                { 31.0, 27.0, 0.0 } };
  const dwg_point_3d tan = { 1.0, 1.0, 0.0 };
  dwg_point_3d pt = { 30.0, 4.0, 0.0 };
  Added list[20];
  int n = 0, failures = 0, error;
  Dwg_Entity_CIRCLE *circle, *mirrored, *boundary;
  Dwg_Entity_ARC *arc;
  Dwg_Entity_LWPOLYLINE *lwpline, *closed, *bulged;
  Dwg_Entity_ELLIPSE *ellipse;
  Dwg_Entity_SPLINE *spline;
  Dwg_Entity_TEXT *text, *centred;
  Dwg_Entity_MTEXT *mtext;
  Dwg_Entity_INSERT *insert;
  Dwg_Entity_ATTDEF *attdef;
  Dwg_Entity_POINT *point, *mpoint;
  Dwg_Entity_RAY *ray;
  Dwg_Entity_HATCH *hatch, *bulged_hatch, *arc_hatch;
  Dwg_Object_BLOCK_HEADER *rep;
  Dwg_DynValue v;
  BITCODE_RLL h_text = 0, rep_handle;
  Dwg_Data readback;
  memset (&v, 0, sizeof (v));
  if (!def || !act)
    {
      fail ("turn whole: fixture objects not found");
      return 1;
    }
  circle = dwg_add_CIRCLE (def, &pt, 1.0);
  pt.x = -30.0; // at x 30 in the block
  pt.y = 8.0;
  mirrored = dwg_add_CIRCLE (def, &pt, 1.0);
  pt.y = 12.0;
  arc = dwg_add_ARC (def, &pt, 1.0, 0.3, 2.0);
  lwpline = dwg_add_LWPOLYLINE (def, 3, tri);
  pt.x = 30.0;
  pt.y = 20.0;
  ellipse = dwg_add_ELLIPSE (def, &pt, 2.0, 0.5);
  spline = dwg_add_SPLINE (def, 3, fit, &tan, &tan);
  pt.y = 31.0;
  text = dwg_add_TEXT (def, "left", &pt, 1.0);
  pt.y = 33.0;
  centred = dwg_add_TEXT (def, "centred", &pt, 1.0);
  pt.y = 40.0;
  mtext = dwg_add_MTEXT (def, &pt, 4.0, "mtext");
  pt.y = 44.0;
  insert = dwg_add_INSERT (def, &pt, "t", 1.0, 1.0, 1.0, 0.2);
  pt.x = 31.0;
  if (insert && !dwg_add_ATTRIB (insert, 1.0, 0, &pt, "TAG", "value"))
    insert = NULL;
  closed = dwg_add_LWPOLYLINE (def, 4, square);
  bulged = dwg_add_LWPOLYLINE (def, 4, square);
  pt.x = 30.0;
  pt.y = 56.0;
  point = dwg_add_POINT (def, &pt);
  pt.y = 58.0;
  mpoint = dwg_add_POINT (def, &pt);
  pt.y = 60.0;
  attdef = dwg_add_ATTDEF (def, 1.0, 0, "Prompt", &pt, "TURNTAG", "default");
  ray = dwg_add_RAY (def, &pt, &tan);
  pt.y = 64.0;
  boundary = dwg_add_CIRCLE (def, &pt, 1.0);
  loglevel = test_loglevel;
  if (!circle || !mirrored || !arc || !lwpline || !ellipse || !spline
      || !text || !centred || !mtext || !insert || !closed || !bulged
      || !point || !mpoint || !attdef || !ray || !boundary)
    {
      fail ("turn whole: could not add the entities");
      return 1;
    }
  mirrored->extrusion.z = arc->extrusion.z = lwpline->extrusion.z = -1.0;
  mpoint->extrusion.z = -1.0;
  ellipse->sm_axis.x = 2.0;
  ellipse->sm_axis.y = 0.5;
  // as the decoder leaves a left-aligned text at rotation 0
  text->dataflags = 0xFF;
  centred->horiz_alignment = 1;
  centred->alignment_pt.x = 30.5;
  centred->alignment_pt.y = 33.0;
  centred->rotation = 0.3;
  point->x_ang = mpoint->x_ang = 0.4;
  closed->flag |= 512;
  bulged->flag |= 512;
  {
    // a bulge on each edge makes dwg_add_HATCH () give a polyline path
    BITCODE_BD *bulges = (BITCODE_BD *)calloc (4, sizeof (BITCODE_BD));
    if (!bulges)
      {
        fail ("turn whole: out of memory");
        return 1;
      }
    bulges[0] = 0.5;
    bulged->bulges = bulges;
    bulged->num_bulges = 4;
    bulged->flag |= 16;
  }
  {
    // copies of the boundaries' objects, which stay where they are when
    // dwg_add_HATCH () adds the HATCH, and may move dwg->object, before it
    // reads them
    const Dwg_Object closed_obj = dwg->object[closed->parent->objid];
    const Dwg_Object bulged_obj = dwg->object[bulged->parent->objid];
    const Dwg_Object boundary_obj = dwg->object[boundary->parent->objid];
    const Dwg_Object *pathobjs[1];
    pathobjs[0] = &closed_obj;
    hatch = dwg_add_HATCH (def, 1, "SOLID", 0, 1, pathobjs);
    pathobjs[0] = &bulged_obj;
    bulged_hatch = dwg_add_HATCH (def, 1, "SOLID", 0, 1, pathobjs);
    pathobjs[0] = &boundary_obj;
    arc_hatch = dwg_add_HATCH (def, 1, "SOLID", 0, 1, pathobjs);
    loglevel = test_loglevel;
    if (!hatch || !bulged_hatch || !arc_hatch || hatch->num_paths != 1
        || hatch->paths[0].flag & 2 || !(bulged_hatch->paths[0].flag & 2))
      {
        fail ("turn whole: could not add the HATCHes");
        return 1;
      }
    hatch->extrusion.z = bulged_hatch->extrusion.z = 1.0;
    arc_hatch->extrusion.z = 1.0;
    // a seed point each
    hatch->seeds = (BITCODE_2RD *)calloc (1, sizeof (BITCODE_2RD));
    if (!hatch->seeds)
      {
        fail ("turn whole: out of memory");
        return 1;
      }
    hatch->seeds[0].x = 41.0;
    hatch->seeds[0].y = 1.0;
    hatch->num_seeds = 1;
  }

  list[n++] = added (dwg, circle->parent, 0, "CIRCLE");
  list[n++] = added (dwg, mirrored->parent, 0, "CIRCLE mirrored");
  list[n++] = added (dwg, arc->parent, 0, "ARC mirrored");
  list[n++] = added (dwg, lwpline->parent, 0, "LWPOLYLINE mirrored");
  list[n++] = added (dwg, ellipse->parent, 0, "ELLIPSE");
  list[n++] = added (dwg, spline->parent, 0, "SPLINE");
  list[n++] = added (dwg, text->parent, 0, "TEXT, rotation unstored");
  list[n++] = added (dwg, centred->parent, 0, "TEXT centred, rotated");
  list[n++] = added (dwg, mtext->parent, 0, "MTEXT");
  list[n++] = added (dwg, insert->parent, 0, "INSERT with an ATTRIB");
  list[n++] = added (dwg, closed->parent, 0, "LWPOLYLINE, a boundary");
  list[n++] = added (dwg, hatch->parent, 0, "HATCH of lines, with a seed");
  list[n++] = added (dwg, bulged->parent, 0, "LWPOLYLINE with a bulge");
  list[n++] = added (dwg, bulged_hatch->parent, 0, "HATCH of a polyline");
  list[n++] = added (dwg, point->parent, 0, "POINT");
  list[n++] = added (dwg, mpoint->parent, 0, "POINT mirrored");
  for (int i = 0; i < n; i++)
    if (!add_dep (dwg, &act->deps, &act->num_deps, list[i].handle))
      {
        fail ("turn whole: out of memory");
        return 1;
      }

  v.name = (char *)"Angle";
  v.value = 1.2;
  rep = evaluate (dwg, TOILET, &v, 1, "turn whole");
  if (!rep)
    return 1;
  rep_handle = block_handle (rep);
  for (int i = 0; i < n; i++)
    {
      const Dwg_Object *copy = copy_of (dwg, def, rep, list[i].handle);
      if (is_turned (dwg, dwg_resolve_handle (dwg, list[i].handle), copy, 1.2))
        OK ("turn whole: the %s turns by 1.2", list[i].what);
      else
        {
          fail ("turn whole: the %s doesn't turn by 1.2", list[i].what);
          failures++;
        }
      if (list[i].handle == entity_handle (dwg, text->parent) && copy)
        h_text = copy->handle.value;
    }

  // Written as r2004 and read back, the TEXT keeps its rotation.
  unlink (WRITTEN);
  loglevel = tracelevel;
  error = dwg_write_file (WRITTEN, dwg);
  loglevel = test_loglevel;
  if (error >= DWG_ERR_CRITICAL || !h_text)
    {
      fail ("turn whole: write r2004: 0x%x", error);
      failures++;
    }
  else
    {
      error = read_dwg (WRITTEN, &readback);
      {
        const Dwg_Object *t = error & ~KNOWN_ERRORS
                                  ? NULL
                                  : dwg_resolve_handle (&readback, h_text);
        const Dwg_Object_BLOCK_HEADER *blk
            = t ? block_header (&readback, rep_handle) : NULL;
        if (t && blk && t->fixedtype == DWG_TYPE_TEXT
            && is_angle (t->tio.entity->tio.TEXT->rotation, 1.2)
            && !(t->tio.entity->tio.TEXT->dataflags & 0x08))
          OK ("turn whole: read back, the TEXT keeps its rotation, 1.2");
        else
          {
            fail ("turn whole: read back 0x%x, the TEXT's rotation %g", error,
                  t && t->fixedtype == DWG_TYPE_TEXT
                      ? t->tio.entity->tio.TEXT->rotation
                      : -1.0);
            failures++;
          }
      }
      free_dwg (&readback);
    }
  unlink (WRITTEN);

  // what the evaluator refuses to turn: an ATTDEF, as AutoCAD left one a
  // rotate action selected as it was; a RAY; a HATCH on an arc, or with a
  // pattern; a CIRCLE not in the XY plane; a LINE turned beyond what a
  // double holds; and an entity the action lists twice
  {
    const BITCODE_RLL h_attdef = entity_handle (dwg, attdef->parent);
    const BITCODE_RLL h_ray = entity_handle (dwg, ray->parent);
    const BITCODE_RLL h_arc_hatch = entity_handle (dwg, arc_hatch->parent);
    const BITCODE_RLL h_circle = entity_handle (dwg, circle->parent);
    Dwg_Entity_LINE *line = NULL;
    for (BITCODE_BL j = 0; !line && j < act->num_deps; j++)
      {
        Dwg_Object *o = dwg_ref_object (dwg, act->deps[j]);
        if (o && o->fixedtype == DWG_TYPE_LINE)
          line = o->tio.entity->tio.LINE;
      }
    if (!line)
      {
        fail ("turn whole: no LINE in 'Rotate Plan''s selection");
        return failures + 1;
      }
    if (add_dep (dwg, &act->deps, &act->num_deps, h_attdef))
      {
        failures += test_refusal (dwg, TOILET, &v, 1,
                                  REFUSED (DWG_ERR_NOTYETSUPPORTED),
                                  "a turn of an ATTDEF");
        act->num_deps--;
      }
    if (add_dep (dwg, &act->deps, &act->num_deps, h_ray))
      {
        failures += test_refusal (dwg, TOILET, &v, 1,
                                  REFUSED (DWG_ERR_NOTYETSUPPORTED),
                                  "a turn of a RAY");
        act->num_deps--;
      }
    if (add_dep (dwg, &act->deps, &act->num_deps, h_arc_hatch))
      {
        failures += test_refusal (dwg, TOILET, &v, 1,
                                  REFUSED (DWG_ERR_NOTYETSUPPORTED),
                                  "a turn of a HATCH on an arc");
        act->num_deps--;
      }
    hatch->is_solid_fill = 0;
    failures += test_refusal (dwg, TOILET, &v, 1,
                              REFUSED (DWG_ERR_NOTYETSUPPORTED),
                              "a turn of a HATCH with a pattern");
    hatch->is_solid_fill = 1;
    circle->extrusion.x = 1.0;
    circle->extrusion.z = 0.0;
    failures += test_refusal (dwg, TOILET, &v, 1,
                              REFUSED (DWG_ERR_NOTYETSUPPORTED),
                              "a turn of a CIRCLE not in the XY plane");
    circle->extrusion.x = 0.0;
    circle->extrusion.z = 1.0;
    {
      const double x = line->start.x;
      line->start.x = 1e308;
      failures += test_refusal (dwg, TOILET, &v, 1,
                                REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                                "a turn beyond what a double holds");
      line->start.x = x;
    }
    if (add_dep (dwg, &act->deps, &act->num_deps, h_circle))
      {
        failures += test_refusal (dwg, TOILET, &v, 1,
                                  REFUSED (DWG_ERR_NOTYETSUPPORTED),
                                  "an entity the rotate action lists twice");
        act->num_deps--;
      }
  }
  if (evaluate (dwg, TOILET, &v, 1, "turn whole, restored"))
    OK ("turn whole: the Toilet evaluates again");
  else
    failures++;
  return failures;
}

/* 'Rotate Plan' turning texts about a base point that isn't the origin:
   the Toilet's Angle parameter moved by (2, 3). A TEXT middle-centred on
   (0,0), whose alignment point AutoCAD leaves out (dataflags 0x02), turns
   its alignment point too, which is then stored, and so does one
   middle-left on (0,0); a left-aligned TEXT keeps its alignment point
   (0,0). */
static int
test_turn_text_anchor (Dwg_Data *dwg)
{
  Dwg_Object_BLOCK_HEADER *def = block_header (dwg, TOILET);
  Dwg_Object_BLOCKROTATEACTION *act
      = rotate_action (dwg, TOILET, "Rotate Plan");
  Dwg_Object *param_obj = dwg_resolve_handle (dwg, TOILET_ANGLE);
  Dwg_Object_BLOCKROTATIONPARAMETER *param
      = param_obj && param_obj->fixedtype == DWG_TYPE_BLOCKROTATIONPARAMETER
            ? param_obj->tio.object->tio.BLOCKROTATIONPARAMETER
            : NULL;
  const dwg_point_3d mc_pt = { -2.25, -1.25, 0.0 }, left_pt = { 6.0, 1.0, 0.0 },
                     ml_pt = { 0.0, -1.25, 0.0 };
  const double dx = 2.0, dy = 3.0, angle = 1.2;
  Dwg_Entity_TEXT *centred, *left, *middle_left;
  BITCODE_3BD basept, endpt, base_angle_pt;
  BITCODE_RLL h_centred, h_left, h_middle_left;
  BITCODE_BL num_deps;
  Dwg_Object_BLOCK_HEADER *rep;
  Dwg_DynValue v;
  int failures = 0;
  memset (&v, 0, sizeof (v));
  if (!def || !act || !param)
    {
      fail ("turn text anchor: fixture objects not found");
      return 1;
    }
  centred = dwg_add_TEXT (def, "MC", &mc_pt, 2.5);
  left = dwg_add_TEXT (def, "left", &left_pt, 1.0);
  middle_left = dwg_add_TEXT (def, "ML", &ml_pt, 2.5);
  loglevel = test_loglevel;
  if (!centred || !left || !middle_left)
    {
      fail ("turn text anchor: could not add the TEXTs");
      return 1;
    }
  centred->horiz_alignment = 1;
  centred->vert_alignment = 2;
  centred->dataflags = 0x3f;
  left->dataflags = 0xff;
  middle_left->vert_alignment = 2;
  middle_left->dataflags = 0x7f;
  h_centred = entity_handle (dwg, centred->parent);
  h_left = entity_handle (dwg, left->parent);
  h_middle_left = entity_handle (dwg, middle_left->parent);
  num_deps = act->num_deps;
  if (!add_dep (dwg, &act->deps, &act->num_deps, h_centred)
      || !add_dep (dwg, &act->deps, &act->num_deps, h_left)
      || !add_dep (dwg, &act->deps, &act->num_deps, h_middle_left))
    {
      fail ("turn text anchor: out of memory");
      return 1;
    }
  basept = param->def_basept;
  endpt = param->def_endpt;
  base_angle_pt = param->def_base_angle_pt;
  param->def_basept.x += dx;
  param->def_basept.y += dy;
  param->def_endpt.x += dx;
  param->def_endpt.y += dy;
  param->def_base_angle_pt.x += dx;
  param->def_base_angle_pt.y += dy;

  v.name = (char *)"Angle";
  v.value = angle;
  rep = evaluate (dwg, TOILET, &v, 1, "turn text anchor");
  if (rep)
    {
      const double cx = param->def_basept.x, cy = param->def_basept.y;
      const double c = cos (angle), s = sin (angle);
      const Dwg_Object *a = copy_of (dwg, def, rep, h_centred);
      const Dwg_Object *b = copy_of (dwg, def, rep, h_left);
      const Dwg_Entity_TEXT *ca
          = a && a->fixedtype == DWG_TYPE_TEXT ? a->tio.entity->tio.TEXT : NULL;
      const Dwg_Entity_TEXT *cb
          = b && b->fixedtype == DWG_TYPE_TEXT ? b->tio.entity->tio.TEXT : NULL;
      const Dwg_Object *m = copy_of (dwg, def, rep, h_middle_left);
      const Dwg_Entity_TEXT *cm
          = m && m->fixedtype == DWG_TYPE_TEXT ? m->tio.entity->tio.TEXT : NULL;
      // (0,0) turned about (cx, cy)
      const double ax = cx - cx * c + cy * s, ay = cy - cx * s - cy * c;
      if (ca && is_point (ca->alignment_pt.x, ca->alignment_pt.y, ax, ay)
          && is_point (ca->ins_pt.x, ca->ins_pt.y,
                       cx + (mc_pt.x - cx) * c - (mc_pt.y - cy) * s,
                       cy + (mc_pt.x - cx) * s + (mc_pt.y - cy) * c)
          && !(ca->dataflags & 0x02))
        OK ("turn text anchor: the TEXT centred on (0,0) turns its alignment "
            "point to (%g, %g), then stored",
            ca->alignment_pt.x, ca->alignment_pt.y);
      else
        {
          fail ("turn text anchor: the TEXT centred on (0,0) has its "
                "alignment point at (%g, %g), dataflags 0x%x, expected "
                "(%g, %g) stored",
                ca ? ca->alignment_pt.x : -1.0, ca ? ca->alignment_pt.y : -1.0,
                ca ? (unsigned)ca->dataflags : 0u, ax, ay);
          failures++;
        }
      if (cb && cb->alignment_pt.x == 0.0 && cb->alignment_pt.y == 0.0
          && cb->dataflags & 0x02
          && is_point (cb->ins_pt.x, cb->ins_pt.y,
                       cx + (left_pt.x - cx) * c - (left_pt.y - cy) * s,
                       cy + (left_pt.x - cx) * s + (left_pt.y - cy) * c))
        OK ("turn text anchor: the left-aligned TEXT turns, its alignment "
            "point stays (0,0), not stored");
      else
        {
          fail ("turn text anchor: the left-aligned TEXT's alignment point "
                "(%g, %g), dataflags 0x%x",
                cb ? cb->alignment_pt.x : -1.0, cb ? cb->alignment_pt.y : -1.0,
                cb ? (unsigned)cb->dataflags : 0u);
          failures++;
        }
      if (cm && is_point (cm->alignment_pt.x, cm->alignment_pt.y, ax, ay)
          && is_point (cm->ins_pt.x, cm->ins_pt.y,
                       cx + (ml_pt.x - cx) * c - (ml_pt.y - cy) * s,
                       cy + (ml_pt.x - cx) * s + (ml_pt.y - cy) * c)
          && !(cm->dataflags & 0x02))
        OK ("turn text anchor: the TEXT middle-left on (0,0) turns its "
            "alignment point to (%g, %g), then stored",
            cm->alignment_pt.x, cm->alignment_pt.y);
      else
        {
          fail ("turn text anchor: the TEXT middle-left on (0,0) has its "
                "alignment point at (%g, %g), dataflags 0x%x, expected "
                "(%g, %g) stored",
                cm ? cm->alignment_pt.x : -1.0, cm ? cm->alignment_pt.y : -1.0,
                cm ? (unsigned)cm->dataflags : 0u, ax, ay);
          failures++;
        }
    }
  else
    failures++;
  param->def_basept = basept;
  param->def_endpt = endpt;
  param->def_base_angle_pt = base_angle_pt;
  act->num_deps = num_deps;
  return failures;
}

/* The Toilet's rotation parameter and 'Rotate Plan', moved into the
   Window's evaluation graph under node ids it doesn't use, with the
   action turning the Window's LINE BBD5, which only 'Stretch Height'
   stretches: a definition with linear and rotation properties. Their
   actions run in an order AutoCAD's evaluation doesn't fix unless they
   are independent, so the evaluator refuses them when they share an
   entity, when an action that runs moves the rotation parameter, or when
   the rotate action that runs turns a linear parameter that runs, listed
   or only selected. */
typedef struct
{
  Dwg_Object_BLOCKROTATIONPARAMETER *param;
  Dwg_Object_BLOCKROTATEACTION *action;
  BITCODE_BL param_id, action_id, num_deps, num_actions, num_nodes;
  BITCODE_BL codes[3];
  BITCODE_H *deps;
  BITCODE_BL *actions;
  BITCODE_H line[2];
} Graft;

#define GRAFT_PARAM 905
#define GRAFT_ACTION 920

static int
graft_rotation (Dwg_Data *dwg, Graft *g)
{
  Dwg_Object *param = dwg_resolve_handle (dwg, TOILET_ANGLE);
  Dwg_Object *action = dwg_resolve_handle (dwg, ROTATE_PLAN);
  Dwg_Object_EVALUATION_GRAPH *graph = definition_graph (dwg, WINDOW);
  Dwg_EVAL_Node *nodes;
  memset (g, 0, sizeof (*g));
  if (!param || param->fixedtype != DWG_TYPE_BLOCKROTATIONPARAMETER || !action
      || action->fixedtype != DWG_TYPE_BLOCKROTATEACTION || !graph)
    return 0;
  nodes = (Dwg_EVAL_Node *)realloc (
      graph->nodes, (graph->num_nodes + 2) * sizeof (Dwg_EVAL_Node));
  if (!nodes)
    return 0;
  graph->nodes = nodes;
  memset (&nodes[graph->num_nodes], 0, 2 * sizeof (Dwg_EVAL_Node));
  nodes[graph->num_nodes].evalexpr
      = dwg_add_handleref (dwg, 5, TOILET_ANGLE, NULL);
  nodes[graph->num_nodes + 1].evalexpr
      = dwg_add_handleref (dwg, 5, ROTATE_PLAN, NULL);
  g->num_nodes = graph->num_nodes;
  graph->num_nodes += 2;
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
  g->line[0] = g->line[1] = dwg_add_handleref (dwg, 4, 0xBBD5, NULL);
  g->action->deps = g->line;
  g->action->num_deps = 1;
  g->action->num_actions = 0;
  return 1;
}

static void
ungraft_rotation (Dwg_Data *dwg, Graft *g)
{
  definition_graph (dwg, WINDOW)->num_nodes = g->num_nodes;
  g->param->evalexpr.nodeid = g->param_id;
  g->action->evalexpr.nodeid = g->action_id;
  for (int i = 0; i < 3; i++)
    g->action->conn_pts[i].code = g->codes[i];
  g->action->deps = g->deps;
  g->action->num_deps = g->num_deps;
  g->action->actions = g->actions;
  g->action->num_actions = g->num_actions;
}

static int
test_rotation_graft (Dwg_Data *dwg)
{
  const Dwg_Object_BLOCK_HEADER *def = block_header (dwg, WINDOW);
  Dwg_Object_BLOCKSTRETCHACTION *width_stretch
      = stretch_action (dwg, WINDOW, "Stretch Width");
  Dwg_Object_BLOCKSTRETCHACTION *divider
      = stretch_action (dwg, WINDOW, "Stretch Divider");
  Dwg_Object_BLOCKLINEARPARAMETER *width
      = linear_param (dwg, WINDOW, "Window Width");
  const Dwg_Object_BLOCK_HEADER *rep;
  Dwg_DynValue v[2];
  BITCODE_BL width_action = 0;
  Graft g;
  int failures = 0;
  memset (v, 0, sizeof (v));
  if (!def || !width_stretch || !divider || !width
      || !graft_rotation (dwg, &g))
    {
      fail ("rotation graft: fixture objects not found");
      return 1;
    }
  v[0].name = (char *)"Angle";
  v[0].value = 1.2;
  v[1].name = (char *)"Window Width";
  v[1].value = 24.0;

  rep = evaluate (dwg, WINDOW, v, 1, "rotation graft, Angle");
  if (rep
      && is_turned (dwg, dwg_resolve_handle (dwg, 0xBBD5),
                    copy_of (dwg, def, rep, 0xBBD5), 1.2)
      && is_turned (dwg, dwg_resolve_handle (dwg, 0xBBD3),
                    copy_of (dwg, def, rep, 0xBBD3), 0.0))
    OK ("rotation graft: Angle 1.2 turns the LINE, and nothing else");
  else
    {
      fail ("rotation graft: Angle 1.2 doesn't turn the LINE alone");
      failures++;
    }
  // Width's stretches don't move BBD5: as in test_linear, vertex 2 of the
  // frame moves to (24, 36)
  rep = evaluate (dwg, WINDOW, v, 2, "rotation graft, Angle and Width");
  {
    const Dwg_Object *frame = rep ? copy_of (dwg, def, rep, 0xBBD3) : NULL;
    if (rep && frame && frame->fixedtype == DWG_TYPE_LWPOLYLINE
        && is_turned (dwg, dwg_resolve_handle (dwg, 0xBBD5),
                      copy_of (dwg, def, rep, 0xBBD5), 1.2)
        && is_point (frame->tio.entity->tio.LWPOLYLINE->points[2].x,
                     frame->tio.entity->tio.LWPOLYLINE->points[2].y, 24.0,
                     36.0))
      OK ("rotation graft: Angle and Width, independent, both apply");
    else
      {
        fail ("rotation graft: Angle and Width don't both apply");
        failures++;
      }
  }
  v[1].name = (char *)"Window Height";
  v[1].value = 54.0;
  failures += test_refusal (dwg, WINDOW, v, 2,
                            REFUSED (DWG_ERR_NOTYETSUPPORTED),
                            "an entity turned and stretched");
  v[1].name = (char *)"Window Width";
  v[1].value = 24.0;

  // 'Stretch Width' moving the rotation parameter's base point
  if (add_stretch_code (width_stretch, GRAFT_PARAM, 0))
    {
      failures += test_refusal (dwg, WINDOW, v, 2,
                                REFUSED (DWG_ERR_NOTYETSUPPORTED),
                                "a stretch moving a rotation parameter "
                                "given a value");
      if (evaluate (dwg, WINDOW, &v[1], 1, "rotation graft, moved, no Angle"))
        OK ("rotation graft: a stretch may move a rotation parameter given "
            "no value");
      else
        failures++;
      pop_stretch_code (width_stretch);
    }
  // and selecting it without listing it
  if (add_dep (dwg, &width_stretch->deps, &width_stretch->num_deps,
               TOILET_ANGLE))
    {
      failures += test_refusal (dwg, WINDOW, v, 2,
                                REFUSED (DWG_ERR_NOTYETSUPPORTED),
                                "a stretch selecting, not listing, a "
                                "rotation parameter given a value");
      width_stretch->num_deps--;
    }
  // 'Rotate Plan' turning Window Width's parameter
  width_action = width->evalexpr.nodeid;
  g.action->actions = &width_action;
  g.action->num_actions = 1;
  failures += test_refusal (dwg, WINDOW, v, 2,
                            REFUSED (DWG_ERR_NOTYETSUPPORTED),
                            "a turn of a linear parameter whose value "
                            "changes");
  v[1].value = 36.0;
  if (evaluate (dwg, WINDOW, v, 2, "rotation graft, Width at its length"))
    OK ("rotation graft: a rotate action may turn a linear parameter that "
        "doesn't run");
  else
    failures++;
  v[1].value = 24.0;
  g.action->num_actions = 0;
  // and selecting Window Width's parameter without listing it
  g.line[1] = dwg_add_handleref (
      dwg, 4, dwg->object[width->parent->objid].handle.value, NULL);
  g.action->num_deps = 2;
  failures += test_refusal (dwg, WINDOW, v, 2,
                            REFUSED (DWG_ERR_NOTYETSUPPORTED),
                            "a rotate action selecting, not listing, a linear "
                            "parameter whose value changes");
  v[1].value = 36.0;
  if (evaluate (dwg, WINDOW, v, 2, "rotation graft, Width selected"))
    OK ("rotation graft: a rotate action may select a linear parameter that "
        "doesn't run");
  else
    failures++;
  v[1].value = 24.0;
  g.action->num_deps = 1;
  g.line[1] = g.line[0];
  // connections, base point and offset the evaluator doesn't support
  {
    BITCODE_T name = g.action->conn_pts[2].name;
    g.action->conn_pts[2].name = (char *)"UpdatedAngle";
    failures += test_refusal (dwg, WINDOW, v, 1,
                              REFUSED (DWG_ERR_NOTYETSUPPORTED),
                              "a rotate action taking another point");
    g.action->conn_pts[2].name = name;
  }
  g.action->dependent = 0;
  failures += test_refusal (dwg, WINDOW, v, 1,
                            REFUSED (DWG_ERR_NOTYETSUPPORTED),
                            "a rotate action with an independent base point");
  g.action->dependent = 1;
  g.action->offset.x = 1.0;
  failures += test_refusal (dwg, WINDOW, v, 1,
                            REFUSED (DWG_ERR_NOTYETSUPPORTED),
                            "a rotate action with its base point offset");
  g.action->offset.x = 0.0;
  // 'Stretch Divider' taking the rotation's value
  {
    BITCODE_BL codes[2];
    for (int i = 0; i < 2; i++)
      {
        codes[i] = divider->conn_pts[i].code;
        divider->conn_pts[i].code = GRAFT_PARAM;
      }
    failures += test_refusal (dwg, WINDOW, v, 1,
                              REFUSED (DWG_ERR_NOTYETSUPPORTED),
                              "a rotation property driving a stretch");
    for (int i = 0; i < 2; i++)
      divider->conn_pts[i].code = codes[i];
  }
  // BBD5 listed twice
  g.action->num_deps = 2;
  failures += test_refusal (dwg, WINDOW, v, 1,
                            REFUSED (DWG_ERR_NOTYETSUPPORTED),
                            "an entity the rotate action lists twice");
  g.action->num_deps = 1;
  ungraft_rotation (dwg, &g);
  return failures;
}

/* Appends node id nodeid to the list *actions of *num_actions node ids of
   an action. */
static int
add_listed (BITCODE_BL **actions, BITCODE_BL *num_actions,
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

/* The refusals a rotate action shares with stretch and move actions, made
   by the Toilet's 'Rotate Plan' at Angle 1.2: an object in its selection
   that is not an entity of the definition (the Window's LINE BBD5), a
   node id in its list that the graph doesn't have, a parameter that
   chains its actions in its list (the Toilet's alignment parameter) and
   one only in its selection (its flip parameter). */
static int
test_rotate_refusals (Dwg_Data *dwg)
{
  Dwg_Object_BLOCKROTATEACTION *act
      = rotate_action (dwg, TOILET, "Rotate Plan");
  Dwg_Object *alignment = graph_node (
      dwg, TOILET, DWG_TYPE_BLOCKALIGNMENTPARAMETER, "Alignment");
  Dwg_Object *flip = graph_node (dwg, TOILET, DWG_TYPE_BLOCKFLIPPARAMETER,
                                 "Flip");
  Dwg_DynValue v;
  int failures = 0;
  memset (&v, 0, sizeof (v));
  if (!act || !alignment || !flip)
    {
      fail ("rotate refusals: fixture objects not found");
      return 1;
    }
  v.name = (char *)"Angle";
  v.value = 1.2;
  if (add_dep (dwg, &act->deps, &act->num_deps, 0xBBD5))
    {
      failures += test_refusal (dwg, TOILET, &v, 1,
                                REFUSED (DWG_ERR_INVALIDHANDLE),
                                "a rotate action selecting an entity of "
                                "another block");
      act->num_deps--;
    }
  else
    failures++;
  if (add_listed (&act->actions, &act->num_actions, 99999))
    {
      failures += test_refusal (dwg, TOILET, &v, 1,
                                REFUSED (DWG_ERR_INVALIDHANDLE),
                                "a rotate action listing a node the graph "
                                "doesn't have");
      act->num_actions--;
    }
  else
    failures++;
  // found again, as the evaluations added objects
  alignment = graph_node (dwg, TOILET, DWG_TYPE_BLOCKALIGNMENTPARAMETER,
                          "Alignment");
  alignment->tio.object->tio.BLOCKALIGNMENTPARAMETER->chain_actions = 1;
  failures += test_refusal (dwg, TOILET, &v, 1,
                            REFUSED (DWG_ERR_NOTYETSUPPORTED),
                            "a rotate action turning a parameter chaining "
                            "its actions");
  alignment->tio.object->tio.BLOCKALIGNMENTPARAMETER->chain_actions = 0;
  act = rotate_action (dwg, TOILET, "Rotate Plan");
  flip = graph_node (dwg, TOILET, DWG_TYPE_BLOCKFLIPPARAMETER, "Flip");
  if (add_dep (dwg, &act->deps, &act->num_deps, flip->handle.value))
    {
      if (evaluate (dwg, TOILET, &v, 1, "rotate refusals, flip selected"))
        OK ("rotate refusals: a rotate action may select another parameter "
            "that doesn't chain its actions");
      else
        failures++;
      flip = graph_node (dwg, TOILET, DWG_TYPE_BLOCKFLIPPARAMETER, "Flip");
      flip->tio.object->tio.BLOCKFLIPPARAMETER->chain_actions = 1;
      failures += test_refusal (dwg, TOILET, &v, 1,
                                REFUSED (DWG_ERR_NOTYETSUPPORTED),
                                "a rotate action selecting, not listing, a "
                                "parameter chaining its actions");
      flip->tio.object->tio.BLOCKFLIPPARAMETER->chain_actions = 0;
      act = rotate_action (dwg, TOILET, "Rotate Plan");
      act->num_deps--;
    }
  else
    failures++;
  return failures;
}

/* A LINEAR or ROTATION value for a parameter with a point that isn't
   finite, as in a corrupt drawing, is refused: its length or angle isn't
   finite either, and an angle would compare as no change. Here Window
   Width's end point and the Toilet's base point. */
static int
test_non_finite_points (Dwg_Data *dwg)
{
  Dwg_Object_BLOCKLINEARPARAMETER *width
      = linear_param (dwg, WINDOW, "Window Width");
  Dwg_Object *obj = dwg_resolve_handle (dwg, TOILET_ANGLE);
  Dwg_Object_BLOCKROTATIONPARAMETER *angle
      = obj && obj->fixedtype == DWG_TYPE_BLOCKROTATIONPARAMETER
            ? obj->tio.object->tio.BLOCKROTATIONPARAMETER
            : NULL;
  Dwg_DynValue v;
  double kept;
  int failures = 0;
  memset (&v, 0, sizeof (v));
  if (!width || !angle)
    {
      fail ("non-finite points: fixture objects not found");
      return 1;
    }
  kept = width->def_endpt.x;
  width->def_endpt.x = (double)NAN;
  v.name = (char *)"Window Width";
  v.value = 24.0;
  failures += test_refusal (dwg, WINDOW, &v, 1,
                            REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "a linear parameter with an end point that isn't "
                            "finite");
  width->def_endpt.x = kept;
  kept = angle->def_basept.x;
  angle->def_basept.x = (double)NAN;
  v.name = (char *)"Angle";
  v.value = 1.2;
  failures += test_refusal (dwg, TOILET, &v, 1,
                            REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                            "a rotation parameter with a base point that "
                            "isn't finite");
  angle->def_basept.x = kept;
  return failures;
}

/* Changes the first letter of the true name of definition def to letter,
   returning the letter it had, or 0 when it has no true name. */
static char
rename_true_name (Dwg_Data *dwg, const BITCODE_RLL def, const char letter)
{
  Dwg_Object *obj = dwg_resolve_handle (dwg, def);
  Dwg_Object_Object *o = obj ? obj->tio.object : NULL;
  for (BITCODE_BL i = 0; o && o->eed && i < o->num_eed; i++)
    {
      const Dwg_Object *appid;
      Dwg_Eed_Data *data = o->eed[i].data;
      // the first item of a group has its size and APPID
      if (!o->eed[i].size || !data)
        continue;
      appid = dwg_resolve_handle (dwg, o->eed[i].handle.value);
      if (appid && appid->fixedtype == DWG_TYPE_APPID
          && !strcmp (appid->tio.object->tio.APPID->name,
                      "AcDbDynamicBlockTrueName")
          && data->code == 0 && !data->u.eed_0.is_tu
          && data->u.eed_0.length)
        {
          const char old = data->u.eed_0.string[0];
          data->u.eed_0.string[0] = letter;
          return old;
        }
    }
  return 0;
}

/* A rotation parameter given a value that another rotate action lists or
   selects: in a new document, the Toilet copied twice, under two true
   names, and the second copy's rotation parameter, renamed 'Angle B',
   moved into the first copy's evaluation graph under a node id it doesn't
   use. With 'Angle B' given a value, the first copy's 'Rotate Plan'
   listing it or only selecting it is refused, as the turn would leave it
   at another angle than the value; with no value it is turned. */
static int
test_rotate_other_rotation (Dwg_Data *src)
{
  Dwg_Data *dst = dwg_new_Document (R_2004, 0, tracelevel);
  Dwg_Object_BLOCK_HEADER *first, *second;
  BITCODE_RLL first_h = 0, angle_h = 0;
  Dwg_Object_EVALUATION_GRAPH *graph;
  Dwg_EVAL_Node *nodes;
  Dwg_Object *angle;
  Dwg_Object_BLOCKROTATIONPARAMETER *_angle;
  Dwg_Object_BLOCKROTATEACTION *act;
  BITCODE_BL nodeid, num_nodes;
  Dwg_DynValue v[2];
  int error = 0, failures = 0;
  char letter;

  first = dwg_dynblock_copy_block (dst, src, block_header (src, TOILET),
                                   &error);
  letter = rename_true_name (src, TOILET, 'Q');
  second = first && !error ? dwg_dynblock_copy_block (
                                 dst, src, block_header (src, TOILET), &error)
                           : NULL;
  if (letter)
    rename_true_name (src, TOILET, letter);
  loglevel = test_loglevel;
  if (!first || !second || first == second || error || !letter)
    {
      fail ("rotate other rotation: copying the Toilet twice: 0x%x", error);
      free_dwg (dst);
      free (dst);
      return 1;
    }
  first_h = block_handle (first);
  angle = graph_node (dst, block_handle (second),
                      DWG_TYPE_BLOCKROTATIONPARAMETER, "Rotation");
  graph = definition_graph (dst, first_h);
  if (!angle || !graph)
    {
      fail ("rotate other rotation: the copies' objects not found");
      free_dwg (dst);
      free (dst);
      return 1;
    }
  angle_h = angle->handle.value;
  _angle = angle->tio.object->tio.BLOCKROTATIONPARAMETER;
  nodeid = _angle->evalexpr.nodeid;
  free (_angle->angle_name);
  _angle->angle_name = strdup ("Angle B");
  nodes = _angle->angle_name
              ? (Dwg_EVAL_Node *)realloc (graph->nodes,
                                          (graph->num_nodes + 1)
                                              * sizeof (Dwg_EVAL_Node))
              : NULL;
  if (!nodes)
    {
      fail ("rotate other rotation: out of memory");
      free_dwg (dst);
      free (dst);
      return 1;
    }
  graph->nodes = nodes;
  _angle->evalexpr.nodeid = 905;
  num_nodes = graph->num_nodes;
  memset (&nodes[num_nodes], 0, sizeof (Dwg_EVAL_Node));
  nodes[num_nodes].evalexpr = dwg_add_handleref (dst, 5, angle_h, NULL);
  graph->num_nodes++;

  memset (v, 0, sizeof (v));
  v[0].name = (char *)"Angle";
  v[0].value = 1.2;
  v[1].name = (char *)"Angle B";
  v[1].value = 0.5;
  act = rotate_action (dst, first_h, "Rotate Plan");
  if (act && add_listed (&act->actions, &act->num_actions, 905))
    {
      failures += test_refusal (dst, first_h, v, 2,
                                REFUSED (DWG_ERR_NOTYETSUPPORTED),
                                "a rotate action turning a rotation parameter "
                                "given a value");
      if (evaluate (dst, first_h, v, 1, "rotate other rotation, no value"))
        OK ("rotate other rotation: a rotate action may turn a rotation "
            "parameter given no value");
      else
        failures++;
      act = rotate_action (dst, first_h, "Rotate Plan");
      act->num_actions--;
    }
  else
    failures++;
  if (act && add_dep (dst, &act->deps, &act->num_deps, angle_h))
    {
      failures += test_refusal (dst, first_h, v, 2,
                                REFUSED (DWG_ERR_NOTYETSUPPORTED),
                                "a rotate action selecting, not listing, a "
                                "rotation parameter given a value");
      act = rotate_action (dst, first_h, "Rotate Plan");
      act->num_deps--;
    }
  else
    failures++;
  definition_graph (dst, first_h)->num_nodes = num_nodes;
  angle = dwg_resolve_handle (dst, angle_h);
  angle->tio.object->tio.BLOCKROTATIONPARAMETER->evalexpr.nodeid = nodeid;
  free_dwg (dst);
  free (dst);
  return failures;
}

/* What the check reports for each placement of the fixture: the
   definition, and the number of entities compared. AutoCAD computed the
   placements at the defaults, the visibility, linear and rotation ones,
   which the evaluator matches; the Door's Door Size drives a scale action,
   and Door Elevation is constraint-based, which it refuses. */
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
  { 0xC8DB, 0xCC00, MATCH, KIND (ROTATION) | KIND (VISIBILITY), TOILET, 215 },
  { 0xC8DC, 0xCD02, MATCH, KIND (ROTATION) | KIND (VISIBILITY), TOILET, 215 },
  { 0xC8DD, 0xCE04, MATCH, KIND (ROTATION) | KIND (VISIBILITY), TOILET, 215 },
  { 0xC8DE, 0xCF06, MATCH, KIND (ROTATION), TOILET, 215 },
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

/* The check compares the angles a turn changes around the circle: an ARC
   of AutoCAD's block of T4 (C8DD), at Angle 225 degrees, still matches
   with its start angle a whole turn on, as AutoCAD can keep an angle just
   below 2 pi where the evaluator has it just above 0; a quarter turn on,
   it differs. And an ARC's sweep must match: with the definition's ARC a
   hair short of a full circle and AutoCAD's a hair long of none, each
   angle matches around the circle, but the arcs differ. */
static int
test_check_turned_angles (Dwg_Data *dwg)
{
  Dwg_Object *arc = first_of_type (dwg, 0xCE04, DWG_TYPE_ARC);
  Dwg_DynBlock_Check_Report *report;
  const Dwg_DynBlock_Check *c;
  double start;
  int failures = 0;
  loglevel = test_loglevel;
  if (!arc)
    {
      fail ("check turned angles: fixture objects not found");
      return 1;
    }
  start = arc->tio.entity->tio.ARC->start_angle;
  arc->tio.entity->tio.ARC->start_angle = start + 2.0 * M_PI;
  report = dwg_dynblock_check (dwg, TOLERANCE);
  loglevel = test_loglevel;
  c = report ? find_check (report, 0xC8DD, 0xCE04) : NULL;
  if (c && c->status == DWG_DYNBLOCK_CHECK_MATCH)
    OK ("check turned angles: an ARC's start angle a whole turn on matches");
  else
    {
      fail ("check turned angles: a whole turn on: status %d, %s",
            c ? (int)c->status : -1, c && c->reason ? c->reason : "");
      failures++;
    }
  dwg_dynblock_free_check_report (report);
  // found again, as the check added objects
  arc = first_of_type (dwg, 0xCE04, DWG_TYPE_ARC);
  if (!arc)
    {
      fail ("check turned angles: AutoCAD's ARC not found again");
      return failures + 1;
    }
  arc->tio.entity->tio.ARC->start_angle = start + M_PI / 2.0;
  report = dwg_dynblock_check (dwg, TOLERANCE);
  loglevel = test_loglevel;
  c = report ? find_check (report, 0xC8DD, 0xCE04) : NULL;
  if (c && c->status == DWG_DYNBLOCK_CHECK_MISMATCH && c->num_differing == 1
      && c->reason && strstr (c->reason, "start_angle"))
    OK ("check turned angles: a quarter turn on differs: %s", c->reason);
  else
    {
      fail ("check turned angles: a quarter turn on: status %d, %s",
            c ? (int)c->status : -1, c && c->reason ? c->reason : "");
      failures++;
    }
  dwg_dynblock_free_check_report (report);
  {
    BITCODE_RL idx = 0;
    BITCODE_RLL self = 0;
    Dwg_Object *src;
    arc = first_of_type (dwg, 0xCE04, DWG_TYPE_ARC);
    src = arc && etag (dwg, arc, &idx, &self) ? rep_entity (dwg, TOILET, idx)
                                              : NULL;
    if (!src || src->fixedtype != DWG_TYPE_ARC
        || src->tio.entity->tio.ARC->start_angle < 1e-9)
      {
        fail ("check turned angles: the definition's ARC not found");
        return failures + 1;
      }
    arc->tio.entity->tio.ARC->start_angle = start;
    arc->tio.entity->tio.ARC->end_angle = start + 1e-10;
    src->tio.entity->tio.ARC->end_angle
        = src->tio.entity->tio.ARC->start_angle - 1e-10;
  }
  report = dwg_dynblock_check (dwg, TOLERANCE);
  loglevel = test_loglevel;
  c = report ? find_check (report, 0xC8DD, 0xCE04) : NULL;
  if (c && c->status == DWG_DYNBLOCK_CHECK_MISMATCH && c->num_differing == 1
      && c->reason && strstr (c->reason, "sweep"))
    OK ("check turned angles: arcs a whole turn apart in sweep differ: %s",
        c->reason);
  else
    {
      fail ("check turned angles: sweeps a whole turn apart: status %d, %s",
            c ? (int)c->status : -1, c && c->reason ? c->reason : "");
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

/* A placement whose evaluation's copy left out an EED group the decoder
   read only in part: its first item decoded, a later one not, as copy
   test's test_eed_partly_decoded shows the decoder leaves one. Here the
   Window's LINE BBD7 gets such a group of the ACAD APPID, after its
   AcDbBlockRepETag: the raw bytes of a string and a long, with the
   string's item and none for the long. (An r2004 file keeps its objects
   compressed, so the bytes can't be changed in a written file as that
   test does with an r2000 one.) The copy drops the group with
   DWG_ERR_INVALIDEED, and each Window placement is then a mismatch,
   though the comparison, which doesn't compare EED, finds no
   difference. */
static int
test_check_eed_read_in_part (Dwg_Data *dwg)
{
  static const char text[] = "read in part";
  const size_t len = sizeof (text) - 1;
  Dwg_Object *line = dwg_resolve_handle (dwg, 0xBBD7);
  BITCODE_H acad = dwg_find_tablehandle (dwg, "ACAD", "APPID");
  Dwg_Object_Entity *ent;
  Dwg_Eed *eed;
  Dwg_Eed_Data *data;
  Dwg_DynBlock_Check_Report *report;
  char bits[16];
  BITCODE_BL num_window = 0;
  int failures = 0;
  loglevel = test_loglevel;
  if (!line || line->fixedtype != DWG_TYPE_LINE || !acad)
    {
      fail ("check EED read in part: fixture objects not found");
      return 1;
    }
  ent = line->tio.entity;
  eed = (Dwg_Eed *)realloc (ent->eed, (ent->num_eed + 2) * sizeof (Dwg_Eed));
  if (!eed)
    {
      fail ("check EED read in part: out of memory");
      return 1;
    }
  ent->eed = eed;
  eed = &ent->eed[ent->num_eed];
  memset (eed, 0, 2 * sizeof (Dwg_Eed));
  // the string, code 0: length, codepage (big-endian), the bytes; the
  // long, code 71, little-endian
  eed[0].size = (BITCODE_BS)(4 + len + 5);
  eed[0].handle = acad->handleref;
  eed[0].raw = (BITCODE_TF)calloc (eed[0].size, 1);
  data = (Dwg_Eed_Data *)calloc (1, sizeof (Dwg_Eed_Data) + len + 1);
  if (!eed[0].raw || !data)
    {
      free (eed[0].raw);
      free (data);
      fail ("check EED read in part: out of memory");
      return 1;
    }
  eed[0].raw[1] = (unsigned char)len;
  eed[0].raw[3] = (unsigned char)dwg->header.codepage;
  memcpy (&eed[0].raw[4], text, len);
  eed[0].raw[4 + len] = 71;
  eed[0].raw[4 + len + 1] = 0x2A;
  data->code = 0;
  data->u.eed_0.length = (BITCODE_RS)len;
  data->u.eed_0.codepage = dwg->header.codepage;
  memcpy (data->u.eed_0.string, text, len + 1);
  eed[0].data = data;
  // eed[1]: the long, which the decoder left without data
  ent->num_eed += 2;
  snprintf (bits, sizeof (bits), "(0x%x)", (unsigned)DWG_ERR_INVALIDEED);

  report = dwg_dynblock_check (dwg, TOLERANCE);
  loglevel = test_loglevel;
  if (!report)
    {
      fail ("check EED read in part: no report");
      return 1;
    }
  for (size_t i = 0; i < NUM_CHECKS; i++)
    {
      const Expected_Check *e = &expected_checks[i];
      const Dwg_DynBlock_Check *c = find_check (report, e->insert, e->block);
      if (e->definition != WINDOW)
        continue;
      num_window++;
      if (c && c->status == DWG_DYNBLOCK_CHECK_MISMATCH && c->evaluated
          && c->num_entities == e->num_entities && !c->num_differing
          && c->reason && strstr (c->reason, bits))
        OK ("check EED read in part " FORMAT_HV ": %s", e->insert, c->reason);
      else
        {
          fail ("check EED read in part " FORMAT_HV ": status %d, %u "
                "entities, %u differ: %s; expected a mismatch naming %s",
                e->insert, c ? (int)c->status : -1,
                c ? (unsigned)c->num_entities : 0,
                c ? (unsigned)c->num_differing : 0,
                c && c->reason ? c->reason : "", bits);
          failures++;
        }
    }
  if (num_window != 5 || report->num_mismatch != num_window
      || report->num_match != expected (MATCH) - num_window)
    {
      fail ("check EED read in part: %u Window placements, %u mismatch, %u "
            "match",
            (unsigned)num_window, (unsigned)report->num_mismatch,
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

/* Turns LINE obj into an ATTDEF tagged "TAG", of default value "value",
   with the given justification, rotation and insertion and alignment
   points. Returns 0 when obj is not a LINE or memory runs out. */
static int
line_to_attdef (Dwg_Object *obj, const BITCODE_BS horiz,
                const BITCODE_BS vert, const double rotation,
                const double x, const double y, const double ax,
                const double ay)
{
  Dwg_Entity_ATTDEF *_attdef;
  if (!obj || obj->fixedtype != DWG_TYPE_LINE)
    return 0;
  _attdef = (Dwg_Entity_ATTDEF *)calloc (1, sizeof (Dwg_Entity_ATTDEF));
  if (!_attdef)
    return 0;
  _attdef->tag = strdup ("TAG");
  _attdef->default_value = strdup ("value");
  _attdef->prompt = strdup ("Prompt");
  if (!_attdef->tag || !_attdef->default_value || !_attdef->prompt)
    {
      free (_attdef->tag);
      free (_attdef->default_value);
      free (_attdef->prompt);
      free (_attdef);
      return 0;
    }
  _attdef->extrusion.z = 1.0;
  _attdef->height = 1.0;
  _attdef->width_factor = 1.0;
  _attdef->horiz_alignment = horiz;
  _attdef->vert_alignment = vert;
  _attdef->rotation = rotation;
  _attdef->ins_pt.x = x;
  _attdef->ins_pt.y = y;
  _attdef->alignment_pt.x = ax;
  _attdef->alignment_pt.y = ay;
  _attdef->parent = obj->tio.entity;
  // a LINE holds no pointer but its parent
  free (obj->tio.entity->tio.LINE);
  obj->tio.entity->tio.ATTDEF = _attdef;
  obj->type = obj->fixedtype = DWG_TYPE_ATTDEF;
  obj->name = (char *)"ATTDEF";
  obj->dxfname = (char *)"ATTDEF";
  return 1;
}

/* Turns LINE obj into an MTEXT with the given extents. Returns 0 when obj
   is not a LINE or memory runs out. */
static int
line_to_mtext (Dwg_Object *obj, const double width, const double height)
{
  Dwg_Entity_MTEXT *_mtext;
  if (!obj || obj->fixedtype != DWG_TYPE_LINE)
    return 0;
  _mtext = (Dwg_Entity_MTEXT *)calloc (1, sizeof (Dwg_Entity_MTEXT));
  if (!_mtext)
    return 0;
  _mtext->text = strdup ("mtext");
  if (!_mtext->text)
    {
      free (_mtext);
      return 0;
    }
  _mtext->extrusion.z = 1.0;
  _mtext->x_axis_dir.x = 1.0;
  _mtext->text_height = 1.0;
  _mtext->rect_width = 10.0;
  _mtext->attachment = 1;
  _mtext->flow_dir = 1;
  _mtext->linespace_factor = 1.0;
  _mtext->extents_width = width;
  _mtext->extents_height = height;
  _mtext->parent = obj->tio.entity;
  free (obj->tio.entity->tio.LINE);
  obj->tio.entity->tio.MTEXT = _mtext;
  obj->type = obj->fixedtype = DWG_TYPE_MTEXT;
  obj->name = (char *)"MTEXT";
  obj->dxfname = (char *)"MTEXT";
  return 1;
}

/* The text in the Trees definition and its placements' blocks, made from
   LINEs of the same index in each, as the definition and AutoCAD's blocks
   hold it, for test_check_measured: a text entity in all five blocks, then
   how AutoCAD's block of each placement differs. */
enum
{
  M_CENTRED,     // ATTDEF centred on the middle
  M_CENTRED_90,  // the same, turned a quarter turn
  M_CENTRED_30,  // ATTDEF centred on the baseline, turned 30 degrees
  M_RIGHT,       // ATTDEF right on the top
  M_MIDDLE,      // ATTDEF middle
  M_LEFT,        // ATTDEF left on the baseline
  M_BOTTOM_LEFT, // ATTDEF left on the bottom
  M_MIDDLE_LEFT, // ATTDEF left on the middle
  M_TOP_LEFT,    // ATTDEF left on the top
  M_CONSTANT,    // constant ATTDEF, centred on the middle
  M_ALIGNED,     // ATTDEF aligned
  M_FIT,         // ATTDEF fit
  M_UNKNOWN_H,   // ATTDEF of horizontal alignment 6, unknown
  M_UNKNOWN_V,   // ATTDEF centred, of vertical alignment 4, unknown
  M_TEXT,        // TEXT centred on the middle
  M_UNMEASURED,  // MTEXT, the definition's extents unmeasured
  M_MEASURED,    // MTEXT, the definition's extents measured
  M_NAN,         // MTEXT, the definition's extents not a number
  NUM_MEASURED
};

/* Makes text entity k of test_check_measured from the LINE obj, as the
   definition has it. Returns 0 on failure. */
static int
measured_entity (Dwg_Object *obj, const int k)
{
  switch (k)
    {
    case M_CENTRED:
      return line_to_attdef (obj, 1, 2, 0.0, 1.0, 2.0, 2.0, 2.5);
    case M_CENTRED_90:
      return line_to_attdef (obj, 1, 2, M_PI / 2.0, 5.5, 1.0, 5.0, 2.0);
    case M_CENTRED_30:
      return line_to_attdef (obj, 1, 0, M_PI / 6.0, 11.134, 1.5, 12.0, 2.0);
    case M_RIGHT:
      return line_to_attdef (obj, 2, 3, 0.0, 1.0, 14.0, 3.0, 15.0);
    case M_MIDDLE:
      return line_to_attdef (obj, 4, 0, 0.0, 1.0, 16.0, 2.0, 16.5);
    case M_LEFT:
      return line_to_attdef (obj, 0, 0, 0.0, 1.0, 4.0, 0.0, 0.0);
    case M_BOTTOM_LEFT:
      return line_to_attdef (obj, 0, 1, 0.0, 1.0, 18.0, 1.0, 17.8);
    case M_MIDDLE_LEFT:
      return line_to_attdef (obj, 0, 2, 0.0, 1.0, 20.0, 1.0, 20.5);
    case M_TOP_LEFT:
      return line_to_attdef (obj, 0, 3, 0.0, 1.0, 22.0, 1.0, 23.0);
    case M_CONSTANT:
      if (!line_to_attdef (obj, 1, 2, 0.0, 1.0, 6.0, 2.0, 6.5))
        return 0;
      obj->tio.entity->tio.ATTDEF->flags = 2;
      return 1;
    case M_ALIGNED:
      return line_to_attdef (obj, 3, 0, 0.0, 1.0, 8.0, 3.0, 8.0);
    case M_FIT:
      return line_to_attdef (obj, 5, 0, 0.0, 1.0, 12.0, 3.0, 12.0);
    case M_UNKNOWN_H:
      return line_to_attdef (obj, 6, 0, 0.0, 1.0, 24.0, 2.0, 24.0);
    case M_UNKNOWN_V:
      return line_to_attdef (obj, 1, 4, 0.0, 1.0, 26.0, 2.0, 26.5);
    case M_TEXT:
      if (!line_to_text (obj, "text"))
        return 0;
      obj->tio.entity->tio.TEXT->horiz_alignment = 1;
      obj->tio.entity->tio.TEXT->vert_alignment = 2;
      obj->tio.entity->tio.TEXT->ins_pt.x = 1.0;
      obj->tio.entity->tio.TEXT->ins_pt.y = 10.0;
      obj->tio.entity->tio.TEXT->alignment_pt.x = 2.0;
      obj->tio.entity->tio.TEXT->alignment_pt.y = 10.5;
      return 1;
    case M_UNMEASURED:
      return line_to_mtext (obj, -1.0, -1.0);
    case M_MEASURED:
      return line_to_mtext (obj, 4.0, 1.0);
    case M_NAN:
      return line_to_mtext (obj, (double)NAN, (double)NAN);
    default:
      return 0;
    }
}

/* What AutoCAD measures from laid-out text matches as equivalent: an
   attribute definition's insertion point moved along the baseline, where
   it is justified centre, right or middle, so that its alignment point
   places it, and an MTEXT's extents where one side has none measured.
   Text of the Trees definition and of its 4 placements' blocks, made from
   LINEs: in AutoCAD's blocks, R1's 3 centred attribute definitions (at 0,
   90 and 30 degrees), its right and its middle one moved along the
   baseline, and the unmeasured MTEXT measured and the measured one
   unmeasured, match; R2's centred one moved across the baseline differs,
   and its measured MTEXT with extents 0, as AutoCAD writes them
   unmeasured too, matches; R3's moves differ for an alignment point, 4
   left attribute definitions (on the baseline, bottom, middle and top), a
   constant, an aligned, a fit, one of horizontal alignment 6 and one of
   vertical alignment 4, and a TEXT, and so do its MTEXT extents infinite
   against the definition's -1 and -1 against the definition's NaN; and
   C145's MTEXT measured differently differs. */
static int
test_check_measured (Dwg_Data *dwg)
{
  static const BITCODE_RLL blocks[] = { TREES, 0xCFF7, 0xD24A, 0xD49D,
                                        0xB6AC };
  const Dwg_Object_BLOCK_HEADER *r1 = block_header (dwg, 0xCFF7);
  BITCODE_RL idx[NUM_MEASURED];
  Dwg_DynBlock_Check_Report *report;
  const Dwg_DynBlock_Check *c;
  int n = 0, failures = 0;
  loglevel = test_loglevel;
  // LINEs of the same index in the definition and all 4 blocks
  for (BITCODE_BL i = 0; r1 && i < r1->num_owned && n < NUM_MEASURED; i++)
    {
      const Dwg_Object *obj = dwg_ref_object (dwg, r1->entities[i]);
      BITCODE_RLL self = 0;
      int all = 1;
      if (!obj || obj->fixedtype != DWG_TYPE_LINE
          || !etag (dwg, obj, &idx[n], &self))
        continue;
      for (size_t b = 0; b < sizeof (blocks) / sizeof (blocks[0]); b++)
        {
          const Dwg_Object *line = rep_entity (dwg, blocks[b], idx[n]);
          all = all && line && line->fixedtype == DWG_TYPE_LINE;
        }
      if (all)
        n++;
    }
  if (n < NUM_MEASURED)
    {
      fail ("check measured: %d of %d LINEs found in Trees", n,
            NUM_MEASURED);
      return 1;
    }
  for (size_t b = 0; b < sizeof (blocks) / sizeof (blocks[0]); b++)
    for (int k = 0; k < NUM_MEASURED; k++)
      if (!measured_entity (rep_entity (dwg, blocks[b], idx[k]), k))
        {
          fail ("check measured: no text %d in " FORMAT_HV, k, blocks[b]);
          return 1;
        }
  // R1's block
  rep_entity (dwg, 0xCFF7, idx[M_CENTRED])->tio.entity->tio.ATTDEF->ins_pt.x
      += 0.4;
  rep_entity (dwg, 0xCFF7, idx[M_CENTRED_90])
      ->tio.entity->tio.ATTDEF->ins_pt.y
      += 0.4;
  {
    Dwg_Entity_ATTDEF *a
        = rep_entity (dwg, 0xCFF7, idx[M_CENTRED_30])->tio.entity->tio.ATTDEF;
    a->ins_pt.x += 0.4 * cos (M_PI / 6.0);
    a->ins_pt.y += 0.4 * sin (M_PI / 6.0);
    rep_entity (dwg, 0xCFF7, idx[M_RIGHT])->tio.entity->tio.ATTDEF->ins_pt.x
        -= 0.4;
    rep_entity (dwg, 0xCFF7, idx[M_MIDDLE])->tio.entity->tio.ATTDEF->ins_pt.x
        += 0.4;
  }
  {
    Dwg_Entity_MTEXT *m
        = rep_entity (dwg, 0xCFF7, idx[M_UNMEASURED])->tio.entity->tio.MTEXT;
    m->extents_width = 5.0;
    m->extents_height = 1.2;
    m = rep_entity (dwg, 0xCFF7, idx[M_MEASURED])->tio.entity->tio.MTEXT;
    m->extents_width = -1.0;
    m->extents_height = -1.0;
  }
  // R2's block
  rep_entity (dwg, 0xD24A, idx[M_CENTRED])->tio.entity->tio.ATTDEF->ins_pt.y
      += 0.4;
  {
    Dwg_Entity_MTEXT *m
        = rep_entity (dwg, 0xD24A, idx[M_MEASURED])->tio.entity->tio.MTEXT;
    m->extents_width = 0.0;
    m->extents_height = 0.0;
  }
  // R3's block
  {
    Dwg_Entity_ATTDEF *a
        = rep_entity (dwg, 0xD49D, idx[M_CENTRED])->tio.entity->tio.ATTDEF;
    a->ins_pt.x += 0.4;
    a->alignment_pt.x += 0.4;
    rep_entity (dwg, 0xD49D, idx[M_LEFT])->tio.entity->tio.ATTDEF->ins_pt.x
        += 0.4;
    for (int k = M_BOTTOM_LEFT; k <= M_TOP_LEFT; k++)
      rep_entity (dwg, 0xD49D, idx[k])->tio.entity->tio.ATTDEF->ins_pt.x
          += 0.4;
    rep_entity (dwg, 0xD49D, idx[M_UNKNOWN_H])
        ->tio.entity->tio.ATTDEF->ins_pt.x
        += 0.4;
    rep_entity (dwg, 0xD49D, idx[M_UNKNOWN_V])
        ->tio.entity->tio.ATTDEF->ins_pt.x
        += 0.4;
    rep_entity (dwg, 0xD49D, idx[M_CONSTANT])
        ->tio.entity->tio.ATTDEF->ins_pt.x
        += 0.4;
    rep_entity (dwg, 0xD49D, idx[M_ALIGNED])->tio.entity->tio.ATTDEF->ins_pt.x
        += 0.4;
    rep_entity (dwg, 0xD49D, idx[M_FIT])->tio.entity->tio.ATTDEF->ins_pt.x
        += 0.4;
    rep_entity (dwg, 0xD49D, idx[M_TEXT])->tio.entity->tio.TEXT->ins_pt.x
        += 0.4;
    // extents that aren't finite on one side, unmeasured on the other
    rep_entity (dwg, 0xD49D, idx[M_UNMEASURED])
        ->tio.entity->tio.MTEXT->extents_width
        = (double)INFINITY;
    rep_entity (dwg, 0xD49D, idx[M_NAN])->tio.entity->tio.MTEXT->extents_width
        = -1.0;
  }
  // C145's block
  rep_entity (dwg, 0xB6AC, idx[M_MEASURED])
      ->tio.entity->tio.MTEXT->extents_width
      = 4.5;

  report = dwg_dynblock_check (dwg, TOLERANCE);
  loglevel = test_loglevel;
  if (!report)
    {
      fail ("check measured: no report");
      return 1;
    }
  c = find_check (report, 0xC8E0, 0xCFF7);
  if (c && c->status == MATCH && !c->num_differing && c->num_equivalent == 7)
    OK ("check measured: R1 matches 5 attribute definitions moved along "
        "the baseline, 3 centred, at 0, 90 and 30 degrees, a right and a "
        "middle one, and 2 MTEXTs measured on one side");
  else
    {
      fail ("check measured R1: status %d, %u differ, %u equivalent, %s",
            c ? (int)c->status : -1, c ? (unsigned)c->num_differing : 0,
            c ? (unsigned)c->num_equivalent : 0,
            c && c->reason ? c->reason : "");
      failures++;
    }
  c = find_check (report, 0xC8E1, 0xD24A);
  if (c && c->status == DWG_DYNBLOCK_CHECK_MISMATCH && c->num_differing == 1
      && c->num_equivalent == 1 && c->reason && strstr (c->reason, "ATTDEF")
      && strstr (c->reason, "ins_pt[1]"))
    OK ("check measured: R2 finds an attribute definition moved across "
        "the baseline: %s",
        c->reason);
  else
    {
      fail ("check measured R2: status %d, %u differ, %u equivalent, %s",
            c ? (int)c->status : -1, c ? (unsigned)c->num_differing : 0,
            c ? (unsigned)c->num_equivalent : 0,
            c && c->reason ? c->reason : "");
      failures++;
    }
  c = find_check (report, 0xC8E2, 0xD49D);
  if (c && c->status == DWG_DYNBLOCK_CHECK_MISMATCH && c->num_differing == 13
      && !c->num_equivalent)
    OK ("check measured: R3 finds an alignment point, 4 left (baseline, "
        "bottom, middle, top), a constant, an aligned, a fit and 2 unknown "
        "attribute definitions and a TEXT moved, and 2 MTEXTs whose extents "
        "aren't finite on one side: %s",
        c->reason ? c->reason : "");
  else
    {
      fail ("check measured R3: status %d, %u differ, %u equivalent, %s",
            c ? (int)c->status : -1, c ? (unsigned)c->num_differing : 0,
            c ? (unsigned)c->num_equivalent : 0,
            c && c->reason ? c->reason : "");
      failures++;
    }
  c = find_check (report, 0xC145, 0xB6AC);
  if (c && c->status == DWG_DYNBLOCK_CHECK_MISMATCH && c->num_differing == 1
      && !c->num_equivalent && c->reason && strstr (c->reason, "MTEXT")
      && strstr (c->reason, "extents_width"))
    OK ("check measured: C145 finds MTEXT extents measured differently: %s",
        c->reason);
  else
    {
      fail ("check measured C145: status %d, %u differ, %u equivalent, %s",
            c ? (int)c->status : -1, c ? (unsigned)c->num_differing : 0,
            c ? (unsigned)c->num_equivalent : 0,
            c && c->reason ? c->reason : "");
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
   both its linear values, which the check gives the evaluator. The
   placement at the defaults, C17F, matches, as neither of its values
   runs. */
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
      if (!e->kinds)
        {
          if (c && c->status == MATCH && c->num_entities == e->num_entities)
            OK ("check cycle " FORMAT_HV ": at the defaults, a match",
                e->insert);
          else
            {
              fail ("check cycle " FORMAT_HV ": status %d, %s; expected a "
                    "match",
                    e->insert, c ? (int)c->status : -1,
                    c && c->reason ? c->reason : "");
              failures++;
            }
        }
      else if (c && c->status == UNSUPPORTED && c->definition == WINDOW
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
  // C17F at the defaults, and four that change a value
  if (num_window != 5 || report->num_failed
      || report->num_unsupported != expected (UNSUPPORTED) + num_window - 1
      || report->num_match != expected (MATCH) - num_window + 1)
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
  failures += test_selected_deps (dwg);
  return failures;
}

/* The evaluations of rotation properties, on one reading of the fixture:
   the ones that change it come last. */
static int
test_rotation_all (Dwg_Data *dwg)
{
  int failures = test_rotation (dwg);
  failures += test_rotation_graft (dwg);
  failures += test_rotate_refusals (dwg);
  failures += test_non_finite_points (dwg);
  failures += test_turn_whole (dwg);
  failures += test_turn_text_anchor (dwg);
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
  failures += on_fixture (test_rotation_all);
  failures += on_fixture (test_rotate_other_rotation);
  // each check adds its evaluations to the document: a fresh one each
  failures += on_fixture (test_check);
  failures += on_fixture (test_check_finds);
  failures += on_fixture (test_check_turned_angles);
  failures += on_fixture (test_check_copy_warning);
  failures += on_fixture (test_check_eed_read_in_part);
  failures += on_fixture (test_check_proxy);
  failures += on_fixture (test_check_ellipse);
  failures += on_fixture (test_check_mleader);
  failures += on_fixture (test_check_owned);
  failures += on_fixture (test_check_utf8);
  failures += on_fixture (test_check_measured);
  failures += on_fixture (test_check_status);
  failures += on_fixture (test_check_cycle);
  return failures ? 1 : 0;
}
