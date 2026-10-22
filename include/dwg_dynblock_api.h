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
 * dynamic-block data, from one document into another, or copy the
 * entities of one document's block or model space into a block of
 * another; describe a definition's properties, read a placement's values,
 * and evaluate a definition at chosen values into the geometry of a
 * representation block, with a check of that evaluation against the
 * representation blocks AutoCAD stored in a drawing; place a definition at
 * chosen values, as AutoCAD stores a placement; give a block a linear
 * property with stretch and move actions, as AutoCAD's Block Editor
 * stores them; and move a block's contents so that its base point is the
 * origin, where AutoCAD expects a dynamic block's.
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

/**
 * Copy the entities of src_block, a block or the model space of document
 * src, into dst_block, a block or the model space of another document.
 *
 * The copies are added to the end of dst_block's entity list, in
 * src_block's order, with what they own: attributes, vertices and
 * extension dictionaries. They are placed as an INSERT of src_block at
 * dst_block's base point plus offset would draw them, unscaled and
 * unturned: each moves by that point minus src_block's base point. The
 * base point of model space is the drawing's INSBASE, where AutoCAD puts
 * the drawing's base when it inserts the drawing as a block; that of
 * another block is its own. References out of the entities are matched
 * by name in dst, and what dst lacks is copied, as dwg_dynblock_copy_block
 * () does for a definition: a block an INSERT draws is copied with it
 * unless dst has a block of its name.
 *
 * A copy keeps its source's EED, but the copies listed in dst_block lose
 * their AcDbBlockRepETag group, which numbers the entities of a dynamic
 * block definition: when dst_block is one, they are numbered after its own
 * entities, as the dwg_dynblock_add_linear () family numbers them. The
 * ATTRIBs a copied INSERT owns keep theirs, which pair them with the
 * attribute definitions of the block the INSERT draws. Copied attribute
 * definitions set dst_block's flag for them. The copies are drawn after
 * dst_block's own entities, unless an ACAD_SORTENTS table of dst_block
 * orders them otherwise; that table and src_block's are not changed or
 * copied. src_block's dynamic-block data, if any, is not copied: an entity
 * copies with its invisible flag as it is.
 *
 * Refused, with nothing changed in dst: a NULL argument, or src the document
 * of dst_block (DWG_ERR_INTERNALERROR); dst_block not a BLOCK_HEADER of its
 * document, src_block not one of src, an entry of src_block's entity list that
 * doesn't resolve to an entity, or copies that would draw dst_block, as an
 * INSERT of a block of src that is matched with dst_block, or with a block of
 * dst that draws it at any depth (DWG_ERR_INVALIDHANDLE); documents that
 * aren't both r2004 (AC1018), or have different codepages
 * (DWG_ERR_NOTYETSUPPORTED); paper space or another layout block, an xref or a
 * block that depends on one, as src_block or dst_block (DWG_ERR_INVALIDTYPE);
 * an offset or base point that isn't finite, or a move that would take a
 * coordinate beyond what a double holds (DWG_ERR_VALUEOUTOFBOUNDS); with a
 * move, an entity of a type the move doesn't handle, which are those
 * dwg_dynblock_evaluate () moves whole (LINE, LWPOLYLINE, ARC, CIRCLE,
 * ELLIPSE, SPLINE, TEXT, ATTDEF, MTEXT, INSERT with its ATTRIBs, and HATCH),
 * or one whose 2D points the move would take out of their plane, as it would
 * those of an entity whose extrusion isn't along Z (DWG_ERR_NOTYETSUPPORTED);
 * and what dwg_dynblock_copy_block () refuses of what a definition owns, among
 * what the entities own, such as the objects of a constraint-based block's
 * network. Each refusal is DWG_ERR_INVALIDDWG with the bit given, logged at
 * LOG_ERROR but for a NULL argument.
 *
 * @param dst_block The block, or model space, to copy into.
 * @param src       The document src_block is in.
 * @param src_block The block, or model space, whose entities to copy.
 * @param offset    Where src_block's base point goes, from dst_block's.
 * @param errorp    If not NULL, receives 0 or DWG_ERR_* bits: from
 *                  DWG_ERR_CRITICAL up, a refusal as above, or
 *                  DWG_ERR_OUTOFMEM: while copying, after which dst may hold
 *                  copies that no block lists, and none is in dst_block's
 *                  list; or while numbering the copies, after which they are
 *                  in dst_block's list, and some may be left unnumbered.
 *                  Below it, the bits of dwg_dynblock_copy_block () for
 *                  references and data the copy left out, and
 *                  DWG_ERR_INVALIDEED when dst_block's counter of entity
 *                  numbers leaves no room for the copies, which are then
 *                  left unnumbered.
 * @return the number of entities added to dst_block's list, also when
 *         memory ran out while numbering them; 0 when refused, or when
 *         memory ran out while copying.
 */
