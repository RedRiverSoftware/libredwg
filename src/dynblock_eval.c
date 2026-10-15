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

/*
 * dynblock_eval.c: dynamic block properties, a placement's stored values,
 * the evaluator, and the check of the evaluator against the representation
 * blocks AutoCAD stored in a drawing. See dwg_dynblock_api.h.
 */

#include "config.h"
#ifdef __STDC_ALLOC_LIB__
#  define __STDC_WANT_LIB_EXT2__ 1 /* for strdup */
#else
#  define _USE_BSD 1
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>

#include "common.h"
#include "bits.h"
#include "dwg.h"
#include "classes.h"
#include "hash.h"
#include "copy.h"
#include "geom.h"
#include "dwg_dynblock_api.h"

#define DWG_LOGLEVEL loglevel
#include "logging.h"
#include "dwg_api.h"

/* What a refused call returns, with a bit for the reason, as copy.c's. */
#define DYNBLOCK_REFUSED DWG_ERR_INVALIDDWG

/* How far a value may be from the definition's and still be at it; also
   the tolerance of the geometry the evaluator checks: a length, an angle,
   or a coordinate against a plane or a frame. */
#define DYNBLOCK_VALUE_EPSILON 1e-9

#define REASON_SIZE 512

/* The start of every evaluation graph node's struct: parameters, actions,
   grips, grip location components and DYNAMICBLOCKPROXYNODE. */
typedef struct _dynblock_element
{
  struct _dwg_object_object *parent;
  Dwg_EvalExpr evalexpr;
} Dynblock_Element;

/* Cuts UTF-8 string s, which vsnprintf cut to its size, back to the end of
   its last whole character. */
static void
utf8_trim (char *s)
{
  size_t len = strlen (s), start = len, need;
  unsigned char lead;
  while (start > 0 && ((unsigned char)s[start - 1] & 0xC0) == 0x80)
    start--;
  if (!start)
    return;
  start--;
  lead = (unsigned char)s[start];
  need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
  if (len - start < need)
    s[start] = '\0';
}

/* Formats the UTF-8 reason. */
static void ATTRIBUTE_FORMAT (3, 4)
    set_reason (char *reason, const size_t size, const char *fmt, ...)
{
  va_list ap;
  int len;
  if (!reason || !size)
    return;
  va_start (ap, fmt);
  len = vsnprintf (reason, size, fmt, ap);
  va_end (ap);
  if (len >= (int)size)
    utf8_trim (reason);
}

static void
free_hash (dwg_inthash *hash)
{
  if (hash)
    hash_free (hash);
}

static Dwg_Object *
handle_object (const Dwg_Data *dwg, const BITCODE_RLL handle)
{
  return handle ? dwg_resolve_handle (dwg, handle) : NULL;
}

static Dwg_Object *
ref_object (const Dwg_Data *dwg, const Dwg_Object_Ref *ref)
{
  return ref ? handle_object (dwg, ref->absolute_ref) : NULL;
}

static BITCODE_RLL
ref_handle (const Dwg_Object_Ref *ref)
{
  return ref ? ref->absolute_ref : 0;
}

/* A UTF-8 copy of a string of dwg, which r2004 keeps in its codepage. */
static char *
utf8_copy (const Dwg_Data *dwg, const char *str)
{
  char *utf8;
  if (!str)
    return strdup ("");
  utf8 = bit_TV_to_utf8 (str, dwg->header.codepage);
  if (!utf8)
    return NULL;
  return utf8 == str ? strdup (str) : utf8;
}

static Dwg_Object_Ref *
object_xdic (const Dwg_Object *obj)
{
  if (obj->supertype == DWG_SUPERTYPE_ENTITY)
    return obj->tio.entity->is_xdic_missing ? NULL
                                            : obj->tio.entity->xdicobjhandle;
  return obj->tio.object->is_xdic_missing ? NULL
                                          : obj->tio.object->xdicobjhandle;
}

/* The entry of DICTIONARY dict named name. */
static Dwg_Object *
dict_item (const Dwg_Data *dwg, const Dwg_Object *dict, const char *name)
{
  const Dwg_Object_DICTIONARY *_dict;
  if (!dict || dict->fixedtype != DWG_TYPE_DICTIONARY)
    return NULL;
  _dict = dict->tio.object->tio.DICTIONARY;
  if (!_dict->texts || !_dict->itemhandles)
    return NULL;
  for (BITCODE_BL i = 0; i < _dict->numitems; i++)
    if (_dict->texts[i] && strEQ (_dict->texts[i], name))
      return ref_object (dwg, _dict->itemhandles[i]);
  return NULL;
}

static Dwg_Object *
xdic_item (const Dwg_Data *dwg, const Dwg_Object *obj, const char *name)
{
  return dict_item (dwg, ref_object (dwg, object_xdic (obj)), name);
}

/* The first EED group of obj with the APPID named appname: its index in
   obj's EED, or -1. The group runs until the next entry with a size. */
static int
eed_group (const Dwg_Data *dwg, const Dwg_Object *obj, const char *appname)
{
  BITCODE_BL num_eed;
  const Dwg_Eed *eed;
  if (obj->supertype == DWG_SUPERTYPE_ENTITY)
    {
      num_eed = obj->tio.entity->num_eed;
      eed = obj->tio.entity->eed;
    }
  else
    {
      num_eed = obj->tio.object->num_eed;
      eed = obj->tio.object->eed;
    }
  for (BITCODE_BL i = 0; eed && i < num_eed; i++)
    {
      const Dwg_Object *appid;
      if (!eed[i].size)
        continue;
      appid = handle_object (dwg, eed[i].handle.value);
      if (appid && appid->fixedtype == DWG_TYPE_APPID
          && appid->tio.object->tio.APPID->name
          && strEQ (appid->tio.object->tio.APPID->name, appname))
        return (int)i;
    }
  return -1;
}

/* The first EED item with the given code in the group starting at
   start. */
static const Dwg_Eed_Data *
eed_group_item (const Dwg_Object *obj, const int start, const BITCODE_RC code)
{
  BITCODE_BL num_eed;
  const Dwg_Eed *eed;
  if (start < 0)
    return NULL;
  if (obj->supertype == DWG_SUPERTYPE_ENTITY)
    {
      num_eed = obj->tio.entity->num_eed;
      eed = obj->tio.entity->eed;
    }
  else
    {
      num_eed = obj->tio.object->num_eed;
      eed = obj->tio.object->eed;
    }
  for (BITCODE_BL i = (BITCODE_BL)start; i < num_eed; i++)
    {
      if (i > (BITCODE_BL)start && eed[i].size)
        break;
      if (eed[i].data && eed[i].data->code == code)
        return eed[i].data;
    }
  return NULL;
}

/* The AcDbBlockRepETag index of entity obj into *indexp. Returns 0 when it
   has none. */
static int
etag_index (const Dwg_Data *dwg, const Dwg_Object *obj, BITCODE_RL *indexp)
{
  const Dwg_Eed_Data *data
      = eed_group_item (obj, eed_group (dwg, obj, "AcDbBlockRepETag"), 71);
  if (!data)
    return 0;
  // Dwg_Eed_Data is packed
  memcpy (indexp, &data->u.eed_71.rl, sizeof (*indexp));
  return 1;
}

/* The definition named by the AcDbBlockRepBTag EED of block obj, or 0. */
static BITCODE_RLL
btag_definition (const Dwg_Data *dwg, const Dwg_Object *obj)
{
  const Dwg_Eed_Data *data
      = eed_group_item (obj, eed_group (dwg, obj, "AcDbBlockRepBTag"), 5);
  BITCODE_RLL handle = 0;
  if (data)
    memcpy (&handle, &data->u.eed_5.entity, sizeof (handle));
  return handle;
}

/* The EVALUATION_GRAPH of definition def, or NULL when def is not a dynamic
   block definition. */
static Dwg_Object *
definition_graph (const Dwg_Data *dwg, const Dwg_Object *def)
{
  Dwg_Object *graph;
  if (!def || def->fixedtype != DWG_TYPE_BLOCK_HEADER)
    return NULL;
  graph = xdic_item (dwg, def, "ACAD_ENHANCEDBLOCK");
  return graph && graph->fixedtype == DWG_TYPE_EVALUATION_GRAPH ? graph
                                                                 : NULL;
}

static int
check_version (const Dwg_Data *dwg, char *reason, const size_t reason_size)
{
  if (dwg->header.from_version != R_2004)
    {
      set_reason (reason, reason_size,
                  "only r2004 drawings are supported, not %s",
                  dwg_version_type (dwg->header.from_version));
      return DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
    }
  return 0;
}

static Dwg_Object *
block_header_object (const Dwg_Object_BLOCK_HEADER *_obj)
{
  const Dwg_Data *dwg;
  if (!_obj || !_obj->parent || !_obj->parent->dwg)
    return NULL;
  dwg = _obj->parent->dwg;
  if (_obj->parent->objid >= dwg->num_objects)
    return NULL;
  return &dwg->object[_obj->parent->objid];
}

/*--------------------------------------------------------------------------------
 * Properties
 */

EXPORT const char *
dwg_dynparam_kind_name (const Dwg_DynParam_Kind kind)
{
  switch (kind)
    {
    case DWG_DYNPARAM_LINEAR:
      return "linear";
    case DWG_DYNPARAM_ROTATION:
      return "rotation";
    case DWG_DYNPARAM_VISIBILITY:
      return "visibility";
    case DWG_DYNPARAM_FLIP:
      return "flip";
    case DWG_DYNPARAM_XY:
      return "xy";
    case DWG_DYNPARAM_POLAR:
      return "polar";
    case DWG_DYNPARAM_POINT:
      return "point";
    case DWG_DYNPARAM_LOOKUP:
      return "lookup";
    case DWG_DYNPARAM_CONSTRAINT:
      return "constraint";
    case DWG_DYNPARAM_USER:
      return "user";
    default:
      return "unknown";
    }
}

/* hypot, as squaring a large component would overflow */
static double
distance_3d (const BITCODE_3BD *a, const BITCODE_3BD *b)
{
  return hypot (hypot (b->x - a->x, b->y - a->y), b->z - a->z);
}

/* angle, in radians, in [0, 2 pi) */
static double
normal_angle (const double angle)
{
  double a = fmod (angle, 2.0 * M_PI);
  if (a < 0.0)
    a += 2.0 * M_PI;
  // a tiny negative angle plus 2 pi rounds to 2 pi
  return a < 2.0 * M_PI ? a : 0.0;
}

/* The angle of end about base, from the direction of base_angle, in
   [0, 2 pi): the value of a rotation parameter. */
static double
rotation_angle (const BITCODE_3BD *base, const BITCODE_3BD *end,
                const BITCODE_3BD *base_angle)
{
  return normal_angle (
      atan2 (end->y - base->y, end->x - base->x)
      - atan2 (base_angle->y - base->y, base_angle->x - base->x));
}

/* The distance between two angles, around the circle. */
static double
angle_difference (const double a, const double b)
{
  double d = fmod (fabs (a - b), 2.0 * M_PI);
  return d > M_PI ? 2.0 * M_PI - d : d;
}

EXPORT void
dwg_dynblock_free_params (Dwg_DynParam *params, const BITCODE_BL count)
{
  if (!params)
    return;
  for (BITCODE_BL i = 0; i < count; i++)
    {
      Dwg_DynParam *p = &params[i];
      free (p->name);
      free (p->default_state);
      for (BITCODE_BL j = 0; p->states && j < p->num_states; j++)
        free (p->states[j]);
      free (p->states);
      free (p->values);
    }
  free (params);
}

typedef struct _param_list
{
  Dwg_DynParam *params;
  BITCODE_BL count;
  BITCODE_BL size;
  int error;
  BITCODE_RLL unresolved; /* a graph node that doesn't resolve */
  const char *unknown_class;
} Param_List;

/* Appends a property of parameter obj, named name in dwg's codepage. */
static Dwg_DynParam *
add_param (Param_List *list, const Dwg_Data *dwg, const Dwg_Object *obj,
           const Dwg_DynParam_Kind kind, const char *name)
{
  Dwg_DynParam *p;
  if (list->count == list->size)
    {
      const BITCODE_BL size = list->size ? list->size * 2 : 8;
      Dwg_DynParam *params = (Dwg_DynParam *)realloc (
          list->params, size * sizeof (Dwg_DynParam));
      if (!params)
        {
          list->error = DWG_ERR_OUTOFMEM;
          return NULL;
        }
      list->params = params;
      list->size = size;
    }
  p = &list->params[list->count];
  memset (p, 0, sizeof (*p));
  p->name = utf8_copy (dwg, name);
  if (!p->name)
    {
      list->error = DWG_ERR_OUTOFMEM;
      return NULL;
    }
  list->count++;
  p->kind = kind;
  p->handle = obj->handle.value;
  p->nodeid = ((const Dynblock_Element *)obj->tio.object->tio
                   .BLOCKVISIBILITYPARAMETER)
                  ->evalexpr.nodeid;
  return p;
}

static void
set_value_set (Param_List *list, Dwg_DynParam *p,
               const Dwg_BLOCKPARAMVALUESET *vs)
{
  p->value_flags = vs->flags;
  p->minimum = vs->minimum;
  p->maximum = vs->maximum;
  p->increment = vs->increment;
  if (vs->num_valuelist && vs->valuelist)
    {
      p->values = (double *)malloc (vs->num_valuelist * sizeof (double));
      if (!p->values)
        {
          list->error = DWG_ERR_OUTOFMEM;
          return;
        }
      memcpy (p->values, vs->valuelist, vs->num_valuelist * sizeof (double));
      p->num_values = vs->num_valuelist;
    }
}

static int
set_states (Param_List *list, const Dwg_Data *dwg, Dwg_DynParam *p,
            const BITCODE_BL num_states, const char *const *names)
{
  if (!num_states)
    return 0;
  p->states = (char **)calloc (num_states, sizeof (char *));
  if (!p->states)
    {
      list->error = DWG_ERR_OUTOFMEM;
      return 1;
    }
  p->num_states = num_states;
  for (BITCODE_BL i = 0; i < num_states; i++)
    {
      p->states[i] = utf8_copy (dwg, names[i]);
      if (!p->states[i])
        {
          list->error = DWG_ERR_OUTOFMEM;
          return 1;
        }
    }
  return 0;
}

/* The DXF name of obj's class, which an object LibreDWG can't decode
   (UNKNOWN_OBJ) has only in the class table. */
static const char *
class_name (const Dwg_Data *dwg, const Dwg_Object *obj)
{
  if (obj->type >= 500 && obj->type - 500 < dwg->num_classes
      && dwg->dwg_class[obj->type - 500].dxfname)
    return dwg->dwg_class[obj->type - 500].dxfname;
  return obj->dxfname ? obj->dxfname : dwg_type_name (obj->fixedtype);
}

/* The properties of the parameters in graph, in node order, into list.
   *parametricp is set when the graph is constraint-based, and
   list->unknown_class names a node of a class LibreDWG can't decode. A
   node that doesn't resolve sets list->error and list->unresolved. */
static void
collect_params (const Dwg_Data *dwg, const Dwg_Object *graph,
                Param_List *list, int *parametricp)
{
  const Dwg_Object_EVALUATION_GRAPH *_graph
      = graph->tio.object->tio.EVALUATION_GRAPH;
  *parametricp = 0;
  for (BITCODE_BL i = 0; _graph->nodes && i < _graph->num_nodes; i++)
    {
      const Dwg_Object *obj = ref_object (dwg, _graph->nodes[i].evalexpr);
      Dwg_DynParam *p = NULL;
      if (!obj && ref_handle (_graph->nodes[i].evalexpr))
        {
          list->error = DYNBLOCK_REFUSED | DWG_ERR_INVALIDHANDLE;
          list->unresolved = ref_handle (_graph->nodes[i].evalexpr);
          return;
        }
      if (!obj)
        continue;
      if (obj->fixedtype == DWG_TYPE_DYNAMICBLOCKPROXYNODE
          || strEQc (class_name (dwg, obj), "ACDB_DYNAMICBLOCKPROXYNODE"))
        {
          *parametricp = 1;
          continue;
        }
      if (obj->fixedtype == DWG_TYPE_UNKNOWN_OBJ && !list->unknown_class)
        list->unknown_class = class_name (dwg, obj);
      switch (obj->fixedtype)
        {
        case DWG_TYPE_BLOCKLINEARPARAMETER:
          {
            const Dwg_Object_BLOCKLINEARPARAMETER *_obj
                = obj->tio.object->tio.BLOCKLINEARPARAMETER;
            p = add_param (list, dwg, obj, DWG_DYNPARAM_LINEAR,
                           _obj->distance_name);
            if (p)
              {
                p->default_value
                    = distance_3d (&_obj->def_basept, &_obj->def_endpt);
                set_value_set (list, p, &_obj->value_set);
              }
          }
          break;
        case DWG_TYPE_BLOCKROTATIONPARAMETER:
          {
            const Dwg_Object_BLOCKROTATIONPARAMETER *_obj
                = obj->tio.object->tio.BLOCKROTATIONPARAMETER;
            p = add_param (list, dwg, obj, DWG_DYNPARAM_ROTATION,
                           _obj->angle_name);
            if (p)
              {
                p->default_value
                    = rotation_angle (&_obj->def_basept, &_obj->def_endpt,
                                      &_obj->def_base_angle_pt);
                set_value_set (list, p, &_obj->angle_value_set);
              }
          }
          break;
        case DWG_TYPE_BLOCKVISIBILITYPARAMETER:
          {
            const Dwg_Object_BLOCKVISIBILITYPARAMETER *_obj
                = obj->tio.object->tio.BLOCKVISIBILITYPARAMETER;
            p = add_param (list, dwg, obj, DWG_DYNPARAM_VISIBILITY,
                           _obj->blockvisi_name);
            if (p && _obj->states && _obj->num_states)
              {
                const char **names = (const char **)calloc (
                    _obj->num_states, sizeof (char *));
                if (!names)
                  {
                    list->error = DWG_ERR_OUTOFMEM;
                    break;
                  }
                for (BITCODE_BL j = 0; j < _obj->num_states; j++)
                  names[j] = _obj->states[j].name;
                if (!set_states (list, dwg, p, _obj->num_states, names))
                  {
                    p->default_state = strdup (p->states[0]);
                    if (!p->default_state)
                      list->error = DWG_ERR_OUTOFMEM;
                  }
                free ((void *)names);
              }
          }
          break;
        case DWG_TYPE_BLOCKFLIPPARAMETER:
          {
            const Dwg_Object_BLOCKFLIPPARAMETER *_obj
                = obj->tio.object->tio.BLOCKFLIPPARAMETER;
            const char *names[2];
            p = add_param (list, dwg, obj, DWG_DYNPARAM_FLIP,
                           _obj->flip_label);
            names[0] = _obj->base_state_label;
            names[1] = _obj->flipped_state_label;
            if (p)
              set_states (list, dwg, p, 2, names);
          }
          break;
        case DWG_TYPE_BLOCKXYPARAMETER:
          {
            const Dwg_Object_BLOCKXYPARAMETER *_obj
                = obj->tio.object->tio.BLOCKXYPARAMETER;
            if (add_param (list, dwg, obj, DWG_DYNPARAM_XY, _obj->x_label))
              add_param (list, dwg, obj, DWG_DYNPARAM_XY, _obj->y_label);
          }
          break;
        case DWG_TYPE_BLOCKPOLARPARAMETER:
          {
            const Dwg_Object_BLOCKPOLARPARAMETER *_obj
                = obj->tio.object->tio.BLOCKPOLARPARAMETER;
            if (add_param (list, dwg, obj, DWG_DYNPARAM_POLAR,
                           _obj->distance_name))
              add_param (list, dwg, obj, DWG_DYNPARAM_POLAR,
                         _obj->angle_name);
          }
          break;
        case DWG_TYPE_BLOCKPOINTPARAMETER:
          add_param (list, dwg, obj, DWG_DYNPARAM_POINT,
                     obj->tio.object->tio.BLOCKPOINTPARAMETER->position_name);
          break;
        case DWG_TYPE_BLOCKLOOKUPPARAMETER:
          add_param (list, dwg, obj, DWG_DYNPARAM_LOOKUP,
                     obj->tio.object->tio.BLOCKLOOKUPPARAMETER->lookup_name);
          break;
        case DWG_TYPE_BLOCKLINEARCONSTRAINTPARAMETER:
        case DWG_TYPE_BLOCKALIGNEDCONSTRAINTPARAMETER:
        case DWG_TYPE_BLOCKHORIZONTALCONSTRAINTPARAMETER:
        case DWG_TYPE_BLOCKVERTICALCONSTRAINTPARAMETER:
          // these four share BLOCKLINEARCONSTRAINTPARAMETER_fields
          *parametricp = 1;
          add_param (list, dwg, obj, DWG_DYNPARAM_CONSTRAINT,
                     obj->tio.object->tio.BLOCKLINEARCONSTRAINTPARAMETER
                         ->expr_name);
          break;
        case DWG_TYPE_BLOCKANGULARCONSTRAINTPARAMETER:
          *parametricp = 1;
          add_param (list, dwg, obj, DWG_DYNPARAM_CONSTRAINT,
                     obj->tio.object->tio.BLOCKANGULARCONSTRAINTPARAMETER
                         ->expr_name);
          break;
        case DWG_TYPE_BLOCKDIAMETRICCONSTRAINTPARAMETER:
          *parametricp = 1;
          add_param (list, dwg, obj, DWG_DYNPARAM_CONSTRAINT,
                     obj->tio.object->tio.BLOCKDIAMETRICCONSTRAINTPARAMETER
                         ->expr_name);
          break;
        case DWG_TYPE_BLOCKRADIALCONSTRAINTPARAMETER:
          *parametricp = 1;
          add_param (list, dwg, obj, DWG_DYNPARAM_CONSTRAINT,
                     obj->tio.object->tio.BLOCKRADIALCONSTRAINTPARAMETER
                         ->expr_name);
          break;
        case DWG_TYPE_BLOCKUSERPARAMETER:
          *parametricp = 1;
          add_param (list, dwg, obj, DWG_DYNPARAM_USER,
                     obj->tio.object->tio.BLOCKUSERPARAMETER->name);
          break;
        default:
          break;
        }
      if (list->error)
        return;
    }
}

