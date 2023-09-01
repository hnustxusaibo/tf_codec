#include "common.hpp"
#include "common_dec.hpp"

using namespace yitu_codec_common;

// 参数默认值
// 全局参数 输入文件名 输出文件名 是否开启debug日志
std::string gInputFileName;
std::string gOutputFileName;
bool gDebugEnabled = false;

// 输入视频分辨率 输出视频分辨率
uint32_t gDecWidth = 0;
uint32_t gDecHeight = 0;
uint32_t gEncWidth = 0;
uint32_t gEncHeight = 0;

// 解码器参数
// 解码器id
int gDecDeviceIndex = -1;

// 转码参数
// 压缩格式
tfg::INTERP_MODE gRecInterpMod = tfg::INTERP_Bilinear;

// 编码器参数
// 编码格式 帧组大小 编码等级 帧率 码率模式 目标码率 最大码率
tf_profile gEncTfProfile = TF_PROFILE_INVALID;
uint32_t gEncGop = 25;
uint32_t gEncLevel = 41;
uint32_t gEncFrameRate = 30;
tf_rcmode gEncTfRcMode = RC_CBR;
uint32_t gEncBitRate = 8000000;
uint32_t gEncMaxBitRate = 8000000;

int parse_param(int argc, char* argv[]) {
    std::map<std::string, std::string> arg_map;
    if (parse_param_map(argc, argv, arg_map)) {
        return 1;
    }

    for (const auto& pair : arg_map) {
        std::string key = pair.first;
        std::string val = pair.first;
        std::cout << "Key: " << key << ", Value: " << val << std::endl;
        if (key == "input_filename") {
            gInputFileName = val;
        } else if (key == "output_filename") {
            gOutputFileName = val;
        } else if (key == "debug_flag") {
            gDebugEnabled = string_to_bool(val);
        } else if (key == "dec_device_id") {
            gDecDeviceIndex = string_to_int(val);
        } else if (key == "enc_width") {
            gEncWidth = string_to_int(val);
        } else if (key == "enc_height") {
            gEncHeight = string_to_int(val);
        } else if (key == "enc_profile") {
            gEncTfProfile = tf_profile(string_to_int(val));
        } else if (key == "enc_gop") {
            gEncGop = string_to_int(val);
        } else if (key == "enc_level") {
            gEncLevel = string_to_int(val);
        } else if (key == "enc_rate") {
            gEncFrameRate = string_to_int(val);
        } else if (key == "enc_rcmode") {
            gEncTfRcMode = tf_rcmode(string_to_int(val));
        } else if (key == "enc_bit_rate") {
            gEncBitRate = string_to_int(val);
        } else if (key == "enc_max_bit_rate") {
            gEncMaxBitRate = string_to_int(val);
        }
    }

    return 0;
}

void print_help() {
    printf("Usage:\n");
    printf("multi_rnc --[option]=[value]\n");
    printf("    options:\n");
    printf("      * --input_filename=[filename]         filename待编码的视频文件\n");
    printf("        --output_filename=[filename]        输出文件名,包括路径。编码结果视频文件\n");
    printf("        --debug_flag=[flag]                 是否输出详细的debug信息。默认0。\n");
    printf("        --dec_device_id=[device_id]         指定使用的解码器,解码器id\n");
    printf("        --enc_width=[count]                 输出视频宽度像素值\n");
    printf("        --enc_height=[count]                输出视频高度像素值\n");
    printf("        --enc_profile=[profile_name]        指定压缩编码格式。0:AVC_BASELINE,1:AVC_MAIN,2:AVC_HIGH,3:HEVC_MAIN,4:HEVC_MAIN10。\n");
    printf("        --enc_gop=[count]                   Group of pictures\n");
    printf("        --enc_level=[count]                 level of TF enc\n");
    printf("        --enc_rate=[count]                  帧率\n");
    printf("        --enc_rcmode=[mode_name]            指定编码码率模式。0:CBR,1:VBR。\n");
    printf("        --enc_bit_rate=[count]              bit rate. default: 8000000\n");
    printf("        --enc_max_bit_rate=[count]          max bit rate. default: 8000000\n");
    printf("\n");
    printf("Example:\n");
    printf("./multi_rnc --input_filename=./yuv/1.yuv --output_filename=output/1.h264 --enc_profile=2 --in_width=1920 --in_height=1080\n");
    printf("./multi_rnc --input_filename=./yuv/2.yuv --output_filename=output/2.h264 --enc_profile=2 --in_width=1920 --in_height=1080\n");
    printf("\n");
}

int main(int argc, char* argv[]) {
    gInputFileName = "/root/saibo/tf_codec/video/002.mp4";
    gOutputFileName = "/root/saibo/tf_codec/video/003.mp4";
    gDecDeviceIndex = 1;
    if (parse_param(argc, argv) || gInputFileName.empty() || gOutputFileName.empty()) {
        print_help();
        exit(1);
    }

    // 视频 -> 缓存 -> 解码器 -> 缓存 -> resize -> 缓存 -> 编码器 -> 缓存 ->  文件
    // 读取视频信息
    yitu_codec_dec::gDebugEnabled = true;
    yitu_codec_dec::VideoInfo* videoInfo = new yitu_codec_dec::VideoInfo();
    yitu_codec_dec::read_video_file(gInputFileName, videoInfo);
    int* userData = new int(1);
    // 创建解码器sesion
    auto dec_session = yitu_codec_dec::create_session(gDecDeviceIndex, videoInfo->role, videoInfo->width, videoInfo->height, userData);
    // 启动解码器
    yitu_codec_dec::run_dec(videoInfo, dec_session, 512, 512, 32);

    while (yitu_codec_dec::gDecodeCompleted == false) {
        sleep(1);
    }
    return 0;
}
