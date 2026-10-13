// Reads PyTorch checkpoints (.pth / .bin written by torch.save): a zip with a
// pickled object tree (data.pkl) and one raw file per tensor storage.
//
// Safe by design: the pickle is interpreted, never executed. Only the few
// constructors a checkpoint uses are understood (OrderedDict, tensors and
// their storages); anything else becomes an inert placeholder.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace echo::ai {

struct PyValue;
using PyRef = std::shared_ptr<PyValue>;

struct TensorRef {
    std::string storageKey;   // file data/<key> in the zip
    std::string dtype;        // "float16", "bfloat16", "float32", "float64", "int64", "int32", "uint8"...
    int64_t offset = 0;       // in elements
    std::vector<int64_t> shape, strides;
};

struct PyValue {
    enum class Kind { None, Bool, Int, Float, Str, Bytes, List, Tuple, Dict, Global, Tensor, Storage, Object };
    Kind kind = Kind::None;
    int64_t i = 0;
    double f = 0.0;
    std::string s;                                // Str, Bytes; Global "module.name"; Storage dtype
    std::vector<PyRef> items;                     // List, Tuple
    std::vector<std::pair<PyRef, PyRef>> dict;    // Dict (insertion order)
    TensorRef tensor;                             // Tensor (and Storage: key)

    // Dict lookup by string key (nullptr if absent or not a dict).
    PyRef get(const std::string& key) const;
    bool isStr() const { return kind == Kind::Str; }
    bool isNumber() const { return kind == Kind::Int || kind == Kind::Float || kind == Kind::Bool; }
    double number() const { return kind == Kind::Float ? f : static_cast<double>(i); }
};

class TorchCheckpoint {
public:
    ~TorchCheckpoint();
    bool open(const std::wstring& path, std::string* error);
    const PyRef& root() const { return root_; }

    // The tensor's values as float32 (half, bfloat16, double and integer
    // storages are converted), in row-major order of its shape.
    bool read(const TensorRef& t, std::vector<float>& out, std::string* error) const;

    // Every tensor in a (state) dict, by its key.
    static std::map<std::string, TensorRef> tensors(const PyRef& dict);

private:
    struct Entry {
        uint64_t dataOffset = 0, size = 0;
    };
    bool readZip(std::string* error);
    bool readEntry(const Entry& e, std::vector<uint8_t>& out) const;
    bool unpickle(const std::vector<uint8_t>& data, std::string* error);

    void* file_ = nullptr;
    std::wstring path_;
    std::string prefix_; // "<archive name>/"
    std::map<std::string, Entry> entries_;
    PyRef root_;
};

} // namespace echo::ai
