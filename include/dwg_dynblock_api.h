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
 * dwg_dynblock_api.h: dynamic blocks. Copy a block definition, with its
 * dynamic-block data, from one document into another; describe a
 * definition's properties, read a placement's values, and evaluate a
 * definition at chosen values into the geometry of a representation
 * block, with a check of that evaluation against the representation
 * blocks AutoCAD stored in a drawing.
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
   DWG_ERR_INVALIDTYPE when def is not a dynamic block definition, with
   DWG_ERR_INVALIDHANDLE when a node of its evaluation graph doesn't
   resolve, or with DWG_ERR_NOTYETSUPPORTED for a drawing other than r2004;
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
   place a dynamic block, with DWG_ERR_INVALIDHANDLE when a node of the
   definition's evaluation graph doesn't resolve, or with
   DWG_ERR_NOTYETSUPPORTED for a drawing other than r2004;
   DWG_ERR_OUTOFMEM; or DWG_ERR_INTERNALERROR for NULL arguments. */
EXPORT int dwg_dynblock_get_values (const Dwg_Entity_INSERT *insert,
                                    Dwg_Object_BLOCK_HEADER **defp,
                                    Dwg_DynValue **values, BITCODE_BL *count);

EXPORT void dwg_dynblock_free_values (Dwg_DynValue *values,
                                      const BITCODE_BL count);

/* Evaluates dynamic block definition def at the given values, into a new
   anonymous block ("*U") in def's document, which it returns: the
   geometry of a representation block for those values.

   The new block holds a copy of each of def's entities, in def's order.
   The ATTRIBs, VERTEXes and SEQENDs that an INSERT, MINSERT, POLYLINE or
   TABLE owns are copied with it, owned by its copy, and are not in the
   block's entity list. Each copy keeps its source's EED, so its
   AcDbBlockRepETag index names the definition entity it was made from,
   while its 1005 handle names the copy itself. The copies leave out their
   sources' extension dictionaries, and the reactors that point outside the
   copies. The block gets def's base point and its flag for attribute
   definitions, and no EED or extension dictionary: AcDbBlockRepBTag, the
   placement and its values are the caller's.

   A property left out of values is not set: it keeps the definition's
   value, unless the actions of another property move its parameter, as
   in AutoCAD. A visibility state sets the invisible flag of each entity
   the visibility parameter controls; without one the copies keep the
   definition's flags, which show the first state.

   A LINEAR value moves its parameter's end point along the parameter, to
   that distance from its base point. The stretch and move actions taking
   the end point's change then move entities by it, times each action's
   distance multiplier and turned by its angle offset: a move action the
   entities it selects, a stretch action the stretch points its frame
   holds, as AutoCAD lists them with the action. Those are the ends of a
   LINE and the vertices of an LWPOLYLINE. An ARC, CIRCLE, ELLIPSE, SPLINE,
   TEXT, ATTDEF, MTEXT, INSERT (with its ATTRIBs) or HATCH moves whole,
   when its stretch points are all in the frame; a HATCH must then lie
   wholly inside the frame, which must be a rectangle. The actions also
   move the points of the parameters they select, so a LINEAR value is
   the distance after other properties' actions moved its parameter, as
   AutoCAD's Properties palette shows it. A LINEAR value is not checked
   against the parameter's value set. Refused, with
   DWG_ERR_NOTYETSUPPORTED: a LINEAR value that changes its parameter when
   the parameter drives an action of another kind (such as scale), or has
   its base point in its middle; an action moving a parameter, of any
   kind, that chains its actions, which AutoCAD would then run too; a
   stretch of part of an entity of another type; a HATCH that crosses the
   frame; an action moving an entity of another type, taking another
   point's change than the end or base point's, or with an XY type; a move
   that would take 2D points out of their plane.

   A ROTATION value is accepted at the definition's value and a FLIP value
   at 0, and refused otherwise; a value of any other kind (XY, POLAR,
   POINT, LOOKUP, CONSTRAINT or USER) is refused whatever it is. A
   definition that is constraint-based (parametric) is refused.

   Returns the new block, with *errorp 0, or below DWG_ERR_CRITICAL the
   bits of the copy for data it left out (such as an array, 3DSOLID ACIS
   data or an EED group it could not copy). Otherwise NULL with *errorp
   from DWG_ERR_CRITICAL up. A refusal is DWG_ERR_INVALIDDWG with a bit for
   the reason: DWG_ERR_NOTYETSUPPORTED for a value, definition or action
   the evaluator doesn't support, or a drawing other than r2004;
   DWG_ERR_UNHANDLEDCLASS for an entity or graph node of a class LibreDWG
   can't decode; DWG_ERR_INVALIDHANDLE for an entity of def, or one it
   owns, that doesn't resolve or isn't an entity, a node of def's
   evaluation graph that doesn't resolve, or an action selecting an object
   that is neither one of def's entities nor a node of its evaluation
   graph; DWG_ERR_VALUEOUTOFBOUNDS for a name no
   property has, a name given twice, a value that isn't finite, a LINEAR
   value below 0, a state the parameter doesn't have, or a flip value other
   than 0 or 1; DWG_ERR_INVALIDTYPE when def is not a dynamic block
   definition; and DWG_ERR_INVALIDDWG alone for an action or parameter
   whose data isn't what AutoCAD writes, such as a stretch point an entity
   doesn't have, or actions that move each other's parameters in a cycle.
   Or the copy's
   refusal, such as of an entity handle def lists twice. A refusal adds
   nothing to the document, and the reason is logged at LOG_ERROR.
   Otherwise DWG_ERR_OUTOFMEM, which leaves what was made in the document,
   or DWG_ERR_INTERNALERROR for NULL arguments. */
