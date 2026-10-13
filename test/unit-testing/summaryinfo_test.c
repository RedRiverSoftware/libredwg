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

/* Uncompressed r2004 SummaryInfo, AppInfo and AppInfoHistory sections
   larger than section_max_decomp_size's caps for them (0x100, 0x400,
   0x1000), which apply to compressed sections only, written and read back;
   and drawing properties too long for a T16 string. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#include "bits.h"

/* The most characters of a T16 string that AutoCAD reads: it takes the
   length, which counts the NUL, as signed. */
#define MAX_T16_LENGTH (INT16_MAX - 1)

static const char *const subject = "The subject, after the title";
static const char *const prop_tag = "Drawing number";
static const char *const prop_value = "DN-0001";

/* Text of the given length that shows its own offsets: "<000000><000008>" */
static char *
offset_text (size_t length)
{
  char *text = (char *)malloc (length + 9);
  size_t n = 0;
  while (n < length)
    n += (size_t)snprintf (&text[n], 9, "<%06u>", (unsigned)n);
  text[length] = '\0';
  return text;
}

static char *
copy_text (const char *text)
{
  char *copy = (char *)malloc (strlen (text) + 1);
  strcpy (copy, text);
  return copy;
}

/* A new r2004 document with the given title, a subject and one custom
   property: the last two follow the title in the section. */
static Dwg_Data *
new_document (const char *title)
{
  Dwg_Data *dwg = dwg_new_Document (R_2004, 0, tracelevel);
  Dwg_SummaryInfo *info = &dwg->summaryinfo;

  free (info->TITLE);
  info->TITLE = copy_text (title);
  free (info->SUBJECT);
  info->SUBJECT = copy_text (subject);
  info->num_props = 1;
  info->props = (Dwg_SummaryInfo_Property *)calloc (
      1, sizeof (Dwg_SummaryInfo_Property));
  info->props[0].tag = copy_text (prop_tag);
  info->props[0].value = copy_text (prop_value);
  return dwg;
}

static int
write_and_read (Dwg_Data *dwg, const char *file, Dwg_Data *read)
{
  int error;
  memset (read, 0, sizeof (Dwg_Data));
  // dwg_write_file refuses to overwrite a file
  unlink (file);
  error = dwg_write_file (file, dwg);
  dwg_free (dwg);
  free (dwg);
  if (error >= DWG_ERR_CRITICAL)
    {
      loglevel = test_loglevel;
      fail ("%s: dwg_write_file 0x%x", file, error);
      return error;
    }
  read->opts = tracelevel;
  error = dwg_read_file (file, read);
  loglevel = test_loglevel;
  if (error >= DWG_ERR_CRITICAL)
    fail ("%s: dwg_read_file 0x%x", file, error);
  return error;
}

static void
free_read (Dwg_Data *read)
{
  loglevel = tracelevel;
  dwg_free (read);
  loglevel = test_loglevel;
}

static const Dwg_Section_Info *
find_section_info (const Dwg_Data *dwg, const Dwg_Section_Type type)
{
  for (BITCODE_BL i = 0; i < dwg->header.section_infohdr.num_desc; i++)
    if (dwg->header.section_info[i].fixedtype == type)
      return &dwg->header.section_info[i];
  return NULL;
}

/* A title of the given length is written, trimmed to MAX_T16_LENGTH if
   it is longer, and read back with the fields after it. */
static int
test_title (const size_t length)
{
  const size_t expected = length < MAX_T16_LENGTH ? length : MAX_T16_LENGTH;
  char file[64];
  char *title = offset_text (length);
  const Dwg_SummaryInfo *info;
  const Dwg_Section_Info *section;
  Dwg_Data read;

  failed = 0;
  snprintf (file, sizeof (file), "summaryinfo_title_%u.dwg",
            (unsigned)length);
  if (write_and_read (new_document (title), file, &read) >= DWG_ERR_CRITICAL)
    {
      free_read (&read);
      free (title);
      return numfailed ();
    }

  info = &read.summaryinfo;
  if (info->TITLE && strlen (info->TITLE) == expected
      && !memcmp (info->TITLE, title, expected))
    ok ("title %u: %u characters read back", (unsigned)length,
        (unsigned)expected);
  else
    fail ("title %u: read back %d characters, expected %u", (unsigned)length,
          info->TITLE ? (int)strlen (info->TITLE) : -1, (unsigned)expected);
  if (info->SUBJECT && !strcmp (info->SUBJECT, subject)
      && info->num_props == 1 && info->props && info->props[0].tag
      && !strcmp (info->props[0].tag, prop_tag) && info->props[0].value
      && !strcmp (info->props[0].value, prop_value))
    ok ("title %u: subject and custom property read back", (unsigned)length);
  else
    fail ("title %u: subject or custom property lost", (unsigned)length);
  // larger than section_max_decomp_size's cap for SummaryInfo
  section = find_section_info (&read, SECTION_SUMMARYINFO);
  if (section && section->max_decomp_size > 0x100)
    ok ("title %u: SummaryInfo page size 0x%x", (unsigned)length,
        (unsigned)section->max_decomp_size);
  else
    fail ("title %u: SummaryInfo page size 0x%x", (unsigned)length,
          section ? (unsigned)section->max_decomp_size : 0);

  free_read (&read);
  free (title);
  if (!numfailed ())
    unlink (file);
  return numfailed ();
}

/* An AppInfo section over its 0x400 cap: one long comment. Written
   as is, in the layout the reader decodes for r2004 (class version 2). */
