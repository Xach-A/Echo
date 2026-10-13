#include "ai/TorchFile.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

namespace echo::ai {

namespace {

uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t u32(const uint8_t* p) { return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24)); }
uint64_t u64(const uint8_t* p) { return u32(p) | (static_cast<uint64_t>(u32(p + 4)) << 32); }

PyRef make(PyValue::Kind k) {
    auto v = std::make_shared<PyValue>();
    v->kind = k;
    return v;
}
PyRef makeInt(int64_t i) {
    auto v = make(PyValue::Kind::Int);
    v->i = i;
    return v;
}
PyRef makeStr(std::string s, PyValue::Kind k = PyValue::Kind::Str) {
    auto v = make(k);
    v->s = std::move(s);
    return v;
}

float halfToFloat(uint16_t h) {
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000) << 16;
    uint32_t exp = (h >> 10) & 0x1F, mant = h & 0x3FF, bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else { // subnormal
            exp = 127 - 15 + 1;
            while (!(mant & 0x400)) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x3FF;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

int dtypeSize(const std::string& d) {
    if (d == "float16" || d == "bfloat16" || d == "int16") return 2;
    if (d == "float32" || d == "int32") return 4;
    if (d == "float64" || d == "int64") return 8;
    if (d == "uint8" || d == "int8" || d == "bool") return 1;
    return 0;
}

std::string storageDtype(const std::string& global) {
    static const std::pair<const char*, const char*> map[] = {
        {"torch.HalfStorage", "float16"},  {"torch.BFloat16Storage", "bfloat16"}, {"torch.FloatStorage", "float32"},
        {"torch.DoubleStorage", "float64"}, {"torch.LongStorage", "int64"},        {"torch.IntStorage", "int32"},
        {"torch.ShortStorage", "int16"},   {"torch.CharStorage", "int8"},          {"torch.ByteStorage", "uint8"},
        {"torch.BoolStorage", "bool"},
    };
    for (const auto& [g, d] : map)
        if (global == g) return d;
    return "";
}

} // namespace

PyRef PyValue::get(const std::string& key) const {
    if (kind != Kind::Dict) return nullptr;
    for (const auto& [k, v] : dict)
        if (k && k->kind == Kind::Str && k->s == key) return v;
    return nullptr;
}

TorchCheckpoint::~TorchCheckpoint() {
    if (file_) std::fclose(static_cast<FILE*>(file_));
}

bool TorchCheckpoint::open(const std::wstring& path, std::string* error) {
    path_ = path;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        if (error) *error = "Can't open the file";
        return false;
    }
    file_ = f;
    if (!readZip(error)) return false;
    auto pkl = entries_.find(prefix_ + "data.pkl");
    std::vector<uint8_t> data;
    if (pkl == entries_.end() || !readEntry(pkl->second, data)) {
        if (error) *error = "Not a PyTorch checkpoint (data.pkl is missing)";
        return false;
    }
    return unpickle(data, error);
}

