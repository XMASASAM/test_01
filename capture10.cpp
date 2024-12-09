#include"f4m.h";
#include"filter.h"

#define PNT(a){printf("%s\n",a);}

#define SHOW_LOG_CAPTURE 0

#if SHOW_LOG_CAPTURE
#define log_capture(a){printf("[log_capture]");a;}
#else
#define log_capture(a){}
#endif


#define SHOW_LOG_WINUI 1

#if SHOW_LOG_WINUI
#define log_winui(a){printf("[log_WinUI]");a;}
#else
#define log_winui(a){}
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
	timer time;
	bool is_active = false;
	std::set<std::function<void(void*, share_frame)>*> new_frame_event_listener;
	std::thread thread_load;
	std::mutex raise_new_frame_event_mtx;
	std::mutex current_video_frame_mtx;
	share_frame current_video_frame;
	std::map<AVPixelFormat, SwsContext*> pix_converter;
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
				if (detect_video_frame(sf.get()))current_video_frame = sf;
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
		thread_load = std::thread(&capture_device::_load, this);
	}

	void close_background_load() {
		is_active = false;
		thread_load.join();
	}

	void set_new_frame_event(std::function<void(void*, share_frame)>* func) {
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
		is_active = false;
		std::lock_guard<std::mutex> lock(current_video_frame_mtx);
		close_background_load();
		capture::close();
	}

	void set_video_format_converter(AVPixelFormat pix_fmt) {
		auto vdec_ctx = capture::video_stream->codec_ctx;
		pix_converter[pix_fmt] = sws_getContext(
			vdec_ctx->width, vdec_ctx->height, vdec_ctx->pix_fmt,
			vdec_ctx->width, vdec_ctx->height, pix_fmt,
			SWS_FAST_BILINEAR, NULL, NULL, NULL);
	}

	int get_current_video_frame_data(uint8_t* data, int data_size, AVPixelFormat pix_fmt) {
		if (!is_active)return -1;

		{
			std::lock_guard<std::mutex> lock(current_video_frame_mtx);

			share_frame cf = current_video_frame;

			if (cf == nullptr)return -1;
			uint8_t* dst[8] = { data };
			int elem = av_get_bits_per_pixel(av_pix_fmt_desc_get(pix_fmt)) / 8;

			int stride[8] = { cf->width * elem };
			if (data_size < cf->width * cf->height * elem)return -1;

			if (pix_converter.find(pix_fmt) == pix_converter.end()) {
				set_video_format_converter(pix_fmt);
			}

			return sws_scale(pix_converter[pix_fmt], cf->data, cf->linesize, 0, cf->height, dst, stride);
		}
	}
	int get_width() {
		return capture::video_stream->codec_ctx->width;
	}
	int get_height() {
		return capture::video_stream->codec_ctx->height;
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





AVCodecContext* capture_get_codec(int64_t capture_ptr, AVMediaType type) {
	auto cap = ((capture*)capture_ptr);
	return cap->get_codec_ctx(type);
}

void capture_set_new_frame_event(int64_t capture_device_ptr, std::function<void(void*, share_frame)>* func) {
	auto cap = ((capture_device*)capture_device_ptr);
	return cap->set_new_frame_event(func);
}

void capture_delete_new_frame_event(int64_t capture_device_ptr, std::function<void(void*, share_frame)>* func) {
	auto cap = ((capture_device*)capture_device_ptr);
	return cap->delete_new_frame_event(func);
}




EXPORT int f4m_capture_open1(const wchar_t* url, capture_device** cap_device_ptr) {
	avdevice_register_all();
	*cap_device_ptr = new capture_device();
	auto cap_ptr = (capture*)(*cap_device_ptr);
	cap_ptr->open(to_utf8(url).c_str(), "dshow", nullptr);
	log_winui(PNT("Opened Device!!!!"));
	(*cap_device_ptr)->start_background_load();
	log_winui(PNT("Start Background Device!!!!"));
	return 1;
}

EXPORT int f4m_capture_close1(capture_device** cap_device_ptr) {
	if (*cap_device_ptr == nullptr)return 1;
	(*cap_device_ptr)->close();
	delete* cap_device_ptr;
	*cap_device_ptr = nullptr;
	log_winui(PNT("Closed Device!!!!"));

	return 1;
}

EXPORT int f4m_capture_get_current_frame(capture_device* cap_device, uint8_t* data, int data_size, int pix_fmt) {
	return cap_device->get_current_video_frame_data(data, data_size, (AVPixelFormat)pix_fmt);
}

EXPORT int f4m_capture_get_frame_size(capture_device* cap_device, int* width, int* height) {
	*width = cap_device->get_width();
	*height = cap_device->get_height();
	return 1;
}

capture* capture_file() {
	const char* file_path1 = "C:\\Users\\N7742\\Videos\\Aver\\20230922090333.mp4";

	auto ff = new capture();
	ff->open(file_path1, nullptr, nullptr);
	return ff;
}





struct frame_data {
	char* data[8];
	int buff_size[8];
	void copy(AVFrame* f) {
		for (int i = 0; i < 8; i++) {
			if (buff_size[i] <= 0)break;
			data[i] = new char[buff_size[i]];
			memcpy_s(data[i], buff_size[i], f->data[i], buff_size[i]);
		}
	}
	~frame_data() {
		for (int i = 0; i < 8; i++) {
			if (buff_size[i] <= 0)break;
			delete[] data[i];
		}
	}

	int64_t get_sum_size() {
		int64_t ans = 0;
		for (int i = 0; i < 8; i++) {
			ans += buff_size[i];
		}
		return ans;
	}
};


AVCodecContext* make_encoder(const char* enc_name,AVCodecContext* dec,AVPixelFormat pix_fmt = AVPixelFormat::AV_PIX_FMT_NONE) {
	const AVCodec* codec = avcodec_find_encoder_by_name(enc_name);
	AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
	codec_ctx->width = dec->width;
	codec_ctx->height = dec->height;
	if (pix_fmt == AVPixelFormat::AV_PIX_FMT_NONE) {
		codec_ctx->pix_fmt = dec->pix_fmt;
	}
	else {
		codec_ctx->pix_fmt = pix_fmt;
	}
	codec_ctx->time_base = {1 , AV_TIME_BASE};//(AVRational){ 1, 25 }; // フレームレートに応じて調整
	AVDictionary* opt = nullptr;
	av_dict_set(&opt, "quality", "realtime", 0);
	av_dict_set(&opt, "speed", "10", 0);
	
	avcodec_open2(codec_ctx, codec,&opt);
	return codec_ctx;
}

filter_ctx* make_frame_converter(AVCodecContext* dec_ctx,AVPixelFormat out_pix_fmt) {
	video_filter_params p;
	p.input.width = dec_ctx->width;
	p.input.height = dec_ctx->height;
	p.input.pix_fmt = dec_ctx->pix_fmt;
	p.input.time_base = { 1,AV_TIME_BASE };
	p.output.width = dec_ctx->width;
	p.output.height = dec_ctx->height;
	p.output.pix_fmt = out_pix_fmt;
	
	return make_video_filter_ctx(p);
}

AVFrame* get_converted_frame(filter_ctx* flt_ctx ,AVFrame** frame) {
	//AVFrame* ans = av_image_alloc(frame->data,frame->linesize,frame->width,frame->height,pix_fmt,1);
	//AVFrame* tmp = av_frame_clone(frame);
	av_buffersrc_add_frame(flt_ctx->buffersrc_ctx, *frame);
	//av_frame_copy(frame, tmp);
	AVFrame* ans = av_frame_alloc();
	int ret = av_buffersink_get_frame(flt_ctx->buffersink_ctx, ans);
	av_frame_free(frame);
	*frame = nullptr;
	return ans;
}

int64_t show_buffsize(AVFrame* frame) {
	int64_t ans = 0;
	if (frame->linesize[1] == 0) {
		ans = frame->height * frame->linesize[0] * 3;
	}
	else {
		ans = frame->height * frame->linesize[0] + frame->linesize[1] * frame->height / 2 + frame->linesize[2] * frame->height / 2;
	}

	std::cout << "buff_size:" << ans << std::endl;
	return ans;
}

int64_t show_buffsize2(AVPacket* packet) {
	int64_t ans = 0;
	ans = packet->buf->size;
	std::cout << "buff_size:" << ans << std::endl;
	return ans;
}

EXPORT void test_cap() {
	auto ff = capture_file();

	//auto enc_ctx = make_encoder("zlib", ff->get_codec_ctx(AVMEDIA_TYPE_VIDEO), AVPixelFormat::AV_PIX_FMT_BGR24);
	
	AVPixelFormat pix = AVPixelFormat::AV_PIX_FMT_NONE;//AV_PIX_FMT_RGB24;
//	pix = AV_PIX_FMT_RGB24;
	//pix = AV_PIX_FMT_YUV422P;
	//pix = AV_PIX_FMT_NV12;
	auto enc_ctx = make_encoder("libvpx-vp9", ff->get_codec_ctx(AVMEDIA_TYPE_VIDEO), pix);

	AVFrame* frame;
	AVPacket* pkt = av_packet_alloc();

	filter_ctx* sws_ctx = nullptr;
	if (pix != AV_PIX_FMT_NONE) {
		sws_ctx = make_frame_converter(ff->get_codec_ctx(AVMEDIA_TYPE_VIDEO), pix);

	}

	int count = 0;
	timer time;
	int64_t all_size = 0;
	int64_t compacted_size = 0;
	int64_t time2_all = 0;
	while (detect_active_read_frame(ff->read(&frame)) && count<100) {
		if (detect_video_frame(frame)) {
			PNT("video_frame");
			AVFrame* frame_c = frame;

			if (sws_ctx != nullptr) {
				frame_c = get_converted_frame(sws_ctx, &frame);
			}


			all_size+= show_buffsize(frame_c);
			timer time2;
			int ret = avcodec_send_frame(enc_ctx, frame_c);

			while (ret >= 0) {
				ret = avcodec_receive_packet(enc_ctx, pkt);
				if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
					break;
				}
				else if (ret < 0) {
					fprintf(stderr, "Error during encoding\n");
					exit(1);
				}

				// エンコードされたパケットを使用
				compacted_size+= show_buffsize2(pkt);

				av_packet_unref(pkt);
			}
			time2_all += time2.elapsed();
			std::cout << "time:" << time2.elapsed() << std::endl;

			count++;
			std::cout << "count:" << count << std::endl;
		}
		else {
			av_frame_free(&frame);
		}
	}
	std::cout << "time[ms]" << time.elapsed() << std::endl;
	std::cout << "count:" << count << std::endl;
	std::cout << "all_size/c:" << all_size/count << std::endl;
	std::cout << "compacted_size/c:" << compacted_size/count << std::endl;
	std::cout << "cpmpacred_rate(compacted/all)%:" << (compacted_size / (double)all_size)*100 << "[%]" << std::endl;
	//std::cout << "time/c[ms]" << time.elapsed() / (double)count << "[ms]" << std::endl;
	std::cout << "time2/c[ms]" << time2_all/(double)count << "[ms]" << std::endl;

	ff->close();
}