EXPORT BITCODE_BL
dwg_dynblock_copy_entities (Dwg_Object_BLOCK_HEADER *dst_block,
                            const Dwg_Data *src,
                            const Dwg_Object_BLOCK_HEADER *src_block,
                            const BITCODE_3BD *offset, int *errorp);

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
   wholly inside the frame, which must be a rectangle, and have its
   extrusion along +Z. As AutoCAD does, a move action moves an ATTDEF only
   when its position is locked: in r2004 files, when its AcDbAttr EED
   group is 1070 0, 1070 1 (see dwg_dynblock_insert ()). It leaves one that
   isn't locked where it is, as does a stretch action that selects one
   wholly outside its frame, while a stretch action whose frame holds its
   insertion point moves it either way. The actions also move the points of
   the parameters they select, so a LINEAR value is the distance after
   other properties' actions moved its parameter, as AutoCAD's Properties
   palette shows it.
   Only the actions of the properties that run run: those given a value
   that changes their parameter, and the linear ones given a value whose
   parameters the stretch or move actions of such a property move. A
   LINEAR value is not checked against the parameter's value set.
   Refused, with DWG_ERR_NOTYETSUPPORTED: a LINEAR value that changes its
   parameter when the parameter drives an action of another kind (such as
   scale), or has its base point in its middle; linear properties that run
   whose actions move each other's parameters in a cycle, as the order
   AutoCAD evaluates them in is not known; an action moving a parameter,
   of any kind, that chains its actions, which AutoCAD would then run too;
   an entity a stretch action selects with none of its stretch points in
   the frame, which AutoCAD moves whole, other than an ATTDEF that isn't
   locked, and a parameter it lists with none of its points in the frame;
   a parameter an action selects but doesn't list among those it moves,
   other than its own, that chains its actions or is a rotation parameter
   given a value; a stretch of a TEXT or ATTDEF aligned or fitted between
   two points, or of part of an entity of another type; a HATCH that
   crosses the frame; an action moving an entity of another type, taking
   another point's change than the end or base point's, or with an XY
   type; a move that would take 2D points out of their plane.

   A ROTATION value turns its parameter's end point about its base point,
   and the rotate actions connected to the parameter then turn the
   entities they select, whole, about that base point, counter-clockwise
   by the change in angle: a LINE, LWPOLYLINE, ARC, CIRCLE, ELLIPSE,
   SPLINE, TEXT, MTEXT, POINT, INSERT (with its ATTRIBs) or a solid HATCH
   bounded by polylines and lines. Angles the turn changes, such as an
   ARC's or an INSERT's rotation, are kept in [0, 2 pi). A ROTATION value
   is not checked against the parameter's value set. Refused, with
   DWG_ERR_NOTYETSUPPORTED: a ROTATION value that changes its parameter
   when the parameter drives an action of another kind than rotate; a
   rotate action taking other points of its parameter than its base point
   and angle change, with a base point independent of its parameter's, or
   offset from it; a turn of an entity of another type, such as an
   ATTDEF, of one whose extrusion is not along Z (either way), or of a
   HATCH with a pattern, a gradient, or a boundary segment other than a
   line; an entity that a rotate action turns and another action that
   runs moves or turns too, or that one rotate action lists twice, as the
   order AutoCAD runs them in is not known; an action that runs moving a
   rotation parameter given a value, or a rotate action that runs turning
   a linear parameter that runs.

   A FLIP value is accepted at 0 and refused otherwise; a value of any
   other kind (XY, POLAR, POINT, LOOKUP, CONSTRAINT or USER) is refused
   whatever it is. A definition that is constraint-based (parametric) is
   refused.

   Returns the new block, with *errorp 0, or below DWG_ERR_CRITICAL the
   bits of the copy for data it left out (such as an array, 3DSOLID ACIS
   data or an EED group it could not copy). Otherwise NULL with *errorp
   from DWG_ERR_CRITICAL up. A refusal is DWG_ERR_INVALIDDWG with a bit for
   the reason:
   - DWG_ERR_NOTYETSUPPORTED for a value, definition or action the
     evaluator doesn't support, or a drawing other than r2004;
   - DWG_ERR_UNHANDLEDCLASS for an entity or graph node of a class
     LibreDWG can't decode;
   - DWG_ERR_INVALIDHANDLE for an entity of def, or one it owns, that
     doesn't resolve or isn't an entity; a node of def's evaluation graph
     that doesn't resolve; an object in an action's selection that is
     neither one of def's entities nor a node of its graph; an object
     whose stretch points a stretch action lists that isn't one of def's
     entities; a node id an action names that the graph doesn't have;
   - DWG_ERR_VALUEOUTOFBOUNDS for a name no property has, a name given
     twice, a value that isn't finite, a LINEAR value below 0, for a
     parameter longer than a double holds (or with a point that isn't
     finite), or that would move a point beyond what a double holds, a
     ROTATION value for a parameter with a point that isn't finite, or that
     would turn a point beyond what a double holds, a state the parameter
     doesn't have, a flip value other than 0 or 1, or a document whose
     block table is full (65535 blocks);
   - DWG_ERR_INVALIDTYPE when def is not a dynamic block definition;
   - no further bit for an action or parameter whose data isn't what
     AutoCAD writes, such as a stretch point an entity or a linear
     parameter doesn't have.
   Or the copy's refusal, such as of an entity handle def lists twice. A
   refusal adds nothing to the document, and the reason is logged at
   LOG_ERROR. Otherwise DWG_ERR_OUTOFMEM, which leaves what was made in the
   document, or DWG_ERR_INTERNALERROR for NULL arguments. */
