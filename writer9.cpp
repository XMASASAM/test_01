#include"qbs.h";
#include"f4m.h";


#define PNT(a){printf("%s\n",a);}

#define SHOW_LOG_CAPTURE 0

#if SHOW_LOG_CAPTURE
#define log_capture(a){printf("[log_capture]");a;}
#else
#define log_capture(a){}
#endif

struct stream_ctx_t {
	AVCodecContext* codec_ctx;
	int stream_index;
};



bool detect_active_read_frame(int ret) {
	if (0 <= ret)return true;
	if (ret == AVERROR(EAGAIN))return true;
	return false;
}






/// <summary>
/// inputformatを生成する.
/// </summary>
AVFormatContext* open_ifmt_ctx(const char* url, const char* fmt_name, AVDictionary* params) {
	AVFormatContext* ifmt_ctx = nullptr;
	auto fmt = av_find_input_format(fmt_name);
	int ret = avformat_open_input(&ifmt_ctx, url, fmt, &params);
	ret = avformat_find_stream_info(ifmt_ctx, NULL);

	log_capture(av_dump_format(ifmt_ctx, 0, url, 0));

	return ifmt_ctx;
}

/// <summary>
/// inputformatからデコーダーを生成する関数.
/// </summary>
/// <returns>エラーが発生するとnullptrを返す.</returns>
stream_ctx_t* make_stream_ctx(AVFormatContext* ifmt_ctx, AVMediaType type) {
	const AVCodec* codec = nullptr;

	int stream_index = av_find_best_stream(ifmt_ctx, type, -1, -1, &codec, 0);
	if (stream_index == AVERROR_STREAM_NOT_FOUND || codec == nullptr)return nullptr;

	AVStream* stream = ifmt_ctx->streams[stream_index];
	auto dec_ctx = avcodec_alloc_context3(codec);

	int ret = avcodec_parameters_to_context(dec_ctx, stream->codecpar);
	if (ret < 0) {
		avcodec_free_context(&dec_ctx);
		return nullptr;
	}

	dec_ctx->pkt_timebase = stream->time_base;
	dec_ctx->framerate = av_guess_frame_rate(ifmt_ctx, stream, NULL);

	ret = avcodec_open2(dec_ctx, codec, NULL);
	if (ret < 0) {
		avcodec_free_context(&dec_ctx);
		return nullptr;
	}

	return new stream_ctx_t{ dec_ctx,stream_index };
}


class capture {
protected:
	AVFormatContext* ifmt_ctx = nullptr;
	stream_ctx_t* video_stream;
	stream_ctx_t* audio_stream;
	AVPacket* read_packet = av_packet_alloc();
	AVFrame* read_frame = av_frame_alloc();

	stream_ctx_t* get_stream_ctx_from_stream_index(int stream_index) {
		if (video_stream != nullptr && stream_index == video_stream->stream_index) {
			log_capture(PNT("get_stream_ctx_from_stream_index:video"));
			return video_stream;
		}
		else
			if (audio_stream != nullptr && stream_index == audio_stream->stream_index) {
				log_capture(PNT("get_stream_ctx_from_stream_index:audio"));
				return audio_stream;
			}
		log_capture(PNT("get_stream_ctx_from_stream_index:other"));
		return nullptr;
	}

	void flush_dec_ctx(AVCodecContext* dec_ctx, std::vector<AVFrame*>* ans) {
		int ret = avcodec_send_packet(dec_ctx, nullptr);
		AVFrame* frame;
		while (detect_active_read_frame(receive_frame_from_dec_ctx(dec_ctx, &frame))) {
			ans->push_back(frame);
		}
	}


public:
	void open(const char* url, const char* fmt_name, AVDictionary* params) {
		ifmt_ctx = open_ifmt_ctx(url, fmt_name, params);
		video_stream = make_stream_ctx(ifmt_ctx, AVMEDIA_TYPE_VIDEO);
		audio_stream = make_stream_ctx(ifmt_ctx, AVMEDIA_TYPE_AUDIO);
	}

	/// <summary>
	/// 
	/// </summary>
	/// <returns>ret</returns>
	int receive_frame_from_dec_ctx(AVCodecContext* dec_ctx, AVFrame** frame) {
		*frame = av_frame_alloc();
		int ret = avcodec_receive_frame(dec_ctx, *frame);
		log_capture(check_error(ret, "avcodec_receive_frame"));

		if (ret < 0) {
			//if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			av_frame_free(frame);
			*frame = nullptr;
			av_packet_unref(read_packet);
			return ret;
		}

		av_packet_unref(read_packet);
		(*frame)->time_base = dec_ctx->pkt_timebase;
		return ret;
	}

