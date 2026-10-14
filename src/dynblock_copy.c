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
 * dynblock_copy.c: copy a block definition, with its dynamic-block data,
 * from one document into another, with the object copy of copy.c.
 *
 * The objects to copy are found by copying into a scratch document first.
 * The copy reports every reference to an object it wasn't given: an object
 * owned by one being copied is part of the definition and is copied next,
 * and the others are matched in dst, or are table records and blocks to
 * copy as well. Following references, rather than taking every object whose
 * owner is in the definition, leaves out the objects a dictionary still
 * names as its owner but no longer lists, such as the evaluation graphs a
 * block kept from earlier edits. Then everything is copied into dst in one
 * go, so that a refusal, which the scratch copy meets first, changes
 * nothing in dst.
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "dwg.h"
#include "hash.h"
#include "copy.h"

#define DWG_LOGLEVEL loglevel
#include "logging.h"
#include "dwg_api.h"
#include "dwg_dynblock_api.h"

/* What a refused copy returns, with a bit for the reason, as copy.c does. */
#define BLOCK_COPY_REFUSED DWG_ERR_INVALIDDWG

/* How an object in the copy came to be copied. */
typedef enum BLOCK_COPY_ROLE
{
  /* A block definition, or an object one owns, with what it owns. */
  ROLE_PART = 1,
  /* A table record dst lacks, without what it owns. */
  ROLE_RECORD
} Block_Copy_Role;

/* What became of a referenced object. */
typedef enum BLOCK_COPY_DECISION
{
  DECIDED_COPY = 1,
  DECIDED_MATCHED,
  DECIDED_LEFT_OUT,
  /* left out unless its owner turns out to be copied */
  DECIDED_DEFERRED
} Block_Copy_Decision;

typedef struct _handle_list
{
  BITCODE_RLL *handles;
  BITCODE_BL num;
  BITCODE_BL max;
} Handle_List;

typedef struct _block_copy
{
  const Dwg_Data *src;
  Dwg_Data *dst;
  Handle_List copy;    /* src objects to copy, in the order found */
  Handle_List pending; /* the next ones to copy into the scratch document */
  Handle_List matched_src, matched_dst; /* src objects matched in dst */
  Handle_List deferred;                 /* DECIDED_DEFERRED objects */
  dwg_inthash *role;    /* src handle: Block_Copy_Role, of each in copy */
  dwg_inthash *decided; /* src handle: Block_Copy_Decision */
  Dwg_Copy_Map *scratch_map;
  int error;
} Block_Copy;

static int
list_add (Handle_List *restrict list, const BITCODE_RLL handle)
{
  if (list->num == list->max)
    {
      const BITCODE_BL max = list->max ? list->max * 2 : 64;
      BITCODE_RLL *handles
          = (BITCODE_RLL *)realloc (list->handles, max * sizeof (BITCODE_RLL));
      if (!handles)
        return DWG_ERR_OUTOFMEM;
      list->handles = handles;
      list->max = max;
    }
  list->handles[list->num++] = handle;
  return 0;
}

static BITCODE_RLL
ref_target (const Dwg_Object_Ref *ref)
{
  return ref ? ref->absolute_ref : 0;
}

static BITCODE_RLL
owner_of (const Dwg_Object *obj)
{
  if (!obj->tio.object)
    return 0;
  if (obj->supertype == DWG_SUPERTYPE_ENTITY)
    return ref_target (obj->tio.entity->ownerhandle);
  return ref_target (obj->tio.object->ownerhandle);
}

/* Table and dictionary names compare without case. Both documents have the
   same codepage, so their bytes can be compared; only ASCII letters fold. */
static int
same_name (const char *a, const size_t alen, const char *b, const size_t blen)
{
  size_t i;
  if (alen != blen)
    return 0;
  for (i = 0; i < alen; i++)
    {
      unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
      if (ca >= 'A' && ca <= 'Z')
        ca += 'a' - 'A';
      if (cb >= 'A' && cb <= 'Z')
        cb += 'a' - 'A';
      if (ca != cb)
        return 0;
    }
  return 1;
}

static int
same_cstr (const char *a, const char *b)
{
  return a && b && same_name (a, strlen (a), b, strlen (b));
}

static int
starts_with (const char *name, const char *prefix)
{
  const size_t len = strlen (prefix);
  return name && strlen (name) >= len && same_name (name, len, prefix, len);
}

/* The name of a dynamic block definition: the string of its
   AcDbDynamicBlockTrueName EED, or AcDbDynamicBlockTrueName2 as AutoCAD
   writes for some, which is not NUL-terminated, or NULL. */
