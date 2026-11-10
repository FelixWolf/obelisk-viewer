#include "viewer/image/J2kDecoder.hpp"
#include "viewer/image/PixelConversion.hpp"

#include <openjpeg.h>

#include <algorithm>
#include <cstring>

namespace viewer::image {
namespace {

struct MemoryStream {
    const uint8_t* data;
    size_t size;
    size_t pos;
};

OPJ_SIZE_T streamRead(void* buffer, OPJ_SIZE_T bytes, void* user)
{
    auto* s = static_cast<MemoryStream*>(user);
    if (s->pos >= s->size) {
        return static_cast<OPJ_SIZE_T>(-1);
    }
    const size_t n = std::min<size_t>(bytes, s->size - s->pos);
    std::memcpy(buffer, s->data + s->pos, n);
    s->pos += n;
    return n;
}

OPJ_OFF_T streamSkip(OPJ_OFF_T bytes, void* user)
{
    auto* s = static_cast<MemoryStream*>(user);
    const auto target = std::clamp<OPJ_OFF_T>(static_cast<OPJ_OFF_T>(s->pos) + bytes, 0,
                                              static_cast<OPJ_OFF_T>(s->size));
    const OPJ_OFF_T skipped = target - static_cast<OPJ_OFF_T>(s->pos);
    s->pos = static_cast<size_t>(target);
    return skipped;
}

OPJ_BOOL streamSeek(OPJ_OFF_T pos, void* user)
{
    auto* s = static_cast<MemoryStream*>(user);
    if (pos < 0 || static_cast<size_t>(pos) > s->size) {
        return OPJ_FALSE;
    }
    s->pos = static_cast<size_t>(pos);
    return OPJ_TRUE;
}

void quietHandler(const char*, void*) {}

bool isJp2Container(std::span<const uint8_t> data)
{
    static constexpr uint8_t kJp2Signature[] = {0x00, 0x00, 0x00, 0x0C, 0x6A, 0x50,
                                                0x20, 0x20, 0x0D, 0x0A, 0x87, 0x0A};
    return data.size() >= sizeof(kJp2Signature) &&
           std::memcmp(data.data(), kJp2Signature, sizeof(kJp2Signature)) == 0;
}

class OpenJpegDecoder final : public J2kDecoder {
public:
    std::string_view name() const override { return "openjpeg"; }

    bool decode(std::span<const uint8_t> data, const J2kDecodeOptions& options,
                Image& out) const override
    {
        MemoryStream mem{data.data(), data.size(), 0};

        opj_stream_t* stream = opj_stream_create(OPJ_J2K_STREAM_CHUNK_SIZE, OPJ_TRUE);
        opj_stream_set_user_data(stream, &mem, nullptr);
        opj_stream_set_user_data_length(stream, data.size());
        opj_stream_set_read_function(stream, streamRead);
        opj_stream_set_skip_function(stream, streamSkip);
        opj_stream_set_seek_function(stream, streamSeek);

        opj_codec_t* codec = opj_create_decompress(isJp2Container(data) ? OPJ_CODEC_JP2 : OPJ_CODEC_J2K);
        opj_set_info_handler(codec, quietHandler, nullptr);
        opj_set_warning_handler(codec, quietHandler, nullptr);
        opj_set_error_handler(codec, quietHandler, nullptr);

        opj_dparameters_t params;
        opj_set_default_decoder_parameters(&params);
        params.cp_reduce = options.discardLevels;

        opj_image_t* image = nullptr;
        bool ok = opj_setup_decoder(codec, &params) &&
                  opj_codec_set_threads(codec, static_cast<int>(options.threads)) &&
                  opj_read_header(stream, codec, &image) &&
                  opj_decode(codec, stream, image) &&
                  opj_end_decompress(codec, stream);

        if (ok && image->numcomps > 0 && image->numcomps <= 4) {
            const uint32_t n = image->numcomps;
            uint8_t precision[4];
            bool isSigned[4];
            for (uint32_t c = 0; c < n; ++c) {
                precision[c] = static_cast<uint8_t>(image->comps[c].prec);
                isSigned[c] = image->comps[c].sgnd != 0;
            }
            const uint32_t w = image->comps[0].w;
            const uint32_t h = image->comps[0].h;
            interleaveTo8Bit(out, w, h, n, precision, isSigned,
                             [&](uint32_t c, uint32_t x, uint32_t y) {
                                 return image->comps[c].data[static_cast<size_t>(y) * image->comps[c].w + x];
                             });
        } else {
            ok = false;
        }

        if (image) {
            opj_image_destroy(image);
        }
        opj_destroy_codec(codec);
        opj_stream_destroy(stream);
        return ok;
    }
};

} // namespace

std::unique_ptr<J2kDecoder> createJ2kDecoder()
{
    return std::make_unique<OpenJpegDecoder>();
}

} // namespace viewer::image
