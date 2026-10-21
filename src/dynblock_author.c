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
 * dynblock_author.c: making dynamic blocks. Copying the entities of a
 * block, or of model space, into a block of another document, and giving a
 * block a linear property with the stretch and move actions it drives, as
 * AutoCAD's Block Editor stores them in r2004 files. See
 * dwg_dynblock_api.h.
 *
 * A property is a parameter node of the block's evaluation graph, with a
 * grip node and the grip's two location components, which give the grip's
 * position from the parameter's points; an action is a node that takes the
 * parameter's values. The graph lists its nodes with their edges, the
 * dependencies between them, as AutoCAD writes them:
 * - a node: its index, the flags 32, its node id (which the element keeps
 *   too, and actions name parameters by), its element, and its first and
 *   last edges in and out;
 * - an edge: its index, the flags 0, how many values it carries, the
 *   nodes it goes from and to, and the previous and next edges into the
 *   node it goes to and out of the node it goes from;
 * - the highest node id handed out, ids being unique over the graph's
 *   life.
 * A linear parameter adds the nodes parameter, grip, X location and Y
 * location, and the edges parameter to each location (1 value) and grip to
 * parameter (2); an action adds its node and an edge from its parameter (2
 * values, its X and Y deltas). A move of another parameter adds no edge.
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "common.h"
#include "bits.h"
#include "dwg.h"
#include "decode.h"
#include "encode.h"
#include "classes.h"
#include "hash.h"
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
#define AUTHOR_REFUSED DWG_ERR_INVALIDDWG

/* The length at or below which the evaluator takes a linear parameter to
   have none, as DYNBLOCK_VALUE_EPSILON in dynblock_eval.c. */
#define AUTHOR_MIN_LENGTH 1e-9

/* The element versions AutoCAD 2027 writes in r2004 files, as in
   test/test-data/2004/Dynblocks.dwg: of the evaluation expression every
   node starts with, and of the block element. */
#define EVALEXPR_MAJOR 33
#define EVALEXPR_MINOR 427
#define ELEMENT_MAJOR 25
#define ELEMENT_MINOR 104

/* The fields every evaluation graph node starts with. */
typedef struct _author_element
{
  struct _dwg_object_object *parent;
  Dwg_EvalExpr evalexpr;
} Author_Element;

/* The fields every block element starts with: parameters, actions and
   grips. */
typedef struct _author_block_element
{
  struct _dwg_object_object *parent;
  BLOCKELEMENT_fields;
} Author_Block_Element;

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

/* The object whose struct obj is, the first field of every object struct
   being its common object data, or NULL when it isn't one of dwg's
   objects of type type. */
static Dwg_Object *
struct_object (const Dwg_Data *dwg, const void *_obj,
               const Dwg_Object_Type type)
{
  const Dwg_Object_Object *o;
  Dwg_Object *obj;
  if (!_obj)
    return NULL;
  o = *(Dwg_Object_Object *const *)_obj;
  if (!o || o->dwg != dwg || o->objid >= dwg->num_objects)
    return NULL;
  obj = &dwg->object[o->objid];
  // a copy of the struct links to the object too, but isn't its
  if (obj->fixedtype != type || obj->tio.object != o
      || (const void *)o->tio.BLOCKLINEARPARAMETER != _obj)
    return NULL;
  return obj;
}

static int
is_finite_3d (const BITCODE_3BD *pt)
{
  return isfinite (pt->x) && isfinite (pt->y) && isfinite (pt->z);
}

/* ASCII letters compare without case, as AutoCAD compares names. */
static int
same_name (const char *a, const char *b)
{
  for (; *a && *b; a++, b++)
    {
      const unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
      if (ca != cb
          && !(ca < 128 && cb < 128
               && (ca | 0x20) == (cb | 0x20) && (ca | 0x20) >= 'a'
               && (ca | 0x20) <= 'z'))
        return 0;
    }
  return !*a && !*b;
}

/*--------------------------------------------------------------------------------
 * EED: the numbers of a definition's entities
 */

/* The name of the APPID of the EED group whose first item is eed. */
static const char *
group_appname (const Dwg_Data *dwg, const Dwg_Eed *eed)
{
  const Dwg_Object *appid = handle_object (dwg, eed->handle.value);
  if (!appid || appid->fixedtype != DWG_TYPE_APPID || !appid->tio.object
      || !appid->tio.object->tio.APPID)
    return NULL;
  return appid->tio.object->tio.APPID->name;
}

/* The index of the first item of the EED group of eed named appname, or
   -1. */
static int
find_group (const Dwg_Data *dwg, const Dwg_Eed *eed, const BITCODE_BL num_eed,
            const char *appname)
{
  for (BITCODE_BL i = 0; eed && i < num_eed; i++)
    if (eed[i].size)
      {
        const char *name = group_appname (dwg, &eed[i]);
        if (name && strEQ (name, appname))
          return (int)i;
      }
  return -1;
}

/* The 1071 item of the EED group starting at item start, or NULL. */
static Dwg_Eed_Data *
group_index_item (const Dwg_Eed *eed, const BITCODE_BL num_eed,
                  const int start)
{
  for (BITCODE_BL i = (BITCODE_BL)start; i < num_eed; i++)
    {
      if (i > (BITCODE_BL)start && eed[i].size)
        break;
      if (eed[i].data && eed[i].data->code == 71)
        return eed[i].data;
    }
  return NULL;
}

/* The AcDbBlockRepETag index of entity obj into *indexp; 0 when it has
   none. */
static int
etag_index (const Dwg_Data *dwg, const Dwg_Object *obj, BITCODE_RL *indexp)
{
  const Dwg_Object_Entity *ent = obj->tio.entity;
  const int start
      = find_group (dwg, ent->eed, ent->num_eed, "AcDbBlockRepETag");
  const Dwg_Eed_Data *item
      = start < 0 ? NULL : group_index_item (ent->eed, ent->num_eed, start);
  if (!item)
    return 0;
  // Dwg_Eed_Data is packed
  memcpy (indexp, &item->u.eed_71.rl, sizeof (*indexp));
  return 1;
}

/* Drops the AcDbBlockRepETag EED groups of the EED *eedp, of *num_eedp
   items. */
static void
drop_etag (const Dwg_Data *dwg, Dwg_Eed **eedp, BITCODE_BL *num_eedp)
{
  Dwg_Eed *eed = *eedp;
  BITCODE_BL kept = 0;
  int drop = 0;
  if (!eed)
    return;
  for (BITCODE_BL i = 0; i < *num_eedp; i++)
    {
      if (eed[i].size)
        {
          const char *name = group_appname (dwg, &eed[i]);
          drop = name && strEQc (name, "AcDbBlockRepETag");
        }
      if (drop)
        {
          free (eed[i].data);
          free (eed[i].raw);
          continue;
        }
      eed[kept++] = eed[i];
    }
  *num_eedp = kept;
  if (!kept)
    {
      free (eed);
      *eedp = NULL;
    }
}

void
dynblock_drop_etag (const Dwg_Data *dwg, Dwg_Object *obj)
{
  drop_etag (dwg, &obj->tio.entity->eed, &obj->tio.entity->num_eed);
}

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

/* Appends to the EED *eedp, of *num_eedp items, an AcDbBlockRepETag group
   of APPID appid: 1070 1, 1071 index, and when handle isn't 0, 1005
   handle. */
static int
append_etag (Dwg_Eed **eedp, BITCODE_BL *num_eedp, const BITCODE_RLL appid,
             const BITCODE_RL index, const BITCODE_RLL handle)
{
  const BITCODE_BL n = handle ? 3 : 2;
  const BITCODE_RS one = 1;
  Dwg_Eed *eed
      = (Dwg_Eed *)realloc (*eedp, (*num_eedp + n) * sizeof (Dwg_Eed));
  Dwg_Eed *group;
  if (!eed)
    return DWG_ERR_OUTOFMEM;
  *eedp = eed;
  group = &eed[*num_eedp];
  memset (group, 0, n * sizeof (Dwg_Eed));
  // each item's size: its code byte and its value
  group[0].size = 3 + 5 + (handle ? 9 : 0);
  group[0].handle.code = 5;
  group[0].handle.value = appid;
  dwg_set_handle_size (&group[0].handle);
  group[0].data = eed_item (70);
  group[1].data = eed_item (71);
  if (handle)
    group[2].data = eed_item (5);
  for (BITCODE_BL i = 0; i < n; i++)
    if (!group[i].data)
      {
        for (BITCODE_BL j = 0; j < n; j++)
          {
            free (group[j].data);
            group[j].data = NULL;
          }
        return DWG_ERR_OUTOFMEM;
      }
  // Dwg_Eed_Data is packed
  memcpy (&group[0].data->u.eed_70.rs, &one, sizeof (one));
  memcpy (&group[1].data->u.eed_71.rl, &index, sizeof (index));
  if (handle)
    memcpy (&group[2].data->u.eed_5.entity, &handle, sizeof (handle));
  *num_eedp += n;
  return 0;
}

/* Numbers the entities of block blk without an AcDbBlockRepETag index, and
   keeps the next index in blk's own group, as AutoCAD keeps a definition
   it has evaluated: the next index is one more than the highest of blk's
   entities, or blk's own counter when that is higher. */
