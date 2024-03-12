/* SPDX-FileCopyrightText: 2012 by Nicholas Bishop. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <cstdlib>

#include "MEM_guardedalloc.h"

#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_vec_types.h"

#include "BLI_array_utils.hh"
#include "BLI_bit_span_ops.hh"
#include "BLI_bitmap_draw_2d.h"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_lasso_2d.hh"
#include "BLI_math_base.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_polyfill_2d.h"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_ccg.h"
#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"
#include "BKE_multires.hh"
#include "BKE_paint.hh"
#include "BKE_pbvh_api.hh"
#include "BKE_scene.hh"
#include "BKE_subdiv_ccg.hh"
#include "BKE_subsurf.hh"

#include "DEG_depsgraph.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_sculpt.hh"
#include "ED_view3d.hh"

#include "bmesh.hh"
#include "tools/bmesh_boolean.hh"

#include "paint_intern.hh"

/* For undo push. */
#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint::mask {
Array<float> duplicate_mask(const Object &object)
{
  const SculptSession &ss = *object.sculpt;
  switch (BKE_pbvh_type(ss.pbvh)) {
    case PBVH_FACES: {
      const Mesh &mesh = *static_cast<const Mesh *>(object.data);
      const bke::AttributeAccessor attributes = mesh.attributes();
      const VArray mask = *attributes.lookup_or_default<float>(
          ".sculpt_mask", bke::AttrDomain::Point, 0.0f);
      Array<float> result(mask.size());
      mask.materialize(result);
      return result;
    }
    case PBVH_GRIDS: {
      const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
      const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
      const Span<CCGElem *> grids = subdiv_ccg.grids;

      Array<float> result(grids.size() * key.grid_area);
      int index = 0;
      for (const int grid : grids.index_range()) {
        CCGElem *elem = grids[grid];
        for (const int i : IndexRange(key.grid_area)) {
          result[index] = *CCG_elem_offset_mask(&key, elem, i);
          index++;
        }
      }
      return result;
    }
    case PBVH_BMESH: {
      BMesh &bm = *ss.bm;
      const int offset = CustomData_get_offset_named(&bm.vdata, CD_PROP_FLOAT, ".sculpt_mask");
      Array<float> result(bm.totvert);
      if (offset == -1) {
        result.fill(0.0f);
      }
      else {
        BM_mesh_elem_table_ensure(&bm, BM_VERT);
        for (const int i : result.index_range()) {
          result[i] = BM_ELEM_CD_GET_FLOAT(BM_vert_at_index(&bm, i), offset);
        }
      }
      return result;
    }
  }
  BLI_assert_unreachable();
  return {};
}

/* The gesture API doesn't write to this enum type,
 * it writes to eSelectOp from ED_select_utils.hh.
 * We must thus map the modes here to the desired
 * eSelectOp modes.
 *
 * Fixes #102349.
 */
enum PaintMaskFloodMode {
  PAINT_MASK_FLOOD_VALUE = SEL_OP_SUB,
  PAINT_MASK_FLOOD_VALUE_INVERSE = SEL_OP_ADD,
  PAINT_MASK_INVERT = SEL_OP_XOR,
};

static const EnumPropertyItem mode_items[] = {
    {PAINT_MASK_FLOOD_VALUE,
     "VALUE",
     0,
     "Value",
     "Set mask to the level specified by the 'value' property"},
    {PAINT_MASK_FLOOD_VALUE_INVERSE,
     "VALUE_INVERSE",
     0,
     "Value Inverted",
     "Set mask to the level specified by the inverted 'value' property"},
    {PAINT_MASK_INVERT, "INVERT", 0, "Invert", "Invert the mask"},
    {0}};

static float mask_flood_fill_get_new_value_for_elem(const float elem,
                                                    PaintMaskFloodMode mode,
                                                    float value)
{
  switch (mode) {
    case PAINT_MASK_FLOOD_VALUE:
      return value;
    case PAINT_MASK_FLOOD_VALUE_INVERSE:
      return 1.0f - value;
    case PAINT_MASK_INVERT:
      return 1.0f - elem;
  }
  BLI_assert_unreachable();
  return 0.0f;
}

static Span<int> get_visible_verts(const PBVHNode &node,
                                   const Span<bool> hide_vert,
                                   Vector<int> &indices)
{
  if (BKE_pbvh_node_fully_hidden_get(&node)) {
    return {};
  }
  const Span<int> verts = BKE_pbvh_node_get_unique_vert_indices(&node);
  if (hide_vert.is_empty()) {
    return verts;
  }
  indices.resize(verts.size());
  const int *end = std::copy_if(verts.begin(), verts.end(), indices.begin(), [&](const int vert) {
    return !hide_vert[vert];
  });
  indices.resize(end - indices.begin());
  return indices;
}