bool TorchCheckpoint::readZip(std::string* error) {
    FILE* f = static_cast<FILE*>(file_);
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    _fseeki64(f, 0, SEEK_END);
    const int64_t size = _ftelli64(f);
    if (size < 22) return fail("Not a PyTorch checkpoint (too small)");
    // End of central directory: the last 22 bytes plus up to 64 KiB of comment.
    const int64_t tailLen = std::min<int64_t>(size, 22 + 65535 + 20);
    std::vector<uint8_t> tail(static_cast<size_t>(tailLen));
    _fseeki64(f, size - tailLen, SEEK_SET);
    if (std::fread(tail.data(), 1, tail.size(), f) != tail.size()) return fail("Can't read the file");
    int64_t eocd = -1;
    for (int64_t i = tailLen - 22; i >= 0; --i)
        if (u32(&tail[i]) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd < 0) return fail("Not a PyTorch checkpoint (not a zip archive)");
    uint64_t count = u16(&tail[eocd + 10]), cdSize = u32(&tail[eocd + 12]), cdOffset = u32(&tail[eocd + 16]);
    if (count == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOffset == 0xFFFFFFFFu) {
        // ZIP64: the locator sits right before the end record.
        if (eocd < 20 || u32(&tail[eocd - 20]) != 0x07064b50) return fail("Unsupported zip archive");
        const uint64_t recOffset = u64(&tail[eocd - 20 + 8]);
        uint8_t rec[56];
        _fseeki64(f, static_cast<int64_t>(recOffset), SEEK_SET);
        if (std::fread(rec, 1, sizeof(rec), f) != sizeof(rec) || u32(rec) != 0x06064b50) return fail("Unsupported zip archive");
        count = u64(rec + 32);
        cdSize = u64(rec + 40);
        cdOffset = u64(rec + 48);
    }
    if (cdOffset + cdSize > static_cast<uint64_t>(size) || cdSize > (1u << 30)) return fail("Damaged zip archive");
    std::vector<uint8_t> cd(static_cast<size_t>(cdSize));
    _fseeki64(f, static_cast<int64_t>(cdOffset), SEEK_SET);
    if (std::fread(cd.data(), 1, cd.size(), f) != cd.size()) return fail("Can't read the file");
    size_t p = 0;
    for (uint64_t n = 0; n < count; ++n) {
        if (p + 46 > cd.size() || u32(&cd[p]) != 0x02014b50) return fail("Damaged zip archive");
        const uint16_t method = u16(&cd[p + 10]);
        uint64_t comp = u32(&cd[p + 20]), uncomp = u32(&cd[p + 24]), local = u32(&cd[p + 42]);
        const uint16_t nameLen = u16(&cd[p + 28]), extraLen = u16(&cd[p + 30]), commentLen = u16(&cd[p + 32]);
        if (p + 46 + nameLen + extraLen > cd.size()) return fail("Damaged zip archive");
        std::string name(reinterpret_cast<const char*>(&cd[p + 46]), nameLen);
        // ZIP64 sizes and offset, in that order, for the fields that overflowed.
        for (size_t e = p + 46 + nameLen; e + 4 <= p + 46 + nameLen + extraLen;) {
            const uint16_t id = u16(&cd[e]), len = u16(&cd[e + 2]);
            if (id == 0x0001) {
                size_t q = e + 4;
                if (uncomp == 0xFFFFFFFFu && q + 8 <= e + 4 + len) uncomp = u64(&cd[q]), q += 8;
                if (comp == 0xFFFFFFFFu && q + 8 <= e + 4 + len) comp = u64(&cd[q]), q += 8;
                if (local == 0xFFFFFFFFu && q + 8 <= e + 4 + len) local = u64(&cd[q]);
            }
            e += 4 + len;
        }
        if (method != 0 && name.find("/data/") != std::string::npos)
            return fail("Compressed checkpoints are not supported (re-save it with torch.save)");
        uint8_t lh[30];
        _fseeki64(f, static_cast<int64_t>(local), SEEK_SET);
        if (std::fread(lh, 1, sizeof(lh), f) != sizeof(lh) || u32(lh) != 0x04034b50) return fail("Damaged zip archive");
        Entry en;
        en.dataOffset = local + 30 + u16(lh + 26) + u16(lh + 28);
        en.size = method == 0 ? uncomp : comp;
        if (en.dataOffset + en.size > static_cast<uint64_t>(size)) return fail("Damaged zip archive");
        if (name.size() >= 9 && name.compare(name.size() - 9, 9, "/data.pkl") == 0) prefix_ = name.substr(0, name.size() - 8);
        entries_[name] = en;
        p += 46 + nameLen + extraLen + commentLen;
    }
    if (prefix_.empty()) return fail("Not a PyTorch checkpoint (data.pkl is missing)");
    return true;
}

bool TorchCheckpoint::readEntry(const Entry& e, std::vector<uint8_t>& out) const {
    FILE* f = static_cast<FILE*>(file_);
    out.resize(static_cast<size_t>(e.size));
    _fseeki64(f, static_cast<int64_t>(e.dataOffset), SEEK_SET);
    return std::fread(out.data(), 1, out.size(), f) == out.size();
}

