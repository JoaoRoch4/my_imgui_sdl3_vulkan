#include "image_ops.hpp"
#include "SDL3/SDL_stdinc.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <fstream>
#include <vector>

// Inclusões das bibliotecas nativas do FFmpeg
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

// ffmpegthumbnailer C API (self-guards with its own extern "C"). Wraps the FFmpeg
// libraries above to do smart, non-black frame selection for video thumbnails.
#include <libffmpegthumbnailer/videothumbnailerc.h>

#include <stb_image_resize2.h>
// Declaration-only: STB_DXT_IMPLEMENTATION is compiled in the standalone `stb`
// lib (thirdparty/stb/CMakeLists.txt), same pattern as stb_image_write above.
#include <stb_dxt.h>

namespace img::ops {

namespace {

	stbir_pixel_layout layout_for(int channels) {
		switch (channels) {
		case 1:
			return STBIR_1CHANNEL;
		case 2:
			return STBIR_2CHANNEL;
		case 3:
			return STBIR_RGB;
		default:
			return STBIR_RGBA;
		}
	}

	// Helper para converter os canais desejados para o enum do FFmpeg AVPixelFormat
	AVPixelFormat get_target_format(int channels) {
		if (channels == 3)
			return AV_PIX_FMT_RGB24;
		return AV_PIX_FMT_RGBA; // Default sRGB/RGBA padrão do seu pipeline
	}

} // namespace

std::optional<std::pair<int, int>> probe_dimensions(std::filesystem::path const& file) {
	std::error_code ec;
	if (!std::filesystem::exists(file, ec) || ec)
		return std::nullopt;

	AVFormatContext* format_ctx = nullptr;
	if (avformat_open_input(&format_ctx, file.string().c_str(), nullptr, nullptr) < 0)
		return std::nullopt;

	std::optional<std::pair<int, int>> dims;
	if (avformat_find_stream_info(format_ctx, nullptr) >= 0) {
		for (unsigned int i = 0; i < format_ctx->nb_streams; ++i) {
			AVCodecParameters const* par = format_ctx->streams[i]->codecpar;
			if (par->codec_type == AVMEDIA_TYPE_VIDEO && par->width > 0 && par->height > 0) {
				dims = std::pair{par->width, par->height};
				break;
			}
		}
	}
	avformat_close_input(&format_ctx);
	return dims;
}

std::expected<ImageBuffer, ImageError> decode_file(std::filesystem::path const& file, int desired_channels) {
	std::error_code ec;
	if (!std::filesystem::exists(file, ec) || ec)
		return std::unexpected(ImageError::FileNotFound);

	AVFormatContext* format_ctx = nullptr;
	// Abre o arquivo de entrada (funciona para PNG, JPG, WEBP, MP4, MKV, etc.)
	if (avformat_open_input(&format_ctx, file.string().c_str(), nullptr, nullptr) < 0) {
		return std::unexpected(ImageError::DecodeFailed);
	}

	// Busca as informações de streams contidas no container
	if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
		avformat_close_input(&format_ctx);
		return std::unexpected(ImageError::DecodeFailed);
	}

	// Localiza o stream de vídeo ou imagem principal
	int            stream_index = -1;
	AVCodec const* codec        = nullptr;
	stream_index                = av_find_best_stream(format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
	if (stream_index < 0 || !codec) {
		avformat_close_input(&format_ctx);
		return std::unexpected(ImageError::UnsupportedFormat);
	}

	AVStream*       stream    = format_ctx->streams[stream_index];
	AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
	if (!codec_ctx) {
		avformat_close_input(&format_ctx);
		return std::unexpected(ImageError::DecodeFailed);
	}

	if (avcodec_parameters_to_context(codec_ctx, stream->codecpar) < 0) {
		avcodec_free_context(&codec_ctx);
		avformat_close_input(&format_ctx);
		return std::unexpected(ImageError::DecodeFailed);
	}

	// Abre o codec correspondente (ex: libwebp, png, h264, hevc)
	if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
		avcodec_free_context(&codec_ctx);
		avformat_close_input(&format_ctx);
		return std::unexpected(ImageError::DecodeFailed);
	}

	// Se for um arquivo de vídeo longo, faz um seek rápido para os 10% do tempo total
	// para evitar extrair frames puramente pretos do início do arquivo.
	if (stream->duration > 0) {
		int64_t target_frame = stream->duration / 10;
		av_seek_frame(format_ctx, stream_index, target_frame, AVSEEK_FLAG_BACKWARD);
	}

	AVPacket* packet        = av_packet_alloc();
	AVFrame*  frame         = av_frame_alloc();
	bool      frame_decoded = false;

