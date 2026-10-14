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
#include "dwg_dynblock_api.h"

#define DWG_LOGLEVEL loglevel
#include "logging.h"
#include "dwg_api.h"

/* What a refused call returns, with a bit for the reason, as copy.c's. */
#define DYNBLOCK_REFUSED DWG_ERR_INVALIDDWG

/* How far a value may be from the definition's and still be at it. */
#define DYNBLOCK_VALUE_EPSILON 1e-9

#define REASON_SIZE 512

/* The start of every evaluation graph node's struct: parameters, actions,
   grips, grip location components and DYNAMICBLOCKPROXYNODE. */
typedef struct _dynblock_element
{
  struct _dwg_object_object *parent;
  Dwg_EvalExpr evalexpr;
} Dynblock_Element;

static void ATTRIBUTE_FORMAT (3, 4)
    set_reason (char *reason, const size_t size, const char *fmt, ...)
{
  va_list ap;
  if (!reason || !size)
    return;
  va_start (ap, fmt);
  vsnprintf (reason, size, fmt, ap);
  va_end (ap);
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

static double
distance_3d (const BITCODE_3BD *a, const BITCODE_3BD *b)
{
  const double dx = b->x - a->x, dy = b->y - a->y, dz = b->z - a->z;
  return sqrt (dx * dx + dy * dy + dz * dz);
}

/* The angle of end about base, from the direction of base_angle, in
   [0, 2 pi): the value of a rotation parameter. */
static double
rotation_angle (const BITCODE_3BD *base, const BITCODE_3BD *end,
                const BITCODE_3BD *base_angle)
{
  double angle = atan2 (end->y - base->y, end->x - base->x)
                 - atan2 (base_angle->y - base->y, base_angle->x - base->x);
  angle = fmod (angle, 2.0 * M_PI);
  if (angle < 0.0)
    angle += 2.0 * M_PI;
  return angle;
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
   list->unknown_class names a node of a class LibreDWG can't decode. */
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
      set_reason (reason, reason_size, "out of memory");
      dwg_dynblock_free_params (list->params, list->count);
      memset (list, 0, sizeof (*list));
      return DWG_ERR_OUTOFMEM;
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
   dwg_dynblock_get_values (). */
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
    // a placement at the defaults draws the definition itself
    def_handle = ref_handle (placement_block (insert));
  error = definition_params (dwg, def_handle, &list, &parametric, reason,
                             reason_size);
  if (error)
    return error;
  *def_handlep = def_handle;
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
      *def_handlep = 0;
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
   VERTEXes and SEQENDs of each after it, into a new array. */
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
      if (ent->supertype != DWG_SUPERTYPE_ENTITY
          || ent->fixedtype == DWG_TYPE_UNKNOWN_ENT)
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

/* The evaluation itself, as dwg_dynblock_evaluate (), into *rep_handlep. */
static int
evaluate (Dwg_Data *dwg, const BITCODE_RLL def_handle,
          const Dwg_DynValue *values, const BITCODE_BL num_values,
          BITCODE_RLL *rep_handlep, char *reason, const size_t reason_size)
{
  Param_List list;
  BITCODE_BL *state = NULL;
  BITCODE_RLL *handles = NULL;
  BITCODE_BL num_handles = 0;
  Dwg_Object_BLOCK_HEADER *_def, *_rep;
  Dwg_Copy_Map *map = NULL;
  Dwg_Copy_Unresolved *unresolved = NULL;
  BITCODE_BL num_unresolved = 0;
  dwg_inthash *copies = NULL;
  int parametric, error, copy_error = 0;
  *rep_handlep = 0;

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
  // the visibility state chosen for each property, + 1, or 0
  state = (BITCODE_BL *)calloc (list.count + 1, sizeof (BITCODE_BL));
  if (!state)
    {
      error = DWG_ERR_OUTOFMEM;
      goto done;
    }
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
          if (fabs (v->value - p->default_value) > DYNBLOCK_VALUE_EPSILON)
            {
              set_reason (reason, reason_size,
                          "linear property '%s': changing it from %g to %g "
                          "is not supported",
                          p->name, p->default_value, v->value);
              error = DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
              goto done;
            }
          break;
        case DWG_DYNPARAM_ROTATION:
          if (angle_difference (v->value, p->default_value)
              > DYNBLOCK_VALUE_EPSILON)
            {
              set_reason (reason, reason_size,
                          "rotation property '%s': changing it from %g to "
                          "%g is not supported",
                          p->name, p->default_value, v->value);
              error = DYNBLOCK_REFUSED | DWG_ERR_NOTYETSUPPORTED;
              goto done;
            }
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
  // the add API doesn't count the entities it adds, as the decoder and the
  // copy do
  dwg->num_entities++;
  _rep->anonymous = 1;
  _rep->flag |= 1;
  _rep->base_pt = _def->base_pt;
  *rep_handlep = block_header_object (_rep)->handle.value;
  for (BITCODE_BL i = 0; i < num_handles; i++)
    if (dwg_copy_map_get (map, handles[i]))
      hash_set (copies, dwg_copy_map_get (map, handles[i]), 1);
  for (BITCODE_BL i = 0; i < num_handles; i++)
    {
      Dwg_Object *copy
          = handle_object (dwg, dwg_copy_map_get (map, handles[i]));
      if (!copy || copy->supertype != DWG_SUPERTYPE_ENTITY)
        continue;
      detach_copy (copy, copies);
      if (!dwg_obj_is_subentity (copy))
        dwg_insert_entity (_rep, copy);
    }
  if (!dwg_add_ENDBLK (_rep))
    {
      set_reason (reason, reason_size, "could not add the block's end");
      error = DWG_ERR_OUTOFMEM;
      goto done;
    }
  dwg->num_entities++;

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
  free (handles);
  free (state);
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
  if (c->differs++ || c->reason[0])
    return;
  va_start (ap, fmt);
  vsnprintf (what, sizeof (what), fmt, ap);
  va_end (ap);
  snprintf (c->reason, sizeof (c->reason), "%s: %s", c->entity, what);
}

static void compare_object (Dynblock_Compare *c, const Dwg_Object *a,
                            const Dwg_Object *b);

/* References compare by what they point at: the same object, entities
   with the same index in their blocks, or owned subentities (ATTRIB,
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
  if (oa && ob && c->depth < 2 && dwg_obj_is_subentity (oa)
      && dwg_obj_is_subentity (ob))
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

static void
compare_string (Dynblock_Compare *c, const char *name, const char *a,
                const char *b)
{
  if (strNE (a ? a : "", b ? b : ""))
    differ (c, "%s is \"%s\", AutoCAD's \"%s\"", name, b ? b : "",
            a ? a : "");
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
   or the counts the specs keep elsewhere. */
static int64_t
array_length (const char *structname, const Dwg_DYNAPI_field *fields,
              const Dwg_DYNAPI_field *f, const void *base)
{
  static const struct
  {
    const char *structname, *field, *count;
  } rules[] = { { "INSERT", "attribs", "num_owned" },
                { "MINSERT", "attribs", "num_owned" },
                { "POLYLINE_2D", "vertex", "num_owned" },
                { "POLYLINE_3D", "vertex", "num_owned" },
                { "POLYLINE_MESH", "vertex", "num_owned" },
                { "POLYLINE_PFACE", "vertex", "num_owned" },
                { "HATCH_Path", "segs", "num_segs_or_paths" },
                { "HATCH_Path", "polyline_paths", "num_segs_or_paths" },
                { NULL, NULL, NULL } };
  const Dwg_DYNAPI_field *count = NULL;
  char name[96];
  for (int i = 0; rules[i].field; i++)
    if (strEQ (structname, rules[i].structname)
        && strEQ (f->name, rules[i].field))
      count = find_field (fields, rules[i].count);
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
  return count ? (int64_t)count_value (count, base) : -1;
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
      if (strEQc (type, "TF") || strEQc (type, "RC*")
          || (len && type[len - 1] == '*' && !is_string_type (type)))
        {
          compare_array (c, name, structname, fields, f, a, b);
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
   visibility flag, layer, colour, linetype and lineweight. The owner,
   links, reactors, extension dictionary and EED are the block's own. */
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
         && is_near (c, a->end_angle - a->start_angle,
                  b->end_angle - b->start_angle);
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
  name = dwg_type_name (a->fixedtype);
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
   reason in c when an entity has no index or shares one. */
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
  int error, oom = 0;

  if (!block_representation (dwg, insert))
    {
      check->status = DWG_DYNBLOCK_CHECK_NO_VALUES;
      set_reason (reason, sizeof (reason),
                  "the INSERT has no AcDbBlockRepresentation");
      goto done;
    }
  error = placement_values (dwg, check->insert, &def_handle, &values, &count,
                            reason, sizeof (reason));
  check->definition = def_handle;
  if (error >= DWG_ERR_CRITICAL)
    {
      check->status = DWG_DYNBLOCK_CHECK_FAILED;
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
      check->status
          = (error & DYNBLOCK_REFUSED)
                    && (error
                        & (DWG_ERR_NOTYETSUPPORTED | DWG_ERR_UNHANDLEDCLASS))
                ? DWG_DYNBLOCK_CHECK_UNSUPPORTED
                : DWG_DYNBLOCK_CHECK_FAILED;
      oom = error & DWG_ERR_OUTOFMEM;
      goto done;
    }
  // data the copy left out shows in the comparison, if it is compared
  reason[0] = '\0';
  compare_blocks (dwg, check->block, check->evaluated, tolerance, check,
                  reason, sizeof (reason));
  // compare_blocks fails only for memory
  oom = check->status == DWG_DYNBLOCK_CHECK_FAILED;
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

static int
is_representation_block (const Dwg_Data *dwg, const Dwg_Object *obj)
{
  return obj && obj->fixedtype == DWG_TYPE_BLOCK_HEADER
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
