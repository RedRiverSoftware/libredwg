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
 * dwg_dynblock_api.h: dynamic blocks. Describe a definition's properties,
 * read a placement's values, and evaluate a definition at chosen values
 * into the geometry of a representation block, with a check of that
 * evaluation against the representation blocks AutoCAD stored in a
 * drawing.
 *
 * A dynamic block definition is a block whose extension dictionary holds
 * an EVALUATION_GRAPH under ACAD_ENHANCEDBLOCK. Its parameters are the
 * graph's parameter nodes, and the properties AutoCAD's Properties palette
 * shows for a placement come from them. A placement whose values differ
 * from the definition's draws a representation block: an anonymous block
 * holding the definition's entities with the actions applied and the
 * visibility state's flags set, each carrying AcDbBlockRepETag EED with
 * the index of the definition entity it was made from. The values are
 * stored on the placement, an INSERT, in its extension dictionary:
 * AcDbBlockRepresentation, then AcDbRepData (the definition) and
 * AppDataCache, then ACAD_ENHANCEDBLOCKDATA, with one XRECORD per graph
 * node, named by the node's id.
 *
 * All names are UTF-8. Only r2004 drawings are supported.
 */

#ifndef DWG_DYNBLOCK_API_H
#define DWG_DYNBLOCK_API_H

#include "dwg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The kind of a dynamic block property: the parameter it comes from. */
typedef enum DWG_DYNPARAM_KIND
{
  DWG_DYNPARAM_LINEAR = 1, /* BLOCKLINEARPARAMETER: a distance */
  DWG_DYNPARAM_ROTATION,   /* BLOCKROTATIONPARAMETER: an angle, in radians */
  DWG_DYNPARAM_VISIBILITY, /* BLOCKVISIBILITYPARAMETER: a visibility state */
  DWG_DYNPARAM_FLIP,       /* BLOCKFLIPPARAMETER: 0 not flipped, 1 flipped */
  DWG_DYNPARAM_XY,         /* BLOCKXYPARAMETER: its X or its Y distance */
  DWG_DYNPARAM_POLAR,      /* BLOCKPOLARPARAMETER: its distance or angle */
  DWG_DYNPARAM_POINT,      /* BLOCKPOINTPARAMETER: a position */
  DWG_DYNPARAM_LOOKUP,     /* BLOCKLOOKUPPARAMETER: a lookup table row */
  DWG_DYNPARAM_CONSTRAINT, /* a constraint parameter of a parametric block */
  DWG_DYNPARAM_USER        /* BLOCKUSERPARAMETER, of a parametric block */
} Dwg_DynParam_Kind;

/* Bits of Dwg_DynParam.value_flags: which limits the value set has. */
#define DWG_DYNPARAM_HAS_MINIMUM 1
#define DWG_DYNPARAM_HAS_MAXIMUM 2
#define DWG_DYNPARAM_HAS_INCREMENT 4
#define DWG_DYNPARAM_HAS_LIST 8

/* A property of a dynamic block definition. The values and limits are
   those of the definition's own geometry and value set. */
typedef struct _dwg_dynparam
{
  char *name; /* as AutoCAD's Properties palette shows it */
  Dwg_DynParam_Kind kind;
  BITCODE_RLL handle; /* the parameter object */
  BITCODE_BL nodeid;  /* its evaluation graph node id */
  /* LINEAR: the distance between the parameter's points. ROTATION: the
     angle of its end point about its base point, from its base angle,
     in [0, 2 pi). FLIP: 0. Others: 0, as no default is read. */
  double default_value;
  /* VISIBILITY: the first state, which a placement at the defaults
     shows. NULL for the other kinds. */
  char *default_state;
  /* VISIBILITY: the state names. FLIP: the labels of its two states. */
  BITCODE_BL num_states;
  char **states;
  /* LINEAR and ROTATION: the value set, as DWG_DYNPARAM_HAS_* bits, its
     limits, and its list. AutoCAD offers default_value as well as the
     list. */
  BITCODE_BL value_flags;
  double minimum;
  double maximum;
  double increment;
  BITCODE_BL num_values;
  double *values;
} Dwg_DynParam;

/* The value of a property, by its name, in the definition's units: for a
   scaled placement AutoCAD's Properties palette shows a LINEAR value
   multiplied by the placement's scale. */
typedef struct _dwg_dynvalue
{
  char *name;
  /* LINEAR: the distance. ROTATION: the angle, in radians. FLIP: 0 or 1. */
  double value;
  /* VISIBILITY: the name of the state; NULL for the other kinds. */
  char *state;
} Dwg_DynValue;

