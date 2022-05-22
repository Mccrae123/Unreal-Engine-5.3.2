// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "WebRTCIncludes.h"

namespace UE::PixelStreaming 
{
	class FVideoEncoderVPX : public webrtc::VideoEncoder
	{
	public:
		FVideoEncoderVPX(int VPXVersion);
		virtual ~FVideoEncoderVPX() = default;

		// WebRTC Interface
		virtual int InitEncode(webrtc::VideoCodec const* codec_settings, webrtc::VideoEncoder::Settings const& settings) override;
		virtual int32 RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) override;
		virtual int32 Release() override;
		virtual int32 Encode(webrtc::VideoFrame const& frame, std::vector<webrtc::VideoFrameType> const* frame_types) override;
		virtual void SetRates(RateControlParameters const& parameters) override;
		virtual webrtc::VideoEncoder::EncoderInfo GetEncoderInfo() const override;

	private:
		std::unique_ptr<webrtc::VideoEncoder> WebRTCVPXEncoder;
	};
} // namespace UE::PixelStreaming
