#include"f4mthird.h";
#include"f4mthird_utility.h";

#define PNT(a){printf("%s\n",a);}

#define SHOW_LOG_WRITER 1

#if SHOW_LOG_WRITER
#define log_writer(a){printf("[log_writer]");a;}
#else
#define log_writer(a){}
#endif


class video_audio_synchronization {
private:
	struct stream_ctx {
		std::queue<share_frame> queue;
		int64_t offset_pts = 0;
		int64_t start_pts = 0;
		AVRational time_base;
		int64_t get_pts_tb1e6(AVFrame* frame) {
			return av_rescale_q(frame->pts - start_pts, frame->time_base, { 1,AV_TIME_BASE });
		}
	};

	stream_ctx video;
	stream_ctx audio;
	bool first = true;
	bool has_audio_frame=true;
	std::mutex mtx;

	int64_t compare_pts(AVFrame* video_frame, AVFrame* audio_frame) {
		int64_t vpts = video.get_pts_tb1e6(video_frame);
		int64_t apts = audio.get_pts_tb1e6(audio_frame);
		return vpts - apts;
	}

public:
	video_audio_synchronization() {}
	video_audio_synchronization(int64_t video_start_pts, AVRational video_tb, int64_t audio_start_pts, AVRational audio_tb) {
		video.start_pts = video_start_pts;
		video.time_base = video_tb;
		audio.start_pts = audio_start_pts;
		audio.time_base = audio_tb;
		has_audio_frame = true;
	}
	video_audio_synchronization(int64_t video_start_pts, AVRational video_tb) {
		video.start_pts = video_start_pts;
		video.time_base = video_tb;
		has_audio_frame = false;
	}
	void input_frame(share_frame frame) {
		//AVFrame* frame = av_frame_clone(f);
		{
			std::lock_guard<std::mutex> lk(mtx);

			if (detect_video_frame(frame.get())) {
				log_writer(printf("vasyn input_frame video frame_width:%d frame_height:%d\n", frame->width, frame->height));
				video.queue.push(frame);
			}
			else {
				log_writer(printf("vasyn input_frame audio nb_samples:%d \n", frame->nb_samples));

				audio.queue.push(frame);
			}
		}


	}

	share_frame get_frame() {
		{
			std::lock_guard<std::mutex> lk(mtx);

			if (first && video.queue.size() && audio.queue.size()) {
				first = false;
				while (0 < compare_pts(video.queue.front().get(), audio.queue.front().get())) {
					//auto tmp = audio.queue.front();
					audio.queue.pop();
					//av_frame_free(&tmp);
					if (!audio.queue.size())break;
				}
				auto vf = video.queue.front();
				video.offset_pts = vf->pts;
				audio.offset_pts = av_rescale_q(video.offset_pts - video.start_pts, vf->time_base, audio.time_base) + audio.start_pts;

			}

			if (first && video.queue.size() && !has_audio_frame) {
				first = false;
				auto vf = video.queue.front();
				video.offset_pts = vf->pts;
			}

			if (first) {
				return nullptr;
			}

			share_frame ans = nullptr;
			if (video.queue.size()) {
				ans = video.queue.front();
				ans->pts -= video.offset_pts;
				log_writer(printf("va_syn video_frame set pts:%lld\n", ans->pts));
				video.queue.pop();
			}
			else if (audio.queue.size()) {
				ans = audio.queue.front();
				ans->pts -= audio.offset_pts;
				log_writer(printf("va_syn audio_frame set pts:%lld\n", ans->pts));
				audio.queue.pop();
			}
			return ans;
		}
	}

	int get_queue_size() {
		{
			std::lock_guard<std::mutex> lk(mtx);
			return video.queue.size() + audio.queue.size();
		}
	}
};


class fifo_ctx {
	AVAudioFifo* fifo;
	AVSampleFormat sample_fmt;
	AVChannelLayout channel_layout;
	int frame_size;
	int sample_rate;
	int64_t pts = LLONG_MIN;
public:
	fifo_ctx() {}
	fifo_ctx(AVSampleFormat sample_fmt, int sample_rate, AVChannelLayout& channel_layout, int frame_size) {
		this->sample_fmt = sample_fmt;
		this->frame_size = frame_size;
		this->channel_layout = channel_layout;
		this->sample_rate = sample_rate;
		fifo = av_audio_fifo_alloc(sample_fmt, channel_layout.nb_channels, 1);
	}