/* The properties of the definition at def_handle. */
static int
definition_params (const Dwg_Data *dwg, const BITCODE_RLL def_handle,
                   Param_List *list, int *parametricp, char *reason,
                   const size_t reason_size)
{
  const Dwg_Object *def = handle_object (dwg, def_handle);
  const Dwg_Object *graph = definition_graph (dwg, def);
  int error = check_version (dwg, reason, reason_size);
  memset (list, 0, sizeof (*list));
  *parametricp = 0;
  if (error)
    return error;
  if (!graph)
    {
      set_reason (reason, reason_size,
                  "block " FORMAT_HV " is not a dynamic block definition",
                  def_handle);
      return DYNBLOCK_REFUSED | DWG_ERR_INVALIDTYPE;
    }
  collect_params (dwg, graph, list, parametricp);
  // a parametric block's constraints are in its ACAD_ASSOCNETWORK
  if (xdic_item (dwg, def, "ACAD_ASSOCNETWORK"))
    *parametricp = 1;
  if (list->error)
    {
      const int list_error = list->error;
      if (list->unresolved)
        set_reason (reason, reason_size,
                    "the evaluation graph's node " FORMAT_HV
                    " doesn't resolve",
                    list->unresolved);
      else
        set_reason (reason, reason_size, "out of memory");
      dwg_dynblock_free_params (list->params, list->count);
      memset (list, 0, sizeof (*list));
      return list_error;
    }
  return 0;
}

EXPORT int
dwg_dynblock_get_params (const Dwg_Object_BLOCK_HEADER *def,
                         Dwg_DynParam **params, BITCODE_BL *count)
{
  const Dwg_Object *obj = block_header_object (def);
  Param_List list;
  int parametric;
  char reason[REASON_SIZE] = "";
  int error;
  if (params)
    *params = NULL;
  if (count)
    *count = 0;
  if (!obj || !params || !count)
    return DWG_ERR_INTERNALERROR;
  loglevel = obj->parent->opts & DWG_OPTS_LOGLEVEL;
  error = definition_params (obj->parent, obj->handle.value, &list,
                             &parametric, reason, sizeof (reason));
  if (error)
    {
      LOG_ERROR ("dwg_dynblock_get_params: %s", reason);
      return error;
    }
  *params = list.params;
  *count = list.count;
  return 0;
}

/*--------------------------------------------------------------------------------
 * Stored values
 */

EXPORT void
dwg_dynblock_free_values (Dwg_DynValue *values, const BITCODE_BL count)
{
  if (!values)
    return;
  for (BITCODE_BL i = 0; i < count; i++)
    {
      free (values[i].name);
      free (values[i].state);
    }
  free (values);
}

/* The items of an ACAD_ENHANCEDBLOCKDATA XRECORD after the two node ids
   (1071) and the element version (70, 70) AutoCAD writes first. */
static const Dwg_Resbuf *
xrecord_values (const Dwg_Object *xrecord)
{
  static const short header[] = { 1071, 1071, 70, 70 };
  const Dwg_Resbuf *rb;
  if (!xrecord || xrecord->fixedtype != DWG_TYPE_XRECORD)
    return NULL;
  rb = xrecord->tio.object->tio.XRECORD->xdata;
  for (unsigned i = 0; i < ARRAY_SIZE (header); i++)
    {
      if (!rb || rb->type != header[i])
        return NULL;
      rb = rb->nextrb;
    }
  return rb;
}

/* The next n items of rb, which must have the given codes. */
static int
take_items (const Dwg_Resbuf *rb, const short *codes, const unsigned n,
            const Dwg_Resbuf **items)
{
  for (unsigned i = 0; i < n; i++)
    {
      if (!rb || rb->type != codes[i])
        return 0;
      items[i] = rb;
      rb = rb->nextrb;
    }
  return 1;
}

static void
resbuf_point (const Dwg_Resbuf *rb, BITCODE_3BD *pt)
{
  pt->x = rb->value.pt[0];
  pt->y = rb->value.pt[1];
  pt->z = rb->value.pt[2];
}

/* Reads the value of property p from the placement's
   ACAD_ENHANCEDBLOCKDATA dictionary data into v. */
static int
read_value (const Dwg_Data *dwg, const Dwg_Object *data,
            const Dwg_DynParam *p, Dwg_DynValue *v, char *reason,
            const size_t reason_size)
{
  char key[16];
  const Dwg_Object *xrecord;
  const Dwg_Resbuf *rb, *items[4];
  snprintf (key, sizeof (key), "%u", (unsigned)p->nodeid);
  xrecord = dict_item (dwg, data, key);
  rb = xrecord_values (xrecord);
  v->name = strdup (p->name);
  if (!v->name)
    return DWG_ERR_OUTOFMEM;
  if (rb)
    switch (p->kind)
      {
      case DWG_DYNPARAM_LINEAR:
        {
          static const short codes[] = { 10, 10 };
          if (take_items (rb, codes, 2, items))
            {
              BITCODE_3BD base, end;
              resbuf_point (items[0], &base);
              resbuf_point (items[1], &end);
              v->value = distance_3d (&base, &end);
              return 0;
            }
        }
        break;
      case DWG_DYNPARAM_ROTATION:
        {
          static const short codes[] = { 10, 10, 10, 10 };
          if (take_items (rb, codes, 4, items))
            {
              BITCODE_3BD base, end, base_angle;
              resbuf_point (items[0], &base);
              resbuf_point (items[1], &end);
              resbuf_point (items[3], &base_angle);
              v->value = rotation_angle (&base, &end, &base_angle);
              return 0;
            }
        }
        break;
      case DWG_DYNPARAM_VISIBILITY:
        {
          static const short codes[] = { 10, 1 };
          if (take_items (rb, codes, 2, items))
            {
              v->state = utf8_copy (dwg, items[1]->value.str.u.data);
              return v->state ? 0 : DWG_ERR_OUTOFMEM;
            }
        }
        break;
      case DWG_DYNPARAM_FLIP:
        {
          static const short codes[] = { 10, 10, 10, 70 };
          if (take_items (rb, codes, 4, items))
            {
              v->value = items[3]->value.i16;
              return 0;
            }
        }
        break;
      default:
        break;
      }
  set_reason (reason, reason_size,
              "%s property '%s': the stored value (XRECORD %s) is %s",
              dwg_dynparam_kind_name (p->kind), p->name, key,
              xrecord ? "not in the layout AutoCAD writes" : "missing");
  return DYNBLOCK_REFUSED;
}

static int
is_readable_kind (const Dwg_DynParam_Kind kind)
{
  return kind == DWG_DYNPARAM_LINEAR || kind == DWG_DYNPARAM_ROTATION
         || kind == DWG_DYNPARAM_VISIBILITY || kind == DWG_DYNPARAM_FLIP;
}

/* The block an INSERT or MINSERT draws. */
static Dwg_Object_Ref *
placement_block (const Dwg_Object *insert)
{
  return insert->fixedtype == DWG_TYPE_MINSERT
             ? insert->tio.entity->tio.MINSERT->block_header
             : insert->tio.entity->tio.INSERT->block_header;
}

/* The AcDbBlockRepresentation dictionary of INSERT insert, or NULL. */
static Dwg_Object *
block_representation (const Dwg_Data *dwg, const Dwg_Object *insert)
{
  Dwg_Object *rep = xdic_item (dwg, insert, "AcDbBlockRepresentation");
  return rep && rep->fixedtype == DWG_TYPE_DICTIONARY ? rep : NULL;
}

/* Reads the values of the placement at insert_handle, as
   dwg_dynblock_get_values (). *def_handlep gets the definition the
   placement names as soon as it is found, also when reading the stored
   values or the definition then fails. */
static int
placement_values (const Dwg_Data *dwg, const BITCODE_RLL insert_handle,
                  BITCODE_RLL *def_handlep, Dwg_DynValue **valuesp,
                  BITCODE_BL *countp, char *reason, const size_t reason_size)
{
  const Dwg_Object *insert = handle_object (dwg, insert_handle);
  const Dwg_Object *rep, *repdata, *data = NULL;
  Param_List list;
  Dwg_DynValue *values;
  BITCODE_BL count = 0;
  BITCODE_RLL def_handle;
  int parametric, error, unread = 0;
  *def_handlep = 0;
  *valuesp = NULL;
  *countp = 0;
  if (!insert
      || (insert->fixedtype != DWG_TYPE_INSERT
          && insert->fixedtype != DWG_TYPE_MINSERT))
    {
      set_reason (reason, reason_size, FORMAT_HV " is not an INSERT",
                  insert_handle);
      return DYNBLOCK_REFUSED | DWG_ERR_INVALIDTYPE;
    }
  rep = block_representation (dwg, insert);
  if (rep)
    {
      repdata = dict_item (dwg, rep, "AcDbRepData");
      if (!repdata || repdata->fixedtype != DWG_TYPE_BLOCKREPRESENTATION)
        {
          set_reason (reason, reason_size,
                      "INSERT " FORMAT_HV ": AcDbBlockRepresentation has no "
                      "AcDbRepData",
                      insert_handle);
          return DYNBLOCK_REFUSED;
        }
      def_handle
          = ref_handle (repdata->tio.object->tio.BLOCKREPRESENTATION->block);
      *def_handlep = def_handle;
      data = dict_item (dwg, dict_item (dwg, rep, "AppDataCache"),
                        "ACAD_ENHANCEDBLOCKDATA");
      if (!data || data->fixedtype != DWG_TYPE_DICTIONARY)
        {
          set_reason (reason, reason_size,
                      "INSERT " FORMAT_HV ": AcDbBlockRepresentation has no "
                      "ACAD_ENHANCEDBLOCKDATA",
                      insert_handle);
          return DYNBLOCK_REFUSED;
        }
    }
  else
    {
      // a placement at the defaults draws the definition itself
      def_handle = ref_handle (placement_block (insert));
      *def_handlep = def_handle;
    }
  error = definition_params (dwg, def_handle, &list, &parametric, reason,
                             reason_size);
  if (error)
    return error;
  values = (Dwg_DynValue *)calloc (list.count ? list.count : 1,
                                   sizeof (Dwg_DynValue));
  if (!values)
    {
      dwg_dynblock_free_params (list.params, list.count);
      return DWG_ERR_OUTOFMEM;
    }
  for (BITCODE_BL i = 0; i < list.count; i++)
    {
      const Dwg_DynParam *p = &list.params[i];
      Dwg_DynValue *v = &values[count];
      if (!is_readable_kind (p->kind))
        {
          if (!unread++)
            set_reason (reason, reason_size,
                        "%s property '%s': stored values of this kind are "
                        "not read",
                        dwg_dynparam_kind_name (p->kind), p->name);
          continue;
        }
      if (data)
        error = read_value (dwg, data, p, v, reason, reason_size);
      else
        {
          v->name = strdup (p->name);
          v->value = p->default_value;
          if (p->default_state)
            v->state = strdup (p->default_state);
          error = !v->name || (p->default_state && !v->state)
                      ? DWG_ERR_OUTOFMEM
                      : 0;
        }
      count++;
      if (error)
        break;
    }
  dwg_dynblock_free_params (list.params, list.count);
  if (error)
    {
      dwg_dynblock_free_values (values, count);
      return error;
    }
  *valuesp = values;
  *countp = count;
  return unread ? DWG_ERR_NOTYETSUPPORTED : 0;
}

EXPORT int
dwg_dynblock_get_values (const Dwg_Entity_INSERT *insert,
                         Dwg_Object_BLOCK_HEADER **defp,
                         Dwg_DynValue **values, BITCODE_BL *count)
{
  const Dwg_Data *dwg;
  const Dwg_Object *obj;
  BITCODE_RLL def_handle;
  char reason[REASON_SIZE] = "";
  int error;
  if (defp)
    *defp = NULL;
  if (values)
    *values = NULL;
  if (count)
    *count = 0;
  if (!insert || !insert->parent || !insert->parent->dwg || !defp || !values
      || !count)
    return DWG_ERR_INTERNALERROR;
  dwg = insert->parent->dwg;
  if (insert->parent->objid >= dwg->num_objects)
    return DWG_ERR_INTERNALERROR;
  obj = &dwg->object[insert->parent->objid];
  loglevel = dwg->opts & DWG_OPTS_LOGLEVEL;
  error = placement_values (dwg, obj->handle.value, &def_handle, values,
                            count, reason, sizeof (reason));
  if (error >= DWG_ERR_CRITICAL)
    {
      LOG_ERROR ("dwg_dynblock_get_values: %s", reason);
      return error;
    }
  if (error)
    LOG_WARN ("dwg_dynblock_get_values: %s", reason);
  *defp = handle_object (dwg, def_handle)->tio.object->tio.BLOCK_HEADER;
  return error;
}

/*--------------------------------------------------------------------------------
 * Evaluation
 */

/* The handles of def's entities, in its order, with the owned ATTRIBs,
   VERTEXes and SEQENDs of each after it (of an INSERT, MINSERT, POLYLINE
   or TABLE), into a new array. */
static int
definition_entities (const Dwg_Data *dwg,
                     const Dwg_Object_BLOCK_HEADER *_def,
                     BITCODE_RLL **handlesp, BITCODE_BL *countp,
                     char *reason, const size_t reason_size)
{
  BITCODE_BL size = _def->num_owned + 1, count = 0;
  BITCODE_RLL *handles = (BITCODE_RLL *)malloc (size * sizeof (BITCODE_RLL));
  *handlesp = NULL;
  *countp = 0;
  if (!handles)
    return DWG_ERR_OUTOFMEM;
  for (BITCODE_BL i = 0; _def->entities && i < _def->num_owned; i++)
    {
      const Dwg_Object *ent = ref_object (dwg, _def->entities[i]);
      BITCODE_BL num_owned = 0;
      BITCODE_H *owned = NULL;
      BITCODE_H seqend = NULL;
      if (!ent)
        {
          set_reason (reason, reason_size,
                      "the definition's entity " FORMAT_HV " doesn't resolve",
                      ref_handle (_def->entities[i]));
          free (handles);
          return DYNBLOCK_REFUSED | DWG_ERR_INVALIDHANDLE;
        }
      if (ent->supertype != DWG_SUPERTYPE_ENTITY)
        {
          set_reason (reason, reason_size,
                      "the definition's entity " FORMAT_HV
                      " is not an entity",
                      ent->handle.value);
          free (handles);
          return DYNBLOCK_REFUSED | DWG_ERR_INVALIDHANDLE;
        }
      if (ent->fixedtype == DWG_TYPE_UNKNOWN_ENT)
        {
          set_reason (reason, reason_size,
                      "entity " FORMAT_HV " is of a class LibreDWG can't "
                      "decode",
                      ent->handle.value);
          free (handles);
          return DYNBLOCK_REFUSED | DWG_ERR_UNHANDLEDCLASS;
        }
      switch (ent->fixedtype)
        {
        case DWG_TYPE_INSERT:
          num_owned = ent->tio.entity->tio.INSERT->num_owned;
          owned = ent->tio.entity->tio.INSERT->attribs;
          seqend = ent->tio.entity->tio.INSERT->seqend;
          break;
        case DWG_TYPE_MINSERT:
          num_owned = ent->tio.entity->tio.MINSERT->num_owned;
          owned = ent->tio.entity->tio.MINSERT->attribs;
          seqend = ent->tio.entity->tio.MINSERT->seqend;
          break;
        case DWG_TYPE_POLYLINE_2D:
        case DWG_TYPE_POLYLINE_3D:
        case DWG_TYPE_POLYLINE_MESH:
        case DWG_TYPE_POLYLINE_PFACE:
          // they share COMMON_ENTITY_POLYLINE
          num_owned = ent->tio.entity->tio.POLYLINE_2D->num_owned;
          owned = ent->tio.entity->tio.POLYLINE_2D->vertex;
          seqend = ent->tio.entity->tio.POLYLINE_2D->seqend;
          break;
        case DWG_TYPE_TABLE:
          num_owned = ent->tio.entity->tio.TABLE->num_owned;
          owned = ent->tio.entity->tio.TABLE->attribs;
          seqend = ent->tio.entity->tio.TABLE->seqend;
          break;
        default:
          break;
        }
      if (!owned)
        num_owned = 0;
      if (count + 2 + num_owned > size)
        {
          BITCODE_RLL *more;
          size = (count + 2 + num_owned) * 2;
          more = (BITCODE_RLL *)realloc (handles, size * sizeof (BITCODE_RLL));
          if (!more)
            {
              free (handles);
              return DWG_ERR_OUTOFMEM;
            }
          handles = more;
        }
      handles[count++] = ent->handle.value;
      for (BITCODE_BL j = 0; j <= num_owned; j++)
        {
          // the owned ATTRIBs or VERTEXes, then the SEQEND
          const Dwg_Object_Ref *ref = j < num_owned ? owned[j] : seqend;
          const Dwg_Object *sub;
          if (!ref_handle (ref))
            continue;
          sub = ref_object (dwg, ref);
          if (!sub || sub->supertype != DWG_SUPERTYPE_ENTITY)
            {
              set_reason (reason, reason_size,
                          "the definition's entity " FORMAT_HV
                          " owns " FORMAT_HV ", which is not an entity",
                          ent->handle.value, ref_handle (ref));
              free (handles);
              return DYNBLOCK_REFUSED | DWG_ERR_INVALIDHANDLE;
            }
          handles[count++] = sub->handle.value;
        }
    }
  *handlesp = handles;
  *countp = count;
  return 0;
}

/* Sets the invisible flags of the copies of the entities visibility
   parameter vis controls, for its state: the entities of the state show,
   the others are hidden. */
static int
apply_visibility (const Dwg_Data *dwg, const Dwg_Object *vis,
                  const BITCODE_BL state, const Dwg_Copy_Map *map)
{
  const Dwg_Object_BLOCKVISIBILITYPARAMETER *_vis
      = vis->tio.object->tio.BLOCKVISIBILITYPARAMETER;
  const Dwg_BLOCKVISIBILITYPARAMETER_state *_state = &_vis->states[state];
  dwg_inthash *shown = hash_new (_state->num_blocks * 2 + 8);
  if (!shown)
    return DWG_ERR_OUTOFMEM;
  for (BITCODE_BL i = 0; _state->blocks && i < _state->num_blocks; i++)
    if (ref_handle (_state->blocks[i]))
      hash_set (shown, ref_handle (_state->blocks[i]), 1);
  for (BITCODE_BL i = 0; _vis->blocks && i < _vis->num_blocks; i++)
    {
      const BITCODE_RLL source = ref_handle (_vis->blocks[i]);
      Dwg_Object *copy
          = source ? handle_object (dwg, dwg_copy_map_get (map, source))
                   : NULL;
      if (copy && copy->supertype == DWG_SUPERTYPE_ENTITY)
        copy->tio.entity->invisible
            = hash_get (shown, source) == HASH_NOT_FOUND ? 1 : 0;
    }
  free_hash (shown);
  return 0;
}

/* Drops the extension dictionary of copy, and its reactors that point at
   no object in copies. */
static void
detach_copy (Dwg_Object *copy, dwg_inthash *copies)
{
  Dwg_Object_Entity *ent = copy->tio.entity;
  BITCODE_BL kept = 0;
  ent->xdicobjhandle = NULL;
  ent->is_xdic_missing = 1;
  for (BITCODE_BL i = 0; ent->reactors && i < ent->num_reactors; i++)
    if (ent->reactors[i] && ent->reactors[i]->absolute_ref
        && hash_get (copies, ent->reactors[i]->absolute_ref)
               != HASH_NOT_FOUND)
      ent->reactors[kept++] = ent->reactors[i];
  ent->num_reactors = kept;
}

/*--------------------------------------------------------------------------------
 * Linear and rotation parameters, and their actions
 *
 * A LINEAR value moves its parameter's end point along the parameter, to
 * the value's distance from its base point. The stretch and move actions
 * connected to that end point then move entities by its change: a move
 * action its whole selection, a stretch action the stretch points its
 * frame holds, which AutoCAD lists with the action (hdls) by their index
 * in the entity's own list of stretch points. An entity wholly inside the
 * frame has all its stretch points listed and so moves whole. AutoCAD also
 * moves whole an entity of the selection (deps) that lies wholly outside
 * the frame; how it lists one is not known, so the evaluator refuses an
 * entity of the selection with no stretch point listed. Each action scales
 * the change by its distance multiplier and turns it by its angle offset.
 *
 * Actions also move the points of other parameters in their selection
 * (codes, for a stretch, and the node ids a move or rotate action lists),
 * which changes those parameters' values unless they are set too. So each
 * linear parameter is evaluated after the actions that move it, of the
 * properties that run: those given a value that changes their parameter,
 * or that such an action moves. The actions of the others don't run. The
 * evaluation graph doesn't list its nodes in that order. As the stretch
 * and move actions translate, their order doesn't change the geometry
 * otherwise.
 *
 * A ROTATION value turns its parameter's end point about its base point,
 * and the rotate actions connected to the parameter then turn the
 * entities they select, whole, about the base point by the change in
 * angle. A turn and a translation of the same entity give different
 * geometry in each order, and the order AutoCAD runs unrelated actions in
 * is not known, so the evaluator refuses an entity that a rotate action
 * turns and another action that runs moves or turns too; and an action
 * that runs moving a rotation parameter given a value, or a rotate action
 * that runs turning a linear parameter that runs. Rotation parameters are
 * then independent of the others, and are evaluated after them.
 *
 * Everything is planned on the definition first, as moves and turns of
 * its entities, so that a refusal comes before anything is copied.
 */

/* A linear parameter during the evaluation: its points as the actions
   evaluated so far moved them, and the value it is set to. */
typedef struct _dynblock_linear
{
  const Dwg_Object *obj;
  const char *name; /* the property's, UTF-8 */
  BITCODE_BL nodeid;
  BITCODE_3BD base;
  BITCODE_3BD end;
  int given;
  double value;
  int running; /* its actions run */
  int done;
} Dynblock_Linear;

/* A rotation parameter during the evaluation, and the value it is set
   to, reduced to [0, 2 pi), so that a huge one runs and turns the points
   and the angles alike, and the angles keep their sweeps. */
typedef struct _dynblock_rotation
{
  const Dwg_Object *obj;
  const char *name; /* the property's, UTF-8 */
  BITCODE_BL nodeid;
  int given;
  double value;
  int running; /* its actions run */
} Dynblock_Rotation;

/* An action of the evaluation graph, and the linear parameter whose value
   it takes. */
typedef struct _dynblock_action
{
  const Dwg_Object *obj;
  char *name; /* UTF-8 */
  int linear; /* index into the linear parameters, or -1 */
} Dynblock_Action;

/* A move of a definition entity, which its copy gets: of all of it, or of
   one of its stretch points. */
typedef struct _dynblock_move
{
  BITCODE_RLL entity;
  int point; /* the stretch point, or -1 for the whole entity */
  BITCODE_3BD delta;
  const char *action; /* the action's name, UTF-8 */
} Dynblock_Move;