static int
number_entities (Dwg_Data *dwg, const BITCODE_RLL blk_handle)
{
  Dwg_Object *blkobj = handle_object (dwg, blk_handle);
  const Dwg_Object_BLOCK_HEADER *blk = blkobj->tio.object->tio.BLOCK_HEADER;
  Dwg_Object_Object *o = blkobj->tio.object;
  const int counter_at
      = find_group (dwg, o->eed, o->num_eed, "AcDbBlockRepETag");
  Dwg_Eed_Data *counter
      = counter_at < 0 ? NULL
                       : group_index_item (o->eed, o->num_eed, counter_at);
  uint64_t next = 0, missing = 0;
  BITCODE_RLL appid;
  int error = 0;

  if (counter)
    {
      BITCODE_RL value;
      memcpy (&value, &counter->u.eed_71.rl, sizeof (value));
      next = value;
    }
  for (BITCODE_BL i = 0; blk->entities && i < blk->num_owned; i++)
    {
      const Dwg_Object *ent = ref_object (dwg, blk->entities[i]);
      BITCODE_RL index;
      if (ent && ent->supertype == DWG_SUPERTYPE_ENTITY
          && etag_index (dwg, ent, &index) && (uint64_t)index + 1 > next)
        next = (uint64_t)index + 1;
    }
  for (BITCODE_BL i = 0; blk->entities && i < blk->num_owned; i++)
    {
      const Dwg_Object *ent = ref_object (dwg, blk->entities[i]);
      BITCODE_RL index;
      if (ent && ent->supertype == DWG_SUPERTYPE_ENTITY
          && !etag_index (dwg, ent, &index))
        missing++;
    }
  // a block with no entities to number keeps no counter, as AutoCAD's
  if (!next && !missing)
    return 0;
  // the counter is a 1071, a signed 32-bit integer
  if (next + missing > 0x7FFFFFFF)
    {
      LOG_WARN ("BLOCK_HEADER " FORMAT_HV " has no AcDbBlockRepETag index "
                "left for its entities, which stay unnumbered",
                blk_handle);
      return DWG_ERR_INVALIDEED;
    }
  appid = appid_handle (dwg, "AcDbBlockRepETag");
  if (!appid)
    return DWG_ERR_OUTOFMEM;
  // adding the APPID may have moved dwg->object
  blkobj = handle_object (dwg, blk_handle);
  o = blkobj->tio.object;
  for (BITCODE_BL i = 0; !error && blk->entities && i < blk->num_owned; i++)
    {
      Dwg_Object *ent = ref_object (dwg, blk->entities[i]);
      BITCODE_RL index;
      if (!ent || ent->supertype != DWG_SUPERTYPE_ENTITY
          || etag_index (dwg, ent, &index))
        continue;
      error = append_etag (&ent->tio.entity->eed, &ent->tio.entity->num_eed,
                           appid, (BITCODE_RL)next, ent->handle.value);
      next++;
    }
  if (error)
    return error;
  // A new group at the end, rather than the old one changed: the encoder
  // writes a group with its bytes as read when it has them, and loses the
  // header of a changed group that one such follows.
  drop_etag (dwg, &o->eed, &o->num_eed);
  return append_etag (&o->eed, &o->num_eed, appid, (BITCODE_RL)next, 0);
}

/*--------------------------------------------------------------------------------
 * New objects of the dynamic-block classes, which the add API has no
 * functions for, made as it makes its objects
 */

#ifdef USE_WRITE
/* A new object of type type, with its class added to dwg when dwg lacks
   it, and the next handle. */
static Dwg_Object *
new_object (Dwg_Data *dwg, const Dwg_Object_Type type, const char *name,
            const char *dxfname, int (*setup) (Dwg_Object *))
{
  const BITCODE_BS num_classes = dwg->num_classes;
  const BITCODE_BL idx = dwg->num_objects;
  Dwg_Object *obj;
  int rc;
  if (dwg_require_class (dwg, dxfname, strlen (dxfname)) < 500)
    return NULL;
  // a new class may have moved dwg->dwg_class
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
  obj->fixedtype = type;
  obj->name = (char *)name;
  obj->dxfname = (char *)dxfname;
  if (setup (obj))
    return NULL;
  (void)dwg_encode_get_class (dwg, obj);
  if (obj->type >= 500 && obj->type - 500u < dwg->num_classes)
    obj->klass = &dwg->dwg_class[obj->type - 500];
  dwg_set_next_objhandle (obj);
  obj->tio.object->is_xdic_missing = 1;
  return obj;
}

/* Gives obj owner, and with reactor set, owner as its one reactor, as
   AutoCAD does for the objects a dictionary owns. */
static int
set_owner (Dwg_Data *dwg, Dwg_Object *obj, const BITCODE_RLL owner,
           const int reactor)
{
  Dwg_Object_Object *o = obj->tio.object;
  o->ownerhandle = dwg_add_handleref (dwg, 4, owner, obj);
  if (!o->ownerhandle)
    return DWG_ERR_OUTOFMEM;
  if (!reactor)
    return 0;
  o->reactors = (BITCODE_H *)calloc (1, sizeof (BITCODE_H));
  if (!o->reactors)
    return DWG_ERR_OUTOFMEM;
  o->reactors[0] = dwg_add_handleref (dwg, 4, owner, NULL);
  if (!o->reactors[0])
    return DWG_ERR_OUTOFMEM;
  o->num_reactors = 1;
  return 0;
}

static BITCODE_T
new_string (Dwg_Data *dwg, const char *utf8)
{
  return dwg_add_u8_input (dwg, utf8);
}

/* Adds the entry name for handle to dict, in the order of names AutoCAD
   keeps entries in. */
static int
add_sorted_item (Dwg_Object_DICTIONARY *dict, const char *name,
                 const BITCODE_RLL handle)
{
  if (!dwg_add_DICTIONARY_item (dict, name, handle))
    return DWG_ERR_OUTOFMEM;
  for (BITCODE_BL j = dict->numitems - 1;
       j > 0 && strcmp (dict->texts[j - 1], dict->texts[j]) > 0; j--)
    {
      BITCODE_T text = dict->texts[j];
      BITCODE_H item = dict->itemhandles[j];
      dict->texts[j] = dict->texts[j - 1];
      dict->itemhandles[j] = dict->itemhandles[j - 1];
      dict->texts[j - 1] = text;
      dict->itemhandles[j - 1] = item;
    }
  return 0;
}

/* The DICTIONARY that is block blkobj's extension dictionary, or NULL. */
static Dwg_Object *
block_xdic (const Dwg_Data *dwg, const Dwg_Object *blkobj)
{
  const Dwg_Object_Object *o = blkobj->tio.object;
  Dwg_Object *dict
      = o->is_xdic_missing ? NULL : ref_object (dwg, o->xdicobjhandle);
  return dict && dict->fixedtype == DWG_TYPE_DICTIONARY ? dict : NULL;
}

/* Makes block blk_handle a dynamic block definition: an empty
   EVALUATION_GRAPH under ACAD_ENHANCEDBLOCK in its extension dictionary,
   which is added when it has none, and for a block other than model space
   the purge preventer AutoCAD keeps beside the graph. Its handle goes into
   *graphp. */
static int
make_definition (Dwg_Data *dwg, const BITCODE_RLL blk_handle,
                 BITCODE_RLL *graphp)
{
  Dwg_Object *blkobj = handle_object (dwg, blk_handle);
  Dwg_Object *dictobj = block_xdic (dwg, blkobj);
  const int model_space = dynblock_is_model_space (dwg, blkobj);
  Dwg_Object_DICTIONARY *dict;
  Dwg_Object *obj;
  BITCODE_RLL dict_handle, graph_handle;
  int error;

  if (!dictobj)
    {
      dict = dwg_add_DICTIONARY (dwg, NULL, NULL, 0);
      dictobj = dict ? dwg_obj_generic_to_object (dict, &error) : NULL;
      if (!dictobj)
        return DWG_ERR_OUTOFMEM;
      dict->is_hardowner = 1;
      dict->cloning = 1;
      dict_handle = dictobj->handle.value;
      error = set_owner (dwg, dictobj, blk_handle, 0);
      if (error)
        return error;
      blkobj = handle_object (dwg, blk_handle);
      blkobj->tio.object->xdicobjhandle
          = dwg_add_handleref (dwg, 3, dict_handle, NULL);
      if (!blkobj->tio.object->xdicobjhandle)
        return DWG_ERR_OUTOFMEM;
      blkobj->tio.object->is_xdic_missing = 0;
    }
  else
    dict_handle = dictobj->handle.value;

  obj = new_object (dwg, DWG_TYPE_EVALUATION_GRAPH, "EVALUATION_GRAPH",
                    "ACAD_EVALUATION_GRAPH", dwg_setup_EVALUATION_GRAPH);
  if (!obj)
    return DWG_ERR_OUTOFMEM;
  graph_handle = obj->handle.value;
  error = set_owner (dwg, obj, dict_handle, 1);
  if (error)
    return error;
  dict = handle_object (dwg, dict_handle)->tio.object->tio.DICTIONARY;
  error = add_sorted_item (dict, "ACAD_ENHANCEDBLOCK", graph_handle);
  if (error)
    return error;

  if (!model_space)
    {
      Dwg_Object_DYNAMICBLOCKPURGEPREVENTER *_pp;
      obj = new_object (dwg, DWG_TYPE_DYNAMICBLOCKPURGEPREVENTER,
                        "DYNAMICBLOCKPURGEPREVENTER",
                        "ACDB_DYNAMICBLOCKPURGEPREVENTER_VERSION",
                        dwg_setup_DYNAMICBLOCKPURGEPREVENTER);
      if (!obj)
        return DWG_ERR_OUTOFMEM;
      _pp = obj->tio.object->tio.DYNAMICBLOCKPURGEPREVENTER;
      _pp->flag = 1;
      _pp->block = dwg_add_handleref (dwg, 5, blk_handle, NULL);
      if (!_pp->block)
        return DWG_ERR_OUTOFMEM;
      error = set_owner (dwg, obj, dict_handle, 1);
      if (error)
        return error;
      dict = handle_object (dwg, dict_handle)->tio.object->tio.DICTIONARY;
      error = add_sorted_item (dict, "AcDbDynamicBlockRoundTripPurgePreventer",
                               obj->handle.value);
      if (error)
        return error;
    }
  *graphp = graph_handle;
  return 0;
}
#endif

/*--------------------------------------------------------------------------------
 * The evaluation graph
 */

static Dwg_Object_EVALUATION_GRAPH *
graph_of (const Dwg_Data *dwg, const BITCODE_RLL graph_handle)
{
  return handle_object (dwg, graph_handle)->tio.object->tio.EVALUATION_GRAPH;
}

/* The element of node i of graph, or NULL. */
static Dwg_Object *
node_object (const Dwg_Data *dwg, const Dwg_Object_EVALUATION_GRAPH *graph,
             const BITCODE_BL i)
{
  return graph->nodes ? ref_object (dwg, graph->nodes[i].evalexpr) : NULL;
}

/* The index of the node of graph whose element is handle, or -1. */
static int
node_index (const Dwg_Object_EVALUATION_GRAPH *graph, const BITCODE_RLL handle)
{
  for (BITCODE_BL i = 0; graph->nodes && i < graph->num_nodes; i++)
    if (ref_handle (graph->nodes[i].evalexpr) == handle)
      return (int)i;
  return -1;
}

