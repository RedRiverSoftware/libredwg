/* ex: ft=c: -*- mode: c; -*- */
/*****************************************************************************/
/*  LibreDWG - free implementation of the DWG file format                    */
/*                                                                           */
/*  Copyright (C) 2026 Free Software Foundation, Inc.                        */
/*                                                                           */
/*  This library is free software, licensed under the terms of the GNU       */
/*  General Public License as published by the Free Software Foundation,     */
/*  either version 3 of the License, or (at your option) any later version.  */
/*  You should have received a copy of the GNU General Public License        */
/*  along with this program.  If not, see <http://www.gnu.org/licenses/>.    */
/*****************************************************************************/

/* The dynamic-block objects of test-data/2004/Dynblocks.dwg, which is
   test-data/2018/Dynblocks.dwg saved as r2004 by AutoCAD 2027, with more
   placements of its blocks. Each object decodes as its class. Encoded from
   its fields and decoded again, it has the same fields, and AutoCAD's object
   and data stream sizes, so the decoder read its whole data stream and no
   more. The classes LibreDWG keeps the raw bits of are encoded to AutoCAD's
   bits. Block elements keep the element version they were decoded with when
   written in the same DWG version, and get the version's own otherwise. */

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
#ifndef DISABLE_JSON
#  include "out_json.h"
#endif

#define FIXTURE "test/test-data/2004/Dynblocks.dwg"

/* The fixture's dynamic-block objects: parameters, grips and actions (the
   block elements), grip location components, evaluation graphs, block
   representations and purge preventers. */
#define NUM_DYNBLOCK_OBJECTS 117
#define NUM_BLOCK_ELEMENTS 48
/* Those LibreDWG keeps the raw bits of: 9 stretch actions, 6 evaluation
   graphs, 23 block representations and 6 purge preventers. */
#define NUM_RAW_BITS_OBJECTS 44

/* The element version AutoCAD 2027 writes into r2004 files. */
#define R2004_BE_MAJOR 25
#define R2004_BE_MINOR 104

/* The fixture reads with only the errors of its constraint-based block,
   Door Elevation: its ACDB_DYNAMICBLOCKPROXYNODE is an unhandled class, and
   its AcDbAssocVariable and AcDbAssoc2dConstraintGroup objects read values
   out of bounds. LibreDWG's rewrites of it read with no others. */
#define KNOWN_ERRORS (DWG_ERR_UNHANDLEDCLASS | DWG_ERR_VALUEOUTOFBOUNDS)

/* What an object looked like in the fixture, before it was written. */
typedef struct
{
  BITCODE_RLL handle;
  char *name;
  BITCODE_RL size;
  BITCODE_RL bitsize;
  BITCODE_RL num_unknown_bits;
  BITCODE_TF unknown_bits;
  int is_element;
} Source_Object;

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
is_dynblock_object (const Dwg_Object *obj)
{
  const char *name = obj->name;
  if (obj->supertype != DWG_SUPERTYPE_OBJECT || !name)
    return 0;
  if (!strncmp (name, "BLOCK", 5))
    return strcmp (name, "BLOCK_CONTROL") && strcmp (name, "BLOCK_HEADER");
  return !strcmp (name, "EVALUATION_GRAPH")
         || !strcmp (name, "DYNAMICBLOCKPURGEPREVENTER");
}

static int
is_block_element (const Dwg_Object *obj)
{
  return is_dynblock_object (obj)
         && dwg_dynapi_entity_field (obj->name, "be_major") != NULL;
}

static void *
object_fields (const Dwg_Object *obj)
{
  return obj->tio.object->tio.APPID;
}

// the dynapi calls set loglevel from the object's document
static void
element_version (const Dwg_Object *obj, BITCODE_BL *major, BITCODE_BL *minor)
{
  *major = *minor = 0;
  dwg_dynapi_entity_value (object_fields (obj), obj->name, "be_major", major,
                           NULL);
  dwg_dynapi_entity_value (object_fields (obj), obj->name, "be_minor", minor,
                           NULL);
  loglevel = test_loglevel;
}

