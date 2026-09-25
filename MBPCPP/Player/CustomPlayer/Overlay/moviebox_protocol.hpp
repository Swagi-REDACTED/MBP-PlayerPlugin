#pragma once

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mbp {

enum class JsonType { Null, Bool, Number, String, Object, Array };

struct JsonValue {
    JsonType type = JsonType::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::map<std::string, JsonValue> object;
    std::vector<JsonValue> array;

    bool isNull() const { return type == JsonType::Null; }
    bool isBool() const { return type == JsonType::Bool; }
    bool isNumber() const { return type == JsonType::Number; }
    bool isString() const { return type == JsonType::String; }
    bool isObject() const { return type == JsonType::Object; }
    bool isArray() const { return type == JsonType::Array; }

    bool asBool(bool fallback = false) const {
        if (isBool()) return boolean;
        if (isNumber()) return number != 0.0;
        return fallback;
    }

    double asNumber(double fallback = 0.0) const {
        if (isNumber() && std::isfinite(number)) return number;
        if (isBool()) return boolean ? 1.0 : 0.0;
        return fallback;
    }

    std::string asString(std::string fallback = {}) const {
        return isString() ? string : std::move(fallback);
    }

    const JsonValue* find(std::string_view key) const {
        if (!isObject()) return nullptr;
        auto it = object.find(std::string(key));
        return it == object.end() ? nullptr : &it->second;
    }
};

namespace detail {

inline void AppendUtf8(std::string& out, std::uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    JsonValue parse() {
        skipSpace();
        JsonValue value = parseValue();
        skipSpace();
        if (pos_ != input_.size()) fail("unexpected trailing data");
        return value;
    }

private:
    std::string_view input_;
    std::size_t pos_ = 0;

    [[noreturn]] void fail(const char* message) const {
        throw std::runtime_error(std::string(message) + " at byte " + std::to_string(pos_));
    }

    void skipSpace() {
        while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_]))) ++pos_;
    }

    bool consume(char c) {
        if (pos_ < input_.size() && input_[pos_] == c) { ++pos_; return true; }
        return false;
    }

    void require(char c) {
        if (!consume(c)) fail("expected delimiter");
    }

    JsonValue parseValue() {
        if (pos_ >= input_.size()) fail("unexpected end of JSON");
        const char c = input_[pos_];
        if (c == 'n') return parseLiteral("null", JsonValue{});
        if (c == 't') { JsonValue v; v.type = JsonType::Bool; v.boolean = true; return parseLiteral("true", std::move(v)); }
        if (c == 'f') { JsonValue v; v.type = JsonType::Bool; v.boolean = false; return parseLiteral("false", std::move(v)); }
        if (c == '"') { JsonValue v; v.type = JsonType::String; v.string = parseString(); return v; }
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber();
        fail("invalid JSON value");
    }

    JsonValue parseLiteral(std::string_view literal, JsonValue value) {
        if (input_.substr(pos_, literal.size()) != literal) fail("invalid literal");
        pos_ += literal.size();
        return value;
    }

    static int hexValue(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    std::uint32_t parseHex4() {
        if (pos_ + 4 > input_.size()) fail("incomplete unicode escape");
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const int h = hexValue(input_[pos_++]);
            if (h < 0) fail("invalid unicode escape");
            value = (value << 4) | static_cast<std::uint32_t>(h);
        }
        return value;
    }

    std::string parseString() {
        require('"');
        std::string out;
        while (pos_ < input_.size()) {
            const unsigned char c = static_cast<unsigned char>(input_[pos_++]);
            if (c == '"') return out;
            if (c < 0x20) fail("control character in string");
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                continue;
            }
            if (pos_ >= input_.size()) fail("incomplete escape");
            const char esc = input_[pos_++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    std::uint32_t cp = parseHex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (pos_ + 2 > input_.size() || input_[pos_] != '\\' || input_[pos_ + 1] != 'u') fail("missing low surrogate");
                        pos_ += 2;
                        const std::uint32_t low = parseHex4();
                        if (low < 0xDC00 || low > 0xDFFF) fail("invalid low surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        fail("unexpected low surrogate");
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: fail("invalid escape");
            }
        }
        fail("unterminated string");
    }

    JsonValue parseObject() {
        JsonValue value;
        value.type = JsonType::Object;
        require('{');
        skipSpace();
        if (consume('}')) return value;
        while (true) {
            skipSpace();
            if (pos_ >= input_.size() || input_[pos_] != '"') fail("expected object key");
            std::string key = parseString();
            skipSpace();
            require(':');
            skipSpace();
            value.object.emplace(std::move(key), parseValue());
            skipSpace();
            if (consume('}')) break;
            require(',');
            skipSpace();
        }
        return value;
    }

    JsonValue parseArray() {
        JsonValue value;
        value.type = JsonType::Array;
        require('[');
        skipSpace();
        if (consume(']')) return value;
        while (true) {
            skipSpace();
            value.array.push_back(parseValue());
            skipSpace();
            if (consume(']')) break;
            require(',');
            skipSpace();
        }
        return value;
    }

    JsonValue parseNumber() {
        const std::size_t start = pos_;
        consume('-');
        if (consume('0')) {
            // Leading zero consumed.
        } else {
            if (pos_ >= input_.size() || input_[pos_] < '1' || input_[pos_] > '9') fail("invalid number");
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        }
        if (consume('.')) {
            if (pos_ >= input_.size() || !std::isdigit(static_cast<unsigned char>(input_[pos_]))) fail("invalid fraction");
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        }
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) ++pos_;
            if (pos_ >= input_.size() || !std::isdigit(static_cast<unsigned char>(input_[pos_]))) fail("invalid exponent");
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        }
        const std::string token(input_.substr(start, pos_ - start));
        char* end = nullptr;
        const double number = std::strtod(token.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(number)) fail("invalid finite number");
        JsonValue value;
        value.type = JsonType::Number;
        value.number = number;
        return value;
    }
};

