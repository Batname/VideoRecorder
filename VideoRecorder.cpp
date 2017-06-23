#define VIDEO_RECORDER_IMPLEMENTATION
#include "VideoRecorder/include/VideoRecorder.h"
#include <iostream>
#include <locale>
#include <codecvt>
#include <filesystem>
#include <algorithm>
#include <iterator>
#include <new>
#include <cstdlib>
#include <cassert>
#include <cctype>
extern "C"
{
#	include <libavcodec/avcodec.h>
#	include <libswscale/swscale.h>
#	include <libavformat/avformat.h>
#	include <libavutil/imgutils.h>
#	include <libavutil/opt.h>
}
#include "DirectXTex.h"

using std::wclog;
using std::wcerr;
using std::endl;

static constexpr unsigned int cache_line = 64;	// for common x86 CPUs
static constexpr const char *const screenshotErrorMsgPrefix = "Fail to save screenshot \"";
static constexpr unsigned int lowFPS = 30, highFPS = 60;

typedef CVideoRecorder::CFrame::FrameData::Format FrameFormat;

static inline void AssertHR(HRESULT hr) noexcept
{
	assert(SUCCEEDED(hr));
}

static inline void CheckHR(HRESULT hr)
{
	if (FAILED(hr))
		throw hr;
}

static inline DXGI_FORMAT GetDXGIFormat(CVideoRecorder::CFrame::FrameData::Format format)
{
	switch (format)
	{
	case FrameFormat::B8G8R8A8:
		return DXGI_FORMAT_B8G8R8A8_UNORM;
	case FrameFormat::R10G10B10A2:
		return DXGI_FORMAT_R10G10B10A2_UNORM;
	default:
		assert(false);
		__assume(false);
	}
}

namespace
{
	const struct Init
	{
		Init()
		{
			av_register_all();
			avcodec_register_all();
		}
	} init;
}

static inline AVCodec *FindEncoder(CVideoRecorder::Codec codec)
{
	switch (codec)
	{
	case CVideoRecorder::Codec::H264:
		return avcodec_find_encoder(AV_CODEC_ID_H264);
	case CVideoRecorder::Codec::HEVC:
		return avcodec_find_encoder(AV_CODEC_ID_HEVC);
	default:
		throw "Invalid codec ID";
	}
}

inline void CVideoRecorder::ContextDeleter::operator()(AVCodecContext *context) const
{
	avcodec_free_context(&context);
}

void CVideoRecorder::FrameDeleter::operator()(AVFrame *frame) const
{
	av_frame_free(&frame);
}

inline void CVideoRecorder::OutputContextDeleter::operator()(AVFormatContext *output) const
{
	avformat_free_context(output);
}

inline const char *CVideoRecorder::EncodePreset_2_Str(Preset preset)
{
#	define ENCODE_PRESET_MAP_ENUM_2_STRING(entry)	\
		case Preset::entry:	return #entry;

	switch (preset)
	{
		GENERATE_ENCOE_PRESETS(ENCODE_PRESET_MAP_ENUM_2_STRING)
	default:
		return nullptr;
	}

#	undef ENCODE_PRESET_MAP_ENUM_2_STRING
}

inline char *CVideoRecorder::AVErrorString(int error)
{
	return av_make_error_string(avErrorBuf.get(), AV_ERROR_MAX_STRING_SIZE, error);
}

bool CVideoRecorder::Encode()
{
	int result = avcodec_send_frame(context.get(), dstFrame.get());
	assert(result == 0);
	if (result < 0)
	{
		wcerr << "Fail to " << (dstFrame ? "send frame to" : "flush") << " the encoder: " << AVErrorString(result) << '.' << endl;
		return false;
	}
	while ((result = avcodec_receive_packet(context.get(), packet.get())) == 0)
	{
		av_packet_rescale_ts(packet.get(), context->time_base, videoStream->time_base);
		packet->stream_index = videoStream->index;
		result = av_interleaved_write_frame(videoFile.get(), packet.get());
		assert(result == 0);
		av_packet_unref(packet.get());
		if (result < 0)
		{
			wcerr << "Fail to write video data to file: " << AVErrorString(result) << '.' << endl;
			return false;
		}
	}
	switch (result)
	{
	case AVERROR(EAGAIN):
	case AVERROR_EOF:
		return true;
	default:
		wcerr << "Fail to receive packet from the encoder: " << AVErrorString(result) << '.' << endl;
		return false;
	}
}

