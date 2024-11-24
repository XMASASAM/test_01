#include"f4mthird.h";
#include"f4mthird_utility.h";

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

	void close() {
		avformat_close_input(&ifmt_ctx);
	}


};

class capture_device :capture {
private:
	timer time;
public:
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

	int64_t get_stream_time(AVMediaType type) {
		stream_ctx_t* stream = (type == AVMEDIA_TYPE_VIDEO ? video_stream : audio_stream);
		av_rescale_q(time.elapsed(), { 1,1000 }, stream->codec_ctx->time_base);
	}

};

class capture_file :capture {

};



capture* capture_camera() {
	capture* ff = (capture*)(new capture_device());
//	ff->open(u8"video=C505 HD Webcam:audio=マイク (C505 HD Webcam)", "dshow", nullptr);
	ff->open(u8"video=C505 HD Webcam", "dshow", nullptr);
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


void capture_write_frames(capture* ff) {
	int count = 0;
	AVFrame* frame;
	std::vector<AVFrame*> buf;
	timer time;
	int countB = 0;
	while (detect_active_read_frame(ff->read(&frame)) && get_writer_time() < 5*AV_TIME_BASE) {
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

void main() {
	//	test_simple_recode();
	//	memory_test();
	//	winapi_test();

	

	avdevice_register_all();
	std::cout << "FFmpeg API" << std::endl;
	auto ff = capture_camera();//capture();
	auto ff2 = capture_file();//capture();

	test_simple_recode(ff->get_codec_ctx(AVMEDIA_TYPE_VIDEO), ff->get_codec_ctx(AVMEDIA_TYPE_AUDIO));
	
	capture_write_frames(ff);

	
	set_filter_ctx(ff2->get_codec_ctx(AVMEDIA_TYPE_VIDEO), ff2->get_codec_ctx(AVMEDIA_TYPE_AUDIO));

	capture_write_frames(ff2);
/*
    ff = capture_camera();

	set_filter_ctx(ff->get_codec_ctx(AVMEDIA_TYPE_VIDEO), ff->get_codec_ctx(AVMEDIA_TYPE_AUDIO));

	capture_write_frames(ff);
	*/
	writer_close();
}
//
// 640x480 20fps:600MB/1分.
// 640x480 10fps:300MB/1分.
// 640x480 17fps:540MB/1分.
// 640x480 10fps:300MB/1分. -> using zlib 150MB/1分.


/*

			printf("%d\n", count);
			printf("timebase:%d/%d pts:%lld\n", frame->time_base.num, frame->time_base.den, frame->pts);
			printf("data_size\n");
			auto tmp_f = av_frame_alloc();
			av_frame_copy_props(tmp_f,frame);
		//	av_free(frame->data[0]);
			int data_size=0;
			if (detect_video_frame(frame)) {
				data_size = frame->linesize[2] * frame->height;
				zlib_test(frame->data[2], data_size);
				//win_test(frame->data[0], data_size);
			}
			else {
				data_size = frame->linesize[0] ;//* frame->nb_samples;
			//	zlib_test(frame->data[0], data_size);
				win_test(frame->data[0], data_size*2);

			}

			countB += sizeof(frame->data[0]) * frame->linesize[0] * frame->height / 8;
			countB += sizeof(frame->data[1]) * frame->linesize[1] * frame->height / 8;
			countB+= sizeof(frame->data[2]) * frame->linesize[2] * frame->height / 8;
			printf("data[0]:%lldB\n", sizeof(frame->data[0])*frame->linesize[0] * frame->height / 8);
			printf("data[1]:%lldB\n", sizeof(frame->data[1])* frame->linesize[1] * frame->height / 8);
			printf("data[2]:%lldB\n",sizeof(frame->data[2])* frame->linesize[2] * frame->height / 8);
			av_frame_free(&frame);
*/
