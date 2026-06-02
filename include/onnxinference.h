#pragma once

#include <string>
#include <vector>

#include <onnxruntime_cxx_api.h>

class OnnxInference {
public:
    enum class ExecutionProvider {
        CPU,
        CUDA
    };

    OnnxInference();
    ~OnnxInference();

    void load(std::string path);
    [[nodiscard]] bool isLoaded() const;
    std::vector<int> getInputSize() const;
    std::vector<int> getOutputnputSize() const;
    [[nodiscard]] const std::vector<std::vector<int64_t>>& getInputShapes() const;
    [[nodiscard]] const std::vector<std::vector<int64_t>>& getOutputShapes() const;
    [[nodiscard]] const std::vector<ONNXTensorElementDataType>& getInputTypes() const;
    std::vector<Ort::Value> run(const std::vector<Ort::Value>& inputTensors) const;
    std::vector<Ort::Value> run_cuda(const std::vector<Ort::Value>& inputTensors) const;
    bool test();
    void close();

    [[nodiscard]] bool isCudaActive() const;
    [[nodiscard]] std::string getExecutionProviderName() const;
    [[nodiscard]] std::string getLastLoadMessage() const;

private:
    void refreshIoMetadata();

    Ort::Env* m_env{nullptr};
    Ort::Session* m_session{nullptr};
    Ort::AllocatorWithDefaultOptions m_allocator;

    std::vector<std::string> m_inputNames;
    std::vector<std::string> m_outputNames;
    std::vector<const char*> m_inputNamePtrs;
    std::vector<const char*> m_outputNamePtrs;

    std::vector<std::vector<int64_t>> m_inputShapes;
    std::vector<std::vector<int64_t>> m_outputShapes;
    std::vector<ONNXTensorElementDataType> m_inputTypes;

    ExecutionProvider m_activeProvider{ExecutionProvider::CPU};
    std::string m_lastLoadMessage;
};