void CVideoRecorder::Cleanup()
{
	context.reset();
	dstFrame.reset();
	if (videoFile->pb)
		avio_closep(&videoFile->pb);
	videoFile.reset();
}

/*
	NOTE: exceptions related to mutex locks
		- aren't handled in worker thread which leads to terminate()
		- calls abort() in main thread
*/

[[noreturn]]
void CVideoRecorder::Error(const std::system_error &error)
{
	wcerr << "System error occured: " << error.what() << endl;
	abort();
}

void CVideoRecorder::Error(const std::exception &error, const char errorMsgPrefix[], const std::wstring *filename)
{
	try
	{
		// wait to establish character order for wcerr and to free memory
		std::unique_lock<decltype(mtx)> lck(mtx);
		workerEvent.wait(lck, [this] { return taskQueue.empty(); });
		wcerr << errorMsgPrefix;
		if (filename)
			wcerr << '\"' << filename << '\"';
		wcerr << ": " << error.what() << '.';
		switch (status)
		{
		case Status::OK:
			wcerr << " Try again...";
			break;
		case Status::CLEAN:
			if (videoFile)
			{
				Cleanup();
				taskQueue.clear();
			}
			break;
		}
		wcerr << endl;
	}
	catch (const std::system_error &error)
	{
		Error(error);
	}
}

static constexpr std::underlying_type<DirectX::WICCodecs>::type CODEC_DDS = 0xFFFF0001, CODEC_TGA = 0xFFFF0002;
static constexpr std::pair<const wchar_t *, DirectX::WICCodecs> pictureFormats[] =
{
	{ L".bmp",	DirectX::WIC_CODEC_BMP			},
	{ L".jpg",	DirectX::WIC_CODEC_JPEG			},
	{ L".jpeg",	DirectX::WIC_CODEC_JPEG			},
	{ L".png",	DirectX::WIC_CODEC_PNG			},
	{ L".tif",	DirectX::WIC_CODEC_TIFF			},
	{ L".tiff",	DirectX::WIC_CODEC_TIFF			},
	{ L".gif",	DirectX::WIC_CODEC_GIF			},
	{ L".hdp",	DirectX::WIC_CODEC_WMP			},
	{ L".jxr",	DirectX::WIC_CODEC_WMP			},
	{ L".wdp",	DirectX::WIC_CODEC_WMP			},
	{ L".ico",	DirectX::WIC_CODEC_ICO			},
	{ L".dds",	DirectX::WICCodecs(CODEC_DDS)	},
	{ L".tga",	DirectX::WICCodecs(CODEC_TGA)	},
};

static DirectX::WICCodecs GetScreenshotCodec(std::wstring &&ext)
{
	std::transform(ext.begin(), ext.end(), ext.begin(), tolower);
	const auto found = std::find_if(std::begin(pictureFormats), std::end(pictureFormats), [&ext](const std::remove_extent<decltype(pictureFormats)>::type &format)
	{
		return ext == format.first;
	});
	if (found == std::end(pictureFormats))
	{
		wcerr << "Unrecognized screenshot format \"" << ext << "\". Using \"tga\" as fallback." << endl;
		return DirectX::WICCodecs(CODEC_TGA);
	}
	else
		return found->second;
}

#pragma region Task
struct CVideoRecorder::ITask
{
	virtual void operator ()(CVideoRecorder &parent) = 0;
	virtual operator bool() const { return true; }	// is task ready to handle
	virtual ~ITask() noexcept = default;
};

#pragma region CFrameTask
class CVideoRecorder::CFrameTask final : public ITask
{
	friend void CFrame::Cancel();

private:
	std::shared_ptr<CFrame> srcFrame;

public:
	CFrameTask(std::shared_ptr<CFrame> &&frame) noexcept : srcFrame(std::move(frame)) { assert(srcFrame); }
	CFrameTask(CFrameTask &&) noexcept = default;

public:
	void operator ()(CVideoRecorder &parent) override;
	operator bool() const override { return srcFrame->ready; }
};
#pragma endregion

