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

using System;
using System.Runtime.InteropServices;

namespace LibreDWGInterop
{
    /// <summary>
    /// Provides SVG generation functionality for DWG files.
    /// This is a hand-written wrapper that provides a cleaner C# API than
    /// the auto-generated SWIG bindings for functions with char** parameters.
    /// </summary>
    public static class DwgSvgApi
    {
        // P/Invoke directly into libredwg
        // Build scripts copy platform-specific libs to this name
        private const string NativeLibrary = "libredwg";

        [DllImport(NativeLibrary, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        private static extern int dwg_to_svg(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string dwgPath,
            out IntPtr svgOut,
            out UIntPtr svgLen,
            int mspaceOnly);

        [DllImport(NativeLibrary, CallingConvention = CallingConvention.Cdecl)]
        private static extern int dwg_data_to_svg(
            IntPtr dwg,
            out IntPtr svgOut,
            out UIntPtr svgLen,
            int mspaceOnly);

        [DllImport(NativeLibrary, CallingConvention = CallingConvention.Cdecl)]
        private static extern void dwg_free_svg(IntPtr svg);

        [DllImport(NativeLibrary, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        private static extern int dwg_write_svg(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string dwgPath,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string svgPath,
            int mspaceOnly);

        /// <summary>
        /// Converts a DWG file to an SVG string.
        /// </summary>
        /// <param name="dwgPath">Path to the DWG file.</param>
        /// <param name="mspaceOnly">If true, render only model space; otherwise prefer paper space if present.</param>
        /// <returns>The SVG content as a string.</returns>
        /// <exception cref="DwgException">Thrown when conversion fails.</exception>
        public static string ToSvg(string dwgPath, bool mspaceOnly = false)
        {
            if (string.IsNullOrEmpty(dwgPath))
                throw new ArgumentNullException(nameof(dwgPath));

            int result = dwg_to_svg(dwgPath, out IntPtr svgPtr, out UIntPtr svgLen, mspaceOnly ? 1 : 0);

            if (result != 0)
                throw new DwgException($"Failed to convert DWG to SVG. Error code: {result}");

            if (svgPtr == IntPtr.Zero)
                return string.Empty;

            try
            {
                return Marshal.PtrToStringUTF8(svgPtr) ?? string.Empty;
            }
            finally
            {
                dwg_free_svg(svgPtr);
            }
        }

        /// <summary>
        /// Converts an already-loaded Dwg_Data structure to an SVG string.
        /// </summary>
        /// <param name="dwgData">The Dwg_Data object.</param>
        /// <param name="mspaceOnly">If true, render only model space; otherwise prefer paper space if present.</param>
        /// <returns>The SVG content as a string.</returns>
        /// <exception cref="DwgException">Thrown when conversion fails.</exception>
        public static string ToSvg(Dwg_Data dwgData, bool mspaceOnly = false)
        {
            if (dwgData == null)
                throw new ArgumentNullException(nameof(dwgData));

            int result = dwg_data_to_svg(Dwg_Data.getCPtr(dwgData).Handle, out IntPtr svgPtr, out UIntPtr svgLen, mspaceOnly ? 1 : 0);

            if (result != 0)
                throw new DwgException($"Failed to convert DWG data to SVG. Error code: {result}");

            if (svgPtr == IntPtr.Zero)
                return string.Empty;

            try
            {
                return Marshal.PtrToStringUTF8(svgPtr) ?? string.Empty;
            }
            finally
            {
                dwg_free_svg(svgPtr);
            }
        }

        /// <summary>
        /// Converts a DWG file and writes the SVG output directly to a file.
        /// </summary>
        /// <param name="dwgPath">Path to the DWG file.</param>
        /// <param name="svgPath">Path for the output SVG file.</param>
        /// <param name="mspaceOnly">If true, render only model space.</param>
        /// <exception cref="DwgException">Thrown when conversion fails.</exception>
        public static void WriteSvg(string dwgPath, string svgPath, bool mspaceOnly = false)
        {
            if (string.IsNullOrEmpty(dwgPath))
                throw new ArgumentNullException(nameof(dwgPath));
            if (string.IsNullOrEmpty(svgPath))
                throw new ArgumentNullException(nameof(svgPath));

            int result = dwg_write_svg(dwgPath, svgPath, mspaceOnly ? 1 : 0);

            if (result != 0)
                throw new DwgException($"Failed to write SVG file. Error code: {result}");
        }
    }

    /// <summary>
    /// Exception thrown when a DWG operation fails.
    /// </summary>
    public class DwgException : Exception
    {
        public DwgException(string message) : base(message) { }
        public DwgException(string message, Exception innerException) : base(message, innerException) { }
    }
}
