/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - storage_backend.h                                       *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2016 Bobby Smiles                                       *
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

#ifndef PARALLAX_STORAGE_H
#define PARALLAX_STORAGE_H

#include <stddef.h>
#include <stdint.h>

struct storage_backend_interface
{
    /* Returns a pointer to storage data
     */
    uint8_t* (*data)(const void* storage);

    /* Returns the size of the storage
     */
    size_t (*size)(const void* storage);

    /* Notify the storage backend that data should be persisted
     */
    void (*save)(void* storage, size_t start, size_t size);
};

struct file_storage
{
    uint8_t* data;
    size_t size;
    const char* filename;
    int first_access;
};

#ifdef __cplusplus
extern "C" {
#endif

/* open_file_storage takes ownership of filename even if reading fails.
 * The data buffer remains available for formatting a new save.
 * open_rom_file_storage takes ownership only on success.
 * close_file_storage releases both data and filename.
 */
int open_file_storage(struct file_storage* storage, size_t size, const char* filename);
int open_rom_file_storage(struct file_storage* storage, const char* filename);
void close_file_storage(struct file_storage* storage);

extern const struct storage_backend_interface g_ifile_storage;
extern const struct storage_backend_interface g_ifile_storage_ro;
extern const struct storage_backend_interface g_isubfile_storage;

#ifdef __cplusplus
}
#endif

#endif
