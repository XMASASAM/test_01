#include"f4m.h";
#include"filter.h"
#define PNT(a){printf("%s\n",a);}

#define SHOW_LOG_WRITER 0

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
	bool has_audio_frame = true;
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
	~fifo_ctx() {
		av_audio_fifo_free(fifo);
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

AVCodecContext* make_enc_ctx(const char* enc_name, codec_params params, int flags) {

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

	if (flags & AVFMT_GLOBALHEADER)//ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
		enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	int ret = avcodec_open2(enc_ctx, enc_ctx->codec, NULL);

	return enc_ctx;
}

AVCodecContext* make_enc_ctx(AVCodecContext* in_enc_ctx) {//, int flags) {
	const AVCodec* enc = avcodec_find_encoder(in_enc_ctx->codec_id);
	if (enc == nullptr) {
		printf("Error encoder name\n");
		return nullptr;
	}

	auto enc_ctx = avcodec_alloc_context3(enc);
	AVCodecParameters* in_enc_params = avcodec_parameters_alloc();
	avcodec_parameters_from_context(in_enc_params, in_enc_ctx);
	avcodec_parameters_to_context(enc_ctx, in_enc_params);
	avcodec_parameters_free(&in_enc_params);

	if (in_enc_ctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER)//ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
		enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	enc_ctx->time_base = in_enc_ctx->time_base;
	int ret = avcodec_open2(enc_ctx, enc_ctx->codec, NULL);

	return enc_ctx;
}

AVCodecContext* make_dec_ctx(AVCodecContext* in_enc_ctx) {
	const AVCodec* dec = avcodec_find_decoder(in_enc_ctx->codec_id);
	if (dec == nullptr) {
		printf("Error encoder name\n");
		return nullptr;
	}


	auto dec_ctx = avcodec_alloc_context3(dec);
	AVCodecParameters* in_enc_params = avcodec_parameters_alloc();

	avcodec_parameters_from_context(in_enc_params, in_enc_ctx);

	avcodec_parameters_to_context(dec_ctx, in_enc_params);
	avcodec_parameters_free(&in_enc_params);


	if (in_enc_ctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER)//ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
		dec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	dec_ctx->time_base = in_enc_ctx->time_base;

	int ret = avcodec_open2(dec_ctx, dec_ctx->codec, NULL);
	return dec_ctx;
}


AVCodecContext* add_stream(AVFormatContext* ofmt_ctx, const char* enc_name, codec_params params) {
	auto new_stream = avformat_new_stream(ofmt_ctx, nullptr);
	auto enc_ctx = make_enc_ctx(enc_name, params, ofmt_ctx->oformat->flags);


	avcodec_parameters_from_context(new_stream->codecpar, enc_ctx);

	return enc_ctx;
}


struct writer_params {
	const char* video_encoder_name;
	const char* audio_encoder_name;
	codec_params encoder_params;
};

class accumulate_frame_process {
	struct stream_ctx {
		AVCodecContext* dec_ctx;
		AVCodecContext* enc_ctx;
		int stream_index = 0;
		//AVFrame* read_frame = av_frame_alloc();
		AVPacket* read_packet = av_packet_alloc();
		std::queue<AVPacket*> input_packet;
		std::queue<AVPacket*> output_packet;

		std::queue<AVFrame*> frames;
		AVFrame* read_frame = av_frame_alloc();
		int64_t fromt_time_ms = LLONG_MIN;
		int64_t offset_packet_pts = 0;
		~stream_ctx()
		{
			avcodec_free_context(&dec_ctx);
			avcodec_free_context(&enc_ctx);
			av_frame_free(&read_frame);
			av_packet_free(&read_packet);
		}
		void set_offset_pts(int64_t pts_ms) {
			offset_packet_pts = av_rescale_q(pts_ms, { 1,1000 }, enc_ctx->time_base);
		}

		int64_t get_time_ms_from_packet(AVPacket* packet) {
			return av_rescale_q(packet->pts, packet->time_base, { 1,1000 });
		}

		void push_packet(AVPacket* pkt) {
			AVPacket* pkt2 = av_packet_clone(pkt);
			if (input_packet.size() == 0) {
				fromt_time_ms = get_time_ms_from_packet(pkt);
				//				offset_packet_pts = 
			}
			input_packet.push(pkt2);
			return;
		}
		void pop_packet() {
			if (input_packet.size() == 0)return;
			auto front_pkt = input_packet.front();
			send_packet(front_pkt);
			input_packet.pop();
			av_packet_free(&front_pkt);

			if (input_packet.size()) {
				fromt_time_ms = get_time_ms_from_packet(input_packet.front());
			}
		}

		void pop_frame() {
			if (frames.size() == 0)return;
			auto front_pkt = frames.front();
			front_pkt->pts -= offset_packet_pts;
			send_frame(front_pkt);
			frames.pop();
			av_frame_free(&front_pkt);
		}

		AVPacket* pop_output_packet() {
			AVPacket* ans = nullptr;
			if (output_packet.size() == 0)return ans;
			ans = output_packet.front();
			output_packet.pop();
			ans->time_base = enc_ctx->time_base;//video_stream.enc_ctx->time_base;
			ans->stream_index = stream_index;
			return ans;
		}

		void send_packet(AVPacket* pkt) {
			int ret = avcodec_send_packet(dec_ctx, pkt);
			while (0 <= avcodec_receive_frame(dec_ctx, read_frame)) {
				AVFrame* frame = av_frame_clone(read_frame);
				//		frame->pts = pkt->pts;
				frame->time_base = dec_ctx->time_base;
				frames.push(frame);
				av_frame_unref(read_frame);
			}
		}

		void send_frame(AVFrame* frame) {

			int ret = avcodec_send_frame(enc_ctx, frame);

			while (0 <= avcodec_receive_packet(enc_ctx, read_packet)) {
				AVPacket* packet = av_packet_clone(read_packet);
				output_packet.push(packet);
				av_packet_unref(read_packet);
			}
		}

		AVFrame* pop_frame_2() {
			if (frames.size() == 0) {
				pop_packet();
			}

			if (frames.size() == 0) {
				send_packet(nullptr);
			}
			AVFrame* f = nullptr;
			if (frames.size() != 0) {
				f = frames.front();
				f->pts -= offset_packet_pts;
				frames.pop();
			}
			return f;
			//			send_frame(front_pkt);
		}

		AVPacket* pop_output_packet_2() {
			if (offset_packet_pts == 0) {
				if (input_packet.size() == 0)return nullptr;
				auto front_pkt = input_packet.front();
				input_packet.pop();
				return front_pkt;
			}

			AVPacket* pkt = pop_output_packet();
			if (pkt != nullptr)return pkt;


			while (!output_packet.size()) {
				if (input_packet.empty() && frames.empty())break;
				pop_packet();
				pop_frame();
			}
			pkt = pop_output_packet();
			if (pkt != nullptr)return pkt;


			send_packet(nullptr);
			while (frames.size() && !output_packet.size()) {
				pop_frame();
			}

			pkt = pop_output_packet();
			if (pkt != nullptr)return pkt;

			send_frame(nullptr);

			pkt = pop_output_packet();
			return pkt;


		}
		void free_input_packet() {
			pop_packet();
			while (frames.size()) {
				auto f = frames.front();
				av_frame_free(&f);
				frames.pop();
			}
		}
		/*
		void flush(){
			while (input_packet.size()) {
				pop_packet();
				pop_frame();
			}
			send_packet(nullptr);

			while (frames.size()) {
				pop_frame();
			}
			send_frame(nullptr);

		}*/
	};
	bool detect_accumulate_frame_pop() {
		current_min_time = (std::min)(video_stream.fromt_time_ms, audio_stream.fromt_time_ms);
		//	log_writer(printf("duration:%lld[ms]\n",current_max_time - current_min_time));
			//現在の録画期間
		if (max_duration_ms < current_max_time - current_min_time)return true;
		//memory 100MB以下の場合はpop
		if (GetAvailPhysMB() < 100)return true;
		return false;
	}
	int64_t max_duration_ms;
	int64_t current_max_time = 0;
	int64_t current_min_time = LLONG_MAX;
	stream_ctx video_stream;
	stream_ctx audio_stream;
	int64_t offset_packet_pts_ms = 0;
public:
	accumulate_frame_process(AVCodecContext* input_video_enc_ctx, AVCodecContext* input_audio_enc_ctx, const char* out_enc_name, int64_t max_duration_ms) {
		this->max_duration_ms = max_duration_ms;
		{
			stream_ctx& st = video_stream;
			st.dec_ctx = make_dec_ctx(input_video_enc_ctx);
			st.enc_ctx = make_enc_ctx(input_video_enc_ctx);
			video_stream.stream_index = 0;
		}

		{
			stream_ctx& st = audio_stream;
			st.dec_ctx = make_dec_ctx(input_audio_enc_ctx);
			st.enc_ctx = make_enc_ctx(input_audio_enc_ctx);
			audio_stream.stream_index = 1;
		}


	}
	~accumulate_frame_process()
	{

	}

	void push_packet(AVPacket* packet) {
		current_max_time = (std::max)(current_max_time, av_rescale_q(packet->pts, packet->time_base, { 1,1000 }));

		if (packet->stream_index == 0) {
			video_stream.push_packet(packet);
		}
		else {
			audio_stream.push_packet(packet);
		}

		while (detect_accumulate_frame_pop()) {
			accumulate_frame_pop();
		}

	}

	void accumulate_frame_pop() {

		video_stream.free_input_packet();
		while (audio_stream.input_packet.size() && (audio_stream.fromt_time_ms < video_stream.fromt_time_ms)) {
			audio_stream.free_input_packet();
		}
		offset_packet_pts_ms = video_stream.fromt_time_ms;
		video_stream.set_offset_pts(offset_packet_pts_ms);
		audio_stream.set_offset_pts(offset_packet_pts_ms);
	}
	void flush() {
		//video_stream.flush();
		//audio_stream.flush();
	}
	AVPacket* pop_packet() {
		AVPacket* ans = nullptr;

		ans = video_stream.pop_output_packet_2();
		if (ans != nullptr)return ans;
		ans = audio_stream.pop_output_packet_2();
		if (ans != nullptr)return ans;
		return ans;
	}
	AVFrame* pop_frame() {
		AVFrame* ans = nullptr;
		if (video_stream.fromt_time_ms <= audio_stream.fromt_time_ms) {
			ans = video_stream.pop_frame_2();
		}
		else {
			ans = audio_stream.pop_frame_2();
		}

		if (ans == nullptr)ans = video_stream.pop_frame_2();
		if (ans == nullptr)ans = audio_stream.pop_frame_2();

		return ans;
	}

	int64_t get_duration_ms() {
		return current_max_time - current_min_time;
	}

};



class writer1 {
	struct stream_ctx {
		AVCodecContext* enc_ctx;
		int stream_index;
		int64_t offset_pts;
		int64_t latest_pts = 0;
	};
	AVFormatContext* ofmt_ctx = nullptr;
	stream_ctx video_stream;
	stream_ctx audio_stream;
	AVPacket* read_packet = av_packet_alloc();
	//	int64_t offset_pts_ms=0;
	int64_t current_duration_ms = 0;
	AVFrame* silent_frame;
public:
	writer1(const char* file_path, writer_params& p, int64_t offset_ms) {
		int ret = avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, file_path);
		int stream_index = 0;

		if (!str_null_or_empty(p.video_encoder_name)) {
			video_stream.enc_ctx = add_stream(ofmt_ctx, p.video_encoder_name, p.encoder_params);
			video_stream.stream_index = stream_index++;
			video_stream.offset_pts = av_rescale_q(offset_ms, { 1,1000 }, video_stream.enc_ctx->time_base);
		}

		if (!str_null_or_empty(p.audio_encoder_name)) {
			audio_stream.enc_ctx = add_stream(ofmt_ctx, p.audio_encoder_name, p.encoder_params);
			audio_stream.stream_index = stream_index++;
			audio_stream.offset_pts = av_rescale_q(offset_ms, { 1,1000 }, audio_stream.enc_ctx->time_base);

		}

		log_writer(av_dump_format(ofmt_ctx, 0, file_path, 1));

		//Fileの出力を必要とするのかの条件式.!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)
		ret = avio_open(&ofmt_ctx->pb, file_path, AVIO_FLAG_WRITE);
		log_writer(check_error(ret, "avio_open"));
		//headerの初期化.
		ret = avformat_write_header(ofmt_ctx, NULL);
		log_writer(check_error(ret, "avformat_write_header"));
		//offset_pts_ms = offset_ms;
	//	thread_write = std::thread(&writer2::write_loop, this);

		auto e = audio_stream.enc_ctx;

		AVFrame* f = av_frame_alloc();
		f->format = e->sample_fmt;
		f->ch_layout = e->ch_layout;
		f->sample_rate = e->sample_rate;
		f->nb_samples = e->frame_size;
		f->time_base = e->time_base;
		av_frame_get_buffer(f, 0);
		// 無音データの設定 
		memset(f->data[0], 0, f->nb_samples * av_get_bytes_per_sample(e->sample_fmt));
		silent_frame = f;

	}

	void input_frame(AVFrame* frame) {
		stream_ctx& st = detect_video_frame(frame) ? video_stream : audio_stream;
		frame->pts -= st.offset_pts;
		if (detect_video_frame(frame)) {
			input_frame(AVMediaType::AVMEDIA_TYPE_VIDEO, frame);
			st.latest_pts = frame->pts;
		}
		else {
			set_silent_frame(frame->pts,frame->time_base);
			input_frame(AVMediaType::AVMEDIA_TYPE_AUDIO, frame);
			st.latest_pts =(std::max)(st.latest_pts , frame->pts + frame->nb_samples);
		}
	}
	void set_silent_frame(int64_t to_frame_pts, AVRational time_base) {
		int64_t target_pts = av_rescale_q(to_frame_pts, time_base, {1,silent_frame->sample_rate});
		int64_t one_frame_duration = silent_frame->nb_samples;//av_rescale_q(silent_audio_frame->nb_samples, {1,silent_audio_frame->sample_rate});
		while (one_frame_duration < target_pts - audio_stream.latest_pts) {
			silent_frame->pts = audio_stream.latest_pts;
			input_frame(AVMediaType::AVMEDIA_TYPE_AUDIO,silent_frame);
			audio_stream.latest_pts = audio_stream.latest_pts + silent_frame->nb_samples;//(std::max)(latest_pts, frame->pts + frame->nb_samples);

		}
	}

	//勝手にpts変えてます.
	void input_frame(AVMediaType type, AVFrame* frame) {
		stream_ctx& st = (type == AVMediaType::AVMEDIA_TYPE_VIDEO) ? video_stream : audio_stream;


		if (frame != nullptr) {
			if (frame->pts < 0)return;

			int64_t pts_ms = av_rescale_q(frame->pts, frame->time_base, { 1,1000 });
			current_duration_ms = (std::max)(current_duration_ms, pts_ms);
			
		}

		avcodec_send_frame(st.enc_ctx, frame);
		while (0 <= avcodec_receive_packet(st.enc_ctx, read_packet)) {
			av_packet_rescale_ts(read_packet,
				st.enc_ctx->time_base,
				ofmt_ctx->streams[st.stream_index]->time_base);
			read_packet->stream_index = st.stream_index;
			int ret = av_interleaved_write_frame(ofmt_ctx, read_packet);
			av_packet_unref(read_packet);
		}
	}


	void close() {
		input_frame(AVMediaType::AVMEDIA_TYPE_VIDEO, nullptr);
		set_silent_frame(video_stream.latest_pts,video_stream.enc_ctx->time_base);
		input_frame(AVMediaType::AVMEDIA_TYPE_AUDIO, nullptr);

		int ret = av_write_trailer(ofmt_ctx);
		avformat_free_context(ofmt_ctx);
		avcodec_free_context(&video_stream.enc_ctx);
		avcodec_free_context(&audio_stream.enc_ctx);
		av_packet_free(&read_packet);

	}
	int64_t get_duration_ms() {
		return current_duration_ms;
	}

};







