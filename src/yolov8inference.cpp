#include "yolov8inference.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include <cuda_runtime.h>
#include <opencv2/cudaarithm.hpp>
#include <opencv2/cudaimgproc.hpp>
#include <opencv2/cudawarping.hpp>

namespace {
const cv::Scalar kLetterboxColor(114, 114, 114);
const cv::Scalar_<float> kLetterboxColorF(114.0f / 255.0f, 114.0f / 255.0f, 114.0f / 255.0f);
constexpr int kDefaultInputSize = 640;

float normalizeProbability(float value) {
    if (value >= 0.0f && value <= 1.0f) {
        return value;
    }

    return 1.0f / (1.0f + std::exp(-value));
}

int64_t safeShapeValue(const std::vector<int64_t>& shape, const size_t index) {
    if (index >= shape.size()) {
        return 0;
    }
    return shape[index];
}

} // namespace

Yolov8CudaPreprocessResult::~Yolov8CudaPreprocessResult() {
    if (gpuBuffer != nullptr) {
        cudaFree(gpuBuffer);
        gpuBuffer = nullptr;
    }
}

Yolov8CudaPreprocessResult::Yolov8CudaPreprocessResult(Yolov8CudaPreprocessResult&& other) noexcept
    : tensors(std::move(other.tensors)),
      gpuBuffer(other.gpuBuffer),
      originalSize(other.originalSize),
      inputSize(other.inputSize),
      scale(other.scale),
      padX(other.padX),
      padY(other.padY) {
    other.gpuBuffer = nullptr;
}

Yolov8CudaPreprocessResult& Yolov8CudaPreprocessResult::operator=(Yolov8CudaPreprocessResult&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    if (gpuBuffer != nullptr) {
        cudaFree(gpuBuffer);
    }

    tensors = std::move(other.tensors);
    gpuBuffer = other.gpuBuffer;
    originalSize = other.originalSize;
    inputSize = other.inputSize;
    scale = other.scale;
    padX = other.padX;
    padY = other.padY;
    other.gpuBuffer = nullptr;
    return *this;
}

Yolov8Inference::Yolov8Inference() = default;

void Yolov8Inference::load(std::string path) {
    m_onnx.load(std::move(path));

    const auto& inputShapes = m_onnx.getInputShapes();
    if (!inputShapes.empty()) {
        m_inputSize = resolveInputSize(inputShapes.front());
    } else {
        m_inputSize = cv::Size{kDefaultInputSize, kDefaultInputSize};
    }
}

void Yolov8Inference::close() {
    m_onnx.close();
    m_inputSize = cv::Size{kDefaultInputSize, kDefaultInputSize};
}

bool Yolov8Inference::isLoaded() const {
    return m_onnx.isLoaded();
}

cv::Size Yolov8Inference::getInputSize() const {
    return m_inputSize;
}

std::string Yolov8Inference::getExecutionProviderName() const {
    return m_onnx.getExecutionProviderName();
}

std::string Yolov8Inference::getLastLoadMessage() const {
    return m_onnx.getLastLoadMessage();
}

cv::Size Yolov8Inference::resolveInputSize(const std::vector<int64_t>& shape) {
    if (shape.size() >= 4) {
        if (shape[1] == 3) {
            const int64_t height = safeShapeValue(shape, shape.size() - 2);
            const int64_t width = safeShapeValue(shape, shape.size() - 1);
            if (height > 0 && width > 0) {
                return cv::Size{static_cast<int>(width), static_cast<int>(height)};
            }
        }

        if (shape.back() == 3) {
            const int64_t height = safeShapeValue(shape, 1);
            const int64_t width = safeShapeValue(shape, 2);
            if (height > 0 && width > 0) {
                return cv::Size{static_cast<int>(width), static_cast<int>(height)};
            }
        }
    }

    return cv::Size{kDefaultInputSize, kDefaultInputSize};
}