static const char *
true_name (const Dwg_Data *dwg, const Dwg_Object *obj, size_t *lenp)
{
  const Dwg_Object_Object *o = obj->tio.object;
  BITCODE_BL i;
  for (i = 0; i < o->num_eed; i++)
    {
      const Dwg_Eed *eed = &o->eed[i];
      const Dwg_Object *appid;
      const char *appname;
      /* The first item of a group has its size and APPID. */
      if (!eed->size || !eed->data)
        continue;
      appid = dwg_resolve_handle (dwg, eed->handle.value);
      if (!appid || appid->fixedtype != DWG_TYPE_APPID || !appid->tio.object
          || !appid->tio.object->tio.APPID)
        continue;
      appname = appid->tio.object->tio.APPID->name;
      if (!same_cstr (appname, "AcDbDynamicBlockTrueName")
          && !same_cstr (appname, "AcDbDynamicBlockTrueName2"))
        continue;
      if (eed->data->code != 0 || eed->data->u.eed_0.is_tu
          || !eed->data->u.eed_0.length)
        continue;
      *lenp = eed->data->u.eed_0.length;
      return eed->data->u.eed_0.string;
    }
  return NULL;
}

/* A block's name for matching: a dynamic definition's true name, or the
   name of a block that isn't anonymous. NULL for anonymous blocks, which
   are all stored as "*U", "*D" and the like. */
static const char *
block_name (const Dwg_Data *dwg, const Dwg_Object *obj, size_t *lenp)
{
  const Dwg_Object_BLOCK_HEADER *blk = obj->tio.object->tio.BLOCK_HEADER;
  const char *name = true_name (dwg, obj, lenp);
  if (name)
    return name;
  if (!blk || blk->anonymous || !blk->name || !*blk->name || *blk->name == '*')
    return NULL;
  *lenp = strlen (blk->name);
  return blk->name;
}

static Dwg_Object_Type
control_type (const Dwg_Object_Type type)
{
  switch (type)
    {
    case DWG_TYPE_BLOCK_HEADER:
      return DWG_TYPE_BLOCK_CONTROL;
    case DWG_TYPE_LAYER:
      return DWG_TYPE_LAYER_CONTROL;
    case DWG_TYPE_STYLE:
      return DWG_TYPE_STYLE_CONTROL;
    case DWG_TYPE_LTYPE:
      return DWG_TYPE_LTYPE_CONTROL;
    case DWG_TYPE_VIEW:
      return DWG_TYPE_VIEW_CONTROL;
    case DWG_TYPE_UCS:
      return DWG_TYPE_UCS_CONTROL;
    case DWG_TYPE_VPORT:
      return DWG_TYPE_VPORT_CONTROL;
    case DWG_TYPE_APPID:
      return DWG_TYPE_APPID_CONTROL;
    case DWG_TYPE_DIMSTYLE:
      return DWG_TYPE_DIMSTYLE_CONTROL;
    case DWG_TYPE_VX_TABLE_RECORD:
      return DWG_TYPE_VX_CONTROL;
    default:
      return DWG_TYPE_UNUSED;
    }
}

/* Every control object starts with these fields. */
static Dwg_Object_APPID_CONTROL *
control_of (const Dwg_Object *ctrl)
{
  return ctrl && ctrl->tio.object ? ctrl->tio.object->tio.APPID_CONTROL : NULL;
}

/* Model space, paper space and other layout blocks, xrefs and blocks that
   depend on one aren't block definitions that can be copied. */
static int
is_copyable_block (const Dwg_Data *dwg, const Dwg_Object *obj)
{
  const Dwg_Object_BLOCK_HEADER *blk = obj->tio.object->tio.BLOCK_HEADER;
  const BITCODE_RLL handle = obj->handle.value;
  if (!blk)
    return 0;
  if (handle == ref_target (dwg->header_vars.BLOCK_RECORD_MSPACE)
      || handle == ref_target (dwg->header_vars.BLOCK_RECORD_PSPACE)
      || ref_target (blk->layout))
    return 0;
  if (starts_with (blk->name, "*Model_Space")
      || starts_with (blk->name, "*Paper_Space"))
    return 0;
  return !blk->blkisxref && !blk->xrefoverlaid && !blk->is_xref_dep;
}

/* The block of dst named as srcobj is in src, or 0. */
static BITCODE_RLL
find_block (const Dwg_Data *dst, const Dwg_Data *src, const Dwg_Object *srcobj)
{
  const Dwg_Object_APPID_CONTROL *ctrl
      = control_of (dwg_get_first_object (dst, DWG_TYPE_BLOCK_CONTROL));
  size_t len, dst_len;
  const char *name = block_name (src, srcobj, &len);
  BITCODE_BS i;
  if (!name || !ctrl || !ctrl->entries)
    return 0;
  for (i = 0; i < ctrl->num_entries; i++)
    {
      const Dwg_Object *obj
          = ctrl->entries[i]
                ? dwg_resolve_handle (dst, ctrl->entries[i]->absolute_ref)
                : NULL;
      const char *dst_name;
      if (!obj || obj->fixedtype != DWG_TYPE_BLOCK_HEADER || !obj->tio.object
          || !obj->tio.object->tio.BLOCK_HEADER)
        continue;
      dst_name = block_name (dst, obj, &dst_len);
      if (dst_name && same_name (name, len, dst_name, dst_len))
        return obj->handle.value;
    }
  return 0;
}

