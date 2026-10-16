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
 * dynblock_insert.c: placing a dynamic block at chosen values, with the
 * representation block and the stored values that AutoCAD keeps for a
 * placement. See dwg_dynblock_api.h.
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
#include <math.h>
#include <float.h>

#include "common.h"
#include "bits.h"
#include "dwg.h"
#include "decode.h"
#include "encode.h"
#include "hash.h"
#include "classes.h"
#include "copy.h"
#include "dwg_dynblock_api.h"
#include "dynblock.h"

#define DWG_LOGLEVEL loglevel
#include "logging.h"
#include "dwg_api.h"

/* in dwg.c */
void dwg_set_next_objhandle (Dwg_Object *obj);
void dwg_set_handle_size (Dwg_Handle *restrict hdl);
BITCODE_H dwg_find_tablehandle_silent (Dwg_Data *restrict dwg,
                                       const char *restrict name,
                                       const char *restrict table);

/* What a refused call returns, with a bit for the reason, as the
   evaluator's. */
#define INSERT_REFUSED DWG_ERR_INVALIDDWG

/* How far a value may be from the definition's and still be at it, as in
   dynblock_eval.c. */
#define VALUE_EPSILON 1e-9

#define REASON_SIZE 512

/* The fields every evaluation graph node starts with: parameters, actions,
   grips and grip location components. */
typedef struct _dynblock_node
{
  struct _dwg_object_object *parent;
  BLOCKELEMENT_fields;
} Dynblock_Node;

/* The start of a two-point parameter: linear, alignment, flip, rotation. */
typedef struct _dynblock_2pt_param
{
  struct _dwg_object_object *parent;
  BLOCK2PTPARAMETER_fields;
} Dynblock_2Pt_Param;

/* What the XRECORD of ACAD_ENHANCEDBLOCKDATA holds for a graph node, after
   the two values of its class and the node's element version. */
typedef enum DYNBLOCK_RECORD_KIND
{
  RECORD_NONE,           /* AutoCAD keeps no record for nodes of the class */
  RECORD_TWO_POINTS,     /* the base and end points, then (0, 0, -1) */
  RECORD_FLIP,           /* as RECORD_TWO_POINTS, then the flip state */
  RECORD_ROTATION,       /* as RECORD_TWO_POINTS, then the base angle point */
  RECORD_VISIBILITY,     /* the grip point, then the state's name */
  RECORD_POINT,          /* the parameter's point */
  RECORD_FLIP_GRIP,      /* two 0s */
  RECORD_ALIGNMENT_GRIP, /* a 0 */
  RECORD_OFFSET,         /* the second of the action's offsets */
  RECORD_BASE_POINT      /* the action's base point */
} Dynblock_Record_Kind;

typedef struct _dynblock_node_record
{
  Dwg_Object_Type type;
  Dynblock_Record_Kind kind;
  /* the two 1071 values AutoCAD starts every record of the class with */
  BITCODE_BL id[2];
} Dynblock_Node_Record;

/* The records AutoCAD writes for a placement, as in
   test/test-data/2004/Dynblocks.dwg, which dynblock_insert_test compares
   them with. A record holds a node's state at the placement's values: a
   parameter's points after the actions have moved them, which the
   evaluation gives; an action's record is the same at every value.
   Classes not listed are refused, as their records are not known. */
static const Dynblock_Node_Record node_records[] = {
  { DWG_TYPE_BLOCKLINEARPARAMETER, RECORD_TWO_POINTS, { 18597260, 25303744 } },
  { DWG_TYPE_BLOCKALIGNMENTPARAMETER,
    RECORD_TWO_POINTS,
    { 82446124, 112284408 } },
  { DWG_TYPE_BLOCKFLIPPARAMETER, RECORD_FLIP, { 6887019, 9372877 } },
  { DWG_TYPE_BLOCKROTATIONPARAMETER, RECORD_ROTATION, { 50242585, 68333042 } },
  { DWG_TYPE_BLOCKVISIBILITYPARAMETER,
    RECORD_VISIBILITY,
    { 135625452, 184556386 } },
  { DWG_TYPE_BLOCKBASEPOINTPARAMETER, RECORD_POINT, { 82437801, 112294725 } },
  { DWG_TYPE_BLOCKFLIPGRIP, RECORD_FLIP_GRIP, { 575085, 763560 } },
  { DWG_TYPE_BLOCKALIGNMENTGRIP, RECORD_ALIGNMENT_GRIP, { 6886097, 9208651 } },
  { DWG_TYPE_BLOCKSTRETCHACTION, RECORD_OFFSET, { 6895636, 9291323 } },
  { DWG_TYPE_BLOCKMOVEACTION, RECORD_OFFSET, { 1553528, 2094574 } },
  /* only seen with a base point of (0, 0, 0): check_representation ()
     refuses others */
  { DWG_TYPE_BLOCKROTATEACTION, RECORD_BASE_POINT, { 4195796, 5656503 } },
  { DWG_TYPE_BLOCKSCALEACTION, RECORD_BASE_POINT, { 2551693, 3435815 } },
  { DWG_TYPE_BLOCKLINEARGRIP, RECORD_NONE, { 0, 0 } },
  { DWG_TYPE_BLOCKROTATIONGRIP, RECORD_NONE, { 0, 0 } },
  { DWG_TYPE_BLOCKVISIBILITYGRIP, RECORD_NONE, { 0, 0 } },
  { DWG_TYPE_BLOCKGRIPLOCATIONCOMPONENT, RECORD_NONE, { 0, 0 } },
  { DWG_TYPE_BLOCKFLIPACTION, RECORD_NONE, { 0, 0 } },
};

/* One placement being made. Objects are kept by handle, as adding objects
   may move dwg->object. */
typedef struct _dynblock_placement
{
  Dwg_Data *dwg;
  BITCODE_RLL owner;
  BITCODE_RLL def;
  BITCODE_RLL graph;
  BITCODE_RLL block; /* the block the INSERT draws: def, or the
                        representation block */
  BITCODE_RLL insert;
  BITCODE_3BD ins_pt;
  double scale;
  double rotation;
  Dwg_DynParam *params;
  BITCODE_BL num_params;
  /* for each property, the visibility state chosen, + 1, or 0 */
  BITCODE_BL *state;
  int at_defaults;
  /* for a representation block, the parameters' points the evaluation
     gives, and where it moves the ATTDEFs */
  Dynblock_State evaluated;
  char reason[REASON_SIZE];
} Dynblock_Placement;

static void ATTRIBUTE_FORMAT (2, 3)
    set_reason (Dynblock_Placement *p, const char *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  vsnprintf (p->reason, sizeof (p->reason), fmt, ap);
  va_end (ap);
}

static Dwg_Object *
handle_object (const Dwg_Data *dwg, const BITCODE_RLL handle)
{
  return handle ? dwg_resolve_handle (dwg, handle) : NULL;
}

static BITCODE_RLL
ref_handle (const Dwg_Object_Ref *ref)
{
  return ref ? ref->absolute_ref : 0;
}

static Dwg_Object *
ref_object (const Dwg_Data *dwg, const Dwg_Object_Ref *ref)
{
  return handle_object (dwg, ref_handle (ref));
}

static Dwg_Object_BLOCK_HEADER *
block_of (const Dwg_Data *dwg, const BITCODE_RLL handle)
{
  return handle_object (dwg, handle)->tio.object->tio.BLOCK_HEADER;
}

/* The object of blk's document whose BLOCK_HEADER blk is, or NULL. A copy
   of a block's struct links to the block's object too, but isn't it. */
static Dwg_Object *
block_object (const Dwg_Object_BLOCK_HEADER *blk)
{
  const Dwg_Data *dwg;
  Dwg_Object *obj;
  if (!blk || !blk->parent || !blk->parent->dwg)
    return NULL;
  dwg = blk->parent->dwg;
  if (blk->parent->objid >= dwg->num_objects)
    return NULL;
  obj = &dwg->object[blk->parent->objid];
  if (obj->fixedtype != DWG_TYPE_BLOCK_HEADER
      || obj->tio.object != blk->parent
      || obj->tio.object->tio.BLOCK_HEADER != blk)
    return NULL;
  return obj;
}

/* The copy, in dwg's codepage, as UTF-8. */
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

/*--------------------------------------------------------------------------------
 * Dictionaries
 */

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

/* Adds handle to the reactors of the common object data o. */
static int
add_reactor (Dwg_Data *dwg, Dwg_Object_Object *o, const BITCODE_RLL handle)
{
  BITCODE_H *reactors = (BITCODE_H *)realloc (
      o->reactors, (o->num_reactors + 1) * sizeof (BITCODE_H));
  if (!reactors)
    return DWG_ERR_OUTOFMEM;
  o->reactors = reactors;
  reactors[o->num_reactors] = dwg_add_handleref (dwg, 4, handle, NULL);
  if (!reactors[o->num_reactors])
    return DWG_ERR_OUTOFMEM;
  o->num_reactors++;
  return 0;
}

/* Gives obj owner, and with reactor set, owner as its reactor, as AutoCAD
   does for the objects a dictionary owns. */
static int
set_owner (Dwg_Data *dwg, Dwg_Object *obj, const BITCODE_RLL owner,
           const int reactor)
{
  obj->tio.object->ownerhandle = dwg_add_handleref (dwg, 4, owner, obj);
  if (!obj->tio.object->ownerhandle)
    return DWG_ERR_OUTOFMEM;
  return reactor ? add_reactor (dwg, obj->tio.object, owner) : 0;
}

/* A new hard-owner DICTIONARY owned by owner, as AutoCAD makes the
   dictionaries of a placement. Its handle goes into *handlep. */
static Dwg_Object_DICTIONARY *
new_dictionary (Dwg_Data *dwg, const BITCODE_RLL owner, const int reactor,
                BITCODE_RLL *handlep)
{
  int err;
  Dwg_Object_DICTIONARY *dict = dwg_add_DICTIONARY (dwg, NULL, NULL, 0);
  Dwg_Object *obj = dict ? dwg_obj_generic_to_object (dict, &err) : NULL;
  if (!obj || set_owner (dwg, obj, owner, reactor))
    return NULL;
  dict->is_hardowner = 1;
  dict->cloning = 1;
  *handlep = obj->handle.value;
  return dict;
}

