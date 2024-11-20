#include"f4mthird.h";
#include"f4mthird_utility.h"

#define PNT(a){printf("%s\n",a);}

#define SHOW_LOG_WRITER 1

#if SHOW_LOG_WRITER
#define log_writer(a){printf("[log_writer]");a;}
#else
#define log_writer(a){}
#endif


class fifo_ctx {
	AVAudioFifo* fifo;
	AVSampleFormat sample_fmt;
	AVChannelLayout channel_layout;
	int frame_size;
	int sample_rate;
	int64_t pts=LLONG_MIN;
public:
	fifo_ctx() {}
	fifo_ctx(AVSampleFormat sample_fmt, int sample_rate, AVChannelLayout& channel_layout, int frame_size) {
		this->sample_fmt = sample_fmt;
		this->frame_size = frame_size;
		this->channel_layout = channel_layout;
		this->sample_rate = sample_rate;
		fifo = av_audio_fifo_alloc(sample_fmt, channel_layout.nb_channels, 1);
	}

	void set_frame(AVFrame* frame) {
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
		f->time_base = {1,sample_rate};
		f->pts = pts;
		log_writer(printf("get_frame pts:%lld\n",pts));
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

AVCodecContext* make_enc_ctx(const char* enc_name,codec_params params) {

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

class writer {
	struct stream_ctx {
		AVCodecContext* enc_ctx;
		int stream_index;
	};
	AVFormatContext* ofmt_ctx = nullptr;
	fifo_ctx* fifo;//audio用.
	stream_ctx video_stream;
	stream_ctx audio_stream;
	AVPacket* read_packet = av_packet_alloc();
public:
	writer(const char* file_path,writer_params& p) {
		int ret = avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, file_path);
		int stream_index=0;
		
		if (!str_null_or_empty(p.video_encoder_name)) {
			video_stream.enc_ctx = add_stream(ofmt_ctx, p.video_encoder_name, p.encoder_params);
			video_stream.stream_index=stream_index++;
		}

		if (!str_null_or_empty(p.audio_encoder_name)) {
			audio_stream.enc_ctx = add_stream(ofmt_ctx, p.audio_encoder_name, p.encoder_params);
			audio_stream.stream_index=stream_index++;

			fifo = new fifo_ctx(
				audio_stream.enc_ctx->sample_fmt,
				audio_stream.enc_ctx->sample_rate,
				audio_stream.enc_ctx->ch_layout,
				audio_stream.enc_ctx->frame_size);
		}


		log_writer(av_dump_format(ofmt_ctx, 0, file_path, 1));

		//Fileの出力を必要とするのかの条件式.!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)
		ret = avio_open(&ofmt_ctx->pb, file_path, AVIO_FLAG_WRITE);
		log_writer(check_error(ret,"avio_open"));
		//headerの初期化.
		ret = avformat_write_header(ofmt_ctx, NULL);
		log_writer(check_error(ret, "avformat_write_header"));

	}
	void write_frame(AVMediaType type,AVFrame* frame) {		
		stream_ctx& stream_ctx = (type == AVMEDIA_TYPE_VIDEO ? video_stream : audio_stream);
		AVCodecContext* enc_ctx = stream_ctx.enc_ctx;

		if (enc_ctx == nullptr) {
			log_writer(PNT("write_frame don't supported stream type"));
			return;
		}

		if (frame!=nullptr) {//flushの際はnullptr.
			frame->pts = av_rescale_q(frame->pts, frame->time_base, enc_ctx->time_base);
		}

		avcodec_send_frame(enc_ctx,frame);

		while (avcodec_receive_packet(enc_ctx, read_packet) == 0) {
			av_packet_rescale_ts(read_packet,
				enc_ctx->time_base,
				ofmt_ctx->streams[stream_ctx.stream_index]->time_base);
			read_packet->stream_index = stream_ctx.stream_index;
			log_writer(printf("write_frame pts:%lld\n",read_packet->pts));
			av_interleaved_write_frame(ofmt_ctx, read_packet);
			av_packet_unref(read_packet);
		}
	}

	void fifo_write_frame(bool flush) {
		while (AVFrame* f = fifo->get_frame(flush)) {
			write_frame(AVMEDIA_TYPE_AUDIO, f);
		}
	}


	void write(AVFrame* frame) {
		if (detect_video_frame(frame)) {
			write_frame(AVMEDIA_TYPE_VIDEO,frame);
		}
		else {
			fifo->set_frame(frame);
			fifo_write_frame(false);
		}
	}

	void close() {
		write_frame(AVMediaType::AVMEDIA_TYPE_VIDEO, nullptr);

		fifo_write_frame(true);
		write_frame(AVMediaType::AVMEDIA_TYPE_AUDIO, nullptr);

		int ret = av_write_trailer(ofmt_ctx);
		avformat_free_context(ofmt_ctx);
	}
	AVCodecParameters* get_codec_params(AVMediaType type) {
		if (type == AVMEDIA_TYPE_VIDEO && video_stream.enc_ctx!=nullptr) {
			return ofmt_ctx->streams[video_stream.stream_index]->codecpar;
		}
		else if(type == AVMEDIA_TYPE_AUDIO && audio_stream.enc_ctx != nullptr){
			return ofmt_ctx->streams[audio_stream.stream_index]->codecpar;
		}
		log_writer(PNT("get_codec_params other type"));
		return nullptr;
	}
};


writer* writer_ctx;
filter_ctx* video_flt_ctx;
filter_ctx* audio_flt_ctx;


void writer_set_frame(AVFrame* frame) {

	filter_ctx* flt_ctx = detect_video_frame(frame) ? video_flt_ctx : audio_flt_ctx;

	av_buffersrc_add_frame(flt_ctx->buffersrc_ctx, frame);

	AVFrame* flt_frame = av_frame_alloc();
	while (av_buffersink_get_frame(flt_ctx->buffersink_ctx, flt_frame) == 0) {

		//flt_frame->pts = flt_frame->best_effort_timestamp;
		flt_frame->time_base = av_buffersink_get_time_base(flt_ctx->buffersink_ctx);
		writer_ctx->write(flt_frame);
		av_frame_unref(flt_frame);
	}
	av_frame_free(&flt_frame);
}

void writer_close() {
	writer_ctx->close();
}

void test_simple_recode(AVCodecContext* video_dec_ctx,AVCodecContext* audio_dec_ctx) {
	const char* file_path = "C:\\Users\\MaMaM\\source\\repos\\f4mthird\\f4mthird\\source\\test.mp4";
	
	writer_params wp;
	wp.video_encoder_name = "libopenh264";
	wp.audio_encoder_name = "aac";
	wp.encoder_params.width = 640;
	wp.encoder_params.height = 480;
	wp.encoder_params.sample_rate = 44100;
	wp.encoder_params.nb_channels = 1;


	writer_ctx = new writer(file_path,wp);

	
	//av_interleaved_write_frame(ofmt_ctx, enc_pkt);

	
	video_filter_params v_par;
	v_par.input.width = video_dec_ctx->width;
	v_par.input.height = video_dec_ctx->height;
	v_par.input.pix_fmt = video_dec_ctx->pix_fmt;
	v_par.input.time_base = video_dec_ctx->pkt_timebase;

	auto wvc = writer_ctx->get_codec_params(AVMEDIA_TYPE_VIDEO);
	v_par.output.width = wvc->width;
	v_par.output.height = wvc->height;
	v_par.output.pix_fmt = (AVPixelFormat)wvc->format;

	video_flt_ctx = make_video_filter_ctx(v_par);

	audio_filter_params a_par;
	a_par.input.nb_channels = audio_dec_ctx->ch_layout.nb_channels;
	a_par.input.sample_rate = audio_dec_ctx->sample_rate;
	a_par.input.sample_fmt = audio_dec_ctx->sample_fmt;
	a_par.input.time_base = audio_dec_ctx->pkt_timebase;

	auto wac = writer_ctx->get_codec_params(AVMEDIA_TYPE_AUDIO);
	a_par.output.nb_channels = wac->ch_layout.nb_channels;
	a_par.output.sample_rate = wac->sample_rate;
	a_par.output.sample_fmt = (AVSampleFormat)wac->format;

	audio_flt_ctx = make_audio_filter_ctx(a_par);
}
