/*
 * XGL
 *
 * Copyright (C) 2014 LunarG, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "genhw/genhw.h"
#include "dset.h"
#include "img.h"
#include "mem.h"
#include "pipeline.h"
#include "state.h"
#include "view.h"
#include "cmd_priv.h"

static void gen6_3DPRIMITIVE(struct intel_cmd *cmd,
                             int prim_type, bool indexed,
                             uint32_t vertex_count,
                             uint32_t vertex_start,
                             uint32_t instance_count,
                             uint32_t instance_start,
                             uint32_t vertex_base)
{
    const uint8_t cmd_len = 6;
    uint32_t dw0;

    CMD_ASSERT(cmd, 6, 6);

    dw0 = GEN_RENDER_CMD(3D, GEN6, 3DPRIMITIVE) |
          prim_type << GEN6_3DPRIM_DW0_TYPE__SHIFT |
          (cmd_len - 2);

    if (indexed)
        dw0 |= GEN6_3DPRIM_DW0_ACCESS_RANDOM;

    cmd_batch_reserve(cmd, cmd_len);
    cmd_batch_write(cmd, dw0);
    cmd_batch_write(cmd, vertex_count);
    cmd_batch_write(cmd, vertex_start);
    cmd_batch_write(cmd, instance_count);
    cmd_batch_write(cmd, instance_start);
    cmd_batch_write(cmd, vertex_base);
}

static void gen7_3DPRIMITIVE(struct intel_cmd *cmd,
                             int prim_type, bool indexed,
                             uint32_t vertex_count,
                             uint32_t vertex_start,
                             uint32_t instance_count,
                             uint32_t instance_start,
                             uint32_t vertex_base)
{
    const uint8_t cmd_len = 7;
    uint32_t dw0, dw1;

    CMD_ASSERT(cmd, 7, 7.5);

    dw0 = GEN_RENDER_CMD(3D, GEN6, 3DPRIMITIVE) | (cmd_len - 2);
    dw1 = prim_type << GEN7_3DPRIM_DW1_TYPE__SHIFT;

    if (indexed)
        dw1 |= GEN7_3DPRIM_DW1_ACCESS_RANDOM;

    cmd_batch_reserve(cmd, cmd_len);
    cmd_batch_write(cmd, dw0);
    cmd_batch_write(cmd, dw1);
    cmd_batch_write(cmd, vertex_count);
    cmd_batch_write(cmd, vertex_start);
    cmd_batch_write(cmd, instance_count);
    cmd_batch_write(cmd, instance_start);
    cmd_batch_write(cmd, vertex_base);
}

static bool gen6_can_primitive_restart(const struct intel_cmd *cmd)
{
    const struct intel_pipeline *p = cmd->bind.pipeline.graphics;
    bool supported;

    CMD_ASSERT(cmd, 6, 7.5);

    if (cmd_gen(cmd) >= INTEL_GEN(7.5))
        return (p->prim_type != GEN6_3DPRIM_RECTLIST);

    switch (p->prim_type) {
    case GEN6_3DPRIM_POINTLIST:
    case GEN6_3DPRIM_LINELIST:
    case GEN6_3DPRIM_LINESTRIP:
    case GEN6_3DPRIM_TRILIST:
    case GEN6_3DPRIM_TRISTRIP:
        supported = true;
        break;
    default:
        supported = false;
        break;
    }

    if (!supported)
        return false;

    switch (cmd->bind.index.type) {
    case XGL_INDEX_8:
        supported = (p->primitive_restart_index != 0xffu);
        break;
    case XGL_INDEX_16:
        supported = (p->primitive_restart_index != 0xffffu);
        break;
    case XGL_INDEX_32:
        supported = (p->primitive_restart_index != 0xffffffffu);
        break;
    default:
        supported = false;
        break;
    }

    return supported;
}

static void gen6_3DSTATE_INDEX_BUFFER(struct intel_cmd *cmd,
                                      const struct intel_mem *mem,
                                      XGL_GPU_SIZE offset,
                                      XGL_INDEX_TYPE type,
                                      bool enable_cut_index)
{
    const uint8_t cmd_len = 3;
    uint32_t dw0, end_offset;
    unsigned offset_align;

    CMD_ASSERT(cmd, 6, 7.5);

    dw0 = GEN_RENDER_CMD(3D, GEN6, 3DSTATE_INDEX_BUFFER) | (cmd_len - 2);

    /* the bit is moved to 3DSTATE_VF */
    if (cmd_gen(cmd) >= INTEL_GEN(7.5))
        assert(!enable_cut_index);
    if (enable_cut_index)
        dw0 |= GEN6_IB_DW0_CUT_INDEX_ENABLE;

    switch (type) {
    case XGL_INDEX_8:
        dw0 |= GEN6_IB_DW0_FORMAT_BYTE;
        offset_align = 1;
        break;
    case XGL_INDEX_16:
        dw0 |= GEN6_IB_DW0_FORMAT_WORD;
        offset_align = 2;
        break;
    case XGL_INDEX_32:
        dw0 |= GEN6_IB_DW0_FORMAT_DWORD;
        offset_align = 4;
        break;
    default:
        cmd->result = XGL_ERROR_INVALID_VALUE;
        return;
        break;
    }

    if (offset % offset_align) {
        cmd->result = XGL_ERROR_INVALID_VALUE;
        return;
    }

    /* aligned and inclusive */
    end_offset = mem->size - (mem->size % offset_align) - 1;

    cmd_batch_reserve(cmd, cmd_len);
    cmd_batch_write(cmd, dw0);
    cmd_batch_reloc(cmd, offset, mem, INTEL_DOMAIN_VERTEX, 0);
    cmd_batch_reloc(cmd, end_offset, mem, INTEL_DOMAIN_VERTEX, 0);
}

