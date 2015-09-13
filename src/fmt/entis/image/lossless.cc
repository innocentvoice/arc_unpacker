#include "err.h"
#include "fmt/entis/common/gamma_decoder.h"
#include "fmt/entis/common/huffman_decoder.h"
#include "fmt/entis/image/lossless.h"
#include "util/range.h"

using namespace au;
using namespace au::fmt::entis;
using namespace au::fmt::entis::image;

namespace
{
    struct DecodeContext
    {
        u8 eri_version;
        u8 op_table;
        u8 encode_type;
        u8 bit_count;

        size_t block_size;
        size_t block_area;
        size_t block_samples;
        size_t channel_count;
        size_t block_stride;

        size_t width_blocks;
        size_t height_blocks;
    };

    using Permutation = std::vector<int>;

    using ColorTransformer = std::function<void(u8 *, const DecodeContext &)>;
}

static Permutation init_permutation(const DecodeContext &ctx)
{
    Permutation permutation;
    permutation.resize(ctx.block_samples * 4);

    auto permutation_ptr = &permutation[0];
    for (auto c : util::range(ctx.channel_count))
    for (auto y : util::range(ctx.block_size))
    for (auto x : util::range(ctx.block_size))
        *permutation_ptr++ = c * ctx.block_area + y * ctx.block_size + x;

    for (auto c : util::range(ctx.channel_count))
    for (auto y : util::range(ctx.block_size))
    for (auto x : util::range(ctx.block_size))
        *permutation_ptr++ = c * ctx.block_area + y + x * ctx.block_size;

    for (auto y : util::range(ctx.block_size))
    for (auto x : util::range(ctx.block_size))
    for (auto c : util::range(ctx.channel_count))
        *permutation_ptr++ = c * ctx.block_area + y * ctx.block_size + x;

    for (auto y : util::range(ctx.block_size))
    for (auto x : util::range(ctx.block_size))
    for (auto c : util::range(ctx.channel_count))
        *permutation_ptr++ = c * ctx.block_area + y + x * ctx.block_size;

    return permutation;
}

static size_t get_channel_count(const EriHeader &header)
{
    switch (header.format_type & EriImage::TypeMask)
    {
        case EriImage::Rgb:
            if (header.bit_depth <= 8)
                return 1;
            if ((header.format_type & EriImage::WithAlpha))
                return 4;
            return 3;

        case EriImage::Gray:
            return 1;
    }
    throw err::CorruptDataError("Unknown pixel format");
}

static void color_op_0000(u8 *decode_buf, const DecodeContext &context)
{
}

static void color_op_0101(u8 *decode_buf, const DecodeContext &context)
{
    auto samples = context.block_area;
    auto size = context.block_area;
    auto i = 0;
    while (size--)
    {
        auto base = decode_buf[i];
        decode_buf[i++ + samples] += base;
    }
}

static void color_op_0110(u8 *decode_buf, const DecodeContext &context)
{
    auto samples = context.block_area * 2;
    auto size = context.block_area;
    auto i = 0;
    while (size--)
    {
        auto base = decode_buf[i];
        decode_buf[i++ + samples] += base;
    }
}

static void color_op_0111(u8 *decode_buf, const DecodeContext &context)
{
    auto samples = context.block_area;
    auto size = context.block_area;
    auto i = 0;
    while (size--)
    {
        auto base = decode_buf[i];
        decode_buf[i + samples] += base;
        decode_buf[i + samples * 2] += base;
        i ++;
    }
}

static void color_op_1001(u8 *decode_buf, const DecodeContext &context)
{
    auto samples = context.block_area;
    auto size = context.block_area;
    auto i = 0;
    while (size--)
    {
        auto base = decode_buf[i + samples];
        decode_buf[i++] += base;
    }
}

static void color_op_1010(u8 *decode_buf, const DecodeContext &context)
{
    auto samples = context.block_area;
    auto size = context.block_area;
    auto i = 0;
    while (size--)
    {
        auto base = decode_buf[i + samples];
        decode_buf[i++ + samples * 2] += base;
    }
}

static void color_op_1011(u8 *decode_buf, const DecodeContext &context)
{
    auto samples = context.block_area;
    auto size = context.block_area;
    auto i = 0;
    while (size--)
    {
        auto base = decode_buf[i + samples];
        decode_buf[i] += base;
        decode_buf[i + samples * 2] += base;
        i ++;
    }
}

static void color_op_1101(u8 *decode_buf, const DecodeContext &context)
{
    auto samples = context.block_area * 2;
    auto size = context.block_area;
    auto i = 0;
    while (size--)
    {
        auto base = decode_buf[i + samples];
        decode_buf[i++] += base;
    }
}

static void color_op_1110(u8 *decode_buf, const DecodeContext &context)
{
    auto samples = context.block_area;
    auto size = context.block_area;
    auto i = 0;
    while (size--)
    {
        auto base = decode_buf[i + samples * 2];
        decode_buf[i++ + samples] += base;
    }
}