static Span<int> get_hidden_verts(const PBVHNode &node,
                                  const Span<bool> hide_vert,
                                  Vector<int> &indices)
{
  if (hide_vert.is_empty()) {
    return {};
  }
  const Span<int> verts = BKE_pbvh_node_get_unique_vert_indices(&node);
  if (BKE_pbvh_node_fully_hidden_get(&node)) {
    return verts;
  }
  indices.resize(verts.size());
  const int *end = std::copy_if(verts.begin(), verts.end(), indices.begin(), [&](const int vert) {
    return hide_vert[vert];
  });
  indices.resize(end - indices.begin());
  return indices;
}

static bool try_remove_mask_mesh(Object &object, const Span<PBVHNode *> nodes)
{
  Mesh &mesh = *static_cast<Mesh *>(object.data);
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  const VArraySpan mask = *attributes.lookup<float>(".sculpt_mask", bke::AttrDomain::Point);
  if (mask.is_empty()) {
    return true;
  }

  /* If there are any hidden vertices that shouldn't be affected with a mask value set, the
   * attribute cannot be removed. This could also be done by building an IndexMask in the full
   * vertex domain. */
  const VArraySpan hide_vert = *attributes.lookup<bool>(".hide_vert", bke::AttrDomain::Point);
  threading::EnumerableThreadSpecific<Vector<int>> all_index_data;
  const bool hidden_masked_verts = threading::parallel_reduce(
      nodes.index_range(),
      1,
      false,
      [&](const IndexRange range, bool init) {
        if (init) {
          return init;
        }
        Vector<int> &index_data = all_index_data.local();
        for (const PBVHNode *node : nodes.slice(range)) {
          const Span<int> verts = get_hidden_verts(*node, hide_vert, index_data);
          if (std::any_of(verts.begin(), verts.end(), [&](int i) { return mask[i] > 0.0f; })) {
            return true;
          }
        }
        return false;
      },
      std::logical_or());
  if (hidden_masked_verts) {
    return false;
  }

  /* Store undo data for nodes with changed mask. */
  threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
    for (PBVHNode *node : nodes.slice(range)) {
      const Span<int> verts = BKE_pbvh_node_get_unique_vert_indices(node);
      if (std::all_of(verts.begin(), verts.end(), [&](const int i) { return mask[i] == 0.0f; })) {
        continue;
      }
      undo::push_node(&object, node, undo::Type::Mask);
      BKE_pbvh_node_mark_redraw(node);
    }
  });

  attributes.remove(".sculpt_mask");
  return true;
}

static void fill_mask_mesh(Object &object, const float value, const Span<PBVHNode *> nodes)
{
  Mesh &mesh = *static_cast<Mesh *>(object.data);
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  const VArraySpan hide_vert = *attributes.lookup<bool>(".hide_vert", bke::AttrDomain::Point);
  if (value == 0.0f) {
    if (try_remove_mask_mesh(object, nodes)) {
      return;
    }
  }

  bke::SpanAttributeWriter<float> mask = attributes.lookup_or_add_for_write_span<float>(
      ".sculpt_mask", bke::AttrDomain::Point);

  threading::EnumerableThreadSpecific<Vector<int>> all_index_data;
  threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
    Vector<int> &index_data = all_index_data.local();
    for (PBVHNode *node : nodes.slice(range)) {
      const Span<int> verts = get_visible_verts(*node, hide_vert, index_data);
      if (std::all_of(verts.begin(), verts.end(), [&](int i) { return mask.span[i] == value; })) {
        continue;
      }
      undo::push_node(&object, node, undo::Type::Mask);
      mask.span.fill_indices(verts, value);
      BKE_pbvh_node_mark_redraw(node);
    }
  });

  mask.finish();
}

static void fill_mask_grids(Main &bmain,
                            const Scene &scene,
                            Depsgraph &depsgraph,
                            Object &object,
                            const float value,
                            const Span<PBVHNode *> nodes)
{
  SubdivCCG &subdiv_ccg = *object.sculpt->subdiv_ccg;

  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  if (value == 0.0f && !key.has_mask) {
    /* Unlike meshes, don't dynamically remove masks since it is interleaved with other data. */
    return;
  }

  MultiresModifierData &mmd = *BKE_sculpt_multires_active(&scene, &object);
  BKE_sculpt_mask_layers_ensure(&depsgraph, &bmain, &object, &mmd);

  const BitGroupVector<> &grid_hidden = subdiv_ccg.grid_hidden;

  const Span<CCGElem *> grids = subdiv_ccg.grids;
  bool any_changed = false;
  threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
    for (PBVHNode *node : nodes.slice(range)) {
      const Span<int> grid_indices = BKE_pbvh_node_get_grid_indices(*node);
      if (std::all_of(grid_indices.begin(), grid_indices.end(), [&](const int grid) {
            CCGElem *elem = grids[grid];
            for (const int i : IndexRange(key.grid_area)) {
              if (*CCG_elem_offset_mask(&key, elem, i) != value) {
                return false;
              }
            }
            return true;
          }))
      {
        continue;
      }
      undo::push_node(&object, node, undo::Type::Mask);

      if (grid_hidden.is_empty()) {
        for (const int grid : grid_indices) {
          CCGElem *elem = grids[grid];
          for (const int i : IndexRange(key.grid_area)) {
            *CCG_elem_offset_mask(&key, elem, i) = value;
          }
        }
      }
      else {
        for (const int grid : grid_indices) {
          CCGElem *elem = grids[grid];
          bits::foreach_0_index(grid_hidden[grid], [&](const int i) {
            *CCG_elem_offset_mask(&key, elem, i) = value;
          });
        }
      }
      BKE_pbvh_node_mark_redraw(node);
      any_changed = true;
    }
  });

  if (any_changed) {
    multires_mark_as_modified(&depsgraph, &object, MULTIRES_COORDS_MODIFIED);
  }
}

