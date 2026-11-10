// Video workspace implementation (see video.hpp). Direct port of
// gui/src/video.zig: timeline, project file, chunked analysis, per-sequence
// identity assignment, cached processing, Studio retouch and ffmpeg export.
//
// Everything that touches the shared arrays (samples, chunks, assigns,
// overrides, segments, edits) goes through data_mu_. The job handoff uses
// atomics plus a message written before the completion flag.

#include "video.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace app {

namespace {

std::string baseName(const std::string& path) {
    const size_t idx = path.find_last_of("/\\");
    return idx == std::string::npos ? path : path.substr(idx + 1);
}

std::string stripExt(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

std::string dirPart(const std::string& path) {
    const size_t idx = path.find_last_of("/\\");
    if (idx == std::string::npos) return "";
    if (idx == 0) return path.substr(0, 1);
    return path.substr(0, idx);
}

std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const char last = a.back();
    if (last == '/' || last == '\\') return a + b;
    return a + "/" + b;
}

// FNV-1a 64-bit. The cache signature only needs to be stable within a run, so a
// simple hash is enough (it is never shared with the Zig build).
struct Hasher {
    uint64_t h = 0xcbf29ce484222325ULL;
    void bytes(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 0x100000001b3ULL;
        }
    }
    template <typename T>
    void val(const T& v) {
        bytes(&v, sizeof(T));
    }
};

uint64_t pathHash(const std::string& s) {
    Hasher hs;
    hs.bytes(s.data(), s.size());
    return hs.h;
}

std::string hex8(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08x", v);
    return buf;
}

std::string hex16(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string sigDirName(VideoQuality q) { return q == VideoQuality::Low ? "low" : "high"; }

// ---- minimal JSON reader (project files) -----------------------------------

struct JVal {
    enum class T { Null, Bool, Num, Str, Arr, Obj } t = T::Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;

    const JVal* get(const char* key) const {
        for (const auto& kv : obj) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
    double numOr(double d) const { return t == T::Num ? num : d; }
    bool boolOr(bool d) const { return t == T::Bool ? b : d; }
};

struct JParser {
    const std::string& s;
    size_t i = 0;
    explicit JParser(const std::string& text) : s(text) {}

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool parse(JVal& out) {
        ws();
        if (!value(out)) return false;
        return true;
    }
    bool value(JVal& v) {
        ws();
        if (i >= s.size()) return false;
        const char c = s[i];
        if (c == '{') return object(v);
        if (c == '[') return array(v);
        if (c == '"') {
            v.t = JVal::T::Str;
            return string(v.str);
        }
        if (c == 't') return lit("true", v, true);
        if (c == 'f') return lit("false", v, false);
        if (c == 'n') return lit("null", v, false);
        v.t = JVal::T::Num;
        const char* begin = s.c_str() + i;
        char* end = nullptr;
        v.num = std::strtod(begin, &end);
        if (!end || end == begin) return false;
        i = static_cast<size_t>(end - s.c_str());
        return true;
    }
    bool lit(const char* w, JVal& v, bool b) {
        const size_t n = std::strlen(w);
        if (s.compare(i, n, w) != 0) return false;
        i += n;
        v.t = JVal::T::Bool;
        v.b = b;
        return true;
    }
    bool string(std::string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        out.clear();
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) {
                ++i;
                switch (s[i]) {
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'u':
                        // Minimal: skip the code point (project strings are paths).
                        if (i + 4 < s.size()) i += 4;
                        out.push_back('?');
                        break;
                    default: out.push_back(s[i]); break;
                }
                ++i;
            } else {
                out.push_back(s[i++]);
            }
        }
        if (i >= s.size()) return false;
        ++i;
        return true;
    }
    bool array(JVal& v) {
        v.t = JVal::T::Arr;
        ++i;
        ws();
        if (i < s.size() && s[i] == ']') {
            ++i;
            return true;
        }
        while (i < s.size()) {
            JVal e;
            if (!value(e)) return false;
            v.arr.push_back(std::move(e));
            ws();
            if (i < s.size() && s[i] == ',') {
                ++i;
                ws();
                continue;
            }
            if (i < s.size() && s[i] == ']') {
                ++i;
                return true;
            }
            return false;
        }
        return false;
    }
    bool object(JVal& v) {
        v.t = JVal::T::Obj;
        ++i;
        ws();
        if (i < s.size() && s[i] == '}') {
            ++i;
            return true;
        }
        while (i < s.size()) {
            std::string key;
            if (!string(key)) return false;
            ws();
            if (i >= s.size() || s[i] != ':') return false;
            ++i;
            JVal val;
            if (!value(val)) return false;
            v.obj.emplace_back(std::move(key), std::move(val));
            ws();
            if (i < s.size() && s[i] == ',') {
                ++i;
                ws();
                continue;
            }
            if (i < s.size() && s[i] == '}') {
                ++i;
                return true;
            }
            return false;
        }
        return false;
    }
};

void jsonStr(std::ostringstream& o, const std::string& s) {
    o << '"';
    for (char c : s) {
        switch (c) {
            case '"': o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\n': o << "\\n"; break;
            case '\t': o << "\\t"; break;
            case '\r': o << "\\r"; break;
            default: o << c; break;
        }
    }
    o << '"';
}

void jsonNum(std::ostringstream& o, double v) {
    if (v == std::floor(v) && std::abs(v) < 1e15) {
        o << static_cast<long long>(v);
    } else {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.6g", v);
        o << buf;
    }
}

// ---- Studio mask painting --------------------------------------------------

void stampDab(std::vector<uint8_t>& mask, int32_t w, int32_t h, float cx, float cy, float radius,
              float hard, bool reveal, float opac) {
    const float o = std::clamp(opac, 0.01f, 1.0f);
    const float inner = radius * hard;
    const float falloff = 1.0f / std::max(0.0001f, radius - inner);

    const int32_t x0 = std::max(0, static_cast<int32_t>(std::floor(cx - radius)));
    const int32_t y0 = std::max(0, static_cast<int32_t>(std::floor(cy - radius)));
    const int32_t x1 = std::min(w - 1, static_cast<int32_t>(std::floor(cx + radius)));
    const int32_t y1 = std::min(h - 1, static_cast<int32_t>(std::floor(cy + radius)));
    if (x1 < x0 || y1 < y0) return;

    const size_t wu = static_cast<size_t>(w);
    for (int32_t yy = y0; yy <= y1; ++yy) {
        uint8_t* row = mask.data() + static_cast<size_t>(yy) * wu;
        const float dy = static_cast<float>(yy) + 0.5f - cy;
        for (int32_t xx = x0; xx <= x1; ++xx) {
            const float dx = static_cast<float>(xx) + 0.5f - cx;
            const float d = std::sqrt(dx * dx + dy * dy);
            float t = 1.0f;
            if (d > inner) t = 1.0f - (d - inner) * falloff;
            if (t <= 0.0f) continue;
            const float a = o * t;
            const float cur = static_cast<float>(row[xx]);
            const float nv = reveal ? cur + (255.0f - cur) * a : cur * (1.0f - a);
            row[xx] = static_cast<uint8_t>(std::clamp(nv, 0.0f, 255.0f));
        }
    }
}

void stampStroke(std::vector<uint8_t>& mask, int32_t w, int32_t h, const VidStroke& st, float sx,
                 float sy, float hard, float opac) {
    const auto& pts = st.pts;
    if (pts.empty()) return;
    const float radius = std::max(0.6f, st.radius * 0.5f * sx);
    const float spacing = std::max(1.0f, radius * 0.35f);

    float px = pts[0].x * sx;
    float py = pts[0].y * sy;
    stampDab(mask, w, h, px, py, radius, hard, st.reveal, opac);
    for (size_t i = 1; i < pts.size(); ++i) {
        const float x = pts[i].x * sx;
        const float y = pts[i].y * sy;
        const float dx = x - px;
        const float dy = y - py;
        const float dist = std::sqrt(dx * dx + dy * dy);
        const uint32_t steps =
            static_cast<uint32_t>(std::max(1.0f, std::ceil(dist / spacing)));
        for (uint32_t k = 1; k <= steps; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(steps);
            stampDab(mask, w, h, px + dx * t, py + dy * t, radius, hard, st.reveal, opac);
        }
        px = x;
        py = y;
    }
}

// Position/size similarity between two boxes; smaller is closer.
float faceDistance(const float a[4], const float b[4]) {
    const float aw = std::max(1.0f, a[2]);
    const float ah = std::max(1.0f, a[3]);
    const float bw = std::max(1.0f, b[2]);
    const float bh = std::max(1.0f, b[3]);
    const float acx = a[0] + aw * 0.5f;
    const float acy = a[1] + ah * 0.5f;
    const float bcx = b[0] + bw * 0.5f;
    const float bcy = b[1] + bh * 0.5f;
    const float dsize = std::abs(bw - aw) / aw + std::abs(bh - ah) / ah;
    const float dpos = (std::abs(bcx - acx) + std::abs(bcy - acy)) / std::max(aw, ah);
    return dsize + dpos;
}

float iou(const float a[4], const float b[4]) {
    const float ax0 = a[0], ay0 = a[1], ax1 = a[0] + a[2], ay1 = a[1] + a[3];
    const float bx0 = b[0], by0 = b[1], bx1 = b[0] + b[2], by1 = b[1] + b[3];
    const float ix = std::max(0.0f, std::min(ax1, bx1) - std::max(ax0, bx0));
    const float iy = std::max(0.0f, std::min(ay1, by1) - std::max(ay0, by0));
    const float inter = ix * iy;
    const float uni = std::max(0.0f, ax1 - ax0) * std::max(0.0f, ay1 - ay0) +
                      std::max(0.0f, bx1 - bx0) * std::max(0.0f, by1 - by0) - inter;
    if (uni <= 0.0001f) return 0.0f;
    return inter / uni;
}