/* Whether table records a and b, of one type, are the same record by name.
   The text styles of shape files have no name, and are the same when their
   files are. */
static int
same_record (const Dwg_Object *a, const Dwg_Object *b)
{
  const char *name = a->tio.object->tio.APPID->name;
  if (a->fixedtype == DWG_TYPE_STYLE && (!name || !*name))
    {
      const Dwg_Object_STYLE *sa = a->tio.object->tio.STYLE;
      const Dwg_Object_STYLE *sb = b->tio.object->tio.STYLE;
      return (!sb->name || !*sb->name) && sa->is_shape == sb->is_shape
             && same_cstr (sa->font_file, sb->font_file);
    }
  return same_cstr (name, b->tio.object->tio.APPID->name);
}

/* The record of dst's table named as table record srcobj is in src, or 0.
   Every table record starts with the fields of an APPID. */
static BITCODE_RLL
find_record (const Dwg_Data *dst, const Dwg_Object *srcobj)
{
  const Dwg_Object *ctrlobj
      = dwg_get_first_object (dst, control_type (srcobj->fixedtype));
  const Dwg_Object_APPID_CONTROL *ctrl = control_of (ctrlobj);
  const char *name = srcobj->tio.object->tio.APPID
                         ? srcobj->tio.object->tio.APPID->name
                         : NULL;
  BITCODE_H extra[2] = { NULL, NULL };
  BITCODE_BS i;
  if (!name || !ctrl)
    return 0;
  /* the linetypes ByLayer and ByBlock aren't among the entries */
  if (ctrlobj->fixedtype == DWG_TYPE_LTYPE_CONTROL)
    {
      extra[0] = ctrlobj->tio.object->tio.LTYPE_CONTROL->bylayer;
      extra[1] = ctrlobj->tio.object->tio.LTYPE_CONTROL->byblock;
    }
  for (i = 0; i < ctrl->num_entries + 2u; i++)
    {
      const BITCODE_H ref = i < ctrl->num_entries
                                ? (ctrl->entries ? ctrl->entries[i] : NULL)
                                : extra[i - ctrl->num_entries];
      const Dwg_Object *obj
          = ref ? dwg_resolve_handle (dst, ref->absolute_ref) : NULL;
      if (obj && obj->fixedtype == srcobj->fixedtype && obj->tio.object
          && obj->tio.object->tio.APPID && same_record (srcobj, obj))
        return obj->handle.value;
    }
  return 0;
}

/* DICTIONARY and DICTIONARYWDFLT start with the same fields. */
static const Dwg_Object_DICTIONARY *
dictionary_of (const Dwg_Object *obj)
{
  if (!obj || !obj->tio.object
      || (obj->fixedtype != DWG_TYPE_DICTIONARY
          && obj->fixedtype != DWG_TYPE_DICTIONARYWDFLT))
    return NULL;
  return obj->tio.object->tio.DICTIONARY;
}

/* The key of the entry of dict that is handle, or NULL. */
static const char *
dictionary_key (const Dwg_Object_DICTIONARY *dict, const BITCODE_RLL handle)
{
  BITCODE_BL i;
  if (!dict || !dict->texts || !dict->itemhandles)
    return NULL;
  for (i = 0; i < dict->numitems; i++)
    if (dict->texts[i] && ref_target (dict->itemhandles[i]) == handle)
      return dict->texts[i];
  return NULL;
}

/* The entry of dict with the key, or 0. */
static BITCODE_RLL
dictionary_entry (const Dwg_Object_DICTIONARY *dict, const char *key)
{
  BITCODE_BL i;
  if (!dict || !dict->texts || !dict->itemhandles || !key)
    return 0;
  for (i = 0; i < dict->numitems; i++)
    if (same_cstr (dict->texts[i], key))
      return ref_target (dict->itemhandles[i]);
  return 0;
}

static const Dwg_Object *
named_object_dictionary (const Dwg_Data *dwg)
{
  const BITCODE_RLL nod
      = ref_target (dwg->header_vars.DICTIONARY_NAMED_OBJECT);
  if (nod)
    return dwg_resolve_handle (dwg, nod);
  // the first DICTIONARY, as dwg_find_dictionary () has it
  return dwg_get_first_object (dwg, DWG_TYPE_DICTIONARY);
}

/* Styles, the entries of the named object dictionary's dictionaries whose
   keys are their names. Not groups, whose reactors would point at a group
   of dst that doesn't list the copy, nor scales or layouts, whose keys are
   arbitrary. */
