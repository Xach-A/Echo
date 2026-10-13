// Builds ONNX models in memory (for ONNX Runtime): nodes, constant tensors
// and fixed-shape inputs/outputs, serialized as an ONNX ModelProto. Used to
// turn voice models from other formats (RVC checkpoints) into graphs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace echo::ai {

enum class OnnxType : int { Float = 1, Int32 = 6, Int64 = 7, Bool = 9 };

struct OnnxAttr {
    enum class Kind { Int, Ints, Float, Floats, String };
    std::string name;
    Kind kind = Kind::Int;
    int64_t i = 0;
    std::vector<int64_t> ints;
    float f = 0.0f;
    std::vector<float> floats;
    std::string s;
};
OnnxAttr attrInt(const std::string& name, int64_t v);
OnnxAttr attrInts(const std::string& name, std::vector<int64_t> v);
OnnxAttr attrFloat(const std::string& name, float v);
OnnxAttr attrString(const std::string& name, std::string v);

class OnnxGraph {
public:
    std::string input(const std::string& name, OnnxType type, std::vector<int64_t> dims);
    // Makes `value` a graph output under `name`.
    void output(const std::string& value, const std::string& name, OnnxType type, std::vector<int64_t> dims);

    std::string constant(std::vector<int64_t> dims, const std::vector<float>& data);
    std::string constantInt64(std::vector<int64_t> dims, const std::vector<int64_t>& data);
    std::string scalar(float v) { return constant({}, {v}); }
    std::string ints(const std::vector<int64_t>& v) { return constantInt64({static_cast<int64_t>(v.size())}, v); }

    // A node with one output; returns the output's name.
    std::string op(const std::string& type, const std::vector<std::string>& inputs, const std::vector<OnnxAttr>& attrs = {});
    // A node with several outputs.
    std::vector<std::string> opN(const std::string& type, const std::vector<std::string>& inputs, int outputs,
                                 const std::vector<OnnxAttr>& attrs = {});

    std::vector<uint8_t> serialize(int opset = 17) const;
    size_t nodeCount() const { return nodes_.size(); }

private:
    struct Node {
        std::string type, name;
        std::vector<std::string> inputs, outputs;
        std::vector<OnnxAttr> attrs;
    };
    struct Tensor {
        std::string name;
        OnnxType type;
        std::vector<int64_t> dims;
        std::vector<uint8_t> raw;
    };
    struct ValueInfo {
        std::string name;
        OnnxType type;
        std::vector<int64_t> dims;
    };
    std::string fresh(const std::string& hint);

    std::vector<Node> nodes_;
    std::vector<Tensor> constants_;
    std::vector<ValueInfo> inputs_, outputs_;
    int counter_ = 0;
};

} // namespace echo::ai
