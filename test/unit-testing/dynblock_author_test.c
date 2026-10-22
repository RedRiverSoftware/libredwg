/* ex: ft=c: -*- mode: c; -*- */
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

/* dwg_dynblock_add_linear (), dwg_dynblock_add_stretch (),
   dwg_dynblock_add_move () and dwg_dynblock_copy_entities (). A box block
   built with the add API gets a Width that stretches its right side and
   moves its caption by half the change, as a block is laid out at a
   chosen width: the graph, its nodes and edges and the elements are as
   AutoCAD's Block Editor stores them (test-data/2004/Dynblocks.dwg's
   Window has the same parameter, grip and stretch); the evaluator
   evaluates the block, placements of it check, and a written r2004 file
   reads back the same. The blocks of test-data/2004/Dynblocks.dwg that
   AutoCAD made dynamic get a Width too, and their entities copy into
   other documents. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <sys/stat.h>

static int tracelevel;
// the library sets loglevel from each document's opts
static int test_loglevel;

#include "config.h"
#ifdef HAVE_UNISTD_H
#  include <unistd.h>
#endif
#include "dwg.h"
#include "tests_common.h"
#include "dwg_api.h"
#include "dwg_dynblock_api.h"

#define OK(...)                                                              \
  do                                                                          \
    {                                                                         \
      loglevel = test_loglevel;                                               \
      ok (__VA_ARGS__);                                                       \
    }                                                                         \
  while (0)
#define FAIL(...)                                                             \
  do                                                                          \
    {                                                                         \
      loglevel = test_loglevel;                                               \
      fail (__VA_ARGS__);                                                     \
    }                                                                         \
  while (0)
/* One check: ok when cond holds, else a failure counted in failures. */
#define CHECK(cond, ...)                                                      \
  do                                                                          \
    {                                                                         \
      if (cond)                                                               \
        OK (__VA_ARGS__);                                                     \
      else                                                                    \
        {                                                                     \
          FAIL (__VA_ARGS__);                                                 \
          failures++;                                                         \
        }                                                                     \
    }                                                                         \
  while (0)

#define FIXTURE "test/test-data/2004/Dynblocks.dwg"
#define WRITTEN "dynblock_author_test.dwg"
#define WRITTEN_COPY "dynblock_author_test_copy.dwg"
#define WRITTEN_LWPOLYLINE "dynblock_author_test_lwpolyline.dwg"
#define WRITTEN_LOCKED "dynblock_author_test_locked.dwg"
#define WRITTEN_VISIBILITY "dynblock_author_test_visibility.dwg"

/* The fixture's definitions */
#define TREES 0xB45B
#define TOILET 0xB8F7
#define DOOR 0xBAC4
#define DOOR_ELEVATION 0xBB0C /* constraint-based */
#define WINDOW 0xBBC1
/* *U31: constraint-based, with an ACAD_ASSOCNETWORK and no graph */
#define U31 0xBB72

/* The fixture reads with the errors of its constraint-based block only. */
#define KNOWN_ERRORS (DWG_ERR_UNHANDLEDCLASS | DWG_ERR_VALUEOUTOFBOUNDS)

/* A refusal: DWG_ERR_INVALIDDWG with the bit for the reason. */
#define REFUSED(bit) (DWG_ERR_INVALIDDWG | (bit))

#define EPSILON 1e-9

static char fixture[512];

static int
find_fixture (void)
{
  const char *dirs[3];
  struct stat attrib;
  dirs[0] = getenv ("top_srcdir");
  dirs[1] = "../..";
  dirs[2] = "../../..";
  for (size_t i = 0; i < sizeof (dirs) / sizeof (dirs[0]); i++)
    {
      if (!dirs[i])
        continue;
      snprintf (fixture, sizeof (fixture), "%s/%s", dirs[i], FIXTURE);
      if (!stat (fixture, &attrib))
        return 1;
    }
  return 0;
}

static int
read_dwg (const char *file, Dwg_Data *dwg)
{
  int error;
  memset (dwg, 0, sizeof (Dwg_Data));
  dwg->opts = tracelevel;
  error = dwg_read_file (file, dwg);
  loglevel = test_loglevel;
  return error;
}

static int
write_dwg (const char *file, Dwg_Data *dwg)
{
  int error;
  // dwg_write_file refuses to overwrite a file
  unlink (file);
  loglevel = tracelevel;
  error = dwg_write_file (file, dwg);
  loglevel = test_loglevel;
  return error;
}

static void
free_dwg (Dwg_Data *dwg)
{
  loglevel = tracelevel;
  dwg_free (dwg);
  loglevel = test_loglevel;
}

static void
free_document (Dwg_Data *dwg)
{
  free_dwg (dwg);
  free (dwg);
}

static Dwg_Data *
new_document (void)
{
  Dwg_Data *dwg = dwg_new_Document (R_2004, 0, tracelevel);
  loglevel = test_loglevel;
  return dwg;
}

static Dwg_Object *
object (const Dwg_Data *dwg, const BITCODE_RLL handle)
{
  return handle ? dwg_resolve_handle (dwg, handle) : NULL;
}

static BITCODE_RLL
ref (const Dwg_Object_Ref *r)
{
  return r ? r->absolute_ref : 0;
}

static BITCODE_RLL
handle_of (const Dwg_Data *dwg, const void *_obj)
{
  const Dwg_Object_Object *o = *(Dwg_Object_Object *const *)_obj;
  return dwg->object[o->objid].handle.value;
}

static Dwg_Object_BLOCK_HEADER *
block_header (const Dwg_Data *dwg, const BITCODE_RLL handle)
{
  Dwg_Object *obj = object (dwg, handle);
  return obj && obj->fixedtype == DWG_TYPE_BLOCK_HEADER
             ? obj->tio.object->tio.BLOCK_HEADER
             : NULL;
}

static Dwg_Object_BLOCK_HEADER *
model_space (Dwg_Data *dwg)
{
  return dwg_model_space_object (dwg)->tio.object->tio.BLOCK_HEADER;
}

static const Dwg_Object *
dict_item (const Dwg_Data *dwg, const Dwg_Object *dict, const char *name)
{
  const Dwg_Object_DICTIONARY *d;
  if (!dict || dict->fixedtype != DWG_TYPE_DICTIONARY)
    return NULL;
  d = dict->tio.object->tio.DICTIONARY;
  for (BITCODE_BL i = 0; d->texts && d->itemhandles && i < d->numitems; i++)
    if (d->texts[i] && !strcmp (d->texts[i], name))
      return object (dwg, ref (d->itemhandles[i]));
  return NULL;
}

static const Dwg_Object *
xdic (const Dwg_Data *dwg, const Dwg_Object *obj)
{
  return obj->tio.object->is_xdic_missing
             ? NULL
             : object (dwg, ref (obj->tio.object->xdicobjhandle));
}

static Dwg_Object_EVALUATION_GRAPH *
graph_of (const Dwg_Data *dwg, const BITCODE_RLL blk)
{
  const Dwg_Object *g
      = dict_item (dwg, xdic (dwg, object (dwg, blk)), "ACAD_ENHANCEDBLOCK");
  return g && g->fixedtype == DWG_TYPE_EVALUATION_GRAPH
             ? g->tio.object->tio.EVALUATION_GRAPH
             : NULL;
}

/* The element of node i of graph. */
static Dwg_Object *
node_object (const Dwg_Data *dwg, const Dwg_Object_EVALUATION_GRAPH *graph,
             const BITCODE_BL i)
{
  return object (dwg, ref (graph->nodes[i].evalexpr));
}

/* The 1070, 1071 and 1005 of obj's AcDbBlockRepETag EED group, or 0 when it
   has none. */
static int
etag (const Dwg_Data *dwg, const Dwg_Object *obj, BITCODE_RS *valuep,
      BITCODE_RL *indexp, BITCODE_RLL *handlep)
{
  const Dwg_Eed *eed = obj->supertype == DWG_SUPERTYPE_ENTITY
                           ? obj->tio.entity->eed
                           : obj->tio.object->eed;
  const BITCODE_BL num_eed = obj->supertype == DWG_SUPERTYPE_ENTITY
                                 ? obj->tio.entity->num_eed
                                 : obj->tio.object->num_eed;
  int in_group = 0, found = 0;
  *valuep = 0;
  *indexp = 0;
  *handlep = 0;
  for (BITCODE_BL i = 0; eed && i < num_eed; i++)
    {
      if (eed[i].size)
        {
          const Dwg_Object *appid = object (dwg, eed[i].handle.value);
          in_group = appid && appid->fixedtype == DWG_TYPE_APPID
                     && !strcmp (appid->tio.object->tio.APPID->name,
                                 "AcDbBlockRepETag");
          found |= in_group;
        }
      if (!in_group || !eed[i].data)
        continue;
      if (eed[i].data->code == 70)
        memcpy (valuep, &eed[i].data->u.eed_70.rs, sizeof (*valuep));
      else if (eed[i].data->code == 71)
        memcpy (indexp, &eed[i].data->u.eed_71.rl, sizeof (*indexp));
      else if (eed[i].data->code == 5)
        memcpy (handlep, &eed[i].data->u.eed_5.entity, sizeof (*handlep));
    }
  return found;
}

/* The number of AcDbBlockRepETag groups in obj's EED. */
static int
etag_groups (const Dwg_Data *dwg, const Dwg_Object *obj)
{
  const Dwg_Eed *eed = obj->tio.entity->eed;
  int n = 0;
  for (BITCODE_BL i = 0; eed && i < obj->tio.entity->num_eed; i++)
    if (eed[i].size)
      {
        const Dwg_Object *appid = object (dwg, eed[i].handle.value);
        n += appid && appid->fixedtype == DWG_TYPE_APPID
             && !strcmp (appid->tio.object->tio.APPID->name,
                         "AcDbBlockRepETag");
      }
  return n;
}

/* Appends n EED items to entity ent, each with zeroed data, the first
   starting a group of the APPID with handle appid that takes size bytes in
   a file. Returns the first, or NULL. */
static Dwg_Eed *
append_eed (Dwg_Object_Entity *ent, const BITCODE_RLL appid,
            const BITCODE_BS size, const BITCODE_BL n)
{
  const BITCODE_BL first = ent->num_eed;
  Dwg_Eed *eed
      = (Dwg_Eed *)realloc (ent->eed, (first + n) * sizeof (Dwg_Eed));
  if (!eed)
    return NULL;
  ent->eed = eed;
  memset (&eed[first], 0, n * sizeof (Dwg_Eed));
  for (BITCODE_BL i = 0; i < n; i++)
    {
      eed[first + i].data = (Dwg_Eed_Data *)calloc (1, sizeof (Dwg_Eed_Data));
      if (!eed[first + i].data)
        return NULL;
      ent->num_eed++;
    }
  eed[first].size = size;
  eed[first].handle.code = 5;
  for (BITCODE_RLL v = appid; v; v >>= 8)
    eed[first].handle.size++;
  eed[first].handle.value = appid;
  return &eed[first];
}

/* Gives entity ent the AcDbBlockRepETag EED AutoCAD gives the entities of
   a dynamic block definition: 1070 1, the index, and its 1005 handle. */
static int
add_etag (Dwg_Data *dwg, Dwg_Object_Entity *ent, const BITCODE_RLL handle,
          const BITCODE_RL index)
{
  BITCODE_H found = dwg_find_tablehandle (dwg, "AcDbBlockRepETag", "APPID");
  BITCODE_RLL appid = found ? found->absolute_ref : 0;
  Dwg_Eed *eed;
  if (!appid)
    {
      Dwg_Object_APPID *_appid = dwg_add_APPID (dwg, "AcDbBlockRepETag");
      if (!_appid)
        return 1;
      appid = handle_of (dwg, _appid);
    }
  eed = append_eed (ent, appid, 3 + 5 + 9, 3);
  if (!eed)
    return 1;
  eed[0].data->code = 70;
  eed[0].data->u.eed_70.rs = 1;
  eed[1].data->code = 71;
  // Dwg_Eed_Data is packed
  memcpy (&eed[1].data->u.eed_71.rl, &index, sizeof (index));
  eed[2].data->code = 5;
  memcpy (&eed[2].data->u.eed_5.entity, &handle, sizeof (handle));
  return 0;
}

/* The box: its top-left corner at the origin, 20 wide and 10 high, of four
   LINEs (top, right, bottom, left), and a circle and a caption, left on
   its baseline, centred. */
enum
{
  TOP,
  RIGHT,
  BOTTOM,
  LEFT,
  CIRCLE,
  CAPTION,
  BOX_ENTITIES
};

static int
draw_box (Dwg_Object_BLOCK_HEADER *blk, BITCODE_RLL *h)
{
  Dwg_Data *dwg = blk->parent->dwg;
  const dwg_point_3d p[4]
      = { { 0, 0, 0 }, { 20, 0, 0 }, { 20, -10, 0 }, { 0, -10, 0 } };
  const dwg_point_3d c = { 10, -5, 0 }, t = { 7, -9, 0 };
  Dwg_Entity_CIRCLE *circle;
  Dwg_Entity_TEXT *text;
  for (int i = 0; i < 4; i++)
    {
      Dwg_Entity_LINE *line = dwg_add_LINE (blk, &p[i], &p[(i + 1) % 4]);
      if (!line)
        return 1;
      h[i] = handle_of (dwg, line);
    }
  circle = dwg_add_CIRCLE (blk, &c, 2.0);
  text = dwg_add_TEXT (blk, "BOX", &t, 2.0);
  if (!circle || !text)
    return 1;
  text->width_factor = 1.0;
  h[CIRCLE] = handle_of (dwg, circle);
  h[CAPTION] = handle_of (dwg, text);
  loglevel = test_loglevel;
  return 0;
}

/* A new block named name in dwg holding the box. */
static BITCODE_RLL
add_box_block (Dwg_Data *dwg, const char *name, BITCODE_RLL *h)
{
  Dwg_Object_BLOCK_HEADER *blk = dwg_add_BLOCK_HEADER (dwg, name);
  if (!blk || !dwg_add_BLOCK (blk, name) || draw_box (blk, h)
      || !dwg_add_ENDBLK (blk))
    return 0;
  loglevel = test_loglevel;
  return handle_of (dwg, blk);
}

static const BITCODE_3BD box_base = { 0, 0, 0 }, box_end = { 20, 0, 0 };
static const BITCODE_2RD frame1 = { 15, 5 }, frame2 = { 25, -15 };

/* Gives block blk the box's Width, its stretch of the top, right and
   bottom LINEs, and its half move of the circle and caption, and more
   objects to move given. */
static Dwg_Object_BLOCKLINEARPARAMETER *
add_width (Dwg_Data *dwg, const BITCODE_RLL blk, const BITCODE_RLL *h,
           const BITCODE_RLL *more, const BITCODE_BL num_more,
           Dwg_Object_BLOCKSTRETCHACTION **stretchp,
           Dwg_Object_BLOCKMOVEACTION **movep, int *errorp)
{
  Dwg_Object_BLOCKLINEARPARAMETER *param;
  BITCODE_RLL moved[8];
  int error;
  *stretchp = NULL;
  *movep = NULL;
  param = dwg_dynblock_add_linear (block_header (dwg, blk), "Width",
                                   &box_base, &box_end, &error);
  *errorp = error;
  if (!param)
    return NULL;
  *stretchp = dwg_dynblock_add_stretch (block_header (dwg, blk), param,
                                        &frame1, &frame2, h, 3, &error);
  *errorp |= error;
  moved[0] = h[CIRCLE];
  moved[1] = h[CAPTION];
  for (BITCODE_BL i = 0; i < num_more && i < 6; i++)
    moved[2 + i] = more[i];
  *movep = dwg_dynblock_add_move (block_header (dwg, blk), param, moved,
                                  2 + num_more, 0.5, &error);
  *errorp |= error;
  loglevel = test_loglevel;
  return param;
}

static Dwg_DynValue
value (const char *name, const double v)
{
  Dwg_DynValue dv;
  memset (&dv, 0, sizeof (dv));
  dv.name = (char *)name;
  dv.value = v;
  return dv;
}

static int
near (const double a, const double b)
{
  return fabs (a - b) <= EPSILON;
}

static const Dwg_Object *
nth_entity (const Dwg_Data *dwg, const Dwg_Object_BLOCK_HEADER *blk,
            const BITCODE_BL i)
{
  return i < blk->num_owned ? object (dwg, ref (blk->entities[i])) : NULL;
}

/* Whether block rep holds the box at width w: its top, right and bottom
   LINEs stretched to w, its circle and caption moved by half the change. */
static int
is_box_at (const Dwg_Data *dwg, const Dwg_Object_BLOCK_HEADER *rep,
           const double w)
{
  const Dwg_Object *top = nth_entity (dwg, rep, TOP);
  const Dwg_Object *right = nth_entity (dwg, rep, RIGHT);
  const Dwg_Object *bottom = nth_entity (dwg, rep, BOTTOM);
  const Dwg_Object *left = nth_entity (dwg, rep, LEFT);
  const Dwg_Object *circle = nth_entity (dwg, rep, CIRCLE);
  const Dwg_Object *text = nth_entity (dwg, rep, CAPTION);
  const double half = (w - 20.0) / 2.0;
  if (!top || !right || !bottom || !left || !circle || !text
      || top->fixedtype != DWG_TYPE_LINE
      || circle->fixedtype != DWG_TYPE_CIRCLE
      || text->fixedtype != DWG_TYPE_TEXT)
    return 0;
  return near (top->tio.entity->tio.LINE->start.x, 0.0)
         && near (top->tio.entity->tio.LINE->end.x, w)
         && near (right->tio.entity->tio.LINE->start.x, w)
         && near (right->tio.entity->tio.LINE->end.x, w)
         && near (bottom->tio.entity->tio.LINE->start.x, w)
         && near (bottom->tio.entity->tio.LINE->end.x, 0.0)
         && near (left->tio.entity->tio.LINE->start.x, 0.0)
         && near (left->tio.entity->tio.LINE->end.x, 0.0)
         && near (circle->tio.entity->tio.CIRCLE->center.x, 10.0 + half)
         && near (circle->tio.entity->tio.CIRCLE->center.y, -5.0)
         && near (text->tio.entity->tio.TEXT->ins_pt.x, 7.0 + half)
         && near (text->tio.entity->tio.TEXT->ins_pt.y, -9.0);
}

/* Evaluates block blk at Width w, and checks the box. */
static int
evaluates_box (Dwg_Data *dwg, const BITCODE_RLL blk, const double w,
               const char *what)
{
  const Dwg_DynValue v = value ("Width", w);
  int error = 0, failures = 0;
  Dwg_Object_BLOCK_HEADER *rep
      = dwg_dynblock_evaluate (block_header (dwg, blk), &v, 1, &error);
  loglevel = test_loglevel;
  CHECK (rep && !error && is_box_at (dwg, rep, w),
         "%s: evaluated at Width %g, the right side at %g and the caption "
         "moved by half (error 0x%x)",
         what, w, w, error);
  return failures;
}

/*--------------------------------------------------------------------------------
 * The linear parameter
 */

/* The node edges of node i: first and last in, first and last out. */
static int
node_edges_are (const Dwg_EVAL_Node *node, const BITCODE_BLd first_in,
                const BITCODE_BLd last_in, const BITCODE_BLd first_out,
                const BITCODE_BLd last_out)
{
  return node->node[0] == first_in && node->node[1] == last_in
         && node->node[2] == first_out && node->node[3] == last_out;
}

/* Edge e: count values from from to to, with its previous and next edges
   in and out. */
static int
edge_is (const Dwg_EVAL_Edge *e, const BITCODE_BLd count,
         const BITCODE_BLd from, const BITCODE_BLd to,
         const BITCODE_BLd prev_in, const BITCODE_BLd next_in,
         const BITCODE_BLd prev_out, const BITCODE_BLd next_out)
{
  return e->nextid == 0 && e->e1 == count && e->e2 == from && e->e3 == to
         && e->out_edge[0] == prev_in && e->out_edge[1] == next_in
         && e->out_edge[2] == prev_out && e->out_edge[3] == next_out
         && e->out_edge[4] == -1;
}

/* The evalexpr and element versions AutoCAD 2027 writes in r2004, as the
   fixture's Window has them. */
static int
is_element (const Dwg_Object *obj, const BITCODE_BL nodeid)
{
  // every element starts with the evaluation expression, then for a block
  // element its name and versions
  const Dwg_Object_BLOCKLINEARGRIP *e
      = obj->tio.object->tio.BLOCKLINEARGRIP;
  return e->evalexpr.parentid == -1 && e->evalexpr.major == 33
         && e->evalexpr.minor == 427 && e->evalexpr.value_code == -9999
         && e->evalexpr.nodeid == nodeid
         && (obj->fixedtype == DWG_TYPE_BLOCKGRIPLOCATIONCOMPONENT
             || (e->be_major == 25 && e->be_minor == 104 && !e->eed1071));
}

