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
   - EED;
   - linetype dash texts in the linetype's strings_area;
   - refused copies. */

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
  if (la != lb || (la && memcmp (a, b, la)))
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
      if (c->map && b[i].raw)
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
  if (c->map
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
          "2004/Text.dwg",        "2004/Underlay.dwg",  NULL };
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
  error += test_ltype_text ();
  error += test_refused ();
  return error ? 1 : 0;
}
