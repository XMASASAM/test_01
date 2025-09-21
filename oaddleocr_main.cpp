// Copyright (c) 2020 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <opencv2/imgcodecs.hpp>

#include <include/args.h>
#include <include/paddlestructure.h>

#include <iostream>
#include <vector>

using namespace PaddleOCR;
#include<Windows.h>
#define EXPORT extern "C" __declspec(dllexport)


PPOCR* sta_ocr = nullptr;


struct ocr_result {
    int x1, y1;
    int x2, y2;
    int x3, y3;
    int x4 ,y4;
    wchar_t* text;
    double score;
};


void check_params(bool is_exit=true) {
  if (FLAGS_det) {
    if (FLAGS_det_model_dir.empty() || FLAGS_image_dir.empty()) {
      std::cout << "Usage[det]: ./ppocr "
                   "--det_model_dir=/PATH/TO/DET_INFERENCE_MODEL/ "
                << "--image_dir=/PATH/TO/INPUT/IMAGE/" << std::endl;
      if(is_exit)exit(1);
    }
  }
  if (FLAGS_rec) {
    std::cout
        << "In PP-OCRv3, rec_image_shape parameter defaults to '3, 48, 320',"
           "if you are using recognition model with PP-OCRv2 or an older "
           "version, "
           "please set --rec_image_shape='3,32,320"
        << std::endl;
    if (FLAGS_rec_model_dir.empty() || FLAGS_image_dir.empty()) {
      std::cout << "Usage[rec]: ./ppocr "
                   "--rec_model_dir=/PATH/TO/REC_INFERENCE_MODEL/ "
                << "--image_dir=/PATH/TO/INPUT/IMAGE/" << std::endl;
      if (is_exit)exit(1);
    }
  }
  if (FLAGS_cls && FLAGS_use_angle_cls) {
    if (FLAGS_cls_model_dir.empty() || FLAGS_image_dir.empty()) {
      std::cout << "Usage[cls]: ./ppocr "
                << "--cls_model_dir=/PATH/TO/REC_INFERENCE_MODEL/ "
                << "--image_dir=/PATH/TO/INPUT/IMAGE/" << std::endl;
      if (is_exit)exit(1);
    }
  }
  if (FLAGS_table) {
    if (FLAGS_table_model_dir.empty() || FLAGS_det_model_dir.empty() ||
        FLAGS_rec_model_dir.empty() || FLAGS_image_dir.empty()) {
      std::cout << "Usage[table]: ./ppocr "
                << "--det_model_dir=/PATH/TO/DET_INFERENCE_MODEL/ "
                << "--rec_model_dir=/PATH/TO/REC_INFERENCE_MODEL/ "
                << "--table_model_dir=/PATH/TO/TABLE_INFERENCE_MODEL/ "
                << "--image_dir=/PATH/TO/INPUT/IMAGE/" << std::endl;
      if (is_exit)exit(1);
    }
  }
  if (FLAGS_layout) {
    if (FLAGS_layout_model_dir.empty() || FLAGS_image_dir.empty()) {
      std::cout << "Usage[layout]: ./ppocr "
                << "--layout_model_dir=/PATH/TO/LAYOUT_INFERENCE_MODEL/ "
                << "--image_dir=/PATH/TO/INPUT/IMAGE/" << std::endl;
      if (is_exit)exit(1);
    }
  }
  if (FLAGS_precision != "fp32" && FLAGS_precision != "fp16" &&
      FLAGS_precision != "int8") {
    std::cout << "precision should be 'fp32'(default), 'fp16' or 'int8'. "
              << std::endl;
    if (is_exit)exit(1);
  }
}

void ocr(std::vector<cv::String> &cv_all_img_names) {
  std::cout << "ppocr: " << "let's init" << std::endl;

  PPOCR ocr;
  std::cout << "bench: " << FLAGS_benchmark << std::endl;

  if (FLAGS_benchmark) {
    ocr.reset_timer();
    std::cout << "bench: " << "ok" << std::endl;

  }

  std::vector<cv::Mat> img_list;
  std::vector<cv::String> img_names;
  for (int i = 0; i < cv_all_img_names.size(); ++i) {
    cv::Mat img = cv::imread(cv_all_img_names[i], cv::IMREAD_COLOR);
    if (!img.data) {
      std::cerr << "[ERROR] image read failed! image path: "
                << cv_all_img_names[i] << std::endl;
      continue;
    }
    img_list.emplace_back(std::move(img));
    img_names.emplace_back(cv_all_img_names[i]);
  }

  std::cout << "img_list: " << "ok" << std::endl;

  std::vector<std::vector<OCRPredictResult>> ocr_results =
      ocr.ocr(img_list, FLAGS_det, FLAGS_rec, FLAGS_cls);
    
  std::cout << "processed_ocr: " << "ok" << std::endl;

  for (int i = 0; i < img_names.size(); ++i) {
    std::cout << "predict img: " << cv_all_img_names[i] << std::endl;
    Utility::print_result(ocr_results[i]);
    if (FLAGS_visualize && FLAGS_det) {
      std::string file_name = Utility::basename(img_names[i]);
      cv::Mat srcimg = img_list[i];
      Utility::VisualizeBboxes(srcimg, ocr_results[i],
                               FLAGS_output + "/" + file_name);
    }
  }
  if (FLAGS_benchmark) {
    ocr.benchmark_log(cv_all_img_names.size());
  }
}