static inline void
gen75_3DSTATE_VF(struct intel_cmd *cmd,
                 bool enable_cut_index,
                 uint32_t cut_index)
{
    const uint8_t cmd_len = 2;
    uint32_t dw0;

    CMD_ASSERT(cmd, 7.5, 7.5);

    dw0 = GEN_RENDER_CMD(3D, GEN75, 3DSTATE_VF) | (cmd_len - 2);
    if (enable_cut_index)
        dw0 |=  GEN75_VF_DW0_CUT_INDEX_ENABLE;

    cmd_batch_reserve(cmd, cmd_len);
    cmd_batch_write(cmd, dw0);
    cmd_batch_write(cmd, cut_index);
}

static void gen6_3DSTATE_DEPTH_BUFFER(struct intel_cmd *cmd,
                                      const struct intel_ds_view *view)
{
    const uint8_t cmd_len = 7;
    uint32_t dw0;

    CMD_ASSERT(cmd, 6, 7.5);

    dw0 = (cmd_gen(cmd) >= INTEL_GEN(7)) ?
        GEN_RENDER_CMD(3D, GEN7, 3DSTATE_DEPTH_BUFFER) :
        GEN_RENDER_CMD(3D, GEN6, 3DSTATE_DEPTH_BUFFER);
    dw0 |= (cmd_len - 2);

    cmd_batch_reserve(cmd, cmd_len);
    cmd_batch_write(cmd, dw0);
    cmd_batch_write(cmd, view->cmd[0]);
    if (view->img) {
        cmd_batch_reloc(cmd, view->cmd[1], view->img->obj.mem,
                        INTEL_DOMAIN_RENDER,
                        INTEL_DOMAIN_RENDER);
    } else {
        cmd_batch_write(cmd, 0);
    }
    cmd_batch_write(cmd, view->cmd[2]);
    cmd_batch_write(cmd, view->cmd[3]);
    cmd_batch_write(cmd, view->cmd[4]);
    cmd_batch_write(cmd, view->cmd[5]);
}

static void gen6_3DSTATE_STENCIL_BUFFER(struct intel_cmd *cmd,
                                        const struct intel_ds_view *view)
{
    const uint8_t cmd_len = 3;
    uint32_t dw0;

    CMD_ASSERT(cmd, 6, 7.5);

    dw0 = (cmd_gen(cmd) >= INTEL_GEN(7)) ?
        GEN_RENDER_CMD(3D, GEN7, 3DSTATE_STENCIL_BUFFER) :
        GEN_RENDER_CMD(3D, GEN6, 3DSTATE_STENCIL_BUFFER);
    dw0 |= (cmd_len - 2);

    cmd_batch_reserve(cmd, cmd_len);
    cmd_batch_write(cmd, dw0);
    cmd_batch_write(cmd, view->cmd[6]);
    if (view->img) {
        cmd_batch_reloc(cmd, view->cmd[7], view->img->obj.mem,
                        INTEL_DOMAIN_RENDER,
                        INTEL_DOMAIN_RENDER);
    } else {
        cmd_batch_write(cmd, 0);
    }
}

