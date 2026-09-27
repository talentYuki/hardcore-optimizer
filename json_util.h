#pragma once
// ============================================================================
// json_util.h — минимальный самодостаточный JSON-парсер/писатель.
//
// Никаких внешних зависимостей: пары «ключ: значение», вложенные объекты и
// массивы храним в простых структурах. Этого достаточно для наших файлов
// темы и бэкапа реестра. Парсер итеративный, без рекурсии.
// ============================================================================

#include <string>
#include <vector>

namespace json {

// Узел JSON-дерева: объект, массив или скаляр.
struct Node {
    enum class Type { Object, Array, String, Number, Bool, Null };

    Type type = Type::Null;
    std::vector<std::pair<std::string, Node>> object; // ключ -> узел (объект)
    std::vector<Node>                          array;  // элементы (массив)
    std::string                                str;    // строковое значение
    double                                     num = 0.0;
    bool                                       boolean = false;

    // Удобные конструкторы для писателя.
    static Node makeObject();
    static Node makeArray();
    static Node makeString(const std::string& s);
    static Node makeNumber(double d);
    static Node makeBool(bool b);
    static Node makeNull();

    // Поиск ключа в объекте. Возвращает nullptr, если отсутствует.
    const Node* find(const std::string& key) const;

    // Чтение скаляров с подстановкой значений по умолчанию.
    std::string  asString(const std::string& defaultVal = {}) const;
    double       asNumber(double defaultVal = 0.0) const;
    bool         asBool(bool defaultVal = false) const;
};

// Разбирает JSON из строки. Возвращает корневой узел (Null при ошибке).
// parse_hint указывает на позицию ошибки (для диагностики), может быть null.
Node parse(const std::string& text, size_t* parse_hint = nullptr);

// Сериализует узел обратно в JSON-строку.
std::string stringify(const Node& root);

} // namespace json