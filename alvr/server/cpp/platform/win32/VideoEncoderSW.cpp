#ifdef ALVR_GPL

#include "VideoEncoderSW.h"

#include "alvr_server/Statistics.h"
#include "alvr_server/Logger.h"
#include "alvr_server/Settings.h"
#include "alvr_server/Utils.h"

#include <iostream>
#include <string>
#include <array>
#include <algorithm>

// FFmpeg 6 renamed the FF_PROFILE_* macros to AV_PROFILE_*; keep the old names
// so this 2022-era source compiles against newer ffmpeg headers.
#ifndef FF_PROFILE_H264_HIGH_10
#define FF_PROFILE_H264_HIGH_10 AV_PROFILE_H264_HIGH_10
#endif
#ifndef FF_PROFILE_H264_HIGH
#define FF_PROFILE_H264_HIGH AV_PROFILE_H264_HIGH
#endif
#ifndef FF_PROFILE_HEVC_MAIN_10
#define FF_PROFILE_HEVC_MAIN_10 AV_PROFILE_HEVC_MAIN_10
#endif
#ifndef FF_PROFILE_HEVC_MAIN
#define FF_PROFILE_HEVC_MAIN AV_PROFILE_HEVC_MAIN
#endif

VideoEncoderSW::VideoEncoderSW(std::shared_ptr<CD3DRender> d3dRender
	, std::shared_ptr<ClientConnection> listener
	, int width, int height)
	: m_d3dRender(d3dRender)
	, m_Listener(listener)
	, m_codec((ALVR_CODEC)Settings::Instance().m_codec)
	, m_refreshRate(Settings::Instance().m_refreshRate)
	, m_renderWidth(width)
	, m_renderHeight(height)
	, m_bitrateInMBits(Settings::Instance().mEncodeBitrateMBs) {
#ifdef ALVR_DEBUG_LOG
	av_log_set_level(AV_LOG_DEBUG);
	av_log_set_callback(LibVALog);
	Debug("Set FFMPEG/LibAV to debug logging");
#endif
	}

VideoEncoderSW::~VideoEncoderSW() {
	if (m_hwDeviceCtx) {
		av_buffer_unref(&m_hwDeviceCtx);
	}
}

void VideoEncoderSW::LibVALog(void* v, int level, const char* data, va_list va) {
	const char* prefix = "[libav]: ";
	std::stringstream sstream;
	sstream << prefix << data;
	vprintf(sstream.str().c_str(), va);
}

