#!/usr/bin/env python3
"""Translates the renderer's HLSL (embedded in the C++ sources) to SPIR-V for SDL's Vulkan backend.

Used when the engine is built with OC4_SPIRV_SHADERS and run with OC4_GPU=vulkan (MoltenVK on macOS,
Windows, and the base for Linux and other Vulkan platforms). Written to src/render/SpirvShaders.inc,
which is checked in: building the game needs none of these tools.
Run this after changing a shader:  python3 tools/shaders/gen_spirv.py
With --check nothing is written: it fails when the checked-in file is not what the tools give now.

SDL's Vulkan backend (SDL_gpu.h) wants combined image samplers: sampler slot n binds texture n with
sampler n. The HLSL keeps them apart, and some textures share another slot's sampler (the leader maps
use baseSampler) or use none (Load). In every case the engine binds, in the texture's own slot, the
same sampler object the shader reads it with. So, for this translation only:

  1. each Sample* call reads its texture with the sampler of the texture's own register, declared
     where the HLSL has none (the engine already binds that sampler there);
  2. HLSL -> SPIR-V with glslang, as gen_msl.py;
  3. spirv-opt --convert-to-sampled-image merges each texture with the sampler at the same set and
     binding (textures read only with Load become sampled images on their own);
  4. the result is validated for Vulkan and checked against SDL's layout before it is written.

Descriptor sets and uniform buffers come from the HLSL registers already (space0/1 vertex, 2/3 fragment).
"""
import json
import re
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_msl  # noqa: E402  (the sources and entry points are shared with the Metal translation)

ROOT = gen_msl.ROOT
OUT = ROOT / "src/render/SpirvShaders.inc"
run = gen_msl.run


def reflect(spv):
    return json.loads(run("spirv-cross", spv, "--reflect"))


def own_slot_samplers(hlsl):
    """Each texture is sampled with the sampler declared at its own register (added if missing)."""
    textures = {m.group(1): (m.group(2), m.group(3)) for m in
                re.finditer(r"^\s*Texture\w*(?:<[^>]*>)?\s+(\w+)\s*:\s*register\(t(\d+),\s*(space\d+)\)", hlsl, re.M)}
    samplers = {(m.group(2), m.group(3)): m.group(1) for m in
                re.finditer(r"^\s*SamplerState\s+(\w+)\s*:\s*register\(s(\d+),\s*(space\d+)\)", hlsl, re.M)}
    added = []

    def sampler_for(texture):
        slot = textures[texture]
        if slot not in samplers:
            samplers[slot] = f"oc4Slot{slot[0]}_{slot[1]}"
            added.append(f"SamplerState {samplers[slot]} : register(s{slot[0]}, {slot[1]});\n")
        return samplers[slot]

    hlsl = re.sub(r"\b(\w+)\.(Sample\w*)\(\s*(\w+)\s*,",
                  lambda m: f"{m.group(1)}.{m.group(2)}({sampler_for(m.group(1)) if m.group(1) in textures else m.group(3)},",
                  hlsl)
    # (the new declarations go after the last resource declaration, before any function uses them)
    last = max(m.end() for m in re.finditer(r"^\s*(?:Texture\w*|SamplerState|ByteAddressBuffer|\w*StructuredBuffer)\b[^\n]*\n", hlsl, re.M))
    return hlsl[:last] + "".join(added) + hlsl[last:]


def translate(tmp, file, entry, stage):
    textures, uniforms = (0, 1) if stage == "vert" else (2, 3)
    original = tmp / f"{entry}.orig.spv"
    hlsl = tmp / f"{entry}.orig.hlsl"
    hlsl.write_text(gen_msl.SOURCES[file], encoding="utf-8")
    space = "space1" if stage == "vert" else "space3"
    compile_args = ["glslang", "-D", "-V", "--hlsl-iomap", "--hlsl-offsets", f"-DUNIFORMS={space}", "-S", stage,
                    "-e", entry, "--source-entrypoint", entry]
    run(*compile_args, "-o", original, hlsl)

    paired = tmp / f"{entry}.hlsl"
    # HLSL permits uint3 at byte 52; Vulkan's standard uniform layout requires
    # 16-byte vector alignment. Scalar padding preserves the host's byte layout.
    source = gen_msl.SOURCES[file].replace("uint3 terrainPad;", "uint terrainPad0; uint terrainPad1; uint terrainPad2;")
    paired.write_text(own_slot_samplers(source), encoding="utf-8")
    separate = tmp / f"{entry}.separate.spv"
    run(*compile_args, "-o", separate, paired)
    images = sorted({(i["set"], i["binding"]) for i in reflect(separate).get("separate_images", [])})
    spv = tmp / f"{entry}.spv"
    if images:
        run("spirv-opt", "--convert-to-sampled-image=" + " ".join(f"{s}:{b}" for s, b in images), "--eliminate-dead-code-aggressive", "--remove-unused-interface-variables", "--eliminate-dead-variables", "-o", spv, separate)
    else:
        spv.write_bytes(separate.read_bytes())
    spv.write_bytes(ordered_variables(spv.read_bytes()))
    run("spirv-val", "--target-env", "vulkan1.0", spv)
    check(entry, textures, uniforms, reflect(original), reflect(spv))
    return spv.read_bytes()


