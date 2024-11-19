#include"f4mthird.h";

#define PNT(a){printf("%s\n",a);}

#define SHOW_LOG_WRITER 1

#if SHOW_LOG_WRITER
#define log_writer(a){printf("[log_writer]");a;}
#else
#define log_writer(a){}
#endif


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


void test_simple_recode() {
	AVFormatContext* ofmt_ctx = nullptr;
	const char* file_path = "C:\\Users\\MaMaM\\source\\repos\\f4mthird\\f4mthird\\source\\writer_test.mp4";

	int ret = avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, file_path);
	auto video_enc = add_stream(ofmt_ctx, "libopenh264", codec_params());
	auto audio_enc = add_stream(ofmt_ctx, "aac", codec_params());

	log_writer(av_dump_format(ofmt_ctx, 0, file_path, 1));

	//Fileの出力を必要とするのかの条件式.!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)
	ret = avio_open(&ofmt_ctx->pb, file_path, AVIO_FLAG_WRITE);
	//headerの初期化.
	ret = avformat_write_header(ofmt_ctx, NULL);

	ret = av_write_trailer(ofmt_ctx);
	avformat_free_context(ofmt_ctx);



}
