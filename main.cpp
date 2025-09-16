// g++ -std=c++17 capture_uploader.cpp -o capture_uploader `pkg-config --cflags --libs opencv4 libcurl`
// Run: ./capture_uploader
#include <opencv2/opencv.hpp>
#include <curl/curl.h>
#include <chrono>
#include <thread>
#include <iostream>
#include <vector>
#include <string>

static bool post_image(const std::vector<uchar>& jpg) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    curl_easy_setopt(curl, CURLOPT_URL, "https://coffee-maker.apifortytwo.com/api/observation");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "coffee-rpi/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);

    // Uncomment if you need an auth header:
    // struct curl_slist* headers = nullptr;
    // headers = curl_slist_append(headers, "Authorization: Bearer YOUR_TOKEN");
    // curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // Build multipart form with a single "image" field from memory
    curl_mime* mime = curl_mime_init(curl);
    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, "image");
    curl_mime_filename(part, "snapshot.jpg");
    curl_mime_data(part, reinterpret_cast<const char*>(jpg.data()), jpg.size());
    curl_mime_type(part, "image/jpeg");
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

    std::string resp;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
        +[](char* ptr, size_t sz, size_t nm, void* ud)->size_t {
            reinterpret_cast<std::string*>(ud)->append(ptr, sz * nm);
            return sz * nm;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    // cleanup
    curl_mime_free(mime);
    // if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        std::cerr << "HTTP error: " << curl_easy_strerror(rc) << "\n";
        return false;
    }
    std::cout << "POST " << code << " | " << resp << "\n";
    return code >= 200 && code < 300;
}

int main() {
    // Pi cam via GStreamer (libcamera). For USB cam: use cv::VideoCapture cap(0);
    std::string pipeline =
        "libcamerasrc ! video/x-raw,width=1280,height=720,framerate=30/1 ! "
        "videoconvert ! appsink";
    cv::VideoCapture cap(pipeline, cv::CAP_GSTREAMER);
    if (!cap.isOpened()) {
        std::cerr << "Could not open camera\n";
        return 1;
    }

    int i = 0;
    while (true) {
        cv::Mat frame;
        if (!cap.read(frame) || frame.empty()) {
            std::cerr << "Failed to read frame; retrying...\n";
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        // JPEG encode in memory
        std::vector<uchar> jpg;
        std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 85};
        if (!cv::imencode(".jpg", frame, jpg, params)) {
            std::cerr << "JPEG encode failed\n";
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        std::cout << "Shot #" << i++ << " (" << jpg.size() << " bytes)\n";
        if (!post_image(jpg)) {
            std::cerr << "Upload failed\n";
        }

        std::this_thread::sleep_for(std::chrono::seconds(20));
    }
}
