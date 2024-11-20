
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


AVFilterContext* make_video_filter_buffer(AVFilterGraph* graph,int width,int height,AVPixelFormat pix_fmt,AVRational time_base,char* args,int nb_args) {
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

AVFilterContext* make_video_filter_buffersink(AVFilterGraph* graph,AVPixelFormat output_pix_fmt) {
	auto buffersink = avfilter_get_by_name("buffersink");
	AVFilterContext* buffersink_ctx = NULL;
	AVPixelFormat pix_fmts[] = {output_pix_fmt};
	int ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, NULL, NULL, NULL, graph);
	ret = av_opt_set_bin(buffersink_ctx, "pix_fmts", (uint8_t*)pix_fmts, sizeof(pix_fmts), AV_OPT_SEARCH_CHILDREN);
	return buffersink_ctx;
}

AVFilterContext* make_video_filter_pad(AVFilterGraph* graph,int pad_width,int pad_height,int input_frame_x,int input_frame_y, char* args, int nb_args) {
	const AVFilter* pad = avfilter_get_by_name("pad");
	AVFilterContext* pad_ctx = NULL;
	//"w=1000:h=1000:x=0:y=0:color=violet"
	snprintf(args, nb_args,
		"w=%d:h=%d:x=%d:y=%d",
		pad_width, pad_height,input_frame_x,input_frame_y);
	avfilter_graph_create_filter(&pad_ctx, pad, NULL, args, NULL,graph);
	return pad_ctx;
}

AVFilterContext* make_video_filter_scale(AVFilterGraph* graph,int output_width,int output_height,char* args, int nb_args) {
	const AVFilter* scale = avfilter_get_by_name("scale");
	snprintf(args, nb_args,
		"w=%d:h=%d",
		output_width,output_height);
	AVFilterContext* scale_ctx = NULL;
	avfilter_graph_create_filter(&scale_ctx, scale, NULL, args, NULL,graph);
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
	auto buffersrc = make_video_filter_buffer(graph,p.input.width,p.input.height,p.input.pix_fmt,p.input.time_base,args,sizeof(args));
	current_ctx = buffersrc;

	//スケーリング層.
	if (p.input.width != p.output.width || p.input.height != p.output.height) {
		auto scale = make_video_filter_scale(graph,p.output.width,p.output.height,args,sizeof(args));
		avfilter_link(current_ctx, 0, scale, 0);
		current_ctx = scale;
	}

	//出力層.
	auto buffersink = make_video_filter_buffersink(graph,p.output.pix_fmt);
	avfilter_link(current_ctx, 0, buffersink, 0);
	current_ctx = buffersink;

	int ret = avfilter_graph_config(graph, NULL);

	return new filter_ctx{ graph,buffersrc,buffersink };
}



AVFilterContext* make_audio_filter_abuffer(AVFilterGraph* graph,int sample_rate ,AVSampleFormat sample_fmt,AVRational time_base,int nb_channels, char* args, int nb_args) {
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
	avfilter_graph_create_filter(&abuffersrc_ctx, buffersrc, NULL,args, NULL,graph);
	return abuffersrc_ctx;
}

AVFilterContext* make_audio_filter_aformat(AVFilterGraph* graph,int sample_rate,AVSampleFormat sample_fmt,int nb_channels, char* args, int nb_args) {
	char buf[64];
	auto aformat = avfilter_get_by_name("aformat");

	AVChannelLayout clt;
	av_channel_layout_default(&clt, nb_channels);
	av_channel_layout_describe(&clt, buf, sizeof(buf));

	snprintf(args, nb_args,
		"sample_fmts=%d:sample_rates=%d:channel_layouts=%s",
		sample_fmt, sample_rate,buf);

	AVFilterContext* aformat_ctx = nullptr;
	avfilter_graph_create_filter(&aformat_ctx, aformat, NULL,args, NULL,graph);
	return aformat_ctx;
}

AVFilterContext* make_audio_filter_abuffersink(AVFilterGraph* graph) {
	auto abuffersink = avfilter_get_by_name("abuffersink");
	AVFilterContext* abuffersink_ctx;

	avfilter_graph_create_filter(&abuffersink_ctx, abuffersink, nullptr,
		NULL, NULL,graph);
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
	auto aformat = make_audio_filter_aformat(graph, p.output.sample_rate, p.output.sample_fmt,p.output.nb_channels, args, sizeof(args));
	avfilter_link(current_ctx, 0,aformat , 0);
	current_ctx = aformat;

	//出力層.
	auto abuffersink = make_audio_filter_abuffersink(graph);
	avfilter_link(current_ctx, 0, abuffersink, 0);
	current_ctx = abuffersink;

	int ret = avfilter_graph_config(graph, NULL);

	return new filter_ctx{ graph,abuffersrc,abuffersink };
}


class tiny_writer {

};



void test_simple_recode() {
	AVFormatContext* ofmt_ctx = nullptr;
	const char* file_path = "";

	int ret = avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, file_path);
	auto video_enc = add_stream(ofmt_ctx, "libopenh264", codec_params());
	auto audio_enc = add_stream(ofmt_ctx, "aac", codec_params());
	fifo_ctx fifo(audio_enc->sample_fmt,audio_enc->sample_rate,audio_enc->ch_layout,audio_enc->frame_size);
	

	log_writer(av_dump_format(ofmt_ctx, 0, file_path, 1));

	//Fileの出力を必要とするのかの条件式.!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)
	ret = avio_open(&ofmt_ctx->pb, file_path, AVIO_FLAG_WRITE);
	//headerの初期化.
	ret = avformat_write_header(ofmt_ctx, NULL);


	//av_interleaved_write_frame(ofmt_ctx, enc_pkt);

	/*
	video_filter_params v_par;
	v_par.input.width = 800;
	v_par.input.height = 600;
	v_par.input.pix_fmt = AV_PIX_FMT_YUYV422;
	v_par.input.time_base = { 1,AV_TIME_BASE };

	v_par.output.width = video_enc->width;
	v_par.output.height = video_enc->height;
	v_par.output.pix_fmt = video_enc->pix_fmt;

	auto video_flt_ctx = make_video_filter_ctx(v_par);

	audio_filter_params a_par;
	a_par.input.nb_channels = 2;
	a_par.input.sample_rate = 44100;
	a_par.input.sample_fmt = AV_SAMPLE_FMT_FLT;
	a_par.input.time_base = { 1,44100 };
	a_par.output.nb_channels = audio_enc->ch_layout.nb_channels;
	a_par.output.sample_rate = audio_enc->sample_rate;
	a_par.output.sample_fmt = audio_enc->sample_fmt;

	auto audio_flt_ctx = make_audio_filter_ctx(a_par);
	*/



	ret = av_write_trailer(ofmt_ctx);
	avformat_free_context(ofmt_ctx);



}
