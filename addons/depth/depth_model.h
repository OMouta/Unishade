#pragma once

#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;
struct IDMLDevice;
struct OrtDmlApi;
#endif
struct OrtApi;
struct OrtEnv;
struct OrtSessionOptions;
struct OrtSession;
struct OrtMemoryInfo;
struct OrtValue;

// Depth Anything V2 through ONNX Runtime, on DirectML on Windows and on Core ML on macOS. ONNX Runtime's library is
// loaded at runtime from the given directory, so the host still runs when the depth files are not installed.
class DepthModel
{
public:
    // ONNX Runtime's paths: wide on Windows, UTF-8 elsewhere.
#ifdef _WIN32
    using Path = std::wstring;
#else
    using Path = std::string;
#endif

    ~DepthModel();

#ifdef _WIN32
    // Builds a session for one input size, since DirectML needs fixed shapes to run fast. device must be
    // the native D3D12 device rather than ReShade's proxy, which DirectML cannot run on. width and height
    // must be multiples of 14.
    // Throws std::runtime_error with a printable reason.
    void Load(const Path& directory, const Path& modelFile, int width, int height, ID3D12Device* device);
#else
    // Builds a session for one input size, since Core ML compiles the model for fixed shapes, which takes seconds. The
    // compiled model is kept in cache, a directory for this size only, so the next session for it starts sooner. width
    // and height must be multiples of 14.
    // Throws std::runtime_error with a printable reason.
    void Load(const Path& directory, const Path& modelFile, int width, int height, const Path& cache);
#endif

    // Whether the model takes and returns float16 rather than float32.
    bool HalfInput() const { return halfInput; }
    bool HalfOutput() const { return halfOutput; }

#ifdef _WIN32
    // Where the model runs. Work for its input and output goes on the same queue.
    ID3D12CommandQueue* Queue() const { return queue; }

    // The buffers Run reads and writes from now on, on the device given to Load. input holds planar RGB,
    // ImageNet-normalized, 3 * width * height values. output receives width * height values of relative
    // inverse depth, larger being closer. Only call while the GPU is not running the model.
    void Bind(ID3D12Resource* input, ID3D12Resource* output);
#else
    // The memory Run reads and writes from now on, holding what Bind on Windows says, in memory the processor can
    // reach. Only call while Run is not running.
    void Bind(void* input, void* output);
#endif

    // Windows: puts the model on the queue behind what is already there, and returns before the GPU is done. macOS:
    // returns once the output is written.
    void Run();

private:
    void Unbind();

    void* library = nullptr;
#ifdef _WIN32
    void* dmlLibrary = nullptr;
    IDMLDevice* dml = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    const OrtDmlApi* dmlApi = nullptr;
#endif
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    OrtSessionOptions* options = nullptr;
    OrtSession* session = nullptr;
    OrtMemoryInfo* memory = nullptr;
    std::string inputName;
    std::string outputName;
    bool halfInput = false;
    bool halfOutput = false;
    std::vector<int64_t> inputShape;
    std::vector<int64_t> outputShape;
#ifdef _WIN32
    // Held until other buffers are bound, or the model is gone.
    ID3D12Resource* inputBuffer = nullptr;
    ID3D12Resource* outputBuffer = nullptr;
    void* inputAllocation = nullptr;
    void* outputAllocation = nullptr;
#endif
    OrtValue* inputValue = nullptr;
    OrtValue* outputValue = nullptr;
};