static int
test_appinfo (void)
{
  const char *file = "summaryinfo_appinfo.dwg";
  // NULL: the comment
  const char *const strings[]
      = { "AppInfoDataList", NULL, "<ProductInformation/>", "1.0" };
  char *comment = offset_text (0x600);
  Dwg_Data *dwg = new_document ("AppInfo");
  Dwg_AppInfo *appinfo = &dwg->appinfo;
  BITCODE_RC *p;
  Dwg_Data read;

  failed = 0;
  // appinfo_name, num_strings, comment, product_info, version
  appinfo->size = 4 + 2 * 4;
  for (int i = 0; i < 4; i++)
    appinfo->size += (BITCODE_RL)strlen (strings[i] ? strings[i] : comment)
                     + 1;
  appinfo->unknown_bits = (BITCODE_TF)calloc (appinfo->size, 1);
  p = appinfo->unknown_bits;
  for (int i = 0; i < 4; i++)
    {
      const char *s = strings[i] ? strings[i] : comment;
      const size_t len = strlen (s) + 1;
      p[0] = (BITCODE_RC)(len & 0xff);
      p[1] = (BITCODE_RC)(len >> 8);
      memcpy (&p[2], s, len);
      p += 2 + len;
      if (i == 0)
        {
          p[0] = 3; // num_strings, RL
          p += 4;
        }
    }

  if (write_and_read (dwg, file, &read) >= DWG_ERR_CRITICAL)
    {
      free_read (&read);
      free (comment);
      return numfailed ();
    }
  if (read.appinfo.size > 0x400 && read.appinfo.comment
      && !strcmp (read.appinfo.comment, comment))
    ok ("appinfo: %u bytes, comment read back", (unsigned)read.appinfo.size);
  else
    fail ("appinfo: %u bytes, comment %s", (unsigned)read.appinfo.size,
          read.appinfo.comment ? "differs" : "missing");

  free_read (&read);
  free (comment);
  if (!numfailed ())
    unlink (file);
  return numfailed ();
}

/* An AppInfoHistory section over its 0x1000 cap, written as is. */
static int
test_appinfohistory (void)
{
  const char *file = "summaryinfo_appinfohistory.dwg";
  const BITCODE_RL size = 0x2000;
  Dwg_Data *dwg = new_document ("AppInfoHistory");
  BITCODE_TF bytes = (BITCODE_TF)malloc (size);
  Dwg_Data read;

  failed = 0;
  for (BITCODE_RL i = 0; i < size; i++)
    bytes[i] = (BITCODE_RC)(i * 7 + 1);
  dwg->appinfohistory.size = size;
  dwg->appinfohistory.unknown_bits = (BITCODE_TF)malloc (size);
  memcpy (dwg->appinfohistory.unknown_bits, bytes, size);

  if (write_and_read (dwg, file, &read) >= DWG_ERR_CRITICAL)
    {
      free_read (&read);
      free (bytes);
      return numfailed ();
    }
  if (read.appinfohistory.size == size && read.appinfohistory.unknown_bits
      && !memcmp (read.appinfohistory.unknown_bits, bytes, size))
    ok ("appinfohistory: %u bytes read back", (unsigned)size);
  else
    fail ("appinfohistory: read back %u bytes, expected %u",
          (unsigned)read.appinfohistory.size, (unsigned)size);

  free_read (&read);
  free (bytes);
  if (!numfailed ())
    unlink (file);
  return numfailed ();
}

/* bit_write_T16 trims a string to fit INT16_MAX as the length: before
   r2007 the length counts the NUL, from r2007 on it doesn't. */
static int
test_write_T16 (const Dwg_Version_Type version, const size_t length,
                const unsigned expected)
{
  const unsigned unit = version >= R_2007 ? 2 : 1;
  char *text = offset_text (length);
  Bit_Chain dat = { 0 };
  unsigned written;

  failed = 0;
  dat.size = 2 * length + 16;
  dat.chain = (unsigned char *)calloc (dat.size, 1);
  dat.version = version;
  dat.from_version = R_2004;
  dat.codepage = 30; // ANSI_1252
  bit_write_T16 (&dat, text);
  loglevel = test_loglevel;
  written = (unsigned)(dat.chain[0] | (dat.chain[1] << 8));
  if (written == expected && dat.byte == 2 + (size_t)unit * expected)
    ok ("write_T16 %s %u: length %u", dwg_version_type (version),
        (unsigned)length, written);
  else
    fail ("write_T16 %s %u: length %u, %u bytes, expected %u",
          dwg_version_type (version), (unsigned)length, written,
          (unsigned)dat.byte, expected);
  free (dat.chain);
  free (text);
  return numfailed ();
}

int
main (int argc, char *argv[])
{
  int error = 0;
  char *trace = getenv ("LIBREDWG_TRACE");
  test_loglevel = is_make_silent () ? 0 : 2; // print ok
  tracelevel = trace ? atoi (trace) : 0;
  (void)argc;
  (void)argv;

  // a SummaryInfo just over its 0x100 cap
  error += test_title (300);
  // two 0x7400 pages
  error += test_title (30000);
  // the longest title AutoCAD reads
  error += test_title (MAX_T16_LENGTH);
  // far too long: trimmed, and the fields after it stay intact
  error += test_title (70000);
  error += test_appinfo ();
  error += test_appinfohistory ();
  // the length counts the NUL: 32766 characters
  error += test_write_T16 (R_2004, INT16_MAX - 1, INT16_MAX);
  error += test_write_T16 (R_2004, INT16_MAX, INT16_MAX);
  error += test_write_T16 (R_2004, 40000, INT16_MAX);
  // the length doesn't count the NUL: 32767 characters
  error += test_write_T16 (R_2007, INT16_MAX, INT16_MAX);
  error += test_write_T16 (R_2007, 40000, INT16_MAX);
  return error ? 1 : 0;
}