bool closeBoxes(const float a[kVideoMaxFaces][4], const float b[kVideoMaxFaces][4], int count) {
    for (int i = 0; i < count; ++i) {
        if (iou(a[i], b[i]) >= 0.18f) continue;
        const float aw = std::max(a[i][2], 1.0f);
        const float ah = std::max(a[i][3], 1.0f);
        const float dx = (a[i][0] + a[i][2] * 0.5f) - (b[i][0] + b[i][2] * 0.5f);
        const float dy = (a[i][1] + a[i][3] * 0.5f) - (b[i][1] + b[i][3] * 0.5f);
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (dist > std::max(aw, ah) * 0.9f) return false;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Video::Video(App* host) : host_(host) {
    for (int i = 0; i < kVideoMaxFaces; ++i) seg_ids[i] = -1;
}

Video::~Video() {
    close();
    if (thread_.joinable()) thread_.join();
}

void Video::setAutotest(const std::string& path) {
    test_path_ = path;
    test_keep_ = std::getenv("FACEFORGE_VIDEOTEST_KEEP") != nullptr;
    test_state_ = VideoTestState::Start;
}

bool Video::hasProject() const {
    return !proj_path_.empty() && ff::fileExists(proj_path_.c_str());
}

// ---------------------------------------------------------------------------
// Paths & project
// ---------------------------------------------------------------------------

std::string Video::baseName(const std::string& p) { return ::app::baseName(p); }
std::string Video::stripExt(const std::string& n) { return ::app::stripExt(n); }

void Video::setCacheRootFor(const std::string& video_path) {
    const std::string name = baseName(video_path);
    const std::string stem = stripExt(name);
    const std::string dir = dirPart(video_path);

    const std::string configured = host_ ? host_->temp_dir : std::string();
    if (!configured.empty()) {
        const uint32_t h = static_cast<uint32_t>(pathHash(video_path) & 0xffffffffULL);
        const std::string sub = stem + "-" + hex8(h) + ".faceforge.cache";
        cache_root_ = joinPath(configured, sub);
    } else {
        cache_root_ = joinPath(dir, ".faceforge.cache");
    }
}

void Video::projectPathFor(const std::string& video_path) {
    const std::string name = baseName(video_path);
    const std::string stem = stripExt(name);
    const std::string dir = dirPart(video_path);
    proj_path_ = joinPath(dir, stem + ".faceforge.json");

    setCacheRootFor(video_path);

    const std::string export_root = host_ ? host_->export_dir : std::string();
    const std::string out_dir = export_root.empty() ? dir : export_root;
    job_out_ = joinPath(out_dir, stem + "_faceforge.mp4");
}

void Video::refreshPaths() {
    if (path_.empty()) return;
    setCacheRootFor(path_);
}

void Video::saveProject() {
    if (proj_path_.empty() || !open_) return;
    std::ostringstream o;
    {
        std::lock_guard<std::mutex> lk(data_mu_);
        o << "{";
        o << "\"version\":1,";
        o << "\"video\":";
        jsonStr(o, path_);
        o << ",\"in_frame\":" << in_frame << ",\"out_frame\":" << out_frame;
        o << ",\"chunk\":" << static_cast<long long>(std::llround(chunk_f));
        o << ",\"stride\":" << static_cast<long long>(std::llround(stride_f));
        o << ",\"default_skip\":" << (default_skip ? "true" : "false");
        o << ",\"samples\":[";
        for (size_t i = 0; i < samples_.size(); ++i) {
            const VidSample& s = samples_[i];
            if (i) o << ",";
            o << "{\"f\":" << s.f << ",\"n\":" << static_cast<int>(s.n) << ",\"b\":[";
            for (int k = 0; k < static_cast<int>(s.n) && k < kVideoMaxFaces; ++k) {
                if (k) o << ",";
                o << "[";
                for (int j = 0; j < 4; ++j) {
                    if (j) o << ",";
                    jsonNum(o, s.b[k][j]);
                }
                o << "]";
            }
            o << "]}";
        }
        o << "],\"chunks\":[";
        for (size_t i = 0; i < chunks_.size(); ++i) {
            const VidChunk& c = chunks_[i];
            if (i) o << ",";
            o << "{\"start\":" << c.start << ",\"end\":" << c.end
              << ",\"done\":" << (c.done ? "true" : "false") << "}";
        }
        o << "],\"assigns\":[";
        for (size_t i = 0; i < assigns_.size(); ++i) {
            const VidAssign& a = assigns_[i];
            if (i) o << ",";
            o << "{\"a\":" << a.a << ",\"b\":" << a.b << ",\"ids\":[";
            for (int k = 0; k < kVideoMaxFaces; ++k) {
                if (k) o << ",";
                o << a.ids[k];
            }
            o << "]}";
        }
        o << "],\"overrides\":[";
        for (size_t i = 0; i < overrides_.size(); ++i) {
            const VidOverride& ov = overrides_[i];
            if (i) o << ",";
            o << "{\"f\":" << ov.f << ",\"ids\":[";
            for (int k = 0; k < kVideoMaxFaces; ++k) {
                if (k) o << ",";
                o << ov.ids[k];
            }
            o << "]}";
        }
        o << "],\"edits\":[";
        for (size_t i = 0; i < edits_.size(); ++i) {
            const VidFrameEdit& e = edits_[i];
            if (i) o << ",";
            o << "{\"f\":" << e.f << ",\"bypassed\":" << (e.bypassed ? "true" : "false")
              << ",\"strokes\":[";
            for (size_t j = 0; j < e.strokes.size(); ++j) {
                const VidStroke& st = e.strokes[j];
                if (j) o << ",";
                o << "{\"radius\":";
                jsonNum(o, st.radius);
                o << ",\"reveal\":" << (st.reveal ? "true" : "false") << ",\"pts\":[";
                for (size_t p = 0; p < st.pts.size(); ++p) {
                    if (p) o << ",";
                    o << "{\"x\":";
                    jsonNum(o, st.pts[p].x);
                    o << ",\"y\":";
                    jsonNum(o, st.pts[p].y);
                    o << "}";
                }
                o << "]}";
            }
            o << "]}";
        }
        o << "]}";
    }
    const std::string text = o.str();
    if (!ff::writeFile(proj_path_.c_str(), reinterpret_cast<const uint8_t*>(text.data()),
                       text.size())) {
        err = "Cannot write the project file";
    }
}

bool Video::loadProject() {
    if (proj_path_.empty()) return false;
    std::vector<uint8_t> bytes;
    if (!ff::readFile(proj_path_.c_str(), bytes)) return false;
    const std::string text(bytes.begin(), bytes.end());
    JVal root;
    {
        JParser p(text);
        if (!p.parse(root) || root.t != JVal::T::Obj) return false;
    }

    {
        std::lock_guard<std::mutex> lk(data_mu_);
        samples_.clear();
        chunks_.clear();
        assigns_.clear();
        overrides_.clear();
        edits_.clear();

        if (const JVal* a = root.get("samples")) {
            for (const JVal& s : a->arr) {
                VidSample smp;
                smp.f = static_cast<int64_t>(s.get("f") ? s.get("f")->numOr(0) : 0);
                int n = static_cast<int>(s.get("n") ? s.get("n")->numOr(0) : 0);
                n = std::clamp(n, 0, kVideoMaxFaces);
                smp.n = static_cast<uint8_t>(n);
                if (const JVal* b = s.get("b")) {
                    for (int k = 0; k < n && k < static_cast<int>(b->arr.size()); ++k) {
                        const JVal& box = b->arr[static_cast<size_t>(k)];
                        for (int j = 0; j < 4 && j < static_cast<int>(box.arr.size()); ++j) {
                            smp.b[k][j] = static_cast<float>(box.arr[static_cast<size_t>(j)].numOr(0));
                        }
                    }
                }
                samples_.push_back(smp);
            }
        }
        if (const JVal* a = root.get("chunks")) {
            for (const JVal& c : a->arr) {
                VidChunk ch;
                ch.start = static_cast<int64_t>(c.get("start") ? c.get("start")->numOr(0) : 0);
                ch.end = static_cast<int64_t>(c.get("end") ? c.get("end")->numOr(0) : 0);
                ch.done = c.get("done") ? c.get("done")->boolOr(false) : false;
                chunks_.push_back(ch);
            }
        }
        if (const JVal* a = root.get("assigns")) {
            for (const JVal& av : a->arr) {
                VidAssign as;
                as.a = static_cast<int64_t>(av.get("a") ? av.get("a")->numOr(0) : 0);
                as.b = static_cast<int64_t>(av.get("b") ? av.get("b")->numOr(0) : 0);
                if (const JVal* ids = av.get("ids")) {
                    for (int k = 0; k < kVideoMaxFaces && k < static_cast<int>(ids->arr.size()); ++k) {
                        as.ids[k] = static_cast<int32_t>(ids->arr[static_cast<size_t>(k)].numOr(-1));
                    }
                }
                assigns_.push_back(as);
            }
        }
        if (const JVal* a = root.get("overrides")) {
            for (const JVal& ov : a->arr) {
                VidOverride o;
                o.f = static_cast<int64_t>(ov.get("f") ? ov.get("f")->numOr(0) : 0);
                if (const JVal* ids = ov.get("ids")) {
                    for (int k = 0; k < kVideoMaxFaces && k < static_cast<int>(ids->arr.size()); ++k) {
                        o.ids[k] = static_cast<int32_t>(ids->arr[static_cast<size_t>(k)].numOr(-1));
                    }
                }
                overrides_.push_back(o);
            }
        }
        if (const JVal* a = root.get("edits")) {
            for (const JVal& e : a->arr) {
                VidFrameEdit fe;
                fe.f = static_cast<int64_t>(e.get("f") ? e.get("f")->numOr(0) : 0);
                fe.bypassed = e.get("bypassed") ? e.get("bypassed")->boolOr(false) : false;
                if (const JVal* strokes = e.get("strokes")) {
                    for (const JVal& js : strokes->arr) {
                        VidStroke st;
                        st.radius = static_cast<float>(js.get("radius") ? js.get("radius")->numOr(30) : 30);
                        st.reveal = js.get("reveal") ? js.get("reveal")->boolOr(true) : true;
                        if (const JVal* pts = js.get("pts")) {
                            for (const JVal& jp : pts->arr) {
                                VidStrokePt p;
                                p.x = static_cast<float>(jp.get("x") ? jp.get("x")->numOr(0) : 0);
                                p.y = static_cast<float>(jp.get("y") ? jp.get("y")->numOr(0) : 0);
                                st.pts.push_back(p);
                            }
                        }
                        fe.strokes.push_back(std::move(st));
                    }
                }
                edits_.push_back(std::move(fe));
            }
        }
    }
    edit_gen_ += 1;

    const double inf = root.get("in_frame") ? root.get("in_frame")->numOr(0) : 0;
    const double outf = root.get("out_frame") ? root.get("out_frame")->numOr(0) : 0;
    const double chunk = root.get("chunk") ? root.get("chunk")->numOr(300) : 300;
    const double stride = root.get("stride") ? root.get("stride")->numOr(1) : 1;
    if (inf >= 0) {
        const int64_t cap = info_.frames > 0 ? info_.frames - 1 : static_cast<int64_t>(inf);
        in_frame = std::min(static_cast<int64_t>(inf), cap);
    }
    if (outf > 0) out_frame = static_cast<int64_t>(outf);
    if (chunk >= 1) chunk_f = static_cast<float>(std::min<int64_t>(static_cast<int64_t>(chunk), 100000));
    if (stride >= 1) stride_f = static_cast<float>(std::min<int64_t>(static_cast<int64_t>(stride), 240));
    default_skip = root.get("default_skip") ? root.get("default_skip")->boolOr(false) : false;
    return true;
}

// ---------------------------------------------------------------------------
// Open / close
// ---------------------------------------------------------------------------

void Video::openPath(const std::string& path) {
    close();
    if (path.empty()) return;
    path_ = path;

    std::string derr;
    if (!decoder_.open(path.c_str(), &derr)) {
        err = "Cannot open this video. Check the file and the codec.";
        return;
    }
    const ff::Video::Info info = decoder_.info();
    if (info.width <= 0 || info.height <= 0) {
        decoder_.close();
        err = "The video has no decodable video stream.";
        return;
    }
    info_ = info;
    open_ = true;
    in_frame = 0;
    out_frame = info.frames > 0 ? info.frames - 1 : 0;
    cur = 0;
    cur_loaded_ = -1;
    playing_ = false;
    sel_seg = -1;

    name_ = baseName(path);
    projectPathFor(path);
    rebuildSegments();
    if (ff::fileExists(proj_path_.c_str())) {
        loadProject();
        rebuildSegments();
        setStatus("Loaded " + name_ + " and its project", false);
    } else {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s \xC2\xB7 %dx%d \xC2\xB7 %.2f fps", name_.c_str(),
                      info.width, info.height, info.fps);
        setStatus(buf, false);
    }
    err.clear();
    seekTo(cur);
    selectSegmentAt(cur);
}

void Video::close() {
    stopPlayback();
    if (job != VideoJob::None) {
        cancel_.store(true, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
        job = VideoJob::None;
    }
    decoder_.close();
    freeFaceThumbs();
    cur_img.reset();
    res_img.reset();
    cur_dirty = true;
    res_dirty = true;
    res_base_.reset();
    cur_loaded_ = -1;
    res_frame_ = -1;
    res_sig_ = ~0ull;
    res_edit_gen_ = ~0ull;
    studio = false;
    stroke_active = false;
    copy_pending = false;
    open_ = false;
    path_.clear();
    proj_path_.clear();
    cache_root_.clear();
    info_ = ff::Video::Info{};
    in_frame = 0;
    out_frame = 0;
    cur = 0;
    playing_ = false;
    {
        std::lock_guard<std::mutex> lk(data_mu_);
        samples_.clear();
        chunks_.clear();
        assigns_.clear();
        overrides_.clear();
        segments_.clear();
        edits_.clear();
    }
    edit_gen_ += 1;
    sel_seg = -1;
}

// ---------------------------------------------------------------------------
// Sequences
// ---------------------------------------------------------------------------

void Video::rebuildSegments() {
    face_thumbs_stale_ = true;
    std::lock_guard<std::mutex> lk(data_mu_);
    segments_.clear();
    const auto& items = samples_;
    if (items.empty()) return;

    const int64_t stride = std::max<int64_t>(1, static_cast<int64_t>(std::llround(stride_f)));

    size_t i = 0;
    while (i < items.size()) {
        const VidSample& s = items[i];
        if (s.n == 0) {
            ++i;
            continue;
        }
        VidSegment seg;
        seg.start = s.f;
        seg.end = s.f;
        seg.count = s.n;
        std::memcpy(seg.boxes, s.b, sizeof(seg.boxes));
        size_t j = i + 1;
        for (; j < items.size(); ++j) {
            const VidSample& t = items[j];
            if (t.n != seg.count) break;
            if (t.f - seg.end > stride * 3) break;
            if (!closeBoxes(seg.boxes, t.b, seg.count)) break;
            seg.end = t.f;
        }
        if (segments_.size() < kMaxSegments) {
            segments_.push_back(seg);
        } else if (!segments_.empty()) {
            segments_.back().end = seg.end;
        }
        i = j;
    }

    while (segments_.size() > kMaxSegments) {
        size_t best = 0;
        int64_t best_gap = INT64_MAX;
        for (size_t k = 0; k + 1 < segments_.size(); ++k) {
            if (segments_[k].count != segments_[k + 1].count) continue;
            const int64_t gap = segments_[k + 1].start - segments_[k].end;
            if (gap < best_gap) {
                best_gap = gap;
                best = k;
            }
        }
        if (best_gap == INT64_MAX) break;
        segments_[best].end = segments_[best + 1].end;
        segments_.erase(segments_.begin() + static_cast<long>(best) + 1);
    }
}

int Video::segmentIndexAtLocked(int64_t frame) const {
    for (size_t i = 0; i < segments_.size(); ++i) {
        if (frame >= segments_[i].start && frame <= segments_[i].end) return static_cast<int>(i);
    }
    return -1;
}

void Video::selectSegmentAt(int64_t frame) {
    int idx;
    VidSegment seg;
    {
        std::lock_guard<std::mutex> lk(data_mu_);
        idx = segmentIndexAtLocked(frame);
        if (idx >= 0) seg = segments_[static_cast<size_t>(idx)];
    }
    sel_seg = idx;
    if (idx >= 0) {
        int32_t ids[kVideoMaxFaces];
        idsForFrame(frame, seg.count, ids);
        for (int k = 0; k < kVideoMaxFaces; ++k) seg_ids[k] = ids[k];
    }
}

void Video::selectSegment(int index) {
    VidSegment seg;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lk(data_mu_);
        if (index >= 0 && index < static_cast<int>(segments_.size())) {
            seg = segments_[static_cast<size_t>(index)];
            ok = true;
        }
    }
    sel_seg = ok ? index : -1;
    if (ok) {
        int32_t ids[kVideoMaxFaces];
        idsForFrame(seg.start, seg.count, ids);
        for (int k = 0; k < kVideoMaxFaces; ++k) seg_ids[k] = ids[k];
    }
}

// ---------------------------------------------------------------------------
// Cache paths
// ---------------------------------------------------------------------------

std::string Video::cacheDirInto(VideoQuality q, uint64_t sig) const {
    return cache_root_ + "/" + hex16(sig) + "/" + sigDirName(q);
}

std::string Video::framePathInto(VideoQuality q, uint64_t sig, int64_t frame) const {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "/f%08lld.jpg", static_cast<long long>(frame));
    return cacheDirInto(q, sig) + buf;
}