static int
is_grip (const Dwg_Object *obj)
{
  switch (obj->fixedtype)
    {
    case DWG_TYPE_BLOCKALIGNMENTGRIP:
    case DWG_TYPE_BLOCKFLIPGRIP:
    case DWG_TYPE_BLOCKLINEARGRIP:
    case DWG_TYPE_BLOCKLOOKUPGRIP:
    case DWG_TYPE_BLOCKPOLARGRIP:
    case DWG_TYPE_BLOCKROTATIONGRIP:
    case DWG_TYPE_BLOCKVISIBILITYGRIP:
    case DWG_TYPE_BLOCKXYGRIP:
    case DWG_TYPE_BLOCKPROPERTIESTABLEGRIP:
      return 1;
    default:
      return 0;
    }
}

/* The DXF name of obj's class, as the evaluator names it. */
static const char *
class_name (const Dwg_Data *dwg, const Dwg_Object *obj)
{
  if (obj->type >= 500 && obj->type - 500 < dwg->num_classes
      && dwg->dwg_class[obj->type - 500].dxfname)
    return dwg->dwg_class[obj->type - 500].dxfname;
  return obj->dxfname ? obj->dxfname : dwg_type_name (obj->fixedtype);
}

/* Whether index is -1 or an index below n. */
static int
is_index (const BITCODE_BLd index, const BITCODE_BL n)
{
  return index == -1 || (index >= 0 && (BITCODE_BL)index < n);
}

/* Refuses, as the evaluator does, a constraint-based (parametric)
   definition, whose proxy nodes LibreDWG can't decode, a node of another
   class it can't decode, and a node that doesn't resolve or isn't a graph
   element; and, before the graph's lists of nodes and edges grow, nodes
   and edges that index anything but each other. */
static int
check_graph (const Dwg_Data *dwg, const Dwg_Object_EVALUATION_GRAPH *graph,
             const char *func)
{
  if ((graph->num_nodes && !graph->nodes)
      || (graph->num_edges && !graph->edges))
    {
      LOG_ERROR ("%s: the evaluation graph lacks its nodes or edges", func);
      return AUTHOR_REFUSED;
    }
  for (BITCODE_BL i = 0; i < graph->num_edges; i++)
    {
      const Dwg_EVAL_Edge *e = &graph->edges[i];
      if (e->e2 < 0 || (BITCODE_BL)e->e2 >= graph->num_nodes || e->e3 < 0
          || (BITCODE_BL)e->e3 >= graph->num_nodes)
        {
          LOG_ERROR ("%s: edge %u of the evaluation graph joins nodes it "
                     "doesn't have",
                     func, (unsigned)i);
          return AUTHOR_REFUSED;
        }
      for (int k = 0; k < 4; k++)
        if (!is_index (e->out_edge[k], graph->num_edges))
          {
            LOG_ERROR ("%s: edge %u of the evaluation graph links an edge it "
                       "doesn't have",
                       func, (unsigned)i);
            return AUTHOR_REFUSED;
          }
    }
  for (BITCODE_BL i = 0; i < graph->num_nodes; i++)
    {
      const Dwg_Object *obj = node_object (dwg, graph, i);
      for (int k = 0; k < 4; k++)
        if (!is_index (graph->nodes[i].node[k], graph->num_edges))
          {
            LOG_ERROR ("%s: node %u of the evaluation graph names an edge it "
                       "doesn't have",
                       func, (unsigned)i);
            return AUTHOR_REFUSED;
          }
      if (!obj)
        {
          LOG_ERROR ("%s: node %u of the evaluation graph doesn't resolve",
                     func, (unsigned)i);
          return AUTHOR_REFUSED | DWG_ERR_INVALIDHANDLE;
        }
      if (obj->fixedtype == DWG_TYPE_DYNAMICBLOCKPROXYNODE
          || strEQc (class_name (dwg, obj), "ACDB_DYNAMICBLOCKPROXYNODE"))
        {
          LOG_ERROR ("%s: the block is a constraint-based (parametric) "
                     "definition, which is not supported",
                     func);
          return AUTHOR_REFUSED | DWG_ERR_NOTYETSUPPORTED;
        }
      if (obj->fixedtype == DWG_TYPE_UNKNOWN_OBJ)
        {
          LOG_ERROR ("%s: node %u of the evaluation graph is a %s, which "
                     "LibreDWG can't decode",
                     func, (unsigned)i, class_name (dwg, obj));
          return AUTHOR_REFUSED | DWG_ERR_UNHANDLEDCLASS;
        }
      if (obj->supertype != DWG_SUPERTYPE_OBJECT || !obj->tio.object
          || !obj->tio.object->tio.BLOCKMOVEACTION
          || !dynblock_is_graph_element (obj))
        {
          LOG_ERROR ("%s: node %u of the evaluation graph is a %s, which is "
                     "no parameter, action, grip or grip location",
                     func, (unsigned)i, class_name (dwg, obj));
          return AUTHOR_REFUSED | DWG_ERR_INVALIDHANDLE;
        }
    }
  return 0;
}

#ifdef USE_WRITE
/* Adds element obj to graph as a new node, with the next node id, which
   it returns, or 0 when memory runs out. check_ids () has checked that
   there is one. */
static BITCODE_BL
add_node (Dwg_Data *dwg, Dwg_Object_EVALUATION_GRAPH *graph,
          Dwg_Object *obj)
{
  Dwg_EVAL_Node *nodes = (Dwg_EVAL_Node *)realloc (
      graph->nodes, (graph->num_nodes + 1) * sizeof (Dwg_EVAL_Node));
  Dwg_EVAL_Node *node;
  if (!nodes)
    return 0;
  graph->nodes = nodes;
  node = &nodes[graph->num_nodes];
  memset (node, 0, sizeof (Dwg_EVAL_Node));
  node->evalexpr = dwg_add_handleref (dwg, 3, obj->handle.value, NULL);
  if (!node->evalexpr)
    return 0;
  node->parent = graph;
  node->id = graph->num_nodes;
  node->edge_flags = 32;
  // ids are handed out from the highest so far, never again; the graph
  // keeps it, and no node has a higher one
  for (BITCODE_BL i = 0; i < graph->num_nodes; i++)
    if (nodes[i].nextid > graph->first_nodeid)
      graph->first_nodeid = nodes[i].nextid;
  graph->first_nodeid++;
  graph->first_nodeid_copy = graph->first_nodeid;
  node->nextid = graph->first_nodeid;
  for (int k = 0; k < 4; k++)
    node->node[k] = -1;
  graph->num_nodes++;
  ((Author_Element *)obj->tio.object->tio.BLOCKMOVEACTION)->evalexpr.nodeid
      = (BITCODE_BL)node->nextid;
  return (BITCODE_BL)node->nextid;
}

/* Adds an edge carrying count values from node from to node to of graph,
   last into to and last out of from. */
static int
add_edge (Dwg_Object_EVALUATION_GRAPH *graph, const BITCODE_BL from,
          const BITCODE_BL to, const BITCODE_BL count)
{
  Dwg_EVAL_Edge *edges = (Dwg_EVAL_Edge *)realloc (
      graph->edges, (graph->num_edges + 1) * sizeof (Dwg_EVAL_Edge));
  Dwg_EVAL_Edge *edge;
  const BITCODE_BLd id = (BITCODE_BLd)graph->num_edges;
  if (!edges)
    return DWG_ERR_OUTOFMEM;
  graph->edges = edges;
  edge = &edges[id];
  memset (edge, 0, sizeof (Dwg_EVAL_Edge));
  edge->parent = graph;
  edge->id = (BITCODE_BL)id;
  edge->nextid = 0;
  edge->e1 = (BITCODE_BLd)count;
  edge->e2 = (BITCODE_BLd)from;
  edge->e3 = (BITCODE_BLd)to;
  // the previous and next edges into to, then out of from
  edge->out_edge[0] = graph->nodes[to].node[1];
  edge->out_edge[1] = -1;
  edge->out_edge[2] = graph->nodes[from].node[3];
  edge->out_edge[3] = -1;
  edge->out_edge[4] = -1;
  if (graph->nodes[to].node[1] >= 0)
    edges[graph->nodes[to].node[1]].out_edge[1] = id;
  else
    graph->nodes[to].node[0] = id;
  graph->nodes[to].node[1] = id;
  if (graph->nodes[from].node[3] >= 0)
    edges[graph->nodes[from].node[3]].out_edge[3] = id;
  else
    graph->nodes[from].node[2] = id;
  graph->nodes[from].node[3] = id;
  graph->num_edges++;
  return 0;
}

/* Sets the fields every node starts with, of element obj. */
static void
init_evalexpr (Dwg_Object *obj)
{
  Author_Element *e = (Author_Element *)obj->tio.object->tio.BLOCKMOVEACTION;
  e->evalexpr.parentid = -1;
  e->evalexpr.major = EVALEXPR_MAJOR;
  e->evalexpr.minor = EVALEXPR_MINOR;
  e->evalexpr.value_code = -9999;
}

/* Whether a block element of graph is named name. */
static int
has_element_name (const Dwg_Data *dwg,
                  const Dwg_Object_EVALUATION_GRAPH *graph, const char *name)
{
  for (BITCODE_BL i = 0; i < graph->num_nodes; i++)
    {
      const Dwg_Object *obj = node_object (dwg, graph, i);
      const Author_Block_Element *e;
      // every node is a graph element (check_graph ()); grip location
      // components and proxy nodes have no name
      if (!obj || obj->fixedtype == DWG_TYPE_BLOCKGRIPLOCATIONCOMPONENT
          || obj->fixedtype == DWG_TYPE_DYNAMICBLOCKPROXYNODE)
        continue;
      e = (const Author_Block_Element *)obj->tio.object->tio.BLOCKMOVEACTION;
      if (e->name && strEQ (e->name, name))
        return 1;
    }
  return 0;
}

/* Sets the fields every block element starts with, of element obj, which
   is named as AutoCAD names a new element of its kind: base, then base1,
   base2 and so on, the first that graph doesn't have. */
static int
init_element (Dwg_Data *dwg, const Dwg_Object_EVALUATION_GRAPH *graph,
              Dwg_Object *obj, const char *base)
{
  Author_Block_Element *e
      = (Author_Block_Element *)obj->tio.object->tio.BLOCKMOVEACTION;
  char name[64];
  init_evalexpr (obj);
  snprintf (name, sizeof (name), "%s", base);
  for (unsigned n = 1; has_element_name (dwg, graph, name) && n < 100000; n++)
    snprintf (name, sizeof (name), "%s%u", base, n);
  e->name = new_string (dwg, name);
  e->be_major = ELEMENT_MAJOR;
  e->be_minor = ELEMENT_MINOR;
  e->eed1071 = 0;
  return e->name ? 0 : DWG_ERR_OUTOFMEM;
}
#endif