static void fill_mask_bmesh(Object &object, const float value, const Span<PBVHNode *> nodes)
{
  BMesh &bm = *object.sculpt->bm;
  const int offset = CustomData_get_offset_named(&bm.vdata, CD_PROP_FLOAT, ".sculpt_mask");
  if (value == 0.0f && offset == -1) {
    return;
  }
  if (offset == -1) {
    /* Mask is not dynamically added or removed for dynamic topology sculpting. */
    BLI_assert_unreachable();
    return;
  }

  undo::push_node(&object, nodes.first(), undo::Type::Mask);
  threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
    for (PBVHNode *node : nodes.slice(range)) {
      bool redraw = false;
      for (BMVert *vert : BKE_pbvh_bmesh_node_unique_verts(node)) {
        if (!BM_elem_flag_test(vert, BM_ELEM_HIDDEN)) {
          if (BM_ELEM_CD_GET_FLOAT(vert, offset) != value) {
            BM_ELEM_CD_SET_FLOAT(vert, offset, value);
            redraw = true;
          }
        }
      }
      if (redraw) {
        BKE_pbvh_node_mark_redraw(node);
      }
    }
  });
}

static void fill_mask(
    Main &bmain, const Scene &scene, Depsgraph &depsgraph, Object &object, const float value)
{
  PBVH &pbvh = *object.sculpt->pbvh;
  Vector<PBVHNode *> nodes = bke::pbvh::search_gather(&pbvh, {});
  switch (BKE_pbvh_type(&pbvh)) {
    case PBVH_FACES:
      fill_mask_mesh(object, value, nodes);
      break;
    case PBVH_GRIDS:
      fill_mask_grids(bmain, scene, depsgraph, object, value, nodes);
      break;
    case PBVH_BMESH:
      fill_mask_bmesh(object, value, nodes);
      break;
  }
  /* Avoid calling #BKE_pbvh_node_mark_update_mask by doing that update here. */
  for (PBVHNode *node : nodes) {
    BKE_pbvh_node_fully_masked_set(node, value == 1.0f);
    BKE_pbvh_node_fully_unmasked_set(node, value == 0.0f);
  }
}

static void invert_mask_mesh(Object &object, const Span<PBVHNode *> nodes)
{
  Mesh &mesh = *static_cast<Mesh *>(object.data);
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();

  const VArraySpan hide_vert = *attributes.lookup<bool>(".hide_vert", bke::AttrDomain::Point);
  bke::SpanAttributeWriter<float> mask = attributes.lookup_or_add_for_write_span<float>(
      ".sculpt_mask", bke::AttrDomain::Point);
  threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
    for (PBVHNode *node : nodes.slice(range)) {
      undo::push_node(&object, node, undo::Type::Mask);
      for (const int vert : BKE_pbvh_node_get_unique_vert_indices(node)) {
        if (!hide_vert.is_empty() && hide_vert[vert]) {
          continue;
        }
        mask.span[vert] = 1.0f - mask.span[vert];
      }
      BKE_pbvh_node_mark_redraw(node);
      bke::pbvh::node_update_mask_mesh(mask.span, *node);
    }
  });
  mask.finish();
}

static void invert_mask_grids(Main &bmain,
                              const Scene &scene,
                              Depsgraph &depsgraph,
                              Object &object,
                              const Span<PBVHNode *> nodes)
{
  SubdivCCG &subdiv_ccg = *object.sculpt->subdiv_ccg;

  MultiresModifierData &mmd = *BKE_sculpt_multires_active(&scene, &object);
  BKE_sculpt_mask_layers_ensure(&depsgraph, &bmain, &object, &mmd);

  const BitGroupVector<> &grid_hidden = subdiv_ccg.grid_hidden;

  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const Span<CCGElem *> grids = subdiv_ccg.grids;
  threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
    for (PBVHNode *node : nodes.slice(range)) {
      undo::push_node(&object, node, undo::Type::Mask);

      const Span<int> grid_indices = BKE_pbvh_node_get_grid_indices(*node);
      if (grid_hidden.is_empty()) {
        for (const int grid : grid_indices) {
          CCGElem *elem = grids[grid];
          for (const int i : IndexRange(key.grid_area)) {
            *CCG_elem_offset_mask(&key, elem, i) = 1.0f - *CCG_elem_offset_mask(&key, elem, i);
          }
        }
      }
      else {
        for (const int grid : grid_indices) {
          CCGElem *elem = grids[grid];
          bits::foreach_0_index(grid_hidden[grid], [&](const int i) {
            *CCG_elem_offset_mask(&key, elem, i) = 1.0f - *CCG_elem_offset_mask(&key, elem, i);
          });
        }
      }
      BKE_pbvh_node_mark_update_mask(node);
      bke::pbvh::node_update_mask_grids(key, grids, *node);
    }
  });

  multires_mark_as_modified(&depsgraph, &object, MULTIRES_COORDS_MODIFIED);
}

