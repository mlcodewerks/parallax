/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - video_capture_backend.h                                 *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2017 Bobby Smiles                                       *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#ifndef PARALLAX_BACKENDS_API_VIDEO_CAPTURE_BACKEND_H
#define PARALLAX_BACKENDS_API_VIDEO_CAPTURE_BACKEND_H

#include "core/types.h"

/* Define default video capture backend
 * if not defined by Makefile/msvc
 */
#if !defined(DEFAULT_VIDEO_CAPTURE_BACKEND)
#if defined(PARALLAX_OPENCV)
#define DEFAULT_VIDEO_CAPTURE_BACKEND "opencv"
#else
#define DEFAULT_VIDEO_CAPTURE_BACKEND ""
#endif
#endif


struct video_capture_backend_interface
{
    /* Backend class name.
     * Must be unique.
     */
    const char* name;

    /* Initialize backend instance (*vin)
     * using (when provided) parameters from config section.
     *
     * Returns CORE_OK on success.
     *
     * You must call corresponding release method to release any allocated resources.
     */
    core_result (*init)(void** vin, const char* section);

    /* Release backend instance and any associated resouces.
     */
    void (*release)(void* vin);

    /* Open a video stream with following properties (width, height)
     * Returns CORE_OK on success.
     */
    core_result (*open)(void* vin, unsigned int width, unsigned int height);

    /* Close a previsouly opened video stream.
     */
    void (*close)(void* vin);

    /* Grab a BGR image on an open stream
     * Returns CORE_OK on success.
     */
    core_result (*grab_image)(void* vin, void* data);
};

/* collection of available video capture backends */
extern const struct video_capture_backend_interface* g_video_capture_backend_interfaces[];

/* helper function which find backend by name (or dummy when NULL/empty) */
const struct video_capture_backend_interface* get_video_capture_backend(const char* name);

#endif