static int
test_linear (void)
{
  Dwg_Data *dwg = new_document ();
  BITCODE_RLL h[BOX_ENTITIES], blk;
  Dwg_Object_BLOCKLINEARPARAMETER *param;
  Dwg_Object_EVALUATION_GRAPH *graph;
  Dwg_Object *obj, *grip, *x, *y, *pp;
  const Dwg_Object *dict;
  Dwg_DynParam *params = NULL;
  BITCODE_BL count = 0;
  int error = 0, failures = 0;

  blk = dwg ? add_box_block (dwg, "BOX", h) : 0;
  param = blk ? dwg_dynblock_add_linear (block_header (dwg, blk), "Width",
                                         &box_base, &box_end, &error)
              : NULL;
  loglevel = test_loglevel;
  if (!param || error)
    {
      FAIL ("linear: dwg_dynblock_add_linear 0x%x", blk ? error : 0);
      if (dwg)
        free_document (dwg);
      return 1;
    }
  OK ("linear: added to a block with no dynamic data");

  // the block's extension dictionary: the graph and the purge preventer
  dict = xdic (dwg, object (dwg, blk));
  graph = graph_of (dwg, blk);
  pp = (Dwg_Object *)dict_item (dwg, dict,
                                "AcDbDynamicBlockRoundTripPurgePreventer");
  CHECK (dict && dict->tio.object->tio.DICTIONARY->is_hardowner
             && dict->tio.object->tio.DICTIONARY->cloning == 1
             && dict->tio.object->tio.DICTIONARY->numitems == 2
             && !strcmp (dict->tio.object->tio.DICTIONARY->texts[0],
                         "ACAD_ENHANCEDBLOCK")
             && ref (dict->tio.object->ownerhandle) == blk,
         "linear: the block's extension dictionary, owned by it, holds "
         "ACAD_ENHANCEDBLOCK and the purge preventer, in that order");
  CHECK (graph && pp && pp->fixedtype == DWG_TYPE_DYNAMICBLOCKPURGEPREVENTER
             && pp->tio.object->tio.DYNAMICBLOCKPURGEPREVENTER->flag == 1
             && ref (pp->tio.object->tio.DYNAMICBLOCKPURGEPREVENTER->block)
                    == blk
             && ref (pp->tio.object->ownerhandle) == dict->handle.value
             && pp->tio.object->num_reactors == 1
             && ref (pp->tio.object->reactors[0]) == dict->handle.value,
         "linear: the purge preventer names the block, owned by the "
         "dictionary");
  if (!graph)
    {
      free_document (dwg);
      return failures + 1;
    }
  {
    const Dwg_Object *gobj = object (dwg, handle_of (dwg, graph));
    CHECK (ref (gobj->tio.object->ownerhandle) == dict->handle.value
               && gobj->tio.object->num_reactors == 1
               && ref (gobj->tio.object->reactors[0]) == dict->handle.value,
           "linear: the graph is owned by the dictionary, its reactor");
  }

  // the nodes: parameter, grip, X and Y location; ids from 1
  CHECK (graph->num_nodes == 4 && graph->first_nodeid == 4
             && graph->first_nodeid_copy == 4 && graph->num_edges == 3,
         "linear: 4 nodes, 3 edges, ids handed out to 4 (%u, %u, %d)",
         (unsigned)graph->num_nodes, (unsigned)graph->num_edges,
         (int)graph->first_nodeid);
  if (graph->num_nodes != 4 || graph->num_edges != 3)
    {
      free_document (dwg);
      return failures;
    }
  for (BITCODE_BL i = 0; i < 4; i++)
    CHECK (graph->nodes[i].id == i && graph->nodes[i].edge_flags == 32
               && graph->nodes[i].nextid == (BITCODE_BLd)(i + 1)
               && graph->nodes[i].evalexpr
               && graph->nodes[i].evalexpr->handleref.code == 3,
           "linear: node %u, id %u, a hard owner of its element", (unsigned)i,
           (unsigned)i + 1);
  obj = node_object (dwg, graph, 0);
  grip = node_object (dwg, graph, 1);
  x = node_object (dwg, graph, 2);
  y = node_object (dwg, graph, 3);
  CHECK (obj && obj->tio.object->tio.BLOCKLINEARPARAMETER == param && grip
             && grip->fixedtype == DWG_TYPE_BLOCKLINEARGRIP && x
             && x->fixedtype == DWG_TYPE_BLOCKGRIPLOCATIONCOMPONENT && y
             && y->fixedtype == DWG_TYPE_BLOCKGRIPLOCATIONCOMPONENT,
         "linear: the parameter, its grip, the grip's X and Y location");
  if (!obj || !grip || !x || !y)
    {
      free_document (dwg);
      return failures;
    }
  CHECK (node_edges_are (&graph->nodes[0], 2, 2, 0, 1)
             && node_edges_are (&graph->nodes[1], -1, -1, 2, 2)
             && node_edges_are (&graph->nodes[2], 0, 0, -1, -1)
             && node_edges_are (&graph->nodes[3], 1, 1, -1, -1),
         "linear: each node's first and last edges in and out");
  CHECK (edge_is (&graph->edges[0], 1, 0, 2, -1, -1, -1, 1)
             && edge_is (&graph->edges[1], 1, 0, 3, -1, -1, 0, -1)
             && edge_is (&graph->edges[2], 2, 1, 0, -1, -1, -1, -1),
         "linear: edges parameter to X and Y (1 value), grip to parameter "
         "(2), linked as AutoCAD links them");
  CHECK (is_element (obj, 1) && is_element (grip, 2) && is_element (x, 3)
             && is_element (y, 4),
         "linear: the elements' versions and node ids");
  for (BITCODE_BL i = 0; i < 4; i++)
    {
      const Dwg_Object *e = node_object (dwg, graph, i);
      CHECK (ref (e->tio.object->ownerhandle) == handle_of (dwg, graph)
                 && !e->tio.object->num_reactors
                 && e->tio.object->is_xdic_missing,
             "linear: node %u owned by the graph, no reactors", (unsigned)i);
    }

  // the parameter
  CHECK (!strcmp (param->name, "Linear")
             && !strcmp (param->distance_name, "Width")
             && !strcmp (param->distance_desc, "") && param->distance == 0.0
             && param->show_properties && !param->chain_actions
             && param->def_basept.x == 0.0 && param->def_endpt.x == 20.0
             && !param->parameter_base_location,
         "linear: the parameter 'Linear', property Width, from (0,0) to "
         "(20,0)");
  CHECK (!param->prop1.num_connections && !param->prop2.num_connections
             && param->prop3.num_connections == 1
             && param->prop3.connections[0].code == 2
             && !strcmp (param->prop3.connections[0].name, "DisplacementX")
             && param->prop4.num_connections == 1
             && param->prop4.connections[0].code == 2
             && !strcmp (param->prop4.connections[0].name, "DisplacementY")
             && param->prop_states && !param->prop_states[0]
             && param->prop_states[1] == 2 && !param->prop_states[2]
             && !param->prop_states[3],
         "linear: the parameter takes the end grip's X and Y displacement");
  CHECK (!param->value_set.flags && !param->value_set.num_valuelist
             && !param->value_set.minimum && !param->value_set.maximum
             && !param->value_set.increment,
         "linear: no value set");
  {
    const Dwg_Object_BLOCKLINEARGRIP *g
        = grip->tio.object->tio.BLOCKLINEARGRIP;
    const Dwg_Object_BLOCKGRIPLOCATIONCOMPONENT *gx
        = x->tio.object->tio.BLOCKGRIPLOCATIONCOMPONENT;
    const Dwg_Object_BLOCKGRIPLOCATIONCOMPONENT *gy
        = y->tio.object->tio.BLOCKGRIPLOCATIONCOMPONENT;
    CHECK (!strcmp (g->name, "End Grip") && g->bg_bl91 == 3
               && g->bg_bl92 == 4 && g->bg_location.x == 20.0
               && g->bg_location.y == 0.0 && !g->bg_insert_cycling
               && g->bg_insert_cycling_weight == -1
               && g->orientation.x == 20.0 && g->orientation.y == 0.0,
           "linear: the end grip at (20,0), located by nodes 3 and 4");
    CHECK (gx->grip_type == 1 && !strcmp (gx->grip_expr, "UpdatedEndX")
               && gy->grip_type == 1
               && !strcmp (gy->grip_expr, "UpdatedEndY"),
           "linear: the grip's location is node 1's updated end point");
  }

  // the entities are numbered, and the block keeps the next number
  {
    const Dwg_Object_BLOCK_HEADER *b = block_header (dwg, blk);
    int numbered = 1;
    BITCODE_RS v;
    BITCODE_RL index;
    BITCODE_RLL hv;
    for (BITCODE_BL i = 0; i < b->num_owned; i++)
      {
        const Dwg_Object *e = nth_entity (dwg, b, i);
        if (!etag (dwg, e, &v, &index, &hv) || v != 1 || index != i
            || hv != e->handle.value)
          numbered = 0;
      }
    CHECK (numbered && b->num_owned == BOX_ENTITIES,
           "linear: the entities numbered 0 to 5, with their own handles");
    CHECK (etag (dwg, object (dwg, blk), &v, &index, &hv) && v == 1
               && index == BOX_ENTITIES && !hv,
           "linear: the block's counter is 6 (%u)", (unsigned)index);
  }

  // the property
  error = dwg_dynblock_get_params (block_header (dwg, blk), &params, &count);
  CHECK (!error && count == 1 && !strcmp (params[0].name, "Width")
             && params[0].kind == DWG_DYNPARAM_LINEAR
             && params[0].default_value == 20.0 && !params[0].value_flags,
         "linear: the property Width, 20 long");
  dwg_dynblock_free_params (params, count);

  // a second one, named on
  {
    const BITCODE_3BD b2 = { 0, -10, 0 }, e2 = { 0, -20, 0 };
    Dwg_Object_BLOCKLINEARPARAMETER *p2 = dwg_dynblock_add_linear (
        block_header (dwg, blk), "Height", &b2, &e2, &error);
    loglevel = test_loglevel;
    graph = graph_of (dwg, blk);
    CHECK (p2 && !error && !strcmp (p2->name, "Linear1")
               && graph->num_nodes == 8 && graph->first_nodeid == 8
               && graph->nodes[4].nextid == 5 && graph->num_edges == 6
               && edge_is (&graph->edges[5], 2, 5, 4, -1, -1, -1, -1),
           "linear: a second parameter, 'Linear1', nodes 5 to 8");
  }
  free_document (dwg);
  return failures;
}

/* The entity numbers: after the highest, gaps kept; none left; none for a
   block with no entities. */
static int
test_numbering (void)
{
  Dwg_Data *dwg = new_document ();
  BITCODE_RLL h[BOX_ENTITIES], blk;
  Dwg_Object_BLOCK_HEADER *empty;
  BITCODE_RS v;
  BITCODE_RL index;
  BITCODE_RLL hv;
  int error = 0, failures = 0;
  blk = dwg ? add_box_block (dwg, "BOX", h) : 0;
  if (!blk)
    {
      FAIL ("numbering: building the block failed");
      if (dwg)
        free_document (dwg);
      return 1;
    }
  // AutoCAD's numbers, with gaps: the others follow the highest
  if (add_etag (dwg, object (dwg, h[TOP])->tio.entity, h[TOP], 0)
      || add_etag (dwg, object (dwg, h[RIGHT])->tio.entity, h[RIGHT], 5))
    {
      FAIL ("numbering: adding the EED failed");
      free_document (dwg);
      return 1;
    }
  dwg_dynblock_add_linear (block_header (dwg, blk), "Width", &box_base,
                           &box_end, &error);
  loglevel = test_loglevel;
  {
    int kept = etag (dwg, object (dwg, h[TOP]), &v, &index, &hv) && !index
               && etag (dwg, object (dwg, h[RIGHT]), &v, &index, &hv)
               && index == 5;
    int after = etag (dwg, object (dwg, h[BOTTOM]), &v, &index, &hv)
                && index == 6
                && etag (dwg, object (dwg, h[CAPTION]), &v, &index, &hv)
                && index == 9;
    int counter = etag (dwg, object (dwg, blk), &v, &index, &hv)
                  && index == 10;
    CHECK (!error && kept && after && counter,
           "numbering: 0 and 5 kept, the others 6 to 9, the counter 10");
  }
  // an index at the end of a 1071 leaves none for a new entity
  {
    const dwg_point_3d a = { 1, 1, 0 }, b = { 2, 2, 0 };
    Dwg_Object_BLOCK_HEADER *full = dwg_add_BLOCK_HEADER (dwg, "FULL");
    Dwg_Entity_LINE *l1 = NULL, *l2 = NULL;
    BITCODE_RLL fh = 0, h1 = 0, h2 = 0;
    if (full && dwg_add_BLOCK (full, "FULL"))
      {
        l1 = dwg_add_LINE (full, &a, &b);
        l2 = dwg_add_LINE (full, &b, &a);
        dwg_add_ENDBLK (full);
      }
    loglevel = test_loglevel;
    if (l1 && l2)
      {
        fh = handle_of (dwg, full);
        h1 = handle_of (dwg, l1);
        h2 = handle_of (dwg, l2);
      }
    if (!fh || add_etag (dwg, object (dwg, h1)->tio.entity, h1, 0x7FFFFFFE))
      {
        FAIL ("numbering: building FULL failed");
        failures++;
      }
    else
      {
        Dwg_Object_BLOCKLINEARPARAMETER *p;
        error = 0;
        loglevel = 0;
        p = dwg_dynblock_add_linear (block_header (dwg, fh), "Width",
                                     &box_base, &box_end, &error);
        loglevel = test_loglevel;
        CHECK (p && error == DWG_ERR_INVALIDEED
                   && !etag (dwg, object (dwg, h2), &v, &index, &hv)
                   && !etag (dwg, object (dwg, fh), &v, &index, &hv),
               "numbering: no index left, the LINE stays unnumbered, the "
               "block has no counter (0x%x)",
               error);
      }
  }
  // a block with no entities gets no counter
  empty = dwg_add_BLOCK_HEADER (dwg, "EMPTY");
  if (empty && dwg_add_BLOCK (empty, "EMPTY") && dwg_add_ENDBLK (empty))
    {
      const BITCODE_RLL eh = handle_of (dwg, empty);
      loglevel = test_loglevel;
      error = 0;
      CHECK (dwg_dynblock_add_linear (block_header (dwg, eh), "Width",
                                      &box_base, &box_end, &error)
                 && !error && !etag (dwg, object (dwg, eh), &v, &index, &hv),
             "numbering: a block with no entities has no counter (0x%x)",
             error);
    }
  else
    {
      FAIL ("numbering: adding EMPTY failed");
      failures++;
    }
  free_document (dwg);
  return failures;
}

/* Model space: as AutoCAD writes a dynamic block to a file of its own,
   with no purge preventer; an extension dictionary it has already gets the
   graph. */
static int
test_linear_model_space (void)
{
  Dwg_Data *dwg = new_document ();
  BITCODE_RLL h[BOX_ENTITIES], ms;
  Dwg_Object_BLOCKLINEARPARAMETER *param;
  Dwg_Object_DICTIONARY *dict;
  const Dwg_Object *xd;
  int error = 0, failures = 0;
  if (!dwg || draw_box (model_space (dwg), h))
    {
      FAIL ("model space: drawing the box failed");
      if (dwg)
        free_document (dwg);
      return 1;
    }
  ms = dwg_model_space_object (dwg)->handle.value;
  // an extension dictionary with an entry already
  dict = dwg_add_DICTIONARY (dwg, NULL, NULL, 0);
  {
    Dwg_Object *msobj = dwg_model_space_object (dwg);
    Dwg_Object *dobj = dwg_obj_generic_to_object (dict, &error);
    msobj->tio.object->xdicobjhandle
        = dwg_add_handleref (dwg, 3, dobj->handle.value, NULL);
    msobj->tio.object->is_xdic_missing = 0;
    dobj->tio.object->ownerhandle = dwg_add_handleref (dwg, 4, ms, dobj);
    dwg_add_DICTIONARY_item (dict, "AAA", ms);
    dwg_add_DICTIONARY_item (dict, "ZZZ", ms);
  }
  param = dwg_dynblock_add_linear (model_space (dwg), "Width", &box_base,
                                   &box_end, &error);
  loglevel = test_loglevel;
  xd = xdic (dwg, object (dwg, ms));
  dict = xd ? xd->tio.object->tio.DICTIONARY : NULL;
  CHECK (param && !error && dict && dict->numitems == 3
             && !strcmp (dict->texts[0], "AAA")
             && !strcmp (dict->texts[1], "ACAD_ENHANCEDBLOCK")
             && !strcmp (dict->texts[2], "ZZZ")
             && graph_of (dwg, ms)
             && !dict_item (dwg, xd,
                            "AcDbDynamicBlockRoundTripPurgePreventer"),
         "model space: the graph joins its extension dictionary, in name "
         "order, with no purge preventer (error 0x%x)",
         error);
  failures += evaluates_box (dwg, ms, 20.0, "model space");
  free_document (dwg);
  return failures;
}

static int
test_linear_refusals (Dwg_Data *src)
{
  Dwg_Data *dwg = new_document ();
  Dwg_Data *r2000 = dwg_new_Document (R_2000, 0, tracelevel);
  BITCODE_RLL h[BOX_ENTITIES], blk;
  const BITCODE_3BD up = { 0, 0, 1 }, nan_pt = { (double)NAN, 0, 0 };
  Dwg_Object_BLOCK_HEADER copy;
  BITCODE_BL num_objects;
  int error = 0, failures = 0;
  loglevel = test_loglevel;
  blk = dwg ? add_box_block (dwg, "BOX", h) : 0;
  if (!blk || !r2000)
    {
      FAIL ("linear refusals: building the block failed");
      if (dwg)
        free_document (dwg);
      if (r2000)
        free_document (r2000);
      return 1;
    }
  dwg_dynblock_add_linear (block_header (dwg, blk), "Width", &box_base,
                           &box_end, &error);
  num_objects = dwg->num_objects;
  loglevel = 0;

#define REFUSES(call, bits, what)                                             \
  do                                                                          \
    {                                                                         \
      error = 0;                                                              \
      loglevel = 0;                                                           \
      CHECK (!(call) && error == (bits) && dwg->num_objects == num_objects,   \
             "linear refusals: %s (0x%x)", what, error);                      \
    }                                                                         \
  while (0)

  REFUSES (dwg_dynblock_add_linear (NULL, "W", &box_base, &box_end, &error),
           REFUSED (DWG_ERR_INTERNALERROR), "no block");
  REFUSES (dwg_dynblock_add_linear (block_header (dwg, blk), NULL, &box_base,
                                    &box_end, &error),
           REFUSED (DWG_ERR_INTERNALERROR), "no name");
  REFUSES (dwg_dynblock_add_linear (block_header (dwg, blk), "", &box_base,
                                    &box_end, &error),
           REFUSED (DWG_ERR_VALUEOUTOFBOUNDS), "an empty name");
  REFUSES (dwg_dynblock_add_linear (block_header (dwg, blk), "WIDTH",
                                    &box_base, &box_end, &error),
           REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
           "a name the block has, in other case");
  REFUSES (dwg_dynblock_add_linear (block_header (dwg, blk), "Other",
                                    &box_base, &box_base, &error),
           REFUSED (DWG_ERR_VALUEOUTOFBOUNDS), "one point");
  REFUSES (dwg_dynblock_add_linear (block_header (dwg, blk), "Other",
                                    &box_base, &up, &error),
           REFUSED (DWG_ERR_VALUEOUTOFBOUNDS), "points at different heights");
  REFUSES (dwg_dynblock_add_linear (block_header (dwg, blk), "Other",
                                    &nan_pt, &box_end, &error),
           REFUSED (DWG_ERR_VALUEOUTOFBOUNDS), "a point that isn't finite");
  REFUSES (dwg_dynblock_add_linear (
               dwg_paper_space_object (dwg)->tio.object->tio.BLOCK_HEADER,
               "Other", &box_base, &box_end, &error),
           REFUSED (DWG_ERR_INVALIDTYPE), "paper space");
  // a copy of the block's struct is not the block
  copy = *block_header (dwg, blk);
  REFUSES (dwg_dynblock_add_linear (&copy, "Other", &box_base, &box_end,
                                    &error),
           REFUSED (DWG_ERR_INVALIDHANDLE), "a copy of the block's struct");
  {
    const BITCODE_BL n2000 = r2000->num_objects;
    error = 0;
    loglevel = 0;
    CHECK (!dwg_dynblock_add_linear (model_space (r2000), "Other", &box_base,
                                     &box_end, &error)
               && error == REFUSED (DWG_ERR_NOTYETSUPPORTED)
               && r2000->num_objects == n2000,
           "linear refusals: an r2000 drawing (0x%x)", error);
  }
  {
    // the fixture's constraint-based definition, in its own document
    const BITCODE_BL nsrc = src->num_objects;
    error = 0;
    loglevel = 0;
    CHECK (!dwg_dynblock_add_linear (block_header (src, DOOR_ELEVATION),
                                     "Other", &box_base, &box_end, &error)
               && error == REFUSED (DWG_ERR_NOTYETSUPPORTED)
               && src->num_objects == nsrc,
           "linear refusals: a constraint-based definition (0x%x)", error);
  }
  loglevel = test_loglevel;
  free_document (dwg);
  free_document (r2000);
  return failures;
}

