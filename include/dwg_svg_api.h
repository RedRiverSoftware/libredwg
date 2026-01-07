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
 * dwg_svg_api.h: SVG generation API for libredwg
 * Exposes dwg2SVG functionality as a library API for use from .NET and other
 * languages.
 */

#ifndef DWG_SVG_API_H
#define DWG_SVG_API_H

#include <stddef.h>
#include "dwg.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Convert a DWG file to an SVG string.
 *
 * @param dwg_path    Path to the DWG file to convert.
 * @param svg_out     On success, receives a pointer to the SVG string (UTF-8).
 *                    Caller must free with dwg_free_svg().
 * @param svg_len     On success, receives the length of the SVG string in bytes.
 * @param mspace_only If non-zero, render only model space; otherwise prefer
 *                    paper space if present.
 * @return 0 on success, or a DWG_ERR_* code on failure.
 */
EXPORT int dwg_to_svg(const char *dwg_path, char **svg_out, size_t *svg_len,
                      int mspace_only);

/**
 * Convert a Dwg_Data structure to an SVG string.
 *
 * @param dwg         Pointer to an already-loaded Dwg_Data structure.
 * @param svg_out     On success, receives a pointer to the SVG string (UTF-8).
 *                    Caller must free with dwg_free_svg().
 * @param svg_len     On success, receives the length of the SVG string in bytes.
 * @param mspace_only If non-zero, render only model space; otherwise prefer
 *                    paper space if present.
 * @return 0 on success, or a DWG_ERR_* code on failure.
 */
EXPORT int dwg_data_to_svg(Dwg_Data *dwg, char **svg_out, size_t *svg_len,
                           int mspace_only);

/**
 * Free an SVG string returned by dwg_to_svg() or dwg_data_to_svg().
 *
 * @param svg  The SVG string to free.
 */
EXPORT void dwg_free_svg(char *svg);

/**
 * Convert a DWG file and write the SVG output directly to a file.
 *
 * @param dwg_path    Path to the DWG file to convert.
 * @param svg_path    Path to the output SVG file.
 * @param mspace_only If non-zero, render only model space.
 * @return 0 on success, or a DWG_ERR_* code on failure.
 */
EXPORT int dwg_write_svg(const char *dwg_path, const char *svg_path,
                         int mspace_only);

#ifdef __cplusplus
}
#endif

#endif /* DWG_SVG_API_H */
