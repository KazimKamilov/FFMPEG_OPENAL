#include <iostream>
#include <vector>
#include <chrono>
#include <sstream>

#include "AL/al.h"
#include "AL/alc.h"

extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"
#include "libswresample/swresample.h"
}

#define TARGET_RESAMPLING_FORMAT AV_SAMPLE_FMT_S16
#define RESAMPLE_TO_MONO
#define FROM_MEMORY

struct SoundData final
{
	std::vector<uint8_t> buffer;
	int sample_rate{ 0 };
	int channels{ 0 };
};

void format_av_error(int ret)
{
	// Only want to trigger this on unhandable errors
	if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
	{
		char errbuff[1028];
		av_strerror(ret, errbuff, 1028);
		fprintf(stderr, "Error message (%d): %s\n", ret, errbuff);
		exit(ret);
	}
}

void format_av_error(void* pointer, const char* error_msg)
{
	if (pointer == nullptr)
	{
		fprintf(stderr, "%s\n", error_msg);
		exit(-1);
	}
}

std::vector<uint8_t> FFMPEG_decode(AVCodecContext* pCodecContext, AVFormatContext* pFormatContext, SwrContext* pResamplerContext)
{
	int error_result{ 0 };
	uint8_t* pBufferData{ nullptr };
	int line_size{ 0 };
	int last_nb_samples{ 0 };
	bool is_eof{ false };
	std::vector<uint8_t> buffer;

	auto packet{ av_packet_alloc() };
	auto frame{ av_frame_alloc() };

	error_result = av_read_frame(pFormatContext, packet);
	format_av_error(error_result);

	error_result = avcodec_send_packet(pCodecContext, packet);
	format_av_error(error_result);

	error_result = 0;

	while (error_result >= 0 && !is_eof)
	{
		do
		{
			if (error_result == AVERROR(EAGAIN))
			{
				error_result = av_read_frame(pFormatContext, packet);

				format_av_error(error_result);

				if (error_result == AVERROR_EOF)
				{
					is_eof = true;
					break;
				}

				error_result = avcodec_send_packet(pCodecContext, packet);

				format_av_error(error_result);
			}

			error_result = avcodec_receive_frame(pCodecContext, frame);
		}
		while (error_result == AVERROR(EAGAIN));

		if (is_eof)
			break;

		if (frame->nb_samples != last_nb_samples)
		{
			error_result = av_samples_alloc(
				&pBufferData, &line_size,
#ifdef RESAMPLE_TO_MONO
				1,
#else
				frame->channels,
#endif
				frame->nb_samples, TARGET_RESAMPLING_FORMAT, 0);

			last_nb_samples = frame->nb_samples;
		}

		format_av_error(error_result);

		error_result = swr_convert(pResamplerContext, &pBufferData, frame->nb_samples,
			const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);

		format_av_error(error_result);

		if ((error_result == AVERROR(EAGAIN)) || (error_result == AVERROR_EOF))
			break;
		else if (error_result < 0)
			format_av_error(error_result);

		buffer.insert(buffer.cend(), pBufferData, pBufferData + static_cast<size_t>(line_size));
	}

	return buffer;
}

static int ReadCallback(void* user_data, uint8_t* data_ptr, int data_size)
{
	auto file{ static_cast<FILE*>(user_data) };

	if (feof(file) != 0)
		return AVERROR_EOF;

	return static_cast<int>(fread(data_ptr, sizeof(uint8_t), data_size, file));
}

static int64_t SeekCallback(void* user_data, int64_t offset, int origin)
{
	auto file{ static_cast<FILE*>(user_data) };

	// If EOF
	if (origin == 0x10000)
		return -1;

	return static_cast<int64_t>(fseek(file, static_cast<long>(offset), origin));
}

