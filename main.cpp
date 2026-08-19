#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#if __has_include(<httplib.hpp>)
#include <httplib.hpp>
#else
#include <httplib.h>
#endif

#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/error/en.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using rapidjson::Document;
using rapidjson::StringBuffer;
using rapidjson::Value;
using rapidjson::Writer;

static int g_max_projects = 10;
static int g_max_growing = 3;

static const std::set<std::string> ALLOWED_STATUSES = {"seed", "growing", "parked"};
static const std::set<std::string> ALLOWED_PRIORITIES = {"low", "medium", "high"};

static std::mutex g_mutex;
static std::shared_ptr<spdlog::logger> g_logger;

// NOTE: BASE_DIR_STR is defined by CMake.
static const fs::path BASE_DIR = fs::path(BASE_DIR_STR);
static const fs::path PROJECTS_FILE = BASE_DIR / "projects.jsonl";
static const fs::path INDEX_FILE = BASE_DIR / "index.html";
static const fs::path ASSETS_DIR = BASE_DIR / "assets";

struct AppException : std::runtime_error {
    int status;
    AppException(int status_, const std::string& what)
        : std::runtime_error(what), status(status_) {}
};

struct Config {
    std::string host = "0.0.0.0";
    int port = 8000;

    int max_projects = 10;
    int max_growing = 3;

    bool has_host = false;
    bool has_port = false;
    bool has_max_projects = false;
    bool has_max_growing = false;
};

struct ConfigError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// -----------------------------------------------------------------------------
// Small utilities
// -----------------------------------------------------------------------------

static std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    auto start = s.find_first_not_of(ws);
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(ws);
    return s.substr(start, end - start + 1);
}

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

static std::string lstrip_hashes(const std::string& s) {
    auto it = s.find_first_not_of('#');
    if (it == std::string::npos) return "";
    return s.substr(it);
}

static std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;

    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];

        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            out.push_back(cur);
            cur.clear();
        } else if (c == '\r') {
            if (!cur.empty() && cur.back() == '\n') cur.pop_back();
            out.push_back(cur);
            cur.clear();
            if (i + 1 < s.size() && s[i + 1] == '\n') ++i;
        } else {
            cur.push_back(c);
        }
    }

    if (!cur.empty() && cur.back() == '\r') cur.pop_back();
    out.push_back(cur);

    return out;
}

static std::string now_iso() {
    std::time_t now = std::time(nullptr);
    std::tm tm_local{};

#ifdef _WIN32
    localtime_s(&tm_local, &now);
#else
    localtime_r(&now, &tm_local);
#endif

    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_local);
    return buf;
}

static std::string today_iso() {
    std::time_t now = std::time(nullptr);
    std::tm tm_local{};

#ifdef _WIN32
    localtime_s(&tm_local, &now);
#else
    localtime_r(&now, &tm_local);
#endif

    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_local);
    return buf;
}

static std::string random_hex12() {
    static thread_local std::mt19937 gen{std::random_device{}()};
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(12);

    for (int i = 0; i < 12; ++i) {
        out.push_back(hex[gen() & 0xF]);
    }

    return out;
}

// -----------------------------------------------------------------------------
// RapidJSON helpers
// -----------------------------------------------------------------------------

static std::string to_json_string(const Value& v) {
    StringBuffer sb;
    Writer<StringBuffer> writer(sb);
    v.Accept(writer);
    return sb.GetString();
}

static bool parse_json(const std::string& body, Document& doc) {
    doc.Parse(body.c_str(), body.size());
    return !doc.HasParseError();
}

static const Value& get_member(const Value& obj, const char* key, const Value& null_value) {
    if (obj.IsObject()) {
        auto it = obj.FindMember(key);
        if (it != obj.MemberEnd()) {
            return it->value;
        }
    }
    return null_value;
}

static std::string value_to_text(const Value& v) {
    if (v.IsString()) {
        return v.GetString();
    }

    if (v.IsBool()) {
        return v.GetBool() ? "True" : "False";
    }

    if (v.IsInt()) {
        return std::to_string(v.GetInt());
    }

    if (v.IsUint()) {
        return std::to_string(v.GetUint());
    }

    if (v.IsInt64()) {
        return std::to_string(v.GetInt64());
    }

    if (v.IsUint64()) {
        return std::to_string(v.GetUint64());
    }

    if (v.IsDouble()) {
        std::ostringstream oss;
        oss << v.GetDouble();
        return oss.str();
    }

    return to_json_string(v);
}

static std::string get_text(const Value& obj, const char* key, const std::string& default_value = "") {
    static const Value null_value;
    const Value& v = get_member(obj, key, null_value);

    if (v.IsString()) {
        std::string s = trim(v.GetString());
        return s.empty() ? default_value : s;
    }

    if (v.IsNull()) {
        return default_value;
    }

    std::string s = trim(value_to_text(v));
    return s.empty() ? default_value : s;
}

