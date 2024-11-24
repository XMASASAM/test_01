#include"f4mthird_utility.h"

bool detect_video_frame(AVFrame* frame) {
	if (frame->width <= 0 || frame->height <= 0)return false;
	return true;
}


void check_error(int ret, const char* com) {
	char buf[255];
	int r = av_strerror(ret, buf, 255);
	if (r == 0) {
		printf("check_error: func=%s : ret=%d : str=%s\n", com, ret, buf);
	}
	else {
		printf("check_error: func=%s : ret=%d : str=%s\n", com, ret, "ok");
	}
}

bool str_null_or_empty(const char* str) {
	if(str == nullptr)return true;
	if(strncmp(str,"",1)==0)return true;
	return false;
}

void av_frame_deleter(AVFrame* frame) {
	av_frame_free(&frame);
}
share_frame make_share_frame(AVFrame* frame) {
	return std::shared_ptr<AVFrame>(frame, av_frame_deleter);
}