	void set_frame(share_frame frame) {
		if (pts == LLONG_MIN) {
			pts = frame->pts;
		}
		int ret = av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) + (frame)->nb_samples);
		ret = av_audio_fifo_write(fifo, (void**)(frame)->extended_data, (frame)->nb_samples);
	}

	AVFrame* get_frame(bool is_flush) {
		if (is_flush) {
			if (av_audio_fifo_size(fifo) <= 0)return nullptr; //is_flushがtrueの時に、fifo_sizeが0の場合trueを返す
		}
		else {
			if (av_audio_fifo_size(fifo) < frame_size)return nullptr;
		}

		int oframesize = (std::min)(frame_size, av_audio_fifo_size(fifo));
		AVFrame* f = av_frame_alloc();
		f->nb_samples = oframesize;
		f->format = sample_fmt;
		av_channel_layout_copy(&f->ch_layout, &channel_layout);
		f->sample_rate = sample_rate;
		f->time_base = { 1,sample_rate };
		f->pts = pts;
		log_writer(printf("get_frame pts:%lld\n", pts));
		pts += oframesize;

		int ret = av_frame_get_buffer(f, 0);
		ret = av_audio_fifo_read(fifo, (void**)f->data, oframesize);
		return f;
	}
};


struct codec_params {
	int width = 640;
	int height = 480;
	int sample_rate = 44100;
	int nb_channels = 1;
};




const void* get_codec_supported_config(const AVCodec* codec, AVCodecConfig conf, int* nb = nullptr) {
	const void* pl = NULL;
	int nb_pl;
	//if (nb != nullptr)nb_pl = nb;
	int ret = avcodec_get_supported_config(NULL, codec, conf, 0, &pl, &nb_pl);
	if (nb != nullptr) {
		*nb = nb_pl;
	}
	if (nb_pl <= 0) {
		return nullptr;
	}
	return pl;
}

AVPixelFormat get_codec_supported_pixformat(const AVCodec* codec) {
	auto p = get_codec_supported_config(codec, AV_CODEC_CONFIG_PIX_FORMAT);
	if (p == nullptr)return AV_PIX_FMT_NONE;
	return *(AVPixelFormat*)(p);
}

AVSampleFormat get_codec_supported_sampleformat(const AVCodec* codec) {
	auto p = get_codec_supported_config(codec, AV_CODEC_CONFIG_SAMPLE_FORMAT);
	if (p == nullptr)return AV_SAMPLE_FMT_NONE;
	return *(AVSampleFormat*)(p);
}
int get_codec_supported_best_samplerate(const AVCodec* codec, int best_samplerate = 44100) {
	int nb;
	auto p = get_codec_supported_config(codec, AV_CODEC_CONFIG_SAMPLE_RATE, &nb);
	if (p == nullptr)return 0;
	//best_samplerateに近いサンプルレートを選択する.
	int best_index = 0;
	int best_diff = INT_MAX;
	for (int i = 0; i < nb; i++) {
		int dif = abs(best_samplerate - ((int*)(p))[i]);
		if (dif < best_diff) {
			best_diff = dif;
			best_index = i;
		}
	}
	return ((int*)p)[best_index];
}
AVChannelLayout get_chlayout(int nb_ch) {
	if (2 <= nb_ch)return AV_CHANNEL_LAYOUT_STEREO;
	return AV_CHANNEL_LAYOUT_MONO;
}

AVCodecContext* make_enc_ctx(const char* enc_name, codec_params params) {

	const AVCodec* enc = avcodec_find_encoder_by_name(enc_name);
	if (enc == nullptr) {
		printf("Error encoder name\n");
		return nullptr;
	}

	auto enc_ctx = avcodec_alloc_context3(enc);

	if (enc->type == AVMEDIA_TYPE_VIDEO) {
		enc_ctx->width = params.width;
		enc_ctx->height = params.height;
		enc_ctx->sample_aspect_ratio = { 0,1 };
		enc_ctx->pix_fmt = get_codec_supported_pixformat(enc);
		enc_ctx->time_base = { 1,AV_TIME_BASE };
	}
	else if (enc->type == AVMEDIA_TYPE_AUDIO) {
		enc_ctx->sample_rate = get_codec_supported_best_samplerate(enc, params.sample_rate);
		enc_ctx->sample_fmt = get_codec_supported_sampleformat(enc);
		enc_ctx->time_base = { 1,enc_ctx->sample_rate };
		enc_ctx->ch_layout = get_chlayout(params.nb_channels);
	}
	else {
		printf("Error encoder type\n");
		avcodec_free_context(&enc_ctx);
		return nullptr;
	}

	return enc_ctx;
}

AVCodecContext* add_stream(AVFormatContext* ofmt_ctx, const char* enc_name, codec_params params) {
	auto new_stream = avformat_new_stream(ofmt_ctx, nullptr);
	auto enc_ctx = make_enc_ctx(enc_name, params);

	if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
		enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	int ret = avcodec_open2(enc_ctx, enc_ctx->codec, NULL);
	avcodec_parameters_from_context(new_stream->codecpar, enc_ctx);

	return enc_ctx;
}