/* Adds the entry name for handle to dict. */
static int
add_item (Dwg_Object_DICTIONARY *dict, const char *name,
          const BITCODE_RLL handle)
{
  return dwg_add_DICTIONARY_item (dict, name, handle) ? 0 : DWG_ERR_OUTOFMEM;
}

/* Sorts the entries of dict by name, as AutoCAD keeps them. */
static void
sort_items (Dwg_Object_DICTIONARY *dict)
{
  for (BITCODE_BL i = 1; i < dict->numitems; i++)
    for (BITCODE_BL j = i;
         j > 0 && strcmp (dict->texts[j - 1], dict->texts[j]) > 0; j--)
      {
        BITCODE_T text = dict->texts[j];
        BITCODE_H item = dict->itemhandles[j];
        dict->texts[j] = dict->texts[j - 1];
        dict->itemhandles[j] = dict->itemhandles[j - 1];
        dict->texts[j - 1] = text;
        dict->itemhandles[j - 1] = item;
      }
}

/* A new BLOCKREPRESENTATION, which the add API has no function for, made as
   it makes its objects: the next handle, and its class added to dwg when
   dwg lacks it. The class table functions come with write support. */
static Dwg_Object *
new_blockrepresentation (Dwg_Data *dwg)
{
#ifdef USE_WRITE
  static const char dxfname[] = "ACDB_BLOCKREPRESENTATION_DATA";
  const BITCODE_BS num_classes = dwg->num_classes;
  const BITCODE_BL idx = dwg->num_objects;
  Dwg_Object *obj;
  int rc;
  if (dwg_require_class (dwg, dxfname, sizeof (dxfname) - 1) < 500)
    return NULL;
  // a new class may have moved dwg->dwg_class, as dwg_copy_objects ()
  // allows for
  if (dwg->num_classes != num_classes)
    for (BITCODE_BL i = 0; i < dwg->num_objects; i++)
      if (dwg->object[i].type >= 500
          && dwg->object[i].type - 500u < dwg->num_classes)
        dwg->object[i].klass = &dwg->dwg_class[dwg->object[i].type - 500];
  rc = dwg_add_object (dwg);
  if (rc > 0)
    return NULL;
  if (rc < 0)
    dwg_resolve_objectrefs_silent (dwg);
  obj = &dwg->object[idx];
  obj->fixedtype = DWG_TYPE_BLOCKREPRESENTATION;
  obj->name = (char *)"BLOCKREPRESENTATION";
  obj->dxfname = (char *)dxfname;
  if (dwg_setup_BLOCKREPRESENTATION (obj))
    return NULL;
  (void)dwg_encode_get_class (dwg, obj);
  if (obj->type >= 500 && obj->type - 500u < dwg->num_classes)
    obj->klass = &dwg->dwg_class[obj->type - 500];
  dwg_set_next_objhandle (obj);
  obj->tio.object->is_xdic_missing = 1;
  return obj;
#else
  (void)dwg;
  return NULL;
#endif
}

/*--------------------------------------------------------------------------------
 * EED
 */

/* A new EED item with the given code, sized for its union member. */
static Dwg_Eed_Data *
eed_item (const BITCODE_RC code)
{
  Dwg_Eed_Data *data = (Dwg_Eed_Data *)calloc (1, sizeof (Dwg_Eed_Data));
  if (data)
    data->code = code;
  return data;
}

/* The handle of the APPID named name, added when dwg has none. */
static BITCODE_RLL
appid_handle (Dwg_Data *dwg, const char *name)
{
  BITCODE_H ref = dwg_find_tablehandle_silent (dwg, name, "APPID");
  Dwg_Object_APPID *appid;
  if (ref && ref->absolute_ref)
    return ref->absolute_ref;
  appid = dwg_add_APPID (dwg, name);
  return appid ? dwg_obj_generic_handlevalue (appid) : 0;
}

/* Sets the EED *eedp, of *num_eedp items, which holds none yet, to one
   group of the APPID with the given handle: a 1070, then a 1071 when
   has_index, then a 1005 handle. */
static int
set_tag_eed (Dwg_Eed **eedp, BITCODE_BL *num_eedp, const BITCODE_RLL appid,
             const BITCODE_RS value, const int has_index,
             const BITCODE_RL index, const BITCODE_RLL handle)
{
  const BITCODE_BL num_eed = has_index ? 3 : 2;
  Dwg_Eed *eed = (Dwg_Eed *)calloc (num_eed, sizeof (Dwg_Eed));
  BITCODE_BL n = 0;
  if (!eed)
    return DWG_ERR_OUTOFMEM;
  // each item's size: its code byte and its value
  eed[0].size = 3 + (has_index ? 5 : 0) + 9;
  eed[0].handle.code = 5;
  eed[0].handle.value = appid;
  dwg_set_handle_size (&eed[0].handle);
  eed[n].data = eed_item (70);
  if (eed[n].data)
    eed[n].data->u.eed_70.rs = value;
  n++;
  if (has_index)
    {
      eed[n].data = eed_item (71);
      if (eed[n].data)
        // Dwg_Eed_Data is packed
        memcpy (&eed[n].data->u.eed_71.rl, &index, sizeof (index));
      n++;
    }
  eed[n].data = eed_item (5);
  if (eed[n].data)
    memcpy (&eed[n].data->u.eed_5.entity, &handle, sizeof (handle));
  n++;
  for (BITCODE_BL i = 0; i < n; i++)
    if (!eed[i].data)
      {
        for (BITCODE_BL j = 0; j < n; j++)
          free (eed[j].data);
        free (eed);
        return DWG_ERR_OUTOFMEM;
      }
  *eedp = eed;
  *num_eedp = n;
  return 0;
}

/* The AcDbBlockRepETag index of entity obj into *indexp, and the value of
   the group's 1070 into *valuep. Returns 0 when it has no such group. */
static int
etag_index (const Dwg_Data *dwg, const Dwg_Object *obj, BITCODE_RS *valuep,
            BITCODE_RL *indexp)
{
  const Dwg_Object_Entity *ent = obj->tio.entity;
  int in_group = 0, found = 0;
  *valuep = 1;
  for (BITCODE_BL i = 0; ent->eed && i < ent->num_eed; i++)
    {
      const Dwg_Eed_Data *data = ent->eed[i].data;
      if (ent->eed[i].size)
        {
          const Dwg_Object *appid
              = handle_object (dwg, ent->eed[i].handle.value);
          in_group = appid && appid->fixedtype == DWG_TYPE_APPID
                     && appid->tio.object->tio.APPID->name
                     && strEQc (appid->tio.object->tio.APPID->name,
                                "AcDbBlockRepETag");
        }
      if (!in_group || !data)
        continue;
      if (data->code == 70)
        memcpy (valuep, &data->u.eed_70.rs, sizeof (*valuep));
      else if (data->code == 71)
        {
          memcpy (indexp, &data->u.eed_71.rl, sizeof (*indexp));
          found = 1;
        }
    }
  return found;
}

/* Appends to the EED of entity to a copy of the AcDbAttr group of attribute
   definition from, as AutoCAD's ATTRIBs carry their definition's. AutoCAD
   2007 and later keep an attribute's lock position in that group in files
   before r2007, which have no field for it: they write 1070 0, 1070 1 for
   a locked attribute and no group for another, and read 1070 0, 1070 0 as
   not locked too. An ATTRIB without the group is not locked, and AutoCAD
   may then leave it where it is when an edit of the placement moves its
   definition. Returns 0 when from has no such group. A group that isn't
   all 1070 items, the only form AutoCAD writes, or whose items don't hold
   all the bytes read for it, is left out, with DWG_ERR_INVALIDEED. */
static int
copy_attr_eed (const Dwg_Data *dwg, const Dwg_Object *from,
               Dwg_Object_Entity *to)
{
  const Dwg_Object_Entity *ent = from->tio.entity;
  const Dwg_Eed *group = NULL;
  BITCODE_BL first = 0, n, i;
  Dwg_Eed *eed;
  for (i = 0; !group && ent->eed && i < ent->num_eed; i++)
    if (ent->eed[i].size)
      {
        const Dwg_Object *appid
            = handle_object (dwg, ent->eed[i].handle.value);
        if (appid && appid->fixedtype == DWG_TYPE_APPID
            && appid->tio.object->tio.APPID->name
            && strEQc (appid->tio.object->tio.APPID->name, "AcDbAttr"))
          {
            group = &ent->eed[i];
            first = i;
          }
      }
  if (!group)
    return 0;
  // the group's items: its first, and those after it without a size
  for (n = 1; first + n < ent->num_eed && !ent->eed[first + n].size; n++)
    ;
  for (i = 0; i < n; i++)
    if (!ent->eed[first + i].data || ent->eed[first + i].data->code != 70)
      break;
  // a 1070 item takes 3 bytes in the file
  if (i < n || (group->raw && group->size != 3 * n))
    {
      LOG_WARN ("dwg_dynblock_insert: the AcDbAttr EED of attribute "
                "definition " FORMAT_HV " isn't 1070 items only, decoded "
                "in full, so its ATTRIB doesn't get it",
                from->handle.value);
      return DWG_ERR_INVALIDEED;
    }
  eed = (Dwg_Eed *)realloc (to->eed, (to->num_eed + n) * sizeof (Dwg_Eed));
  if (!eed)
    return DWG_ERR_OUTOFMEM;
  to->eed = eed;
  for (i = 0; i < n; i++)
    {
      Dwg_Eed *item = &eed[to->num_eed];
      memset (item, 0, sizeof (*item));
      item->data = eed_item (70);
      if (!item->data)
        return DWG_ERR_OUTOFMEM;
      // Dwg_Eed_Data is packed
      memcpy (&item->data->u.eed_70.rs, &ent->eed[first + i].data->u.eed_70.rs,
              sizeof (item->data->u.eed_70.rs));
      if (!i)
        {
          item->size = (BITCODE_BS)(3 * n);
          item->handle = group->handle;
        }
      to->num_eed++;
    }
  return 0;
}

/*--------------------------------------------------------------------------------
 * Checks before anything is added
 */