#pragma region CStartVideoRecordRequest
class CVideoRecorder::CStartVideoRecordRequest final : public ITask
{
	const std::wstring filename;
	const unsigned int width, height;
	const Codec codecID;
	const int64_t crf;
	const Preset preset;
	const bool _10bit, highFPS;
	const bool matchedStop;

public:
	const std::wstring &GetFilename() const noexcept { return filename; }

public:
	CStartVideoRecordRequest(std::wstring &&filename, unsigned int width, unsigned int height, bool _10bit, bool highFPS, Codec codec, int64_t crf, Preset preset, bool matchedStop) noexcept :
		filename(std::move(filename)), width(width), height(height),
		_10bit(_10bit), highFPS(highFPS), codecID(codec), crf(crf), preset(preset), matchedStop(matchedStop) {}
	CStartVideoRecordRequest(CStartVideoRecordRequest &&) noexcept = default;

public:
	void operator ()(CVideoRecorder &parent) override;
};
#pragma endregion

#pragma region CStopVideoRecordRequest
class CVideoRecorder::CStopVideoRecordRequest final : public ITask
{
	const bool matchedStart;

public:
	CStopVideoRecordRequest(bool matchedStart) noexcept : matchedStart(matchedStart) {}
	CStopVideoRecordRequest(CStopVideoRecordRequest &&) noexcept = default;

public:
	void operator ()(CVideoRecorder &parent) override;
};
#pragma endregion

void CVideoRecorder::CFrameTask::operator ()(CVideoRecorder &parent)
{
	using namespace DirectX;

	auto srcFrameData = srcFrame->GetFrameData();
	if (!srcFrameData.pixels)
	{
		wcerr << "Invalid frame occured. Skipping it." << endl;
		return;
	}

	while (!srcFrame->screenshotPaths.empty())
	{
		wclog << "Saving screenshot \"" << srcFrame->screenshotPaths.front() << "\"..." << endl;

		try
		{
			std::tr2::sys::path screenshotPath(srcFrame->screenshotPaths.front());
			const auto screenshotCodec = GetScreenshotCodec(screenshotPath.extension());

			const Image image =
			{
				srcFrameData.width, srcFrameData.height, GetDXGIFormat(srcFrameData.format),
				srcFrameData.stride, srcFrameData.stride * srcFrameData.height, const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(srcFrameData.pixels))
			};

			switch (screenshotCodec)
			{
			case CODEC_DDS:
				CheckHR(SaveToDDSFile(image, DDS_FLAGS_NONE, srcFrame->screenshotPaths.front().c_str()));
				break;
			case CODEC_TGA:
				CheckHR(SaveToTGAFile(image, srcFrame->screenshotPaths.front().c_str()));
				break;
			default:
				CheckHR(SaveToWICFile(image, WIC_FLAGS_NONE, GetWICCodec(screenshotCodec), srcFrame->screenshotPaths.front().c_str()));
				break;
			}

			wclog << "Screenshot \"" << srcFrame->screenshotPaths.front() << "\" has been saved." << endl;
		}
		catch (HRESULT hr)
		{
			wcerr << screenshotErrorMsgPrefix << srcFrame->screenshotPaths.front() << "\" (hr=" << hr << ")." << endl;
		}
		catch (const std::exception &error)
		{
			wcerr << screenshotErrorMsgPrefix << srcFrame->screenshotPaths.front() << ": " << error.what() << '.' << endl;
		}

		srcFrame->screenshotPaths.pop();
	}

	if (srcFrame->videoPendingFrames && parent.videoFile)
	{
		static constexpr char convertErrorMsgPrefix[] = "Fail to convert frame for video";
		av_init_packet(parent.packet.get());
		parent.packet->data = NULL;
		parent.packet->size = 0;

		AVPixelFormat srcVideoFormat = AV_PIX_FMT_BGRA;
		ScratchImage convertedImage;
		switch (srcFrameData.format)
		{
		case FrameFormat::R10G10B10A2:
		{
			const Image srcImage =
			{
				srcFrameData.width, srcFrameData.height, GetDXGIFormat(srcFrameData.format),
				srcFrameData.stride, srcFrameData.stride * srcFrameData.height, const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(srcFrameData.pixels))
			};
			const auto intermediateDXFormat = parent.context->pix_fmt == AV_PIX_FMT_YUV420P10 ? (srcVideoFormat = AV_PIX_FMT_RGBA64, DXGI_FORMAT_R16G16B16A16_UNORM) : DXGI_FORMAT_B8G8R8A8_UNORM;
			const HRESULT hr = Convert(srcImage, intermediateDXFormat, TEX_FILTER_DEFAULT, .5f, convertedImage);
			if (FAILED(hr))
			{
				wcerr << convertErrorMsgPrefix << " (hr=" << hr << ")." << endl;
				parent.Cleanup();
				return;
			}
			const auto resultImage = convertedImage.GetImage(0, 0, 0);
			srcFrameData.stride = resultImage->rowPitch;
			srcFrameData.pixels = resultImage->pixels;
			break;
		}
		}

		parent.cvtCtx.reset(sws_getCachedContext(parent.cvtCtx.release(),
			srcFrameData.width, srcFrameData.height, srcVideoFormat,
			parent.context->width, parent.context->height, parent.context->pix_fmt,
			SWS_BILINEAR, NULL, NULL, NULL));
		assert(parent.cvtCtx);
		if (!parent.cvtCtx)
		{
			wcerr << convertErrorMsgPrefix << '.' << endl;
			parent.Cleanup();
			return;
		}
		const int srcStride = srcFrameData.stride;
		sws_scale(parent.cvtCtx.get(), reinterpret_cast<const uint8_t *const*>(&srcFrameData.pixels), &srcStride, 0, srcFrameData.height, parent.dstFrame->data, parent.dstFrame->linesize);
		convertedImage.Release();

		do
		{
			const int result = av_frame_make_writable(parent.dstFrame.get());
			assert(result == 0);
			if (result < 0)
			{
				wcerr << "Fail to prepare video frame for writing: " << parent.AVErrorString(result) << '.' << endl;
				parent.Cleanup();
				return;
			}
			if (!parent.Encode())
			{
				parent.Cleanup();
				return;
			}
			parent.dstFrame->pts++;
		} while (--srcFrame->videoPendingFrames);
	}
}