/* A turn of a whole definition entity, which its copy gets: about the
   point center of the block's XY plane, counter-clockwise by angle
   radians. */
typedef struct _dynblock_turn
{
  BITCODE_RLL entity;
  BITCODE_2RD center;
  double angle;
  const char *action; /* the action's name, UTF-8 */
} Dynblock_Turn;

typedef struct _dynblock_plan
{
  const Dwg_Data *dwg;
  dwg_inthash *entities; /* def's own entities */
  const Dwg_Object **nodes; /* the evaluation graph's elements */
  BITCODE_BL num_nodes;
  dwg_inthash *node_handles;
  Dynblock_Linear *linear;
  BITCODE_BL num_linear;
  Dynblock_Rotation *rotations;
  BITCODE_BL num_rotations;
  Dynblock_Action *actions;
  BITCODE_BL num_actions;
  Dynblock_Move *moves;
  BITCODE_BL num_moves;
  BITCODE_BL size_moves;
  Dynblock_Turn *turns;
  BITCODE_BL num_turns;
  BITCODE_BL size_turns;
  char *reason;
  size_t reason_size;
} Dynblock_Plan;

/* The connections of action obj to the values of its parameters, or NULL
   when obj is not an action with them. */
static const Dwg_BLOCKACTION_connectionpts *
action_connections (const Dwg_Object *obj, BITCODE_BL *countp)
{
  *countp = 0;
  switch (obj->fixedtype)
    {
    case DWG_TYPE_BLOCKMOVEACTION:
      *countp = 2;
      return obj->tio.object->tio.BLOCKMOVEACTION->conn_pts;
    case DWG_TYPE_BLOCKSTRETCHACTION:
      *countp = 2;
      return obj->tio.object->tio.BLOCKSTRETCHACTION->conn_pts;
    case DWG_TYPE_BLOCKSCALEACTION:
      *countp = 5;
      return obj->tio.object->tio.BLOCKSCALEACTION->conn_pts;
    case DWG_TYPE_BLOCKROTATEACTION:
      *countp = 3;
      return obj->tio.object->tio.BLOCKROTATEACTION->conn_pts;
    case DWG_TYPE_BLOCKFLIPACTION:
      *countp = 4;
      return obj->tio.object->tio.BLOCKFLIPACTION->conn_pts;
    case DWG_TYPE_BLOCKARRAYACTION:
      *countp = 4;
      return obj->tio.object->tio.BLOCKARRAYACTION->conn_pts;
    case DWG_TYPE_BLOCKPOLARSTRETCHACTION:
      *countp = 6;
      return obj->tio.object->tio.BLOCKPOLARSTRETCHACTION->conn_pts;
    default:
      return NULL;
    }
}

/* Whether action obj takes a value of the parameter with node id nodeid. */
static int
is_connected (const Dwg_Object *obj, const BITCODE_BL nodeid)
{
  BITCODE_BL n;
  const Dwg_BLOCKACTION_connectionpts *conn = action_connections (obj, &n);
  for (BITCODE_BL i = 0; conn && i < n; i++)
    if (conn[i].code == nodeid)
      return 1;
  return 0;
}

static int
find_linear (const Dynblock_Plan *plan, const BITCODE_BL nodeid)
{
  for (BITCODE_BL i = 0; i < plan->num_linear; i++)
    if (plan->linear[i].nodeid == nodeid)
      return (int)i;
  return -1;
}

static int
find_rotation (const Dynblock_Plan *plan, const BITCODE_BL nodeid)
{
  for (BITCODE_BL i = 0; i < plan->num_rotations; i++)
    if (plan->rotations[i].nodeid == nodeid)
      return (int)i;
  return -1;
}

static BITCODE_BL
node_id (const Dwg_Object *obj)
{
  // every graph element starts with Dynblock_Element
  return ((const Dynblock_Element *)obj->tio.object->tio.BLOCKMOVEACTION)
      ->evalexpr.nodeid;
}

/* The node id of the parameter whose value action a takes: the first its
   connections name. */
static BITCODE_BL
action_parameter (const Dynblock_Action *a)
{
  BITCODE_BL n;
  const Dwg_BLOCKACTION_connectionpts *conn
      = action_connections (a->obj, &n);
  return conn && n ? conn[0].code : 0;
}

/* The node of the evaluation graph with node id nodeid, or NULL. */
static const Dwg_Object *
find_node (const Dynblock_Plan *plan, const BITCODE_BL nodeid)
{
  for (BITCODE_BL i = 0; i < plan->num_nodes; i++)
    {
      const Dynblock_Element *e
          = plan->nodes[i] ? (const Dynblock_Element *)plan->nodes[i]
                                 ->tio.object->tio.BLOCKMOVEACTION
                           : NULL;
      if (e && e->evalexpr.nodeid == nodeid)
        return plan->nodes[i];
    }
  return NULL;
}

static int
is_parameter (const Dwg_Object *obj)
{
  switch (obj->fixedtype)
    {
    case DWG_TYPE_BLOCKLINEARPARAMETER:
    case DWG_TYPE_BLOCKROTATIONPARAMETER:
    case DWG_TYPE_BLOCKVISIBILITYPARAMETER:
    case DWG_TYPE_BLOCKFLIPPARAMETER:
    case DWG_TYPE_BLOCKXYPARAMETER:
    case DWG_TYPE_BLOCKPOLARPARAMETER:
    case DWG_TYPE_BLOCKPOINTPARAMETER:
    case DWG_TYPE_BLOCKLOOKUPPARAMETER:
    case DWG_TYPE_BLOCKALIGNMENTPARAMETER:
    case DWG_TYPE_BLOCKBASEPOINTPARAMETER:
    case DWG_TYPE_BLOCKUSERPARAMETER:
    case DWG_TYPE_BLOCKLINEARCONSTRAINTPARAMETER:
    case DWG_TYPE_BLOCKALIGNEDCONSTRAINTPARAMETER:
    case DWG_TYPE_BLOCKHORIZONTALCONSTRAINTPARAMETER:
    case DWG_TYPE_BLOCKVERTICALCONSTRAINTPARAMETER:
    case DWG_TYPE_BLOCKANGULARCONSTRAINTPARAMETER:
    case DWG_TYPE_BLOCKDIAMETRICCONSTRAINTPARAMETER:
    case DWG_TYPE_BLOCKRADIALCONSTRAINTPARAMETER:
      return 1;
    default:
      return 0;
    }
}

/* Whether obj is of a type of evaluation graph node whose struct starts
   as Dynblock_Element does: a parameter, an action, a grip, a grip
   location component or a proxy node. */
static int
is_graph_element (const Dwg_Object *obj)
{
  BITCODE_BL n;
  if (is_parameter (obj) || action_connections (obj, &n))
    return 1;
  switch (obj->fixedtype)
    {
    case DWG_TYPE_BLOCKLOOKUPACTION:
    case DWG_TYPE_BLOCKALIGNMENTGRIP:
    case DWG_TYPE_BLOCKFLIPGRIP:
    case DWG_TYPE_BLOCKLINEARGRIP:
    case DWG_TYPE_BLOCKLOOKUPGRIP:
    case DWG_TYPE_BLOCKPOLARGRIP:
    case DWG_TYPE_BLOCKROTATIONGRIP:
    case DWG_TYPE_BLOCKVISIBILITYGRIP:
    case DWG_TYPE_BLOCKXYGRIP:
    case DWG_TYPE_BLOCKPROPERTIESTABLEGRIP:
    case DWG_TYPE_BLOCKGRIPLOCATIONCOMPONENT:
    case DWG_TYPE_DYNAMICBLOCKPROXYNODE:
      return 1;
    default:
      return 0;
    }
}

/* Refuses an action that runs moving the points of rotation parameter r
   given a value: its rotate actions would turn about a point that
   moved. */
static int
check_moved_rotation (Dynblock_Plan *plan, const Dynblock_Action *a,
                      const int r)
{
  if (r < 0 || !plan->rotations[r].given)
    return 0;
  set_reason (plan->reason, plan->reason_size,
              "action '%s' moves rotation property '%s', which is given a "
              "value; that is not supported",
              a->name, plan->rotations[r].name);
  return DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
}

/* Checks the node with node id nodeid, other than a linear parameter,
   whose points action a moves: refuses an id the evaluation graph doesn't
   have, a rotation parameter given a value, as check_moved_rotation ()
   says, and a parameter that chains its actions, as AutoCAD would run them
   too. */
static int
check_selected_node (Dynblock_Plan *plan, const Dynblock_Action *a,
                     const BITCODE_BL nodeid)
{
  const Dwg_Object *obj = find_node (plan, nodeid);
  int error;
  if (!obj)
    {
      set_reason (plan->reason, plan->reason_size,
                  "action '%s' selects node %u, which the evaluation graph "
                  "doesn't have",
                  a->name, (unsigned)nodeid);
      return DYNBLOCK_REFUSED | DWG_ERR_INVALIDHANDLE;
    }
  error = check_moved_rotation (plan, a, find_rotation (plan, nodeid));
  if (error)
    return error;
  // every parameter starts with BLOCKPARAMETER_fields
  if (!is_parameter (obj)
      || !obj->tio.object->tio.BLOCKVISIBILITYPARAMETER->chain_actions)
    return 0;
  set_reason (plan->reason, plan->reason_size,
              "action '%s' moves %s (node %u), which chains its actions; "
              "that is not supported",
              a->name, dwg_type_name (obj->fixedtype), (unsigned)nodeid);
  return DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
}

/* Refuses rotate action a turning linear parameter k when it runs: its
   stretch and move actions would then run along a turned direction. */
static int
check_turned_linear (Dynblock_Plan *plan, const Dynblock_Action *a,
                     const int k)
{
  if (k < 0 || !plan->linear[k].running)
    return 0;
  set_reason (plan->reason, plan->reason_size,
              "rotate action '%s' turns linear property '%s', whose value "
              "changes; that is not supported",
              a->name, plan->linear[k].name);
  return DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
}

/* Whether action a lists the parameter with node id nodeid among those
   whose points it moves: a stretch action in codes, a move or rotate
   action by its node id. */
static int
lists_node (const Dwg_Object *a, const BITCODE_BL nodeid)
{
  if (a->fixedtype == DWG_TYPE_BLOCKSTRETCHACTION)
    {
      const Dwg_Object_BLOCKSTRETCHACTION *_a
          = a->tio.object->tio.BLOCKSTRETCHACTION;
      for (BITCODE_BL i = 0; _a->codes && i < _a->num_codes; i++)
        if (_a->codes[i].bl95 == nodeid)
          return 1;
      return 0;
    }
  else
    {
      // every action starts with BLOCKACTION_fields
      const Dwg_Object_BLOCKMOVEACTION *_a
          = a->tio.object->tio.BLOCKMOVEACTION;
      for (BITCODE_BL i = 0; _a->actions && i < _a->num_actions; i++)
        if (_a->actions[i] == nodeid)
          return 1;
      return 0;
    }
}

/* Checks node obj of the evaluation graph, which action a selects (deps).
   A parameter the action lists among those whose points it moves is
   checked with them, and the action's own parameter is the one its value
   changes. Another parameter would move with the selection, and how
   AutoCAD stores one that does is not known: refuses one that chains its
   actions, which AutoCAD would then run, a rotation parameter given a
   value, or a linear parameter that runs, which a rotate action would
   turn, as check_turned_linear () says. Grips are left alone. */
static int
check_selected_dep (Dynblock_Plan *plan, const Dynblock_Action *a,
                    const Dwg_Object *obj)
{
  const BITCODE_BL nodeid = node_id (obj);
  const int r = find_rotation (plan, nodeid);
  int error;
  if (!is_parameter (obj) || nodeid == action_parameter (a)
      || lists_node (a->obj, nodeid))
    return 0;
  if (a->obj->fixedtype == DWG_TYPE_BLOCKROTATEACTION)
    {
      error = check_turned_linear (plan, a, find_linear (plan, nodeid));
      if (error)
        return error;
    }
  if (r >= 0 && plan->rotations[r].given)
    {
      set_reason (plan->reason, plan->reason_size,
                  "action '%s' selects rotation property '%s', which is "
                  "given a value, without listing it among the parameters "
                  "it moves; that is not supported",
                  a->name, plan->rotations[r].name);
      return DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
    }
  // every parameter starts with BLOCKPARAMETER_fields
  if (!obj->tio.object->tio.BLOCKVISIBILITYPARAMETER->chain_actions)
    return 0;
  set_reason (plan->reason, plan->reason_size,
              "action '%s' selects %s (node %u), which chains its actions, "
              "without listing it among the parameters it moves; that is not "
              "supported",
              a->name, dwg_type_name (obj->fixedtype), (unsigned)nodeid);
  return DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
}

static int
add_move (Dynblock_Plan *plan, const Dynblock_Action *a,
          const BITCODE_RLL entity, const int point, const BITCODE_3BD *delta)
{
  Dynblock_Move *m;
  if (plan->num_moves == plan->size_moves)
    {
      const BITCODE_BL size = plan->size_moves ? plan->size_moves * 2 : 32;
      Dynblock_Move *moves = (Dynblock_Move *)realloc (
          plan->moves, size * sizeof (Dynblock_Move));
      if (!moves)
        return DWG_ERR_OUTOFMEM;
      plan->moves = moves;
      plan->size_moves = size;
    }
  m = &plan->moves[plan->num_moves++];
  m->entity = entity;
  m->point = point;
  m->delta = *delta;
  m->action = a->name;
  return 0;
}

/* The number of stretch points AutoCAD gives entity obj, which a stretch
   action's indexes count: a LINE its start and end, an LWPOLYLINE its
   vertices, a SPLINE its control points (its fit points when it is defined
   by them), a TEXT or ATTDEF its position, an INSERT its insertion point;
   2 for an ARC, 1 for a CIRCLE or a HATCH, 5 for an ELLIPSE or MTEXT, 4
   for an elliptical arc. 0 for the other types, which the evaluator
   doesn't stretch, and for a TEXT or ATTDEF aligned or fitted between two
   points. */
static BITCODE_BL
stretch_points (const Dwg_Object *obj)
{
  switch (obj->fixedtype)
    {
    case DWG_TYPE_LINE:
    case DWG_TYPE_ARC:
      return 2;
    case DWG_TYPE_LWPOLYLINE:
      return obj->tio.entity->tio.LWPOLYLINE->points
                 ? obj->tio.entity->tio.LWPOLYLINE->num_points
                 : 0;
    case DWG_TYPE_CIRCLE:
    case DWG_TYPE_INSERT:
    case DWG_TYPE_HATCH:
      return 1;
    case DWG_TYPE_ELLIPSE:
      {
        const Dwg_Entity_ELLIPSE *_obj = obj->tio.entity->tio.ELLIPSE;
        const double sweep = fabs (_obj->end_angle - _obj->start_angle);
        return fabs (sweep - 2.0 * M_PI) < DYNBLOCK_VALUE_EPSILON ? 5 : 4;
      }
    case DWG_TYPE_SPLINE:
      {
        const Dwg_Entity_SPLINE *_obj = obj->tio.entity->tio.SPLINE;
        return _obj->scenario == 2 ? _obj->num_fit_pts : _obj->num_ctrl_pts;
      }
    case DWG_TYPE_MTEXT:
      return 5;
    case DWG_TYPE_TEXT:
      {
        const BITCODE_BS h = obj->tio.entity->tio.TEXT->horiz_alignment;
        return h == 3 || h == 5 ? 0 : 1;
      }
    case DWG_TYPE_ATTDEF:
      {
        const BITCODE_BS h = obj->tio.entity->tio.ATTDEF->horiz_alignment;
        return h == 3 || h == 5 ? 0 : 1;
      }
    default:
      return 0;
    }
}

/* Whether the evaluator moves an entity of obj's type whole. */
static int
is_movable (const Dwg_Object *obj)
{
  switch (obj->fixedtype)
    {
    case DWG_TYPE_LINE:
    case DWG_TYPE_LWPOLYLINE:
    case DWG_TYPE_ARC:
    case DWG_TYPE_CIRCLE:
    case DWG_TYPE_ELLIPSE:
    case DWG_TYPE_SPLINE:
    case DWG_TYPE_TEXT:
    case DWG_TYPE_ATTDEF:
    case DWG_TYPE_MTEXT:
    case DWG_TYPE_INSERT:
    case DWG_TYPE_HATCH:
      return 1;
    default:
      return 0;
    }
}

/* The extrusion of an entity whose points are in its object coordinate
   system (OCS), with *planarp set when its points are 2D at an elevation;
   NULL for an entity with WCS points. */
static const BITCODE_BE *
entity_ocs (const Dwg_Object *obj, int *planarp)
{
  *planarp = 0;
  switch (obj->fixedtype)
    {
    case DWG_TYPE_ARC:
      return &obj->tio.entity->tio.ARC->extrusion;
    case DWG_TYPE_CIRCLE:
      return &obj->tio.entity->tio.CIRCLE->extrusion;
    case DWG_TYPE_INSERT:
      return &obj->tio.entity->tio.INSERT->extrusion;
    case DWG_TYPE_LWPOLYLINE:
      *planarp = 1;
      return &obj->tio.entity->tio.LWPOLYLINE->extrusion;
    case DWG_TYPE_TEXT:
      *planarp = 1;
      return &obj->tio.entity->tio.TEXT->extrusion;
    case DWG_TYPE_ATTDEF:
      *planarp = 1;
      return &obj->tio.entity->tio.ATTDEF->extrusion;
    case DWG_TYPE_ATTRIB:
      *planarp = 1;
      return &obj->tio.entity->tio.ATTRIB->extrusion;
    case DWG_TYPE_HATCH:
      *planarp = 1;
      return &obj->tio.entity->tio.HATCH->extrusion;
    default:
      return NULL;
    }
}

/* delta in the coordinates of entity obj: its OCS, by the arbitrary axis
   algorithm, for an entity with OCS points. */
static void
entity_delta (const Dwg_Object *obj, const BITCODE_3BD *delta,
              BITCODE_3BD *out)
{
  int planar;
  const BITCODE_BE *ext = entity_ocs (obj, &planar);
  if (!ext || (ext->x == 0.0 && ext->y == 0.0 && ext->z >= 0.0))
    *out = *delta;
  else
    transform_OCS (out, *delta, *ext);
}

/* Refuses a move of entity obj, or of an ATTRIB it owns, that would take
   2D points out of their plane: possible only for an entity whose
   extrusion is not along Z. */
static int
check_planar (Dynblock_Plan *plan, const Dwg_Object *obj,
              const BITCODE_3BD *delta)
{
  int planar;
  BITCODE_3BD d;
  if (!entity_ocs (obj, &planar) || !planar)
    return 0;
  entity_delta (obj, delta, &d);
  if (fabs (d.z) <= DYNBLOCK_VALUE_EPSILON)
    return 0;
  set_reason (plan->reason, plan->reason_size,
              "moving %s " FORMAT_HV " out of its plane is not supported",
              dwg_type_name (obj->fixedtype), obj->handle.value);
  return DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
}

/* Grows the box lo, hi to hold the point x, y. */
static void
box_add (double *lo, double *hi, const double x, const double y)
{
  lo[0] = fmin (lo[0], x);
  lo[1] = fmin (lo[1], y);
  hi[0] = fmax (hi[0], x);
  hi[1] = fmax (hi[1], y);
}

/* A box that holds the boundary of HATCH _obj, in its OCS: arcs and
   bulges count as their whole circle, an ellipse as the circle of its
   major axis, and a spline as its control and fit points. Returns 0 when
   the boundary has no point, or a segment of a type it doesn't know. */
static int
hatch_box (const Dwg_Entity_HATCH *_obj, double *lo, double *hi)
{
  lo[0] = lo[1] = HUGE_VAL;
  hi[0] = hi[1] = -HUGE_VAL;
  for (BITCODE_BL i = 0; _obj->paths && i < _obj->num_paths; i++)
    {
      const Dwg_HATCH_Path *path = &_obj->paths[i];
      if (path->flag & 2)
        {
          // a polyline path. The arc of a bulged segment, of radius r,
          // lies within 2 r of its chord's middle, which is inside its
          // circle.
          const BITCODE_BL n = path->num_segs_or_paths;
          for (BITCODE_BL j = 0; path->polyline_paths && j < n; j++)
            {
              const Dwg_HATCH_PolylinePath *p = &path->polyline_paths[j];
              const Dwg_HATCH_PolylinePath *q
                  = &path->polyline_paths[(j + 1) % n];
              box_add (lo, hi, p->point.x, p->point.y);
              if (path->bulges_present && p->bulge != 0.0)
                {
                  const double chord = hypot (q->point.x - p->point.x,
                                              q->point.y - p->point.y);
                  const double reach = chord * (1.0 + p->bulge * p->bulge)
                                       / (2.0 * fabs (p->bulge));
                  const double mx = (p->point.x + q->point.x) / 2.0;
                  const double my = (p->point.y + q->point.y) / 2.0;
                  box_add (lo, hi, mx - reach, my - reach);
                  box_add (lo, hi, mx + reach, my + reach);
                }
            }
          continue;
        }
      for (BITCODE_BL j = 0; path->segs && j < path->num_segs_or_paths; j++)
        {
          const Dwg_HATCH_PathSeg *s = &path->segs[j];
          switch (s->curve_type)
            {
            case 1:
              box_add (lo, hi, s->first_endpoint.x, s->first_endpoint.y);
              box_add (lo, hi, s->second_endpoint.x, s->second_endpoint.y);
              break;
            case 2:
              box_add (lo, hi, s->center.x - s->radius,
                       s->center.y - s->radius);
              box_add (lo, hi, s->center.x + s->radius,
                       s->center.y + s->radius);
              break;
            case 3:
              {
                // endpoint is the major axis, from the centre
                const double r = hypot (s->endpoint.x, s->endpoint.y);
                box_add (lo, hi, s->center.x - r, s->center.y - r);
                box_add (lo, hi, s->center.x + r, s->center.y + r);
              }
              break;
            case 4:
              for (BITCODE_BL k = 0;
                   s->control_points && k < s->num_control_points; k++)
                box_add (lo, hi, s->control_points[k].point.x,
                         s->control_points[k].point.y);
              for (BITCODE_BL k = 0; s->fitpts && k < s->num_fitpts; k++)
                box_add (lo, hi, s->fitpts[k].x, s->fitpts[k].y);
              break;
            default:
              return 0;
            }
        }
    }
  return lo[0] <= hi[0] && lo[1] <= hi[1];
}

/* Refuses a whole move of HATCH obj by stretch action a unless its
   boundary lies wholly inside the action's frame, which must be a
   rectangle, and its extrusion is along +Z, so that its OCS is the
   frame's. A HATCH has a single stretch point, so one that is listed may
   still cross the frame, and a stretch would then reshape its boundary
   rather than move it. */