static int
is_named_style (const Dwg_Object_Type type)
{
  switch (type)
    {
    case DWG_TYPE_PLACEHOLDER: // plot style
    case DWG_TYPE_MLINESTYLE:
    case DWG_TYPE_MLEADERSTYLE:
    case DWG_TYPE_TABLESTYLE:
    case DWG_TYPE_MATERIAL:
    case DWG_TYPE_VISUALSTYLE:
    case DWG_TYPE_DBCOLOR: // colour book colour
      return 1;
    default:
      return 0;
    }
}

/* For a style in one of src's named object dictionary's dictionaries, such
   as a plot style of ACAD_PLOTSTYLENAME, the entry of the same key in dst's
   dictionary of the same name, or 0. A plot style dst lacks is dst's
   default plot style. */
static BITCODE_RLL
find_dictionary_entry (const Dwg_Data *dst, const Dwg_Data *src,
                       const Dwg_Object *srcobj)
{
  const Dwg_Object *dictobj = dwg_resolve_handle (src, owner_of (srcobj));
  const Dwg_Object *nod = named_object_dictionary (src);
  const Dwg_Object *dst_nod = named_object_dictionary (dst);
  const Dwg_Object *dst_dictobj;
  const char *key, *dict_name;
  BITCODE_RLL found;
  if (!is_named_style (srcobj->fixedtype) || !dictobj || !nod || !dst_nod
      || owner_of (dictobj) != nod->handle.value)
    return 0;
  key = dictionary_key (dictionary_of (dictobj), srcobj->handle.value);
  dict_name = dictionary_key (dictionary_of (nod), dictobj->handle.value);
  if (!key || !dict_name)
    return 0;
  dst_dictobj = dwg_resolve_handle (
      dst, dictionary_entry (dictionary_of (dst_nod), dict_name));
  found = dictionary_entry (dictionary_of (dst_dictobj), key);
  if (!found && srcobj->fixedtype == DWG_TYPE_PLACEHOLDER && dst_dictobj
      && dst_dictobj->fixedtype == DWG_TYPE_DICTIONARYWDFLT)
    found
        = ref_target (dst_dictobj->tio.object->tio.DICTIONARYWDFLT->defaultid);
  return found;
}

/*--------------------------------------------------------------------------------
 * Finding what to copy, in a scratch document
 */

static void
decide (Block_Copy *restrict bc, const BITCODE_RLL handle,
        const Block_Copy_Decision decision)
{
  hash_set (bc->decided, handle, (uint64_t)decision);
}

static void
add_copy (Block_Copy *restrict bc, const BITCODE_RLL handle,
          const Block_Copy_Role role)
{
  bc->error |= list_add (&bc->copy, handle);
  bc->error |= list_add (&bc->pending, handle);
  hash_set (bc->role, handle, (uint64_t)role);
  decide (bc, handle, DECIDED_COPY);
}

/* An object of a constraint-based (parametric) block's associative
   network, which LibreDWG would encode from fields that AutoCAD rejects,
   or its evaluation graph's ACDB_DYNAMICBLOCKPROXYNODE, which LibreDWG
   can't decode. */
static int
is_constraint_object (const Dwg_Object *obj)
{
  return obj->dxfname
         && (memBEGINc (obj->dxfname, "ACDBASSOC")
             || strEQc (obj->dxfname, "ACDB_DYNAMICBLOCKPROXYNODE"));
}

/* Copies obj as part of a definition, or refuses the copy. */
static void
add_part (Block_Copy *restrict bc, const Dwg_Object *restrict obj)
{
  if (is_constraint_object (obj))
    {
      LOG_ERROR ("dwg_dynblock_copy_block: %s " FORMAT_HV
                 " belongs to a constraint-based block, which can't be "
                 "copied",
                 obj->dxfname, obj->handle.value);
      bc->error |= BLOCK_COPY_REFUSED | DWG_ERR_NOTYETSUPPORTED;
      return;
    }
  add_copy (bc, obj->handle.value, ROLE_PART);
}

/* An object found before its owner may be owned by an object copied later:
   it is copied if its owner is by the end. */
static void
defer (Block_Copy *restrict bc, const BITCODE_RLL handle)
{
  bc->error |= list_add (&bc->deferred, handle);
  decide (bc, handle, DECIDED_DEFERRED);
}

/* Copies the deferred objects whose owners are copied now. */
static void
copy_deferred (Block_Copy *restrict bc)
{
  BITCODE_BL i, j = 0;
  for (i = 0; i < bc->deferred.num; i++)
    {
      const BITCODE_RLL handle = bc->deferred.handles[i];
      const Dwg_Object *obj = dwg_resolve_handle (bc->src, handle);
      const BITCODE_RLL owner = obj ? owner_of (obj) : 0;
      if (owner && hash_get (bc->role, owner) == ROLE_PART)
        add_part (bc, obj);
      else
        bc->deferred.handles[j++] = handle;
    }
  bc->deferred.num = j;
}