	// Loop de leitura de pacotes até decodificar o primeiro frame válido (I-Frame)
	while (av_read_frame(format_ctx, packet) >= 0) {
		if (packet->stream_index == stream_index) {
			if (avcodec_send_packet(codec_ctx, packet) >= 0) {
				int response = avcodec_receive_frame(codec_ctx, frame);
				if (response == 0) {
					frame_decoded = true;
					av_packet_unref(packet);
					break;
				}
			}
		}
		av_packet_unref(packet);
	}

	if (!frame_decoded) {
		av_frame_free(&frame);
		av_packet_free(&packet);
		avcodec_free_context(&codec_ctx);
		avformat_close_input(&format_ctx);
		return std::unexpected(ImageError::DecodeFailed);
	}

	// Aloca a estrutura final do ImageBuffer do seu sistema
	ImageBuffer out;
	out.width    = codec_ctx->width;
	out.height   = codec_ctx->height;
	out.channels = desired_channels;
	out.data.resize(static_cast<std::size_t>(out.width) * out.height * out.channels);

	// Inicializa o contexto do SwsScale para converter do formato nativo da imagem/vídeo
	// (ex: YUV420p, NV12) para o formato alvo linear RGBA8/RGB8 do seu pipeline
	AVPixelFormat target_fmt = get_target_format(desired_channels);
	SwsContext*   sws_ctx = sws_getContext(out.width, out.height, codec_ctx->pix_fmt, out.width, out.height, target_fmt,
		  SWS_BILINEAR, nullptr, nullptr, nullptr);

	if (sws_ctx) {
		uint8_t* dest_pointers[4]  = {out.data.data(), nullptr, nullptr, nullptr};
		Uint64      dest_linesizes[4] = {out.width * out.channels, 0, 0, 0};

		// Executa a conversão de espaço de cores e cópia via CPU de forma otimizada (vetorizada)
		sws_scale(sws_ctx, frame->data, frame->linesize, 0, out.height, 
			dest_pointers, std::bit_cast<int*>(&dest_linesizes));
		sws_freeContext(sws_ctx);
	} else {
		frame_decoded = false;
	}

	// Liberação de recursos de memória alocados pelo FFmpeg
	av_frame_free(&frame);
	av_packet_free(&packet);
	avcodec_free_context(&codec_ctx);
	avformat_close_input(&format_ctx);

	if (!frame_decoded) {
		return std::unexpected(ImageError::DecodeFailed);
	}

	return out;
}

std::expected<ImageBuffer, ImageError>
decode_video_thumbnail(std::filesystem::path const& file, int thumbnail_size, int desired_channels) {
	std::error_code ec;
	if (!std::filesystem::exists(file, ec) || ec)
		return std::unexpected(ImageError::FileNotFound);
	if (desired_channels != 3 && desired_channels != 4)
		return std::unexpected(ImageError::UnsupportedFormat);

	video_thumbnailer* thumb = video_thumbnailer_create();
	if (!thumb)
		return std::unexpected(ImageError::DecodeFailed);

	// Raw interleaved RGB24 must be requested explicitly — the default (Png) would
	// hand back an encoded blob instead of pixels. The longest-edge target is set
	// here; passing 0 keeps the native frame size.
	thumb->thumbnail_image_type = Rgb;
	if (thumbnail_size > 0)
		video_thumbnailer_set_size(thumb, thumbnail_size, thumbnail_size);

	// Thumbnail-selection policy. workaround_bugs tolerates slightly-broken or
	// non-seekable streams — the main cause of ffmpegthumbnailer's "Seeking in
	// video failed" on short clips / odd containers — by falling back to an early
	// frame instead of failing the whole thumbnail. The remaining knobs
	// (seek_percentage / seek_time / overlay_film_strip / prefer_embedded_metadata)
	// stay at library defaults: 10% sample, no film strip, keep aspect. GIFs no
	// longer reach this path (the file browser routes them to the still-image
	// decoder), so the 10% default is appropriate for real videos here.
	thumb->workaround_bugs = 1;

	image_data* img = video_thumbnailer_create_image_data();
	if (!img) {
		video_thumbnailer_destroy(thumb);
		return std::unexpected(ImageError::DecodeFailed);
	}

	int const rc = video_thumbnailer_generate_thumbnail_to_buffer(thumb, file.string().c_str(), img);

	std::expected<ImageBuffer, ImageError> result;
	if (rc != 0 || img->image_data_ptr == nullptr || img->image_data_size <= 0) {
		result = std::unexpected(ImageError::DecodeFailed);
	} else {
		// ffmpegthumbnailer always emits tightly-packed RGB24; expand to RGBA with
		// opaque alpha when the caller wants 4 channels, else copy straight through.
		ImageBuffer out;
		out.width             = img->image_data_width;
		out.height            = img->image_data_height;
		out.channels          = desired_channels;
		std::size_t const  px = static_cast<std::size_t>(out.width) * out.height;
		out.data.resize(px * desired_channels);

		uint8_t const* src = img->image_data_ptr;
		if (desired_channels == 3) {
			std::copy_n(src, px * 3, out.data.begin());
		} else {
			for (std::size_t i = 0; i < px; ++i) {
				out.data[i * 4 + 0] = src[i * 3 + 0];
				out.data[i * 4 + 1] = src[i * 3 + 1];
				out.data[i * 4 + 2] = src[i * 3 + 2];
				out.data[i * 4 + 3] = 255;
			}
		}
		result = std::move(out);
	}

	// Single cleanup path — both handles are freed on every outcome above.
	video_thumbnailer_destroy_image_data(img);
	video_thumbnailer_destroy(thumb);
	return result;
}

