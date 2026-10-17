#include "rql/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <locale>
#include <sstream>

#if defined(RQL_HAVE_FLOAT_CHARCONV)
#include <charconv>
#endif

namespace rql {

// ------------------------------------------------------------------ numbers

namespace {

constexpr double kMaxSafeInt = 9007199254740992.0;  // 2^53

#if !defined(RQL_HAVE_FLOAT_CHARCONV)
std::string printf_fixup(const char* buf) {
    std::string s(buf);
    // Neutralize a non-"C" LC_NUMERIC locale (decimal comma).
    for (auto& c : s) {
        if (c == ',') c = '.';
    }
    return s;
}
#endif

}  // namespace

bool parse_double(std::string_view text, double& out) {
    if (text.empty()) return false;
#if defined(RQL_HAVE_FLOAT_CHARCONV)
    const char* b = text.data();
    const char* e = b + text.size();
    if (*b == '+') return false;
    auto r = std::from_chars(b, e, out);
    return r.ec == std::errc() && r.ptr == e;
#else
    std::istringstream in{std::string(text)};
    in.imbue(std::locale::classic());
    double d = 0;
    in >> d;
    if (in.fail()) return false;
    in.peek();
    if (!in.eof()) return false;
    out = d;
    return true;
#endif
}

std::string format_double(double d) {
    if (std::isnan(d)) return "NaN";
    if (std::isinf(d)) return d > 0 ? "Infinity" : "-Infinity";
    if (std::fabs(d) < kMaxSafeInt && std::floor(d) == d) {
        long long v = static_cast<long long>(d);
        return std::to_string(v);
    }
#if defined(RQL_HAVE_FLOAT_CHARCONV)
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, d);
    return std::string(buf, r.ptr);
#else
    char buf[64];
    for (int p = 1; p <= 17; ++p) {
        std::snprintf(buf, sizeof buf, "%.*g", p, d);
        std::string s = printf_fixup(buf);
        double back = 0;
        if (parse_double(s, back) && back == d) return s;
    }
    std::snprintf(buf, sizeof buf, "%.17g", d);
    return printf_fixup(buf);
#endif
}

std::string format_number_text(double d) {
    if (std::isnan(d)) return "NaN";
    if (std::isinf(d)) return d > 0 ? "Infinity" : "-Infinity";
    if (d == 0) return std::signbit(d) ? "-0" : "0";
    std::string sci;
#if defined(RQL_HAVE_FLOAT_CHARCONV)
    {
        char buf[64];
        auto r = std::to_chars(buf, buf + sizeof buf, d, std::chars_format::scientific);
        sci.assign(buf, r.ptr);
    }
#else
    {
        char buf[64];
        for (int p = 0; p <= 16; ++p) {
            std::snprintf(buf, sizeof buf, "%.*e", p, d);
            sci = printf_fixup(buf);
            double back = 0;
            if (parse_double(sci, back) && back == d) break;
        }
    }
#endif
    // sci: [-]D[.DDD]e[+-]XX
    bool neg = sci[0] == '-';
    size_t pos = neg ? 1 : 0;
    size_t epos = sci.find_first_of("eE");
    std::string digits;
    for (size_t i = pos; i < epos; ++i)
        if (sci[i] != '.') digits.push_back(sci[i]);
    int exp10 = std::atoi(sci.c_str() + epos + 1);
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    std::string out = neg ? "-" : "";
    if (exp10 > -5 && exp10 < 15) {
        if (exp10 >= 0) {
            size_t intLen = static_cast<size_t>(exp10) + 1;
            if (digits.size() <= intLen) {
                out += digits;
                out.append(intLen - digits.size(), '0');
            } else {
                out += digits.substr(0, intLen);
                out += '.';
                out += digits.substr(intLen);
            }
        } else {
            out += "0.";
            out.append(static_cast<size_t>(-exp10 - 1), '0');
            out += digits;
        }
    } else {
        out += digits[0];
        if (digits.size() > 1) {
            out += '.';
            out += digits.substr(1);
        }
        out += 'E';
        out += exp10 < 0 ? '-' : '+';
        int a = exp10 < 0 ? -exp10 : exp10;
        if (a < 10) out += '0';
        out += std::to_string(a);
    }
    return out;
}

std::string format_float(float f) {
    if (std::isnan(f)) return "NaN";
    if (std::isinf(f)) return f > 0 ? "Infinity" : "-Infinity";
#if defined(RQL_HAVE_FLOAT_CHARCONV)
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, f);
    return std::string(buf, r.ptr);
#else
    char buf[64];
    for (int p = 1; p <= 9; ++p) {
        std::snprintf(buf, sizeof buf, "%.*g", p, static_cast<double>(f));
        std::string s = printf_fixup(buf);
        double back = 0;
        if (parse_double(s, back) && static_cast<float>(back) == f) return s;
    }
    std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(f));
    return printf_fixup(buf);
#endif
}

// ------------------------------------------------------------------ object helpers

