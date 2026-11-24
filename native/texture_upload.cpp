#include "runtime.h"
#include "PVRTDecompress.h"

#include <GLES/gl.h>
#include <algorithm>
#include <new>
#include <vector>

namespace {
constexpr GLenum pvrtc_rgb4 = 0x8c00;
constexpr GLenum pvrtc_rgb2 = 0x8c01;
constexpr GLenum pvrtc_rgba4 = 0x8c02;
constexpr GLenum pvrtc_rgba2 = 0x8c03;

bool power_of_two(uint32_t size)
{
    return size && !(size & (size - 1));
}

void require_upload_range(CPU *cpu, uint32_t address, uint32_t size)
{
    if (!size || !a5_guest_range(address, size)) {
        guest_abort(cpu, "Unsupported compressed texture source range");
    }
}
}

extern "C" void a5_upload_compressed_texture(CPU *cpu)
{
    GLenum target = cpu->r[0];
    GLint level = static_cast<GLint>(cpu->r[1]);
    GLenum format = cpu->r[2];
    uint32_t width = cpu->r[3];
    uint32_t height = rd32(cpu->r[13]);
    GLint border = static_cast<GLint>(rd32(cpu->r[13] + 4));
    uint32_t bytes = rd32(cpu->r[13] + 8);
    uint32_t address = rd32(cpu->r[13] + 12);
    require_upload_range(cpu, address, bytes);
    if (format < pvrtc_rgb4 || format > pvrtc_rgba2) {
        glCompressedTexImage2D(target, level, format, width, height, border, bytes, G(address));
        return;
    }

    bool two_bits = format == pvrtc_rgb2 || format == pvrtc_rgba2;
    bool opaque = format == pvrtc_rgb2 || format == pvrtc_rgb4;
    GLint max_texture = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture);
    if (max_texture <= 0 || border || !power_of_two(width) || !power_of_two(height) ||
        width > static_cast<uint32_t>(max_texture) ||
        height > static_cast<uint32_t>(max_texture) || width > 8192 || height > 8192) {
        guest_abort(cpu, "Invalid PVRTC1 texture dimensions %ux%u", width, height);
    }

    uint32_t storage_width = std::max(width, two_bits ? 16u : 8u);
    uint32_t storage_height = std::max(height, 8u);
    uint32_t required = storage_width * storage_height / (two_bits ? 4u : 2u);
    if (required != bytes) {
        guest_abort(cpu, "PVRTC1 level expected %u bytes, got %u", required, bytes);
    }

    try {
        std::vector<uint32_t> blocks(bytes / sizeof(uint32_t));
        memcpy(blocks.data(), G(address), bytes);
        std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
        pvr::PVRTDecompressPVRTC(blocks.data(), two_bits, width, height, pixels.data());
        if (opaque) {
            for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
        }
        GLint alignment;
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(target, level, GL_RGBA, width, height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, pixels.data());
        glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    } catch (const std::bad_alloc &) {
        guest_abort(cpu, "Out of memory decoding PVRTC1 texture");
    }
}