/* Constraint-based (parametric) blocks, which dwg_dynblock_evaluate ()
   refuses, are refused with nothing added: the fixture's *U31, whose
   constraints are in its ACAD_ASSOCNETWORK and which has no graph; and a
   block whose graph has a constraint or a user parameter node, but no
   proxy node, by all three functions. */
static int
test_constraint_based (Dwg_Data *src)
{
  Dwg_Data *dwg;
  BITCODE_RLL h[BOX_ENTITIES], blk = 0, spare_h;
  Dwg_Object_BLOCKLINEARPARAMETER *param = NULL, *spare = NULL;
  Dwg_Object_BLOCKSTRETCHACTION *s;
  Dwg_Object_BLOCKMOVEACTION *m;
  const BITCODE_3BD sb = { 0, -20, 0 }, se = { 5, -20, 0 };
  const struct
  {
    Dwg_Object_Type type;
    const char *name;
    int (*setup) (Dwg_Object *);
  } kinds[2] = {
    { DWG_TYPE_BLOCKLINEARCONSTRAINTPARAMETER,
      "BLOCKLINEARCONSTRAINTPARAMETER",
      dwg_setup_BLOCKLINEARCONSTRAINTPARAMETER },
    { DWG_TYPE_BLOCKUSERPARAMETER, "BLOCKUSERPARAMETER",
      dwg_setup_BLOCKUSERPARAMETER },
  };
  int error = 0, failures = 0;

  {
    const Dwg_Object *u31 = object (src, U31);
    const BITCODE_BL nsrc = src->num_objects;
    CHECK (u31 && u31->fixedtype == DWG_TYPE_BLOCK_HEADER
               && dict_item (src, xdic (src, u31), "ACAD_ASSOCNETWORK")
               && !graph_of (src, U31),
           "constraint-based: *U31 has an ACAD_ASSOCNETWORK and no graph");
    error = 0;
    loglevel = 0;
    CHECK (!dwg_dynblock_add_linear (block_header (src, U31), "Other",
                                     &box_base, &box_end, &error)
               && error == REFUSED (DWG_ERR_NOTYETSUPPORTED)
               && src->num_objects == nsrc && !graph_of (src, U31),
           "constraint-based: *U31 refused, with no graph added (0x%x)",
           error);
    loglevel = test_loglevel;
  }

  dwg = new_document ();
  blk = dwg ? add_box_block (dwg, "BOX", h) : 0;
  param = blk ? add_width (dwg, blk, h, NULL, 0, &s, &m, &error) : NULL;
  if (param)
    spare = dwg_dynblock_add_linear (block_header (dwg, blk), "Spare", &sb,
                                     &se, &error);
  loglevel = test_loglevel;
  if (!spare)
    {
      FAIL ("constraint-based: building the block failed (0x%x)", error);
      if (dwg)
        free_document (dwg);
      return failures + 1;
    }
  spare_h = handle_of (dwg, spare);
  for (int k = 0; k < 2; k++)
    {
      Dwg_Object *obj = object (dwg, spare_h);
      Dwg_Object_Object *saved = obj->tio.object;
      const Dwg_Object_Type saved_type = obj->fixedtype;
      char *saved_name = obj->name, *saved_dxfname = obj->dxfname;
      const BITCODE_BL num_objects = dwg->num_objects;
      // Spare's node, as the reader leaves a node of that type: its type,
      // with a struct of it
      obj->fixedtype = kinds[k].type;
      obj->name = obj->dxfname = (char *)kinds[k].name;
      if (kinds[k].setup (obj))
        {
          obj->tio.object = saved;
          obj->fixedtype = saved_type;
          obj->name = saved_name;
          obj->dxfname = saved_dxfname;
          FAIL ("constraint-based: setting up a %s failed", kinds[k].name);
          failures++;
          continue;
        }
      error = 0;
      loglevel = 0;
      CHECK (!dwg_dynblock_add_linear (block_header (dwg, blk), "Other",
                                       &box_base, &box_end, &error)
                 && error == REFUSED (DWG_ERR_NOTYETSUPPORTED)
                 && dwg->num_objects == num_objects,
             "constraint-based: a %s node, the linear refused (0x%x)",
             kinds[k].name, error);
      error = 0;
      loglevel = 0;
      CHECK (!dwg_dynblock_add_stretch (block_header (dwg, blk), param,
                                        &frame1, &frame2, h, 3, &error)
                 && error == REFUSED (DWG_ERR_NOTYETSUPPORTED)
                 && dwg->num_objects == num_objects,
             "constraint-based: a %s node, the stretch refused (0x%x)",
             kinds[k].name, error);
      error = 0;
      loglevel = 0;
      CHECK (!dwg_dynblock_add_move (block_header (dwg, blk), param,
                                     &h[CIRCLE], 1, 0.5, &error)
                 && error == REFUSED (DWG_ERR_NOTYETSUPPORTED)
                 && dwg->num_objects == num_objects,
             "constraint-based: a %s node, the move refused (0x%x)",
             kinds[k].name, error);
      loglevel = test_loglevel;
      // what the calls added, had they not refused, may have moved it
      obj = object (dwg, spare_h);
      free (obj->tio.object->tio.BLOCKUSERPARAMETER);
      free (obj->tio.object);
      // the setup copies the names in a document of the add API
      if ((const char *)obj->name != kinds[k].name)
        free (obj->name);
      if ((const char *)obj->dxfname != kinds[k].name)
        free (obj->dxfname);
      obj->tio.object = saved;
      obj->fixedtype = saved_type;
      obj->name = saved_name;
      obj->dxfname = saved_dxfname;
    }
  // Spare a linear parameter again: the block takes another
  error = 0;
  CHECK (dwg_dynblock_add_linear (block_header (dwg, blk), "Other", &box_base,
                                  &box_end, &error)
             && !error,
         "constraint-based: with Spare linear again, another added (0x%x)",
         error);
  loglevel = test_loglevel;
  free_document (dwg);
  return failures;
}

/*--------------------------------------------------------------------------------
 * The stretch and the move
 */

static int
test_actions (void)
{
  Dwg_Data *dwg = new_document ();
  BITCODE_RLL h[BOX_ENTITIES], blk;
  Dwg_Object_BLOCKLINEARPARAMETER *param;
  Dwg_Object_BLOCKSTRETCHACTION *stretch;
  Dwg_Object_BLOCKMOVEACTION *move;
  Dwg_Object_EVALUATION_GRAPH *graph;
  int error = 0, failures = 0;
  blk = dwg ? add_box_block (dwg, "BOX", h) : 0;
  param = blk ? add_width (dwg, blk, h, NULL, 0, &stretch, &move, &error)
              : NULL;
  if (!param || !stretch || !move || error)
    {
      FAIL ("actions: adding the Width failed (0x%x)", blk ? error : 0);
      if (dwg)
        free_document (dwg);
      return 1;
    }
  OK ("actions: a stretch and a move of the Width");
  graph = graph_of (dwg, blk);
  CHECK (graph->num_nodes == 6 && graph->first_nodeid == 6
             && graph->num_edges == 5
             && node_object (dwg, graph, 4)->tio.object->tio.BLOCKSTRETCHACTION
                    == stretch
             && node_object (dwg, graph, 5)->tio.object->tio.BLOCKMOVEACTION
                    == move
             && is_element (node_object (dwg, graph, 4), 5)
             && is_element (node_object (dwg, graph, 5), 6),
         "actions: nodes 5 and 6");
  CHECK (edge_is (&graph->edges[1], 1, 0, 3, -1, -1, 0, 3)
             && edge_is (&graph->edges[3], 2, 0, 4, -1, -1, 1, 4)
             && edge_is (&graph->edges[4], 2, 0, 5, -1, -1, 3, -1)
             && node_edges_are (&graph->nodes[0], 2, 2, 0, 4)
             && node_edges_are (&graph->nodes[4], 3, 3, -1, -1)
             && node_edges_are (&graph->nodes[5], 4, 4, -1, -1),
         "actions: an edge from the parameter to each (2 values), last out "
         "of the parameter");

  // the stretch, as the fixture's Window's 'Stretch Width'
  CHECK (!strcmp (stretch->name, "Stretch")
             && stretch->conn_pts[0].code == 1
             && !strcmp (stretch->conn_pts[0].name, "EndXDelta")
             && stretch->conn_pts[1].code == 1
             && !strcmp (stretch->conn_pts[1].name, "EndYDelta")
             && stretch->action_offset_x == 1.0
             && stretch->action_offset_y == 0.0 && !stretch->xy_type
             && !stretch->num_actions && !stretch->num_codes,
         "actions: the stretch takes the Width's end point deltas, times 1");
  CHECK (stretch->num_pts == 2 && stretch->pts[0].x == 25.0
             && stretch->pts[0].y == 5.0 && stretch->pts[1].x == 15.0
             && stretch->pts[1].y == -15.0
             && stretch->display_location.x == 20.0
             && stretch->display_location.y == -5.0,
         "actions: the frame from its upper right corner, the icon at its "
         "centre");
  CHECK (stretch->num_deps == 3 && ref (stretch->deps[0]) == h[TOP]
             && ref (stretch->deps[1]) == h[RIGHT]
             && ref (stretch->deps[2]) == h[BOTTOM] && stretch->num_hdls == 3
             && ref (stretch->hdls[0].hdl) == h[TOP]
             && stretch->hdls[0].num_indexes == 1
             && stretch->hdls[0].indexes[0] == 1
             && ref (stretch->hdls[1].hdl) == h[RIGHT]
             && stretch->hdls[1].num_indexes == 2
             && stretch->hdls[1].indexes[0] == 0
             && stretch->hdls[1].indexes[1] == 1
             && ref (stretch->hdls[2].hdl) == h[BOTTOM]
             && stretch->hdls[2].num_indexes == 1
             && stretch->hdls[2].indexes[0] == 0,
         "actions: the stretch lists the LINE ends in its frame");
  CHECK (!strcmp (move->name, "Move") && move->conn_pts[0].code == 1
             && !strcmp (move->conn_pts[0].name, "EndXDelta")
             && move->conn_pts[1].code == 1
             && !strcmp (move->conn_pts[1].name, "EndYDelta")
             && move->action_offset_x == 0.5 && move->action_offset_y == 0.0
             && !move->xy_type && !move->num_actions && move->num_deps == 2
             && ref (move->deps[0]) == h[CIRCLE]
             && ref (move->deps[1]) == h[CAPTION]
             && move->display_location.x == 20.0,
         "actions: the move of the circle and caption, times 0.5");

  failures += evaluates_box (dwg, blk, 30.0, "actions");
  failures += evaluates_box (dwg, blk, 12.5, "actions");

  // an entity added since is numbered by the next action
  {
    const dwg_point_3d a = { 20, -2, 0 }, b = { 18, -2, 0 };
    Dwg_Entity_LINE *tick = dwg_add_LINE (block_header (dwg, blk), &a, &b);
    BITCODE_RLL th = tick ? handle_of (dwg, tick) : 0;
    BITCODE_RS v;
    BITCODE_RL index;
    BITCODE_RLL hv;
    loglevel = test_loglevel;
    CHECK (th
               && dwg_dynblock_add_stretch (block_header (dwg, blk), param,
                                            &frame1, &frame2, &th, 1, &error)
               && !error && etag (dwg, object (dwg, th), &v, &index, &hv)
               && index == BOX_ENTITIES && hv == th
               && etag (dwg, object (dwg, blk), &v, &index, &hv)
               && index == BOX_ENTITIES + 1,
           "actions: a LINE added since is numbered 6, the counter 7");
    // all of it in the frame: it moves whole
    {
      const Dwg_DynValue dv = value ("Width", 30.0);
      Dwg_Object_BLOCK_HEADER *rep
          = dwg_dynblock_evaluate (block_header (dwg, blk), &dv, 1, &error);
      const Dwg_Object *t = rep ? nth_entity (dwg, rep, BOX_ENTITIES) : NULL;
      loglevel = test_loglevel;
      CHECK (t && t->fixedtype == DWG_TYPE_LINE
                 && near (t->tio.entity->tio.LINE->start.x, 30.0)
                 && near (t->tio.entity->tio.LINE->end.x, 28.0),
             "actions: a LINE wholly in the frame moves whole");
    }
  }
  free_document (dwg);
  return failures;
}

/* A block of the shapes that are stretched or refused: an ARC, a centred
   TEXT, an LWPOLYLINE turned over, a CIRCLE, an INSERT and a POINT. */
static int
test_action_refusals (Dwg_Data *src)
{
  Dwg_Data *dwg = new_document ();
  Dwg_Data *other = new_document ();
  BITCODE_RLL h[BOX_ENTITIES], blk, oblk, oh[BOX_ENTITIES];
  BITCODE_RLL arc, text, lwpl, point, ms_line, grip;
  Dwg_Object_BLOCKLINEARPARAMETER *param, *oparam;
  Dwg_Object_BLOCKSTRETCHACTION *s;
  Dwg_Object_BLOCKMOVEACTION *m;
  BITCODE_BL num_objects;
  int error = 0, failures = 0;
  blk = dwg ? add_box_block (dwg, "BOX", h) : 0;
  oblk = other ? add_box_block (other, "BOX", oh) : 0;
  param = blk ? add_width (dwg, blk, h, NULL, 0, &s, &m, &error) : NULL;
  oparam = oblk ? add_width (other, oblk, oh, NULL, 0, &s, &m, &error) : NULL;
  if (!param || !oparam)
    {
      FAIL ("action refusals: building the blocks failed");
      if (dwg)
        free_document (dwg);
      if (other)
        free_document (other);
      return 1;
    }
  {
    Dwg_Object_BLOCK_HEADER *b = block_header (dwg, blk);
    const dwg_point_3d c = { 20, -5, 0 }, p = { 20, -6, 0 },
                       far = { 22, -5, 0 };
    const dwg_point_2d pts[2] = { { 18, -3 }, { 22, -3 } };
    Dwg_Entity_ARC *a = dwg_add_ARC (b, &c, 1.0, 0.0, M_PI);
    Dwg_Entity_TEXT *t = dwg_add_TEXT (b, "C", &c, 1.0);
    Dwg_Entity_LWPOLYLINE *l = dwg_add_LWPOLYLINE (b, 2, pts);
    Dwg_Entity_POINT *pt = dwg_add_POINT (b, &p);
    // in model space, of the same document
    Dwg_Entity_LINE *o = dwg_add_LINE (model_space (dwg), &c, &far);
    if (!a || !t || !l || !pt || !o)
      {
        FAIL ("action refusals: adding the entities failed");
        free_document (dwg);
        free_document (other);
        return 1;
      }
    arc = handle_of (dwg, a);
    t->horiz_alignment = 1;
    t->alignment_pt.x = 20;
    t->alignment_pt.y = -5;
    text = handle_of (dwg, t);
    l->extrusion.z = -1.0;
    lwpl = handle_of (dwg, l);
    point = handle_of (dwg, pt);
    ms_line = handle_of (dwg, o);
    grip = node_object (dwg, graph_of (dwg, blk), 1)->handle.value;
  }
  num_objects = dwg->num_objects;
  loglevel = 0;

#define STRETCH_REFUSES(p, c1, c2, ents, n, bits, what)                       \
  do                                                                          \
    {                                                                         \
      error = 0;                                                              \
      loglevel = 0;                                                           \
      CHECK (!dwg_dynblock_add_stretch (block_header (dwg, blk), p, c1, c2,   \
                                        ents, n, &error)                      \
                 && error == (bits) && dwg->num_objects == num_objects,       \
             "stretch refusals: %s (0x%x)", what, error);                     \
    }                                                                         \
  while (0)
#define MOVE_REFUSES(p, objs, n, mult, bits, what)                            \
  do                                                                          \
    {                                                                         \
      error = 0;                                                              \
      loglevel = 0;                                                           \
      CHECK (!dwg_dynblock_add_move (block_header (dwg, blk), p, objs, n,     \
                                     mult, &error)                            \
                 && error == (bits) && dwg->num_objects == num_objects,       \
             "move refusals: %s (0x%x)", what, error);                        \
    }                                                                         \
  while (0)

  {
    const BITCODE_2RD flat = { 25, 5 }, inf = { (double)INFINITY, 0 };
    BITCODE_RLL twice[2] = { h[TOP], h[TOP] };
    STRETCH_REFUSES (param, &frame1, &frame2, NULL, 1,
                     REFUSED (DWG_ERR_INTERNALERROR), "no entities");
    STRETCH_REFUSES (param, &frame1, &frame2, h, 0,
                     REFUSED (DWG_ERR_VALUEOUTOFBOUNDS), "none selected");
    STRETCH_REFUSES (param, &frame1, &flat, h, 1,
                     REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                     "corners sharing a y");
    STRETCH_REFUSES (param, &frame1, &inf, h, 1,
                     REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                     "a corner that isn't finite");
    STRETCH_REFUSES (param, &frame1, &frame2, twice, 2,
                     REFUSED (DWG_ERR_VALUEOUTOFBOUNDS), "an entity twice");
    STRETCH_REFUSES (oparam, &frame1, &frame2, h, 1,
                     REFUSED (DWG_ERR_INVALIDHANDLE),
                     "a parameter of another document");
    STRETCH_REFUSES (param, &frame1, &frame2, &ms_line, 1,
                     REFUSED (DWG_ERR_INVALIDHANDLE),
                     "an entity of model space");
    STRETCH_REFUSES (param, &frame1, &frame2, &grip, 1,
                     REFUSED (DWG_ERR_INVALIDHANDLE), "a grip");
    STRETCH_REFUSES (param, &frame1, &frame2, &h[LEFT], 1,
                     REFUSED (DWG_ERR_NOTYETSUPPORTED),
                     "an entity with no point in the frame");
    STRETCH_REFUSES (param, &frame1, &frame2, &arc, 1,
                     REFUSED (DWG_ERR_NOTYETSUPPORTED),
                     "an ARC, whose stretch points aren't placed");
    STRETCH_REFUSES (param, &frame1, &frame2, &text, 1,
                     REFUSED (DWG_ERR_NOTYETSUPPORTED), "a centred TEXT");
    STRETCH_REFUSES (param, &frame1, &frame2, &lwpl, 1,
                     REFUSED (DWG_ERR_NOTYETSUPPORTED),
                     "an LWPOLYLINE turned over");
    STRETCH_REFUSES (param, &frame1, &frame2, &point, 1,
                     REFUSED (DWG_ERR_NOTYETSUPPORTED), "a POINT");
  }
  {
    BITCODE_RLL own = handle_of (dwg, param);
    BITCODE_RLL twice[2] = { h[CIRCLE], h[CIRCLE] };
    MOVE_REFUSES (param, h, 1, (double)NAN, REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                  "a multiplier that isn't finite");
    MOVE_REFUSES (param, h, 0, 0.5, REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                  "none selected");
    MOVE_REFUSES (param, twice, 2, 0.5, REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                  "an object twice");
    MOVE_REFUSES (param, &own, 1, 0.5, REFUSED (DWG_ERR_NOTYETSUPPORTED),
                  "its own parameter");
    MOVE_REFUSES (param, &point, 1, 0.5, REFUSED (DWG_ERR_NOTYETSUPPORTED),
                  "a POINT");
    MOVE_REFUSES (param, &grip, 1, 0.5, REFUSED (DWG_ERR_NOTYETSUPPORTED),
                  "a grip");
    MOVE_REFUSES (param, &ms_line, 1, 0.5, REFUSED (DWG_ERR_INVALIDHANDLE),
                  "an entity of model space");
    MOVE_REFUSES (oparam, &h[CIRCLE], 1, 0.5, REFUSED (DWG_ERR_INVALIDHANDLE),
                  "a parameter of another document");
  }
  {
    // the struct of a grip location component, shorter than a linear
    // parameter's, as param: refused before its points are read (ASan
    // checks the reads)
    const Dwg_Object_BLOCKLINEARPARAMETER *loc
        = (const Dwg_Object_BLOCKLINEARPARAMETER *)node_object (
              dwg, graph_of (dwg, blk), 2)
              ->tio.object->tio.BLOCKGRIPLOCATIONCOMPONENT;
    STRETCH_REFUSES (loc, &frame1, &frame2, h, 1,
                     REFUSED (DWG_ERR_INVALIDHANDLE),
                     "a grip location component as the parameter");
    MOVE_REFUSES (loc, &h[CIRCLE], 1, 0.5, REFUSED (DWG_ERR_INVALIDHANDLE),
                  "a grip location component as the parameter");
  }
  // a parameter with its base point in its middle, or driving a scale
  // action: the evaluator refuses both
  param->parameter_base_location = 1;
  STRETCH_REFUSES (param, &frame1, &frame2, h, 1,
                   REFUSED (DWG_ERR_NOTYETSUPPORTED),
                   "a parameter based in its middle");
  param->parameter_base_location = 0;
  {
    Dwg_Data *doors = new_document ();
    Dwg_Object_BLOCK_HEADER *door
        = dwg_dynblock_copy_block (doors, src, block_header (src, DOOR),
                                   &error);
    Dwg_DynParam *params = NULL;
    BITCODE_BL count = 0;
    loglevel = test_loglevel;
    if (door && !dwg_dynblock_get_params (door, &params, &count))
      {
        const Dwg_Object *size = NULL, *chained = NULL;
        BITCODE_RLL line = 0;
        for (BITCODE_BL i = 0; i < count; i++)
          if (!strcmp (params[i].name, "Door Size"))
            size = object (doors, params[i].handle);
          else if (!strcmp (params[i].name, "Wall Thickness"))
            chained = object (doors, params[i].handle);
        for (BITCODE_BL i = 0; i < door->num_owned && !line; i++)
          {
            const Dwg_Object *e = nth_entity (doors, door, i);
            if (e && e->fixedtype == DWG_TYPE_LINE)
              line = e->handle.value;
          }
        error = 0;
        loglevel = 0;
        CHECK (size && line
                   && !dwg_dynblock_add_move (
                       door, size->tio.object->tio.BLOCKLINEARPARAMETER,
                       &line, 1, 1.0, &error)
                   && error == REFUSED (DWG_ERR_NOTYETSUPPORTED),
               "move refusals: a parameter driving a scale action (0x%x)",
               error);
        // a parameter that chains its actions can't be carried
        if (size && chained)
          {
            BITCODE_RLL c = chained->handle.value;
            Dwg_Object_BLOCKLINEARPARAMETER *w
                = chained->tio.object->tio.BLOCKLINEARPARAMETER;
            const BITCODE_3BD b0 = { 0, 0, 0 }, e0 = { 10, 0, 0 };
            Dwg_Object_BLOCKLINEARPARAMETER *mine;
            loglevel = 0;
            mine = dwg_dynblock_add_linear (door, "Mine", &b0, &e0, &error);
            w->chain_actions = 1;
            error = 0;
            CHECK (mine
                       && !dwg_dynblock_add_move (door, mine, &c, 1, 1.0,
                                                  &error)
                       && error == REFUSED (DWG_ERR_NOTYETSUPPORTED),
                   "move refusals: a parameter that chains its actions "
                   "(0x%x)",
                   error);
            w->chain_actions = 0;
          }
      }
    else
      {
        FAIL ("action refusals: the Door didn't copy (0x%x)", error);
        failures++;
      }
    dwg_dynblock_free_params (params, count);
    free_document (doors);
  }
  loglevel = test_loglevel;
  free_document (dwg);
  free_document (other);
  return failures;
}