AVFilterContext* make_video_filter_buffer(AVFilterGraph* graph, int width, int height, AVPixelFormat pix_fmt, AVRational time_base, char* args, int nb_args) {
	auto buffersrc = avfilter_get_by_name("buffer");
	snprintf(args, nb_args,
		"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
		width, height, pix_fmt,
		time_base.num, time_base.den,
		0, 1);

	AVFilterContext* buffersrc_ctx = NULL;

	avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, NULL, args, NULL, graph);
	return buffersrc_ctx;

}

AVFilterContext* make_video_filter_buffersink(AVFilterGraph* graph, AVPixelFormat output_pix_fmt) {
	auto buffersink = avfilter_get_by_name("buffersink");
	AVFilterContext* buffersink_ctx = NULL;
	AVPixelFormat pix_fmts[] = { output_pix_fmt };
	int ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, NULL, NULL, NULL, graph);
	ret = av_opt_set_bin(buffersink_ctx, "pix_fmts", (uint8_t*)pix_fmts, sizeof(pix_fmts), AV_OPT_SEARCH_CHILDREN);
	return buffersink_ctx;
}

AVFilterContext* make_video_filter_pad(AVFilterGraph* graph, int pad_width, int pad_height, int input_frame_x, int input_frame_y, char* args, int nb_args) {
	const AVFilter* pad = avfilter_get_by_name("pad");
	AVFilterContext* pad_ctx = NULL;
	//"w=1000:h=1000:x=0:y=0:color=violet"
	snprintf(args, nb_args,
		"w=%d:h=%d:x=%d:y=%d",
		pad_width, pad_height, input_frame_x, input_frame_y);
	avfilter_graph_create_filter(&pad_ctx, pad, NULL, args, NULL, graph);
	return pad_ctx;
}

AVFilterContext* make_video_filter_scale(AVFilterGraph* graph, int output_width, int output_height, char* args, int nb_args) {
	const AVFilter* scale = avfilter_get_by_name("scale");
	snprintf(args, nb_args,
		"w=%d:h=%d",
		output_width, output_height);
	AVFilterContext* scale_ctx = NULL;
	avfilter_graph_create_filter(&scale_ctx, scale, NULL, args, NULL, graph);
	return scale_ctx;
}


struct filter_ctx {
	AVFilterGraph* graph;
	AVFilterContext* buffersrc_ctx;
	AVFilterContext* buffersink_ctx;
};

struct video_filter_params {
	struct params {
		int width;
		int height;
		AVPixelFormat pix_fmt;
		AVRational time_base;
	};
	params input;
	params output;

};

filter_ctx* make_video_filter_ctx(video_filter_params& p) {
	char args[512];
	AVFilterGraph* graph = avfilter_graph_alloc();
	AVFilterContext* current_ctx = nullptr;
	//入力層.
	auto buffersrc = make_video_filter_buffer(graph, p.input.width, p.input.height, p.input.pix_fmt, p.input.time_base, args, sizeof(args));
	current_ctx = buffersrc;

	//スケーリング層.
	if (p.input.width != p.output.width || p.input.height != p.output.height) {
		auto scale = make_video_filter_scale(graph, p.output.width, p.output.height, args, sizeof(args));
		avfilter_link(current_ctx, 0, scale, 0);
		current_ctx = scale;
	}

	//出力層.
	auto buffersink = make_video_filter_buffersink(graph, p.output.pix_fmt);
	avfilter_link(current_ctx, 0, buffersink, 0);
	current_ctx = buffersink;

	int ret = avfilter_graph_config(graph, NULL);

	return new filter_ctx{ graph,buffersrc,buffersink };
}



AVFilterContext* make_audio_filter_abuffer(AVFilterGraph* graph, int sample_rate, AVSampleFormat sample_fmt, AVRational time_base, int nb_channels, char* args, int nb_args) {
	char buf[64];
	auto buffersrc = avfilter_get_by_name("abuffer");

	AVChannelLayout clt;
	av_channel_layout_default(&clt, nb_channels);
	av_channel_layout_describe(&clt, buf, sizeof(buf));

	snprintf(args, nb_args,
		"time_base=%d/%d:sample_rate=%d:sample_fmt=%d:channel_layout=%s",
		time_base.num, time_base.den, sample_rate,
		sample_fmt,
		buf);
	AVFilterContext* abuffersrc_ctx = NULL;
	avfilter_graph_create_filter(&abuffersrc_ctx, buffersrc, NULL, args, NULL, graph);
	return abuffersrc_ctx;
}

AVFilterContext* make_audio_filter_aformat(AVFilterGraph* graph, int sample_rate, AVSampleFormat sample_fmt, int nb_channels, char* args, int nb_args) {
	char buf[64];
	auto aformat = avfilter_get_by_name("aformat");

	AVChannelLayout clt;
	av_channel_layout_default(&clt, nb_channels);
	av_channel_layout_describe(&clt, buf, sizeof(buf));

	snprintf(args, nb_args,
		"sample_fmts=%d:sample_rates=%d:channel_layouts=%s",
		sample_fmt, sample_rate, buf);

	AVFilterContext* aformat_ctx = nullptr;
	avfilter_graph_create_filter(&aformat_ctx, aformat, NULL, args, NULL, graph);
	return aformat_ctx;
}