void structure(std::vector<cv::String> &cv_all_img_names) {
  PaddleOCR::PaddleStructure engine;

  if (FLAGS_benchmark) {
    engine.reset_timer();
  }

  for (int i = 0; i < cv_all_img_names.size(); ++i) {
    std::cout << "predict img: " << cv_all_img_names[i] << std::endl;
    cv::Mat img = cv::imread(cv_all_img_names[i], cv::IMREAD_COLOR);
    if (!img.data) {
      std::cerr << "[ERROR] image read failed! image path: "
                << cv_all_img_names[i] << std::endl;
      continue;
    }

    std::vector<StructurePredictResult> structure_results = engine.structure(
        img, FLAGS_layout, FLAGS_table, FLAGS_det && FLAGS_rec);

    for (size_t j = 0; j < structure_results.size(); ++j) {
      std::cout << j << "\ttype: " << structure_results[j].type
                << ", region: [";
      std::cout << structure_results[j].box[0] << ","
                << structure_results[j].box[1] << ","
                << structure_results[j].box[2] << ","
                << structure_results[j].box[3] << "], score: ";
      std::cout << structure_results[j].confidence << ", res: ";

      if (structure_results[j].type == "table") {
        std::cout << structure_results[j].html << std::endl;
        if (structure_results[j].cell_box.size() > 0 && FLAGS_visualize) {
          std::string file_name = Utility::basename(cv_all_img_names[i]);

          Utility::VisualizeBboxes(img, structure_results[j],
                                   FLAGS_output + "/" + std::to_string(j) +
                                       "_" + file_name);
        }
      } else {
        std::cout << "count of ocr result is : "
                  << structure_results[j].text_res.size() << std::endl;
        if (structure_results[j].text_res.size() > 0) {
          std::cout << "********** print ocr result "
                    << "**********" << std::endl;
          Utility::print_result(structure_results[j].text_res);
          std::cout << "********** end print ocr result "
                    << "**********" << std::endl;
        }
      }
    }
  }
  if (FLAGS_benchmark) {
    engine.benchmark_log(cv_all_img_names.size());
  }
}

int main(int argc, char **argv) {
    std::cout << "eee" << std::endl;

//    std::locale::global(std::locale("en_US.UTF-8"));

  // Parsing command-line
  google::ParseCommandLineFlags(&argc, &argv, true);
  check_params();
  std::cout << "FLAGS_output: " << FLAGS_output << std::endl;
  if (!Utility::PathExists(FLAGS_image_dir)) {
    std::cerr << "[ERROR] image path not exist! image_dir: " << FLAGS_image_dir
              << std::endl;
    exit(1);
  }

  std::vector<cv::String> cv_all_img_names;
  cv::glob(FLAGS_image_dir, cv_all_img_names);
  std::cout << "total images num: " << cv_all_img_names.size() << std::endl;

  if (!Utility::PathExists(FLAGS_output)) {
    Utility::CreateDir(FLAGS_output);
  }
  std::cout << "ocr_1: " << FLAGS_type << std::endl;

  if (FLAGS_type == "ocr") {
    ocr(cv_all_img_names);
  } else if (FLAGS_type == "structure") {
    structure(cv_all_img_names);
  } else {
    std::cout << "only value in ['ocr','structure'] is supported" << std::endl;
  }
}