void VideoEncoderSW::Initialize() {
	int err;
	Debug("Initializing VideoEncoderSW.\n");

	if(!ToFFMPEGCodec(m_codec)) throw MakeException("Invalid requested codec %d", m_codec);

	// Try the Intel QuickSync hardware encoder first: it ships inside the
	// bundled ffmpeg libraries and only requires installed Intel graphics
	// drivers. Fall back to pure software encoding when unavailable. Note:
	// an up-to-date Intel driver is required for the ffmpeg oneVPL runtime to
	// produce a decodable stream (older drivers can yield a green screen on
	// the client).
	for (int pass = 0; pass < 2; ++pass) {
		bool qsv = pass == 0;
		const AVCodec *codec = qsv
			? avcodec_find_encoder_by_name(m_codec == ALVR_CODEC_H265 ? "hevc_qsv" : "h264_qsv")
			: avcodec_find_encoder(ToFFMPEGCodec(m_codec));
		if (codec == NULL) continue;

		// Initialize CodecContext
		m_codecContext = avcodec_alloc_context3(codec);
		if(m_codecContext == NULL) throw MakeException("Failed to allocate encoder id %d", codec->id);

		// QSV needs a hardware device context to upload the system-memory frames
		// we feed it. ffmpeg's CLI creates this automatically; library users must
		// do it explicitly, otherwise every avcodec_send_frame() returns EINVAL
		// (no encoded output -> client shows a green/garbage frame).
		if (qsv) {
			AVBufferRef *hwDevice = nullptr;
			if (av_hwdevice_ctx_create(&hwDevice, AV_HWDEVICE_TYPE_QSV, "auto", nullptr, 0) < 0) {
				Debug("Failed to create QSV hardware device, falling back to software encoding.");
				av_buffer_unref(&hwDevice);
				continue;
			}
			m_hwDeviceCtx = hwDevice;
			m_codecContext->hw_device_ctx = av_buffer_ref(hwDevice);
		}

		// Set codec settings
		AVDictionary* opt = NULL;
		if (qsv) {
			av_dict_set(&opt, "async_depth", "1", 0);
			av_dict_set(&opt, "idr_interval", "0", 0);
		} else {
			av_dict_set(&opt, "preset", "ultrafast", 0);
			av_dict_set(&opt, "tune", "zerolatency", 0);
		}
		if (qsv) {
			// The profile must match the actual output format: QSV encodes
			// 8-bit H.264 (NV12 input) and 8/10-bit HEVC (NV12/P010 input).
			switch (m_codec) {
				case ALVR_CODEC_H264:
					m_codecContext->profile = FF_PROFILE_H264_HIGH;
					break;
				case ALVR_CODEC_H265:
					m_codecContext->profile =
						Settings::Instance().m_use10bitEncoder ? FF_PROFILE_HEVC_MAIN_10 : FF_PROFILE_HEVC_MAIN;
					break;
			}
		} else {
			switch (m_codec) {
				case ALVR_CODEC_H264:
					m_codecContext->profile = Settings::Instance().m_use10bitEncoder ? FF_PROFILE_H264_HIGH_10 : FF_PROFILE_H264_HIGH;
					break;
				case ALVR_CODEC_H265:
					m_codecContext->profile = Settings::Instance().m_use10bitEncoder ? FF_PROFILE_HEVC_MAIN_10 : FF_PROFILE_HEVC_MAIN;
					break;
			}
		}

		// QSV requires the encode dimensions to be a multiple of 16; round down
		// to be safe (x264 tolerates arbitrary sizes, so leave those alone).
		int encWidth = qsv ? ((Settings::Instance().m_renderWidth + 15) & ~15) : Settings::Instance().m_renderWidth;
		int encHeight = qsv ? ((Settings::Instance().m_renderHeight + 15) & ~15) : Settings::Instance().m_renderHeight;

		m_codecContext->width = encWidth;
		m_codecContext->height = encHeight;
		m_codecContext->time_base = AVRational{1, (int)(1e9)};
		m_codecContext->framerate = AVRational{Settings::Instance().m_refreshRate, 1};
		m_codecContext->sample_aspect_ratio = AVRational{1, 1};
		m_isQsv = qsv;
		if (qsv) {
			// The encoder receives QSV hardware frames; we upload system NV12
			// (P010 for 10-bit HEVC) into them ourselves, so the encoder's input
			// pixel format is QSV. The actual memory layout is defined by the
			// hw_frames_ctx built after avcodec_open2. The new oneVPL runtime
			// rejects system-memory frames (EINVAL / -22) unless we do this.
			m_codecContext->pix_fmt = AV_PIX_FMT_QSV;
		} else {
			m_codecContext->pix_fmt = Settings::Instance().m_use10bitEncoder ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
		}
		m_codecContext->max_b_frames = 0;
		m_codecContext->bit_rate = Settings::Instance().mEncodeBitrateMBs * 1000 * 1000;
		m_codecContext->thread_count = Settings::Instance().m_swThreadCount;

		err = avcodec_open2(m_codecContext, codec, &opt);
		av_dict_free(&opt);
		if(err) {
			avcodec_free_context(&m_codecContext);
			m_codecContext = NULL;
			if (qsv) {
				Debug("QuickSync encoder unavailable (err=%d), falling back to software encoding.", err);
				continue;
			}
			throw MakeException("Cannot open video encoder codec: %d", err);
		}

		Info("Using %s encoder.", qsv ? "Intel QuickSync" : "software");

		// Config transfer/encode frames
		m_transferredFrame = av_frame_alloc();
		m_transferredFrame->buf[0] = av_buffer_alloc(1);

		if (qsv) {
			// Build a QSV hardware-frame pool. Each sws-scaled system NV12 frame
			// is uploaded into one of these surfaces before being sent; modern
			// oneVPL drivers require hardware frames and reject system memory.
			AVBufferRef *hwFrames = av_hwframe_ctx_alloc(m_hwDeviceCtx);
			AVHWFramesContext *fr = (AVHWFramesContext *)hwFrames->data;
			fr->format = AV_PIX_FMT_QSV;
			fr->sw_format = (m_codec == ALVR_CODEC_H265 && Settings::Instance().m_use10bitEncoder)
				? AV_PIX_FMT_P010LE
				: AV_PIX_FMT_NV12;
			fr->width = encWidth;
			fr->height = encHeight;
			fr->initial_pool_size = 16;
			if ((err = av_hwframe_ctx_init(hwFrames)) < 0)
				throw MakeException("Error initializing QSV hw frames: %d", err);
			m_hwFramesCtx = hwFrames;
			m_codecContext->hw_frames_ctx = av_buffer_ref(hwFrames);

			m_encoderFrame = av_frame_alloc();
			m_encoderFrame->format = AV_PIX_FMT_QSV;
			m_encoderFrame->hw_frames_ctx = av_buffer_ref(m_hwFramesCtx);
			m_encoderFrame->width = encWidth;
			m_encoderFrame->height = encHeight;
			if ((err = av_hwframe_get_buffer(m_encoderFrame, 0)))
				throw MakeException("Error allocating QSV encoder frame: %d", err);

			m_swFrame = av_frame_alloc();
			m_swFrame->format = fr->sw_format;
			m_swFrame->width = encWidth;
			m_swFrame->height = encHeight;
			if ((err = av_frame_get_buffer(m_swFrame, 0)))
				throw MakeException("Error allocating sw frame: %d", err);
		} else {
			m_encoderFrame = av_frame_alloc();
			m_encoderFrame->width = encWidth;
			m_encoderFrame->height = encHeight;
			m_encoderFrame->format = m_codecContext->pix_fmt;
			if((err = av_frame_get_buffer(m_encoderFrame, 0))) throw MakeException("Error when allocating encoder frame: %d", err);
		}

		Debug("Successfully initialized VideoEncoderSW");
		return;
	}

	throw MakeException("No usable video encoder found for codec %d", m_codec);
}