/*--------------------------------------------------------------------------------
 * The block an authoring function works on
 */

typedef struct _author_block
{
  Dwg_Data *dwg;
  BITCODE_RLL blk;   /* the block */
  BITCODE_RLL graph; /* its EVALUATION_GRAPH, 0 for none yet */
  dwg_inthash *entities; /* blk's entities */
} Author_Block;

static void
free_block (Author_Block *ab)
{
  if (ab->entities)
    hash_free (ab->entities);
  ab->entities = NULL;
}

/* Checks block blk for function func, into *ab: a block of its document,
   in an r2004 drawing, that is model space or a block other than a layout
   or an xref, whose graph, if it has one, is one LibreDWG reads. */
static int
author_block (Dwg_Object_BLOCK_HEADER *blk, const char *func,
              Author_Block *ab)
{
  const Dwg_Object *blkobj = block_object (blk);
  const Dwg_Object *graph;
  memset (ab, 0, sizeof (Author_Block));
  if (!blkobj)
    {
      if (blk && blk->parent && blk->parent->dwg)
        loglevel = blk->parent->dwg->opts & DWG_OPTS_LOGLEVEL;
      LOG_ERROR ("%s: blk is not a block of its document", func);
      return AUTHOR_REFUSED | DWG_ERR_INVALIDHANDLE;
    }
  ab->dwg = blkobj->parent;
  loglevel = ab->dwg->opts & DWG_OPTS_LOGLEVEL;
  ab->blk = blkobj->handle.value;
  if (ab->dwg->header.from_version != R_2004)
    {
      LOG_ERROR ("%s: only r2004 drawings are supported, not %s", func,
                 dwg_version_type (ab->dwg->header.from_version));
      return AUTHOR_REFUSED | DWG_ERR_NOTYETSUPPORTED;
    }
#ifndef USE_WRITE
  LOG_ERROR ("%s: making dynamic blocks needs LibreDWG built with write "
             "support",
             func);
  return AUTHOR_REFUSED | DWG_ERR_NOTYETSUPPORTED;
#endif
  if (!dynblock_is_copyable_block (ab->dwg, blkobj))
    {
      LOG_ERROR ("%s: BLOCK_HEADER " FORMAT_HV " is a layout block other "
                 "than model space, or an xref",
                 func, ab->blk);
      return AUTHOR_REFUSED | DWG_ERR_INVALIDTYPE;
    }
  graph = dynblock_graph (ab->dwg, blkobj);
  if (graph)
    {
      int error;
      ab->graph = graph->handle.value;
      error = check_graph (ab->dwg, graph->tio.object->tio.EVALUATION_GRAPH,
                           func);
      if (error)
        return error;
    }
  ab->entities = hash_new (blk->num_owned * 2 + 8);
  if (!ab->entities)
    return DWG_ERR_OUTOFMEM;
  for (BITCODE_BL i = 0; blk->entities && i < blk->num_owned; i++)
    if (ref_handle (blk->entities[i]))
      hash_set (ab->entities, ref_handle (blk->entities[i]), 1);
  return 0;
}

/* Checks that param is a linear parameter of ab's graph that the evaluator
   evaluates: its base point at its start, and driving only stretch and
   move actions. Its object goes into *objp, its node index into
   *indexp. */
static int
check_param (const Author_Block *ab,
             const Dwg_Object_BLOCKLINEARPARAMETER *param, const char *func,
             const Dwg_Object **objp, int *indexp)
{
  const Dwg_Object *obj
      = struct_object (ab->dwg, param, DWG_TYPE_BLOCKLINEARPARAMETER);
  const Dwg_Object_EVALUATION_GRAPH *graph
      = ab->graph ? graph_of (ab->dwg, ab->graph) : NULL;
  const int index = obj && graph ? node_index (graph, obj->handle.value) : -1;
  if (index < 0)
    {
      LOG_ERROR ("%s: param is not a linear parameter of the block's "
                 "evaluation graph",
                 func);
      return AUTHOR_REFUSED | DWG_ERR_INVALIDHANDLE;
    }
  if (param->parameter_base_location)
    {
      LOG_ERROR ("%s: a linear parameter with its base point in its middle "
                 "is not supported",
                 func);
      return AUTHOR_REFUSED | DWG_ERR_NOTYETSUPPORTED;
    }
  for (BITCODE_BL i = 0; graph->edges && i < graph->num_edges; i++)
    {
      const Dwg_EVAL_Edge *edge = &graph->edges[i];
      const Dwg_Object *to;
      if (edge->e2 != index || edge->e3 < 0
          || (BITCODE_BL)edge->e3 >= graph->num_nodes)
        continue;
      to = node_object (ab->dwg, graph, (BITCODE_BL)edge->e3);
      if (to && to->fixedtype != DWG_TYPE_BLOCKGRIPLOCATIONCOMPONENT
          && to->fixedtype != DWG_TYPE_BLOCKSTRETCHACTION
          && to->fixedtype != DWG_TYPE_BLOCKMOVEACTION)
        {
          LOG_ERROR ("%s: param drives %s " FORMAT_HV ", which is not "
                     "supported",
                     func, dwg_type_name (to->fixedtype), to->handle.value);
          return AUTHOR_REFUSED | DWG_ERR_NOTYETSUPPORTED;
        }
    }
  *objp = obj;
  *indexp = index;
  return 0;
}

/* Refuses an addition of n nodes to ab's graph, if it has one, that would
   take node ids beyond what the graph's signed 32-bit ids hold: they are
   handed out after the highest so far, which the graph keeps, and which
   no node has passed. */
static int
check_ids (const Author_Block *ab, const BITCODE_BL n, const char *func)
{
  const Dwg_Object_EVALUATION_GRAPH *graph;
  int64_t highest;
  if (!ab->graph)
    return 0;
  graph = graph_of (ab->dwg, ab->graph);
  highest = graph->first_nodeid;
  for (BITCODE_BL i = 0; i < graph->num_nodes; i++)
    if (graph->nodes[i].nextid > highest)
      highest = graph->nodes[i].nextid;
  if (highest + (int64_t)n <= INT32_MAX)
    return 0;
  LOG_ERROR ("%s: the evaluation graph has no node ids left", func);
  return AUTHOR_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
}

/* Checks the handles of a selection: each given once, and n > 0. */
static int
check_selection (const BITCODE_RLL *handles, const BITCODE_BL n,
                 const char *func)
{
  if (!n)
    {
      LOG_ERROR ("%s: nothing is selected", func);
      return AUTHOR_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
    }
  for (BITCODE_BL i = 1; i < n; i++)
    for (BITCODE_BL j = 0; j < i; j++)
      if (handles[i] == handles[j])
        {
          LOG_ERROR ("%s: " FORMAT_HV " is selected twice", func,
                     handles[i]);
          return AUTHOR_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
        }
  return 0;
}

/* The entity of ab's block that handle names, or NULL with a refusal. */
static const Dwg_Object *
block_entity (const Author_Block *ab, const BITCODE_RLL handle,
              const char *func, int *errorp)
{
  const Dwg_Object *obj = handle_object (ab->dwg, handle);
  if (!obj || obj->supertype != DWG_SUPERTYPE_ENTITY
      || hash_get (ab->entities, handle) == HASH_NOT_FOUND)
    {
      LOG_ERROR ("%s: " FORMAT_HV " is not an entity of the block", func,
                 handle);
      *errorp = AUTHOR_REFUSED | DWG_ERR_INVALIDHANDLE;
      return NULL;
    }
  return obj;
}

/* Refuses object obj of a stretch's or move's selection, an entity or a
   parameter the move carries, that a rotate action of ab's graph turns,
   selecting it or listing its node id: the evaluator can't order a turn
   and a move of the same points. */
static int
check_turned (const Author_Block *ab, const Dwg_Object *obj,
              const char *func)
{
  const Dwg_Object_EVALUATION_GRAPH *graph;
  int node;
  if (!ab->graph)
    return 0;
  graph = graph_of (ab->dwg, ab->graph);
  node = node_index (graph, obj->handle.value);
  for (BITCODE_BL i = 0; i < graph->num_nodes; i++)
    {
      const Dwg_Object *a = node_object (ab->dwg, graph, i);
      const Dwg_Object_BLOCKROTATEACTION *_a;
      int turns = 0;
      if (!a || a->fixedtype != DWG_TYPE_BLOCKROTATEACTION)
        continue;
      _a = a->tio.object->tio.BLOCKROTATEACTION;
      for (BITCODE_BL j = 0; _a->deps && j < _a->num_deps; j++)
        if (ref_handle (_a->deps[j]) == obj->handle.value)
          turns = 1;
      for (BITCODE_BL j = 0; node >= 0 && _a->actions && j < _a->num_actions;
           j++)
        if (_a->actions[j] == (BITCODE_BL)graph->nodes[node].nextid)
          turns = 1;
      if (turns)
        {
          LOG_ERROR ("%s: %s " FORMAT_HV " is turned by rotate action "
                     FORMAT_HV ", which the evaluator can't order with "
                     "another action moving it: not supported",
                     func, dwg_type_name (obj->fixedtype), obj->handle.value,
                     a->handle.value);
          return AUTHOR_REFUSED | DWG_ERR_NOTYETSUPPORTED;
        }
    }
  return 0;
}

static Dwg_Object_BLOCKLINEARPARAMETER *
refused_linear (int *errorp, const int error)
{
  if (errorp)
    *errorp = error;
  return NULL;
}

/*--------------------------------------------------------------------------------
 * The API
 */

/* The handles of the entities of block blkobj of src into a new array
   *topsp, of *nump, after checking that each resolves, and when the
   copies move by delta, that the evaluator's move takes it. */
