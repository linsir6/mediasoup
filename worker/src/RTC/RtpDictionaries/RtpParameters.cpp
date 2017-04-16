#define MS_CLASS "RTC::RtpParameters"
// #define MS_LOG_DEV

#include "Logger.hpp"
#include "MediaSoupError.hpp"
#include "RTC/RtpDictionaries.hpp"
#include <unordered_set>

namespace RTC
{
	/* Instance methods. */

	RtpParameters::RtpParameters(Json::Value& data)
	{
		MS_TRACE();

		static const Json::StaticString JsonString_muxId("muxId");
		static const Json::StaticString JsonString_codecs("codecs");
		static const Json::StaticString JsonString_encodings("encodings");
		static const Json::StaticString JsonString_headerExtensions("headerExtensions");
		static const Json::StaticString JsonString_rtcp("rtcp");
		static const Json::StaticString JsonString_userParameters("userParameters");

		// `muxId` is optional.
		if (data[JsonString_muxId].isString())
		{
			this->muxId = data[JsonString_muxId].asString();

			if (this->muxId.empty())
				MS_THROW_ERROR("empty RtpParameters.muxId");
		}

		// `codecs` is mandatory.
		if (data[JsonString_codecs].isArray())
		{
			auto& jsonCodecs = data[JsonString_codecs];

			for (auto& jsonCodec : jsonCodecs)
			{
				RTC::RtpCodecParameters codec(jsonCodec, RTC::Scope::RECEIVE);

				// Append to the codecs vector.
				this->codecs.push_back(codec);
			}
		}
		else
		{
			MS_THROW_ERROR("missing RtpParameters.codecs");
		}

		// `encodings` is optional.
		if (data[JsonString_encodings].isArray())
		{
			auto& jsonArray = data[JsonString_encodings];

			for (auto& i : jsonArray)
			{
				RTC::RtpEncodingParameters encoding(i);

				// Append to the encodings vector.
				this->encodings.push_back(encoding);
			}
		}

		// `headerExtensions` is optional.
		if (data[JsonString_headerExtensions].isArray())
		{
			auto& jsonArray = data[JsonString_headerExtensions];

			for (auto& i : jsonArray)
			{
				RTC::RtpHeaderExtensionParameters headerExtension(i);

				// If a known header extension, append to the headerExtensions vector.
				if (headerExtension.type != RtpHeaderExtensionUri::Type::UNKNOWN)
					this->headerExtensions.push_back(headerExtension);
			}
		}

		// `rtcp` is optional.
		if (data[JsonString_rtcp].isObject())
		{
			this->rtcp    = RTC::RtcpParameters(data[JsonString_rtcp]);
			this->hasRtcp = true;
		}

		// `userParameters` is optional.
		if (data[JsonString_userParameters].isObject())
			this->userParameters = data[JsonString_userParameters];
		else
			this->userParameters = Json::objectValue;

		// Validate RTP parameters.
		ValidateCodecs();
		ValidateEncodings();
	}

	RtpParameters::RtpParameters(const RtpParameters* rtpParameters)
	    : muxId(rtpParameters->muxId), codecs(rtpParameters->codecs),
	      encodings(rtpParameters->encodings), headerExtensions(rtpParameters->headerExtensions),
	      rtcp(rtpParameters->rtcp), hasRtcp(rtpParameters->hasRtcp),
	      userParameters(rtpParameters->userParameters)
	{
		MS_TRACE();
	}