std::expected<ImageBuffer, ImageError> resize(ImageBuffer const& src, int dst_w, int dst_h) {
	if (!src.valid() || dst_w <= 0 || dst_h <= 0)
		return std::unexpected(ImageError::ResizeFailed);

	ImageBuffer dst;
	dst.width    = dst_w;
	dst.height   = dst_h;
	dst.channels = src.channels;
	dst.data.resize(static_cast<std::size_t>(dst_w) * dst_h * src.channels);

	unsigned char const* res = stbir_resize_uint8_linear(src.data.data(), src.width, src.height, 0, dst.data.data(),
		dst_w, dst_h, 0, layout_for(src.channels));

	if (res == nullptr)
		return std::unexpected(ImageError::ResizeFailed);
	return dst;
}

std::expected<ImageBuffer, ImageError>
resize_parallel(ImageBuffer const& src, int dst_w, int dst_h, int max_splits, SplitExecutor const& exec) {
	if (!src.valid() || dst_w <= 0 || dst_h <= 0 || max_splits < 1)
		return std::unexpected(ImageError::ResizeFailed);

	ImageBuffer dst;
	dst.width    = dst_w;
	dst.height   = dst_h;
	dst.channels = src.channels;
	dst.data.resize(static_cast<std::size_t>(dst_w) * dst_h * src.channels);

	STBIR_RESIZE r;
	stbir_resize_init(&r, src.data.data(), src.width, src.height, 0, dst.data.data(), dst_w, dst_h, 0,
		layout_for(src.channels), STBIR_TYPE_UINT8);
	r.horizontal_edge   = STBIR_EDGE_CLAMP;
	r.vertical_edge     = STBIR_EDGE_CLAMP;
	r.horizontal_filter = STBIR_FILTER_DEFAULT;
	r.vertical_filter   = STBIR_FILTER_DEFAULT;

	int const splits = stbir_build_samplers_with_splits(&r, max_splits);
	if (splits <= 0)
		return std::unexpected(ImageError::ResizeFailed);

	std::atomic<int> failures {0};
	exec(splits, [&](int i) {
		if (stbir_resize_extended_split(&r, i, 1) == 0)
			failures.fetch_add(1, std::memory_order_relaxed);
	});

	stbir_free_samplers(&r);

	if (failures.load(std::memory_order_relaxed) != 0)
		return std::unexpected(ImageError::ResizeFailed);
	return dst;
}

std::expected<bool, ImageError> encode_png(ImageBuffer const& src, std::filesystem::path const& out) {
	if (!src.valid())
		return std::unexpected(ImageError::EncodeFailed);

	// Lossless PNG via libavcodec's native PNG encoder. RGBA/RGB24 in, RGBA/RGB24 out
	// — no chroma subsampling, so encode->decode round-trips byte-for-byte, the alpha
	// channel survives, and the file on disk genuinely matches its .png name (the old
	// path emitted MJPEG bytes into a .png-named file: lossy and mislabeled).
	AVCodec const* codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
	if (!codec)
		return std::unexpected(ImageError::EncodeFailed);

	AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
	if (!codec_ctx)
		return std::unexpected(ImageError::EncodeFailed);

	AVPixelFormat const pix_fmt = get_target_format(src.channels); // RGBA (4ch) / RGB24 (3ch)
	codec_ctx->width     = src.width;
	codec_ctx->height    = src.height;
	codec_ctx->time_base = {1, 1};
	codec_ctx->pix_fmt   = pix_fmt;

	if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
		avcodec_free_context(&codec_ctx);
		return std::unexpected(ImageError::EncodeFailed);
	}

	AVFrame* frame = av_frame_alloc();
	frame->format  = pix_fmt;
	frame->width   = codec_ctx->width;
	frame->height  = codec_ctx->height;
	av_image_alloc(frame->data, frame->linesize, codec_ctx->width, codec_ctx->height, pix_fmt, 32);

	// Repack the tightly-packed CPU buffer into the frame's (32-byte-aligned) linesize.
	// Source and destination pixel formats are identical, so this is a pure stride copy.
	SwsContext* sws_ctx = sws_getContext(src.width, src.height, pix_fmt, src.width, src.height, pix_fmt,
		SWS_POINT, nullptr, nullptr, nullptr);

	if (sws_ctx) {
		uint8_t const* src_pointers[4]  = {src.data.data(), nullptr, nullptr, nullptr};
		Uint64            src_linesizes[4] = {src.width * src.channels, 0, 0, 0};
		sws_scale(sws_ctx, src_pointers, std::bit_cast<int*>(&src_linesizes), 0, src.height, frame->data, frame->linesize);
		sws_freeContext(sws_ctx);
	}

	AVPacket* packet         = av_packet_alloc();
	bool      encode_success = false;

	if (avcodec_send_frame(codec_ctx, frame) >= 0) {
		if (avcodec_receive_packet(codec_ctx, packet) == 0) {
			std::ofstream f(out, std::ios::binary);
			if (f.is_open()) {
				f.write(std::bit_cast<char const*>(packet->data), packet->size);
				encode_success = true;
			}
			av_packet_unref(packet);
		}
	}

	av_freep(&frame->data[0]);
	av_frame_free(&frame);
	av_packet_free(&packet);
	avcodec_free_context(&codec_ctx);

	if (!encode_success)
		return std::unexpected(ImageError::EncodeFailed);
	return true;
}