static int
entities_to_copy (const Dwg_Data *src, const Dwg_Object *blkobj,
                  const BITCODE_3BD *delta, BITCODE_RLL **topsp,
                  BITCODE_BL *nump)
{
  const Dwg_Object_BLOCK_HEADER *blk = blkobj->tio.object->tio.BLOCK_HEADER;
  const int moves = delta->x != 0.0 || delta->y != 0.0 || delta->z != 0.0;
  const double size = fabs (delta->x) + fabs (delta->y) + fabs (delta->z);
  BITCODE_RLL *tops;
  BITCODE_BL n = 0;
  *topsp = NULL;
  *nump = 0;
  tops = (BITCODE_RLL *)malloc ((blk->num_owned + 1) * sizeof (BITCODE_RLL));
  if (!tops)
    return DWG_ERR_OUTOFMEM;
  for (BITCODE_BL i = 0; blk->entities && i < blk->num_owned; i++)
    {
      const BITCODE_RLL handle = ref_handle (blk->entities[i]);
      const Dwg_Object *obj = handle_object (src, handle);
      int error = 0;
      if (!obj || obj->supertype != DWG_SUPERTYPE_ENTITY || !obj->tio.entity)
        {
          LOG_ERROR ("dwg_dynblock_copy_entities: entity " FORMAT_HV
                     " of src_block doesn't resolve to an entity",
                     handle);
          error = AUTHOR_REFUSED | DWG_ERR_INVALIDHANDLE;
        }
      else if (moves && !dynblock_is_movable (obj))
        {
          LOG_ERROR ("dwg_dynblock_copy_entities: moving %s " FORMAT_HV
                     " is not supported",
                     dwg_type_name (obj->fixedtype), handle);
          error = AUTHOR_REFUSED | DWG_ERR_NOTYETSUPPORTED;
        }
      else if (moves && dynblock_leaves_plane (src, obj, delta))
        {
          LOG_ERROR ("dwg_dynblock_copy_entities: moving %s " FORMAT_HV
                     " out of its plane is not supported",
                     dwg_type_name (obj->fixedtype), handle);
          error = AUTHOR_REFUSED | DWG_ERR_NOTYETSUPPORTED;
        }
      else if (moves && !isfinite (dynblock_entity_reach (src, obj) + size))
        {
          LOG_ERROR ("dwg_dynblock_copy_entities: the move would take "
                     "%s " FORMAT_HV " beyond what a double holds",
                     dwg_type_name (obj->fixedtype), handle);
          error = AUTHOR_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
        }
      if (error)
        {
          free (tops);
          return error;
        }
      tops[n++] = handle;
    }
  *topsp = tops;
  *nump = n;
  return 0;
}

/* Adds the copies of the entities tops to block dst_block, moved by delta,
   and returns how many it added. */
static BITCODE_BL
place_copies (Dwg_Data *dst, Dwg_Object_BLOCK_HEADER *dst_block,
              const BITCODE_RLL *tops, const BITCODE_BL num_tops,
              const Dwg_Copy_Map *map, const BITCODE_3BD *delta)
{
  const int moves = delta->x != 0.0 || delta->y != 0.0 || delta->z != 0.0;
  int attdefs = 0;
  BITCODE_BL count = 0;
  for (BITCODE_BL i = 0; i < num_tops; i++)
    {
      Dwg_Object *copy
          = handle_object (dst, dwg_copy_map_get (map, tops[i]));
      if (!copy || copy->supertype != DWG_SUPERTYPE_ENTITY)
        continue;
      // dwg_insert_entity () gives it the mode of dst_block: model space,
      // paper space, or a block it names as its owner
      copy->tio.entity->entmode = 0;
      dwg_insert_entity (dst_block, copy);
      dynblock_drop_etag (dst, copy);
      if (moves)
        dynblock_move_entity (dst, copy, delta);
      if (copy->fixedtype == DWG_TYPE_ATTDEF)
        attdefs = 1;
      count++;
    }
  if (attdefs)
    {
      dst_block->hasattrs = 1;
      dst_block->flag |= 2;
    }
  return count;
}

static BITCODE_BL
refused_copy (int *errorp, const int error)
{
  if (errorp)
    *errorp = AUTHOR_REFUSED | error;
  return 0;
}

EXPORT BITCODE_BL
dwg_dynblock_copy_entities (Dwg_Object_BLOCK_HEADER *dst_block,
                            const Dwg_Data *src,
                            const Dwg_Object_BLOCK_HEADER *src_block,
                            const BITCODE_3BD *offset, int *errorp)
{
  Dwg_Data *dst;
  const Dwg_Object *srcobj, *dstobj;
  const BITCODE_3BD *src_base, *dst_base;
  BITCODE_3BD delta;
  BITCODE_RLL *tops = NULL;
  BITCODE_BL num_tops = 0, count = 0;
  BITCODE_RLL dst_handle;
  Dwg_Copy_Map *map;
  int error;

  if (errorp)
    *errorp = 0;
  if (!dst_block || !src || !src_block || !offset)
    return refused_copy (errorp, DWG_ERR_INTERNALERROR);
  dstobj = block_object (dst_block);
  if (!dstobj)
    {
      LOG_ERROR ("dwg_dynblock_copy_entities: dst_block is not a block of "
                 "its document");
      return refused_copy (errorp, DWG_ERR_INVALIDHANDLE);
    }
  dst = dstobj->parent;
  loglevel = dst->opts & DWG_OPTS_LOGLEVEL;
  if (src == dst)
    {
      LOG_ERROR ("dwg_dynblock_copy_entities: src is dst_block's document");
      return refused_copy (errorp, DWG_ERR_INTERNALERROR);
    }
  if (src->header.from_version != R_2004 || dst->header.from_version != R_2004)
    {
      LOG_ERROR ("dwg_dynblock_copy_entities: only r2004 (AC1018) documents "
                 "are supported, not %s into %s",
                 dwg_version_type (src->header.from_version),
                 dwg_version_type (dst->header.from_version));
      return refused_copy (errorp, DWG_ERR_NOTYETSUPPORTED);
    }
  if (src->header.codepage != dst->header.codepage)
    {
      LOG_ERROR ("dwg_dynblock_copy_entities: source codepage %u differs "
                 "from destination codepage %u",
                 (unsigned)src->header.codepage,
                 (unsigned)dst->header.codepage);
      return refused_copy (errorp, DWG_ERR_NOTYETSUPPORTED);
    }
  srcobj = block_object (src_block);
  if (!srcobj || srcobj->parent != src)
    {
      LOG_ERROR ("dwg_dynblock_copy_entities: src_block is not a block of "
                 "src");
      return refused_copy (errorp, DWG_ERR_INVALIDHANDLE);
    }
  if (!dynblock_is_copyable_block (src, srcobj)
      || !dynblock_is_copyable_block (dst, dstobj))
    {
      LOG_ERROR ("dwg_dynblock_copy_entities: BLOCK_HEADER " FORMAT_HV
                 " is a layout block other than model space, or an xref",
                 dynblock_is_copyable_block (src, srcobj)
                     ? dstobj->handle.value
                     : srcobj->handle.value);
      return refused_copy (errorp, DWG_ERR_INVALIDTYPE);
    }
  // the base point of model space is the drawing's INSBASE, where AutoCAD
  // puts the drawing's base when it inserts the drawing as a block
  src_base = dynblock_is_model_space (src, srcobj)
                 ? &src->header_vars.INSBASE
                 : &src_block->base_pt;
  dst_base = dynblock_is_model_space (dst, dstobj)
                 ? &dst->header_vars.INSBASE
                 : &dst_block->base_pt;
  delta.x = dst_base->x + offset->x - src_base->x;
  delta.y = dst_base->y + offset->y - src_base->y;
  delta.z = dst_base->z + offset->z - src_base->z;
  if (!is_finite_3d (offset) || !is_finite_3d (src_base)
      || !is_finite_3d (dst_base) || !is_finite_3d (&delta))
    {
      LOG_ERROR ("dwg_dynblock_copy_entities: the offset or a base point "
                 "isn't finite, or the move is beyond what a double holds");
      return refused_copy (errorp, DWG_ERR_VALUEOUTOFBOUNDS);
    }
  error = entities_to_copy (src, srcobj, &delta, &tops, &num_tops);
  if (error || !num_tops)
    {
      free (tops);
      if (errorp)
        *errorp = error;
      return 0;
    }

  dst_handle = dstobj->handle.value;
  map = dwg_copy_map_new ();
  if (!map)
    error = DWG_ERR_OUTOFMEM;
  else
    error = dynblock_copy_entities (dst, src, srcobj->handle.value,
                                    dst_handle, tops, num_tops, map);
  if (error < DWG_ERR_CRITICAL)
    {
      count = place_copies (dst, dst_block, tops, num_tops, map, &delta);
      if (dynblock_graph (dst, handle_object (dst, dst_handle)))
        error |= number_entities (dst, dst_handle);
    }
  else
    LOG_ERROR ("dwg_dynblock_copy_entities: the entities of BLOCK_HEADER "
               FORMAT_HV " not copied, error 0x%x",
               srcobj->handle.value, (unsigned)error);
  free (tops);
  dwg_copy_map_free (map);
  if (errorp)
    *errorp = error;
  return error >= DWG_ERR_CRITICAL ? 0 : count;
}