static void gen6_3DSTATE_HIER_DEPTH_BUFFER(struct intel_cmd *cmd,
                                           const struct intel_ds_view *view)
{
    const uint8_t cmd_len = 3;
    uint32_t dw0;

    CMD_ASSERT(cmd, 6, 7.5);

    dw0 = (cmd_gen(cmd) >= INTEL_GEN(7)) ?
        GEN_RENDER_CMD(3D, GEN7, 3DSTATE_HIER_DEPTH_BUFFER) :
        GEN_RENDER_CMD(3D, GEN6, 3DSTATE_HIER_DEPTH_BUFFER);
    dw0 |= (cmd_len - 2);

    cmd_batch_reserve(cmd, cmd_len);
    cmd_batch_write(cmd, dw0);
    cmd_batch_write(cmd, view->cmd[8]);
    if (view->img) {
        cmd_batch_reloc(cmd, view->cmd[9], view->img->obj.mem,
                        INTEL_DOMAIN_RENDER,
                        INTEL_DOMAIN_RENDER);
    } else {
        cmd_batch_write(cmd, 0);
    }
}

static void gen6_3DSTATE_CC_STATE_POINTERS(struct intel_cmd *cmd,
                                           XGL_UINT blend_pos,
                                           XGL_UINT ds_pos,
                                           XGL_UINT cc_pos)
{
    const uint8_t cmd_len = 4;
    uint32_t dw0;

    CMD_ASSERT(cmd, 6, 6);

    dw0 = GEN_RENDER_CMD(3D, GEN6, 3DSTATE_CC_STATE_POINTERS) |
          (cmd_len - 2);

    cmd_batch_reserve(cmd, cmd_len);
    cmd_batch_write(cmd, dw0);
    cmd_batch_write(cmd, (blend_pos << 2) | 1);
    cmd_batch_write(cmd, (ds_pos << 2) | 1);
    cmd_batch_write(cmd, (cc_pos << 2) | 1);
}

static void gen6_3DSTATE_VIEWPORT_STATE_POINTERS(struct intel_cmd *cmd,
                                                 XGL_UINT clip_pos,
                                                 XGL_UINT sf_pos,
                                                 XGL_UINT cc_pos)
{
    const uint8_t cmd_len = 4;
    uint32_t dw0;

    CMD_ASSERT(cmd, 6, 6);

    dw0 = GEN_RENDER_CMD(3D, GEN6, 3DSTATE_VIEWPORT_STATE_POINTERS) |
          GEN6_PTR_VP_DW0_CLIP_CHANGED |
          GEN6_PTR_VP_DW0_SF_CHANGED |
          GEN6_PTR_VP_DW0_CC_CHANGED |
          (cmd_len - 2);

    cmd_batch_reserve(cmd, cmd_len);
    cmd_batch_write(cmd, dw0);
    cmd_batch_write(cmd, clip_pos << 2);
    cmd_batch_write(cmd, sf_pos << 2);
    cmd_batch_write(cmd, cc_pos << 2);
}

static void gen6_3DSTATE_SCISSOR_STATE_POINTERS(struct intel_cmd *cmd,
                                                XGL_UINT scissor_pos)
{
    const uint8_t cmd_len = 2;
    uint32_t dw0;

    CMD_ASSERT(cmd, 6, 6);

    dw0 = GEN_RENDER_CMD(3D, GEN6, 3DSTATE_SCISSOR_STATE_POINTERS) |
          (cmd_len - 2);

    cmd_batch_reserve(cmd, cmd_len);
    cmd_batch_write(cmd, dw0);
    cmd_batch_write(cmd, scissor_pos << 2);
}

static void gen7_3dstate_pointer(struct intel_cmd *cmd,
                                 int subop, XGL_UINT pos)
{
    const uint8_t cmd_len = 2;
    const uint32_t dw0 = GEN6_RENDER_TYPE_RENDER |
                         GEN6_RENDER_SUBTYPE_3D |
                         subop | (cmd_len - 2);

    cmd_batch_reserve(cmd, cmd_len);
    cmd_batch_write(cmd, dw0);
    cmd_batch_write(cmd, pos << 2);
}