static const Dynblock_Node_Record *
node_record (const Dwg_Object_Type type)
{
  for (size_t i = 0; i < ARRAY_SIZE (node_records); i++)
    if (node_records[i].type == type)
      return &node_records[i];
  return NULL;
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

/* angle, in radians, in [0, 2 pi), as dynblock_eval.c reduces it */
static double
normal_angle (const double angle)
{
  double a = fmod (angle, 2.0 * M_PI);
  if (a < 0.0)
    a += 2.0 * M_PI;
  // a tiny negative angle plus 2 pi rounds to 2 pi
  return a < 2.0 * M_PI ? a : 0.0;
}

/* Whether value is within the limits of dp's value set and on its
   increment, counted from its minimum or 0, with tolerance eps. */
static int
is_within_value_set (const Dwg_DynParam *dp, const double value,
                     const double eps)
{
  if ((dp->value_flags & DWG_DYNPARAM_HAS_MINIMUM)
      && value < dp->minimum - eps)
    return 0;
  if ((dp->value_flags & DWG_DYNPARAM_HAS_MAXIMUM)
      && value > dp->maximum + eps)
    return 0;
  if ((dp->value_flags & DWG_DYNPARAM_HAS_INCREMENT) && dp->increment > 0.0)
    {
      const double base
          = dp->value_flags & DWG_DYNPARAM_HAS_MINIMUM ? dp->minimum : 0.0;
      const double steps = (value - base) / dp->increment;
      if (fabs (steps - round (steps)) * dp->increment > eps)
        return 0;
    }
  return 1;
}

/* Whether value, of a LINEAR or ROTATION property dp other than its
   definition's (an angle reduced to [0, 2 pi)), is one dp's value set
   allows: in its list, or with no list within its limits and on its
   increment, counted from its minimum or 0; an angle when one a whole
   turn away is, as an angle of 0 is at a maximum of 2 pi. AutoCAD moves a
   value it doesn't allow to one it does on its next edit of the placement.
   The tolerance is VALUE_EPSILON, or for a huge value 8 DBL_EPSILON times
   its size, 8 to 16 units in its last place. */
static int
is_allowed_value (const Dwg_DynParam *dp, const double value)
{
  const double eps = fmax (VALUE_EPSILON, 8.0 * DBL_EPSILON * fabs (value));
  if ((dp->value_flags & DWG_DYNPARAM_HAS_LIST) && dp->num_values)
    {
      for (BITCODE_BL i = 0; i < dp->num_values; i++)
        {
          double d = fabs (value - dp->values[i]);
          if (dp->kind == DWG_DYNPARAM_ROTATION)
            {
              d = fmod (d, 2.0 * M_PI);
              d = fmin (d, 2.0 * M_PI - d);
            }
          if (d <= eps)
            return 1;
        }
      return 0;
    }
  if (dp->kind == DWG_DYNPARAM_ROTATION)
    return is_within_value_set (dp, value, eps)
           || is_within_value_set (dp, value + 2.0 * M_PI, eps)
           || is_within_value_set (dp, value - 2.0 * M_PI, eps);
  return is_within_value_set (dp, value, eps);
}

/* Matches values to the definition's properties into p->state, and decides
   whether the placement is at the definition's values. */
static int
check_values (Dynblock_Placement *p, const Dwg_DynValue *values,
              const BITCODE_BL num_values)
{
  p->state = (BITCODE_BL *)calloc (p->num_params + 1, sizeof (BITCODE_BL));
  if (!p->state)
    return DWG_ERR_OUTOFMEM;
  p->at_defaults = 1;
  for (BITCODE_BL i = 0; i < num_values; i++)
    {
      const Dwg_DynValue *v = &values[i];
      const Dwg_DynParam *dp = NULL;
      BITCODE_BL j;
      for (j = 0; v->name && j < p->num_params; j++)
        if (strEQ (p->params[j].name, v->name))
          {
            dp = &p->params[j];
            break;
          }
      if (!dp)
        {
          set_reason (p, "no property is named '%s'",
                      v->name ? v->name : "(NULL)");
          return INSERT_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
        }
      for (BITCODE_BL k = 0; k < i; k++)
        if (values[k].name && strEQ (values[k].name, v->name))
          {
            set_reason (p, "property '%s' is given twice", v->name);
            return INSERT_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
          }
      if (dp->kind != DWG_DYNPARAM_VISIBILITY && !isfinite (v->value))
        {
          set_reason (p, "property '%s' is given %g", dp->name, v->value);
          return INSERT_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
        }
      switch (dp->kind)
        {
        case DWG_DYNPARAM_VISIBILITY:
          {
            BITCODE_BL s;
            for (s = 0; v->state && s < dp->num_states; s++)
              if (strEQ (dp->states[s], v->state))
                break;
            if (!v->state || s == dp->num_states)
              {
                set_reason (p, "visibility property '%s' has no state '%s'",
                            dp->name, v->state ? v->state : "(NULL)");
                return INSERT_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
              }
            p->state[j] = s + 1;
            // the first state is the definition's
            if (s)
              p->at_defaults = 0;
          }
          break;
        case DWG_DYNPARAM_FLIP:
          if (v->value != 0.0 && v->value != 1.0)
            {
              set_reason (p, "flip property '%s' is 0 or 1, not %g", dp->name,
                          v->value);
              return INSERT_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
            }
          // the evaluation refuses a flipped block
          if (v->value != 0.0)
            p->at_defaults = 0;
          break;
        case DWG_DYNPARAM_LINEAR:
          if (fabs (v->value - dp->default_value) <= VALUE_EPSILON)
            break;
          p->at_defaults = 0;
          if (!is_allowed_value (dp, v->value))
            {
              set_reason (p,
                          "linear property '%s': its value set doesn't "
                          "allow %g",
                          dp->name, v->value);
              return INSERT_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
            }
          break;
        case DWG_DYNPARAM_ROTATION:
          {
            // each reduced first: the difference of a huge value and the
            // default would lose the default
            const double a = normal_angle (v->value);
            double d = fabs (a - normal_angle (dp->default_value));
            if (d > M_PI)
              d = 2.0 * M_PI - d;
            if (d <= VALUE_EPSILON)
              break;
            p->at_defaults = 0;
            if (!is_allowed_value (dp, a))
              {
                set_reason (p,
                            "rotation property '%s': its value set doesn't "
                            "allow %g",
                            dp->name, v->value);
                return INSERT_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
              }
          }
          break;
        default:
          set_reason (p, "%s property '%s': setting it is not supported",
                      dwg_dynparam_kind_name (dp->kind), dp->name);
          return INSERT_REFUSED | DWG_ERR_NOTYETSUPPORTED;
        }
    }
  return 0;
}

/* Whether block blk, or a block it inserts, at any depth, inserts block
   target. */
static int
inserts_block (const Dwg_Data *dwg, const BITCODE_RLL blk,
               const BITCODE_RLL target, dwg_inthash *seen)
{
  const Dwg_Object *obj = handle_object (dwg, blk);
  const Dwg_Object_BLOCK_HEADER *_blk;
  if (!obj || obj->fixedtype != DWG_TYPE_BLOCK_HEADER
      || hash_get (seen, blk) != HASH_NOT_FOUND)
    return 0;
  hash_set (seen, blk, 1);
  _blk = obj->tio.object->tio.BLOCK_HEADER;
  for (BITCODE_BL i = 0; _blk->entities && i < _blk->num_owned; i++)
    {
      const Dwg_Object *ent = ref_object (dwg, _blk->entities[i]);
      BITCODE_RLL drawn = 0;
      if (!ent)
        continue;
      if (ent->fixedtype == DWG_TYPE_INSERT)
        drawn = ref_handle (ent->tio.entity->tio.INSERT->block_header);
      else if (ent->fixedtype == DWG_TYPE_MINSERT)
        drawn = ref_handle (ent->tio.entity->tio.MINSERT->block_header);
      if (drawn
          && (drawn == target || inserts_block (dwg, drawn, target, seen)))
        return 1;
    }
  return 0;
}

/* For a representation block: refuses a graph node whose record isn't
   known, a rotate or scale action whose base point isn't (0, 0, 0), as
   its record holds the base point and AutoCAD's has only been seen with
   (0, 0, 0), and an entity that owns a block of its own, which the
   evaluated copy would share with the definition's entity. */
static int
check_representation (Dynblock_Placement *p)
{
  Dwg_Data *dwg = p->dwg;
  const Dwg_Object_EVALUATION_GRAPH *graph
      = handle_object (dwg, p->graph)->tio.object->tio.EVALUATION_GRAPH;
  const Dwg_Object_BLOCK_HEADER *def = block_of (dwg, p->def);
  for (BITCODE_BL i = 0; graph->nodes && i < graph->num_nodes; i++)
    {
      const Dwg_Object *node = ref_object (dwg, graph->nodes[i].evalexpr);
      const BITCODE_3BD *base = NULL;
      if (node && !node_record (node->fixedtype))
        {
          set_reason (p,
                      "the placement data of an evaluation graph node of "
                      "class %s is not known",
                      class_name (dwg, node));
          return INSERT_REFUSED | DWG_ERR_NOTYETSUPPORTED;
        }
      if (node && node->fixedtype == DWG_TYPE_BLOCKROTATEACTION)
        base = &node->tio.object->tio.BLOCKROTATEACTION->base_pt;
      else if (node && node->fixedtype == DWG_TYPE_BLOCKSCALEACTION)
        base = &node->tio.object->tio.BLOCKSCALEACTION->base_pt;
      if (base && (base->x != 0.0 || base->y != 0.0 || base->z != 0.0))
        {
          set_reason (p,
                      "the placement data of %s " FORMAT_HV " with the base "
                      "point (%g, %g, %g) is not known",
                      node->name, node->handle.value, base->x, base->y,
                      base->z);
          return INSERT_REFUSED | DWG_ERR_NOTYETSUPPORTED;
        }
    }
  for (BITCODE_BL i = 0; def->entities && i < def->num_owned; i++)
    {
      const Dwg_Object *ent = ref_object (dwg, def->entities[i]);
      BITCODE_RLL own = 0;
      if (!ent)
        continue;
      switch (ent->fixedtype)
        {
        case DWG_TYPE_DIMENSION_ORDINATE:
        case DWG_TYPE_DIMENSION_LINEAR:
        case DWG_TYPE_DIMENSION_ALIGNED:
        case DWG_TYPE_DIMENSION_ANG3PT:
        case DWG_TYPE_DIMENSION_ANG2LN:
        case DWG_TYPE_DIMENSION_RADIUS:
        case DWG_TYPE_DIMENSION_DIAMETER:
        case DWG_TYPE_ARC_DIMENSION:
        case DWG_TYPE_LARGE_RADIAL_DIMENSION:
          // they share DIMENSION_COMMON
          own = ref_handle (ent->tio.entity->tio.DIMENSION_common->block);
          break;
        case DWG_TYPE_TABLE:
          own = ref_handle (ent->tio.entity->tio.TABLE->block_header);
          break;
        default:
          break;
        }
      if (own)
        {
          set_reason (p,
                      "%s " FORMAT_HV " draws a block of its own, which the "
                      "representation block would share with the "
                      "definition",
                      ent->name, ent->handle.value);
          return INSERT_REFUSED | DWG_ERR_NOTYETSUPPORTED;
        }
    }
  return 0;
}

/* Whether entity ent is an attribute definition that gets an ATTRIB: one
   that isn't constant. */
static int
is_placed_attdef (const Dwg_Object *ent)
{
  return ent && ent->fixedtype == DWG_TYPE_ATTDEF
         && !(ent->tio.entity->tio.ATTDEF->flags & 2);
}

/* The width atts give the value of the attribute tagged tag, or 0 when
   they give none: a width that isn't finite or is negative, which
   check_attributes () refuses, counts as none. */
static double
att_width (const Dwg_AttValue *atts, const BITCODE_BL num_atts,
           const char *tag)
{
  for (BITCODE_BL i = 0; i < num_atts; i++)
    if (atts[i].tag && strEQ (atts[i].tag, tag))
      return isfinite (atts[i].width) && atts[i].width > 0.0 ? atts[i].width
                                                             : 0.0;
  return 0.0;
}

/* The product of finite a, b and c, which is infinite only when it is
   beyond what a double holds: their exponents are added apart from their
   mantissas, so that no partial product overflows. */
static double
product3 (const double a, const double b, const double c)
{
  int ea, eb, ec;
  const double m = frexp (a, &ea) * frexp (b, &eb) * frexp (c, &ec);
  return ldexp (m, ea + eb + ec);
}

/* Moves *pt, the insertion point of attdef's ATTRIB placed by p, aligned
   at *align, to where AutoCAD starts text of the given width (as
   Dwg_AttValue has it) that is centred, right-aligned or middle: back
   from its alignment point along the baseline by the width at the
   ATTRIB's height and width factor, or half of it when centred or middle,
   forward for backward text, and across the baseline as far as the
   definition's text starts. That offset doesn't depend on the text,
   except for middle text, which AutoCAD also centres on the height of its
   characters. Without a width, for other text, or for text in a vertical
   style, which runs down instead, *pt stays where the definition's text
   starts. */
static void
start_at_width (const Dynblock_Placement *p, const Dwg_Entity_ATTDEF *attdef,
                const double width, BITCODE_3BD *pt, const BITCODE_3BD *align)
{
  const BITCODE_BS h = attdef->horiz_alignment;
  const double rotation = attdef->rotation + p->rotation;
  const double c = cos (rotation), s = sin (rotation);
  const Dwg_Object *style = ref_object (p->dwg, attdef->style);
  double across, share, width_factor, height;
  if (!(width > 0.0) || (h != 1 && h != 2 && h != 4)
      || attdef->vert_alignment > 3
      || (style && style->fixedtype == DWG_TYPE_STYLE
          && style->tio.object->tio.STYLE->is_vertical))
    return;
  across = (pt->y - align->y) * c - (pt->x - align->x) * s;
  // AutoCAD's width factor is positive; the add API leaves it 0
  width_factor = attdef->width_factor > 0.0 ? attdef->width_factor : 1.0;
  // the share of the width the text starts from its alignment point along
  // the baseline: back, or forward for backward text
  share = (h == 2 ? -1.0 : -0.5) * width_factor;
  if (attdef->generation & 2)
    share = -share;
  // the ATTRIB's height, which check_attributes () finds finite. Each
  // coordinate's part of the offset is multiplied out on its own, as it
  // can be held where the offset can't.
  height = attdef->height * p->scale;
  pt->x = align->x + product3 (width, share * c, height) - across * s;
  pt->y = align->y + product3 (width, share * s, height) + across * c;
}

/* The WCS point of block point (x, y, z), placed by p in the block with
   base point base. */
static void
place_point (const Dynblock_Placement *p, const BITCODE_3BD *base,
             const double x, const double y, const double z, BITCODE_3BD *out)
{
  const double dx = (x - base->x) * p->scale, dy = (y - base->y) * p->scale;
  const double c = cos (p->rotation), s = sin (p->rotation);
  out->x = p->ins_pt.x + dx * c - dy * s;
  out->y = p->ins_pt.y + dx * s + dy * c;
  out->z = p->ins_pt.z + (z - base->z) * p->scale;
}

/* The insertion and alignment points of attdef's ATTRIB, placed by p in
   the block with base point base, from attdef's points in the block,
   ins_pt and alignment_pt. Text left-aligned on the baseline doesn't use
   its alignment point: AutoCAD keeps it (0,0), which dwg_set_dataflags ()
   leaves out (dataflags 0x02). */
static void
place_attdef (const Dynblock_Placement *p, const BITCODE_3BD *base,
              const Dwg_Entity_ATTDEF *attdef, const BITCODE_2DPOINT *ins_pt,
              const BITCODE_2DPOINT *alignment_pt, BITCODE_3BD *pt,
              BITCODE_3BD *align)
{
  place_point (p, base, ins_pt->x, ins_pt->y, attdef->elevation, pt);
  if (!attdef->horiz_alignment && !attdef->vert_alignment)
    {
      align->x = align->y = 0.0;
      align->z = pt->z;
    }
  else
    place_point (p, base, alignment_pt->x, alignment_pt->y,
                 attdef->elevation, align);
}

/* Where the evaluation moves the definition's ATTDEF attdef, its
   representation block's copy: the moved points into *ins_ptp and
   *alignment_ptp, or attdef's own when it doesn't move it. */
static void
attdef_place (const Dynblock_Placement *p, const BITCODE_RLL handle,
              const Dwg_Entity_ATTDEF *attdef, BITCODE_2DPOINT *ins_ptp,
              BITCODE_2DPOINT *alignment_ptp)
{
  *ins_ptp = attdef->ins_pt;
  *alignment_ptp = attdef->alignment_pt;
  for (BITCODE_BL i = 0; i < p->evaluated.num_attdefs; i++)
    if (p->evaluated.attdefs[i].entity == handle)
      {
        *ins_ptp = p->evaluated.attdefs[i].ins_pt;
        *alignment_ptp = p->evaluated.attdefs[i].alignment_pt;
      }
}

/* Checks atts against def's attribute definitions: each tag names one, is
   given once, and has a value and a width that is finite and not
   negative. The definitions must lie in the XY plane, as the INSERT's
   transform is applied in it, have tags that dwg_add_ATTRIB () takes,
   which are fewer than AutoCAD allows, be placed at points, a height and a
   thickness that a double holds, and start, at the width given, where a
   double holds (start_at_width ()), so that a refusal comes before
   anything is added. The representation block's attribute definitions are
   def's, moved as the evaluation moves them. */
static int
check_attributes (Dynblock_Placement *p, const Dwg_AttValue *atts,
                  const BITCODE_BL num_atts)
{
  Dwg_Data *dwg = p->dwg;
  const Dwg_Object_BLOCK_HEADER *def = block_of (dwg, p->def);
  for (BITCODE_BL i = 0; def->entities && i < def->num_owned; i++)
    {
      const Dwg_Object *ent = ref_object (dwg, def->entities[i]);
      const Dwg_Entity_ATTDEF *attdef;
      BITCODE_3BD pt, align;
      BITCODE_2DPOINT ins_pt, alignment_pt;
      char *tag;
      int valid;
      if (!is_placed_attdef (ent))
        continue;
      attdef = ent->tio.entity->tio.ATTDEF;
      if (attdef->extrusion.x != 0.0 || attdef->extrusion.y != 0.0
          || attdef->extrusion.z != 1.0)
        {
          set_reason (p,
                      "attribute definition " FORMAT_HV " does not lie in "
                      "the XY plane: its extrusion is not (0, 0, 1)",
                      ent->handle.value);
          return INSERT_REFUSED | DWG_ERR_NOTYETSUPPORTED;
        }
      attdef_place (p, ent->handle.value, attdef, &ins_pt, &alignment_pt);
      place_attdef (p, &def->base_pt, attdef, &ins_pt, &alignment_pt, &pt,
                    &align);
      if (!isfinite (pt.x) || !isfinite (pt.y) || !isfinite (pt.z)
          || !isfinite (align.x) || !isfinite (align.y)
          || !isfinite (attdef->height * p->scale)
          || !isfinite (attdef->thickness * p->scale))
        {
          set_reason (p,
                      "attribute definition " FORMAT_HV " placed at a scale "
                      "of %g has a point, height or thickness beyond what "
                      "a double holds",
                      ent->handle.value, p->scale);
          return INSERT_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
        }
      tag = utf8_copy (dwg, attdef->tag);
      if (!tag)
        return DWG_ERR_OUTOFMEM;
      // where the value's width given starts the text
      {
        const double width = att_width (atts, num_atts, tag);
        start_at_width (p, attdef, width, &pt, &align);
        if (!isfinite (pt.x) || !isfinite (pt.y))
          {
            set_reason (p,
                        "attribute definition " FORMAT_HV " placed at a "
                        "scale of %g starts beyond what a double holds at "
                        "the width given, %g",
                        ent->handle.value, p->scale, width);
            free (tag);
            return INSERT_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
          }
      }
      valid = dwg_is_valid_tag (tag);
      if (!valid)
        set_reason (p,
                    "attribute definition " FORMAT_HV " has the tag '%s', "
                    "which dwg_add_ATTRIB () refuses",
                    ent->handle.value, tag);
      free (tag);
      if (!valid)
        return INSERT_REFUSED | DWG_ERR_NOTYETSUPPORTED;
    }
  for (BITCODE_BL i = 0; i < num_atts; i++)
    {
      int found = 0;
      if (!atts[i].tag || !atts[i].value)
        {
          set_reason (p, "attribute %u has no %s", (unsigned)i,
                      atts[i].tag ? "value" : "tag");
          return INSERT_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
        }
      if (!isfinite (atts[i].width) || atts[i].width < 0.0)
        {
          set_reason (p,
                      "attribute '%s' has a width of %g, which isn't a "
                      "finite number of 0 or more",
                      atts[i].tag, atts[i].width);
          return INSERT_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
        }
      for (BITCODE_BL k = 0; k < i; k++)
        if (strEQ (atts[k].tag, atts[i].tag))
          {
            set_reason (p, "attribute '%s' is given twice", atts[i].tag);
            return INSERT_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
          }
      for (BITCODE_BL k = 0; !found && def->entities && k < def->num_owned;
           k++)
        {
          const Dwg_Object *ent = ref_object (dwg, def->entities[k]);
          char *tag;
          if (!is_placed_attdef (ent))
            continue;
          tag = utf8_copy (dwg, ent->tio.entity->tio.ATTDEF->tag);
          if (!tag)
            return DWG_ERR_OUTOFMEM;
          found = strEQ (tag, atts[i].tag);
          free (tag);
        }
      if (!found)
        {
          set_reason (p,
                      "the block has no attribute definition tagged '%s' "
                      "that isn't constant",
                      atts[i].tag);
          return INSERT_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
        }
    }
  return 0;
}

/*--------------------------------------------------------------------------------
 * The representation block
 */

/* Lists INSERT or MINSERT obj among the INSERTs of the block it draws, as
   dwg_add_INSERT () lists the INSERTs it adds. */
static int
list_insert (Dwg_Data *dwg, const Dwg_Object *obj)
{
  const BITCODE_H ref = obj->fixedtype == DWG_TYPE_INSERT
                            ? obj->tio.entity->tio.INSERT->block_header
                            : obj->tio.entity->tio.MINSERT->block_header;
  Dwg_Object *blkobj = ref_object (dwg, ref);
  Dwg_Object_BLOCK_HEADER *blk;
  BITCODE_H *inserts;
  if (!blkobj || blkobj->fixedtype != DWG_TYPE_BLOCK_HEADER)
    return 0;
  blk = blkobj->tio.object->tio.BLOCK_HEADER;
  for (BITCODE_BL i = 0; blk->inserts && i < blk->num_inserts; i++)
    if (ref_handle (blk->inserts[i]) == obj->handle.value)
      return 0;
  inserts = (BITCODE_H *)realloc (blk->inserts,
                                  (blk->num_inserts + 1) * sizeof (BITCODE_H));
  if (!inserts)
    return DWG_ERR_OUTOFMEM;
  blk->inserts = inserts;
  inserts[blk->num_inserts]
      = dwg_add_handleref (dwg, 4, obj->handle.value, NULL);
  if (!inserts[blk->num_inserts])
    return DWG_ERR_OUTOFMEM;
  blk->num_inserts++;
  return 0;
}

/* A map from each of def's entities to the representation block's entity
   at the same place: the evaluated block holds a copy of each of def's
   entities, in def's order. */
static Dwg_Copy_Map *
entity_map (const Dwg_Object_BLOCK_HEADER *def,
            const Dwg_Object_BLOCK_HEADER *rep)
{
  Dwg_Copy_Map *map = dwg_copy_map_new ();
  if (!map)
    return NULL;
  for (BITCODE_BL i = 0; i < def->num_owned && i < rep->num_owned; i++)
    if (ref_handle (def->entities[i]) && ref_handle (rep->entities[i]))
      dwg_copy_map_set (map, ref_handle (def->entities[i]),
                        ref_handle (rep->entities[i]));
  return map;
}

/* An entity's sort key, by which a block draws it, and its place in the
   block. */
typedef struct
{
  BITCODE_RLL key;
  BITCODE_BL index;
} Draw_Key;

static int
compare_draw_keys (const void *a, const void *b)
{
  const Draw_Key *x = (const Draw_Key *)a;
  const Draw_Key *y = (const Draw_Key *)b;
  if (x->key != y->key)
    return x->key < y->key ? -1 : 1;
  return x->index < y->index ? -1 : x->index > y->index;
}

static int
compare_handles (const void *a, const void *b)
{
  const BITCODE_RLL x = *(const BITCODE_RLL *)a;
  const BITCODE_RLL y = *(const BITCODE_RLL *)b;
  return x < y ? -1 : x > y;
}

/* Sets the entries of the representation block's sort table, rep_table, so
   that the block draws its copies in the order def draws its entities.
   A block draws an entity by its sort key: the key of the entity's entry
   in the table, or else the entity's own handle. A key is a handle value
   that need not name an entity of the block (it may be an erased
   entity's), so mapping the keys to the copies' handles could reorder
   them. Instead the copies' own handles, in ascending order, are handed
   out as keys in def's draw order. A copy gets an entry when def's table
   lists its entity or when its key isn't its own handle; def's entries
   for entities of other blocks have no copy and are left out. */
static int
order_sortents (Dynblock_Placement *p, const BITCODE_RLL def_table,
                const BITCODE_RLL rep_table)
{
  Dwg_Data *dwg = p->dwg;
  const Dwg_Object_BLOCK_HEADER *def = block_of (dwg, p->def);
  const Dwg_Object_BLOCK_HEADER *rep = block_of (dwg, p->block);
  const Dwg_Object_SORTENTSTABLE *dt
      = handle_object (dwg, def_table)->tio.object->tio.SORTENTSTABLE;
  Dwg_Object_SORTENTSTABLE *rt
      = handle_object (dwg, rep_table)->tio.object->tio.SORTENTSTABLE;
  const BITCODE_BL n
      = def->num_owned < rep->num_owned ? def->num_owned : rep->num_owned;
  Draw_Key *order = (Draw_Key *)calloc (n ? n : 1, sizeof (Draw_Key));
  BITCODE_RLL *handles = (BITCODE_RLL *)calloc (n ? n : 1, sizeof (*handles));
  BITCODE_RLL *keys = (BITCODE_RLL *)calloc (n ? n : 1, sizeof (*keys));
  char *listed = (char *)calloc (n ? n : 1, 1);
  BITCODE_H *ents = NULL, *sort_ents = NULL;
  BITCODE_BL num_order = 0, num = 0;
  int error = DWG_ERR_OUTOFMEM;

  if (!order || !handles || !keys || !listed)
    goto done;
  // def's draw order, and the copies' handles
  for (BITCODE_BL i = 0; i < n; i++)
    {
      const BITCODE_RLL def_h = ref_handle (def->entities[i]);
      const BITCODE_RLL copy_h = ref_handle (rep->entities[i]);
      BITCODE_RLL key = def_h;
      if (!def_h || !copy_h)
        continue;
      for (BITCODE_BL j = 0; dt->ents && dt->sort_ents && j < dt->num_ents;
           j++)
        if (ref_handle (dt->ents[j]) == def_h)
          {
            key = ref_handle (dt->sort_ents[j]);
            listed[i] = 1;
            break;
          }
      order[num_order].key = key;
      order[num_order].index = i;
      handles[num_order] = copy_h;
      num_order++;
    }
  qsort (order, num_order, sizeof (Draw_Key), compare_draw_keys);
  qsort (handles, num_order, sizeof (BITCODE_RLL), compare_handles);
  for (BITCODE_BL k = 0; k < num_order; k++)
    keys[order[k].index] = handles[k];
  for (BITCODE_BL i = 0; i < n; i++)
    if (keys[i] && (listed[i] || keys[i] != ref_handle (rep->entities[i])))
      num++;
  if (num)
    {
      ents = (BITCODE_H *)calloc (num, sizeof (BITCODE_H));
      sort_ents = (BITCODE_H *)calloc (num, sizeof (BITCODE_H));
      if (!ents || !sort_ents)
        goto done;
    }
  num = 0;
  // in the block's order
  for (BITCODE_BL i = 0; i < n; i++)
    {
      const BITCODE_RLL copy_h = ref_handle (rep->entities[i]);
      if (!keys[i] || !(listed[i] || keys[i] != copy_h))
        continue;
      ents[num] = dwg_add_handleref (dwg, 4, copy_h, NULL);
      sort_ents[num] = dwg_add_handleref (dwg, 0, keys[i], NULL);
      if (!ents[num] || !sort_ents[num])
        goto done;
      num++;
    }
  free (rt->ents);
  free (rt->sort_ents);
  rt->ents = ents;
  rt->sort_ents = sort_ents;
  rt->num_ents = num;
  ents = sort_ents = NULL;
  error = 0;
done:
  free (ents);
  free (sort_ents);
  free (order);
  free (handles);
  free (keys);
  free (listed);
  if (error)
    set_reason (p, "ordering the sort table failed (0x%x)", (unsigned)error);
  return error;
}

/* Copies def's ACAD_SORTENTS table, the draw order, into a new extension
   dictionary of the representation block, with entries for the copies
   that keep def's draw order (order_sortents ()). AutoCAD gives a
   representation block one exactly when its definition has one. */
static int
copy_sortents (Dynblock_Placement *p, const Dwg_Object *sortents)
{
  Dwg_Data *dwg = p->dwg;
  const BITCODE_RLL source = sortents->handle.value;
  const BITCODE_RLL def_xdic
      = ref_handle (object_xdic (handle_object (dwg, p->def)));
  Dwg_Object_DICTIONARY *xdic;
  Dwg_Object *repobj;
  Dwg_Copy_Map *map;
  Dwg_Copy_Unresolved *unresolved = NULL;
  BITCODE_BL num_unresolved = 0;
  BITCODE_RLL xdic_handle, copy;
  int error;

  xdic = new_dictionary (dwg, p->block, 0, &xdic_handle);
  if (!xdic)
    return DWG_ERR_OUTOFMEM;
  repobj = handle_object (dwg, p->block);
  repobj->tio.object->xdicobjhandle
      = dwg_add_handleref (dwg, 3, xdic_handle, NULL);
  repobj->tio.object->is_xdic_missing = 0;
  map = entity_map (block_of (dwg, p->def), block_of (dwg, p->block));
  if (!map)
    return DWG_ERR_OUTOFMEM;
  dwg_copy_map_set (map, p->def, p->block);
  dwg_copy_map_set (map, def_xdic, xdic_handle);
  error = dwg_copy_objects (dwg, dwg, &source, 1, map, &unresolved,
                            &num_unresolved);
  free (unresolved);
  copy = dwg_copy_map_get (map, source);
  dwg_copy_map_free (map);
  if (error >= DWG_ERR_CRITICAL || !copy)
    {
      set_reason (p, "copying the sort table failed (0x%x)",
                  (unsigned)error);
      return error ? error : DWG_ERR_INTERNALERROR;
    }
  {
    const int order_error = order_sortents (p, source, copy);
    if (order_error)
      return order_error | error;
  }
  // xdic's struct doesn't move with dwg->object
  return add_item (xdic, "ACAD_SORTENTS", copy) | error;
}

/* Gives each IMAGE and WIPEOUT of the representation block an image
   definition reactor of its own: a reactor is owned by its entity, and the
   evaluated copy still names the definition entity's. */
static int
copy_image_reactors (Dynblock_Placement *p)
{
  Dwg_Data *dwg = p->dwg;
  const Dwg_Object_BLOCK_HEADER *def = block_of (dwg, p->def);
  const Dwg_Object_BLOCK_HEADER *rep = block_of (dwg, p->block);
  int error = 0;
  for (BITCODE_BL i = 0; i < def->num_owned && i < rep->num_owned; i++)
    {
      const BITCODE_RLL source = ref_handle (def->entities[i]);
      const BITCODE_RLL target = ref_handle (rep->entities[i]);
      Dwg_Object *copy = handle_object (dwg, target);
      BITCODE_H *reactorp, imagedef;
      BITCODE_RLL reactor, new_reactor;
      Dwg_Copy_Map *map;
      Dwg_Copy_Unresolved *unresolved = NULL;
      BITCODE_BL num_unresolved = 0;
      Dwg_Object *defobj;
      if (!copy)
        continue;
      // IMAGE and WIPEOUT share their fields
      if (copy->fixedtype == DWG_TYPE_IMAGE)
        {
          reactorp = &copy->tio.entity->tio.IMAGE->imagedefreactor;
          imagedef = copy->tio.entity->tio.IMAGE->imagedef;
        }
      else if (copy->fixedtype == DWG_TYPE_WIPEOUT)
        {
          reactorp = &copy->tio.entity->tio.WIPEOUT->imagedefreactor;
          imagedef = copy->tio.entity->tio.WIPEOUT->imagedef;
        }
      else
        continue;
      reactor = ref_handle (*reactorp);
      if (!reactor || !handle_object (dwg, reactor))
        continue;
      map = dwg_copy_map_new ();
      if (!map)
        return error | DWG_ERR_OUTOFMEM;
      dwg_copy_map_set (map, source, target);
      error |= dwg_copy_objects (dwg, dwg, &reactor, 1, map, &unresolved,
                                 &num_unresolved);
      free (unresolved);
      new_reactor = dwg_copy_map_get (map, reactor);
      dwg_copy_map_free (map);
      if (error >= DWG_ERR_CRITICAL || !new_reactor)
        {
          set_reason (p, "copying the image definition reactor of " FORMAT_HV
                      " failed (0x%x)",
                      source, (unsigned)error);
          return error ? error : DWG_ERR_INTERNALERROR;
        }
      // dwg->object may have moved
      copy = handle_object (dwg, target);
      reactorp = copy->fixedtype == DWG_TYPE_IMAGE
                     ? &copy->tio.entity->tio.IMAGE->imagedefreactor
                     : &copy->tio.entity->tio.WIPEOUT->imagedefreactor;
      *reactorp = dwg_add_handleref (dwg, 3, new_reactor, NULL);
      if (!*reactorp)
        return error | DWG_ERR_OUTOFMEM;
      // the image definition lists its reactors
      defobj = ref_object (dwg, imagedef);
      if (defobj && defobj->supertype == DWG_SUPERTYPE_OBJECT)
        error |= add_reactor (dwg, defobj->tio.object, new_reactor);
    }
  return error;
}

/* Gives the evaluated block what AutoCAD's representation blocks have and
   the evaluation leaves out: AcDbBlockRepBTag EED naming the definition,
   the definition's hasattrs flag and sort table, its nested INSERTs listed
   by the blocks they draw, and image reactors of its own. */
static int
complete_representation (Dynblock_Placement *p)
{
  Dwg_Data *dwg = p->dwg;
  Dwg_Object_BLOCK_HEADER *rep = block_of (dwg, p->block);
  const Dwg_Object_BLOCK_HEADER *def = block_of (dwg, p->def);
  // first, as adding the APPID may move dwg->object
  const BITCODE_RLL btag = appid_handle (dwg, "AcDbBlockRepBTag");
  const Dwg_Object *sortents;
  int error;

  if (!btag)
    return DWG_ERR_OUTOFMEM;
  if (rep->num_owned != def->num_owned)
    {
      set_reason (p, "the evaluated block has %u entities, the definition %u",
                  (unsigned)rep->num_owned, (unsigned)def->num_owned);
      return DWG_ERR_INTERNALERROR;
    }
  rep->hasattrs = def->hasattrs;
  {
    Dwg_Object_Object *o = handle_object (dwg, p->block)->tio.object;
    error = set_tag_eed (&o->eed, &o->num_eed, btag, 1, 0, 0, p->def);
  }
  for (BITCODE_BL i = 0; !error && i < rep->num_owned; i++)
    {
      const Dwg_Object *ent = ref_object (dwg, rep->entities[i]);
      if (ent
          && (ent->fixedtype == DWG_TYPE_INSERT
              || ent->fixedtype == DWG_TYPE_MINSERT))
        error = list_insert (dwg, ent);
    }
  sortents = xdic_item (dwg, handle_object (dwg, p->def), "ACAD_SORTENTS");
  if (!error && sortents && sortents->fixedtype == DWG_TYPE_SORTENTSTABLE)
    error = copy_sortents (p, sortents);
  if (error < DWG_ERR_CRITICAL)
    error |= copy_image_reactors (p);
  return error;
}

/*--------------------------------------------------------------------------------
 * The stored values
 */

static int
record_point (Dwg_Object_XRECORD *xrec, const BITCODE_3BD *pt)
{
  return dwg_add_XRECORD_pointd3d (xrec, 10, pt) ? 0 : DWG_ERR_OUTOFMEM;
}

/* Appends a string item with the bytes of str, which is in the document's
   codepage, as the decoder reads one. */
static int
record_string (const Dwg_Data *dwg, Dwg_Object_XRECORD *xrec, const short dxf,
               const char *str)
{
  const size_t len = str ? strlen (str) : 0;
  Dwg_Resbuf *rb = (Dwg_Resbuf *)calloc (1, sizeof (Dwg_Resbuf));
  Dwg_Resbuf *last = xrec->xdata;
  if (!rb)
    return DWG_ERR_OUTOFMEM;
  rb->type = dxf;
  rb->value.str.codepage = dwg->header.codepage;
  rb->value.str.size = (BITCODE_RS)len;
  rb->value.str.u.data = (char *)malloc (len + 1);
  if (!rb->value.str.u.data)
    {
      free (rb);
      return DWG_ERR_OUTOFMEM;
    }
  memcpy (rb->value.str.u.data, str ? str : "", len + 1);
  while (last && last->nextrb)
    last = last->nextrb;
  if (last)
    last->nextrb = rb;
  else
    xrec->xdata = rb;
  xrec->num_xdata++;
  // the group code, the length, the codepage and the bytes
  xrec->xdata_size += 2 + 2 + 1 + (BITCODE_BL)len;
  return 0;
}

/* Point i of the parameter node as the evaluation leaves it, or def, the
   definition's, for a parameter the evaluation gives no points. */
static const BITCODE_3BD *
evaluated_point (const Dynblock_Placement *p, const BITCODE_RLL node,
                 const BITCODE_BL i, const BITCODE_3BD *def)
{
  for (BITCODE_BL j = 0; j < p->evaluated.num_nodes; j++)
    if (p->evaluated.nodes[j].handle == node
        && i < p->evaluated.nodes[j].num_points)
      return &p->evaluated.nodes[j].points[i];
  return def;
}

/* Adds the ACAD_ENHANCEDBLOCKDATA XRECORD of graph node node, of class
   record rec, to data, the dictionary with handle data_handle. A
   parameter's points are those the evaluation leaves it, as AutoCAD
   stores them; an action's record is the same at every value. */
static int
add_node_record (Dynblock_Placement *p, Dwg_Object_DICTIONARY *data,
                 const BITCODE_RLL data_handle, const BITCODE_RLL node_handle,
                 const Dynblock_Node_Record *rec)
{
  Dwg_Data *dwg = p->dwg;
  const Dwg_Object *node = handle_object (dwg, node_handle);
  const Dynblock_Node *element
      = (const Dynblock_Node *)node->tio.object->tio.BLOCKVISIBILITYPARAMETER;
  static const BITCODE_3BD normal = { 0.0, 0.0, -1.0 };
  Dwg_Object_XRECORD *xrec;
  Dwg_Object *xobj;
  char key[16];
  int error, err;

  snprintf (key, sizeof (key), "%u", (unsigned)element->evalexpr.nodeid);
  xrec = dwg_add_XRECORD (data, key);
  xobj = xrec ? dwg_obj_generic_to_object (xrec, &err) : NULL;
  if (!xobj)
    return DWG_ERR_OUTOFMEM;
  error = set_owner (dwg, xobj, data_handle, 1);
  // the add API may have moved dwg->object
  node = handle_object (dwg, node_handle);
  if (!dwg_add_XRECORD_int32 (xrec, 1071, rec->id[0])
      || !dwg_add_XRECORD_int32 (xrec, 1071, rec->id[1])
      || !dwg_add_XRECORD_int16 (xrec, 70, (BITCODE_BS)element->be_major)
      || !dwg_add_XRECORD_int16 (xrec, 70, (BITCODE_BS)element->be_minor))
    return DWG_ERR_OUTOFMEM;
  switch (rec->kind)
    {
    case RECORD_TWO_POINTS:
    case RECORD_FLIP:
    case RECORD_ROTATION:
      {
        // the two-point parameters share BLOCK2PTPARAMETER_fields
        const Dynblock_2Pt_Param *_p = (const Dynblock_2Pt_Param *)element;
        error |= record_point (
            xrec, evaluated_point (p, node_handle, 0, &_p->def_basept));
        error |= record_point (
            xrec, evaluated_point (p, node_handle, 1, &_p->def_endpt));
        // AutoCAD writes (0, 0, -1) here
        error |= record_point (xrec, &normal);
        if (rec->kind == RECORD_FLIP)
          error |= dwg_add_XRECORD_int16 (xrec, 70, 0) ? 0 : DWG_ERR_OUTOFMEM;
        // the base angle point, which the evaluation leaves where it is
        else if (rec->kind == RECORD_ROTATION)
          error |= record_point (xrec, &node->tio.object->tio
                                            .BLOCKROTATIONPARAMETER
                                            ->def_base_angle_pt);
      }
      break;
    case RECORD_VISIBILITY:
      {
        const Dwg_Object_BLOCKVISIBILITYPARAMETER *_vis
            = node->tio.object->tio.BLOCKVISIBILITYPARAMETER;
        BITCODE_BL state = 0;
        for (BITCODE_BL j = 0; j < p->num_params; j++)
          if (p->params[j].handle == node_handle && p->state[j])
            state = p->state[j] - 1;
        error |= record_point (
            xrec, evaluated_point (p, node_handle, 0, &_vis->def_pt));
        error |= record_string (
            dwg, xrec, 1,
            _vis->states && state < _vis->num_states ? _vis->states[state].name
                                                     : NULL);
      }
      break;
    case RECORD_POINT:
      error |= record_point (
          xrec,
          evaluated_point (
              p, node_handle, 0,
              &node->tio.object->tio.BLOCKBASEPOINTPARAMETER->def_pt));
      break;
    case RECORD_FLIP_GRIP:
      if (!dwg_add_XRECORD_int16 (xrec, 70, 0)
          || !dwg_add_XRECORD_int16 (xrec, 70, 0))
        error |= DWG_ERR_OUTOFMEM;
      break;
    case RECORD_ALIGNMENT_GRIP:
      if (!dwg_add_XRECORD_int16 (xrec, 70, 0))
        error |= DWG_ERR_OUTOFMEM;
      break;
    case RECORD_OFFSET:
      {
        const double offset
            = node->fixedtype == DWG_TYPE_BLOCKSTRETCHACTION
                  ? node->tio.object->tio.BLOCKSTRETCHACTION->action_offset_y
                  : node->tio.object->tio.BLOCKMOVEACTION->action_offset_y;
        if (!dwg_add_XRECORD_real (xrec, 40, offset))
          error |= DWG_ERR_OUTOFMEM;
      }
      break;
    case RECORD_BASE_POINT:
      error |= record_point (
          xrec, node->fixedtype == DWG_TYPE_BLOCKROTATEACTION
                    ? &node->tio.object->tio.BLOCKROTATEACTION->base_pt
                    : &node->tio.object->tio.BLOCKSCALEACTION->base_pt);
      break;
    case RECORD_NONE:
    default:
      break;
    }
  return error;
}

/* Gives the INSERT the extension dictionary AutoCAD stores a placement's
   values in: AcDbBlockRepresentation, holding AcDbRepData, which names the
   definition, and AppDataCache, holding ACAD_ENHANCEDBLOCKDATA with a
   record for each graph node AutoCAD keeps one of. */
static int
add_representation_data (Dynblock_Placement *p)
{
  Dwg_Data *dwg = p->dwg;
  Dwg_Object_DICTIONARY *xdic, *rep, *cache, *data;
  BITCODE_RLL xdic_h, rep_h, cache_h, data_h, repdata_h;
  Dwg_Object *obj;
  const Dwg_Object_EVALUATION_GRAPH *graph;
  int error;

  xdic = new_dictionary (dwg, p->insert, 0, &xdic_h);
  if (!xdic)
    return DWG_ERR_OUTOFMEM;
  obj = handle_object (dwg, p->insert);
  obj->tio.entity->xdicobjhandle = dwg_add_handleref (dwg, 3, xdic_h, NULL);
  obj->tio.entity->is_xdic_missing = 0;
  rep = new_dictionary (dwg, xdic_h, 1, &rep_h);
  if (!rep || add_item (xdic, "AcDbBlockRepresentation", rep_h))
    return DWG_ERR_OUTOFMEM;

  obj = new_blockrepresentation (dwg);
  if (!obj)
    return DWG_ERR_OUTOFMEM;
  repdata_h = obj->handle.value;
  error = set_owner (dwg, obj, rep_h, 1);
  obj->tio.object->tio.BLOCKREPRESENTATION->flag = 1;
  obj->tio.object->tio.BLOCKREPRESENTATION->block
      = dwg_add_handleref (dwg, 5, p->def, NULL);
  if (error || add_item (rep, "AcDbRepData", repdata_h))
    return DWG_ERR_OUTOFMEM;

  cache = new_dictionary (dwg, rep_h, 1, &cache_h);
  if (!cache || add_item (rep, "AppDataCache", cache_h))
    return DWG_ERR_OUTOFMEM;
  data = new_dictionary (dwg, cache_h, 1, &data_h);
  if (!data || add_item (cache, "ACAD_ENHANCEDBLOCKDATA", data_h))
    return DWG_ERR_OUTOFMEM;

  graph = handle_object (dwg, p->graph)->tio.object->tio.EVALUATION_GRAPH;
  for (BITCODE_BL i = 0; !error && graph->nodes && i < graph->num_nodes; i++)
    {
      const BITCODE_RLL node = ref_handle (graph->nodes[i].evalexpr);
      const Dwg_Object *nodeobj = handle_object (dwg, node);
      const Dynblock_Node_Record *rec
          = nodeobj ? node_record (nodeobj->fixedtype) : NULL;
      if (rec && rec->kind != RECORD_NONE)
        error = add_node_record (p, data, data_h, node, rec);
      // dwg->object may have moved
      graph = handle_object (dwg, p->graph)->tio.object->tio.EVALUATION_GRAPH;
    }
  sort_items (data);
  return error;
}

/*--------------------------------------------------------------------------------
 * Attributes
 */

/* Gives entity to the layer, colour, linetype, lineweight and plot style
   of entity from. */
static int
copy_entity_properties (Dwg_Data *dwg, const Dwg_Object_Entity *from,
                        Dwg_Object_Entity *to)
{
  to->layer = from->layer
                  ? dwg_add_handleref (dwg, 5, from->layer->absolute_ref, NULL)
                  : NULL;
  to->color.index = from->color.index;
  to->color.flag = from->color.flag;
  to->color.rgb = from->color.rgb;
  to->color.method = from->color.method;
  to->color.alpha_raw = from->color.alpha_raw;
  to->color.alpha_type = from->color.alpha_type;
  to->color.alpha = from->color.alpha;
  to->color.name = from->color.name ? strdup (from->color.name) : NULL;
  to->color.book_name
      = from->color.book_name ? strdup (from->color.book_name) : NULL;
  to->color.handle = from->color.handle
                         ? dwg_add_handleref (dwg, 5,
                                              from->color.handle->absolute_ref,
                                              NULL)
                         : NULL;
  to->ltype_flags = from->ltype_flags;
  to->ltype = from->ltype
                  ? dwg_add_handleref (dwg, 5, from->ltype->absolute_ref, NULL)
                  : NULL;
  to->ltype_scale = from->ltype_scale;
  to->linewt = from->linewt;
  to->plotstyle_flags = from->plotstyle_flags;
  to->plotstyle = from->plotstyle ? dwg_add_handleref (
                                        dwg, 5, from->plotstyle->absolute_ref,
                                        NULL)
                                  : NULL;
  if ((from->color.name && !to->color.name)
      || (from->color.book_name && !to->color.book_name))
    return DWG_ERR_OUTOFMEM;
  return 0;
}

/* Gives the INSERT an ATTRIB for each of its block's attribute definitions
   that isn't constant, in the block's order, as AutoCAD's INSERT does: the
   definition's text properties, with the placement's transform applied,
   and the value from atts, or the definition's default, starting where the
   value's width in atts moves it (start_at_width ()). With etags, each
   ATTRIB gets its definition's AcDbBlockRepETag index, as AutoCAD's do on
   a placement at the definition's values; then each gets a copy of its
   definition's AcDbAttr EED (copy_attr_eed ()). */
static int
add_attributes (Dynblock_Placement *p, const Dwg_AttValue *atts,
                const BITCODE_BL num_atts, const int etags)
{
  Dwg_Data *dwg = p->dwg;
  Dwg_Entity_INSERT *insert
      = handle_object (dwg, p->insert)->tio.entity->tio.INSERT;
  const BITCODE_RLL etag_appid
      = etags ? appid_handle (dwg, "AcDbBlockRepETag") : 0;
  const Dwg_Object_BLOCK_HEADER *blk = block_of (dwg, p->block);
  const BITCODE_BL num_owned = blk->num_owned;
  BITCODE_RLL *attdefs;
  BITCODE_3BD base = blk->base_pt;
  BITCODE_BL n = 0;
  int error = 0;

  if (etags && !etag_appid)
    return DWG_ERR_OUTOFMEM;
  // the block's entity list doesn't move, but the objects may
  attdefs = (BITCODE_RLL *)calloc (num_owned + 1, sizeof (BITCODE_RLL));
  if (!attdefs)
    return DWG_ERR_OUTOFMEM;
  for (BITCODE_BL i = 0; blk->entities && i < num_owned; i++)
    if (is_placed_attdef (ref_object (dwg, blk->entities[i])))
      attdefs[n++] = ref_handle (blk->entities[i]);

  // an AcDbAttr group left out (DWG_ERR_INVALIDEED) doesn't stop the rest
  for (BITCODE_BL i = 0; error < DWG_ERR_CRITICAL && i < n; i++)
    {
      const Dwg_Object *defobj = handle_object (dwg, attdefs[i]);
      const Dwg_Entity_ATTDEF *attdef = defobj->tio.entity->tio.ATTDEF;
      char *tag = utf8_copy (dwg, attdef->tag);
      char *dflt = utf8_copy (dwg, attdef->default_value);
      const char *value = NULL;
      BITCODE_3BD pt, align;
      dwg_point_3d ins;
      Dwg_Entity_ATTRIB *attrib;
      Dwg_Object *attobj;
      BITCODE_RS etag_value = 0;
      BITCODE_RL etag = 0;
      int has_etag = 0, err;

      if (!tag || !dflt)
        {
          free (tag);
          free (dflt);
          error |= DWG_ERR_OUTOFMEM;
          break;
        }
      for (BITCODE_BL k = 0; k < num_atts && !value; k++)
        if (strEQ (atts[k].tag, tag))
          value = atts[k].value;
      // the block's ATTDEFs are already where the evaluation put them
      place_attdef (p, &base, attdef, &attdef->ins_pt, &attdef->alignment_pt,
                    &pt, &align);
      if (etags)
        has_etag = etag_index (dwg, defobj, &etag_value, &etag);
      start_at_width (p, attdef, att_width (atts, num_atts, tag), &pt, &align);
      ins.x = pt.x;
      ins.y = pt.y;
      ins.z = pt.z;
      attrib = dwg_add_ATTRIB (insert, attdef->height * p->scale,
                               attdef->flags, &ins, tag, value ? value : dflt);
      free (tag);
      free (dflt);
      if (!attrib)
        {
          error |= DWG_ERR_OUTOFMEM;
          break;
        }
      attobj = dwg_obj_generic_to_object (attrib, &err);
      defobj = handle_object (dwg, attdefs[i]);
      attdef = defobj->tio.entity->tio.ATTDEF;
      error |= copy_entity_properties (dwg, defobj->tio.entity,
                                       attobj->tio.entity);
      attrib->alignment_pt.x = align.x;
      attrib->alignment_pt.y = align.y;
      attrib->extrusion = attdef->extrusion;
      attrib->thickness = attdef->thickness * p->scale;
      attrib->oblique_angle = attdef->oblique_angle;
      attrib->rotation = fmod (attdef->rotation + p->rotation, 2.0 * M_PI);
      if (attrib->rotation < 0.0)
        attrib->rotation += 2.0 * M_PI;
      attrib->width_factor = attdef->width_factor;
      attrib->generation = attdef->generation;
      attrib->horiz_alignment = attdef->horiz_alignment;
      attrib->vert_alignment = attdef->vert_alignment;
      attrib->field_length = attdef->field_length;
      attrib->flags = attdef->flags;
      attrib->lock_position_flag = attdef->lock_position_flag;
      attrib->style = attdef->style ? dwg_add_handleref (
                                          dwg, 5, attdef->style->absolute_ref,
                                          NULL)
                                    : NULL;
#ifdef USE_WRITE
      // only the DWG encoder, which comes with write support, reads them
      dwg_set_dataflags (attobj);
#endif
      if (error < DWG_ERR_CRITICAL && has_etag)
        // AutoCAD's ATTRIBs carry the index, with a 1005 handle of 0
        error |= set_tag_eed (&attobj->tio.entity->eed,
                              &attobj->tio.entity->num_eed, etag_appid,
                              etag_value, 1, etag, 0);
      if (error < DWG_ERR_CRITICAL)
        error |= copy_attr_eed (dwg, defobj, attobj->tio.entity);
    }
  free (attdefs);
  return error;
}

/*--------------------------------------------------------------------------------
 * The API
 */

static int
place (Dynblock_Placement *p, const Dwg_DynValue *values,
       const BITCODE_BL num_values, const Dwg_AttValue *atts,
       const BITCODE_BL num_atts, int *eval_errorp)
{
  Dwg_Data *dwg = p->dwg;
  Dwg_Object *defobj = handle_object (dwg, p->def);
  Dwg_Object *graph = xdic_item (dwg, defobj, "ACAD_ENHANCEDBLOCK");
  Dwg_Entity_INSERT *insert;
  dwg_inthash *seen;
  int error, eval_error = 0, cycle;
  *eval_errorp = 0;

  if (dwg->header.from_version != R_2004)
    {
      set_reason (p, "only r2004 drawings are supported, not %s",
                  dwg_version_type (dwg->header.from_version));
      return INSERT_REFUSED | DWG_ERR_NOTYETSUPPORTED;
    }
  if (!graph || graph->fixedtype != DWG_TYPE_EVALUATION_GRAPH)
    {
      set_reason (p, "block " FORMAT_HV " is not a dynamic block definition",
                  p->def);
      return INSERT_REFUSED | DWG_ERR_INVALIDTYPE;
    }
  p->graph = graph->handle.value;
  seen = hash_new (64);
  if (!seen)
    return DWG_ERR_OUTOFMEM;
  cycle = p->owner == p->def || inserts_block (dwg, p->def, p->owner, seen);
  hash_free (seen);
  if (cycle)
    {
      set_reason (p, "block " FORMAT_HV " is the definition, or a block it "
                  "inserts",
                  p->owner);
      return INSERT_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
    }
  error = dwg_dynblock_get_params (defobj->tio.object->tio.BLOCK_HEADER,
                                   &p->params, &p->num_params);
  if (error)
    {
      set_reason (p, "reading the definition's properties failed (0x%x)",
                  (unsigned)error);
      return error;
    }
  error = check_values (p, values, num_values);
#ifndef USE_WRITE
  // the stored values can't be made: see new_blockrepresentation ()
  if (!error && !p->at_defaults)
    {
      set_reason (p, "a placement at other values than the definition's "
                     "needs LibreDWG built with write support");
      error = INSERT_REFUSED | DWG_ERR_NOTYETSUPPORTED;
    }
#endif
  // the evaluation planned, and its refusals, before anything is added
  if (!error && !p->at_defaults)
    error = dynblock_evaluated_state (dwg, p->def, values, num_values,
                                      &p->evaluated, p->reason,
                                      sizeof (p->reason));
  if (!error && !p->at_defaults)
    error = check_representation (p);
  if (!error)
    error = check_attributes (p, atts, num_atts);
  if (error)
    return error;

  if (p->at_defaults)
    p->block = p->def;
  else
    {
      Dwg_Object_BLOCK_HEADER *rep = dwg_dynblock_evaluate (
          handle_object (dwg, p->def)->tio.object->tio.BLOCK_HEADER, values,
          num_values, &eval_error);
      if (!rep)
        {
          set_reason (p, "the evaluation failed (0x%x)", (unsigned)eval_error);
          return eval_error;
        }
      // below DWG_ERR_CRITICAL: data the evaluation's copy left out
      *eval_errorp = eval_error;
      p->block = block_object (rep)->handle.value;
      error = complete_representation (p);
      if (error >= DWG_ERR_CRITICAL)
        return error;
    }

  {
    const dwg_point_3d ins_pt = { p->ins_pt.x, p->ins_pt.y, p->ins_pt.z };
    insert = dwg_add_INSERT_block (block_of (dwg, p->owner), &ins_pt,
                                   block_of (dwg, p->block), p->scale,
                                   p->scale, p->scale, p->rotation);
  }
  if (!insert)
    {
      set_reason (p, "could not add the INSERT");
      return error | DWG_ERR_OUTOFMEM;
    }
  p->insert = dwg->object[insert->parent->objid].handle.value;
  if (!p->at_defaults)
    error |= add_representation_data (p);
  if (error < DWG_ERR_CRITICAL)
    error |= add_attributes (p, atts, num_atts, p->at_defaults);
  return error;
}

EXPORT Dwg_Entity_INSERT *
dwg_dynblock_insert (Dwg_Object_BLOCK_HEADER *owner,
                     Dwg_Object_BLOCK_HEADER *def,
                     const BITCODE_3BD *ins_pt, const double scale,
                     const double rotation, const Dwg_DynValue *values,
                     const BITCODE_BL num_values, const Dwg_AttValue *atts,
                     const BITCODE_BL num_atts, int *errorp)
{
  const Dwg_Object *ownerobj = block_object (owner);
  const Dwg_Object *defobj = block_object (def);
  Dynblock_Placement p;
  Dwg_Entity_INSERT *insert = NULL;
  int error, eval_error = 0;

  if (errorp)
    *errorp = 0;
  if (!owner || !def || !ins_pt || (num_values && !values)
      || (num_atts && !atts))
    {
      if (errorp)
        *errorp = DWG_ERR_INTERNALERROR;
      return NULL;
    }
  if (!ownerobj || !defobj)
    {
      // the log level of the document the structs link to
      const Dwg_Data *dwg = def->parent && def->parent->dwg
                                ? def->parent->dwg
                                : (owner->parent ? owner->parent->dwg : NULL);
      if (dwg)
        loglevel = dwg->opts & DWG_OPTS_LOGLEVEL;
      LOG_ERROR ("dwg_dynblock_insert: %s is not a block of its document",
                 ownerobj ? "def" : "owner");
      if (errorp)
        *errorp = INSERT_REFUSED | DWG_ERR_INVALIDHANDLE;
      return NULL;
    }
  memset (&p, 0, sizeof (p));
  p.dwg = defobj->parent;
  loglevel = p.dwg->opts & DWG_OPTS_LOGLEVEL;
  p.owner = ownerobj->handle.value;
  p.def = defobj->handle.value;
  p.ins_pt = *ins_pt;
  p.scale = scale;
  p.rotation = fmod (rotation, 2.0 * M_PI);
  if (p.rotation < 0.0)
    p.rotation += 2.0 * M_PI;
  if (ownerobj->parent != defobj->parent)
    {
      set_reason (&p, "owner and def are blocks of different documents");
      error = INSERT_REFUSED | DWG_ERR_INVALIDHANDLE;
    }
  // a negative scale mirrors the block, which the attributes' placing
  // doesn't do
  else if (!isfinite (ins_pt->x) || !isfinite (ins_pt->y)
           || !isfinite (ins_pt->z) || !isfinite (scale) || scale <= 0.0
           || !isfinite (rotation))
    {
      set_reason (&p, "the insertion point, scale or rotation isn't finite, "
                      "or the scale isn't positive");
      error = INSERT_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
    }
  else
    error = place (&p, values, num_values, atts, num_atts, &eval_error);
  if (p.insert)
    {
      const Dwg_Object *obj = handle_object (p.dwg, p.insert);
      insert = obj ? obj->tio.entity->tio.INSERT : NULL;
    }
  error |= eval_error;
  if (error >= DWG_ERR_CRITICAL)
    {
      LOG_ERROR ("dwg_dynblock_insert: %s",
                 p.reason[0] ? p.reason : "failed");
      insert = NULL;
    }
  else if (error)
    LOG_WARN ("dwg_dynblock_insert: data was left out (0x%x)",
              (unsigned)error);
  dwg_dynblock_free_params (p.params, p.num_params);
  free (p.state);
  dynblock_free_state (&p.evaluated);
  if (errorp)
    *errorp = error;
  return insert;
}