static void invert_mask_bmesh(Object &object, const Span<PBVHNode *> nodes)
{
  BMesh &bm = *object.sculpt->bm;
  const int offset = CustomData_get_offset_named(&bm.vdata, CD_PROP_FLOAT, ".sculpt_mask");
  if (offset == -1) {
    BLI_assert_unreachable();
    return;
  }

  undo::push_node(&object, nodes.first(), undo::Type::Mask);
  threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
    for (PBVHNode *node : nodes.slice(range)) {
      for (BMVert *vert : BKE_pbvh_bmesh_node_unique_verts(node)) {
        if (!BM_elem_flag_test(vert, BM_ELEM_HIDDEN)) {
          BM_ELEM_CD_SET_FLOAT(vert, offset, 1.0f - BM_ELEM_CD_GET_FLOAT(vert, offset));
        }
      }
      BKE_pbvh_node_mark_update_mask(node);
      bke::pbvh::node_update_mask_bmesh(offset, *node);
    }
  });
}

static void invert_mask(Main &bmain, const Scene &scene, Depsgraph &depsgraph, Object &object)
{
  Vector<PBVHNode *> nodes = bke::pbvh::search_gather(object.sculpt->pbvh, {});
  switch (BKE_pbvh_type(object.sculpt->pbvh)) {
    case PBVH_FACES:
      invert_mask_mesh(object, nodes);
      break;
    case PBVH_GRIDS:
      invert_mask_grids(bmain, scene, depsgraph, object, nodes);
      break;
    case PBVH_BMESH:
      invert_mask_bmesh(object, nodes);
      break;
  }
}

static int mask_flood_fill_exec(bContext *C, wmOperator *op)
{
  Main &bmain = *CTX_data_main(C);
  const Scene &scene = *CTX_data_scene(C);
  Object &object = *CTX_data_active_object(C);
  Depsgraph &depsgraph = *CTX_data_ensure_evaluated_depsgraph(C);

  const PaintMaskFloodMode mode = PaintMaskFloodMode(RNA_enum_get(op->ptr, "mode"));
  const float value = RNA_float_get(op->ptr, "value");

  BKE_sculpt_update_object_for_edit(&depsgraph, &object, false);

  undo::push_begin(&object, op);
  switch (mode) {
    case PAINT_MASK_FLOOD_VALUE:
      fill_mask(bmain, scene, depsgraph, object, value);
      break;
    case PAINT_MASK_FLOOD_VALUE_INVERSE:
      fill_mask(bmain, scene, depsgraph, object, 1.0f - value);
      break;
    case PAINT_MASK_INVERT:
      invert_mask(bmain, scene, depsgraph, object);
      break;
  }

  undo::push_end(&object);

  SCULPT_tag_update_overlays(C);

  return OPERATOR_FINISHED;
}

void PAINT_OT_mask_flood_fill(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Mask Flood Fill";
  ot->idname = "PAINT_OT_mask_flood_fill";
  ot->description = "Fill the whole mask with a given value, or invert its values";

  /* API callbacks. */
  ot->exec = mask_flood_fill_exec;
  ot->poll = SCULPT_mode_poll;

  ot->flag = OPTYPE_REGISTER;

  /* RNA. */
  RNA_def_enum(ot->srna, "mode", mode_items, PAINT_MASK_FLOOD_VALUE, "Mode", nullptr);
  RNA_def_float(
      ot->srna,
      "value",
      0.0f,
      0.0f,
      1.0f,
      "Value",
      "Mask level to use when mode is 'Value'; zero means no masking and one is fully masked",
      0.0f,
      1.0f);
}
/* Face Set Gesture Operation. */

struct SculptGestureFaceSetOperation {
  gesture::Operation op;

  int new_face_set_id;
};

static void sculpt_gesture_face_set_begin(bContext &C, gesture::GestureData &gesture_data)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(&C);
  BKE_sculpt_update_object_for_edit(depsgraph, gesture_data.vc.obact, false);
}