static XGL_UINT gen6_BLEND_STATE(struct intel_cmd *cmd,
                                 const struct intel_blend_state *state)
{
    const uint8_t cmd_align = GEN6_ALIGNMENT_BLEND_STATE;
    const uint8_t cmd_len = XGL_MAX_COLOR_ATTACHMENTS * 2;

    CMD_ASSERT(cmd, 6, 7.5);
    STATIC_ASSERT(ARRAY_SIZE(state->cmd) >= cmd_len);

    return cmd_state_copy(cmd, state->cmd, cmd_len, cmd_align);
}

static XGL_UINT gen6_DEPTH_STENCIL_STATE(struct intel_cmd *cmd,
                                         const struct intel_ds_state *state)
{
    const uint8_t cmd_align = GEN6_ALIGNMENT_DEPTH_STENCIL_STATE;
    const uint8_t cmd_len = 3;

    CMD_ASSERT(cmd, 6, 7.5);
    STATIC_ASSERT(ARRAY_SIZE(state->cmd) >= cmd_len);

    return cmd_state_copy(cmd, state->cmd, cmd_len, cmd_align);
}

static XGL_UINT gen6_COLOR_CALC_STATE(struct intel_cmd *cmd,
                                      uint32_t stencil_ref,
                                      const uint32_t blend_color[4])
{
    const uint8_t cmd_align = GEN6_ALIGNMENT_COLOR_CALC_STATE;
    const uint8_t cmd_len = 6;
    XGL_UINT pos;
    uint32_t *dw;

    CMD_ASSERT(cmd, 6, 7.5);

    dw = cmd_state_reserve(cmd, cmd_len, cmd_align, &pos);
    dw[0] = stencil_ref;
    dw[1] = 0;
    dw[2] = blend_color[0];
    dw[3] = blend_color[1];
    dw[4] = blend_color[2];
    dw[5] = blend_color[3];
    cmd_state_advance(cmd, cmd_len);

    return pos;
}

static void gen6_cc_states(struct intel_cmd *cmd)
{
    const struct intel_blend_state *blend = cmd->bind.state.blend;
    const struct intel_ds_state *ds = cmd->bind.state.ds;
    XGL_UINT blend_pos, ds_pos, cc_pos;
    uint32_t stencil_ref;
    uint32_t blend_color[4];

    CMD_ASSERT(cmd, 6, 6);

    if (blend) {
        blend_pos = gen6_BLEND_STATE(cmd, blend);
        memcpy(blend_color, blend->cmd_blend_color, sizeof(blend_color));
    } else {
        blend_pos = 0;
        memset(blend_color, 0, sizeof(blend_color));
    }

    if (ds) {
        ds_pos = gen6_DEPTH_STENCIL_STATE(cmd, ds);
        stencil_ref = ds->cmd_stencil_ref;
    } else {
        ds_pos = 0;
        stencil_ref = 0;
    }

    cc_pos = gen6_COLOR_CALC_STATE(cmd, stencil_ref, blend_color);

    gen6_3DSTATE_CC_STATE_POINTERS(cmd, blend_pos, ds_pos, cc_pos);
}

static void gen6_viewport_states(struct intel_cmd *cmd)
{
    const struct intel_viewport_state *viewport = cmd->bind.state.viewport;
    XGL_UINT pos;

    if (!viewport)
        return;

    pos = cmd_state_copy(cmd, viewport->cmd, viewport->cmd_len,
            viewport->cmd_align);

    gen6_3DSTATE_VIEWPORT_STATE_POINTERS(cmd,
            pos + viewport->cmd_clip_offset,
            pos,
            pos + viewport->cmd_cc_offset);

    pos = (viewport->scissor_enable) ?
        pos + viewport->cmd_scissor_rect_offset : 0;

    gen6_3DSTATE_SCISSOR_STATE_POINTERS(cmd, pos);
}

