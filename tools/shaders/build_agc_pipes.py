#!/usr/bin/env python3
# From EVO Player for PS5 (tools/build_agc_pipes.py), GPL-3.0-or-later.
# Jelly5: builds app/shaders/*.pipe into app/engine/shaders/agc/<name>_pipe.h
# (the engine's own pipes are already built and are not touched). Run through
# tools/shaders/build.sh, which provides amdllpc in a container.
"""Compile EVO's AGC pipelines from .pipe files with AMD's LLPC (amdllpc).

Replaces tools/build_agc_shaders.py (GLSL -> glslangValidator -> opengnm-psbc ->
ps5-opengl's package writer). That chain was a hand-written reimplementation of
ps5-opengl's Gallium caller, and it reimplemented the caller without its
overrides, shipping two silent hardware-only bugs:

  * VGT_ESGS_RING_ITEMSIZE packaged as 4 rather than 1 for vertex-source NGG,
    which multiplies every vertex index by four - the GPU wedged on the first
    DCB containing a real draw and the process was killed ~55s later.
  * No --descriptor-binding for the VERTEX stage, so a shader declaring
    "layout(set=0, binding=0) uniform ScreenConstants" read its projection
    matrix from a descriptor slot nobody had described. Draws executed, retired
    and faulted nothing while producing exactly zero fragments.

Neither was detectable without hardware. In the .pipe format the user-data
layout is declared in [ResourceMapping] beside the shader source, so the
omission is not expressible; and the AGC register values are DERIVED from the
PAL metadata amdllpc emits rather than guessed at by a wrapper.

Chain (ported from ps5-xash3d-halflife's tools/build_shader.py and
tools/generate_agc_metadata.py, both published under that project's licence):

    amdllpc -gfxip=10.1.3 -o=X.pal.elf X.pipe
    llvm-objcopy --dump-section=.text=X.text.bin X.pal.elf
    llvm-readelf --symbols   -> _amdgpu_gs_main / _amdgpu_ps_main extents
    llvm-readelf --notes     -> AMDGPU Metadata YAML (PAL) -> AGC registers

Output is one C header per pipeline with the two ISA blobs and the derived
register tables, consumed by evo_agc_shader_header.c at runtime.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

try:
    import yaml
except ImportError as error:  # pragma: no cover
    raise SystemExit(
        "PyYAML is required to decode PAL metadata (apt: python3-yaml). "
        "Run this inside the amdllpc image: docker compose -f docker-compose.yml "
        "-f docker-compose.amdllpc.yml run --rm ps5-dev python3 tools/build_agc_pipes.py"
    ) from error

ROOT = Path(__file__).resolve().parents[2]
SHADER_DIR = ROOT / "app/shaders"                      # Jelly5's .pipe sources
HEADER_DIR = ROOT / "app/engine/shaders/agc"           # where the engine includes them
TARGET = "gfx1013"
GFXIP = "10.1.3"


class PalMetadataLoader(yaml.SafeLoader):
    """Safe YAML loader that understands LLVM's !str scalar spelling."""


PalMetadataLoader.add_constructor(
    "!str", lambda loader, node: loader.construct_scalar(node)
)


def run(arguments: list[str]) -> str:
    result = subprocess.run(arguments, cwd=ROOT, text=True, capture_output=True)
    if result.returncode != 0:
        raise SystemExit(
            f"command failed ({result.returncode}): {' '.join(arguments)}\n"
            f"{result.stdout}\n{result.stderr}"
        )
    return result.stdout


def symbol_extent(symbols: str, name: str) -> tuple[int, int]:
    match = re.search(
        rf"^\s*\d+:\s+([0-9a-fA-F]+)\s+(\d+)\s+FUNC.*\s{name}$",
        symbols,
        re.MULTILINE,
    )
    if match is None:
        raise SystemExit(f"missing PAL shader symbol: {name}")
    return int(match.group(1), 16), int(match.group(2))