static void color_op_1111(u8 *decode_buf, const DecodeContext &context)
{
    auto samples = context.block_area;
    auto size = context.block_area;
    auto i = 0;
    while (size--)
    {
        auto base = decode_buf[i + samples * 2];
        decode_buf[i] += base;
        decode_buf[i + samples] += base;
        i ++;
    }
}

static std::vector<ColorTransformer> color_ops
{
    color_op_0000,
    color_op_0000,
    color_op_0000,
    color_op_0000,
    color_op_0000,
    color_op_0101,
    color_op_0110,
    color_op_0111,
    color_op_0000,
    color_op_1001,
    color_op_1010,
    color_op_1011,
    color_op_0000,
    color_op_1101,
    color_op_1110,
    color_op_1111,
};

static void transform(
    u8 transformer_code,
    const DecodeContext &ctx,
    const Permutation &permutation,
    const bstr &arrange_buf,
    u8 *prev_block_row,
    u8 *prev_block_col,
    bstr &block_out)
{
    auto diff_mode   = (transformer_code & 0b11'00'0000) >> 6;
    auto perm_offset = (transformer_code & 0b00'11'0000) >> 4;
    auto color_op    = (transformer_code & 0b00'00'1111);

    for (auto i : util::range(ctx.block_samples))
    {
        block_out[permutation[perm_offset * ctx.block_samples + i]]
            = arrange_buf[i];
    }
    if (!transformer_code)
        return;

    color_ops[color_op](block_out.get<u8>(), ctx);

    if (diff_mode & 0x01)
    {
        auto block_out_ptr = block_out.get<u8>();
        auto next_block_col_ptr = prev_block_col;
        for (auto i : util::range(ctx.block_stride))
        {
            u8 last_value = *next_block_col_ptr;
            for (auto j : util::range(ctx.block_size))
            {
                last_value += *block_out_ptr;
                *block_out_ptr++ = last_value;
            }
            *next_block_col_ptr++ = last_value;
        }
    }
    else
    {
        auto block_out_ptr = block_out.get<u8>();
        auto next_block_col_ptr = prev_block_col;
        for (auto i : util::range(ctx.block_stride))
        {
            *next_block_col_ptr++ = block_out_ptr[ctx.block_size - 1];
            block_out_ptr += ctx.block_size;
        }
    }

    {
        auto prev_block_row_base = prev_block_row;
        auto block_out_ptr = block_out.get<u8>();
        for (auto k : util::range(ctx.channel_count))
        {
            auto prev_block_row_ptr = prev_block_row_base;
            for (auto i : util::range(ctx.block_size))
            {
                auto ptrCurrentLine = block_out_ptr;
                for (auto j : util::range(ctx.block_size))
                    *block_out_ptr++ += *prev_block_row_ptr++;
                prev_block_row_ptr = ptrCurrentLine;
            }
            for (auto j : util::range(ctx.block_size))
                *prev_block_row_base++ = *prev_block_row_ptr++;
        }
    }
}

static u8 get_transformer_code(
    const EriHeader &header,
    DecodeContext &ctx,
    common::Decoder &decoder,

    u8 *&transformer_codes_ptr,
    common::HuffmanTree &huffman_tree)
{
    if (ctx.channel_count < 3)
    {
        if (!(ctx.encode_type & 0x01) && header.architecture
            == common::Architecture::RunLengthGamma)
        {
            decoder.reset();
        }
        if (header.format_type == EriImage::Gray)
            return 0b11'00'0000;
        return 0b00'00'0000;
    }

    if (ctx.encode_type & 1)
        return *transformer_codes_ptr++;

    if (header.architecture == common::Architecture::RunLengthHuffman)
    {
        return dynamic_cast<common::HuffmanDecoder&>(decoder)
                .get_huffman_code(huffman_tree);
    }

    if (header.architecture == common::Architecture::RunLengthGamma)
    {
        auto transformer_code = 0b11'00'0000 | decoder.bit_reader->get(4);
        decoder.reset();
        return transformer_code;
    }

    throw err::NotSupportedError("Architecture not supported");
}

static std::vector<u8> prefetch_transformer_codes(
    const DecodeContext &ctx,
    const EriHeader &header,
    common::Decoder &decoder,
    common::HuffmanTree &huffman_tree)
{
    std::vector<u8> transformer_codes;
    if (!(ctx.encode_type & 0x01) || (ctx.channel_count < 3))
        return transformer_codes;
    for (auto i : util::range(ctx.width_blocks * ctx.height_blocks))
    {
        u8 op_code;
        if (header.architecture == common::Architecture::RunLengthGamma)
        {
            op_code = 0b11'00'0000 | decoder.bit_reader->get(4);
        }
        else if (header.architecture == common::Architecture::RunLengthHuffman)
        {
            op_code = dynamic_cast<common::HuffmanDecoder&>(decoder)
                .get_huffman_code(huffman_tree);
        }
        else
            throw err::NotSupportedError("Architecture not supported");
        transformer_codes.push_back(op_code);
    }
    return transformer_codes;
}