cv::Mat Yolov8Inference::makeLetterboxedImage(const cv::Mat& image,
                                              const cv::Size& targetSize,
                                              float& scale,
                                              int& padX,
                                              int& padY) {
    if (image.empty()) {
        throw std::runtime_error("Input image is empty.");
    }

    cv::Mat sourceBgr;
    switch (image.channels()) {
        case 3:
            sourceBgr = image;
            break;
        case 4:
            cv::cvtColor(image, sourceBgr, cv::COLOR_BGRA2BGR);
            break;
        case 1:
            cv::cvtColor(image, sourceBgr, cv::COLOR_GRAY2BGR);
            break;
        default:
            throw std::runtime_error("Unsupported image channel count for YOLOv8 preprocessing.");
    }

    if (targetSize.width <= 0 || targetSize.height <= 0) {
        throw std::runtime_error("Invalid YOLOv8 input size.");
    }

    const float scaleX = static_cast<float>(targetSize.width) / static_cast<float>(sourceBgr.cols);
    const float scaleY = static_cast<float>(targetSize.height) / static_cast<float>(sourceBgr.rows);
    scale = std::min(scaleX, scaleY);

    const int resizedWidth = std::max(1, static_cast<int>(std::round(static_cast<float>(sourceBgr.cols) * scale)));
    const int resizedHeight = std::max(1, static_cast<int>(std::round(static_cast<float>(sourceBgr.rows) * scale)));

    padX = (targetSize.width - resizedWidth) / 2;
    padY = (targetSize.height - resizedHeight) / 2;

    cv::Mat resized;
    cv::resize(sourceBgr, resized, cv::Size{resizedWidth, resizedHeight}, 0.0, 0.0, cv::INTER_LINEAR);

    cv::Mat letterboxed(targetSize, CV_8UC3, kLetterboxColor);
    resized.copyTo(letterboxed(cv::Rect{padX, padY, resizedWidth, resizedHeight}));
    return letterboxed;
}

Yolov8PreprocessResult Yolov8Inference::preprocess(const cv::Mat& image) const {
    Yolov8PreprocessResult result;
    result.originalSize = image.size();
    result.inputSize = m_inputSize;

    float scale = 1.0f;
    int padX = 0;
    int padY = 0;
    const cv::Mat letterboxed = makeLetterboxedImage(image, result.inputSize, scale, padX, padY);
    result.scale = scale;
    result.padX = padX;
    result.padY = padY;

    cv::Mat rgb;
    cv::cvtColor(letterboxed, rgb, cv::COLOR_BGR2RGB);
    rgb.convertTo(rgb, CV_32FC3, 1.0 / 255.0);

    const int width = rgb.cols;
    const int height = rgb.rows;
    const size_t planeSize = static_cast<size_t>(width) * static_cast<size_t>(height);

    result.tensorBuffer.resize(planeSize * 3);
    std::vector<float>& buffer = result.tensorBuffer;

    for (int y = 0; y < height; ++y) {
        const cv::Vec3f* row = rgb.ptr<cv::Vec3f>(y);
        for (int x = 0; x < width; ++x) {
            const cv::Vec3f& pixel = row[x];
            const size_t index = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
            buffer[index] = pixel[0];
            buffer[planeSize + index] = pixel[1];
            buffer[planeSize * 2 + index] = pixel[2];
        }
    }

    const std::vector<int64_t> shape{
        1,
        3,
        static_cast<int64_t>(height),
        static_cast<int64_t>(width)
    };

    const Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    result.tensors.emplace_back(Ort::Value::CreateTensor<float>(
        memoryInfo,
        buffer.data(),
        buffer.size(),
        shape.data(),
        shape.size()
    ));

    return result;
}

