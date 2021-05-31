/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2016 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup DNA
 */

#pragma once

#include "DNA_ID.h"

#ifdef __cplusplus
extern "C" {
#endif

struct GSet;

/* CacheFile::flag */
enum {
  CACHEFILE_DS_EXPAND = (1 << 0),
  CACHEFILE_UNUSED_0 = (1 << 1),
};

#if 0 /* UNUSED */
/* CacheFile::draw_flag */
enum {
  CACHEFILE_KEYFRAME_DRAWN = (1 << 0),
};
#endif

/* Representation of an object's path inside the Alembic file.
 * Note that this is not a file path. */
typedef struct AlembicObjectPath {
  struct AlembicObjectPath *next, *prev;

  char path[4096];
} AlembicObjectPath;

/* CacheFile::velocity_unit
 * Determines what temporal unit is used to interpret velocity vectors for motion blur effects. */
enum {
  CACHEFILE_VELOCITY_UNIT_FRAME,
  CACHEFILE_VELOCITY_UNIT_SECOND,
};

typedef struct CacheFile {
  ID id;
  struct AnimData *adt;

  /** Paths of the objects inside of the Alembic archive referenced by this CacheFile. */
  ListBase object_paths;

  /** 1024 = FILE_MAX. */
  char filepath[1024];

  char is_sequence;
  char forward_axis;
  char up_axis;
  char override_frame;

  float scale;
  /** The frame/time to lookup in the cache file. */
  float frame;
  /** The frame offset to subtract. */
  float frame_offset;

  /** Default radius assigned to curves or points if no such property exists for them. */
  float default_radius;

  /** Animation flag. */
  short flag;

  /** Do not load data from the cache file and display objects in the scene as boxes, Cycles will
   * load objects directly from the CacheFile. Other render engines which can load Alembic data
   * directly can take care of rendering it themselves.
   */
  char use_render_procedural;

  char velocity_unit;
  /* Name of the velocity property in the Alembic file. */
  char velocity_name[64];

  /** Enable data prefetching when using the Cycles Procedural. */
  char use_prefetch;
  char _pad[3];

  int prefetch_cache_size;

  /** The frequency in frame per seconds at which the data in the cache file should evaluated.
   * This is necessary to have here as the data may have been generated based on a different
   * FPS than the one used for the scene (e.g. some asset was produced at 60 FPS and used in
   * a project rendered/animated at 120 FPS).
   */
  float frame_rate;
  char _pad2[4];

  /* Runtime */
  struct AbcArchiveHandle *handle;
  char handle_filepath[1024];
  struct GSet *handle_readers;
} CacheFile;

#ifdef __cplusplus
}
#endif
