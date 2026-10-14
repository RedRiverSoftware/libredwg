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

/* src/copy.c, the deep copy of objects with their handles remapped:
   - every object of public r2004 and r2000 drawings copied into an empty
     document, compared field by field with its source, then written and
     read back and compared again;
   - copies within one document;
   - references to objects outside the copy, reported and then resolved;
   - EED, also a string the decoder left unread;
   - linetype dash texts in the linetype's strings_area;
   - refused copies;
   - block definitions, with their dynamic-block data, copied by
     dwg_dynblock_copy_block () into a new document and into a public
     drawing: compared with their sources, written and read back. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>

static int tracelevel;
// the library sets loglevel from each document's opts
static int test_loglevel;

#include "config.h"
#ifdef HAVE_UNISTD_H
#  include <unistd.h>
#endif
/* The copy itself, for its internals and for its spec macros, which copy
   the header variables of the documents the clone test builds. */
#include "copy.c"
/* The block copy, for the map its copies are made through. */
#include "dynblock_copy.c"
#include "tests_common.h"
#include "dwg_api.h"

/* Failures listed per comparison before the rest are only counted:
   COPY_TEST_REPORTED in the environment overrides it. */
static int max_reported = 10;

/* ok and fail print by loglevel, which the library sets from each
   document's opts. */
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