AVFilterContext* make_audio_filter_abuffersink(AVFilterGraph* graph) {
	auto abuffersink = avfilter_get_by_name("abuffersink");
	AVFilterContext* abuffersink_ctx;

	avfilter_graph_create_filter(&abuffersink_ctx, abuffersink, nullptr,
		NULL, NULL, graph);
	return abuffersink_ctx;
}

struct audio_filter_params {
	struct params {
		int sample_rate;
		AVSampleFormat sample_fmt;
		AVRational time_base;
		int nb_channels;
	};
	params input;
	params output;
};

filter_ctx* make_audio_filter_ctx(audio_filter_params& p) {
	char args[512];
	AVFilterGraph* graph = avfilter_graph_alloc();
	AVFilterContext* current_ctx = nullptr;
	//入力層.
	auto abuffersrc = make_audio_filter_abuffer(graph, p.input.sample_rate, p.input.sample_fmt, p.input.time_base, p.input.nb_channels, args, sizeof(args));
	current_ctx = abuffersrc;

	//formst層.
	auto aformat = make_audio_filter_aformat(graph, p.output.sample_rate, p.output.sample_fmt, p.output.nb_channels, args, sizeof(args));
	avfilter_link(current_ctx, 0, aformat, 0);
	current_ctx = aformat;

	//出力層.
	auto abuffersink = make_audio_filter_abuffersink(graph);
	avfilter_link(current_ctx, 0, abuffersink, 0);
	current_ctx = abuffersink;

	int ret = avfilter_graph_config(graph, NULL);

	return new filter_ctx{ graph,abuffersrc,abuffersink };
}

struct writer_params {
	const char* video_encoder_name;
	const char* audio_encoder_name;
	codec_params encoder_params;
};
/*
class writer {
	struct stream_ctx {
		AVCodecContext* enc_ctx;
		int stream_index;
		int64_t current_pts = 0;
	};
	AVFormatContext* ofmt_ctx = nullptr;
	fifo_ctx* fifo;//audio用.
	stream_ctx video_stream;
	stream_ctx audio_stream;
	AVPacket* read_packet = av_packet_alloc();
	video_audio_synchronization* vasyn;
public:
	writer(const char* file_path, writer_params& p) {
		int ret = avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, file_path);
		int stream_index = 0;

		if (!str_null_or_empty(p.video_encoder_name)) {
			video_stream.enc_ctx = add_stream(ofmt_ctx, p.video_encoder_name, p.encoder_params);
			video_stream.stream_index = stream_index++;
		}

		if (!str_null_or_empty(p.audio_encoder_name)) {
			audio_stream.enc_ctx = add_stream(ofmt_ctx, p.audio_encoder_name, p.encoder_params);
			audio_stream.stream_index = stream_index++;

			fifo = new fifo_ctx(
				audio_stream.enc_ctx->sample_fmt,
				audio_stream.enc_ctx->sample_rate,
				audio_stream.enc_ctx->ch_layout,
				audio_stream.enc_ctx->frame_size);
		}

		if (has_video_stream() && has_audio_stream()) {
			vasyn = new video_audio_synchronization(
				0, video_stream.enc_ctx->time_base,
				0, audio_stream.enc_ctx->time_base
			);
		}

		log_writer(av_dump_format(ofmt_ctx, 0, file_path, 1));

		//Fileの出力を必要とするのかの条件式.!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)
		ret = avio_open(&ofmt_ctx->pb, file_path, AVIO_FLAG_WRITE);
		log_writer(check_error(ret, "avio_open"));
		//headerの初期化.
		ret = avformat_write_header(ofmt_ctx, NULL);
		log_writer(check_error(ret, "avformat_write_header"));

	}
	void write_frame(AVMediaType type, AVFrame* frame) {
		stream_ctx& stream_ctx = (type == AVMEDIA_TYPE_VIDEO ? video_stream : audio_stream);
		AVCodecContext* enc_ctx = stream_ctx.enc_ctx;

		if (enc_ctx == nullptr) {
			log_writer(PNT("write_frame don't supported stream type"));
			return;
		}

		if (frame != nullptr) {//flushの際はnullptr.
			frame->pts = av_rescale_q(frame->pts, frame->time_base, enc_ctx->time_base);
		}

		if (type == AVMEDIA_TYPE_AUDIO && frame != nullptr) {
			log_writer(printf("audio_nb_frames:%d\n", frame->nb_samples));
		}

		avcodec_send_frame(enc_ctx, frame);

		while (avcodec_receive_packet(enc_ctx, read_packet) == 0) {
			av_packet_rescale_ts(read_packet,
				enc_ctx->time_base,
				ofmt_ctx->streams[stream_ctx.stream_index]->time_base);
			read_packet->stream_index = stream_ctx.stream_index;
			log_writer(printf("write_frame pts:%lld\n", read_packet->pts));
			av_interleaved_write_frame(ofmt_ctx, read_packet);
			av_packet_unref(read_packet);
		}
	}

	void fifo_write_frame(bool flush, int64_t offset_pts) {
		int64_t pts = offset_pts;
		while (AVFrame* f = fifo->get_frame(flush)) {
			f->pts = pts;
			pts += f->nb_samples;
			vasyn->input_frame(f);
		}
		audio_stream.current_pts = pts;
		vasyn_write_frame();
	}

	void vasyn_write_frame() {
		while (auto f = vasyn->get_frame()) {
			if (detect_video_frame(f)) {
				write_frame(AVMEDIA_TYPE_VIDEO, f);
			}
			else {
				write_frame(AVMEDIA_TYPE_AUDIO, f);
			}
			av_frame_free(&f);
		}
	}

	void write(AVFrame* frame) {
		if (detect_video_frame(frame)) {
			vasyn->input_frame(frame);
			vasyn_write_frame();
		}
		else {
			fifo->set_frame(frame);
			fifo_write_frame(false, (std::max)(frame->pts, audio_stream.current_pts));
		}
	}

	void close() {
		write_frame(AVMediaType::AVMEDIA_TYPE_VIDEO, nullptr);

		fifo_write_frame(true, audio_stream.current_pts);
		write_frame(AVMediaType::AVMEDIA_TYPE_AUDIO, nullptr);

		int ret = av_write_trailer(ofmt_ctx);
		avformat_free_context(ofmt_ctx);
	}
	AVCodecParameters* get_codec_params(AVMediaType type) {
		if (type == AVMEDIA_TYPE_VIDEO && video_stream.enc_ctx != nullptr) {
			return ofmt_ctx->streams[video_stream.stream_index]->codecpar;
		}
		else if (type == AVMEDIA_TYPE_AUDIO && audio_stream.enc_ctx != nullptr) {
			return ofmt_ctx->streams[audio_stream.stream_index]->codecpar;
		}
		log_writer(PNT("get_codec_params other type"));
		return nullptr;
	}

	bool has_video_stream() {
		return video_stream.enc_ctx != nullptr;
	}
	bool has_audio_stream() {
		return audio_stream.enc_ctx != nullptr;
	}

};

*/
class writer2 {
protected:
	struct stream_ctx {
		AVCodecContext* enc_ctx;
		int stream_index;
		int64_t latest_packet_pts=LLONG_MIN;
	//	int64_t latest_encoder_pts=LLONG_MIN;

	};
	struct write_frame_t {
		AVMediaType type;
		share_frame frame;
	};

