#include "onnxinference.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <limits>
#include <numeric>
#include <string>
#include <stdexcept>
#include <vector>

namespace {

Ort::SessionOptions makeBaseSessionOptions() {
    Ort::SessionOptions options;
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
    options.SetIntraOpNumThreads(0);
    return options;
}

std::vector<int> toIntShape(const std::vector<int64_t>& shape) {
    std::vector<int> result;
    result.reserve(shape.size());

    for (const int64_t dim : shape) {
        if (dim > static_cast<int64_t>(std::numeric_limits<int>::max())) {
            result.push_back(std::numeric_limits<int>::max());
            continue;
        }
        if (dim < static_cast<int64_t>(std::numeric_limits<int>::min())) {
            result.push_back(std::numeric_limits<int>::min());
            continue;
        }
        result.push_back(static_cast<int>(dim));
    }

    return result;
}

std::vector<int64_t> normalizeDynamicShape(std::vector<int64_t> shape) {
    if (shape.empty()) {
        shape.push_back(1);
        return shape;
    }

    for (int64_t& dim : shape) {
        if (dim <= 0) {
            dim = 1;
        }
    }

    return shape;
}

size_t computeElementCount(const std::vector<int64_t>& shape) {
    return std::accumulate(
        shape.begin(),
        shape.end(),
        static_cast<size_t>(1),
        [](const size_t current, const int64_t dim) {
            const size_t safeDim = static_cast<size_t>(std::max<int64_t>(1, dim));
            return current * safeDim;
        }
    );
}

template <typename T>
Ort::Value makeZeroTensor(const Ort::MemoryInfo& memoryInfo,
                          const std::vector<int64_t>& shape,
                          std::vector<std::shared_ptr<void>>& bufferKeepers) {
    const size_t elementCount = computeElementCount(shape);

    auto buffer = std::shared_ptr<T>(new T[elementCount](), std::default_delete<T[]>());
    bufferKeepers.emplace_back(std::static_pointer_cast<void>(buffer));

    return Ort::Value::CreateTensor<T>(
        memoryInfo,
        buffer.get(),
        elementCount,
        shape.data(),
        shape.size()
    );
}

} // namespace

OnnxInference::OnnxInference()
{
    m_env = new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "OnnxInference");
    m_lastLoadMessage = "No model loaded.";
}

OnnxInference::~OnnxInference() {
    close();
    delete m_env;
    m_env = nullptr;
}

void OnnxInference::load(std::string path) {
    close();

    const auto createSession = [this, &path](Ort::SessionOptions& options) {
#ifdef _WIN32
        const std::wstring widePath(path.begin(), path.end());
        return new Ort::Session(*m_env, widePath.c_str(), options);
#else
        return new Ort::Session(*m_env, path.c_str(), options);
#endif
    };

    try {
        Ort::SessionOptions cudaOptions = makeBaseSessionOptions();
        OrtStatus* status = OrtSessionOptionsAppendExecutionProvider_CUDA(cudaOptions, 0);
        if (status != nullptr) {
            const OrtApi& api = Ort::GetApi();
            const std::string errorMessage = api.GetErrorMessage(status);
            api.ReleaseStatus(status);
            throw std::runtime_error(errorMessage);
        }

        delete m_session;
        m_session = createSession(cudaOptions);
        m_activeProvider = ExecutionProvider::CUDA;
        m_lastLoadMessage = "Loaded with CUDA execution provider.";
    } catch (const std::exception& cudaError) {
        Ort::SessionOptions cpuOptions = makeBaseSessionOptions();
        delete m_session;
        m_session = createSession(cpuOptions);
        m_activeProvider = ExecutionProvider::CPU;
        m_lastLoadMessage = std::string("CUDA unavailable, CPU fallback used: ") + cudaError.what();
    }

    refreshIoMetadata();
}

bool OnnxInference::isLoaded() const {
    return static_cast<bool>(m_session);
}

std::vector<int> OnnxInference::getInputSize() const {
    if (m_inputShapes.empty()) {
        return {};
    }

    return toIntShape(m_inputShapes.front());
}

std::vector<int> OnnxInference::getOutputnputSize() const {
    if (m_outputShapes.empty()) {
        return {};
    }

    return toIntShape(m_outputShapes.front());
}

const std::vector<std::vector<int64_t>>& OnnxInference::getInputShapes() const {
    return m_inputShapes;
}

const std::vector<std::vector<int64_t>>& OnnxInference::getOutputShapes() const {
    return m_outputShapes;
}

const std::vector<ONNXTensorElementDataType>& OnnxInference::getInputTypes() const {
    return m_inputTypes;
}

std::vector<Ort::Value> OnnxInference::run(const std::vector<Ort::Value>& inputTensors) const {
    if (!m_session) {
        throw std::runtime_error("No ONNX model is loaded.");
    }

    if (inputTensors.size() != m_inputNamePtrs.size()) {
        throw std::runtime_error("Input tensor count does not match model inputs.");
    }

    return m_session->Run(
        Ort::RunOptions{nullptr},
        m_inputNamePtrs.data(),
        inputTensors.data(),
        inputTensors.size(),
        m_outputNamePtrs.data(),
        m_outputNamePtrs.size()
    );
}

std::vector<Ort::Value> OnnxInference::run_cuda(const std::vector<Ort::Value>& inputTensors) const {
    if (!isCudaActive()) {
        throw std::runtime_error("CUDA execution provider is not active. Cannot run with CUDA tensors.");
    }
    return run(inputTensors);
}

