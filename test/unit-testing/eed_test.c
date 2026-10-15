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

/* EED as the decoder reads it. A LINE gets EED groups of given bytes, is
   written as r2000 and r2004 and read back:
   - a group's later item is sized by the bytes left in its group: a string
     that claims one byte more is left unread, a binary chunk that does
     fails, and either way the next group is read from the group's end;
   - a byte left over at a group's end is skipped;
   - an item the decoder can't read leaves the rest of its group to the
     raw bytes, while the groups after it and the LINE's own data are still
     read.
   In an r11 file, a string that ends an entity's EED group is read. And in
   public drawings every EED group is read whole: its items take all of its
   bytes. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#define OK(...)                                                               \
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

/* A group read whole: a 1000 string "after" and a 1070 short 7. Strings
   have their length, then the codepage, 30 for ANSI_1252, big-endian. */
static const unsigned char after_group[]
    = { 0, 5, 0, 30, 'a', 'f', 't', 'e', 'r', 70, 7, 0 };

static const dwg_point_3d line_start = { 0.0, 0.0, 0.0 };
static const dwg_point_3d line_end = { 10.0, 5.0, 0.0 };

typedef struct
{
  Dwg_Data *dwg;
  BITCODE_RLL line;   // the LINE's handle
  BITCODE_RLL layer;  // its layer's
  BITCODE_RLL appid1; // the APPIDs of its two groups
  BITCODE_RLL appid2;
} Drawing;

static int
test_file (const char *name, char *path, const size_t size)
{
  struct stat st;
  const char *top_srcdir = getenv ("top_srcdir");
  const char *const prefixes[]
      = { "../test-data", "../../test/test-data", "../../../test/test-data",
          "../../../../test/test-data", NULL };
  if (top_srcdir)
    {
      snprintf (path, size, "%s/test/test-data/%s", top_srcdir, name);
      if (!stat (path, &st))
        return 1;
    }
  for (const char *const *p = prefixes; *p; p++)
    {
      snprintf (path, size, "%s/%s", *p, name);
      if (!stat (path, &st))
        return 1;
    }
  return 0;
}

static int
read_dwg (const char *path, Dwg_Data *dwg)
{
  int error;
  memset (dwg, 0, sizeof (Dwg_Data));
  dwg->opts = tracelevel;
  error = dwg_read_file (path, dwg);
  loglevel = test_loglevel;
  return error;
}

