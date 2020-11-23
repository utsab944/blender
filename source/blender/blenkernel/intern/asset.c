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
 */

/** \file
 * \ingroup bke
 */

#include <string.h>

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BKE_asset.h"
#include "BKE_icons.h"
#include "BKE_idprop.h"

#include "DNA_ID.h"
#include "DNA_asset_types.h"
#include "DNA_defaults.h"

#include "BLO_read_write.h"

#include "MEM_guardedalloc.h"

AssetData *BKE_asset_data_create(void)
{
  AssetData *asset_data = MEM_callocN(sizeof(AssetData), __func__);
  memcpy(asset_data, DNA_struct_default_get(AssetData), sizeof(*asset_data));
  return asset_data;
}

void BKE_asset_data_free(AssetData *asset_data)
{
  if (asset_data->properties) {
    IDP_FreeProperty(asset_data->properties);
  }
  MEM_SAFE_FREE(asset_data->description);
  BLI_freelistN(&asset_data->tags);

  MEM_SAFE_FREE(asset_data);
}

struct CustomTagEnsureResult BKE_assetdata_tag_ensure(AssetData *asset_data, const char *name)
{
  struct CustomTagEnsureResult result = {.tag = NULL};
  if (!name[0]) {
    return result;
  }

  CustomTag *tag = BLI_findstring(&asset_data->tags, name, offsetof(CustomTag, name));

  if (tag) {
    result.tag = tag;
    result.is_new = false;
    return result;
  }

  tag = MEM_mallocN(sizeof(*tag), __func__);
  BLI_strncpy(tag->name, name, sizeof(tag->name));

  BLI_addtail(&asset_data->tags, tag);

  result.tag = tag;
  result.is_new = true;
  return result;
}

void BKE_assetdata_tag_remove(AssetData *asset_data, CustomTag *tag)
{
  BLI_freelinkN(&asset_data->tags, tag);
}

/* .blend file API -------------------------------------------- */

void BKE_assetdata_write(BlendWriter *writer, AssetData *asset_data)
{
  BLO_write_struct(writer, AssetData, asset_data);

  if (asset_data->properties) {
    IDP_BlendWrite(writer, asset_data->properties);
  }

  if (asset_data->description) {
    BLO_write_string(writer, asset_data->description);
  }
  LISTBASE_FOREACH (CustomTag *, tag, &asset_data->tags) {
    BLO_write_struct(writer, CustomTag, tag);
  }
}

void BKE_assetdata_read(BlendDataReader *reader, AssetData *asset_data)
{
  /* asset_data itself has been read already. */

  if (asset_data->properties) {
    BLO_read_data_address(reader, &asset_data->properties);
    IDP_BlendDataRead(reader, &asset_data->properties);
  }

  BLO_read_data_address(reader, &asset_data->description);
  BLO_read_list(reader, &asset_data->tags);
}
