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
   the map. Until dwg_copy_resolve () fills it in, the copy holds a NULL
   handle in its place, or 0 for a handle value in EED or XRECORD data. A
   handle with code 0 is a value, and keeps its source value instead. */
typedef struct _dwg_copy_unresolved
{
  BITCODE_RLL holder; /* the copy holding the reference, in dst */
  BITCODE_RLL target; /* the handle it referred to, in src */
  BITCODE_RC code;    /* the reference's handle code */
  Dwg_Copy_Slot kind;
  /* Inside the holder's data, maybe in one of its arrays (reactors, a
     block's entities, EED): valid until that is reallocated, so resolve
     before changing the copies. */
  void *slot;
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
   (SORTENTSTABLE's sort keys): it is mapped when map has an entry for it,
   a copy or not, and otherwise keeps its value and, between documents, is
   reported too.

   The same goes for a copy's owner, reactors and extension dictionary when
   they are not copied. Within one document a copy keeps its original's, so
   it shares the original's extension dictionary. Between documents an
   extension dictionary left out becomes a NULL handle while
   is_xdic_missing stays 0. The caller copies the dictionary as well, or
   drops it: a NULL handle and, from r2004, is_xdic_missing set.

   Raw bits that the encoder would write unchanged (unknown bits, raw EED)
   are cleared, so the copies are encoded from their fields. An EED group
   read from a file, which has raw bytes, is left out when its items don't
   hold all of those bytes: when the decoder failed on one of them, or left
   unread a string that claims more bytes than the group has left, and so
   the items after it, or skipped a byte left over at the group's end. So
   is a group whose first item has no data.
   Variable-type classes the copies need are added to dst. Adding the
   copies to their owners (a block's entity list, a dictionary) is the
   caller's, and so is the r13-r2000 entity chain (prev_entity,
   next_entity) of copied entities whose neighbours were not copied. The
   copies keep their sources' nolinks, and while it is set the encoder
   writes no prev_entity and next_entity, so a caller that links a copy
   into a chain clears it.

   The copy walks the specs, and so runs their unguarded statements again,
   some of which derive fields from others: from r2000, LAYER's frozen, off,
   frozen_in_new, locked, plotflag, linewt and flag are derived from flag0.
   The sources need their fields as decoded from a DWG file. A LAYER made
   with the add API or imported from DXF can have flags its flag0 doesn't
   hold, and its copy loses them: with flag0 0, frozen, off, frozen_in_new,
   locked, plotflag and linewt are all 0 in the copy.

   src and dst must be r13 to r2004 documents (R_13b1 to R_2004) of the same
   version and codepage. Objects whose class LibreDWG cannot decode
   (UNKNOWN_ENT, UNKNOWN_OBJ) are refused, as is a handle that is already in
   map. A refusal changes nothing.

   Returns 0, or DWG_ERR_* bits. From DWG_ERR_CRITICAL up: a refusal, also
   of a NULL dst, src or map, or of NULL handles when num_handles isn't 0
   (DWG_ERR_INTERNALERROR); or an allocation failure (DWG_ERR_OUTOFMEM),
   after which dst keeps the objects added so far, some of them incomplete,
   but none pointing into src's data, so that both documents can be freed.
   Below it, the copies are made, with some data left out:
   DWG_ERR_VALUEOUTOFBOUNDS for an array, or 3DSOLID ACIS data, that could
   not be copied and is NULL in the copy, and DWG_ERR_INVALIDEED for an EED
   group left out of a copy. DWG_ERR_VALUEOUTOFBOUNDS also comes from a
   source whose class_version is above 10 (IMAGE, IMAGEDEF,
   IMAGEDEF_REACTOR, GEODATA, RASTERVARIABLES): the decoder stops reading
   such an object there, so the copy of one it read leaves nothing out. */
int dwg_copy_objects (Dwg_Data *dst, Dwg_Data *src,
                      const BITCODE_RLL *restrict handles,
                      const BITCODE_BL num_handles, Dwg_Copy_Map *restrict map,
                      Dwg_Copy_Unresolved **restrict unresolvedp,
                      BITCODE_BL *restrict num_unresolvedp);

/* Points each unresolved reference whose target is now in map at the mapped
   handle, and moves the ones still unresolved to the front of the array.
   Returns 0, DWG_ERR_OUTOFMEM, or DWG_ERR_INTERNALERROR for a NULL dst, map
   or num_unresolvedp. */
int dwg_copy_resolve (Dwg_Data *restrict dst, const Dwg_Copy_Map *restrict map,
                      Dwg_Copy_Unresolved *restrict unresolved,
                      BITCODE_BL *restrict num_unresolvedp);

#endif
