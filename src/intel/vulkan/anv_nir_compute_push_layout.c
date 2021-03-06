/*
 * Copyright © 2019 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "anv_nir.h"
#include "compiler/brw_nir.h"
#include "util/mesa-sha1.h"

void
anv_nir_compute_push_layout(const struct anv_physical_device *pdevice,
                            nir_shader *nir,
                            struct brw_stage_prog_data *prog_data,
                            struct anv_pipeline_bind_map *map,
                            void *mem_ctx)
{
   memset(map->push_ranges, 0, sizeof(map->push_ranges));

   unsigned push_start = UINT_MAX, push_end = 0;
   nir_foreach_function(function, nir) {
      if (!function->impl)
         continue;

      nir_foreach_block(block, function->impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;

            nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
            if (intrin->intrinsic != nir_intrinsic_load_push_constant)
               continue;

            unsigned base = nir_intrinsic_base(intrin);
            unsigned range = nir_intrinsic_range(intrin);
            push_start = MIN2(push_start, base);
            push_end = MAX2(push_end, base + range);
         }
      }
   }

   const bool has_push_intrinsic = push_start <= push_end;

   if (nir->info.stage == MESA_SHADER_COMPUTE) {
      /* For compute shaders, we always have to have the subgroup ID.  The
       * back-end compiler will "helpfully" add it for us in the last push
       * constant slot.  Yes, there is an off-by-one error here but that's
       * because the back-end will add it so we want to claim the number of
       * push constants one dword less than the full amount including
       * gl_SubgroupId.
       */
      assert(push_end <= offsetof(struct anv_push_constants, cs.subgroup_id));
      push_end = offsetof(struct anv_push_constants, cs.subgroup_id);
   }

   /* Align push_start down to a 32B boundary and make it no larger than
    * push_end (no push constants is indicated by push_start = UINT_MAX).
    */
   push_start = MIN2(push_start, push_end);
   push_start &= ~31u;

   if (has_push_intrinsic) {
      nir_foreach_function(function, nir) {
         if (!function->impl)
            continue;

         nir_foreach_block(block, function->impl) {
            nir_foreach_instr(instr, block) {
               if (instr->type != nir_instr_type_intrinsic)
                  continue;

               nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
               if (intrin->intrinsic != nir_intrinsic_load_push_constant)
                  continue;

               intrin->intrinsic = nir_intrinsic_load_uniform;
               nir_intrinsic_set_base(intrin,
                                      nir_intrinsic_base(intrin) -
                                      push_start);
            }
         }
      }
   }

   /* For vec4 our push data size needs to be aligned to a vec4 and for
    * scalar, it needs to be aligned to a DWORD.
    */
   const unsigned align =
      pdevice->compiler->scalar_stage[nir->info.stage] ? 4 : 16;
   nir->num_uniforms = ALIGN(push_end - push_start, align);
   prog_data->nr_params = nir->num_uniforms / 4;
   prog_data->param = ralloc_array(mem_ctx, uint32_t, prog_data->nr_params);

   struct anv_push_range push_constant_range = {
      .set = ANV_DESCRIPTOR_SET_PUSH_CONSTANTS,
      .start = push_start / 32,
      .length = DIV_ROUND_UP(push_end - push_start, 32),
   };

   if ((pdevice->info.gen >= 8 || pdevice->info.is_haswell) &&
       nir->info.stage != MESA_SHADER_COMPUTE) {
      brw_nir_analyze_ubo_ranges(pdevice->compiler, nir, NULL,
                                 prog_data->ubo_ranges);

      /* We can push at most 64 registers worth of data.  The back-end
       * compiler would do this fixup for us but we'd like to calculate
       * the push constant layout ourselves.
       */
      unsigned total_push_regs = push_constant_range.length;
      for (unsigned i = 0; i < 4; i++) {
         if (total_push_regs + prog_data->ubo_ranges[i].length > 64)
            prog_data->ubo_ranges[i].length = 64 - total_push_regs;
         total_push_regs += prog_data->ubo_ranges[i].length;
      }
      assert(total_push_regs <= 64);

      /* The Skylake PRM contains the following restriction:
       *
       *    "The driver must ensure The following case does not occur
       *     without a flush to the 3D engine: 3DSTATE_CONSTANT_* with
       *     buffer 3 read length equal to zero committed followed by a
       *     3DSTATE_CONSTANT_* with buffer 0 read length not equal to
       *     zero committed."
       *
       * To avoid this, we program the buffers in the highest slots.
       * This way, slot 0 is only used if slot 3 is also used.
       */
      int n = 3;

      for (int i = 3; i >= 0; i--) {
         const struct brw_ubo_range *ubo_range = &prog_data->ubo_ranges[i];
         if (ubo_range->length == 0)
            continue;

         const struct anv_pipeline_binding *binding =
            &map->surface_to_descriptor[ubo_range->block];

         map->push_ranges[n--] = (struct anv_push_range) {
            .set = binding->set,
            .index = binding->index,
            .dynamic_offset_index = binding->dynamic_offset_index,
            .start = ubo_range->start,
            .length = ubo_range->length,
         };
      }

      if (push_constant_range.length > 0)
         map->push_ranges[n--] = push_constant_range;
   } else {
      /* For Ivy Bridge, the push constants packets have a different
       * rule that would require us to iterate in the other direction
       * and possibly mess around with dynamic state base address.
       * Don't bother; just emit regular push constants at n = 0.
       *
       * In the compute case, we don't have multiple push ranges so it's
       * better to just provide one in push_ranges[0].
       */
      map->push_ranges[0] = push_constant_range;
   }

   /* Now that we're done computing the push constant portion of the
    * bind map, hash it.  This lets us quickly determine if the actual
    * mapping has changed and not just a no-op pipeline change.
    */
   _mesa_sha1_compute(map->push_ranges,
                      sizeof(map->push_ranges),
                      map->push_sha1);
}

void
anv_nir_validate_push_layout(struct brw_stage_prog_data *prog_data,
                             struct anv_pipeline_bind_map *map)
{
#ifndef NDEBUG
   unsigned prog_data_push_size = DIV_ROUND_UP(prog_data->nr_params, 8);
   for (unsigned i = 0; i < 4; i++)
      prog_data_push_size += prog_data->ubo_ranges[i].length;

   unsigned bind_map_push_size = 0;
   for (unsigned i = 0; i < 4; i++)
      bind_map_push_size += map->push_ranges[i].length;

   /* We could go through everything again but it should be enough to assert
    * that they push the same number of registers.  This should alert us if
    * the back-end compiler decides to re-arrange stuff or shrink a range.
    */
   assert(prog_data_push_size == bind_map_push_size);
#endif
}