static int
write_dwg (const char *path, Dwg_Data *dwg)
{
  int error;
  // dwg_write_file refuses to overwrite a file
  unlink (path);
  error = dwg_write_file (path, dwg);
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

/* An APPID handle of an EED group: code 5, and as many bytes as its value
   needs. */
static void
set_appid_handle (Dwg_Handle *hdl, const BITCODE_RLL appid)
{
  hdl->code = 5;
  hdl->value = appid;
  hdl->size = 0;
  for (BITCODE_RLL v = appid; v; v >>= 8)
    hdl->size++;
}

/* Appends to ent an EED group of APPID appid that the encoder writes as the
   bytes given. */
static int
add_raw_group (Dwg_Object_Entity *ent, const BITCODE_RLL appid,
               const unsigned char *bytes, const BITCODE_BS size)
{
  Dwg_Eed *eed
      = (Dwg_Eed *)realloc (ent->eed, (ent->num_eed + 1) * sizeof (Dwg_Eed));
  if (!eed)
    return 0;
  ent->eed = eed;
  eed = &ent->eed[ent->num_eed];
  memset (eed, 0, sizeof (Dwg_Eed));
  eed->raw = (BITCODE_TF)malloc (size);
  if (!eed->raw)
    return 0;
  memcpy (eed->raw, bytes, size);
  eed->size = size;
  set_appid_handle (&eed->handle, appid);
  ent->num_eed++;
  return 1;
}

/* A new document with two APPIDs and a LINE, which gets an EED group of
   the bytes given, then after_group. */
static int
new_drawing (const Dwg_Version_Type version, const unsigned char *group,
             const BITCODE_BS size, Drawing *d)
{
  const Dwg_Object *mspace;
  Dwg_Object_APPID *appid1, *appid2;
  Dwg_Entity_LINE *line = NULL;
  const Dwg_Object *obj;

  memset (d, 0, sizeof (Drawing));
  d->dwg = dwg_new_Document (version, 0, tracelevel);
  loglevel = test_loglevel;
  mspace = d->dwg ? dwg_model_space_object (d->dwg) : NULL;
  appid1 = d->dwg ? dwg_add_APPID (d->dwg, "EED_TEST") : NULL;
  appid2 = d->dwg ? dwg_add_APPID (d->dwg, "EED_TEST_2") : NULL;
  if (mspace)
    line = dwg_add_LINE (mspace->tio.object->tio.BLOCK_HEADER, &line_start,
                         &line_end);
  if (!appid1 || !appid2 || !line)
    return 0;
  d->appid1 = d->dwg->object[appid1->parent->objid].handle.value;
  d->appid2 = d->dwg->object[appid2->parent->objid].handle.value;
  obj = &d->dwg->object[line->parent->objid];
  d->line = obj->handle.value;
  d->layer = obj->tio.entity->layer ? obj->tio.entity->layer->absolute_ref : 0;
  return add_raw_group (obj->tio.entity, d->appid1, group, size)
         && add_raw_group (obj->tio.entity, d->appid2, after_group,
                           sizeof (after_group));
}

static void
free_drawing (Drawing *d)
{
  if (d->dwg)
    {
      free_dwg (d->dwg);
      free (d->dwg);
    }
  d->dwg = NULL;
}

static const char *
version_name (const Dwg_Version_Type version)
{
  return version == R_2004 ? "r2004" : "r2000";
}

/* Writes the drawing to file and reads it back into drawn. Returns the
   LINE read back, or NULL. */
static const Dwg_Object *
write_and_read (Drawing *d, const char *file, Dwg_Data *drawn, int *errorp)
{
  const Dwg_Object *obj;
  int error = write_dwg (file, d->dwg);
  if (error >= DWG_ERR_CRITICAL)
    {
      memset (drawn, 0, sizeof (Dwg_Data));
      *errorp = error;
      return NULL;
    }
  *errorp = error = read_dwg (file, drawn);
  if (error >= DWG_ERR_CRITICAL)
    return NULL;
  obj = dwg_resolve_handle (drawn, d->line);
  return obj && obj->fixedtype == DWG_TYPE_LINE ? obj : NULL;
}

/* Whether EED item data is the 8-bit string text. */
static int
is_string (const Dwg_Eed_Data *data, const char *text)
{
  return data && data->code == 0 && !data->u.eed_0.is_tu
         && data->u.eed_0.length == strlen (text)
         && !strcmp (data->u.eed_0.string, text);
}

static int
is_short (const Dwg_Eed_Data *data, const BITCODE_RS value)
{
  return data && data->code == 70 && data->u.eed_70.rs == value;
}

/* Whether the LINE read back has its own data and layer, which follow its
   EED, and after_group as its EED from eed[first] on. */
static int
line_read_whole (const Drawing *d, const Dwg_Object *obj,
                 const BITCODE_BL first)
{
  const Dwg_Object_Entity *ent = obj->tio.entity;
  const Dwg_Entity_LINE *line = ent->tio.LINE;
  return line->start.x == line_start.x && line->start.y == line_start.y
         && line->end.x == line_end.x && line->end.y == line_end.y
         && ent->layer && ent->layer->absolute_ref == d->layer
         && ent->num_eed == first + 2 && ent->eed[first].size
         && ent->eed[first].handle.value == d->appid2
         && is_string (ent->eed[first].data, "after")
         && !ent->eed[first + 1].size
         && is_short (ent->eed[first + 1].data, 7);
}

/* A group's later string that claims one byte more than is left in the
   group: read, it would take the first byte after the group, and the next
   group's size would be read from the byte after that. */
static int
test_string_overrun (const Dwg_Version_Type version)
{
  const char *what = "later EED string one byte longer than its group";
  const char *file = "eed_string_overrun.dwg";
  const unsigned char group[] = { 0,   3,   0,   30, 'a', 'b', 'c',
                                  0,   4,   0,   30, 'x', 'y', 'z' };
  Drawing d;
  Dwg_Data drawn;
  const Dwg_Object *obj = NULL;
  const Dwg_Eed *eed;
  int error = 0;

  failed = 0;
  memset (&drawn, 0, sizeof (drawn));
  if (new_drawing (version, group, sizeof (group), &d))
    obj = write_and_read (&d, file, &drawn, &error);
  if (!obj)
    {
      FAIL ("%s, %s: no LINE read back, error 0x%x", what,
            version_name (version), (unsigned)error);
      goto cleanup;
    }
  eed = obj->tio.entity->eed;
  // left unread: the length kept, no bytes
  if (obj->tio.entity->num_eed >= 2 && eed[0].size == sizeof (group)
      && is_string (eed[0].data, "abc") && !eed[1].size && eed[1].data
      && eed[1].data->code == 0 && eed[1].data->u.eed_0.length == 4
      && !eed[1].data->u.eed_0.string[0])
    OK ("%s, %s: left unread", what, version_name (version));
  else
    FAIL ("%s, %s: not left unread", what, version_name (version));
  if (line_read_whole (&d, obj, 2) && !(error & DWG_ERR_INVALIDEED))
    OK ("%s, %s: the next group and the LINE read", what,
        version_name (version));
  else
    FAIL ("%s, %s: the next group or the LINE misread, error 0x%x", what,
          version_name (version), (unsigned)error);

cleanup:
  free_dwg (&drawn);
  free_drawing (&d);
  if (!numfailed ())
    unlink (file);
  return numfailed ();
}

/* A group's later binary chunk that claims one byte more than is left in
   the group fails, and the rest of the group is skipped. */
static int
test_binary_overrun (const Dwg_Version_Type version)
{
  const char *what = "later EED binary chunk one byte longer than its group";
  const char *file = "eed_binary_overrun.dwg";
  const unsigned char group[]
      = { 0, 3, 0, 30, 'a', 'b', 'c', 4, 3, 0x01, 0x02 };
  Drawing d;
  Dwg_Data drawn;
  const Dwg_Object *obj = NULL;
  const Dwg_Eed *eed;
  int error = 0;

  failed = 0;
  memset (&drawn, 0, sizeof (drawn));
  if (new_drawing (version, group, sizeof (group), &d))
    obj = write_and_read (&d, file, &drawn, &error);
  if (!obj)
    {
      FAIL ("%s, %s: no LINE read back, error 0x%x", what,
            version_name (version), (unsigned)error);
      goto cleanup;
    }
  eed = obj->tio.entity->eed;
  if (obj->tio.entity->num_eed >= 2 && eed[0].size == sizeof (group)
      && eed[0].raw && !memcmp (eed[0].raw, group, sizeof (group))
      && is_string (eed[0].data, "abc") && !eed[1].size && !eed[1].data
      && (error & DWG_ERR_INVALIDEED))
    OK ("%s, %s: not read, its group's bytes kept (error 0x%x)", what,
        version_name (version), (unsigned)error);
  else
    FAIL ("%s, %s: read, or its group's bytes not kept, error 0x%x", what,
          version_name (version), (unsigned)error);
  if (line_read_whole (&d, obj, 2))
    OK ("%s, %s: the next group and the LINE read", what,
        version_name (version));
  else
    FAIL ("%s, %s: the next group or the LINE misread", what,
          version_name (version));

cleanup:
  free_dwg (&drawn);
  free_drawing (&d);
  if (!numfailed ())
    unlink (file);
  return numfailed ();
}

/* An item with a code the decoder doesn't know: the rest of its group is
   left to the group's raw bytes, which a rewrite writes back. The next
   group and the LINE's data after the EED are read as before. */
static int
test_failed_item (const Dwg_Version_Type version)
{
  const char *what = "EED item of an unknown code";
  const char *file = "eed_failed_item.dwg";
  const char *rewritten = "eed_failed_item_rewritten.dwg";
  const unsigned char group[] = { 0, 3, 0, 30, 'a', 'b', 'c', 99, 0, 0 };
  Drawing d;
  Dwg_Data drawn, reread;
  const Dwg_Object *obj = NULL;
  const Dwg_Eed *eed;
  int error = 0;

  failed = 0;
  memset (&drawn, 0, sizeof (drawn));
  memset (&reread, 0, sizeof (reread));
  if (new_drawing (version, group, sizeof (group), &d))
    obj = write_and_read (&d, file, &drawn, &error);
  if (!obj)
    {
      FAIL ("%s, %s: no LINE read back, error 0x%x", what,
            version_name (version), (unsigned)error);
      goto cleanup;
    }
  eed = obj->tio.entity->eed;
  if (obj->tio.entity->num_eed >= 2 && eed[0].size == sizeof (group)
      && eed[0].raw && !memcmp (eed[0].raw, group, sizeof (group))
      && is_string (eed[0].data, "abc") && !eed[1].size && !eed[1].data
      && (error & DWG_ERR_INVALIDEED))
    OK ("%s, %s: not read, its group's bytes kept (error 0x%x)", what,
        version_name (version), (unsigned)error);
  else
    FAIL ("%s, %s: read, or its group's bytes not kept, error 0x%x", what,
          version_name (version), (unsigned)error);
  if (line_read_whole (&d, obj, 2))
    OK ("%s, %s: the next group and the LINE read", what,
        version_name (version));
  else
    FAIL ("%s, %s: the next group or the LINE misread", what,
          version_name (version));

  // a rewrite of what was read writes both groups' bytes
  error = write_dwg (rewritten, &drawn);
  if (error < DWG_ERR_CRITICAL)
    error = read_dwg (rewritten, &reread);
  obj = error < DWG_ERR_CRITICAL ? dwg_resolve_handle (&reread, d.line)
                                 : NULL;
  eed = obj ? obj->tio.entity->eed : NULL;
  if (eed && obj->tio.entity->num_eed >= 2 && eed[0].size == sizeof (group)
      && eed[0].raw && !memcmp (eed[0].raw, group, sizeof (group))
      && line_read_whole (&d, obj, 2))
    OK ("%s, %s: both groups rewritten", what, version_name (version));
  else
    FAIL ("%s, %s: rewritten with other EED, error 0x%x", what,
          version_name (version), (unsigned)error);

cleanup:
  free_dwg (&reread);
  free_dwg (&drawn);
  free_drawing (&d);
  if (!numfailed ())
    {
      unlink (file);
      unlink (rewritten);
    }
  return numfailed ();
}

/* A byte left over at the end of a group, after its later items: no item
   is that short, so it is skipped, and the next group's size is read from
   the group's end. */
static int
test_byte_left_over (const Dwg_Version_Type version)
{
  const char *what = "EED group with a byte left over";
  const char *file = "eed_byte_left_over.dwg";
  const unsigned char group[] = { 0, 3, 0, 30, 'a', 'b', 'c', 70, 9, 0, 0 };
  Drawing d;
  Dwg_Data drawn;
  const Dwg_Object *obj = NULL;
  const Dwg_Eed *eed;
  int error = 0;

  failed = 0;
  memset (&drawn, 0, sizeof (drawn));
  if (new_drawing (version, group, sizeof (group), &d))
    obj = write_and_read (&d, file, &drawn, &error);
  if (!obj)
    {
      FAIL ("%s, %s: no LINE read back, error 0x%x", what,
            version_name (version), (unsigned)error);
      goto cleanup;
    }
  eed = obj->tio.entity->eed;
  if (obj->tio.entity->num_eed >= 2 && eed[0].size == sizeof (group)
      && is_string (eed[0].data, "abc") && !eed[1].size
      && is_short (eed[1].data, 9) && line_read_whole (&d, obj, 2)
      && !(error & DWG_ERR_INVALIDEED))
    OK ("%s, %s: its items, the next group and the LINE read", what,
        version_name (version));
  else
    FAIL ("%s, %s: misread, error 0x%x", what, version_name (version),
          (unsigned)error);

cleanup:
  free_dwg (&drawn);
  free_drawing (&d);
  if (!numfailed ())
    unlink (file);
  return numfailed ();
}

/* An r11 entity has one EED group, of items without a codepage: its APPID
   index (1001) and then the data, as in r11/ACEB10.dwg. A string that ends
   its group, there or as the only item, is read. */
static int
test_r11_groups (void)
{
  const char *what = "r11 EED group ending with a string";
  const char *file = "eed_r11_groups.dwg";
  const unsigned char with_appid[] = { 1, 0, 0, 0, 3, 'a', 'b', 'c' };
  const unsigned char string_only[] = { 0, 3, 'x', 'y', 'z' };
  const unsigned char *groups[] = { with_appid, string_only };
  const BITCODE_BS sizes[] = { sizeof (with_appid), sizeof (string_only) };
  Dwg_Data *dwg = dwg_new_Document (R_11, 0, tracelevel);
  const Dwg_Object *mspace = dwg ? dwg_model_space_object (dwg) : NULL;
  const Dwg_Object *lines[2] = { NULL, NULL };
  Dwg_Data drawn;
  int error = 0, made = 1, n = 0;

  failed = 0;
  loglevel = test_loglevel;
  memset (&drawn, 0, sizeof (drawn));
  for (int k = 0; k < 2 && made; k++)
    {
      Dwg_Entity_LINE *line
          = mspace ? dwg_add_LINE (mspace->tio.object->tio.BLOCK_HEADER,
                                   &line_start, &line_end)
                   : NULL;
      Dwg_Object_Entity *ent
          = line ? dwg->object[line->parent->objid].tio.entity : NULL;
      made = ent && add_raw_group (ent, 0, groups[k], sizes[k]);
      if (made)
        {
          // the extra flags, which say that the entity has EED
          ent->flag_r11 |= FLAG_R11_HAS_PSPACE;
          ent->extra_r11 |= EXTRA_R11_HAS_EED;
        }
    }
  if (made)
    error = write_dwg (file, dwg);
  if (made && error < DWG_ERR_CRITICAL)
    error = read_dwg (file, &drawn);
  // r11 handles are given when read: the LINEs in their order
  for (BITCODE_BL i = 0; made && error < DWG_ERR_CRITICAL
                         && i < drawn.num_objects && n < 2;
       i++)
    if (drawn.object[i].fixedtype == DWG_TYPE_LINE)
      lines[n++] = &drawn.object[i];
  if (n != 2)
    {
      FAIL ("%s: %d LINEs read back, error 0x%x", what, n, (unsigned)error);
      goto cleanup;
    }
  if (lines[0]->tio.entity->num_eed == 2
      && lines[0]->tio.entity->eed[0].data
      && lines[0]->tio.entity->eed[0].data->code == 1
      && lines[0]->tio.entity->eed[0].data->u.eed_1.appid_index == 0
      && is_string (lines[0]->tio.entity->eed[1].data, "abc"))
    OK ("%s, after its APPID index: read", what);
  else
    FAIL ("%s, after its APPID index: misread", what);
  if (lines[1]->tio.entity->num_eed == 1
      && is_string (lines[1]->tio.entity->eed[0].data, "xyz"))
    OK ("%s, as the only item: read", what);
  else
    FAIL ("%s, as the only item: misread", what);

cleanup:
  free_dwg (&drawn);
  if (dwg)
    {
      free_dwg (dwg);
      free (dwg);
    }
  if (!numfailed ())
    unlink (file);
  return numfailed ();
}

/* The bytes an EED item takes in a file of the given version, its code
   included, or 0 for a code that has no size there. */
static size_t
item_file_size (const Dwg_Eed_Data *data, const Dwg_Version_Type version)
{
  switch (data->code)
    {
    case 0:
      if (version < R_13b1) // length [RC], then the string
        return 2 + (size_t)data->u.eed_0.length;
      if (version < R_2007a) // length [RC], codepage [RS], then the string
        return 4 + (size_t)data->u.eed_0.length;
      return 3 + 2 * (size_t)data->u.eed_0_r2007.length;
    case 1:
    case 70:
      return 3;
    case 2:
      return 2;
    case 3:
      return version < R_13b1 ? 3 : 9;
    case 5:
    case 40:
    case 41:
    case 42:
      return 9;
    case 4: // length [RC], then the bytes
      return 2 + (size_t)data->u.eed_4.length;
    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
      return 25;
    case 71:
      return 5;
    default:
      return 0;
    }
}

/* Every EED group of a public drawing is read whole: each of its items
   decoded, and their sizes adding up to the group's. */
static int
test_groups_read_whole (const char *name)
{
  char path[512];
  Dwg_Data drawn;
  BITCODE_BL groups = 0, items = 0;
  int error;

  failed = 0;
  if (!test_file (name, path, sizeof (path)))
    {
      FAIL ("%s not found", name);
      return numfailed ();
    }
  error = read_dwg (path, &drawn);
  if (error >= DWG_ERR_CRITICAL)
    {
      FAIL ("%s: read error 0x%x", name, (unsigned)error);
      free_dwg (&drawn);
      return numfailed ();
    }
  for (BITCODE_BL o = 0; o < drawn.num_objects; o++)
    {
      const Dwg_Object *obj = &drawn.object[o];
      const Dwg_Eed *eed;
      BITCODE_BL num_eed;
      if (obj->supertype == DWG_SUPERTYPE_ENTITY && obj->tio.entity)
        {
          eed = obj->tio.entity->eed;
          num_eed = obj->tio.entity->num_eed;
        }
      else if (obj->supertype == DWG_SUPERTYPE_OBJECT && obj->tio.object)
        {
          eed = obj->tio.object->eed;
          num_eed = obj->tio.object->num_eed;
        }
      else
        continue;
      for (BITCODE_BL first = 0; eed && first < num_eed;)
        {
          BITCODE_BL i = first;
          size_t total = 0;
          int decoded = 1;
          if (!eed[first].size)
            {
              first++;
              continue;
            }
          groups++;
          do
            {
              const size_t size
                  = eed[i].data ? item_file_size (eed[i].data,
                                                  drawn.header.version)
                                : 0;
              if (!size)
                decoded = 0;
              total += size;
              items++;
              i++;
            }
          while (i < num_eed && !eed[i].size);
          if (!decoded || total != eed[first].size)
            FAIL ("%s: %s " FORMAT_HV " EED[%u] group of %u bytes read as "
                  "%u in items%s",
                  name, obj->name, obj->handle.value, (unsigned)first,
                  (unsigned)eed[first].size, (unsigned)total,
                  decoded ? "" : ", not all decoded");
          first = i;
        }
    }
  if (!groups)
    FAIL ("%s: no EED groups", name);
  else if (!numfailed ())
    OK ("%s: %u EED groups read whole, %u items", name, (unsigned)groups,
        (unsigned)items);
  free_dwg (&drawn);
  return numfailed ();
}

int
main (int argc, char *argv[])
{
  const Dwg_Version_Type versions[] = { R_2000, R_2004 };
  const char *const drawings[]
      = { "example_2000.dwg",   "example_2004.dwg", "2004/Dynblocks.dwg",
          "example_r14.dwg",    "example_r13.dwg",  "r11/ACEB10.dwg",
          NULL };
  int error = 0;
  char *trace = getenv ("LIBREDWG_TRACE");
  (void)argc;
  (void)argv;
  test_loglevel = is_make_silent () ? 0 : 2; // print ok
  tracelevel = trace ? atoi (trace) : 0;
  loglevel = test_loglevel;

  for (size_t v = 0; v < sizeof (versions) / sizeof (versions[0]); v++)
    {
      error += test_string_overrun (versions[v]);
      error += test_binary_overrun (versions[v]);
      error += test_failed_item (versions[v]);
      error += test_byte_left_over (versions[v]);
    }
  error += test_r11_groups ();
  for (const char *const *f = drawings; *f; f++)
    error += test_groups_read_whole (*f);
  return error ? 1 : 0;
}