inline std::string EscapeJsonString(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (unsigned char c : value) {
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
                    static const char* hex = "0123456789ABCDEF";
                    out += "\\u00";
                    out.push_back(hex[(c >> 4) & 0xF]);
                    out.push_back(hex[c & 0xF]);
                } else out.push_back(static_cast<char>(c));
                break;
        }
    }
    return out;
}

inline const JsonValue* Field(const JsonValue& object, const char* key) { return object.find(key); }
inline std::string StringField(const JsonValue& object, const char* key, std::string fallback = {}) {
    const JsonValue* v = Field(object, key);
    return v ? v->asString(std::move(fallback)) : std::move(fallback);
}
inline double NumberField(const JsonValue& object, const char* key, double fallback = 0.0) {
    const JsonValue* v = Field(object, key);
    return v ? v->asNumber(fallback) : fallback;
}
inline bool BoolField(const JsonValue& object, const char* key, bool fallback = false) {
    const JsonValue* v = Field(object, key);
    return v ? v->asBool(fallback) : fallback;
}

} // namespace detail

struct PlaybackRequest {
    std::string action = "poll";
    std::int64_t revision = 0;
    double position = 0.0;
    double duration = 0.0;
    bool playing = false;
    bool ready = false;
    int volume = 0;
    bool muted = false;
    bool metadata = false;
    std::string id;
    double value = 0.0;
};

struct PlaybackSource {
    std::int64_t revision = 0;
    std::string uri;
    std::string title = "MovieBoxPro";
    std::string engine = "FFmpeg";
    double seconds = 0.0;
    bool playing = true;
    double rate = 1.0;
    std::string settingsTitle;
    std::int16_t season = 0;
    std::int16_t episode = 0;
    std::uint8_t boxType = 0;
};

struct PlaybackChoice {
    std::string id;
    std::string label;
    std::string group;
    bool selected = false;
};

struct PlaybackMetadata {
    bool isSeries = false;
    bool loading = false;
    std::string error;
    std::vector<PlaybackChoice> episodes;
    std::vector<PlaybackChoice> qualities;
    std::vector<PlaybackChoice> subtitles;
    std::vector<PlaybackChoice> servers;
};

struct PlaybackCommand {
    std::string action;
    std::map<std::string, JsonValue> values;
};

struct SubtitleCue {
    double start = 0.0;
    double end = 0.0;
    std::string text;
};

struct PlaybackReply {
    std::string error;
    std::optional<PlaybackSource> source;
    std::optional<PlaybackMetadata> metadata;
    std::vector<PlaybackCommand> commands;
    std::optional<std::vector<SubtitleCue>> cues;
    double subtitleDelay = 0.0;
    bool closed = false;
};

inline std::string SerializePlaybackRequest(const PlaybackRequest& request) {
    auto finite = [](double value) { return std::isfinite(value) ? value : 0.0; };
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(17);
    out << '{'
        << "\"Action\":\"" << detail::EscapeJsonString(request.action) << "\","
        << "\"Revision\":" << request.revision << ','
        << "\"Position\":" << finite(request.position) << ','
        << "\"Duration\":" << finite(request.duration) << ','
        << "\"Playing\":" << (request.playing ? "true" : "false") << ','
        << "\"Ready\":" << (request.ready ? "true" : "false") << ','
        << "\"Volume\":" << request.volume << ','
        << "\"Muted\":" << (request.muted ? "true" : "false") << ','
        << "\"Metadata\":" << (request.metadata ? "true" : "false") << ','
        << "\"Id\":\"" << detail::EscapeJsonString(request.id) << "\","
        << "\"Value\":" << finite(request.value)
        << '}';
    return out.str();
}