static void gen7_cc_states(struct intel_cmd *cmd)
{
    const struct intel_blend_state *blend = cmd->bind.state.blend;
    const struct intel_ds_state *ds = cmd->bind.state.ds;
    uint32_t stencil_ref;
    uint32_t blend_color[4];
    XGL_UINT pos;

    CMD_ASSERT(cmd, 7, 7.5);

    if (!blend && !ds)
        return;

    if (blend) {
        pos = gen6_BLEND_STATE(cmd, blend);
        gen7_3dstate_pointer(cmd,
                GEN7_RENDER_OPCODE_3DSTATE_BLEND_STATE_POINTERS, pos);

        memcpy(blend_color, blend->cmd_blend_color, sizeof(blend_color));
    } else {
        memset(blend_color, 0, sizeof(blend_color));
    }

    if (ds) {
        pos = gen6_DEPTH_STENCIL_STATE(cmd, ds);
        gen7_3dstate_pointer(cmd,
                GEN7_RENDER_OPCODE_3DSTATE_DEPTH_STENCIL_STATE_POINTERS, pos);
    } else {
        stencil_ref = 0;
    }

    pos = gen6_COLOR_CALC_STATE(cmd, stencil_ref, blend_color);
    gen7_3dstate_pointer(cmd,
            GEN6_RENDER_OPCODE_3DSTATE_CC_STATE_POINTERS, pos);
}

static void gen7_viewport_states(struct intel_cmd *cmd)
{
    const struct intel_viewport_state *viewport = cmd->bind.state.viewport;
    XGL_UINT pos;

    if (!viewport)
        return;

    pos = cmd_state_copy(cmd, viewport->cmd, viewport->cmd_len,
            viewport->cmd_align);

    gen7_3dstate_pointer(cmd,
            GEN7_RENDER_OPCODE_3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP, pos);
    gen7_3dstate_pointer(cmd,
            GEN7_RENDER_OPCODE_3DSTATE_VIEWPORT_STATE_POINTERS_CC,
            pos + viewport->cmd_cc_offset);
    if (viewport->scissor_enable) {
        gen7_3dstate_pointer(cmd,
                GEN6_RENDER_OPCODE_3DSTATE_SCISSOR_STATE_POINTERS,
                pos + viewport->cmd_scissor_rect_offset);
    }
}

static void emit_bounded_states(struct intel_cmd *cmd)
{
    const struct intel_msaa_state *msaa = cmd->bind.state.msaa;

    /* TODO more states */

    if (cmd_gen(cmd) >= INTEL_GEN(7)) {
        gen7_cc_states(cmd);
        gen7_viewport_states(cmd);
    } else {
        gen6_cc_states(cmd);
        gen6_viewport_states(cmd);
    }

    /* 3DSTATE_MULTISAMPLE and 3DSTATE_SAMPLE_MASK */
    cmd_batch_reserve(cmd, msaa->cmd_len);
    cmd_batch_write_n(cmd, msaa->cmd, msaa->cmd_len);
}

XGL_VOID XGLAPI intelCmdBindPipeline(
    XGL_CMD_BUFFER                              cmdBuffer,
    XGL_PIPELINE_BIND_POINT                     pipelineBindPoint,
    XGL_PIPELINE                                pipeline)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);

    switch (pipelineBindPoint) {
    case XGL_PIPELINE_BIND_POINT_COMPUTE:
        cmd->bind.pipeline.compute = intel_pipeline(pipeline);
        break;
    case XGL_PIPELINE_BIND_POINT_GRAPHICS:
        cmd->bind.pipeline.graphics = intel_pipeline(pipeline);
        break;
    default:
        break;
    }
}

XGL_VOID XGLAPI intelCmdBindPipelineDelta(
    XGL_CMD_BUFFER                              cmdBuffer,
    XGL_PIPELINE_BIND_POINT                     pipelineBindPoint,
    XGL_PIPELINE_DELTA                          delta)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);

    switch (pipelineBindPoint) {
    case XGL_PIPELINE_BIND_POINT_COMPUTE:
        cmd->bind.pipeline.compute_delta = delta;
        break;
    case XGL_PIPELINE_BIND_POINT_GRAPHICS:
        cmd->bind.pipeline.graphics_delta = delta;
        break;
    default:
        break;
    }
}