namespace {

// Encode the block-row range [by_lo, by_hi) of `src` into `out`. The full output
// must be pre-sized to bc1_size(src.width, src.height); each worker writes to
// disjoint 8-byte slices, so no synchronization is required.
void encode_bc1_rows(ImageBuffer const &src, int by_lo, int by_hi, std::vector<std::byte> &out) {
	const int W  = src.width;
	const int H  = src.height;
	const int bx = (W + 3) / 4;

	std::array<unsigned char, 64> block{}; // 4x4 RGBA, edge-replicated for partial blocks
	for (int byi = by_lo; byi < by_hi; ++byi) {
		for (int bxi = 0; bxi < bx; ++bxi) {
			for (int ry = 0; ry < 4; ++ry) {
				const int sy = std::min(byi * 4 + ry, H - 1); // clamp/replicate edge
				for (int rx = 0; rx < 4; ++rx) {
					const int         sx    = std::min(bxi * 4 + rx, W - 1);
					const std::size_t src_i = (static_cast<std::size_t>(sy) * W + sx) * 4u;
					const std::size_t dst_i = (static_cast<std::size_t>(ry) * 4 + rx) * 4u;
					block[dst_i + 0]        = src.data[src_i + 0];
					block[dst_i + 1]        = src.data[src_i + 1];
					block[dst_i + 2]        = src.data[src_i + 2];
					block[dst_i + 3]        = 255; // BC1 ignores alpha; keep opaque
				}
			}
			const std::size_t out_off
				= (static_cast<std::size_t>(byi) * static_cast<std::size_t>(bx) + static_cast<std::size_t>(bxi)) * 8u;
			stb_compress_dxt_block(std::bit_cast<unsigned char *>(out.data() + out_off), block.data(),
				0 /*no alpha -> DXT1, 8 bytes*/, STB_DXT_HIGHQUAL);
		}
	}
}

} // namespace

std::expected<std::vector<std::byte>, ImageError> encode_bc1(ImageBuffer const &src) {
	if (!src.valid() || src.channels != 4)
		return std::unexpected(ImageError::EncodeFailed);

	const int              by = (src.height + 3) / 4;
	std::vector<std::byte> out(bc1_size(src.width, src.height));
	encode_bc1_rows(src, 0, by, out);
	return out;
}

std::expected<std::vector<std::byte>, ImageError>
encode_bc1_parallel(ImageBuffer const &src, int max_splits, SplitExecutor const &exec) {
	if (!src.valid() || src.channels != 4 || max_splits < 1)
		return std::unexpected(ImageError::EncodeFailed);

	const int              by = (src.height + 3) / 4;
	std::vector<std::byte> out(bc1_size(src.width, src.height));

	// Each worker takes a contiguous range of block-rows. Splits cap at `by`
	// because there is no point spawning more workers than there are rows.
	const int splits = std::min(max_splits, by);
	if (splits <= 1) {
		encode_bc1_rows(src, 0, by, out);
		return out;
	}

	const int rows_per_split = (by + splits - 1) / splits; // ceil
	exec(splits, [&](int i) {
		const int lo = i * rows_per_split;
		const int hi = std::min(by, lo + rows_per_split);
		if (lo < hi)
			encode_bc1_rows(src, lo, hi, out);
	});
	return out;
}

} // namespace img::ops