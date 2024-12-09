#include"filter.h"

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
		"w=%d:h=%d:x=%d:y=%d:color=red",
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

AVFilterContext* make_video_filter_crop(AVFilterGraph* graph, int crop_x, int crop_y, int crop_width, int crop_height, char* args, int nb_args) {
	const AVFilter* crop = avfilter_get_by_name("crop");
	snprintf(args, nb_args,
		"x=%d:y=%d:w=%d:h=%d",
		crop_x, crop_y, crop_width, crop_height);
	AVFilterContext* crop_ctx = NULL;
	avfilter_graph_create_filter(&crop_ctx, crop, NULL, args, NULL, graph);
	return crop_ctx;
}

filter_ctx* make_video_filter_ctx(video_filter_params& p) {
	char args[512];
	AVFilterGraph* graph = avfilter_graph_alloc();
	AVFilterContext* current_ctx = nullptr;
	int cw = p.input.width;
	int ch = p.input.height;
	//入力層.
	auto buffersrc = make_video_filter_buffer(graph, p.input.width, p.input.height, p.input.pix_fmt, p.input.time_base, args, sizeof(args));
	current_ctx = buffersrc;

	//クロップ層.
	if (0 <= p.input_crop.lt.x && 0 <= p.input_crop.lt.y && 1 <= p.input_crop.size.x && 1 <= p.input_crop.size.y) {
		auto crop = make_video_filter_crop(graph, p.input_crop.lt.x, p.input_crop.lt.y, p.input_crop.size.x, p.input_crop.size.y, args, sizeof(args));
		avfilter_link(current_ctx, 0, crop, 0);
		current_ctx = crop;
		cw = p.input_crop.size.x;
		ch = p.input_crop.size.y;
	}

	//スケーリング層.
	if (1 <= p.scaled_size.x && 1<= p.scaled_size.y){//p.input.width != p.output.width || p.input.height != p.output.height) {
		auto scale = make_video_filter_scale(graph, p.scaled_size.x, p.scaled_size.y, args, sizeof(args));
		avfilter_link(current_ctx, 0, scale, 0);
		current_ctx = scale;
		cw = p.scaled_size.x;
		ch = p.scaled_size.y;
	}
	//pad層.
	if (0 <= p.output_locate.x && 0 <= p.output_locate.y) {
		auto pad = make_video_filter_pad(graph, p.output.width, p.output.height, p.output_locate.x, p.output_locate.y, args, sizeof(args));
		avfilter_link(current_ctx, 0, pad, 0);
		current_ctx = pad;
		cw = p.output.width;
		ch = p.output.height;
	}

	//出力と現在のフレームサイズが違っていたらスケーリングする.
	if (cw != p.output.width || ch != p.output.height) {
		auto scale = make_video_filter_scale(graph, p.output.width, p.output.height, args, sizeof(args));
		avfilter_link(current_ctx, 0, scale, 0);
		current_ctx = scale;
		cw = p.output.width;
		ch = p.output.height;
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


filter_ctx* make_audio_filter_ctx(audio_filter_params& p) {
	char args[512];
	AVFilterGraph* graph = avfilter_graph_alloc();
	AVFilterContext* current_ctx = nullptr;
	//入力層.
	auto abuffersrc = make_audio_filter_abuffer(graph, p.input.sample_rate, p.input.sample_fmt, p.input.time_base, p.input.nb_channels, args, sizeof(args));
	current_ctx = abuffersrc;

	//format層.
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
