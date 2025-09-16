// g++ -std=c++17 capture_uploader.cpp -o capture_uploader `pkg-config --cflags --libs opencv4 libcurl`
// Run: ./capture_uploader

#include <opencv2/opencv.hpp>
#include <curl/curl.h>
#include <chrono>
#include <thread>
#include <iostream>
#include <vector>
#include <string>
#include <iomanip>
#include <sstream>
#include <signal.h>

// -------- CONFIG: choose ONE of these --------
#define USE_USB_CAM                 // use /dev/video* (USB cam)
// #define USE_LIBCAMERA_GSTREAMER   // Pi cam via libcamera/GStreamer

static std::string ts() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t = system_clock::to_time_t(now);
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
        << "." << std::setw(3) << std::setfill('0') << ms.count();
    return oss.str();
}

static void log_info(const std::string& m){ std::cerr << ts() << " [INFO]  " << m << "\n"; }
static void log_warn(const std::string& m){ std::cerr << ts() << " [WARN]  " << m << "\n"; }
static void log_err (const std::string& m){ std::cerr << ts() << " [ERROR] " << m << "\n"; }
static void log_ok  (const std::string& m){ std::cerr << ts() << " [OK]    " << m << "\n"; }

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int){ g_stop = 1; }

static size_t write_cb(char* ptr, size_t sz, size_t nm, void* ud){
    auto* s = static_cast<std::string*>(ud);
    s->append(ptr, sz*nm);
    return sz*nm;
}

static bool post_image(const std::vector<uchar>& jpg, int shot) {
    log_info("Preparing POST for shot #" + std::to_string(shot) + " (" + std::to_string(jpg.size()) + " bytes)");

    CURL* curl = curl_easy_init();
    if (!curl) { log_err("curl_easy_init failed"); return false; }

    curl_easy_setopt(curl, CURLOPT_URL, "https://coffee-maker.apifortytwo.com/api/observation");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "coffee-rpi/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L); // libcurl debug to stderr

    // Optional auth header:
    // struct curl_slist* headers = nullptr;
    // headers = curl_slist_append(headers, "Authorization: Bearer YOUR_TOKEN_HERE");
    // curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // multipart: single field "image"
    curl_mime* mime = curl_mime_init(curl);
    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, "image");
    curl_mime_filename(part, "snapshot.jpg");
    curl_mime_data(part, reinterpret_cast<const char*>(jpg.data()), jpg.size());
    curl_mime_type(part, "image/jpeg");
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

    std::string resp;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    log_info("Sending POST …");
    CURLcode rc = curl_easy_perform(curl);

    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);

    // cleanup
    curl_mime_free(mime);
    // if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        log_err(std::string("POST failed: ") + curl_easy_strerror(rc));
        return false;
    }

    std::cerr << ts() << " [INFO]  HTTP status: " << http << "\n";
    std::cerr << ts() << " [INFO]  Response body: " << (resp.empty()? "<empty>" : resp) << "\n";

    if (http >= 200 && http < 300) {
        log_ok("Upload success (shot #" + std::to_string(shot) + ")");
        return true;
    } else {
        log_warn("Non-2xx response for shot #" + std::to_string(shot));
        return false;
    }
}

int main() {
    signal(SIGINT, on_sigint);
    log_info("Starting capture_uploader (interval 20s)");

    // Global init/cleanup for libcurl
    curl_global_init(CURL_GLOBAL_DEFAULT);

#if defined(USE_USB_CAM)
    // Prefer V4L2 backend for USB cams
    cv::VideoCapture cap(0, cv::CAP_V4L2);
    log_info("Opening camera: device index 0 (USB, V4L2)");
    // Ask for common settings (camera may clamp to supported values)
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 1280);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 720);
    cap.set(cv::CAP_PROP_FPS, 30);
#elif defined(USE_LIBCAMERA_GSTREAMER)
    std::string pipeline =
        "libcamerasrc ! video/x-raw,width=1280,height=720,framerate=30/1 ! "
        "videoconvert ! appsink";
    cv::VideoCapture cap(pipeline, cv::CAP_GSTREAMER);
    log_info("Opening camera: GStreamer pipeline via libcamera");
#else
#error "Select a capture source (USE_USB_CAM or USE_LIBCAMERA_GSTREAMER)"
#endif

    if (!cap.isOpened()) {
        log_err("Could not open camera (is OpenCV built with the right backend?)");
        log_info("Tips: try a different index (1/2), ensure user is in 'video' group, or switch backend.");
        curl_global_cleanup();
        return 1;
    }
    log_ok("Camera opened");

    int shot = 0;
    while (!g_stop) {
        log_info("Capturing frame for shot #" + std::to_string(shot));
        cv::Mat frame;
        for (int i = 0; i < 5; i++) {
            cap.grab(); // just grab without decoding
        }
        if (!cap.read(frame) || frame.empty()) {
            log_warn("Failed to read frame; retrying in 5s …");
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        log_ok("Frame captured: " + std::to_string(frame.cols) + "x" + std::to_string(frame.rows) +
               ", raw ~" + std::to_string(frame.total() * frame.elemSize()) + " bytes");

        // JPEG encode
        std::vector<uchar> jpg;
        std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 85};
        if (!cv::imencode(".jpg", frame, jpg, params)) {
            log_err("JPEG encode failed; retrying in 5s …");
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        log_info("JPEG encoded: " + std::to_string(jpg.size()) + " bytes");

        // Upload
        bool ok = post_image(jpg, shot);
        if (!ok) log_warn("Upload failed for shot #" + std::to_string(shot));

        for (int s = 20; s > 0 && !g_stop; --s) {
            std::cerr << ts() << " [INFO]  Sleeping … " << s << "s     \r" << std::flush;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        std::cerr << "\n";
        ++shot;
    }

    log_info("Exiting (SIGINT received)");
    curl_global_cleanup();
    return 0;
}