static void
add_match (Block_Copy *restrict bc, const BITCODE_RLL handle,
           const BITCODE_RLL dst_handle)
{
  bc->error |= list_add (&bc->matched_src, handle);
  bc->error |= list_add (&bc->matched_dst, dst_handle);
  // references to it in the next scratch copies are not reported again
  dwg_copy_map_set (bc->scratch_map, handle, dst_handle);
  decide (bc, handle, DECIDED_MATCHED);
}

/* Decides what becomes of the object an unresolved reference refers to. */
static void
classify (Block_Copy *restrict bc, const Dwg_Copy_Unresolved *restrict u)
{
  const BITCODE_RLL target = u->target;
  const Dwg_Object *obj;
  BITCODE_RLL found, owner;

  // a handle value, such as a sort key, which the copy keeps
  if (u->kind == DWG_COPY_SLOT_REF && !u->code)
    return;
  if (hash_get (bc->decided, target) != HASH_NOT_FOUND)
    return;
  obj = dwg_resolve_handle (bc->src, target);
  if (!obj || !obj->tio.object)
    {
      decide (bc, target, DECIDED_LEFT_OUT);
      return;
    }
  owner = owner_of (obj);
  if (owner && hash_get (bc->role, owner) == ROLE_PART)
    {
      add_part (bc, obj);
      return;
    }
  if (obj->fixedtype == DWG_TYPE_BLOCK_HEADER)
    {
      found = find_block (bc->dst, bc->src, obj);
      if (found)
        add_match (bc, target, found);
      else if (is_copyable_block (bc->src, obj))
        add_part (bc, obj);
      else
        decide (bc, target, DECIDED_LEFT_OUT);
    }
  else if (dwg_obj_is_table (obj))
    {
      found = find_record (bc->dst, obj);
      if (found)
        add_match (bc, target, found);
      else
        add_copy (bc, target, ROLE_RECORD);
    }
  else if (dwg_obj_is_control (obj))
    {
      const Dwg_Object *ctrl = dwg_get_first_object (bc->dst, obj->fixedtype);
      if (ctrl)
        add_match (bc, target, ctrl->handle.value);
      else
        decide (bc, target, DECIDED_LEFT_OUT);
    }
  else
    {
      found = find_dictionary_entry (bc->dst, bc->src, obj);
      if (found)
        add_match (bc, target, found);
      else
        defer (bc, target);
    }
}

/* An empty document to copy src's objects into. */
static Dwg_Data *
scratch_new (const Dwg_Data *src, const Dwg_Data *dst)
{
  Dwg_Data *dwg = (Dwg_Data *)calloc (1, sizeof (Dwg_Data));
  if (!dwg)
    return NULL;
  dwg->opts = dst->opts & DWG_OPTS_LOGLEVEL;
  dwg->header.version = dwg->header.from_version = src->header.from_version;
  dwg->header.codepage = src->header.codepage;
  return dwg;
}

/* Fills bc->copy with the objects to copy, starting from the definition
   top, and bc->matched_src/dst with the objects matched in dst. */
static int
find_copy (Block_Copy *restrict bc, const BITCODE_RLL top)
{
  Dwg_Data *scratch = scratch_new (bc->src, bc->dst);
  int error = 0;

  bc->scratch_map = dwg_copy_map_new ();
  if (!scratch || !bc->scratch_map)
    {
      error = DWG_ERR_OUTOFMEM;
      goto done;
    }
  add_part (bc, dwg_resolve_handle (bc->src, top));
  while (!bc->error && bc->pending.num)
    {
      Handle_List batch = bc->pending;
      Dwg_Copy_Unresolved *unresolved = NULL;
      BITCODE_BL num_unresolved = 0, i;
      int rc;
      memset (&bc->pending, 0, sizeof (Handle_List));
      // the copy itself reports what it can't copy
      rc = dwg_copy_objects (scratch, (Dwg_Data *)bc->src, batch.handles,
                             batch.num, bc->scratch_map, &unresolved,
                             &num_unresolved);
      if (rc < DWG_ERR_CRITICAL)
        {
          for (i = 0; i < num_unresolved; i++)
            classify (bc, &unresolved[i]);
          copy_deferred (bc);
        }
      free (unresolved);
      free (batch.handles);
      if (rc >= DWG_ERR_CRITICAL)
        {
          error = rc;
          break;
        }
    }
  error |= bc->error;

done:
  if (scratch)
    {
      dwg_free (scratch);
      free (scratch);
    }
  dwg_copy_map_free (bc->scratch_map);
  bc->scratch_map = NULL;
  // dwg_free sets it from the scratch document's opts
  loglevel = bc->dst->opts & DWG_OPTS_LOGLEVEL;
  return error;
}