static void face_set_gesture_apply_mesh(gesture::GestureData &gesture_data,
                                        const Span<PBVHNode *> nodes)
{
  SculptGestureFaceSetOperation *face_set_operation = (SculptGestureFaceSetOperation *)
                                                          gesture_data.operation;
  const int new_face_set = face_set_operation->new_face_set_id;
  Object &object = *gesture_data.vc.obact;
  SculptSession &ss = *gesture_data.ss;
  const PBVH &pbvh = *gesture_data.ss->pbvh;

  const Span<float3> positions = ss.vert_positions;
  const OffsetIndices<int> faces = ss.faces;
  const Span<int> corner_verts = ss.corner_verts;
  const bool *hide_poly = ss.hide_poly;
  bke::SpanAttributeWriter<int> face_sets = face_set::ensure_face_sets_mesh(object);

  threading::parallel_for(gesture_data.nodes.index_range(), 1, [&](const IndexRange range) {
    for (PBVHNode *node : nodes.slice(range)) {
      undo::push_node(gesture_data.vc.obact, node, undo::Type::FaceSet);

      bool any_updated = false;
      for (const int face : BKE_pbvh_node_calc_face_indices(pbvh, *node)) {
        if (hide_poly && hide_poly[face]) {
          continue;
        }
        const Span<int> face_verts = corner_verts.slice(faces[face]);
        const float3 face_center = bke::mesh::face_center_calc(positions, face_verts);
        const float3 face_normal = bke::mesh::face_normal_calc(positions, face_verts);
        if (!gesture::is_affected(gesture_data, face_center, face_normal)) {
          continue;
        }
        face_sets.span[face] = new_face_set;
        any_updated = true;
      }
      if (any_updated) {
        BKE_pbvh_node_mark_update_face_sets(node);
      }
    }
  });

  face_sets.finish();
}

static void face_set_gesture_apply_bmesh(gesture::GestureData &gesture_data,
                                         const Span<PBVHNode *> nodes)
{
  SculptGestureFaceSetOperation *face_set_operation = (SculptGestureFaceSetOperation *)
                                                          gesture_data.operation;
  const int new_face_set = face_set_operation->new_face_set_id;
  SculptSession &ss = *gesture_data.ss;
  BMesh *bm = ss.bm;
  const int offset = CustomData_get_offset_named(&bm->pdata, CD_PROP_INT32, ".sculpt_face_set");

  threading::parallel_for(gesture_data.nodes.index_range(), 1, [&](const IndexRange range) {
    for (PBVHNode *node : nodes.slice(range)) {
      undo::push_node(gesture_data.vc.obact, node, undo::Type::FaceSet);

      bool any_updated = false;
      for (BMFace *face : BKE_pbvh_bmesh_node_faces(node)) {
        if (BM_elem_flag_test(face, BM_ELEM_HIDDEN)) {
          continue;
        }
        float3 center;
        BM_face_calc_center_median(face, center);
        if (!gesture::is_affected(gesture_data, center, face->no)) {
          continue;
        }
        BM_ELEM_CD_SET_INT(face, offset, new_face_set);
        any_updated = true;
      }

      if (any_updated) {
        BKE_pbvh_node_mark_update_visibility(node);
      }
    }
  });
}

static void sculpt_gesture_face_set_apply_for_symmetry_pass(bContext & /*C*/,
                                                            gesture::GestureData &gesture_data)
{
  switch (BKE_pbvh_type(gesture_data.ss->pbvh)) {
    case PBVH_GRIDS:
    case PBVH_FACES:
      face_set_gesture_apply_mesh(gesture_data, gesture_data.nodes);
      break;
    case PBVH_BMESH:
      face_set_gesture_apply_bmesh(gesture_data, gesture_data.nodes);
  }
}

static void sculpt_gesture_face_set_end(bContext & /*C*/, gesture::GestureData & /*gesture_data*/)
{
}

static void sculpt_gesture_init_face_set_properties(gesture::GestureData &gesture_data,
                                                    wmOperator & /*op*/)
{
  Object &object = *gesture_data.vc.obact;
  gesture_data.operation = reinterpret_cast<gesture::Operation *>(
      MEM_cnew<SculptGestureFaceSetOperation>(__func__));

  SculptGestureFaceSetOperation *face_set_operation = (SculptGestureFaceSetOperation *)
                                                          gesture_data.operation;

  face_set_operation->op.begin = sculpt_gesture_face_set_begin;
  face_set_operation->op.apply_for_symmetry_pass = sculpt_gesture_face_set_apply_for_symmetry_pass;
  face_set_operation->op.end = sculpt_gesture_face_set_end;

  face_set_operation->new_face_set_id = face_set::find_next_available_id(object);
}

/* Mask Gesture Operation. */

struct SculptGestureMaskOperation {
  gesture::Operation op;

  PaintMaskFloodMode mode;
  float value;
};

static void sculpt_gesture_mask_begin(bContext &C, gesture::GestureData &gesture_data)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(&C);
  BKE_sculpt_update_object_for_edit(depsgraph, gesture_data.vc.obact, false);
}