std::vector<Yolov8Detection> Yolov8Inference::inference(const cv::Mat& image,
                                                        float confidenceThreshold,
                                                        float iouThreshold,
                                                        bool decodeOutput) const {
    if (!isLoaded()) {
        throw std::runtime_error("No YOLOv8 model is loaded.");
    }

    auto preprocessResult = preprocess(image);
    const auto outputs = m_onnx.run(preprocessResult.tensors);
    return postprocess(outputs, preprocessResult, confidenceThreshold, iouThreshold, decodeOutput);
}

std::vector<Yolov8Detection> Yolov8Inference::postprocess(const std::vector<Ort::Value>& outputs,
                                                          const Yolov8PreprocessResult& preprocessResult,
                                                          float confidenceThreshold,
                                                          float iouThreshold,
                                                          bool decodeOutput) const {
    std::vector<Yolov8Detection> detections;

    for (const Ort::Value& output : outputs) {
        auto decoded = decodeOutputTensor(output, preprocessResult, confidenceThreshold, decodeOutput);
        detections.insert(detections.end(), decoded.begin(), decoded.end());
    }

    if (decodeOutput) {
        return nonMaxSuppression(std::move(detections), iouThreshold);
    }

    return detections;
}

std::vector<int64_t> Yolov8Inference::squeezeShape(const std::vector<int64_t>& shape) {
    std::vector<int64_t> result = shape;
    while (result.size() > 1 && result.front() == 1) {
        result.erase(result.begin());
    }
    return result;
}

bool Yolov8Inference::isDecodedDetectionOutput(const std::vector<int64_t>& shape) {
    if (shape.size() == 2) {
        return shape[0] == 6 || shape[1] == 6;
    }

    if (shape.size() == 3) {
        return shape[1] == 6 || shape[2] == 6;
    }

    return false;
}

cv::Rect2f Yolov8Inference::clampBox(const cv::Rect2f& box, const cv::Size& size) {
    if (size.width <= 0 || size.height <= 0) {
        return box;
    }

    const float left = std::clamp(box.x, 0.0f, static_cast<float>(size.width - 1));
    const float top = std::clamp(box.y, 0.0f, static_cast<float>(size.height - 1));
    const float right = std::clamp(box.x + box.width, 0.0f, static_cast<float>(size.width - 1));
    const float bottom = std::clamp(box.y + box.height, 0.0f, static_cast<float>(size.height - 1));

    return cv::Rect2f{left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top)};
}

float Yolov8Inference::intersectionOverUnion(const cv::Rect2f& lhs, const cv::Rect2f& rhs) {
    const float left = std::max(lhs.x, rhs.x);
    const float top = std::max(lhs.y, rhs.y);
    const float right = std::min(lhs.x + lhs.width, rhs.x + rhs.width);
    const float bottom = std::min(lhs.y + lhs.height, rhs.y + rhs.height);

    const float intersectionWidth = std::max(0.0f, right - left);
    const float intersectionHeight = std::max(0.0f, bottom - top);
    const float intersectionArea = intersectionWidth * intersectionHeight;
    const float lhsArea = std::max(0.0f, lhs.width) * std::max(0.0f, lhs.height);
    const float rhsArea = std::max(0.0f, rhs.width) * std::max(0.0f, rhs.height);
    const float unionArea = lhsArea + rhsArea - intersectionArea;

    if (unionArea <= 0.0f) {
        return 0.0f;
    }

    return intersectionArea / unionArea;
}

std::vector<Yolov8Detection> Yolov8Inference::nonMaxSuppression(std::vector<Yolov8Detection> detections,
                                                                float iouThreshold) {
    if (detections.empty()) {
        return {};
    }

    std::sort(detections.begin(), detections.end(), [](const Yolov8Detection& lhs, const Yolov8Detection& rhs) {
        return lhs.score > rhs.score;
    });

    std::vector<Yolov8Detection> selected;
    std::vector<bool> suppressed(detections.size(), false);

    for (size_t i = 0; i < detections.size(); ++i) {
        if (suppressed[i]) {
            continue;
        }

        selected.push_back(detections[i]);

        for (size_t j = i + 1; j < detections.size(); ++j) {
            if (suppressed[j]) {
                continue;
            }

            if (detections[i].classId != detections[j].classId) {
                continue;
            }

            if (intersectionOverUnion(detections[i].box, detections[j].box) > iouThreshold) {
                suppressed[j] = true;
            }
        }
    }

    return selected;
}