ff::Image Video::cachedFrame(int64_t frame, uint64_t sig) {
    const VideoQuality order[2] = {VideoQuality::High, VideoQuality::Low};
    for (VideoQuality q : order) {
        const std::string p = framePathInto(q, sig, frame);
        if (ff::fileExists(p.c_str())) return ff::Image::load(p.c_str());
    }
    return {};
}

// ---------------------------------------------------------------------------
// Cache signature & assignments
// ---------------------------------------------------------------------------

uint64_t Video::cacheSig() const {
    Hasher h;
    const int cnt = host_ ? host_->identityCount() : 0;
    h.val(cnt);
    const int enh = host_ ? static_cast<int>(host_->enhancer) : 0;
    h.val(enh);
    const bool mouth = host_ ? host_->restore_mouth : true;
    h.val(mouth);
    const bool dskip = default_skip;
    h.val(dskip);
    {
        std::lock_guard<std::mutex> lk(data_mu_);
        for (const VidAssign& a : assigns_) {
            h.val(a.a);
            h.val(a.b);
            for (int k = 0; k < kVideoMaxFaces; ++k) h.val(a.ids[k]);
        }
        for (const VidOverride& o : overrides_) {
            h.val(o.f);
            for (int k = 0; k < kVideoMaxFaces; ++k) h.val(o.ids[k]);
        }
    }
    return h.h;
}

void Video::idsForLocked(int64_t frame, int count, int32_t out[kVideoMaxFaces]) const {
    const int32_t fallback = default_skip ? kVideoSkipId : -1;
    for (int i = 0; i < kVideoMaxFaces; ++i) out[i] = fallback;
    for (const VidOverride& o : overrides_) {
        if (o.f == frame) {
            for (int i = 0; i < count && i < kVideoMaxFaces; ++i) out[i] = o.ids[i];
            return;
        }
    }
    const VidAssign* best = nullptr;
    for (const VidAssign& a : assigns_) {
        if (frame >= a.a && frame <= a.b) best = &a;
    }
    if (best) {
        for (int i = 0; i < count && i < kVideoMaxFaces; ++i) out[i] = best->ids[i];
    }
}

void Video::idsForFrame(int64_t frame, int count, int32_t out[kVideoMaxFaces]) const {
    std::lock_guard<std::mutex> lk(data_mu_);
    idsForLocked(frame, count, out);
}

int Video::boxesForFrameLocked(int64_t frame, float out[kVideoMaxFaces][4]) const {
    size_t lo = 0;
    size_t hi = samples_.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (samples_[mid].f < frame) lo = mid + 1;
        else hi = mid;
    }
    const VidSample* smp = nullptr;
    if (lo < samples_.size() && samples_[lo].f == frame) smp = &samples_[lo];
    if (!smp && lo > 0 && samples_[lo - 1].f == frame) smp = &samples_[lo - 1];
    if (smp) {
        std::memcpy(out, smp->b, sizeof(smp->b));
        return smp->n;
    }
    for (const VidSegment& seg : segments_) {
        if (frame >= seg.start && frame <= seg.end) {
            std::memcpy(out, seg.boxes, sizeof(seg.boxes));
            return seg.count;
        }
    }
    return 0;
}

int Video::boxesForFrame(int64_t frame, float out[kVideoMaxFaces][4]) const {
    std::lock_guard<std::mutex> lk(data_mu_);
    return boxesForFrameLocked(frame, out);
}

int Video::segmentCount() const {
    std::lock_guard<std::mutex> lk(data_mu_);
    return static_cast<int>(segments_.size());
}

VidSegment Video::segment(int index) const {
    std::lock_guard<std::mutex> lk(data_mu_);
    if (index < 0 || index >= static_cast<int>(segments_.size())) return VidSegment{};
    return segments_[static_cast<size_t>(index)];
}

int Video::sampleCount() const {
    std::lock_guard<std::mutex> lk(data_mu_);
    return static_cast<int>(samples_.size());
}

bool Video::selectedSegmentBounds(int64_t* start, int64_t* end, int* count) const {
    std::lock_guard<std::mutex> lk(data_mu_);
    if (sel_seg < 0 || sel_seg >= static_cast<int>(segments_.size())) return false;
    const VidSegment& seg = segments_[static_cast<size_t>(sel_seg)];
    if (start) *start = seg.start;
    if (end) *end = seg.end;
    if (count) *count = seg.count;
    return true;
}

void Video::chunkStats(int* done, int* total) const {
    const int64_t span = std::max<int64_t>(1, out_frame - in_frame + 1);
    const int64_t chunk = std::max<int64_t>(1, static_cast<int64_t>(std::llround(chunk_f)));
    if (total) *total = static_cast<int>(std::max<int64_t>(1, (span + chunk - 1) / chunk));
    int d = 0;
    {
        std::lock_guard<std::mutex> lk(data_mu_);
        for (const VidChunk& c : chunks_) {
            if (c.done && c.end >= in_frame && c.start <= out_frame) ++d;
        }
    }
    if (done) *done = d;
}

bool Video::chunkInfo(int64_t frame, int64_t* start, int64_t* end, int* index,
                      int* count) const {
    const int64_t chunk = std::max<int64_t>(1, static_cast<int64_t>(std::llround(chunk_f)));
    const int64_t first = std::max<int64_t>(0, in_frame);
    const int64_t last = std::max<int64_t>(first, out_frame);
    const int64_t span = last - first + 1;
    const int total = static_cast<int>(std::max<int64_t>(1, (span + chunk - 1) / chunk));
    const int64_t clamped = std::clamp<int64_t>(frame, first, last);
    const int idx = static_cast<int>(std::min<int64_t>((clamped - first) / chunk,
                                                       static_cast<int64_t>(total) - 1));
    const int64_t s = first + static_cast<int64_t>(idx) * chunk;
    if (start) *start = s;
    if (end) *end = std::min<int64_t>(last, s + chunk - 1);
    if (index) *index = idx;
    if (count) *count = total;
    return true;
}