static void
set_element_version (Dwg_Object *obj, BITCODE_BL major, BITCODE_BL minor)
{
  dwg_dynapi_entity_set_value (object_fields (obj), obj->name, "be_major",
                               &major, false);
  dwg_dynapi_entity_set_value (object_fields (obj), obj->name, "be_minor",
                               &minor, false);
  loglevel = test_loglevel;
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

static void
free_dwg (Dwg_Data *dwg)
{
  loglevel = tracelevel;
  dwg_free (dwg);
  loglevel = test_loglevel;
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

/* LibreDWG keeps the raw bits of a few classes, such as BLOCKSTRETCHACTION
   and EVALUATION_GRAPH, and writes them back as they are into the same DWG
   version. Without them the object is encoded from its fields. Returns the
   raw bits, which the caller frees. */
static BITCODE_TF
take_unknown_bits (Dwg_Object *obj)
{
  BITCODE_TF bits = obj->unknown_bits;
  obj->unknown_bits = NULL;
  obj->num_unknown_bits = 0;
  return bits;
}

/* Bit i of raw bits as bit_read_bits stores them: whole bytes as read, then
   the bits of a last partial byte from its lowest bit up. */
static int
bit_at (const BITCODE_TF bits, BITCODE_RL num_bits, BITCODE_RL i)
{
  if (i < num_bits / 8 * 8)
    return (bits[i / 8] >> (7 - i % 8)) & 1;
  return (bits[i / 8] >> (i % 8)) & 1;
}

/* Raw bits run to the end of the object's last byte. AutoCAD pads the bits
   after the handle stream with ones, LibreDWG with zeros: the bits are the
   same if they only differ there. */
static int
same_bits_but_padding (const BITCODE_TF autocad, const BITCODE_TF libredwg,
                       BITCODE_RL num_bits)
{
  BITCODE_RL i = 0;
  while (i < num_bits
         && bit_at (autocad, num_bits, i) == bit_at (libredwg, num_bits, i))
    i++;
  if (num_bits - i > 7)
    return 0;
  for (; i < num_bits; i++)
    if (!bit_at (autocad, num_bits, i) || bit_at (libredwg, num_bits, i))
      return 0;
  return 1;
}

static int
is_read_error (int error)
{
  return error >= DWG_ERR_CRITICAL || (error & ~KNOWN_ERRORS);
}

/* The fixture reads with only its known errors, and the proxy node is the
   only object not decoded. Every dynamic-block object decodes as its class,
   and the block elements have the element version AutoCAD 2027 writes. */
static int
test_decode (void)
{
  Dwg_Data dwg;
  unsigned num_dynblock = 0, num_elements = 0, num_unknown = 0;
  unsigned num_other_version = 0;
  int error;

  failed = 0;
  error = read_dwg (fixture, &dwg);
  if (is_read_error (error))
    {
      fail ("decode: dwg_read_file 0x%x", error);
      free_dwg (&dwg);
      return numfailed ();
    }
  ok ("decode: dwg_read_file 0x%x", error);

  for (BITCODE_BL i = 0; i < dwg.num_objects; i++)
    {
      const Dwg_Object *obj = &dwg.object[i];
      if (obj->fixedtype == DWG_TYPE_UNKNOWN_OBJ
          || obj->fixedtype == DWG_TYPE_UNKNOWN_ENT)
        {
          const Dwg_Class *klass = NULL;
          if (obj->type >= 500 && obj->type - 500 < dwg.num_classes)
            klass = &dwg.dwg_class[obj->type - 500];
          num_unknown++;
          if (!klass || !klass->dxfname
              || strcmp (klass->dxfname, "ACDB_DYNAMICBLOCKPROXYNODE"))
            fail ("decode: %s " FORMAT_HV " not decoded",
                  klass && klass->dxfname ? klass->dxfname : obj->name,
                  obj->handle.value);
        }
      if (!is_dynblock_object (obj))
        continue;
      num_dynblock++;
      if (is_block_element (obj))
        {
          BITCODE_BL major, minor;
          num_elements++;
          element_version (obj, &major, &minor);
          if (major != R2004_BE_MAJOR || minor != R2004_BE_MINOR)
            {
              num_other_version++;
              fail ("decode: %s " FORMAT_HV " element version %u/%u",
                    obj->name, obj->handle.value, (unsigned)major,
                    (unsigned)minor);
            }
        }
    }
  if (num_dynblock == NUM_DYNBLOCK_OBJECTS
      && num_elements == NUM_BLOCK_ELEMENTS)
    ok ("decode: %u dynamic-block objects, %u block elements", num_dynblock,
        num_elements);
  else
    fail ("decode: %u dynamic-block objects, %u block elements, expected %u "
          "and %u",
          num_dynblock, num_elements, NUM_DYNBLOCK_OBJECTS,
          NUM_BLOCK_ELEMENTS);
  if (num_unknown == 1)
    ok ("decode: only the proxy node is not decoded");
  else
    fail ("decode: %u objects not decoded, expected 1", num_unknown);
  if (!num_other_version)
    ok ("decode: element version %u/%u", R2004_BE_MAJOR, R2004_BE_MINOR);

  free_dwg (&dwg);
  return numfailed ();
}

#ifndef DISABLE_JSON

/* The JSON text of one object in out_json's OBJECTS array, from its line
   "    {" up to its line "    }". */
typedef struct
{
  BITCODE_RLL handle;
  const char *text;
  size_t len;
} Json_Object;

static char *
write_json (const char *file, Dwg_Data *dwg)
{
  Bit_Chain dat = { 0 };
  struct stat attrib;
  char *text = NULL;
  FILE *fh = fopen (file, "wb");
  if (!fh)
    return NULL;
  dat.fh = fh;
  dat.version = dat.from_version = dwg->header.version;
  dat.codepage = dwg->header.codepage;
  dat.opts = dwg->opts;
  loglevel = tracelevel;
  dwg_write_json (&dat, dwg);
  loglevel = test_loglevel;
  fclose (fh);

  fh = fopen (file, "rb");
  if (fh && !stat (file, &attrib))
    {
      size_t size = (size_t)attrib.st_size;
      text = (char *)malloc (size + 1);
      if (text && fread (text, 1, size, fh) == size)
        text[size] = '\0';
      else
        {
          free (text);
          text = NULL;
        }
    }
  if (fh)
    fclose (fh);
  return text;
}

/* Splits out_json's OBJECTS array into its objects. */
static Json_Object *
json_objects (const char *json, unsigned *num_objects)
{
  const char *p = strstr (json, "\n  \"OBJECTS\": [\n");
  Json_Object *objects = NULL;
  unsigned n = 0, alloced = 0;
  const char *start = NULL;
  BITCODE_RLL handle = 0;

  *num_objects = 0;
  if (!p)
    return NULL;
  p = strchr (p + 1, '\n') + 1;
  while (*p && strncmp (p, "  ]", 3))
    {
      const char *eol = strchr (p, '\n');
      if (!eol)
        break;
      if (!strncmp (p, "    {\n", 6))
        {
          start = p;
          handle = 0;
        }
      else if (!strncmp (p, "      \"handle\": [", 17))
        {
          // [code,size,value]
          const char *end = (const char *)memchr (p, ']', (size_t)(eol - p));
          const char *last = p;
          for (const char *c = p; end && c < end; c++)
            if (*c == ',')
              last = c + 1;
          handle = strtoull (last, NULL, 10);
        }
      else if (start && !strncmp (p, "    }", 5))
        {
          if (n == alloced)
            {
              Json_Object *more;
              alloced = alloced ? 2 * alloced : 1024;
              more = (Json_Object *)realloc (objects,
                                             alloced * sizeof (Json_Object));
              if (!more)
                break;
              objects = more;
            }
          objects[n].handle = handle;
          objects[n].text = start;
          objects[n].len = (size_t)(eol - start);
          n++;
          start = NULL;
        }
      p = eol + 1;
    }
  *num_objects = n;
  return objects;
}

static const Json_Object *
find_json_object (const Json_Object *objects, unsigned num_objects,
                  BITCODE_RLL handle)
{
  for (unsigned i = 0; i < num_objects; i++)
    if (objects[i].handle == handle)
      return &objects[i];
  return NULL;
}

/* Whether one of the object's lines is the given one, after its indent and
   without the comma after a value. */
static int
json_object_has_line (const Json_Object *o, const char *line)
{
  const size_t line_len = strlen (line);
  const char *p = o->text, *end = o->text + o->len;
  while (p < end)
    {
      const char *eol = (const char *)memchr (p, '\n', (size_t)(end - p));
      const char *q = p, *last;
      if (!eol)
        eol = end;
      last = eol;
      while (q < last && *q == ' ')
        q++;
      if (last > q && last[-1] == ',')
        last--;
      if ((size_t)(last - q) == line_len && !memcmp (q, line, line_len))
        return 1;
      p = eol + 1;
    }
  return 0;
}

/* Lines of an object that aren't its fields: its index in the object
   array, its sizes (checked on their own), and raw bits. */
static int
is_skipped_line (const char *line, size_t len)
{
  const char *const keys[] = { "\"index\":", "\"size\":", "\"bitsize\":",
                               "\"num_unknown_bits\":", "\"unknown_bits\":" };
  while (len && *line == ' ')
    {
      line++;
      len--;
    }
  for (size_t i = 0; i < sizeof (keys) / sizeof (keys[0]); i++)
    if (len >= strlen (keys[i]) && !strncmp (line, keys[i], strlen (keys[i])))
      return 1;
  return 0;
}

/* Compares two objects' JSON line by line. Returns the first line of a
   that differs, or NULL. */
static const char *
json_object_diff (const Json_Object *a, const Json_Object *b, size_t *len)
{
  const char *pa = a->text, *enda = a->text + a->len;
  const char *pb = b->text, *endb = b->text + b->len;
  while (pa < enda || pb < endb)
    {
      const char *eola = (const char *)memchr (pa, '\n', (size_t)(enda - pa));
      const char *eolb = (const char *)memchr (pb, '\n', (size_t)(endb - pb));
      size_t lena = eola ? (size_t)(eola - pa) : (size_t)(enda - pa);
      size_t lenb = eolb ? (size_t)(eolb - pb) : (size_t)(endb - pb);
      if (pa < enda && is_skipped_line (pa, lena))
        {
          pa += lena + 1;
          continue;
        }
      if (pb < endb && is_skipped_line (pb, lenb))
        {
          pb += lenb + 1;
          continue;
        }
      if (pa >= enda || pb >= endb || lena != lenb || memcmp (pa, pb, lena))
        {
          *len = pa < enda ? lena : 0;
          return pa < enda ? pa : "(end of object)";
        }
      pa += lena + 1;
      pb += lenb + 1;
    }
  return NULL;
}

#endif /* DISABLE_JSON */

/* Decode the fixture, encode its dynamic-block objects from their fields
   and decode them again: AutoCAD's sizes, AutoCAD's raw bits where the
   source has them, and the same fields. The JSON has the decoded element
   versions. */
static int
test_roundtrip (void)
{
  const char *file = "dynblocks_roundtrip.dwg";
#ifndef DISABLE_JSON
  const char *json_source = "dynblocks_source.json";
  const char *json_written = "dynblocks_roundtrip.json";
  char *json_a = NULL, *json_b = NULL;
  char major_line[32], minor_line[32];
  unsigned num_fields = 0, num_json_elements = 0, num_json_versions = 0;
#endif
  Source_Object source[NUM_DYNBLOCK_OBJECTS];
  unsigned num_source = 0, num_size = 0;
  unsigned num_raw = 0, num_same_raw = 0;
  Dwg_Data dwg, read;
  int error;

  failed = 0;
  memset (source, 0, sizeof (source));
  error = read_dwg (fixture, &dwg);
  if (is_read_error (error))
    {
      fail ("roundtrip: dwg_read_file 0x%x", error);
      free_dwg (&dwg);
      return numfailed ();
    }
  for (BITCODE_BL i = 0; i < dwg.num_objects; i++)
    {
      Dwg_Object *obj = &dwg.object[i];
      Source_Object *src;
      if (!is_dynblock_object (obj))
        continue;
      if (num_source == NUM_DYNBLOCK_OBJECTS)
        {
          fail ("roundtrip: more than %u dynamic-block objects",
                NUM_DYNBLOCK_OBJECTS);
          break;
        }
      src = &source[num_source++];
      src->handle = obj->handle.value;
      src->name = strdup (obj->name);
      src->size = obj->size;
      src->bitsize = obj->bitsize;
      src->is_element = is_block_element (obj);
      src->num_unknown_bits = obj->num_unknown_bits;
      src->unknown_bits = take_unknown_bits (obj);
    }
#ifndef DISABLE_JSON
  json_a = write_json (json_source, &dwg);
#endif
  error = write_dwg (file, &dwg);
  free_dwg (&dwg);
  if (error >= DWG_ERR_CRITICAL)
    {
      fail ("roundtrip: dwg_write_file 0x%x", error);
      goto done;
    }
  error = read_dwg (file, &read);
  if (is_read_error (error))
    {
      fail ("roundtrip: dwg_read_file %s 0x%x", file, error);
      free_dwg (&read);
      goto done;
    }

  for (unsigned i = 0; i < num_source; i++)
    {
      const Source_Object *src = &source[i];
      const Dwg_Object *obj = dwg_resolve_handle (&read, src->handle);
      loglevel = test_loglevel;
      if (!obj || !obj->name || strcmp (obj->name, src->name))
        {
          fail ("roundtrip: %s " FORMAT_HV " missing", src->name,
                src->handle);
          continue;
        }
      if (obj->size == src->size && obj->bitsize == src->bitsize)
        num_size++;
      else
        fail ("roundtrip: %s " FORMAT_HV " size %u bitsize %u, AutoCAD's "
              "%u and %u",
              src->name, src->handle, (unsigned)obj->size,
              (unsigned)obj->bitsize, (unsigned)src->size,
              (unsigned)src->bitsize);
      if (src->num_unknown_bits)
        {
          num_raw++;
          if (obj->num_unknown_bits == src->num_unknown_bits
              && obj->unknown_bits
              && same_bits_but_padding (src->unknown_bits, obj->unknown_bits,
                                        src->num_unknown_bits))
            num_same_raw++;
          else
            fail ("roundtrip: %s " FORMAT_HV " encoded to other bits than "
                  "AutoCAD's",
                  src->name, src->handle);
        }
    }
  if (num_source != NUM_DYNBLOCK_OBJECTS)
    fail ("roundtrip: %u dynamic-block objects, expected %u", num_source,
          NUM_DYNBLOCK_OBJECTS);
  else if (num_size == num_source)
    ok ("roundtrip: %u objects with AutoCAD's sizes", num_size);
  if (num_raw != NUM_RAW_BITS_OBJECTS)
    fail ("roundtrip: %u objects with raw bits, expected %u", num_raw,
          NUM_RAW_BITS_OBJECTS);
  else if (num_same_raw == num_raw)
    ok ("roundtrip: %u objects encoded to AutoCAD's bits", num_raw);

#ifndef DISABLE_JSON
  json_b = write_json (json_written, &read);
  snprintf (major_line, sizeof (major_line), "\"be_major\": %u",
            R2004_BE_MAJOR);
  snprintf (minor_line, sizeof (minor_line), "\"be_minor\": %u",
            R2004_BE_MINOR);
  if (json_a && json_b)
    {
      unsigned num_a, num_b;
      Json_Object *objects_a = json_objects (json_a, &num_a);
      Json_Object *objects_b = json_objects (json_b, &num_b);
      for (unsigned i = 0; i < num_source; i++)
        {
          const Source_Object *src = &source[i];
          const Json_Object *a
              = find_json_object (objects_a, num_a, src->handle);
          const Json_Object *b
              = find_json_object (objects_b, num_b, src->handle);
          const char *diff;
          size_t len = 0;
          if (!a || !b)
            {
              fail ("roundtrip: %s " FORMAT_HV " not in the JSON", src->name,
                    src->handle);
              continue;
            }
          diff = json_object_diff (a, b, &len);
          if (!diff)
            num_fields++;
          else
            fail ("roundtrip: %s " FORMAT_HV " fields differ at %.*s",
                  src->name, src->handle, (int)(len > 200 ? 200 : len),
                  diff);
          if (src->is_element)
            {
              num_json_elements++;
              if (json_object_has_line (a, major_line)
                  && json_object_has_line (a, minor_line))
                num_json_versions++;
              else if (num_json_elements - num_json_versions == 1)
                fail ("roundtrip: %s " FORMAT_HV " element version not in "
                      "the JSON",
                      src->name, src->handle);
            }
        }
      if (num_fields == num_source)
        ok ("roundtrip: %u objects with the same fields", num_fields);
      if (num_json_elements == NUM_BLOCK_ELEMENTS
          && num_json_versions == num_json_elements)
        ok ("roundtrip: %u element versions in the JSON", num_json_versions);
      else
        fail ("roundtrip: %u of %u block elements with their element version "
              "in the JSON",
              num_json_versions, num_json_elements);
      free (objects_a);
      free (objects_b);
    }
  else
    fail ("roundtrip: JSON not written");
#endif
  free_dwg (&read);

done:
#ifndef DISABLE_JSON
  free (json_a);
  free (json_b);
#endif
  for (unsigned i = 0; i < num_source; i++)
    {
      free (source[i].unknown_bits);
      free (source[i].name);
    }
  if (!numfailed ())
    {
      unlink (file);
#ifndef DISABLE_JSON
      unlink (json_source);
      unlink (json_written);
#endif
    }
  return numfailed ();
}

/* Every block element of the fixture with the element version
   major/minor, written as r2004 and read back, has expect_major and
   expect_minor. */
static int
test_element_version (BITCODE_BL major, BITCODE_BL minor,
                      BITCODE_BL expect_major, BITCODE_BL expect_minor,
                      const char *what)
{
  char file[64];
  unsigned num_elements = 0, num_expected = 0;
  Dwg_Data dwg, read;
  int error;

  failed = 0;
  snprintf (file, sizeof (file), "dynblocks_version_%u_%u.dwg",
            (unsigned)major, (unsigned)minor);
  error = read_dwg (fixture, &dwg);
  if (is_read_error (error))
    {
      fail ("%s: dwg_read_file 0x%x", what, error);
      free_dwg (&dwg);
      return numfailed ();
    }
  for (BITCODE_BL i = 0; i < dwg.num_objects; i++)
    {
      Dwg_Object *obj = &dwg.object[i];
      if (!is_dynblock_object (obj))
        continue;
      free (take_unknown_bits (obj));
      if (is_block_element (obj))
        set_element_version (obj, major, minor);
    }
  error = write_dwg (file, &dwg);
  free_dwg (&dwg);
  if (error >= DWG_ERR_CRITICAL)
    {
      fail ("%s: dwg_write_file 0x%x", what, error);
      return numfailed ();
    }
  error = read_dwg (file, &read);
  if (is_read_error (error))
    {
      fail ("%s: dwg_read_file %s 0x%x", what, file, error);
      free_dwg (&read);
      return numfailed ();
    }
  for (BITCODE_BL i = 0; i < read.num_objects; i++)
    {
      const Dwg_Object *obj = &read.object[i];
      BITCODE_BL be_major, be_minor;
      if (!is_block_element (obj))
        continue;
      num_elements++;
      element_version (obj, &be_major, &be_minor);
      if (be_major == expect_major && be_minor == expect_minor)
        num_expected++;
      else if (num_elements - num_expected == 1) // the first one only
        fail ("%s: %s " FORMAT_HV " element version %u/%u, expected %u/%u",
              what, obj->name, obj->handle.value, (unsigned)be_major,
              (unsigned)be_minor, (unsigned)expect_major,
              (unsigned)expect_minor);
    }
  if (num_elements == NUM_BLOCK_ELEMENTS && num_expected == num_elements)
    ok ("%s: %u/%u written as %u/%u", what, (unsigned)major,
        (unsigned)minor, (unsigned)expect_major, (unsigned)expect_minor);
  else
    fail ("%s: %u of %u block elements with %u/%u", what, num_expected,
          num_elements, (unsigned)expect_major, (unsigned)expect_minor);
  free_dwg (&read);
  if (!numfailed ())
    unlink (file);
  return numfailed ();
}

int
main (int argc, char *argv[])
{
  int error = 0;
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
  error += test_decode ();
  error += test_roundtrip ();
  // as AutoCAD 2006 writes it into r2004 files
  error += test_element_version (25, 76, 25, 76, "AutoCAD 2006's version");
  // as a new element has it
  error += test_element_version (0, 0, R2004_BE_MAJOR, R2004_BE_MINOR,
                                 "no version");
  // as decoded from an r2018 file
  error += test_element_version (33, 29, R2004_BE_MAJOR, R2004_BE_MINOR,
                                 "r2018's version");
  return error ? 1 : 0;
}