std::vector<Yolov8Detection> Yolov8Inference::decodeOutputTensor(const Ort::Value& output,
                                                                 const Yolov8PreprocessResult& preprocessResult,
                                                                 float confidenceThreshold,
                                                                 bool decodeOutput) const {
    static_cast<void>(decodeOutput);

    const Ort::TensorTypeAndShapeInfo tensorInfo = output.GetTensorTypeAndShapeInfo();
    std::vector<int64_t> shape = squeezeShape(tensorInfo.GetShape());

    if (shape.empty()) {
        return {};
    }

    if (tensorInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        throw std::runtime_error("YOLOv8 output tensor must be float32.");
    }

    const float* data = output.GetTensorData<float>();
    if (data == nullptr) {
        return {};
    }

    const bool decodedOutput = isDecodedDetectionOutput(shape);
    const int64_t networkWidth = std::max<int64_t>(1, preprocessResult.inputSize.width);
    const int64_t networkHeight = std::max<int64_t>(1, preprocessResult.inputSize.height);

    std::vector<Yolov8Detection> detections;

    if (decodedOutput) {
        int64_t rows = 0;
        int64_t cols = 0;
        bool rowMajor = true;

        if (shape.size() == 2) {
            if (shape[1] == 6) {
                rows = shape[0];
                cols = 6;
            } else {
                rows = shape[1];
                cols = shape[0];
                rowMajor = false;
            }
        } else if (shape.size() == 3) {
            if (shape[2] == 6) {
                rows = shape[1];
                cols = 6;
            } else {
                rows = shape[2];
                cols = shape[1];
                rowMajor = false;
            }
        }

        if (rows <= 0 || cols < 6) {
            return {};
        }

        detections.reserve(static_cast<size_t>(rows));
        for (int64_t rowIndex = 0; rowIndex < rows; ++rowIndex) {
            const float* row = rowMajor ? (data + rowIndex * cols) : (data + rowIndex);
            if (rowMajor) {
                const float score = normalizeProbability(row[4]);
                if (score < confidenceThreshold) {
                    continue;
                }

                const float x1 = row[0];
                const float y1 = row[1];
                const float x2 = row[2];
                const float y2 = row[3];
                const int classId = static_cast<int>(std::round(row[5]));

                cv::Rect2f box{x1, y1, x2 - x1, y2 - y1};
                if (std::max({std::abs(box.x), std::abs(box.y), std::abs(box.width), std::abs(box.height)}) <= 2.0f) {
                    box.x *= static_cast<float>(networkWidth);
                    box.y *= static_cast<float>(networkHeight);
                    box.width *= static_cast<float>(networkWidth);
                    box.height *= static_cast<float>(networkHeight);
                }

                box.x = (box.x - static_cast<float>(preprocessResult.padX)) / preprocessResult.scale;
                box.y = (box.y - static_cast<float>(preprocessResult.padY)) / preprocessResult.scale;
                box.width /= preprocessResult.scale;
                box.height /= preprocessResult.scale;
                detections.push_back({classId, score, clampBox(box, preprocessResult.originalSize)});
            } else {
                const float score = normalizeProbability(row[4 * rows]);
                if (score < confidenceThreshold) {
                    continue;
                }

                const float x1 = row[0 * rows];
                const float y1 = row[1 * rows];
                const float x2 = row[2 * rows];
                const float y2 = row[3 * rows];
                const int classId = static_cast<int>(std::round(row[5 * rows]));

                cv::Rect2f box{x1, y1, x2 - x1, y2 - y1};
                if (std::max({std::abs(box.x), std::abs(box.y), std::abs(box.width), std::abs(box.height)}) <= 2.0f) {
                    box.x *= static_cast<float>(networkWidth);
                    box.y *= static_cast<float>(networkHeight);
                    box.width *= static_cast<float>(networkWidth);
                    box.height *= static_cast<float>(networkHeight);
                }

                box.x = (box.x - static_cast<float>(preprocessResult.padX)) / preprocessResult.scale;
                box.y = (box.y - static_cast<float>(preprocessResult.padY)) / preprocessResult.scale;
                box.width /= preprocessResult.scale;
                box.height /= preprocessResult.scale;
                detections.push_back({classId, score, clampBox(box, preprocessResult.originalSize)});
            }
        }

        return detections;
    }

    int64_t rows = 0;
    int64_t attributes = 0;
    bool channelsFirst;

    if (shape.size() == 2) {
        if (shape[0] <= 128 && shape[1] > shape[0]) {
            attributes = shape[0];
            rows = shape[1];
            channelsFirst = true;
        } else {
            rows = shape[0];
            attributes = shape[1];
            channelsFirst = false;
        }
    } else if (shape.size() == 3) {
        if (shape[1] <= 128 && shape[2] > shape[1]) {
            attributes = shape[1];
            rows = shape[2];
            channelsFirst = true;
        } else {
            rows = shape[1];
            attributes = shape[2];
            channelsFirst = false;
        }
    }

    if (rows <= 0 || attributes < 6) {
        return {};
    }

    const int scoreOffset = (attributes == 85) ? 5 : 4;
    const int classCount = static_cast<int>(attributes - scoreOffset);
    if (classCount <= 0) {
        return {};
    }

    detections.reserve(static_cast<size_t>(rows));
    for (int64_t rowIndex = 0; rowIndex < rows; ++rowIndex) {
        float centerX = 0.0f;
        float centerY = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float objectness = 1.0f;

        if (channelsFirst) {
            const int64_t rowOffset = rowIndex;
            centerX = data[0 * rows + rowOffset];
            centerY = data[1 * rows + rowOffset];
            width = data[2 * rows + rowOffset];
            height = data[3 * rows + rowOffset];
            if (scoreOffset == 5) {
                objectness = normalizeProbability(data[4 * rows + rowOffset]);
            }
        } else {
            const float* row = data + rowIndex * attributes;
            centerX = row[0];
            centerY = row[1];
            width = row[2];
            height = row[3];
            if (scoreOffset == 5) {
                objectness = normalizeProbability(row[4]);
            }
        }

        if (std::max({std::abs(centerX), std::abs(centerY), std::abs(width), std::abs(height)}) <= 2.0f) {
            centerX *= static_cast<float>(networkWidth);
            centerY *= static_cast<float>(networkHeight);
            width *= static_cast<float>(networkWidth);
            height *= static_cast<float>(networkHeight);
        }

        int bestClassId = -1;
        float bestScore = 0.0f;

        for (int classIndex = 0; classIndex < classCount; ++classIndex) {
            float classScore = 0.0f;
            if (channelsFirst) {
                classScore = data[(scoreOffset + classIndex) * rows + rowIndex];
            } else {
                classScore = data[rowIndex * attributes + scoreOffset + classIndex];
            }

            classScore = normalizeProbability(classScore);

            if (scoreOffset == 5) {
                classScore *= objectness;
            }

            if (classScore > bestScore) {
                bestScore = classScore;
                bestClassId = classIndex;
            }
        }

        if (bestClassId < 0 || bestScore < confidenceThreshold) {
            continue;
        }

        const float x1 = centerX - width * 0.5f;
        const float y1 = centerY - height * 0.5f;
        cv::Rect2f box{x1, y1, width, height};
        box.x = (box.x - static_cast<float>(preprocessResult.padX)) / preprocessResult.scale;
        box.y = (box.y - static_cast<float>(preprocessResult.padY)) / preprocessResult.scale;
        box.width /= preprocessResult.scale;
        box.height /= preprocessResult.scale;

        detections.push_back({bestClassId, bestScore, clampBox(box, preprocessResult.originalSize)});
    }

    return detections;
}