	AVFormatContext* ofmt_ctx = nullptr;
	stream_ctx video_stream;
	stream_ctx audio_stream;
	AVPacket* read_packet = av_packet_alloc();
	std::thread thread_write;
	std::mutex thread_write_mtx;
	std::mutex frame_queue_mtx;
	std::condition_variable thread_write_cond;
	std::queue<write_frame_t> thread_write_queue;
	bool is_active=true;
public:
	writer2(const char* file_path, writer_params& p) {
		int ret = avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, file_path);
		int stream_index = 0;

		if (!str_null_or_empty(p.video_encoder_name)) {
			video_stream.enc_ctx = add_stream(ofmt_ctx, p.video_encoder_name, p.encoder_params);
			video_stream.stream_index = stream_index++;
		}

		if (!str_null_or_empty(p.audio_encoder_name)) {
			audio_stream.enc_ctx = add_stream(ofmt_ctx, p.audio_encoder_name, p.encoder_params);
			audio_stream.stream_index = stream_index++;
		}


		log_writer(av_dump_format(ofmt_ctx, 0, file_path, 1));

		//Fileの出力を必要とするのかの条件式.!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)
		ret = avio_open(&ofmt_ctx->pb, file_path, AVIO_FLAG_WRITE);
		log_writer(check_error(ret, "avio_open"));
		//headerの初期化.
		ret = avformat_write_header(ofmt_ctx, NULL);
		log_writer(check_error(ret, "avformat_write_header"));
		thread_write = std::thread(&writer2::write_loop,this);
	}

	void write(share_frame frame) {
		if (detect_video_frame(frame.get())) {
			//write(AVMEDIA_TYPE_VIDEO, frame);
			push_frame(AVMEDIA_TYPE_VIDEO,frame);
		}
		else {
			push_frame(AVMEDIA_TYPE_AUDIO, frame);
			//write(AVMEDIA_TYPE_AUDIO, frame);
		}
	}

	void _write(AVMediaType type, share_frame frame) {
		stream_ctx& stream_ctx = (type == AVMEDIA_TYPE_VIDEO ? video_stream : audio_stream);
		AVCodecContext* enc_ctx = stream_ctx.enc_ctx;

		if (enc_ctx == nullptr) {
			log_writer(PNT("write_frame don't supported stream type"));
			return;
		}

		if (frame != nullptr) {//flushの際はnullptr.
			frame->pts = av_rescale_q(frame->pts, frame->time_base, enc_ctx->time_base);

		//	stream_ctx.latest_encoder_pts = frame->pts;
		}

		if (type == AVMEDIA_TYPE_AUDIO && frame != nullptr) {
			log_writer(printf("audio_nb_frames:%d\n", frame->nb_samples));
		}

		avcodec_send_frame(enc_ctx, frame.get());

		while (avcodec_receive_packet(enc_ctx, read_packet) == 0) {
			av_packet_rescale_ts(read_packet,
				enc_ctx->time_base,
				ofmt_ctx->streams[stream_ctx.stream_index]->time_base);
			read_packet->stream_index = stream_ctx.stream_index;
			log_writer(printf("write_frame pts:%lld\n", read_packet->pts));

			//stream_ctx.latest_packet_pts = (std::max)(stream_ctx.latest_packet_pts, read_packet->pts);

			av_interleaved_write_frame(ofmt_ctx, read_packet);
			av_packet_unref(read_packet);
		}
	}
	void push_frame(AVMediaType type,share_frame frame) {
		
		stream_ctx& stream_ctx = (type == AVMEDIA_TYPE_VIDEO ? video_stream : audio_stream);		
		
		if (frame != nullptr) {
			stream_ctx.latest_packet_pts = av_rescale_q(frame->pts, frame->time_base, ofmt_ctx->streams[stream_ctx.stream_index]->time_base);
		}

		{
			std::lock_guard<std::mutex> lock(frame_queue_mtx);
			thread_write_queue.push({type,frame});
		}

		{
			std::lock_guard<std::mutex> lock(thread_write_mtx);
			thread_write_cond.notify_all();
		}
	}
	void write_loop() {
		while (is_active || thread_write_queue.size())
		{
			{
				std::unique_lock<std::mutex> lock(thread_write_mtx);
				thread_write_cond.wait(lock, [&] {return thread_write_queue.size() || !is_active; });
			}

			int queue_size;
			write_frame_t frame;

			while (true) {
				{
					std::lock_guard<std::mutex> lock(frame_queue_mtx);
					queue_size = thread_write_queue.size();
					if (queue_size <= 0)break;
					frame = thread_write_queue.front();
					thread_write_queue.pop();
				}
				_write(frame.type, frame.frame);
			}
		}
	}

	void close() {
		push_frame(AVMEDIA_TYPE_VIDEO, nullptr);
		push_frame(AVMEDIA_TYPE_AUDIO, nullptr);
		is_active = false;
		thread_write.join();

		int ret = av_write_trailer(ofmt_ctx);
		avformat_free_context(ofmt_ctx);
	}
	AVCodecParameters* get_codec_params(AVMediaType type) {
		if (type == AVMEDIA_TYPE_VIDEO && video_stream.enc_ctx != nullptr) {
			return ofmt_ctx->streams[video_stream.stream_index]->codecpar;
		}
		else if (type == AVMEDIA_TYPE_AUDIO && audio_stream.enc_ctx != nullptr) {
			return ofmt_ctx->streams[audio_stream.stream_index]->codecpar;
		}
		log_writer(PNT("get_codec_params other type"));
		return nullptr;
	}

	bool has_video_stream() {
		return video_stream.enc_ctx != nullptr;
	}
	bool has_audio_stream() {
		return audio_stream.enc_ctx != nullptr;
	}

	int64_t get_current_duration_tn1e6() {
		int64_t lpts = 0;
		if (has_video_stream()) {
			lpts = av_rescale_q(video_stream.latest_packet_pts, ofmt_ctx->streams[video_stream.stream_index]->time_base, {1,AV_TIME_BASE});
		}
		if (has_audio_stream()) {
			auto t = av_rescale_q(audio_stream.latest_packet_pts, ofmt_ctx->streams[audio_stream.stream_index]->time_base, { 1,AV_TIME_BASE });
			lpts = (std::max)(lpts, t);
		}
		return lpts;
	}

};