SoundData read_audio_into_buffer(const char* filename)
{
	av_log_set_level(AV_LOG_INFO);

	AVFormatContext* pFormatContext{ nullptr };
	AVCodecParameters* pCodecParams{ nullptr };

	int error_result{ 0 };

#ifdef FROM_MEMORY
	auto file{ fopen(filename, "rb") };

	constexpr size_t BUFFER_SIZE{ 4096u };
	auto data_ptr{ static_cast<uint8_t*>(av_malloc(BUFFER_SIZE)) };

	auto pInputContext{ avio_alloc_context(data_ptr, BUFFER_SIZE, 0, file, ReadCallback, nullptr, SeekCallback) };

	format_av_error(pInputContext, "Cannot allocate FFMPEG I/O context!");

	pFormatContext = avformat_alloc_context();

	pFormatContext->pb = pInputContext;
	pFormatContext->flags |= AVFMT_FLAG_CUSTOM_IO;

	error_result = avformat_open_input(&pFormatContext, "", nullptr, nullptr);
#else
	error_result = avformat_open_input(&pFormatContext, filename, nullptr, nullptr);
#endif

	format_av_error(error_result);

	error_result = avformat_find_stream_info(pFormatContext, nullptr);
	format_av_error(error_result);

	// Find video stream
	int video_stream_idx{ -1 };

	for (unsigned int i = 0u; i < pFormatContext->nb_streams; ++i)
	{
		const auto pStream{ pFormatContext->streams[i] };

		if (pStream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			video_stream_idx = i;
			break;
		}
	}

	if (video_stream_idx == -1)
		format_av_error(nullptr, "FFMPEG could not find any audio stream!");

	const auto pStream{ pFormatContext->streams[video_stream_idx] };
	pCodecParams = pStream->codecpar;

	format_av_error(pCodecParams, "FFMPEG could not find audio stream!");

	const auto pCodec{ avcodec_find_decoder(pCodecParams->codec_id) };
	format_av_error(pCodec, "FFMPEG could not find target audio codec!");

	auto pCodecContext{ avcodec_alloc_context3(pCodec) };
	format_av_error(pCodecContext, "FFMPEG could not alloc target audio codec!");

	avcodec_parameters_to_context(pCodecContext, pCodecParams);

	error_result = avcodec_open2(pCodecContext, pCodec, nullptr);
	format_av_error(error_result);

	auto pResampler{ swr_alloc_set_opts(nullptr,
#ifdef RESAMPLE_TO_MONO
		AV_CH_LAYOUT_MONO,
#else
		AV_CH_LAYOUT_STEREO,
#endif
		TARGET_RESAMPLING_FORMAT,
		pCodecParams->sample_rate, pCodecParams->channel_layout,
		static_cast<AVSampleFormat>(pCodecParams->format),
		pCodecParams->sample_rate, 0, nullptr) };

	swr_init(pResampler);

	format_av_error(pResampler, "Something went wrong with FFMPEG allocating audio resample context!");

	SoundData sound_data;
	sound_data.buffer = FFMPEG_decode(pCodecContext, pFormatContext, pResampler);
	sound_data.sample_rate = pCodecParams->sample_rate;
	//sound_data.format = av_get_sample_fmt_name(TARGET_FORMAT);
#ifdef RESAMPLE_TO_MONO
	sound_data.channels = 1;
#else
	sound_data.channels = pCodecParams->channels;
#endif

#ifdef FROM_MEMORY
	avio_context_free(&pInputContext);
	fclose(file);
#endif

	swr_close(pResampler);
	avformat_close_input(&pFormatContext);
	avcodec_free_context(&pCodecContext);
	avformat_free_context(pFormatContext);

	return sound_data;
}

static void sleep(int64_t msecs)
{
	bool sleep{ true };
	auto start_time{ std::chrono::system_clock::now() };

	while (sleep)
	{
		auto current_time{ std::chrono::system_clock::now() };
		auto elapsed_time{ std::chrono::duration_cast<std::chrono::milliseconds>(current_time - start_time) };

		if (elapsed_time.count() > msecs)
			sleep = false;
	}
}

int main()
{
	auto pDevice{ alcOpenDevice(nullptr) };

	if (pDevice)
	{
		auto pContext{ alcCreateContext(pDevice, nullptr) };
		if (pContext)
		{
			std::cout << "OpenAL device opened: " << alcGetString(pDevice, ALC_DEVICE_SPECIFIER) << std::endl;
			alcMakeContextCurrent(pContext);
		}
		else
		{
			std::cout << "Cannot create OpenAL context! Exit..." << std::endl;

			alcCloseDevice(pDevice);

			return 1;
		}
	}
	else
	{
		std::cout << "Cannot create OpenAL device! Exit..." << std::endl;
		return 1;
	}

	const auto& sound_data{ read_audio_into_buffer("test.ogg") };

	ALuint al_buffer{ 0u };
	ALuint al_source{ 0u };
	ALenum format{ 0 };
	ALint state{ 0 };

	alGenBuffers(1, &al_buffer);

	if (sound_data.channels > 1)
		format = AL_FORMAT_STEREO16;
	else if (sound_data.channels == 1)
		format = AL_FORMAT_MONO16;

	alBufferData(al_buffer, format, sound_data.buffer.data(), static_cast<ALsizei>(sound_data.buffer.size()), sound_data.sample_rate);

	alGenSources(1, &al_source);
	alSourcei(al_source, AL_BUFFER, al_buffer);

	alSourcePlay(al_source);

	std::cout << "Playing source..." << std::endl;

	do
	{
		sleep(1000);
		alGetSourcei(al_source, AL_SOURCE_STATE, &state);
	} while (state == AL_PLAYING);

	std::cout << "Done!" << std::endl;

	alSourceStop(al_source);

	alDeleteSources(1, &al_source);
	alDeleteBuffers(1, &al_buffer);

	auto pContext{ alcGetCurrentContext() };

	alcMakeContextCurrent(nullptr);
	alcDestroyContext(pContext);
	alcCloseDevice(pDevice);

	return 0;
}