static int
test_file (const char *name, char *path, const size_t size)
{
  struct stat st;
  const char *top_srcdir = getenv ("top_srcdir");
  const char *const prefixes[]
      = { "../test-data", "../../test/test-data", "../../../test/test-data",
          "../../../../test/test-data", NULL };
  if (!stat (name, &st))
    {
      snprintf (path, size, "%s", name);
      return 1;
    }
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

/* The objects copy_test copies of a document: all but those the copy
   refuses. */
static int
is_copyable (const Dwg_Object *obj)
{
  return obj->tio.object && copy_supported (obj->fixedtype)
         && obj->fixedtype != DWG_TYPE_UNKNOWN_ENT
         && obj->fixedtype != DWG_TYPE_UNKNOWN_OBJ;
}

/* An empty document with src's version and header, for copies of all of
   src's objects. Its handles start at src's next one, so that a handle
   left pointing into src stands out. */
static Dwg_Data *
new_document_like (const Dwg_Data *src)
{
  Dwg_Data *dwg = (Dwg_Data *)calloc (1, sizeof (Dwg_Data));
  dwg->opts = src->opts;
  dwg->header = src->header;
  dwg->header.section = NULL;
  dwg->header.section_info = NULL;
  memset (&dwg->header.section_infohdr, 0,
          sizeof (dwg->header.section_infohdr));
  dwg->auxheader = src->auxheader;
  dwg->header_vars.HANDSEED = dwg_add_handleref (
      dwg, 0, src->header_vars.HANDSEED->absolute_ref, NULL);
  return dwg;
}

/* Copies src's header variables into dst through map, which has the
   copies of the objects they refer to. HANDSEED stays dst's own. Returns
   the number of references left unresolved. */
static BITCODE_BL
copy_header_vars (Dwg_Data *dst, Dwg_Data *src, Dwg_Copy_Map *map)
{
  Copy_Chain cdat;
  Bit_Chain *dat = &cdat.dat;
  Bit_Chain *hdl_dat = dat;
  Bit_Chain *str_dat = dat;
  Dwg_Header_Variables *_obj = &dst->header_vars;
  Dwg_Object holder;
  Dwg_Object *obj = &holder;
  Dwg_Data *dwg = dst;
  const BITCODE_H handseed = _obj->HANDSEED;
  int error = 0;
  BITCODE_BL num_unresolved;
  (void)hdl_dat;
  (void)str_dat;
  (void)dwg;
  (void)error;

  memset (&cdat, 0, sizeof (cdat));
  cdat.dat.opts = src->opts;
  cdat.dat.version = cdat.dat.from_version = src->header.from_version;
  cdat.dat.codepage = src->header.codepage;
  cdat.dat.size = 0x7FFFFFFF;
  cdat.src = src;
  cdat.dst = dst;
  cdat.map = map;
  memset (&holder, 0, sizeof (holder));
  holder.parent = dst;
  holder.name = (char *)"header";

  memcpy (_obj, &src->header_vars, sizeof (Dwg_Header_Variables));
  // clang-format off
  #include "header_variables.spec"
  // clang-format on
  // free.c frees these outside the spec
  copy_string (&cdat, (char **)&_obj->DWGCODEPAGE, COPY_TV);
  copy_string (&cdat, (char **)&_obj->HYPERLINKBASE, COPY_T);
  copy_string (&cdat, (char **)&_obj->STYLESHEET, COPY_T);
  copy_string (&cdat, (char **)&_obj->FINGERPRINTGUID, COPY_T);
  copy_string (&cdat, (char **)&_obj->VERSIONGUID, COPY_T);
  copy_string (&cdat, (char **)&_obj->PROJECTNAME, COPY_T);
  copy_string (&cdat, (char **)&_obj->DIMPOST, COPY_T);
  copy_string (&cdat, (char **)&_obj->DIMAPOST, COPY_T);
  copy_string (&cdat, (char **)&_obj->DIMBLK_T, COPY_T);
  copy_string (&cdat, (char **)&_obj->DIMBLK1_T, COPY_T);
  copy_string (&cdat, (char **)&_obj->DIMBLK2_T, COPY_T);
  copy_string (&cdat, (char **)&_obj->MENU, COPY_TV);
  copy_string (&cdat, (char **)&_obj->unit1_name, COPY_TV);
  copy_string (&cdat, (char **)&_obj->unit2_name, COPY_TV);
  copy_string (&cdat, (char **)&_obj->unit3_name, COPY_TV);
  copy_string (&cdat, (char **)&_obj->unit4_name, COPY_TV);
  _obj->HANDSEED = handseed;

  // values (code 0, such as HANDSEED) are reported too
  num_unresolved = 0;
  for (BITCODE_BL i = 0; i < cdat.num_unresolved; i++)
    if (cdat.unresolved[i].code)
      num_unresolved++;
  free (cdat.unresolved);
  free (cdat.slots.keys);
  return num_unresolved;
}

/*--------------------------------------------------------------------------------
 * Field comparison, driven by the dynapi field lists
 */

typedef struct _compare
{
  /* the copy's map: src handles to dst handles; NULL to compare a document
     with one read back from its file */
  const Dwg_Copy_Map *map;
  /* within one document references outside the copy keep their target */
  int same_document;
  /* Doubles read back may differ from those written by less than 1e-12:
     bit_write_DD writes a value that close to its default (for LWPOLYLINE
     points, the previous point) as the default, as a plain dwgrewrite of
     example_2004.dwg shows too. Copies must match exactly. */
  int dd_tolerance;
  int is_tu;
  /* the documents' version, which sizes LTYPE.strings_area */
  Dwg_Version_Type version;
  /* PROXY data last bytes left out of a read-back comparison, see
     unwritten_last_byte () */
  unsigned unwritten;
  /* a read back of a document built with the add API, see
     ADD_API_ACAD_APPID, and the strings with characters outside ASCII
     that differ in it */
  int add_api_readback;
  unsigned non_ascii;
  const char *what;
  int failed;
} Compare;

static void ATTRIBUTE_FORMAT (2, 3)
    compare_fail (Compare *c, const char *fmt, ...)
{
  if (c->failed++ < max_reported && test_loglevel >= 2)
    {
      va_list ap;
      printf ("  # %s: ", c->what);
      va_start (ap, fmt);
      vprintf (fmt, ap);
      va_end (ap);
      printf ("\n");
    }
}

/* The handle a reference to target in a should have in b. */
static BITCODE_RLL
compare_target (const Compare *c, const BITCODE_RLL target)
{
  BITCODE_RLL mapped;
  if (!c->map || !target)
    return target;
  mapped = dwg_copy_map_get (c->map, target);
  if (!mapped && c->same_document)
    return target;
  return mapped;
}

static void
compare_handle (Compare *c, const char *name, const BITCODE_RLL a,
                const BITCODE_RLL b)
{
  const BITCODE_RLL expected = compare_target (c, a);
  if (expected != b)
    compare_fail (c, "%s: " FORMAT_HV ", expected " FORMAT_HV " (from " FORMAT_HV ")",
                  name, b, expected, a);
}

static void
compare_ref (Compare *c, const char *name, const Dwg_Object_Ref *a,
             const Dwg_Object_Ref *b)
{
  const BITCODE_RLL ta = a ? a->absolute_ref : 0;
  const BITCODE_RLL tb = b ? b->absolute_ref : 0;
  // code 0 handles are values, which keep their value unless they name a
  // copied object
  if (a && !a->handleref.code && c->map && !dwg_copy_map_get (c->map, ta))
    {
      if (ta != tb)
        compare_fail (c, "%s: value " FORMAT_HV ", expected " FORMAT_HV,
                      name, tb, ta);
      return;
    }
  compare_handle (c, name, ta, tb);
}

static int
has_non_ascii (const char *s)
{
  for (; s && *s; s++)
    if ((unsigned char)*s >= 0x80)
      return 1;
  return 0;
}

static void
compare_string (Compare *c, const char *name, const char *a, const char *b,
                const int wide)
{
  size_t la, lb;
  if (wide)
    {
      la = a ? bit_wcs2len ((BITCODE_TU)a) * 2 : 0;
      lb = b ? bit_wcs2len ((BITCODE_TU)b) * 2 : 0;
    }
  else
    {
      la = a ? strlen (a) : 0;
      lb = b ? strlen (b) : 0;
    }
  if ((la != lb || (la && memcmp (a, b, la))) && c->add_api_readback && !wide
      && has_non_ascii (a))
    {
      // see ADD_API_ACAD_APPID
      if (c->non_ascii++ < (unsigned)max_reported && test_loglevel >= 2)
        printf ("  # TODO %s: %s: \"%s\", expected \"%s\"\n", c->what, name,
                b ? b : "", a);
    }
  else if (la != lb || (la && memcmp (a, b, la)))
    compare_fail (c, "%s: \"%s\", expected \"%s\"", name,
                  b && !wide ? b : "", a && !wide ? a : "");
  else if (c->map && a && a == b)
    compare_fail (c, "%s: shares its string with the source", name);
}

static int
is_string_type (const char *type)
{
  return strEQc (type, "T") || strEQc (type, "TV") || strEQc (type, "TU")
         || strEQc (type, "T16") || strEQc (type, "TU16")
         || strEQc (type, "T32") || strEQc (type, "D2T");
}

static int
is_wide_type (const Compare *c, const char *type)
{
  return strEQc (type, "TU") || strEQc (type, "TU16")
         || (c->is_tu
             && (strEQc (type, "T") || strEQc (type, "T16")
                 || strEQc (type, "T32")));
}

/* Types made of doubles only, compared with ==, so that -0.0 equals 0.0 as
   the encoder writes it. */
static int
is_double_type (const char *type)
{
  const char *const types[]
      = { "BD",  "RD",      "2RD",     "3BD",   "3RD",   "2BD",  "3DPOINT",
          "2DPOINT", "BE",  "3BD_1",   "2BD_1", "DD",    "BT",   NULL };
  for (const char *const *t = types; *t; t++)
    if (strEQ (type, *t))
      return 1;
  return 0;
}

/* The size of a scalar or point type, 0 for others. */
static size_t
scalar_size (const char *type)
{
  const struct
  {
    const char *type;
    size_t size;
  } sizes[] = { { "B", 1 },
                { "BB", 1 },
                { "3B", 1 },
                { "4BITS", 1 },
                { "RC", 1 },
                { "RCd", 1 },
                { "RCx", 1 },
                { "BS", 2 },
                { "BSd", 2 },
                { "BSx", 2 },
                { "RS", 2 },
                { "RSd", 2 },
                { "RSx", 2 },
                { "BL", 4 },
                { "BLd", 4 },
                { "BLx", 4 },
                { "RL", 4 },
                { "RLd", 4 },
                { "RLx", 4 },
                { "MS", 4 },
                { "int", sizeof (int) },
                { "int32_t", sizeof (int32_t) },
                { "unsigned long", sizeof (unsigned long) },
                { "BLL", 8 },
                { "RLL", 8 },
                { "RLLd", 8 },
                { "BD", 8 },
                { "RD", 8 },
                { "BT", 8 },
                { "DD", 8 },
                { "2RD", 16 },
                { "2BD", 16 },
                { "2BD_1", 16 },
                { "2DPOINT", 16 },
                { "3BD", 24 },
                { "3BD_1", 24 },
                { "3RD", 24 },
                { "3DPOINT", 24 },
                { "BE", 24 },
                { NULL, 0 } };
  for (int i = 0; sizes[i].type; i++)
    if (strEQ (type, sizes[i].type))
      return sizes[i].size;
  return 0;
}

static int
same_double (const double a, const double b, const int dd_tolerance)
{
  if (a == b || (isnan (a) && isnan (b)))
    return 1;
  return dd_tolerance && fabs (a - b) < 1e-12;
}

static void
compare_doubles (Compare *c, const char *name, const double *a,
                 const double *b, const size_t n)
{
  for (size_t i = 0; i < n; i++)
    if (!same_double (a[i], b[i], c->dd_tolerance))
      {
        compare_fail (c, "%s[%u]: %.17g, expected %.17g", name, (unsigned)i,
                      b[i], a[i]);
        return;
      }
}

static void
compare_color (Compare *c, const char *name, const Dwg_Color *a,
               const Dwg_Color *b)
{
  char sub[128];
  if (a->index != b->index || a->flag != b->flag || a->rgb != b->rgb
      || a->alpha_type != b->alpha_type || a->alpha != b->alpha)
    compare_fail (c, "%s: color %d/0x%x/0x%x, expected %d/0x%x/0x%x", name,
                  b->index, b->flag, b->rgb, a->index, a->flag, a->rgb);
  snprintf (sub, sizeof (sub), "%s.name", name);
  compare_string (c, sub, a->name, b->name, c->is_tu);
  snprintf (sub, sizeof (sub), "%s.book_name", name);
  compare_string (c, sub, a->book_name, b->book_name, c->is_tu);
  snprintf (sub, sizeof (sub), "%s.handle", name);
  compare_ref (c, sub, a->handle, b->handle);
}

/* A struct being compared, a with b: for the fields of the structs inside
   it, whose lengths it may hold. */
typedef struct _outer
{
  const Dwg_DYNAPI_field *fields;
  const void *a;
  const void *b;
} Outer;

static void compare_fields (Compare *c, const char *prefix,
                            const char *structname,
                            const Dwg_DYNAPI_field *fields, const void *a,
                            const void *b, const Outer *outer);

static const Dwg_DYNAPI_field *
find_field (const Dwg_DYNAPI_field *fields, const char *name)
{
  for (const Dwg_DYNAPI_field *f = fields; f->name; f++)
    if (strEQ (f->name, name))
      return f;
  return NULL;
}

static long
int_value (const Dwg_DYNAPI_field *fields, const char *name, const void *base)
{
  const Dwg_DYNAPI_field *f = find_field (fields, name);
  const char *p;
  if (!f)
    return 0;
  p = (const char *)base + f->offset;
  switch (f->size)
    {
    case 1:
      return *(const unsigned char *)p;
    case 2:
      return *(const int16_t *)p;
    case 4:
      return *(const int32_t *)p;
    default:
      return 0;
    }
}

/* Whether field is a member of a union in structname other than the one in
   use, which holds another member's bytes. The dynapi lists have every
   member. */
static int
inactive_member (const char *structname, const Dwg_DYNAPI_field *fields,
                 const char *field, const void *base)
{
  if (strEQc (structname, "EvalExpr") && memBEGINc (field, "value."))
    {
      const char *active;
      switch (int_value (fields, "value_code", base))
        {
        case 40:
          active = "value.num40";
          break;
        case 10:
          active = "value.pt2d";
          break;
        case 11:
          active = "value.pt3d";
          break;
        case 1:
          active = "value.text1";
          break;
        case 90:
          active = "value.long90";
          break;
        case 91:
          active = "value.handle91";
          break;
        case 70:
          active = "value.short70";
          break;
        default:
          return 1;
        }
      return strNE (field, active);
    }
  if (strEQc (structname, "EvalVariant") && memBEGINc (field, "u."))
    {
      const long code = int_value (fields, "code", base);
      const char *active;
      switch (code ? dwg_resbuf_value_type ((short)code) : DWG_VT_INVALID)
        {
        case DWG_VT_REAL:
          active = "u.bd";
          break;
        case DWG_VT_INT32:
          active = "u.bl";
          break;
        case DWG_VT_INT16:
          active = "u.bs";
          break;
        case DWG_VT_INT8:
          active = "u.rc";
          break;
        case DWG_VT_STRING:
          active = "u.text";
          break;
        case DWG_VT_HANDLE:
          active = "u.handle";
          break;
        default:
          return 1;
        }
      return strNE (field, active);
    }
  return 0;
}

/* The union MLEADER_AnnotContext.content holds text or a block, as its
   flags say: compares that member only. */
static void
compare_mleader_content (Compare *c, const char *name, const Outer *self,
                         const void *pa, const void *pb)
{
  char member[180];
  if (int_value (self->fields, "has_content_txt", self->a))
    {
      snprintf (member, sizeof (member), "%s.txt", name);
      compare_fields (c, member, "MLEADER_Content_MText",
                      dwg_dynapi_subclass_fields ("MLEADER_Content_MText"),
                      pa, pb, self);
    }
  else if (int_value (self->fields, "has_content_blk", self->a))
    {
      snprintf (member, sizeof (member), "%s.blk", name);
      compare_fields (c, member, "MLEADER_Content_Block",
                      dwg_dynapi_subclass_fields ("MLEADER_Content_Block"),
                      pa, pb, self);
    }
}

/* The dynapi fields of a struct type named after its Dwg_ prefix: a
   subclass, or an object embedded in another (LAYOUT's PLOTSETTINGS), whose
   name without Object_ or Entity_ goes to *structnamep. */
static const Dwg_DYNAPI_field *
struct_fields (const char *name, const char **structnamep)
{
  const Dwg_DYNAPI_field *fields = dwg_dynapi_subclass_fields (name);
  *structnamep = name;
  if (fields)
    return fields;
  if (memBEGINc (name, "Object_"))
    *structnamep = &name[strlen ("Object_")];
  else if (memBEGINc (name, "Entity_"))
    *structnamep = &name[strlen ("Entity_")];
  else
    return NULL;
  return dwg_dynapi_entity_fields (*structnamep);
}

/* A value of the given type: a field, an element of an array, or an
   inlined struct, which outer holds. */
static void
compare_value (Compare *c, const char *name, const char *type, const void *a,
               const void *b, const Outer *outer)
{
  if (strEQc (type, "H"))
    compare_ref (c, name, *(const BITCODE_H *)a, *(const BITCODE_H *)b);
  else if (is_string_type (type))
    compare_string (c, name, *(char *const *)a, *(char *const *)b,
                    is_wide_type (c, type));
  else if (strEQc (type, "CMC") || strEQc (type, "CMTC")
           || strEQc (type, "ENC"))
    compare_color (c, name, (const Dwg_Color *)a, (const Dwg_Color *)b);
  else if (is_double_type (type))
    compare_doubles (c, name, (const double *)a, (const double *)b,
                     scalar_size (type) / 8);
  else if (scalar_size (type))
    {
      if (memcmp (a, b, scalar_size (type)))
        compare_fail (c, "%s [%s] differs", name, type);
    }
  else if (memBEGINc (type, "Dwg_"))
    {
      const char *structname;
      const Dwg_DYNAPI_field *sub = struct_fields (&type[4], &structname);
      if (sub)
        compare_fields (c, name, structname, sub, a, b, outer);
      else
        compare_fail (c, "%s [%s]: no dynapi fields to compare", name, type);
    }
  else
    compare_fail (c, "%s [%s]: a type the test can't compare", name, type);
}

/* The value of an unsigned count field. */
static uint64_t
count_value (const Dwg_DYNAPI_field *f, const void *base)
{
  const char *p = (const char *)base + f->offset;
  switch (f->size)
    {
    case 1:
      return *(const unsigned char *)p;
    case 2:
      return *(const uint16_t *)p;
    case 4:
      return *(const uint32_t *)p;
    case 8:
      return *(const uint64_t *)p;
    default:
      return 0;
    }
}

/* The field named like name with prefix before its last component:
   cellstyle.borders has cellstyle.num_borders. */
static const Dwg_DYNAPI_field *
prefixed_field (const Dwg_DYNAPI_field *fields, const char *name,
                const char *prefix, const char *suffix)
{
  char field[120];
  const char *dot = strrchr (name, '.');
  const int head = dot ? (int)(dot - name + 1) : 0;
  snprintf (field, sizeof (field), "%.*s%s%s%s", head, name, prefix,
            &name[head], suffix);
  return find_field (fields, field);
}

/* Binary chunks, compared byte by byte. */
static int
is_binary_type (const char *type)
{
  return strEQc (type, "TF") || strEQc (type, "RC*");
}

/* How an array or binary chunk whose length is not in a num_<name> or
   <name>_size field of its struct gets it. */
typedef enum LENGTH_RULE
{
  LENGTH_FIELD,       /* elements, in count */
  LENGTH_OUTER_FIELD, /* elements, in count of the struct holding it */
  LENGTH_BITS,        /* bits, in count */
  LENGTH_FIXED,       /* fixed elements */
  LENGTH_STRING,      /* a NUL-terminated string in a binary field */
  LENGTH_STRINGS_AREA /* LTYPE's: 256 bytes, 512 from r2007 */
} Length_Rule;

typedef struct _array_length
{
  const char *structname; /* NULL for any */
  const char *field;
  Length_Rule rule;
  const char *count;
  unsigned fixed;
} Array_Length;

/* The arrays and binary chunks of dwg.spec and dwg2.spec whose length is
   not in a num_<name> or <name>_size field: as the specs read them. The
   ACIS data of 3DSOLID and the like is in acis_length (). */
static const Array_Length array_lengths[] = {
  { "BLOCK_HEADER", "entities", LENGTH_FIELD, "num_owned", 0 },
  { "INSERT", "attribs", LENGTH_FIELD, "num_owned", 0 },
  { "MINSERT", "attribs", LENGTH_FIELD, "num_owned", 0 },
  { "TABLE", "attribs", LENGTH_FIELD, "num_owned", 0 },
  { "POLYLINE_2D", "vertex", LENGTH_FIELD, "num_owned", 0 },
  { "POLYLINE_3D", "vertex", LENGTH_FIELD, "num_owned", 0 },
  { "POLYLINE_MESH", "vertex", LENGTH_FIELD, "num_owned", 0 },
  { "POLYLINE_PFACE", "vertex", LENGTH_FIELD, "num_owned", 0 },
  { "DICTIONARY", "texts", LENGTH_FIELD, "numitems", 0 },
  { "DICTIONARY", "itemhandles", LENGTH_FIELD, "numitems", 0 },
  { "DICTIONARYWDFLT", "texts", LENGTH_FIELD, "numitems", 0 },
  { "DICTIONARYWDFLT", "itemhandles", LENGTH_FIELD, "numitems", 0 },
  { "SORTENTSTABLE", "sort_ents", LENGTH_FIELD, "num_ents", 0 },
  { "LTYPE", "dashes", LENGTH_FIELD, "numdashes", 0 },
  { "LTYPE", "strings_area", LENGTH_STRINGS_AREA, NULL, 0 },
  { "TABLE", "col_widths", LENGTH_FIELD, "num_cols", 0 },
  { "TABLE", "row_heights", LENGTH_FIELD, "num_rows", 0 },
  { "BLOCKLOOKUPACTION", "lut", LENGTH_FIELD, "numelems", 0 },
  { "BLOCKLOOKUPACTION", "exprs", LENGTH_FIELD, "numelems", 0 },
  { "BLOCKPOLARSTRETCHACTION", "shorts", LENGTH_FIELD, "num_hdls", 0 },
  /* one or the other, as the path's flag says */
  { "HATCH_Path", "segs", LENGTH_FIELD, "num_segs_or_paths", 0 },
  { "HATCH_Path", "polyline_paths", LENGTH_FIELD, "num_segs_or_paths", 0 },
  { "DATATABLE_column", "rows", LENGTH_OUTER_FIELD, "num_rows", 0 },
  { "PROXY_ENTITY", "data", LENGTH_BITS, "data_numbits", 0 },
  { "PROXY_OBJECT", "data", LENGTH_BITS, "data_numbits", 0 },
  { "OLE2FRAME", "oleclient", LENGTH_STRING, NULL, 0 },
  { "DIMASSOC", "ref", LENGTH_FIXED, NULL, 6 },
  { "SPATIAL_FILTER", "inverse_transform", LENGTH_FIXED, NULL, 12 },
  { "SPATIAL_FILTER", "transform", LENGTH_FIXED, NULL, 12 },
  { "MLEADER_Content_Block", "transform", LENGTH_FIXED, NULL, 16 },
  { "ACSH_HistoryNode", "trans", LENGTH_FIXED, NULL, 16 },
  { NULL, "prop_states", LENGTH_FIXED, NULL, 4 },
  { NULL, "transmatrix", LENGTH_FIXED, NULL, 16 },
  { NULL, "rel_transform", LENGTH_FIXED, NULL, 16 },
  { NULL, "sweep_entity_transmatrix", LENGTH_FIXED, NULL, 16 },
  { NULL, "path_entity_transmatrix", LENGTH_FIXED, NULL, 16 },
  { NULL, "sweep_transmatrix", LENGTH_FIXED, NULL, 16 },
  { NULL, "loft_entity_transmatrix", LENGTH_FIXED, NULL, 16 },
  { NULL, "revolved_entity_transmatrix", LENGTH_FIXED, NULL, 16 },
  { NULL, "sweepentity_transform", LENGTH_FIXED, NULL, 16 },
  { NULL, "pathentity_transform", LENGTH_FIXED, NULL, 16 },
  { NULL, NULL, LENGTH_FIXED, NULL, 0 }
};

static const Array_Length *
find_length_rule (const char *structname, const char *field)
{
  const Array_Length *any = NULL;
  for (const Array_Length *r = array_lengths; r->field; r++)
    if (strEQ (r->field, field))
      {
        if (r->structname && strEQ (r->structname, structname))
          return r;
        if (!r->structname)
          any = r;
      }
  return any;
}

/* The ACIS data of 3DSOLID and the classes that share its fields. Version 1
   has num_blocks encrypted blocks, of block_size[i] bytes, and an empty one
   after them, and acis_data holds their decrypted text; version 2 has a
   block_size of 2 and sab_size bytes of acis_data. */
static int
is_acis_struct (const Dwg_DYNAPI_field *fields)
{
  return find_field (fields, "num_blocks") && find_field (fields, "sab_size")
         && find_field (fields, "encr_sat_data");
}

static int64_t
acis_length (const Dwg_DYNAPI_field *fields, const char *field,
             const void *base)
{
  const char *p = (const char *)base;
  const uint64_t version = count_value (find_field (fields, "version"), p);
  const uint64_t num_blocks
      = count_value (find_field (fields, "num_blocks"), p);
  const BITCODE_BL *block_size = *(
      BITCODE_BL *const *)(p + find_field (fields, "block_size")->offset);
  const unsigned char *acis_data = *(
      unsigned char *const *)(p + find_field (fields, "acis_data")->offset);
  int64_t size = 0;
  if (strEQc (field, "block_size"))
    return version == 2 ? 2 : (int64_t)num_blocks + 1;
  if (strEQc (field, "encr_sat_data"))
    return (int64_t)num_blocks + 1;
  if (strNE (field, "acis_data"))
    return -1;
  if (version == 2)
    size = (int64_t)count_value (find_field (fields, "sab_size"), p);
  else if (block_size)
    for (uint64_t i = 0; i < num_blocks; i++)
      size += block_size[i];
  if (!size && acis_data)
    size = (int64_t)strlen ((const char *)acis_data);
  return size;
}

/* The length of array or binary field f of the struct at base, in elements
   or bytes, or -1 when the test has no rule for it. outer_base is the
   struct holding it, if any. */
static int64_t
field_length (const Compare *c, const char *structname,
              const Dwg_DYNAPI_field *fields, const Dwg_DYNAPI_field *f,
              const void *base, const Outer *outer, const void *outer_base)
{
  const Array_Length *rule = find_length_rule (structname, f->name);
  const Dwg_DYNAPI_field *count;
  if (is_acis_struct (fields)
      && (strEQc (f->name, "block_size") || strEQc (f->name, "encr_sat_data")
          || strEQc (f->name, "acis_data")))
    return acis_length (fields, f->name, base);
  if (rule)
    {
      const void *field_base = base;
      const Dwg_DYNAPI_field *count_fields = fields;
      if (rule->rule == LENGTH_FIXED)
        return rule->fixed;
      if (rule->rule == LENGTH_STRINGS_AREA)
        return c->version <= R_2004 ? 256 : 512;
      if (rule->rule == LENGTH_STRING)
        {
          const char *str
              = *(const char *const *)((const char *)base + f->offset);
          return str ? (int64_t)strlen (str) : 0;
        }
      if (rule->rule == LENGTH_OUTER_FIELD)
        {
          if (!outer)
            return -1;
          field_base = outer_base;
          count_fields = outer->fields;
        }
      count = find_field (count_fields, rule->count);
      if (!count)
        return -1;
      if (rule->rule == LENGTH_BITS)
        return (int64_t)((count_value (count, field_base) + 7) / 8);
      return (int64_t)count_value (count, field_base);
    }
  count = prefixed_field (fields, f->name, "num_", "");
  if (count)
    return (int64_t)count_value (count, base);
  if (is_binary_type (f->type))
    {
      count = prefixed_field (fields, f->name, "", "_size");
      if (count)
        return (int64_t)count_value (count, base);
    }
  return -1;
}

static size_t
element_size (const char *type)
{
  if (strEQc (type, "H") || is_string_type (type))
    return sizeof (void *);
  if (scalar_size (type))
    return scalar_size (type);
  if (memBEGINc (type, "Dwg_"))
    return (size_t)dwg_dynapi_fields_size (&type[4]);
  return 0;
}

static void
compare_bytes (Compare *c, const char *name, const unsigned char *a,
               const unsigned char *b, const size_t len)
{
  for (size_t i = 0; i < len; i++)
    if (a[i] != b[i])
      {
        compare_fail (c, "%s: byte %" PRIuSIZE " of %" PRIuSIZE
                      " is 0x%02x, expected 0x%02x",
                      name, i, len, b[i], a[i]);
        return;
      }
}

/* bit_write_bits writes the bits of a partial last byte in the reverse of
   the order bit_read_bits reads them into it, so the data_numbits % 8 last
   bits of PROXY_ENTITY and PROXY_OBJECT data don't read back as written,
   in a plain rewrite of a drawing too. Read back, that byte is left out of
   the comparison, and counted. */
static int
unwritten_last_byte (const Compare *c, const char *structname,
                     const Dwg_DYNAPI_field *f, const Outer *self)
{
  const Dwg_DYNAPI_field *numbits;
  if (c->map || strNE (f->name, "data")
      || (strNE (structname, "PROXY_ENTITY")
          && strNE (structname, "PROXY_OBJECT")))
    return 0;
  numbits = find_field (self->fields, "data_numbits");
  return numbits && count_value (numbits, self->a) % 8;
}

/* An array, or a binary chunk, held by pointer: compared up to the length
   its struct gives it. */
static void
compare_array (Compare *c, const char *name, const char *structname,
               const Dwg_DYNAPI_field *f, const Outer *self,
               const Outer *outer)
{
  const void *va = *(void *const *)((const char *)self->a + f->offset);
  const void *vb = *(void *const *)((const char *)self->b + f->offset);
  const int binary = is_binary_type (f->type);
  const size_t len = strlen (f->type);
  int64_t la, lb;
  char etype[80];
  size_t esize;

  // The reader points OLE2FRAME.oleclient at a string constant.
  if (c->map && va && va == vb
      && !(strEQc (structname, "OLE2FRAME") && strEQc (f->name, "oleclient")))
    {
      compare_fail (c, "%s: shares its %s with the source", name,
                    binary ? "data" : "array");
      return;
    }
  la = field_length (c, structname, self->fields, f, self->a, outer,
                     outer ? outer->a : NULL);
  lb = field_length (c, structname, self->fields, f, self->b, outer,
                     outer ? outer->b : NULL);
  if (la < 0 || lb < 0)
    {
      if (va || vb)
        compare_fail (c, "%s [%s]: no length to compare it by", name,
                      f->type);
      return;
    }
  if (la != lb)
    {
      compare_fail (c, "%s: length %" PRId64 ", expected %" PRId64, name, lb,
                    la);
      return;
    }
  if (!la)
    return;
  if (!va || !vb)
    {
      // a length without data: a field the reader failed on
      if (!va != !vb)
        compare_fail (c, "%s: %s data of length %" PRId64, name,
                      va ? "no" : "unexpected", la);
      return;
    }
  if (binary)
    {
      size_t n = (size_t)la;
      if (unwritten_last_byte (c, structname, f, self))
        {
          n--;
          c->unwritten++;
        }
      compare_bytes (c, name, (const unsigned char *)va,
                     (const unsigned char *)vb, n);
      return;
    }
  if (strEQc (f->type, "char **"))
    {
      // ACIS blocks, of block_size bytes each, which compare on their own
      const Dwg_DYNAPI_field *bs = find_field (self->fields, "block_size");
      const BITCODE_BL *sizes
          = *(const BITCODE_BL *const *)((const char *)self->a + bs->offset);
      for (int64_t i = 0; i < la; i++)
        {
          const unsigned char *ba = ((unsigned char *const *)va)[i];
          const unsigned char *bb = ((unsigned char *const *)vb)[i];
          char ename[180];
          snprintf (ename, sizeof (ename), "%s[%u]", name, (unsigned)i);
          if (c->map && ba && ba == bb)
            compare_fail (c, "%s: shares its data with the source", ename);
          else if (sizes && sizes[i] && (!ba || !bb))
            compare_fail (c, "%s: %s data", ename, ba ? "no" : "unexpected");
          else if (sizes && sizes[i])
            compare_bytes (c, ename, ba, bb, sizes[i]);
        }
      return;
    }
  snprintf (etype, sizeof (etype), "%.*s", (int)(len - 1), f->type);
  esize = element_size (etype);
  if (!esize)
    {
      compare_fail (c, "%s [%s]: elements the test can't compare", name,
                    f->type);
      return;
    }
  for (int64_t i = 0; i < la; i++)
    {
      char ename[180];
      snprintf (ename, sizeof (ename), "%s[%u]", name, (unsigned)i);
      compare_value (c, ename, etype, (const char *)va + i * esize,
                     (const char *)vb + i * esize, self);
    }
}

static void
compare_fields (Compare *c, const char *prefix, const char *structname,
                const Dwg_DYNAPI_field *fields, const void *a, const void *b,
                const Outer *outer)
{
  const Outer self = { fields, a, b };
  for (const Dwg_DYNAPI_field *f = fields; f->name; f++)
    {
      const char *type = f->type;
      const size_t len = strlen (type);
      const char *pa = (const char *)a + f->offset;
      const char *pb = (const char *)b + f->offset;
      size_t esize;
      char name[160];
      // links into the document and its memory, not data. handleref is
      // unused by the library, and the copy clears it.
      if (strEQc (f->name, "parent") || strEQc (f->name, "dwg")
          || strEQc (f->name, "objid") || strEQc (f->name, "__iterator")
          || strEQc (f->name, "eed") || strEQc (f->name, "num_eed")
          || strEQc (f->name, "xdata") || strEQc (f->name, "handleref")
          || memBEGINc (type, "struct "))
        continue;
      if (inactive_member (structname, fields, f->name, a))
        continue;
      snprintf (name, sizeof (name), "%s%s%s", prefix, *prefix ? "." : "",
                f->name);
      if (strEQc (structname, "MLEADER_AnnotContext")
          && strEQc (f->name, "content"))
        {
          compare_mleader_content (c, name, &self, pa, pb);
          continue;
        }
      if (is_binary_type (type)
          || (len && type[len - 1] == '*' && !is_string_type (type)))
        {
          compare_array (c, name, structname, f, &self, outer);
          continue;
        }
      // an array held in the struct: dynapi gives the size of all of it
      esize = element_size (type);
      if (esize && f->size > esize && f->size % esize == 0)
        for (size_t i = 0; i < f->size / esize; i++)
          {
            char ename[180];
            snprintf (ename, sizeof (ename), "%s[%u]", name, (unsigned)i);
            compare_value (c, ename, type, pa + i * esize, pb + i * esize,
                           &self);
          }
      else
        compare_value (c, name, type, pa, pb, &self);
    }
}

static void
compare_eed (Compare *c, const Dwg_Eed *a, const BITCODE_BL na,
             const Dwg_Eed *b, const BITCODE_BL nb)
{
  if (na != nb)
    {
      compare_fail (c, "num_eed %u, expected %u", (unsigned)nb, (unsigned)na);
      return;
    }
  for (BITCODE_BL i = 0; i < na; i++)
    {
      const Dwg_Eed_Data *da = a[i].data, *db = b[i].data;
      char name[32];
      snprintf (name, sizeof (name), "eed[%u]", (unsigned)i);
      if (c->map && !c->dd_tolerance && b[i].raw)
        compare_fail (c, "%s: raw bytes kept", name);
      if (!!a[i].size != !!b[i].size)
        compare_fail (c, "%s: group start %d, expected %d", name, !!b[i].size,
                      !!a[i].size);
      compare_handle (c, name, a[i].handle.value, b[i].handle.value);
      if (!da || !db)
        {
          if (da != db)
            compare_fail (c, "%s: data %p, expected %p", name, (void *)db,
                          (void *)da);
          continue;
        }
      if (da->code != db->code)
        {
          compare_fail (c, "%s: code %u, expected %u", name, db->code,
                        da->code);
          continue;
        }
      switch (da->code)
        {
        case 0:
          if (da->u.eed_0.is_tu)
            compare_string (c, name, (const char *)da->u.eed_0_r2007.string,
                            (const char *)db->u.eed_0_r2007.string, 1);
          else
            compare_string (c, name, da->u.eed_0.string, db->u.eed_0.string,
                            0);
          break;
        case 3:
          compare_handle (c, name, da->u.eed_3.layer, db->u.eed_3.layer);
          break;
        case 4:
          if (da->u.eed_4.length != db->u.eed_4.length
              || memcmp (da->u.eed_4.data, db->u.eed_4.data,
                         da->u.eed_4.length))
            compare_fail (c, "%s: binary differs", name);
          break;
        case 5:
          compare_handle (c, name, da->u.eed_5.entity, db->u.eed_5.entity);
          break;
        default:
          {
            size_t size;
            if (da->code == 2)
              size = sizeof (da->u.eed_2);
            else if (da->code >= 10 && da->code <= 15)
              size = sizeof (da->u.eed_10);
            else if (da->code >= 40 && da->code <= 42)
              size = sizeof (da->u.eed_40);
            else if (da->code == 70)
              size = sizeof (da->u.eed_70);
            else if (da->code == 71)
              size = sizeof (da->u.eed_71);
            else
              size = 0;
            if (size && memcmp (&da->u, &db->u, size))
              compare_fail (c, "%s: code %u value differs", name, da->code);
          }
          break;
        }
    }
}

static void
compare_xdata (Compare *c, const Dwg_Resbuf *a, const Dwg_Resbuf *b)
{
  unsigned i = 0;
  for (; a && b; a = a->nextrb, b = b->nextrb, i++)
    {
      char name[32];
      snprintf (name, sizeof (name), "xdata[%u]", i);
      if (a->type != b->type)
        {
          compare_fail (c, "%s: type %d, expected %d", name, b->type,
                        a->type);
          return;
        }
      switch (dwg_resbuf_value_type (a->type))
        {
        case DWG_VT_STRING:
        case DWG_VT_BINARY:
          if (a->value.str.size != b->value.str.size
              || (a->value.str.size && a->value.str.u.data
                  && memcmp (a->value.str.u.data, b->value.str.u.data,
                             a->value.str.size
                                 * (a->value.str.is_tu ? 2 : 1))))
            compare_fail (c, "%s: string differs", name);
          else if (c->map && a->value.str.u.data
                   && a->value.str.u.data == b->value.str.u.data)
            compare_fail (c, "%s: shares its string with the source", name);
          break;
        case DWG_VT_OBJECTID:
        case DWG_VT_HANDLE:
          if (a->type >= 330 && a->type <= 399)
            compare_handle (c, name, a->value.absref, b->value.absref);
          else if (a->value.absref != b->value.absref)
            compare_fail (c, "%s: handle differs", name);
          break;
        default:
          if (memcmp (&a->value, &b->value, sizeof (a->value.pt)))
            compare_fail (c, "%s: value differs", name);
          break;
        }
    }
  if (a || b)
    compare_fail (c, "xdata: %u items, then %s", i,
                  a ? "items missing" : "extra items");
}

/* Compares object b with object a: a copy with its source when c->map is
   set, else an object read back with the one written. */
static void
compare_object (Compare *c, const Dwg_Data *adwg, const Dwg_Object *a,
                const Dwg_Data *bdwg, const Dwg_Object *b)
{
  const Dwg_DYNAPI_field *fields;
  const char *aclass = a->type >= 500 && a->type - 500u < adwg->num_classes
                           ? adwg->dwg_class[a->type - 500].dxfname
                           : NULL;
  const char *bclass = b->type >= 500 && b->type - 500u < bdwg->num_classes
                           ? bdwg->dwg_class[b->type - 500].dxfname
                           : NULL;
  if (a->fixedtype != b->fixedtype || a->supertype != b->supertype
      || (a->type < 500) != (b->type < 500)
      || (aclass && (!bclass || strNE (aclass, bclass))))
    {
      compare_fail (c, "type %s/%u, expected %s/%u", b->name, b->type,
                    a->name, a->type);
      return;
    }
  // copies, not objects read back, which may have raw bits again
  if (c->map && !c->dd_tolerance
      && (b->unknown_bits || b->num_unknown_bits || b->unknown_rest
          || b->num_unknown_rest))
    compare_fail (c, "raw bits kept");
  if (!a->tio.object || !b->tio.object)
    return;
  if (a->supertype == DWG_SUPERTYPE_ENTITY)
    {
      const Dwg_Object_Entity *ea = a->tio.entity, *eb = b->tio.entity;
      compare_fields (c, "", "Entity", dwg_dynapi_common_entity_fields (), ea,
                      eb, NULL);
      compare_eed (c, ea->eed, ea->num_eed, eb->eed, eb->num_eed);
      if (!ea->tio.UNUSED || !eb->tio.UNUSED)
        return;
      fields = dwg_dynapi_entity_fields (a->name);
      if (fields)
        compare_fields (c, "", a->name, fields, ea->tio.UNUSED,
                        eb->tio.UNUSED, NULL);
      else
        compare_fail (c, "no dynapi fields for %s", a->name);
    }
  else
    {
      const Dwg_Object_Object *oa = a->tio.object, *ob = b->tio.object;
      compare_fields (c, "", "Object", dwg_dynapi_common_object_fields (), oa,
                      ob, NULL);
      compare_eed (c, oa->eed, oa->num_eed, ob->eed, ob->num_eed);
      if (!oa->tio.APPID || !ob->tio.APPID)
        return;
      fields = dwg_dynapi_entity_fields (a->name);
      if (fields)
        compare_fields (c, "", a->name, fields, oa->tio.APPID,
                        ob->tio.APPID, NULL);
      else
        compare_fail (c, "no dynapi fields for %s", a->name);
      if (a->fixedtype == DWG_TYPE_XRECORD)
        compare_xdata (c, oa->tio.XRECORD->xdata, ob->tio.XRECORD->xdata);
    }
}

/* Every reference in dwg is NULL or resolves in dwg, and none has a handle
   below first, where the source's handles are. Handles with code 0 are
   values, not references (HANDSEED, sort keys). */
static void
check_refs (const char *what, Dwg_Data *dwg, const BITCODE_RLL first)
{
  unsigned below = 0, dangling = 0, refs = 0;
  for (BITCODE_BL i = 0; i < dwg->num_object_refs; i++)
    {
      const Dwg_Object_Ref *ref = dwg->object_ref[i];
      if (!ref || !ref->absolute_ref || !ref->handleref.code)
        continue;
      refs++;
      if (ref->absolute_ref < first)
        below++;
      else if (!dwg_resolve_handle (dwg, ref->absolute_ref))
        dangling++;
    }
  if (!below && !dangling)
    OK ("%s: %u references resolve in the copy", what, refs);
  else
    FAIL ("%s: of %u references, %u point at source handles, %u resolve to "
          "nothing",
          what, refs, below, dangling);
}

/* The name of the top-level field of obj at slot, for reports. */
static const char *
field_of (const Dwg_Object *obj, const void *slot)
{
  const Dwg_DYNAPI_field *lists[2];
  const char *bases[2];
  if (!obj || !obj->tio.object)
    return "?";
  if (obj->supertype == DWG_SUPERTYPE_ENTITY)
    {
      lists[0] = dwg_dynapi_common_entity_fields ();
      bases[0] = (const char *)obj->tio.entity;
      bases[1] = (const char *)obj->tio.entity->tio.UNUSED;
    }
  else
    {
      lists[0] = dwg_dynapi_common_object_fields ();
      bases[0] = (const char *)obj->tio.object;
      bases[1] = (const char *)obj->tio.object->tio.APPID;
    }
  lists[1] = dwg_dynapi_entity_fields (obj->name);
  for (int l = 0; l < 2; l++)
    for (const Dwg_DYNAPI_field *f = lists[l]; f && f->name && bases[l]; f++)
      if (bases[l] + f->offset == (const char *)slot)
        return f->name;
  return "nested";
}

/* Objects LibreDWG reads wrongly from the test drawings, so that their
   copies, encoded from the fields read, don't read back the same. The
   reader fails on the nodes of the ASSOC2DCONSTRAINTGROUP in
   2004/Constraints.dwg and 2000/Constraints.dwg (a node with 1866687297
   connections); a rewrite of the drawing keeps its raw bits instead. */
static int
misread_class (const Dwg_Object *obj)
{
  return obj->fixedtype == DWG_TYPE_ASSOC2DCONSTRAINTGROUP;
}

/* Every reference with a relative code (6, 8, 0xA, 0xC) in the fields of
   the objects from first_index on has the target its code gives from its
   holder's handle. Returns how many there are. */
static unsigned
check_relative (Dwg_Data *dwg, const BITCODE_BL first_index,
                unsigned *wrong)
{
  unsigned relative = 0;
  *wrong = 0;
  for (BITCODE_BL i = first_index; i < dwg->num_objects; i++)
    {
      const Dwg_Object *obj = &dwg->object[i];
      const Dwg_DYNAPI_field *lists[2];
      const void *bases[2];
      if (!obj->tio.object)
        continue;
      if (obj->supertype == DWG_SUPERTYPE_ENTITY)
        {
          lists[0] = dwg_dynapi_common_entity_fields ();
          bases[0] = obj->tio.entity;
          bases[1] = obj->tio.entity->tio.UNUSED;
        }
      else
        {
          lists[0] = dwg_dynapi_common_object_fields ();
          bases[0] = obj->tio.object;
          bases[1] = obj->tio.object->tio.APPID;
        }
      lists[1] = dwg_dynapi_entity_fields (obj->name);
      for (int l = 0; l < 2; l++)
        {
          if (!lists[l] || !bases[l])
            continue;
          for (const Dwg_DYNAPI_field *f = lists[l]; f->name; f++)
            {
              const Dwg_Object_Ref *ref;
              BITCODE_RLL expected;
              if (strNE (f->type, "H"))
                continue;
              ref = *(const BITCODE_H *)((const char *)bases[l] + f->offset);
              if (!ref || ref->handleref.code < 6 || ref->handleref.code > 0xC)
                continue;
              relative++;
              switch (ref->handleref.code)
                {
                case 6:
                  expected = obj->handle.value + 1;
                  break;
                case 8:
                  expected = obj->handle.value - 1;
                  break;
                case 0xA:
                  expected = obj->handle.value + ref->handleref.value;
                  break;
                case 0xC:
                  expected = obj->handle.value - ref->handleref.value;
                  break;
                default:
                  continue;
                }
              if (expected != ref->absolute_ref)
                (*wrong)++;
            }
        }
    }
  return relative;
}

/* Copies every object of the drawing into an empty document, compares the
   copies with their sources, frees the source, writes the copy as version
   (0: the source's), reads it back and compares again. A read back as
   another version gets the same objects, and references, only. */
static int
test_clone (const char *name, Dwg_Version_Type version)
{
  char path[256], file[512], what[300];
  const char *keep = getenv ("COPY_TEST_KEEP");
  const char *base;
  Dwg_Data src, readback;
  Dwg_Data *dst;
  Dwg_Copy_Map *map;
  Dwg_Copy_Unresolved *unresolved = NULL;
  BITCODE_BL num_unresolved = 0, num_handles = 0, header_unresolved;
  BITCODE_RLL *handles, first;
  unsigned skipped = 0, to_skipped = 0, dangling = 0, relative, wrong;
  BITCODE_BL num_entities = 0;
  Compare c;
  int error;

  failed = 0;
  if (!test_file (name, path, sizeof (path)))
    {
      FAIL ("%s not found", name);
      return 1;
    }
  if (read_dwg (path, &src) >= DWG_ERR_CRITICAL)
    {
      FAIL ("clone %s: read failed", name);
      free_dwg (&src);
      return 1;
    }
  if (!version)
    version = src.header.from_version;
  snprintf (what, sizeof (what), "clone %s as %s", name,
            dwg_version_type (version));
  first = src.header_vars.HANDSEED->absolute_ref;
  dst = new_document_like (&src);

  handles = (BITCODE_RLL *)calloc (src.num_objects, sizeof (BITCODE_RLL));
  for (BITCODE_BL i = 0; i < src.num_objects; i++)
    {
      const Dwg_Object *obj = &src.object[i];
      if (is_copyable (obj))
        {
          handles[num_handles++] = obj->handle.value;
          if (obj->supertype == DWG_SUPERTYPE_ENTITY)
            num_entities++;
        }
      else
        skipped++;
    }
  map = dwg_copy_map_new ();
  loglevel = tracelevel;
  error = dwg_copy_objects (dst, &src, handles, num_handles, map, &unresolved,
                            &num_unresolved);
  if (!error && num_handles && dst->num_objects == num_handles
      && dst->num_entities == num_entities)
    OK ("%s: copied %u objects, %u of them entities, left out %u (error "
        "0x%x)",
        what, (unsigned)num_handles, (unsigned)num_entities, skipped,
        (unsigned)error);
  else
    {
      FAIL ("%s: copy error 0x%x, %u of %u objects, %u of %u entities",
            what, (unsigned)error, (unsigned)dst->num_objects,
            (unsigned)num_handles, (unsigned)dst->num_entities,
            (unsigned)num_entities);
      free_dwg (&src);
      goto cleanup;
    }

  // the copies' references outside the copy are to the objects left out,
  // or to handles that aren't in the source either
  for (BITCODE_BL i = 0; i < num_unresolved; i++)
    {
      const Dwg_Object *target
          = dwg_resolve_handle (&src, unresolved[i].target);
      if (!target)
        {
          // listed when COPY_TEST_REPORTED asks for more than the default
          if (dangling++ < (unsigned)max_reported && max_reported > 10)
            {
              const Dwg_Object *holder
                  = dwg_resolve_handle (dst, unresolved[i].holder);
              BITCODE_RLL source = 0;
              for (BITCODE_BL k = 0; k < num_handles && !source; k++)
                if (dwg_copy_map_get (map, handles[k]) == unresolved[i].holder)
                  source = handles[k];
              printf ("  # dangling: %s " FORMAT_HV " (from " FORMAT_HV
                      ") code %u to " FORMAT_HV " (%s)\n",
                      holder ? holder->name : "?", unresolved[i].holder,
                      source, unresolved[i].code, unresolved[i].target,
                      field_of (holder, unresolved[i].slot));
            }
        }
      else if (!is_copyable (target))
        to_skipped++;
      else
        FAIL ("%s: unresolved reference from " FORMAT_HV " to copied %s "
              FORMAT_HV,
              what, unresolved[i].holder, target->name, unresolved[i].target);
    }
  OK ("%s: %u references to objects left out, %u dangling in the source",
      what, to_skipped, dangling);

  header_unresolved = copy_header_vars (dst, &src, map);
  OK ("%s: header variables copied, %u references to objects left out", what,
      (unsigned)header_unresolved);

  memset (&c, 0, sizeof (c));
  c.map = map;
  c.is_tu = src.header.from_version >= R_2007 && !(src.opts & DWG_OPTS_IN);
  c.version = src.header.from_version;
  for (BITCODE_BL i = 0; i < num_handles; i++)
    {
      const Dwg_Object *a = dwg_resolve_handle (&src, handles[i]);
      const Dwg_Object *b
          = dwg_resolve_handle (dst, dwg_copy_map_get (map, handles[i]));
      char obj_what[64];
      snprintf (obj_what, sizeof (obj_what), "%s " FORMAT_HV, a->name,
                a->handle.value);
      c.what = obj_what;
      if (!b)
        compare_fail (&c, "copy not found");
      else
        compare_object (&c, &src, a, dst, b);
    }
  if (!c.failed)
    OK ("%s: every copy has its source's fields", what);
  else
    FAIL ("%s: %d field differences between copies and sources", what,
          c.failed);
  check_refs (what, dst, first);
  relative = check_relative (dst, 0, &wrong);
  if (!wrong)
    OK ("%s: %u relative references recomputed", what, relative);
  else
    FAIL ("%s: %u of %u relative references wrong", what, wrong, relative);

  // no copy may need the source from here on
  free_dwg (&src);
  dst->header.version = version;
  base = strrchr (name, '/');
  if (!base)
    base = strrchr (name, '\\');
  base = base ? base + 1 : name;
  snprintf (file, sizeof (file), "%s%scopy_%.*s_%s.dwg", keep ? keep : "",
            keep ? "/" : "",
            (int)(strrchr (base, '.') ? strrchr (base, '.') - base
                                      : (ptrdiff_t)strlen (base)),
            base, dwg_version_type (version));
  loglevel = tracelevel;
  error = write_dwg (file, dst);
  if (error >= DWG_ERR_CRITICAL)
    {
      FAIL ("%s: write error 0x%x", what, (unsigned)error);
      goto cleanup;
    }
  error = read_dwg (file, &readback);
  if (error >= DWG_ERR_CRITICAL)
    {
      FAIL ("%s: read back error 0x%x", what, (unsigned)error);
      free_dwg (&readback);
      goto cleanup;
    }
  if (readback.num_objects == dst->num_objects)
    OK ("%s: %u objects read back", what, (unsigned)readback.num_objects);
  else
    FAIL ("%s: %u objects read back, %u written", what,
          (unsigned)readback.num_objects, (unsigned)dst->num_objects);
  if (version == dst->header.from_version)
    {
      unsigned misread = 0;
      memset (&c, 0, sizeof (c));
      c.dd_tolerance = 1;
      c.is_tu = IS_FROM_TU_DWG (dst);
      c.version = dst->header.from_version;
      for (BITCODE_BL i = 0; i < dst->num_objects; i++)
        {
          const Dwg_Object *a = &dst->object[i];
          const Dwg_Object *b
              = dwg_resolve_handle (&readback, a->handle.value);
          char obj_what[64];
          if (misread_class (a))
            {
              misread++;
              continue;
            }
          snprintf (obj_what, sizeof (obj_what), "read back %s " FORMAT_HV,
                    a->name, a->handle.value);
          c.what = obj_what;
          if (!b)
            compare_fail (&c, "not read back");
          else
            compare_object (&c, dst, a, &readback, b);
        }
      if (!c.failed)
        OK ("%s: every object reads back with the fields written, but %u "
            "misread from the source and %u PROXY data last bytes",
            what, misread, c.unwritten);
      else
        FAIL ("%s: %d field differences read back", what, c.failed);
    }
  check_refs (what, &readback, first);
  free_dwg (&readback);
  if (!keep && !numfailed ())
    unlink (file);

cleanup:
  free_dwg (dst);
  free (dst);
  dwg_copy_map_free (map);
  free (unresolved);
  free (handles);
  return numfailed ();
}

/* The model space entities of dwg, with the vertices, attributes and
   SEQENDs they own. */
static BITCODE_RLL *
model_space_entities (Dwg_Data *dwg, BITCODE_BL *nump)
{
  BITCODE_RLL *handles
      = (BITCODE_RLL *)calloc (dwg->num_objects, sizeof (BITCODE_RLL));
  Dwg_Copy_Map *in = dwg_copy_map_new ();
  int added = 1;
  *nump = 0;
  // owners come before what they own
  while (added)
    {
      added = 0;
      for (BITCODE_BL i = 0; i < dwg->num_objects; i++)
        {
          const Dwg_Object *obj = &dwg->object[i];
          const Dwg_Object_Entity *ent;
          if (obj->supertype != DWG_SUPERTYPE_ENTITY || !is_copyable (obj)
              || dwg_copy_map_get (in, obj->handle.value))
            continue;
          ent = obj->tio.entity;
          if (ent->entmode == 2
              || (ent->ownerhandle
                  && dwg_copy_map_get (in, ent->ownerhandle->absolute_ref)))
            {
              handles[(*nump)++] = obj->handle.value;
              dwg_copy_map_set (in, obj->handle.value, 1);
              added = 1;
            }
        }
    }
  dwg_copy_map_free (in);
  return handles;
}

/* Copies the model space entities of the drawing within the drawing:
   references between them go to the copies, and the others keep their
   targets. */
static int
test_same_document (const char *name)
{
  char path[256], file[128], what[160];
  Dwg_Data dwg, readback;
  Dwg_Copy_Map *map;
  Dwg_Copy_Unresolved *unresolved = NULL;
  BITCODE_BL num_unresolved = 0, num_handles, num_objects, num_entities;
  BITCODE_RLL *handles;
  unsigned relative, wrong, owners = 0;
  Compare c;
  int error;

  failed = 0;
  snprintf (what, sizeof (what), "copy %s within itself", name);
  if (!test_file (name, path, sizeof (path))
      || read_dwg (path, &dwg) >= DWG_ERR_CRITICAL)
    {
      FAIL ("%s: read failed", what);
      return 1;
    }
  num_objects = dwg.num_objects;
  num_entities = dwg.num_entities;
  handles = model_space_entities (&dwg, &num_handles);
  map = dwg_copy_map_new ();
  loglevel = tracelevel;
  error = dwg_copy_objects (&dwg, &dwg, handles, num_handles, map,
                            &unresolved, &num_unresolved);
  if (!error && num_handles && !num_unresolved
      && dwg.num_objects == num_objects + num_handles
      && dwg.num_entities == num_entities + num_handles)
    OK ("%s: copied %u entities, none unresolved (error 0x%x)", what,
        (unsigned)num_handles, (unsigned)error);
  else
    FAIL ("%s: error 0x%x, %u unresolved, %u objects", what, (unsigned)error,
          (unsigned)num_unresolved, (unsigned)dwg.num_objects);

  memset (&c, 0, sizeof (c));
  c.map = map;
  c.same_document = 1;
  c.is_tu = (dwg.header.from_version >= R_2007 && !(dwg.opts & DWG_OPTS_IN));
  c.version = dwg.header.from_version;
  for (BITCODE_BL i = 0; i < num_handles; i++)
    {
      const Dwg_Object *a = dwg_resolve_handle (&dwg, handles[i]);
      const Dwg_Object *b
          = dwg_resolve_handle (&dwg, dwg_copy_map_get (map, handles[i]));
      char obj_what[64];
      snprintf (obj_what, sizeof (obj_what), "%s " FORMAT_HV, a->name,
                a->handle.value);
      c.what = obj_what;
      if (!b)
        compare_fail (&c, "copy not found");
      else
        {
          compare_object (&c, &dwg, a, &dwg, b);
          // owned entities point at the copy of their owner
          if (a->tio.entity->entmode != 2 && b->tio.entity->ownerhandle
              && b->tio.entity->ownerhandle->absolute_ref
                     == dwg_copy_map_get (
                         map, a->tio.entity->ownerhandle->absolute_ref))
            owners++;
        }
    }
  if (!c.failed)
    OK ("%s: every copy has its source's fields, %u owned by copies", what,
        owners);
  else
    FAIL ("%s: %d field differences between copies and sources", what,
          c.failed);
  relative = check_relative (&dwg, num_objects, &wrong);
  if (!wrong)
    OK ("%s: %u relative references recomputed", what, relative);
  else
    FAIL ("%s: %u of %u relative references wrong", what, wrong, relative);

  snprintf (file, sizeof (file), "copy_within_%s",
            strrchr (name, '/') ? strrchr (name, '/') + 1 : name);
  memset (&readback, 0, sizeof (readback));
  loglevel = tracelevel;
  error = write_dwg (file, &dwg);
  if (error < DWG_ERR_CRITICAL)
    error = read_dwg (file, &readback);
  if (error >= DWG_ERR_CRITICAL)
    FAIL ("%s: write or read back error 0x%x", what, (unsigned)error);
  else
    {
      memset (&c, 0, sizeof (c));
      c.dd_tolerance = 1;
      c.is_tu = (dwg.header.from_version >= R_2007 && !(dwg.opts & DWG_OPTS_IN));
      c.version = dwg.header.from_version;
      for (BITCODE_BL i = num_objects; i < dwg.num_objects; i++)
        {
          const Dwg_Object *a = &dwg.object[i];
          const Dwg_Object *b
              = dwg_resolve_handle (&readback, a->handle.value);
          char obj_what[64];
          snprintf (obj_what, sizeof (obj_what), "read back %s " FORMAT_HV,
                    a->name, a->handle.value);
          c.what = obj_what;
          if (!b)
            compare_fail (&c, "not read back");
          else
            compare_object (&c, &dwg, a, &readback, b);
        }
      if (readback.num_objects == dwg.num_objects && !c.failed)
        OK ("%s: %u objects read back, the copies with their fields, but "
            "%u PROXY data last bytes",
            what, (unsigned)readback.num_objects, c.unwritten);
      else
        FAIL ("%s: %u objects read back of %u, %d field differences", what,
              (unsigned)readback.num_objects, (unsigned)dwg.num_objects,
              c.failed);
    }
  free_dwg (&readback);
  if (!numfailed ())
    unlink (file);
  free_dwg (&dwg);
  dwg_copy_map_free (map);
  free (unresolved);
  free (handles);
  return numfailed ();
}

/* The destination handle of the record in dst that matches source object
   target by type and name, as the caller of a copy might find it: a layer
   dst lacks is added. */
static BITCODE_RLL
match_by_name (Dwg_Data *dst, Dwg_Data *src, const BITCODE_RLL target)
{
  const Dwg_Object *obj = dwg_resolve_handle (src, target);
  const char *table = NULL;
  char *name;
  BITCODE_H ref;
  int error;
  if (!obj)
    return 0;
  if (obj->fixedtype == DWG_TYPE_BLOCK_HEADER)
    return dwg_model_space_object (dst)->handle.value;
  if (obj->fixedtype == DWG_TYPE_LAYER)
    table = "LAYER";
  else if (obj->fixedtype == DWG_TYPE_LTYPE)
    table = "LTYPE";
  else if (obj->fixedtype == DWG_TYPE_STYLE)
    table = "STYLE";
  else if (obj->fixedtype == DWG_TYPE_APPID)
    table = "APPID";
  else
    return 0;
  name = dwg_obj_table_get_name (obj, &error);
  ref = name ? dwg_find_tablehandle (dst, name, table) : NULL;
  if (!ref && name && obj->fixedtype == DWG_TYPE_LAYER)
    {
      const Dwg_Object_LAYER *layer = dwg_add_LAYER (dst, name);
      return layer ? dst->object[layer->parent->objid].handle.value : 0;
    }
  return ref ? ref->absolute_ref : 0;
}

/* Copies a LINE into a new document: its references to the source's layer
   and the like are reported and left NULL, and dwg_copy_resolve () points
   them at the new document's records once the caller maps them. */
static int
test_unresolved (void)
{
  const char *what = "unresolved references";
  char path[256];
  Dwg_Data src;
  Dwg_Data *dst;
  const Dwg_Object *line = NULL, *copy;
  Dwg_Copy_Map *map;
  Dwg_Copy_Unresolved *unresolved = NULL;
  BITCODE_BL num_unresolved = 0;
  BITCODE_RLL handle, layer;
  const char *layer_name;
  unsigned unmatched = 0;
  int layer_reported = 0, error;

  failed = 0;
  if (!test_file ("example_2004.dwg", path, sizeof (path))
      || read_dwg (path, &src) >= DWG_ERR_CRITICAL)
    {
      FAIL ("%s: read failed", what);
      return 1;
    }
  for (BITCODE_BL i = 0; i < src.num_objects && !line; i++)
    if (src.object[i].fixedtype == DWG_TYPE_LINE
        && src.object[i].tio.entity->entmode == 2)
      line = &src.object[i];
  dst = dwg_new_Document (R_2004, 0, tracelevel);
  handle = line->handle.value;
  layer = line->tio.entity->layer->absolute_ref;
  layer_name = dwg_obj_table_get_name (dwg_resolve_handle (&src, layer),
                                       &error);
  map = dwg_copy_map_new ();
  loglevel = tracelevel;
  error = dwg_copy_objects (dst, &src, &handle, 1, map, &unresolved,
                            &num_unresolved);
  copy = dwg_resolve_handle (dst, dwg_copy_map_get (map, handle));
  for (BITCODE_BL i = 0; i < num_unresolved; i++)
    if (unresolved[i].target == layer
        && unresolved[i].kind == DWG_COPY_SLOT_REF
        && unresolved[i].holder == copy->handle.value)
      layer_reported = 1;
  if (error < DWG_ERR_CRITICAL && copy && layer_reported
      && copy->tio.entity->layer && !copy->tio.entity->layer->absolute_ref)
    OK ("%s: %u reported, the layer among them, NULL in the copy", what,
        (unsigned)num_unresolved);
  else
    FAIL ("%s: error 0x%x, %u reported, layer reported %d", what,
          (unsigned)error, (unsigned)num_unresolved, layer_reported);

  // what a caller can't match by name, it would copy, or leave NULL
  for (BITCODE_BL i = 0; i < num_unresolved; i++)
    {
      const BITCODE_RLL mapped
          = match_by_name (dst, &src, unresolved[i].target);
      if (mapped)
        dwg_copy_map_set (map, unresolved[i].target, mapped);
      else
        unmatched++;
    }
  error = dwg_copy_resolve (dst, map, unresolved, &num_unresolved);
  loglevel = test_loglevel;
  for (BITCODE_BL i = 0; i < num_unresolved; i++)
    {
      const Dwg_Object *target
          = dwg_resolve_handle (&src, unresolved[i].target);
      printf ("  # left unresolved: %s " FORMAT_HV "\n",
              target ? target->name : "?", unresolved[i].target);
    }
  if (!error && num_unresolved == unmatched
      && copy->tio.entity->layer->absolute_ref
             == dwg_find_tablehandle (dst, layer_name, "LAYER")->absolute_ref)
    OK ("%s: those matched by name resolved, the layer to the new "
        "document's layer %s; %u left",
        what, layer_name, (unsigned)num_unresolved);
  else
    FAIL ("%s: error 0x%x, %u left unresolved, %u unmatched", what,
          (unsigned)error, (unsigned)num_unresolved, unmatched);

  free_dwg (&src);
  free_dwg (dst);
  free (dst);
  dwg_copy_map_free (map);
  free (unresolved);
  return numfailed ();
}

/* A new EED item of the given code, with room for a string of len. */
static Dwg_Eed_Data *
new_eed_data (const BITCODE_RC code, const size_t len)
{
  Dwg_Eed_Data *data
      = (Dwg_Eed_Data *)calloc (1, sizeof (Dwg_Eed_Data) + len + 1);
  data->code = code;
  return data;
}

/* EED copied without its raw bytes: its APPID and layer handles are
   reported until resolved, and its 1005 handle points at the copy of the
   entity it names. Then written and read back. */
static int
test_eed (void)
{
  const char *what = "EED";
  const char *text = "copied text";
  const char *file = "copy_eed.dwg";
  const dwg_point_3d p0 = { 0.0, 0.0, 0.0 }, p1 = { 10.0, 0.0, 0.0 },
                     p2 = { 0.0, 10.0, 0.0 };
  Dwg_Data *src = dwg_new_Document (R_2004, 0, tracelevel);
  Dwg_Data *dst = dwg_new_Document (R_2004, 0, tracelevel);
  Dwg_Data readback;
  Dwg_Object_BLOCK_HEADER *mspace
      = dwg_model_space_object (src)->tio.object->tio.BLOCK_HEADER;
  Dwg_Object_APPID *appid = dwg_add_APPID (src, "COPY_TEST");
  Dwg_Entity_LINE *line1 = dwg_add_LINE (mspace, &p0, &p1);
  Dwg_Entity_LINE *line2 = dwg_add_LINE (mspace, &p0, &p2);
  const Dwg_Object *o1 = &src->object[line1->parent->objid];
  const Dwg_Object *o2 = &src->object[line2->parent->objid];
  const BITCODE_RLL appid_handle
      = src->object[appid->parent->objid].handle.value;
  const BITCODE_RLL layer0
      = dwg_find_tablehandle (src, "0", "LAYER")->absolute_ref;
  Dwg_Object_Entity *ent = o1->tio.entity;
  BITCODE_RLL handles[2];
  const Dwg_Object *copy1, *copy2;
  const Dwg_Eed *eed;
  Dwg_Copy_Map *map = dwg_copy_map_new ();
  Dwg_Copy_Unresolved *unresolved = NULL;
  BITCODE_BL num_unresolved = 0;
  unsigned eed_reported = 0;
  int error;

  failed = 0;
  // one group: a string, the other line, layer 0 and a real
  ent->num_eed = 4;
  ent->eed = (Dwg_Eed *)calloc (4, sizeof (Dwg_Eed));
  ent->eed[0].size = 1; // starts the group; the encoder sizes it
  for (int i = 0; i < 4; i++)
    {
      ent->eed[i].handle.code = 5;
      ent->eed[i].handle.value = appid_handle;
      dwg_set_handle_size (&ent->eed[i].handle);
    }
  ent->eed[0].data = new_eed_data (0, strlen (text));
  ent->eed[0].data->u.eed_0.length = (BITCODE_RS)strlen (text);
  ent->eed[0].data->u.eed_0.codepage = src->header.codepage;
  memcpy (ent->eed[0].data->u.eed_0.string, text, strlen (text) + 1);
  ent->eed[1].data = new_eed_data (5, 0);
  ent->eed[1].data->u.eed_5.entity = o2->handle.value;
  ent->eed[2].data = new_eed_data (3, 0);
  ent->eed[2].data->u.eed_3.layer = layer0;
  ent->eed[3].data = new_eed_data (40, 0);
  ent->eed[3].data->u.eed_40.real = 1.5;

  handles[0] = o1->handle.value;
  handles[1] = o2->handle.value;
  loglevel = tracelevel;
  error = dwg_copy_objects (dst, src, handles, 2, map, &unresolved,
                            &num_unresolved);
  copy1 = dwg_resolve_handle (dst, dwg_copy_map_get (map, handles[0]));
  copy2 = dwg_resolve_handle (dst, dwg_copy_map_get (map, handles[1]));
  for (BITCODE_BL i = 0; i < num_unresolved; i++)
    if ((unresolved[i].kind == DWG_COPY_SLOT_EED_APPID
         && unresolved[i].target == appid_handle)
        || (unresolved[i].kind == DWG_COPY_SLOT_HANDLE
            && unresolved[i].target == layer0))
      eed_reported++;
  eed = copy1 ? copy1->tio.entity->eed : NULL;
  if (error < DWG_ERR_CRITICAL && copy1 && copy2 && eed
      && copy1->tio.entity->num_eed == 4 && eed != ent->eed
      && eed[0].data != ent->eed[0].data && !eed[0].raw
      && !strcmp (eed[0].data->u.eed_0.string, text)
      && eed[1].data->u.eed_5.entity == copy2->handle.value
      && !eed[0].handle.value && !eed[2].data->u.eed_3.layer
      && eed[3].data->u.eed_40.real == 1.5 && eed_reported == 5)
    OK ("%s: copied, 1005 remapped, APPID and layer reported", what);
  else
    FAIL ("%s: error 0x%x, %u EED handles reported", what, (unsigned)error,
          eed_reported);

  dwg_add_APPID (dst, "COPY_TEST");
  for (BITCODE_BL i = 0; i < num_unresolved; i++)
    {
      const BITCODE_RLL mapped = match_by_name (dst, src, unresolved[i].target);
      if (mapped)
        dwg_copy_map_set (map, unresolved[i].target, mapped);
    }
  error = dwg_copy_resolve (dst, map, unresolved, &num_unresolved);
  copy1 = dwg_resolve_handle (dst, dwg_copy_map_get (map, handles[0]));
  eed = copy1->tio.entity->eed;
  if (!error && !num_unresolved
      && eed[0].handle.value == eed[3].handle.value
      && eed[0].handle.value
             == dwg_find_tablehandle (dst, "COPY_TEST", "APPID")->absolute_ref
      && eed[2].data->u.eed_3.layer
             == dwg_find_tablehandle (dst, "0", "LAYER")->absolute_ref)
    OK ("%s: APPID and layer resolved to the new document's", what);
  else
    FAIL ("%s: error 0x%x, %u left unresolved", what, (unsigned)error,
          (unsigned)num_unresolved);

  free_dwg (src);
  free (src);
  memset (&readback, 0, sizeof (readback));
  error = write_dwg (file, dst);
  if (error < DWG_ERR_CRITICAL)
    error = read_dwg (file, &readback);
  if (error >= DWG_ERR_CRITICAL)
    FAIL ("%s: write or read back error 0x%x", what, (unsigned)error);
  else
    {
      Compare c;
      const Dwg_Object *read1 = dwg_resolve_handle (&readback, copy1->handle.value);
      memset (&c, 0, sizeof (c));
      c.what = "read back EED";
      if (read1)
        compare_eed (&c, copy1->tio.entity->eed, copy1->tio.entity->num_eed,
                     read1->tio.entity->eed, read1->tio.entity->num_eed);
      if (read1 && !c.failed)
        OK ("%s: read back the same", what);
      else
        FAIL ("%s: %s, %d differences", what, read1 ? "read" : "not read",
              c.failed);
    }
  free_dwg (&readback);
  if (!numfailed ())
    unlink (file);
  free_dwg (dst);
  free (dst);
  dwg_copy_map_free (map);
  free (unresolved);
  return numfailed ();
}

/* The byte that starts at bit of data, which a bit chain reads most
   significant bit first. */
static unsigned char
byte_at_bit (const unsigned char *data, const size_t bit)
{
  const size_t byte = bit / 8;
  const unsigned shift = (unsigned)(bit % 8);
  if (!shift)
    return data[byte];
  return (unsigned char)((data[byte] << shift)
                         | (data[byte + 1] >> (8 - shift)));
}

static void
set_byte_at_bit (unsigned char *data, const size_t bit,
                 const unsigned char value)
{
  const size_t byte = bit / 8;
  const unsigned shift = (unsigned)(bit % 8);
  if (!shift)
    {
      data[byte] = value;
      return;
    }
  data[byte] = (unsigned char)((data[byte] & (0xFF << (8 - shift)))
                               | (value >> shift));
  data[byte + 1] = (unsigned char)((data[byte + 1] & (0xFF >> shift))
                                   | (value << (8 - shift)));
}

/* Writes example_2000.dwg to file with the length of its first "DesignCenter
   Data" EED string set to 255, more than its group holds. Returns 0 when
   the drawing or the string isn't found. */
static int
write_unread_eed_drawing (const char *file)
{
  // code 0, length 17, codepage 30 (big-endian), then the string
  const unsigned char item[] = { 0x00, 0x11, 0x00, 0x1E, 'D', 'e', 's', 'i',
                                 'g',  'n',  'C',  'e',  'n', 't', 'e', 'r',
                                 ' ',  'D',  'a',  't',  'a' };
  char path[256];
  unsigned char *data = NULL;
  size_t size = 0, bit;
  long end;
  FILE *fp;
  int found = 0, written = 0;

  if (!test_file ("example_2000.dwg", path, sizeof (path)))
    return 0;
  fp = fopen (path, "rb");
  if (!fp)
    return 0;
  end = fseek (fp, 0, SEEK_END) ? -1 : ftell (fp);
  if (end > (long)sizeof (item) && !fseek (fp, 0, SEEK_SET))
    {
      size = (size_t)end;
      data = (unsigned char *)malloc (size);
    }
  if (!data || fread (data, 1, size, fp) != size)
    {
      fclose (fp);
      free (data);
      return 0;
    }
  fclose (fp);
  // an object's EED can start at any bit of the file
  for (bit = 0; bit / 8 + sizeof (item) < size; bit++)
    {
      size_t k = 0;
      while (k < sizeof (item) && byte_at_bit (data, bit + k * 8) == item[k])
        k++;
      if (k == sizeof (item))
        {
          found = 1;
          break;
        }
    }
  if (found)
    {
      set_byte_at_bit (data, bit + 8, 0xFF);
      fp = fopen (file, "wb");
      if (fp)
        {
          written = fwrite (data, 1, size, fp) == size;
          written &= !fclose (fp);
        }
    }
  free (data);
  return written;
}

static const Dwg_Eed *
object_eed (const Dwg_Object *obj, BITCODE_BL *num_eedp)
{
  *num_eedp = 0;
  if (obj->supertype == DWG_SUPERTYPE_ENTITY && obj->tio.entity)
    {
      *num_eedp = obj->tio.entity->num_eed;
      return obj->tio.entity->eed;
    }
  if (obj->supertype == DWG_SUPERTYPE_OBJECT && obj->tio.object)
    {
      *num_eedp = obj->tio.object->num_eed;
      return obj->tio.object->eed;
    }
  return NULL;
}

/* The index of the EED string in obj that write_unread_eed_drawing ()
   changed, as the decoder left it: unread, with zero bytes and the length
   255. -1 if obj has none. */
static int
unread_eed_string (const Dwg_Object *obj)
{
  BITCODE_BL num_eed;
  const Dwg_Eed *eed = object_eed (obj, &num_eed);
  for (BITCODE_BL i = 0; eed && i < num_eed; i++)
    {
      const Dwg_Eed_Data *data = eed[i].data;
      if (data && data->code == 0 && !data->u.eed_0.is_tu
          && data->u.eed_0.length == 255 && !data->u.eed_0.string[0])
        return (int)i;
    }
  return -1;
}

/* An EED string that claims more bytes than its group holds. For r13 to
   r2004 the decoder keeps it unread: the item has the claimed length,
   zero bytes, and only the room its group's size gives it. The encoder
   writes a string that starts with NUL as the empty string, and the copy
   copies it as that, reading only the item's first string byte. First an
   item built as the decoder leaves it, then the one the decoder leaves in
   example_2000.dwg with one length byte changed. */
static int
test_eed_unread_string (void)
{
  const char *what = "EED string left unread";
  const char *file = "copy_eed_unread.dwg";
  const char *drawing = "copy_eed_unread_source.dwg";
  const dwg_point_3d p0 = { 0.0, 0.0, 0.0 }, p1 = { 10.0, 0.0, 0.0 };
  const BITCODE_BS group_size = 31;
  Dwg_Data *src = dwg_new_Document (R_2000, 0, tracelevel);
  Dwg_Data *dst = dwg_new_Document (R_2000, 0, tracelevel);
  Dwg_Data readback, drawn;
  Dwg_Object_BLOCK_HEADER *mspace
      = dwg_model_space_object (src)->tio.object->tio.BLOCK_HEADER;
  Dwg_Object_APPID *appid = dwg_add_APPID (src, "COPY_TEST");
  Dwg_Entity_LINE *line = dwg_add_LINE (mspace, &p0, &p1);
  const Dwg_Object *obj = &src->object[line->parent->objid];
  const Dwg_Object *copy;
  const Dwg_Eed_Data *data;
  Dwg_Object_Entity *ent = obj->tio.entity;
  BITCODE_RLL handle = obj->handle.value;
  Dwg_Copy_Map *map = dwg_copy_map_new ();
  Dwg_Copy_Unresolved *unresolved = NULL;
  BITCODE_BL num_unresolved = 0;
  int error, index = -1;

  failed = 0;
  ent->num_eed = 1;
  ent->eed = (Dwg_Eed *)calloc (1, sizeof (Dwg_Eed));
  ent->eed[0].size = group_size;
  ent->eed[0].handle.code = 5;
  ent->eed[0].handle.value = src->object[appid->parent->objid].handle.value;
  dwg_set_handle_size (&ent->eed[0].handle);
  // allocated as the decoder allocates an item of its group
  ent->eed[0].data = (Dwg_Eed_Data *)calloc (group_size + 8, 1);
  ent->eed[0].data->code = 0;
  ent->eed[0].data->u.eed_0.length = 255;
  ent->eed[0].data->u.eed_0.codepage = src->header.codepage;

  loglevel = tracelevel;
  error = dwg_copy_objects (dst, src, &handle, 1, map, &unresolved,
                            &num_unresolved);
  copy = dwg_resolve_handle (dst, dwg_copy_map_get (map, handle));
  data = copy && copy->tio.entity->num_eed == 1
             ? copy->tio.entity->eed[0].data
             : NULL;
  if (!error && data && data->code == 0 && !data->u.eed_0.is_tu
      && data->u.eed_0.length == 0 && !data->u.eed_0.string[0]
      && data->u.eed_0.codepage == src->header.codepage
      && copy->tio.entity->eed[0].size)
    OK ("%s: copied as the empty string", what);
  else
    FAIL ("%s: error 0x%x, copied with length %d", what, (unsigned)error,
          data ? (int)data->u.eed_0.length : -1);

  dwg_add_APPID (dst, "COPY_TEST");
  for (BITCODE_BL i = 0; i < num_unresolved; i++)
    {
      const BITCODE_RLL mapped
          = match_by_name (dst, src, unresolved[i].target);
      if (mapped)
        dwg_copy_map_set (map, unresolved[i].target, mapped);
    }
  dwg_copy_resolve (dst, map, unresolved, &num_unresolved);
  memset (&readback, 0, sizeof (readback));
  error = write_dwg (file, dst);
  if (error < DWG_ERR_CRITICAL)
    error = read_dwg (file, &readback);
  if (error >= DWG_ERR_CRITICAL || !copy)
    FAIL ("%s: write or read back error 0x%x", what, (unsigned)error);
  else
    {
      const Dwg_Object *read
          = dwg_resolve_handle (&readback, copy->handle.value);
      data = read && read->tio.entity->num_eed == 1
                 ? read->tio.entity->eed[0].data
                 : NULL;
      if (data && data->code == 0 && data->u.eed_0.length == 0
          && !data->u.eed_0.string[0])
        OK ("%s: read back as the empty string", what);
      else
        FAIL ("%s: %s, length %d", what, read ? "read" : "not read",
              data ? (int)data->u.eed_0.length : -1);
    }
  free_dwg (&readback);
  free_dwg (src);
  free (src);
  free_dwg (dst);
  free (dst);
  dwg_copy_map_free (map);
  free (unresolved);
  unresolved = NULL;
  num_unresolved = 0;

  if (!write_unread_eed_drawing (drawing)
      || read_dwg (drawing, &drawn) >= DWG_ERR_CRITICAL)
    {
      FAIL ("%s: no drawing made from example_2000.dwg's \"DesignCenter "
            "Data\" string",
            what);
      unlink (drawing);
      unlink (file);
      return numfailed ();
    }
  obj = NULL;
  for (BITCODE_BL i = 0; i < drawn.num_objects && index < 0; i++)
    {
      obj = &drawn.object[i];
      index = unread_eed_string (obj);
    }
  dst = new_document_like (&drawn);
  map = dwg_copy_map_new ();
  copy = NULL;
  data = NULL;
  if (index >= 0)
    {
      BITCODE_BL num_eed, num_copied;
      const Dwg_Eed *copied;
      handle = obj->handle.value;
      loglevel = tracelevel;
      error = dwg_copy_objects (dst, &drawn, &handle, 1, map, &unresolved,
                                &num_unresolved);
      copy = dwg_resolve_handle (dst, dwg_copy_map_get (map, handle));
      object_eed (obj, &num_eed);
      copied = copy ? object_eed (copy, &num_copied) : NULL;
      if (copied && num_copied == num_eed)
        data = copied[index].data;
    }
  if (index < 0)
    FAIL ("%s: no EED string left unread in the changed drawing", what);
  else if (!error && data && data->code == 0 && data->u.eed_0.length == 0
           && !data->u.eed_0.string[0])
    OK ("%s: %s " FORMAT_HV " EED[%d] in example_2000.dwg copied as the "
        "empty string",
        what, obj->name, obj->handle.value, index);
  else
    FAIL ("%s: %s " FORMAT_HV " EED[%d]: error 0x%x, copied with length %d",
          what, obj->name, obj->handle.value, index, (unsigned)error,
          data ? (int)data->u.eed_0.length : -1);

  free_dwg (&drawn);
  free_dwg (dst);
  free (dst);
  dwg_copy_map_free (map);
  free (unresolved);
  if (!numfailed ())
    {
      unlink (file);
      unlink (drawing);
    }
  return numfailed ();
}

/* A linetype's dash texts, which the decoder points into the linetype's
   strings_area: the copy's point into the copy's strings_area, at the same
   offsets. None of the public drawings has a linetype with text, so a
   linetype of example_2004.dwg gets two text dashes, laid out as the
   decoder lays them out. */
static int
test_ltype_text (void)
{
  const char *what = "LTYPE dash texts";
  const char *file = "copy_ltype_text.dwg";
  char path[256];
  Dwg_Data dwg, readback;
  Dwg_Object_LTYPE *ltype = NULL;
  const Dwg_Object_LTYPE *copy = NULL;
  const Dwg_Object *obj, *copy_obj;
  BITCODE_H style;
  BITCODE_RLL handle = 0;
  Dwg_Copy_Map *map;
  Dwg_Copy_Unresolved *unresolved = NULL;
  BITCODE_BL num_unresolved = 0;
  Compare c;
  int error;

  failed = 0;
  if (!test_file ("example_2004.dwg", path, sizeof (path))
      || read_dwg (path, &dwg) >= DWG_ERR_CRITICAL)
    {
      FAIL ("%s: read failed", what);
      return 1;
    }
  style = dwg_find_tablehandle (&dwg, "Standard", "STYLE");
  for (BITCODE_BL i = 0; i < dwg.num_objects && !ltype; i++)
    if (dwg.object[i].fixedtype == DWG_TYPE_LTYPE
        && dwg.object[i].tio.object->tio.LTYPE->strings_area
        && !dwg.object[i].tio.object->tio.LTYPE->numdashes)
      {
        ltype = dwg.object[i].tio.object->tio.LTYPE;
        handle = dwg.object[i].handle.value;
      }
  if (!ltype || !style)
    {
      FAIL ("%s: no linetype without dashes, or no Standard style", what);
      free_dwg (&dwg);
      return 1;
    }
  free (ltype->dashes);
  ltype->numdashes = 2;
  ltype->dashes = (Dwg_LTYPE_dash *)calloc (2, sizeof (Dwg_LTYPE_dash));
  ltype->pattern_len = 0.75;
  ltype->has_strings_area = 1;
  memset (ltype->strings_area, 0, 256);
  memcpy (ltype->strings_area, "AB", 3);
  memcpy (ltype->strings_area + 3, "CDE", 4);
  for (int i = 0; i < 2; i++)
    {
      ltype->dashes[i].parent = ltype;
      ltype->dashes[i].length = i ? -0.25 : 0.5;
      ltype->dashes[i].shape_flag = 2;
      ltype->dashes[i].scale = 1.0;
      ltype->dashes[i].style
          = dwg_add_handleref (&dwg, 5, style->absolute_ref, NULL);
      ltype->dashes[i].text = (char *)ltype->strings_area + (i ? 3 : 0);
    }

  map = dwg_copy_map_new ();
  loglevel = tracelevel;
  error = dwg_copy_objects (&dwg, &dwg, &handle, 1, map, &unresolved,
                            &num_unresolved);
  copy_obj = dwg_resolve_handle (&dwg, dwg_copy_map_get (map, handle));
  if (copy_obj && copy_obj->fixedtype == DWG_TYPE_LTYPE)
    copy = copy_obj->tio.object->tio.LTYPE;
  if (!error && copy && copy->numdashes == 2 && copy->dashes
      && copy->strings_area && copy->strings_area != ltype->strings_area
      && copy->dashes[0].text == (char *)copy->strings_area
      && copy->dashes[1].text == (char *)copy->strings_area + 3)
    OK ("%s: copied into the copy's strings_area, at offsets 0 and 3", what);
  else
    FAIL ("%s: error 0x%x, copy %p, texts not in the copy's strings_area",
          what, (unsigned)error, (void *)copy);

  memset (&c, 0, sizeof (c));
  c.map = map;
  c.same_document = 1;
  c.version = dwg.header.from_version;
  c.what = what;
  obj = dwg_resolve_handle (&dwg, handle);
  if (copy)
    compare_object (&c, &dwg, obj, &dwg, copy_obj);
  if (copy && !c.failed)
    OK ("%s: the copy has its source's fields", what);
  else
    FAIL ("%s: %d field differences between copy and source", what,
          c.failed);

  memset (&readback, 0, sizeof (readback));
  error = write_dwg (file, &dwg);
  if (error < DWG_ERR_CRITICAL)
    error = read_dwg (file, &readback);
  if (error >= DWG_ERR_CRITICAL || !copy)
    FAIL ("%s: write or read back error 0x%x", what, (unsigned)error);
  else
    {
      const Dwg_Object *read
          = dwg_resolve_handle (&readback, copy_obj->handle.value);
      memset (&c, 0, sizeof (c));
      c.dd_tolerance = 1;
      c.version = dwg.header.from_version;
      c.what = "read back LTYPE";
      if (read)
        compare_object (&c, &dwg, copy_obj, &readback, read);
      if (read && !c.failed)
        OK ("%s: read back the same", what);
      else
        FAIL ("%s: %s, %d differences", what, read ? "read" : "not read",
              c.failed);
    }
  free_dwg (&readback);
  if (!numfailed ())
    unlink (file);
  free_dwg (&dwg);
  dwg_copy_map_free (map);
  free (unresolved);
  return numfailed ();
}

/* A refused copy returns an error from DWG_ERR_CRITICAL up and adds
   nothing to dst. */
static void
check_refused (const char *what, Dwg_Data *dst, Dwg_Data *src,
               const BITCODE_RLL *handles, const BITCODE_BL num_handles,
               Dwg_Copy_Map *map)
{
  const BITCODE_BL num_objects = dst->num_objects;
  const BITCODE_BL num_classes = dst->num_classes;
  Dwg_Copy_Unresolved *unresolved = NULL;
  BITCODE_BL num_unresolved = 0;
  int error;
  loglevel = 0; // the refusal logs an error
  error = dwg_copy_objects (dst, src, handles, num_handles, map, &unresolved,
                            &num_unresolved);
  if (error >= DWG_ERR_CRITICAL && dst->num_objects == num_objects
      && dst->num_classes == num_classes && !num_unresolved)
    OK ("refused: %s (error 0x%x)", what, (unsigned)error);
  else
    FAIL ("refused: %s: error 0x%x, %u objects added", what, (unsigned)error,
          (unsigned)(dst->num_objects - num_objects));
  free (unresolved);
}

static int
test_refused (void)
{
  char path2004[256], path2000[256], path2007[256];
  Dwg_Data src2004, src2000, src2007;
  Dwg_Data *dst;
  Dwg_Copy_Map *map = dwg_copy_map_new ();
  BITCODE_RLL handles[2], unknown = 0, line = 0;

  failed = 0;
  if (!test_file ("example_2004.dwg", path2004, sizeof (path2004))
      || !test_file ("example_2000.dwg", path2000, sizeof (path2000))
      || !test_file ("example_2007.dwg", path2007, sizeof (path2007))
      || read_dwg (path2004, &src2004) >= DWG_ERR_CRITICAL
      || read_dwg (path2000, &src2000) >= DWG_ERR_CRITICAL
      || read_dwg (path2007, &src2007) >= DWG_ERR_CRITICAL)
    {
      FAIL ("refused: read failed");
      return 1;
    }
  for (BITCODE_BL i = 0; i < src2004.num_objects; i++)
    {
      const Dwg_Object *obj = &src2004.object[i];
      if (!unknown && obj->fixedtype == DWG_TYPE_UNKNOWN_OBJ)
        unknown = obj->handle.value;
      if (!line && obj->fixedtype == DWG_TYPE_LINE)
        line = obj->handle.value;
    }
  dst = new_document_like (&src2004);

  handles[0] = src2000.object[src2000.num_objects - 1].handle.value;
  check_refused ("an r2000 object into an r2004 document", dst, &src2000,
                 handles, 1, map);
  handles[0] = src2007.object[src2007.num_objects - 1].handle.value;
  check_refused ("an r2007 document, even within itself", &src2007, &src2007,
                 handles, 1, map);
  handles[0] = unknown;
  check_refused ("an object of a class LibreDWG can't decode", dst, &src2004,
                 handles, 1, map);
  handles[0] = handles[1] = line;
  check_refused ("an object twice", dst, &src2004, handles, 2, map);
  handles[0] = 0x7FFFFFF0;
  check_refused ("a handle not in the document", dst, &src2004, handles, 1,
                 map);
  dwg_copy_map_set (map, line, 0x10);
  handles[0] = line;
  check_refused ("an object already in the map", dst, &src2004, handles, 1,
                 map);

  free_dwg (&src2004);
  free_dwg (&src2000);
  free_dwg (&src2007);
  free_dwg (dst);
  free (dst);
  dwg_copy_map_free (map);
  return numfailed ();
}

/*--------------------------------------------------------------------------------
 * dwg_dynblock_copy_block
 */

/* The drawing whose blocks the block copy tests copy. Its constraint-based
   block, Door Elevation, holds an ACDB_DYNAMICBLOCKPROXYNODE, which
   LibreDWG can't decode, so the copy refuses it, and the one
   representation block that refers to it. */
#define DYNBLOCKS "2004/Dynblocks.dwg"
#define DOOR_ELEVATION "Door Elevation - Imperial"
#define DYNBLOCKS_REFUSED 2

/* A block's name as the block copy matches it, or NULL. */
static const char *
test_block_name (const Dwg_Data *dwg, const Dwg_Object *obj, char *buf,
                 const size_t size)
{
  size_t len;
  const char *name = block_name (dwg, obj, &len);
  if (!name)
    return NULL;
  snprintf (buf, size, "%.*s", (int)len, name);
  return buf;
}

/* The blocks of dwg's block table, only the dynamic definitions or all. */
static const Dwg_Object **
table_blocks (const Dwg_Data *dwg, const int dynamic, BITCODE_BL *nump)
{
  const Dwg_Object_APPID_CONTROL *ctrl
      = control_of (dwg_get_first_object (dwg, DWG_TYPE_BLOCK_CONTROL));
  const Dwg_Object **blocks;
  *nump = 0;
  if (!ctrl || !ctrl->num_entries)
    return NULL;
  blocks
      = (const Dwg_Object **)calloc (ctrl->num_entries, sizeof (Dwg_Object *));
  for (BITCODE_BS i = 0; i < ctrl->num_entries; i++)
    {
      const Dwg_Object *obj
          = ctrl->entries[i]
                ? dwg_resolve_handle (dwg, ctrl->entries[i]->absolute_ref)
                : NULL;
      size_t len;
      if (obj && obj->fixedtype == DWG_TYPE_BLOCK_HEADER
          && (!dynamic || true_name (dwg, obj, &len)))
        blocks[(*nump)++] = obj;
    }
  return blocks;
}

/* The name of a table record, or of a block as the block copy matches it. */
static const char *
record_name (const Dwg_Data *dwg, const Dwg_Object *obj, char *buf,
             const size_t size)
{
  if (obj->fixedtype == DWG_TYPE_BLOCK_HEADER)
    return test_block_name (dwg, obj, buf, size);
  return obj->tio.object && obj->tio.object->tio.APPID
             ? obj->tio.object->tio.APPID->name
             : NULL;
}

/* Each table of dwg lists each of its records once, and has no two
   records, nor two blocks, of the same name. */
static void
check_tables (const char *what, const Dwg_Data *dwg)
{
  const Dwg_Object_Type controls[]
      = { DWG_TYPE_BLOCK_CONTROL, DWG_TYPE_LAYER_CONTROL,
          DWG_TYPE_STYLE_CONTROL, DWG_TYPE_LTYPE_CONTROL,
          DWG_TYPE_APPID_CONTROL, DWG_TYPE_DIMSTYLE_CONTROL };
  unsigned twice = 0, same = 0, records = 0;
  for (size_t t = 0; t < sizeof (controls) / sizeof (controls[0]); t++)
    {
      const Dwg_Object_APPID_CONTROL *ctrl
          = control_of (dwg_get_first_object (dwg, controls[t]));
      if (!ctrl || !ctrl->entries)
        continue;
      for (BITCODE_BS i = 0; i < ctrl->num_entries; i++)
        {
          const Dwg_Object *a
              = ctrl->entries[i]
                    ? dwg_resolve_handle (dwg, ctrl->entries[i]->absolute_ref)
                    : NULL;
          char abuf[256], bbuf[256];
          const char *aname;
          if (!a)
            continue;
          records++;
          aname = record_name (dwg, a, abuf, sizeof (abuf));
          for (BITCODE_BS j = i + 1; j < ctrl->num_entries; j++)
            {
              const Dwg_Object *b
                  = ctrl->entries[j] ? dwg_resolve_handle (
                                           dwg, ctrl->entries[j]->absolute_ref)
                                     : NULL;
              if (!b)
                continue;
              if (a == b)
                twice++;
              else if (aname
                       && same_cstr (
                           aname, record_name (dwg, b, bbuf, sizeof (bbuf))))
                {
                  if (test_loglevel >= 2)
                    printf ("  # %s: %s " FORMAT_HV " and " FORMAT_HV
                            " both named %s\n",
                            what, a->name, a->handle.value, b->handle.value,
                            aname);
                  same++;
                }
            }
        }
    }
  if (!twice && !same)
    OK ("%s: %u table records, each listed once, no two of a name", what,
        records);
  else
    FAIL ("%s: of %u table records, %u listed twice, %u pairs of a name", what,
          records, twice, same);
}

/* Each block from first on lists exactly the INSERTs that draw it, and
   each INSERT from first on is listed by its block. */
static void
check_inserts (const char *what, const Dwg_Data *dwg, const BITCODE_BL first)
{
  unsigned listed = 0, wrong = 0, missing = 0;
  for (BITCODE_BL i = 0; i < dwg->num_objects; i++)
    {
      const Dwg_Object *obj = &dwg->object[i];
      if (obj->fixedtype == DWG_TYPE_BLOCK_HEADER && i >= first)
        {
          const Dwg_Object_BLOCK_HEADER *blk
              = obj->tio.object->tio.BLOCK_HEADER;
          for (BITCODE_BL k = 0; k < blk->num_inserts; k++)
            {
              const Dwg_Object *ins
                  = dwg_resolve_handle (dwg, ref_target (blk->inserts[k]));
              if (ins && ins->fixedtype == DWG_TYPE_INSERT
                  && ref_target (ins->tio.entity->tio.INSERT->block_header)
                         == obj->handle.value)
                listed++;
              else
                wrong++;
            }
        }
      else if (obj->fixedtype == DWG_TYPE_INSERT && i >= first)
        {
          const Dwg_Object *blkobj = dwg_resolve_handle (
              dwg, ref_target (obj->tio.entity->tio.INSERT->block_header));
          const Dwg_Object_BLOCK_HEADER *blk
              = blkobj && blkobj->fixedtype == DWG_TYPE_BLOCK_HEADER
                    ? blkobj->tio.object->tio.BLOCK_HEADER
                    : NULL;
          int found = 0;
          for (BITCODE_BL k = 0; blk && k < blk->num_inserts; k++)
            if (ref_target (blk->inserts[k]) == obj->handle.value)
              found = 1;
          if (!found)
            missing++;
        }
    }
  if (!wrong && !missing)
    OK ("%s: the blocks list their %u INSERTs", what, listed);
  else
    FAIL ("%s: %u INSERTs listed by the wrong block, %u not listed", what,
          wrong, missing);
}

/* A document built with the add API is encoded as a DXF import, as
   dwg_add_Document () sets DWG_OPTS_IN. The encoder then leaves out a
   BLOCK_HEADER's EED of the APPID with handle 0x12, ACAD, such as its
   "DesignCenter Data" (EED_ALLOWED in encode.c, a workaround for a crash
   on a DXF import, LibreDWG #244), and computes TEXT, ATTRIB and ATTDEF
   dataflags afresh (dwg_set_dataflags), which may differ from AutoCAD's
   for the same values. A read back of such a document is compared with
   both allowed for. The encoder also takes its 8-bit strings to be UTF-8,
   as the add API makes them, and converts them to the codepage, while a
   copy holds its source's codepage bytes: characters outside ASCII are
   lost. Until the copy converts its strings, compare_string () reports
   those as TODO. */
#define ADD_API_ACAD_APPID 0x12

/* The EED of a, but for the groups whose APPID map maps to dropped_appid,
   in eed (with room for num_eed items). Returns the number left. */
static BITCODE_BL
eed_without_appid (const Dwg_Eed *a, const BITCODE_BL num_eed,
                   const Dwg_Copy_Map *map, const BITCODE_RLL dropped_appid,
                   Dwg_Eed *eed, unsigned *dropped)
{
  BITCODE_BL n = 0;
  int drop = 0;
  for (BITCODE_BL i = 0; i < num_eed; i++)
    {
      // the first item of a group has its size and APPID
      if (a[i].size)
        {
          drop = dwg_copy_map_get (map, a[i].handle.value) == dropped_appid;
          *dropped += drop;
        }
      if (!drop)
        eed[n++] = a[i];
    }
  return n;
}

/* A copied BLOCK_HEADER has its source's fields but its INSERTs, which
   check_inserts () checks. A read back from a document built with the add
   API lacks its ACAD EED, see ADD_API_ACAD_APPID, which dropped counts. */
static void
compare_block_header (Compare *c, const Dwg_Object *a, const Dwg_Object *b,
                      unsigned *dropped)
{
  const Dwg_Object_Object *oa = a->tio.object;
  Dwg_Object_BLOCK_HEADER ba = *oa->tio.BLOCK_HEADER;
  Dwg_Object_BLOCK_HEADER bb = *b->tio.object->tio.BLOCK_HEADER;
  ba.num_inserts = bb.num_inserts = 0;
  ba.inserts = bb.inserts = NULL;
  compare_fields (c, "", "Object", dwg_dynapi_common_object_fields (), oa,
                  b->tio.object, NULL);
  if (c->add_api_readback && oa->num_eed)
    {
      Dwg_Eed *eed = (Dwg_Eed *)calloc (oa->num_eed, sizeof (Dwg_Eed));
      const BITCODE_BL n = eed_without_appid (
          oa->eed, oa->num_eed, c->map, ADD_API_ACAD_APPID, eed, dropped);
      compare_eed (c, eed, n, b->tio.object->eed, b->tio.object->num_eed);
      free (eed);
    }
  else
    compare_eed (c, oa->eed, oa->num_eed, b->tio.object->eed,
                 b->tio.object->num_eed);
  compare_fields (c, "", "BLOCK_HEADER",
                  dwg_dynapi_entity_fields ("BLOCK_HEADER"), &ba, &bb, NULL);
}

/* A table record the block copy added has its source's fields, but no
   extension dictionary. */
static void
compare_record (Compare *c, const Dwg_Object *a, const Dwg_Object *b)
{
  const Dwg_Object_Object *ob = b->tio.object;
  compare_eed (c, a->tio.object->eed, a->tio.object->num_eed, ob->eed,
               ob->num_eed);
  compare_fields (c, "", a->name, dwg_dynapi_entity_fields (a->name),
                  a->tio.object->tio.APPID, ob->tio.APPID, NULL);
  if (ref_target (ob->xdicobjhandle) || ob->num_reactors
      || (a->tio.object->xdicobjhandle && !ob->is_xdic_missing))
    compare_fail (c, "kept its extension dictionary or reactors");
}

/* What compare_block_copies () found. */
typedef struct _block_copies
{
  unsigned copies;
  unsigned blocks;
  unsigned records;
  unsigned dropped; /* ACAD EED groups of blocks read back, see above */
} Block_Copies;

/* Compares the objects the block copy added to dst, from index first on,
   with their sources in src through map. dst may be read back from the
   document the copies were made in. */
static void
compare_block_copies (Compare *c, Dwg_Data *src, Dwg_Data *dst,
                      const Dwg_Copy_Map *map, const BITCODE_BL first,
                      Block_Copies *found)
{
  memset (found, 0, sizeof (Block_Copies));
  for (BITCODE_BL i = 0; i < src->num_objects; i++)
    {
      const Dwg_Object *a = &src->object[i];
      const BITCODE_RLL h = dwg_copy_map_get (map, a->handle.value);
      const Dwg_Object *b = h ? dwg_resolve_handle (dst, h) : NULL;
      char what[64];
      // not copied, or matched in dst
      if (!b || b->index < first)
        continue;
      found->copies++;
      snprintf (what, sizeof (what), "%s%s " FORMAT_HV,
                c->dd_tolerance ? "read back " : "", a->name, a->handle.value);
      c->what = what;
      if (a->fixedtype == DWG_TYPE_BLOCK_HEADER)
        {
          found->blocks++;
          compare_block_header (c, a, b, &found->dropped);
        }
      else if (dwg_obj_is_table (a))
        {
          found->records++;
          compare_record (c, a, b);
        }
      else
        {
          // the read back's dataflags are computed afresh, see above
          if (c->add_api_readback && b->tio.entity && b->tio.entity->tio.TEXT)
            {
              if (a->fixedtype == DWG_TYPE_TEXT)
                b->tio.entity->tio.TEXT->dataflags
                    = a->tio.entity->tio.TEXT->dataflags;
              else if (a->fixedtype == DWG_TYPE_ATTRIB)
                b->tio.entity->tio.ATTRIB->dataflags
                    = a->tio.entity->tio.ATTRIB->dataflags;
              else if (a->fixedtype == DWG_TYPE_ATTDEF)
                b->tio.entity->tio.ATTDEF->dataflags
                    = a->tio.entity->tio.ATTDEF->dataflags;
            }
          compare_object (c, src, a, dst, b);
        }
    }
}

/* Adds the entries of map for src's objects to total. */
static void
merge_map (Dwg_Copy_Map *total, const Dwg_Copy_Map *map, const Dwg_Data *src)
{
  for (BITCODE_BL i = 0; i < src->num_objects; i++)
    {
      const BITCODE_RLL h
          = dwg_copy_map_get (map, src->object[i].handle.value);
      if (h)
        dwg_copy_map_set (total, src->object[i].handle.value, h);
    }
}

/* The number of times dwg's block table lists block. */
static int
in_block_table (const Dwg_Data *dwg, const BITCODE_RLL block)
{
  const Dwg_Object_APPID_CONTROL *ctrl
      = control_of (dwg_get_first_object (dwg, DWG_TYPE_BLOCK_CONTROL));
  int listed = 0;
  for (BITCODE_BS i = 0; ctrl && ctrl->entries && i < ctrl->num_entries; i++)
    if (ref_target (ctrl->entries[i]) == block)
      listed++;
  return listed;
}

/* Copies blk of src into dst and checks the copy: its objects have their
   sources' fields, and it is in dst's block table. A copy of a block of a
   name dst has already is that block, as is a second copy of the block.
   Adds the copy's map to total. Returns whether blk was copied. */
static int
copy_and_check_block (const char *what, Dwg_Data *dst, Dwg_Data *src,
                      const Dwg_Object *blk, const int refuse,
                      Dwg_Copy_Map *total)
{
  const BITCODE_BL first = dst->num_objects;
  const BITCODE_BS num_classes = dst->num_classes;
  const BITCODE_RLL existing = find_block (dst, src, blk);
  Dwg_Copy_Map *map = dwg_copy_map_new ();
  Dwg_Object_BLOCK_HEADER *copy, *again;
  const Dwg_Object *copyobj;
  Block_Copies found;
  Compare c;
  int error, error2, copied = 0;

  loglevel = refuse ? 0 : tracelevel; // a refusal logs an error
  copy = copy_block (dst, src, blk->tio.object->tio.BLOCK_HEADER, map, &error);
  loglevel = test_loglevel;
  if (refuse)
    {
      if (!copy && error >= DWG_ERR_CRITICAL && dst->num_objects == first
          && dst->num_classes == num_classes)
        OK ("%s: refused (error 0x%x), nothing added", what, (unsigned)error);
      else
        FAIL ("%s: not refused: error 0x%x, %u objects added", what,
              (unsigned)error, (unsigned)(dst->num_objects - first));
      goto done;
    }
  if (existing)
    {
      if (copy && copy->parent->objid < first
          && dst->object[copy->parent->objid].handle.value == existing
          && !error && dst->num_objects == first)
        OK ("%s: dst has a block of its name, " FORMAT_HV
            ", which it returns, adding nothing",
            what, existing);
      else
        FAIL ("%s: dst has a block of its name, " FORMAT_HV
              ", but error 0x%x, %u objects added",
              what, existing, (unsigned)error,
              (unsigned)(dst->num_objects - first));
      goto done;
    }
  if (!copy || error)
    {
      FAIL ("%s: error 0x%x", what, (unsigned)error);
      goto done;
    }
  copied = 1;
  if (total)
    merge_map (total, map, src);
  copyobj = &dst->object[copy->parent->objid];
  memset (&c, 0, sizeof (c));
  c.map = map;
  c.version = src->header.from_version;
  compare_block_copies (&c, src, dst, map, first, &found);
  if (!c.failed && found.copies == dst->num_objects - first
      && copyobj->index >= first)
    OK ("%s: copied as " FORMAT_HV ": %u objects, %u of them blocks and %u "
        "table records, each with its source's fields",
        what, copyobj->handle.value, found.copies, found.blocks,
        found.records);
  else
    FAIL ("%s: %d field differences, %u copies of %u objects added", what,
          c.failed, found.copies, (unsigned)(dst->num_objects - first));
  if (in_block_table (dst, copyobj->handle.value) == 1)
    OK ("%s: listed in dst's block table", what);
  else
    FAIL ("%s: listed %d times in dst's block table", what,
          in_block_table (dst, copyobj->handle.value));

  loglevel = tracelevel;
  again = dwg_dynblock_copy_block (dst, src, blk->tio.object->tio.BLOCK_HEADER,
                                   &error2);
  loglevel = test_loglevel;
  if (again == copy && !error2 && dst->num_objects == first + found.copies)
    OK ("%s: copied again, the first copy, nothing added", what);
  else
    FAIL ("%s: copied again: error 0x%x, %s, %u objects added", what,
          (unsigned)error2, again == copy ? "the first copy" : "not it",
          (unsigned)(dst->num_objects - first - found.copies));

done:
  dwg_copy_map_free (map);
  return copied;
}

/* Writes dst as r2004 and reads it back: each copy, from index first on,
   reads back with its source's fields, compared through map, and every
   reference resolves. */
static void
check_block_readback (const char *what, Dwg_Data *dst, const BITCODE_BL first,
                      Dwg_Data *src, const Dwg_Copy_Map *map, const char *file)
{
  const int add_api = (dst->opts & DWG_OPTS_IN) == DWG_OPTS_IN;
  Dwg_Data readback;
  Block_Copies found;
  Compare c;
  int error;
  memset (&readback, 0, sizeof (readback));
  loglevel = tracelevel;
  error = write_dwg (file, dst);
  if (error < DWG_ERR_CRITICAL)
    error = read_dwg (file, &readback);
  if (error >= DWG_ERR_CRITICAL)
    {
      FAIL ("%s: write or read back error 0x%x", what, (unsigned)error);
      free_dwg (&readback);
      return;
    }
  memset (&c, 0, sizeof (c));
  c.map = map;
  c.dd_tolerance = 1;
  c.add_api_readback = add_api;
  c.version = dst->header.from_version;
  compare_block_copies (&c, src, &readback, map, first, &found);
  if (readback.num_objects == dst->num_objects
      && found.copies == dst->num_objects - first && !c.failed)
    OK ("%s: %u objects read back, the %u copies with their sources' "
        "fields, but %u PROXY data last bytes and %u ACAD EED groups of "
        "blocks (not written into an add API document)",
        what, (unsigned)readback.num_objects, found.copies, c.unwritten,
        found.dropped);
  else
    FAIL ("%s: %u objects read back of %u, %u copies of %u, %d field "
          "differences",
          what, (unsigned)readback.num_objects, (unsigned)dst->num_objects,
          found.copies, (unsigned)(dst->num_objects - first), c.failed);
  if (c.non_ascii)
    {
      loglevel = test_loglevel;
      todo ("%s: %u strings with characters outside ASCII lose them in a "
            "document built with the add API",
            what, c.non_ascii);
    }
  check_refs (what, &readback, 0);
  check_tables (what, &readback);
  check_inserts (what, &readback, first);
  free_dwg (&readback);
  if (!numfailed () && !getenv ("COPY_TEST_KEEP"))
    unlink (file);
}

/* Copies the dynamic block definitions of the fixture into a new r2004
   document, or into the public drawing dst_name. */
static int
test_copy_dynblocks (const char *dst_name)
{
  char path[256], file[128], what[300];
  Dwg_Data src, dstdwg;
  Dwg_Data *dst;
  Dwg_Copy_Map *total;
  const Dwg_Object **blocks;
  BITCODE_BL num_blocks, first;
  unsigned copied = 0;

  failed = 0;
  if (!test_file (DYNBLOCKS, path, sizeof (path))
      || read_dwg (path, &src) >= DWG_ERR_CRITICAL)
    {
      FAIL ("copy dynamic blocks: %s read failed", DYNBLOCKS);
      return 1;
    }
  if (dst_name)
    {
      char dst_path[256];
      if (!test_file (dst_name, dst_path, sizeof (dst_path))
          || read_dwg (dst_path, &dstdwg) >= DWG_ERR_CRITICAL)
        {
          FAIL ("copy dynamic blocks: %s read failed", dst_name);
          free_dwg (&src);
          return 1;
        }
      dst = &dstdwg;
    }
  else
    dst = dwg_new_Document (R_2004, 0, tracelevel);
  loglevel = test_loglevel;
  first = dst->num_objects;
  total = dwg_copy_map_new ();
  blocks = table_blocks (&src, 1, &num_blocks);
  for (BITCODE_BL i = 0; i < num_blocks; i++)
    {
      char name[256];
      test_block_name (&src, blocks[i], name, sizeof (name));
      snprintf (what, sizeof (what), "copy %s into %s", name,
                dst_name ? dst_name : "a new document");
      copied += copy_and_check_block (what, dst, &src, blocks[i],
                                      strEQc (name, DOOR_ELEVATION), total);
    }
  snprintf (what, sizeof (what), "copy dynamic blocks into %s",
            dst_name ? dst_name : "a new document");
  if (num_blocks == 6 && copied == num_blocks - 1)
    OK ("%s: %u of the %u definitions copied", what, copied,
        (unsigned)num_blocks);
  else
    FAIL ("%s: %u of the %u definitions copied", what, copied,
          (unsigned)num_blocks);
  check_tables (what, dst);
  check_inserts (what, dst, first);
  check_refs (what, dst, 0);
  if (dst->block_control.num_entries
      == control_of (dwg_get_first_object (dst, DWG_TYPE_BLOCK_CONTROL))
             ->num_entries)
    OK ("%s: dwg->block_control has the table's %u entries", what,
        (unsigned)dst->block_control.num_entries);
  else
    FAIL ("%s: dwg->block_control has %u entries", what,
          (unsigned)dst->block_control.num_entries);
  snprintf (file, sizeof (file), "copy_dynblocks_into_%s.dwg",
            dst_name ? "example_2004" : "new");
  check_block_readback (what, dst, first, &src, total, file);

  free (blocks);
  dwg_copy_map_free (total);
  free_dwg (&src);
  free_dwg (dst);
  if (!dst_name)
    free (dst);
  return numfailed ();
}

/* Copies every block of the fixture's block table into a new document,
   plain and anonymous ones too, then example_2004's plain blocks into
   another. Each copy is compared with its source; the refused ones are
   Door Elevation and the representation block that refers to it. */
static int
test_copy_all_blocks (void)
{
  const char *what = "copy every block";
  char path[256], name[256], blkwhat[300];
  Dwg_Data src, src2;
  Dwg_Data *dst;
  Dwg_Copy_Map *total;
  const Dwg_Object **blocks;
  BITCODE_BL num_blocks, first;
  unsigned refused = 0, copied = 0, existing = 0, layouts = 0, objects = 0;
  Compare c;

  failed = 0;
  if (!test_file (DYNBLOCKS, path, sizeof (path))
      || read_dwg (path, &src) >= DWG_ERR_CRITICAL)
    {
      FAIL ("%s: %s read failed", what, DYNBLOCKS);
      return 1;
    }
  dst = dwg_new_Document (R_2004, 0, tracelevel);
  loglevel = test_loglevel;
  first = dst->num_objects;
  total = dwg_copy_map_new ();
  memset (&c, 0, sizeof (c));
  c.version = src.header.from_version;
  blocks = table_blocks (&src, 0, &num_blocks);
  for (BITCODE_BL i = 0; i < num_blocks; i++)
    {
      const Dwg_Object *blk = blocks[i];
      const BITCODE_BL before = dst->num_objects;
      Dwg_Copy_Map *map;
      Dwg_Object_BLOCK_HEADER *copy;
      Block_Copies found;
      int error;
      if (!is_copyable_block (&src, blk))
        {
          layouts++;
          continue;
        }
      // copied already, with a block that inserts it
      if (find_block (dst, &src, blk))
        {
          existing++;
          continue;
        }
      snprintf (blkwhat, sizeof (blkwhat), "copy %s " FORMAT_HV,
                test_block_name (&src, blk, name, sizeof (name))
                    ? name
                    : blk->tio.object->tio.BLOCK_HEADER->name,
                blk->handle.value);
      map = dwg_copy_map_new ();
      loglevel = 0; // a refusal logs an error
      copy = copy_block (dst, &src, blk->tio.object->tio.BLOCK_HEADER, map,
                         &error);
      loglevel = test_loglevel;
      if (!copy && error >= DWG_ERR_CRITICAL && dst->num_objects == before)
        refused++;
      else if (!copy || error)
        FAIL ("%s: error 0x%x, %u objects added", blkwhat, (unsigned)error,
              (unsigned)(dst->num_objects - before));
      else
        {
          copied++;
          merge_map (total, map, &src);
          c.map = map;
          compare_block_copies (&c, &src, dst, map, before, &found);
          objects += found.copies;
          if (found.copies != dst->num_objects - before)
            FAIL ("%s: %u copies of %u objects added", blkwhat, found.copies,
                  (unsigned)(dst->num_objects - before));
        }
      dwg_copy_map_free (map);
    }
  if (refused == DYNBLOCKS_REFUSED && !c.failed && !numfailed ())
    OK ("%s: %u blocks copied, %u objects in all, each with its source's "
        "fields; %u copied already with others, %u refused, %u layout "
        "blocks left",
        what, copied, objects, existing, refused, layouts);
  else
    FAIL ("%s: %u blocks copied, %u refused, %d field differences", what,
          copied, refused, c.failed);
  free (blocks);
  check_tables (what, dst);
  check_inserts (what, dst, first);
  check_refs (what, dst, 0);
  check_block_readback (what, dst, first, &src, total, "copy_every_block.dwg");
  free_dwg (dst);
  free (dst);
  dwg_copy_map_free (total);
  free_dwg (&src);

  // example_2004's plain blocks into another new document
  what = "copy example_2004's blocks";
  if (!test_file ("example_2004.dwg", path, sizeof (path))
      || read_dwg (path, &src2) >= DWG_ERR_CRITICAL)
    {
      FAIL ("%s: example_2004.dwg read failed", what);
      return numfailed ();
    }
  dst = dwg_new_Document (R_2004, 0, tracelevel);
  loglevel = test_loglevel;
  first = dst->num_objects;
  total = dwg_copy_map_new ();
  blocks = table_blocks (&src2, 0, &num_blocks);
  copied = 0;
  for (BITCODE_BL i = 0; i < num_blocks; i++)
    if (test_block_name (&src2, blocks[i], name, sizeof (name)))
      {
        snprintf (blkwhat, sizeof (blkwhat), "copy example_2004's %s", name);
        copied
            += copy_and_check_block (blkwhat, dst, &src2, blocks[i], 0, total);
      }
  if (copied == 2)
    OK ("%s: its 2 named blocks copied", what);
  else
    FAIL ("%s: %u named blocks copied", what, copied);
  check_block_readback (what, dst, first, &src2, total,
                        "copy_example_2004_blocks.dwg");
  free (blocks);
  free_dwg (dst);
  free (dst);
  dwg_copy_map_free (total);
  free_dwg (&src2);
  return numfailed ();
}

/* The block of dwg with the true name, or NULL. */
static const Dwg_Object *
dynamic_block (const Dwg_Data *dwg, const char *name)
{
  BITCODE_BL num_blocks;
  const Dwg_Object **blocks = table_blocks (dwg, 1, &num_blocks);
  const Dwg_Object *found = NULL;
  for (BITCODE_BL i = 0; i < num_blocks && !found; i++)
    {
      char buf[256];
      if (same_cstr (test_block_name (dwg, blocks[i], buf, sizeof (buf)),
                     name))
        found = blocks[i];
    }
  free (blocks);
  return found;
}

/* The number of objects of type, from index first on. */
static unsigned
count_type (const Dwg_Data *dwg, const BITCODE_BL first,
            const Dwg_Object_Type type)
{
  unsigned n = 0;
  for (BITCODE_BL i = first; i < dwg->num_objects; i++)
    if (dwg->object[i].fixedtype == type)
      n++;
  return n;
}

/* An object that a definition's extension dictionary names as its owner
   but doesn't list is left out: Door's ACAD_SORTENTS entry is taken out of
   its dictionary for the copy, as if AutoCAD had replaced its sort table
   with another. */
static int
test_copy_block_orphan (void)
{
  const char *what = "copy a block with an unlisted sort table";
  char path[256];
  Dwg_Data src;
  Dwg_Data *dst;
  const Dwg_Object *door, *xdic;
  Dwg_Object_DICTIONARY *dict;
  BITCODE_BL k, last, first;
  BITCODE_T text;
  BITCODE_H item;
  unsigned listed, unlisted;
  int error;

  failed = 0;
  if (!test_file (DYNBLOCKS, path, sizeof (path))
      || read_dwg (path, &src) >= DWG_ERR_CRITICAL)
    {
      FAIL ("%s: %s read failed", what, DYNBLOCKS);
      return 1;
    }
  door = dynamic_block (&src, "Door - Imperial");
  xdic = door ? dwg_resolve_handle (
                    &src, ref_target (door->tio.object->xdicobjhandle))
              : NULL;
  dict = xdic ? xdic->tio.object->tio.DICTIONARY : NULL;
  for (k = 0; dict && k < dict->numitems; k++)
    if (same_cstr (dict->texts[k], "ACAD_SORTENTS"))
      break;
  if (!dict || k == dict->numitems)
    {
      FAIL ("%s: Door's ACAD_SORTENTS not found", what);
      free_dwg (&src);
      return 1;
    }

  dst = dwg_new_Document (R_2004, 0, tracelevel);
  first = dst->num_objects;
  loglevel = tracelevel;
  (void)dwg_dynblock_copy_block (dst, &src, door->tio.object->tio.BLOCK_HEADER,
                                 &error);
  loglevel = test_loglevel;
  listed = count_type (dst, first, DWG_TYPE_SORTENTSTABLE);
  free_dwg (dst);
  free (dst);

  // the entry moves to the end, out of numitems
  last = dict->numitems - 1;
  text = dict->texts[k];
  item = dict->itemhandles[k];
  dict->texts[k] = dict->texts[last];
  dict->itemhandles[k] = dict->itemhandles[last];
  dict->texts[last] = text;
  dict->itemhandles[last] = item;
  dict->numitems--;
  dst = dwg_new_Document (R_2004, 0, tracelevel);
  first = dst->num_objects;
  loglevel = tracelevel;
  (void)dwg_dynblock_copy_block (dst, &src, door->tio.object->tio.BLOCK_HEADER,
                                 &error);
  loglevel = test_loglevel;
  unlisted = count_type (dst, first, DWG_TYPE_SORTENTSTABLE);
  dict->numitems++;
  if (listed == 1 && !unlisted && !error)
    OK ("%s: listed, the sort table is copied; unlisted, it is left out",
        what);
  else
    FAIL ("%s: %u sort tables copied while listed, %u unlisted, error 0x%x",
          what, listed, unlisted, (unsigned)error);
  check_refs (what, dst, 0);
  free_dwg (dst);
  free (dst);
  free_dwg (&src);
  return numfailed ();
}

/* Colours survive the copy and a write and read back: entities of a
   definition ByLayer, ByBlock, in an ACI colour and in a true colour, and
   the colours of the layers copied with a definition, alteZZa's. The
   fixture's blocks have no true colour, so a source entity is given the one
   AutoCAD writes for RGB 0,159,223, as LibreDWG decodes it. */
static int
test_copy_block_colours (void)
{
  const char *what = "copy colours";
  const Dwg_Color colors[] = {
    { .index = 256, .flag = 1 },                       // ByLayer
    { .index = 0 },                                    // ByBlock
    { .index = 3 },                                    // ACI green
    { .index = 142, .rgb = 0xc2009fdf, .flag = 0x80 }, // true colour
  };
  const size_t num_colors = sizeof (colors) / sizeof (colors[0]);
  char path[256];
  Dwg_Data src, readback;
  Dwg_Data *dst;
  const Dwg_Object *window, *altezza;
  const Dwg_Object_BLOCK_HEADER *blk;
  Dwg_Copy_Map *map, *map2 = NULL;
  Dwg_Object_BLOCK_HEADER *copy;
  BITCODE_RLL handles[4], layer_handles[16];
  Dwg_Color layer_colors[16];
  BITCODE_BL first;
  unsigned same = 0, layers = 0, num_layers = 0;
  int error;

  failed = 0;
  if (!test_file (DYNBLOCKS, path, sizeof (path))
      || read_dwg (path, &src) >= DWG_ERR_CRITICAL)
    {
      FAIL ("%s: %s read failed", what, DYNBLOCKS);
      return 1;
    }
  window = dynamic_block (&src, "Aluminum Window (Elevation) - Imperial");
  blk = window ? window->tio.object->tio.BLOCK_HEADER : NULL;
  if (!blk || blk->num_owned < num_colors)
    {
      FAIL ("%s: Window not found", what);
      free_dwg (&src);
      return 1;
    }
  for (size_t i = 0; i < num_colors; i++)
    {
      Dwg_Object *ent
          = dwg_resolve_handle (&src, ref_target (blk->entities[i]));
      ent->tio.entity->color = colors[i];
    }

  dst = dwg_new_Document (R_2004, 0, tracelevel);
  first = dst->num_objects;
  map = dwg_copy_map_new ();
  loglevel = tracelevel;
  copy = copy_block (dst, &src, blk, map, &error);
  loglevel = test_loglevel;
  if (!copy || error)
    {
      FAIL ("%s: error 0x%x", what, (unsigned)error);
      free_dwg (&src);
      goto done;
    }
  for (size_t i = 0; i < num_colors; i++)
    handles[i] = dwg_copy_map_get (map, ref_target (blk->entities[i]));
  // the layers dst lacked, copied with alteZZa
  altezza = dynamic_block (&src, "alteZZa");
  map2 = dwg_copy_map_new ();
  loglevel = tracelevel;
  copy = altezza
             ? copy_block (dst, &src, altezza->tio.object->tio.BLOCK_HEADER,
                           map2, &error)
             : NULL;
  loglevel = test_loglevel;
  if (!copy || error)
    {
      FAIL ("%s: alteZZa error 0x%x", what, (unsigned)error);
      free_dwg (&src);
      goto done;
    }
  for (BITCODE_BL i = 0; i < src.num_objects && num_layers < 16; i++)
    {
      const Dwg_Object *a = &src.object[i];
      const Dwg_Object *b
          = dwg_resolve_handle (dst, dwg_copy_map_get (map2, a->handle.value));
      if (a->fixedtype == DWG_TYPE_LAYER && b && b->index >= first)
        {
          layer_handles[num_layers] = b->handle.value;
          layer_colors[num_layers++] = a->tio.object->tio.LAYER->color;
        }
    }
  free_dwg (&src);

  memset (&readback, 0, sizeof (readback));
  loglevel = tracelevel;
  error = write_dwg ("copy_colours.dwg", dst);
  if (error < DWG_ERR_CRITICAL)
    error = read_dwg ("copy_colours.dwg", &readback);
  if (error >= DWG_ERR_CRITICAL)
    FAIL ("%s: write or read back error 0x%x", what, (unsigned)error);
  else
    {
      for (size_t i = 0; i < num_colors; i++)
        {
          const Dwg_Object *ent = dwg_resolve_handle (&readback, handles[i]);
          const Dwg_Color *col = ent ? &ent->tio.entity->color : NULL;
          if (col && col->index == colors[i].index && col->rgb == colors[i].rgb
              && col->flag == colors[i].flag)
            same++;
          else
            FAIL ("%s: entity %u read back %d/%x/%x, expected %d/%x/%x", what,
                  (unsigned)i, col ? col->index : -1, col ? col->rgb : 0,
                  col ? col->flag : 0, colors[i].index, colors[i].rgb,
                  colors[i].flag);
        }
      for (unsigned i = 0; i < num_layers; i++)
        {
          const Dwg_Object *layer
              = dwg_resolve_handle (&readback, layer_handles[i]);
          const Dwg_Color *col
              = layer ? &layer->tio.object->tio.LAYER->color : NULL;
          if (col && col->index == layer_colors[i].index
              && col->rgb == layer_colors[i].rgb
              && col->method == layer_colors[i].method)
            layers++;
          else
            FAIL ("%s: layer " FORMAT_HV " read back %d/%x, expected %d/%x",
                  what, layer_handles[i], col ? col->index : -1,
                  col ? col->rgb : 0, layer_colors[i].index,
                  layer_colors[i].rgb);
        }
    }
  if (same == num_colors && layers && layers == num_layers)
    OK ("%s: ByLayer, ByBlock, ACI and true colour entities read back so, "
        "and the %u layers copied with alteZZa in their colours",
        what, layers);
  else
    FAIL ("%s: %u of %u entity colours and %u of %u layer colours read "
          "back",
          what, same, (unsigned)num_colors, layers, num_layers);
  free_dwg (&readback);
  if (!numfailed ())
    unlink ("copy_colours.dwg");

done:
  dwg_copy_map_free (map);
  dwg_copy_map_free (map2);
  free_dwg (dst);
  free (dst);
  return numfailed ();
}

/* A refused block copy returns NULL with an error from DWG_ERR_CRITICAL up,
   with the reason bit, and adds nothing to dst. */
static void
check_block_refused (const char *what, Dwg_Data *dst, const Dwg_Data *src,
                     const Dwg_Object_BLOCK_HEADER *blk, const int reason)
{
  const BITCODE_BL num_objects = dst ? dst->num_objects : 0;
  const BITCODE_BS num_classes = dst ? dst->num_classes : 0;
  Dwg_Object_BLOCK_HEADER *copy;
  int error;
  loglevel = 0; // the refusal logs an error
  copy = dwg_dynblock_copy_block (dst, src, blk, &error);
  loglevel = test_loglevel;
  if (!copy && error >= DWG_ERR_CRITICAL && (error & reason)
      && (!dst
          || (dst->num_objects == num_objects
              && dst->num_classes == num_classes)))
    OK ("block copy refused: %s (error 0x%x)", what, (unsigned)error);
  else
    FAIL ("block copy refused: %s: error 0x%x, %u objects added", what,
          (unsigned)error,
          dst ? (unsigned)(dst->num_objects - num_objects) : 0);
}

static int
test_copy_block_refused (void)
{
  char path[256], path2000[256], path2004[256];
  Dwg_Data src, src2000, src2004;
  Dwg_Data *dst;
  const Dwg_Object *window, *door_elevation, *ms, *ps, *blk2000 = NULL;
  BITCODE_BL num_blocks;
  const Dwg_Object **blocks;

  failed = 0;
  if (!test_file (DYNBLOCKS, path, sizeof (path))
      || !test_file ("example_2000.dwg", path2000, sizeof (path2000))
      || !test_file ("example_2004.dwg", path2004, sizeof (path2004))
      || read_dwg (path, &src) >= DWG_ERR_CRITICAL
      || read_dwg (path2000, &src2000) >= DWG_ERR_CRITICAL
      || read_dwg (path2004, &src2004) >= DWG_ERR_CRITICAL)
    {
      FAIL ("block copy refused: read failed");
      return 1;
    }
  dst = dwg_new_Document (R_2004, 0, tracelevel);
  window = dynamic_block (&src, "Aluminum Window (Elevation) - Imperial");
  door_elevation = dynamic_block (&src, DOOR_ELEVATION);
  ms = dwg_model_space_object (&src);
  ps = dwg_paper_space_object (&src);
  blocks = table_blocks (&src2000, 0, &num_blocks);
  for (BITCODE_BL i = 0; i < num_blocks && !blk2000; i++)
    if (is_copyable_block (&src2000, blocks[i]))
      blk2000 = blocks[i];
  free (blocks);
  if (!window || !door_elevation || !ms || !ps || !blk2000)
    {
      FAIL ("block copy refused: blocks not found");
      goto done;
    }

  check_block_refused ("no dst", NULL, &src,
                       window->tio.object->tio.BLOCK_HEADER,
                       DWG_ERR_INTERNALERROR);
  check_block_refused ("no src", dst, NULL,
                       window->tio.object->tio.BLOCK_HEADER,
                       DWG_ERR_INTERNALERROR);
  check_block_refused ("no block", dst, &src, NULL, DWG_ERR_INTERNALERROR);
  check_block_refused ("within one document", &src, &src,
                       window->tio.object->tio.BLOCK_HEADER,
                       DWG_ERR_INTERNALERROR);
  check_block_refused ("an r2000 source", dst, &src2000,
                       blk2000->tio.object->tio.BLOCK_HEADER,
                       DWG_ERR_NOTYETSUPPORTED);
  check_block_refused ("into an r2000 document", &src2000, &src,
                       window->tio.object->tio.BLOCK_HEADER,
                       DWG_ERR_NOTYETSUPPORTED);
  dst->header.codepage++;
  check_block_refused ("another codepage", dst, &src,
                       window->tio.object->tio.BLOCK_HEADER,
                       DWG_ERR_NOTYETSUPPORTED);
  dst->header.codepage--;
  check_block_refused ("a block of another document", dst, &src2004,
                       window->tio.object->tio.BLOCK_HEADER,
                       DWG_ERR_INVALIDHANDLE);
  check_block_refused ("model space", dst, &src,
                       ms->tio.object->tio.BLOCK_HEADER, DWG_ERR_INVALIDTYPE);
  check_block_refused ("paper space", dst, &src,
                       ps->tio.object->tio.BLOCK_HEADER, DWG_ERR_INVALIDTYPE);
  check_block_refused ("a constraint-based block", dst, &src,
                       door_elevation->tio.object->tio.BLOCK_HEADER,
                       DWG_ERR_NOTYETSUPPORTED);

done:
  free_dwg (&src);
  free_dwg (&src2000);
  free_dwg (&src2004);
  free_dwg (dst);
  free (dst);
  return numfailed ();
}

/* The first object of the type in dwg owned by an entry of the named
   object dictionary named dict, or NULL. */
static const Dwg_Object *
dictionary_object (const Dwg_Data *dwg, const char *dict,
                   const Dwg_Object_Type type)
{
  const Dwg_Object *nod = named_object_dictionary (dwg);
  const BITCODE_RLL d = dictionary_entry (dictionary_of (nod), dict);
  for (BITCODE_BL i = 0; d && i < dwg->num_objects; i++)
    if (dwg->object[i].fixedtype == type && owner_of (&dwg->object[i]) == d)
      return &dwg->object[i];
  return NULL;
}

/* How the block copy matches what it doesn't copy, and finds what it
   copies: styles of the named object dictionary by key, but not scales,
   whose keys are arbitrary; text styles of shape files by their files; an
   object met before its owner is copied once its owner is. */
static int
test_copy_block_matching (void)
{
  const char *what = "block copy matching";
  char path[256];
  Dwg_Data src;
  const Dwg_Object *plotstyle, *mlinestyle, *scale, *window, *graph = NULL,
                                                             *param = NULL;
  Dwg_Object_Object oa, ob;
  Dwg_Object_STYLE sa, sb;
  Dwg_Object a, b;
  Block_Copy bc;
  Dwg_Copy_Unresolved u;

  failed = 0;
  if (!test_file (DYNBLOCKS, path, sizeof (path))
      || read_dwg (path, &src) >= DWG_ERR_CRITICAL)
    {
      FAIL ("%s: %s read failed", what, DYNBLOCKS);
      return 1;
    }
  plotstyle
      = dictionary_object (&src, "ACAD_PLOTSTYLENAME", DWG_TYPE_PLACEHOLDER);
  mlinestyle
      = dictionary_object (&src, "ACAD_MLINESTYLE", DWG_TYPE_MLINESTYLE);
  scale = dictionary_object (&src, "ACAD_SCALELIST", DWG_TYPE_SCALE);
  if (plotstyle && mlinestyle && scale
      && find_dictionary_entry (&src, &src, plotstyle)
             == plotstyle->handle.value
      && find_dictionary_entry (&src, &src, mlinestyle)
             == mlinestyle->handle.value
      && !find_dictionary_entry (&src, &src, scale))
    OK ("%s: a plot style and a multiline style match the entry of their "
        "key, a scale doesn't",
        what);
  else
    FAIL ("%s: plot style %p, multiline style %p, scale %p", what,
          (void *)plotstyle, (void *)mlinestyle, (void *)scale);

  // text styles of shape files have no names
  memset (&a, 0, sizeof (a));
  memset (&b, 0, sizeof (b));
  memset (&oa, 0, sizeof (oa));
  memset (&ob, 0, sizeof (ob));
  memset (&sa, 0, sizeof (sa));
  memset (&sb, 0, sizeof (sb));
  a.fixedtype = b.fixedtype = DWG_TYPE_STYLE;
  a.tio.object = &oa;
  b.tio.object = &ob;
  oa.tio.STYLE = &sa;
  ob.tio.STYLE = &sb;
  sa.name = sb.name = (char *)"";
  sa.is_shape = sb.is_shape = 1;
  sa.font_file = (char *)"ltypeshp.shx";
  sb.font_file = (char *)"myshapes.shx";
  if (!same_record (&a, &b))
    {
      sb.font_file = (char *)"LTYPESHP.SHX";
      if (same_record (&a, &b))
        {
          sa.name = (char *)"Standard";
          sb.name = (char *)"STANDARD";
          sb.font_file = (char *)"myshapes.shx";
          if (same_record (&a, &b))
            OK ("%s: unnamed text styles match by their shape files, named "
                "ones by name",
                what);
          else
            FAIL ("%s: named text styles of other names", what);
        }
      else
        FAIL ("%s: shape text styles of one file don't match", what);
    }
  else
    FAIL ("%s: shape text styles of different files match", what);

  // Window's linear parameter, met before its evaluation graph is copied
  window = dynamic_block (&src, "Aluminum Window (Elevation) - Imperial");
  for (BITCODE_BL i = 0; window && i < src.num_objects && !param; i++)
    if (src.object[i].fixedtype == DWG_TYPE_BLOCKLINEARPARAMETER)
      {
        const Dwg_Object *g
            = dwg_resolve_handle (&src, owner_of (&src.object[i]));
        const Dwg_Object *xdic
            = g ? dwg_resolve_handle (&src, owner_of (g)) : NULL;
        if (xdic && owner_of (xdic) == window->handle.value)
          {
            param = &src.object[i];
            graph = g;
          }
      }
  memset (&bc, 0, sizeof (bc));
  bc.src = &src;
  bc.dst = &src;
  bc.role = hash_new (16);
  bc.decided = hash_new (16);
  memset (&u, 0, sizeof (u));
  u.kind = DWG_COPY_SLOT_REF;
  u.code = 4;
  u.target = param ? param->handle.value : 0;
  if (param)
    {
      int deferred, copied;
      classify (&bc, &u);
      deferred = hash_get (bc.decided, u.target) == DECIDED_DEFERRED;
      add_copy (&bc, graph->handle.value, ROLE_PART);
      copy_deferred (&bc);
      copied = hash_get (bc.role, u.target) == ROLE_PART;
      if (deferred && copied && !bc.deferred.num)
        OK ("%s: a parameter met before its graph is copied with it", what);
      else
        FAIL ("%s: parameter deferred %d, copied %d", what, deferred, copied);
    }
  else
    FAIL ("%s: Window's linear parameter not found", what);
  free (bc.copy.handles);
  free (bc.pending.handles);
  free (bc.deferred.handles);
  hash_free (bc.role);
  hash_free (bc.decided);
  free_dwg (&src);
  return numfailed ();
}

int
main (int argc, char *argv[])
{
  const char *const r2004[]
      = { "example_2004.dwg",     "2004/Arc.dwg",       "2004/circle.dwg",
          "2004/Constraints.dwg", "2004/ConstructionLine.dwg",
          "2004/Donut.dwg",       "2004/Ellipse.dwg",   "2004/HatchG.dwg",
          "2004/Helix.dwg",       "2004/Leader.dwg",    "2004/Line.dwg",
          "2004/material.dwg",    "2004/Multiline.dwg", "2004/Point.dwg",
          "2004/PolyLine3D.dwg",  "2004/Polygon.dwg",   "2004/Polyline.dwg",
          "2004/RAY.dwg",         "2004/Spline.dwg",    "2004/Surface.dwg",
          "2004/Text.dwg",        "2004/Underlay.dwg",  "2004/Dynblocks.dwg",
          NULL };
  const char *const r2000[]
      = { "example_2000.dwg",     "sample_2000.dwg",    "2000/Arc.dwg",
          "2000/circle.dwg",      "2000/Cone.dwg",      "2000/Constraints.dwg",
          "2000/ConstructionLine.dwg", "2000/Donut.dwg", "2000/Ellipse.dwg",
          "2000/entities-2d.dwg", "2000/entities-3d.dwg", "2000/Helix.dwg",
          "2000/Leader.dwg",      "2000/Line.dwg",      "2000/Multiline.dwg",
          "2000/Point.dwg",       "2000/PolyLine2D.dwg", "2000/PolyLine3D.dwg",
          "2000/Polygon.dwg",     "2000/Polyline.dwg",  "2000/RAY.dwg",
          "2000/Spline.dwg",      "2000/Text.dwg",      "2000/TS1.dwg",
          NULL };
  int error = 0;
  char *trace = getenv ("LIBREDWG_TRACE");
  char *reported = getenv ("COPY_TEST_REPORTED");
  test_loglevel = is_make_silent () ? 0 : 2; // print ok
  tracelevel = trace ? atoi (trace) : 0;
  loglevel = test_loglevel;
  if (reported)
    max_reported = atoi (reported);

  // other drawings, as their own version
  if (argc > 1)
    {
      for (int i = 1; i < argc; i++)
        error += test_clone (argv[i], 0);
      return error ? 1 : 0;
    }

  for (const char *const *f = r2004; *f; f++)
    error += test_clone (*f, 0);
  for (const char *const *f = r2000; *f; f++)
    error += test_clone (*f, 0);
  error += test_clone ("example_2000.dwg", R_2004);
  error += test_same_document ("example_2004.dwg");
  error += test_same_document ("example_2000.dwg");
  error += test_unresolved ();
  error += test_eed ();
  error += test_eed_unread_string ();
  error += test_ltype_text ();
  error += test_refused ();
  error += test_copy_dynblocks (NULL);
  error += test_copy_dynblocks ("example_2004.dwg");
  error += test_copy_all_blocks ();
  error += test_copy_block_orphan ();
  error += test_copy_block_colours ();
  error += test_copy_block_refused ();
  error += test_copy_block_matching ();
  return error ? 1 : 0;
}