bool TorchCheckpoint::unpickle(const std::vector<uint8_t>& data, std::string* error) {
    std::vector<PyRef> stack;
    std::vector<size_t> marks;
    std::map<uint64_t, PyRef> memo;
    size_t p = 0;
    auto fail = [&](const std::string& why) {
        if (error) *error = "Unreadable checkpoint: " + why;
        return false;
    };
    auto need = [&](size_t n) { return p + n <= data.size(); };
    auto pop = [&]() -> PyRef {
        if (stack.empty()) return nullptr;
        PyRef v = stack.back();
        stack.pop_back();
        return v;
    };
    auto popMark = [&](std::vector<PyRef>& items) {
        if (marks.empty()) return false;
        const size_t m = marks.back();
        marks.pop_back();
        if (m > stack.size()) return false;
        items.assign(stack.begin() + static_cast<long>(m), stack.end());
        stack.resize(m);
        return true;
    };
    auto line = [&]() {
        std::string s;
        while (p < data.size() && data[p] != '\n') s.push_back(static_cast<char>(data[p++]));
        ++p;
        return s;
    };
    auto reduce = [&](const PyRef& fn, const PyRef& args) -> PyRef {
        const std::string name = fn && fn->kind == PyValue::Kind::Global ? fn->s : "";
        const auto& a = args ? args->items : std::vector<PyRef>{};
        if (name == "collections.OrderedDict") return make(PyValue::Kind::Dict);
        if (name == "torch._utils._rebuild_tensor_v2" && a.size() >= 4 && a[0] && a[0]->kind == PyValue::Kind::Storage) {
            auto t = make(PyValue::Kind::Tensor);
            t->tensor.storageKey = a[0]->tensor.storageKey;
            t->tensor.dtype = a[0]->s;
            t->tensor.offset = a[1] ? a[1]->i : 0;
            for (const auto& d : a[2]->items) t->tensor.shape.push_back(d->i);
            for (const auto& d : a[3]->items) t->tensor.strides.push_back(d->i);
            return t;
        }
        if ((name == "torch._utils._rebuild_parameter" || name == "torch._utils._rebuild_parameter_with_state") && !a.empty())
            return a[0];
        auto o = makeStr(name, PyValue::Kind::Object);
        return o;
    };

    while (p < data.size()) {
        const uint8_t op = data[p++];
        switch (op) {
        case 0x80: p += 1; break;           // PROTO
        case 0x95: p += 8; break;           // FRAME
        case '.': goto done;                // STOP
        case '(': marks.push_back(stack.size()); break;
        case '}': stack.push_back(make(PyValue::Kind::Dict)); break;
        case ']': stack.push_back(make(PyValue::Kind::List)); break;
        case ')': stack.push_back(make(PyValue::Kind::Tuple)); break;
        case 0x8f: stack.push_back(make(PyValue::Kind::List)); break; // EMPTY_SET
        case 'N': stack.push_back(make(PyValue::Kind::None)); break;
        case 0x88:
        case 0x89: {
            auto b = make(PyValue::Kind::Bool);
            b->i = op == 0x88;
            stack.push_back(b);
            break;
        }
        case 'J':
            if (!need(4)) return fail("truncated");
            stack.push_back(makeInt(static_cast<int32_t>(u32(&data[p]))));
            p += 4;
            break;
        case 'K':
            if (!need(1)) return fail("truncated");
            stack.push_back(makeInt(data[p++]));
            break;
        case 'M':
            if (!need(2)) return fail("truncated");
            stack.push_back(makeInt(u16(&data[p])));
            p += 2;
            break;
        case 0x8a: { // LONG1
            if (!need(1)) return fail("truncated");
            const int n = data[p++];
            if (!need(n) || n > 8) return fail("integer too large");
            int64_t v = 0;
            for (int k = 0; k < n; ++k) v |= static_cast<int64_t>(data[p + k]) << (8 * k);
            if (n > 0 && n < 8 && (data[p + n - 1] & 0x80)) v -= static_cast<int64_t>(1) << (8 * n);
            p += n;
            stack.push_back(makeInt(v));
            break;
        }
        case 'I':
        case 'L': {
            std::string s = line();
            if (!s.empty() && s.back() == 'L') s.pop_back();
            if (s == "00" || s == "01") {
                auto b = make(PyValue::Kind::Bool);
                b->i = s == "01";
                stack.push_back(b);
            } else {
                stack.push_back(makeInt(std::strtoll(s.c_str(), nullptr, 10)));
            }
            break;
        }
        case 'G': {
            if (!need(8)) return fail("truncated");
            uint64_t bits = 0;
            for (int k = 0; k < 8; ++k) bits = (bits << 8) | data[p + k];
            double d;
            std::memcpy(&d, &bits, 8);
            p += 8;
            auto v = make(PyValue::Kind::Float);
            v->f = d;
            stack.push_back(v);
            break;
        }
        case 'X':
        case 0x8c:
        case 0x8d:
        case 'B':
        case 'C':
        case 0x8e:
        case 'T':
        case 'U': {
            uint64_t n = 0;
            if (op == 0x8c || op == 'C' || op == 'U') {
                if (!need(1)) return fail("truncated");
                n = data[p++];
            } else if (op == 0x8d || op == 0x8e) {
                if (!need(8)) return fail("truncated");
                n = u64(&data[p]);
                p += 8;
            } else {
                if (!need(4)) return fail("truncated");
                n = u32(&data[p]);
                p += 4;
            }
            if (!need(static_cast<size_t>(n))) return fail("truncated");
            const bool bytes = op == 'B' || op == 'C' || op == 0x8e;
            stack.push_back(makeStr(std::string(reinterpret_cast<const char*>(&data[p]), static_cast<size_t>(n)),
                                    bytes ? PyValue::Kind::Bytes : PyValue::Kind::Str));
            p += static_cast<size_t>(n);
            break;
        }
        case 'q':
        case 'r': {
            const size_t n = op == 'q' ? 1 : 4;
            if (!need(n) || stack.empty()) return fail("truncated");
            memo[op == 'q' ? data[p] : u32(&data[p])] = stack.back();
            p += n;
            break;
        }
        case 0x94:
            if (stack.empty()) return fail("bad memo");
            memo[memo.size()] = stack.back();
            break;
        case 'h':
        case 'j': {
            const size_t n = op == 'h' ? 1 : 4;
            if (!need(n)) return fail("truncated");
            auto it = memo.find(op == 'h' ? data[p] : u32(&data[p]));
            p += n;
            if (it == memo.end()) return fail("bad memo reference");
            stack.push_back(it->second);
            break;
        }
        case 't': {
            auto t = make(PyValue::Kind::Tuple);
            if (!popMark(t->items)) return fail("bad tuple");
            stack.push_back(t);
            break;
        }
        case 0x85:
        case 0x86:
        case 0x87: {
            const size_t n = op - 0x84;
            if (stack.size() < n) return fail("bad tuple");
            auto t = make(PyValue::Kind::Tuple);
            t->items.assign(stack.end() - static_cast<long>(n), stack.end());
            stack.resize(stack.size() - n);
            stack.push_back(t);
            break;
        }
        case 'a': {
            PyRef v = pop();
            if (stack.empty() || stack.back()->kind != PyValue::Kind::List) return fail("bad append");
            stack.back()->items.push_back(v);
            break;
        }
        case 'e':
        case 0x90: { // APPENDS, ADDITEMS
            std::vector<PyRef> items;
            if (!popMark(items) || stack.empty()) return fail("bad append");
            auto& dst = stack.back()->items;
            dst.insert(dst.end(), items.begin(), items.end());
            break;
        }
        case 0x91: { // FROZENSET
            auto t = make(PyValue::Kind::List);
            if (!popMark(t->items)) return fail("bad set");
            stack.push_back(t);
            break;
        }
        case 's': {
            PyRef v = pop(), k = pop();
            if (stack.empty() || stack.back()->kind != PyValue::Kind::Dict) return fail("bad setitem");
            stack.back()->dict.emplace_back(k, v);
            break;
        }
        case 'u': {
            std::vector<PyRef> items;
            if (!popMark(items) || stack.empty() || stack.back()->kind != PyValue::Kind::Dict || items.size() % 2)
                return fail("bad setitems");
            for (size_t k = 0; k + 1 < items.size(); k += 2) stack.back()->dict.emplace_back(items[k], items[k + 1]);
            break;
        }
        case 'c': {
            std::string module = line(), name = line();
            stack.push_back(makeStr(module + "." + name, PyValue::Kind::Global));
            break;
        }
        case 0x93: { // STACK_GLOBAL
            PyRef name = pop(), module = pop();
            if (!name || !module) return fail("bad global");
            stack.push_back(makeStr(module->s + "." + name->s, PyValue::Kind::Global));
            break;
        }
        case 'R': {
            PyRef args = pop(), fn = pop();
            stack.push_back(reduce(fn, args));
            break;
        }
        case 0x81: { // NEWOBJ
            PyRef args = pop(), cls = pop();
            stack.push_back(reduce(cls, args));
            break;
        }
        case 0x92: { // NEWOBJ_EX
            pop();
            PyRef args = pop(), cls = pop();
            stack.push_back(reduce(cls, args));
            break;
        }
        case 'b': { // BUILD: object state, not needed
            pop();
            break;
        }
        case 'Q': { // BINPERSID: ('storage', type, key, location, numel)
            PyRef pid = pop();
            if (!pid || pid->kind != PyValue::Kind::Tuple || pid->items.size() < 3) return fail("bad storage reference");
            auto st = make(PyValue::Kind::Storage);
            st->s = storageDtype(pid->items[1] ? pid->items[1]->s : "");
            st->tensor.storageKey = pid->items[2] ? pid->items[2]->s : "";
            stack.push_back(st);
            break;
        }
        case '0': pop(); break;
        case '1': {
            std::vector<PyRef> items;
            if (!popMark(items)) return fail("bad mark");
            break;
        }
        case '2':
            if (stack.empty()) return fail("bad dup");
            stack.push_back(stack.back());
            break;
        default: {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "unsupported pickle opcode 0x%02X", op);
            return fail(buf);
        }
        }
    }
