#include <opencv2/opencv.hpp>
#include <iostream>

using namespace cv;
using namespace cv::dnn;

int main(int argc, char *argv[])
{
    if (argc != 2) {
        std::cerr << "Usage: qrtest <image-file>\n";
        return 1;
    }

    Mat img = imread(argv[1]);

    Mat gray;
    cvtColor(img, gray, COLOR_BGR2GRAY);

    Net net = readNetFromCaffe(
        "detect.prototxt",
        "detect.caffemodel"
    );
    net.setPreferableBackend(DNN_BACKEND_OPENCV);
    net.setPreferableTarget(DNN_TARGET_OPENCL);

    Mat detect;
    resize(gray, detect, Size(533, 300), 0, 0, INTER_LINEAR);

    Mat blob = blobFromImage(detect, 1.0 / 255, Size(detect.cols, detect.rows),
                             {0.0f, 0.0f, 0.0f}, false, false);

    net.setInput(blob, "data");
    net.forward("detection_output");

    return 0;
}
