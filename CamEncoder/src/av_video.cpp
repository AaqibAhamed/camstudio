/**
 * Copyright(C) 2018  Steven Hoving
 *
 * This program is free software : you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.If not, see < https://www.gnu.org/licenses/>.
 */

#include "CamEncoder/av_video.h"
#include "CamEncoder/av_dict.h"
#include "CamEncoder/av_error.h"
#include <fmt/printf.h>
#include <fmt/ostream.h>
#include <cassert>

std::ostream &operator<<(std::ostream &os, const AVMediaType d)
{
    return os << av_get_media_type_string(d);
}

std::ostream &operator<<(std::ostream &os, const AVCodecID d)
{
    return os << avcodec_get_name(d);
}

std::ostream &operator<<(std::ostream &os, const AVRational d)
{
    return os << d.den << '/' << d.num;
}

std::ostream &operator<<(std::ostream &os, const AVPixelFormat d)
{
    return os << av_get_pix_fmt_name(d);
}

/*!
 * truncate fps, only used for mpeg4.
 */
AVRational truncate_fps(AVRational fps)
{
    while ((fps.num & ~0xFFFF) || (fps.den & ~0xFFFF))
    {
        fps.num >>= 1;
        fps.den >>= 1;
    }
    return fps;
}

/*!
 * \note hack, helper function sets both framerate and time_base.
 * resulting in the video encoder to be set to Constant Frame Rate.
 */
void set_fps(AVCodecContext *context, const AVRational &fps)
{
    context->time_base = { fps.den, fps.num };
    context->framerate = { fps.num, fps.den };
}

/*!
 * calculate a approximate gob size.
 */
int calculate_gop_size(const av_video_meta &meta)
{
    double gob_size = ((meta.fps.num / meta.fps.den) + 0.5) * 10.0;
    return static_cast<int>(gob_size);
}

void apply_preset(av_dict &av_opts, std::optional<video::preset> preset)
{
    const auto preset_idx = static_cast<int>(preset.value_or(video::preset::medium));
    const auto preset_name = video::preset_names.at(preset_idx);
    av_opts["preset"] = preset_name;
}

void apply_tune(av_dict &av_opts, std::optional<video::tune> preset)
{
    if (!preset)
        return;

    const auto tune_idx = static_cast<int>(preset.value());
    const auto tune_name = video::tune_names.at(tune_idx);
    av_opts["tune"] = tune_name;
}

void apply_profile(av_dict &av_opts, std::optional<video::profile> profile)
{
    if (!profile)
        return;

    const auto profile_idx = static_cast<int>(profile.value());
    const auto profile_name = video::profile_names.at(profile_idx);
    av_opts["profile"] = profile_name;
}

void dump_context(AVCodecContext *context)
{
    AVCodecParameters * params = avcodec_parameters_alloc();
    avcodec_parameters_from_context(params, context);

    fmt::print("Codec:\n");
    fmt::print("    codec type: {}\n", params->codec_type);
    fmt::print("      codec id: {}\n", params->codec_id);
    fmt::print("        format: {}\n", static_cast<AVPixelFormat>(params->format));
    fmt::print("      bit rate: {}\n", params->bit_rate);
    fmt::print("           bcs: {}\n", params->bits_per_coded_sample);
    fmt::print("           brs: {}\n", params->bits_per_raw_sample);

    fmt::print("       profile: {}\n", params->profile);
    fmt::print("         level: {}\n", params->level);
    fmt::print("  aspect ratio: {}\n", params->sample_aspect_ratio);
    fmt::print("    dimentions: {}x{}\n", params->width, params->height);

    avcodec_parameters_free(&params);
    params = nullptr;
}

