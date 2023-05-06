#include "jpeg2umat.hpp"

#include <stdexcept>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/core/ocl.hpp>
#include <opencv2/core/mat.hpp>

#include <CL/cl.h>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/buffer.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/hwcontext_opencl.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}


static std::string averr(int errnum)
{
    static char err_string[128];
    return std::string(av_make_error_string(err_string, 128, errnum));
}

static bool has_hwaccel_support(enum AVHWDeviceType type)
{
    enum AVHWDeviceType curr;

    for (curr = av_hwdevice_iterate_types(AV_HWDEVICE_TYPE_NONE);
         curr != AV_HWDEVICE_TYPE_NONE;
         curr = av_hwdevice_iterate_types(curr)) {
         if (curr == type)
             return true;
    }

    return false;
}

static enum AVPixelFormat get_vaapi_format(
    AVCodecContext *ctx,
    const enum AVPixelFormat *pix_fmts
) {
    const enum AVPixelFormat *p;

    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_VAAPI)
            return *p;
    }

    av_log(NULL, AV_LOG_ERROR, "Unable to find VAAPI pix_fmt\n");
    return AV_PIX_FMT_NONE;
}

jpeg2umat::jpeg2umat(size_t w, size_t h, const char *device)
{
    const AVCodec *decoder;
    cv::ocl::OpenCLExecutionContext cl_exec_ctx;
    std::vector<cl_context_properties> props;
    std::vector<char> platform_name;
    cl_platform_id platform;
    size_t size;
    int ret;

    this->w = w;
    this->h = h;
    filters_initialized = false;

    // check VAAPI and OpenCL support
    if (!has_hwaccel_support(AV_HWDEVICE_TYPE_VAAPI))
        throw std::runtime_error("ffmpeg was built without VAAPI support");

    if (!has_hwaccel_support(AV_HWDEVICE_TYPE_OPENCL))
        throw std::runtime_error("ffmpeg was built without OpenCL support");

    // create a libavcodec VAAPI context
    ret = av_hwdevice_ctx_create(&vaapi_device_ctx,
                                 AV_HWDEVICE_TYPE_VAAPI,
                                 device, NULL, 0);

    if (ret < 0)
        throw std::runtime_error("Failed to create a VAAPI device context: " +
                                 averr(ret));

    // create a libavcodec OpenCL context from VAAPI context
    ret = av_hwdevice_ctx_create_derived(&opencl_device_ctx,
                                         AV_HWDEVICE_TYPE_OPENCL,
                                         vaapi_device_ctx, 0);

    if (ret < 0)
        throw std::runtime_error("Failed to create a OpenCL device context: " +
                                 averr(ret));

    // get AVOpenCLDeviceContext
    AVHWDeviceContext *hwdev = (AVHWDeviceContext *)opencl_device_ctx->data;
    AVOpenCLDeviceContext *hwctx = (AVOpenCLDeviceContext *)hwdev->hwctx;

    // find OpenCL platform id
    size = 0;
    ret = clGetContextInfo(hwctx->context, CL_CONTEXT_PROPERTIES,
                           0, NULL, &size);

    if (ret != CL_SUCCESS || size == 0)
        throw std::runtime_error("clGetContextInfo() failed to get props "
                                 "size");

    props.resize(size);
    ret = clGetContextInfo(hwctx->context, CL_CONTEXT_PROPERTIES,
                           size, props.data(), NULL);

    if (ret != CL_SUCCESS)
        throw std::runtime_error("clGetContextInfo() failed");

    bool found = false;

    for (int i = 0; props[i] != 0; i = i + 2) {
        if (props[i] == CL_CONTEXT_PLATFORM) {
            platform = (cl_platform_id) props[i + 1];
            found = true;
        }
    }

    if (!found)
        throw std::runtime_error("clGetContextInfo() platform not found");

    // find OpenCL platform name
    ret = clGetPlatformInfo(platform, CL_PLATFORM_NAME, 0, NULL, &size);

    if (ret != CL_SUCCESS || size == 0)
        throw std::runtime_error("clGetContextInfo() failed");

    platform_name.resize(size);

    ret = clGetPlatformInfo(platform, CL_PLATFORM_NAME,
                            size, platform_name.data(), NULL);

    if (ret != CL_SUCCESS)
        throw std::runtime_error("clGetContextInfo() failed");

    // init OpenCV context
    cl_exec_ctx = cv::ocl::OpenCLExecutionContext::create(
        platform_name.data(),
        platform,
        hwctx->context,
        hwctx->device_id
    );
    cl_exec_ctx.bind();

    // find MJPEG decoder
    decoder = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    if (!decoder)
        throw std::runtime_error("Cannot find MJPEG decoder");

    // create decoder context
    decoder_ctx = avcodec_alloc_context3(decoder);
    if (!decoder_ctx)
        throw std::runtime_error("Cannot allocate decoder context");

    decoder_ctx->hw_device_ctx = av_buffer_ref(vaapi_device_ctx);
    decoder_ctx->get_format = get_vaapi_format;

    // open decoder
    if (avcodec_open2(decoder_ctx, decoder, NULL) < 0)
        throw std::runtime_error("Failed to open MJPEG decodern");
}