void VideoEncoderSW::Shutdown() {
	Debug("Shutting down VideoEncoderSW.\n");

	av_frame_free(&m_transferredFrame);
	av_frame_free(&m_encoderFrame);
	av_frame_free(&m_swFrame);

	avcodec_free_context(&m_codecContext);
	sws_freeContext(m_scalerContext);
	m_scalerContext = nullptr;
	if (m_hwFramesCtx) av_buffer_unref(&m_hwFramesCtx);

	Debug("Successfully shutdown VideoEncoderSW.\n");
}

bool VideoEncoderSW::should_keep_nal_h264(const uint8_t *header_start) {
 	uint8_t nal_type = (header_start[2] == 0 ? header_start[4] : header_start[3]) & 0x1F;
    switch (nal_type) {
		case 6: // supplemental enhancement information
		case 9: // access unit delimiter
			return false;
		default:
			return true;
    }
}

bool VideoEncoderSW::should_keep_nal_h265(const uint8_t *header_start) {
	uint8_t nal_type = ((header_start[2] == 0 ? header_start[4] : header_start[3]) >> 1) & 0x3F;
	switch (nal_type) {
		case 35: // access unit delimiter
		case 39: // supplemental enhancement information
		return false;
		default:
		return true;
	}
}

void VideoEncoderSW::filter_NAL(const uint8_t *input, size_t input_size, std::vector<uint8_t> &out)
{
	if (input_size < 4) return;
	ALVR_CODEC codec = m_codec;
	std::array<uint8_t, 3> header = {{0, 0, 1}};
	const uint8_t *end = input + input_size;
	const uint8_t *header_start = input;
	while (header_start != end) {
		const uint8_t *next_header = std::search(header_start + 3, end, header.begin(), header.end());
		if (next_header != end && next_header[-1] == 0) next_header--;
		if (codec == ALVR_CODEC_H264 && should_keep_nal_h264(header_start))
		out.insert(out.end(), header_start, next_header);
		if (codec == ALVR_CODEC_H265 && should_keep_nal_h265(header_start))
		out.insert(out.end(), header_start, next_header);
		header_start = next_header;
	}
}