AVFrame *create_video_frame(AVPixelFormat pix_fmt, int width, int height)
{
    AVFrame *video_frame = av_frame_alloc();
    if (!video_frame)
        return nullptr;

    video_frame->format = pix_fmt;
    video_frame->width = width;
    video_frame->height = height;

    /* allocate the buffers for the frame data */
    if (int ret = av_frame_get_buffer(video_frame, 32); ret < 0)
    {
        fprintf(stderr, "Could not allocate frame data.\n");
        throw std::runtime_error("blegh!!");
    }

    return video_frame;
}

av_video::av_video(const av_video_codec &config, const av_video_meta &meta)
{
    bool truncate_framerate = false;
    switch (config.id)
    {
    /* cam studio video codec is currently disabled because the ffmpeg implementation only supports
     * decoding.
     */
#if 0
    case AV_CODEC_ID_CSCD:
        printf("av_video: CamStudio encoder\n");
        break;
#endif
    case AV_CODEC_ID_H264:
        codec_type_ = av_video_codec_type::h264;
        printf("av_video: H264 encoder\n");
        break;
    default:
        throw std::runtime_error("av_video: unsupported encoder");
        break;
    }

    codec_ = avcodec_find_encoder(config.id);
    if (codec_ == nullptr)
        throw std::runtime_error("av_video: unable to find video encoder");

    context_ = avcodec_alloc_context3(codec_);

    auto fps = AVRational{ meta.fps.den, meta.fps.num };

    // Check if the codec has a specific set of supported frame rates. If it has, find the nearest
    // matching framerate.
    if (codec_->supported_framerates)
    {
        const auto idx = av_find_nearest_q_idx(fps, codec_->supported_framerates);
        AVRational supported_fps = codec_->supported_framerates[idx];
        if (supported_fps != fps)
        {
            fmt::print("av_video: framerate {} is not supported. Using {}.", fps, supported_fps);
            fps = supported_fps;
        }
    }

    set_fps(context_, fps);

    // calculate a approximate gob size.
    context_->gop_size = calculate_gop_size(meta);

    // either quality of bitrate must be set.
    assert(!!meta.quality || !!meta.bitrate);

    apply_preset(av_opts_, meta.preset);
    apply_tune(av_opts_, meta.tune);
    apply_profile(av_opts_, meta.profile);

    // Now set the things in context that we don't want to allow
    // the user to override.
    if (meta.bitrate)
    {
        // Average bitrate
        context_->bit_rate = static_cast<int64_t>(1000.0 * meta.bitrate.value());

        // ffmpeg's mpeg2 encoder requires that the bit_rate_tolerance be >= bitrate * fps
        //context_->bit_rate_tolerance = static_cast<int>(context_->bit_rate * av_q2d(fps) + 1);
    }
    else
    {
        /* Constant quantizer */
        context_->flags |= AV_CODEC_FLAG_QSCALE;

        /* global_quality only seem to apply to mpeg 1, 2 and 4 */
        //context_->global_quality = static_cast<int>(FF_QP2LAMBDA * meta.quality.value() + 0.5);

        // x264 requires this.
        av_opts_["crf"] = meta.quality.value();
    }

    context_->width = meta.width;
    context_->height = meta.height;
    context_->pix_fmt = AV_PIX_FMT_YUV420P;

    //context_->sample_aspect_ratio.num = job->par.num;
    //context_->sample_aspect_ratio.den = job->par.den;

    // \todo we have no grayscale settings for now
    //if (grayscale)
    //    context->flags |= AV_CODEC_FLAG_GRAY;

    context_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    frame_ = create_video_frame(context_->pix_fmt, context_->width, context_->height);
    sws_context_ = create_software_scaler(AV_PIX_FMT_BGR24, context_->width, context_->height);
}

av_video::~av_video()
{
    avcodec_free_context(&context_);
    av_frame_free(&frame_);
}