def ordered_variables(spv):
    """Global variables declared one after another, put in the order of their ids.

    spirv-opt writes the ones it converted in an order that differs between its builds (macOS and
    Linux swap two); they do not depend on each other, so this only makes the file reproducible.
    """
    words = [int.from_bytes(spv[i:i + 4], "little") for i in range(0, len(spv), 4)]
    out, at, group = words[:5], 5, []
    OP_VARIABLE, OP_FUNCTION = 59, 54
    while at < len(words):
        count, op = words[at] >> 16, words[at] & 0xFFFF
        if op == OP_FUNCTION:
            break
        instruction = words[at:at + count]
        if op == OP_VARIABLE and count == 4:  # (no initializer: nothing it could depend on but its type)
            group.append(instruction)
        else:
            out += [w for i in sorted(group, key=lambda i: i[2]) for w in i] + instruction
            group = []
        at += count
    out += [w for i in sorted(group, key=lambda i: i[2]) for w in i] + words[at:]
    return b"".join(w.to_bytes(4, "little") for w in out)


def check(entry, textures, uniforms, before, after):
    """The Vulkan shader binds what the HLSL bound, where SDL looks for it."""
    images = sorted(i["binding"] for i in before.get("separate_images", []))
    combined = after.get("textures", [])
    if sorted(t["binding"] for t in combined) != images or any(t["set"] != textures for t in combined):
        sys.exit(f"{entry}: combined samplers {combined} do not match the textures at {images}")
    if after.get("separate_images") or after.get("separate_samplers"):
        sys.exit(f"{entry}: separate images or samplers left: {after.get('separate_images')} {after.get('separate_samplers')}")
    storage = after.get("ssbos", [])
    if [(b["set"], b["binding"]) for b in storage] != [(textures, len(images) + i) for i in range(len(storage))]:
        sys.exit(f"{entry}: storage buffers {storage} are not right after the {len(images)} samplers")
    ubo = lambda r: sorted((u["name"], u["set"], u["binding"], u.get("block_size")) for u in r.get("ubos", []))
    if ubo(after) != ubo(before) or any(u["set"] != uniforms for u in after.get("ubos", [])):
        sys.exit(f"{entry}: uniform buffers {after.get('ubos')} differ from {before.get('ubos')}")
    loc = lambda r, k: sorted(v["location"] for v in r.get(k, []))
    if loc(after, "inputs") != loc(before, "inputs") or loc(after, "outputs") != loc(before, "outputs"):
        sys.exit(f"{entry}: stage inputs or outputs moved")


def main():
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        out = ["// Generated by tools/shaders/gen_spirv.py from the renderer's HLSL. Do not edit.\n"
               "// (included in Renderer.cpp after the SpirvShader struct)\n"]
        table = []
        for file, entry, stage, _ in gen_msl.ENTRIES:
            spv = translate(tmp, file, entry, stage)
            words = [int.from_bytes(spv[i:i + 4], "little") for i in range(0, len(spv), 4)]
            print(file, entry, len(spv), "bytes")
            name = f"kSpirv_{entry}"
            body = ",".join(f"0x{w:08x}u" for w in words)
            out.append(f"const uint32_t {name}[] = {{{body}}};\n")
            table.append(f'\t{{ "{file}", "{entry}", {name}, sizeof({name}) }},\n')
        out.append("const SpirvShader kSpirvShaders[] = {\n" + "".join(table) + "};\n")
    gen_msl.finish(OUT, "".join(out))


if __name__ == "__main__":
    main()
