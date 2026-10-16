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
 * copy.c: deep copy of objects from one document into another, or within
 * one document, with every handle remapped to the copies.
 *
 * The per-class code is generated from the specs, as free.c's is. Each copy
 * starts as a byte copy of its source structs, and the spec walk then
 * replaces every pointer in it with a copy of what it points to: strings,
 * binary chunks, arrays and the structs in them, handle references and
 * EED. IS_FREE is defined so that the walk takes the same paths as
 * dwg_free: a pointer that free releases is one the copy must duplicate.
 * Those paths visit some fields twice, so each copy keeps the set of slots
 * it has already replaced.
 */

#include "config.h"
#ifdef __STDC_ALLOC_LIB__
#  define __STDC_WANT_LIB_EXT2__ 1 /* for strdup */
#else
#  define _USE_BSD 1
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <assert.h>

#include "common.h"
#include "bits.h"
#include "dwg.h"
#include "decode.h"
#include "encode.h"
#include "classes.h"
#include "hash.h"
#include "copy.h"

#define DWG_LOGLEVEL loglevel
#include "logging.h"
#include "dwg_api.h"

/* in dwg.c */
void dwg_set_next_objhandle (Dwg_Object *obj);
void dwg_set_handle_size (Dwg_Handle *restrict hdl);

/* The loop counters shared by the generated functions, as in free.c, one
   set per thread. */
static DWG_THREAD_LOCAL BITCODE_BL rcount1, rcount2;

/* What a refused copy returns, with a bit for the reason. */
#define COPY_REFUSED DWG_ERR_INVALIDDWG

struct _dwg_copy_map
{
  dwg_inthash *hash;
};

/* The kinds of document, by the import flags their opts carry. The encoder
   writes some fields of each kind differently. */
typedef enum COPY_KIND
{
  COPY_KIND_DWG,     /* read from a DWG file */
  COPY_KIND_DXF,     /* imported from DXF */
  COPY_KIND_JSON,    /* imported from JSON */
  COPY_KIND_ADD_API, /* built with the add API, which sets both flags */
} Copy_Kind;

/* How the copy converts 8-bit strings (TV, and T before r2007). */
typedef enum COPY_TV
{
  COPY_TV_AS_IS,
  COPY_TV_TO_UTF8,  /* from the codepage */
  COPY_TV_FROM_UTF8 /* to the codepage */
} Copy_Tv;

/* The slots of one copy that hold copied data already: an open-addressed
   set of addresses. */
typedef struct _copy_slots
{
  const void **keys;
  size_t size; /* 0 or a power of 2 */
  size_t count;
} Copy_Slots;

/* The copy's state. Its Bit_Chain comes first, so the dat that every
   generated function gets is the address of the whole Copy_Chain. */
typedef struct _copy_chain
{
  Bit_Chain dat;
  Dwg_Data *src;
  Dwg_Data *dst;
  Dwg_Copy_Map *map;
  Copy_Slots slots;
  Dwg_Copy_Unresolved *unresolved;
  BITCODE_BL num_unresolved;
  BITCODE_BL max_unresolved;
  Copy_Kind src_kind, dst_kind;
  Copy_Tv tv;
  /* the copy's XRECORD strings converted to other lengths */
  int xdata_resized;
  int error;
} Copy_Chain;

#define CHAIN ((Copy_Chain *)dat)

typedef enum COPY_STRING
{
  COPY_TV, /* 8-bit */
  COPY_TU, /* UTF-16 */
  COPY_T   /* UTF-16 from r2007 DWG sources, else 8-bit */
} Copy_String;

static void copy_ref (Copy_Chain *restrict cdat,
                      const Dwg_Object *restrict holder,
                      BITCODE_H *restrict slot);
static void copy_string (Copy_Chain *restrict cdat, char **restrict slot,
                         const Copy_String kind);
static void copy_bytes (Copy_Chain *restrict cdat,
                        unsigned char **restrict slot, const size_t len);
static void copy_tfv (Copy_Chain *restrict cdat, char **restrict slot,
                      const size_t len);
static int copy_vector (Copy_Chain *restrict cdat, void **restrict slot,
                        const size_t count, const size_t elemsize);
static void copy_cmc (Copy_Chain *restrict cdat, Dwg_Color *restrict color);
static void copy_xdata (Copy_Chain *restrict cdat,
                        const Dwg_Object *restrict holder,
                        Dwg_Resbuf **restrict slot);
static void copy_3dsolid (Copy_Chain *restrict cdat,
                          Dwg_Entity_3DSOLID *restrict _obj);

/*--------------------------------------------------------------------------------
 * MACROS
 */

#define ACTION copy
#define IS_FREE

#undef UNTIL
#undef SINCE
#undef PRE
#undef VERSIONS
#undef VERSION
#define UNTIL(v) if (dat->from_version <= v)
#define SINCE(v) if (dat->from_version >= v)
#define PRE(v) if (dat->from_version < v)
#define VERSIONS(v1, v2)                                                      \
  if (dat->from_version >= v1 && dat->from_version <= v2)
#define VERSION(v) if (dat->from_version == v)

/* Scalars, points and inlined arrays came with the byte copy. */
#define VALUE(value, type, dxf)
#define VALUE_RC(value, dxf) VALUE (value, RC, dxf)
#define VALUE_RS(value, dxf) VALUE (value, RS, dxf)
#define VALUE_RL(value, dxf) VALUE (value, RL, dxf)
#define VALUE_RLx(value, dxf) VALUE (value, RL, dxf)
#define VALUE_RD(value, dxf) VALUE (value, RD, dxf)
#define VALUE_BD(value, dxf) VALUE (value, BD, dxf)

#define FIELD(name, type)                                                     \
  {                                                                           \
  }
#define FIELD_TRACE(name, type)
#define FIELD_G_TRACE(name, type, dxfgroup)
#define FIELD_CAST(name, type, cast, dxf)                                     \
  {                                                                           \
  }
#define SUB_FIELD_CAST(o, name, type, cast, dxf)                              \
  {                                                                           \
  }
#define SUB_FIELD(o, nam, type, dxf) FIELD (_obj->o.nam, type)

#define FIELD_B(name, dxf) FIELD (name, B)
#define FIELD_BB(name, dxf) FIELD (name, BB)
#define FIELD_3B(name, dxf) FIELD (name, 3B)
#define FIELD_BS(name, dxf) FIELD (name, BS)
#define FIELD_BL(name, dxf) FIELD (name, BL)
#define FIELD_BLL(name, dxf) FIELD (name, BLL)
#define FIELD_BD(name, dxf) FIELD (name, BD)
#define FIELD_RC(name, dxf) FIELD (name, RC)
#define FIELD_RS(name, dxf) FIELD (name, RS)
#define FIELD_RD(name, dxf) FIELD (name, RD)
#define FIELD_RL(name, dxf) FIELD (name, RL)
#define FIELD_RLL(name, dxf) FIELD (name, RLL)
#define FIELD_MC(name, dxf) FIELD (name, MC)
#define FIELD_MS(name, dxf) FIELD (name, MS)
#define FIELD_BT(name, dxf) FIELD (name, BT)
#define FIELD_4BITS(name, dxf)                                                \
  {                                                                           \
  }
#define FIELD_BE(name, dxf)                                                   \
  {                                                                           \
  }
#define FIELD_DD(name, _default, dxf)                                         \
  {                                                                           \
  }
#define FIELD_2DD(name, def, dxf)                                             \
  {                                                                           \
  }
#define FIELD_3DD(name, def, dxf)                                             \
  {                                                                           \
  }
#define FIELD_2RD(name, dxf)                                                  \
  {                                                                           \
  }
#define FIELD_2BD(name, dxf)                                                  \
  {                                                                           \
  }
#define FIELD_2BD_1(name, dxf)                                                \
  {                                                                           \
  }
#define FIELD_3RD(name, dxf)                                                  \
  {                                                                           \
  }
#define FIELD_3BD(name, dxf)                                                  \
  {                                                                           \
  }
#define FIELD_3BD_1(name, dxf)                                                \
  {                                                                           \
  }
#define FIELD_3DPOINT(name, dxf)                                              \
  {                                                                           \
  }
#define FIELD_2PT_TRACE(name, type, dxf)                                      \
  {                                                                           \
  }
#define FIELD_3PT_TRACE(name, type, dxf)                                      \
  {                                                                           \
  }
#define FIELD_TIMEBLL(name, dxf)
#define FIELD_TIMERLL(name, dxf)
#define FIELD_NUM_INSERTS(num_inserts, type, dxf)
#define FIELD_VECTOR_INL(nam, type, size, dxf)
#define SUB_FIELD_VECTOR_INL(o, nam, type, size, dxf)
#define FIELD_TFF(name, len, dxf)                                             \
  {                                                                           \
  }
#define FIELD_TFFx(name, len, dxf)                                            \
  {                                                                           \
  }
#define VALUE_TFF(value, dxf)

/* Handle references. obj is the copy holding them, for relative codes. */
#define ANYCODE -1
#define VALUE_HANDLE(ref, nam, code, dxf)                                     \
  {                                                                           \
    copy_ref (CHAIN, obj, &(ref));                                            \
  }
#define FIELD_HANDLE(nam, code, dxf) VALUE_HANDLE (_obj->nam, nam, code, dxf)
#define SUB_FIELD_HANDLE(o, nam, code, dxf)                                   \
  VALUE_HANDLE (_obj->o.nam, nam, code, dxf)
#define FIELD_DATAHANDLE(name, code, dxf) FIELD_HANDLE (name, code, dxf)
#define FIELD_HANDLE_N(name, vcount, code, dxf) FIELD_HANDLE (name, code, dxf)
/* obj->handle, set when the copy is made */
#define VALUE_H(hdl, dxf)

/* Strings and binary chunks */
#define VALUE_TV(value, dxf)                                                  \
  {                                                                           \
    copy_string (CHAIN, (char **)&(value), COPY_TV);                          \
  }
#define FIELD_TV(nam, dxf) VALUE_TV (_obj->nam, dxf)
#define FIELD_TU(nam, dxf)                                                    \
  {                                                                           \
    copy_string (CHAIN, (char **)&_obj->nam, COPY_TU);                        \
  }
#define FIELD_T(nam, dxf)                                                     \
  {                                                                           \
    copy_string (CHAIN, (char **)&_obj->nam, COPY_T);                         \
  }
#define FIELD_T16(nam, dxf) FIELD_T (nam, dxf)
#define FIELD_TU16(nam, dxf) FIELD_TU (nam, dxf)
#define FIELD_T32(nam, dxf) FIELD_T (nam, dxf)
#define FIELD_D2T(nam, dxf) FIELD_TV (nam, dxf)
#define FIELD_TF(nam, len, dxf)                                               \
  {                                                                           \
    copy_bytes (CHAIN, (unsigned char **)&_obj->nam, (size_t)(len));          \
  }
#define FIELD_TFv(nam, len, dxf)                                              \
  {                                                                           \
    copy_tfv (CHAIN, (char **)&_obj->nam, (size_t)(len));                     \
  }
#define FIELD_BINARY(nam, len, dxf) FIELD_TF (nam, len, dxf)
/* Only HANDLE_UNKNOWN_BITS and UNKNOWN_BITS_REST use this, for the raw
   bits, which the copy clears. */
#define VALUE_TF(value, dxf)

#define FIELD_CMC(color, dxf)                                                 \
  {                                                                           \
    copy_cmc (CHAIN, &_obj->color);                                           \
  }
#define SUB_FIELD_CMC(o, color, dxf)                                          \
  {                                                                           \
    copy_cmc (CHAIN, &_obj->o.color);                                         \
  }