	/// <summary>
	/// 
	/// </summary>
	/// <returns>ret</returns>
	virtual int read(AVFrame** frame) {
		int ret = av_read_frame(ifmt_ctx, read_packet);
		if (ret < 0) {
			*frame = nullptr;
			av_packet_unref(read_packet);
			return -1;
		}
		log_capture(printf("readpacket_pts:%lld\n", read_packet->pts));

		auto stream_ctx = get_stream_ctx_from_stream_index(read_packet->stream_index);
		auto dec_ctx = stream_ctx->codec_ctx;

		ret = avcodec_send_packet(dec_ctx, read_packet);
		log_capture(check_error(ret, "avcodec_send_packet"));

		return receive_frame_from_dec_ctx(dec_ctx, frame);

		//return 1;
	}


	void flush(std::vector<AVFrame*>* frames) {
		if (video_stream != nullptr) {
			flush_dec_ctx(video_stream->codec_ctx, frames);
		}
		if (audio_stream != nullptr) {
			flush_dec_ctx(audio_stream->codec_ctx, frames);
		}
	}

	AVCodecContext* get_codec_ctx(AVMediaType type) {
		if (type == AVMEDIA_TYPE_VIDEO && video_stream != nullptr) {
			return video_stream->codec_ctx;
		}
		else if (type == AVMEDIA_TYPE_AUDIO && audio_stream != nullptr) {
			return audio_stream->codec_ctx;
		}
		return nullptr;
	}

	virtual void close() {
		avformat_close_input(&ifmt_ctx);
	}


};

class capture_device :capture {
private:
	qbs::timer time;
	bool is_active = false;
	std::set<std::function<void(void*, share_frame)>*> new_frame_event_listener;
	std::thread thread_load;
	std::mutex raise_new_frame_event_mtx;
	void raise_new_frame_event(share_frame frame) {
		{
			std::lock_guard<std::mutex> lock(raise_new_frame_event_mtx);
			for (auto listener : new_frame_event_listener) {
				(*listener)(this, frame);
			}
		}
	}

	void _load() {
		while (is_active) {
			AVFrame* frame;
			read(&frame);
			if (frame != nullptr) {
				share_frame sf = make_share_frame(frame);
				raise_new_frame_event(sf);
			}

		}
	}

public:
	using new_frame_delegate = std::function<void(capture_device*, share_frame)>;
	int read(AVFrame** frame) override {
		int ret = capture::read(frame);
		if (*frame != nullptr) {
			(*frame)->pts = av_rescale_q(time.elapsed(), { 1,1000 }, (*frame)->time_base);

			if (!detect_video_frame(*frame)) {
				(*frame)->pts -= av_rescale_q((*frame)->nb_samples, { 1,audio_stream->codec_ctx->sample_rate }, (*frame)->time_base);
			}
		}
		return ret;
	}

	void start_background_load() {
		is_active = true;
		thread_load = std::thread(&capture_device::_load,this);
	}

	void close_background_load() {
		is_active = false;
		thread_load.join();
	}

	void set_new_frame_event(std::function<void(void*,share_frame)>* func) {
		{
			std::lock_guard<std::mutex> lock(raise_new_frame_event_mtx);
			new_frame_event_listener.insert(func);
		}

	}

	void delete_new_frame_event(std::function<void(void*, share_frame)>* func) {
		{
			std::lock_guard<std::mutex> lock(raise_new_frame_event_mtx);
			new_frame_event_listener.erase(func);
		}
	}

	void close() override {
		close_background_load();
		capture::close();
	}


	/*つかっていない.
	int64_t get_stream_time(AVMediaType type) {
		stream_ctx_t* stream = (type == AVMEDIA_TYPE_VIDEO ? video_stream : audio_stream);
		av_rescale_q(time.elapsed(), { 1,1000 }, stream->codec_ctx->time_base);
	}*/

};

class capture_file :capture {

};



capture* capture_camera() {
	capture* ff = (capture*)(new capture_device());
	//	ff->open(u8"video=C505 HD Webcam:audio=マイク (C505 HD Webcam)", "dshow", nullptr);
	//ff->open(u8"video=C505 HD Webcam", "dshow", nullptr);
	ff->open(u8"video=Live Gamer EXTREME 3:audio=HDMI/Line In (Live Gamer EXTREME 3)", "dshow", nullptr);
	return ff;
}

capture* capture_camera2() {
	capture* ff = (capture*)(new capture_device());
	//	ff->open(u8"video=C505 HD Webcam:audio=マイク (C505 HD Webcam)", "dshow", nullptr);
	//ff->open(u8"video=C505 HD Webcam", "dshow", nullptr);
	ff->open(u8"video=Front Camera", "dshow", nullptr);
	return ff;
}


capture* capture_file() {
	const char* file_path1 = "C:\\Users\\MaMaM\\source\\repos\\f4mthird\\f4mthird\\source\\poke.mp4";
	//const char* file_path1 = "C:\\Users\\MaMaM\\source\\repos\\f4mthird\\f4mthird\\source\\poke.mp4";
	//	const char* file_path1 = "C:\\Users\\MaMaM\\source\\repos\\f4mthird\\f4mthird\\source\\poke.mp4";

	auto ff = new capture();
	ff->open(file_path1, nullptr, nullptr);
	return ff;
}