EXPORT Dwg_Object_BLOCKLINEARPARAMETER *
dwg_dynblock_add_linear (Dwg_Object_BLOCK_HEADER *blk, const char *name,
                         const BITCODE_3BD *base_pt,
                         const BITCODE_3BD *end_pt, int *errorp)
{
  static const char func[] = "dwg_dynblock_add_linear";
  Author_Block ab;
  int error;
  if (errorp)
    *errorp = 0;
  if (!blk || !name || !base_pt || !end_pt)
    return refused_linear (errorp, AUTHOR_REFUSED | DWG_ERR_INTERNALERROR);
  error = author_block (blk, func, &ab);
  if (!error && !*name)
    {
      LOG_ERROR ("%s: the property has no name", func);
      error = AUTHOR_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
    }
  if (!error
      && (!is_finite_3d (base_pt) || !is_finite_3d (end_pt)
          || base_pt->z != end_pt->z
          || !(hypot (end_pt->x - base_pt->x, end_pt->y - base_pt->y)
               > AUTHOR_MIN_LENGTH)
          || !isfinite (hypot (end_pt->x - base_pt->x,
                               end_pt->y - base_pt->y))))
    {
      LOG_ERROR ("%s: the points aren't finite, are no more than %g apart, "
                 "or aren't in one XY plane",
                 func, AUTHOR_MIN_LENGTH);
      error = AUTHOR_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
    }
  // the parameter, its grip and the grip's two location components
  if (!error)
    error = check_ids (&ab, 4, func);
  if (!error && ab.graph)
    {
      // the property's name, as dwg_dynblock_evaluate () looks values up
      Dwg_DynParam *params = NULL;
      BITCODE_BL count = 0;
      error = dwg_dynblock_get_params (blk, &params, &count);
      for (BITCODE_BL i = 0; !error && i < count; i++)
        if (params[i].name && same_name (params[i].name, name))
          {
            LOG_ERROR ("%s: the block has a property named '%s' already",
                       func, name);
            error = AUTHOR_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
          }
      dwg_dynblock_free_params (params, count);
    }
  if (error)
    {
      free_block (&ab);
      return refused_linear (errorp, error);
    }

#ifdef USE_WRITE
  {
    Dwg_Data *dwg = ab.dwg;
    Dwg_Object *obj;
    Dwg_Object_EVALUATION_GRAPH *graph = NULL;
    Dwg_Object_BLOCKLINEARPARAMETER *_param = NULL;
    Dwg_Object_BLOCKLINEARGRIP *_grip;
    BITCODE_RLL h[4];
    BITCODE_BL ids[4], first;
    const char *const exprs[2] = { "UpdatedEndX", "UpdatedEndY" };
    const char *const connections[2] = { "DisplacementX", "DisplacementY" };

    if (!ab.graph)
      error = make_definition (dwg, ab.blk, &ab.graph);
    // the parameter, its grip, and the grip's X and Y location components
    obj = error ? NULL
                : new_object (dwg, DWG_TYPE_BLOCKLINEARPARAMETER,
                              "BLOCKLINEARPARAMETER", "BLOCKLINEARPARAMETER",
                              dwg_setup_BLOCKLINEARPARAMETER);
    h[0] = obj ? obj->handle.value : 0;
    obj = obj ? new_object (dwg, DWG_TYPE_BLOCKLINEARGRIP, "BLOCKLINEARGRIP",
                            "BLOCKLINEARGRIP", dwg_setup_BLOCKLINEARGRIP)
              : NULL;
    h[1] = obj ? obj->handle.value : 0;
    for (int k = 2; k < 4; k++)
      {
        obj = obj ? new_object (dwg, DWG_TYPE_BLOCKGRIPLOCATIONCOMPONENT,
                                "BLOCKGRIPLOCATIONCOMPONENT",
                                "BLOCKGRIPLOCATIONCOMPONENT",
                                dwg_setup_BLOCKGRIPLOCATIONCOMPONENT)
                  : NULL;
        h[k] = obj ? obj->handle.value : 0;
      }
    if (!error && !obj)
      error = DWG_ERR_OUTOFMEM;
    if (!error)
      {
        graph = graph_of (dwg, ab.graph);
        // a node read with the graph's raw bits must be written from its
        // fields now, which give AutoCAD's bits (dynblocks_test)
        obj = handle_object (dwg, ab.graph);
        free (obj->unknown_bits);
        obj->unknown_bits = NULL;
        obj->num_unknown_bits = 0;
        first = graph->num_nodes;
        for (int k = 0; !error && k < 4; k++)
          {
            obj = handle_object (dwg, h[k]);
            error = set_owner (dwg, obj, ab.graph, 0);
            ids[k] = error ? 0 : add_node (dwg, graph, obj);
            if (!error && !ids[k])
              error = DWG_ERR_OUTOFMEM;
          }
        if (!error)
          error = add_edge (graph, first, first + 2, 1);
        if (!error)
          error = add_edge (graph, first, first + 3, 1);
        if (!error)
          error = add_edge (graph, first + 1, first, 2);
      }
    if (!error)
      {
        obj = handle_object (dwg, h[0]);
        _param = obj->tio.object->tio.BLOCKLINEARPARAMETER;
        error = init_element (dwg, graph, obj, "Linear");
        _param->show_properties = 1;
        _param->chain_actions = 0;
        _param->def_basept = *base_pt;
        _param->def_endpt = *end_pt;
        _param->upd_basept = _param->basept = *base_pt;
        _param->upd_endpt = _param->endpt = *end_pt;
        _param->parameter_base_location = 0;
        // the grip's X and Y displacements
        _param->prop3.connections = (Dwg_BLOCKPARAMETER_connection *)calloc (
            1, sizeof (Dwg_BLOCKPARAMETER_connection));
        _param->prop4.connections = (Dwg_BLOCKPARAMETER_connection *)calloc (
            1, sizeof (Dwg_BLOCKPARAMETER_connection));
        _param->prop_states = (BITCODE_BL *)calloc (4, sizeof (BITCODE_BL));
        if (!_param->prop3.connections || !_param->prop4.connections
            || !_param->prop_states)
          error = DWG_ERR_OUTOFMEM;
        else
          {
            _param->prop3.num_connections = 1;
            _param->prop3.connections[0].code = ids[1];
            _param->prop3.connections[0].name
                = new_string (dwg, connections[0]);
            _param->prop4.num_connections = 1;
            _param->prop4.connections[0].code = ids[1];
            _param->prop4.connections[0].name
                = new_string (dwg, connections[1]);
            _param->prop_states[1] = ids[1];
          }
        // value_set.desc, which only DXF and JSON have, stays NULL: the
        // DWG free doesn't free it
        _param->distance_name = new_string (dwg, name);
        _param->distance_desc = new_string (dwg, "");
        _param->distance = 0.0;
        if (!error
            && (!_param->prop3.connections[0].name
                || !_param->prop4.connections[0].name
                || !_param->distance_name || !_param->distance_desc))
          error = DWG_ERR_OUTOFMEM;
      }
    if (!error)
      {
        obj = handle_object (dwg, h[1]);
        _grip = obj->tio.object->tio.BLOCKLINEARGRIP;
        error = init_element (dwg, graph, obj, "End Grip");
        _grip->bg_bl91 = ids[2];
        _grip->bg_bl92 = ids[3];
        _grip->bg_location = *end_pt;
        _grip->bg_insert_cycling = 0;
        _grip->bg_insert_cycling_weight = -1;
        _grip->orientation.x = end_pt->x - base_pt->x;
        _grip->orientation.y = end_pt->y - base_pt->y;
        _grip->orientation.z = end_pt->z - base_pt->z;
      }
    // the grip's position: the parameter's end point, as it moves
    for (int k = 0; !error && k < 2; k++)
      {
        Dwg_Object_BLOCKGRIPLOCATIONCOMPONENT *_loc;
        obj = handle_object (dwg, h[2 + k]);
        init_evalexpr (obj);
        _loc = obj->tio.object->tio.BLOCKGRIPLOCATIONCOMPONENT;
        _loc->grip_type = ids[0];
        _loc->grip_expr = new_string (dwg, exprs[k]);
        if (!_loc->grip_expr)
          error = DWG_ERR_OUTOFMEM;
      }
    if (!error)
      error = number_entities (dwg, ab.blk);
    free_block (&ab);
    if (errorp)
      *errorp = error;
    return error >= DWG_ERR_CRITICAL ? NULL : _param;
  }
#else
  free_block (&ab);
  return refused_linear (errorp, AUTHOR_REFUSED | DWG_ERR_NOTYETSUPPORTED);
#endif
}

#ifdef USE_WRITE
/* Makes obj, a new action, a node of ab's graph that takes the X and Y
   deltas of the end point of the linear parameter at node index index:
   its owner, node, edge, element fields, display location and
   connections. */
static int
add_action (Author_Block *ab, Dwg_Object *obj, const int index,
            const char *base, const BITCODE_3BD *display,
            Dwg_BLOCKACTION_connectionpts *conn)
{
  Dwg_Data *dwg = ab->dwg;
  Dwg_Object_EVALUATION_GRAPH *graph = graph_of (dwg, ab->graph);
  // every action starts with BLOCKACTION_fields
  Dwg_Object_BLOCKMOVEACTION *_a = obj->tio.object->tio.BLOCKMOVEACTION;
  const BITCODE_BL param_id = (BITCODE_BL)graph->nodes[index].nextid;
  const BITCODE_BL node = graph->num_nodes;
  Dwg_Object *graphobj = handle_object (dwg, ab->graph);
  int error;
  // a graph read with its raw bits must be written from its fields now,
  // which give AutoCAD's bits (dynblocks_test)
  free (graphobj->unknown_bits);
  graphobj->unknown_bits = NULL;
  graphobj->num_unknown_bits = 0;
  error = set_owner (dwg, obj, ab->graph, 0);
  if (error)
    return error;
  if (!add_node (dwg, graph, obj))
    return DWG_ERR_OUTOFMEM;
  error = add_edge (graph, (BITCODE_BL)index, node, 2);
  if (error)
    return error;
  error = init_element (dwg, graph, obj, base);
  if (error)
    return error;
  _a->display_location = *display;
  conn[0].code = param_id;
  conn[0].name = new_string (dwg, "EndXDelta");
  conn[1].code = param_id;
  conn[1].name = new_string (dwg, "EndYDelta");
  return conn[0].name && conn[1].name ? 0 : DWG_ERR_OUTOFMEM;
}
#endif

static Dwg_Object_BLOCKSTRETCHACTION *
refused_stretch (int *errorp, const int error)
{
  if (errorp)
    *errorp = error;
  return NULL;
}

/* The direction of the end point of param, the change of the points an
   action of it moves, at a length of 1000: dynblock_leaves_plane (), which
   allows a change out of a 2D entity's plane of up to 1e-9, as the
   evaluator does, then allows one of 1e-12 of the change, so that any
   value the parameter is likely given keeps within the evaluator's. */
static void
param_direction (const Dwg_Object_BLOCKLINEARPARAMETER *param,
                 BITCODE_3BD *dir)
{
  const double dx = param->def_endpt.x - param->def_basept.x;
  const double dy = param->def_endpt.y - param->def_basept.y;
  const double dz = param->def_endpt.z - param->def_basept.z;
  // as distance_3d () in the evaluator: the squares of a finite length
  // can overflow
  const double length = hypot (hypot (dx, dy), dz);
  dir->x = length > 0.0 ? dx / length * 1000.0 : 0.0;
  dir->y = length > 0.0 ? dy / length * 1000.0 : 0.0;
  dir->z = length > 0.0 ? dz / length * 1000.0 : 0.0;
}

/* The stretch points of the entities to stretch: the entity, and the
   indexes of its points in the frame. */
typedef struct _author_stretched
{
  BITCODE_RLL handle;
  BITCODE_BS num_indexes;
  BITCODE_BL *indexes;
} Author_Stretched;

#ifdef USE_WRITE
static int
compare_stretched (const void *pa, const void *pb)
{
  const BITCODE_RLL a = ((const Author_Stretched *)pa)->handle;
  const BITCODE_RLL b = ((const Author_Stretched *)pb)->handle;
  return a < b ? -1 : a > b ? 1 : 0;
}
#endif

static void
free_stretched (Author_Stretched *s, const BITCODE_BL n)
{
  for (BITCODE_BL i = 0; s && i < n; i++)
    free (s[i].indexes);
  free (s);
}