class writer3 :writer2 {
	fifo_ctx* fifo = nullptr;//audio用.
	video_audio_synchronization* vasyn = nullptr;
	struct stream_ctx {
		int64_t passed_pts = 0;
	};
	int64_t offset_input_pts_tb1e6 = 0;
	stream_ctx video_stream;
	stream_ctx audio_stream;
	share_frame silent_audio_frame=nullptr;
	bool recieve_audio_frame=true;
	std::condition_variable vasyn_cond_val;
	std::mutex vasyn_cond_mtx;
	bool is_active=true;
public:
	writer3(const char* file_path, writer_params& p) :writer2(file_path, p) {

		//AVFrame* f = //silent_audio_frame;
		auto e = writer2::audio_stream.enc_ctx;

		AVFrame* f = av_frame_alloc();
		f->format = e->sample_fmt;
		f->ch_layout = e->ch_layout; 
		f->sample_rate = e->sample_rate; 
		f->nb_samples = e->frame_size; 
		f->time_base = e->time_base;
		av_frame_get_buffer(f, 0);
		// 無音データの設定 
		memset(f->data[0], 0, f->nb_samples * av_get_bytes_per_sample(e->sample_fmt));
		silent_audio_frame = make_share_frame(f);
	}

	void set_vasyn(bool is_input_audio_frame) {
		if (fifo != nullptr) {
			fifo_write_frame(false, audio_stream.passed_pts);
			delete fifo;
			fifo = nullptr;
		}
		if (vasyn != nullptr) {
			delete vasyn;
			vasyn = nullptr;
		}

		if (has_audio_stream()) {
			auto e = writer2::audio_stream.enc_ctx;
			fifo = new fifo_ctx(
				e->sample_fmt,
				e->sample_rate,
				e->ch_layout,
				e->frame_size);

		}
		
		//ここまでの処理でこれまで入力されたframeは全てwriter2へ入力された.
		offset_input_pts_tb1e6 = get_current_duration_tn1e6() + 1000;//ここの10000は適当. 0.001sの間をとる.
		if (offset_input_pts_tb1e6<0) {
			offset_input_pts_tb1e6=0;
		}
		
		video_stream.passed_pts = 0;
		audio_stream.passed_pts = 0;


		if (has_video_stream() && has_audio_stream()) {
			if (is_input_audio_frame) {
				vasyn = new video_audio_synchronization(
					0, writer2::video_stream.enc_ctx->time_base,
					0, writer2::audio_stream.enc_ctx->time_base
				);
			}
			else {
				vasyn = new video_audio_synchronization(
					0, writer2::video_stream.enc_ctx->time_base
				);
			}
		}
		recieve_audio_frame = is_input_audio_frame;
	}