bool OnnxInference::test() {
    if (!m_session || m_inputNamePtrs.size() != m_inputShapes.size() || m_inputShapes.size() != m_inputTypes.size()) {
        return false;
    }

    const Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    std::vector<Ort::Value> inputTensors;
    inputTensors.reserve(m_inputShapes.size());

    // Run() 호출 시점까지 입력 버퍼가 살아있도록 별도 보관한다.
    std::vector<std::shared_ptr<void>> bufferKeepers;
    bufferKeepers.reserve(m_inputShapes.size());

    try {
        for (size_t index = 0; index < m_inputShapes.size(); ++index) {
            const std::vector<int64_t> normalizedShape = normalizeDynamicShape(m_inputShapes[index]);

            switch (m_inputTypes[index]) {
                case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
                    inputTensors.emplace_back(makeZeroTensor<float>(memoryInfo, normalizedShape, bufferKeepers));
                    break;
                case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE:
                    inputTensors.emplace_back(makeZeroTensor<double>(memoryInfo, normalizedShape, bufferKeepers));
                    break;
                case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:
                    inputTensors.emplace_back(makeZeroTensor<int8_t>(memoryInfo, normalizedShape, bufferKeepers));
                    break;
                case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:
                    inputTensors.emplace_back(makeZeroTensor<uint8_t>(memoryInfo, normalizedShape, bufferKeepers));
                    break;
                case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16:
                    inputTensors.emplace_back(makeZeroTensor<int16_t>(memoryInfo, normalizedShape, bufferKeepers));
                    break;
                case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16:
                    inputTensors.emplace_back(makeZeroTensor<uint16_t>(memoryInfo, normalizedShape, bufferKeepers));
                    break;
                case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:
                    inputTensors.emplace_back(makeZeroTensor<int32_t>(memoryInfo, normalizedShape, bufferKeepers));
                    break;
                case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT32:
                    inputTensors.emplace_back(makeZeroTensor<uint32_t>(memoryInfo, normalizedShape, bufferKeepers));
                    break;
                case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:
                    inputTensors.emplace_back(makeZeroTensor<int64_t>(memoryInfo, normalizedShape, bufferKeepers));
                    break;
                case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT64:
                    inputTensors.emplace_back(makeZeroTensor<uint64_t>(memoryInfo, normalizedShape, bufferKeepers));
                    break;
                case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL:
                    inputTensors.emplace_back(makeZeroTensor<bool>(memoryInfo, normalizedShape, bufferKeepers));
                    break;
                case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16:
                case ONNX_TENSOR_ELEMENT_DATA_TYPE_BFLOAT16:
                    inputTensors.emplace_back(makeZeroTensor<uint16_t>(memoryInfo, normalizedShape, bufferKeepers));
                    break;
                default:
                    return false;
            }
        }

        auto outputTensors = run(inputTensors);

        return !outputTensors.empty() || m_outputNamePtrs.empty();
    } catch (...) {
        return false;
    }
}

void OnnxInference::close() {
    delete m_session;
    m_session = nullptr;
    m_activeProvider = ExecutionProvider::CPU;

    m_inputNames.clear();
    m_outputNames.clear();
    m_inputNamePtrs.clear();
    m_outputNamePtrs.clear();

    m_inputShapes.clear();
    m_outputShapes.clear();
    m_inputTypes.clear();
}

bool OnnxInference::isCudaActive() const {
    return m_activeProvider == ExecutionProvider::CUDA;
}

std::string OnnxInference::getExecutionProviderName() const {
    return isCudaActive() ? "CUDA" : "CPU";
}

std::string OnnxInference::getLastLoadMessage() const {
    return m_lastLoadMessage;
}

void OnnxInference::refreshIoMetadata() {
    if (!m_session) {
        return;
    }

    m_inputNames.clear();
    m_outputNames.clear();
    m_inputNamePtrs.clear();
    m_outputNamePtrs.clear();
    m_inputShapes.clear();
    m_outputShapes.clear();
    m_inputTypes.clear();

    const size_t inputCount = m_session->GetInputCount();
    m_inputNames.reserve(inputCount);
    m_inputNamePtrs.reserve(inputCount);
    m_inputShapes.reserve(inputCount);
    m_inputTypes.reserve(inputCount);

    for (size_t index = 0; index < inputCount; ++index) {
        auto inputName = m_session->GetInputNameAllocated(index, m_allocator);
        m_inputNames.emplace_back(inputName.get());
        m_inputNamePtrs.emplace_back(m_inputNames.back().c_str());

        const Ort::TypeInfo typeInfo = m_session->GetInputTypeInfo(index);
        const auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();

        m_inputShapes.emplace_back(tensorInfo.GetShape());
        m_inputTypes.emplace_back(tensorInfo.GetElementType());
    }

    const size_t outputCount = m_session->GetOutputCount();
    m_outputNames.reserve(outputCount);
    m_outputNamePtrs.reserve(outputCount);
    m_outputShapes.reserve(outputCount);

    for (size_t index = 0; index < outputCount; ++index) {
        auto outputName = m_session->GetOutputNameAllocated(index, m_allocator);
        m_outputNames.emplace_back(outputName.get());
        m_outputNamePtrs.emplace_back(m_outputNames.back().c_str());

        const Ort::TypeInfo typeInfo = m_session->GetOutputTypeInfo(index);
        const auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();

        m_outputShapes.emplace_back(tensorInfo.GetShape());
    }
}