class writer2 {
protected:
	struct stream_ctx {
		AVCodecContext* enc_ctx;
		int stream_index;
		int64_t latest_frame_pts = LLONG_MIN;
		//int64_t passed_packet_pts = 0;
		int64_t passed_frame_pts = 0;
		AVCodecParameters* par = avcodec_parameters_alloc();
		//	int64_t latest_encoder_pts=LLONG_MIN;

	};
	struct write_frame_t {
		AVMediaType type;
		share_frame frame;
		bool has_silent_audio_frame = false;
	};

	//	AVFormatContext* ofmt_ctx = nullptr;
	stream_ctx video_stream;
	stream_ctx audio_stream;
	AVPacket* read_packet = av_packet_alloc();
	std::thread thread_write;
	std::mutex thread_write_mtx;
	std::mutex frame_queue_mtx;
	std::condition_variable thread_write_cond;
	std::queue<write_frame_t> thread_write_queue;
	bool is_active = true;
	share_frame silent_audio_frame = nullptr;
	accumulate_frame_process* accumulate_frame;
	writer_params params;
	std::string file_path;
public:
	writer2(const char* file_path, writer_params& p) {
		params = p;
		this->file_path = file_path;
		//		int ret = avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, file_path);
		int stream_index = 0;

		if (!str_null_or_empty(p.video_encoder_name)) {
			video_stream.enc_ctx = make_enc_ctx(p.video_encoder_name, p.encoder_params, 0); //add_stream(ofmt_ctx, p.video_encoder_name, p.encoder_params);
			video_stream.stream_index = stream_index++;
			avcodec_parameters_from_context(video_stream.par, video_stream.enc_ctx);
		}

		if (!str_null_or_empty(p.audio_encoder_name)) {
			audio_stream.enc_ctx = make_enc_ctx(p.audio_encoder_name, p.encoder_params, 0);//add_stream(ofmt_ctx, p.audio_encoder_name, p.encoder_params);
			audio_stream.stream_index = stream_index++;
			avcodec_parameters_from_context(audio_stream.par, audio_stream.enc_ctx);

		}


		log_writer(av_dump_format(ofmt_ctx, 0, file_path, 1));

		//Fileの出力を必要とするのかの条件式.!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)
//		ret = avio_open(&ofmt_ctx->pb, file_path, AVIO_FLAG_WRITE);
//		log_writer(check_error(ret, "avio_open"));
		//headerの初期化.
//		ret = avformat_write_header(ofmt_ctx, NULL);
		log_writer(check_error(ret, "avformat_write_header"));
		thread_write = std::thread(&writer2::write_loop, this);



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
		accumulate_frame = new accumulate_frame_process(video_stream.enc_ctx, audio_stream.enc_ctx, "h264", 10 * 60 * 1000);

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
			stream_ctx.passed_frame_pts = frame->pts + frame->nb_samples;
		}