// ── CUDA 경로 구현 ────────────────────────────────────────────────────────────

cv::cuda::GpuMat Yolov8Inference::makeLetterboxedImageCuda(const cv::cuda::GpuMat& image,
                                                            const cv::Size& targetSize,
                                                            float& scale,
                                                            int& padX,
                                                            int& padY) {
    if (image.empty()) {
        throw std::runtime_error("Input GpuMat is empty.");
    }
    if (targetSize.width <= 0 || targetSize.height <= 0) {
        throw std::runtime_error("Invalid YOLOv8 input size.");
    }

    // 채널 변환 (GPU)
    cv::cuda::GpuMat sourceBgr;
    switch (image.channels()) {
        case 3:
            sourceBgr = image;
            break;
        case 4:
            cv::cuda::cvtColor(image, sourceBgr, cv::COLOR_BGRA2BGR);
            break;
        case 1:
            cv::cuda::cvtColor(image, sourceBgr, cv::COLOR_GRAY2BGR);
            break;
        default:
            throw std::runtime_error("Unsupported image channel count for YOLOv8 CUDA preprocessing.");
    }

    const float scaleX = static_cast<float>(targetSize.width)  / static_cast<float>(sourceBgr.cols);
    const float scaleY = static_cast<float>(targetSize.height) / static_cast<float>(sourceBgr.rows);
    scale = std::min(scaleX, scaleY);

    const int resizedWidth  = std::max(1, static_cast<int>(std::round(static_cast<float>(sourceBgr.cols) * scale)));
    const int resizedHeight = std::max(1, static_cast<int>(std::round(static_cast<float>(sourceBgr.rows) * scale)));

    padX = (targetSize.width  - resizedWidth)  / 2;
    padY = (targetSize.height - resizedHeight) / 2;

    // 리사이즈 (GPU)
    cv::cuda::GpuMat resized;
    cv::cuda::resize(sourceBgr, resized, cv::Size{resizedWidth, resizedHeight}, 0.0, 0.0, cv::INTER_LINEAR);

    // 레터박스 패딩 – GPU 에서 전체 캔버스를 회색으로 채운 뒤 ROI에 복사
    cv::cuda::GpuMat letterboxed(targetSize, CV_8UC3, kLetterboxColor);
    resized.copyTo(letterboxed(cv::Rect{padX, padY, resizedWidth, resizedHeight}));

    return letterboxed;
}