EXPORT Dwg_Object_BLOCK_HEADER *
dwg_dynblock_evaluate (Dwg_Object_BLOCK_HEADER *def,
                       const Dwg_DynValue *values, const BITCODE_BL num_values,
                       int *errorp);

/* The value of an attribute, for dwg_dynblock_insert (). UTF-8. */
typedef struct _dwg_attvalue
{
  char *tag; /* the attribute definition's tag, as it has it */
  char *value;
  /* The value's width as AutoCAD lays it out in its text style's font, at
     a height of 1 and a width factor of 1, which places text justified
     centre, right or middle as AutoCAD does (see dwg_dynblock_insert ()).
     For a TrueType font: from the start of the text to the end of its
     last character, that is the sum of the characters' advance widths, or
     to the right edge of the last character's outline where that reaches
     further, in font units, divided by the height of the font's capital A
     (AutoCAD draws a capital A as tall as the text): 1466 of Arial's 2048
     units. AutoCAD applies no kerning. 0 for none, so zero the struct
     before filling it. */
  double width;
} Dwg_AttValue;

/* Places dynamic block definition def in block owner, such as model space,
   at the given values, as AutoCAD stores a placement, and returns the new
   INSERT. It is at ins_pt, with a uniform scale and a rotation about Z in
   radians.

   values are as dwg_dynblock_evaluate () takes them, in the definition's
   units: for a scaled placement, AutoCAD's Properties palette shows a
   LINEAR value multiplied by the scale. Properties left out keep the
   definition's values.

   At the definition's values (the first visibility state, and every other
   value the definition's), the INSERT draws def itself. Otherwise it draws
   a new representation block, which dwg_dynblock_evaluate () makes, and
   which gets what AutoCAD's have: AcDbBlockRepBTag EED naming def, def's
   hasattrs flag, an ACAD_SORTENTS table of its own when def has one,
   which draws the block's entities in the order def draws its own (the
   entities' handles are handed out as sort keys in that order), its
   nested INSERTs and MINSERTs listed by the blocks they draw, and for each
   IMAGE or WIPEOUT that has an image definition reactor, a reactor of its
   own. The INSERT then gets the extension dictionary AutoCAD stores the
   values in: AcDbBlockRepresentation, holding AcDbRepData (a
   BLOCKREPRESENTATION that names def) and AppDataCache, holding
   ACAD_ENHANCEDBLOCKDATA with an XRECORD named by the node id for each
   graph node that AutoCAD keeps a record of: for a parameter, its points
   as the evaluation's actions leave them (a linear parameter's end point
   at its value, a rotation parameter's turned to its angle, and the points
   of the parameters the actions that run move or turn), and for an action
   what the definition holds. Each placement at other values gets a
   representation block of its own.

   A placement at other values also gives def the AcDbBlockRepETag EED
   group AutoCAD keeps on a dynamic block definition once it has evaluated
   it, when def has none: 1070 1, then a 1071 counter, one more than the
   highest AcDbBlockRepETag index of def's entities, which keep their
   indexes, gaps included. AutoCAD pairs a representation block's entities
   with the definition's by that index. When it evaluates a definition
   without the group itself, as for an edit of a placement at the
   definition's values, a RESETBLOCK or a new placement, it numbers the
   definition's entities afresh and adds the group; it then no longer
   pairs the representation blocks made before with the definition, and
   its edits of their placements change their stored values but not their
   geometry. A group def has already is left as it is. A def none of whose
   entities has an index gets no group.

   Attributes: the INSERT gets an ATTRIB for each attribute definition of
   the block it draws that isn't constant, where the evaluation moved it
   when its position is locked (see below), in the block's order, with the
   definition's tag, flags, text style, height, width factor, oblique
   angle, alignment, layer, colour, linetype, lineweight and plot style;
   its position, alignment point, height, thickness and rotation placed by
   the INSERT (text left-aligned on the baseline has no alignment point,
   and the ATTRIB stores none, as AutoCAD's don't); and its value from
   atts, by tag, or else the definition's
   default value. A preset attribute takes its value from atts too: preset
   only means AutoCAD doesn't prompt for it. At the definition's values
   each ATTRIB gets the AcDbBlockRepETag index of its definition, as
   AutoCAD's do. Each ATTRIB gets a copy of its definition's AcDbAttr EED,
   as AutoCAD's do: AutoCAD 2007 and later keep an attribute's lock
   position there in r2004 files (1070 0, 1070 1 when locked, and no group
   otherwise), and leave an ATTRIB that isn't locked where it is when an
   edit of the placement moves its definition. So the ATTRIB of an
   attribute definition that isn't locked starts where def's own starts,
   wherever the evaluation moved the representation block's copy. They
   are added with dwg_add_ATTRIB (), which links them to the INSERT as
   AutoCAD does.

   Where text justified centre, right or middle starts, its insertion
   point, AutoCAD derives from its alignment point and the width it
   measures for the text in its font, and draws a stored ATTRIB from that
   insertion point. LibreDWG has no fonts, so for such an attribute give
   the value's width in atts (Dwg_AttValue's width). The ATTRIB then starts
   where AutoCAD starts it: back from its alignment point along the
   baseline (forward for backward text) by that width at the ATTRIB's
   height and width factor, or by half of it when centred or middle; and
   across the baseline as far as the definition's text does. Two things
   AutoCAD measures that a width doesn't hold: it centres text on its
   first character's outline where that starts left of the text's start,
   so a centred or middle value beginning with such a character, as
   Arial's A, j and _ do (by 3, 94 and 31 of its 2048 units), sits half
   that overhang off; and it centres middle text on the height of its
   characters too, which LibreDWG takes from the definition's text, so a
   value whose characters reach lower or higher than those, such as one
   with descenders against capitals, sits off its alignment point across
   the baseline, by up to about half the height. Text in a vertical style
   starts where the definition's does. With no width (0), or for an
   attribute atts don't give, the ATTRIB starts where the definition's
   text starts, which AutoCAD laid out for the definition's tag or default
   value: a value of another width then sits off its alignment point along
   the baseline in AutoCAD, though not in LibreDWG's SVG output, which
   places such text by its alignment point. To place a default value as
   AutoCAD does, give it in atts with its width. Text left-aligned,
   aligned or fit, or with a vertical alignment above 3, which AutoCAD
   doesn't define, starts where the definition's does whatever the width.
   A width that is negative or isn't finite, or at which the text's start,
   a coordinate of its offset from its alignment point, or a sum on the
   way to the start would be beyond what a double holds, is refused, with
   nothing added (DWG_ERR_VALUEOUTOFBOUNDS). Such a sum is an alignment
   point's coordinate plus the part of the offset along the baseline, or
   the distance across the baseline of a definition whose insertion and
   alignment points are more than a double apart.

   Refused, with nothing added: owner or def not a BLOCK_HEADER of their
   document, or of different documents (DWG_ERR_INVALIDHANDLE); a drawing
   other than r2004 (DWG_ERR_NOTYETSUPPORTED); def not a dynamic block
   definition (DWG_ERR_INVALIDTYPE); owner def, or a block def inserts at
   any depth, an insertion point, scale or rotation that isn't finite, a
   scale that isn't positive, or a LINEAR or ROTATION value other than the
   definition's that the parameter's value set doesn't allow, which AutoCAD
   would move to one it allows on its next edit of the placement: one not
   in the set's list, or with no list one outside its limits or off its
   increment, counted from its minimum or 0 (DWG_ERR_VALUEOUTOFBOUNDS);
   values that dwg_dynblock_evaluate () refuses, with its reasons, such as
   a FLIP value of 1; for a representation block, a graph node of a class
   whose stored value is not known (one of a class LibreDWG can't decode
   is refused first, as dwg_dynblock_evaluate () refuses it, with
   DWG_ERR_UNHANDLEDCLASS), a rotate or scale action whose base point
   isn't (0, 0, 0), an entity that draws a block of its own, as a
   DIMENSION does, or values at which an action that runs moves a
   parameter in a way whose stored points AutoCAD's placements have not
   shown: a rotation parameter that another action moves or turns, a base
   point parameter that an action moves or turns, a parameter an action
   selects without listing it among those it moves, an action listing its
   own parameter or one parameter twice, a stretch point that the record
   of a parameter other than a linear one doesn't hold, or a point listed
   twice, a rotation parameter given a value whose end point is at its
   base point, or a parameter that a rotate action turns and another
   action moves or turns too (DWG_ERR_NOTYETSUPPORTED), or that would move
   a parameter's point beyond what a double holds, or leave the points of
   a parameter given a value that don't hold it, as at coordinates so large
   that they round (DWG_ERR_VALUEOUTOFBOUNDS); an attribute definition
   whose extrusion
   isn't (0, 0, 1), out of the XY plane, or with a tag dwg_add_ATTRIB ()
   refuses, such as one with a period (DWG_ERR_NOTYETSUPPORTED); an
   attribute definition whose placed position, alignment point, height or
   thickness is beyond what a double holds, at a huge scale, its position
   and alignment point being where its ATTRIB starts from: where the
   evaluation moved it when its position is locked, and def's own
   otherwise (see Attributes above), or an attribute tag that names no
   attribute definition that isn't constant, is given twice, or has no
   value (DWG_ERR_VALUEOUTOFBOUNDS). In a
   LibreDWG built without write support, a placement at other values than
   the definition's is refused (DWG_ERR_NOTYETSUPPORTED).

   @param errorp If not NULL, receives 0 or DWG_ERR_* bits. From
                 DWG_ERR_CRITICAL up no INSERT is returned: a refusal is
                 DWG_ERR_INVALIDDWG with a bit for the reason, logged at
                 LOG_ERROR; DWG_ERR_INTERNALERROR for NULL arguments; any
                 other failure, such as DWG_ERR_OUTOFMEM, may leave what was
                 made in the document. Below it, the representation block
                 was made with data left out, as dwg_dynblock_evaluate ()
                 returns, or an ATTRIB was left without its definition's
                 AcDbAttr EED, which wasn't 1070 items only, decoded in
                 full, or def without its AcDbBlockRepETag counter, as its
                 highest index leaves no room for one in a 1071, a signed
                 32-bit integer (DWG_ERR_INVALIDEED).
   @return the INSERT, or NULL. */