Json& Json::set(std::string_view key, Json v) {
    for (size_t i = keys_.size(); i-- > 0;) {
        if (keys_[i] == key) {
            items_[i] = std::move(v);
            return items_[i];
        }
    }
    keys_.emplace_back(key);
    items_.push_back(std::move(v));
    return items_.back();
}

const Json* Json::find(std::string_view key) const {
    if (type_ != Type::Object) return nullptr;
    for (size_t i = keys_.size(); i-- > 0;) {
        if (keys_[i] == key) return &items_[i];
    }
    return nullptr;
}

Json* Json::find(std::string_view key) {
    return const_cast<Json*>(static_cast<const Json*>(this)->find(key));
}

bool Json::erase(std::string_view key) {
    bool any = false;
    for (size_t i = keys_.size(); i-- > 0;) {
        if (keys_[i] == key) {
            keys_.erase(keys_.begin() + static_cast<std::ptrdiff_t>(i));
            items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(i));
            any = true;
        }
    }
    return any;
}

// ------------------------------------------------------------------ writer

void json_escape_to(std::string& out, std::string_view s) {
    static const char* hex = "0123456789abcdef";
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(hex[c >> 4]);
                    out.push_back(hex[c & 15]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

namespace {
void newline(std::string& out, int indent, int level) {
    if (indent < 0) return;
    out.push_back('\n');
    out.append(static_cast<size_t>(indent * level), ' ');
}
}  // namespace

void Json::dump_to(std::string& out, int indent, int level) const {
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += b_ ? "true" : "false"; break;
        case Type::Number:
            if (!std::isfinite(n_)) out += "null";
            else out += format_double(n_);
            break;
        case Type::String: json_escape_to(out, s_); break;
        case Type::Array:
            out.push_back('[');
            for (size_t i = 0; i < items_.size(); ++i) {
                if (i) out.push_back(',');
                newline(out, indent, level + 1);
                items_[i].dump_to(out, indent, level + 1);
            }
            if (!items_.empty()) newline(out, indent, level);
            out.push_back(']');
            break;
        case Type::Object:
            out.push_back('{');
            for (size_t i = 0; i < items_.size(); ++i) {
                if (i) out.push_back(',');
                newline(out, indent, level + 1);
                json_escape_to(out, keys_[i]);
                out.push_back(':');
                if (indent >= 0) out.push_back(' ');
                items_[i].dump_to(out, indent, level + 1);
            }
            if (!items_.empty()) newline(out, indent, level);
            out.push_back('}');
            break;
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    dump_to(out, indent, 0);
    return out;
}

// ------------------------------------------------------------------ parser

namespace {

class Parser {
public:
    Parser(std::string_view t, int max_depth) : t_(t), max_depth_(max_depth) {}

    Json run() {
        skip_ws();
        Json v = value(0);
        skip_ws();
        if (i_ != t_.size()) fail("unexpected trailing characters");
        return v;
    }

private:
    std::string_view t_;
    int max_depth_;
    size_t i_ = 0;

    [[noreturn]] void fail(const std::string& m) const { throw JsonParseError(m, i_); }

    void skip_ws() {
        while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\t' || t_[i_] == '\n' || t_[i_] == '\r')) ++i_;
    }

    bool lit(std::string_view w) {
        if (t_.substr(i_, w.size()) == w) {
            i_ += w.size();
            return true;
        }
        return false;
    }

    Json value(int depth) {
        if (depth >= max_depth_) fail("nesting too deep");
        if (i_ >= t_.size()) fail("unexpected end of input");
        char c = t_[i_];
        switch (c) {
            case '{': return object(depth);
            case '[': return array(depth);
            case '"': return Json(string());
            case 't': if (lit("true")) return Json(true); break;
            case 'f': if (lit("false")) return Json(false); break;
            case 'n': if (lit("null")) return Json(); break;
            default:
                if (c == '-' || (c >= '0' && c <= '9')) return number();
        }
        fail("unexpected character");
    }

    Json object(int depth) {
        ++i_;
        Json o = Json::object();
        skip_ws();
        if (i_ < t_.size() && t_[i_] == '}') { ++i_; return o; }
        for (;;) {
            skip_ws();
            if (i_ >= t_.size() || t_[i_] != '"') fail("expected property name");
            std::string k = string();
            skip_ws();
            if (i_ >= t_.size() || t_[i_] != ':') fail("expected ':'");
            ++i_;
            skip_ws();
            Json v = value(depth + 1);
            o.append(std::move(k), std::move(v));
            skip_ws();
            if (i_ >= t_.size()) fail("unterminated object");
            if (t_[i_] == ',') { ++i_; continue; }
            if (t_[i_] == '}') { ++i_; return o; }
            fail("expected ',' or '}'");
        }
    }

    Json array(int depth) {
        ++i_;
        Json a = Json::array();
        skip_ws();
        if (i_ < t_.size() && t_[i_] == ']') { ++i_; return a; }
        for (;;) {
            skip_ws();
            a.push(value(depth + 1));
            skip_ws();
            if (i_ >= t_.size()) fail("unterminated array");
            if (t_[i_] == ',') { ++i_; continue; }
            if (t_[i_] == ']') { ++i_; return a; }
            fail("expected ',' or ']'");
        }
    }

    Json number() {
        size_t start = i_;
        bool integral = true;
        if (t_[i_] == '-') ++i_;
        if (i_ >= t_.size()) fail("invalid number");
        if (t_[i_] == '0') {
            ++i_;
        } else if (t_[i_] >= '1' && t_[i_] <= '9') {
            while (i_ < t_.size() && t_[i_] >= '0' && t_[i_] <= '9') ++i_;
        } else {
            fail("invalid number");
        }
        if (i_ < t_.size() && t_[i_] == '.') {
            integral = false;
            ++i_;
            if (i_ >= t_.size() || !(t_[i_] >= '0' && t_[i_] <= '9')) fail("invalid number");
            while (i_ < t_.size() && t_[i_] >= '0' && t_[i_] <= '9') ++i_;
        }
        if (i_ < t_.size() && (t_[i_] == 'e' || t_[i_] == 'E')) {
            integral = false;
            ++i_;
            if (i_ < t_.size() && (t_[i_] == '+' || t_[i_] == '-')) ++i_;
            if (i_ >= t_.size() || !(t_[i_] >= '0' && t_[i_] <= '9')) fail("invalid number");
            while (i_ < t_.size() && t_[i_] >= '0' && t_[i_] <= '9') ++i_;
        }
        double d = 0;
        if (!parse_double(t_.substr(start, i_ - start), d)) {
            // Out-of-range magnitudes: from_chars reports ERANGE; follow strtod semantics.
            std::string s(t_.substr(start, i_ - start));
            d = std::strtod(s.c_str(), nullptr);
        }
        return Json::number(d, integral);
    }

    unsigned hex4() {
        if (i_ + 4 > t_.size()) fail("invalid \\u escape");
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = t_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else fail("invalid \\u escape");
        }
        return v;
    }

    static void put_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    // Validates one UTF-8 sequence starting at i_ and copies it.
    void utf8_char(std::string& out) {
        unsigned char c = static_cast<unsigned char>(t_[i_]);
        int len;
        unsigned cp;
        if (c < 0x80) { out.push_back(static_cast<char>(c)); ++i_; return; }
        if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        else fail("invalid UTF-8");
        if (i_ + static_cast<size_t>(len) > t_.size()) fail("invalid UTF-8");
        for (int k = 1; k < len; ++k) {
            unsigned char cc = static_cast<unsigned char>(t_[i_ + static_cast<size_t>(k)]);
            if ((cc & 0xC0) != 0x80) fail("invalid UTF-8");
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
            (cp >= 0xD800 && cp <= 0xDFFF))
            fail("invalid UTF-8");
        out.append(t_.data() + i_, static_cast<size_t>(len));
        i_ += static_cast<size_t>(len);
    }

    std::string string() {
        ++i_;  // opening quote
        std::string out;
        for (;;) {
            if (i_ >= t_.size()) fail("unterminated string");
            char c = t_[i_];
            if (c == '"') { ++i_; return out; }
            if (static_cast<unsigned char>(c) < 0x20) fail("control character in string");
            if (c != '\\') { utf8_char(out); continue; }
            ++i_;
            if (i_ >= t_.size()) fail("unterminated escape");
            char e = t_[i_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned cp = hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (i_ + 2 > t_.size() || t_[i_] != '\\' || t_[i_ + 1] != 'u') fail("unpaired surrogate");
                        i_ += 2;
                        unsigned lo = hex4();
                        if (lo < 0xDC00 || lo > 0xDFFF) fail("unpaired surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        fail("unpaired surrogate");
                    }
                    put_utf8(out, cp);
                    break;
                }
                default: fail("invalid escape");
            }
        }
    }
};

}  // namespace

Json Json::parse(std::string_view text, int max_depth) { return Parser(text, max_depth).run(); }

bool Json::try_parse(std::string_view text, Json& out, std::string& error, int max_depth) noexcept {
    try {
        out = parse(text, max_depth);
        return true;
    } catch (const std::exception& e) {
        try {
            error = e.what();
        } catch (...) {
        }
        return false;
    } catch (...) {
        return false;
    }
}

bool json_equal(const Json& a, const Json& b) {
    if (a.type() != b.type()) return false;
    switch (a.type()) {
        case Json::Type::Null: return true;
        case Json::Type::Bool: return a.as_bool() == b.as_bool();
        case Json::Type::Number: return a.as_number() == b.as_number();
        case Json::Type::String: return a.as_string() == b.as_string();
        case Json::Type::Array:
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (!json_equal(a.at(i), b.at(i))) return false;
            return true;
        case Json::Type::Object:
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                const Json* bv = b.find(a.key(i));
                if (!bv || !json_equal(a.at(i), *bv)) return false;
            }
            return true;
    }
    return false;
}

}  // namespace rql
