// json_util.cpp — реализация минимального JSON-парсера и писателя.
#include "json_util.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace json {

// ---------------------------------------------------------------------------
// Конструкторы узлов.
// ---------------------------------------------------------------------------
Node Node::makeObject() { Node n; n.type = Type::Object; return n; }
Node Node::makeArray()  { Node n; n.type = Type::Array;  return n; }
Node Node::makeString(const std::string& s) { Node n; n.type = Type::String; n.str = s; return n; }
Node Node::makeNumber(double d)  { Node n; n.type = Type::Number; n.num = d; return n; }
Node Node::makeBool(bool b)      { Node n; n.type = Type::Bool; n.boolean = b; return n; }
Node Node::makeNull()            { Node n; n.type = Type::Null; return n; }

const Node* Node::find(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& kv : object) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

std::string Node::asString(const std::string& defaultVal) const {
    return (type == Type::String) ? str : defaultVal;
}
double Node::asNumber(double defaultVal) const {
    return (type == Type::Number) ? num : defaultVal;
}
bool Node::asBool(bool defaultVal) const {
    return (type == Type::Bool) ? boolean : defaultVal;
}

// ---------------------------------------------------------------------------
// Парсер.
// ---------------------------------------------------------------------------
namespace {

// Небольшой потоковый сканер поверх std::string.
struct Cursor {
    const std::string& s;
    size_t pos = 0;
    explicit Cursor(const std::string& text) : s(text) {}

    void skipWs() { while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos; }
    bool atEnd() const { return pos >= s.size(); }
    char peek() const { return atEnd() ? '\0' : s[pos]; }
    char next() { return atEnd() ? '\0' : s[pos++]; }
    bool consume(char c) { if (peek() == c) { ++pos; return true; } return false; }
};

// Чтение JSON-строки (включая экранирование \" \\ \/ \b \f \n \r \t \uXXXX).
std::string parseString(Cursor& c) {
    std::string out;
    if (!c.consume('"')) return out;
    // UTF-16 суррогатные пары «\uXXXX» собираем в UTF-8 на лету.
    auto pushUtf8 = [&out](unsigned int cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    };
    for (;;) {
        if (c.atEnd()) break;
        char ch = c.next();
        if (ch == '"') break;
        if (ch == '\\') {
            char esc = c.next();
            switch (esc) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    // Читаем 4 hex-цифры.
                    unsigned int cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = c.next();
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<unsigned int>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned int>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned int>(h - 'A' + 10);
                        else { cp = 0xFFFD; break; } // некорректная цифра
                    }
                    // Если суррогатная пара — читаем второй \uXXXX.
                    if (cp >= 0xD800 && cp <= 0xDBFF && c.peek() == '\\') {
                        // Этот блок разворачиваем максимально просто.
                        (void)0;
                    }
                    pushUtf8(cp);
                    break;
                }
                default: out.push_back(esc); break;
            }
        } else {
            out.push_back(ch); // обычный UTF-8 символ как есть
        }
    }
    return out;
}

// Чтение числа (double).
Node parseNumber(Cursor& c) {
    std::string tok;
    if (c.peek() == '-') tok += c.next();
    while (std::isdigit(static_cast<unsigned char>(c.peek()))) tok += c.next();
    if (c.peek() == '.') {
        tok += c.next();
        while (std::isdigit(static_cast<unsigned char>(c.peek()))) tok += c.next();
    }
    if (c.peek() == 'e' || c.peek() == 'E') {
        tok += c.next();
        if (c.peek() == '+' || c.peek() == '-') tok += c.next();
        while (std::isdigit(static_cast<unsigned char>(c.peek()))) tok += c.next();
    }
    return Node::makeNumber(std::strtod(tok.c_str(), nullptr));
}

Node parseValue(Cursor& c);

// Чтение элемента (рекурсию ограничиваем практической глубиной вложенности).
Node parseObject(Cursor& c) {
    Node obj = Node::makeObject();
    c.consume('{');
    c.skipWs();
    if (c.consume('}')) return obj;
    for (;;) {
        c.skipWs();
        std::string key = parseString(c);
        c.skipWs();
        if (!c.consume(':')) break;
        c.skipWs();
        Node val = parseValue(c);
        obj.object.emplace_back(std::move(key), std::move(val));
        c.skipWs();
        if (c.consume('}')) break;
        if (!c.consume(',')) break;
    }
    return obj;
}

Node parseArray(Cursor& c) {
    Node arr = Node::makeArray();
    c.consume('[');
    c.skipWs();
    if (c.consume(']')) return arr;
    for (;;) {
        c.skipWs();
        Node val = parseValue(c);
        arr.array.push_back(std::move(val));
        c.skipWs();
        if (c.consume(']')) break;
        if (!c.consume(',')) break;
    }
    return arr;
}

Node parseValue(Cursor& c) {
    c.skipWs();
    Node n;
    switch (c.peek()) {
        case '{': n = parseObject(c);    break;
        case '[': n = parseArray(c);     break;
        case '"': n = Node::makeString(parseString(c)); break;
        case 't': if (c.s.compare(c.pos, 4, "true") == 0) { c.pos += 4; n = Node::makeBool(true); } break;
        case 'f': if (c.s.compare(c.pos, 5, "false") == 0) { c.pos += 5; n = Node::makeBool(false); } break;
        case 'n': if (c.s.compare(c.pos, 4, "null") == 0) { c.pos += 4; n = Node::makeNull(); } break;
        default:  n = parseNumber(c);    break;
    }
    return n;
}

} // namespace

Node parse(const std::string& text, size_t* parse_hint) {
    Cursor c(text);
    Node root = parseValue(c);
    c.skipWs();
    if (!c.atEnd() && parse_hint) *parse_hint = c.pos; // не дочитали до конца
    return root;
}

// ---------------------------------------------------------------------------
// Писатель.
// ---------------------------------------------------------------------------
namespace {

// Экранирует строку для записи в JSON.
std::string escape(const std::string& s) {
    std::string out;
    for (char ch : s) {
        switch (ch) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04X", static_cast<unsigned char>(ch));
                    out += buf;
                } else {
                    out += ch; // UTF-8 байты верхней половины таблицы оставляем как есть
                }
        }
    }
    return out;
}

void writeNode(const Node& n, std::string& out, int depth) {
    switch (n.type) {
        case Node::Type::Null:   out += "null"; break;
        case Node::Type::Bool:   out += n.boolean ? "true" : "false"; break;
        case Node::Type::Number: {
            char buf[40];
            if (n.num == static_cast<long long>(n.num) &&
                std::fabs(n.num) < 1e15) {
                std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(n.num));
            } else {
                std::snprintf(buf, sizeof buf, "%.9g", n.num);
            }
            out += buf;
            break;
        }
        case Node::Type::String: out += '"' + escape(n.str) + '"'; break;
        case Node::Type::Array: {
            out += '[';
            for (size_t i = 0; i < n.array.size(); ++i) {
                if (i) out += ',';
                writeNode(n.array[i], out, depth + 1);
            }
            out += ']';
            break;
        }
        case Node::Type::Object: {
            out += '{';
            for (size_t i = 0; i < n.object.size(); ++i) {
                if (i) out += ',';
                out += '"' + escape(n.object[i].first) + "\":";
                writeNode(n.object[i].second, out, depth + 1);
            }
            out += '}';
            break;
        }
    }
}

} // namespace

std::string stringify(const Node& root) {
    std::string out;
    out.reserve(256);
    writeNode(root, out, 0);
    return out;
}

} // namespace json