EXPORT Dwg_Entity_INSERT *
dwg_dynblock_insert (Dwg_Object_BLOCK_HEADER *owner,
                     Dwg_Object_BLOCK_HEADER *def, const BITCODE_3BD *ins_pt,
                     const double scale, const double rotation,
                     const Dwg_DynValue *values, const BITCODE_BL num_values,
                     const Dwg_AttValue *atts, const BITCODE_BL num_atts,
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
     that doesn't resolve */
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
  /* of which match only as the same entity stored another way: an ELLIPSE
     with its major axis reversed and its parameters turned by pi, as
     AutoCAD keeps one after some grip edits; or text that differs only in
     what AutoCAD measures from its layout, which the evaluation can't do
     (see dwg_dynblock_check ()) */
  BITCODE_BL num_equivalent;
  /* UTF-8: what differs first, preceded by the bits of the data the
     evaluation left out, if it left some out; or why the check is
     unsupported, failed or has no values; NULL for a match */
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
   DWG_DYNBLOCK_CHECK_NO_VALUES. Blocks made before AutoCAD numbered their
   definition's entities afresh, which it does when it evaluates a
   definition without an AcDbBlockRepETag group (see dwg_dynblock_insert
   ()), no longer pair with the definition, and are mismatches.

   Entities compare by type, by their invisible flag, layer, colour,
   linetype flags, linetype, linetype scale, lineweight, plot style flags
   and plot style, and field by field with the dynapi field lists:
   floating-point numbers within tolerance, integers, strings and binary
   data, such as a proxy entity's, exactly, and references by the entity
   they point at, paired by index within the blocks, so the same handle
   matches, whether or not it resolves. The angles a turn changes, an
   ARC's, the rotation of an INSERT, TEXT or ATTRIB and a POINT's X axis
   angle, compare around the circle, so that 0 matches 2 pi, and an ARC's
   sweep from its start to its end angle must match too. An entry of
   either block's entity list that doesn't resolve is a difference. An
   array the comparison has no length for, unless empty in both, counts as
   a difference. An ELLIPSE also matches the same curve with its major
   axis reversed, and is counted in num_equivalent. So does text that
   differs only in what AutoCAD measures from the laid-out text, which the
   evaluation can't do: an MTEXT's extents_width or extents_height where
   one side has it unmeasured (0 or less: AutoCAD writes 0 or -1) and
   both are finite, and the insertion point of an attribute definition
   that isn't constant, justified other than left, aligned or fit (centre,
   right or middle), where the alignment point matches and the insertion
   point differs only along the baseline, or for middle text also across
   it by up to the text's height. AutoCAD derives that insertion point
   from the alignment point and the width of the tag in a definition, but
   of the default value in the blocks it evaluates, and centres middle
   text on the height of its characters too. The
   blocks' base points compare too: a difference there is
   a mismatch, though not of an entity. EED is not compared.

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

