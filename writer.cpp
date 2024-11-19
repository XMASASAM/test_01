#include"qbs.h"
#include"f4m.h"



struct codec_params {
	int width=640;
	int height = 480;
	int sample_rate=44100;
	int nb_channels = 1;
};



void* get_codec_supported_config(const AVCodec* codec,AVCodecConfig conf,int* nb=nullptr) {
	void** pl;
	int* nb_pl;
	if (nb != nullptr)nb_pl = nb;
	int ret = avcodec_get_supported_config(NULL, codec, conf, 0, pl, nb_pl);
	if (*nb_pl <= 0) {
		return nullptr;
	}
	return *pl;
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
int get_codec_supported_best_samplerate(const AVCodec* codec,int best_samplerate=44100) {
	int nb;
	auto p = get_codec_supported_config(codec, AV_CODEC_CONFIG_SAMPLE_RATE,&nb);
	if (p == nullptr)return 0;
	//best_samplerateに近いサンプルレートを選択する.
	int best_index = 0;
	int best_diff = INT_MAX;
	for (int i = 0; i < nb; i++) {
		int dif = abs(best_samplerate - *(int*)(p));
		if (best_diff < dif) {
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

int make_enc_ctx(const char* enc_name,codec_params params) {
//	auto new_stream = avformat_new_stream(ofmt_ctx, NULL);
//	if (new_stream == nullptr) {
//		printf("Error new stream\n");
//		return -1;
//	}
	const AVCodec* enc = avcodec_find_encoder_by_name(enc_name);
	if (enc == nullptr) {
		printf("Error encoder name\n");
		return -1;
	}

	auto enc_ctx = avcodec_alloc_context3(enc);
	if (enc->type == AVMEDIA_TYPE_VIDEO) {
		enc_ctx->width = params.width;
		enc_ctx->height = params.height;
		enc_ctx->sample_aspect_ratio = { 0,1 };
		enc_ctx->pix_fmt = get_codec_supported_pixformat(enc);
		//stream_indexを記録する必要あり.
	}
	else if (enc->type == AVMEDIA_TYPE_AUDIO) {
		enc_ctx->sample_rate = get_codec_supported_best_samplerate(enc,params.sample_rate);
		enc_ctx->sample_fmt = get_codec_supported_sampleformat(enc);
		enc_ctx->time_base = { 1,enc_ctx->sample_rate };
		enc_ctx->ch_layout = get_chlayout(params.nb_channels);

		//av_channel_layout_copy(enc_ctx->ch_layout, );
		//av_channel_layout_from_string();
		//	enc_ctx->sample_fmt = encoder->sample_fmts[0];

//		astream.enc_ctx = enc_ctx;
//		astream.stream_index = stream_count;

//		fifo = av_audio_fifo_alloc(enc_ctx->sample_fmt,
//			enc_ctx->ch_layout.nb_channels, 1);

	}
	else {
		printf("Error encoder type\n");
		return -1;
	}
	
}

EXPORT void test_simple_recode() {
	AVFormatContext* ofmt_ctx = nullptr;
	const char* file_path = "C:\\Users\\N7742\\Videos\\Test\\test.mp4";
	int ret = avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL,file_path);
	auto new_stream = avformat_new_stream(ofmt_ctx,nullptr);
	make_enc_ctx("h264",codec_params());
	
	



}
