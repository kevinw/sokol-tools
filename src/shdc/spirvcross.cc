/*
    translate SPIRV bytecode to shader sources and generate
    uniform block reflection information, wrapper around
    https://github.com/KhronosGroup/SPIRV-Cross
*/
#include "shdc.h"
#include "fmt/format.h"
#include "pystring.h"
#include "spirv_glsl.hpp"
#include "spirv_hlsl.hpp"
#include "spirv_msl.hpp"

/*
    for "Vulkan convention", fragment shader uniform block bindings live in the same
    descriptor set as vertex shader uniform blocks, but are offset by 4:

    set=0, binding=0..3:    vertex shader uniform blocks
    set=0, binding=4..7:    fragment shader uniform blocks
*/
static const uint32_t vk_fs_ub_binding_offset = 4;

using namespace spirv_cross;

namespace shdc {

int spirvcross_t::find_source_by_snippet_index(int snippet_index) const {
    for (int i = 0; i < (int)sources.size(); i++) {
        if (sources[i].snippet_index == snippet_index) {
            return i;
        }
    }
    return -1;
}

static void fix_ub_matrix_force_colmajor(Compiler& compiler) {
    /* go though all uniform block matrixes and decorate them with
        column-major, this is needed in the HLSL backend to fix the
        multiplication order
    */
    ShaderResources res = compiler.get_shader_resources();
    for (const Resource& ub_res: res.uniform_buffers) {
        const SPIRType& ub_type = compiler.get_type(ub_res.base_type_id);
        for (int m_index = 0; m_index < (int)ub_type.member_types.size(); m_index++) {
            const SPIRType& m_type = compiler.get_type(ub_type.member_types[m_index]);
            if ((m_type.basetype == SPIRType::Float) && (m_type.vecsize > 1) && (m_type.columns > 1)) {
                compiler.set_member_decoration(ub_res.base_type_id, m_index, spv::DecorationColMajor);
            }
        }
    }
}

static void fix_bind_slots(Compiler& compiler, snippet_t::type_t type, bool is_vulkan) {
    /*
        This overrides all bind slots like this:

        Target is not WebGPU:
            - both vertex shader and fragment shader:
                - uniform blocks go into set=0 and start at binding=0
                - images go into set=1 and start at binding=0
        Target is WebGPU:
            - uniform blocks go into set=0
                - vertex shader uniform blocks start at binding=0
                - fragment shader uniform blocks start at binding=1
            - vertex shader images go into set=1, start at binding=0
            - fragment shader images go into set=2, start at binding=0

        NOTE that any existing binding definitions are always overwritten,
        this differs from previous behaviour which checked if explicit
        bindings existed.
    */
    ShaderResources res = compiler.get_shader_resources();
    uint32_t ub_slot = 0;
    if (is_vulkan) {
        ub_slot = (type == snippet_t::type_t::VS) ? 0 : vk_fs_ub_binding_offset;
    }
    for (const Resource& ub_res: res.uniform_buffers) {
        compiler.set_decoration(ub_res.id, spv::DecorationDescriptorSet, 0);
        compiler.set_decoration(ub_res.id, spv::DecorationBinding, ub_slot++);
    }

    uint32_t img_slot = 0;
    uint32_t img_set = (type == snippet_t::type_t::VS) ? 1 : 2;
    for (const Resource& img_res: res.sampled_images) {
        compiler.set_decoration(img_res.id, spv::DecorationDescriptorSet, img_set);
        compiler.set_decoration(img_res.id, spv::DecorationBinding, img_slot++);
    }
}

static void flatten_uniform_blocks(CompilerGLSL& compiler) {
    /* this flattens each uniform block into a vec4 array, in WebGL/GLES2 this
        allows more efficient uniform updates
    */
    ShaderResources res = compiler.get_shader_resources();
    for (const Resource& ub_res: res.uniform_buffers) {
        compiler.flatten_buffer_block(ub_res.id);
    }
}

static uniform_t::type_t spirtype_to_uniform_type(const SPIRType& type) {
    if (type.basetype == SPIRType::Float) {
        if (type.columns == 1) {
            // scalar or vec
            switch (type.vecsize) {
                case 1: return uniform_t::FLOAT;
                case 2: return uniform_t::FLOAT2;
                case 3: return uniform_t::FLOAT3;
                case 4: return uniform_t::FLOAT4;
            }
        }
        else {
            // a matrix
            if ((type.vecsize == 4) && (type.columns == 4)) {
                return uniform_t::MAT4;
            }
        }
    }
    // fallthrough: invalid type
    return uniform_t::INVALID;
}

static image_t::type_t spirtype_to_image_type(const SPIRType& type) {
    if (type.image.arrayed) {
        if (type.image.dim == spv::Dim2D) {
            return image_t::IMAGE_TYPE_ARRAY;
        }
    }
    else {
        switch (type.image.dim) {
            case spv::Dim2D:    return image_t::IMAGE_TYPE_2D;
            case spv::DimCube:  return image_t::IMAGE_TYPE_CUBE;
            case spv::Dim3D:    return image_t::IMAGE_TYPE_3D;
            default: break;
        }
    }
    // fallthrough: invalid type
    return image_t::IMAGE_TYPE_INVALID;
}

static image_t::basetype_t spirtype_to_image_basetype(const SPIRType& type) {
    switch (type.basetype) {
        case SPIRType::Int:
        case SPIRType::Short:
        case SPIRType::SByte:
            return image_t::IMAGE_BASETYPE_SINT;
        case SPIRType::UInt:
        case SPIRType::UShort:
        case SPIRType::UByte:
            return image_t::IMAGE_BASETYPE_UINT;
        default:
            return image_t::IMAGE_BASETYPE_FLOAT;
    }
}

static spirvcross_refl_t parse_reflection(const Compiler& compiler, bool is_vulkan) {
    spirvcross_refl_t refl;
    ShaderResources shd_resources = compiler.get_shader_resources();
    // shader stage
    switch (compiler.get_execution_model()) {
        case spv::ExecutionModelVertex:   refl.stage = stage_t::VS; break;
        case spv::ExecutionModelFragment: refl.stage = stage_t::FS; break;
        default: refl.stage = stage_t::INVALID; break;
    }
    // find entry point
    const auto entry_points = compiler.get_entry_points_and_stages();
    for (const auto& item: entry_points) {
        if (compiler.get_execution_model() == item.execution_model) {
            refl.entry_point = item.name;
            break;
        }
    }
    // stage inputs and outputs
    for (const Resource& res_attr: shd_resources.stage_inputs) {
        attr_t refl_attr;
        refl_attr.slot = compiler.get_decoration(res_attr.id, spv::DecorationLocation);
        refl_attr.name = res_attr.name;
        refl_attr.sem_name = "TEXCOORD";
        refl_attr.sem_index = refl_attr.slot;
        refl.inputs[refl_attr.slot] = refl_attr;
    }
    for (const Resource& res_attr: shd_resources.stage_outputs) {
        attr_t refl_attr;
        refl_attr.slot = compiler.get_decoration(res_attr.id, spv::DecorationLocation);
        refl_attr.name = res_attr.name;
        refl_attr.sem_name = "TEXCOORD";
        refl_attr.sem_index = refl_attr.slot;
        refl.outputs[refl_attr.slot] = refl_attr;
    }
    // uniform blocks
    for (const Resource& ub_res: shd_resources.uniform_buffers) {
        uniform_block_t refl_ub;
        const SPIRType& ub_type = compiler.get_type(ub_res.base_type_id);
        refl_ub.slot = compiler.get_decoration(ub_res.id, spv::DecorationBinding);
        // shift fragment shader uniform blocks binding back to
        if (is_vulkan && (refl_ub.slot >= (int)vk_fs_ub_binding_offset)) {
            refl_ub.slot -= 4;
        }
        refl_ub.size = (int) compiler.get_declared_struct_size(ub_type);
        refl_ub.name = ub_res.name;
        for (int m_index = 0; m_index < (int)ub_type.member_types.size(); m_index++) {
            uniform_t refl_uniform;
            refl_uniform.name = compiler.get_member_name(ub_res.base_type_id, m_index);
            const SPIRType& m_type = compiler.get_type(ub_type.member_types[m_index]);
            refl_uniform.type = spirtype_to_uniform_type(m_type);
            if (m_type.array.size() > 0) {
                refl_uniform.array_count = m_type.array[0];
            }
            refl_uniform.offset = compiler.type_struct_member_offset(ub_type, m_index);
            refl_ub.uniforms.push_back(refl_uniform);
        }
        refl.uniform_blocks.push_back(refl_ub);
    }
    // images
    for (const Resource& img_res: shd_resources.sampled_images) {
        image_t refl_img;
        refl_img.slot = compiler.get_decoration(img_res.id, spv::DecorationBinding);
        refl_img.name = img_res.name;
        const SPIRType& img_type = compiler.get_type(img_res.type_id);
        refl_img.type = spirtype_to_image_type(img_type);
        refl_img.base_type = spirtype_to_image_basetype(compiler.get_type(img_type.image.type));
        refl.images.push_back(refl_img);
    }
    return refl;
}

static spirvcross_source_t to_glsl(const spirv_blob_t& blob, int glsl_version, bool is_gles, bool is_vulkan, uint32_t opt_mask, snippet_t::type_t type) {
    CompilerGLSL compiler(blob.bytecode);
    CompilerGLSL::Options options;
    options.emit_line_directives = false;
    options.version = glsl_version;
    options.es = is_gles;
    options.vulkan_semantics = is_vulkan;
    options.enable_420pack_extension = false;
    options.vertex.fixup_clipspace = (0 != (opt_mask & option_t::FIXUP_CLIPSPACE));
    options.vertex.flip_vert_y = (0 != (opt_mask & option_t::FLIP_VERT_Y));
    compiler.set_common_options(options);
    fix_bind_slots(compiler, type, is_vulkan);
    fix_ub_matrix_force_colmajor(compiler);
    if (!is_vulkan) {
        flatten_uniform_blocks(compiler);
    }
    std::string src = compiler.compile();
    spirvcross_source_t res;
    if (!src.empty()) {
        res.valid = true;
        res.source_code = std::move(src);
        res.refl = parse_reflection(compiler, is_vulkan);
    }
    return res;
}

static spirvcross_source_t to_hlsl5(const spirv_blob_t& blob, uint32_t opt_mask, snippet_t::type_t type) {
    CompilerHLSL compiler(blob.bytecode);
    CompilerGLSL::Options commonOptions;
    commonOptions.emit_line_directives = true;
    commonOptions.vertex.fixup_clipspace = (0 != (opt_mask & option_t::FIXUP_CLIPSPACE));
    commonOptions.vertex.flip_vert_y = (0 != (opt_mask & option_t::FLIP_VERT_Y));
    compiler.set_common_options(commonOptions);
    CompilerHLSL::Options hlslOptions;
    hlslOptions.shader_model = 50;
    hlslOptions.point_size_compat = true;
    compiler.set_hlsl_options(hlslOptions);
    fix_bind_slots(compiler, type, false);
    fix_ub_matrix_force_colmajor(compiler);
    std::string src = compiler.compile();
    spirvcross_source_t res;
    if (!src.empty()) {
        res.valid = true;
        res.source_code = std::move(src);
        res.refl = parse_reflection(compiler, false);
    }
    return res;
}

static spirvcross_source_t to_msl(const spirv_blob_t& blob, CompilerMSL::Options::Platform plat, uint32_t opt_mask, snippet_t::type_t type) {
    CompilerMSL compiler(blob.bytecode);
    CompilerGLSL::Options commonOptions;
    commonOptions.emit_line_directives = true;
    commonOptions.vertex.fixup_clipspace = (0 != (opt_mask & option_t::FIXUP_CLIPSPACE));
    commonOptions.vertex.flip_vert_y = (0 != (opt_mask & option_t::FLIP_VERT_Y));
    compiler.set_common_options(commonOptions);
    CompilerMSL::Options mslOptions;
    mslOptions.platform = plat;
    mslOptions.enable_decoration_binding = true;
    compiler.set_msl_options(mslOptions);
    fix_bind_slots(compiler, type, false);
    std::string src = compiler.compile();
    spirvcross_source_t res;
    if (!src.empty()) {
        res.valid = true;
        res.source_code = std::move(src);
        res.refl = parse_reflection(compiler, false);
        // Metal's entry point function are called main0() because main() is reserved
        res.refl.entry_point += "0";
    }
    return res;
}

static int find_unique_uniform_block_by_name(const spirvcross_t& spv_cross, const std::string& name) {
    for (int i = 0; i < (int)spv_cross.unique_uniform_blocks.size(); i++) {
        if (spv_cross.unique_uniform_blocks[i].name == name) {
            return i;
        }
    }
    return -1;
}

static int find_unique_image_by_name(const spirvcross_t& spv_cross, const std::string& name) {
    for (int i = 0; i < (int)spv_cross.unique_images.size(); i++) {
        if (spv_cross.unique_images[i].name == name) {
            return i;
        }
    }
    return -1;
}

// find all identical uniform blocks across all shaders, and check for collisions
static bool gather_unique_uniform_blocks(const input_t& inp, spirvcross_t& spv_cross) {
    for (spirvcross_source_t& src: spv_cross.sources) {
        for (uniform_block_t& ub: src.refl.uniform_blocks) {
            int other_ub_index = find_unique_uniform_block_by_name(spv_cross, ub.name);
            if (other_ub_index >= 0) {
                if (ub.equals(spv_cross.unique_uniform_blocks[other_ub_index])) {
                    // identical uniform block already exists, take note of the index
                    ub.unique_index = other_ub_index;
                }
                else {
                    spv_cross.error = errmsg_t::error(inp.base_path, 0, fmt::format("conflicting uniform block definitions found for '{}'", ub.name));
                    return false;
                }
            }
            else {
                // a new unique uniform block
                ub.unique_index = (int) spv_cross.unique_uniform_blocks.size();
                spv_cross.unique_uniform_blocks.push_back(ub);
            }
        }
    }
    return true;
}

// find all identical images across all shaders, and check for collisions
static bool gather_unique_images(const input_t& inp, spirvcross_t& spv_cross) {
    for (spirvcross_source_t& src: spv_cross.sources) {
        for (image_t& img: src.refl.images) {
            int other_img_index = find_unique_image_by_name(spv_cross, img.name);
            if (other_img_index >= 0) {
                if (img.equals(spv_cross.unique_images[other_img_index])) {
                    // identical image already exists, take note of the index
                    img.unique_index = other_img_index;
                }
                else {
                    spv_cross.error = errmsg_t::error(inp.base_path, 0, fmt::format("conflicting texture definitions found for '{}'", img.name));
                    return false;
                }
            }
            else {
                // new unique image
                img.unique_index = (int) spv_cross.unique_images.size();
                spv_cross.unique_images.push_back(img);
            }
        }
    }
    return true;
}

// check that the vertex shader outputs match the fragment shader inputs for each program
// FIXME: this should also check the attribute's type
static errmsg_t validate_linking(const input_t& inp, const spirvcross_t& spv_cross) {
    for (const auto& prog_item: inp.programs) {
        const program_t& prog = prog_item.second;
        int vs_snippet_index = inp.vs_map.at(prog.vs_name);
        int fs_snippet_index = inp.fs_map.at(prog.fs_name);
        int vs_src_index = spv_cross.find_source_by_snippet_index(vs_snippet_index);
        int fs_src_index = spv_cross.find_source_by_snippet_index(fs_snippet_index);
        assert((vs_src_index >= 0) && (fs_src_index >= 0));
        const spirvcross_source_t& vs_src = spv_cross.sources[vs_src_index];
        const spirvcross_source_t& fs_src = spv_cross.sources[fs_src_index];
        assert(vs_snippet_index == vs_src.snippet_index);
        assert(fs_snippet_index == fs_src.snippet_index);
        for (int i = 0; i < attr_t::NUM; i++) {
            const attr_t& vs_out = vs_src.refl.outputs[i];
            const attr_t& fs_inp = fs_src.refl.inputs[i];
            if (!vs_out.equals(fs_inp)) {
                return inp.error(prog.line_index,
                    fmt::format("outputs of vs '{}' don't match inputs of fs '{}' for attr #{} (vs={},fs={})\n",
                    prog.vs_name, prog.fs_name, i, vs_out.name, fs_inp.name));
            }
        }
    }
    return errmsg_t();
}

spirvcross_t spirvcross_t::translate(const input_t& inp, const spirv_t& spirv, slang_t::type_t slang) {
    spirvcross_t spv_cross;
    for (const auto& blob: spirv.blobs) {
        spirvcross_source_t src;
        uint32_t opt_mask = inp.snippets[blob.snippet_index].options[(int)slang];
        snippet_t::type_t type = inp.snippets[blob.snippet_index].type;
        assert((type == snippet_t::VS) || (type == snippet_t::FS));
        switch (slang) {
            case slang_t::GLSL330:
                src = to_glsl(blob, 330, false, false, opt_mask, type);
                break;
            case slang_t::GLSL100:
                src = to_glsl(blob, 100, true, false, opt_mask, type);
                break;
            case slang_t::GLSL300ES:
                src = to_glsl(blob, 300, true, false, opt_mask, type);
                break;
            case slang_t::HLSL5:
                src = to_hlsl5(blob, opt_mask, type);
                break;
            case slang_t::METAL_MACOS:
                src = to_msl(blob, CompilerMSL::Options::macOS, opt_mask, type);
                break;
            case slang_t::METAL_IOS:
            case slang_t::METAL_SIM:
                src = to_msl(blob, CompilerMSL::Options::iOS, opt_mask, type);
                break;
            case slang_t::WGPU:
                // hackety hack, just compile to GLSL even for SPIRV output
                // so that we can use the same SPIRV-Cross's reflection API
                // calls as for the other output types
                src = to_glsl(blob, 450, false, true, opt_mask, type);
                break;
            default: break;
        }
        if (src.valid) {
            src.snippet_index = blob.snippet_index;
            spv_cross.sources.push_back(std::move(src));
        }
        else {
            const int line_index = inp.snippets[blob.snippet_index].lines[0];
            std::string err_msg = fmt::format("Failed to cross-compile to {}.", slang_t::to_str((slang_t::type_t)slang));
            spv_cross.error = inp.error(line_index, err_msg);
            return spv_cross;
        }
        if (!gather_unique_uniform_blocks(inp, spv_cross)) {
            // error has been set in spv_cross.error
            return spv_cross;
        }
        if (!gather_unique_images(inp, spv_cross)) {
            // error has been set in spv_cross.error
            return spv_cross;
        }
    }
    // check that vertex-shader outputs match their fragment shader inputs
    errmsg_t err = validate_linking(inp, spv_cross);
    if (err.valid) {
        spv_cross.error = err;
        return spv_cross;
    }
    return spv_cross;
}

// bare binary format
static void write_string(FILE* stream, const std::string& str) {
    assert(str.size() < UINT16_MAX);
    uint16_t sz = (uint16_t)str.size();

    // write the length of the string as a u16
    fwrite(&sz, sizeof(uint16_t), 1, stream);

    // write the string's bytes
    fwrite(str.data(), str.size(), 1, stream);
}

static void write_byte(FILE* stream, unsigned char byte) {
    fwrite(&byte, sizeof(unsigned char), 1, stream);
}

static void write_u16(FILE* stream, uint16_t val) {
    fwrite(&val, sizeof(uint16_t), 1, stream);
}

static void write_attr(FILE* stream, const attr_t& attr) {
    write_string(stream, attr.name);
    write_u16(stream, attr.slot);
    write_string(stream, attr.sem_name);
    write_byte(stream, attr.sem_index);
}

static void write_uniform_block(FILE* stream, const uniform_block_t& ub) {
    write_u16(stream, ub.uniforms.size());
    for (const uniform_t& uniform: ub.uniforms) {
        write_string(stream, uniform.name);
        write_byte(stream, uniform.type);
        write_u16(stream, uniform.array_count);
        write_u16(stream, uniform.offset);
    }
}

static void write_image(FILE* stream, const image_t& img) {
    write_string(stream, img.name);
    write_u16(stream, img.slot);
    write_byte(stream, img.type);
    write_byte(stream, img.base_type);
}

static void write_chars(FILE* stream, const char* chars) {
    fwrite(chars, strlen(chars), 1, stream);
}

static const uint16_t BINARY_FORMAT_VERSION = 1;

void spirvcross_t::write_binary_reflection_info(FILE* stream, const spirvcross_refl_t& refl) const {
    write_chars(stream, "SHDC"); // magic header
    write_u16(stream, BINARY_FORMAT_VERSION);

    write_byte(stream, (unsigned char)refl.stage); // stage_t as a byte
    write_string(stream, refl.entry_point); // entry point as a (u16 len, string bytes) pair

    // inputs
    int count = 0;
    for (const attr_t& attr: refl.inputs) {
        if (attr.slot >= 0) {
            ++count;  
        }
    }

    write_u16(stream, count); // input count as u16
    for (const attr_t& attr: refl.inputs) {
        if (attr.slot >= 0) {
            write_attr(stream, attr);
        }
    }
    
    // outputs
    count = 0;
    for (const attr_t& attr: refl.outputs) {
        if (attr.slot >= 0) {
            ++count;
        }
    }
    
    write_u16(stream, count); // output count as u16
    for (const attr_t& attr: refl.outputs) {
        if (attr.slot >= 0) {
            write_attr(stream, attr);
        }
    }

    // uniform blocks
    write_u16(stream, refl.uniform_blocks.size());
    for (const uniform_block_t& ub: refl.uniform_blocks) {
        write_uniform_block(stream, ub);
    }

    // images
    write_u16(stream, refl.images.size());
    for (const image_t& img: refl.images) {
        write_image(stream, img);
    }
}

//

void spirvcross_t::write_reflection_info(FILE* stream, const spirvcross_source_t& source, const std::string& indent) const {
    fmt::print(stream, "{}stage: {}\n", indent, stage_t::to_str(source.refl.stage));
    fmt::print(stream, "{}entry: {}\n", indent, source.refl.entry_point);
    fmt::print(stream, "{}inputs:\n", indent);
    for (const attr_t& attr: source.refl.inputs) {
        if (attr.slot >= 0) {
            fmt::print(stream, "{}  {}: slot={}, sem_name={}, sem_index={}\n", indent, attr.name, attr.slot, attr.sem_name, attr.sem_index);
        }
    }
    fmt::print(stream, "{}outputs:\n", indent);
    for (const attr_t& attr: source.refl.outputs) {
        if (attr.slot >= 0) {
            fmt::print(stream, "{}  {}: slot={}, sem_name={}, sem_index={}\n", indent, attr.name, attr.slot, attr.sem_name, attr.sem_index);
        }
    }
    for (const uniform_block_t& ub: source.refl.uniform_blocks) {
        fmt::print(stream, "{}uniform block: {}, slot: {}, size: {}\n", indent, ub.name, ub.slot, ub.size);
        for (const uniform_t& uniform: ub.uniforms) {
            fmt::print(stream, "{}  member: {}, type: {}, array_count: {}, offset: {}\n",
                indent,
                uniform.name,
                uniform_t::type_to_str(uniform.type),
                uniform.array_count,
                uniform.offset);
        }
    }
    for (const image_t& img: source.refl.images) {
        fmt::print(stream, "{}image: {}, slot: {}, type: {}, basetype: {}\n",
            indent, img.name, img.slot, image_t::type_to_str(img.type), image_t::basetype_to_str(img.base_type));
    }
    fmt::print(stream, "\n");
}

void spirvcross_t::dump_debug(FILE* stream, errmsg_t::msg_format_t err_fmt, slang_t::type_t slang) const {
    fmt::print(stream, "spirvcross_t ({}):\n", slang_t::to_str(slang));
    if (error.valid) {
        fmt::print(stream, "  error: {}\n", error.as_string(err_fmt));
    }
    else {
        fmt::print(stream, "  error: not set\n");
    }
    for (const spirvcross_source_t& source: sources) {
        fmt::print(stream, "    source for snippet {}:\n", source.snippet_index);
        std::vector<std::string> lines;
        pystring::splitlines(source.source_code, lines);
        for (const std::string& line: lines) {
            fmt::print(stream, "      {}\n", line);
        }
        fmt::print(stream, "    reflection for snippet {}:\n", source.snippet_index);
        write_reflection_info(stream, source, "      ");
    }
    fmt::print(stream, "\n");
}

} // namespace shdc