static void mask_gesture_apply_task(gesture::GestureData &gesture_data,
                                    const SculptMaskWriteInfo mask_write,
                                    PBVHNode *node)
{
  SculptGestureMaskOperation *mask_operation = (SculptGestureMaskOperation *)
                                                   gesture_data.operation;
  Object *ob = gesture_data.vc.obact;

  const bool is_multires = BKE_pbvh_type(gesture_data.ss->pbvh) == PBVH_GRIDS;

  PBVHVertexIter vd;
  bool any_masked = false;
  bool redraw = false;

  BKE_pbvh_vertex_iter_begin (gesture_data.ss->pbvh, node, vd, PBVH_ITER_UNIQUE) {
    float vertex_normal[3];
    const float *co = SCULPT_vertex_co_get(gesture_data.ss, vd.vertex);
    SCULPT_vertex_normal_get(gesture_data.ss, vd.vertex, vertex_normal);

    if (gesture::is_affected(gesture_data, co, vertex_normal)) {
      float prevmask = vd.mask;
      if (!any_masked) {
        any_masked = true;

        undo::push_node(ob, node, undo::Type::Mask);

        if (is_multires) {
          BKE_pbvh_node_mark_positions_update(node);
        }
      }
      const float new_mask = mask_flood_fill_get_new_value_for_elem(
          prevmask, mask_operation->mode, mask_operation->value);
      if (prevmask != new_mask) {
        SCULPT_mask_vert_set(BKE_pbvh_type(ob->sculpt->pbvh), mask_write, new_mask, vd);
        redraw = true;
      }
    }
  }
  BKE_pbvh_vertex_iter_end;

  if (redraw) {
    BKE_pbvh_node_mark_update_mask(node);
  }
}

static void sculpt_gesture_mask_apply_for_symmetry_pass(bContext & /*C*/,
                                                        gesture::GestureData &gesture_data)
{
  const SculptMaskWriteInfo mask_write = SCULPT_mask_get_for_write(gesture_data.ss);
  threading::parallel_for(gesture_data.nodes.index_range(), 1, [&](const IndexRange range) {
    for (const int i : range) {
      mask_gesture_apply_task(gesture_data, mask_write, gesture_data.nodes[i]);
    }
  });
}

static void sculpt_gesture_mask_end(bContext &C, gesture::GestureData &gesture_data)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(&C);
  if (BKE_pbvh_type(gesture_data.ss->pbvh) == PBVH_GRIDS) {
    multires_mark_as_modified(depsgraph, gesture_data.vc.obact, MULTIRES_COORDS_MODIFIED);
  }
  blender::bke::pbvh::update_mask(*gesture_data.ss->pbvh);
}

static void sculpt_gesture_init_mask_properties(bContext &C,
                                                gesture::GestureData &gesture_data,
                                                wmOperator &op)
{
  gesture_data.operation = reinterpret_cast<gesture::Operation *>(
      MEM_cnew<SculptGestureMaskOperation>(__func__));

  SculptGestureMaskOperation *mask_operation = (SculptGestureMaskOperation *)
                                                   gesture_data.operation;

  Object *object = gesture_data.vc.obact;
  MultiresModifierData *mmd = BKE_sculpt_multires_active(gesture_data.vc.scene, object);
  BKE_sculpt_mask_layers_ensure(
      CTX_data_depsgraph_pointer(&C), CTX_data_main(&C), gesture_data.vc.obact, mmd);

  mask_operation->op.begin = sculpt_gesture_mask_begin;
  mask_operation->op.apply_for_symmetry_pass = sculpt_gesture_mask_apply_for_symmetry_pass;
  mask_operation->op.end = sculpt_gesture_mask_end;

  mask_operation->mode = PaintMaskFloodMode(RNA_enum_get(op.ptr, "mode"));
  mask_operation->value = RNA_float_get(op.ptr, "value");
}

static void paint_mask_gesture_operator_properties(wmOperatorType *ot)
{
  RNA_def_enum(ot->srna, "mode", mode_items, PAINT_MASK_FLOOD_VALUE, "Mode", nullptr);
  RNA_def_float(
      ot->srna,
      "value",
      1.0f,
      0.0f,
      1.0f,
      "Value",
      "Mask level to use when mode is 'Value'; zero means no masking and one is fully masked",
      0.0f,
      1.0f);
}

/* Project Gesture Operation. */

struct SculptGestureProjectOperation {
  gesture::Operation operation;
};

static void sculpt_gesture_project_begin(bContext &C, gesture::GestureData &gesture_data)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(&C);
  BKE_sculpt_update_object_for_edit(depsgraph, gesture_data.vc.obact, false);
}

static void project_line_gesture_apply_task(gesture::GestureData &gesture_data, PBVHNode *node)
{
  PBVHVertexIter vd;
  bool any_updated = false;

  undo::push_node(gesture_data.vc.obact, node, undo::Type::Position);

  BKE_pbvh_vertex_iter_begin (gesture_data.ss->pbvh, node, vd, PBVH_ITER_UNIQUE) {
    float vertex_normal[3];
    const float *co = SCULPT_vertex_co_get(gesture_data.ss, vd.vertex);
    SCULPT_vertex_normal_get(gesture_data.ss, vd.vertex, vertex_normal);

    if (!gesture::is_affected(gesture_data, co, vertex_normal)) {
      continue;
    }

    float projected_pos[3];
    closest_to_plane_v3(projected_pos, gesture_data.line.plane, vd.co);

    float disp[3];
    sub_v3_v3v3(disp, projected_pos, vd.co);
    const float mask = vd.mask;
    mul_v3_fl(disp, 1.0f - mask);
    if (is_zero_v3(disp)) {
      continue;
    }
    add_v3_v3(vd.co, disp);
    any_updated = true;
  }
  BKE_pbvh_vertex_iter_end;

  if (any_updated) {
    BKE_pbvh_node_mark_update(node);
  }
}

