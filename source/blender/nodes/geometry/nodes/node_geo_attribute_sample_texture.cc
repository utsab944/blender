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

#include "BLI_compiler_attrs.h"
#include "BLI_task.hh"

#include "DNA_texture_types.h"

#include "BKE_texture.h"

#include "RE_texture.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "node_geometry_util.hh"

static bNodeSocketTemplate geo_node_attribute_sample_texture_in[] = {
    {SOCK_GEOMETRY, N_("Geometry")},
    {SOCK_TEXTURE, N_("Texture")},
    {SOCK_ATTRIBUTE, N_("Mapping"), 0.0f, 0.0f, 0.0f, 0.0f, -FLT_MAX, FLT_MAX},
    {-1, ""},
};

static bNodeSocketTemplate geo_node_attribute_sample_texture_out[] = {
    {SOCK_GEOMETRY, N_("Geometry")},
    {SOCK_ATTRIBUTE, N_("Result")},
    {-1, ""},
};

static void geo_node_attribute_sample_texture_init(bNodeTree *UNUSED(tree), bNode *node)
{
  blender::nodes::set_attribute_socket_data_type(*node, "Mapping", SOCK_VECTOR);
  blender::nodes::set_attribute_socket_data_type(*node, "Result", SOCK_RGBA);
}

namespace blender::nodes {

static AttributeDomain get_result_domain(const GeometryComponent &component,
                                         const StringRef map_name)
{
  /* Use the name of the map attribute. */
  std::optional<AttributeMetaData> map_info = component.attribute_get_meta_data(map_name);
  if (map_info) {
    return map_info->domain;
  }

  /* The node won't execute in this case, but we still have to return a value. */
  return ATTR_DOMAIN_POINT;
}

static void execute_on_component(GeometryComponent &component,
                                 const GeoNodeExecParams &params,
                                 const AttributeRef &result_ref)
{
  Tex *texture = params.get_input<Tex *>("Texture");
  if (texture == nullptr) {
    return;
  }

  const AttributeRef mapping_ref = params.get_input<AttributeRef>("Mapping");
  if (!component.attribute_exists(mapping_ref.name())) {
    return;
  }

  const AttributeDomain result_domain = get_result_domain(component, mapping_ref.name());

  OutputAttribute_Typed<ColorGeometry4f> attribute_out =
      component.attribute_try_get_for_output_only<ColorGeometry4f>(result_ref.name(),
                                                                   result_domain);
  if (!attribute_out) {
    return;
  }

  GVArray_Typed<float3> mapping_attribute = component.attribute_get_for_read<float3>(
      mapping_ref.name(), result_domain, {0, 0, 0});

  MutableSpan<ColorGeometry4f> colors = attribute_out.as_span();
  threading::parallel_for(IndexRange(mapping_attribute.size()), 128, [&](IndexRange range) {
    for (const int i : range) {
      TexResult texture_result = {0};
      const float3 position = mapping_attribute[i];
      /* For legacy reasons we have to map [0, 1] to [-1, 1] to support uv mappings. */
      const float3 remapped_position = position * 2.0f - float3(1.0f);
      BKE_texture_get_value(nullptr, texture, remapped_position, &texture_result, false);
      colors[i] = {texture_result.tr, texture_result.tg, texture_result.tb, texture_result.ta};
    }
  });

  attribute_out.save();
}

static void geo_node_attribute_sample_texture_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry");
  AttributeRef result_attribute_ref = params.declare_output_attribute("Result", CD_PROP_COLOR);

  geometry_set = geometry_set_realize_instances(geometry_set);

  if (geometry_set.has<MeshComponent>()) {
    execute_on_component(
        geometry_set.get_component_for_write<MeshComponent>(), params, result_attribute_ref);
  }
  if (geometry_set.has<PointCloudComponent>()) {
    execute_on_component(
        geometry_set.get_component_for_write<PointCloudComponent>(), params, result_attribute_ref);
  }
  if (geometry_set.has<CurveComponent>()) {
    execute_on_component(
        geometry_set.get_component_for_write<CurveComponent>(), params, result_attribute_ref);
  }

  params.set_output("Geometry", geometry_set);
  params.set_output("Result", result_attribute_ref);
}

}  // namespace blender::nodes

void register_node_type_geo_sample_texture()
{
  static bNodeType ntype;

  geo_node_type_base(&ntype,
                     GEO_NODE_ATTRIBUTE_SAMPLE_TEXTURE,
                     "Attribute Sample Texture",
                     NODE_CLASS_ATTRIBUTE,
                     0);
  node_type_size_preset(&ntype, NODE_SIZE_LARGE);
  node_type_init(&ntype, &geo_node_attribute_sample_texture_init);
  node_type_socket_templates(
      &ntype, geo_node_attribute_sample_texture_in, geo_node_attribute_sample_texture_out);
  ntype.geometry_node_execute = blender::nodes::geo_node_attribute_sample_texture_exec;
  nodeRegisterType(&ntype);
}