static int
check_hatch (Dynblock_Plan *plan, const Dynblock_Action *a,
             const Dwg_Object *obj)
{
  const Dwg_Object_BLOCKSTRETCHACTION *_a
      = a->obj->tio.object->tio.BLOCKSTRETCHACTION;
  const Dwg_Entity_HATCH *_obj = obj->tio.entity->tio.HATCH;
  double lo[2], hi[2];
  if (_a->num_pts != 2 || !_a->pts)
    {
      set_reason (plan->reason, plan->reason_size,
                  "stretch action '%s' moves HATCH " FORMAT_HV
                  ": only a rectangular frame is supported",
                  a->name, obj->handle.value);
      return DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
    }
  if (_obj->extrusion.x != 0.0 || _obj->extrusion.y != 0.0
      || _obj->extrusion.z <= 0.0)
    {
      set_reason (plan->reason, plan->reason_size,
                  "stretch action '%s' moves HATCH " FORMAT_HV
                  ", which is not in the XY plane",
                  a->name, obj->handle.value);
      return DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
    }
  if (!hatch_box (_obj, lo, hi)
      || lo[0] < fmin (_a->pts[0].x, _a->pts[1].x) - DYNBLOCK_VALUE_EPSILON
      || hi[0] > fmax (_a->pts[0].x, _a->pts[1].x) + DYNBLOCK_VALUE_EPSILON
      || lo[1] < fmin (_a->pts[0].y, _a->pts[1].y) - DYNBLOCK_VALUE_EPSILON
      || hi[1] > fmax (_a->pts[0].y, _a->pts[1].y) + DYNBLOCK_VALUE_EPSILON)
    {
      set_reason (plan->reason, plan->reason_size,
                  "stretch action '%s': HATCH " FORMAT_HV
                  " is not wholly inside its frame, which is not supported",
                  a->name, obj->handle.value);
      return DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
    }
  return 0;
}

/* The entity of the definition named by handle in the selection of action
   a, or NULL with the reason when it isn't one. */
static const Dwg_Object *
selected_entity (Dynblock_Plan *plan, const Dynblock_Action *a,
                 const BITCODE_RLL handle, int *errorp)
{
  const Dwg_Object *obj = handle_object (plan->dwg, handle);
  *errorp = 0;
  if (!obj || obj->supertype != DWG_SUPERTYPE_ENTITY
      || hash_get (plan->entities, handle) == HASH_NOT_FOUND)
    {
      set_reason (plan->reason, plan->reason_size,
                  "action '%s' selects " FORMAT_HV
                  ", which is not an entity of the definition",
                  a->name, handle);
      *errorp = DYNBLOCK_REFUSED | DWG_ERR_INVALIDHANDLE;
      return NULL;
    }
  return obj;
}

/* Plans a whole move of entity obj by delta, and of the ATTRIBs an INSERT
   owns. */
static int
plan_whole (Dynblock_Plan *plan, const Dynblock_Action *a,
            const Dwg_Object *obj, const BITCODE_3BD *delta)
{
  int error;
  if (!is_movable (obj))
    {
      set_reason (plan->reason, plan->reason_size,
                  "action '%s': moving %s " FORMAT_HV " is not supported",
                  a->name, dwg_type_name (obj->fixedtype),
                  obj->handle.value);
      return DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
    }
  error = check_planar (plan, obj, delta);
  if (!error && obj->fixedtype == DWG_TYPE_INSERT)
    {
      const Dwg_Entity_INSERT *_obj = obj->tio.entity->tio.INSERT;
      for (BITCODE_BL i = 0; !error && _obj->attribs && i < _obj->num_owned;
           i++)
        {
          const Dwg_Object *attrib = ref_object (plan->dwg, _obj->attribs[i]);
          if (attrib && attrib->fixedtype == DWG_TYPE_ATTRIB)
            error = check_planar (plan, attrib, delta);
        }
    }
  return error ? error : add_move (plan, a, obj->handle.value, -1, delta);
}

/* Refuses an entity that stretch action a selects (deps) with none of its
   stretch points listed (hdls): one wholly outside the action's frame,
   which AutoCAD moves whole. The parameters and grips the action selects
   are nodes of the graph: it lists the points of the parameters it moves
   by node id (codes), and its own parameter, whose end point the value
   moves, with no entry; check_selected_dep () checks the others. */
static int
check_stretch_selection (Dynblock_Plan *plan, const Dynblock_Action *a)
{
  const Dwg_Object_BLOCKSTRETCHACTION *_a
      = a->obj->tio.object->tio.BLOCKSTRETCHACTION;
  dwg_inthash *listed = hash_new ((uint64_t)_a->num_hdls * 2 + 8);
  int error = 0;
  if (!listed)
    return DWG_ERR_OUTOFMEM;
  for (BITCODE_BL i = 0; _a->hdls && i < _a->num_hdls; i++)
    {
      const BITCODE_RLL handle = ref_handle (_a->hdls[i].hdl);
      if (handle && _a->hdls[i].indexes && _a->hdls[i].num_indexes)
        hash_set (listed, handle, 1);
    }
  for (BITCODE_BL i = 0; _a->deps && i < _a->num_deps && !error; i++)
    {
      const BITCODE_RLL handle = ref_handle (_a->deps[i]);
      const Dwg_Object *obj;
      if (handle && hash_get (plan->node_handles, handle) != HASH_NOT_FOUND)
        {
          error = check_selected_dep (plan, a,
                                      handle_object (plan->dwg, handle));
          continue;
        }
      obj = selected_entity (plan, a, handle, &error);
      if (obj && hash_get (listed, handle) == HASH_NOT_FOUND)
        {
          set_reason (plan->reason, plan->reason_size,
                      "stretch action '%s' selects %s " FORMAT_HV
                      " with none of its stretch points in its frame; "
                      "moving an entity outside the frame is not supported",
                      a->name, dwg_type_name (obj->fixedtype), handle);
          error = DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
        }
    }
  free_hash (listed);
  return error;
}

/* Plans stretch action a moving the stretch points it lists by delta,
   and the points of the linear parameters it lists. */
static int
plan_stretch (Dynblock_Plan *plan, const Dynblock_Action *a,
              const BITCODE_3BD *delta)
{
  const Dwg_Object_BLOCKSTRETCHACTION *_a
      = a->obj->tio.object->tio.BLOCKSTRETCHACTION;
  int error = 0;
  for (BITCODE_BL i = 0; _a->hdls && i < _a->num_hdls && !error; i++)
    {
      const Dwg_BLOCKSTRETCHACTION_handles *h = &_a->hdls[i];
      const Dwg_Object *obj
          = selected_entity (plan, a, ref_handle (h->hdl), &error);
      BITCODE_BL count, listed = 0;
      unsigned char *seen;
      if (!obj)
        break;
      count = stretch_points (obj);
      if (!count)
        {
          set_reason (plan->reason, plan->reason_size,
                      "stretch action '%s': stretching %s " FORMAT_HV
                      " is not supported",
                      a->name, dwg_type_name (obj->fixedtype),
                      obj->handle.value);
          return DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
        }
      seen = (unsigned char *)calloc (count, 1);
      if (!seen)
        return DWG_ERR_OUTOFMEM;
      for (BITCODE_BS j = 0; h->indexes && j < h->num_indexes; j++)
        {
          if (h->indexes[j] >= count || seen[h->indexes[j]])
            {
              set_reason (plan->reason, plan->reason_size,
                          "stretch action '%s' lists point %u of %s " FORMAT_HV
                          ", which has %u",
                          a->name, (unsigned)h->indexes[j],
                          dwg_type_name (obj->fixedtype), obj->handle.value,
                          (unsigned)count);
              free (seen);
              return DYNBLOCK_REFUSED;
            }
          seen[h->indexes[j]] = 1;
          listed++;
        }
      free (seen);
      if (!listed)
        {
          set_reason (plan->reason, plan->reason_size,
                      "stretch action '%s' lists %s " FORMAT_HV
                      " with none of its stretch points in its frame; "
                      "moving an entity outside the frame is not supported",
                      a->name, dwg_type_name (obj->fixedtype),
                      obj->handle.value);
          return DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
        }
      if (listed == count)
        {
          if (obj->fixedtype == DWG_TYPE_HATCH)
            error = check_hatch (plan, a, obj);
          if (!error)
            error = plan_whole (plan, a, obj, delta);
        }
      else if (obj->fixedtype == DWG_TYPE_LINE
               || obj->fixedtype == DWG_TYPE_LWPOLYLINE)
        {
          error = check_planar (plan, obj, delta);
          for (BITCODE_BS j = 0; !error && j < h->num_indexes; j++)
            error = add_move (plan, a, obj->handle.value, (int)h->indexes[j],
                              delta);
        }
      else
        {
          set_reason (plan->reason, plan->reason_size,
                      "stretch action '%s': stretching part of %s " FORMAT_HV
                      " is not supported",
                      a->name, dwg_type_name (obj->fixedtype),
                      obj->handle.value);
          return DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
        }
    }
  if (!error)
    error = check_stretch_selection (plan, a);
  if (error)
    return error;
  for (BITCODE_BL i = 0; _a->codes && i < _a->num_codes; i++)
    {
      const Dwg_BLOCKSTRETCHACTION_codes *c = &_a->codes[i];
      const int k = find_linear (plan, c->bl95);
      if (k < 0)
        {
          error = check_selected_node (plan, a, c->bl95);
          if (error)
            return error;
        }
      // a parameter wholly outside the frame, which AutoCAD moves whole
      if (!c->indexes || !c->num_indexes)
        {
          const Dwg_Object *node = find_node (plan, c->bl95);
          set_reason (plan->reason, plan->reason_size,
                      "stretch action '%s' selects %s (node %u) with none "
                      "of its points in its frame; moving a parameter "
                      "outside the frame is not supported",
                      a->name,
                      node ? dwg_type_name (node->fixedtype) : "a node",
                      (unsigned)c->bl95);
          return DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
        }
      for (BITCODE_BS j = 0; k >= 0 && j < c->num_indexes; j++)
        {
          Dynblock_Linear *q = &plan->linear[k];
          if (c->indexes[j] > 1)
            {
              set_reason (plan->reason, plan->reason_size,
                          "stretch action '%s' lists point %u of linear "
                          "property '%s', which has 2",
                          a->name, (unsigned)c->indexes[j], q->name);
              return DYNBLOCK_REFUSED;
            }
          if (c->indexes[j] == 0)
            {
              q->base.x += delta->x;
              q->base.y += delta->y;
              q->base.z += delta->z;
            }
          else
            {
              q->end.x += delta->x;
              q->end.y += delta->y;
              q->end.z += delta->z;
            }
        }
    }
  return 0;
}

/* Plans move action a moving the entities it selects by delta, and the
   linear parameters it lists. */
static int
plan_move (Dynblock_Plan *plan, const Dynblock_Action *a,
           const BITCODE_3BD *delta)
{
  const Dwg_Object_BLOCKMOVEACTION *_a
      = a->obj->tio.object->tio.BLOCKMOVEACTION;
  int error = 0;
  for (BITCODE_BL i = 0; _a->deps && i < _a->num_deps && !error; i++)
    {
      const BITCODE_RLL handle = ref_handle (_a->deps[i]);
      const Dwg_Object *obj;
      // the parameters and grips it moves are nodes of the graph; it lists
      // the parameters by id too
      if (handle && hash_get (plan->node_handles, handle) != HASH_NOT_FOUND)
        {
          error = check_selected_dep (plan, a,
                                      handle_object (plan->dwg, handle));
          continue;
        }
      obj = selected_entity (plan, a, handle, &error);
      if (obj)
        error = plan_whole (plan, a, obj, delta);
    }
  for (BITCODE_BL i = 0; !error && _a->actions && i < _a->num_actions; i++)
    {
      const int k = find_linear (plan, _a->actions[i]);
      if (k < 0)
        error = check_selected_node (plan, a, _a->actions[i]);
      else
        {
          Dynblock_Linear *q = &plan->linear[k];
          q->base.x += delta->x;
          q->base.y += delta->y;
          q->base.z += delta->z;
          q->end.x += delta->x;
          q->end.y += delta->y;
          q->end.z += delta->z;
        }
    }
  return error;
}

/* Whether action a moves points of the linear parameter with node id
   nodeid. */
static int
moves_linear (const Dwg_Object *a, const BITCODE_BL nodeid)
{
  if (a->fixedtype == DWG_TYPE_BLOCKSTRETCHACTION)
    {
      const Dwg_Object_BLOCKSTRETCHACTION *_a
          = a->tio.object->tio.BLOCKSTRETCHACTION;
      for (BITCODE_BL i = 0; _a->codes && i < _a->num_codes; i++)
        if (_a->codes[i].bl95 == nodeid && _a->codes[i].num_indexes)
          return 1;
    }
  else if (a->fixedtype == DWG_TYPE_BLOCKMOVEACTION)
    {
      const Dwg_Object_BLOCKMOVEACTION *_a
          = a->tio.object->tio.BLOCKMOVEACTION;
      for (BITCODE_BL i = 0; _a->actions && i < _a->num_actions; i++)
        if (_a->actions[i] == nodeid)
          return 1;
    }
  return 0;
}

/* Marks the parameters that run, whose actions run: a linear one given a
   value that differs from its length, or that a stretch or move action of
   one that runs moves, worked out until no more are found; a rotation one
   given a value that differs from its angle. A rotate action doesn't
   change the length of a linear parameter it turns. */
static void
mark_running (Dynblock_Plan *plan)
{
  int more;
  for (BITCODE_BL i = 0; i < plan->num_linear; i++)
    {
      Dynblock_Linear *l = &plan->linear[i];
      l->running = l->given
                   && fabs (l->value - distance_3d (&l->base, &l->end))
                          > DYNBLOCK_VALUE_EPSILON;
    }
  for (BITCODE_BL i = 0; i < plan->num_rotations; i++)
    {
      Dynblock_Rotation *r = &plan->rotations[i];
      const Dwg_Object_BLOCKROTATIONPARAMETER *_r
          = r->obj->tio.object->tio.BLOCKROTATIONPARAMETER;
      r->running = r->given
                   && angle_difference (
                          r->value,
                          rotation_angle (&_r->def_basept, &_r->def_endpt,
                                          &_r->def_base_angle_pt))
                          > DYNBLOCK_VALUE_EPSILON;
    }
  do
    {
      more = 0;
      for (BITCODE_BL j = 0; j < plan->num_actions; j++)
        {
          const int k = plan->actions[j].linear;
          if (k < 0 || !plan->linear[k].running)
            continue;
          for (BITCODE_BL i = 0; i < plan->num_linear; i++)
            {
              Dynblock_Linear *l = &plan->linear[i];
              if (l->given && !l->running
                  && moves_linear (plan->actions[j].obj, l->nodeid))
                more = l->running = 1;
            }
        }
    }
  while (more);
}

/* The next linear parameter to evaluate: one that no action of a
   parameter that runs, not yet evaluated, moves; the actions of the
   others don't run. -1 when all are evaluated, -2 when the rest wait on
   parameters that run whose actions move each other's. */
static int
next_linear (const Dynblock_Plan *plan)
{
  int pending = 0;
  for (BITCODE_BL i = 0; i < plan->num_linear; i++)
    {
      int moved = 0;
      if (plan->linear[i].done)
        continue;
      pending = 1;
      for (BITCODE_BL j = 0; !moved && j < plan->num_actions; j++)
        {
          const int k = plan->actions[j].linear;
          moved = k >= 0 && k != (int)i && plan->linear[k].running
                  && !plan->linear[k].done
                  && moves_linear (plan->actions[j].obj,
                                   plan->linear[i].nodeid);
        }
      if (!moved)
        return (int)i;
    }
  return pending ? -2 : -1;
}

/* The change of the point an action's connection names, from the change
   of its linear parameter's end point: "EndXDelta" and "EndYDelta" take
   it, "BaseXDelta" and "BaseYDelta" 0, as a value doesn't move the base
   point. Returns 0 for another name. */
static int
connection_delta (const Dwg_BLOCKACTION_connectionpts *conn, const int y,
                  const BITCODE_3BD *end_delta, double *deltap)
{
  if (!conn->name)
    return 0;
  if (strEQ (conn->name, y ? "EndYDelta" : "EndXDelta"))
    {
      *deltap = y ? end_delta->y : end_delta->x;
      return 1;
    }
  if (strEQ (conn->name, y ? "BaseYDelta" : "BaseXDelta"))
    {
      *deltap = 0.0;
      return 1;
    }
  return 0;
}

/* Plans the actions connected to linear parameter p, whose end point moved
   by end_delta. */
static int
plan_actions_of (Dynblock_Plan *plan, const Dynblock_Linear *p,
                 const BITCODE_3BD *end_delta)
{
  for (BITCODE_BL i = 0; i < plan->num_actions; i++)
    {
      const Dynblock_Action *a = &plan->actions[i];
      const Dwg_BLOCKACTION_connectionpts *conn;
      BITCODE_BL n;
      double dx, dy, multiplier, angle;
      BITCODE_RC xy_type;
      BITCODE_3BD delta;
      int error;
      if (!is_connected (a->obj, p->nodeid))
        continue;
      conn = action_connections (a->obj, &n);
      if (a->obj->fixedtype != DWG_TYPE_BLOCKSTRETCHACTION
          && a->obj->fixedtype != DWG_TYPE_BLOCKMOVEACTION)
        {
          set_reason (plan->reason, plan->reason_size,
                      "linear property '%s' drives %s '%s', which is not "
                      "supported",
                      p->name, dwg_type_name (a->obj->fixedtype), a->name);
          return DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
        }
      if (conn[0].code != p->nodeid || conn[1].code != p->nodeid
          || !connection_delta (&conn[0], 0, end_delta, &dx)
          || !connection_delta (&conn[1], 1, end_delta, &dy))
        {
          char *name0 = utf8_copy (plan->dwg, conn[0].name);
          char *name1 = utf8_copy (plan->dwg, conn[1].name);
          set_reason (plan->reason, plan->reason_size,
                      "%s '%s' takes '%s' and '%s' of linear property '%s', "
                      "which is not supported",
                      dwg_type_name (a->obj->fixedtype), a->name,
                      name0 ? name0 : "", name1 ? name1 : "", p->name);
          free (name0);
          free (name1);
          return DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
        }
      if (a->obj->fixedtype == DWG_TYPE_BLOCKSTRETCHACTION)
        {
          const Dwg_Object_BLOCKSTRETCHACTION *_a
              = a->obj->tio.object->tio.BLOCKSTRETCHACTION;
          multiplier = _a->action_offset_x;
          angle = _a->action_offset_y;
          xy_type = _a->xy_type;
        }
      else
        {
          const Dwg_Object_BLOCKMOVEACTION *_a
              = a->obj->tio.object->tio.BLOCKMOVEACTION;
          multiplier = _a->action_offset_x;
          angle = _a->action_offset_y;
          xy_type = _a->xy_type;
        }
      if (xy_type)
        {
          set_reason (plan->reason, plan->reason_size,
                      "%s '%s' has XY type %u, which is not supported",
                      dwg_type_name (a->obj->fixedtype), a->name,
                      (unsigned)xy_type);
          return DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
        }
      // action_offset_x is the distance multiplier, action_offset_y the
      // angle offset, counter-clockwise
      delta.x = multiplier * (dx * cos (angle) - dy * sin (angle));
      delta.y = multiplier * (dx * sin (angle) + dy * cos (angle));
      delta.z = 0.0;
      if (!isfinite (delta.x) || !isfinite (delta.y))
        {
          set_reason (plan->reason, plan->reason_size,
                      "linear property '%s' is given %g, which %s '%s' "
                      "can't move points by",
                      p->name, p->value, dwg_type_name (a->obj->fixedtype),
                      a->name);
          return DYNBLOCK_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
        }
      if (delta.x == 0.0 && delta.y == 0.0)
        continue;
      error = a->obj->fixedtype == DWG_TYPE_BLOCKSTRETCHACTION
                  ? plan_stretch (plan, a, &delta)
                  : plan_move (plan, a, &delta);
      if (error)
        return error;
    }
  return 0;
}

/* Whether the evaluator turns an entity of obj's type. Not an ATTDEF: in
   a check, AutoCAD left the ATTDEFs a rotate action selected as they were,
   neither moved nor turned, in the representation block and on the
   placement, so how it turns one is not known. */
static int
is_turnable (const Dwg_Object *obj)
{
  return (is_movable (obj) && obj->fixedtype != DWG_TYPE_ATTDEF)
         || obj->fixedtype == DWG_TYPE_POINT;
}

/* The extrusion of entity obj, of a type the evaluator turns, or NULL for
   a SPLINE, which has none. */
static const BITCODE_BE *
entity_normal (const Dwg_Object *obj)
{
  int planar;
  const BITCODE_BE *ext = entity_ocs (obj, &planar);
  if (ext)
    return ext;
  switch (obj->fixedtype)
    {
    case DWG_TYPE_LINE:
      return &obj->tio.entity->tio.LINE->extrusion;
    case DWG_TYPE_ELLIPSE:
      return &obj->tio.entity->tio.ELLIPSE->extrusion;
    case DWG_TYPE_MTEXT:
      return &obj->tio.entity->tio.MTEXT->extrusion;
    case DWG_TYPE_POINT:
      return &obj->tio.entity->tio.POINT->extrusion;
    default:
      return NULL;
    }
}

/* Refuses a turn of entity obj by rotate action a unless its extrusion is
   along Z, either way, which a turn about Z leaves as it is. An extrusion
   of 0, as the decoder leaves an LWPOLYLINE that stores none, is +Z. */
static int
check_turn_plane (Dynblock_Plan *plan, const Dynblock_Action *a,
                  const Dwg_Object *obj)
{
  const BITCODE_BE *ext = entity_normal (obj);
  if (!ext || (ext->x == 0.0 && ext->y == 0.0))
    return 0;
  set_reason (plan->reason, plan->reason_size,
              "rotate action '%s': turning %s " FORMAT_HV
              ", which is not in the XY plane, is not supported",
              a->name, dwg_type_name (obj->fixedtype), obj->handle.value);
  return DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
}

/* Refuses a turn of HATCH obj by rotate action a unless it is a solid fill
   with no gradient, whose boundary is made of polylines and lines: how
   AutoCAD stores the turn of an arc, elliptical arc or spline segment of a
   boundary, or of a pattern, is not known. */