static void sculpt_gesture_project_apply_for_symmetry_pass(bContext & /*C*/,
                                                           gesture::GestureData &gesture_data)
{
  switch (gesture_data.shape_type) {
    case gesture::ShapeType::Line:
      threading::parallel_for(gesture_data.nodes.index_range(), 1, [&](const IndexRange range) {
        for (const int i : range) {
          project_line_gesture_apply_task(gesture_data, gesture_data.nodes[i]);
        }
      });
      break;
    case gesture::ShapeType::Lasso:
    case gesture::ShapeType::Box:
      /* Gesture shape projection not implemented yet. */
      BLI_assert(false);
      break;
  }
}

static void sculpt_gesture_project_end(bContext &C, gesture::GestureData &gesture_data)
{
  SculptSession *ss = gesture_data.ss;
  Sculpt *sd = CTX_data_tool_settings(&C)->sculpt;
  if (ss->deform_modifiers_active || ss->shapekey_active) {
    SCULPT_flush_stroke_deform(sd, gesture_data.vc.obact, true);
  }

  SCULPT_flush_update_step(&C, SCULPT_UPDATE_COORDS);
  SCULPT_flush_update_done(&C, gesture_data.vc.obact, SCULPT_UPDATE_COORDS);
}

static void sculpt_gesture_init_project_properties(gesture::GestureData &gesture_data,
                                                   wmOperator & /*op*/)
{
  gesture_data.operation = reinterpret_cast<gesture::Operation *>(
      MEM_cnew<SculptGestureFaceSetOperation>(__func__));

  SculptGestureProjectOperation *project_operation = (SculptGestureProjectOperation *)
                                                         gesture_data.operation;

  project_operation->operation.begin = sculpt_gesture_project_begin;
  project_operation->operation.apply_for_symmetry_pass =
      sculpt_gesture_project_apply_for_symmetry_pass;
  project_operation->operation.end = sculpt_gesture_project_end;
}

static int paint_mask_gesture_box_exec(bContext *C, wmOperator *op)
{
  std::unique_ptr<gesture::GestureData> gesture_data = gesture::init_from_box(C, op);
  if (!gesture_data) {
    return OPERATOR_CANCELLED;
  }
  sculpt_gesture_init_mask_properties(*C, *gesture_data, *op);
  gesture::apply(*C, *gesture_data, *op);
  return OPERATOR_FINISHED;
}

static int paint_mask_gesture_lasso_exec(bContext *C, wmOperator *op)
{
  std::unique_ptr<gesture::GestureData> gesture_data = gesture::init_from_lasso(C, op);
  if (!gesture_data) {
    return OPERATOR_CANCELLED;
  }
  sculpt_gesture_init_mask_properties(*C, *gesture_data, *op);
  gesture::apply(*C, *gesture_data, *op);
  return OPERATOR_FINISHED;
}

static int paint_mask_gesture_line_exec(bContext *C, wmOperator *op)
{
  std::unique_ptr<gesture::GestureData> gesture_data = gesture::init_from_line(C, op);
  if (!gesture_data) {
    return OPERATOR_CANCELLED;
  }
  sculpt_gesture_init_mask_properties(*C, *gesture_data, *op);
  gesture::apply(*C, *gesture_data, *op);
  return OPERATOR_FINISHED;
}

static int face_set_gesture_box_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  const View3D *v3d = CTX_wm_view3d(C);
  const Base *base = CTX_data_active_base(C);
  if (!BKE_base_is_visible(v3d, base)) {
    return OPERATOR_CANCELLED;
  }

  return WM_gesture_box_invoke(C, op, event);
}

static int face_set_gesture_box_exec(bContext *C, wmOperator *op)
{
  std::unique_ptr<gesture::GestureData> gesture_data = gesture::init_from_box(C, op);
  if (!gesture_data) {
    return OPERATOR_CANCELLED;
  }
  sculpt_gesture_init_face_set_properties(*gesture_data, *op);
  gesture::apply(*C, *gesture_data, *op);
  return OPERATOR_FINISHED;
}

static int face_set_gesture_lasso_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  const View3D *v3d = CTX_wm_view3d(C);
  const Base *base = CTX_data_active_base(C);
  if (!BKE_base_is_visible(v3d, base)) {
    return OPERATOR_CANCELLED;
  }

  return WM_gesture_lasso_invoke(C, op, event);
}

static int face_set_gesture_lasso_exec(bContext *C, wmOperator *op)
{
  std::unique_ptr<gesture::GestureData> gesture_data = gesture::init_from_lasso(C, op);
  if (!gesture_data) {
    return OPERATOR_CANCELLED;
  }
  sculpt_gesture_init_face_set_properties(*gesture_data, *op);
  gesture::apply(*C, *gesture_data, *op);
  return OPERATOR_FINISHED;
}