static bool get_bool(const Value& obj, const char* key, bool default_value) {
    static const Value null_value;
    const Value& v = get_member(obj, key, null_value);

    if (v.IsBool()) {
        return v.GetBool();
    }

    if (v.IsString()) {
        std::string s = to_lower(trim(v.GetString()));
        if (s == "true" || s == "1" || s == "yes" || s == "on") return true;
        if (s == "false" || s == "0" || s == "no" || s == "off") return false;
    }

    return default_value;
}

static int parse_int_from_string(const std::string& raw, int default_value) {
    std::string s = trim(raw);
    if (s.empty()) return default_value;

    try {
        size_t pos = 0;
        long long v = std::stoll(s, &pos);
        if (pos == s.size()) {
            if (v > INT32_MAX) return INT32_MAX;
            if (v < INT32_MIN) return INT32_MIN;
            return static_cast<int>(v);
        }
    } catch (...) {
        return default_value;
    }

    return default_value;
}

static int get_int(const Value& obj, const char* key, int default_value) {
    static const Value null_value;
    const Value& v = get_member(obj, key, null_value);

    if (v.IsInt() || v.IsUint() || v.IsInt64() || v.IsUint64()) {
        long long val = 0;

        if (v.IsInt()) val = v.GetInt();
        else if (v.IsUint()) val = v.GetUint();
        else if (v.IsInt64()) val = v.GetInt64();
        else val = static_cast<long long>(v.GetUint64());

        // Python behavior: `value or default`
        if (val == 0) return default_value;

        if (val > INT32_MAX) return INT32_MAX;
        if (val < INT32_MIN) return INT32_MIN;
        return static_cast<int>(val);
    }

    if (v.IsString()) {
        return parse_int_from_string(v.GetString(), default_value);
    }

    return default_value;
}

static Value normalize_list_field(const Value& value, Document& out) {
    Value arr(rapidjson::kArrayType);
    auto& alloc = out.GetAllocator();

    if (value.IsNull()) {
        return arr;
    }

    if (value.IsString()) {
        std::string s = trim(value.GetString());
        if (!s.empty()) {
            Value v(s.c_str(), alloc);
            arr.PushBack(v.Move(), alloc);
        }
        return arr;
    }

    if (value.IsArray()) {
        for (const auto& item : value.GetArray()) {
            if (item.IsNull()) continue;
            std::string s = trim(value_to_text(item));
            if (!s.empty()) {
                Value v(s.c_str(), alloc);
                arr.PushBack(v.Move(), alloc);
            }
        }
        return arr;
    }

    std::string s = trim(value_to_text(value));
    if (!s.empty()) {
        Value v(s.c_str(), alloc);
        arr.PushBack(v.Move(), alloc);
    }

    return arr;
}