Yolov8CudaPreprocessResult Yolov8Inference::preprocess_cuda(const cv::cuda::GpuMat& image) const {
    Yolov8CudaPreprocessResult result;
    result.originalSize = image.size();
    result.inputSize    = m_inputSize;

    float scale = 1.0f;
    int   padX  = 0;
    int   padY  = 0;

    // 1. Letterbox (GPU)
    cv::cuda::GpuMat letterboxed = makeLetterboxedImageCuda(image, result.inputSize, scale, padX, padY);
    result.scale = scale;
    result.padX  = padX;
    result.padY  = padY;

    const int width  = letterboxed.cols;
    const int height = letterboxed.rows;

    // 2. BGR → RGB (GPU)
    cv::cuda::GpuMat rgb;
    cv::cuda::cvtColor(letterboxed, rgb, cv::COLOR_BGR2RGB);

    // 3. uint8 → float32, normalize /255 (GPU)
    cv::cuda::GpuMat rgbFloat;
    rgb.convertTo(rgbFloat, CV_32FC3, 1.0 / 255.0);

    // 4. HWC → CHW: split into 3 channels, then pack into contiguous CUDA buffer
    std::vector<cv::cuda::GpuMat> channels(3);
    cv::cuda::split(rgbFloat, channels);

    const size_t planeSz    = static_cast<size_t>(width) * static_cast<size_t>(height);
    const size_t totalBytes = planeSz * 3 * sizeof(float);

    float* rawPtr = nullptr;
    if (cudaMalloc(reinterpret_cast<void**>(&rawPtr), totalBytes) != cudaSuccess) {
        throw std::runtime_error("cudaMalloc failed in preprocess_cuda.");
    }

    result.gpuBuffer = rawPtr;

    // 각 채널을 순서대로 GPU 버퍼에 memcpy (device→device)
    for (int c = 0; c < 3; ++c) {
        // GpuMat::step 은 행 단위 바이트 크기 (padding 포함 가능) → cudaMemcpy2D 사용
        const cudaError_t err = cudaMemcpy2D(
            rawPtr + static_cast<ptrdiff_t>(c) * static_cast<ptrdiff_t>(planeSz),
            static_cast<size_t>(width) * sizeof(float),  // dst pitch (tight)
            channels[c].ptr<float>(),
            channels[c].step,                             // src pitch
            static_cast<size_t>(width) * sizeof(float),  // width in bytes
            static_cast<size_t>(height),
            cudaMemcpyDeviceToDevice
        );
        if (err != cudaSuccess) {
            throw std::runtime_error(std::string("cudaMemcpy2D failed: ") + cudaGetErrorString(err));
        }
    }

    // 5. Ort::Value – CUDA 메모리를 직접 가리키는 텐서 생성
    const std::vector<int64_t> shape{1, 3, static_cast<int64_t>(height), static_cast<int64_t>(width)};
    const Ort::MemoryInfo cudaMemInfo("Cuda", OrtDeviceAllocator, 0, OrtMemTypeDefault);

    result.tensors.emplace_back(Ort::Value::CreateTensor(
        cudaMemInfo,
        rawPtr,
        planeSz * 3 * sizeof(float),  // 원소 수가 아닌 바이트 수
        shape.data(),
        shape.size(),
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT
    ));

    return result;
}