char* to_utf8(const wchar_t* src) {
    char* buf;
    int dst_size, rc;

    rc = WideCharToMultiByte(CP_ACP, 0, src, -1, NULL, 0, NULL, NULL);//CP_ACP
    if (rc == 0) {
        return nullptr;
    }

    dst_size = rc + 1;
    buf = (char*)malloc(dst_size);
    if (buf == NULL) {
        return nullptr;
    }
    //CP_UTF8
    rc = WideCharToMultiByte(CP_ACP, 0, src, -1, buf, dst_size, NULL, NULL);
    if (rc == 0) {
        free(buf);
        return nullptr;
    }
    buf[rc] = '\0';

    //std::string ans = buf;

//    free(buf);

  //  return ans;
    return buf;
}
/*
std::string ConvertToACPFromString(const std::string& str) {
    // まずUTF-8文字列をUnicode（ワイド文字列）に変換
    int wideSize = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (wideSize <= 0) {
        throw std::runtime_error("Failed to calculate wide character buffer size");
    }

    std::wstring wideString(wideSize, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, wideString.data(), wideSize);

    // ワイド文字列をACP文字列に変換
    int acpSize = WideCharToMultiByte(CP_ACP, 0, wideString.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (acpSize <= 0) {
        throw std::runtime_error("Failed to calculate ACP buffer size");
    }

    std::string acpString(acpSize, '\0');
    WideCharToMultiByte(CP_ACP, 0, wideString.c_str(), -1, acpString.data(), acpSize, nullptr, nullptr);

    // 終端文字を削除して文字列を返す
    return std::string(acpString.c_str());
}*/

wchar_t* ConvertToACPFromString_ptr(const std::string& str) {
    // まずUTF-8文字列をUnicode（ワイド文字列）に変換
    int wideSize = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (wideSize <= 0) {
        throw std::runtime_error("Failed to calculate wide character buffer size");
    }

    std::wstring wideString(wideSize, L'\0');


    auto dst_size = wideSize + 1;
    auto buf = new wchar_t[dst_size];//(wchar_t*)malloc(dst_size);

    //MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, wideString.data(), wideSize);
    auto rc = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, buf, wideSize);

    buf[rc] = '\0';
    return buf;
    /*
    // ワイド文字列をACP文字列に変換
    int acpSize = WideCharToMultiByte(CP_ACP, 0, wideString.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (acpSize <= 0) {
        throw std::runtime_error("Failed to calculate ACP buffer size");
    }


    std::string acpString(acpSize, '\0');
    WideCharToMultiByte(CP_ACP, 0, wideString.c_str(), -1, acpString.data(), acpSize, nullptr, nullptr);

    // 終端文字を削除して文字列を返す
    return std::string(acpString.c_str());*/
}

EXPORT int paddleocr_check() {
    std::cout << "eee" << std::endl;

    wprintf(L"qwe\n");
    return 2;
}

EXPORT void paddleocr_init(int argc , const wchar_t** argv) {
    //google::ParseCommandLineFlags(&argc, &argv, true);
    std::wcout << L"wert" << std::endl;
    char** argv_utf8 = new char*[argc];
    for (int i = 0; i < argc; i++) {
        argv_utf8[i] = to_utf8(argv[i]);
        std::cout << argv_utf8[i] << std::endl;
    }
    //main(argc,argv_utf8);
    google::ParseCommandLineFlags(&argc, &argv_utf8, true);
    check_params(false);
    sta_ocr = new PPOCR();
}
EXPORT int paddleocr_detect(const wchar_t* img_path, std::vector<ocr_result>*& lst) {
    auto img_path_acp = to_utf8(img_path);

    cv::Mat img = cv::imread(img_path_acp, cv::IMREAD_COLOR);
    std::vector<OCRPredictResult> _ocr_result = sta_ocr->ocr(img, FLAGS_det, FLAGS_rec, FLAGS_cls);
 //   std::cout << ConvertToACPFromString(_ocr_result[0].text) << std::endl;


    lst = new std::vector<ocr_result>();
    for (auto& i:_ocr_result) {
        ocr_result r;
        r.x1 = i.box[0][0];
        r.y1 = i.box[0][1];
        r.x2 = i.box[1][0];
        r.y2 = i.box[1][1];
        r.x3 = i.box[2][0];
        r.y3 = i.box[2][1];
        r.x4 = i.box[3][0];
        r.y4 = i.box[3][1];
        r.text = ConvertToACPFromString_ptr(i.text);
        r.score = i.score;
        (lst)->push_back(r);
    }
    Utility::print_result(_ocr_result);

    return (lst)->size();
}

EXPORT ocr_result paddleocr_get_result(std::vector<ocr_result>* lst,int index) {
    return lst->at(index);
}

EXPORT void paddleocr_delete_result_list(std::vector<ocr_result>* lst) {
    for (auto& i : *lst) {
        delete[] i.text;
    }
    delete lst;
}

EXPORT void paddleocr_close() {
    delete sta_ocr;
}

/*

EXPORT void paddleocr_init(int argc, char** argv) {
    //google::ParseCommandLineFlags(&argc, &argv, true);
    //char** argv_utf8 = new char* [argc];
    for (int i = 0; i < argc; i++) {
        //argv_utf8[i] = to_utf8(argv[i]);
        std::cout << argv[i] << std::endl;
    }
    main_2(argc, argv);
    // google::ParseCommandLineFlags(&argc, &argv_utf8, true);
    // check_params(false);
}*/