static int
check_turn_hatch (Dynblock_Plan *plan, const Dynblock_Action *a,
                  const Dwg_Object *obj)
{
  const Dwg_Entity_HATCH *_obj = obj->tio.entity->tio.HATCH;
  if (!_obj->is_solid_fill || _obj->is_gradient_fill)
    {
      set_reason (plan->reason, plan->reason_size,
                  "rotate action '%s': turning HATCH " FORMAT_HV
                  ", which is not a solid fill, is not supported",
                  a->name, obj->handle.value);
      return DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
    }
  for (BITCODE_BL i = 0; _obj->paths && i < _obj->num_paths; i++)
    {
      const Dwg_HATCH_Path *path = &_obj->paths[i];
      if (path->flag & 2)
        continue;
      for (BITCODE_BL j = 0; path->segs && j < path->num_segs_or_paths; j++)
        if (path->segs[j].curve_type != 1)
          {
            set_reason (plan->reason, plan->reason_size,
                        "rotate action '%s': turning HATCH " FORMAT_HV
                        " with a boundary segment of type %u is not "
                        "supported",
                        a->name, obj->handle.value,
                        (unsigned)path->segs[j].curve_type);
            return DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
          }
    }
  return 0;
}

static int
add_turn (Dynblock_Plan *plan, const Dynblock_Action *a,
          const BITCODE_RLL entity, const BITCODE_2RD *center,
          const double angle)
{
  Dynblock_Turn *t;
  if (plan->num_turns == plan->size_turns)
    {
      const BITCODE_BL size = plan->size_turns ? plan->size_turns * 2 : 32;
      Dynblock_Turn *turns = (Dynblock_Turn *)realloc (
          plan->turns, size * sizeof (Dynblock_Turn));
      if (!turns)
        return DWG_ERR_OUTOFMEM;
      plan->turns = turns;
      plan->size_turns = size;
    }
  t = &plan->turns[plan->num_turns++];
  t->entity = entity;
  t->center = *center;
  t->angle = angle;
  t->action = a->name;
  return 0;
}

/* Plans a turn of entity obj, whole, by rotate action a, about center by
   angle, with the ATTRIBs an INSERT owns. */
static int
plan_turn (Dynblock_Plan *plan, const Dynblock_Action *a,
           const Dwg_Object *obj, const BITCODE_2RD *center,
           const double angle)
{
  int error;
  if (!is_turnable (obj))
    {
      set_reason (plan->reason, plan->reason_size,
                  "rotate action '%s': turning %s " FORMAT_HV
                  " is not supported",
                  a->name, dwg_type_name (obj->fixedtype), obj->handle.value);
      return DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
    }
  error = check_turn_plane (plan, a, obj);
  if (!error && obj->fixedtype == DWG_TYPE_HATCH)
    error = check_turn_hatch (plan, a, obj);
  if (!error && obj->fixedtype == DWG_TYPE_INSERT)
    {
      const Dwg_Entity_INSERT *_obj = obj->tio.entity->tio.INSERT;
      for (BITCODE_BL i = 0; !error && _obj->attribs && i < _obj->num_owned;
           i++)
        {
          const Dwg_Object *attrib = ref_object (plan->dwg, _obj->attribs[i]);
          if (attrib && attrib->fixedtype == DWG_TYPE_ATTRIB)
            error = check_turn_plane (plan, a, attrib);
        }
    }
  return error ? error : add_turn (plan, a, obj->handle.value, center, angle);
}

/* Plans rotate action a turning the entities it selects about center by
   angle. The parameters it turns are checked, as check_turned_linear ()
   and check_selected_node () say. */
static int
plan_rotate (Dynblock_Plan *plan, const Dynblock_Action *a,
             const BITCODE_2RD *center, const double angle)
{
  const Dwg_Object_BLOCKROTATEACTION *_a
      = a->obj->tio.object->tio.BLOCKROTATEACTION;
  const BITCODE_BL own = action_parameter (a);
  int error = 0;
  for (BITCODE_BL i = 0; _a->deps && i < _a->num_deps && !error; i++)
    {
      const BITCODE_RLL handle = ref_handle (_a->deps[i]);
      const Dwg_Object *obj;
      // the parameters and grips it turns are nodes of the graph; it lists
      // the parameters by id too
      if (handle && hash_get (plan->node_handles, handle) != HASH_NOT_FOUND)
        {
          error = check_selected_dep (plan, a,
                                      handle_object (plan->dwg, handle));
          continue;
        }
      obj = selected_entity (plan, a, handle, &error);
      if (obj)
        error = plan_turn (plan, a, obj, center, angle);
    }
  for (BITCODE_BL i = 0; !error && _a->actions && i < _a->num_actions; i++)
    {
      const BITCODE_BL nodeid = _a->actions[i];
      // its own parameter's end point turns with the value
      if (nodeid == own)
        continue;
      error = check_turned_linear (plan, a, find_linear (plan, nodeid));
      if (!error)
        error = check_selected_node (plan, a, nodeid);
    }
  return error;
}

/* Whether connection conn names the point name of the parameter with node
   id nodeid. */
static int
is_connection (const Dwg_BLOCKACTION_connectionpts *conn,
               const BITCODE_BL nodeid, const char *name)
{
  return conn->code == nodeid && conn->name && strEQ (conn->name, name);
}

/* Plans the actions connected to rotation parameter r, whose value turns
   its end point about its base point by angle. */
static int
plan_rotation_actions (Dynblock_Plan *plan, const Dynblock_Rotation *r,
                       const double angle)
{
  const Dwg_Object_BLOCKROTATIONPARAMETER *_r
      = r->obj->tio.object->tio.BLOCKROTATIONPARAMETER;
  for (BITCODE_BL i = 0; i < plan->num_actions; i++)
    {
      const Dynblock_Action *a = &plan->actions[i];
      const Dwg_Object_BLOCKROTATEACTION *_a;
      BITCODE_2RD center;
      int error;
      if (!is_connected (a->obj, r->nodeid))
        continue;
      if (a->obj->fixedtype != DWG_TYPE_BLOCKROTATEACTION)
        {
          set_reason (plan->reason, plan->reason_size,
                      "rotation property '%s' drives %s '%s', which is not "
                      "supported",
                      r->name, dwg_type_name (a->obj->fixedtype), a->name);
          return DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
        }
      _a = a->obj->tio.object->tio.BLOCKROTATEACTION;
      if (!is_connection (&_a->conn_pts[0], r->nodeid, "UpdatedBaseX")
          || !is_connection (&_a->conn_pts[1], r->nodeid, "UpdatedBaseY")
          || !is_connection (&_a->conn_pts[2], r->nodeid, "AngleDelta"))
        {
          char *names[3];
          for (int j = 0; j < 3; j++)
            names[j] = utf8_copy (plan->dwg, _a->conn_pts[j].name);
          set_reason (plan->reason, plan->reason_size,
                      "rotate action '%s' takes '%s', '%s' and '%s' of "
                      "rotation property '%s', which is not supported",
                      a->name, names[0] ? names[0] : "",
                      names[1] ? names[1] : "", names[2] ? names[2] : "",
                      r->name);
          for (int j = 0; j < 3; j++)
            free (names[j]);
          return DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
        }
      if (!_a->dependent)
        {
          set_reason (plan->reason, plan->reason_size,
                      "rotate action '%s' has a base point independent of "
                      "its parameter's, which is not supported",
                      a->name);
          return DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
        }
      if (_a->offset.x != 0.0 || _a->offset.y != 0.0)
        {
          set_reason (plan->reason, plan->reason_size,
                      "rotate action '%s' has its base point offset from its "
                      "parameter's, which is not supported",
                      a->name);
          return DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
        }
      // UpdatedBaseX and UpdatedBaseY: the parameter's base point, which
      // no action that runs moves
      center.x = _r->def_basept.x;
      center.y = _r->def_basept.y;
      error = plan_rotate (plan, a, &center, angle);
      if (error)
        return error;
    }
  return 0;
}

static void
free_plan (Dynblock_Plan *plan)
{
  free_hash (plan->entities);
  free_hash (plan->node_handles);
  free ((void *)plan->nodes);
  free (plan->linear);
  free (plan->rotations);
  for (BITCODE_BL i = 0; plan->actions && i < plan->num_actions; i++)
    free (plan->actions[i].name);
  free (plan->actions);
  free (plan->moves);
  free (plan->turns);
}

static int
is_at (const BITCODE_3BD *a, const BITCODE_3BD *b)
{
  return a->x == b->x && a->y == b->y && a->z == b->z;
}

#define REACH(m, v) (m) = fmax ((m), fabs (v))
#define REACH_2D(m, pt)                                                       \
  do                                                                          \
    {                                                                         \
      REACH (m, (pt).x);                                                      \
      REACH (m, (pt).y);                                                      \
    }                                                                         \
  while (0)
#define REACH_3D(m, pt)                                                       \
  do                                                                          \
    {                                                                         \
      REACH_2D (m, pt);                                                       \
      REACH (m, (pt).z);                                                      \
    }                                                                         \
  while (0)

/* The largest magnitude of a coordinate the evaluator moves of entity obj,
   with the ATTRIBs an INSERT owns. */
static double
entity_reach (const Dwg_Data *dwg, const Dwg_Object *obj)
{
  double m = 0.0;
  switch (obj->fixedtype)
    {
    case DWG_TYPE_LINE:
      REACH_3D (m, obj->tio.entity->tio.LINE->start);
      REACH_3D (m, obj->tio.entity->tio.LINE->end);
      break;
    case DWG_TYPE_LWPOLYLINE:
      {
        const Dwg_Entity_LWPOLYLINE *_obj = obj->tio.entity->tio.LWPOLYLINE;
        for (BITCODE_BL i = 0; _obj->points && i < _obj->num_points; i++)
          REACH_2D (m, _obj->points[i]);
      }
      break;
    case DWG_TYPE_ARC:
      REACH_3D (m, obj->tio.entity->tio.ARC->center);
      break;
    case DWG_TYPE_CIRCLE:
      REACH_3D (m, obj->tio.entity->tio.CIRCLE->center);
      break;
    case DWG_TYPE_ELLIPSE:
      REACH_3D (m, obj->tio.entity->tio.ELLIPSE->center);
      break;
    case DWG_TYPE_SPLINE:
      {
        const Dwg_Entity_SPLINE *_obj = obj->tio.entity->tio.SPLINE;
        for (BITCODE_BL i = 0; _obj->ctrl_pts && i < _obj->num_ctrl_pts; i++)
          REACH_3D (m, _obj->ctrl_pts[i]);
        for (BITCODE_BS i = 0; _obj->fit_pts && i < _obj->num_fit_pts; i++)
          REACH_3D (m, _obj->fit_pts[i]);
      }
      break;
    case DWG_TYPE_TEXT:
      REACH_2D (m, obj->tio.entity->tio.TEXT->ins_pt);
      REACH_2D (m, obj->tio.entity->tio.TEXT->alignment_pt);
      break;
    case DWG_TYPE_ATTDEF:
      REACH_2D (m, obj->tio.entity->tio.ATTDEF->ins_pt);
      REACH_2D (m, obj->tio.entity->tio.ATTDEF->alignment_pt);
      break;
    case DWG_TYPE_ATTRIB:
      REACH_2D (m, obj->tio.entity->tio.ATTRIB->ins_pt);
      REACH_2D (m, obj->tio.entity->tio.ATTRIB->alignment_pt);
      break;
    case DWG_TYPE_MTEXT:
      REACH_3D (m, obj->tio.entity->tio.MTEXT->ins_pt);
      break;
    case DWG_TYPE_INSERT:
      {
        const Dwg_Entity_INSERT *_obj = obj->tio.entity->tio.INSERT;
        REACH_3D (m, _obj->ins_pt);
        for (BITCODE_BL i = 0; _obj->attribs && i < _obj->num_owned; i++)
          {
            const Dwg_Object *attrib = ref_object (dwg, _obj->attribs[i]);
            if (attrib && attrib->fixedtype == DWG_TYPE_ATTRIB)
              m = fmax (m, entity_reach (dwg, attrib));
          }
      }
      break;
    case DWG_TYPE_HATCH:
      {
        const Dwg_Entity_HATCH *_obj = obj->tio.entity->tio.HATCH;
        for (BITCODE_BL i = 0; _obj->paths && i < _obj->num_paths; i++)
          {
            const Dwg_HATCH_Path *path = &_obj->paths[i];
            const BITCODE_BL n = path->num_segs_or_paths;
            for (BITCODE_BL j = 0;
                 (path->flag & 2) && path->polyline_paths && j < n; j++)
              REACH_2D (m, path->polyline_paths[j].point);
            for (BITCODE_BL j = 0; !(path->flag & 2) && path->segs && j < n;
                 j++)
              {
                const Dwg_HATCH_PathSeg *s = &path->segs[j];
                REACH_2D (m, s->first_endpoint);
                REACH_2D (m, s->second_endpoint);
                REACH_2D (m, s->center);
                for (BITCODE_BL k = 0;
                     s->control_points && k < s->num_control_points; k++)
                  REACH_2D (m, s->control_points[k].point);
                for (BITCODE_BL k = 0; s->fitpts && k < s->num_fitpts; k++)
                  REACH_2D (m, s->fitpts[k]);
              }
          }
        for (BITCODE_BL i = 0; _obj->seeds && i < _obj->num_seeds; i++)
          REACH_2D (m, _obj->seeds[i]);
        for (BITCODE_BS i = 0; _obj->deflines && i < _obj->num_deflines; i++)
          REACH_2D (m, _obj->deflines[i].pt0);
      }
      break;
    default:
      break;
    }
  return m;
}

static double
move_size (const Dynblock_Move *m)
{
  return fabs (m->delta.x) + fabs (m->delta.y) + fabs (m->delta.z);
}

/* Orders moves by entity, then by stretch point, the whole moves (-1)
   first. */
static int
compare_moves (const void *pa, const void *pb)
{
  const Dynblock_Move *a = (const Dynblock_Move *)pa;
  const Dynblock_Move *b = (const Dynblock_Move *)pb;
  if (a->entity != b->entity)
    return a->entity < b->entity ? -1 : 1;
  return a->point < b->point ? -1 : a->point > b->point ? 1 : 0;
}

/* Refuses moves that could take a coordinate beyond what a double holds:
   an entity's largest coordinate, plus the sum of its whole moves and of
   the moves of the stretch point moved most, must stay finite. */
static int
check_reach (Dynblock_Plan *plan)
{
  const BITCODE_BL n = plan->num_moves;
  Dynblock_Move *sorted;
  int error = 0;
  if (!n)
    return 0;
  sorted = (Dynblock_Move *)malloc (n * sizeof (Dynblock_Move));
  if (!sorted)
    return DWG_ERR_OUTOFMEM;
  memcpy (sorted, plan->moves, n * sizeof (Dynblock_Move));
  qsort (sorted, n, sizeof (Dynblock_Move), compare_moves);
  for (BITCODE_BL i = 0; i < n && !error;)
    {
      const BITCODE_RLL entity = sorted[i].entity;
      const Dwg_Object *obj = handle_object (plan->dwg, entity);
      double whole = 0.0, most = 0.0, reach;
      // the entity's whole moves, then those of each of its stretch points
      while (i < n && sorted[i].entity == entity)
        {
          const int point = sorted[i].point;
          double sum = 0.0;
          for (; i < n && sorted[i].entity == entity
                 && sorted[i].point == point;
               i++)
            sum += move_size (&sorted[i]);
          if (point < 0)
            whole = sum;
          else
            most = fmax (most, sum);
        }
      reach = (obj ? entity_reach (plan->dwg, obj) : 0.0) + whole + most;
      if (!isfinite (reach))
        {
          set_reason (plan->reason, plan->reason_size,
                      "the actions would move %s " FORMAT_HV
                      " beyond what a double holds",
                      obj ? dwg_type_name (obj->fixedtype) : "entity",
                      entity);
          error = DYNBLOCK_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
        }
    }
  free (sorted);
  return error;
}

/* The largest magnitude of a coordinate or vector component the evaluator
   turns of entity obj, with the ATTRIBs an INSERT owns. */
static double
turn_reach (const Dwg_Data *dwg, const Dwg_Object *obj)
{
  double m = entity_reach (dwg, obj);
  switch (obj->fixedtype)
    {
    case DWG_TYPE_ELLIPSE:
      REACH_2D (m, obj->tio.entity->tio.ELLIPSE->sm_axis);
      break;
    case DWG_TYPE_SPLINE:
      REACH_2D (m, obj->tio.entity->tio.SPLINE->beg_tan_vec);
      REACH_2D (m, obj->tio.entity->tio.SPLINE->end_tan_vec);
      break;
    case DWG_TYPE_MTEXT:
      REACH_2D (m, obj->tio.entity->tio.MTEXT->x_axis_dir);
      break;
    case DWG_TYPE_POINT:
      REACH (m, obj->tio.entity->tio.POINT->x);
      REACH (m, obj->tio.entity->tio.POINT->y);
      break;
    default:
      break;
    }
  return m;
}

/* Refuses an entity that a rotate action turns and another action that
   runs moves or turns too, as the order AutoCAD runs them in is not known;
   and a turn that could take a coordinate beyond what a double holds:
   turned about c, a point p, or a vector, stays within 3 |c| + 2 |p| in
   each coordinate. */
static int
check_turns (Dynblock_Plan *plan)
{
  dwg_inthash *turned;
  int error = 0;
  if (!plan->num_turns)
    return 0;
  turned = hash_new ((uint64_t)plan->num_turns * 2 + 8);
  if (!turned)
    return DWG_ERR_OUTOFMEM;
  for (BITCODE_BL i = 0; i < plan->num_turns && !error; i++)
    {
      const Dynblock_Turn *t = &plan->turns[i];
      const Dwg_Object *obj = handle_object (plan->dwg, t->entity);
      const uint64_t first = hash_get (turned, t->entity);
      const double center = fmax (fabs (t->center.x), fabs (t->center.y));
      if (first != HASH_NOT_FOUND)
        {
          set_reason (plan->reason, plan->reason_size,
                      "%s " FORMAT_HV " is turned by rotate actions '%s' and "
                      "'%s'; the order AutoCAD runs them in is not known, "
                      "which is not supported",
                      obj ? dwg_type_name (obj->fixedtype) : "entity",
                      t->entity, plan->turns[first].action, t->action);
          error = DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
        }
      else if (!isfinite (3.0 * center
                          + 2.0 * (obj ? turn_reach (plan->dwg, obj) : 0.0)))
        {
          set_reason (plan->reason, plan->reason_size,
                      "rotate action '%s' would turn %s " FORMAT_HV
                      " beyond what a double holds",
                      t->action,
                      obj ? dwg_type_name (obj->fixedtype) : "entity",
                      t->entity);
          error = DYNBLOCK_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
        }
      hash_set (turned, t->entity, i);
    }
  for (BITCODE_BL i = 0; i < plan->num_moves && !error; i++)
    {
      const Dynblock_Move *m = &plan->moves[i];
      const uint64_t t = hash_get (turned, m->entity);
      const Dwg_Object *obj;
      if (t == HASH_NOT_FOUND)
        continue;
      obj = handle_object (plan->dwg, m->entity);
      set_reason (plan->reason, plan->reason_size,
                  "%s " FORMAT_HV " is turned by rotate action '%s' and "
                  "moved by action '%s'; the order AutoCAD runs them in is "
                  "not known, which is not supported",
                  obj ? dwg_type_name (obj->fixedtype) : "entity", m->entity,
                  plan->turns[t].action, m->action);
      error = DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
    }
  free_hash (turned);
  return error;
}

/* Plans the LINEAR and ROTATION values of list, given in value (a NaN
   where not given), for definition _def, whose evaluation graph is graph,
   into plan->moves and plan->turns. */