/*--------------------------------------------------------------------------------
 * The copy into dst
 */

static int
in_array (const void *slot, const BITCODE_H *array, const BITCODE_BL num)
{
  return array && num && (const BITCODE_H *)slot >= array
         && (const BITCODE_H *)slot < array + num;
}

/* What becomes of a reference the copy into dst left NULL: every object
   that could be matched was in the map before the copy, so these are the
   references that are left out. Reactors and a block's INSERTs are dropped
   afterwards, and a table record loses its extension dictionary. Returns
   DWG_ERR_INVALIDHANDLE for any other. */
static int
left_out (Block_Copy *restrict bc, const Dwg_Copy_Unresolved *restrict u,
          const BITCODE_BL first)
{
  Dwg_Object *holder;
  const Dwg_Object *target;
  BITCODE_BL index;
  if (u->kind == DWG_COPY_SLOT_REF && !u->code)
    return 0;
  holder = dwg_resolve_handle (bc->dst, u->holder);
  if (!holder || !holder->tio.object || holder->index < first)
    return 0;
  index = holder->index - first;
  if (u->kind == DWG_COPY_SLOT_REF)
    {
      if (holder->supertype == DWG_SUPERTYPE_ENTITY)
        {
          const Dwg_Object_Entity *ent = holder->tio.entity;
          if (in_array (u->slot, ent->reactors, ent->num_reactors))
            return 0;
        }
      else
        {
          Dwg_Object_Object *o = holder->tio.object;
          if (in_array (u->slot, o->reactors, o->num_reactors))
            return 0;
          if (holder->fixedtype == DWG_TYPE_BLOCK_HEADER && o->tio.BLOCK_HEADER
              && in_array (u->slot, o->tio.BLOCK_HEADER->inserts,
                           o->tio.BLOCK_HEADER->num_inserts))
            return 0;
          if (u->slot == (void *)&o->xdicobjhandle
              && hash_get (bc->role, bc->copy.handles[index]) == ROLE_RECORD)
            {
              o->is_xdic_missing = 1;
              return 0;
            }
        }
    }
  target = dwg_resolve_handle (bc->src, u->target);
  if (!target)
    {
      LOG_WARN ("dwg_dynblock_copy_block: %s " FORMAT_HV
                " refers to " FORMAT_HV ", which src doesn't have",
                holder->name, holder->handle.value, u->target);
      return 0;
    }
  LOG_WARN ("dwg_dynblock_copy_block: %s " FORMAT_HV " refers to %s " FORMAT_HV
            ", which was neither matched in dst nor copied: left NULL",
            holder->name, holder->handle.value, target->name, u->target);
  return DWG_ERR_INVALIDHANDLE;
}

/* Drops the NULL references, left by left_out (), from a list. */
static void
drop_null_refs (BITCODE_H **arrayp, BITCODE_BL *nump)
{
  BITCODE_H *array = *arrayp;
  BITCODE_BL i, j = 0;
  if (!array)
    return;
  for (i = 0; i < *nump; i++)
    if (array[i] && array[i]->absolute_ref)
      array[j++] = array[i];
  *nump = j;
  if (!j)
    {
      free (array);
      *arrayp = NULL;
    }
}

/* Lists a copied table record, or block, in dst's table. */
static int
add_to_table (Dwg_Data *restrict dst, const Dwg_Object *restrict obj)
{
  Dwg_Object *ctrlobj
      = dwg_get_first_object (dst, control_type (obj->fixedtype));
  Dwg_Object_APPID_CONTROL *ctrl = control_of (ctrlobj);
  BITCODE_H *entries;
  if (!ctrl)
    {
      LOG_ERROR ("dwg_dynblock_copy_block: dst has no table for %s",
                 obj->name);
      return DWG_ERR_INVALIDHANDLE;
    }
  entries = (BITCODE_H *)realloc (ctrl->entries, (ctrl->num_entries + 1)
                                                     * sizeof (BITCODE_H));
  if (!entries)
    return DWG_ERR_OUTOFMEM;
  ctrl->entries = entries;
  entries[ctrl->num_entries]
      = dwg_add_handleref (dst, 2, obj->handle.value, NULL);
  if (!entries[ctrl->num_entries])
    return DWG_ERR_OUTOFMEM;
  ctrl->num_entries++;
  // as the add API does after adding a record
  ctrlobj->size = 0;
  ctrlobj->bitsize = 0;
  return 0;
}

/* Lists a copied INSERT in the INSERTs of the block it draws, which may be
   a block of dst's. */