/* The stretch points of entity obj in the frame lo, hi, into *s. dir is
   a change of the stretch's parameter's end point. */
static int
stretched_points (const Dwg_Data *dwg, const Dwg_Object *obj,
                  const BITCODE_2RD *lo, const BITCODE_2RD *hi,
                  const BITCODE_3BD *dir, Author_Stretched *s)
{
  static const char func[] = "dwg_dynblock_add_stretch";
  const BITCODE_BL count = dynblock_stretch_points (obj);
  s->handle = obj->handle.value;
  if (dynblock_leaves_plane (dwg, obj, dir))
    {
      LOG_ERROR ("%s: stretching %s " FORMAT_HV " would take it, or an "
                 "ATTRIB it owns, out of its plane, which is not supported",
                 func, dwg_type_name (obj->fixedtype), obj->handle.value);
      return AUTHOR_REFUSED | DWG_ERR_NOTYETSUPPORTED;
    }
  if (!count)
    {
      LOG_ERROR ("%s: stretching %s " FORMAT_HV " is not supported", func,
                 dwg_type_name (obj->fixedtype), obj->handle.value);
      return AUTHOR_REFUSED | DWG_ERR_NOTYETSUPPORTED;
    }
  // a BS counts them
  if (count > 0x7FFF)
    {
      LOG_ERROR ("%s: %s " FORMAT_HV " has too many points", func,
                 dwg_type_name (obj->fixedtype), obj->handle.value);
      return AUTHOR_REFUSED | DWG_ERR_NOTYETSUPPORTED;
    }
  s->indexes = (BITCODE_BL *)calloc (count, sizeof (BITCODE_BL));
  if (!s->indexes)
    return DWG_ERR_OUTOFMEM;
  for (BITCODE_BL i = 0; i < count; i++)
    {
      BITCODE_3BD pt;
      if (!dynblock_stretch_point (obj, i, &pt))
        {
          LOG_ERROR ("%s: where the stretch points of %s " FORMAT_HV
                     " are is not known: stretching it is not supported",
                     func, dwg_type_name (obj->fixedtype), obj->handle.value);
          return AUTHOR_REFUSED | DWG_ERR_NOTYETSUPPORTED;
        }
      if (pt.x >= lo->x && pt.x <= hi->x && pt.y >= lo->y && pt.y <= hi->y)
        s->indexes[s->num_indexes++] = i;
    }
  if (!s->num_indexes)
    {
      LOG_ERROR ("%s: %s " FORMAT_HV " has no stretch point in the frame; "
                 "moving an entity outside the frame is not supported",
                 func, dwg_type_name (obj->fixedtype), obj->handle.value);
      return AUTHOR_REFUSED | DWG_ERR_NOTYETSUPPORTED;
    }
  return 0;
}

EXPORT Dwg_Object_BLOCKSTRETCHACTION *
dwg_dynblock_add_stretch (Dwg_Object_BLOCK_HEADER *blk,
                          const Dwg_Object_BLOCKLINEARPARAMETER *param,
                          const BITCODE_2RD *corner1,
                          const BITCODE_2RD *corner2,
                          const BITCODE_RLL *entities,
                          const BITCODE_BL num_entities, int *errorp)
{
  static const char func[] = "dwg_dynblock_add_stretch";
  Author_Block ab;
  Author_Stretched *stretched = NULL;
  const Dwg_Object *paramobj;
  BITCODE_2RD lo, hi;
  BITCODE_3BD dir;
  int index = -1, error;
  if (errorp)
    *errorp = 0;
  if (!blk || !param || !corner1 || !corner2 || !entities)
    return refused_stretch (errorp, AUTHOR_REFUSED | DWG_ERR_INTERNALERROR);
  error = author_block (blk, func, &ab);
  if (!error)
    error = check_param (&ab, param, func, &paramobj, &index);
  if (!error)
    error = check_ids (&ab, 1, func);
  if (!error
      && (!isfinite (corner1->x) || !isfinite (corner1->y)
          || !isfinite (corner2->x) || !isfinite (corner2->y)
          || corner1->x == corner2->x || corner1->y == corner2->y))
    {
      LOG_ERROR ("%s: the corners aren't finite, or don't span a rectangle",
                 func);
      error = AUTHOR_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
    }
  if (!error)
    error = check_selection (entities, num_entities, func);
  if (!error)
    {
      stretched = (Author_Stretched *)calloc (num_entities,
                                              sizeof (Author_Stretched));
      if (!stretched)
        error = DWG_ERR_OUTOFMEM;
    }
  lo.x = fmin (corner1->x, corner2->x);
  lo.y = fmin (corner1->y, corner2->y);
  hi.x = fmax (corner1->x, corner2->x);
  hi.y = fmax (corner1->y, corner2->y);
  param_direction (param, &dir);
  for (BITCODE_BL i = 0; !error && i < num_entities; i++)
    {
      const Dwg_Object *obj = block_entity (&ab, entities[i], func, &error);
      if (obj)
        error = check_turned (&ab, obj, func);
      if (obj && !error)
        error = stretched_points (ab.dwg, obj, &lo, &hi, &dir, &stretched[i]);
    }
  if (error)
    {
      free_stretched (stretched, num_entities);
      free_block (&ab);
      return refused_stretch (errorp, error);
    }

#ifdef USE_WRITE
  {
    Dwg_Data *dwg = ab.dwg;
    Dwg_Object_BLOCKSTRETCHACTION *_a = NULL;
    Dwg_Object *obj = new_object (dwg, DWG_TYPE_BLOCKSTRETCHACTION,
                                  "BLOCKSTRETCHACTION", "BLOCKSTRETCHACTION",
                                  dwg_setup_BLOCKSTRETCHACTION);
    BITCODE_3BD display;
    // halves first: the sum of two finite corners can overflow
    display.x = lo.x / 2.0 + hi.x / 2.0;
    display.y = lo.y / 2.0 + hi.y / 2.0;
    display.z = 0.0;
    if (!obj)
      error = DWG_ERR_OUTOFMEM;
    else
      {
        _a = obj->tio.object->tio.BLOCKSTRETCHACTION;
        error = add_action (&ab, obj, index, "Stretch", &display,
                            _a->conn_pts);
      }
    if (!error)
      {
        // the selection, in the order given; the entities with their
        // stretch points in the frame by handle, and the frame by two
        // corners, as AutoCAD lists them
        _a->deps = (BITCODE_H *)calloc (num_entities, sizeof (BITCODE_H));
        _a->hdls = (Dwg_BLOCKSTRETCHACTION_handles *)calloc (
            num_entities, sizeof (Dwg_BLOCKSTRETCHACTION_handles));
        _a->pts = (BITCODE_2RD *)calloc (2, sizeof (BITCODE_2RD));
        if (!_a->deps || !_a->hdls || !_a->pts)
          error = DWG_ERR_OUTOFMEM;
      }
    // the handles saved before new_object (), not entities, which may
    // point into dwg->object, which adding an object can move; not yet
    // sorted, they are in the order given
    for (BITCODE_BL i = 0; !error && i < num_entities; i++)
      {
        _a->deps[i] = dwg_add_handleref (dwg, 4, stretched[i].handle, NULL);
        if (!_a->deps[i])
          error = DWG_ERR_OUTOFMEM;
        _a->num_deps = i + 1;
      }
    if (!error)
      qsort (stretched, num_entities, sizeof (Author_Stretched),
             compare_stretched);
    for (BITCODE_BL i = 0; !error && i < num_entities; i++)
      {
        Dwg_BLOCKSTRETCHACTION_handles *h = &_a->hdls[i];
        h->parent = _a;
        h->hdl = dwg_add_handleref (dwg, 4, stretched[i].handle, NULL);
        h->num_indexes = stretched[i].num_indexes;
        h->indexes = stretched[i].indexes;
        stretched[i].indexes = NULL;
        _a->num_hdls = i + 1;
        if (!h->hdl)
          error = DWG_ERR_OUTOFMEM;
      }
    if (!error)
      {
        _a->num_pts = 2;
        _a->pts[0] = hi;
        _a->pts[1] = lo;
        _a->action_offset_x = 1.0;
        _a->action_offset_y = 0.0;
        _a->xy_type = 0;
        error = number_entities (dwg, ab.blk);
      }
    free_stretched (stretched, num_entities);
    free_block (&ab);
    if (errorp)
      *errorp = error;
    return error >= DWG_ERR_CRITICAL ? NULL : _a;
  }
#else
  free_stretched (stretched, num_entities);
  free_block (&ab);
  return refused_stretch (errorp, AUTHOR_REFUSED | DWG_ERR_NOTYETSUPPORTED);
#endif
}

static Dwg_Object_BLOCKMOVEACTION *
refused_move (int *errorp, const int error)
{
  if (errorp)
    *errorp = error;
  return NULL;
}

/* Whether an action of the linear parameter with node id from moves the
   points of the linear parameter with node id to, directly or through the
   actions of the parameters it moves: a stretch listing it among its codes,
   or a move among the parameters it moves. The evaluator can't order
   linear parameters that move each other in a cycle. */
static int
moves_linear (const Dwg_Data *dwg, const Dwg_Object_EVALUATION_GRAPH *graph,
              const BITCODE_BL from, const BITCODE_BL to)
{
  BITCODE_BL *queue = (BITCODE_BL *)calloc (graph->num_nodes + 1,
                                            sizeof (BITCODE_BL));
  unsigned char *seen = (unsigned char *)calloc (graph->num_nodes + 1, 1);
  BITCODE_BL head = 0, tail = 0;
  int found = 0;
  if (!queue || !seen)
    {
      free (queue);
      free (seen);
      // as if it did: the move is refused
      return 1;
    }
  queue[tail++] = from;
  while (head < tail && !found)
    {
      const BITCODE_BL id = queue[head++];
      for (BITCODE_BL i = 0; !found && i < graph->num_nodes; i++)
        {
          const Dwg_Object *a = node_object (dwg, graph, i);
          BITCODE_BL n = 0;
          const BITCODE_BL *moved = NULL;
          const Dwg_BLOCKSTRETCHACTION_codes *codes = NULL;
          if (!a)
            continue;
          if (a->fixedtype == DWG_TYPE_BLOCKMOVEACTION)
            {
              const Dwg_Object_BLOCKMOVEACTION *_a
                  = a->tio.object->tio.BLOCKMOVEACTION;
              if (_a->conn_pts[0].code != id)
                continue;
              n = _a->actions ? _a->num_actions : 0;
              moved = _a->actions;
            }
          else if (a->fixedtype == DWG_TYPE_BLOCKSTRETCHACTION)
            {
              const Dwg_Object_BLOCKSTRETCHACTION *_a
                  = a->tio.object->tio.BLOCKSTRETCHACTION;
              if (_a->conn_pts[0].code != id)
                continue;
              n = _a->codes ? _a->num_codes : 0;
              codes = _a->codes;
            }
          else
            continue;
          for (BITCODE_BL j = 0; !found && j < n; j++)
            {
              const BITCODE_BL next = moved ? moved[j] : codes[j].bl95;
              BITCODE_BL k;
              if (next == to)
                found = 1;
              // queue each linear parameter once
              for (k = 0; k < graph->num_nodes; k++)
                if ((BITCODE_BL)graph->nodes[k].nextid == next)
                  break;
              if (!found && k < graph->num_nodes && !seen[k]
                  && node_object (dwg, graph, k)
                  && node_object (dwg, graph, k)->fixedtype
                         == DWG_TYPE_BLOCKLINEARPARAMETER)
                {
                  seen[k] = 1;
                  queue[tail++] = next;
                }
            }
        }
    }
  free (queue);
  free (seen);
  return found;
}

