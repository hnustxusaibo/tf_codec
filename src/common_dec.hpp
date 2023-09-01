#include "common.hpp"

using namespace yitu_codec_common;

namespace yitu_codec_dec {

// 视频文件信息
struct VideoInfo {
    AVFormatContext *avFormatContext;
    int videoIndex;
    int width;
    int height;
    TFDEC_DECODER_ROLE role;
    bool gNeedFilter;
    bool gNeedFilterH265;
};
// 解码结果统计
int gLoadedFrameCount = 0;
// 入解码器统计
int gTfEnqueuedFrameCount = 0;
// 解码完成flag
bool gLoadCompleted = false;
bool gTfEnqueueCompleted = false;
bool gCallbackCompleted = false;
bool gDecodeCompleted = false;

int gDecodedFrameCount = 0;
int gDecodedBytes = 0;

// 输入输出帧数据列表
std::queue<FrameData *> inFrameQueue;
std::queue<FrameData *> outFrameQueue;
/// 队列锁
std::mutex inFrameQueueLock;
std::mutex outFrameQueueLock;

// 输入输出信号量控制
Semaphore gInFrameCache_sem;
Semaphore gOutFrameCache_sem;
// PV用于控制硬解码单元buffer中的帧数
Semaphore gCacheHardware_sem;
// 控制器session
TFDEC_HANDLE gSessionHandle;

bool gDebugEnabled;

// 线程声明
void load_frames(VideoInfo *videoInfo);
void enqueue_frames();
void save_file();

/// @brief 启动解码器 并启动解码 输入输出进程
/// @param session 解码器控制器session
/// @param inFrameCacheSize 内存帧缓存数量
/// @param outFrameCacheSize 内存帧缓存数量
/// @param frameHardwareCacheSize  tf解码器缓存数量
/// @return
int run_dec(VideoInfo *videoInfo, TFDEC_HANDLE session, int inFrameCacheSize, int outFrameCacheSize, int frameHardwareCacheSize) {
    gSessionHandle = session;
    // 缓存控制 信号量初始化
    while (inFrameCacheSize--) {
        gInFrameCache_sem.notify();
    }
    while (outFrameCacheSize--) {
        gOutFrameCache_sem.notify();
    }
    while (frameHardwareCacheSize--) {
        gCacheHardware_sem.notify();
    }
    // 数量列表清理
    while (!inFrameQueue.empty()) inFrameQueue.pop();
    while (!outFrameQueue.empty()) outFrameQueue.pop();

    std::thread loadFramesThread;
    loadFramesThread = std::thread(&load_frames, videoInfo);

    std::thread enqueueFramesThread;
    enqueueFramesThread = std::thread(&enqueue_frames);

    std::thread saveFileThread;
    saveFileThread = std::thread(&save_file);

    loadFramesThread.join();
    enqueueFramesThread.join();
    saveFileThread.join();

    // 压缩帧都已经加载完，且都已经
    // 等待所有解码的回调完
    int i = 0;
    while (!gDecodeCompleted) {
        if (((i++) % 100) == 0) {
            printf("Waiting for decoding callback: Loaded: %d, Enqueued: %d, Decoded: %d.\n", gLoadedFrameCount,
                   gTfEnqueuedFrameCount, gDecodedFrameCount);
        }
        usleep(10000);
    }
    printf("Decode complete: Loaded: %d, Enqueued: %d, Decoded: %d.\n", gLoadedFrameCount, gTfEnqueuedFrameCount,
           gDecodedFrameCount);

    return 0;
}

void load_frames(VideoInfo *videoInfo) {
    printf("Load frames thread start.\n");

    // 准备过滤器
    AVBSFContext *bsf_ctx = nullptr;
    if (videoInfo->gNeedFilter || videoInfo->gNeedFilterH265) {
        std::string filter_name;
        if (videoInfo->gNeedFilter) filter_name = "h264_mp4toannexb";
        if (videoInfo->gNeedFilterH265) filter_name = "hevc_mp4toannexb";
        const AVBitStreamFilter *filter = av_bsf_get_by_name(filter_name.c_str());
        int ret = av_bsf_alloc(filter, &bsf_ctx);
        if (ret < 0) {
            // 分配比特流过滤器上下文失败
            printf("ERROR: Failed to allocate bitstream filter context.\n");
            exit(-1);
        }
    }
    // 准备读取数据
    AVPacket *pAvPacket;
    pAvPacket = av_packet_alloc();

    int totalFrameCount = 0, totalVideoFrameCount = 0;
    while (true) {
        if (av_read_frame(videoInfo->avFormatContext, pAvPacket) >= 0) {
            if (pAvPacket->stream_index == videoInfo->videoIndex) {
                totalVideoFrameCount++;
                // 执行packet过滤
                if (videoInfo->gNeedFilter || videoInfo->gNeedFilterH265) {
                    if (av_bsf_send_packet(bsf_ctx, pAvPacket) != 0) {
                        av_packet_unref(pAvPacket);
                        continue;
                    }
                    while (av_bsf_receive_packet(bsf_ctx, pAvPacket) != 0) {
                        continue;
                    }
                }

                FrameData *frameData = new FrameData(pAvPacket->data, pAvPacket->size, pAvPacket->pts, false);
                gLoadedFrameCount++;

                gInFrameCache_sem.wait();
                inFrameQueueLock.lock();
                inFrameQueue.push(frameData);
                inFrameQueueLock.unlock();

                if (gDebugEnabled) {
                    printf("Frame loaded. %d. Timestamp: %ld\n", gLoadedFrameCount, frameData->GetTimestamp());
                }
            }
            totalFrameCount++;

        } else {
            // 无数据或者错误
            break;
        }
        av_packet_unref(pAvPacket);
    }
    // 插入结束帧，此帧不计入视频帧统计
    FrameData *frameData = new FrameData();
    frameData->SetIsEnd(true);
    inFrameQueueLock.lock();
    inFrameQueue.push(frameData);
    inFrameQueueLock.unlock();

    if (gDebugEnabled) {
        printf("Frame loaded. %d\n", gLoadedFrameCount);
    }

    // 清理资源
    av_packet_free(&pAvPacket);
    av_bsf_free(&bsf_ctx);
    gLoadCompleted = true;
    printf("Load frames thread complete.\n");
}

/**
 * 从inFrameQueue读取,向TF硬件插入帧
 */
void enqueue_frames() {
    printf("Enqueue frames thread start.\n");
    FrameData *frameData = NULL;
    while (true) {
        // 等待tfdec内存空闲
        gCacheHardware_sem.wait();
        inFrameQueueLock.lock();
        if (inFrameQueue.empty()) {
            inFrameQueueLock.unlock();
            // 压缩帧加载慢，需要等待
            usleep(1000);
            continue;
        }
        FrameData *frameData = inFrameQueue.front();
        inFrameQueue.pop();
        inFrameQueueLock.unlock();
        // 释放队列
        gInFrameCache_sem.notify();

        // 加入TF设备的buffer
        void *buffer = NULL;
        int size = 0;
        unsigned int flag = TFDEC_BUFFER_FLAG_EOS;
        unsigned long timestamp = frameData->GetTimestamp();
        if (!frameData->GetIsEnd()) {
            buffer = frameData->GetData();
            size = frameData->GetLength();
            flag = TFDEC_BUFFER_FLAG_ENDOFFRAME;
        }

        int enqueueFailedCount = 0;
        int ret = 0;
        while (true) {
            ret = tfdec_enqueue_buffer(gSessionHandle, buffer, size, timestamp, flag);
            if (ret == TFDEC_STATUS_SUCCESS) {
                break;
            }
            if (gDebugEnabled) {
                printf("Frame enqueue failed. ret: %d.\n", ret);
            }
            enqueueFailedCount++;
            usleep(10);
            if (enqueueFailedCount >= (10000000 / 10)) {
                printf("ERROR: tfdec_enqueue_buffer has kept failing for %d times.\n", enqueueFailedCount);
                exit(-1);
            }
        }
        if (flag != TFDEC_BUFFER_FLAG_EOS) {
            gTfEnqueuedFrameCount++;
            if (gDebugEnabled) {
                printf("Frame enqueued. Count: %d, Timestamp: %ld, ret: %d.\n", gTfEnqueuedFrameCount, frameData->GetTimestamp(), ret);
            }
        }

        // 最后一帧
        if (frameData->GetIsEnd()) {
            break;
        }
        delete frameData;
    }
    gTfEnqueueCompleted = true;
    printf("Enqueue frames thread complete.\n");
}

/**
 * tf视频解码后的回调函数（按enqueue的顺序回调）
 * @param session       - session handle
 * @param buffer          - 输出数据地址，位于TF dec output buffer
 * @param size          - 输出数据大小
 * @param timestamp     - 加入output buffer的时间戳
 * @param flag          - 帧的结束标记
 *                          TFDEC_BUFFER_FLAG_ENDOFFRAME    一帧的结束
 *                          TFDEC_BUFFER_FLAG_EOS           整个流的结束
 * @param pdata         - 创建session时的user_data地址
 */
void callback(TFDEC_HANDLE session, void *buffer, int size, unsigned long timestamp, unsigned int flag, void *pUserdata) {
    // 解码输出存入内存
    FrameData *frameData = new FrameData((unsigned char *)buffer, size, 0L, false);
    // 归还output buffer 解码器中数据-1
    tfdec_return_output(session, buffer);
    gCacheHardware_sem.notify();

    if (flag == TFDEC_BUFFER_FLAG_EOS) {
        frameData->SetIsEnd(true);
    } else {
        gDecodedFrameCount++;
        gDecodedBytes += size;
        if (gDebugEnabled) {
            printf("Frame decoded. count: %d, Timestamp: %ld\n", gDecodedFrameCount, timestamp);
        }
    }
    // 入输出队列
    gOutFrameCache_sem.wait();
    outFrameQueueLock.lock();
    outFrameQueue.push(frameData);
    outFrameQueueLock.unlock();
    if (frameData->GetIsEnd()) {
        gCallbackCompleted = true;
    }
}

/// @brief 创建解码器session
/// @param deviceIndex  解码器设备id
/// @param role 解码视频类型
/// @param width 视频宽
/// @param height 视频高
/// @param userData 用户信息
TFDEC_HANDLE create_session(int deviceIndex, TFDEC_DECODER_ROLE role, int width, int height, void *userData) {
    // id -> useDev
    std::string useDev = "/dev/mv500";
    if (deviceIndex > 1) {
        useDev = useDev + "-" + std::to_string(deviceIndex);
    }
    TFDEC_HANDLE session = tfdec_create(useDev.c_str(), role, width, height, 4, callback, userData);
    printf("Create session done. Session handle: %p\n", session);

    if (session == NULL) {
        printf("ERROR: Session create failed.\n");
        exit(-1);
    }
    return session;
}

void destroy_session(TFDEC_HANDLE session) {
    printf("Destroy TF session.\n");
    tfdec_destroy(session);
    printf("Destroy TF session done.\n");
}

// 读取视频文件信息
int read_video_file(std::string fileName, VideoInfo *videoInfo) {
    const char *filePath = fileName.c_str();
    AVCodec *codec = NULL;

    TFDEC_DECODER_ROLE role = DECODER_H264;

    if (avformat_open_input(&videoInfo->avFormatContext, filePath, NULL, NULL) < 0) {
        printf("can't open file %s\n", filePath);
        exit(1);
    }

    if (avformat_find_stream_info(videoInfo->avFormatContext, NULL) < 0) {
        printf("can't recognise stream type.\n");
    }

    videoInfo->videoIndex = av_find_best_stream(videoInfo->avFormatContext, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (videoInfo->videoIndex < 0) {
        printf("no video stream in this file\n");
        exit(1);
    }
    printf("video index: %d\n", videoInfo->videoIndex);

    auto codecpar = videoInfo->avFormatContext->streams[videoInfo->videoIndex]->codecpar;
    videoInfo->width = codecpar->width;
    videoInfo->height = codecpar->height;
    videoInfo->role = DECODER_H264;
    switch (codecpar->codec_id) {
        case AV_CODEC_ID_MPEG4:
            videoInfo->role = DECODER_MPG4;
            printf("---mpg4---\n");
            if (codecpar->codec_tag == MKTAG('m', 'p', '4', 'v')) {
                uint32_t head_size = codecpar->extradata_size;
                uint8_t *stream_head = (uint8_t *)malloc(head_size);
                memcpy(stream_head, codecpar->extradata, head_size);
            }
            break;

        case AV_CODEC_ID_H264:
            printf("---h264---\n");
            videoInfo->role = DECODER_H264;
            if (codecpar->codec_tag == MKTAG('a', 'v', 'c', '1') || codecpar->codec_tag == 0) {
                printf("---H264 : need_filter---\n");
                videoInfo->gNeedFilter = true;
            }
            break;

        case AV_CODEC_ID_HEVC:
            printf("---hevc---\n");
            videoInfo->role = DECODER_HEVC;
            if (codecpar->codec_tag == MKTAG('h', 'e', 'v', '1' || codecpar->codec_tag == 0)) {
                printf("---H265 : need_filter---\n");
                videoInfo->gNeedFilterH265 = true;
            }
            break;

        case AV_CODEC_ID_VP8:
            printf("---vp8---");
            videoInfo->role = DECODER_VP8;
            break;

        case AV_CODEC_ID_MPEG2VIDEO:
            printf("---mpeg2---");
            videoInfo->role = DECODER_MPG2;
            break;

        default:
            break;
    }

    return 0;
}

void save_file() {
    std::string filename = "/root/saibo/tf_codec/out/003_out.yuv";
    std::fstream gOutputFStream;
    gOutputFStream.open(filename, std::ios::out | std::ios::binary);
    if (!gOutputFStream.is_open() || !gOutputFStream.good()) {
        printf("ERROR: Unable to open file %s.\n", filename.c_str());
    }

    int size = 0;
    while (true) {
        // 等待tfdec内存空闲
        outFrameQueueLock.lock();
        if (outFrameQueue.empty()) {
            outFrameQueueLock.unlock();
            // 压缩帧加载慢，需要等待
            usleep(1000);
            continue;
        }
        FrameData *frameData = outFrameQueue.front();
        outFrameQueue.pop();
        outFrameQueueLock.unlock();
        // 释放队列
        gOutFrameCache_sem.notify();

        if (frameData->GetIsEnd()) {
            gDecodeCompleted = true;
            printf("save done!\n");
            break;
        }
        size++;
        printf("save file frame size: %d\n", size);

        // auto ret = tfg::I420_Planar_ScaleEx((uint8_t *)frameData->GetData(), nullptr, 1920, 1080,
        //                          (uint8_t *)frameData->GetData(), nullptr, 1280, 720, tfg::INTERP_Bilinear);
        gOutputFStream.write((const char *)frameData->GetData(), frameData->GetLength());
    }

    if (gOutputFStream.is_open()) {
        gOutputFStream.close();
    }
}
}  // namespace yitu_codec_dec