void CVideoRecorder::CStartVideoRecordRequest::operator ()(CVideoRecorder &parent)
{
	if (!matchedStop)
		wcerr << "Starting new video record session without stopping previouse one." << endl;

	if (parent.videoFile)
	{
		CStopVideoRecordRequest stopRecord(true);
		stopRecord(parent);
	}

	try
	{
		const AVCodec *const codec = FindEncoder(codecID);
		if (!codec)
			throw "Fail to find codec";

		parent.context.reset(avcodec_alloc_context3(codec));
		if (!parent.context)
		{
			wcerr << "Fail to init codec for video \"" << filename << "\"." << endl;
			return;
		}

		parent.context->width = width & ~1;
		parent.context->height = height & ~1;
		parent.context->coded_width = parent.context->coded_height = 0;
		parent.context->time_base = { 1, highFPS ? ::highFPS : ::lowFPS };
		parent.context->pix_fmt = AV_PIX_FMT_YUV420P;
		if (const auto availableThreads = std::thread::hardware_concurrency())
			parent.context->thread_count = availableThreads;	// TODO: consider reserving 1 or more threads for other stuff

		if (crf != -1)
		{
			const int result = av_opt_set_int(parent.context.get(), "crf", crf, AV_OPT_SEARCH_CHILDREN);
			assert(result == 0);
			if (result < 0)
				wcerr << "Fail to set crf for video \"" << filename << "\": " << parent.AVErrorString(result) << '.' << endl;
		}

		if (preset != Preset::Default)
		{
			if (const char *const presetStr = EncodePreset_2_Str(preset))
			{
				const int result = av_opt_set(parent.context->priv_data, "preset", presetStr, 0);
				assert(result == 0);
				if (result < 0)
					wcerr << "Fail to set preset preset for video \"" << filename << "\": " << parent.AVErrorString(result) << '.' << endl;
			}
			else
				wcerr << "Invalid encode preset value for video \"" << filename << "\"." << endl;
		}

		wclog << "Recording video \"" << filename << "\" (using " << parent.context->thread_count << " threads for encoding)..." << endl;

		{
			const int result = avcodec_open2(parent.context.get(), codec, NULL);
			assert(result == 0);
			if (result != 0)
			{
				wcerr << "Fail to open codec for video \"" << filename << "\": " << parent.AVErrorString(result) << '.' << endl;
				parent.Cleanup();
				return;
			}
		}

		parent.dstFrame.reset(av_frame_alloc());
		assert(parent.dstFrame);
		if (!parent.dstFrame)
		{
			wcerr << "Fail to allocate frame for video \"" << filename << "\"." << endl;
			parent.Cleanup();
			return;
		}

		parent.dstFrame->format = _10bit ? AV_PIX_FMT_YUV420P10 : AV_PIX_FMT_YUV420P;
		parent.dstFrame->width = parent.context->width;
		parent.dstFrame->height = parent.context->height;
		parent.dstFrame->pts = 0;

		{
			const int result = av_frame_get_buffer(parent.dstFrame.get(), cache_line);
			assert(result >= 0);
			if (result < 0)
			{
				wcerr << "Fail to allocate frame data for video \"" << filename << "\": " << parent.AVErrorString(result) << '.' << endl;
				parent.Cleanup();
				return;
			}
		}

		// NOTE: exception during string conversion will lead to process termination since it thrown from worker thread
		const std::string convertedFilename = std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(filename);

		{
			AVFormatContext *output;
			const int result = avformat_alloc_output_context2(&output, NULL, NULL, convertedFilename.c_str());
			assert(result >= 0);
			if (result < 0)
			{
				wcerr << "Fail to init output context for video file \"" << filename << "\": " << parent.AVErrorString(result) << '.' << endl;
				parent.Cleanup();
				return;
			}
			parent.videoFile.reset(output);
		}

		parent.videoStream = avformat_new_stream(parent.videoFile.get(), codec);
		assert(parent.videoStream);
		if (!parent.videoStream)
		{
			wcerr << "Fail to add video stream for file \"" << filename << "\"." << endl;
			parent.Cleanup();
			return;
		}

		{
			const int result = avcodec_parameters_from_context(parent.videoStream->codecpar, parent.context.get());
			assert(result >= 0);
			if (result < 0)
			{
				wcerr << "Fail to extract codec parameters for video file \"" << filename << "\":" << parent.AVErrorString(result) << '.' << endl;
				parent.Cleanup();
				return;
			}
		}

		parent.videoStream->time_base = parent.context->time_base;

		{
			const int result = avio_open(&parent.videoFile->pb, convertedFilename.c_str(), AVIO_FLAG_WRITE);
			assert(result >= 0);
			if (result < 0)
			{
				wcerr << "Fail to create video file \"" << filename << "\":" << parent.AVErrorString(result) << '.' << endl;
				parent.Cleanup();
				return;
			}
		}

		{
			const int result = avformat_write_header(parent.videoFile.get(), NULL);
			assert(result == AVSTREAM_INIT_IN_WRITE_HEADER);
			if (result < 0)
			{
				wcerr << "Fail to write header for video file \"" << filename << "\":" << parent.AVErrorString(result) << '.' << endl;
				parent.Cleanup();
				return;
			}
		}
	}
	catch (const char error[])
	{
		wcerr << error << " for video \"" << filename << "\"." << endl;
	}
}