static int
add_to_inserts (Dwg_Data *restrict dst, const Dwg_Object *restrict obj)
{
  const BITCODE_H ref = obj->fixedtype == DWG_TYPE_INSERT
                            ? obj->tio.entity->tio.INSERT->block_header
                            : obj->tio.entity->tio.MINSERT->block_header;
  Dwg_Object *blkobj = dwg_resolve_handle (dst, ref_target (ref));
  Dwg_Object_BLOCK_HEADER *blk;
  BITCODE_H *inserts;
  BITCODE_BL i;
  if (!blkobj || blkobj->fixedtype != DWG_TYPE_BLOCK_HEADER
      || !blkobj->tio.object || !blkobj->tio.object->tio.BLOCK_HEADER)
    return 0;
  blk = blkobj->tio.object->tio.BLOCK_HEADER;
  for (i = 0; i < blk->num_inserts; i++)
    if (blk->inserts && ref_target (blk->inserts[i]) == obj->handle.value)
      return 0;
  inserts = (BITCODE_H *)realloc (blk->inserts,
                                  (blk->num_inserts + 1) * sizeof (BITCODE_H));
  if (!inserts)
    return DWG_ERR_OUTOFMEM;
  blk->inserts = inserts;
  inserts[blk->num_inserts]
      = dwg_add_handleref (dst, 4, obj->handle.value, NULL);
  if (!inserts[blk->num_inserts])
    return DWG_ERR_OUTOFMEM;
  blk->num_inserts++;
  // as add_to_table () does for a table
  blkobj->size = 0;
  blkobj->bitsize = 0;
  return 0;
}

/* Copies bc->copy into dst, through map, and links the copies into dst:
   the blocks and table records into their tables, the INSERTs into their
   blocks' lists. */
static int
copy_into_dst (Block_Copy *restrict bc, Dwg_Copy_Map *restrict map)
{
  Dwg_Data *dst = bc->dst;
  const BITCODE_BL first = dst->num_objects;
  Dwg_Copy_Unresolved *unresolved = NULL;
  BITCODE_BL num_unresolved = 0, i;
  int error, blocks = 0;

  for (i = 0; i < bc->matched_src.num; i++)
    dwg_copy_map_set (map, bc->matched_src.handles[i],
                      bc->matched_dst.handles[i]);
  error = dwg_copy_objects (dst, (Dwg_Data *)bc->src, bc->copy.handles,
                            bc->copy.num, map, &unresolved, &num_unresolved);
  if (error < DWG_ERR_CRITICAL)
    for (i = 0; i < num_unresolved; i++)
      error |= left_out (bc, &unresolved[i], first);
  free (unresolved);
  if (error >= DWG_ERR_CRITICAL)
    return error;

  // the copies are in dst in the order of bc->copy
  for (i = first; i < dst->num_objects; i++)
    {
      Dwg_Object *obj = &dst->object[i];
      const uint64_t role = hash_get (bc->role, bc->copy.handles[i - first]);
      if (obj->supertype == DWG_SUPERTYPE_ENTITY)
        drop_null_refs (&obj->tio.entity->reactors,
                        &obj->tio.entity->num_reactors);
      else
        drop_null_refs (&obj->tio.object->reactors,
                        &obj->tio.object->num_reactors);
      if (obj->fixedtype == DWG_TYPE_BLOCK_HEADER
          && obj->tio.object->tio.BLOCK_HEADER)
        {
          Dwg_Object_BLOCK_HEADER *blk = obj->tio.object->tio.BLOCK_HEADER;
          drop_null_refs (&blk->inserts, &blk->num_inserts);
          blocks++;
        }
      if (obj->fixedtype == DWG_TYPE_BLOCK_HEADER || role == ROLE_RECORD)
        error |= add_to_table (dst, obj);
    }
  for (i = first; i < dst->num_objects; i++)
    {
      const Dwg_Object *obj = &dst->object[i];
      if (obj->fixedtype == DWG_TYPE_INSERT
          || obj->fixedtype == DWG_TYPE_MINSERT)
        error |= add_to_inserts (dst, obj);
    }
  if (blocks)
    {
      // dwg->block_control is a copy of the BLOCK_CONTROL's fields
      const Dwg_Object *ctrl
          = dwg_get_first_object (dst, DWG_TYPE_BLOCK_CONTROL);
      if (ctrl)
        dst->block_control = *ctrl->tio.object->tio.BLOCK_CONTROL;
    }
  return error;
}

/*--------------------------------------------------------------------------------
 * The API
 */

/* The object of src that src_block is the BLOCK_HEADER of, or NULL. */
static const Dwg_Object *
block_object (const Dwg_Data *src, const Dwg_Object_BLOCK_HEADER *src_block)
{
  const Dwg_Object *obj;
  if (!src_block->parent || src_block->parent->objid >= src->num_objects)
    return NULL;
  obj = &src->object[src_block->parent->objid];
  if (obj->fixedtype != DWG_TYPE_BLOCK_HEADER
      || obj->tio.object != src_block->parent)
    return NULL;
  return obj;
}

