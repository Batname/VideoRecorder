#pragma once

#include <vector>
#include <string>
#include <queue>
#include <deque>
#include <memory>
#include <utility>
#include <tuple>
#include <type_traits>
#include <functional>
#include <chrono>
#include <ratio>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <exception>
#include <system_error>
#include <cstdint>

class CVideoRecorder
{
	static const struct AVCodec *const codec;

	std::unique_ptr<char []> avErrorBuf;

	struct ContextDeleter
	{
		inline void operator ()(struct AVCodecContext *context) const;
	};
	std::unique_ptr<struct AVCodecContext, ContextDeleter> context;

	std::unique_ptr<struct SwsContext, void (*const)(struct SwsContext *swsContext)> cvtCtx;

	const std::unique_ptr<struct AVPacket> packet;

	struct FrameDeleter
	{
		void operator ()(struct AVFrame *frame) const;
	};
	std::unique_ptr<struct AVFrame, FrameDeleter> dstFrame;

	typedef std::chrono::steady_clock clock;
	template<unsigned int FPS>
	using FrameDuration = std::chrono::duration<clock::rep, std::ratio<1, FPS>>;
	clock::time_point nextFrame;

	struct OutputContextDeleter
	{
		inline void operator ()(struct AVFormatContext *output) const;
	};
	std::unique_ptr<struct AVFormatContext, OutputContextDeleter> videoFile;

	struct AVStream *videoStream;

	std::queue<std::wstring> screenshotPaths;

	struct ITask;
	class CFrameTask;
	class CStartVideoRecordRequest;
	class CStopVideoRecordRequest;
	std::deque<std::unique_ptr<ITask>> taskQueue;

	bool finish = false;
	std::mutex mtx;
	std::condition_variable workerEvent;
	std::thread worker;

	enum class Status : uint_least8_t
	{
		OK,
		RETRY,
		CLEAN,
	} status = Status::OK;

	enum class RecordMode
	{
		STOPPED,
		LOW_FPS,
		HIGH_FPS,
	} recordMode = RecordMode::STOPPED;

public:
#	define GENERATE_ENCOE_PERFORMANCE_MODE(template, performance) template(performance)
#	define GENERATE_ENCOE_PERFORMANCE_MODES(template)			\
		GENERATE_ENCOE_PERFORMANCE_MODE(template, placebo)		\
		GENERATE_ENCOE_PERFORMANCE_MODE(template, veryslow)		\
		GENERATE_ENCOE_PERFORMANCE_MODE(template, slower)		\
		GENERATE_ENCOE_PERFORMANCE_MODE(template, slow)			\
		GENERATE_ENCOE_PERFORMANCE_MODE(template, medium)		\
		GENERATE_ENCOE_PERFORMANCE_MODE(template, fast)			\
		GENERATE_ENCOE_PERFORMANCE_MODE(template, faster)		\
		GENERATE_ENCOE_PERFORMANCE_MODE(template, veryfast)		\
		GENERATE_ENCOE_PERFORMANCE_MODE(template, superfast)	\
		GENERATE_ENCOE_PERFORMANCE_MODE(template, ultrafast)
#	define ENCOE_PERFORMANCE_ENUM_ENTRY(entry) entry,
	struct EncodeConfig
	{
		int64_t crf;
		enum class Performance
		{
			GENERATE_ENCOE_PERFORMANCE_MODES(ENCOE_PERFORMANCE_ENUM_ENTRY)
		} performance;
	};
#	ifndef VIDEO_RECORDER_IMPLEMENTATION
#		undef GENERATE_ENCOE_PERFORMANCE_MODE
#		undef GENERATE_ENCOE_PERFORMANCE_MODES
#	endif
#	undef ENCOE_PERFORMANCE_ENUM_ENTRY

	class CFrame
	{
		friend class CVideoRecorder;

	private:
		CVideoRecorder &parent;
		decltype(screenshotPaths) screenshotPaths;
		std::conditional<std::is_floating_point<clock::rep>::value, uintmax_t, clock::rep>::type videoPendingFrames;
		bool ready = false;

	public:
		typedef std::tuple<decltype(parent), decltype(screenshotPaths), decltype(videoPendingFrames)> &&Opaque;

	protected:
		CFrame(Opaque opaque);
		CFrame(CFrame &) = delete;
		void operator =(CFrame &) = delete;
		virtual ~CFrame() = default;

	public:
		void Ready(), Cancel();

	public:
		struct FrameData
		{
			enum class Format
			{
				B8G8R8A8,
				R10G10B10A2,
			} format;
			unsigned int width, height;
			size_t stride;
			const void *pixels;
		};
		
		virtual FrameData GetFrameData() const = 0;
	};

private:
	static inline const char *EncodePerformance_2_Str(EncodeConfig::Performance performance);
	inline char *AVErrorString(int error);
	int WritePacket();
	void KillRecordSession();
	[[noreturn]]
	void Error(const std::system_error &error);
	void Error(const std::exception &error, const char errorMsgPrefix[], const std::wstring *filename = nullptr);
	template<unsigned int FPS>
	inline void AdvanceFrame(clock::time_point now, decltype(CFrame::videoPendingFrames) &videoPendingFrames);
	void StartRecordImpl(std::wstring filename, unsigned int width, unsigned int height, bool _10bit, bool highFPS, const EncodeConfig &config, std::unique_ptr<CStartVideoRecordRequest> &&task = nullptr);
	void Process();

public:
	CVideoRecorder();
#if 1
	CVideoRecorder(CVideoRecorder &) = delete;
	void operator =(CVideoRecorder &) = delete;
#else
	CVideoRecorder(CVideoRecorder &&);
#endif
	~CVideoRecorder();

public:
	void SampleFrame(const std::function<std::shared_ptr<CFrame> (CFrame::Opaque)> &RequestFrameCallback);
	void StartRecord(std::wstring filename, unsigned int width, unsigned int height, bool _10bit/*8 if false*/, bool highFPS/*60 if true, 30 if false*/, const EncodeConfig &config = { -1 });
	void StopRecord();
	void Screenshot(std::wstring filename);
};