def decode_pal_metadata(notes: str) -> dict:
    match = re.search(r"AMDGPU Metadata: ---\n(.*?)\n\.\.\.", notes, re.DOTALL)
    if match is None:
        raise SystemExit("PAL metadata YAML was not decoded")
    metadata = yaml.load(match.group(1), Loader=PalMetadataLoader)
    pipeline = metadata["amdpal.pipelines"][0]
    stages = pipeline[".hardware_stages"]

    def stage(key: str) -> dict:
        value = stages[key]
        return {
            "sgpr_count": value[".sgpr_count"],
            "user_sgprs": value[".user_sgprs"],
            "vgpr_count": value[".vgpr_count"],
            "wavefront_size": value[".wavefront_size"],
            "wgp_mode": value.get(".wgp_mode", False),
            "user_data_reg_map": value[".user_data_reg_map"],
        }

    return {
        "pipeline_type": pipeline[".type"],
        "hardware_stages": {"pre_raster_gs": stage(".gs"), "pixel": stage(".ps")},
        "graphics_register_metadata": pipeline[".graphics_registers"],
    }


# --------------------------------------------------------------------------
# PAL metadata -> AGC register tables.
# Ported from ps5-xash3d-halflife/tools/generate_agc_metadata.py.
# --------------------------------------------------------------------------

def bit(value: object) -> int:
    return int(bool(value))


def pack(values: list[int], width: int) -> int:
    mask = (1 << width) - 1
    return sum((value & mask) << (index * width) for index, value in enumerate(values))


# Which [ResourceMapping] descriptor a userDataNode is. LLPC numbers the plain
# (non-special) nodes per stage, in declaration order, so "const" and "texture"
# are separate indices per stage rather than the node's global index.
_VIS_VERTEX = 2
_VIS_FRAGMENT = 64


def resource_mapping_plan(pipe_text: str) -> dict:
    """Parse a .pipe's [ResourceMapping] section.

    Returns per-stage ordered lists of plain userDataNode indices and the node
    each descriptor kind maps to, so derive() can turn "which [ResourceMapping]
    entry holds the const buffer / texture" into "which user SGPR slot" via
    slot_of(stage_map, plain_index).

    LLPC only promotes DescriptorTableVaPtr nodes to plain user-data entries;
    IndirectUserDataVaPtr (the vertex-buffer table) gets a PAL special instead
    and is skipped here. Nodes are renumbered per stage in declaration order
    (visibility 2 = vertex stage, 64 = fragment stage), which is why the blur
    shader - with only fragment descriptors - ends up with its const buffer at
    plain 0 and its texture at plain 1 within the fragment group.
    """
    lines = pipe_text.splitlines()
    for i, line in enumerate(lines):
        if line.strip() == "[ResourceMapping]":
            section = "\n".join(lines[i + 1:])
            break
    else:
        return {"vs_const_plain": -1, "ps_const_plain": -1, "ps_texture_plain": -1}
    section = section.split("\n[", 1)[0]
    nodes: dict[int, dict[str, object]] = {}
    order: list[int] = []
    for line in section.splitlines():
        line = line.strip()
        if not line.startswith("userDataNode["):
            continue
        head, _, value = line.partition(" = ")
        match = re.match(r"userDataNode\[(\d+)\]\.(.+)", head)
        if match is None:
            raise SystemExit(f"unparseable [ResourceMapping] line: {line}")
        idx = int(match.group(1))
        key = match.group(2)
        if idx not in nodes:
            nodes[idx] = {}
            order.append(idx)
        nodes[idx][key] = value

    def plain_by_stage(visibility: int) -> list[tuple[int, str]]:
        plain = []
        for idx in order:
            node = nodes[idx]
            vis = int(node.get("visibility", "0"))
            typ = str(node.get("type", ""))
            if vis & visibility and typ == "DescriptorTableVaPtr":
                plain.append((idx, str(node.get("next[0].type", ""))))
        return plain

    def first_matching(plain: list[tuple[int, str]], kinds: tuple[str, ...]) -> int:
        for plain_index, (node_idx, kind) in enumerate(plain):
            if kind in kinds:
                return plain_index
        return -1

    vertex = plain_by_stage(_VIS_VERTEX)
    fragment = plain_by_stage(_VIS_FRAGMENT)
    return {
        # plain index [0..n] of the const-buffer table in each stage, or -1.
        "vs_const_plain": first_matching(vertex, ("DescriptorConstBuffer",)),
        "ps_const_plain": first_matching(fragment, ("DescriptorConstBuffer",)),
        # plain index of the first fragment texture table, or -1.
        "ps_texture_plain": first_matching(fragment, ("DescriptorCombinedTexture",)),
    }