void CVideoRecorder::CStopVideoRecordRequest::operator ()(CVideoRecorder &parent)
{
	if (!matchedStart)
		wcerr << "Stopping video record without matched start." << endl;

	if (!parent.videoFile)
		return;

	bool ok = parent.Encode();

	int result = av_write_trailer(parent.videoFile.get());
	assert(result == 0);
	if (result < 0)
	{
		wcerr << "Fail to write video stream trailer: " << parent.AVErrorString(result) << '.' << endl;
		ok = false;
	}

	result = avio_closep(&parent.videoFile->pb);
	assert(result == 0);
	if (result < 0)
	{
		wcerr << "Fail to flush trailing video data to file: " << parent.AVErrorString(result) << '.' << endl;
		ok = false;
	}

	parent.Cleanup();

	if (ok)
		wclog << "Video has been recorded." << endl;
	else
		wcerr << "Fail to record video." << endl;
}
#pragma endregion

#pragma region CFrame
CVideoRecorder::CFrame::CFrame(Opaque opaque) :
	parent				(std::get<0>(std::move(opaque))),
	screenshotPaths		(std::get<1>(std::move(opaque))),
	videoPendingFrames	(std::get<2>(std::move(opaque)))
{}

void CVideoRecorder::CFrame::Ready()
{
	try
	{
		std::lock_guard<decltype(mtx)> lck(parent.mtx);
		ready = true;
		parent.workerEvent.notify_all();
	}
	catch (const std::system_error &error)
	{
		parent.Error(error);
	}
}