XGL_VOID XGLAPI intelCmdBindStateObject(
    XGL_CMD_BUFFER                              cmdBuffer,
    XGL_STATE_BIND_POINT                        stateBindPoint,
    XGL_STATE_OBJECT                            state)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);

    switch (stateBindPoint) {
    case XGL_STATE_BIND_VIEWPORT:
        cmd->bind.state.viewport =
            intel_viewport_state((XGL_VIEWPORT_STATE_OBJECT) state);
        break;
    case XGL_STATE_BIND_RASTER:
        cmd->bind.state.raster =
            intel_raster_state((XGL_RASTER_STATE_OBJECT) state);
        break;
    case XGL_STATE_BIND_DEPTH_STENCIL:
        cmd->bind.state.ds =
            intel_ds_state((XGL_DEPTH_STENCIL_STATE_OBJECT) state);
        break;
    case XGL_STATE_BIND_COLOR_BLEND:
        cmd->bind.state.blend =
            intel_blend_state((XGL_COLOR_BLEND_STATE_OBJECT) state);
        break;
    case XGL_STATE_BIND_MSAA:
        cmd->bind.state.msaa =
            intel_msaa_state((XGL_MSAA_STATE_OBJECT) state);
        break;
    default:
        break;
    }
}

XGL_VOID XGLAPI intelCmdBindDescriptorSet(
    XGL_CMD_BUFFER                              cmdBuffer,
    XGL_PIPELINE_BIND_POINT                     pipelineBindPoint,
    XGL_UINT                                    index,
    XGL_DESCRIPTOR_SET                          descriptorSet,
    XGL_UINT                                    slotOffset)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);
    struct intel_dset *dset = intel_dset(descriptorSet);

    assert(!index);

    switch (pipelineBindPoint) {
    case XGL_PIPELINE_BIND_POINT_COMPUTE:
        cmd->bind.dset.compute = dset;
        cmd->bind.dset.compute_offset = slotOffset;
        break;
    case XGL_PIPELINE_BIND_POINT_GRAPHICS:
        cmd->bind.dset.graphics = dset;
        cmd->bind.dset.graphics_offset = slotOffset;
        break;
    default:
        break;
    }
}

XGL_VOID XGLAPI intelCmdBindDynamicMemoryView(
    XGL_CMD_BUFFER                              cmdBuffer,
    XGL_PIPELINE_BIND_POINT                     pipelineBindPoint,
    const XGL_MEMORY_VIEW_ATTACH_INFO*          pMemView)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);

    switch (pipelineBindPoint) {
    case XGL_PIPELINE_BIND_POINT_COMPUTE:
        intel_mem_view_init(&cmd->bind.mem_view.compute, cmd->dev, pMemView);
        break;
    case XGL_PIPELINE_BIND_POINT_GRAPHICS:
        intel_mem_view_init(&cmd->bind.mem_view.graphics, cmd->dev, pMemView);
        break;
    default:
        break;
    }
}

XGL_VOID XGLAPI intelCmdBindIndexData(
    XGL_CMD_BUFFER                              cmdBuffer,
    XGL_GPU_MEMORY                              mem_,
    XGL_GPU_SIZE                                offset,
    XGL_INDEX_TYPE                              indexType)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);
    struct intel_mem *mem = intel_mem(mem_);

    if (cmd_gen(cmd) >= INTEL_GEN(7.5)) {
        gen6_3DSTATE_INDEX_BUFFER(cmd, mem, offset, indexType, false);
    } else {
        cmd->bind.index.mem = mem;
        cmd->bind.index.offset = offset;
        cmd->bind.index.type = indexType;
    }
}

XGL_VOID XGLAPI intelCmdBindAttachments(
    XGL_CMD_BUFFER                              cmdBuffer,
    XGL_UINT                                    colorAttachmentCount,
    const XGL_COLOR_ATTACHMENT_BIND_INFO*       pColorAttachments,
    const XGL_DEPTH_STENCIL_BIND_INFO*          pDepthStencilAttachment)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);
    const struct intel_ds_view *ds;
    XGL_UINT i;

    for (i = 0; i < colorAttachmentCount; i++) {
        const XGL_COLOR_ATTACHMENT_BIND_INFO *att = &pColorAttachments[i];
        struct intel_rt_view *rt = intel_rt_view(att->view);

        cmd->bind.att.rt[i] = rt;
    }

    cmd->bind.att.rt_count = colorAttachmentCount;

    if (pDepthStencilAttachment) {
        cmd->bind.att.ds = intel_ds_view(pDepthStencilAttachment->view);
        ds = cmd->bind.att.ds;

    } else {
        /* all zeros */
        static const struct intel_ds_view null_ds;
        ds = &null_ds;
    }

    /* TODO workarounds */
    gen6_3DSTATE_DEPTH_BUFFER(cmd, ds);
    gen6_3DSTATE_STENCIL_BUFFER(cmd, ds);
    gen6_3DSTATE_HIER_DEPTH_BUFFER(cmd, ds);
}

