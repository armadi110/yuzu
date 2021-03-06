// MIT License
//
// Copyright (c) Ryujinx Team and Contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
// associated documentation files (the "Software"), to deal in the Software without restriction,
// including without limitation the rights to use, copy, modify, merge, publish, distribute,
// sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or
// substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
// NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//

#include <array>
#include "common/bit_util.h"
#include "video_core/command_classes/codecs/h264.h"
#include "video_core/gpu.h"
#include "video_core/memory_manager.h"

namespace Tegra::Decoder {
namespace {
// ZigZag LUTs from libavcodec.
constexpr std::array<u8, 64> zig_zag_direct{
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

constexpr std::array<u8, 16> zig_zag_scan{
    0 + 0 * 4, 1 + 0 * 4, 0 + 1 * 4, 0 + 2 * 4, 1 + 1 * 4, 2 + 0 * 4, 3 + 0 * 4, 2 + 1 * 4,
    1 + 2 * 4, 0 + 3 * 4, 1 + 3 * 4, 2 + 2 * 4, 3 + 1 * 4, 3 + 2 * 4, 2 + 3 * 4, 3 + 3 * 4,
};
} // Anonymous namespace

H264::H264(GPU& gpu_) : gpu(gpu_) {}

H264::~H264() = default;

std::vector<u8>& H264::ComposeFrameHeader(NvdecCommon::NvdecRegisters& state, bool is_first_frame) {
    H264DecoderContext context{};
    gpu.MemoryManager().ReadBlock(state.picture_info_offset, &context, sizeof(H264DecoderContext));

    const s32 frame_number = static_cast<s32>((context.h264_parameter_set.flags >> 46) & 0x1ffff);
    if (!is_first_frame && frame_number != 0) {
        frame.resize(context.frame_data_size);

        gpu.MemoryManager().ReadBlock(state.frame_bitstream_offset, frame.data(), frame.size());
    } else {
        /// Encode header
        H264BitWriter writer{};
        writer.WriteU(1, 24);
        writer.WriteU(0, 1);
        writer.WriteU(3, 2);
        writer.WriteU(7, 5);
        writer.WriteU(100, 8);
        writer.WriteU(0, 8);
        writer.WriteU(31, 8);
        writer.WriteUe(0);
        const auto chroma_format_idc =
            static_cast<u32>((context.h264_parameter_set.flags >> 12) & 3);
        writer.WriteUe(chroma_format_idc);
        if (chroma_format_idc == 3) {
            writer.WriteBit(false);
        }

        writer.WriteUe(0);
        writer.WriteUe(0);
        writer.WriteBit(false); // QpprimeYZeroTransformBypassFlag
        writer.WriteBit(false); // Scaling matrix present flag

        const auto order_cnt_type = static_cast<u32>((context.h264_parameter_set.flags >> 14) & 3);
        writer.WriteUe(static_cast<u32>((context.h264_parameter_set.flags >> 8) & 0xf));
        writer.WriteUe(order_cnt_type);
        if (order_cnt_type == 0) {
            writer.WriteUe(context.h264_parameter_set.log2_max_pic_order_cnt);
        } else if (order_cnt_type == 1) {
            writer.WriteBit(context.h264_parameter_set.delta_pic_order_always_zero_flag != 0);

            writer.WriteSe(0);
            writer.WriteSe(0);
            writer.WriteUe(0);
        }

        const s32 pic_height = context.h264_parameter_set.pic_height_in_map_units /
                               (context.h264_parameter_set.frame_mbs_only_flag ? 1 : 2);

        writer.WriteUe(16);
        writer.WriteBit(false);
        writer.WriteUe(context.h264_parameter_set.pic_width_in_mbs - 1);
        writer.WriteUe(pic_height - 1);
        writer.WriteBit(context.h264_parameter_set.frame_mbs_only_flag != 0);

        if (!context.h264_parameter_set.frame_mbs_only_flag) {
            writer.WriteBit(((context.h264_parameter_set.flags >> 0) & 1) != 0);
        }

        writer.WriteBit(((context.h264_parameter_set.flags >> 1) & 1) != 0);
        writer.WriteBit(false); // Frame cropping flag
        writer.WriteBit(false); // VUI parameter present flag

        writer.End();

        // H264 PPS
        writer.WriteU(1, 24);
        writer.WriteU(0, 1);
        writer.WriteU(3, 2);
        writer.WriteU(8, 5);

        writer.WriteUe(0);
        writer.WriteUe(0);

        writer.WriteBit(context.h264_parameter_set.entropy_coding_mode_flag != 0);
        writer.WriteBit(false);
        writer.WriteUe(0);
        writer.WriteUe(context.h264_parameter_set.num_refidx_l0_default_active);
        writer.WriteUe(context.h264_parameter_set.num_refidx_l1_default_active);
        writer.WriteBit(((context.h264_parameter_set.flags >> 2) & 1) != 0);
        writer.WriteU(static_cast<s32>((context.h264_parameter_set.flags >> 32) & 0x3), 2);
        s32 pic_init_qp = static_cast<s32>((context.h264_parameter_set.flags >> 16) & 0x3f);
        pic_init_qp = (pic_init_qp << 26) >> 26;
        writer.WriteSe(pic_init_qp);
        writer.WriteSe(0);
        s32 chroma_qp_index_offset =
            static_cast<s32>((context.h264_parameter_set.flags >> 22) & 0x1f);
        chroma_qp_index_offset = (chroma_qp_index_offset << 27) >> 27;

        writer.WriteSe(chroma_qp_index_offset);
        writer.WriteBit(context.h264_parameter_set.deblocking_filter_control_flag != 0);
        writer.WriteBit(((context.h264_parameter_set.flags >> 3) & 1) != 0);
        writer.WriteBit(context.h264_parameter_set.redundant_pic_count_flag != 0);
        writer.WriteBit(context.h264_parameter_set.transform_8x8_mode_flag != 0);

        writer.WriteBit(true);

        for (s32 index = 0; index < 6; index++) {
            writer.WriteBit(true);
            const auto matrix_x4 =
                std::vector<u8>(context.scaling_matrix_4.begin(), context.scaling_matrix_4.end());
            writer.WriteScalingList(matrix_x4, index * 16, 16);
        }

        if (context.h264_parameter_set.transform_8x8_mode_flag) {
            for (s32 index = 0; index < 2; index++) {
                writer.WriteBit(true);
                const auto matrix_x8 = std::vector<u8>(context.scaling_matrix_8.begin(),
                                                       context.scaling_matrix_8.end());

                writer.WriteScalingList(matrix_x8, index * 64, 64);
            }
        }

        s32 chroma_qp_index_offset2 =
            static_cast<s32>((context.h264_parameter_set.flags >> 27) & 0x1f);
        chroma_qp_index_offset2 = (chroma_qp_index_offset2 << 27) >> 27;

        writer.WriteSe(chroma_qp_index_offset2);

        writer.End();

        const auto& encoded_header = writer.GetByteArray();
        frame.resize(encoded_header.size() + context.frame_data_size);
        std::memcpy(frame.data(), encoded_header.data(), encoded_header.size());

        gpu.MemoryManager().ReadBlock(state.frame_bitstream_offset,
                                      frame.data() + encoded_header.size(),
                                      context.frame_data_size);
    }

    return frame;
}

H264BitWriter::H264BitWriter() = default;

H264BitWriter::~H264BitWriter() = default;

void H264BitWriter::WriteU(s32 value, s32 value_sz) {
    WriteBits(value, value_sz);
}

void H264BitWriter::WriteSe(s32 value) {
    WriteExpGolombCodedInt(value);
}

void H264BitWriter::WriteUe(u32 value) {
    WriteExpGolombCodedUInt(value);
}

void H264BitWriter::End() {
    WriteBit(true);
    Flush();
}

void H264BitWriter::WriteBit(bool state) {
    WriteBits(state ? 1 : 0, 1);
}

void H264BitWriter::WriteScalingList(const std::vector<u8>& list, s32 start, s32 count) {
    std::vector<u8> scan(count);
    if (count == 16) {
        std::memcpy(scan.data(), zig_zag_scan.data(), scan.size());
    } else {
        std::memcpy(scan.data(), zig_zag_direct.data(), scan.size());
    }
    u8 last_scale = 8;

    for (s32 index = 0; index < count; index++) {
        const u8 value = list[start + scan[index]];
        const s32 delta_scale = static_cast<s32>(value - last_scale);

        WriteSe(delta_scale);

        last_scale = value;
    }
}

std::vector<u8>& H264BitWriter::GetByteArray() {
    return byte_array;
}

const std::vector<u8>& H264BitWriter::GetByteArray() const {
    return byte_array;
}

void H264BitWriter::WriteBits(s32 value, s32 bit_count) {
    s32 value_pos = 0;

    s32 remaining = bit_count;

    while (remaining > 0) {
        s32 copy_size = remaining;

        const s32 free_bits = GetFreeBufferBits();

        if (copy_size > free_bits) {
            copy_size = free_bits;
        }

        const s32 mask = (1 << copy_size) - 1;

        const s32 src_shift = (bit_count - value_pos) - copy_size;
        const s32 dst_shift = (buffer_size - buffer_pos) - copy_size;

        buffer |= ((value >> src_shift) & mask) << dst_shift;

        value_pos += copy_size;
        buffer_pos += copy_size;
        remaining -= copy_size;
    }
}

void H264BitWriter::WriteExpGolombCodedInt(s32 value) {
    const s32 sign = value <= 0 ? 0 : 1;
    if (value < 0) {
        value = -value;
    }
    value = (value << 1) - sign;
    WriteExpGolombCodedUInt(value);
}

void H264BitWriter::WriteExpGolombCodedUInt(u32 value) {
    const s32 size = 32 - Common::CountLeadingZeroes32(static_cast<s32>(value + 1));
    WriteBits(1, size);

    value -= (1U << (size - 1)) - 1;
    WriteBits(static_cast<s32>(value), size - 1);
}

s32 H264BitWriter::GetFreeBufferBits() {
    if (buffer_pos == buffer_size) {
        Flush();
    }

    return buffer_size - buffer_pos;
}

void H264BitWriter::Flush() {
    if (buffer_pos == 0) {
        return;
    }
    byte_array.push_back(static_cast<u8>(buffer));

    buffer = 0;
    buffer_pos = 0;
}
} // namespace Tegra::Decoder