		if (type == AVMEDIA_TYPE_AUDIO && frame != nullptr) {
			log_writer(printf("audio_nb_frames:%d\n", frame->nb_samples));
		}

		avcodec_send_frame(enc_ctx, frame.get());

		while (avcodec_receive_packet(enc_ctx, read_packet) == 0) {
			read_packet->stream_index = stream_ctx.stream_index;
			read_packet->time_base = stream_ctx.enc_ctx->time_base;//ofmt_ctx->streams[read_packet->stream_index]->time_base;//accumulate_frameの処理でtime_baseを利用するため.
			//			stream_ctx.passed_packet_pts = (std::max)(stream_ctx.passed_packet_pts, read_packet->pts);

			accumulate_frame->push_packet(read_packet);
			av_packet_unref(read_packet);
			continue;
			//上は検証用.

//			av_packet_rescale_ts(read_packet,
//				enc_ctx->time_base,
//				ofmt_ctx->streams[stream_ctx.stream_index]->time_base);
//			read_packet->stream_index = stream_ctx.stream_index;
//			log_writer(printf("write_frame pts:%lld\n", read_packet->pts));
//			read_packet->time_base = ofmt_ctx->streams[read_packet->stream_index]->time_base;//accumulate_frameの処理でtime_baseを利用するため.
	//		stream_ctx.passed_packet_pts = (std::max)(stream_ctx.passed_packet_pts, read_packet->pts);
//			accumulate_frame_t node(read_packet);
			//accumulate_frame.push(read_packet);
			//av_interleaved_write_frame(ofmt_ctx, read_packet);
//			accumulate_frame->push_packet(read_packet);
//			av_packet_unref(read_packet);
		}
	}
	void push_frame(AVMediaType type, share_frame frame, bool has_silent_frame) {

		stream_ctx& stream_ctx = (type == AVMEDIA_TYPE_VIDEO ? video_stream : audio_stream);

		if (frame != nullptr) {
			stream_ctx.latest_frame_pts = av_rescale_q(frame->pts, frame->time_base, stream_ctx.enc_ctx->time_base);//ofmt_ctx->streams[stream_ctx.stream_index]->time_base);
		}

		{
			std::lock_guard<std::mutex> lock(frame_queue_mtx);
			thread_write_queue.push({ type,frame ,has_silent_frame });
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
				/*
				if (frame.frame != nullptr && frame.has_silent_audio_frame) {
					int64_t v_pts = av_rescale_q(video_stream.passed_frame_pts, video_stream.enc_ctx->time_base, audio_stream.enc_ctx->time_base);
					while (audio_stream.passed_frame_pts < v_pts) {
						silent_audio_frame->pts = audio_stream.passed_frame_pts;// + silent_audio_frame->nb_samples;
						_write(AVMEDIA_TYPE_AUDIO, silent_audio_frame);
					}
				}*/

			}
		}

		log_writer("writer_base end write file\n");

	}

	void close() {
		push_frame(AVMEDIA_TYPE_VIDEO, nullptr, false);
		push_frame(AVMEDIA_TYPE_AUDIO, nullptr, false);
		is_active = false;
		{
			std::lock_guard<std::mutex> lock(thread_write_mtx);
			thread_write_cond.notify_all();
		}
		thread_write.join();
		accumulate_frame->flush();

		file_path[file_path.size() - 5] = '0';
		writer1* wt1 = new writer1(file_path.c_str(), params, 0);
		int count = 0;
		int div_duration_ms = 60 * 1000;
		int current_max_duration = accumulate_frame->get_duration_ms() % div_duration_ms;
		if (current_max_duration < 1000)current_max_duration += div_duration_ms;
		while (AVFrame* frame = accumulate_frame->pop_frame()) {
			if (current_max_duration <= wt1->get_duration_ms() && detect_video_frame(frame)) {
				count++;
				wt1->close();
				delete wt1;
				file_path[file_path.size() - 5] = count + '0';
				wt1 = new writer1(file_path.c_str(), params, av_rescale_q(frame->pts, frame->time_base, { 1,1000 }));
				current_max_duration = div_duration_ms;
			}
			wt1->input_frame(frame);
			av_frame_free(&frame);
		}
		if (wt1 != nullptr) {
			wt1->close();
			delete wt1;
		}
		/*while (AVPacket* pkt = accumulate_frame->pop_packet()) {
			av_packet_rescale_ts(pkt,
				pkt->time_base,
				ofmt_ctx->streams[pkt->stream_index]->time_base);

			int ret = av_interleaved_write_frame(ofmt_ctx, pkt);
			av_packet_free(&pkt);
		}*/
		delete accumulate_frame;


		//		int ret = av_write_trailer(ofmt_ctx);
		//		avformat_free_context(ofmt_ctx);
		avcodec_free_context(&video_stream.enc_ctx);
		avcodec_free_context(&audio_stream.enc_ctx);
		avcodec_parameters_free(&video_stream.par);
		avcodec_parameters_free(&audio_stream.par);
		av_packet_free(&read_packet);
	}
	AVCodecParameters* get_codec_params(AVMediaType type) {
		if (type == AVMEDIA_TYPE_VIDEO && video_stream.enc_ctx != nullptr) {
			return video_stream.par;//ofmt_ctx->streams[video_stream.stream_index]->codecpar;
		}
		else if (type == AVMEDIA_TYPE_AUDIO && audio_stream.enc_ctx != nullptr) {
			return audio_stream.par;//ofmt_ctx->streams[audio_stream.stream_index]->codecpar;
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
			lpts = av_rescale_q(video_stream.latest_frame_pts, video_stream.enc_ctx->time_base, { 1,AV_TIME_BASE });//ofmt_ctx->streams[video_stream.stream_index]->time_base, { 1,AV_TIME_BASE });
		}
		if (has_audio_stream()) {
			auto t = av_rescale_q(audio_stream.latest_frame_pts, audio_stream.enc_ctx->time_base, { 1,AV_TIME_BASE });//ofmt_ctx->streams[audio_stream.stream_index]->time_base, { 1,AV_TIME_BASE });
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
	bool recieve_audio_frame = true;
	std::condition_variable vasyn_cond_val;
	std::mutex vasyn_cond_mtx;
	std::mutex thread_mtx;
	bool is_active = true;

	void write_frame_with_offset(share_frame frame) {
		frame->pts += av_rescale_q(offset_input_pts_tb1e6, { 1,AV_TIME_BASE }, frame->time_base);
		//writer2::write(frame);
		AVMediaType type = detect_video_frame(frame.get()) ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
		push_frame(type, frame, !recieve_audio_frame);
		log_writer(printf("latest_pts:%lld\n", get_current_duration_tn1e6() / AV_TIME_BASE));
	}

	void vasyn_write_frame() {//ここでは0からのptsが入力される予定.
		while (auto frame = vasyn->get_frame()) {
			write_frame_with_offset(frame);
		}
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

	void _set_vasyn(bool is_input_audio_frame) {
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
		if (offset_input_pts_tb1e6 < 0) {
			offset_input_pts_tb1e6 = 0;
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


public:
	writer3(const char* file_path, writer_params& p) :writer2(file_path, p) {

		//AVFrame* f = //silent_audio_frame;

	}
	void set_vasyn(bool is_input_audio_frame) {

		//	std::lock_guard<std::mutex> lock(thread_mtx);
		_set_vasyn(is_input_audio_frame);
	}

	int64_t get_current_vasyn_duration_tb1e6() {
		//	std::lock_guard<std::mutex> lock(thread_mtx);
		int64_t d = get_current_duration_tn1e6() - offset_input_pts_tb1e6;
		if (d < 0)return 0;
		return d;

	}


	void write(share_frame frame) {
		//std::lock_guard<std::mutex> lock(thread_mtx);

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
		//std::lock_guard<std::mutex> lock(thread_mtx);

		fifo_write_frame(true, audio_stream.passed_pts);
		writer2::close();
		if (fifo != nullptr)delete fifo;
		if (vasyn != nullptr)delete vasyn;
	}


};



AVCodecContext* capture_get_codec(int64_t capture_ptr, AVMediaType type);
void capture_set_new_frame_event(int64_t capture_device_ptr, std::function<void(void*, share_frame)>* func);
void capture_delete_new_frame_event(int64_t capture_device_ptr, std::function<void(void*, share_frame)>* func);

class writer4 {//single_capture
private:
	writer3* writer_ctx = nullptr;
	video_filter_params v_par;
	filter_ctx* video_flt_ctx = nullptr;
	audio_filter_params a_par;
	filter_ctx* audio_flt_ctx = nullptr;
	std::function<void(void*, share_frame)> new_frame_loaded = std::bind(&writer4::new_frame_event, this, std::placeholders::_1, std::placeholders::_2);
	int64_t current_cap = 0;
	std::queue<share_frame> frame_queue;
	bool is_active;
	std::thread thread_write;
	std::condition_variable thread_write_cond;
	std::mutex thread_write_mtx;
	std::mutex frame_queue_mtx;
	std::mutex filter_change_mtx;
	std::mutex capture_change_mtx;
	writer_params writer_param;
public:
	writer4(const char* file_path) {
		//const char* file_path = "C:\\Users\\Videos\\Test\\test.mp4";
		//get_codec_supported_pixformat(avcodec_find_encoder_by_name("jpeg2000"));
		//const char* file_path = "C:\\Users\\MaMaM\\source\\repos\\f4mthird\\f4mthird\\source\\test.mp4";
		writer_params wp;
		wp.video_encoder_name = "libopenh264";
		//mjpeg
	///	wp.video_encoder_name = "libvpx-vp9";
	//	wp.video_encoder_name = "h264";
		wp.audio_encoder_name = "aac";
		wp.encoder_params.width = 640;
		wp.encoder_params.height = 480;
		wp.encoder_params.sample_rate = 44100;
		wp.encoder_params.nb_channels = 1;
		writer_ctx = new writer3(file_path, wp);
		is_active = true;
		thread_write = std::thread(&writer4::_write_loop, this);
		writer_param = wp;
	}

	void write(int64_t capture_ptr) {

		if (current_cap != 0) {
			capture_delete_new_frame_event(current_cap, &new_frame_loaded);
		}

		std::lock_guard<std::mutex> lock(capture_change_mtx);
		auto vdec = capture_get_codec(capture_ptr, AVMEDIA_TYPE_VIDEO);
		auto adec = capture_get_codec(capture_ptr, AVMEDIA_TYPE_AUDIO);
		writer_ctx->set_vasyn(adec != nullptr);

		current_cap = capture_ptr;
		if (video_flt_ctx != nullptr) {
			delete video_flt_ctx;
			video_flt_ctx = nullptr;
		}
		if (audio_flt_ctx != nullptr) {
			delete audio_flt_ctx;
			audio_flt_ctx = nullptr;
		}
		v_par = video_filter_params();

		v_par.input.width = vdec->width;
		v_par.input.height = vdec->height;
		v_par.input.pix_fmt = vdec->pix_fmt;
		v_par.input.time_base = vdec->pkt_timebase;

		auto wvc = ((writer2*)writer_ctx)->get_codec_params(AVMEDIA_TYPE_VIDEO);
		v_par.output.width = wvc->width;
		v_par.output.height = wvc->height;
		v_par.output.pix_fmt = (AVPixelFormat)wvc->format;

		video_flt_ctx = make_video_filter_ctx(v_par);

		if (adec != nullptr) {

			a_par.input.nb_channels = adec->ch_layout.nb_channels;
			a_par.input.sample_rate = adec->sample_rate;
			a_par.input.sample_fmt = adec->sample_fmt;
			a_par.input.time_base = adec->pkt_timebase;

			auto wac = ((writer2*)writer_ctx)->get_codec_params(AVMEDIA_TYPE_AUDIO);
			a_par.output.nb_channels = wac->ch_layout.nb_channels;
			a_par.output.sample_rate = wac->sample_rate;
			a_par.output.sample_fmt = (AVSampleFormat)wac->format;

			audio_flt_ctx = make_audio_filter_ctx(a_par);
		}

		capture_set_new_frame_event(capture_ptr, &new_frame_loaded);

	}

	void set_crop(int crop_x, int crop_y, int crop_w, int crop_h) {

		std::lock_guard<std::mutex> lock(filter_change_mtx);

		if (video_flt_ctx != nullptr) {
			delete video_flt_ctx;
			video_flt_ctx = nullptr;
		}

		//video_filter_params v_par;
		v_par.input_crop.lt.x = crop_x;
		v_par.input_crop.lt.y = crop_y;
		v_par.input_crop.size.x = crop_w;
		v_par.input_crop.size.y = crop_h;
		int scaled_x = 0;
		int scaled_y = 0;
		get_aspect(writer_param.encoder_params.width, writer_param.encoder_params.height, crop_w, crop_h,&scaled_x,&scaled_y);

		v_par.scaled_size.x = scaled_x;
		v_par.scaled_size.y = scaled_y;

		v_par.output_locate.x = (writer_param.encoder_params.width - scaled_x) / 2;
		v_par.output_locate.y = (writer_param.encoder_params.height - scaled_y) / 2;
		video_flt_ctx = make_video_filter_ctx(v_par);

	}


	void _write_loop() {
		while (is_active || frame_queue.size()) {
			{
				std::unique_lock<std::mutex> lock(thread_write_mtx);
				thread_write_cond.wait(lock, [&] {return frame_queue.size() || !is_active; });
			}
			int queue_size;
			share_frame frame;

			{
				std::lock_guard<std::mutex> lock(capture_change_mtx);
				while (true) {

					{
						std::lock_guard<std::mutex> lock(frame_queue_mtx);
						queue_size = frame_queue.size();
						log_writer(printf("writer4 queue_size:%d\n", queue_size));
						if (queue_size <= 0)break;
						frame = frame_queue.front();
						frame_queue.pop();
					}

					{//やりたいこと.
						std::lock_guard<std::mutex> lock(filter_change_mtx);

						filter_ctx* flt_ctx = detect_video_frame(frame.get()) ? video_flt_ctx : audio_flt_ctx;
						AVFrame* tmp_frame = av_frame_clone(frame.get());//先にallocしれrefし続ける方がいいかも？.
						av_buffersrc_add_frame(flt_ctx->buffersrc_ctx, tmp_frame);
						av_frame_free(&tmp_frame);
						while (true) {
							AVFrame* flt_frame = av_frame_alloc();
							int ret = av_buffersink_get_frame(flt_ctx->buffersink_ctx, flt_frame);
							if (ret < 0) {
								av_frame_free(&flt_frame);
								break;
							}
							flt_frame->time_base = av_buffersink_get_time_base(flt_ctx->buffersink_ctx);
							writer_ctx->write(make_share_frame(flt_frame));
							log_writer(printf("writer4_input_frame:%d\n", queue_size));

						}

					}
					log_writer(printf("writer4_thread while\n"));
				}
			}
			log_writer(printf("writer4_thread while outside\n"));

		}

		log_writer(printf("writer4_thread writer_ctx->close\n"));

		writer_ctx->close();

		log_writer(printf("writer4_thread end\n"));

	}

	//これをどうやって呼び出すのか....
	void new_frame_event(void* sender, share_frame frame) {
		{
			std::lock_guard<std::mutex> lock(frame_queue_mtx);
			frame_queue.push(frame);
		}

		{
			std::lock_guard<std::mutex> lock(thread_write_mtx);
			thread_write_cond.notify_all();
		}
	}

	void close() {
		log_writer(printf("writer4_close\n"));

		if (current_cap != 0) {
			capture_delete_new_frame_event(current_cap, &new_frame_loaded);
		}
		is_active = false;
		log_writer(printf("writer4_thread_writer_join\n"));

		{
			std::lock_guard<std::mutex> lock(thread_write_mtx);
			thread_write_cond.notify_all();
		}

		thread_write.join();
		delete writer_ctx;
		delete video_flt_ctx;
		delete audio_flt_ctx;
	}

	int64_t get_current_capture_duration() {
		return writer_ctx->get_current_vasyn_duration_tb1e6();
	}

};

EXPORT int f4m_writer_open(const wchar_t* path, int64_t cap_device_ptr, writer4** writer_ptr) {
	*writer_ptr = new writer4(to_utf8(path).c_str());
	(*writer_ptr)->write(cap_device_ptr);
	return 1;
}

EXPORT int f4m_writer_close(writer4** writer_ptr) {
	(*writer_ptr)->close();
	delete* writer_ptr;
	*writer_ptr = nullptr;
	return 1;

}

EXPORT int f4m_writer_change_capture(writer4* writer_ptr , int64_t cap_device_ptr) {
	writer_ptr->write(cap_device_ptr);
	return 1;
}

EXPORT int f4m_writer_set_crop(writer4* writer_ptr , int crop_x, int crop_y, int crop_w, int crop_h) {
	writer_ptr->set_crop(crop_x,crop_y,crop_w,crop_h);
	return 1;
}