void CVideoRecorder::CFrame::Cancel()
{
	try
	{
		std::lock_guard<decltype(mtx)> lck(parent.mtx);
		/*
			remove_if always traverses all the range
			find_if/erase pair allows to stop traverse after element being removed was found

			no exception should be thrown during erase() because std::unique_ptr has noexcept move ctor/assignment
		*/
		const auto pred = [this](decltype(parent.taskQueue)::const_reference task)
		{
			if (const CFrameTask *frameTask = dynamic_cast<const CFrameTask *>(task.get()))
				return frameTask->srcFrame.get() == this;
			return false;
		};
		const auto taskToDelete = std::find_if(parent.taskQueue.cbegin(), parent.taskQueue.cend(), pred);
		if (taskToDelete != parent.taskQueue.cend())
		{
			parent.taskQueue.erase(taskToDelete);
			assert(std::find_if(parent.taskQueue.cbegin(), parent.taskQueue.cend(), pred) == parent.taskQueue.cend());
		}
		parent.workerEvent.notify_all();
	}
	catch (const std::system_error &error)
	{
		parent.Error(error);
	}
}
#pragma endregion

void CVideoRecorder::Process()
{
	std::unique_lock<decltype(mtx)> lck(mtx);
	while (!finish)
	{
		if (taskQueue.empty() || !*taskQueue.front())
		{
			workerEvent.notify_one();
			workerEvent.wait(lck);
		}
		else
		{
			auto task = std::move(taskQueue.front());
			taskQueue.pop_front();
			lck.unlock();
			task->operator ()(*this);
			lck.lock();
		}
	}
}

CVideoRecorder::CVideoRecorder() try :
	avErrorBuf(std::make_unique<char []>(AV_ERROR_MAX_STRING_SIZE)),
	cvtCtx(nullptr, sws_freeContext),
	packet(std::make_unique<decltype(packet)::element_type>()),
	worker(std::mem_fn(&CVideoRecorder::Process), this)
{
}
catch (const std::exception &error)
{
	wcerr << "Fail to init video recorder: " << error.what() << '.' << endl;
}

/*
	make it external in order to allow for forward decl for std::unique_ptr
	declaring move ctor also disables copy ctor/assignment which is desired
*/
#if 0
CVideoRecorder::CVideoRecorder(CVideoRecorder &&) = default;
#endif

CVideoRecorder::~CVideoRecorder()
{
	try
	{
		if (recordMode != RecordMode::STOPPED)
		{
			// wait to establish character order for wcerr
			std::unique_lock<decltype(mtx)> lck(mtx);
			workerEvent.wait(lck, [this] { return taskQueue.empty(); });
			wcerr << "Destroying video recorder without stopping current record session." << endl;
			lck.unlock();
			StopRecord();
		}

		{
			std::unique_lock<decltype(mtx)> lck(mtx);
			workerEvent.wait(lck, [this] { return taskQueue.empty(); });

			finish = true;
			workerEvent.notify_all();
		}

		worker.join();
	}
	catch (const std::system_error &error)
	{
		Error(error);
	}
}

// 1 call site
template<unsigned int FPS>
inline void CVideoRecorder::AdvanceFrame(clock::time_point now, decltype(CFrame::videoPendingFrames) &videoPendingFrames)
{
	using std::chrono::duration_cast;
	const auto delta = duration_cast<FrameDuration<FPS>>(now - nextFrame) + FrameDuration<FPS>(1u);
	nextFrame += duration_cast<clock::duration>(delta);
	videoPendingFrames = delta.count();
}