static int project_line_gesture_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  const View3D *v3d = CTX_wm_view3d(C);
  const Base *base = CTX_data_active_base(C);
  if (!BKE_base_is_visible(v3d, base)) {
    return OPERATOR_CANCELLED;
  }

  return WM_gesture_straightline_active_side_invoke(C, op, event);
}

static int project_gesture_line_exec(bContext *C, wmOperator *op)
{
  std::unique_ptr<gesture::GestureData> gesture_data = gesture::init_from_line(C, op);
  if (!gesture_data) {
    return OPERATOR_CANCELLED;
  }
  sculpt_gesture_init_project_properties(*gesture_data, *op);
  gesture::apply(*C, *gesture_data, *op);
  return OPERATOR_FINISHED;
}

void PAINT_OT_mask_lasso_gesture(wmOperatorType *ot)
{
  ot->name = "Mask Lasso Gesture";
  ot->idname = "PAINT_OT_mask_lasso_gesture";
  ot->description = "Add mask within the lasso as you move the brush";

  ot->invoke = WM_gesture_lasso_invoke;
  ot->modal = WM_gesture_lasso_modal;
  ot->exec = paint_mask_gesture_lasso_exec;

  ot->poll = SCULPT_mode_poll_view3d;

  ot->flag = OPTYPE_REGISTER | OPTYPE_DEPENDS_ON_CURSOR;

  /* Properties. */
  WM_operator_properties_gesture_lasso(ot);
  gesture::operator_properties(ot);

  paint_mask_gesture_operator_properties(ot);
}

void PAINT_OT_mask_box_gesture(wmOperatorType *ot)
{
  ot->name = "Mask Box Gesture";
  ot->idname = "PAINT_OT_mask_box_gesture";
  ot->description = "Add mask within the box as you move the brush";

  ot->invoke = WM_gesture_box_invoke;
  ot->modal = WM_gesture_box_modal;
  ot->exec = paint_mask_gesture_box_exec;

  ot->poll = SCULPT_mode_poll_view3d;

  ot->flag = OPTYPE_REGISTER;

  /* Properties. */
  WM_operator_properties_border(ot);
  gesture::operator_properties(ot);

  paint_mask_gesture_operator_properties(ot);
}

void PAINT_OT_mask_line_gesture(wmOperatorType *ot)
{
  ot->name = "Mask Line Gesture";
  ot->idname = "PAINT_OT_mask_line_gesture";
  ot->description = "Add mask to the right of a line as you move the brush";

  ot->invoke = WM_gesture_straightline_active_side_invoke;
  ot->modal = WM_gesture_straightline_oneshot_modal;
  ot->exec = paint_mask_gesture_line_exec;

  ot->poll = SCULPT_mode_poll_view3d;

  ot->flag = OPTYPE_REGISTER;

  /* Properties. */
  WM_operator_properties_gesture_straightline(ot, WM_CURSOR_EDIT);
  gesture::operator_properties(ot);

  paint_mask_gesture_operator_properties(ot);
}

void SCULPT_OT_face_set_lasso_gesture(wmOperatorType *ot)
{
  ot->name = "Face Set Lasso Gesture";
  ot->idname = "SCULPT_OT_face_set_lasso_gesture";
  ot->description = "Add face set within the lasso as you move the brush";

  ot->invoke = face_set_gesture_lasso_invoke;
  ot->modal = WM_gesture_lasso_modal;
  ot->exec = face_set_gesture_lasso_exec;

  ot->poll = SCULPT_mode_poll_view3d;

  ot->flag = OPTYPE_DEPENDS_ON_CURSOR;

  /* Properties. */
  WM_operator_properties_gesture_lasso(ot);
  gesture::operator_properties(ot);
}

void SCULPT_OT_face_set_box_gesture(wmOperatorType *ot)
{
  ot->name = "Face Set Box Gesture";
  ot->idname = "SCULPT_OT_face_set_box_gesture";
  ot->description = "Add face set within the box as you move the brush";

  ot->invoke = face_set_gesture_box_invoke;
  ot->modal = WM_gesture_box_modal;
  ot->exec = face_set_gesture_box_exec;

  ot->poll = SCULPT_mode_poll_view3d;

  ot->flag = OPTYPE_REGISTER;

  /* Properties. */
  WM_operator_properties_border(ot);
  gesture::operator_properties(ot);
}

void SCULPT_OT_project_line_gesture(wmOperatorType *ot)
{
  ot->name = "Project Line Gesture";
  ot->idname = "SCULPT_OT_project_line_gesture";
  ot->description = "Project the geometry onto a plane defined by a line";

  ot->invoke = project_line_gesture_invoke;
  ot->modal = WM_gesture_straightline_oneshot_modal;
  ot->exec = project_gesture_line_exec;

  ot->poll = SCULPT_mode_poll_view3d;

  ot->flag = OPTYPE_REGISTER;

  /* Properties. */
  WM_operator_properties_gesture_straightline(ot, WM_CURSOR_EDIT);
  gesture::operator_properties(ot);
}

}  // namespace blender::ed::sculpt_paint::mask
