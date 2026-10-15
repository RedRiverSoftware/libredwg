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
 * stores. Not exported.
 */

#ifndef DYNBLOCK_H
#define DYNBLOCK_H

#include "dwg.h"
#include "dwg_dynblock_api.h"

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

/* The change of the position of one of the definition's ATTDEFs, which
   the evaluation moves whole. */
typedef struct _dynblock_entity_delta
{
  BITCODE_RLL entity;
  BITCODE_3BD delta;
} Dynblock_Entity_Delta;

typedef struct _dynblock_state
{
  Dynblock_Node_State *nodes; /* each parameter with points, in graph order */
  BITCODE_BL num_nodes;
  Dynblock_Entity_Delta *deltas; /* the ATTDEFs that move */
  BITCODE_BL num_deltas;
} Dynblock_State;

/* Plans the evaluation of definition def_handle at values as
   dwg_dynblock_evaluate () does, with its refusals, and without changing
   the document, into *state, which dynblock_free_state () frees. Also
   refused, with DWG_ERR_NOTYETSUPPORTED: an evaluation that moves a
   parameter in a way whose stored state AutoCAD's placements have not
   shown, such as a rotation parameter that another action moves, or a
   parameter an action selects without listing it among those it moves.
   Returns 0 or DWG_ERR_* bits, with the reason in reason. */
int dynblock_evaluated_state (Dwg_Data *dwg, const BITCODE_RLL def_handle,
                              const Dwg_DynValue *values,
                              const BITCODE_BL num_values,
                              Dynblock_State *state, char *reason,
                              const size_t reason_size);

void dynblock_free_state (Dynblock_State *state);

#endif /* DYNBLOCK_H */