	Json::Value RtpParameters::toJson() const
	{
		MS_TRACE();

		static const Json::StaticString JsonString_muxId("muxId");
		static const Json::StaticString JsonString_codecs("codecs");
		static const Json::StaticString JsonString_encodings("encodings");
		static const Json::StaticString JsonString_headerExtensions("headerExtensions");
		static const Json::StaticString JsonString_rtcp("rtcp");
		static const Json::StaticString JsonString_userParameters("userParameters");

		Json::Value json(Json::objectValue);

		// Add `muxId`.
		if (!this->muxId.empty())
			json[JsonString_muxId] = this->muxId;

		// Add `codecs`.
		json[JsonString_codecs] = Json::arrayValue;

		for (auto& entry : this->codecs)
		{
			json[JsonString_codecs].append(entry.toJson());
		}

		// Add `encodings`.
		json[JsonString_encodings] = Json::arrayValue;

		for (auto& entry : this->encodings)
		{
			json[JsonString_encodings].append(entry.toJson());
		}

		// Add `headerExtensions`.
		json[JsonString_headerExtensions] = Json::arrayValue;

		for (auto& entry : this->headerExtensions)
		{
			json[JsonString_headerExtensions].append(entry.toJson());
		}

		// Add `rtcp`.
		if (this->hasRtcp)
			json[JsonString_rtcp] = this->rtcp.toJson();

		// Add `userParameters`.
		json[JsonString_userParameters] = this->userParameters;

		return json;
	}

	void RtpParameters::ReduceCodecsAndEncodings(RtpCapabilities& capabilities)
	{
		MS_TRACE();

		std::vector<uint8_t> removedCodecPayloadTypes;

		for (auto it = this->codecs.begin(); it != this->codecs.end();)
		{
			auto& codec = *it;
			auto it2    = capabilities.codecs.begin();

			for (; it2 != capabilities.codecs.end(); ++it2)
			{
				auto& codecCapability = *it2;

				if (codecCapability.Matches(codec, true))
				{
					// Once matched, remove the unsupported RTCP feedback from the given codec.
					codec.ReduceRtcpFeedback(codecCapability.rtcpFeedback);

					++it;
					break;
				}
			}
			if (it2 == capabilities.codecs.end())
			{
				MS_WARN_DEV(
				    "no matching peer codec capability found [payloadType:%" PRIu8 ", mime:%s]",
				    codec.payloadType,
				    codec.mime.GetName().c_str());

				removedCodecPayloadTypes.push_back(codec.payloadType);
				it = this->codecs.erase(it);
			}
		}

		// Remove encodings if associated to removed codecs.
		for (auto it = this->encodings.begin(); it != this->encodings.end();)
		{
			auto& encoding = *it;
			auto it2       = removedCodecPayloadTypes.begin();

			for (; it2 != removedCodecPayloadTypes.end(); ++it2)
			{
				auto removedCodecPayloadType = *it2;

				if (encoding.codecPayloadType == removedCodecPayloadType)
				{
					MS_WARN_DEV("removing encoding without matching codec");

					it = this->encodings.erase(it);
					break;
				}
			}
			if (it2 == removedCodecPayloadTypes.end())
			{
				++it;
			}
		}

		// Finally validate codecs and encodings.
		ValidateCodecs();
		ValidateEncodings();
	}

	void RtpParameters::ReduceHeaderExtensions(std::vector<RtpHeaderExtension>& supportedHeaderExtensions)
	{
		MS_TRACE();

		std::vector<RTC::RtpHeaderExtensionParameters> updatedHeaderExtensions;

		for (auto& headerExtension : this->headerExtensions)
		{
			for (auto& supportedHeaderExtension : supportedHeaderExtensions)
			{
				if (headerExtension.type == supportedHeaderExtension.type)
				{
					// Set the same id and other properties.
					headerExtension.id      = supportedHeaderExtension.preferredId;
					headerExtension.encrypt = supportedHeaderExtension.preferredEncrypt;

					updatedHeaderExtensions.push_back(headerExtension);

					break;
				}
			}
		}

		this->headerExtensions = updatedHeaderExtensions;
	}

	RTC::RtpCodecParameters& RtpParameters::GetCodecForEncoding(RtpEncodingParameters& encoding)
	{
		MS_TRACE();

		static RTC::RtpCodecParameters fakeCodec;
		uint8_t payloadType = encoding.codecPayloadType;

		auto it = this->codecs.begin();
		for (; it != this->codecs.end(); ++it)
		{
			auto& codec = *it;

			if (codec.payloadType == payloadType)
				return codec;
		}
		// This should never happen.
		if (it == this->codecs.end())
		{
			MS_ABORT("no valid codec payload type for the given encoding");
		}

		return fakeCodec;
	}