static void validate_ctx(const DecodeContext &ctx, const EriHeader &header)
{
    if (ctx.op_table || (ctx.encode_type & 0xFE))
        throw err::CorruptDataError("Unexpected meta data");

    switch (ctx.eri_version)
    {
        case 1:
            if (ctx.bit_count != 0)
                throw new err::UnsupportedBitDepthError(ctx.bit_count);
            break;
        case 8:
            if (ctx.bit_count != 8)
                throw new err::UnsupportedBitDepthError(ctx.bit_count);
            break;
        case 16:
            if (ctx.bit_count != 8 || ctx.encode_type)
                throw new err::UnsupportedBitDepthError(ctx.bit_count);
            break;
        default:
            throw err::UnsupportedVersionError(ctx.eri_version);
    }

    if (!header.blocking_degree)
        throw err::CorruptDataError("Blocking degree not set");
}

static bstr crop(
    const bstr &input,
    const DecodeContext &ctx,
    const EriHeader &header)
{
    bstr output(header.width * header.height * ctx.channel_count);
    for (auto y : util::range(header.height))
    {
        auto output_ptr = output.get<u8>();
        auto input_ptr = input.get<u8>();
        output_ptr += y * header.width * ctx.channel_count;
        input_ptr += y * ctx.width_blocks * ctx.block_stride;
        for (auto x : util::range(header.width * ctx.channel_count))
            *output_ptr++ = *input_ptr++;
    }
    return output;
}

bstr image::decode_lossless_pixel_data(
    const EriHeader &header, common::Decoder &decoder)
{
    DecodeContext ctx;
    ctx.eri_version = decoder.bit_reader->get(8);
    ctx.op_table = decoder.bit_reader->get(8);
    ctx.encode_type = decoder.bit_reader->get(8);
    ctx.bit_count = decoder.bit_reader->get(8);

    ctx.channel_count = get_channel_count(header);
    ctx.block_size = (1 << header.blocking_degree);
    ctx.block_area = ctx.block_size * ctx.block_size;
    ctx.block_samples = ctx.block_area * ctx.channel_count;
    ctx.block_stride = ctx.block_size * ctx.channel_count;

    ctx.width_blocks = (header.width + ctx.block_size - 1) / ctx.block_size;
    ctx.height_blocks = (header.height + ctx.block_size - 1) / ctx.block_size;

    validate_ctx(ctx, header);

    common::HuffmanTree huffman_tree;

    auto permutation = init_permutation(ctx);
    auto transformer_codes = prefetch_transformer_codes(
        ctx,
        header,
        decoder,
        huffman_tree);

    if (decoder.bit_reader->get(1))
        throw err::CorruptDataError("Expected 0 bit");

    if (header.architecture == common::Architecture::RunLengthGamma)
    {
        if (ctx.encode_type & 0x01)
            decoder.reset();
    }
    else if (header.architecture == common::Architecture::RunLengthHuffman)
        decoder.reset();
    else
        throw err::NotSupportedError("Architecture not supported");

    bstr output(ctx.width_blocks * ctx.height_blocks * ctx.block_samples);
    bstr arrange_buf(ctx.block_samples);
    bstr block_out(ctx.block_samples);
    bstr prev_col(ctx.height_blocks * ctx.block_stride);
    bstr prev_row(ctx.width_blocks * ctx.block_stride);

    auto transformer_codes_ptr = &transformer_codes[0];
    for (auto y : util::range(ctx.height_blocks))
    {
        for (auto x : util::range(ctx.width_blocks))
        {
            u8 transformer_code = get_transformer_code(
                header,
                ctx,
                decoder,
                transformer_codes_ptr,
                huffman_tree);

            decoder.decode(arrange_buf.get<u8>(), arrange_buf.size());

            transform(
                transformer_code,
                ctx,
                permutation,
                arrange_buf,
                prev_row.get<u8>() + x * ctx.block_stride,
                prev_col.get<u8>() + y * ctx.block_stride,
                block_out);

            auto *block_out_ptr = block_out.get<u8>();
            for (auto c : util::range(ctx.channel_count))
            for (auto yy : util::range(ctx.block_size))
            for (auto xx : util::range(ctx.block_size))
            {
                auto sum_y = y * ctx.block_size + yy;
                auto sum_x = x * ctx.block_size + xx;
                auto pos = sum_y * ctx.width_blocks * ctx.block_size + sum_x;
                output[pos * ctx.channel_count + c] = *block_out_ptr++;
            }
        }
    }

    return crop(output, ctx, header);
}