	int64_t get_current_vasyn_duration_tb1e6() {
		int64_t d = get_current_duration_tn1e6() - offset_input_pts_tb1e6;
		if(d<0)return 0;
		return d;
	}

	void fifo_write_frame(bool flush, int64_t offset_pts) {
		int64_t pts = offset_pts;
		while (AVFrame* f = fifo->get_frame(flush)) {
			f->pts = pts;
			pts += f->nb_samples;
			vasyn->input_frame(make_share_frame(f));
		}
		audio_stream.passed_pts = pts;
		vasyn_write_frame();
	}

	void thread_vasyn_write_frame() {
		while (is_active) {
			{
				std::unique_lock<std::mutex> lk(vasyn_cond_mtx);
				vasyn_cond_val.wait(lk,[&]{return vasyn->get_queue_size();});
				
			}
		}
	}

	void vasyn_write_frame() {//ここでは0からのptsが入力される予定.
		while (auto frame = vasyn->get_frame()) {
			if (!recieve_audio_frame) {
				int64_t pts = audio_stream.passed_pts;
				while (pts < av_rescale_q(frame->pts, frame->time_base, writer2::audio_stream.enc_ctx->time_base)) {
					silent_audio_frame->pts = pts;
					pts += silent_audio_frame->nb_samples;
					writer2::write(make_share_frame(av_frame_clone(silent_audio_frame.get())));
				}
				audio_stream.passed_pts = pts;
			}
			frame->pts += av_rescale_q(offset_input_pts_tb1e6, { 1,AV_TIME_BASE }, frame->time_base);
			writer2::write(frame);
			log_writer(printf("latest_pts:%lld\n", get_current_duration_tn1e6() / AV_TIME_BASE));
			//av_frame_free(&frame);
		}
	}

	void write(share_frame frame) {
		//このframeのtimebaseはdecoderのtimebase
		if (detect_video_frame(frame.get())) {
			vasyn->input_frame(frame);
			vasyn_write_frame();
		}
		else {
			fifo->set_frame(frame);
			fifo_write_frame(false, (std::max)(frame->pts, audio_stream.passed_pts));
		}
	}

	void close() {
		fifo_write_frame(true, audio_stream.passed_pts);
		writer2::close();
	}


};
//

writer3* writer_ctx = nullptr;
filter_ctx* video_flt_ctx = nullptr;
filter_ctx* audio_flt_ctx = nullptr;


void writer_set_frame(AVFrame* frame) {

	filter_ctx* flt_ctx = detect_video_frame(frame) ? video_flt_ctx : audio_flt_ctx;

	av_buffersrc_add_frame(flt_ctx->buffersrc_ctx, frame);

	while (true) {
		AVFrame* flt_frame = av_frame_alloc();
		int ret = av_buffersink_get_frame(flt_ctx->buffersink_ctx, flt_frame);
		if (ret < 0) {
			av_frame_free(&flt_frame);
			break;
		}
		//flt_frame->pts = flt_frame->best_effort_timestamp;
		flt_frame->time_base = av_buffersink_get_time_base(flt_ctx->buffersink_ctx);
		writer_ctx->write(make_share_frame(flt_frame));
		//av_frame_unref(flt_frame);
	}
	//av_frame_free(&flt_frame);
}