void Video::stepChunk(int delta) {
    if (!open_ || delta == 0) return;
    const int64_t chunk = std::max<int64_t>(1, static_cast<int64_t>(std::llround(chunk_f)));
    int64_t start = 0;
    int idx = 0, count = 0;
    chunkInfo(cur, &start, nullptr, &idx, &count);
    const int target = std::clamp(idx + delta, 0, std::max(0, count - 1));
    if (target == idx) return;
    stopPlayback();
    seekTo(start + static_cast<int64_t>(target - idx) * chunk);
    selectSegmentAt(cur);
}

void Video::mergeSamples(const std::vector<VidSample>& batch, int64_t from, int64_t to) {
    std::lock_guard<std::mutex> lk(data_mu_);
    size_t i = 0;
    while (i < samples_.size()) {
        if (samples_[i].f >= from && samples_[i].f <= to) {
            samples_.erase(samples_.begin() + static_cast<long>(i));
        } else {
            ++i;
        }
    }
    for (const VidSample& s : batch) {
        size_t lo = 0, hi = samples_.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (samples_[mid].f < s.f) lo = mid + 1;
            else hi = mid;
        }
        samples_.insert(samples_.begin() + static_cast<long>(lo), s);
    }
}

bool Video::chunkDone(int64_t start, int64_t end) {
    std::lock_guard<std::mutex> lk(data_mu_);
    for (const VidChunk& c : chunks_) {
        if (c.start <= start && c.end >= end) return c.done;
    }
    return false;
}

void Video::markChunk(int64_t start, int64_t end) {
    std::lock_guard<std::mutex> lk(data_mu_);
    for (VidChunk& c : chunks_) {
        if (c.start == start && c.end == end) {
            c.done = true;
            return;
        }
    }
    if (chunks_.size() < kMaxChunks) {
        chunks_.push_back(VidChunk{start, end, true});
    }
}

// ---- sequence assignment editing ------------------------------------------

namespace {
// Overlapping ranges are merged into the new one; the caller holds data_mu_.
void upsertAssignLocked(std::vector<VidAssign>& assigns, int64_t a, int64_t b,
                        const int32_t ids[kVideoMaxFaces]) {
    size_t i = 0;
    while (i < assigns.size()) {
        const VidAssign& cur = assigns[i];
        if (cur.b < a || cur.a > b) {
            ++i;
            continue;
        }
        const int64_t na = std::min(cur.a, a);
        const int64_t nb = std::max(cur.b, b);
        assigns.erase(assigns.begin() + static_cast<long>(i));
        upsertAssignLocked(assigns, na, nb, ids);
        return;
    }
    if (assigns.size() >= kMaxAssigns) assigns.erase(assigns.begin());
    VidAssign as;
    as.a = a;
    as.b = b;
    for (int k = 0; k < kVideoMaxFaces; ++k) as.ids[k] = ids[k];
    assigns.push_back(as);
}

void setOverrideLocked(std::vector<VidOverride>& overrides, int64_t frame,
                       const int32_t ids[kVideoMaxFaces]) {
    for (VidOverride& o : overrides) {
        if (o.f == frame) {
            for (int k = 0; k < kVideoMaxFaces; ++k) o.ids[k] = ids[k];
            return;
        }
    }
    if (overrides.size() >= kMaxOverrides) overrides.erase(overrides.begin());
    VidOverride o;
    o.f = frame;
    for (int k = 0; k < kVideoMaxFaces; ++k) o.ids[k] = ids[k];
    overrides.push_back(o);
}

void clearOverrideLocked(std::vector<VidOverride>& overrides, int64_t frame) {
    for (size_t i = 0; i < overrides.size(); ++i) {
        if (overrides[i].f == frame) {
            overrides.erase(overrides.begin() + static_cast<long>(i));
            return;
        }
    }
}
} // namespace

void Video::setSegId(int k, int32_t id) {
    if (k < 0 || k >= kVideoMaxFaces) return;
    seg_ids[k] = id;
}

void Video::setAllSegDefault() {
    int count = 0;
    {
        std::lock_guard<std::mutex> lk(data_mu_);
        if (sel_seg >= 0 && sel_seg < static_cast<int>(segments_.size())) {
            count = segments_[static_cast<size_t>(sel_seg)].count;
        }
    }
    for (int k = 0; k < count && k < kVideoMaxFaces; ++k) seg_ids[k] = -1;
    setStatus("Sequence faces set to the default identity", false);
}

void Video::setAllSegSkip() {
    int count = 0;
    {
        std::lock_guard<std::mutex> lk(data_mu_);
        if (sel_seg >= 0 && sel_seg < static_cast<int>(segments_.size())) {
            count = segments_[static_cast<size_t>(sel_seg)].count;
        }
    }
    for (int k = 0; k < count && k < kVideoMaxFaces; ++k) seg_ids[k] = kVideoSkipId;
    setStatus("Sequence faces set to leave unchanged", false);
}

void Video::applyAssignment() {
    VidSegment seg;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lk(data_mu_);
        if (sel_seg >= 0 && sel_seg < static_cast<int>(segments_.size())) {
            seg = segments_[static_cast<size_t>(sel_seg)];
            ok = true;
            upsertAssignLocked(assigns_, seg.start, seg.end, seg_ids);
        }
    }
    if (!ok) return;
    saveProject();
    setStatus("Assignment applied to the sequence", false);
}

void Video::propagateForward() {
    int64_t start = 0;
    int count = 0;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lk(data_mu_);
        if (sel_seg >= 0 && sel_seg < static_cast<int>(segments_.size())) {
            const VidSegment& seg = segments_[static_cast<size_t>(sel_seg)];
            start = seg.start;
            count = seg.count;
            ok = true;
            upsertAssignLocked(assigns_, start, out_frame, seg_ids);
        }
    }
    (void)count;
    if (!ok) return;
    saveProject();
    setStatus("Assignment propagated forward", false);
}

void Video::applyToFrame() {
    {
        std::lock_guard<std::mutex> lk(data_mu_);
        setOverrideLocked(overrides_, cur, seg_ids);
    }
    saveProject();
    res_frame_ = -1;
    setStatus("Assignment applied to this frame only", false);
}

void Video::clearFrameOverride() {
    {
        std::lock_guard<std::mutex> lk(data_mu_);
        clearOverrideLocked(overrides_, cur);
    }
    saveProject();
    res_frame_ = -1;
}

// ---------------------------------------------------------------------------
// Studio retouch
// ---------------------------------------------------------------------------

int Video::frameEditIndexLocked(int64_t frame) const {
    for (size_t i = 0; i < edits_.size(); ++i) {
        if (edits_[i].f == frame) return static_cast<int>(i);
    }
    return -1;
}

int Video::ensureEditLocked(int64_t frame) {
    const int idx = frameEditIndexLocked(frame);
    if (idx >= 0) return idx;
    if (edits_.size() >= kMaxEdits) return -1;
    VidFrameEdit e;
    e.f = frame;
    edits_.push_back(std::move(e));
    return static_cast<int>(edits_.size() - 1);
}

bool Video::hasActiveEdit(int64_t frame) {
    std::lock_guard<std::mutex> lk(data_mu_);
    const int idx = frameEditIndexLocked(frame);
    if (idx < 0) return false;
    const VidFrameEdit& e = edits_[static_cast<size_t>(idx)];
    return !e.bypassed && !e.strokes.empty();
}

int Video::editsInRange(int64_t a, int64_t b) const {
    std::lock_guard<std::mutex> lk(data_mu_);
    int n = 0;
    for (const VidFrameEdit& e : edits_) {
        if (e.f >= a && e.f <= b) ++n;
    }
    return n;
}

void Video::undoStroke() {
    {
        std::lock_guard<std::mutex> lk(data_mu_);
        const int idx = frameEditIndexLocked(cur);
        if (idx >= 0) {
            VidFrameEdit& e = edits_[static_cast<size_t>(idx)];
            if (!e.strokes.empty()) e.strokes.pop_back();
            if (e.strokes.empty()) edits_.erase(edits_.begin() + idx);
        }
    }
    edit_gen_ += 1;
    saveProject();
}

void Video::deleteCurrentEdit() {
    bool removed = false;
    {
        std::lock_guard<std::mutex> lk(data_mu_);
        const int idx = frameEditIndexLocked(cur);
        if (idx >= 0) {
            edits_.erase(edits_.begin() + idx);
            removed = true;
        }
    }
    (void)removed;
    edit_gen_ += 1;
    saveProject();
    setStatus("Frame retouch deleted", false);
}

void Video::clearAllEdits() {
    {
        std::lock_guard<std::mutex> lk(data_mu_);
        edits_.clear();
    }
    edit_gen_ += 1;
    saveProject();
    setStatus("All frame retouches cleared", false);
}

void Video::setCurrentBypass(bool on) {
    {
        std::lock_guard<std::mutex> lk(data_mu_);
        const int idx = frameEditIndexLocked(cur);
        if (idx >= 0) edits_[static_cast<size_t>(idx)].bypassed = on;
    }
    edit_gen_ += 1;
    saveProject();
}

void Video::toggleCurrentBypass() {
    {
        std::lock_guard<std::mutex> lk(data_mu_);
        const int idx = frameEditIndexLocked(cur);
        if (idx >= 0) edits_[static_cast<size_t>(idx)].bypassed = !edits_[static_cast<size_t>(idx)].bypassed;
    }
    edit_gen_ += 1;
    saveProject();
}

Video::EditInfo Video::currentEditInfo() const {
    std::lock_guard<std::mutex> lk(data_mu_);
    const int idx = frameEditIndexLocked(cur);
    if (idx < 0) return EditInfo{};
    const VidFrameEdit& e = edits_[static_cast<size_t>(idx)];
    EditInfo info;
    info.strokes = static_cast<int>(e.strokes.size());
    info.bypassed = e.bypassed;
    return info;
}

void Video::studioBegin(float sx, float sy) {
    if (!studio) return;
    {
        std::lock_guard<std::mutex> lk(data_mu_);
        const int idx = ensureEditLocked(cur);
        if (idx >= 0) {
            VidFrameEdit& e = edits_[static_cast<size_t>(idx)];
            e.bypassed = false;
            if (e.strokes.size() < kMaxStrokesPerEdit) {
                VidStroke st;
                st.radius = studio_size;
                st.reveal = studio_erase;
                st.pts.push_back(VidStrokePt{sx, sy});
                e.strokes.push_back(std::move(st));
                stroke_active = true;
                stroke_last_x = sx;
                stroke_last_y = sy;
            }
        }
    }
    edit_gen_ += 1;
}

void Video::studioTo(float sx, float sy) {
    if (!stroke_active) return;
    const float min_d = std::max(2.0f, studio_size * 0.2f);
    const float dx = sx - stroke_last_x;
    const float dy = sy - stroke_last_y;
    if (dx * dx + dy * dy < min_d * min_d) return;
    {
        std::lock_guard<std::mutex> lk(data_mu_);
        const int idx = frameEditIndexLocked(cur);
        if (idx >= 0) {
            VidFrameEdit& e = edits_[static_cast<size_t>(idx)];
            if (!e.strokes.empty()) {
                e.strokes.back().pts.push_back(VidStrokePt{sx, sy});
                stroke_last_x = sx;
                stroke_last_y = sy;
            }
        }
    }
    edit_gen_ += 1;
}

