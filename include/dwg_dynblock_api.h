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
 * dwg_dynblock_api.h: dynamic blocks: copy a block definition, with its
 * dynamic-block data, from one document into another.
 */

#ifndef DWG_DYNBLOCK_API_H
#define DWG_DYNBLOCK_API_H

#include "dwg.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Copy a block definition, with any dynamic-block data, from src into dst.
 *
 * The copy holds the BLOCK_HEADER, its BLOCK, ENDBLK and entities, and the
 * objects they own: extension dictionaries, with a dynamic block's
 * EVALUATION_GRAPH and its parameters, actions, grips and grip location
 * components, the purge preventer and ACAD_SORTENTS; attributes and
 * vertices. EED comes with them, such as a dynamic block's
 * AcDbDynamicBlockTrueName and AcDbDynamicBlockGUID, and each entity's
 * AcDbBlockRepETag. An object that a dictionary still names as its owner but
 * no longer lists is left out.
 *
 * References out of the definition are matched by name in dst: layers,
 * linetypes, text styles, dimension styles, APPIDs and the other symbol
 * tables, and the styles kept in the named object dictionary's
 * dictionaries: plot, multiline, multileader and table styles, materials,
 * visual styles and colour book colours. A table record dst lacks is
 * copied from src, without its extension dictionary, and added to its
 * table; a plot style dst lacks becomes dst's default one, and another
 * style dst lacks is a NULL reference. Blocks the definition inserts are
 * copied with it, unless dst has a block of the same name. Classes are
 * added to dst as needed. The definition's list of INSERTs is not copied:
 * the copy has no placements yet. A reactor (a link back to an object that
 * watches this one) is matched or copied as any other reference is, so one
 * to a block, table record or style that dst has, or that is copied, is
 * kept, pointing at dst's object; other reactors to objects outside the
 * definition, such as groups, are dropped, with no error. A reference to an
 * object src doesn't have is NULL in the copy, with a warning logged but no
 * error, as src is broken there already.
 *
 * Names: a dynamic block definition is stored as an anonymous block named
 * "*U", and its name is in its AcDbDynamicBlockTrueName (or
 * AcDbDynamicBlockTrueName2) EED. Blocks are matched by that name, or for
 * other blocks by their own name; never by "*U", and an anonymous block
 * without a true name matches nothing. When dst already has a block of
 * src_block's name, nothing is copied and dst's definition is returned,
 * whatever it holds, as AutoCAD's INSERT uses the definition a drawing
 * already has. Copying the same definition into one document twice
 * therefore returns the first copy, and copies an anonymous block without a
 * true name again.
 *
 * src must be read from a DWG file. Plain blocks copy too.
 *
 * Refused, with nothing changed in dst: a NULL argument; src == dst;
 * src_block not a BLOCK_HEADER of src; documents that aren't both r2004
 * (AC1018), or have different codepages; model space, paper space and other
 * layout blocks, xrefs and blocks that depend on one; a constraint-based
 * (parametric) definition, with DWG_ERR_NOTYETSUPPORTED, as LibreDWG can't
 * write its associative network so that AutoCAD reads it; a definition
 * that holds another object LibreDWG can't decode.
 *
 * @param dst       The document to copy into.
 * @param src       The document src_block is in.
 * @param src_block The block definition to copy.
 * @param errorp    If not NULL, receives 0 or DWG_ERR_* bits. From
 *                  DWG_ERR_CRITICAL up, the copy was refused, as
 *                  DWG_ERR_INVALIDDWG with a bit for the reason, or ran out
 *                  of memory (DWG_ERR_OUTOFMEM), after which dst may hold
 *                  copies that no table lists. Below it, the copy was made
 *                  with something left out: DWG_ERR_INVALIDHANDLE for a
 *                  reference to an object of src that could be neither
 *                  matched nor copied and is NULL in the copy,
 *                  DWG_ERR_VALUEOUTOFBOUNDS for data that could not be
 *                  copied or a copy that a full table of dst (65535
 *                  records) could not list, and DWG_ERR_INVALIDEED for an
 *                  EED group whose items could not be decoded.
 * @return dst's definition, or NULL when the copy was refused or failed.
 */
EXPORT Dwg_Object_BLOCK_HEADER *
dwg_dynblock_copy_block (Dwg_Data *dst, const Dwg_Data *src,
                         const Dwg_Object_BLOCK_HEADER *src_block,
                         int *errorp);

#ifdef __cplusplus
}
#endif

#endif /* DWG_DYNBLOCK_API_H */