/* Describes the properties of dynamic block definition def, in the order
   of its evaluation graph's nodes, into a new array *params of *count
   entries that dwg_dynblock_free_params () frees. An XY or a polar
   parameter gives two properties, the others one. Alignment and base point
   parameters, which have no property, are left out.

   Returns 0, or from DWG_ERR_CRITICAL up: DWG_ERR_INVALIDDWG with
   DWG_ERR_INVALIDTYPE when def is not a dynamic block definition, or with
   DWG_ERR_NOTYETSUPPORTED for a drawing other than r2004;
   DWG_ERR_OUTOFMEM; or DWG_ERR_INTERNALERROR for NULL arguments. */
EXPORT int dwg_dynblock_get_params (const Dwg_Object_BLOCK_HEADER *def,
                                    Dwg_DynParam **params, BITCODE_BL *count);

EXPORT void dwg_dynblock_free_params (Dwg_DynParam *params,
                                      const BITCODE_BL count);

/* Reads the values stored on placement insert: the definition it places
   into *defp, and the values of the definition's properties into a new
   array *values of *count entries, in the order of
   dwg_dynblock_get_params (), that dwg_dynblock_free_values () frees. A
   placement drawing the definition itself, with no
   AcDbBlockRepresentation, is at the defaults, and gets them.

   Values are read for LINEAR, ROTATION, VISIBILITY and FLIP properties.
   Properties of the other kinds are left out of *values, and the return
   value is then DWG_ERR_NOTYETSUPPORTED, below DWG_ERR_CRITICAL.

   Returns 0, DWG_ERR_NOTYETSUPPORTED as above, or from DWG_ERR_CRITICAL
   up: DWG_ERR_INVALIDDWG alone when a stored value is missing or not in
   the layout AutoCAD writes, with DWG_ERR_INVALIDTYPE when insert does not
   place a dynamic block, or with DWG_ERR_NOTYETSUPPORTED for a drawing
   other than r2004; DWG_ERR_OUTOFMEM; or DWG_ERR_INTERNALERROR for NULL
   arguments. */
EXPORT int dwg_dynblock_get_values (const Dwg_Entity_INSERT *insert,
                                    Dwg_Object_BLOCK_HEADER **defp,
                                    Dwg_DynValue **values, BITCODE_BL *count);

EXPORT void dwg_dynblock_free_values (Dwg_DynValue *values,
                                      const BITCODE_BL count);

/* Evaluates dynamic block definition def at the given values, into a new
   anonymous block ("*U") in def's document, which it returns: the
   geometry of a representation block for those values.

   The new block holds a copy of each of def's entities, in def's order,
   with the owned ATTRIBs, VERTEXes and SEQENDs of INSERTs and POLYLINEs
   among them. Each copy keeps its source's EED, so its
   AcDbBlockRepETag index names the definition entity it was made from,
   while its 1005 handle names the copy itself. The copies leave out their
   sources' extension dictionaries, and the reactors that point outside the
   copies. A visibility state given in values sets the invisible flag of
   each entity the visibility parameter controls; without one the copies
   keep the definition's flags, which show the first state. The block gets
   def's base point, and no EED or extension dictionary: AcDbBlockRepBTag,
   the placement and its values are the caller's.

   Properties left out of values keep the definition's values, and so do
   those given at them. A value that changes the geometry is evaluated
   only for the kinds supported: VISIBILITY. Changing any other kind is
   refused, as is a definition that is constraint-based (parametric).

   Returns the new block, with *errorp 0, or below DWG_ERR_CRITICAL the
   bits of the copy for data it left out (an array, or an EED group, it
   could not copy). Otherwise NULL with *errorp from DWG_ERR_CRITICAL up. A
   refusal is DWG_ERR_INVALIDDWG with a bit for the reason:
   DWG_ERR_NOTYETSUPPORTED for a value or definition the evaluator doesn't
   support, or a drawing other than r2004; DWG_ERR_UNHANDLEDCLASS for an
   entity or graph node of a class LibreDWG can't decode;
   DWG_ERR_INVALIDHANDLE for an entity of def, or one it owns, that
   doesn't resolve or isn't an entity;
   DWG_ERR_VALUEOUTOFBOUNDS for a name no property has, a name given twice,
   a value that isn't finite, a state the parameter doesn't have, or a
   flip value other than 0 or 1;
   DWG_ERR_INVALIDTYPE when def is not a dynamic block definition. Or the
   copy's refusal, such as of an entity handle def lists twice. A refusal
   adds nothing to the document, and the reason is logged at LOG_ERROR.
   Otherwise DWG_ERR_OUTOFMEM, which leaves what was made in the document,
   or DWG_ERR_INTERNALERROR for NULL arguments. */