void test_simple_recode(AVCodecContext* video_dec_ctx, AVCodecContext* audio_dec_ctx);
void writer_set_frame(AVFrame* frame);
void writer_close();
void set_filter_ctx(AVCodecContext* video_dec_ctx, AVCodecContext* audio_dec_ctx);
int64_t get_writer_time();

void writer4_set_capture(int64_t capture_ptr);
void writer4_set_crop(int crop_x, int crop_y, int crop_w, int crop_h);
void writer4_close();

void capture_write_frames(capture* ff) {
	int count = 0;
	AVFrame* frame;
	std::vector<AVFrame*> buf;
	qbs::timer time;
	int countB = 0;
	while (detect_active_read_frame(ff->read(&frame)) && get_writer_time() < 5 * AV_TIME_BASE) {
		count++;
		if (frame != nullptr) {
			writer_set_frame(frame);
		}
		av_frame_free(&frame);
	}
	PNT("flush!");
	printf("%ld\n", countB);
	std::vector<AVFrame*> frames;

	ff->flush(&frames);
	for (auto frame : frames) {
		count++;
		writer_set_frame(frame);

		printf("%d\n", count);
	}

	ff->close();
	delete ff;
}

void test_capture_recode1() {
	std::cout << "FFmpeg API" << std::endl;
	auto ff = capture_camera();//capture();
	//auto ff2 = capture_file();//capture();

	test_simple_recode(ff->get_codec_ctx(AVMEDIA_TYPE_VIDEO), ff->get_codec_ctx(AVMEDIA_TYPE_AUDIO));

	capture_write_frames(ff);

	auto ff2 = capture_camera2();
	set_filter_ctx(ff2->get_codec_ctx(AVMEDIA_TYPE_VIDEO), ff2->get_codec_ctx(AVMEDIA_TYPE_AUDIO));

	capture_write_frames(ff2);

	auto ff3 = capture_camera();
	set_filter_ctx(ff3->get_codec_ctx(AVMEDIA_TYPE_VIDEO), ff3->get_codec_ctx(AVMEDIA_TYPE_AUDIO));

	capture_write_frames(ff3);

	/*
		ff = capture_camera();

		set_filter_ctx(ff->get_codec_ctx(AVMEDIA_TYPE_VIDEO), ff->get_codec_ctx(AVMEDIA_TYPE_AUDIO));

		capture_write_frames(ff);
	*/

	writer_close();
}

void frame_new_frame(void* sender, share_frame frame) {
	printf("frame kita!!!!\n");
}

void test_backtest() {
	auto cap = capture_camera2();
	capture_device* capd = (capture_device*)(cap);


	capd->start_background_load();

	std::function<void(void*, share_frame)> func(frame_new_frame);

	capd->set_new_frame_event(&func);
	std::this_thread::sleep_for(std::chrono::seconds(5));
	capd->close();
}

void test_writer4() {
	auto cap = capture_camera();
	//auto cap2 = capture_camera2();
	capture_device* capd = (capture_device*)(cap);
	//capture_device* capd2 = (capture_device*)(cap2);

	capd->start_background_load();
	//capd2->start_background_load();

//	writer4_set_capture((int64_t)capd2);
//	std::this_thread::sleep_for(std::chrono::seconds(5));

	writer4_set_capture((int64_t)capd);
	std::this_thread::sleep_for(std::chrono::seconds(5));

//	writer4_set_capture((int64_t)capd2);
//	std::this_thread::sleep_for(std::chrono::seconds(5));

//	writer4_set_capture((int64_t)capd);
//	std::this_thread::sleep_for(std::chrono::seconds(5));

	writer4_set_crop(10,10,100,100);
	std::this_thread::sleep_for(std::chrono::seconds(5));

	writer4_set_crop(0, 10, 400, 400);
	std::this_thread::sleep_for(std::chrono::seconds(5));

	writer4_close();
	capd->close();
//	capd2->close();
}


EXPORT void test_correct_recode() {

	avdevice_register_all();

	//test_capture_recode1();
	//test_backtest();
	test_writer4();
}

AVCodecContext* capture_get_codec(int64_t capture_ptr,AVMediaType type) {
	auto cap = ((capture*)capture_ptr);
	return cap->get_codec_ctx(type);
}

void capture_set_new_frame_event(int64_t capture_device_ptr,std::function<void(void*,share_frame)>* func) {
	auto cap = ((capture_device*)capture_device_ptr);
	return cap->set_new_frame_event(func);
}

void capture_delete_new_frame_event(int64_t capture_device_ptr, std::function<void(void*, share_frame)>* func) {
	auto cap = ((capture_device*)capture_device_ptr);
	return cap->delete_new_frame_event(func);
}
