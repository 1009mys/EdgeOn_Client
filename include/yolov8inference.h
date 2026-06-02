#pragma once

#include <string>
#include <vector>

#include <opencv2/opencv.hpp>
#include <opencv2/core/cuda.hpp>

#include "onnxinference.h"

struct Yolov8Detection {
    int classId{-1};
    float score{0.0f};
    cv::Rect2f box{};
};

struct Yolov8PreprocessResult {
    std::vector<Ort::Value> tensors;
    std::vector<float> tensorBuffer;
    cv::Size originalSize{};
    cv::Size inputSize{};
    float scale{1.0f};
    int padX{0};
    int padY{0};
};

// GPU 메모리에 상주하는 전처리 결과.
// gpuBuffer는 cudaMalloc으로 할당된 CHW float32 데이터를 소유하며,
// tensors 안의 Ort::Value가 해당 포인터를 직접 참조한다.
struct Yolov8CudaPreprocessResult {
    std::vector<Ort::Value> tensors;
    float* gpuBuffer{nullptr}; // CUDA 메모리 소유권 보관
    cv::Size originalSize{};
    cv::Size inputSize{};
    float scale{1.0f};
    int padX{0};
    int padY{0};

    Yolov8CudaPreprocessResult() = default;
    ~Yolov8CudaPreprocessResult();
    Yolov8CudaPreprocessResult(const Yolov8CudaPreprocessResult&) = delete;
    Yolov8CudaPreprocessResult& operator=(const Yolov8CudaPreprocessResult&) = delete;
    Yolov8CudaPreprocessResult(Yolov8CudaPreprocessResult&& other) noexcept;
    Yolov8CudaPreprocessResult& operator=(Yolov8CudaPreprocessResult&& other) noexcept;
};

class Yolov8Inference {
public:
    Yolov8Inference();

    void load(std::string path);
    void close();

    [[nodiscard]] bool isLoaded() const;
    [[nodiscard]] cv::Size getInputSize() const;
    [[nodiscard]] std::string getExecutionProviderName() const;
    [[nodiscard]] std::string getLastLoadMessage() const;

    // ── CPU 경로 ──────────────────────────────────────────────────────────────
    Yolov8PreprocessResult preprocess(const cv::Mat& image) const;
    std::vector<Yolov8Detection> inference(const cv::Mat& image,
                                          float confidenceThreshold = 0.25f,
                                          float iouThreshold = 0.45f,
                                          bool decodeOutput = true) const;
    std::vector<Yolov8Detection> postprocess(const std::vector<Ort::Value>& outputs,
                                             const Yolov8PreprocessResult& preprocessResult,
                                             float confidenceThreshold,
                                             float iouThreshold,
                                             bool decodeOutput) const;

    // ── CUDA 경로 (입력 전처리가 GPU 메모리를 벗어나지 않음) ──────────────────
    Yolov8CudaPreprocessResult preprocess_cuda(const cv::cuda::GpuMat& image) const;
    std::vector<Yolov8Detection> inference_cuda(const cv::cuda::GpuMat& image,
                                                float confidenceThreshold = 0.25f,
                                                float iouThreshold = 0.45f,
                                                bool decodeOutput = true) const;
    std::vector<Yolov8Detection> postprocess_cuda(const std::vector<Ort::Value>& outputs,
                                                   const Yolov8CudaPreprocessResult& preprocessResult,
                                                   float confidenceThreshold,
                                                   float iouThreshold,
                                                   bool decodeOutput) const;

private:
    static cv::Size resolveInputSize(const std::vector<int64_t>& shape);

    // CPU 전처리 헬퍼
    static cv::Mat makeLetterboxedImage(const cv::Mat& image,
                                        const cv::Size& targetSize,
                                        float& scale,
                                        int& padX,
                                        int& padY);

    // CUDA 전처리 헬퍼 – 결과는 GpuMat 로 반환, CPU 경유 없음
    static cv::cuda::GpuMat makeLetterboxedImageCuda(const cv::cuda::GpuMat& image,
                                                      const cv::Size& targetSize,
                                                      float& scale,
                                                      int& padX,
                                                      int& padY);

    static cv::Rect2f clampBox(const cv::Rect2f& box, const cv::Size& size);
    static float intersectionOverUnion(const cv::Rect2f& lhs, const cv::Rect2f& rhs);
    static std::vector<Yolov8Detection> nonMaxSuppression(std::vector<Yolov8Detection> detections, float iouThreshold);
    static bool isDecodedDetectionOutput(const std::vector<int64_t>& shape);
    static std::vector<int64_t> squeezeShape(const std::vector<int64_t>& shape);
    std::vector<Yolov8Detection> decodeOutputTensor(const Ort::Value& output,
                                                    const Yolov8PreprocessResult& preprocessResult,
                                                    float confidenceThreshold,
                                                    bool decodeOutput) const;

    OnnxInference m_onnx;
    cv::Size m_inputSize{640, 640};
};