void CVideoRecorder::SampleFrame(const std::function<std::shared_ptr<CFrame> (CFrame::Opaque)> &RequestFrameCallback)
{
	decltype(CFrame::videoPendingFrames) videoPendingFrames = 0;

	const auto nextFrameBackup = nextFrame;
	if (recordMode != RecordMode::STOPPED)
	{
		const auto now = clock::now();
		if (now >= nextFrame)
		{
			switch (recordMode)
			{
			case RecordMode::LOW_FPS:
				AdvanceFrame<lowFPS>(now, videoPendingFrames);
				break;
			case RecordMode::HIGH_FPS:
				AdvanceFrame<highFPS>(now, videoPendingFrames);
				break;
			}
		}
	}

	if (videoPendingFrames || !screenshotPaths.empty())
	{
		try
		{
			auto task = std::make_unique<CFrameTask>(RequestFrameCallback(std::make_tuple(std::ref(*this), std::move(screenshotPaths), std::move(videoPendingFrames))));
			std::lock_guard<decltype(mtx)> lck(mtx);
			taskQueue.push_back(std::move(task));
			workerEvent.notify_all();
		}
		catch (const std::system_error &error)
		{
			Error(error);
		}
		catch (const std::exception &error)
		{
			nextFrame = nextFrameBackup;
			Error(error, "Fail to sample frame");
			if (status == Status::OK)
			{
				status = Status::RETRY;
				SampleFrame(RequestFrameCallback);
				status = Status::OK;
			}
		}
	}
}

// CStartVideoRecordRequest steals (moves) filename during construction => can not reuse filename during retry => reuse task instead (if it was created successfully)
void CVideoRecorder::StartRecordImpl(std::wstring filename, unsigned int width, unsigned int height, bool _10bit, bool highFPS, Codec codec, int64_t crf, Preset preset, std::unique_ptr<CStartVideoRecordRequest> &&task)
{
	try
	{
		if (!task)
			task.reset(new CStartVideoRecordRequest(std::move(filename), width, height, _10bit, highFPS, codec, crf, preset, recordMode == RecordMode::STOPPED));
		std::lock_guard<decltype(mtx)> lck(mtx);
		taskQueue.push_back(std::move(task));
		workerEvent.notify_all();
		recordMode = highFPS ? RecordMode::HIGH_FPS : RecordMode::LOW_FPS;
		nextFrame = clock::now();
	}
	catch (const std::system_error &error)
	{
		Error(error);
	}
	catch (const std::exception &error)
	{
		Error(error, "Fail to start video record ", &(task ? task->GetFilename() : filename));
		if (status == Status::OK)
		{
			status = Status::RETRY;
			StartRecordImpl(std::move(filename), width, height, _10bit, highFPS, codec, crf, preset, std::move(task));
			status = Status::OK;
		}
	}
}

void CVideoRecorder::StartRecord(std::wstring filename, unsigned int width, unsigned int height, bool _10bit, bool highFPS, Codec codec, int64_t crf, Preset preset)
{
	StartRecordImpl(std::move(filename), width, height, _10bit, highFPS, codec, crf, preset);
}

void CVideoRecorder::StopRecord()
{
	try
	{
		auto task = std::make_unique<CStopVideoRecordRequest>(recordMode != RecordMode::STOPPED);
		std::lock_guard<decltype(mtx)> lck(mtx);
		taskQueue.push_back(std::move(task));
		workerEvent.notify_all();
		recordMode = RecordMode::STOPPED;
	}
	catch (const std::system_error &error)
	{
		Error(error);
	}
	catch (const std::exception &error)
	{
		Error(error, "Fail to stop video record");
		if (status == Status::OK)
		{
			status = Status::CLEAN;
			StopRecord();
			status = Status::OK;
		}
	}
}

void CVideoRecorder::Screenshot(std::wstring filename)
{
	try
	{
		screenshotPaths.push(std::move(filename));
	}
	// locks does not happen here => no need to catch 'std::system_error'
	catch (const std::exception &error)
	{
		Error(error, screenshotErrorMsgPrefix, &filename);
		if (status == Status::OK)
		{
			status = Status::RETRY;
			Screenshot(std::move(filename));
			status = Status::OK;
		}
	}
}