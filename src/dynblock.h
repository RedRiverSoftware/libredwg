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
 * dynblock.h: what the evaluator in dynblock_eval.c tells
 * dynblock_insert.c about an evaluation, for the values a placement
 * stores; and what the authoring functions of dynblock_author.c take from
 * the evaluator and from the block copy of dynblock_copy.c. Not exported.
 */

#ifndef DYNBLOCK_H
#define DYNBLOCK_H

#include "dwg.h"
#include "dwg_dynblock_api.h"
#include "copy.h"

/* A parameter of the evaluation graph after an evaluation: its points as
   the actions that ran left them, in the order of the record AutoCAD
   stores for it on a placement. A linear, alignment, flip or rotation
   parameter has its base and end points, a visibility or base point
   parameter its one point. */
typedef struct _dynblock_node_state
{
  BITCODE_RLL handle; /* the parameter */
  BITCODE_BL num_points;
  BITCODE_3BD points[2];
} Dynblock_Node_State;

/* One of the definition's ATTDEFs that the evaluation moves whole: its
   insertion and alignment points after the moves, added to them one after
   another as the evaluation adds them, so that they are the copy's to the
   last bit. */
typedef struct _dynblock_attdef_place
{
  BITCODE_RLL entity;
  BITCODE_2DPOINT ins_pt;
  BITCODE_2DPOINT alignment_pt;
} Dynblock_Attdef_Place;

typedef struct _dynblock_state
{
  Dynblock_Node_State *nodes; /* each parameter with points, in graph order */
  BITCODE_BL num_nodes;
  Dynblock_Attdef_Place *attdefs; /* the ATTDEFs that move */
  BITCODE_BL num_attdefs;
} Dynblock_State;

/* Plans the evaluation of definition def_handle at values as
   dwg_dynblock_evaluate () does, with its refusals, and without changing
   the document, into *state, which dynblock_free_state () frees. Also
   refused, with DWG_ERR_NOTYETSUPPORTED: an evaluation that moves a
   parameter in a way whose stored state AutoCAD's placements have not
   shown, such as a rotation parameter that another action moves, or a
   parameter an action selects without listing it among those it moves;
   and with DWG_ERR_VALUEOUTOFBOUNDS, a point the actions would move
   beyond what a double holds, or a LINEAR or ROTATION value that its
   parameter's points don't hold after the evaluation, as at coordinates
   so large that they round. Returns 0 or DWG_ERR_* bits, with the reason
   in reason. */
int dynblock_evaluated_state (Dwg_Data *dwg, const BITCODE_RLL def_handle,
                              const Dwg_DynValue *values,
                              const BITCODE_BL num_values,
                              Dynblock_State *state, char *reason,
                              const size_t reason_size);

void dynblock_free_state (Dynblock_State *state);

/* What dynblock_author.c takes from the evaluator, so that the blocks it
   makes move and stretch as the evaluator evaluates them. */

/* Whether the evaluator moves an entity of obj's type whole. */
int dynblock_is_movable (const Dwg_Object *obj);

/* Whether a move of entity obj, or of an ATTRIB it owns, by wcs_delta
   would take its 2D points out of their plane. */
int dynblock_leaves_plane (const Dwg_Data *dwg, const Dwg_Object *obj,
                           const BITCODE_3BD *wcs_delta);

/* The largest magnitude of a coordinate dynblock_move_entity () moves of
   entity obj, with the ATTRIBs an INSERT owns. */
double dynblock_entity_reach (const Dwg_Data *dwg, const Dwg_Object *obj);

/* Moves entity obj whole by wcs_delta, with the ATTRIBs an INSERT owns, as
   the evaluator moves a copy. */
void dynblock_move_entity (Dwg_Data *dwg, Dwg_Object *obj,
                           const BITCODE_3BD *wcs_delta);

/* The number of stretch points of entity obj that a stretch action's
   indexes count, as the evaluator counts them; 0 for a type it doesn't
   stretch. */
BITCODE_BL dynblock_stretch_points (const Dwg_Object *obj);

/* Where stretch point i of entity obj is, in its block's coordinates, into
   *pt: a LINE's start and end, an LWPOLYLINE's vertices, a CIRCLE's
   centre, an INSERT's insertion point, and the insertion point of a TEXT
   or ATTDEF justified left on its baseline, for an entity whose extrusion
   is along +Z. Returns 0 for a point of another entity, whose position
   isn't known here. */
int dynblock_stretch_point (const Dwg_Object *obj, const BITCODE_BL i,
                            BITCODE_3BD *pt);

/* The EVALUATION_GRAPH of block obj, or NULL when obj is not a dynamic
   block definition. */
Dwg_Object *dynblock_graph (const Dwg_Data *dwg, const Dwg_Object *obj);

/* In dynblock_copy.c, for dwg_dynblock_copy_entities () */

int dynblock_is_model_space (const Dwg_Data *dwg, const Dwg_Object *obj);

/* Whether block obj is model space, or a block that isn't a layout, an
   xref or a block depending on one. */
int dynblock_is_copyable_block (const Dwg_Data *dwg, const Dwg_Object *obj);

/* Copies the entities tops of src's block src_block, with what they own,
   into dst through map, which then has an entry for each copy, as
   dwg_dynblock_copy_block () copies a definition's: references out of
   them are matched by name in dst, or copied, and those to src_block are
   to dst's block dst_block. The copies are in no block's entity list.
   Returns 0 or DWG_ERR_* bits as dwg_dynblock_copy_block () does; a
   refusal changes nothing in dst. */
int dynblock_copy_entities (Dwg_Data *dst, const Dwg_Data *src,
                            const BITCODE_RLL src_block,
                            const BITCODE_RLL dst_block,
                            const BITCODE_RLL *tops,
                            const BITCODE_BL num_tops, Dwg_Copy_Map *map);

/* In dynblock_author.c */

/* Drops the AcDbBlockRepETag EED groups of entity obj, which number the
   entities of a dynamic block definition. */
void dynblock_drop_etag (const Dwg_Data *dwg, Dwg_Object *obj);

#endif /* DYNBLOCK_H */