XGL_VOID XGLAPI intelCmdDraw(
    XGL_CMD_BUFFER                              cmdBuffer,
    XGL_UINT                                    firstVertex,
    XGL_UINT                                    vertexCount,
    XGL_UINT                                    firstInstance,
    XGL_UINT                                    instanceCount)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);
    const struct intel_pipeline *p = cmd->bind.pipeline.graphics;

    emit_bounded_states(cmd);

    if (cmd_gen(cmd) >= INTEL_GEN(7)) {
        gen7_3DPRIMITIVE(cmd, p->prim_type, false, vertexCount,
                firstVertex, instanceCount, firstInstance, 0);
    } else {
        gen6_3DPRIMITIVE(cmd, p->prim_type, false, vertexCount,
                firstVertex, instanceCount, firstInstance, 0);
    }
}

XGL_VOID XGLAPI intelCmdDrawIndexed(
    XGL_CMD_BUFFER                              cmdBuffer,
    XGL_UINT                                    firstIndex,
    XGL_UINT                                    indexCount,
    XGL_INT                                     vertexOffset,
    XGL_UINT                                    firstInstance,
    XGL_UINT                                    instanceCount)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);
    const struct intel_pipeline *p = cmd->bind.pipeline.graphics;

    emit_bounded_states(cmd);

    if (p->primitive_restart && !gen6_can_primitive_restart(cmd))
        cmd->result = XGL_ERROR_UNKNOWN;

    if (cmd_gen(cmd) >= INTEL_GEN(7.5)) {
        gen75_3DSTATE_VF(cmd, p->primitive_restart,
                p->primitive_restart_index);
    } else {
        gen6_3DSTATE_INDEX_BUFFER(cmd, cmd->bind.index.mem,
                cmd->bind.index.offset, cmd->bind.index.type,
                p->primitive_restart);
    }

    if (cmd_gen(cmd) >= INTEL_GEN(7)) {
        gen7_3DPRIMITIVE(cmd, p->prim_type, true, indexCount,
                firstIndex, instanceCount, firstInstance, 0);
    } else {
        gen6_3DPRIMITIVE(cmd, p->prim_type, true, indexCount,
                firstIndex, instanceCount, firstInstance, 0);
    }
}

XGL_VOID XGLAPI intelCmdDrawIndirect(
    XGL_CMD_BUFFER                              cmdBuffer,
    XGL_GPU_MEMORY                              mem,
    XGL_GPU_SIZE                                offset,
    XGL_UINT32                                  count,
    XGL_UINT32                                  stride)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);

    cmd->result = XGL_ERROR_UNKNOWN;
}

XGL_VOID XGLAPI intelCmdDrawIndexedIndirect(
    XGL_CMD_BUFFER                              cmdBuffer,
    XGL_GPU_MEMORY                              mem,
    XGL_GPU_SIZE                                offset,
    XGL_UINT32                                  count,
    XGL_UINT32                                  stride)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);

    cmd->result = XGL_ERROR_UNKNOWN;
}

XGL_VOID XGLAPI intelCmdDispatch(
    XGL_CMD_BUFFER                              cmdBuffer,
    XGL_UINT                                    x,
    XGL_UINT                                    y,
    XGL_UINT                                    z)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);

    cmd->result = XGL_ERROR_UNKNOWN;
}

XGL_VOID XGLAPI intelCmdDispatchIndirect(
    XGL_CMD_BUFFER                              cmdBuffer,
    XGL_GPU_MEMORY                              mem,
    XGL_GPU_SIZE                                offset)
{
    struct intel_cmd *cmd = intel_cmd(cmdBuffer);

    cmd->result = XGL_ERROR_UNKNOWN;
}