def derive(manifest: dict, plan: dict) -> dict:
    if (manifest.get("target"), manifest.get("pipeline_type"),
            manifest.get("no_relocations")) != (TARGET, "Ngg", True):
        raise SystemExit(
            f"expected a relocation-free {TARGET} NGG pipeline, got "
            f"{manifest.get('target')}/{manifest.get('pipeline_type')}/"
            f"{manifest.get('no_relocations')}"
        )
    graphics = manifest["graphics_register_metadata"]
    stages = manifest["hardware_stages"]
    gs = stages["pre_raster_gs"]
    ps = stages["pixel"]

    user_map = {v for v in gs["user_data_reg_map"] if v != 0xFFFFFFFF}
    base_vertex = 0x10000003 in user_map
    base_instance = 0x10000004 in user_map
    draw_index = 0x10000005 in user_map
    draw_modifier = bit(base_vertex) | (bit(base_instance) << 2) | (bit(draw_index) << 3)

    # Which user SGPR slot holds what.
    #
    # .user_data_reg_map is indexed by user SGPR. An entry is either a PAL
    # "special" (0x1000xxxx, hardware- or driver-supplied) or a plain index into
    # the [ResourceMapping] user-data entries, which is what the runtime has to
    # write itself.
    #
    # For ui_screen_2d the vertex stage comes out as:
    #   slot 0 = 0x10000000 GlobalTable          (driver-supplied)
    #   slot 1 = 0          -> our const-buffer table
    #   slot 2 = 0x1000000f VertexBufferTable    (from IndirectUserDataVaPtr)
    #   slot 3 = 0x10000003 BaseVertex           (supplied by the draw packet)
    #   slot 4 = 0x10000004 BaseInstance         (supplied by the draw packet)
    #
    # Note the vertex-buffer table is a SPECIAL, not resource-mapping index 1:
    # IndirectUserDataVaPtr is hardware-managed, so looking for the node's own
    # index finds nothing. Deriving this rather than assuming it is the whole
    # point - the psbc path hardcoded these slots and a wrong one is invisible,
    # producing a shader that reads a resource through an unset pointer and so
    # draws nothing while faulting nothing.
    PAL_GLOBAL_TABLE = 0x10000000
    PAL_BASE_VERTEX = 0x10000003
    PAL_BASE_INSTANCE = 0x10000004
    PAL_DRAW_INDEX = 0x10000005
    PAL_VERTEX_BUFFER_TABLE = 0x1000000F

    def slot_of(reg_map: list[int], entry: int) -> int:
        for index, value in enumerate(reg_map):
            if value == entry:
                return index
        return -1

    gs_map = gs["user_data_reg_map"]

    ps_map = ps["user_data_reg_map"]

    onchip = graphics[".vgt_gs_onchip_cntl"]
    subgroup = graphics[".ge_ngg_subgrp_cntl"]
    stages_en = graphics[".vgt_shader_stages_en"]
    db = graphics[".db_shader_control"]
    bary = graphics[".spi_baryc_cntl"]

    def ps_inputs(fields: dict) -> int:
        names = (
            ".persp_sample_ena", ".persp_center_ena", ".persp_centroid_ena",
            ".persp_pull_model_ena", ".linear_sample_ena", ".linear_center_ena",
            ".linear_centroid_ena", ".line_stipple_tex_ena", ".pos_x_float_ena",
            ".pos_y_float_ena", ".pos_z_float_ena", ".pos_w_float_ena",
            ".front_face_ena", ".ancillary_ena", ".sample_coverage_ena",
            ".pos_fixed_pt_ena",
        )
        return sum(bit(fields.get(n, False)) << i for i, n in enumerate(names))

    pre_cx = [
        (0x1FF, graphics[".max_verts_per_subgroup"] & 0x3FF),
        (0x2D3, (subgroup[".prim_amp_factor"] & 0x1FF) |
                ((subgroup[".threads_per_subgroup"] & 0x1FF) << 9)),
        (0x207, 0),
        (0x1C2, graphics[".spi_shader_idx_format"] & 0xF),
        (0x1C3, pack(graphics[".spi_shader_pos_format"], 4)),
        # SPI_VS_OUT_CONFIG. VS_EXPORT_COUNT is bits [1:5] and holds
        # (parameter exports - 1); leaving it at zero tells the hardware the
        # vertex stage exports ONE parameter. A pipeline whose PS then reads
        # more interpolants than that gets a parameter-cache allocation too
        # small for what the VS writes, and the extra attributes come back as
        # garbage that varies per triangle - diagonal wedges across every quad.
        # Two varyings survived the omission; four did not.
        (0x1B1, ((graphics[".spi_vs_out_config"].get(".vs_export_count", 0) & 0x1F) << 1) |
                (bit(graphics[".spi_vs_out_config"].get(".no_pc_export")) << 7)),
        (0x2AB, graphics[".vgt_esgs_ring_itemsize"] & 0x7FFF),
        (0x2E4, 0),
        (0x2CE, graphics[".vgt_gs_max_vert_out"] & 0x7FF),
        (0x291, (onchip[".es_verts_per_subgroup"] & 0x7FF) |
                ((onchip[".gs_prims_per_subgroup"] & 0x7FF) << 11) |
                ((onchip[".gs_inst_prims_per_subgrp"] & 0x3FF) << 22)),
    ]
    db_value = (
        bit(db.get(".z_export_enable")) |
        (bit(db.get(".stencil_test_val_export_enable")) << 1) |
        ((db[".z_order"] & 3) << 4) |
        (bit(db.get(".kill_enable")) << 6) |
        (bit(db.get(".mask_export_enable")) << 8) |
        (bit(db.get(".exec_on_hier_fail")) << 9) |
        (bit(db.get(".exec_on_noop")) << 10) |
        (bit(db.get(".alpha_to_mask_disable")) << 11) |
        (bit(db.get(".depth_before_shader")) << 12) |
        ((db[".conservative_z_export"] & 3) << 13) |
        (bit(db.get(".primitive_ordered_pixel_shader")) << 16) |
        (bit(db.get(".pre_shader_depth_coverage_enable")) << 23)
    )
    pixel_cx = [
        (0x08F, pack(list(graphics[".cb_shader_mask"].values()), 4)),
        (0x203, db_value),
        (0x310, (graphics[".pa_sc_shader_control"][".wave_break_region_size"] & 3) << 5),
        (0x1B8, ((bit(bary[".pos_float_location"]) & 3) << 16) |
                (bit(bary[".front_face_all_bits"]) << 24)),
        (0x1B4, ps_inputs(graphics[".spi_ps_input_addr"])),
        (0x1B3, ps_inputs(graphics[".spi_ps_input_ena"])),
        (0x1B6, (graphics[".spi_ps_in_control"][".num_interps"] & 0x3F) |
                (bit(ps["wavefront_size"] == 32) << 15)),
        (0x1C5, pack(list(graphics[".spi_shader_col_format"].values()), 4)),
        (0x1C4, 0),
    ]


    # SPI_PS_INPUT_CNTL_0..N (0x191 + i): where each PS interpolant reads its
    # parameter from, and whether it is flat-shaded. Without these every input
    # defaults to offset 0 with interpolation on, so a `flat` varying is
    # interpolated from the wrong slot - garbage, not just a wrong colour.
    #
    # These do NOT go in pixel_cx. The shader-header arena is a fixed 0x148-byte
    # console ABI whose CX array holds exactly 9 pixel registers; a tenth makes
    # sceAgc reject the header and the whole renderer fails to initialise. They
    # are emitted with the pipeline's other bind-time context registers instead.
    ps_input_cntl = [
        (cntl.get(".offset", 0) & 0x3F) |
        ((cntl.get(".default_val", 0) & 3) << 8) |
        (bit(cntl.get(".flat_shade")) << 10) |
        (bit(cntl.get(".pt_sprite_tex")) << 17) |
        (bit(cntl.get(".fp16_interp_mode")) << 19) |
        ((cntl.get(".attr0_valid", 0) & 1) << 24) |
        ((cntl.get(".attr1_valid", 0) & 1) << 25)
        for cntl in graphics.get(".spi_ps_input_cntl", [])
    ]

    def rsrc1(stage: dict, wave32: bool, component: int, gs_stage: bool) -> int:
        vgprs = 0 if stage["vgpr_count"] == 0 else \
            (stage["vgpr_count"] - 1) // (8 if wave32 else 4)
        sgprs = (stage["sgpr_count"] - 1) // 8
        value = vgprs | (sgprs << 6) | (192 << 12) | (1 << 21) | (1 << 25)
        if gs_stage:
            value |= bit(stage.get("wgp_mode")) << 27 | ((component & 3) << 29)
        return value

    stage_word = (
        ((stages_en.get(".es_stage_en", 0) & 3) << 3) |
        (bit(stages_en.get(".gs_stage_en")) << 5) |
        ((stages_en.get(".vs_stage_en", 0) & 3) << 6) |
        (bit(stages_en.get(".primgen_en")) << 13) |
        ((stages_en.get(".max_primgroup_in_wave", 0) & 0xF) << 15) |
        (bit(stages_en.get(".gs_w32_en")) << 22) |
        (bit(stages_en.get(".vs_w32_en")) << 23) |
        (bit(stages_en.get(".primgen_passthru_en")) << 25)
    )
    return {
        "gs_rsrc1": rsrc1(gs, True, graphics[".gs_vgpr_comp_cnt"], True),
        "gs_rsrc2": ((gs["user_sgprs"] & 0x1F) << 1) |
                    ((graphics[".es_vgpr_comp_cnt"] & 3) << 16),
        "ps_rsrc1": rsrc1(ps, False, 0, False),
        "ps_rsrc2": (ps["user_sgprs"] & 0x1F) << 1,
        "ge_cntl": (onchip[".gs_prims_per_subgroup"] & 0x1FF) |
                   ((onchip[".es_verts_per_subgroup"] & 0x1FF) << 9),
        "shader_stages_en": stage_word,
        "gs_out_prim_type": 2,
        "draw_modifier": draw_modifier,
        "gs_user_sgprs": gs["user_sgprs"],
        "ps_user_sgprs": ps["user_sgprs"],
        "gs_user_data_reg_map": list(gs_map),
        "ps_user_data_reg_map": list(ps_map),
        # [ResourceMapping] plain node -> user SGPR slot, per stage. "const" and
        # "texture" can live in either stage; don't assume the vertex stage owns
        # the const-buffer table (ui_backdrop_blur keeps it in the fragment
        # stage alongside the texture).
        "vs_global_table_dword": slot_of(gs_map, PAL_GLOBAL_TABLE),
        "vs_const_table_dword": slot_of(gs_map, plan["vs_const_plain"])
        if plan["vs_const_plain"] >= 0 else -1,
        "vs_vertex_table_dword": slot_of(gs_map, PAL_VERTEX_BUFFER_TABLE),
        "ps_const_table_dword": slot_of(ps_map, plan["ps_const_plain"])
        if plan["ps_const_plain"] >= 0 else -1,
        "ps_texture_table_dword": slot_of(ps_map, plan["ps_texture_plain"])
        if plan["ps_texture_plain"] >= 0 else -1,
        # How many user-data dwords the runtime actually writes: everything up
        # to the last slot it owns. BaseVertex/BaseInstance sit above that and
        # are filled by the draw packet (see draw_modifier), never by us.
        "vs_write_count": max(slot_of(gs_map, plan["vs_const_plain"])
                              if plan["vs_const_plain"] >= 0 else -1,
                              slot_of(gs_map, PAL_VERTEX_BUFFER_TABLE), 0) + 1,
        "ps_write_count": max(slot_of(ps_map, plan["ps_const_plain"])
                              if plan["ps_const_plain"] >= 0 else -1,
                              slot_of(ps_map, plan["ps_texture_plain"])
                              if plan["ps_texture_plain"] >= 0 else -1,
                              0) + 1,
        "pre_raster_cx": pre_cx,
        "pixel_cx": pixel_cx,
        "ps_input_cntl": ps_input_cntl,
    }


