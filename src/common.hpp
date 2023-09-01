#ifndef COMMON_HPP
#define COMMON_HPP

//<editor-fold desc="Head">
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <mutex>
#include <queue>
#include <sstream>
#include <thread>
#include <unordered_set>

#include "libtfdec.h"
#include "tfenc_api.h"
#include "tfgh.h"

#ifdef __cplusplus
extern "C" {
#endif
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#ifdef __cplusplus
}
#endif
//</editor-fold>

namespace yitu_codec_common {
// 信号量结构体
class Semaphore {
   public:
    Semaphore(int count_ = 0)
        : count(count_) {
    }

    inline void notify() {
        std::unique_lock<std::mutex> lock(mtx);
        count++;
        // notify the waiting thread
        cv.notify_one();
    }

    inline void wait() {
        std::unique_lock<std::mutex> lock(mtx);
        while (count == 0) {
            // wait on the mutex until notify is called
            cv.wait(lock);
        }
        count--;
    }

    inline int getCount() {
        return count;
    }

   private:
    std::mutex mtx;
    std::condition_variable cv;
    int count;
};

// 帧数据结构
class FrameData {
   public:
    FrameData() {
        data = nullptr;
        length = 0L;
        timestamp = 0L;
        isEnd = true;
        handled = 0;
    }

    FrameData(unsigned char *data, unsigned long length, unsigned long timestamp, bool isEnd) {
        this->data = new unsigned char[length];
        memcpy(this->data, data, length);
        this->length = length;
        this->timestamp = timestamp;
        this->isEnd = isEnd;
        this->handled = 0;
    }

    ~FrameData() {
        if (data != NULL) {
            delete[] data;
            data = NULL;
        }
    }

    unsigned char *GetData() {
        return data;
    }

    unsigned long GetLength() {
        return length;
    }

    unsigned long GetTimestamp() {
        return timestamp;
    }

    bool GetIsEnd() {
        return isEnd;
    }

    /// 注意此处无内存拷贝，直接引用外面给的内存区域
    void SetData(unsigned char *data) {
        this->data = data;
    }

    void SetLength(unsigned long length) {
        this->length = length;
    }

    void SetTimestamp(unsigned long timestamp) {
        this->timestamp = timestamp;
    }

    void SetIsEnd(bool isEnd) {
        this->isEnd = isEnd;
    }

    void IncHandled() {
        handled++;
    }

    int GetHandled() {
        return handled;
    }

   private:
    unsigned char *data;
    unsigned long length;
    /// 本程序中，timestamp 被设置为视频帧号，用于在callback中找到相关编号
    unsigned long timestamp;
    bool isEnd;
    /// 处理的次数，用于统计被多次使用的次数
    int handled;
};

/// YUV数据格式
/// TF ENC只支持NV12，其它格式需要转换
enum YuvFormat {
    YUV_FORMAT_I420 = 0,
    YUV_FORMAT_NV12,
    YUV_FORMAT_INVALID
};

// 参数解析函数  --key=val 解析为 std::map<key,val>
int parse_param_map(int argc, char *argv[], std::map<std::string, std::string> &arg) {
    for (int i = 1; i < argc; ++i) {
        std::string param(argv[i]);
        size_t pos = param.find('=');
        if (pos != std::string::npos && param.substr(0, 2) == "--") {
            std::string key = param.substr(2, pos - 2);
            std::string value = param.substr(pos + 1);
            arg[key] = value;
        } else {
            std::cout << "Invalid parameter format: " << param << std::endl;
            return 1;
        }
    }
    return 0;
}

int string_to_bool(const std::string &str) {
    return str == "1";
}

int string_to_int(const std::string &str) {
    return std::stoi(str);
}

YuvFormat string_to_yuvformat(const std::string &str) {
    if ((str == "I420") || (str == "i420")) {
        return YUV_FORMAT_I420;
    }
    if ((str == "NV12") || (str == "nv12")) {
        return YUV_FORMAT_NV12;
    }
    printf("ERROR: Unknown YUV format. %s.", str.c_str());
    exit(-1);
}

}  // namespace yitu_codec_common
#endif  // COMMON_HPP
