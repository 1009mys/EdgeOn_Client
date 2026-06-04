#include <QApplication>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <opencv2/core/cuda.hpp>
#include <opencv2/opencv.hpp>

#include "DBManager.h"
#include "mainwindow.h"
#include "yolov8inference.h"

namespace {

namespace fs = std::filesystem;

struct BatchStats {
    size_t discoveredFiles{0};
    size_t processedFiles{0};
    size_t failedFiles{0};

    size_t totalCpuDetections{0};
    size_t totalCudaDetections{0};
    size_t perImageCountMatches{0};

    double cpuTotalMs{0.0};
    double cudaTotalMs{0.0};
    double cpuMaxMs{0.0};
    double cudaMaxMs{0.0};

    std::map<int, size_t> cpuClassHistogram;
    std::map<int, size_t> cudaClassHistogram;
    std::vector<std::string> failures;
};

bool isJpgExtension(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext == ".jpg";
}

std::vector<fs::path> collectJpgFiles(const fs::path& directory, const size_t maxCount) {
    std::vector<fs::path> files;

    if (!fs::exists(directory) || !fs::is_directory(directory)) {
        return files;
    }

    for (const auto& entry : fs::directory_iterator(directory)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (!isJpgExtension(entry.path())) {
            continue;
        }
        files.push_back(entry.path());
    }

    std::sort(files.begin(), files.end());
    if (files.size() > maxCount) {
        files.resize(maxCount);
    }
    return files;
}

void addHistogram(std::map<int, size_t>& histogram, const std::vector<Yolov8Detection>& detections) {
    for (const auto& det : detections) {
        ++histogram[det.classId];
    }
}

void printHistogram(const std::string& title, const std::map<int, size_t>& histogram) {
    std::cout << "  " << title << ":";
    if (histogram.empty()) {
        std::cout << " none" << std::endl;
        return;
    }
    std::cout << std::endl;
    for (const auto& [classId, count] : histogram) {
        std::cout << "    class " << classId << " -> " << count << std::endl;
    }
}

void printBatchSummary(const BatchStats& stats, const bool runCudaPath) {
    const double cpuAvg = stats.processedFiles > 0
        ? (stats.cpuTotalMs / static_cast<double>(stats.processedFiles))
        : 0.0;
    const double cudaAvg = (runCudaPath && stats.processedFiles > 0)
        ? (stats.cudaTotalMs / static_cast<double>(stats.processedFiles))
        : 0.0;

    std::cout << "\n===== FINAL AGGREGATED RESULT =====" << std::endl;
    std::cout << "  files discovered : " << stats.discoveredFiles << std::endl;
    std::cout << "  files processed  : " << stats.processedFiles << std::endl;
    std::cout << "  files failed     : " << stats.failedFiles << std::endl;

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "  CPU total detections : " << stats.totalCpuDetections << std::endl;
    std::cout << "  CPU avg ms/image     : " << cpuAvg << std::endl;
    std::cout << "  CPU max ms/image     : " << stats.cpuMaxMs << std::endl;

    if (runCudaPath) {
        std::cout << "  CUDA total detections: " << stats.totalCudaDetections << std::endl;
        std::cout << "  CUDA avg ms/image    : " << cudaAvg << std::endl;
        std::cout << "  CUDA max ms/image    : " << stats.cudaMaxMs << std::endl;
        std::cout << "  count matches        : " << stats.perImageCountMatches
                  << " / " << stats.processedFiles << std::endl;
    } else {
        std::cout << "  CUDA path            : skipped" << std::endl;
    }
    /*
    printHistogram("CPU class histogram", stats.cpuClassHistogram);
    if (runCudaPath) {
        printHistogram("CUDA class histogram", stats.cudaClassHistogram);
    }
    */

    if (!stats.failures.empty()) {
        std::cout << "  failures:" << std::endl;
        for (const auto& fail : stats.failures) {
            std::cout << "    - " << fail << std::endl;
        }
    }
}

BatchStats runBatchInferenceTest(Yolov8Inference& inference,
                                 const fs::path& imageDirectory,
                                 const size_t maxImages) {
    BatchStats stats;

    if (!inference.isLoaded()) {
        stats.failures.emplace_back("model is not loaded");
        return stats;
    }

    const auto files = collectJpgFiles(imageDirectory, maxImages);
    stats.discoveredFiles = files.size();

    const bool hasCudaDevice = cv::cuda::getCudaEnabledDeviceCount() > 0;
    const bool runCudaPath = hasCudaDevice && inference.getExecutionProviderName() == "CUDA";

    std::cout << "\n[Batch Test] directory : " << imageDirectory.string() << std::endl;
    std::cout << "[Batch Test] jpg files  : " << files.size() << " (max " << maxImages << ")" << std::endl;
    std::cout << "[Batch Test] CUDA path  : " << (runCudaPath ? "enabled" : "disabled") << std::endl;

    if (files.empty()) {
        stats.failures.emplace_back("no .jpg files found in the provided directory");
        return stats;
    }

    for (size_t index = 0; index < files.size(); ++index) {
        const auto& file = files[index];
        try {
            cv::Mat cpuImage = cv::imread(file.string(), cv::IMREAD_COLOR);
            if (cpuImage.empty()) {
                ++stats.failedFiles;
                stats.failures.emplace_back("read failed: " + file.string());
                continue;
            }

            const auto cpuStart = std::chrono::steady_clock::now();
            const auto cpuDetections = inference.inference(cpuImage, 0.25f, 0.45f, true);
            const auto cpuEnd = std::chrono::steady_clock::now();

            const double cpuMs = std::chrono::duration<double, std::milli>(cpuEnd - cpuStart).count();
            stats.cpuTotalMs += cpuMs;
            stats.cpuMaxMs = std::max(stats.cpuMaxMs, cpuMs);
            stats.totalCpuDetections += cpuDetections.size();
            addHistogram(stats.cpuClassHistogram, cpuDetections);

            size_t cudaCount = 0;
            if (runCudaPath) {
                cv::cuda::GpuMat gpuImage;
                gpuImage.upload(cpuImage);

                const auto cudaStart = std::chrono::steady_clock::now();
                const auto cudaDetections = inference.inference_cuda(gpuImage, 0.25f, 0.45f, true);
                const auto cudaEnd = std::chrono::steady_clock::now();

                const double cudaMs = std::chrono::duration<double, std::milli>(cudaEnd - cudaStart).count();
                stats.cudaTotalMs += cudaMs;
                stats.cudaMaxMs = std::max(stats.cudaMaxMs, cudaMs);

                cudaCount = cudaDetections.size();
                stats.totalCudaDetections += cudaCount;
                addHistogram(stats.cudaClassHistogram, cudaDetections);

                if (cudaCount == cpuDetections.size()) {
                    ++stats.perImageCountMatches;
                }
            }

            ++stats.processedFiles;
            std::cout << "  [" << (index + 1) << "/" << files.size() << "] "
                      << file.filename().string()
                      << " | CPU det=" << cpuDetections.size();
            if (runCudaPath) {
                std::cout << " | CUDA det=" << cudaCount;
            }
            std::cout << std::endl;
        } catch (const std::exception& ex) {
            ++stats.failedFiles;
            stats.failures.emplace_back(file.string() + " | " + ex.what());
        }
    }

    printBatchSummary(stats, runCudaPath);
    return stats;
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName("EdgeOn");
    QCoreApplication::setOrganizationDomain("edgeon.local");
    QCoreApplication::setApplicationName("RTSP Multi-Channel Viewer");

    DBManager& dbManager = DBManager::instance("db.db");
    dbManager.initialize();
    /*
    const std::string modelPath = "models/yolov8s.onnx";
    const fs::path imageDirectory = "E:\\datasets\\coco\\val2017\\val2017";
    constexpr size_t kMaxImages = 100;

    try {
        Yolov8Inference inference;
        std::cout << "[YOLOv8] Loading model: " << modelPath << std::endl;
        inference.load(modelPath);
        std::cout << "  ExecutionProvider : " << inference.getExecutionProviderName() << std::endl;
        std::cout << "  LoadMessage       : " << inference.getLastLoadMessage() << std::endl;
        std::cout << "  InputSize         : ["
                  << inference.getInputSize().width << ", "
                  << inference.getInputSize().height << "]" << std::endl;

        const BatchStats stats = runBatchInferenceTest(inference, imageDirectory, kMaxImages);
        if (stats.processedFiles == 0) {
            std::cerr << "[YOLOv8] No images were successfully processed." << std::endl;
        }

        inference.close();
    } catch (const std::exception& ex) {
        std::cerr << "[YOLOv8] Error: " << ex.what() << std::endl;
    }*/

    MainWindow w;
    w.show();

    return app.exec();
}

