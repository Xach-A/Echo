#include "ai/OnnxGraph.h"

#include <cstring>

namespace echo::ai {

namespace {

// Protocol buffers wire format.
class Proto {
public:
    std::vector<uint8_t> bytes;
    void varint(uint64_t v) {
        while (v >= 0x80) {
            bytes.push_back(static_cast<uint8_t>(v | 0x80));
            v >>= 7;
        }
        bytes.push_back(static_cast<uint8_t>(v));
    }
    void tag(int field, int wire) { varint(static_cast<uint64_t>(field) << 3 | static_cast<uint64_t>(wire)); }
    void int64(int field, int64_t v) {
        tag(field, 0);
        varint(static_cast<uint64_t>(v));
    }
    void fixed32(int field, float v) {
        tag(field, 5);
        uint32_t b;
        std::memcpy(&b, &v, 4);
        for (int k = 0; k < 4; ++k) bytes.push_back(static_cast<uint8_t>(b >> (8 * k)));
    }
    void blob(int field, const void* data, size_t n) {
        tag(field, 2);
        varint(n);
        const auto* p = static_cast<const uint8_t*>(data);
        bytes.insert(bytes.end(), p, p + n);
    }
    void str(int field, const std::string& s) { blob(field, s.data(), s.size()); }
    void msg(int field, const Proto& m) { blob(field, m.bytes.data(), m.bytes.size()); }
};

Proto typeProto(OnnxType type, const std::vector<int64_t>& dims) {
    Proto shape;
    for (int64_t d : dims) {
        Proto dim;
        dim.int64(1, d); // dim_value
        shape.msg(1, dim);
    }
    Proto tensor;
    tensor.int64(1, static_cast<int64_t>(type)); // elem_type
    tensor.msg(2, shape);
    Proto t;
    t.msg(1, tensor); // tensor_type
    return t;
}

} // namespace

OnnxAttr attrInt(const std::string& name, int64_t v) {
    OnnxAttr a;
    a.name = name;
    a.kind = OnnxAttr::Kind::Int;
    a.i = v;
    return a;
}
OnnxAttr attrInts(const std::string& name, std::vector<int64_t> v) {
    OnnxAttr a;
    a.name = name;
    a.kind = OnnxAttr::Kind::Ints;
    a.ints = std::move(v);
    return a;
}
OnnxAttr attrFloat(const std::string& name, float v) {
    OnnxAttr a;
    a.name = name;
    a.kind = OnnxAttr::Kind::Float;
    a.f = v;
    return a;
}
OnnxAttr attrString(const std::string& name, std::string v) {
    OnnxAttr a;
    a.name = name;
    a.kind = OnnxAttr::Kind::String;
    a.s = std::move(v);
    return a;
}

std::string OnnxGraph::fresh(const std::string& hint) { return hint + "_" + std::to_string(counter_++); }

std::string OnnxGraph::input(const std::string& name, OnnxType type, std::vector<int64_t> dims) {
    inputs_.push_back({name, type, std::move(dims)});
    return name;
}

void OnnxGraph::output(const std::string& value, const std::string& name, OnnxType type, std::vector<int64_t> dims) {
    if (value != name) {
        Node n;
        n.type = "Identity";
        n.name = fresh("out");
        n.inputs = {value};
        n.outputs = {name};
        nodes_.push_back(std::move(n));
    }
    outputs_.push_back({name, type, std::move(dims)});
}

std::string OnnxGraph::constant(std::vector<int64_t> dims, const std::vector<float>& data) {
    Tensor t;
    t.name = fresh("w");
    t.type = OnnxType::Float;
    t.dims = std::move(dims);
    t.raw.resize(data.size() * 4);
    std::memcpy(t.raw.data(), data.data(), t.raw.size());
    constants_.push_back(std::move(t));
    return constants_.back().name;
}

std::string OnnxGraph::constantInt64(std::vector<int64_t> dims, const std::vector<int64_t>& data) {
    Tensor t;
    t.name = fresh("i");
    t.type = OnnxType::Int64;
    t.dims = std::move(dims);
    t.raw.resize(data.size() * 8);
    std::memcpy(t.raw.data(), data.data(), t.raw.size());
    constants_.push_back(std::move(t));
    return constants_.back().name;
}

std::string OnnxGraph::op(const std::string& type, const std::vector<std::string>& inputs, const std::vector<OnnxAttr>& attrs) {
    return opN(type, inputs, 1, attrs)[0];
}

std::vector<std::string> OnnxGraph::opN(const std::string& type, const std::vector<std::string>& inputs, int outputs,
                                        const std::vector<OnnxAttr>& attrs) {
    Node n;
    n.type = type;
    n.name = fresh(type);
    n.inputs = inputs;
    for (int k = 0; k < outputs; ++k) n.outputs.push_back(n.name + "_o" + std::to_string(k));
    n.attrs = attrs;
    nodes_.push_back(n);
    return n.outputs;
}

std::vector<uint8_t> OnnxGraph::serialize(int opset) const {
    Proto graph;
    for (const Node& n : nodes_) {
        Proto node;
        for (const auto& i : n.inputs) node.str(1, i);
        for (const auto& o : n.outputs) node.str(2, o);
        node.str(3, n.name);
        node.str(4, n.type);
        for (const OnnxAttr& a : n.attrs) {
            Proto attr;
            attr.str(1, a.name);
            switch (a.kind) {
            case OnnxAttr::Kind::Int:
                attr.int64(3, a.i);
                attr.int64(20, 2);
                break;
            case OnnxAttr::Kind::Ints:
                for (int64_t v : a.ints) attr.int64(8, v);
                attr.int64(20, 7);
                break;
            case OnnxAttr::Kind::Float:
                attr.fixed32(2, a.f);
                attr.int64(20, 1);
                break;
            case OnnxAttr::Kind::Floats:
                for (float v : a.floats) attr.fixed32(7, v);
                attr.int64(20, 6);
                break;
            case OnnxAttr::Kind::String:
                attr.str(4, a.s);
                attr.int64(20, 3);
                break;
            }
            node.msg(5, attr);
        }
        graph.msg(1, node);
    }
    graph.str(2, "echo");
    for (const Tensor& t : constants_) {
        Proto tensor;
        for (int64_t d : t.dims) tensor.int64(1, d);
        tensor.int64(2, static_cast<int64_t>(t.type));
        tensor.str(8, t.name);
        tensor.blob(9, t.raw.data(), t.raw.size());
        graph.msg(5, tensor);
    }
    for (const ValueInfo& v : inputs_) {
        Proto vi;
        vi.str(1, v.name);
        vi.msg(2, typeProto(v.type, v.dims));
        graph.msg(11, vi);
    }
    for (const ValueInfo& v : outputs_) {
        Proto vi;
        vi.str(1, v.name);
        vi.msg(2, typeProto(v.type, v.dims));
        graph.msg(12, vi);
    }
    Proto model;
    model.int64(1, 8); // IR version 8 (opset 17)
    model.str(2, "Echo");
    Proto opsetId;
    opsetId.str(1, "");
    opsetId.int64(2, opset);
    model.msg(8, opsetId);
    model.msg(7, graph);
    return model.bytes;
}

} // namespace echo::ai