/*--------------------------------------------------------------------------------
 * A block that is dynamic already
 */

/* The box with a vertical LINE at x 4, which a linear parameter Shift
   moves by its full change: the Width's move carries the LINE and
   Shift. */
static int
test_carried_parameter (void)
{
  Dwg_Data *dwg = new_document ();
  BITCODE_RLL h[BOX_ENTITIES], blk, marker = 0, more[2];
  const dwg_point_3d m1 = { 4, -3, 0 }, m2 = { 4, -7, 0 };
  const BITCODE_3BD sbase = { 4, -12, 0 }, send = { 9, -12, 0 };
  Dwg_Object_BLOCKLINEARPARAMETER *shift = NULL, *param;
  Dwg_Object_BLOCKSTRETCHACTION *s;
  Dwg_Object_BLOCKMOVEACTION *m = NULL, *shift_move = NULL;
  Dwg_Object_EVALUATION_GRAPH *graph;
  int error = 0, failures = 0;
  blk = dwg ? add_box_block (dwg, "CAPTION", h) : 0;
  if (blk)
    {
      Dwg_Entity_LINE *l = dwg_add_LINE (block_header (dwg, blk), &m1, &m2);
      marker = l ? handle_of (dwg, l) : 0;
      shift = dwg_dynblock_add_linear (block_header (dwg, blk), "Shift",
                                       &sbase, &send, &error);
      shift_move = shift ? dwg_dynblock_add_move (block_header (dwg, blk),
                                                  shift, &marker, 1, 1.0,
                                                  &error)
                         : NULL;
    }
  more[0] = marker;
  more[1] = shift ? handle_of (dwg, shift) : 0;
  param = shift_move ? add_width (dwg, blk, h, more, 2, &s, &m, &error)
                     : NULL;
  loglevel = test_loglevel;
  if (!param || !m || error)
    {
      FAIL ("carried: building the block failed (0x%x)", error);
      if (dwg)
        free_document (dwg);
      return 1;
    }
  graph = graph_of (dwg, blk);
  // Shift: nodes 1 to 4, its move 5; the Width: 6 to 9, stretch 10, move 11
  CHECK (graph->num_nodes == 11 && graph->first_nodeid == 11
             && m->num_actions == 1 && m->actions[0] == 1
             && m->num_deps == 5 && ref (m->deps[0]) == h[CIRCLE]
             && ref (m->deps[1]) == h[CAPTION] && ref (m->deps[2]) == marker
             && ref (m->deps[3]) == handle_of (dwg, shift)
             && ref (m->deps[4])
                    == node_object (dwg, graph, 1)->handle.value,
         "carried: the move selects Shift and its grip, and lists Shift's "
         "node id");
  CHECK (graph->num_edges == 4 + 5 && graph->edges[8].e2 == 5
             && graph->edges[8].e3 == 10,
         "carried: no edge to Shift from the move");
  {
    Dwg_DynValue v[2];
    Dwg_Object_BLOCK_HEADER *rep;
    v[0] = value ("Width", 40.0);
    v[1] = value ("Shift", 8.0);
    rep = dwg_dynblock_evaluate (block_header (dwg, blk), v, 1, &error);
    loglevel = test_loglevel;
    CHECK (rep && !error && is_box_at (dwg, rep, 40.0)
               && near (nth_entity (dwg, rep, BOX_ENTITIES)
                            ->tio.entity->tio.LINE->start.x,
                        14.0),
           "carried: at Width 40 the marker moves by half, with Shift "
           "(0x%x)",
           error);
    rep = dwg_dynblock_evaluate (block_header (dwg, blk), v, 2, &error);
    loglevel = test_loglevel;
    CHECK (rep && !error && is_box_at (dwg, rep, 40.0)
               && near (nth_entity (dwg, rep, BOX_ENTITIES)
                            ->tio.entity->tio.LINE->start.x,
                        17.0),
           "carried: at Width 40 and Shift 8 too, Shift counts from where "
           "the Width moved it (0x%x)",
           error);
  }
  {
    const BITCODE_3BD at = { 0, 50, 0 };
    const Dwg_DynValue v = value ("Width", 33.0);
    Dwg_Entity_INSERT *ins
        = dwg_dynblock_insert (model_space (dwg), block_header (dwg, blk),
                               &at, 1.0, 0.0, &v, 1, NULL, 0, &error);
    loglevel = test_loglevel;
    CHECK (ins && !error,
           "carried: placed at Width 33, Shift carried (0x%x)", error);
  }
  free_document (dwg);
  return failures;
}

/* Of the fixture's Trees, block trees of dwg: a horizontal LINE reaching x
   60, into *linep, and the CIRCLE, into *circlep. */
static void
trees_line_circle (const Dwg_Data *dwg, const Dwg_Object_BLOCK_HEADER *trees,
                   BITCODE_RLL *linep, BITCODE_RLL *circlep)
{
  *linep = *circlep = 0;
  for (BITCODE_BL i = 0; i < trees->num_owned; i++)
    {
      const Dwg_Object *e = nth_entity (dwg, trees, i);
      if (e && e->fixedtype == DWG_TYPE_CIRCLE)
        *circlep = e->handle.value;
      else if (!*linep && e && e->fixedtype == DWG_TYPE_LINE
               && fabs (e->tio.entity->tio.LINE->start.y
                        - e->tio.entity->tio.LINE->end.y)
                      < 1e-6
               && fabs (fmax (e->tio.entity->tio.LINE->start.x,
                              e->tio.entity->tio.LINE->end.x)
                        - 60.0)
                      < 1e-6)
        *linep = e->handle.value;
    }
}

/* The fixture's Trees, with AutoCAD's visibility parameter, copied into a
   new document: the Width joins its graph after AutoCAD's ids, and its
   move carries the visibility parameter with its grip. */
static int
test_autocad_graph (Dwg_Data *src)
{
  Dwg_Data *dwg = new_document ();
  Dwg_Object_BLOCK_HEADER *trees;
  Dwg_Object_EVALUATION_GRAPH *graph;
  BITCODE_RLL blk, vis = 0, line = 0, circle = 0;
  BITCODE_BL nodes, edges;
  BITCODE_BLd first_id;
  Dwg_Object *gobj;
  int error = 0, failures = 0;
  trees = dwg ? dwg_dynblock_copy_block (dwg, src, block_header (src, TREES),
                                         &error)
              : NULL;
  loglevel = test_loglevel;
  if (!trees)
    {
      FAIL ("AutoCAD's graph: copying Trees failed (0x%x)", error);
      if (dwg)
        free_document (dwg);
      return 1;
    }
  blk = handle_of (dwg, trees);
  graph = graph_of (dwg, blk);
  gobj = object (dwg, handle_of (dwg, graph));
  nodes = graph->num_nodes;
  edges = graph->num_edges;
  first_id = graph->first_nodeid;
  {
    Dwg_DynParam *params = NULL;
    BITCODE_BL count = 0;
    if (!dwg_dynblock_get_params (trees, &params, &count) && count)
      vis = params[0].handle;
    dwg_dynblock_free_params (params, count);
  }
  trees_line_circle (dwg, trees, &line, &circle);
  CHECK (vis && line && circle && nodes == 4 && first_id == 4,
         "AutoCAD's graph: Trees, its visibility, a LINE and the CIRCLE, "
         "its 4 nodes with ids up to 4");
  if (vis && line && circle)
    {
      const BITCODE_3BD b = { -60, 0, 0 }, e = { 60, 0, 0 };
      const BITCODE_2RD f1 = { 50, -100 }, f2 = { 70, 100 };
      BITCODE_RLL moved[2] = { circle, vis };
      Dwg_Object_BLOCKLINEARPARAMETER *p
          = dwg_dynblock_add_linear (trees, "Width", &b, &e, &error);
      Dwg_Object_BLOCKMOVEACTION *m = NULL;
      trees = block_header (dwg, blk);
      if (p && dwg_dynblock_add_stretch (trees, p, &f1, &f2, &line, 1, &error))
        m = dwg_dynblock_add_move (block_header (dwg, blk), p, moved, 2, 0.5,
                                   &error);
      loglevel = test_loglevel;
      graph = graph_of (dwg, blk);
      gobj = object (dwg, handle_of (dwg, graph));
      CHECK (m && !error && graph->num_nodes == nodes + 6
             && graph->num_edges == edges + 5
             && graph->nodes[nodes].nextid == first_id + 1
             && graph->first_nodeid == first_id + 6
             && !gobj->num_unknown_bits && !gobj->unknown_bits,
             "AutoCAD's graph: the Width's nodes from id %d, written from "
             "its fields (0x%x)",
             (int)first_id + 1, error);
      CHECK (m && m->num_actions == 1 && m->num_deps == 3
                 && ref (m->deps[1]) == vis
                 && node_object (dwg, graph, 1)->handle.value
                        == ref (m->deps[2]),
             "AutoCAD's graph: the move carries the visibility parameter "
             "and its grip");
      {
        Dwg_DynValue v[2];
        Dwg_Object_BLOCK_HEADER *rep;
        v[0] = value ("Width", 140.0);
        v[1] = value ("Type (view)", 0.0);
        v[1].state = (char *)"Shrub (Plan)";
        rep = dwg_dynblock_evaluate (block_header (dwg, blk), v, 2, &error);
        loglevel = test_loglevel;
        CHECK (rep && !error, "AutoCAD's graph: evaluated at Width 140 and "
                              "another state (0x%x)",
               error);
      }
      {
        const BITCODE_3BD at = { 0, 0, 0 };
        const Dwg_DynValue v = value ("Width", 130.0);
        Dwg_Entity_INSERT *ins = dwg_dynblock_insert (
            model_space (dwg), block_header (dwg, blk), &at, 1.0, 0.0, &v, 1,
            NULL, 0, &error);
        loglevel = test_loglevel;
        CHECK (ins && !error, "AutoCAD's graph: placed at Width 130 (0x%x)",
               error);
      }
    }
  free_document (dwg);
  return failures;
}

/* The visibility parameter of block blk's graph, or NULL. */
static Dwg_Object_BLOCKVISIBILITYPARAMETER *
visibility_of (const Dwg_Data *dwg, const BITCODE_RLL blk)
{
  const Dwg_Object_EVALUATION_GRAPH *graph = graph_of (dwg, blk);
  for (BITCODE_BL i = 0; graph && i < graph->num_nodes; i++)
    {
      const Dwg_Object *obj = node_object (dwg, graph, i);
      if (obj && obj->fixedtype == DWG_TYPE_BLOCKVISIBILITYPARAMETER)
        return obj->tio.object->tio.BLOCKVISIBILITYPARAMETER;
    }
  return NULL;
}

/* The handle of the element of node i of block blk's graph, or 0. */
static BITCODE_RLL
node_handle (const Dwg_Data *dwg, const BITCODE_RLL blk, const BITCODE_BL i)
{
  const Dwg_Object *obj = node_object (dwg, graph_of (dwg, blk), i);
  return obj ? obj->handle.value : 0;
}

/* How many times state lists handle. */
static int
times_listed (const Dwg_BLOCKVISIBILITYPARAMETER_state *state,
              const BITCODE_RLL handle)
{
  int n = 0;
  for (BITCODE_BL k = 0; state->params && k < state->num_params; k++)
    if (ref (state->params[k]) == handle)
      n++;
  return n;
}

#define TREES_STATES 12

/* Whether state lists the count handles of want, in that order, and no
   more. */
static int
lists_are (const Dwg_BLOCKVISIBILITYPARAMETER_state *state,
           const BITCODE_RLL *want, const BITCODE_BL count)
{
  if (state->num_params != count || (count && !state->params))
    return 0;
  for (BITCODE_BL k = 0; k < count; k++)
    if (ref (state->params[k]) != want[k])
      return 0;
  return 1;
}

/* The fixture's Trees, whose visibility states each list its visibility
   parameter and grip, copied into a new document. AutoCAD shows a
   property and a grip of a block with a visibility parameter only in the
   states that list them: a Width, its grip, its stretch and its move are
   listed in every state, after what the state listed before; the move of
   a second property, taken out of two states, only in the states that
   still list that property; and the lists read back from a written
   file. */
static int
test_visibility_states (Dwg_Data *src)
{
  Dwg_Data *dwg = new_document ();
  Dwg_Data back;
  Dwg_Object_BLOCK_HEADER *trees;
  Dwg_Object_BLOCKVISIBILITYPARAMETER *vis;
  Dwg_Object_BLOCKLINEARPARAMETER *width = NULL, *shift = NULL;
  Dwg_Object_BLOCKSTRETCHACTION *s = NULL;
  Dwg_Object_BLOCKMOVEACTION *m = NULL, *shift_move = NULL;
  BITCODE_RLL blk, vis_h, line, circle, before[TREES_STATES][2];
  BITCODE_RLL grip = 0, shift_grip = 0;
  BITCODE_BL nodes;
  int error = 0, failures = 0, good;
  trees = dwg ? dwg_dynblock_copy_block (dwg, src, block_header (src, TREES),
                                         &error)
              : NULL;
  loglevel = test_loglevel;
  vis = trees ? visibility_of (dwg, handle_of (dwg, trees)) : NULL;
  if (!vis || vis->num_states != TREES_STATES || !vis->states)
    {
      FAIL ("visibility states: copying Trees failed, or it hasn't %d "
            "states (0x%x)",
            TREES_STATES, error);
      if (dwg)
        free_document (dwg);
      return 1;
    }
  blk = handle_of (dwg, trees);
  vis_h = handle_of (dwg, vis);
  nodes = graph_of (dwg, blk)->num_nodes;
  trees_line_circle (dwg, trees, &line, &circle);
  good = line && circle;
  for (BITCODE_BL j = 0; good && j < TREES_STATES; j++)
    {
      const Dwg_BLOCKVISIBILITYPARAMETER_state *st = &vis->states[j];
      good = st->num_params == 2 && st->params && ref (st->params[0]) == vis_h;
      before[j][0] = good ? ref (st->params[0]) : 0;
      before[j][1] = good ? ref (st->params[1]) : 0;
    }
  CHECK (good, "visibility states: Trees' states each list its visibility "
             "parameter and grip, and it has the LINE and the CIRCLE");
  if (!good)
    {
      free_document (dwg);
      return failures;
    }

  {
    const BITCODE_3BD b = { -60, 0, 0 }, e = { 60, 0, 0 };
    const BITCODE_2RD f1 = { 50, -100 }, f2 = { 70, 100 };
    BITCODE_RLL moved[2];
    width = dwg_dynblock_add_linear (block_header (dwg, blk), "Width", &b, &e,
                                     &error);
    // the grip is the node after the parameter's
    grip = width && !error ? node_handle (dwg, blk, nodes + 1) : 0;
    s = grip ? dwg_dynblock_add_stretch (block_header (dwg, blk), width, &f1,
                                         &f2, &line, 1, &error)
             : NULL;
    moved[0] = circle;
    moved[1] = vis_h;
    m = s && !error ? dwg_dynblock_add_move (block_header (dwg, blk), width,
                                             moved, 2, 0.5, &error)
                    : NULL;
    loglevel = test_loglevel;
  }
  vis = visibility_of (dwg, blk);
  good = m && !error;
  for (BITCODE_BL j = 0; good && j < TREES_STATES; j++)
    {
      const BITCODE_RLL want[6] = {
        before[j][0], before[j][1], handle_of (dwg, width),
        grip,         handle_of (dwg, s), handle_of (dwg, m)
      };
      good = lists_are (&vis->states[j], want, 6);
    }
  CHECK (good, "visibility states: the Width, its grip, its stretch and its "
             "move listed in every state, after what it listed (0x%x)",
         error);

  // a second property, taken out of the first two states, as AutoCAD
  // stores a property it shows in some states only
  if (good)
    {
      const BITCODE_3BD b = { -60, -20, 0 }, e = { -40, -20, 0 };
      nodes = graph_of (dwg, blk)->num_nodes;
      shift = dwg_dynblock_add_linear (block_header (dwg, blk), "Shift", &b,
                                       &e, &error);
      loglevel = test_loglevel;
      shift_grip = shift && !error ? node_handle (dwg, blk, nodes + 1) : 0;
      vis = visibility_of (dwg, blk);
      good = shift_grip != 0;
      for (BITCODE_BL j = 0; good && j < TREES_STATES; j++)
        {
          Dwg_BLOCKVISIBILITYPARAMETER_state *st = &vis->states[j];
          good = st->num_params == 8
               && ref (st->params[6]) == handle_of (dwg, shift)
               && ref (st->params[7]) == shift_grip;
          if (j < 2)
            st->num_params = 6;
        }
      CHECK (good, "visibility states: Shift and its grip listed in every "
                 "state (0x%x)",
             error);
      shift_move = good ? dwg_dynblock_add_move (block_header (dwg, blk),
                                                 shift, &circle, 1, 1.0,
                                                 &error)
                        : NULL;
      loglevel = test_loglevel;
      vis = visibility_of (dwg, blk);
      good = shift_move && !error;
      for (BITCODE_BL j = 0; good && j < TREES_STATES; j++)
        {
          const Dwg_BLOCKVISIBILITYPARAMETER_state *st = &vis->states[j];
          good = j < 2 ? st->num_params == 6
                           && !times_listed (st, handle_of (dwg, shift_move))
                     : st->num_params == 9
                           && ref (st->params[8])
                                  == handle_of (dwg, shift_move);
        }
      CHECK (good, "visibility states: Shift's move listed only in the states "
                 "that list Shift (0x%x)",
             error);
    }

  error = write_dwg (WRITTEN_VISIBILITY, dwg);
  CHECK (error < DWG_ERR_CRITICAL, "visibility states: written (0x%x)",
         error);
  error = read_dwg (WRITTEN_VISIBILITY, &back);
  CHECK (error < DWG_ERR_CRITICAL, "visibility states: read back (0x%x)",
         error);
  if (error < DWG_ERR_CRITICAL)
    {
      const Dwg_Object_BLOCKVISIBILITYPARAMETER *bvis
          = visibility_of (&back, blk);
      vis = visibility_of (dwg, blk);
      good = bvis && bvis->num_states == TREES_STATES && bvis->states;
      for (BITCODE_BL j = 0; good && j < TREES_STATES; j++)
        {
          const Dwg_BLOCKVISIBILITYPARAMETER_state *st = &vis->states[j];
          const Dwg_BLOCKVISIBILITYPARAMETER_state *bst = &bvis->states[j];
          good = bst->num_params == st->num_params
               && (!st->num_params || bst->params);
          for (BITCODE_BL k = 0; good && k < st->num_params; k++)
            good = ref (bst->params[k]) == ref (st->params[k]);
        }
      CHECK (good, "visibility states: every state's list reads back");
      free_dwg (&back);
    }
  free_document (dwg);
  // kept to look at when a check failed
  if (!failures)
    unlink (WRITTEN_VISIBILITY);
  return failures;
}