// Pull the stream headers (SPS/PPS, plus VPS for HEVC) out of the first
// keyframe so they can be re-injected on later keyframes. Stops at the first
// coded slice.
void VideoEncoderSW::cache_sps_pps(const uint8_t *input, size_t input_size)
{
	if (input_size < 4) return;
	std::array<uint8_t, 3> header = {{0, 0, 1}};
	const uint8_t *end = input + input_size;
	const uint8_t *header_start = input;
	while (header_start != end) {
		const uint8_t *next_header = std::search(header_start + 3, end, header.begin(), header.end());
		if (next_header != end && next_header[-1] == 0) next_header--;
		uint8_t nal_type = header_start[3] & 0x1F;
		bool is_header = (m_codec == ALVR_CODEC_H264)
			? (nal_type == 7 || nal_type == 8)
			: (nal_type == 32 || nal_type == 33 || nal_type == 34);
		if (is_header) {
			m_spsPpsCache.insert(m_spsPpsCache.end(), header_start, next_header);
		} else if (nal_type == 1 || nal_type == 5 || (m_codec == ALVR_CODEC_H265 && nal_type <= 21)) {
			// first coded slice -> headers are complete
			break;
		}
		header_start = next_header;
	}
}

void VideoEncoderSW::Transmit(ID3D11Texture2D *pTexture, uint64_t presentationTime, uint64_t targetTimestampNs, bool insertIDR) {
	// Handle bitrate changes
	if(m_Listener->GetStatistics()->CheckBitrateUpdated()) {
		//Debug("Bitrate changed");
		m_codecContext->bit_rate = m_Listener->GetStatistics()->GetBitrate() * 1000000L;
	}

	// Setup staging texture if not defined yet; we can only define it here as we now have the texture's size
	if(!stagingTex) {
		HRESULT hr = SetupStagingTexture(pTexture);
		if(FAILED(hr)) {
			Error("Failed to create staging texture: %p %ls", hr, GetErrorStr(hr).c_str());
			return;
		}
		Debug("Success in creating staging texture");
	}

	// Copy texture and map it to memory
	/// SteamVR crashes if the swapchain textures are set to staging, which is needed to be read by the CPU.
	/// Unless there's another solution we have to copy the texture every time, which is gonna be another performance hit.
	HRESULT hr = CopyTexture(pTexture);
	if(FAILED(hr)) {
		Error("Failed to copy texture to staging: %p %ls", hr, GetErrorStr(hr).c_str());
		return;
	}
	//Debug("Success in mapping staging texture");

	int err;

	// Setup software scaler if not defined yet; we can only define it here as we now have the texture's size
	// FIXME: Hardcoded to DirectX's R8G8B8A8, make more robust system if needed
	if(!m_scalerContext) {
		// sws always writes to a system-memory frame; for QSV that is the
		// intermediate NV12 (P010) frame we then upload into a QSV surface.
		AVPixelFormat swsDstFormat = m_isQsv
			? ((m_codec == ALVR_CODEC_H265 && Settings::Instance().m_use10bitEncoder) ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12)
			: m_codecContext->pix_fmt;
		m_scalerContext = sws_getContext(stagingTexDesc.Width, stagingTexDesc.Height, AV_PIX_FMT_RGBA,
		m_codecContext->width, m_codecContext->height, swsDstFormat,
		SWS_BILINEAR, NULL, NULL, NULL);
		if(!m_scalerContext) {
			Error("Couldn't initialize SWScaler.");
			m_d3dRender->GetContext()->Unmap(stagingTex.Get(), 0);
			return;
		}
		Debug("Successfully initialized SWScaler.");
	}

	// We got the texture, populate tansferredFrame with data
	m_transferredFrame->width = stagingTexDesc.Width;
	m_transferredFrame->height = stagingTexDesc.Height;
	m_transferredFrame->data[0] = (uint8_t*)stagingTexMap.pData;
	m_transferredFrame->linesize[0] = stagingTexMap.RowPitch;
	m_transferredFrame->format = AV_PIX_FMT_RGBA;
	m_transferredFrame->pts = targetTimestampNs;

	// Use SWScaler for scaling
	AVFrame *swsDst = m_isQsv ? m_swFrame : m_encoderFrame;
	if(sws_scale(m_scalerContext, m_transferredFrame->data, m_transferredFrame->linesize,
				0, m_transferredFrame->height, swsDst->data, swsDst->linesize) == 0) {
		Error("SWScale failed.");
		m_d3dRender->GetContext()->Unmap(stagingTex.Get(), 0);
		return;
	}
	//Debug("SWScale succeeded.");

	// Send frame for encoding
	if (m_isQsv) {
		// Upload the sws output into a QSV hardware surface. Modern oneVPL
		// rejects system-memory frames (EINVAL / -22).
		if ((err = av_hwframe_transfer_data(m_encoderFrame, m_swFrame, 0)) < 0) {
			Error("QSV frame upload failed: err code %d", err);
			m_d3dRender->GetContext()->Unmap(stagingTex.Get(), 0);
			return;
		}
	}
	m_encoderFrame->pict_type = insertIDR ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
	m_encoderFrame->pts = targetTimestampNs;

	if((err = avcodec_send_frame(m_codecContext, m_encoderFrame)) < 0) {
		Error("Encoding frame failed: err code %d", err);
		m_d3dRender->GetContext()->Unmap(stagingTex.Get(), 0);
		return;
	}
	//Debug("Send frame succeeded.");

	// Retrieve frames from encoding and send them until buffer is emptied
	while(true) {
		AVPacket *packet = av_packet_alloc();
		if((err = avcodec_receive_packet(m_codecContext, packet))) {
			if(err == AVERROR(EAGAIN)) {
				// Output buffer was emptied, move on
				break;
			} else {
				Error("Received encoded frame failed: err code %d", err);
				av_packet_free(&packet);
				m_d3dRender->GetContext()->Unmap(stagingTex.Get(), 0);
				return;
			}
		}
		//Debug("Received encoded packet");

		// Send encoded frame to client
		std::vector<uint8_t> encoded_data;
		if (m_isQsv && (packet->flags & AV_PKT_FLAG_KEY)) {
			// QSV emits SPS/PPS only on the first keyframe; cache them and
			// re-inject on every IDR so older decoders (e.g. the Go) can
			// reconfigure when ALVR requests a periodic keyframe.
			if (m_spsPpsCache.empty()) {
				cache_sps_pps(packet->data, packet->size);
			}
			if (!m_spsPpsCache.empty()) {
				std::vector<uint8_t> withHeader;
				withHeader.insert(withHeader.end(), m_spsPpsCache.begin(), m_spsPpsCache.end());
				withHeader.insert(withHeader.end(), packet->data, packet->data + packet->size);
				filter_NAL(withHeader.data(), withHeader.size(), encoded_data);
			} else {
				filter_NAL(packet->data, packet->size, encoded_data);
			}
		} else {
			filter_NAL(packet->data, packet->size, encoded_data);
		}
		m_Listener->SendVideo(encoded_data.data(), encoded_data.size(), packet->pts);
		av_packet_free(&packet);
		//Debug("Sent encoded packet to client");
	}

	// Send statistics to client
	m_Listener->GetStatistics()->EncodeOutput(GetTimestampUs() - presentationTime);

	// Unmap the copied texture and delete it
	m_d3dRender->GetContext()->Unmap(stagingTex.Get(), 0);
}