void writer_close() {
	writer_ctx->close();
}

int64_t get_writer_time() {
	return writer_ctx->get_current_vasyn_duration_tb1e6();
}


void test_simple_recode(AVCodecContext* video_dec_ctx, AVCodecContext* audio_dec_ctx) {
	//const char* file_path = "C:\\Users\\Videos\\Test\\test.mp4";
	get_codec_supported_pixformat(avcodec_find_encoder_by_name("jpeg2000"));
	const char* file_path = "C:\\Users\\MaMaM\\source\\repos\\f4mthird\\f4mthird\\source\\test.mp4";
	writer_params wp;
	wp.video_encoder_name = "libopenh264";
	//mjpeg
//	wp.video_encoder_name = "libvpx-vp9";
//	wp.video_encoder_name = "h264";
	wp.audio_encoder_name = "aac";
	wp.encoder_params.width = 640;
	wp.encoder_params.height = 480;
	wp.encoder_params.sample_rate = 44100;
	wp.encoder_params.nb_channels = 1;


	writer_ctx = new writer3(file_path, wp);


	//av_interleaved_write_frame(ofmt_ctx, enc_pkt);


	video_filter_params v_par;
	v_par.input.width = video_dec_ctx->width;
	v_par.input.height = video_dec_ctx->height;
	v_par.input.pix_fmt = video_dec_ctx->pix_fmt;
	v_par.input.time_base = video_dec_ctx->pkt_timebase;

	auto wvc = ((writer2*)writer_ctx)->get_codec_params(AVMEDIA_TYPE_VIDEO);
	v_par.output.width = wvc->width;
	v_par.output.height = wvc->height;
	v_par.output.pix_fmt = (AVPixelFormat)wvc->format;

	video_flt_ctx = make_video_filter_ctx(v_par);

	if (audio_dec_ctx!=nullptr) {
		audio_filter_params a_par;
		a_par.input.nb_channels = audio_dec_ctx->ch_layout.nb_channels;
		a_par.input.sample_rate = audio_dec_ctx->sample_rate;
		a_par.input.sample_fmt = audio_dec_ctx->sample_fmt;
		a_par.input.time_base = audio_dec_ctx->pkt_timebase;

		auto wac = ((writer2*)writer_ctx)->get_codec_params(AVMEDIA_TYPE_AUDIO);
		a_par.output.nb_channels = wac->ch_layout.nb_channels;
		a_par.output.sample_rate = wac->sample_rate;
		a_par.output.sample_fmt = (AVSampleFormat)wac->format;

		audio_flt_ctx = make_audio_filter_ctx(a_par);
		writer_ctx->set_vasyn(true);
	}
	else {
		writer_ctx->set_vasyn(false);
	}
	
}

void set_filter_ctx(AVCodecContext* video_dec_ctx, AVCodecContext* audio_dec_ctx) {
	
	writer_ctx->set_vasyn(audio_dec_ctx!=nullptr);

	if (video_flt_ctx != nullptr) {
		delete video_flt_ctx;
	}
	if (audio_flt_ctx != nullptr) {
		delete audio_flt_ctx;
	}
	video_filter_params v_par;
	v_par.input.width = video_dec_ctx->width;
	v_par.input.height = video_dec_ctx->height;
	v_par.input.pix_fmt = video_dec_ctx->pix_fmt;
	v_par.input.time_base = video_dec_ctx->pkt_timebase;

	auto wvc = ((writer2*)writer_ctx)->get_codec_params(AVMEDIA_TYPE_VIDEO);
	v_par.output.width = wvc->width;
	v_par.output.height = wvc->height;
	v_par.output.pix_fmt = (AVPixelFormat)wvc->format;

	video_flt_ctx = make_video_filter_ctx(v_par);

	if (audio_dec_ctx!=nullptr) {

		audio_filter_params a_par;
		a_par.input.nb_channels = audio_dec_ctx->ch_layout.nb_channels;
		a_par.input.sample_rate = audio_dec_ctx->sample_rate;
		a_par.input.sample_fmt = audio_dec_ctx->sample_fmt;
		a_par.input.time_base = audio_dec_ctx->pkt_timebase;

		auto wac = ((writer2*)writer_ctx)->get_codec_params(AVMEDIA_TYPE_AUDIO);
		a_par.output.nb_channels = wac->ch_layout.nb_channels;
		a_par.output.sample_rate = wac->sample_rate;
		a_par.output.sample_fmt = (AVSampleFormat)wac->format;

		audio_flt_ctx = make_audio_filter_ctx(a_par);
	}
}
