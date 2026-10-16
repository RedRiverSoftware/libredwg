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
   the items after it. So is a group whose first item has no data.
   Variable-type classes the copies need are added to dst. Adding the
   copies to their owners (a block's entity list, a dictionary) is the
   caller's, and so is the r13-r2000 entity chain (prev_entity,
   next_entity) of copied entities whose neighbours were not copied. The
   copies keep their sources' nolinks, and while it is set the encoder
   writes no prev_entity and next_entity, so a caller that links a copy
   into a chain clears it.

   src and dst may be documents of different kinds: read from a DWG file,
   imported from DXF or JSON, or built with the add API, whose
   dwg_add_Document () sets both import flags (DWG_OPTS_IN). The copies
   keep their sources' values, converted where the encoder takes a kind's
   values differently:
   - 8-bit strings are UTF-8 in a document imported from JSON or built with
     the add API, and in its codepage in the others. They are converted
     between the two, with \U+XXXX for a character the codepage lacks, as
     the encoder converts them. An XRECORD's strings are converted with
     their own codepage, NULs inside them kept, and its xdata_size computed
     again, as it is too for a copy from an import or a document built
     with the add API into a document read from a DWG file. An XRECORD
     string too long once converted is left out, NULL with size 0, with
     DWG_ERR_VALUEOUTOFBOUNDS: above 0x7FFF bytes in the codepage, the most
     the decoder reads, or above 0xFFFF in UTF-8, the most its size holds
     (the encoder converts it, and truncates the result to 0x7FFF). EED
     strings, which the encoder writes as they are in every kind, are
     copied as they are. A string that is not UTF-8 in a document whose
     strings are, or that has a character beyond U+FFFF, which \U+XXXX
     can't hold, is copied as it is, with DWG_ERR_VALUEOUTOFBOUNDS.
   - From r2000 a LAYER holds its flags three ways: flag0, the fields
     frozen, off, frozen_in_new, locked, plotflag and linewt, and the DXF
     flag with the sign of the colour index. The encoder writes flag0 as it
     is for a JSON import, from the DXF flag and the colour for a DXF import,
     plotted, and from the fields for the others. A copy into a document of
     another kind holds all three as its source stands for them: flag0 from
     a JSON import, the DXF flag and the colour from a DXF import, and the
     fields from the others.
   - The encoder works out the dataflags of a TEXT, ATTRIB or ATTDEF from
     its values for a DXF import, a document built with the add API, or
     one of r2000 or older, and writes those of the others as they are,
     leaving out the values they flag as defaults. A copy into a document
     whose encoder takes them otherwise than its source's gets them from
     its values, or, the other way, the values its source's flags leave
     out at their defaults.
   The copy walks the specs, which set some fields from others as the
   decoder needs them (a table record's DXF flag from its bits, LAYER's
   fields from flag0). Those fields then take their sources' values again.
   So does a count, when it is no larger than the walk's, which may have
   copied that many elements (TABLESTYLE's three row styles, IMAGE's two
   clip vertices of a rectangle).

   src and dst must be r13 to r2004 documents (R_13b1 to R_2004), and,
   between documents, of the same version and codepage. A copy is refused
   with DWG_ERR_INVALIDDWG and a bit for the reason, and the refusal
   changes nothing:
   - DWG_ERR_INTERNALERROR: dst, src or map is NULL, or handles is NULL
     while num_handles isn't 0;
   - DWG_ERR_NOTYETSUPPORTED: the documents' versions or codepages;
   - DWG_ERR_INVALIDHANDLE: a handle that is 0, or not in src, or listed
     twice, or already in map;
   - DWG_ERR_UNHANDLEDCLASS: an object without data, or of a class LibreDWG
     cannot decode (UNKNOWN_ENT, UNKNOWN_OBJ) or copy;
   - DWG_ERR_INVALIDTYPE: an object of a variable type whose class src
     lacks.

   Returns 0, or DWG_ERR_* bits. From DWG_ERR_CRITICAL up: a refusal, or an
   allocation failure (DWG_ERR_OUTOFMEM), after which dst keeps the objects
   added so far, some of them incomplete, but none pointing into src's
   data, so that both documents can be freed. Below it, the copies are
   made, with some data left out: DWG_ERR_VALUEOUTOFBOUNDS for an array or
   binary chunk, or 3DSOLID ACIS data, that could not be copied and is NULL
   in the copy, among them those the specs don't walk, which a JSON import
   can set (EXTRUDEDSURFACE's bindata), and DWG_ERR_INVALIDEED for an EED
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