/* Arrays. COPY_VECTOR copies the array a field points to, unless the field
   is the array itself (an inlined one: its address is its value), and
   evaluates to 1, for the conditions below. The arrays are copied before
   their elements are walked, so the walk only ever writes into the copy. */
#define COPY_VECTOR(x, count)                                                 \
  (((void *)&(x) == (void *)(x))                                              \
       ? 1                                                                    \
       : copy_vector (CHAIN, (void **)&(x), (size_t)(count), sizeof (*(x))))

#define FIELD_VECTOR_N(nam, type, size, dxf)                                  \
  if (COPY_VECTOR (_obj->nam, size) && (size) > 0 && _obj->nam)               \
    {                                                                         \
      for (vcount = 0; vcount < (BITCODE_BL)(size); vcount++)                 \
        FIELD_##type (nam[vcount], dxf);                                      \
    }
#define FIELD_VECTOR(nam, type, size, dxf)                                    \
  FIELD_VECTOR_N (nam, type, _obj->size, dxf)
#define FIELD_VECTOR_N1(nam, type, size, dxf)                                 \
  FIELD_VECTOR_N (nam, type, size, dxf)
#define FIELD_VECTOR_T(nam, type, size, dxf)                                  \
  FIELD_VECTOR_N (nam, type, _obj->size, dxf)
#define FIELD_2RD_VECTOR(nam, size, dxf)                                      \
  {                                                                           \
    (void)COPY_VECTOR (_obj->nam, _obj->size);                                \
  }
#define FIELD_2DD_VECTOR(nam, size, dxf) FIELD_2RD_VECTOR (nam, size, dxf)
#define FIELD_3DPOINT_VECTOR(nam, size, dxf) FIELD_2RD_VECTOR (nam, size, dxf)
#define SUB_FIELD_2RD_VECTOR(o, nam, size, dxf)                               \
  {                                                                           \
    (void)COPY_VECTOR (_obj->o.nam, _obj->o.size);                            \
  }
#define SUB_FIELD_3BD_VECTOR(o, nam, size, dxf)                               \
  SUB_FIELD_2RD_VECTOR (o, nam, size, dxf)
#define SUB_FIELD_VECTOR_N(o, nam, type, size, dxf)                           \
  if (COPY_VECTOR (_obj->o.nam, size) && (size) > 0 && _obj->o.nam)           \
    {                                                                         \
      for (vcount = 0; vcount < (BITCODE_BL)(size); vcount++)                 \
        SUB_FIELD_##type (o, nam[vcount], dxf);                               \
    }
#define SUB_FIELD_VECTOR(o, nam, type, sizefield, dxf)                        \
  SUB_FIELD_VECTOR_N (o, nam, type, _obj->o.sizefield, dxf)
#define SUB_FIELD_VECTOR_TYPESIZE(o, nam, size, typesize, dxf)                \
  {                                                                           \
    (void)copy_vector (CHAIN, (void **)&_obj->o.nam, (size_t)_obj->o.size,    \
                       (size_t)(typesize));                                   \
  }
#define HANDLE_VECTOR_N(nam, size, code, dxf)                                 \
  if (COPY_VECTOR (_obj->nam, size) && (size) > 0 && _obj->nam)               \
    {                                                                         \
      for (vcount = 0; vcount < (BITCODE_BL)(size); vcount++)                 \
        {                                                                     \
          FIELD_HANDLE_N (nam[vcount], vcount, code, dxf);                    \
        }                                                                     \
    }
#define HANDLE_VECTOR(nam, sizefield, code, dxf)                              \
  HANDLE_VECTOR_N (nam, FIELD_VALUE (sizefield), code, dxf)
#define SUB_HANDLE_VECTOR(o, nam, sizefield, code, dxf)                       \
  if (COPY_VECTOR (_obj->o.nam, _obj->o.sizefield) && _obj->o.sizefield > 0   \
      && _obj->o.nam)                                                         \
    {                                                                         \
      for (vcount = 0; vcount < (BITCODE_BL)_obj->o.sizefield; vcount++)      \
        {                                                                     \
          SUB_FIELD_HANDLE (o, nam[vcount], code, dxf);                       \
        }                                                                     \
    }

/* REPEAT copies the array before the loop over its elements. The variants
   that don't allocate when decoding still walk arrays allocated elsewhere,
   so they copy too. */