static int
plan_values (Dynblock_Plan *plan, const Dwg_Object_BLOCK_HEADER *_def,
             const Dwg_Object *graph, const Param_List *list,
             const double *value)
{
  const Dwg_Object_EVALUATION_GRAPH *_graph
      = graph->tio.object->tio.EVALUATION_GRAPH;
  int error = 0, next;

  plan->entities = hash_new (_def->num_owned * 2 + 8);
  plan->node_handles = hash_new (_graph->num_nodes * 2 + 8);
  plan->nodes = (const Dwg_Object **)calloc (_graph->num_nodes + 1,
                                             sizeof (Dwg_Object *));
  plan->linear
      = (Dynblock_Linear *)calloc (list->count + 1, sizeof (Dynblock_Linear));
  plan->rotations = (Dynblock_Rotation *)calloc (list->count + 1,
                                                 sizeof (Dynblock_Rotation));
  plan->actions = (Dynblock_Action *)calloc (_graph->num_nodes + 1,
                                             sizeof (Dynblock_Action));
  if (!plan->entities || !plan->node_handles || !plan->nodes || !plan->linear
      || !plan->rotations || !plan->actions)
    return DWG_ERR_OUTOFMEM;
  for (BITCODE_BL i = 0; _def->entities && i < _def->num_owned; i++)
    if (ref_handle (_def->entities[i]))
      hash_set (plan->entities, ref_handle (_def->entities[i]), 1);
  for (BITCODE_BL i = 0; _graph->nodes && i < _graph->num_nodes; i++)
    {
      const Dwg_Object *obj
          = ref_object (plan->dwg, _graph->nodes[i].evalexpr);
      if (obj && is_graph_element (obj))
        {
          plan->nodes[plan->num_nodes++] = obj;
          hash_set (plan->node_handles, obj->handle.value, 1);
        }
    }
  for (BITCODE_BL i = 0; i < list->count; i++)
    {
      const Dwg_DynParam *p = &list->params[i];
      const Dwg_Object *obj = handle_object (plan->dwg, p->handle);
      if (p->kind == DWG_DYNPARAM_LINEAR)
        {
          const Dwg_Object_BLOCKLINEARPARAMETER *_obj
              = obj->tio.object->tio.BLOCKLINEARPARAMETER;
          Dynblock_Linear *l = &plan->linear[plan->num_linear++];
          l->obj = obj;
          l->name = p->name;
          l->nodeid = p->nodeid;
          l->base = _obj->def_basept;
          l->end = _obj->def_endpt;
          l->given = !isnan (value[i]);
          l->value = value[i];
        }
      else if (p->kind == DWG_DYNPARAM_ROTATION)
        {
          Dynblock_Rotation *r = &plan->rotations[plan->num_rotations++];
          r->obj = obj;
          r->name = p->name;
          r->nodeid = p->nodeid;
          r->given = !isnan (value[i]);
          r->value = r->given ? normal_angle (value[i]) : value[i];
        }
    }
  for (BITCODE_BL i = 0; _graph->nodes && i < _graph->num_nodes; i++)
    {
      const Dwg_Object *obj
          = ref_object (plan->dwg, _graph->nodes[i].evalexpr);
      BITCODE_BL n;
      const Dwg_BLOCKACTION_connectionpts *conn;
      Dynblock_Action *a;
      if (!obj || !(conn = action_connections (obj, &n)))
        continue;
      a = &plan->actions[plan->num_actions++];
      a->obj = obj;
      // every action starts with BLOCKACTION_fields, and so with its name
      a->name
          = utf8_copy (plan->dwg, obj->tio.object->tio.BLOCKMOVEACTION->name);
      if (!a->name)
        return DWG_ERR_OUTOFMEM;
      a->linear = -1;
      for (BITCODE_BL j = 0; j < n && a->linear < 0; j++)
        a->linear = find_linear (plan, conn[j].code);
    }
  mark_running (plan);

  while (!error && (next = next_linear (plan)) >= 0)
    {
      Dynblock_Linear *p = &plan->linear[next];
      const Dwg_Object_BLOCKLINEARPARAMETER *_p
          = p->obj->tio.object->tio.BLOCKLINEARPARAMETER;
      const double length = distance_3d (&p->base, &p->end);
      BITCODE_3BD end, end_delta;
      p->done = 1;
      // Moved by another property's action, it would trigger its own
      // actions too (AutoCAD's chain actions).
      if (_p->chain_actions
          && (!is_at (&p->base, &_p->def_basept)
              || !is_at (&p->end, &_p->def_endpt)))
        {
          set_reason (plan->reason, plan->reason_size,
                      "linear property '%s' chains its actions to another "
                      "property's, which is not supported",
                      p->name);
          return DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
        }
      if (!p->given || fabs (p->value - length) <= DYNBLOCK_VALUE_EPSILON)
        continue;
      if (length <= DYNBLOCK_VALUE_EPSILON)
        {
          set_reason (plan->reason, plan->reason_size,
                      "linear property '%s' has no length to set", p->name);
          return DYNBLOCK_REFUSED;
        }
      if (!isfinite (length))
        {
          set_reason (plan->reason, plan->reason_size,
                      "linear property '%s' is longer than a double holds",
                      p->name);
          return DYNBLOCK_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
        }
      if (_p->parameter_base_location != 0)
        {
          set_reason (plan->reason, plan->reason_size,
                      "linear property '%s' has its base point in its "
                      "middle, which is not supported",
                      p->name);
          return DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
        }
      // the direction first, as value / length can overflow
      end.x = p->base.x + (p->end.x - p->base.x) / length * p->value;
      end.y = p->base.y + (p->end.y - p->base.y) / length * p->value;
      end.z = p->base.z + (p->end.z - p->base.z) / length * p->value;
      end_delta.x = end.x - p->end.x;
      end_delta.y = end.y - p->end.y;
      end_delta.z = end.z - p->end.z;
      p->end = end;
      error = plan_actions_of (plan, p, &end_delta);
    }
  if (!error && next == -2)
    {
      // which of them AutoCAD evaluates first is not known
      const Dynblock_Linear *p = NULL;
      for (BITCODE_BL i = 0; !p && i < plan->num_linear; i++)
        if (plan->linear[i].running && !plan->linear[i].done)
          p = &plan->linear[i];
      set_reason (plan->reason, plan->reason_size,
                  "linear properties whose actions run, such as '%s', wait "
                  "on each other: their actions move each other's "
                  "parameters in a cycle, which is not supported",
                  p ? p->name : "");
      return DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
    }
  // No action that ran moved a rotation parameter given a value, so each
  // turns from the definition's angle.
  for (BITCODE_BL i = 0; !error && i < plan->num_rotations; i++)
    {
      const Dynblock_Rotation *r = &plan->rotations[i];
      const Dwg_Object_BLOCKROTATIONPARAMETER *_r
          = r->obj->tio.object->tio.BLOCKROTATIONPARAMETER;
      if (r->running)
        error = plan_rotation_actions (
            plan, r,
            r->value
                - rotation_angle (&_r->def_basept, &_r->def_endpt,
                                  &_r->def_base_angle_pt));
    }
  if (!error)
    error = check_turns (plan);
  return error ? error : check_reach (plan);
}

/* Moves pt, a 2D point of any of the 2D point types, by the x and y of
   delta. */
#define MOVE_2D(pt, delta)                                                    \
  do                                                                          \
    {                                                                         \
      (pt).x += (delta)->x;                                                   \
      (pt).y += (delta)->y;                                                   \
    }                                                                         \
  while (0)

static void
move_3d (BITCODE_3BD *pt, const BITCODE_3BD *delta)
{
  pt->x += delta->x;
  pt->y += delta->y;
  pt->z += delta->z;
}

/* Moves the boundary, seed points and pattern of HATCH _obj by delta, in
   its OCS. */
static void
move_hatch (Dwg_Entity_HATCH *_obj, const BITCODE_3BD *delta)
{
  for (BITCODE_BL i = 0; _obj->paths && i < _obj->num_paths; i++)
    {
      Dwg_HATCH_Path *path = &_obj->paths[i];
      if (path->flag & 2)
        {
          for (BITCODE_BL j = 0;
               path->polyline_paths && j < path->num_segs_or_paths; j++)
            MOVE_2D (path->polyline_paths[j].point, delta);
          continue;
        }
      for (BITCODE_BL j = 0; path->segs && j < path->num_segs_or_paths; j++)
        {
          Dwg_HATCH_PathSeg *s = &path->segs[j];
          switch (s->curve_type)
            {
            case 1:
              MOVE_2D (s->first_endpoint, delta);
              MOVE_2D (s->second_endpoint, delta);
              break;
            case 2:
            case 3:
              // an ellipse's endpoint is its major axis, a vector
              MOVE_2D (s->center, delta);
              break;
            case 4:
              for (BITCODE_BL k = 0;
                   s->control_points && k < s->num_control_points; k++)
                MOVE_2D (s->control_points[k].point, delta);
              for (BITCODE_BL k = 0; s->fitpts && k < s->num_fitpts; k++)
                MOVE_2D (s->fitpts[k], delta);
              break;
            default:
              break;
            }
        }
    }
  for (BITCODE_BL i = 0; _obj->seeds && i < _obj->num_seeds; i++)
    MOVE_2D (_obj->seeds[i], delta);
  for (BITCODE_BS i = 0; _obj->deflines && i < _obj->num_deflines; i++)
    {
      MOVE_2D (_obj->deflines[i].pt0, delta);
    }
}

/* Sets the dataflags bit 0x02 of a TEXT, ATTDEF or ATTRIB, which leaves out
   an alignment point of (0,0), as its alignment point is. The encoder
   writes the flags as they are unless it converts the drawing. */
#define SET_ALIGNMENT_FLAG(_obj)                                              \
  do                                                                          \
    {                                                                         \
      if ((_obj)->alignment_pt.x == 0.0 && (_obj)->alignment_pt.y == 0.0)     \
        (_obj)->dataflags |= 0x02;                                            \
      else                                                                    \
        (_obj)->dataflags &= ~0x02;                                           \
    }                                                                         \
  while (0)

/* Moves a TEXT, ATTDEF or ATTRIB by delta, in its OCS: its insertion
   point, and the alignment point of text that isn't left-aligned on the
   baseline, also when that is (0,0) and not stored. Left-aligned text
   doesn't use its alignment point, and AutoCAD keeps it (0,0). */
#define MOVE_TEXT(_obj, delta)                                                \
  do                                                                          \
    {                                                                         \
      MOVE_2D ((_obj)->ins_pt, delta);                                        \
      if ((_obj)->horiz_alignment || (_obj)->vert_alignment)                  \
        {                                                                     \
          MOVE_2D ((_obj)->alignment_pt, delta);                              \
          SET_ALIGNMENT_FLAG (_obj);                                          \
        }                                                                     \
    }                                                                         \
  while (0)

/* Moves copy obj whole by wcs_delta, and the ATTRIBs an INSERT owns. */
static void
move_whole (Dwg_Data *dwg, Dwg_Object *obj, const BITCODE_3BD *wcs_delta)
{
  BITCODE_3BD delta;
  entity_delta (obj, wcs_delta, &delta);
  switch (obj->fixedtype)
    {
    case DWG_TYPE_LINE:
      move_3d (&obj->tio.entity->tio.LINE->start, &delta);
      move_3d (&obj->tio.entity->tio.LINE->end, &delta);
      break;
    case DWG_TYPE_LWPOLYLINE:
      {
        Dwg_Entity_LWPOLYLINE *_obj = obj->tio.entity->tio.LWPOLYLINE;
        for (BITCODE_BL i = 0; _obj->points && i < _obj->num_points; i++)
          MOVE_2D (_obj->points[i], &delta);
      }
      break;
    case DWG_TYPE_ARC:
      move_3d (&obj->tio.entity->tio.ARC->center, &delta);
      break;
    case DWG_TYPE_CIRCLE:
      move_3d (&obj->tio.entity->tio.CIRCLE->center, &delta);
      break;
    case DWG_TYPE_ELLIPSE:
      move_3d (&obj->tio.entity->tio.ELLIPSE->center, &delta);
      break;
    case DWG_TYPE_SPLINE:
      {
        Dwg_Entity_SPLINE *_obj = obj->tio.entity->tio.SPLINE;
        for (BITCODE_BL i = 0; _obj->ctrl_pts && i < _obj->num_ctrl_pts; i++)
          {
            _obj->ctrl_pts[i].x += delta.x;
            _obj->ctrl_pts[i].y += delta.y;
            _obj->ctrl_pts[i].z += delta.z;
          }
        for (BITCODE_BS i = 0; _obj->fit_pts && i < _obj->num_fit_pts; i++)
          move_3d (&_obj->fit_pts[i], &delta);
      }
      break;
    case DWG_TYPE_TEXT:
      MOVE_TEXT (obj->tio.entity->tio.TEXT, &delta);
      break;
    case DWG_TYPE_ATTDEF:
      MOVE_TEXT (obj->tio.entity->tio.ATTDEF, &delta);
      break;
    case DWG_TYPE_ATTRIB:
      MOVE_TEXT (obj->tio.entity->tio.ATTRIB, &delta);
      break;
    case DWG_TYPE_MTEXT:
      move_3d (&obj->tio.entity->tio.MTEXT->ins_pt, &delta);
      break;
    case DWG_TYPE_INSERT:
      {
        Dwg_Entity_INSERT *_obj = obj->tio.entity->tio.INSERT;
        move_3d (&_obj->ins_pt, &delta);
        for (BITCODE_BL i = 0; _obj->attribs && i < _obj->num_owned; i++)
          {
            Dwg_Object *attrib = ref_object (dwg, _obj->attribs[i]);
            if (attrib && attrib->fixedtype == DWG_TYPE_ATTRIB)
              move_whole (dwg, attrib, wcs_delta);
          }
      }
      break;
    case DWG_TYPE_HATCH:
      move_hatch (obj->tio.entity->tio.HATCH, &delta);
      break;
    default:
      break;
    }
}

/* Moves stretch point point of copy obj, a LINE or LWPOLYLINE, by
   wcs_delta. */
static void
move_point (Dwg_Object *obj, const int point, const BITCODE_3BD *wcs_delta)
{
  BITCODE_3BD delta;
  entity_delta (obj, wcs_delta, &delta);
  if (obj->fixedtype == DWG_TYPE_LINE)
    move_3d (point ? &obj->tio.entity->tio.LINE->end
                   : &obj->tio.entity->tio.LINE->start,
             &delta);
  else if (obj->fixedtype == DWG_TYPE_LWPOLYLINE)
    MOVE_2D (obj->tio.entity->tio.LWPOLYLINE->points[point], &delta);
}

/* Applies the planned moves to the copies of the definition's entities. */
static void
apply_moves (Dwg_Data *dwg, const Dynblock_Plan *plan, const Dwg_Copy_Map *map)
{
  for (BITCODE_BL i = 0; i < plan->num_moves; i++)
    {
      const Dynblock_Move *m = &plan->moves[i];
      Dwg_Object *copy
          = handle_object (dwg, dwg_copy_map_get (map, m->entity));
      if (!copy)
        continue;
      if (m->point < 0)
        move_whole (dwg, copy, &m->delta);
      else
        move_point (copy, m->point, &m->delta);
    }
}

/* A turn in an entity's own coordinates: about cx, cy, counter-clockwise
   by angle, whose cosine and sine are c and s. */
typedef struct _dynblock_frame
{
  double cx, cy, angle, c, s;
} Dynblock_Frame;

/* Turns the point x, y by frame f. */
static void
turn_xy (double *x, double *y, const Dynblock_Frame *f)
{
  const double dx = *x - f->cx, dy = *y - f->cy;
  *x = f->cx + dx * f->c - dy * f->s;
  *y = f->cy + dx * f->s + dy * f->c;
}

/* Turns the vector x, y by frame f's angle. */
static void
turn_vector (double *x, double *y, const Dynblock_Frame *f)
{
  const double vx = *x, vy = *y;
  *x = vx * f->c - vy * f->s;
  *y = vx * f->s + vy * f->c;
}

#define TURN_2D(pt, f) turn_xy (&(pt).x, &(pt).y, f)
#define TURN_VECTOR(v, f) turn_vector (&(v).x, &(v).y, f)

/* Turns a TEXT or ATTRIB by frame f, in its OCS: its insertion point, the
   alignment point of text that isn't left-aligned on the baseline, as
   MOVE_TEXT moves it, and its rotation, which the encoder of an r2004
   drawing stores only when dataflags says so. */
#define TURN_TEXT(_obj, f)                                                    \
  do                                                                          \
    {                                                                         \
      TURN_2D ((_obj)->ins_pt, f);                                            \
      if ((_obj)->horiz_alignment || (_obj)->vert_alignment)                  \
        {                                                                     \
          TURN_2D ((_obj)->alignment_pt, f);                                  \
          SET_ALIGNMENT_FLAG (_obj);                                          \
        }                                                                     \
      (_obj)->rotation = normal_angle ((_obj)->rotation + (f)->angle);        \
      if ((_obj)->rotation != 0.0)                                            \
        (_obj)->dataflags &= ~0x08;                                           \
      else                                                                    \
        (_obj)->dataflags |= 0x08;                                            \
    }                                                                         \
  while (0)

/* Turns HATCH _obj by frame f, in its OCS: its boundary, of polylines and
   lines as check_turn_hatch () checks, and its seed points. A solid fill
   has no pattern to turn. */
static void
turn_hatch (Dwg_Entity_HATCH *_obj, const Dynblock_Frame *f)
{
  for (BITCODE_BL i = 0; _obj->paths && i < _obj->num_paths; i++)
    {
      Dwg_HATCH_Path *path = &_obj->paths[i];
      if (path->flag & 2)
        {
          for (BITCODE_BL j = 0;
               path->polyline_paths && j < path->num_segs_or_paths; j++)
            TURN_2D (path->polyline_paths[j].point, f);
          continue;
        }
      for (BITCODE_BL j = 0; path->segs && j < path->num_segs_or_paths; j++)
        {
          TURN_2D (path->segs[j].first_endpoint, f);
          TURN_2D (path->segs[j].second_endpoint, f);
        }
    }
  for (BITCODE_BL i = 0; _obj->seeds && i < _obj->num_seeds; i++)
    TURN_2D (_obj->seeds[i], f);
}

/* Turns copy obj whole about the block's point center, counter-clockwise
   by angle, and the ATTRIBs an INSERT owns. An entity with OCS points
   whose extrusion is -Z turns the other way in its OCS, whose X axis is
   the block's -X. */
static void
turn_whole (Dwg_Data *dwg, Dwg_Object *obj, const BITCODE_2RD *center,
            const double angle)
{
  int planar;
  const BITCODE_BE *ocs = entity_ocs (obj, &planar);
  const BITCODE_BE *normal = entity_normal (obj);
  const double sign = ocs && ocs->z < 0.0 ? -1.0 : 1.0;
  Dynblock_Frame f;
  f.cx = sign * center->x;
  f.cy = center->y;
  f.angle = sign * angle;
  f.c = cos (f.angle);
  f.s = sin (f.angle);
  switch (obj->fixedtype)
    {
    case DWG_TYPE_LINE:
      TURN_2D (obj->tio.entity->tio.LINE->start, &f);
      TURN_2D (obj->tio.entity->tio.LINE->end, &f);
      break;
    case DWG_TYPE_LWPOLYLINE:
      {
        Dwg_Entity_LWPOLYLINE *_obj = obj->tio.entity->tio.LWPOLYLINE;
        for (BITCODE_BL i = 0; _obj->points && i < _obj->num_points; i++)
          TURN_2D (_obj->points[i], &f);
      }
      break;
    case DWG_TYPE_ARC:
      {
        Dwg_Entity_ARC *_obj = obj->tio.entity->tio.ARC;
        TURN_2D (_obj->center, &f);
        _obj->start_angle = normal_angle (_obj->start_angle + f.angle);
        _obj->end_angle = normal_angle (_obj->end_angle + f.angle);
      }
      break;
    case DWG_TYPE_CIRCLE:
      TURN_2D (obj->tio.entity->tio.CIRCLE->center, &f);
      break;
    case DWG_TYPE_ELLIPSE:
      TURN_2D (obj->tio.entity->tio.ELLIPSE->center, &f);
      TURN_VECTOR (obj->tio.entity->tio.ELLIPSE->sm_axis, &f);
      break;
    case DWG_TYPE_SPLINE:
      {
        Dwg_Entity_SPLINE *_obj = obj->tio.entity->tio.SPLINE;
        for (BITCODE_BL i = 0; _obj->ctrl_pts && i < _obj->num_ctrl_pts; i++)
          turn_xy (&_obj->ctrl_pts[i].x, &_obj->ctrl_pts[i].y, &f);
        for (BITCODE_BS i = 0; _obj->fit_pts && i < _obj->num_fit_pts; i++)
          TURN_2D (_obj->fit_pts[i], &f);
        TURN_VECTOR (_obj->beg_tan_vec, &f);
        TURN_VECTOR (_obj->end_tan_vec, &f);
      }
      break;
    case DWG_TYPE_TEXT:
      TURN_TEXT (obj->tio.entity->tio.TEXT, &f);
      break;
    case DWG_TYPE_ATTRIB:
      TURN_TEXT (obj->tio.entity->tio.ATTRIB, &f);
      break;
    case DWG_TYPE_MTEXT:
      TURN_2D (obj->tio.entity->tio.MTEXT->ins_pt, &f);
      TURN_VECTOR (obj->tio.entity->tio.MTEXT->x_axis_dir, &f);
      break;
    case DWG_TYPE_INSERT:
      {
        Dwg_Entity_INSERT *_obj = obj->tio.entity->tio.INSERT;
        TURN_2D (_obj->ins_pt, &f);
        _obj->rotation = normal_angle (_obj->rotation + f.angle);
        for (BITCODE_BL i = 0; _obj->attribs && i < _obj->num_owned; i++)
          {
            Dwg_Object *attrib = ref_object (dwg, _obj->attribs[i]);
            if (attrib && attrib->fixedtype == DWG_TYPE_ATTRIB)
              turn_whole (dwg, attrib, center, angle);
          }
      }
      break;
    case DWG_TYPE_HATCH:
      turn_hatch (obj->tio.entity->tio.HATCH, &f);
      break;
    case DWG_TYPE_POINT:
      {
        // its position is in the WCS, its X axis angle in its own plane
        Dwg_Entity_POINT *_obj = obj->tio.entity->tio.POINT;
        turn_xy (&_obj->x, &_obj->y, &f);
        _obj->x_ang = normal_angle (
            _obj->x_ang + (normal && normal->z < 0.0 ? -angle : angle));
      }
      break;
    default:
      break;
    }
}

/* Applies the planned turns to the copies of the definition's entities. */
static void
apply_turns (Dwg_Data *dwg, const Dynblock_Plan *plan, const Dwg_Copy_Map *map)
{
  for (BITCODE_BL i = 0; i < plan->num_turns; i++)
    {
      const Dynblock_Turn *t = &plan->turns[i];
      Dwg_Object *copy
          = handle_object (dwg, dwg_copy_map_get (map, t->entity));
      if (copy)
        turn_whole (dwg, copy, &t->center, t->angle);
    }
}