void av_video::open(AVStream *stream, av_dict &dict)
{
    // create a copy of our settings dict as avcodec_open2 clears it and fills it with the invalid
    //entries if it encounters them.
    dict = av_opts_;
    auto av_opts = av_opts_;

    if (int ret = avcodec_open2(context_, codec_, av_opts); ret)
        throw std::runtime_error(fmt::format("av_video: unable to open video encoder: {}",
            av_error_to_string(ret)));

    // avcodec_open populates the opts dictionary with the
    // things it didn't recognize.
    if (!av_opts.empty())
    {
        AVDictionaryEntry *t = nullptr;
        for (int i = 0; i < av_opts.size(); ++i)
        {
            t = av_opts.at("", t, AV_DICT_IGNORE_SUFFIX);
            fmt::print("av_video: unknown avcodec option: {}", t->key);
        }
    }

    if (stream != nullptr)
    {
        if (int ret = avcodec_parameters_from_context(stream->codecpar, context_); ret)
            throw std::runtime_error(
                fmt::format("av_video: failed to copy avcodec parameters: {}", av_error_to_string(ret)));
    }

    dump_context(context_);
}

void av_video::push_encode_frame(timestamp_t timestamp, BITMAPINFO *image)
{
    // also handle encoder flush
    AVFrame *encode_frame = nullptr;
    if (image != nullptr)
    {
        /* when we pass a frame to the encoder, it may keep a reference to it
        * internally; make sure we do not overwrite it here
        */
        if (av_frame_make_writable(frame_) < 0)
            throw std::runtime_error("Unable to make temp video frame writable");

        const auto &header = image->bmiHeader;

        const auto *src_data = ((LPBYTE)image) + header.biSize + (header.biClrUsed * sizeof(RGBQUAD));
        const auto src_data_size = header.biSizeImage;
        const auto src_width = header.biWidth;
        const auto src_height = header.biHeight;
        const auto src_pixel_format = AV_PIX_FMT_BGR24;

        const auto dst_width = context_->width;
        const auto dst_height = context_->height;
        const auto dst_pixel_format = context_->pix_fmt;

        // convert from rgb to yuv420p
        const uint8_t * const src[3] = {
            src_data + (src_data_size - (src_width * 3)),
            nullptr,
            nullptr
        };

        const int src_stride[3] = { src_width * -3, 0, 0 };
        const int dst_stride[3] = { dst_width, dst_width / 2, dst_width / 2 }; // for YUV420
        //const int dst_stride[3] = { dst_width, dst_width, dst_width }; // for YUV444
        sws_scale(sws_context_, src,
            src_stride, 0,
            src_height, frame_->data,
            dst_stride);

        frame_->pts = timestamp;
        encode_frame = frame_;
    }
    else
    {
        fmt::print("flush encoder\n");
    }

    if (int ret = avcodec_send_frame(context_, encode_frame); ret < 0)
        throw std::runtime_error(fmt::format("send video frame to encoder failed: {}",
            av_error_to_string(ret)));
}

bool av_video::pull_encoded_packet(AVPacket *pkt, bool *valid_packet)
{
    pkt->data = nullptr;
    pkt->size = 0;

    int ret = avcodec_receive_packet(context_, pkt);

    // only when ret == 0 we have a valid packet.
    *valid_packet = (ret == 0);

    if (ret == 0)
        return true;

    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        return true;

    return false;
}

av_video_codec_type av_video::get_codec_type() const noexcept
{
    return codec_type_;
}

AVCodecContext *av_video::get_codec_context() const noexcept
{
    return context_;
}

AVRational av_video::get_time_base() const noexcept
{
    return context_->time_base;
}

// \todo handle output pixel format, for when we want to encode yuv444 video.
SwsContext *av_video::create_software_scaler(AVPixelFormat src_pixel_format, int width, int height)
{
    SwsContext *software_scaler_context = sws_getContext(width, height,
        src_pixel_format,
        width, height,
        AV_PIX_FMT_YUV420P,
        //AV_PIX_FMT_YUV444P,
        SWS_BICUBIC, nullptr, nullptr, nullptr);

    if (!software_scaler_context)
        throw std::runtime_error("Could not initialize the conversion context");

    return software_scaler_context;
}