EXPORT Dwg_Object_BLOCK_HEADER *
dwg_dynblock_evaluate (Dwg_Object_BLOCK_HEADER *def,
                       const Dwg_DynValue *values, const BITCODE_BL num_values,
                       int *errorp);

/* The outcome of checking one placement's representation block. */
typedef enum DWG_DYNBLOCK_CHECK_STATUS
{
  /* every entity of the evaluation matches AutoCAD's */
  DWG_DYNBLOCK_CHECK_MATCH = 0,
  /* the evaluation differs from AutoCAD's block */
  DWG_DYNBLOCK_CHECK_MISMATCH,
  /* the evaluator refuses the placement's values or definition, or a stored
     value isn't read */
  DWG_DYNBLOCK_CHECK_UNSUPPORTED,
  /* no values to evaluate: no INSERT draws the block, or its INSERT has no
     AcDbBlockRepresentation */
  DWG_DYNBLOCK_CHECK_NO_VALUES,
  /* reading the values or evaluating them failed */
  DWG_DYNBLOCK_CHECK_FAILED
} Dwg_DynBlock_Check_Status;

typedef struct _dwg_dynblock_check
{
  BITCODE_RLL insert;     /* the placement, 0 for a block none draws */
  BITCODE_RLL block;      /* the representation block AutoCAD stored */
  BITCODE_RLL definition; /* the definition, 0 when not known */
  BITCODE_RLL evaluated;  /* the block the evaluation made, 0 for none */
  Dwg_DynBlock_Check_Status status;
  /* a bit (1 << kind) for each kind of property whose stored value
     differs from the definition's, among the values read: a check that is
     unsupported because a stored value isn't read doesn't know whether
     that property changed */
  BITCODE_BL kinds;
  BITCODE_BL num_entities;  /* entities compared */
  BITCODE_BL num_differing; /* of which differ */
  /* of which match only as the same curve stored another way: an ELLIPSE
     with its major axis reversed and its parameters turned by pi, as
     AutoCAD keeps one after some grip edits */
  BITCODE_BL num_equivalent;
  /* UTF-8: what differs first, or why the check is unsupported or failed;
     NULL for a match */
  char *reason;
} Dwg_DynBlock_Check;

typedef struct _dwg_dynblock_check_report
{
  BITCODE_BL num_checks;
  Dwg_DynBlock_Check *checks;
  BITCODE_BL num_match;
  BITCODE_BL num_mismatch;
  BITCODE_BL num_unsupported;
  BITCODE_BL num_no_values;
  BITCODE_BL num_failed;
} Dwg_DynBlock_Check_Report;

/* Checks the evaluator against the representation blocks AutoCAD stored in
   dwg. For every INSERT that draws a representation block (an anonymous
   block with AcDbBlockRepBTag EED), it reads the INSERT's values,
   evaluates its definition at them, and compares the evaluated block with
   AutoCAD's entity by entity, paired by their AcDbBlockRepETag index. A
   representation block no INSERT draws gets a check too, with status
   DWG_DYNBLOCK_CHECK_NO_VALUES.

   Entities compare by type, by their invisible flag, layer, colour,
   linetype and lineweight, and field by field with the dynapi field
   lists: numbers within tolerance, strings exactly, and references by
   the entity they point at, paired by index within the blocks, so the
   same handle matches, whether or not it resolves. An entry of either
   block's entity list that doesn't resolve is a difference. An array
   the comparison has no length for, unless empty in both, counts as a
   difference. An ELLIPSE also matches the same curve with its major axis
   reversed, and is counted in num_equivalent. The blocks' base points
   compare too: a difference there is a mismatch, though not of an
   entity.

   Each evaluation adds its block to dwg, so check a document that won't be
   written. Returns the report, which dwg_dynblock_free_check_report ()
   frees, or NULL when dwg is NULL or memory runs out. */
EXPORT Dwg_DynBlock_Check_Report *dwg_dynblock_check (Dwg_Data *dwg,
                                                      const double tolerance);

EXPORT void
dwg_dynblock_free_check_report (Dwg_DynBlock_Check_Report *report);

/* The name of a kind, such as "linear", or "unknown". */
EXPORT const char *dwg_dynparam_kind_name (const Dwg_DynParam_Kind kind);

#ifdef __cplusplus
}
#endif

#endif /* DWG_DYNBLOCK_API_H */