	inline void RtpParameters::ValidateCodecs()
	{
		MS_TRACE();

		static std::string JsonString_apt = "apt";

		// Must be at least one codec.
		if (this->codecs.empty())
			MS_THROW_ERROR("empty RtpParameters.codecs");

		std::unordered_set<uint8_t> payloadTypes;

		for (auto& codec : this->codecs)
		{
			if (payloadTypes.find(codec.payloadType) != payloadTypes.end())
				MS_THROW_ERROR("duplicated codec.payloadType");
			else
				payloadTypes.insert(codec.payloadType);

			switch (codec.mime.subtype)
			{
				// A RTX codec must have 'apt' parameter pointing to a non RTX codec.
				case RTC::RtpCodecMime::Subtype::RTX:
				{
					// NOTE: RtpCodecParameters already asserted that there is 'apt' parameter.
					int32_t apt = codec.parameters.GetInteger(JsonString_apt);
					auto it     = this->codecs.begin();

					for (; it != this->codecs.end(); ++it)
					{
						auto codec = *it;

						if ((int32_t)codec.payloadType == apt)
						{
							if (codec.mime.subtype == RTC::RtpCodecMime::Subtype::RTX)
								MS_THROW_ERROR("apt in RTX codec points to a RTX codec");
							else if (codec.mime.subtype == RTC::RtpCodecMime::Subtype::ULPFEC)
								MS_THROW_ERROR("apt in RTX codec points to a ULPFEC codec");
							else if (codec.mime.subtype == RTC::RtpCodecMime::Subtype::FLEXFEC)
								MS_THROW_ERROR("apt in RTX codec points to a FLEXFEC codec");
							else
								break;
						}
						if (it == this->codecs.end())
							MS_THROW_ERROR("apt in RTX codec points to a non existing codec");
					}

					break;
				}

				default:;
			}
		}
	}

	inline void RtpParameters::ValidateEncodings()
	{
		uint8_t firstMediaPayloadType;

		{
			auto it = this->codecs.begin();

			for (; it != this->codecs.end(); ++it)
			{
				auto& codec = *it;

				// Must be a media codec.
				if (codec.mime.IsMediaCodec())
				{
					firstMediaPayloadType = codec.payloadType;

					break;
				}
			}
			if (it == this->codecs.end())
				MS_THROW_ERROR("no media codecs found");
		}

		// If there are no encodings create one with `codecPayloadType` pointing to
		// the first media codec.
		if (this->encodings.empty())
		{
			RTC::RtpEncodingParameters encoding;

			encoding.codecPayloadType    = firstMediaPayloadType;
			encoding.hasCodecPayloadType = true;

			// Insert into the encodings vector.
			this->encodings.push_back(encoding);
		}
		// Otherwise iterate all the encodings, set the first payloadType in all of them
		// with `codecPayloadType` unset, and check that others point to a media codec.
		else
		{
			for (auto& encoding : this->encodings)
			{
				if (!encoding.hasCodecPayloadType)
				{
					encoding.codecPayloadType    = firstMediaPayloadType;
					encoding.hasCodecPayloadType = true;
				}
				else
				{
					auto it = this->codecs.begin();

					for (; it != this->codecs.end(); ++it)
					{
						auto codec = *it;

						if (codec.payloadType == encoding.codecPayloadType)
						{
							// Must be a media codec.
							if (codec.mime.IsMediaCodec())
								break;
							else
								MS_THROW_ERROR("invalid encoding.codecPayloadType");
						}
					}
					if (it == this->codecs.end())
						MS_THROW_ERROR("unknown encoding.codecPayloadType");
				}
			}
		}
	}
} // namespace RTC