/* Gives block blk a linear property named name: a length, from base_pt to
   end_pt in blk's coordinates, that AutoCAD's Properties palette shows for
   each placement of blk, with a grip at end_pt to drag. A new value moves
   the end point along the line from base_pt, and the stretch and move
   actions that dwg_dynblock_add_stretch () and dwg_dynblock_add_move ()
   give the property move entities by that change. The property has no
   value set, so any length is allowed, and its parameter doesn't chain
   actions. It is stored as AutoCAD's Block Editor stores a linear
   parameter with one grip, in r2004 files: the parameter, its grip and
   the grip's two location components, as nodes of blk's evaluation
   graph.

   blk may be a block definition, or model space: a drawing whose model
   space holds a dynamic block's evaluation graph is how AutoCAD writes a
   dynamic block to a file of its own (WBLOCK), and is a dynamic block
   definition when AutoCAD inserts the drawing as a block.

   On a block that isn't yet a dynamic block definition, it first makes it
   one: an EVALUATION_GRAPH under ACAD_ENHANCEDBLOCK in blk's extension
   dictionary, which is added when blk has none, and for a block other
   than model space the AcDbDynamicBlockRoundTripPurgePreventer that
   AutoCAD keeps beside the graph. On a dynamic block definition, the
   parameter joins its graph, with node ids after the highest the graph
   has given out.

   It also numbers blk's entities as AutoCAD numbers a definition's once
   it has evaluated it: each entity without an AcDbBlockRepETag index gets
   the next one, as the EED group 1070 1, 1071 index, 1005 its own handle,
   and blk keeps the next index to give in its own group, 1070 1, 1071
   index. AutoCAD pairs the entities of a placement's representation block
   with the definition's by those indexes (see dwg_dynblock_insert ()).
   dwg_dynblock_add_stretch () and dwg_dynblock_add_move () number them
   too, for entities added since.

   Refused, with nothing changed: a NULL argument (DWG_ERR_INTERNALERROR); blk
   not a BLOCK_HEADER of its document, or a node of its evaluation graph that
   doesn't resolve or isn't a parameter, action, grip or grip location
   component (DWG_ERR_INVALIDHANDLE); a drawing other than r2004, or a
   constraint-based (parametric) block, which dwg_dynblock_evaluate () refuses:
   one whose extension dictionary holds an ACAD_ASSOCNETWORK, the network of
   its constraints, whether or not it has an evaluation graph, or whose graph
   has a proxy node, a constraint parameter or a user parameter
   (DWG_ERR_NOTYETSUPPORTED); a node of another class LibreDWG can't decode
   (DWG_ERR_UNHANDLEDCLASS); paper space or another layout block, an xref or a
   block that depends on one (DWG_ERR_INVALIDTYPE); an empty name, or one that
   a property of blk has already, as dwg_dynblock_get_params () names them,
   ASCII letters compared without case; points that aren't finite, so far apart
   that their distance overflows a double, no more than 1e-9 apart, which the
   evaluator takes as no length, or at different heights, or a graph with no
   node ids left for four nodes, its ids being signed 32-bit integers
   (DWG_ERR_VALUEOUTOFBOUNDS); a graph whose nodes and edges index nodes or
   edges it doesn't have (no further bit). Each refusal is DWG_ERR_INVALIDDWG
   with the bit given, logged at LOG_ERROR but for a NULL argument. In a
   LibreDWG built without write support, all are refused
   (DWG_ERR_NOTYETSUPPORTED).

   @param errorp If not NULL, receives 0 or DWG_ERR_* bits: from
                 DWG_ERR_CRITICAL up, a refusal as above, or
                 DWG_ERR_OUTOFMEM, which may leave some of what was made
                 in the document; below it, DWG_ERR_INVALIDEED when blk's
                 counter of entity numbers leaves no room for more, and
                 its entities without a number are left without one.
   @return the new parameter, or NULL. */