/* Whether obj is a parameter that a move may carry. */
static int
is_carried_kind (const Dwg_Object *obj)
{
  return obj->fixedtype == DWG_TYPE_BLOCKLINEARPARAMETER
         || obj->fixedtype == DWG_TYPE_BLOCKVISIBILITYPARAMETER
         || obj->fixedtype == DWG_TYPE_BLOCKFLIPPARAMETER;
}

/* Checks object handle of a move's selection, an entity of ab's block or a
   parameter of its graph other than node param_index, which the move
   carries; the node index of a parameter goes into *nodep, -1 for an
   entity. dir is a change of the move's parameter's end point. */
static int
check_moved (const Author_Block *ab, const BITCODE_RLL handle,
             const int param_index, const BITCODE_3BD *dir, int *nodep)
{
  static const char func[] = "dwg_dynblock_add_move";
  const Dwg_Object *obj = handle_object (ab->dwg, handle);
  const Dwg_Object_EVALUATION_GRAPH *graph = graph_of (ab->dwg, ab->graph);
  const int node = node_index (graph, handle);
  *nodep = -1;
  if (obj && obj->supertype == DWG_SUPERTYPE_ENTITY
      && hash_get (ab->entities, handle) != HASH_NOT_FOUND)
    {
      if (!dynblock_is_movable (obj))
        {
          LOG_ERROR ("%s: moving %s " FORMAT_HV " is not supported", func,
                     dwg_type_name (obj->fixedtype), handle);
          return AUTHOR_REFUSED | DWG_ERR_NOTYETSUPPORTED;
        }
      if (dynblock_leaves_plane (ab->dwg, obj, dir))
        {
          LOG_ERROR ("%s: moving %s " FORMAT_HV " out of its plane is not "
                     "supported",
                     func, dwg_type_name (obj->fixedtype), handle);
          return AUTHOR_REFUSED | DWG_ERR_NOTYETSUPPORTED;
        }
      return check_turned (ab, obj, func);
    }
  if (!obj || node < 0)
    {
      LOG_ERROR ("%s: " FORMAT_HV " is neither an entity of the block nor "
                 "a parameter of its graph",
                 func, handle);
      return AUTHOR_REFUSED | DWG_ERR_INVALIDHANDLE;
    }
  // every parameter starts with BLOCKPARAMETER_fields
  if (node == param_index || !is_carried_kind (obj)
      || obj->tio.object->tio.BLOCKVISIBILITYPARAMETER->chain_actions)
    {
      LOG_ERROR ("%s: moving %s " FORMAT_HV " is not supported%s", func,
                 dwg_type_name (obj->fixedtype), handle,
                 node == param_index ? ": it is the move's own parameter"
                 : is_carried_kind (obj) ? ": it chains its actions"
                                         : "");
      return AUTHOR_REFUSED | DWG_ERR_NOTYETSUPPORTED;
    }
  if (obj->fixedtype == DWG_TYPE_BLOCKLINEARPARAMETER
      && moves_linear (ab->dwg, graph, (BITCODE_BL)graph->nodes[node].nextid,
                       (BITCODE_BL)graph->nodes[param_index].nextid))
    {
      LOG_ERROR ("%s: moving %s " FORMAT_HV " is not supported: its "
                 "actions move the move's own parameter, a cycle",
                 func, dwg_type_name (obj->fixedtype), handle);
      return AUTHOR_REFUSED | DWG_ERR_NOTYETSUPPORTED;
    }
  *nodep = node;
  return check_turned (ab, obj, func);
}

/* An object of a move's selection: its handle, saved before the move adds
   objects, as the selection may point into dwg->object, which adding an
   object can move; and the node index of a parameter the move carries, -1
   for an entity. */
typedef struct _author_moved
{
  BITCODE_RLL handle;
  int node;
} Author_Moved;

EXPORT Dwg_Object_BLOCKMOVEACTION *
dwg_dynblock_add_move (Dwg_Object_BLOCK_HEADER *blk,
                       const Dwg_Object_BLOCKLINEARPARAMETER *param,
                       const BITCODE_RLL *objects,
                       const BITCODE_BL num_objects, const double multiplier,
                       int *errorp)
{
  static const char func[] = "dwg_dynblock_add_move";
  Author_Block ab;
  const Dwg_Object *paramobj;
  Author_Moved *moved = NULL;
  int index = -1, error;
  BITCODE_BL num_deps = 0, num_carried = 0;
  BITCODE_3BD dir;
  if (errorp)
    *errorp = 0;
  if (!blk || !param || !objects)
    return refused_move (errorp, AUTHOR_REFUSED | DWG_ERR_INTERNALERROR);
  error = author_block (blk, func, &ab);
  if (!error)
    error = check_param (&ab, param, func, &paramobj, &index);
  if (!error)
    error = check_ids (&ab, 1, func);
  if (!error && !isfinite (multiplier))
    {
      LOG_ERROR ("%s: the multiplier isn't finite", func);
      error = AUTHOR_REFUSED | DWG_ERR_VALUEOUTOFBOUNDS;
    }
  if (!error)
    error = check_selection (objects, num_objects, func);
  if (!error)
    {
      moved = (Author_Moved *)calloc (num_objects, sizeof (Author_Moved));
      if (!moved)
        error = DWG_ERR_OUTOFMEM;
    }
  param_direction (param, &dir);
  for (BITCODE_BL i = 0; !error && i < num_objects; i++)
    {
      moved[i].handle = objects[i];
      error = check_moved (&ab, objects[i], index, &dir, &moved[i].node);
      num_deps++;
      if (!error && moved[i].node >= 0)
        {
          // and the parameter's grips
          const Dwg_Object_EVALUATION_GRAPH *graph
              = graph_of (ab.dwg, ab.graph);
          num_carried++;
          for (BITCODE_BL j = 0; graph->edges && j < graph->num_edges; j++)
            if (graph->edges[j].e3 == moved[i].node
                && graph->edges[j].e2 >= 0
                && (BITCODE_BL)graph->edges[j].e2 < graph->num_nodes)
              {
                const Dwg_Object *grip = node_object (
                    ab.dwg, graph, (BITCODE_BL)graph->edges[j].e2);
                if (grip && is_grip (grip))
                  num_deps++;
              }
        }
    }
  if (error)
    {
      free (moved);
      free_block (&ab);
      return refused_move (errorp, error);
    }

#ifdef USE_WRITE
  {
    Dwg_Data *dwg = ab.dwg;
    Dwg_Object_BLOCKMOVEACTION *_a = NULL;
    Dwg_Object *obj = new_object (dwg, DWG_TYPE_BLOCKMOVEACTION,
                                  "BLOCKMOVEACTION", "BLOCKMOVEACTION",
                                  dwg_setup_BLOCKMOVEACTION);
    BITCODE_BL d = 0, c = 0;
    if (!obj)
      error = DWG_ERR_OUTOFMEM;
    else
      {
        _a = obj->tio.object->tio.BLOCKMOVEACTION;
        error = add_action (&ab, obj, index, "Move", &param->def_endpt,
                            _a->conn_pts);
      }
    if (!error)
      {
        _a->deps = (BITCODE_H *)calloc (num_deps, sizeof (BITCODE_H));
        _a->actions = num_carried ? (BITCODE_BL *)calloc (
                                        num_carried, sizeof (BITCODE_BL))
                                  : NULL;
        if (!_a->deps || (num_carried && !_a->actions))
          error = DWG_ERR_OUTOFMEM;
      }
    // the selection, in the order given, each parameter followed by its
    // grips; and the parameters by node id, as AutoCAD lists them
    for (BITCODE_BL i = 0; !error && i < num_objects; i++)
      {
        const Dwg_Object_EVALUATION_GRAPH *graph = graph_of (dwg, ab.graph);
        _a->deps[d] = dwg_add_handleref (dwg, 4, moved[i].handle, NULL);
        if (!_a->deps[d++])
          error = DWG_ERR_OUTOFMEM;
        if (moved[i].node < 0)
          continue;
        _a->actions[c++] = (BITCODE_BL)graph->nodes[moved[i].node].nextid;
        for (BITCODE_BL j = 0; !error && graph->edges && j < graph->num_edges;
             j++)
          if (graph->edges[j].e3 == moved[i].node
              && graph->edges[j].e2 >= 0
              && (BITCODE_BL)graph->edges[j].e2 < graph->num_nodes)
            {
              const Dwg_Object *grip = node_object (
                  dwg, graph, (BITCODE_BL)graph->edges[j].e2);
              if (grip && is_grip (grip))
                {
                  _a->deps[d] = dwg_add_handleref (dwg, 4,
                                                   grip->handle.value, NULL);
                  if (!_a->deps[d++])
                    error = DWG_ERR_OUTOFMEM;
                }
            }
      }
    if (_a)
      {
        _a->num_deps = d;
        _a->num_actions = c;
      }
    if (!error)
      {
        _a->action_offset_x = multiplier;
        _a->action_offset_y = 0.0;
        _a->xy_type = 0;
        error = number_entities (dwg, ab.blk);
      }
    free (moved);
    free_block (&ab);
    if (errorp)
      *errorp = error;
    return error >= DWG_ERR_CRITICAL ? NULL : _a;
  }
#else
  free (moved);
  free_block (&ab);
  return refused_move (errorp, AUTHOR_REFUSED | DWG_ERR_NOTYETSUPPORTED);
#endif
}
