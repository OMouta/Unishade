#include "depth_model.h"

#ifdef _WIN32
#include <windows.h>
#include <d3d12.h>
#include <onnxruntime_c_api.h>
#include <dml_provider_factory.h>
#else
#include <dlfcn.h>
#include <onnxruntime_c_api.h>
#endif

#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace
{
struct OrtError
{
    const OrtApi* api;
    void operator()(OrtStatus* status) const
    {
        if (!status)
            return;
        std::string message = api->GetErrorMessage(status);
        api->ReleaseStatus(status);
        throw std::runtime_error(message);
    }
};

struct TensorInfo
{
    ONNXTensorElementDataType type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    size_t rank = 0;
};

// Takes the type info, which it releases.
TensorInfo ReadTensorInfo(const OrtApi* api, OrtTypeInfo* info)
{
    TensorInfo tensor;
    const OrtTensorTypeAndShapeInfo* shape = nullptr;
    OrtStatus* status = api->CastTypeInfoToTensorInfo(info, &shape);
    if (!status)
        status = api->GetTensorElementType(shape, &tensor.type);
    if (!status)
        status = api->GetDimensionsCount(shape, &tensor.rank);
    api->ReleaseTypeInfo(info);
    OrtError{ api }(status);
    return tensor;
}

bool IsHalf(ONNXTensorElementDataType type, const char* what)
{
    if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16)
        return true;
    if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
        return false;
    throw std::runtime_error(std::string("The model ") + what + " is neither float32 nor float16.");
}

// A tensor of the given shape over memory the model reads or writes.
OrtValue* CreateTensor(const OrtApi* api, const OrtMemoryInfo* memory, void* data, const std::vector<int64_t>& shape, bool half)
{
    size_t count = 1;
    for (int64_t dimension : shape)
        count *= static_cast<size_t>(dimension);
    OrtValue* tensor = nullptr;
    OrtError{ api }(api->CreateTensorWithDataAsOrtValue(memory, data, count * (half ? 2 : 4), shape.data(), shape.size(),
                                                         half ? ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16 : ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &tensor));
    return tensor;
}
} // namespace

DepthModel::~DepthModel()
{
    if (api)
    {
        // On Windows, waits for the GPU to finish the model, which may still use the buffers.
        if (session)
            api->ReleaseSession(session);
        Unbind();
        if (options)
            api->ReleaseSessionOptions(options);
        if (memory)
            api->ReleaseMemoryInfo(memory);
        if (env)
            api->ReleaseEnv(env);
    }
#ifdef _WIN32
    if (queue)
        queue->Release();
    if (dml)
        dml->Release();
    if (library)
        FreeLibrary(static_cast<HMODULE>(library));
    if (dmlLibrary)
        FreeLibrary(static_cast<HMODULE>(dmlLibrary));
#else
    if (library)
        dlclose(library);
#endif
}

#ifdef _WIN32
void DepthModel::Load(const Path& directory, const Path& modelFile, int width, int height, ID3D12Device* device)
#else
void DepthModel::Load(const Path& directory, const Path& modelFile, int width, int height, const Path& cache)
#endif
{
    using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();
#ifdef _WIN32
    // DirectML.dll is an import of onnxruntime.dll. Loaded first, it is the one onnxruntime.dll gets, rather than the
    // copy in System32, which is not the version this ONNX Runtime was built with.
    dmlLibrary = LoadLibraryW((directory + L"DirectML.dll").c_str());
    library = LoadLibraryExW((directory + L"onnxruntime.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!library || !dmlLibrary)
        throw std::runtime_error("onnxruntime.dll or DirectML.dll could not be loaded.");

    auto getApiBase = reinterpret_cast<GetApiBase>(GetProcAddress(static_cast<HMODULE>(library), "OrtGetApiBase"));
    using CreateDml = HRESULT(WINAPI*)(ID3D12Device*, DML_CREATE_DEVICE_FLAGS, REFIID, void**);
    auto createDml = reinterpret_cast<CreateDml>(GetProcAddress(static_cast<HMODULE>(dmlLibrary), "DMLCreateDevice"));
    if (!getApiBase || !createDml)
        throw std::runtime_error("onnxruntime.dll or DirectML.dll is not the expected build.");
#else
    library = dlopen((directory + "libonnxruntime.dylib").c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library)
    {
        const char* reason = dlerror();
        throw std::runtime_error(std::string("libonnxruntime.dylib could not be loaded: ") + (reason ? reason : "no reason given."));
    }
    auto getApiBase = reinterpret_cast<GetApiBase>(dlsym(library, "OrtGetApiBase"));
    if (!getApiBase)
        throw std::runtime_error("libonnxruntime.dylib is not ONNX Runtime.");
#endif
    api = getApiBase()->GetApi(ORT_API_VERSION);
    if (!api)
        throw std::runtime_error("ONNX Runtime is older than the version this add-on was built for.");
    const OrtError check{ api };
#ifdef _WIN32
    check(api->GetExecutionProviderApi("DML", ORT_API_VERSION, reinterpret_cast<const void**>(&dmlApi)));

    // Letting ONNX Runtime pick the GPU would have it call D3D12CreateDevice and get ReShade's proxy,
    // on which DirectML fails while uploading its fused graph. Build the DirectML device and queue on
    // the given device instead.
    if (FAILED(createDml(device, DML_CREATE_DEVICE_FLAG_NONE, IID_PPV_ARGS(&dml))))
        throw std::runtime_error("DirectML could not use this GPU.");
    const D3D12_COMMAND_QUEUE_DESC queueDesc{ D3D12_COMMAND_LIST_TYPE_DIRECT };
    if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))))
        throw std::runtime_error("The GPU command queue could not be created.");