EXPORT Dwg_Object_BLOCKLINEARPARAMETER *
dwg_dynblock_add_linear (Dwg_Object_BLOCK_HEADER *blk, const char *name,
                         const BITCODE_3BD *base_pt,
                         const BITCODE_3BD *end_pt, int *errorp);

/* Gives the linear property of param, a parameter that
   dwg_dynblock_add_linear () made or AutoCAD made, of block blk, a stretch
   action: a new value of the property moves the stretch points of the
   given entities that lie in the rectangle with opposite corners corner1
   and corner2, in blk's XY plane, by the change of param's end point, as
   dwg_dynblock_evaluate () evaluates it. Those points are the ends of a
   LINE and the vertices of an LWPOLYLINE, which then stretch; a CIRCLE's
   centre, an INSERT's insertion point, and the insertion point of a TEXT
   or ATTDEF justified left on its baseline, which then move whole, as
   does a LINE or LWPOLYLINE with all its points in the rectangle. A point
   on the rectangle's edge is in it. The action is stored as AutoCAD's
   Block Editor stores a stretch of those entities with that frame: each
   entity with the indexes of its stretch points in the frame, numbered as
   dwg_dynblock_evaluate () numbers them. Its distance multiplier is 1.

   entities are the handles of blk's entities to stretch, each with at
   least one stretch point in the frame: AutoCAD moves whole an entity of
   the selection that lies wholly outside the frame, which
   dwg_dynblock_evaluate () doesn't evaluate. The entities are numbered as
   dwg_dynblock_add_linear () numbers them. Each ATTDEF among them gets its
   position locked, as AutoCAD's Block Editor expects of an attribute
   definition an action moves: AutoCAD's move actions leave one that isn't
   locked where it is, and the ATTRIBs of its placements stay behind when
   their definition moves. Its AcDbAttr EED group becomes 1070 0, 1070 1, a
   new group appended to its EED in place of any it had, and the document
   gets the AcDbAttr APPID when it has none; its lock_position_flag, which
   r2007 and later files store instead, is set too. An ATTDEF already
   locked is left as it is. The lock is the ATTDEF's own, so from then on
   the block's other move actions that select it move it too, and a
   stretch action that selects it wholly outside its frame moves it whole,
   as AutoCAD moves any entity selected so: dwg_dynblock_evaluate () then
   refuses the values that run that action, as it refuses such a selection
   of any other entity. The ATTRIBs of placements made before keep their
   own AcDbAttr EED.

   Refused, with nothing changed: a NULL argument (DWG_ERR_INTERNALERROR); blk
   not a BLOCK_HEADER of its document, param not a linear parameter of blk's
   evaluation graph (nor a copy of its struct), or a handle that isn't one of
   blk's entities (DWG_ERR_INVALIDHANDLE); a drawing other than r2004, a
   constraint-based block, an entity of another type, an LWPOLYLINE, CIRCLE,
   INSERT, TEXT or ATTDEF whose extrusion isn't along +Z (an LWPOLYLINE that
   stores none, as read from a file, is), an entity with no stretch point in
   the frame, one whose 2D points a change along param would take out of their
   plane by more than 1e-12 of the change, as it would those of an ATTRIB an
   INSERT owns whose plane param's direction leaves, whatever param's length,
   or a param with its base point in its middle, or driving an action other
   than a stretch or a move, which dwg_dynblock_evaluate () refuses to
   evaluate, or an entity a rotate action of the graph turns, which it can't
   order with the stretch (DWG_ERR_NOTYETSUPPORTED); no entities, an entity
   given twice, corners that aren't finite or don't span a rectangle, sharing
   an x or a y, or a graph with no node id left (DWG_ERR_VALUEOUTOFBOUNDS).
   Each refusal is DWG_ERR_INVALIDDWG with the bit given, logged at LOG_ERROR
   but for a NULL argument; errorp and the return value are as for
   dwg_dynblock_add_linear (). */