/* Evaluates block blk at Length w, and checks its frame, its entity at:
   the vertices 2 and 3 at x w, 0 and 1 unmoved. */
static int
evaluates_frame (Dwg_Data *dwg, const BITCODE_RLL blk, const BITCODE_BL at,
                 const double w, const char *what)
{
  const Dwg_DynValue v = value ("Length", w);
  int error = 0, failures = 0;
  Dwg_Object_BLOCK_HEADER *rep
      = dwg_dynblock_evaluate (block_header (dwg, blk), &v, 1, &error);
  const Dwg_Object *o = rep ? nth_entity (dwg, rep, at) : NULL;
  const Dwg_Entity_LWPOLYLINE *l = o && o->fixedtype == DWG_TYPE_LWPOLYLINE
                                       ? o->tio.entity->tio.LWPOLYLINE
                                       : NULL;
  loglevel = test_loglevel;
  CHECK (rep && !error && l && l->num_points == 4 && near (l->points[0].x, 0)
             && near (l->points[0].y, 0) && near (l->points[1].x, 0)
             && near (l->points[1].y, 36) && near (l->points[2].x, w)
             && near (l->points[2].y, 36) && near (l->points[3].x, w)
             && near (l->points[3].y, 0),
         "%s: evaluated at Length %g, the frame's vertices 2 and 3 at x %g, "
         "0 and 1 unmoved (0x%x)",
         what, w, w, error);
  return failures;
}

/* An LWPOLYLINE as AutoCAD usually writes one: its flags store no
   extrusion, which the decoder leaves 0. The fixture's Window, copied into
   a new document, gets a Length along X, whose stretch selects the right
   side of the Window's frame, a closed LWPOLYLINE 36 by 36: at another
   length its two vertices in the stretch's frame move and the others stay,
   before and after the drawing is written and read back. */
static int
test_lwpolyline_from_dwg (Dwg_Data *src)
{
  Dwg_Data *dwg = new_document ();
  Dwg_Object_BLOCK_HEADER *window = NULL;
  Dwg_Object_BLOCKLINEARPARAMETER *param = NULL;
  Dwg_Object_BLOCKSTRETCHACTION *s = NULL;
  const BITCODE_3BD b = { 0, 0, 0 }, e = { 36, 0, 0 };
  const BITCODE_2RD c1 = { 30, -5 }, c2 = { 40, 41 };
  BITCODE_RLL blk = 0, frame = 0;
  BITCODE_BL at = 0;
  Dwg_Data back;
  int error = 0, failures = 0;
  if (dwg)
    window = dwg_dynblock_copy_block (dwg, src, block_header (src, WINDOW),
                                      &error);
  loglevel = test_loglevel;
  // the Window's frame: its closed LWPOLYLINE of four points
  for (BITCODE_BL i = 0; window && i < window->num_owned && !frame; i++)
    {
      const Dwg_Object *o = nth_entity (dwg, window, i);
      if (o && o->fixedtype == DWG_TYPE_LWPOLYLINE
          && o->tio.entity->tio.LWPOLYLINE->num_points == 4
          && (o->tio.entity->tio.LWPOLYLINE->flag & 512))
        {
          frame = o->handle.value;
          at = i;
        }
    }
  if (!frame)
    {
      FAIL ("LWPOLYLINE read: the Window or its frame not found (0x%x)",
            error);
      if (dwg)
        free_document (dwg);
      return 1;
    }
  blk = handle_of (dwg, window);
  {
    const Dwg_Entity_LWPOLYLINE *l
        = object (dwg, frame)->tio.entity->tio.LWPOLYLINE;
    CHECK (!(l->flag & 1) && l->extrusion.x == 0.0 && l->extrusion.y == 0.0
               && l->extrusion.z == 0.0,
           "LWPOLYLINE read: the Window's frame stores no extrusion, which "
           "reads as 0");
  }
  param = dwg_dynblock_add_linear (window, "Length", &b, &e, &error);
  if (param)
    s = dwg_dynblock_add_stretch (block_header (dwg, blk), param, &c1, &c2,
                                  &frame, 1, &error);
  loglevel = test_loglevel;
  CHECK (s && !error && s->num_hdls == 1 && ref (s->hdls[0].hdl) == frame
             && s->hdls[0].num_indexes == 2 && s->hdls[0].indexes[0] == 2
             && s->hdls[0].indexes[1] == 3,
         "LWPOLYLINE read: stretched, its vertices 2 and 3 in the frame "
         "(0x%x)",
         error);
  if (!s)
    {
      free_document (dwg);
      return failures;
    }
  failures += evaluates_frame (dwg, blk, at, 50.0, "LWPOLYLINE read");

  error = write_dwg (WRITTEN_LWPOLYLINE, dwg);
  CHECK (error < DWG_ERR_CRITICAL, "LWPOLYLINE read: written (0x%x)", error);
  error = error < DWG_ERR_CRITICAL ? read_dwg (WRITTEN_LWPOLYLINE, &back)
                                   : error;
  CHECK (error < DWG_ERR_CRITICAL, "LWPOLYLINE read: read back (0x%x)",
         error);
  if (error >= DWG_ERR_CRITICAL)
    {
      free_document (dwg);
      return failures;
    }
  {
    const Dwg_Object *o = object (&back, frame);
    const Dwg_Object_EVALUATION_GRAPH *graph = graph_of (&back, blk);
    const Dwg_Object *a
        = graph && graph->num_nodes
              ? node_object (&back, graph, graph->num_nodes - 1)
              : NULL;
    const Dwg_Object_BLOCKSTRETCHACTION *bs
        = a && a->fixedtype == DWG_TYPE_BLOCKSTRETCHACTION
              ? a->tio.object->tio.BLOCKSTRETCHACTION
              : NULL;
    CHECK (o && o->fixedtype == DWG_TYPE_LWPOLYLINE
               && !(o->tio.entity->tio.LWPOLYLINE->flag & 1)
               && o->tio.entity->tio.LWPOLYLINE->extrusion.z == 0.0
               && bs && bs->num_hdls == 1 && ref (bs->hdls[0].hdl) == frame
               && bs->hdls[0].num_indexes == 2
               && bs->hdls[0].indexes[0] == 2
               && bs->hdls[0].indexes[1] == 3,
           "LWPOLYLINE read: the frame, still with no extrusion, and the "
           "stretch read back");
  }
  failures += evaluates_frame (&back, blk, at, 20.0,
                               "LWPOLYLINE read, read back");
  free_dwg (&back);
  free_document (dwg);
  // kept to look at when a check failed
  if (!failures)
    unlink (WRITTEN_LWPOLYLINE);
  return failures;
}

/*--------------------------------------------------------------------------------
 * Written and read back
 */

/* Whether the graph of block blk of a and b are the same. */
static int
same_graph (const Dwg_Data *a, const BITCODE_RLL blk_a, const Dwg_Data *b,
            const BITCODE_RLL blk_b)
{
  const Dwg_Object_EVALUATION_GRAPH *ga = graph_of (a, blk_a);
  const Dwg_Object_EVALUATION_GRAPH *gb = graph_of (b, blk_b);
  if (!ga || !gb || ga->num_nodes != gb->num_nodes
      || ga->num_edges != gb->num_edges
      || ga->first_nodeid != gb->first_nodeid)
    return 0;
  for (BITCODE_BL i = 0; i < ga->num_nodes; i++)
    {
      const Dwg_EVAL_Node *na = &ga->nodes[i], *nb = &gb->nodes[i];
      const Dwg_Object *ea = node_object (a, ga, i);
      const Dwg_Object *eb = node_object (b, gb, i);
      if (na->id != nb->id || na->edge_flags != nb->edge_flags
          || na->nextid != nb->nextid || !ea || !eb
          || ea->fixedtype != eb->fixedtype
          || memcmp (na->node, nb->node, sizeof (na->node)))
        return 0;
    }
  for (BITCODE_BL i = 0; i < ga->num_edges; i++)
    {
      const Dwg_EVAL_Edge *ea = &ga->edges[i], *eb = &gb->edges[i];
      if (ea->id != eb->id || ea->nextid != eb->nextid || ea->e1 != eb->e1
          || ea->e2 != eb->e2 || ea->e3 != eb->e3
          || memcmp (ea->out_edge, eb->out_edge, sizeof (ea->out_edge)))
        return 0;
    }
  return 1;
}

static int
test_written (void)
{
  Dwg_Data *dwg = new_document ();
  Dwg_Data back;
  BITCODE_RLL h[BOX_ENTITIES], blk, ins_default = 0, ins_40 = 0;
  Dwg_Object_BLOCKSTRETCHACTION *s;
  Dwg_Object_BLOCKMOVEACTION *m;
  Dwg_DynBlock_Check_Report *report;
  int error = 0, failures = 0;
  blk = dwg ? add_box_block (dwg, "BOX", h) : 0;
  if (!blk || !add_width (dwg, blk, h, NULL, 0, &s, &m, &error) || error)
    {
      FAIL ("written: building the block failed");
      if (dwg)
        free_document (dwg);
      return 1;
    }
  {
    const dwg_point_3d at = { 100, 100, 0 };
    const BITCODE_3BD at2 = { 100, 50, 0 };
    const Dwg_DynValue v = value ("Width", 40.0);
    Dwg_Entity_INSERT *a = dwg_add_INSERT_block (
        model_space (dwg), &at, block_header (dwg, blk), 1, 1, 1, 0);
    Dwg_Entity_INSERT *b
        = dwg_dynblock_insert (model_space (dwg), block_header (dwg, blk),
                               &at2, 1.0, 0.0, &v, 1, NULL, 0, &error);
    loglevel = test_loglevel;
    ins_default = a ? handle_of (dwg, a) : 0;
    ins_40 = b ? handle_of (dwg, b) : 0;
  }
  error = write_dwg (WRITTEN, dwg);
  CHECK (ins_default && ins_40 && error < DWG_ERR_CRITICAL,
         "written: placed at the defaults and at Width 40, written (0x%x)",
         error);
  error = read_dwg (WRITTEN, &back);
  CHECK (error < DWG_ERR_CRITICAL, "written: read back (0x%x)", error);
  if (error >= DWG_ERR_CRITICAL)
    {
      free_document (dwg);
      return failures;
    }
  CHECK (same_graph (dwg, blk, &back, blk),
         "written: the graph reads back the same");
  {
    const Dwg_Object *p = node_object (&back, graph_of (&back, blk), 0);
    const Dwg_Object *st = node_object (&back, graph_of (&back, blk), 4);
    const Dwg_Object *mv = node_object (&back, graph_of (&back, blk), 5);
    const Dwg_Object_BLOCKSTRETCHACTION *bs
        = st ? st->tio.object->tio.BLOCKSTRETCHACTION : NULL;
    CHECK (p && p->fixedtype == DWG_TYPE_BLOCKLINEARPARAMETER
               && !strcmp (p->tio.object->tio.BLOCKLINEARPARAMETER
                               ->distance_name,
                           "Width")
               && bs && bs->num_hdls == 3 && bs->num_deps == 3
               && bs->num_pts == 2 && bs->pts[0].x == 25.0
               && !strcmp (bs->conn_pts[0].name, "EndXDelta")
               && mv && mv->tio.object->tio.BLOCKMOVEACTION->action_offset_x
                            == 0.5,
           "written: the parameter and actions read back");
  }
  failures += evaluates_box (&back, blk, 35.0, "written");
  report = dwg_dynblock_check (&back, 1e-9);
  loglevel = test_loglevel;
  CHECK (report && report->num_checks == 1 && report->num_match == 1
             && report->checks[0].insert == ins_40,
         "written: the placement at Width 40 checks against its "
         "representation block");
  dwg_dynblock_free_check_report (report);
  free_dwg (&back);
  free_document (dwg);
  // kept to look at when a check failed
  if (!failures)
    unlink (WRITTEN);
  return failures;
}

/*--------------------------------------------------------------------------------
 * Attribute definitions an action selects
 */

/* The handle of the APPID named name, added to dwg when it has none, or 0
   when memory runs out. */
static BITCODE_RLL
appid_of (Dwg_Data *dwg, const char *name)
{
  BITCODE_H found = dwg_find_tablehandle (dwg, name, "APPID");
  Dwg_Object_APPID *appid;
  loglevel = test_loglevel;
  if (found && found->absolute_ref)
    return found->absolute_ref;
  appid = dwg_add_APPID (dwg, name);
  loglevel = test_loglevel;
  return appid ? handle_of (dwg, appid) : 0;
}

/* Gives entity ent an EED group of the APPID named name, of n 1070 items
   of the given values. Returns 1 when memory runs out. */
static int
add_1070_group (Dwg_Data *dwg, Dwg_Object_Entity *ent, const char *name,
                const BITCODE_RS *values, const BITCODE_BL n)
{
  const BITCODE_RLL appid = appid_of (dwg, name);
  // a 1070 takes 3 bytes in a file
  Dwg_Eed *eed
      = appid ? append_eed (ent, appid, (BITCODE_BS)(3 * n), n) : NULL;
  if (!eed)
    return 1;
  for (BITCODE_BL i = 0; i < n; i++)
    {
      eed[i].data->code = 70;
      // Dwg_Eed_Data is packed
      memcpy (&eed[i].data->u.eed_70.rs, &values[i], sizeof (values[i]));
    }
  return 0;
}

/* The values of entity obj's EED group of the APPID named name, of 1070
   items, into values, up to max of them; *lastp is set when the group is
   the last of obj's EED. Returns how many items the group has, -1 when
   obj has no such group, or -2 when it has two, or an item that isn't a
   1070. */
static int
group_1070s (const Dwg_Data *dwg, const Dwg_Object *obj, const char *name,
             BITCODE_RS *values, const int max, int *lastp)
{
  const Dwg_Eed *eed = obj->tio.entity->eed;
  int n = -1, in_group = 0;
  *lastp = 0;
  for (BITCODE_BL i = 0; eed && i < obj->tio.entity->num_eed; i++)
    {
      if (eed[i].size)
        {
          const Dwg_Object *appid = object (dwg, eed[i].handle.value);
          in_group = appid && appid->fixedtype == DWG_TYPE_APPID
                     && !strcmp (appid->tio.object->tio.APPID->name, name);
          if (in_group && n >= 0)
            return -2;
          if (in_group)
            n = 0;
          *lastp = in_group;
        }
      if (!in_group)
        continue;
      if (!eed[i].data || eed[i].data->code != 70)
        return -2;
      if (n < max)
        // Dwg_Eed_Data is packed
        memcpy (&values[n], &eed[i].data->u.eed_70.rs, sizeof (values[n]));
      n++;
    }
  return n;
}

/* Whether entity obj has its position locked as AutoCAD locks it in r2004
   files: one AcDbAttr EED group, 1070 0, 1070 1; and with last, as the
   last group of its EED. */
static int
is_locked (const Dwg_Data *dwg, const Dwg_Object *obj, const int last)
{
  BITCODE_RS v[2] = { 9, 9 };
  int is_last;
  return group_1070s (dwg, obj, "AcDbAttr", v, 2, &is_last) == 2 && v[0] == 0
         && v[1] == 1 && (!last || is_last);
}

/* The number of APPIDs named name in dwg. */
static int
count_appids (const Dwg_Data *dwg, const char *name)
{
  int n = 0;
  for (BITCODE_BL i = 0; i < dwg->num_objects; i++)
    if (dwg->object[i].fixedtype == DWG_TYPE_APPID
        && dwg->object[i].tio.object->tio.APPID->name
        && !strcmp (dwg->object[i].tio.object->tio.APPID->name, name))
      n++;
  return n;
}

/* The single 1070 of entity obj's TESTAPP EED group, or -1. */
static int
test_group (const Dwg_Data *dwg, const Dwg_Object *obj)
{
  BITCODE_RS v = 0;
  int last;
  return group_1070s (dwg, obj, "TESTAPP", &v, 1, &last) == 1 ? v : -1;
}

/* The box's attribute definitions for test_locked_attdefs: NAME centred,
   the others left on their baseline. */
enum
{
  A_NAME,   // centred on (10, -2): in the move
  A_LEFT,   // at (3, -8), AcDbAttr 1070 0, 1070 0, then TESTAPP 7: in it
  A_LOCKED, // at (3, -6), AcDbAttr 1070 0, 1070 1, then TESTAPP 8: in it
  A_SIDE,   // at (21, -3), in the stretch's frame: in the stretch
  A_FREE,   // at (1, -4): in neither
  NUM_ATTDEFS
};

/* The box drawn in a new block BOX of a new document, with its attribute
   definitions, written and read back into *dwg, so that their EED groups
   are kept as the bytes read, as for a block read from any file. */
static int
locked_box (Dwg_Data *dwg, BITCODE_RLL *blkp, BITCODE_RLL *h,
            BITCODE_RLL *a)
{
  static const char *tags[NUM_ATTDEFS]
      = { "NAME", "LEFT", "LOCKED", "SIDE", "FREE" };
  static const dwg_point_3d at[NUM_ATTDEFS] = {
    { 8, -2, 0 }, { 3, -8, 0 }, { 3, -6, 0 }, { 21, -3, 0 }, { 1, -4, 0 }
  };
  const BITCODE_RS unlocked[2] = { 0, 0 }, locked[2] = { 0, 1 }, seven = 7,
                   eight = 8;
  Dwg_Data *src = new_document ();
  Dwg_Object_BLOCK_HEADER *blk;
  Dwg_Entity_ATTDEF *att[NUM_ATTDEFS];
  int error;
  *blkp = src ? add_box_block (src, "BOX", h) : 0;
  blk = *blkp ? block_header (src, *blkp) : NULL;
  for (int i = 0; blk && i < NUM_ATTDEFS; i++)
    {
      att[i] = dwg_add_ATTDEF (blk, 1.0, 0, "Prompt", &at[i], tags[i], "X");
      loglevel = test_loglevel;
      if (!att[i])
        blk = NULL;
      else
        {
          att[i]->width_factor = 1.0;
          a[i] = handle_of (src, att[i]);
        }
    }
  if (!blk)
    {
      if (src)
        free_document (src);
      return 1;
    }
  att[A_NAME]->horiz_alignment = 1;
  att[A_NAME]->alignment_pt.x = 10.0;
  att[A_NAME]->alignment_pt.y = -2.0;
  if (add_1070_group (src, att[A_LEFT]->parent, "AcDbAttr", unlocked, 2)
      || add_1070_group (src, att[A_LEFT]->parent, "TESTAPP", &seven, 1)
      || add_1070_group (src, att[A_LOCKED]->parent, "AcDbAttr", locked, 2)
      || add_1070_group (src, att[A_LOCKED]->parent, "TESTAPP", &eight, 1))
    {
      free_document (src);
      return 1;
    }
  error = write_dwg (WRITTEN_LOCKED, src);
  free_document (src);
  if (error >= DWG_ERR_CRITICAL)
    return 1;
  if (read_dwg (WRITTEN_LOCKED, dwg) >= DWG_ERR_CRITICAL)
    {
      free_dwg (dwg);
      return 1;
    }
  return 0;
}

/* Checks the attribute definitions of test_locked_attdefs in dwg, after
   the actions were added: those the actions select locked, A_LEFT's group
   given way to a new one at the end, A_LOCKED's left as it was, the other
   groups kept, and A_FREE and the TEXT caption not locked. */