bool jpeg2umat::init_filters(const char *filters_descr)
{
    char args[256];
    const AVFilter *buffersrc = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();
    AVFilterInOut *inputs = avfilter_inout_alloc();
    AVFilterInOut *outputs = avfilter_inout_alloc();
    int ret = 0;

    filter_graph = avfilter_graph_alloc();
    if (filter_graph == NULL) {
        av_log(NULL, AV_LOG_ERROR, "Cannot allocate filter graph\n");
        goto end;
    }

    // buffer video source
    // the decoded frames from the decoder will be inserted here
    snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=1/1",
             decoder_ctx->width, decoder_ctx->height,
             decoder_ctx->pix_fmt);

    ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                                       args, NULL, filter_graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source\n");
        goto end;
    }

    memset(par, 0, sizeof(*par));
    par->format = AV_PIX_FMT_NONE;
    par->hw_frames_ctx = decoder_ctx->hw_frames_ctx;

    ret = av_buffersrc_parameters_set(buffersrc_ctx, par);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot set buffer source parameters\n");
        goto end;
    }

    av_freep(&par);

    // buffer video sink
    // to terminate the filter chain
    ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                                       NULL, NULL, filter_graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create buffer sink\n");
        goto end;
    }

    // inputs and outputs
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    inputs->name        = av_strdup("out");
    inputs->filter_ctx  = buffersink_ctx;
    inputs->pad_idx     = 0;
    inputs->next        = NULL;

    ret = avfilter_graph_parse_ptr(filter_graph, filters_descr,
                                   &inputs, &outputs, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot parse filters\n");
        goto end;
    }

    ret = avfilter_graph_config(filter_graph, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot configure filters\n");
        goto end;
    }

    // create opencl_hw_frames_ctx
    ret = av_hwframe_ctx_create_derived(
        &opencl_hw_frames_ctx,
        AV_PIX_FMT_OPENCL,
        opencl_device_ctx,
        decoder_ctx->hw_frames_ctx,
        AV_HWFRAME_MAP_DIRECT
    );
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create derived hwframe context\n");
        goto end;
    }

end:
    av_freep(&par);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return ret == 0;
}

void jpeg2umat::decode2gray(void *jpeg, size_t size, cv::UMat &dst)
{
    AVPacket *packet = av_packet_alloc();
    AVFrame *cl_frame, *frame;
    int ret;

    if (packet == NULL)
        throw std::runtime_error("Failed to allocate packet");

    packet->data = (unsigned char *)jpeg;
    packet->size = size;

    // decode jpeg
    ret = avcodec_send_packet(decoder_ctx, packet);
    if (ret < 0) {
        av_packet_free(&packet);
        throw std::runtime_error("Failed to send frame: " + averr(ret));
    }

    frame = av_frame_alloc();

    if (frame == NULL) {
        av_packet_free(&packet);
        throw std::runtime_error("Failed to allocate frame");
    }

    ret = avcodec_receive_frame(decoder_ctx, frame);
    if (ret < 0) {
        av_frame_free(&frame);
        av_packet_free(&packet);
        throw std::runtime_error("Failed to receive frame: " + averr(ret));
    }

    av_packet_free(&packet);

    // initialize filters
    if (!filters_initialized) {
        char filters_description[64];

        if (w > 0 && h > 0) {
            snprintf(filters_description, sizeof(filters_description),
                     "scale_vaapi=format=nv12:w=%lu:h=%lu:mode=fast", w, h);
        } else {
            snprintf(filters_description, sizeof(filters_description),
                     "scale_vaapi=format=nv12");
        }

        if (!init_filters(filters_description)) {
            av_frame_free(&frame);
            throw std::runtime_error("Failed to init filters");
        }

        filters_initialized = true;
    }

    // push frame to filter chain
    ret = av_buffersrc_add_frame(buffersrc_ctx, frame);
    if (ret < 0) {
        av_frame_free(&frame);
        throw std::runtime_error("Failed to add frame to filter chain");
    }

    // pull frame from filter chain
    ret = av_buffersink_get_frame(buffersink_ctx, frame);
    if (ret < 0) {
        av_frame_free(&frame);
        throw std::runtime_error("Failed to get frame from filter chain");
    }

    // map VAAPI frame to OpenCL frame
    cl_frame = av_frame_alloc();
    if (cl_frame == NULL) {
        av_frame_free(&frame);
        throw std::runtime_error("Failed to allocate opencl frame");
    }

    cl_frame->hw_frames_ctx = av_buffer_ref(opencl_hw_frames_ctx);
    cl_frame->format = AV_PIX_FMT_OPENCL;

    ret = av_hwframe_map(cl_frame, frame, AV_HWFRAME_MAP_READ);
    if (ret < 0) {
        av_frame_free(&cl_frame);
        av_frame_free(&frame);
        throw std::runtime_error("Failed to map frame from vaapi to opencl");
    }

    // convert OpenCL frame (Image2D) to UMat object (Buffer)
    // (only Y plane -> grayscale image)
    cv::ocl::convertFromImage(cl_frame->data[0], dst);

    // cleanup
    av_frame_free(&cl_frame);
    av_frame_free(&frame);
}