EXPORT Dwg_Object_BLOCKSTRETCHACTION *
dwg_dynblock_add_stretch (Dwg_Object_BLOCK_HEADER *blk,
                          const Dwg_Object_BLOCKLINEARPARAMETER *param,
                          const BITCODE_2RD *corner1,
                          const BITCODE_2RD *corner2,
                          const BITCODE_RLL *entities,
                          const BITCODE_BL num_entities, int *errorp);

/* Gives the linear property of param, a parameter of block blk as for
   dwg_dynblock_add_stretch (), a move action: a new value of the property
   moves the given objects by the change of param's end point times
   multiplier, as dwg_dynblock_evaluate () evaluates it. A multiplier of
   0.5 keeps the objects centred on a length that grows from its base
   point.

   objects are handles of blk's entities, which move whole, of the types
   dwg_dynblock_evaluate () moves whole (LINE, LWPOLYLINE, ARC, CIRCLE,
   ELLIPSE, SPLINE, TEXT, ATTDEF, MTEXT, INSERT with its ATTRIBs, and
   HATCH); and of linear, visibility or flip parameters of blk's
   evaluation graph other than param, which move with them, points and
   grips, so that the dynamic properties a block already has stay with the
   entities they act on. A parameter is stored in the action's selection
   with its grips, and listed by node id among the parameters it moves, as
   AutoCAD's Block Editor stores a move whose selection holds a parameter.
   The entities are numbered as dwg_dynblock_add_linear () numbers them,
   and the ATTDEFs among them locked as dwg_dynblock_add_stretch () locks
   them.

   Refused, with nothing changed: as for dwg_dynblock_add_stretch (), for blk,
   param and the entities, with a handle that is neither one of blk's entities
   nor a node of its graph; an entity of another type, or one whose 2D points
   the move would take out of their plane, by the same measure as for
   dwg_dynblock_add_stretch (), a node of the graph other than a linear,
   visibility or flip parameter (a parameter of another kind, a grip or an
   action), a parameter that chains actions, or a linear parameter whose own
   actions move param, directly or through other parameters' actions, or an
   entity or parameter that a rotate action of the graph turns, selecting it or
   listing it, which the evaluator can't order (DWG_ERR_NOTYETSUPPORTED); a
   multiplier that isn't finite, no objects, an object given twice, or a graph
   with no node id left (DWG_ERR_VALUEOUTOFBOUNDS). errorp and the return value
   are as for dwg_dynblock_add_linear (). */
