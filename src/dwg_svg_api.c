/*****************************************************************************/
/*  LibreDWG - free implementation of the DWG file format                    */
/*                                                                           */
/*  Copyright (C) 2025 Red River Software                                    */
/*                                                                           */
/*  This library is free software, licensed under the terms of the GNU       */
/*  General Public License as published by the Free Software Foundation,     */
/*  either version 3 of the License, or (at your option) any later version.  */
/*  You should have received a copy of the GNU General Public License        */
/*  along with this program.  If not, see <http://www.gnu.org/licenses/>.    */
/*****************************************************************************/

/*
 * dwg_svg_api.c: SVG generation API wrapper
 *
 * This file includes dwg2SVG.c and wraps its functionality to expose a
 * library API. The approach minimizes changes to dwg2SVG.c for easier
 * upstream merging.
 */

/* Must be defined before any includes for strcasestr on Linux */
#define _GNU_SOURCE

#include "config.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dwg.h>
#include <dwg_api.h>
#include <dwg_svg_api.h>

/* ---------------------------------------------------------------------
 * 1) Output callback plumbing
 * --------------------------------------------------------------------- */

typedef void (*dwg_svg_write_cb)(const char *data, size_t len, void *user);

static dwg_svg_write_cb g_svg_writer = NULL;
static void *g_svg_writer_user = NULL;

static int
dwg_svg_printf(const char *fmt, ...)
{
  char buf[8192];
  va_list ap;
  int n;

  va_start(ap, fmt);
  n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  if (n < 0)
    return n;

  if (g_svg_writer)
    {
      size_t len = (size_t)n;
      if (len >= sizeof(buf))
        len = sizeof(buf) - 1;
      g_svg_writer(buf, len, g_svg_writer_user);
    }
  else
    {
      fwrite(buf, 1, (size_t)n, stdout);
    }
  return n;
}

/* ---------------------------------------------------------------------
 * 2) Include dwg2SVG.c with main() disabled and printf() redirected
 * --------------------------------------------------------------------- */

#define DWG2SVG_NO_MAIN
#define printf dwg_svg_printf

/* escape.c and dwg2SVG.c are in ../programs/, adjust path accordingly */
#include "../programs/escape.c"
#include "../programs/dwg2SVG.c"

#undef printf

/*
 * Now we have access to:
 *   - static int opts, static int mspace;
 *   - Dwg_Data g_dwg;
 *   - static void output_SVG(Dwg_Data *dwg);
 *   - all static helpers, etc.
 */

/* ---------------------------------------------------------------------
 * 3) Memory buffer implementation
 * --------------------------------------------------------------------- */

typedef struct
{
  char *data;
  size_t len;
  size_t cap;
} dwg_svg_buffer;

static void
dwg_svg_buffer_writer(const char *chunk, size_t len, void *user)
{
  dwg_svg_buffer *buf = (dwg_svg_buffer *)user;
  char *p;

  if (!len)
    return;

  if (buf->len + len + 1 > buf->cap)
    {
      size_t new_cap = buf->cap ? buf->cap * 2 : 8192;
      while (new_cap < buf->len + len + 1)
        new_cap *= 2;

      p = (char *)realloc(buf->data, new_cap);
      if (!p)
        return; /* OOM: silently drop; caller gets partial SVG */
      buf->data = p;
      buf->cap = new_cap;
    }

  memcpy(buf->data + buf->len, chunk, len);
  buf->len += len;
  buf->data[buf->len] = '\0';
}

/* ---------------------------------------------------------------------
 * 4) File writer implementation
 * --------------------------------------------------------------------- */

static void
dwg_svg_file_writer(const char *chunk, size_t len, void *user)
{
  FILE *f = (FILE *)user;
  if (!len || !f)
    return;
  fwrite(chunk, 1, len, f);
}

/* ---------------------------------------------------------------------
 * 5) Internal render helper for Dwg_Data
 * --------------------------------------------------------------------- */

/* Forward declaration from decode.h */
void dwg_resolve_objectrefs_silent(Dwg_Data *restrict dwg);