def blob_rows(data: bytes) -> str:
    rows = []
    for i in range(0, len(data), 16):
        rows.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",")
    return "\n".join(rows)


def emit_header(name: str, gs: bytes, ps: bytes, values: dict) -> str:
    upper = name.upper()

    def regs(symbol: str, rows: list[tuple[int, int]]) -> str:
        body = ",\n".join(f"    {{{o:#05x}u, {v:#010x}u}}" for o, v in rows)
        return (f"static const evo_agc_reg_t {symbol}[] = {{\n{body}\n}};")

    return f"""/* {name} - generated by tools/build_agc_pipes.py from {name}.pipe. DO NOT EDIT.
 * Compiled with amdllpc -gfxip={GFXIP} ({TARGET}); register values derived from
 * the PAL metadata in the resulting ELF's AMDGPU Metadata note. */
#ifndef EVO_AGC_PIPE_{upper}_H
#define EVO_AGC_PIPE_{upper}_H

#include "evo_agc_shader_header.h"

#define {upper}_GS_ISA_BYTES {len(gs)}u
#define {upper}_PS_ISA_BYTES {len(ps)}u
#define {upper}_GS_RSRC1 {values['gs_rsrc1']:#010x}u
#define {upper}_GS_RSRC2 {values['gs_rsrc2']:#010x}u
#define {upper}_PS_RSRC1 {values['ps_rsrc1']:#010x}u
#define {upper}_PS_RSRC2 {values['ps_rsrc2']:#010x}u
#define {upper}_GE_CNTL {values['ge_cntl']:#010x}u
#define {upper}_SHADER_STAGES_EN {values['shader_stages_en']:#010x}u
#define {upper}_GS_OUT_PRIM_TYPE {values['gs_out_prim_type']:#010x}u
#define {upper}_DRAW_MODIFIER {values['draw_modifier']:#018x}ull
#define {upper}_GS_USER_SGPRS {values['gs_user_sgprs']}u
#define {upper}_PS_USER_SGPRS {values['ps_user_sgprs']}u
#define {upper}_VS_CONST_TABLE_DWORD {values['vs_const_table_dword']}
#define {upper}_VS_VERTEX_TABLE_DWORD {values['vs_vertex_table_dword']}
#define {upper}_PS_CONST_TABLE_DWORD {values['ps_const_table_dword']}
#define {upper}_PS_TEXTURE_TABLE_DWORD {values['ps_texture_table_dword']}
#define {upper}_VS_WRITE_COUNT {values['vs_write_count']}u
#define {upper}_PS_WRITE_COUNT {values['ps_write_count']}u

/* PAL user_data_reg_map, for cross-checking the user-SGPR layout the runtime
 * writes at SH 0x8c (vertex) and 0x0c (pixel):
 *   gs: {values['gs_user_data_reg_map']}
 *   ps: {values['ps_user_data_reg_map']}
 * 0x10000003 = base vertex, 0x10000004 = base instance, 0x10000005 = draw index;
 * small values are descriptor-table slots in [ResourceMapping] order. */

static const uint8_t {name}_gs_isa[{len(gs)}] __attribute__((aligned(256))) = {{
{blob_rows(gs)}
}};

static const uint8_t {name}_ps_isa[{len(ps)}] __attribute__((aligned(256))) = {{
{blob_rows(ps)}
}};

{regs(f"{name}_pre_raster_cx", values["pre_raster_cx"])}

{regs(f"{name}_pixel_cx", values["pixel_cx"])}

{regs(f"{name}_ps_input_cntl", [(0x191 + i, v) for i, v in enumerate(values["ps_input_cntl"])])}

static const evo_agc_shader_metadata_t {name}_metadata = {{
    .gs_isa = {name}_gs_isa,
    .gs_isa_bytes = {len(gs)}u,
    .ps_isa = {name}_ps_isa,
    .ps_isa_bytes = {len(ps)}u,
    .gs_rsrc1 = {upper}_GS_RSRC1,
    .gs_rsrc2 = {upper}_GS_RSRC2,
    .ps_rsrc1 = {upper}_PS_RSRC1,
    .ps_rsrc2 = {upper}_PS_RSRC2,
    .ge_cntl = {upper}_GE_CNTL,
    .shader_stages_en = {upper}_SHADER_STAGES_EN,
    .gs_out_prim_type = {upper}_GS_OUT_PRIM_TYPE,
    .draw_modifier = {upper}_DRAW_MODIFIER,
    .pre_raster_cx = {name}_pre_raster_cx,
    .pre_raster_cx_count = {len(values['pre_raster_cx'])}u,
    .pixel_cx = {name}_pixel_cx,
    .pixel_cx_count = {len(values['pixel_cx'])}u,
    .ps_input_cntl = {name}_ps_input_cntl,
    .ps_input_cntl_count = {len(values['ps_input_cntl'])}u,
    .vs_user_sgpr_count = {values['vs_write_count']}u,
    .ps_user_sgpr_count = {values['ps_write_count']}u,
    .vs_const_table_dword = {values['vs_const_table_dword']},
    .vs_vertex_table_dword = {values['vs_vertex_table_dword']},
    .ps_const_table_dword = {values['ps_const_table_dword']},
    .ps_texture_table_dword = {values['ps_texture_table_dword']},
}};

#endif /* EVO_AGC_PIPE_{upper}_H */
"""