void Video::studioEnd() {
    if (!stroke_active) return;
    stroke_active = false;
    saveProject();
}

int Video::referenceFaceIndexLocked(const VidFrameEdit& edit, const float boxes[kVideoMaxFaces][4],
                                    int count) const {
    if (count == 0) return -1;
    float cx = 0, cy = 0, n = 0;
    for (const VidStroke& st : edit.strokes) {
        for (const VidStrokePt& p : st.pts) {
            cx += p.x;
            cy += p.y;
            n += 1;
        }
    }
    if (n == 0) return -1;
    cx /= n;
    cy /= n;
    int best = -1;
    float best_d = 3.4e38f;
    for (int k = 0; k < count && k < kVideoMaxFaces; ++k) {
        const float bx = boxes[k][0] + boxes[k][2] * 0.5f;
        const float by = boxes[k][1] + boxes[k][3] * 0.5f;
        const float d = (bx - cx) * (bx - cx) + (by - cy) * (by - cy);
        if (d < best_d) {
            best_d = d;
            best = k;
        }
    }
    return best;
}

int Video::countCopyConflicts() {
    std::lock_guard<std::mutex> lk(data_mu_);
    const int src_idx = frameEditIndexLocked(cur);
    if (src_idx < 0) return 0;
    if (edits_[static_cast<size_t>(src_idx)].strokes.empty()) return 0;
    const int seg_idx = segmentIndexAtLocked(cur);
    if (seg_idx < 0) return 0;
    const VidSegment seg = segments_[static_cast<size_t>(seg_idx)];

    float rboxes[kVideoMaxFaces][4] = {};
    const int rcount = boxesForFrameLocked(cur, rboxes);
    const int rf = referenceFaceIndexLocked(edits_[static_cast<size_t>(src_idx)], rboxes, rcount);
    if (rf < 0) return 0;
    const float* rb = rboxes[rf];

    int n = 0;
    for (int64_t f = seg.start; f <= seg.end; ++f) {
        if (f == cur) continue;
        float tboxes[kVideoMaxFaces][4] = {};
        const int tcount = boxesForFrameLocked(f, tboxes);
        if (tcount == 0) continue;
        int best = -1;
        float best_d = 3.4e38f;
        for (int k = 0; k < tcount; ++k) {
            const float d = faceDistance(rb, tboxes[k]);
            if (d < best_d) {
                best_d = d;
                best = k;
            }
        }
        if (best < 0) continue;
        if (faceDistance(rb, tboxes[best]) > 1.7f) continue;
        const int ti = frameEditIndexLocked(f);
        if (ti >= 0 && !edits_[static_cast<size_t>(ti)].strokes.empty()) ++n;
    }
    return n;
}

void Video::copyToSimilar(bool overwrite) {
    int applied = 0;
    {
        std::lock_guard<std::mutex> lk(data_mu_);
        const int src_idx = frameEditIndexLocked(cur);
        if (src_idx < 0 || edits_[static_cast<size_t>(src_idx)].strokes.empty()) return;
        const int seg_idx = segmentIndexAtLocked(cur);
        if (seg_idx < 0) return;
        const VidSegment seg = segments_[static_cast<size_t>(seg_idx)];

        float rboxes[kVideoMaxFaces][4] = {};
        const int rcount = boxesForFrameLocked(cur, rboxes);
        const int rf = referenceFaceIndexLocked(edits_[static_cast<size_t>(src_idx)], rboxes, rcount);
        if (rf < 0) return;
        const float rw = std::max(1.0f, rboxes[rf][2]);
        const float rh = std::max(1.0f, rboxes[rf][3]);
        const float rcx = rboxes[rf][0] + rw * 0.5f;
        const float rcy = rboxes[rf][1] + rh * 0.5f;

        for (int64_t f = seg.start; f <= seg.end; ++f) {
            if (f == cur) continue;
            float tboxes[kVideoMaxFaces][4] = {};
            const int tcount = boxesForFrameLocked(f, tboxes);
            if (tcount == 0) continue;
            int best = -1;
            float best_d = 3.4e38f;
            for (int k = 0; k < tcount; ++k) {
                const float d = faceDistance(rboxes[rf], tboxes[k]);
                if (d < best_d) {
                    best_d = d;
                    best = k;
                }
            }
            if (best < 0) continue;
            const float* tb = tboxes[best];
            if (faceDistance(rboxes[rf], tb) > 1.7f) continue;

            const int existing = frameEditIndexLocked(f);
            if (existing >= 0 && !edits_[static_cast<size_t>(existing)].strokes.empty() && !overwrite)
                continue;

            const float tw = std::max(1.0f, tb[2]);
            const float th = std::max(1.0f, tb[3]);
            const float tcx = tb[0] + tw * 0.5f;
            const float tcy = tb[1] + th * 0.5f;
            const float sx = tw / rw;
            const float sy = th / rh;

            int tidx;
            if (existing >= 0) {
                tidx = existing;
                VidFrameEdit& tgt = edits_[static_cast<size_t>(tidx)];
                if (overwrite && !tgt.strokes.empty()) {
                    tgt.strokes.clear();
                    tgt.bypassed = false;
                }
            } else {
                if (edits_.size() >= kMaxEdits) continue;
                VidFrameEdit ne;
                ne.f = f;
                edits_.push_back(std::move(ne));
                tidx = static_cast<int>(edits_.size() - 1);
            }

            const VidFrameEdit ref = edits_[static_cast<size_t>(src_idx)];
            for (const VidStroke& st : ref.strokes) {
                VidStroke ns;
                ns.radius = st.radius;
                ns.reveal = st.reveal;
                ns.pts.reserve(st.pts.size());
                for (const VidStrokePt& p : st.pts) {
                    ns.pts.push_back(VidStrokePt{tcx + (p.x - rcx) * sx, tcy + (p.y - rcy) * sy});
                }
                edits_[static_cast<size_t>(tidx)].strokes.push_back(std::move(ns));
            }
            ++applied;
        }
    }
    edit_gen_ += 1;
    saveProject();
    char buf[96];
    std::snprintf(buf, sizeof(buf), "Retouch copied to %d frame(s)", applied);
    setStatus(buf, false);
}