EXPORT Dwg_Object_BLOCKMOVEACTION *
dwg_dynblock_add_move (Dwg_Object_BLOCK_HEADER *blk,
                       const Dwg_Object_BLOCKLINEARPARAMETER *param,
                       const BITCODE_RLL *objects,
                       const BITCODE_BL num_objects, const double multiplier,
                       int *errorp);

/* Moves the contents of block blk so that its base point is the origin:
   its entities, and the points of the parameters, grips and actions of
   its evaluation graph, move by minus its base point, which becomes
   (0, 0, 0). An INSERT of blk then draws it where it did. AutoCAD draws a
   dynamic block's grips as if its base point were the origin, which its
   Block Editor keeps it at: those of a definition whose base point is
   elsewhere are drawn off its geometry by that point. blk may be a block
   definition, dynamic or not, or model space, whose base point is the
   drawing's INSBASE, from which AutoCAD inserts the drawing as a block. A
   block whose base point is the origin is left as it is.

   Entities move whole, as dwg_dynblock_evaluate () moves them, with the
   ATTRIBs an INSERT owns. The points that move are a linear parameter's
   base and end points, a visibility parameter's point, the location of
   their grips, an action's display location, and a stretch action's
   frame. A linear grip's orientation, a vector, and an action's offsets
   stay as they are, as do the drawing's views, its limits and its record
   of its extents.

   Refused, with nothing changed: a NULL argument (DWG_ERR_INTERNALERROR); blk
   not a BLOCK_HEADER of its document, an entry of its entity list that doesn't
   resolve to an entity or names one listed before, or a node of its
   evaluation graph that doesn't resolve or isn't a parameter, action, grip or
   grip location component (DWG_ERR_INVALIDHANDLE); a drawing other than
   r2004, a constraint-based (parametric) block as for dwg_dynblock_add_linear
   (), a block with placements (an INSERT or MINSERT of it, or a
   representation block of it, which a placement at values other than the
   defaults draws), whose stored data hold its points as they are, an entity
   of a type the move doesn't handle, which are those dwg_dynblock_copy_entities
   () moves, or one whose 2D points the move would take out of their plane,
   or a node of another class, among them base point parameters and rotate and
   scale actions, whose stored points AutoCAD's files have only shown at the
   origin (DWG_ERR_NOTYETSUPPORTED); a node of a class LibreDWG can't decode
   (DWG_ERR_UNHANDLEDCLASS); paper space or another layout block, an xref or a
   block that depends on one (DWG_ERR_INVALIDTYPE); a base point that isn't
   finite, or a move that would take a coordinate beyond what a double holds
   (DWG_ERR_VALUEOUTOFBOUNDS); a graph whose nodes and edges index nodes or
   edges it doesn't have (no further bit). Each refusal is DWG_ERR_INVALIDDWG
   with the bit given, logged at LOG_ERROR but for a NULL argument. In a
   LibreDWG built without write support, all are refused
   (DWG_ERR_NOTYETSUPPORTED).

   @return 0, or the DWG_ERR_* bits of a refusal, or DWG_ERR_OUTOFMEM, with
           nothing changed. */
EXPORT int dwg_dynblock_move_to_origin (Dwg_Object_BLOCK_HEADER *blk);

/* The name of a kind, such as "linear", or "unknown". */
EXPORT const char *dwg_dynparam_kind_name (const Dwg_DynParam_Kind kind);

#ifdef __cplusplus
}
#endif

#endif /* DWG_DYNBLOCK_API_H */