def build_pipe(pipe: Path, out_dir: Path, amdllpc: str, readelf: str,
               objcopy: str) -> str:
    name = pipe.stem
    out_dir.mkdir(parents=True, exist_ok=True)
    elf = out_dir / f"{name}.pal.elf"
    text_path = out_dir / f"{name}.text.bin"

    run([amdllpc, f"-gfxip={GFXIP}", f"-o={elf}", str(pipe)])
    run([objcopy, f"--dump-section=.text={text_path}", str(elf)])
    header = run([readelf, "--file-header", str(elf)])
    sections = run([readelf, "--sections", "--elf-output-style=GNU", str(elf)])
    symbols = run([readelf, "--symbols", "--elf-output-style=GNU", str(elf)])
    notes = run([readelf, "--notes", "--elf-output-style=LLVM", str(elf)])

    if "AMDGPU" not in header or "0x42" not in header:
        raise SystemExit(f"[{name}] not the expected {TARGET} PAL ELF")
    if re.search(r"\]\s+\.rela?(?:\.|\s)", sections):
        raise SystemExit(f"[{name}] shader ELF contains unresolved relocations")

    text = text_path.read_bytes()
    blobs = {}
    for key, symbol in (("gs", "_amdgpu_gs_main"), ("ps", "_amdgpu_ps_main")):
        offset, size = symbol_extent(symbols, symbol)
        if size <= 0 or offset + size > len(text):
            raise SystemExit(f"[{name}] PAL symbol extent exceeds .text: {symbol}")
        blobs[key] = text[offset:offset + size]

    plan = resource_mapping_plan(pipe.read_text())
    manifest = {
        "name": name,
        "target": TARGET,
        "gfxip": GFXIP,
        "no_relocations": True,
        "source_sha256": hashlib.sha256(pipe.read_bytes()).hexdigest(),
        **decode_pal_metadata(notes),
    }
    (out_dir / f"{name}.manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    )
    values = derive(manifest, plan)
    (HEADER_DIR / f"{name}_pipe.h").write_text(
        emit_header(name, blobs["gs"], blobs["ps"], values)
    )
    # The vertex-buffer table is optional: the video pipelines build their quad
    # from gl_VertexIndex and declare no IndirectUserDataVaPtr, so -1 there is
    # correct rather than a missing binding. The constant-buffer and texture
    # tables are always required - a -1 for either means the shader would read
    # that resource through an unset pointer and silently draw nothing. The
    # const-buffer table may live in either stage (ui_screen_2d/video: vertex,
    # ui_backdrop_blur: fragment), so require a non-negative slot in at least
    # one of them.
    if (values["vs_const_table_dword"] < 0 and
            values["ps_const_table_dword"] < 0) or \
            values["ps_texture_table_dword"] < 0:
        raise SystemExit(
            f"[{name}] a required [ResourceMapping] node has no user SGPR slot: "
            f"const(vs)={values['vs_const_table_dword']} "
            f"const(ps)={values['ps_const_table_dword']} "
            f"texture={values['ps_texture_table_dword']}. The shader would read "
            f"that resource from an unset pointer."
        )
    print(f"  {name}: gs={len(blobs['gs'])}B ps={len(blobs['ps'])}B "
          f"esgs_itemsize={dict(values['pre_raster_cx'])[0x2AB]} "
          f"draw_modifier={values['draw_modifier']:#x} "
          f"user_dwords(const_vs={values['vs_const_table_dword']},"
          f"const_ps={values['ps_const_table_dword']},"
          f"vtx={values['vs_vertex_table_dword']},"
          f"tex={values['ps_texture_table_dword']}) "
          f"write(vs={values['vs_write_count']},ps={values['ps_write_count']})")
    return name


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--amdllpc", default="amdllpc")
    parser.add_argument("--readelf", default="llvm-readelf-18")
    parser.add_argument("--objcopy", default="llvm-objcopy-18")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/agc_pipes")
    parser.add_argument("pipes", nargs="*", type=Path)
    args = parser.parse_args()

    pipes = args.pipes or sorted(SHADER_DIR.glob("*.pipe"))
    if not pipes:
        raise SystemExit(f"no .pipe files found in {SHADER_DIR}")

    print(f"Building {len(pipes)} AGC pipeline(s) with amdllpc ({TARGET})...")
    names = [build_pipe(p, args.output_dir, args.amdllpc, args.readelf,
                        args.objcopy) for p in pipes]

    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