static Value normalize_tags(const Value& value, Document& out) {
    Value arr(rapidjson::kArrayType);
    auto& alloc = out.GetAllocator();
    std::vector<std::string> raw_items;

    if (value.IsString()) {
        std::string s = value.GetString();
        std::string cur;
        for (char c : s) {
            if (c == ',') {
                raw_items.push_back(trim(cur));
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        raw_items.push_back(trim(cur));
    } else {
        Value normalized = normalize_list_field(value, out);
        for (const auto& item : normalized.GetArray()) {
            raw_items.push_back(item.GetString());
        }
    }

    std::set<std::string> seen;
    for (auto& item : raw_items) {
        item = trim(item);
        item = lstrip_hashes(item);
        item = trim(item);

        if (item.empty()) continue;
        if (seen.count(item)) continue;
        seen.insert(item);

        Value v(item.c_str(), alloc);
        arr.PushBack(v.Move(), alloc);
    }

    return arr;
}

static bool is_http_url(const std::string& s) {
    if (s.size() >= 7 && to_lower(s.substr(0, 7)) == "http://") return true;
    if (s.size() >= 8 && to_lower(s.substr(0, 8)) == "https://") return true;
    return false;
}

static void push_link(Value& arr, const std::string& label, const std::string& url, Document& out) {
    auto& alloc = out.GetAllocator();

    Value obj(rapidjson::kObjectType);

    Value label_value(label.c_str(), alloc);
    Value url_value(url.c_str(), alloc);

    obj.AddMember("label", label_value.Move(), alloc);
    obj.AddMember("url", url_value.Move(), alloc);

    arr.PushBack(obj.Move(), alloc);
}

static void process_link_from_text(Value& arr, std::string text, Document& out) {
    text = trim(text);
    if (text.empty()) return;

    if (is_http_url(text)) {
        push_link(arr, text, text, out);
        return;
    }

    size_t colon = text.find(':');
    if (colon != std::string::npos) {
        std::string label = trim(text.substr(0, colon));
        std::string url = trim(text.substr(colon + 1));
        if (!url.empty()) {
            push_link(arr, label.empty() ? url : label, url, out);
            return;
        }
    }

    push_link(arr, text, text, out);
}

static Value normalize_links(const Value& value, Document& out) {
    Value arr(rapidjson::kArrayType);

    auto process_value = [&](const Value& item) {
        if (item.IsNull()) return;

        if (item.IsObject()) {
            std::string url = get_text(item, "url", "");
            if (url.empty()) return;

            std::string label = get_text(item, "label", url);
            push_link(arr, label, url, out);
            return;
        }

        process_link_from_text(arr, value_to_text(item), out);
    };

    if (value.IsNull()) {
        return arr;
    }

    if (value.IsString()) {
        for (const auto& line : split_lines(value.GetString())) {
            process_link_from_text(arr, line, out);
        }
        return arr;
    }

    if (value.IsArray()) {
        for (const auto& item : value.GetArray()) {
            process_value(item);
        }
        return arr;
    }

    process_value(value);
    return arr;
}

static void push_note(Value& arr, const std::string& date, const std::string& text, Document& out) {
    auto& alloc = out.GetAllocator();

    Value obj(rapidjson::kObjectType);

    Value date_value(date.c_str(), alloc);
    Value text_value(text.c_str(), alloc);

    obj.AddMember("date", date_value.Move(), alloc);
    obj.AddMember("text", text_value.Move(), alloc);

    arr.PushBack(obj.Move(), alloc);
}

static void process_note_from_value(Value& arr, const Value& item, Document& out) {
    if (item.IsNull()) return;

    if (item.IsObject()) {
        std::string text = get_text(item, "text", "");
        if (text.empty()) text = get_text(item, "note", "");
        if (text.empty()) return;

        std::string date = get_text(item, "date", today_iso());
        push_note(arr, date, text, out);
        return;
    }

    std::string text = trim(value_to_text(item));
    if (!text.empty()) {
        push_note(arr, today_iso(), text, out);
    }
}

static Value normalize_notes(const Value& value, Document& out) {
    Value arr(rapidjson::kArrayType);

    if (value.IsNull()) {
        return arr;
    }

    if (value.IsString()) {
        for (const auto& line : split_lines(value.GetString())) {
            std::string text = trim(line);
            if (!text.empty()) {
                push_note(arr, today_iso(), text, out);
            }
        }
        return arr;
    }

    if (value.IsArray()) {
        for (const auto& item : value.GetArray()) {
            process_note_from_value(arr, item, out);
        }
        return arr;
    }

    process_note_from_value(arr, value, out);
    return arr;
}

static void add_string_member(Document& doc, const char* key, const std::string& value) {
    auto& alloc = doc.GetAllocator();
    Value v(value.c_str(), alloc);

    doc.AddMember(rapidjson::StringRef(key), v.Move(), alloc);
}

static std::string normalize_project(const Document& data, const std::string& id_override = "") {
    Document out;
    out.SetObject();

    std::string project_id = id_override;
    if (project_id.empty()) {
        project_id = get_text(data, "id", "");
        if (project_id.empty()) {
            project_id = "idea-" + random_hex12();
        }
    }

    std::string title = get_text(data, "title", "Untitled idea");
    if (title.empty()) title = "Untitled idea";

    std::string status = to_lower(get_text(data, "status", "seed"));
    if (status.empty()) status = "seed";

    if (ALLOWED_STATUSES.count(status) == 0) {
        if (status == "done") {
            status = "parked";
        } else {
            status = "seed";
        }
    }

    std::string priority = to_lower(get_text(data, "priority", "medium"));
    if (priority.empty()) priority = "medium";
    if (ALLOWED_PRIORITIES.count(priority) == 0) {
        priority = "medium";
    }

    int version = get_int(data, "version", 1);
    version = std::max(1, version);

    int confidence = get_int(data, "confidence", 5);
    confidence = std::max(0, std::min(10, confidence));

    bool love = get_bool(data, "love", false);

    std::string next_action = get_text(data, "next", "");

    if (status == "growing" && next_action.empty()) {
        status = "seed";
    }

    static const Value null_value;

    add_string_member(out, "id", project_id);
    add_string_member(out, "title", title);
    add_string_member(out, "status", status);
    add_string_member(out, "priority", priority);

    out.AddMember("confidence", confidence, out.GetAllocator());
    out.AddMember("version", version, out.GetAllocator());
    out.AddMember("love", love, out.GetAllocator());

    add_string_member(out, "summary", get_text(data, "summary", ""));
    add_string_member(out, "details", get_text(data, "details", ""));
    add_string_member(out, "spark", get_text(data, "spark", ""));
    add_string_member(out, "next", next_action);
    add_string_member(out, "blockers", get_text(data, "blockers", ""));

    Value tags = normalize_tags(get_member(data, "tags", null_value), out);
    Value links = normalize_links(get_member(data, "links", null_value), out);
    Value images = normalize_list_field(get_member(data, "images", null_value), out);
    Value notes = normalize_notes(get_member(data, "notes", null_value), out);

    out.AddMember("tags", tags.Move(), out.GetAllocator());
    out.AddMember("links", links.Move(), out.GetAllocator());
    out.AddMember("images", images.Move(), out.GetAllocator());
    out.AddMember("notes", notes.Move(), out.GetAllocator());

    add_string_member(out, "createdAt", get_text(data, "createdAt", now_iso()));
    add_string_member(out, "updatedAt", now_iso());
    add_string_member(out, "lastViewed", get_text(data, "lastViewed", ""));

    return to_json_string(out);
}


// -----------------------------------------------------------------------------
// Configuration (minimal flat config parser, no yaml-cpp)
// -----------------------------------------------------------------------------

static std::string strip_quotes(const std::string& s) {
    std::string t = trim(s);

    if (t.size() >= 2) {
        char first = t.front();
        char last = t.back();

        if ((first == '"' && last == '"') ||
            (first == '\'' && last == '\'')) {
            return t.substr(1, t.size() - 2);
        }
    }

    return t;
}

static Config load_config(const fs::path& path) {
    Config cfg;

    if (!fs::exists(path)) {
        g_logger->info("Config file not found: {}. Using defaults.", path.string());
        return cfg;
    }

    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        throw ConfigError("Cannot open config file: " + path.string());
    }

    std::string content;
    content.assign(std::istreambuf_iterator<char>(f),
                   std::istreambuf_iterator<char>());

    int line_number = 0;

    for (const auto& raw_line : split_lines(content)) {
        ++line_number;

        std::string line = trim(raw_line);

        if (line.empty()) {
            continue;
        }

        if (line.front() == '#') {
            continue;
        }

        size_t colon = line.find(':');

        if (colon == std::string::npos) {
            g_logger->warn("Ignoring invalid config line {} in {}: {}",
                           line_number, path.string(), line);
            continue;
        }

        std::string key = trim(line.substr(0, colon));
        std::string value = strip_quotes(line.substr(colon + 1));

        if (key == "host") {
            if (!value.empty()) {
                cfg.host = value;
                cfg.has_host = true;
            }
        } else if (key == "port") {
            int v = parse_int_from_string(value, 0);

            if (v <= 0 || v >= 65536) {
                throw ConfigError(
                    "Invalid port in config line " +
                    std::to_string(line_number) +
                    ": " + value
                );
            }

            cfg.port = v;
            cfg.has_port = true;
        } else if (key == "max_projects") {
            int v = parse_int_from_string(value, 0);

            if (v <= 0) {
                throw ConfigError(
                    "Invalid max_projects in config line " +
                    std::to_string(line_number) +
                    ": " + value
                );
            }

            cfg.max_projects = v;
            cfg.has_max_projects = true;
        } else if (key == "max_growing") {
            int v = parse_int_from_string(value, 0);

            if (v <= 0) {
                throw ConfigError(
                    "Invalid max_growing in config line " +
                    std::to_string(line_number) +
                    ": " + value
                );
            }

            cfg.max_growing = v;
            cfg.has_max_growing = true;
        } else {
            g_logger->debug("Ignoring unknown config key: {}", key);
        }
    }

    return cfg;
}

static void apply_cli_overrides(
    Config& cfg,
    const std::optional<std::string>& cli_host,
    const std::optional<int>& cli_port)
{
    if (cli_host.has_value()) {
        cfg.host = *cli_host;
    }

    if (cli_port.has_value()) {
        cfg.port = *cli_port;
    } else if (!cfg.has_port) {
        if (const char* env_port = std::getenv("PORT")) {
            int v = parse_int_from_string(env_port, 0);

            if (v > 0 && v < 65536) {
                cfg.port = v;
            }
        }
    }
}

// -----------------------------------------------------------------------------
// JSONL storage
// -----------------------------------------------------------------------------

static std::vector<std::string> load_projects_locked() {
    std::vector<std::string> projects;

    if (!fs::exists(PROJECTS_FILE)) {
        return projects;
    }

    std::ifstream f(PROJECTS_FILE.string());
    if (!f.is_open()) {
        throw AppException(500, "Cannot open projects file.");
    }

    int line_number = 0;
    std::string line;

    while (std::getline(f, line)) {
        ++line_number;
        std::string trimmed = trim(line);
        if (trimmed.empty()) continue;

        Document doc;
        if (!parse_json(trimmed, doc)) {
            std::ostringstream oss;
            oss << "Invalid JSONL on line " << line_number << ": "
                << rapidjson::GetParseError_En(doc.GetParseError());
            g_logger->error("{}", oss.str());
            throw AppException(500, oss.str());
        }

        if (!doc.IsObject()) {
            std::ostringstream oss;
            oss << "Invalid JSONL on line " << line_number << ": expected object";
            g_logger->error("{}", oss.str());
            throw AppException(500, oss.str());
        }

        std::string project_id = get_text(doc, "id", "");
        if (project_id.empty()) {
            std::ostringstream oss;
            oss << "idea-line-" << line_number;
            project_id = oss.str();
        }

        projects.push_back(normalize_project(doc, project_id));
    }

    return projects;
}

static void save_projects_locked(const std::vector<std::string>& projects) {
    fs::path tmp_file = PROJECTS_FILE;
    tmp_file += ".tmp";

    std::ofstream f(tmp_file.string(), std::ios::trunc);
    if (!f.is_open()) {
        throw AppException(500, "Cannot open tmp projects file.");
    }

    for (const auto& project : projects) {
        f << project << "\n";
    }

    f.close();

    if (!f.good()) {
        throw AppException(500, "Failed writing tmp projects file.");
    }

    std::error_code ec;
    fs::rename(tmp_file, PROJECTS_FILE, ec);
    if (ec) {
        throw AppException(500, "Failed replacing projects file.");
    }
}

static std::optional<std::string> find_project_line_locked(const std::vector<std::string>& projects, const std::string& project_id) {
    for (const auto& line : projects) {
        Document doc;
        if (parse_json(line, doc) && get_text(doc, "id", "") == project_id) {
            return line;
        }
    }
    return std::nullopt;
}

static std::string get_project_line_or_404_locked(const std::string& project_id) {
    auto projects = load_projects_locked();
    auto line = find_project_line_locked(projects, project_id);
    if (!line) {
        throw AppException(404, "Project not found.");
    }
    return *line;
}

static int count_growing_locked(const std::vector<std::string>& projects, const std::string& exclude_id = "") {
    int count = 0;

    for (const auto& line : projects) {
        Document doc;
        if (!parse_json(line, doc)) continue;

        std::string id = get_text(doc, "id", "");
        std::string status = get_text(doc, "status", "");

        if (status == "growing" && id != exclude_id) {
            ++count;
        }
    }

    return count;
}

static std::string status_from_line(const std::string& line) {
    Document doc;
    if (parse_json(line, doc)) {
        return get_text(doc, "status", "seed");
    }
    return "seed";
}

static void replace_project_line_locked(std::vector<std::string>& projects, const std::string& project_id, const std::string& new_line) {
    for (auto& line : projects) {
        Document doc;
        if (parse_json(line, doc) && get_text(doc, "id", "") == project_id) {
            line = new_line;
        }
    }
}

// -----------------------------------------------------------------------------
// HTTP helpers
// -----------------------------------------------------------------------------

static void set_json_response(httplib::Response& res, const std::string& body) {
    res.status = 200;
    res.set_content(body, "application/json");
}

static void set_error(httplib::Response& res, int status, const std::string& detail) {
    Document doc;
    doc.SetObject();

    Value detail_value(detail.c_str(), doc.GetAllocator());
    doc.AddMember("detail", detail_value.Move(), doc.GetAllocator());

    res.status = status;
    res.set_content(to_json_string(doc), "application/json");
}

static void handle_request(
    std::function<void(const httplib::Request&, httplib::Response&)> handler,
    const httplib::Request& req,
    httplib::Response& res)
{
    try {
        handler(req, res);
    } catch (const AppException& e) {
        g_logger->warn("HTTP {} {}", e.status, e.what());
        set_error(res, e.status, e.what());
    } catch (const std::exception& e) {
        g_logger->error("Unexpected error: {}", e.what());
        set_error(res, 500, e.what());
    } catch (...) {
        g_logger->error("Unknown error");
        set_error(res, 500, "Unknown error");
    }
}

static std::string read_file_to_string(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        return "";
    }

    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

// -----------------------------------------------------------------------------
// API handlers
// -----------------------------------------------------------------------------

static void api_meta(const httplib::Request&, httplib::Response& res) {
    std::lock_guard<std::mutex> lock(g_mutex);

    auto projects = load_projects_locked();

    int total = static_cast<int>(projects.size());
    int growing = 0;
    int seed = 0;
    int parked = 0;

    for (const auto& line : projects) {
        Document doc;
        if (!parse_json(line, doc)) continue;

        std::string status = get_text(doc, "status", "seed");
        if (status == "growing") ++growing;
        else if (status == "seed") ++seed;
        else if (status == "parked") ++parked;
    }

    Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();

    doc.AddMember("total", total, alloc);
    doc.AddMember("maxTotal", g_max_projects, alloc);
    doc.AddMember("growing", growing, alloc);
    doc.AddMember("maxGrowing", g_max_growing, alloc);
    doc.AddMember("seed", seed, alloc);
    doc.AddMember("parked", parked, alloc);

    set_json_response(res, to_json_string(doc));
}

static void api_list_projects(const httplib::Request& req, httplib::Response& res) {
    std::lock_guard<std::mutex> lock(g_mutex);

    auto projects = load_projects_locked();

    std::string status = req.get_param_value("status");
    std::string q = req.get_param_value("q");

    if (!status.empty() && status != "all") {
        std::vector<std::string> filtered;
        for (const auto& line : projects) {
            Document doc;
            if (parse_json(line, doc) && get_text(doc, "status", "") == status) {
                filtered.push_back(line);
            }
        }
        projects = filtered;
    }

    if (!q.empty()) {
        std::string needle = to_lower(q);
        std::vector<std::string> filtered;

        for (const auto& line : projects) {
            Document doc;
            if (!parse_json(line, doc)) continue;

            std::string haystack;
            haystack += get_text(doc, "title", "");
            haystack += " ";
            haystack += get_text(doc, "summary", "");
            haystack += " ";
            haystack += get_text(doc, "details", "");
            haystack += " ";
            haystack += get_text(doc, "spark", "");
            haystack += " ";
            haystack += get_text(doc, "next", "");
            haystack += " ";
            haystack += get_text(doc, "blockers", "");
            haystack += " ";

            static const Value null_value;
            const Value& tags = get_member(doc, "tags", null_value);
            if (tags.IsArray()) {
                for (const auto& tag : tags.GetArray()) {
                    if (tag.IsString()) {
                        haystack += tag.GetString();
                        haystack += " ";
                    }
                }
            }

            if (to_lower(haystack).find(needle) != std::string::npos) {
                filtered.push_back(line);
            }
        }

        projects = filtered;
    }

    std::string out = "[";
    bool first = true;

    for (const auto& line : projects) {
        if (!first) out += ",";
        out += line;
        first = false;
    }

    out += "]";

    set_json_response(res, out);
}

static void api_create_project(const httplib::Request& req, httplib::Response& res) {
    Document doc;
    if (!parse_json(req.body, doc) || !doc.IsObject()) {
        throw AppException(400, "Invalid JSON body.");
    }

    std::lock_guard<std::mutex> lock(g_mutex);

    auto projects = load_projects_locked();

    if (static_cast<int>(projects.size()) >= g_max_projects) {
        throw AppException(400, "Maximum of " + std::to_string(g_max_projects) + " projects reached.");
    }

    std::string project_id = get_text(doc, "id", "");

    if (!project_id.empty()) {
        if (find_project_line_locked(projects, project_id)) {
            throw AppException(409, "Project with id '" + project_id + "' already exists.");
        }
    }

    std::string line = normalize_project(doc);

    if (status_from_line(line) == "growing") {
        if (count_growing_locked(projects) >= g_max_growing) {
            throw AppException(400, "Maximum of " + std::to_string(g_max_growing) + " growing projects reached.");
        }
    }

    projects.push_back(line);
    save_projects_locked(projects);

    set_json_response(res, line);
}

static void api_get_project(const httplib::Request& req, httplib::Response& res) {
    std::string project_id = req.matches[1].str();

    std::lock_guard<std::mutex> lock(g_mutex);
    std::string line = get_project_line_or_404_locked(project_id);

    set_json_response(res, line);
}

static void api_update_project(const httplib::Request& req, httplib::Response& res) {
    std::string project_id = req.matches[1].str();

    Document doc;
    if (!parse_json(req.body, doc) || !doc.IsObject()) {
        throw AppException(400, "Invalid JSON body.");
    }

    std::lock_guard<std::mutex> lock(g_mutex);

    auto projects = load_projects_locked();
    auto old_line_opt = find_project_line_locked(projects, project_id);

    if (!old_line_opt) {
        throw AppException(404, "Project not found.");
    }

    Document old_doc;
    if (!parse_json(*old_line_opt, old_doc)) {
        throw AppException(500, "Failed to parse existing project.");
    }

    std::string old_status = get_text(old_doc, "status", "seed");

    auto& alloc = doc.GetAllocator();

    doc.RemoveMember("id");
    Value id_value(project_id.c_str(), alloc);
    doc.AddMember("id", id_value.Move(), alloc);

    if (!doc.HasMember("love") || doc["love"].IsNull()) {
        bool old_love = get_bool(old_doc, "love", false);
        doc.RemoveMember("love");
        doc.AddMember("love", old_love, alloc);
    }

    std::string created_at = get_text(old_doc, "createdAt", now_iso());
    if (created_at.empty()) created_at = now_iso();

    std::string last_viewed = get_text(old_doc, "lastViewed", "");

    doc.RemoveMember("createdAt");
    Value created_at_value(created_at.c_str(), alloc);
    doc.AddMember("createdAt", created_at_value.Move(), alloc);

    doc.RemoveMember("lastViewed");
    Value last_viewed_value(last_viewed.c_str(), alloc);
    doc.AddMember("lastViewed", last_viewed_value.Move(), alloc);

    std::string line = normalize_project(doc, project_id);

    if (status_from_line(line) == "growing" && old_status != "growing") {
        if (count_growing_locked(projects, project_id) >= g_max_growing) {
            throw AppException(400, "Maximum of " + std::to_string(g_max_growing) + " growing projects reached.");
        }
    }

    replace_project_line_locked(projects, project_id, line);
    save_projects_locked(projects);

    set_json_response(res, line);
}

static void api_delete_project(const httplib::Request& req, httplib::Response& res) {
    std::string project_id = req.matches[1].str();

    std::lock_guard<std::mutex> lock(g_mutex);

    auto projects = load_projects_locked();

    bool found = false;
    std::vector<std::string> remaining;
    remaining.reserve(projects.size());

    for (const auto& line : projects) {
        Document doc;
        if (parse_json(line, doc) && get_text(doc, "id", "") == project_id) {
            found = true;
        } else {
            remaining.push_back(line);
        }
    }

    if (!found) {
        throw AppException(404, "Project not found.");
    }

    save_projects_locked(remaining);

    Document doc;
    doc.SetObject();

    Value deleted_value(project_id.c_str(), doc.GetAllocator());
    doc.AddMember("deleted", deleted_value.Move(), doc.GetAllocator());

    set_json_response(res, to_json_string(doc));
}

static void api_mark_viewed(const httplib::Request& req, httplib::Response& res) {
    std::string project_id = req.matches[1].str();

    std::lock_guard<std::mutex> lock(g_mutex);

    auto projects = load_projects_locked();

    auto line_opt = find_project_line_locked(projects, project_id);
    if (!line_opt) {
        throw AppException(404, "Project not found.");
    }

    Document doc;
    if (!parse_json(*line_opt, doc)) {
        throw AppException(500, "Failed to parse existing project.");
    }

    auto& alloc = doc.GetAllocator();

    doc.RemoveMember("lastViewed");
    Value last_viewed_value(now_iso().c_str(), alloc);
    doc.AddMember("lastViewed", last_viewed_value.Move(), alloc);

    std::string updated_line = to_json_string(doc);

    replace_project_line_locked(projects, project_id, updated_line);
    save_projects_locked(projects);

    set_json_response(res, updated_line);
}

static void api_add_note(const httplib::Request& req, httplib::Response& res) {
    Document doc;
    if (!parse_json(req.body, doc) || !doc.IsObject()) {
        throw AppException(400, "Invalid JSON body.");
    }

    std::string text = get_text(doc, "text", "");
    if (text.empty()) {
        throw AppException(400, "Note text is required.");
    }

    std::string note_date = get_text(doc, "date", today_iso());
    if (note_date.empty()) note_date = today_iso();

    std::string project_id = req.matches[1].str();

    std::lock_guard<std::mutex> lock(g_mutex);

    auto projects = load_projects_locked();

    auto line_opt = find_project_line_locked(projects, project_id);
    if (!line_opt) {
        throw AppException(404, "Project not found.");
    }

    Document project;
    if (!parse_json(*line_opt, project)) {
        throw AppException(500, "Failed to parse existing project.");
    }

    auto& alloc = project.GetAllocator();

    if (!project.HasMember("notes") || !project["notes"].IsArray()) {
        Value notes(rapidjson::kArrayType);
        project.AddMember("notes", notes.Move(), alloc);
    }

    Value note(rapidjson::kObjectType);

    Value date_value(note_date.c_str(), alloc);
    Value text_value(text.c_str(), alloc);

    note.AddMember("date", date_value.Move(), alloc);
    note.AddMember("text", text_value.Move(), alloc);

    project["notes"].PushBack(note.Move(), alloc);

    project.RemoveMember("updatedAt");
    Value updated_at_value(now_iso().c_str(), alloc);
    project.AddMember("updatedAt", updated_at_value.Move(), alloc);

    std::string updated_line = to_json_string(project);

    replace_project_line_locked(projects, project_id, updated_line);
    save_projects_locked(projects);

    set_json_response(res, updated_line);
}

// -----------------------------------------------------------------------------
// Logging setup
// -----------------------------------------------------------------------------

static void setup_logging() {
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("calm_ideas_wall.log", true);
    auto stdout_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

    std::vector<spdlog::sink_ptr> sinks{file_sink, stdout_sink};
    g_logger = std::make_shared<spdlog::logger>("calm", sinks.begin(), sinks.end());
    g_logger->set_level(spdlog::level::info);
    g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    g_logger->flush_on(spdlog::level::info);

    spdlog::register_logger(g_logger);
    spdlog::set_default_logger(g_logger);
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

int main(int argc, char** argv) {
    setup_logging();

    // ------------------------------------------------------------------
    // Command line options
    // ------------------------------------------------------------------
    std::optional<std::string> cli_host;
    std::optional<int> cli_port;
    std::optional<fs::path> cli_config_path;

    auto print_usage = [&]() {
        std::cout
            << "Usage: " << (argc > 0 ? argv[0] : "calm_ideas_wall") << " [options]\n"
            << "\n"
            << "Options:\n"
            << "  --host <host>      Bind address (e.g. 127.0.0.1, 0.0.0.0)\n"
            << "  --port <port>      Port to listen on\n"
            << "  --config <path>    Path to config file\n"
            << "  -h, --help         Show this help\n";
    };

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        auto takes_value = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                std::exit(1);
            }
            return argv[++i];
        };

        if (arg == "-h" || arg == "--help") {
            print_usage();
            return 0;

        } else if (arg == "--host") {
            cli_host = takes_value(arg);

        } else if (arg.rfind("--host=", 0) == 0) {
            cli_host = arg.substr(7);

        } else if (arg == "--port") {
            std::string raw = takes_value(arg);
            int v = parse_int_from_string(raw, 0);

            if (v <= 0 || v >= 65536) {
                std::cerr << "Invalid port: " << raw << "\n";
                return 1;
            }

            cli_port = v;

        } else if (arg.rfind("--port=", 0) == 0) {
            std::string raw = arg.substr(7);
            int v = parse_int_from_string(raw, 0);

            if (v <= 0 || v >= 65536) {
                std::cerr << "Invalid port: " << raw << "\n";
                return 1;
            }

            cli_port = v;

        } else if (arg == "--config") {
            cli_config_path = fs::path(takes_value(arg));

        } else if (arg.rfind("--config=", 0) == 0) {
            cli_config_path = fs::path(arg.substr(9));

        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage();
            return 1;
        }
    }

    // ------------------------------------------------------------------
    // Load config file
    // ------------------------------------------------------------------
    //
    // This is the line that decides the default config file name.
    //
    fs::path config_path = cli_config_path.value_or(BASE_DIR / "config");

    Config cfg;
    try {
        cfg = load_config(config_path);
    } catch (const ConfigError& e) {
        g_logger->error("Configuration error: {}", e.what());
        return 1;
    }

    apply_cli_overrides(cfg, cli_host, cli_port);

    if (cfg.host.empty()) {
        cfg.host = "0.0.0.0";
    }

    if (cfg.port <= 0 || cfg.port >= 65536) {
        g_logger->error("Invalid port: {}", cfg.port);
        return 1;
    }

    g_max_projects = cfg.max_projects > 0 ? cfg.max_projects : 10;
    g_max_growing = cfg.max_growing > 0 ? cfg.max_growing : 3;

    std::error_code ec;
    fs::create_directories(ASSETS_DIR, ec);

    httplib::Server svr;
    svr.set_keep_alive_max_count(100);

    // Static assets
    svr.set_mount_point("/assets", ASSETS_DIR.string());

    // Index
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        if (!fs::exists(INDEX_FILE)) {
            set_error(res, 500, "index.html is missing.");
            return;
        }

        std::string html = read_file_to_string(INDEX_FILE);
        if (html.empty()) {
            set_error(res, 500, "index.html is empty or unreadable.");
            return;
        }

        res.status = 200;
        res.set_content(html, "text/html; charset=utf-8");
    });

    // API
    svr.Get("/api/meta", [](const httplib::Request& req, httplib::Response& res) {
        handle_request(api_meta, req, res);
    });

    svr.Get("/api/projects", [](const httplib::Request& req, httplib::Response& res) {
        handle_request(api_list_projects, req, res);
    });

    svr.Post("/api/projects", [](const httplib::Request& req, httplib::Response& res) {
        handle_request(api_create_project, req, res);
    });

    svr.Get(R"(/api/projects/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        handle_request(api_get_project, req, res);
    });

    svr.Put(R"(/api/projects/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        handle_request(api_update_project, req, res);
    });

    svr.Delete(R"(/api/projects/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        handle_request(api_delete_project, req, res);
    });

    svr.Post(R"(/api/projects/([^/]+)/view)", [](const httplib::Request& req, httplib::Response& res) {
        handle_request(api_mark_viewed, req, res);
    });

    svr.Post(R"(/api/projects/([^/]+)/notes)", [](const httplib::Request& req, httplib::Response& res) {
        handle_request(api_add_note, req, res);
    });

    g_logger->info("Config file: {}", config_path.string());
    g_logger->info("Host: {}", cfg.host);
    g_logger->info("Starting calm_ideas_wall on port {}", cfg.port);
    g_logger->info("Projects file: {}", PROJECTS_FILE.string());
    g_logger->info("Index file: {}", INDEX_FILE.string());
    g_logger->info("Limits: max_projects={} max_growing={}",
                   g_max_projects,
                   g_max_growing);

    if (!svr.listen(cfg.host, cfg.port)) {
        g_logger->error("Failed to start HTTP server on {} port {}", cfg.host, cfg.port);
        return 1;
    }

    return 0;
}