static int
locks_are (const Dwg_Data *dwg, const BITCODE_RLL *h, const BITCODE_RLL *a,
           const char *what)
{
  const Dwg_Object *o[NUM_ATTDEFS];
  BITCODE_RS v[2];
  int last, failures = 0;
  for (int i = 0; i < NUM_ATTDEFS; i++)
    {
      o[i] = object (dwg, a[i]);
      if (!o[i] || o[i]->fixedtype != DWG_TYPE_ATTDEF)
        {
          FAIL ("%s: attribute definition %d not found", what, i);
          return 1;
        }
    }
  CHECK (is_locked (dwg, o[A_NAME], 1) && is_locked (dwg, o[A_SIDE], 1),
         "%s: the move's and the stretch's new ATTDEFs are locked, by a "
         "group at the end",
         what);
  CHECK (is_locked (dwg, o[A_LEFT], 1) && test_group (dwg, o[A_LEFT]) == 7,
         "%s: an unlocked AcDbAttr group gives way to a locked one at the "
         "end; the group after it is kept",
         what);
  CHECK (is_locked (dwg, o[A_LOCKED], 0) && !is_locked (dwg, o[A_LOCKED], 1)
             && test_group (dwg, o[A_LOCKED]) == 8,
         "%s: a locked ATTDEF keeps its group where it was", what);
  CHECK (group_1070s (dwg, o[A_FREE], "AcDbAttr", v, 2, &last) == -1
             && group_1070s (dwg, object (dwg, h[CAPTION]), "AcDbAttr", v, 2,
                             &last)
                    == -1,
         "%s: an ATTDEF no action selects, and a TEXT, get no AcDbAttr "
         "group",
         what);
  return failures;
}

/* Whether ATTDEF obj of block rep is at (x, y), with its alignment point
   at (ax, ay) when it has one. */
static int
attdef_at (const Dwg_Object *obj, const double x, const double y,
           const double ax, const double ay)
{
  const Dwg_Entity_ATTDEF *_obj;
  if (!obj || obj->fixedtype != DWG_TYPE_ATTDEF)
    return 0;
  _obj = obj->tio.entity->tio.ATTDEF;
  return near (_obj->ins_pt.x, x) && near (_obj->ins_pt.y, y)
         && (!_obj->horiz_alignment
             || (near (_obj->alignment_pt.x, ax)
                 && near (_obj->alignment_pt.y, ay)));
}

/* Evaluates block blk at Width 30, 10 more than its own: the locked
   ATTDEFs the move selects move by 5, the stretch's by 10, A_FREE stays. */
static int
evaluates_attdefs (Dwg_Data *dwg, const BITCODE_RLL blk, const char *what)
{
  const Dwg_DynValue v = value ("Width", 30.0);
  int error = 0, failures = 0;
  Dwg_Object_BLOCK_HEADER *rep
      = dwg_dynblock_evaluate (block_header (dwg, blk), &v, 1, &error);
  loglevel = test_loglevel;
  if (!rep || error)
    {
      FAIL ("%s: evaluating at Width 30 failed (0x%x)", what, error);
      return 1;
    }
  // the ATTDEFs follow the box's entities
  CHECK (attdef_at (nth_entity (dwg, rep, BOX_ENTITIES + A_NAME), 13.0,
                    -2.0, 15.0, -2.0)
             && attdef_at (nth_entity (dwg, rep, BOX_ENTITIES + A_LEFT), 8.0,
                           -8.0, 0.0, 0.0)
             && attdef_at (nth_entity (dwg, rep, BOX_ENTITIES + A_LOCKED),
                           8.0, -6.0, 0.0, 0.0),
         "%s: at Width 30, the ATTDEFs the move selects move by 5", what);
  CHECK (attdef_at (nth_entity (dwg, rep, BOX_ENTITIES + A_SIDE), 31.0, -3.0,
                    0.0, 0.0),
         "%s: the ATTDEF the stretch selects moves by 10", what);
  CHECK (attdef_at (nth_entity (dwg, rep, BOX_ENTITIES + A_FREE), 1.0, -4.0,
                    0.0, 0.0),
         "%s: the ATTDEF neither selects stays", what);
  return failures;
}

/* dwg_dynblock_add_move () and dwg_dynblock_add_stretch () lock the
   position of the attribute definitions they select, which AutoCAD moves
   with an action only when locked, in a block read from a file: an
   AcDbAttr group 1070 0, 1070 1 at the end of each one's EED, in place of
   an unlocked group, which a group kept as the bytes read follows; a
   locked one is left as it is. A refused move locks nothing. The block
   evaluates with them moved, placements check, and the written file reads
   back the same. */
static int
test_locked_attdefs (void)
{
  Dwg_Data *dwg = (Dwg_Data *)calloc (1, sizeof (Dwg_Data));
  Dwg_Data back;
  BITCODE_RLL h[BOX_ENTITIES], a[NUM_ATTDEFS], blk = 0, ins_40 = 0;
  Dwg_Object_BLOCKLINEARPARAMETER *param;
  Dwg_Object_BLOCKSTRETCHACTION *s;
  Dwg_Object_BLOCKMOVEACTION *m;
  Dwg_DynBlock_Check_Report *report;
  BITCODE_BL num_eed;
  BITCODE_RS v[2];
  int error = 0, failures = 0, last;
  if (!dwg || locked_box (dwg, &blk, h, a))
    {
      FAIL ("locked ATTDEFs: building the block failed");
      free (dwg);
      unlink (WRITTEN_LOCKED);
      return 1;
    }
  param = dwg_dynblock_add_linear (block_header (dwg, blk), "Width",
                                   &box_base, &box_end, &error);
  loglevel = test_loglevel;
  if (!param || error)
    {
      FAIL ("locked ATTDEFs: adding Width failed (0x%x)", error);
      free_document (dwg);
      return 1;
    }
  // refused, for the POINT: nothing is locked
  {
    const dwg_point_3d pt = { 5, -5, 0 };
    Dwg_Entity_POINT *point = dwg_add_POINT (block_header (dwg, blk), &pt);
    BITCODE_RLL refused[2];
    loglevel = test_loglevel;
    refused[0] = a[A_NAME];
    refused[1] = point ? handle_of (dwg, point) : 0;
    m = point ? dwg_dynblock_add_move (block_header (dwg, blk), param,
                                       refused, 2, 0.5, &error)
              : NULL;
    loglevel = test_loglevel;
    CHECK (point && !m && error == REFUSED (DWG_ERR_NOTYETSUPPORTED)
               && group_1070s (dwg, object (dwg, a[A_NAME]), "AcDbAttr", v,
                               2, &last)
                      == -1,
           "locked ATTDEFs: a refused move locks nothing (0x%x)", error);
  }
  num_eed = object (dwg, a[A_LOCKED])->tio.entity->num_eed;
  {
    const BITCODE_RLL stretched[4]
        = { h[TOP], h[RIGHT], h[BOTTOM], a[A_SIDE] };
    const BITCODE_RLL moved[5]
        = { h[CIRCLE], h[CAPTION], a[A_NAME], a[A_LEFT], a[A_LOCKED] };
    s = dwg_dynblock_add_stretch (block_header (dwg, blk), param, &frame1,
                                  &frame2, stretched, 4, &error);
    loglevel = test_loglevel;
    CHECK (s && !error, "locked ATTDEFs: stretch added (0x%x)", error);
    m = dwg_dynblock_add_move (block_header (dwg, blk), param, moved, 5, 0.5,
                               &error);
    loglevel = test_loglevel;
    CHECK (m && !error, "locked ATTDEFs: move added (0x%x)", error);
  }
  if (!s || !m)
    {
      free_document (dwg);
      unlink (WRITTEN_LOCKED);
      return failures;
    }
  failures += locks_are (dwg, h, a, "locked ATTDEFs");
  CHECK (object (dwg, a[A_LOCKED])->tio.entity->num_eed == num_eed,
         "locked ATTDEFs: the locked ATTDEF's EED is unchanged");
  // the field r2007 and later files store the lock in, which r2004 files
  // don't, so it doesn't read back
  CHECK (object (dwg, a[A_NAME])->tio.entity->tio.ATTDEF->lock_position_flag
             && object (dwg, a[A_LEFT])
                    ->tio.entity->tio.ATTDEF->lock_position_flag
             && object (dwg, a[A_SIDE])
                    ->tio.entity->tio.ATTDEF->lock_position_flag
             && !object (dwg, a[A_FREE])
                     ->tio.entity->tio.ATTDEF->lock_position_flag,
         "locked ATTDEFs: the ATTDEFs locked get lock_position_flag too");
  CHECK (count_appids (dwg, "AcDbAttr") == 1,
         "locked ATTDEFs: one AcDbAttr APPID, the file's");
  failures += evaluates_attdefs (dwg, blk, "locked ATTDEFs");
  {
    const BITCODE_3BD at = { 100, 50, 0 };
    const Dwg_DynValue v40 = value ("Width", 40.0);
    Dwg_Entity_INSERT *ins
        = dwg_dynblock_insert (model_space (dwg), block_header (dwg, blk),
                               &at, 1.0, 0.0, &v40, 1, NULL, 0, &error);
    loglevel = test_loglevel;
    ins_40 = ins && !error ? handle_of (dwg, ins) : 0;
    CHECK (ins && !error && ins->num_owned == NUM_ATTDEFS,
           "locked ATTDEFs: placed at Width 40 (0x%x)", error);
  }

  error = write_dwg (WRITTEN_LOCKED, dwg);
  free_document (dwg);
  error = error < DWG_ERR_CRITICAL ? read_dwg (WRITTEN_LOCKED, &back) : error;
  CHECK (error < DWG_ERR_CRITICAL, "locked ATTDEFs: written and read back "
                                   "(0x%x)",
         error);
  if (error >= DWG_ERR_CRITICAL)
    return failures;
  failures += locks_are (&back, h, a, "locked ATTDEFs read back");
  failures += evaluates_attdefs (&back, blk, "locked ATTDEFs read back");
  {
    // the placement's ATTRIBs, in the block's order: where the evaluation
    // moved their ATTDEFs, each with its definition's lock
    const Dwg_Object *insobj = object (&back, ins_40);
    const Dwg_Entity_INSERT *ins
        = insobj && insobj->fixedtype == DWG_TYPE_INSERT
              ? insobj->tio.entity->tio.INSERT
              : NULL;
    static const double x[NUM_ATTDEFS] = { 110 + 10, 103 + 10, 103 + 10,
                                           121 + 20, 101 };
    int at = ins && ins->num_owned == NUM_ATTDEFS;
    for (int i = 0; at && i < NUM_ATTDEFS; i++)
      {
        const Dwg_Object *attrib = object (&back, ref (ins->attribs[i]));
        const Dwg_Entity_ATTRIB *_attrib
            = attrib && attrib->fixedtype == DWG_TYPE_ATTRIB
                  ? attrib->tio.entity->tio.ATTRIB
                  : NULL;
        at = _attrib
             && near (i == A_NAME ? _attrib->alignment_pt.x
                                  : _attrib->ins_pt.x,
                      x[i])
             && (i == A_FREE ? !is_locked (&back, attrib, 0)
                             : is_locked (&back, attrib, 0));
      }
    CHECK (at, "locked ATTDEFs read back: the placement's ATTRIBs are where "
               "the evaluation moved their ATTDEFs, with their locks");
  }
  report = dwg_dynblock_check (&back, 1e-9);
  loglevel = test_loglevel;
  CHECK (report && report->num_checks == 1 && report->num_match == 1
             && report->checks[0].insert == ins_40,
         "locked ATTDEFs read back: the placement at Width 40 checks against "
         "its representation block");
  dwg_dynblock_free_check_report (report);
  free_dwg (&back);
  // kept to look at when a check failed
  if (!failures)
    unlink (WRITTEN_LOCKED);
  return failures;
}

/* The first lock in a document without an AcDbAttr APPID adds one, which
   can move the document's object list: the ATTDEF is found again after
   it. Here the list is full but for the move's action, so that adding the
   APPID reallocates it, which an ASan build catches when the ATTDEF's old
   object is used. */
static int
test_lock_adds_appid (void)
{
  Dwg_Data *dwg = new_document ();
  BITCODE_RLL h[BOX_ENTITIES], blk, att_h = 0;
  Dwg_Object_BLOCKLINEARPARAMETER *param = NULL;
  Dwg_Object_BLOCKMOVEACTION *m;
  BITCODE_BL before;
  int error = 0, failures = 0;
  blk = dwg ? add_box_block (dwg, "BOX", h) : 0;
  if (blk)
    {
      const dwg_point_3d at = { 3, -8, 0 };
      Dwg_Entity_ATTDEF *att = dwg_add_ATTDEF (block_header (dwg, blk), 1.0,
                                               0, "Prompt", &at, "TAG", "X");
      att_h = att ? handle_of (dwg, att) : 0;
      if (att)
        param = dwg_dynblock_add_linear (block_header (dwg, blk), "Width",
                                         &box_base, &box_end, &error);
    }
  loglevel = test_loglevel;
  if (!param || count_appids (dwg, "AcDbAttr"))
    {
      FAIL ("lock adds the APPID: building the block failed (0x%x)", error);
      if (dwg)
        free_document (dwg);
      return 1;
    }
  before = dwg->num_objects;
  dwg->num_alloced_objects = before + 1;
  m = dwg_dynblock_add_move (block_header (dwg, blk), param, &att_h, 1, 0.5,
                             &error);
  loglevel = test_loglevel;
  CHECK (m && !error && dwg->num_objects == before + 2
             && dwg->num_alloced_objects > before + 1
             && count_appids (dwg, "AcDbAttr") == 1
             && is_locked (dwg, object (dwg, att_h), 1),
         "lock adds the APPID: the move's action, then the APPID, which "
         "reallocates the object list; the ATTDEF is locked (0x%x)",
         error);
  free_document (dwg);
  return failures;
}

/*--------------------------------------------------------------------------------
 * Copying entities
 */

/* A drawing of a box at x 100, y 200 with its base point there, on layer
   BOXES, and an attribute definition and an INSERT of block MARK. */
static Dwg_Data *
box_drawing (BITCODE_RLL *h)
{
  Dwg_Data *dwg = new_document ();
  Dwg_Object_BLOCK_HEADER *mark;
  Dwg_Object_LAYER *layer;
  const dwg_point_3d a = { 100, 200, 0 }, b = { 120, 200, 0 },
                     c = { 120, 190, 0 }, mk = { 110, 195, 0 },
                     ins = { 102, 192, 0 }, o = { 0, 0, 0 }, o2 = { 1, 1, 0 };
  Dwg_Entity_LINE *l1, *l2;
  Dwg_Entity_ATTDEF *att;
  Dwg_Entity_INSERT *mi;
  if (!dwg)
    return NULL;
  dwg->header_vars.INSBASE.x = 100;
  dwg->header_vars.INSBASE.y = 200;
  layer = dwg_add_LAYER (dwg, "BOXES");
  mark = dwg_add_BLOCK_HEADER (dwg, "MARK");
  if (!layer || !mark || !dwg_add_BLOCK (mark, "MARK")
      || !dwg_add_LINE (mark, &o, &o2) || !dwg_add_ENDBLK (mark))
    {
      free_document (dwg);
      return NULL;
    }
  l1 = dwg_add_LINE (model_space (dwg), &a, &b);
  l2 = dwg_add_LINE (model_space (dwg), &b, &c);
  att = dwg_add_ATTDEF (model_space (dwg), 2.0, 0, "Tag?", &ins, "TAG", "X");
  mi = dwg_add_INSERT (model_space (dwg), &mk, "MARK", 1, 1, 1, 0);
  loglevel = test_loglevel;
  if (!l1 || !l2 || !att || !mi)
    {
      free_document (dwg);
      return NULL;
    }
  att->width_factor = 1.0;
  {
    Dwg_Object *lo = dwg_obj_generic_to_object (layer, &(int){ 0 });
    dwg_obj_generic_to_object (l1, &(int){ 0 })->tio.entity->layer
        = dwg_add_handleref (dwg, 5, lo->handle.value, NULL);
  }
  h[0] = handle_of (dwg, l1);
  h[1] = handle_of (dwg, l2);
  h[2] = handle_of (dwg, att);
  h[3] = handle_of (dwg, mi);
  return dwg;
}

static int
test_copy_entities (void)
{
  BITCODE_RLL sh[4], h[BOX_ENTITIES], blk;
  Dwg_Data *src = box_drawing (sh);
  Dwg_Data *dst = new_document ();
  Dwg_Object_BLOCK_HEADER *b;
  const BITCODE_3BD offset = { 5, -1, 0 };
  BITCODE_BL n, before;
  int error = 0, failures = 0;
  blk = dst ? add_box_block (dst, "BOX", h) : 0;
  if (!src || !blk)
    {
      FAIL ("copy: building the drawings failed");
      if (src)
        free_document (src);
      if (dst)
        free_document (dst);
      return 1;
    }
  // a block based at (1, 2): src's base point (100, 200) goes to (6, 1)
  block_header (dst, blk)->base_pt.x = 1;
  block_header (dst, blk)->base_pt.y = 2;
  before = block_header (dst, blk)->num_owned;
  n = dwg_dynblock_copy_entities (block_header (dst, blk), src,
                                  model_space (src), &offset, &error);
  loglevel = test_loglevel;
  b = block_header (dst, blk);
  CHECK (n == 4 && !error && b->num_owned == before + 4,
         "copy: 4 entities of model space appended to a block (0x%x)",
         error);
  if (n == 4)
    {
      const Dwg_Object *l1 = nth_entity (dst, b, before);
      const Dwg_Object *l2 = nth_entity (dst, b, before + 1);
      const Dwg_Object *att = nth_entity (dst, b, before + 2);
      const Dwg_Object *mi = nth_entity (dst, b, before + 3);
      const Dwg_Object *layer
          = l1 ? object (dst, ref (l1->tio.entity->layer)) : NULL;
      const Dwg_Object *mark
          = mi ? object (dst, ref (mi->tio.entity->tio.INSERT->block_header))
               : NULL;
      CHECK (l1 && l1->fixedtype == DWG_TYPE_LINE && l2
                 && l2->fixedtype == DWG_TYPE_LINE && att
                 && att->fixedtype == DWG_TYPE_ATTDEF && mi
                 && mi->fixedtype == DWG_TYPE_INSERT,
             "copy: in src's order");
      if (l1 && l2 && att && mi && l1->fixedtype == DWG_TYPE_LINE
          && mi->fixedtype == DWG_TYPE_INSERT
          && att->fixedtype == DWG_TYPE_ATTDEF)
        {
          CHECK (near (l1->tio.entity->tio.LINE->start.x, 6.0)
                     && near (l1->tio.entity->tio.LINE->start.y, 1.0)
                     && near (l1->tio.entity->tio.LINE->end.x, 26.0)
                     && near (att->tio.entity->tio.ATTDEF->ins_pt.x, 8.0)
                     && near (att->tio.entity->tio.ATTDEF->ins_pt.y, -7.0)
                     && near (mi->tio.entity->tio.INSERT->ins_pt.x, 16.0)
                     && near (mi->tio.entity->tio.INSERT->ins_pt.y, -4.0),
                 "copy: src's base point at the block's plus the offset");
          CHECK (!l1->tio.entity->entmode
                     && ref (l1->tio.entity->ownerhandle) == blk
                     && !mi->tio.entity->entmode
                     && ref (mi->tio.entity->ownerhandle) == blk,
                 "copy: owned by the block");
          CHECK (layer && layer->fixedtype == DWG_TYPE_LAYER
                     && !strcmp (layer->tio.object->tio.LAYER->name, "BOXES"),
                 "copy: on BOXES, which dst gets");
          CHECK (mark && mark->fixedtype == DWG_TYPE_BLOCK_HEADER
                     && !strcmp (mark->tio.object->tio.BLOCK_HEADER->name,
                                 "MARK")
                     && mark->tio.object->tio.BLOCK_HEADER->num_inserts == 1
                     && ref (mark->tio.object->tio.BLOCK_HEADER->inserts[0])
                            == mi->handle.value,
                 "copy: the INSERT draws MARK, which dst gets, and lists it");
          CHECK (b->hasattrs && (b->flag & 2),
                 "copy: the block has attribute definitions now");
        }
    }
  // again, into model space at its INSBASE; a block MARK is matched
  {
    Dwg_Object_BLOCK_HEADER *ms = model_space (dst);
    const BITCODE_3BD zero = { 0, 0, 0 };
    BITCODE_BL blocks = 0, ms_before = ms->num_owned;
    dst->header_vars.INSBASE.x = 50;
    n = dwg_dynblock_copy_entities (ms, src, model_space (src), &zero,
                                    &error);
    loglevel = test_loglevel;
    for (BITCODE_BL i = 0; i < dst->num_objects; i++)
      if (dst->object[i].fixedtype == DWG_TYPE_BLOCK_HEADER
          && !strcmp (dst->object[i].tio.object->tio.BLOCK_HEADER->name,
                      "MARK"))
        blocks++;
    {
      const Dwg_Object *l1 = nth_entity (dst, model_space (dst), ms_before);
      CHECK (n == 4 && !error && blocks == 1 && l1
                 && l1->fixedtype == DWG_TYPE_LINE
                 && l1->tio.entity->entmode == 2
                 && near (l1->tio.entity->tio.LINE->start.x, 50.0)
                 && near (l1->tio.entity->tio.LINE->start.y, 0.0),
             "copy: into model space at its INSBASE, MARK matched (0x%x)",
             error);
    }
  }
  // the written document reads back
  error = write_dwg (WRITTEN_COPY, dst);
  {
    Dwg_Data back;
    int rerror = error < DWG_ERR_CRITICAL ? read_dwg (WRITTEN_COPY, &back)
                                          : error;
    CHECK (rerror < DWG_ERR_CRITICAL
               && block_header (&back, blk)->num_owned == before + 4,
           "copy: written and read back (0x%x, 0x%x)", error, rerror);
    if (rerror < DWG_ERR_CRITICAL)
      free_dwg (&back);
  }
  free_document (src);
  free_document (dst);
  if (!failures)
    unlink (WRITTEN_COPY);
  return failures;
}