inline PlaybackChoice ParseChoice(const JsonValue& value) {
    PlaybackChoice choice;
    choice.id = detail::StringField(value, "Id");
    choice.label = detail::StringField(value, "Label");
    choice.group = detail::StringField(value, "Group");
    choice.selected = detail::BoolField(value, "Selected");
    return choice;
}

inline void ParseChoiceArray(const JsonValue& object, const char* key, std::vector<PlaybackChoice>& out) {
    const JsonValue* array = detail::Field(object, key);
    if (!array || !array->isArray()) return;
    out.reserve(array->array.size());
    for (const JsonValue& item : array->array) if (item.isObject()) out.push_back(ParseChoice(item));
}

inline bool ParsePlaybackReply(std::string_view json, PlaybackReply& out, std::string& parseError) {
    try {
        JsonValue root = detail::JsonParser(json).parse();
        if (!root.isObject()) throw std::runtime_error("playback reply root is not an object");

        PlaybackReply parsed;
        if (const JsonValue* error = root.find("Error"); error && error->isString()) parsed.error = error->string;

        if (const JsonValue* source = root.find("Source"); source && source->isObject()) {
            PlaybackSource item;
            item.revision = static_cast<std::int64_t>(detail::NumberField(*source, "Revision", 0));
            item.uri = detail::StringField(*source, "Uri");
            item.title = detail::StringField(*source, "Title", "MovieBoxPro");
            item.engine = detail::StringField(*source, "Engine", "FFmpeg");
            item.seconds = detail::NumberField(*source, "Seconds", 0.0);
            item.playing = detail::BoolField(*source, "Playing", true);
            item.rate = detail::NumberField(*source, "Rate", 1.0);
            item.settingsTitle = detail::StringField(*source, "SettingsTitle");
            item.season = static_cast<std::int16_t>(detail::NumberField(*source, "Season", 0));
            item.episode = static_cast<std::int16_t>(detail::NumberField(*source, "Episode", 0));
            item.boxType = static_cast<std::uint8_t>(detail::NumberField(*source, "BoxType", 0));
            parsed.source = std::move(item);
        }

        if (const JsonValue* metadata = root.find("Metadata"); metadata && metadata->isObject()) {
            PlaybackMetadata item;
            item.isSeries = detail::BoolField(*metadata, "IsSeries", false);
            item.loading = detail::BoolField(*metadata, "Loading", false);
            if (const JsonValue* error = metadata->find("Error"); error && error->isString()) item.error = error->string;
            ParseChoiceArray(*metadata, "Episodes", item.episodes);
            ParseChoiceArray(*metadata, "Qualities", item.qualities);
            ParseChoiceArray(*metadata, "Subtitles", item.subtitles);
            ParseChoiceArray(*metadata, "Servers", item.servers);
            parsed.metadata = std::move(item);
        }

        if (const JsonValue* commands = root.find("Commands"); commands && commands->isArray()) {
            parsed.commands.reserve(commands->array.size());
            for (const JsonValue& command : commands->array) {
                if (!command.isObject()) continue;
                PlaybackCommand item;
                item.action = detail::StringField(command, "Action");
                if (const JsonValue* values = command.find("Values"); values && values->isObject()) item.values = values->object;
                parsed.commands.push_back(std::move(item));
            }
        }

        if (const JsonValue* cues = root.find("Cues"); cues && cues->isArray()) {
            std::vector<SubtitleCue> items;
            items.reserve(cues->array.size());
            for (const JsonValue& cue : cues->array) {
                if (!cue.isObject()) continue;
                SubtitleCue item;
                item.start = detail::NumberField(cue, "Start", 0.0);
                item.end = detail::NumberField(cue, "End", 0.0);
                item.text = detail::StringField(cue, "Text");
                items.push_back(std::move(item));
            }
            parsed.cues = std::move(items);
        }

        parsed.subtitleDelay = detail::NumberField(root, "SubtitleDelay", 0.0);
        parsed.closed = detail::BoolField(root, "Closed", false);
        out = std::move(parsed);
        parseError.clear();
        return true;
    } catch (const std::exception& ex) {
        parseError = ex.what();
        return false;
    }
}

} // namespace mbp