#define _REPEAT(times, name, type, idx)                                       \
  if (COPY_VECTOR (_obj->name, _obj->times) && _obj->times > 0                \
      && _obj->name != NULL)                                                  \
    for (rcount##idx = 0; rcount##idx < (BITCODE_BL)_obj->times;              \
         rcount##idx++)
#define _REPEAT_C(times, name, type, idx) _REPEAT (times, name, type, idx)
#define _REPEAT_CN(times, name, type, idx)                                    \
  if (COPY_VECTOR (_obj->name, times) && _obj->name != NULL)                  \
    for (rcount##idx = 0; rcount##idx < (BITCODE_BL)(times); rcount##idx++)
#define _REPEAT_N(times, name, type, idx) _REPEAT_CN (times, name, type, idx)
#define _REPEAT_CNF(times, name, type, idx)                                   \
  _REPEAT_CN (times, name, type, idx)
#define _REPEAT_NF(times, name, type, idx) _REPEAT_CN (times, name, type, idx)
#define _REPEAT_F(times, size, name, type, idx)                               \
  _REPEAT (times, name, type, idx)
#define REPEAT(times, name, type) _REPEAT (times, name, type, 1)
#define REPEAT2(times, name, type) _REPEAT (times, name, type, 2)
#define REPEAT3(times, name, type) _REPEAT (times, name, type, 3)
#define REPEAT4(times, name, type) _REPEAT (times, name, type, 4)
#define REPEAT_C(times, name, type) _REPEAT_C (times, name, type, 1)
#define REPEAT2_C(times, name, type) _REPEAT_C (times, name, type, 2)
#define REPEAT3_C(times, name, type) _REPEAT_C (times, name, type, 3)
#define REPEAT4_C(times, name, type) _REPEAT_C (times, name, type, 4)
#define REPEAT_CN(times, name, type) _REPEAT_CN (times, name, type, 1)
#define REPEAT_N(times, name, type) _REPEAT_N (times, name, type, 1)
#define REPEAT_F(times, size, name, type) _REPEAT_F (times, size, name, type, 1)
#define END_REPEAT(field)
#define END_REPEAT_F(field)

#define FIELD_XDATA(name, size)                                               \
  {                                                                           \
    copy_xdata (CHAIN, obj, &_obj->name);                                     \
  }

#define REACTORS(code)                                                        \
  if (COPY_VECTOR (obj->tio.object->reactors,                                 \
                   obj->tio.object->num_reactors)                             \
      && obj->tio.object->reactors)                                           \
    {                                                                         \
      for (vcount = 0; vcount < obj->tio.object->num_reactors; vcount++)      \
        VALUE_HANDLE (obj->tio.object->reactors[vcount], reactors, code,      \
                      330);                                                   \
    }
#define ENT_REACTORS(code)                                                    \
  if (COPY_VECTOR (_ent->reactors, _ent->num_reactors) && _ent->reactors)     \
    {                                                                         \
      for (vcount = 0; vcount < _ent->num_reactors; vcount++)                 \
        VALUE_HANDLE (_ent->reactors[vcount], reactors, code, 330);           \
    }
/* Copied whether or not is_xdic_missing says it's there: a document built
   with the add API can hold it either way. */
#define XDICOBJHANDLE(code)                                                   \
  VALUE_HANDLE (obj->tio.object->xdicobjhandle, xdicobjhandle, code, 0)
#define ENT_XDICOBJHANDLE(code)                                               \
  VALUE_HANDLE (_ent->xdicobjhandle, xdicobjhandle, code, 0)

#define COMMON_ENTITY_HANDLE_DATA
#define SECTION_STRING_STREAM
#define START_STRING_STREAM
#define END_STRING_STREAM
#define START_HANDLE_STREAM

#include "spec.h"

/* With IS_FREE, dwg_spec_shared.h defines FREE_3DSOLID to call a function of
   free.c's. Including it first, without IS_FREE, keeps it out; dwg.spec's
   include of it is then empty. */
#undef IS_FREE
#include "dwg_spec_shared.h"
#define IS_FREE
#undef FREE_3DSOLID
#define FREE_3DSOLID                                                          \
  {                                                                           \
    copy_3dsolid (CHAIN, (Dwg_Entity_3DSOLID *)_obj);                         \
  }

/* spec.h defines these once, for every action but the decoder. The copy
   sets parent pointers as the decoder does, to the structs of the copy. */
#undef SET_PARENT
#undef SET_PARENT_OBJ
#undef SET_PARENT_FIELD
#define SET_PARENT(field, to) _obj->field.parent = to
#define SET_PARENT_OBJ(field) SET_PARENT (field, _obj)
#define SET_PARENT_FIELD(field, what_parent, to) _obj->field.what_parent = to

/* The decoder bounded these values already. With IS_FREE they return from
   the walk, which would leave the rest of the copy pointing into src. The
   specs follow some with an else. */
#undef VALUEOUTOFBOUNDS
#undef SUB_VALUEOUTOFBOUNDS
#define VALUEOUTOFBOUNDS(field, maxvalue)                                     \
  if (0)                                                                      \
    {                                                                         \
    }
#define SUB_VALUEOUTOFBOUNDS(o, field, maxvalue) VALUEOUTOFBOUNDS (field, 0)

/* RESET_VER writes a global, which the copy doesn't use. */
#undef RESET_VER
#define RESET_VER

static int copy_common_entity (Copy_Chain *restrict cdat,
                               Dwg_Object *restrict obj,
                               const Dwg_Object *restrict srcobj);
static int copy_common_object (Copy_Chain *restrict cdat,
                               Dwg_Object *restrict obj,
                               const Dwg_Object *restrict srcobj);
static int copy_fixup_before (Copy_Chain *restrict cdat,
                              Dwg_Object *restrict obj,
                              const Dwg_Object *restrict srcobj,
                              const Dwg_Object_Type type);
static int copy_fixup_after (Copy_Chain *restrict cdat,
                             Dwg_Object *restrict obj,
                             const Dwg_Object *restrict srcobj,
                             const Dwg_Object_Type type);
static int copy_sweep (Copy_Chain *restrict cdat, const Dwg_Object *obj,
                       const Dwg_DYNAPI_field *fields, void *restrict _obj);

/* Name of a class in the dynapi tables, without the leading _ of _3DFACE. */
#define DYNAPI_NAME(token) ((#token)[0] == '_' ? &(#token)[1] : #token)

/* dwg_copy_TOKEN copies the common data, then the class's struct: a byte
   copy, then fixups that need explicit code, the spec walk, the source's
   values again where the walk derived others, and a sweep of the struct's
   references and strings for any the walk didn't reach. The
   class's struct is allocated first, so that a copy with common data has a
   class struct as its source has, which dwg_free () walks. When out of
   memory, every pointer the walk can't copy is cleared, so that freeing
   dst never frees src's data. */
#define DWG_ENTITY(token)                                                     \
  static int dwg_copy_##token##_private (Bit_Chain *dat, Bit_Chain *hdl_dat,  \
                                         Bit_Chain *str_dat,                  \
                                         Dwg_Object *restrict obj);           \
                                                                              \
  static int dwg_copy_##token (Bit_Chain *restrict dat,                       \
                               Dwg_Object *restrict obj,                      \
                               const Dwg_Object *restrict srcobj)             \
  {                                                                           \
    const Dwg_Entity_##token *src = srcobj->tio.entity->tio.token;            \
    const Dwg_DYNAPI_field *fields                                            \
        = dwg_dynapi_entity_fields (DYNAPI_NAME (token));                     \
    Dwg_Entity_##token *_obj = NULL;                                          \
    int error;                                                                \
    if (src)                                                                  \
      {                                                                       \
        _obj = (Dwg_Entity_##token *)malloc (sizeof (Dwg_Entity_##token));   \
        if (!_obj)                                                            \
          return DWG_ERR_OUTOFMEM;                                            \
      }                                                                       \
    error = copy_common_entity (CHAIN, obj, srcobj);                          \
    if (!_obj)                                                                \
      return error;                                                           \
    if (!obj->tio.entity)                                                     \
      {                                                                       \
        free (_obj);                                                          \
        return error;                                                         \
      }                                                                       \
    memcpy (_obj, src, sizeof (Dwg_Entity_##token));                          \
    _obj->parent = obj->tio.entity;                                           \
    obj->tio.entity->tio.token = _obj;                                        \
    error |= copy_fixup_before (CHAIN, obj, srcobj, DWG_TYPE_##token);        \
    error |= dwg_copy_##token##_private (dat, dat, dat, obj);                 \
    copy_restore_values (fields, _obj, src);                                  \
    error |= copy_fixup_after (CHAIN, obj, srcobj, DWG_TYPE_##token);         \
    error |= copy_sweep (CHAIN, obj, fields, _obj);                           \
    return error;                                                             \
  }                                                                           \
                                                                              \
  static int dwg_copy_##token##_private (Bit_Chain *dat, Bit_Chain *hdl_dat,  \
                                         Bit_Chain *str_dat,                  \
                                         Dwg_Object *restrict obj)            \
  {                                                                           \
    BITCODE_BL vcount, rcount3, rcount4;                                      \
    Dwg_Entity_##token *ent, *_obj;                                           \
    Dwg_Object_Entity *_ent;                                                  \
    Dwg_Data *dwg = obj->parent;                                              \
    int error = 0;                                                            \
    _ent = obj->tio.entity;                                                   \
    if (!_ent)                                                                \
      return 0;                                                               \
    _obj = ent = _ent->tio.token;

#define DWG_ENTITY_END                                                        \
  return error;                                                               \
  }

#define DWG_OBJECT(token)                                                     \
  static int dwg_copy_##token##_private (Bit_Chain *dat, Bit_Chain *hdl_dat,  \
                                         Bit_Chain *str_dat,                  \
                                         Dwg_Object *restrict obj);           \
                                                                              \
  static int dwg_copy_##token (Bit_Chain *restrict dat,                       \
                               Dwg_Object *restrict obj,                      \
                               const Dwg_Object *restrict srcobj)             \
  {                                                                           \
    const Dwg_Object_##token *src = srcobj->tio.object->tio.token;            \
    const Dwg_DYNAPI_field *fields                                            \
        = dwg_dynapi_entity_fields (DYNAPI_NAME (token));                     \
    Dwg_Object_##token *_obj = NULL;                                          \
    int error;                                                                \
    if (src)                                                                  \
      {                                                                       \
        _obj = (Dwg_Object_##token *)malloc (sizeof (Dwg_Object_##token));    \
        if (!_obj)                                                            \
          return DWG_ERR_OUTOFMEM;                                            \
      }                                                                       \
    error = copy_common_object (CHAIN, obj, srcobj);                          \
    if (!_obj)                                                                \
      return error;                                                           \
    if (!obj->tio.object)                                                     \
      {                                                                       \
        free (_obj);                                                          \
        return error;                                                         \
      }                                                                       \
    memcpy (_obj, src, sizeof (Dwg_Object_##token));                          \
    _obj->parent = obj->tio.object;                                           \
    obj->tio.object->tio.token = _obj;                                        \
    error |= copy_fixup_before (CHAIN, obj, srcobj, DWG_TYPE_##token);        \
    error |= dwg_copy_##token##_private (dat, dat, dat, obj);                 \
    copy_restore_values (fields, _obj, src);                                  \
    error |= copy_fixup_after (CHAIN, obj, srcobj, DWG_TYPE_##token);         \
    error |= copy_sweep (CHAIN, obj, fields, _obj);                           \
    return error;                                                             \
  }                                                                           \
                                                                              \
  static int dwg_copy_##token##_private (Bit_Chain *dat, Bit_Chain *hdl_dat,  \
                                         Bit_Chain *str_dat,                  \
                                         Dwg_Object *restrict obj)            \
  {                                                                           \
    BITCODE_BL vcount, rcount3, rcount4;                                      \
    Dwg_Object_##token *_obj;                                                 \
    Dwg_Data *dwg = obj->parent;                                              \
    int error = 0;                                                            \
    if (!obj->tio.object)                                                     \
      return 0;                                                               \
    _obj = obj->tio.object->tio.token;

#define DWG_OBJECT_END                                                        \
  return error;                                                               \
  }

/*--------------------------------------------------------------------------------
 * Slots, strings, arrays and references
 */

static size_t
slot_hash (const void *slot)
{
  uint64_t h = (uint64_t)(uintptr_t)slot;
  h ^= h >> 33;
  h *= UINT64_C (0xff51afd7ed558ccd);
  h ^= h >> 33;
  return (size_t)h;
}

static int
slots_grow (Copy_Slots *restrict s)
{
  const size_t size = s->size ? s->size * 2 : 64;
  const void **keys = (const void **)calloc (size, sizeof (void *));
  size_t i;
  if (!keys)
    return DWG_ERR_OUTOFMEM;
  for (i = 0; i < s->size; i++)
    if (s->keys[i])
      {
        size_t j = slot_hash (s->keys[i]) & (size - 1);
        while (keys[j])
          j = (j + 1) & (size - 1);
        keys[j] = s->keys[i];
      }
  free (s->keys);
  s->keys = keys;
  s->size = size;
  return 0;
}

static int
slot_visited (const Copy_Chain *restrict cdat, const void *slot)
{
  const Copy_Slots *s = &cdat->slots;
  size_t i;
  if (!s->size)
    return 0;
  i = slot_hash (slot) & (s->size - 1);
  while (s->keys[i])
    {
      if (s->keys[i] == slot)
        return 1;
      i = (i + 1) & (s->size - 1);
    }
  return 0;
}

/* Adds slot to the set. Returns 1 if it was not in the set yet, so the
   caller copies what it holds, 0 if it was, and -1 if it was not but the
   set can't grow. */
static int
slot_first_visit (Copy_Chain *restrict cdat, const void *slot)
{
  Copy_Slots *s = &cdat->slots;
  size_t i = 0;
  if (s->size)
    {
      i = slot_hash (slot) & (s->size - 1);
      while (s->keys[i])
        {
          if (s->keys[i] == slot)
            return 0;
          i = (i + 1) & (s->size - 1);
        }
    }
  // the set grows at half full, so a probe always meets an empty key
  if (s->count * 2 >= s->size)
    {
      if (slots_grow (s))
        {
          cdat->error |= DWG_ERR_OUTOFMEM;
          return -1;
        }
      i = slot_hash (slot) & (s->size - 1);
      while (s->keys[i])
        i = (i + 1) & (s->size - 1);
    }
  s->keys[i] = slot;
  s->count++;
  return 1;
}

/* Whether the caller copies what slot holds now: on its first visit. A slot
   the set can't take when out of memory still holds src's pointer, and is
   cleared instead, so that freeing dst doesn't free src's data. */
static int
slot_claim (Copy_Chain *restrict cdat, void **slot)
{
  const int first = slot_first_visit (cdat, slot);
  if (first < 0)
    *slot = NULL;
  return first > 0;
}

/* Empties the set for the next object. A set grown large by one object is
   dropped rather than cleared for each of the following ones. */
static void
slots_clear (Copy_Slots *restrict s)
{
  if (s->size > 4096)
    {
      free (s->keys);
      s->keys = NULL;
      s->size = 0;
    }
  else if (s->count)
    memset (s->keys, 0, s->size * sizeof (void *));
  s->count = 0;
}

/* Whether the len bytes at str are UTF-8 of characters up to U+FFFF, which
   bit_utf8_to_TV () converts in full: sequences of 1 to 3 bytes, none of
   them overlong or a surrogate. \U+XXXX can't hold a character beyond. */
static int
is_utf8_bmp (const unsigned char *restrict str, const size_t len)
{
  size_t i = 0;
  while (i < len)
    {
      const unsigned char c = str[i];
      if (c < 0x80)
        i++;
      else if (c >= 0xC2 && c <= 0xDF)
        {
          if (i + 1 >= len || (str[i + 1] & 0xC0) != 0x80)
            return 0;
          i += 2;
        }
      else if ((c & 0xF0) == 0xE0)
        {
          if (i + 2 >= len || (str[i + 1] & 0xC0) != 0x80
              || (str[i + 2] & 0xC0) != 0x80
              || (c == 0xE0 && str[i + 1] < 0xA0)  // overlong
              || (c == 0xED && str[i + 1] >= 0xA0)) // surrogate
            return 0;
          i += 3;
        }
      else
        return 0;
    }
  return 1;
}

/* A copy of the 8-bit string str, of len bytes, converted as cdat->tv says
   with the given codepage, or NULL when out of memory. Converted to UTF-8,
   \U+XXXX becomes the character; converted to the codepage, a character
   the codepage lacks becomes \U+XXXX, as the encoder converts it. A string
   that is not UTF-8 in a document whose strings are, or that has a
   character beyond U+FFFF, is copied as it is, with
   DWG_ERR_VALUEOUTOFBOUNDS. */
static char *
copy_tv (Copy_Chain *restrict cdat, const char *restrict str,
         const size_t len, const BITCODE_RS codepage)
{
  char *copy;
  if (cdat->tv == COPY_TV_TO_UTF8)
    {
      // str itself when there is nothing to convert, NULL when out of memory
      char *utf8 = bit_TV_to_utf8 (str, codepage);
      if (utf8 != str)
        return utf8;
    }
  else if (cdat->tv == COPY_TV_FROM_UTF8)
    {
      if (is_utf8_bmp ((const unsigned char *)str, len))
        {
          /* A character takes at most 7 bytes as \U+XXXX, for 2 or 3 bytes
             of UTF-8, and \uXXXX takes 7 for its 6. */
          const size_t size = len * 4 + 8;
          copy = (char *)malloc (size);
          if (!copy)
            return NULL;
          if (bit_utf8_to_TV (copy, (const unsigned char *)str, size, len, 0,
                              codepage))
            return copy;
          free (copy);
        }
      LOG_WARN ("copy: string \"%s\" is not UTF-8 of characters up to "
                "U+FFFF, copied as it is",
                str);
      cdat->error |= DWG_ERR_VALUEOUTOFBOUNDS;
    }
  copy = (char *)malloc (len + 1);
  if (copy)
    {
      memcpy (copy, str, len);
      copy[len] = '\0';
    }
  return copy;
}

/* copy_tv () for a string of size bytes that may hold NULs, as an XRECORD
   string read from a file may: each part between them is converted, and
   the NULs kept. The copy, with a NUL after it, is *sizep bytes long; NULL
   when out of memory. */
static char *
copy_tv_parts (Copy_Chain *restrict cdat, const char *restrict str,
               const size_t size, const BITCODE_RS codepage,
               size_t *restrict sizep)
{
  // ends in a NUL, as copy_tv () reads a part up to one
  char *parts = (char *)malloc (size + 1);
  char *copy = NULL;
  size_t pos = 0, copy_size = 0;
  if (!parts)
    return NULL;
  memcpy (parts, str, size);
  parts[size] = '\0';
  for (;;)
    {
      const size_t len = strlen (parts + pos);
      char *part = copy_tv (cdat, parts + pos, len, codepage);
      size_t part_size;
      char *grown;
      if (!part)
        break;
      part_size = strlen (part);
      grown = (char *)realloc (copy, copy_size + part_size + 1);
      if (!grown)
        {
          free (part);
          break;
        }
      copy = grown;
      // with the NUL after it, kept when another part follows
      memcpy (copy + copy_size, part, part_size + 1);
      copy_size += part_size;
      free (part);
      pos += len;
      if (pos >= size)
        {
          free (parts);
          *sizep = copy_size;
          return copy;
        }
      copy_size++;
      pos++;
    }
  free (parts);
  free (copy);
  return NULL;
}

static void
copy_string (Copy_Chain *restrict cdat, char **restrict slot,
             const Copy_String kind)
{
  const char *str = *slot;
  size_t size;
  char *copy;
  const Bit_Chain *dat = &cdat->dat;
  if (!str || !slot_claim (cdat, (void **)slot))
    return;
  if (kind == COPY_TU || (kind == COPY_T && IS_FROM_TU (dat)))
    {
      size = (bit_wcs2len ((BITCODE_TU)str) + 1) * 2;
      copy = (char *)malloc (size);
      if (copy)
        memcpy (copy, str, size);
    }
  else
    copy = copy_tv (cdat, str, strlen (str), dat->codepage);
  if (!copy)
    cdat->error |= DWG_ERR_OUTOFMEM;
  *slot = copy;
}

/* A binary chunk of len bytes, with a NUL after it as the decoder adds. */
static void
copy_bytes (Copy_Chain *restrict cdat, unsigned char **restrict slot,
            const size_t len)
{
  const unsigned char *bytes = *slot;
  unsigned char *copy;
  if (!bytes || !slot_claim (cdat, (void **)slot))
    return;
  copy = (unsigned char *)malloc (len + 1);
  if (copy)
    {
      memcpy (copy, bytes, len);
      copy[len] = '\0';
    }
  else
    cdat->error |= DWG_ERR_OUTOFMEM;
  *slot = copy;
}

/* A string the encoder writes as len bytes: the copy has room for them
   even when the string is shorter. */
static void
copy_tfv (Copy_Chain *restrict cdat, char **restrict slot, const size_t len)
{
  const char *str = *slot;
  size_t n;
  char *copy;
  if (!str || !slot_claim (cdat, (void **)slot))
    return;
  n = strlen (str);
  copy = (char *)calloc ((n > len ? n : len) + 1, 1);
  if (copy)
    memcpy (copy, str, n);
  else
    cdat->error |= DWG_ERR_OUTOFMEM;
  *slot = copy;
}

/* Always returns 1, for the macros' conditions. */
static int
copy_vector (Copy_Chain *restrict cdat, void **restrict slot,
             const size_t count, const size_t elemsize)
{
  const void *array = *slot;
  void *copy = NULL;
  if (!array || !slot_claim (cdat, slot))
    return 1;
  if (count && elemsize && count <= SIZE_MAX / elemsize)
    {
      copy = malloc (count * elemsize);
      if (copy)
        memcpy (copy, array, count * elemsize);
      else
        cdat->error |= DWG_ERR_OUTOFMEM;
    }
  else if (count)
    {
      LOG_ERROR ("copy: array of %" PRIuSIZE " elements too large", count);
      cdat->error |= DWG_ERR_VALUEOUTOFBOUNDS;
    }
  /* An array with no elements has nothing to copy: the copy holds NULL. */
  *slot = copy;
  return 1;
}

static void
copy_cmc (Copy_Chain *restrict cdat, Dwg_Color *restrict color)
{
  copy_string (cdat, (char **)&color->name, COPY_T);
  copy_string (cdat, (char **)&color->book_name, COPY_T);
}

static int
add_unresolved (Copy_Chain *restrict cdat, const Dwg_Object *restrict holder,
                const Dwg_Copy_Slot kind, void *slot, const BITCODE_RLL target,
                const BITCODE_RC code)
{
  Dwg_Copy_Unresolved *u;
  if (cdat->num_unresolved == cdat->max_unresolved)
    {
      const BITCODE_BL max
          = cdat->max_unresolved ? cdat->max_unresolved * 2 : 16;
      Dwg_Copy_Unresolved *list = (Dwg_Copy_Unresolved *)realloc (
          cdat->unresolved, max * sizeof (Dwg_Copy_Unresolved));
      if (!list)
        {
          cdat->error |= DWG_ERR_OUTOFMEM;
          return DWG_ERR_OUTOFMEM;
        }
      cdat->unresolved = list;
      cdat->max_unresolved = max;
    }
  u = &cdat->unresolved[cdat->num_unresolved++];
  u->holder = holder->handle.value;
  u->target = target;
  u->code = code;
  u->kind = kind;
  u->slot = slot;
  return 0;
}

/* The target of a source handle in dst, or 0 when it's outside the copy:
   0 stays 0, mapped handles are mapped, and with src == dst the rest keep
   their target. */
static BITCODE_RLL
copy_target (const Copy_Chain *restrict cdat, const BITCODE_RLL target,
             int *restrict unresolved)
{
  BITCODE_RLL mapped;
  *unresolved = 0;
  if (!target)
    return 0;
  mapped = dwg_copy_map_get (cdat->map, target);
  if (mapped)
    return mapped;
  if (cdat->src == cdat->dst)
    return target;
  *unresolved = 1;
  return 0;
}

/* A reference in dst with the given code to target, held by holder. A
   relative code (6, 8, 0xA, 0xC) is recomputed from the holder's handle; a
   holder can't refer to itself relatively, so that becomes a soft pointer.
   NULL handles are absolute too. */
static BITCODE_H
copy_new_ref (Dwg_Data *restrict dst, const BITCODE_RC code,
              const BITCODE_RLL target, const Dwg_Object *restrict holder)
{
  if (code > 5)
    {
      if (!target || target == holder->handle.value)
        return dwg_add_handleref (dst, DWG_HDL_SOFTPTR, target, NULL);
      return dwg_add_handleref (dst, code, target, holder);
    }
  return dwg_add_handleref (dst, code, target, NULL);
}

static void
copy_ref (Copy_Chain *restrict cdat, const Dwg_Object *restrict holder,
          BITCODE_H *restrict slot)
{
  const Dwg_Object_Ref *ref = *slot;
  BITCODE_RLL target;
  BITCODE_RC code;
  int unresolved;
  if (!ref || !slot_claim (cdat, (void **)slot))
    return;
  code = ref->handleref.code;
  /* 0xE points at its holder, whatever its value. */
  if (code == 0xE)
    {
      BITCODE_H copy = dwg_new_ref (cdat->dst);
      if (copy)
        {
          copy->handleref = ref->handleref;
          copy->handleref.is_global = 1;
          copy->absolute_ref = holder->handle.value;
        }
      else
        cdat->error |= DWG_ERR_OUTOFMEM;
      *slot = copy;
      return;
    }
  /* Code 0 marks a handle value rather than a reference, as SORTENTSTABLE's
     sort keys are, which may be the handles of erased entities: one that
     names a copied object takes the copy's handle, the others keep their
     value, and are reported between documents. */
  if (code == 0)
    {
      target = dwg_copy_map_get (cdat->map, ref->absolute_ref);
      if (!target)
        {
          target = ref->absolute_ref;
          if (target && cdat->src != cdat->dst)
            add_unresolved (cdat, holder, DWG_COPY_SLOT_REF, slot, target,
                            code);
        }
    }
  else
    {
      target = copy_target (cdat, ref->absolute_ref, &unresolved);
      if (unresolved)
        add_unresolved (cdat, holder, DWG_COPY_SLOT_REF, slot,
                        ref->absolute_ref, code);
    }
  *slot = copy_new_ref (cdat->dst, code, target, holder);
  if (!*slot)
    cdat->error |= DWG_ERR_OUTOFMEM;
}

/* A handle value in EED or XRECORD data. EED data is packed, which leaves
   its handles unaligned, so slot is only ever accessed with memcpy. */
static void
copy_handle_value (Copy_Chain *restrict cdat,
                   const Dwg_Object *restrict holder, void *restrict slot)
{
  int unresolved;
  BITCODE_RLL target, mapped;
  memcpy (&target, slot, sizeof (target));
  mapped = copy_target (cdat, target, &unresolved);
  memcpy (slot, &mapped, sizeof (mapped));
  if (unresolved)
    add_unresolved (cdat, holder, DWG_COPY_SLOT_HANDLE, slot, target, 0);
}

/*--------------------------------------------------------------------------------
 * EED and XRECORD data
 */

/* The length of an 8-bit EED string as the encoder writes it: a string that
   starts with NUL as the empty string. The decoder leaves a string that
   claims more bytes than are left in its group unread, with that length and
   zero bytes, in an item sized by what is left of its group (for a group's
   later items, one byte more), so its length can't be trusted. */
static BITCODE_RS
eed_string_length (const Dwg_Eed_Data *restrict data)
{
  return data->u.eed_0.string[0] ? data->u.eed_0.length : 0;
}

/* The bytes of an EED item the copy keeps, which may be fewer than
   sizeof (Dwg_Eed_Data): the decoder sizes each item by what is left of its
   group. Strings and binary chunks are inline; an 8-bit string keeps the
   length eed_string_length () gives it. */
static size_t
eed_data_size (const Dwg_Eed_Data *restrict data)
{
  switch (data->code)
    {
    case 0:
      if (data->u.eed_0.is_tu)
        return offsetof (Dwg_Eed_Data, u.eed_0_r2007.string)
               + ((size_t)data->u.eed_0_r2007.length + 1) * 2;
      return offsetof (Dwg_Eed_Data, u.eed_0.string)
             + (size_t)eed_string_length (data) + 1;
    case 1:
      return offsetof (Dwg_Eed_Data, u) + sizeof (data->u.eed_1);
    case 2:
      return offsetof (Dwg_Eed_Data, u) + sizeof (data->u.eed_2);
    case 3:
      return offsetof (Dwg_Eed_Data, u) + sizeof (data->u.eed_3);
    case 4:
      return offsetof (Dwg_Eed_Data, u.eed_4.data)
             + (size_t)data->u.eed_4.length;
    case 5:
      return offsetof (Dwg_Eed_Data, u) + sizeof (data->u.eed_5);
    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
      return offsetof (Dwg_Eed_Data, u) + sizeof (data->u.eed_10);
    case 40:
    case 41:
    case 42:
      return offsetof (Dwg_Eed_Data, u) + sizeof (data->u.eed_40);
    case 70:
      return offsetof (Dwg_Eed_Data, u) + sizeof (data->u.eed_70);
    case 71:
      return offsetof (Dwg_Eed_Data, u) + sizeof (data->u.eed_71);
    default:
      return offsetof (Dwg_Eed_Data, u);
    }
}

/* The bytes an EED item takes in an r13 to r2004 DWG file, its code
   included, or 0 for a code the decoder doesn't read from one, or a UTF-16
   string, which it reads from r2007 on only. */
static size_t
eed_item_file_size (const Dwg_Eed_Data *restrict data)
{
  switch (data->code)
    {
    case 0: // length [RC], codepage [RS], then the string
      return data->u.eed_0.is_tu ? 0 : 4 + (size_t)data->u.eed_0.length;
    case 1:
    case 70:
      return 3;
    case 2:
      return 2;
    case 3:
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

/* Whether the items of the EED group that starts at eed[first] hold all of
   it: every one of them decoded, their sizes adding up to the group's. They
   don't when the decoder failed on a later item, which it leaves without
   data, or left a string unread, after which it skips to the group's end:
   the items after it are then only in the group's raw bytes. */
static int
eed_group_decoded (const Dwg_Eed *restrict eed, const BITCODE_BL num_eed,
                   const BITCODE_BL first)
{
  size_t total = 0;
  BITCODE_BL i;
  for (i = first; i < num_eed && (i == first || !eed[i].size); i++)
    {
      const size_t size = eed[i].data ? eed_item_file_size (eed[i].data) : 0;
      if (!size)
        return 0;
      total += size;
    }
  return total == eed[first].size;
}

/* Copies the EED, without its raw bytes, so the encoder writes it from the
   items. The APPID handles and the handles in it (1003 layer, 1005) are
   remapped. A group read from a file, which has raw bytes, is left out
   when its items don't hold all of those bytes: the rest is only in the
   raw bytes, with handles that can't be remapped. A group whose first item
   has no data is left out too. */
static int
copy_eed (Copy_Chain *restrict cdat, const Dwg_Object *restrict holder,
          Dwg_Eed **restrict eedp, BITCODE_BL *restrict num_eedp)
{
  const Dwg_Eed *src = *eedp;
  const BITCODE_BL num_eed = *num_eedp;
  Dwg_Eed *eed;
  BITCODE_BL i, j = 0;
  int skip = 0;
  int error = 0;

  *eedp = NULL;
  *num_eedp = 0;
  if (!src || !num_eed)
    return 0;
  eed = (Dwg_Eed *)calloc (num_eed, sizeof (Dwg_Eed));
  if (!eed)
    return DWG_ERR_OUTOFMEM;
  for (i = 0; i < num_eed; i++)
    {
      const Dwg_Eed *e = &src[i];
      Dwg_Eed *copy;
      int unresolved;
      size_t size;
      if (e->size)
        {
          skip = !e->data || (e->raw && !eed_group_decoded (src, num_eed, i));
          if (skip)
            {
              LOG_WARN ("copy: %s " FORMAT_HV " EED[%u] group of %u bytes "
                        "dropped, its items could not all be decoded",
                        holder->name, holder->handle.value, (unsigned)i,
                        (unsigned)e->size);
              error |= DWG_ERR_INVALIDEED;
            }
        }
      if (skip || !e->data)
        continue;
      copy = &eed[j];
      size = eed_data_size (e->data);
      copy->data = (Dwg_Eed_Data *)calloc (
          size > sizeof (Dwg_Eed_Data) ? size : sizeof (Dwg_Eed_Data), 1);
      if (!copy->data)
        {
          error |= DWG_ERR_OUTOFMEM;
          break;
        }
      memcpy (copy->data, e->data, size);
      if (copy->data->code == 0 && !copy->data->u.eed_0.is_tu)
        copy->data->u.eed_0.length = eed_string_length (e->data);
      copy->size = e->size;
      copy->handle = e->handle;
      copy->raw = NULL;
      j++;

      copy->handle.value
          = copy_target (cdat, e->handle.value, &unresolved);
      dwg_set_handle_size (&copy->handle);
      if (unresolved)
        add_unresolved (cdat, holder, DWG_COPY_SLOT_EED_APPID, &copy->handle,
                        e->handle.value, e->handle.code);
      if (copy->data->code == 5)
        copy_handle_value (cdat, holder, &copy->data->u.eed_5.entity);
      else if (copy->data->code == 3)
        copy_handle_value (cdat, holder, &copy->data->u.eed_3.layer);
    }
  if (j)
    {
      *eedp = eed;
      *num_eedp = j;
    }
  else
    free (eed);
  return error;
}

/* The bytes XRECORD data takes in an r13 to r2004 DWG file, as the decoder
   reads it: each item's type [RS], then its value. The encoder writes
   num_xdata items, and the JSON importer leaves an empty one after
   them. */
static BITCODE_BL
copy_xdata_size (const Dwg_Resbuf *restrict rbuf, const BITCODE_BL num_xdata)
{
  size_t size = 0;
  for (BITCODE_BL i = 0; rbuf && i < num_xdata; rbuf = rbuf->nextrb, i++)
    {
      size += 2;
      switch (dwg_resbuf_value_type (rbuf->type))
        {
        case DWG_VT_STRING: // length [RS], codepage [RC]
          size += 3 + (size_t)rbuf->value.str.size;
          break;
        case DWG_VT_BINARY: // length [RC]
          size += 1 + (size_t)rbuf->value.str.size;
          break;
        case DWG_VT_BOOL:
        case DWG_VT_INT8:
          size += 1;
          break;
        case DWG_VT_INT16:
          size += 2;
          break;
        case DWG_VT_INT32:
          size += 4;
          break;
        case DWG_VT_REAL:
        case DWG_VT_INT64:
        case DWG_VT_HANDLE:
        case DWG_VT_OBJECTID:
          size += 8;
          break;
        case DWG_VT_POINT3D:
          size += 24;
          break;
        default:
          break;
        }
    }
  return (BITCODE_BL)size;
}

/* Copies an XRECORD's data, remapping its object handles: 330-369 and
   390-399. 320-329 are arbitrary handles, which AutoCAD doesn't translate
   when it copies objects either. Its strings are converted as the others
   are. */
static void
copy_xdata (Copy_Chain *restrict cdat, const Dwg_Object *restrict holder,
            Dwg_Resbuf **restrict slot)
{
  const Dwg_Resbuf *rbuf = *slot;
  Dwg_Resbuf *root = NULL, *last = NULL;
  if (!rbuf || !slot_claim (cdat, (void **)slot))
    return;
  for (; rbuf; rbuf = rbuf->nextrb)
    {
      Dwg_Resbuf *copy = (Dwg_Resbuf *)malloc (sizeof (Dwg_Resbuf));
      if (!copy)
        {
          cdat->error |= DWG_ERR_OUTOFMEM;
          break;
        }
      memcpy (copy, rbuf, sizeof (Dwg_Resbuf));
      copy->nextrb = NULL;
      if (last)
        last->nextrb = copy;
      else
        root = copy;
      last = copy;
      switch (dwg_resbuf_value_type (rbuf->type))
        {
        case DWG_VT_STRING:
          // with its own codepage, as the encoder converts it
          if (rbuf->value.str.u.data && !rbuf->value.str.is_tu
              && cdat->tv != COPY_TV_AS_IS)
            {
              size_t size = 0;
              char *conv = copy_tv_parts (
                  cdat, rbuf->value.str.u.data, rbuf->value.str.size,
                  rbuf->value.str.codepage, &size);
              copy->value.str.u.data = conv;
              if (!conv)
                {
                  copy->value.str.size = 0;
                  cdat->error |= DWG_ERR_OUTOFMEM;
                }
              /* dst's encoder writes a string in the codepage as it is, and
                 dwg_decode_xdata () reads a length above 0x7FFF as negative
                 and skips the string. A string in UTF-8 is converted again
                 when it is written, and truncated to 0x7FFF bytes then. */
              else if (size > (cdat->tv == COPY_TV_FROM_UTF8 ? 0x7FFF
                                                              : 0xFFFF))
                {
                  LOG_ERROR ("copy: %s " FORMAT_HV " XRECORD string of %u "
                             "bytes too long once converted",
                             holder->name, holder->handle.value,
                             (unsigned)rbuf->value.str.size);
                  free (conv);
                  copy->value.str.u.data = NULL;
                  copy->value.str.size = 0;
                  cdat->error |= DWG_ERR_VALUEOUTOFBOUNDS;
                }
              else
                copy->value.str.size = (BITCODE_RS)size;
              if (copy->value.str.size != rbuf->value.str.size)
                cdat->xdata_resized = 1;
              break;
            }
          // fall through
        case DWG_VT_BINARY:
          if (rbuf->value.str.u.data)
            {
              const size_t size = rbuf->value.str.is_tu
                                      ? ((size_t)rbuf->value.str.size + 1) * 2
                                      : (size_t)rbuf->value.str.size + 1;
              copy->value.str.u.data = (char *)calloc (size, 1);
              if (copy->value.str.u.data)
                memcpy (copy->value.str.u.data, rbuf->value.str.u.data,
                        rbuf->value.str.is_tu
                            ? (size_t)rbuf->value.str.size * 2
                            : (size_t)rbuf->value.str.size);
              else
                cdat->error |= DWG_ERR_OUTOFMEM;
            }
          break;
        case DWG_VT_OBJECTID:
        case DWG_VT_HANDLE:
          if (rbuf->type >= 330 && rbuf->type <= 399)
            copy_handle_value (cdat, holder, &copy->value.absref);
          break;
        default:
          break;
        }
    }
  *slot = root;
}

/* The ACIS data of 3DSOLID and the classes that share its layout. Version 1
   (SAT) has num_blocks encrypted blocks and an empty one after them, and
   acis_data holds their decrypted text; version 2 (SAB) holds sab_size bytes
   in acis_data. */
static void
copy_3dsolid (Copy_Chain *restrict cdat, Dwg_Entity_3DSOLID *restrict _obj)
{
  const BITCODE_BL num_blocks = _obj->num_blocks + 1;
  size_t acis_size = 0;
  BITCODE_BL i;

  if (_obj->block_size)
    (void)copy_vector (cdat, (void **)&_obj->block_size,
                       _obj->version == 2 ? 2 : num_blocks,
                       sizeof (BITCODE_BL));
  if (_obj->encr_sat_data && _obj->block_size)
    {
      (void)copy_vector (cdat, (void **)&_obj->encr_sat_data, num_blocks,
                         sizeof (char *));
      for (i = 0; _obj->encr_sat_data && i < num_blocks; i++)
        copy_bytes (cdat, (unsigned char **)&_obj->encr_sat_data[i],
                    _obj->block_size[i]);
    }
  else if (_obj->encr_sat_data
           && slot_claim (cdat, (void **)&_obj->encr_sat_data))
    {
      LOG_ERROR ("copy: 3DSOLID encr_sat_data without block_size not copied");
      _obj->encr_sat_data = NULL;
      cdat->error |= DWG_ERR_VALUEOUTOFBOUNDS;
    }
  if (_obj->version == 2)
    acis_size = _obj->sab_size;
  else if (_obj->block_size)
    for (i = 0; i < _obj->num_blocks; i++)
      acis_size += _obj->block_size[i];
  if (!acis_size && _obj->acis_data)
    acis_size = strlen ((char *)_obj->acis_data);
  copy_bytes (cdat, &_obj->acis_data, acis_size);
}

/*--------------------------------------------------------------------------------
 * Common data
 */

static int
copy_common_entity (Copy_Chain *restrict cdat, Dwg_Object *restrict obj,
                    const Dwg_Object *restrict srcobj)
{
  Bit_Chain *dat = &cdat->dat;
  Bit_Chain *hdl_dat = dat;
  Bit_Chain *str_dat = dat;
  Dwg_Data *dwg = obj->parent;
  Dwg_Object_Entity *_obj, *_ent;
  BITCODE_BL vcount;
  int error = 0;

  _ent = (Dwg_Object_Entity *)malloc (sizeof (Dwg_Object_Entity));
  if (!_ent)
    return DWG_ERR_OUTOFMEM;
  memcpy (_ent, srcobj->tio.entity, sizeof (Dwg_Object_Entity));
  _ent->tio.UNUSED = NULL;
  _ent->objid = obj->index;
  _ent->dwg = dwg;
  obj->tio.entity = _ent;
  _obj = _ent;
  (void)hdl_dat;
  (void)str_dat;

  error |= copy_eed (cdat, obj, &_ent->eed, &_ent->num_eed);

  // clang-format off
  #include "common_entity_data.spec"
  SINCE (R_13b1) {
    #include "common_entity_handle_data.spec"
  }
  // clang-format on

  /* The spec reads the owner only for entmode 0, and other handles only
     when their flags say they're there. */
  error |= copy_sweep (cdat, obj, dwg_dynapi_common_entity_fields (), _ent);
  copy_ref (cdat, obj, &_ent->color.handle);
  return error;
}

static int
copy_common_object (Copy_Chain *restrict cdat, Dwg_Object *restrict obj,
                    const Dwg_Object *restrict srcobj)
{
  Bit_Chain *dat = &cdat->dat;
  Bit_Chain *hdl_dat = dat;
  Dwg_Data *dwg = obj->parent;
  Dwg_Object_Object *_obj;
  BITCODE_BL vcount;
  int error = 0;

  _obj = (Dwg_Object_Object *)malloc (sizeof (Dwg_Object_Object));
  if (!_obj)
    return DWG_ERR_OUTOFMEM;
  memcpy (_obj, srcobj->tio.object, sizeof (Dwg_Object_Object));
  _obj->tio.APPID = NULL;
  _obj->objid = obj->index;
  _obj->dwg = dwg;
  /* not used by the library; it points into src when set */
  _obj->handleref = NULL;
  obj->tio.object = _obj;
  (void)hdl_dat;

  error |= copy_eed (cdat, obj, &_obj->eed, &_obj->num_eed);

  // clang-format off
  #include "common_object_handle_data.spec"
  // clang-format on

  error |= copy_sweep (cdat, obj, dwg_dynapi_common_object_fields (), _obj);
  return error;
}

/* Copies, or clears, the references, strings, arrays and binary chunks in a
   struct's fields that the spec walk didn't reach, as listed by dynapi:
   references and strings are copied; an array or binary chunk of unknown
   length can't be, and is cleared, as a NULL is safer than data shared
   with src. A source imported from JSON can set fields that the specs
   don't walk, as EXTRUDEDSURFACE's bindata. */
static int
copy_sweep (Copy_Chain *restrict cdat, const Dwg_Object *obj,
            const Dwg_DYNAPI_field *fields, void *restrict _obj)
{
  const Dwg_DYNAPI_field *f;
  int error = 0;
  if (!fields)
    return 0;
  for (f = fields; f->name; f++)
    {
      void *slot = (char *)_obj + f->offset;
      const char *type = f->type;
      const size_t len = strlen (type);
      if (strEQc (type, "H"))
        copy_ref (cdat, obj, (BITCODE_H *)slot);
      else if (strEQc (type, "TV") || strEQc (type, "D2T"))
        copy_string (cdat, (char **)slot, COPY_TV);
      else if (strEQc (type, "TU") || strEQc (type, "TU16"))
        copy_string (cdat, (char **)slot, COPY_TU);
      else if (strEQc (type, "T") || strEQc (type, "T16")
               || strEQc (type, "T32"))
        copy_string (cdat, (char **)slot, COPY_T);
      /* The other pointers to data the struct owns: arrays, and binary
         chunks (TF), whose length only the spec walk knows. Pointers to
         other structs (parent, dwg) don't own what they point to. */
      else if (f->is_malloc
               && (strEQc (type, "TF")
                   || (len && type[len - 1] == '*'
                       && !memBEGINc (type, "struct ")
                       && strNE (type, "Dwg_Eed*")))
               && *(void **)slot && !slot_visited (cdat, slot))
        {
          LOG_ERROR ("copy: %s.%s [%s] not copied", obj->name, f->name,
                     type);
          *(void **)slot = NULL;
          error |= DWG_ERR_VALUEOUTOFBOUNDS;
        }
    }
  return error;
}

/* Whether a field of this dynapi type is a plain value, which the byte copy
   copied whole: a number, a point or a date, but not a colour, which holds
   strings and a reference, nor a struct. */
static int
copy_is_value_type (const char *type)
{
  static const char *const types[]
      = { "B",   "BB",   "3B",      "4BITS",   "BS",      "BSd",     "BSx",
          "BL",  "BLd",  "BLx",     "BLL",     "BD",      "BE",      "BT",
          "RC",  "RCd",  "RS",      "RSd",     "RL",      "RLd",     "RLx",
          "RLL", "RD",   "MC",      "MS",      "2RD",     "2BD",     "2BD_1",
          "2DD", "3RD",  "3BD",     "3BD_1",   "3DD",     "2DPOINT", "3DPOINT",
          "int", "TIMEBLL", "TIMERLL", "int32_t", "unsigned long", NULL };
  const char *const *t;
  for (t = types; *t; t++)
    if (strEQ (type, *t))
      return 1;
  return 0;
}

/* An unsigned count of 1 to 8 bytes. */
static uint64_t
copy_count (const void *p, const size_t size)
{
  switch (size)
    {
    case 1:
      return *(const uint8_t *)p;
    case 2:
      return *(const uint16_t *)p;
    case 4:
      return *(const uint32_t *)p;
    case 8:
      return *(const uint64_t *)p;
    default:
      return UINT64_MAX;
    }
}

/* The walk runs the specs' statements that set fields from other fields, as
   the decoder needs them: LAYER's frozen, off, locked, plotflag and linewt
   from flag0, a table record's DXF flag from its bits, PROXY_ENTITY's
   versions. For a source read from a DWG file they change nothing, but a
   source built with the add API or imported from DXF or JSON can hold
   values they don't derive. So the copy's value fields take their
   source's values again. A count (num_*, *_size) does so only when it is
   no more than the walk's, which may have copied that many elements
   (TABLESTYLE's three row styles, IMAGE's two clip vertices of a
   rectangle). */
static void
copy_restore_values (const Dwg_DYNAPI_field *fields, void *restrict _obj,
                     const void *restrict src)
{
  const Dwg_DYNAPI_field *f;
  if (!fields)
    return;
  for (f = fields; f->name; f++)
    {
      const size_t len = strlen (f->name);
      char *slot = (char *)_obj + f->offset;
      const char *from = (const char *)src + f->offset;
      if (f->is_malloc || !copy_is_value_type (f->type))
        continue;
      if ((memBEGINc (f->name, "num_")
           || (len > 5 && strEQc (&f->name[len - 5], "_size")))
          && copy_count (from, f->size) > copy_count (slot, f->size))
        continue;
      memcpy (slot, from, f->size);
    }
}

/* From r2000 a LAYER holds its flags three ways: flag0, the bitmask a DWG
   file stores; the fields frozen, off, frozen_in_new, locked, plotflag and
   linewt; and flag, DXF group 70, with the sign of the colour index for
   off. The encoder writes flag0, but takes it from a different one for
   each kind of document: as it is from a document imported from JSON;
   from flag and the colour of one imported from DXF, plotted; and from the
   fields of any other. This is the flag0 a layer of src stands for: what
   the encoder writes of it. */
static BITCODE_BS
copy_layer_flag0 (const Dwg_Object_LAYER *restrict src, const Copy_Kind kind)
{
  int frozen, off, frozen_in_new, locked, plotflag;
  if (kind == COPY_KIND_JSON)
    return src->flag0;
  if (kind == COPY_KIND_DXF)
    {
      frozen = src->flag & 1;
      frozen_in_new = src->flag & 2;
      locked = src->flag & 4;
      off = src->color.index < 0;
      plotflag = 1;
    }
  else
    {
      frozen = src->frozen;
      frozen_in_new = src->frozen_in_new;
      locked = src->locked;
      off = src->off;
      plotflag = src->plotflag;
    }
  return (BITCODE_BS)((frozen ? 1 : 0) | (off ? 2 : 0)
                      | (frozen_in_new ? 4 : 0) | (locked ? 8 : 0)
                      | (plotflag ? 16 : 0) | ((src->linewt & 0x1F) << 5));
}

/* A copy of a LAYER into a document of another kind holds its flags the
   three ways, as copy_layer_flag0 () reads them from its source, so that
   dst's encoder writes them whichever way it takes them. */
static void
copy_layer_flags (const Copy_Chain *restrict cdat,
                  Dwg_Object_LAYER *restrict _obj,
                  const Dwg_Object_LAYER *restrict src)
{
  BITCODE_BS flag0;
  if (cdat->src_kind == cdat->dst_kind || cdat->dat.from_version < R_2000b)
    return;
  flag0 = copy_layer_flag0 (src, cdat->src_kind);
  _obj->flag0 = flag0;
  _obj->frozen = flag0 & 1;
  _obj->off = (flag0 & 2) ? 1 : 0;
  _obj->frozen_in_new = (flag0 & 4) ? 1 : 0;
  _obj->locked = (flag0 & 8) ? 1 : 0;
  _obj->plotflag = (flag0 & 16) ? 1 : 0;
  _obj->linewt = (flag0 & 0x03E0) >> 5;
  _obj->flag = (BITCODE_RC)((_obj->flag & ~7) | _obj->frozen
                            | (_obj->frozen_in_new << 1)
                            | (_obj->locked << 2));
  if (cdat->dst_kind == COPY_KIND_DXF)
    _obj->color.index = _obj->off ? -abs (_obj->color.index)
                                  : abs (_obj->color.index);
}

/* Whether the encoder works out the dataflags of dwg's TEXTs, ATTRIBs and
   ATTDEFs from their values (dwg.spec): for a DXF import, a document built
   with the add API, and r2000 and older. It writes those of the others as
   they are, and leaves out the values they flag as defaults. */
static int
copy_dataflags_from_values (const Dwg_Data *dwg)
{
  return (dwg->opts & DWG_OPTS_INDXF) || dwg->header.from_version <= R_2000;
}

/* A TEXT, ATTRIB or ATTDEF copied into a document whose encoder takes its
   dataflags otherwise than its source's. From a source whose encoder works
   them out, the copy gets them from its values: the DXF importer sets a
   flag for a value that is there, where a DWG file sets it for one left
   out. From the others, the values its flags leave out get their
   defaults, as the decoder reads them: a JSON import holds a width factor
   of 0 where its flag stands for 1. Only the DWG encoder, which comes with
   write support, reads the flags. */
#ifdef USE_WRITE
#  define COPY_SET_DATAFLAGS dwg_set_dataflags (obj)
#else
#  define COPY_SET_DATAFLAGS ((void)0)
#endif
#define COPY_TEXT_DATAFLAGS(token)                                            \
  {                                                                           \
    Dwg_Entity_##token *_obj = obj->tio.entity->tio.token;                    \
    const int src_values = copy_dataflags_from_values (cdat->src);            \
    const int dst_values = copy_dataflags_from_values (cdat->dst);            \
    if (src_values && !dst_values)                                            \
      COPY_SET_DATAFLAGS;                                                     \
    else if (!src_values && dst_values)                                       \
      {                                                                       \
        if (_obj->dataflags & 0x01)                                           \
          _obj->elevation = 0.0;                                              \
        if (_obj->dataflags & 0x02)                                           \
          _obj->alignment_pt.x = _obj->alignment_pt.y = 0.0;                  \
        if (_obj->dataflags & 0x04)                                           \
          _obj->oblique_angle = 0.0;                                          \
        if (_obj->dataflags & 0x08)                                           \
          _obj->rotation = 0.0;                                               \
        if (_obj->dataflags & 0x10)                                           \
          _obj->width_factor = 1.0;                                           \
        if (_obj->dataflags & 0x20)                                           \
          _obj->generation = 0;                                               \
        if (_obj->dataflags & 0x40)                                           \
          _obj->horiz_alignment = 0;                                          \
        if (_obj->dataflags & 0x80)                                           \
          _obj->vert_alignment = 0;                                           \
      }                                                                       \
  }

/*--------------------------------------------------------------------------------
 * Fixups the specs can't express
 */

/* type is the class of the generated function calling these, rather than
   obj->fixedtype: where the compiler inlines them, it then drops the other
   classes' cases, instead of seeing casts of the class's struct to larger
   ones. */
static int
copy_fixup_before (Copy_Chain *restrict cdat, Dwg_Object *restrict obj,
                   const Dwg_Object *restrict srcobj,
                   const Dwg_Object_Type type)
{
  (void)srcobj;
  switch (type)
    {
    /* The specs free these with data_numbits / 8 bytes, which drops a
       partial last byte. */
    case DWG_TYPE_PROXY_ENTITY:
      {
        Dwg_Entity_PROXY_ENTITY *_obj = obj->tio.entity->tio.PROXY_ENTITY;
        copy_bytes (cdat, &_obj->data, ((size_t)_obj->data_numbits + 7) / 8);
      }
      break;
    case DWG_TYPE_PROXY_OBJECT:
      {
        Dwg_Object_PROXY_OBJECT *_obj = obj->tio.object->tio.PROXY_OBJECT;
        copy_bytes (cdat, &_obj->data, ((size_t)_obj->data_numbits + 7) / 8);
      }
      break;
    /* Only the DXF writer walks oleclient, so dwg_free () never frees it:
       the decoder points it at a static string. The copy shares it, which
       the sweep would clear. */
    case DWG_TYPE_OLE2FRAME:
      {
        Dwg_Entity_OLE2FRAME *_obj = obj->tio.entity->tio.OLE2FRAME;
        (void)slot_first_visit (cdat, &_obj->oleclient);
      }
      break;
    default:
      break;
    }
  return 0;
}

static int
copy_fixup_after (Copy_Chain *restrict cdat, Dwg_Object *restrict obj,
                  const Dwg_Object *restrict srcobj,
                  const Dwg_Object_Type type)
{
  switch (type)
    {
    /* The decoder points each dash's text into strings_area; point the
       copies into the copy of it. Texts held elsewhere (DXF imports) are
       copied. */
    case DWG_TYPE_LTYPE:
      {
        Dwg_Object_LTYPE *_obj = obj->tio.object->tio.LTYPE;
        const Dwg_Object_LTYPE *src = srcobj->tio.object->tio.LTYPE;
        const uintptr_t area = (uintptr_t)src->strings_area;
        const uintptr_t area_size
            = cdat->dat.from_version <= R_2004 ? 256 : 512;
        BITCODE_BS i;
        if (!_obj->dashes)
          break;
        for (i = 0; i < _obj->numdashes; i++)
          {
            const uintptr_t text = (uintptr_t)_obj->dashes[i].text;
            if (!text)
              continue;
            if (area && _obj->strings_area
                && _obj->strings_area != src->strings_area && text >= area
                && text < area + area_size)
              _obj->dashes[i].text
                  = (char *)_obj->strings_area + (text - area);
            else
              copy_string (cdat, &_obj->dashes[i].text, COPY_T);
          }
      }
      break;
    case DWG_TYPE_LAYER:
      copy_layer_flags (cdat, obj->tio.object->tio.LAYER,
                        srcobj->tio.object->tio.LAYER);
      break;
    case DWG_TYPE_TEXT:
      COPY_TEXT_DATAFLAGS (TEXT);
      break;
    case DWG_TYPE_ATTRIB:
      COPY_TEXT_DATAFLAGS (ATTRIB);
      break;
    case DWG_TYPE_ATTDEF:
      COPY_TEXT_DATAFLAGS (ATTDEF);
      break;
    /* The encoder of a document read from a DWG file writes xdata_size as
       it is, and leaves out a string that would end beyond it, so it
       follows strings converted to other lengths. The encoder of an import
       or of a document built with the add API computes it as it writes, so
       their xdata_size may be off: dwg_add_XRECORD_string () counts 4 bytes
       and the string, where a string takes 5 and itself. */
    case DWG_TYPE_XRECORD:
      if (cdat->xdata_resized
          || ((cdat->src->opts & DWG_OPTS_IN)
              && !(cdat->dst->opts & DWG_OPTS_IN)))
        {
          Dwg_Object_XRECORD *_obj = obj->tio.object->tio.XRECORD;
          _obj->xdata_size = copy_xdata_size (_obj->xdata, _obj->num_xdata);
          cdat->xdata_resized = 0;
        }
      break;
    /* The walk sets each vertex's num_lines to the MLINE's, as the decoder
       needs it, where a DXF import leaves 0, and copies the MLINE's
       num_lines lines of each. The vertices' counts take their sources'
       values again as copy_restore_values () gives counts theirs. */
    case DWG_TYPE_MLINE:
      {
        Dwg_Entity_MLINE *_obj = obj->tio.entity->tio.MLINE;
        const Dwg_Entity_MLINE *src = srcobj->tio.entity->tio.MLINE;
        BITCODE_BS i;
        for (i = 0; _obj->verts && src->verts && i < _obj->num_verts; i++)
          if (src->verts[i].num_lines <= _obj->verts[i].num_lines)
            _obj->verts[i].num_lines = src->verts[i].num_lines;
      }
      break;
    /* A DWG file has no control points or knots for a fit-point spline
       (scenario 2), but the DXF importer sets them. There the spec walks
       the knots, for freeing, and frees the control points without walking
       them (END_REPEAT), which copies nothing. */
    case DWG_TYPE_SPLINE:
      {
        Dwg_Entity_SPLINE *_obj = obj->tio.entity->tio.SPLINE;
        BITCODE_BL i;
        if (_obj->scenario & 1)
          break;
        (void)copy_vector (cdat, (void **)&_obj->ctrl_pts,
                           _obj->num_ctrl_pts,
                           sizeof (Dwg_SPLINE_control_point));
        for (i = 0; _obj->ctrl_pts && i < _obj->num_ctrl_pts; i++)
          _obj->ctrl_pts[i].parent = _obj;
      }
      break;
    default:
      break;
    }
  return 0;
}

#include "dwg.spec"
#include "dwg2.spec"

/*--------------------------------------------------------------------------------
 * The copy
 */

static int
copy_supported (const Dwg_Object_Type fixedtype)
{
#undef DWG_ENTITY
#undef DWG_OBJECT
#define DWG_ENTITY(name)                                                      \
  case DWG_TYPE_##name:                                                       \
    return 1;
#define DWG_OBJECT(name) DWG_ENTITY (name)

  switch (fixedtype)
    {
#include "objects.inc"
    default:
      return 0;
    }
#undef DWG_ENTITY
#undef DWG_OBJECT
}

static int
copy_object_data (Copy_Chain *restrict cdat, Dwg_Object *restrict obj,
                  const Dwg_Object *restrict srcobj)
{
  Bit_Chain *dat = &cdat->dat;
#define DWG_ENTITY(name)                                                      \
  case DWG_TYPE_##name:                                                       \
    return dwg_copy_##name (dat, obj, srcobj);
#define DWG_OBJECT(name) DWG_ENTITY (name)

  switch (srcobj->fixedtype)
    {
#include "objects.inc"
    default:
      return COPY_REFUSED | DWG_ERR_UNHANDLEDCLASS;
    }
#undef DWG_ENTITY
#undef DWG_OBJECT
}

Dwg_Copy_Map *
dwg_copy_map_new (void)
{
  Dwg_Copy_Map *map = (Dwg_Copy_Map *)calloc (1, sizeof (Dwg_Copy_Map));
  if (!map)
    return NULL;
  map->hash = hash_new (256);
  if (!map->hash)
    {
      free (map);
      return NULL;
    }
  return map;
}

void
dwg_copy_map_free (Dwg_Copy_Map *map)
{
  if (!map)
    return;
  hash_free (map->hash);
  free (map);
}

int
dwg_copy_map_set (Dwg_Copy_Map *restrict map, const BITCODE_RLL src_handle,
                  const BITCODE_RLL dst_handle)
{
  if (!src_handle || !dst_handle)
    return DWG_ERR_INVALIDHANDLE;
  hash_set (map->hash, src_handle, dst_handle);
  return 0;
}

BITCODE_RLL
dwg_copy_map_get (const Dwg_Copy_Map *restrict map,
                  const BITCODE_RLL src_handle)
{
  uint64_t value;
  if (!map || !src_handle)
    return 0;
  value = hash_get (map->hash, src_handle);
  return value == HASH_NOT_FOUND ? 0 : value;
}

static Copy_Kind
copy_kind (const Dwg_Data *dwg)
{
  switch (dwg->opts & DWG_OPTS_IN)
    {
    case DWG_OPTS_IN:
      return COPY_KIND_ADD_API;
    case DWG_OPTS_INDXF:
      return COPY_KIND_DXF;
    case DWG_OPTS_INJSON:
      return COPY_KIND_JSON;
    default:
      return COPY_KIND_DWG;
    }
}

/* Whether a document's 8-bit strings are UTF-8 rather than in its codepage:
   the encoder converts them to the codepage when it writes a document
   imported from JSON or built with the add API (bit_write_TV (), and
   XRECORD strings), and writes the others as they are. */
static int
copy_tv_is_utf8 (const Dwg_Data *dwg)
{
  return (dwg->opts & DWG_OPTS_INJSON) != 0;
}

/* Why src and dst can't be copied between, or 0. */
static int
copy_check_documents (const Dwg_Data *restrict dst,
                      const Dwg_Data *restrict src)
{
  /* Untested from r2007 on, where strings are UTF-16 and free.c frees some
     TABLESTYLE fields outside the spec walk, which this doesn't mirror. The
     r2007 alpha and beta versions sort below R_2007. */
  if (src->header.from_version < R_13b1 || dst->header.from_version < R_13b1
      || src->header.from_version > R_2004
      || dst->header.from_version > R_2004)
    {
      LOG_ERROR ("copy: only documents from r13 to r2004 are supported");
      return COPY_REFUSED | DWG_ERR_NOTYETSUPPORTED;
    }
  if (src == dst)
    return 0;
  if (src->header.from_version != dst->header.from_version)
    {
      LOG_ERROR ("copy: source version %s differs from destination version "
                 "%s",
                 dwg_version_type (src->header.from_version),
                 dwg_version_type (dst->header.from_version));
      return COPY_REFUSED | DWG_ERR_NOTYETSUPPORTED;
    }
  if (src->header.codepage != dst->header.codepage)
    {
      LOG_ERROR ("copy: source codepage %u differs from destination "
                 "codepage %u",
                 (unsigned)src->header.codepage,
                 (unsigned)dst->header.codepage);
      return COPY_REFUSED | DWG_ERR_NOTYETSUPPORTED;
    }
  return 0;
}

/* The copy's type: a fixed type, or the number of dst's class of the same
   name, which is added from src's class when dst has none. *movedp is set
   once dst's classes may have moved, also when adding one then fails. */
static int
copy_class_type (Copy_Chain *restrict cdat, const Dwg_Object *restrict srcobj,
                 BITCODE_BS *restrict typep, int *restrict movedp)
{
  Dwg_Data *dst = cdat->dst;
  const Dwg_Data *src = cdat->src;
  const Dwg_Class *klass;
  Dwg_Class *classes, *copy;
  char *dxfname;
  BITCODE_BL i;

  if (srcobj->type < 500)
    {
      *typep = srcobj->type;
      return 0;
    }
  i = srcobj->type - 500;
  if (i >= src->num_classes || !src->dwg_class[i].dxfname)
    {
      LOG_ERROR ("copy: no class for %s type %u", srcobj->name,
                 (unsigned)srcobj->type);
      return COPY_REFUSED | DWG_ERR_INVALIDTYPE;
    }
  klass = &src->dwg_class[i];
  for (i = 0; i < dst->num_classes; i++)
    if (dst->dwg_class[i].dxfname
        && strEQ (dst->dwg_class[i].dxfname, klass->dxfname))
      {
        *typep = (BITCODE_BS)(500 + i);
        dst->dwg_class[i].num_instances++;
        return 0;
      }

  classes = (Dwg_Class *)realloc (dst->dwg_class, (dst->num_classes + 1)
                                                      * sizeof (Dwg_Class));
  if (!classes)
    return DWG_ERR_OUTOFMEM;
  dst->dwg_class = classes;
  *movedp = 1;
  dxfname = strdup (klass->dxfname);
  if (!dxfname)
    return DWG_ERR_OUTOFMEM;
  copy = &classes[dst->num_classes];
  memcpy (copy, klass, sizeof (Dwg_Class));
  copy->number = (BITCODE_BS)(500 + dst->num_classes);
  copy->num_instances = 1;
  copy->dxfname = dxfname;
  dst->num_classes++;
  /* src's strings, which these replace with copies */
  copy_string (cdat, &copy->appname, COPY_T);
  copy_string (cdat, &copy->cppname, COPY_T);
  copy_string (cdat, (char **)&copy->dxfname_u, COPY_TU);
  /* The next class added may move the array to where these slots were. */
  slots_clear (&cdat->slots);
  *typep = copy->number;
  return 0;
}

/* name and dxfname of a copy: static strings, or dst's own. A variable
   type's dxfname may be src's class name, so it takes dst's. dst frees
   them when it was imported from DXF or JSON, or made with the add API. */
static int
copy_names (Dwg_Data *restrict dst, const Dwg_Data *restrict src,
            Dwg_Object *restrict obj, const Dwg_Object *restrict srcobj)
{
  const char *dxfname = srcobj->dxfname;
  if (obj->type >= 500)
    dxfname = dst->dwg_class[obj->type - 500].dxfname;
  else if (src->opts & DWG_OPTS_IN) // src frees its own
    dxfname = dwg_type_dxfname (srcobj->fixedtype);
  if ((dst->opts & DWG_OPTS_IN) && dxfname)
    {
      obj->dxfname = strdup (dxfname);
      if (!obj->dxfname)
        return DWG_ERR_OUTOFMEM;
    }
  else
    obj->dxfname = (char *)dxfname;

  if (!(src->opts & DWG_OPTS_INJSON) || !srcobj->name)
    obj->name = srcobj->name;
  else
    obj->name = (char *)dwg_type_name (srcobj->fixedtype);
  if ((dst->opts & DWG_OPTS_INJSON) && obj->name)
    {
      obj->name = strdup (obj->name);
      if (!obj->name)
        return DWG_ERR_OUTOFMEM;
    }
  return 0;
}

/* Frees the names copy_names gave dst's copies from index first on that
   have no data: a failure stops the copy before their data is copied, and
   dwg_free_object frees nothing of an object without data. */
static void
copy_free_names (Dwg_Data *restrict dst, const BITCODE_BL first)
{
  BITCODE_BL i;
  for (i = first; i < dst->num_objects; i++)
    {
      Dwg_Object *obj = &dst->object[i];
      if (obj->tio.object)
        continue;
      if (dst->opts & DWG_OPTS_IN)
        {
          free (obj->dxfname);
          obj->dxfname = NULL;
        }
      if (dst->opts & DWG_OPTS_INJSON)
        {
          free (obj->name);
          obj->name = NULL;
        }
    }
}

/* Points dst's objects of a class, from index first on, at their class:
   adding a class may move dst->dwg_class. */
static void
copy_point_at_classes (Dwg_Data *restrict dst, const BITCODE_BL first)
{
  BITCODE_BL i;
  for (i = first; i < dst->num_objects; i++)
    {
      Dwg_Object *obj = &dst->object[i];
      if (obj->type >= 500 && obj->type - 500u < dst->num_classes)
        obj->klass = &dst->dwg_class[obj->type - 500];
    }
}

int
dwg_copy_objects (Dwg_Data *dst, Dwg_Data *src,
                  const BITCODE_RLL *restrict handles,
                  const BITCODE_BL num_handles, Dwg_Copy_Map *restrict map,
                  Dwg_Copy_Unresolved **restrict unresolvedp,
                  BITCODE_BL *restrict num_unresolvedp)
{
  Copy_Chain cdat;
  BITCODE_BL *src_index = NULL, *dst_index = NULL;
  dwg_inthash *seen = NULL;
  BITCODE_BL i, first;
  int realloced = 0, classes_moved = 0;
  int error = 0;

  if (unresolvedp)
    *unresolvedp = NULL;
  if (num_unresolvedp)
    *num_unresolvedp = 0;
  if (!dst || !src || !map || (num_handles && !handles))
    return COPY_REFUSED | DWG_ERR_INTERNALERROR;
  loglevel = dst->opts & DWG_OPTS_LOGLEVEL;
  error = copy_check_documents (dst, src);
  if (error)
    return error;
  if (!num_handles)
    return 0;

  memset (&cdat, 0, sizeof (cdat));
  cdat.dat.opts = src->opts;
  cdat.dat.version = cdat.dat.from_version = src->header.from_version;
  cdat.dat.codepage = src->header.codepage;
  /* The walk checks some sizes against dat->size; nothing is read. */
  cdat.dat.size = 0x7FFFFFFF;
  cdat.src = src;
  cdat.dst = dst;
  cdat.map = map;
  cdat.src_kind = copy_kind (src);
  cdat.dst_kind = copy_kind (dst);
  if (copy_tv_is_utf8 (src) == copy_tv_is_utf8 (dst))
    cdat.tv = COPY_TV_AS_IS;
  else
    cdat.tv = copy_tv_is_utf8 (dst) ? COPY_TV_TO_UTF8 : COPY_TV_FROM_UTF8;
  first = dst->num_objects;

  /* Check every source object before dst changes. */
  src_index = (BITCODE_BL *)calloc (num_handles, sizeof (BITCODE_BL));
  dst_index = (BITCODE_BL *)calloc (num_handles, sizeof (BITCODE_BL));
  seen = hash_new (num_handles * 2);
  if (!src_index || !dst_index || !seen)
    {
      error = DWG_ERR_OUTOFMEM;
      goto done;
    }
  for (i = 0; i < num_handles; i++)
    {
      const Dwg_Object *srcobj = handles[i]
                                     ? dwg_resolve_handle (src, handles[i])
                                     : NULL;
      if (!srcobj)
        {
          LOG_ERROR ("copy: no object " FORMAT_HV " in src", handles[i]);
          error = COPY_REFUSED | DWG_ERR_INVALIDHANDLE;
          goto done;
        }
      if (hash_get (seen, handles[i]) != HASH_NOT_FOUND
          || dwg_copy_map_get (map, handles[i]))
        {
          LOG_ERROR ("copy: object " FORMAT_HV " is already copied or mapped",
                     handles[i]);
          error = COPY_REFUSED | DWG_ERR_INVALIDHANDLE;
          goto done;
        }
      hash_set (seen, handles[i], 1);
      if (!copy_supported (srcobj->fixedtype) || !srcobj->tio.object
          || srcobj->fixedtype == DWG_TYPE_UNKNOWN_ENT
          || srcobj->fixedtype == DWG_TYPE_UNKNOWN_OBJ)
        {
          LOG_ERROR ("copy: %s " FORMAT_HV " is not supported",
                     srcobj->name ? srcobj->name : "?", handles[i]);
          error = COPY_REFUSED | DWG_ERR_UNHANDLEDCLASS;
          goto done;
        }
      if (srcobj->type >= 500
          && (srcobj->type - 500u >= src->num_classes
              || !src->dwg_class[srcobj->type - 500].dxfname))
        {
          LOG_ERROR ("copy: no class for %s " FORMAT_HV, srcobj->name,
                     handles[i]);
          error = COPY_REFUSED | DWG_ERR_INVALIDTYPE;
          goto done;
        }
      src_index[i] = srcobj->index;
    }

  /* The copies and their handles, so that the walk can map references
     between them and recompute relative codes. */
  for (i = 0; i < num_handles; i++)
    {
      const Dwg_Object *srcobj;
      Dwg_Object *obj;
      BITCODE_BS type = 0;
      const BITCODE_BL idx = dst->num_objects;
      int rc = dwg_add_object (dst);
      if (rc > 0)
        {
          error |= rc;
          goto done;
        }
      if (rc < 0)
        realloced = 1;
      srcobj = &src->object[src_index[i]];
      error |= copy_class_type (&cdat, srcobj, &type, &classes_moved);
      // its class's names may have failed to copy too
      if ((error | cdat.error) >= DWG_ERR_CRITICAL)
        goto done;
      obj = &dst->object[idx];
      obj->supertype = srcobj->supertype;
      if (obj->supertype == DWG_SUPERTYPE_ENTITY)
        dst->num_entities++;
      obj->fixedtype = srcobj->fixedtype;
      obj->type = type;
      obj->has_strings = srcobj->has_strings;
      if (obj->fixedtype == DWG_TYPE_LAYOUT && type >= 500
          && !dst->layout_type)
        dst->layout_type = type;
      error |= copy_names (dst, src, obj, srcobj);
      if (error >= DWG_ERR_CRITICAL)
        goto done;
      dwg_set_next_objhandle (obj);
      dwg_copy_map_set (map, srcobj->handle.value, obj->handle.value);
      dst_index[i] = idx;
    }
  copy_point_at_classes (dst, classes_moved ? 0 : first);
  classes_moved = 0;

  for (i = 0; i < num_handles; i++)
    {
      const Dwg_Object *srcobj = &src->object[src_index[i]];
      Dwg_Object *obj = &dst->object[dst_index[i]];
      LOG_TRACE ("copy %s " FORMAT_HV " => " FORMAT_HV "\n", srcobj->name,
                 srcobj->handle.value, obj->handle.value);
      slots_clear (&cdat.slots);
      error |= copy_object_data (&cdat, obj, srcobj);
      error |= cdat.error;
      cdat.error = 0;
      if (error >= DWG_ERR_CRITICAL)
        goto done;
    }

done:
  error |= cdat.error;
  copy_free_names (dst, first);
  // classes added before a failure may have moved dst->dwg_class
  if (classes_moved)
    copy_point_at_classes (dst, 0);
  // dst->object moved: the references' cached objects are stale
  if (realloced)
    dwg_resolve_objectrefs_silent (dst);
  free (src_index);
  free (dst_index);
  if (seen)
    hash_free (seen);
  if (unresolvedp)
    *unresolvedp = cdat.unresolved;
  else
    free (cdat.unresolved);
  if (num_unresolvedp)
    *num_unresolvedp = cdat.num_unresolved;
  free (cdat.slots.keys);
  return error;
}

int
dwg_copy_resolve (Dwg_Data *restrict dst, const Dwg_Copy_Map *restrict map,
                  Dwg_Copy_Unresolved *restrict unresolved,
                  BITCODE_BL *restrict num_unresolvedp)
{
  BITCODE_BL i, j = 0;
  int error = 0;
  if (!dst || !map || !num_unresolvedp)
    return DWG_ERR_INTERNALERROR;
  for (i = 0; i < *num_unresolvedp; i++)
    {
      Dwg_Copy_Unresolved *u = &unresolved[i];
      const BITCODE_RLL mapped = dwg_copy_map_get (map, u->target);
      if (!mapped)
        {
          unresolved[j++] = *u;
          continue;
        }
      switch (u->kind)
        {
        case DWG_COPY_SLOT_REF:
          {
            const Dwg_Object *holder = dwg_resolve_handle (dst, u->holder);
            BITCODE_H ref
                = holder ? copy_new_ref (dst, u->code, mapped, holder)
                         : dwg_add_handleref (dst, u->code > 5 ? 4 : u->code,
                                              mapped, NULL);
            if (!ref)
              {
                error |= DWG_ERR_OUTOFMEM;
                unresolved[j++] = *u;
                continue;
              }
            *(BITCODE_H *)u->slot = ref;
          }
          break;
        case DWG_COPY_SLOT_EED_APPID:
          {
            Dwg_Handle *hdl = (Dwg_Handle *)u->slot;
            hdl->value = mapped;
            dwg_set_handle_size (hdl);
          }
          break;
        case DWG_COPY_SLOT_HANDLE:
          // may be unaligned, in packed EED data
          memcpy (u->slot, &mapped, sizeof (mapped));
          break;
        default:
          unresolved[j++] = *u;
          break;
        }
    }
  *num_unresolvedp = j;
  return error;
}