std::vector<uint8_t> Video::buildEditMaskLocked(const VidFrameEdit& edit, int32_t w, int32_t h,
                                                float sx, float sy) const {
    if (w <= 0 || h <= 0) return {};
    std::vector<uint8_t> mask(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
    const float hard = std::clamp(studio_hard, 0.0f, 0.98f);
    for (const VidStroke& st : edit.strokes) {
        stampStroke(mask, w, h, st, sx, sy, hard, studio_opac);
    }
    return mask;
}

void Video::applyEditToImage(ff::Image& img, const ff::Image& orig, int32_t src_w, int32_t src_h,
                             int64_t frame) {
    if (src_w <= 0 || src_h <= 0) return;
    std::vector<uint8_t> mask;
    {
        std::lock_guard<std::mutex> lk(data_mu_);
        const int idx = frameEditIndexLocked(frame);
        if (idx < 0) return;
        const VidFrameEdit& edit = edits_[static_cast<size_t>(idx)];
        if (edit.bypassed || edit.strokes.empty()) return;
        const float sx = static_cast<float>(img.width) / static_cast<float>(src_w);
        const float sy = static_cast<float>(img.height) / static_cast<float>(src_h);
        mask = buildEditMaskLocked(edit, img.width, img.height, sx, sy);
    }
    if (mask.empty()) return;
    ff::blendMask(img, orig, mask.data(), img.width, img.height);
}

// ---------------------------------------------------------------------------
// Decode / preview
// ---------------------------------------------------------------------------

void Video::freeFaceThumbs() {
    for (int i = 0; i < kVideoMaxFaces; ++i) {
        face_thumbs[i].img.reset();
        face_thumbs[i].dirty = false;
    }
    face_thumb_seg_ = -1;
}

ff::Image makeFaceThumb(const ff::Image& img, const float box[4]) {
    const float bw = std::max(1.0f, box[2]);
    const float bh = std::max(1.0f, box[3]);
    const float cx = box[0] + bw * 0.5f;
    const float cy = box[1] + bh * 0.5f;
    const float side = std::max(bw, bh) * 1.35f;
    const float iw = static_cast<float>(img.width);
    const float ih = static_cast<float>(img.height);
    const float x0 = std::clamp(cx - side * 0.5f, 0.0f, std::max(0.0f, iw - 1.0f));
    const float y0 = std::clamp(cy - side * 0.5f, 0.0f, std::max(0.0f, ih - 1.0f));
    const float x1 = std::clamp(x0 + side, 1.0f, iw);
    const float y1 = std::clamp(y0 + side, 1.0f, ih);
    const int32_t rx = static_cast<int32_t>(x0);
    const int32_t ry = static_cast<int32_t>(y0);
    const int32_t rw = std::max(1, static_cast<int32_t>(x1 - x0));
    const int32_t rh = std::max(1, static_cast<int32_t>(y1 - y0));
    return img.resizeRegion(rx, ry, rw, rh, 72, 72);
}

void Video::ensureFaceThumbs() {
    // Do not seek the decoder away from the playback window just to refresh
    // thumbnails; they update on the next paused tick.
    if (playing_) return;
    if (face_thumbs_stale_) {
        freeFaceThumbs();
        face_thumbs_stale_ = false;
    }
    const int seg_idx = sel_seg;
    VidSegment seg;
    bool have_seg = false;
    {
        std::lock_guard<std::mutex> lk(data_mu_);
        if (seg_idx >= 0 && seg_idx < static_cast<int>(segments_.size())) {
            seg = segments_[static_cast<size_t>(seg_idx)];
            have_seg = true;
        }
    }
    if (!have_seg) {
        if (face_thumb_seg_ != -1) freeFaceThumbs();
        return;
    }
    if (face_thumb_seg_ == seg_idx) return;

    freeFaceThumbs();
    face_thumb_seg_ = seg_idx;

    if (!decoder_.ptr) return;
    ff::Image frame = decoder_.frame(seg.start);
    if (!frame.valid()) return;
    const int n = std::min(static_cast<int>(seg.count), kVideoMaxFaces);
    for (int k = 0; k < n; ++k) {
        face_thumbs[k].img = makeFaceThumb(frame, seg.boxes[k]);
        face_thumbs[k].dirty = true;
    }
}

void Video::seekTo(int64_t frame) {
    if (!open_) return;
    const int64_t max_frame = info_.frames > 0 ? info_.frames - 1 : INT64_MAX;
    cur = std::clamp<int64_t>(frame, 0, std::max<int64_t>(0, max_frame));
    res_frame_ = -1;
}

void Video::stepBy(int64_t d) {
    seekTo(cur + d);
    selectSegmentAt(cur);
}

void Video::ensureFrame() {
    if (!open_ || cur_loaded_ == cur) return;
    if (!decoder_.ptr) return;
    cur_img.reset();
    cur_img = decoder_.frame(cur);
    cur_loaded_ = cur;
    cur_dirty = true;
}

void Video::ensureResult() {
    if (!open_) return;
    const uint64_t sig = cacheSig();
    if (res_frame_ == cur && res_sig_ == sig && res_edit_gen_ == edit_gen_) return;
    res_base_.reset();
    res_img.reset();
    res_frame_ = cur;
    res_sig_ = sig;
    ff::Image im = cachedFrame(cur, sig);
    if (im.valid()) {
        res_base_ = std::move(im);
        rebuildEditedResult();
    } else {
        res_edit_gen_ = edit_gen_;
        res_img.reset();
        res_dirty = true;
    }
}

void Video::rebuildEditedResult() {
    res_img.reset();
    if (!res_base_.valid()) {
        res_edit_gen_ = edit_gen_;
        return;
    }
    if (!hasActiveEdit(cur)) {
        res_img = std::move(res_base_);
        res_edit_gen_ = edit_gen_;
        res_dirty = true;
        return;
    }
    ff::Image disp = res_base_.clone();
    if (!disp.valid()) {
        res_edit_gen_ = edit_gen_;
        return;
    }
    ensureFrame();
    if (cur_img.valid()) {
        ff::Image owned;
        const ff::Image* orig = &cur_img;
        if (cur_img.width != disp.width || cur_img.height != disp.height) {
            owned = cur_img.resized(disp.width, disp.height);
            if (owned.valid()) orig = &owned;
        }
        applyEditToImage(disp, *orig, cur_img.width, cur_img.height, cur);
    }
    res_img = std::move(disp);
    res_dirty = true;
    res_edit_gen_ = edit_gen_;
}

// ---- playback --------------------------------------------------------------

namespace {
struct Bounds {
    int64_t from = 0;
    int64_t to = 0;
};
} // namespace

static Bounds playBoundsFor(const Video& v, const std::vector<VidSegment>& segs, bool have_segs) {
    Bounds b;
    if (v.play_sequence && v.sel_seg >= 0 && have_segs) {
        b.from = segs[static_cast<size_t>(v.sel_seg)].start;
        b.to = segs[static_cast<size_t>(v.sel_seg)].end;
        return b;
    }
    b.from = v.in_frame;
    b.to = v.out_frame;
    return b;
}

void Video::startPlayback() {
    if (!open_ || playing_) return;
    Bounds b;
    {
        std::lock_guard<std::mutex> lk(data_mu_);
        b = playBoundsFor(*this, segments_, !segments_.empty());
    }
    if (cur < b.from || cur >= b.to) {
        cur = b.from;
        res_frame_ = -1;
    }
    playing_ = true;
    play_t0_ = ff::nowNs();
    play_from_ = cur;
}

void Video::stopPlayback() { playing_ = false; }

void Video::advancePlayback() {
    if (!playing_ || !open_) return;
    Bounds b;
    {
        std::lock_guard<std::mutex> lk(data_mu_);
        b = playBoundsFor(*this, segments_, !segments_.empty());
    }
    const double fps = info_.fps > 0 ? info_.fps : 25.0;
    const double dt = static_cast<double>(ff::nowNs() - play_t0_) / 1e9;
    const int64_t target = play_from_ + static_cast<int64_t>(std::floor(dt * fps));
    if (target <= cur) return;
    if (target > b.to) {
        if (loop_play) {
            play_from_ = b.from;
            play_t0_ = ff::nowNs();
            seekTo(b.from);
            ensureFrame();
            ensureResult();
            selectSegmentAt(cur);
        } else {
            stopPlayback();
        }
        return;
    }
    seekTo(target);
    if (playing_) {
        ensureFrame();
        ensureResult();
        selectSegmentAt(cur);
    }
}

// ---------------------------------------------------------------------------
// Frame processing
// ---------------------------------------------------------------------------

Video::RenderResult Video::renderFrame(ff::Engine& engine, const ff::Image& src, int64_t frame,
                                       VideoQuality quality) {
    RenderResult result;
    const float lng = static_cast<float>(std::max(src.width, src.height));
    const bool downscale = quality == VideoQuality::Low && lng > 640.0f;
    const float f = downscale ? 640.0f / lng : 1.0f;
    const int32_t ww = std::max(1, static_cast<int32_t>(static_cast<float>(src.width) * f));
    const int32_t wh = std::max(1, static_cast<int32_t>(static_cast<float>(src.height) * f));

    ff::Image work_owned;
    const ff::Image* work = &src;
    if (downscale) {
        work_owned = src.resized(ww, wh);
        if (!work_owned.valid()) return result;
        work = &work_owned;
    }
    const int32_t W = work->width;
    const int32_t H = work->height;
    const size_t bytes = static_cast<size_t>(W) * static_cast<size_t>(H) * 3u;

    ff_face faces[kVideoMaxFaces];
    int n = 0;
    if (!engine.detect(*work, faces, kVideoMaxFaces, &n)) n = 0;
    if (n > kVideoMaxFaces) n = kVideoMaxFaces;
    int32_t ids[kVideoMaxFaces];
    idsForFrame(frame, n, ids);

    std::vector<uint8_t> ping(bytes);
    std::vector<uint8_t> pong(bytes);
    std::memcpy(ping.data(), work->data, bytes);

    ff::Image cur = ff::Image::borrow(ping.data(), W, H, W * 3);
    ff::Image next = ff::Image::borrow(pong.data(), W, H, W * 3);

    const int id_count = host_ ? host_->identityCount() : 0;
    const int def = host_ ? host_->defaultIdentity() : 0;
    const bool mouth = host_ ? host_->restore_mouth : true;
    engine.setRestoreMouth(mouth);

    ff_face act_faces[kVideoMaxFaces];
    int act_n = 0;

    for (int i = 0; i < n; ++i) {
        const int32_t chosen = ids[i];
        if (chosen == kVideoSkipId) continue;
        int id;
        if (chosen >= 0) {
            id = chosen;
        } else if (def < id_count) {
            id = def;
        } else {
            continue;
        }
        if (id >= id_count) continue;

        float embedding[512];
        if (!host_ || !host_->identityEmbedding(id, embedding)) continue;

        double matrix[6] = {0, 0, 0, 0, 0, 0};
        bool swap_ok = false;
        ff::Image patch = engine.swapPatch(cur, embedding, faces[i], matrix, &swap_ok);
        if (!swap_ok || !patch.valid()) continue;
        ff::Image canvas = ff::Image::borrow(cur.data, cur.width, cur.height, cur.stride);
        if (!ff::pastePatch(canvas, patch, matrix, &next)) continue;
        // Swap the ping/pong roles.
        std::swap(ping, pong);
        cur = ff::Image::borrow(ping.data(), W, H, W * 3);
        next = ff::Image::borrow(pong.data(), W, H, W * 3);
        act_faces[act_n++] = faces[i];
    }

    if (quality == VideoQuality::High && act_n > 0 && !enhancer_failed && job_enhancer_ != 0) {
        engine.setEnhancer(job_enhancer_);
        ff::Image en = ff::Image::borrow(pong.data(), W, H, W * 3);
        std::vector<uint8_t> ebuf(bytes);
        // enhance writes into en->data; the output must be a separate buffer.
        en.data = ebuf.data();
        en.stride = W * 3;
        const bool enh_ok = engine.enhance(cur, act_faces, act_n, &en);
        if (enh_ok) {
            std::memcpy(cur.data, ebuf.data(), bytes);
        } else {
            enhancer_failed = true;
            engine.setEnhancer(0);
        }
    } else {
        engine.setEnhancer(0);
    }

    // Match the pristine frame's grain on the restored faces so a smoothed
    // GFPGAN / swapper face blends into a noisy or compressed clip. High quality
    // (export) only.
    if (quality == VideoQuality::High && act_n > 0 && job_grain_match_ > 0) {
        ff::Image ref = ff::Image::borrow(work->data, W, H, work->stride);
        std::vector<uint8_t> gbuf(bytes);
        ff::Image gm = ff::Image::borrow(gbuf.data(), W, H, W * 3);
        if (engine.grainMatch(cur, ref, act_faces, act_n, job_grain_match_, &gm)) {
            std::memcpy(cur.data, gbuf.data(), bytes);
        }
    }

    ff::Image packed = ff::Image::borrow(cur.data, W, H, W * 3);
    result.img = packed.clone();
    result.scale = f;
    return result;
}

bool Video::processAndCache(ff::Engine& engine, ff::Video& video, int64_t frame, VideoQuality q,
                            uint64_t sig, bool use_cache) {
    const std::string path = framePathInto(q, sig, frame);
    if (use_cache && ff::fileExists(path.c_str())) return true;
    ff::Image src = video.frame(frame);
    if (!src.valid()) return false;
    RenderResult r = renderFrame(engine, src, frame, q);
    if (!r.img.valid()) return false;
    if (!r.img.save(path.c_str())) return false;
    return true;
}

ff::Image Video::renderExportFrame(ff::Engine& engine, ff::Video& video, int64_t frame,
                                   uint64_t sig) {
    ff::Image img;
    const std::string hp = framePathInto(VideoQuality::High, sig, frame);
    if (ff::fileExists(hp.c_str())) {
        img = ff::Image::load(hp.c_str());
    }
    if (!img.valid()) {
        ff::Image src = video.frame(frame);
        if (!src.valid()) return {};
        RenderResult r = renderFrame(engine, src, frame, VideoQuality::High);
        img = std::move(r.img);
    }
    if (hasActiveEdit(frame) && img.valid()) {
        ff::Image src = video.frame(frame);
        if (src.valid()) {
            ff::Image owned;
            const ff::Image* orig = &src;
            if (src.width != img.width || src.height != img.height) {
                owned = src.resized(img.width, img.height);
                if (owned.valid()) orig = &owned;
            }
            applyEditToImage(img, *orig, src.width, src.height, frame);
        }
    }
    return img;
}

void Video::ensureCacheDirs() {
    if (cache_root_.empty()) return;
    ff::mkdirs(cache_root_.c_str());
    const uint64_t sig = cacheSig();
    ff::mkdirs(cacheDirInto(VideoQuality::Low, sig).c_str());
    ff::mkdirs(cacheDirInto(VideoQuality::High, sig).c_str());
}

// ---------------------------------------------------------------------------
// Jobs
// ---------------------------------------------------------------------------

void Video::setStatus(const std::string& text, bool is_error) {
    status = text;
    status_err = is_error;
}

void Video::setJobError(const std::string& message) { job_msg_ = message; }

bool Video::spawn(VideoJob kind) {
    if (!open_) {
        setStatus("Open a video first", true);
        return false;
    }
    if (job != VideoJob::None) {
        setStatus("A video job is already running", true);
        return false;
    }
    if (host_ && host_->busy()) {
        setStatus("Wait for the current photo job to finish", true);
        return false;
    }
    // Analysis only detects faces and stores samples; identities are needed
    // when a swap is actually rendered (process/export).
    if ((kind == VideoJob::Process || kind == VideoJob::Encode) &&
        (!host_ || host_->identityCount() == 0)) {
        setStatus("Add an identity image first", true);
        return false;
    }
    ensureCacheDirs();
    job = kind;
    cancel_.store(false, std::memory_order_release);
    done_.store(false, std::memory_order_release);
    ok_.store(false, std::memory_order_release);
    progress_atomic_.store(0, std::memory_order_relaxed);
    job_done_atomic_.store(0, std::memory_order_relaxed);
    job_total_atomic_.store(1, std::memory_order_relaxed);
    job_msg_.clear();
    err.clear();
    enhancer_failed = false;
    job_enhancer_ = host_ ? static_cast<int>(host_->enhancer) : 0;
    job_grain_match_ = host_ ? host_->grain_match : 0;
    job_restore_mouth_ = host_ ? host_->restore_mouth : true;

    host_->setVideoBusy(true);
    thread_ = std::thread([this, kind]() { jobMain(kind); });
    return true;
}

ff::Video Video::openJobVideo() {
    ff::Video v;
    std::string e;
    if (!v.open(path_.c_str(), &e)) {
        setJobError("Cannot reopen the video");
        return {};
    }
    return v;
}

void Video::jobMain(VideoJob kind) {
    bool ok = true;
    switch (kind) {
        case VideoJob::Analyze: ok = runAnalyze(); break;
        case VideoJob::Process: ok = runProcess(); break;
        case VideoJob::Encode: ok = runExport(); break;
        case VideoJob::None: break;
    }
    ok_.store(ok, std::memory_order_release);
    done_.store(true, std::memory_order_release);
}

bool Video::runAnalyze() {
    ff::Video video = openJobVideo();
    if (!video.ptr) return false;
    ff::Engine* engine = host_->ensureVideoEngine(true);
    if (!engine) {
        setJobError(host_->videoEngineError().empty() ? "Cannot load the models"
                                                      : host_->videoEngineError());
        return false;
    }
    uint32_t total = 0;
    for (int64_t f = job_from_; f <= job_to_; f += job_chunk_) total += 1;
    job_total_atomic_.store(std::max<uint32_t>(1, total), std::memory_order_relaxed);
    uint32_t done_chunks = 0;

    std::vector<VidSample> batch;
    for (int64_t start = job_from_; start <= job_to_ && !cancel_.load(std::memory_order_acquire);
         start += job_chunk_) {
        const int64_t end = std::min(start + job_chunk_ - 1, job_to_);
        if (chunkDone(start, end)) {
            done_chunks += 1;
            job_done_atomic_.store(done_chunks, std::memory_order_relaxed);
            continue;
        }
        batch.clear();
        for (int64_t fi = start;
             fi <= end && !cancel_.load(std::memory_order_acquire); fi += job_stride_) {
            waitWhilePaused();
            ff::Image src = video.frame(fi);
            if (!src.valid()) continue;
            ff_face faces[kVideoMaxFaces];
            int n = 0;
            if (!engine->detect(src, faces, kVideoMaxFaces, &n)) n = 0;
            if (n > kVideoMaxFaces) n = kVideoMaxFaces;
            VidSample smp;
            smp.f = fi;
            smp.n = static_cast<uint8_t>(n);
            for (int k = 0; k < n; ++k) {
                smp.b[k][0] = faces[k].bbox[0];
                smp.b[k][1] = faces[k].bbox[1];
                smp.b[k][2] = faces[k].bbox[2] - faces[k].bbox[0];
                smp.b[k][3] = faces[k].bbox[3] - faces[k].bbox[1];
            }
            batch.push_back(smp);
        }
        if (!batch.empty()) {
            mergeSamples(batch, start, end);
            saveProject();
        }
        if (!cancel_.load(std::memory_order_acquire)) {
            markChunk(start, end);
            done_chunks += 1;
            job_done_atomic_.store(done_chunks, std::memory_order_relaxed);
            progress_atomic_.store(
                static_cast<uint32_t>(std::min<int64_t>(
                    1000, static_cast<int64_t>(done_chunks) * 1000 / std::max<int64_t>(1, total))),
                std::memory_order_relaxed);
        }
    }
    return true;
}

bool Video::runProcess() {
    ff::Video video = openJobVideo();
    if (!video.ptr) return false;
    ff::Engine* engine = host_->ensureVideoEngine(true);
    if (!engine) {
        setJobError(host_->videoEngineError().empty() ? "Cannot load the models"
                                                      : host_->videoEngineError());
        return false;
    }
    const VideoQuality q = job_quality_;
    const uint64_t sig = cacheSig();
    const int64_t span = std::max<int64_t>(1, job_to_ - job_from_ + 1);
    job_total_atomic_.store(static_cast<uint32_t>(span), std::memory_order_relaxed);
    uint32_t done = 0;
    for (int64_t f = job_from_; f <= job_to_ && !cancel_.load(std::memory_order_acquire); ++f) {
        waitWhilePaused();
        processAndCache(*engine, video, f, q, sig, job_use_cache_);
        done += 1;
        job_done_atomic_.store(done, std::memory_order_relaxed);
        progress_atomic_.store(
            static_cast<uint32_t>(std::min<int64_t>(1000, static_cast<int64_t>(done) * 1000 / span)),
            std::memory_order_relaxed);
    }
    return true;
}

bool Video::runExport() {
    ff::Video video = openJobVideo();
    if (!video.ptr) return false;
    ff::Engine* engine = host_->ensureVideoEngine(true);
    if (!engine) {
        setJobError(host_->videoEngineError().empty() ? "Cannot load the models"
                                                      : host_->videoEngineError());
        return false;
    }
    const std::string out = job_out_;
    const uint64_t sig = cacheSig();
    const int32_t w = video.info().width;
    const int32_t h = video.info().height;
    const double fps = info_.fps > 0 ? info_.fps : 25.0;
    const int64_t total = std::max<int64_t>(1, job_to_ - job_from_ + 1);
    const int64_t chunk = std::max<int64_t>(1, job_chunk_);
    job_total_atomic_.store(static_cast<uint32_t>(total), std::memory_order_relaxed);

    const std::string configured = host_ ? host_->temp_dir : std::string();
    std::string video_out;
    if (job_audio_) {
        if (!configured.empty() && !cache_root_.empty()) {
            ensureCacheDirs();
            video_out = cache_root_ + "/mux_video.mp4";
        } else {
            video_out = out + ".video.mp4";
        }
    } else {
        video_out = out;
    }

    if (job_chunks_) {
        ensureCacheDirs();
        size_t part_index = 0;
        int64_t f = job_from_;
        while (f <= job_to_ && !cancel_.load(std::memory_order_acquire)) {
            char pbuf[64];
            std::snprintf(pbuf, sizeof(pbuf), "/part_%04zu.mp4", part_index);
            const std::string part_path = cache_root_ + pbuf;
            ff::VideoWriter writer;
            std::string werr;
            if (!writer.open(part_path.c_str(), w, h, fps, "libx264", 16, "veryfast", "ffmpeg",
                             &werr)) {
                setJobError("Cannot start the video encoder (ffmpeg)");
                return false;
            }
            const int64_t part_end = std::min(f + chunk - 1, job_to_);
            for (; f <= part_end && !cancel_.load(std::memory_order_acquire); ++f) {
                waitWhilePaused();
                ff::Image frame_img = renderExportFrame(*engine, video, f, sig);
                if (!frame_img.valid()) continue;
                if (frame_img.width != w || frame_img.height != h) {
                    ff::Image fixed = frame_img.resized(w, h);
                    if (!fixed.valid()) continue;
                    frame_img = std::move(fixed);
                }
                if (!writer.write(frame_img)) {
                    setJobError("Encoder rejected a frame");
                    return false;
                }
                job_done_atomic_.store(static_cast<uint32_t>(f - job_from_ + 1),
                                       std::memory_order_relaxed);
                progress_atomic_.store(
                    static_cast<uint32_t>(std::min<int64_t>(
                        1000, (f - job_from_ + 1) * 1000 / total)),
                    std::memory_order_relaxed);
            }
            std::string cerr;
            if (!writer.close(&cerr)) {
                setJobError("ffmpeg failed while writing a part");
                return false;
            }
            ++part_index;
        }
        if (cancel_.load(std::memory_order_acquire)) return false;

        std::string list;
        for (size_t k = 0; k < part_index; ++k) {
            char pbuf[64];
            std::snprintf(pbuf, sizeof(pbuf), "/part_%04zu.mp4", k);
            list += "file '" + cache_root_ + pbuf + "'\n";
        }
        const std::string list_path = cache_root_ + "/concat.txt";
        if (!ff::writeFile(list_path.c_str(), reinterpret_cast<const uint8_t*>(list.data()),
                           list.size())) {
            setJobError("Cannot write the concat list");
            return false;
        }
        const std::string video_only = job_audio_ ? video_out : out;
        if (!ff::concatVideos(list_path.c_str(), video_only.c_str(), "ffmpeg")) {
            setJobError("Cannot concatenate the exported parts");
            return false;
        }
        if (job_audio_) {
            const double start_sec = static_cast<double>(job_from_) / fps;
            const double dur = static_cast<double>(job_to_ - job_from_ + 1) / fps;
            if (!ff::muxAudio(video_only.c_str(), path_.c_str(), start_sec, dur, out.c_str(),
                              "ffmpeg")) {
                setJobError("Video written, but muxing the audio failed");
                return false;
            }
            ff::removeFile(video_only.c_str());
        }
        return true;
    }

    ff::VideoWriter writer;
    std::string werr;
    for (;;) {
        if (cancel_.load(std::memory_order_acquire)) return false;
        if (!paused_.load(std::memory_order_acquire)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (!writer.open(video_out.c_str(), w, h, fps, "libx264", 16, "veryfast", "ffmpeg", &werr)) {
        setJobError("Cannot start the video encoder (ffmpeg)");
        return false;
    }
    uint32_t done = 0;
    for (int64_t f = job_from_; f <= job_to_ && !cancel_.load(std::memory_order_acquire); ++f) {
        ff::Image frame_img = renderExportFrame(*engine, video, f, sig);
        if (!frame_img.valid()) continue;
        if (frame_img.width != w || frame_img.height != h) {
            ff::Image fixed = frame_img.resized(w, h);
            if (!fixed.valid()) continue;
            frame_img = std::move(fixed);
        }
        if (!writer.write(frame_img)) {
            setJobError("Encoder rejected a frame");
            return false;
        }
        waitWhilePaused();
        done += 1;
        job_done_atomic_.store(done, std::memory_order_relaxed);
        progress_atomic_.store(
            static_cast<uint32_t>(std::min<int64_t>(1000, static_cast<int64_t>(done) * 1000 / total)),
            std::memory_order_relaxed);
    }
    std::string cerr;
    if (!writer.close(&cerr)) {
        setJobError("ffmpeg failed while encoding");
        return false;
    }
    if (cancel_.load(std::memory_order_acquire)) return false;
    if (job_audio_) {
        const double start_sec = static_cast<double>(job_from_) / fps;
        const double dur = static_cast<double>(job_to_ - job_from_ + 1) / fps;
        if (!ff::muxAudio(video_out.c_str(), path_.c_str(), start_sec, dur, out.c_str(), "ffmpeg")) {
            setJobError("Video written, but muxing the audio failed");
            return false;
        }
        ff::removeFile(video_out.c_str());
    }
    return true;
}

// ---------------------------------------------------------------------------
// Job polling
// ---------------------------------------------------------------------------

void Video::poll() {
    advancePlayback();
    progress = progress_atomic_.load(std::memory_order_relaxed);
    job_done_frames = job_done_atomic_.load(std::memory_order_relaxed);
    job_total_frames = job_total_atomic_.load(std::memory_order_relaxed);

    if (job != VideoJob::None && done_.load(std::memory_order_acquire)) {
        if (thread_.joinable()) thread_.join();
        const bool ok = ok_.load(std::memory_order_acquire);
        const bool cancelled = cancel_.load(std::memory_order_acquire);
        const VideoJob kind = job;
        job = VideoJob::None;
        done_.store(false, std::memory_order_release);
        if (host_) {
            host_->setVideoBusy(false);
            if (host_->engine.ptr) provider = host_->engine.provider();
        }

        if (kind == VideoJob::Analyze) {
            rebuildSegments();
            selectSegmentAt(cur);
        }
        if (kind == VideoJob::Process || kind == VideoJob::Encode) {
            res_frame_ = -1;
            ensureResult();
        }
        if (!ok) {
            setStatus(job_msg_.empty() ? "Video job failed" : job_msg_, true);
        } else if (cancelled) {
            setStatus("Cancelled \xC2\xB7 finished work was saved", false);
        } else {
            switch (kind) {
                case VideoJob::Analyze: {
                    char buf[96];
                    std::snprintf(buf, sizeof(buf), "Analysis finished \xC2\xB7 %d sequence(s)",
                                  segmentCount());
                    setStatus(buf, false);
                    break;
                }
                case VideoJob::Process: setStatus("Frames processed and cached", false); break;
                case VideoJob::Encode:
                    setStatus("Exported " + baseName(job_out_), false);
                    break;
                case VideoJob::None: break;
            }
        }
        saveProject();
        cancel_.store(false, std::memory_order_release);
    }

    autotestStep();

    // A requested close only runs once the worker has stopped, so it never
    // blocks the UI thread.
    if (close_requested && job == VideoJob::None) {
        close_requested = false;
        close();
    }
    if (!open_) return;

    ensureFrame();
    ensureResult();
    ensureFaceThumbs();
}

void Video::startAnalyze() {
    job_from_ = in_frame;
    job_to_ = out_frame;
    job_chunk_ = std::max<int64_t>(1, static_cast<int64_t>(std::llround(chunk_f)));
    job_stride_ = std::max<int64_t>(1, static_cast<int64_t>(std::llround(stride_f)));
    spawn(VideoJob::Analyze);
}

void Video::startProcessFrame() {
    job_from_ = cur;
    job_to_ = cur;
    job_quality_ = low_quality ? VideoQuality::Low : VideoQuality::High;
    job_use_cache_ = process_use_cache;
    job_chunk_ = std::max<int64_t>(1, static_cast<int64_t>(std::llround(chunk_f)));
    job_stride_ = 1;
    spawn(VideoJob::Process);
}

void Video::startProcessRange(int64_t from, int64_t to) {
    job_from_ = std::clamp<int64_t>(from, 0, out_frame);
    job_to_ = std::clamp<int64_t>(to, job_from_, out_frame);
    job_quality_ = low_quality ? VideoQuality::Low : VideoQuality::High;
    job_use_cache_ = process_use_cache;
    job_chunk_ = std::max<int64_t>(1, static_cast<int64_t>(std::llround(chunk_f)));
    job_stride_ = 1;
    spawn(VideoJob::Process);
}

void Video::startProcessSequence() {
    int64_t start = 0, end = 0;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lk(data_mu_);
        if (sel_seg >= 0 && sel_seg < static_cast<int>(segments_.size())) {
            start = segments_[static_cast<size_t>(sel_seg)].start;
            end = segments_[static_cast<size_t>(sel_seg)].end;
            ok = true;
        }
    }
    if (ok) startProcessRange(start, end);
    else startProcessFrame();
}

void Video::startProcessChunk() {
    const int64_t chunk = std::max<int64_t>(1, static_cast<int64_t>(std::llround(chunk_f)));
    const int64_t start = in_frame + ((cur - in_frame) / chunk) * chunk;
    startProcessRange(start, std::min(start + chunk - 1, out_frame));
}

void Video::cancel() {
    // Signal the worker and unpause it; poll() performs the join and cleanup
    // once the worker sets done_. Do not join here.
    paused_.store(false, std::memory_order_release);
    cancel_.store(true, std::memory_order_release);
}

void Video::pause() {
    if (job == VideoJob::None) {
        return;
    }
    paused_.store(true, std::memory_order_release);
}

void Video::resume() {
    if (job == VideoJob::None) {
        return;
    }
    paused_.store(false, std::memory_order_release);
}

void Video::togglePause() {
    if (job == VideoJob::None) {
        return;
    }
    paused_.store(!paused_.load(std::memory_order_acquire), std::memory_order_release);
}

void Video::waitWhilePaused() {
    // Do not advance the frame index while paused: sleep until resumed or
    // cancelled. cancel() clears paused_, so this always terminates.
    while (paused_.load(std::memory_order_acquire) &&
           !cancel_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void Video::clearAnalysis() {
    {
        std::lock_guard<std::mutex> lk(data_mu_);
        samples_.clear();
        chunks_.clear();
        segments_.clear();
    }
    saveProject();
    setStatus("Analysis cleared", false);
}

void Video::startExport(bool whole) {
    if (!open_) return;
    job_from_ = whole ? 0 : in_frame;
    job_to_ = whole ? std::max<int64_t>(0, info_.frames - 1) : out_frame;
    job_audio_ = export_audio;
    job_chunks_ = export_chunks;
    job_chunk_ = std::max<int64_t>(1, static_cast<int64_t>(std::llround(chunk_f)));
    if (spawn(VideoJob::Encode)) setStatus("Exporting...", false);
}

void Video::startExportTo(const std::string& out, int64_t from, int64_t to, bool audio,
                          bool chunks) {
    if (!open_) return;
    job_out_ = out;
    job_from_ = from;
    job_to_ = to;
    job_audio_ = audio;
    job_chunks_ = chunks;
    job_chunk_ = std::max<int64_t>(1, static_cast<int64_t>(std::llround(chunk_f)));
    if (spawn(VideoJob::Encode)) setStatus("Exporting...", false);
}

bool Video::frameCachedLow(int64_t frame) const {
    const std::string p = framePathInto(VideoQuality::Low, cacheSig(), frame);
    return ff::fileExists(p.c_str());
}

bool Video::frameCachedHigh(int64_t frame) const {
    const std::string p = framePathInto(VideoQuality::High, cacheSig(), frame);
    return ff::fileExists(p.c_str());
}

void Video::forgetFrameCache() {
    const uint64_t sig = cacheSig();
    ff::removeFile(framePathInto(VideoQuality::Low, sig, cur).c_str());
    ff::removeFile(framePathInto(VideoQuality::High, sig, cur).c_str());
    res_frame_ = -1;
    setStatus("Cache entry removed for this frame", false);
}

// ---------------------------------------------------------------------------
// Autotest
// ---------------------------------------------------------------------------

void Video::testFinish() {
    if (test_keep_) {
        seekTo(std::min<int64_t>(3, job_to_));
        test_state_ = VideoTestState::Done;
    } else {
        test_state_ = VideoTestState::Done;
        test_done_.store(true, std::memory_order_release);
    }
}

void Video::autotestStep() {
    switch (test_state_) {
        case VideoTestState::Off:
        case VideoTestState::Done:
            return;
        case VideoTestState::Start: {
            if (!host_ || host_->busy()) return;
            if (host_->identityCount() == 0) return;
            if (test_path_.empty()) {
                test_state_ = VideoTestState::Done;
                return;
            }
            std::printf("[videotest] opening %s\n", test_path_.c_str());
            openPath(test_path_);
            in_frame = 0;
            out_frame = std::min<int64_t>(12, std::max<int64_t>(0, info_.frames - 1));
            startAnalyze();
            test_state_ = VideoTestState::WaitAnalyze;
            return;
        }
        case VideoTestState::WaitAnalyze: {
            if (job != VideoJob::None) return;
            int seg0 = 0;
            {
                std::lock_guard<std::mutex> lk(data_mu_);
                if (!segments_.empty()) seg0 = segments_[0].count;
            }
            std::printf("[videotest] analysis done: samples=%d segments=%d faces0=%d\n",
                        sampleCount(), segmentCount(), seg0);
            for (int k = 0; k < kVideoMaxFaces; ++k) seg_ids[k] = -1;
            startProcessRange(0, std::min<int64_t>(4, out_frame));
            test_state_ = VideoTestState::WaitProcess;
            return;
        }
        case VideoTestState::WaitProcess: {
            if (job != VideoJob::None) return;
            std::printf("[videotest] processed range done, exporting\n");
            job_out_ = "/tmp/opencode/vtest/gui_out.mp4";
            job_from_ = 0;
            job_to_ = std::min<int64_t>(4, out_frame);
            job_audio_ = true;
            job_chunks_ = true;
            if (!spawn(VideoJob::Encode)) {
                std::printf("[videotest] FAILED to start export\n");
                test_state_ = VideoTestState::Done;
                return;
            }
            test_state_ = VideoTestState::WaitExport;
            return;
        }
        case VideoTestState::WaitExport: {
            if (job != VideoJob::None) return;
            const bool exists = ff::fileExists(job_out_.c_str());
            std::printf("[videotest] %s: export %s\n", exists ? "OK" : "FAILED", job_out_.c_str());
            int seg_count = 0;
            {
                std::lock_guard<std::mutex> lk(data_mu_);
                seg_count = static_cast<int>(segments_.size());
            }
            if (seg_count > 0) {
                int64_t start = 0;
                {
                    std::lock_guard<std::mutex> lk(data_mu_);
                    start = segments_[0].start;
                }
                sel_seg = 0;
                play_sequence = true;
                loop_play = true;
                seekTo(start);
                selectSegmentAt(cur);
                startPlayback();
                test_play_moves_ = 0;
                test_play_last_ = cur;
                test_play_budget_ = 900;
                test_state_ = VideoTestState::WaitPlay;
                return;
            }
            testFinish();
            return;
        }
        case VideoTestState::WaitPlay: {
            int64_t seg_start = 0, seg_end = 0;
            {
                std::lock_guard<std::mutex> lk(data_mu_);
                if (!segments_.empty()) {
                    seg_start = segments_[0].start;
                    seg_end = segments_[0].end;
                }
            }
            if (cur < seg_start || cur > seg_end) {
                std::printf("[videotest] FAILED: playback left sequence bounds (cur=%lld, seg=%lld-%lld)\n",
                            static_cast<long long>(cur), static_cast<long long>(seg_start),
                            static_cast<long long>(seg_end));
                stopPlayback();
                play_sequence = false;
                testFinish();
                return;
            }
            if (cur != test_play_last_) {
                test_play_moves_ += 1;
                test_play_last_ = cur;
            }
            test_play_budget_ -= 1;
            if (test_play_moves_ < 3 && test_play_budget_ > 0) return;
            const bool ok = test_play_moves_ >= 3;
            std::printf("[videotest] %s: playback advanced %d frame(s) inside sequence %lld-%lld\n",
                        ok ? "OK" : "FAILED", test_play_moves_,
                        static_cast<long long>(seg_start), static_cast<long long>(seg_end));
            stopPlayback();
            play_sequence = false;
            testFinish();
            return;
        }
    }
}

} // namespace app