/* Copied into a dynamic block definition, entities are numbered after its
   own; copied from one, they lose its numbers. */
static int
test_copy_numbering (Dwg_Data *fixture_dwg)
{
  BITCODE_RLL sh[4], h[BOX_ENTITIES], blk;
  Dwg_Data *src = box_drawing (sh);
  Dwg_Data *dst = new_document ();
  Dwg_Object_BLOCKSTRETCHACTION *s;
  Dwg_Object_BLOCKMOVEACTION *m;
  const BITCODE_3BD zero = { 0, 0, 0 };
  BITCODE_BL n;
  int error = 0, failures = 0;
  blk = dst ? add_box_block (dst, "BOX", h) : 0;
  if (!src || !blk || !add_width (dst, blk, h, NULL, 0, &s, &m, &error))
    {
      FAIL ("copy numbering: building the drawings failed");
      if (src)
        free_document (src);
      if (dst)
        free_document (dst);
      return 1;
    }
  n = dwg_dynblock_copy_entities (block_header (dst, blk), src,
                                  model_space (src), &zero, &error);
  loglevel = test_loglevel;
  {
    const Dwg_Object_BLOCK_HEADER *b = block_header (dst, blk);
    int numbered = n == 4;
    BITCODE_RS v;
    BITCODE_RL index;
    BITCODE_RLL hv;
    for (BITCODE_BL i = 0; numbered && i < 4; i++)
      {
        const Dwg_Object *e = nth_entity (dst, b, BOX_ENTITIES + i);
        numbered = e && etag (dst, e, &v, &index, &hv)
                   && index == BOX_ENTITIES + i && hv == e->handle.value;
      }
    CHECK (numbered && !error && etag (dst, object (dst, blk), &v, &index, &hv)
               && index == BOX_ENTITIES + 4,
           "copy numbering: numbered 6 to 9 in a definition, the counter 10 "
           "(0x%x)",
           error);
  }
  // the fixture's Trees, whose entities AutoCAD numbered, into a plain
  // block: they keep no AcDbBlockRepETag group
  {
    Dwg_Data *plain = new_document ();
    Dwg_Object_BLOCK_HEADER *pb = dwg_add_BLOCK_HEADER (plain, "PLAIN");
    const Dwg_Object_BLOCK_HEADER *trees = block_header (fixture_dwg, TREES);
    int tagged = 0;
    if (pb && dwg_add_BLOCK (pb, "PLAIN") && dwg_add_ENDBLK (pb))
      {
        BITCODE_RLL pbh = handle_of (plain, pb);
        n = dwg_dynblock_copy_entities (pb, fixture_dwg, trees, &zero,
                                        &error);
        loglevel = test_loglevel;
        pb = block_header (plain, pbh);
        for (BITCODE_BL i = 0; i < pb->num_owned; i++)
          {
            const Dwg_Object *e = nth_entity (plain, pb, i);
            if (e && etag_groups (plain, e))
              tagged++;
          }
        CHECK (n == trees->num_owned && n == pb->num_owned && !tagged
                   && error < DWG_ERR_CRITICAL,
               "copy numbering: Trees' %u entities, read from DWG, copied "
               "without their numbers (%d tagged, 0x%x)",
               (unsigned)n, tagged, error);
      }
    else
      {
        FAIL ("copy numbering: adding PLAIN failed");
        failures++;
      }
    free_document (plain);
  }
  free_document (src);
  free_document (dst);
  return failures;
}

static int
test_copy_refusals (void)
{
  BITCODE_RLL sh[4], h[BOX_ENTITIES], blk;
  Dwg_Data *src = box_drawing (sh);
  Dwg_Data *dst = new_document ();
  Dwg_Data *r2000 = dwg_new_Document (R_2000, 0, tracelevel);
  const BITCODE_3BD zero = { 0, 0, 0 }, inf = { (double)INFINITY, 0, 0 },
                    huge_offset = { DBL_MAX, 0, 0 };
  Dwg_Object_BLOCK_HEADER copy;
  BITCODE_BL num_objects;
  int error = 0, failures = 0;
  blk = dst ? add_box_block (dst, "BOX", h) : 0;
  if (!src || !blk || !r2000)
    {
      FAIL ("copy refusals: building the drawings failed");
      if (src)
        free_document (src);
      if (dst)
        free_document (dst);
      if (r2000)
        free_document (r2000);
      return 1;
    }
  num_objects = dst->num_objects;
  loglevel = test_loglevel;

#define COPY_REFUSES(dstb, s, srcb, off, bits, what)                          \
  do                                                                          \
    {                                                                         \
      error = 0;                                                              \
      loglevel = 0;                                                           \
      CHECK (!dwg_dynblock_copy_entities (dstb, s, srcb, off, &error)         \
                 && error == (bits) && dst->num_objects == num_objects,       \
             "copy refusals: %s (0x%x)", what, error);                        \
    }                                                                         \
  while (0)

  COPY_REFUSES (NULL, src, model_space (src), &zero,
                REFUSED (DWG_ERR_INTERNALERROR), "no block");
  COPY_REFUSES (block_header (dst, blk), src, model_space (src), NULL,
                REFUSED (DWG_ERR_INTERNALERROR), "no offset");
  COPY_REFUSES (block_header (dst, blk), dst, model_space (dst), &zero,
                REFUSED (DWG_ERR_INTERNALERROR), "one document");
  COPY_REFUSES (block_header (dst, blk), r2000, model_space (r2000), &zero,
                REFUSED (DWG_ERR_NOTYETSUPPORTED), "from an r2000 drawing");
  COPY_REFUSES (block_header (dst, blk), src, model_space (dst), &zero,
                REFUSED (DWG_ERR_INVALIDHANDLE),
                "a block of another document");
  COPY_REFUSES (block_header (dst, blk), src,
                dwg_paper_space_object (src)->tio.object->tio.BLOCK_HEADER,
                &zero, REFUSED (DWG_ERR_INVALIDTYPE), "from paper space");
  COPY_REFUSES (dwg_paper_space_object (dst)->tio.object->tio.BLOCK_HEADER,
                src, model_space (src), &zero, REFUSED (DWG_ERR_INVALIDTYPE),
                "into paper space");
  copy = *block_header (dst, blk);
  COPY_REFUSES (&copy, src, model_space (src), &zero,
                REFUSED (DWG_ERR_INVALIDHANDLE),
                "a copy of the block's struct");
  COPY_REFUSES (block_header (dst, blk), src, model_space (src), &inf,
                REFUSED (DWG_ERR_VALUEOUTOFBOUNDS), "an offset not finite");
  {
    // the offset of src's base point is beyond what a double holds
    Dwg_Object_BLOCK_HEADER *b = block_header (dst, blk);
    b->base_pt.x = DBL_MAX;
    COPY_REFUSES (b, src, model_space (src), &huge_offset,
                  REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                  "a base point plus an offset beyond what a double holds");
    b->base_pt.x = 0.0;
  }
  {
    // a LINE out at 0.6 DBL_MAX, moved by 0.6 DBL_MAX
    Dwg_Data *far_src = new_document ();
    const dwg_point_3d a = { 0.6 * DBL_MAX, 0, 0 }, b = { 0, 0, 0 };
    const BITCODE_3BD off = { 0.6 * DBL_MAX, 0, 0 };
    if (far_src && dwg_add_LINE (model_space (far_src), &a, &b))
      COPY_REFUSES (block_header (dst, blk), far_src, model_space (far_src),
                    &off, REFUSED (DWG_ERR_VALUEOUTOFBOUNDS),
                    "a move of a LINE beyond what a double holds");
    else
      {
        FAIL ("copy refusals: the far drawing failed");
        failures++;
      }
    if (far_src)
      free_document (far_src);
  }
  {
    const dwg_point_3d p = { 1, 1, 0 };
    Dwg_Entity_POINT *pt = dwg_add_POINT (model_space (src), &p);
    const BITCODE_3BD off = { 100, 200, 0 }, moved = { 1, 0, 0 };
    BITCODE_BL n;
    loglevel = test_loglevel;
    // the move of src's base point (100, 200) to the block's (0, 0) by
    // (100, 200) is none: a POINT copies as it is
    n = pt ? dwg_dynblock_copy_entities (block_header (dst, blk), src,
                                         model_space (src), &off, &error)
           : 0;
    loglevel = test_loglevel;
    CHECK (n == 5 && !error, "copy refusals: a POINT copies unmoved (0x%x)",
           error);
    num_objects = dst->num_objects;
    COPY_REFUSES (block_header (dst, blk), src, model_space (src), &moved,
                  REFUSED (DWG_ERR_NOTYETSUPPORTED), "a POINT moved");
  }
  loglevel = test_loglevel;
  free_document (src);
  free_document (dst);
  free_document (r2000);
  return failures;
}

/* The fixture's own Trees, as read: its graph is read with its raw bits,
   which the encoder would write as they were read. With the Width it is
   written from its fields, and reads back with the Width's nodes. Changes
   the fixture, so it runs last. */
static int
test_graph_read_from_dwg (Dwg_Data *src)
{
  Dwg_Object_EVALUATION_GRAPH *graph = graph_of (src, TREES);
  Dwg_Object *gobj = graph ? object (src, handle_of (src, graph)) : NULL;
  const BITCODE_3BD b = { -60, 0, 0 }, e = { 60, 0, 0 };
  Dwg_Object_BLOCKLINEARPARAMETER *p;
  Dwg_Data back;
  int error = 0, failures = 0;
  CHECK (gobj && gobj->num_unknown_bits && gobj->unknown_bits,
         "read graph: Trees' graph is read with its raw bits");
  if (!gobj)
    return failures;
  p = dwg_dynblock_add_linear (block_header (src, TREES), "Width", &b, &e,
                               &error);
  loglevel = test_loglevel;
  gobj = object (src, handle_of (src, graph_of (src, TREES)));
  CHECK (p && !error && !gobj->num_unknown_bits && !gobj->unknown_bits,
         "read graph: with the Width, its raw bits are dropped (0x%x)",
         error);
  error = write_dwg (WRITTEN, src);
  CHECK (error < DWG_ERR_CRITICAL, "read graph: the fixture written (0x%x)",
         error);
  error = error < DWG_ERR_CRITICAL ? read_dwg (WRITTEN, &back) : error;
  if (error >= DWG_ERR_CRITICAL && (error & ~KNOWN_ERRORS))
    {
      FAIL ("read graph: read back 0x%x", error);
      return failures + 1;
    }
  {
    Dwg_DynParam *params = NULL;
    BITCODE_BL count = 0;
    int perror = dwg_dynblock_get_params (block_header (&back, TREES), &params,
                                          &count);
    const int same = same_graph (src, TREES, &back, TREES);
    CHECK (same && !perror && count == 2 && !strcmp (params[1].name, "Width")
               && params[1].default_value == 120.0,
           "read graph: Trees reads back with its visibility and the Width "
           "(same graph %d, 0x%x, %u properties)",
           same, perror, (unsigned)count);
    dwg_dynblock_free_params (params, count);
  }
  free_dwg (&back);
  if (!failures)
    unlink (WRITTEN);
  return failures;
}

/* What the functions refuse before anything is added, beyond their
   arguments: a graph that indexes nodes or edges it doesn't have, or with
   a node that isn't a graph element; a length the evaluator takes as
   none; an ATTRIB a stretch would take out of its plane; and moves that
   carry parameters in a cycle, which the evaluator can't order. */
static int
test_preflight (void)
{
  Dwg_Data *dwg = new_document ();
  BITCODE_RLL h[BOX_ENTITIES], blk, pin_ins = 0;
  Dwg_Object_BLOCKLINEARPARAMETER *param;
  Dwg_Object_BLOCKSTRETCHACTION *s;
  Dwg_Object_BLOCKMOVEACTION *m;
  Dwg_Object_EVALUATION_GRAPH *graph;
  BITCODE_BL num_objects;
  int error = 0, failures = 0;
  blk = dwg ? add_box_block (dwg, "BOX", h) : 0;
  param = blk ? add_width (dwg, blk, h, NULL, 0, &s, &m, &error) : NULL;
  if (!param)
    {
      FAIL ("preflight: building the block failed");
      if (dwg)
        free_document (dwg);
      return 1;
    }
  graph = graph_of (dwg, blk);
  num_objects = dwg->num_objects;

  // a node index beyond the edges, then an edge to a node beyond the nodes
  {
    const BITCODE_BLd last_out = graph->nodes[0].node[3];
    const BITCODE_BLd to = graph->edges[0].e3;
    graph->nodes[0].node[3] = 100;
    error = 0;
    loglevel = 0;
    CHECK (!dwg_dynblock_add_move (block_header (dwg, blk), param,
                                   &h[CIRCLE], 1, 0.5, &error)
               && error == DWG_ERR_INVALIDDWG
               && dwg->num_objects == num_objects,
           "preflight: a node naming an edge the graph doesn't have (0x%x)",
           error);
    graph->nodes[0].node[3] = last_out;
    graph->edges[0].e3 = 100;
    error = 0;
    loglevel = 0;
    CHECK (!dwg_dynblock_add_move (block_header (dwg, blk), param,
                                   &h[CIRCLE], 1, 0.5, &error)
               && error == DWG_ERR_INVALIDDWG
               && dwg->num_objects == num_objects,
           "preflight: an edge to a node the graph doesn't have (0x%x)",
           error);
    graph->edges[0].e3 = to;
  }
  // a node that is an entity
  {
    BITCODE_H grip = graph->nodes[1].evalexpr;
    graph->nodes[1].evalexpr = dwg_add_handleref (dwg, 3, h[TOP], NULL);
    error = 0;
    loglevel = 0;
    CHECK (!dwg_dynblock_add_move (block_header (dwg, blk), param,
                                   &h[CIRCLE], 1, 0.5, &error)
               && error == REFUSED (DWG_ERR_INVALIDHANDLE)
               && dwg->num_objects == num_objects,
           "preflight: a node that is a LINE (0x%x)", error);
    graph->nodes[1].evalexpr = grip;
  }
  // node ids at the end of a signed 32-bit integer: none left for the
  // four nodes of a parameter, then none for an action
  {
    const BITCODE_BLd first = graph->first_nodeid;
    const BITCODE_3BD b = { 0, -50, 0 }, e = { 5, -50, 0 };
    graph->first_nodeid = 0x7FFFFFFF - 3;
    error = 0;
    loglevel = 0;
    CHECK (!dwg_dynblock_add_linear (block_header (dwg, blk), "Late", &b, &e,
                                     &error)
               && error == REFUSED (DWG_ERR_VALUEOUTOFBOUNDS)
               && dwg->num_objects == num_objects,
           "preflight: three node ids left for a parameter's four (0x%x)",
           error);
    graph->first_nodeid = 0x7FFFFFFF;
    error = 0;
    loglevel = 0;
    CHECK (!dwg_dynblock_add_move (block_header (dwg, blk), param,
                                   &h[CIRCLE], 1, 0.5, &error)
               && error == REFUSED (DWG_ERR_VALUEOUTOFBOUNDS)
               && dwg->num_objects == num_objects,
           "preflight: no node id left for an action (0x%x)", error);
    graph->first_nodeid = first;
  }
  // a copy of the parameter's struct, which isn't the graph's
  {
    Dwg_Object_BLOCKLINEARPARAMETER copy = *param;
    error = 0;
    loglevel = 0;
    CHECK (!dwg_dynblock_add_move (block_header (dwg, blk), &copy,
                                   &h[CIRCLE], 1, 0.5, &error)
               && error == REFUSED (DWG_ERR_INVALIDHANDLE)
               && dwg->num_objects == num_objects,
           "preflight: a copy of the parameter's struct (0x%x)", error);
  }
  // a length of 5e-10, which the evaluator takes as none
  {
    const BITCODE_3BD b = { 0, 0, 0 }, e = { 5e-10, 0, 0 };
    error = 0;
    loglevel = 0;
    CHECK (!dwg_dynblock_add_linear (block_header (dwg, blk), "Tiny", &b, &e,
                                     &error)
               && error == REFUSED (DWG_ERR_VALUEOUTOFBOUNDS)
               && dwg->num_objects == num_objects,
           "preflight: a length of 5e-10 (0x%x)", error);
  }
  // an INSERT in the frame whose ATTRIB is turned out of the XY plane
  {
    Dwg_Object_BLOCK_HEADER *pin = dwg_add_BLOCK_HEADER (dwg, "PIN");
    const dwg_point_3d o = { 0, 0, 0 }, o2 = { 1, 0, 0 }, at = { 20, -5, 0 };
    Dwg_Entity_INSERT *ins = NULL;
    Dwg_Entity_ATTRIB *attrib = NULL;
    if (pin && dwg_add_BLOCK (pin, "PIN") && dwg_add_LINE (pin, &o, &o2)
        && dwg_add_ENDBLK (pin))
      ins = dwg_add_INSERT_block (block_header (dwg, blk), &at,
                                  block_header (dwg, handle_of (dwg, pin)),
                                  1, 1, 1, 0);
    if (ins)
      attrib = dwg_add_ATTRIB (ins, 1.0, 0, &at, "TAG", "V");
    loglevel = test_loglevel;
    if (!attrib)
      {
        FAIL ("preflight: adding the INSERT and ATTRIB failed");
        failures++;
      }
    else
      {
        pin_ins = handle_of (dwg, ins);
        attrib->extrusion.x = 1.0;
        attrib->extrusion.z = 0.0;
        num_objects = dwg->num_objects;
        error = 0;
        loglevel = 0;
        CHECK (!dwg_dynblock_add_stretch (block_header (dwg, blk), param,
                                          &frame1, &frame2, &pin_ins, 1,
                                          &error)
                   && error == REFUSED (DWG_ERR_NOTYETSUPPORTED)
                   && dwg->num_objects == num_objects,
               "preflight: a stretch of an INSERT whose ATTRIB it would take "
               "out of its plane (0x%x)",
               error);
        // turned a little, under a parameter 2e-9 long: the plane check
        // doesn't depend on its length
        {
          const BITCODE_3BD tb = { 0, -60, 0 }, te = { 2e-9, -60, 0 };
          Dwg_Object_BLOCKLINEARPARAMETER *tiny;
          attrib->extrusion.x = 0.1;
          attrib->extrusion.z = sqrt (0.99);
          loglevel = 0;
          tiny = dwg_dynblock_add_linear (block_header (dwg, blk), "Tiny2",
                                          &tb, &te, &error);
          num_objects = dwg->num_objects;
          error = 0;
          CHECK (tiny
                     && !dwg_dynblock_add_stretch (block_header (dwg, blk),
                                                   tiny, &frame1, &frame2,
                                                   &pin_ins, 1, &error)
                     && error == REFUSED (DWG_ERR_NOTYETSUPPORTED)
                     && dwg->num_objects == num_objects,
                 "preflight: a stretch along a parameter 2e-9 long of an "
                 "INSERT whose ATTRIB leaves its plane (0x%x)",
                 error);
          error = 0;
          loglevel = 0;
          CHECK (tiny
                     && !dwg_dynblock_add_move (block_header (dwg, blk), tiny,
                                                &pin_ins, 1, 1.0, &error)
                     && error == REFUSED (DWG_ERR_NOTYETSUPPORTED)
                     && dwg->num_objects == num_objects,
                 "preflight: a move of it, too (0x%x)", error);
        }
        // turned to face X, under a parameter 1e200 long, whose length's
        // square overflows
        {
          const BITCODE_3BD hb = { 0, -70, 0 }, he = { 1e200, -70, 0 };
          Dwg_Object_BLOCKLINEARPARAMETER *huge;
          attrib->extrusion.x = 1.0;
          attrib->extrusion.z = 0.0;
          loglevel = 0;
          huge = dwg_dynblock_add_linear (block_header (dwg, blk), "Huge",
                                          &hb, &he, &error);
          num_objects = dwg->num_objects;
          error = 0;
          CHECK (huge
                     && !dwg_dynblock_add_stretch (block_header (dwg, blk),
                                                   huge, &frame1, &frame2,
                                                   &pin_ins, 1, &error)
                     && error == REFUSED (DWG_ERR_NOTYETSUPPORTED)
                     && dwg->num_objects == num_objects,
                 "preflight: a stretch along a parameter 1e200 long of an "
                 "INSERT whose ATTRIB faces X (0x%x)",
                 error);
          error = 0;
          loglevel = 0;
          CHECK (huge
                     && !dwg_dynblock_add_move (block_header (dwg, blk), huge,
                                                &pin_ins, 1, 1.0, &error)
                     && error == REFUSED (DWG_ERR_NOTYETSUPPORTED)
                     && dwg->num_objects == num_objects,
                 "preflight: a move of it along the parameter 1e200 long "
                 "(0x%x)",
                 error);
        }
        attrib->extrusion.x = 0.0;
        attrib->extrusion.z = 1.0;
        error = 0;
        loglevel = 0;
        CHECK (dwg_dynblock_add_stretch (block_header (dwg, blk), param,
                                         &frame1, &frame2, &pin_ins, 1,
                                         &error)
                   && !error,
               "preflight: the INSERT with its ATTRIB in the plane (0x%x)",
               error);
      }
  }
  // carries in a cycle: A carries B, B carries C; C carrying A, or B A,
  // closes one
  {
    const BITCODE_3BD a0 = { 0, -20, 0 }, a1 = { 5, -20, 0 },
                      b0 = { 0, -30, 0 }, b1 = { 5, -30, 0 },
                      c0 = { 0, -40, 0 }, c1 = { 5, -40, 0 };
    Dwg_Object_BLOCKLINEARPARAMETER *pa, *pb, *pc;
    BITCODE_RLL ha, hb, hc;
    loglevel = 0;
    pa = dwg_dynblock_add_linear (block_header (dwg, blk), "A", &a0, &a1,
                                  &error);
    pb = dwg_dynblock_add_linear (block_header (dwg, blk), "B", &b0, &b1,
                                  &error);
    pc = dwg_dynblock_add_linear (block_header (dwg, blk), "C", &c0, &c1,
                                  &error);
    loglevel = test_loglevel;
    ha = pa ? handle_of (dwg, pa) : 0;
    hb = pb ? handle_of (dwg, pb) : 0;
    hc = pc ? handle_of (dwg, pc) : 0;
    CHECK (pa && pb && pc
               && dwg_dynblock_add_move (block_header (dwg, blk), pa, &hb, 1,
                                         1.0, &error)
               && dwg_dynblock_add_move (block_header (dwg, blk), pb, &hc, 1,
                                         1.0, &error),
           "preflight: A's move carries B, B's carries C (0x%x)", error);
    num_objects = dwg->num_objects;
    error = 0;
    loglevel = 0;
    CHECK (pb && pc
               && !dwg_dynblock_add_move (block_header (dwg, blk), pb, &ha, 1,
                                          1.0, &error)
               && error == REFUSED (DWG_ERR_NOTYETSUPPORTED)
               && dwg->num_objects == num_objects,
           "preflight: B's move carrying A, which carries B (0x%x)", error);
    error = 0;
    loglevel = 0;
    CHECK (pc
               && !dwg_dynblock_add_move (block_header (dwg, blk), pc, &ha, 1,
                                          1.0, &error)
               && error == REFUSED (DWG_ERR_NOTYETSUPPORTED)
               && dwg->num_objects == num_objects,
           "preflight: C's move carrying A, which reaches C through B "
           "(0x%x)",
           error);
    error = 0;
    loglevel = 0;
    CHECK (pa
           && dwg_dynblock_add_move (block_header (dwg, blk), pa, &hc, 1, 1.0,
                                     &error)
           && !error,
           "preflight: A's move carrying C too, no cycle (0x%x)", error);
  }
  loglevel = test_loglevel;
  free_document (dwg);
  return failures;
}

