#include <iostream>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/core/mat.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/calib3d.hpp>

#include <DynamsoftBarcodeReader.h>
#include <DynamsoftCommon.h>

#include "jpeg2umat.hpp"

class QRScan {
    private:
        // libav VAAPI jpeg decoder -> grayscale cv::UMat
        jpeg2umat *j2u;

        // preallocate image matrices
        cv::UMat gray;
        cv::UMat undistorted;
        cv::UMat detect;
        cv::UMat detect32f;

        // undistort matrices
        cv::Mat K;
        cv::Mat D;
        cv::UMat map1;
        cv::UMat map2;
        cv::Size scan_size;
        bool maps_initialized;

        // detector dnn
        cv::dnn::Net detector;
        cv::Size detect_size;
        int blob_size[4];

        // dynamsoft barcode reader
        // apply license before creating QRScan object
        dynamsoft::dbr::CBarcodeReader *dbr;

        // last scan results
        std::vector<std::string> results;
        unsigned int curr;

    public:
        // constructor
        QRScan(const char *undistort_path,             // undistort.yml
               const char *detector_prototxt_path,     // detect.prototxt
               const char *detector_caffe_model_path,  // detect.caffemodel
               unsigned short scan_width = 0,          // undistorted img width
               unsigned short scan_height = 0,         //            and height
               const char *hwdevice = NULL);           // /dev/dri/renderD128

        // destructor
        //   - do not run!
        ~QRScan();

        // GPU part:
        //   - decode jpeg using VAAPI / convert to NV12
        //   - copy luminance plane to OpenCV's UMat
        //   - undistort / scale grayscale image
        // CPU part:
        //   - run WeChatCV's DNN to find QR Code objects
        //   - run Dynamsoft Barcode Reader to decode found objects
        //   - store results
        int process_jpeg(void *data, size_t size);

        // get result from the last process_jpeg()
        //   - run multiple times until NULL
        const char *get_result();
};


QRScan::QRScan(const char *undistort_path,
               const char *detector_prototxt_path,
               const char *detector_caffe_model_path,
               unsigned short scan_width,
               unsigned short scan_height,
               const char *hwdevice)
{
    // init VAAPI jpeg decoder (and OpenCL context for OpenCV)
    j2u = new jpeg2umat(0, 0, hwdevice);

    // read undistort matrices from yml/xml
    cv::FileStorage undistort(undistort_path, cv::FileStorage::READ);
    undistort["K"] >> K;
    undistort["D"] >> D;
    undistort.release();

    if (K.empty())
        throw std::runtime_error("K is empty");

    if (D.empty())
        throw std::runtime_error("D is empty");

    maps_initialized = false;

    // init detector dnn
    detector = cv::dnn::readNetFromCaffe(
        detector_prototxt_path,
        detector_caffe_model_path
    );

    // https://github.com/opencv/opencv/issues/22235
    detector.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
    detector.setPreferableTarget(cv::dnn::DNN_TARGET_OPENCL);

    // save indistorted image size
    scan_size = cv::Size(scan_width, scan_height);

    // init DynamsoftBarcodeReader
    dbr = new dynamsoft::dbr::CBarcodeReader;

    // scan only for QR Code
    PublicRuntimeSettings settings;
    dbr->GetRuntimeSettings(&settings);
    settings.barcodeFormatIds = BF_QR_CODE;
    settings.barcodeFormatIds_2 = BF_NULL;
    settings.minResultConfidence = 30;
    dbr->UpdateRuntimeSettings(&settings);
}