/* The evaluation itself, as dwg_dynblock_evaluate (), into *rep_handlep. */
static int
evaluate (Dwg_Data *dwg, const BITCODE_RLL def_handle,
          const Dwg_DynValue *values, const BITCODE_BL num_values,
          BITCODE_RLL *rep_handlep, char *reason, const size_t reason_size)
{
  Param_List list;
  BITCODE_BL *state = NULL;
  double *number = NULL;
  BITCODE_RLL *handles = NULL;
  BITCODE_BL num_handles = 0;
  Dwg_Object_BLOCK_HEADER *_def, *_rep;
  Dwg_Copy_Map *map = NULL;
  Dwg_Copy_Unresolved *unresolved = NULL;
  BITCODE_BL num_unresolved = 0;
  dwg_inthash *copies = NULL;
  Dynblock_Plan plan;
  int parametric, error, copy_error = 0;
  *rep_handlep = 0;
  memset (&plan, 0, sizeof (plan));

  error = definition_params (dwg, def_handle, &list, &parametric, reason,
                             reason_size);
  if (error)
    return error;
  if (parametric)
    {
      set_reason (reason, reason_size,
                  "constraint-based (parametric) blocks are not supported");
      error = DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
      goto done;
    }
  if (list.unknown_class)
    {
      set_reason (reason, reason_size,
                  "the evaluation graph has a node of class %s, which "
                  "LibreDWG can't decode",
                  list.unknown_class);
      error = DYNBLOCK_REFUSED | DWG_ERR_UNHANDLEDCLASS;
      goto done;
    }
  // the visibility state chosen for each property, + 1, or 0; and the
  // LINEAR or ROTATION value, or a NaN
  state = (BITCODE_BL *)calloc (list.count + 1, sizeof (BITCODE_BL));
  number = (double *)malloc ((list.count + 1) * sizeof (double));
  if (!state || !number)
    {
      error = DWG_ERR_OUTOFMEM;
      goto done;
    }
  for (BITCODE_BL j = 0; j < list.count; j++)
    number[j] = nan ("");
  for (BITCODE_BL i = 0; i < num_values; i++)
    {
      const Dwg_DynValue *v = &values[i];
      const Dwg_DynParam *p = NULL;
      BITCODE_BL j;
      for (j = 0; v->name && j < list.count; j++)
        if (strEQ (list.params[j].name, v->name))
          {
            p = &list.params[j];
            break;
          }
      if (!p)
        {
          set_reason (reason, reason_size, "no property is named '%s'",
                      v->name ? v->name : "(NULL)");
          error = DYNBLOCK_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
          goto done;
        }
      for (BITCODE_BL k = 0; k < i; k++)
        if (values[k].name && strEQ (values[k].name, v->name))
          {
            set_reason (reason, reason_size,
                        "property '%s' is given twice", v->name);
            error = DYNBLOCK_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
            goto done;
          }
      if (p->kind != DWG_DYNPARAM_VISIBILITY && !isfinite (v->value))
        {
          set_reason (reason, reason_size, "property '%s' is given %g",
                      p->name, v->value);
          error = DYNBLOCK_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
          goto done;
        }
      switch (p->kind)
        {
        case DWG_DYNPARAM_VISIBILITY:
          {
            BITCODE_BL s;
            for (s = 0; v->state && s < p->num_states; s++)
              if (strEQ (p->states[s], v->state))
                break;
            if (!v->state || s == p->num_states)
              {
                set_reason (reason, reason_size,
                            "visibility property '%s' has no state '%s'",
                            p->name, v->state ? v->state : "(NULL)");
                error = DYNBLOCK_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
                goto done;
              }
            state[j] = s + 1;
          }
          break;
        case DWG_DYNPARAM_FLIP:
          if (v->value != 0.0 && v->value != 1.0)
            {
              set_reason (reason, reason_size,
                          "flip property '%s' is 0 or 1, not %g", p->name,
                          v->value);
              error = DYNBLOCK_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
              goto done;
            }
          if (v->value != 0.0)
            {
              set_reason (reason, reason_size,
                          "flip property '%s': flipping is not supported",
                          p->name);
              error = DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
              goto done;
            }
          break;
        case DWG_DYNPARAM_LINEAR:
          if (v->value < 0.0)
            {
              set_reason (reason, reason_size,
                          "linear property '%s' is given %g, below 0",
                          p->name, v->value);
              error = DYNBLOCK_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
              goto done;
            }
          number[j] = v->value;
          break;
        case DWG_DYNPARAM_ROTATION:
          number[j] = v->value;
          break;
        default:
          set_reason (reason, reason_size,
                      "%s property '%s': setting it is not supported",
                      dwg_dynparam_kind_name (p->kind), p->name);
          error = DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
          goto done;
        }
    }

  _def = handle_object (dwg, def_handle)->tio.object->tio.BLOCK_HEADER;
  error = definition_entities (dwg, _def, &handles, &num_handles, reason,
                               reason_size);
  if (error)
    goto done;
  plan.dwg = dwg;
  plan.reason = reason;
  plan.reason_size = reason_size;
  error = plan_values (&plan, _def,
                       definition_graph (dwg, handle_object (dwg, def_handle)),
                       &list, number);
  if (error)
    goto done;
  // The block table's count is 16-bit, and the add API would wrap it. As
  // dynblock_copy.c's add_to_table (), checked before anything is added.
  {
    const Dwg_Object *ctrl
        = dwg_get_first_object (dwg, DWG_TYPE_BLOCK_CONTROL);
    if (ctrl && ctrl->tio.object->tio.BLOCK_CONTROL->num_entries == 0xFFFF)
      {
        set_reason (reason, reason_size,
                    "the block table is full: it lists 65535 blocks");
        error = DYNBLOCK_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
        goto done;
      }
  }
  map = dwg_copy_map_new ();
  copies = hash_new (num_handles * 2 + 8);
  if (!map || !copies)
    {
      error = DWG_ERR_OUTOFMEM;
      goto done;
    }

  // The copies first, so that a refused copy adds nothing, then the block
  // that owns them. Adding objects may move dwg->object, so objects are
  // found by handle after each step.
  error = dwg_copy_objects (dwg, dwg, handles, num_handles, map, &unresolved,
                            &num_unresolved);
  free (unresolved);
  if (error >= DWG_ERR_CRITICAL)
    {
      set_reason (reason, reason_size, "copying the entities failed (0x%x)",
                  (unsigned)error);
      goto done;
    }
  // below DWG_ERR_CRITICAL: data the copy left out, returned at the end
  copy_error = error;
  error = 0;
  _rep = dwg_add_BLOCK_HEADER (dwg, "*U");
  if (!_rep || !dwg_add_BLOCK (_rep, "*U"))
    {
      set_reason (reason, reason_size, "could not add the block");
      error = DWG_ERR_OUTOFMEM;
      goto done;
    }
  _rep->anonymous = 1;
  _rep->flag |= 1;
  // the add API sets these only for an ATTDEF it adds itself, and sets
  // only hasattrs of the two
  _rep->hasattrs = _def->hasattrs || (_def->flag & 2);
  if (_rep->hasattrs)
    _rep->flag |= 2;
  _rep->base_pt = _def->base_pt;
  *rep_handlep = block_header_object (_rep)->handle.value;
  for (BITCODE_BL i = 0; i < num_handles; i++)
    if (dwg_copy_map_get (map, handles[i]))
      hash_set (copies, dwg_copy_map_get (map, handles[i]), 1);
  for (BITCODE_BL i = 0; i < num_handles; i++)
    {
      Dwg_Object *copy
          = handle_object (dwg, dwg_copy_map_get (map, handles[i]));
      if (copy && copy->supertype == DWG_SUPERTYPE_ENTITY)
        detach_copy (copy, copies);
    }
  // def's own entities join the block; the ATTRIBs, VERTEXes and SEQENDs
  // they own keep their copied owner
  for (BITCODE_BL i = 0; _def->entities && i < _def->num_owned; i++)
    {
      Dwg_Object *copy = handle_object (
          dwg, dwg_copy_map_get (map, ref_handle (_def->entities[i])));
      if (copy)
        dwg_insert_entity (_rep, copy);
    }
  if (!dwg_add_ENDBLK (_rep))
    {
      set_reason (reason, reason_size, "could not add the block's end");
      error = DWG_ERR_OUTOFMEM;
      goto done;
    }

  apply_moves (dwg, &plan, map);
  apply_turns (dwg, &plan, map);
  for (BITCODE_BL j = 0; j < list.count; j++)
    if (state[j])
      {
        error = apply_visibility (
            dwg, handle_object (dwg, list.params[j].handle), state[j] - 1,
            map);
        if (error)
          goto done;
      }

done:
  if (!error && copy_error)
    {
      error = copy_error;
      set_reason (reason, reason_size,
                  "the copy left out data it could not copy (0x%x)",
                  (unsigned)copy_error);
    }
  if (error && !reason[0])
    set_reason (reason, reason_size, "error 0x%x", (unsigned)error);
  free_hash (copies);
  dwg_copy_map_free (map);
  free_plan (&plan);
  free (handles);
  free (state);
  free (number);
  dwg_dynblock_free_params (list.params, list.count);
  return error;
}

EXPORT Dwg_Object_BLOCK_HEADER *
dwg_dynblock_evaluate (Dwg_Object_BLOCK_HEADER *def,
                       const Dwg_DynValue *values, const BITCODE_BL num_values,
                       int *errorp)
{
  const Dwg_Object *obj = block_header_object (def);
  Dwg_Data *dwg;
  BITCODE_RLL rep_handle;
  char reason[REASON_SIZE] = "";
  int error;
  if (errorp)
    *errorp = 0;
  if (!obj || (num_values && !values))
    {
      if (errorp)
        *errorp = DWG_ERR_INTERNALERROR;
      return NULL;
    }
  dwg = obj->parent;
  loglevel = dwg->opts & DWG_OPTS_LOGLEVEL;
  error = evaluate (dwg, obj->handle.value, values, num_values, &rep_handle,
                    reason, sizeof (reason));
  if (errorp)
    *errorp = error;
  if (error >= DWG_ERR_CRITICAL)
    {
      LOG_ERROR ("dwg_dynblock_evaluate: %s", reason);
      return NULL;
    }
  if (error)
    LOG_WARN ("dwg_dynblock_evaluate: %s", reason);
  return handle_object (dwg, rep_handle)->tio.object->tio.BLOCK_HEADER;
}

/*--------------------------------------------------------------------------------
 * The check against AutoCAD's representation blocks
 */

typedef struct _dynblock_compare
{
  const Dwg_Data *dwg;
  double tolerance;
  /* entity handle to AcDbBlockRepETag index, in AutoCAD's block (a) and
     in the evaluated block (b) */
  dwg_inthash *index_a;
  dwg_inthash *index_b;
  int depth;
  const char *entity; /* the entity being compared, for the reason */
  int differs;        /* the entity differs */
  int equivalent;     /* the entity is the same curve, stored another way */
  char reason[REASON_SIZE];
} Dynblock_Compare;

static void ATTRIBUTE_FORMAT (2, 3)
    differ (Dynblock_Compare *c, const char *fmt, ...)
{
  char what[REASON_SIZE / 2];
  va_list ap;
  int len;
  if (c->differs++ || c->reason[0])
    return;
  va_start (ap, fmt);
  len = vsnprintf (what, sizeof (what), fmt, ap);
  va_end (ap);
  if (len >= (int)sizeof (what))
    utf8_trim (what);
  set_reason (c->reason, sizeof (c->reason), "%s: %s", c->entity, what);
}

static void compare_object (Dynblock_Compare *c, const Dwg_Object *a,
                            const Dwg_Object *b);

/* An entity another entity owns: an ATTRIB, VERTEX or SEQEND. */
static int
is_owned_entity (const Dwg_Object *obj)
{
  return dwg_obj_is_subentity (obj) || obj->fixedtype == DWG_TYPE_SEQEND;
}

/* References compare by what they point at: the same object, entities
   with the same index in their blocks, or owned entities (ATTRIB,
   VERTEX, SEQEND) that compare equal. */
static void
compare_handles (Dynblock_Compare *c, const char *name, const BITCODE_RLL ta,
                 const BITCODE_RLL tb)
{
  uint64_t ia, ib;
  const Dwg_Object *oa, *ob;
  if (ta == tb)
    return;
  ia = ta ? hash_get (c->index_a, ta) : HASH_NOT_FOUND;
  ib = tb ? hash_get (c->index_b, tb) : HASH_NOT_FOUND;
  if (ia != HASH_NOT_FOUND && ia == ib)
    return;
  oa = handle_object (c->dwg, ta);
  ob = handle_object (c->dwg, tb);
  if (oa && ob && c->depth < 2 && is_owned_entity (oa)
      && is_owned_entity (ob))
    {
      c->depth++;
      compare_object (c, oa, ob);
      c->depth--;
      return;
    }
  differ (c, "%s points at " FORMAT_HV ", AutoCAD's at " FORMAT_HV, name,
          tb, ta);
}

static void
compare_ref (Dynblock_Compare *c, const char *name, const Dwg_Object_Ref *a,
             const Dwg_Object_Ref *b)
{
  compare_handles (c, name, ref_handle (a), ref_handle (b));
}

static int
is_string_type (const char *type)
{
  return strEQc (type, "T") || strEQc (type, "TV") || strEQc (type, "TU")
         || strEQc (type, "T16") || strEQc (type, "TU16")
         || strEQc (type, "T32") || strEQc (type, "D2T");
}

static int
is_double_type (const char *type)
{
  static const char *const types[]
      = { "BD",      "RD",      "2RD", "3BD",   "3RD",   "2BD", "3DPOINT",
          "2DPOINT", "BE",      "3BD_1", "2BD_1", "DD",  "BT",  NULL };
  for (const char *const *t = types; *t; t++)
    if (strEQ (type, *t))
      return 1;
  return 0;
}

/* The size of a scalar or point type, 0 for others. */
static size_t
scalar_size (const char *type)
{
  static const struct
  {
    const char *type;
    size_t size;
  } sizes[] = { { "B", 1 },        { "BB", 1 },      { "3B", 1 },
                { "4BITS", 1 },    { "RC", 1 },      { "RCd", 1 },
                { "RCx", 1 },      { "BS", 2 },      { "BSd", 2 },
                { "BSx", 2 },      { "RS", 2 },      { "RSd", 2 },
                { "RSx", 2 },      { "BL", 4 },      { "BLd", 4 },
                { "BLx", 4 },      { "RL", 4 },      { "RLd", 4 },
                { "RLx", 4 },      { "MS", 4 },      { "BLL", 8 },
                { "RLL", 8 },      { "RLLd", 8 },    { "BD", 8 },
                { "RD", 8 },       { "BT", 8 },      { "DD", 8 },
                { "2RD", 16 },     { "2BD", 16 },    { "2BD_1", 16 },
                { "2DPOINT", 16 }, { "3BD", 24 },    { "3BD_1", 24 },
                { "3RD", 24 },     { "3DPOINT", 24 }, { "BE", 24 },
                { "int", sizeof (int) },
                { "int32_t", sizeof (int32_t) },
                { "unsigned long", sizeof (unsigned long) },
                { NULL, 0 } };
  for (int i = 0; sizes[i].type; i++)
    if (strEQ (type, sizes[i].type))
      return sizes[i].size;
  return 0;
}

static void
compare_doubles (Dynblock_Compare *c, const char *name, const double *a,
                 const double *b, const size_t n)
{
  for (size_t i = 0; i < n; i++)
    if (!(fabs (a[i] - b[i]) <= c->tolerance)
        && !(isnan (a[i]) && isnan (b[i])))
      {
        if (n > 1)
          differ (c, "%s[%u] is %.17g, AutoCAD's %.17g", name, (unsigned)i,
                  b[i], a[i]);
        else
          differ (c, "%s is %.17g, AutoCAD's %.17g", name, b[i], a[i]);
        return;
      }
}

/* Strings compare as the drawing keeps them, in its codepage, and are
   given in the reason in UTF-8. */
static void
compare_string (Dynblock_Compare *c, const char *name, const char *a,
                const char *b)
{
  char *ua, *ub;
  if (strEQ (a ? a : "", b ? b : ""))
    return;
  ua = utf8_copy (c->dwg, a);
  ub = utf8_copy (c->dwg, b);
  differ (c, "%s is \"%s\", AutoCAD's \"%s\"", name, ub ? ub : "?",
          ua ? ua : "?");
  free (ua);
  free (ub);
}

static void
compare_color (Dynblock_Compare *c, const char *name, const Dwg_Color *a,
               const Dwg_Color *b)
{
  char sub[180];
  if (a->index != b->index || a->flag != b->flag || a->rgb != b->rgb
      || a->method != b->method || a->alpha_type != b->alpha_type
      || a->alpha != b->alpha)
    differ (c, "%s is %d/0x%x/0x%x, AutoCAD's %d/0x%x/0x%x", name, b->index,
            b->rgb, b->alpha, a->index, a->rgb, a->alpha);
  snprintf (sub, sizeof (sub), "%s.name", name);
  compare_string (c, sub, a->name, b->name);
  snprintf (sub, sizeof (sub), "%s.book_name", name);
  compare_string (c, sub, a->book_name, b->book_name);
  compare_ref (c, name, a->handle, b->handle);
}

static const Dwg_DYNAPI_field *
find_field (const Dwg_DYNAPI_field *fields, const char *name)
{
  for (const Dwg_DYNAPI_field *f = fields; f->name; f++)
    if (strEQ (f->name, name))
      return f;
  return NULL;
}

static uint64_t
count_value (const Dwg_DYNAPI_field *f, const void *base)
{
  const char *p = (const char *)base + f->offset;
  switch (f->size)
    {
    case 1:
      return *(const unsigned char *)p;
    case 2:
      return *(const uint16_t *)p;
    case 4:
      return *(const uint32_t *)p;
    case 8:
      return *(const uint64_t *)p;
    default:
      return 0;
    }
}

/* The number of elements of array field f of the struct at base, or -1
   when there is no rule for it: num_<name>, <name>_size for binary chunks,
   or the counts the specs keep elsewhere.

   A proxy entity's data is data_numbits bits long, in whole bytes: the
   decoder leaves its data_size 0. bit_read_bits puts the bits of a partial
   last byte at its high end and leaves the low bits 0, so comparing the
   whole byte compares the bits it holds. */
static int64_t
array_length (const char *structname, const Dwg_DYNAPI_field *fields,
              const Dwg_DYNAPI_field *f, const void *base)
{
  static const struct
  {
    const char *structname, *field, *count;
    int bits;  /* count is in bits */
    int fixed; /* the length, when there is no count */
  } rules[] = { { "INSERT", "attribs", "num_owned", 0, 0 },
                { "MINSERT", "attribs", "num_owned", 0, 0 },
                { "POLYLINE_2D", "vertex", "num_owned", 0, 0 },
                { "POLYLINE_3D", "vertex", "num_owned", 0, 0 },
                { "POLYLINE_MESH", "vertex", "num_owned", 0, 0 },
                { "POLYLINE_PFACE", "vertex", "num_owned", 0, 0 },
                { "HATCH_Path", "segs", "num_segs_or_paths", 0, 0 },
                { "HATCH_Path", "polyline_paths", "num_segs_or_paths", 0, 0 },
                { "PROXY_ENTITY", "data", "data_numbits", 1, 0 },
                // a 4x4 matrix
                { "MLEADER_Content_Block", "transform", NULL, 0, 16 },
                { NULL, NULL, NULL, 0, 0 } };
  const Dwg_DYNAPI_field *count = NULL;
  int bits = 0;
  char name[96];
  for (int i = 0; rules[i].field; i++)
    if (strEQ (structname, rules[i].structname)
        && strEQ (f->name, rules[i].field))
      {
        if (rules[i].fixed)
          return rules[i].fixed;
        count = find_field (fields, rules[i].count);
        bits = count && rules[i].bits;
      }
  if (!count)
    {
      snprintf (name, sizeof (name), "num_%s", f->name);
      count = find_field (fields, name);
    }
  if (!count && (strEQc (f->type, "TF") || strEQc (f->type, "RC*")))
    {
      snprintf (name, sizeof (name), "%s_size", f->name);
      count = find_field (fields, name);
    }
  if (!count)
    return -1;
  if (bits)
    return (int64_t)((count_value (count, base) + 7) / 8);
  return (int64_t)count_value (count, base);
}

static size_t
element_size (const char *type)
{
  if (strEQc (type, "H") || is_string_type (type))
    return sizeof (void *);
  if (scalar_size (type))
    return scalar_size (type);
  if (memBEGINc (type, "Dwg_"))
    return (size_t)dwg_dynapi_fields_size (&type[4]);
  return 0;
}

static void compare_fields (Dynblock_Compare *c, const char *prefix,
                            const char *structname,
                            const Dwg_DYNAPI_field *fields, const void *a,
                            const void *b);

static void
compare_value (Dynblock_Compare *c, const char *name, const char *type,
               const void *a, const void *b)
{
  if (strEQc (type, "H"))
    compare_ref (c, name, *(const BITCODE_H *)a, *(const BITCODE_H *)b);
  else if (is_string_type (type))
    compare_string (c, name, *(char *const *)a, *(char *const *)b);
  else if (strEQc (type, "CMC") || strEQc (type, "CMTC")
           || strEQc (type, "ENC"))
    compare_color (c, name, (const Dwg_Color *)a, (const Dwg_Color *)b);
  else if (is_double_type (type))
    compare_doubles (c, name, (const double *)a, (const double *)b,
                     scalar_size (type) / 8);
  else if (scalar_size (type))
    {
      if (memcmp (a, b, scalar_size (type)))
        differ (c, "%s [%s] differs", name, type);
    }
  else if (memBEGINc (type, "Dwg_"))
    {
      const Dwg_DYNAPI_field *sub = dwg_dynapi_subclass_fields (&type[4]);
      if (sub)
        compare_fields (c, name, &type[4], sub, a, b);
      else
        differ (c, "%s [%s] has no fields to compare", name, type);
    }
  else
    differ (c, "%s [%s] is of a type the check can't compare", name, type);
}

static void
compare_array (Dynblock_Compare *c, const char *name, const char *structname,
               const Dwg_DYNAPI_field *fields, const Dwg_DYNAPI_field *f,
               const void *a, const void *b)
{
  const void *va = *(void *const *)((const char *)a + f->offset);
  const void *vb = *(void *const *)((const char *)b + f->offset);
  const int64_t la = array_length (structname, fields, f, a);
  const int64_t lb = array_length (structname, fields, f, b);
  const size_t len = strlen (f->type);
  char etype[80];
  size_t esize;
  if (la < 0 || lb < 0)
    {
      if (va || vb)
        differ (c, "%s [%s] has no length to compare it by", name, f->type);
      return;
    }
  if (la != lb)
    {
      differ (c, "%s has %" PRId64 " elements, AutoCAD's %" PRId64, name, lb,
              la);
      return;
    }
  if (!la || (!va && !vb))
    return;
  if (!va || !vb)
    {
      differ (c, "%s: %s data", name, va ? "no" : "unexpected");
      return;
    }
  if (strEQc (f->type, "TF") || strEQc (f->type, "RC*"))
    {
      if (memcmp (va, vb, (size_t)la))
        differ (c, "%s differs", name);
      return;
    }
  snprintf (etype, sizeof (etype), "%.*s", (int)(len - 1), f->type);
  esize = element_size (etype);
  if (!esize)
    {
      differ (c, "%s [%s] has elements the check can't compare", name,
              f->type);
      return;
    }
  for (int64_t i = 0; i < la && !c->differs; i++)
    {
      char ename[192];
      snprintf (ename, sizeof (ename), "%s[%u]", name, (unsigned)i);
      compare_value (c, ename, etype, (const char *)va + i * esize,
                     (const char *)vb + i * esize);
    }
}

/* The content of a multileader's context, a union, at a and b: compares
   the member the context ctx, with fields, says it holds. */
static void
compare_mleader_content (Dynblock_Compare *c, const char *name,
                         const Dwg_DYNAPI_field *fields, const void *ctx,
                         const void *a, const void *b)
{
  const Dwg_DYNAPI_field *txt = find_field (fields, "has_content_txt");
  const Dwg_DYNAPI_field *blk = find_field (fields, "has_content_blk");
  const Dwg_DYNAPI_field *member_fields;
  const char *member, *structname;
  char sub[180];
  if (txt && count_value (txt, ctx))
    {
      member = "txt";
      structname = "MLEADER_Content_MText";
    }
  else if (blk && count_value (blk, ctx))
    {
      member = "blk";
      structname = "MLEADER_Content_Block";
    }
  else
    return;
  snprintf (sub, sizeof (sub), "%s.%s", name, member);
  member_fields = dwg_dynapi_subclass_fields (structname);
  if (member_fields)
    compare_fields (c, sub, structname, member_fields, a, b);
  else
    differ (c, "%s [%s] has no fields to compare", sub, structname);
}

/* Whether field of struct structname is an angle that a turn changes, of
   which 0 and 2 pi, or any two a whole turn apart, give the same
   geometry: AutoCAD's evaluation can put one just below 2 pi where the
   evaluator's is just above 0. */