EXPORT Dwg_Object_BLOCK_HEADER *
dwg_dynblock_evaluate (Dwg_Object_BLOCK_HEADER *def,
                       const Dwg_DynValue *values, const BITCODE_BL num_values,
                       int *errorp);

/* The outcome of checking one placement's representation block. */
typedef enum DWG_DYNBLOCK_CHECK_STATUS
{
  /* every entity of the evaluation matches AutoCAD's, and the evaluation
     left out no data */
  DWG_DYNBLOCK_CHECK_MATCH = 0,
  /* the evaluation differs from AutoCAD's block, or left out data it
     could not copy */
  DWG_DYNBLOCK_CHECK_MISMATCH,
  /* the evaluator doesn't support the placement's values or definition, or
     the drawing: reading or evaluating them was refused with
     DWG_ERR_NOTYETSUPPORTED or DWG_ERR_UNHANDLEDCLASS, or a stored value
     isn't read */
  DWG_DYNBLOCK_CHECK_UNSUPPORTED,
  /* no values to evaluate: no INSERT draws the block, or its INSERT has no
     AcDbBlockRepresentation */
  DWG_DYNBLOCK_CHECK_NO_VALUES,
  /* reading the values or evaluating them failed otherwise: any other
     refusal, such as of a stored value that is missing, or of a reference
     that doesn't resolve, or no memory */
  DWG_DYNBLOCK_CHECK_FAILED
} Dwg_DynBlock_Check_Status;

typedef struct _dwg_dynblock_check
{
  BITCODE_RLL insert; /* the placement, 0 for a block none draws */
  BITCODE_RLL block;  /* the representation block AutoCAD stored */
  /* the definition: the one the placement's values name, or else the one
     the block's AcDbBlockRepBTag names */
  BITCODE_RLL definition;
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
  /* UTF-8: what differs first, preceded by the bits of the data the
     evaluation left out, if it left some out; or why the check is
     unsupported or failed; NULL for a match */
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
   dwg. For every INSERT or MINSERT that draws a representation block (an
   anonymous block with AcDbBlockRepBTag EED), it reads the INSERT's values,
   evaluates its definition at them, and compares the evaluated block with
   AutoCAD's entity by entity, paired by their AcDbBlockRepETag index. A
   representation block no INSERT draws gets a check too, with status
   DWG_DYNBLOCK_CHECK_NO_VALUES.

   Entities compare by type, by their invisible flag, layer, colour,
   linetype flags, linetype, linetype scale, lineweight, plot style flags
   and plot style, and field by field with the dynapi field lists:
   numbers within tolerance, strings and binary data, such as a proxy
   entity's, exactly, and references by the entity they point at, paired
   by index within the blocks, so the same handle matches, whether or not
   it resolves. An entry of either block's entity list that doesn't
   resolve is a difference. An array the comparison has no length for,
   unless empty in both, counts as a difference. An ELLIPSE also matches
   the same curve with its major axis reversed, and is counted in
   num_equivalent. The blocks' base points compare too: a difference there
   is a mismatch, though not of an entity. EED is not compared.

   An evaluation that returns bits below DWG_ERR_CRITICAL, for data its
   copy left out (such as an array, 3DSOLID ACIS data or an EED group it
   could not copy), is a mismatch whatever the comparison finds, as the
   comparison doesn't see all such data. Its reason gives those bits first,
   then what differs, if anything.

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
