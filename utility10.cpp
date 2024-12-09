#include"f4m.h"


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
	if (str == nullptr)return true;
	if (strncmp(str, "", 1) == 0)return true;
	return false;
}


void av_frame_deleter(AVFrame* frame) {
	av_frame_free(&frame);
}
share_frame make_share_frame(AVFrame* frame) {
	return std::shared_ptr<AVFrame>(frame, av_frame_deleter);
}


std::string to_utf8(const wchar_t* src) {
	char* buf;
	int dst_size, rc;

	rc = WideCharToMultiByte(CP_UTF8, 0, src, -1, NULL, 0, NULL, NULL);//CP_ACP
	if (rc == 0) {
		return "";
	}

	dst_size = rc + 1;
	buf = (char*)malloc(dst_size);
	if (buf == NULL) {
		return "";
	}

	rc = WideCharToMultiByte(CP_UTF8, 0, src, -1, buf, dst_size, NULL, NULL);
	if (rc == 0) {
		free(buf);
		return "";
	}
	buf[rc] = '\0';

	std::string ans = buf;

	free(buf);

	return ans;
}

int64_t GetAvailPhysMB() {
	MEMORYSTATUSEX statex;
	statex.dwLength = sizeof(statex);
	int64_t mb = -1;
	if (GlobalMemoryStatusEx(&statex)) {
		mb = statex.ullAvailPhys / (1024 * 1024);
		//		std::cout << "There is " <<  << " MB of available physical memory.\n";
	}
	else {
		std::cerr << "Failed to get memory status.\n";
	}
	return mb;
}

void get_aspect(int x,int y,int aspect_x,int aspect_y,int* aspected_x,int* aspected_y) {
	if (x * aspect_y < y* aspect_x)
	{
		*aspected_x = x;
		*aspected_y = x * (aspect_y / (double)aspect_x);
	}
	else
	{
		*aspected_y = y;
		*aspected_x = y * (aspect_x / (double)aspect_y);
	}
}
