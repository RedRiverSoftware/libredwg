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
 * copy.h: deep copy of objects from one document into another, or within
 * one document, with every handle remapped to the copies.
 */

#ifndef COPY_H
#define COPY_H

#include "dwg.h"

/* An old-to-new handle map, from handles in the source document to handles
   in the destination. A copy adds an entry for every object it copies. An
   entry added before a copy makes references to that source handle point at
   the given destination object instead, for example a layer matched by
   name. */
typedef struct _dwg_copy_map Dwg_Copy_Map;

Dwg_Copy_Map *dwg_copy_map_new (void);
void dwg_copy_map_free (Dwg_Copy_Map *map);
/* Returns 0, or DWG_ERR_INVALIDHANDLE for a 0 handle. */
int dwg_copy_map_set (Dwg_Copy_Map *restrict map, const BITCODE_RLL src_handle,
                      const BITCODE_RLL dst_handle);
/* Returns the destination handle mapped to src_handle, or 0. */
BITCODE_RLL dwg_copy_map_get (const Dwg_Copy_Map *restrict map,
                              const BITCODE_RLL src_handle);

/* Where an unresolved reference is held in the copy. */
typedef enum DWG_COPY_SLOT
{
  DWG_COPY_SLOT_REF = 1,    /* a handle reference field */
  DWG_COPY_SLOT_EED_APPID,  /* the APPID handle of an EED entry */
  DWG_COPY_SLOT_HANDLE,     /* a handle value in EED data or XRECORD data */
} Dwg_Copy_Slot;

/* A reference in a copy to a source object that was neither copied nor in
   the map. The copy holds a NULL handle in its place until
   dwg_copy_resolve () fills it in. */
typedef struct _dwg_copy_unresolved
{
  BITCODE_RLL holder; /* the copy holding the reference, in dst */
  BITCODE_RLL target; /* the handle it referred to, in src */
  BITCODE_RC code;    /* the reference's handle code */
  Dwg_Copy_Slot kind;
  void *slot; /* inside the holder's data */
} Dwg_Copy_Unresolved;

/* Copies the objects with the given handles from src into dst, appending
   them to dst's objects in the order given, each with a new handle.

   Every handle reference in the copies is remapped through map, which
   gains an entry for every copy: references between the copied objects
   point at the copies, and relative handle codes are recomputed against the
   new handles. A reference to an object that is neither copied nor in the
   map keeps its target when src == dst. Otherwise it becomes a NULL handle
   and is reported in *unresolvedp, an array of *num_unresolvedp entries
   that the caller frees. A handle with code 0 is a value, not a reference
   (SORTENTSTABLE's sort keys): it is mapped when it names a copied object,
   and otherwise keeps its value and, between documents, is reported too.

   Raw bits that the encoder would write unchanged (unknown bits, raw EED)
   are cleared, so the copies are encoded from their fields. Variable-type
   classes the copies need are added to dst. Adding the copies to their
   owners (a block's entity list, a dictionary) is the caller's, and so is
   the r13-r2000 entity chain (prev_entity, next_entity) of copied entities
   whose neighbours were not copied.

   src and dst must be r13 to r2004 documents of the same version and
   codepage. Objects whose class LibreDWG cannot decode (UNKNOWN_ENT,
   UNKNOWN_OBJ) are refused, as is a handle that is already in map. A
   refusal changes nothing.

   Returns 0, or DWG_ERR_* bits: DWG_ERR_CRITICAL or above for a refusal, or
   for an allocation failure, after which dst keeps the copies made so
   far. */
int dwg_copy_objects (Dwg_Data *dst, Dwg_Data *src,
                      const BITCODE_RLL *restrict handles,
                      const BITCODE_BL num_handles, Dwg_Copy_Map *restrict map,
                      Dwg_Copy_Unresolved **restrict unresolvedp,
                      BITCODE_BL *restrict num_unresolvedp);

/* Points each unresolved reference whose target is now in map at the mapped
   handle, and moves the ones still unresolved to the front of the array.
   Returns 0, or DWG_ERR_OUTOFMEM. */
int dwg_copy_resolve (Dwg_Data *restrict dst, const Dwg_Copy_Map *restrict map,
                      Dwg_Copy_Unresolved *restrict unresolved,
                      BITCODE_BL *restrict num_unresolvedp);

#endif