#endif

    check(api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "Unishade", &env));
    check(api->CreateSessionOptions(&options));
#ifdef _WIN32
    // DirectML requires sequential execution without memory patterns.
    check(api->SetSessionExecutionMode(options, ORT_SEQUENTIAL));
    check(api->DisableMemPattern(options));
#endif
    check(api->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL));
    // Otherwise idle worker threads spin and take CPU time from the game.
    check(api->AddSessionConfigEntry(options, "session.intra_op.allow_spinning", "0"));
    // DirectML and Core ML compile the graph for fixed shapes. Left dynamic, the model runs many times slower.
    check(api->AddFreeDimensionOverrideByName(options, "batch_size", 1));
    check(api->AddFreeDimensionOverrideByName(options, "height", height));
    check(api->AddFreeDimensionOverrideByName(options, "width", width));
#ifdef _WIN32
    check(dmlApi->SessionOptionsAppendExecutionProvider_DML1(options, dml, queue));
    check(api->CreateSession(env, (directory + modelFile).c_str(), options, &session));
    check(api->CreateMemoryInfo("DML", OrtDeviceAllocator, 0, OrtMemTypeDefault, &memory));
#else
    // ML Program is the format for macOS 12 and later. The older NeuralNetwork format has no LayerNormalization, Erf or
    // ReduceMean, which the model uses, so those would run on the processor. ALL lets Core ML put each part on the
    // Neural Engine, the graphics card or the processor. Core ML compiles the model when the session is made, so the
    // compiled model is kept in cache. RequireStaticInputShapes stays off: the overrides above already fix the inputs,
    // and turning it on would only leave nodes whose shapes ONNX Runtime cannot infer to the processor.
    const char* keys[] = { "ModelFormat", "MLComputeUnits", "ModelCacheDirectory" };
    const char* values[] = { "MLProgram", "ALL", cache.c_str() };
    check(api->SessionOptionsAppendExecutionProvider(options, "CoreML", keys, values, std::size(keys)));
    check(api->CreateSession(env, (directory + modelFile).c_str(), options, &session));
    check(api->CreateCpuMemoryInfo(OrtDeviceAllocator, OrtMemTypeDefault, &memory));
#endif

    OrtAllocator* allocator = nullptr;
    check(api->GetAllocatorWithDefaultOptions(&allocator));
    size_t inputs = 0, outputs = 0;
    check(api->SessionGetInputCount(session, &inputs));
    check(api->SessionGetOutputCount(session, &outputs));
    if (inputs != 1 || outputs != 1)
        throw std::runtime_error("The model does not take one image and return one depth map.");
    char* name = nullptr;
    check(api->SessionGetInputName(session, 0, allocator, &name));
    inputName = name;
    check(api->AllocatorFree(allocator, name));
    check(api->SessionGetOutputName(session, 0, allocator, &name));
    outputName = name;
    check(api->AllocatorFree(allocator, name));

    OrtTypeInfo* info = nullptr;
    check(api->SessionGetInputTypeInfo(session, 0, &info));
    halfInput = IsHalf(ReadTensorInfo(api, info).type, "input");
    check(api->SessionGetOutputTypeInfo(session, 0, &info));
    const TensorInfo output = ReadTensorInfo(api, info);
    halfOutput = IsHalf(output.type, "output");
    // A depth map with or without a channel dimension.
    inputShape = { 1, 3, height, width };
    if (output.rank == 3)
        outputShape = { 1, height, width };
    else if (output.rank == 4)
        outputShape = { 1, 1, height, width };
    else
        throw std::runtime_error("The model returns a depth map of an unexpected shape.");
}

void DepthModel::Unbind()
{
    for (OrtValue** value : { &inputValue, &outputValue })
        if (*value)
            api->ReleaseValue(std::exchange(*value, nullptr));
#ifdef _WIN32
    for (void** allocation : { &inputAllocation, &outputAllocation })
        if (*allocation)
            dmlApi->FreeGPUAllocation(std::exchange(*allocation, nullptr));
    for (ID3D12Resource** buffer : { &inputBuffer, &outputBuffer })
        if (*buffer)
            std::exchange(*buffer, nullptr)->Release();
#endif
}

#ifdef _WIN32
void DepthModel::Bind(ID3D12Resource* input, ID3D12Resource* output)
{
    Unbind();
    inputBuffer = input;
    outputBuffer = output;
    input->AddRef();
    output->AddRef();
    const OrtError check{ api };
    check(dmlApi->CreateGPUAllocationFromD3DResource(input, &inputAllocation));
    inputValue = CreateTensor(api, memory, inputAllocation, inputShape, halfInput);
    check(dmlApi->CreateGPUAllocationFromD3DResource(output, &outputAllocation));
    outputValue = CreateTensor(api, memory, outputAllocation, outputShape, halfOutput);
}
#else
void DepthModel::Bind(void* input, void* output)
{
    Unbind();
    inputValue = CreateTensor(api, memory, input, inputShape, halfInput);
    outputValue = CreateTensor(api, memory, output, outputShape, halfOutput);
}
#endif

void DepthModel::Run()
{
    if (!inputValue || !outputValue)
        throw std::logic_error("The depth model has no buffers to run on.");
    const char* inNames[] = { inputName.c_str() };
    const char* outNames[] = { outputName.c_str() };
    OrtError{ api }(api->Run(session, nullptr, inNames, &inputValue, 1, outNames, 1, &outputValue));
}