std::vector<Yolov8Detection> Yolov8Inference::inference_cuda(const cv::cuda::GpuMat& image,
                                                              float confidenceThreshold,
                                                              float iouThreshold,
                                                              bool decodeOutput) const {
    if (!isLoaded()) {
        throw std::runtime_error("No YOLOv8 model is loaded.");
    }

    auto preprocessResult = preprocess_cuda(image);
    const auto outputs    = m_onnx.run_cuda(preprocessResult.tensors);
    return postprocess_cuda(outputs, preprocessResult, confidenceThreshold, iouThreshold, decodeOutput);
}

std::vector<Yolov8Detection> Yolov8Inference::postprocess_cuda(const std::vector<Ort::Value>& outputs,
                                                                 const Yolov8CudaPreprocessResult& cudaResult,
                                                                 float confidenceThreshold,
                                                                 float iouThreshold,
                                                                 bool decodeOutput) const {
    // ORT CUDA EP 는 출력 텐서를 기본적으로 CPU(host) 메모리에 기록한다.
    // 따라서 탐지 결과 디코딩은 CPU에서 수행하며, 이는 입력 전처리와 무관하다.

    // CPU postprocess 와 동일한 로직을 재사용하기 위해 메타데이터만 CPU 구조체로 변환
    Yolov8PreprocessResult cpuMeta;
    cpuMeta.originalSize = cudaResult.originalSize;
    cpuMeta.inputSize    = cudaResult.inputSize;
    cpuMeta.scale        = cudaResult.scale;
    cpuMeta.padX         = cudaResult.padX;
    cpuMeta.padY         = cudaResult.padY;
    // cpuMeta.tensors / tensorKeeper 는 postprocess 내부에서 사용되지 않음

    return postprocess(outputs, cpuMeta, confidenceThreshold, iouThreshold, decodeOutput);
}