/* Copies that would draw the block they are copied into: an INSERT of a
   block matched with it, or with a block that draws it. */
static int
test_copy_draws_itself (void)
{
  BITCODE_RLL sh[4], h[BOX_ENTITIES];
  Dwg_Data *src = box_drawing (sh);
  Dwg_Data *dst = new_document ();
  const BITCODE_3BD zero = { 0, 0, 0 };
  BITCODE_RLL mark = 0, target = 0;
  BITCODE_BL num_objects;
  int error = 0, failures = 0;
  // dst's own MARK, and TARGET, which MARK draws
  if (dst)
    {
      mark = add_box_block (dst, "MARK", h);
      target = add_box_block (dst, "TARGET", h);
    }
  if (!src || !mark || !target)
    {
      FAIL ("copy draws itself: building the drawings failed");
      if (src)
        free_document (src);
      if (dst)
        free_document (dst);
      return 1;
    }
  num_objects = dst->num_objects;
  error = 0;
  loglevel = 0;
  CHECK (!dwg_dynblock_copy_entities (block_header (dst, mark), src,
                                      model_space (src), &zero, &error)
             && error == REFUSED (DWG_ERR_INVALIDHANDLE)
             && dst->num_objects == num_objects,
         "copy draws itself: an INSERT of MARK into MARK (0x%x)", error);
  {
    const dwg_point_3d at = { 0, 0, 0 };
    loglevel = 0;
    if (!dwg_add_INSERT_block (block_header (dst, mark), &at,
                               block_header (dst, target), 1, 1, 1, 0))
      {
        FAIL ("copy draws itself: MARK's INSERT of TARGET not added");
        failures++;
      }
  }
  num_objects = dst->num_objects;
  error = 0;
  loglevel = 0;
  CHECK (!dwg_dynblock_copy_entities (block_header (dst, target), src,
                                      model_space (src), &zero, &error)
             && error == REFUSED (DWG_ERR_INVALIDHANDLE)
             && dst->num_objects == num_objects,
         "copy draws itself: an INSERT of MARK, which draws TARGET, into "
         "TARGET (0x%x)",
         error);
  error = 0;
  loglevel = 0;
  CHECK (dwg_dynblock_copy_entities (model_space (dst), src, model_space (src),
                                     &zero, &error)
             == 4
             && !error,
         "copy draws itself: into model space, MARK matched (0x%x)", error);
  loglevel = test_loglevel;
  free_document (src);
  free_document (dst);
  return failures;
}

/* The public Toilet's rotate action turns its entities. A stretch or move
   of a LINE it turns, or a move carrying a parameter it lists or selects,
   is refused, with nothing added, as the evaluator can't order the turn
   and the move; once it turns none of them, all three are made. */
static int
test_turned (Dwg_Data *src)
{
  Dwg_Data *dwg = new_document ();
  Dwg_Object_BLOCK_HEADER *toilet = NULL;
  Dwg_Object_EVALUATION_GRAPH *graph = NULL;
  Dwg_Object_BLOCKROTATEACTION *rotate = NULL;
  Dwg_Object_BLOCKLINEARPARAMETER *width = NULL, *shift = NULL;
  const BITCODE_3BD b0 = { 0, 0, 0 }, e0 = { 10, 0, 0 };
  const BITCODE_3BD b1 = { 0, 5, 0 }, e1 = { 10, 5, 0 };
  const BITCODE_2RD all1 = { -1e6, -1e6 }, all2 = { 1e6, 1e6 };
  BITCODE_RLL turned = 0, shift_h;
  BITCODE_BL num_objects;
  int error = 0, failures = 0;
  if (dwg)
    toilet = dwg_dynblock_copy_block (dwg, src, block_header (src, TOILET),
                                      &error);
  loglevel = test_loglevel;
  if (toilet)
    graph = graph_of (dwg, handle_of (dwg, toilet));
  for (BITCODE_BL i = 0; graph && i < graph->num_nodes && !rotate; i++)
    {
      Dwg_Object *a = node_object (dwg, graph, i);
      if (a && a->fixedtype == DWG_TYPE_BLOCKROTATEACTION)
        rotate = a->tio.object->tio.BLOCKROTATEACTION;
    }
  for (BITCODE_BL i = 0; rotate && rotate->deps && i < rotate->num_deps;
       i++)
    {
      const Dwg_Object *e = object (dwg, ref (rotate->deps[i]));
      if (!turned && e && e->fixedtype == DWG_TYPE_LINE)
        turned = e->handle.value;
    }
  if (turned)
    width = dwg_dynblock_add_linear (toilet, "Toilet Width", &b0, &e0,
                                     &error);
  if (width)
    shift = dwg_dynblock_add_linear (toilet, "Toilet Shift", &b1, &e1,
                                     &error);
  loglevel = test_loglevel;
  if (!shift)
    {
      FAIL ("turned: the Toilet, a LINE its rotate action turns, or its new "
            "parameters not found (0x%x)",
            error);
      if (dwg)
        free_document (dwg);
      return 1;
    }
  shift_h = handle_of (dwg, shift);
  num_objects = dwg->num_objects;
  error = 0;
  loglevel = 0;
  CHECK (!dwg_dynblock_add_stretch (toilet, width, &all1, &all2, &turned, 1,
                                    &error)
             && error == REFUSED (DWG_ERR_NOTYETSUPPORTED)
             && dwg->num_objects == num_objects,
         "turned: a stretch of a LINE the rotate action turns (0x%x)",
         error);
  error = 0;
  loglevel = 0;
  CHECK (!dwg_dynblock_add_move (toilet, width, &turned, 1, 1.0, &error)
             && error == REFUSED (DWG_ERR_NOTYETSUPPORTED)
             && dwg->num_objects == num_objects,
         "turned: a move of it (0x%x)", error);
  {
    BITCODE_BL *actions = rotate->actions;
    const BITCODE_BL num_actions = rotate->num_actions;
    BITCODE_H *deps = rotate->deps;
    const BITCODE_BL num_deps = rotate->num_deps;
    BITCODE_BL shift_id = (BITCODE_BL)shift->evalexpr.nodeid;
    BITCODE_H shift_dep = dwg_add_handleref (dwg, 4, shift_h, NULL);
    // Shift listed among the parameters it turns, then only selected
    rotate->actions = &shift_id;
    rotate->num_actions = 1;
    rotate->deps = NULL;
    rotate->num_deps = 0;
    error = 0;
    loglevel = 0;
    CHECK (!dwg_dynblock_add_move (toilet, width, &shift_h, 1, 1.0, &error)
               && error == REFUSED (DWG_ERR_NOTYETSUPPORTED)
               && dwg->num_objects == num_objects,
           "turned: a move carrying a parameter the rotate action lists "
           "(0x%x)",
           error);
    rotate->actions = NULL;
    rotate->num_actions = 0;
    rotate->deps = &shift_dep;
    rotate->num_deps = 1;
    error = 0;
    loglevel = 0;
    CHECK (!dwg_dynblock_add_move (toilet, width, &shift_h, 1, 1.0, &error)
               && error == REFUSED (DWG_ERR_NOTYETSUPPORTED)
               && dwg->num_objects == num_objects,
           "turned: a move carrying a parameter the rotate action selects "
           "(0x%x)",
           error);
    // turning none of them
    rotate->deps = NULL;
    rotate->num_deps = 0;
    error = 0;
    loglevel = 0;
    CHECK (dwg_dynblock_add_stretch (toilet, width, &all1, &all2, &turned, 1,
                                     &error)
               && !error,
           "turned: the stretch, once the rotate action turns nothing "
           "(0x%x)",
           error);
    error = 0;
    loglevel = 0;
    CHECK (dwg_dynblock_add_move (toilet, width, &turned, 1, 1.0, &error)
               && !error,
           "turned: and the move of the LINE (0x%x)", error);
    error = 0;
    loglevel = 0;
    CHECK (dwg_dynblock_add_move (toilet, width, &shift_h, 1, 1.0, &error)
               && !error,
           "turned: and the move carrying Shift (0x%x)", error);
    rotate->actions = actions;
    rotate->num_actions = num_actions;
    rotate->deps = deps;
    rotate->num_deps = num_deps;
  }
  loglevel = test_loglevel;
  free_document (dwg);
  return failures;
}

/* A selection that points into the document's object list, which adding
   the action's object moves when the list is full: the action lists the
   entities selected, read before the list moved (ASan checks the reads). */
static int
test_selection_in_object_list (void)
{
  Dwg_Data *dwg = new_document ();
  BITCODE_RLL h[BOX_ENTITIES], blk;
  Dwg_Object_BLOCKLINEARPARAMETER *param = NULL;
  Dwg_Object_BLOCKSTRETCHACTION *s;
  Dwg_Object_BLOCKMOVEACTION *m;
  BITCODE_RLL listed;
  int error = 0, failures = 0;
  blk = dwg ? add_box_block (dwg, "BOX", h) : 0;
  if (blk)
    param = dwg_dynblock_add_linear (block_header (dwg, blk), "Width",
                                     &box_base, &box_end, &error);
  loglevel = test_loglevel;
  if (!param)
    {
      FAIL ("object list: building the block failed (0x%x)", error);
      if (dwg)
        free_document (dwg);
      return 1;
    }
  {
    const BITCODE_BL right = object (dwg, h[RIGHT])->index;
    // full, so that dwg_add_object () reallocates it
    dwg->num_alloced_objects = dwg->num_objects;
    error = 0;
    s = dwg_dynblock_add_stretch (block_header (dwg, blk), param, &frame1,
                                  &frame2, &dwg->object[right].handle.value,
                                  1, &error);
    loglevel = test_loglevel;
    listed = s && s->num_deps == 1 && s->num_hdls == 1
                     && ref (s->hdls[0].hdl) == h[RIGHT]
                 ? ref (s->deps[0])
                 : 0;
    CHECK (s && !error && listed == h[RIGHT],
           "object list: the stretch lists the LINE selected (0x%x, "
           FORMAT_HV ")",
           error, listed);
  }
  {
    const BITCODE_BL circle = object (dwg, h[CIRCLE])->index;
    dwg->num_alloced_objects = dwg->num_objects;
    error = 0;
    m = dwg_dynblock_add_move (block_header (dwg, blk), param,
                               &dwg->object[circle].handle.value, 1, 0.5,
                               &error);
    loglevel = test_loglevel;
    listed = m && m->num_deps == 1 ? ref (m->deps[0]) : 0;
    CHECK (m && !error && listed == h[CIRCLE],
           "object list: the move lists the CIRCLE selected (0x%x, "
           FORMAT_HV ")",
           error, listed);
  }
  free_document (dwg);
  return failures;
}

/* A stretch frame whose corners are finite, but their sum isn't: the
   action is displayed at its centre all the same. */
static int
test_far_frame (void)
{
  Dwg_Data *dwg = new_document ();
  Dwg_Object_BLOCK_HEADER *blk = dwg ? dwg_add_BLOCK_HEADER (dwg, "FAR")
                                     : NULL;
  const dwg_point_3d a = { 1.2e308, 0, 0 }, b = { 1.3e308, 0, 0 };
  const BITCODE_2RD c1 = { 1e308, -1 }, c2 = { 1.5e308, 1 };
  Dwg_Entity_LINE *line = NULL;
  Dwg_Object_BLOCKLINEARPARAMETER *param = NULL;
  Dwg_Object_BLOCKSTRETCHACTION *s = NULL;
  BITCODE_RLL h = 0;
  double x = 0.0, y = 0.0;
  int error = 0, failures = 0;
  if (blk && dwg_add_BLOCK (blk, "FAR"))
    line = dwg_add_LINE (blk, &a, &b);
  if (line && dwg_add_ENDBLK (blk))
    param = dwg_dynblock_add_linear (blk, "Width", &box_base, &box_end,
                                     &error);
  loglevel = test_loglevel;
  if (!param)
    {
      FAIL ("far frame: building the block failed (0x%x)", error);
      if (dwg)
        free_document (dwg);
      return 1;
    }
  h = handle_of (dwg, line);
  error = 0;
  s = dwg_dynblock_add_stretch (blk, param, &c1, &c2, &h, 1, &error);
  loglevel = test_loglevel;
  if (s)
    {
      x = s->display_location.x;
      y = s->display_location.y;
    }
  CHECK (s && !error && fabs (x - 1.25e308) <= 1.25e308 * 1e-15 && y == 0.0,
         "far frame: the stretch displayed at the frame's centre, x %g "
         "(0x%x)",
         x, error);
  free_document (dwg);
  return failures;
}

/* CIRCLEs copied with an offset that has a z: one whose extrusion is along
   -Z, its OCS turned over in x and z, moves by the offset in WCS as the
   one along +Z does. */
static int
test_copy_turned_over (void)
{
  Dwg_Data *src = new_document ();
  Dwg_Data *dst = new_document ();
  Dwg_Object_BLOCK_HEADER *b = dst ? dwg_add_BLOCK_HEADER (dst, "TURNED")
                                   : NULL;
  const dwg_point_3d o = { 0, 0, 0 };
  const BITCODE_3BD offset = { 2, 3, 5 };
  Dwg_Entity_CIRCLE *down = NULL, *up = NULL;
  BITCODE_BL n = 0;
  BITCODE_3BD dc = { 0, 0, 0 }, uc = { 0, 0, 0 };
  int error = 0, failures = 0;
  if (src)
    {
      down = dwg_add_CIRCLE (model_space (src), &o, 1.0);
      up = dwg_add_CIRCLE (model_space (src), &o, 1.0);
    }
  if (b && (!dwg_add_BLOCK (b, "TURNED") || !dwg_add_ENDBLK (b)))
    b = NULL;
  loglevel = test_loglevel;
  if (!down || !up || !b)
    {
      FAIL ("turned over: building the drawings failed");
      if (src)
        free_document (src);
      if (dst)
        free_document (dst);
      return 1;
    }
  down->extrusion.z = -1.0;
  n = dwg_dynblock_copy_entities (b, src, model_space (src), &offset,
                                  &error);
  loglevel = test_loglevel;
  if (n == 2)
    {
      const Dwg_Object *d = nth_entity (dst, b, 0);
      const Dwg_Object *u = nth_entity (dst, b, 1);
      if (d && d->fixedtype == DWG_TYPE_CIRCLE)
        dc = d->tio.entity->tio.CIRCLE->center;
      if (u && u->fixedtype == DWG_TYPE_CIRCLE)
        uc = u->tio.entity->tio.CIRCLE->center;
    }
  CHECK (n == 2 && !error && near (dc.x, -2.0) && near (dc.y, 3.0)
             && near (dc.z, -5.0),
         "turned over: the CIRCLE along -Z centred at (%g, %g, %g) in its "
         "OCS, (2, 3, 5) in WCS (0x%x)",
         dc.x, dc.y, dc.z, error);
  CHECK (near (uc.x, 2.0) && near (uc.y, 3.0) && near (uc.z, 5.0),
         "turned over: the CIRCLE along +Z centred at (%g, %g, %g)", uc.x,
         uc.y, uc.z);
  free_document (src);
  free_document (dst);
  return failures;
}

int
main (int argc, char *argv[])
{
  Dwg_Data src;
  int error = 0, failures = 0;
  char *trace = getenv ("LIBREDWG_TRACE");
  test_loglevel = is_make_silent () ? 0 : 2; // print ok
  tracelevel = trace ? atoi (trace) : 0;
  loglevel = test_loglevel;
  (void)argc;
  (void)argv;

  if (!find_fixture ())
    {
      fail ("%s not found", FIXTURE);
      return 1;
    }
  error = read_dwg (fixture, &src);
  if (error & ~KNOWN_ERRORS)
    {
      fail ("dwg_read_file 0x%x", error);
      return 1;
    }
  failures += test_linear ();
  failures += test_linear_model_space ();
  failures += test_numbering ();
  failures += test_linear_refusals (&src);
  failures += test_constraint_based (&src);
  failures += test_actions ();
  failures += test_action_refusals (&src);
  failures += test_preflight ();
  failures += test_carried_parameter ();
  failures += test_autocad_graph (&src);
  failures += test_visibility_states (&src);
  failures += test_lwpolyline_from_dwg (&src);
  failures += test_written ();
  failures += test_locked_attdefs ();
  failures += test_lock_adds_appid ();
  failures += test_copy_entities ();
  failures += test_copy_numbering (&src);
  failures += test_copy_refusals ();
  failures += test_copy_draws_itself ();
  failures += test_turned (&src);
  failures += test_selection_in_object_list ();
  failures += test_far_frame ();
  failures += test_copy_turned_over ();
  failures += test_graph_read_from_dwg (&src);
  free_dwg (&src);
  return failures ? 1 : 0;
}