static Dwg_Object_BLOCK_HEADER *
refused (int *errorp, const int error)
{
  if (errorp)
    *errorp = BLOCK_COPY_REFUSED | error;
  return NULL;
}

/* dwg_dynblock_copy_block (), with the map from src handles to dst handles
   given: when the block is copied, it has an entry for every object copied
   and every object matched. */
static Dwg_Object_BLOCK_HEADER *
copy_block (Dwg_Data *dst, const Dwg_Data *src,
            const Dwg_Object_BLOCK_HEADER *src_block,
            Dwg_Copy_Map *restrict map, int *errorp)
{
  const Dwg_Object *srcobj, *copy;
  Block_Copy bc;
  BITCODE_RLL found;
  int error;

  if (errorp)
    *errorp = 0;
  if (!dst || !src || !src_block || !map)
    return refused (errorp, DWG_ERR_INTERNALERROR);
  loglevel = dst->opts & DWG_OPTS_LOGLEVEL;
  if (src == dst)
    {
      LOG_ERROR ("dwg_dynblock_copy_block: src and dst are one document");
      return refused (errorp, DWG_ERR_INTERNALERROR);
    }
  if (src->header.from_version != R_2004 || dst->header.from_version != R_2004)
    {
      LOG_ERROR ("dwg_dynblock_copy_block: only r2004 (AC1018) documents "
                 "are supported, not %s into %s",
                 dwg_version_type (src->header.from_version),
                 dwg_version_type (dst->header.from_version));
      return refused (errorp, DWG_ERR_NOTYETSUPPORTED);
    }
  if (src->header.codepage != dst->header.codepage)
    {
      LOG_ERROR ("dwg_dynblock_copy_block: source codepage %u differs from "
                 "destination codepage %u",
                 (unsigned)src->header.codepage,
                 (unsigned)dst->header.codepage);
      return refused (errorp, DWG_ERR_NOTYETSUPPORTED);
    }
  srcobj = block_object (src, src_block);
  if (!srcobj)
    {
      LOG_ERROR ("dwg_dynblock_copy_block: src_block is not a block of src");
      return refused (errorp, DWG_ERR_INVALIDHANDLE);
    }
  if (!is_copyable_block (src, srcobj))
    {
      LOG_ERROR ("dwg_dynblock_copy_block: BLOCK_HEADER " FORMAT_HV
                 " is a layout block or an xref, not a block definition",
                 srcobj->handle.value);
      return refused (errorp, DWG_ERR_INVALIDTYPE);
    }

  // a block of the same name in dst is the definition to use
  found = find_block (dst, src, srcobj);
  if (found)
    {
      LOG_TRACE ("dwg_dynblock_copy_block: dst has BLOCK_HEADER " FORMAT_HV
                 " of that name\n",
                 found);
      return dwg_resolve_handle (dst, found)->tio.object->tio.BLOCK_HEADER;
    }

  memset (&bc, 0, sizeof (bc));
  bc.src = src;
  bc.dst = dst;
  bc.role = hash_new (1024);
  bc.decided = hash_new (1024);
  if (!bc.role || !bc.decided)
    error = DWG_ERR_OUTOFMEM;
  else
    error = find_copy (&bc, srcobj->handle.value);
  if (error < DWG_ERR_CRITICAL)
    error |= copy_into_dst (&bc, map);

  copy = error < DWG_ERR_CRITICAL
             ? dwg_resolve_handle (
                   dst, dwg_copy_map_get (map, srcobj->handle.value))
             : NULL;
  if (error >= DWG_ERR_CRITICAL)
    LOG_ERROR ("dwg_dynblock_copy_block: BLOCK_HEADER " FORMAT_HV
               " not copied, error 0x%x",
               srcobj->handle.value, (unsigned)error);
  free (bc.copy.handles);
  free (bc.pending.handles);
  free (bc.matched_src.handles);
  free (bc.matched_dst.handles);
  free (bc.deferred.handles);
  if (bc.role)
    hash_free (bc.role);
  if (bc.decided)
    hash_free (bc.decided);
  if (errorp)
    *errorp = error;
  return copy ? copy->tio.object->tio.BLOCK_HEADER : NULL;
}

EXPORT Dwg_Object_BLOCK_HEADER *
dwg_dynblock_copy_block (Dwg_Data *dst, const Dwg_Data *src,
                         const Dwg_Object_BLOCK_HEADER *src_block, int *errorp)
{
  Dwg_Copy_Map *map = dwg_copy_map_new ();
  Dwg_Object_BLOCK_HEADER *copy;
  if (!map)
    {
      if (errorp)
        *errorp = DWG_ERR_OUTOFMEM;
      return NULL;
    }
  copy = copy_block (dst, src, src_block, map, errorp);
  dwg_copy_map_free (map);
  return copy;
}
