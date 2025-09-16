// g++ -std=c++17 capture_uploader.cpp -o capture_uploader \
//     `pkg-config --cflags --libs opencv4 libcurl`
//
// Run: ./capture_uploader

#include <opencv2/opencv.hpp>
#include <curl/curl.h>
#include <chrono>
#include <thread>
#include <iostream>
#include <vector>
#include <string>

static bool post_image(const std::vector<uchar>& jpg, int shotIndex) {
    std::cout << "[INFO] Starting upload for shot #" << shotIndex
              << " (" << jpg.size() << " bytes)\n";

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "[ERROR] Failed to init curl\n";
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL,
        "https://coffee-maker.apifortytwo.com/api/observation");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "coffee-rpi/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);

    // Build multipart form with one "image" field
    curl_mime* mime = curl_mime_init(curl);
    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, "image");
    curl_mime_filename(part, "snapshot.jpg");
    curl_mime_data(part, reinterpret_cast<const char*>(jpg.data()), jpg.size());
    curl_mime_type(part, "image/jpeg");
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

    // Capture response text
    std::string resp;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
        +[](char* ptr, size_t sz, size_t nm, void* ud)->size_t {
            reinterpret_cast<std::string*>(ud)->append(ptr, sz * nm);
            return sz * nm;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    std::cout << "[INFO] Sending POST request...\n";
    CURLcode rc = curl_easy_perform(curl);

    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    // cleanup
    curl_mime_free(mime);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        std::cerr << "[ERROR] curl_easy_perform failed: "
                  << curl_easy_strerror(rc) << "\n";
        return false;
    }

    std::cout << "[INFO] POST returned HTTP " << code << "\n";
    std::cout << "[INFO] Response body:\n" << resp << "\n";

    return code >= 200 && code < 300;
}

int main() {
    // Pi cam via GStreamer (libcamera).
    std::string pipeline =
        "libcamerasrc ! video/x-raw,width=1280,height=720,framerate=30/1 ! "
        "videoconvert ! appsink";

    // For USB cam: cv::VideoCapture cap(0);
    cv::VideoCapture cap(pipeline, cv::CAP_GSTREAMER);
    if (!cap.isOpened()) {
        std::cerr << "[FATAL] Could not open camera\n";
        return 1;
    }

    std::cout << "[INFO] Camera opened successfully\n";
    int shot = 0;

    while (true) {
        std::cout << "[INFO] Capturing frame for shot #" << shot << "...\n";
        cv::Mat frame;
        if (!cap.read(frame) || frame.empty()) {
            std::cerr << "[WARN] Failed to read frame; retrying in 5s...\n";
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        std::cout << "[INFO] Frame captured: "
                  << frame.cols << "x" << frame.rows
                  << " (" << frame.total() * frame.elemSize() << " bytes raw)\n";

        // JPEG encode in memory
        std::vector<uchar> jpg;
        std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 85};
        if (!cv::imencode(".jpg", frame, jpg, params)) {
            std::cerr << "[ERROR] JPEG encode failed\n";
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        // Upload
        bool ok = post_image(jpg, shot);
        if (ok) {
            std::cout << "[SUCCESS] Shot #" << shot << " uploaded successfully\n";
        } else {
            std::cerr << "[FAIL] Shot #" << shot << " upload failed\n";
        }

        shot++;
        std::cout << "[INFO] Sleeping 20s before next capture...\n\n";
        std::this_thread::sleep_for(std::chrono::seconds(20));
    }
}