HRESULT VideoEncoderSW::SetupStagingTexture(ID3D11Texture2D *pTexture) {
	D3D11_TEXTURE2D_DESC desc;
	pTexture->GetDesc(&desc);
	stagingTexDesc.Width = desc.Width;
	stagingTexDesc.Height = desc.Height;
	stagingTexDesc.MipLevels = desc.MipLevels;
	stagingTexDesc.ArraySize = desc.ArraySize;
	stagingTexDesc.Format = desc.Format;
	stagingTexDesc.SampleDesc = desc.SampleDesc;
	stagingTexDesc.Usage = D3D11_USAGE_STAGING;
	stagingTexDesc.BindFlags = 0;
	stagingTexDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	stagingTexDesc.MiscFlags = 0;

	return m_d3dRender->GetDevice()->CreateTexture2D(&stagingTexDesc, nullptr, &stagingTex);
}

HRESULT VideoEncoderSW::CopyTexture(ID3D11Texture2D *pTexture) {
	m_d3dRender->GetContext()->CopyResource(stagingTex.Get(), pTexture);
	return m_d3dRender->GetContext()->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &stagingTexMap);
}

AVCodecID VideoEncoderSW::ToFFMPEGCodec(ALVR_CODEC codec) {
	switch (codec) {
		case ALVR_CODEC_H264:
			return AV_CODEC_ID_H264;
		case ALVR_CODEC_H265:
			return AV_CODEC_ID_HEVC;
		default:
			return AV_CODEC_ID_NONE;
	}
}

#endif // ALVR_GPL