done:
    if (stack.empty()) return fail("empty");
    root_ = stack.back();
    return true;
}

bool TorchCheckpoint::read(const TensorRef& t, std::vector<float>& out, std::string* error) const {
    auto it = entries_.find(prefix_ + "data/" + t.storageKey);
    const int es = dtypeSize(t.dtype);
    if (it == entries_.end() || es == 0) {
        if (error) *error = "Missing or unsupported tensor storage";
        return false;
    }
    std::vector<uint8_t> raw;
    if (!readEntry(it->second, raw)) {
        if (error) *error = "Can't read a tensor";
        return false;
    }
    int64_t count = 1;
    for (int64_t d : t.shape) count *= d;
    out.assign(static_cast<size_t>(count), 0.0f);
    const size_t dims = t.shape.size();
    std::vector<int64_t> idx(dims, 0);
    auto value = [&](int64_t e, float& v) {
        const size_t b = static_cast<size_t>(e) * es;
        if (b + es > raw.size()) return false;
        const uint8_t* q = &raw[b];
        if (t.dtype == "float16") v = halfToFloat(u16(q));
        else if (t.dtype == "bfloat16") {
            const uint32_t bits = static_cast<uint32_t>(u16(q)) << 16;
            std::memcpy(&v, &bits, 4);
        } else if (t.dtype == "float32") std::memcpy(&v, q, 4);
        else if (t.dtype == "float64") {
            double d;
            std::memcpy(&d, q, 8);
            v = static_cast<float>(d);
        } else if (t.dtype == "int64") v = static_cast<float>(static_cast<int64_t>(u64(q)));
        else if (t.dtype == "int32") v = static_cast<float>(static_cast<int32_t>(u32(q)));
        else if (t.dtype == "int16") v = static_cast<float>(static_cast<int16_t>(u16(q)));
        else if (t.dtype == "int8") v = static_cast<float>(static_cast<int8_t>(*q));
        else v = static_cast<float>(*q);
        return true;
    };
    for (int64_t n = 0; n < count; ++n) {
        int64_t e = t.offset;
        for (size_t d = 0; d < dims; ++d) e += idx[d] * (d < t.strides.size() ? t.strides[d] : 0);
        if (!value(e, out[static_cast<size_t>(n)])) {
            if (error) *error = "Tensor outside its storage";
            return false;
        }
        for (size_t d = dims; d-- > 0;) {
            if (++idx[d] < t.shape[d]) break;
            idx[d] = 0;
        }
    }
    return true;
}

std::map<std::string, TensorRef> TorchCheckpoint::tensors(const PyRef& dict) {
    std::map<std::string, TensorRef> out;
    if (!dict || dict->kind != PyValue::Kind::Dict) return out;
    for (const auto& [k, v] : dict->dict)
        if (k && k->kind == PyValue::Kind::Str && v && v->kind == PyValue::Kind::Tensor) out[k->s] = v->tensor;
    return out;
}

} // namespace echo::ai