int QRScan::process_jpeg(void *data, size_t size)
{
    // empty results vector
    results.clear();
    curr = 0;

    // decode jpeg image
    j2u->decode2gray(data, size, gray);

    // calculate map1 & map2, detect_size and prepare blob size
    if (!maps_initialized) {
        cv::Size gray_size = gray.size();
        if (scan_size.width == 0 || scan_size.height == 0)
            scan_size = gray_size;

        cv::initUndistortRectifyMap(
            K, D, cv::Mat(),
            cv::getOptimalNewCameraMatrix(K, D, gray_size, 0,
                                          scan_size, 0, true),
            scan_size, CV_16SC2, map1, map2
        );

        // preallocate undistort matrix
        undistorted.create(scan_size.height, scan_size.width, CV_8UC1);

        float ratio = sqrt(1.0 * scan_size.width * scan_size.height /
                           (400 * 400));
        detect_size.width = static_cast<int>(scan_size.width / ratio);
        detect_size.height = static_cast<int>(scan_size.height / ratio);

        // preallocate detect matrices
        detect.create(detect_size.height, detect_size.width, CV_8UC1);
        detect32f.create(detect_size.height, detect_size.width, CV_32FC1);

        // calculate blob size
        blob_size[0] = 1;  // one image
        blob_size[1] = 1;  // one channel
        blob_size[2] = detect_size.height;
        blob_size[3] = detect_size.width;

        // done
        maps_initialized = true;
    }

    // remap image
    cv::remap(gray, undistorted, map1, map2, cv::INTER_AREA);

    // prepare undistorted image for detector dnn
    cv::resize(undistorted, detect, detect_size, 0, 0, cv::INTER_AREA);
    detect.convertTo(detect32f, CV_32F, 1.0 / 255);

    // run detector
    detector.setInput(detect32f.reshape(1, 4, blob_size), "data");
    auto prob = detector.forward("detection_output");

    // process results
    for (int row = 0; row < prob.size[2]; row++) {
        const float* prob_score = prob.ptr<float>(0, 0, row);
        if (prob_score[1] == 1) {
            float x0 = prob_score[3] * undistorted.cols;
            float y0 = prob_score[4] * undistorted.rows;
            float x1 = prob_score[5] * undistorted.cols;
            float y1 = prob_score[6] * undistorted.rows;

            float padx = std::max(0.1 * (x1 - x0), 15.0);
            float pady = std::max(0.1 * (y1 - y0), 15.0);

            int crop_x = std::max(static_cast<int>(x0 - padx), 0);
            int crop_y = std::max(static_cast<int>(y0 - pady), 0);
            int end_x = std::min(static_cast<int>(x1 + padx),
                                 undistorted.cols - 1);
            int end_y = std::min(static_cast<int>(y1 + pady),
                                 undistorted.rows - 1);

            cv::Rect roi(crop_x, crop_y,
                         end_x - crop_x + 1, end_y - crop_y + 1);

            if (roi.width < 20 || roi.height < 20)
                continue;

            // download candidate from GPU to CPU
            cv::Mat candidate;
            undistorted(roi).copyTo(candidate);

            assert(candidate.isContinuous());

            // decode QR Code
            int rc = dbr->DecodeBuffer(candidate.data,
                                       candidate.cols,
                                       candidate.rows,
                                       candidate.step.p[0],
                                       IPF_GRAYSCALED, "");
            if (rc != DBR_OK)
                continue;

            // store results
            TextResultArray* dbrResults = NULL;
            dbr->GetAllTextResults(&dbrResults);

            if (dbrResults != NULL && dbrResults->resultsCount > 0) {
                for (int i = 0; i < dbrResults->resultsCount; ++i) {
                    std::string str(dbrResults->results[i]->barcodeText);
                    results.push_back(str);
                }
            }

            if (dbrResults != NULL)
                dynamsoft::dbr::CBarcodeReader::FreeTextResults(&dbrResults);
        }
    }

    return results.size();
}


const char *QRScan::get_result()
{
    if (curr == results.size())
        return NULL;

    return results.at(curr++).c_str();
}


QRScan::~QRScan()
{
    std::cerr << "QRScan cleanup not supported\n";
    exit(1);
}


extern "C"
{
    QRScan *qrscan_init(const char *undistort_path,
                        const char *detector_prototxt_path,
                        const char *detector_caffe_model_path,
                        unsigned short scan_width,
                        unsigned short scan_height,
                        const char *hwdevice)
    {
        return new QRScan(undistort_path,
                          detector_prototxt_path,
                          detector_caffe_model_path,
                          scan_width,
                          scan_height,
                          hwdevice);
    }

    void qrscan_destroy(QRScan *qrscan)
    {
        delete qrscan;
    }

    int qrscan_process_jpeg(QRScan *qrscan, void *data, size_t size)
    {
        return qrscan->process_jpeg(data, size);
    }

    const char *qrscan_get_result(QRScan *qrscan)
    {
        return qrscan->get_result();
    }
}