static int
dwg_svg_render_data(Dwg_Data *dwg, int only_mspace, dwg_svg_write_cb writer,
                    void *writer_user)
{
  if (!dwg)
    return DWG_ERR_INVALIDDWG;

  /* Ensure object references are resolved (needed for entity iteration).
   * This is especially important for programmatically created documents
   * where dirty_refs may not be set. */
  dwg_resolve_objectrefs_silent(dwg);

  /* Configure global state used by dwg2SVG.c */
  mspace = only_mspace;

  g_svg_writer = writer;
  g_svg_writer_user = writer_user;

  output_SVG(dwg);

  g_svg_writer = NULL;
  g_svg_writer_user = NULL;

  return 0;
}

/* ---------------------------------------------------------------------
 * 6) Internal render helper for file path
 * --------------------------------------------------------------------- */

static int
dwg_svg_render_file(const char *dwg_path, int verbose, int only_mspace,
                    dwg_svg_write_cb writer, void *writer_user)
{
  int error;

  /* Configure global state used by dwg2SVG.c */
  opts = verbose;
  mspace = only_mspace;

  memset(&g_dwg, 0, sizeof(g_dwg));
  g_dwg.opts = opts;

  g_svg_writer = writer;
  g_svg_writer_user = writer_user;

  error = dwg_read_file(dwg_path, &g_dwg);

  if (error < DWG_ERR_CRITICAL)
    output_SVG(&g_dwg);

  /* Always free for library usage */
  dwg_free(&g_dwg);

  g_svg_writer = NULL;
  g_svg_writer_user = NULL;

  return (error >= DWG_ERR_CRITICAL) ? error : 0;
}

/* ---------------------------------------------------------------------
 * 7) Public API: dwg_to_svg (file path to string)
 * --------------------------------------------------------------------- */

EXPORT int
dwg_to_svg(const char *dwg_path, char **svg_out, size_t *svg_len,
           int mspace_only)
{
  dwg_svg_buffer buf = { 0 };
  int rc;

  if (!dwg_path || !svg_out || !svg_len)
    return DWG_ERR_INVALIDDWG;

  rc = dwg_svg_render_file(dwg_path, 0, mspace_only, dwg_svg_buffer_writer,
                           &buf);

  if (rc != 0)
    {
      free(buf.data);
      *svg_out = NULL;
      *svg_len = 0;
      return rc;
    }

  if (!buf.data)
    {
      buf.data = (char *)malloc(1);
      if (!buf.data)
        return DWG_ERR_OUTOFMEM;
      buf.data[0] = '\0';
    }

  *svg_out = buf.data;
  *svg_len = buf.len;
  return 0;
}

/* ---------------------------------------------------------------------
 * 8) Public API: dwg_data_to_svg (Dwg_Data to string)
 * --------------------------------------------------------------------- */

EXPORT int
dwg_data_to_svg(Dwg_Data *dwg, char **svg_out, size_t *svg_len, int mspace_only)
{
  dwg_svg_buffer buf = { 0 };
  int rc;

  if (!dwg || !svg_out || !svg_len)
    return DWG_ERR_INVALIDDWG;

  rc = dwg_svg_render_data(dwg, mspace_only, dwg_svg_buffer_writer, &buf);

  if (rc != 0)
    {
      free(buf.data);
      *svg_out = NULL;
      *svg_len = 0;
      return rc;
    }

  if (!buf.data)
    {
      buf.data = (char *)malloc(1);
      if (!buf.data)
        return DWG_ERR_OUTOFMEM;
      buf.data[0] = '\0';
    }

  *svg_out = buf.data;
  *svg_len = buf.len;
  return 0;
}

/* ---------------------------------------------------------------------
 * 9) Public API: dwg_free_svg
 * --------------------------------------------------------------------- */

EXPORT void
dwg_free_svg(char *svg)
{
  free(svg);
}

/* ---------------------------------------------------------------------
 * 10) Public API: dwg_write_svg (file to file)
 * --------------------------------------------------------------------- */

EXPORT int
dwg_write_svg(const char *dwg_path, const char *svg_path, int mspace_only)
{
  FILE *f;
  int rc;

  if (!dwg_path || !svg_path)
    return DWG_ERR_INVALIDDWG;

  f = fopen(svg_path, "wb");
  if (!f)
    return DWG_ERR_IOERROR;

  rc = dwg_svg_render_file(dwg_path, 0, mspace_only, dwg_svg_file_writer, f);

  fflush(f);
  fclose(f);

  return rc;
}
