#ifndef _JPEG2UMAT_HPP_
#define _JPEG2UMAT_HPP_

#include <cstddef>

#include <CL/cl.h>
#include <opencv2/core/mat.hpp>

extern "C" {
#include <libavutil/buffer.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
}

class jpeg2umat {
    size_t w;
    size_t h;

    AVBufferRef *vaapi_device_ctx;
    AVBufferRef *opencl_device_ctx;
    AVBufferRef *opencl_hw_frames_ctx;

    AVCodecContext *decoder_ctx;

    AVFilterGraph *filter_graph;
    AVFilterContext *buffersink_ctx;
    AVFilterContext *buffersrc_ctx;

  private:
    bool filters_initialized;
    bool init_filters(const char *filterd_descr);

  public:
    jpeg2umat(size_t w = 0, size_t h = 0, const char *device = NULL);
    void decode2gray(void *jpeg, size_t size, cv::UMat &dst);
};

#endif