static int
is_turned_angle (const char *structname, const char *field)
{
  static const char *const rules[][2]
      = { { "ARC", "start_angle" },   { "ARC", "end_angle" },
          { "INSERT", "rotation" },   { "TEXT", "rotation" },
          { "ATTRIB", "rotation" },   { "POINT", "x_ang" },
          { NULL, NULL } };
  for (int i = 0; rules[i][0]; i++)
    if (strEQ (structname, rules[i][0]) && strEQ (field, rules[i][1]))
      return 1;
  return 0;
}

static void
compare_fields (Dynblock_Compare *c, const char *prefix,
                const char *structname, const Dwg_DYNAPI_field *fields,
                const void *a, const void *b)
{
  for (const Dwg_DYNAPI_field *f = fields; f->name && !c->differs; f++)
    {
      const char *type = f->type;
      const size_t len = strlen (type);
      const char *pa = (const char *)a + f->offset;
      const char *pb = (const char *)b + f->offset;
      size_t esize;
      char name[160];
      // links into the document and its memory, not data
      if (strEQc (f->name, "parent") || strEQc (f->name, "dwg")
          || strEQc (f->name, "objid") || strEQc (f->name, "__iterator")
          || memBEGINc (type, "struct "))
        continue;
      snprintf (name, sizeof (name), "%s%s%s", prefix, *prefix ? "." : "",
                f->name);
      if (strEQc (type, "Dwg_MLEADER_Content"))
        {
          compare_mleader_content (c, name, fields, a, pa, pb);
          continue;
        }
      if (strEQc (type, "TF") || strEQc (type, "RC*")
          || (len && type[len - 1] == '*' && !is_string_type (type)))
        {
          compare_array (c, name, structname, fields, f, a, b);
          continue;
        }
      if ((strEQc (type, "BD") || strEQc (type, "RD"))
          && is_turned_angle (structname, f->name))
        {
          const double da = *(const double *)pa, db = *(const double *)pb;
          if (!(angle_difference (da, db) <= c->tolerance))
            differ (c, "%s is %.17g, AutoCAD's %.17g", name, db, da);
          // Angles that each match around the circle can still give arcs
          // a whole turn apart in sweep: a hair short of a full circle and
          // a hair long of none.
          else if (strEQc (structname, "ARC") && strEQc (f->name, "end_angle"))
            {
              const Dwg_Entity_ARC *arc_a = (const Dwg_Entity_ARC *)a;
              const Dwg_Entity_ARC *arc_b = (const Dwg_Entity_ARC *)b;
              const double sa
                  = normal_angle (arc_a->end_angle - arc_a->start_angle);
              const double sb
                  = normal_angle (arc_b->end_angle - arc_b->start_angle);
              if (!(fabs (sa - sb) <= c->tolerance))
                differ (c, "sweep is %.17g, AutoCAD's %.17g", sb, sa);
            }
          continue;
        }
      // an array held in the struct: dynapi gives the size of all of it
      esize = element_size (type);
      if (esize && f->size > esize && f->size % esize == 0)
        for (size_t i = 0; i < f->size / esize; i++)
          {
            char ename[180];
            snprintf (ename, sizeof (ename), "%s[%u]", name, (unsigned)i);
            compare_value (c, ename, type, pa + i * esize, pb + i * esize);
          }
      else
        compare_value (c, name, type, pa, pb);
    }
}

/* The entity data both copies keep from their definition entity: the
   visibility flag, layer, colour, linetype and its scale, lineweight and
   plot style. The owner, links, reactors, extension dictionary and EED
   are the block's own. */
static void
compare_common (Dynblock_Compare *c, const Dwg_Object_Entity *a,
                const Dwg_Object_Entity *b)
{
  if (a->invisible != b->invisible)
    differ (c, "invisible is %u, AutoCAD's %u", (unsigned)b->invisible,
            (unsigned)a->invisible);
  compare_ref (c, "layer", a->layer, b->layer);
  compare_color (c, "color", &a->color, &b->color);
  if (a->ltype_flags != b->ltype_flags)
    differ (c, "ltype_flags is %u, AutoCAD's %u", (unsigned)b->ltype_flags,
            (unsigned)a->ltype_flags);
  compare_ref (c, "ltype", a->ltype, b->ltype);
  compare_doubles (c, "ltype_scale", &a->ltype_scale, &b->ltype_scale, 1);
  if (a->linewt != b->linewt)
    differ (c, "linewt is %u, AutoCAD's %u", (unsigned)b->linewt,
            (unsigned)a->linewt);
  if (a->plotstyle_flags != b->plotstyle_flags)
    differ (c, "plotstyle_flags is %u, AutoCAD's %u",
            (unsigned)b->plotstyle_flags, (unsigned)a->plotstyle_flags);
  compare_ref (c, "plotstyle", a->plotstyle, b->plotstyle);
}

static int
is_near (const Dynblock_Compare *c, const double a, const double b)
{
  return fabs (a - b) <= c->tolerance;
}

/* Whether the angles a and b are the same within the tolerance, around the
   circle. */
static int
is_near_angle (const Dynblock_Compare *c, const double a, const double b)
{
  return angle_difference (a, b) <= c->tolerance;
}

/* The sweep of ELLIPSE e from its start to its end parameter, counter-
   clockwise, in [0, 2 pi), but 2 pi for a whole turn, so that a full
   ellipse stays distinct from an arc of no length. */
static double
ellipse_sweep (const Dynblock_Compare *c, const Dwg_Entity_ELLIPSE *e)
{
  const double raw = e->end_angle - e->start_angle;
  double sweep;
  if (raw != 0.0 && fabs (fabs (raw) - 2.0 * M_PI) <= c->tolerance)
    return 2.0 * M_PI;
  sweep = fmod (raw, 2.0 * M_PI);
  return sweep < 0.0 ? sweep + 2.0 * M_PI : sweep;
}

/* Whether ELLIPSE b is ELLIPSE a with its major axis reversed and its
   parameters turned by pi: the same curve, stored another way. AutoCAD
   stores a representation block's ellipse so after some grip edits, and
   keeps it when they are undone; its evaluation from the definition keeps
   the definition's axis. */
static int
is_reversed_ellipse (const Dynblock_Compare *c, const Dwg_Entity_ELLIPSE *a,
                     const Dwg_Entity_ELLIPSE *b)
{
  return is_near (c, a->center.x, b->center.x)
         && is_near (c, a->center.y, b->center.y)
         && is_near (c, a->center.z, b->center.z)
         && is_near (c, a->extrusion.x, b->extrusion.x)
         && is_near (c, a->extrusion.y, b->extrusion.y)
         && is_near (c, a->extrusion.z, b->extrusion.z)
         && is_near (c, a->axis_ratio, b->axis_ratio)
         && is_near (c, a->sm_axis.x, -b->sm_axis.x)
         && is_near (c, a->sm_axis.y, -b->sm_axis.y)
         && is_near (c, a->sm_axis.z, -b->sm_axis.z)
         && is_near_angle (c, a->start_angle + M_PI, b->start_angle)
         && is_near (c, ellipse_sweep (c, a), ellipse_sweep (c, b));
}

static void
compare_object (Dynblock_Compare *c, const Dwg_Object *a, const Dwg_Object *b)
{
  const Dwg_DYNAPI_field *fields;
  const char *name;
  if (a->fixedtype != b->fixedtype)
    {
      differ (c, "is a %s, AutoCAD's a %s", dwg_type_name (b->fixedtype),
              dwg_type_name (a->fixedtype));
      return;
    }
  if (a->supertype != DWG_SUPERTYPE_ENTITY)
    {
      differ (c, "is not an entity");
      return;
    }
  compare_common (c, a->tio.entity, b->tio.entity);
  if (a->fixedtype == DWG_TYPE_ELLIPSE && !c->differs
      && is_reversed_ellipse (c, a->tio.entity->tio.ELLIPSE,
                              b->tio.entity->tio.ELLIPSE))
    {
      c->equivalent++;
      return;
    }
  // dwg_type_name gives a proxy entity its DXF name, ACAD_PROXY_ENTITY;
  // dynapi lists its fields as PROXY_ENTITY
  name = a->fixedtype == DWG_TYPE_PROXY_ENTITY
             ? "PROXY_ENTITY"
             : dwg_type_name (a->fixedtype);
  fields = name ? dwg_dynapi_entity_fields (name) : NULL;
  if (!fields)
    {
      differ (c, "%s has no fields to compare", name ? name : "?");
      return;
    }
  compare_fields (c, "", name, fields, a->tio.entity->tio.LINE,
                  b->tio.entity->tio.LINE);
}

/* Maps each entity of block _blk to its AcDbBlockRepETag index into index,
   and each index + 1 to its entity into byindex. Returns 0, or 1 with the
   reason in c when an entry of the block's entity list doesn't resolve, or
   an entity has no index or shares one. */
static int
index_entities (Dynblock_Compare *c, const Dwg_Object_BLOCK_HEADER *_blk,
                const char *whose, dwg_inthash *index, dwg_inthash *byindex)
{
  for (BITCODE_BL i = 0; _blk->entities && i < _blk->num_owned; i++)
    {
      const Dwg_Object *ent = ref_object (c->dwg, _blk->entities[i]);
      BITCODE_RL idx;
      if (!ent)
        {
          snprintf (c->reason, sizeof (c->reason),
                    "%s entity " FORMAT_HV " doesn't resolve", whose,
                    ref_handle (_blk->entities[i]));
          return 1;
        }
      if (!etag_index (c->dwg, ent, &idx))
        {
          snprintf (c->reason, sizeof (c->reason),
                    "%s entity " FORMAT_HV " has no AcDbBlockRepETag", whose,
                    ent->handle.value);
          return 1;
        }
      // hash keys can't be 0
      if (hash_get (byindex, (uint64_t)idx + 1) != HASH_NOT_FOUND)
        {
          snprintf (c->reason, sizeof (c->reason),
                    "%s entities " FORMAT_HV " and " FORMAT_HV
                    " share AcDbBlockRepETag index %u",
                    whose, (BITCODE_RLL)hash_get (byindex, (uint64_t)idx + 1),
                    ent->handle.value, (unsigned)idx);
          return 1;
        }
      hash_set (index, ent->handle.value, idx);
      hash_set (byindex, (uint64_t)idx + 1, ent->handle.value);
    }
  return 0;
}

/* Compares AutoCAD's representation block a with evaluated block b, into
   check. */
static void
compare_blocks (const Dwg_Data *dwg, const BITCODE_RLL a_handle,
                const BITCODE_RLL b_handle, const double tolerance,
                Dwg_DynBlock_Check *check, char *reason,
                const size_t reason_size)
{
  const Dwg_Object_BLOCK_HEADER *_a
      = handle_object (dwg, a_handle)->tio.object->tio.BLOCK_HEADER;
  const Dwg_Object_BLOCK_HEADER *_b
      = handle_object (dwg, b_handle)->tio.object->tio.BLOCK_HEADER;
  Dynblock_Compare c;
  dwg_inthash *byindex_a, *byindex_b;
  memset (&c, 0, sizeof (c));
  c.dwg = dwg;
  c.tolerance = tolerance;
  c.index_a = hash_new (_a->num_owned * 2 + 8);
  c.index_b = hash_new (_b->num_owned * 2 + 8);
  byindex_a = hash_new (_a->num_owned * 2 + 8);
  byindex_b = hash_new (_b->num_owned * 2 + 8);
  check->status = DWG_DYNBLOCK_CHECK_MISMATCH;
  if (!c.index_a || !c.index_b || !byindex_a || !byindex_b)
    {
      check->status = DWG_DYNBLOCK_CHECK_FAILED;
      set_reason (reason, reason_size, "out of memory");
      goto done;
    }
  if (index_entities (&c, _a, "AutoCAD's", c.index_a, byindex_a)
      || index_entities (&c, _b, "the evaluation's", c.index_b, byindex_b))
    {
      set_reason (reason, reason_size, "%s", c.reason);
      goto done;
    }
  if (_a->num_owned != _b->num_owned)
    set_reason (reason, reason_size,
                "the evaluation has %u entities, AutoCAD's block %u",
                (unsigned)_b->num_owned, (unsigned)_a->num_owned);
  c.entity = "the block";
  compare_doubles (&c, "base_pt", &_a->base_pt.x, &_b->base_pt.x, 3);
  for (BITCODE_BL i = 0; _a->entities && i < _a->num_owned; i++)
    {
      const Dwg_Object *ea = ref_object (dwg, _a->entities[i]);
      const Dwg_Object *eb;
      BITCODE_RL idx;
      uint64_t hb;
      char entity[96];
      if (!ea || !etag_index (dwg, ea, &idx))
        continue;
      hb = hash_get (byindex_b, (uint64_t)idx + 1);
      snprintf (entity, sizeof (entity), "%s " FORMAT_HV " (index %u)",
                dwg_type_name (ea->fixedtype), ea->handle.value,
                (unsigned)idx);
      c.entity = entity;
      c.differs = 0;
      c.equivalent = 0;
      check->num_entities++;
      eb = hb != HASH_NOT_FOUND ? handle_object (dwg, (BITCODE_RLL)hb) : NULL;
      if (!eb)
        differ (&c, "the evaluation has no entity with this index");
      else
        compare_object (&c, ea, eb);
      if (c.differs)
        check->num_differing++;
      else if (c.equivalent)
        check->num_equivalent++;
    }
  if (!reason[0] && c.reason[0])
    set_reason (reason, reason_size, "%s", c.reason);
  if (!reason[0])
    check->status = DWG_DYNBLOCK_CHECK_MATCH;
done:
  free_hash (c.index_a);
  free_hash (c.index_b);
  free_hash (byindex_a);
  free_hash (byindex_b);
}

/* The value of property p differs from the definition's. */
static int
value_differs (const Dwg_DynParam *p, const Dwg_DynValue *v)
{
  switch (p->kind)
    {
    case DWG_DYNPARAM_VISIBILITY:
      return v->state && p->default_state
             && strNE (v->state, p->default_state);
    case DWG_DYNPARAM_ROTATION:
      return angle_difference (v->value, p->default_value)
             > DYNBLOCK_VALUE_EPSILON;
    default:
      return fabs (v->value - p->default_value) > DYNBLOCK_VALUE_EPSILON;
    }
}

/* The kinds of the values differing from the definition's, as bits. */
static BITCODE_BL
changed_kinds (const Dwg_Data *dwg, const BITCODE_RLL def_handle,
               const Dwg_DynValue *values, const BITCODE_BL count)
{
  Param_List list;
  int parametric;
  BITCODE_BL kinds = 0;
  if (definition_params (dwg, def_handle, &list, &parametric, NULL, 0))
    return 0;
  for (BITCODE_BL i = 0; i < count; i++)
    for (BITCODE_BL j = 0; j < list.count; j++)
      if (strEQ (list.params[j].name, values[i].name))
        {
          if (value_differs (&list.params[j], &values[i]))
            kinds |= 1U << list.params[j].kind;
          break;
        }
  dwg_dynblock_free_params (list.params, list.count);
  return kinds;
}

/* The status of a check whose reading of the values or evaluation returned
   error, from DWG_ERR_CRITICAL up: a refusal of what the evaluator doesn't
   support is unsupported, other errors fail. */
static Dwg_DynBlock_Check_Status
error_status (const int error)
{
  return (error & DYNBLOCK_REFUSED)
                 && (error
                     & (DWG_ERR_NOTYETSUPPORTED | DWG_ERR_UNHANDLEDCLASS))
             ? DWG_DYNBLOCK_CHECK_UNSUPPORTED
             : DWG_DYNBLOCK_CHECK_FAILED;
}

/* Checks the placement check names, into check. Returns 0, or
   DWG_ERR_OUTOFMEM. */
static int
check_placement (Dwg_Data *dwg, Dwg_DynBlock_Check *check,
                 const double tolerance)
{
  const Dwg_Object *insert = handle_object (dwg, check->insert);
  Dwg_DynValue *values = NULL;
  BITCODE_BL count = 0;
  BITCODE_RLL def_handle = 0;
  char reason[REASON_SIZE] = "";
  char left_out[REASON_SIZE] = "";
  int error, oom = 0;

  // the definition the block names, unless the values name another
  check->definition = btag_definition (dwg, handle_object (dwg, check->block));
  if (!block_representation (dwg, insert))
    {
      check->status = DWG_DYNBLOCK_CHECK_NO_VALUES;
      set_reason (reason, sizeof (reason),
                  "the INSERT has no AcDbBlockRepresentation");
      goto done;
    }
  error = placement_values (dwg, check->insert, &def_handle, &values, &count,
                            reason, sizeof (reason));
  if (def_handle)
    check->definition = def_handle;
  if (error >= DWG_ERR_CRITICAL)
    {
      check->status = error_status (error);
      oom = error & DWG_ERR_OUTOFMEM;
      goto done;
    }
  check->kinds = changed_kinds (dwg, def_handle, values, count);
  if (error)
    {
      check->status = DWG_DYNBLOCK_CHECK_UNSUPPORTED;
      goto done;
    }
  error = evaluate (dwg, def_handle, values, count, &check->evaluated,
                    reason, sizeof (reason));
  if (error >= DWG_ERR_CRITICAL)
    {
      check->status = error_status (error);
      oom = error & DWG_ERR_OUTOFMEM;
      goto done;
    }
  // below DWG_ERR_CRITICAL: the copy left out data, as reason says
  if (error)
    memcpy (left_out, reason, sizeof (left_out));
  reason[0] = '\0';
  compare_blocks (dwg, check->block, check->evaluated, tolerance, check,
                  reason, sizeof (reason));
  // compare_blocks fails only for memory
  oom = check->status == DWG_DYNBLOCK_CHECK_FAILED;
  // The comparison doesn't see all the copy can leave out, such as an EED
  // group, so an evaluation that left data out is never a match.
  if (error && !oom)
    {
      char differs[REASON_SIZE];
      memcpy (differs, reason, sizeof (differs));
      set_reason (reason, sizeof (reason), "%s%s%s", left_out,
                  differs[0] ? "; " : "", differs);
      check->status = DWG_DYNBLOCK_CHECK_MISMATCH;
    }
done:
  dwg_dynblock_free_values (values, count);
  if (check->status != DWG_DYNBLOCK_CHECK_MATCH)
    {
      check->reason = strdup (reason);
      if (!check->reason)
        oom = 1;
    }
  return oom ? DWG_ERR_OUTOFMEM : 0;
}

/* An anonymous block with AcDbBlockRepBTag EED. */
static int
is_representation_block (const Dwg_Data *dwg, const Dwg_Object *obj)
{
  return obj && obj->fixedtype == DWG_TYPE_BLOCK_HEADER
         && obj->tio.object->tio.BLOCK_HEADER->anonymous
         && btag_definition (dwg, obj) != 0;
}

static Dwg_DynBlock_Check *
add_check (Dwg_DynBlock_Check_Report *report, BITCODE_BL *size)
{
  Dwg_DynBlock_Check *check;
  if (report->num_checks == *size)
    {
      const BITCODE_BL more = *size ? *size * 2 : 16;
      Dwg_DynBlock_Check *checks = (Dwg_DynBlock_Check *)realloc (
          report->checks, more * sizeof (Dwg_DynBlock_Check));
      if (!checks)
        return NULL;
      report->checks = checks;
      *size = more;
    }
  check = &report->checks[report->num_checks++];
  memset (check, 0, sizeof (*check));
  return check;
}

EXPORT Dwg_DynBlock_Check_Report *
dwg_dynblock_check (Dwg_Data *dwg, const double tolerance)
{
  Dwg_DynBlock_Check_Report *report;
  dwg_inthash *drawn;
  BITCODE_BL size = 0, num_objects;
  if (!dwg)
    return NULL;
  loglevel = dwg->opts & DWG_OPTS_LOGLEVEL;
  report = (Dwg_DynBlock_Check_Report *)calloc (
      1, sizeof (Dwg_DynBlock_Check_Report));
  drawn = hash_new (64);
  if (!report || !drawn)
    {
      free (report);
      free_hash (drawn);
      return NULL;
    }
  // The placements, found before any evaluation adds objects.
  num_objects = dwg->num_objects;
  for (BITCODE_BL i = 0; i < num_objects; i++)
    {
      const Dwg_Object *obj = &dwg->object[i];
      const Dwg_Object *block;
      Dwg_DynBlock_Check *check;
      if (obj->fixedtype != DWG_TYPE_INSERT
          && obj->fixedtype != DWG_TYPE_MINSERT)
        continue;
      block = ref_object (dwg, placement_block (obj));
      if (!is_representation_block (dwg, block))
        continue;
      check = add_check (report, &size);
      if (!check)
        goto oom;
      check->insert = obj->handle.value;
      check->block = block->handle.value;
      hash_set (drawn, block->handle.value, 1);
    }
  for (BITCODE_BL i = 0; i < num_objects; i++)
    {
      const Dwg_Object *obj = &dwg->object[i];
      Dwg_DynBlock_Check *check;
      if (!is_representation_block (dwg, obj)
          || hash_get (drawn, obj->handle.value) != HASH_NOT_FOUND)
        continue;
      check = add_check (report, &size);
      if (!check)
        goto oom;
      check->block = obj->handle.value;
      check->definition = btag_definition (dwg, obj);
      check->status = DWG_DYNBLOCK_CHECK_NO_VALUES;
      check->reason = strdup ("no INSERT draws the block");
      if (!check->reason)
        goto oom;
    }
  free_hash (drawn);
  drawn = NULL;

  for (BITCODE_BL i = 0; i < report->num_checks; i++)
    {
      Dwg_DynBlock_Check *check = &report->checks[i];
      if (check->insert && check_placement (dwg, check, tolerance))
        goto oom;
      switch (check->status)
        {
        case DWG_DYNBLOCK_CHECK_MATCH:
          report->num_match++;
          break;
        case DWG_DYNBLOCK_CHECK_MISMATCH:
          report->num_mismatch++;
          break;
        case DWG_DYNBLOCK_CHECK_UNSUPPORTED:
          report->num_unsupported++;
          break;
        case DWG_DYNBLOCK_CHECK_NO_VALUES:
          report->num_no_values++;
          break;
        default:
          report->num_failed++;
          break;
        }
    }
  return report;

oom:
  free_hash (drawn);
  dwg_dynblock_free_check_report (report);
  return NULL;
}

EXPORT void
dwg_dynblock_free_check_report (Dwg_DynBlock_Check_Report *report)
{
  if (!report)
    return;
  for (BITCODE_BL i = 0; report->checks && i < report->num_checks; i++)
    free (report->checks[i].reason);
  free (report->checks);
  free (report);
}
