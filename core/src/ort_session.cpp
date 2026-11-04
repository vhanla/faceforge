#include "internal.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace ff {

namespace {

Ort::SessionOptions make_session_options(const ff_options& opt, bool use_cuda) {
    Ort::SessionOptions so;
    so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    so.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    so.SetLogSeverityLevel(3);
    if (opt.intra_threads > 0) {
        so.SetIntraOpNumThreads(opt.intra_threads);
        so.SetInterOpNumThreads(1);
    }

    if (use_cuda) {
        std::unordered_map<std::string, std::string> cuda;
        cuda["device_id"] = std::to_string(opt.device_id);
        // Grow the arena in exactly the requested chunks: on a 4 GB card the
        // default next-power-of-two growth wastes hundreds of MiB.
        cuda["arena_extend_strategy"] = "kSameAsRequested";
        cuda["cudnn_conv_algo_search"] = "HEURISTIC";
        cuda["do_copy_in_default_stream"] = "1";
        cuda["enable_cuda_graph"] = "0";
        if (opt.gpu_mem_limit > 0) {
            cuda["gpu_mem_limit"] = std::to_string(opt.gpu_mem_limit);
        }
        Ort::CUDAProviderOptions cuda_options;
        cuda_options.Update(cuda);
        so.AppendExecutionProvider_CUDA_V2(*cuda_options);
    }
    return so;
}

std::string shape_to_string(const std::vector<int64_t>& shape) {
    std::string s = "[";
    for (size_t i = 0; i < shape.size(); ++i) {
        if (i) s += ",";
        s += std::to_string(shape[i]);
    }
    s += "]";
    return s;
}

}  // namespace

bool ModelSession::open(Ort::Env& env, const std::filesystem::path& path,
                        const ff_options& opt, bool use_cuda, std::string* error) {
    try {
        auto so = make_session_options(opt, use_cuda);
        // std::filesystem::path::value_type is wchar_t on Windows (matching
        // ORTCHAR_T) and char on POSIX, so this works on both without a
        // platform-specific conversion.
        session_ = std::make_unique<Ort::Session>(env, path.c_str(), so);

        Ort::AllocatorWithDefaultOptions alloc;
        const size_t ni = session_->GetInputCount();
        const size_t no = session_->GetOutputCount();
        input_names_.clear();
        output_names_.clear();
        input_shapes_.clear();
        output_shapes_.clear();
        for (size_t i = 0; i < ni; ++i) {
            auto name = session_->GetInputNameAllocated(i, alloc);
            input_names_.emplace_back(name.get());
            input_shapes_.push_back(
                session_->GetInputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape());
        }
        for (size_t i = 0; i < no; ++i) {
            auto name = session_->GetOutputNameAllocated(i, alloc);
            output_names_.emplace_back(name.get());
            output_shapes_.push_back(
                session_->GetOutputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape());
        }
        return true;
    } catch (const std::exception& ex) {
        if (error) *error = path.string() + ": " + ex.what();
        session_.reset();
        return false;
    }
}

std::vector<Ort::Value> ModelSession::run(std::vector<Ort::Value>& inputs) {
    std::vector<const char*> in_names;
    std::vector<const char*> out_names;
    in_names.reserve(input_names_.size());
    out_names.reserve(output_names_.size());
    for (auto& n : input_names_) in_names.push_back(n.c_str());
    for (auto& n : output_names_) out_names.push_back(n.c_str());
    return session_->Run(Ort::RunOptions{nullptr}, in_names.data(), inputs.data(),
                         inputs.size(), out_names.data(), out_names.size());
}

ModelSession* Engine::session(const std::string& key, const std::string& filename) {
    auto it = sessions.find(key);
    if (it != sessions.end()) return it->second.get();

    // u8path interprets the UTF-8 string correctly on Windows (where narrow
    // strings are ANSI) and stays a no-op on POSIX.
    const std::filesystem::path path = std::filesystem::u8path(model_dir + "/" + filename);
    if (!std::filesystem::exists(path)) {
        set_error("missing model: " + path.string());
        return nullptr;
    }

    auto ms = std::make_unique<ModelSession>();
    std::string err;
    const bool want_cuda = using_cuda;
    if (!ms->open(*env, path, opt, want_cuda, &err)) {
        // In AUTO mode a broken CUDA install must not make the app unusable.
        if (want_cuda && opt.provider == FF_PROVIDER_AUTO) {
            ms = std::make_unique<ModelSession>();
            if (!ms->open(*env, path, opt, false, &err)) {
                set_error(err);
                return nullptr;
            }
            using_cuda = false;
            provider_name = "cpu";
        } else {
            set_error(err);
            return nullptr;
        }
    }
    ModelSession* raw = ms.get();
    sessions[key] = std::move(ms);
    return raw;
}

void Engine::unload(const std::string& key) {
    sessions.erase(key);
}

void Engine::unload_all() {
    sessions.clear();
}

}  // namespace ff
